#include <renderer/race_scene_renderer.hpp>
#include <renderer/hardware/opengl_renderer.hpp>
#include <renderer/software/software_renderer.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mh::render {
namespace {
void require(bool condition, const char *operation) {
  if (!condition) throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
#ifndef NDEBUG
void validate_texture_levels(const std::vector<mh::content::PamRgbaImage> *levels) {
  if (levels == nullptr) return;
  for (const auto &image : *levels)
    if (image.width == 0 || image.height == 0 || image.rgba.size() != static_cast<std::size_t>(image.width)*image.height*4)
      throw std::runtime_error("race texture image is inconsistent");
}
#endif
}
struct RaceRenderer::Impl {
  SceneLighting lighting{};
  float focal_length = 0;
  int width = 0, height = 0;
  double near_plane = 0.5, fog_start = 0, fog_end = 0;
  std::array<float,3> fog_color{};
  bool fog_enabled = false, checksum_enabled = false, tron_hidden_line = false;
  std::vector<SceneRasterCommand> commands;
  std::vector<VehicleShadowDepthProjection> shadows;
  std::vector<HeadlightDepthProjection> headlights;
  unsigned int requested_workers = 0, active_workers = 0;
  RaceRendererBackend backend = RaceRendererBackend::automatic;
  std::uint64_t checksum = 0;
  double render_ms = 0, upload_ms = 0;
  std::unique_ptr<hardware::OpenGLRenderer> gl;
  std::unique_ptr<software::SoftwareRenderer> software;
};
RaceRenderer::RaceRenderer() : impl_(std::make_unique<Impl>()) {}
RaceRenderer::~RaceRenderer() = default;
void RaceRenderer::set_lighting(const SceneLighting &lighting) noexcept { impl_->lighting = lighting; }
void RaceRenderer::set_requested_software_workers(unsigned int workers) noexcept { impl_->requested_workers = workers; }
void RaceRenderer::set_distance_fog(double start, double end, std::array<float,3> color, bool enabled) {
  if (enabled && (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end <= start))
    throw std::runtime_error("race distance fog range is invalid");
  impl_->fog_start = start; impl_->fog_end = end; impl_->fog_color = color; impl_->fog_enabled = enabled;
}
void RaceRenderer::set_backend(RaceRendererBackend backend) noexcept { impl_->backend = backend; }
void RaceRenderer::set_checksum_enabled(bool enabled) noexcept { impl_->checksum_enabled = enabled; }
void RaceRenderer::set_tron_hidden_line(bool enabled) noexcept { impl_->tron_hidden_line = enabled; }
void RaceRenderer::begin_frame(SDL_Renderer *renderer, int width, int height, double near_plane, float focal_length) {
  if (width <= 0 || height <= 0 || !std::isfinite(near_plane) || near_plane <= 0 ||
      static_cast<std::uint64_t>(width)*height > std::numeric_limits<std::size_t>::max()/4)
    throw std::runtime_error("race render-target dimensions are invalid");
  impl_->width = width; impl_->height = height; impl_->near_plane = near_plane; impl_->focal_length = focal_length;
  impl_->commands.clear(); impl_->shadows.clear(); impl_->headlights.clear();
  if (impl_->backend != RaceRendererBackend::software && !impl_->gl) {
    impl_->gl = std::make_unique<hardware::OpenGLRenderer>();
    if (!impl_->gl->initialize(renderer)) throw std::runtime_error("Native OpenGL GPU renderer initialization failed");
    impl_->backend = RaceRendererBackend::primary_hardware;
  }
}
#include <renderer/race_scene_recording.inc>

void RaceRenderer::present(SDL_Renderer *renderer) {
  const auto trilinear = std::any_of(impl_->commands.begin(), impl_->commands.end(), [](const auto &c) {
    return c.texture_levels != nullptr && c.trilinear_filtering;
  });
  const SceneFrameView frame{impl_->width, impl_->height, impl_->near_plane, &impl_->commands,
    &impl_->shadows, &impl_->headlights, impl_->tron_hidden_line, trilinear, impl_->fog_start,
    impl_->fog_end, impl_->fog_color, impl_->fog_enabled, impl_->focal_length, &impl_->lighting};
  if (impl_->backend != RaceRendererBackend::software && !impl_->checksum_enabled) {
    auto *texture = impl_->gl->render(renderer, frame);
    require(texture != nullptr, "render native OpenGL scene");
    require(SDL_RenderTexture(renderer, texture, nullptr, nullptr), "compose native OpenGL scene");
    impl_->active_workers = 0; impl_->render_ms = impl_->gl->last_render_ms(); impl_->upload_ms = 0;
  } else {
    if (!impl_->software) impl_->software = std::make_unique<software::SoftwareRenderer>();
    const auto result = impl_->software->render(renderer, frame, impl_->requested_workers, impl_->checksum_enabled);
    impl_->active_workers = result.active_workers; impl_->checksum = result.color_checksum;
    impl_->render_ms = result.raster_ms; impl_->upload_ms = result.upload_ms;
  }
}
int RaceRenderer::width() const noexcept { return impl_->width; }
int RaceRenderer::height() const noexcept { return impl_->height; }
RaceRendererBackend RaceRenderer::backend() const noexcept { return impl_->backend; }
unsigned int RaceRenderer::active_software_workers() const noexcept { return impl_->active_workers; }
std::uint64_t RaceRenderer::color_checksum() const noexcept { return impl_->checksum; }
double RaceRenderer::last_render_ms() const noexcept { return impl_->render_ms; }
double RaceRenderer::last_upload_ms() const noexcept { return impl_->upload_ms; }
}
