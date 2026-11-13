#pragma once

#include <SDL3/SDL.h>
#include <ui/frontend/controller.hpp>

namespace mh::platform {

// Text belongs to the selected editable row, rather than the startup window.
// Explicit activation also gives Steam a field rectangle for its floating OSK.
class DeckTextInput {
public:
  static void configure() {
    if (SDL_getenv("SteamGameId") || SDL_getenv("SteamDeck"))
      SDL_SetHint(SDL_HINT_ENABLE_STEAM_SCREEN_KEYBOARD, "1");
    SDL_SetHintWithPriority(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0", SDL_HINT_OVERRIDE);
  }

  static int field(const ui::FrontEndState &state) {
    using namespace ui;
    if (state.screen() == FrontEndScreen::personal_options) {
      const auto row = state.personal_options_field();
      if (row == PersonalOptionsField::name || row == PersonalOptionsField::team ||
          (row >= PersonalOptionsField::short_key_0 && row <= PersonalOptionsField::short_key_9))
        return 1 + static_cast<int>(row);
    }
    if (state.screen() == FrontEndScreen::league_create &&
        state.league_create_field() == LeagueCreateField::name &&
        !state.league_create_confirmation_open()) return 40;
    if (state.screen() == FrontEndScreen::multiplayer) {
      if (state.multiplayer_page() == MultiplayerPage::join &&
          (state.multiplayer_join_field() == MultiplayerJoinField::address ||
           state.multiplayer_join_field() == MultiplayerJoinField::password))
        return 50 + static_cast<int>(state.multiplayer_join_field());
      if (state.multiplayer_page() == MultiplayerPage::create &&
          (state.multiplayer_create_field() == MultiplayerCreateField::session_name ||
           state.multiplayer_create_field() == MultiplayerCreateField::session_password))
        return 60 + static_cast<int>(state.multiplayer_create_field());
      if (state.multiplayer_page() == MultiplayerPage::lobby) return 70;
    }
    return 0;
  }

  void sync(SDL_Window *window, const ui::FrontEndState &state, bool ready) {
    const int selected = ready ? field(state) : 0;
    if (selected != field_) {
      stop(window);
      field_ = selected;
    }
    if (field_ == 0) return;
    int width = 0, height = 0;
    SDL_GetWindowSize(window, &width, &height);
    // The Personal Options form occupies the upper half of the 16:10 menu.
    const SDL_Rect area{width / 3, height / 5, width / 2, height / 16};
    SDL_SetTextInputArea(window, &area, 0);
    if (!SDL_TextInputActive(window)) SDL_StartTextInput(window);
  }

  bool activate(SDL_Window *window, const SDL_Event &event, bool gamepad_confirm = false) {
    if (field_ == 0 || field_ == 70 || event.type != SDL_EVENT_KEY_DOWN || event.key.repeat ||
        (event.key.key != SDLK_RETURN && event.key.key != SDLK_KP_ENTER) ||
        !SDL_HasScreenKeyboardSupport()) return false;
    // Steam+X can open the keyboard independently of SDL. Its Enter must not
    // reopen it or leave the page. Only the menu's A button requests opening.
    if (!gamepad_confirm && !SDL_ScreenKeyboardShown(window)) return true;
    // Return from the floating keyboard dismisses it, retaining the selected row.
    if (SDL_ScreenKeyboardShown(window)) {
      stop(window);
      SDL_StartTextInput(window);
    } else {
      stop(window);
      SDL_SetHintWithPriority(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1", SDL_HINT_OVERRIDE);
      SDL_StartTextInput(window);
      SDL_SetHintWithPriority(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0", SDL_HINT_OVERRIDE);
    }
    return true;
  }

  static void trace(SDL_Window *window, const SDL_Event &event, const ui::FrontEndState &state) {
    static const bool enabled = SDL_getenv("MOTORHEAD_INPUT_TRACE") != nullptr;
    if (!enabled) return;
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_TEXT_INPUT ||
        event.type == SDL_EVENT_WINDOW_FOCUS_GAINED || event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
      // Diagnose delivery/focus without recording names, passwords, or key values.
      SDL_Log("Deck input: event=%u screen=%u field=%d active=%d focus=%d",
          static_cast<unsigned>(event.type), static_cast<unsigned>(state.screen()),
          field(state), SDL_TextInputActive(window), SDL_GetKeyboardFocus() == window);
    }
  }

private:
  static void stop(SDL_Window *window) {
    // SDL only hides a keyboard it opened when this hint is enabled.
    // Ordinary field activation stays silent, so enable it just for closing.
    const bool shown = SDL_ScreenKeyboardShown(window);
    if (shown) SDL_SetHintWithPriority(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "1", SDL_HINT_OVERRIDE);
    SDL_StopTextInput(window);
    if (shown) SDL_SetHintWithPriority(SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0", SDL_HINT_OVERRIDE);
  }
  int field_ = 0;
};

} // namespace mh::platform
