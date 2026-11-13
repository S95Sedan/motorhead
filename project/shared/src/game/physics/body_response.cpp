#include <game/physics/body_response.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace mh::game {
namespace {

void require_finite(const CollisionVector3 &value, const char *name) {
  for (const auto component : value) {
    if (!std::isfinite(component)) {
      throw std::invalid_argument(name);
    }
  }
}

double dot(const CollisionVector3 &left, const CollisionVector3 &right) {
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

double stored_float32(const double value) {
  return static_cast<double>(static_cast<float>(value));
}

double stored_extended_float32(const long double value) {
  return static_cast<double>(static_cast<float>(value));
}

double original_signed_power(const double base, const double exponent) {
  // Motorhead's helper at RVA 0x000C4480 does not use the ISO pow domain
  // semantics for a negative base and fractional exponent. It raises the
  // absolute base at 0x000C458F..0x000C4595, then restores the original
  // base sign at 0x000C45A0..0x000C45A4.
  const auto magnitude = std::pow(std::fabs(base), exponent);
  return std::signbit(base) ? -magnitude : magnitude;
}

CollisionVector3
project_original_float32_vector_to_body(const BodyBasis3 &basis,
                                        const CollisionVector3 &world_vector) {
  CollisionVector3 result{};
  for (std::size_t axis = 0U; axis < result.size(); ++axis) {
    // RVA 0x000427E0 evaluates Y, X, then Z on the x87 stack and rounds
    // only when each completed component is stored.
    auto component =
        static_cast<long double>(stored_float32(world_vector[1U])) *
        static_cast<long double>(stored_float32(basis[axis][1U]));
    component += static_cast<long double>(stored_float32(world_vector[0U])) *
                 static_cast<long double>(stored_float32(basis[axis][0U]));
    component += static_cast<long double>(stored_float32(world_vector[2U])) *
                 static_cast<long double>(stored_float32(basis[axis][2U]));
    result[axis] = stored_extended_float32(component);
  }
  return result;
}

} // namespace

CollisionVector3
project_world_vector_to_body(const BodyBasis3 &basis,
                             const CollisionVector3 &world_vector) {
  require_finite(world_vector, "world vector must be finite");
  for (const auto &row : basis) {
    require_finite(row, "body basis must be finite");
  }
  return {dot(basis[0], world_vector), dot(basis[1], world_vector),
          dot(basis[2], world_vector)};
}

OriginalWheelForceResult
calculate_original_wheel_force(const OriginalWheelForceInputs &inputs) {
  require_finite(inputs.world_normal, "world contact normal must be finite");
  require_finite(inputs.dynamic_vector, "wheel dynamic vector must be finite");
  require_finite(inputs.base_force, "wheel base force must be finite");
  if (!std::isfinite(inputs.dynamic_x_scale) ||
      !std::isfinite(inputs.suspension_axis_y) ||
      !std::isfinite(inputs.spring_scalar) ||
      !std::isfinite(inputs.state_delta_scalar)) {
    throw std::invalid_argument("wheel force scalar must be finite");
  }

  CollisionVector3 body_normal{};
  for (std::size_t row = 0U; row < body_normal.size(); ++row) {
    auto component = static_cast<long double>(0.0);
    for (std::size_t column = 0U; column < body_normal.size(); ++column) {
      component +=
          static_cast<long double>(
              stored_float32(inputs.body_basis[row][column])) *
          static_cast<long double>(stored_float32(inputs.world_normal[column]));
    }
    body_normal[row] = stored_float32(component);
  }
  const auto normal_y =
      static_cast<long double>(stored_float32(inputs.world_normal[1]));
  const auto body_normal_y =
      static_cast<long double>(stored_float32(body_normal[1]));
  const auto unrounded_vertical_squared = normal_y * normal_y;
  const auto vertical_squared =
      static_cast<long double>(stored_float32(unrounded_vertical_squared));
  const auto normal_y_cubed = static_cast<long double>(
      stored_float32(unrounded_vertical_squared * normal_y));
  const auto tangent_scale = normal_y_cubed * body_normal_y * body_normal_y;
  const CollisionVector3 scaled_dynamic{
      stored_float32(stored_float32(inputs.dynamic_vector[0]) *
                     stored_float32(inputs.dynamic_x_scale)),
      stored_float32(inputs.dynamic_vector[1]),
      stored_float32(inputs.dynamic_vector[2])};
  auto normal_component = static_cast<long double>(0.0);
  for (std::size_t axis = 0U; axis < scaled_dynamic.size(); ++axis) {
    normal_component +=
        static_cast<long double>(scaled_dynamic[axis]) *
        static_cast<long double>(stored_float32(body_normal[axis]));
  }
  std::array<long double, 3U> tangent{};
  for (std::size_t axis = 0U; axis < tangent.size(); ++axis) {
    tangent[axis] = static_cast<long double>(scaled_dynamic[axis]) -
                    normal_component * static_cast<long double>(
                                           stored_float32(body_normal[axis]));
  }

  CollisionVector3 force{};
  force[0] = stored_float32(
      static_cast<long double>(stored_float32(inputs.base_force[0])) +
      tangent[0] * tangent_scale);
  force[1] = stored_float32(
      static_cast<long double>(stored_float32(inputs.base_force[1])) +
      static_cast<long double>(inputs.state_delta_scalar) +
      tangent[1] * tangent_scale -
      static_cast<long double>(stored_float32(inputs.suspension_axis_y)) *
          static_cast<long double>(inputs.spring_scalar) * vertical_squared);
  force[2] = stored_float32(
      static_cast<long double>(stored_float32(inputs.base_force[2])) +
      tangent[2] * tangent_scale);
  return {body_normal, force};
}

std::array<CollisionVector3, 4U> calculate_original_wheel_dynamic_vectors(
    const OriginalWheelDynamicVectorInputs &inputs) {
  if (!std::isfinite(inputs.primary_scalar) ||
      !std::isfinite(inputs.secondary_scalar) ||
      !std::isfinite(inputs.body_phase)) {
    throw std::invalid_argument("wheel dynamic-vector input must be finite");
  }

  double front_x = inputs.primary_scalar;
  double rear_x = 0.0;
  if (inputs.alternate_distribution) {
    const auto mix = (1.0 + std::sin(inputs.body_phase * 0.033)) * 0.25;
    front_x = inputs.primary_scalar * (mix + 0.5);
    rear_x = inputs.primary_scalar * (mix - 1.0);
  }

  return {{{front_x, 0.0, 0.0},
           {front_x, 0.0, 0.0},
           {rear_x, 0.0, inputs.secondary_scalar},
           {rear_x, 0.0, inputs.secondary_scalar}}};
}

double calculate_original_wheel_primary_scalar(
    const OriginalWheelPrimaryScalarInputs &inputs) {
  if (!std::isfinite(inputs.steering) || !std::isfinite(inputs.turn_force) ||
      !std::isfinite(inputs.ramp_limit) ||
      !std::isfinite(inputs.longitudinal_velocity)) {
    throw std::invalid_argument("primary wheel-scalar input must be finite");
  }

  const auto steering = std::clamp(inputs.steering, -1.0, 1.0);
  const auto magnitude = std::fabs(inputs.longitudinal_velocity);
  const auto ramp =
      magnitude < inputs.ramp_limit ? magnitude / inputs.ramp_limit : 1.0;
  auto result = steering * inputs.turn_force * ramp;
  if (inputs.longitudinal_velocity < 0.0 &&
      !inputs.suppress_negative_body_reversal) {
    result = -result;
  }
  return result;
}

double calculate_original_retained_steering(
    const OriginalRetainedSteeringInputs &inputs) {
  if (!std::isfinite(inputs.previous) || !std::isfinite(inputs.requested)) {
    throw std::invalid_argument("retained steering input must be finite");
  }
  constexpr double original_steering_response = 0.2;
  const auto requested = std::clamp(inputs.requested, -1.0, 1.0);
  const auto updated =
      stored_float32(inputs.previous + (requested - inputs.previous) *
                                           original_steering_response);
  return std::clamp(updated, -1.0, 1.0);
}

double
calculate_original_wheel_secondary_scalar(const double drivetrain_output) {
  if (!std::isfinite(drivetrain_output)) {
    throw std::invalid_argument("drivetrain output must be finite");
  }
  return drivetrain_output * 2.0;
}

double calculate_original_wheel_dynamic_x_scale(
    const OriginalWheelSurfaceScaleInputs &inputs) {
  if (!std::isfinite(inputs.static_factor) ||
      !std::isfinite(inputs.active_factor) ||
      !std::isfinite(inputs.active_divisor)) {
    throw std::invalid_argument("wheel surface-scale input must be finite");
  }
  if (!inputs.has_retained_surface) {
    return 1.0;
  }

  auto scale = inputs.static_factor;
  if (inputs.use_active_factor) {
    const auto active_ratio =
        std::clamp(10.0 / inputs.active_divisor, 0.0, 1.0);
    scale = inputs.active_factor * active_ratio;
  }
  if (inputs.apply_reduction) {
    scale *= 0.8;
  }
  return scale;
}

std::array<CollisionVector3, 4U> calculate_original_wheel_base_forces(
    const OriginalWheelBaseForceInputs &inputs) {
  if (!std::isfinite(inputs.authored_weight) ||
      !std::isfinite(inputs.global_vertical_scale) ||
      !std::isfinite(inputs.longitudinal_velocity)) {
    throw std::invalid_argument("wheel base-force input must be finite");
  }

  const auto weight =
      static_cast<long double>(stored_float32(inputs.authored_weight));
  const auto vertical_scale =
      static_cast<long double>(stored_float32(inputs.global_vertical_scale));
  const auto longitudinal_velocity =
      static_cast<long double>(stored_float32(inputs.longitudinal_velocity));
  const auto common_world_y = stored_extended_float32(-vertical_scale * weight);
  const auto distribution_world_y = stored_extended_float32(
      weight * static_cast<long double>(-0.05) * longitudinal_velocity);
  const auto common = project_original_float32_vector_to_body(
      inputs.body_basis, {0.0, common_world_y, 0.0});
  const auto distribution = project_original_float32_vector_to_body(
      inputs.body_basis, {0.0, distribution_world_y, 0.0});

  std::array<CollisionVector3, 4U> forces{};
  for (std::size_t index = 0U; index < forces.size(); ++index) {
    const auto distribution_scale =
        static_cast<long double>(index < 2U ? 0.3 : 0.2);
    for (std::size_t axis = 0U; axis < forces[index].size(); ++axis) {
      const auto common_component = stored_extended_float32(
          static_cast<long double>(stored_float32(common[axis])) * 0.25L);
      forces[index][axis] = stored_extended_float32(
          static_cast<long double>(stored_float32(distribution[axis])) *
              distribution_scale +
          static_cast<long double>(stored_float32(common_component)));
    }
  }
  return forces;
}

OriginalWheelLongitudinalLoadTransferResult
apply_original_wheel_longitudinal_load_transfer(
    const std::array<CollisionVector3, 4U> &base_forces,
    const OriginalWheelLongitudinalLoadTransferInputs &inputs) {
  for (const auto &force : base_forces) {
    require_finite(force, "wheel base force must be finite");
  }
  if (!std::isfinite(inputs.previous_longitudinal_velocity) ||
      !std::isfinite(inputs.current_longitudinal_velocity) ||
      !std::isfinite(inputs.slice_seconds) || inputs.slice_seconds <= 0.0) {
    throw std::invalid_argument(
        "wheel longitudinal load-transfer input is invalid");
  }

  OriginalWheelLongitudinalLoadTransferResult result;
  result.base_forces = base_forces;
  if (!inputs.all_wheels_grounded) {
    return result;
  }

  const auto previous = static_cast<long double>(
      stored_float32(inputs.previous_longitudinal_velocity));
  const auto current = static_cast<long double>(
      stored_float32(inputs.current_longitudinal_velocity));
  const auto slice =
      static_cast<long double>(stored_float32(inputs.slice_seconds));
  result.candidate = std::clamp(
      stored_extended_float32(((previous - current) / slice) *
                              (static_cast<long double>(500.0) -
                               current * static_cast<long double>(3.0))),
      -8000.0, 8000.0);
  const auto candidate =
      static_cast<long double>(stored_float32(result.candidate));
  const auto half_candidate =
      static_cast<long double>(static_cast<double>(result.candidate) * 0.5);
  if (result.candidate > 0.0) {
    result.base_forces[0U][1U] = stored_extended_float32(
        static_cast<long double>(stored_float32(result.base_forces[0U][1U])) -
        candidate);
    result.base_forces[1U][1U] = stored_extended_float32(
        static_cast<long double>(stored_float32(result.base_forces[1U][1U])) -
        candidate);
    result.base_forces[2U][1U] = stored_extended_float32(
        static_cast<long double>(stored_float32(result.base_forces[2U][1U])) +
        half_candidate);
    result.base_forces[3U][1U] = stored_extended_float32(
        static_cast<long double>(stored_float32(result.base_forces[3U][1U])) +
        half_candidate);
  } else {
    result.base_forces[0U][1U] = stored_extended_float32(
        static_cast<long double>(stored_float32(result.base_forces[0U][1U])) -
        half_candidate);
    result.base_forces[1U][1U] = stored_extended_float32(
        static_cast<long double>(stored_float32(result.base_forces[1U][1U])) -
        half_candidate);
    result.base_forces[2U][1U] = stored_extended_float32(
        static_cast<long double>(stored_float32(result.base_forces[2U][1U])) +
        candidate);
    result.base_forces[3U][1U] = stored_extended_float32(
        static_cast<long double>(stored_float32(result.base_forces[3U][1U])) +
        candidate);
  }
  return result;
}

double
finalize_original_drivetrain_output(const double accumulated_output,
                                    const std::uint32_t control_flags,
                                    const std::size_t selected_gear_index) {
  if (!std::isfinite(accumulated_output)) {
    throw std::invalid_argument("drivetrain output must be finite");
  }
  if ((control_flags & 0x30U) != 0U) {
    return 0.0;
  }
  return selected_gear_index == 0U ? -accumulated_output : accumulated_output;
}

std::size_t calculate_original_drivetrain_candidate_gear(
    const OriginalDrivetrainCandidateGearInputs &inputs) {
  if (inputs.maximum_gear_index == 0U ||
      inputs.current_gear_index > inputs.maximum_gear_index) {
    throw std::invalid_argument("drivetrain gear range is invalid");
  }

  auto candidate = inputs.current_gear_index;
  if (!inputs.first_shift_latched &&
      inputs.current_gear_index < inputs.maximum_gear_index) {
    ++candidate;
  }
  if (!inputs.second_shift_latched) {
    if (inputs.current_gear_index == 0U) {
      candidate = 1U;
    } else {
      --candidate;
    }
  }
  return candidate;
}

OriginalDrivetrainPrimaryDriveCandidateResult
calculate_original_drivetrain_primary_drive_candidate(
    const OriginalDrivetrainPrimaryDriveCandidateInputs &inputs) {
  const std::array values{
      inputs.longitudinal_velocity,
      inputs.steering,
      inputs.control_a,
      inputs.current_gear_value,
      inputs.current_engine_scalar,
      inputs.authored_minimum,
  };
  if (!std::all_of(values.begin(), values.end(),
                   [](const double value) { return std::isfinite(value); }) ||
      inputs.current_gear_value <= 0.0 || inputs.current_engine_scalar < 0.0 ||
      inputs.authored_minimum < 0.0 || inputs.control_a < 0.0 ||
      inputs.control_a > 1.0 || inputs.current_gear_index >= 10U ||
      inputs.candidate_gear_index >= 10U) {
    throw std::invalid_argument(
        "drivetrain primary-drive candidate input is invalid");
  }

  constexpr std::array<float, 10U> original_candidate_schedule{
      0.0F, 400.0F, 350.0F, 330.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F,
  };
  auto scheduled = static_cast<double>(
      original_candidate_schedule[inputs.candidate_gear_index]);
  if (inputs.current_gear_index == 1U) {
    constexpr double original_steering_scale = 500.0;
    scheduled =
        stored_float32(static_cast<long double>(scheduled) +
                       static_cast<long double>(std::fabs(inputs.steering)) *
                           original_steering_scale);
  }

  constexpr double original_speed_scale = 3.6;
  constexpr double original_low_speed_fraction = 0.3;
  constexpr double original_minimum_scale = 0.001;
  constexpr double original_falloff_divisor = 0.7;
  const auto speed =
      std::fabs(inputs.longitudinal_velocity) * original_speed_scale;
  auto factor = inputs.authored_minimum * original_minimum_scale;
  if (inputs.current_gear_value * original_low_speed_fraction <= speed) {
    factor *= (inputs.current_gear_value - speed) /
              (inputs.current_gear_value * original_falloff_divisor);
  }
  const auto candidate = stored_float32(static_cast<long double>(scheduled) *
                                        static_cast<long double>(factor));
  constexpr double original_control_threshold = 0.1;
  const auto active = !inputs.suppressed &&
                      candidate > inputs.current_engine_scalar &&
                      inputs.control_a > original_control_threshold &&
                      inputs.candidate_gear_index != 0U;
  return {candidate, active, active && !inputs.engine_transition_active};
}

OriginalDrivetrainControlMapping calculate_original_drivetrain_control_mapping(
    const double control_a, const double control_b,
    const double authored_brake_force, const std::size_t current_gear_index) {
  if (!std::isfinite(control_a) || !std::isfinite(control_b) ||
      !std::isfinite(authored_brake_force)) {
    throw std::invalid_argument("drivetrain control input must be finite");
  }
  if (current_gear_index == 0U) {
    return {-control_b, control_a * authored_brake_force};
  }
  return {control_a, control_b * authored_brake_force};
}

double calculate_original_drivetrain_baseline_contribution(
    const OriginalDrivetrainBaselineInputs &inputs) {
  if (!std::isfinite(inputs.control_a) || !std::isfinite(inputs.control_b) ||
      !std::isfinite(inputs.current_gear_value) ||
      !std::isfinite(inputs.engine_scalar) ||
      !std::isfinite(inputs.longitudinal_velocity)) {
    throw std::invalid_argument("drivetrain baseline input must be finite");
  }
  if (inputs.control_a < 0.0 || inputs.control_a > 1.0 ||
      inputs.control_b < 0.0 || inputs.control_b > 1.0) {
    throw std::invalid_argument("drivetrain control must be clamped");
  }
  if (inputs.current_gear_value == 0.0) {
    throw std::invalid_argument("drivetrain gear value must be nonzero");
  }

  const auto selected_control =
      inputs.current_gear_index == 0U ? inputs.control_b : inputs.control_a;
  if (inputs.alternate_mode || inputs.transition_active ||
      selected_control >= 0.15) {
    return 0.0;
  }

  const auto magnitude =
      inputs.engine_scalar * 13.0 / inputs.current_gear_value;
  return inputs.longitudinal_velocity > 0.0 ? -magnitude : magnitude;
}

double calculate_original_drivetrain_brake_contribution(
    const double longitudinal_velocity, const double control_b,
    const double authored_brake_force) {
  if (!std::isfinite(longitudinal_velocity) || !std::isfinite(control_b) ||
      !std::isfinite(authored_brake_force)) {
    throw std::invalid_argument("drivetrain brake input must be finite");
  }
  if (control_b < 0.0 || control_b > 1.0) {
    throw std::invalid_argument("drivetrain control must be clamped");
  }
  return -std::clamp(longitudinal_velocity, -1.0, 1.0) * control_b *
         authored_brake_force;
}

double calculate_original_drivetrain_low_speed_contribution(
    const double longitudinal_velocity, const double drive_scalar) {
  if (!std::isfinite(longitudinal_velocity) || !std::isfinite(drive_scalar)) {
    throw std::invalid_argument("drivetrain low-speed input must be finite");
  }
  const auto speed_scale = std::fabs(longitudinal_velocity * 3.6);
  if (speed_scale >= 10.0) {
    return 0.0;
  }
  return (10.0 - speed_scale) * drive_scalar * 300.0;
}

double calculate_original_drivetrain_manual_coast_contribution(
    const double longitudinal_velocity, const double current_gear_value,
    const double engine_scalar, const double control_a,
    const bool engine_transition_active) {
  if (!std::isfinite(longitudinal_velocity) ||
      !std::isfinite(current_gear_value) || !std::isfinite(engine_scalar) ||
      !std::isfinite(control_a) || current_gear_value <= 0.0 ||
      engine_scalar < 0.0 || control_a < 0.0 || control_a > 1.0) {
    throw std::invalid_argument("manual drivetrain coast input is invalid");
  }
  // p3.1 0x0001e1b5..0x0001e263: manual coasting is absent while the
  // accelerator is at least 0.15 or the preceding engine transition is live.
  if (control_a >= 0.15 || engine_transition_active) {
    return 0.0;
  }

  auto factor = 13.0;
  const auto speed = std::fabs(longitudinal_velocity);
  if (speed > current_gear_value) {
    factor = speed / current_gear_value * 13.0;
  }
  const auto magnitude = factor * engine_scalar / current_gear_value;
  return longitudinal_velocity > 0.0 ? -magnitude : magnitude;
}

double calculate_original_drivetrain_engine_candidate(
    const double engine_scalar, const double drive_term,
    const double slice_seconds, const double authored_maximum) {
  if (!std::isfinite(engine_scalar) || !std::isfinite(drive_term) ||
      !std::isfinite(slice_seconds) || !std::isfinite(authored_maximum)) {
    throw std::invalid_argument("drivetrain engine input must be finite");
  }
  if (engine_scalar < 0.0 || slice_seconds < 0.0 || authored_maximum <= 0.0) {
    throw std::invalid_argument("drivetrain engine input is invalid");
  }
  const auto candidate = (engine_scalar + drive_term * 27.5 * slice_seconds) *
                         std::pow(0.625, slice_seconds);
  return std::min(candidate, authored_maximum);
}

double calculate_original_drivetrain_engine_drive_term(
    const double candidate_gear_value, const double reference_value,
    const double drive_scalar) {
  if (!std::isfinite(candidate_gear_value) || !std::isfinite(reference_value) ||
      !std::isfinite(drive_scalar)) {
    throw std::invalid_argument("drivetrain engine-drive input must be finite");
  }
  return ((candidate_gear_value - reference_value) * 0.02 + 80.0) *
         drive_scalar;
}

double calculate_original_drivetrain_speed_candidate(
    const double longitudinal_velocity, const double current_gear_value,
    const double authored_minimum, const double authored_maximum) {
  if (!std::isfinite(longitudinal_velocity) ||
      !std::isfinite(current_gear_value) || !std::isfinite(authored_minimum) ||
      !std::isfinite(authored_maximum) || current_gear_value == 0.0 ||
      authored_maximum <= authored_minimum) {
    throw std::invalid_argument("drivetrain speed-candidate input is invalid");
  }
  const auto speed_scale = std::fabs(longitudinal_velocity * 3.6);
  const auto speed_candidate =
      authored_minimum +
      (authored_maximum - authored_minimum) * speed_scale / current_gear_value;
  return std::max(authored_minimum + 1.0, speed_candidate);
}

double calculate_original_drivetrain_acceleration_mode_a(
    const OriginalDrivetrainAccelerationModeAInputs &inputs) {
  const std::array values{inputs.engine_scalar,
                          inputs.authored_minimum,
                          inputs.authored_maximum,
                          inputs.reference_value,
                          inputs.candidate_gear_value,
                          inputs.drive_scalar,
                          inputs.authored_acceleration_force};
  if (!std::all_of(values.begin(), values.end(),
                   [](const double value) { return std::isfinite(value); })) {
    throw std::invalid_argument("drivetrain acceleration input must be finite");
  }

  const auto engine_offset =
      (inputs.engine_scalar - inputs.authored_minimum) * 0.001;
  const auto curve_divisor = inputs.authored_maximum * 0.00097 - engine_offset;
  const auto gear_divisor =
      inputs.candidate_gear_value * inputs.candidate_gear_value * 0.0005;
  if (engine_offset < 0.0 || curve_divisor == 0.0 || gear_divisor == 0.0) {
    throw std::invalid_argument("drivetrain acceleration domain is invalid");
  }

  const auto curve = std::sqrt(engine_offset) - 2.0 / curve_divisor;
  const auto gear_scale = inputs.reference_value / gear_divisor * 0.7;
  const auto result = curve * gear_scale * inputs.drive_scalar *
                      inputs.authored_acceleration_force;
  if (!std::isfinite(result)) {
    throw std::invalid_argument("drivetrain acceleration result is not finite");
  }
  return result;
}

double calculate_original_drivetrain_acceleration_mode_b(
    const OriginalDrivetrainAccelerationModeBInputs &inputs) {
  const std::array values{
      inputs.longitudinal_velocity, inputs.current_gear_value,
      inputs.authored_minimum,      inputs.authored_maximum,
      inputs.drive_scalar,          inputs.authored_acceleration_force};
  if (!std::all_of(values.begin(), values.end(),
                   [](const double value) { return std::isfinite(value); }) ||
      inputs.current_gear_value == 0.0) {
    throw std::invalid_argument("drivetrain mode-B input is invalid");
  }

  const auto candidate = calculate_original_drivetrain_speed_candidate(
      inputs.longitudinal_velocity, inputs.current_gear_value,
      inputs.authored_minimum, inputs.authored_maximum);
  const auto offset = std::min((candidate - inputs.authored_minimum) * 0.001,
                               inputs.authored_maximum * 0.00093);
  const auto curve_divisor = inputs.authored_maximum * 0.00097 - offset;
  if (offset < 0.0 || curve_divisor == 0.0) {
    throw std::invalid_argument("drivetrain mode-B domain is invalid");
  }

  const auto result = (std::sqrt(offset) - 2.0 / curve_divisor) *
                      inputs.drive_scalar * inputs.authored_acceleration_force;
  if (!std::isfinite(result)) {
    throw std::invalid_argument("drivetrain mode-B result is not finite");
  }
  return result;
}

double calculate_original_drivetrain_acceleration_mode_c(
    const OriginalDrivetrainAccelerationModeCInputs &inputs) {
  const std::array values{
      inputs.longitudinal_velocity, inputs.current_gear_value,
      inputs.authored_minimum,      inputs.authored_maximum,
      inputs.drive_scalar,          inputs.authored_acceleration_force};
  if (!std::all_of(values.begin(), values.end(),
                   [](const double value) { return std::isfinite(value); }) ||
      inputs.current_gear_value == 0.0) {
    throw std::invalid_argument("drivetrain mode-C input is invalid");
  }

  const auto candidate = calculate_original_drivetrain_speed_candidate(
      inputs.longitudinal_velocity, inputs.current_gear_value,
      inputs.authored_minimum, inputs.authored_maximum);
  const auto gear_adjusted_limit =
      inputs.authored_maximum * 0.000935 -
      static_cast<double>(inputs.candidate_gear_index) * 0.15;
  const auto offset = std::min((candidate - inputs.authored_minimum) * 0.001,
                               gear_adjusted_limit);
  const auto curve_divisor = inputs.authored_maximum * 0.001 - offset;
  if (offset < 0.0 || curve_divisor == 0.0) {
    throw std::invalid_argument("drivetrain mode-C domain is invalid");
  }

  const auto result = (std::sqrt(offset) - 2.0 / curve_divisor) *
                      inputs.drive_scalar * inputs.authored_acceleration_force;
  if (!std::isfinite(result)) {
    throw std::invalid_argument("drivetrain mode-C result is not finite");
  }
  return result;
}

std::size_t calculate_original_drivetrain_automatic_gear(
    const OriginalDrivetrainAutomaticGearInputs &inputs) {
  const std::array values{inputs.longitudinal_velocity, inputs.control_a,
                          inputs.control_b, inputs.transition_scalar,
                          inputs.authored_maximum};
  if (!std::all_of(values.begin(), values.end(),
                   [](const double value) { return std::isfinite(value); }) ||
      inputs.control_a < 0.0 || inputs.control_a > 1.0 ||
      inputs.control_b < 0.0 || inputs.control_b > 1.0 ||
      inputs.authored_maximum <= 0.0 || inputs.maximum_gear_index == 0U ||
      inputs.current_gear_index > inputs.maximum_gear_index) {
    throw std::invalid_argument("automatic gear input is invalid");
  }

  auto gear = inputs.current_gear_index;
  if (inputs.longitudinal_velocity >= 2.0) {
    if (gear == 0U) {
      gear = 1U;
    }
    if (gear > 1U && inputs.transition_scalar < inputs.authored_maximum * 0.6) {
      --gear;
    }
    if (inputs.transition_scalar > inputs.authored_maximum * 0.912 &&
        gear < inputs.maximum_gear_index) {
      ++gear;
    }
  }

  if (inputs.longitudinal_velocity <= -2.0 && inputs.control_b > 0.05) {
    gear = 0U;
  }

  if (std::fabs(inputs.longitudinal_velocity) < 2.0 &&
      (!inputs.direction_transition_active || gear == 0U)) {
    if (inputs.control_b > 0.25 && inputs.control_a < 0.25) {
      gear = 0U;
    }
    if (inputs.control_b < 0.25 && inputs.control_a > 0.25) {
      gear = 1U;
    }
  }
  return gear;
}

OriginalDrivetrainManualControls calculate_original_drivetrain_manual_controls(
    const double control_a, const double control_b,
    const double authored_brake_force) {
  if (!std::isfinite(control_a) || !std::isfinite(control_b) ||
      !std::isfinite(authored_brake_force) || control_a < 0.0 ||
      control_a > 1.0 || control_b < 0.0 || control_b > 1.0) {
    throw std::invalid_argument("manual drivetrain control input is invalid");
  }

  const auto strong_control_b_active = control_b > 0.9;
  return {
      strong_control_b_active ? 0.0 : control_a,
      control_b * authored_brake_force,
      control_b > 0.15,
      strong_control_b_active,
  };
}

OriginalDrivetrainManualGearResult calculate_original_drivetrain_manual_gear(
    const OriginalDrivetrainManualGearInputs &inputs) {
  if (!std::isfinite(inputs.longitudinal_velocity) ||
      inputs.maximum_gear_index == 0U ||
      inputs.current_gear_index > inputs.maximum_gear_index) {
    throw std::invalid_argument("manual drivetrain gear input is invalid");
  }

  auto gear = inputs.current_gear_index;
  auto first_shift_latched = inputs.first_shift_latched;
  auto second_shift_latched = inputs.second_shift_latched;

  if ((inputs.control_flags & 0x20U) != 0U) {
    second_shift_latched = false;
  }
  if ((inputs.control_flags & 0x10U) != 0U) {
    first_shift_latched = false;
  }

  if ((inputs.control_flags & 0x20U) == 0U && !second_shift_latched) {
    second_shift_latched = true;
    if (gear == 1U && inputs.longitudinal_velocity < 5.0) {
      gear = 0U;
    }
    if (gear > 1U) {
      --gear;
    }
  }

  if ((inputs.control_flags & 0x10U) == 0U && !first_shift_latched) {
    if (gear == 0U) {
      first_shift_latched = true;
      ++gear;
    } else {
      if (gear < inputs.maximum_gear_index) {
        ++gear;
      }
      first_shift_latched = true;
    }
  }

  return {gear, first_shift_latched, second_shift_latched};
}

OriginalDrivetrainEngineStateResult calculate_original_drivetrain_engine_state(
    const OriginalDrivetrainEngineStateInputs &inputs) {
  const std::array values{
      inputs.engine_scalar,    inputs.transition_candidate,
      inputs.authored_minimum, inputs.authored_maximum,
      inputs.slice_seconds,    inputs.control_a,
  };
  if (!std::all_of(values.begin(), values.end(),
                   [](const double value) { return std::isfinite(value); }) ||
      inputs.authored_minimum < 0.0 ||
      inputs.authored_maximum <= inputs.authored_minimum ||
      inputs.slice_seconds < 0.0 || inputs.control_a < 0.0 ||
      inputs.control_a > 1.0) {
    throw std::invalid_argument("drivetrain engine-state input is invalid");
  }

  const auto direct_commit = inputs.first_shift_latched &&
                             inputs.second_shift_latched &&
                             (inputs.transition_source_a_active ||
                              inputs.transition_source_b_active) &&
                             !inputs.external_transition_active;
  if (direct_commit) {
    return {inputs.transition_candidate, false};
  }

  if (inputs.control_a > 0.1) {
    const auto rise_scale =
        std::max(((inputs.authored_maximum - inputs.engine_scalar) + 3000.0) *
                     0.1 * inputs.control_a,
                 10.0);
    return {inputs.engine_scalar + inputs.slice_seconds * 25.0 * rise_scale,
            true};
  }

  return {std::max(inputs.engine_scalar - inputs.slice_seconds * 6000.0,
                   inputs.authored_minimum),
          true};
}

OriginalDrivetrainModeCStateResult calculate_original_drivetrain_mode_c_state(
    const OriginalDrivetrainModeCStateInputs &inputs) {
  const auto transition_candidate =
      calculate_original_drivetrain_speed_candidate(
          inputs.longitudinal_velocity, inputs.current_gear_value,
          inputs.authored_minimum, inputs.authored_maximum);
  const auto current_gear_index = calculate_original_drivetrain_automatic_gear(
      {inputs.longitudinal_velocity, inputs.control_a, inputs.control_b,
       transition_candidate, inputs.authored_maximum, inputs.current_gear_index,
       inputs.maximum_gear_index, inputs.direction_transition_active});
  const auto engine = calculate_original_drivetrain_engine_state(
      {inputs.engine_scalar, transition_candidate, inputs.authored_minimum,
       inputs.authored_maximum, inputs.slice_seconds, inputs.control_a,
       inputs.first_shift_latched, inputs.second_shift_latched,
       inputs.transition_source_a_active, inputs.transition_source_b_active,
       inputs.external_transition_active});
  return {transition_candidate,
          current_gear_index,
          engine.engine_scalar,
          engine.transition_active,
          inputs.control_b > 0.9,
          false,
          false,
          inputs.first_shift_latched,
          inputs.second_shift_latched};
}

OriginalDrivetrainModeCForceResult calculate_original_drivetrain_mode_c_force(
    const OriginalDrivetrainModeCForceInputs &inputs) {
  auto state_inputs = inputs.state;
  state_inputs.control_a = std::clamp(state_inputs.control_a, 0.0, 1.0);
  state_inputs.control_b = std::clamp(state_inputs.control_b, 0.0, 1.0);
  const auto original_control_a = state_inputs.control_a;
  // RVA 0x0001de01 stores drivetrain +0xa0 when control B is strictly above
  // the binary64 0.9 constant and clears control A for the same update.
  if (state_inputs.control_b > 0.9) {
    state_inputs.control_a = 0.0;
  }

  const auto candidate_gear_index =
      calculate_original_drivetrain_candidate_gear(
          {state_inputs.current_gear_index, state_inputs.maximum_gear_index,
           state_inputs.first_shift_latched,
           state_inputs.second_shift_latched});
  const auto primary_drive =
      calculate_original_drivetrain_primary_drive_candidate(
          {state_inputs.longitudinal_velocity, inputs.steering,
           original_control_a, state_inputs.current_gear_value,
           state_inputs.engine_scalar, state_inputs.authored_minimum,
           state_inputs.current_gear_index, candidate_gear_index,
           inputs.engine_transition_active,
           inputs.suppress_primary_drive_candidate &&
               inputs.acceleration_selector == 2U});
  if (inputs.manual_mode) {
    if (!std::isfinite(inputs.candidate_gear_value) ||
        !std::isfinite(inputs.reference_value) ||
        inputs.candidate_gear_value <= 0.0 || inputs.reference_value <= 0.0) {
      throw std::invalid_argument("manual drivetrain ratio input is invalid");
    }

    const auto manual_controls = calculate_original_drivetrain_manual_controls(
        state_inputs.control_a, state_inputs.control_b,
        inputs.authored_brake_force);
    double transition_candidate = 0.0;
    double acceleration = 0.0;
    if (primary_drive.candidate_active) {
      const auto drive_term = calculate_original_drivetrain_engine_drive_term(
          inputs.candidate_gear_value, inputs.reference_value,
          manual_controls.drive_scalar);
      transition_candidate =
          stored_float32(calculate_original_drivetrain_engine_candidate(
              state_inputs.engine_scalar, drive_term,
              state_inputs.slice_seconds, state_inputs.authored_maximum));
      acceleration = calculate_original_drivetrain_acceleration_mode_a(
          {state_inputs.engine_scalar, state_inputs.authored_minimum,
           state_inputs.authored_maximum, inputs.reference_value,
           inputs.candidate_gear_value, manual_controls.drive_scalar,
           inputs.authored_acceleration_force});
    } else {
      transition_candidate =
          stored_float32(calculate_original_drivetrain_speed_candidate(
              state_inputs.longitudinal_velocity,
              state_inputs.current_gear_value, state_inputs.authored_minimum,
              state_inputs.authored_maximum));
      acceleration = calculate_original_drivetrain_acceleration_mode_c(
          {state_inputs.longitudinal_velocity, state_inputs.current_gear_value,
           candidate_gear_index, state_inputs.authored_minimum,
           state_inputs.authored_maximum, manual_controls.drive_scalar,
           inputs.authored_acceleration_force});
    }

    const auto low_speed = calculate_original_drivetrain_low_speed_contribution(
        state_inputs.longitudinal_velocity, manual_controls.drive_scalar);
    auto accumulated_output = stored_float32(acceleration);
    accumulated_output =
        stored_float32(static_cast<long double>(accumulated_output) +
                       static_cast<long double>(low_speed));
    // The manual branch clears the accumulator while either shift control is
    // held, then applies reverse before coast and brake (RVA 0x1e170 onward).
    if ((inputs.control_flags & 0x30U) != 0U) {
      accumulated_output = 0.0;
    }
    if (candidate_gear_index == 0U) {
      accumulated_output = stored_float32(-accumulated_output);
    }

    const auto coast = calculate_original_drivetrain_manual_coast_contribution(
        state_inputs.longitudinal_velocity, state_inputs.current_gear_value,
        state_inputs.engine_scalar, state_inputs.control_a,
        inputs.engine_transition_active);
    accumulated_output =
        stored_float32(static_cast<long double>(accumulated_output) +
                       static_cast<long double>(coast));
    const auto brake = calculate_original_drivetrain_brake_contribution(
        state_inputs.longitudinal_velocity, state_inputs.control_b,
        inputs.authored_brake_force);
    accumulated_output =
        stored_float32(static_cast<long double>(accumulated_output) +
                       static_cast<long double>(brake));

    const auto manual_gear = calculate_original_drivetrain_manual_gear(
        {inputs.control_flags, state_inputs.longitudinal_velocity,
         state_inputs.current_gear_index, state_inputs.maximum_gear_index,
         state_inputs.first_shift_latched, state_inputs.second_shift_latched});
    const auto engine = calculate_original_drivetrain_engine_state(
        {state_inputs.engine_scalar, transition_candidate,
         state_inputs.authored_minimum, state_inputs.authored_maximum,
         state_inputs.slice_seconds, state_inputs.control_a,
         manual_gear.first_shift_latched, manual_gear.second_shift_latched,
         state_inputs.transition_source_a_active,
         state_inputs.transition_source_b_active,
         state_inputs.external_transition_active});
    OriginalDrivetrainModeCStateResult state{transition_candidate,
                                             manual_gear.current_gear_index,
                                             engine.engine_scalar,
                                             engine.transition_active,
                                             state_inputs.control_b > 0.9,
                                             primary_drive.candidate_active,
                                             primary_drive.transition_active,
                                             manual_gear.first_shift_latched,
                                             manual_gear.second_shift_latched};
    const auto primary = calculate_original_wheel_primary_scalar(
        {inputs.steering, inputs.authored_turn_force,
         inputs.steering_ramp_limit, state_inputs.longitudinal_velocity,
         inputs.suppress_negative_body_reversal});
    const auto secondary =
        calculate_original_wheel_secondary_scalar(accumulated_output);
    return {state,
            candidate_gear_index,
            {manual_controls.drive_scalar, manual_controls.brake_force},
            coast,
            acceleration,
            brake,
            low_speed,
            accumulated_output,
            {primary, secondary, 0.0, false}};
  }
  const auto controls = calculate_original_drivetrain_control_mapping(
      state_inputs.control_a, state_inputs.control_b,
      inputs.authored_brake_force, state_inputs.current_gear_index);
  const auto baseline = calculate_original_drivetrain_baseline_contribution(
      {false, inputs.engine_transition_active, state_inputs.current_gear_index,
       state_inputs.control_a, state_inputs.control_b,
       state_inputs.current_gear_value, state_inputs.engine_scalar,
       state_inputs.longitudinal_velocity});
  double acceleration = 0.0;
  if (inputs.acceleration_selector == 2U) {
    acceleration = calculate_original_drivetrain_acceleration_mode_b(
        {state_inputs.longitudinal_velocity, state_inputs.current_gear_value,
         state_inputs.authored_minimum, state_inputs.authored_maximum,
         controls.drive_scalar, inputs.authored_acceleration_force});
  } else if (inputs.acceleration_selector == 0U) {
    acceleration = calculate_original_drivetrain_acceleration_mode_c(
        {state_inputs.longitudinal_velocity, state_inputs.current_gear_value,
         candidate_gear_index, state_inputs.authored_minimum,
         state_inputs.authored_maximum, controls.drive_scalar,
         inputs.authored_acceleration_force});
  } else {
    throw std::invalid_argument(
        "automatic drivetrain acceleration selector is unsupported");
  }
  const auto brake = calculate_original_drivetrain_brake_contribution(
      state_inputs.longitudinal_velocity, state_inputs.control_b,
      inputs.authored_brake_force);
  const auto low_speed = calculate_original_drivetrain_low_speed_contribution(
      state_inputs.longitudinal_velocity, controls.drive_scalar);
  // p3.1 stores stack+0x160 to float32 after each contribution (for example
  // RVAs 0x1DAB4 and 0x1DC98). A binary64 sum shifts some CPU launch forces by
  // one ULP, which becomes visible after left/right torque cancellation.
  auto accumulated_output = stored_float32(baseline);
  accumulated_output =
      stored_float32(static_cast<long double>(accumulated_output) +
                     static_cast<long double>(acceleration));
  accumulated_output =
      stored_float32(static_cast<long double>(accumulated_output) +
                     static_cast<long double>(brake));
  accumulated_output =
      stored_float32(static_cast<long double>(accumulated_output) +
                     static_cast<long double>(low_speed));
  const auto output = finalize_original_drivetrain_output(
      accumulated_output, inputs.control_flags, candidate_gear_index);
  const auto primary = calculate_original_wheel_primary_scalar(
      {inputs.steering, inputs.authored_turn_force, inputs.steering_ramp_limit,
       state_inputs.longitudinal_velocity,
       inputs.suppress_negative_body_reversal});
  const auto secondary = calculate_original_wheel_secondary_scalar(output);

  auto state = calculate_original_drivetrain_mode_c_state(state_inputs);
  state.primary_drive_candidate_active = primary_drive.candidate_active;
  state.primary_drive_transition_active = primary_drive.transition_active;
  return {state,
          candidate_gear_index,
          controls,
          baseline,
          acceleration,
          brake,
          low_speed,
          output,
          {primary, secondary, 0.0, false}};
}

double select_original_global_vertical_scale(
    const std::uint8_t global_flags) noexcept {
  return (global_flags & 0x02U) != 0U ? 7.905 : 15.81;
}

CollisionVector3
calculate_original_world_gravity_delta(const double global_vertical_scale,
                                       const double slice_seconds) {
  if (!std::isfinite(global_vertical_scale) || !std::isfinite(slice_seconds) ||
      global_vertical_scale < 0.0 || slice_seconds < 0.0) {
    throw std::invalid_argument("body gravity input is invalid");
  }
  return {0.0, -global_vertical_scale * 0.87 * slice_seconds, 0.0};
}

OriginalBodyMassProperties
make_original_body_mass_properties(const double authored_weight) {
  if (!std::isfinite(authored_weight) || authored_weight <= 0.0) {
    throw std::invalid_argument(
        "authored body weight must be positive and finite");
  }
  return {authored_weight, {3500.0, 2000.0, 1500.0}};
}

CollisionVector3 calculate_original_vehicle_velocity_stabilizer_seed(
    const OriginalVehicleVelocityStabilizerInputs &inputs) {
  require_finite(inputs.velocity.local_linear,
                 "vehicle stabilizer linear velocity must be finite");
  require_finite(inputs.velocity.local_angular,
                 "vehicle stabilizer angular velocity must be finite");
  if (inputs.wheel_count > 4U) {
    throw std::invalid_argument("vehicle stabilizer wheel count exceeds four");
  }
  if (inputs.retained_history_sum <= 3U || inputs.wheel_count == 0U) {
    return {};
  }

  const auto lateral = static_cast<float>(inputs.velocity.local_linear[0U]);
  const auto longitudinal =
      static_cast<float>(inputs.velocity.local_linear[2U]);
  const auto coefficient = inputs.reduced_coefficient ? 10.0 : 80.0;
  auto seed_z = 0.0F;
  for (std::size_t wheel = 0U; wheel < inputs.wheel_count; ++wheel) {
    seed_z = static_cast<float>(
        static_cast<double>(seed_z) -
        static_cast<double>(lateral) * coefficient *
            std::sqrt(std::fabs(static_cast<double>(longitudinal))));
  }
  return {0.0, 0.0, static_cast<double>(seed_z)};
}

void apply_original_body_force_response(
    OriginalBodyVelocityState &state,
    const OriginalBodyForceResponseInputs &inputs) {
  require_finite(inputs.accumulated_force.linear,
                 "linear force accumulator must be finite");
  require_finite(inputs.accumulated_force.angular,
                 "angular force accumulator must be finite");
  require_finite(inputs.principal_inertia, "principal inertia must be finite");
  if (!std::isfinite(inputs.mass) || !std::isfinite(inputs.slice_seconds) ||
      inputs.mass <= 0.0 || inputs.slice_seconds < 0.0 ||
      std::any_of(
          inputs.principal_inertia.begin(), inputs.principal_inertia.end(),
          [](const double value) { return value <= 0.0; })) {
    throw std::invalid_argument("body force-response input is invalid");
  }

  std::array<long double, 3U> linear_delta{};
  std::array<long double, 3U> angular_delta{};
  const auto stored_slice =
      static_cast<long double>(stored_float32(inputs.slice_seconds));
  const auto stored_mass =
      static_cast<long double>(stored_float32(inputs.mass));
  const auto stored_response_scale = static_cast<long double>(
      stored_float32((1.0L / stored_mass) * stored_slice));
  for (std::size_t axis = 0U; axis < linear_delta.size(); ++axis) {
    linear_delta[axis] = static_cast<long double>(stored_float32(
                             inputs.accumulated_force.linear[axis])) *
                         stored_response_scale;
    angular_delta[axis] =
        (1.0L / static_cast<long double>(
                    stored_float32(inputs.principal_inertia[axis]))) *
        static_cast<long double>(
            stored_float32(inputs.accumulated_force.angular[axis])) *
        stored_slice;
  }
  for (std::size_t axis = 0U; axis < linear_delta.size(); ++axis) {
    state.local_linear[axis] = stored_float32(
        static_cast<long double>(stored_float32(state.local_linear[axis])) +
        static_cast<long double>(linear_delta[axis]));
    state.local_angular[axis] = stored_float32(
        static_cast<long double>(stored_float32(state.local_angular[axis])) +
        static_cast<long double>(angular_delta[axis]));
  }

  if (std::fabs(state.local_linear[0]) < 0.7 &&
      std::fabs(state.local_linear[2]) < 3.0) {
    state.local_linear[0] = 0.0;
  }
}

OriginalVehicleGroundedDampingResult apply_original_vehicle_grounded_damping(
    OriginalBodyVelocityState &state,
    const OriginalVehicleGroundedDampingProfile &profile,
    const double slice_seconds, const double linear_base_scale) {
  require_finite(state.local_linear,
                 "grounded vehicle linear velocity must be finite");
  require_finite(state.local_angular,
                 "grounded vehicle angular velocity must be finite");
  require_finite(profile.angular_bases,
                 "grounded vehicle angular damping bases must be finite");
  if (!std::isfinite(profile.lateral_linear_base) ||
      !std::isfinite(profile.longitudinal_linear_base) ||
      !std::isfinite(linear_base_scale) || !std::isfinite(slice_seconds) ||
      slice_seconds <= 0.0 || slice_seconds > 0.04 || linear_base_scale < 0.0 ||
      linear_base_scale > 1.0) {
    throw std::invalid_argument(
        "grounded vehicle damping input is invalid: lateral=" +
        std::to_string(profile.lateral_linear_base) +
        ", longitudinal=" + std::to_string(profile.longitudinal_linear_base) +
        ", angular=[" + std::to_string(profile.angular_bases[0U]) + "," +
        std::to_string(profile.angular_bases[1U]) + "," +
        std::to_string(profile.angular_bases[2U]) +
        "], linear_scale=" + std::to_string(linear_base_scale) +
        ", slice=" + std::to_string(slice_seconds));
  }

  OriginalVehicleGroundedDampingResult result;
  result.applied_profile = profile;
  result.linear_base_scale = linear_base_scale;
  result.input_velocity = state;
  result.linear_factors[0U] = original_signed_power(
      profile.lateral_linear_base * linear_base_scale, slice_seconds);
  result.linear_factors[2U] = original_signed_power(
      profile.longitudinal_linear_base * linear_base_scale, slice_seconds);
  for (std::size_t axis = 0U; axis < result.angular_factors.size(); ++axis) {
    result.angular_factors[axis] =
        original_signed_power(profile.angular_bases[axis], slice_seconds);
    state.local_angular[axis] =
        stored_float32(stored_float32(state.local_angular[axis]) *
                       result.angular_factors[axis]);
  }
  state.local_linear[0U] = stored_float32(
      stored_float32(state.local_linear[0U]) * result.linear_factors[0U]);
  state.local_linear[2U] = stored_float32(
      stored_float32(state.local_linear[2U]) * result.linear_factors[2U]);
  result.output_velocity = state;
  return result;
}

double calculate_original_vehicle_grounded_linear_base_scale(
    const double longitudinal_velocity, const double brake_force,
    const double authored_brake_factor, const std::uint8_t global_flags) {
  if (!std::isfinite(longitudinal_velocity) || !std::isfinite(brake_force) ||
      !std::isfinite(authored_brake_factor) || brake_force < 0.0 ||
      authored_brake_factor < 0.0 ||
      (brake_force > 0.0 && authored_brake_factor == 0.0)) {
    throw std::invalid_argument(
        "grounded vehicle linear-base scale input is invalid");
  }
  if ((global_flags & 0x01U) != 0U) {
    return 0.0;
  }
  if (brake_force == 0.0) {
    return 1.0;
  }

  const auto speed = std::fabs(longitudinal_velocity);
  const auto speed_attenuation = 1.0 - speed * 0.007;
  auto brake_scale = brake_force * speed_attenuation;
  if (speed < 6.0) {
    const auto authored_ratio = 1.1 / authored_brake_factor;
    const auto low_speed_blend =
        authored_ratio - ((authored_ratio - 1.0) / 6.0) * speed;
    brake_scale *= low_speed_blend;
  }
  return std::clamp(1.0 - brake_scale, 0.0, 1.0);
}

void apply_original_body_velocity_delta(
    OriginalBodyVelocityState &state,
    const CollisionVector3 &local_linear_delta,
    const CollisionVector3 &local_angular_delta) {
  require_finite(state.local_linear,
                 "local linear velocity state must be finite");
  require_finite(state.local_angular,
                 "local angular velocity state must be finite");
  require_finite(local_linear_delta, "local linear delta must be finite");
  require_finite(local_angular_delta, "local angular delta must be finite");

  for (std::size_t axis = 0U; axis < state.local_linear.size(); ++axis) {
    state.local_linear[axis] += local_linear_delta[axis];
    state.local_angular[axis] += local_angular_delta[axis];
  }
}

void accumulate_body_force_at_point(BodyForceAccumulator &accumulator,
                                    const CollisionVector3 &force,
                                    const CollisionVector3 &application_point,
                                    const CollisionVector3 &center_of_mass) {
  require_finite(accumulator.linear, "linear force accumulator must be finite");
  require_finite(accumulator.angular,
                 "angular force accumulator must be finite");
  require_finite(force, "body force must be finite");
  require_finite(application_point, "force application point must be finite");
  require_finite(center_of_mass, "center of mass must be finite");

  const CollisionVector3 stored_force{stored_float32(force[0]),
                                      stored_float32(force[1]),
                                      stored_float32(force[2])};
  const CollisionVector3 lever{
      stored_float32(stored_float32(application_point[0]) -
                     stored_float32(center_of_mass[0])),
      stored_float32(stored_float32(application_point[1]) -
                     stored_float32(center_of_mass[1])),
      stored_float32(stored_float32(application_point[2]) -
                     stored_float32(center_of_mass[2]))};
  const std::array<long double, 3U> torque{
      static_cast<long double>(lever[1U]) *
              static_cast<long double>(stored_force[2U]) -
          static_cast<long double>(lever[2U]) *
              static_cast<long double>(stored_force[1U]),
      static_cast<long double>(lever[2U]) *
              static_cast<long double>(stored_force[0U]) -
          static_cast<long double>(lever[0U]) *
              static_cast<long double>(stored_force[2U]),
      static_cast<long double>(lever[0U]) *
              static_cast<long double>(stored_force[1U]) -
          static_cast<long double>(lever[1U]) *
              static_cast<long double>(stored_force[0U])};
  for (std::size_t axis = 0U; axis < accumulator.linear.size(); ++axis) {
    accumulator.linear[axis] = stored_float32(
        static_cast<long double>(stored_float32(accumulator.linear[axis])) +
        static_cast<long double>(stored_force[axis]));
    accumulator.angular[axis] = stored_float32(
        static_cast<long double>(stored_float32(accumulator.angular[axis])) +
        torque[axis]);
  }
}

} // namespace mh::game
