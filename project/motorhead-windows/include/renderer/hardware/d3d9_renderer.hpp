#pragma once

#include <renderer/types.hpp>

#include <memory>

struct SDL_Renderer;

namespace mh::render::hardware {

class D3D9Renderer final {
public:
  D3D9Renderer();
  ~D3D9Renderer();
  D3D9Renderer(const D3D9Renderer &) = delete;
  D3D9Renderer &operator=(const D3D9Renderer &) = delete;

  [[nodiscard]] bool initialize(SDL_Renderer *renderer);
  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] bool render(SDL_Renderer *renderer,
                            const SceneFrameView &frame);
  [[nodiscard]] double last_render_ms() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mh::render::hardware
