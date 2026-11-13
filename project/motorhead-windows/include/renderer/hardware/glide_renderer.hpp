#pragma once

#include <renderer/types.hpp>

#include <memory>

struct SDL_Renderer;
struct SDL_Texture;

namespace mh::render::hardware {

// Self-contained emulation of Motorhead's Glide raster path. It owns its
// emulation shaders, state, textures, depth target and draw batching; no
// external Glide wrapper and no D3D11Renderer delegation is involved.
class GlideRenderer {
public:
  GlideRenderer();
  ~GlideRenderer();
  GlideRenderer(const GlideRenderer &) = delete;
  GlideRenderer &operator=(const GlideRenderer &) = delete;

  [[nodiscard]] bool initialize(SDL_Renderer *renderer);
  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] SDL_Texture *render(SDL_Renderer *renderer,
                                    const SceneFrameView &frame);
  [[nodiscard]] double last_render_ms() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mh::render::hardware
