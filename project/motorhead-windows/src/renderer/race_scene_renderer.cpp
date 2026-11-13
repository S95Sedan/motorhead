#include <renderer/race_scene_renderer.hpp>

#include <renderer/hardware/d3d11_renderer.hpp>
#include <renderer/hardware/d3d12_renderer.hpp>
#include <renderer/hardware/d3d9_renderer.hpp>
#include <renderer/hardware/glide_renderer.hpp>
#include <renderer/software/software_renderer.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mh::render {

namespace {

void require(const bool condition, const char *operation) {
  if (!condition) {
    throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
  }
}

#ifndef NDEBUG
void validate_texture_levels(
    const std::vector<mh::content::PamRgbaImage> *levels) {
  if (levels == nullptr) {
    return;
  }
  for (const auto &image : *levels) {
    if (image.width == 0U || image.height == 0U ||
        image.rgba.size() !=
            static_cast<std::size_t>(image.width) * image.height * 4U) {
      throw std::runtime_error("race texture image is inconsistent");
    }
  }
}
#endif

} // namespace

struct RaceRenderer::Impl {
  SceneLighting lighting{};
  float focal_length = 0.0F;
  int width = 0;
  int height = 0;
  double near_plane = 0.5;
  double fog_start = 0.0;
  double fog_end = 0.0;
  std::array<float, 3U> fog_color{};
  bool fog_enabled = false;
  std::vector<SceneRasterCommand> commands;
  std::vector<VehicleShadowDepthProjection> shadows;
  std::vector<HeadlightDepthProjection> headlights;
  unsigned int requested_workers = 0U;
  unsigned int active_workers = 0U;
  RaceRendererBackend backend = RaceRendererBackend::automatic;
  bool checksum_enabled = false;
  bool tron_hidden_line = false;
  bool d3d11_initialization_attempted = false;
  bool d3d9_initialization_attempted = false;
  bool d3d12_initialization_attempted = false;
  bool glide_initialization_attempted = false;
  std::uint64_t checksum = 0U;
  double render_ms = 0.0;
  double upload_ms = 0.0;
  std::unique_ptr<hardware::D3D11Renderer> d3d11_renderer;
  std::unique_ptr<hardware::D3D9Renderer> d3d9_renderer;
  std::unique_ptr<hardware::D3D12Renderer> d3d12_renderer;
  std::unique_ptr<hardware::GlideRenderer> glide_renderer;
  std::unique_ptr<software::SoftwareRenderer> software_renderer;
};

RaceRenderer::RaceRenderer() : impl_(std::make_unique<Impl>()) {}
RaceRenderer::~RaceRenderer() = default;

void RaceRenderer::set_lighting(const SceneLighting &lighting) noexcept {
  impl_->lighting = lighting;
}

void RaceRenderer::set_requested_software_workers(
    const unsigned int workers) noexcept {
  impl_->requested_workers = workers;
}

void RaceRenderer::set_distance_fog(const double start_distance,
                                    const double end_distance,
                                    const std::array<float, 3U> color,
                                    const bool enabled) {
  if (enabled &&
      (!std::isfinite(start_distance) || !std::isfinite(end_distance) ||
       start_distance < 0.0 || end_distance <= start_distance)) {
    throw std::runtime_error("race distance fog range is invalid");
  }
  impl_->fog_start = start_distance;
  impl_->fog_end = end_distance;
  impl_->fog_color = color;
  impl_->fog_enabled = enabled;
}

void RaceRenderer::set_backend(const RaceRendererBackend backend) noexcept {
  impl_->backend = backend;
}

void RaceRenderer::set_checksum_enabled(const bool enabled) noexcept {
  impl_->checksum_enabled = enabled;
}

void RaceRenderer::set_tron_hidden_line(const bool enabled) noexcept {
  impl_->tron_hidden_line = enabled;
}

void RaceRenderer::begin_frame(SDL_Renderer *renderer, const int width,
                               const int height, const double near_plane,
                               const float focal_length) {
  impl_->focal_length = focal_length;
  if (width <= 0 || height <= 0 || !std::isfinite(near_plane) ||
      near_plane <= 0.0 ||
      static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) >
          std::numeric_limits<std::size_t>::max() / 4U) {
    throw std::runtime_error("race render-target dimensions are invalid");
  }
  impl_->width = width;
  impl_->height = height;
  impl_->near_plane = near_plane;
  impl_->commands.clear();
  impl_->shadows.clear();
  impl_->headlights.clear();

  if (impl_->backend == RaceRendererBackend::primary_hardware &&
      !impl_->d3d11_initialization_attempted) {
    impl_->d3d11_initialization_attempted = true;
    impl_->d3d11_renderer = std::make_unique<hardware::D3D11Renderer>();
    auto initialized = false;
    try {
      initialized = impl_->d3d11_renderer->initialize(renderer);
    } catch (...) {
    }
    if (!initialized) {
      impl_->d3d11_renderer.reset();
      impl_->backend = RaceRendererBackend::software;
    }
  }
  if (impl_->backend == RaceRendererBackend::d3d9 &&
      !impl_->d3d9_initialization_attempted) {
    impl_->d3d9_initialization_attempted = true;
    impl_->d3d9_renderer = std::make_unique<hardware::D3D9Renderer>();
    auto initialized = false;
    try {
      initialized = impl_->d3d9_renderer->initialize(renderer);
    } catch (...) {
    }
    if (!initialized) {
      impl_->d3d9_renderer.reset();
      impl_->backend = RaceRendererBackend::software;
    }
  }
  if (impl_->backend == RaceRendererBackend::d3d12 &&
      !impl_->d3d12_initialization_attempted) {
    impl_->d3d12_initialization_attempted = true;
    impl_->d3d12_renderer = std::make_unique<hardware::D3D12Renderer>();
    auto initialized = false;
    try {
      initialized = impl_->d3d12_renderer->initialize(renderer);
    } catch (...) {
    }
    if (!initialized) {
      impl_->d3d12_renderer.reset();
      impl_->backend = RaceRendererBackend::software;
    }
  }
  if (impl_->backend == RaceRendererBackend::glide &&
      !impl_->glide_initialization_attempted) {
    impl_->glide_initialization_attempted = true;
    impl_->glide_renderer = std::make_unique<hardware::GlideRenderer>();
    auto initialized = false;
    try {
      initialized = impl_->glide_renderer->initialize(renderer);
    } catch (...) {
    }
    if (!initialized) {
      impl_->glide_renderer.reset();
      impl_->backend = RaceRendererBackend::software;
    }
  }
  if ((impl_->backend == RaceRendererBackend::software ||
       impl_->checksum_enabled) &&
      impl_->software_renderer == nullptr) {
    impl_->software_renderer = std::make_unique<software::SoftwareRenderer>();
  }
}

#include <renderer/race_scene_recording.inc>

void RaceRenderer::present(SDL_Renderer *renderer) {
  const auto trilinear_filtering = std::any_of(
      impl_->commands.begin(), impl_->commands.end(), [](const auto &command) {
        return command.texture_levels != nullptr && command.trilinear_filtering;
      });
  const SceneFrameView frame{impl_->width,
                             impl_->height,
                             impl_->near_plane,
                             &impl_->commands,
                             &impl_->shadows,
                             &impl_->headlights,
                             impl_->tron_hidden_line,
                             trilinear_filtering,
                             impl_->fog_start,
                             impl_->fog_end,
                             impl_->fog_color,
                             impl_->fog_enabled,
                             impl_->focal_length,
                             &impl_->lighting};
  if (impl_->backend == RaceRendererBackend::primary_hardware &&
      !impl_->checksum_enabled) {
    if (impl_->d3d11_renderer == nullptr ||
        !impl_->d3d11_renderer->available()) {
      throw std::runtime_error(
          "the selected D3D11 renderer could not initialize");
    }
    auto *texture = impl_->d3d11_renderer->render(renderer, frame);
    require(texture != nullptr, "render D3D11 race scene");
    require(SDL_RenderTexture(renderer, texture, nullptr, nullptr),
            "compose D3D11 race scene");
    impl_->active_workers = 0U;
    impl_->render_ms = impl_->d3d11_renderer->last_render_ms();
    impl_->upload_ms = 0.0;
    return;
  }
  if (impl_->backend == RaceRendererBackend::d3d9 && !impl_->checksum_enabled) {
    if (impl_->d3d9_renderer == nullptr || !impl_->d3d9_renderer->available()) {
      throw std::runtime_error(
          "the selected D3D9 renderer could not initialize");
    }
    require(impl_->d3d9_renderer->render(renderer, frame),
            "render native D3D9 race scene");
    impl_->active_workers = 0U;
    impl_->render_ms = impl_->d3d9_renderer->last_render_ms();
    impl_->upload_ms = 0.0;
    return;
  }
  if (impl_->backend == RaceRendererBackend::d3d12 &&
      !impl_->checksum_enabled) {
    if (impl_->d3d12_renderer == nullptr ||
        !impl_->d3d12_renderer->available()) {
      throw std::runtime_error(
          "the selected D3D12 renderer could not initialize");
    }
    auto *texture = impl_->d3d12_renderer->render(renderer, frame);
    require(texture != nullptr, "render native D3D12 race scene");
    require(SDL_RenderTexture(renderer, texture, nullptr, nullptr),
            "compose native D3D12 race scene");
    impl_->active_workers = 0U;
    impl_->render_ms = impl_->d3d12_renderer->last_render_ms();
    impl_->upload_ms = 0.0;
    return;
  }
  if (impl_->backend == RaceRendererBackend::glide &&
      !impl_->checksum_enabled) {
    if (impl_->glide_renderer == nullptr ||
        !impl_->glide_renderer->available()) {
      throw std::runtime_error(
          "the selected Glide renderer could not initialize");
    }
    auto *texture = impl_->glide_renderer->render(renderer, frame);
    require(texture != nullptr, "render Glide race scene");
    require(SDL_RenderTexture(renderer, texture, nullptr, nullptr),
            "compose Glide race scene");
    impl_->active_workers = 0U;
    impl_->render_ms = impl_->glide_renderer->last_render_ms();
    impl_->upload_ms = 0.0;
    return;
  }

  if (impl_->software_renderer == nullptr) {
    impl_->software_renderer = std::make_unique<software::SoftwareRenderer>();
  }
  const auto result = impl_->software_renderer->render(
      renderer, frame, impl_->requested_workers, impl_->checksum_enabled);
  impl_->active_workers = result.active_workers;
  impl_->checksum = result.color_checksum;
  impl_->render_ms = result.raster_ms;
  impl_->upload_ms = result.upload_ms;
}

int RaceRenderer::width() const noexcept { return impl_->width; }
int RaceRenderer::height() const noexcept { return impl_->height; }
RaceRendererBackend RaceRenderer::backend() const noexcept {
  return impl_->backend;
}
unsigned int RaceRenderer::active_software_workers() const noexcept {
  return impl_->active_workers;
}
std::uint64_t RaceRenderer::color_checksum() const noexcept {
  return impl_->checksum;
}
double RaceRenderer::last_render_ms() const noexcept {
  return impl_->render_ms;
}
double RaceRenderer::last_upload_ms() const noexcept {
  return impl_->upload_ms;
}

} // namespace mh::render
