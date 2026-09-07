#pragma once

#include <content/formats/texture_assets.hpp>
#include <game/physics/collision.hpp>
#include <game/vehicle/runtime.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

struct SceneRasterCommand {
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
};

} // namespace mh::render
