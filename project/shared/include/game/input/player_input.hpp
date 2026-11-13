#pragma once

#include <game/physics/simulation.hpp>

namespace mh::game {

// Platform adapters populate this neutral state. Keyboard values are digital;
// gamepad throttle/brake use [0, 1] and steering uses [-1, 1]. Keeping SDL out
// of mh_game makes recorded inputs and headless replay use the same mapping.
struct PlayerControlSources {
  bool keyboard_throttle = false;
  bool keyboard_brake = false;
  bool keyboard_left = false;
  bool keyboard_right = false;
  bool keyboard_handbrake = false;
  double gamepad_throttle = 0.0;
  double gamepad_brake = 0.0;
  double gamepad_steering = 0.0;
  bool gamepad_handbrake = false;
};

struct PlayerControlMapping {
  double steering_deadzone = 0.15;
  // Recovered p3.1 digital steering begins at half lock and changes by about
  // 0.6 lock units per second while held.
  double keyboard_steering_initial = 0.5;
  double keyboard_steering_rate = 0.6;
};

// One platform sample combines continuous driving controls with logical
// actions. Platform adapters combine bindings (for example keyboard Enter and
// a gamepad Start button) into these booleans.
struct PlayerInputSources {
  PlayerControlSources controls{};
  bool horn = false;
  bool rear_view = false;
  bool shift_up = false;
  bool shift_down = false;
  bool start = false;
  bool restart = false;
  bool toggle_camera = false;
  bool select_in_car_camera = false;
  bool select_out_car_camera = false;
  bool select_bumper_camera = false;
  bool select_far_camera = false;
  // Retail CLO actions which are fixed (not editable on the Control Options
  // page). Replay Race consumes these instead of the reconstruction's normal
  // F1..F4/C playable-camera shortcuts.
  bool retail_in_car_view = false;
  bool retail_out_car_view = false;
  bool retail_camera_view = false;
  bool retail_cycle_players = false;
};

// Actions are rising-edge pulses consumed by exactly one simulation tick.
// Continuous controls remain active on every tick while their source is held.
struct PlayerTickInput {
  ControlInput controls{};
  bool horn_held = false;
  bool start_pressed = false;
  bool restart_pressed = false;
  bool toggle_camera_pressed = false;
  bool select_in_car_camera_pressed = false;
  bool select_out_car_camera_pressed = false;
  bool select_bumper_camera_pressed = false;
  bool select_far_camera_pressed = false;
};

// Digital throttle/brake take the maximum of their analog counterparts.
// Opposing keyboard steering cancels; otherwise a pressed keyboard direction
// takes precedence over the analog stick. The stick deadzone is rescaled so
// full deflection remains exactly +/-1.
[[nodiscard]] ControlInput
map_player_controls(const PlayerControlSources &sources,
                    const PlayerControlMapping &mapping = {});

// Bridges render/event-rate input sampling to simulation slices. Rising edges
// are latched until a slice consumes them, so a quick button press is not lost
// when no physics step is due during the render frame.
class PlayerInputRouter {
public:
  explicit PlayerInputRouter(PlayerControlMapping mapping = {});

  void update(const PlayerInputSources &sources);
  [[nodiscard]] PlayerTickInput consume_tick() noexcept;
  [[nodiscard]] PlayerTickInput
  consume_tick(SimulationDuration elapsed) noexcept;
  void reset() noexcept;

private:
  PlayerControlMapping mapping_{};
  ControlInput controls_{};
  bool horn_held_ = false;
  bool shift_up_held_ = false;
  bool shift_down_held_ = false;
  bool previous_start_ = false;
  bool previous_restart_ = false;
  bool previous_toggle_camera_ = false;
  bool previous_select_in_car_camera_ = false;
  bool previous_select_out_car_camera_ = false;
  bool previous_select_bumper_camera_ = false;
  bool previous_select_far_camera_ = false;
  bool pending_start_ = false;
  bool pending_restart_ = false;
  bool pending_toggle_camera_ = false;
  bool pending_select_in_car_camera_ = false;
  bool pending_select_out_car_camera_ = false;
  bool pending_select_bumper_camera_ = false;
  bool pending_select_far_camera_ = false;
  int keyboard_steering_direction_ = 0;
  int active_keyboard_steering_direction_ = 0;
  double keyboard_steering_ = 0.0;
};

} // namespace mh::game
