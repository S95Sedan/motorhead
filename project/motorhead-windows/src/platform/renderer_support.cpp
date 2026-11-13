#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <platform/renderer_support.hpp>
#include <SDL3/SDL.h>
#include <renderer/race_scene_renderer.hpp>
#include <array>
#include <algorithm>
#include <stdexcept>
namespace mh::platform {
std::array<std::uint32_t, 2U> graphic_display_dimensions(SDL_Window *) noexcept { return {}; }
namespace { void require(bool ok,const char *message) { if(!ok) throw std::runtime_error(std::string(message)+": "+SDL_GetError()); } }
SDL_Renderer *create_front_end_renderer(SDL_Window *window) {
  constexpr std::array drivers{"direct3d12", "direct3d11", "direct3d"};
  for (const auto *driver : drivers) {
    SDL_ClearError();
    if (auto *renderer = SDL_CreateRenderer(window, driver);
        renderer != nullptr) {
      return renderer;
    }
  }
  return SDL_CreateRenderer(window, nullptr);
}
[[nodiscard]] SDL_Renderer *
create_race_renderer(SDL_Window *window,
                     mh::render::RaceRendererBackend &backend) {
  const auto try_driver = [window](const char *driver) {
    SDL_ClearError();
    return SDL_CreateRenderer(window, driver);
  };
  const auto try_hardware = [&](const bool d3d12, const bool d3d11,
                                const bool d3d9) -> SDL_Renderer * {
    if (d3d12) {
      if (auto *renderer = try_driver("direct3d12"); renderer != nullptr) {
        if (backend != mh::render::RaceRendererBackend::software &&
            backend != mh::render::RaceRendererBackend::glide) {
          backend = mh::render::RaceRendererBackend::d3d12;
        }
        return renderer;
      }
    }
    if (d3d11) {
      if (auto *renderer = try_driver("direct3d11"); renderer != nullptr) {
        if (backend != mh::render::RaceRendererBackend::software &&
            backend != mh::render::RaceRendererBackend::glide) {
          backend = mh::render::RaceRendererBackend::primary_hardware;
        }
        return renderer;
      }
    }
    if (d3d9) {
      if (auto *renderer = try_driver("direct3d"); renderer != nullptr) {
        if (backend != mh::render::RaceRendererBackend::software) {
          backend = mh::render::RaceRendererBackend::d3d9;
        }
        return renderer;
      }
    }
    return nullptr;
  };

  SDL_Renderer *renderer = nullptr;
  switch (backend) {
  case mh::render::RaceRendererBackend::automatic:
  case mh::render::RaceRendererBackend::d3d12:
    renderer = try_hardware(true, true, true);
    break;
  case mh::render::RaceRendererBackend::primary_hardware:
  case mh::render::RaceRendererBackend::glide:
    renderer = try_hardware(false, true, true);
    break;
  case mh::render::RaceRendererBackend::d3d9:
    renderer = try_hardware(false, false, true);
    break;
  case mh::render::RaceRendererBackend::software:
    renderer = try_hardware(true, true, true);
    break;
  }
  if (renderer == nullptr) {
    backend = mh::render::RaceRendererBackend::software;
    renderer = SDL_CreateRenderer(window, nullptr);
  }
  return renderer;
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
  auto *host_handle = static_cast<HWND>(
      SDL_GetPointerProperty(SDL_GetWindowProperties(host),
                             SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
  if (host_handle == nullptr) {
    throw std::runtime_error("query hosted race Win32 parent");
  }
  RECT client{};
  POINT origin{};
  if (GetClientRect(host_handle, &client) == FALSE ||
      ClientToScreen(host_handle, &origin) == FALSE) {
    throw std::runtime_error("query hosted race client bounds");
  }
  const auto client_width = client.right - client.left;
  const auto client_height = client.bottom - client.top;
  const auto width = client_width > 0 ? client_width : fallback_width;
  const auto height = client_height > 0 ? client_height : fallback_height;
  require(SDL_SetWindowBordered(race, false),
          "remove hosted race window border");
  require(SDL_SetWindowResizable(race, false), "lock hosted race overlay size");
  require(SDL_SetWindowPosition(race, origin.x, origin.y),
          "position hosted race overlay");
  require(SDL_SetWindowSize(race, width, height), "size hosted race overlay");
  require(SDL_SyncWindow(race), "synchronize hosted race overlay");
}
bool use_hosted_overlay(ui::GraphicWindowMode mode) { return mode != ui::GraphicWindowMode::fullscreen; }


}
