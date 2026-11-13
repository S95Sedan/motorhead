#include <platform/renderer_options.hpp>
#include <array>
namespace mh::platform {
const char *primary_renderer_key() { return "opengl"; }
const char *primary_renderer_label() { return "OpenGL"; }
const char *primary_renderer_driver() { return "opengl"; }
std::span<const ui::GraphicWindowMode> window_mode_choices() {
  static constexpr std::array choices{ui::GraphicWindowMode::borderless};
  return choices;
}
ui::GraphicWindowMode normalize_window_mode(ui::GraphicWindowMode) noexcept {
  return ui::GraphicWindowMode::borderless;
}
const char *window_mode_label(ui::GraphicWindowMode) noexcept { return "Fullscreen"; }
ui::GraphicRendererBackend normalize_renderer_backend(ui::GraphicRendererBackend backend) noexcept {
  return backend == ui::GraphicRendererBackend::software ? backend : ui::GraphicRendererBackend::primary_hardware;
}
bool screen_size_supported(ui::GraphicScreenSize size, std::array<std::uint32_t, 2U> display) noexcept {
  if (display[0] == 0 || display[1] == 0) display = {1280, 800};
  const auto dimensions = ui::graphic_screen_dimensions(size);
  return dimensions[0] <= display[0] && dimensions[1] <= display[1];
}
std::span<const ui::GraphicRendererBackend> renderer_choices() {
  static constexpr std::array choices{ui::GraphicRendererBackend::primary_hardware, ui::GraphicRendererBackend::software}; return choices;
}
}
