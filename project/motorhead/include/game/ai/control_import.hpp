#pragma once

#include <cstdint>
#include <span>

namespace mh::game {

struct OriginalAiControlOutput {
  float throttle = 0.0F;
  float brake = 0.0F;
  float steering = 0.0F;
};

struct OriginalAiLongitudinalDecisionInput {
  bool recovery_brake = false;
  bool lookahead_curve_available = false;
  bool current_segment_is_straight = false;
  bool special_brake_override = false;
  float current_speed = 0.0F;
  float current_safe_speed = 0.0F;
  float lookahead_safe_speed = 0.0F;
};

struct OriginalAiSafeSpeedInput {
  float signed_turn_radius = 0.0F;
  float curvature_state = 0.0F;
  float curvature_limit = 0.0F;
  float global_speed_scale = 0.0F;
  float vehicle_curve_coefficient = 0.0F;
};

struct OriginalAiCurveFeedForwardInput {
  float current_signed_turn_radius = 0.0F;
  float group_signed_turn_radius = 0.0F;
  float curvature_state = 0.0F;
  float curvature_limit = 0.0F;
  float global_speed_scale = 0.0F;
  float vehicle_curve_coefficient = 0.0F;
  float vehicle_feed_forward_coefficient = 0.0F;
  float current_speed = 0.0F;
};

struct OriginalAiBaseSteeringInput {
  float route_lateral_extent_a = 0.0F;
  float route_lateral_extent_b = 0.0F;
  float target_lateral_offset = 0.0F;
  float measured_lateral_offset = 0.0F;
  float lateral_gain = 0.0F;
  float lateral_rate_correction = 0.0F;
  float curve_feed_forward = 0.0F;
  float direction_alignment = 1.0F;
  float route_side_cross = 0.0F;
  bool collision_avoidance_active = false;
};

struct OriginalAiRouteFrameInput {
  float route_x = 0.0F;
  float route_z = 0.0F;
  float route_tangent_x = 0.0F;
  float route_tangent_z = 0.0F;
  float vehicle_x = 0.0F;
  float vehicle_z = 0.0F;
  float vehicle_forward_x = 0.0F;
  float vehicle_forward_z = 0.0F;
};

struct OriginalAiRouteFrame {
  float measured_lateral_offset = 0.0F;
  float direction_alignment = 0.0F;
  float route_side_cross = 0.0F;
  bool forward_aligned = false;
};

enum class OriginalAiNearbySide {
  none,
  positive,
  negative,
};

struct OriginalAiNearbySideInput {
  int route_sample_delta = 0;
  float route_state = 0.0F;
  float route_limit = 0.0F;
  float lateral_difference = 0.0F;
  float self_half_width = 0.0F;
  float other_half_width = 0.0F;
};

struct OriginalAiAvoidanceTarget {
  float target_lateral_offset = 0.0F;
  bool active = false;
};

struct OriginalAiAvoidanceOpponent {
  bool live = false;
  std::int32_t route_sample = 0;
  float current_speed = 0.0F;
  float half_width = 0.0F;
  float half_length = 0.0F;
  float direction_alignment = 0.0F;
  float target_lateral_offset = 0.0F;
  float measured_lateral_offset = 0.0F;
};

struct OriginalAiAvoidanceChoiceInput {
  std::int32_t route_sample_count = 0;
  std::int32_t self_route_sample = 0;
  float current_speed = 0.0F;
  float half_width = 0.0F;
  float target_lateral_offset = 0.0F;
  float measured_lateral_offset = 0.0F;
  float lateral_rate = 0.0F;
  float route_lateral_extent_a = 0.0F;
  float route_lateral_extent_b = 0.0F;
  float initial_signed_avoidance_offset = 0.0F;
  bool positive_side_occupied = false;
  bool negative_side_occupied = false;
};

struct OriginalAiAvoidanceChoice {
  float signed_avoidance_offset = 0.0F;
  bool force_full_brake = false;
};

struct OriginalAiLateralRateState {
  float previous_lateral_offset = 0.0F;
  float lateral_rate = 0.0F;
};

struct OriginalAiStuckRecoveryInput {
  float measured_lateral_offset = 0.0F;
  float direction_alignment = 0.0F;
  float current_speed = 0.0F;
  float signed_longitudinal_request = 0.0F;
  float time_step_seconds = 0.0F;
};

struct OriginalAiStuckRecoveryState {
  float timer_seconds = 0.0F;
  bool active = false;
};

struct OriginalAiRecoverySteeringInput {
  float route_lateral_extent_a = 0.0F;
  float route_lateral_extent_b = 0.0F;
  float measured_lateral_offset = 0.0F;
  float current_speed = 0.0F;
  float signed_longitudinal_request = 0.0F;
  float direction_alignment = 0.0F;
  float base_steering_request = 0.0F;
  bool forward_aligned = true;
  bool stuck_recovery_active = false;
};

struct OriginalAiRecoverySteeringOutput {
  float steering_request = 0.0F;
  bool force_full_throttle = false;
  bool latch_active = false;
};

struct OriginalAiLookaheadCandidate {
  std::int32_t route_record_offset = 0;
  float signed_turn_radius = 0.0F;
};

struct OriginalAiLookaheadSelectionInput {
  OriginalAiLookaheadCandidate retained_candidate;
  float current_speed = 0.0F;
  float route_spacing_control = 0.0F;
  float curvature_state = 0.0F;
  float curvature_limit = 0.0F;
  float global_speed_scale = 0.0F;
  float vehicle_curve_coefficient = 0.0F;
};

struct OriginalAiLookaheadSelectionOutput {
  OriginalAiLookaheadCandidate selected_candidate;
  std::int32_t braking_sample_delta = 0;
  bool braking_sample_delta_updated = false;
};

struct OriginalAiGroupControl {
  float lateral_gain = 0.0F;
  float lateral_rate_correction_scale = 0.0F;
  float curve_feed_forward_coefficient = 0.0F;
  float safe_speed_curve_coefficient = 1.0F;
  float retained_unit_coefficient = 1.0F;
};

struct OriginalAiRandomState {
  std::uint32_t seed = 1U;
};

struct OriginalAiStochasticControlState {
  float channel_a = 0.0F;
  float channel_b = 0.0F;
  float channel_c = 0.0F;
};

struct OriginalAiControllerState {
  float positive_request_accumulator = 0.0F;
  float target_lateral_offset = 0.0F;
  OriginalAiLateralRateState lateral_rate{};
  OriginalAiStuckRecoveryState stuck_recovery{};
  bool route_width_recovery_latch = false;
  float previous_brake = 0.0F;
};

struct OriginalAiControllerInput {
  float time_step_seconds = 0.0F;
  float current_speed = 0.0F;
  float current_signed_turn_radius = 0.0F;
  float current_group_signed_turn_radius = 0.0F;
  float lookahead_signed_turn_radius = 0.0F;
  float curvature_state = 0.0F;
  float curvature_limit = 0.0F;
  float global_speed_scale = 0.0F;
  float vehicle_curve_coefficient = 0.0F;
  float vehicle_feed_forward_coefficient = 0.0F;
  float route_lateral_extent_a = 0.0F;
  float route_lateral_extent_b = 0.0F;
  float lateral_gain = 0.0F;
  float lateral_rate_correction_scale = 0.0F;
  float measured_lateral_offset = 0.0F;
  float direction_alignment = 1.0F;
  float route_side_cross = 0.0F;
  float half_width = 0.0F;
  // Stable host racing line; zero preserves p3.1 center-line decay.
  float preferred_lateral_offset = 0.0F;
  std::int32_t route_sample_count = 0;
  std::int32_t route_sample = 0;
  bool forward_aligned = true;
  bool lookahead_curve_available = false;
  bool recovery_brake = false;
  bool special_brake_override = false;
  bool reset_positive_request_accumulator = false;
  bool positive_side_occupied = false;
  bool negative_side_occupied = false;
  // Race hosts may seed the recovered selector before physical contact when
  // their separate car-to-car contact owner predicts an imminent overlap.
  // Zero preserves the exact p3.1 controller path.
  float external_signed_avoidance_offset = 0.0F;
  bool external_force_full_brake = false;
};

struct OriginalAiControllerStep {
  OriginalAiControlOutput controls{};
  float signed_longitudinal_request = 0.0F;
  float current_safe_speed = 0.0F;
  float lookahead_safe_speed = 0.0F;
  bool collision_avoidance_active = false;
  bool forced_brake_for_avoidance = false;
};

// Exact X/Z route-frame measurement at p3.1 RVA
// 0x000169cc..0x00016aba. The route tangent is authored as a unit vector.
[[nodiscard]] OriginalAiRouteFrame
original_ai_measure_route_frame(const OriginalAiRouteFrameInput &input);

// Exact first-pass nearby-side classifier at p3.1 RVA
// 0x00017236..0x000172fd. It is applied to each of the seven other live slots.
[[nodiscard]] OriginalAiNearbySide
original_ai_classify_nearby_side(const OriginalAiNearbySideInput &input);

// Exact final avoidance-offset integration at p3.1 RVA
// 0x0001761b..0x00017648. A zero offset decays the target by 20 percent.
[[nodiscard]] OriginalAiAvoidanceTarget
original_ai_apply_avoidance_offset(float target_lateral_offset,
                                   float signed_avoidance_offset);

// Exact seven-other-slot passing/avoidance choice at p3.1 RVA
// 0x00017302..0x0001761b. Opponents must be supplied in the retail slot
// iteration order; a terminal pass-side choice or brake fallback stops the
// scan.
[[nodiscard]] OriginalAiAvoidanceChoice original_ai_choose_avoidance(
    const OriginalAiAvoidanceChoiceInput &input,
    std::span<const OriginalAiAvoidanceOpponent> opponents);

// Exact forward-aligned lateral-rate owner at p3.1 RVA
// 0x0001775f..0x000177e1. Reverse alignment retains both prior values.
[[nodiscard]] OriginalAiLateralRateState
original_ai_update_lateral_rate(float measured_lateral_offset,
                                float time_step_seconds, bool forward_aligned,
                                const OriginalAiLateralRateState &previous);

// Exact per-slot stuck/recovery timer at p3.1 RVA
// 0x00017699..0x0001775f.
[[nodiscard]] OriginalAiStuckRecoveryState
original_ai_update_stuck_recovery(const OriginalAiStuckRecoveryInput &input,
                                  const OriginalAiStuckRecoveryState &previous);

// Exact route-width recovery-steering latch at p3.1 RVA
// 0x000179bf..0x00017b09. The stuck flag's final steering inversion is owned by
// this same boundary.
[[nodiscard]] OriginalAiRecoverySteeringOutput
original_ai_apply_recovery_steering(
    const OriginalAiRecoverySteeringInput &input, bool previous_latch_active);

// Exact curved-record candidate choice at p3.1 RVA
// 0x00016f49..0x000171eb. The caller supplies the forward circular horizon as
// monotonically increasing record-relative offsets after sentinel wrapping.
[[nodiscard]] OriginalAiLookaheadSelectionOutput
original_ai_select_lookahead_candidate(
    const OriginalAiLookaheadSelectionInput &input,
    std::span<const OriginalAiLookaheadCandidate> forward_horizon);

// Exact p3.1 AI initializer calculation at RVA
// 0x00018021..0x00018057. The source is the live-car field at +0x4f3.
[[nodiscard]] float
original_ai_route_spacing_control(float live_car_field_0x4f3);

// Exact five-float group-control record initialized at p3.1 RVA
// 0x00018141..0x0001819a. The first three fields are consumed at
// 0x0001764b..0x0001767f; the fourth is the safe-speed curve coefficient.
[[nodiscard]] OriginalAiGroupControl
original_ai_derive_group_control(float signed_group_turn_radius,
                                 float runtime_group_scale);

// Exact process-local p3.1 random-number update at RVA
// 0x000c3a8c..0x000c3aaf.
[[nodiscard]] std::uint32_t
original_ai_random_next(OriginalAiRandomState &state) noexcept;

// Reproduces the p3.1 process seed source at RVA 0x000bcad0..0x000bcb0b:
// truncated QueryPerformanceCounter/QueryPerformanceFrequency milliseconds.
// On Windows steady_clock uses the same monotonic performance-counter domain.
[[nodiscard]] std::uint32_t original_ai_runtime_time_seed() noexcept;

// Exact initializer half-scale at p3.1 RVA 0x00018098..0x000180c7.
[[nodiscard]] OriginalAiStochasticControlState
original_ai_initial_stochastic_control(float base_a, float base_b,
                                       float base_c);

// Exact three-channel AI random walk at p3.1 RVA
// 0x00016ad5..0x00016bca. One shared process RNG supplies three consecutive
// values per active AI update.
void original_ai_update_stochastic_control(
    OriginalAiStochasticControlState &control,
    OriginalAiRandomState &random) noexcept;

// Exact finite-input safe-speed producer shared by the current and lookahead
// route records at p3.1 RVA 0x00016bfd..0x00016c64 and
// 0x00016cac..0x00016d13. A zero signed radius uses the retail straight-line
// sentinel speed of 100.
[[nodiscard]] float
original_ai_safe_speed(const OriginalAiSafeSpeedInput &input);

// Exact curve steering feed-forward producer at p3.1 RVA
// 0x000177e8..0x000178cc. Straight current samples return zero.
[[nodiscard]] float
original_ai_curve_feed_forward(const OriginalAiCurveFeedForwardInput &input);

// Exact speed-normalized lateral-rate correction at p3.1 RVA
// 0x000178cc..0x00017907. At low speed, positive-side offsets beyond five
// units suppress the correction; a zero speed produces no correction.
[[nodiscard]] float
original_ai_lateral_rate_correction(float measured_lateral_offset,
                                    float current_speed, float lateral_rate,
                                    float lateral_rate_correction_scale);

// Exact finite-input branch table at p3.1 RVA 0x00016c66..0x00016e04.
// The condition that makes a lookahead curve available remains a separate
// recovered boundary.
[[nodiscard]] float original_ai_select_longitudinal_request(
    const OriginalAiLongitudinalDecisionInput &input);

// Exact positive-request accumulator at p3.1 RVA
// 0x00016e12..0x00016e84. The accumulator is one per live vehicle slot.
// Negative and zero requests bypass this stage.
[[nodiscard]] float original_ai_apply_positive_request_slew(
    float signed_longitudinal_request, float time_step_seconds,
    bool reset_accumulator, float &accumulator);

// Exact base steering equation and direction normalization at p3.1 RVA
// 0x0001790b..0x000179bf. Recovery-state steering after this boundary is
// intentionally separate.
[[nodiscard]] float
original_ai_base_steering_request(const OriginalAiBaseSteeringInput &input);

// Exact final control handoff from p3.1 AI update RVA
// 0x00016e04..0x00016ebc and 0x00017b09..0x00017b11. This function does
// not invent the upstream request/steering equations.
[[nodiscard]] OriginalAiControlOutput
original_ai_control_handoff(float signed_longitudinal_request,
                            float previous_brake,
                            float signed_steering_request);

// Stateful composition of the recovered p3.1 post-progression controller
// boundaries. Route-cursor and lookahead-candidate selection remain explicit
// upstream owners; this boundary preserves the original update ordering from
// longitudinal request through final live controls.
[[nodiscard]] OriginalAiControllerStep original_ai_controller_step(
    const OriginalAiControllerInput &input,
    std::span<const OriginalAiAvoidanceOpponent> opponents,
    OriginalAiControllerState &state);

} // namespace mh::game
