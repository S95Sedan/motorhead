#pragma once

#include <renderer/types.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace mh::render {

enum class RaceRendererBackend : std::uint8_t {
  automatic,
  d3d9,
  d3d11,
  d3d12,
  glide,
  software,
};

class RaceRenderer {
public:
  RaceRenderer();
  ~RaceRenderer();
  RaceRenderer(const RaceRenderer &) = delete;
  RaceRenderer &operator=(const RaceRenderer &) = delete;

  void set_requested_software_workers(unsigned int workers) noexcept;
  void set_backend(RaceRendererBackend backend) noexcept;
  void set_checksum_enabled(bool enabled) noexcept;
  void set_tron_hidden_line(bool enabled) noexcept;

  void begin_frame(SDL_Renderer *renderer, int width, int height,
                   double near_plane);
  void set_distance_fog(double start_distance, double end_distance,
                        std::array<float, 3U> color, bool enabled);
  void
  record_geometry(const std::array<SDL_Vertex, 4U> &vertices,
                  const std::array<double, 4U> &view_depths,
                  std::uint8_t vertex_count,
                  const std::vector<mh::content::PamRgbaImage> *texture_levels,
                  bool trilinear_filtering,
                  SceneBlendMode blend_mode = SceneBlendMode::opaque,
                  bool coplanar_depth_bias = false, bool depth_test = true);
  void record_shadow(VehicleShadowDepthProjection projection);
  void record_headlights(HeadlightDepthProjection projection);
  void present(SDL_Renderer *renderer);

  [[nodiscard]] int width() const noexcept;
  [[nodiscard]] int height() const noexcept;
  [[nodiscard]] RaceRendererBackend backend() const noexcept;
  [[nodiscard]] unsigned int active_software_workers() const noexcept;
  [[nodiscard]] std::uint64_t color_checksum() const noexcept;
  [[nodiscard]] double last_render_ms() const noexcept;
  [[nodiscard]] double last_upload_ms() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace mh::render
