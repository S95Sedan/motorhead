#include <game/physics/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace mh::game {
namespace {

constexpr double pi = 3.141592653589793238462643383279502884;

void require_finite(const double value, const char *name) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(name) + " must be finite");
  }
}

void validate_parameters(const VehicleParameters &parameters) {
  require_finite(parameters.forward_acceleration, "forward acceleration");
  require_finite(parameters.reverse_acceleration, "reverse acceleration");
  require_finite(parameters.braking_deceleration, "braking deceleration");
  require_finite(parameters.linear_drag, "linear drag");
  require_finite(parameters.wheelbase, "wheelbase");
  require_finite(parameters.maximum_steering_angle, "maximum steering angle");
  require_finite(parameters.maximum_forward_speed, "maximum forward speed");
  require_finite(parameters.maximum_reverse_speed, "maximum reverse speed");
  if (parameters.forward_acceleration < 0.0 ||
      parameters.reverse_acceleration < 0.0 ||
      parameters.braking_deceleration < 0.0 || parameters.linear_drag < 0.0 ||
      parameters.wheelbase <= 0.0 || parameters.maximum_steering_angle < 0.0 ||
      parameters.maximum_forward_speed <= 0.0 ||
      parameters.maximum_reverse_speed < 0.0) {
    throw std::invalid_argument(
        "vehicle parameters are outside their valid range");
  }
}

double normalize_heading(const double value) {
  return std::remainder(value, 2.0 * pi);
}

} // namespace

float clamp_original_frame_elapsed_seconds(const double elapsed_seconds) {
  require_finite(elapsed_seconds, "frame elapsed seconds");
  return std::clamp(static_cast<float>(elapsed_seconds),
                    original_minimum_frame_elapsed_seconds,
                    original_maximum_frame_elapsed_seconds);
}

OriginalPhysicsSliceSchedule
make_original_physics_slice_schedule(const double elapsed_seconds) {
  OriginalPhysicsSliceSchedule schedule;
  schedule.frame_elapsed_seconds =
      clamp_original_frame_elapsed_seconds(elapsed_seconds);

  auto remaining = schedule.frame_elapsed_seconds;
  while (remaining > 0.0F &&
         schedule.slice_count < original_maximum_physics_slice_count) {
    const auto slice = remaining > original_maximum_physics_slice_seconds
                           ? original_maximum_physics_slice_seconds
                           : remaining;
    schedule.slices[schedule.slice_count++] = slice;
    schedule.simulated_seconds += slice;

    // p3.1 subtracts the full 0.04f after every pass, including the final
    // partial pass, and exits when the remaining value is no longer positive.
    remaining -= original_maximum_physics_slice_seconds;
  }
  schedule.discarded_seconds = std::max(remaining, 0.0F);
  return schedule;
}

FixedStepClock::FixedStepClock(const SimulationDuration step) : step_(step) {
  if (step_.count() <= 0) {
    throw std::invalid_argument("fixed simulation step must be positive");
  }
}

std::uint64_t FixedStepClock::consume(const SimulationDuration elapsed) {
  if (elapsed.count() < 0) {
    throw std::invalid_argument("elapsed simulation time cannot be negative");
  }
  if (elapsed > SimulationDuration::max() - remainder_) {
    throw std::overflow_error("fixed-step accumulator overflow");
  }
  remainder_ += elapsed;
  const auto due = remainder_.count() / step_.count();
  remainder_ -= step_ * due;
  return static_cast<std::uint64_t>(due);
}

SimulationDuration FixedStepClock::step() const noexcept { return step_; }

SimulationDuration FixedStepClock::remainder() const noexcept {
  return remainder_;
}

double FixedStepClock::interpolation_alpha() const noexcept {
  return static_cast<double>(remainder_.count()) /
         static_cast<double>(step_.count());
}

void FixedStepClock::reset() noexcept {
  remainder_ = SimulationDuration::zero();
}

VehicleSimulation::VehicleSimulation(const VehicleParameters parameters)
    : parameters_(parameters) {
  validate_parameters(parameters_);
}

void VehicleSimulation::reset(const VehicleState &state) {
  require_finite(state.position_x, "position x");
  require_finite(state.position_z, "position z");
  require_finite(state.heading_radians, "heading");
  require_finite(state.speed, "speed");
  if (state.speed > parameters_.maximum_forward_speed ||
      state.speed < -parameters_.maximum_reverse_speed) {
    throw std::invalid_argument(
        "initial vehicle speed is outside configured limits");
  }
  state_ = state;
  state_.heading_radians = normalize_heading(state_.heading_radians);
}

void VehicleSimulation::step(const ControlInput &input) {
  require_finite(input.throttle, "throttle");
  require_finite(input.brake, "brake");
  require_finite(input.steering, "steering");
  const auto throttle = std::clamp(input.throttle, -1.0, 1.0);
  const auto brake = std::clamp(input.brake, 0.0, 1.0);
  const auto steering = std::clamp(input.steering, -1.0, 1.0);
  const auto seconds = std::chrono::duration<double>(fixed_step).count();

  auto acceleration = throttle >= 0.0
                          ? throttle * parameters_.forward_acceleration
                          : throttle * parameters_.reverse_acceleration;
  acceleration -= parameters_.linear_drag * state_.speed;
  if (state_.speed > 0.0) {
    acceleration -= brake * parameters_.braking_deceleration;
  } else if (state_.speed < 0.0) {
    acceleration += brake * parameters_.braking_deceleration;
  }

  auto speed = state_.speed + acceleration * seconds;
  if (brake > 0.0 && throttle == 0.0 &&
      ((state_.speed > 0.0 && speed < 0.0) ||
       (state_.speed < 0.0 && speed > 0.0))) {
    speed = 0.0;
  }
  speed = std::clamp(speed, -parameters_.maximum_reverse_speed,
                     parameters_.maximum_forward_speed);

  const auto steering_angle = steering * parameters_.maximum_steering_angle;
  const auto yaw_rate =
      speed / parameters_.wheelbase * std::tan(steering_angle);
  const auto heading =
      normalize_heading(state_.heading_radians + yaw_rate * seconds);
  state_.position_x += std::sin(heading) * speed * seconds;
  state_.position_z += std::cos(heading) * speed * seconds;
  state_.heading_radians = heading;
  state_.speed = speed;
  ++state_.tick;
}

const VehicleParameters &VehicleSimulation::parameters() const noexcept {
  return parameters_;
}

const VehicleState &VehicleSimulation::state() const noexcept { return state_; }

} // namespace mh::game
