#pragma once
#include <ui/frontend/controller.hpp>
#include <algorithm>
#include <span>
namespace mh::platform {
const char *primary_renderer_key();
const char *primary_renderer_label();
const char *primary_renderer_driver();
std::span<const ui::GraphicRendererBackend> renderer_choices();
ui::GraphicRendererBackend normalize_renderer_backend(ui::GraphicRendererBackend backend) noexcept;
std::span<const ui::GraphicWindowMode> window_mode_choices();
ui::GraphicWindowMode normalize_window_mode(ui::GraphicWindowMode mode) noexcept;
const char *window_mode_label(ui::GraphicWindowMode mode) noexcept;
inline ui::GraphicWindowMode step_window_mode(ui::GraphicWindowMode current, bool increase, bool wrap = false) {
  const auto choices = window_mode_choices();
  const auto it = std::find(choices.begin(), choices.end(), current);
  auto index = it == choices.end() ? 0 : static_cast<int>(it - choices.begin());
  const auto count = static_cast<int>(choices.size());
  index = wrap ? (index + (increase ? 1 : count - 1)) % count
               : std::clamp(index + (increase ? 1 : -1), 0, count - 1);
  return choices[static_cast<std::size_t>(index)];
}
bool screen_size_supported(ui::GraphicScreenSize size,
                           std::array<std::uint32_t, 2U> display) noexcept;
inline void constrain_graphic_options(ui::GraphicOptionsConfiguration &configuration) noexcept {
  configuration.renderer_backend = normalize_renderer_backend(configuration.renderer_backend);
  configuration.window_mode = normalize_window_mode(configuration.window_mode);
  if (screen_size_supported(configuration.screen_size, configuration.display_dimensions)) return;
  const auto tier = ui::graphic_screen_size_tier(configuration.screen_size);
  for (int candidate = static_cast<int>(tier); candidate >= 0; --candidate) {
    const auto size = ui::graphic_screen_size_for_aspect(configuration.aspect_ratio, static_cast<std::size_t>(candidate));
    if (screen_size_supported(size, configuration.display_dimensions)) {
      configuration.screen_size = size;
      return;
    }
  }
  // On a narrow external display an entire aspect family may not fit.
  for (int aspect = 0; aspect < 3; ++aspect) {
    const auto size = ui::graphic_screen_size_for_aspect(static_cast<ui::GraphicAspectRatio>(aspect), 0);
    if (screen_size_supported(size, configuration.display_dimensions)) {
      configuration.screen_size = size;
      configuration.aspect_ratio = ui::graphic_screen_aspect_ratio(size);
      return;
    }
  }
}
inline ui::GraphicRendererBackend step_renderer(ui::GraphicRendererBackend current, bool increase) {
  const auto choices=renderer_choices();
  auto it=std::find(choices.begin(),choices.end(),current);
  auto index=it==choices.end()?0:static_cast<int>(it-choices.begin());
  index=std::clamp(index+(increase?1:-1),0,static_cast<int>(choices.size())-1);
  return choices[static_cast<std::size_t>(index)];
}
}
