#pragma once

#include <game/input/player_input.hpp>
#include <platform/control_binding.hpp>
#include <optional>
#include <string_view>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_video.h>

#include <cstdint>

namespace mh::platform {

inline constexpr Sint16 original_negative_joystick_threshold = -27306;
inline constexpr Sint16 original_positive_joystick_threshold = 27305;

enum class SdlMouseBinding : std::uint8_t {
  none,
  left,
  right,
  up,
  down,
  button_left,
  button_right,
  button_middle,
};

enum class SdlJoystickBinding : std::uint8_t {
  none,
  left,
  right,
  up,
  down,
  in,
  out,
  button1,
  button2,
  button3,
  button4,
  button5,
  button6,
  button7,
  button8,
};

struct SdlControlBinding {
  SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
  SdlMouseBinding mouse = SdlMouseBinding::none;
  SdlJoystickBinding joystick = SdlJoystickBinding::none;
  PlatformControlBinding platform{};
};

struct SdlKeyboardBindings {
  SdlControlBinding turn_left{};
  SdlControlBinding turn_right{};
  SdlControlBinding accelerate{};
  SdlControlBinding brake{};
  SdlControlBinding shift_up{};
  SdlControlBinding shift_down{};
  SdlControlBinding handbrake{};
  SdlControlBinding rear_view{};
  SdlControlBinding horn{};
  SdlControlBinding in_car_view{};
  SdlControlBinding out_car_view{};
  SdlControlBinding camera_view{};
  SdlControlBinding cycle_players{};
};

std::optional<SdlControlBinding> parse_control_binding(std::string_view name);
std::optional<bool> platform_binding_down(const SdlControlBinding &binding);
void augment_player_input(const SdlKeyboardBindings &bindings, SDL_Joystick *joystick,
                          game::PlayerInputSources &sources);

// SDL3 adapter for the platform-neutral player input contract. It owns at most
// one raw joystick (which also covers SDL gamepads and wheels), follows
// hot-plug events, and samples only the selected retail CLO bindings.
class SdlPlayerInput {
public:
  explicit SdlPlayerInput(SdlKeyboardBindings bindings = {});
  ~SdlPlayerInput();

  SdlPlayerInput(const SdlPlayerInput &) = delete;
  SdlPlayerInput &operator=(const SdlPlayerInput &) = delete;

  void handle_event(const SDL_Event &event);
  [[nodiscard]] game::PlayerInputSources sample() const;
  [[nodiscard]] bool has_gamepad() const noexcept;
  [[nodiscard]] bool has_joystick() const noexcept;

private:
  void open_first_joystick();
  void close_joystick() noexcept;

  SDL_Joystick *joystick_ = nullptr;
  SDL_JoystickID joystick_id_ = 0U;
  SDL_Window *relative_mouse_window_ = nullptr;
  bool restore_relative_mouse_mode_ = false;
  SdlKeyboardBindings bindings_{};
};

} // namespace mh::platform
