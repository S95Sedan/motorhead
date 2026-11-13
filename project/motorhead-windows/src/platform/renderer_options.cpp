#include <platform/renderer_options.hpp>
#include <array>
namespace mh::platform {
const char *primary_renderer_key() { return "d3d11"; }
const char *primary_renderer_label() { return "D3D11"; }
const char *primary_renderer_driver() { return "direct3d11"; }
std::span<const ui::GraphicWindowMode> window_mode_choices() {
  static constexpr std::array choices{ui::GraphicWindowMode::windowed,
      ui::GraphicWindowMode::borderless, ui::GraphicWindowMode::fullscreen};
  return choices;
}
ui::GraphicWindowMode normalize_window_mode(ui::GraphicWindowMode mode) noexcept { return mode; }
const char *window_mode_label(ui::GraphicWindowMode mode) noexcept {
  return ui::graphic_window_mode_config_name(mode).data();
}
ui::GraphicRendererBackend normalize_renderer_backend(ui::GraphicRendererBackend backend) noexcept { return backend; }
bool screen_size_supported(ui::GraphicScreenSize, std::array<std::uint32_t, 2U>) noexcept { return true; }
std::span<const ui::GraphicRendererBackend> renderer_choices() {
  static constexpr std::array choices{ui::GraphicRendererBackend::automatic, ui::GraphicRendererBackend::d3d9, ui::GraphicRendererBackend::primary_hardware, ui::GraphicRendererBackend::d3d12, ui::GraphicRendererBackend::glide, ui::GraphicRendererBackend::software}; return choices;
}
}
