#pragma once

#include <game/physics/body_response.hpp>

#include <cstddef>

namespace mh::game {

struct OriginalBodyPoseState {
  BodyBasis3 body_basis{{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};
  CollisionVector3 world_position{};
};

struct OriginalBodyPoseStepInputs {
  std::size_t retained_contact_count = 0U;
  double slice_seconds = 0.0;
};

struct OriginalBodyPoseStepResult {
  double linear_damping = 1.0;
  double angular_damping = 1.0;
  CollisionVector3 world_displacement{};
  CollisionVector3 world_linear_velocity{};
};

struct OriginalBodyPoseVelocityDeltaInputs {
  CollisionVector3 local_linear_velocity_delta{};
  CollisionVector3 local_angular_velocity_delta{};
  double slice_seconds = 0.0;
};

struct OriginalBodyPoseVelocityDeltaResult {
  CollisionVector3 world_displacement{};
  CollisionVector3 world_linear_velocity{};
};

[[nodiscard]] CollisionVector3
project_body_vector_to_world(const BodyBasis3 &basis,
                             const CollisionVector3 &body_vector);

[[nodiscard]] CollisionVector3
project_body_point_to_world(const OriginalBodyPoseState &pose,
                            const CollisionVector3 &body_point);

// Reproduces the post-response p3.1 body update at RVA 0x000a56f0 after its
// separate out-of-world guard. It damps local velocities, advances world
// position along the current body axes, applies local X/Y/Z rotations in that
// order, and expresses linear velocity in the resulting body frame.
[[nodiscard]] OriginalBodyPoseStepResult
advance_original_body_pose(OriginalBodyPoseState &pose,
                           OriginalBodyVelocityState &velocity,
                           const OriginalBodyPoseStepInputs &inputs);

// Applies a post-prediction velocity delta using the original collision
// solver's order: add the local deltas, advance the already-predicted pose by
// only those deltas, rotate X/Y/Z, then compensate the complete local linear
// velocity for the incremental basis change. No damping is repeated here.
[[nodiscard]] OriginalBodyPoseVelocityDeltaResult
advance_original_body_pose_velocity_delta(
    OriginalBodyPoseState &pose, OriginalBodyVelocityState &velocity,
    const OriginalBodyPoseVelocityDeltaInputs &inputs);

// The vehicle owner at RVA 0x000a1424 does not use the generic-body damping
// path above. Once per outer slice it advances by half the change from the
// velocity snapshot retained at the start of the preceding slice. The current
// linear velocity is then re-expressed through the incremental rotation.
[[nodiscard]] OriginalBodyPoseVelocityDeltaResult
apply_original_vehicle_pose_history_correction(
    OriginalBodyPoseState &pose, OriginalBodyVelocityState &velocity,
    const OriginalBodyVelocityState &previous_velocity, double slice_seconds);

// Advances one of the original vehicle owner's three undamped slice/3 pose
// substeps using the complete current velocity.
[[nodiscard]] OriginalBodyPoseVelocityDeltaResult
advance_original_vehicle_body_pose_substep(OriginalBodyPoseState &pose,
                                           OriginalBodyVelocityState &velocity,
                                           double substep_seconds);

} // namespace mh::game
