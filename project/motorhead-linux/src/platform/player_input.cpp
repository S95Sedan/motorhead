#include <platform/sdl_input.hpp>
#include <platform/deck_controller.hpp>
namespace mh::platform {
std::optional<SdlControlBinding> parse_control_binding(std::string_view name) {
 auto pad=deck_binding_from_name(name); if(!pad) return std::nullopt; SdlControlBinding binding; binding.platform=pad; return binding;
}
std::optional<bool> platform_binding_down(const SdlControlBinding &binding) { if(!binding.platform) return std::nullopt; return deck_binding_strength(deck_gamepad(),binding.platform)>0.15; }
void augment_player_input(const SdlKeyboardBindings &bindings, SDL_Joystick *joystick, game::PlayerInputSources &sources) {
  const auto configured_pad = [](const SdlControlBinding &binding) {
    return static_cast<bool>(binding.platform) || binding.joystick != SdlJoystickBinding::none;
  };
  const bool custom_pad = configured_pad(bindings.turn_left) || configured_pad(bindings.turn_right) ||
      configured_pad(bindings.accelerate) || configured_pad(bindings.brake) ||
      configured_pad(bindings.handbrake) || configured_pad(bindings.shift_up) ||
      configured_pad(bindings.shift_down) || configured_pad(bindings.horn) ||
      configured_pad(bindings.rear_view);
  if (custom_pad) {
    const auto strength = [joystick](const SdlControlBinding &binding) {
      if (binding.platform) return deck_binding_strength(deck_gamepad(), binding.platform);
      int axis = -1, direction = 1;
      switch (binding.joystick) {
      case SdlJoystickBinding::left: axis = 0; direction = -1; break;
      case SdlJoystickBinding::right: axis = 0; break;
      case SdlJoystickBinding::up: axis = 1; direction = -1; break;
      case SdlJoystickBinding::down: axis = 1; break;
      case SdlJoystickBinding::in: axis = 2; direction = -1; break;
      case SdlJoystickBinding::out: axis = 2; break;
      default: return 0.0;
      }
      if (joystick == nullptr || axis >= SDL_GetNumJoystickAxes(joystick)) return 0.0;
      return std::clamp(direction * static_cast<double>(SDL_GetJoystickAxis(joystick, axis)) / 32767.0, 0.0, 1.0);
    };
    const auto analog = [](const SdlControlBinding &binding) {
      return binding.platform.axis != SDL_GAMEPAD_AXIS_INVALID ||
          (binding.joystick >= SdlJoystickBinding::left && binding.joystick <= SdlJoystickBinding::out);
    };
    if (analog(bindings.turn_left)) sources.controls.keyboard_left = false;
    if (analog(bindings.turn_right)) sources.controls.keyboard_right = false;
    if (analog(bindings.accelerate)) sources.controls.keyboard_throttle = false;
    if (analog(bindings.brake)) sources.controls.keyboard_brake = false;
    sources.controls.gamepad_steering = strength(bindings.turn_right) - strength(bindings.turn_left);
    sources.controls.gamepad_throttle = strength(bindings.accelerate);
    sources.controls.gamepad_brake = strength(bindings.brake);
  }
  if (auto *pad = deck_gamepad(); pad != nullptr && !custom_pad) {
    const auto button = [pad](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); };
    const auto trigger = [pad](SDL_GamepadAxis a) {
      const double value = static_cast<double>(SDL_GetGamepadAxis(pad, a)) / 32767.0;
      return value < 0.05 ? 0.0 : std::clamp(value, 0.0, 1.0);
    };
    sources.controls.gamepad_steering = std::clamp(
        static_cast<double>(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX)) / 32767.0, -1.0, 1.0);
    sources.controls.gamepad_throttle = trigger(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    sources.controls.gamepad_brake = trigger(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    sources.controls.gamepad_handbrake = button(SDL_GAMEPAD_BUTTON_SOUTH);
    sources.controls.keyboard_left |= button(SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    sources.controls.keyboard_right |= button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    sources.rear_view |= button(SDL_GAMEPAD_BUTTON_EAST);
    sources.horn |= button(SDL_GAMEPAD_BUTTON_WEST);
    sources.toggle_camera |= button(SDL_GAMEPAD_BUTTON_NORTH);
    sources.shift_up |= button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    sources.shift_down |= button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    sources.retail_in_car_view |= button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    sources.retail_out_car_view |= button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    sources.retail_camera_view |= button(SDL_GAMEPAD_BUTTON_NORTH);
    sources.retail_cycle_players |= button(SDL_GAMEPAD_BUTTON_WEST);
  }
}
}
