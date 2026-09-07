#pragma once

#include <game/physics/body_pose.hpp>

#include <cstddef>
#include <optional>
#include <vector>

namespace mh::game {

struct BodyHullContactSample {
  CollisionVector3 local_point{};
  CollisionVector3 segment_start{};
  CollisionVector3 segment_end{};
  std::optional<CollisionHit> hit;
  std::optional<double> hit_fraction;
};

struct BodyHullContactFrame {
  std::vector<BodyHullContactSample> samples;
  std::size_t retained_contact_count = 0U;
};

struct OriginalBodyHullReactionInputs {
  BodyBasis3 body_basis{};
  CollisionVector3 local_point{};
  CollisionVector3 local_center_of_mass{};
  CollisionVector3 world_normal{};
  CollisionVector3 world_sweep{};
  OriginalBodyVelocityState velocity{};
  OriginalBodyMassProperties mass_properties{};
  double hit_fraction = 0.0;
  bool vehicle_stabilization = false;
};

struct OriginalBodyHullReactionResult {
  CollisionVector3 body_normal{};
  CollisionVector3 local_lever{};
  CollisionVector3 local_contact_velocity{};
  CollisionVector3 world_position_correction{};
  CollisionVector3 local_linear_velocity_delta{};
  CollisionVector3 local_angular_velocity_delta{};
  double normal_velocity = 0.0;
  double effective_inverse_mass = 0.0;
  double impulse = 0.0;
};

struct OriginalBodyHullReactionApplicationResult {
  CollisionVector3 world_position_correction{};
  OriginalBodyPoseVelocityDeltaResult pose_delta{};
};

struct OriginalBodyHullPostStabilizationResult {
  std::size_t crossing_count = 0U;
  CollisionVector3 accumulated_local_linear_correction{};
  CollisionVector3 discarded_local_angular_correction{};
  CollisionVector3 applied_local_linear_delta{};
  CollisionVector3 world_displacement{};
};

// Reproduces the ordinary movable-body final crossing pass at RVA
// 0x000a3a2c..0x000a3e1b. Unlike the later vehicle-only pass, this branch has
// no slope filter and accumulates body_normal * (1 - fraction) without the
// car's factor of 50. The angular accumulator is likewise computed but not
// applied.
[[nodiscard]] OriginalBodyHullPostStabilizationResult
apply_original_generic_body_hull_post_stabilization(
    OriginalBodyPoseState &pose, OriginalBodyVelocityState &velocity,
    const BodyHullContactFrame &frame,
    const CollisionVector3 &local_center_of_mass, double slice_seconds);

// The original static-track reaction selects the active hull record with the
// smallest fraction. Equal fractions keep the earlier hull slot.
[[nodiscard]] std::optional<std::size_t>
select_original_body_hull_reaction_sample(const BodyHullContactFrame &frame);

// Reproduces the static-body impulse and penetration correction at RVA
// 0x000a401d..0x000a45a9. vehicle_stabilization selects the flag-bit-0x04
// inertia and delta filter forced by the car scheduler.
[[nodiscard]] OriginalBodyHullReactionResult
calculate_original_body_hull_reaction(
    const OriginalBodyHullReactionInputs &inputs);

// Applies the reaction to the already-predicted pose. The original adds the
// world correction first and then integrates only the local velocity deltas;
// force response and damping are not evaluated again.
[[nodiscard]] OriginalBodyHullReactionApplicationResult
apply_original_body_hull_reaction(
    OriginalBodyPoseState &pose, OriginalBodyVelocityState &velocity,
    const OriginalBodyHullReactionResult &reaction, double slice_seconds);

// Reproduces the final per-point track-crossing pass at RVA
// 0x000a35a0..0x000a3a28. It removes inward normal velocity, applies the
// original car slope filter, and adds the accumulated linear correction to
// both velocity and pose. The original computes an angular accumulator but
// clears the applied angular vector, so the discarded value is diagnostic.
[[nodiscard]] OriginalBodyHullPostStabilizationResult
apply_original_body_hull_post_stabilization(
    OriginalBodyPoseState &pose, OriginalBodyVelocityState &velocity,
    const BodyHullContactFrame &frame,
    const CollisionVector3 &local_center_of_mass, double slice_seconds);

// Reproduces the recovered current-to-retained hull record ownership. Each
// local hull point sweeps from the previous pose to the current pose; the
// resulting active records become the damping input for the following slice.
class BodyHullContactSystem {
public:
  explicit BodyHullContactSystem(BodyHullRig rig);

  [[nodiscard]] BodyHullContactFrame
  sample(const OriginalBodyPoseState &previous_pose,
         const OriginalBodyPoseState &current_pose,
         const CollisionWorld &world);

  [[nodiscard]] std::size_t retained_contact_count() const noexcept;
  [[nodiscard]] const BodyHullRig &rig() const noexcept;
  void reset() noexcept;

private:
  BodyHullRig rig_;
  std::size_t retained_contact_count_ = 0U;
};

} // namespace mh::game
