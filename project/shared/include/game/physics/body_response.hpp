#pragma once

#include <game/physics/collision.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace mh::game {

using BodyBasis3 = std::array<CollisionVector3, 3U>;

struct BodyForceAccumulator {
  CollisionVector3 linear{};
  CollisionVector3 angular{};
};

struct OriginalBodyVelocityState {
  CollisionVector3 local_linear{};
  CollisionVector3 local_angular{};
};

struct OriginalBodyMassProperties {
  double mass = 0.0;
  CollisionVector3 principal_inertia{};
};

struct OriginalBodyForceResponseInputs {
  BodyForceAccumulator accumulated_force{};
  double mass = 0.0;
  CollisionVector3 principal_inertia{};
  double slice_seconds = 0.0;
};

struct OriginalWheelForceInputs {
  BodyBasis3 body_basis{};
  CollisionVector3 world_normal{};
  // The physical meaning of the original slot +0x1c/+0x20/+0x24 vector is
  // deliberately left open until its producer is recovered.
  CollisionVector3 dynamic_vector{};
  CollisionVector3 base_force{};
  double dynamic_x_scale = 0.0;
  double suspension_axis_y = 0.0;
  double spring_scalar = 0.0;
  double state_delta_scalar = 0.0;
};

struct OriginalWheelForceResult {
  CollisionVector3 body_normal{};
  CollisionVector3 force{};
};

struct OriginalWheelDynamicVectorInputs {
  // These names describe data flow only; their physical meaning remains open.
  double primary_scalar = 0.0;
  double secondary_scalar = 0.0;
  double body_phase = 0.0;
  bool alternate_distribution = false;
};

struct OriginalWheelPrimaryScalarInputs {
  double steering = 0.0;
  double turn_force = 0.0;
  double ramp_limit = 0.0;
  double longitudinal_velocity = 0.0;
  // Mirrors original car offset +0x90: nonzero suppresses the negative-body
  // sign reversal. Its physical mode name remains open.
  bool suppress_negative_body_reversal = false;
};

struct OriginalRetainedSteeringInputs {
  double previous = 0.0;
  double requested = 0.0;
};

struct OriginalWheelSurfaceScaleInputs {
  bool has_retained_surface = false;
  double static_factor = 1.0;
  double active_factor = 1.0;
  double active_divisor = 1.0;
  bool use_active_factor = false;
  bool apply_reduction = false;
};

struct OriginalWheelBaseForceInputs {
  BodyBasis3 body_basis{};
  double authored_weight = 0.0;
  double global_vertical_scale = 0.0;
  double longitudinal_velocity = 0.0;
};

struct OriginalDrivetrainCandidateGearInputs {
  std::size_t current_gear_index = 0U;
  std::size_t maximum_gear_index = 0U;
  // Original state offsets +0x58 and +0x5c; physical action names remain open.
  bool first_shift_latched = false;
  bool second_shift_latched = false;
};

struct OriginalDrivetrainControlMapping {
  double drive_scalar = 0.0;
  double brake_force = 0.0;
};

struct OriginalDrivetrainBaselineInputs {
  bool alternate_mode = false;
  bool transition_active = false;
  std::size_t current_gear_index = 0U;
  double control_a = 0.0;
  double control_b = 0.0;
  double current_gear_value = 0.0;
  double engine_scalar = 0.0;
  double longitudinal_velocity = 0.0;
};

struct OriginalDrivetrainAccelerationModeAInputs {
  double engine_scalar = 0.0;
  double authored_minimum = 0.0;
  double authored_maximum = 0.0;
  double reference_value = 0.0;
  double candidate_gear_value = 0.0;
  double drive_scalar = 0.0;
  double authored_acceleration_force = 0.0;
};

struct OriginalDrivetrainAccelerationModeBInputs {
  double longitudinal_velocity = 0.0;
  double current_gear_value = 0.0;
  double authored_minimum = 0.0;
  double authored_maximum = 0.0;
  double drive_scalar = 0.0;
  double authored_acceleration_force = 0.0;
};

struct OriginalDrivetrainAccelerationModeCInputs {
  double longitudinal_velocity = 0.0;
  double current_gear_value = 0.0;
  std::size_t candidate_gear_index = 0U;
  double authored_minimum = 0.0;
  double authored_maximum = 0.0;
  double drive_scalar = 0.0;
  double authored_acceleration_force = 0.0;
};

struct OriginalDrivetrainAutomaticGearInputs {
  double longitudinal_velocity = 0.0;
  double control_a = 0.0;
  double control_b = 0.0;
  double transition_scalar = 0.0;
  double authored_maximum = 0.0;
  std::size_t current_gear_index = 0U;
  std::size_t maximum_gear_index = 0U;
  // Original state offset +0xa0. Its complete transition ownership remains
  // open, so the neutral data-flow name is retained here.
  bool direction_transition_active = false;
};

struct OriginalDrivetrainManualControls {
  double drive_scalar = 0.0;
  double brake_force = 0.0;
  bool moderate_control_b_active = false;
  bool strong_control_b_active = false;
};

struct OriginalDrivetrainManualGearInputs {
  std::uint32_t control_flags = 0U;
  double longitudinal_velocity = 0.0;
  std::size_t current_gear_index = 0U;
  std::size_t maximum_gear_index = 0U;
  bool first_shift_latched = false;
  bool second_shift_latched = false;
};

struct OriginalDrivetrainManualGearResult {
  std::size_t current_gear_index = 0U;
  bool first_shift_latched = false;
  bool second_shift_latched = false;
};

struct OriginalDrivetrainEngineStateInputs {
  double engine_scalar = 0.0;
  double transition_candidate = 0.0;
  double authored_minimum = 0.0;
  double authored_maximum = 0.0;
  double slice_seconds = 0.0;
  double control_a = 0.0;
  bool first_shift_latched = false;
  bool second_shift_latched = false;
  // Original state offsets +0x13c and +0x140 remain physically unnamed.
  bool transition_source_a_active = false;
  bool transition_source_b_active = false;
  // A nonzero entry value prevents the direct transition-candidate commit.
  bool external_transition_active = false;
};

struct OriginalDrivetrainEngineStateResult {
  double engine_scalar = 0.0;
  bool transition_active = false;
};

// One normal automatic mode-C state transition. This binds the shared
// speed-derived candidate to the recovered automatic-gear and engine-state
// updates without assigning a physical name to the traction accumulator.
struct OriginalDrivetrainModeCStateInputs {
  double longitudinal_velocity = 0.0;
  double control_a = 0.0;
  double control_b = 0.0;
  double current_gear_value = 0.0;
  double engine_scalar = 0.0;
  double authored_minimum = 0.0;
  double authored_maximum = 0.0;
  double slice_seconds = 0.0;
  std::size_t current_gear_index = 0U;
  std::size_t maximum_gear_index = 0U;
  bool first_shift_latched = false;
  bool second_shift_latched = false;
  bool direction_transition_active = false;
  bool transition_source_a_active = false;
  bool transition_source_b_active = false;
  bool external_transition_active = false;
};

struct OriginalDrivetrainModeCStateResult {
  double transition_candidate = 0.0;
  std::size_t current_gear_index = 0U;
  double engine_scalar = 0.0;
  bool engine_transition_active = false;
  // Original drivetrain +0xa0. The current control owner replaces it every
  // update from the strict control-B > 0.9 comparison.
  bool direction_transition_active = false;
  // Original drivetrain +0x98 and car +0x94. The first is the recovered
  // candidate comparison; the second additionally requires the preceding
  // +0x9c engine-transition state to be clear.
  bool primary_drive_candidate_active = false;
  bool primary_drive_transition_active = false;
  bool first_shift_latched = false;
  bool second_shift_latched = false;
};

struct OriginalDrivetrainModeCForceInputs {
  OriginalDrivetrainModeCStateInputs state{};
  double steering = 0.0;
  double authored_brake_force = 0.0;
  double authored_turn_force = 0.0;
  double steering_ramp_limit = 0.0;
  double authored_acceleration_force = 0.0;
  std::uint32_t acceleration_selector = 0U;
  bool suppress_negative_body_reversal = false;
  bool engine_transition_active = false;
  std::uint32_t control_flags = 0U;
  // p3.1 suppresses +0x98 only when a separate global selector is active and
  // acceleration mode 2 is selected. Ordinary race setup leaves it false.
  bool suppress_primary_drive_candidate = false;
  // The manual branch consumes the pre-shift candidate ratio and the fixed
  // drivetrain +0xd4 reference scalar. Automatic mode does not read either.
  double candidate_gear_value = 0.0;
  double reference_value = 3000.0;
  bool manual_mode = false;
};

struct OriginalDrivetrainPrimaryDriveCandidateInputs {
  double longitudinal_velocity = 0.0;
  double steering = 0.0;
  double control_a = 0.0;
  double current_gear_value = 0.0;
  double current_engine_scalar = 0.0;
  double authored_minimum = 0.0;
  std::size_t current_gear_index = 0U;
  std::size_t candidate_gear_index = 0U;
  bool engine_transition_active = false;
  bool suppressed = false;
};

struct OriginalDrivetrainPrimaryDriveCandidateResult {
  double candidate = 0.0;
  bool candidate_active = false;
  bool transition_active = false;
};

struct OriginalDrivetrainModeCForceResult {
  OriginalDrivetrainModeCStateResult state{};
  std::size_t candidate_gear_index = 0U;
  OriginalDrivetrainControlMapping controls{};
  double baseline_contribution = 0.0;
  double acceleration_contribution = 0.0;
  double brake_contribution = 0.0;
  double low_speed_contribution = 0.0;
  double drivetrain_output = 0.0;
  OriginalWheelDynamicVectorInputs wheel_dynamic_input{};
};

struct OriginalWheelLongitudinalLoadTransferInputs {
  double previous_longitudinal_velocity = 0.0;
  double current_longitudinal_velocity = 0.0;
  double slice_seconds = 0.0;
  bool all_wheels_grounded = false;
};

struct OriginalWheelLongitudinalLoadTransferResult {
  std::array<CollisionVector3, 4U> base_forces{};
  double candidate = 0.0;
};

// The p3.1 grounded-car branch applies these exponential bases once after
// all three force/pose substeps. The current values are instruction-locked
// where literal and oracle-locked for the stable Goldbridge road profile.
struct OriginalVehicleGroundedDampingProfile {
  double lateral_linear_base = 0.0005;
  double longitudinal_linear_base = 0.8581;
  CollisionVector3 angular_bases{0.00005, 0.000007175, 0.00005};
};

// One authored Material.mat Snd* value. The retail row stores a WAV name,
// followed by two integers, two floats, and two more integers. Their runtime
// meanings remain deliberately unnamed until the dispatch owner is recovered;
// retaining the exact source tuple avoids baking in guessed audio semantics.
struct OriginalMaterialSoundDefinition {
  std::string wave_file;
  std::int32_t parameter_1 = 0;
  std::int32_t parameter_2 = 0;
  float scalar_1 = 0.0F;
  float scalar_2 = 0.0F;
  std::int32_t parameter_5 = 0; // leading sample-frame trim
  std::int32_t parameter_6 = 0; // trailing sample-frame trim
};

// One p3.1 0x4c0-byte material row's grounded-vehicle fields. The eight
// breakaway/damping floats begin at +0x20, the turn-reduction values follow,
// SparkAnimID is stored at +0x4b0 and the authored skid-mark selector at
// +0x4bc. A track-local SRF slot replaces the initializer defaults with its
// named Material.mat row.
struct OriginalVehicleGroundedMaterialProfile {
  float lateral_threshold = 7.0F;
  float yaw_threshold = 26000.0F;
  float alternate_lateral_base = 0.2F;
  float lateral_base = 0.000555F;
  float alternate_longitudinal_base = 0.796714F;
  float longitudinal_base = 0.858086F;
  float alternate_angular_y_base = 0.010429F;
  float angular_y_base = 0.000007F;
  float spin_turn_reduce = 0.9F;
  float no_spin_turn_reduce = 1.0F;
  std::optional<OriginalMaterialSoundDefinition> contact_sound;
  std::optional<OriginalMaterialSoundDefinition> weak_impulse_sound;
  std::optional<OriginalMaterialSoundDefinition> hard_impulse_sound;
  std::optional<OriginalMaterialSoundDefinition> scratch_sound;
  std::optional<OriginalMaterialSoundDefinition> slide_sound;
  std::optional<OriginalMaterialSoundDefinition> spin_sound;
  std::uint8_t spark_animation_id = 0U;
  std::uint8_t slide_type = 0U;
  std::uint8_t skid_mark = 0U;
};

using OriginalVehicleGroundedMaterialTable =
    std::array<OriginalVehicleGroundedMaterialProfile, 26U>;

struct OriginalVehicleGroundedDampingResult {
  OriginalVehicleGroundedDampingProfile applied_profile{};
  double linear_base_scale = 1.0;
  CollisionVector3 linear_factors{1.0, 1.0, 1.0};
  CollisionVector3 angular_factors{1.0, 1.0, 1.0};
  OriginalBodyVelocityState input_velocity{};
  OriginalBodyVelocityState output_velocity{};
};

struct OriginalVehicleGroundedDampingSelection {
  OriginalVehicleGroundedDampingProfile profile{};
  bool alternate_branch = false;
  // Retain the three explicit car-state inputs separately. The original
  // skid-mark owner consumes state A, state B, and the resulting +0xcc
  // accumulator; alternate_branch alone is intentionally broader.
  bool alternate_state_a = false;
  bool alternate_state_b = false;
  bool alternate_state_c = false;
  double lateral_threshold = 0.0;
  double lateral_excess = 0.0;
  double yaw_metric = 0.0;
  double yaw_threshold = 0.0;
  // Original car +0xcc. The alternate branch retains this value for the
  // following update's spin/turn surface scaling.
  double retained_traction_accumulator = 0.0;
};

struct OriginalVehicleVelocityStabilizerInputs {
  OriginalBodyVelocityState velocity{};
  std::size_t wheel_count = 4U;
  std::size_t retained_history_sum = 0U;
  // Original owner +0x90 selects 10 instead of 80. The physical mode name is
  // still open, so retain a data-flow description.
  bool reduced_coefficient = false;
};

[[nodiscard]] CollisionVector3
project_world_vector_to_body(const BodyBasis3 &basis,
                             const CollisionVector3 &world_vector);

[[nodiscard]] OriginalWheelForceResult
calculate_original_wheel_force(const OriginalWheelForceInputs &inputs);

// FL, FR, BR, BL: the original four-slot order.
[[nodiscard]] std::array<CollisionVector3, 4U>
calculate_original_wheel_dynamic_vectors(
    const OriginalWheelDynamicVectorInputs &inputs);

[[nodiscard]] double calculate_original_wheel_primary_scalar(
    const OriginalWheelPrimaryScalarInputs &inputs);

// Body +0xd8 is updated once per vehicle step as
// previous + (clamp(requested, -1, 1) - previous) * 0.2, with the original
// float32 store boundary and final [-1, 1] clamp.
[[nodiscard]] double calculate_original_retained_steering(
    const OriginalRetainedSteeringInputs &inputs);

[[nodiscard]] double
calculate_original_wheel_secondary_scalar(double drivetrain_output);

[[nodiscard]] double calculate_original_wheel_dynamic_x_scale(
    const OriginalWheelSurfaceScaleInputs &inputs);

// FL, FR, BR, BL: the original four-slot order.
[[nodiscard]] std::array<CollisionVector3, 4U>
calculate_original_wheel_base_forces(
    const OriginalWheelBaseForceInputs &inputs);

[[nodiscard]] OriginalWheelLongitudinalLoadTransferResult
apply_original_wheel_longitudinal_load_transfer(
    const std::array<CollisionVector3, 4U> &base_forces,
    const OriginalWheelLongitudinalLoadTransferInputs &inputs);

[[nodiscard]] double
finalize_original_drivetrain_output(double accumulated_output,
                                    std::uint32_t control_flags,
                                    std::size_t selected_gear_index);

[[nodiscard]] std::size_t calculate_original_drivetrain_candidate_gear(
    const OriginalDrivetrainCandidateGearInputs &inputs);

// Recovers p3.1 drivetrain +0x98 and its car +0x94 handoff from
// 0x0001d3fb..0x0001d563. The four nonzero schedule values are executable
// constants, not CAR-specific tuning.
[[nodiscard]] OriginalDrivetrainPrimaryDriveCandidateResult
calculate_original_drivetrain_primary_drive_candidate(
    const OriginalDrivetrainPrimaryDriveCandidateInputs &inputs);

[[nodiscard]] OriginalDrivetrainControlMapping
calculate_original_drivetrain_control_mapping(double control_a,
                                              double control_b,
                                              double authored_brake_force,
                                              std::size_t current_gear_index);

[[nodiscard]] double calculate_original_drivetrain_baseline_contribution(
    const OriginalDrivetrainBaselineInputs &inputs);

[[nodiscard]] double
calculate_original_drivetrain_brake_contribution(double longitudinal_velocity,
                                                 double control_b,
                                                 double authored_brake_force);

[[nodiscard]] double calculate_original_drivetrain_low_speed_contribution(
    double longitudinal_velocity, double drive_scalar);

[[nodiscard]] double calculate_original_drivetrain_manual_coast_contribution(
    double longitudinal_velocity, double current_gear_value,
    double engine_scalar, double control_a, bool engine_transition_active);

[[nodiscard]] double calculate_original_drivetrain_engine_candidate(
    double engine_scalar, double drive_term, double slice_seconds,
    double authored_maximum);

[[nodiscard]] double calculate_original_drivetrain_engine_drive_term(
    double candidate_gear_value, double reference_value, double drive_scalar);

[[nodiscard]] double calculate_original_drivetrain_speed_candidate(
    double longitudinal_velocity, double current_gear_value,
    double authored_minimum, double authored_maximum);

[[nodiscard]] double calculate_original_drivetrain_acceleration_mode_a(
    const OriginalDrivetrainAccelerationModeAInputs &inputs);

[[nodiscard]] double calculate_original_drivetrain_acceleration_mode_b(
    const OriginalDrivetrainAccelerationModeBInputs &inputs);

[[nodiscard]] double calculate_original_drivetrain_acceleration_mode_c(
    const OriginalDrivetrainAccelerationModeCInputs &inputs);

[[nodiscard]] std::size_t calculate_original_drivetrain_automatic_gear(
    const OriginalDrivetrainAutomaticGearInputs &inputs);

[[nodiscard]] OriginalDrivetrainManualControls
calculate_original_drivetrain_manual_controls(double control_a,
                                              double control_b,
                                              double authored_brake_force);

[[nodiscard]] OriginalDrivetrainManualGearResult
calculate_original_drivetrain_manual_gear(
    const OriginalDrivetrainManualGearInputs &inputs);

[[nodiscard]] OriginalDrivetrainEngineStateResult
calculate_original_drivetrain_engine_state(
    const OriginalDrivetrainEngineStateInputs &inputs);

[[nodiscard]] OriginalDrivetrainModeCStateResult
calculate_original_drivetrain_mode_c_state(
    const OriginalDrivetrainModeCStateInputs &inputs);

[[nodiscard]] OriginalDrivetrainModeCForceResult
calculate_original_drivetrain_mode_c_force(
    const OriginalDrivetrainModeCForceInputs &inputs);

// The frame scheduler writes 15.81 every pass, or exactly half that value
// while original global flag bit 0x02 is set.
[[nodiscard]] double
select_original_global_vertical_scale(std::uint8_t global_flags) noexcept;

[[nodiscard]] CollisionVector3
calculate_original_world_gravity_delta(double global_vertical_scale,
                                       double slice_seconds);

[[nodiscard]] OriginalBodyMassProperties
make_original_body_mass_properties(double authored_weight);

// Converts accumulated wheel forces and torques to body-local velocity,
// including the recovered low-speed lateral snap.
void apply_original_body_force_response(
    OriginalBodyVelocityState &state,
    const OriginalBodyForceResponseInputs &inputs);

[[nodiscard]] OriginalVehicleGroundedDampingResult
apply_original_vehicle_grounded_damping(
    OriginalBodyVelocityState &state,
    const OriginalVehicleGroundedDampingProfile &profile, double slice_seconds,
    double linear_base_scale = 1.0);

// Goldbridge material 4 contributes zero to the separate phase-based seed.
// This reproduces the captured velocity-derived Z seed that is copied into
// each of the three response substeps.
[[nodiscard]] CollisionVector3
calculate_original_vehicle_velocity_stabilizer_seed(
    const OriginalVehicleVelocityStabilizerInputs &inputs);

// The original car owner reduces all four surface-derived linear damping bases
// from the live brake scalar and post-substep longitudinal speed. Angular bases
// are not scaled by this path.
[[nodiscard]] double calculate_original_vehicle_grounded_linear_base_scale(
    double longitudinal_velocity, double brake_force,
    double authored_brake_factor, std::uint8_t global_flags);

void apply_original_body_velocity_delta(
    OriginalBodyVelocityState &state,
    const CollisionVector3 &local_linear_delta,
    const CollisionVector3 &local_angular_delta);

void accumulate_body_force_at_point(BodyForceAccumulator &accumulator,
                                    const CollisionVector3 &force,
                                    const CollisionVector3 &application_point,
                                    const CollisionVector3 &center_of_mass);

} // namespace mh::game
