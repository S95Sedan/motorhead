#include <game/vehicle/drive.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace mh::game {

void VehicleLaunchTractionRuntime::begin(const double engine_scalar,
                                         const double minimum_rpm,
                                         const double maximum_rpm) {
  if (!std::isfinite(engine_scalar) || !std::isfinite(minimum_rpm) ||
      !std::isfinite(maximum_rpm) || minimum_rpm < 0.0 ||
      maximum_rpm <= minimum_rpm) {
    throw std::invalid_argument("vehicle launch traction input is invalid");
  }
  constexpr double launch_window_seconds = 1.35;
  remaining_seconds_ = launch_window_seconds;
  engine_preload_ = std::clamp(
      (engine_scalar - minimum_rpm) / (maximum_rpm - minimum_rpm), 0.0, 1.0);
}

VehicleLaunchTractionFrame
VehicleLaunchTractionRuntime::step(const ControlInput &controls,
                                   const double longitudinal_velocity,
                                   const double slice_seconds) {
  if (!std::isfinite(controls.throttle) || !std::isfinite(controls.brake) ||
      !std::isfinite(controls.steering) ||
      !std::isfinite(longitudinal_velocity) || !std::isfinite(slice_seconds) ||
      slice_seconds <= 0.0) {
    throw std::invalid_argument("vehicle launch traction step is invalid");
  }

  VehicleLaunchTractionFrame result{controls, 0.0};
  if (remaining_seconds_ <= 0.0 || controls.brake > 0.1 || controls.handbrake) {
    remaining_seconds_ = std::max(0.0, remaining_seconds_ - slice_seconds);
    return result;
  }

  constexpr double slip_throttle = 0.62;
  constexpr double slip_speed_limit = 13.0;
  constexpr double fade_seconds = 0.55;
  constexpr double maximum_traction_loss = 0.52;
  const auto requested_throttle = std::clamp(controls.throttle, 0.0, 1.0);
  const auto excess_request = std::clamp(
      (requested_throttle - slip_throttle) / (1.0 - slip_throttle), 0.0, 1.0);
  const auto speed_factor = std::clamp(
      1.0 - std::abs(longitudinal_velocity) / slip_speed_limit, 0.0, 1.0);
  const auto time_factor =
      std::clamp(remaining_seconds_ / fade_seconds, 0.0, 1.0);
  const auto preload_factor = 0.45 + engine_preload_ * 0.55;
  result.wheelspin =
      excess_request * speed_factor * time_factor * preload_factor;
  result.controls.throttle =
      controls.throttle * (1.0 - maximum_traction_loss * result.wheelspin);
  remaining_seconds_ = std::max(0.0, remaining_seconds_ - slice_seconds);
  return result;
}

void VehicleLaunchTractionRuntime::reset() noexcept {
  remaining_seconds_ = 0.0;
  engine_preload_ = 0.0;
}

bool VehicleLaunchTractionRuntime::active() const noexcept {
  return remaining_seconds_ > 0.0;
}

OriginalVehicleDriveSystem::OriginalVehicleDriveSystem(
    RecoveredVehicleRuntimeTuning tuning)
    : base_tuning_(tuning), tuning_(std::move(tuning)) {
  const auto valid_gears =
      tuning_.gear_values.size() >= 2U &&
      std::all_of(tuning_.gear_values.begin(), tuning_.gear_values.end(),
                  [](const float value) {
                    return std::isfinite(value) && value > 0.0F;
                  });
  if (!valid_gears || !std::isfinite(tuning_.minimum_rpm) ||
      !std::isfinite(tuning_.maximum_rpm) ||
      !std::isfinite(tuning_.acceleration_force) ||
      !std::isfinite(tuning_.brake_force) ||
      !std::isfinite(tuning_.turn_force) || tuning_.minimum_rpm < 0.0F ||
      tuning_.maximum_rpm <= tuning_.minimum_rpm ||
      tuning_.acceleration_force < 0.0F || tuning_.brake_force < 0.0F ||
      tuning_.turn_force < 0.0F) {
    throw std::invalid_argument("vehicle drive tuning is invalid");
  }
  reset();
}

OriginalVehicleModeCSceneStepInputs
OriginalVehicleDriveSystem::prepare_step(const ControlInput &controls,
                                         const double longitudinal_velocity,
                                         const double slice_seconds) {
  const auto finite =
      std::isfinite(controls.throttle) && std::isfinite(controls.brake) &&
      std::isfinite(controls.steering) &&
      std::isfinite(longitudinal_velocity) && std::isfinite(slice_seconds);
  // Retail AI steering is not bounded at its final control handoff. The
  // recovered drivetrain is the normalization owner: it clamps both scalar
  // controls and the wheel-primary path clamps steering. Reject only
  // non-finite values here so those exact downstream rules remain reachable.
  if (!finite || slice_seconds <= 0.0) {
    throw std::invalid_argument("vehicle drive step input is invalid");
  }

  OriginalDrivetrainModeCForceInputs drivetrain{};
  const auto gear_index =
      std::min(state_.current_gear_index, tuning_.gear_values.size() - 1U);
  const auto first_shift_latched =
      automatic_transmission_ ? true : state_.first_shift_latched;
  const auto second_shift_latched =
      automatic_transmission_ ? true : state_.second_shift_latched;
  drivetrain.state = {
      longitudinal_velocity,
      controls.throttle,
      controls.brake,
      tuning_.gear_values[gear_index],
      state_.engine_scalar,
      tuning_.minimum_rpm,
      tuning_.maximum_rpm,
      slice_seconds,
      gear_index,
      tuning_.gear_values.size() - 1U,
      first_shift_latched,
      second_shift_latched,
      state_.direction_transition_active,
      true,
      true,
      false,
  };
  state_.steering_scalar = calculate_original_retained_steering(
      {state_.steering_scalar, controls.steering});
  // Body +0xd8 retains the smoothed presentation/MDE state. The recovered
  // wheel-primary producer independently consumes the raw requested control
  // and applies its own clamp and speed ramp.
  drivetrain.steering = controls.steering;
  drivetrain.authored_brake_force = tuning_.brake_force;
  drivetrain.authored_turn_force = tuning_.turn_force;
  drivetrain.steering_ramp_limit = 4.0;
  drivetrain.authored_acceleration_force = tuning_.acceleration_force;
  drivetrain.acceleration_selector = tuning_.acceleration_selector;
  drivetrain.engine_transition_active = state_.engine_transition_active;
  if (!automatic_transmission_) {
    if (controls.shift_up) {
      drivetrain.control_flags |= 0x10U;
    }
    if (controls.shift_down) {
      drivetrain.control_flags |= 0x20U;
    }
  }
  const auto candidate_gear_index =
      calculate_original_drivetrain_candidate_gear(
          {gear_index, tuning_.gear_values.size() - 1U, first_shift_latched,
           second_shift_latched});
  drivetrain.candidate_gear_value = tuning_.gear_values[candidate_gear_index];
  drivetrain.reference_value = 3000.0;
  drivetrain.manual_mode = !automatic_transmission_;
  OriginalVehicleModeCSceneStepInputs result;
  result.drivetrain = drivetrain;
  result.grip_response_constants = tuning_.grip_response_constants;
  result.handbrake_active = controls.handbrake;
  return result;
}

void OriginalVehicleDriveSystem::commit_step(
    const OriginalDrivetrainModeCForceResult &result) {
  if (result.state.current_gear_index >= tuning_.gear_values.size() ||
      !std::isfinite(result.state.engine_scalar) ||
      result.state.engine_scalar < 0.0) {
    throw std::invalid_argument("vehicle drive result is invalid");
  }
  state_.current_gear_index = result.state.current_gear_index;
  state_.engine_scalar =
      static_cast<double>(static_cast<float>(result.state.engine_scalar));
  state_.engine_transition_active = result.state.engine_transition_active;
  state_.direction_transition_active = result.state.direction_transition_active;
  state_.first_shift_latched = result.state.first_shift_latched;
  state_.second_shift_latched = result.state.second_shift_latched;
}

void OriginalVehicleDriveSystem::advance_state_only(
    const ControlInput &controls, const double longitudinal_velocity,
    const double slice_seconds) {
  const auto input =
      prepare_step(controls, longitudinal_velocity, slice_seconds);
  if (!automatic_transmission_) {
    // Race staging holds the body at zero speed, so committing the ordinary
    // manual speed candidate here would pin RPM to minimum+1 after the first
    // update. Retain only the manual release latches/gear while the shared
    // grid-preload owner below advances the engine from live accelerator input.
    const auto manual_gear = calculate_original_drivetrain_manual_gear(
        {input.drivetrain.control_flags, longitudinal_velocity,
         state_.current_gear_index, tuning_.gear_values.size() - 1U,
         state_.first_shift_latched, state_.second_shift_latched});
    state_.current_gear_index = manual_gear.current_gear_index;
    state_.first_shift_latched = manual_gear.first_shift_latched;
    state_.second_shift_latched = manual_gear.second_shift_latched;
  }
  const auto direction_transition_active =
      input.drivetrain.state.control_b > 0.9;
  const auto control_a =
      direction_transition_active ? 0.0 : input.drivetrain.state.control_a;
  const auto engine = calculate_original_drivetrain_engine_state(
      {state_.engine_scalar, state_.engine_scalar, tuning_.minimum_rpm,
       tuning_.maximum_rpm, slice_seconds, std::clamp(control_a, 0.0, 1.0),
       false, false, false, false, false});
  state_.engine_scalar =
      static_cast<double>(static_cast<float>(engine.engine_scalar));
  state_.engine_transition_active = engine.transition_active;
  state_.direction_transition_active = direction_transition_active;
}

void OriginalVehicleDriveSystem::apply_performance_scale(const float factor) {
  if (!std::isfinite(factor) || factor < 0.0F) {
    throw std::invalid_argument("vehicle performance scale is invalid");
  }
  tuning_.gear_values = base_tuning_.gear_values;
  for (std::size_t index = 1U; index < tuning_.gear_values.size(); ++index) {
    tuning_.gear_values[index] = static_cast<float>(
        static_cast<double>(base_tuning_.gear_values[index]) *
        static_cast<double>(factor));
  }
  tuning_.acceleration_force =
      static_cast<float>(static_cast<double>(base_tuning_.acceleration_force) *
                         static_cast<double>(factor));
}

void OriginalVehicleDriveSystem::set_automatic_transmission(
    const bool automatic) noexcept {
  automatic_transmission_ = automatic;
  state_.first_shift_latched = true;
  state_.second_shift_latched = true;
}

void OriginalVehicleDriveSystem::seed_captured_state(
    const OriginalVehicleDriveState &state) {
  if (state.current_gear_index >= tuning_.gear_values.size() ||
      !std::isfinite(state.engine_scalar) || state.engine_scalar < 0.0 ||
      !std::isfinite(state.steering_scalar)) {
    throw std::invalid_argument("captured vehicle drive state is invalid");
  }
  state_ = state;
}

void OriginalVehicleDriveSystem::reset() noexcept {
  tuning_ = base_tuning_;
  reset_retained_state();
}

void OriginalVehicleDriveSystem::reset_retained_state() noexcept {
  state_.current_gear_index = tuning_.gear_values.size() > 1U ? 1U : 0U;
  state_.engine_scalar = static_cast<double>(tuning_.minimum_rpm);
  state_.steering_scalar = 0.0;
  state_.engine_transition_active = false;
  state_.direction_transition_active = false;
  state_.first_shift_latched = true;
  state_.second_shift_latched = true;
}

const RecoveredVehicleRuntimeTuning &
OriginalVehicleDriveSystem::tuning() const noexcept {
  return tuning_;
}

const OriginalVehicleDriveState &
OriginalVehicleDriveSystem::state() const noexcept {
  return state_;
}

std::optional<double>
OriginalVehicleDriveSystem::active_manual_speed_ceiling() const noexcept {
  if (automatic_transmission_ ||
      state_.current_gear_index >= tuning_.gear_values.size()) {
    return std::nullopt;
  }
  constexpr double kilometres_per_hour_per_metre_per_second = 3.6;
  return static_cast<double>(tuning_.gear_values[state_.current_gear_index]) /
         kilometres_per_hour_per_metre_per_second;
}

} // namespace mh::game
