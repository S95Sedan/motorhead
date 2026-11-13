#include <renderer/hardware/d3d9_projection_shaders.hpp>
#include <renderer/hardware/d3d9_renderer.hpp>

#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#include <d3d9.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mh::render::hardware {
namespace {

template <typename T> class ComPtr {
public:
  ComPtr() = default;
  explicit ComPtr(T *value) : value_(value) {}
  ~ComPtr() { reset(); }
  ComPtr(const ComPtr &) = delete;
  ComPtr &operator=(const ComPtr &) = delete;
  ComPtr(ComPtr &&other) noexcept
      : value_(std::exchange(other.value_, nullptr)) {}
  ComPtr &operator=(ComPtr &&other) noexcept {
    if (this != &other)
      reset(std::exchange(other.value_, nullptr));
    return *this;
  }
  T *get() const noexcept { return value_; }
  T **put() noexcept {
    reset();
    return &value_;
  }
  void reset(T *value = nullptr) noexcept {
    if (value_ != nullptr)
      value_->Release();
    value_ = value;
  }
  explicit operator bool() const noexcept { return value_ != nullptr; }

private:
  T *value_ = nullptr;
};

void require_hr(const HRESULT hr, const char *operation) {
  if (FAILED(hr)) {
    throw std::runtime_error(std::string(operation) + " failed (HRESULT " +
                             std::to_string(static_cast<unsigned long>(hr)) +
                             ")");
  }
}

struct Vertex {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
  float rhw = 1.0F;
  D3DCOLOR color = 0xffffffffU;
  float u = 0.0F;
  float v = 0.0F;
};
constexpr DWORD vertex_fvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
// D3D9's pre-transformed raster convention places pixel centres at integer
// minus one half. The shared scene coordinates use modern integer pixel
// centres, so every XYZRHW submission needs this backend-only correction.
// Without it, adjacent road triangles expose one-pixel joins and the sampled
// depth field used by projected shadows is displaced from the colour image.
constexpr float d3d9_pixel_center_offset = -0.5F;

struct ProjectionConstants {
  std::array<float, 4U> output_and_projection{};
  std::array<float, 4U> view_position{};
  std::array<float, 4U> view_right{};
  std::array<float, 4U> view_up{};
  std::array<float, 4U> view_forward{};
  std::array<float, 4U> world_bounds{};
  std::array<float, 4U> cue_parameters{};
  std::array<float, 4U> cue_color{};
  std::array<float, 4U> vehicle_position{};
  std::array<float, 4U> body_basis_0{};
  std::array<float, 4U> body_basis_1{};
  std::array<float, 4U> body_basis_2{};
  std::array<float, 4U> source_0_center{};
  std::array<float, 4U> source_0_direction{};
  std::array<float, 4U> source_1_center{};
  std::array<float, 4U> source_1_direction{};
  std::array<std::array<float, 4U>, 18U> cell_xz{};
  std::array<std::array<float, 4U>, 9U> cell_y_valid{};
};

std::array<float, 4U> vector4(const mh::game::CollisionVector3 &source,
                              const float fourth = 0.0F) {
  return {static_cast<float>(source[0U]), static_cast<float>(source[1U]),
          static_cast<float>(source[2U]), fourth};
}

D3DCOLOR pack_color(const SDL_FColor &color) {
  const auto byte = [](const float value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F +
                                     0.5F);
  };
  return D3DCOLOR_ARGB(byte(color.a), byte(color.r), byte(color.g),
                       byte(color.b));
}

SDL_FColor shade(const SceneRasterCommand &command, const std::size_t index,
                 const SceneFrameView &frame) {
  const auto &base = command.vertices[index].color;
  const auto cue = command.fog_cues[index];
  if (command.blend_mode == SceneBlendMode::additive) {
    return {base.r * (1.0F - cue), base.g * (1.0F - cue), base.b * (1.0F - cue),
            base.a};
  }
  return {base.r * (1.0F - cue) + frame.fog_color[0U] / 255.0F * cue,
          base.g * (1.0F - cue) + frame.fog_color[1U] / 255.0F * cue,
          base.b * (1.0F - cue) + frame.fog_color[2U] / 255.0F * cue, base.a};
}

bool project(const PerspectiveView &view,
             const mh::game::CollisionVector3 &world, SDL_FPoint &screen,
             float &depth) {
  std::array<double, 3U> delta{};
  for (std::size_t i = 0; i < 3U; ++i)
    delta[i] = world[i] - view.position[i];
  const auto dot = [&delta](const mh::game::CollisionVector3 &axis) {
    return delta[0] * axis[0] + delta[1] * axis[1] + delta[2] * axis[2];
  };
  const auto z = dot(view.forward);
  if (!std::isfinite(z) || z <= view.near_plane)
    return false;
  screen.x = static_cast<float>(view.center_x +
                                dot(view.right) / z * view.focal_length);
  screen.y =
      static_cast<float>(view.center_y - dot(view.up) / z * view.focal_length);
  depth = static_cast<float>(z);
  return std::isfinite(screen.x) && std::isfinite(screen.y);
}

} // namespace

struct D3D9Renderer::Impl {
  struct BatchState {
    const std::vector<mh::content::PamRgbaImage> *texture_levels = nullptr;
    SceneBlendMode blend = SceneBlendMode::opaque;
    bool trilinear = false;
    bool depth_test = true;
    bool depth_bias = false;
  };

  struct DrawBatch {
    BatchState state;
    std::size_t first_vertex = 0U;
    std::size_t vertex_count = 0U;
  };

  SDL_Renderer *owner = nullptr;
  ComPtr<IDirect3DDevice9> device;
  ComPtr<IDirect3DVertexBuffer9> vertex_buffer;
  ComPtr<IDirect3DTexture9> sampled_depth;
  ComPtr<IDirect3DSurface9> depth_surface;
  ComPtr<IDirect3DPixelShader9> shadow_shader;
  ComPtr<IDirect3DPixelShader9> headlight_shader;
  ComPtr<IDirect3DStateBlock9> saved_state;
  bool depth_sampling = false;
  UINT depth_width = 0U;
  UINT depth_height = 0U;
  double render_ms = 0.0;
  std::unordered_map<const std::vector<mh::content::PamRgbaImage> *,
                     ComPtr<IDirect3DTexture9>>
      textures;
  std::vector<Vertex> batch_vertices;
  std::vector<DrawBatch> draw_batches;
  std::vector<const SceneRasterCommand *> ordered_commands;
  std::size_t vertex_capacity = 0U;

  void ensure_depth(const UINT width, const UINT height) {
    if (depth_surface && depth_width == width && depth_height == height)
      return;
    sampled_depth.reset();
    depth_surface.reset();
    constexpr auto intz =
        static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));
    auto hr = E_FAIL;
    if (shadow_shader && headlight_shader) {
      hr = device.get()->CreateTexture(width, height, 1U, D3DUSAGE_DEPTHSTENCIL,
                                       intz, D3DPOOL_DEFAULT,
                                       sampled_depth.put(), nullptr);
    }
    if (SUCCEEDED(hr)) {
      require_hr(sampled_depth.get()->GetSurfaceLevel(0U, depth_surface.put()),
                 "query D3D9 sampled depth surface");
      depth_sampling = true;
    } else {
      depth_sampling = false;
      hr = device.get()->CreateDepthStencilSurface(
          width, height, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0U, TRUE,
          depth_surface.put(), nullptr);
      if (FAILED(hr)) {
        require_hr(device.get()->CreateDepthStencilSurface(
                       width, height, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0U, TRUE,
                       depth_surface.put(), nullptr),
                   "create D3D9 depth surface");
      }
    }
    depth_width = width;
    depth_height = height;
  }

  IDirect3DTexture9 *
  texture_for(const std::vector<mh::content::PamRgbaImage> *levels) {
    if (levels == nullptr || levels->empty())
      return nullptr;
    if (const auto found = textures.find(levels); found != textures.end())
      return found->second.get();
    const auto &image = levels->front();
    ComPtr<IDirect3DTexture9> texture;
    require_hr(device.get()->CreateTexture(image.width, image.height,
                                           static_cast<UINT>(levels->size()),
                                           0U, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                                           texture.put(), nullptr),
               "create D3D9 scene texture");
    for (std::size_t level = 0U; level < levels->size(); ++level) {
      const auto &level_image = levels->at(level);
      D3DLOCKED_RECT locked{};
      require_hr(texture.get()->LockRect(static_cast<UINT>(level), &locked,
                                         nullptr, 0U),
                 "lock D3D9 scene texture level");
      for (std::uint32_t y = 0; y < level_image.height; ++y) {
        auto *destination = static_cast<std::uint8_t *>(locked.pBits) +
                            static_cast<std::size_t>(y) * locked.Pitch;
        const auto *source =
            level_image.rgba.data() +
            static_cast<std::size_t>(y) * level_image.width * 4U;
        for (std::uint32_t x = 0; x < level_image.width; ++x) {
          destination[x * 4U + 0U] = source[x * 4U + 2U];
          destination[x * 4U + 1U] = source[x * 4U + 1U];
          destination[x * 4U + 2U] = source[x * 4U + 0U];
          destination[x * 4U + 3U] = source[x * 4U + 3U];
        }
      }
      require_hr(texture.get()->UnlockRect(static_cast<UINT>(level)),
                 "unlock D3D9 scene texture level");
    }
    auto *result = texture.get();
    textures.emplace(levels, std::move(texture));
    return result;
  }

  void set_blend(const SceneBlendMode mode, const bool textured) {
    device.get()->SetRenderState(D3DRS_ALPHATESTENABLE,
                                 mode == SceneBlendMode::opaque && textured);
    device.get()->SetRenderState(D3DRS_ALPHAREF, 127U);
    device.get()->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
    if (mode == SceneBlendMode::opaque) {
      device.get()->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
      device.get()->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
      device.get()->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    } else {
      device.get()->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
      device.get()->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
      device.get()->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
      device.get()->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
      device.get()->SetRenderState(D3DRS_DESTBLEND,
                                   mode == SceneBlendMode::additive
                                       ? D3DBLEND_ONE
                                       : D3DBLEND_INVSRCALPHA);
      device.get()->SetRenderState(
          D3DRS_SRCBLENDALPHA,
          mode == SceneBlendMode::additive ? D3DBLEND_ZERO : D3DBLEND_ONE);
      device.get()->SetRenderState(D3DRS_DESTBLENDALPHA,
                                   mode == SceneBlendMode::additive
                                       ? D3DBLEND_ONE
                                       : D3DBLEND_INVSRCALPHA);
    }
  }

  Vertex make_vertex(const SceneRasterCommand &command, const std::size_t index,
                      const SceneFrameView &frame, const bool tron) {
    const auto depth =
        std::max<double>(command.view_depths[index], frame.near_plane);
    const auto &source = command.vertices[index];
    auto color = tron ? SDL_FColor{0.0F, 0.0F, 0.5F, 1.0F}
                      : shade(command, index, frame);
    return {source.position.x + d3d9_pixel_center_offset,
            source.position.y + d3d9_pixel_center_offset,
            static_cast<float>(
                std::clamp(1.0 - frame.near_plane / depth, 0.0, 1.0)),
            static_cast<float>(1.0 / depth),
            pack_color(color),
            source.tex_coord.x,
            source.tex_coord.y};
  }

  void draw_commands(const std::span<const SceneRasterCommand> commands,
                     const SceneFrameView &frame, const bool tron) {
    BatchState state;
    auto &vertices = batch_vertices;
    vertices.clear();
    auto &batches = draw_batches;
    batches.clear();
    auto &ordered = ordered_commands;
    ordered.clear();
    if (ordered.capacity() < commands.size()) {
      ordered.reserve(commands.size());
    }
    for (const auto &command : commands) {
      if (!command.shadow_projection_index.has_value() &&
          command.vertex_count >= 3U && command.vertex_count <= 4U &&
          command.blend_mode == SceneBlendMode::opaque) {
        ordered.push_back(&command);
      }
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto *left,
                                                 const auto *right) {
      const std::less<const void *> pointer_less;
      if (left->texture_levels != right->texture_levels) {
        return pointer_less(left->texture_levels, right->texture_levels);
      }
      if (left->trilinear_filtering != right->trilinear_filtering) {
        return left->trilinear_filtering < right->trilinear_filtering;
      }
      if (left->depth_test != right->depth_test) {
        return left->depth_test < right->depth_test;
      }
      return left->coplanar_depth_bias < right->coplanar_depth_bias;
    });
    for (const auto &command : commands) {
      if (!command.shadow_projection_index.has_value() &&
          command.vertex_count >= 3U && command.vertex_count <= 4U &&
          command.blend_mode != SceneBlendMode::opaque) {
        ordered.push_back(&command);
      }
    }
    bool have_state = false;
    auto batch_start = std::size_t{0U};
    const auto finish_batch = [&]() {
      if (vertices.size() == batch_start)
        return;
      batches.push_back(
          {state, batch_start, vertices.size() - batch_start});
      batch_start = vertices.size();
    };
    for (const auto *command_pointer : ordered) {
      const auto &command = *command_pointer;
      const BatchState next{command.texture_levels, command.blend_mode,
                            command.trilinear_filtering, command.depth_test,
                            command.coplanar_depth_bias};
      const auto compatible =
          have_state && state.texture_levels == next.texture_levels &&
          state.blend == next.blend && state.trilinear == next.trilinear &&
          state.depth_test == next.depth_test &&
          state.depth_bias == next.depth_bias;
      if (!compatible) {
        finish_batch();
        state = next;
        have_state = true;
      }
      const auto first = make_vertex(command, 0U, frame, tron);
      const auto third = make_vertex(command, 2U, frame, tron);
      vertices.push_back(first);
      vertices.push_back(make_vertex(command, 1U, frame, tron));
      vertices.push_back(third);
      if (command.vertex_count == 4U) {
        vertices.push_back(first);
        vertices.push_back(third);
        vertices.push_back(make_vertex(command, 3U, frame, tron));
      }
    }
    finish_batch();
    if (vertices.empty()) {
      return;
    }
    if (vertices.size() >
        std::numeric_limits<UINT>::max() / sizeof(Vertex)) {
      throw std::runtime_error("D3D9 scene vertex batch is too large");
    }
    if (!vertex_buffer || vertex_capacity < vertices.size()) {
      vertex_buffer.reset();
      vertex_capacity = std::max<std::size_t>(4096U, vertex_capacity);
      while (vertex_capacity < vertices.size()) {
        vertex_capacity *= 2U;
      }
      require_hr(device.get()->CreateVertexBuffer(
                     static_cast<UINT>(vertex_capacity * sizeof(Vertex)),
                     D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, vertex_fvf,
                     D3DPOOL_DEFAULT, vertex_buffer.put(), nullptr),
                 "create D3D9 scene vertex buffer");
    }
    void *mapped = nullptr;
    require_hr(vertex_buffer.get()->Lock(
                   0U, static_cast<UINT>(vertices.size() * sizeof(Vertex)),
                   &mapped, D3DLOCK_DISCARD),
               "lock D3D9 scene vertex buffer");
    std::memcpy(mapped, vertices.data(), vertices.size() * sizeof(Vertex));
    require_hr(vertex_buffer.get()->Unlock(),
               "unlock D3D9 scene vertex buffer");
    require_hr(device.get()->SetStreamSource(0U, vertex_buffer.get(), 0U,
                                             sizeof(Vertex)),
               "bind D3D9 scene vertex buffer");
    device.get()->SetSamplerState(0U, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    device.get()->SetSamplerState(0U, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    device.get()->SetSamplerState(0U, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device.get()->SetSamplerState(0U, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device.get()->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    BatchState previous_state;
    auto previous_textured = false;
    auto first_batch = true;
    for (const auto &batch : batches) {
      auto *texture = tron ? nullptr : texture_for(batch.state.texture_levels);
      const auto textured = texture != nullptr;
      device.get()->SetTexture(0U, texture);
      if (first_batch ||
          previous_state.trilinear != batch.state.trilinear) {
        device.get()->SetSamplerState(
            0U, D3DSAMP_MIPFILTER,
            batch.state.trilinear ? D3DTEXF_LINEAR : D3DTEXF_NONE);
      }
      if (first_batch || previous_state.blend != batch.state.blend ||
          previous_textured != textured) {
        set_blend(tron ? SceneBlendMode::opaque : batch.state.blend, textured);
      }
      if (first_batch || previous_state.depth_test != batch.state.depth_test) {
        device.get()->SetRenderState(
            D3DRS_ZENABLE,
            batch.state.depth_test ? D3DZB_TRUE : D3DZB_FALSE);
      }
      if (first_batch || previous_state.depth_bias != batch.state.depth_bias) {
        device.get()->SetRenderState(
            D3DRS_DEPTHBIAS, batch.state.depth_bias ? 0xbdcccccdU : 0U);
      }
      const auto first = static_cast<UINT>(batch.first_vertex);
      const auto primitives = static_cast<UINT>(batch.vertex_count / 3U);
      require_hr(device.get()->DrawPrimitive(D3DPT_TRIANGLELIST, first,
                                             primitives),
                 "draw D3D9 scene geometry batch");
      if (tron) {
        device.get()->SetTexture(0U, nullptr);
        device.get()->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device.get()->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
        device.get()->SetRenderState(
            D3DRS_TEXTUREFACTOR,
            D3DCOLOR_ARGB(255U, 112U, 148U, 255U));
        device.get()->SetTextureStageState(0U, D3DTSS_COLOROP,
                                           D3DTOP_SELECTARG1);
        device.get()->SetTextureStageState(0U, D3DTSS_COLORARG1,
                                           D3DTA_TFACTOR);
        device.get()->SetTextureStageState(0U, D3DTSS_ALPHAOP,
                                           D3DTOP_SELECTARG1);
        device.get()->SetTextureStageState(0U, D3DTSS_ALPHAARG1,
                                           D3DTA_TFACTOR);
        require_hr(device.get()->DrawPrimitive(D3DPT_TRIANGLELIST, first,
                                               primitives),
                   "draw D3D9 Tron wireframe batch");
        device.get()->SetTextureStageState(0U, D3DTSS_COLOROP,
                                           D3DTOP_MODULATE);
        device.get()->SetTextureStageState(0U, D3DTSS_COLORARG1,
                                           D3DTA_TEXTURE);
        device.get()->SetTextureStageState(0U, D3DTSS_COLORARG2,
                                           D3DTA_DIFFUSE);
        device.get()->SetTextureStageState(0U, D3DTSS_ALPHAOP,
                                           D3DTOP_MODULATE);
        device.get()->SetTextureStageState(0U, D3DTSS_ALPHAARG1,
                                           D3DTA_TEXTURE);
        device.get()->SetTextureStageState(0U, D3DTSS_ALPHAARG2,
                                           D3DTA_DIFFUSE);
        first_batch = true;
      } else {
        previous_state = batch.state;
        previous_textured = textured;
        first_batch = false;
      }
    }
  }

  void draw_projected_quad(const std::array<SDL_FPoint, 4U> &points,
                           const std::array<float, 4U> &depths,
                           const double near_plane,
                           const std::array<D3DCOLOR, 4U> &colors,
                           const SceneBlendMode blend) {
    constexpr std::array<std::size_t, 6U> order{0U, 1U, 2U, 0U, 2U, 3U};
    std::array<Vertex, 6U> vertices{};
    for (std::size_t i = 0; i < order.size(); ++i) {
      const auto source = order[i];
      const auto depth = std::max<double>(depths[source], near_plane);
      vertices[i] = {points[source].x + d3d9_pixel_center_offset,
                     points[source].y + d3d9_pixel_center_offset,
                     static_cast<float>(std::clamp(
                         1.0 - near_plane / depth - 1.0e-5, 0.0, 1.0)),
                     static_cast<float>(1.0 / depth),
                     colors[source],
                     0.0F,
                     0.0F};
    }
    device.get()->SetTexture(0U, nullptr);
    set_blend(blend, false);
    device.get()->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    require_hr(device.get()->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2U,
                                             vertices.data(), sizeof(Vertex)),
               "draw D3D9 projected effect");
  }

  void draw_projected_quad(const std::array<SDL_FPoint, 4U> &points,
                           const std::array<float, 4U> &depths,
                           const double near_plane, const D3DCOLOR color,
                           const SceneBlendMode blend) {
    std::array<D3DCOLOR, 4U> colors{};
    colors.fill(color);
    draw_projected_quad(points, depths, near_plane, colors, blend);
  }

  ProjectionConstants
  projection_base(const PerspectiveView &view, const double render_distance,
                  const double cue_start, const bool cue_enabled,
                  const std::array<float, 3U> &color) const {
    ProjectionConstants constants;
    constants.output_and_projection = {
        static_cast<float>(depth_width), static_cast<float>(depth_height),
        view.center_x, static_cast<float>(view.near_plane)};
    constants.view_position = vector4(view.position, view.center_y);
    constants.view_right = vector4(view.right);
    constants.view_up = vector4(view.up);
    constants.view_forward = vector4(view.forward);
    constants.cue_parameters = {static_cast<float>(render_distance),
                                static_cast<float>(cue_start),
                                cue_enabled ? 1.0F : 0.0F, view.focal_length};
    constants.cue_color = {color[0U], color[1U], color[2U], 1.0F};
    return constants;
  }

  void draw_depth_projection(const int minimum_x, const int maximum_x,
                             const int minimum_y, const int maximum_y,
                             IDirect3DPixelShader9 *shader,
                             const ProjectionConstants &constants,
                             const bool additive) {
    const auto left = static_cast<float>(minimum_x);
    const auto right = static_cast<float>(maximum_x + 1);
    const auto top = static_cast<float>(minimum_y);
    const auto bottom = static_cast<float>(maximum_y + 1);
    constexpr std::array<std::size_t, 6U> order{0U, 1U, 2U, 0U, 2U, 3U};
    const std::array<std::array<float, 2U>, 4U> corners{
        {{left, top}, {right, top}, {right, bottom}, {left, bottom}}};
    std::array<Vertex, 6U> vertices{};
    for (std::size_t index = 0U; index < order.size(); ++index) {
      const auto &corner = corners[order[index]];
      vertices[index] = {corner[0U] - 0.5F, corner[1U] - 0.5F, 0.0F,      1.0F,
                         0xffffffffU,       corner[0U],        corner[1U]};
    }
    const RECT scissor{minimum_x, minimum_y, maximum_x + 1, maximum_y + 1};
    device.get()->SetScissorRect(&scissor);
    device.get()->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    device.get()->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    device.get()->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device.get()->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device.get()->SetRenderState(D3DRS_ALPHABLENDENABLE,
                                 additive ? TRUE : FALSE);
    if (additive) {
      device.get()->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
      device.get()->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    }
    device.get()->SetSamplerState(0U, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device.get()->SetSamplerState(0U, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device.get()->SetSamplerState(0U, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device.get()->SetSamplerState(0U, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device.get()->SetTexture(0U, sampled_depth.get());
    device.get()->SetPixelShader(shader);
    require_hr(device.get()->SetPixelShaderConstantF(
                   0U, reinterpret_cast<const float *>(&constants),
                   static_cast<UINT>(sizeof(constants) / sizeof(float) / 4U)),
               "upload D3D9 projection constants");
    require_hr(device.get()->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2U,
                                             vertices.data(), sizeof(Vertex)),
               "draw depth-aware D3D9 projection");
    device.get()->SetPixelShader(nullptr);
    device.get()->SetTexture(0U, nullptr);
    device.get()->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  }

  void draw_shadow(const VehicleShadowDepthProjection &projection,
                   const double near_plane) {
    if (depth_sampling && shadow_shader) {
      auto constants = projection_base(
          projection.view, projection.render_distance, projection.cue_start,
          projection.cue_enabled,
          {projection.cue_color[0U] / 255.0F, projection.cue_color[1U] / 255.0F,
           projection.cue_color[2U] / 255.0F});
      constants.world_bounds = {static_cast<float>(projection.minimum_world_x),
                                static_cast<float>(projection.maximum_world_x),
                                static_cast<float>(projection.minimum_world_z),
                                static_cast<float>(projection.maximum_world_z)};
      for (std::size_t cell = 0U; cell < projection.cells.size(); ++cell) {
        const auto &source = projection.cells[cell];
        constants.cell_xz[cell * 2U] = {
            static_cast<float>(source.world_xz[0U][0U]),
            static_cast<float>(source.world_xz[0U][1U]),
            static_cast<float>(source.world_xz[1U][0U]),
            static_cast<float>(source.world_xz[1U][1U])};
        constants.cell_xz[cell * 2U + 1U] = {
            static_cast<float>(source.world_xz[2U][0U]),
            static_cast<float>(source.world_xz[2U][1U]),
            static_cast<float>(source.world_xz[3U][0U]),
            static_cast<float>(source.world_xz[3U][1U])};
        constants.cell_y_valid[cell] = {
            static_cast<float>(source.minimum_receiver_y),
            static_cast<float>(source.maximum_receiver_y),
            source.valid ? 1.0F : 0.0F, 0.0F};
      }
      draw_depth_projection(projection.minimum_x, projection.maximum_x,
                            projection.minimum_y, projection.maximum_y,
                            shadow_shader.get(), constants, false);
      return;
    }
    for (const auto &cell : projection.cells) {
      if (!cell.valid)
        continue;
      std::array<SDL_FPoint, 4U> points{};
      std::array<float, 4U> depths{};
      auto valid = true;
      for (std::size_t i = 0; i < 4U; ++i) {
        const mh::game::CollisionVector3 world{
            cell.world_xz[i][0], cell.world_y[i] + 0.006, cell.world_xz[i][1]};
        valid = valid && project(projection.view, world, points[i], depths[i]);
      }
      if (valid) {
        draw_projected_quad(points, depths, near_plane,
                            D3DCOLOR_ARGB(255U, 10U, 10U, 10U),
                            SceneBlendMode::opaque);
      }
    }
  }

  void draw_headlight(const HeadlightDepthProjection &projection,
                      const double near_plane) {
    if (depth_sampling && headlight_shader) {
      auto constants = projection_base(
          projection.view, projection.render_distance, projection.cue_start,
          projection.cue_enabled, {0.0F, 0.0F, 0.0F});
      constants.vehicle_position = vector4(projection.vehicle.world_position);
      constants.body_basis_0 = vector4(projection.vehicle.body_basis[0U]);
      constants.body_basis_1 = vector4(projection.vehicle.body_basis[1U]);
      constants.body_basis_2 = vector4(projection.vehicle.body_basis[2U]);
      constants.source_0_center = vector4(projection.sources[0U].center);
      constants.source_0_direction = vector4(projection.sources[0U].direction);
      constants.source_1_center = vector4(projection.sources[1U].center);
      constants.source_1_direction = vector4(projection.sources[1U].direction);
      draw_depth_projection(projection.minimum_x, projection.maximum_x,
                            projection.minimum_y, projection.maximum_y,
                            headlight_shader.get(), constants, true);
      return;
    }
    constexpr std::array<double, 7U> distances{0.35, 2.0,  5.0, 8.0,
                                               11.0, 15.0, 20.0};
    constexpr std::array<double, 9U> lateral_positions{
        -0.5, -0.25, 0.0, 0.25, 0.5, 0.75, 1.0, 1.25, 1.5};
    constexpr std::array<double, 9U> lateral_coverage{
        0.0, 0.15625, 0.5, 0.84375, 1.0, 0.84375, 0.5, 0.15625, 0.0};
    auto local_to_world = [&projection](const std::array<double, 3U> &local) {
      auto world = projection.vehicle.world_position;
      for (std::size_t axis = 0; axis < 3U; ++axis) {
        for (std::size_t component = 0; component < 3U; ++component) {
          world[component] +=
              local[axis] * projection.vehicle.body_basis[axis][component];
        }
      }
      return world;
    };
    const auto beam_alpha = [&distances](const double distance,
                                         const double coverage) {
      const auto fade_in = std::clamp((distance - distances[0U]) /
                                          (distances[1U] - distances[0U]),
                                      0.0, 1.0);
      const auto remaining =
          std::clamp(1.0 - distance / distances.back(), 0.0, 1.0);
      return static_cast<BYTE>(
          std::clamp(fade_in * remaining * remaining * coverage * 0.46 * 255.0,
                     0.0, 255.0));
    };
    const auto beam_point = [&projection, &local_to_world](
                                const double distance, const double lateral,
                                SDL_FPoint &point, float &depth) {
      std::array<std::array<double, 3U>, 2U> edges{};
      for (std::size_t side = 0U; side < edges.size(); ++side) {
        for (std::size_t axis = 0U; axis < edges[side].size(); ++axis) {
          edges[side][axis] =
              projection.sources[side].center[axis] +
              projection.sources[side].direction[axis] * distance;
        }
      }
      std::array<double, 3U> local{};
      for (std::size_t axis = 0U; axis < local.size(); ++axis) {
        local[axis] =
            edges[0U][axis] * (1.0 - lateral) + edges[1U][axis] * lateral;
      }
      local[1U] = -0.35;
      return project(projection.view, local_to_world(local), point, depth);
    };
    for (std::size_t strip = 0U; strip + 1U < distances.size(); ++strip) {
      for (std::size_t band = 0U; band + 1U < lateral_positions.size();
           ++band) {
        const std::array<std::array<std::size_t, 2U>, 4U> corners{
            {{strip, band},
             {strip, band + 1U},
             {strip + 1U, band + 1U},
             {strip + 1U, band}}};
        std::array<SDL_FPoint, 4U> points{};
        std::array<float, 4U> depths{};
        std::array<D3DCOLOR, 4U> colors{};
        auto valid = true;
        for (std::size_t corner = 0U; corner < corners.size(); ++corner) {
          const auto distance_index = corners[corner][0U];
          const auto lateral_index = corners[corner][1U];
          valid = valid && beam_point(distances[distance_index],
                                      lateral_positions[lateral_index],
                                      points[corner], depths[corner]);
          colors[corner] =
              D3DCOLOR_ARGB(beam_alpha(distances[distance_index],
                                       lateral_coverage[lateral_index]),
                            150U, 150U, 150U);
        }
        if (valid) {
          draw_projected_quad(points, depths, near_plane, colors,
                              SceneBlendMode::additive);
        }
      }
    }
  }
};

D3D9Renderer::D3D9Renderer() : impl_(std::make_unique<Impl>()) {}
D3D9Renderer::~D3D9Renderer() = default;

bool D3D9Renderer::initialize(SDL_Renderer *renderer) {
  impl_->owner = renderer;
  impl_->device.reset();
  impl_->vertex_buffer.reset();
  impl_->vertex_capacity = 0U;
  impl_->shadow_shader.reset();
  impl_->headlight_shader.reset();
  impl_->saved_state.reset();
  impl_->textures.clear();
  impl_->batch_vertices.clear();
  impl_->batch_vertices.reserve(4096U);
  impl_->draw_batches.clear();
  impl_->draw_batches.reserve(256U);
  impl_->ordered_commands.clear();
  impl_->ordered_commands.reserve(16384U);
  if (renderer == nullptr ||
      std::string_view(SDL_GetRendererName(renderer)) != "direct3d")
    return false;
  auto *device = static_cast<IDirect3DDevice9 *>(
      SDL_GetPointerProperty(SDL_GetRendererProperties(renderer),
                             SDL_PROP_RENDERER_D3D9_DEVICE_POINTER, nullptr));
  if (device == nullptr)
    return false;
  device->AddRef();
  impl_->device.reset(device);
  D3DCAPS9 capabilities{};
  if (SUCCEEDED(device->GetDeviceCaps(&capabilities)) &&
      capabilities.PixelShaderVersion >= D3DPS_VERSION(3, 0)) {
    const auto shadow_result = device->CreatePixelShader(
        reinterpret_cast<const DWORD *>(shadow_shader_bytecode.data()),
        impl_->shadow_shader.put());
    const auto headlight_result = device->CreatePixelShader(
        reinterpret_cast<const DWORD *>(headlight_shader_bytecode.data()),
        impl_->headlight_shader.put());
    if (FAILED(shadow_result) || FAILED(headlight_result)) {
      impl_->shadow_shader.reset();
      impl_->headlight_shader.reset();
    }
  }
  require_hr(device->CreateStateBlock(D3DSBT_ALL, impl_->saved_state.put()),
             "create D3D9 renderer state block");
  return true;
}

bool D3D9Renderer::available() const noexcept { return bool(impl_->device); }

bool D3D9Renderer::render(SDL_Renderer *renderer, const SceneFrameView &frame) {
  if (!impl_->device || renderer != impl_->owner || frame.commands == nullptr ||
      frame.shadow_projections == nullptr ||
      frame.headlight_projections == nullptr)
    return false;
  if (!SDL_FlushRenderer(renderer))
    return false;
  const auto start = std::chrono::steady_clock::now();
  require_hr(impl_->saved_state.get()->Capture(),
             "capture D3D9 renderer state values");
  ComPtr<IDirect3DSurface9> target;
  require_hr(impl_->device.get()->GetRenderTarget(0U, target.put()),
             "query D3D9 render target");
  D3DSURFACE_DESC target_desc{};
  require_hr(target.get()->GetDesc(&target_desc), "query D3D9 target size");
  impl_->ensure_depth(target_desc.Width, target_desc.Height);
  require_hr(
      impl_->device.get()->SetDepthStencilSurface(impl_->depth_surface.get()),
      "bind D3D9 depth surface");
  require_hr(
      impl_->device.get()->Clear(0U, nullptr, D3DCLEAR_ZBUFFER, 0U, 1.0F, 0U),
      "clear D3D9 depth surface");
  D3DVIEWPORT9 viewport{0U,   0U,  target_desc.Width, target_desc.Height,
                        0.0F, 1.0F};
  impl_->device.get()->SetViewport(&viewport);
  impl_->device.get()->SetFVF(vertex_fvf);
  impl_->device.get()->SetVertexShader(nullptr);
  impl_->device.get()->SetPixelShader(nullptr);
  impl_->device.get()->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
  impl_->device.get()->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
  impl_->device.get()->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  impl_->device.get()->SetRenderState(D3DRS_LIGHTING, FALSE);
  impl_->device.get()->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
  impl_->device.get()->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  impl_->device.get()->SetTextureStageState(0U, D3DTSS_COLOROP,
                                            D3DTOP_MODULATE);
  impl_->device.get()->SetTextureStageState(0U, D3DTSS_COLORARG1,
                                            D3DTA_TEXTURE);
  impl_->device.get()->SetTextureStageState(0U, D3DTSS_COLORARG2,
                                            D3DTA_DIFFUSE);
  impl_->device.get()->SetTextureStageState(0U, D3DTSS_ALPHAOP,
                                            D3DTOP_MODULATE);
  impl_->device.get()->SetTextureStageState(0U, D3DTSS_ALPHAARG1,
                                            D3DTA_TEXTURE);
  impl_->device.get()->SetTextureStageState(0U, D3DTSS_ALPHAARG2,
                                            D3DTA_DIFFUSE);
  impl_->device.get()->SetSamplerState(0U, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
  impl_->device.get()->SetSamplerState(0U, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
  impl_->device.get()->SetSamplerState(0U, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  impl_->device.get()->SetSamplerState(0U, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  // Shadow markers separate world receivers from cars. Batch within each
  // geometry span, never across a projection that must precede those cars.
  const std::span<const SceneRasterCommand> commands(*frame.commands);
  auto first = std::size_t{0U};
  while (first < commands.size()) {
    const auto shadow_pass = commands[first].shadow_projection_index.has_value();
    auto last = first + 1U;
    while (last < commands.size() &&
           commands[last].shadow_projection_index.has_value() == shadow_pass) {
      ++last;
    }
    require_hr(impl_->device.get()->SetDepthStencilSurface(
                   shadow_pass && impl_->depth_sampling
                       ? nullptr : impl_->depth_surface.get()),
               "bind D3D9 scene pass depth surface");
    if (shadow_pass) {
      for (auto index = first; index < last; ++index) {
        impl_->draw_shadow(
            frame.shadow_projections->at(*commands[index].shadow_projection_index),
            frame.near_plane);
      }
    } else {
      impl_->draw_commands(commands.subspan(first, last - first), frame,
                           frame.tron_hidden_line);
    }
    first = last;
  }
  if (impl_->depth_sampling) {
    require_hr(impl_->device.get()->SetDepthStencilSurface(nullptr),
               "unbind sampled D3D9 depth surface");
  }
  for (const auto &headlight : *frame.headlight_projections) {
    impl_->draw_headlight(headlight, frame.near_plane);
  }
  require_hr(impl_->saved_state.get()->Apply(), "restore D3D9 renderer state");
  impl_->render_ms = std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - start)
                         .count();
  return true;
}

double D3D9Renderer::last_render_ms() const noexcept {
  return impl_->render_ms;
}

} // namespace mh::render::hardware
