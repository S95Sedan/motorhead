#pragma once

#include <game/physics/body_pose.hpp>

#include <optional>

namespace mh::content {
struct MotionData;
}

namespace mh::game {

enum class VehicleCameraMode { chase, far_chase, in_car, bumper };

// Playable player-engine monitoring levels. Rear preserves the recovered mixer
// output, while the two body-mounted views bring the engine progressively
// forward without changing the mixer or any non-player sound owner.
[[nodiscard]] double player_engine_camera_gain(VehicleCameraMode mode) noexcept;

struct VehicleCameraConfig {
  // The one playable rear view is also the final 3-2-1 hold. Keeping one set
  // of anchors and one camera owner makes the handoff at GO seamless.
  CollisionVector3 chase_position_local{0.0, 2.0, -5.1};
  CollisionVector3 chase_target_local{0.0, 1.4333333333333333, 0.0};
  // The optional second rear view preserves the same 1:9 downward angle while
  // exposing substantially more of the car and road.
  CollisionVector3 far_chase_position_local{0.0, 2.0, -7.0};
  CollisionVector3 far_chase_target_local{0.0, 1.2222222222222223, 0.0};
  CollisionVector3 in_car_position_local{0.0, 0.75, 0.15};
  CollisionVector3 in_car_target_local{0.0, 0.75, 10.0};
  CollisionVector3 bumper_position_local{0.0, 0.48, 2.15};
  CollisionVector3 bumper_target_local{0.0, 0.50, 12.0};
};

struct VehicleCameraPose {
  CollisionVector3 world_position{};
  CollisionVector3 world_target{};
  CollisionVector3 world_up{0.0, 1.0, 0.0};
};

// Common playable camera tuning. Distance is positive behind the vehicle,
// height is above its origin, and angle is downward pitch in degrees
// (0 = level, 90 = down).
struct CameraModeTuning {
  double height = 0.0;
  double distance = 0.0;
  double angle_degrees = 0.0;
};

struct VehicleCameraTuning {
  std::optional<CameraModeTuning> chase;
  std::optional<CameraModeTuning> far_chase;
  std::optional<CameraModeTuning> in_car;
  std::optional<CameraModeTuning> bumper;
};

// Identity camera branches recovered from p3.1. Supercars uses the fixed
// top-down basis installed by RVA 0x00043988; its finalized playable anchor
// is 40 units high and 12 units ahead. Ignition retains the recovered level
// basis/pitch compensation with its finalized 15-high, 20-behind, 25-degree
// rear perspective.
[[nodiscard]] VehicleCameraPose make_original_supercars_camera(
    const OriginalBodyPoseState &vehicle_pose,
    const CameraModeTuning &tuning = {40.0, -12.0, 90.0});

[[nodiscard]] VehicleCameraPose make_original_ignition_camera(
    const OriginalBodyPoseState &vehicle_pose,
    const CameraModeTuning &tuning = {15.0, 20.0, 25.0});

// Owns the retained translation used exclusively by p3.1's Ignition/UDS
// camera at RVA 0x000439ec. The original constrains the preceding translation
// to a two-unit radius around the body, then applies fixed world-axis response
// factors (0.05, 0.9, 1.0) toward the newly derived camera position.
class OriginalIgnitionCameraRuntime {
public:
  [[nodiscard]] VehicleCameraPose
  step(const OriginalBodyPoseState &vehicle_pose,
       const CameraModeTuning &tuning = {15.0, 20.0, 25.0});

  void reset() noexcept;

private:
  bool retained_position_valid_ = false;
  CollisionVector3 retained_position_{};
};

// Retains the outside-camera translation with a stable horizontal distance.
// Body-mounted views seed it so returning outside starts from the current
// camera position.
class VehicleCameraRuntime {
public:
  [[nodiscard]] VehicleCameraPose
  step(const OriginalBodyPoseState &vehicle_pose, VehicleCameraMode mode,
       double elapsed_seconds, const VehicleCameraConfig &config = {},
       const VehicleCameraTuning &tuning = {});

  void seed(const VehicleCameraPose &camera);
  void reset() noexcept;

private:
  bool retained_position_valid_ = false;
  CollisionVector3 retained_position_{};
};

// Linearly interpolates position and reconstructs an orthonormal body basis
// from the interpolated forward/right directions. Alpha is canonical [0, 1].
[[nodiscard]] OriginalBodyPoseState
interpolate_vehicle_pose(const OriginalBodyPoseState &previous,
                         const OriginalBodyPoseState &current, double alpha);

// Builds camera anchors from an interpolated complete 3D vehicle pose. Chase
// uses the recovered p3.1 OutCarView position-height equation, world-space
// target offset, and level-horizon owner. The in-car and bumper offsets remain
// body-relative tuning.
[[nodiscard]] VehicleCameraPose
make_vehicle_camera(const OriginalBodyPoseState &vehicle_pose,
                    VehicleCameraMode mode,
                    const VehicleCameraConfig &config = {},
                    const VehicleCameraTuning &tuning = {});

// p3.1's held RearView branch at RVA 0x000786e2 copies the complete vehicle
// transform, translates two units along local Y and five along local Z, then
// applies an exact pi rotation around local Y. It bypasses the normal retained
// outside-camera recurrence while held.
[[nodiscard]] VehicleCameraPose
make_original_rear_view_camera(const OriginalBodyPoseState &vehicle_pose);

// Retail CameraView (the fixed F3 action in the stock CLO files) selects the
// nearest authored key from the track definition's CameraSplineName motion.
// Each key's Linear value selects one of seven source camera constructors.
[[nodiscard]] VehicleCameraPose
make_original_camera_spline_camera(const mh::content::MotionData &camera_motion,
                                   const OriginalBodyPoseState &vehicle_pose);

} // namespace mh::game
