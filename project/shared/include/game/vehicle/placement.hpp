#pragma once

#include <game/physics/wheel_contact.hpp>

#include <array>
#include <optional>

namespace mh::game {

struct VehicleContactPlacement {
  OriginalBodyPoseState pose{};
  double local_up_adjustment = 0.0;
  double feasible_adjustment_minimum = 0.0;
  double feasible_adjustment_maximum = 0.0;
  std::array<CollisionHit, 4U> support_hits{};
  std::array<WheelSpringSegmentSample, 4U> spring_segments{};
};

// Finds the bounded local-up interval in which all four recovered suspension
// segments cross track geometry, then chooses its midpoint. This is a neutral
// geometric placement boundary: it does not claim the original equilibrium,
// tire radius, or race-grid spacing.
[[nodiscard]] std::optional<VehicleContactPlacement>
place_vehicle_for_full_wheel_contact(const WheelProbeRig &rig,
                                     const OriginalBodyPoseState &base_pose,
                                     const CollisionWorld &world,
                                     double maximum_local_up_adjustment);

} // namespace mh::game
