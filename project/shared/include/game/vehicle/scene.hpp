#pragma once

#include <game/physics/body_hull_contact.hpp>
#include <game/physics/body_pose.hpp>
#include <game/physics/vehicle_response.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace mh::game {

struct OriginalVehicleSceneState {
  OriginalBodyPoseState pose{};
  OriginalBodyVelocityState velocity{};
  // Active static-hull normals retained by the preceding vehicle step. The
  // dynamic-body owner projects its separation and linear impulse vectors
  // against these records in authored hull-point order.
  std::vector<CollisionVector3> dynamic_contact_world_normals;
};

struct OriginalVehicleSceneStepInputs {
  OriginalWheelDynamicVectorInputs dynamic_vector{};
  std::array<OriginalWheelSurfaceScaleInputs, 4U> surface_scales{};
  std::size_t retained_body_contact_count = 0U;
  std::uint8_t global_flags = 0U;
  double slice_seconds = 0.0;
  double grounded_brake_force = 0.0;
  double authored_brake_factor = 0.0;
  std::int32_t longitudinal_damping_modifier_index = -1;
  bool reduced_stabilizer_coefficient = false;
  double retained_traction_accumulator = 0.0;
  std::array<float, 4U> grip_response_constants{1.025F, 1.025F, 1.025F, 1.025F};
  std::size_t current_gear_index = 1U;
  bool alternate_damping_state_a = false;
  bool alternate_damping_state_b = false;
  bool alternate_damping_state_c = false;
};

struct OriginalVehicleSceneFrame {
  OriginalVehicleResponseFrame response{};
  std::array<OriginalVehicleResponseFrame, 3U> vehicle_response_substeps{};
  std::array<CollisionVector3, 4U> prepared_base_forces{};
  CollisionVector3 pre_wheel_angular_seed{};
  double longitudinal_load_transfer_candidate = 0.0;
  OriginalBodyPoseStepResult pose{};
  OriginalBodyPoseState physics_previous_pose{};
  OriginalBodyPoseState physics_final_pose{};
  OriginalBodyPoseVelocityDeltaResult vehicle_history_correction{};
  std::array<OriginalBodyPoseVelocityDeltaResult, 3U> vehicle_pose_substeps{};
  std::array<OriginalBodyPoseState, 3U> vehicle_post_pose_states{};
  std::array<OriginalBodyVelocityState, 3U> vehicle_post_pose_velocities{};
  std::optional<OriginalVehicleGroundedDampingResult> grounded_damping;
  std::optional<OriginalVehicleGroundedDampingSelection>
      grounded_damping_selection;
  std::optional<BodyHullContactFrame> body_hull;
  std::vector<BodyHullContactSample> body_hull_reaction_samples;
  std::vector<OriginalBodyHullReactionResult> body_hull_reactions;
  // p3.1 retains the strictly greatest absolute rigid impulse in each outer
  // frame together with its material, scratch metric, and selected hull slot.
  // The race owner consumes this record on the following outer frame.
  struct HullAudioRecord {
    double maximum_absolute_impulse = 0.0;
    double scratch_candidate = 0.0;
    std::size_t material_index = 0U;
    std::size_t hull_point_index = 0U;
    bool active = false;
  } hull_audio_record{};
  std::optional<OriginalBodyHullPostStabilizationResult>
      body_hull_post_stabilization;
};

struct OriginalVehicleModeCSceneStepInputs {
  OriginalDrivetrainModeCForceInputs drivetrain{};
  std::array<OriginalWheelSurfaceScaleInputs, 4U> surface_scales{};
  std::size_t retained_body_contact_count = 0U;
  std::uint8_t global_flags = 0U;
  std::int32_t longitudinal_damping_modifier_index = -1;
  std::array<float, 4U> grip_response_constants{1.025F, 1.025F, 1.025F, 1.025F};
  bool handbrake_active = false;
};

struct OriginalVehicleModeCSceneFrame {
  OriginalDrivetrainModeCForceResult drivetrain{};
  OriginalVehicleSceneFrame scene{};
};

struct OriginalVehicleSceneCollisionStage {
  OriginalVehicleSceneFrame frame{};
  OriginalVehicleSceneStepInputs inputs{};
  OriginalBodyPoseState physics_previous_pose{};
  std::optional<BodyHullContactFrame> retained_body_hull;
  std::size_t static_passes_attempted = 0U;
  bool finalized = false;
};

// Reconstructs the normal grounded branch's retained-surface aggregate. The
// p3.1 material initializer writes the exact default row to every material
// before Material.mat overrides. Runtime track import binds each collision
// material index through its track-local SRF name table.
[[nodiscard]] OriginalVehicleGroundedDampingProfile
calculate_original_vehicle_grounded_damping_profile(
    const WheelContactFrame &contacts,
    const std::array<float, 4U> &grip_response_constants = {1.025F, 1.025F,
                                                            1.025F, 1.025F},
    const OriginalVehicleGroundedMaterialTable &materials = {});

// Selects the two p3.1 grounded damping profiles. The alternate profile is
// entered in a forward gear when the exact lateral-velocity or
// speed-squared/yaw thresholds are crossed, or when one of the three retained
// owner-state inputs is active.
[[nodiscard]] OriginalVehicleGroundedDampingSelection
select_original_vehicle_grounded_damping_profile(
    const WheelContactFrame &contacts,
    const std::array<float, 4U> &grip_response_constants,
    const OriginalBodyVelocityState &velocity, std::size_t current_gear_index,
    bool alternate_state_a = false, bool alternate_state_b = false,
    bool alternate_state_c = false,
    const OriginalVehicleGroundedMaterialTable &materials = {});

// Owns the recovered vehicle contact-to-pose order: prior-velocity correction,
// three force/pose substeps, bounded rigid hull reaction, and final crossing
// stabilization. The explicit retained count remains source-compatible but is
// not consumed by the distinct undamped car pose owner.
class OriginalVehicleSceneSystem {
public:
  explicit OriginalVehicleSceneSystem(OriginalVehicleResponseConfig config);
  OriginalVehicleSceneSystem(OriginalVehicleResponseConfig config,
                             BodyHullRig body_hull);

  [[nodiscard]] OriginalVehicleSceneFrame
  step(OriginalVehicleSceneState &state,
       const OriginalVehicleSceneStepInputs &inputs,
       const CollisionWorld &world);

  // Splits the recovered scene step at p3.1's shared collision scheduler.
  // begin_collision_step leaves state.pose in the internal physics-origin
  // frame. Each static pass mutates that state once. finish_collision_step
  // performs the final crossing stabilization and restores the car object
  // origin used by rendering, AI, and dynamic-body collision.
  [[nodiscard]] OriginalVehicleSceneCollisionStage
  begin_collision_step(OriginalVehicleSceneState &state,
                       const OriginalVehicleSceneStepInputs &inputs,
                       const CollisionWorld &world);
  [[nodiscard]] bool
  apply_static_collision_pass(OriginalVehicleSceneState &state,
                              OriginalVehicleSceneCollisionStage &stage,
                              const CollisionWorld &world);
  [[nodiscard]] OriginalVehicleSceneFrame
  finish_collision_step(OriginalVehicleSceneState &state,
                        OriginalVehicleSceneCollisionStage &stage,
                        const CollisionWorld &world);

  [[nodiscard]] OriginalVehicleModeCSceneFrame
  step_mode_c(OriginalVehicleSceneState &state,
              const OriginalVehicleModeCSceneStepInputs &inputs,
              const CollisionWorld &world);

  [[nodiscard]] const OriginalVehicleResponseConfig &config() const noexcept;
  void seed_wheel_contact_history(
      const std::array<WheelContactScalarState, 4U> &current);
  void
  seed_previous_vehicle_velocity(const OriginalBodyVelocityState &velocity);
  void seed_previous_wheel_contact_count(std::size_t count);
  void
  seed_previous_wheel_contact_flags(const std::array<std::uint32_t, 4U> &flags);
  void reset() noexcept;

private:
  OriginalVehicleResponseSystem response_;
  std::optional<BodyHullContactSystem> body_hull_;
  OriginalBodyVelocityState previous_vehicle_velocity_{};
  std::size_t previous_wheel_contact_history_sum_ = 0U;
};

} // namespace mh::game
