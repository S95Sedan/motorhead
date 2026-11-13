#include <platform/renderer_support.hpp>
#include <SDL3/SDL.h>
#include <renderer/race_scene_renderer.hpp>
#include <array>
#include <algorithm>
#include <stdexcept>
namespace mh::platform {
namespace { void require(bool ok,const char *message) { if(!ok) throw std::runtime_error(std::string(message)+": "+SDL_GetError()); } }
SDL_Renderer *create_front_end_renderer(SDL_Window *window) {
  return SDL_CreateRenderer(window, "opengl");
}
[[nodiscard]] SDL_Renderer *
create_race_renderer(SDL_Window *window, mh::render::RaceRendererBackend &backend) {
  if (backend != mh::render::RaceRendererBackend::software)
    backend = mh::render::RaceRendererBackend::primary_hardware;
  return SDL_CreateRenderer(window, "opengl");
}
std::array<std::uint32_t, 2U> graphic_display_dimensions(SDL_Window *window) noexcept {
  const auto display = window != nullptr ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay();
  if (const auto *mode = SDL_GetDesktopDisplayMode(display);
      mode != nullptr && mode->w > 0 && mode->h > 0) {
    return {static_cast<std::uint32_t>(mode->w), static_cast<std::uint32_t>(mode->h)};
  }
  return {1280, 800};
}
void align_hosted_race_window(SDL_Window *host, SDL_Window *race,
                              const int fallback_width,
                              const int fallback_height) {
  if (host == nullptr || race == nullptr) {
    throw std::invalid_argument("hosted race alignment requires two windows");
  }
  if (fallback_width <= 0 || fallback_height <= 0) {
    throw std::invalid_argument("hosted race alignment fallback is invalid");
  }
  // Fullscreen transitions are asynchronous on SDL/Win32.  Querying the
  // parent's client rectangle before both windows have committed the change can
  // transiently return a zero-sized rectangle, which SDL_SetWindowSize rejects.
  require(SDL_SyncWindow(host), "synchronize hosted race parent");
  require(SDL_SyncWindow(race), "synchronize hosted race overlay transition");
  int origin_x = 0, origin_y = 0, width = 0, height = 0;
  require(SDL_GetWindowPosition(host, &origin_x, &origin_y), "query hosted race position");
  require(SDL_GetWindowSize(host, &width, &height), "query hosted race size");
  width = width > 0 ? width : fallback_width;
  height = height > 0 ? height : fallback_height;
  require(SDL_SetWindowBordered(race, false),
          "remove hosted race window border");
  require(SDL_SetWindowResizable(race, false), "lock hosted race overlay size");
  require(SDL_SetWindowPosition(race, origin_x, origin_y),
          "position hosted race overlay");
  require(SDL_SetWindowSize(race, width, height), "size hosted race overlay");
  require(SDL_SyncWindow(race), "synchronize hosted race overlay");
}
bool use_hosted_overlay(ui::GraphicWindowMode mode) { (void)mode; return false; }


}
