#pragma once

#include <content/formats/league_definition.hpp>
#include <network/session.hpp>
#include <ui/frontend/renderer.hpp>

#include <optional>
#include <vector>

struct SDL_Window;
struct SDL_Renderer;

namespace mh::app {

// Runs a selected Motorhead race through shared graphics, through a dedicated
// race window on an already initialized SDL runtime, or through the unified
// executable's standalone developer dispatch mode.
int run_motorhead_race(
    int argc, char **argv, SDL_Window *shared_window = nullptr,
    SDL_Renderer *shared_renderer = nullptr,
    std::optional<mh::ui::RaceResultEntry> *completed_player_result = nullptr,
    std::vector<mh::ui::RaceResultEntry> *completed_results = nullptr,
    mh::content::LeagueDivisionFinishingOrders *completed_league_results =
        nullptr,
    mh::network::LanSession *multiplayer_session = nullptr,
    bool reuse_initialized_sdl = false);

} // namespace mh::app
