#include <core/json.hpp>
#include <formats/ai_route.hpp>
#include <formats/car_definition.hpp>
#include <formats/collision_mesh.hpp>
#include <formats/bitmap_font.hpp>
#include <formats/iff_image.hpp>
#include <formats/model_geometry.hpp>
#include <formats/track_world.hpp>
#include <formats/texture_archive.hpp>
#include <services/texture_catalog.hpp>
#include <formats/track_definition.hpp>
#include <application.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

template <typename T, void (*Destroy)(T *)>
using SdlPointer = std::unique_ptr<T, decltype(Destroy)>;

enum class CarShading : std::uint8_t {
  flat,
  gouraud,
  reflection,
  glenz,
};

constexpr std::array<std::string_view, 4U> car_shading_names{
    "Flat", "Gouraud", "Reflection", "Glenz"};
constexpr float viewer_footer_height = 58.0F;
constexpr float viewer_help_toggle_width = 44.0F;
constexpr float viewer_help_toggle_right_margin = 16.0F;
constexpr float viewer_solid_pill_width = 68.0F;
constexpr float viewer_wireframe_pill_width = 100.0F;
constexpr float viewer_normals_pill_width = 82.0F;
constexpr float viewer_collision_pill_width = 94.0F;
constexpr float viewer_textures_pill_width = 86.0F;
constexpr float viewer_pill_gap = 10.0F;
constexpr float viewer_content_left = 24.0F;
constexpr float viewer_content_right_margin = 24.0F;
constexpr float viewer_viewport_top = 108.0F;
constexpr float viewer_viewport_bottom_gap = 72.0F;
constexpr float full_track_pitch_radians = 0.436332313F;
constexpr float full_track_pitch_sine = 0.422618262F;
constexpr float full_track_pitch_cosine = 0.906307787F;
constexpr float camera_minimum_pitch = -1.35F;
constexpr float camera_maximum_pitch = 1.35F;

std::string_view car_shading_name(const CarShading shading) {
  return car_shading_names[static_cast<std::size_t>(shading)];
}

CarShading parse_car_shading(const std::string_view value) {
  for (std::size_t index = 0U; index < car_shading_names.size(); ++index) {
    auto name = std::string(car_shading_names[index]);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](const char character) {
                     return character >= 'A' && character <= 'Z'
                                ? static_cast<char>(character + ('a' - 'A'))
                                : character;
                   });
    if (value == name) {
      return static_cast<CarShading>(index);
    }
  }
  throw std::runtime_error(
      "--car-shading must be flat, gouraud, reflection, or glenz");
}

struct Options {
  std::filesystem::path content_root;
  std::filesystem::path model_relative_path;
  std::optional<std::filesystem::path> car_definition_relative_path;
  std::optional<std::filesystem::path> track_definition_relative_path;
  std::string track;
  std::string pack = "tex1";
  std::string view = "inspect";
  float initial_zoom = 1.0F;
  std::optional<std::filesystem::path> override_root;
  std::optional<std::filesystem::path> report_path;
  std::optional<std::filesystem::path> screenshot_path;
  std::optional<std::size_t> local_cell;
  std::optional<std::size_t> route_group;
  std::optional<std::size_t> route_sample;
  bool route_chase = false;
  bool report_only = false;
  bool collision_overlay = false;
  bool lighting_overlay = false;
  bool applied_lighting = true;
  bool route_overlay = false;
  bool hd_enabled = true;
  bool depth_buffer = true;
  CarShading car_shading = CarShading::glenz;
  std::uint32_t maximum_frames = 0U;
};

struct VehicleSource {
  std::filesystem::path model_relative_path;
  mh::content::CarDefinition definition;
  std::filesystem::path definition_relative_path;
};

struct TrackSource {
  std::filesystem::path definition_relative_path;
  std::filesystem::path model_relative_path;
  std::string texture_track;
  mh::content::TrackDefinition definition;
};

struct Vec3 {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

struct MaterialTexture {
  std::string material_name;
  std::string logical_id;
  std::uint32_t original_width = 0U;
  std::uint32_t original_height = 0U;
  bool override_used = false;
  mh::content::PamRgbaImage image;
  SdlPointer<SDL_Texture, SDL_DestroyTexture> texture{nullptr,
                                                      SDL_DestroyTexture};
  std::optional<mh::content::PamRgbaImage> override_image;
  SdlPointer<SDL_Texture, SDL_DestroyTexture> override_texture{
      nullptr, SDL_DestroyTexture};
};

struct VehicleEnvironmentTexture {
  mh::content::PamRgbaImage image;
  SdlPointer<SDL_Texture, SDL_DestroyTexture> texture{nullptr,
                                                      SDL_DestroyTexture};
};

struct VehicleEnvironmentTextures {
  VehicleEnvironmentTexture environment;
  VehicleEnvironmentTexture reflection;
  VehicleEnvironmentTexture phong;
  bool complete = false;
};

enum class VehicleEnvironmentMap {
  none,
  environment,
  reflection,
  phong,
};

struct ModelComponent {
  std::string label;
  std::filesystem::path relative_path;
  std::string texture_track;
  Vec3 translation;
  mh::content::MyoData model;
  std::vector<std::optional<std::size_t>> material_indices;
  std::vector<std::uint32_t> face_object_indices;
  std::vector<std::array<std::array<std::uint8_t, 4U>, 4U>>
      face_vertex_colors;
};

struct Scene {
  std::vector<ModelComponent> components;
  std::vector<MaterialTexture> materials;
  Vec3 minimum;
  Vec3 maximum;
  std::filesystem::path collision_relative_path;
  std::vector<mh::content::ColPolygon> collision_polygons;
  std::vector<mh::content::MywPointLight> world_lights;
  std::vector<mh::content::MywLensFlare> world_lens_flares;
  std::vector<mh::content::MywObject> world_objects;
  std::vector<mh::content::MywGridCell> world_grid_cells;
  mh::content::AiRouteData world_route;
  std::vector<Vec3> world_route_positions;
  std::vector<std::size_t> occupied_world_cells;
  std::vector<std::size_t> world_section_cells;
  std::uint32_t world_grid_width = 0U;
  std::uint32_t world_grid_height = 0U;
  double world_grid_cell_size_x = 0.0;
  double world_grid_cell_size_z = 0.0;
  std::array<float, 3U> world_accelerated_brightness{1.0F, 1.0F, 1.0F};
  bool assembled_car = false;
  std::string wheel_geometry = "none";
  std::string wheel_anchor_source = "none";
  std::optional<char> lod;
  std::uint64_t fallback_materials = 0U;
  std::uint64_t override_materials = 0U;
  bool world_scene = false;
  std::string track_name;
  std::filesystem::path track_definition_relative_path;
  std::size_t track_roster_index = 0U;
  std::size_t track_roster_count = 0U;
  std::string vehicle_name;
  std::size_t vehicle_roster_index = 0U;
  std::size_t vehicle_roster_count = 0U;
  std::array<mh::content::CarColor, 3U> vehicle_default_colors{};
  VehicleEnvironmentTextures vehicle_environment;
  std::array<float, 3U> vehicle_object_ambient{1.0F, 1.0F, 1.0F};
  std::array<float, 3U> vehicle_object_brightness{1.0F, 1.0F, 1.0F};
  std::array<float, 3U> vehicle_accelerated_brightness{1.0F, 1.0F, 1.0F};
  std::array<std::uint8_t, 3U> vehicle_directional_color{};
  float vehicle_directional_intensity = 0.0F;
  std::array<float, 3U> vehicle_directional_direction{};
};

struct ViewerCache {
  std::filesystem::path content_root;
  std::array<std::optional<std::vector<VehicleSource>>, 4U> vehicle_sources;
  std::optional<std::vector<TrackSource>> track_sources;
  std::optional<mh::content::TextureCatalog> texture_catalog;
  std::set<std::string> texture_catalog_focus_materials;
  std::map<std::string, mh::content::MyoData> models;
  std::map<std::string, mh::content::MywData> worlds;
  std::map<std::string, mh::content::AiRouteData> routes;
  std::map<std::string, mh::content::PdiArchive> pdi_archives;
  std::map<std::string, mh::content::PamRgbaImage> texture_images;
};

struct ProjectedVertex {
  float x = 0.0F;
  float y = 0.0F;
  float depth = 0.0F;
  bool visible = true;
};

struct ProjectedFace {
  std::size_t component_index = 0U;
  std::size_t face_index = 0U;
  float depth = 0.0F;
};

struct ViewVolume {
  Vec3 center;
  float radius = 1.0F;
  float camera_distance = 0.0F;
  bool clip_near = false;
  bool fit_full_track = false;
  float full_track_width = 0.0F;
  float full_track_height = 0.0F;
  float full_track_depth = 0.0F;
  std::vector<bool> visible_world_objects;
};

struct RenderStats {
  std::uint64_t submitted_faces = 0U;
  std::uint64_t submitted_world_objects = 0U;
  std::uint64_t shaded_faces = 0U;
  std::uint64_t light_contributions = 0U;
  std::uint64_t submitted_route_segments = 0U;
  std::uint64_t depth_tested_fragments = 0U;
  std::uint64_t depth_rejected_fragments = 0U;
  int depth_width = 0;
  int depth_height = 0;
};

struct DepthRenderResources {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> color;
  std::vector<float> values;
  SdlPointer<SDL_Texture, SDL_DestroyTexture> texture{nullptr,
                                                      SDL_DestroyTexture};
};

struct ViewerFont {
  std::array<std::uint32_t, mh::content::fnt_glyph_count> advances{};
  int tile_width = 0;
  int tile_height = 0;
  int minimum_x = 0;
  SdlPointer<SDL_Texture, SDL_DestroyTexture> texture{nullptr,
                                                      SDL_DestroyTexture};
};

struct VertexLighting {
  std::array<float, 3U> color{1.0F, 1.0F, 1.0F};
  std::uint64_t contributions = 0U;
};

std::string ascii_lower(std::string value) {
  for (auto &character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
  }
  return value;
}

std::string ascii_upper(std::string value) {
  for (auto &character : value) {
    if (character >= 'a' && character <= 'z') {
      character = static_cast<char>(character - ('a' - 'A'));
    }
  }
  return value;
}

std::string filename_lower(const std::filesystem::path &path) {
  return ascii_lower(path.filename().generic_string());
}

void require(const bool condition, const std::string_view operation) {
  if (!condition) {
    throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
  }
}

std::uint32_t fnt_store_width(const mh::content::FntOperationKind kind) {
  switch (kind) {
  case mh::content::FntOperationKind::store1:
    return 1U;
  case mh::content::FntOperationKind::store2:
    return 2U;
  case mh::content::FntOperationKind::store4:
    return 4U;
  case mh::content::FntOperationKind::store8:
    return 8U;
  default:
    return 0U;
  }
}

ViewerFont load_viewer_font(SDL_Renderer *renderer,
                            const std::filesystem::path &path) {
  const auto source = mh::content::read_fnt(path);
  int minimum_x = 0;
  int maximum_x = 1;
  int maximum_row = static_cast<int>(source.line_metric);
  for (const auto &glyph : source.glyphs) {
    maximum_x = std::max(maximum_x, static_cast<int>(glyph.advance_width));
    int row = 0;
    for (const auto &operation : glyph.operations) {
      if (operation.kind == mh::content::FntOperationKind::advance_row) {
        ++row;
      } else if (operation.kind ==
                 mh::content::FntOperationKind::advance_two_rows) {
        row += 2;
      } else {
        const auto start = static_cast<int>(operation.displacement);
        minimum_x = std::min(minimum_x, start);
        maximum_x = std::max(
            maximum_x,
            start + static_cast<int>(fnt_store_width(operation.kind)));
        maximum_row = std::max(maximum_row, row + 1);
      }
    }
  }
  ViewerFont font;
  font.minimum_x = minimum_x;
  font.tile_width = maximum_x - minimum_x;
  font.tile_height = maximum_row;
  constexpr int atlas_columns = 16;
  constexpr int atlas_rows = 16;
  const auto atlas_width = font.tile_width * atlas_columns;
  const auto atlas_height = font.tile_height * atlas_rows;
  std::vector<std::uint8_t> pixels(
      static_cast<std::size_t>(atlas_width) * atlas_height * 4U, 0U);
  for (std::size_t glyph_index = 0U; glyph_index < source.glyphs.size();
       ++glyph_index) {
    const auto &glyph = source.glyphs[glyph_index];
    font.advances[glyph_index] = glyph.advance_width;
    const auto tile_x =
        static_cast<int>(glyph_index % atlas_columns) * font.tile_width;
    const auto tile_y =
        static_cast<int>(glyph_index / atlas_columns) * font.tile_height;
    int row = 0;
    for (const auto &operation : glyph.operations) {
      if (operation.kind == mh::content::FntOperationKind::advance_row) {
        ++row;
        continue;
      }
      if (operation.kind ==
          mh::content::FntOperationKind::advance_two_rows) {
        row += 2;
        continue;
      }
      const auto store_width = fnt_store_width(operation.kind);
      const auto x = tile_x + static_cast<int>(operation.displacement) -
                     font.minimum_x;
      const auto y = tile_y + row;
      for (std::uint32_t pixel = 0U; pixel < store_width; ++pixel) {
        const auto offset =
            (static_cast<std::size_t>(y) * atlas_width + x + pixel) * 4U;
        pixels[offset] = 255U;
        pixels[offset + 1U] = 255U;
        pixels[offset + 2U] = 255U;
        pixels[offset + 3U] = 255U;
      }
    }
  }
  font.texture.reset(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, atlas_width,
                                       atlas_height));
  require(font.texture != nullptr, "create viewer menu font atlas");
  require(SDL_UpdateTexture(font.texture.get(), nullptr, pixels.data(),
                            atlas_width * 4),
          "upload viewer menu font atlas");
  require(SDL_SetTextureBlendMode(font.texture.get(), SDL_BLENDMODE_BLEND),
          "set viewer menu font blending");
  require(SDL_SetTextureScaleMode(font.texture.get(), SDL_SCALEMODE_NEAREST),
          "set viewer menu font scaling");
  return font;
}

[[maybe_unused]] void draw_viewer_text(SDL_Renderer *renderer,
                      const ViewerFont &font,
                      const float x, const float y,
                      const std::string_view text, const SDL_FColor color,
                      const float scale = 1.0F) {
  require(SDL_SetTextureColorModFloat(font.texture.get(), color.r, color.g,
                                      color.b),
          "set viewer menu font color");
  require(SDL_SetTextureAlphaModFloat(font.texture.get(), color.a),
          "set viewer menu font alpha");
  auto pen_x = x;
  for (const auto character : text) {
    const auto glyph =
        static_cast<std::uint8_t>(static_cast<unsigned char>(character));
    const SDL_FRect source{
        static_cast<float>(glyph % 16U) * font.tile_width,
        static_cast<float>(glyph / 16U) * font.tile_height,
        static_cast<float>(font.tile_width),
        static_cast<float>(font.tile_height)};
    const SDL_FRect target{
        pen_x + static_cast<float>(font.minimum_x) * scale, y,
        static_cast<float>(font.tile_width) * scale,
        static_cast<float>(font.tile_height) * scale};
    require(SDL_RenderTexture(renderer, font.texture.get(), &source, &target),
            "render viewer menu text");
    pen_x += static_cast<float>(font.advances[glyph]) * scale;
  }
}

struct UiTextTexture {
  int width = 0;
  int height = 0;
  SdlPointer<SDL_Texture, SDL_DestroyTexture> texture{nullptr,
                                                      SDL_DestroyTexture};
};

struct UiTextCache {
  std::map<std::string, UiTextTexture> textures;
};

#ifdef _WIN32
std::wstring utf8_to_wide(const std::string_view text) {
  if (text.empty()) {
    return {};
  }
  const auto count = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
      nullptr, 0);
  if (count <= 0) {
    throw std::runtime_error("convert model-viewer UI text to UTF-16");
  }
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                          static_cast<int>(text.size()), result.data(),
                          count) != count) {
    throw std::runtime_error("convert model-viewer UI text to UTF-16");
  }
  return result;
}

const UiTextTexture &ensure_ui_text(SDL_Renderer *renderer, UiTextCache &cache,
                                    const std::string_view text,
                                    const int pixel_height, const bool bold) {
  const auto key = std::to_string(pixel_height) + (bold ? "|b|" : "|r|") +
                   std::string(text);
  if (const auto existing = cache.textures.find(key);
      existing != cache.textures.end()) {
    return existing->second;
  }
  const auto wide = utf8_to_wide(text);
  const auto dc = CreateCompatibleDC(nullptr);
  if (dc == nullptr) {
    throw std::runtime_error("create model-viewer UI font context");
  }
  const auto font = CreateFontW(
      -pixel_height, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE,
      FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  if (font == nullptr) {
    DeleteDC(dc);
    throw std::runtime_error("create model-viewer UI font");
  }
  const auto previous_font = SelectObject(dc, font);
  SIZE measured{};
  if (!GetTextExtentPoint32W(dc, wide.data(), static_cast<int>(wide.size()),
                             &measured)) {
    SelectObject(dc, previous_font);
    DeleteObject(font);
    DeleteDC(dc);
    throw std::runtime_error("measure model-viewer UI text");
  }
  const auto width = std::max(1L, measured.cx + 3L);
  const auto height = std::max(1L, measured.cy + 3L);
  BITMAPINFO bitmap_info{};
  bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bitmap_info.bmiHeader.biWidth = width;
  bitmap_info.bmiHeader.biHeight = -height;
  bitmap_info.bmiHeader.biPlanes = 1;
  bitmap_info.bmiHeader.biBitCount = 32;
  bitmap_info.bmiHeader.biCompression = BI_RGB;
  void *dib_pixels = nullptr;
  const auto bitmap =
      CreateDIBSection(dc, &bitmap_info, DIB_RGB_COLORS, &dib_pixels, nullptr,
                       0U);
  if (bitmap == nullptr || dib_pixels == nullptr) {
    SelectObject(dc, previous_font);
    DeleteObject(font);
    DeleteDC(dc);
    throw std::runtime_error("create model-viewer UI text bitmap");
  }
  const auto previous_bitmap = SelectObject(dc, bitmap);
  std::memset(dib_pixels, 0,
              static_cast<std::size_t>(width) * height * 4U);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(255, 255, 255));
  if (!TextOutW(dc, 1, 0, wide.data(), static_cast<int>(wide.size()))) {
    SelectObject(dc, previous_bitmap);
    SelectObject(dc, previous_font);
    DeleteObject(bitmap);
    DeleteObject(font);
    DeleteDC(dc);
    throw std::runtime_error("rasterize model-viewer UI text");
  }
  GdiFlush();
  const auto *source = static_cast<const std::uint8_t *>(dib_pixels);
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4U,
                                 0U);
  for (std::size_t pixel = 0U;
       pixel < static_cast<std::size_t>(width) * height; ++pixel) {
    const auto alpha = std::max({source[pixel * 4U], source[pixel * 4U + 1U],
                                 source[pixel * 4U + 2U]});
    rgba[pixel * 4U] = 255U;
    rgba[pixel * 4U + 1U] = 255U;
    rgba[pixel * 4U + 2U] = 255U;
    rgba[pixel * 4U + 3U] = alpha;
  }
  SelectObject(dc, previous_bitmap);
  SelectObject(dc, previous_font);
  DeleteObject(bitmap);
  DeleteObject(font);
  DeleteDC(dc);

  UiTextTexture result;
  result.width = static_cast<int>(width);
  result.height = static_cast<int>(height);
  result.texture.reset(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                         SDL_TEXTUREACCESS_STATIC,
                                         result.width, result.height));
  require(result.texture != nullptr, "create modern viewer UI text texture");
  require(SDL_UpdateTexture(result.texture.get(), nullptr, rgba.data(),
                            result.width * 4),
          "upload modern viewer UI text texture");
  require(SDL_SetTextureBlendMode(result.texture.get(), SDL_BLENDMODE_BLEND),
          "set modern viewer UI text blending");
  require(SDL_SetTextureScaleMode(result.texture.get(), SDL_SCALEMODE_LINEAR),
          "set modern viewer UI text scaling");
  const auto inserted = cache.textures.emplace(key, std::move(result)).first;
  return inserted->second;
}
#endif

float ui_text_width(SDL_Renderer *renderer, UiTextCache &cache,
                    const ViewerFont &fallback, const std::string_view text,
                    const int pixel_height, const bool bold = false) {
#ifdef _WIN32
  (void)fallback;
  return static_cast<float>(
      ensure_ui_text(renderer, cache, text, pixel_height, bold).width);
#else
  const auto scale = static_cast<float>(pixel_height) /
                     std::max(1, fallback.tile_height);
  auto width = 0.0F;
  for (const auto character : text) {
    const auto glyph =
        static_cast<std::uint8_t>(static_cast<unsigned char>(character));
    width += static_cast<float>(fallback.advances[glyph]) * scale;
  }
  return width;
#endif
}

void draw_ui_text(SDL_Renderer *renderer, UiTextCache &cache,
                  const ViewerFont &fallback, const float x, const float y,
                  const std::string_view text, const SDL_FColor color,
                  const int pixel_height, const bool bold = false) {
#ifdef _WIN32
  (void)fallback;
  const auto &entry =
      ensure_ui_text(renderer, cache, text, pixel_height, bold);
  require(SDL_SetTextureColorModFloat(entry.texture.get(), color.r, color.g,
                                      color.b),
          "set modern viewer UI text color");
  require(SDL_SetTextureAlphaModFloat(entry.texture.get(), color.a),
          "set modern viewer UI text alpha");
  const SDL_FRect target{x, y, static_cast<float>(entry.width),
                         static_cast<float>(entry.height)};
  require(SDL_RenderTexture(renderer, entry.texture.get(), nullptr, &target),
          "render modern viewer UI text");
#else
  draw_viewer_text(renderer, fallback, x, y, text, color,
                   static_cast<float>(pixel_height) /
                       std::max(1, fallback.tile_height));
#endif
}

Options parse_options(const int argc, char **argv) {
  if (argc < 3) {
    throw std::runtime_error(
        "usage: Viewer <content-root>"
        " [model-relative-path|Game/carNN.car|--cars|--tracks]"
        " [--track track1..track8] [--pack tex1|tex2] [--overrides <root>]"
        " [--track-definition <Game/TrackN.trk>]"
        " [--frames <count>] [--report <path>] [--screenshot <bmp>]"
        " [--view inspect|rear|top] [--zoom 0.25..8] [--local-cell <index>]"
        " [--route-group <index>] [--route-sample <index>] [--route-chase]"
        " [--collision] [--lights] [--applied-lights] [--route]"
        " [--car-shading flat|gouraud|reflection|glenz]"
        " [--depth] [--report-only]");
  }
  Options options;
  options.content_root = argv[1];
  options.model_relative_path = argv[2];
  if (options.model_relative_path.is_absolute()) {
    throw std::runtime_error("model path must be relative to the content root");
  }
  for (int index = 3; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--track") {
      if (++index >= argc) {
        throw std::runtime_error("--track requires a value");
      }
      options.track = ascii_lower(argv[index]);
    } else if (argument == "--track-definition") {
      if (++index >= argc) {
        throw std::runtime_error("--track-definition requires a path");
      }
      options.track_definition_relative_path =
          std::filesystem::path(argv[index]);
      if (options.track_definition_relative_path->is_absolute()) {
        throw std::runtime_error(
            "--track-definition must be relative to the content root");
      }
    } else if (argument == "--pack") {
      if (++index >= argc) {
        throw std::runtime_error("--pack requires a value");
      }
      options.pack = ascii_lower(argv[index]);
    } else if (argument == "--overrides") {
      if (++index >= argc) {
        throw std::runtime_error("--overrides requires a path");
      }
      options.override_root = std::filesystem::path(argv[index]);
    } else if (argument == "--view") {
      if (++index >= argc) {
        throw std::runtime_error("--view requires a value");
      }
      options.view = ascii_lower(argv[index]);
    } else if (argument == "--zoom") {
      if (++index >= argc) {
        throw std::runtime_error("--zoom requires a value");
      }
      options.initial_zoom = std::stof(argv[index]);
      if (!std::isfinite(options.initial_zoom) ||
          options.initial_zoom < 0.25F || options.initial_zoom > 8.0F) {
        throw std::runtime_error("--zoom must be between 0.25 and 8");
      }
    } else if (argument == "--frames") {
      if (++index >= argc) {
        throw std::runtime_error("--frames requires a count");
      }
      const auto frames = std::stoull(argv[index]);
      if (frames == 0U || frames > 36000U) {
        throw std::runtime_error(
            "--frames must be in the range 1 through 36000");
      }
      options.maximum_frames = static_cast<std::uint32_t>(frames);
    } else if (argument == "--local-cell") {
      if (++index >= argc) {
        throw std::runtime_error("--local-cell requires an index");
      }
      const auto cell = std::stoull(argv[index]);
      if (cell > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("--local-cell exceeds the platform range");
      }
      options.local_cell = static_cast<std::size_t>(cell);
    } else if (argument == "--route-group") {
      if (++index >= argc) {
        throw std::runtime_error("--route-group requires an index");
      }
      const auto group = std::stoull(argv[index]);
      if (group > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("--route-group exceeds the platform range");
      }
      options.route_group = static_cast<std::size_t>(group);
      options.route_overlay = true;
    } else if (argument == "--route-sample") {
      if (++index >= argc) {
        throw std::runtime_error("--route-sample requires an index");
      }
      const auto sample = std::stoull(argv[index]);
      if (sample > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("--route-sample exceeds the platform range");
      }
      options.route_sample = static_cast<std::size_t>(sample);
      options.route_overlay = true;
    } else if (argument == "--route-chase") {
      options.route_chase = true;
    } else if (argument == "--report") {
      if (++index >= argc) {
        throw std::runtime_error("--report requires a path");
      }
      options.report_path = std::filesystem::path(argv[index]);
    } else if (argument == "--screenshot") {
      if (++index >= argc) {
        throw std::runtime_error("--screenshot requires a path");
      }
      options.screenshot_path = std::filesystem::path(argv[index]);
    } else if (argument == "--report-only") {
      options.report_only = true;
    } else if (argument == "--collision") {
      options.collision_overlay = true;
    } else if (argument == "--lights") {
      options.lighting_overlay = true;
    } else if (argument == "--applied-lights") {
      options.applied_lighting = true;
    } else if (argument == "--route") {
      options.route_overlay = true;
    } else if (argument == "--car-shading") {
      if (++index >= argc) {
        throw std::runtime_error("--car-shading requires a value");
      }
      options.car_shading = parse_car_shading(ascii_lower(argv[index]));
    } else if (argument == "--depth") {
      options.depth_buffer = true;
    } else {
      throw std::runtime_error("unknown option: " + std::string(argument));
    }
  }
  if (options.pack != "tex1" && options.pack != "tex2") {
    throw std::runtime_error("--pack must be tex1 or tex2");
  }
  if (options.view != "inspect" && options.view != "rear" &&
      options.view != "top") {
    throw std::runtime_error("--view must be inspect, rear, or top");
  }
  const auto route_focus_count =
      static_cast<unsigned>(options.route_group.has_value()) +
      static_cast<unsigned>(options.route_sample.has_value());
  if (route_focus_count > 1U ||
      (options.local_cell.has_value() && route_focus_count != 0U)) {
    throw std::runtime_error("--local-cell, --route-group, and --route-sample "
                             "are mutually exclusive");
  }
  if (options.route_chase && route_focus_count == 0U) {
    throw std::runtime_error(
        "--route-chase requires --route-group or --route-sample");
  }
  if (!options.track.empty() &&
      (options.track.size() != 6U || !options.track.starts_with("track") ||
       options.track.back() < '1' || options.track.back() > '8')) {
    throw std::runtime_error("--track must be track1 through track8");
  }
  return options;
}

std::filesystem::path checked_model_path(const Options &options) {
  std::error_code error;
  const auto root =
      std::filesystem::absolute(options.content_root, error).lexically_normal();
  const auto model =
      std::filesystem::absolute(root / options.model_relative_path, error)
          .lexically_normal();
  auto relative = std::filesystem::relative(model, root, error);
  if (error || relative.empty() || relative.is_absolute() ||
      (!relative.empty() && *relative.begin() == "..")) {
    throw std::runtime_error("model path escapes the content root");
  }
  return model;
}

std::filesystem::path content_relative_path(std::string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  auto path = std::filesystem::path(value).lexically_normal();
  if (path.empty() || path.is_absolute() ||
      (!path.empty() && *path.begin() == "..")) {
    throw std::runtime_error("CAR component path escapes the content root");
  }
  return path;
}

std::filesystem::path
checked_content_path(const Options &options,
                     const std::filesystem::path &relative) {
  std::error_code error;
  const auto root =
      std::filesystem::absolute(options.content_root, error).lexically_normal();
  const auto result =
      std::filesystem::absolute(root / relative, error).lexically_normal();
  const auto within = std::filesystem::relative(result, root, error);
  if (error || within.empty() || within.is_absolute() ||
      (!within.empty() && *within.begin() == "..")) {
    throw std::runtime_error("component path escapes the content root");
  }
  return result;
}

std::filesystem::path
resolve_relative_case_insensitive(const std::filesystem::path &root,
                                  std::string reference) {
  std::replace(reference.begin(), reference.end(), '\\', '/');
  const auto relative = std::filesystem::path(reference).lexically_normal();
  if (relative.empty() || relative.is_absolute()) {
    throw std::runtime_error("authored path is not relative: " + reference);
  }
  auto current = root;
  for (const auto &part : relative) {
    const auto part_text = part.string();
    if (part_text.empty() || part_text == "." || part_text == "..") {
      throw std::runtime_error("authored path leaves its content root: " +
                               reference);
    }
    const auto wanted = ascii_lower(part_text);
    std::optional<std::filesystem::path> match;
    for (const auto &entry : std::filesystem::directory_iterator(current)) {
      if (ascii_lower(entry.path().filename().string()) != wanted) {
        continue;
      }
      if (match.has_value()) {
        throw std::runtime_error("authored path is case-ambiguous: " +
                                 reference);
      }
      match = entry.path();
    }
    if (!match.has_value()) {
      throw std::runtime_error("authored path was not found: " + reference);
    }
    current = *match;
  }
  return current;
}

std::filesystem::path model_lod_path(const std::string &base, const char lod) {
  return content_relative_path(base + lod + ".MYO");
}

ViewerCache &viewer_cache(const Options &options) {
  static ViewerCache cache;
  const auto root =
      std::filesystem::absolute(options.content_root).lexically_normal();
  if (cache.content_root != root) {
    cache = ViewerCache{};
    cache.content_root = root;
  }
  return cache;
}

const mh::content::MyoData &
cached_model(const Options &options, const std::filesystem::path &relative) {
  auto &cache = viewer_cache(options);
  const auto key = ascii_lower(relative.lexically_normal().generic_string());
  const auto existing = cache.models.find(key);
  if (existing != cache.models.end()) {
    return existing->second;
  }
  const auto inserted = cache.models.emplace(
      key, mh::content::read_myo(checked_content_path(options, relative)));
  return inserted.first->second;
}

const mh::content::MywData &
cached_world(const Options &options, const std::filesystem::path &relative) {
  auto &cache = viewer_cache(options);
  const auto key = ascii_lower(relative.lexically_normal().generic_string());
  const auto existing = cache.worlds.find(key);
  if (existing != cache.worlds.end()) {
    return existing->second;
  }
  const auto inserted = cache.worlds.emplace(
      key, mh::content::read_myw(checked_content_path(options, relative)));
  return inserted.first->second;
}

const mh::content::AiRouteData &
cached_route(const Options &options, const std::filesystem::path &path) {
  auto &cache = viewer_cache(options);
  const auto key = ascii_lower(
      std::filesystem::absolute(path).lexically_normal().generic_string());
  const auto existing = cache.routes.find(key);
  if (existing != cache.routes.end()) {
    return existing->second;
  }
  const auto inserted =
      cache.routes.emplace(key, mh::content::read_ai_route(path));
  return inserted.first->second;
}

const std::vector<TrackSource> &discover_track_sources(const Options &options);

const mh::content::TextureCatalog &
cached_texture_catalog(const Options &options,
                       const std::set<std::string> &required_materials) {
  auto &cache = viewer_cache(options);
  if (cache.texture_catalog.has_value() &&
      !cache.texture_catalog_focus_materials.empty() &&
      !std::all_of(required_materials.begin(), required_materials.end(),
                   [&cache](const auto &material) {
                     return cache.texture_catalog_focus_materials.contains(
                         material);
                   })) {
    // The initial vehicle can use a focused PDI-only catalog. The optional
    // S40 is installed as Car99 and adds a small loose-texture directory;
    // merge that directory directly instead of rescanning and hashing the
    // entire installed game.
    const auto s40_relative = std::filesystem::path("Cars") / "Car99";
    const auto s40_root = options.content_root / s40_relative;
    auto extended_with_s40 = false;
    if (std::filesystem::is_directory(s40_root)) {
      auto s40_catalog = mh::content::build_texture_catalog(s40_root);
      std::set<std::string> s40_materials;
      for (const auto &asset : s40_catalog.assets) {
        s40_materials.insert(
            ascii_lower(std::filesystem::path(asset.source_path)
                            .filename()
                            .generic_string()));
      }
      extended_with_s40 = std::all_of(
          required_materials.begin(), required_materials.end(),
          [&cache, &s40_materials](const auto &material) {
            return cache.texture_catalog_focus_materials.contains(material) ||
                   s40_materials.contains(material);
          });
      if (extended_with_s40) {
        constexpr std::string_view loose_prefix = "texture/loose/";
        for (auto &asset : s40_catalog.assets) {
          if (!asset.logical_id.starts_with(loose_prefix)) {
            continue;
          }
          asset.logical_id =
              "texture/loose/cars/car99/" +
              asset.logical_id.substr(loose_prefix.size());
          asset.source_path =
              (s40_relative / std::filesystem::path(asset.source_path))
                  .generic_string();
          cache.texture_catalog->assets.push_back(std::move(asset));
        }
        cache.texture_catalog->loose_tga_count +=
            s40_catalog.loose_tga_count;
        cache.texture_catalog->loose_iff_count +=
            s40_catalog.loose_iff_count;
        cache.texture_catalog->pixel_count += s40_catalog.pixel_count;
        cache.texture_catalog->transparent_pixels +=
            s40_catalog.transparent_pixels;
        std::sort(cache.texture_catalog->assets.begin(),
                  cache.texture_catalog->assets.end(),
                  [](const auto &left, const auto &right) {
                    return left.logical_id < right.logical_id;
                  });
        cache.texture_catalog_focus_materials.insert(s40_materials.begin(),
                                                     s40_materials.end());
      }
    }
    if (!extended_with_s40) {
      // Let the normal focused-catalog path below rebuild the selected
      // track's PDI set before considering a full installation scan. This is
      // especially important when switching from Car99 to track view: a full
      // scan blocks the window long enough for Windows to report an app hang.
      cache.texture_catalog.reset();
      cache.texture_catalog_focus_materials.clear();
    }
  }
  if (!cache.texture_catalog.has_value()) {
    bool focused_catalog = false;
    // Car99 owns all of its materials in one small loose-texture directory.
    // A direct launch therefore does not need to enumerate every PDI and
    // loose texture under the install root first.
    const auto car99_relative = std::filesystem::path("Cars") / "Car99";
    const auto car99_root = options.content_root / car99_relative;
    if (std::filesystem::is_directory(car99_root)) {
      auto catalog = mh::content::build_texture_catalog(car99_root);
      std::set<std::string> entry_names;
      for (const auto &asset : catalog.assets) {
        entry_names.insert(
            ascii_lower(std::filesystem::path(asset.source_path)
                            .filename()
                            .generic_string()));
      }
      focused_catalog =
          !required_materials.empty() &&
          std::all_of(required_materials.begin(), required_materials.end(),
                      [&entry_names](const auto &material) {
                        return entry_names.contains(material);
                      });
      if (focused_catalog) {
        constexpr std::string_view loose_prefix = "texture/loose/";
        catalog.root =
            std::filesystem::absolute(options.content_root).lexically_normal();
        for (auto &asset : catalog.assets) {
          if (asset.logical_id.starts_with(loose_prefix)) {
            asset.logical_id =
                "texture/loose/cars/car99/" +
                asset.logical_id.substr(loose_prefix.size());
            asset.source_path =
                (car99_relative / std::filesystem::path(asset.source_path))
                    .generic_string();
          }
        }
        std::sort(catalog.assets.begin(), catalog.assets.end(),
                  [](const auto &left, const auto &right) {
                    return left.logical_id < right.logical_id;
                  });
        cache.texture_catalog = std::move(catalog);
        cache.texture_catalog_focus_materials = std::move(entry_names);
      }
    }
    if (!focused_catalog && !options.track.empty() && !options.pack.empty()) {
      mh::content::TextureCatalog catalog;
      catalog.root =
          std::filesystem::absolute(options.content_root).lexically_normal();
      std::set<std::string> archive_keys;
      std::set<std::string> entry_names;
      for (const auto &source : discover_track_sources(options)) {
        for (const auto *const pack : {"tex1", "tex2"}) {
          const auto relative =
              (content_relative_path(source.definition.base_path) / "Packed" /
               (std::string(pack) + ".pdi"))
                  .lexically_normal();
          const auto archive_key = ascii_lower(relative.generic_string());
          if (!archive_keys.insert(archive_key).second) {
            continue;
          }
          auto archive =
              mh::content::read_pdi(checked_content_path(options, relative));
          const auto selected_archive =
              source.texture_track == options.track && pack == options.pack;
          for (const auto &entry : archive.entries) {
            if (selected_archive) {
              entry_names.insert(ascii_lower(entry.name));
            }
            mh::content::TextureAsset asset;
            asset.logical_id =
                "texture/pdi/" + archive_key + "/" + ascii_lower(entry.name);
            asset.source_kind = "pdi-iff";
            asset.source_path = relative.generic_string();
            asset.entry_name = entry.name;
            asset.source_bytes = archive.file_bytes;
            asset.content_bytes = entry.byte_size;
            asset.width = entry.width;
            asset.height = entry.height;
            asset.transparent_pixels = entry.transparent_pixels;
            catalog.pixel_count +=
                static_cast<std::uint64_t>(entry.width) * entry.height;
            catalog.transparent_pixels += entry.transparent_pixels;
            catalog.assets.push_back(std::move(asset));
          }
          ++catalog.pdi_archive_count;
          if (selected_archive) {
            cache.pdi_archives.emplace(archive_key, std::move(archive));
          }
        }
      }
      focused_catalog =
          std::all_of(required_materials.begin(), required_materials.end(),
                      [&entry_names](const auto &material) {
                        return entry_names.contains(material);
                      });
      if (focused_catalog) {
        catalog.pdi_texture_count = catalog.assets.size();
        std::sort(catalog.assets.begin(), catalog.assets.end(),
                  [](const auto &left, const auto &right) {
                    return left.logical_id < right.logical_id;
                  });
        cache.texture_catalog = std::move(catalog);
        cache.texture_catalog_focus_materials = std::move(entry_names);
      }
    }
    if (!focused_catalog) {
      cache.texture_catalog =
          mh::content::build_texture_catalog(options.content_root);
      cache.texture_catalog_focus_materials.clear();
    }
  }
  return *cache.texture_catalog;
}

const mh::content::PamRgbaImage &
cached_texture_image(const Options &options,
                     const mh::content::TextureCatalog &catalog,
                     const mh::content::TextureAsset &asset) {
  auto &cache = viewer_cache(options);
  const auto existing = cache.texture_images.find(asset.logical_id);
  if (existing != cache.texture_images.end()) {
    return existing->second;
  }
  mh::content::PamRgbaImage image;
  if (asset.source_kind == "pdi-iff") {
    const auto archive_key = ascii_lower(asset.source_path);
    auto archive = cache.pdi_archives.find(archive_key);
    if (archive == cache.pdi_archives.end()) {
      archive = cache.pdi_archives
                    .emplace(archive_key,
                             mh::content::read_pdi(
                                 catalog.root /
                                 std::filesystem::path(asset.source_path)))
                    .first;
    }
    const auto entry = std::find_if(
        archive->second.entries.begin(), archive->second.entries.end(),
        [&asset](const auto &candidate) {
          return ascii_lower(candidate.name) == ascii_lower(asset.entry_name);
        });
    if (entry == archive->second.entries.end()) {
      throw std::runtime_error(
          "cataloged PDI texture entry is no longer present: " +
          asset.logical_id);
    }
    const auto start =
        static_cast<std::size_t>(archive->second.payload_offset) +
        entry->relative_offset;
    const auto content =
        std::span<const std::uint8_t>(archive->second.source_bytes)
            .subspan(start, entry->byte_size);
    const auto decoded = mh::content::parse_iff_ilbm(content);
    image.width = decoded.width;
    image.height = decoded.height;
    image.palette_indices = decoded.palette_indices;
    image.rgba = decoded.rgba;
  } else {
    image = mh::content::load_texture_asset_rgba(catalog, asset);
  }
  if (image.width != asset.width || image.height != asset.height) {
    throw std::runtime_error(
        "decoded texture dimensions changed since catalog construction");
  }
  const auto inserted =
      cache.texture_images.emplace(asset.logical_id, std::move(image));
  return inserted.first->second;
}

const std::vector<VehicleSource> &
discover_vehicle_sources(const Options &options, const char lod) {
  if (lod < '0' || lod > '3') {
    throw std::runtime_error("vehicle LOD is outside the supported range");
  }
  auto &cached_sources =
      viewer_cache(options)
          .vehicle_sources[static_cast<std::size_t>(lod - '0')];
  if (cached_sources.has_value()) {
    return *cached_sources;
  }
  const auto game_relative = std::filesystem::path("Game");
  const auto game_path = checked_content_path(options, game_relative);
  std::error_code error;
  if (!std::filesystem::is_directory(game_path, error) || error) {
    cached_sources.emplace();
    return *cached_sources;
  }

  std::vector<std::filesystem::path> definitions;
  for (const auto &entry : std::filesystem::directory_iterator(game_path)) {
    if (entry.is_regular_file() &&
        ascii_lower(entry.path().extension().generic_string()) == ".car") {
      definitions.push_back(entry.path());
    }
  }
  std::sort(definitions.begin(), definitions.end(),
            [](const auto &left, const auto &right) {
              return ascii_lower(left.filename().generic_string()) <
                     ascii_lower(right.filename().generic_string());
            });

  std::vector<VehicleSource> result;
  for (const auto &definition_path : definitions) {
    auto definition = mh::content::read_car(definition_path);
    const auto model_relative = model_lod_path(definition.model_base, lod);
    const auto model_path = checked_content_path(options, model_relative);
    if (!std::filesystem::is_regular_file(model_path, error) || error) {
      throw std::runtime_error(
          "CAR definition references a missing LOD model: " +
          definition_path.filename().generic_string());
    }
    // Shared meshes may have different names, colours, and wheel placement.
    result.push_back(VehicleSource{model_relative, std::move(definition),
                                   game_relative / definition_path.filename()});
  }
  cached_sources = std::move(result);
  return *cached_sources;
}

void resolve_vehicle_roster_default(Options &options) {
  if (ascii_lower(options.model_relative_path.extension().string()) == ".car") {
    options.car_definition_relative_path = options.model_relative_path;
    const auto car = mh::content::read_car(
        checked_content_path(options, options.model_relative_path));
    options.model_relative_path = model_lod_path(car.model_base, '0');
    return;
  }
  if (options.model_relative_path.generic_string() != "--cars") {
    return;
  }
  const auto &sources = discover_vehicle_sources(options, '0');
  if (sources.empty()) {
    throw std::runtime_error("no original Game/*.car definitions were found");
  }
  options.model_relative_path = sources.front().model_relative_path;
  options.car_definition_relative_path = sources.front().definition_relative_path;
}

std::string infer_track(const std::filesystem::path &model_path) {
  for (const auto &part : model_path) {
    const auto folded = ascii_lower(part.generic_string());
    if (folded.size() == 6U && folded.starts_with("track") &&
        folded.back() >= '1' && folded.back() <= '8') {
      return folded;
    }
  }
  return {};
}

std::optional<std::uint32_t>
track_definition_number(const std::filesystem::path &path) {
  const auto stem = ascii_lower(path.stem().generic_string());
  if (!stem.starts_with("track") || stem.size() == 5U) {
    return std::nullopt;
  }
  std::uint64_t number = 0U;
  for (std::size_t index = 5U; index < stem.size(); ++index) {
    if (stem[index] < '0' || stem[index] > '9') {
      return std::nullopt;
    }
    number = number * 10U + static_cast<unsigned>(stem[index] - '0');
    if (number > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
  }
  return static_cast<std::uint32_t>(number);
}

const std::vector<TrackSource> &discover_track_sources(const Options &options) {
  auto &cached_sources = viewer_cache(options).track_sources;
  if (cached_sources.has_value()) {
    return *cached_sources;
  }
  const auto game_relative = std::filesystem::path("Game");
  const auto game_path = checked_content_path(options, game_relative);
  std::vector<std::filesystem::path> definitions;
  for (const auto &entry : std::filesystem::directory_iterator(game_path)) {
    if (entry.is_regular_file() &&
        ascii_lower(entry.path().extension().generic_string()) == ".trk" &&
        track_definition_number(entry.path()).has_value()) {
      definitions.push_back(entry.path());
    }
  }
  std::sort(definitions.begin(), definitions.end(),
            [](const auto &left, const auto &right) {
              const auto left_number = *track_definition_number(left);
              const auto right_number = *track_definition_number(right);
              if (left_number != right_number) {
                return left_number < right_number;
              }
              return ascii_lower(left.filename().generic_string()) <
                     ascii_lower(right.filename().generic_string());
            });

  std::vector<TrackSource> result;
  std::error_code error;
  for (const auto &definition_path : definitions) {
    auto definition = mh::content::read_track_definition(definition_path);
    const auto model_relative =
        (content_relative_path(definition.base_path) /
         content_relative_path(definition.world_filename))
            .lexically_normal();
    const auto model_path = checked_content_path(options, model_relative);
    if (!std::filesystem::is_regular_file(model_path, error) || error) {
      throw std::runtime_error(
          "TRK definition references a missing world model: " +
          definition_path.filename().generic_string());
    }
    const auto texture_track = infer_track(model_relative);
    if (texture_track.empty()) {
      throw std::runtime_error(
          "TRK base path does not identify a track texture scope: " +
          definition_path.filename().generic_string());
    }
    result.push_back(TrackSource{game_relative / definition_path.filename(),
                                 model_relative, texture_track,
                                 std::move(definition)});
  }
  cached_sources = std::move(result);
  return *cached_sources;
}

const TrackSource &selected_track_source(const Options &options) {
  const auto &sources = discover_track_sources(options);
  if (sources.empty()) {
    throw std::runtime_error(
        "no original Game/Track*.trk definitions were found");
  }
  if (options.track_definition_relative_path.has_value()) {
    const auto selected =
        ascii_lower(options.track_definition_relative_path->lexically_normal()
                        .generic_string());
    const auto source = std::find_if(
        sources.begin(), sources.end(), [&selected](const auto &candidate) {
          return ascii_lower(
                     candidate.definition_relative_path.lexically_normal()
                         .generic_string()) == selected;
        });
    if (source == sources.end()) {
      throw std::runtime_error(
          "--track-definition is not a discovered original TRK definition");
    }
    return *source;
  }
  const auto selected_model = ascii_lower(
      options.model_relative_path.lexically_normal().generic_string());
  const auto source = std::find_if(
      sources.begin(), sources.end(),
      [&selected_model, &options](const auto &candidate) {
        return ascii_lower(candidate.model_relative_path.lexically_normal()
                               .generic_string()) == selected_model &&
               (options.track.empty() ||
                candidate.texture_track == options.track);
      });
  if (source == sources.end()) {
    throw std::runtime_error(
        "the selected world is not referenced by an original TRK definition");
  }
  return *source;
}

const TrackSource &adjacent_track_source(const Options &options,
                                         const bool previous) {
  const auto &sources = discover_track_sources(options);
  const auto &selected = selected_track_source(options);
  const auto current =
      static_cast<std::size_t>(std::distance(sources.data(), &selected));
  const auto adjacent = previous
                            ? current == 0U ? sources.size() - 1U : current - 1U
                            : (current + 1U) % sources.size();
  return sources[adjacent];
}

const VehicleSource &adjacent_vehicle_source(const Options &options,
                                             const bool previous) {
  const auto &sources = discover_vehicle_sources(options, '0');
  if (sources.empty()) {
    throw std::runtime_error("no original Game/*.car definitions were found");
  }
  const auto selected =
      ascii_lower(options.model_relative_path.lexically_normal().generic_string());
  const auto current_source = std::find_if(
      sources.begin(), sources.end(), [&selected, &options](const auto &candidate) {
        if (options.car_definition_relative_path) {
          return ascii_lower(candidate.definition_relative_path.generic_string()) ==
                 ascii_lower(options.car_definition_relative_path->lexically_normal().generic_string());
        }
        return ascii_lower(candidate.model_relative_path.lexically_normal()
                               .generic_string()) == selected;
      });
  const auto current = current_source == sources.end()
                           ? 0U
                           : static_cast<std::size_t>(
                                 std::distance(sources.begin(), current_source));
  const auto adjacent = previous
                            ? current == 0U ? sources.size() - 1U : current - 1U
                            : (current + 1U) % sources.size();
  return sources[adjacent];
}

void resolve_track_roster_default(Options &options) {
  const auto explicit_world =
      ascii_lower(options.model_relative_path.extension().generic_string()) ==
      ".myw";
  if (options.model_relative_path.generic_string() != "--tracks" &&
      !explicit_world) {
    return;
  }
  const auto &sources = discover_track_sources(options);
  if (sources.empty()) {
    throw std::runtime_error(
        "no original Game/Track*.trk definitions were found");
  }
  const auto &source = options.track_definition_relative_path.has_value()
                           ? selected_track_source(options)
                           : sources.front();
  options.model_relative_path = source.model_relative_path;
  options.track_definition_relative_path = source.definition_relative_path;
  options.track = source.texture_track;
}

std::optional<char> infer_lod(const std::filesystem::path &model_path) {
  const auto stem = ascii_lower(model_path.stem().generic_string());
  if (stem.size() >= 4U && stem.substr(stem.size() - 4U, 3U) == "obj" &&
      stem.back() >= '0' && stem.back() <= '3') {
    return stem.back();
  }
  return std::nullopt;
}

std::string asset_name(const mh::content::TextureAsset &asset) {
  if (!asset.entry_name.empty()) {
    return ascii_lower(asset.entry_name);
  }
  return filename_lower(std::filesystem::path(asset.source_path));
}

const mh::content::TextureAsset &
select_texture_asset(const mh::content::TextureCatalog &catalog,
                     const std::string &material, const std::string &track,
                     const std::string &pack, bool &fallback,
                     const std::string &fallback_track = {}) {
  std::vector<const mh::content::TextureAsset *> named;
  for (const auto &asset : catalog.assets) {
    if (asset_name(asset) == ascii_lower(material)) {
      named.push_back(&asset);
    }
  }
  if (named.empty()) {
    throw std::runtime_error("material has no catalog texture: " + material);
  }
  std::vector<const mh::content::TextureAsset *> scoped;
  if (!track.empty()) {
    auto scope = track;
    while (!scope.empty() && scope.back() == '/') {
      scope.pop_back();
    }
    const auto marker = scope.starts_with("cars/") ? "/" + scope + "/"
                                                  : "/tracks/" + scope + "/";
    for (const auto *asset : named) {
      const auto logical = "/" + ascii_lower(asset->logical_id);
      if (logical.find(marker) != std::string::npos) {
        scoped.push_back(asset);
      }
    }
  }
  if (scoped.empty() && track.starts_with("cars/")) {
    // Retail car textures live in the selected track's texture archive.
    const auto marker = "/tracks/" +
                        (fallback_track.empty() ? "track1" : fallback_track) + "/";
    for (const auto *asset : named) {
      if (("/" + ascii_lower(asset->logical_id)).find(marker) != std::string::npos) {
        scoped.push_back(asset);
      }
    }
  }
  fallback = !track.empty() && scoped.empty();
  auto candidates = scoped.empty() ? named : scoped;
  std::vector<const mh::content::TextureAsset *> packed;
  for (const auto *asset : candidates) {
    if (asset->source_kind != "pdi-iff" ||
        filename_lower(std::filesystem::path(asset->source_path)) ==
            pack + ".pdi") {
      packed.push_back(asset);
    }
  }
  if (!packed.empty()) {
    candidates = std::move(packed);
  }
  if (candidates.size() != 1U) {
    throw std::runtime_error("material selection remains ambiguous for " +
                             material + " in texture scope " + track +
                             " and pack " + pack);
  }
  return *candidates.front();
}

SdlPointer<SDL_Texture, SDL_DestroyTexture>
upload_texture(SDL_Renderer *renderer, const mh::content::PamRgbaImage &image) {
  SdlPointer<SDL_Texture, SDL_DestroyTexture> texture(
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                        SDL_TEXTUREACCESS_STATIC, static_cast<int>(image.width),
                        static_cast<int>(image.height)),
      SDL_DestroyTexture);
  require(texture != nullptr, "create model texture");
  require(SDL_UpdateTexture(texture.get(), nullptr, image.rgba.data(),
                            static_cast<int>(image.width * 4U)),
          "upload model texture");
  require(SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND),
          "set model texture blend mode");
  require(SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_LINEAR),
          "set model texture scale mode");
  return texture;
}

VehicleEnvironmentTexture
upload_vehicle_environment_texture(SDL_Renderer *renderer,
                                   const std::filesystem::path &path,
                                   const float color_scale) {
  if (!std::isfinite(color_scale) || color_scale < 0.0F ||
      color_scale > 16.0F) {
    throw std::runtime_error(
        "vehicle environment-map color scale is outside its safe range");
  }
  const auto source = mh::content::read_iff_ilbm(path);
  VehicleEnvironmentTexture result;
  result.image.width = source.width;
  result.image.height = source.height;
  result.image.rgba = source.rgba;
  for (std::size_t pixel = 0U; pixel < result.image.rgba.size(); pixel += 4U) {
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
      result.image.rgba[pixel + channel] = static_cast<std::uint8_t>(std::clamp(
          std::lround(static_cast<float>(result.image.rgba[pixel + channel]) *
                      color_scale),
          0L, 255L));
    }
  }
  result.texture = upload_texture(renderer, result.image);
  require(SDL_SetTextureBlendMode(result.texture.get(), SDL_BLENDMODE_NONE),
          "set vehicle environment-map blend mode");
  require(SDL_SetTextureScaleMode(result.texture.get(), SDL_SCALEMODE_LINEAR),
          "set vehicle environment-map scale mode");
  return result;
}

VehicleEnvironmentTextures
load_vehicle_environment_textures(SDL_Renderer *renderer,
                                  const mh::content::TrackDefinition &track,
                                  const std::filesystem::path &content_root) {
  VehicleEnvironmentTextures result;
  for (const auto &reference : track.references) {
    if (reference.field != "envmapname") {
      continue;
    }
    const auto path =
        resolve_relative_case_insensitive(content_root, reference.value);
    const auto name = filename_lower(path);
    if (name.starts_with("env_")) {
      if (result.environment.texture != nullptr) {
        throw std::runtime_error(
            "vehicle material track has multiple base environment maps");
      }
      result.environment =
          upload_vehicle_environment_texture(renderer, path, 1.0F);
    } else if (name.starts_with("ref_")) {
      if (result.reflection.texture != nullptr) {
        throw std::runtime_error(
            "vehicle material track has multiple reflection maps");
      }
      result.reflection =
          upload_vehicle_environment_texture(renderer, path, 1.0F);
    } else if (name.starts_with("phong_")) {
      if (result.phong.texture != nullptr) {
        throw std::runtime_error(
            "vehicle material track has multiple Phong maps");
      }
      result.phong = upload_vehicle_environment_texture(renderer, path, 1.0F);
    }
  }
  result.complete = result.environment.texture != nullptr &&
                    result.reflection.texture != nullptr &&
                    result.phong.texture != nullptr;
  if (!result.complete) {
    throw std::runtime_error(
        "vehicle material track does not provide the recovered three-map set");
  }
  return result;
}

VehicleEnvironmentMap
vehicle_environment_map(const mh::content::MyoFace &face) {
  if (!face.has_vertex_normals ||
      (face.primitive_type != 8U && face.primitive_type != 9U)) {
    return VehicleEnvironmentMap::none;
  }
  if ((face.flags & 0x60U) == 0x60U) {
    return VehicleEnvironmentMap::phong;
  }
  if ((face.flags & 0x40U) != 0U) {
    return VehicleEnvironmentMap::reflection;
  }
  return VehicleEnvironmentMap::environment;
}

const VehicleEnvironmentTexture *
vehicle_environment_texture(const VehicleEnvironmentTextures &textures,
                            const VehicleEnvironmentMap map) {
  switch (map) {
  case VehicleEnvironmentMap::environment:
    return &textures.environment;
  case VehicleEnvironmentMap::reflection:
    return &textures.reflection;
  case VehicleEnvironmentMap::phong:
    return &textures.phong;
  case VehicleEnvironmentMap::none:
    return nullptr;
  }
  return nullptr;
}

mh::content::MyoData
viewer_model_from_world(const mh::content::MywData &world) {
  mh::content::MyoData model;
  model.file_bytes = world.file_bytes;
  model.minimum = world.minimum;
  model.maximum = world.maximum;
  model.triangle_count = world.triangle_count;
  model.quad_count = world.quad_count;
  model.vertex_reference_count = world.vertex_reference_count;
  model.textured_face_count = world.material_primitive_count;
  model.texture_coordinate_count = world.texture_coordinate_count;
  model.positions = world.positions;
  model.normals = world.normals;
  model.names = world.material_names;
  model.faces.reserve(world.primitives.size());
  for (const auto &primitive : world.primitives) {
    if (primitive.vertex_count == 0U) {
      continue;
    }
    mh::content::MyoFace face;
    face.primitive_type = primitive.primitive_type;
    face.flags = primitive.flags;
    face.vertex_count = primitive.vertex_count;
    face.color = {255U, 255U, 255U};
    for (std::size_t vertex = 0U; vertex < primitive.vertex_count; ++vertex) {
      if (primitive.position_indices[vertex] >
          std::numeric_limits<std::uint16_t>::max()) {
        throw std::runtime_error(
            "MYW position index exceeds viewer model range");
      }
      face.position_indices[vertex] =
          static_cast<std::uint16_t>(primitive.position_indices[vertex]);
      face.texture_coordinates[vertex] = primitive.texture_coordinates[vertex];
    }
    if (primitive.normal_index > std::numeric_limits<std::uint16_t>::max()) {
      throw std::runtime_error("MYW normal index exceeds viewer model range");
    }
    face.normal_index = static_cast<std::uint16_t>(primitive.normal_index);
    face.has_texture_coordinates = primitive.has_texture_coordinates;
    face.material_name_index = primitive.material_name_index;
    model.faces.push_back(face);
  }
  return model;
}

struct WheelVisualSurface {
  float outer_x = 0.0F;
  float center_y = 0.0F;
  float center_z = 0.0F;
};

WheelVisualSurface wheel_visual_surface(const mh::content::MyoData &model,
                                        const bool left_side) {
  std::array<float, 3U> minimum{std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::max()};
  std::array<float, 3U> maximum{std::numeric_limits<float>::lowest(),
                                std::numeric_limits<float>::lowest(),
                                std::numeric_limits<float>::lowest()};
  bool found = false;
  for (const auto &face : model.faces) {
    if (!face.has_texture_coordinates) {
      continue;
    }
    for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
      const auto &position = model.positions[face.position_indices[vertex]];
      for (std::size_t axis = 0U; axis < 3U; ++axis) {
        minimum[axis] = std::min(minimum[axis], position[axis]);
        maximum[axis] = std::max(maximum[axis], position[axis]);
      }
      found = true;
    }
  }
  if (!found) {
    minimum = model.minimum;
    maximum = model.maximum;
  }
  return WheelVisualSurface{left_side ? minimum[0U] : maximum[0U],
                            (minimum[1U] + maximum[1U]) * 0.5F,
                            (minimum[2U] + maximum[2U]) * 0.5F};
}

Scene load_scene(SDL_Renderer *renderer, Options &options) {
  const auto model_path = checked_model_path(options);
  if (options.track.empty()) {
    options.track = infer_track(options.model_relative_path);
  }
  Scene scene;
  const auto add_component = [&](const std::string &label,
                                 const std::filesystem::path &relative,
                                 const Vec3 translation) {
    ModelComponent component;
    component.label = label;
    component.relative_path = relative;
    component.translation = translation;
    component.model = cached_model(options, relative);
    scene.components.push_back(std::move(component));
    return scene.components.size() - 1U;
  };

  const auto world_model =
      ascii_lower(options.model_relative_path.extension().generic_string()) ==
      ".myw";
  if (options.route_overlay && !world_model) {
    throw std::runtime_error("--route is available only for a MYW track");
  }
  if (world_model) {
    const auto &track_sources = discover_track_sources(options);
    const auto selected_model = ascii_lower(
        options.model_relative_path.lexically_normal().generic_string());
    const auto selected_definition =
        options.track_definition_relative_path.has_value()
            ? ascii_lower(
                  options.track_definition_relative_path->lexically_normal()
                      .generic_string())
            : std::string{};
    const auto track_source = std::find_if(
        track_sources.begin(), track_sources.end(), [&](const auto &candidate) {
          if (!selected_definition.empty()) {
            return ascii_lower(
                       candidate.definition_relative_path.lexically_normal()
                           .generic_string()) == selected_definition;
          }
          return ascii_lower(candidate.model_relative_path.lexically_normal()
                                 .generic_string()) == selected_model &&
                 candidate.texture_track == options.track;
        });
    if (track_source != track_sources.end()) {
      scene.track_name = track_source->definition.name;
      scene.track_definition_relative_path =
          track_source->definition_relative_path;
      scene.track_roster_index = static_cast<std::size_t>(
          std::distance(track_sources.begin(), track_source));
      scene.track_roster_count = track_sources.size();
      scene.world_accelerated_brightness =
          track_source->definition.environment.accelerated_brightness.value_or(
              std::array<float, 3U>{1.0F, 1.0F, 1.0F});
    }
    const auto &world = cached_world(options, options.model_relative_path);
    ModelComponent component;
    component.label = "world";
    component.relative_path = options.model_relative_path;
    component.texture_track = options.track;
    component.model = viewer_model_from_world(world);
    component.face_object_indices.reserve(world.primitives.size());
    component.face_vertex_colors.reserve(world.primitives.size());
    for (const auto &primitive : world.primitives) {
      if (primitive.vertex_count != 0U) {
        component.face_object_indices.push_back(primitive.object_index);
        component.face_vertex_colors.push_back(primitive.vertex_colors);
      }
    }
    scene.components.push_back(std::move(component));
    scene.world_lights = world.lights;
    scene.world_lens_flares = world.lens_flares;
    scene.world_objects = world.objects;
    scene.world_grid_cells = world.grid_cells;
    scene.world_grid_width = world.grid_width;
    scene.world_grid_height = world.grid_height;
    scene.world_grid_cell_size_x = world.grid_cell_size_x;
    scene.world_grid_cell_size_z = world.grid_cell_size_z;
    for (std::size_t index = 0U; index < world.grid_cells.size(); ++index) {
      if (world.grid_cells[index].object_index !=
          std::numeric_limits<std::uint32_t>::max()) {
        scene.occupied_world_cells.push_back(index);
      }
    }
    constexpr std::uint32_t section_span = 8U;
    const auto section_columns =
        (world.grid_width + section_span - 1U) / section_span;
    const auto section_rows =
        (world.grid_height + section_span - 1U) / section_span;
    const auto no_cell = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> representatives(
        static_cast<std::size_t>(section_columns) * section_rows, no_cell);
    std::vector<std::uint32_t> best_distances(
        representatives.size(), std::numeric_limits<std::uint32_t>::max());
    for (const auto cell : scene.occupied_world_cells) {
      const auto column = static_cast<std::uint32_t>(cell % world.grid_width);
      const auto row = static_cast<std::uint32_t>(cell / world.grid_width);
      const auto section_column = column / section_span;
      const auto section_row = row / section_span;
      const auto section =
          static_cast<std::size_t>(section_row) * section_columns +
          section_column;
      const auto center_column =
          std::min(world.grid_width - 1U,
                   section_column * section_span + section_span / 2U);
      const auto center_row =
          std::min(world.grid_height - 1U,
                   section_row * section_span + section_span / 2U);
      const auto column_distance = column > center_column
                                       ? column - center_column
                                       : center_column - column;
      const auto row_distance =
          row > center_row ? row - center_row : center_row - row;
      const auto distance =
          column_distance * column_distance + row_distance * row_distance;
      if (distance < best_distances[section] ||
          (distance == best_distances[section] &&
           cell < representatives[section])) {
        representatives[section] = cell;
        best_distances[section] = distance;
      }
    }
    for (const auto representative : representatives) {
      if (representative != no_cell) {
        scene.world_section_cells.push_back(representative);
      }
    }
    std::optional<std::filesystem::path> route_path;
    for (const auto *const directory : {"AI", "Ai", "ai"}) {
      const auto candidate = model_path.parent_path() / directory / "ai.dat";
      std::error_code route_error;
      if (std::filesystem::is_regular_file(candidate, route_error) &&
          !route_error) {
        route_path = candidate;
        break;
      }
    }
    if (route_path.has_value()) {
      scene.world_route = cached_route(options, *route_path);
      scene.world_route_positions.reserve(scene.world_route.samples.size());
      std::optional<float> previous_route_height;
      for (std::size_t sample_index = 0U;
           sample_index < scene.world_route.samples.size(); ++sample_index) {
        const auto &sample = scene.world_route.samples[sample_index];
        std::optional<float> predicted_route_height;
        if (sample_index != 0U && previous_route_height.has_value()) {
          const auto &previous_sample =
              scene.world_route.samples[sample_index - 1U];
          const auto dx = sample.position[0U] - previous_sample.position[0U];
          const auto dz = sample.position[1U] - previous_sample.position[1U];
          const auto horizontal_distance = std::sqrt(dx * dx + dz * dz);
          // The file stores X/Z positions but a normalized 3D tangent.  Use
          // its Y component to carry the selected road deck between nearby
          // samples and groups.  Large X/Z gaps begin a separate route run.
          constexpr float maximum_continuous_step = 12.0F;
          if (horizontal_distance <= maximum_continuous_step) {
            const auto average_direction_y =
                (previous_sample.direction[1U] + sample.direction[1U]) * 0.5F;
            const auto average_horizontal_direction = std::max(
                0.05F,
                (std::hypot(previous_sample.direction[0U],
                            previous_sample.direction[2U]) +
                 std::hypot(sample.direction[0U], sample.direction[2U])) *
                    0.5F);
            predicted_route_height = *previous_route_height +
                                     horizontal_distance * average_direction_y /
                                         average_horizontal_direction;
          } else {
            previous_route_height.reset();
          }
        }
        float best_distance_squared = std::numeric_limits<float>::infinity();
        float nearest_height = 0.0F;
        std::vector<float> surface_heights;
        const auto consider_triangle = [&](const std::uint32_t first_index,
                                           const std::uint32_t second_index,
                                           const std::uint32_t third_index) {
          const auto &first = world.positions[first_index];
          const auto &second = world.positions[second_index];
          const auto &third = world.positions[third_index];
          const auto denominator =
              (second[2U] - third[2U]) * (first[0U] - third[0U]) +
              (third[0U] - second[0U]) * (first[2U] - third[2U]);
          if (std::abs(denominator) < 0.000001F) {
            return;
          }
          const auto first_weight =
              ((second[2U] - third[2U]) * (sample.position[0U] - third[0U]) +
               (third[0U] - second[0U]) * (sample.position[1U] - third[2U])) /
              denominator;
          const auto second_weight =
              ((third[2U] - first[2U]) * (sample.position[0U] - third[0U]) +
               (first[0U] - third[0U]) * (sample.position[1U] - third[2U])) /
              denominator;
          const auto third_weight = 1.0F - first_weight - second_weight;
          constexpr float edge_tolerance = -0.0001F;
          if (first_weight < edge_tolerance || second_weight < edge_tolerance ||
              third_weight < edge_tolerance) {
            return;
          }
          const auto candidate_height = first_weight * first[1U] +
                                        second_weight * second[1U] +
                                        third_weight * third[1U];
          surface_heights.push_back(candidate_height);
        };
        const auto consider_object = [&](const std::uint32_t object_index) {
          if (object_index >= world.objects.size()) {
            return;
          }
          const auto &object = world.objects[object_index];
          for (std::uint32_t primitive_index = object.first_primitive;
               primitive_index <
               object.first_primitive + object.primitive_count;
               ++primitive_index) {
            const auto &primitive = world.primitives[primitive_index];
            if (primitive.vertex_count >= 3U) {
              consider_triangle(primitive.position_indices[0U],
                                primitive.position_indices[1U],
                                primitive.position_indices[2U]);
            }
            if (primitive.vertex_count == 4U) {
              consider_triangle(primitive.position_indices[0U],
                                primitive.position_indices[2U],
                                primitive.position_indices[3U]);
            }
          }
          for (std::uint32_t position_index = object.first_position;
               position_index < object.first_position + object.position_count;
               ++position_index) {
            const auto &position = world.positions[position_index];
            const auto dx = position[0U] - sample.position[0U];
            const auto dz = position[2U] - sample.position[1U];
            const auto distance_squared = dx * dx + dz * dz;
            if (distance_squared < best_distance_squared) {
              best_distance_squared = distance_squared;
              nearest_height = position[1U];
            }
          }
        };
        const auto column = static_cast<std::int64_t>(std::floor(
            (static_cast<double>(sample.position[0U]) - world.minimum[0U]) /
            world.grid_cell_size_x));
        const auto row = static_cast<std::int64_t>(std::floor(
            (static_cast<double>(sample.position[1U]) - world.minimum[2U]) /
            world.grid_cell_size_z));
        if (column >= 0 && row >= 0 &&
            column < static_cast<std::int64_t>(world.grid_width) &&
            row < static_cast<std::int64_t>(world.grid_height)) {
          constexpr std::int64_t search_radius = 2;
          std::set<std::uint32_t> considered_objects;
          for (auto dz = -search_radius; dz <= search_radius; ++dz) {
            for (auto dx = -search_radius; dx <= search_radius; ++dx) {
              const auto candidate_column = column + dx;
              const auto candidate_row = row + dz;
              if (candidate_column < 0 || candidate_row < 0 ||
                  candidate_column >=
                      static_cast<std::int64_t>(world.grid_width) ||
                  candidate_row >=
                      static_cast<std::int64_t>(world.grid_height)) {
                continue;
              }
              const auto cell =
                  static_cast<std::size_t>(candidate_row) * world.grid_width +
                  static_cast<std::size_t>(candidate_column);
              const auto object_index = world.grid_cells[cell].object_index;
              if (object_index != std::numeric_limits<std::uint32_t>::max() &&
                  considered_objects.insert(object_index).second) {
                consider_object(object_index);
              }
            }
          }
        }
        if (!std::isfinite(best_distance_squared)) {
          for (std::uint32_t object_index = 0U;
               object_index < world.objects.size(); ++object_index) {
            consider_object(object_index);
          }
        }
        auto route_height = predicted_route_height.value_or(nearest_height);
        if (!surface_heights.empty()) {
          const auto target_height =
              predicted_route_height.value_or(nearest_height);
          const auto best = std::min_element(
              surface_heights.begin(), surface_heights.end(),
              [target_height](const float left, const float right) {
                return std::abs(left - target_height) <
                       std::abs(right - target_height);
              });
          // A candidate several metres from the tangent prediction is almost
          // certainly another bridge/deck.  Keep the continuous route there;
          // a later matching surface will anchor it again.
          constexpr float maximum_surface_correction = 0.75F;
          if (!predicted_route_height.has_value() ||
              std::abs(*best - target_height) <= maximum_surface_correction) {
            route_height = *best;
          }
        }
        previous_route_height = route_height;
        scene.world_route_positions.push_back(Vec3{
            sample.position[0U], route_height + 0.08F, sample.position[1U]});
      }
    } else if (options.route_overlay) {
      throw std::runtime_error(
          "--route requires an AI/ai.dat beside the track");
    }
    if (options.route_group.has_value() &&
        (*options.route_group >= scene.world_route.groups.size() ||
         scene.world_route.groups[*options.route_group].observed_sample_count ==
             0U)) {
      throw std::runtime_error(
          "--route-group must select a nonempty AI route group");
    }
    if (options.route_sample.has_value() &&
        *options.route_sample >= scene.world_route.samples.size()) {
      throw std::runtime_error(
          "--route-sample exceeds the AI route sample table");
    }
    if (options.local_cell.has_value() &&
        (*options.local_cell >= scene.world_grid_cells.size() ||
         scene.world_grid_cells[*options.local_cell].object_index ==
             std::numeric_limits<std::uint32_t>::max())) {
      throw std::runtime_error(
          "--local-cell must select an occupied MYW grid cell");
    }
    scene.world_scene = true;
  }

  bool assembled = false;
  const auto selected_model = ascii_lower(
      options.model_relative_path.lexically_normal().generic_string());
  std::vector<char> candidate_lods;
  if (const auto inferred_lod = infer_lod(options.model_relative_path)) {
    candidate_lods.push_back(*inferred_lod);
  } else {
    candidate_lods = {'0', '1', '2', '3'};
  }
  for (const auto lod : candidate_lods) {
    const auto &sources = discover_vehicle_sources(options, lod);
    const auto source = std::find_if(
        sources.begin(), sources.end(),
        [&selected_model, &options](const auto &candidate) {
          return ascii_lower(candidate.model_relative_path.lexically_normal()
                                 .generic_string()) == selected_model &&
                 (!options.car_definition_relative_path ||
                  ascii_lower(candidate.definition_relative_path.generic_string()) ==
                      ascii_lower(options.car_definition_relative_path->lexically_normal().generic_string()));
        });
    if (source == sources.end()) {
      continue;
    }
    const auto &car = source->definition;
    const auto &body_relative = source->model_relative_path;
    scene.vehicle_name = car.name;
    scene.vehicle_roster_index =
        static_cast<std::size_t>(std::distance(sources.begin(), source));
    scene.vehicle_roster_count = sources.size();
    scene.vehicle_default_colors = car.colors;
    const auto body_index = add_component("body", body_relative, {});
    scene.components[body_index].texture_track =
        ascii_lower(content_relative_path(car.texture_path).generic_string());
    scene.lod = lod;
    const auto count_anchors = [](const auto &anchors) {
      return static_cast<std::size_t>(
          std::count_if(anchors.begin(), anchors.end(),
                        [](const auto &anchor) { return anchor.has_value(); }));
    };
    const auto authored_model =
        ascii_lower(content_relative_path(car.model_base).generic_string());
    const auto authored_s40 = authored_model == "cars/s40/s40" ||
                              authored_model == "cars/car99/car99" ||
                              (authored_model == "cars/car15/car15" &&
                               ascii_lower(car.name) == "volvo s40");
    auto anchor_wheels = car.wheels;
    if (authored_s40) {
      // The early S40 body has more eligible fender polygons than the retail
      // bodies. Seed recovery inside the actual axle regions so it selects the
      // three wheel-arch faces, then use the centers recovered from the mesh.
      anchor_wheels[0U].center = {-0.807F, 0.273F, 1.390F};
      anchor_wheels[1U].center = {0.807F, 0.273F, 1.390F};
      anchor_wheels[2U].center = {0.807F, 0.273F, -1.439F};
      anchor_wheels[3U].center = {-0.807F, 0.273F, -1.439F};
    }
    std::array<std::optional<mh::content::CarWheelAnchor>, 4U>
        selected_anchors{};
    // Car99 LOD0/1 wheel centers are fitted directly from the authored body.
    // Do not rerun polygon recovery when the result cannot affect placement.
    if (!authored_s40 || lod >= '2') {
      selected_anchors = mh::content::recover_embedded_wheel_anchors(
          scene.components[body_index].model, anchor_wheels);
    }
    const auto embedded_wheels =
        lod >= '2' && count_anchors(selected_anchors) >= 3U;
    if (!embedded_wheels) {
      constexpr std::array<std::string_view, 4U> corners{"FL", "FR", "BR",
                                                         "BL"};
      std::array<std::size_t, 4U> wheel_indices{};
      for (std::size_t index = 0U; index < car.wheels.size(); ++index) {
        const auto &wheel = car.wheels[index];
        wheel_indices[index] =
            add_component(std::string("wheel-") + std::string(corners[index]),
                          model_lod_path(wheel.model_base, lod),
                          Vec3{wheel.center.x, wheel.center.y, wheel.center.z});
        scene.components[wheel_indices[index]].texture_track =
            ascii_lower(content_relative_path(wheel.texture_path).generic_string());
      }

      if (authored_s40) {
        // Robust LOD0 circle fits use 11 front and eight rear wheel-opening
        // vertices. A least-squares lateral plane through those vertices,
        // minus the textured wheel half-width, locates the matching X centers.
        constexpr std::array<float, 2U> s40_axle_x{0.757720402F, 0.770104233F};
        constexpr std::array<float, 2U> s40_axle_y{0.248563099F, 0.220657897F};
        constexpr std::array<float, 2U> s40_axle_z{1.335512739F, -1.267001193F};
        for (std::size_t index = 0U; index < car.wheels.size(); ++index) {
          const auto axle = index < 2U ? 0U : 1U;
          scene.components[wheel_indices[index]].translation = Vec3{
              index == 0U || index == 3U ? -s40_axle_x[axle] : s40_axle_x[axle],
              s40_axle_y[axle], s40_axle_z[axle]};
        }
        scene.wheel_geometry = "separate-components";
        scene.wheel_anchor_source = "body-wheel-arch-fit";
      } else {
        const auto lod2 =
            lod == '2'
                ? scene.components[body_index].model
                : cached_model(options, model_lod_path(car.model_base, '2'));
        const auto lod3 =
            lod == '3'
                ? scene.components[body_index].model
                : cached_model(options, model_lod_path(car.model_base, '3'));
        const auto lod2_anchors =
            mh::content::recover_embedded_wheel_anchors(lod2, anchor_wheels);
        const auto lod3_anchors =
            mh::content::recover_embedded_wheel_anchors(lod3, anchor_wheels);
        const auto anchor_score = [&](const auto &anchors) {
          if (count_anchors(anchors) != car.wheels.size()) {
            return std::numeric_limits<double>::infinity();
          }
          double squared_error = 0.0;
          for (std::size_t index = 0U; index < car.wheels.size(); ++index) {
            const auto &component = scene.components[wheel_indices[index]];
            const auto &anchor = anchors[index]->outer_center;
            const auto surface =
                wheel_visual_surface(component.model, anchor.x < 0.0F);
            const auto local_center_x =
                (component.model.minimum[0] + component.model.maximum[0]) * 0.5;
            const auto local_center_z =
                (component.model.minimum[2] + component.model.maximum[2]) * 0.5;
            const auto inferred_center_x = static_cast<double>(anchor.x) -
                                           surface.outer_x + local_center_x;
            const auto inferred_center_z = static_cast<double>(anchor.z) -
                                           surface.center_z + local_center_z;
            const auto delta_x = inferred_center_x - car.wheels[index].center.x;
            const auto delta_z = inferred_center_z - car.wheels[index].center.z;
            squared_error += delta_x * delta_x + delta_z * delta_z;
          }
          return std::sqrt(squared_error / car.wheels.size());
        };
        const auto lod2_score = anchor_score(lod2_anchors);
        const auto lod3_score = anchor_score(lod3_anchors);
        // Prefer the nearer LOD2 evidence unless LOD3 materially reduces
        // the inferred wheel-center error.
        constexpr double anchor_selection_margin = 0.03;
        const auto use_lod3 =
            std::isfinite(lod3_score) &&
            (!std::isfinite(lod2_score) ||
             lod3_score + anchor_selection_margin < lod2_score);
        const auto &visual_anchors = use_lod3 ? lod3_anchors : lod2_anchors;
        const auto use_visual_anchors =
            count_anchors(visual_anchors) == car.wheels.size();
        for (std::size_t index = 0U; index < car.wheels.size(); ++index) {
          if (!use_visual_anchors) {
            // Standard suspension at rest: k*c*(c+0.1) supports a quarter
            // of the car's weight. Physics centres are not visual centres.
            if (car.physics && car.physics->spring_strength > 0.0F &&
                car.physics->spring_length > 0.0F) {
              const auto load = car.physics->weight * 9.81F / 4.0F;
              const auto compression = std::clamp(
                  (std::sqrt(0.01F + 4.0F * load /
                                           car.physics->spring_strength) - 0.1F) * 0.5F,
                  0.0F, 1.0F);
              for (const auto wheel_index : wheel_indices) {
                auto &translation = scene.components[wheel_index].translation;
                translation.x *= 1.25F;
                translation.y -= car.physics->spring_length * (1.0F - compression);
              }
            }
            break;
          }
          auto &component = scene.components[wheel_indices[index]];
          const auto &anchor = visual_anchors[index]->outer_center;
          const auto surface =
              wheel_visual_surface(component.model, anchor.x < 0.0F);
          component.translation =
              Vec3{anchor.x - surface.outer_x, anchor.y - surface.center_y,
                   anchor.z - surface.center_z};
        }
        scene.wheel_geometry = "separate-components";
        scene.wheel_anchor_source = use_visual_anchors
                                        ? (use_lod3 ? "body-lod3" : "body-lod2")
                               : "static-suspension";
      }
    } else {
      scene.wheel_geometry = "body-embedded";
      scene.wheel_anchor_source = "selected-body";
    }
    if (!car.collision_path.empty()) {
      scene.collision_relative_path = content_relative_path(car.collision_path);
      const auto collision = mh::content::read_col(
          checked_content_path(options, scene.collision_relative_path));
      scene.collision_polygons =
          mh::content::reconstruct_col_polygons(collision);
    }
    assembled = true;
    break;
  }
  if (!assembled && !scene.world_scene) {
    add_component("model", options.model_relative_path, {});
  }
  scene.assembled_car = assembled;
  if (scene.assembled_car) {
    const auto material_track =
        mh::content::read_track_definition(checked_content_path(
            options, options.track_definition_relative_path.value_or(
                         std::filesystem::path("Game") / "Track8.trk")));
    scene.vehicle_environment = load_vehicle_environment_textures(
        renderer, material_track, options.content_root);
    // Cars are inspected outside a race, so light them with one stable neutral
    // daylight rig rather than inheriting the time-of-day tint of the track
    // that supplied the authored environment maps.  This keeps every shading
    // mode comparable while preserving the original paint/material inputs.
    scene.vehicle_object_ambient = {0.84F, 0.86F, 0.90F};
    scene.vehicle_object_brightness = {1.05F, 1.05F, 1.05F};
    scene.vehicle_accelerated_brightness = {1.22F, 1.22F, 1.22F};
    scene.vehicle_directional_color = {255U, 247U, 230U};
    scene.vehicle_directional_intensity = 0.62F;
    scene.vehicle_directional_direction = {-0.36F, 0.82F, -0.44F};
  }

  bool first_position = true;
  for (const auto &component : scene.components) {
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
      const auto translation = axis == 0U   ? component.translation.x
                               : axis == 1U ? component.translation.y
                                            : component.translation.z;
      const auto minimum = component.model.minimum[axis] + translation;
      const auto maximum = component.model.maximum[axis] + translation;
      auto *scene_minimum = axis == 0U   ? &scene.minimum.x
                            : axis == 1U ? &scene.minimum.y
                                         : &scene.minimum.z;
      auto *scene_maximum = axis == 0U   ? &scene.maximum.x
                            : axis == 1U ? &scene.maximum.y
                                         : &scene.maximum.z;
      if (first_position) {
        *scene_minimum = minimum;
        *scene_maximum = maximum;
      } else {
        *scene_minimum = std::min(*scene_minimum, minimum);
        *scene_maximum = std::max(*scene_maximum, maximum);
      }
    }
    first_position = false;
  }
  std::set<std::string> required_materials;
  for (const auto &component : scene.components) {
    for (const auto &face : component.model.faces) {
      if (face.has_texture_coordinates &&
          face.material_name_index < component.model.names.size()) {
        required_materials.insert(
            ascii_lower(component.model.names[face.material_name_index]));
      }
    }
  }
  const auto &catalog = cached_texture_catalog(options, required_materials);
  std::map<std::string, std::filesystem::path> overrides;
  if (options.override_root.has_value()) {
    const auto resolved =
        mh::content::resolve_texture_overrides(catalog, *options.override_root);
    for (const auto &item : resolved.overrides) {
      overrides.emplace(item.logical_id, resolved.root / item.relative_path);
    }
  }
  std::map<std::string, std::size_t> loaded_materials;
  for (auto &component : scene.components) {
    component.material_indices.resize(component.model.names.size());
    for (std::size_t index = 0U; index < component.model.names.size();
         ++index) {
      const auto used =
          std::any_of(component.model.faces.begin(),
                      component.model.faces.end(), [index](const auto &face) {
                        return face.has_texture_coordinates &&
                               face.material_name_index == index;
                      });
      if (!used) {
        continue;
      }
      bool fallback = false;
      const auto &asset =
          select_texture_asset(catalog, component.model.names[index],
                               component.texture_track.empty() ? options.track
                                                               : component.texture_track,
                               options.pack, fallback, options.track);
      const auto existing = loaded_materials.find(asset.logical_id);
      if (existing != loaded_materials.end()) {
        component.material_indices[index] = existing->second;
        continue;
      }
      MaterialTexture material;
      material.material_name = ascii_lower(component.model.names[index]);
      material.logical_id = asset.logical_id;
      material.original_width = asset.width;
      material.original_height = asset.height;
      const auto override = overrides.find(asset.logical_id);
      material.image = cached_texture_image(options, catalog, asset);
      material.texture = upload_texture(renderer, material.image);
      if (override != overrides.end()) {
        material.override_image = mh::content::read_pam_rgba(override->second);
        material.override_texture =
            upload_texture(renderer, *material.override_image);
        material.override_used = true;
        ++scene.override_materials;
      }
      if (fallback) {
        ++scene.fallback_materials;
      }
      const auto material_index = scene.materials.size();
      loaded_materials.emplace(asset.logical_id, material_index);
      scene.materials.push_back(std::move(material));
      component.material_indices[index] = material_index;
    }
  }
  return scene;
}

Vec3 rotate(const Vec3 value, const float yaw, const float pitch) {
  const auto cy = std::cos(yaw);
  const auto sy = std::sin(yaw);
  const Vec3 around_y{cy * value.x + sy * value.z, value.y,
                      -sy * value.x + cy * value.z};
  const auto cx = std::cos(pitch);
  const auto sx = std::sin(pitch);
  return Vec3{around_y.x, cx * around_y.y - sx * around_y.z,
              sx * around_y.y + cx * around_y.z};
}

SDL_FPoint vehicle_environment_coordinate(Vec3 view_normal) {
  const auto length =
      std::sqrt(view_normal.x * view_normal.x + view_normal.y * view_normal.y +
                view_normal.z * view_normal.z);
  if (!std::isfinite(length) || length <= 1.0e-8F) {
    view_normal = Vec3{0.0F, 1.0F, 0.0F};
  } else {
    view_normal.x /= length;
    view_normal.y /= length;
    view_normal.z /= length;
  }
  constexpr float original_map_scale = 0.495F;
  return SDL_FPoint{
      std::clamp(0.5F + view_normal.x * original_map_scale, 0.0F, 1.0F),
      std::clamp(0.5F - view_normal.y * original_map_scale, 0.0F, 1.0F)};
}

std::array<float, 3U>
vehicle_lighting_factor(const Scene &scene, Vec3 normal,
                        const bool accelerated_brightness) {
  const auto length =
      std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
  if (!std::isfinite(length) || length <= 1.0e-8F) {
    normal = Vec3{0.0F, 1.0F, 0.0F};
  } else {
    normal.x /= length;
    normal.y /= length;
    normal.z /= length;
  }
  const auto diffuse = std::max(
      0.0F, normal.x * scene.vehicle_directional_direction[0U] +
                normal.y * scene.vehicle_directional_direction[1U] +
                normal.z * scene.vehicle_directional_direction[2U]);
  std::array<float, 3U> result{};
  for (std::size_t channel = 0U; channel < result.size(); ++channel) {
    const auto directional =
        diffuse * scene.vehicle_directional_intensity *
        (static_cast<float>(scene.vehicle_directional_color[channel]) /
         255.0F);
    result[channel] =
        (scene.vehicle_object_ambient[channel] + directional) *
        scene.vehicle_object_brightness[channel] *
        (accelerated_brightness
             ? scene.vehicle_accelerated_brightness[channel]
             : 1.0F);
  }
  return result;
}

std::pair<float, float> camera_angles(const std::string_view view) {
  constexpr float pi = 3.14159265358979323846F;
  if (view == "rear") {
    return {pi, full_track_pitch_radians};
  }
  if (view == "top") {
    return {0.0F, full_track_pitch_radians};
  }
  return {0.72F, full_track_pitch_radians};
}

std::size_t route_group_sample_index(const Scene &scene,
                                     const std::size_t group_index) {
  if (group_index >= scene.world_route.groups.size()) {
    throw std::runtime_error("AI route group exceeds the metadata table");
  }
  const auto &group = scene.world_route.groups[group_index];
  if (group.observed_sample_count == 0U ||
      group.first_sample == std::numeric_limits<std::uint32_t>::max()) {
    throw std::runtime_error("AI route group has no observed samples");
  }
  return static_cast<std::size_t>(group.first_sample) +
         group.observed_sample_count / 2U;
}

std::size_t adjacent_route_group(const Scene &scene, const std::size_t current,
                                 const bool previous) {
  if (scene.world_route.groups.empty()) {
    throw std::runtime_error("AI route has no groups");
  }
  auto candidate = current;
  for (std::size_t attempt = 0U; attempt < scene.world_route.groups.size();
       ++attempt) {
    candidate = previous ? candidate == 0U
                               ? scene.world_route.groups.size() - 1U
                               : candidate - 1U
                         : (candidate + 1U) % scene.world_route.groups.size();
    if (scene.world_route.groups[candidate].observed_sample_count != 0U) {
      return candidate;
    }
  }
  throw std::runtime_error("AI route has no nonempty groups");
}

std::pair<float, float> route_camera_angles(const Scene &scene,
                                            const std::size_t group_index,
                                            const bool chase = false) {
  const auto sample_index = route_group_sample_index(scene, group_index);
  const auto &direction = scene.world_route.samples[sample_index].direction;
  if (!chase) {
    return {std::atan2(-direction[0U], direction[2U]), -0.12F};
  }
  constexpr float pi = 3.14159265358979323846F;
  const auto horizontal = std::hypot(direction[0U], direction[2U]);
  return {std::atan2(-direction[0U], direction[2U]) + pi,
          -std::atan2(direction[1U], std::max(horizontal, 0.0001F))};
}

std::pair<float, float>
route_sample_camera_angles(const Scene &scene, const std::size_t sample_index,
                           const bool chase = false) {
  if (sample_index >= scene.world_route.samples.size()) {
    throw std::runtime_error("AI route camera sample exceeds the sample table");
  }
  const auto group_index = static_cast<std::size_t>(
      scene.world_route.samples[sample_index].group_index);
  if (chase) {
    const auto &direction = scene.world_route.samples[sample_index].direction;
    constexpr float pi = 3.14159265358979323846F;
    const auto horizontal = std::hypot(direction[0U], direction[2U]);
    return {std::atan2(-direction[0U], direction[2U]) + pi,
            -std::atan2(direction[1U], std::max(horizontal, 0.0001F))};
  }
  return route_camera_angles(scene, group_index);
}

std::size_t adjacent_route_sample(const Scene &scene, const std::size_t current,
                                  const bool previous) {
  if (scene.world_route.samples.empty()) {
    throw std::runtime_error("AI route has no samples");
  }
  return previous ? current == 0U ? scene.world_route.samples.size() - 1U
                                  : current - 1U
                  : (current + 1U) % scene.world_route.samples.size();
}

ViewVolume make_view_volume(const Scene &scene,
                            const std::optional<std::size_t> local_cell,
                            const std::optional<std::size_t> route_group,
                            const std::optional<std::size_t> route_sample,
                            const bool route_chase) {
  ViewVolume view;
  view.center = Vec3{(scene.minimum.x + scene.maximum.x) * 0.5F,
                     (scene.minimum.y + scene.maximum.y) * 0.5F,
                     (scene.minimum.z + scene.maximum.z) * 0.5F};
  view.radius = std::max({scene.maximum.x - scene.minimum.x,
                          scene.maximum.y - scene.minimum.y,
                          scene.maximum.z - scene.minimum.z, 0.001F}) *
                0.5F;
  if (route_group.has_value() || route_sample.has_value()) {
    const auto sample_index =
        route_sample.has_value()
            ? *route_sample
            : route_group_sample_index(scene, *route_group);
    if (sample_index >= scene.world_route_positions.size()) {
      throw std::runtime_error("AI route focus sample has no viewer position");
    }
    view.center = scene.world_route_positions[sample_index];
    if (route_chase) {
      const auto &direction = scene.world_route.samples[sample_index].direction;
      constexpr float look_ahead = 10.0F;
      constexpr float camera_height = 2.2F;
      view.center.x += direction[0U] * look_ahead;
      view.center.y += camera_height + direction[1U] * look_ahead;
      view.center.z += direction[2U] * look_ahead;
      view.camera_distance = 14.0F;
      view.clip_near = true;
    }
    view.radius = std::max(
        {static_cast<float>(scene.world_grid_cell_size_x * 3.5),
         static_cast<float>(scene.world_grid_cell_size_z * 3.5), 48.0F});
    view.visible_world_objects.assign(scene.world_objects.size(), false);
    const auto column = static_cast<std::int32_t>(
        std::floor((static_cast<double>(view.center.x) - scene.minimum.x) /
                   scene.world_grid_cell_size_x));
    const auto row = static_cast<std::int32_t>(
        std::floor((static_cast<double>(view.center.z) - scene.minimum.z) /
                   scene.world_grid_cell_size_z));
    constexpr std::int32_t neighborhood = 4;
    for (auto candidate_row = std::max(0, row - neighborhood);
         candidate_row <=
         std::min(static_cast<std::int32_t>(scene.world_grid_height) - 1,
                  row + neighborhood);
         ++candidate_row) {
      for (auto candidate_column = std::max(0, column - neighborhood);
           candidate_column <=
           std::min(static_cast<std::int32_t>(scene.world_grid_width) - 1,
                    column + neighborhood);
           ++candidate_column) {
        const auto cell =
            static_cast<std::size_t>(candidate_row) * scene.world_grid_width +
            static_cast<std::size_t>(candidate_column);
        const auto object_index = scene.world_grid_cells[cell].object_index;
        if (object_index != std::numeric_limits<std::uint32_t>::max() &&
            object_index < view.visible_world_objects.size()) {
          view.visible_world_objects[object_index] = true;
        }
      }
    }
    for (std::size_t index = 0U; index < scene.world_objects.size(); ++index) {
      const auto &sphere = scene.world_objects[index].bounding_sphere;
      const auto dx = sphere[0U] - view.center.x;
      const auto dz = sphere[2U] - view.center.z;
      const auto limit = view.radius * 1.5F + sphere[3U];
      if (dx * dx + dz * dz <= limit * limit) {
        view.visible_world_objects[index] = true;
      }
    }
    return view;
  }
  if (!local_cell.has_value()) {
    view.fit_full_track = scene.world_scene;
    view.full_track_width =
        std::max(scene.maximum.x - scene.minimum.x, 0.001F);
    view.full_track_height =
        std::max(scene.maximum.y - scene.minimum.y, 0.001F);
    view.full_track_depth =
        std::max(scene.maximum.z - scene.minimum.z, 0.001F);
    return view;
  }
  if (*local_cell >= scene.world_grid_cells.size()) {
    throw std::runtime_error("local MYW cell exceeds the grid");
  }
  const auto object_index = scene.world_grid_cells[*local_cell].object_index;
  if (object_index == std::numeric_limits<std::uint32_t>::max() ||
      object_index >= scene.world_objects.size()) {
    throw std::runtime_error("local MYW cell is not occupied");
  }
  const auto &anchor = scene.world_objects[object_index].bounding_sphere;
  view.center = Vec3{anchor[0], anchor[1], anchor[2]};
  view.radius =
      std::max({static_cast<float>(scene.world_grid_cell_size_x * 4.0),
                static_cast<float>(scene.world_grid_cell_size_z * 4.0),
                anchor[3] * 2.0F, 20.0F});
  view.visible_world_objects.assign(scene.world_objects.size(), false);
  constexpr std::int32_t neighborhood = 4;
  const auto selected_column =
      static_cast<std::int32_t>(*local_cell % scene.world_grid_width);
  const auto selected_row =
      static_cast<std::int32_t>(*local_cell / scene.world_grid_width);
  for (auto row = std::max(0, selected_row - neighborhood);
       row <= std::min(static_cast<std::int32_t>(scene.world_grid_height) - 1,
                       selected_row + neighborhood);
       ++row) {
    for (auto column = std::max(0, selected_column - neighborhood);
         column <=
         std::min(static_cast<std::int32_t>(scene.world_grid_width) - 1,
                  selected_column + neighborhood);
         ++column) {
      const auto cell_index =
          static_cast<std::size_t>(row) * scene.world_grid_width +
          static_cast<std::size_t>(column);
      const auto nearby_object =
          scene.world_grid_cells[cell_index].object_index;
      if (nearby_object != std::numeric_limits<std::uint32_t>::max() &&
          nearby_object < view.visible_world_objects.size()) {
        view.visible_world_objects[nearby_object] = true;
      }
    }
  }
  for (std::size_t index = 0U; index < scene.world_objects.size(); ++index) {
    const auto &sphere = scene.world_objects[index].bounding_sphere;
    const auto dx = sphere[0] - view.center.x;
    const auto dz = sphere[2] - view.center.z;
    const auto limit = view.radius * 1.5F + sphere[3];
    if (dx * dx + dz * dz <= limit * limit) {
      view.visible_world_objects[index] = true;
    }
  }
  return view;
}

float full_track_fitted_scale(const ViewVolume &view, const int width,
                              const int height, const float zoom) {
  const auto fitted_width = std::max(
      1.0F, static_cast<float>(width) - viewer_content_left -
                viewer_content_right_margin);
  const auto fitted_height = std::max(
      1.0F, static_cast<float>(height) - viewer_viewport_top -
                viewer_footer_height - viewer_viewport_bottom_gap);
  const auto rotated_width =
      std::hypot(view.full_track_width, view.full_track_depth);
  const auto rotated_height =
      view.full_track_height * full_track_pitch_cosine +
      rotated_width * full_track_pitch_sine;
  return std::min(fitted_width / std::max(rotated_width, 0.001F),
                  fitted_height / std::max(rotated_height, 0.001F)) *
         0.94F * zoom;
}

std::vector<ProjectedVertex>
project_positions(const ViewVolume &view, const ModelComponent &component,
                  const int width, const int height, const float yaw,
                  const float pitch, const float zoom, const float pan_x,
                  const float pan_y) {
  const auto center = view.center;
  const auto radius = view.radius;
  const auto camera =
      view.camera_distance > 0.0F ? view.camera_distance : radius * 3.4F;
  const auto focal = static_cast<float>(std::min(width, height)) * 1.08F * zoom;
  const auto fitted_world = view.fit_full_track;
  constexpr float fitted_top_margin = viewer_viewport_top;
  constexpr float fitted_bottom_margin =
      viewer_footer_height + viewer_viewport_bottom_gap;
  const auto fitted_width = std::max(
      1.0F, static_cast<float>(width) - viewer_content_left -
                viewer_content_right_margin);
  const auto fitted_height = std::max(
      1.0F, static_cast<float>(height) - fitted_top_margin -
                fitted_bottom_margin);
  // Keep the fit independent of live camera angles. Re-evaluating it while
  // orbiting made the whole world visibly breathe/stretch.
  const auto fitted_scale =
      full_track_fitted_scale(view, width, height, zoom);
  const auto fitted_center_y =
      fitted_top_margin + fitted_height * 0.5F;
  const auto viewport_center_x = viewer_content_left + fitted_width * 0.5F;
  std::vector<ProjectedVertex> output;
  output.reserve(component.model.positions.size());
  for (const auto &position : component.model.positions) {
    const auto transformed =
        rotate(Vec3{position[0] + component.translation.x - center.x,
                    position[1] + component.translation.y - center.y,
                    position[2] + component.translation.z - center.z},
               yaw, pitch);
    const auto raw_depth = camera - transformed.z;
    const auto near_depth = view.clip_near ? 0.25F : radius * 0.05F;
    const auto depth = std::max(near_depth, raw_depth);
    output.push_back(ProjectedVertex{
        fitted_world ? viewport_center_x + pan_x -
                           transformed.x * fitted_scale
                   : viewport_center_x + pan_x -
                         transformed.x * focal / depth,
        fitted_world ? fitted_center_y + pan_y - transformed.y * fitted_scale
                   : static_cast<float>(height) * 0.46F + pan_y -
                         transformed.y * focal / depth,
        depth, !view.clip_near || raw_depth > near_depth});
  }
  return output;
}

ProjectedVertex project_world_position(const ViewVolume &view,
                                       const Vec3 position, const int width,
                                       const int height, const float yaw,
                                       const float pitch, const float zoom,
                                       const float pan_x, const float pan_y) {
  const auto center = view.center;
  const auto radius = view.radius;
  const auto transformed = rotate(
      Vec3{position.x - center.x, position.y - center.y, position.z - center.z},
      yaw, pitch);
  const auto camera =
      view.camera_distance > 0.0F ? view.camera_distance : radius * 3.4F;
  const auto raw_depth = camera - transformed.z;
  const auto near_depth = view.clip_near ? 0.25F : radius * 0.05F;
  const auto depth = std::max(near_depth, raw_depth);
  const auto focal = static_cast<float>(std::min(width, height)) * 1.08F * zoom;
  const auto fitted_world = view.fit_full_track;
  constexpr float fitted_top_margin = viewer_viewport_top;
  constexpr float fitted_bottom_margin =
      viewer_footer_height + viewer_viewport_bottom_gap;
  const auto fitted_width = std::max(
      1.0F, static_cast<float>(width) - viewer_content_left -
                viewer_content_right_margin);
  const auto fitted_height = std::max(
      1.0F, static_cast<float>(height) - fitted_top_margin -
                fitted_bottom_margin);
  const auto fitted_scale =
      full_track_fitted_scale(view, width, height, zoom);
  const auto fitted_center_y =
      fitted_top_margin + fitted_height * 0.5F;
  const auto viewport_center_x = viewer_content_left + fitted_width * 0.5F;
  return ProjectedVertex{
      fitted_world ? viewport_center_x + pan_x -
                         transformed.x * fitted_scale
                 : viewport_center_x + pan_x -
                       transformed.x * focal / depth,
      fitted_world ? fitted_center_y + pan_y - transformed.y * fitted_scale
                 : static_cast<float>(height) * 0.46F + pan_y -
                       transformed.y * focal / depth,
      depth, !view.clip_near || raw_depth > near_depth};
}

VertexLighting shade_world_vertex(const Scene &scene, const Vec3 position,
                                  const Vec3) {
  // MYW vertex colours already contain the authored static illumination.
  // Lit mode adds nearby dynamic point-light energy without dimming that base.
  constexpr float ambient = 1.0F;
  constexpr float minimum_original_range = 0.1F;
  constexpr float original_light_scale = 1.75F;
  constexpr float original_negative_light_scale = 0.25F;
  VertexLighting result{{ambient, ambient, ambient}, 0U};
  const std::vector<std::uint32_t> *light_indices = nullptr;
  const auto has_grid =
      scene.world_grid_width != 0U && scene.world_grid_height != 0U &&
      scene.world_grid_cell_size_x > 0.0 && scene.world_grid_cell_size_z > 0.0;
  if (has_grid) {
    const auto column = static_cast<std::int64_t>(
        std::floor((static_cast<double>(position.x) - scene.minimum.x) /
                   scene.world_grid_cell_size_x));
    const auto row = static_cast<std::int64_t>(
        std::floor((static_cast<double>(position.z) - scene.minimum.z) /
                   scene.world_grid_cell_size_z));
    if (column >= 0 && row >= 0 &&
        column < static_cast<std::int64_t>(scene.world_grid_width) &&
        row < static_cast<std::int64_t>(scene.world_grid_height)) {
      const auto cell = static_cast<std::size_t>(row) * scene.world_grid_width +
                        static_cast<std::size_t>(column);
      if (cell < scene.world_grid_cells.size()) {
        light_indices = &scene.world_grid_cells[cell].light_indices;
      }
    }
    if (light_indices == nullptr) {
      return result;
    }
  }
  const auto evaluate_light = [&](const mh::content::MywPointLight &light) {
    if (light.range <= minimum_original_range) {
      return;
    }
    const Vec3 to_light{light.position[0] - position.x,
                        light.position[1] - position.y,
                        light.position[2] - position.z};
    const auto distance_squared = to_light.x * to_light.x +
                                  to_light.y * to_light.y +
                                  to_light.z * to_light.z;
    const auto range_squared = light.range * light.range;
    if (distance_squared >= range_squared) {
      return;
    }
    const auto distance = std::sqrt(distance_squared);
    const auto range_falloff = 1.0F - distance / light.range;
    auto intensity =
        light.color[3] * range_falloff * range_falloff * original_light_scale;
    if (intensity < 0.0F) {
      intensity *= original_negative_light_scale;
    }
    if (intensity == 0.0F) {
      return;
    }
    for (std::size_t channel = 0U; channel < result.color.size(); ++channel) {
      result.color[channel] +=
          std::clamp(light.color[channel], 0.0F, 1.0F) * intensity;
    }
    ++result.contributions;
  };
  if (has_grid) {
    for (const auto index : *light_indices) {
      if (index < scene.world_lights.size()) {
        evaluate_light(scene.world_lights[index]);
      }
    }
  } else {
    for (const auto &light : scene.world_lights) {
      evaluate_light(light);
    }
  }
  for (auto &channel : result.color) {
    channel = std::clamp(channel, 0.0F, 1.0F);
  }
  return result;
}

RenderStats render_scene(
    SDL_Renderer *renderer, const ViewerFont &title_font,
    const ViewerFont &pause_font, UiTextCache &ui_text_cache,
    const Scene &scene, const int width,
    const int height, const float yaw, const float pitch, const bool wireframe,
    const bool normals, const bool textures, const bool collision_overlay,
    const bool lighting_overlay, const bool applied_lighting,
    const bool route_overlay, const bool hd_enabled, const bool depth_enabled,
    const CarShading car_shading, DepthRenderResources &depth_resources,
    const float zoom, const float pan_x, const float pan_y,
    const std::optional<std::size_t> local_cell = std::nullopt,
    const std::optional<std::size_t> route_group = std::nullopt,
    const std::optional<std::size_t> route_sample = std::nullopt,
    const bool route_playback = false, const bool route_chase = false,
    const bool show_help = false,
    const bool shading_dropdown_open = false) {
  require(SDL_SetRenderDrawColor(renderer, 9U, 13U, 22U, 255U),
          "set viewer clear color");
  require(SDL_RenderClear(renderer), "clear model viewer");
  require(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND),
          "enable viewer interface blending");
  // A restrained blue-black backdrop gives the model enough separation from
  // the viewport without introducing textures or another asset dependency.
  constexpr int backdrop_bands = 48;
  for (auto band = 0; band < backdrop_bands; ++band) {
    const auto y0 = static_cast<float>(height) *
                    static_cast<float>(band) / backdrop_bands;
    const auto y1 = static_cast<float>(height) *
                    static_cast<float>(band + 1) / backdrop_bands;
    const auto normalized =
        (static_cast<float>(band) + 0.5F) / backdrop_bands;
    const auto center_weight =
        std::max(0.0F, 1.0F - std::abs(normalized - 0.52F) * 2.0F);
    const auto red = static_cast<std::uint8_t>(7.0F + center_weight * 3.0F);
    const auto green =
        static_cast<std::uint8_t>(11.0F + center_weight * 7.0F);
    const auto blue =
        static_cast<std::uint8_t>(20.0F + center_weight * 15.0F);
    require(SDL_SetRenderDrawColor(renderer, red, green, blue, 255U),
            "set viewer backdrop band color");
    const SDL_FRect backdrop_band{0.0F, y0, static_cast<float>(width),
                                  std::max(1.0F, y1 - y0 + 1.0F)};
    require(SDL_RenderFillRect(renderer, &backdrop_band),
            "render viewer backdrop band");
  }
  if (!scene.world_scene) {
    const auto floor_y = static_cast<float>(height) * 0.68F;
    const auto floor_bottom =
        static_cast<float>(height) - viewer_footer_height - 1.0F;
    constexpr int floor_bands = 28;
    for (auto band = 0; band < floor_bands; ++band) {
      const auto ratio = static_cast<float>(band) / floor_bands;
      const auto y0 = floor_y + (floor_bottom - floor_y) * ratio;
      const auto y1 = floor_y +
                      (floor_bottom - floor_y) *
                          static_cast<float>(band + 1) / floor_bands;
      const auto lift = static_cast<std::uint8_t>((1.0F - ratio) * 8.0F);
      require(SDL_SetRenderDrawColor(renderer, 9U + lift, 13U + lift,
                                     20U + lift, 255U),
              "set viewer floor band color");
      const SDL_FRect floor_band{0.0F, y0, static_cast<float>(width),
                                 std::max(1.0F, y1 - y0 + 1.0F)};
      require(SDL_RenderFillRect(renderer, &floor_band),
              "render viewer floor band");
    }
    constexpr int shadow_rows = 42;
    for (auto row = 0; row < shadow_rows; ++row) {
      const auto normalized =
          (static_cast<float>(row) + 0.5F) / shadow_rows * 2.0F - 1.0F;
      const auto half_width =
          static_cast<float>(width) * 0.24F *
          std::sqrt(std::max(0.0F, 1.0F - normalized * normalized));
      const auto alpha = 0.28F * (1.0F - std::abs(normalized));
      require(SDL_SetRenderDrawColorFloat(renderer, 0.0F, 0.0F, 0.0F, alpha),
              "set viewer vehicle shadow color");
      require(SDL_RenderLine(renderer,
                             static_cast<float>(width) * 0.52F - half_width,
                             floor_y - 7.0F + row,
                             static_cast<float>(width) * 0.52F + half_width,
                             floor_y - 7.0F + row),
              "render viewer vehicle shadow");
    }
    const auto draw_floor_glow = [&](const float center_x,
                                     const float center_y,
                                     const float glow_width,
                                     const float glow_height) {
      const auto rows = static_cast<int>(glow_height * 2.0F);
      for (auto row = 0; row < rows; ++row) {
        const auto normalized =
            (static_cast<float>(row) + 0.5F) / rows * 2.0F - 1.0F;
        const auto half_width =
            glow_width *
            std::sqrt(std::max(0.0F, 1.0F - normalized * normalized));
        const auto alpha = 0.022F *
                           std::pow(std::max(0.0F, 1.0F -
                                                      std::abs(normalized)),
                                    2.0F);
        require(SDL_SetRenderDrawColorFloat(renderer, 1.0F, 0.22F, 0.02F,
                                            alpha),
                "set viewer floor glow color");
        require(SDL_RenderLine(renderer, center_x - half_width,
                               center_y - glow_height + row,
                               center_x + half_width,
                               center_y - glow_height + row),
                "render viewer floor glow");
      }
    };
    draw_floor_glow(static_cast<float>(width) * 0.37F, floor_y + 72.0F,
                    static_cast<float>(width) * 0.055F, 68.0F);
    draw_floor_glow(static_cast<float>(width) * 0.46F, floor_y + 66.0F,
                    static_cast<float>(width) * 0.045F, 56.0F);
  }
  const auto view = make_view_volume(scene, local_cell, route_group,
                                     route_sample, route_chase);
  RenderStats stats;
  if (!view.visible_world_objects.empty()) {
    stats.submitted_world_objects = static_cast<std::uint64_t>(
        std::count(view.visible_world_objects.begin(),
                   view.visible_world_objects.end(), true));
  } else {
    stats.submitted_world_objects = scene.world_objects.size();
  }
  std::vector<std::vector<ProjectedVertex>> projected;
  projected.reserve(scene.components.size());
  std::vector<ProjectedFace> faces;
  for (std::size_t component_index = 0U;
       component_index < scene.components.size(); ++component_index) {
    const auto &component = scene.components[component_index];
    projected.push_back(project_positions(view, component, width, height, yaw,
                                          pitch, zoom, pan_x, pan_y));
    faces.reserve(faces.size() + component.model.faces.size());
    for (std::size_t index = 0U; index < component.model.faces.size();
         ++index) {
      if (!view.visible_world_objects.empty() &&
          index < component.face_object_indices.size()) {
        const auto object_index = component.face_object_indices[index];
        if (object_index >= view.visible_world_objects.size() ||
            !view.visible_world_objects[object_index]) {
          continue;
        }
      }
      const auto &face = component.model.faces[index];
      float depth = 0.0F;
      bool face_visible = true;
      for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
        const auto &projected_vertex =
            projected.back()[face.position_indices[vertex]];
        depth += projected_vertex.depth;
        face_visible = face_visible && projected_vertex.visible;
      }
      if (!face_visible) {
        continue;
      }
      faces.push_back(
          ProjectedFace{component_index, index, depth / face.vertex_count});
    }
  }
  stats.submitted_faces = faces.size();
  std::sort(faces.begin(), faces.end(),
            [](const auto &left, const auto &right) {
              return left.depth > right.depth;
            });
  constexpr std::int64_t playback_depth_numerator = 3;
  constexpr std::int64_t playback_depth_denominator = 4;
  const auto playback_depth_extent = [](const int extent) {
    return std::max(1, static_cast<int>((static_cast<std::int64_t>(extent) *
                                             playback_depth_numerator +
                                         playback_depth_denominator - 1) /
                                        playback_depth_denominator));
  };
  const auto depth_width =
      depth_enabled && route_playback ? playback_depth_extent(width) : width;
  const auto depth_height =
      depth_enabled && route_playback ? playback_depth_extent(height) : height;
  if (depth_enabled) {
    if (depth_width <= 0 || depth_height <= 0 ||
        static_cast<std::uint64_t>(depth_width) * depth_height >
            std::numeric_limits<std::size_t>::max() / 4U) {
      throw std::runtime_error(
          "depth-buffer dimensions exceed the platform range");
    }
    stats.depth_width = depth_width;
    stats.depth_height = depth_height;
    const auto pixels = static_cast<std::size_t>(depth_width) *
                        static_cast<std::size_t>(depth_height);
    depth_resources.color.resize(pixels * 4U);
    depth_resources.values.resize(pixels);
    std::fill(depth_resources.values.begin(), depth_resources.values.end(),
              std::numeric_limits<float>::infinity());
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel) {
      depth_resources.color[pixel * 4U] = 9U;
      depth_resources.color[pixel * 4U + 1U] = 13U;
      depth_resources.color[pixel * 4U + 2U] = 22U;
      depth_resources.color[pixel * 4U + 3U] = 255U;
    }
    if (depth_resources.texture == nullptr ||
        depth_resources.width != depth_width ||
        depth_resources.height != depth_height) {
      depth_resources.texture.reset(SDL_CreateTexture(
          renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
          depth_width, depth_height));
      require(depth_resources.texture != nullptr,
              "create depth-buffer texture");
      require(SDL_SetTextureScaleMode(depth_resources.texture.get(),
                                      SDL_SCALEMODE_LINEAR),
              "set depth-buffer scaling");
      depth_resources.width = depth_width;
      depth_resources.height = depth_height;
    }
  }
  constexpr std::array<int, 6U> triangles{0, 1, 2, 0, 2, 3};
  const auto depth_scale_x =
      static_cast<float>(depth_width) / static_cast<float>(width);
  const auto depth_scale_y =
      static_cast<float>(depth_height) / static_cast<float>(height);
  const auto rasterize_triangle = [&](const std::array<SDL_Vertex, 4U>
                                          &vertices,
                                      const std::size_t component_index,
                                      const mh::content::MyoFace &face,
                                      const std::array<std::size_t, 3U>
                                          &corners,
                                      const mh::content::PamRgbaImage *image,
                                      const bool additive = false,
                                      const float opacity = 1.0F,
                                      const float depth_bias = 1.0F) {
    const auto &first = vertices[corners[0U]];
    const auto &second = vertices[corners[1U]];
    const auto &third = vertices[corners[2U]];
    const auto edge = [](const SDL_FPoint &a, const SDL_FPoint &b,
                         const float x, const float y) {
      return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
    };
    const SDL_FPoint first_position{first.position.x * depth_scale_x,
                                    first.position.y * depth_scale_y};
    const SDL_FPoint second_position{second.position.x * depth_scale_x,
                                     second.position.y * depth_scale_y};
    const SDL_FPoint third_position{third.position.x * depth_scale_x,
                                    third.position.y * depth_scale_y};
    const auto area = edge(first_position, second_position, third_position.x,
                           third_position.y);
    if (!std::isfinite(area) || std::abs(area) < 0.000001F) {
      return;
    }
    const auto minimum_x = std::max(
        0, static_cast<int>(std::floor(std::min(
               {first_position.x, second_position.x, third_position.x}))));
    const auto maximum_x = std::min(
        depth_width - 1,
        static_cast<int>(std::ceil(std::max(
            {first_position.x, second_position.x, third_position.x}))));
    const auto minimum_y = std::max(
        0, static_cast<int>(std::floor(std::min(
               {first_position.y, second_position.y, third_position.y}))));
    const auto maximum_y = std::min(
        depth_height - 1,
        static_cast<int>(std::ceil(std::max(
            {first_position.y, second_position.y, third_position.y}))));
    if (minimum_x > maximum_x || minimum_y > maximum_y) {
      return;
    }
    const std::array<float, 3U> inverse_depth{
        1.0F / projected[component_index][face.position_indices[corners[0U]]]
                   .depth,
        1.0F / projected[component_index][face.position_indices[corners[1U]]]
                   .depth,
        1.0F / projected[component_index][face.position_indices[corners[2U]]]
                   .depth};
    const auto wrap = [](const int value, const int extent) {
      const auto remainder = value % extent;
      return remainder < 0 ? remainder + extent : remainder;
    };
    for (auto y = minimum_y; y <= maximum_y; ++y) {
      for (auto x = minimum_x; x <= maximum_x; ++x) {
        const auto sample_x = static_cast<float>(x) + 0.5F;
        const auto sample_y = static_cast<float>(y) + 0.5F;
        const auto w0 =
            edge(second_position, third_position, sample_x, sample_y) / area;
        const auto w1 =
            edge(third_position, first_position, sample_x, sample_y) / area;
        const auto w2 = 1.0F - w0 - w1;
        if (w0 < -0.00001F || w1 < -0.00001F || w2 < -0.00001F) {
          continue;
        }
        const auto inverse = w0 * inverse_depth[0U] + w1 * inverse_depth[1U] +
                             w2 * inverse_depth[2U];
        if (inverse <= 0.0F || !std::isfinite(inverse)) {
          continue;
        }
        const auto depth = (1.0F / inverse) * depth_bias;
        const auto pixel = static_cast<std::size_t>(y) *
                               static_cast<std::size_t>(depth_width) +
                           static_cast<std::size_t>(x);
        ++stats.depth_tested_fragments;
        if (depth >= depth_resources.values[pixel]) {
          ++stats.depth_rejected_fragments;
          continue;
        }
        const auto interpolate = [&](const float a, const float b,
                                     const float c) {
          return (w0 * a * inverse_depth[0U] + w1 * b * inverse_depth[1U] +
                  w2 * c * inverse_depth[2U]) /
                 inverse;
        };
        const auto u = interpolate(first.tex_coord.x, second.tex_coord.x,
                                   third.tex_coord.x);
        const auto v = interpolate(first.tex_coord.y, second.tex_coord.y,
                                   third.tex_coord.y);
        std::array<float, 4U> texel{1.0F, 1.0F, 1.0F, 1.0F};
        if (image != nullptr) {
          const auto source_x = u * static_cast<float>(image->width) - 0.5F;
          const auto source_y = v * static_cast<float>(image->height) - 0.5F;
          const auto floor_x = std::floor(source_x);
          const auto floor_y = std::floor(source_y);
          const auto x0 = static_cast<int>(floor_x);
          const auto y0 = static_cast<int>(floor_y);
          const auto tx = source_x - floor_x;
          const auto ty = source_y - floor_y;
          const auto wrapped_x0 = wrap(x0, static_cast<int>(image->width));
          const auto wrapped_x1 = wrap(x0 + 1, static_cast<int>(image->width));
          const auto wrapped_y0 = wrap(y0, static_cast<int>(image->height));
          const auto wrapped_y1 = wrap(y0 + 1, static_cast<int>(image->height));
          const std::array<std::size_t, 4U> offsets{
              (static_cast<std::size_t>(wrapped_y0) * image->width +
               static_cast<std::size_t>(wrapped_x0)) *
                  4U,
              (static_cast<std::size_t>(wrapped_y0) * image->width +
               static_cast<std::size_t>(wrapped_x1)) *
                  4U,
              (static_cast<std::size_t>(wrapped_y1) * image->width +
               static_cast<std::size_t>(wrapped_x0)) *
                  4U,
              (static_cast<std::size_t>(wrapped_y1) * image->width +
               static_cast<std::size_t>(wrapped_x1)) *
                  4U};
          for (std::size_t channel = 0U; channel < texel.size(); ++channel) {
            const auto top =
                (static_cast<float>(image->rgba[offsets[0U] + channel]) /
                 255.0F) *
                    (1.0F - tx) +
                (static_cast<float>(image->rgba[offsets[1U] + channel]) /
                 255.0F) *
                    tx;
            const auto bottom =
                (static_cast<float>(image->rgba[offsets[2U] + channel]) /
                 255.0F) *
                    (1.0F - tx) +
                (static_cast<float>(image->rgba[offsets[3U] + channel]) /
                 255.0F) *
                    tx;
            texel[channel] = top * (1.0F - ty) + bottom * ty;
          }
        }
        const auto alpha = texel[3U] * opacity *
                           interpolate(first.color.a, second.color.a,
                                       third.color.a);
        if ((!additive && alpha < 0.5F) || (additive && alpha <= 0.001F)) {
          continue;
        }
        const std::array<float, 3U> colors{
            texel[0U] *
                interpolate(first.color.r, second.color.r, third.color.r),
            texel[1U] *
                interpolate(first.color.g, second.color.g, third.color.g),
            texel[2U] *
                interpolate(first.color.b, second.color.b, third.color.b)};
        for (std::size_t channel = 0U; channel < colors.size(); ++channel) {
          const auto destination =
              static_cast<float>(
                  depth_resources.color[pixel * 4U + channel]) /
              255.0F;
          const auto output = additive
                                  ? destination + colors[channel] * alpha
                                  : colors[channel];
          depth_resources.color[pixel * 4U + channel] =
              static_cast<std::uint8_t>(std::clamp(output, 0.0F, 1.0F) *
                                            255.0F +
                                        0.5F);
        }
        depth_resources.values[pixel] = depth;
      }
    }
  };
  for (const auto &projected_face : faces) {
    const auto &component = scene.components[projected_face.component_index];
    const auto &face = component.model.faces[projected_face.face_index];
    const auto authored_environment_map =
        scene.assembled_car ? vehicle_environment_map(face)
                            : VehicleEnvironmentMap::none;
    const auto environment_map =
        textures && scene.assembled_car &&
                car_shading != CarShading::flat &&
                car_shading != CarShading::gouraud
            ? authored_environment_map
            : VehicleEnvironmentMap::none;
    const auto *environment_texture =
        environment_map == VehicleEnvironmentMap::none
            ? nullptr
            : vehicle_environment_texture(scene.vehicle_environment,
                                          environment_map);
    const MaterialTexture *material = nullptr;
    if (environment_texture == nullptr && textures &&
        face.has_texture_coordinates &&
        face.material_name_index < component.material_indices.size() &&
        component.material_indices[face.material_name_index].has_value()) {
      material =
          &scene.materials[*component
                                .material_indices[face.material_name_index]];
    }
    const auto material_name =
        face.material_name_index < component.model.names.size()
            ? ascii_lower(component.model.names[face.material_name_index])
            : std::string{};
    const auto emissive = material_name == "ogblight.iff" ||
                          material_name == "ogflight.iff" ||
                          material_name == "redpatbl.iff";
    std::array<SDL_Vertex, 4U> vertices{};
    const auto body_component = component.label == "body";
    const auto base_color =
        scene.assembled_car
            ? mh::content::select_car_face_base_color(
                  face, scene.vehicle_default_colors,
                  scene.vehicle_default_colors, body_component)
                  .rgb
            : face.color;
    auto red = static_cast<float>(base_color[0]) / 255.0F;
    auto green = static_cast<float>(base_color[1]) / 255.0F;
    auto blue = static_cast<float>(base_color[2]) / 255.0F;
    if (material != nullptr) {
      red = 1.0F;
      green = 1.0F;
      blue = 1.0F;
    }
    const auto valid_normal =
        face.normal_index < component.model.normals.size();
    Vec3 face_normal{};
    if (valid_normal) {
      const auto &source = component.model.normals[face.normal_index];
      face_normal = Vec3{source[0], source[1], source[2]};
    }
    bool face_shaded = false;
    for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
      const auto &point = projected[projected_face.component_index]
                                   [face.position_indices[vertex]];
      vertices[vertex].position = SDL_FPoint{point.x, point.y};
      auto vertex_red = red;
      auto vertex_green = green;
      auto vertex_blue = blue;
      if (scene.world_scene &&
          projected_face.face_index < component.face_vertex_colors.size()) {
        const auto &authored =
            component.face_vertex_colors[projected_face.face_index][vertex];
        vertex_red =
            static_cast<float>(authored[0U]) / 255.0F *
            scene.world_accelerated_brightness[0U];
        vertex_green =
            static_cast<float>(authored[1U]) / 255.0F *
            scene.world_accelerated_brightness[1U];
        vertex_blue =
            static_cast<float>(authored[2U]) / 255.0F *
            scene.world_accelerated_brightness[2U];
      }
      if (scene.assembled_car && !emissive && valid_normal) {
        const auto normal_index =
            car_shading != CarShading::flat && face.has_vertex_normals
                ? face.normal_indices[vertex]
                : face.normal_index;
        if (normal_index >= component.model.normals.size()) {
          throw std::runtime_error(
              "MYO vehicle vertex normal is outside its parsed table");
        }
        const auto &source_normal = component.model.normals[normal_index];
        const auto authored_environment_surface =
            authored_environment_map != VehicleEnvironmentMap::none;
        const auto apply_accelerated =
            !body_component || !authored_environment_surface ||
            (car_shading != CarShading::flat &&
             car_shading != CarShading::gouraud);
        const auto lighting = vehicle_lighting_factor(
            scene,
            Vec3{source_normal[0U], source_normal[1U], source_normal[2U]},
            apply_accelerated);
        vertex_red *= lighting[0U];
        vertex_green *= lighting[1U];
        vertex_blue *= lighting[2U];
      }
      if (applied_lighting && scene.world_scene && valid_normal) {
        const auto &source =
            component.model.positions[face.position_indices[vertex]];
        const auto lighting =
            shade_world_vertex(scene,
                               Vec3{source[0] + component.translation.x,
                                    source[1] + component.translation.y,
                                    source[2] + component.translation.z},
                               face_normal);
        vertex_red *= lighting.color[0];
        vertex_green *= lighting.color[1];
        vertex_blue *= lighting.color[2];
        stats.light_contributions += lighting.contributions;
        face_shaded = face_shaded || lighting.contributions != 0U;
      }
      vertices[vertex].color =
          SDL_FColor{vertex_red, vertex_green, vertex_blue, 1.0F};
      if (environment_texture != nullptr) {
        const auto normal_index = face.has_vertex_normals
                                      ? face.normal_indices[vertex]
                                      : face.normal_index;
        if (normal_index >= component.model.normals.size()) {
          throw std::runtime_error(
              "MYO vehicle vertex normal is outside its parsed table");
        }
        const auto &source_normal = component.model.normals[normal_index];
        vertices[vertex].tex_coord = vehicle_environment_coordinate(rotate(
            Vec3{source_normal[0U], source_normal[1U], source_normal[2U]}, yaw,
            pitch));
      } else if (material != nullptr) {
        vertices[vertex].tex_coord = SDL_FPoint{
            face.texture_coordinates[vertex][0] / material->original_width,
            face.texture_coordinates[vertex][1] / material->original_height};
      }
    }
    if (applied_lighting && scene.world_scene && face_shaded) {
      ++stats.shaded_faces;
    }
    const auto index_count = face.vertex_count == 3U ? 3 : 6;
    SDL_Texture *render_texture =
        environment_texture != nullptr
            ? environment_texture->texture.get()
            : material == nullptr
                  ? nullptr
                  : hd_enabled && material->override_used
                        ? material->override_texture.get()
                        : material->texture.get();
    if (depth_enabled) {
      const mh::content::PamRgbaImage *image = nullptr;
      if (environment_texture != nullptr) {
        image = &environment_texture->image;
      } else if (material != nullptr) {
        image = hd_enabled && material->override_used
                    ? &*material->override_image
                    : &material->image;
      }
      rasterize_triangle(vertices, projected_face.component_index, face,
                         {0U, 1U, 2U}, image);
      if (face.vertex_count == 4U) {
        rasterize_triangle(vertices, projected_face.component_index, face,
                           {0U, 2U, 3U}, image);
      }
      if (car_shading == CarShading::glenz && body_component &&
          environment_texture != nullptr) {
        auto coat = vertices;
        for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
          coat[vertex].color = SDL_FColor{1.0F, 1.0F, 1.0F, 1.0F};
        }
        rasterize_triangle(coat, projected_face.component_index, face,
                           {0U, 1U, 2U}, image, true, 0.14F, 0.9999F);
        if (face.vertex_count == 4U) {
          rasterize_triangle(coat, projected_face.component_index, face,
                             {0U, 2U, 3U}, image, true, 0.14F, 0.9999F);
        }
      }
    } else {
      require(SDL_RenderGeometry(renderer, render_texture, vertices.data(),
                                 static_cast<int>(face.vertex_count),
                                 triangles.data(), index_count),
              "render model face");
      if (car_shading == CarShading::glenz && body_component &&
          environment_texture != nullptr) {
        // Match the race renderer: Glenz retains the opaque reflection-mapped
        // paint and adds a neutral 14% clear-coat.  The coat is additive, so
        // cabin and internal geometry never become visible through the body.
        auto coat = vertices;
        for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
          coat[vertex].color = SDL_FColor{1.0F, 1.0F, 1.0F, 0.14F};
        }
        require(SDL_SetTextureBlendMode(render_texture, SDL_BLENDMODE_ADD),
                "set Glenz clear-coat blend mode");
        require(SDL_RenderGeometry(renderer, render_texture, coat.data(),
                                   static_cast<int>(face.vertex_count),
                                   triangles.data(), index_count),
                "render Glenz clear-coat");
        require(SDL_SetTextureBlendMode(render_texture, SDL_BLENDMODE_NONE),
                "restore vehicle environment blend mode");
      }
    }
    if (wireframe && !depth_enabled) {
      require(SDL_SetRenderDrawColor(renderer, 68U, 220U, 255U, 210U),
              "set wireframe color");
      for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
        const auto &first = vertices[vertex].position;
        const auto &second =
            vertices[(vertex + 1U) % face.vertex_count].position;
        require(SDL_RenderLine(renderer, first.x, first.y, second.x, second.y),
                "render wireframe edge");
      }
    }
  }
  if (depth_enabled) {
    require(SDL_UpdateTexture(depth_resources.texture.get(), nullptr,
                              depth_resources.color.data(), depth_width * 4),
            "upload depth-buffer texture");
    require(SDL_RenderTexture(renderer, depth_resources.texture.get(), nullptr,
                              nullptr),
            "render depth-buffer texture");
    if (wireframe) {
      require(SDL_SetRenderDrawColor(renderer, 68U, 220U, 255U, 210U),
              "set deferred wireframe color");
      for (const auto &projected_face : faces) {
        const auto &face = scene.components[projected_face.component_index]
                               .model.faces[projected_face.face_index];
        for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
          const auto &first = projected[projected_face.component_index]
                                       [face.position_indices[vertex]];
          const auto &second =
              projected[projected_face.component_index]
                       [face.position_indices[(vertex + 1U) %
                                              face.vertex_count]];
          require(
              SDL_RenderLine(renderer, first.x, first.y, second.x, second.y),
              "render deferred wireframe edge");
        }
      }
    }
  }
  if (normals) {
    require(SDL_SetRenderDrawColor(renderer, 255U, 190U, 72U, 255U),
            "set normal color");
    for (std::size_t component_index = 0U;
         component_index < scene.components.size(); ++component_index) {
      const auto &component = scene.components[component_index];
      for (const auto &face : component.model.faces) {
        if (face.normal_index >= component.model.normals.size()) {
          continue;
        }
        float x = 0.0F;
        float y = 0.0F;
        for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
          const auto &point =
              projected[component_index][face.position_indices[vertex]];
          x += point.x;
          y += point.y;
        }
        x /= face.vertex_count;
        y /= face.vertex_count;
        const auto &source = component.model.normals[face.normal_index];
        if (!std::isfinite(source[0]) || !std::isfinite(source[1]) ||
            !std::isfinite(source[2])) {
          continue;
        }
        const auto normal =
            rotate(Vec3{source[0], source[1], source[2]}, yaw, pitch);
        require(SDL_RenderLine(renderer, x, y, x + normal.x * 24.0F,
                               y - normal.y * 24.0F),
                "render face normal");
      }
    }
  }
  if (collision_overlay) {
    require(SDL_SetRenderDrawColor(renderer, 255U, 64U, 210U, 230U),
            "set collision overlay color");
    for (const auto &polygon : scene.collision_polygons) {
      if (polygon.vertices.size() < 2U) {
        continue;
      }
      for (std::size_t index = 0U; index < polygon.vertices.size(); ++index) {
        const auto &first_source = polygon.vertices[index];
        const auto &second_source =
            polygon.vertices[(index + 1U) % polygon.vertices.size()];
        const auto first = project_world_position(
            view, Vec3{first_source[0], first_source[1], first_source[2]},
            width, height, yaw, pitch, zoom, pan_x, pan_y);
        const auto second = project_world_position(
            view, Vec3{second_source[0], second_source[1], second_source[2]},
            width, height, yaw, pitch, zoom, pan_x, pan_y);
        if (!first.visible || !second.visible) {
          continue;
        }
        require(SDL_RenderLine(renderer, first.x, first.y, second.x, second.y),
                "render collision polygon edge");
      }
    }
  }
  if (lighting_overlay) {
    constexpr std::size_t circle_segments = 24U;
    for (const auto &light : scene.world_lights) {
      if (local_cell.has_value() || route_group.has_value()) {
        const auto dx = light.position[0] - view.center.x;
        const auto dz = light.position[2] - view.center.z;
        const auto limit = view.radius * 1.5F + light.range;
        if (dx * dx + dz * dz > limit * limit) {
          continue;
        }
      }
      const auto center = project_world_position(
          view, Vec3{light.position[0], light.position[1], light.position[2]},
          width, height, yaw, pitch, zoom, pan_x, pan_y);
      if (!center.visible) {
        continue;
      }
      const auto channel = [](const float value) {
        return static_cast<std::uint8_t>(std::clamp(value, 0.0F, 1.0F) *
                                         255.0F);
      };
      require(SDL_SetRenderDrawColor(renderer, channel(light.color[0]),
                                     channel(light.color[1]),
                                     channel(light.color[2]), 230U),
              "set point-light overlay color");
      require(SDL_RenderLine(renderer, center.x - 5.0F, center.y,
                             center.x + 5.0F, center.y),
              "render point-light marker");
      require(SDL_RenderLine(renderer, center.x, center.y - 5.0F, center.x,
                             center.y + 5.0F),
              "render point-light marker");
      ProjectedVertex previous{};
      for (std::size_t segment = 0U; segment <= circle_segments; ++segment) {
        const auto angle = static_cast<float>(segment) * 6.28318530718F /
                           static_cast<float>(circle_segments);
        const auto point = project_world_position(
            view,
            Vec3{light.position[0] + std::cos(angle) * light.range,
                 light.position[1],
                 light.position[2] + std::sin(angle) * light.range},
            width, height, yaw, pitch, zoom, pan_x, pan_y);
        if (segment != 0U && previous.visible && point.visible) {
          require(SDL_RenderLine(renderer, previous.x, previous.y, point.x,
                                 point.y),
                  "render point-light range");
        }
        previous = point;
      }
    }
    require(SDL_SetRenderDrawColor(renderer, 255U, 230U, 64U, 240U),
            "set lens-flare overlay color");
    for (const auto &flare : scene.world_lens_flares) {
      if (local_cell.has_value() || route_group.has_value()) {
        const auto dx = flare.position[0] - view.center.x;
        const auto dz = flare.position[2] - view.center.z;
        if (dx * dx + dz * dz > view.radius * view.radius * 2.25F) {
          continue;
        }
      }
      const auto first = project_world_position(
          view, Vec3{flare.position[0], flare.position[1], flare.position[2]},
          width, height, yaw, pitch, zoom, pan_x, pan_y);
      const auto second = project_world_position(
          view,
          Vec3{flare.secondary_position[0], flare.secondary_position[1],
               flare.secondary_position[2]},
          width, height, yaw, pitch, zoom, pan_x, pan_y);
      if (!first.visible || !second.visible) {
        continue;
      }
      require(SDL_RenderLine(renderer, first.x, first.y, second.x, second.y),
              "render lens-flare direction");
      require(SDL_RenderLine(renderer, first.x - 4.0F, first.y - 4.0F,
                             first.x + 4.0F, first.y + 4.0F),
              "render lens-flare marker");
      require(SDL_RenderLine(renderer, first.x - 4.0F, first.y + 4.0F,
                             first.x + 4.0F, first.y - 4.0F),
              "render lens-flare marker");
    }
  }
  if (route_overlay &&
      scene.world_route_positions.size() == scene.world_route.samples.size()) {
    require(SDL_SetRenderDrawColor(renderer, 70U, 255U, 126U, 255U),
            "set AI route overlay color");
    for (std::size_t index = 1U; index < scene.world_route_positions.size();
         ++index) {
      if (scene.world_route.samples[index - 1U].group_index !=
          scene.world_route.samples[index].group_index) {
        continue;
      }
      if (route_group.has_value() &&
          scene.world_route.samples[index].group_index != *route_group) {
        continue;
      }
      const auto &first_source = scene.world_route_positions[index - 1U];
      const auto &second_source = scene.world_route_positions[index];
      if (local_cell.has_value() || route_group.has_value()) {
        const auto first_dx = first_source.x - view.center.x;
        const auto first_dz = first_source.z - view.center.z;
        const auto second_dx = second_source.x - view.center.x;
        const auto second_dz = second_source.z - view.center.z;
        const auto limit_squared = view.radius * view.radius * 2.25F;
        if (first_dx * first_dx + first_dz * first_dz > limit_squared &&
            second_dx * second_dx + second_dz * second_dz > limit_squared) {
          continue;
        }
      }
      const auto first = project_world_position(
          view, first_source, width, height, yaw, pitch, zoom, pan_x, pan_y);
      const auto second = project_world_position(
          view, second_source, width, height, yaw, pitch, zoom, pan_x, pan_y);
      if (!first.visible || !second.visible) {
        continue;
      }
      require(SDL_RenderLine(renderer, first.x, first.y, second.x, second.y),
              "render AI route segment");
      require(SDL_RenderLine(renderer, first.x, first.y + 1.0F, second.x,
                             second.y + 1.0F),
              "render AI route segment width");
      ++stats.submitted_route_segments;
    }
  }
  std::uint64_t face_count = 0U;
  std::uint64_t position_count = 0U;
  for (const auto &component : scene.components) {
    face_count += component.model.faces.size();
    position_count += component.model.positions.size();
  }
  require(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND),
          "set viewer menu blend mode");
  constexpr SDL_FColor dock_color{0.006F, 0.009F, 0.016F, 0.96F};
  constexpr SDL_FColor help_color{0.020F, 0.025F, 0.035F, 0.98F};
  constexpr SDL_FColor divider_color{0.17F, 0.19F, 0.23F, 0.92F};
  constexpr SDL_FColor accent_rule_color{0.70F, 0.19F, 0.0F, 0.82F};
  // Exact retail front-end orange and inactive pause-menu red-orange.
  constexpr SDL_FColor accent_color{1.0F, 100.0F / 255.0F, 0.0F, 1.0F};
  constexpr SDL_FColor pause_color{224.0F / 255.0F, 104.0F / 255.0F,
                                   32.0F / 255.0F, 1.0F};
  constexpr SDL_FColor enabled_color{70.0F / 255.0F, 210.0F / 255.0F,
                                     145.0F / 255.0F, 1.0F};
  constexpr SDL_FColor selected_color{1.0F, 1.0F, 1.0F, 1.0F};
  constexpr SDL_FColor muted_color{0.52F, 0.56F, 0.64F, 1.0F};
  const auto set_color = [&](const SDL_FColor color,
                             const std::string_view operation) {
    require(SDL_SetRenderDrawColorFloat(renderer, color.r, color.g, color.b,
                                        color.a),
            operation);
  };
  const auto fill_rounded_rect = [&](const SDL_FRect rect,
                                     const float radius,
                                     const SDL_FColor color,
                                     const std::string_view operation) {
    set_color(color, operation);
    const auto rows = std::max(1, static_cast<int>(std::ceil(rect.h)));
    for (auto row = 0; row < rows; ++row) {
      const auto y = static_cast<float>(row) + 0.5F;
      auto inset = 0.0F;
      if (y < radius) {
        const auto dy = radius - y;
        inset = radius -
                std::sqrt(std::max(0.0F, radius * radius - dy * dy));
      } else if (y > rect.h - radius) {
        const auto dy = y - (rect.h - radius);
        inset = radius -
                std::sqrt(std::max(0.0F, radius * radius - dy * dy));
      }
      require(SDL_RenderLine(renderer, rect.x + inset, rect.y + row,
                             rect.x + rect.w - inset, rect.y + row),
              operation);
    }
  };
  const auto draw_rounded_panel = [&](const SDL_FRect rect,
                                      const float radius,
                                      const SDL_FColor fill,
                                      const SDL_FColor border,
                                      const std::string_view operation) {
    fill_rounded_rect(rect, radius, border, operation);
    const SDL_FRect inner{rect.x + 1.0F, rect.y + 1.0F,
                          std::max(1.0F, rect.w - 2.0F),
                          std::max(1.0F, rect.h - 2.0F)};
    fill_rounded_rect(inner, std::max(1.0F, radius - 1.0F), fill, operation);
  };
  const auto draw_text = [&](const float x, const float y,
                             const std::string_view text,
                             const SDL_FColor color,
                             const float scale = 1.0F) {
    draw_ui_text(renderer, ui_text_cache, pause_font, x, y, text, color,
                 std::max(9, static_cast<int>(std::round(13.0F * scale))));
  };
  const auto text_width = [&](const ViewerFont &font,
                              const std::string_view text, const float scale) {
    return ui_text_width(
        renderer, ui_text_cache, font, text,
        std::max(9, static_cast<int>(std::round(13.0F * scale))));
  };
  const auto draw_shortcut = [&](const float x, const float y,
                                 const std::string_view key,
                                 const std::string_view description) {
    draw_text(x, y, key, accent_color, 1.12F);
    draw_text(x + 86.0F, y, description, selected_color, 1.12F);
  };

  constexpr float margin = viewer_content_right_margin;
  const auto display_name = ascii_upper(
      scene.world_scene
          ? scene.track_name
          : scene.assembled_car ? scene.vehicle_name : "Standalone MYO");
  const auto dock_y =
      std::max(180.0F, static_cast<float>(height) - viewer_footer_height);
  const auto layout_left = viewer_content_left;

  // Identity and scene statistics share a single quiet header, leaving the
  // entire center of the window to the model.
  const SDL_FRect header{0.0F, 0.0F, static_cast<float>(width), 93.0F};
  set_color(dock_color, "set viewer identity header color");
  require(SDL_RenderFillRect(renderer, &header),
          "render viewer identity header");
  set_color(accent_rule_color, "set viewer identity header rule color");
  require(SDL_RenderLine(renderer, 0.0F, 92.0F, static_cast<float>(width),
                         92.0F),
          "render viewer identity header rule");

  constexpr float game_name_scale = 2.65F;
  const auto name_x = layout_left;
  draw_viewer_text(renderer, title_font, name_x, 18.0F, display_name,
                   pause_color, game_name_scale);
  const auto game_text_width = [&](const std::string_view text,
                                   const float scale) {
    auto result = 0.0F;
    for (const auto character : text) {
      const auto glyph = static_cast<std::uint8_t>(
          static_cast<unsigned char>(character));
      result += static_cast<float>(title_font.advances[glyph]) * scale;
    }
    return result;
  };
  // Model facts sit directly beneath the identity and use the same authored
  // game font. Labels remain red; values remain white.
  auto detail_x = name_x;
  constexpr float detail_scale = 1.05F;
  const auto draw_header_detail = [&](const std::string_view label,
                                      const std::string &value) {
    draw_viewer_text(renderer, title_font, detail_x, 59.0F, label,
                     pause_color, detail_scale);
    detail_x += game_text_width(label, detail_scale) + 7.0F;
    draw_viewer_text(renderer, title_font, detail_x, 59.0F, value,
                     selected_color, detail_scale);
    detail_x += game_text_width(value, detail_scale) + 22.0F;
  };
  draw_header_detail(
      "FACES",
      std::to_string(local_cell.has_value() || route_group.has_value()
                         ? stats.submitted_faces
                         : face_count));
  draw_header_detail("VERTICES", std::to_string(position_count));
  draw_header_detail("MATERIALS", std::to_string(scene.materials.size()));
  draw_header_detail("PARTS", std::to_string(scene.components.size()));

  const SDL_FRect shading_box{static_cast<float>(width) - 149.0F, 57.0F,
                              120.0F, 27.0F};
  draw_viewer_text(renderer, title_font, shading_box.x, 18.0F,
                   scene.world_scene ? "LIGHTING" : "SHADING", pause_color,
                   2.15F);
  fill_rounded_rect(shading_box, 2.0F, help_color,
                    "render viewer shading selection");
  const auto shading_value =
      scene.world_scene
          ? applied_lighting ? std::string_view("LIT")
                             : std::string_view("AUTHORED")
          : scene.assembled_car ? car_shading_name(car_shading)
                                : std::string_view("MODEL");
  draw_viewer_text(renderer, title_font, shading_box.x + 8.0F,
                   shading_box.y + 7.0F,
                   ascii_upper(std::string(shading_value)), selected_color,
                   1.05F);
  draw_viewer_text(renderer, title_font,
                   shading_box.x + shading_box.w - 18.0F,
                   shading_box.y + 7.0F,
                   shading_dropdown_open ? "^" : "V", pause_color, 1.05F);

  const auto viewport_bottom = dock_y - viewer_viewport_bottom_gap;
  if (scene.world_scene && view.fit_full_track) {
    const auto fitted_scale = full_track_fitted_scale(view, width, height, zoom);
    const auto desired_distance = 64.0F / std::max(fitted_scale, 0.001F);
    const auto magnitude =
        std::pow(10.0F, std::floor(std::log10(desired_distance)));
    const auto normalized = desired_distance / magnitude;
    const auto step = normalized < 2.0F ? 1.0F
                      : normalized < 5.0F ? 2.0F
                                          : 5.0F;
    const auto scale_distance = step * magnitude;
    const auto scale_width = scale_distance * fitted_scale;
    std::ostringstream scale_label;
    if (scale_distance >= 1000.0F) {
      scale_label << std::fixed << std::setprecision(1)
                  << scale_distance / 1000.0F << " km";
    } else {
      scale_label << static_cast<unsigned>(std::round(scale_distance)) << " m";
    }
    draw_text(viewer_content_left, viewport_bottom - 28.0F,
              scale_label.str(), muted_color, 0.78F);
    set_color(divider_color, "set viewer scale marker color");
    require(SDL_RenderLine(renderer, viewer_content_left,
                           viewport_bottom - 12.0F,
                           viewer_content_left + scale_width,
                           viewport_bottom - 12.0F),
            "render viewer scale marker");
  }

  const SDL_FRect dock{0.0F, dock_y, static_cast<float>(width),
                       viewer_footer_height};
  set_color(dock_color, "set viewer control dock color");
  require(SDL_RenderFillRect(renderer, &dock), "render viewer control dock");
  set_color(divider_color, "set viewer control dock rule color");
  require(SDL_RenderLine(renderer, 0.0F, dock_y, static_cast<float>(width),
                         dock_y),
          "render viewer control dock rule");

  const auto draw_footer_action = [&](const float x, const std::string_view key,
                                      const std::string_view label,
                                      const bool active = false) {
    const SDL_FRect key_box{x, dock_y + 15.0F,
                            text_width(pause_font, key, 0.88F) + 16.0F,
                            28.0F};
    draw_rounded_panel(key_box, 2.0F, help_color,
                       active ? accent_color : divider_color,
                       "render viewer footer key");
    draw_text(key_box.x + 8.0F, key_box.y + 8.0F, key,
              active ? accent_color : selected_color, 0.88F);
    draw_text(key_box.x + key_box.w + 9.0F, key_box.y + 8.0F, label,
              selected_color, 0.88F);
  };
  auto toggle_x = layout_left;
  const auto draw_display_toggle = [&](const std::string_view label,
                                       const float control_width,
                                       const bool enabled) {
    draw_text(toggle_x, dock_y + 22.0F, label,
              enabled ? enabled_color : selected_color, 0.90F);
    const SDL_FRect checkbox{toggle_x + control_width - 22.0F,
                             dock_y + 18.0F, 17.0F, 17.0F};
    if (enabled) {
      set_color(enabled_color, "set enabled viewer checkbox color");
      require(SDL_RenderFillRect(renderer, &checkbox),
              "render enabled viewer checkbox");
      constexpr SDL_FColor check_color{0.02F, 0.03F, 0.03F, 1.0F};
      set_color(check_color, "set viewer checkbox check color");
      require(SDL_RenderLine(renderer, checkbox.x + 4.0F,
                             checkbox.y + 8.0F, checkbox.x + 7.0F,
                             checkbox.y + 12.0F),
              "render viewer checkbox check");
      require(SDL_RenderLine(renderer, checkbox.x + 7.0F,
                             checkbox.y + 12.0F, checkbox.x + 13.0F,
                             checkbox.y + 4.0F),
              "render viewer checkbox check");
    } else {
      set_color(divider_color, "set disabled viewer checkbox color");
      require(SDL_RenderRect(renderer, &checkbox),
              "render disabled viewer checkbox");
    }
    toggle_x += control_width + viewer_pill_gap;
  };
  draw_display_toggle("Textures", viewer_textures_pill_width, textures);
  draw_display_toggle("Solid", viewer_solid_pill_width,
                      !wireframe && !normals && !collision_overlay);
  draw_display_toggle("Wireframe", viewer_wireframe_pill_width, wireframe);
  draw_display_toggle("Normals", viewer_normals_pill_width, normals);
  draw_display_toggle("Collision", viewer_collision_pill_width,
                      collision_overlay);

  draw_footer_action(static_cast<float>(width) - 430.0F, "←", "Previous");
  draw_footer_action(static_cast<float>(width) - 328.0F, "→", "Next");
  draw_footer_action(static_cast<float>(width) - 226.0F, "R", "Reset");
  draw_footer_action(static_cast<float>(width) - 138.0F, "M", "Mode");
  draw_footer_action(static_cast<float>(width) - 48.0F, "?", "", show_help);

  if (shading_dropdown_open) {
    constexpr float dropdown_row_height = 24.0F;
    const auto dropdown_rows =
        scene.assembled_car ? car_shading_names.size() : 2U;
    const SDL_FRect dropdown{static_cast<float>(width) - 149.0F, 87.0F,
                             120.0F,
                             dropdown_row_height * dropdown_rows};
    draw_rounded_panel(dropdown, 2.0F, help_color, divider_color,
                       "render viewer shading dropdown");
    for (std::size_t index = 0U; index < dropdown_rows; ++index) {
      const auto selected = scene.assembled_car
                                ? index == static_cast<std::size_t>(car_shading)
                                : index == (applied_lighting ? 1U : 0U);
      const auto option_name = scene.assembled_car
                                   ? car_shading_names[index]
                                   : index == 0U ? std::string_view("Authored")
                                                 : std::string_view("Lit");
      const SDL_FRect row{dropdown.x + 1.0F,
                          dropdown.y + 1.0F + index * dropdown_row_height,
                          dropdown.w - 2.0F, dropdown_row_height - 1.0F};
      if (selected) {
        constexpr SDL_FColor selected_fill{0.18F, 0.055F, 0.02F, 1.0F};
        set_color(selected_fill, "set selected shading row color");
        require(SDL_RenderFillRect(renderer, &row),
                "render selected shading row");
      }
      draw_viewer_text(
          renderer, title_font, row.x + 8.0F, row.y + 5.0F,
          ascii_upper(std::string(option_name)),
          selected ? pause_color : selected_color,
          1.0F);
    }
  }

  if (show_help) {
    const auto help_width =
        std::min(430.0F, static_cast<float>(width) - margin * 2.0F);
    const auto help_height = scene.world_scene ? 144.0F : 58.0F;
    const SDL_FRect help_panel{
        static_cast<float>(width) - margin - help_width,
        std::max(86.0F, dock_y - help_height - 12.0F), help_width,
        help_height};
    draw_rounded_panel(help_panel, 13.0F, help_color, divider_color,
                       "render viewer help panel");
    draw_text(help_panel.x + 17.0F, help_panel.y + 16.0F, "CONTROLS",
              selected_color, 1.18F);
    draw_text(help_panel.x + help_panel.w - 24.0F, help_panel.y + 16.0F, "X",
              muted_color, 1.0F);
    set_color(divider_color, "set viewer help title rule color");
    require(SDL_RenderLine(renderer, help_panel.x + 16.0F,
                           help_panel.y + 40.0F,
                           help_panel.x + help_panel.w - 16.0F,
                           help_panel.y + 40.0F),
            "render viewer help title rule");
    const auto help_column_width = (help_panel.w - 34.0F) / 2.0F;
    const auto help_x = [&](const std::size_t column) {
      return help_panel.x + 17.0F +
             static_cast<float>(column) * help_column_width;
    };
    const auto help_y = [&](const std::size_t row) {
      return help_panel.y + 52.0F + static_cast<float>(row) * 20.0F;
    };
    if (scene.world_scene) {
      draw_text(help_x(0U), help_y(0U), "VIEW", pause_color, 1.0F);
      draw_shortcut(help_x(0U), help_y(1U), "PGUP/DN", "Focus");
      draw_shortcut(help_x(0U), help_y(2U), "SPACE", "Play / pause");
      draw_text(help_x(1U), help_y(0U), "TRACK TOOLS", pause_color, 1.0F);
      draw_shortcut(help_x(1U), help_y(1U), "V", "View");
      draw_shortcut(help_x(1U), help_y(2U), "P", "Route line");
      draw_shortcut(help_x(1U), help_y(3U), "L", "Light markers");
    }
  }
  return stats;
}

std::uint64_t hash_surface(const SDL_Surface &surface) {
  constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
  constexpr std::uint64_t prime = 1099511628211ULL;
  auto hash = offset_basis;
  const auto bytes = static_cast<std::size_t>(surface.pitch) *
                     static_cast<std::size_t>(surface.h);
  const auto *data = static_cast<const std::uint8_t *>(surface.pixels);
  for (std::size_t index = 0U; index < bytes; ++index) {
    hash ^= data[index];
    hash *= prime;
  }
  return hash;
}

std::string hex16(std::uint64_t value) {
  constexpr std::array<char, 16U> digits{'0', '1', '2', '3', '4', '5',
                                         '6', '7', '8', '9', 'a', 'b',
                                         'c', 'd', 'e', 'f'};
  std::string result(16U, '0');
  for (std::size_t index = result.size(); index > 0U; --index) {
    result[index - 1U] = digits[value & 0xfU];
    value >>= 4U;
  }
  return result;
}

std::string make_report(const Options &options, const Scene &scene,
                        const std::string_view renderer_name,
                        const std::uint32_t frames,
                        const std::uint64_t frame_hash,
                        const RenderStats &render_stats) {
  std::uint64_t positions = 0U;
  std::uint64_t faces = 0U;
  std::uint64_t textured_faces = 0U;
  for (const auto &component : scene.components) {
    positions += component.model.positions.size();
    faces += component.model.faces.size();
    textured_faces += component.model.textured_face_count;
  }
  std::string report = "{\"schema\":\"motorhead.model-viewer-report.v2\"";
  report += ",\"model\":" + mh::common::json_string(
                                options.model_relative_path.generic_string());
  report += ",\"track\":" + mh::common::json_string(options.track);
  report += ",\"track_definition\":" +
            (scene.track_definition_relative_path.empty()
                 ? std::string("null")
                 : mh::common::json_string(
                       scene.track_definition_relative_path.generic_string()));
  report +=
      ",\"track_name\":" + (scene.track_name.empty()
                                ? std::string("null")
                                : mh::common::json_string(scene.track_name));
  report += ",\"track_roster_index\":" +
            (scene.track_roster_count == 0U
                 ? std::string("null")
                 : std::to_string(scene.track_roster_index));
  report +=
      ",\"track_roster_count\":" + std::to_string(scene.track_roster_count);
  report += ",\"pack\":" + mh::common::json_string(options.pack);
  report += ",\"view\":" + mh::common::json_string(options.view);
  report += ",\"renderer\":" + mh::common::json_string(renderer_name);
  report += ",\"frames\":" + std::to_string(frames);
  const auto lod = scene.lod;
  report += ",\"lod\":" + (lod.has_value() ? std::to_string(*lod - '0')
                                           : std::string("null"));
  report += ",\"assembled_car\":" +
            std::string(scene.assembled_car ? "true" : "false");
  report += ",\"vehicle_name\":" +
            (scene.vehicle_name.empty()
                 ? std::string("null")
                 : mh::common::json_string(scene.vehicle_name));
  report += ",\"vehicle_default_colors\":";
  if (!scene.assembled_car) {
    report += "null";
  } else {
    report += '[';
    for (std::size_t index = 0U; index < scene.vehicle_default_colors.size();
         ++index) {
      if (index != 0U) {
        report += ',';
      }
      const auto &color = scene.vehicle_default_colors[index];
      report += '[' + std::to_string(color.red) + ',' +
                std::to_string(color.green) + ',' + std::to_string(color.blue) +
                ']';
    }
    report += ']';
  }
  report += ",\"vehicle_environment_materials\":" +
            std::string(scene.vehicle_environment.complete ? "true" : "false");
  report += ",\"car_shading\":" +
            mh::common::json_string(car_shading_name(options.car_shading));
  report +=
      ",\"world_scene\":" + std::string(scene.world_scene ? "true" : "false");
  report +=
      ",\"world_light_count\":" + std::to_string(scene.world_lights.size());
  report += ",\"world_lens_flare_count\":" +
            std::to_string(scene.world_lens_flares.size());
  report += ",\"world_section_count\":" +
            std::to_string(scene.world_section_cells.size());
  report += ",\"route_sample_count\":" +
            std::to_string(scene.world_route.samples.size());
  report += ",\"route_group_count\":" +
            std::to_string(scene.world_route.groups.size());
  report += ",\"route_metadata_count_mismatches\":" +
            std::to_string(scene.world_route.metadata_count_mismatches);
  report += ",\"route_overlay\":" +
            std::string(options.route_overlay ? "true" : "false");
  report += ",\"lighting_overlay\":" +
            std::string(options.lighting_overlay ? "true" : "false");
  report += ",\"applied_lighting\":" +
            std::string(options.applied_lighting ? "true" : "false");
  report += ",\"local_cell\":" + (options.local_cell.has_value()
                                      ? std::to_string(*options.local_cell)
                                      : std::string("null"));
  report += ",\"route_group\":" + (options.route_group.has_value()
                                       ? std::to_string(*options.route_group)
                                       : std::string("null"));
  report += ",\"route_sample\":" + (options.route_sample.has_value()
                                        ? std::to_string(*options.route_sample)
                                        : std::string("null"));
  report +=
      ",\"route_chase\":" + std::string(options.route_chase ? "true" : "false");
  report += ",\"depth_buffer\":" +
            std::string(options.depth_buffer ? "true" : "false");
  report +=
      ",\"submitted_faces\":" + std::to_string(render_stats.submitted_faces);
  report += ",\"submitted_world_objects\":" +
            std::to_string(render_stats.submitted_world_objects);
  report += ",\"shaded_faces\":" + std::to_string(render_stats.shaded_faces);
  report += ",\"light_contributions\":" +
            std::to_string(render_stats.light_contributions);
  report += ",\"submitted_route_segments\":" +
            std::to_string(render_stats.submitted_route_segments);
  report += ",\"depth_tested_fragments\":" +
            std::to_string(render_stats.depth_tested_fragments);
  report += ",\"depth_rejected_fragments\":" +
            std::to_string(render_stats.depth_rejected_fragments);
  report += ",\"depth_width\":" + std::to_string(render_stats.depth_width);
  report += ",\"depth_height\":" + std::to_string(render_stats.depth_height);
  report +=
      ",\"wheel_geometry\":" + mh::common::json_string(scene.wheel_geometry);
  report += ",\"wheel_anchor_source\":" +
            mh::common::json_string(scene.wheel_anchor_source);
  report += ",\"component_count\":" + std::to_string(scene.components.size());
  report += ",\"positions\":" + std::to_string(positions);
  report += ",\"faces\":" + std::to_string(faces);
  report += ",\"textured_faces\":" + std::to_string(textured_faces);
  report += ",\"selected_materials\":" + std::to_string(scene.materials.size());
  report +=
      ",\"fallback_materials\":" + std::to_string(scene.fallback_materials);
  report +=
      ",\"hd_enabled\":" + std::string(options.hd_enabled ? "true" : "false");
  report += ",\"available_override_materials\":" +
            std::to_string(scene.override_materials);
  report += ",\"override_materials\":" +
            std::to_string(options.hd_enabled ? scene.override_materials : 0U);
  std::uint64_t collision_vertices = 0U;
  for (const auto &polygon : scene.collision_polygons) {
    collision_vertices += polygon.vertices.size();
  }
  report += ",\"collision_polygons\":" +
            std::to_string(scene.collision_polygons.size());
  report += ",\"collision_vertices\":" + std::to_string(collision_vertices);
  report += ",\"frame_readback_fnv1a64\":" +
            mh::common::json_string(hex16(frame_hash));
  report += ",\"components\":[";
  bool first = true;
  for (const auto &component : scene.components) {
    if (!first) {
      report += ',';
    }
    first = false;
    report += "{\"label\":" + mh::common::json_string(component.label);
    report += ",\"model\":" +
              mh::common::json_string(component.relative_path.generic_string());
    report += ",\"translation\":[" + std::to_string(component.translation.x) +
              ',' + std::to_string(component.translation.y) + ',' +
              std::to_string(component.translation.z) + ']';
    report +=
        ",\"positions\":" + std::to_string(component.model.positions.size());
    report +=
        ",\"faces\":" + std::to_string(component.model.faces.size()) + '}';
  }
  report += "],\"selected_textures\":[";
  first = true;
  for (const auto &material : scene.materials) {
    if (!first) {
      report += ',';
    }
    first = false;
    report +=
        "{\"material\":" + mh::common::json_string(material.material_name);
    report += ",\"logical_id\":" + mh::common::json_string(material.logical_id);
    const auto use_override = options.hd_enabled && material.override_used;
    const auto &image =
        use_override ? *material.override_image : material.image;
    report += ",\"width\":" + std::to_string(image.width);
    report += ",\"height\":" + std::to_string(image.height);
    report += ",\"override_available\":" +
              std::string(material.override_used ? "true" : "false");
    report +=
        ",\"override\":" + std::string(use_override ? "true" : "false") + '}';
  }
  report += "],\"passed\":true}\n";
  return report;
}

void write_report(const std::filesystem::path &path,
                  const std::string &report) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(report.data(), static_cast<std::streamsize>(report.size()));
  if (!output) {
    throw std::runtime_error("failed to write model-viewer report");
  }
}

int run_report_only(Options options) {
  require(SDL_Init(SDL_INIT_VIDEO), "initialize SDL for software report");
  struct QuitSdl {
    ~QuitSdl() { SDL_Quit(); }
  } quit_sdl;
  SdlPointer<SDL_Surface, SDL_DestroySurface> surface(
      SDL_CreateSurface(960, 720, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
  require(surface != nullptr, "create report surface");
  SdlPointer<SDL_Renderer, SDL_DestroyRenderer> renderer(
      SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer);
  require(renderer != nullptr, "create software renderer");
  const auto title_font = load_viewer_font(
      renderer.get(), checked_content_path(options, "Data/FONT1.FNT"));
  const auto pause_font = load_viewer_font(
      renderer.get(), checked_content_path(options, "Data/FONT1S.FNT"));
  UiTextCache ui_text_cache;
  auto scene = load_scene(renderer.get(), options);
  auto effective_route_group = options.route_group;
  if (options.route_sample.has_value()) {
    effective_route_group = static_cast<std::size_t>(
        scene.world_route.samples[*options.route_sample].group_index);
  }
  if (scene.world_scene && options.view == "inspect" &&
      !options.local_cell.has_value() && !effective_route_group.has_value()) {
    options.view = "top";
  }
  if (options.local_cell.has_value() || effective_route_group.has_value()) {
    options.view = "inspect";
  }
  options.hd_enabled = scene.override_materials != 0U;
  const auto [yaw, pitch] =
      options.route_sample.has_value()
          ? route_sample_camera_angles(scene, *options.route_sample,
                                       options.route_chase)
      : effective_route_group.has_value()
          ? route_camera_angles(scene, *effective_route_group,
                                options.route_chase)
          : camera_angles(options.view);
  DepthRenderResources depth_resources;
  const auto render_stats = render_scene(
      renderer.get(), title_font, pause_font, ui_text_cache, scene, surface->w,
      surface->h,
      yaw, pitch, false, false,
      true, options.collision_overlay, options.lighting_overlay,
      options.applied_lighting, options.route_overlay, options.hd_enabled,
      options.depth_buffer, options.car_shading, depth_resources, options.initial_zoom, 0.0F,
      0.0F,
      options.local_cell, effective_route_group, options.route_sample, false,
      options.route_chase);
  require(SDL_RenderPresent(renderer.get()), "finish software frame");
  if (options.screenshot_path.has_value()) {
    if (options.screenshot_path->has_parent_path()) {
      std::filesystem::create_directories(
          options.screenshot_path->parent_path());
    }
    require(
        SDL_SaveBMP(surface.get(), options.screenshot_path->string().c_str()),
        "save model viewer screenshot");
  }
  const auto report = make_report(options, scene, "software", 1U,
                                  hash_surface(*surface), render_stats);
  std::cout << report;
  if (options.report_path.has_value()) {
    write_report(*options.report_path, report);
  }
  return 0;
}

int run_interactive(Options options) {
  require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD),
          "initialize SDL model viewer");
  struct QuitSdl {
    ~QuitSdl() { SDL_Quit(); }
  } quit_sdl;
  // Resource identifier 1 is the original Motorhead icon embedded by
  // resources.rc. Use it for the title bar and taskbar as well as the EXE.
  SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
  SdlPointer<SDL_Window, SDL_DestroyWindow> window(
      SDL_CreateWindow("Motorhead - Model Viewer", 1280, 720,
                       SDL_WINDOW_RESIZABLE),
      SDL_DestroyWindow);
  require(window != nullptr, "create model viewer window");
#ifdef _WIN32
#endif
  require(SDL_SetWindowMinimumSize(window.get(), 1000, 640),
          "set model viewer minimum window size");
  SdlPointer<SDL_Renderer, SDL_DestroyRenderer> renderer(
      SDL_CreateGPURenderer(nullptr, window.get()), SDL_DestroyRenderer);
  require(renderer != nullptr, "create model viewer GPU renderer");
  const auto title_font = load_viewer_font(
      renderer.get(), checked_content_path(options, "Data/FONT1.FNT"));
  const auto pause_font = load_viewer_font(
      renderer.get(), checked_content_path(options, "Data/FONT1S.FNT"));
  UiTextCache ui_text_cache;
  auto scene = load_scene(renderer.get(), options);
  auto initial_route_group = options.route_group;
  if (options.route_sample.has_value()) {
    initial_route_group = static_cast<std::size_t>(
        scene.world_route.samples[*options.route_sample].group_index);
  }
  if (scene.world_scene && options.view == "inspect" &&
      !options.local_cell.has_value() && !initial_route_group.has_value()) {
    options.view = "top";
  }
  if (options.local_cell.has_value() || initial_route_group.has_value()) {
    options.view = "inspect";
  }
  options.hd_enabled = scene.override_materials != 0U;
  auto [yaw, pitch] =
      options.route_sample.has_value()
          ? route_sample_camera_angles(scene, *options.route_sample,
                                       options.route_chase)
      : initial_route_group.has_value()
          ? route_camera_angles(scene, *initial_route_group,
                                options.route_chase)
          : camera_angles(options.view);
  bool wireframe = false;
  bool normals = false;
  bool textures = true;
  bool collision_overlay = options.collision_overlay;
  bool lighting_overlay = options.lighting_overlay;
  bool applied_lighting = options.applied_lighting;
  bool route_overlay = options.route_overlay;
  bool route_playback = false;
  bool route_chase = options.route_chase;
  bool depth_buffer = options.depth_buffer;
  bool rotate_dragging = false;
  bool pan_dragging = false;
  bool show_help = false;
  bool shading_dropdown_open = false;
  bool running = true;
  float zoom = options.initial_zoom;
  float pan_x = 0.0F;
  float pan_y = 0.0F;
  std::uint32_t frames = 0U;
  std::optional<std::uint64_t> readback_hash;
  std::optional<std::size_t> active_local_cell = options.local_cell;
  std::optional<std::size_t> active_route_group = initial_route_group;
  std::optional<std::size_t> active_route_sample = options.route_sample;
  if (active_route_group.has_value() && !active_route_sample.has_value()) {
    active_route_sample = route_group_sample_index(scene, *active_route_group);
  }
  auto last_route_advance = SDL_GetTicks();
  DepthRenderResources depth_resources;
  std::size_t local_cell_cursor = 0U;
  bool auto_rotate = !active_local_cell.has_value() &&
                     !active_route_group.has_value() &&
                     !active_route_sample.has_value();
  auto last_auto_rotate = SDL_GetTicks();
  if (active_local_cell.has_value()) {
    const auto selected_column = *active_local_cell % scene.world_grid_width;
    const auto selected_row = *active_local_cell / scene.world_grid_width;
    std::size_t best_distance = std::numeric_limits<std::size_t>::max();
    for (std::size_t index = 0U; index < scene.world_section_cells.size();
         ++index) {
      const auto candidate = scene.world_section_cells[index];
      const auto column = candidate % scene.world_grid_width;
      const auto row = candidate / scene.world_grid_width;
      const auto dx = column > selected_column ? column - selected_column
                                               : selected_column - column;
      const auto dy =
          row > selected_row ? row - selected_row : selected_row - row;
      const auto distance = dx * dx + dy * dy;
      if (distance < best_distance) {
        best_distance = distance;
        local_cell_cursor = index;
      }
    }
  }
  const auto select_adjacent = [&](const bool previous) {
    if (scene.world_scene) {
      const auto &source = adjacent_track_source(options, previous);
      options.model_relative_path = source.model_relative_path;
      options.track_definition_relative_path = source.definition_relative_path;
      options.track = source.texture_track;
      options.view = "top";
    } else {
      const auto &source = adjacent_vehicle_source(options, previous);
      options.model_relative_path = source.model_relative_path;
      options.car_definition_relative_path = source.definition_relative_path;
      options.track_definition_relative_path.reset();
      options.view = "inspect";
    }
    options.local_cell.reset();
    options.route_group.reset();
    options.route_sample.reset();
    options.route_chase = false;
    options.route_overlay = false;
    options.lighting_overlay = false;
    active_local_cell.reset();
    active_route_group.reset();
    active_route_sample.reset();
    route_playback = false;
    route_chase = false;
    route_overlay = false;
    lighting_overlay = false;
    local_cell_cursor = 0U;
    scene = load_scene(renderer.get(), options);
    options.hd_enabled = scene.override_materials != 0U;
    const auto angles = camera_angles(options.view);
    yaw = angles.first;
    pitch = angles.second;
    zoom = 1.0F;
    pan_x = 0.0F;
    pan_y = 0.0F;
    rotate_dragging = false;
    pan_dragging = false;
    shading_dropdown_open = false;
    readback_hash.reset();
    auto_rotate = true;
    last_auto_rotate = SDL_GetTicks();
  };
  RenderStats render_stats;
  while (running) {
    const auto frame_started = SDL_GetTicks();
    const auto auto_rotate_elapsed =
        std::min<std::uint64_t>(frame_started - last_auto_rotate, 100U);
    last_auto_rotate = frame_started;
    if (auto_rotate) {
      constexpr float auto_rotate_radians_per_millisecond = 0.00012F;
      yaw -= static_cast<float>(auto_rotate_elapsed) *
             auto_rotate_radians_per_millisecond;
    }
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT) {
        running = false;
      } else if (event.type == SDL_EVENT_KEY_DOWN) {
        const auto repeated_navigation_key =
            event.key.key == SDLK_UP || event.key.key == SDLK_DOWN ||
            event.key.key == SDLK_PLUS || event.key.key == SDLK_EQUALS ||
            event.key.key == SDLK_KP_PLUS || event.key.key == SDLK_MINUS ||
            event.key.key == SDLK_KP_MINUS || event.key.key == SDLK_PAGEUP ||
            event.key.key == SDLK_PAGEDOWN;
        if (event.key.repeat && !repeated_navigation_key) {
          continue;
        }
        if (event.key.key == SDLK_ESCAPE) {
          if (shading_dropdown_open) {
            shading_dropdown_open = false;
          } else if (show_help) {
            show_help = false;
          } else {
            running = false;
          }
        } else if (event.key.key == SDLK_QUESTION) {
          show_help = !show_help;
          shading_dropdown_open = false;
        } else if (event.key.key == SDLK_W) {
          wireframe = !wireframe;
        } else if (event.key.key == SDLK_N) {
          normals = !normals;
        } else if (event.key.key == SDLK_T) {
          textures = !textures;
        } else if (event.key.key == SDLK_LEFT ||
                   event.key.key == SDLK_RIGHT) {
          select_adjacent(event.key.key == SDLK_LEFT);
        } else if (event.key.key == SDLK_M) {
          const auto switch_to_tracks = !scene.world_scene;
          options.model_relative_path =
              switch_to_tracks ? std::filesystem::path("--tracks")
                               : std::filesystem::path("--cars");
          options.track_definition_relative_path.reset();
          options.local_cell.reset();
          options.route_group.reset();
          options.route_sample.reset();
          options.route_chase = false;
          options.view = switch_to_tracks ? "top" : "inspect";
          active_local_cell.reset();
          active_route_group.reset();
          active_route_sample.reset();
          route_playback = false;
          route_chase = false;
          route_overlay = false;
          options.route_overlay = false;
          lighting_overlay = false;
          options.lighting_overlay = false;
          local_cell_cursor = 0U;
          resolve_vehicle_roster_default(options);
          resolve_track_roster_default(options);
          scene = load_scene(renderer.get(), options);
          options.hd_enabled = scene.override_materials != 0U;
          const auto angles = camera_angles(options.view);
          yaw = angles.first;
          pitch = angles.second;
          zoom = 1.0F;
          pan_x = 0.0F;
          pan_y = 0.0F;
          rotate_dragging = false;
          pan_dragging = false;
          auto_rotate = true;
          last_auto_rotate = SDL_GetTicks();
        } else if (event.key.key == SDLK_C) {
          collision_overlay = !collision_overlay;
        } else if (event.key.key == SDLK_L && scene.world_scene) {
          lighting_overlay = !lighting_overlay;
          options.lighting_overlay = lighting_overlay;
        } else if (event.key.key == SDLK_S && scene.assembled_car) {
          options.car_shading = static_cast<CarShading>(
              (static_cast<std::size_t>(options.car_shading) + 1U) %
              car_shading_names.size());
          shading_dropdown_open = false;
        } else if (event.key.key == SDLK_P && scene.world_scene &&
                   !scene.world_route.samples.empty()) {
          route_overlay = !route_overlay;
          options.route_overlay = route_overlay;
        } else if (event.key.key == SDLK_V && scene.world_scene &&
                   !scene.world_route.groups.empty()) {
          auto_rotate = false;
          route_playback = false;
          if (active_route_group.has_value()) {
            route_chase = false;
            options.route_chase = false;
            active_route_group.reset();
            active_route_sample.reset();
            options.route_group.reset();
            options.route_sample.reset();
            options.view = "top";
            const auto angles = camera_angles(options.view);
            yaw = angles.first;
            pitch = angles.second;
          } else {
            route_chase = true;
            options.route_chase = true;
            auto group = scene.world_route.groups.size() - 1U;
            active_route_group = adjacent_route_group(scene, group, false);
            options.route_group = active_route_group;
            active_route_sample =
                route_group_sample_index(scene, *active_route_group);
            options.route_sample = active_route_sample;
            active_local_cell.reset();
            options.local_cell.reset();
            options.view = "inspect";
            route_overlay = true;
            options.route_overlay = true;
            const auto angles = route_sample_camera_angles(
                scene, *active_route_sample, route_chase);
            yaw = angles.first;
            pitch = angles.second;
          }
          zoom = 1.0F;
          pan_x = 0.0F;
          pan_y = 0.0F;
        } else if (event.key.key == SDLK_F && scene.world_scene &&
                   !scene.world_section_cells.empty()) {
          auto_rotate = false;
          route_playback = false;
          if (active_route_group.has_value()) {
            route_chase = false;
            options.route_chase = false;
            active_route_group.reset();
            active_route_sample.reset();
            options.route_group.reset();
            options.route_sample.reset();
            options.view = "top";
          } else if (active_local_cell.has_value()) {
            active_local_cell.reset();
            options.local_cell.reset();
            options.view = "top";
          } else {
            active_local_cell = scene.world_section_cells[local_cell_cursor];
            options.local_cell = active_local_cell;
            options.view = "inspect";
          }
          const auto angles = camera_angles(options.view);
          yaw = angles.first;
          pitch = angles.second;
          zoom = 1.0F;
          pan_x = 0.0F;
          pan_y = 0.0F;
        } else if ((event.key.key == SDLK_PAGEUP ||
                    event.key.key == SDLK_PAGEDOWN) &&
                   (active_route_group.has_value() ||
                    (active_local_cell.has_value() &&
                     !scene.world_section_cells.empty()))) {
          auto_rotate = false;
          if (active_route_group.has_value()) {
            route_playback = false;
            active_route_group = adjacent_route_group(
                scene, *active_route_group, event.key.key == SDLK_PAGEUP);
            options.route_group = active_route_group;
            active_route_sample =
                route_group_sample_index(scene, *active_route_group);
            options.route_sample = active_route_sample;
            const auto angles = route_sample_camera_angles(
                scene, *active_route_sample, route_chase);
            yaw = angles.first;
            pitch = angles.second;
          } else if (event.key.key == SDLK_PAGEUP) {
            local_cell_cursor = local_cell_cursor == 0U
                                    ? scene.world_section_cells.size() - 1U
                                    : local_cell_cursor - 1U;
          } else {
            local_cell_cursor =
                (local_cell_cursor + 1U) % scene.world_section_cells.size();
          }
          if (!active_route_group.has_value()) {
            active_local_cell = scene.world_section_cells[local_cell_cursor];
            options.local_cell = active_local_cell;
          }
          pan_x = 0.0F;
          pan_y = 0.0F;
        } else if (event.key.key == SDLK_SPACE &&
                   active_route_sample.has_value()) {
          route_playback = !route_playback;
          last_route_advance = SDL_GetTicks();
        } else if (event.key.key == SDLK_B &&
                   active_route_sample.has_value()) {
          auto_rotate = false;
          route_chase = !route_chase;
          options.route_chase = route_chase;
          const auto angles = route_sample_camera_angles(
              scene, *active_route_sample, route_chase);
          yaw = angles.first;
          pitch = angles.second;
          zoom = 1.0F;
          pan_x = 0.0F;
          pan_y = 0.0F;
        } else if (event.key.key == SDLK_B) {
          auto_rotate = false;
          options.view = "rear";
          const auto angles = camera_angles(options.view);
          yaw = angles.first;
          pitch = angles.second;
        } else if (event.key.key == SDLK_R) {
          options.view = scene.world_scene
                             ? active_local_cell.has_value() ||
                                       active_route_group.has_value()
                                   ? "inspect"
                                   : "top"
                             : "inspect";
          const auto angles =
              active_route_sample.has_value()
                  ? route_sample_camera_angles(scene, *active_route_sample,
                                               route_chase)
                  : camera_angles(options.view);
          yaw = angles.first;
          pitch = angles.second;
          zoom = 1.0F;
          pan_x = 0.0F;
          pan_y = 0.0F;
          auto_rotate = !active_local_cell.has_value() &&
                        !active_route_group.has_value() &&
                        !active_route_sample.has_value();
          last_auto_rotate = SDL_GetTicks();
        } else if (event.key.key == SDLK_PLUS || event.key.key == SDLK_EQUALS ||
                   event.key.key == SDLK_KP_PLUS) {
          auto_rotate = false;
          zoom = std::min(8.0F, zoom * 1.15F);
        } else if (event.key.key == SDLK_MINUS ||
                   event.key.key == SDLK_KP_MINUS) {
          auto_rotate = false;
          zoom = std::max(0.35F, zoom / 1.15F);
        } else if (event.key.key == SDLK_UP) {
          auto_rotate = false;
          pitch = std::max(camera_minimum_pitch, pitch - 0.08F);
        } else if (event.key.key == SDLK_DOWN) {
          auto_rotate = false;
          pitch = std::min(camera_maximum_pitch, pitch + 0.08F);
        }
      } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                 event.button.button == SDL_BUTTON_LEFT) {
        require(SDL_ConvertEventToRenderCoordinates(renderer.get(), &event),
                "map help-toggle click to renderer coordinates");
        int output_width = 0;
        int output_height = 0;
        require(SDL_GetRenderOutputSize(renderer.get(), &output_width,
                                        &output_height),
                "query help-toggle hit area");
        const auto footer_y =
            static_cast<float>(output_height) - viewer_footer_height;
        const auto over_help_toggle =
            event.button.x >=
                static_cast<float>(output_width) -
                    viewer_help_toggle_right_margin -
                    viewer_help_toggle_width &&
            event.button.x <=
                static_cast<float>(output_width) -
                    viewer_help_toggle_right_margin &&
            event.button.y >= footer_y;
        const auto in_footer = event.button.y >= footer_y;
        const auto in_pill = [&](const float x, const float pill_width) {
          return event.button.x >= x && event.button.x <= x + pill_width &&
                 event.button.y >= footer_y + 10.0F &&
                 event.button.y <= footer_y + 48.0F;
        };
        auto pill_x = viewer_content_left;
        const auto over_textures =
            in_pill(pill_x, viewer_textures_pill_width);
        pill_x += viewer_textures_pill_width + viewer_pill_gap;
        const auto over_solid = in_pill(pill_x, viewer_solid_pill_width);
        pill_x += viewer_solid_pill_width + viewer_pill_gap;
        const auto over_wireframe =
            in_pill(pill_x, viewer_wireframe_pill_width);
        pill_x += viewer_wireframe_pill_width + viewer_pill_gap;
        const auto over_normals = in_pill(pill_x, viewer_normals_pill_width);
        pill_x += viewer_normals_pill_width + viewer_pill_gap;
        const auto over_collision =
            in_pill(pill_x, viewer_collision_pill_width);
        const auto shading_x = static_cast<float>(output_width) - 149.0F;
        const auto over_shading =
            event.button.x >= shading_x &&
            event.button.x <= shading_x + 120.0F &&
            event.button.y >= 57.0F && event.button.y <= 84.0F;
        std::optional<std::size_t> shading_option;
        const auto shading_option_count =
            scene.assembled_car ? car_shading_names.size() : 2U;
        if (shading_dropdown_open && event.button.x >= shading_x &&
            event.button.x <= shading_x + 120.0F &&
            event.button.y >= 87.0F &&
            event.button.y <
                87.0F + 24.0F * static_cast<float>(shading_option_count)) {
          shading_option = static_cast<std::size_t>(
              (event.button.y - 87.0F) / 24.0F);
          if (*shading_option >= shading_option_count) {
            shading_option.reset();
          }
        }
        auto over_help_close = false;
        if (show_help) {
          const auto help_width = std::min(
              430.0F, static_cast<float>(output_width) -
                          viewer_content_right_margin * 2.0F);
          const auto help_height = scene.world_scene ? 144.0F : 58.0F;
          const auto help_x = static_cast<float>(output_width) -
                              viewer_content_right_margin - help_width;
          const auto help_y =
              std::max(86.0F, footer_y - help_height - 12.0F);
          over_help_close =
              event.button.x >= help_x + help_width - 44.0F &&
              event.button.x <= help_x + help_width &&
              event.button.y >= help_y && event.button.y <= help_y + 44.0F;
        }
        if (shading_option.has_value()) {
          if (scene.assembled_car) {
            options.car_shading = static_cast<CarShading>(*shading_option);
          } else {
            applied_lighting = *shading_option == 1U;
            options.applied_lighting = applied_lighting;
          }
          shading_dropdown_open = false;
          rotate_dragging = false;
        } else if (over_help_close) {
          show_help = false;
          rotate_dragging = false;
        } else if (over_help_toggle) {
          show_help = !show_help;
          shading_dropdown_open = false;
          rotate_dragging = false;
        } else if (over_solid) {
          wireframe = false;
          normals = false;
          collision_overlay = false;
          rotate_dragging = false;
        } else if (over_wireframe) {
          wireframe = !wireframe;
          rotate_dragging = false;
        } else if (over_normals) {
          normals = !normals;
          rotate_dragging = false;
        } else if (over_collision) {
          collision_overlay = !collision_overlay;
          rotate_dragging = false;
        } else if (over_textures) {
          textures = !textures;
          rotate_dragging = false;
        } else if (over_shading) {
          shading_dropdown_open = !shading_dropdown_open;
          rotate_dragging = false;
        } else if (shading_dropdown_open) {
          shading_dropdown_open = false;
          rotate_dragging = false;
        } else if (in_footer) {
          rotate_dragging = false;
        } else {
          auto_rotate = false;
          rotate_dragging = true;
        }
      } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                 event.button.button == SDL_BUTTON_LEFT) {
        rotate_dragging = false;
      } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                 (event.button.button == SDL_BUTTON_MIDDLE ||
                  event.button.button == SDL_BUTTON_RIGHT)) {
        auto_rotate = false;
        pan_dragging = true;
      } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                 (event.button.button == SDL_BUTTON_MIDDLE ||
                  event.button.button == SDL_BUTTON_RIGHT)) {
        pan_dragging = false;
      } else if (event.type == SDL_EVENT_MOUSE_MOTION) {
        if (rotate_dragging) {
          yaw -= event.motion.xrel * 0.008F;
          pitch = std::clamp(pitch + event.motion.yrel * 0.008F,
                             camera_minimum_pitch, camera_maximum_pitch);
        }
        if (pan_dragging) {
          pan_x += event.motion.xrel;
          pan_y += event.motion.yrel;
        }
      } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        if (event.wheel.y != 0.0F) {
          auto_rotate = false;
        }
        if (event.wheel.y > 0.0F) {
          zoom = std::min(8.0F, zoom * 1.15F);
        } else if (event.wheel.y < 0.0F) {
          zoom = std::max(0.35F, zoom / 1.15F);
        }
      }
    }
    if (route_playback && active_route_sample.has_value()) {
      constexpr std::uint64_t route_step_milliseconds = 40U;
      constexpr std::uint64_t maximum_catch_up_steps = 8U;
      const auto now = SDL_GetTicks();
      const auto elapsed = now - last_route_advance;
      const auto due_steps = elapsed / route_step_milliseconds;
      if (due_steps != 0U) {
        const auto steps = std::min(due_steps, maximum_catch_up_steps);
        for (std::uint64_t step = 0U; step < steps; ++step) {
          active_route_sample =
              adjacent_route_sample(scene, *active_route_sample, false);
        }
        active_route_group = static_cast<std::size_t>(
            scene.world_route.samples[*active_route_sample].group_index);
        options.route_sample = active_route_sample;
        options.route_group = active_route_group;
        const auto angles = route_sample_camera_angles(
            scene, *active_route_sample, route_chase);
        yaw = angles.first;
        pitch = angles.second;
        pan_x = 0.0F;
        pan_y = 0.0F;
        last_route_advance =
            due_steps > maximum_catch_up_steps
                ? now
                : last_route_advance + steps * route_step_milliseconds;
      }
    }
    int width = 0;
    int height = 0;
    require(SDL_GetRenderOutputSize(renderer.get(), &width, &height),
            "query model viewer output size");
    render_stats = render_scene(
        renderer.get(), title_font, pause_font, ui_text_cache, scene, width,
        height, yaw,
        pitch, wireframe, normals, textures, collision_overlay, lighting_overlay,
        applied_lighting, route_overlay, options.hd_enabled, depth_buffer,
        options.car_shading, depth_resources, zoom, pan_x, pan_y,
        active_local_cell, active_route_group, active_route_sample,
        route_playback, route_chase, show_help, shading_dropdown_open);
    if (!readback_hash.has_value()) {
      SdlPointer<SDL_Surface, SDL_DestroySurface> readback(
          SDL_RenderReadPixels(renderer.get(), nullptr), SDL_DestroySurface);
      require(readback != nullptr, "read model viewer frame");
      readback_hash = hash_surface(*readback);
    }
    require(SDL_RenderPresent(renderer.get()), "present model viewer frame");
    ++frames;
    if (options.maximum_frames != 0U && frames >= options.maximum_frames) {
      running = false;
    }
    constexpr std::uint64_t target_frame_milliseconds = 16U;
    const auto frame_elapsed = SDL_GetTicks() - frame_started;
    if (frame_elapsed < target_frame_milliseconds) {
      SDL_Delay(static_cast<std::uint32_t>(target_frame_milliseconds -
                                           frame_elapsed));
    }
  }
  const auto *renderer_value = SDL_GetRendererName(renderer.get());
  const auto report = make_report(
      options, scene, renderer_value == nullptr ? "unknown" : renderer_value,
      frames, readback_hash.value_or(0U), render_stats);
  std::cout << report;
  if (options.report_path.has_value()) {
    write_report(*options.report_path, report);
  }
  return 0;
}

} // namespace

int mh::viewer::run(const int argc, char **argv) {
  try {
    std::vector<std::string> default_arguments;
    std::vector<char *> default_argument_pointers;
    int effective_argc = argc;
    char **effective_argv = argv;
    const auto first_argument_is_option =
        argc >= 2 && std::string_view(argv[1]).starts_with("--");
    if (argc == 1 || first_argument_is_option || argc == 2) {
      std::filesystem::path executable_directory;
      if (const auto *base_path = SDL_GetBasePath();
          base_path != nullptr && *base_path != '\0') {
        executable_directory =
            std::filesystem::path(base_path).lexically_normal();
      } else {
        executable_directory =
            std::filesystem::absolute(argv[0]).lexically_normal().parent_path();
      }
      const auto is_content_root = [](const std::filesystem::path &root) {
        return std::filesystem::is_directory(root / "Game") &&
               std::filesystem::is_directory(root / "Cars") &&
               std::filesystem::is_directory(root / "Tracks");
      };
      std::filesystem::path content_root;
      if (argc == 2 && !first_argument_is_option) {
        content_root = std::filesystem::absolute(argv[1]).lexically_normal();
      } else {
        if (is_content_root(executable_directory)) {
          content_root = executable_directory.lexically_normal();
        }
      }
      if (content_root.empty() || !is_content_root(content_root)) {
        throw std::runtime_error(
            "runtime data was not found beside Modelviewer.exe; "
            "expected the original Game, Cars, and Tracks folders "
            "beside it");
      }
      auto mode = std::string("--cars");
      if (first_argument_is_option) {
        for (int index = 1; index < argc; ++index) {
          const std::string_view argument(argv[index]);
          if (argument == "--cars" || argument == "--tracks") {
            mode = argument;
          }
        }
      }
      default_arguments = {argv[0], content_root.string()};
      default_arguments.emplace_back(std::move(mode));
      default_arguments.insert(default_arguments.end(),
                               {"--track", "track1", "--pack", "tex1"});
      if (first_argument_is_option) {
        for (int index = 1; index < argc; ++index) {
          const std::string_view argument(argv[index]);
          if (argument != "--cars" && argument != "--tracks") {
            default_arguments.emplace_back(argv[index]);
          }
        }
      }
      const auto override_root =
          executable_directory / "User" / "viewer-overrides";
      if (std::filesystem::is_directory(override_root)) {
        default_arguments.insert(default_arguments.end(),
                                 {"--overrides", override_root.string()});
      }
      default_argument_pointers.reserve(default_arguments.size());
      for (auto &argument : default_arguments) {
        default_argument_pointers.push_back(argument.data());
      }
      effective_argc = static_cast<int>(default_argument_pointers.size());
      effective_argv = default_argument_pointers.data();
    }
    auto options = parse_options(effective_argc, effective_argv);
    resolve_vehicle_roster_default(options);
    resolve_track_roster_default(options);
    if (options.report_only) {
      return run_report_only(std::move(options));
    }
#ifdef _WIN32
    const auto instance = CreateMutexW(
        nullptr, FALSE, L"Local\\MotorheadModelViewer");
    if (instance == nullptr) {
      throw std::runtime_error("cannot create model-viewer instance guard");
    }
    struct CloseInstance {
      HANDLE handle;
      ~CloseInstance() { CloseHandle(handle); }
    } close_instance{instance};
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      return 0;
    }
#endif
    return run_interactive(std::move(options));
  } catch (const std::exception &error) {
    std::cerr << "ModelViewer: " << error.what() << '\n';
    return 1;
  }
}
