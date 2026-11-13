#pragma once

#include <platform/deck_controller.hpp>

namespace mh::platform {

// Share the front-end controller across movies and menus. Keeping it open
// between clips avoids treating a held button as a fresh press each time.
class DeckMovieInput {
public:
  explicit DeckMovieInput(DeckController &controller) : controller_(controller) {}

  bool handle(SDL_Event &event) {
    const bool skip =
        (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) ||
        event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
        (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN &&
         !SDL_IsGamepad(event.jbutton.which));
    // SDL emits raw joystick and mapped gamepad events for the same press.
    // Count only the mapped event, so one press skips one clip.
    // Feed hot-plug events through even when no skip was requested.
    controller_.handle(event, false);
    return skip;
  }

private:
  DeckController &controller_;
};

} // namespace mh::platform
