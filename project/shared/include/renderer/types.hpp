#pragma once

#include <content/formats/texture_assets.hpp>
#include <game/physics/collision.hpp>
#include <game/vehicle/runtime.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <memory>
#include <vector>

namespace mh::render {

struct PerspectiveView {
  mh::game::CollisionVector3 position{};
  mh::game::CollisionVector3 right{};
  mh::game::CollisionVector3 up{};
  mh::game::CollisionVector3 forward{};
  float center_x = 0.0F;
  float center_y = 0.0F;
  float focal_length = 1.0F;
  double near_plane = 0.2;
};

struct VehicleHeadlightSource {
  mh::game::CollisionVector3 center{};
  mh::game::CollisionVector3 direction{};
};

struct HeadlightDepthProjection {
  mh::game::OriginalBodyPoseState vehicle{};
  std::array<VehicleHeadlightSource, 2U> sources{};
  PerspectiveView view{};
  int minimum_x = 0;
  int maximum_x = -1;
  int minimum_y = 0;
  int maximum_y = -1;
  double render_distance = 0.0;
  double cue_start = 0.0;
  bool cue_enabled = false;
};

struct VehicleShadowReceiverCell {
  std::array<std::array<double, 2U>, 4U> world_xz{};
  std::array<double, 4U> world_y{};
  double minimum_receiver_y = 0.0;
  double maximum_receiver_y = 0.0;
  bool valid = false;
};

struct VehicleShadowDepthProjection {
  std::array<VehicleShadowReceiverCell, 9U> cells{};
  PerspectiveView view{};
  int minimum_x = 0;
  int maximum_x = -1;
  int minimum_y = 0;
  int maximum_y = -1;
  double minimum_world_x = 0.0;
  double maximum_world_x = 0.0;
  double minimum_world_z = 0.0;
  double maximum_world_z = 0.0;
  double render_distance = 0.0;
  double cue_start = 0.0;
  std::array<float, 3U> cue_color{};
  bool cue_enabled = false;
};

enum class SceneBlendMode : std::uint8_t { opaque, alpha, additive };

struct SceneCarSurface {
  std::array<std::array<float, 3U>, 4U> view_positions{};
  std::array<float, 3U> albedo{};
  std::array<float, 3U> world_up{};
  std::array<float, 3U> light_direction{};
  std::array<float, 3U> light_color{};
  // 0 = legacy, 1 = paint/environment map, 2 = textured body, 3 = tinted glass.
  // 4 = textured world, 5 = lamp metal, 6 = untextured world, 7 = lamp emitter.
  // 8 = dry asphalt (inferred from original material names).
  // 9 = textured wheel, with texture-derived rim relief.
  // 10 = vegetation, 11 = porous mineral/masonry, 12 = architectural glass.
  float kind = 0.0F;
};

struct SceneEnvironmentData {
  std::array<std::array<float, 4U>, 8U * 1536U> probe_colors{};
  // Radial cube maps for 32 track lights and 32 headlights, 16x16 per face.
  std::array<float, 64U * 1536U> shadow_depths{};
};

// Float4 records before environment mirror the D3D12 lighting constant buffer.
struct SceneLighting {
  std::array<float, 4U> control{};
  std::array<float, 4U> ambient{};
  std::array<float, 4U> direction{};
  std::array<float, 4U> sun{};
  std::array<float, 4U> up{};
  std::array<std::array<float, 4U>, 32U> positions{};
  std::array<std::array<float, 4U>, 32U> colors{};
  std::array<std::array<float, 4U>, 32U> headlight_positions{};
  std::array<std::array<float, 4U>, 32U> headlight_directions{};
  std::array<std::array<float, 4U>, 32U> headlight_colors{};
  std::array<float, 4U> probe_right{}, probe_up{}, probe_forward{};
  std::array<std::array<float, 4U>, 8U> probe_origins{};
  std::array<float, 4U> camera_position{};
  std::array<float, 4U> history_right{}, history_up{}, history_forward{}, history_origin{};
  std::array<float, 4U> history_projection{};
  std::array<std::array<float, 4U>, 8U> history_car_origins{};
  std::array<float, 4U> temporal_jitter{};
  std::array<std::array<float, 4U>, 8U> car_right{}, car_up{}, car_forward{}, car_shadow_centers{};
  std::array<std::array<float, 4U>, 8U> history_car_right{}, history_car_up{}, history_car_forward{};
  std::shared_ptr<SceneEnvironmentData> environment;
};

struct SceneRasterCommand {
  // View-space normals for authored vehicle environment maps; zero disables
  // per-pixel mapping. Other backends retain the original vertex UVs.
  std::array<std::array<float, 3U>, 4U> environment_normals{};
  SceneCarSurface car_surface{};
  std::array<SDL_Vertex, 4U> vertices{};
  std::array<double, 4U> view_depths{};
  std::array<float, 4U> fog_cues{};
  std::uint8_t vertex_count = 0U;
  const std::vector<mh::content::PamRgbaImage> *texture_levels = nullptr;
  bool trilinear_filtering = false;
  SceneBlendMode blend_mode = SceneBlendMode::opaque;
  bool coplanar_depth_bias = false;
  bool depth_test = true;
  int minimum_y = 0;
  int maximum_y = -1;
  std::optional<std::size_t> shadow_projection_index;
};

struct SceneFrameView {
  int width = 0;
  int height = 0;
  double near_plane = 0.5;
  const std::vector<SceneRasterCommand> *commands = nullptr;
  const std::vector<VehicleShadowDepthProjection> *shadow_projections = nullptr;
  const std::vector<HeadlightDepthProjection> *headlight_projections = nullptr;
  bool tron_hidden_line = false;
  bool trilinear_filtering = false;
  double fog_start = 0.0;
  double fog_end = 0.0;
  std::array<float, 3U> fog_color{};
  bool fog_enabled = false;
  float focal_length = 0.0F;
  const SceneLighting *lighting = nullptr;
};

} // namespace mh::render
