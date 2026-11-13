#pragma once

#include <renderer/types.hpp>

#include <cstdint>
#include <memory>

struct SDL_Renderer;

// The software backend is retained as the compatibility fallback. It owns all
// CPU raster, projection, worker, upload, and checksum state.

namespace mh::render::software {

struct SoftwareRenderResult {
  unsigned int active_workers = 1U;
  std::uint64_t color_checksum = 0U;
  double raster_ms = 0.0;
  double upload_ms = 0.0;
};

class SoftwareRenderer final {
public:
  SoftwareRenderer();
  ~SoftwareRenderer();
  SoftwareRenderer(const SoftwareRenderer &) = delete;
  SoftwareRenderer &operator=(const SoftwareRenderer &) = delete;

  [[nodiscard]] SoftwareRenderResult
  render(SDL_Renderer *renderer, const SceneFrameView &frame,
         unsigned int requested_workers, bool checksum_enabled);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mh::render::software
