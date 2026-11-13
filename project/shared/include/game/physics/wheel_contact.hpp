#pragma once

#include <game/physics/body_pose.hpp>

#include <array>
#include <optional>

namespace mh::game {

enum class WheelCorner { front_left, front_right, back_right, back_left };

struct VehicleTransform {
  CollisionVector3 position{};
  double heading_radians = 0.0;
};

struct WheelProbeRig {
  // FL, FR, BR, BL: the order used by the authored CAR wheel block.
  std::array<CollisionVector3, 4U> local_origins{};
  double query_distance = 0.0;
  double authored_spring_length = 0.0;
  double authored_spring_strength = 0.0;
};

struct WheelProbeSample {
  WheelCorner corner = WheelCorner::front_left;
  CollisionVector3 world_origin{};
  std::optional<CollisionHit> hit;
};

struct WheelContactScalarState {
  // Exact recovered scalar mapping. Physical naming remains deliberately open.
  double state_fraction = 1.0;
  double complement_fraction = 0.0;
};

struct WheelContactScalarTransition {
  double previous_state_fraction = 1.0;
  double current_state_fraction = 1.0;
  double previous_minus_current = 0.0;
};

enum class OriginalWheelResponseProfile { standard, demon_grem, g_ride_west };

struct OriginalWheelResponseParameters {
  double spring_length = 0.0;
  double spring_strength = 0.0;
  double previous_minus_current_scale = 0.0;
};

struct OriginalWheelResponseScalars {
  double spring = 0.0;
  double state_delta = 0.0;
};

class WheelContactScalarHistory {
public:
  [[nodiscard]] std::array<WheelContactScalarTransition, 4U>
  update(const std::array<WheelContactScalarState, 4U> &current);
  void seed(const std::array<WheelContactScalarState, 4U> &current);
  void reset() noexcept;

private:
  std::array<double, 4U> previous_{1.0, 1.0, 1.0, 1.0};
};

struct WheelSpringSegmentSample {
  WheelCorner corner = WheelCorner::front_left;
  CollisionVector3 world_center{};
  CollisionVector3 segment_start{};
  CollisionVector3 segment_end{};
  std::optional<CollisionHit> hit;
  std::optional<double> hit_fraction;
  WheelContactScalarState scalar_state{};
};

struct WheelContactFrame {
  std::array<WheelSpringSegmentSample, 4U> wheels{};
  std::array<WheelContactScalarTransition, 4U> transitions{};
  std::array<OriginalWheelResponseScalars, 4U> response_scalars{};
};

class WheelContactSystem {
public:
  explicit WheelContactSystem(WheelProbeRig rig,
                              OriginalWheelResponseProfile profile =
                                  OriginalWheelResponseProfile::standard);

  [[nodiscard]] WheelContactFrame sample(const VehicleTransform &transform,
                                         const CollisionWorld &world);
  [[nodiscard]] WheelContactFrame sample(const OriginalBodyPoseState &pose,
                                         const CollisionWorld &world);
  [[nodiscard]] WheelContactFrame
  sample(const OriginalBodyPoseState &pose, const CollisionWorld &world,
         bool reuse_retained_surfaces);
  [[nodiscard]] const WheelProbeRig &rig() const noexcept;
  [[nodiscard]] const OriginalWheelResponseParameters &
  response() const noexcept;
  void seed_previous_state(
      const std::array<WheelContactScalarState, 4U> &current);
  void begin_response_group() noexcept;
  void reset() noexcept;

private:
  WheelProbeRig rig_;
  OriginalWheelResponseParameters response_;
  WheelContactScalarHistory history_;
  std::array<std::optional<CollisionHit>, 4U> retained_group_hits_{};
};

[[nodiscard]] std::array<WheelProbeSample, 4U>
sample_wheel_probes(const WheelProbeRig &rig, const VehicleTransform &transform,
                    const CollisionWorld &world);

[[nodiscard]] std::array<WheelProbeSample, 4U>
sample_wheel_probes(const WheelProbeRig &rig, const OriginalBodyPoseState &pose,
                    const CollisionWorld &world);

// Reproduces the recovered original segment layout without applying suspension
// response: local axis (0, -spring_length, 0), endpoint A at center - axis,
// endpoint B at center + axis, and a normalized hit fraction along A -> B.
[[nodiscard]] std::array<WheelSpringSegmentSample, 4U>
sample_wheel_spring_segments(const WheelProbeRig &rig,
                             const VehicleTransform &transform,
                             const CollisionWorld &world);

[[nodiscard]] std::array<WheelSpringSegmentSample, 4U>
sample_wheel_spring_segments(const WheelProbeRig &rig,
                             const OriginalBodyPoseState &pose,
                             const CollisionWorld &world);

[[nodiscard]] std::array<WheelSpringSegmentSample, 4U>
sample_wheel_spring_segments(const WheelProbeRig &rig,
                             const OriginalWheelResponseParameters &response,
                             const VehicleTransform &transform,
                             const CollisionWorld &world);

[[nodiscard]] std::array<WheelSpringSegmentSample, 4U>
sample_wheel_spring_segments(const WheelProbeRig &rig,
                             const OriginalWheelResponseParameters &response,
                             const OriginalBodyPoseState &pose,
                             const CollisionWorld &world);

[[nodiscard]] OriginalBodyPoseState
make_original_body_pose(const VehicleTransform &transform);

[[nodiscard]] WheelContactScalarState
map_original_wheel_contact_fraction(std::optional<double> hit_fraction);

[[nodiscard]] OriginalWheelResponseParameters
make_original_wheel_response(double authored_spring_length,
                             double authored_spring_strength,
                             OriginalWheelResponseProfile profile);

[[nodiscard]] OriginalWheelResponseScalars
calculate_original_wheel_response_scalars(
    const OriginalWheelResponseParameters &response,
    const WheelContactScalarState &state,
    const WheelContactScalarTransition &transition);

} // namespace mh::game
