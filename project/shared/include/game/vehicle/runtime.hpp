#pragma once

#include <game/physics/dynamic_vehicle_contact.hpp>
#include <game/vehicle/drive.hpp>
#include <game/vehicle/placement.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace mh::game {

struct OriginalVehicleRaceRecoveryState {
  bool external_recovery_active = false;
  float external_recovery_seconds = 0.0F;
  float insufficient_contact_seconds = 0.0F;
  bool ai_recovery_phase_active = false;
  float ai_recovery_phase_seconds = 0.0F;
};

struct OriginalVehicleRaceRecoveryInput {
  bool ai_vehicle = false;
  bool ai_stuck_recovery_active = false;
  bool external_recovery_active = false;
  bool reset_enabled = true;
  bool reset_inhibited = false;
  std::array<double, 3U> observer_position{};
  // Tangent selected from the track SplineName owner for sub_0003b710. Retail
  // retains the object position and rebuilds its basis from this direction.
  std::array<double, 3U> recovery_spline_direction{};
  double slice_seconds = 0.0;
  // Optional off-route relocation; empty preserves retail's in-place reset.
  std::optional<std::array<double, 3U>> recovery_spline_position;
  bool recover_below_track = false;
};

enum class OriginalVehicleRaceRecoveryReason {
  none,
  below_world,
  external_state_timeout,
  insufficient_contact_timeout,
  distant_ai_stuck_timeout,
};

struct OriginalVehicleRaceRecoveryResult {
  OriginalVehicleRaceRecoveryReason reason =
      OriginalVehicleRaceRecoveryReason::none;
  bool reset_applied = false;
};

// Complete deterministic owner for one recovered vehicle in one collision
// world. Content import remains outside this class; a playable host and a
// headless replay therefore share the same placement, reset, and step path.
class OriginalVehicleRuntime {
public:
  OriginalVehicleRuntime(CollisionWorld world,
                         OriginalVehicleResponseConfig response_config,
                         BodyHullRig body_hull,
                         RecoveredVehicleRuntimeTuning drive_tuning,
                         OriginalBodyPoseState start_pose,
                         double maximum_start_adjustment = 4.0);

  [[nodiscard]] OriginalVehicleModeCSceneFrame
  step(const ControlInput &controls, double slice_seconds);
  // Two-phase form used by the shared p3.1 collision scheduler. All live
  // vehicles begin their force/pose work first; the scheduler then calls one
  // static pass immediately before each dynamic full-list scan; finalization
  // performs the post-collision hull stabilization and drivetrain commit.
  void begin_interleaved_collision_step(const ControlInput &controls,
                                        double slice_seconds);
  [[nodiscard]] bool apply_interleaved_static_collision_pass();
  [[nodiscard]] OriginalVehicleModeCSceneFrame
  finish_interleaved_collision_step();
  [[nodiscard]] bool interleaved_collision_step_active() const noexcept;
  // p3.1 copies the preceding body material to car+0x154 once before its
  // bounded slice loop. Hosts that execute multiple slices for one elapsed
  // frame bracket them with these calls; a standalone step brackets itself.
  void begin_outer_frame();
  void end_outer_frame();
  void advance_drive_state_only(const ControlInput &controls,
                                double slice_seconds);
  void apply_drive_performance_scale(float factor);
  void set_automatic_transmission(bool automatic) noexcept;
  // Installs the original identity-selector byte consumed by vehicle physics.
  // Bit 0x02 selects the recovered moon-gravity branch; the suspension
  // response profile itself remains part of the immutable response config.
  void set_original_global_flags(std::uint8_t flags) noexcept;
  // Advances suspension/contact state while retaining the authored grid X/Z
  // and orientation. This reproduces the pre-race settling boundary without
  // allowing drivetrain creep or collision response to move a staged car out
  // of its slot.
  void settle_suspension_on_grid(double slice_seconds);
  // Seeds the retained per-wheel scalar history consumed by the next original
  // suspension response. Values are the original post-mapping state fractions,
  // not segment hit fractions.
  void seed_wheel_contact_history(const std::array<float, 4U> &state_fractions);
  // Capture comparators also need the previous-contact flags retained beside
  // those fractions. Ordinary race initialization uses the one-argument form.
  void seed_wheel_contact_history(
      const std::array<float, 4U> &state_fractions,
      const std::array<std::uint32_t, 4U> &previous_contact_flags);
  void seed_captured_drive_state(const OriginalVehicleDriveState &state);
  // Installs car+0x90 for a paired retail transition. Normal gameplay updates
  // it from the preceding grounded damping branch.
  void seed_reduced_stabilizer_selector(bool active) noexcept;
  void seed_retained_traction_accumulator(double value);
  // Installs car+0x154 for a paired retail transition. Normal gameplay derives
  // it from the preceding outer frame's selected static-hull material.
  void seed_longitudinal_damping_modifier_index(std::int32_t material_index);
  // Installs a captured outer body pose and current body velocity. AI reads
  // this matrix directly; the response owner applies its internal -0.32
  // local-Y shift only while sampling suspension and collision.
  void
  seed_captured_body_state(const OriginalBodyPoseState &body_pose,
                           const OriginalBodyVelocityState &velocity,
                           const OriginalBodyVelocityState &previous_velocity);
  // Applies p3.1's race-level post-physics recovery owner at RVA
  // 0x0001d05c and its reset caller at 0x0007826c..0x00078313.
  [[nodiscard]] OriginalVehicleRaceRecoveryResult
  update_race_recovery(const OriginalVehicleModeCSceneFrame &frame,
                       const OriginalVehicleRaceRecoveryInput &input);
  void reset();

  [[nodiscard]] const OriginalVehicleSceneState &state() const noexcept;
  [[nodiscard]] OriginalBodyPoseState physics_pose() const noexcept;
  [[nodiscard]] OriginalBodyPoseState ai_pose() const;
  [[nodiscard]] std::array<WheelSpringSegmentSample, 4U>
  current_wheel_contacts() const;
  [[nodiscard]] const OriginalBodyPoseState &previous_pose() const noexcept;
  [[nodiscard]] const OriginalVehicleDriveSystem &drive() const noexcept;
  [[nodiscard]] std::int32_t
  longitudinal_damping_modifier_index() const noexcept;
  [[nodiscard]] const OriginalVehicleRaceRecoveryState &
  race_recovery_state() const noexcept;
  [[nodiscard]] const CollisionWorld &world() const noexcept;

  // Applies the split world-position correction and local velocity result
  // produced by the original dynamic-body solver after the recovered
  // static-world vehicle step.
  void
  apply_dynamic_contact(const CollisionVector3 &world_position_delta,
                        const CollisionVector3 &local_linear_velocity_delta,
                        const CollisionVector3 &local_angular_velocity_delta);

private:
  CollisionWorld world_;
  OriginalVehicleSceneSystem scene_;
  OriginalVehicleDriveSystem drive_;
  OriginalBodyPoseState start_pose_{};
  double maximum_start_adjustment_ = 4.0;
  OriginalVehicleSceneState state_{};
  OriginalBodyPoseState previous_pose_{};
  OriginalVehicleRaceRecoveryState race_recovery_{};
  std::optional<double> track_floor_;
  std::optional<OriginalBodyPoseState> last_grounded_pose_;
  // p3.1 copies body+0x72c to car+0x154 before the next outer frame, then
  // clears the body field. Any nonnegative material selects the 0.4
  // longitudinal grounded-damping multiplier.
  std::int32_t longitudinal_damping_modifier_index_ = -1;
  std::int32_t retained_hull_material_index_ = -1;
  bool reduced_stabilizer_selector_ = false;
  double retained_traction_accumulator_ = 0.0;
  bool outer_frame_active_ = false;
  std::uint8_t original_global_flags_ = 0U;
  struct PendingInterleavedCollisionStep {
    OriginalVehicleModeCSceneFrame frame{};
    OriginalVehicleSceneCollisionStage scene_stage{};
    bool implicit_outer_frame = false;
  };
  std::optional<PendingInterleavedCollisionStep>
      pending_interleaved_collision_step_;

  [[nodiscard]] bool
  apply_race_recovery_reset(
      const std::array<double, 3U> &spline_direction,
      const std::optional<std::array<double, 3U>> &spline_position);
  void apply_manual_gear_overspeed_response(double slice_seconds);
};

struct OriginalDynamicVehicleContactScheduleParticipant {
  OriginalVehicleRuntime *vehicle = nullptr;
  const OriginalDynamicVehicleContactShape *shape = nullptr;
  // Number of successful static-hull passes already reproduced by the
  // vehicle scene for this slice. Retail uses either static or dynamic
  // success to keep the per-body loop alive, up to its three-pass limit.
  std::size_t static_reaction_passes = 0U;
};

struct OriginalDynamicVehicleContactScheduleEvent {
  std::size_t pass = 0U;
  std::size_t first_index = 0U;
  std::size_t second_index = 0U;
  OriginalVehicleSceneState first_before{};
  OriginalVehicleSceneState second_before{};
  OriginalDynamicVehicleContactResponse response{};
  OriginalVehicleSceneState first_after{};
  OriginalVehicleSceneState second_after{};
};

struct OriginalInterleavedVehicleContactScheduleResult {
  std::vector<OriginalDynamicVehicleContactScheduleEvent> events;
  // Indexed exactly like the participant list. Retail finalizes an outer body
  // before advancing to the next one, so these frames are produced inside the
  // shared scheduler rather than in a later all-body loop.
  std::vector<OriginalVehicleModeCSceneFrame> frames;
};

struct OriginalEnvironmentDynamicContactParticipant {
  OriginalVehicleSceneState *body = nullptr;
  const OriginalDynamicVehicleContactShape *shape = nullptr;
  std::uint32_t collision_identifier = 0U;
};

struct OriginalVehicleEnvironmentCollisionState {
  bool active = false;
  std::uint32_t collision_identifier = 0U;
  double maximum_collision_sound_scalar = 0.0;
};

struct OriginalVehicleEnvironmentContactEvent {
  std::size_t vehicle_index = 0U;
  std::size_t environment_index = 0U;
  OriginalVehicleSceneState vehicle_before{};
  OriginalVehicleSceneState environment_before{};
  OriginalDynamicVehicleContactResponse response{};
  OriginalVehicleSceneState vehicle_after{};
  OriginalVehicleSceneState environment_after{};
};

struct OriginalVehicleEnvironmentContactScheduleResult {
  std::vector<OriginalVehicleEnvironmentContactEvent> events;
  std::vector<OriginalVehicleEnvironmentCollisionState> vehicle_collisions;
};

struct OriginalEnvironmentBodyStaticContactResult {
  std::size_t passes_attempted = 0U;
  BodyHullContactFrame retained_body_hull{};
  std::vector<BodyHullContactSample> reaction_samples;
  std::vector<OriginalBodyHullReactionResult> reactions;
  std::optional<OriginalBodyHullPostStabilizationResult> post_stabilization;
};

// Reproduces the dynamic portion of p3.1 scheduler
// 0x000a5b10..0x000a5b8d. Each body becomes the outer owner in list order and
// scans the complete list through solver 0x000a4cf8 on each of up to three
// passes. This is intentionally different from three global unique-pair
// passes. Returned events preserve commit order for diagnostics.
[[nodiscard]] std::vector<OriginalDynamicVehicleContactScheduleEvent>
apply_original_dynamic_vehicle_contact_schedule(
    std::span<const OriginalDynamicVehicleContactScheduleParticipant>
        participants);

// Complete collision portion of p3.1 scheduler 0x000a5b10..0x000a5b8d for
// runtimes that have all entered begin_interleaved_collision_step(). Each
// outer-body pass mutates one static reaction and then scans the full dynamic
// list before continuing.
[[nodiscard]] OriginalInterleavedVehicleContactScheduleResult
apply_original_interleaved_vehicle_contact_schedule(
    std::span<const OriginalDynamicVehicleContactScheduleParticipant>
        participants);

// Later direct-body pass at RVA 0x000a5c27..0x000a5c59. Each vehicle scans
// every LWS body in source-list order after its ordinary collision step has
// finalized. Accepted contacts copy the LWS body+0x14 identifier to the car.
[[nodiscard]] OriginalVehicleEnvironmentContactScheduleResult
apply_original_vehicle_environment_contact_schedule(
    std::span<const OriginalDynamicVehicleContactScheduleParticipant> vehicles,
    std::span<const OriginalEnvironmentDynamicContactParticipant>
        environment_bodies);

// Reproduces the later ordinary movable-body static-track owner at RVA
// 0x000a5c6b..0x000a5d2a. It runs at most three rigid hull reactions, stops on
// the first pass without a hit, executes the distinct generic final-crossing
// branch only when the last pass hit, and retains the union of successful
// current records for the next generic pose step.
[[nodiscard]] OriginalEnvironmentBodyStaticContactResult
apply_original_environment_body_static_contact_schedule(
    OriginalVehicleSceneState &body,
    const OriginalDynamicVehicleContactShape &shape,
    const OriginalBodyPoseState &physics_previous_pose,
    const CollisionWorld &world, double slice_seconds);

} // namespace mh::game
