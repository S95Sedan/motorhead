#pragma once

// Only the isolated Steam Deck build includes this adapter.
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>

namespace mh::platform {

inline SDL_Gamepad *deck_gamepad() {
  int count = 0;
  auto *ids = SDL_GetGamepads(&count);
  SDL_Gamepad *result = nullptr;
  for (int i = 0; i < count && result == nullptr; ++i) {
    result = SDL_GetGamepadFromID(ids[i]);
  }
  SDL_free(ids);
  return result;
}

class DeckController {
public:
  DeckController() {
    initialized_ = SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    open();
  }
  ~DeckController() {
    if (pad_ != nullptr) SDL_CloseGamepad(pad_);
    if (initialized_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
  }
  DeckController(const DeckController &) = delete;
  DeckController &operator=(const DeckController &) = delete;

  // Translate navigation in place; driving is sampled as analog input.
  void handle(SDL_Event &event, bool navigation) {
    if (event.type == SDL_EVENT_GAMEPAD_REMOVED && pad_ != nullptr &&
        event.gdevice.which == SDL_GetGamepadID(pad_)) {
      SDL_CloseGamepad(pad_);
      pad_ = nullptr;
      directions_.fill(0);
      open();
    } else if (event.type == SDL_EVENT_GAMEPAD_ADDED && pad_ == nullptr) {
      open();
    }
    if (pad_ == nullptr) return;
    SDL_Keycode key = 0;
    bool down = true;
    if ((event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
         event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) &&
        event.gbutton.which == SDL_GetGamepadID(pad_)) {
      down = event.gbutton.down;
      switch (event.gbutton.button) {
      case SDL_GAMEPAD_BUTTON_START: key = SDLK_ESCAPE; break;
      case SDL_GAMEPAD_BUTTON_SOUTH: if (navigation) key = SDLK_RETURN; break;
      case SDL_GAMEPAD_BUTTON_EAST: if (navigation) key = SDLK_ESCAPE; break;
      case SDL_GAMEPAD_BUTTON_DPAD_UP: if (navigation) key = SDLK_UP; break;
      case SDL_GAMEPAD_BUTTON_DPAD_DOWN: if (navigation) key = SDLK_DOWN; break;
      case SDL_GAMEPAD_BUTTON_DPAD_LEFT: if (navigation) key = SDLK_LEFT; break;
      case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: if (navigation) key = SDLK_RIGHT; break;
      default: break;
      }
    } else if (event.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
               event.gaxis.which == SDL_GetGamepadID(pad_) &&
               event.gaxis.axis <= SDL_GAMEPAD_AXIS_LEFTY) {
      const auto axis = static_cast<std::size_t>(event.gaxis.axis);
      const int direction = event.gaxis.value < -16000 ? -1 :
                            event.gaxis.value > 16000 ? 1 : 0;
      if (navigation && direction != 0 && direction != directions_[axis]) {
        key = axis == 0 ? (direction < 0 ? SDLK_LEFT : SDLK_RIGHT) :
                          (direction < 0 ? SDLK_UP : SDLK_DOWN);
      }
      directions_[axis] = direction;
    }
    if (key != 0) {
      const auto timestamp = event.common.timestamp;
      event = {};
      event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
      event.key.timestamp = timestamp;
      event.key.key = key;
      event.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
      event.key.down = down;
    }
  }

private:
  void open() {
    if (!initialized_) return;
    int count = 0;
    auto *ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count && pad_ == nullptr; ++i) {
      pad_ = SDL_OpenGamepad(ids[i]);
    }
    SDL_free(ids);
  }
  bool initialized_ = false;
  SDL_Gamepad *pad_ = nullptr;
  std::array<int, 2> directions_{};
};

} // namespace mh::platform
