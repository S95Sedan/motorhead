#include <platform/sdl_input.hpp>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <algorithm>

namespace mh::platform {
namespace {} // namespace

SdlPlayerInput::SdlPlayerInput(const SdlKeyboardBindings bindings)
    : bindings_(bindings) {
  const auto uses_mouse_motion = [](const SdlControlBinding &binding) {
    return binding.mouse == SdlMouseBinding::left ||
           binding.mouse == SdlMouseBinding::right ||
           binding.mouse == SdlMouseBinding::up ||
           binding.mouse == SdlMouseBinding::down;
  };
  const auto mouse_motion_selected = uses_mouse_motion(bindings_.turn_left) ||
                                     uses_mouse_motion(bindings_.turn_right) ||
                                     uses_mouse_motion(bindings_.accelerate) ||
                                     uses_mouse_motion(bindings_.brake) ||
                                     uses_mouse_motion(bindings_.shift_up) ||
                                     uses_mouse_motion(bindings_.shift_down) ||
                                     uses_mouse_motion(bindings_.handbrake) ||
                                     uses_mouse_motion(bindings_.rear_view) ||
                                     uses_mouse_motion(bindings_.horn) ||
                                     uses_mouse_motion(bindings_.in_car_view) ||
                                     uses_mouse_motion(bindings_.out_car_view) ||
                                     uses_mouse_motion(bindings_.camera_view) ||
                                     uses_mouse_motion(bindings_.cycle_players);
  if (mouse_motion_selected) {
    relative_mouse_window_ = SDL_GetMouseFocus();
    if (relative_mouse_window_ != nullptr &&
        !SDL_GetWindowRelativeMouseMode(relative_mouse_window_)) {
      restore_relative_mouse_mode_ =
          SDL_SetWindowRelativeMouseMode(relative_mouse_window_, true);
    }
  }
  open_first_joystick();
}

SdlPlayerInput::~SdlPlayerInput() {
  close_joystick();
  if (restore_relative_mouse_mode_ && relative_mouse_window_ != nullptr) {
    static_cast<void>(
        SDL_SetWindowRelativeMouseMode(relative_mouse_window_, false));
  }
}

void SdlPlayerInput::handle_event(const SDL_Event &event) {
  if (event.type == SDL_EVENT_JOYSTICK_ADDED && joystick_ == nullptr) {
    joystick_ = SDL_OpenJoystick(event.jdevice.which);
    if (joystick_ != nullptr) {
      joystick_id_ = SDL_GetJoystickID(joystick_);
    }
  } else if (event.type == SDL_EVENT_JOYSTICK_REMOVED &&
             event.jdevice.which == joystick_id_) {
    close_joystick();
    open_first_joystick();
  }
}

game::PlayerInputSources SdlPlayerInput::sample() const {
  int keyboard_count = 0;
  const bool *keyboard = SDL_GetKeyboardState(&keyboard_count);
  const auto key_down = [keyboard, keyboard_count](const SDL_Scancode key) {
    const auto index = static_cast<int>(key);
    return keyboard != nullptr && index >= 0 && index < keyboard_count &&
           keyboard[index];
  };
  float mouse_x = 0.0F;
  float mouse_y = 0.0F;
  const auto mouse_buttons = SDL_GetRelativeMouseState(&mouse_x, &mouse_y);
  const auto binding_down = [&key_down, mouse_x, mouse_y, mouse_buttons,
                             this](const SdlControlBinding &binding) {
    switch (binding.mouse) {
    case SdlMouseBinding::left:
      return mouse_x < 0.0F;
    case SdlMouseBinding::right:
      return mouse_x > 0.0F;
    case SdlMouseBinding::up:
      return mouse_y < 0.0F;
    case SdlMouseBinding::down:
      return mouse_y > 0.0F;
    case SdlMouseBinding::button_left:
      return (mouse_buttons & SDL_BUTTON_LMASK) != 0U;
    case SdlMouseBinding::button_right:
      return (mouse_buttons & SDL_BUTTON_RMASK) != 0U;
    case SdlMouseBinding::button_middle:
      return (mouse_buttons & SDL_BUTTON_MMASK) != 0U;
    case SdlMouseBinding::none:
      break;
    }
    const auto joystick_axis = [this](const int axis) {
      return joystick_ == nullptr || axis >= SDL_GetNumJoystickAxes(joystick_)
                 ? static_cast<Sint16>(0)
                 : SDL_GetJoystickAxis(joystick_, axis);
    };
    switch (binding.joystick) {
    case SdlJoystickBinding::left:
      return joystick_axis(0) < original_negative_joystick_threshold;
    case SdlJoystickBinding::right:
      return joystick_axis(0) > original_positive_joystick_threshold;
    case SdlJoystickBinding::up:
      return joystick_axis(1) < original_negative_joystick_threshold;
    case SdlJoystickBinding::down:
      return joystick_axis(1) > original_positive_joystick_threshold;
    case SdlJoystickBinding::in:
      return joystick_axis(2) < original_negative_joystick_threshold;
    case SdlJoystickBinding::out:
      return joystick_axis(2) > original_positive_joystick_threshold;
    case SdlJoystickBinding::button1:
    case SdlJoystickBinding::button2:
    case SdlJoystickBinding::button3:
    case SdlJoystickBinding::button4:
    case SdlJoystickBinding::button5:
    case SdlJoystickBinding::button6:
    case SdlJoystickBinding::button7:
    case SdlJoystickBinding::button8: {
      const auto button = static_cast<int>(binding.joystick) -
                          static_cast<int>(SdlJoystickBinding::button1);
      return joystick_ != nullptr &&
             button < SDL_GetNumJoystickButtons(joystick_) &&
             SDL_GetJoystickButton(joystick_, button);
    }
    case SdlJoystickBinding::none:
      break;
    }
    const auto key = binding.key;
    if (key == SDL_SCANCODE_LSHIFT) {
      return key_down(SDL_SCANCODE_LSHIFT) || key_down(SDL_SCANCODE_RSHIFT);
    }
    if (key == SDL_SCANCODE_LCTRL) {
      return key_down(SDL_SCANCODE_LCTRL) || key_down(SDL_SCANCODE_RCTRL);
    }
    if (key == SDL_SCANCODE_LALT) {
      return key_down(SDL_SCANCODE_LALT) || key_down(SDL_SCANCODE_RALT);
    }
    return key_down(key);
  };

  game::PlayerInputSources sources{};
  // Normal play follows the selected retail CLO layout. Development-era
  // WASD/arrow fallbacks made remapped controls non-exclusive.
  sources.controls.keyboard_throttle = binding_down(bindings_.accelerate);
  sources.controls.keyboard_brake = binding_down(bindings_.brake);
  sources.controls.keyboard_left = binding_down(bindings_.turn_left);
  sources.controls.keyboard_right = binding_down(bindings_.turn_right);
  sources.controls.keyboard_handbrake = binding_down(bindings_.handbrake);
  sources.shift_up = binding_down(bindings_.shift_up);
  sources.shift_down = binding_down(bindings_.shift_down);
  sources.rear_view = binding_down(bindings_.rear_view);
  sources.horn = binding_down(bindings_.horn);
  // Race start/restart are owned by the automatic countdown and pause menu,
  // not hidden direct-play shortcuts. C cycles the four reconstructed views;
  // F1..F4 select each view directly.
  sources.toggle_camera = key_down(SDL_SCANCODE_C);
  sources.select_bumper_camera = key_down(SDL_SCANCODE_F1);
  sources.select_in_car_camera = key_down(SDL_SCANCODE_F2);
  sources.select_out_car_camera = key_down(SDL_SCANCODE_F3);
  sources.select_far_camera = key_down(SDL_SCANCODE_F4);
  sources.retail_in_car_view = binding_down(bindings_.in_car_view);
  sources.retail_out_car_view = binding_down(bindings_.out_car_view);
  sources.retail_camera_view = binding_down(bindings_.camera_view);
  sources.retail_cycle_players = binding_down(bindings_.cycle_players);

  return sources;
}

bool SdlPlayerInput::has_gamepad() const noexcept {
  return joystick_ != nullptr && SDL_IsGamepad(joystick_id_);
}

bool SdlPlayerInput::has_joystick() const noexcept {
  return joystick_ != nullptr;
}

void SdlPlayerInput::open_first_joystick() {
  int count = 0;
  SDL_JoystickID *identifiers = SDL_GetJoysticks(&count);
  if (identifiers == nullptr) {
    return;
  }
  if (count > 0) {
    joystick_ = SDL_OpenJoystick(identifiers[0]);
    if (joystick_ != nullptr) {
      joystick_id_ = SDL_GetJoystickID(joystick_);
    }
  }
  SDL_free(identifiers);
}

void SdlPlayerInput::close_joystick() noexcept {
  if (joystick_ != nullptr) {
    SDL_CloseJoystick(joystick_);
    joystick_ = nullptr;
  }
  joystick_id_ = 0U;
}

} // namespace mh::platform
