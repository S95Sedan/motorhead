#include <game/ai/control_import.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mh::game {

namespace {

std::int32_t original_ai_truncate_i32(const long double value,
                                      const char *overflow_message) {
  const auto truncated = std::trunc(value);
  if (truncated <
          static_cast<long double>(std::numeric_limits<std::int32_t>::min()) ||
      truncated >
          static_cast<long double>(std::numeric_limits<std::int32_t>::max())) {
    throw std::overflow_error(overflow_message);
  }
  return static_cast<std::int32_t>(truncated);
}

float original_ai_safe_speed_squared(const OriginalAiSafeSpeedInput &input) {
  auto radius = input.signed_turn_radius;
  if (1.0F - input.curvature_state < input.curvature_limit) {
    radius = static_cast<float>(static_cast<double>(radius) *
                                static_cast<double>(1.5625F));
  }

  constexpr auto gravity_like_scale = 9.82;
  const auto coefficient =
      static_cast<long double>(input.vehicle_curve_coefficient);
  return static_cast<float>(std::fabs(static_cast<long double>(radius)) *
                            static_cast<long double>(gravity_like_scale) *
                            static_cast<long double>(input.global_speed_scale) *
                            coefficient * coefficient);
}

std::int32_t
original_ai_truncated_record_delta(const float safe_speed_squared,
                                   const float current_speed_squared,
                                   const float route_spacing_control) {
  return original_ai_truncate_i32(
      (static_cast<long double>(safe_speed_squared) -
       static_cast<long double>(current_speed_squared)) /
          static_cast<long double>(route_spacing_control) * 0.5L,
      "original AI lookahead record displacement overflow");
}

} // namespace

float original_ai_route_spacing_control(const float live_car_field_0x4f3) {
  if (!std::isfinite(live_car_field_0x4f3) || live_car_field_0x4f3 == 0.0F) {
    throw std::invalid_argument(
        "original AI route spacing requires a finite nonzero car field");
  }
  return -28000.0F / live_car_field_0x4f3;
}

OriginalAiGroupControl
original_ai_derive_group_control(const float signed_group_turn_radius,
                                 const float runtime_group_scale) {
  if (!std::isfinite(signed_group_turn_radius) ||
      !std::isfinite(runtime_group_scale)) {
    throw std::invalid_argument(
        "original AI group control requires finite inputs");
  }

  // The retail initializer keeps 0.0015 as an f64 x87 operand and rounds each
  // destination independently to f32.
  constexpr auto radius_scale = 0.001499999954223634L;
  const auto radius_factor =
      1.0L - std::fabs(static_cast<long double>(signed_group_turn_radius)) *
                 radius_scale;
  if (radius_factor == 0.0L) {
    throw std::invalid_argument(
        "original AI group control radius produced a singular record");
  }

  OriginalAiGroupControl result;
  result.lateral_gain = static_cast<float>(radius_factor * 0.5L);
  result.lateral_rate_correction_scale =
      static_cast<float>(3.9L / radius_factor);
  result.curve_feed_forward_coefficient = static_cast<float>(
      radius_factor * static_cast<long double>(runtime_group_scale));
  return result;
}

std::uint32_t original_ai_random_next(OriginalAiRandomState &state) noexcept {
  state.seed = state.seed * 0x41c64e6dU + 0x00003039U;
  return (state.seed >> 16U) & 0x00007fffU;
}

std::uint32_t original_ai_runtime_time_seed() noexcept {
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  return static_cast<std::uint32_t>(milliseconds);
}

OriginalAiStochasticControlState
original_ai_initial_stochastic_control(const float base_a, const float base_b,
                                       const float base_c) {
  if (!std::isfinite(base_a) || !std::isfinite(base_b) ||
      !std::isfinite(base_c)) {
    throw std::invalid_argument(
        "original AI stochastic initialization requires finite inputs");
  }
  return {base_a * 0.5F, base_b * 0.5F, base_c * 0.5F};
}

void original_ai_update_stochastic_control(
    OriginalAiStochasticControlState &control,
    OriginalAiRandomState &random) noexcept {
  const auto walk = [&random](float &value, const float negative_step,
                              const float positive_step) {
    const auto sample = original_ai_random_next(random);
    if (sample >= 0x3fffU) {
      if (value < 1.0F) {
        value += positive_step;
      }
    } else if (value > 0.0F) {
      value += negative_step;
    }
  };
  walk(control.channel_a, -0.01F, 0.01F);
  walk(control.channel_b, -0.05F, 0.05F);
  walk(control.channel_c, -0.01F, 0.01F);
}

OriginalAiRouteFrame
original_ai_measure_route_frame(const OriginalAiRouteFrameInput &input) {
  const auto values = {input.route_x,           input.route_z,
                       input.route_tangent_x,   input.route_tangent_z,
                       input.vehicle_x,         input.vehicle_z,
                       input.vehicle_forward_x, input.vehicle_forward_z};
  if (!std::all_of(values.begin(), values.end(),
                   [](const float value) { return std::isfinite(value); })) {
    throw std::invalid_argument(
        "original AI route frame requires finite inputs");
  }

  OriginalAiRouteFrame result;
  const auto forward_x = static_cast<long double>(input.vehicle_forward_x);
  const auto forward_z = static_cast<long double>(input.vehicle_forward_z);
  const auto tangent_x = static_cast<long double>(input.route_tangent_x);
  const auto tangent_z = static_cast<long double>(input.route_tangent_z);
  result.direction_alignment =
      static_cast<float>(forward_x * tangent_x + forward_z * tangent_z);
  result.forward_aligned = result.direction_alignment >= 0.0F;
  result.route_side_cross =
      static_cast<float>(tangent_x * forward_z - tangent_z * forward_x);

  // p3.1 keeps the position deltas, distance, and projection in x87 extended
  // precision, but spills the distance and projection independently to f32
  // before the lateral-magnitude square/subtract/square-root sequence.
  const auto delta_x = static_cast<long double>(input.vehicle_x) -
                       static_cast<long double>(input.route_x);
  const auto delta_z = static_cast<long double>(input.vehicle_z) -
                       static_cast<long double>(input.route_z);
  const auto distance = std::sqrt(delta_x * delta_x + delta_z * delta_z);
  const auto projection = delta_x * tangent_x + delta_z * tangent_z;
  const auto absolute_projection = std::fabs(projection);
  const auto stored_distance = static_cast<float>(distance);
  const auto stored_projection = static_cast<float>(projection);

  auto lateral_magnitude = 0.0F;
  if (distance >= absolute_projection) {
    if ((std::bit_cast<unsigned int>(stored_projection) & 0x7fffffffU) != 0U) {
      const auto spilled_distance = static_cast<long double>(stored_distance);
      const auto spilled_projection =
          static_cast<long double>(stored_projection);
      lateral_magnitude = static_cast<float>(
          std::sqrt(spilled_distance * spilled_distance -
                    spilled_projection * spilled_projection));
    } else {
      lateral_magnitude = stored_distance;
    }
  }
  const auto position_side = delta_z * tangent_x - delta_x * tangent_z;
  result.measured_lateral_offset =
      position_side > 0.0F ? -lateral_magnitude : lateral_magnitude;
  return result;
}

OriginalAiNearbySide
original_ai_classify_nearby_side(const OriginalAiNearbySideInput &input) {
  const auto values = {input.route_state, input.route_limit,
                       input.lateral_difference, input.self_half_width,
                       input.other_half_width};
  if (!std::all_of(values.begin(), values.end(),
                   [](const float value) { return std::isfinite(value); })) {
    throw std::invalid_argument(
        "original AI nearby-side classification requires finite inputs");
  }
  if (std::abs(input.route_sample_delta) >= 5 ||
      1.0F - input.route_state <= input.route_limit) {
    return OriginalAiNearbySide::none;
  }

  const auto clearance = (static_cast<long double>(input.self_half_width) +
                          static_cast<long double>(input.other_half_width)) *
                             static_cast<long double>(0.5F) +
                         static_cast<long double>(0.1F) +
                         static_cast<long double>(0.2F);
  if (input.lateral_difference > 0.0F && input.lateral_difference < clearance) {
    return OriginalAiNearbySide::positive;
  }
  if (input.lateral_difference < 0.0F &&
      -input.lateral_difference < clearance) {
    return OriginalAiNearbySide::negative;
  }
  return OriginalAiNearbySide::none;
}

OriginalAiAvoidanceTarget
original_ai_apply_avoidance_offset(const float target_lateral_offset,
                                   const float signed_avoidance_offset) {
  if (!std::isfinite(target_lateral_offset) ||
      !std::isfinite(signed_avoidance_offset)) {
    throw std::invalid_argument(
        "original AI avoidance integration requires finite inputs");
  }
  if ((std::bit_cast<unsigned int>(signed_avoidance_offset) & 0x7fffffffU) !=
      0U) {
    return {target_lateral_offset + signed_avoidance_offset, true};
  }
  return {target_lateral_offset - target_lateral_offset * 0.2F, false};
}

OriginalAiAvoidanceChoice original_ai_choose_avoidance(
    const OriginalAiAvoidanceChoiceInput &input,
    const std::span<const OriginalAiAvoidanceOpponent> opponents) {
  const auto self_values = {
      input.current_speed,          input.half_width,
      input.target_lateral_offset,  input.measured_lateral_offset,
      input.lateral_rate,           input.route_lateral_extent_a,
      input.route_lateral_extent_b, input.initial_signed_avoidance_offset};
  if (input.route_sample_count <= 0 || input.self_route_sample < 0 ||
      input.self_route_sample >= input.route_sample_count ||
      opponents.size() > 7U ||
      !std::all_of(self_values.begin(), self_values.end(),
                   [](const float value) { return std::isfinite(value); })) {
    throw std::invalid_argument(
        "original AI avoidance choice requires a bounded finite route state");
  }

  auto result =
      OriginalAiAvoidanceChoice{input.initial_signed_avoidance_offset, false};
  auto forward_candidate_seen = false;
  constexpr auto lateral_step = 0.2F;

  for (const auto &opponent : opponents) {
    if (!opponent.live) {
      continue;
    }
    const auto opponent_values = {
        opponent.current_speed,         opponent.half_width,
        opponent.half_length,           opponent.direction_alignment,
        opponent.target_lateral_offset, opponent.measured_lateral_offset};
    if (opponent.route_sample < 0 ||
        opponent.route_sample >= input.route_sample_count ||
        !std::all_of(
            opponent_values.begin(), opponent_values.end(),
            [](const float value) { return std::isfinite(value); })) {
      throw std::invalid_argument(
          "original AI avoidance opponent requires a bounded finite state");
    }

    // The x87 expression is stored to binary32 before either clearance test.
    const auto projected_opponent_extent = static_cast<float>(
        (1.0L - static_cast<long double>(opponent.direction_alignment)) *
            static_cast<long double>(opponent.half_length) +
        static_cast<long double>(opponent.half_width) *
            static_cast<long double>(opponent.direction_alignment));
    const auto predicted_sample_delta = original_ai_truncate_i32(
        (static_cast<long double>(input.current_speed) -
         static_cast<long double>(opponent.current_speed)) *
            5.0L,
        "original AI avoidance prediction overflow");
    const auto route_clearance = static_cast<float>(
        static_cast<long double>(projected_opponent_extent) * 0.5L +
        static_cast<long double>(input.half_width) + 0.1L + 0.1L);

    if (predicted_sample_delta < 0) {
      continue;
    }

    const auto route_delta = opponent.route_sample - input.self_route_sample;
    const auto forward_without_wrap =
        route_delta < predicted_sample_delta &&
        input.self_route_sample < opponent.route_sample;
    const auto forward_across_wrap =
        route_delta < predicted_sample_delta - input.route_sample_count &&
        input.self_route_sample > opponent.route_sample;
    const auto is_forward_candidate =
        forward_without_wrap || forward_across_wrap;

    if (is_forward_candidate) {
      forward_candidate_seen = true;
      if (!(static_cast<long double>(opponent.current_speed) * 1.01L <
            static_cast<long double>(input.current_speed))) {
        continue;
      }

      const auto lateral_overlap = static_cast<long double>(std::fabs(
          opponent.measured_lateral_offset - input.measured_lateral_offset));
      const auto overlap_limit =
          (static_cast<long double>(input.half_width) +
           static_cast<long double>(projected_opponent_extent)) *
              0.5L +
          0.1L;
      if (!(lateral_overlap < overlap_limit)) {
        continue;
      }

      const auto positive_space_available = [&]() {
        return static_cast<long double>(input.route_lateral_extent_b) -
                       static_cast<long double>(
                           opponent.measured_lateral_offset) >
                   static_cast<long double>(route_clearance) &&
               static_cast<long double>(input.route_lateral_extent_b) -
                       static_cast<long double>(
                           opponent.target_lateral_offset) -
                       static_cast<long double>(
                           result.signed_avoidance_offset) >
                   static_cast<long double>(route_clearance) &&
               !input.negative_side_occupied;
      };
      const auto negative_space_available = [&]() {
        return static_cast<long double>(input.route_lateral_extent_a) +
                       static_cast<long double>(
                           opponent.measured_lateral_offset) >
                   static_cast<long double>(route_clearance) &&
               static_cast<long double>(input.route_lateral_extent_a) +
                       static_cast<long double>(
                           opponent.target_lateral_offset) +
                       static_cast<long double>(
                           result.signed_avoidance_offset) >
                   static_cast<long double>(route_clearance) &&
               !input.positive_side_occupied;
      };
      const auto choose_positive = [&]() {
        result.signed_avoidance_offset = static_cast<float>(
            static_cast<long double>(result.signed_avoidance_offset) +
            static_cast<long double>(lateral_step));
      };
      const auto choose_negative = [&]() {
        result.signed_avoidance_offset = static_cast<float>(
            static_cast<long double>(result.signed_avoidance_offset) -
            static_cast<long double>(lateral_step));
      };

      if (opponent.measured_lateral_offset < input.measured_lateral_offset) {
        if (positive_space_available()) {
          choose_positive();
          return result;
        }
        if (negative_space_available()) {
          choose_negative();
          return result;
        }
      } else if (opponent.measured_lateral_offset >
                 input.measured_lateral_offset) {
        if (negative_space_available()) {
          choose_negative();
          return result;
        }
        if (positive_space_available()) {
          choose_positive();
          return result;
        }
      } else {
        return result;
      }

      result.force_full_brake = true;
      return result;
    }

    // The retail same-record branch is disabled after any forward candidate
    // was observed. It uses self lateral rate and can accumulate through the
    // remaining opponent slots instead of terminating the scan.
    if (opponent.route_sample != input.self_route_sample ||
        forward_candidate_seen) {
      continue;
    }
    if (opponent.measured_lateral_offset < input.measured_lateral_offset &&
        input.lateral_rate < 0.0F &&
        static_cast<long double>(input.route_lateral_extent_b) -
                static_cast<long double>(input.target_lateral_offset) -
                static_cast<long double>(result.signed_avoidance_offset) >
            static_cast<long double>(route_clearance)) {
      result.signed_avoidance_offset = static_cast<float>(
          static_cast<long double>(result.signed_avoidance_offset) +
          static_cast<long double>(lateral_step));
    } else if (opponent.measured_lateral_offset >
                   input.measured_lateral_offset &&
               input.lateral_rate > 0.0F &&
               static_cast<long double>(input.route_lateral_extent_a) +
                       static_cast<long double>(input.target_lateral_offset) +
                       static_cast<long double>(
                           result.signed_avoidance_offset) >
                   static_cast<long double>(route_clearance)) {
      result.signed_avoidance_offset = static_cast<float>(
          static_cast<long double>(result.signed_avoidance_offset) -
          static_cast<long double>(lateral_step));
    }
  }
  return result;
}

OriginalAiLateralRateState original_ai_update_lateral_rate(
    const float measured_lateral_offset, const float time_step_seconds,
    const bool forward_aligned, const OriginalAiLateralRateState &previous) {
  if (!std::isfinite(measured_lateral_offset) ||
      !std::isfinite(time_step_seconds) ||
      !std::isfinite(previous.previous_lateral_offset) ||
      !std::isfinite(previous.lateral_rate) || time_step_seconds <= 0.0F) {
    throw std::invalid_argument(
        "original AI lateral rate requires finite positive-time inputs");
  }
  if (!forward_aligned) {
    return previous;
  }

  return {
      measured_lateral_offset,
      static_cast<float>(
          (static_cast<double>(measured_lateral_offset) -
           static_cast<double>(previous.previous_lateral_offset)) /
          static_cast<double>(time_step_seconds)),
  };
}

OriginalAiStuckRecoveryState original_ai_update_stuck_recovery(
    const OriginalAiStuckRecoveryInput &input,
    const OriginalAiStuckRecoveryState &previous) {
  const auto values = {
      input.measured_lateral_offset, input.direction_alignment,
      input.current_speed,           input.signed_longitudinal_request,
      input.time_step_seconds,       previous.timer_seconds};
  if (!std::all_of(values.begin(), values.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      input.time_step_seconds < 0.0F) {
    throw std::invalid_argument(
        "original AI stuck recovery requires finite nonnegative-time inputs");
  }

  auto result = previous;
  if (result.active) {
    if (result.timer_seconds > 0.0F) {
      result.timer_seconds =
          static_cast<float>(static_cast<double>(result.timer_seconds) -
                             static_cast<double>(input.time_step_seconds));
    } else {
      result.active = false;
    }

    if (std::fabs(input.measured_lateral_offset) < 2.0F &&
        input.direction_alignment > 0.8F) {
      result.active = false;
      result.timer_seconds = 0.0F;
    }
    return result;
  }

  constexpr auto stuck_speed_threshold = 1.388888888888889;
  if (std::fabs(static_cast<double>(input.current_speed)) <
          stuck_speed_threshold &&
      input.signed_longitudinal_request > 0.0F) {
    result.timer_seconds =
        static_cast<float>(static_cast<double>(result.timer_seconds) +
                           static_cast<double>(input.time_step_seconds));
    if (result.timer_seconds > 2.0F) {
      result.timer_seconds = 2.0F;
      result.active = true;
    }
  }
  return result;
}

OriginalAiRecoverySteeringOutput original_ai_apply_recovery_steering(
    const OriginalAiRecoverySteeringInput &input,
    const bool previous_latch_active) {
  const auto values = {
      input.route_lateral_extent_a,      input.route_lateral_extent_b,
      input.measured_lateral_offset,     input.current_speed,
      input.signed_longitudinal_request, input.direction_alignment,
      input.base_steering_request};
  if (!std::all_of(values.begin(), values.end(),
                   [](const float value) { return std::isfinite(value); })) {
    throw std::invalid_argument(
        "original AI recovery steering requires finite inputs");
  }

  auto result = OriginalAiRecoverySteeringOutput{input.base_steering_request,
                                                 false, previous_latch_active};

  // Retail enters the latch below 50 km/h only after crossing twice either
  // authored route extent. Equality remains outside the latch.
  if (!result.latch_active &&
      static_cast<double>(input.current_speed) < 13.888888888888889 &&
      (static_cast<double>(input.measured_lateral_offset) <
           -2.0 * static_cast<double>(input.route_lateral_extent_a) ||
       static_cast<double>(input.measured_lateral_offset) >
           2.0 * static_cast<double>(input.route_lateral_extent_b))) {
    result.latch_active = true;
  }

  if (result.latch_active) {
    const auto alignment = static_cast<double>(input.direction_alignment);
    if (alignment > 0.5 || alignment < -0.5) {
      result.force_full_throttle = input.signed_longitudinal_request > 0.0F;
      result.steering_request =
          input.measured_lateral_offset < 0.0F ? 1.0F : -1.0F;
      if (!input.forward_aligned) {
        result.steering_request = -result.steering_request;
      }
    } else if (!input.stuck_recovery_active) {
      result.steering_request = 0.0F;
    }

    // The original uses two sign-specific strict comparisons. In particular,
    // exactly zero does not clear an already-active latch on this update.
    if ((input.measured_lateral_offset < 0.0F &&
         -input.measured_lateral_offset < input.route_lateral_extent_a) ||
        (input.measured_lateral_offset > 0.0F &&
         input.measured_lateral_offset < input.route_lateral_extent_b)) {
      result.latch_active = false;
    }
  }

  if (input.stuck_recovery_active) {
    result.steering_request = -result.steering_request;
  }
  return result;
}

OriginalAiLookaheadSelectionOutput original_ai_select_lookahead_candidate(
    const OriginalAiLookaheadSelectionInput &input,
    const std::span<const OriginalAiLookaheadCandidate> forward_horizon) {
  const auto values = {input.retained_candidate.signed_turn_radius,
                       input.current_speed,
                       input.route_spacing_control,
                       input.curvature_state,
                       input.curvature_limit,
                       input.global_speed_scale,
                       input.vehicle_curve_coefficient};
  if (!std::all_of(values.begin(), values.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      input.route_spacing_control == 0.0F || input.global_speed_scale < 0.0F) {
    throw std::invalid_argument(
        "original AI lookahead selection requires finite nonzero-spacing "
        "inputs");
  }
  for (const auto &candidate : forward_horizon) {
    if (!std::isfinite(candidate.signed_turn_radius)) {
      throw std::invalid_argument(
          "original AI lookahead candidate radius must be finite");
    }
  }

  auto output =
      OriginalAiLookaheadSelectionOutput{input.retained_candidate, 0, false};
  const auto speed_squared =
      static_cast<float>(static_cast<long double>(input.current_speed) *
                         static_cast<long double>(input.current_speed));
  const auto safe_speed_squared_for = [&input](const float signed_turn_radius) {
    OriginalAiSafeSpeedInput safe;
    safe.signed_turn_radius = signed_turn_radius;
    safe.curvature_state = input.curvature_state;
    safe.curvature_limit = input.curvature_limit;
    safe.global_speed_scale = input.global_speed_scale;
    safe.vehicle_curve_coefficient = input.vehicle_curve_coefficient;
    return original_ai_safe_speed_squared(safe);
  };

  for (const auto &candidate : forward_horizon) {
    if ((std::bit_cast<unsigned int>(candidate.signed_turn_radius) &
         0x7fffffffU) == 0U) {
      continue;
    }
    if ((std::bit_cast<unsigned int>(
             output.selected_candidate.signed_turn_radius) &
         0x7fffffffU) == 0U) {
      output.selected_candidate = candidate;
      continue;
    }

    const auto retained_delta = original_ai_truncated_record_delta(
        safe_speed_squared_for(output.selected_candidate.signed_turn_radius),
        speed_squared, input.route_spacing_control);
    const auto candidate_delta = original_ai_truncated_record_delta(
        safe_speed_squared_for(candidate.signed_turn_radius), speed_squared,
        input.route_spacing_control);
    const auto retained_braking_record =
        static_cast<std::int64_t>(
            output.selected_candidate.route_record_offset) -
        static_cast<std::int64_t>(retained_delta);
    const auto candidate_braking_record =
        static_cast<std::int64_t>(candidate.route_record_offset) -
        static_cast<std::int64_t>(candidate_delta);

    // RVA 0x170ce..0x170d6 replaces on unsigned-address <=. Relative
    // offsets are nonnegative after the caller resolves circular wrapping.
    if (candidate_braking_record <= retained_braking_record) {
      output.selected_candidate = candidate;
    }
  }

  if ((std::bit_cast<unsigned int>(
           output.selected_candidate.signed_turn_radius) &
       0x7fffffffU) != 0U) {
    const auto safe_speed_squared =
        safe_speed_squared_for(output.selected_candidate.signed_turn_radius);
    if (safe_speed_squared < speed_squared) {
      output.braking_sample_delta = original_ai_truncated_record_delta(
          safe_speed_squared, speed_squared, input.route_spacing_control);
      output.braking_sample_delta_updated = true;
    }
  }
  return output;
}

float original_ai_safe_speed(const OriginalAiSafeSpeedInput &input) {
  if (!std::isfinite(input.signed_turn_radius) ||
      !std::isfinite(input.curvature_state) ||
      !std::isfinite(input.curvature_limit) ||
      !std::isfinite(input.global_speed_scale) ||
      !std::isfinite(input.vehicle_curve_coefficient) ||
      input.global_speed_scale < 0.0F) {
    throw std::invalid_argument(
        "original AI safe speed requires finite nonnegative-scale inputs");
  }

  // Retail tests the radius with the sign bit masked, so both signed zero
  // encodings select the straight-line sentinel.
  if ((std::bit_cast<unsigned int>(input.signed_turn_radius) & 0x7fffffffU) ==
      0U) {
    return 100.0F;
  }

  return std::sqrt(original_ai_safe_speed_squared(input));
}

float original_ai_curve_feed_forward(
    const OriginalAiCurveFeedForwardInput &input) {
  const auto values = {input.current_signed_turn_radius,
                       input.group_signed_turn_radius,
                       input.curvature_state,
                       input.curvature_limit,
                       input.global_speed_scale,
                       input.vehicle_curve_coefficient,
                       input.vehicle_feed_forward_coefficient,
                       input.current_speed};
  if (!std::all_of(values.begin(), values.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      input.global_speed_scale < 0.0F) {
    throw std::invalid_argument(
        "original AI curve feed-forward requires finite nonnegative-scale "
        "inputs");
  }
  if ((std::bit_cast<unsigned int>(input.current_signed_turn_radius) &
       0x7fffffffU) == 0U) {
    return 0.0F;
  }

  const auto effective_radius = static_cast<float>(
      (static_cast<double>(input.group_signed_turn_radius) +
       static_cast<double>(input.current_signed_turn_radius)) *
      static_cast<double>(0.5F));
  OriginalAiSafeSpeedInput safe_speed_input;
  safe_speed_input.signed_turn_radius = effective_radius;
  safe_speed_input.curvature_state = input.curvature_state;
  safe_speed_input.curvature_limit = input.curvature_limit;
  safe_speed_input.global_speed_scale = input.global_speed_scale;
  safe_speed_input.vehicle_curve_coefficient = input.vehicle_curve_coefficient;
  const auto safe_speed_squared =
      original_ai_safe_speed_squared(safe_speed_input);
  if (safe_speed_squared <= 0.0F) {
    throw std::invalid_argument(
        "original AI curve feed-forward requires positive safe speed");
  }

  const auto safe_speed =
      std::sqrt(static_cast<long double>(safe_speed_squared));
  auto request = (static_cast<long double>(input.current_speed) *
                  static_cast<long double>(input.current_speed) /
                  static_cast<long double>(safe_speed_squared)) *
                 std::atan(static_cast<long double>(
                               input.vehicle_feed_forward_coefficient) /
                           safe_speed);
  if (request > static_cast<long double>(input.group_signed_turn_radius)) {
    request = -request;
  }
  return static_cast<float>(request);
}

float original_ai_select_longitudinal_request(
    const OriginalAiLongitudinalDecisionInput &input) {
  if (!std::isfinite(input.current_speed) ||
      !std::isfinite(input.current_safe_speed) ||
      !std::isfinite(input.lookahead_safe_speed)) {
    throw std::invalid_argument(
        "original AI longitudinal decision requires finite inputs");
  }

  constexpr auto hard_brake_multiplier = 1.52F;
  constexpr auto full_throttle_multiplier = 0.95F;
  const auto speed = static_cast<double>(input.current_speed);
  const auto lookahead_safe_speed =
      static_cast<double>(input.lookahead_safe_speed);
  auto request = 0.0F;
  if (input.recovery_brake) {
    request = -0.5F;
  } else if (!input.lookahead_curve_available) {
    if (input.current_segment_is_straight ||
        input.current_speed < input.current_safe_speed) {
      request = 1.0F;
    }
  } else if (lookahead_safe_speed * static_cast<double>(hard_brake_multiplier) <
             speed) {
    request = -0.89F;
  } else if (input.current_segment_is_straight ||
             input.current_speed < input.current_safe_speed) {
    if (lookahead_safe_speed * static_cast<double>(full_throttle_multiplier) >
        speed) {
      request = 1.0F;
    } else if (input.current_speed <= input.lookahead_safe_speed &&
               (input.current_segment_is_straight ||
                input.current_speed < input.current_safe_speed) &&
               input.current_speed < input.lookahead_safe_speed) {
      request = 0.5F;
    }
  } else if (input.current_speed <= input.lookahead_safe_speed &&
             input.current_speed < input.lookahead_safe_speed) {
    request = 0.5F;
  }

  // RVA 0x00016dc7..0x00016dfd is evaluated after every decision path,
  // including recovery, and replaces the request when its track/runtime
  // condition is active.
  return input.special_brake_override ? -0.89F : request;
}

float original_ai_apply_positive_request_slew(
    const float signed_longitudinal_request, const float time_step_seconds,
    const bool reset_accumulator, float &accumulator) {
  if (!std::isfinite(signed_longitudinal_request) ||
      !std::isfinite(time_step_seconds) || time_step_seconds < 0.0F ||
      !std::isfinite(accumulator)) {
    throw std::invalid_argument(
        "original AI positive slew requires finite nonnegative-time inputs");
  }
  if (signed_longitudinal_request <= 0.0F) {
    return signed_longitudinal_request;
  }
  if (reset_accumulator) {
    accumulator = 0.5F;
    return 0.0F;
  }

  if (accumulator < 1.0F) {
    constexpr auto slew_per_second = 0.25;
    accumulator = static_cast<float>(static_cast<double>(accumulator) +
                                     static_cast<double>(time_step_seconds) *
                                         slew_per_second);
  }
  return accumulator < signed_longitudinal_request
             ? accumulator
             : signed_longitudinal_request;
}

float original_ai_base_steering_request(
    const OriginalAiBaseSteeringInput &input) {
  const auto values = {
      input.route_lateral_extent_a, input.route_lateral_extent_b,
      input.target_lateral_offset,  input.measured_lateral_offset,
      input.lateral_gain,           input.lateral_rate_correction,
      input.curve_feed_forward,     input.direction_alignment,
      input.route_side_cross};
  if (!std::all_of(values.begin(), values.end(),
                   [](const float value) { return std::isfinite(value); })) {
    throw std::invalid_argument(
        "original AI base steering requires finite inputs");
  }

  const auto lateral_width =
      input.route_lateral_extent_a + input.route_lateral_extent_b;
  if (lateral_width == 0.0F) {
    throw std::invalid_argument(
        "original AI base steering requires nonzero route width");
  }

  auto gain = input.lateral_gain;
  const auto absolute_alignment = std::fabs(input.direction_alignment);
  if (absolute_alignment < 0.9F) {
    gain = static_cast<float>((2.0 - static_cast<double>(absolute_alignment)) *
                              static_cast<double>(gain));
  }

  auto request =
      static_cast<float>(((static_cast<double>(input.target_lateral_offset) -
                           static_cast<double>(input.measured_lateral_offset)) *
                          static_cast<double>(gain)) /
                             static_cast<double>(lateral_width) -
                         static_cast<double>(input.lateral_rate_correction) +
                         static_cast<double>(input.curve_feed_forward));
  if (input.collision_avoidance_active) {
    request = static_cast<float>(static_cast<double>(request) * 2.0);
  }

  if (absolute_alignment < 0.5F) {
    return input.route_side_cross > 0.0F ? 1.0F : -1.0F;
  }
  return input.direction_alignment < 0.0F ? -request : request;
}

float original_ai_lateral_rate_correction(
    const float measured_lateral_offset, const float current_speed,
    const float lateral_rate, const float lateral_rate_correction_scale) {
  const std::array values{measured_lateral_offset, current_speed, lateral_rate,
                          lateral_rate_correction_scale};
  if (!std::all_of(values.begin(), values.end(),
                   [](const float value) { return std::isfinite(value); })) {
    throw std::invalid_argument(
        "original AI lateral-rate correction requires finite inputs");
  }

  constexpr auto recovery_speed_threshold = 13.888888888888889;
  if (std::fabs(static_cast<double>(current_speed)) <
          recovery_speed_threshold &&
      measured_lateral_offset > 5.0F) {
    return 0.0F;
  }
  if ((std::bit_cast<std::uint32_t>(current_speed) & 0x7fffffffU) == 0U) {
    return 0.0F;
  }
  return static_cast<float>(static_cast<double>(lateral_rate_correction_scale) *
                            static_cast<double>(lateral_rate) /
                            static_cast<double>(current_speed));
}

OriginalAiControlOutput
original_ai_control_handoff(const float signed_longitudinal_request,
                            const float previous_brake,
                            const float signed_steering_request) {
  if (!std::isfinite(signed_longitudinal_request) ||
      !std::isfinite(previous_brake) ||
      !std::isfinite(signed_steering_request)) {
    throw std::invalid_argument(
        "original AI control handoff requires finite inputs");
  }

  OriginalAiControlOutput output;
  if (signed_longitudinal_request > 0.0F) {
    output.throttle = signed_longitudinal_request;
  } else if (signed_longitudinal_request < 0.0F) {
    output.brake = -signed_longitudinal_request;
  } else {
    // The retail zero branch copies the prior brake slot to throttle before
    // clearing brake. Preserve this unusual transition rather than replacing
    // it with an intuitive all-zero result.
    output.throttle = previous_brake;
  }
  output.steering = signed_steering_request;
  return output;
}

OriginalAiControllerStep original_ai_controller_step(
    const OriginalAiControllerInput &input,
    const std::span<const OriginalAiAvoidanceOpponent> opponents,
    OriginalAiControllerState &state) {
  const auto finite_inputs = {input.time_step_seconds,
                              input.current_speed,
                              input.current_signed_turn_radius,
                              input.current_group_signed_turn_radius,
                              input.lookahead_signed_turn_radius,
                              input.curvature_state,
                              input.curvature_limit,
                              input.global_speed_scale,
                              input.vehicle_curve_coefficient,
                              input.vehicle_feed_forward_coefficient,
                              input.route_lateral_extent_a,
                              input.route_lateral_extent_b,
                              input.lateral_gain,
                              input.lateral_rate_correction_scale,
                              input.measured_lateral_offset,
                              input.direction_alignment,
                              input.route_side_cross,
                              input.half_width,
                              input.preferred_lateral_offset,
                              input.external_signed_avoidance_offset,
                              state.positive_request_accumulator,
                              state.target_lateral_offset,
                              state.lateral_rate.previous_lateral_offset,
                              state.lateral_rate.lateral_rate,
                              state.stuck_recovery.timer_seconds,
                              state.previous_brake};
  if (input.time_step_seconds < 0.0F || input.route_sample_count <= 0 ||
      input.route_sample < 0 ||
      input.route_sample >= input.route_sample_count ||
      !std::all_of(
          finite_inputs.begin(), finite_inputs.end(),
          [](const float value) { return std::isfinite(value); })) {
    throw std::invalid_argument(
        "original AI controller requires a bounded finite frame");
  }

  OriginalAiSafeSpeedInput safe_speed;
  safe_speed.curvature_state = input.curvature_state;
  safe_speed.curvature_limit = input.curvature_limit;
  safe_speed.global_speed_scale = input.global_speed_scale;
  safe_speed.vehicle_curve_coefficient = input.vehicle_curve_coefficient;
  safe_speed.signed_turn_radius = input.current_signed_turn_radius;
  const auto current_safe_speed = original_ai_safe_speed(safe_speed);
  safe_speed.signed_turn_radius = input.lookahead_signed_turn_radius;
  const auto lookahead_safe_speed = original_ai_safe_speed(safe_speed);

  OriginalAiLongitudinalDecisionInput decision;
  decision.recovery_brake = input.recovery_brake;
  decision.lookahead_curve_available = input.lookahead_curve_available;
  decision.current_segment_is_straight =
      input.current_signed_turn_radius == 0.0F;
  decision.special_brake_override = input.special_brake_override;
  decision.current_speed = input.current_speed;
  decision.current_safe_speed = current_safe_speed;
  decision.lookahead_safe_speed = lookahead_safe_speed;
  auto longitudinal_request = original_ai_select_longitudinal_request(decision);
  longitudinal_request = original_ai_apply_positive_request_slew(
      longitudinal_request, input.time_step_seconds,
      input.reset_positive_request_accumulator,
      state.positive_request_accumulator);

  OriginalAiAvoidanceChoiceInput avoidance_input;
  avoidance_input.route_sample_count = input.route_sample_count;
  avoidance_input.self_route_sample = input.route_sample;
  avoidance_input.current_speed = input.current_speed;
  avoidance_input.half_width = input.half_width;
  avoidance_input.target_lateral_offset = state.target_lateral_offset;
  avoidance_input.measured_lateral_offset = input.measured_lateral_offset;
  // The same-record branch consumes the retained derivative. The new
  // derivative is not stored until after avoidance in the retail update.
  avoidance_input.lateral_rate = state.lateral_rate.lateral_rate;
  avoidance_input.route_lateral_extent_a = input.route_lateral_extent_a;
  avoidance_input.route_lateral_extent_b = input.route_lateral_extent_b;
  avoidance_input.positive_side_occupied = input.positive_side_occupied;
  avoidance_input.negative_side_occupied = input.negative_side_occupied;
  avoidance_input.initial_signed_avoidance_offset =
      input.external_signed_avoidance_offset;
  auto avoidance = original_ai_choose_avoidance(avoidance_input, opponents);
  avoidance.force_full_brake =
      avoidance.force_full_brake || input.external_force_full_brake;
  OriginalAiAvoidanceTarget avoidance_target;
  if (input.preferred_lateral_offset == 0.0F) {
    // Preserve exact retail ordering when lane spreading is disabled.
    avoidance_target = original_ai_apply_avoidance_offset(
        state.target_lateral_offset, avoidance.signed_avoidance_offset);
    state.target_lateral_offset = avoidance_target.target_lateral_offset;
  } else {
    const auto transient_target = static_cast<float>(
        static_cast<double>(state.target_lateral_offset) -
        static_cast<double>(input.preferred_lateral_offset));
    avoidance_target = original_ai_apply_avoidance_offset(
        transient_target, avoidance.signed_avoidance_offset);
    state.target_lateral_offset = static_cast<float>(
        static_cast<double>(input.preferred_lateral_offset) +
        static_cast<double>(avoidance_target.target_lateral_offset));
    constexpr float edge_margin = 0.35F;
    const auto negative_limit = std::max(
        0.0F, input.route_lateral_extent_a - input.half_width - edge_margin);
    const auto positive_limit = std::max(
        0.0F, input.route_lateral_extent_b - input.half_width - edge_margin);
    state.target_lateral_offset = std::clamp(
        state.target_lateral_offset, -negative_limit, positive_limit);
  }

  OriginalAiStuckRecoveryInput stuck_input;
  stuck_input.measured_lateral_offset = input.measured_lateral_offset;
  stuck_input.direction_alignment = input.direction_alignment;
  stuck_input.current_speed = input.current_speed;
  stuck_input.signed_longitudinal_request = longitudinal_request;
  stuck_input.time_step_seconds = input.time_step_seconds;
  state.stuck_recovery =
      original_ai_update_stuck_recovery(stuck_input, state.stuck_recovery);

  state.lateral_rate = original_ai_update_lateral_rate(
      input.measured_lateral_offset, input.time_step_seconds,
      input.forward_aligned, state.lateral_rate);

  OriginalAiCurveFeedForwardInput feed_forward;
  feed_forward.current_signed_turn_radius = input.current_signed_turn_radius;
  feed_forward.group_signed_turn_radius =
      input.current_group_signed_turn_radius;
  feed_forward.curvature_state = input.curvature_state;
  feed_forward.curvature_limit = input.curvature_limit;
  feed_forward.global_speed_scale = input.global_speed_scale;
  feed_forward.vehicle_curve_coefficient = input.vehicle_curve_coefficient;
  feed_forward.vehicle_feed_forward_coefficient =
      input.vehicle_feed_forward_coefficient;
  feed_forward.current_speed = input.current_speed;

  OriginalAiBaseSteeringInput steering;
  steering.route_lateral_extent_a = input.route_lateral_extent_a;
  steering.route_lateral_extent_b = input.route_lateral_extent_b;
  steering.target_lateral_offset = state.target_lateral_offset;
  steering.measured_lateral_offset = input.measured_lateral_offset;
  steering.lateral_gain = input.lateral_gain;
  steering.lateral_rate_correction = original_ai_lateral_rate_correction(
      input.measured_lateral_offset, input.current_speed,
      state.lateral_rate.lateral_rate, input.lateral_rate_correction_scale);
  steering.curve_feed_forward = original_ai_curve_feed_forward(feed_forward);
  steering.direction_alignment = input.direction_alignment;
  steering.route_side_cross = input.route_side_cross;
  steering.collision_avoidance_active = avoidance_target.active;
  const auto base_steering = original_ai_base_steering_request(steering);

  OriginalAiRecoverySteeringInput recovery;
  recovery.route_lateral_extent_a = input.route_lateral_extent_a;
  recovery.route_lateral_extent_b = input.route_lateral_extent_b;
  recovery.measured_lateral_offset = input.measured_lateral_offset;
  recovery.current_speed = input.current_speed;
  recovery.signed_longitudinal_request = longitudinal_request;
  recovery.direction_alignment = input.direction_alignment;
  recovery.base_steering_request = base_steering;
  recovery.forward_aligned = input.forward_aligned;
  recovery.stuck_recovery_active = state.stuck_recovery.active;
  const auto recovered = original_ai_apply_recovery_steering(
      recovery, state.route_width_recovery_latch);
  state.route_width_recovery_latch = recovered.latch_active;

  auto controls = original_ai_control_handoff(
      longitudinal_request, state.previous_brake, recovered.steering_request);
  if (avoidance.force_full_brake) {
    controls.throttle = 0.0F;
    controls.brake = 1.0F;
  }
  // This write occurs after avoidance in p3.1 and intentionally does not
  // clear a brake slot that was already forced by a blocked pass.
  if (recovered.force_full_throttle) {
    controls.throttle = 1.0F;
  }
  state.previous_brake = controls.brake;

  return {
      controls,
      longitudinal_request,
      current_safe_speed,
      lookahead_safe_speed,
      avoidance_target.active,
      avoidance.force_full_brake,
  };
}

} // namespace mh::game
