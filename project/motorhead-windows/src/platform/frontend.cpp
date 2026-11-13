#include <platform/frontend.hpp>
#include <stdexcept>
namespace mh::platform {
void configure_frontend_platform() {}
struct FrontEndPlatform::Impl { SDL_Window *window; bool lost=false; };
FrontEndPlatform::FrontEndPlatform(SDL_Window *window) : impl_(std::make_unique<Impl>(Impl{window})) {
  if(!SDL_StartTextInput(window)) throw std::runtime_error(SDL_GetError());
}
FrontEndPlatform::~FrontEndPlatform()=default;
bool FrontEndPlatform::skip_movie(SDL_Event &event) {
  return event.type==SDL_EVENT_KEY_DOWN && !event.key.repeat;
}
bool FrontEndPlatform::handle_event(SDL_Event &,const ui::FrontEndState &,bool,
    const std::function<void(const std::string &)> &) { return false; }
void FrontEndPlatform::update(const ui::FrontEndState &,bool) {}
bool FrontEndPlatform::recover_after_focus(const SDL_Event &event) {
  if(event.window.windowID!=SDL_GetWindowID(impl_->window)) return false;
  if(event.type==SDL_EVENT_WINDOW_FOCUS_LOST) impl_->lost=true;
  if(event.type!=SDL_EVENT_WINDOW_FOCUS_GAINED && event.type!=SDL_EVENT_WINDOW_RESTORED) return false;
  const bool recover=impl_->lost;
  impl_->lost=false;
  return recover;
}
struct RacePlatformInput::Impl {};
RacePlatformInput::RacePlatformInput():impl_(std::make_unique<Impl>()) {}
RacePlatformInput::~RacePlatformInput()=default;
void RacePlatformInput::handle(SDL_Event &,bool) {}
}
