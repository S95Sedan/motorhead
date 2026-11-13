#include <platform/frontend.hpp>
#include <platform/deck_controller.hpp>
#include <platform/deck_text_input.hpp>
#include <platform/deck_movie_input.hpp>
#include <platform/deck_bindings.hpp>
namespace mh::platform {
void configure_frontend_platform() { DeckTextInput::configure(); }
struct FrontEndPlatform::Impl {
  SDL_Window *window;
  DeckController controller;
  DeckTextInput text;
  DeckMovieInput movies{controller};
  explicit Impl(SDL_Window *value):window(value) {}
};
FrontEndPlatform::FrontEndPlatform(SDL_Window *window):impl_(std::make_unique<Impl>(window)) {}
FrontEndPlatform::~FrontEndPlatform()=default;
bool FrontEndPlatform::skip_movie(SDL_Event &event) { return impl_->movies.handle(event); }
bool FrontEndPlatform::handle_event(SDL_Event &event,const ui::FrontEndState &state,bool ready,
    const std::function<void(const std::string &)> &bind) {
  update(state,ready);
  DeckTextInput::trace(impl_->window,event,state);
  const bool capturing=ready && state.control_binding_capture_active();
  if(capturing) {
    const auto binding=deck_binding_from_event(event);
    if(!binding.empty()) {
      bind(std::string(binding)); impl_->controller.handle(event,false); return true;
    }
  }
  if(event.type==SDL_EVENT_JOYSTICK_AXIS_MOTION || event.type==SDL_EVENT_JOYSTICK_BUTTON_DOWN) {
    const auto id=event.type==SDL_EVENT_JOYSTICK_AXIS_MOTION?event.jaxis.which:event.jbutton.which;
    if(SDL_IsGamepad(id)) return true;
    if(capturing && event.type==SDL_EVENT_JOYSTICK_AXIS_MOTION && event.jaxis.axis<3 &&
       (event.jaxis.value < -16000 || event.jaxis.value > 16000)) {
      const bool negative=event.jaxis.value<0;
      bind(event.jaxis.axis==0?(negative?"JoyLeft":"JoyRight"):
           event.jaxis.axis==1?(negative?"JoyUp":"JoyDown"):(negative?"JoyIn":"JoyOut"));
      return true;
    }
    if(capturing && event.type==SDL_EVENT_JOYSTICK_BUTTON_DOWN && event.jbutton.button<8) {
      bind("JoyButton"+std::to_string(event.jbutton.button+1));return true;
    }
  }
  const bool confirm=event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button==SDL_GAMEPAD_BUTTON_SOUTH;
  impl_->controller.handle(event,!capturing);
  return ready && impl_->text.activate(impl_->window,event,confirm);
}
void FrontEndPlatform::update(const ui::FrontEndState &state,bool ready) {
  impl_->text.sync(impl_->window,state,ready);
}
bool FrontEndPlatform::recover_after_focus(const SDL_Event &) { return false; }
struct RacePlatformInput::Impl { DeckController controller; };
RacePlatformInput::RacePlatformInput():impl_(std::make_unique<Impl>()) {}
RacePlatformInput::~RacePlatformInput()=default;
void RacePlatformInput::handle(SDL_Event &event,bool navigation) { impl_->controller.handle(event,navigation); }
}
