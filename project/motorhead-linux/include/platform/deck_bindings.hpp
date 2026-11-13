#pragma once

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <string_view>

namespace mh::platform {

// Stable SDL gamepad names, independent of the controller's raw button order.
struct DeckBinding {
  SDL_GamepadAxis axis = SDL_GAMEPAD_AXIS_INVALID;
  int direction = 1;
  SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_INVALID;
  explicit operator bool() const {
    return axis != SDL_GAMEPAD_AXIS_INVALID || button != SDL_GAMEPAD_BUTTON_INVALID;
  }
};

struct NamedDeckBinding { std::string_view name; DeckBinding binding; };
inline constexpr std::array deck_bindings{
  NamedDeckBinding{"PadLeft", {SDL_GAMEPAD_AXIS_LEFTX, -1}},
  NamedDeckBinding{"PadRight", {SDL_GAMEPAD_AXIS_LEFTX, 1}},
  NamedDeckBinding{"PadUp", {SDL_GAMEPAD_AXIS_LEFTY, -1}},
  NamedDeckBinding{"PadDown", {SDL_GAMEPAD_AXIS_LEFTY, 1}},
  NamedDeckBinding{"PadRLeft", {SDL_GAMEPAD_AXIS_RIGHTX, -1}},
  NamedDeckBinding{"PadRRight", {SDL_GAMEPAD_AXIS_RIGHTX, 1}},
  NamedDeckBinding{"PadRUp", {SDL_GAMEPAD_AXIS_RIGHTY, -1}},
  NamedDeckBinding{"PadRDown", {SDL_GAMEPAD_AXIS_RIGHTY, 1}},
  NamedDeckBinding{"PadLT", {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1}},
  NamedDeckBinding{"PadRT", {SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1}},
  NamedDeckBinding{"PadA", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_SOUTH}},
  NamedDeckBinding{"PadB", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_EAST}},
  NamedDeckBinding{"PadX", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_WEST}},
  NamedDeckBinding{"PadY", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_NORTH}},
  NamedDeckBinding{"PadLB", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}},
  NamedDeckBinding{"PadRB", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}},
  NamedDeckBinding{"PadLStick", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_LEFT_STICK}},
  NamedDeckBinding{"PadRStick", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_RIGHT_STICK}},
  NamedDeckBinding{"PadStart", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_START}},
  NamedDeckBinding{"PadBack", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_BACK}},
  NamedDeckBinding{"PadDLeft", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_DPAD_LEFT}},
  NamedDeckBinding{"PadDRight", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_DPAD_RIGHT}},
  NamedDeckBinding{"PadDUp", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_DPAD_UP}},
  NamedDeckBinding{"PadDDown", {SDL_GAMEPAD_AXIS_INVALID, 1, SDL_GAMEPAD_BUTTON_DPAD_DOWN}},
};

inline DeckBinding deck_binding_from_name(std::string_view name) {
  for (const auto &entry : deck_bindings) if (entry.name == name) return entry.binding;
  return {};
}

inline std::string_view deck_binding_from_event(const SDL_Event &event) {
  DeckBinding binding;
  if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
    if (event.gaxis.value > -16000 && event.gaxis.value < 16000) return {};
    binding.axis = static_cast<SDL_GamepadAxis>(event.gaxis.axis);
    binding.direction = event.gaxis.value < 0 ? -1 : 1;
  } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
    binding.button = static_cast<SDL_GamepadButton>(event.gbutton.button);
  } else return {};
  for (const auto &entry : deck_bindings) {
    if (entry.binding.axis == binding.axis && entry.binding.direction == binding.direction &&
        entry.binding.button == binding.button) return entry.name;
  }
  return {};
}

inline double deck_binding_strength(SDL_Gamepad *pad, DeckBinding binding) {
  if (pad == nullptr) return 0.0;
  if (binding.button != SDL_GAMEPAD_BUTTON_INVALID)
    return SDL_GetGamepadButton(pad, binding.button) ? 1.0 : 0.0;
  if (binding.axis == SDL_GAMEPAD_AXIS_INVALID) return 0.0;
  const double value = std::clamp(binding.direction *
      static_cast<double>(SDL_GetGamepadAxis(pad, binding.axis)) / 32767.0, 0.0, 1.0);
  return value < 0.05 ? 0.0 : value;
}

} // namespace mh::platform
