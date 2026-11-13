#include <platform/sdl_input.hpp>
namespace mh::platform {
std::optional<SdlControlBinding> parse_control_binding(std::string_view) { return std::nullopt; }
std::optional<bool> platform_binding_down(const SdlControlBinding &) { return std::nullopt; }
void augment_player_input(const SdlKeyboardBindings &, SDL_Joystick *, game::PlayerInputSources &) {}
}
