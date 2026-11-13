#pragma once
struct SDL_Window;
struct SDL_Renderer;
#include <cstdint>
namespace mh::render { enum class RaceRendererBackend : std::uint8_t; }
#include <platform/renderer_options.hpp>
#include <algorithm>
#include <span>
namespace mh::platform {
SDL_Renderer *create_front_end_renderer(SDL_Window *window);
SDL_Renderer *create_race_renderer(SDL_Window *window, render::RaceRendererBackend &backend);
std::array<std::uint32_t, 2U> graphic_display_dimensions(SDL_Window *window) noexcept;
void align_hosted_race_window(SDL_Window *host, SDL_Window *race, int width, int height);
bool use_hosted_overlay(ui::GraphicWindowMode mode);
}
