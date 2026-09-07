#include <game/input/player_input.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mh::game {
namespace {

void require_finite(const double value, const std::string_view label) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(label) + " must be finite");
  }
}

double rescale_deadzone(const double value, const double deadzone) {
  const auto magnitude = std::abs(value);
  if (magnitude <= deadzone) {
    return 0.0;
  }
  return std::copysign((magnitude - deadzone) / (1.0 - deadzone), value);
}

} // namespace

ControlInput map_player_controls(const PlayerControlSources &sources,
                                 const PlayerControlMapping &mapping) {
  require_finite(sources.gamepad_throttle, "gamepad throttle");
  require_finite(sources.gamepad_brake, "gamepad brake");
  require_finite(sources.gamepad_steering, "gamepad steering");
  require_finite(mapping.steering_deadzone, "steering deadzone");
  require_finite(mapping.keyboard_steering_initial,
                 "keyboard steering initial");
  require_finite(mapping.keyboard_steering_rate, "keyboard steering rate");
  if (sources.gamepad_throttle < 0.0 || sources.gamepad_throttle > 1.0 ||
      sources.gamepad_brake < 0.0 || sources.gamepad_brake > 1.0 ||
      sources.gamepad_steering < -1.0 || sources.gamepad_steering > 1.0) {
    throw std::invalid_argument(
        "gamepad controls are outside canonical ranges");
  }
  if (mapping.steering_deadzone < 0.0 || mapping.steering_deadzone >= 1.0 ||
      mapping.keyboard_steering_initial < 0.0 ||
      mapping.keyboard_steering_initial > 1.0 ||
      mapping.keyboard_steering_rate < 0.0) {
    throw std::invalid_argument("player control mapping is outside its range");
  }

  const auto keyboard_steering = static_cast<int>(sources.keyboard_right) -
                                 static_cast<int>(sources.keyboard_left);
  const auto analog_steering =
      rescale_deadzone(sources.gamepad_steering, mapping.steering_deadzone);
  return {
      std::max(sources.keyboard_throttle ? 1.0 : 0.0, sources.gamepad_throttle),
      std::max(sources.keyboard_brake ? 1.0 : 0.0, sources.gamepad_brake),
      keyboard_steering == 0 ? analog_steering
                             : static_cast<double>(keyboard_steering),
      sources.keyboard_handbrake || sources.gamepad_handbrake};
}

PlayerInputRouter::PlayerInputRouter(const PlayerControlMapping mapping)
    : mapping_(mapping) {
  static_cast<void>(map_player_controls({}, mapping_));
}

void PlayerInputRouter::update(const PlayerInputSources &sources) {
  controls_ = map_player_controls(sources.controls, mapping_);
  horn_held_ = sources.horn;
  shift_up_held_ = sources.shift_up;
  shift_down_held_ = sources.shift_down;
  keyboard_steering_direction_ =
      static_cast<int>(sources.controls.keyboard_right) -
      static_cast<int>(sources.controls.keyboard_left);
  pending_start_ = pending_start_ || (sources.start && !previous_start_);
  pending_restart_ =
      pending_restart_ || (sources.restart && !previous_restart_);
  pending_toggle_camera_ = pending_toggle_camera_ ||
                           (sources.toggle_camera && !previous_toggle_camera_);
  pending_select_in_car_camera_ =
      pending_select_in_car_camera_ ||
      (sources.select_in_car_camera && !previous_select_in_car_camera_);
  pending_select_out_car_camera_ =
      pending_select_out_car_camera_ ||
      (sources.select_out_car_camera && !previous_select_out_car_camera_);
  pending_select_bumper_camera_ =
      pending_select_bumper_camera_ ||
      (sources.select_bumper_camera && !previous_select_bumper_camera_);
  pending_select_far_camera_ =
      pending_select_far_camera_ ||
      (sources.select_far_camera && !previous_select_far_camera_);
  previous_start_ = sources.start;
  previous_restart_ = sources.restart;
  previous_toggle_camera_ = sources.toggle_camera;
  previous_select_in_car_camera_ = sources.select_in_car_camera;
  previous_select_out_car_camera_ = sources.select_out_car_camera;
  previous_select_bumper_camera_ = sources.select_bumper_camera;
  previous_select_far_camera_ = sources.select_far_camera;
}

PlayerTickInput PlayerInputRouter::consume_tick() noexcept {
  return consume_tick(
      std::chrono::duration_cast<SimulationDuration>(fixed_step));
}

PlayerTickInput
PlayerInputRouter::consume_tick(const SimulationDuration elapsed) noexcept {
  auto tick_controls = controls_;
  tick_controls.shift_up = shift_up_held_;
  tick_controls.shift_down = shift_down_held_;
  if (keyboard_steering_direction_ != 0) {
    if (keyboard_steering_direction_ != active_keyboard_steering_direction_) {
      active_keyboard_steering_direction_ = keyboard_steering_direction_;
      keyboard_steering_ = static_cast<double>(keyboard_steering_direction_) *
                           mapping_.keyboard_steering_initial;
    } else {
      const auto seconds =
          std::max(0.0, std::chrono::duration<double>(elapsed).count());
      keyboard_steering_ += static_cast<double>(keyboard_steering_direction_) *
                            mapping_.keyboard_steering_rate * seconds;
      keyboard_steering_ = std::clamp(keyboard_steering_, -1.0, 1.0);
    }
    tick_controls.steering = keyboard_steering_;
  } else {
    active_keyboard_steering_direction_ = 0;
    keyboard_steering_ = 0.0;
  }
  const PlayerTickInput tick{
      tick_controls,
      horn_held_,
      pending_start_,
      pending_restart_,
      pending_toggle_camera_,
      pending_select_in_car_camera_,
      pending_select_out_car_camera_,
      pending_select_bumper_camera_,
      pending_select_far_camera_,
  };
  pending_start_ = false;
  pending_restart_ = false;
  pending_toggle_camera_ = false;
  pending_select_in_car_camera_ = false;
  pending_select_out_car_camera_ = false;
  pending_select_bumper_camera_ = false;
  pending_select_far_camera_ = false;
  return tick;
}

void PlayerInputRouter::reset() noexcept {
  controls_ = {};
  horn_held_ = false;
  shift_up_held_ = false;
  shift_down_held_ = false;
  previous_start_ = false;
  previous_restart_ = false;
  previous_toggle_camera_ = false;
  previous_select_in_car_camera_ = false;
  previous_select_out_car_camera_ = false;
  previous_select_bumper_camera_ = false;
  previous_select_far_camera_ = false;
  pending_start_ = false;
  pending_restart_ = false;
  pending_toggle_camera_ = false;
  pending_select_in_car_camera_ = false;
  pending_select_out_car_camera_ = false;
  pending_select_bumper_camera_ = false;
  pending_select_far_camera_ = false;
  keyboard_steering_direction_ = 0;
  active_keyboard_steering_direction_ = 0;
  keyboard_steering_ = 0.0;
}

} // namespace mh::game
