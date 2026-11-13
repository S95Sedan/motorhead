#pragma once
#include <SDL3/SDL.h>
#include <ui/frontend/controller.hpp>
#include <functional>
#include <memory>
#include <string>

namespace mh::platform {
void configure_frontend_platform();
class FrontEndPlatform {
public:
  explicit FrontEndPlatform(SDL_Window *window);
  ~FrontEndPlatform();
  FrontEndPlatform(const FrontEndPlatform &) = delete;
  FrontEndPlatform &operator=(const FrontEndPlatform &) = delete;
  bool skip_movie(SDL_Event &event);
  bool handle_event(SDL_Event &event, const ui::FrontEndState &state, bool ready,
                    const std::function<void(const std::string &)> &bind);
  void update(const ui::FrontEndState &state, bool ready);
  bool recover_after_focus(const SDL_Event &event);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class RacePlatformInput {
public:
  RacePlatformInput();
  ~RacePlatformInput();
  RacePlatformInput(const RacePlatformInput &) = delete;
  RacePlatformInput &operator=(const RacePlatformInput &) = delete;
  void handle(SDL_Event &event, bool navigation);
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
