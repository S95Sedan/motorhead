#include <content/formats/ai_driver_profile.hpp>
#include <content/formats/ai_route.hpp>
#include <content/formats/car_definition.hpp>
#include <content/formats/s40_content.hpp>
#include <content/formats/col_collision_mesh.hpp>
#include <content/formats/iff_image.hpp>
#include <content/formats/league_definition.hpp>
#include <content/formats/mde_replay.hpp>
#include <content/formats/motion_path.hpp>
#include <content/formats/myo_object_model.hpp>
#include <content/formats/myw_world_model.hpp>
#include <content/formats/texture_assets.hpp>
#include <content/formats/tga_image.hpp>
#include <content/formats/track_definition.hpp>
#include <core/filesystem/case_insensitive.hpp>
#include <core/logging/runtime_log.hpp>
#include <core/serialization/json.hpp>
#include <disc/cd_audio.hpp>
#include <game/ai/vehicle_runtime.hpp>
#include <game/gameplay/cheats.hpp>
#include <game/physics/collision_import.hpp>
#include <game/physics/simulation.hpp>
#include <game/race/camera.hpp>
#include <game/race/course_import.hpp>
#include <game/race/distance_cue.hpp>
#include <game/race/drive_session.hpp>
#include <game/race/ghost_import.hpp>
#include <game/race/sky.hpp>
#include <game/vehicle/import.hpp>
#include <game/vehicle/motion_import.hpp>
#include <game/vehicle/presentation.hpp>
#include <game/vehicle/runtime.hpp>
#include <network/session.hpp>
#include <platform/sdl_input.hpp>
#include <platform/sdl_audio_output.hpp>
#include <ui/config/game_config.hpp>
#include <ui/frontend/renderer.hpp>

#include "race_runtime.hpp"
#include <renderer/race_scene_renderer.hpp>
#include <renderer/types.hpp>

#include <SDL3/SDL.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using mh::common::resolve_relative_case_insensitive;

template <typename T, void (*Destroy)(T *)>
using SdlPointer = std::unique_ptr<T, decltype(Destroy)>;

template <typename T, void (*Destroy)(T *)> struct ConditionalSdlDestroy {
  bool owned = true;
  void operator()(T *value) const {
    if (owned && value != nullptr) {
      Destroy(value);
    }
  }
};

void require(const bool condition, const char *operation) {
  if (!condition) {
    throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
  }
}

template <typename Value, typename Compare>
void stable_insertion_sort(std::vector<Value> &values, Compare compare) {
  for (std::size_t index = 1U; index < values.size(); ++index) {
    auto value = std::move(values[index]);
    auto position = index;
    while (position != 0U && compare(value, values[position - 1U])) {
      values[position] = std::move(values[position - 1U]);
      --position;
    }
    values[position] = std::move(value);
  }
}

[[nodiscard]] mh::render::RaceRendererBackend
race_renderer_backend(const mh::ui::GraphicRendererBackend backend) noexcept {
  switch (backend) {
  case mh::ui::GraphicRendererBackend::automatic:
    return mh::render::RaceRendererBackend::automatic;
  case mh::ui::GraphicRendererBackend::d3d9:
    return mh::render::RaceRendererBackend::d3d9;
  case mh::ui::GraphicRendererBackend::d3d11:
    return mh::render::RaceRendererBackend::d3d11;
  case mh::ui::GraphicRendererBackend::d3d12:
    return mh::render::RaceRendererBackend::d3d12;
  case mh::ui::GraphicRendererBackend::glide:
    return mh::render::RaceRendererBackend::glide;
  case mh::ui::GraphicRendererBackend::software:
    return mh::render::RaceRendererBackend::software;
  }
  return mh::render::RaceRendererBackend::automatic;
}

[[nodiscard]] mh::ui::GraphicRendererBackend graphic_renderer_backend(
    const mh::render::RaceRendererBackend backend) noexcept {
  switch (backend) {
  case mh::render::RaceRendererBackend::automatic:
    return mh::ui::GraphicRendererBackend::automatic;
  case mh::render::RaceRendererBackend::d3d9:
    return mh::ui::GraphicRendererBackend::d3d9;
  case mh::render::RaceRendererBackend::d3d11:
    return mh::ui::GraphicRendererBackend::d3d11;
  case mh::render::RaceRendererBackend::d3d12:
    return mh::ui::GraphicRendererBackend::d3d12;
  case mh::render::RaceRendererBackend::glide:
    return mh::ui::GraphicRendererBackend::glide;
  case mh::render::RaceRendererBackend::software:
    return mh::ui::GraphicRendererBackend::software;
  }
  return mh::ui::GraphicRendererBackend::automatic;
}

[[nodiscard]] std::string_view
renderer_backend_name(const mh::render::RaceRendererBackend backend) noexcept {
  return mh::ui::graphic_renderer_backend_label(
      graphic_renderer_backend(backend));
}

[[nodiscard]] std::string_view
renderer_backend_token(const mh::render::RaceRendererBackend backend) noexcept {
  switch (backend) {
  case mh::render::RaceRendererBackend::automatic:
    return "auto";
  case mh::render::RaceRendererBackend::d3d9:
    return "d3d9";
  case mh::render::RaceRendererBackend::d3d11:
    return "d3d11";
  case mh::render::RaceRendererBackend::d3d12:
    return "d3d12";
  case mh::render::RaceRendererBackend::glide:
    return "glide";
  case mh::render::RaceRendererBackend::software:
    return "software";
  }
  return "auto";
}

[[nodiscard]] SDL_Renderer *
create_race_renderer(SDL_Window *window,
                     mh::render::RaceRendererBackend &backend) {
  const auto try_driver = [window](const char *driver) {
    SDL_ClearError();
    return SDL_CreateRenderer(window, driver);
  };
  const auto try_hardware = [&](const bool d3d12, const bool d3d11,
                                const bool d3d9) -> SDL_Renderer * {
    if (d3d12) {
      if (auto *renderer = try_driver("direct3d12"); renderer != nullptr) {
        if (backend != mh::render::RaceRendererBackend::software &&
            backend != mh::render::RaceRendererBackend::glide) {
          backend = mh::render::RaceRendererBackend::d3d12;
        }
        return renderer;
      }
    }
    if (d3d11) {
      if (auto *renderer = try_driver("direct3d11"); renderer != nullptr) {
        if (backend != mh::render::RaceRendererBackend::software &&
            backend != mh::render::RaceRendererBackend::glide) {
          backend = mh::render::RaceRendererBackend::d3d11;
        }
        return renderer;
      }
    }
    if (d3d9) {
      if (auto *renderer = try_driver("direct3d"); renderer != nullptr) {
        if (backend != mh::render::RaceRendererBackend::software) {
          backend = mh::render::RaceRendererBackend::d3d9;
        }
        return renderer;
      }
    }
    return nullptr;
  };

  SDL_Renderer *renderer = nullptr;
  switch (backend) {
  case mh::render::RaceRendererBackend::automatic:
  case mh::render::RaceRendererBackend::d3d12:
    renderer = try_hardware(true, true, true);
    break;
  case mh::render::RaceRendererBackend::d3d11:
  case mh::render::RaceRendererBackend::glide:
    renderer = try_hardware(false, true, true);
    break;
  case mh::render::RaceRendererBackend::d3d9:
    renderer = try_hardware(false, false, true);
    break;
  case mh::render::RaceRendererBackend::software:
    renderer = try_hardware(true, true, true);
    break;
  }
  if (renderer == nullptr) {
    backend = mh::render::RaceRendererBackend::software;
    renderer = SDL_CreateRenderer(window, nullptr);
  }
  return renderer;
}

[[nodiscard]] bool
renderer_driver_matches(const mh::render::RaceRendererBackend backend,
                        const std::string_view driver) noexcept {
  switch (backend) {
  case mh::render::RaceRendererBackend::d3d9:
    return driver == "direct3d";
  case mh::render::RaceRendererBackend::d3d12:
    return driver == "direct3d12";
  case mh::render::RaceRendererBackend::d3d11:
  case mh::render::RaceRendererBackend::glide:
    return driver == "direct3d11";
  case mh::render::RaceRendererBackend::software:
    return true;
  case mh::render::RaceRendererBackend::automatic:
    return false;
  }
  return false;
}

struct BuiltInCameraSettings {
  mh::game::VehicleCameraTuning playable{
      mh::game::CameraModeTuning{2.0, 5.1, 6.34019174590991},
      mh::game::CameraModeTuning{2.0, 7.0, 6.34019174590991},
      mh::game::CameraModeTuning{0.75, -0.15, 0.0},
      mh::game::CameraModeTuning{0.48, -2.15, -0.116324392260247}};
  mh::game::CameraModeTuning supercars{40.0, -12.0, 90.0};
  mh::game::CameraModeTuning ignition{15.0, 20.0, 25.0};
};

struct RendererVsyncRestore {
  SDL_Renderer *renderer = nullptr;
  bool restore = false;

  ~RendererVsyncRestore() {
    if (restore && renderer != nullptr) {
      static_cast<void>(SDL_SetRenderVSync(renderer, 1));
    }
  }
};

struct ProjectedPoint {
  float x = 0.0F;
  float y = 0.0F;
};

struct ViewPoint {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

using PerspectiveView = mh::render::PerspectiveView;

ViewPoint to_view(const PerspectiveView &view,
                  const mh::game::CollisionVector3 &world);
ProjectedPoint project(const PerspectiveView &view, const ViewPoint &point);

struct OverheadWorldCutaway {
  mh::game::CollisionVector3 vehicle_position{};
  mh::game::CollisionVector3 vehicle_forward{0.0, 0.0, 1.0};
  double longitudinal_radius = 34.0;
  double lateral_half_width = 10.0;
  double minimum_height_above_vehicle = 3.25;
};

struct CarVisualComponent {
  mh::content::MyoData model;
  mh::game::CollisionVector3 translation{};
  mh::game::CollisionVector3 suspension_axis{};
  std::vector<std::optional<std::size_t>> material_indices;
  std::optional<std::size_t> wheel_index;
  bool scene_blink_emissive = false;
};

struct VehicleHaloAttachment {
  mh::game::CollisionVector3 center{};
  mh::game::CollisionVector3 face_normal{0.0, 0.0, 1.0};
};

using VehicleHeadlightSource = mh::render::VehicleHeadlightSource;

using HeadlightDepthProjection = mh::render::HeadlightDepthProjection;
using VehicleShadowReceiverCell = mh::render::VehicleShadowReceiverCell;
using VehicleShadowDepthProjection = mh::render::VehicleShadowDepthProjection;

struct CarVisual {
  std::vector<CarVisualComponent> components;
  std::filesystem::path texture_root;
  mh::game::CollisionVector3 body_minimum{};
  mh::game::CollisionVector3 body_maximum{};
  std::array<mh::content::CarColor, 3U> default_colors{};
  std::array<mh::content::CarColor, 3U> active_colors{};
  std::optional<std::array<std::array<mh::content::CarVector, 4U>, 4U>>
      shadow_points;
  std::array<std::vector<VehicleHaloAttachment>, 2U> halo_attachments;
  std::array<std::vector<mh::content::PamRgbaImage>, 2U> halo_images;
  std::vector<VehicleHeadlightSource> headlight_sources;
  std::array<std::optional<std::size_t>, 2U> emissive_material_indices;
  std::uint64_t face_count = 0U;
};

struct WheelVisualState {
  std::array<double, 4U> spin_radians{};
  std::array<float, 4U> suspension_states{};
  double steering = 0.0;
};

struct SkidMarkSegment {
  mh::game::CollisionVector3 start{};
  mh::game::CollisionVector3 end{};
  mh::game::CollisionVector3 contact_normal{};
  std::uint8_t material_mark = 0U;
};

struct SkidMarkEmitter {
  struct Sample {
    mh::game::CollisionVector3 point{};
    std::uint8_t material_mark = 0U;
    bool segment_open = false;
  };
  std::array<std::optional<Sample>, 4U> previous{};
  std::array<std::deque<SkidMarkSegment>, 4U> trails{};
};

struct TireSmokeParticle {
  mh::game::CollisionVector3 position{};
  mh::game::CollisionVector3 drift{};
  double age_seconds = 0.0;
  double lifetime_seconds = 0.65;
  double size = 0.35;
  double phase = 0.0;
};

struct TireSmokeEmitter {
  std::array<double, 4U> emission_accumulator{};
  std::array<bool, 4U> active_sources{};
  std::deque<TireSmokeParticle> particles;
  std::uint32_t sequence = 0U;
};

struct SparkParticle {
  mh::game::CollisionVector3 position{};
  mh::game::CollisionVector3 velocity{};
  double age_seconds = 0.0;
  double lifetime_seconds = 0.35;
};

struct SparkEmitter {
  std::deque<SparkParticle> particles;
  double emission_cooldown_seconds = 0.0;
  std::uint32_t sequence = 0U;
  bool contact_active = false;
};

struct RecordedCarVisual {
  mh::content::MdeV3RacerMetadata racer;
  mh::content::CarDefinition car;
  CarVisual visual;
  mh::game::OriginalDynamicVehicleContactShape contact_shape;
};

struct MaterialTexture {
  std::string logical_id;
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  // Some authored repeating road surfaces contain a narrow full-height
  // border strip. The retail asset remains untouched on disk; the runtime
  // upload omits that strip so filtered WRAP sampling cannot blend it back in
  // at every repeat boundary.
  std::uint32_t sampled_left_crop = 0U;
  float sampled_u_scale = 0.0F;
  float sampled_u_offset = 0.0F;
  std::uint64_t transparent_pixels = 0U;
  mh::content::PamRgbaImage image;
  std::optional<mh::content::PamRgbaImage> override_image;
  std::vector<mh::content::PamRgbaImage> raster_levels;
  std::vector<mh::content::PamRgbaImage> override_raster_levels;
  SdlPointer<SDL_Texture, SDL_DestroyTexture> texture{nullptr,
                                                      SDL_DestroyTexture};
  SdlPointer<SDL_Texture, SDL_DestroyTexture> override_texture{
      nullptr, SDL_DestroyTexture};
  SdlPointer<SDL_Texture, SDL_DestroyTexture> fog_texture{nullptr,
                                                          SDL_DestroyTexture};
  SdlPointer<SDL_Texture, SDL_DestroyTexture> override_fog_texture{
      nullptr, SDL_DestroyTexture};
};

struct VehicleEnvironmentTextures {
  SdlPointer<SDL_Texture, SDL_DestroyTexture> environment{nullptr,
                                                          SDL_DestroyTexture};
  SdlPointer<SDL_Texture, SDL_DestroyTexture> reflection{nullptr,
                                                         SDL_DestroyTexture};
  SdlPointer<SDL_Texture, SDL_DestroyTexture> phong{nullptr,
                                                    SDL_DestroyTexture};
  mh::content::PamRgbaImage environment_image;
  mh::content::PamRgbaImage reflection_image;
  mh::content::PamRgbaImage phong_image;
  std::vector<mh::content::PamRgbaImage> environment_raster_levels;
  std::vector<mh::content::PamRgbaImage> reflection_raster_levels;
  std::vector<mh::content::PamRgbaImage> phong_raster_levels;
};

struct WorldHaloAttachment {
  mh::game::CollisionVector3 center{};
  double half_size = 0.0;
  std::size_t image_index = 0U;
};

struct WorldLampBeam {
  mh::game::CollisionVector3 source{};
  mh::game::CollisionVector3 source_edge_direction{1.0, 0.0, 0.0};
  mh::game::CollisionVector3 receiver{};
  std::array<float, 3U> color{1.0F, 1.0F, 1.0F};
  double source_half_width = 0.0;
  double receiver_half_width = 0.0;
  float alpha = 0.0F;
};

enum class VehicleEnvironmentMap {
  none,
  environment,
  reflection,
  phong,
};

struct WorldVisual {
  mh::content::MywData world;
  mh::content::TextureCatalog catalog;
  std::vector<MaterialTexture> materials;
  std::vector<std::optional<std::size_t>> material_indices;
  std::map<std::string, std::filesystem::path> override_paths;
  std::uint64_t override_materials = 0U;
  VehicleEnvironmentTextures vehicle_environment;
  std::vector<WorldHaloAttachment> halo_attachments;
  std::vector<std::vector<mh::content::PamRgbaImage>> halo_images;
  std::vector<WorldLampBeam> lamp_beams;
  std::vector<mh::content::PamRgbaImage> lamp_beam_images;
  bool generated_spatial_cells = false;
};

struct BackgroundVisual {
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  SdlPointer<SDL_Texture, SDL_DestroyTexture> texture{nullptr,
                                                      SDL_DestroyTexture};
};

struct HudTexture {
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  SdlPointer<SDL_Texture, SDL_DestroyTexture> texture{nullptr,
                                                      SDL_DestroyTexture};
};

struct RaceHudVisual {
  HudTexture meter_active;
  HudTexture meter_inactive;
  HudTexture gear_label;
  HudTexture speed_unit_metric;
  HudTexture speed_unit_imperial;
  std::array<HudTexture, 10U> large_digits;
  std::array<HudTexture, 10U> small_digits;
  HudTexture reverse;
  HudTexture colon;
  HudTexture total_label;
  HudTexture time_label;
  HudTexture best_label;
  HudTexture lap_label;
  HudTexture position_label;
  HudTexture player_marker;
  HudTexture avenger_marker;
  HudTexture opponent_marker;
  HudTexture ghost_marker;
  HudTexture font;
  std::array<float, 256U> font_advances{};
  float font_line_metric = 0.0F;
  std::array<HudTexture, 4U> countdown;
  std::array<float, 2U> map_min{};
  std::array<float, 2U> map_max{};
};

struct LiveRaceRosterSlot {
  std::string driver_nick;
  std::string car_name;
  std::string portrait_name;
  std::array<std::uint8_t, 3U> driver_color{};
  std::uint32_t starting_score = 0U;
  std::optional<std::array<float, 2U>> ai_profile_percentages;
};

struct AiOpeningDynamicContactRecord {
  std::size_t pass = 0U;
  std::size_t other_slot = 0U;
  bool self_is_first = false;
  mh::game::OriginalDynamicVehicleContactResponse response{};
};

struct AiOpeningControlRecord {
  std::size_t cycle = 0U;
  std::size_t order = 0U;
  std::size_t slot = 0U;
  mh::game::OriginalAiVehicleObservation observation{};
  float current_speed = 0.0F;
  float time_step_seconds = 0.0F;
  std::array<float, 2U> vehicle_xz{};
  std::array<float, 2U> vehicle_forward_xz{};
  mh::game::OriginalBodyPoseState body_pose{};
  mh::game::OriginalBodyVelocityState body_velocity{};
  mh::game::OriginalVehicleDriveState drive_state{};
  std::array<float, 4U> retained_wheel_states{};
  std::array<std::uint32_t, 4U> retained_wheel_contact_flags{};
  std::array<mh::game::OriginalBodyVelocityState, 3U>
      preceding_response_velocities{};
  std::array<mh::game::BodyForceAccumulator, 3U> preceding_accumulated_forces{};
  std::array<mh::game::OriginalVehicleResponseFrame, 3U>
      preceding_response_frames{};
  std::array<mh::game::OriginalBodyPoseState, 3U> preceding_post_pose_states{};
  std::array<mh::game::OriginalBodyVelocityState, 3U>
      preceding_post_pose_velocities{};
  std::optional<mh::game::OriginalVehicleGroundedDampingResult>
      preceding_grounded_damping;
  std::optional<mh::game::OriginalDrivetrainModeCForceResult>
      preceding_drivetrain;
  std::size_t preceding_body_hull_reaction_count = 0U;
  std::vector<AiOpeningDynamicContactRecord> preceding_dynamic_contacts;
  mh::game::OriginalAiControllerState state_before{};
  mh::game::OriginalAiStochasticControlState stochastic_before{};
  mh::game::OriginalAiControllerStep step{};
  mh::game::OriginalAiControllerState state_after{};
  mh::game::OriginalAiStochasticControlState stochastic_after{};
};

using SceneBlendMode = mh::render::SceneBlendMode;

struct ProjectedModelFace {
  std::array<ProjectedPoint, 4U> vertices{};
  std::array<double, 4U> view_depths{};
  std::array<SDL_FPoint, 4U> texture_coordinates{};
  std::array<SDL_FPoint, 4U> environment_coordinates{};
  std::array<std::array<float, 3U>, 4U> light_contributions{};
  std::array<std::uint8_t, 3U> color{};
  std::optional<std::size_t> material_index;
  std::uint8_t vertex_count = 0U;
  double depth = 0.0;
  bool apply_object_lighting = true;
  bool apply_accelerated_brightness = true;
  float opacity = 1.0F;
  VehicleEnvironmentMap environment_map = VehicleEnvironmentMap::none;
  SceneBlendMode blend_mode = SceneBlendMode::opaque;
};

struct ViewTextureVertex {
  ViewPoint point{};
  std::array<float, 3U> light_contribution{};
  std::array<float, 3U> color{1.0F, 1.0F, 1.0F};
  float u = 0.0F;
  float v = 0.0F;
};

struct PreparedVehicleNormal {
  std::array<float, 3U> light_contribution{};
  SDL_FPoint environment_coordinate{};
};

struct PreparedVehiclePointLight {
  std::array<float, 3U> direction{};
  std::array<float, 3U> color{};
  float intensity = 0.0F;
  bool specular = false;
};

struct WorldRenderScratch {
  std::vector<std::uint8_t> visible_objects;
  std::vector<std::uint8_t> active_spatial_objects;
  std::vector<std::uint8_t> transformed_positions;
  std::vector<ViewPoint> view_positions;
};

struct VehicleRenderScratch {
  std::vector<ProjectedModelFace> faces;
  std::vector<const ProjectedModelFace *> face_order;
  std::vector<ViewPoint> view_positions;
  std::vector<PreparedVehicleNormal> normals;
  std::vector<PreparedVehiclePointLight> point_lights;
};

using SceneDepthResources = mh::render::RaceRenderer;

struct RendererProfileTotals {
  std::uint64_t frames = 0U;
  double background_ms = 0.0;
  double depth_clear_ms = 0.0;
  double shadows_ms = 0.0;
  double world_ms = 0.0;
  double vehicles_ms = 0.0;
  double skid_marks_ms = 0.0;
  double depth_upload_ms = 0.0;
  double raster_ms = 0.0;
  double texture_upload_ms = 0.0;
  double hud_ms = 0.0;
  double presentation_ms = 0.0;
  double render_total_ms = 0.0;
  double render_total_squared_ms = 0.0;
  double render_total_minimum_ms = std::numeric_limits<double>::infinity();
  double render_total_maximum_ms = 0.0;
  std::uint64_t presentation_intervals = 0U;
  double presentation_interval_ms = 0.0;
  double presentation_interval_squared_ms = 0.0;
  double presentation_interval_minimum_ms =
      std::numeric_limits<double>::infinity();
  double presentation_interval_maximum_ms = 0.0;
};

struct PresentationBackBuffers {
  int width = 0;
  int height = 0;
  SDL_PixelFormat pixel_format = SDL_PIXELFORMAT_UNKNOWN;
  std::size_t active = 0U;
  std::size_t completed_frames = 0U;
  bool direct = false;
  std::vector<SdlPointer<SDL_Texture, SDL_DestroyTexture>> textures;
};

struct WorldRenderEnvironment {
  std::array<float, 3U> brightness{1.0F, 1.0F, 1.0F};
  std::array<float, 3U> object_brightness{1.0F, 1.0F, 1.0F};
  std::array<float, 3U> object_ambient{1.0F, 1.0F, 1.0F};
  std::array<float, 3U> accelerated_brightness{1.0F, 1.0F, 1.0F};
  std::array<float, 3U> base_lighting{1.0F, 1.0F, 1.0F};
  float specular_factor = 0.0F;
  float light_intensity = 1.0F;
  std::array<float, 3U> cue_color{};
  double cue_start = 0.0;
  bool cue_enabled = false;
};

struct AudioClip {
  SDL_AudioSpec specification{};
  std::vector<std::uint8_t> bytes;
};

enum class AudioPan {
  center,
  left,
  right,
};

struct AudioDevice {
  SDL_AudioDeviceID identifier = 0U;
  SDL_AudioSpec specification{};
  int sample_frames = 0;

  explicit AudioDevice(const std::string_view output_name) {
    SDL_AudioSpec requested{};
    require(mh::platform::audio_output_format(output_name,
                                     &requested, &sample_frames),
            "query preferred playback device format");
    identifier =
        mh::platform::open_audio_output(output_name, &requested);
    require(identifier != 0U, "open shared playback device");
    require(
        SDL_GetAudioDeviceFormat(identifier, &specification, &sample_frames),
        "query shared playback device format");
    require(SDL_PauseAudioDevice(identifier),
            "pause shared playback device during setup");
  }

  AudioDevice(const AudioDevice &) = delete;
  AudioDevice &operator=(const AudioDevice &) = delete;
  AudioDevice(AudioDevice &&other) noexcept
      : identifier(std::exchange(other.identifier, 0U)),
        specification(other.specification), sample_frames(other.sample_frames) {
  }
  AudioDevice &operator=(AudioDevice &&other) noexcept {
    if (this != &other) {
      if (identifier != 0U) {
        SDL_CloseAudioDevice(identifier);
      }
      identifier = std::exchange(other.identifier, 0U);
      specification = other.specification;
      sample_frames = other.sample_frames;
    }
    return *this;
  }
  ~AudioDevice() {
    if (identifier != 0U) {
      SDL_CloseAudioDevice(identifier);
    }
  }

  void set_enabled(const bool enabled) const {
    require(enabled ? SDL_ResumeAudioDevice(identifier)
                    : SDL_PauseAudioDevice(identifier),
            enabled ? "resume shared playback device"
                    : "pause shared playback device");
  }
};

struct LoopingAudioState {
  AudioClip clip;
  std::size_t cursor = 0U;
  bool queue_complete_loop_on_request = false;
  std::atomic_bool failed = false;

  void queue(SDL_AudioStream *stream, std::size_t requested_bytes) noexcept {
    if (queue_complete_loop_on_request && requested_bytes != 0U) {
      requested_bytes = clip.bytes.size();
    }
    const auto frame_bytes =
        static_cast<std::size_t>(SDL_AUDIO_FRAMESIZE(clip.specification));
    requested_bytes =
        (requested_bytes + frame_bytes - 1U) / frame_bytes * frame_bytes;
    while (requested_bytes != 0U) {
      const auto contiguous = clip.bytes.size() - cursor;
      auto chunk = std::min(requested_bytes, contiguous);
      chunk -= chunk % frame_bytes;
      if (chunk == 0U) {
        cursor = 0U;
        continue;
      }
      if (!SDL_PutAudioStreamData(stream, clip.bytes.data() + cursor,
                                  static_cast<int>(chunk))) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
      cursor = (cursor + chunk) % clip.bytes.size();
      requested_bytes -= chunk;
    }
  }
};

void SDLCALL refill_looping_audio(void *userdata, SDL_AudioStream *stream,
                                  const int additional_amount,
                                  const int /*total_amount*/) {
  if (additional_amount <= 0) {
    return;
  }
  static_cast<LoopingAudioState *>(userdata)->queue(
      stream, static_cast<std::size_t>(additional_amount));
}

struct LoopingAudio {
  // Keep callback state alive until after the stream/device is destroyed.
  std::unique_ptr<LoopingAudioState> state;
  SdlPointer<SDL_AudioStream, SDL_DestroyAudioStream> stream{
      nullptr, SDL_DestroyAudioStream};
  SDL_AudioDeviceID device = 0U;
  bool bound = false;

  void set_bound(const bool enabled) {
    if (enabled == bound) {
      return;
    }
    if (enabled) {
      require(SDL_BindAudioStream(device, stream.get()),
              "resume looping audio stream");
      bound = true;
      return;
    }
    SDL_UnbindAudioStream(stream.get());
    bound = false;
  }

  void refill() {
    if (state->failed.load(std::memory_order_relaxed)) {
      throw std::runtime_error("queue looping audio: " +
                               std::string(SDL_GetError()));
    }
  }
};

struct SpatialLoopingAudio {
  std::array<LoopingAudio, 2U> channels;

  [[nodiscard]] std::array<SDL_AudioStream *, 2U> streams() {
    return {channels[0U].stream.get(), channels[1U].stream.get()};
  }

  void set_gain(const double gain, const std::array<double, 2U> &channel_gains,
                const float volume_scale) {
    for (std::size_t channel = 0U; channel < channels.size(); ++channel) {
      require(
          SDL_SetAudioStreamGain(
              channels[channel].stream.get(),
              static_cast<float>(gain * channel_gains[channel]) * volume_scale),
          "set spatial looping-audio gain");
    }
  }

  void set_frequency_ratio(const float ratio) {
    for (auto &channel : channels) {
      require(SDL_SetAudioStreamFrequencyRatio(channel.stream.get(), ratio),
              "set spatial looping-audio pitch");
    }
  }

  void refill() {
    for (auto &channel : channels) {
      channel.refill();
    }
  }
};

struct OneShotAudio {
  AudioClip clip;
  SdlPointer<SDL_AudioStream, SDL_DestroyAudioStream> stream{
      nullptr, SDL_DestroyAudioStream};
  float base_gain = 1.0F;

  void trigger() {
    require(SDL_ClearAudioStream(stream.get()), "clear one-shot audio");
    require(SDL_PutAudioStreamData(stream.get(), clip.bytes.data(),
                                   static_cast<int>(clip.bytes.size())),
            "queue one-shot audio");
    require(SDL_FlushAudioStream(stream.get()), "flush one-shot audio");
  }

  void stop() const {
    require(SDL_ClearAudioStream(stream.get()), "clear one-shot audio");
  }

  void set_volume_scale(const float scale) const {
    require(SDL_SetAudioStreamGain(stream.get(), base_gain * scale),
            "set one-shot audio volume");
  }

  [[nodiscard]] bool active() const {
    return SDL_GetAudioStreamQueued(stream.get()) > 0;
  }
};

struct RetailEngineAudio {
  std::array<LoopingAudio, 6U> layers;
  OneShotAudio gear_change;
  SDL_AudioDeviceID device = 0U;
  bool bound = true;
  float volume_scale = 1.0F;

  std::array<SDL_AudioStream *, 7U> streams() {
    return {layers[0U].stream.get(), layers[1U].stream.get(),
            layers[2U].stream.get(), layers[3U].stream.get(),
            layers[4U].stream.get(), layers[5U].stream.get(),
            gear_change.stream.get()};
  }

  void set_enabled(const bool enabled) {
    if (enabled == bound) {
      return;
    }
    auto stream_list = streams();
    if (enabled) {
      require(SDL_BindAudioStreams(device, stream_list.data(),
                                   static_cast<int>(stream_list.size())),
              "bind retail engine streams");
      bound = true;
      return;
    }
    SDL_UnbindAudioStreams(stream_list.data(),
                           static_cast<int>(stream_list.size()));
    for (auto *stream : stream_list) {
      require(SDL_ClearAudioStream(stream), "clear retail engine stream");
    }
    bound = false;
  }

  void set_volume_scale(const float scale) {
    volume_scale = std::clamp(scale, 0.0F, 1.0F);
    gear_change.set_volume_scale(volume_scale);
  }

  void update(const mh::game::EngineAudioMixFrame &mix, const bool racing,
              const double camera_gain) {
    if (!std::isfinite(camera_gain) || camera_gain < 0.0) {
      throw std::invalid_argument(
          "player engine camera gain must be finite and non-negative");
    }
    const std::array recovered_levels{mix.gason_level,  mix.gasrele_level,
                                      mix.gas_ot_level, mix.gas_ot_level,
                                      mix.gas_rt_level, mix.gas_rt_level};
    for (std::size_t index = 0U; index < layers.size(); ++index) {
      require(SDL_SetAudioStreamFrequencyRatio(
                  layers[index].stream.get(),
                  static_cast<float>(mix.frequency_ratio)),
              "set retail engine layer pitch");
      const auto gain = racing ? mh::game::retail_engine_audio_linear_gain(
                                     recovered_levels[index])
                               : 0.0;
      require(SDL_SetAudioStreamGain(layers[index].stream.get(),
                                     static_cast<float>(gain * camera_gain) *
                                         volume_scale),
              "set retail engine layer gain");
      layers[index].refill();
    }
    if (racing && mix.gear_changed) {
      gear_change.set_volume_scale(
          static_cast<float>(static_cast<double>(volume_scale) * camera_gain));
      gear_change.trigger();
    }
  }

  void stop() const {
    for (const auto &layer : layers) {
      require(SDL_SetAudioStreamGain(layer.stream.get(), 0.0F),
              "silence retail engine layer");
      require(SDL_ClearAudioStream(layer.stream.get()),
              "clear retail engine layer");
    }
    gear_change.stop();
  }
};

struct OpponentEngineAudioFrame {
  std::array<double, 3U> position{};
  double engine_scalar = 0.0;
};

std::array<double, 3U>
spatial_listener_right(const mh::game::VehicleCameraPose &listener) {
  std::array<double, 3U> forward{
      listener.world_target[0U] - listener.world_position[0U],
      listener.world_target[1U] - listener.world_position[1U],
      listener.world_target[2U] - listener.world_position[2U]};
  const auto forward_length =
      std::sqrt(forward[0U] * forward[0U] + forward[1U] * forward[1U] +
                forward[2U] * forward[2U]);
  if (forward_length <= 1.0e-12) {
    throw std::logic_error("spatial listener has no forward direction");
  }
  for (auto &component : forward) {
    component /= forward_length;
  }
  std::array<double, 3U> right{
      listener.world_up[1U] * forward[2U] - listener.world_up[2U] * forward[1U],
      listener.world_up[2U] * forward[0U] - listener.world_up[0U] * forward[2U],
      listener.world_up[0U] * forward[1U] -
          listener.world_up[1U] * forward[0U]};
  const auto right_length = std::sqrt(
      right[0U] * right[0U] + right[1U] * right[1U] + right[2U] * right[2U]);
  if (right_length <= 1.0e-12) {
    throw std::logic_error("spatial listener has no right direction");
  }
  for (auto &component : right) {
    component /= right_length;
  }
  return right;
}

struct OpponentEngineAudio {
  std::vector<SpatialLoopingAudio> layers;
  SDL_AudioDeviceID device = 0U;
  bool bound = true;
  float volume_scale = 1.0F;

  void set_enabled(const bool enabled) {
    if (enabled == bound) {
      return;
    }
    std::vector<SDL_AudioStream *> streams;
    streams.reserve(layers.size() * 2U);
    for (auto &layer : layers) {
      const auto channel_streams = layer.streams();
      streams.insert(streams.end(), channel_streams.begin(),
                     channel_streams.end());
    }
    if (enabled) {
      if (!streams.empty()) {
        require(SDL_BindAudioStreams(device, streams.data(),
                                     static_cast<int>(streams.size())),
                "bind opponent engine streams");
      }
      bound = true;
      return;
    }
    if (!streams.empty()) {
      SDL_UnbindAudioStreams(streams.data(), static_cast<int>(streams.size()));
    }
    for (auto *stream : streams) {
      require(SDL_ClearAudioStream(stream), "clear opponent engine stream");
    }
    bound = false;
  }

  void set_volume_scale(const float scale) {
    volume_scale = std::clamp(scale, 0.0F, 1.0F);
  }

  void update(const std::vector<OpponentEngineAudioFrame> &frames,
              const mh::game::VehicleCameraPose &listener, const bool active) {
    if (frames.size() != layers.size()) {
      throw std::logic_error("opponent engine audio frame count changed");
    }
    constexpr double original_enemy_engine_level = 0.141;
    constexpr double original_enemy_engine_minimum_distance = 15.0;
    constexpr double original_enemy_engine_maximum_distance = 250.0;
    constexpr double original_enemy_engine_pitch_scale = 0.00016412;
    constexpr double original_enemy_engine_pitch_bias = 0.31646;
    const auto base_gain =
        mh::game::retail_spatial_audio_linear_gain(original_enemy_engine_level);
    const auto listener_right = spatial_listener_right(listener);
    for (std::size_t index = 0U; index < layers.size(); ++index) {
      auto &layer = layers[index];
      const auto distance_gain = mh::game::original_spatial_sound_distance_gain(
          frames[index].position, listener.world_position,
          original_enemy_engine_minimum_distance,
          original_enemy_engine_maximum_distance);
      const auto channel_gains = mh::game::direct_sound_3d_stereo_channel_gains(
          frames[index].position, listener.world_position, listener_right);
      layer.set_gain(active ? base_gain * distance_gain : 0.0, channel_gains,
                     volume_scale);
      layer.set_frequency_ratio(static_cast<float>(
          frames[index].engine_scalar * original_enemy_engine_pitch_scale +
          original_enemy_engine_pitch_bias));
      layer.refill();
    }
  }

  void stop() const {
    for (const auto &layer : layers) {
      for (const auto &channel : layer.channels) {
        require(SDL_SetAudioStreamGain(channel.stream.get(), 0.0F),
                "silence opponent engine layer");
        require(SDL_ClearAudioStream(channel.stream.get()),
                "clear opponent engine layer");
      }
    }
  }
};

struct RaceEventAudio {
  std::array<OneShotAudio, 3U> countdown;
  OneShotAudio go;
  std::array<OneShotAudio, 2U> finish;

  void trigger_countdown(const std::size_t index) {
    countdown.at(index).trigger();
  }
  void trigger_go() { go.trigger(); }
  void trigger_finish() {
    for (auto &sound : finish) {
      sound.trigger();
    }
  }

  void stop() const {
    for (const auto &sound : countdown) {
      sound.stop();
    }
    go.stop();
    for (const auto &sound : finish) {
      sound.stop();
    }
  }

  void set_volume_scale(const float scale) const {
    for (const auto &sound : countdown) {
      sound.set_volume_scale(scale);
    }
    go.set_volume_scale(scale);
    for (const auto &sound : finish) {
      sound.set_volume_scale(scale);
    }
  }
};

struct HornRaceAudio {
  OneShotAudio sound;

  void update(const bool held, const bool controls_active) {
    // p3.1 calls its one-shot start owner every sound update while car +0xb4
    // is nonzero. The owner ignores an already active buffer, so a held horn
    // retriggers only after the selected sample has completed; releasing it
    // does not truncate the current sample.
    if (held && controls_active && !sound.active()) {
      sound.trigger();
    }
  }

  void stop() const { sound.stop(); }
  void set_volume_scale(const float scale) const {
    sound.set_volume_scale(scale);
  }
};

enum class MaterialLoopRole : std::size_t {
  contact = 0U,
  slide = 1U,
  spin = 2U,
};

struct MaterialLoopBinding {
  mh::game::OriginalMaterialSoundDefinition definition;
  LoopingAudio audio;
};

struct MaterialOneShotBinding {
  mh::game::OriginalMaterialSoundDefinition definition;
  OneShotAudio audio;
};

struct MaterialScratchBinding {
  mh::game::OriginalMaterialSoundDefinition definition;
  std::array<LoopingAudio, 2U> audio;
};

struct MaterialRaceAudio {
  std::array<std::vector<MaterialLoopBinding>, 3U> loops;
  std::array<std::array<std::optional<std::size_t>, 26U>, 3U> material_loops{};
  std::array<std::vector<MaterialOneShotBinding>, 2U> impulses;
  std::array<std::array<std::optional<std::size_t>, 26U>, 2U>
      material_impulses{};
  std::vector<MaterialScratchBinding> scratches;
  std::array<std::optional<std::size_t>, 26U> material_scratches{};
  LoopingAudio special_slide;
  std::array<OneShotAudio, 2U> special_slide_exit;
  float volume_scale = 1.0F;
  bool special_slide_active = false;
  std::size_t special_slide_exit_index = 0U;

  void set_volume_scale(const float scale) {
    volume_scale = std::clamp(scale, 0.0F, 1.0F);
  }

  void stop() {
    for (auto &role : loops) {
      for (auto &binding : role) {
        require(SDL_SetAudioStreamGain(binding.audio.stream.get(), 0.0F),
                "silence material loop on pause");
      }
    }
    for (auto &role : impulses) {
      for (auto &binding : role) {
        binding.audio.stop();
      }
    }
    for (auto &binding : scratches) {
      for (auto &audio : binding.audio) {
        require(SDL_SetAudioStreamGain(audio.stream.get(), 0.0F),
                "silence material scratch on pause");
      }
    }
    require(SDL_SetAudioStreamGain(special_slide.stream.get(), 0.0F),
            "silence special material slide on pause");
    for (const auto &sound : special_slide_exit) {
      sound.stop();
    }
    special_slide_active = false;
  }

  void update(const mh::game::OriginalVehicleModeCSceneFrame &frame,
              const mh::game::OriginalBodyVelocityState &velocity,
              const mh::game::OriginalVehicleGroundedMaterialTable &materials,
              const mh::game::OriginalVehicleSceneFrame::HullAudioRecord
                  &hull_audio_record) {
    std::array<std::vector<double>, 3U> levels;
    for (std::size_t role = 0U; role < loops.size(); ++role) {
      levels[role].resize(loops[role].size(), 0.0);
    }

    std::array<std::optional<std::size_t>, 4U> wheel_materials{};
    for (std::size_t wheel = 0U; wheel < wheel_materials.size(); ++wheel) {
      const auto &contact = frame.scene.response.contacts.wheels[wheel];
      if (contact.hit.has_value() && contact.hit->material < materials.size()) {
        wheel_materials[wheel] =
            static_cast<std::size_t>(contact.hit->material);
      }
    }
    const auto selected = mh::game::select_original_material_contact_sound(
        wheel_materials, materials);

    const auto set_level = [&](const MaterialLoopRole role,
                               const std::size_t material, const double level) {
      const auto role_index = static_cast<std::size_t>(role);
      const auto binding = material_loops[role_index][material];
      if (binding.has_value()) {
        levels[role_index][*binding] =
            std::max(levels[role_index][*binding], level);
      }
    };

    if (selected.has_value()) {
      const auto &material = materials[*selected];
      if (material.contact_sound.has_value()) {
        // The shared SDL backend is the non-spatial branch of this p3.1
        // owner, which carries the exact 1.2 player-attached multiplier.
        set_level(MaterialLoopRole::contact, *selected,
                  mh::game::original_material_contact_sound_level(
                      velocity.local_linear[2U],
                      material.contact_sound->scalar_1, false));
      }

      const auto selection = frame.scene.grounded_damping_selection;
      const auto primary_transition =
          selection.has_value() && selection->alternate_state_a;
      const auto direction_transition =
          selection.has_value() && selection->alternate_state_b;
      const auto spin_transition = primary_transition || direction_transition;
      const auto retained_slide = selection.has_value() &&
                                  selection->alternate_branch &&
                                  !spin_transition;
      const auto traction = selection.has_value()
                                ? selection->retained_traction_accumulator
                                : 0.0;
      if (spin_transition && material.spin_sound.has_value()) {
        set_level(MaterialLoopRole::spin, *selected,
                  mh::game::original_material_spin_sound_level(
                      traction, material.spin_sound->scalar_1));
      } else if (retained_slide && material.slide_type != 1U &&
                 material.slide_sound.has_value()) {
        set_level(MaterialLoopRole::slide, *selected,
                  mh::game::original_material_slide_sound_level(
                      traction, material.slide_type));
      }

      const auto special_now = retained_slide && material.slide_type == 1U;
      const auto special_level =
          special_now ? mh::game::original_material_slide_sound_level(
                            traction, material.slide_type)
                      : 0.0;
      require(SDL_SetAudioStreamGain(
                  special_slide.stream.get(),
                  static_cast<float>(mh::game::retail_engine_audio_linear_gain(
                      special_level)) *
                      volume_scale),
              "set special material slide gain");
      special_slide.refill();
      if (special_slide_active && !special_now) {
        auto &exit = special_slide_exit[special_slide_exit_index];
        exit.set_volume_scale(
            static_cast<float>(mh::game::retail_engine_audio_linear_gain(0.5)) *
            volume_scale);
        exit.trigger();
        special_slide_exit_index ^= 1U;
      }
      special_slide_active = special_now;
    } else {
      require(SDL_SetAudioStreamGain(special_slide.stream.get(), 0.0F),
              "silence special material slide");
      special_slide.refill();
      special_slide_active = false;
    }

    for (std::size_t role = 0U; role < loops.size(); ++role) {
      for (std::size_t index = 0U; index < loops[role].size(); ++index) {
        auto &binding = loops[role][index];
        const auto level = levels[role][index];
        require(SDL_SetAudioStreamGain(
                    binding.audio.stream.get(),
                    static_cast<float>(
                        mh::game::retail_engine_audio_linear_gain(level)) *
                        volume_scale),
                "set material sound gain");
        require(SDL_SetAudioStreamFrequencyRatio(binding.audio.stream.get(),
                                                 binding.definition.scalar_2),
                "set authored material sound pitch");
        binding.audio.refill();
      }
    }

    std::vector<std::array<double, 2U>> scratch_levels(scratches.size());

    if (hull_audio_record.active &&
        hull_audio_record.material_index < materials.size()) {
      const auto hull_material = hull_audio_record.material_index;
      const auto hull_impulse = hull_audio_record.maximum_absolute_impulse;
      const auto &material = materials[hull_material];
      const auto trigger_impulse = [&](const std::size_t role,
                                       const double level) {
        const auto binding = material_impulses[role][hull_material];
        if (!binding.has_value()) {
          return;
        }
        auto &sound = impulses[role][*binding].audio;
        if (!sound.active()) {
          sound.set_volume_scale(
              static_cast<float>(
                  mh::game::retail_engine_audio_linear_gain(level)) *
              volume_scale);
          sound.trigger();
        }
      };
      constexpr double original_hard_impulse_threshold = 18500.0;
      if (hull_impulse < original_hard_impulse_threshold &&
          material.weak_impulse_sound.has_value()) {
        trigger_impulse(
            0U, mh::game::original_material_weak_impulse_sound_level(
                    hull_impulse, material.weak_impulse_sound->scalar_1));
      } else if (hull_impulse >= original_hard_impulse_threshold &&
                 material.hard_impulse_sound.has_value()) {
        trigger_impulse(1U,
                        mh::game::original_material_hard_impulse_sound_level(
                            material.hard_impulse_sound->scalar_1));
      }

      if (material.spark_animation_id != 0U &&
          hull_audio_record.scratch_candidate >= 0.0 &&
          material.scratch_sound.has_value()) {
        const auto scratch_binding = material_scratches[hull_material];
        if (scratch_binding.has_value()) {
          const auto pan = mh::game::original_material_scratch_pan(
              hull_audio_record.hull_point_index);
          const auto channel = pan < 0.0 ? 0U : 1U;
          const auto level = mh::game::original_material_scratch_sound_level(
              hull_audio_record.scratch_candidate,
              material.scratch_sound->scalar_1);
          scratch_levels[*scratch_binding][channel] =
              std::max(scratch_levels[*scratch_binding][channel], level);
        }
      }
    }
    for (std::size_t binding_index = 0U; binding_index < scratches.size();
         ++binding_index) {
      auto &binding = scratches[binding_index];
      for (std::size_t channel = 0U; channel < binding.audio.size();
           ++channel) {
        auto &sound = binding.audio[channel];
        require(
            SDL_SetAudioStreamGain(
                sound.stream.get(),
                static_cast<float>(mh::game::retail_engine_audio_linear_gain(
                    scratch_levels[binding_index][channel])) *
                    volume_scale),
            "set material scratch gain");
        require(SDL_SetAudioStreamFrequencyRatio(sound.stream.get(),
                                                 binding.definition.scalar_2),
                "set authored material scratch pitch");
        sound.refill();
      }
    }
  }
};

struct EnvironmentLoopBinding {
  mh::game::OriginalEnvironmentLoopSound definition;
  SpatialLoopingAudio audio;
};

struct EnvironmentObjectLoopBinding {
  mh::game::OriginalObjectEnvironmentLoopSound definition;
  std::size_t scene_object_index = 0U;
  SpatialLoopingAudio audio;
};

struct EnvironmentCollisionBodyAsset {
  std::size_t scene_object_index = 0U;
  std::filesystem::path model_path;
  std::filesystem::path collision_path;
  mh::game::OriginalDynamicVehicleContactShape shape;
};

struct EnvironmentCollisionBodyRuntime {
  mh::game::OriginalVehicleSceneState body;
  mh::game::OriginalBodyPoseState physics_previous_pose{};
  std::size_t retained_body_hull_contact_count = 0U;
  std::uint32_t collision_identifier = 0U;
  bool detached_from_authored_motion = false;
};

struct EnvironmentSceneVisualAsset {
  CarVisual visual;
  double bounding_radius = 0.0;
  std::vector<WorldHaloAttachment> light_attachments;
  bool has_blink_emissive_light = false;
};

struct EnvironmentSceneVisualInstance {
  std::size_t scene_object_index = 0U;
  std::size_t asset_index = 0U;
  std::size_t position_owner_scene_object_index = 0U;
  std::optional<std::size_t> collision_body_index;
};

struct EnvironmentSceneVisual {
  std::vector<EnvironmentSceneVisualAsset> assets;
  std::vector<EnvironmentSceneVisualInstance> instances;
};

struct EnvironmentObjectCollisionBinding {
  mh::game::OriginalObjectCollisionSound definition;
  std::array<OneShotAudio, 8U> audio;
};

struct EnvironmentRaceAudio {
  std::vector<EnvironmentLoopBinding> fixed_loops;
  mh::game::OriginalEnvironmentScene scene;
  std::vector<EnvironmentObjectLoopBinding> object_loops;
  std::vector<EnvironmentObjectCollisionBinding> object_collisions;
  std::array<std::optional<std::size_t>, 16U> collision_slots{};
  std::array<std::optional<std::size_t>, 8U> active_collisions{};
  float volume_scale = 1.0F;

  void set_volume_scale(const float scale) {
    volume_scale = std::clamp(scale, 0.0F, 1.0F);
    for (std::size_t vehicle = 0U; vehicle < active_collisions.size();
         ++vehicle) {
      if (active_collisions[vehicle].has_value()) {
        object_collisions[*active_collisions[vehicle]]
            .audio[vehicle]
            .set_volume_scale(volume_scale);
      }
    }
  }

  void stop() {
    constexpr std::array<double, 2U> silent_channels{0.0, 0.0};
    for (auto &binding : fixed_loops) {
      binding.audio.set_gain(0.0, silent_channels, 0.0F);
    }
    for (auto &binding : object_loops) {
      binding.audio.set_gain(0.0, silent_channels, 0.0F);
    }
    for (auto &binding : object_collisions) {
      for (const auto &audio : binding.audio) {
        audio.stop();
      }
    }
    active_collisions.fill(std::nullopt);
  }

  void trigger_collision(const std::size_t vehicle_index,
                         const std::uint32_t identifier,
                         const double collision_scalar) {
    if (vehicle_index >= active_collisions.size() ||
        identifier >= collision_slots.size() ||
        !collision_slots[identifier].has_value()) {
      return;
    }
    const auto binding_index = *collision_slots[identifier];
    auto &binding = object_collisions[binding_index];
    auto &audio = binding.audio[vehicle_index];
    auto &active_collision = active_collisions[vehicle_index];
    if (active_collision == binding_index && audio.active()) {
      return;
    }
    if (active_collision.has_value()) {
      object_collisions[*active_collision].audio[vehicle_index].stop();
    }
    audio.base_gain =
        static_cast<float>(mh::game::retail_engine_audio_linear_gain(
            mh::game::original_object_collision_sound_level(collision_scalar)));
    audio.set_volume_scale(volume_scale);
    require(SDL_SetAudioStreamFrequencyRatio(
                audio.stream.get(),
                static_cast<float>(mh::game::original_environment_sound_pitch(
                    static_cast<double>(binding.definition.pitch)))),
            "set object collision sound pitch");
    audio.trigger();
    active_collision = binding_index;
  }

  void update(const mh::game::VehicleCameraPose &listener,
              const double authored_scene_frame) {
    const auto listener_right = spatial_listener_right(listener);
    const auto update_loop = [&](const auto &definition,
                                 const std::array<double, 3U> &source_position,
                                 SpatialLoopingAudio &audio) {
      const auto distance_gain =
          mh::game::original_environment_sound_distance_gain(
              source_position, listener.world_position,
              static_cast<double>(definition.minimum_distance),
              static_cast<double>(definition.maximum_distance));
      // p3.1 submits Volume*0.14 as the spatial buffer's base volume. Its 3D
      // buffer applies the min-distance rolloff afterwards. Folding distance
      // into the nonlinear volume conversion changes the audible balance and
      // made low-priority ambience (notably Goldbridge's crow loops) mask the
      // vehicle layers.
      const auto base_level = mh::game::original_environment_sound_level(
          static_cast<double>(definition.volume), 1.0);
      const auto channel_gains = mh::game::direct_sound_3d_stereo_channel_gains(
          source_position, listener.world_position, listener_right);
      audio.set_gain(mh::game::retail_spatial_audio_linear_gain(base_level) *
                         distance_gain,
                     channel_gains, volume_scale);
      audio.set_frequency_ratio(
          static_cast<float>(mh::game::original_environment_sound_pitch(
              static_cast<double>(definition.pitch))));
      audio.refill();
    };
    for (auto &binding : fixed_loops) {
      const auto &definition = binding.definition;
      const std::array<double, 3U> source_position{
          static_cast<double>(definition.position[0U]),
          static_cast<double>(definition.position[1U]),
          static_cast<double>(definition.position[2U])};
      update_loop(definition, source_position, binding.audio);
    }
    for (auto &binding : object_loops) {
      const auto source_position =
          mh::game::sample_original_environment_scene_position(
              scene.objects.at(binding.scene_object_index),
              authored_scene_frame);
      update_loop(binding.definition, source_position, binding.audio);
    }
  }
};

std::string ascii_lower(std::string value) {
  for (auto &character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
  }
  return value;
}

std::string filename_lower(const std::filesystem::path &path) {
  return ascii_lower(path.filename().generic_string());
}

std::string trim_ascii(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1U);
}

std::string read_motorhead_config_value(const std::filesystem::path &path,
                                        const std::string_view key) {
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("cannot open motorhead.cfg: " + path.string());
  }
  std::string line;
  while (std::getline(stream, line)) {
    if (const auto comment = line.find("//"); comment != std::string::npos) {
      line.erase(comment);
    }
    line = trim_ascii(std::move(line));
    if (!line.starts_with(key)) {
      continue;
    }
    if (line.size() > key.size() && line[key.size()] != ' ' &&
        line[key.size()] != '\t') {
      continue;
    }
    return trim_ascii(line.substr(key.size()));
  }
  throw std::runtime_error("motorhead.cfg has no " + std::string(key));
}

std::string read_original_scroll_text(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot open original scroll text: " +
                             path.string());
  }
  std::string result((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
  if (const auto terminator = result.find('\0');
      terminator != std::string::npos) {
    result.erase(terminator);
  }
  for (auto &character : result) {
    if (character == '\r' || character == '\n' || character == '\t') {
      character = ' ';
    }
  }
  return result;
}

mh::content::CarColor parse_rgb_argument(const std::string_view value,
                                         const std::string_view option) {
  std::istringstream stream{std::string(value)};
  std::array<unsigned int, 3U> channels{};
  char first_separator = '\0';
  char second_separator = '\0';
  std::string trailing;
  if (!(stream >> channels[0U] >> first_separator >> channels[1U] >>
        second_separator >> channels[2U]) ||
      first_separator != ',' || second_separator != ',' ||
      (stream >> trailing) ||
      std::any_of(channels.begin(), channels.end(),
                  [](const auto channel) { return channel > 255U; })) {
    throw std::invalid_argument(std::string(option) +
                                " requires exact byte values R,G,B");
  }
  return {static_cast<std::uint8_t>(channels[0U]),
          static_cast<std::uint8_t>(channels[1U]),
          static_cast<std::uint8_t>(channels[2U])};
}

mh::content::CarColor parse_config_rgb(const std::string_view value,
                                       const std::string_view field) {
  std::istringstream stream{std::string(value)};
  std::array<unsigned int, 3U> channels{};
  std::string trailing;
  if (!(stream >> channels[0U] >> channels[1U] >> channels[2U]) ||
      (stream >> trailing) ||
      std::any_of(channels.begin(), channels.end(),
                  [](const auto channel) { return channel > 255U; })) {
    throw std::invalid_argument(std::string(field) +
                                " requires three byte values");
  }
  return {static_cast<std::uint8_t>(channels[0U]),
          static_cast<std::uint8_t>(channels[1U]),
          static_cast<std::uint8_t>(channels[2U])};
}

AudioClip load_wav_clip(const std::filesystem::path &path) {
  SDL_AudioSpec specification{};
  std::uint8_t *buffer = nullptr;
  std::uint32_t byte_count = 0U;
  require(
      SDL_LoadWAV(path.string().c_str(), &specification, &buffer, &byte_count),
      "load local WAV");
  std::unique_ptr<void, decltype(&SDL_free)> guard(buffer, SDL_free);
  if (byte_count == 0U ||
      byte_count %
              static_cast<std::uint32_t>(SDL_AUDIO_FRAMESIZE(specification)) !=
          0U) {
    throw std::runtime_error("local WAV has an empty or partial sample frame");
  }
  AudioClip result;
  result.specification = specification;
  result.bytes.assign(buffer, buffer + byte_count);
  return result;
}

AudioClip load_cdda_clip(const std::filesystem::path &cue_path,
                         const int track_number) {
  const auto track = mh::disc::read_cdda_track_pcm(cue_path, track_number);
  if (track.sample_rate >
      static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("CDDA sample rate exceeds SDL's range");
  }
  AudioClip result;
  result.specification.format = SDL_AUDIO_S16LE;
  result.specification.channels = static_cast<int>(track.channels);
  result.specification.freq = static_cast<int>(track.sample_rate);
  result.bytes = track.pcm;
  return result;
}

AudioClip preconvert_sample_rate(AudioClip clip, const int target_frequency) {
  if (target_frequency <= 0) {
    throw std::runtime_error("playback device reported an invalid frequency");
  }
  if (clip.specification.freq == target_frequency) {
    return clip;
  }
  auto target = clip.specification;
  target.freq = target_frequency;
  std::uint8_t *buffer = nullptr;
  int byte_count = 0;
  require(SDL_ConvertAudioSamples(&clip.specification, clip.bytes.data(),
                                  static_cast<int>(clip.bytes.size()), &target,
                                  &buffer, &byte_count),
          "preconvert complete music sample rate");
  std::unique_ptr<void, decltype(&SDL_free)> guard(buffer, SDL_free);
  if (byte_count <= 0 || byte_count % SDL_AUDIO_FRAMESIZE(target) != 0) {
    throw std::runtime_error(
        "preconverted music has an empty or partial sample frame");
  }
  AudioClip result;
  result.specification = target;
  result.bytes.assign(buffer, buffer + byte_count);
  return result;
}

AudioClip pan_mono_clip(AudioClip clip, const AudioPan pan) {
  if (pan == AudioPan::center) {
    return clip;
  }
  if (clip.specification.channels != 1U ||
      clip.specification.format != SDL_AUDIO_S16LE ||
      clip.bytes.size() % sizeof(std::int16_t) != 0U) {
    throw std::runtime_error(
        "retail hard-panned engine layers must be mono signed 16-bit WAV");
  }

  AudioClip result;
  result.specification = clip.specification;
  result.specification.channels = 2U;
  const auto sample_count = clip.bytes.size() / sizeof(std::int16_t);
  result.bytes.resize(sample_count * sizeof(std::int16_t) * 2U, 0U);
  for (std::size_t sample = 0U; sample < sample_count; ++sample) {
    const auto source_offset = sample * sizeof(std::int16_t);
    const auto destination_offset =
        sample * sizeof(std::int16_t) * 2U +
        (pan == AudioPan::right ? sizeof(std::int16_t) : 0U);
    result.bytes[destination_offset] = clip.bytes[source_offset];
    result.bytes[destination_offset + 1U] = clip.bytes[source_offset + 1U];
  }
  return result;
}

LoopingAudio start_looping_audio_clip(
    AudioClip clip, const SDL_AudioDeviceID device, const float gain,
    const AudioPan pan = AudioPan::center,
    const std::size_t queue_milliseconds = 2000U,
    const int target_frequency = 0, const bool queue_complete_loop = false) {
  if (queue_milliseconds == 0U || queue_milliseconds > 2000U) {
    throw std::invalid_argument(
        "audio queue duration must be in the range 1..2000 milliseconds");
  }
  LoopingAudio result;
  result.state = std::make_unique<LoopingAudioState>();
  result.state->clip = pan_mono_clip(std::move(clip), pan);
  if (target_frequency != 0) {
    result.state->clip =
        preconvert_sample_rate(std::move(result.state->clip), target_frequency);
  }
  result.state->queue_complete_loop_on_request = queue_complete_loop;
  result.stream.reset(
      SDL_CreateAudioStream(&result.state->clip.specification, nullptr));
  require(result.stream != nullptr, "create looping audio stream");
  require(SDL_SetAudioStreamGetCallback(
              result.stream.get(), refill_looping_audio, result.state.get()),
          "set looping audio callback");
  require(SDL_BindAudioStream(device, result.stream.get()),
          "bind looping audio to shared device");
  result.device = device;
  result.bound = true;
  require(SDL_SetAudioStreamGain(result.stream.get(), gain),
          "set looping audio gain");
  const auto frame_bytes = static_cast<std::size_t>(
      SDL_AUDIO_FRAMESIZE(result.state->clip.specification));
  const auto initial_queue_bytes =
      queue_complete_loop
          ? result.state->clip.bytes.size()
          : std::max(frame_bytes, static_cast<std::size_t>(
                                      result.state->clip.specification.freq) *
                                      frame_bytes * queue_milliseconds / 1000U);
  result.state->queue(result.stream.get(), initial_queue_bytes);
  result.refill();
  return result;
}

LoopingAudio start_looping_audio(const std::filesystem::path &path,
                                 const SDL_AudioDeviceID device,
                                 const float gain,
                                 const AudioPan pan = AudioPan::center,
                                 const std::size_t queue_milliseconds = 2000U,
                                 const int target_frequency = 0,
                                 const bool queue_complete_loop = false) {
  return start_looping_audio_clip(load_wav_clip(path), device, gain, pan,
                                  queue_milliseconds, target_frequency,
                                  queue_complete_loop);
}

SpatialLoopingAudio
start_spatial_looping_audio(const std::filesystem::path &path,
                            const SDL_AudioDeviceID device,
                            const std::size_t queue_milliseconds = 75U) {
  auto clip = load_wav_clip(path);
  SpatialLoopingAudio result;
  result.channels[0U] = start_looping_audio_clip(
      clip, device, 0.0F, AudioPan::left, queue_milliseconds);
  result.channels[1U] = start_looping_audio_clip(
      std::move(clip), device, 0.0F, AudioPan::right, queue_milliseconds);
  return result;
}

OneShotAudio start_one_shot_audio_clip(AudioClip clip,
                                       const SDL_AudioDeviceID device,
                                       const float gain) {
  OneShotAudio result;
  result.clip = std::move(clip);
  result.stream.reset(
      SDL_CreateAudioStream(&result.clip.specification, nullptr));
  require(result.stream != nullptr, "create one-shot audio stream");
  require(SDL_BindAudioStream(device, result.stream.get()),
          "bind one-shot audio to shared device");
  require(SDL_SetAudioStreamGain(result.stream.get(), gain),
          "set one-shot audio gain");
  result.base_gain = gain;
  return result;
}

OneShotAudio start_one_shot_audio(const std::filesystem::path &path,
                                  const SDL_AudioDeviceID device,
                                  const float gain) {
  return start_one_shot_audio_clip(load_wav_clip(path), device, gain);
}

std::filesystem::path
find_sibling_case_insensitive(const std::filesystem::path &directory,
                              const std::string &filename) {
  const auto wanted = ascii_lower(filename);
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file() && filename_lower(entry.path()) == wanted) {
      return entry.path();
    }
  }
  throw std::runtime_error("required local asset was not found: " +
                           (directory / filename).string());
}

AudioClip load_original_material_sound_clip(
    const std::filesystem::path &sound_root,
    const mh::game::OriginalMaterialSoundDefinition &definition) {
  auto clip = load_wav_clip(
      find_sibling_case_insensitive(sound_root, definition.wave_file));
  const auto frame_bytes =
      static_cast<std::size_t>(SDL_AUDIO_FRAMESIZE(clip.specification));
  const auto sample_frames = clip.bytes.size() / frame_bytes;
  const auto window = mh::game::original_material_sound_sample_window(
      sample_frames, definition.parameter_5, definition.parameter_6);
  if (!window.has_value()) {
    throw std::runtime_error("invalid original material sound sample window: " +
                             definition.wave_file);
  }
  const auto first_byte = window->leading_trim_frames * frame_bytes;
  const auto last_byte =
      clip.bytes.size() - window->trailing_trim_frames * frame_bytes;
  if (first_byte != 0U || last_byte != clip.bytes.size()) {
    clip.bytes = std::vector<std::uint8_t>(
        std::next(clip.bytes.begin(), static_cast<std::ptrdiff_t>(first_byte)),
        std::next(clip.bytes.begin(), static_cast<std::ptrdiff_t>(last_byte)));
  }
  return clip;
}

std::filesystem::path
find_child_directory_case_insensitive(const std::filesystem::path &directory,
                                      const std::string &name) {
  const auto wanted = ascii_lower(name);
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_directory() && filename_lower(entry.path()) == wanted) {
      return entry.path();
    }
  }
  throw std::runtime_error("required local directory was not found: " +
                           (directory / name).string());
}

std::vector<EnvironmentCollisionBodyAsset>
load_environment_collision_body_assets(
    const mh::game::OriginalEnvironmentScene &scene,
    const std::filesystem::path &scene_path) {
  const auto objects_root = find_child_directory_case_insensitive(
      scene_path.parent_path(), "Objects");
  std::vector<EnvironmentCollisionBodyAsset> result;
  for (std::size_t object_index = 0U; object_index < scene.objects.size();
       ++object_index) {
    const auto &object = scene.objects[object_index];
    // Retail stores the integer sentinel 1 instead of allocating body+0xc8
    // when UnseenByRays is present. Such an object never enters the physical
    // body list built at RVA 0x00077efd..0x00077f23.
    if (object.unseen_by_rays) {
      continue;
    }
    auto source = object.source_object;
    std::replace(source.begin(), source.end(), '\\', '/');
    const auto stem = std::filesystem::path(source).stem().string();
    if (stem.empty()) {
      throw std::runtime_error(
          "environment scene object has no source basename");
    }
    EnvironmentCollisionBodyAsset asset;
    asset.scene_object_index = object_index;
    // The p3.1 LoadObject owner formats the authored basename as .myo, then
    // rewrites that same extension to .COL for body+0x764. Names and case
    // remain entirely source-driven.
    asset.model_path =
        find_sibling_case_insensitive(objects_root, stem + ".myo");
    asset.collision_path =
        find_sibling_case_insensitive(objects_root, stem + ".col");
    asset.shape = mh::game::make_original_environment_collision_shape(
        mh::content::read_myo(asset.model_path),
        mh::content::read_col(asset.collision_path), object.polygon_size);
    result.push_back(std::move(asset));
  }
  return result;
}

EnvironmentSceneVisual load_environment_scene_visual(
    const mh::game::OriginalEnvironmentScene &scene,
    const std::filesystem::path &scene_path,
    const std::vector<EnvironmentCollisionBodyAsset> &collision_assets) {
  const auto objects_root = find_child_directory_case_insensitive(
      scene_path.parent_path(), "Objects");
  EnvironmentSceneVisual result;
  result.instances.reserve(scene.objects.size());
  std::map<std::string, std::size_t> asset_indices;
  for (std::size_t object_index = 0U; object_index < scene.objects.size();
       ++object_index) {
    auto source = scene.objects[object_index].source_object;
    std::replace(source.begin(), source.end(), '\\', '/');
    const auto stem = std::filesystem::path(source).stem().string();
    if (stem.empty()) {
      throw std::runtime_error(
          "environment scene visual has no source basename");
    }
    const auto key = ascii_lower(stem);
    auto found = asset_indices.find(key);
    if (found == asset_indices.end()) {
      auto model = mh::content::read_myo(
          find_sibling_case_insensitive(objects_root, stem + ".myo"));
      EnvironmentSceneVisualAsset asset;
      for (const auto &position : model.positions) {
        asset.bounding_radius = std::max(
            asset.bounding_radius,
            std::sqrt(static_cast<double>(position[0U]) * position[0U] +
                      static_cast<double>(position[1U]) * position[1U] +
                      static_cast<double>(position[2U]) * position[2U]));
      }
      asset.light_attachments.reserve(model.light_records.size());
      for (const auto &light : model.light_records) {
        WorldHaloAttachment attachment;
        for (std::size_t axis = 0U; axis < attachment.center.size(); ++axis) {
          attachment.center[axis] =
              (static_cast<double>(light.position[axis]) +
               static_cast<double>(light.secondary_position[axis])) *
              0.5;
        }
        const auto dx = static_cast<double>(light.secondary_position[0U]) -
                        light.position[0U];
        const auto dy = static_cast<double>(light.secondary_position[1U]) -
                        light.position[1U];
        const auto dz = static_cast<double>(light.secondary_position[2U]) -
                        light.position[2U];
        // MYO stores the physical lamp's diagonal anchors. The optical halo
        // extends beyond that mesh, but retains large authored sources such
        // as tunnel panels and machinery lamps. This bounded conversion also
        // keeps tiny warning bulbs visible without a per-asset exception.
        attachment.half_size =
            std::clamp(std::sqrt(dx * dx + dy * dy + dz * dz), 0.32, 1.8);
        attachment.image_index = light.image_index;
        asset.light_attachments.push_back(attachment);
      }
      // MYO v2 props can pair a fixed body with a lamp mesh identified by both
      // authored light points lying in one material's bounds. Atlantika's
      // je_conf uses LWS 0/1 scale keys as emission, not whole-prop scale.
      std::vector<bool> light_materials(model.names.size(), false);
      constexpr float light_bound_epsilon = 1.0e-4F;
      for (const auto &light : model.light_records) {
        for (std::size_t material = 0U; material < model.names.size();
             ++material) {
          std::array<float, 3U> minimum{std::numeric_limits<float>::infinity(),
                                        std::numeric_limits<float>::infinity(),
                                        std::numeric_limits<float>::infinity()};
          std::array<float, 3U> maximum{
              -std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity(),
              -std::numeric_limits<float>::infinity()};
          auto found_face = false;
          for (const auto &face : model.faces) {
            if (!face.has_texture_coordinates ||
                face.material_name_index != material) {
              continue;
            }
            found_face = true;
            for (std::size_t vertex = 0U; vertex < face.vertex_count;
                 ++vertex) {
              const auto &position =
                  model.positions.at(face.position_indices[vertex]);
              for (std::size_t axis = 0U; axis < minimum.size(); ++axis) {
                minimum[axis] = std::min(minimum[axis], position[axis]);
                maximum[axis] = std::max(maximum[axis], position[axis]);
              }
            }
          }
          const auto contains = [&](const std::array<float, 3U> &point) {
            for (std::size_t axis = 0U; axis < point.size(); ++axis) {
              if (point[axis] < minimum[axis] - light_bound_epsilon ||
                  point[axis] > maximum[axis] + light_bound_epsilon) {
                return false;
              }
            }
            return true;
          };
          if (found_face && contains(light.position) &&
              contains(light.secondary_position)) {
            light_materials[material] = true;
          }
        }
      }
      const auto is_light_face = [&](const mh::content::MyoFace &face) {
        return face.has_texture_coordinates &&
               face.material_name_index < light_materials.size() &&
               light_materials[face.material_name_index];
      };
      const auto light_face_count = static_cast<std::size_t>(
          std::count_if(model.faces.begin(), model.faces.end(), is_light_face));
      if (light_face_count != 0U && light_face_count != model.faces.size()) {
        auto fixed_model = model;
        std::erase_if(fixed_model.faces, is_light_face);
        std::erase_if(model.faces,
                      [&](const auto &face) { return !is_light_face(face); });
        asset.visual.face_count = fixed_model.faces.size() + model.faces.size();
        asset.visual.components.push_back(
            {std::move(fixed_model), {}, {}, {}, std::nullopt, false});
        asset.visual.components.push_back(
            {std::move(model), {}, {}, {}, std::nullopt, true});
        asset.has_blink_emissive_light = true;
      } else {
        asset.visual.face_count = model.faces.size();
        asset.visual.components.push_back(
            {std::move(model), {}, {}, {}, std::nullopt, false});
      }
      const auto asset_index = result.assets.size();
      result.assets.push_back(std::move(asset));
      found = asset_indices.emplace(key, asset_index).first;
    }
    EnvironmentSceneVisualInstance instance;
    instance.scene_object_index = object_index;
    instance.asset_index = found->second;
    instance.position_owner_scene_object_index = object_index;
    const auto &object = scene.objects[object_index];
    if (object.keys.size() > 1U) {
      for (std::size_t candidate_index = 0U; candidate_index < object_index;
           ++candidate_index) {
        const auto &candidate = scene.objects[candidate_index];
        if (candidate.keys.size() != object.keys.size()) {
          continue;
        }
        auto matched_position_path = true;
        for (std::size_t key_index = 0U; key_index < object.keys.size();
             ++key_index) {
          if (candidate.keys[key_index].frame != object.keys[key_index].frame) {
            matched_position_path = false;
            break;
          }
          for (std::size_t axis = 0U; axis < 3U; ++axis) {
            if (std::abs(candidate.keys[key_index].position[axis] -
                         object.keys[key_index].position[axis]) > 0.1) {
              matched_position_path = false;
              break;
            }
          }
          if (!matched_position_path) {
            break;
          }
        }
        if (matched_position_path) {
          instance.position_owner_scene_object_index = candidate_index;
          break;
        }
      }
    }
    const auto collision =
        std::find_if(collision_assets.begin(), collision_assets.end(),
                     [object_index](const auto &asset) {
                       return asset.scene_object_index == object_index;
                     });
    if (collision != collision_assets.end()) {
      instance.collision_body_index = static_cast<std::size_t>(
          std::distance(collision_assets.begin(), collision));
    }
    result.instances.push_back(instance);
  }
  return result;
}

RetailEngineAudio
start_retail_engine_audio(const std::filesystem::path &sound_root,
                          const SDL_AudioDeviceID device) {
  RetailEngineAudio result;
  result.device = device;
  result.layers[0U] = start_looping_audio(
      find_sibling_case_insensitive(sound_root, "GASON.WAV"), device, 0.0F,
      AudioPan::center, 75U);
  result.layers[1U] = start_looping_audio(
      find_sibling_case_insensitive(sound_root, "GASRELE.WAV"), device, 0.0F,
      AudioPan::center, 75U);
  result.layers[2U] = start_looping_audio(
      find_sibling_case_insensitive(sound_root, "GasOTL.wav"), device, 0.0F,
      AudioPan::left, 75U);
  result.layers[3U] = start_looping_audio(
      find_sibling_case_insensitive(sound_root, "GasOTR.wav"), device, 0.0F,
      AudioPan::right, 75U);
  result.layers[4U] = start_looping_audio(
      find_sibling_case_insensitive(sound_root, "GasRTL.wav"), device, 0.0F,
      AudioPan::left, 75U);
  result.layers[5U] = start_looping_audio(
      find_sibling_case_insensitive(sound_root, "GasRTR.wav"), device, 0.0F,
      AudioPan::right, 75U);
  result.gear_change = start_one_shot_audio(
      find_sibling_case_insensitive(sound_root, "Gear3.wav"), device,
      static_cast<float>(mh::game::retail_engine_audio_linear_gain(1.5)));
  return result;
}

OpponentEngineAudio
start_opponent_engine_audio(const std::filesystem::path &sound_root,
                            const SDL_AudioDeviceID device,
                            const std::size_t opponent_count) {
  OpponentEngineAudio result;
  result.device = device;
  result.layers.reserve(opponent_count);
  const auto path = find_sibling_case_insensitive(sound_root, "ENEMYENG.WAV");
  for (std::size_t opponent = 0U; opponent < opponent_count; ++opponent) {
    result.layers.push_back(start_spatial_looping_audio(path, device));
  }
  return result;
}

RaceEventAudio start_race_event_audio(const std::filesystem::path &sound_root,
                                      const SDL_AudioDeviceID device) {
  const auto start_root =
      find_child_directory_case_insensitive(sound_root, "STARTEN");
  RaceEventAudio result;
  const auto countdown_path =
      find_sibling_case_insensitive(start_root, "!32.WAV");
  const auto cue_gain =
      static_cast<float>(mh::game::original_race_cue_linear_gain());
  for (auto &countdown : result.countdown) {
    countdown = start_one_shot_audio(countdown_path, device, cue_gain);
  }
  result.go = start_one_shot_audio(
      find_sibling_case_insensitive(start_root, "GO!.WAV"), device, cue_gain);
  const auto finish_path =
      find_sibling_case_insensitive(sound_root, "Finish2.wav");
  for (auto &finish : result.finish) {
    finish = start_one_shot_audio(finish_path, device, cue_gain);
  }
  return result;
}

HornRaceAudio start_horn_race_audio(const std::filesystem::path &sample_path,
                                    const SDL_AudioDeviceID device) {
  HornRaceAudio result;
  // The CHF-selected horn object uses the normal non-spatial race-sound
  // backend and retains the sample's authored amplitude.
  result.sound = start_one_shot_audio(sample_path, device, 1.0F);
  return result;
}

bool same_material_sound_definition(
    const mh::game::OriginalMaterialSoundDefinition &left,
    const mh::game::OriginalMaterialSoundDefinition &right) {
  return ascii_lower(left.wave_file) == ascii_lower(right.wave_file) &&
         left.parameter_1 == right.parameter_1 &&
         left.parameter_2 == right.parameter_2 &&
         left.scalar_1 == right.scalar_1 && left.scalar_2 == right.scalar_2 &&
         left.parameter_5 == right.parameter_5 &&
         left.parameter_6 == right.parameter_6;
}

MaterialRaceAudio start_material_race_audio(
    const std::filesystem::path &sound_root, const SDL_AudioDeviceID device,
    const mh::game::OriginalVehicleGroundedMaterialTable &materials) {
  MaterialRaceAudio result;
  const auto definition_for = [](const auto &material,
                                 const MaterialLoopRole role)
      -> const std::optional<mh::game::OriginalMaterialSoundDefinition> & {
    switch (role) {
    case MaterialLoopRole::contact:
      return material.contact_sound;
    case MaterialLoopRole::slide:
      return material.slide_sound;
    case MaterialLoopRole::spin:
      return material.spin_sound;
    }
    throw std::logic_error("unknown material loop role");
  };

  for (std::size_t role_index = 0U; role_index < result.loops.size();
       ++role_index) {
    const auto role = static_cast<MaterialLoopRole>(role_index);
    for (std::size_t material_index = 0U; material_index < materials.size();
         ++material_index) {
      const auto &definition = definition_for(materials[material_index], role);
      if (!definition.has_value()) {
        continue;
      }
      auto found = std::find_if(result.loops[role_index].begin(),
                                result.loops[role_index].end(),
                                [&definition](const auto &binding) {
                                  return same_material_sound_definition(
                                      binding.definition, *definition);
                                });
      if (found == result.loops[role_index].end()) {
        MaterialLoopBinding binding;
        binding.definition = *definition;
        binding.audio = start_looping_audio_clip(
            load_original_material_sound_clip(sound_root, *definition), device,
            0.0F, AudioPan::center, 75U);
        result.loops[role_index].push_back(std::move(binding));
        found = std::prev(result.loops[role_index].end());
      }
      result.material_loops[role_index][material_index] =
          static_cast<std::size_t>(
              std::distance(result.loops[role_index].begin(), found));
    }
  }

  for (std::size_t role = 0U; role < result.impulses.size(); ++role) {
    for (std::size_t material_index = 0U; material_index < materials.size();
         ++material_index) {
      const auto &definition =
          role == 0U ? materials[material_index].weak_impulse_sound
                     : materials[material_index].hard_impulse_sound;
      if (!definition.has_value()) {
        continue;
      }
      auto found = std::find_if(result.impulses[role].begin(),
                                result.impulses[role].end(),
                                [&definition](const auto &binding) {
                                  return same_material_sound_definition(
                                      binding.definition, *definition);
                                });
      if (found == result.impulses[role].end()) {
        MaterialOneShotBinding binding;
        binding.definition = *definition;
        binding.audio = start_one_shot_audio_clip(
            load_original_material_sound_clip(sound_root, *definition), device,
            0.0F);
        require(SDL_SetAudioStreamFrequencyRatio(binding.audio.stream.get(),
                                                 definition->scalar_2),
                "set authored material impulse pitch");
        result.impulses[role].push_back(std::move(binding));
        found = std::prev(result.impulses[role].end());
      }
      result.material_impulses[role][material_index] = static_cast<std::size_t>(
          std::distance(result.impulses[role].begin(), found));
    }
  }

  for (std::size_t material_index = 0U; material_index < materials.size();
       ++material_index) {
    const auto &definition = materials[material_index].scratch_sound;
    if (!definition.has_value()) {
      continue;
    }
    auto found = std::find_if(result.scratches.begin(), result.scratches.end(),
                              [&definition](const auto &binding) {
                                return same_material_sound_definition(
                                    binding.definition, *definition);
                              });
    if (found == result.scratches.end()) {
      MaterialScratchBinding binding;
      binding.definition = *definition;
      auto clip = load_original_material_sound_clip(sound_root, *definition);
      binding.audio[0U] =
          start_looping_audio_clip(clip, device, 0.0F, AudioPan::left, 75U);
      binding.audio[1U] = start_looping_audio_clip(std::move(clip), device,
                                                   0.0F, AudioPan::right, 75U);
      result.scratches.push_back(std::move(binding));
      found = std::prev(result.scratches.end());
    }
    result.material_scratches[material_index] = static_cast<std::size_t>(
        std::distance(result.scratches.begin(), found));
  }

  // These three non-material names are exact p3.1 constants used only by the
  // SlideType-1 branch and its alternating release sound.
  result.special_slide = start_looping_audio(
      find_sibling_case_insensitive(sound_root, "slide1.wav"), device, 0.0F,
      AudioPan::center, 75U);
  result.special_slide_exit[0U] = start_one_shot_audio(
      find_sibling_case_insensitive(sound_root, "slidee1.wav"), device, 0.0F);
  result.special_slide_exit[1U] = start_one_shot_audio(
      find_sibling_case_insensitive(sound_root, "slidee2.wav"), device, 0.0F);
  return result;
}

EnvironmentRaceAudio start_environment_race_audio(
    const std::filesystem::path &sound_root, const SDL_AudioDeviceID device,
    const mh::game::OriginalEnvironmentSoundTable &definitions,
    const mh::game::OriginalEnvironmentScene &scene) {
  EnvironmentRaceAudio result;
  result.scene = scene;
  result.fixed_loops.reserve(definitions.fixed_loops.size());
  for (const auto &definition : definitions.fixed_loops) {
    EnvironmentLoopBinding binding;
    binding.definition = definition;
    binding.audio = start_spatial_looping_audio(
        find_sibling_case_insensitive(sound_root, definition.wave_file),
        device);
    result.fixed_loops.push_back(std::move(binding));
  }
  for (const auto &definition : definitions.object_loops) {
    for (std::size_t object_index = 0U;
         object_index < result.scene.objects.size(); ++object_index) {
      const auto identifier =
          result.scene.objects[object_index].environment_identifier;
      if (!identifier.has_value() || definition.object_number < 0 ||
          *identifier != static_cast<std::uint32_t>(definition.object_number)) {
        continue;
      }
      EnvironmentObjectLoopBinding binding;
      binding.definition = definition;
      binding.scene_object_index = object_index;
      binding.audio = start_spatial_looping_audio(
          find_sibling_case_insensitive(sound_root, definition.wave_file),
          device);
      result.object_loops.push_back(std::move(binding));
    }
  }
  for (const auto &definition : definitions.object_collisions) {
    if (definition.object_number < 0 ||
        definition.object_number >=
            static_cast<std::int32_t>(result.collision_slots.size())) {
      throw std::runtime_error(
          "ObjectColSound identifier exceeds the original 0..15 slots");
    }
    EnvironmentObjectCollisionBinding binding;
    binding.definition = definition;
    const auto sound_path =
        find_sibling_case_insensitive(sound_root, definition.wave_file);
    for (auto &audio : binding.audio) {
      audio = start_one_shot_audio(sound_path, device, 0.0F);
    }
    const auto binding_index = result.object_collisions.size();
    result.object_collisions.push_back(std::move(binding));
    // Initializer RVA 0x000b36f6 overwrites a previously populated slot, so
    // duplicate definitions intentionally retain the last authored record.
    result.collision_slots[static_cast<std::size_t>(definition.object_number)] =
        binding_index;
  }
  return result;
}

std::string asset_name(const mh::content::TextureAsset &asset) {
  if (!asset.entry_name.empty()) {
    return ascii_lower(asset.entry_name);
  }
  return filename_lower(std::filesystem::path(asset.source_path));
}

const mh::content::TextureAsset &
select_track_texture(const mh::content::TextureCatalog &catalog,
                     const std::string &material_name,
                     const std::string &archive_name) {
  std::vector<const mh::content::TextureAsset *> candidates;
  for (const auto &asset : catalog.assets) {
    if (asset_name(asset) != ascii_lower(material_name)) {
      continue;
    }
    if (asset.source_kind == "pdi-iff" &&
        filename_lower(std::filesystem::path(asset.source_path)) !=
            ascii_lower(archive_name)) {
      continue;
    }
    candidates.push_back(&asset);
  }
  if (candidates.size() != 1U) {
    throw std::runtime_error("Goldbridge material does not resolve uniquely: " +
                             material_name);
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
  require(texture != nullptr, "create Goldbridge material texture");
  require(SDL_UpdateTexture(texture.get(), nullptr, image.rgba.data(),
                            static_cast<int>(image.width * 4U)),
          "upload Goldbridge material texture");
  require(SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_BLEND),
          "set Goldbridge material blend mode");
  require(SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_NEAREST),
          "set Goldbridge material scale mode");
  return texture;
}

HudTexture load_hud_texture(SDL_Renderer *renderer,
                            const std::filesystem::path &directory,
                            const std::string &filename) {
  const auto source =
      mh::content::read_tga(find_sibling_case_insensitive(directory, filename));
  mh::content::PamRgbaImage image;
  image.width = source.width;
  image.height = source.height;
  image.palette_indices = source.palette_indices;
  image.rgba = source.rgba;
  for (std::size_t pixel = 0U; pixel < image.rgba.size(); pixel += 4U) {
    if (image.rgba[pixel] >= 240U && image.rgba[pixel + 1U] <= 24U &&
        image.rgba[pixel + 2U] >= 240U) {
      image.rgba[pixel + 3U] = 0U;
    }
  }
  return {image.width, image.height, upload_texture(renderer, image)};
}

HudTexture load_hud_font_texture(SDL_Renderer *renderer,
                                 const mh::content::FntData &font,
                                 std::array<float, 256U> &advances) {
  constexpr std::uint32_t cell_width = 24U;
  constexpr std::uint32_t cell_height = 16U;
  constexpr std::uint32_t columns = 16U;
  constexpr std::uint32_t rows = 16U;
  const auto atlas_width = cell_width * columns;
  const auto atlas_height = cell_height * rows;
  std::vector<std::uint8_t> mask(static_cast<std::size_t>(atlas_width) *
                                 atlas_height);
  for (std::uint32_t codepoint = 1U; codepoint < 256U; ++codepoint) {
    const std::array character{
        static_cast<char>(static_cast<unsigned char>(codepoint))};
    const auto cell_x = (codepoint % columns) * cell_width;
    const auto cell_y = (codepoint / columns) * cell_height;
    mh::content::render_fnt_text_8bit(
        font, std::string_view(character.data(), character.size()), mask,
        atlas_width, atlas_height, atlas_width, cell_x + 8U, cell_y, 255U);
    advances[codepoint] =
        static_cast<float>(font.glyphs[codepoint].advance_width);
  }
  mh::content::PamRgbaImage image;
  image.width = atlas_width;
  image.height = atlas_height;
  image.rgba.resize(mask.size() * 4U);
  for (std::size_t pixel = 0U; pixel < mask.size(); ++pixel) {
    image.rgba[pixel * 4U] = 255U;
    image.rgba[pixel * 4U + 1U] = 255U;
    image.rgba[pixel * 4U + 2U] = 255U;
    image.rgba[pixel * 4U + 3U] = mask[pixel];
  }
  return {image.width, image.height, upload_texture(renderer, image)};
}

RaceHudVisual load_race_hud(SDL_Renderer *renderer,
                            const std::filesystem::path &content_root,
                            const mh::content::AiRouteData &route) {
  const auto directory = content_root / "Data" / "d3d";
  RaceHudVisual result;
  result.meter_active = load_hud_texture(renderer, directory, "meter00.tga");
  result.meter_inactive = load_hud_texture(renderer, directory, "meter01.tga");
  result.gear_label = load_hud_texture(renderer, directory, "TGEAR00.TGA");
  result.speed_unit_metric =
      load_hud_texture(renderer, directory, "TKMH00.TGA");
  result.speed_unit_imperial =
      load_hud_texture(renderer, directory, "Tkmh01.TGA");
  for (std::size_t digit = 0U; digit < 10U; ++digit) {
    result.large_digits[digit] = load_hud_texture(
        renderer, directory, "bnum" + std::to_string(digit) + "00.tga");
    result.small_digits[digit] = load_hud_texture(
        renderer, directory, "snum" + std::to_string(digit) + "00.tga");
  }
  result.reverse = load_hud_texture(renderer, directory, "snumr00.tga");
  result.colon = load_hud_texture(renderer, directory, "kolon00.tga");
  result.total_label = load_hud_texture(renderer, directory, "total.tga");
  result.time_label = load_hud_texture(renderer, directory, "time.tga");
  result.best_label = load_hud_texture(renderer, directory, "best.tga");
  result.lap_label = load_hud_texture(renderer, directory, "lap.tga");
  result.position_label = load_hud_texture(renderer, directory, "pos.tga");
  result.player_marker = load_hud_texture(renderer, directory, "plupp.tga");
  result.avenger_marker = load_hud_texture(renderer, directory, "avenger.tga");
  result.opponent_marker =
      load_hud_texture(renderer, directory, "pluppother.tga");
  result.ghost_marker = load_hud_texture(renderer, directory, "ghost00.tga");
  const auto font = mh::content::read_fnt(content_root / "Data" / "FONT1S.FNT");
  result.font_line_metric = static_cast<float>(font.line_metric);
  result.font = load_hud_font_texture(renderer, font, result.font_advances);
  for (std::size_t phase = 0U; phase < result.countdown.size(); ++phase) {
    result.countdown[phase] = load_hud_texture(
        renderer, directory, "count0" + std::to_string(phase) + ".tga");
  }
  if (route.samples.empty()) {
    throw std::runtime_error("race HUD minimap requires a nonempty AI route");
  }
  result.map_min = route.samples.front().position;
  result.map_max = route.samples.front().position;
  for (const auto &sample : route.samples) {
    for (std::size_t axis = 0U; axis < 2U; ++axis) {
      result.map_min[axis] =
          std::min(result.map_min[axis], sample.position[axis]);
      result.map_max[axis] =
          std::max(result.map_max[axis], sample.position[axis]);
    }
  }
  return result;
}

SdlPointer<SDL_Texture, SDL_DestroyTexture>
upload_fog_mask(SDL_Renderer *renderer,
                const mh::content::PamRgbaImage &source) {
  auto mask = source;
  for (std::size_t pixel = 0U; pixel < mask.rgba.size(); pixel += 4U) {
    mask.rgba[pixel] = 255U;
    mask.rgba[pixel + 1U] = 255U;
    mask.rgba[pixel + 2U] = 255U;
  }
  return upload_texture(renderer, mask);
}

SdlPointer<SDL_Texture, SDL_DestroyTexture> upload_vehicle_environment_texture(
    SDL_Renderer *renderer, const std::filesystem::path &path,
    const float color_scale, mh::content::PamRgbaImage &retained_image) {
  if (!std::isfinite(color_scale) || color_scale < 0.0F ||
      color_scale > 16.0F) {
    throw std::runtime_error(
        "vehicle environment-map color scale is outside its safe range");
  }
  const auto source = mh::content::read_iff_ilbm(path);
  mh::content::PamRgbaImage image;
  image.width = source.width;
  image.height = source.height;
  image.rgba = source.rgba;
  for (std::size_t pixel = 0U; pixel < image.rgba.size(); pixel += 4U) {
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
      image.rgba[pixel + channel] = static_cast<std::uint8_t>(std::clamp(
          std::lround(static_cast<float>(image.rgba[pixel + channel]) *
                      color_scale),
          0L, 255L));
    }
  }
  auto texture = upload_texture(renderer, image);
  require(SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_NONE),
          "set vehicle environment-map blend mode");
  require(SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_LINEAR),
          "set vehicle environment-map scale mode");
  retained_image = std::move(image);
  return texture;
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
    if (name.starts_with("env_") || name.starts_with("envphong_")) {
      if (result.environment != nullptr) {
        throw std::runtime_error(
            "active track definition has multiple base environment maps");
      }
      result.environment = upload_vehicle_environment_texture(
          renderer, path, 1.0F, result.environment_image);
    } else if (name.starts_with("ref_")) {
      if (result.reflection != nullptr) {
        throw std::runtime_error(
            "active track definition has multiple reflection maps");
      }
      result.reflection = upload_vehicle_environment_texture(
          renderer, path, 1.0F, result.reflection_image);
    } else if (name.starts_with("phong_")) {
      if (result.phong != nullptr) {
        throw std::runtime_error(
            "active track definition has multiple phong maps");
      }
      result.phong = upload_vehicle_environment_texture(renderer, path, 1.0F,
                                                        result.phong_image);
    }
  }
  if (result.environment == nullptr || result.reflection == nullptr ||
      result.phong == nullptr) {
    throw std::runtime_error(
        "active track definition does not provide the recovered three-map "
        "vehicle material set");
  }
  return result;
}

BackgroundVisual load_background(SDL_Renderer *renderer,
                                 const std::filesystem::path &path) {
  const auto image = mh::content::read_tga(path);
  mh::content::PamRgbaImage rgba;
  rgba.width = image.width;
  rgba.height = image.height;
  rgba.rgba = image.rgba;
  BackgroundVisual result;
  result.width = rgba.width;
  result.height = rgba.height;
  result.texture = upload_texture(renderer, rgba);
  require(SDL_SetTextureBlendMode(result.texture.get(), SDL_BLENDMODE_NONE),
          "set Goldbridge background blend mode");
  return result;
}

void render_background(SDL_Renderer *renderer,
                       const BackgroundVisual &background,
                       const PerspectiveView &view, const int width,
                       const int height, const float authored_horizon,
                       const mh::content::TrackRgbColor top_color,
                       const bool thunder_mode = false,
                       const double effect_seconds = 0.0,
                       const bool thunder_flash = false) {
  // The authored panel is 640 pixels wide. Preserve its 196/480 occupancy
  // from that 4:3 source canvas instead of stretching it over the viewport.
  const auto source_canvas_height =
      static_cast<float>(background.width) * 3.0F / 4.0F;
  if (source_canvas_height <= 0.0F) {
    throw std::runtime_error("Goldbridge background has invalid dimensions");
  }
  const auto destination_height = std::min(
      static_cast<float>(height), static_cast<float>(height) *
                                      static_cast<float>(background.height) /
                                      source_canvas_height);
  // The accepted cylindrical adapter previously placed source row 96 on the
  // 640x480 canvas's row 96, equivalent to Horizon 0.2. Apply the exact p3.1
  // accelerated backend's rounded screen-row displacement relative to that
  // baseline; world geometry retains its independently recovered projection.
  const auto destination_y = static_cast<float>(
      mh::game::original_accelerated_horizon_offset(
          authored_horizon, static_cast<std::uint32_t>(height)) -
      mh::game::original_accelerated_horizon_offset(
          0.2F, static_cast<std::uint32_t>(height)));
  // The accelerated owner retains separate TopColour and BottomColour
  // background vertices. The frame is already cleared to BottomColour; cover
  // only a positive upper gap exposed by a lowered authored panorama with the
  // track's TopColour before drawing the opaque sky panel.
  if (destination_y > 0.0F) {
    const auto thunder_channel = [&](const std::uint8_t channel,
                                     const bool red) {
      if (!thunder_mode || thunder_flash || red) {
        return channel;
      }
      return static_cast<std::uint8_t>(
          (static_cast<std::uint32_t>(channel) * 128U) / 255U);
    };
    require(
        SDL_SetRenderDrawColor(renderer, thunder_channel(top_color.red, true),
                               thunder_channel(top_color.green, false),
                               thunder_channel(top_color.blue, false), 255U),
        "set upper background color");
    const SDL_FRect upper_gap{
        0.0F, 0.0F, static_cast<float>(width),
        std::min(destination_y, static_cast<float>(height))};
    require(SDL_RenderFillRect(renderer, &upper_gap),
            "render upper background color");
  }
  auto background_forward_x = view.forward[0U];
  auto background_forward_z = view.forward[2U];
  if (thunder_mode) {
    // p3.1 RVA 0x0005ce20 selects 0.005 radians per millisecond for Thunder.
    const auto angle = effect_seconds * 5.0;
    const auto cosine = std::cos(angle);
    const auto sine = std::sin(angle);
    const auto rotated_x =
        background_forward_x * cosine - background_forward_z * sine;
    background_forward_z =
        background_forward_x * sine + background_forward_z * cosine;
    background_forward_x = rotated_x;
    require(SDL_SetTextureColorMod(background.texture.get(), 255U,
                                   thunder_flash ? 255U : 128U,
                                   thunder_flash ? 255U : 128U),
            "set Thunder background modulation");
  }
  const auto window = mh::game::make_cylindrical_sky_window(
      background_forward_x, background_forward_z,
      static_cast<double>(background.width), static_cast<double>(width),
      static_cast<double>(view.focal_length));
  const auto first_span =
      std::min(window.source_span,
               static_cast<double>(background.width) - window.source_start);
  const auto first_destination_width =
      static_cast<double>(width) * first_span / window.source_span;
  const auto render_slice =
      [&](const double source_start, const double source_span,
          const double destination_start, const double destination_width) {
        const SDL_FRect source{static_cast<float>(source_start), 0.0F,
                               static_cast<float>(source_span),
                               static_cast<float>(background.height)};
        const SDL_FRect destination{
            static_cast<float>(destination_start), destination_y,
            static_cast<float>(destination_width), destination_height};
        require(SDL_RenderTexture(renderer, background.texture.get(), &source,
                                  &destination),
                "render Goldbridge background");
      };
  render_slice(window.source_start, first_span, 0.0, first_destination_width);
  const auto remaining_span = window.source_span - first_span;
  if (remaining_span > 1.0e-9) {
    render_slice(0.0, remaining_span, first_destination_width,
                 static_cast<double>(width) - first_destination_width);
  }
  if (thunder_mode) {
    require(SDL_SetTextureColorMod(background.texture.get(), 255U, 255U, 255U),
            "restore background modulation");
  }
}

void render_horizon_fog(SDL_Renderer *renderer, const int width,
                        const int height, const float horizon_y,
                        const std::array<float, 3U> cue_color) {
  const auto bounded_horizon =
      std::clamp(horizon_y, 0.0F, static_cast<float>(height));
  const auto top = std::max(0.0F, bounded_horizon - height * 0.23F);
  const auto bottom =
      std::min(static_cast<float>(height), bounded_horizon + height * 0.20F);
  const SDL_FColor clear{cue_color[0U] / 255.0F, cue_color[1U] / 255.0F,
                         cue_color[2U] / 255.0F, 0.0F};
  const SDL_FColor dense{clear.r, clear.g, clear.b, 0.92F};
  const std::array<SDL_Vertex, 6U> vertices{{
      {{0.0F, top}, clear, {}},
      {{static_cast<float>(width), top}, clear, {}},
      {{0.0F, bounded_horizon}, dense, {}},
      {{static_cast<float>(width), bounded_horizon}, dense, {}},
      {{0.0F, bottom}, clear, {}},
      {{static_cast<float>(width), bottom}, clear, {}},
  }};
  constexpr std::array<int, 12U> indices{0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4};
  SDL_BlendMode previous = SDL_BLENDMODE_NONE;
  require(SDL_GetRenderDrawBlendMode(renderer, &previous),
          "query horizon fog blend mode");
  require(SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND),
          "set horizon fog blend mode");
  require(SDL_RenderGeometry(renderer, nullptr, vertices.data(),
                             static_cast<int>(vertices.size()), indices.data(),
                             static_cast<int>(indices.size())),
          "render horizon fog veil");
  require(SDL_SetRenderDrawBlendMode(renderer, previous),
          "restore horizon fog blend mode");
}

enum class HudGroupAnchor : std::uint8_t { automatic, top_right };

SDL_FRect hud_rect(const HudTexture &texture, const float x, const float y,
                   const int width, const int height,
                   const HudGroupAnchor anchor = HudGroupAnchor::automatic) {
  const auto viewport = mh::game::original_hud_viewport(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  const auto scale = static_cast<float>(viewport.scale);
  const auto anchored_x =
      anchor == HudGroupAnchor::top_right
          ? x * scale + static_cast<float>(viewport.offset_x * 2.0)
          : static_cast<float>(mh::game::original_hud_x(viewport, x));
  const auto anchored_y =
      anchor == HudGroupAnchor::top_right
          ? y * scale
          : static_cast<float>(mh::game::original_hud_y(viewport, y));
  return {anchored_x, anchored_y, static_cast<float>(texture.width) * scale,
          static_cast<float>(texture.height) * scale};
}

void draw_hud_texture(SDL_Renderer *renderer, const HudTexture &texture,
                      const float x, const float y, const int width,
                      const int height,
                      const HudGroupAnchor anchor = HudGroupAnchor::automatic) {
  const auto destination = hud_rect(texture, x, y, width, height, anchor);
  require(
      SDL_RenderTexture(renderer, texture.texture.get(), nullptr, &destination),
      "render original race HUD sprite");
}

void draw_hud_font_text(SDL_Renderer *renderer, const RaceHudVisual &hud,
                        const std::string_view text, const float x,
                        const float y, const std::array<std::uint8_t, 3U> color,
                        const int width, const int height,
                        const bool centered_canvas = false) {
  // This atlas is generated locally from the decoded retail FONT1S.FNT.
  constexpr float glyph_width = 24.0F;
  constexpr float glyph_height = 16.0F;
  const auto viewport = mh::game::original_hud_viewport(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  const auto scale = static_cast<float>(viewport.scale);
  auto text_width = 0.0F;
  for (const auto raw_character : text) {
    const auto character =
        static_cast<std::uint8_t>(static_cast<unsigned char>(raw_character));
    text_width += hud.font_advances[character];
  }
  const auto group_center = x + text_width * 0.5F;
  const auto horizontal_anchor =
      centered_canvas
          ? static_cast<float>(viewport.offset_x)
          : static_cast<float>(mh::game::original_hud_x(viewport, group_center)) -
                group_center * scale;
  require(SDL_SetTextureColorMod(hud.font.texture.get(), color[0U], color[1U],
                                 color[2U]),
          "set original race HUD font color");
  auto cursor = x;
  for (const auto raw_character : text) {
    const auto character =
        static_cast<std::uint8_t>(static_cast<unsigned char>(raw_character));
    const auto atlas_index = static_cast<std::uint32_t>(character);
    const SDL_FRect source{static_cast<float>(atlas_index % 16U) * glyph_width,
                           static_cast<float>(atlas_index / 16U) * glyph_height,
                           glyph_width, glyph_height};
    const SDL_FRect destination{
        // The generated atlas gives every decoded glyph an eight-pixel guard
        // for the FNT program's signed horizontal stores. The caller's X is
        // the retail pen position, so place that guard to its left.
        horizontal_anchor + (cursor - 8.0F) * scale,
        centered_canvas ? static_cast<float>(viewport.offset_y) + y * scale
                        : static_cast<float>(mh::game::original_hud_y(viewport, y)),
        glyph_width * scale, glyph_height * scale};
    require(SDL_RenderTexture(renderer, hud.font.texture.get(), &source,
                              &destination),
            "render original race HUD font glyph");
    cursor += hud.font_advances[character];
  }
  require(SDL_SetTextureColorMod(hud.font.texture.get(), 255U, 255U, 255U),
          "restore original race HUD font color");
}

void draw_hud_font_glyph_scaled(SDL_Renderer *renderer,
                                const RaceHudVisual &hud,
                                const char raw_character, const float x,
                                const float y, const float glyph_scale,
                                const std::array<std::uint8_t, 3U> color,
                                const int width, const int height,
                                const bool centered_canvas = false) {
  constexpr float glyph_width = 24.0F;
  constexpr float glyph_height = 16.0F;
  const auto viewport = mh::game::original_hud_viewport(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  const auto scale = static_cast<float>(viewport.scale);
  const auto character =
      static_cast<std::uint8_t>(static_cast<unsigned char>(raw_character));
  const auto atlas_index = static_cast<std::uint32_t>(character);
  const SDL_FRect source{static_cast<float>(atlas_index % 16U) * glyph_width,
                         static_cast<float>(atlas_index / 16U) * glyph_height,
                         glyph_width, glyph_height};
  const SDL_FRect destination{
      centered_canvas
          ? static_cast<float>(viewport.offset_x) + (x - 8.0F * glyph_scale) * scale
          : static_cast<float>(
                mh::game::original_hud_x(viewport, x - 8.0F * glyph_scale)),
      centered_canvas ? static_cast<float>(viewport.offset_y) + y * scale
                      : static_cast<float>(mh::game::original_hud_y(viewport, y)),
      glyph_width * glyph_scale * scale, glyph_height * glyph_scale * scale};
  require(SDL_SetTextureColorMod(hud.font.texture.get(), color[0U], color[1U],
                                 color[2U]),
          "set scaled original race HUD font color");
  require(SDL_RenderTexture(renderer, hud.font.texture.get(), &source,
                            &destination),
          "render scaled original race HUD font glyph");
  require(SDL_SetTextureColorMod(hud.font.texture.get(), 255U, 255U, 255U),
          "restore scaled original race HUD font color");
}

float hud_font_text_width(const RaceHudVisual &hud,
                          const std::string_view text) {
  auto width = 0.0F;
  for (const auto raw_character : text) {
    const auto character =
        static_cast<std::uint8_t>(static_cast<unsigned char>(raw_character));
    width += hud.font_advances[character];
  }
  return width;
}

void draw_vehicle_name_plate_text(SDL_Renderer *renderer,
                                  const RaceHudVisual &hud,
                                  const std::string_view text,
                                  const float screen_x, const float screen_y,
                                  const float scale,
                                  const std::array<std::uint8_t, 3U> color) {
  constexpr float glyph_width = 24.0F;
  constexpr float glyph_height = 16.0F;
  require(SDL_SetTextureColorMod(hud.font.texture.get(), color[0U], color[1U],
                                 color[2U]),
          "set vehicle nameplate font color");
  auto cursor = screen_x;
  for (const auto raw_character : text) {
    const auto character =
        static_cast<std::uint8_t>(static_cast<unsigned char>(raw_character));
    const auto atlas_index = static_cast<std::uint32_t>(character);
    const SDL_FRect source{static_cast<float>(atlas_index % 16U) * glyph_width,
                           static_cast<float>(atlas_index / 16U) * glyph_height,
                           glyph_width, glyph_height};
    const SDL_FRect destination{cursor - 8.0F * scale, screen_y,
                                glyph_width * scale, glyph_height * scale};
    require(SDL_RenderTexture(renderer, hud.font.texture.get(), &source,
                              &destination),
            "render vehicle nameplate font glyph");
    cursor += hud.font_advances[character] * scale;
  }
  require(SDL_SetTextureColorMod(hud.font.texture.get(), 255U, 255U, 255U),
          "restore vehicle nameplate font color");
}

struct VehicleNamePlateOccluder {
  const CarVisual *visual = nullptr;
  mh::game::OriginalBodyPoseState pose{};
};

bool ray_intersects_vehicle_before(
    const mh::game::CollisionVector3 &ray_origin,
    const mh::game::CollisionVector3 &ray_direction,
    const double target_distance, const VehicleNamePlateOccluder &occluder) {
  if (occluder.visual == nullptr) {
    return false;
  }
  const mh::game::CollisionVector3 relative{
      ray_origin[0U] - occluder.pose.world_position[0U],
      ray_origin[1U] - occluder.pose.world_position[1U],
      ray_origin[2U] - occluder.pose.world_position[2U]};
  mh::game::CollisionVector3 local_origin{};
  mh::game::CollisionVector3 local_direction{};
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    for (std::size_t world_axis = 0U; world_axis < 3U; ++world_axis) {
      local_origin[axis] +=
          relative[world_axis] * occluder.pose.body_basis[axis][world_axis];
      local_direction[axis] += ray_direction[world_axis] *
                               occluder.pose.body_basis[axis][world_axis];
    }
  }
  auto entry = 0.0;
  auto exit = target_distance;
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    const auto minimum = occluder.visual->body_minimum[axis] - 0.08;
    const auto maximum = occluder.visual->body_maximum[axis] + 0.08;
    if (std::abs(local_direction[axis]) <= 1.0e-8) {
      if (local_origin[axis] < minimum || local_origin[axis] > maximum) {
        return false;
      }
      continue;
    }
    auto first = (minimum - local_origin[axis]) / local_direction[axis];
    auto second = (maximum - local_origin[axis]) / local_direction[axis];
    if (first > second) {
      std::swap(first, second);
    }
    entry = std::max(entry, first);
    exit = std::min(exit, second);
    if (entry > exit) {
      return false;
    }
  }
  return exit > 0.0 && entry < target_distance - 0.12;
}

std::optional<SDL_FRect> render_vehicle_name_plate(
    SDL_Renderer *renderer, const RaceHudVisual &hud,
    const std::string_view name, const std::array<std::uint8_t, 3U> color,
    const CarVisual &visual, const mh::game::OriginalBodyPoseState &pose,
    const mh::game::CollisionWorld &collision_world,
    const PerspectiveView &view, const mh::ui::GraphicNamePlateMode mode,
    const std::span<const VehicleNamePlateOccluder> nearer_vehicles,
    const std::span<const SDL_FRect> nearer_vehicle_bounds, const int width,
    const int height) {
  if (mode == mh::ui::GraphicNamePlateMode::none || name.empty()) {
    return std::nullopt;
  }
  // A fixed world-up offset can separate a plate from its car when the body is
  // pitched, banked, or viewed obliquely. Project the eight corners of the
  // selected car's actual body bounds and attach the plate to the top-centre of
  // that screen-space silhouette instead.
  auto minimum_x = std::numeric_limits<float>::infinity();
  auto maximum_x = -std::numeric_limits<float>::infinity();
  auto minimum_y = std::numeric_limits<float>::infinity();
  auto maximum_y = -std::numeric_limits<float>::infinity();
  for (std::size_t corner = 0U; corner < 8U; ++corner) {
    const mh::game::CollisionVector3 local{
        (corner & 1U) != 0U ? visual.body_maximum[0U] : visual.body_minimum[0U],
        (corner & 2U) != 0U ? visual.body_maximum[1U] : visual.body_minimum[1U],
        (corner & 4U) != 0U ? visual.body_maximum[2U]
                            : visual.body_minimum[2U]};
    const auto camera =
        to_view(view, mh::game::project_body_point_to_world(pose, local));
    if (!std::isfinite(camera.x) || !std::isfinite(camera.y) ||
        !std::isfinite(camera.z) || camera.z <= view.near_plane) {
      return std::nullopt;
    }
    const auto projected = project(view, camera);
    if (!std::isfinite(projected.x) || !std::isfinite(projected.y)) {
      return std::nullopt;
    }
    minimum_x = std::min(minimum_x, projected.x);
    maximum_x = std::max(maximum_x, projected.x);
    minimum_y = std::min(minimum_y, projected.y);
    maximum_y = std::max(maximum_y, projected.y);
  }
  const SDL_FRect body_bounds{minimum_x, minimum_y, maximum_x - minimum_x,
                              maximum_y - minimum_y};
  // A partially/off-screen opponent was the main source of apparently free
  // floating names: its body was clipped away but the fixed overlay survived.
  // Require the complete projected body silhouette before showing its plate,
  // while retaining its bounds so it can still occlude labels behind it.
  if (minimum_x < 0.0F || maximum_x >= static_cast<float>(width) ||
      minimum_y < 0.0F || maximum_y >= static_cast<float>(height)) {
    return body_bounds;
  }
  for (const auto &nearer : nearer_vehicle_bounds) {
    const auto overlap_left = std::max(body_bounds.x, nearer.x);
    const auto overlap_top = std::max(body_bounds.y, nearer.y);
    const auto overlap_right =
        std::min(body_bounds.x + body_bounds.w, nearer.x + nearer.w);
    const auto overlap_bottom =
        std::min(body_bounds.y + body_bounds.h, nearer.y + nearer.h);
    const auto overlap_width = std::max(0.0F, overlap_right - overlap_left);
    const auto overlap_height = std::max(0.0F, overlap_bottom - overlap_top);
    const auto overlap = overlap_width * overlap_height;
    const auto body_area = std::max(1.0F, body_bounds.w * body_bounds.h);
    const auto horizontal_overlap_fraction =
        overlap_width / std::max(1.0F, std::min(body_bounds.w, nearer.w));
    const auto vertical_gap =
        std::max({0.0F, body_bounds.y - (nearer.y + nearer.h),
                  nearer.y - (body_bounds.y + body_bounds.h)});
    const auto close_in_depth_projection =
        horizontal_overlap_fraction >= 0.35F &&
        vertical_gap <= std::max(body_bounds.h, nearer.h) * 0.35F;
    if (overlap / body_area >= 0.12F || close_in_depth_projection) {
      // Keep this rectangle as an occluder for still farther cars, but do not
      // draw a name for a body that is mostly covered by a nearer vehicle.
      return body_bounds;
    }
  }
  const mh::game::CollisionVector3 roof_center_local{
      (visual.body_minimum[0U] + visual.body_maximum[0U]) * 0.5,
      visual.body_maximum[1U],
      (visual.body_minimum[2U] + visual.body_maximum[2U]) * 0.5};
  const auto roof_center =
      mh::game::project_body_point_to_world(pose, roof_center_local);
  const mh::game::CollisionVector3 camera_to_roof{
      roof_center[0U] - view.position[0U], roof_center[1U] - view.position[1U],
      roof_center[2U] - view.position[2U]};
  const auto roof_distance = std::sqrt(camera_to_roof[0U] * camera_to_roof[0U] +
                                       camera_to_roof[1U] * camera_to_roof[1U] +
                                       camera_to_roof[2U] * camera_to_roof[2U]);
  if (roof_distance > 1.0e-6) {
    const mh::game::CollisionVector3 direction{
        camera_to_roof[0U] / roof_distance, camera_to_roof[1U] / roof_distance,
        camera_to_roof[2U] / roof_distance};
    const auto obstruction =
        collision_world.raycast(view.position, direction, roof_distance);
    if (obstruction.has_value() &&
        obstruction->distance < roof_distance - 0.12) {
      return body_bounds;
    }
    if (std::any_of(nearer_vehicles.begin(), nearer_vehicles.end(),
                    [&](const auto &occluder) {
                      return ray_intersects_vehicle_before(
                          view.position, direction, roof_distance, occluder);
                    })) {
      return body_bounds;
    }
  }
  const auto viewport = mh::game::original_hud_viewport(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  const auto scale = static_cast<float>(viewport.scale);
  const auto text_width = hud_font_text_width(hud, name);
  const mh::game::CollisionVector3 roof_anchor_local{
      0.0, visual.body_maximum[1U], 0.0};
  const auto roof_anchor_view = to_view(
      view, mh::game::project_body_point_to_world(pose, roof_anchor_local));
  const auto roof_anchor_projected = project(view, roof_anchor_view);
  const auto projected_x = roof_anchor_projected.x;
  const auto projected_y = minimum_y - 3.0F * scale;
  // Do not clamp a detached label onto the viewport edge. If there is no room
  // for the complete plate above the visible body, omit it for that frame.
  if (projected_x - text_width * scale * 0.5F < 0.0F ||
      projected_x + text_width * scale * 0.5F >= static_cast<float>(width) ||
      projected_y < 0.0F || projected_y >= static_cast<float>(height)) {
    return body_bounds;
  }
  const auto screen_x = projected_x - text_width * scale * 0.5F;
  if (mode == mh::ui::GraphicNamePlateMode::flat) {
    const SDL_FRect plate{
        projected_x - text_width * scale * 0.5F - 3.0F * scale,
        projected_y - 1.0F * scale, (text_width + 6.0F) * scale,
        std::max(1.0F, hud.font_line_metric + 2.0F) * scale};
    require(SDL_SetRenderDrawColor(renderer, 0U, 0U, 0U, 210U),
            "set flat vehicle nameplate color");
    require(SDL_RenderFillRect(renderer, &plate),
            "render flat vehicle nameplate");
  }
  // Retail name plates are translucent overlays.  Keeping the team colour
  // while lowering coverage lets lamp glare and scenery remain visible
  // through the letters, as in the original accelerated renderer.
  require(SDL_SetTextureAlphaMod(hud.font.texture.get(), 112U),
          "set vehicle nameplate shadow alpha");
  draw_vehicle_name_plate_text(renderer, hud, name, screen_x + scale,
                               projected_y + scale, scale, {0U, 0U, 0U});
  require(SDL_SetTextureAlphaMod(hud.font.texture.get(), 168U),
          "set vehicle nameplate color alpha");
  draw_vehicle_name_plate_text(renderer, hud, name, screen_x, projected_y,
                               scale, color);
  require(SDL_SetTextureAlphaMod(hud.font.texture.get(), 255U),
          "restore vehicle nameplate alpha");
  return body_bounds;
}

void render_original_mde_notice(
    SDL_Renderer *renderer, const RaceHudVisual &hud,
    const mh::game::OriginalMdeVisibleNotice &notice, const int width,
    const int height) {
  const auto y = static_cast<float>(
      mh::game::original_mde_notice_stack_top_y +
      notice.vertical_slot * mh::game::original_mde_notice_line_metric);
  draw_hud_font_text(renderer, hud, notice.text, 8.0F, y,
                     mh::game::original_mde_notice_rgb, width, height);
}

std::string format_hud_gap(const double seconds, const char sign) {
  const auto centiseconds = static_cast<std::uint64_t>(
      std::clamp(std::llround(std::fabs(seconds) * 100.0), 0LL, 599999LL));
  const auto minutes = centiseconds / 6000U;
  const auto whole_seconds = (centiseconds / 100U) % 60U;
  const auto fraction = centiseconds % 100U;
  std::ostringstream text;
  text << sign << std::setfill('0') << std::setw(2) << minutes << ':'
       << std::setw(2) << whole_seconds << '.' << std::setw(2) << fraction;
  return text.str();
}

void render_race_gap_panel(SDL_Renderer *renderer, const RaceHudVisual &hud,
                           const mh::content::AiRouteData &route,
                           const std::span<const LiveRaceRosterSlot> roster,
                           const std::span<const std::int64_t> vehicle_progress,
                           const std::span<const float> vehicle_speeds,
                           const int width, const int height) {
  if (roster.size() < 2U || roster.size() > 8U ||
      vehicle_progress.size() != roster.size() ||
      vehicle_speeds.size() != roster.size() || route.samples.empty()) {
    return;
  }
  std::vector<std::size_t> standings(roster.size());
  std::iota(standings.begin(), standings.end(), 0U);
  std::stable_sort(standings.begin(), standings.end(),
                   [&](const std::size_t left, const std::size_t right) {
                     if (vehicle_progress[left] != vehicle_progress[right]) {
                       return vehicle_progress[left] > vehicle_progress[right];
                     }
                     return left < right;
                   });
  double route_length = 0.0;
  for (std::size_t sample = 0U; sample < route.samples.size(); ++sample) {
    const auto next = (sample + 1U) % route.samples.size();
    const auto dx = static_cast<double>(route.samples[next].position[0U] -
                                        route.samples[sample].position[0U]);
    const auto dz = static_cast<double>(route.samples[next].position[1U] -
                                        route.samples[sample].position[1U]);
    route_length += std::hypot(dx, dz);
  }
  const auto metres_per_sample =
      route_length / static_cast<double>(route.samples.size());
  constexpr float origin_x = 8.0F;
  constexpr float origin_y = 348.0F;
  constexpr float row_height = 16.0F;
  constexpr float gap_right = 160.0F;
  for (std::size_t rank = 0U; rank < standings.size(); ++rank) {
    const auto slot = standings[rank];
    auto nickname = roster[slot].driver_nick;
    if (nickname.size() > 10U) {
      nickname.resize(10U);
    }
    const auto sample_delta = vehicle_progress[slot] - vehicle_progress[0U];
    const auto reference_speed =
        std::max(10.0, 0.5 * (static_cast<double>(vehicle_speeds[slot]) +
                              static_cast<double>(vehicle_speeds[0U])));
    const auto gap_seconds = static_cast<double>(std::abs(sample_delta)) *
                             metres_per_sample / reference_speed;
    const auto sign = slot == 0U ? ' ' : sample_delta > 0 ? '-' : '+';
    const auto name_text = std::to_string(rank + 1U) + " " + nickname;
    const auto gap_text = format_hud_gap(gap_seconds, sign);
    const auto gap_x = gap_right - hud_font_text_width(hud, gap_text);
    const auto y = origin_y + static_cast<float>(rank) * row_height;
    draw_hud_font_text(renderer, hud, name_text, origin_x + 1.0F, y + 1.0F,
                       {0U, 0U, 0U}, width, height);
    draw_hud_font_text(renderer, hud, gap_text, gap_x + 1.0F, y + 1.0F,
                       {0U, 0U, 0U}, width, height);
    const auto color = roster[slot].driver_color;
    draw_hud_font_text(renderer, hud, name_text, origin_x, y, color, width,
                       height);
    draw_hud_font_text(renderer, hud, gap_text, gap_x, y, color, width, height);
  }
}

enum class RacePausePage : std::size_t {
  race = 0U,
  graphics = 1U,
  sound = 2U,
  gameplay = 3U,
};

struct RacePauseMenuState {
  RacePausePage page = RacePausePage::race;
  std::array<std::size_t, 4U> selections{};
  mh::ui::GraphicRendererBackend renderer_backend =
      mh::ui::GraphicRendererBackend::d3d11;
  std::size_t car_shading = 2U;
  bool better_perspective = false;
  float view_distance_percent = 80.0F;
  mh::ui::GraphicWindowMode window_mode = mh::ui::GraphicWindowMode::windowed;
  int sound_effects_volume = 253;
  int cd_volume = 253;
  mh::ui::GraphicInfoMode info_detail = mh::ui::GraphicInfoMode::all;
  mh::ui::GraphicInfoMode info_map = mh::ui::GraphicInfoMode::all;
  mh::ui::GraphicInfoMode checkpoint_info = mh::ui::GraphicInfoMode::all;
  std::uint32_t checkpoint_display_time_ms = 4000U;
  bool camera_shake = true;
  int ui_scale_percent = 100;
  std::uint64_t hud_preview_until_ticks = 0U;

  [[nodiscard]] std::size_t page_index() const {
    return static_cast<std::size_t>(page);
  }

  [[nodiscard]] std::size_t selection() const {
    return selections[page_index()];
  }

  [[nodiscard]] std::size_t choice_count() const {
    switch (page) {
    case RacePausePage::race:
      return 6U;
    case RacePausePage::graphics:
      return 6U;
    case RacePausePage::sound:
      return 3U;
    case RacePausePage::gameplay:
      return 7U;
    }
    throw std::runtime_error("invalid race pause-menu page");
  }

  void previous_choice() {
    auto &selected = selections[page_index()];
    selected = selected == 0U ? choice_count() - 1U : selected - 1U;
  }

  void next_choice() {
    auto &selected = selections[page_index()];
    selected = (selected + 1U) % choice_count();
  }

  [[nodiscard]] bool hud_preview_active(const std::uint64_t ticks) const {
    return page == RacePausePage::gameplay && ticks < hud_preview_until_ticks;
  }
};

std::string_view race_pause_info_mode_text(const mh::ui::GraphicInfoMode mode) {
  switch (mode) {
  case mh::ui::GraphicInfoMode::none:
    return "None";
  case mh::ui::GraphicInfoMode::selective:
    return "Selective";
  case mh::ui::GraphicInfoMode::all:
    return "All";
  }
  return "All";
}

struct RacePauseChoiceText {
  std::string description;
  std::string value;

  [[nodiscard]] bool uses_setting_columns() const noexcept {
    return !value.empty();
  }
};

RacePauseChoiceText race_pause_choice_text(const RacePauseMenuState &state,
                                           const std::size_t index) {
  static constexpr std::array<std::string_view, 6U> race_choices{{
      "Continue race",
      "Restart race",
      "GFX Options",
      "Gameplay Options",
      "SFX Options",
      "Quit to menu",
  }};
  static constexpr std::array<std::string_view, 4U> shading_values{{
      "Flat",
      "Gouraud",
      "Reflection",
      "Glenz",
  }};
  switch (state.page) {
  case RacePausePage::race:
    return {std::string(race_choices.at(index)), {}};
  case RacePausePage::graphics:
    switch (index) {
    case 0U:
      return {"Renderer:", std::string(mh::ui::graphic_renderer_backend_label(
                               state.renderer_backend))};
    case 1U:
      return {"Car shading:",
              std::string(shading_values.at(state.car_shading))};
    case 2U:
      return {"Perspective quality:",
              state.better_perspective ? "Better" : "Normal"};
    case 3U:
      return {"View distance:", std::to_string(static_cast<int>(
                                    std::lround(state.view_distance_percent))) +
                                    '%'};
    case 4U:
      return {"Window mode:",
              std::string(
                  mh::ui::graphic_window_mode_config_name(state.window_mode))};
    case 5U:
      return {"Back", {}};
    default:
      break;
    }
    break;
  case RacePausePage::sound:
    switch (index) {
    case 0U:
      return {
          "SFX Volume:",
          std::to_string(static_cast<int>(std::lround(
                             static_cast<double>(state.sound_effects_volume) *
                             20.0 / 254.0)) *
                         5) +
              '%'};
    case 1U:
      return {"CD Volume:",
              std::to_string(
                  static_cast<int>(std::lround(
                      static_cast<double>(state.cd_volume) * 20.0 / 254.0)) *
                  5) +
                  '%'};
    case 2U:
      return {"Back", {}};
    default:
      break;
    }
    break;
  case RacePausePage::gameplay:
    switch (index) {
    case 0U:
      return {"Race information:",
              std::string(race_pause_info_mode_text(state.info_detail))};
    case 1U:
      return {"Minimap:",
              std::string(race_pause_info_mode_text(state.info_map))};
    case 2U:
      return {"Checkpoint display:",
              std::string(race_pause_info_mode_text(state.checkpoint_info))};
    case 3U: {
      std::ostringstream value;
      value << std::fixed
            << std::setprecision(
                   state.checkpoint_display_time_ms % 1000U == 0U ? 0 : 1)
            << static_cast<double>(state.checkpoint_display_time_ms) / 1000.0
            << 's';
      return {"Checkpoint display time:", value.str()};
    }
    case 4U:
      return {"Camera shake:", state.camera_shake ? "On" : "Off"};
    case 5U:
      return {"UI scale:", std::to_string(state.ui_scale_percent) + '%'};
    case 6U:
      return {"Back", {}};
    default:
      break;
    }
    break;
  }
  throw std::runtime_error("invalid race pause-menu choice");
}

void apply_race_window_mode(SDL_Window *window,
                            const mh::ui::GraphicWindowMode mode,
                            const int windowed_width,
                            const int windowed_height) {
  if (window == nullptr) {
    throw std::invalid_argument("race window mode requires a window");
  }
  const auto flags = SDL_GetWindowFlags(window);
  const auto fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0U;
  if (mode == mh::ui::GraphicWindowMode::windowed) {
    if (fullscreen) {
      require(SDL_SetWindowFullscreen(window, false),
              "leave in-race fullscreen mode");
    }
    require(SDL_SetWindowFullscreenMode(window, nullptr),
            "clear in-race exclusive fullscreen mode");
    require(SDL_SetWindowBordered(window, true),
            "restore in-race window border");
    require(SDL_SetWindowResizable(window, true),
            "restore in-race resizable window mode");
    int current_width = 0;
    int current_height = 0;
    require(SDL_GetWindowSize(window, &current_width, &current_height),
            "query in-race window size");
    if (current_width != windowed_width || current_height != windowed_height) {
      require(SDL_SetWindowSize(window, windowed_width, windowed_height),
              "apply in-race window size");
    }
    return;
  }
  if (mode == mh::ui::GraphicWindowMode::borderless) {
    const auto *active_mode = SDL_GetWindowFullscreenMode(window);
    if (!fullscreen || active_mode != nullptr) {
      require(SDL_SetWindowFullscreenMode(window, nullptr),
              "select in-race borderless desktop mode");
      require(SDL_SetWindowFullscreen(window, true),
              "enter in-race borderless desktop mode");
    }
    return;
  }
  auto display = SDL_GetDisplayForWindow(window);
  if (display == 0U) {
    display = SDL_GetPrimaryDisplay();
  }
  require(display != 0U, "find display for in-race fullscreen mode");
  SDL_DisplayMode closest{};
  require(SDL_GetClosestFullscreenDisplayMode(
              display, windowed_width, windowed_height, 0.0F, false, &closest),
          "find in-race fullscreen display mode");
  const auto *active_mode = SDL_GetWindowFullscreenMode(window);
  if (!fullscreen || active_mode == nullptr || active_mode->w != closest.w ||
      active_mode->h != closest.h ||
      active_mode->pixel_density != closest.pixel_density ||
      active_mode->refresh_rate != closest.refresh_rate) {
    require(SDL_SetWindowFullscreenMode(window, &closest),
            "select in-race fullscreen display mode");
    require(SDL_SetWindowFullscreen(window, true),
            "enter in-race fullscreen mode");
  }
}

SDL_Window *create_hosted_race_window(SDL_Window *host,
                                      const std::string &title, const int width,
                                      const int height) {
  const auto properties = SDL_CreateProperties();
  if (properties == 0U) {
    return nullptr;
  }
  SDL_SetStringProperty(properties, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                        title.c_str());
  SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
  SDL_SetNumberProperty(properties, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
                        height);
  SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_HIDDEN_BOOLEAN,
                         true);
  SDL_SetBooleanProperty(properties, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN,
                         true);
  // Do not use SDL_WINDOW_UTILITY here. SDL's Win32 backend gives utility
  // windows a separate hidden proxy owner and ignores the supplied parent.
  // The race overlay must be owned by the persistent front-end HWND so it
  // remains above that window across focus and activation changes.
  SDL_SetPointerProperty(properties, SDL_PROP_WINDOW_CREATE_PARENT_POINTER,
                         host);
  auto *window = SDL_CreateWindowWithProperties(properties);
  SDL_DestroyProperties(properties);
  return window;
}

void align_hosted_race_window(SDL_Window *host, SDL_Window *race,
                              const int fallback_width,
                              const int fallback_height) {
  if (host == nullptr || race == nullptr) {
    throw std::invalid_argument("hosted race alignment requires two windows");
  }
  if (fallback_width <= 0 || fallback_height <= 0) {
    throw std::invalid_argument("hosted race alignment fallback is invalid");
  }
  // Fullscreen transitions are asynchronous on SDL/Win32.  Querying the
  // parent's client rectangle before both windows have committed the change can
  // transiently return a zero-sized rectangle, which SDL_SetWindowSize rejects.
  require(SDL_SyncWindow(host), "synchronize hosted race parent");
  require(SDL_SyncWindow(race), "synchronize hosted race overlay transition");
  auto *host_handle = static_cast<HWND>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(host),
                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (host_handle == nullptr) {
    throw std::runtime_error("query hosted race Win32 parent");
  }
  RECT client{};
  POINT origin{};
  if (GetClientRect(host_handle, &client) == FALSE ||
      ClientToScreen(host_handle, &origin) == FALSE) {
    throw std::runtime_error("query hosted race client bounds");
  }
  const auto client_width = client.right - client.left;
  const auto client_height = client.bottom - client.top;
  const auto width = client_width > 0 ? client_width : fallback_width;
  const auto height = client_height > 0 ? client_height : fallback_height;
  require(SDL_SetWindowBordered(race, false),
          "remove hosted race window border");
  require(SDL_SetWindowResizable(race, false), "lock hosted race overlay size");
  require(SDL_SetWindowPosition(race, origin.x, origin.y),
          "position hosted race overlay");
  require(SDL_SetWindowSize(race, width, height), "size hosted race overlay");
  require(SDL_SyncWindow(race), "synchronize hosted race overlay");
}

void render_race_pause_menu(SDL_Renderer *renderer, const RaceHudVisual &hud,
                            const RacePauseMenuState &state, const int width,
                            const int height) {
  // p3.1 RVA 0x7ac4e renders the active page through the FONT1S owner in
  // centre-alignment mode. Its +0x74 method returns FONT1S's authored
  // eight-pixel line metric. Every row is first drawn black at (+1,+1), then
  // The original +0x88 colour setter receives B/G/R in ECX/EBX/EDX, so the
  // inactive register triplet 0x20/0x68/0xe0 displays as RGB 224/104/32.
  // The selected row is white.
  const auto logical_row_height = hud.font_line_metric;
  constexpr float logical_center_x = 320.0F;
  constexpr float logical_center_y = 240.0F;
  const auto count = state.choice_count();
  const auto start_y =
      logical_center_y - logical_row_height * static_cast<float>(count / 2U);
  for (std::size_t index = 0U; index < count; ++index) {
    const auto text = race_pause_choice_text(state, index);
    const auto y = start_y + logical_row_height * static_cast<float>(index);
    const auto color = index == state.selection()
                           ? std::array<std::uint8_t, 3U>{255U, 255U, 255U}
                           : std::array<std::uint8_t, 3U>{224U, 104U, 32U};
    const auto draw_text = [&](const std::string_view value, const float x) {
      draw_hud_font_text(renderer, hud, value, x + 1.0F, y + 1.0F, {0U, 0U, 0U},
                         width, height);
      draw_hud_font_text(renderer, hud, value, x, y, color, width, height);
    };
    if (text.uses_setting_columns()) {
      constexpr float description_right = logical_center_x - 6.0F;
      constexpr float value_left = logical_center_x + 6.0F;
      draw_text(text.description,
                description_right - hud_font_text_width(hud, text.description));
      draw_text(text.value, value_left);
    } else {
      draw_text(text.description,
                logical_center_x -
                    hud_font_text_width(hud, text.description) * 0.5F);
    }
  }
}

float draw_hud_small_number(
    SDL_Renderer *renderer, const RaceHudVisual &hud, const std::uint32_t value,
    const float x, const float y, const int width, const int height,
    const HudGroupAnchor anchor = HudGroupAnchor::automatic) {
  // The retail HUD lays the small numerals out in fixed-width cells.  In
  // particular snum1 is five pixels narrower than the other source sprites;
  // advancing by its bitmap width makes a running time visibly breathe.
  constexpr float digit_advance = 18.0F;
  const auto text = std::to_string(value);
  auto cursor = x;
  for (const auto character : text) {
    const auto digit = static_cast<std::size_t>(character - '0');
    draw_hud_texture(renderer, hud.small_digits[digit], cursor, y, width,
                     height, anchor);
    cursor += digit_advance;
  }
  return cursor;
}

void render_original_tron_scroll(SDL_Renderer *renderer,
                                 const RaceHudVisual &hud,
                                 const mh::game::RaceProgress &progress,
                                 const int width, const int height) {
  if (progress.phase != mh::game::RacePhase::racing) {
    return;
  }
  constexpr std::array<std::string_view, 8U> lines{
      "Hello there",
      "Here is something for all the oldskool people out there:",
      "A tribute to the classic movie TRON!",
      "Enjoy the look and feel of the very early 80's --",
      "when hidden-lines were 'in'",
      "and never forget your roots!",
      "Regards,",
      "all the former demosceners at Digital Illusions",
  };
  constexpr auto duration = std::chrono::seconds(40);
  const auto elapsed = progress.timing.total;
  if (elapsed < mh::game::SimulationDuration::zero() || elapsed >= duration) {
    return;
  }
  const auto fraction = std::chrono::duration<double>(elapsed).count() /
                        std::chrono::duration<double>(duration).count();
  constexpr float line_spacing = 24.0F;
  const auto scroll_offset = static_cast<float>(fraction) * line_spacing *
                             static_cast<float>(lines.size());
  constexpr float x = 30.0F;
  constexpr float first_y = 438.0F;
  const auto viewport = mh::game::original_hud_viewport(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  const auto scale = static_cast<float>(viewport.scale);
  const SDL_Rect message_clip{
      static_cast<int>(std::lround(viewport.offset_x)),
      static_cast<int>(std::lround(mh::game::original_hud_y(viewport, 428.0F))),
      static_cast<int>(std::lround(640.0F * scale)),
      static_cast<int>(std::lround(52.0F * scale))};
  require(SDL_SetRenderClipRect(renderer, &message_clip),
          "set TRON message clip");
  for (std::size_t row = 0U; row < lines.size(); ++row) {
    const auto y =
        first_y + static_cast<float>(row) * line_spacing - scroll_offset;
    if (y > 480.0F || y + 16.0F < 428.0F) {
      continue;
    }
    draw_hud_font_text(renderer, hud, lines[row], x, y, {255U, 255U, 255U},
                       width, height);
  }
  require(SDL_SetRenderClipRect(renderer, nullptr), "clear TRON message clip");
}

void render_original_tron_message_backdrop(
    SDL_Renderer *renderer, const mh::game::RaceProgress &progress,
    const int width, const int height) {
  constexpr auto duration = std::chrono::seconds(40);
  if (progress.phase != mh::game::RacePhase::racing ||
      progress.timing.total < mh::game::SimulationDuration::zero() ||
      progress.timing.total >= duration) {
    return;
  }
  const auto viewport = mh::game::original_hud_viewport(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  const auto scale = static_cast<float>(viewport.scale);
  const SDL_FRect backdrop{
      static_cast<float>(viewport.offset_x),
      static_cast<float>(mh::game::original_hud_y(viewport, 428.0F)),
      640.0F * scale, 52.0F * scale};
  require(SDL_SetRenderDrawColor(renderer, 0U, 0U, 0U, 255U),
          "set TRON message backdrop color");
  require(SDL_RenderFillRect(renderer, &backdrop),
          "render TRON message backdrop");
}

void render_original_black_lotus_message(SDL_Renderer *renderer,
                                         const RaceHudVisual &hud,
                                         const int width, const int height) {
  constexpr std::string_view first = "WELCOME TO CLUB TBL!";
  constexpr std::string_view second = "Tonights Guest DJ: Randolph Carter!";
  const auto draw_centered = [&](const std::string_view text, const float y) {
    const auto x = (640.0F - hud_font_text_width(hud, text)) * 0.5F;
    draw_hud_font_text(renderer, hud, text, x + 1.0F, y + 1.0F, {0U, 0U, 0U},
                       width, height);
    draw_hud_font_text(renderer, hud, text, x, y, {255U, 255U, 255U}, width,
                       height);
  };
  draw_centered(first, 220.0F);
  draw_centered(second, 238.0F);
}

void render_original_atlantika_scroll(SDL_Renderer *renderer,
                                      const RaceHudVisual &hud,
                                      const std::string_view text,
                                      const std::uint64_t frame,
                                      const int width, const int height) {
  if (text.empty()) {
    return;
  }
  constexpr double phase_scale = 2.0 * std::numbers::pi / 4096.0;
  const auto phase = static_cast<double>((frame * 10U) % 4096U);

  constexpr std::string_view title = "HCR&D";
  constexpr float title_scale = 10.0F;
  constexpr float title_spacing = 80.0F;
  const auto title_animation_phase = static_cast<double>((frame * 7U) % 4096U);
  const auto title_width = title_spacing * static_cast<float>(title.size() - 1U) +
                           hud.font_advances[static_cast<unsigned char>(title.back())] *
                               title_scale;
  const auto title_x = (640.0F - title_width) * 0.5F;
  for (std::size_t index = 0U; index < title.size(); ++index) {
    const auto title_phase =
        std::fmod(title_animation_phase + static_cast<double>(index * 200U), 4096.0);
    const auto x = title_x + static_cast<float>(index) * title_spacing;
    const auto y =
        480.0F - hud.font_line_metric * title_scale - static_cast<float>(
                     std::abs(std::sin(title_phase * phase_scale)) * 160.0);
    draw_hud_font_glyph_scaled(renderer, hud, title[index], x + 6.0F, y + 6.0F,
                               title_scale, {0U, 0U, 0U}, width, height, true);
    draw_hud_font_glyph_scaled(renderer, hud, title[index], x, y, title_scale,
                               {232U, 48U, 24U}, width, height, true);
  }

  constexpr std::uint64_t initial_x = 640U + 50U;
  std::uint64_t first_character = 0U;
  float base_x = static_cast<float>(initial_x);
  if (frame <= initial_x) {
    base_x -= static_cast<float>(frame);
  } else {
    const auto extra = frame - initial_x - 1U;
    // The original counts through both eight and zero before advancing a glyph.
    first_character = 1U + extra / 9U;
    base_x = 8.0F - static_cast<float>(extra % 9U);
  }
  base_x -= 50.0F + hud.font_advances[static_cast<unsigned char>(' ')];
  for (std::size_t visible = 0U; visible < 110U; ++visible) {
    const auto index =
        static_cast<std::size_t>((first_character + visible) % text.size());
    const auto y_phase =
        std::fmod(phase +
                      static_cast<double>(((first_character + visible) % 4096U) * 200U),
                  4096.0);
    const auto pen_x = base_x + static_cast<float>(visible) * 8.0F;
    if (pen_x >=
        690.0F + hud.font_advances[static_cast<unsigned char>(' ')]) {
      break;
    }
    // Overlapping slow waves vary local spacing without reversing letter order.
    const auto drift =
        14.0 * std::sin(static_cast<double>(pen_x) * 0.022 +
                        static_cast<double>(frame) * 0.013) +
        8.0 * std::sin(static_cast<double>(pen_x) * 0.037 -
                       static_cast<double>(frame) * 0.009);
    const auto x = pen_x + static_cast<float>(drift);
    const auto y =
        145.0F + static_cast<float>(std::sin(y_phase * phase_scale) * 40.0);
    const std::array<char, 2U> character{text[index], '\0'};
    draw_hud_font_text(renderer, hud, character.data(), x, y,
                       {255U, 255U, 255U}, width, height, true);
  }
}

float hud_small_number_width(const RaceHudVisual &, const std::uint32_t value) {
  constexpr float digit_advance = 18.0F;
  return static_cast<float>(std::to_string(value).size()) * digit_advance;
}

void draw_hud_time(SDL_Renderer *renderer, const RaceHudVisual &hud,
                   const mh::game::SimulationDuration duration,
                   const float right, const float y, const int width,
                   const int height,
                   const HudGroupAnchor anchor = HudGroupAnchor::automatic) {
  const auto milliseconds = std::max<std::int64_t>(
      0,
      std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
  const auto centiseconds = milliseconds / 10;
  const auto minutes = (centiseconds / 6000) % 100;
  const auto seconds = (centiseconds / 100) % 60;
  const auto hundredths = centiseconds % 100;
  const std::array<std::uint32_t, 6U> digits{
      static_cast<std::uint32_t>(minutes / 10),
      static_cast<std::uint32_t>(minutes % 10),
      static_cast<std::uint32_t>(seconds / 10),
      static_cast<std::uint32_t>(seconds % 10),
      static_cast<std::uint32_t>(hundredths / 10),
      static_cast<std::uint32_t>(hundredths % 10)};
  // p3.1 sub_00022960 walks right-to-left in exact 20-pixel digit cells and
  // ten-pixel colon cells, independently of the source bitmap widths.
  constexpr float digit_advance = 20.0F;
  constexpr float colon_advance = 10.0F;
  const auto total_width =
      static_cast<float>(digits.size()) * digit_advance + 2.0F * colon_advance;
  auto cursor = right - total_width;
  for (std::size_t index = 0U; index < digits.size(); ++index) {
    draw_hud_texture(renderer, hud.small_digits[digits[index]], cursor, y,
                     width, height, anchor);
    cursor += digit_advance;
    if (index == 1U || index == 3U) {
      draw_hud_texture(renderer, hud.colon, cursor, y, width, height, anchor);
      cursor += colon_advance;
    }
  }
}

void render_race_hud(SDL_Renderer *renderer, const RaceHudVisual &hud,
                     const mh::content::AiRouteData &route,
                     const std::span<const LiveRaceRosterSlot> roster,
                     const mh::game::RaceConfig &race_config,
                     const mh::game::RaceProgress &progress,
                     const double speed_kmh, const double engine_scalar,
                     const std::size_t gear_index,
                     const std::uint32_t race_position,
                     const std::span<const std::size_t> vehicle_samples,
                     const std::span<const std::size_t> ghost_samples,
                     const std::span<const std::int64_t> vehicle_progress,
                     const std::span<const float> vehicle_speeds,
                     const bool checkpoint_info_visible,
                     const mh::ui::GraphicInfoMode info_detail_mode,
                     const mh::ui::GraphicInfoMode info_map_mode,
                     const bool metric_units, const bool avenger_map,
                     const int width, const int height) {
  const auto hud_viewport = mh::game::original_hud_viewport(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  if (info_detail_mode != mh::ui::GraphicInfoMode::none) {
    const auto display_speed_kmh =
        metric_units ? speed_kmh : speed_kmh * 0.621371192237334;
    const auto speed = static_cast<std::uint32_t>(
        std::clamp(std::lround(std::fabs(display_speed_kmh)), 0L, 999L));
    // p3.1 sub_00023284 draws the least-significant speed digit first. Its
    // right-aligned field starts at X=95, retreats in exact 24-pixel cells, and
    // aligns each BNUM image using 30-(stored width-1). The single digit zero
    // is therefore at X=73 rather than at the left edge of the screen.
    auto remaining_speed = speed;
    auto speed_cursor = 95.0F;
    do {
      const auto digit = static_cast<std::size_t>(remaining_speed % 10U);
      remaining_speed /= 10U;
      speed_cursor -= 24.0F;
      const auto digit_x =
          speed_cursor + 30.0F -
          static_cast<float>(hud.large_digits[digit].width - 1U);
      draw_hud_texture(renderer, hud.large_digits[digit], digit_x, 13.0F, width,
                       height);
    } while (remaining_speed != 0U);
    const auto &speed_unit =
        metric_units ? hud.speed_unit_metric : hud.speed_unit_imperial;
    draw_hud_texture(renderer, speed_unit, 100.0F, 34.0F, width, height);
    draw_hud_texture(renderer, hud.meter_inactive, 20.0F, 55.0F, width, height);
    // p3.1 RVA 0x7954f multiplies the active body's engine field by the exact
    // f32 constant at VA 0x0050ce14 (0.00008). sub_000226b8 then clamps the
    // fraction to 0..1 before cropping meter00. This remains live during the
    // locked countdown because the player drivetrain may free-rev there.
    const auto meter_fraction =
        std::clamp(static_cast<float>(engine_scalar) * 0.00008F, 0.0F, 1.0F);
    if (meter_fraction > 0.0F) {
      const SDL_FRect source{0.0F, 0.0F,
                             static_cast<float>(hud.meter_active.width) *
                                 meter_fraction,
                             static_cast<float>(hud.meter_active.height)};
      auto destination =
          hud_rect(hud.meter_active, 20.0F, 55.0F, width, height);
      destination.w *= meter_fraction;
      require(SDL_RenderTexture(renderer, hud.meter_active.texture.get(),
                                &source, &destination),
              "render original race HUD rev meter");
    }
    // The same original owner places TGEAR at (10,55), with the value seven
    // pixels after its stored width and one pixel lower.
    draw_hud_texture(renderer, hud.gear_label, 10.0F, 55.0F, width, height);
    const auto gear_x =
        10.0F + static_cast<float>(hud.gear_label.width - 1U) + 7.0F;
    if (gear_index == 0U) {
      draw_hud_texture(renderer, hud.reverse, gear_x, 56.0F, width, height);
    } else {
      const auto digit = std::min<std::size_t>(gear_index, 9U);
      draw_hud_texture(renderer, hud.small_digits[digit], gear_x, 56.0F, width,
                       height);
    }

    // p3.1 sub_00023284 owns a 16-pixel right margin, forty-pixel groups, and
    // separate label/value baselines. Loaded glyph widths are stored as
    // width-1.
    constexpr float right = 624.0F;
    constexpr auto timing_anchor = HudGroupAnchor::top_right;
    const auto stored_width = [](const HudTexture &texture) {
      return static_cast<float>(texture.width - 1U);
    };
    const auto time_x = right - stored_width(hud.time_label);
    const auto lap_prefix_x = time_x - (stored_width(hud.lap_label) + 3.0F);
    draw_hud_texture(renderer, hud.total_label,
                     time_x - (stored_width(hud.total_label) + 3.0F), 10.0F,
                     width, height, timing_anchor);
    draw_hud_texture(renderer, hud.time_label, time_x, 10.0F, width, height,
                     timing_anchor);
    draw_hud_time(renderer, hud, progress.timing.total, right, 30.0F, width,
                  height, timing_anchor);
    draw_hud_texture(renderer, hud.total_label,
                     lap_prefix_x - (stored_width(hud.total_label) + 3.0F),
                     50.0F, width, height, timing_anchor);
    draw_hud_texture(renderer, hud.lap_label, lap_prefix_x, 50.0F, width,
                     height, timing_anchor);
    draw_hud_texture(renderer, hud.time_label, time_x, 50.0F, width, height,
                     timing_anchor);
    draw_hud_time(renderer, hud, progress.timing.current_lap, right, 70.0F,
                  width, height, timing_anchor);
    draw_hud_texture(renderer, hud.best_label,
                     lap_prefix_x - (stored_width(hud.best_label) + 3.0F),
                     90.0F, width, height, timing_anchor);
    draw_hud_texture(renderer, hud.lap_label, lap_prefix_x, 90.0F, width,
                     height, timing_anchor);
    draw_hud_texture(renderer, hud.time_label, time_x, 90.0F, width, height,
                     timing_anchor);
    auto best_lap = mh::game::SimulationDuration::zero();
    if (!progress.timing.completed_laps.empty()) {
      best_lap = *std::min_element(progress.timing.completed_laps.begin(),
                                   progress.timing.completed_laps.end());
    }
    draw_hud_time(renderer, hud, best_lap, right, 110.0F, width, height,
                  timing_anchor);
    draw_hud_texture(renderer, hud.lap_label,
                     right - stored_width(hud.lap_label), 130.0F, width, height,
                     timing_anchor);
    const auto current_lap = std::max(1U, progress.current_lap);
    const auto lap_number_x =
        right - hud_small_number_width(hud, current_lap) - 7.0F -
        hud_small_number_width(hud, race_config.lap_count);
    auto cursor =
        draw_hud_small_number(renderer, hud, current_lap, lap_number_x, 150.0F,
                              width, height, timing_anchor);
    require(SDL_SetRenderDrawColor(renderer, 235U, 235U, 255U, 255U),
            "set original race HUD separator color");
    const auto draw_fraction_separator =
        [&](const float logical_x, const float logical_y, const char *context) {
          // Transform the fraction mark as one HUD glyph. Transforming both
          // endpoints independently lets a slash which crosses an anchoring
          // boundary acquire two different offsets at non-default UI scales.
          const auto scale = static_cast<float>(hud_viewport.scale);
          const auto origin_x = logical_x * scale +
                                static_cast<float>(hud_viewport.offset_x * 2.0);
          const auto origin_y = logical_y * scale;
          require(SDL_RenderLine(renderer, origin_x + scale,
                                 origin_y + 13.0F * scale,
                                 origin_x + 6.0F * scale, origin_y + scale),
                  context);
        };
    draw_fraction_separator(cursor, 150.0F,
                            "render original race HUD lap separator");
    static_cast<void>(draw_hud_small_number(
        renderer, hud, race_config.lap_count, cursor + 7.0F, 150.0F, width,
        height, timing_anchor));
    constexpr float position_label_y = 170.0F;
    constexpr float position_value_y = 190.0F;
    draw_hud_texture(renderer, hud.position_label,
                     right - stored_width(hud.position_label), position_label_y,
                     width, height, timing_anchor);
    const auto vehicle_count =
        static_cast<std::uint32_t>(vehicle_samples.size());
    const auto position_number_x =
        right - hud_small_number_width(hud, race_position) - 7.0F -
        hud_small_number_width(hud, vehicle_count);
    cursor =
        draw_hud_small_number(renderer, hud, race_position, position_number_x,
                              position_value_y, width, height, timing_anchor);
    draw_fraction_separator(cursor, position_value_y,
                            "render original race HUD position separator");
    static_cast<void>(draw_hud_small_number(renderer, hud, vehicle_count,
                                            cursor + 7.0F, position_value_y,
                                            width, height, timing_anchor));
  }

  if (info_map_mode != mh::ui::GraphicInfoMode::none) {
    constexpr float map_x = 552.0F;
    constexpr float map_y = 399.0F;
    constexpr float map_width = 78.0F;
    constexpr float map_height = 74.0F;
    const auto extent_x = std::max(0.001F, hud.map_max[0U] - hud.map_min[0U]);
    const auto extent_z = std::max(0.001F, hud.map_max[1U] - hud.map_min[1U]);
    const auto map_scale =
        std::min(map_width / extent_x, map_height / extent_z);
    const auto used_width = extent_x * map_scale;
    const auto used_height = extent_z * map_scale;
    const auto map_logical_point =
        [&](const mh::content::AiRouteSample &sample) {
          return SDL_FPoint{
              map_x + (map_width - used_width) * 0.5F +
                  (sample.position[0U] - hud.map_min[0U]) * map_scale,
              map_y + (map_height - used_height) * 0.5F +
                  (hud.map_max[1U] - sample.position[1U]) * map_scale};
        };
    const auto map_point = [&](const mh::content::AiRouteSample &sample) {
      const auto logical = map_logical_point(sample);
      return SDL_FPoint{
          static_cast<float>(mh::game::original_hud_x(hud_viewport, logical.x)),
          static_cast<float>(
              mh::game::original_hud_y(hud_viewport, logical.y))};
    };
    require(SDL_SetRenderDrawColor(renderer, 190U, 195U, 205U, 255U),
            "set original race HUD minimap color");
    for (std::size_t sample = 0U; sample < route.samples.size(); ++sample) {
      const auto next = (sample + 1U) % route.samples.size();
      const auto first = map_point(route.samples[sample]);
      const auto second = map_point(route.samples[next]);
      require(SDL_RenderLine(renderer, first.x, first.y, second.x, second.y),
              "render original race HUD minimap");
    }
    const auto visible_vehicle_count =
        info_map_mode == mh::ui::GraphicInfoMode::all
            ? vehicle_samples.size()
            : std::min<std::size_t>(1U, vehicle_samples.size());
    for (std::size_t slot = 0U; slot < visible_vehicle_count; ++slot) {
      const auto sample = vehicle_samples[slot] % route.samples.size();
      const auto point = map_logical_point(route.samples[sample]);
      const auto &marker =
          slot == 0U ? (avenger_map ? hud.avenger_marker : hud.player_marker)
                     : hud.opponent_marker;
      const auto marker_x = point.x - static_cast<float>(marker.width) * 0.5F;
      const auto marker_y = point.y - static_cast<float>(marker.height) * 0.5F;
      draw_hud_texture(renderer, marker, marker_x, marker_y, width, height);
    }
    for (const auto route_sample : ghost_samples) {
      if (info_map_mode != mh::ui::GraphicInfoMode::all) {
        break;
      }
      const auto sample = route_sample % route.samples.size();
      const auto point = map_logical_point(route.samples[sample]);
      const auto marker_x =
          point.x - static_cast<float>(hud.ghost_marker.width) * 0.5F;
      const auto marker_y =
          point.y - static_cast<float>(hud.ghost_marker.height) * 0.5F;
      draw_hud_texture(renderer, hud.ghost_marker, marker_x, marker_y, width,
                       height);
    }
  }
  if (checkpoint_info_visible &&
      info_detail_mode != mh::ui::GraphicInfoMode::none &&
      progress.phase == mh::game::RacePhase::racing &&
      vehicle_samples.size() == roster.size()) {
    render_race_gap_panel(renderer, hud, route, roster, vehicle_progress,
                          vehicle_speeds, width, height);
  }
}

void quantize_rgb565(mh::content::PamRgbaImage &image) {
  for (std::size_t pixel = 0U; pixel + 3U < image.rgba.size(); pixel += 4U) {
    const auto red = static_cast<std::uint8_t>(image.rgba[pixel] >> 3U);
    const auto green = static_cast<std::uint8_t>(image.rgba[pixel + 1U] >> 2U);
    const auto blue = static_cast<std::uint8_t>(image.rgba[pixel + 2U] >> 3U);
    image.rgba[pixel] = static_cast<std::uint8_t>((red << 3U) | (red >> 2U));
    image.rgba[pixel + 1U] =
        static_cast<std::uint8_t>((green << 2U) | (green >> 4U));
    image.rgba[pixel + 2U] =
        static_cast<std::uint8_t>((blue << 3U) | (blue >> 2U));
  }
}

using AuthoredPalette =
    std::array<std::optional<std::array<std::uint8_t, 4U>>, 256U>;

AuthoredPalette authored_palette(const mh::content::PamRgbaImage &image) {
  AuthoredPalette result{};
  const auto pixels = static_cast<std::size_t>(image.width) * image.height;
  if (image.palette_indices.size() != pixels ||
      image.rgba.size() != pixels * 4U) {
    return result;
  }
  for (std::size_t pixel = 0U; pixel < pixels; ++pixel) {
    const auto index = image.palette_indices[pixel];
    const auto offset = pixel * 4U;
    const std::array<std::uint8_t, 4U> color{
        image.rgba[offset], image.rgba[offset + 1U], image.rgba[offset + 2U],
        image.rgba[offset + 3U]};
    if (!result[index].has_value()) {
      result[index] = color;
    }
  }
  return result;
}

std::uint8_t
closest_authored_palette_index(const AuthoredPalette &palette,
                               const std::array<std::uint8_t, 4U> &color) {
  auto best_index = std::uint8_t{0U};
  auto best_distance = std::numeric_limits<std::uint64_t>::max();
  for (std::size_t index = 0U; index < palette.size(); ++index) {
    if (!palette[index].has_value()) {
      continue;
    }
    std::uint64_t distance = 0U;
    for (std::size_t channel = 0U; channel < color.size(); ++channel) {
      const auto delta = static_cast<std::int32_t>(color[channel]) -
                         static_cast<std::int32_t>((*palette[index])[channel]);
      distance += static_cast<std::uint64_t>(delta * delta);
    }
    if (distance < best_distance) {
      best_distance = distance;
      best_index = static_cast<std::uint8_t>(index);
    }
  }
  return best_index;
}

std::vector<mh::content::PamRgbaImage>
make_original_texture_levels(const mh::content::PamRgbaImage &source,
                             const std::uint32_t format_bits,
                             const std::size_t level_count) {
  const auto pixels = static_cast<std::size_t>(source.width) * source.height;
  if (source.width == 0U || source.height == 0U ||
      source.rgba.size() != pixels * 4U) {
    return {};
  }
  if (format_bits != 8U && format_bits != 16U && format_bits != 32U) {
    throw std::runtime_error("unsupported renderer texture format");
  }
  auto maximum_levels = std::size_t{1U};
  auto level_width = source.width;
  auto level_height = source.height;
  while (level_width > 1U || level_height > 1U) {
    level_width = std::max(1U, level_width / 2U);
    level_height = std::max(1U, level_height / 2U);
    ++maximum_levels;
  }
  const auto effective_level_count =
      std::min(std::max(std::size_t{1U}, level_count), maximum_levels);
  std::vector<mh::content::PamRgbaImage> levels;
  levels.reserve(effective_level_count);
  levels.push_back(source);
  const auto palette = authored_palette(source);
  if (format_bits == 16U) {
    quantize_rgb565(levels.front());
  }
  while (levels.size() < effective_level_count &&
         (levels.back().width > 1U || levels.back().height > 1U)) {
    const auto &previous = levels.back();
    mh::content::PamRgbaImage next;
    next.width = std::max(1U, previous.width / 2U);
    next.height = std::max(1U, previous.height / 2U);
    next.rgba.resize(static_cast<std::size_t>(next.width) * next.height * 4U);
    if (format_bits == 8U &&
        std::any_of(palette.begin(), palette.end(),
                    [](const auto &entry) { return entry.has_value(); })) {
      next.palette_indices.resize(static_cast<std::size_t>(next.width) *
                                  next.height);
    }
    for (std::uint32_t y = 0U; y < next.height; ++y) {
      for (std::uint32_t x = 0U; x < next.width; ++x) {
        std::array<std::uint32_t, 4U> sum{};
        for (std::uint32_t offset_y = 0U; offset_y < 2U; ++offset_y) {
          for (std::uint32_t offset_x = 0U; offset_x < 2U; ++offset_x) {
            const auto source_x =
                std::min(previous.width - 1U, x * 2U + offset_x);
            const auto source_y =
                std::min(previous.height - 1U, y * 2U + offset_y);
            const auto source_offset =
                (static_cast<std::size_t>(source_y) * previous.width +
                 source_x) *
                4U;
            for (std::size_t channel = 0U; channel < sum.size(); ++channel) {
              sum[channel] += previous.rgba[source_offset + channel];
            }
          }
        }
        std::array<std::uint8_t, 4U> color{};
        for (std::size_t channel = 0U; channel < color.size(); ++channel) {
          color[channel] = static_cast<std::uint8_t>((sum[channel] + 2U) / 4U);
        }
        const auto destination_pixel =
            static_cast<std::size_t>(y) * next.width + x;
        if (!next.palette_indices.empty()) {
          const auto palette_index =
              closest_authored_palette_index(palette, color);
          next.palette_indices[destination_pixel] = palette_index;
          color = *palette[palette_index];
        }
        const auto destination_offset = destination_pixel * 4U;
        std::copy(color.begin(), color.end(),
                  next.rgba.begin() +
                      static_cast<std::ptrdiff_t>(destination_offset));
      }
    }
    if (format_bits == 16U) {
      quantize_rgb565(next);
    }
    levels.push_back(std::move(next));
  }
  return levels;
}

const std::vector<mh::content::PamRgbaImage> *
active_texture_images(const MaterialTexture &material, const bool enhanced) {
  const auto &levels = enhanced && material.override_image.has_value()
                           ? material.override_raster_levels
                           : material.raster_levels;
  return levels.empty() ? nullptr : &levels;
}

void prepare_renderer_texture_levels(WorldVisual &visual,
                                     const std::uint32_t format_bits,
                                     const bool trilinear_filtering) {
  const auto level_count = trilinear_filtering
                               ? std::numeric_limits<std::size_t>::max()
                               : std::size_t{1U};
  for (auto &material : visual.materials) {
    material.raster_levels =
        make_original_texture_levels(material.image, format_bits, level_count);
    if (material.override_image.has_value()) {
      material.override_raster_levels = make_original_texture_levels(
          *material.override_image, format_bits, level_count);
    }
  }
  auto &environment = visual.vehicle_environment;
  environment.environment_raster_levels = make_original_texture_levels(
      environment.environment_image, format_bits, level_count);
  environment.reflection_raster_levels = make_original_texture_levels(
      environment.reflection_image, format_bits, level_count);
  environment.phong_raster_levels = make_original_texture_levels(
      environment.phong_image, format_bits, level_count);
}

std::optional<std::filesystem::path> find_runtime_texture_override(
    const std::map<std::string, std::filesystem::path> &overrides,
    const std::string &color_logical_id) {
  auto found = overrides.find(color_logical_id);
  if (found != overrides.end()) {
    return found->second;
  }
  auto paired_mask_id = color_logical_id;
  const auto marker = paired_mask_id.find("/tex2.pdi/");
  if (marker == std::string::npos) {
    return std::nullopt;
  }
  paired_mask_id.replace(marker, std::string_view("/tex2.pdi/").size(),
                         "/tex1.pdi/");
  found = overrides.find(paired_mask_id);
  return found == overrides.end()
             ? std::nullopt
             : std::optional<std::filesystem::path>(found->second);
}

void set_visual_profile(const WorldVisual &visual,
                        const BackgroundVisual &background,
                        const bool enhanced) {
  require(SDL_SetTextureScaleMode(background.texture.get(),
                                  enhanced ? SDL_SCALEMODE_LINEAR
                                           : SDL_SCALEMODE_NEAREST),
          "switch Goldbridge background profile");
  for (const auto &material : visual.materials) {
    require(SDL_SetTextureScaleMode(material.texture.get(),
                                    enhanced ? SDL_SCALEMODE_LINEAR
                                             : SDL_SCALEMODE_NEAREST),
            "switch Goldbridge texture profile");
    if (material.override_texture != nullptr) {
      require(SDL_SetTextureScaleMode(material.override_texture.get(),
                                      SDL_SCALEMODE_LINEAR),
              "switch Goldbridge HD texture profile");
    }
    if (material.fog_texture != nullptr) {
      require(SDL_SetTextureScaleMode(material.fog_texture.get(),
                                      enhanced ? SDL_SCALEMODE_LINEAR
                                               : SDL_SCALEMODE_NEAREST),
              "switch Goldbridge fog-mask profile");
    }
    if (material.override_fog_texture != nullptr) {
      require(SDL_SetTextureScaleMode(material.override_fog_texture.get(),
                                      SDL_SCALEMODE_LINEAR),
              "switch Goldbridge HD fog-mask profile");
    }
  }
}

std::uint32_t
detected_surface_texture_left_crop(const mh::content::MywData &world,
                                   const std::size_t material_name_index,
                                   const mh::content::PamRgbaImage &image) {
  if (image.width < 64U || image.height < 64U || image.width != image.height ||
      image.rgba.size() !=
          static_cast<std::size_t>(image.width) * image.height * 4U) {
    return 0U;
  }

  std::size_t used_primitives = 0U;
  std::size_t upward_primitives = 0U;
  for (const auto &primitive : world.primitives) {
    if (!primitive.has_texture_coordinates ||
        primitive.material_name_index != material_name_index ||
        primitive.normal_index >= world.normals.size()) {
      continue;
    }
    ++used_primitives;
    if (world.normals[primitive.normal_index][1U] > 0.85F) {
      ++upward_primitives;
    }
  }
  // This is a surface-layout correction, not a general texture filter.
  // Restrict it to frequently repeated, overwhelmingly upward-facing assets
  // so signs, lamps, walls, and vehicle textures retain their authored UVs.
  if (used_primitives < 16U || upward_primitives * 5U < used_primitives * 4U) {
    return 0U;
  }

  std::vector<double> column_luminance(image.width, 0.0);
  for (std::uint32_t x = 0U; x < image.width; ++x) {
    double sum = 0.0;
    for (std::uint32_t y = 0U; y < image.height; ++y) {
      const auto pixel = (static_cast<std::size_t>(y) * image.width + x) * 4U;
      sum += image.rgba[pixel] * 0.2126 + image.rgba[pixel + 1U] * 0.7152 +
             image.rgba[pixel + 2U] * 0.0722;
    }
    column_luminance[x] = sum / image.height;
  }

  auto sorted_luminance = column_luminance;
  std::sort(sorted_luminance.begin(), sorted_luminance.end());
  const auto baseline = sorted_luminance[sorted_luminance.size() / 2U];
  const auto edge_width = std::max(8U, image.width / 8U);
  const auto left_peak_iterator =
      std::max_element(column_luminance.begin(),
                       std::next(column_luminance.begin(),
                                 static_cast<std::ptrdiff_t>(edge_width)));
  const auto left_peak = *left_peak_iterator;
  const auto right_peak =
      *std::max_element(std::prev(column_luminance.end(),
                                  static_cast<std::ptrdiff_t>(edge_width)),
                        column_luminance.end());
  const auto required_peak = baseline + std::max(18.0, baseline * 0.20);
  if (left_peak < required_peak || right_peak >= required_peak) {
    return 0U;
  }

  const auto peak = static_cast<std::uint32_t>(
      std::distance(column_luminance.begin(), left_peak_iterator));
  if (peak > image.width / 16U) {
    return 0U;
  }
  const auto return_threshold = baseline + (left_peak - baseline) * 0.18;
  for (auto x = peak + 1U; x + 1U < edge_width; ++x) {
    if (column_luminance[x] <= return_threshold &&
        column_luminance[x + 1U] <= return_threshold) {
      return x;
    }
  }
  return 0U;
}

mh::content::PamRgbaImage
crop_runtime_texture_left(const mh::content::PamRgbaImage &source,
                          const std::uint32_t left_crop) {
  if (left_crop == 0U || left_crop >= source.width || source.height == 0U) {
    return source;
  }
  if (source.rgba.size() !=
      static_cast<std::size_t>(source.width) * source.height * 4U) {
    throw std::runtime_error("runtime texture crop has invalid RGBA data");
  }

  mh::content::PamRgbaImage result;
  result.width = source.width - left_crop;
  result.height = source.height;
  result.rgba.resize(static_cast<std::size_t>(result.width) * result.height *
                     4U);
  const auto source_rgba_stride = static_cast<std::size_t>(source.width) * 4U;
  const auto result_rgba_stride = static_cast<std::size_t>(result.width) * 4U;
  for (std::uint32_t y = 0U; y < result.height; ++y) {
    const auto *source_row = source.rgba.data() +
                             static_cast<std::size_t>(y) * source_rgba_stride +
                             static_cast<std::size_t>(left_crop) * 4U;
    auto *result_row =
        result.rgba.data() + static_cast<std::size_t>(y) * result_rgba_stride;
    std::copy_n(source_row, result_rgba_stride, result_row);
  }

  if (!source.palette_indices.empty()) {
    if (source.palette_indices.size() !=
        static_cast<std::size_t>(source.width) * source.height) {
      throw std::runtime_error(
          "runtime texture crop has invalid palette-index data");
    }
    result.palette_indices.resize(static_cast<std::size_t>(result.width) *
                                  result.height);
    for (std::uint32_t y = 0U; y < result.height; ++y) {
      const auto *source_row = source.palette_indices.data() +
                               static_cast<std::size_t>(y) * source.width +
                               left_crop;
      auto *result_row = result.palette_indices.data() +
                         static_cast<std::size_t>(y) * result.width;
      std::copy_n(source_row, result.width, result_row);
    }
  }
  return result;
}

WorldVisual
load_world_visual(SDL_Renderer *renderer,
                  const std::filesystem::path &world_path,
                  const std::filesystem::path &texture_root,
                  const std::set<std::string> &zero_transparent_materials,
                  const std::optional<std::filesystem::path> &override_root,
                  const std::vector<std::filesystem::path> &halo_paths) {
  WorldVisual result;
  result.world = mh::content::read_myw(world_path);
  result.catalog = mh::content::build_texture_catalog(texture_root);
  result.halo_images.reserve(halo_paths.size());
  for (const auto &path : halo_paths) {
    const auto source = mh::content::read_iff_ilbm(path);
    mh::content::PamRgbaImage image;
    image.width = source.width;
    image.height = source.height;
    image.rgba = source.rgba;
    result.halo_images.push_back({std::move(image)});
  }
  result.halo_attachments.reserve(result.world.lens_flares.size());
  for (const auto &flare : result.world.lens_flares) {
    if (flare.image_index >= result.halo_images.size()) {
      throw std::runtime_error(
          "MYW lens flare references an unavailable track Halo slot");
    }
    WorldHaloAttachment attachment;
    const mh::game::CollisionVector3 primary{
        flare.position[0U], flare.position[1U], flare.position[2U]};
    const mh::game::CollisionVector3 secondary{flare.secondary_position[0U],
                                               flare.secondary_position[1U],
                                               flare.secondary_position[2U]};
    // Fixed MYW lamps use the same two-anchor convention as the recovered
    // MYO vehicle/prop lights: the emitting face lies between the anchors.
    // Center the optical source there instead of pinning it to one edge of
    // the lamp mesh. Preserve the recovered world-space halo extent so this
    // positional correction does not make authored road lights disappear.
    for (std::size_t axis = 0U; axis < attachment.center.size(); ++axis) {
      attachment.center[axis] = (primary[axis] + secondary[axis]) * 0.5;
    }
    attachment.image_index = flare.image_index;
    const auto authored_radius = std::array<double, 3U>{
        secondary[0U] - primary[0U], secondary[1U] - primary[1U],
        secondary[2U] - primary[2U]};
    attachment.half_size = std::sqrt(authored_radius[0U] * authored_radius[0U] +
                                     authored_radius[1U] * authored_radius[1U] +
                                     authored_radius[2U] * authored_radius[2U]);
    if (!std::isfinite(attachment.half_size) ||
        attachment.half_size <= 1.0e-8) {
      continue;
    }
    result.halo_attachments.push_back(attachment);
  }
  if (override_root.has_value()) {
    const auto resolved =
        mh::content::resolve_texture_overrides(result.catalog, *override_root);
    for (const auto &item : resolved.overrides) {
      result.override_paths.emplace(item.logical_id,
                                    resolved.root / item.relative_path);
    }
  }
  result.material_indices.resize(result.world.material_names.size());
  result.materials.reserve(result.world.material_names.size());
  for (std::size_t index = 0U; index < result.world.material_names.size();
       ++index) {
    const auto used = std::any_of(
        result.world.primitives.begin(), result.world.primitives.end(),
        [index](const auto &primitive) {
          return primitive.has_texture_coordinates &&
                 primitive.material_name_index == index;
        });
    if (!used) {
      continue;
    }
    const auto &asset = select_track_texture(
        result.catalog, result.world.material_names[index], "tex2.pdi");
    auto image = mh::content::load_texture_asset_rgba(result.catalog, asset);
    static_cast<void>(mh::content::make_rgba_opaque(image));
    std::uint64_t transparent_pixels = 0U;
    if (zero_transparent_materials.contains(
            ascii_lower(result.world.material_names[index]))) {
      const auto &mask_asset = select_track_texture(
          result.catalog, result.world.material_names[index], "tex1.pdi");
      const auto mask =
          mh::content::load_texture_asset_rgba(result.catalog, mask_asset);
      if (mask.width != image.width || mask.height != image.height ||
          mask.palette_indices.size() !=
              static_cast<std::size_t>(image.width) * image.height) {
        throw std::runtime_error(
            "authored zero-transparent material pair is inconsistent");
      }
      image.palette_indices = mask.palette_indices;
      transparent_pixels = mh::content::apply_palette_zero_transparency(image);
    }
    const auto material_index = result.materials.size();
    MaterialTexture material;
    material.logical_id = asset.logical_id;
    material.width = image.width;
    material.height = image.height;
    material.sampled_left_crop =
        detected_surface_texture_left_crop(result.world, index, image);
    material.sampled_u_scale = 1.0F / static_cast<float>(material.width);
    if (material.sampled_left_crop != 0U) {
      image = crop_runtime_texture_left(image, material.sampled_left_crop);
    }
    material.transparent_pixels = transparent_pixels;
    material.texture = upload_texture(renderer, image);
    material.image = image;
    if (transparent_pixels != 0U) {
      material.fog_texture = upload_fog_mask(renderer, image);
    }
    const auto override =
        find_runtime_texture_override(result.override_paths, asset.logical_id);
    if (override.has_value()) {
      auto override_image = mh::content::read_pam_rgba(*override);
      if (material.sampled_left_crop != 0U && material.width != 0U) {
        const auto scaled_crop = static_cast<std::uint32_t>(std::clamp(
            std::llround(static_cast<double>(material.sampled_left_crop) *
                         override_image.width / material.width),
            0LL, static_cast<long long>(override_image.width - 1U)));
        override_image = crop_runtime_texture_left(override_image, scaled_crop);
      }
      material.override_texture = upload_texture(renderer, override_image);
      material.override_image = override_image;
      if (transparent_pixels != 0U) {
        material.override_fog_texture =
            upload_fog_mask(renderer, override_image);
      }
      ++result.override_materials;
    }
    result.materials.push_back(std::move(material));
    result.material_indices[index] = material_index;
  }
  return result;
}


std::filesystem::path content_relative_path(std::string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  auto path = std::filesystem::path(value).lexically_normal();
  if (path.empty() || path.is_absolute() ||
      (!path.empty() && *path.begin() == "..")) {
    throw std::runtime_error("CAR visual path escapes the content root");
  }
  return path;
}

bool is_early_s40_car(const mh::content::CarDefinition &car) {
  return mh::content::is_s40_car(car);
}

std::array<std::array<mh::content::CarVector, 4U>, 4U>
shadow_points_from_body_bounds(const mh::game::CollisionVector3 &minimum,
                               const mh::game::CollisionVector3 &maximum) {
  const auto center_x = (minimum[0U] + maximum[0U]) * 0.5;
  const auto half_width = (maximum[0U] - minimum[0U]) * 0.5;
  const auto span_z = minimum[2U] - maximum[2U];
  const auto point = [center_x, half_width](const double lateral,
                                            const double z) {
    return mh::content::CarVector{
        static_cast<float>(center_x + half_width * lateral), 0.0F,
        static_cast<float>(z)};
  };
  const auto row_z = [front = maximum[2U], span_z](const double fraction) {
    return front + span_z * fraction;
  };

  std::array<std::array<mh::content::CarVector, 4U>, 4U> result{};
  result[0U] = {point(-0.82, row_z(0.05)), point(-0.5, maximum[2U]),
                point(0.5, maximum[2U]), point(0.82, row_z(0.05))};
  result[1U] = {point(-1.0, row_z(0.18)), point(-0.5, row_z(0.18)),
                point(0.5, row_z(0.18)), point(1.0, row_z(0.18))};
  result[2U] = {point(-1.0, row_z(0.82)), point(-0.5, row_z(0.82)),
                point(0.5, row_z(0.82)), point(1.0, row_z(0.82))};
  result[3U] = {point(-0.82, row_z(0.95)), point(-0.5, minimum[2U]),
                point(0.5, minimum[2U]), point(0.82, row_z(0.95))};
  return result;
}

std::filesystem::path model_lod_path(const std::string &base, const char lod) {
  return content_relative_path(base + lod + ".MYO");
}

void apply_playable_car_tuning(mh::content::CarDefinition &car) {
  if (is_early_s40_car(car)) {
    for (auto &wheel : car.wheels) {
      wheel.center.y = 0.55F;
    }
  }
  if (ascii_lower(car.name) == "superbee" && car.physics.has_value()) {
    car.physics->spring_length = 0.30;
  }
}

CarVisual load_car_visual(
    const mh::content::CarDefinition &car,
    const std::filesystem::path &content_root,
    const std::optional<std::array<mh::content::CarColor, 3U>> active_colors =
        std::nullopt,
    const mh::game::OriginalWheelResponseProfile suspension_profile =
        mh::game::OriginalWheelResponseProfile::standard,
    const mh::ui::GraphicCarDetail detail = mh::ui::GraphicCarDetail::high) {
  CarVisual result;
  const auto superbee_presentation = ascii_lower(car.name) == "superbee";
  const auto early_s40_presentation = is_early_s40_car(car);
  // Keep SuperBee's requested 0.20 presentation separate from its 0.30
  // suspension length so its body shell does not sit at the spring endpoint.
  const auto body_presentation_y = superbee_presentation ? -0.10 : 0.0;
  result.default_colors = car.colors;
  result.active_colors = active_colors.value_or(car.colors);
  result.texture_root =
      resolve_relative_case_insensitive(content_root, car.texture_path);
  result.shadow_points = car.shadow_points;
  const auto read_model = [&content_root](const std::filesystem::path &path) {
    return mh::content::read_myo(content_root / path);
  };
  const auto lod = detail == mh::ui::GraphicCarDetail::low      ? '2'
                   : detail == mh::ui::GraphicCarDetail::medium ? '1'
                                                                : '0';
  auto body = read_model(model_lod_path(car.model_base, lod));
  result.body_minimum = {static_cast<double>(body.minimum[0U]),
                         static_cast<double>(body.minimum[1U]) +
                             body_presentation_y,
                         static_cast<double>(body.minimum[2U])};
  result.body_maximum = {static_cast<double>(body.maximum[0U]),
                         static_cast<double>(body.maximum[1U]) +
                             body_presentation_y,
                         static_cast<double>(body.maximum[2U])};
  if (early_s40_presentation && !result.shadow_points.has_value()) {
    // Early S40 data has no CAR shadow grid; fit one to its body footprint.
    result.shadow_points = shadow_points_from_body_bounds(result.body_minimum,
                                                          result.body_maximum);
  }
  struct HaloFaceCandidate {
    std::size_t material_index = 0U;
    mh::game::CollisionVector3 center{};
    mh::game::CollisionVector3 normal{};
  };
  std::array<std::vector<HaloFaceCandidate>, 2U> halo_candidates;
  const auto longitudinal_midpoint =
      (static_cast<double>(body.minimum[2U]) + body.maximum[2U]) * 0.5;
  for (const auto &face : body.faces) {
    if (!face.has_texture_coordinates || face.primitive_type == 8U ||
        face.primitive_type == 9U || face.vertex_count < 3U ||
        face.vertex_count > 4U || face.normal_index >= body.normals.size()) {
      continue;
    }
    const auto &source_normal = body.normals[face.normal_index];
    if (!std::isfinite(source_normal[0U]) ||
        !std::isfinite(source_normal[1U]) ||
        !std::isfinite(source_normal[2U]) ||
        std::abs(source_normal[2U]) < 0.5F) {
      continue;
    }
    HaloFaceCandidate candidate;
    candidate.material_index = face.material_name_index;
    candidate.normal = {source_normal[0U], source_normal[1U],
                        source_normal[2U]};
    for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
      const auto &position = body.positions[face.position_indices[vertex]];
      for (std::size_t axis = 0U; axis < candidate.center.size(); ++axis) {
        candidate.center[axis] += position[axis] / face.vertex_count;
      }
    }
    const auto side = candidate.center[2U] >= longitudinal_midpoint ? 0U : 1U;
    halo_candidates[side].push_back(candidate);
  }
  if (car.halo_paths.size() != 2U) {
    throw std::runtime_error("CAR definition omits its two Halo images");
  }
  for (const auto &record : body.light_records) {
    if (record.image_index >= result.halo_attachments.size()) {
      throw std::runtime_error(
          "CAR body MYO lamp record references an unavailable Halo slot");
    }
    mh::game::CollisionVector3 center{};
    for (std::size_t axis = 0U; axis < center.size(); ++axis) {
      center[axis] = (static_cast<double>(record.position[axis]) +
                      static_cast<double>(record.secondary_position[axis])) *
                     0.5;
    }
    auto face_normal = mh::game::CollisionVector3{
        0.0, 0.0, record.image_index == 0U ? 1.0 : -1.0};
    const auto &candidates = halo_candidates[record.image_index];
    if (!candidates.empty()) {
      const auto selected = std::min_element(
          candidates.begin(), candidates.end(),
          [&center](const auto &left, const auto &right) {
            const auto squared_distance = [&center](const auto &candidate) {
              auto value = 0.0;
              for (std::size_t axis = 0U; axis < center.size(); ++axis) {
                const auto difference = candidate.center[axis] - center[axis];
                value += difference * difference;
              }
              return value;
            };
            return squared_distance(left) < squared_distance(right);
          });
      face_normal = selected->normal;
    }
    const auto expected_longitudinal_sign =
        record.image_index == 0U ? 1.0 : -1.0;
    if (face_normal[2U] * expected_longitudinal_sign < 0.0) {
      for (auto &component : face_normal) {
        component = -component;
      }
    }
    center[1U] += body_presentation_y;
    result.halo_attachments[record.image_index].push_back(
        {center, face_normal});
    if (record.image_index == 0U) {
      // p3.1 RVA 0x507ec installs these exact normalized front-lamp aim
      // vectors from the authored lamp's local X side.
      constexpr double lateral = 0.19611613513818404;
      constexpr double forward = 0.9805806756909202;
      result.headlight_sources.push_back(
          {center, {center[0U] < 0.0 ? -lateral : lateral, 0.0, forward}});
    }
  }
  for (std::size_t side = 0U; side < halo_candidates.size(); ++side) {
    if (halo_candidates[side].empty() || side >= car.halo_paths.size()) {
      continue;
    }
    const auto selected = std::max_element(
        halo_candidates[side].begin(), halo_candidates[side].end(),
        [side](const auto &left, const auto &right) {
          return side == 0U ? left.center[2U] < right.center[2U]
                            : left.center[2U] > right.center[2U];
        });
    if (early_s40_presentation && side == 1U &&
        result.halo_attachments[side].empty()) {
      // This early MYO has no explicit lamp records. Its rear light clusters
      // are the four rear-facing VMainB faces (upper/lower on each side); the
      // most rearward textured face used by the generic fallback belongs to
      // the wing. Average each authored pair so the halos sit on the actual
      // left and right tail lamps.
      std::array<mh::game::CollisionVector3, 2U> centers{};
      std::array<mh::game::CollisionVector3, 2U> normals{};
      std::array<std::size_t, 2U> counts{};
      std::optional<std::size_t> lamp_material;
      for (const auto &candidate : halo_candidates[side]) {
        if (candidate.material_index >= body.names.size() ||
            ascii_lower(body.names[candidate.material_index]) != "vmainb.iff" ||
            candidate.normal[2U] > -0.5) {
          continue;
        }
        const auto lamp = candidate.center[0U] < 0.0 ? 0U : 1U;
        for (std::size_t axis = 0U; axis < centers[lamp].size(); ++axis) {
          centers[lamp][axis] += candidate.center[axis];
          normals[lamp][axis] += candidate.normal[axis];
        }
        ++counts[lamp];
        lamp_material = candidate.material_index;
      }
      if (counts[0U] != 0U && counts[1U] != 0U && lamp_material.has_value()) {
        for (std::size_t lamp = 0U; lamp < centers.size(); ++lamp) {
          auto length_squared = 0.0;
          for (std::size_t axis = 0U; axis < centers[lamp].size(); ++axis) {
            centers[lamp][axis] /= static_cast<double>(counts[lamp]);
            length_squared += normals[lamp][axis] * normals[lamp][axis];
          }
          const auto inverse_length =
              length_squared > 0.0 ? 1.0 / std::sqrt(length_squared) : 1.0;
          for (auto &component : normals[lamp]) {
            component *= inverse_length;
          }
          centers[lamp][1U] += body_presentation_y;
          result.halo_attachments[side].push_back(
              {centers[lamp], normals[lamp]});
        }
        result.emissive_material_indices[side] = *lamp_material;
      }
    }
    if (result.halo_attachments[side].empty()) {
      // The early Volvo MYO predates the later explicit lamp table. Its lamp
      // faces are still authored as the extreme forward/rear textured faces,
      // so use the same geometry candidates that identify the emissive
      // material instead of rejecting the otherwise valid model.
      auto center = selected->center;
      center[1U] += body_presentation_y;
      auto face_normal = selected->normal;
      const auto expected_longitudinal_sign = side == 0U ? 1.0 : -1.0;
      if (face_normal[2U] * expected_longitudinal_sign < 0.0) {
        for (auto &component : face_normal) {
          component = -component;
        }
      }
      result.halo_attachments[side].push_back({center, face_normal});
      if (side == 0U) {
        constexpr double lateral = 0.19611613513818404;
        constexpr double forward = 0.9805806756909202;
        result.headlight_sources.push_back(
            {center, {center[0U] < 0.0 ? -lateral : lateral, 0.0, forward}});
      }
    }
    if (!result.emissive_material_indices[side].has_value()) {
      result.emissive_material_indices[side] = selected->material_index;
    }
    const auto halo_source = mh::content::read_iff_ilbm(
        resolve_relative_case_insensitive(content_root, car.halo_paths[side]));
    mh::content::PamRgbaImage halo_image;
    halo_image.width = halo_source.width;
    halo_image.height = halo_source.height;
    halo_image.rgba = halo_source.rgba;
    result.halo_images[side].push_back(std::move(halo_image));
  }
  result.face_count += body.faces.size();
  result.components.push_back(
      {std::move(body), {0.0, body_presentation_y, 0.0}, {}, {}, std::nullopt});
  if (!car.physics.has_value()) {
    throw std::runtime_error(
        "CAR visual omits its wheel suspension definition");
  }
  const auto active_wheel_response = mh::game::make_original_wheel_response(
      car.physics->spring_length, car.physics->spring_strength,
      suspension_profile);
  const mh::game::CollisionVector3 suspension_axis{
      0.0, -active_wheel_response.spring_length, 0.0};
  for (std::size_t index = 0U; index < car.wheels.size(); ++index) {
    const auto &wheel = car.wheels[index];
    auto model = read_model(model_lod_path(wheel.model_base, lod));
    // Circle fits locate the S40 axles in X/Z. Its CAR uses one shared visual
    // height, while measured rest-state compensation preserves suspension.
    constexpr std::array<mh::game::CollisionVector3, 4U>
        s40_fitted_visual_centers{{
            {-0.757720402, 0.305573, 1.335512739},
            {0.757720402, 0.305573, 1.335512739},
            {0.770104233, 0.305573, -1.267001193},
            {-0.770104233, 0.305573, -1.267001193},
        }};
    constexpr std::array<double, 4U> s40_flat_grid_suspension_states{
        0.790815353, 0.790815353, 0.790815353, 0.790815353};
    auto authored_center = mh::game::CollisionVector3{
        wheel.center.x, wheel.center.y, wheel.center.z};
    if (early_s40_presentation) {
      const auto &fitted = s40_fitted_visual_centers[index];
      authored_center = {
          fitted[0U] / 1.25,
          fitted[1U] + active_wheel_response.spring_length *
                           s40_flat_grid_suspension_states[index],
          fitted[2U],
      };
    }
    result.face_count += model.faces.size();
    result.components.push_back({std::move(model),
                                 authored_center,
                                 suspension_axis,
                                 {},
                                 std::optional{index}});
  }
  return result;
}

void advance_wheel_visual_spin(WheelVisualState &state,
                               const double front_spin_rate,
                               const double rear_spin_rate,
                               const double seconds) {
  state.spin_radians[0U] = mh::game::original_wheel_visual_spin_phase(
      state.spin_radians[0U], front_spin_rate, seconds);
  state.spin_radians[1U] = state.spin_radians[0U];
  state.spin_radians[2U] = mh::game::original_wheel_visual_spin_phase(
      state.spin_radians[2U], rear_spin_rate, seconds);
  state.spin_radians[3U] = state.spin_radians[2U];
}

void update_wheel_visual(WheelVisualState &state,
                         const double longitudinal_speed,
                         const double retained_steering,
                         const std::array<float, 4U> &retained_wheel_states,
                         const double seconds,
                         const double launch_wheelspin = 0.0) {
  // Body +0xd8 is p3.1's independently retained steering-presentation state.
  // The original front-wheel owner multiplies it by the binary64 0.7
  // constant at VA 0x00505216 before applying the wheel-object rotation.
  state.steering =
      mh::game::original_front_wheel_visual_angle(retained_steering);
  state.suspension_states = retained_wheel_states;
  // Every non-player sample in the canonical eight-cycle p3.1 opening corpus
  // stores body +0xe4/+0xe8 as exactly 2 * local longitudinal velocity.
  // Retain one shared front and one shared rear phase, as the object owner
  // does.
  const auto captured_spin_rate = longitudinal_speed * 2.0;
  constexpr double launch_spin_rate = 36.0;
  advance_wheel_visual_spin(
      state, captured_spin_rate,
      captured_spin_rate + launch_wheelspin * launch_spin_rate, seconds);
}

std::array<float, 4U>
settled_wheel_visual_states(const mh::game::OriginalVehicleRuntime &vehicle) {
  const auto contacts = vehicle.current_wheel_contacts();
  std::array<float, 4U> result{};
  for (std::size_t wheel = 0U; wheel < result.size(); ++wheel) {
    result[wheel] =
        static_cast<float>(contacts[wheel].scalar_state.state_fraction);
  }
  return result;
}

std::filesystem::path
find_car_definition_by_name(const std::filesystem::path &content_root,
                            const std::string_view name) {
  const auto game_root =
      find_child_directory_case_insensitive(content_root, "Game");
  for (const auto &entry : std::filesystem::directory_iterator(game_root)) {
    if (!entry.is_regular_file() ||
        ascii_lower(entry.path().extension().string()) != ".car") {
      continue;
    }
    const auto candidate = mh::content::read_car(entry.path());
    if (ascii_lower(candidate.name) == ascii_lower(std::string(name))) {
      return entry.path();
    }
  }
  throw std::runtime_error("MDE racer car has no matching retail CAR: " +
                           std::string(name));
}

std::string
car_name_by_original_index(const std::filesystem::path &content_root,
                           const std::uint8_t index) {
  const auto game_root =
      find_child_directory_case_insensitive(content_root, "Game");
  const auto number = static_cast<std::uint32_t>(index) + 1U;
  const auto stem = "car" + (number < 10U ? std::string("0") : std::string{}) +
                    std::to_string(number) + ".car";
  const auto definition_path = find_sibling_case_insensitive(game_root, stem);
  return mh::content::read_car(definition_path).name;
}

std::vector<RecordedCarVisual>
load_recorded_car_visuals(const mh::content::MdeV3Data &demo,
                          const std::filesystem::path &content_root,
                          const mh::ui::GraphicCarDetail detail) {
  std::vector<RecordedCarVisual> result;
  result.reserve(demo.stream_count);
  for (const auto &racer : demo.racer_slots) {
    if (!racer.active) {
      continue;
    }
    const auto definition_path =
        find_car_definition_by_name(content_root, racer.car_name);
    auto definition = mh::content::read_car(definition_path);
    apply_playable_car_tuning(definition);
    auto visual = load_car_visual(
        definition, content_root, std::nullopt,
        mh::game::OriginalWheelResponseProfile::standard, detail);
    const auto collision =
        mh::content::read_col(resolve_relative_case_insensitive(
            content_root, definition.collision_path));
    auto contact_shape = mh::game::make_original_dynamic_vehicle_contact_shape(
        definition, collision);
    result.push_back({racer, std::move(definition), std::move(visual),
                      std::move(contact_shape)});
  }
  if (result.size() != demo.stream_count) {
    throw std::runtime_error(
        "MDE racer metadata does not map one car to every stream");
  }
  return result;
}

void bind_car_materials(SDL_Renderer *renderer, WorldVisual &world,
                        CarVisual &car) {
  const auto car_catalog =
      car.texture_root.empty()
          ? std::optional<mh::content::TextureCatalog>{}
          : std::optional<mh::content::TextureCatalog>{
                mh::content::build_texture_catalog(car.texture_root)};
  const auto &catalog = car_catalog.has_value() ? *car_catalog : world.catalog;
  const auto find_unique_asset = [](const mh::content::TextureCatalog &source,
                                    const std::string &material_name,
                                    const std::string &archive_name) {
    const mh::content::TextureAsset *result = nullptr;
    for (const auto &asset : source.assets) {
      if (asset_name(asset) != ascii_lower(material_name) ||
          (asset.source_kind == "pdi-iff" &&
           filename_lower(std::filesystem::path(asset.source_path)) !=
               ascii_lower(archive_name))) {
        continue;
      }
      if (result != nullptr) {
        throw std::runtime_error("car material does not resolve uniquely: " +
                                 material_name);
      }
      result = &asset;
    }
    return result;
  };
  for (auto &component : car.components) {
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
      const auto *asset =
          find_unique_asset(catalog, component.model.names[index], "tex2.pdi");
      const auto asset_from_car_catalog =
          asset != nullptr && car_catalog.has_value();
      if (asset == nullptr && car_catalog.has_value()) {
        asset = &select_track_texture(world.catalog,
                                      component.model.names[index], "tex2.pdi");
      }
      if (asset == nullptr) {
        throw std::runtime_error("car material is unavailable: " +
                                 component.model.names[index]);
      }
      const auto material_identity =
          car_catalog.has_value()
              ? asset->logical_id + "/" + asset->content_sha256
              : asset->logical_id;
      const auto existing =
          std::find_if(world.materials.begin(), world.materials.end(),
                       [&material_identity](const auto &material) {
                         return material.logical_id == material_identity;
                       });
      if (existing != world.materials.end()) {
        component.material_indices[index] = static_cast<std::size_t>(
            std::distance(world.materials.begin(), existing));
        continue;
      }
      const auto &asset_catalog =
          asset_from_car_catalog ? catalog : world.catalog;
      auto image = mh::content::load_texture_asset_rgba(asset_catalog, *asset);
      static_cast<void>(mh::content::make_rgba_opaque(image));
      MaterialTexture material;
      material.logical_id = material_identity;
      material.width = image.width;
      material.height = image.height;
      material.texture = upload_texture(renderer, image);
      material.image = image;
      const auto override = find_runtime_texture_override(world.override_paths,
                                                          asset->logical_id);
      if (override.has_value()) {
        auto override_image = mh::content::read_pam_rgba(*override);
        material.override_texture = upload_texture(renderer, override_image);
        material.override_image = std::move(override_image);
        ++world.override_materials;
      }
      component.material_indices[index] = world.materials.size();
      world.materials.push_back(std::move(material));
    }
  }
}

double dot(const mh::game::CollisionVector3 &left,
           const mh::game::CollisionVector3 &right) {
  return left[0U] * right[0U] + left[1U] * right[1U] + left[2U] * right[2U];
}

mh::game::CollisionVector3 subtract(const mh::game::CollisionVector3 &left,
                                    const mh::game::CollisionVector3 &right) {
  return {left[0U] - right[0U], left[1U] - right[1U], left[2U] - right[2U]};
}

mh::game::CollisionVector3 cross(const mh::game::CollisionVector3 &left,
                                 const mh::game::CollisionVector3 &right) {
  return {left[1U] * right[2U] - left[2U] * right[1U],
          left[2U] * right[0U] - left[0U] * right[2U],
          left[0U] * right[1U] - left[1U] * right[0U]};
}

mh::game::CollisionVector3 normalized(const mh::game::CollisionVector3 &value) {
  const auto length = std::sqrt(dot(value, value));
  if (!std::isfinite(length) || length <= 1.0e-8) {
    throw std::runtime_error("camera basis is degenerate");
  }
  return {value[0U] / length, value[1U] / length, value[2U] / length};
}

mh::game::CollisionVector3
normalized_or(const mh::game::CollisionVector3 &value,
              const mh::game::CollisionVector3 &fallback) {
  const auto length = std::sqrt(dot(value, value));
  if (!std::isfinite(length) || length <= 1.0e-8) {
    return fallback;
  }
  return {value[0U] / length, value[1U] / length, value[2U] / length};
}

mh::content::PamRgbaImage make_lamp_beam_image() {
  constexpr std::uint32_t side = 64U;
  mh::content::PamRgbaImage image;
  image.width = side;
  image.height = side;
  image.rgba.resize(static_cast<std::size_t>(side) * side * 4U);
  for (std::uint32_t y = 0U; y < side; ++y) {
    const auto vertical =
        static_cast<double>(y) / static_cast<double>(side - 1U);
    for (std::uint32_t x = 0U; x < side; ++x) {
      const auto horizontal =
          static_cast<double>(x) / static_cast<double>(side - 1U);
      const auto smoothstep = [](const double edge, const double value) {
        const auto position = std::clamp(value / edge, 0.0, 1.0);
        return position * position * (3.0 - 2.0 * position);
      };
      // Use an exact transparent border and broad cubic shoulders on all four
      // sides. Exact zeroes stop texture clamping from revealing the quad's
      // outline, while the wide transition keeps the cone soft at any scale.
      const auto side_distance = 1.0 - std::abs(horizontal * 2.0 - 1.0);
      const auto lateral = smoothstep(0.88, side_distance);
      const auto source_fade = smoothstep(0.34, vertical);
      const auto receiver_fade = smoothstep(0.42, 1.0 - vertical);
      const auto coverage = lateral * source_fade * receiver_fade * 0.65;
      const auto pixel = (static_cast<std::size_t>(y) * side + x) * 4U;
      image.rgba[pixel] = 255U;
      image.rgba[pixel + 1U] = 255U;
      image.rgba[pixel + 2U] = 255U;
      image.rgba[pixel + 3U] = static_cast<std::uint8_t>(
          std::clamp(coverage * 255.0, 0.0, 255.0) + 0.5);
    }
  }
  return image;
}

void prepare_world_lamp_beams(WorldVisual &visual,
                              const mh::game::CollisionWorld &collision_world,
                              const mh::content::AiRouteData &route) {
  visual.lamp_beams.clear();
  visual.lamp_beam_images = {make_lamp_beam_image()};
  visual.lamp_beams.reserve(visual.world.lens_flares.size());
  if (route.samples.size() < 2U) {
    return;
  }
  for (const auto &flare : visual.world.lens_flares) {
    mh::game::CollisionVector3 source{};
    for (std::size_t axis = 0U; axis < source.size(); ++axis) {
      source[axis] = (static_cast<double>(flare.position[axis]) +
                      static_cast<double>(flare.secondary_position[axis])) *
                     0.5;
    }
    mh::game::CollisionVector3 authored_edge{};
    for (std::size_t axis = 0U; axis < authored_edge.size(); ++axis) {
      authored_edge[axis] =
          static_cast<double>(flare.secondary_position[axis]) -
          static_cast<double>(flare.position[axis]);
    }
    const mh::content::MywPointLight *nearest_light = nullptr;
    auto nearest_distance_squared = std::numeric_limits<double>::max();
    for (const auto &light : visual.world.lights) {
      const auto dx = static_cast<double>(light.position[0U]) - source[0U];
      const auto dy = static_cast<double>(light.position[1U]) - source[1U];
      const auto dz = static_cast<double>(light.position[2U]) - source[2U];
      const auto distance_squared = dx * dx + dy * dy + dz * dz;
      if (distance_squared < nearest_distance_squared) {
        nearest_distance_squared = distance_squared;
        nearest_light = &light;
      }
    }
    if (nearest_light == nullptr) {
      continue;
    }
    const auto match_distance = std::sqrt(nearest_distance_squared);
    const auto maximum_match_distance = std::max(
        4.5, std::min(12.0, static_cast<double>(nearest_light->range) * 0.45));
    if (match_distance > maximum_match_distance) {
      continue;
    }

    // Project the fixture onto the closest segment of the authored AI racing
    // line. This supplies a stable track-facing aim for roadside lamps without
    // doing any per-frame route search.
    std::array<double, 2U> route_target{};
    auto route_receiver_half_width = 0.0;
    auto nearest_route_distance_squared = std::numeric_limits<double>::max();
    for (std::size_t sample = 0U; sample < route.samples.size(); ++sample) {
      const auto next = (sample + 1U) % route.samples.size();
      const std::array<double, 2U> first{route.samples[sample].position[0U],
                                         route.samples[sample].position[1U]};
      const std::array<double, 2U> second{route.samples[next].position[0U],
                                          route.samples[next].position[1U]};
      const auto segment_x = second[0U] - first[0U];
      const auto segment_z = second[1U] - first[1U];
      const auto length_squared = segment_x * segment_x + segment_z * segment_z;
      if (length_squared <= 1.0e-10) {
        continue;
      }
      const auto fraction = std::clamp(((source[0U] - first[0U]) * segment_x +
                                        (source[2U] - first[1U]) * segment_z) /
                                           length_squared,
                                       0.0, 1.0);
      const std::array<double, 2U> projected{first[0U] + segment_x * fraction,
                                             first[1U] + segment_z * fraction};
      const auto dx = projected[0U] - source[0U];
      const auto dz = projected[1U] - source[2U];
      const auto distance_squared = dx * dx + dz * dz;
      if (distance_squared < nearest_route_distance_squared) {
        nearest_route_distance_squared = distance_squared;
        route_target = projected;
        const auto first_road_width =
            std::abs(
                static_cast<double>(route.samples[sample].surface_values[0U])) +
            std::abs(
                static_cast<double>(route.samples[sample].surface_values[1U]));
        const auto second_road_width =
            std::abs(
                static_cast<double>(route.samples[next].surface_values[0U])) +
            std::abs(
                static_cast<double>(route.samples[next].surface_values[1U]));
        const auto road_width =
            first_road_width * (1.0 - fraction) + second_road_width * fraction;
        // The receiver quad is two half-widths wide. A quarter of the authored
        // full road width therefore makes its road-side edge cover 50%.
        route_receiver_half_width = road_width * 0.25;
      }
    }
    if (!std::isfinite(nearest_route_distance_squared)) {
      continue;
    }
    // AI.dat's X/Z sample positions are the authored road centreline. Aim at
    // that exact closest point: the previous range-limited inward step could
    // leave a distant fixture's receiver stranded near the road edge.
    const mh::game::CollisionVector3 aimed_above_track{
        route_target[0U], source[1U] + 0.1, route_target[1U]};
    const auto receiver =
        collision_world.raycast(aimed_above_track, {0.0, -1.0, 0.0}, 18.0);
    if (!receiver.has_value() || receiver->normal[1U] < 0.62 ||
        receiver->distance < 1.6) {
      continue;
    }

    WorldLampBeam beam;
    beam.source = source;
    beam.receiver = receiver->point;
    beam.receiver[1U] += 0.025;
    auto source_edge = authored_edge;
    source_edge[1U] = 0.0;
    beam.source_edge_direction = normalized_or(
        source_edge,
        mh::game::CollisionVector3{-(beam.receiver[2U] - beam.source[2U]), 0.0,
                                   beam.receiver[0U] - beam.source[0U]});
    beam.source_half_width = std::clamp(
        std::sqrt(dot(authored_edge, authored_edge)) * 0.5, 0.22, 1.35);
    beam.receiver_half_width = std::max(
        std::clamp(static_cast<double>(nearest_light->range) * 0.11, 1.8, 4.0),
        route_receiver_half_width);
    beam.alpha = 0.28F;
    for (std::size_t channel = 0U; channel < beam.color.size(); ++channel) {
      beam.color[channel] =
          std::clamp(nearest_light->color[channel], 0.0F, 1.0F);
    }
    visual.lamp_beams.push_back(beam);
  }
}

SDL_FPoint
vehicle_environment_coordinate(mh::game::CollisionVector3 world_normal,
                               const PerspectiveView &view) {
  world_normal = normalized_or(world_normal, {0.0, 1.0, 0.0});
  const auto horizontal = dot(world_normal, view.right);
  const auto vertical = dot(world_normal, view.up);
  constexpr double original_map_scale = 0.495;
  return {static_cast<float>(
              std::clamp(0.5 + horizontal * original_map_scale, 0.0, 1.0)),
          static_cast<float>(
              std::clamp(0.5 - vertical * original_map_scale, 0.0, 1.0))};
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

const std::vector<mh::content::PamRgbaImage> *
vehicle_environment_images(const VehicleEnvironmentTextures &textures,
                           const VehicleEnvironmentMap map) {
  switch (map) {
  case VehicleEnvironmentMap::environment:
    return &textures.environment_raster_levels;
  case VehicleEnvironmentMap::reflection:
    return &textures.reflection_raster_levels;
  case VehicleEnvironmentMap::phong:
    return &textures.phong_raster_levels;
  case VehicleEnvironmentMap::none:
    return nullptr;
  }
  return nullptr;
}

void begin_presentation_back_buffer(SDL_Renderer *renderer,
                                    PresentationBackBuffers &buffers,
                                    const int width, const int height,
                                    const std::size_t back_buffer_count,
                                    const SDL_PixelFormat pixel_format,
                                    const bool direct) {
  if (back_buffer_count != 1U && back_buffer_count != 2U) {
    throw std::runtime_error("race back-buffer count must be one or two");
  }
  if (direct) {
    if (!buffers.direct || buffers.width != width || buffers.height != height) {
      buffers.textures.clear();
      buffers.width = width;
      buffers.height = height;
      buffers.pixel_format = pixel_format;
      buffers.active = 0U;
      buffers.completed_frames = 0U;
      buffers.direct = true;
    }
    require(SDL_SetRenderTarget(renderer, nullptr),
            "select direct race presentation target");
    return;
  }
  if (buffers.width != width || buffers.height != height ||
      buffers.textures.size() != back_buffer_count ||
      buffers.pixel_format != pixel_format || buffers.direct) {
    buffers.textures.clear();
    buffers.textures.reserve(back_buffer_count);
    for (std::size_t index = 0U; index < back_buffer_count; ++index) {
      SdlPointer<SDL_Texture, SDL_DestroyTexture> texture(
          SDL_CreateTexture(renderer, pixel_format, SDL_TEXTUREACCESS_TARGET,
                            width, height),
          SDL_DestroyTexture);
      require(texture != nullptr, "create race presentation back buffer");
      require(SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_NONE),
              "set race presentation back-buffer blend mode");
      require(SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_NEAREST),
              "set race presentation back-buffer scale mode");
      buffers.textures.push_back(std::move(texture));
    }
    buffers.width = width;
    buffers.height = height;
    buffers.pixel_format = pixel_format;
    buffers.active = 0U;
    buffers.completed_frames = 0U;
    buffers.direct = false;
  }
  require(SDL_SetRenderTarget(renderer, buffers.textures[buffers.active].get()),
          "select race presentation back buffer");
}

void present_presentation_back_buffer(SDL_Renderer *renderer,
                                      PresentationBackBuffers &buffers,
                                      const bool underwater = false,
                                      const double effect_seconds = 0.0,
                                      const SDL_FRect *destination = nullptr,
                                      const bool motion_blur = false) {
  if (buffers.direct) {
    ++buffers.completed_frames;
    return;
  }
  if (buffers.textures.empty() || buffers.active >= buffers.textures.size()) {
    throw std::runtime_error("race presentation back buffer is not active");
  }
  auto *completed = buffers.textures[buffers.active].get();
  require(SDL_SetRenderTarget(renderer, nullptr),
          "select race presentation front buffer");
  if (!underwater) {
    if (destination != nullptr) {
      require(SDL_SetRenderDrawColor(renderer, 0U, 0U, 0U, 255U),
              "set race presentation border color");
      require(SDL_RenderClear(renderer), "clear race presentation borders");
    }
    require(SDL_RenderTexture(renderer, completed, nullptr, destination),
            "copy race presentation back buffer");
    if (motion_blur && buffers.completed_frames != 0U &&
        buffers.textures.size() > 1U) {
      const auto previous_index =
          (buffers.active + buffers.textures.size() - 1U) %
          buffers.textures.size();
      auto *previous = buffers.textures[previous_index].get();
      require(SDL_SetTextureBlendMode(previous, SDL_BLENDMODE_BLEND),
              "set motion-blur history blend mode");
      // A 32% history contribution is clearly visible at racing speed while
      // retaining the low-cost two-frame implementation on every backend.
      require(SDL_SetTextureAlphaMod(previous, 82U),
              "set motion-blur history alpha");
      require(SDL_RenderTexture(renderer, previous, nullptr, destination),
              "blend motion-blur history");
      require(SDL_SetTextureAlphaMod(previous, 255U),
              "restore motion-blur history alpha");
      require(SDL_SetTextureBlendMode(previous, SDL_BLENDMODE_NONE),
              "restore motion-blur history blend mode");
    }
  } else {
    require(SDL_SetRenderDrawColor(renderer, 0U, 0U, 0U, 255U),
            "set underwater border color");
    require(SDL_RenderClear(renderer), "clear underwater presentation");
    constexpr int strip_count = 60;
    for (int strip = 0; strip < strip_count; ++strip) {
      const auto source_y = static_cast<float>(buffers.height) * strip /
                            static_cast<float>(strip_count);
      const auto next_y = static_cast<float>(buffers.height) * (strip + 1) /
                          static_cast<float>(strip_count);
      const auto strip_height = next_y - source_y;
      // p3.1 RVA 0x0005ce67 replaces Thunder's 0.005 radians/ms with
      // Underwater's exact 0.001 radians/ms.
      const auto wave = std::sin(effect_seconds + strip * 0.32);
      const auto displacement = static_cast<float>(wave * 4.0);
      const SDL_FRect source{0.0F, source_y, static_cast<float>(buffers.width),
                             strip_height};
      const auto output_x = destination != nullptr ? destination->x : 0.0F;
      const auto output_y = destination != nullptr ? destination->y : 0.0F;
      const auto output_scale_x =
          destination != nullptr
              ? destination->w / static_cast<float>(buffers.width)
              : 1.0F;
      const auto output_scale_y =
          destination != nullptr
              ? destination->h / static_cast<float>(buffers.height)
              : 1.0F;
      const SDL_FRect strip_destination{
          output_x + displacement * output_scale_x,
          output_y + source_y * output_scale_y,
          static_cast<float>(buffers.width) * output_scale_x,
          (strip_height + 1.0F) * output_scale_y};
      require(
          SDL_RenderTexture(renderer, completed, &source, &strip_destination),
          "render underwater wave strip");
    }
  }
  ++buffers.completed_frames;
  buffers.active = (buffers.active + 1U) % buffers.textures.size();
}

void render_skid_marks(const SkidMarkEmitter &emitter,
                       const PerspectiveView &view,
                       const double render_distance,
                       const WorldRenderEnvironment &,
                       SceneDepthResources &depth_resources) {
  for (const auto &trail : emitter.trails) {
    for (const auto &mark : trail) {
      const auto ribbon = mh::game::original_accelerated_skid_mark_ribbon(
          mark.start, mark.end, mark.contact_normal);
      if (!ribbon.has_value()) {
        continue;
      }

      std::vector<ViewTextureVertex> polygon;
      polygon.reserve(6U);
      for (const auto &corner : ribbon->corners) {
        ViewTextureVertex vertex;
        vertex.point = to_view(view, corner);
        polygon.push_back(vertex);
      }

      std::vector<ViewTextureVertex> clipped;
      clipped.reserve(6U);
      auto previous = polygon.back();
      auto previous_inside = previous.point.z >= view.near_plane;
      for (const auto &current : polygon) {
        const auto current_inside = current.point.z >= view.near_plane;
        const auto intersection = [&view](const ViewTextureVertex &outside,
                                          const ViewTextureVertex &inside) {
          const auto denominator = inside.point.z - outside.point.z;
          const auto factor = (view.near_plane - outside.point.z) / denominator;
          ViewTextureVertex result;
          result.point = {
              outside.point.x + (inside.point.x - outside.point.x) * factor,
              outside.point.y + (inside.point.y - outside.point.y) * factor,
              view.near_plane};
          return result;
        };
        if (current_inside) {
          if (!previous_inside) {
            clipped.push_back(intersection(previous, current));
          }
          clipped.push_back(current);
        } else if (previous_inside) {
          clipped.push_back(intersection(current, previous));
        }
        previous = current;
        previous_inside = current_inside;
      }
      if (clipped.size() < 3U) {
        continue;
      }
      if (std::all_of(clipped.begin(), clipped.end(),
                      [render_distance](const auto &vertex) {
                        return vertex.point.z > render_distance;
                      })) {
        continue;
      }

      const auto color = mh::game::original_skid_mark_color(mark.material_mark);
      const std::array<float, 3U> base_color{
          static_cast<float>(color[0U]) / 255.0F,
          static_cast<float>(color[1U]) / 255.0F,
          static_cast<float>(color[2U]) / 255.0F};
      for (std::size_t corner = 1U; corner + 1U < clipped.size(); ++corner) {
        const std::array<ViewTextureVertex, 3U> triangle{
            clipped[0U], clipped[corner], clipped[corner + 1U]};
        std::array<SDL_Vertex, 4U> vertices{};
        std::array<double, 4U> view_depths{};
        for (std::size_t vertex = 0U; vertex < triangle.size(); ++vertex) {
          const auto projected = project(view, triangle[vertex].point);
          vertices[vertex].position = {projected.x, projected.y};
          vertices[vertex].color = {base_color[0U], base_color[1U],
                                    base_color[2U], 1.0F};
          view_depths[vertex] = triangle[vertex].point.z;
        }
        depth_resources.record_geometry(vertices, view_depths, 3U, nullptr,
                                        false);
      }
    }
  }
}

void render_tire_smoke(const TireSmokeEmitter &emitter,
                       const PerspectiveView &view,
                       const double render_distance,
                       const WorldRenderEnvironment &,
                       SceneDepthResources &depth_resources) {
  // Six alpha-interpolated wedges form a soft puff without sampling a sprite.
  // The edge vertices carry zero coverage, so there is no rectangular card and
  // no backend-specific alpha-mask interpretation.
  constexpr std::size_t wedge_count = 6U;
  constexpr double full_circle = std::numbers::pi_v<double> * 2.0;
  for (auto particle = emitter.particles.rbegin();
       particle != emitter.particles.rend(); ++particle) {
    const auto life = std::clamp(
        particle->age_seconds / particle->lifetime_seconds, 0.0, 1.0);
    const auto fade_in = std::clamp(particle->age_seconds / 0.08, 0.0, 1.0);
    const auto fade_out = (1.0 - life) * (1.0 - life);
    const auto alpha = static_cast<float>(0.52 * fade_in * fade_out);
    if (alpha <= 0.001F) {
      continue;
    }
    const auto center = to_view(view, particle->position);
    if (center.z < view.near_plane || center.z > render_distance) {
      continue;
    }
    const auto radius = particle->size * (1.0 + life * 2.1);
    const auto radius_x = radius * 1.15;
    const auto radius_y = radius * 0.82;
    const auto gray = static_cast<float>(0.54 + life * 0.12);
    for (std::size_t wedge = 0U; wedge < wedge_count; ++wedge) {
      const auto first_angle =
          particle->phase + full_circle * static_cast<double>(wedge) /
                                static_cast<double>(wedge_count);
      const auto second_angle =
          particle->phase + full_circle * static_cast<double>(wedge + 1U) /
                                static_cast<double>(wedge_count);
      const std::array<ViewPoint, 3U> points{
          {center,
           {center.x + std::cos(first_angle) * radius_x,
            center.y + std::sin(first_angle) * radius_y, center.z},
           {center.x + std::cos(second_angle) * radius_x,
            center.y + std::sin(second_angle) * radius_y, center.z}}};
      std::array<SDL_Vertex, 4U> vertices{};
      std::array<double, 4U> view_depths{};
      for (std::size_t vertex = 0U; vertex < points.size(); ++vertex) {
        const auto projected = project(view, points[vertex]);
        vertices[vertex].position = {projected.x, projected.y};
        vertices[vertex].color = {gray, gray, gray,
                                  vertex == 0U ? alpha : 0.0F};
        view_depths[vertex] = center.z;
      }
      depth_resources.record_geometry(vertices, view_depths, 3U, nullptr, false,
                                      SceneBlendMode::alpha);
    }
  }
}

void render_sparks(const SparkEmitter &emitter, const PerspectiveView &view,
                   const double render_distance,
                   const WorldRenderEnvironment &,
                   SceneDepthResources &depth_resources) {
  for (auto particle = emitter.particles.rbegin();
       particle != emitter.particles.rend(); ++particle) {
    const auto life = std::clamp(
        particle->age_seconds / particle->lifetime_seconds, 0.0, 1.0);
    const auto alpha = static_cast<float>((1.0 - life) * (1.0 - life));
    if (alpha <= 0.001F) {
      continue;
    }
    auto tail_position = particle->position;
    for (std::size_t axis = 0U; axis < tail_position.size(); ++axis) {
      tail_position[axis] -= particle->velocity[axis] * 0.045;
    }
    const auto head = to_view(view, particle->position);
    const auto tail = to_view(view, tail_position);
    if (head.z < view.near_plane || head.z > render_distance ||
        tail.z < view.near_plane) {
      continue;
    }
    const auto projected_head = project(view, head);
    const auto projected_tail = project(view, tail);
    auto direction_x = static_cast<double>(projected_head.x - projected_tail.x);
    auto direction_y = static_cast<double>(projected_head.y - projected_tail.y);
    auto length = std::hypot(direction_x, direction_y);
    if (length < 0.001) {
      direction_x = 0.0;
      direction_y = -1.0;
      length = 1.0;
    }
    direction_x /= length;
    direction_y /= length;
    const auto visible_length = std::clamp(length, 4.0, 22.0);
    const auto tail_x =
        static_cast<double>(projected_head.x) - direction_x * visible_length;
    const auto tail_y =
        static_cast<double>(projected_head.y) - direction_y * visible_length;
    const auto half_width = 1.15 + (1.0 - life) * 0.85;
    const auto perpendicular_x = -direction_y * half_width;
    const auto perpendicular_y = direction_x * half_width;
    std::array<SDL_Vertex, 4U> vertices{};
    std::array<double, 4U> view_depths{};
    vertices[0U].position = {projected_head.x, projected_head.y};
    vertices[0U].color = {1.0F, 0.88F, 0.48F, alpha};
    vertices[1U].position = {static_cast<float>(tail_x + perpendicular_x),
                             static_cast<float>(tail_y + perpendicular_y)};
    vertices[1U].color = {1.0F, 0.24F, 0.02F, 0.0F};
    vertices[2U].position = {static_cast<float>(tail_x - perpendicular_x),
                             static_cast<float>(tail_y - perpendicular_y)};
    vertices[2U].color = {1.0F, 0.24F, 0.02F, 0.0F};
    for (std::size_t vertex = 0U; vertex < 3U; ++vertex) {
      view_depths[vertex] = vertex == 0U ? head.z : tail.z;
    }
    depth_resources.record_geometry(vertices, view_depths, 3U, nullptr, false,
                                    SceneBlendMode::additive);
  }
}

PerspectiveView make_perspective_view(const mh::game::VehicleCameraPose &camera,
                                      const int width, const int height) {
  auto forward =
      normalized(subtract(camera.world_target, camera.world_position));
  auto right = normalized(cross(camera.world_up, forward));
  auto up = normalized(cross(forward, right));
  const auto projection = mh::game::original_race_projection(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  return {camera.world_position,
          right,
          up,
          forward,
          static_cast<float>(projection.center_x),
          static_cast<float>(projection.center_y),
          static_cast<float>(projection.focal_length),
          projection.near_plane};
}

ViewPoint to_view(const PerspectiveView &view,
                  const mh::game::CollisionVector3 &world) {
  const auto delta = subtract(world, view.position);
  return {dot(delta, view.right), dot(delta, view.up),
          dot(delta, view.forward)};
}

ProjectedPoint project(const PerspectiveView &view, const ViewPoint &point) {
  return {view.center_x +
              static_cast<float>(point.x / point.z) * view.focal_length,
          view.center_y -
              static_cast<float>(point.y / point.z) * view.focal_length};
}

bool project_line(const PerspectiveView &view,
                  const mh::game::CollisionVector3 &first_world,
                  const mh::game::CollisionVector3 &second_world,
                  ProjectedPoint &first_projected,
                  ProjectedPoint &second_projected) {
  auto first = to_view(view, first_world);
  auto second = to_view(view, second_world);
  if (first.z < view.near_plane && second.z < view.near_plane) {
    return false;
  }
  const auto clip = [&view](ViewPoint &behind, const ViewPoint &visible) {
    const auto denominator = visible.z - behind.z;
    if (std::abs(denominator) <= 1.0e-12) {
      return;
    }
    const auto factor = (view.near_plane - behind.z) / denominator;
    behind.x += (visible.x - behind.x) * factor;
    behind.y += (visible.y - behind.y) * factor;
    behind.z = view.near_plane;
  };
  if (first.z < view.near_plane) {
    clip(first, second);
  }
  if (second.z < view.near_plane) {
    clip(second, first);
  }
  first_projected = project(view, first);
  second_projected = project(view, second);
  return std::isfinite(first_projected.x) && std::isfinite(first_projected.y) &&
         std::isfinite(second_projected.x) && std::isfinite(second_projected.y);
}

void update_skid_marks(
    SkidMarkEmitter &emitter,
    const mh::game::OriginalVehicleModeCSceneFrame &frame,
    const mh::game::OriginalBodyVelocityState &velocity,
    const mh::game::OriginalVehicleGroundedMaterialTable &materials,
    const double launch_wheelspin = 0.0) {
  const auto sliding =
      frame.scene.grounded_damping_selection.has_value() &&
      mh::game::original_skid_mark_active(
          frame.scene.grounded_damping_selection->alternate_state_a,
          frame.scene.grounded_damping_selection->alternate_state_b,
          frame.scene.grounded_damping_selection
              ->retained_traction_accumulator);
  constexpr std::size_t original_ring_entries = 50U;
  const auto advance_distance_squared =
      mh::game::original_skid_mark_advance_distance_squared(
          velocity.local_linear[0U], velocity.local_linear[2U]);
  for (std::size_t wheel = 0U; wheel < emitter.previous.size(); ++wheel) {
    const auto &contact = frame.scene.response.contacts.wheels[wheel];
    const auto driven_wheel_spinning = wheel >= 2U && launch_wheelspin > 0.18;
    if ((!sliding && !driven_wheel_spinning) || !contact.hit.has_value()) {
      emitter.previous[wheel].reset();
      continue;
    }
    const auto material_index =
        contact.hit->material < materials.size()
            ? static_cast<std::size_t>(contact.hit->material)
            : 0U;
    const auto material_mark = materials[material_index].skid_mark;
    if (material_mark == 0U || material_mark > 4U) {
      emitter.previous[wheel].reset();
      continue;
    }
    auto point = contact.hit->point;
    auto contact_normal = contact.hit->normal;
    // Retail stores both vectors as binary32 in each 0x30-byte ring record
    // before the accelerated renderer consumes them.
    for (std::size_t axis = 0U; axis < point.size(); ++axis) {
      point[axis] = static_cast<float>(point[axis]);
      contact_normal[axis] = static_cast<float>(contact_normal[axis]);
    }
    if (emitter.previous[wheel].has_value()) {
      auto distance_squared = 0.0;
      for (std::size_t axis = 0U; axis < point.size(); ++axis) {
        const auto delta = point[axis] - emitter.previous[wheel]->point[axis];
        distance_squared += delta * delta;
      }
      const auto action = mh::game::original_skid_mark_point_action(
          distance_squared, advance_distance_squared);
      if (action == mh::game::OriginalSkidMarkPointAction::break_strip) {
        emitter.previous[wheel].reset();
        continue;
      } else if (action != mh::game::OriginalSkidMarkPointAction::retain) {
        auto &trail = emitter.trails[wheel];
        auto &sample = *emitter.previous[wheel];
        if (sample.segment_open && !trail.empty()) {
          trail.back().end = point;
          trail.back().contact_normal = contact_normal;
        } else {
          trail.push_back(
              {sample.point, point, contact_normal, sample.material_mark});
          sample.segment_open = true;
        }
        if (trail.size() > original_ring_entries) {
          trail.pop_front();
        }
        if (action == mh::game::OriginalSkidMarkPointAction::advance) {
          sample = SkidMarkEmitter::Sample{point, material_mark, false};
        }
      }
    }
    if (!emitter.previous[wheel].has_value()) {
      emitter.previous[wheel] =
          SkidMarkEmitter::Sample{point, material_mark, false};
    }
  }
}

void update_tire_smoke(TireSmokeEmitter &emitter,
                       const mh::game::OriginalVehicleModeCSceneFrame &frame,
                       const double elapsed_seconds,
                       const double launch_wheelspin = 0.0) {
  // Dense tire haze needs substantial overlap between consecutive puffs. Keep
  // the rate deterministic and let the renderer batch the resulting wedges.
  constexpr double emission_seconds = 1.0 / 36.0;
  const auto unit_noise = [](std::uint32_t value) {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return static_cast<double>(value & 0x00ffffffU) /
           static_cast<double>(0x01000000U);
  };
  for (auto &particle : emitter.particles) {
    particle.age_seconds += elapsed_seconds;
    for (std::size_t axis = 0U; axis < particle.position.size(); ++axis) {
      particle.position[axis] += particle.drift[axis] * elapsed_seconds;
    }
  }
  while (!emitter.particles.empty() &&
         emitter.particles.front().age_seconds >=
             emitter.particles.front().lifetime_seconds) {
    emitter.particles.pop_front();
  }

  // Smoke is owned by the active grip-break branch, not the stricter
  // skid-mark gate. Reusing the latter suppressed ordinary lateral/yaw and
  // handbrake smoke even though the wheels had already exceeded grip.
  const auto sliding = frame.scene.grounded_damping_selection.has_value() &&
                       mh::game::original_tire_smoke_active(
                           *frame.scene.grounded_damping_selection);
  for (std::size_t wheel = 0U; wheel < emitter.emission_accumulator.size();
       ++wheel) {
    const auto &contact = frame.scene.response.contacts.wheels[wheel];
    const auto driven_wheel_spinning = wheel >= 2U && launch_wheelspin > 0.08;
    if ((!sliding && !driven_wheel_spinning) || !contact.hit.has_value()) {
      emitter.emission_accumulator[wheel] = 0.0;
      emitter.active_sources[wheel] = false;
      continue;
    }
    // Emit on the first active slice. Waiting for a complete emission period
    // caused short grip breaks to end without ever producing a visible puff.
    if (!emitter.active_sources[wheel]) {
      emitter.active_sources[wheel] = true;
      emitter.emission_accumulator[wheel] = emission_seconds;
    } else {
      emitter.emission_accumulator[wheel] += elapsed_seconds;
    }
    while (emitter.emission_accumulator[wheel] >= emission_seconds) {
      emitter.emission_accumulator[wheel] -= emission_seconds;
      auto position = contact.hit->point;
      for (std::size_t axis = 0U; axis < position.size(); ++axis) {
        position[axis] += contact.hit->normal[axis] * 0.08;
        position[axis] = static_cast<float>(position[axis]);
      }
      const auto seed =
          ++emitter.sequence + static_cast<std::uint32_t>(wheel * 0x9e3779b9U);
      const auto lateral = unit_noise(seed) * 2.0 - 1.0;
      const auto longitudinal = unit_noise(seed ^ 0xa511e9b3U) * 2.0 - 1.0;
      const auto lifetime = 0.82 + unit_noise(seed ^ 0x63d83595U) * 0.28;
      const auto size = 0.34 + unit_noise(seed ^ 0xc2b2ae35U) * 0.14;
      const auto phase =
          unit_noise(seed ^ 0x27d4eb2fU) * std::numbers::pi_v<double> * 2.0;
      emitter.particles.push_back(
          {position,
           {lateral * 0.18, 0.46 + unit_noise(seed ^ 0x165667b1U) * 0.16,
            longitudinal * 0.18},
           0.0,
           lifetime,
           size,
           phase});
      if (emitter.particles.size() > 160U) {
        emitter.particles.pop_front();
      }
    }
  }
}

void update_sparks(
    SparkEmitter &emitter,
    const mh::game::OriginalVehicleModeCSceneFrame &frame,
    const mh::game::OriginalVehicleGroundedMaterialTable &materials,
    const double elapsed_seconds) {
  const auto unit_noise = [](std::uint32_t value) {
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    value *= 0x846ca68bU;
    value ^= value >> 16U;
    return static_cast<double>(value & 0x00ffffffU) /
           static_cast<double>(0x01000000U);
  };
  emitter.emission_cooldown_seconds =
      std::max(0.0, emitter.emission_cooldown_seconds - elapsed_seconds);
  for (auto &particle : emitter.particles) {
    particle.age_seconds += elapsed_seconds;
    for (std::size_t axis = 0U; axis < particle.position.size(); ++axis) {
      particle.position[axis] += particle.velocity[axis] * elapsed_seconds;
    }
    particle.velocity[1U] -= 8.5 * elapsed_seconds;
  }
  while (!emitter.particles.empty() &&
         emitter.particles.front().age_seconds >=
             emitter.particles.front().lifetime_seconds) {
    emitter.particles.pop_front();
  }
  const auto &record = frame.scene.hull_audio_record;
  if (!record.active || record.material_index >= materials.size() ||
      materials[record.material_index].spark_animation_id == 0U ||
      !frame.scene.body_hull.has_value() ||
      record.hull_point_index >= frame.scene.body_hull->samples.size()) {
    emitter.contact_active = false;
    return;
  }
  const auto &sample = frame.scene.body_hull->samples[record.hull_point_index];
  if (!sample.hit.has_value()) {
    emitter.contact_active = false;
    return;
  }
  if (emitter.contact_active && emitter.emission_cooldown_seconds > 0.0) {
    return;
  }
  auto position = sample.hit->point;
  for (std::size_t axis = 0U; axis < position.size(); ++axis) {
    position[axis] += sample.hit->normal[axis] * 0.05;
  }
  constexpr std::size_t burst_size = 5U;
  for (std::size_t spark = 0U; spark < burst_size; ++spark) {
    const auto seed =
        ++emitter.sequence + static_cast<std::uint32_t>(spark * 0x9e3779b9U);
    const auto angle =
        unit_noise(seed ^ 0x85ebca6bU) * std::numbers::pi_v<double> * 2.0;
    const auto tangent_speed = 1.3 + unit_noise(seed ^ 0xc2b2ae35U) * 2.2;
    const auto normal_speed = 1.8 + unit_noise(seed ^ 0x27d4eb2fU) * 2.4;
    mh::game::CollisionVector3 velocity{
        std::cos(angle) * tangent_speed + sample.hit->normal[0U] * normal_speed,
        1.2 + unit_noise(seed ^ 0x165667b1U) * 2.8 +
            sample.hit->normal[1U] * normal_speed,
        std::sin(angle) * tangent_speed +
            sample.hit->normal[2U] * normal_speed};
    const auto lifetime = 0.22 + unit_noise(seed ^ 0xd3a2646cU) * 0.22;
    emitter.particles.push_back({position, velocity, 0.0, lifetime});
  }
  emitter.contact_active = true;
  emitter.emission_cooldown_seconds = 0.075;
  while (emitter.particles.size() > 80U) {
    emitter.particles.pop_front();
  }
}

void emit_world_triangle(SceneDepthResources &depth_resources,
                         const std::array<ViewTextureVertex, 3U> &source,
                         const std::vector<mh::content::PamRgbaImage> *images,
                         const bool trilinear_filtering,
                         const PerspectiveView &view, const int width,
                         const int height,
                         const WorldRenderEnvironment &environment) {
  std::array<SDL_Vertex, 4U> vertices{};
  std::array<double, 4U> view_depths{};
  auto outside_left = true;
  auto outside_right = true;
  auto outside_top = true;
  auto outside_bottom = true;
  constexpr float margin = 200.0F;
  for (std::size_t vertex = 0U; vertex < source.size(); ++vertex) {
    const auto point = project(view, source[vertex].point);
    vertices[vertex].position = {point.x, point.y};
    vertices[vertex].color = {
        std::clamp(source[vertex].color[0U] *
                       environment.accelerated_brightness[0U],
                   0.0F, 1.0F),
        std::clamp(source[vertex].color[1U] *
                       environment.accelerated_brightness[1U],
                   0.0F, 1.0F),
        std::clamp(source[vertex].color[2U] *
                       environment.accelerated_brightness[2U],
                   0.0F, 1.0F),
        1.0F};
    vertices[vertex].tex_coord = {source[vertex].u, source[vertex].v};
    view_depths[vertex] = source[vertex].point.z;
    outside_left = outside_left && point.x < -margin;
    outside_right =
        outside_right && point.x > static_cast<float>(width) + margin;
    outside_top = outside_top && point.y < -margin;
    outside_bottom =
        outside_bottom && point.y > static_cast<float>(height) + margin;
  }
  if (outside_left || outside_right || outside_top || outside_bottom) {
    return;
  }
  depth_resources.record_geometry(vertices, view_depths, 3U, images,
                                  trilinear_filtering);
}

void render_world(
    const WorldVisual &visual, const PerspectiveView &view, const int width,
    const int height, const bool enhanced, const bool trilinear_filtering,
    const double render_distance, const WorldRenderEnvironment &environment,
    SceneDepthResources &depth_resources, WorldRenderScratch &scratch,
    const std::optional<mh::game::CollisionVector3> &spatial_focus =
        std::nullopt,
    const std::optional<OverheadWorldCutaway> &cutaway = std::nullopt) {
  scratch.visible_objects.assign(visual.world.objects.size(), 0U);
  scratch.active_spatial_objects.assign(visual.world.objects.size(), 1U);
  auto &visible_objects = scratch.visible_objects;
  auto &active_spatial_objects = scratch.active_spatial_objects;
  if (visual.generated_spatial_cells && spatial_focus.has_value()) {
    std::fill(active_spatial_objects.begin(), active_spatial_objects.end(),
              false);
    const auto active_cell = mh::content::myw_grid_cell_index(
        visual.world, (*spatial_focus)[0U], (*spatial_focus)[2U]);
    if (active_cell.has_value()) {
      const auto active_column =
          static_cast<std::int32_t>(*active_cell % visual.world.grid_width);
      const auto active_row =
          static_cast<std::int32_t>(*active_cell / visual.world.grid_width);
      constexpr std::int32_t safety_ring = 2;
      for (auto row_offset = -safety_ring; row_offset <= safety_ring;
           ++row_offset) {
        for (auto column_offset = -safety_ring; column_offset <= safety_ring;
             ++column_offset) {
          const auto column = active_column + column_offset;
          const auto row = active_row + row_offset;
          if (column < 0 || row < 0 ||
              column >= static_cast<std::int32_t>(visual.world.grid_width) ||
              row >= static_cast<std::int32_t>(visual.world.grid_height)) {
            continue;
          }
          const auto dense_index =
              static_cast<std::size_t>(row) * visual.world.grid_width +
              static_cast<std::size_t>(column);
          const auto object_index =
              visual.world.grid_object_indices[dense_index];
          if (object_index < active_spatial_objects.size()) {
            active_spatial_objects[object_index] = true;
          }
        }
      }
    }
  }
  constexpr double screen_margin = 200.0;
  const auto inverse_focal_length = 1.0 / view.focal_length;
  const auto left_slope =
      (-screen_margin - view.center_x) * inverse_focal_length;
  const auto right_slope =
      (static_cast<double>(width) + screen_margin - view.center_x) *
      inverse_focal_length;
  const auto bottom_slope =
      (view.center_y - static_cast<double>(height) - screen_margin) *
      inverse_focal_length;
  const auto top_slope = (view.center_y + screen_margin) * inverse_focal_length;
  const auto left_scale = std::sqrt(1.0 + left_slope * left_slope);
  const auto right_scale = std::sqrt(1.0 + right_slope * right_slope);
  const auto bottom_scale = std::sqrt(1.0 + bottom_slope * bottom_slope);
  const auto top_scale = std::sqrt(1.0 + top_slope * top_slope);
  for (std::size_t index = 0U; index < visual.world.objects.size(); ++index) {
    const auto &sphere = visual.world.objects[index].bounding_sphere;
    const auto dx = static_cast<double>(sphere[0U]) - view.position[0U];
    const auto dy = static_cast<double>(sphere[1U]) - view.position[1U];
    const auto dz = static_cast<double>(sphere[2U]) - view.position[2U];
    const auto radius = std::max(0.0, static_cast<double>(sphere[3U]));
    // Retain one fully fogged guard band behind the nominal limit. Object
    // culling then happens invisibly inside opaque fog instead of exposing a
    // silhouette-shaped horizon cut.
    const auto limit = render_distance * 1.10 + radius;
    if (!active_spatial_objects[index] ||
        dx * dx + dy * dy + dz * dz > limit * limit) {
      continue;
    }
    const auto center = to_view(view, {sphere[0U], sphere[1U], sphere[2U]});
    visible_objects[index] =
        center.z + radius >= view.near_plane &&
        center.x - left_slope * center.z >= -radius * left_scale &&
        center.x - right_slope * center.z <= radius * right_scale &&
        center.y - bottom_slope * center.z >= -radius * bottom_scale &&
        center.y - top_slope * center.z <= radius * top_scale;
  }

  scratch.transformed_positions.assign(visual.world.positions.size(), 0U);
  scratch.view_positions.resize(visual.world.positions.size());
  for (std::size_t object_index = 0U;
       object_index < visual.world.objects.size(); ++object_index) {
    if (!visible_objects[object_index]) {
      continue;
    }
    const auto &object = visual.world.objects[object_index];
    const auto primitive_end =
        static_cast<std::size_t>(object.first_primitive) +
        static_cast<std::size_t>(object.primitive_count);
    for (auto primitive_index =
             static_cast<std::size_t>(object.first_primitive);
         primitive_index < primitive_end; ++primitive_index) {
      const auto &primitive = visual.world.primitives[primitive_index];
      if (primitive.vertex_count < 3U || primitive.vertex_count > 4U) {
        continue;
      }
      if (cutaway.has_value()) {
        double minimum_x = std::numeric_limits<double>::infinity();
        double maximum_x = -std::numeric_limits<double>::infinity();
        double minimum_y = std::numeric_limits<double>::infinity();
        double maximum_y = -std::numeric_limits<double>::infinity();
        double minimum_z = std::numeric_limits<double>::infinity();
        double maximum_z = -std::numeric_limits<double>::infinity();
        for (std::size_t vertex = 0U; vertex < primitive.vertex_count;
             ++vertex) {
          const auto &source =
              visual.world.positions[primitive.position_indices[vertex]];
          minimum_x = std::min(minimum_x, static_cast<double>(source[0U]));
          maximum_x = std::max(maximum_x, static_cast<double>(source[0U]));
          minimum_y = std::min(minimum_y, static_cast<double>(source[1U]));
          maximum_y = std::max(maximum_y, static_cast<double>(source[1U]));
          minimum_z = std::min(minimum_z, static_cast<double>(source[2U]));
          maximum_z = std::max(maximum_z, static_cast<double>(source[2U]));
        }
        // The cutaway is spatial rather than normal-based: beams, rocks, signs,
        // and curved tunnel shells can all obstruct an elevated driving view
        // even though none is a horizontal roof face.
        const auto elevated =
            minimum_y > cutaway->vehicle_position[1U] +
                            cutaway->minimum_height_above_vehicle &&
            maximum_y < view.position[1U] - 0.25;
        const auto center_x = (minimum_x + maximum_x) * 0.5;
        const auto center_z = (minimum_z + maximum_z) * 0.5;
        const auto dx = center_x - cutaway->vehicle_position[0U];
        const auto dz = center_z - cutaway->vehicle_position[2U];
        const auto forward_length = std::hypot(cutaway->vehicle_forward[0U],
                                               cutaway->vehicle_forward[2U]);
        const auto forward_x = cutaway->vehicle_forward[0U] / forward_length;
        const auto forward_z = cutaway->vehicle_forward[2U] / forward_length;
        const auto longitudinal = dx * forward_x + dz * forward_z;
        const auto lateral = dx * forward_z - dz * forward_x;
        const auto inside_corridor =
            std::abs(longitudinal) <= cutaway->longitudinal_radius &&
            std::abs(lateral) <= cutaway->lateral_half_width;
        if (elevated && inside_corridor) {
          continue;
        }
      }
      std::optional<std::size_t> material_index;
      if (primitive.has_texture_coordinates &&
          primitive.material_name_index < visual.material_indices.size()) {
        material_index = visual.material_indices[primitive.material_name_index];
      }
      const MaterialTexture *material = material_index.has_value()
                                            ? &visual.materials[*material_index]
                                            : nullptr;
      std::array<ViewTextureVertex, 6U> polygon{};
      std::size_t polygon_size = 0U;
      for (std::size_t vertex = 0U; vertex < primitive.vertex_count; ++vertex) {
        const auto position_index = primitive.position_indices[vertex];
        const auto &source = visual.world.positions[position_index];
        ViewTextureVertex item;
        if (!scratch.transformed_positions[position_index]) {
          scratch.view_positions[position_index] =
              to_view(view, {source[0U], source[1U], source[2U]});
          scratch.transformed_positions[position_index] = 1U;
        }
        item.point = scratch.view_positions[position_index];
        for (std::size_t channel = 0U; channel < item.color.size(); ++channel) {
          item.color[channel] =
              static_cast<float>(primitive.vertex_colors[vertex][channel]) /
              255.0F;
        }
        if (material != nullptr) {
          const auto authored_u = primitive.texture_coordinates[vertex][0U];
          item.u = authored_u * material->sampled_u_scale +
                   material->sampled_u_offset;
          item.v = primitive.texture_coordinates[vertex][1U] /
                   static_cast<float>(material->height);
        }
        polygon[polygon_size++] = item;
      }

      std::array<ViewTextureVertex, 6U> clipped{};
      std::size_t clipped_size = 0U;
      auto previous = polygon[polygon_size - 1U];
      auto previous_inside = previous.point.z >= view.near_plane;
      for (std::size_t index = 0U; index < polygon_size; ++index) {
        const auto &current = polygon[index];
        const auto current_inside = current.point.z >= view.near_plane;
        const auto intersection = [&view](const ViewTextureVertex &outside,
                                          const ViewTextureVertex &inside) {
          const auto denominator = inside.point.z - outside.point.z;
          const auto factor = (view.near_plane - outside.point.z) / denominator;
          ViewTextureVertex result;
          result.point = {
              outside.point.x + (inside.point.x - outside.point.x) * factor,
              outside.point.y + (inside.point.y - outside.point.y) * factor,
              view.near_plane};
          result.u =
              outside.u + (inside.u - outside.u) * static_cast<float>(factor);
          result.v =
              outside.v + (inside.v - outside.v) * static_cast<float>(factor);
          for (std::size_t channel = 0U; channel < result.color.size();
               ++channel) {
            result.color[channel] =
                outside.color[channel] +
                (inside.color[channel] - outside.color[channel]) *
                    static_cast<float>(factor);
          }
          return result;
        };
        if (current_inside) {
          if (!previous_inside) {
            clipped[clipped_size++] = intersection(previous, current);
          }
          clipped[clipped_size++] = current;
        } else if (previous_inside) {
          clipped[clipped_size++] = intersection(current, previous);
        }
        previous = current;
        previous_inside = current_inside;
      }
      if (clipped_size < 3U) {
        continue;
      }
      const auto *images =
          material_index.has_value()
              ? active_texture_images(visual.materials[*material_index],
                                      enhanced)
              : nullptr;
      for (std::size_t corner = 1U; corner + 1U < clipped_size; ++corner) {
        const std::array<ViewTextureVertex, 3U> source{
            clipped[0U], clipped[corner], clipped[corner + 1U]};
        emit_world_triangle(depth_resources, source, images,
                            trilinear_filtering, view, width, height,
                            environment);
      }
    }
  }
}

void render_vehicle_shadow(const CarVisual &visual,
                           const mh::game::OriginalBodyPoseState &vehicle,
                           const mh::game::CollisionWorld &collision_world,
                           const PerspectiveView &view,
                           const double render_distance,
                           const WorldRenderEnvironment &environment,
                           SceneDepthResources &depth_resources) {
  if (!visual.shadow_points.has_value()) {
    return;
  }
  const auto center = to_view(view, vehicle.world_position);
  if (center.z <= view.near_plane || center.z >= render_distance) {
    return;
  }

  // p3.1 VA 0x4ec024 casts this nine-cell table through each CAR's 4x4
  // ShadowPoints from -10 to +1 vertically. A depth marker preserves those
  // footprints without coplanar geometry crossing the camera or elevations.
  static constexpr std::array<std::array<std::size_t, 4U>, 9U> cells{{
      {5U, 4U, 0U, 1U},
      {6U, 5U, 1U, 2U},
      {6U, 2U, 3U, 7U},
      {5U, 9U, 8U, 4U},
      {5U, 6U, 10U, 9U},
      {10U, 6U, 7U, 11U},
      {9U, 13U, 12U, 8U},
      {9U, 10U, 14U, 13U},
      {10U, 11U, 15U, 14U},
  }};

  std::array<mh::game::CollisionVector3, 16U> body_points{};
  std::array<std::optional<mh::game::CollisionHit>, 16U> hits;
  for (std::size_t row = 0U; row < 4U; ++row) {
    for (std::size_t column = 0U; column < 4U; ++column) {
      const auto &source = (*visual.shadow_points)[row][column];
      const auto body_point = mh::game::project_body_point_to_world(
          vehicle, {source.x, source.y, source.z});
      const auto point_index = row * 4U + column;
      body_points[point_index] = body_point;
      auto lower = body_point;
      auto upper = body_point;
      lower[1U] -= 10.0;
      upper[1U] += 1.0;
      hits[point_index] = collision_world.original_segment_cast(lower, upper);
    }
  }

  VehicleShadowDepthProjection projection;
  projection.view = view;
  projection.render_distance = render_distance;
  // Depth-projection renderers consume an absolute view-space distance,
  // whereas WorldRenderEnvironment retains the common normalized fraction.
  projection.cue_start = render_distance * environment.cue_start;
  projection.cue_color = environment.cue_color;
  projection.cue_enabled = environment.cue_enabled;
  projection.minimum_world_x = std::numeric_limits<double>::infinity();
  projection.maximum_world_x = -std::numeric_limits<double>::infinity();
  projection.minimum_world_z = std::numeric_limits<double>::infinity();
  projection.maximum_world_z = -std::numeric_limits<double>::infinity();
  auto have_receiver = false;
  for (std::size_t cell_index = 0U; cell_index < cells.size(); ++cell_index) {
    const auto &indices = cells[cell_index];
    auto &receiver = projection.cells[cell_index];
    auto hit_count = 0U;
    auto minimum_receiver_y = std::numeric_limits<double>::infinity();
    auto maximum_receiver_y = -std::numeric_limits<double>::infinity();
    std::array<std::optional<double>, 4U> receiver_heights{};
    for (std::size_t corner = 0U; corner < indices.size(); ++corner) {
      const auto point_index = indices[corner];
      receiver.world_xz[corner] = {body_points[point_index][0U],
                                   body_points[point_index][2U]};
      projection.minimum_world_x =
          std::min(projection.minimum_world_x, body_points[point_index][0U]);
      projection.maximum_world_x =
          std::max(projection.maximum_world_x, body_points[point_index][0U]);
      projection.minimum_world_z =
          std::min(projection.minimum_world_z, body_points[point_index][2U]);
      projection.maximum_world_z =
          std::max(projection.maximum_world_z, body_points[point_index][2U]);
      if (hits[point_index].has_value()) {
        receiver_heights[corner] = hits[point_index]->point[1U];
        minimum_receiver_y =
            std::min(minimum_receiver_y, hits[point_index]->point[1U]);
        maximum_receiver_y =
            std::max(maximum_receiver_y, hits[point_index]->point[1U]);
        ++hit_count;
      }
    }
    if (hit_count == 0U) {
      continue;
    }
    for (std::size_t corner = 0U; corner < receiver.world_y.size(); ++corner) {
      // A partially supported authored cell retains the highest sampled
      // receiver for its missing corner, matching the old conservative plane
      // while preserving every real corner height that was previously lost.
      receiver.world_y[corner] =
          receiver_heights[corner].value_or(maximum_receiver_y);
    }
    // Collision and visible MYW receiver meshes can differ by a few source
    // quantisation steps. This allowance is vertical only; the exact authored
    // X/Z footprint remains unchanged and the visible depth pixel remains the
    // final authority, so it cannot create a bridge or screen-space wedge.
    constexpr double receiver_height_allowance = 0.35;
    receiver.minimum_receiver_y =
        minimum_receiver_y - receiver_height_allowance;
    receiver.maximum_receiver_y =
        maximum_receiver_y + receiver_height_allowance;
    receiver.valid = true;
    have_receiver = true;
  }
  if (!have_receiver) {
    return;
  }

  const auto center_screen = project(view, center);
  auto maximum_extent = 0.0;
  for (const auto &point : body_points) {
    maximum_extent = std::max(
        maximum_extent, std::hypot(point[0U] - vehicle.world_position[0U],
                                   point[2U] - vehicle.world_position[2U]));
  }
  const auto screen_radius =
      std::clamp(static_cast<double>(view.focal_length) *
                     (maximum_extent + 0.35) / center.z,
                 2.0,
                 static_cast<double>(std::max(depth_resources.width(),
                                              depth_resources.height())) *
                     0.75);
  constexpr int screen_margin = 2;
  projection.minimum_x =
      std::clamp(static_cast<int>(std::floor(center_screen.x - screen_radius)) -
                     screen_margin,
                 0, depth_resources.width() - 1);
  projection.maximum_x =
      std::clamp(static_cast<int>(std::ceil(center_screen.x + screen_radius)) +
                     screen_margin,
                 0, depth_resources.width() - 1);
  projection.minimum_y = std::clamp(
      static_cast<int>(std::floor(center_screen.y - screen_radius * 0.6)) -
          screen_margin,
      0, depth_resources.height() - 1);
  projection.maximum_y = std::clamp(
      static_cast<int>(std::ceil(center_screen.y + screen_radius * 1.4)) +
          screen_margin,
      0, depth_resources.height() - 1);
  if (projection.minimum_x > projection.maximum_x ||
      projection.minimum_y > projection.maximum_y) {
    return;
  }

  depth_resources.record_shadow(projection);
}

void render_track(SDL_Renderer *renderer,
                  const std::vector<mh::content::ColPolygon> &polygons,
                  const PerspectiveView &view, const int width,
                  const int height) {
  for (const auto &polygon : polygons) {
    if (polygon.vertices.size() < 2U) {
      continue;
    }
    const auto road = polygon.surface == 4U;
    require(SDL_SetRenderDrawColor(renderer, road ? 112U : 54U,
                                   road ? 142U : 72U, road ? 154U : 82U, 255U),
            "set track color");
    for (std::size_t index = 0U; index < polygon.vertices.size(); ++index) {
      const auto next = (index + 1U) % polygon.vertices.size();
      const mh::game::CollisionVector3 first_world{polygon.vertices[index][0U],
                                                   polygon.vertices[index][1U],
                                                   polygon.vertices[index][2U]};
      const mh::game::CollisionVector3 second_world{polygon.vertices[next][0U],
                                                    polygon.vertices[next][1U],
                                                    polygon.vertices[next][2U]};
      ProjectedPoint first;
      ProjectedPoint second;
      if (!project_line(view, first_world, second_world, first, second)) {
        continue;
      }
      constexpr float margin = 100.0F;
      const auto outside = (first.x < -margin && second.x < -margin) ||
                           (first.x > static_cast<float>(width) + margin &&
                            second.x > static_cast<float>(width) + margin) ||
                           (first.y < -margin && second.y < -margin) ||
                           (first.y > static_cast<float>(height) + margin &&
                            second.y > static_cast<float>(height) + margin);
      if (!outside) {
        require(SDL_RenderLine(renderer, first.x, first.y, second.x, second.y),
                "render track edge");
      }
    }
  }
}

void render_vehicle(const CarVisual &visual, const WorldVisual &world_visual,
                    const mh::game::OriginalBodyPoseState &vehicle,
                    const PerspectiveView &view, const bool enhanced,
                    const bool trilinear_filtering,
                    const double render_distance,
                    const WorldRenderEnvironment &environment,
                    const bool point_lighting_enabled,
                    SceneDepthResources &depth_resources,
                    VehicleRenderScratch &scratch,
                    const WheelVisualState &wheel_state = {},
                    const bool scene_blink_emissive = true,
                    const mh::ui::GraphicCarShading shading =
                        mh::ui::GraphicCarShading::reflection) {
  const mh::game::CollisionVector3 local_center{
      (visual.body_minimum[0U] + visual.body_maximum[0U]) * 0.5,
      (visual.body_minimum[1U] + visual.body_maximum[1U]) * 0.5,
      (visual.body_minimum[2U] + visual.body_maximum[2U]) * 0.5};
  auto local_radius_squared = 0.0;
  for (std::size_t axis = 0U; axis < local_center.size(); ++axis) {
    const auto extent =
        (visual.body_maximum[axis] - visual.body_minimum[axis]) * 0.5;
    local_radius_squared += extent * extent;
  }
  auto maximum_basis_scale = 0.0;
  for (const auto &basis : vehicle.body_basis) {
    maximum_basis_scale = std::max(
        maximum_basis_scale,
        std::sqrt(basis[0U] * basis[0U] + basis[1U] * basis[1U] +
                  basis[2U] * basis[2U]));
  }
  const auto radius =
      (std::sqrt(local_radius_squared) + 0.75) * maximum_basis_scale;
  const auto center = to_view(
      view, mh::game::project_body_point_to_world(vehicle, local_center));
  if (center.z + radius <= view.near_plane ||
      center.z - radius >= render_distance) {
    return;
  }
  if (center.z > view.near_plane) {
    const auto screen = project(view, center);
    const auto screen_radius =
        static_cast<double>(view.focal_length) * radius /
        std::max(view.near_plane, center.z - radius);
    constexpr double margin = 24.0;
    if (screen.x + screen_radius < -margin ||
        screen.x - screen_radius > depth_resources.width() + margin ||
        screen.y + screen_radius < -margin ||
        screen.y - screen_radius > depth_resources.height() + margin) {
      return;
    }
  }
  // p3.1's object-light initializer (RVA 0x0004c1e8, called from
  // 0x00050ad0) selects the MYW grid cell and precomputes every light's
  // distance, direction and falloff once from the object's body origin.  The
  // smooth-face renderer then reuses those values for all of the object's
  // vertex normals.  Using each vertex position here incorrectly changes the
  // selected cell and light response across a single car.
  const std::array<float, 3U> object_lighting_position{
      static_cast<float>(vehicle.world_position[0U]),
      static_cast<float>(vehicle.world_position[1U]),
      static_cast<float>(vehicle.world_position[2U])};
  const std::array<float, 3U> object_view_direction{
      static_cast<float>(view.position[0U] - vehicle.world_position[0U]),
      static_cast<float>(view.position[1U] - vehicle.world_position[1U]),
      static_cast<float>(view.position[2U] - vehicle.world_position[2U])};
  const auto normalize_float3 = [](const std::array<float, 3U> &value) {
    const auto length_squared = value[0U] * value[0U] +
                                value[1U] * value[1U] +
                                value[2U] * value[2U];
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-12F) {
      return std::array<float, 3U>{};
    }
    const auto reciprocal = 1.0F / std::sqrt(length_squared);
    return std::array<float, 3U>{value[0U] * reciprocal,
                                value[1U] * reciprocal,
                                value[2U] * reciprocal};
  };
  const auto unit_view_direction = normalize_float3(object_view_direction);
  auto &point_lights = scratch.point_lights;
  point_lights.clear();
  if (point_lighting_enabled) {
    const auto cell = mh::content::myw_grid_cell_index(
        world_visual.world, object_lighting_position[0U],
        object_lighting_position[2U]);
    if (cell.has_value() && *cell < world_visual.world.grid_cells.size()) {
      constexpr float minimum_range = 0.1F;
      constexpr float maximum_positive_intensity = 0.4F;
      constexpr float negative_scale = 0.25F;
      for (const auto light_index :
           world_visual.world.grid_cells[*cell].light_indices) {
        if (light_index >= world_visual.world.lights.size()) {
          continue;
        }
        const auto &light = world_visual.world.lights[light_index];
        if (light.range <= minimum_range) {
          continue;
        }
        const std::array<float, 3U> offset{
            light.position[0U] - object_lighting_position[0U],
            light.position[1U] - object_lighting_position[1U],
            light.position[2U] - object_lighting_position[2U]};
        const auto distance_squared =
            offset[0U] * offset[0U] + offset[1U] * offset[1U] +
            offset[2U] * offset[2U];
        if (distance_squared >= light.range * light.range ||
            distance_squared <= 1.0e-12F) {
          continue;
        }
        const auto distance = std::sqrt(distance_squared);
        const auto falloff = 1.0F - distance / light.range;
        auto intensity = light.color[3U] * falloff * falloff *
                         environment.light_intensity;
        intensity = std::min(intensity, maximum_positive_intensity);
        if (intensity < 0.0F) {
          intensity *= negative_scale;
        }
        point_lights.push_back(
            {{{offset[0U] / distance, offset[1U] / distance,
               offset[2U] / distance}},
             {{std::clamp(light.color[0U], 0.0F, 1.0F),
               std::clamp(light.color[1U], 0.0F, 1.0F),
               std::clamp(light.color[2U], 0.0F, 1.0F)}},
             intensity, light.color[3U] > 0.0F});
      }
    }
  }
  auto &faces = scratch.faces;
  faces.clear();
  if (faces.capacity() < static_cast<std::size_t>(visual.face_count)) {
    faces.reserve(static_cast<std::size_t>(visual.face_count));
  }
  for (const auto &component : visual.components) {
    const auto blink_emissive =
        component.scene_blink_emissive && scene_blink_emissive;

    // MYO faces index shared position and normal tables.  Transforming and
    // lighting those tables once per component is equivalent to doing the
    // same arithmetic again for every face corner, but avoids multiplying the
    // most expensive part of vehicle presentation by the mesh's index reuse.
    // Recorded races make that former cost especially visible because every
    // MDE stream submits a complete car in the same frame.
    auto component_translation = component.translation;
    const auto wheel_index = component.wheel_index;
    auto spin_cos = 1.0;
    auto spin_sin = 0.0;
    auto steer_cos = 1.0;
    auto steer_sin = 0.0;
    if (wheel_index.has_value()) {
      const auto wheel = *wheel_index;
      spin_cos = std::cos(wheel_state.spin_radians[wheel]);
      spin_sin = std::sin(wheel_state.spin_radians[wheel]);
      if (wheel < 2U) {
        steer_cos = std::cos(wheel_state.steering);
        steer_sin = std::sin(wheel_state.steering);
      }
      component_translation = mh::game::original_wheel_visual_translation(
          component.translation, component.suspension_axis,
          wheel_state.suspension_states[wheel]);
    }

    auto &component_view_positions = scratch.view_positions;
    component_view_positions.clear();
    if (component_view_positions.capacity() < component.model.positions.size()) {
      component_view_positions.reserve(component.model.positions.size());
    }
    for (const auto &source : component.model.positions) {
      mh::game::CollisionVector3 local{source[0U], source[1U], source[2U]};
      if (wheel_index.has_value()) {
        const auto wheel = *wheel_index;
        const auto spun_y = local[1U] * spin_cos - local[2U] * spin_sin;
        const auto spun_z = local[1U] * spin_sin + local[2U] * spin_cos;
        local[1U] = spun_y;
        local[2U] = spun_z;
        if (wheel < 2U) {
          const auto steered_x = local[0U] * steer_cos + local[2U] * steer_sin;
          const auto steered_z = -local[0U] * steer_sin + local[2U] * steer_cos;
          local[0U] = steered_x;
          local[2U] = steered_z;
        }
      }
      for (std::size_t axis = 0U; axis < local.size(); ++axis) {
        local[axis] += component_translation[axis];
      }
      component_view_positions.push_back(
          to_view(view, mh::game::project_body_point_to_world(vehicle, local)));
    }

    auto &component_normals = scratch.normals;
    component_normals.clear();
    if (component_normals.capacity() < component.model.normals.size()) {
      component_normals.reserve(component.model.normals.size());
    }
    for (const auto &source_normal : component.model.normals) {
      // Dominator's original ASCIII body LODs contain explicit 0xffc00000
      // normal sentinels. Retain the established world-up presentation
      // fallback rather than passing them into the strict body transform.
      auto world_normal = mh::game::CollisionVector3{0.0, 1.0, 0.0};
      if (std::isfinite(source_normal[0U]) &&
          std::isfinite(source_normal[1U]) &&
          std::isfinite(source_normal[2U])) {
        world_normal = mh::game::project_body_vector_to_world(
            vehicle.body_basis,
            {source_normal[0U], source_normal[1U], source_normal[2U]});
      }
      PreparedVehicleNormal prepared;
      prepared.environment_coordinate =
          vehicle_environment_coordinate(world_normal, view);
      if (point_lighting_enabled) {
        const auto unit_normal = normalize_float3(
            {static_cast<float>(world_normal[0U]),
             static_cast<float>(world_normal[1U]),
             static_cast<float>(world_normal[2U])});
        for (const auto &light : point_lights) {
          const auto diffuse =
              unit_normal[0U] * light.direction[0U] +
              unit_normal[1U] * light.direction[1U] +
              unit_normal[2U] * light.direction[2U];
          if (diffuse <= 0.0F) {
            continue;
          }
          auto response = diffuse;
          if (light.specular) {
            const std::array<float, 3U> reflection{
                unit_normal[0U] * (2.0F * diffuse) - light.direction[0U],
                unit_normal[1U] * (2.0F * diffuse) - light.direction[1U],
                unit_normal[2U] * (2.0F * diffuse) - light.direction[2U]};
            const auto alignment =
                reflection[0U] * unit_view_direction[0U] +
                reflection[1U] * unit_view_direction[1U] +
                reflection[2U] * unit_view_direction[2U];
            if (alignment > 0.5F) {
              response += alignment * alignment * alignment *
                          environment.specular_factor;
            }
          }
          const auto intensity = light.intensity * response;
          for (std::size_t channel = 0U;
               channel < prepared.light_contribution.size(); ++channel) {
            prepared.light_contribution[channel] +=
                light.color[channel] * intensity;
          }
        }
      }
      const auto directional =
          mh::content::evaluate_original_myw_directional_lighting(
              world_visual.world, {static_cast<float>(world_normal[0U]),
                                   static_cast<float>(world_normal[1U]),
                                   static_cast<float>(world_normal[2U])});
      for (std::size_t channel = 0U; channel < directional.size(); ++channel) {
        prepared.light_contribution[channel] += directional[channel];
      }
      component_normals.push_back(prepared);
    }

    for (const auto &face : component.model.faces) {
      ProjectedModelFace projected_face;
      const auto base_color = mh::content::select_car_face_base_color(
          face, visual.default_colors, visual.active_colors,
          !component.wheel_index.has_value());
      projected_face.color = base_color.rgb;
      projected_face.apply_object_lighting = base_color.apply_object_lighting;
      if (!component.wheel_index.has_value() && face.has_texture_coordinates &&
          std::find(visual.emissive_material_indices.begin(),
                    visual.emissive_material_indices.end(),
                    std::optional<std::size_t>{face.material_name_index}) !=
              visual.emissive_material_indices.end()) {
        // The two ordered CAR Halo records are the body lamp groups.  Retail
        // keeps their base texture emissive while adding the associated halo
        // sprite, so neither the MYW ambient nor its directional light may
        // darken them.
        projected_face.apply_object_lighting = false;
      }
      projected_face.vertex_count = face.vertex_count;
      projected_face.environment_map = vehicle_environment_map(face);
      const auto authored_environment_surface =
          projected_face.environment_map != VehicleEnvironmentMap::none;
      const auto glenz_environment_layer =
          shading == mh::ui::GraphicCarShading::glenz &&
          !component.wheel_index.has_value() && authored_environment_surface;
      if (shading == mh::ui::GraphicCarShading::flat ||
          shading == mh::ui::GraphicCarShading::gouraud) {
        // 3daccbrightness compensates for the dark authored reflection maps.
        // Reusing it after Flat/Gouraud deliberately remove those maps clips
        // the paint channels and turns the body into a neon cyan surface.
        // Keep the authored ambient, point and directional lighting, but do
        // not apply that reflection compensation to the bare body colour.
        if (!component.wheel_index.has_value() &&
            authored_environment_surface) {
          projected_face.apply_accelerated_brightness = false;
        }
        projected_face.environment_map = VehicleEnvironmentMap::none;
      }
      if (face.has_texture_coordinates &&
          face.material_name_index < component.material_indices.size()) {
        projected_face.material_index =
            component.material_indices[face.material_name_index];
      }
      const auto *material =
          projected_face.material_index.has_value()
              ? &world_visual.materials[*projected_face.material_index]
              : nullptr;
      if (face.vertex_count < 3U || face.vertex_count > 4U) {
        continue;
      }
      std::array<ViewTextureVertex, 6U> polygon{};
      std::size_t polygon_size = 0U;
      for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
        const auto normal_index = shading != mh::ui::GraphicCarShading::flat &&
                                          face.has_vertex_normals
                                      ? face.normal_indices[vertex]
                                      : face.normal_index;
        if (normal_index >= component_normals.size()) {
          throw std::runtime_error(
              "MYO vehicle vertex normal is outside its parsed table");
        }
        const auto &prepared_normal = component_normals[normal_index];
        if (projected_face.environment_map != VehicleEnvironmentMap::none) {
          projected_face.environment_coordinates[vertex] =
              prepared_normal.environment_coordinate;
        }
        projected_face.light_contributions[vertex] =
            prepared_normal.light_contribution;
        ViewTextureVertex item;
        const auto position_index = face.position_indices[vertex];
        if (position_index >= component_view_positions.size()) {
          throw std::runtime_error(
              "MYO vehicle vertex position is outside its parsed table");
        }
        item.point = component_view_positions[position_index];
        item.light_contribution = projected_face.light_contributions[vertex];
        if (projected_face.environment_map != VehicleEnvironmentMap::none) {
          item.u = projected_face.environment_coordinates[vertex].x;
          item.v = projected_face.environment_coordinates[vertex].y;
        } else if (material != nullptr) {
          item.u = face.texture_coordinates[vertex][0U] /
                   static_cast<float>(material->width);
          item.v = face.texture_coordinates[vertex][1U] /
                   static_cast<float>(material->height);
        }
        polygon[polygon_size++] = item;
      }

      // p3.1 submits clipped car polygons to the same depth-owned scene as
      // the world. Dropping an entire MYO face when only one vertex crosses
      // the near plane exposes internal car geometry and makes overlapping
      // vehicles appear to clip through one another.
      std::array<ViewTextureVertex, 6U> clipped{};
      std::size_t clipped_size = 0U;
      auto previous = polygon[polygon_size - 1U];
      auto previous_inside = previous.point.z >= view.near_plane;
      for (std::size_t index = 0U; index < polygon_size; ++index) {
        const auto &current = polygon[index];
        const auto current_inside = current.point.z >= view.near_plane;
        const auto intersection = [&view](const ViewTextureVertex &outside,
                                          const ViewTextureVertex &inside) {
          const auto denominator = inside.point.z - outside.point.z;
          const auto factor = (view.near_plane - outside.point.z) / denominator;
          ViewTextureVertex result;
          result.point = {
              outside.point.x + (inside.point.x - outside.point.x) * factor,
              outside.point.y + (inside.point.y - outside.point.y) * factor,
              view.near_plane};
          result.u =
              outside.u + (inside.u - outside.u) * static_cast<float>(factor);
          result.v =
              outside.v + (inside.v - outside.v) * static_cast<float>(factor);
          for (std::size_t channel = 0U;
               channel < result.light_contribution.size(); ++channel) {
            result.light_contribution[channel] =
                outside.light_contribution[channel] +
                (inside.light_contribution[channel] -
                 outside.light_contribution[channel]) *
                    static_cast<float>(factor);
          }
          return result;
        };
        if (current_inside) {
          if (!previous_inside) {
            clipped[clipped_size++] = intersection(previous, current);
          }
          clipped[clipped_size++] = current;
        } else if (previous_inside) {
          clipped[clipped_size++] = intersection(current, previous);
        }
        previous = current;
        previous_inside = current_inside;
      }
      if (clipped_size < 3U) {
        continue;
      }
      for (std::size_t corner = 1U; corner + 1U < clipped_size; ++corner) {
        auto triangle = projected_face;
        triangle.vertex_count = 3U;
        triangle.depth = 0.0;
        const std::array<ViewTextureVertex, 3U> source{
            clipped[0U], clipped[corner], clipped[corner + 1U]};
        for (std::size_t vertex = 0U; vertex < source.size(); ++vertex) {
          triangle.vertices[vertex] = project(view, source[vertex].point);
          triangle.view_depths[vertex] = source[vertex].point.z;
          triangle.texture_coordinates[vertex] = {source[vertex].u,
                                                  source[vertex].v};
          triangle.environment_coordinates[vertex] = {source[vertex].u,
                                                      source[vertex].v};
          triangle.light_contributions[vertex] =
              source[vertex].light_contribution;
          triangle.depth += source[vertex].point.z;
        }
        triangle.depth /= 3.0;
        if (glenz_environment_layer) {
          // Keep the reflection-mapped paint opaque. A neutral Glenz coat adds
          // authored highlights without the bright cyan result of an
          // untextured paint pass or exposing internal meshes.
          auto paint = triangle;
          paint.blend_mode = SceneBlendMode::opaque;
          paint.opacity = 1.0F;
          faces.push_back(paint);

          triangle.color = {255U, 255U, 255U};
          triangle.apply_object_lighting = false;
          triangle.blend_mode = SceneBlendMode::additive;
          triangle.opacity = 0.14F;
          triangle.depth *= 0.9999;
          for (std::size_t vertex = 0U; vertex < triangle.vertex_count;
               ++vertex) {
            triangle.view_depths[vertex] *= 0.9999;
          }
        }
        faces.push_back(triangle);
        if (blink_emissive) {
          // Keep the physical lamp in both states; add its authored blink
          // texture for emission without making the black field opaque.
          auto emission = triangle;
          emission.apply_object_lighting = false;
          emission.blend_mode = SceneBlendMode::additive;
          emission.depth *= 0.9999;
          for (std::size_t vertex = 0U; vertex < emission.vertex_count;
               ++vertex) {
            emission.view_depths[vertex] *= 0.9999;
          }
          faces.push_back(emission);
        }
      }
    }
  }
  auto &face_order = scratch.face_order;
  face_order.clear();
  if (face_order.capacity() < faces.size()) {
    face_order.reserve(faces.size());
  }
  for (const auto &face : faces) {
    if (face.blend_mode == SceneBlendMode::opaque) {
      face_order.push_back(&face);
    }
  }
  const auto translucent_begin = face_order.size();
  for (const auto &face : faces) {
    if (face.blend_mode != SceneBlendMode::opaque) {
      face_order.push_back(&face);
    }
  }
  std::sort(face_order.begin() +
                static_cast<std::ptrdiff_t>(translucent_begin),
            face_order.end(),
            [](const auto *left, const auto *right) {
              return left->depth > right->depth;
            });
  for (const auto *face_pointer : face_order) {
    const auto &face = *face_pointer;
    std::array<SDL_Vertex, 4U> vertices{};
    auto red = static_cast<float>(face.color[0U]) / 255.0F;
    auto green = static_cast<float>(face.color[1U]) / 255.0F;
    auto blue = static_cast<float>(face.color[2U]) / 255.0F;
    for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
      vertices[vertex].position = {face.vertices[vertex].x,
                                   face.vertices[vertex].y};
      if (!face.apply_object_lighting) {
        vertices[vertex].color = {red, green, blue, face.opacity};
      } else {
        const auto accelerated = face.apply_accelerated_brightness
                                     ? environment.accelerated_brightness
                                     : std::array<float, 3U>{1.0F, 1.0F, 1.0F};
        vertices[vertex].color = {
            std::clamp(red *
                           (environment.object_ambient[0U] +
                            face.light_contributions[vertex][0U]) *
                           environment.object_brightness[0U] * accelerated[0U],
                       0.0F, 1.0F),
            std::clamp(green *
                           (environment.object_ambient[1U] +
                            face.light_contributions[vertex][1U]) *
                           environment.object_brightness[1U] * accelerated[1U],
                       0.0F, 1.0F),
            std::clamp(blue *
                           (environment.object_ambient[2U] +
                            face.light_contributions[vertex][2U]) *
                           environment.object_brightness[2U] * accelerated[2U],
                       0.0F, 1.0F),
            face.opacity};
      }
      vertices[vertex].tex_coord =
          face.environment_map == VehicleEnvironmentMap::none
              ? face.texture_coordinates[vertex]
              : face.environment_coordinates[vertex];
    }
    const auto *images =
        face.environment_map == VehicleEnvironmentMap::none
            ? (face.material_index.has_value()
                   ? active_texture_images(
                         world_visual.materials[*face.material_index], enhanced)
                   : nullptr)
            : vehicle_environment_images(world_visual.vehicle_environment,
                                         face.environment_map);
    depth_resources.record_geometry(vertices, face.view_depths,
                                    face.vertex_count, images,
                                    trilinear_filtering, face.blend_mode);
  }
}

void render_vehicle_halos(const CarVisual &visual,
                          const mh::game::OriginalBodyPoseState &vehicle,
                          const PerspectiveView &view,
                          const double render_distance,
                          const WorldRenderEnvironment &,
                          SceneDepthResources &depth_resources,
                          const bool lens_flares_enabled,
                          const bool rear_lamps_bright) {
  constexpr std::array<SDL_FPoint, 4U> texture_coordinates{
      {{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}}};
  const auto forward_point = mh::game::project_body_point_to_world(
      vehicle, mh::game::CollisionVector3{0.0, 0.0, 1.0});
  mh::game::CollisionVector3 forward{};
  mh::game::CollisionVector3 camera_direction{};
  auto camera_distance_squared = 0.0;
  for (std::size_t axis = 0U; axis < forward.size(); ++axis) {
    forward[axis] = forward_point[axis] - vehicle.world_position[axis];
    camera_direction[axis] = view.position[axis] - vehicle.world_position[axis];
    camera_distance_squared += camera_direction[axis] * camera_direction[axis];
  }
  if (camera_distance_squared <= 0.0) {
    return;
  }
  auto front_facing = 0.0;
  for (std::size_t axis = 0U; axis < forward.size(); ++axis) {
    front_facing += forward[axis] * camera_direction[axis];
  }
  front_facing /= std::sqrt(camera_distance_squared);
  for (std::size_t side = 0U; side < visual.halo_attachments.size(); ++side) {
    if (visual.halo_images[side].empty()) {
      continue;
    }
    // The first CAR halo belongs to front-facing lamp geometry and the
    // second to rear-facing geometry.  Retail does not show either billboard
    // through the opposite end of the body; its view-facing response also
    // removes the abrupt side-on pop.
    const auto facing =
        std::clamp(side == 0U ? front_facing : -front_facing, 0.0, 1.0);
    if (facing <= 0.0) {
      continue;
    }
    for (const auto &attachment : visual.halo_attachments[side]) {
      const auto world =
          mh::game::project_body_point_to_world(vehicle, attachment.center);
      const auto center = to_view(view, world);
      if (center.z < view.near_plane || center.z > render_distance) {
        continue;
      }
      const auto layer_count = lens_flares_enabled ? 2U : 1U;
      for (std::size_t layer = 0U; layer < layer_count; ++layer) {
        const auto half_size = layer == 0U ? 0.43 : 0.92;
        const auto layer_alpha =
            static_cast<float>(facing * (layer == 0U ? 1.0 : 0.28));
        // The complete lamp glow belongs to the authored lamp face, not to
        // the camera. Carry that face's normal through the body transform so
        // both its core and softer outer layer follow the car and retain the
        // correct left/right angle while turning.
        const auto world_normal =
            normalized(mh::game::project_body_vector_to_world(
                vehicle.body_basis, attachment.face_normal));
        auto face_up = mh::game::project_body_vector_to_world(
            vehicle.body_basis, {0.0, 1.0, 0.0});
        auto face_right = cross(face_up, world_normal);
        if (dot(face_right, face_right) <= 1.0e-12) {
          face_right = mh::game::project_body_vector_to_world(
              vehicle.body_basis, {1.0, 0.0, 0.0});
        }
        face_right = normalized(face_right);
        face_up = normalized(cross(world_normal, face_right));
        const auto plane_offset = layer == 0U ? 0.012 : 0.018;
        const auto face_corner = [&](const double right_scale,
                                     const double up_scale) {
          mh::game::CollisionVector3 point{};
          for (std::size_t axis = 0U; axis < point.size(); ++axis) {
            point[axis] = world[axis] + world_normal[axis] * plane_offset +
                          face_right[axis] * half_size * right_scale +
                          face_up[axis] * half_size * up_scale;
          }
          return to_view(view, point);
        };
        const std::array<ViewPoint, 4U> corners{
            {face_corner(-1.0, 1.0), face_corner(1.0, 1.0),
             face_corner(1.0, -1.0), face_corner(-1.0, -1.0)}};
        std::array<SDL_Vertex, 4U> vertices{};
        std::array<double, 4U> view_depths{};
        for (std::size_t corner = 0U; corner < corners.size(); ++corner) {
          const auto projected = project(view, corners[corner]);
          vertices[corner].position = {projected.x, projected.y};
          vertices[corner].color = {1.0F, 1.0F, 1.0F, layer_alpha};
          vertices[corner].tex_coord = texture_coordinates[corner];
          view_depths[corner] = corners[corner].z;
        }
        depth_resources.record_geometry(vertices, view_depths, 4U,
                                        &visual.halo_images[side], false,
                                        SceneBlendMode::additive);
        if (side == 1U && rear_lamps_bright) {
          // The normal rear halo remains the running light. Brake and
          // handbrake add the same authored red image once more, matching the
          // brighter additive response without replacing or recolouring it.
          depth_resources.record_geometry(vertices, view_depths, 4U,
                                          &visual.halo_images[side], false,
                                          SceneBlendMode::additive);
        }
      }
    }
  }
}

void render_world_halos(const WorldVisual &visual, const PerspectiveView &view,
                        const double render_distance,
                        const WorldRenderEnvironment &,
                        SceneDepthResources &depth_resources) {
  constexpr std::array<SDL_FPoint, 4U> texture_coordinates{
      {{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}}};
  for (const auto &attachment : visual.halo_attachments) {
    if (attachment.image_index >= visual.halo_images.size()) {
      continue;
    }
    const auto center = to_view(view, attachment.center);
    if (center.z < view.near_plane || center.z > render_distance) {
      continue;
    }
    if (std::abs(center.x) > center.z * 1.6 ||
        std::abs(center.y) > center.z * 1.3) {
      continue;
    }
    // MYW lens flares are camera-facing optical sprites. Their second authored
    // position supplies the world-space radius; treating the record as a world
    // face turns lamps edge-on and suppresses their hot core from side views.
    const std::array<ViewPoint, 4U> corners{{
        {center.x - attachment.half_size, center.y - attachment.half_size,
         center.z},
        {center.x + attachment.half_size, center.y - attachment.half_size,
         center.z},
        {center.x + attachment.half_size, center.y + attachment.half_size,
         center.z},
        {center.x - attachment.half_size, center.y + attachment.half_size,
         center.z},
    }};
    std::array<SDL_Vertex, 4U> vertices{};
    std::array<double, 4U> view_depths{};
    const auto depth_bias = std::min(attachment.half_size * 0.18, 0.28);
    for (std::size_t corner = 0U; corner < corners.size(); ++corner) {
      const auto projected = project(view, corners[corner]);
      vertices[corner].position = {projected.x, projected.y};
      vertices[corner].color = {1.0F, 1.0F, 1.0F, 1.0F};
      vertices[corner].tex_coord = texture_coordinates[corner];
      // Pull only the depth test slightly toward the camera so the tiny lamp
      // fixture cannot cut the additive starburst in half. The bounded offset
      // still lets nearby walls and track geometry occlude the flare.
      view_depths[corner] = std::max(view.near_plane, center.z - depth_bias);
    }
    depth_resources.record_geometry(vertices, view_depths, 4U,
                                    &visual.halo_images[attachment.image_index],
                                    false, SceneBlendMode::additive);
  }
}

void render_world_lamp_beams(const WorldVisual &visual,
                             const PerspectiveView &view,
                             const double render_distance,
                             const WorldRenderEnvironment &,
                             SceneDepthResources &depth_resources) {
  constexpr std::array<SDL_FPoint, 4U> texture_coordinates{
      {{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 1.0F}, {0.0F, 1.0F}}};
  if (visual.lamp_beam_images.empty()) {
    return;
  }
  for (const auto &beam : visual.lamp_beams) {
    auto camera_alpha = beam.alpha;
    const auto beam_axis = subtract(beam.receiver, beam.source);
    const auto beam_length_squared = dot(beam_axis, beam_axis);
    if (beam_length_squared > 1.0e-8) {
      const auto source_to_camera = subtract(view.position, beam.source);
      const auto raw_fraction =
          dot(source_to_camera, beam_axis) / beam_length_squared;
      if (raw_fraction > 0.0 && raw_fraction < 1.0) {
        const auto fraction = std::clamp(raw_fraction, 0.0, 1.0);
        mh::game::CollisionVector3 closest{};
        for (std::size_t axis = 0U; axis < closest.size(); ++axis) {
          closest[axis] = beam.source[axis] + beam_axis[axis] * fraction;
        }
        const auto camera_offset = subtract(view.position, closest);
        const auto distance_from_axis =
            std::sqrt(dot(camera_offset, camera_offset));
        const auto local_half_width =
            beam.source_half_width +
            (beam.receiver_half_width - beam.source_half_width) * fraction;
        const auto normalized_distance =
            distance_from_axis / std::max(local_half_width, 1.0e-6);
        // A single translucent beam card is convincing from outside, but can
        // cover most of the screen when the chase camera enters its volume.
        // Fade only that camera's interior view from 6% at the cone core to
        // full strength just outside its edge. This preserves the visible
        // beam and lamp for every external viewpoint without a hard pop.
        const auto fade_position =
            std::clamp((normalized_distance - 0.34) / (1.08 - 0.34), 0.0, 1.0);
        const auto smooth_fade =
            fade_position * fade_position * (3.0 - 2.0 * fade_position);
        camera_alpha *= static_cast<float>(0.06 + 0.94 * smooth_fade);
      }
    }
    const auto source_view = to_view(view, beam.source);
    const auto receiver_view = to_view(view, beam.receiver);
    if (source_view.z < view.near_plane || receiver_view.z < view.near_plane ||
        source_view.z > render_distance || receiver_view.z > render_distance) {
      continue;
    }
    const mh::game::CollisionVector3 source_to_camera{
        view.position[0U] - beam.source[0U], 0.0,
        view.position[2U] - beam.source[2U]};
    const auto receiver_right =
        normalized_or(mh::game::CollisionVector3{-source_to_camera[2U], 0.0,
                                                 source_to_camera[0U]},
                      {1.0, 0.0, 0.0});
    const auto corner = [&](const mh::game::CollisionVector3 &center,
                            const mh::game::CollisionVector3 &edge_direction,
                            const double half_width, const double side) {
      return to_view(view,
                     mh::game::CollisionVector3{
                         center[0U] + edge_direction[0U] * half_width * side,
                         center[1U] + edge_direction[1U] * half_width * side,
                         center[2U] + edge_direction[2U] * half_width * side});
    };
    const std::array<ViewPoint, 4U> corners{
        {corner(beam.source, beam.source_edge_direction, beam.source_half_width,
                -1.0),
         corner(beam.source, beam.source_edge_direction, beam.source_half_width,
                1.0),
         corner(beam.receiver, receiver_right, beam.receiver_half_width, 1.0),
         corner(beam.receiver, receiver_right, beam.receiver_half_width,
                -1.0)}};
    // A wide receiver edge can cross behind the near plane while both beam
    // centres are still in front of it (notably beside NeoCity's start-area
    // lamps). Such a quad cannot be projected or passed to the retail cue
    // equation as-is. Suppress it for only those few camera positions; once
    // all four corners are valid the same authored beam becomes visible.
    if (std::any_of(corners.begin(), corners.end(), [&](const auto &point) {
          return !std::isfinite(point.x) || !std::isfinite(point.y) ||
                 !std::isfinite(point.z) || point.z < view.near_plane ||
                 point.z > render_distance;
        })) {
      continue;
    }
    std::array<SDL_Vertex, 4U> vertices{};
    std::array<double, 4U> view_depths{};
    for (std::size_t index = 0U; index < corners.size(); ++index) {
      const auto projected = project(view, corners[index]);
      vertices[index].position = {projected.x, projected.y};
      vertices[index].color = {beam.color[0U], beam.color[1U], beam.color[2U],
                               camera_alpha};
      vertices[index].tex_coord = texture_coordinates[index];
      view_depths[index] = corners[index].z;
    }
    depth_resources.record_geometry(vertices, view_depths, 4U,
                                    &visual.lamp_beam_images, false,
                                    SceneBlendMode::additive);
  }
}

void render_vehicle_headlight_projection(
    const CarVisual &visual, const mh::game::OriginalBodyPoseState &vehicle,
    const PerspectiveView &view, const double render_distance,
    const WorldRenderEnvironment &environment,
    SceneDepthResources &depth_resources) {
  if (visual.headlight_sources.size() < 2U) {
    return;
  }
  const auto camera_dx = vehicle.world_position[0U] - view.position[0U];
  const auto camera_dy = vehicle.world_position[1U] - view.position[1U];
  const auto camera_dz = vehicle.world_position[2U] - view.position[2U];
  constexpr double maximum_projection_reach = 20.0;
  const auto maximum_camera_distance =
      render_distance + maximum_projection_reach;
  if (camera_dx * camera_dx + camera_dy * camera_dy + camera_dz * camera_dz >
      maximum_camera_distance * maximum_camera_distance) {
    return;
  }
  const auto [left, right] = std::minmax_element(
      visual.headlight_sources.begin(), visual.headlight_sources.end(),
      [](const auto &first, const auto &second) {
        return first.center[0U] < second.center[0U];
      });
  const std::array<const VehicleHeadlightSource *, 2U> sources{&*left, &*right};

  // The source lamps and outward aim are authored/recovered per vehicle. Keep
  // only a conservative screen bound here; after opaque rasterization the
  // retained depth reconstructs the exact visible receiver for every affected
  // pixel. This cannot bridge the road to a wall or leave a missing mesh cell,
  // and costs far less than clipping every beam cell against every MYW face.
  constexpr std::array<double, 7U> distances{0.35, 2.0,  5.0, 8.0,
                                             11.0, 15.0, 20.0};
  constexpr std::array<double, 5U> lateral_coverage{0.0, 0.65, 1.0, 0.65, 0.0};
  HeadlightDepthProjection projection;
  projection.vehicle = vehicle;
  projection.sources = {*sources[0U], *sources[1U]};
  projection.view = view;
  projection.render_distance = render_distance;
  projection.cue_start = render_distance * environment.cue_start;
  projection.cue_enabled = environment.cue_enabled;
  projection.minimum_x = depth_resources.width();
  projection.maximum_x = -1;
  projection.minimum_y = depth_resources.height();
  projection.maximum_y = -1;
  constexpr std::array<double, 2U> receiver_height_offsets{-4.0, 4.0};
  for (std::size_t row = 0U; row < distances.size(); ++row) {
    for (std::size_t column = 0U; column < lateral_coverage.size(); ++column) {
      const auto lateral =
          -0.5 + 2.0 * static_cast<double>(column) /
                     static_cast<double>(lateral_coverage.size() - 1U);
      mh::game::CollisionVector3 local{};
      for (std::size_t axis = 0U; axis < local.size(); ++axis) {
        const auto first = sources[0U]->center[axis] +
                           sources[0U]->direction[axis] * distances[row];
        const auto second = sources[1U]->center[axis] +
                            sources[1U]->direction[axis] * distances[row];
        local[axis] = first * (1.0 - lateral) + second * lateral;
      }
      for (const auto height_offset : receiver_height_offsets) {
        auto bound = local;
        bound[1U] += height_offset;
        const auto point = to_view(
            view, mh::game::project_body_point_to_world(vehicle, bound));
        if (point.z < view.near_plane) {
          continue;
        }
        const auto screen = project(view, point);
        if (!std::isfinite(screen.x) || !std::isfinite(screen.y)) {
          continue;
        }
        projection.minimum_x = std::min(projection.minimum_x,
                                        static_cast<int>(std::floor(screen.x)));
        projection.maximum_x = std::max(projection.maximum_x,
                                        static_cast<int>(std::ceil(screen.x)));
        projection.minimum_y = std::min(projection.minimum_y,
                                        static_cast<int>(std::floor(screen.y)));
        projection.maximum_y = std::max(projection.maximum_y,
                                        static_cast<int>(std::ceil(screen.y)));
      }
    }
  }
  constexpr int screen_margin = 2;
  projection.minimum_x = std::clamp(projection.minimum_x - screen_margin, 0,
                                    depth_resources.width() - 1);
  projection.maximum_x = std::clamp(projection.maximum_x + screen_margin, 0,
                                    depth_resources.width() - 1);
  projection.minimum_y = std::clamp(projection.minimum_y - screen_margin, 0,
                                    depth_resources.height() - 1);
  projection.maximum_y = std::clamp(projection.maximum_y + screen_margin, 0,
                                    depth_resources.height() - 1);
  if (projection.minimum_x <= projection.maximum_x &&
      projection.minimum_y <= projection.maximum_y) {
    depth_resources.record_headlights(projection);
  }
}

void render_environment_scene(
    const EnvironmentSceneVisual &visual,
    const mh::game::OriginalEnvironmentScene &scene,
    const std::vector<EnvironmentCollisionBodyRuntime> &collision_bodies,
    const double authored_frame, const WorldVisual &world_visual,
    const PerspectiveView &view, const bool enhanced,
    const bool trilinear_filtering, const double render_distance,
    const WorldRenderEnvironment &environment,
    const bool point_lighting_enabled, SceneDepthResources &depth_resources,
    VehicleRenderScratch &scratch) {
  for (const auto &instance : visual.instances) {
    const auto &object = scene.objects.at(instance.scene_object_index);
    const auto motion = mh::game::sample_original_environment_scene_motion(
        object, authored_frame);
    const auto maximum_scale = std::max({std::abs(motion.attributes[0U]),
                                         std::abs(motion.attributes[1U]),
                                         std::abs(motion.attributes[2U])});
    const auto &asset = visual.assets.at(instance.asset_index);
    if (!asset.has_blink_emissive_light && maximum_scale <= 1.0e-9) {
      continue;
    }
    // Authored moving objects retain their LWS visual transform until an
    // actual vehicle contact hands the object to rigid-body simulation. The
    // physics pass may resolve transient static overlap on its collision body;
    // using that corrected pose here made independently authored assemblies
    // (notably Okkun's matched helicopter/rotor paths) visibly separate and
    // disturbed the rotor's authored heading animation.
    auto pose = mh::game::make_original_environment_collision_pose(
        object, authored_frame);
    if (instance.position_owner_scene_object_index !=
        instance.scene_object_index) {
      const auto owner_motion =
          mh::game::sample_original_environment_scene_motion(
              scene.objects.at(instance.position_owner_scene_object_index),
              authored_frame);
      for (std::size_t axis = 0U; axis < 3U; ++axis) {
        pose.world_position[axis] = static_cast<double>(
            static_cast<float>(owner_motion.position[axis]));
      }
    }
    if (instance.collision_body_index.has_value()) {
      const auto &runtime = collision_bodies.at(*instance.collision_body_index);
      if (!mh::game::original_environment_scene_has_transform_motion(object) &&
          runtime.detached_from_authored_motion) {
        pose = runtime.body.pose;
      }
    }
    const auto dx = pose.world_position[0U] - view.position[0U];
    const auto dy = pose.world_position[1U] - view.position[1U];
    const auto dz = pose.world_position[2U] - view.position[2U];
    const auto distance_limit =
        render_distance +
        asset.bounding_radius *
            (asset.has_blink_emissive_light ? 1.0 : maximum_scale);
    if (dx * dx + dy * dy + dz * dz > distance_limit * distance_limit) {
      continue;
    }
    if (!asset.has_blink_emissive_light) {
      // Ordinary LWS ObjectMotion channels 6..8 are local X/Y/Z scale.
      for (std::size_t local_axis = 0U; local_axis < 3U; ++local_axis) {
        for (std::size_t world_axis = 0U; world_axis < 3U; ++world_axis) {
          pose.body_basis[local_axis][world_axis] *=
              motion.attributes[local_axis];
        }
      }
    }
    render_vehicle(asset.visual, world_visual, pose, view, enhanced,
                   trilinear_filtering, render_distance, environment,
                   point_lighting_enabled, depth_resources, scratch, {},
                   maximum_scale >= 0.5);
  }
}

std::string phase_text(const mh::game::RaceProgress &progress) {
  switch (progress.phase) {
  case mh::game::RacePhase::ready:
    return "READY - PRESS ENTER OR GAMEPAD START";
  case mh::game::RacePhase::countdown:
    return "COUNTDOWN";
  case mh::game::RacePhase::racing:
    return "DRIVING";
  case mh::game::RacePhase::finished:
    return "FINISHED - PRESS ENTER, GAMEPAD START, OR R TO RESTART";
  }
  return "UNKNOWN";
}

std::optional<std::size_t>
start_signal_phase(const mh::game::RaceProgress &progress,
                   const mh::game::RaceConfig &config,
                   const mh::game::SimulationDuration go_display_remaining) {
  if (progress.phase == mh::game::RacePhase::countdown) {
    const auto signal_duration =
        config.countdown_duration - config.countdown_lead_in_duration;
    if (progress.countdown_remaining > signal_duration) {
      return std::nullopt;
    }
    const auto phase_duration = signal_duration / 3;
    if (progress.countdown_remaining > phase_duration * 2) {
      return 0U;
    }
    if (progress.countdown_remaining > phase_duration) {
      return 1U;
    }
    return 2U;
  }
  if (go_display_remaining > mh::game::SimulationDuration::zero()) {
    return 3U;
  }
  return std::nullopt;
}

std::size_t camera_cycle_index(const mh::game::VehicleCameraMode mode) {
  switch (mode) {
  case mh::game::VehicleCameraMode::chase:
    return 0U;
  case mh::game::VehicleCameraMode::far_chase:
    return 1U;
  case mh::game::VehicleCameraMode::in_car:
    return 2U;
  case mh::game::VehicleCameraMode::bumper:
    return 3U;
  }
  return 0U;
}

const char *camera_mode_name(const mh::game::VehicleCameraMode mode) {
  switch (mode) {
  case mh::game::VehicleCameraMode::chase:
    return "REAR";
  case mh::game::VehicleCameraMode::far_chase:
    return "FAR REAR";
  case mh::game::VehicleCameraMode::in_car:
    return "IN-CAR";
  case mh::game::VehicleCameraMode::bumper:
    return "BUMPER";
  }
  return "UNKNOWN";
}

struct AuthoredStartFlyby {
  std::array<mh::content::MotionData, 2U> shots;
  // p3.1 retains the race-setup timestamp, holds StartSpline1 at frame zero
  // through +6000 ms, then samples StartSpline1 and StartSpline2 in the exact
  // +6000..8000 and +8000..10000 ms windows. The final +10000..12000 ms
  // window is the rear-camera hold. The sampler multiplier at VA 0x0050cbcb
  // is exactly 0.0125 authored-frame units per millisecond.
  static constexpr auto lead_in_duration = std::chrono::seconds(6);
  static constexpr auto signal_phase_duration = std::chrono::seconds(2);
  static constexpr double authored_frames_per_second = 12.5;

  void validate_shot(const std::size_t shot_index) const {
    const auto &shot = shots.at(shot_index);
    if (shot.keyframes.size() < 2U) {
      throw std::runtime_error(
          "authored start-camera motion has fewer than two keys");
    }
    const auto frame_span =
        shot.keyframes.back().frame - shot.keyframes.front().frame;
    if (frame_span <= 0) {
      throw std::runtime_error(
          "authored start-camera motion has no positive duration");
    }
    const auto sampled_span = static_cast<double>(frame_span);
    const auto required_span =
        std::chrono::duration<double>(signal_phase_duration).count() *
        authored_frames_per_second;
    if (sampled_span < required_span) {
      throw std::runtime_error(
          "authored start-camera motion does not cover the retail window");
    }
  }

  [[nodiscard]] mh::game::SimulationDuration phase_duration() const {
    validate_shot(0U);
    validate_shot(1U);
    return signal_phase_duration;
  }

  [[nodiscard]] mh::game::SimulationDuration duration() const {
    return lead_in_duration + phase_duration() * 3;
  }

  [[nodiscard]] mh::game::SimulationDuration signal_duration() const {
    return phase_duration() * 3;
  }

  [[nodiscard]] bool
  active(const mh::game::RaceProgress &progress) const noexcept {
    return progress.phase == mh::game::RacePhase::countdown;
  }

  [[nodiscard]] std::optional<std::size_t>
  phase(const mh::game::RaceProgress &progress) const {
    if (!active(progress)) {
      throw std::logic_error("start-camera phase requested outside countdown");
    }
    const auto one_phase = phase_duration();
    const auto sequence_elapsed = duration() - progress.countdown_remaining;
    if (sequence_elapsed < lead_in_duration) {
      return 0U;
    }
    const auto signal_elapsed = sequence_elapsed - lead_in_duration;
    if (signal_elapsed >= one_phase * 2) {
      return std::nullopt;
    }
    return signal_elapsed < one_phase ? 0U : 1U;
  }

  [[nodiscard]] mh::game::VehicleCameraPose
  camera(const mh::game::RaceProgress &progress,
         const mh::game::OriginalBodyPoseState &vehicle_pose,
         const mh::game::VehicleCameraTuning &tuning) const {
    const auto active_phase = phase(progress);
    if (!active_phase.has_value()) {
      return mh::game::make_vehicle_camera(
          vehicle_pose, mh::game::VehicleCameraMode::chase, {}, tuning);
    }
    const auto one_phase = phase_duration();
    const auto sequence_elapsed = duration() - progress.countdown_remaining;
    const auto signal_elapsed = std::max(mh::game::SimulationDuration::zero(),
                                         sequence_elapsed - lead_in_duration);
    const auto phase_elapsed =
        signal_elapsed - one_phase * static_cast<std::int64_t>(*active_phase);
    const auto active_seconds =
        std::chrono::duration<double>(
            std::clamp(phase_elapsed, mh::game::SimulationDuration::zero(),
                       one_phase))
            .count();
    const auto &active_shot = shots[*active_phase];
    const auto authored_frame =
        static_cast<double>(active_shot.keyframes.front().frame) +
        active_seconds * authored_frames_per_second;
    const auto sample = mh::content::motion_sample(active_shot, authored_frame);
    auto target = vehicle_pose.world_position;
    for (std::size_t axis = 0U; axis < target.size(); ++axis) {
      target[axis] += vehicle_pose.body_basis[1U][axis] * 0.7;
    }
    return {sample.position, target, {0.0, 1.0, 0.0}};
  }
};


SdlPointer<SDL_Texture, SDL_DestroyTexture>
upload_tga_texture(SDL_Renderer *renderer, const mh::content::TgaImage &source,
                   const bool zero_is_transparent = false) {
  mh::content::PamRgbaImage image;
  image.width = source.width;
  image.height = source.height;
  image.rgba = source.rgba;
  if (zero_is_transparent) {
    for (std::size_t pixel = 0U; pixel < image.rgba.size(); pixel += 4U) {
      if (image.rgba[pixel] == 0U && image.rgba[pixel + 1U] == 0U &&
          image.rgba[pixel + 2U] == 0U) {
        image.rgba[pixel + 3U] = 0U;
      }
    }
  }
  return upload_texture(renderer, image);
}

struct RetailLoadingScreen {
  static constexpr std::uint32_t logical_width = 640U;
  static constexpr std::uint32_t logical_height = 400U;
  static constexpr std::uint32_t indicator_width = 12U;
  static constexpr std::uint32_t indicator_height = 11U;
  static constexpr std::array<float, 2U> indicator_x{593.0F, 611.0F};
  static constexpr std::array<float, 6U> indicator_y{303.0F, 317.0F, 330.0F,
                                                     343.0F, 357.0F, 370.0F};

  SDL_Renderer *renderer = nullptr;
  SdlPointer<SDL_Texture, SDL_DestroyTexture> background{nullptr,
                                                         SDL_DestroyTexture};
  SdlPointer<SDL_Texture, SDL_DestroyTexture> indicator{nullptr,
                                                        SDL_DestroyTexture};

  RetailLoadingScreen(SDL_Renderer *active_renderer,
                      const mh::content::TgaImage &background_image,
                      const mh::content::TgaImage &animation_image)
      : renderer(active_renderer),
        background(upload_tga_texture(active_renderer, background_image)) {
    if (background_image.width != logical_width ||
        background_image.height != logical_height) {
      throw std::runtime_error(
          "retail loading background is not the audited 640x400 image");
    }
    if (animation_image.width != indicator_width ||
        animation_image.height != indicator_height * 3U) {
      throw std::runtime_error(
          "retail loading animation is not the audited 12x33 strip");
    }
    mh::content::TgaImage first_frame = animation_image;
    first_frame.height = indicator_height;
    first_frame.rgba.resize(static_cast<std::size_t>(indicator_width) *
                            indicator_height * 4U);
    indicator = upload_tga_texture(active_renderer, first_frame, true);
  }

  void present(const std::size_t completed_rows,
               const std::optional<std::filesystem::path> &screenshot =
                   std::nullopt) const {
    if (completed_rows > indicator_y.size()) {
      throw std::runtime_error(
          "retail loading progress exceeds its six audited rows");
    }
    int width = 0;
    int height = 0;
    require(SDL_GetRenderOutputSize(renderer, &width, &height),
            "query loading-screen render size");
    require(SDL_SetRenderDrawColor(renderer, 0U, 0U, 0U, 255U),
            "set loading-screen clear color");
    require(SDL_RenderClear(renderer), "clear loading screen");
    constexpr float presentation_aspect = 4.0F / 3.0F;
    auto presentation_width = static_cast<float>(width);
    auto presentation_height = presentation_width / presentation_aspect;
    if (presentation_height > static_cast<float>(height)) {
      presentation_height = static_cast<float>(height);
      presentation_width = presentation_height * presentation_aspect;
    }
    const SDL_FRect output{
        (static_cast<float>(width) - presentation_width) * 0.5F,
        (static_cast<float>(height) - presentation_height) * 0.5F,
        presentation_width, presentation_height};
    require(SDL_RenderTexture(renderer, background.get(), nullptr, &output),
            "render retail loading background");

    const auto scale_x = output.w / static_cast<float>(logical_width);
    const auto scale_y = output.h / static_cast<float>(logical_height);
    for (std::size_t completed = 0U; completed < completed_rows; ++completed) {
      const auto row = indicator_y.size() - 1U - completed;
      for (const auto x : indicator_x) {
        const SDL_FRect destination{
            output.x + x * scale_x, output.y + indicator_y[row] * scale_y,
            static_cast<float>(indicator_width) * scale_x,
            static_cast<float>(indicator_height) * scale_y};
        require(
            SDL_RenderTexture(renderer, indicator.get(), nullptr, &destination),
            "render retail loading indicator");
      }
    }
    if (screenshot.has_value()) {
      if (screenshot->has_parent_path()) {
        std::filesystem::create_directories(screenshot->parent_path());
      }
      SdlPointer<SDL_Surface, SDL_DestroySurface> readback(
          SDL_RenderReadPixels(renderer, nullptr), SDL_DestroySurface);
      require(readback != nullptr, "read retail loading frame");
      require(SDL_SaveBMP(readback.get(), screenshot->string().c_str()),
              "save retail loading screenshot");
    }
    require(SDL_RenderPresent(renderer), "present retail loading screen");
  }
};

void render_start_signal(SDL_Renderer *renderer, const RaceHudVisual &hud,
                         const int width, const int height,
                         const std::optional<std::size_t> phase) {
  if (!phase.has_value()) {
    return;
  }
  const auto &texture = hud.countdown.at(*phase);
  constexpr float retail_countdown_scale = 0.72F;
  constexpr float logical_center_x = 320.0F;
  constexpr float logical_center_y = 97.0F;
  const auto viewport = mh::game::original_hud_viewport(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  const auto output_scale = static_cast<float>(viewport.scale);
  const SDL_FRect destination{
      static_cast<float>(mh::game::original_hud_x(viewport, logical_center_x)) -
          static_cast<float>(texture.width) * retail_countdown_scale *
              output_scale * 0.5F,
      static_cast<float>(mh::game::original_hud_y(
          viewport, logical_center_y - static_cast<float>(texture.height) *
                                           retail_countdown_scale * 0.5F)),
      static_cast<float>(texture.width) * retail_countdown_scale * output_scale,
      static_cast<float>(texture.height) * retail_countdown_scale *
          output_scale};
  require(
      SDL_RenderTexture(renderer, texture.texture.get(), nullptr, &destination),
      "render original race countdown sprite");
}

SDL_FRect result_presentation_rect(const int width, const int height) {
  const auto result = mh::ui::front_end_presentation_rect(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  return {result.x, result.y, result.width, result.height};
}

SDL_FRect fitted_presentation_rect(const int source_width,
                                   const int source_height,
                                   const int output_width,
                                   const int output_height) {
  if (source_width <= 0 || source_height <= 0 || output_width <= 0 ||
      output_height <= 0) {
    throw std::invalid_argument("presentation dimensions must be positive");
  }
  const auto source_aspect =
      static_cast<float>(source_width) / static_cast<float>(source_height);
  const auto output_aspect =
      static_cast<float>(output_width) / static_cast<float>(output_height);
  if (output_aspect > source_aspect) {
    const auto height = static_cast<float>(output_height);
    const auto width = height * source_aspect;
    return {(static_cast<float>(output_width) - width) * 0.5F, 0.0F, width,
            height};
  }
  const auto width = static_cast<float>(output_width);
  const auto height = width / source_aspect;
  return {0.0F, (static_cast<float>(output_height) - height) * 0.5F, width,
          height};
}





} // namespace

int mh::app::run_motorhead_race(
    const int argc, char **argv, SDL_Window *shared_window,
    SDL_Renderer *shared_renderer,
    std::optional<mh::ui::RaceResultEntry> *completed_player_result,
    std::vector<mh::ui::RaceResultEntry> *completed_results,
    mh::content::LeagueDivisionFinishingOrders *completed_league_results,
    mh::network::LanSession *multiplayer_session,
    const bool reuse_initialized_sdl) {
  if (completed_player_result != nullptr) {
    completed_player_result->reset();
  }
  if (completed_results != nullptr) {
    completed_results->clear();
  }
  if (completed_league_results != nullptr) {
    for (auto &division : *completed_league_results) {
      division.clear();
    }
  }
  const auto suppress_error_dialog =
      shared_window == nullptr && argc > 0 && argv != nullptr &&
      std::any_of(argv, argv + argc, [](const char *argument) {
        return argument != nullptr && std::string_view(argument) == "--hidden";
      });
  try {
    if (argc < 5) {
      throw std::invalid_argument(
          "usage: Motorhead --race-runtime <track.col> <car.car> "
          "<trioval.mot> <ai.dat> [--frames count] [--size widthxheight] "
          "[--camera rear|far|in-car|bumper] [--enhanced] [--collision] "
          "[--track-definition TrackN.trk] "
          "[--ghost-demo recording.MDE "
          "--ghost-mode race|replay|benchmark] "
          "[--overrides root] [--music track.wav] "
          "[--music-gain multiplier] "
          "[--music-cd-drive drive --music-track number] "
          "[--music-cue disc.cue --music-track number] "
          "[--view-distance-percent 30..150] [--brightness 0..2] "
          "[--window-mode windowed|borderless|fullscreen] "
          "[--configuration-root directory] "
          "[--true-colour on|off] [--triple-buffer on|off] "
          "[--trilinear-filtering on|off] [--texture-format 8|16|32] "
          "[--renderer-settings-report path] "
          "[--renderer-profile-report path] "
          "[--benchmark-report path] "
          "[--renderer-backend auto|d3d9|d3d11|d3d12|glide|software] "
          "[--renderer-workers 1..64] "
          "[--start-paused race|graphics|gameplay|sound] "
          "[--no-background] "
          "[--detail-mode low|medium|high|maximum|custom] "
          "[--info-detail none|selective|all] "
          "[--info-map none|selective|all] "
          "[--lens-flares on|off] [--sparks on|off] "
          "[--shadows on|off] [--skid-marks on|off] "
          "[--smoke on|off] [--halos on|off] [--background on|off] "
          "[--name-plates none|flat|transparent] "
          "[--track-detail medium|high] [--car-detail low|medium|high] "
          "[--car-shading flat|gouraud|reflection|glenz] "
          "[--motion-blur on|off] [--camera-shake on|off] "
          "[--ui-scale-percent 50..150] [--z-read on|off] "
          "[--checkpoint-info none|selective|all] "
          "[--checkpoint-delay-ms 0..10000] "
          "[--measurement metric|imperial] "
          "[--player-name name --team-name name --player-colour R,G,B] "
          "[--short-key0 text ... --short-key9 text] "
          "[--bind-left key --bind-right key "
          "--bind-accelerate key --bind-brake key "
          "--bind-gear-up key --bind-gear-down key --bind-handbrake key "
          "--bind-rear-view key --bind-horn key "
          "--bind-in-car-view key --bind-out-car-view key "
          "--bind-camera-view key --bind-cycle-players key] "
          "[--automatic-transmission on|off] "
          "[--horn-sound selected.wav] "
          "[--sfx-volume 0..255] [--music-volume 0..255] "
          "[--engine-sounds directory] [--race-sounds directory] "
          "[--result-style race|league] [--league-definition league.LGF] "
          "[--difficulty 0|1|2] "
          "[--live-opponents] [--one-make-opponents] "
          "[--single-race-cpu-profile] [--cpu-catch-up] "
          "[--custom-car-colours on|off "
          "--car-colour1 R,G,B --car-colour2 R,G,B --car-colour3 R,G,B] "
          "[--ai-control-report path --ai-random-seed value "
          "[--ai-physics-capture-oracle]] "
          "[--laps count] [--screenshot path] "
          "[--loading-screenshot path] [--hidden]");
    }
    const BuiltInCameraSettings camera_settings;
    std::uint64_t maximum_frames = 0U;
    int window_width = 1280;
    int window_height = 800;
    bool collision_overlay = false;
    bool enhanced_profile = false;
    bool background_enabled = true;
    bool shadows_enabled = true;
    bool skid_marks_enabled = true;
    bool smoke_enabled = true;
    bool halos_enabled = true;
    bool lens_flares_enabled = true;
    bool sparks_enabled = true;
    bool motion_blur_enabled = false;
    bool camera_shake_enabled = true;
    int ui_scale_percent = 100;
    bool z_read_enabled = false;
    auto info_detail_mode = mh::ui::GraphicInfoMode::all;
    auto info_map_mode = mh::ui::GraphicInfoMode::all;
    auto detail_mode = mh::ui::GraphicDetailMode::high;
    auto name_plate_mode = mh::ui::GraphicNamePlateMode::transparent;
    auto track_detail = mh::ui::GraphicTrackDetail::high;
    auto car_detail = mh::ui::GraphicCarDetail::high;
    auto car_shading = mh::ui::GraphicCarShading::reflection;
    bool live_opponents_requested = false;
    bool one_make_opponents = false;
    bool single_race_cpu_profile = false;
    bool cpu_catch_up_enabled = false;
    bool custom_car_colours = false;
    bool custom_car_colours_supplied = false;
    std::optional<std::string> configured_player_name;
    std::optional<std::string> configured_team_name;
    std::optional<mh::content::CarColor> configured_player_colour;
    std::array<std::optional<std::string>, 10U> configured_short_keys{};
    std::array<std::optional<mh::content::CarColor>, 3U>
        configured_car_colours{};
    bool metric_units = true;
    auto checkpoint_info_mode = mh::ui::GraphicInfoMode::all;
    std::uint32_t checkpoint_display_time_ms = 4000U;
    bool hidden_window = false;
    int initial_view_distance_percent = 80;
    auto initial_window_mode = mh::ui::GraphicWindowMode::windowed;
    bool initial_window_mode_supplied = false;
    std::optional<std::filesystem::path> configuration_root;
    float display_brightness = 1.0F;
    auto renderer_backend = mh::render::RaceRendererBackend::automatic;
    bool renderer_backend_supplied = false;
    bool renderer_true_colour = false;
    bool renderer_triple_buffer = false;
    bool renderer_trilinear_filtering = false;
    std::uint32_t renderer_texture_format = 8U;
    std::optional<std::filesystem::path> renderer_settings_report_path;
    std::optional<std::filesystem::path> renderer_profile_report_path;
    std::optional<std::filesystem::path> benchmark_report_path;
    unsigned int renderer_workers = 0U;
    std::optional<RacePausePage> initial_pause_page;
    std::uint32_t race_laps = 3U;
    std::size_t quick_race_difficulty = 1U;
    auto initial_camera = mh::game::VehicleCameraMode::chase;
    std::optional<std::filesystem::path> screenshot_path;
    std::optional<std::filesystem::path> loading_screenshot_path;
    std::optional<std::filesystem::path> explicit_track_definition_path;
    std::optional<std::filesystem::path> ghost_demo_path;
    auto ghost_mode = mh::game::OriginalMdePlaybackMode::ghost_race;
    bool ghost_mode_is_explicit = false;
    std::optional<std::filesystem::path> override_root;
    std::optional<std::filesystem::path> music_path;
    float music_gain = 1.0F;
    std::optional<std::filesystem::path> music_cue_path;
    std::optional<std::filesystem::path> music_cd_drive_path;
    std::optional<int> music_track_number;
    std::optional<std::filesystem::path> engine_sound_root;
    std::optional<std::filesystem::path> race_sound_root;
    std::optional<std::filesystem::path> horn_sound_path;
    std::optional<std::filesystem::path> league_definition_path;
    std::optional<std::filesystem::path> ai_control_report_path;
    std::optional<std::uint32_t> ai_random_seed;
    bool ai_physics_capture_oracle = false;
    bool league_result_style = false;
    int initial_sfx_volume = 253;
    int initial_music_volume = 253;
    bool automatic_transmission = true;
    std::array<std::string, 13U> retail_drive_bindings{};
    for (int index = 5; index < argc; ++index) {
      const auto option = std::string(argv[index]);
      if (option == "--frames") {
        if (++index >= argc) {
          throw std::invalid_argument("--frames requires a count");
        }
        maximum_frames = std::stoull(argv[index]);
        if (maximum_frames == 0U || maximum_frames > 60'000U) {
          throw std::invalid_argument(
              "frame count must be in the range 1 through 60000");
        }
      } else if (option == "--laps") {
        if (++index >= argc) {
          throw std::invalid_argument("--laps requires a count");
        }
        const auto parsed_laps = std::stoull(argv[index]);
        if (parsed_laps == 0U || parsed_laps > 25U) {
          throw std::invalid_argument(
              "lap count must be in the range 1 through 25");
        }
        race_laps = static_cast<std::uint32_t>(parsed_laps);
      } else if (option == "--difficulty") {
        if (++index >= argc) {
          throw std::invalid_argument("--difficulty requires 0, 1, or 2");
        }
        const auto parsed = std::stoull(argv[index]);
        if (parsed > 2U) {
          throw std::invalid_argument(
              "--difficulty must be 0 (Easy), 1 (Medium), or 2 (Hard)");
        }
        quick_race_difficulty = static_cast<std::size_t>(parsed);
      } else if (option == "--size") {
        if (++index >= argc) {
          throw std::invalid_argument("--size requires widthxheight");
        }
        const auto size = std::string(argv[index]);
        const auto separator = size.find_first_of("xX");
        if (separator == std::string::npos) {
          throw std::invalid_argument("--size requires widthxheight");
        }
        window_width = std::stoi(size.substr(0U, separator));
        window_height = std::stoi(size.substr(separator + 1U));
        if (window_width < 640 || window_width > 7680 || window_height < 400 ||
            window_height > 4320) {
          throw std::invalid_argument(
              "window size is outside the supported 640x400..7680x4320 range");
        }
      } else if (option == "--camera") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--camera requires rear, far, in-car, or bumper");
        }
        const auto camera = std::string(argv[index]);
        if (camera == "rear" || camera == "chase" || camera == "close") {
          initial_camera = mh::game::VehicleCameraMode::chase;
        } else if (camera == "far") {
          initial_camera = mh::game::VehicleCameraMode::far_chase;
        } else if (camera == "in-car") {
          initial_camera = mh::game::VehicleCameraMode::in_car;
        } else if (camera == "bumper") {
          initial_camera = mh::game::VehicleCameraMode::bumper;
        } else {
          throw std::invalid_argument(
              "--camera requires rear, far, in-car, or bumper");
        }
      } else if (option == "--screenshot") {
        if (++index >= argc) {
          throw std::invalid_argument("--screenshot requires a path");
        }
        screenshot_path = std::filesystem::path(argv[index]);
      } else if (option == "--loading-screenshot") {
        if (++index >= argc) {
          throw std::invalid_argument("--loading-screenshot requires a path");
        }
        loading_screenshot_path = std::filesystem::path(argv[index]);
      } else if (option == "--track-definition") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--track-definition requires a TrackN.trk path");
        }
        explicit_track_definition_path = std::filesystem::path(argv[index]);
      } else if (option == "--ghost-demo") {
        if (++index >= argc) {
          throw std::invalid_argument("--ghost-demo requires an MDE path");
        }
        ghost_demo_path = std::filesystem::path(argv[index]);
      } else if (option == "--ghost-mode") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--ghost-mode requires race, replay, or benchmark");
        }
        const auto mode = std::string_view(argv[index]);
        if (mode == "race") {
          ghost_mode = mh::game::OriginalMdePlaybackMode::ghost_race;
        } else if (mode == "replay") {
          ghost_mode = mh::game::OriginalMdePlaybackMode::replay;
        } else if (mode == "benchmark") {
          ghost_mode = mh::game::OriginalMdePlaybackMode::benchmark;
        } else {
          throw std::invalid_argument(
              "--ghost-mode requires race, replay, or benchmark");
        }
        ghost_mode_is_explicit = true;
      } else if (option == "--overrides") {
        if (++index >= argc) {
          throw std::invalid_argument("--overrides requires a root directory");
        }
        override_root = std::filesystem::path(argv[index]);
      } else if (option == "--music") {
        if (++index >= argc) {
          throw std::invalid_argument("--music requires a WAV path");
        }
        music_path = std::filesystem::path(argv[index]);
      } else if (option == "--music-gain") {
        if (++index >= argc) {
          throw std::invalid_argument("--music-gain requires a multiplier");
        }
        music_gain = std::stof(argv[index]);
        if (!std::isfinite(music_gain) || music_gain <= 0.0F ||
            music_gain > 4.0F) {
          throw std::invalid_argument(
              "--music-gain must be greater than zero and at most four");
        }
      } else if (option == "--music-cue") {
        if (++index >= argc) {
          throw std::invalid_argument("--music-cue requires a CUE path");
        }
        music_cue_path = std::filesystem::path(argv[index]);
      } else if (option == "--music-cd-drive") {
        if (++index >= argc) {
          throw std::invalid_argument("--music-cd-drive requires a drive");
        }
        music_cd_drive_path = std::filesystem::path(argv[index]);
      } else if (option == "--music-track") {
        if (++index >= argc) {
          throw std::invalid_argument("--music-track requires a number");
        }
        const auto parsed = std::stoi(argv[index]);
        if (parsed < 2 || parsed > 99) {
          throw std::invalid_argument(
              "--music-track must be in the range 2 through 99");
        }
        music_track_number = parsed;
      } else if (option == "--view-distance-percent") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--view-distance-percent requires a value");
        }
        initial_view_distance_percent = std::stoi(argv[index]);
        if (initial_view_distance_percent < 30 ||
            initial_view_distance_percent > 150) {
          throw std::invalid_argument(
              "--view-distance-percent must be in the range 30 through 150");
        }
      } else if (option == "--ui-scale-percent") {
        if (++index >= argc) {
          throw std::invalid_argument("--ui-scale-percent requires a value");
        }
        ui_scale_percent = std::stoi(argv[index]);
        if (ui_scale_percent < 50 || ui_scale_percent > 150 ||
            ui_scale_percent % 5 != 0) {
          throw std::invalid_argument(
              "--ui-scale-percent must be a 5% step from 50 through 150");
        }
      } else if (option == "--window-mode") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--window-mode requires windowed, borderless, or fullscreen");
        }
        const auto value = ascii_lower(argv[index]);
        if (value != "windowed" && value != "borderless" &&
            value != "fullscreen") {
          throw std::invalid_argument(
              "--window-mode requires windowed, borderless, or fullscreen");
        }
        initial_window_mode = mh::ui::graphic_window_mode_from_config(value);
        initial_window_mode_supplied = true;
      } else if (option == "--configuration-root") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--configuration-root requires a directory");
        }
        configuration_root = std::filesystem::path(argv[index]);
      } else if (option == "--brightness") {
        if (++index >= argc) {
          throw std::invalid_argument("--brightness requires a value");
        }
        display_brightness = std::stof(argv[index]);
        if (!std::isfinite(display_brightness) || display_brightness < 0.0F ||
            display_brightness > 2.0F) {
          throw std::invalid_argument(
              "--brightness must be finite and in the range 0 through 2");
        }
      } else if (option == "--renderer-backend") {
        if (++index >= argc) {
          throw std::invalid_argument("--renderer-backend requires auto, "
                                      "d3d9, d3d11, d3d12, glide, or software");
        }
        const auto value = ascii_lower(argv[index]);
        if (value == "auto") {
          renderer_backend = mh::render::RaceRendererBackend::automatic;
        } else if (value == "d3d9") {
          renderer_backend = mh::render::RaceRendererBackend::d3d9;
        } else if (value == "d3d11") {
          renderer_backend = mh::render::RaceRendererBackend::d3d11;
        } else if (value == "d3d12") {
          renderer_backend = mh::render::RaceRendererBackend::d3d12;
        } else if (value == "glide") {
          renderer_backend = mh::render::RaceRendererBackend::glide;
        } else if (value == "software") {
          renderer_backend = mh::render::RaceRendererBackend::software;
        } else {
          throw std::invalid_argument("--renderer-backend requires auto, "
                                      "d3d9, d3d11, d3d12, glide, or software");
        }
        renderer_backend_supplied = true;
      } else if (option == "--true-colour" || option == "--triple-buffer" ||
                 option == "--trilinear-filtering") {
        if (++index >= argc) {
          throw std::invalid_argument(option + " requires on or off");
        }
        const auto value = ascii_lower(argv[index]);
        if (value != "on" && value != "off") {
          throw std::invalid_argument(option + " requires on or off");
        }
        const auto enabled = value == "on";
        if (option == "--true-colour") {
          renderer_true_colour = enabled;
        } else if (option == "--triple-buffer") {
          renderer_triple_buffer = enabled;
        } else {
          renderer_trilinear_filtering = enabled;
        }
      } else if (option == "--texture-format") {
        if (++index >= argc) {
          throw std::invalid_argument("--texture-format requires 8, 16, or 32");
        }
        const auto parsed = std::stoul(argv[index]);
        if (parsed != 8U && parsed != 16U && parsed != 32U) {
          throw std::invalid_argument("--texture-format requires 8, 16, or 32");
        }
        renderer_texture_format = parsed;
      } else if (option == "--renderer-settings-report") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--renderer-settings-report requires a path");
        }
        renderer_settings_report_path = std::filesystem::path(argv[index]);
      } else if (option == "--renderer-profile-report") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--renderer-profile-report requires a path");
        }
        renderer_profile_report_path = std::filesystem::path(argv[index]);
      } else if (option == "--benchmark-report") {
        if (++index >= argc) {
          throw std::invalid_argument("--benchmark-report requires a path");
        }
        benchmark_report_path = std::filesystem::path(argv[index]);
      } else if (option == "--renderer-workers") {
        if (++index >= argc) {
          throw std::invalid_argument("--renderer-workers requires a count");
        }
        const auto parsed = std::stoul(argv[index]);
        if (parsed == 0U || parsed > 64U) {
          throw std::invalid_argument(
              "--renderer-workers must be in the range 1 through 64");
        }
        renderer_workers = parsed;
      } else if (option == "--start-paused") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--start-paused requires race, graphics, gameplay, or sound");
        }
        const auto page = ascii_lower(argv[index]);
        if (page == "race") {
          initial_pause_page = RacePausePage::race;
        } else if (page == "graphics") {
          initial_pause_page = RacePausePage::graphics;
        } else if (page == "sound") {
          initial_pause_page = RacePausePage::sound;
        } else if (page == "gameplay") {
          initial_pause_page = RacePausePage::gameplay;
        } else {
          throw std::invalid_argument(
              "--start-paused requires race, graphics, gameplay, or sound");
        }
      } else if (option == "--measurement") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--measurement requires metric or imperial");
        }
        const auto measurement = std::string_view(argv[index]);
        if (measurement == "metric") {
          metric_units = true;
        } else if (measurement == "imperial") {
          metric_units = false;
        } else {
          throw std::invalid_argument(
              "--measurement requires metric or imperial");
        }
      } else if (option == "--info-detail" || option == "--info-map") {
        if (++index >= argc) {
          throw std::invalid_argument(option +
                                      " requires none, selective, or all");
        }
        const auto value = ascii_lower(argv[index]);
        const auto parsed =
            value == "none"        ? mh::ui::GraphicInfoMode::none
            : value == "selective" ? mh::ui::GraphicInfoMode::selective
            : value == "all"
                ? mh::ui::GraphicInfoMode::all
                : throw std::invalid_argument(
                      option + " requires none, selective, or all");
        (option == "--info-detail" ? info_detail_mode : info_map_mode) = parsed;
      } else if (option == "--detail-mode") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--detail-mode requires low, medium, high, maximum, or custom");
        }
        const auto value = ascii_lower(argv[index]);
        if (value == "low")
          detail_mode = mh::ui::GraphicDetailMode::low;
        else if (value == "medium")
          detail_mode = mh::ui::GraphicDetailMode::medium;
        else if (value == "high")
          detail_mode = mh::ui::GraphicDetailMode::high;
        else if (value == "maximum" || value == "max")
          detail_mode = mh::ui::GraphicDetailMode::maximum;
        else if (value == "custom")
          detail_mode = mh::ui::GraphicDetailMode::custom;
        else
          throw std::invalid_argument(
              "--detail-mode requires low, medium, high, maximum, or custom");
      } else if (option == "--name-plates") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--name-plates requires none, flat, or transparent");
        }
        const auto value = ascii_lower(argv[index]);
        if (value == "none")
          name_plate_mode = mh::ui::GraphicNamePlateMode::none;
        else if (value == "flat")
          name_plate_mode = mh::ui::GraphicNamePlateMode::flat;
        else if (value == "transparent")
          name_plate_mode = mh::ui::GraphicNamePlateMode::transparent;
        else
          throw std::invalid_argument(
              "--name-plates requires none, flat, or transparent");
      } else if (option == "--track-detail") {
        if (++index >= argc) {
          throw std::invalid_argument("--track-detail requires medium or high");
        }
        const auto value = ascii_lower(argv[index]);
        if (value == "medium")
          track_detail = mh::ui::GraphicTrackDetail::medium;
        else if (value == "high")
          track_detail = mh::ui::GraphicTrackDetail::high;
        else
          throw std::invalid_argument("--track-detail requires medium or high");
      } else if (option == "--car-detail") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--car-detail requires low, medium, or high");
        }
        const auto value = ascii_lower(argv[index]);
        if (value == "low")
          car_detail = mh::ui::GraphicCarDetail::low;
        else if (value == "medium")
          car_detail = mh::ui::GraphicCarDetail::medium;
        else if (value == "high")
          car_detail = mh::ui::GraphicCarDetail::high;
        else
          throw std::invalid_argument(
              "--car-detail requires low, medium, or high");
      } else if (option == "--car-shading") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--car-shading requires flat, gouraud, reflection, or glenz");
        }
        const auto value = ascii_lower(argv[index]);
        if (value == "flat")
          car_shading = mh::ui::GraphicCarShading::flat;
        else if (value == "gouraud")
          car_shading = mh::ui::GraphicCarShading::gouraud;
        else if (value == "reflection")
          car_shading = mh::ui::GraphicCarShading::reflection;
        else if (value == "glenz")
          car_shading = mh::ui::GraphicCarShading::glenz;
        else
          throw std::invalid_argument(
              "--car-shading requires flat, gouraud, reflection, or glenz");
      } else if (option == "--shadows") {
        if (++index >= argc) {
          throw std::invalid_argument("--shadows requires on or off");
        }
        const auto enabled = ascii_lower(argv[index]);
        if (enabled == "on") {
          shadows_enabled = true;
        } else if (enabled == "off") {
          shadows_enabled = false;
        } else {
          throw std::invalid_argument("--shadows requires on or off");
        }
      } else if (option == "--skid-marks") {
        if (++index >= argc) {
          throw std::invalid_argument("--skid-marks requires on or off");
        }
        const auto enabled = ascii_lower(argv[index]);
        if (enabled == "on") {
          skid_marks_enabled = true;
        } else if (enabled == "off") {
          skid_marks_enabled = false;
        } else {
          throw std::invalid_argument("--skid-marks requires on or off");
        }
      } else if (option == "--smoke" || option == "--halos" ||
                 option == "--lens-flares" || option == "--sparks" ||
                 option == "--background" || option == "--motion-blur" ||
                 option == "--camera-shake" || option == "--z-read") {
        if (++index >= argc) {
          throw std::invalid_argument(option + " requires on or off");
        }
        const auto enabled = ascii_lower(argv[index]);
        if (enabled != "on" && enabled != "off") {
          throw std::invalid_argument(option + " requires on or off");
        }
        auto &destination = option == "--smoke"          ? smoke_enabled
                            : option == "--halos"        ? halos_enabled
                            : option == "--lens-flares"  ? lens_flares_enabled
                            : option == "--sparks"       ? sparks_enabled
                            : option == "--background"   ? background_enabled
                            : option == "--motion-blur"  ? motion_blur_enabled
                            : option == "--camera-shake" ? camera_shake_enabled
                                                         : z_read_enabled;
        destination = enabled == "on";
      } else if (option == "--checkpoint-info") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--checkpoint-info requires none, selective, or all");
        }
        const auto mode = ascii_lower(argv[index]);
        if (mode == "none") {
          checkpoint_info_mode = mh::ui::GraphicInfoMode::none;
        } else if (mode == "selective") {
          checkpoint_info_mode = mh::ui::GraphicInfoMode::selective;
        } else if (mode == "all") {
          checkpoint_info_mode = mh::ui::GraphicInfoMode::all;
        } else {
          throw std::invalid_argument(
              "--checkpoint-info requires none, selective, or all");
        }
      } else if (option == "--checkpoint-delay-ms") {
        if (++index >= argc) {
          throw std::invalid_argument("--checkpoint-delay-ms requires a value");
        }
        const auto parsed = std::stoull(argv[index]);
        if (parsed > 10'000U) {
          throw std::invalid_argument(
              "--checkpoint-delay-ms must be in the range 0 through 10000");
        }
        checkpoint_display_time_ms = static_cast<std::uint32_t>(parsed);
      } else if (option == "--custom-car-colours") {
        if (++index >= argc || custom_car_colours_supplied) {
          throw std::invalid_argument(
              "--custom-car-colours requires exactly one on or off value");
        }
        const auto enabled = ascii_lower(argv[index]);
        if (enabled == "on") {
          custom_car_colours = true;
        } else if (enabled == "off") {
          custom_car_colours = false;
        } else {
          throw std::invalid_argument(
              "--custom-car-colours requires on or off");
        }
        custom_car_colours_supplied = true;
      } else if (option == "--player-name" || option == "--team-name") {
        if (++index >= argc) {
          throw std::invalid_argument(option + " requires a value");
        }
        auto &destination = option == "--player-name" ? configured_player_name
                                                      : configured_team_name;
        if (destination.has_value()) {
          throw std::invalid_argument(option + " was supplied more than once");
        }
        destination = argv[index];
      } else if (option == "--player-colour") {
        if (++index >= argc || configured_player_colour.has_value()) {
          throw std::invalid_argument(
              "--player-colour requires exactly one R,G,B value");
        }
        configured_player_colour =
            parse_rgb_argument(argv[index], "--player-colour");
      } else if (option.size() == std::string_view("--short-key0").size() &&
                 option.starts_with("--short-key") && option.back() >= '0' &&
                 option.back() <= '9') {
        if (++index >= argc) {
          throw std::invalid_argument(option + " requires a value");
        }
        const auto slot = static_cast<std::size_t>(option.back() - '0');
        if (configured_short_keys[slot].has_value()) {
          throw std::invalid_argument(option + " was supplied more than once");
        }
        configured_short_keys[slot] = argv[index];
      } else if (option == "--car-colour1" || option == "--car-colour2" ||
                 option == "--car-colour3") {
        if (++index >= argc) {
          throw std::invalid_argument(option + " requires R,G,B");
        }
        const auto slot = static_cast<std::size_t>(option.back() - '1');
        if (configured_car_colours[slot].has_value()) {
          throw std::invalid_argument(option + " was supplied more than once");
        }
        configured_car_colours[slot] = parse_rgb_argument(argv[index], option);
      } else if (option == "--bind-left" || option == "--bind-right" ||
                 option == "--bind-accelerate" || option == "--bind-brake" ||
                 option == "--bind-gear-up" || option == "--bind-gear-down" ||
                 option == "--bind-handbrake" || option == "--bind-rear-view" ||
                 option == "--bind-horn" || option == "--bind-in-car-view" ||
                 option == "--bind-out-car-view" ||
                 option == "--bind-camera-view" ||
                 option == "--bind-cycle-players") {
        if (++index >= argc) {
          throw std::invalid_argument(option + " requires a key name");
        }
        const auto binding_index = option == "--bind-left"           ? 0U
                                   : option == "--bind-right"        ? 1U
                                   : option == "--bind-accelerate"   ? 2U
                                   : option == "--bind-brake"        ? 3U
                                   : option == "--bind-handbrake"    ? 4U
                                   : option == "--bind-rear-view"    ? 5U
                                   : option == "--bind-horn"         ? 6U
                                   : option == "--bind-gear-up"      ? 7U
                                   : option == "--bind-gear-down"    ? 8U
                                   : option == "--bind-in-car-view"  ? 9U
                                   : option == "--bind-out-car-view" ? 10U
                                   : option == "--bind-camera-view"  ? 11U
                                                                     : 12U;
        retail_drive_bindings[binding_index] = argv[index];
      } else if (option == "--automatic-transmission") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--automatic-transmission requires on or off");
        }
        const auto enabled = ascii_lower(argv[index]);
        if (enabled != "on" && enabled != "off") {
          throw std::invalid_argument(
              "--automatic-transmission requires on or off");
        }
        automatic_transmission = enabled == "on";
      } else if (option == "--sfx-volume" || option == "--music-volume") {
        if (++index >= argc) {
          throw std::invalid_argument(option + " requires a value");
        }
        const auto parsed = std::stoi(argv[index]);
        if (parsed < 0 || parsed > 255) {
          throw std::invalid_argument(option +
                                      " must be in the range 0 through 255");
        }
        auto &destination = option == "--sfx-volume" ? initial_sfx_volume
                                                     : initial_music_volume;
        destination = std::min(parsed, 254);
      } else if (option == "--engine-sounds") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--engine-sounds requires the retail Sounds directory");
        }
        engine_sound_root = std::filesystem::path(argv[index]);
      } else if (option == "--race-sounds") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--race-sounds requires the retail Sounds directory");
        }
        race_sound_root = std::filesystem::path(argv[index]);
      } else if (option == "--horn-sound") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--horn-sound requires the selected CHF WAV path");
        }
        horn_sound_path = std::filesystem::path(argv[index]);
      } else if (option == "--result-style") {
        if (++index >= argc) {
          throw std::invalid_argument("--result-style requires race or league");
        }
        const auto style = std::string_view(argv[index]);
        if (style == "race") {
          league_result_style = false;
        } else if (style == "league") {
          league_result_style = true;
        } else {
          throw std::invalid_argument("--result-style requires race or league");
        }
      } else if (option == "--league-definition") {
        if (++index >= argc) {
          throw std::invalid_argument(
              "--league-definition requires an LGF path");
        }
        league_definition_path = std::filesystem::path(argv[index]);
      } else if (option == "--ai-control-report") {
        if (++index >= argc) {
          throw std::invalid_argument("--ai-control-report requires a path");
        }
        ai_control_report_path = std::filesystem::path(argv[index]);
      } else if (option == "--ai-random-seed") {
        if (++index >= argc) {
          throw std::invalid_argument("--ai-random-seed requires a value");
        }
        const auto parsed = std::stoull(argv[index], nullptr, 0);
        if (parsed > std::numeric_limits<std::uint32_t>::max()) {
          throw std::invalid_argument(
              "--ai-random-seed is outside the uint32 range");
        }
        ai_random_seed = static_cast<std::uint32_t>(parsed);
      } else if (option == "--ai-physics-capture-oracle") {
        ai_physics_capture_oracle = true;
      } else if (option == "--collision") {
        collision_overlay = true;
      } else if (option == "--enhanced") {
        enhanced_profile = true;
      } else if (option == "--no-background") {
        background_enabled = false;
      } else if (option == "--live-opponents") {
        live_opponents_requested = true;
      } else if (option == "--one-make-opponents") {
        one_make_opponents = true;
      } else if (option == "--single-race-cpu-profile") {
        single_race_cpu_profile = true;
      } else if (option == "--cpu-catch-up") {
        cpu_catch_up_enabled = true;
      } else if (option == "--hidden") {
        hidden_window = true;
      } else {
        throw std::invalid_argument("unknown option: " + option);
      }
    }
    if ((music_cue_path.has_value() || music_cd_drive_path.has_value()) !=
        music_track_number.has_value()) {
      throw std::invalid_argument(
          "a CD music source and --music-track must be supplied together");
    }
    const auto music_source_count =
        static_cast<unsigned int>(music_path.has_value()) +
        static_cast<unsigned int>(music_cue_path.has_value()) +
        static_cast<unsigned int>(music_cd_drive_path.has_value());
    if (music_source_count > 1U) {
      throw std::invalid_argument("only one music source may be supplied");
    }
    if (ghost_demo_path.has_value() != ghost_mode_is_explicit) {
      throw std::invalid_argument(
          "--ghost-demo and --ghost-mode must be supplied together");
    }
    if (league_definition_path.has_value() != league_result_style) {
      throw std::invalid_argument(
          "--league-definition and --result-style league must be supplied "
          "together");
    }
    if (ai_control_report_path.has_value() != ai_random_seed.has_value()) {
      throw std::invalid_argument(
          "--ai-control-report and --ai-random-seed must be supplied together");
    }
    if (ai_physics_capture_oracle && !ai_control_report_path.has_value()) {
      throw std::invalid_argument(
          "--ai-physics-capture-oracle requires --ai-control-report");
    }
    const auto supplied_car_colour_count =
        static_cast<std::size_t>(std::count_if(
            configured_car_colours.begin(), configured_car_colours.end(),
            [](const auto &value) { return value.has_value(); }));
    if (supplied_car_colour_count != 0U &&
        supplied_car_colour_count != configured_car_colours.size()) {
      throw std::invalid_argument(
          "all three --car-colour values must be supplied together");
    }
    if (custom_car_colours &&
        supplied_car_colour_count != configured_car_colours.size()) {
      throw std::invalid_argument(
          "custom car colours require all three --car-colour values");
    }
    mh::game::set_original_hud_ui_scale(static_cast<double>(ui_scale_percent) /
                                        100.0);

    const auto collision_path = std::filesystem::path(argv[1]);
    const auto collision = mh::content::read_col(collision_path);
    const auto polygons =
        mh::content::reconstruct_col_polygons(collision);
    const auto make_track_collision_world = [&]() {
      return mh::game::make_collision_world(collision);
    };
    const auto shadow_collision_world = make_track_collision_world();
    const auto track_root = collision_path.parent_path().parent_path();
    const auto car_path = std::filesystem::path(argv[2]);
    auto car = mh::content::read_car(car_path);
    apply_playable_car_tuning(car);
    const auto content_root = car_path.parent_path().parent_path();
    const auto configuration_path =
        configuration_root.has_value()
            ? mh::ui::motorhead_configuration_path(*configuration_root)
            : content_root / "Game" / "motorhead.cfg";
    if (configuration_root.has_value()) {
      static_cast<void>(mh::common::initialize_runtime_log(*configuration_root /
                                                           "motorhead.log"));
    }
    if (!initial_window_mode_supplied) {
      initial_window_mode = mh::ui::graphic_window_mode_from_config(
          mh::ui::configuration_file_value(configuration_path, "WindowMode"));
    }
    if (!renderer_backend_supplied) {
      const auto configured = mh::ui::graphic_renderer_backend_from_config(
          mh::ui::configuration_file_value(configuration_path, "Renderer"));
      renderer_backend = race_renderer_backend(configured);
    }
    {
      std::ostringstream message;
      message << "Race runtime: renderer="
              << renderer_backend_token(renderer_backend)
              << ", resolution=" << window_width << 'x' << window_height
              << ", window="
              << mh::ui::graphic_window_mode_config_name(initial_window_mode)
              << ", track=" << collision_path.filename().string()
              << ", car=" << car_path.filename().string();
      mh::common::log_runtime_info(message.str());
    }
    const auto identity_player_name = configured_player_name.value_or(
        read_motorhead_config_value(configuration_path, "PlayerName"));
    const auto identity_team_name = configured_team_name.value_or(
        read_motorhead_config_value(configuration_path, "TeamName"));
    const auto pc_cheats = mh::game::original_pc_cheat_state(
        identity_player_name, identity_team_name);
    if (mh::game::original_pc_hidden_executable_story().empty()) {
      throw std::runtime_error("original executable story is missing");
    }
    std::optional<mh::content::MdeV3Data> ghost_demo;
    if (ghost_demo_path.has_value()) {
      ghost_demo.emplace(mh::content::read_mde(*ghost_demo_path));
    }
    const auto track_definition_path =
        explicit_track_definition_path.has_value()
            ? *explicit_track_definition_path
            : find_sibling_case_insensitive(
                  find_child_directory_case_insensitive(content_root, "Game"),
                  track_root.filename().string() + ".trk");
    const auto track_definition =
        mh::content::read_track_definition(track_definition_path);
    std::string atlantika_scroll_text;
    const auto active_track_name = ascii_lower(track_definition.name);
    if (active_track_name == "atlantika" || active_track_name == "atlantikar") {
      atlantika_scroll_text =
          read_original_scroll_text(resolve_relative_case_insensitive(
              content_root, "Data\\d3d\\tullinge.iff"));
    }
    std::optional<mh::game::OriginalEnvironmentSoundTable>
        environment_sound_definitions;
    const auto scene_reference = std::find_if(
        track_definition.references.begin(), track_definition.references.end(),
        [](const auto &candidate) { return candidate.field == "scenename"; });
    if (scene_reference == track_definition.references.end()) {
      throw std::runtime_error(
          "active track definition has no environment scene");
    }
    const auto environment_scene_path =
        resolve_relative_case_insensitive(content_root, scene_reference->value);
    const auto environment_scene =
        mh::game::read_original_environment_scene(environment_scene_path);
    const auto environment_collision_body_assets =
        load_environment_collision_body_assets(environment_scene,
                                               environment_scene_path);
    auto environment_scene_visual =
        load_environment_scene_visual(environment_scene, environment_scene_path,
                                      environment_collision_body_assets);
    std::vector<EnvironmentCollisionBodyRuntime> environment_collision_bodies;
    environment_collision_bodies.reserve(
        environment_collision_body_assets.size());
    for (const auto &asset : environment_collision_body_assets) {
      const auto &object =
          environment_scene.objects.at(asset.scene_object_index);
      EnvironmentCollisionBodyRuntime runtime;
      runtime.body.pose =
          mh::game::make_original_environment_collision_pose(object, 0.0);
      runtime.physics_previous_pose = runtime.body.pose;
      runtime.collision_identifier = object.collision_identifier.value_or(0U);
      environment_collision_bodies.push_back(std::move(runtime));
    }
    if (race_sound_root.has_value()) {
      const auto sound_reference = std::find_if(
          track_definition.references.begin(),
          track_definition.references.end(), [](const auto &candidate) {
            return candidate.field == "envsoundname";
          });
      if (sound_reference == track_definition.references.end()) {
        throw std::runtime_error(
            "active track definition has no environment sound definition");
      }
      environment_sound_definitions =
          mh::game::read_original_environment_sounds(
              resolve_relative_case_insensitive(content_root,
                                                sound_reference->value));
    }
    if (ghost_demo.has_value() && ascii_lower(ghost_demo->track_name) !=
                                      ascii_lower(track_definition.name)) {
      throw std::runtime_error(
          "selected MDE track does not match the active race track");
    }
    const auto mde_presentation =
        ghost_demo.has_value()
            ? std::optional<mh::game::OriginalMdePresentationContract>(
                  mh::game::original_mde_presentation_contract(ghost_mode))
            : std::nullopt;
    const auto recorded_presentation =
        mde_presentation.has_value() &&
        (mde_presentation->active_render_flags &
         static_cast<std::uint8_t>(mh::game::OriginalMdeRenderFlag::demo)) !=
            0U;
    const auto replay_presentation =
        recorded_presentation &&
        ghost_mode == mh::game::OriginalMdePlaybackMode::replay;
    const auto benchmark_presentation =
        recorded_presentation &&
        ghost_mode == mh::game::OriginalMdePlaybackMode::benchmark;
    if (benchmark_presentation && !benchmark_report_path.has_value()) {
      auto motorhead_directory = std::filesystem::current_path();
      if (const auto *base_path = SDL_GetBasePath();
          base_path != nullptr && *base_path != '\0') {
        motorhead_directory =
            std::filesystem::path(base_path).lexically_normal();
        if (motorhead_directory.filename().empty()) {
          motorhead_directory = motorhead_directory.parent_path();
        }
      }
      // The retail Ghost Mode page names this exact file and location:
      // "'Fps.txt' in your Motorhead directory."
      benchmark_report_path = motorhead_directory / "Fps.txt";
    }
    std::optional<mh::content::MotionData> recorded_camera_motion;
    if (replay_presentation) {
      const auto camera_reference = std::find_if(
          track_definition.references.begin(),
          track_definition.references.end(), [](const auto &candidate) {
            return candidate.field == "camerasplinename";
          });
      if (camera_reference == track_definition.references.end()) {
        throw std::runtime_error(
            "recorded presentation track has no CameraSplineName");
      }
      recorded_camera_motion =
          mh::content::read_motion(resolve_relative_case_insensitive(
              content_root, camera_reference->value));
    }
    const auto base_world_path = resolve_relative_case_insensitive(
        content_root, (std::filesystem::path(track_definition.base_path) /
                       track_definition.world_filename)
                          .string());
    const auto world_path = track_detail == mh::ui::GraphicTrackDetail::high
                                ? resolve_relative_case_insensitive(
                                      base_world_path.parent_path(),
                                      (std::filesystem::path("Hardware") /
                                       base_world_path.filename())
                                          .string())
                                : base_world_path;
    if (ascii_lower(track_definition.world_filename) !=
        ascii_lower(world_path.filename().string())) {
      throw std::runtime_error(
          "active track definition does not select the loaded MYW");
    }
    if (!track_definition.environment.view_distance.has_value()) {
      throw std::runtime_error(
          "active track definition has no authored view distance");
    }
    const auto render_distance =
        static_cast<double>(*track_definition.environment.view_distance);
    WorldRenderEnvironment render_environment;
    render_environment.brightness =
        track_definition.environment.world_brightness.value_or(
            std::array<float, 3U>{1.0F, 1.0F, 1.0F});
    render_environment.object_brightness =
        track_definition.environment.object_brightness.value_or(
            std::array<float, 3U>{1.0F, 1.0F, 1.0F});
    render_environment.object_ambient =
        track_definition.environment.object_ambient.value_or(
            std::array<float, 3U>{1.0F, 1.0F, 1.0F});
    render_environment.accelerated_brightness =
        track_definition.environment.accelerated_brightness.value_or(
            std::array<float, 3U>{1.0F, 1.0F, 1.0F});
    render_environment.specular_factor =
        track_definition.environment.specular_factor.value_or(0.0F);
    render_environment.light_intensity =
        track_definition.environment.light_intensity.value_or(1.0F);
    if (track_definition.environment.top_color.has_value() &&
        track_definition.environment.light_intensity.has_value()) {
      const auto &top_color = *track_definition.environment.top_color;
      render_environment.base_lighting =
          mh::content::evaluate_original_myw_base_lighting(
              {top_color.red, top_color.green, top_color.blue},
              *track_definition.environment.light_intensity);
    }
    if (track_definition.environment.cue_color.has_value() &&
        track_definition.environment.cue_start.has_value()) {
      render_environment.cue_color = *track_definition.environment.cue_color;
      // Use the track's authored cue colour, but reserve the nearest 55% of
      // the selected view range as clear visibility. The wider final 45%
      // gives perspective projection enough screen space for visible fog.
      // Because every call receives the active (percentage-scaled) render
      // distance, this remains proportional across the supported range.
      constexpr double extended_view_cue_start = 0.55;
      render_environment.cue_start = extended_view_cue_start;
      render_environment.cue_enabled = true;
    }
    const auto scale_render_environment =
        [&render_environment](const std::array<float, 3U> scale) {
          for (std::size_t channel = 0U; channel < scale.size(); ++channel) {
            render_environment.brightness[channel] *= scale[channel];
            render_environment.object_brightness[channel] *= scale[channel];
            render_environment.object_ambient[channel] *= scale[channel];
            render_environment.accelerated_brightness[channel] *=
                scale[channel];
            render_environment.base_lighting[channel] *= scale[channel];
          }
        };
    if (pc_cheats.active(mh::game::OriginalPcCheat::thunder)) {
      // Exact non-flash accelerated-renderer multipliers written at p3.1
      // RVA 0x0005127c..0x000512d3.
      scale_render_environment({0.8F, 0.4F, 0.4F});
    } else if (pc_cheats.active(mh::game::OriginalPcCheat::underwater)) {
      // Exact underwater multipliers from RVA 0x000512de..0x00051344.
      scale_render_environment({0.5F, 1.0F, 1.2F});
    }
    std::set<std::string> zero_transparent_materials;
    std::optional<std::string> background_reference;
    std::vector<std::filesystem::path> world_halo_paths;
    for (const auto &reference : track_definition.references) {
      if (reference.field == "zerotransparent") {
        zero_transparent_materials.insert(ascii_lower(reference.value));
      } else if (reference.field == "backgroundname") {
        if (background_reference.has_value()) {
          throw std::runtime_error(
              "active track definition has multiple backgrounds");
        }
        background_reference = reference.value;
      } else if (reference.field == "halo") {
        world_halo_paths.push_back(
            resolve_relative_case_insensitive(content_root, reference.value));
      }
    }
    if (!background_reference.has_value()) {
      throw std::runtime_error(
          "active track definition has no authored background");
    }
    const auto loading_background =
        mh::content::read_tga(content_root / "Data" / "Loading.TGA");
    const auto loading_animation =
        mh::content::read_tga(content_root / "Data" / "LoadAnim.tga");
    const auto compact_result_background = mh::content::read_tga(
        content_root / "Data" /
        (league_result_style ? "lresults2.TGA" : "results2.TGA"));
    const auto full_result_background = mh::content::read_tga(
        content_root / "Data" /
        (league_result_style ? "LResults.TGA" : "Results.TGA"));
    const auto result_font =
        mh::content::read_fnt(content_root / "Data" / "FONT1.FNT");
    const auto player_portrait =
        mh::content::read_tga(content_root / "Game" / "player.tga");
    auto player_name = identity_player_name;
    auto player_colour = configured_player_colour.value_or(parse_config_rgb(
        read_motorhead_config_value(configuration_path, "PlayerColour"),
        "motorhead.cfg PlayerColour"));
    // Team and short-key values are retained at the playable-session boundary.
    // Their consumers are the multiplayer chat/session layer, which is outside
    // this single-player race host, but the front end must not silently drop
    // them.
    const auto player_team_name = identity_team_name;
    std::array<std::string, 10U> player_short_keys{};
    for (std::size_t slot = 0U; slot < player_short_keys.size(); ++slot) {
      player_short_keys[slot] =
          configured_short_keys[slot].value_or(std::string{});
    }
    static_cast<void>(player_team_name);
    static_cast<void>(player_short_keys);
    std::optional<mh::content::LeagueDefinition> selected_league;
    if (league_definition_path.has_value()) {
      selected_league = mh::content::read_league(*league_definition_path);
      const auto field = mh::content::league_race_field(*selected_league);
      const auto &human = field.front();
      const auto division = std::min<std::size_t>(
          human.division, human.division_cars.size() - 1U);
      if (ascii_lower(human.division_cars[division]) != ascii_lower(car.name)) {
        throw std::runtime_error(
            "selected league human car does not match the active car");
      }
      player_name = human.name;
      player_colour = {human.color.red, human.color.green, human.color.blue};
    }
    const auto motion =
        mh::content::read_motion(std::filesystem::path(argv[3]));
    const auto route =
        mh::content::read_ai_route(std::filesystem::path(argv[4]));
    std::array<std::optional<std::string>, 2U> start_motion_references;
    for (const auto &reference : track_definition.references) {
      if (reference.field == "startspline1") {
        start_motion_references[0U] = reference.value;
      } else if (reference.field == "startspline2") {
        start_motion_references[1U] = reference.value;
      }
    }
    if (!start_motion_references[0U].has_value() ||
        !start_motion_references[1U].has_value()) {
      throw std::runtime_error(
          "active track definition has no complete start-camera pair");
    }
    const auto start_frame = mh::content::motion_path_frame(motion, 0U);
    const auto start_flyby =
        AuthoredStartFlyby{{
                  mh::content::read_motion(resolve_relative_case_insensitive(
                      track_root, *start_motion_references[0U])),
                  mh::content::read_motion(resolve_relative_case_insensitive(
                      track_root, *start_motion_references[1U])),
              }};
    if (!track_definition.start_grid.has_value()) {
      throw std::runtime_error(
          "active track definition has no authored start grid");
    }
    const auto canonical_resonic_goldbridge =
        ascii_lower(track_definition.name) == "goldbridge" &&
        ascii_lower(car.name) == "resonic" && !ghost_demo.has_value() &&
!selected_league.has_value();
    const auto canonical_opening_fixture =
        canonical_resonic_goldbridge &&
        (ai_control_report_path.has_value() || ai_physics_capture_oracle);
    const auto captured_local_race_roster_grid =
        !ghost_demo.has_value() && !selected_league.has_value() &&
        (live_opponents_requested || canonical_resonic_goldbridge);
    mh::game::OriginalAiRandomState opponent_random{
        ai_random_seed.value_or(mh::game::original_ai_runtime_time_seed())};
    std::optional<mh::content::LeagueDivisionFinishingOrders>
        league_division_finishing_orders;
    std::optional<std::uint32_t> active_league_division;
    if (selected_league.has_value()) {
      const auto human = std::find_if(
          selected_league->players.begin(), selected_league->players.end(),
          [](const mh::content::LeaguePlayer &player) { return player.human; });
      if (human == selected_league->players.end() || human->division > 3U) {
        throw std::runtime_error(
            "selected league has no bounded human division");
      }
      active_league_division = human->division;
      league_division_finishing_orders.emplace();
      for (std::size_t division = 0U;
           division < league_division_finishing_orders->size(); ++division) {
        if (division == *active_league_division) {
          continue;
        }
        std::vector<std::string> authored_names;
        for (const auto &player : selected_league->players) {
          if (player.division == division) {
            authored_names.push_back(player.name);
          }
        }
        const auto positions = mh::game::original_league_position_permutation(
            authored_names.size(), opponent_random);
        auto &finishing_order = (*league_division_finishing_orders)[division];
        finishing_order.resize(authored_names.size());
        for (std::size_t player = 0U; player < authored_names.size();
             ++player) {
          finishing_order[positions[player] - 1U] = authored_names[player];
        }
      }
    }
    std::optional<std::vector<mh::content::AiDriverProfile>>
        generated_driver_catalog;
    std::optional<std::array<std::size_t, 8U>> generated_driver_indices;
    std::optional<std::array<std::size_t, 8U>> generated_local_grid;
    if (captured_local_race_roster_grid && !canonical_opening_fixture) {
      generated_driver_catalog = mh::content::read_ai_driver_profile_catalog(
          content_root / "League" / "Profiles");
      generated_driver_indices = mh::game::original_local_race_driver_indices(
          *generated_driver_catalog, opponent_random);
      generated_local_grid =
          mh::game::original_local_race_grid_permutation(opponent_random);
      if (single_race_cpu_profile) {
        mh::game::original_single_race_player_overlay(*generated_driver_indices,
                                                      *generated_local_grid);
      }
    }
    const auto local_grid_slot = [&](const std::size_t live_slot) {
      if (generated_local_grid.has_value()) {
        return (*generated_local_grid)[live_slot];
      }
      return single_race_cpu_profile
                 ? mh::game::original_captured_single_race_grid_slot(live_slot)
                 : mh::game::original_goldbridge_quick_race_grid_slot(
                       live_slot);
    };
    const auto networked_race =
        multiplayer_session != nullptr &&
        multiplayer_session->state() == mh::network::SessionState::racing;
    const auto network_grid_slot = [&](const std::uint8_t peer) {
      const auto player =
          std::find_if(multiplayer_session->players().begin(),
                       multiplayer_session->players().end(),
                       [peer](const mh::network::LobbyPlayer &candidate) {
                         return candidate.peer == peer;
                       });
      if (player == multiplayer_session->players().end() ||
          player->grid_position == 0U ||
          player->grid_position > mh::network::maximum_players) {
        throw std::runtime_error(
            "multiplayer player has no authoritative grid position");
      }
      return static_cast<std::size_t>(player->grid_position - 1U);
    };
    constexpr std::size_t single_player_start_slot = 0U;
    const auto player_grid_slot =
        networked_race ? network_grid_slot(multiplayer_session->local_peer())
        : captured_local_race_roster_grid
            ? local_grid_slot(single_player_start_slot)
            : single_player_start_slot;
    const auto start_pose =
        mh::game::make_original_track_pose(
                  start_frame,
                  mh::game::make_original_start_grid_offsets(
                      *track_definition.start_grid, player_grid_slot));
    // Checkpoint events are encoded directly in SplineName key rotation Y.
    // The p3.1 race owner rounds (channel + 1) at RVA 0x0003bbae and accepts
    // the event when its retained projection time crosses that authored key.
    const auto materials_reference = std::find_if(
        track_definition.references.begin(), track_definition.references.end(),
        [](const auto &reference) {
          return reference.field == "materialsname";
        });
    if (materials_reference == track_definition.references.end()) {
      throw std::runtime_error(
          "active track definition has no materials reference");
    }
    const auto grounded_materials =
        mh::game::make_original_track_grounded_material_table(
            content_root / "Data" / "Material.mat",
            resolve_relative_case_insensitive(content_root,
                                              materials_reference->value));
    const auto player_wheel_response_profile =
        pc_cheats.active(mh::game::OriginalPcCheat::mega_springs)
            ? mh::game::OriginalWheelResponseProfile::demon_grem
        : pc_cheats.active(mh::game::OriginalPcCheat::la_suspension)
            ? mh::game::OriginalWheelResponseProfile::g_ride_west
            : mh::game::OriginalWheelResponseProfile::standard;
    auto player_response_config =
        mh::game::make_original_vehicle_response_config(
            car, 5.0, player_wheel_response_profile);
    player_response_config.grounded_materials = grounded_materials;
    mh::game::OriginalVehicleRuntime vehicle(
        make_track_collision_world(), std::move(player_response_config),
        mh::game::make_original_vehicle_body_hull_rig(car),
        mh::game::make_recovered_vehicle_runtime_tuning(car), start_pose, 0.0);
    vehicle.set_automatic_transmission(automatic_transmission);
    // The identity byte selects the Demon/Grem and G-Ride/West suspension
    // profiles while the response configuration is built above. Do not then
    // reuse those selector bits as live grounded-body flags: bit 0x01 makes
    // that separate owner clear horizontal velocity at every wheel contact.
    // Of the restored PC identities, only Buzz Aldrin/NASA's bit 0x02 belongs
    // to the live vertical-scale owner.
    const auto live_vehicle_global_flags =
        pc_cheats.active(mh::game::OriginalPcCheat::moon_gravity)
            ? static_cast<std::uint8_t>(mh::game::OriginalPcCheat::moon_gravity)
            : std::uint8_t{0U};
    vehicle.set_original_global_flags(live_vehicle_global_flags);
    constexpr std::size_t grid_settlement_ticks = 600U;
    constexpr double grid_settlement_slice_seconds = 0.01;
    const auto settle_grid_vehicle =
        [=](mh::game::OriginalVehicleRuntime &staged_vehicle) {
          for (std::size_t tick = 0U; tick < grid_settlement_ticks; ++tick) {
            staged_vehicle.settle_suspension_on_grid(
                grid_settlement_slice_seconds);
          }
        };
    const auto seed_captured_body =
        [](mh::game::OriginalVehicleRuntime &staged_vehicle,
           const mh::game::OriginalQuickRaceOpeningPhysicsState &captured) {
          mh::game::OriginalBodyPoseState pose;
          mh::game::OriginalBodyVelocityState velocity;
          mh::game::OriginalBodyVelocityState previous_velocity;
          for (std::size_t axis = 0U; axis < 3U; ++axis) {
            pose.world_position[axis] =
                static_cast<double>(captured.body_position[axis]);
            velocity.local_linear[axis] =
                static_cast<double>(captured.local_linear_velocity[axis]);
            velocity.local_angular[axis] =
                static_cast<double>(captured.local_angular_velocity[axis]);
            previous_velocity.local_linear[axis] = static_cast<double>(
                captured.previous_local_linear_velocity[axis]);
            previous_velocity.local_angular[axis] = static_cast<double>(
                captured.previous_local_angular_velocity[axis]);
            for (std::size_t component = 0U; component < 3U; ++component) {
              pose.body_basis[axis][component] =
                  static_cast<double>(captured.body_basis[axis][component]);
            }
          }
          staged_vehicle.seed_captured_body_state(pose, velocity,
                                                  previous_velocity);
          staged_vehicle.seed_wheel_contact_history(
              captured.previous_wheel_states);
        };
    settle_grid_vehicle(vehicle);
    if (ai_physics_capture_oracle) {
      const auto &captured =
          mh::game::original_goldbridge_quick_race_opening_physics_states()[0U];
      if (captured.slot_index != 0U) {
        throw std::runtime_error(
            "captured opening physics player slot ordering changed");
      }
      seed_captured_body(vehicle, captured);
    } else if (canonical_opening_fixture) {
      const auto &opening_wheels =
          mh::game::original_goldbridge_quick_race_opening_wheel_states();
      vehicle.seed_wheel_contact_history(
          opening_wheels[0U].previous_wheel_states);
    }
    struct CanonicalOpponent {
      std::size_t slot_index = 0U;
      mh::content::CarDefinition car;
      std::unique_ptr<mh::game::OriginalVehicleRuntime> vehicle;
      std::unique_ptr<mh::game::OriginalAiVehicleController> controller;
      CarVisual visual;
      mh::game::OriginalAiControlOutput controls{};
      mh::game::ControlInput presentation_controls{};
      WheelVisualState wheels{};
      mh::game::OriginalDynamicVehicleContactShape contact_shape{};
      SkidMarkEmitter skid_marks{};
      TireSmokeEmitter tire_smoke{};
      SparkEmitter sparks{};
      mh::game::OriginalVehicleModeCSceneFrame physics_frame{};
      std::array<float, 4U> retained_wheel_states{};
      std::array<std::uint32_t, 4U> retained_wheel_contact_flags{};
      std::array<mh::game::OriginalBodyVelocityState, 3U>
          preceding_response_velocities{};
      std::array<mh::game::BodyForceAccumulator, 3U>
          preceding_accumulated_forces{};
      std::array<mh::game::OriginalVehicleResponseFrame, 3U>
          preceding_response_frames{};
      std::array<mh::game::OriginalBodyPoseState, 3U>
          preceding_post_pose_states{};
      std::array<mh::game::OriginalBodyVelocityState, 3U>
          preceding_post_pose_velocities{};
      std::optional<mh::game::OriginalVehicleGroundedDampingResult>
          preceding_grounded_damping;
      std::optional<mh::game::OriginalDrivetrainModeCForceResult>
          preceding_drivetrain;
      std::size_t preceding_body_hull_reaction_count = 0U;
      std::vector<AiOpeningDynamicContactRecord> preceding_dynamic_contacts;
      mh::game::OriginalBodyPoseState presentation_pose{};
    };
    std::vector<CanonicalOpponent> opponents;
    const auto normal_live_opponent_race = captured_local_race_roster_grid;
    std::vector<LiveRaceRosterSlot> race_roster;
    float cpu_catch_up_percent = 0.0F;
    std::optional<std::size_t> league_cpu_profile_selector;
    if (selected_league.has_value()) {
      const auto field = mh::content::load_league_race_roster(
          *selected_league, content_root / "League" / "Profiles");
      const auto &human = field.front();
      const auto division = static_cast<std::size_t>(human.player.division);
      league_cpu_profile_selector = division;
      race_roster.push_back({human.player.name,
                             human.active_car,
                             "player.tga",
                             {human.player.color.red, human.player.color.green,
                              human.player.color.blue},
                             human.player.score,
                             std::nullopt});
      for (const auto &league_slot :
           std::span<const mh::content::LeagueRaceRosterSlot>(field).subspan(
               1U)) {
        const auto &league_player = league_slot.player;
        const auto &driver_profile = *league_slot.ai_profile;
        race_roster.push_back(
            {league_player.name,
             league_slot.active_car,
             driver_profile.player_picture,
             {league_player.color.red, league_player.color.green,
              league_player.color.blue},
             league_player.score,
             std::array<float, 2U>{driver_profile.aggressiveness,
                                   driver_profile.eagerness}});
      }
    } else if (normal_live_opponent_race) {
      race_roster.reserve(8U);
      if (generated_driver_catalog.has_value() &&
          generated_driver_indices.has_value()) {
        const auto driver_car_index =
            mh::game::original_local_race_driver_car_index(car.division);
        race_roster.push_back(
            {player_name,
             car.name,
             "player.tga",
             {player_colour.red, player_colour.green, player_colour.blue},
             0U,
             std::nullopt});
        for (std::size_t slot = 1U; slot < 8U; ++slot) {
          const auto &driver =
              (*generated_driver_catalog)[(*generated_driver_indices)[slot]];
          race_roster.push_back(
              {driver.player_nick,
               driver.division_cars[driver_car_index],
               driver.player_picture,
               {driver.player_color.red, driver.player_color.green,
                driver.player_color.blue},
               0U,
               std::array<float, 2U>{driver.aggressiveness, driver.eagerness}});
        }
      } else {
        const auto &captured_roster =
            mh::game::original_goldbridge_quick_race_roster();
        for (const auto &slot : captured_roster) {
          race_roster.push_back(
              {slot.slot_index == 0U ? player_name
                                     : std::string(slot.driver_nick),
               slot.slot_index == 0U ? car.name : std::string(slot.car_name),
               std::string(slot.portrait_name),
               slot.slot_index == 0U
                   ? std::array<std::uint8_t, 3U>{player_colour.red,
                                                  player_colour.green,
                                                  player_colour.blue}
                   : slot.driver_color,
               0U, std::nullopt});
        }
      }
    } else {
      race_roster.push_back(
          {player_name,
           car.name,
           "player.tga",
           {player_colour.red, player_colour.green, player_colour.blue},
           0U,
           std::nullopt});
    }
    std::vector<std::uint8_t> network_peer_by_slot;
    std::vector<bool> network_automatic_gear_by_slot;
    if (networked_race) {
      race_roster.clear();
      const auto local_peer = multiplayer_session->local_peer();
      const auto append_network_player =
          [&](const mh::network::LobbyPlayer &player, const bool local) {
            race_roster.push_back(
                {player.name,
                 local ? car.name
                       : car_name_by_original_index(content_root,
                                                    player.car_selection),
                 "player.tga",
                 local ? std::array<std::uint8_t, 3U>{player_colour.red,
                                                      player_colour.green,
                                                      player_colour.blue}
                       : std::array<std::uint8_t, 3U>{255U, 255U, 255U},
                 0U, std::nullopt});
            network_peer_by_slot.push_back(player.peer);
            network_automatic_gear_by_slot.push_back(player.automatic_gear);
          };
      const auto local =
          std::find_if(multiplayer_session->players().begin(),
                       multiplayer_session->players().end(),
                       [local_peer](const mh::network::LobbyPlayer &player) {
                         return player.peer == local_peer;
                       });
      if (local == multiplayer_session->players().end()) {
        throw std::runtime_error("multiplayer roster omits the local player");
      }
      append_network_player(*local, true);
      for (const auto &player : multiplayer_session->players()) {
        if (player.peer != local_peer) {
          append_network_player(player, false);
        }
      }
    }
    if (one_make_opponents) {
      for (auto &slot :
           std::span<LiveRaceRosterSlot>(race_roster).subspan(1U)) {
        slot.car_name = car.name;
      }
      if (race_roster.size() > 1U) {
        mh::common::log_runtime_info("One-make opponent field: " +
                                     std::to_string(race_roster.size() - 1U) +
                                     " opponents use " + car.name);
      }
    }
    const auto &cpu_profile =
        [&]() -> const mh::game::OriginalCpuRaceProfile & {
      if (league_cpu_profile_selector.has_value()) {
        const auto index = mh::game::original_cpu_race_profile_index(
            mh::game::OriginalCpuRaceProfileFamily::league,
            *league_cpu_profile_selector);
        return mh::game::original_cpu_race_profiles()[index];
      }
      if (single_race_cpu_profile) {
        return mh::game::original_single_race_cpu_profiles()
            [quick_race_difficulty];
      }
      const auto index = mh::game::original_cpu_race_profile_index(
          mh::game::OriginalCpuRaceProfileFamily::quick_race,
          quick_race_difficulty);
      return mh::game::original_cpu_race_profiles()[index];
    }();
    if (race_roster.size() > 1U) {
      const auto &initializer =
          mh::game::original_goldbridge_quick_race_ai_initializer_slots();
      cpu_catch_up_percent =
          cpu_catch_up_enabled ? cpu_profile.catch_up_percent : 0.0F;
      opponents.reserve(race_roster.size() - 1U);
      for (std::size_t slot = 1U; slot < race_roster.size(); ++slot) {
        auto opponent_car =
            one_make_opponents
                ? car
                : mh::content::read_car(find_car_definition_by_name(
                      content_root, race_roster[slot].car_name));
        apply_playable_car_tuning(opponent_car);
        const auto opponent_grid_slot =
            networked_race ? network_grid_slot(network_peer_by_slot.at(slot))
            : captured_local_race_roster_grid ? local_grid_slot(slot)
                                              : slot;
        auto opponent_pose = mh::game::make_original_track_pose(
            start_frame, mh::game::make_original_start_grid_offsets(
                             *track_definition.start_grid, opponent_grid_slot));
        if (ai_physics_capture_oracle) {
          const auto &captured = mh::game::
              original_goldbridge_quick_race_opening_physics_states()[slot];
          if (captured.slot_index != slot) {
            throw std::runtime_error(
                "captured opening physics slot ordering changed");
          }
          opponent_pose.world_position = {
              static_cast<double>(captured.body_position[0U]),
              static_cast<double>(captured.body_position[1U]),
              static_cast<double>(captured.body_position[2U])};
          for (std::size_t axis = 0U; axis < 3U; ++axis) {
            for (std::size_t component = 0U; component < 3U; ++component) {
              opponent_pose.body_basis[axis][component] =
                  static_cast<double>(captured.body_basis[axis][component]);
            }
          }
        } else if (canonical_opening_fixture) {
          const auto &captured_pose =
              mh::game::original_goldbridge_quick_race_opening_ai_poses()[slot -
                                                                          1U];
          if (captured_pose.slot_index != slot) {
            throw std::runtime_error(
                "canonical opening AI pose slot ordering changed");
          }
          opponent_pose.world_position[0U] = captured_pose.vehicle_x;
          opponent_pose.world_position[2U] = captured_pose.vehicle_z;
          opponent_pose.body_basis = {{
              {captured_pose.vehicle_forward_z, 0.0,
               -captured_pose.vehicle_forward_x},
              {0.0, 1.0, 0.0},
              {captured_pose.vehicle_forward_x, 0.0,
               captured_pose.vehicle_forward_z},
          }};
        }
        auto opponent_response_config =
            mh::game::make_original_vehicle_response_config(opponent_car, 5.0);
        opponent_response_config.grounded_materials = grounded_materials;
        auto opponent_drive_tuning =
            ai_physics_capture_oracle
                ? mh::game::original_goldbridge_quick_race_runtime_drive_tuning(
                      slot)
                : mh::game::original_cpu_race_profile_drive_tuning(opponent_car,
                                                                   cpu_profile);
        auto opponent_vehicle =
            std::make_unique<mh::game::OriginalVehicleRuntime>(
                mh::game::make_collision_world(collision),
                std::move(opponent_response_config),
                mh::game::make_original_vehicle_body_hull_rig(opponent_car),
                std::move(opponent_drive_tuning), opponent_pose, 0.0);
        if (networked_race) {
          opponent_vehicle->set_automatic_transmission(
              network_automatic_gear_by_slot.at(slot));
        }
        settle_grid_vehicle(*opponent_vehicle);
        if (ai_physics_capture_oracle) {
          const auto &captured = mh::game::
              original_goldbridge_quick_race_opening_physics_states()[slot];
          seed_captured_body(*opponent_vehicle, captured);
        } else if (canonical_opening_fixture) {
          const auto &captured_wheels = mh::game::
              original_goldbridge_quick_race_opening_wheel_states()[slot];
          if (captured_wheels.slot_index != slot) {
            throw std::runtime_error(
                "canonical opening wheel-state slot ordering changed");
          }
          opponent_vehicle->seed_wheel_contact_history(
              captured_wheels.previous_wheel_states);
        }
        std::array<float, 4U> retained_wheel_states{};
        std::array<std::uint32_t, 4U> retained_wheel_contact_flags{};
        if (ai_physics_capture_oracle) {
          const auto &captured = mh::game::
              original_goldbridge_quick_race_opening_physics_states()[slot];
          retained_wheel_states = captured.previous_wheel_states;
          retained_wheel_contact_flags = {1U, 1U, 1U, 1U};
        } else if (canonical_opening_fixture) {
          const auto &captured_wheels = mh::game::
              original_goldbridge_quick_race_opening_wheel_states()[slot];
          retained_wheel_states = captured_wheels.previous_wheel_states;
          retained_wheel_contact_flags = {1U, 1U, 1U, 1U};
        } else {
          retained_wheel_states =
              settled_wheel_visual_states(*opponent_vehicle);
          retained_wheel_contact_flags = {1U, 1U, 1U, 1U};
        }
        WheelVisualState opponent_wheels;
        opponent_wheels.suspension_states = retained_wheel_states;
        const auto opponent_ai_pose = opponent_vehicle->ai_pose();
        const std::array<float, 2U> initial_xz{
            static_cast<float>(opponent_ai_pose.world_position[0U]),
            static_cast<float>(opponent_ai_pose.world_position[2U])};
        auto opponent_tuning = initializer[slot].tuning;
        if (!ai_physics_capture_oracle) {
          opponent_tuning.runtime_speed_scale = cpu_profile.global_speed_scale;
          opponent_tuning.runtime_group_scale = cpu_profile.route_group_scale;
        }
        if (!opponent_car.physics.has_value()) {
          throw std::runtime_error("opponent CAR omits its physics definition");
        }
        opponent_tuning.route_spacing_control =
            mh::game::original_ai_route_spacing_control(
                opponent_car.physics->weight);
        if (race_roster[slot].ai_profile_percentages.has_value()) {
          const auto &profile = *race_roster[slot].ai_profile_percentages;
          opponent_tuning = mh::game::original_ai_apply_driver_profile(
              opponent_tuning, profile[0U], profile[1U]);
        }
        auto opponent_controller =
            std::make_unique<mh::game::OriginalAiVehicleController>(
                route, opponent_tuning, initial_xz, opponent_random);
        if (!canonical_opening_fixture) {
          opponent_controller->set_lateral_preference(
              mh::game::ai_competitor_lateral_preference(slot));
        }
        if (canonical_opening_fixture) {
          opponent_controller->initialize_lateral_target(
              initializer[slot].captured_initial_lateral_target);
        } else {
          opponent_controller->initialize_lateral_target(
              opponent_vehicle->state().pose);
        }
        const auto opponent_collision =
            mh::content::read_col(resolve_relative_case_insensitive(
                content_root, opponent_car.collision_path));
        const auto opponent_contact_shape =
            mh::game::make_original_dynamic_vehicle_contact_shape(
                opponent_car, opponent_collision);
        opponents.push_back({slot,
                             std::move(opponent_car),
                             std::move(opponent_vehicle),
                             std::move(opponent_controller),
                             {},
                             {},
                             {},
                             opponent_wheels,
                             opponent_contact_shape,
                             {},
                             {},
                             {},
                             {},
                             retained_wheel_states,
                             retained_wheel_contact_flags,
                             {},
                             {},
                             {},
                             {},
                             {},
                             std::nullopt,
                             std::nullopt,
                             0U,
                             {}});
      }
    }
    if (ai_control_report_path.has_value() &&
        (!canonical_resonic_goldbridge || opponents.size() != 7U)) {
      throw std::invalid_argument("AI control reporting requires canonical "
                                  "Resonic Goldbridge Quick Race");
    }
    struct AiOpeningContactRecord {
      std::size_t slot = 0U;
      mh::game::OriginalBodyPoseState physics_pose{};
      std::array<mh::game::WheelSpringSegmentSample, 4U> wheels{};
    };
    std::vector<AiOpeningContactRecord> ai_opening_contacts;
    if (ai_control_report_path.has_value()) {
      ai_opening_contacts.reserve(8U);
      ai_opening_contacts.push_back(
          {0U, vehicle.physics_pose(), vehicle.current_wheel_contacts()});
      for (const auto &opponent : opponents) {
        ai_opening_contacts.push_back(
            {opponent.slot_index, opponent.vehicle->physics_pose(),
             opponent.vehicle->current_wheel_contacts()});
      }
    }
    std::vector<mh::content::TgaImage> result_portraits;
    result_portraits.reserve(race_roster.size());
    result_portraits.push_back(player_portrait);
    if (race_roster.size() > 1U) {
      const auto portrait_root = content_root / "League" / "Profiles" / "Gfx";
      for (std::size_t slot = 1U; slot < race_roster.size(); ++slot) {
        if (ascii_lower(race_roster[slot].portrait_name) == "player.tga") {
          // Network players own their local Game/player.tga; it is not a league
          // CPU portrait and therefore must never be resolved below Profiles.
          result_portraits.push_back(player_portrait);
        } else {
          result_portraits.push_back(
              mh::content::read_tga(find_sibling_case_insensitive(
                  portrait_root, race_roster[slot].portrait_name)));
        }
      }
    }
    const auto player_collision = mh::content::read_col(
        resolve_relative_case_insensitive(content_root, car.collision_path));
    const auto player_contact_shape =
        mh::game::make_original_dynamic_vehicle_contact_shape(car,
                                                              player_collision);
    const auto player_ai_pose = vehicle.ai_pose();
    const std::array<float, 2U> player_initial_xz{
        static_cast<float>(player_ai_pose.world_position[0U]),
        static_cast<float>(player_ai_pose.world_position[2U])};
    mh::game::OriginalAiRouteCursor player_route_cursor(route,
                                                        player_initial_xz);
    auto post_finish_player_tuning =
        mh::game::make_playable_post_finish_player_tuning(
            car.physics->weight, cpu_profile,
            static_cast<float>(player_contact_shape.half_width),
            static_cast<float>(player_contact_shape.half_length));
    mh::game::OriginalAiVehicleController post_finish_player_controller(
        route, post_finish_player_tuning, player_initial_xz, opponent_random);
    post_finish_player_controller.initialize_lateral_target(vehicle.ai_pose());

    if (shared_renderer != nullptr && shared_window == nullptr) {
      throw std::invalid_argument(
          "a shared race renderer requires its owning window");
    }
    const auto hosted_race =
        shared_window != nullptr && shared_renderer == nullptr;
    // Exclusive D3D9 devices require a top-level focus window. Keep the
    // overlay used by windowed and borderless races, but render exclusive
    // fullscreen directly into the persistent front-end window.
    const auto hosted_overlay =
        hosted_race &&
        initial_window_mode != mh::ui::GraphicWindowMode::fullscreen;
    const auto owns_window = shared_window == nullptr || hosted_overlay;
    const auto owns_renderer = shared_renderer == nullptr;
    const auto shared_graphics = shared_renderer != nullptr;
    const auto owns_sdl_runtime =
        shared_window == nullptr && !reuse_initialized_sdl;
    if (owns_sdl_runtime) {
      require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK |
                       SDL_INIT_GAMEPAD),
              "initialize SDL3");
    }
    struct ConditionalSdlGuard {
      bool active = false;
      ~ConditionalSdlGuard() {
        if (active) {
          SDL_Quit();
        }
      }
    } sdl_guard{owns_sdl_runtime};
    const std::string race_window_title = "Motorhead";
    // Resource identifier 1 is the original icon embedded by motorhead.rc;
    // only a standalone race window registers a fresh window class, but the
    // hint is harmless when the front end shares its window.
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
    std::unique_ptr<SDL_Window,
                    ConditionalSdlDestroy<SDL_Window, SDL_DestroyWindow>>
        window(
            hosted_overlay
                ? create_hosted_race_window(shared_window, race_window_title,
                                            window_width, window_height)
            : shared_window != nullptr
                ? shared_window
                : SDL_CreateWindow(
                      race_window_title.c_str(), window_width, window_height,
                      SDL_WINDOW_RESIZABLE |
                          (hidden_window ? SDL_WINDOW_HIDDEN : 0U)),
            ConditionalSdlDestroy<SDL_Window, SDL_DestroyWindow>{owns_window});
    require(window != nullptr, "create window");
    if (hosted_overlay) {
      // The front end owns the persistent top-level window.  Native hardware
      // backends use a temporary child overlay only because SDL cannot replace
      // that window's renderer in place.  Fullscreen state therefore belongs
      // to the parent; applying it to the child makes the overlay disappear on
      // a borderless/fullscreen -> windowed transition and exposes the frozen
      // front-end frame below it.
      apply_race_window_mode(shared_window, initial_window_mode, window_width,
                             window_height);
      align_hosted_race_window(shared_window, window.get(), window_width,
                               window_height);
    } else {
      apply_race_window_mode(window.get(), initial_window_mode, window_width,
                             window_height);
    }
    SDL_Renderer *raw_renderer = shared_renderer;
    if (raw_renderer == nullptr) {
      raw_renderer = create_race_renderer(window.get(), renderer_backend);
    }
    std::unique_ptr<SDL_Renderer,
                    ConditionalSdlDestroy<SDL_Renderer, SDL_DestroyRenderer>>
        renderer(raw_renderer,
                 ConditionalSdlDestroy<SDL_Renderer, SDL_DestroyRenderer>{
                     owns_renderer});
    require(renderer != nullptr, "create renderer");
    if (const auto *driver = SDL_GetRendererName(renderer.get());
        driver != nullptr) {
      mh::common::log_runtime_info(std::string("Active SDL renderer: ") +
                                   driver);
    }
    require(SDL_SetRenderColorScale(renderer.get(), display_brightness),
            "apply Graphic Options brightness");
    require(SDL_SetWindowTitle(window.get(), race_window_title.c_str()),
            "set race window title");
    if (hosted_overlay) {
      require(SDL_SetWindowTitle(shared_window, race_window_title.c_str()),
              "set hosted race title");
      require(SDL_ShowWindow(window.get()), "show hosted race overlay");
      require(SDL_RaiseWindow(window.get()), "focus hosted race overlay");
    }
    // Replay samples are evaluated from wall time, so an unpaced immediate
    // swap exposes every variation in the CPU raster workload as visible
    // motion judder.  Keep the same one-refresh presentation clock used by a
    // playable race.  The replay-specific renderer work is now below the
    // refresh budget; adaptive/immediate presentation is therefore neither
    // necessary nor correct here.
    require(SDL_SetRenderVSync(renderer.get(), benchmark_presentation ? 0 : 1),
            benchmark_presentation ? "disable vertical sync for benchmark"
                                   : "enable vertical sync");
    RendererVsyncRestore renderer_vsync_restore{
        renderer.get(), shared_graphics && benchmark_presentation};
    require(SDL_SetRenderDrawBlendMode(renderer.get(), SDL_BLENDMODE_BLEND),
            "enable distance-cue blending");
    const auto original_texture_format_code = renderer_texture_format == 8U ? 1U
                                              : renderer_texture_format == 16U
                                                  ? 3U
                                                  : 9U;
    const auto original_texture_red_mask =
        renderer_texture_format == 16U   ? 0x0000f800U
        : renderer_texture_format == 32U ? 0x00ff0000U
                                         : 0U;
    const auto original_texture_green_mask =
        renderer_texture_format == 16U   ? 0x000007e0U
        : renderer_texture_format == 32U ? 0x0000ff00U
                                         : 0U;
    const auto original_texture_blue_mask =
        renderer_texture_format == 16U   ? 0x0000001fU
        : renderer_texture_format == 32U ? 0x000000ffU
                                         : 0U;
    if (renderer_settings_report_path.has_value()) {
      if (!renderer_settings_report_path->parent_path().empty()) {
        std::filesystem::create_directories(
            renderer_settings_report_path->parent_path());
      }
      std::ofstream report(*renderer_settings_report_path,
                           std::ios::binary | std::ios::trunc);
      if (!report) {
        throw std::runtime_error("could not create renderer settings report: " +
                                 renderer_settings_report_path->string());
      }
      const auto *driver = SDL_GetRendererName(renderer.get());
      report << "{\n"
             << "  \"schema\": \"motorhead.renderer-settings-handoff.v1\",\n"
             << "  \"renderer_driver\": \""
             << (driver == nullptr ? "unknown" : driver) << "\",\n"
             << "  \"scene_renderer_backend\": \""
             << renderer_backend_token(renderer_backend) << "\",\n"
             << "  \"width\": " << window_width << ",\n"
             << "  \"height\": " << window_height << ",\n"
             << "  \"true_colour\": "
             << (renderer_true_colour ? "true" : "false") << ",\n"
             << "  \"triple_buffer\": "
             << (renderer_triple_buffer ? "true" : "false") << ",\n"
             << "  \"trilinear_filtering\": "
             << (renderer_trilinear_filtering ? "true" : "false") << ",\n"
             << "  \"detail_mode\": " << static_cast<unsigned>(detail_mode)
             << ",\n"
             << "  \"texture_format_bits\": " << renderer_texture_format
             << ",\n"
             << "  \"original_texture_format_code\": "
             << original_texture_format_code << ",\n"
             << "  \"texture_red_mask\": " << original_texture_red_mask << ",\n"
             << "  \"texture_green_mask\": " << original_texture_green_mask
             << ",\n"
             << "  \"texture_blue_mask\": " << original_texture_blue_mask
             << ",\n"
             << "  \"texture_mip_levels\": "
             << (renderer_trilinear_filtering ? 3U : 1U) << ",\n"
             << "  \"texture_magnification_filter\": \"linear\",\n"
             << "  \"texture_minification_filter\": \""
             << (renderer_trilinear_filtering ? "linear-mip-linear" : "linear")
             << "\",\n"
             << "  \"requested_display_bits\": "
             << (renderer_true_colour ? 32U : 16U) << ",\n"
             << "  \"display_red_mask\": "
             << (renderer_true_colour ? 0x00ff0000U : 0x0000f800U) << ",\n"
             << "  \"display_green_mask\": "
             << (renderer_true_colour ? 0x0000ff00U : 0x000007e0U) << ",\n"
             << "  \"display_blue_mask\": "
             << (renderer_true_colour ? 0x000000ffU : 0x0000001fU) << ",\n"
             << "  \"display_surface_format\": \""
             << (renderer_true_colour ? "X8R8G8B8" : "RGB565") << "\",\n"
             << "  \"requested_back_buffer_count\": "
             << (renderer_triple_buffer ? 2U : 1U) << ",\n"
             << "  \"screen_dimension_applied\": true,\n"
             << "  \"texture_surface_semantics_applied\": true,\n"
             << "  \"texture_filter_semantics_applied\": true,\n"
             << "  \"display_surface_semantics_applied\": true,\n"
             << "  \"flip_chain_semantics_applied\": true,\n"
             << "  \"replacement_surface_semantics_applied\": true,\n"
             << "  \"guard_reason\": \"\"\n"
             << "}\n";
      if (!report) {
        throw std::runtime_error("could not write renderer settings report: " +
                                 renderer_settings_report_path->string());
      }
    }
    RetailLoadingScreen loading_screen(renderer.get(), loading_background,
                                       loading_animation);
    loading_screen.present(0U);
    std::optional<std::array<mh::content::CarColor, 3U>> player_active_colors;
    if (custom_car_colours) {
      player_active_colors = {
          *configured_car_colours[0U],
          *configured_car_colours[1U],
          *configured_car_colours[2U],
      };
    }
    auto car_visual =
        load_car_visual(car, content_root, player_active_colors,
                        player_wheel_response_profile, car_detail);
    for (auto &opponent : opponents) {
      opponent.visual = load_car_visual(
          opponent.car, content_root, std::nullopt,
          mh::game::OriginalWheelResponseProfile::standard, car_detail);
    }
    auto recorded_car_visuals =
        ghost_demo.has_value()
            ? load_recorded_car_visuals(*ghost_demo, content_root, car_detail)
            : std::vector<RecordedCarVisual>{};
    std::vector<WheelVisualState> recorded_wheel_visuals(
        recorded_car_visuals.size());
    loading_screen.present(1U);
    SdlPointer<SDL_Texture, SDL_DestroyTexture> result_texture(
        SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_RGBA32,
                          SDL_TEXTUREACCESS_STREAMING,
                          static_cast<int>(mh::ui::front_end_logical_width),
                          static_cast<int>(mh::ui::front_end_logical_height)),
        SDL_DestroyTexture);
    require(result_texture != nullptr, "create race-results texture");
    require(SDL_SetTextureScaleMode(result_texture.get(), SDL_SCALEMODE_LINEAR),
            "set race-results scaling");
    auto world_visual =
        load_world_visual(
                  renderer.get(), world_path, base_world_path.parent_path(),
                  zero_transparent_materials, override_root, world_halo_paths);
    prepare_world_lamp_beams(world_visual, shadow_collision_world, route);
    loading_screen.present(2U);
    world_visual.vehicle_environment = load_vehicle_environment_textures(
        renderer.get(), track_definition, content_root);
    loading_screen.present(3U);
    auto background_visual = load_background(
        renderer.get(),
        resolve_relative_case_insensitive(content_root, *background_reference));
    auto race_hud = load_race_hud(renderer.get(), content_root, route);
    loading_screen.present(4U);
    bind_car_materials(renderer.get(), world_visual, car_visual);
    for (auto &opponent : opponents) {
      bind_car_materials(renderer.get(), world_visual, opponent.visual);
    }
    for (auto &recorded : recorded_car_visuals) {
      bind_car_materials(renderer.get(), world_visual, recorded.visual);
    }
    for (auto &asset : environment_scene_visual.assets) {
      bind_car_materials(renderer.get(), world_visual, asset.visual);
    }
    prepare_renderer_texture_levels(world_visual, renderer_texture_format,
                                    renderer_trilinear_filtering);
    set_visual_profile(world_visual, background_visual, enhanced_profile);
    loading_screen.present(5U);
    std::optional<AudioDevice> audio_device;
    std::optional<LoopingAudio> music;
    std::optional<RetailEngineAudio> engine_sound;
    std::optional<OpponentEngineAudio> opponent_engine_sound;
    std::optional<RaceEventAudio> race_sound;
    std::optional<HornRaceAudio> horn_sound;
    std::optional<MaterialRaceAudio> material_sound;
    std::optional<EnvironmentRaceAudio> environment_sound;
    std::optional<AudioClip> pause_menu_loop_clip;
    std::optional<LoopingAudio> pause_menu_loop;
    std::optional<OneShotAudio> pause_menu_enter;
    std::optional<OneShotAudio> pause_menu_cursor;
    std::optional<OneShotAudio> pause_menu_error;
    std::optional<mh::disc::MountedCddaDisc> mounted_cdda;
    if (music_cd_drive_path.has_value()) {
      auto discovered = mh::disc::find_mounted_motorhead_cdda();
      if (!discovered.has_value() ||
          discovered->root.root_name().string() !=
              music_cd_drive_path->root_name().string()) {
        throw std::runtime_error(
            "the selected mounted MOTORHEAD audio CD is unavailable");
      }
      mounted_cdda = std::move(discovered);
    }
    if (music_path.has_value() || music_cue_path.has_value() ||
        mounted_cdda.has_value() || engine_sound_root.has_value() ||
        race_sound_root.has_value() || horn_sound_path.has_value()) {
      audio_device.emplace(
          configuration_root.has_value()
              ? mh::ui::configuration_file_value(
                    mh::ui::motorhead_configuration_path(*configuration_root),
                    "AudioOutputDevice")
              : std::string{});
    }
    if (music_path.has_value()) {
      music.emplace(start_looping_audio(
          *music_path, audio_device->identifier, 0.22F * music_gain,
          AudioPan::center, 2000U, audio_device->specification.freq, true));
    } else if (music_cue_path.has_value()) {
      music.emplace(start_looping_audio_clip(
          load_cdda_clip(*music_cue_path, *music_track_number),
          audio_device->identifier, 0.22F, AudioPan::center, 2000U,
          audio_device->specification.freq, true));
    } else if (mounted_cdda.has_value()) {
      auto track = mh::disc::read_mounted_cdda_track_pcm(*mounted_cdda,
                                                         *music_track_number);
      AudioClip clip;
      clip.specification.format = SDL_AUDIO_S16LE;
      clip.specification.channels = static_cast<int>(track.channels);
      clip.specification.freq = static_cast<int>(track.sample_rate);
      clip.bytes = std::move(track.pcm);
      music.emplace(start_looping_audio_clip(
          std::move(clip), audio_device->identifier, 0.22F, AudioPan::center,
          2000U, audio_device->specification.freq, true));
    }
    if (engine_sound_root.has_value()) {
      engine_sound.emplace(start_retail_engine_audio(*engine_sound_root,
                                                     audio_device->identifier));
      opponent_engine_sound.emplace(start_opponent_engine_audio(
          *engine_sound_root, audio_device->identifier, opponents.size()));
    }
    if (race_sound_root.has_value()) {
      race_sound.emplace(
          start_race_event_audio(*race_sound_root, audio_device->identifier));
      material_sound.emplace(start_material_race_audio(
          *race_sound_root, audio_device->identifier, grounded_materials));
      environment_sound.emplace(start_environment_race_audio(
          *race_sound_root, audio_device->identifier,
          *environment_sound_definitions, environment_scene));
      // p3.1 pause owner initializes these exact four source WAVs at RVAs
      // 0x74d1f..0x74dd9. Menyloop is a pause-only loop; the other three are
      // the retained page interaction sounds.
      pause_menu_loop_clip.emplace(load_wav_clip(
          find_sibling_case_insensitive(*race_sound_root, "Menyloop.wav")));
      pause_menu_enter.emplace(start_one_shot_audio(
          find_sibling_case_insensitive(*race_sound_root, "Enter.wav"),
          audio_device->identifier, 1.0F));
      pause_menu_cursor.emplace(start_one_shot_audio(
          find_sibling_case_insensitive(*race_sound_root, "Cursor.wav"),
          audio_device->identifier, 1.0F));
      pause_menu_error.emplace(start_one_shot_audio(
          find_sibling_case_insensitive(*race_sound_root, "Error.wav"),
          audio_device->identifier, 1.0F));
    }
    if (horn_sound_path.has_value()) {
      horn_sound.emplace(
          start_horn_race_audio(*horn_sound_path, audio_device->identifier));
    }
    loading_screen.present(6U, loading_screenshot_path);
    const auto audio_enabled = audio_device.has_value();
    const auto music_enabled = music.has_value();
    const auto engine_enabled =
        engine_sound.has_value() && !recorded_presentation;
    const auto point_lighting_enabled = true;
    if (audio_enabled) {
      audio_device->set_enabled(true);
    }
    mh::game::EngineAudioMixState engine_audio_mix;
    auto player_engine_track_mix = 0.0;
    double environment_scene_frame = 0.0;
    WheelVisualState player_wheels;
    player_wheels.suspension_states = settled_wheel_visual_states(vehicle);
    SceneDepthResources scene_depth;
    WorldRenderScratch world_render_scratch;
    VehicleRenderScratch vehicle_render_scratch;
    scene_depth.set_backend(renderer_backend);
    scene_depth.set_requested_software_workers(renderer_workers);
    scene_depth.set_tron_hidden_line(
        pc_cheats.active(mh::game::OriginalPcCheat::tron));
    scene_depth.set_checksum_enabled(
        renderer_profile_report_path.has_value() &&
        renderer_backend == mh::render::RaceRendererBackend::software);
    PresentationBackBuffers presentation_back_buffers;
    std::vector<OpponentEngineAudioFrame> opponent_audio_frames;
    opponent_audio_frames.reserve(opponents.size());
    RendererProfileTotals renderer_profile;
    std::optional<std::chrono::steady_clock::time_point>
        renderer_profile_previous_present;
    bool scene_renderer_logged = false;
    auto active_window_mode = initial_window_mode;
    const auto presentation_period_for_active_display = [&] {
      auto presentation_rate = 60.0;
      const auto display = SDL_GetDisplayForWindow(window.get());
      const auto *display_mode =
          display != 0U ? SDL_GetCurrentDisplayMode(display) : nullptr;
      if (display_mode != nullptr &&
          std::isfinite(display_mode->refresh_rate) &&
          display_mode->refresh_rate >= 30.0F) {
        // The software renderer targets the retail 60-Hz ceiling and cannot
        // produce useful extra simulation presentations for a 120/144-Hz
        // desktop. Preserve fractional rates such as 59.94 Hz.
        presentation_rate =
            std::min(60.0, static_cast<double>(display_mode->refresh_rate));
      }
      return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          std::chrono::duration<double>(1.0 / presentation_rate));
    };
    auto stable_present_period = presentation_period_for_active_display();
    std::optional<std::chrono::steady_clock::time_point> stable_next_present;
    bool benchmark_race_started = false;
    const auto stable_presentation_active = [&] {
      // Benchmark is paced through its opening sequence, then deliberately
      // uncapped for its recorded measurement. Its D3D9 device is configured
      // once at startup because resetting it at GO is unreliable on XP-era
      // drivers.
      return !benchmark_presentation || !benchmark_race_started;
    };
    const auto pace_stable_present = [&] {
      // Benchmark deliberately measures an uncapped renderer after GO.
      // Replay and borderless gameplay use an explicit one-refresh clock
      // because SDL's software-renderer VSync request is not a reliable
      // pacing source for borderless-desktop presentation.
      if (!stable_presentation_active()) {
        return;
      }
      const auto now = std::chrono::steady_clock::now();
      if (stable_next_present.has_value() && now < *stable_next_present) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                *stable_next_present - now);
        SDL_DelayPrecise(static_cast<Uint64>(remaining.count()));
      }
    };
    const auto complete_stable_present = [&] {
      if (stable_presentation_active()) {
        const auto now = std::chrono::steady_clock::now();
        if (stable_next_present.has_value()) {
          *stable_next_present += stable_present_period;
          if (*stable_next_present < now) {
            *stable_next_present = now;
          }
        } else {
          stable_next_present = now + stable_present_period;
        }
      } else {
        stable_next_present.reset();
      }
    };
    SkidMarkEmitter player_skid_marks;
    TireSmokeEmitter player_tire_smoke;
    SparkEmitter player_sparks;
    mh::game::VehicleLaunchTractionRuntime player_launch_traction;
    double player_launch_wheelspin = 0.0;
    mh::game::OriginalVehicleSceneFrame::HullAudioRecord
        retained_player_hull_audio{};
    std::array<std::size_t, 8U> hud_vehicle_samples{};
    std::array<std::int32_t, 8U> hud_previous_samples{};
    std::array<std::int64_t, 8U> hud_progress_samples{};
    std::array<std::int64_t, 8U> catch_up_sample_progress{};
    std::array<float, 8U> catch_up_progress_by_rank{};
    std::array<std::size_t, 8U> catch_up_rank_by_slot{};
    std::array<float, 8U> hud_vehicle_speeds{};
    std::array<std::uint32_t, 8U> hud_completed_laps{};
    std::array<mh::game::SimulationDuration, 8U> hud_lap_started{};
    std::array<mh::game::SimulationDuration, 8U> hud_best_laps{};
    std::array<std::optional<mh::game::SimulationDuration>, 8U>
        hud_finish_times{};
    std::array<mh::game::ControlInput, mh::network::maximum_players>
        network_controls{};
    std::uint32_t network_simulation_tick = 0U;
    std::uint32_t network_snapshot_divider = 0U;
    std::uint32_t network_restart_generation = 0U;
    std::uint32_t network_pause_generation = 0U;
    std::uint32_t observed_network_restart_generation = 0U;
    std::uint32_t observed_network_pause_generation = 0U;
    bool network_pause_state = false;
    bool network_control_state_dirty = false;
    bool network_restart_pending = false;
    std::optional<std::vector<mh::network::RaceResultData>> network_results;
    const auto basis_to_quaternion =
        [](const mh::game::OriginalBodyPoseState &pose) {
          std::array<float, 4U> result{};
          const auto m00 = pose.body_basis[0U][0U];
          const auto m11 = pose.body_basis[1U][1U];
          const auto m22 = pose.body_basis[2U][2U];
          const auto trace = m00 + m11 + m22;
          double x = 0.0;
          double y = 0.0;
          double z = 0.0;
          double w = 1.0;
          if (trace > 0.0) {
            const auto scale = std::sqrt(trace + 1.0) * 2.0;
            w = 0.25 * scale;
            x = (pose.body_basis[2U][1U] - pose.body_basis[1U][2U]) / scale;
            y = (pose.body_basis[0U][2U] - pose.body_basis[2U][0U]) / scale;
            z = (pose.body_basis[1U][0U] - pose.body_basis[0U][1U]) / scale;
          } else if (m00 > m11 && m00 > m22) {
            const auto scale = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
            w = (pose.body_basis[2U][1U] - pose.body_basis[1U][2U]) / scale;
            x = 0.25 * scale;
            y = (pose.body_basis[0U][1U] + pose.body_basis[1U][0U]) / scale;
            z = (pose.body_basis[0U][2U] + pose.body_basis[2U][0U]) / scale;
          } else if (m11 > m22) {
            const auto scale = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
            w = (pose.body_basis[0U][2U] - pose.body_basis[2U][0U]) / scale;
            x = (pose.body_basis[0U][1U] + pose.body_basis[1U][0U]) / scale;
            y = 0.25 * scale;
            z = (pose.body_basis[1U][2U] + pose.body_basis[2U][1U]) / scale;
          } else {
            const auto scale = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
            w = (pose.body_basis[1U][0U] - pose.body_basis[0U][1U]) / scale;
            x = (pose.body_basis[0U][2U] + pose.body_basis[2U][0U]) / scale;
            y = (pose.body_basis[1U][2U] + pose.body_basis[2U][1U]) / scale;
            z = 0.25 * scale;
          }
          result = {static_cast<float>(x), static_cast<float>(y),
                    static_cast<float>(z), static_cast<float>(w)};
          return result;
        };
    const auto quaternion_to_basis = [](const std::array<float, 4U> &value) {
      const auto x = static_cast<double>(value[0U]);
      const auto y = static_cast<double>(value[1U]);
      const auto z = static_cast<double>(value[2U]);
      const auto w = static_cast<double>(value[3U]);
      return std::array<std::array<double, 3U>, 3U>{{
          {1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w),
           2.0 * (x * z + y * w)},
          {2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z),
           2.0 * (y * z - x * w)},
          {2.0 * (x * z - y * w), 2.0 * (y * z + x * w),
           1.0 - 2.0 * (x * x + y * y)},
      }};
    };
    bool hud_samples_initialized = false;
    bool catch_up_rank_initialized = false;
    std::uint32_t hud_race_position = 1U;
    auto checkpoint_display_remaining = mh::game::SimulationDuration::zero();
    std::optional<mh::game::OriginalMdePlayback> ghost_playback;
    std::uint32_t ghost_launch_milliseconds = 0U;
    std::uint32_t ghost_countdown_start_milliseconds = 0U;
    std::uint32_t ghost_countdown_hold_milliseconds = 0U;
    if (ghost_demo.has_value()) {
      ghost_playback.emplace(*ghost_demo, ghost_mode);
      const auto countdown_milliseconds = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
              start_flyby.duration())
              .count());
      const auto bounded_countdown = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(countdown_milliseconds,
                                  std::numeric_limits<std::uint32_t>::max()));
      const auto start_alignment = mh::game::make_original_mde_start_alignment(
          *ghost_demo, bounded_countdown);
      ghost_launch_milliseconds = start_alignment.go_milliseconds;
      ghost_countdown_start_milliseconds =
          start_alignment.countdown_start_milliseconds;
      ghost_countdown_hold_milliseconds =
          start_alignment.host_hold_milliseconds;
    }
    std::vector<mh::game::OriginalMdeBodyPlaybackSample> ghost_samples;
    std::optional<std::uint64_t> ghost_started_ticks;
    std::uint64_t ghost_previous_ticks = 0U;
    auto ghost_countdown_hold_remaining_milliseconds =
        ghost_countdown_hold_milliseconds;
    auto recorded_camera_body_slot =
        mde_presentation.has_value() ? mde_presentation->camera_body_slot : 0U;
    bool previous_retail_cycle_players = false;
    bool recorded_camera_view_active = false;
    mh::game::OriginalMdeNoticeQueue ghost_notices;
    bool ghost_start_notice_enqueued = false;
    bool ghost_end_notice_enqueued = false;
    mh::game::SimulationDuration go_display_remaining{};
    // p3.1 runs the sorted AI owner and every vehicle once per variable
    // bounded outer-physics slice. Each vehicle step owns its separate three
    // response substeps; subdividing that outer slice again changes
    // suspension and recovery behavior.

    const auto retail_binding = [](const std::string &name) {
      const auto mouse_binding = [](const mh::platform::SdlMouseBinding mouse) {
        return mh::platform::SdlControlBinding{
            SDL_SCANCODE_UNKNOWN, mouse,
            mh::platform::SdlJoystickBinding::none};
      };
      const auto joystick_binding =
          [](const mh::platform::SdlJoystickBinding joystick) {
            return mh::platform::SdlControlBinding{
                SDL_SCANCODE_UNKNOWN, mh::platform::SdlMouseBinding::none,
                joystick};
          };
      if (name == "MouseLeft") {
        return mouse_binding(mh::platform::SdlMouseBinding::left);
      }
      if (name == "MouseRight") {
        return mouse_binding(mh::platform::SdlMouseBinding::right);
      }
      if (name == "MouseUp") {
        return mouse_binding(mh::platform::SdlMouseBinding::up);
      }
      if (name == "MouseDown") {
        return mouse_binding(mh::platform::SdlMouseBinding::down);
      }
      if (name == "MouseButtonLeft") {
        return mouse_binding(mh::platform::SdlMouseBinding::button_left);
      }
      if (name == "MouseButtonRight") {
        return mouse_binding(mh::platform::SdlMouseBinding::button_right);
      }
      if (name == "MouseButtonMiddle") {
        return mouse_binding(mh::platform::SdlMouseBinding::button_middle);
      }
      if (name == "JoyLeft") {
        return joystick_binding(mh::platform::SdlJoystickBinding::left);
      }
      if (name == "JoyRight") {
        return joystick_binding(mh::platform::SdlJoystickBinding::right);
      }
      if (name == "JoyUp") {
        return joystick_binding(mh::platform::SdlJoystickBinding::up);
      }
      if (name == "JoyDown") {
        return joystick_binding(mh::platform::SdlJoystickBinding::down);
      }
      if (name == "JoyIn") {
        return joystick_binding(mh::platform::SdlJoystickBinding::in);
      }
      if (name == "JoyOut") {
        return joystick_binding(mh::platform::SdlJoystickBinding::out);
      }
      if (name.size() == 10U && name.starts_with("JoyButton") &&
          name.back() >= '1' && name.back() <= '8') {
        const auto button = static_cast<mh::platform::SdlJoystickBinding>(
            static_cast<std::uint8_t>(
                mh::platform::SdlJoystickBinding::button1) +
            static_cast<std::uint8_t>(name.back() - '1'));
        return joystick_binding(button);
      }
      auto key = SDL_SCANCODE_UNKNOWN;
      if (name == "LeftArrow") {
        key = SDL_SCANCODE_LEFT;
      } else if (name == "RightArrow") {
        key = SDL_SCANCODE_RIGHT;
      } else if (name == "UpArrow") {
        key = SDL_SCANCODE_UP;
      } else if (name == "DownArrow") {
        key = SDL_SCANCODE_DOWN;
      } else if (name == "Shift") {
        key = SDL_SCANCODE_LSHIFT;
      } else if (name == "Ctrl") {
        key = SDL_SCANCODE_LCTRL;
      } else if (name == "Alt") {
        key = SDL_SCANCODE_LALT;
      } else if (name == "Esc") {
        key = SDL_SCANCODE_ESCAPE;
      } else {
        key = SDL_GetScancodeFromName(name.c_str());
      }
      if (key == SDL_SCANCODE_UNKNOWN) {
        throw std::invalid_argument("unsupported retail control key: " + name);
      }
      return mh::platform::SdlControlBinding{
          key, mh::platform::SdlMouseBinding::none,
          mh::platform::SdlJoystickBinding::none};
    };
    mh::platform::SdlKeyboardBindings keyboard_bindings;
    if (!retail_drive_bindings[0U].empty()) {
      keyboard_bindings.turn_left = retail_binding(retail_drive_bindings[0U]);
      keyboard_bindings.turn_right = retail_binding(retail_drive_bindings[1U]);
      keyboard_bindings.accelerate = retail_binding(retail_drive_bindings[2U]);
      keyboard_bindings.brake = retail_binding(retail_drive_bindings[3U]);
    }
    if (!retail_drive_bindings[4U].empty()) {
      keyboard_bindings.handbrake = retail_binding(retail_drive_bindings[4U]);
    }
    if (!retail_drive_bindings[5U].empty()) {
      keyboard_bindings.rear_view = retail_binding(retail_drive_bindings[5U]);
    }
    if (!retail_drive_bindings[6U].empty()) {
      keyboard_bindings.horn = retail_binding(retail_drive_bindings[6U]);
    }
    if (!retail_drive_bindings[7U].empty()) {
      keyboard_bindings.shift_up = retail_binding(retail_drive_bindings[7U]);
    }
    if (!retail_drive_bindings[8U].empty()) {
      keyboard_bindings.shift_down = retail_binding(retail_drive_bindings[8U]);
    }
    if (!retail_drive_bindings[9U].empty()) {
      keyboard_bindings.in_car_view = retail_binding(retail_drive_bindings[9U]);
    }
    if (!retail_drive_bindings[10U].empty()) {
      keyboard_bindings.out_car_view =
          retail_binding(retail_drive_bindings[10U]);
    }
    if (!retail_drive_bindings[11U].empty()) {
      keyboard_bindings.camera_view =
          retail_binding(retail_drive_bindings[11U]);
    }
    if (!retail_drive_bindings[12U].empty()) {
      keyboard_bindings.cycle_players =
          retail_binding(retail_drive_bindings[12U]);
    }
    mh::platform::SdlPlayerInput player_input(keyboard_bindings);
    mh::game::PlayerInputRouter input_router;
    mh::game::DriveSession session({race_laps, 0U, start_flyby.duration(),
                                    AuthoredStartFlyby::lead_in_duration});
    mh::game::OriginalSplineRaceProgress player_race_progress(motion,
                                                              race_laps);
    for (std::size_t index = 0U; index < camera_cycle_index(initial_camera);
         ++index) {
      mh::game::PlayerTickInput camera_input;
      camera_input.toggle_camera_pressed = true;
      static_cast<void>(session.step(camera_input, mh::game::fixed_step));
    }
    mh::game::VehicleCameraRuntime vehicle_camera_runtime;
    auto environment_collision_focus = vehicle.state().pose.world_position;
    auto previous_time = std::chrono::steady_clock::now();
    bool running = true;
    bool result_visible = false;
    mh::game::ControlInput player_presentation_controls{};
    bool pause_visible = initial_pause_page.has_value();
    bool pause_restart_requested = false;
    std::uint32_t black_lotus_dwell_frames = 0U;
    std::uint64_t atlantika_scroll_frames = 0U;
    std::uint32_t thunder_random_state = 1U;
    double thunder_effect_seconds = 0.0;
    auto active_location_easter_egg =
        mh::game::OriginalPcLocationEasterEgg::none;
    mh::game::OriginalPostFinishSchedule post_finish_schedule;
    RacePauseMenuState pause_menu;
    if (initial_pause_page.has_value()) {
      pause_menu.page = *initial_pause_page;
    }
    pause_menu.view_distance_percent = initial_view_distance_percent;
    pause_menu.window_mode = initial_window_mode;
    pause_menu.renderer_backend = graphic_renderer_backend(renderer_backend);
    pause_menu.car_shading = static_cast<std::size_t>(car_shading);
    const auto quantize_race_volume = [](const int volume) {
      constexpr int levels = 20;
      const auto level =
          std::clamp(static_cast<int>(std::lround(static_cast<double>(volume) *
                                                  levels / 254.0)),
                     0, levels);
      return (level * 254 + levels / 2) / levels;
    };
    pause_menu.sound_effects_volume = quantize_race_volume(initial_sfx_volume);
    pause_menu.cd_volume = quantize_race_volume(initial_music_volume);
    pause_menu.info_detail = info_detail_mode;
    pause_menu.info_map = info_map_mode;
    pause_menu.checkpoint_info = checkpoint_info_mode;
    pause_menu.checkpoint_display_time_ms = checkpoint_display_time_ms;
    pause_menu.camera_shake = camera_shake_enabled;
    pause_menu.ui_scale_percent = ui_scale_percent;
    const auto send_network_pause_request = [&](const bool paused) {
      if (!networked_race) {
        return;
      }
      if (multiplayer_session->is_host()) {
        if (network_pause_state != paused) {
          network_pause_state = paused;
          ++network_pause_generation;
          network_control_state_dirty = true;
        }
        return;
      }
      mh::network::RaceInputFrame request;
      request.tick = network_simulation_tick++;
      request.pause_requested = true;
      request.pause_state = paused;
      (void)multiplayer_session->send_input(request);
    };
    const auto hosted_parent_window_id =
        hosted_overlay ? SDL_GetWindowID(shared_window) : 0U;
    const auto hosted_overlay_window_id =
        hosted_overlay ? SDL_GetWindowID(window.get()) : 0U;
    bool hosted_overlay_realign_pending = false;
    bool hosted_overlay_raise_pending = false;
    const auto send_network_restart_request = [&] {
      if (!networked_race) {
        pause_restart_requested = true;
        pause_visible = false;
        return;
      }
      if (multiplayer_session->is_host()) {
        pause_restart_requested = true;
        pause_visible = false;
        return;
      }
      mh::network::RaceInputFrame request;
      request.tick = network_simulation_tick++;
      request.restart_requested = true;
      (void)multiplayer_session->send_input(request);
      // Stay frozen on the pause frame until the host publishes the committed
      // restart generation. This prevents the requesting client from running
      // a separate countdown while the host is still on the old race.
    };
    const auto make_network_snapshot = [&] {
      mh::network::RaceSnapshot snapshot;
      snapshot.tick = network_simulation_tick;
      snapshot.restart_generation = network_restart_generation;
      snapshot.pause_generation = network_pause_generation;
      snapshot.paused = network_pause_state;
      snapshot.vehicles.reserve(race_roster.size());
      const auto append_vehicle_state =
          [&](const std::size_t slot,
              const mh::game::OriginalVehicleRuntime &runtime) {
            const auto &state = runtime.state();
            mh::network::NetworkVehicleState network_state;
            network_state.peer = network_peer_by_slot[slot];
            for (std::size_t axis = 0U; axis < 3U; ++axis) {
              network_state.position[axis] =
                  static_cast<float>(state.pose.world_position[axis]);
              network_state.velocity[axis] =
                  static_cast<float>(state.velocity.local_linear[axis]);
            }
            network_state.rotation = basis_to_quaternion(state.pose);
            network_state.speed =
                static_cast<float>(state.velocity.local_linear[2U]);
            network_state.lap = hud_completed_laps[slot];
            network_state.checkpoint =
                static_cast<std::uint32_t>(hud_vehicle_samples[slot]);
            network_state.finished = hud_finish_times[slot].has_value();
            snapshot.vehicles.push_back(network_state);
          };
      append_vehicle_state(0U, vehicle);
      for (const auto &opponent : opponents) {
        append_vehicle_state(opponent.slot_index, *opponent.vehicle);
      }
      return snapshot;
    };
    const auto initial_sfx_scale =
        static_cast<float>(pause_menu.sound_effects_volume) / 254.0F;
    const auto set_race_sfx_scale = [&](const float scale) {
      if (engine_sound.has_value()) {
        engine_sound->set_volume_scale(scale);
      }
      if (opponent_engine_sound.has_value()) {
        opponent_engine_sound->set_volume_scale(scale);
      }
      if (race_sound.has_value()) {
        race_sound->set_volume_scale(scale);
      }
      if (horn_sound.has_value()) {
        horn_sound->set_volume_scale(scale);
      }
      if (material_sound.has_value()) {
        material_sound->set_volume_scale(scale);
      }
      if (environment_sound.has_value()) {
        environment_sound->set_volume_scale(scale);
      }
    };
    const auto set_pause_sfx_scale = [&](const float scale) {
      if (pause_menu_loop.has_value()) {
        require(SDL_SetAudioStreamGain(pause_menu_loop->stream.get(), scale),
                "set pause-menu loop gain");
      }
      if (pause_menu_enter.has_value()) {
        pause_menu_enter->set_volume_scale(scale);
      }
      if (pause_menu_cursor.has_value()) {
        pause_menu_cursor->set_volume_scale(scale);
      }
      if (pause_menu_error.has_value()) {
        pause_menu_error->set_volume_scale(scale);
      }
    };
    set_race_sfx_scale(initial_sfx_scale);
    set_pause_sfx_scale(initial_sfx_scale);
    if (music.has_value()) {
      const auto initial_music_scale =
          static_cast<float>(pause_menu.cd_volume) / 254.0F;
      require(SDL_SetAudioStreamGain(music->stream.get(),
                                     0.22F * music_gain * initial_music_scale),
              "set original initial in-race CD volume");
    }
    bool automatic_countdown_pending = true;
    bool pause_audio_active = false;
    const auto enter_pause_audio = [&] {
      // p3.1 RVA 0x3f0d7 calls the global SFX stop owner before starting the
      // pause loop and Enter sound. The active CD track is not switched; its
      // stream is paused in place below and resumes at the retained cursor.
      set_race_sfx_scale(0.0F);
      // Preserve the active race-track cursor while muting it. Unbinding only
      // the music stream stops device consumption without clearing its queued
      // PCM; the pause-menu loop remains bound to the shared device.
      if (music.has_value()) {
        music->set_bound(false);
      }
      if (engine_sound.has_value()) {
        engine_sound->stop();
      }
      if (opponent_engine_sound.has_value()) {
        opponent_engine_sound->stop();
      }
      if (race_sound.has_value()) {
        race_sound->stop();
      }
      if (horn_sound.has_value()) {
        horn_sound->stop();
      }
      if (material_sound.has_value()) {
        material_sound->stop();
      }
      if (environment_sound.has_value()) {
        environment_sound->stop();
      }
      if (pause_menu_loop_clip.has_value()) {
        pause_menu_loop.emplace(start_looping_audio_clip(
            *pause_menu_loop_clip, audio_device->identifier, initial_sfx_scale,
            AudioPan::center, 2000U, audio_device->specification.freq, true));
      }
      set_pause_sfx_scale(static_cast<float>(pause_menu.sound_effects_volume) /
                          254.0F);
      if (pause_menu_enter.has_value()) {
        pause_menu_enter->trigger();
      }
      pause_audio_active = true;
    };
    const auto leave_pause_audio = [&] {
      pause_menu_loop.reset();
      if (pause_menu_enter.has_value()) {
        pause_menu_enter->stop();
      }
      if (pause_menu_cursor.has_value()) {
        pause_menu_cursor->stop();
      }
      if (pause_menu_error.has_value()) {
        pause_menu_error->stop();
      }
      set_race_sfx_scale(static_cast<float>(pause_menu.sound_effects_volume) /
                         254.0F);
      if (music.has_value()) {
        music->set_bound(true);
      }
      pause_audio_active = false;
    };
    int exit_code = 0;
    std::uint64_t rendered_frames = 0U;
    using BenchmarkClock = std::chrono::steady_clock;
    std::optional<BenchmarkClock::time_point> benchmark_measurement_start;
    std::optional<BenchmarkClock::time_point> benchmark_measurement_end;
    std::uint64_t benchmark_presented_frames = 0U;
    bool benchmark_completed = false;
    constexpr std::size_t ai_opening_report_cycles = 8U;
    static constexpr std::array<float, ai_opening_report_cycles>
        captured_ai_opening_time_steps{0.009534515F, 0.008745959F, 0.008422965F,
                                       0.008383738F, 0.008743325F, 0.008341884F,
                                       0.008034888F, 0.007619031F};
    static constexpr std::array<float, ai_opening_report_cycles>
        captured_ai_physics_time_steps{0.04F, 0.04F, 0.04F, 0.04F,
                                       0.04F, 0.04F, 0.04F, 0.04F};
    const auto ai_opening_time_step = [&](const std::size_t cycle) {
      const auto bounded = std::min(cycle, ai_opening_report_cycles - 1U);
      return ai_physics_capture_oracle
                 ? captured_ai_physics_time_steps[bounded]
                 : captured_ai_opening_time_steps[bounded];
    };
    std::size_t ai_opening_cycle = 0U;
    std::vector<AiOpeningControlRecord> ai_opening_records;
    if (ai_control_report_path.has_value()) {
      ai_opening_records.reserve(ai_opening_report_cycles * 7U);
    }
    const auto prepare_result_frame = [&] {
      const auto &timing = session.race().progress().timing;
      const auto best_lap =
          timing.completed_laps.empty()
              ? mh::game::SimulationDuration::zero()
              : *std::min_element(timing.completed_laps.begin(),
                                  timing.completed_laps.end());
      const auto bounded_ms = [](const std::int64_t value) {
        return static_cast<std::uint32_t>(std::clamp<std::int64_t>(
            value, 0, std::numeric_limits<std::uint32_t>::max()));
      };
      std::vector<mh::ui::RaceResultEntry> entries;
      std::vector<mh::content::TgaImage> portraits;
      const auto authoritative_network_results =
          networked_race && !multiplayer_session->is_host() &&
          network_results.has_value();
      if (authoritative_network_results) {
        for (const auto &network_result : *network_results) {
          const auto found =
              std::find(network_peer_by_slot.begin(),
                        network_peer_by_slot.end(), network_result.peer);
          if (found == network_peer_by_slot.end()) {
            continue;
          }
          const auto slot = static_cast<std::size_t>(
              std::distance(network_peer_by_slot.begin(), found));
          hud_best_laps[slot] =
              std::chrono::milliseconds(network_result.best_lap_ms);
          if (network_result.finished) {
            hud_finish_times[slot] =
                std::chrono::milliseconds(network_result.race_time_ms);
            hud_completed_laps[slot] = session.race().config().lap_count;
          }
        }
      } else {
        hud_finish_times[0U] = timing.total;
        hud_best_laps[0U] = best_lap;
        hud_completed_laps[0U] = session.race().config().lap_count;
      }
      auto cutoff_time = timing.total + post_finish_schedule.elapsed();
      if (authoritative_network_results) {
        for (const auto &result : *network_results) {
          cutoff_time =
              std::max(cutoff_time,
                       std::chrono::duration_cast<mh::game::SimulationDuration>(
                           std::chrono::milliseconds(result.race_time_ms)));
        }
      }
      const auto cutoff_time_ms = bounded_ms(
          std::chrono::duration_cast<std::chrono::milliseconds>(cutoff_time)
              .count());
      std::vector<mh::ui::RaceResultVehicleState> result_states;
      result_states.reserve(race_roster.size());
      for (std::size_t slot = 0U; slot < race_roster.size(); ++slot) {
        std::optional<std::uint32_t> finish_time_ms;
        if (hud_finish_times[slot].has_value()) {
          finish_time_ms =
              bounded_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                             *hud_finish_times[slot])
                             .count());
        }
        result_states.push_back(
            {race_roster[slot].car_name, race_roster[slot].driver_nick,
             static_cast<std::int32_t>(std::min<std::uint32_t>(
                 race_roster[slot].starting_score,
                 static_cast<std::uint32_t>(
                     std::numeric_limits<std::int32_t>::max()))),
             finish_time_ms, hud_progress_samples[slot],
             bounded_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                            hud_best_laps[slot])
                            .count())});
      }
      const auto ranked_results = mh::ui::make_race_result_field(
          result_states, cutoff_time_ms, league_result_style);
      entries.reserve(ranked_results.size());
      portraits.reserve(ranked_results.size());
      std::vector<mh::network::RaceResultData> published_results;
      if (networked_race && multiplayer_session->is_host()) {
        published_results.reserve(ranked_results.size());
      }
      for (const auto &ranked : ranked_results) {
        const auto slot = ranked.source_index;
        auto entry = ranked.entry;
        if (slot == 0U && completed_player_result != nullptr) {
          *completed_player_result = entry;
        }
        if (networked_race && multiplayer_session->is_host()) {
          published_results.push_back(
              {network_peer_by_slot[slot],
               static_cast<std::uint8_t>(entry.finishing_position),
               entry.best_lap_ms, entry.race_time_ms,
               hud_finish_times[slot].has_value()});
        }
        entries.push_back(std::move(entry));
        portraits.push_back(result_portraits[slot]);
      }
      if (!published_results.empty()) {
        (void)multiplayer_session->publish_results(published_results);
      }
      if (completed_results != nullptr) {
        *completed_results = entries;
      }
      if (league_division_finishing_orders.has_value() &&
          active_league_division.has_value()) {
        auto &active_order =
            (*league_division_finishing_orders)[static_cast<std::size_t>(
                *active_league_division)];
        active_order.clear();
        active_order.reserve(entries.size());
        for (const auto &entry : entries) {
          active_order.push_back(entry.nickname);
        }
        if (completed_league_results != nullptr) {
          *completed_league_results = *league_division_finishing_orders;
        }
      }
      const auto &result_background = entries.size() <= 2U
                                          ? compact_result_background
                                          : full_result_background;
      const auto frame =
          league_result_style
              ? mh::ui::compose_league_results_frame(
                    result_background, result_font, entries, portraits)
              : mh::ui::compose_race_results_frame(
                    result_background, result_font, entries, portraits);
      require(SDL_UpdateTexture(result_texture.get(), nullptr,
                                frame.rgba.data(),
                                static_cast<int>(frame.width * 4U)),
              "upload race-results frame");
      if (mounted_cdda.has_value()) {
        const auto scale = static_cast<float>(pause_menu.cd_volume) / 254.0F;
        auto track = mh::disc::read_mounted_cdda_track_pcm(*mounted_cdda, 2U);
        AudioClip clip;
        clip.specification.format = SDL_AUDIO_S16LE;
        clip.specification.channels = static_cast<int>(track.channels);
        clip.specification.freq = static_cast<int>(track.sample_rate);
        clip.bytes = std::move(track.pcm);
        music.emplace(start_looping_audio_clip(
            std::move(clip), audio_device->identifier, 0.22F * scale,
            AudioPan::center, 2000U, audio_device->specification.freq, true));
        if (!music_enabled) {
          require(SDL_SetAudioStreamGain(music->stream.get(), 0.0F),
                  "retain muted music on race results");
        }
      } else if (music_cue_path.has_value()) {
        const auto scale = static_cast<float>(pause_menu.cd_volume) / 254.0F;
        music.emplace(start_looping_audio_clip(
            load_cdda_clip(*music_cue_path, 2U), audio_device->identifier,
            0.22F * scale, AudioPan::center, 2000U,
            audio_device->specification.freq, true));
        if (!music_enabled) {
          require(SDL_SetAudioStreamGain(music->stream.get(), 0.0F),
                  "retain muted music on race results");
        }
      } else if (music_path.has_value()) {
        const auto results_track = music_path->parent_path() / "track02.wav";
        if (std::filesystem::is_regular_file(results_track)) {
          const auto scale = static_cast<float>(pause_menu.cd_volume) / 254.0F;
          music.emplace(start_looping_audio(
              results_track, audio_device->identifier, 0.22F * scale,
              AudioPan::center, 2000U, audio_device->specification.freq, true));
          if (!music_enabled) {
            require(SDL_SetAudioStreamGain(music->stream.get(), 0.0F),
                    "retain muted music on race results");
          }
        }
      }
      if (engine_sound.has_value()) {
        engine_sound->stop();
      }
      if (opponent_engine_sound.has_value()) {
        opponent_engine_sound->stop();
      }
      result_visible = true;
    };
    struct OpponentNamePlateCandidate {
      std::size_t opponent_index = 0U;
      mh::game::OriginalBodyPoseState pose{};
      double view_depth = 0.0;
    };
    std::vector<std::size_t> ranked_slots(race_roster.size());
    std::vector<mh::game::OriginalAiTrafficVehicle> traffic_vehicles(
        race_roster.size());
    std::vector<std::size_t> sorted_live_slots(race_roster.size());
    std::vector<mh::game::OriginalDynamicVehicleContactScheduleParticipant>
        contact_participants;
    std::vector<std::size_t> contact_live_slots;
    std::vector<CanonicalOpponent *> contact_opponents;
    std::vector<mh::game::OriginalEnvironmentDynamicContactParticipant>
        environment_contact_participants;
    std::vector<mh::game::OriginalDynamicVehicleContactScheduleParticipant>
        environment_vehicle_participants;
    contact_participants.reserve(opponents.size() + 1U);
    contact_live_slots.reserve(opponents.size() + 1U);
    contact_opponents.reserve(opponents.size() + 1U);
    environment_contact_participants.reserve(
        environment_collision_bodies.size());
    environment_vehicle_participants.reserve(opponents.size() + 1U);
    std::vector<OpponentNamePlateCandidate> name_plate_candidates;
    std::vector<SDL_FRect> nearer_vehicle_bounds;
    std::vector<VehicleNamePlateOccluder> nearer_vehicles;
    name_plate_candidates.reserve(opponents.size());
    nearer_vehicle_bounds.reserve(opponents.size());
    nearer_vehicles.reserve(opponents.size());
    std::vector<std::size_t> rendered_ghost_samples;
    while (running) {
      SDL_Event event{};
      while (SDL_PollEvent(&event)) {
        if (!result_visible) {
          player_input.handle_event(event);
        }
        if (hosted_overlay &&
            (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
             event.type == SDL_EVENT_WINDOW_RESTORED ||
             event.type == SDL_EVENT_WINDOW_SHOWN) &&
            (event.window.windowID == hosted_parent_window_id ||
             event.window.windowID == hosted_overlay_window_id)) {
          hosted_overlay_realign_pending = true;
          hosted_overlay_raise_pending = true;
        } else if (hosted_overlay &&
                   (event.type == SDL_EVENT_WINDOW_RESIZED ||
                    event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) &&
                   event.window.windowID == hosted_parent_window_id) {
          hosted_overlay_realign_pending = true;
        }
        if (event.type == SDL_EVENT_QUIT) {
          running = false;
        } else if (recorded_presentation && event.type == SDL_EVENT_KEY_DOWN &&
                   !event.key.repeat && event.key.key == SDLK_ESCAPE) {
          exit_code = 4;
          running = false;
        } else if (result_visible && event.type == SDL_EVENT_KEY_DOWN &&
                   !event.key.repeat &&
                   (event.key.key == SDLK_ESCAPE ||
                    event.key.key == SDLK_RETURN)) {
          exit_code = 4;
          running = false;
        } else if (!result_visible && event.type == SDL_EVENT_KEY_DOWN &&
                   !event.key.repeat && event.key.key == SDLK_ESCAPE) {
          if (!pause_visible) {
            pause_visible = true;
            pause_menu.page = RacePausePage::race;
            pause_menu.selections[0U] = 0U;
            input_router.reset();
            send_network_pause_request(true);
          } else if (pause_menu.page != RacePausePage::race) {
            pause_menu.page = RacePausePage::race;
          } else {
            pause_visible = false;
            send_network_pause_request(false);
          }
        } else if (pause_visible && event.type == SDL_EVENT_KEY_DOWN &&
                   !event.key.repeat && event.key.key == SDLK_UP) {
          pause_menu.hud_preview_until_ticks = 0U;
          pause_menu.previous_choice();
          if (pause_menu_cursor.has_value()) {
            pause_menu_cursor->trigger();
          }
        } else if (pause_visible && event.type == SDL_EVENT_KEY_DOWN &&
                   !event.key.repeat && event.key.key == SDLK_DOWN) {
          pause_menu.hud_preview_until_ticks = 0U;
          pause_menu.next_choice();
          if (pause_menu_cursor.has_value()) {
            pause_menu_cursor->trigger();
          }
        } else if (pause_visible && event.type == SDL_EVENT_KEY_DOWN &&
                   (!event.key.repeat ||
                    (pause_menu.page == RacePausePage::graphics &&
                     pause_menu.selection() == 3U) ||
                    (pause_menu.page == RacePausePage::gameplay &&
                     (pause_menu.selection() == 3U ||
                      pause_menu.selection() == 5U))) &&
                   (event.key.key == SDLK_LEFT ||
                    event.key.key == SDLK_RIGHT)) {
          const auto increase = event.key.key == SDLK_RIGHT;
          if (pause_menu.page == RacePausePage::graphics) {
            switch (pause_menu.selection()) {
            case 0U: {
              const auto current =
                  static_cast<std::uint8_t>(pause_menu.renderer_backend);
              const auto maximum = static_cast<std::uint8_t>(
                  mh::ui::GraphicRendererBackend::software);
              const auto next =
                  increase ? std::min<std::uint8_t>(maximum, current + 1U)
                           : (current == 0U ? 0U : current - 1U);
              pause_menu.renderer_backend =
                  static_cast<mh::ui::GraphicRendererBackend>(next);
              const auto requested_backend =
                  race_renderer_backend(pause_menu.renderer_backend);
              const auto *active_driver = SDL_GetRendererName(renderer.get());
              // D3D9 and D3D12 require a different native SDL device and
              // swap chain. Never relabel/fallback the current renderer in
              // place; persist the selection and apply it on the next race.
              if (active_driver != nullptr &&
                  renderer_driver_matches(requested_backend, active_driver)) {
                renderer_backend = requested_backend;
                scene_depth.set_backend(renderer_backend);
              }
              if (configuration_root.has_value()) {
                mh::ui::update_motorhead_configuration(
                    *configuration_root,
                    {{"Renderer",
                      std::string(mh::ui::graphic_renderer_backend_config_name(
                          pause_menu.renderer_backend))}});
              }
              break;
            }
            case 1U:
              if (increase && pause_menu.car_shading < 3U) {
                ++pause_menu.car_shading;
              } else if (!increase && pause_menu.car_shading > 0U) {
                --pause_menu.car_shading;
              }
              car_shading = static_cast<mh::ui::GraphicCarShading>(
                  pause_menu.car_shading);
              if (configuration_root.has_value()) {
                static constexpr std::array<std::string_view, 4U> names{
                    "Flat", "Gouraud", "Reflection", "Glenz"};
                mh::ui::update_motorhead_configuration(
                    *configuration_root,
                    {{"CarShading",
                      std::string(names.at(pause_menu.car_shading))},
                     {"DetailMode", "Custom"}});
              }
              break;
            case 2U:
              pause_menu.better_perspective = increase;
              break;
            case 3U:
              pause_menu.view_distance_percent =
                  mh::ui::adjust_original_view_distance(
                      pause_menu.view_distance_percent,
                      increase ? mh::ui::MenuDirection::right
                               : mh::ui::MenuDirection::left,
                      1U);
              if (configuration_root.has_value()) {
                mh::ui::update_motorhead_configuration(
                    *configuration_root,
                    {{"ViewDistance",
                      std::to_string(static_cast<int>(
                          std::lround(pause_menu.view_distance_percent))) +
                          ".00"},
                     {"DetailMode", "Custom"}});
              }
              break;
            case 4U: {
              const auto mode_index = static_cast<int>(pause_menu.window_mode);
              const auto next_index = (mode_index + (increase ? 1 : 2)) % 3;
              pause_menu.window_mode =
                  static_cast<mh::ui::GraphicWindowMode>(next_index);
              if (hosted_overlay) {
                // An active child renderer cannot become an exclusive D3D9
                // focus window. Persist the user's choice, then use borderless
                // for this race; the next race starts exclusive on the parent.
                const auto live_mode =
                    pause_menu.window_mode ==
                            mh::ui::GraphicWindowMode::fullscreen
                        ? mh::ui::GraphicWindowMode::borderless
                        : pause_menu.window_mode;
                apply_race_window_mode(shared_window, live_mode, window_width,
                                       window_height);
                align_hosted_race_window(shared_window, window.get(),
                                         window_width, window_height);
                require(SDL_ShowWindow(window.get()),
                        "show hosted race overlay after mode change");
                require(SDL_RaiseWindow(window.get()),
                        "raise hosted race overlay after mode change");
              } else {
                apply_race_window_mode(window.get(), pause_menu.window_mode,
                                       window_width, window_height);
              }
              active_window_mode = pause_menu.window_mode;
              stable_present_period = presentation_period_for_active_display();
              stable_next_present.reset();
              // The embedded front end always supplies its reconstruction-owned
              // User root. Developer dispatch without that handoff may read the
              // retail baseline, but must never rewrite it.
              if (configuration_root.has_value()) {
                mh::ui::update_motorhead_configuration(
                    *configuration_root,
                    {{"WindowMode",
                      std::string(mh::ui::graphic_window_mode_config_name(
                          pause_menu.window_mode))}});
              }
              break;
            }
            default:
              break;
            }
          } else if (pause_menu.page == RacePausePage::sound) {
            auto *volume =
                pause_menu.selection() == 0U ? &pause_menu.sound_effects_volume
                : pause_menu.selection() == 1U ? &pause_menu.cd_volume
                                               : nullptr;
            if (volume != nullptr) {
              constexpr int levels = 20;
              auto level = std::clamp(
                  static_cast<int>(std::lround(static_cast<double>(*volume) *
                                               levels / 254.0)),
                  0, levels);
              level = std::clamp(level + (increase ? 1 : -1), 0, levels);
              *volume = (level * 254 + levels / 2) / levels;
            }
            if (pause_menu.selection() == 0U) {
              const auto scale =
                  static_cast<float>(pause_menu.sound_effects_volume) / 254.0F;
              set_pause_sfx_scale(scale);
            } else if (pause_menu.selection() == 1U && music.has_value()) {
              const auto scale =
                  static_cast<float>(pause_menu.cd_volume) / 254.0F;
              require(SDL_SetAudioStreamGain(
                          music->stream.get(),
                          music_enabled ? 0.22F * music_gain * scale : 0.0F),
                      "set in-race CD volume");
            }
            if (volume != nullptr && configuration_root.has_value()) {
              // Persist the same exact 5% level shown by both sound menus.
              const auto level =
                  std::clamp(static_cast<int>(std::lround(
                                 static_cast<double>(*volume) * 20.0 / 254.0)),
                             0, 20);
              std::ostringstream persisted;
              persisted << std::fixed << std::setprecision(2)
                        << static_cast<double>(level * 5);
              mh::ui::update_motorhead_configuration(
                  *configuration_root,
                  {{pause_menu.selection() == 0U ? "SFXVolume" : "MusicVolume",
                    persisted.str()}});
            }
            if (pause_menu_cursor.has_value()) {
              pause_menu_cursor->trigger();
            }
          } else if (pause_menu.page == RacePausePage::gameplay) {
            const auto step_info_mode = [increase](auto &mode) {
              const auto old = static_cast<std::uint8_t>(mode);
              const auto next = increase ? std::min<std::uint8_t>(2U, old + 1U)
                                         : (old == 0U ? 0U : old - 1U);
              mode = static_cast<mh::ui::GraphicInfoMode>(next);
              return next != old;
            };
            const auto info_config_name = [](const auto mode) {
              return std::string(race_pause_info_mode_text(mode));
            };
            bool changed = false;
            bool preview_hud = false;
            switch (pause_menu.selection()) {
            case 0U:
              changed = step_info_mode(pause_menu.info_detail);
              info_detail_mode = pause_menu.info_detail;
              preview_hud = changed;
              if (changed && configuration_root.has_value()) {
                mh::ui::update_motorhead_configuration(
                    *configuration_root,
                    {{"InfoDetail", info_config_name(pause_menu.info_detail)}});
              }
              break;
            case 1U:
              changed = step_info_mode(pause_menu.info_map);
              info_map_mode = pause_menu.info_map;
              preview_hud = changed;
              if (changed && configuration_root.has_value()) {
                mh::ui::update_motorhead_configuration(
                    *configuration_root,
                    {{"RoadMap", info_config_name(pause_menu.info_map)}});
              }
              break;
            case 2U:
              changed = step_info_mode(pause_menu.checkpoint_info);
              checkpoint_info_mode = pause_menu.checkpoint_info;
              preview_hud = changed;
              if (changed && configuration_root.has_value()) {
                mh::ui::update_motorhead_configuration(
                    *configuration_root,
                    {{"CheckpointInfo",
                      info_config_name(pause_menu.checkpoint_info)}});
              }
              break;
            case 3U: {
              const auto old = pause_menu.checkpoint_display_time_ms;
              pause_menu.checkpoint_display_time_ms =
                  increase ? std::min<std::uint32_t>(10'000U, old + 500U)
                           : (old < 500U ? 0U : old - 500U);
              changed = pause_menu.checkpoint_display_time_ms != old;
              checkpoint_display_time_ms =
                  pause_menu.checkpoint_display_time_ms;
              preview_hud = changed;
              if (changed && configuration_root.has_value()) {
                std::ostringstream seconds;
                seconds << std::fixed << std::setprecision(2)
                        << static_cast<double>(checkpoint_display_time_ms) /
                               1000.0;
                mh::ui::update_motorhead_configuration(
                    *configuration_root, {{"CheckpointDelay", seconds.str()}});
              }
              break;
            }
            case 4U: {
              const auto old = pause_menu.camera_shake;
              pause_menu.camera_shake = increase;
              changed = pause_menu.camera_shake != old;
              camera_shake_enabled = pause_menu.camera_shake;
              if (changed && configuration_root.has_value()) {
                mh::ui::update_motorhead_configuration(
                    *configuration_root,
                    {{"CameraShake", camera_shake_enabled ? "On" : "Off"}});
              }
              break;
            }
            case 5U: {
              const auto old = pause_menu.ui_scale_percent;
              pause_menu.ui_scale_percent =
                  std::clamp(old + (increase ? 5 : -5), 50, 150);
              changed = pause_menu.ui_scale_percent != old;
              ui_scale_percent = pause_menu.ui_scale_percent;
              mh::game::set_original_hud_ui_scale(
                  static_cast<double>(ui_scale_percent) / 100.0);
              preview_hud = changed;
              if (changed && configuration_root.has_value()) {
                mh::ui::update_motorhead_configuration(
                    *configuration_root,
                    {{"UIScale", std::to_string(ui_scale_percent) + ".00"}});
              }
              break;
            }
            default:
              break;
            }
            if (preview_hud) {
              pause_menu.hud_preview_until_ticks = SDL_GetTicks() + 5000U;
            }
            if (changed && pause_menu_cursor.has_value()) {
              pause_menu_cursor->trigger();
            } else if (!changed && pause_menu_error.has_value()) {
              pause_menu_error->trigger();
            }
          }
        } else if (pause_visible && event.type == SDL_EVENT_KEY_DOWN &&
                   !event.key.repeat && event.key.key == SDLK_RETURN) {
          if (pause_menu_enter.has_value()) {
            pause_menu_enter->trigger();
          }
          if (pause_menu.page == RacePausePage::race) {
            if (pause_menu.selection() == 0U) {
              pause_visible = false;
              send_network_pause_request(false);
            } else if (pause_menu.selection() == 1U) {
              send_network_restart_request();
            } else if (pause_menu.selection() == 2U) {
              pause_menu.page = RacePausePage::graphics;
            } else if (pause_menu.selection() == 3U) {
              pause_menu.page = RacePausePage::gameplay;
            } else if (pause_menu.selection() == 4U) {
              pause_menu.page = RacePausePage::sound;
            } else if (pause_menu.selection() == 5U) {
              if (networked_race) {
                // Commit and flush the whole-session lobby return while both
                // race loops are still servicing their network connections.
                multiplayer_session->finish_race();
              }
              exit_code = 4;
              running = false;
            }
          } else {
            const auto back_index =
                pause_menu.page == RacePausePage::graphics   ? 5U
                : pause_menu.page == RacePausePage::gameplay ? 6U
                                                             : 2U;
            if (pause_menu.selection() == back_index) {
              pause_menu.page = RacePausePage::race;
            }
          }
        } else if (start_flyby.active(session.race().progress())) {
          // The retail start-camera owner does not hand driving or camera
          // controls to the player until both authored shots have completed.
        } else if (result_visible) {
          // The recovered result screen polls only Escape and Return.
        }
      }
      if (pause_visible && !pause_audio_active) {
        enter_pause_audio();
      } else if (!pause_visible && pause_audio_active) {
        leave_pause_audio();
      }
      bool presentation_horn_held = false;
      bool presentation_rear_view_held = false;
      if (!result_visible && !pause_visible) {
        const auto sampled_input = player_input.sample();
        if (recorded_presentation) {
          mh::game::PlayerInputSources replay_input;
          replay_input.select_in_car_camera = sampled_input.retail_in_car_view;
          replay_input.select_out_car_camera =
              sampled_input.retail_out_car_view;
          input_router.update(replay_input);

          // p3.1 evaluates these fixed actions in F1/F2/F3 order. CameraView
          // selects the track's CameraSplineName owner; either stable vehicle
          // view exits it.
          if (sampled_input.retail_in_car_view ||
              sampled_input.retail_out_car_view) {
            recorded_camera_view_active = false;
          }
          if (sampled_input.retail_camera_view) {
            recorded_camera_view_active = true;
          }

          const auto cycle_pressed = sampled_input.retail_cycle_players;
          if (cycle_pressed && !previous_retail_cycle_players &&
              ghost_mode == mh::game::OriginalMdePlaybackMode::replay &&
              !ghost_samples.empty()) {
            const auto next = mh::game::next_original_mde_camera_body_slot(
                ghost_samples, recorded_camera_body_slot);
            if (next.has_value()) {
              recorded_camera_body_slot = *next;
            }
          }
          previous_retail_cycle_players = cycle_pressed;
        } else {
          presentation_horn_held = sampled_input.horn;
          presentation_rear_view_held = sampled_input.rear_view;
          input_router.update(sampled_input);
        }
      }
      if (hosted_overlay_realign_pending) {
        align_hosted_race_window(shared_window, window.get(), window_width,
                                 window_height);
        require(SDL_ShowWindow(window.get()),
                "restore hosted race overlay after focus change");
        if (hosted_overlay_raise_pending) {
          require(SDL_RaiseWindow(window.get()),
                  "raise hosted race overlay after focus change");
        }
        hosted_overlay_realign_pending = false;
        hosted_overlay_raise_pending = false;
      }

      const auto now = std::chrono::steady_clock::now();
      auto elapsed = std::chrono::duration_cast<mh::game::SimulationDuration>(
          now - previous_time);
      previous_time = now;
      const auto environment_elapsed_seconds =
          pause_visible ? 0.0 : std::chrono::duration<double>(elapsed).count();
      environment_scene_frame +=
          environment_elapsed_seconds * environment_scene.frames_per_second;
      for (std::size_t body_index = 0U;
           body_index < environment_collision_bodies.size(); ++body_index) {
        const auto &asset = environment_collision_body_assets[body_index];
        auto &runtime = environment_collision_bodies[body_index];
        if (!runtime.detached_from_authored_motion) {
          runtime.body.pose =
              mh::game::make_original_environment_collision_pose(
                  environment_scene.objects.at(asset.scene_object_index),
                  environment_scene_frame);
        }
      }
      const auto physics_schedule =
          result_visible || pause_visible
              ? mh::game::OriginalPhysicsSliceSchedule{}
              : mh::game::make_original_physics_slice_schedule(
                    std::chrono::duration<double>(elapsed).count());
      std::array<bool, 3U> countdown_sound_requested{};
      auto go_sound_requested = false;
      auto finish_sound_requested = false;
      auto outer_frame_active = physics_schedule.slice_count != 0U;
      if (networked_race) {
        multiplayer_session->tick(SDL_GetTicks());
        if (!multiplayer_session->is_host()) {
          if (auto results = multiplayer_session->take_results();
              results.has_value() && !result_visible) {
            network_results = std::move(*results);
            prepare_result_frame();
            outer_frame_active = false;
          }
        }
        if (multiplayer_session->state() != mh::network::SessionState::racing) {
          // A peer's LEAVEGAME is a return to the retained front-end lobby,
          // not an application exit. Stop this embedded race immediately so
          // no frame is rendered from state that the network owner retired.
          exit_code = 4;
          running = false;
          continue;
        } else if (multiplayer_session->is_host()) {
          for (const auto &[peer, input] : multiplayer_session->take_inputs()) {
            if (peer < network_controls.size()) {
              network_controls[peer] = {static_cast<double>(input.throttle),
                                        static_cast<double>(input.brake),
                                        static_cast<double>(input.steering),
                                        input.handbrake,
                                        input.shift_up,
                                        input.shift_down};
            }
            if (input.restart_requested) {
              pause_restart_requested = true;
              if (network_pause_state) {
                network_pause_state = false;
                pause_visible = false;
                ++network_pause_generation;
              }
            } else if (input.pause_requested &&
                       network_pause_state != input.pause_state) {
              network_pause_state = input.pause_state;
              pause_visible = input.pause_state;
              pause_menu.page = RacePausePage::race;
              pause_menu.selections[0U] = 0U;
              if (pause_visible) {
                input_router.reset();
              }
              ++network_pause_generation;
              network_control_state_dirty = true;
            }
          }
        } else if (const auto snapshot = multiplayer_session->take_snapshot();
                   snapshot.has_value()) {
          if (snapshot->restart_generation !=
              observed_network_restart_generation) {
            observed_network_restart_generation = snapshot->restart_generation;
            network_restart_pending = true;
            pause_visible = false;
          }
          if (snapshot->pause_generation != observed_network_pause_generation) {
            observed_network_pause_generation = snapshot->pause_generation;
            network_pause_state = snapshot->paused;
            pause_visible = snapshot->paused;
            pause_menu.page = RacePausePage::race;
            pause_menu.selections[0U] = 0U;
            if (pause_visible) {
              input_router.reset();
            }
          }
          for (const auto &network_state : snapshot->vehicles) {
            const auto found =
                std::find(network_peer_by_slot.begin(),
                          network_peer_by_slot.end(), network_state.peer);
            if (found == network_peer_by_slot.end()) {
              continue;
            }
            const auto slot = static_cast<std::size_t>(
                std::distance(network_peer_by_slot.begin(), found));
            auto *runtime =
                slot == 0U ? &vehicle : opponents[slot - 1U].vehicle.get();
            mh::game::OriginalBodyPoseState pose = runtime->state().pose;
            pose.world_position = {
                static_cast<double>(network_state.position[0U]),
                static_cast<double>(network_state.position[1U]),
                static_cast<double>(network_state.position[2U])};
            pose.body_basis = quaternion_to_basis(network_state.rotation);
            mh::game::OriginalBodyVelocityState velocity{};
            velocity.local_linear = {
                static_cast<double>(network_state.velocity[0U]),
                static_cast<double>(network_state.velocity[1U]),
                static_cast<double>(network_state.velocity[2U])};
            runtime->seed_captured_body_state(pose, velocity, velocity);
          }
        }
        if (multiplayer_session->is_host() && network_control_state_dirty) {
          (void)multiplayer_session->publish_snapshot(make_network_snapshot());
          network_control_state_dirty = false;
        }
      }
      if (outer_frame_active) {
        vehicle.begin_outer_frame();
        for (auto &opponent : opponents) {
          opponent.vehicle->begin_outer_frame();
        }
      }
      for (std::size_t tick = 0U; tick < ((result_visible || pause_visible)
                                              ? 0U
                                              : physics_schedule.slice_count);
           ++tick) {
        const auto physics_seconds =
            ai_control_report_path.has_value()
                ? static_cast<double>(ai_opening_time_step(ai_opening_cycle))
                : static_cast<double>(physics_schedule.slices[tick]);
        const auto physics_duration =
            std::chrono::duration_cast<mh::game::SimulationDuration>(
                std::chrono::duration<double>(physics_seconds));
        const auto simulated_physics_seconds =
            pc_cheats.active(mh::game::OriginalPcCheat::underwater)
                ? physics_seconds * 0.5
                : physics_seconds;
        auto tick_input = input_router.consume_tick(physics_duration);
        const auto start_sequence_active =
            start_flyby.active(session.race().progress());
        const auto authoritative_network_restart =
            networked_race && !multiplayer_session->is_host() &&
            network_restart_pending;
        if (pause_restart_requested) {
          tick_input.restart_pressed = true;
          pause_restart_requested = false;
        }
        if (network_restart_pending) {
          tick_input.restart_pressed = true;
          network_restart_pending = false;
        }
        if (start_sequence_active) {
          tick_input.start_pressed = false;
          if (!recorded_presentation) {
            tick_input.toggle_camera_pressed = false;
            tick_input.select_in_car_camera_pressed = false;
            tick_input.select_out_car_camera_pressed = false;
            tick_input.select_bumper_camera_pressed = false;
            tick_input.select_far_camera_pressed = false;
          }
        }
        const auto staged_player_controls = tick_input.controls;
        if (networked_race) {
          mh::network::RaceInputFrame network_input;
          network_input.tick = network_simulation_tick++;
          network_input.steering =
              static_cast<float>(staged_player_controls.steering);
          network_input.throttle =
              static_cast<float>(staged_player_controls.throttle);
          network_input.brake =
              static_cast<float>(staged_player_controls.brake);
          network_input.handbrake = staged_player_controls.handbrake;
          network_input.horn = tick_input.horn_held;
          network_input.shift_up = staged_player_controls.shift_up;
          network_input.shift_down = staged_player_controls.shift_down;
          const auto restart_routing = mh::network::route_race_restart(
              multiplayer_session->is_host(), tick_input.restart_pressed,
              authoritative_network_restart);
          network_input.restart_requested = restart_routing.send_request;
          (void)multiplayer_session->send_input(network_input);
          // Only the host's generation may commit a restart on a client. Do
          // not echo that authoritative commit as a fresh request.
          tick_input.restart_pressed = restart_routing.apply_local;
        }
        if (post_finish_schedule.active()) {
          tick_input.start_pressed = false;
        }
        if (automatic_countdown_pending &&
            session.race().progress().phase == mh::game::RacePhase::ready) {
          tick_input.start_pressed = true;
          automatic_countdown_pending = false;
        }
        const auto command = session.step(tick_input, physics_duration);
        post_finish_schedule.advance(physics_duration);
        countdown_sound_requested[0U] = countdown_sound_requested[0U] ||
                                        command.start_events.countdown_three;
        countdown_sound_requested[1U] =
            countdown_sound_requested[1U] || command.start_events.countdown_two;
        countdown_sound_requested[2U] =
            countdown_sound_requested[2U] || command.start_events.countdown_one;
        go_sound_requested = go_sound_requested || command.start_events.go;
        if (go_display_remaining > mh::game::SimulationDuration::zero()) {
          go_display_remaining =
              std::max(mh::game::SimulationDuration::zero(),
                       go_display_remaining - physics_duration);
        }
        if (command.start_events.go) {
          benchmark_race_started = benchmark_presentation;
          go_display_remaining = std::chrono::seconds(1);
          const auto &launch_tuning = vehicle.drive().tuning();
          player_launch_traction.begin(vehicle.drive().state().engine_scalar,
                                       launch_tuning.minimum_rpm,
                                       launch_tuning.maximum_rpm);
          session.set_camera_mode(initial_camera);
          if (ghost_playback.has_value()) {
            // Retail MDEs retain roughly six seconds of stationary grid
            // pre-roll. Align their first visible displacement to this host's
            // GO event instead of stacking the retained delay after our own
            // countdown.
            ghost_playback->reset(ghost_launch_milliseconds);
            ghost_countdown_hold_remaining_milliseconds = 0U;
            ghost_started_ticks.reset();
            ghost_previous_ticks = 0U;
            ghost_samples.clear();
            std::fill(recorded_wheel_visuals.begin(),
                      recorded_wheel_visuals.end(), WheelVisualState{});
          }
        }
        if (command.reset_vehicle) {
          if (networked_race && multiplayer_session->is_host()) {
            ++network_restart_generation;
            if (network_pause_state) {
              network_pause_state = false;
              ++network_pause_generation;
            }
            network_control_state_dirty = true;
          }
          if (horn_sound.has_value()) {
            horn_sound->stop();
          }
          vehicle.reset();
          settle_grid_vehicle(vehicle);
          // reset() closes the frame owned by every runtime. Do not issue an
          // unmatched end_outer_frame() after this tick.
          outer_frame_active = false;
          player_race_progress =
              mh::game::OriginalSplineRaceProgress(motion, race_laps);
          checkpoint_display_remaining = mh::game::SimulationDuration::zero();
          automatic_countdown_pending = true;
          player_route_cursor.reset(player_initial_xz);
          post_finish_player_controller.reset(player_initial_xz);
          post_finish_player_controller.initialize_lateral_target(
              vehicle.ai_pose());
          player_wheels = {};
          player_presentation_controls = {};
          player_wheels.suspension_states =
              settled_wheel_visual_states(vehicle);
          for (auto &opponent : opponents) {
            opponent.vehicle->reset();
            settle_grid_vehicle(*opponent.vehicle);
            const std::array<float, 2U> opponent_xz{
                static_cast<float>(
                    opponent.vehicle->state().pose.world_position[0U]),
                static_cast<float>(
                    opponent.vehicle->state().pose.world_position[2U])};
            opponent.controller->reset(opponent_xz);
            if (canonical_opening_fixture) {
              opponent.controller->initialize_lateral_target(
                  mh::game::
                      original_goldbridge_quick_race_ai_initializer_slots()
                          [opponent.slot_index]
                              .captured_initial_lateral_target);
            } else {
              opponent.controller->initialize_lateral_target(
                  opponent.vehicle->state().pose);
            }
            opponent.controls = {};
            opponent.presentation_controls = {};
            opponent.retained_wheel_states =
                settled_wheel_visual_states(*opponent.vehicle);
            opponent.retained_wheel_contact_flags = {1U, 1U, 1U, 1U};
            opponent.wheels = {};
            opponent.wheels.suspension_states = opponent.retained_wheel_states;
            opponent.skid_marks = {};
            opponent.tire_smoke = {};
          }
          engine_audio_mix = {};
          player_engine_track_mix = 0.0;
          player_skid_marks = {};
          player_tire_smoke = {};
          player_launch_traction.reset();
          player_launch_wheelspin = 0.0;
          retained_player_hull_audio = {};
          hud_samples_initialized = false;
          catch_up_rank_initialized = false;
          hud_progress_samples.fill(0);
          catch_up_sample_progress.fill(0);
          catch_up_progress_by_rank.fill(0.0F);
          hud_vehicle_speeds.fill(0.0F);
          hud_completed_laps.fill(0U);
          hud_lap_started.fill(mh::game::SimulationDuration::zero());
          hud_best_laps.fill(mh::game::SimulationDuration::zero());
          hud_finish_times.fill(std::nullopt);
          post_finish_schedule.reset();
          go_display_remaining = mh::game::SimulationDuration::zero();
          if (ghost_playback.has_value()) {
            ghost_playback->reset(ghost_countdown_start_milliseconds);
            ghost_countdown_hold_remaining_milliseconds =
                ghost_countdown_hold_milliseconds;
            recorded_camera_body_slot = mde_presentation.has_value()
                                            ? mde_presentation->camera_body_slot
                                            : 0U;
            previous_retail_cycle_players = false;
            recorded_camera_view_active = false;
            ghost_started_ticks.reset();
            ghost_previous_ticks = 0U;
            ghost_samples.clear();
            std::fill(recorded_wheel_visuals.begin(),
                      recorded_wheel_visuals.end(), WheelVisualState{});
            ghost_notices.clear();
            ghost_start_notice_enqueued = false;
            ghost_end_notice_enqueued = false;
          }
        } else if (recorded_presentation) {
          // p3.1 mode 6 applies MDE stream i directly to body slot i. Keep the
          // host/session step for opening and input, but do not simulate the
          // reconstruction-only grid vehicle hidden beneath playback.
        } else if (session.race().progress().phase ==
                   mh::game::RacePhase::countdown) {
          player_launch_wheelspin = 0.0;
          player_presentation_controls = staged_player_controls;
          vehicle.advance_drive_state_only(staged_player_controls,
                                           simulated_physics_seconds);
          for (auto &opponent : opponents) {
            // AI's retained positive-request accumulator is ready for the
            // first live control slice, but it must not free-rev every CPU
            // drivetrain throughout the locked start sequence. Hold the
            // opponent engines at idle until GO releases AI.
            opponent.vehicle->advance_drive_state_only(
                {}, simulated_physics_seconds);
            opponent.presentation_controls = {};
          }
        } else if (session.race().progress().phase ==
                       mh::game::RacePhase::racing ||
                   post_finish_schedule.active()) {
          std::array<mh::game::OriginalAiVehicleObservation, 8U>
              race_observations{};
          std::array<float, 8U> race_speeds{};
          if (!opponents.empty()) {
            race_observations[0U] = mh::game::original_ai_observe_vehicle(
                route, player_route_cursor, vehicle.ai_pose());
            race_speeds[0U] =
                static_cast<float>(vehicle.state().velocity.local_linear[2U]);
            hud_vehicle_speeds[0U] = race_speeds[0U];
            for (auto &opponent : opponents) {
              race_observations[opponent.slot_index] =
                  opponent.controller->observe(opponent.vehicle->ai_pose());
              race_speeds[opponent.slot_index] = static_cast<float>(
                  opponent.vehicle->state().velocity.local_linear[2U]);
              hud_vehicle_speeds[opponent.slot_index] =
                  race_speeds[opponent.slot_index];
            }
            const auto route_sample_count =
                static_cast<std::int32_t>(route.samples.size());
            for (std::size_t slot = 0U; slot < race_roster.size(); ++slot) {
              const auto sample = race_observations[slot].route_sample;
              hud_vehicle_samples[slot] = static_cast<std::size_t>(sample);
              if (!hud_samples_initialized) {
                hud_previous_samples[slot] = sample;
                auto initial_delta =
                    sample - race_observations[0U].route_sample;
                if (initial_delta < -route_sample_count / 2) {
                  initial_delta += route_sample_count;
                } else if (initial_delta > route_sample_count / 2) {
                  initial_delta -= route_sample_count;
                }
                hud_progress_samples[slot] = initial_delta;
              } else {
                auto delta = sample - hud_previous_samples[slot];
                if (delta < -route_sample_count / 2) {
                  delta += route_sample_count;
                } else if (delta > route_sample_count / 2) {
                  delta -= route_sample_count;
                }
                hud_progress_samples[slot] += delta;
                hud_previous_samples[slot] = sample;
              }
              catch_up_sample_progress[slot] =
                  static_cast<std::int64_t>(sample) +
                  static_cast<std::int64_t>(hud_completed_laps[slot]) *
                      route_sample_count;
            }
            if (!hud_samples_initialized) {
              hud_samples_initialized = true;
            }
            if (!ai_physics_capture_oracle && cpu_catch_up_percent > 0.0F) {
              std::iota(ranked_slots.begin(), ranked_slots.end(), 0U);
              const auto sort_ranked_slots = [&] {
                stable_insertion_sort(
                    ranked_slots,
                    [&catch_up_sample_progress, &hud_finish_times](
                        const std::size_t left, const std::size_t right) {
                      const auto left_finished =
                          hud_finish_times[left].has_value();
                      const auto right_finished =
                          hud_finish_times[right].has_value();
                      if (left_finished != right_finished) {
                        return left_finished;
                      }
                      if (left_finished) {
                        return *hud_finish_times[left] <
                               *hud_finish_times[right];
                      }
                      if (catch_up_sample_progress[left] !=
                          catch_up_sample_progress[right]) {
                        return catch_up_sample_progress[left] >
                               catch_up_sample_progress[right];
                      }
                      return false;
                    });
              };
              if (!catch_up_rank_initialized) {
                sort_ranked_slots();
                for (std::size_t rank = 0U; rank < ranked_slots.size();
                     ++rank) {
                  catch_up_rank_by_slot[ranked_slots[rank]] = rank;
                }
                catch_up_rank_initialized = true;
              }
              const auto factors_by_rank =
                  mh::game::original_cpu_catch_up_factors(
                      std::span<const float>(catch_up_progress_by_rank.data(),
                                             race_roster.size()),
                      catch_up_rank_by_slot[0U], cpu_catch_up_percent);
              for (auto &opponent : opponents) {
                opponent.vehicle->apply_drive_performance_scale(
                    factors_by_rank
                        [catch_up_rank_by_slot[opponent.slot_index]]);
              }
              for (std::size_t slot = 0U; slot < race_roster.size(); ++slot) {
                catch_up_progress_by_rank[catch_up_rank_by_slot[slot]] =
                    static_cast<float>(catch_up_sample_progress[slot]);
              }
              std::iota(ranked_slots.begin(), ranked_slots.end(), 0U);
              sort_ranked_slots();
              for (std::size_t rank = 0U; rank < ranked_slots.size(); ++rank) {
                catch_up_rank_by_slot[ranked_slots[rank]] = rank;
              }
            }
            const auto race_time = session.race().progress().timing.total +
                                   post_finish_schedule.elapsed();
            for (std::size_t slot = 1U; slot < race_roster.size(); ++slot) {
              while (hud_completed_laps[slot] <
                         session.race().config().lap_count &&
                     hud_progress_samples[slot] >=
                         static_cast<std::int64_t>(route_sample_count) *
                             static_cast<std::int64_t>(
                                 hud_completed_laps[slot] + 1U)) {
                const auto lap_time = race_time - hud_lap_started[slot];
                if (hud_best_laps[slot] ==
                        mh::game::SimulationDuration::zero() ||
                    lap_time < hud_best_laps[slot]) {
                  hud_best_laps[slot] = lap_time;
                }
                hud_lap_started[slot] = race_time;
                ++hud_completed_laps[slot];
                if (hud_completed_laps[slot] ==
                    session.race().config().lap_count) {
                  hud_finish_times[slot] = race_time;
                }
              }
            }
            hud_race_position = 1U;
            for (std::size_t slot = 1U; slot < race_roster.size(); ++slot) {
              if (hud_progress_samples[slot] > hud_progress_samples[0U]) {
                ++hud_race_position;
              }
            }
          }
          // The local input owner is disabled after the finish, while the
          // vehicle remains in the live field. Continue it with the same
          // source-backed route controller used by race AI so the retained
          // ten-second presentation follows the authored line.
          auto simulation_controls = command.controls;
          mh::game::OriginalAiControlOutput post_finish_player_controls{};
          auto post_finish_player_controls_ready = false;
          const auto player_audio_hull = retained_player_hull_audio;
          mh::game::OriginalVehicleModeCSceneFrame player_physics_frame;
          for (std::size_t body_index = 0U;
               body_index < environment_collision_bodies.size(); ++body_index) {
            auto &environment_body = environment_collision_bodies[body_index];
            const auto scene_object_index =
                environment_collision_body_assets[body_index]
                    .scene_object_index;
            if (mh::game::original_environment_scene_has_transform_motion(
                    environment_scene.objects[scene_object_index])) {
              // LWS objects whose position or rotation changes are kinematic.
              // Their scene path, not accumulated rigid velocity, owns the
              // next rendered pose. Attribute-only lamp keys remain movable.
              environment_body.body.velocity = {};
              environment_body.retained_body_hull_contact_count = 0U;
              continue;
            }
            environment_body.physics_previous_pose = environment_body.body.pose;
            if (environment_body.detached_from_authored_motion) {
              // Loose environment bodies share the global p3.1 vertical
              // acceleration selected for the race. Convert that world-space
              // velocity delta into the body's current local basis before the
              // generic damping/pose update. Omitting this left struck cones
              // carrying their upward contact impulse indefinitely.
              const auto gravity_world =
                  mh::game::calculate_original_world_gravity_delta(
                      mh::game::select_original_global_vertical_scale(
                          pc_cheats.mask),
                      simulated_physics_seconds);
              const auto gravity_local = mh::game::project_world_vector_to_body(
                  environment_body.body.pose.body_basis, gravity_world);
              for (std::size_t axis = 0U;
                   axis < environment_body.body.velocity.local_linear.size();
                   ++axis) {
                environment_body.body.velocity.local_linear[axis] +=
                    gravity_local[axis];
              }
            }
            static_cast<void>(mh::game::advance_original_body_pose(
                environment_body.body.pose, environment_body.body.velocity,
                {environment_body.retained_body_hull_contact_count,
                 simulated_physics_seconds}));
          }
          std::array<float, 4U> player_retained_wheel_states{};

          if (!opponents.empty()) {
            const auto ai_step_seconds =
                ai_control_report_path.has_value()
                    ? static_cast<double>(
                          ai_opening_time_step(ai_opening_cycle))
                    : simulated_physics_seconds;
            const auto &initializer =
                mh::game::original_goldbridge_quick_race_ai_initializer_slots();
            for (std::size_t slot = 0U; slot < traffic_vehicles.size();
                 ++slot) {
              const auto *controller =
                  slot == 0U ? (post_finish_schedule.active()
                                    ? &post_finish_player_controller
                                    : nullptr)
                             : opponents[slot - 1U].controller.get();
              traffic_vehicles[slot] = {
                  slot,
                  race_observations[slot],
                  race_speeds[slot],
                  controller == nullptr ? initializer[slot].tuning.half_width
                                        : controller->tuning().half_width,
                  controller == nullptr ? initializer[slot].tuning.half_length
                                        : controller->tuning().half_length,
                  controller == nullptr
                      ? 0.0F
                      : controller->controller_state().target_lateral_offset};
            }
            // Retail and p3.1 stably sort the live-car pointer array from
            // greatest route record/projection to least. The player remains a
            // non-controller traffic participant until its finish state hands
            // the car to the route controller for the retained live interval.
            std::iota(sorted_live_slots.begin(), sorted_live_slots.end(), 0U);
            stable_insertion_sort(
                sorted_live_slots,
                [&race_observations](const std::size_t left,
                                     const std::size_t right) {
                  const auto &left_observation = race_observations[left];
                  const auto &right_observation = race_observations[right];
                  if (left_observation.route_sample !=
                      right_observation.route_sample) {
                    return left_observation.route_sample >
                           right_observation.route_sample;
                  }
                  return left_observation.route_longitudinal_projection >
                         right_observation.route_longitudinal_projection;
                });
            for (std::size_t sorted_order = 0U;
                 sorted_order < sorted_live_slots.size(); ++sorted_order) {
              const auto self_slot = sorted_live_slots[sorted_order];
              auto *controller =
                  self_slot == 0U ? (post_finish_schedule.active()
                                         ? &post_finish_player_controller
                                         : nullptr)
                                  : opponents[self_slot - 1U].controller.get();
              if (controller == nullptr) {
                continue;
              }
              auto *opponent =
                  self_slot == 0U ? nullptr : &opponents[self_slot - 1U];
              auto positive_side_occupied = false;
              auto negative_side_occupied = false;
              for (const auto other_slot : sorted_live_slots) {
                if (other_slot == self_slot) {
                  continue;
                }
                const auto side = mh::game::original_ai_classify_nearby_side(
                    {race_observations[self_slot].route_sample -
                         race_observations[other_slot].route_sample,
                     controller->tuning().stochastic_base_a,
                     controller->stochastic_state().channel_a,
                     race_observations[self_slot]
                             .route_frame.measured_lateral_offset -
                         race_observations[other_slot]
                             .route_frame.measured_lateral_offset,
                     controller->tuning().half_width,
                     traffic_vehicles[other_slot].half_width});
                positive_side_occupied =
                    positive_side_occupied ||
                    side == mh::game::OriginalAiNearbySide::positive;
                negative_side_occupied =
                    negative_side_occupied ||
                    side == mh::game::OriginalAiNearbySide::negative;
              }

              std::array<mh::game::OriginalAiAvoidanceOpponent, 7U>
                  avoidance_opponents{};
              auto opponent_index = std::size_t{0U};
              for (auto other_it = sorted_live_slots.rbegin();
                   other_it != sorted_live_slots.rend(); ++other_it) {
                const auto other_slot = *other_it;
                if (other_slot == self_slot) {
                  continue;
                }
                const auto *other_controller =
                    other_slot == 0U
                        ? (post_finish_schedule.active()
                               ? &post_finish_player_controller
                               : nullptr)
                        : opponents[other_slot - 1U].controller.get();
                const auto &other_tuning = traffic_vehicles[other_slot];
                avoidance_opponents[opponent_index++] = {
                    true,
                    race_observations[other_slot].route_sample,
                    race_speeds[other_slot],
                    other_tuning.half_width,
                    other_tuning.half_length,
                    race_observations[other_slot]
                        .route_frame.direction_alignment,
                    other_controller == nullptr
                        ? 0.0F
                        : other_controller->controller_state()
                              .target_lateral_offset,
                    race_observations[other_slot]
                        .route_frame.measured_lateral_offset};
              }
              const auto state_before = controller->controller_state();
              const auto stochastic_before = controller->stochastic_state();
              const auto controlled_pose = opponent == nullptr
                                               ? vehicle.ai_pose()
                                               : opponent->vehicle->ai_pose();
              const auto ai = controller->step(
                  controlled_pose, race_speeds[self_slot],
                  static_cast<float>(ai_step_seconds), avoidance_opponents,
                  positive_side_occupied, negative_side_occupied);
              if (opponent == nullptr) {
                post_finish_player_controls = ai.controls;
                post_finish_player_controls_ready = true;
              } else {
                opponent->controls = ai.controls;
              }
              if (opponent != nullptr && ai_control_report_path.has_value() &&
                  ai_opening_cycle < ai_opening_report_cycles) {
                ai_opening_records.push_back(
                    {ai_opening_cycle,
                     sorted_order,
                     self_slot,
                     race_observations[self_slot],
                     race_speeds[self_slot],
                     static_cast<float>(ai_step_seconds),
                     {static_cast<float>(controlled_pose.world_position[0U]),
                      static_cast<float>(controlled_pose.world_position[2U])},
                     {static_cast<float>(controlled_pose.body_basis[2U][0U]),
                      static_cast<float>(controlled_pose.body_basis[2U][2U])},
                     opponent->vehicle->state().pose,
                     opponent->vehicle->state().velocity,
                     opponent->vehicle->drive().state(),
                     opponent->retained_wheel_states,
                     opponent->retained_wheel_contact_flags,
                     opponent->preceding_response_velocities,
                     opponent->preceding_accumulated_forces,
                     opponent->preceding_response_frames,
                     opponent->preceding_post_pose_states,
                     opponent->preceding_post_pose_velocities,
                     opponent->preceding_grounded_damping,
                     opponent->preceding_drivetrain,
                     opponent->preceding_body_hull_reaction_count,
                     opponent->preceding_dynamic_contacts,
                     state_before,
                     stochastic_before,
                     ai,
                     controller->controller_state(),
                     controller->stochastic_state()});
              }
            }
            if (ai_control_report_path.has_value() &&
                ai_opening_cycle < ai_opening_report_cycles) {
              ++ai_opening_cycle;
              if (ai_opening_cycle == ai_opening_report_cycles) {
                running = false;
              }
            }
          }
          if (post_finish_schedule.active() &&
              !post_finish_player_controls_ready) {
            const auto ai = post_finish_player_controller.step(
                vehicle.ai_pose(),
                static_cast<float>(vehicle.state().velocity.local_linear[2U]),
                static_cast<float>(simulated_physics_seconds));
            post_finish_player_controls = ai.controls;
            post_finish_player_controls_ready = true;
          }
          if (post_finish_player_controls_ready) {
            simulation_controls = {
                static_cast<double>(post_finish_player_controls.throttle),
                static_cast<double>(post_finish_player_controls.brake),
                static_cast<double>(post_finish_player_controls.steering)};
          }
          player_launch_wheelspin = 0.0;
          if (!post_finish_schedule.active()) {
            const auto launch = player_launch_traction.step(
                simulation_controls, vehicle.state().velocity.local_linear[2U],
                simulated_physics_seconds);
            simulation_controls = launch.controls;
            player_launch_wheelspin = launch.wheelspin;
          }
          player_presentation_controls = simulation_controls;
          vehicle.begin_interleaved_collision_step(simulation_controls,
                                                   simulated_physics_seconds);
          for (auto &opponent : opponents) {
            auto live_controls = opponent.controls;
            mh::game::ControlInput opponent_simulation_controls{
                static_cast<double>(live_controls.throttle),
                static_cast<double>(live_controls.brake),
                static_cast<double>(live_controls.steering)};
            if (networked_race) {
              const auto peer = network_peer_by_slot[opponent.slot_index];
              if (multiplayer_session->is_host() &&
                  peer < network_controls.size()) {
                opponent_simulation_controls = network_controls[peer];
              } else {
                opponent_simulation_controls = {};
              }
            }
            opponent.presentation_controls = opponent_simulation_controls;
            opponent.vehicle->begin_interleaved_collision_step(
                opponent_simulation_controls, simulated_physics_seconds);
          }
          // p3.1 schedules each live body as the outer owner after all vehicle
          // force steps. The reconstructed scene supplies the successful
          // static-pass count that controls this loop; the dynamic portion
          // scans the complete live-body list from its beginning on each pass.
          if (ai_control_report_path.has_value()) {
            for (auto &opponent : opponents) {
              opponent.preceding_dynamic_contacts.clear();
            }
          }
          contact_participants.clear();
          contact_live_slots.clear();
          contact_opponents.clear();
          contact_participants.push_back({&vehicle, &player_contact_shape, 0U});
          contact_live_slots.push_back(0U);
          contact_opponents.push_back(nullptr);
          for (auto &opponent : opponents) {
            contact_participants.push_back(
                {opponent.vehicle.get(), &opponent.contact_shape, 0U});
            contact_live_slots.push_back(opponent.slot_index);
            contact_opponents.push_back(&opponent);
          }
          auto contact_schedule =
              mh::game::apply_original_interleaved_vehicle_contact_schedule(
                  contact_participants);
          environment_contact_participants.clear();
          for (std::size_t body_index = 0U;
               body_index < environment_collision_bodies.size(); ++body_index) {
            auto &runtime = environment_collision_bodies[body_index];
            environment_contact_participants.push_back(
                {&runtime.body,
                 &environment_collision_body_assets[body_index].shape,
                 runtime.collision_identifier});
          }
          environment_vehicle_participants.clear();
          constexpr double original_environment_vehicle_radius = 150.0;
          for (const auto &participant : contact_participants) {
            const auto &position =
                participant.vehicle->state().pose.world_position;
            const auto dx = position[0U] - environment_collision_focus[0U];
            const auto dy = position[1U] - environment_collision_focus[1U];
            const auto dz = position[2U] - environment_collision_focus[2U];
            if (std::sqrt(dx * dx + dy * dy + dz * dz) <
                original_environment_vehicle_radius) {
              environment_vehicle_participants.push_back(participant);
            }
          }
          const auto environment_contact_schedule =
              mh::game::apply_original_vehicle_environment_contact_schedule(
                  environment_vehicle_participants,
                  environment_contact_participants);
          for (const auto &event : environment_contact_schedule.events) {
            const auto scene_object_index =
                environment_collision_body_assets.at(event.environment_index)
                    .scene_object_index;
            const auto &scene_object =
                environment_scene.objects.at(scene_object_index);
            // Transform-static LWS bodies are loose placed scenery and hand
            // off to rigid-body motion after contact, even when separate keys
            // animate lamp attributes. True position/rotation paths remain
            // kinematic: permanently detaching one member of an authored
            // assembly made Okkun's looping rotor freeze in space while its
            // matched fuselage path continued.
            if (!mh::game::original_environment_scene_has_transform_motion(
                    scene_object)) {
              environment_collision_bodies.at(event.environment_index)
                  .detached_from_authored_motion = true;
            }
          }
          for (std::size_t body_index = 0U;
               body_index < environment_collision_bodies.size(); ++body_index) {
            auto &runtime = environment_collision_bodies[body_index];
            const auto scene_object_index =
                environment_collision_body_assets[body_index]
                    .scene_object_index;
            if (mh::game::original_environment_scene_has_transform_motion(
                    environment_scene.objects[scene_object_index])) {
              // The static track must not block an authored looping path.
              // Vehicle contacts remain active in the direct-body pass above.
              runtime.retained_body_hull_contact_count = 0U;
              continue;
            }
            const auto static_contact = mh::game::
                apply_original_environment_body_static_contact_schedule(
                    runtime.body,
                    environment_collision_body_assets[body_index].shape,
                    runtime.physics_previous_pose, vehicle.world(),
                    simulated_physics_seconds);
            runtime.retained_body_hull_contact_count =
                static_contact.retained_body_hull.retained_contact_count;
          }
          if (audio_enabled && environment_sound.has_value()) {
            for (std::size_t vehicle_index = 0U;
                 vehicle_index <
                 environment_contact_schedule.vehicle_collisions.size();
                 ++vehicle_index) {
              const auto &collision = environment_contact_schedule
                                          .vehicle_collisions[vehicle_index];
              if (collision.active) {
                environment_sound->trigger_collision(
                    vehicle_index, collision.collision_identifier,
                    collision.maximum_collision_sound_scalar);
              }
            }
          }
          if (ai_control_report_path.has_value()) {
            for (const auto &event : contact_schedule.events) {
              auto *first = contact_opponents[event.first_index];
              auto *second = contact_opponents[event.second_index];
              if (first != nullptr) {
                first->preceding_dynamic_contacts.push_back(
                    {event.pass, contact_live_slots[event.second_index], true,
                     event.response});
              }
              if (second != nullptr) {
                second->preceding_dynamic_contacts.push_back(
                    {event.pass, contact_live_slots[event.first_index], false,
                     event.response});
              }
            }
          }
          player_physics_frame = std::move(contact_schedule.frames[0U]);
          if (skid_marks_enabled) {
            update_skid_marks(player_skid_marks, player_physics_frame,
                              vehicle.state().velocity, grounded_materials,
                              player_launch_wheelspin);
          }
          if (smoke_enabled) {
            update_tire_smoke(player_tire_smoke, player_physics_frame,
                              simulated_physics_seconds,
                              player_launch_wheelspin);
          }
          if (sparks_enabled) {
            update_sparks(player_sparks, player_physics_frame,
                          grounded_materials, simulated_physics_seconds);
          }
          if (material_sound.has_value()) {
            material_sound->update(player_physics_frame,
                                   vehicle.state().velocity, grounded_materials,
                                   player_audio_hull);
          }
          retained_player_hull_audio =
              player_physics_frame.scene.hull_audio_record;
          for (std::size_t wheel = 0U;
               wheel < player_retained_wheel_states.size(); ++wheel) {
            player_retained_wheel_states[wheel] = static_cast<float>(
                player_physics_frame.scene.response.contacts.wheels[wheel]
                    .scalar_state.state_fraction);
          }
          update_wheel_visual(
              player_wheels, vehicle.state().velocity.local_linear[2U],
              vehicle.drive().state().steering_scalar,
              player_retained_wheel_states, simulated_physics_seconds,
              player_launch_wheelspin);
          for (std::size_t opponent_index = 0U;
               opponent_index < opponents.size(); ++opponent_index) {
            auto &opponent = opponents[opponent_index];
            opponent.physics_frame =
                std::move(contact_schedule.frames[opponent_index + 1U]);
            const auto &physics_frame = opponent.physics_frame;
            for (std::size_t substep = 0U;
                 substep < opponent.preceding_response_velocities.size();
                 ++substep) {
              opponent.preceding_response_velocities[substep] =
                  physics_frame.scene.vehicle_response_substeps[substep]
                      .velocity;
              opponent.preceding_accumulated_forces[substep] =
                  physics_frame.scene.vehicle_response_substeps[substep]
                      .accumulated_force;
              opponent.preceding_response_frames[substep] =
                  physics_frame.scene.vehicle_response_substeps[substep];
              opponent.preceding_post_pose_states[substep] =
                  physics_frame.scene.vehicle_post_pose_states[substep];
              opponent.preceding_post_pose_velocities[substep] =
                  physics_frame.scene.vehicle_post_pose_velocities[substep];
            }
            opponent.preceding_grounded_damping =
                physics_frame.scene.grounded_damping;
            opponent.preceding_drivetrain = physics_frame.drivetrain;
            opponent.preceding_body_hull_reaction_count =
                physics_frame.scene.body_hull_reactions.size();
            for (std::size_t wheel = 0U;
                 wheel < opponent.retained_wheel_states.size(); ++wheel) {
              const auto &contact =
                  physics_frame.scene.response.contacts.wheels[wheel];
              opponent.retained_wheel_states[wheel] =
                  static_cast<float>(contact.scalar_state.state_fraction);
              opponent.retained_wheel_contact_flags[wheel] =
                  contact.hit.has_value() ? 1U : 0U;
            }
            update_wheel_visual(
                opponent.wheels,
                opponent.vehicle->state().velocity.local_linear[2U],
                opponent.vehicle->drive().state().steering_scalar,
                opponent.retained_wheel_states, simulated_physics_seconds);
            if (skid_marks_enabled) {
              update_skid_marks(opponent.skid_marks, physics_frame,
                                opponent.vehicle->state().velocity,
                                grounded_materials);
            }
            if (smoke_enabled) {
              update_tire_smoke(opponent.tire_smoke, physics_frame,
                                simulated_physics_seconds);
            }
            if (sparks_enabled) {
              update_sparks(opponent.sparks, physics_frame, grounded_materials,
                            simulated_physics_seconds);
            }
          }
          const auto player_recovery_pose = vehicle.physics_pose();
          const auto player_recovery_direction =
              mh::game::original_race_recovery_spline_direction(
                  motion, player_recovery_pose.world_position);
          const auto player_recovery = vehicle.update_race_recovery(
              player_physics_frame, {false,
                                     false,
                                     false,
                                     true,
                                     false,
                                     {},
                                     player_recovery_direction,
                                     simulated_physics_seconds,
                                     std::nullopt,
                                     true});
          if (player_recovery.reset_applied) {
            retained_player_hull_audio = {};
            for (auto &previous : player_skid_marks.previous) {
              previous.reset();
            }
            player_tire_smoke = {};
            player_sparks = {};
          }
          // Retail runs the per-live race recovery owner only after the
          // shared physics/collision scheduler has completed all body passes.
          for (auto &opponent : opponents) {
            const auto recovery_pose = opponent.vehicle->physics_pose();
            const auto recovery_direction =
                mh::game::original_race_recovery_spline_direction(
                    motion, recovery_pose.world_position);
            const auto recovery_observation =
                opponent.controller->observe(opponent.vehicle->ai_pose());
            const auto route_sample =
                static_cast<std::size_t>(recovery_observation.route_sample);
            const auto &recovery_route = route.samples[route_sample];
            const auto outside_route = mh::game::ai_competitor_outside_route(
                recovery_observation.route_frame.measured_lateral_offset,
                recovery_route.surface_values[0U],
                recovery_route.surface_values[1U],
                opponent.controller->tuning().half_width);
            const auto recovery_position =
                outside_route
                    ? std::optional<std::array<
                          double, 3U>>{mh::game::
                                           race_recovery_spline_position(
                                               motion,
                                               recovery_pose.world_position)}
                    : std::nullopt;
            const auto recovery = opponent.vehicle->update_race_recovery(
                opponent.physics_frame,
                {true,
                 opponent.controller->controller_state().stuck_recovery.active,
                 outside_route, true, false,
                 vehicle.physics_pose().world_position, recovery_direction,
                 simulated_physics_seconds, recovery_position});
            if (recovery.reset_applied) {
              const auto recovered_pose = opponent.vehicle->ai_pose();
              const std::array<float, 2U> recovered_xz{
                  static_cast<float>(recovered_pose.world_position[0U]),
                  static_cast<float>(recovered_pose.world_position[2U])};
              opponent.controller->reset(recovered_xz);
              opponent.controller->initialize_lateral_target(recovered_pose);
              static_cast<void>(opponent.controller->observe(recovered_pose));
              for (auto &previous : opponent.skid_marks.previous) {
                previous.reset();
              }
              opponent.tire_smoke = {};
            }
          }
          const auto &player_world_position =
              vehicle.state().pose.world_position;
          const auto race_update =
              player_race_progress.update(player_world_position);
          if (checkpoint_display_remaining >
              mh::game::SimulationDuration::zero()) {
            checkpoint_display_remaining =
                std::max(mh::game::SimulationDuration::zero(),
                         checkpoint_display_remaining - physics_duration);
          }
          if (checkpoint_info_mode != mh::ui::GraphicInfoMode::none &&
              checkpoint_display_time_ms != 0U &&
              race_update.checkpoint_crossed) {
            checkpoint_display_remaining =
                std::chrono::milliseconds(checkpoint_display_time_ms);
          }
          player_engine_track_mix = mh::game::original_race_spline_engine_mix(
              motion, player_world_position, race_update.current_key);
          if (race_update.lap_completed) {
            if (session.cross_finish_line()) {
              if (session.race().progress().phase ==
                  mh::game::RacePhase::finished) {
                finish_sound_requested = true;
                post_finish_schedule.begin();
                const auto finish_pose = vehicle.ai_pose();
                const std::array<float, 2U> finish_xz{
                    static_cast<float>(finish_pose.world_position[0U]),
                    static_cast<float>(finish_pose.world_position[2U])};
                post_finish_player_controller.reset(finish_xz);
                post_finish_player_controller.initialize_lateral_target(
                    finish_pose);
              }
            }
          }
          if (networked_race && multiplayer_session->is_host() &&
              (++network_snapshot_divider % 2U) == 0U) {
            (void)multiplayer_session->publish_snapshot(
                make_network_snapshot());
            network_control_state_dirty = false;
          }
          if (post_finish_schedule.results_due()) {
            prepare_result_frame();
            break;
          }
        }
      }
      if (outer_frame_active) {
        vehicle.end_outer_frame();
        for (auto &opponent : opponents) {
          opponent.vehicle->end_outer_frame();
        }
      }

      const auto replay_phase = session.race().progress().phase;
      const auto replay_should_be_visible =
          recorded_presentation || replay_phase == mh::game::RacePhase::ready ||
          replay_phase == mh::game::RacePhase::countdown ||
          replay_phase == mh::game::RacePhase::racing;
      if (ghost_playback.has_value() && replay_should_be_visible) {
        const auto ticks = SDL_GetTicks();
        if (pause_visible) {
          // Physics, opponents, race timing, and the authored flyby are
          // fixed-step gated above. Preserve the last MDE samples and keep
          // rebasing its wall clock so Resume cannot consume time spent here.
          if (ghost_started_ticks.has_value()) {
            ghost_previous_ticks = ticks;
          }
        } else {
          if (!ghost_started_ticks.has_value()) {
            const auto start_milliseconds =
                replay_phase == mh::game::RacePhase::racing
                    ? ghost_launch_milliseconds
                    : ghost_countdown_start_milliseconds;
            ghost_playback->reset(start_milliseconds);
            ghost_started_ticks = ticks;
            ghost_previous_ticks = ticks;
            if (!ghost_start_notice_enqueued && mde_presentation.has_value()) {
              ghost_notices.enqueue(mde_presentation->start_notice, ticks);
              ghost_start_notice_enqueued = true;
            }
          }
          auto delta = static_cast<std::uint32_t>(std::min<std::uint64_t>(
              ticks - ghost_previous_ticks,
              std::numeric_limits<std::uint32_t>::max()));
          ghost_previous_ticks = ticks;
          const auto holding_recorded_countdown =
              replay_phase != mh::game::RacePhase::racing &&
              ghost_countdown_hold_remaining_milliseconds != 0U;
          if (holding_recorded_countdown) {
            const auto held =
                std::min(delta, ghost_countdown_hold_remaining_milliseconds);
            delta -= held;
            ghost_countdown_hold_remaining_milliseconds -= held;
          }
          const auto previous_playback_milliseconds =
              ghost_playback->milliseconds();
          std::vector<mh::game::OriginalMdeBodyPlaybackSample>
              next_ghost_samples;
          if (holding_recorded_countdown && delta == 0U) {
            next_ghost_samples = ghost_playback->current_samples();
          } else if (benchmark_presentation &&
                     replay_phase != mh::game::RacePhase::racing) {
            // Benchmark's recovered +30-ms clock is the measured-race clock,
            // not a license to accelerate the 12-second start sequence at an
            // uncapped modern frame rate. During the flyby/countdown, retain
            // the same wall-clock MDE alignment as Replay; GO resets to the
            // measured launch sample and enables fixed-step benchmarking.
            const auto current = ghost_playback->milliseconds();
            const auto target =
                static_cast<std::uint32_t>(std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(current) + delta,
                    std::numeric_limits<std::uint32_t>::max()));
            ghost_playback->reset(target);
            next_ghost_samples = ghost_playback->current_samples();
          } else {
            next_ghost_samples = ghost_playback->advance(delta);
          }
          if (!next_ghost_samples.empty()) {
            const auto playback_seconds =
                static_cast<double>(ghost_playback->milliseconds() -
                                    previous_playback_milliseconds) /
                1000.0;
            for (const auto &sample : next_ghost_samples) {
              if (sample.stream_index >= recorded_wheel_visuals.size()) {
                throw std::runtime_error(
                    "MDE playback stream has no wheel presentation state");
              }
              auto &wheels = recorded_wheel_visuals[sample.stream_index];
              wheels.steering = mh::game::original_front_wheel_visual_angle(
                  sample.decoded_state.body_d8_normalized);
              wheels.suspension_states = sample.decoded_state.wheel_history;
              advance_wheel_visual_spin(
                  wheels, sample.decoded_state.body_e4_bounded,
                  sample.decoded_state.body_e8_bounded, playback_seconds);
            }
            ghost_samples = std::move(next_ghost_samples);
          } else if (!ghost_playback->active()) {
            if (!ghost_end_notice_enqueued && mde_presentation.has_value()) {
              ghost_notices.enqueue(mde_presentation->end_notice, ticks);
              ghost_end_notice_enqueued = true;
            }
            if (recorded_presentation) {
              benchmark_completed = benchmark_presentation;
              exit_code = 4;
              running = false;
            }
          }
        }
      } else {
        ghost_samples.clear();
      }

      const auto benchmark_frame_active =
          benchmark_presentation && ghost_playback.has_value() &&
          ghost_playback->active() &&
          replay_phase == mh::game::RacePhase::racing && !ghost_samples.empty();
      if (benchmark_frame_active && !benchmark_measurement_start.has_value()) {
        benchmark_measurement_start = BenchmarkClock::now();
      }

      using RendererProfileClock = std::chrono::steady_clock;
      const auto renderer_profile_enabled =
          renderer_profile_report_path.has_value();
      const auto render_profile_start =
          renderer_profile_enabled ? RendererProfileClock::now()
                                   : RendererProfileClock::time_point{};
      const auto elapsed_profile_ms = [](const auto start, const auto end) {
        return std::chrono::duration<double, std::milli>(end - start).count();
      };
      int output_width = 0;
      int output_height = 0;
      require(SDL_GetRenderOutputSize(renderer.get(), &output_width,
                                      &output_height),
              "query render size");
      const auto borderless_presentation =
          active_window_mode == mh::ui::GraphicWindowMode::borderless;
      const auto width = borderless_presentation ? window_width : output_width;
      const auto height =
          borderless_presentation ? window_height : output_height;
      const auto borderless_destination =
          borderless_presentation
              ? std::optional<SDL_FRect>(fitted_presentation_rect(
                    width, height, output_width, output_height))
              : std::nullopt;
      const auto presentation_fills_output =
          !borderless_destination.has_value() ||
          (std::abs(borderless_destination->x) < 0.01F &&
           std::abs(borderless_destination->y) < 0.01F &&
           std::abs(borderless_destination->w -
                    static_cast<float>(output_width)) < 0.01F &&
           std::abs(borderless_destination->h -
                    static_cast<float>(output_height)) < 0.01F);
      const auto underwater_mode =
          pc_cheats.active(mh::game::OriginalPcCheat::underwater);
      const auto direct_presentation =
          renderer_true_colour && !motion_blur_enabled && !underwater_mode &&
          width == output_width && height == output_height &&
          presentation_fills_output;
      begin_presentation_back_buffer(
          renderer.get(), presentation_back_buffers, width, height,
          renderer_triple_buffer || motion_blur_enabled ? 2U : 1U,
          renderer_true_colour ? SDL_PIXELFORMAT_RGBA32
                               : SDL_PIXELFORMAT_RGB565,
          direct_presentation);
      const auto clear_color =
          result_visible ? mh::content::TrackRgbColor{0U, 0U, 0U}
                         : track_definition.environment.bottom_color.value_or(
                               mh::content::TrackRgbColor{7U, 12U, 20U});
      const auto tron_mode = pc_cheats.active(mh::game::OriginalPcCheat::tron);
      require(SDL_SetRenderDrawColor(renderer.get(),
                                     tron_mode ? 0U : clear_color.red,
                                     tron_mode ? 0U : clear_color.green,
                                     tron_mode ? 0U : clear_color.blue, 255U),
              "set clear color");
      require(SDL_RenderClear(renderer.get()), "clear frame");
      if (result_visible) {
        if (audio_enabled && music.has_value()) {
          music->refill();
        }
        if (audio_enabled && race_sound.has_value() && finish_sound_requested) {
          race_sound->trigger_finish();
        }
        const auto destination = result_presentation_rect(width, height);
        require(SDL_RenderTexture(renderer.get(), result_texture.get(), nullptr,
                                  &destination),
                "render race-results frame");
        present_presentation_back_buffer(
            renderer.get(), presentation_back_buffers, false, 0.0,
            borderless_destination.has_value() ? &*borderless_destination
                                               : nullptr);
        pace_stable_present();
        require(SDL_RenderPresent(renderer.get()),
                "present race-results frame");
        complete_stable_present();
        ++rendered_frames;
        if (maximum_frames != 0U && rendered_frames >= maximum_frames) {
          running = false;
        }
        continue;
      }
      const auto pose = mh::game::interpolate_vehicle_pose(
          vehicle.previous_pose(), vehicle.state().pose, 1.0);
      if (!recorded_presentation) {
        for (auto &opponent : opponents) {
          opponent.presentation_pose = mh::game::interpolate_vehicle_pose(
              opponent.vehicle->previous_pose(),
              opponent.vehicle->state().pose, 1.0);
        }
      }
      const auto elapsed_seconds =
          pause_visible ? 0.0 : std::chrono::duration<double>(elapsed).count();
      const auto &player_visual_pose = pose;
      active_location_easter_egg = mh::game::original_pc_location_easter_egg(
          track_definition.name, pose.world_position);
      if (active_location_easter_egg !=
          mh::game::OriginalPcLocationEasterEgg::atlantika_scroll) {
        atlantika_scroll_frames = 0U;
      }
      if (active_location_easter_egg ==
          mh::game::OriginalPcLocationEasterEgg::black_lotus_club) {
        if (black_lotus_dwell_frames < 202U) {
          ++black_lotus_dwell_frames;
        }
      } else {
        black_lotus_dwell_frames = 0U;
      }
      thunder_effect_seconds += elapsed_seconds;
      if (recorded_presentation && !ghost_samples.empty() &&
          (!mde_presentation.has_value() ||
           ghost_samples.front().body_slot !=
               mde_presentation->first_body_slot)) {
        throw std::runtime_error(
            "recorded presentation camera body slot diverged from p3.1");
      }
      const mh::game::OriginalMdeBodyPlaybackSample *recorded_camera_sample =
          nullptr;
      if (recorded_presentation && !ghost_samples.empty()) {
        const auto found =
            std::find_if(ghost_samples.begin(), ghost_samples.end(),
                         [recorded_camera_body_slot](const auto &sample) {
                           return sample.body_slot == recorded_camera_body_slot;
                         });
        if (found == ghost_samples.end()) {
          throw std::runtime_error(
              "recorded presentation has no p3.1 camera body slot");
        }
        recorded_camera_sample = &*found;
      }
      const auto body_mounted_camera =
          session.camera_mode() == mh::game::VehicleCameraMode::in_car ||
          session.camera_mode() == mh::game::VehicleCameraMode::bumper;
      const auto camera_pose =
          recorded_camera_sample != nullptr
              ? recorded_camera_sample->ghost.pose
              : (body_mounted_camera && camera_shake_enabled ? player_visual_pose
                                                             : pose);
      mh::game::VehicleCameraPose camera;
      if (!recorded_presentation &&
          pc_cheats.active(mh::game::OriginalPcCheat::supercars_camera)) {
        camera = mh::game::make_original_supercars_camera(
            pose, camera_settings.supercars);
        vehicle_camera_runtime.seed(camera);
      } else if (!recorded_presentation &&
                 pc_cheats.active(mh::game::OriginalPcCheat::ignition_camera)) {
        // Keep the tuned bird's-eye owner rigidly behind the car. The
        // recovered retained-X response is unsuitable here because it turns
        // the view into a side perspective during direction changes.
        camera = mh::game::make_original_ignition_camera(
            pose, camera_settings.ignition);
        vehicle_camera_runtime.seed(camera);
      } else if (recorded_presentation) {
        if (recorded_camera_view_active) {
          if (!recorded_camera_motion.has_value()) {
            throw std::runtime_error(
                "recorded CameraView has no authored camera motion");
          }
          camera = mh::game::make_original_camera_spline_camera(
              *recorded_camera_motion, camera_pose);
          vehicle_camera_runtime.seed(camera);
        } else {
          camera = vehicle_camera_runtime.step(
              camera_pose, session.camera_mode(), elapsed_seconds, {},
              camera_settings.playable);
        }
      } else if (start_flyby.active(session.race().progress())) {
        camera = start_flyby.camera(session.race().progress(), pose,
                                    camera_settings.playable);
        vehicle_camera_runtime.seed(camera);
      } else if (presentation_rear_view_held) {
        camera = mh::game::make_original_rear_view_camera(pose);
      } else {
        camera = vehicle_camera_runtime.step(camera_pose, session.camera_mode(),
                                             elapsed_seconds, {},
                                             camera_settings.playable);
      }
      environment_collision_focus = camera.world_position;
      const auto view = make_perspective_view(camera, width, height);
      auto background_view = view;
      if (pc_cheats.active(mh::game::OriginalPcCheat::supercars_camera)) {
        // The recovered Supercars owner looks straight down, so its camera
        // forward has no horizontal component. The retail cylindrical sky
        // still retains the followed car's heading; provide that heading to
        // the background adapter while leaving the actual perspective view
        // and all world projection strictly top-down.
        background_view.forward[0U] = pose.body_basis[2U][0U];
        background_view.forward[2U] = pose.body_basis[2U][2U];
      }
      const auto background_profile_start =
          renderer_profile_enabled ? RendererProfileClock::now()
                                   : RendererProfileClock::time_point{};
      if (background_enabled && !tron_mode) {
        if (!track_definition.environment.horizon.has_value()) {
          throw std::runtime_error(
              "active track definition has no authored Horizon value");
        }
        const auto thunder_mode =
            pc_cheats.active(mh::game::OriginalPcCheat::thunder);
        bool thunder_flash = false;
        if (thunder_mode) {
          // The p3.1 renderer calls the Watcom/MSVC-compatible LCG once and
          // flashes only for the strict interval (20000, 20400).
          thunder_random_state = thunder_random_state * 0x41c64e6dU + 0x3039U;
          const auto random_value = (thunder_random_state >> 16U) & 0x7fffU;
          thunder_flash = random_value > 20000U && random_value < 20400U;
        }
        render_background(
            renderer.get(), background_visual, background_view, width, height,
            *track_definition.environment.horizon,
            track_definition.environment.top_color.value_or(clear_color),
            thunder_mode, thunder_effect_seconds, thunder_flash);
      }
      if (render_environment.cue_enabled && !tron_mode) {
        render_horizon_fog(renderer.get(), width, height, view.center_y,
                           render_environment.cue_color);
      }
      const auto background_profile_end =
          renderer_profile_enabled ? RendererProfileClock::now()
                                   : RendererProfileClock::time_point{};
      scene_depth.begin_frame(renderer.get(), width, height, view.near_plane);
      if (!scene_renderer_logged) {
        mh::common::log_runtime_info(
            "Active race scene renderer: " +
            std::string(renderer_backend_name(scene_depth.backend())));
        scene_renderer_logged = true;
      }
      const auto depth_clear_profile_end =
          renderer_profile_enabled ? RendererProfileClock::now()
                                   : RendererProfileClock::time_point{};
      const auto &drive_state = vehicle.drive().state();
      if (audio_enabled) {
        if (music.has_value()) {
          music->refill();
        }
        if (pause_menu_loop.has_value()) {
          pause_menu_loop->refill();
        }
        if (engine_sound.has_value()) {
          const auto mix = mh::game::update_engine_audio_mix(
              engine_audio_mix, drive_state.current_gear_index,
              drive_state.engine_scalar, player_engine_track_mix);
          const auto engine_active =
              !pause_visible &&
              (session.race().progress().phase ==
                   mh::game::RacePhase::countdown ||
               session.race().progress().phase == mh::game::RacePhase::racing ||
               post_finish_schedule.active());
          if (engine_enabled) {
            engine_sound->update(
                mix, engine_active,
                mh::game::player_engine_camera_gain(
                    start_flyby.active(session.race().progress())
                        ? mh::game::VehicleCameraMode::chase
                        : session.camera_mode()));
          }
          if (opponent_engine_sound.has_value() && engine_enabled) {
            opponent_audio_frames.clear();
            for (const auto &opponent : opponents) {
              opponent_audio_frames.push_back(
                  {opponent.vehicle->state().pose.world_position,
                   opponent.vehicle->drive().state().engine_scalar});
            }
            opponent_engine_sound->update(opponent_audio_frames, camera,
                                          engine_active);
          }
        }
        if (race_sound.has_value()) {
          for (std::size_t index = 0U; index < countdown_sound_requested.size();
               ++index) {
            if (countdown_sound_requested[index]) {
              race_sound->trigger_countdown(index);
            }
          }
          if (go_sound_requested) {
            race_sound->trigger_go();
          }
          if (finish_sound_requested) {
            race_sound->trigger_finish();
          }
        }
        if (horn_sound.has_value()) {
          const auto horn_controls_active =
              !pause_visible && !result_visible &&
              !start_flyby.active(session.race().progress()) &&
              session.race().progress().phase == mh::game::RacePhase::racing &&
              !post_finish_schedule.active();
          horn_sound->update(presentation_horn_held, horn_controls_active);
        }
        if (environment_sound.has_value()) {
          environment_sound->update(camera, environment_scene_frame);
        }
      }
      const auto active_render_distance =
          render_distance *
          static_cast<double>(pause_menu.view_distance_percent) / 100.0;
      scene_depth.set_distance_fog(
          active_render_distance * render_environment.cue_start,
          active_render_distance, render_environment.cue_color,
          render_environment.cue_enabled && !tron_mode);
      const auto world_profile_start = renderer_profile_enabled
                                           ? RendererProfileClock::now()
                                           : RendererProfileClock::time_point{};
      std::optional<OverheadWorldCutaway> overhead_cutaway;
      if (!recorded_presentation &&
          (pc_cheats.active(mh::game::OriginalPcCheat::supercars_camera) ||
           pc_cheats.active(mh::game::OriginalPcCheat::ignition_camera))) {
        overhead_cutaway = OverheadWorldCutaway{
            pose.world_position, pose.body_basis[2U], 34.0, 10.0, 3.25};
      }
      render_world(world_visual, view, width, height, enhanced_profile,
                   renderer_trilinear_filtering, active_render_distance,
                   render_environment, scene_depth, world_render_scratch,
                   pose.world_position, overhead_cutaway);
      render_environment_scene(environment_scene_visual, environment_scene,
                               environment_collision_bodies,
                               environment_scene_frame, world_visual, view,
                               enhanced_profile, renderer_trilinear_filtering,
                               active_render_distance, render_environment,
                               point_lighting_enabled, scene_depth,
                               vehicle_render_scratch);
      const auto world_profile_end = renderer_profile_enabled
                                         ? RendererProfileClock::now()
                                         : RendererProfileClock::time_point{};
      const auto shadows_profile_start =
          renderer_profile_enabled ? RendererProfileClock::now()
                                   : RendererProfileClock::time_point{};
      if (shadows_enabled && !tron_mode) {
        for (const auto &sample : ghost_samples) {
          if (sample.stream_index >= recorded_car_visuals.size()) {
            throw std::runtime_error(
                "MDE playback stream has no bound recorded-car shadow");
          }
          render_vehicle_shadow(
              recorded_car_visuals[sample.stream_index].visual,
              sample.ghost.pose, shadow_collision_world, view,
              active_render_distance, render_environment, scene_depth);
        }
        if (!recorded_presentation) {
          for (const auto &opponent : opponents) {
            const auto &opponent_pose = opponent.presentation_pose;
            render_vehicle_shadow(
                opponent.visual, opponent_pose, shadow_collision_world, view,
                active_render_distance, render_environment, scene_depth);
          }
          render_vehicle_shadow(
              car_visual, player_visual_pose, shadow_collision_world, view,
              active_render_distance, render_environment, scene_depth);
        }
      }
      const auto shadows_profile_end = renderer_profile_enabled
                                           ? RendererProfileClock::now()
                                           : RendererProfileClock::time_point{};
      const auto recorded_camera_body_visible =
          !recorded_presentation || recorded_camera_view_active ||
          session.camera_mode() == mh::game::VehicleCameraMode::chase ||
          session.camera_mode() == mh::game::VehicleCameraMode::far_chase;
      for (const auto &sample : ghost_samples) {
        if (sample.stream_index >= recorded_car_visuals.size()) {
          throw std::runtime_error(
              "MDE playback stream has no bound recorded-car visual");
        }
        // Mode 6 installs the followed recording in body slot zero (or the
        // CyclePlayers-selected slot). The normal InCarView renderer suppresses
        // that body just as it suppresses the live player's body; otherwise the
        // correctly placed camera is enclosed by the recorded chassis. Keep
        // every other recorded racer visible.
        if (recorded_presentation &&
            sample.body_slot == recorded_camera_body_slot &&
            !recorded_camera_body_visible) {
          continue;
        }
        const auto &recorded = recorded_car_visuals[sample.stream_index];
        render_vehicle(recorded.visual, world_visual, sample.ghost.pose, view,
                       enhanced_profile, renderer_trilinear_filtering,
                       active_render_distance, render_environment,
                       point_lighting_enabled, scene_depth,
                       vehicle_render_scratch,
                       recorded_wheel_visuals[sample.stream_index], true,
                       car_shading);
      }
      if (!recorded_presentation) {
        for (const auto &opponent : opponents) {
          const auto &opponent_pose = opponent.presentation_pose;
          render_vehicle(opponent.visual, world_visual, opponent_pose, view,
                         enhanced_profile, renderer_trilinear_filtering,
                         active_render_distance, render_environment,
                         point_lighting_enabled, scene_depth,
                         vehicle_render_scratch, opponent.wheels, true,
                         car_shading);
        }
      }
      const auto player_body_visible =
          !recorded_presentation &&
          (pc_cheats.active(mh::game::OriginalPcCheat::supercars_camera) ||
           pc_cheats.active(mh::game::OriginalPcCheat::ignition_camera) ||
           start_flyby.active(session.race().progress()) ||
           session.camera_mode() == mh::game::VehicleCameraMode::chase ||
           session.camera_mode() == mh::game::VehicleCameraMode::far_chase);
      if (player_body_visible) {
        render_vehicle(car_visual, world_visual, player_visual_pose, view,
                       enhanced_profile, renderer_trilinear_filtering,
                       active_render_distance, render_environment,
                       point_lighting_enabled, scene_depth,
                       vehicle_render_scratch, player_wheels, true, car_shading);
      }
      // Lens flares are additive, non-depth-writing geometry. Submit them
      // only after the complete opaque world/vehicle field exists so a car
      // occludes an authored level light only when its actual depth wins.
      if (lens_flares_enabled && !tron_mode) {
        render_world_lamp_beams(world_visual, view, active_render_distance,
                                render_environment, scene_depth);
        render_world_halos(world_visual, view, active_render_distance,
                           render_environment, scene_depth);
      }
      // Retail particles are submitted after the complete opaque vehicle
      // field.  Doing this per car makes a later body overwrite an earlier
      // additive lamp even when the lamp is the nearer surface.
      if (halos_enabled && !tron_mode) {
        for (const auto &sample : ghost_samples) {
          const auto &recorded = recorded_car_visuals[sample.stream_index];
          render_vehicle_headlight_projection(
              recorded.visual, sample.ghost.pose, view, active_render_distance,
              render_environment, scene_depth);
          if (!recorded_presentation ||
              sample.body_slot != recorded_camera_body_slot ||
              recorded_camera_body_visible) {
            render_vehicle_halos(recorded.visual, sample.ghost.pose, view,
                                 active_render_distance, render_environment,
                                 scene_depth, lens_flares_enabled, false);
          }
        }
        if (!recorded_presentation) {
          for (const auto &opponent : opponents) {
            const auto &opponent_pose = opponent.presentation_pose;
            render_vehicle_headlight_projection(
                opponent.visual, opponent_pose, view, active_render_distance,
                render_environment, scene_depth);
            render_vehicle_halos(opponent.visual, opponent_pose, view,
                                 active_render_distance, render_environment,
                                 scene_depth, lens_flares_enabled,
                                 mh::game::vehicle_rear_lamps_bright(
                                     opponent.presentation_controls));
          }
          render_vehicle_headlight_projection(car_visual, player_visual_pose,
                                              view, active_render_distance,
                                              render_environment, scene_depth);
        }
        if (player_body_visible) {
          render_vehicle_halos(car_visual, player_visual_pose, view,
                               active_render_distance, render_environment,
                               scene_depth, lens_flares_enabled,
                               mh::game::vehicle_rear_lamps_bright(
                                   player_presentation_controls));
        }
      }
      const auto vehicles_profile_end =
          renderer_profile_enabled ? RendererProfileClock::now()
                                   : RendererProfileClock::time_point{};
      if (skid_marks_enabled && !tron_mode) {
        render_skid_marks(player_skid_marks, view, active_render_distance,
                          render_environment, scene_depth);
        for (const auto &opponent : opponents) {
          render_skid_marks(opponent.skid_marks, view, active_render_distance,
                            render_environment, scene_depth);
        }
      }
      if (smoke_enabled && !tron_mode) {
        render_tire_smoke(player_tire_smoke, view, active_render_distance,
                          render_environment, scene_depth);
        for (const auto &opponent : opponents) {
          render_tire_smoke(opponent.tire_smoke, view, active_render_distance,
                            render_environment, scene_depth);
        }
      }
      if (sparks_enabled && !tron_mode) {
        render_sparks(player_sparks, view, active_render_distance,
                      render_environment, scene_depth);
        for (const auto &opponent : opponents) {
          render_sparks(opponent.sparks, view, active_render_distance,
                        render_environment, scene_depth);
        }
      }
      const auto skid_marks_profile_end =
          renderer_profile_enabled ? RendererProfileClock::now()
                                   : RendererProfileClock::time_point{};
      scene_depth.present(renderer.get());
      if (!recorded_presentation && !pause_visible &&
          name_plate_mode != mh::ui::GraphicNamePlateMode::none) {
        name_plate_candidates.clear();
        for (std::size_t index = 0U; index < opponents.size(); ++index) {
          const auto &opponent = opponents[index];
          const auto &opponent_pose = opponent.presentation_pose;
          name_plate_candidates.push_back(
              {index, opponent_pose,
               to_view(view, opponent_pose.world_position).z});
        }
        stable_insertion_sort(name_plate_candidates,
                              [](const auto &left, const auto &right) {
                                return left.view_depth < right.view_depth;
                              });
        nearer_vehicle_bounds.clear();
        nearer_vehicles.clear();
        for (const auto &candidate : name_plate_candidates) {
          const auto &opponent = opponents[candidate.opponent_index];
          if (opponent.slot_index >= race_roster.size()) {
            continue;
          }
          const auto rendered_bounds = render_vehicle_name_plate(
              renderer.get(), race_hud,
              race_roster[opponent.slot_index].driver_nick,
              race_roster[opponent.slot_index].driver_color, opponent.visual,
              candidate.pose, shadow_collision_world, view, name_plate_mode,
              nearer_vehicles, nearer_vehicle_bounds, width, height);
          if (rendered_bounds.has_value()) {
            nearer_vehicle_bounds.push_back(*rendered_bounds);
          }
          nearer_vehicles.push_back({&opponent.visual, candidate.pose});
        }
      }
      const auto depth_upload_profile_end =
          renderer_profile_enabled ? RendererProfileClock::now()
                                   : RendererProfileClock::time_point{};
      if (collision_overlay) {
        render_track(renderer.get(), polygons, view, width, height);
      }
      const auto raw_speed_kmh =
          vehicle.state().velocity.local_linear[2U] * 3.6;
      const auto displayed_speed_kmh =
          std::abs(raw_speed_kmh) < 0.5 ? 0.0 : raw_speed_kmh;
      auto rendered_vehicle_samples = hud_vehicle_samples;
      if (!hud_samples_initialized) {
        rendered_vehicle_samples[0U] = player_route_cursor.current_sample();
        for (const auto &opponent : opponents) {
          rendered_vehicle_samples[opponent.slot_index] =
              opponent.controller->current_sample();
        }
      }
      const auto hud_preview =
          pause_visible && pause_menu.hud_preview_active(SDL_GetTicks());
      // p3.1's race renderer branches around the normal HUD owners at RVAs
      // 0x7977e, 0x79a35, 0x79c43, and 0x7aec6 while the pause flag is set.
      // Our Gameplay page temporarily restores those HUD owners as a frozen
      // five-second preview while keeping physics and pause audio suspended.
      if (!recorded_presentation && (!pause_visible || hud_preview)) {
        rendered_ghost_samples.clear();
        rendered_ghost_samples.reserve(ghost_samples.size());
        for (const auto &sample : ghost_samples) {
          const std::array<float, 2U> ghost_xz{
              static_cast<float>(sample.ghost.pose.world_position[0U]),
              static_cast<float>(sample.ghost.pose.world_position[2U])};
          rendered_ghost_samples.push_back(
              mh::game::original_ai_nearest_route_sample(route, ghost_xz));
        }
        if (tron_mode) {
          render_original_tron_message_backdrop(
              renderer.get(), session.race().progress(), width, height);
        }
        render_race_hud(
            renderer.get(), race_hud, route, race_roster,
            session.race().config(), session.race().progress(),
            displayed_speed_kmh, drive_state.engine_scalar,
            drive_state.current_gear_index, hud_race_position,
            std::span<const std::size_t>(rendered_vehicle_samples.data(),
                                         race_roster.size()),
            std::span<const std::size_t>(rendered_ghost_samples),
            std::span<const std::int64_t>(hud_progress_samples.data(),
                                          race_roster.size()),
            std::span<const float>(hud_vehicle_speeds.data(),
                                   race_roster.size()),
            (checkpoint_display_remaining >
                 mh::game::SimulationDuration::zero() ||
             (hud_preview &&
              checkpoint_info_mode != mh::ui::GraphicInfoMode::none)),
            info_detail_mode, info_map_mode, metric_units,
            pc_cheats.avenger_map, width, height);
        if (tron_mode) {
          render_original_tron_scroll(renderer.get(), race_hud,
                                      session.race().progress(), width, height);
        }
        if (active_location_easter_egg ==
                mh::game::OriginalPcLocationEasterEgg::black_lotus_club &&
            black_lotus_dwell_frames >= 202U) {
          render_original_black_lotus_message(renderer.get(), race_hud, width,
                                              height);
        } else if (active_location_easter_egg ==
                   mh::game::OriginalPcLocationEasterEgg::atlantika_scroll) {
          render_original_atlantika_scroll(
              renderer.get(), race_hud, atlantika_scroll_text,
              atlantika_scroll_frames, width, height);
          if (!pause_visible) {
            ++atlantika_scroll_frames;
          }
        }
        render_start_signal(renderer.get(), race_hud, width, height,
                            start_signal_phase(session.race().progress(),
                                               session.race().config(),
                                               go_display_remaining));
      }
      if (!pause_visible) {
        for (const auto &notice : ghost_notices.visible(SDL_GetTicks())) {
          render_original_mde_notice(renderer.get(), race_hud, notice, width,
                                     height);
        }
      }
      if (pause_visible) {
        render_race_pause_menu(renderer.get(), race_hud, pause_menu, width,
                               height);
      }

      if (!start_flyby.active(session.race().progress()) && collision_overlay) {
        const auto race_heading =
            "MOTORHEAD " + track_definition.name + " - MILESTONE 4";
        require(SDL_SetRenderDrawColor(renderer.get(), 80U, 220U, 255U, 255U),
                "set HUD color");
        require(SDL_RenderDebugText(renderer.get(), 20.0F, 18.0F,
                                    race_heading.c_str()),
                "render title");
        require(SDL_RenderDebugText(renderer.get(), 20.0F, 36.0F,
                                    "CLO BINDINGS DRIVE | F1 BUMPER | "
                                    "F2 IN-CAR | F3 OUTSIDE | F4 FAR | "
                                    "C CYCLE | ESC MENU"),
                "render controls");
        std::ostringstream status;
        status.setf(std::ios::fixed);
        status.precision(1);
        status << phase_text(session.race().progress()) << " | SPEED "
               << displayed_speed_kmh << " KM/H | GEAR "
               << drive_state.current_gear_index << " | RPM "
               << drive_state.engine_scalar << " | LAP "
               << session.race().progress().current_lap << '/' << race_laps
               << " | SECTOR " << player_race_progress.current_sector() << '/'
               << player_race_progress.sector_count() << " | TIME "
               << std::chrono::duration<double>(
                      session.race().progress().timing.total)
                      .count()
               << " | PROFILE " << (enhanced_profile ? "ENHANCED" : "CLASSIC")
               << " | CAMERA " << camera_mode_name(session.camera_mode())
               << " | LIGHTS " << (point_lighting_enabled ? "ON" : "OFF")
               << " | CAR " << car.name;
        if (ghost_playback.has_value()) {
          status << " | GHOST "
                 << (ghost_playback->active() ? "PLAYING " : "FINISHED ")
                 << ghost_playback->milliseconds() << "MS";
        }
        if (enhanced_profile && world_visual.override_materials != 0U) {
          status << " HD " << world_visual.override_materials;
        }
        if (music.has_value() || engine_sound.has_value() ||
            race_sound.has_value()) {
          status << " | AUDIO " << (audio_enabled ? "ON" : "MUTED") << ' '
                 << audio_device->specification.freq << "HZ";
          if (music.has_value()) {
            status << " MUSIC " << (music_enabled ? "ON" : "OFF");
          }
          if (engine_sound.has_value()) {
            status << " ENGINE " << (engine_enabled ? "ON" : "OFF");
          }
          if (race_sound.has_value()) {
            status << " RACE-SFX ON";
          }
        }
        require(SDL_RenderDebugText(renderer.get(), 20.0F, 58.0F,
                                    status.str().c_str()),
                "render status");
      }
      const auto capture_frame =
          screenshot_path.has_value() &&
          (maximum_frames == 0U || rendered_frames + 1U == maximum_frames);
      const auto hud_profile_end = renderer_profile_enabled
                                       ? RendererProfileClock::now()
                                       : RendererProfileClock::time_point{};
      present_presentation_back_buffer(
          renderer.get(), presentation_back_buffers,
          underwater_mode,
          std::chrono::duration<double>(session.race().progress().timing.total)
              .count(),
          borderless_destination.has_value() ? &*borderless_destination
                                             : nullptr,
          motion_blur_enabled && !pause_visible);
      if (capture_frame) {
        if (screenshot_path->has_parent_path()) {
          std::filesystem::create_directories(screenshot_path->parent_path());
        }
        // Read the fully composed front buffer. The active RGB565 presentation
        // target is not a portable screenshot source across render backends.
        SdlPointer<SDL_Surface, SDL_DestroySurface> readback(
            SDL_RenderReadPixels(renderer.get(), nullptr), SDL_DestroySurface);
        require(readback != nullptr, "read race frame");
        require(SDL_SaveBMP(readback.get(), screenshot_path->string().c_str()),
                "save race screenshot");
        screenshot_path.reset();
      }
      pace_stable_present();
      require(SDL_RenderPresent(renderer.get()), "present frame");
      if (benchmark_frame_active) {
        ++benchmark_presented_frames;
        benchmark_measurement_end = BenchmarkClock::now();
      }
      complete_stable_present();
      if (renderer_profile_enabled) {
        const auto presentation_profile_end = RendererProfileClock::now();
        ++renderer_profile.frames;
        renderer_profile.background_ms += elapsed_profile_ms(
            background_profile_start, background_profile_end);
        renderer_profile.depth_clear_ms +=
            elapsed_profile_ms(background_profile_end, depth_clear_profile_end);
        renderer_profile.shadows_ms +=
            elapsed_profile_ms(shadows_profile_start, shadows_profile_end);
        renderer_profile.world_ms +=
            elapsed_profile_ms(world_profile_start, world_profile_end);
        renderer_profile.vehicles_ms +=
            elapsed_profile_ms(shadows_profile_end, vehicles_profile_end);
        renderer_profile.skid_marks_ms +=
            elapsed_profile_ms(vehicles_profile_end, skid_marks_profile_end);
        renderer_profile.depth_upload_ms += elapsed_profile_ms(
            skid_marks_profile_end, depth_upload_profile_end);
        renderer_profile.raster_ms += scene_depth.last_render_ms();
        renderer_profile.texture_upload_ms += scene_depth.last_upload_ms();
        renderer_profile.hud_ms +=
            elapsed_profile_ms(depth_upload_profile_end, hud_profile_end);
        renderer_profile.presentation_ms +=
            elapsed_profile_ms(hud_profile_end, presentation_profile_end);
        const auto render_total =
            elapsed_profile_ms(render_profile_start, presentation_profile_end);
        renderer_profile.render_total_ms += render_total;
        renderer_profile.render_total_squared_ms += render_total * render_total;
        renderer_profile.render_total_minimum_ms =
            std::min(renderer_profile.render_total_minimum_ms, render_total);
        renderer_profile.render_total_maximum_ms =
            std::max(renderer_profile.render_total_maximum_ms, render_total);
        if (renderer_profile_previous_present.has_value()) {
          const auto interval = elapsed_profile_ms(
              *renderer_profile_previous_present, presentation_profile_end);
          ++renderer_profile.presentation_intervals;
          renderer_profile.presentation_interval_ms += interval;
          renderer_profile.presentation_interval_squared_ms +=
              interval * interval;
          renderer_profile.presentation_interval_minimum_ms = std::min(
              renderer_profile.presentation_interval_minimum_ms, interval);
          renderer_profile.presentation_interval_maximum_ms = std::max(
              renderer_profile.presentation_interval_maximum_ms, interval);
        }
        renderer_profile_previous_present = presentation_profile_end;
      }
      ++rendered_frames;
      if (maximum_frames != 0U && rendered_frames >= maximum_frames) {
        running = false;
      }
    }
    if (benchmark_completed && benchmark_report_path.has_value()) {
      if (!benchmark_measurement_start.has_value() ||
          !benchmark_measurement_end.has_value() ||
          benchmark_presented_frames == 0U) {
        throw std::runtime_error(
            "benchmark completed without a measured presentation interval");
      }
      if (!benchmark_report_path->parent_path().empty()) {
        std::filesystem::create_directories(
            benchmark_report_path->parent_path());
      }
      std::ofstream report(*benchmark_report_path,
                           std::ios::binary | std::ios::trunc);
      if (!report) {
        throw std::runtime_error("could not create benchmark report: " +
                                 benchmark_report_path->string());
      }

      const auto elapsed_seconds =
          std::chrono::duration<double>(*benchmark_measurement_end -
                                        *benchmark_measurement_start)
              .count();
      const auto frames_per_second =
          elapsed_seconds > 0.0
              ? static_cast<double>(benchmark_presented_frames) /
                    elapsed_seconds
              : 0.0;
      const auto config_path =
          configuration_root.has_value()
              ? mh::ui::motorhead_configuration_path(*configuration_root)
              : std::filesystem::path{};
      const auto config_value = [&](const std::string_view key,
                                    const std::string_view fallback) {
        if (!config_path.empty() &&
            std::filesystem::is_regular_file(config_path)) {
          const auto value = mh::ui::configuration_file_value(config_path, key);
          if (!value.empty()) {
            return value;
          }
        }
        return std::string(fallback);
      };
      const auto on_off = [](const bool enabled) {
        return enabled ? std::string_view("On") : std::string_view("Off");
      };
      const auto now = std::time(nullptr);
      std::tm local_time{};
#if defined(_WIN32)
      localtime_s(&local_time, &now);
#else
      localtime_r(&now, &local_time);
#endif
      const auto demo_name = ghost_demo_path.has_value()
                                 ? ghost_demo_path->stem().string()
                                 : std::string{};
      const auto sound_device =
          config_value("Sounder", audio_enabled ? "SDL Audio" : "Nosound");
      const auto renderer_name = renderer_backend_name(renderer_backend);

      // Keep the p3.1 text contract byte-for-byte in structure and naming.
      // The retail writer opens relative "Fps.txt" in the Motorhead directory.
      report
          << "**************************************************************\n"
          << "*** Motorhead Benchmark Results, " << std::setfill('0')
          << std::setw(4) << local_time.tm_year + 1900 << std::setw(2)
          << local_time.tm_mon + 1 << std::setw(2) << local_time.tm_mday << ", "
          << std::setw(2) << local_time.tm_hour << ':' << std::setw(2)
          << local_time.tm_min << "\n"
          << "*** Digital Illusions CE, www.dice.se\n"
          << "***\n"
          << "*** Demo file: " << demo_name << ".mde\n"
          << "***\n"
          << "**************************************************************"
             "\n\n"
          << std::setfill(' ') << std::fixed << std::setprecision(1)
          << "Frames/Sec:\t\t\t" << frames_per_second << "\n"
          << "Time:\t\t\t\t" << elapsed_seconds << "\n"
          << "Frames:\t\t\t\t" << benchmark_presented_frames << "\n\n"
          << "Render device:\t\t\t" << renderer_name << "\n"
          << "Sound device:\t\t\t" << sound_device << "\n\n"
          << "Screen width:\t\t\t" << window_width << "\n"
          << "Screen height:\t\t\t" << window_height << "\n"
          << "Screen BPP:\t\t\t" << (renderer_true_colour ? 32 : 16) << "\n\n"
          << "True colour rendering:\t\t" << on_off(renderer_true_colour)
          << "\n"
          << "Triple buffer:\t\t\t" << on_off(renderer_triple_buffer) << "\n"
          << "Trilinear filtering:\t\t" << on_off(renderer_trilinear_filtering)
          << "\n"
          << "Texture format:\t\t\t" << renderer_texture_format << "-bit\n"
          << "Info detail:\t\t\t" << config_value("InfoDetail", "All") << "\n"
          << "Info map:\t\t\t" << config_value("RoadMap", "All") << "\n"
          << "Graphic detail:\t\t\t" << config_value("DetailMode", "Custom")
          << "\n\n"
          << "Lensflares:\t\t\t" << config_value("LensFlares", "On") << "\n"
          << "Sparks:\t\t\t\t" << config_value("Sparks", "On") << "\n"
          << "Smoke:\t\t\t\t" << on_off(smoke_enabled) << "\n"
          << "Halos:\t\t\t\t" << on_off(halos_enabled) << "\n"
          << "Skidmarks:\t\t\t" << on_off(skid_marks_enabled) << "\n"
          << "Shadow:\t\t\t\t" << on_off(shadows_enabled) << "\n"
          << "Nameplates:\t\t\t" << config_value("NamePlates", "None") << "\n"
          << "Background:\t\t\t" << (background_enabled ? "Picture" : "Blank")
          << "\n"
          << "Track detail:\t\t\t" << config_value("TrackDetail", "High")
          << "\n"
          << "Car detail:\t\t\t" << config_value("CarDetail", "High") << "\n"
          << "Car shading:\t\t\t" << config_value("CarShading", "Reflection")
          << "\n"
          << "View distance:\t\t\t" << std::setprecision(1)
          << static_cast<double>(initial_view_distance_percent) << "%\n"
          << "Motionblur:\t\t\t" << config_value("MotionBlur", "Off") << "\n"
          << "Z buffer read:\t\t\t" << config_value("ZRead", "Off") << "\n\n"
          << "**************************************************************\n";
      if (!report) {
        throw std::runtime_error("could not write benchmark report: " +
                                 benchmark_report_path->string());
      }
    }
    if (renderer_profile_report_path.has_value()) {
      if (!renderer_profile_report_path->parent_path().empty()) {
        std::filesystem::create_directories(
            renderer_profile_report_path->parent_path());
      }
      std::ofstream report(*renderer_profile_report_path,
                           std::ios::binary | std::ios::trunc);
      if (!report) {
        throw std::runtime_error("could not create renderer profile report");
      }
      const auto per_frame = [&renderer_profile](const double total) {
        return renderer_profile.frames == 0U
                   ? 0.0
                   : total / static_cast<double>(renderer_profile.frames);
      };
      const auto standard_deviation = [](const double total,
                                         const double squared_total,
                                         const std::uint64_t count) {
        if (count == 0U) {
          return 0.0;
        }
        const auto mean = total / static_cast<double>(count);
        return std::sqrt(std::max(
            0.0, squared_total / static_cast<double>(count) - mean * mean));
      };
      const auto flat_colored_primitives =
          std::accumulate(world_visual.world.primitive_type_counts.begin(),
                          world_visual.world.primitive_type_counts.begin() + 4U,
                          std::uint64_t{0U});
      const auto vertex_colored_primitives =
          std::accumulate(world_visual.world.primitive_type_counts.begin() + 4U,
                          world_visual.world.primitive_type_counts.begin() + 8U,
                          std::uint64_t{0U});
      const auto linked_light_cells = static_cast<std::uint64_t>(std::count_if(
          world_visual.world.grid_cells.begin(),
          world_visual.world.grid_cells.end(),
          [](const auto &cell) { return !cell.light_indices.empty(); }));
      const auto linked_light_references = std::accumulate(
          world_visual.world.grid_cells.begin(),
          world_visual.world.grid_cells.end(), std::uint64_t{0U},
          [](const auto total, const auto &cell) {
            return total +
                   static_cast<std::uint64_t>(cell.light_indices.size());
          });
      report
          << std::setprecision(10)
          << "{\n  \"schema\": \"motorhead.renderer-profile.v1\",\n"
          << "  \"frames\": " << renderer_profile.frames << ",\n"
          << "  \"raster_workers\": " << scene_depth.active_software_workers()
          << ",\n  \"world_variant\": \""
          << renderer_backend_token(renderer_backend) << "\",\n"
          << "  \"world_flat_colored_primitives\": " << flat_colored_primitives
          << ",\n"
          << "  \"world_vertex_colored_primitives\": "
          << vertex_colored_primitives << ",\n"
          << "  \"world_point_lights\": " << world_visual.world.lights.size()
          << ",\n"
          << "  \"world_linked_light_cells\": " << linked_light_cells << ",\n"
          << "  \"world_linked_light_references\": " << linked_light_references
          << ",\n"
          << "  \"world_ambient_rgb\": ["
          << static_cast<unsigned>(world_visual.world.ambient_color[0U]) << ", "
          << static_cast<unsigned>(world_visual.world.ambient_color[1U]) << ", "
          << static_cast<unsigned>(world_visual.world.ambient_color[2U])
          << "],\n"
          << "  \"world_ambient_intensity\": "
          << world_visual.world.ambient_intensity << ",\n"
          << "  \"world_directional_rgb\": ["
          << static_cast<unsigned>(world_visual.world.directional_color[0U])
          << ", "
          << static_cast<unsigned>(world_visual.world.directional_color[1U])
          << ", "
          << static_cast<unsigned>(world_visual.world.directional_color[2U])
          << "],\n"
          << "  \"world_directional_intensity\": "
          << world_visual.world.directional_intensity << ",\n"
          << "  \"track_object_ambient\": ["
          << render_environment.object_ambient[0U] << ", "
          << render_environment.object_ambient[1U] << ", "
          << render_environment.object_ambient[2U] << "],\n"
          << "  \"track_object_brightness\": ["
          << render_environment.object_brightness[0U] << ", "
          << render_environment.object_brightness[1U] << ", "
          << render_environment.object_brightness[2U] << "],\n"
          << "  \"track_accelerated_brightness\": ["
          << render_environment.accelerated_brightness[0U] << ", "
          << render_environment.accelerated_brightness[1U] << ", "
          << render_environment.accelerated_brightness[2U] << "],\n"
          << "  \"track_light_intensity\": "
          << render_environment.light_intensity << ",\n"
          << "  \"track_specular_factor\": "
          << render_environment.specular_factor << ",\n"
          << "  \"environment_scene_instances\": "
          << environment_scene_visual.instances.size() << ",\n"
          << "  \"environment_scene_assets\": "
          << environment_scene_visual.assets.size() << ",\n"
          << "  \"last_scene_rgba_fnv1a64\": \"0x" << std::hex
          << scene_depth.color_checksum() << std::dec << "\",\n"
          << "  \"milliseconds_per_frame\": {\n"
          << "    \"background\": " << per_frame(renderer_profile.background_ms)
          << ",\n"
          << "    \"depth_clear\": "
          << per_frame(renderer_profile.depth_clear_ms) << ",\n"
          << "    \"shadows\": " << per_frame(renderer_profile.shadows_ms)
          << ",\n"
          << "    \"world\": " << per_frame(renderer_profile.world_ms) << ",\n"
          << "    \"vehicles\": " << per_frame(renderer_profile.vehicles_ms)
          << ",\n"
          << "    \"skid_marks\": " << per_frame(renderer_profile.skid_marks_ms)
          << ",\n"
          << "    \"depth_upload\": "
          << per_frame(renderer_profile.depth_upload_ms) << ",\n"
          << "    \"raster\": " << per_frame(renderer_profile.raster_ms)
          << ",\n"
          << "    \"texture_upload\": "
          << per_frame(renderer_profile.texture_upload_ms) << ",\n"
          << "    \"hud\": " << per_frame(renderer_profile.hud_ms) << ",\n"
          << "    \"presentation\": "
          << per_frame(renderer_profile.presentation_ms) << ",\n"
          << "    \"render_total\": "
          << per_frame(renderer_profile.render_total_ms) << "\n"
          << "  },\n"
          << "  \"frame_pacing_ms\": {\n"
          << "    \"render_minimum\": "
          << (renderer_profile.frames == 0U
                  ? 0.0
                  : renderer_profile.render_total_minimum_ms)
          << ",\n    \"render_maximum\": "
          << renderer_profile.render_total_maximum_ms
          << ",\n    \"render_standard_deviation\": "
          << standard_deviation(renderer_profile.render_total_ms,
                                renderer_profile.render_total_squared_ms,
                                renderer_profile.frames)
          << ",\n    \"present_interval_mean\": "
          << (renderer_profile.presentation_intervals == 0U
                  ? 0.0
                  : renderer_profile.presentation_interval_ms /
                        static_cast<double>(
                            renderer_profile.presentation_intervals))
          << ",\n    \"present_interval_minimum\": "
          << (renderer_profile.presentation_intervals == 0U
                  ? 0.0
                  : renderer_profile.presentation_interval_minimum_ms)
          << ",\n    \"present_interval_maximum\": "
          << renderer_profile.presentation_interval_maximum_ms
          << ",\n    \"present_interval_standard_deviation\": "
          << standard_deviation(
                 renderer_profile.presentation_interval_ms,
                 renderer_profile.presentation_interval_squared_ms,
                 renderer_profile.presentation_intervals)
          << "\n  }\n}\n";
      if (!report) {
        throw std::runtime_error("could not write renderer profile report");
      }
    }
    if (ai_control_report_path.has_value()) {
      if (ai_opening_records.size() != ai_opening_report_cycles * 7U) {
        throw std::runtime_error("AI opening report did not capture eight "
                                 "complete seven-car updates");
      }
      if (ai_control_report_path->has_parent_path()) {
        std::filesystem::create_directories(
            ai_control_report_path->parent_path());
      }
      std::ofstream report(*ai_control_report_path,
                           std::ios::binary | std::ios::trunc);
      if (!report) {
        throw std::runtime_error("could not create AI opening report");
      }
      report << std::setprecision(std::numeric_limits<float>::max_digits10);
      const auto write_stochastic =
          [&report](const mh::game::OriginalAiStochasticControlState &state) {
            report << '[' << state.channel_a << ',' << state.channel_b << ','
                   << state.channel_c << ']';
          };
      const auto write_controller_state =
          [&report](const mh::game::OriginalAiControllerState &state) {
            report << "{\"positive_request_accumulator\":"
                   << state.positive_request_accumulator
                   << ",\"target_lateral_offset\":"
                   << state.target_lateral_offset
                   << ",\"previous_lateral_offset\":"
                   << state.lateral_rate.previous_lateral_offset
                   << ",\"lateral_error_derivative\":"
                   << state.lateral_rate.lateral_rate
                   << ",\"stuck_timer_seconds\":"
                   << state.stuck_recovery.timer_seconds << ",\"stuck_active\":"
                   << (state.stuck_recovery.active ? "true" : "false")
                   << ",\"route_width_recovery_latch\":"
                   << (state.route_width_recovery_latch ? "true" : "false")
                   << ",\"previous_brake\":" << state.previous_brake << '}';
          };
      report
          << "{\"schema\":\"motorhead.reconstructed-ai-controls-opening.v1\","
          << "\"random_seed\":\"0x" << std::hex << std::setw(8)
          << std::setfill('0') << *ai_random_seed << std::dec
          << "\",\"time_step_source\":\""
          << (ai_physics_capture_oracle ? "captured-p3.1-ai-physics-opening"
                                        : "captured-p3.1-opening")
          << "\","
          << "\"update_cycles\":" << ai_opening_report_cycles
          << ",\"initial_physics\":[";
      for (std::size_t index = 0U; index < ai_opening_contacts.size();
           ++index) {
        if (index != 0U) {
          report << ',';
        }
        const auto &contact = ai_opening_contacts[index];
        report << "{\"slot\":" << contact.slot << ",\"physics_position\":["
               << contact.physics_pose.world_position[0U] << ','
               << contact.physics_pose.world_position[1U] << ','
               << contact.physics_pose.world_position[2U] << "],\"wheels\":[";
        for (std::size_t wheel = 0U; wheel < contact.wheels.size(); ++wheel) {
          if (wheel != 0U) {
            report << ',';
          }
          const auto &sample = contact.wheels[wheel];
          report << "{\"hit\":" << (sample.hit.has_value() ? "true" : "false")
                 << ",\"hit_fraction\":";
          if (sample.hit_fraction.has_value()) {
            report << *sample.hit_fraction;
          } else {
            report << "null";
          }
          report << ",\"state_fraction\":" << sample.scalar_state.state_fraction
                 << ",\"material\":";
          if (sample.hit.has_value()) {
            report << sample.hit->material;
          } else {
            report << "null";
          }
          report << ",\"surface_index\":";
          if (sample.hit.has_value()) {
            report << sample.hit->surface_index;
          } else {
            report << "null";
          }
          report << ",\"normal\":";
          if (sample.hit.has_value()) {
            report << '[' << sample.hit->normal[0U] << ','
                   << sample.hit->normal[1U] << ',' << sample.hit->normal[2U]
                   << ']';
          } else {
            report << "null";
          }
          report << '}';
        }
        report << "]}";
      }
      report << "],\"outputs\":[";
      for (std::size_t index = 0U; index < ai_opening_records.size(); ++index) {
        const auto &record = ai_opening_records[index];
        if (index != 0U) {
          report << ',';
        }
        report << "{\"cycle\":" << record.cycle << ",\"order\":" << record.order
               << ",\"slot\":" << record.slot
               << ",\"route_sample\":" << record.observation.route_sample
               << ",\"longitudinal_projection\":"
               << record.observation.route_longitudinal_projection
               << ",\"measured_lateral_offset\":"
               << record.observation.route_frame.measured_lateral_offset
               << ",\"direction_alignment\":"
               << record.observation.route_frame.direction_alignment
               << ",\"route_side_cross\":"
               << record.observation.route_frame.route_side_cross
               << ",\"current_speed\":" << record.current_speed
               << ",\"time_step_seconds\":" << record.time_step_seconds
               << ",\"vehicle_x\":" << record.vehicle_xz[0U]
               << ",\"vehicle_z\":" << record.vehicle_xz[1U]
               << ",\"vehicle_forward_x\":" << record.vehicle_forward_xz[0U]
               << ",\"vehicle_forward_z\":" << record.vehicle_forward_xz[1U]
               << ",\"body_basis\":[";
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
          for (std::size_t component = 0U; component < 3U; ++component) {
            if (axis != 0U || component != 0U) {
              report << ',';
            }
            report << static_cast<float>(
                record.body_pose.body_basis[axis][component]);
          }
        }
        report
            << "],\"body_position\":["
            << static_cast<float>(record.body_pose.world_position[0U]) << ','
            << static_cast<float>(record.body_pose.world_position[1U]) << ','
            << static_cast<float>(record.body_pose.world_position[2U])
            << "],\"local_linear_velocity\":["
            << static_cast<float>(record.body_velocity.local_linear[0U]) << ','
            << static_cast<float>(record.body_velocity.local_linear[1U]) << ','
            << static_cast<float>(record.body_velocity.local_linear[2U])
            << "],\"local_angular_velocity\":["
            << static_cast<float>(record.body_velocity.local_angular[0U]) << ','
            << static_cast<float>(record.body_velocity.local_angular[1U]) << ','
            << static_cast<float>(record.body_velocity.local_angular[2U])
            << "],\"drive_state\":{\"current_gear_index\":"
            << record.drive_state.current_gear_index
            << ",\"engine_scalar\":" << record.drive_state.engine_scalar
            << ",\"steering_scalar\":" << record.drive_state.steering_scalar
            << ",\"engine_transition_active\":"
            << (record.drive_state.engine_transition_active ? "true" : "false")
            << "},\"retained_wheel_states\":[";
        for (std::size_t wheel = 0U;
             wheel < record.retained_wheel_states.size(); ++wheel) {
          if (wheel != 0U) {
            report << ',';
          }
          report << record.retained_wheel_states[wheel];
        }
        report << "],\"retained_wheel_contact_flags\":[";
        for (std::size_t wheel = 0U;
             wheel < record.retained_wheel_contact_flags.size(); ++wheel) {
          if (wheel != 0U) {
            report << ',';
          }
          report << record.retained_wheel_contact_flags[wheel];
        }
        report << "],\"preceding_response_substeps\":[";
        for (std::size_t substep = 0U;
             substep < record.preceding_response_velocities.size(); ++substep) {
          if (substep != 0U) {
            report << ',';
          }
          const auto &velocity = record.preceding_response_velocities[substep];
          const auto &force = record.preceding_accumulated_forces[substep];
          const auto &response = record.preceding_response_frames[substep];
          report << "{\"local_linear\":["
                 << static_cast<float>(velocity.local_linear[0U]) << ','
                 << static_cast<float>(velocity.local_linear[1U]) << ','
                 << static_cast<float>(velocity.local_linear[2U])
                 << "],\"local_angular\":["
                 << static_cast<float>(velocity.local_angular[0U]) << ','
                 << static_cast<float>(velocity.local_angular[1U]) << ','
                 << static_cast<float>(velocity.local_angular[2U])
                 << "],\"linear_force\":["
                 << static_cast<float>(force.linear[0U]) << ','
                 << static_cast<float>(force.linear[1U]) << ','
                 << static_cast<float>(force.linear[2U])
                 << "],\"angular_force\":["
                 << static_cast<float>(force.angular[0U]) << ','
                 << static_cast<float>(force.angular[1U]) << ','
                 << static_cast<float>(force.angular[2U])
                 << "],\"wheel_forces\":[";
          for (std::size_t wheel = 0U; wheel < response.wheel_forces.size();
               ++wheel) {
            if (wheel != 0U) {
              report << ',';
            }
            const auto &wheel_force = response.wheel_forces[wheel].force;
            const auto &wheel_contact = response.contacts.wheels[wheel];
            const auto &wheel_scalars =
                response.contacts.response_scalars[wheel];
            report << "{\"force\":[" << static_cast<float>(wheel_force[0U])
                   << ',' << static_cast<float>(wheel_force[1U]) << ','
                   << static_cast<float>(wheel_force[2U])
                   << "],\"state_fraction\":"
                   << static_cast<float>(
                          wheel_contact.scalar_state.state_fraction)
                   << ",\"spring\":" << static_cast<float>(wheel_scalars.spring)
                   << ",\"state_delta\":"
                   << static_cast<float>(wheel_scalars.state_delta) << '}';
          }
          report << "]}";
        }
        report << "],\"preceding_pose_substeps\":[";
        for (std::size_t substep = 0U;
             substep < record.preceding_post_pose_states.size(); ++substep) {
          if (substep != 0U) {
            report << ',';
          }
          const auto &pose = record.preceding_post_pose_states[substep];
          const auto &velocity = record.preceding_post_pose_velocities[substep];
          report << "{\"body_basis\":[";
          for (std::size_t axis = 0U; axis < 3U; ++axis) {
            for (std::size_t component = 0U; component < 3U; ++component) {
              if (axis != 0U || component != 0U) {
                report << ',';
              }
              report << static_cast<float>(pose.body_basis[axis][component]);
            }
          }
          report << "],\"local_linear\":["
                 << static_cast<float>(velocity.local_linear[0U]) << ','
                 << static_cast<float>(velocity.local_linear[1U]) << ','
                 << static_cast<float>(velocity.local_linear[2U])
                 << "],\"local_angular\":["
                 << static_cast<float>(velocity.local_angular[0U]) << ','
                 << static_cast<float>(velocity.local_angular[1U]) << ','
                 << static_cast<float>(velocity.local_angular[2U]) << "]}";
        }
        report << "],\"preceding_drivetrain\":";
        if (record.preceding_drivetrain.has_value()) {
          const auto &drive = *record.preceding_drivetrain;
          report << "{\"candidate_gear_index\":" << drive.candidate_gear_index
                 << ",\"current_gear_index\":" << drive.state.current_gear_index
                 << ",\"engine_scalar\":" << drive.state.engine_scalar
                 << ",\"engine_transition_active\":"
                 << (drive.state.engine_transition_active ? "true" : "false")
                 << ",\"transition_candidate\":"
                 << drive.state.transition_candidate
                 << ",\"baseline\":" << drive.baseline_contribution
                 << ",\"acceleration\":" << drive.acceleration_contribution
                 << ",\"brake\":" << drive.brake_contribution
                 << ",\"low_speed\":" << drive.low_speed_contribution
                 << ",\"output\":" << drive.drivetrain_output
                 << ",\"primary_scalar\":"
                 << drive.wheel_dynamic_input.primary_scalar
                 << ",\"secondary_scalar\":"
                 << drive.wheel_dynamic_input.secondary_scalar << '}';
        } else {
          report << "null";
        }
        report << ",\"preceding_grounded_damping\":";
        if (record.preceding_grounded_damping.has_value()) {
          const auto &damping = *record.preceding_grounded_damping;
          report << "{\"linear_factors\":[" << damping.linear_factors[0U] << ','
                 << damping.linear_factors[1U] << ','
                 << damping.linear_factors[2U] << "],\"angular_factors\":["
                 << damping.angular_factors[0U] << ','
                 << damping.angular_factors[1U] << ','
                 << damping.angular_factors[2U] << "],\"input_local_linear\":["
                 << static_cast<float>(damping.input_velocity.local_linear[0U])
                 << ','
                 << static_cast<float>(damping.input_velocity.local_linear[1U])
                 << ','
                 << static_cast<float>(damping.input_velocity.local_linear[2U])
                 << "],\"input_local_angular\":["
                 << static_cast<float>(damping.input_velocity.local_angular[0U])
                 << ','
                 << static_cast<float>(damping.input_velocity.local_angular[1U])
                 << ','
                 << static_cast<float>(damping.input_velocity.local_angular[2U])
                 << "]}";
        } else {
          report << "null";
        }
        report << ",\"preceding_body_hull_reaction_count\":"
               << record.preceding_body_hull_reaction_count;
        report << ",\"preceding_dynamic_contacts\":[";
        for (std::size_t contact_index = 0U;
             contact_index < record.preceding_dynamic_contacts.size();
             ++contact_index) {
          if (contact_index != 0U) {
            report << ',';
          }
          const auto &contact =
              record.preceding_dynamic_contacts[contact_index];
          const auto &response = contact.response;
          const auto &position_delta =
              contact.self_is_first ? response.first_world_position_delta
                                    : response.second_world_position_delta;
          const auto &linear_delta =
              contact.self_is_first
                  ? response.first_local_linear_velocity_delta
                  : response.second_local_linear_velocity_delta;
          const auto &angular_delta =
              contact.self_is_first
                  ? response.first_local_angular_velocity_delta
                  : response.second_local_angular_velocity_delta;
          report << "{\"pass\":" << contact.pass
                 << ",\"other_slot\":" << contact.other_slot
                 << ",\"self_is_first\":"
                 << (contact.self_is_first ? "true" : "false")
                 << ",\"world_normal\":[" << response.world_normal[0U] << ','
                 << response.world_normal[1U] << ','
                 << response.world_normal[2U] << "],\"world_contact_point\":["
                 << response.world_contact_point[0U] << ','
                 << response.world_contact_point[1U] << ','
                 << response.world_contact_point[2U]
                 << "],\"world_position_delta\":[" << position_delta[0U] << ','
                 << position_delta[1U] << ',' << position_delta[2U]
                 << "],\"local_linear_velocity_delta\":[" << linear_delta[0U]
                 << ',' << linear_delta[1U] << ',' << linear_delta[2U]
                 << "],\"local_angular_velocity_delta\":[" << angular_delta[0U]
                 << ',' << angular_delta[1U] << ',' << angular_delta[2U]
                 << "],\"normal_relative_velocity\":"
                 << response.normal_relative_velocity
                 << ",\"effective_inverse_mass\":"
                 << response.effective_inverse_mass
                 << ",\"penetration\":" << response.penetration
                 << ",\"impulse\":" << response.impulse << '}';
        }
        report << ']';
        report << ",\"state_before\":";
        write_controller_state(record.state_before);
        report << ",\"stochastic_before\":";
        write_stochastic(record.stochastic_before);
        report << ",\"controls\":{\"throttle\":"
               << record.step.controls.throttle
               << ",\"brake\":" << record.step.controls.brake
               << ",\"steering\":" << record.step.controls.steering
               << "},\"signed_longitudinal_request\":"
               << record.step.signed_longitudinal_request
               << ",\"current_safe_speed\":" << record.step.current_safe_speed
               << ",\"lookahead_safe_speed\":"
               << record.step.lookahead_safe_speed
               << ",\"collision_avoidance_active\":"
               << (record.step.collision_avoidance_active ? "true" : "false")
               << ",\"forced_brake_for_avoidance\":"
               << (record.step.forced_brake_for_avoidance ? "true" : "false")
               << ",\"state_after\":";
        write_controller_state(record.state_after);
        report << ",\"stochastic_after\":";
        write_stochastic(record.stochastic_after);
        report << '}';
      }
      report << "],\"passed\":true}\n";
      if (!report) {
        throw std::runtime_error("could not write AI opening report");
      }
    }
    mh::common::log_runtime_info("Race runtime stopped with code " +
                                 std::to_string(exit_code));
    return exit_code;
  } catch (const std::exception &error) {
    std::cerr << "Motorhead race failed: " << error.what() << '\n';
    mh::common::log_runtime_error(std::string("Race runtime failed: ") +
                                  error.what());
    if (!suppress_error_dialog) {
      auto message = std::string(error.what());
      if (const auto log_path = mh::common::runtime_log_path();
          !log_path.empty()) {
        message += "\n\nDetails were written to:\n" + log_path.string();
      }
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Motorhead race failed",
                               message.c_str(), nullptr);
    }
    return 1;
  } catch (...) {
    constexpr auto message = "Motorhead race failed with an unknown error.";
    std::cerr << message << '\n';
    mh::common::log_runtime_error(message);
    if (!suppress_error_dialog) {
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Motorhead race failed",
                               message, nullptr);
    }
    return 1;
  }
}
