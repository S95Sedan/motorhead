#pragma once

#include <renderer/types.hpp>

#include <memory>

struct SDL_Renderer;
struct SDL_Texture;

namespace mh::render::hardware {

class D3D12Renderer final {
public:
  D3D12Renderer();
  ~D3D12Renderer();
  D3D12Renderer(const D3D12Renderer &) = delete;
  D3D12Renderer &operator=(const D3D12Renderer &) = delete;

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
