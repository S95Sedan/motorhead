#pragma once

#include <renderer/types.hpp>

#include <memory>

struct SDL_Renderer;
struct SDL_Texture;

namespace mh::render::hardware {

class OpenGLRenderer {
public:
  OpenGLRenderer();
  ~OpenGLRenderer();
  OpenGLRenderer(const OpenGLRenderer &) = delete;
  OpenGLRenderer &operator=(const OpenGLRenderer &) = delete;

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
