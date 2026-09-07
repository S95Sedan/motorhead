#include <renderer/software/software_renderer.hpp>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mh::render::software {
namespace {

void require(const bool condition, const char *operation) {
  if (!condition) {
    throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
  }
}

class RasterPool {
public:
  RasterPool() = default;
  RasterPool(const RasterPool &) = delete;
  RasterPool &operator=(const RasterPool &) = delete;
  ~RasterPool() {
    {
      const std::lock_guard lock(mutex_);
      quit_ = true;
    }
    available_.notify_all();
    for (auto &worker : workers_) worker.join();
  }

  void run(const std::size_t count,
           const std::function<void(std::size_t)> &job) {
    if (count == 0U) return;
    if (count == 1U) {
      job(0U);
      return;
    }
    {
      const std::lock_guard lock(mutex_);
      job_ = &job;
      count_ = count;
      next_ = 0U;
      remaining_ = count;
      failure_ = nullptr;
      ++generation_;
      while (workers_.size() + 1U < count) {
        workers_.emplace_back([this] { serve(); });
      }
    }
    available_.notify_all();
    execute();
    std::unique_lock lock(mutex_);
    complete_.wait(lock, [this] { return remaining_ == 0U; });
    job_ = nullptr;
    if (failure_ != nullptr) {
      std::rethrow_exception(std::exchange(failure_, nullptr));
    }
  }

private:
  void execute() {
    while (true) {
      const std::function<void(std::size_t)> *job = nullptr;
      std::size_t index = 0U;
      {
        const std::lock_guard lock(mutex_);
        if (next_ >= count_) return;
        index = next_++;
        job = job_;
      }
      try {
        (*job)(index);
      } catch (...) {
        const std::lock_guard lock(mutex_);
        if (failure_ == nullptr) failure_ = std::current_exception();
      }
      const std::lock_guard lock(mutex_);
      if (--remaining_ == 0U) complete_.notify_all();
    }
  }

  void serve() {
    auto generation = std::uint64_t{0U};
    while (true) {
      {
        std::unique_lock lock(mutex_);
        available_.wait(lock,
                        [this, generation] {
                          return quit_ || generation_ != generation;
                        });
        if (quit_) return;
        generation = generation_;
      }
      execute();
    }
  }

  std::mutex mutex_;
  std::condition_variable available_;
  std::condition_variable complete_;
  std::vector<std::thread> workers_;
  const std::function<void(std::size_t)> *job_ = nullptr;
  std::size_t count_ = 0U;
  std::size_t next_ = 0U;
  std::size_t remaining_ = 0U;
  std::uint64_t generation_ = 0U;
  std::exception_ptr failure_;
  bool quit_ = false;
};

struct Surface {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> color;
  std::vector<float> depth;
};

std::array<float, 4U>
sample_level(const mh::content::PamRgbaImage &image, const float u,
             const float v) {
  const auto sx = u * static_cast<float>(image.width) - 0.5F;
  const auto sy = v * static_cast<float>(image.height) - 0.5F;
  const auto x0 = static_cast<int>(std::floor(sx));
  const auto y0 = static_cast<int>(std::floor(sy));
  const auto tx = sx - std::floor(sx);
  const auto ty = sy - std::floor(sy);
  const auto wrap = [](const int value, const int extent) {
    const auto remainder = value % extent;
    return remainder < 0 ? remainder + extent : remainder;
  };
  const auto pixel = [&image, &wrap](const int x, const int y) {
    return (static_cast<std::size_t>(wrap(y, static_cast<int>(image.height))) *
                image.width +
            static_cast<std::size_t>(wrap(x, static_cast<int>(image.width)))) *
           4U;
  };
  const std::array<std::size_t, 4U> taps{
      pixel(x0, y0), pixel(x0 + 1, y0), pixel(x0, y0 + 1),
      pixel(x0 + 1, y0 + 1)};
  std::array<float, 4U> result{};
  for (std::size_t channel = 0U; channel < result.size(); ++channel) {
    const auto top = image.rgba[taps[0U] + channel] * (1.0F - tx) +
                     image.rgba[taps[1U] + channel] * tx;
    const auto bottom = image.rgba[taps[2U] + channel] * (1.0F - tx) +
                        image.rgba[taps[3U] + channel] * tx;
    result[channel] =
        (top * (1.0F - ty) + bottom * ty) * (1.0F / 255.0F);
  }
  return result;
}

std::array<float, 4U>
sample_texture(const std::vector<mh::content::PamRgbaImage> *levels,
               const bool trilinear, const float u, const float v,
               const float lod) {
  if (levels == nullptr || levels->empty()) {
    return {1.0F, 1.0F, 1.0F, 1.0F};
  }
  const auto maximum = levels->size() - 1U;
  const auto bounded = std::clamp(lod, 0.0F, static_cast<float>(maximum));
  const auto first_index = static_cast<std::size_t>(std::floor(bounded));
  const auto second_index = std::min(maximum, first_index + 1U);
  auto result = sample_level((*levels)[first_index], u, v);
  if (!trilinear || first_index == second_index) return result;
  const auto second = sample_level((*levels)[second_index], u, v);
  const auto factor = bounded - static_cast<float>(first_index);
  for (std::size_t channel = 0U; channel < result.size(); ++channel) {
    result[channel] = result[channel] * (1.0F - factor) +
                      second[channel] * factor;
  }
  return result;
}

void raster_triangle(Surface &surface, const SceneFrameView &frame,
                     const SceneRasterCommand &command,
                     const std::array<std::size_t, 3U> corners,
                     const int clip_minimum_y, const int clip_maximum_y) {
  const auto &a = command.vertices[corners[0U]];
  const auto &b = command.vertices[corners[1U]];
  const auto &c = command.vertices[corners[2U]];
  const auto edge = [](const SDL_FPoint &first, const SDL_FPoint &second,
                       const float x, const float y) {
    return (x - first.x) * (second.y - first.y) -
           (y - first.y) * (second.x - first.x);
  };
  const auto area = edge(a.position, b.position, c.position.x, c.position.y);
  if (!std::isfinite(area) || std::abs(area) < 0.000001F) return;
  const auto minimum_x = std::max(
      0, static_cast<int>(std::floor(
             std::min({a.position.x, b.position.x, c.position.x}))));
  const auto maximum_x = std::min(
      surface.width - 1,
      static_cast<int>(std::ceil(
          std::max({a.position.x, b.position.x, c.position.x}))));
  const auto minimum_y = std::max(
      clip_minimum_y,
      static_cast<int>(std::floor(
          std::min({a.position.y, b.position.y, c.position.y}))));
  const auto maximum_y = std::min(
      clip_maximum_y,
      static_cast<int>(std::ceil(
          std::max({a.position.y, b.position.y, c.position.y}))));
  if (minimum_x > maximum_x || minimum_y > maximum_y) return;
  const std::array<float, 3U> inverse_depth{
      1.0F / static_cast<float>(command.view_depths[corners[0U]]),
      1.0F / static_cast<float>(command.view_depths[corners[1U]]),
      1.0F / static_cast<float>(command.view_depths[corners[2U]])};
  const auto interpolate = [&inverse_depth](const float w0, const float w1,
                                            const float w2,
                                            const float inverse, const float x,
                                            const float y, const float z) {
    return (w0 * x * inverse_depth[0U] + w1 * y * inverse_depth[1U] +
            w2 * z * inverse_depth[2U]) /
           inverse;
  };
  for (auto y = minimum_y; y <= maximum_y; ++y) {
    for (auto x = minimum_x; x <= maximum_x; ++x) {
      const auto px = static_cast<float>(x) + 0.5F;
      const auto py = static_cast<float>(y) + 0.5F;
      const auto w0 = edge(b.position, c.position, px, py) / area;
      const auto w1 = edge(c.position, a.position, px, py) / area;
      const auto w2 = 1.0F - w0 - w1;
      if (w0 < -0.00001F || w1 < -0.00001F || w2 < -0.00001F) continue;
      const auto inverse = w0 * inverse_depth[0U] + w1 * inverse_depth[1U] +
                           w2 * inverse_depth[2U];
      if (!std::isfinite(inverse) || inverse <= 0.0F) continue;
      const auto depth = 1.0F / inverse;
      const auto tested_depth =
          command.coplanar_depth_bias
              ? depth - std::max(0.001F, depth * 0.0001F)
              : depth;
      const auto pixel = static_cast<std::size_t>(y) * surface.width + x;
      if (command.depth_test && tested_depth >= surface.depth[pixel]) continue;
      const auto u = interpolate(w0, w1, w2, inverse, a.tex_coord.x,
                                 b.tex_coord.x, c.tex_coord.x);
      const auto v = interpolate(w0, w1, w2, inverse, a.tex_coord.y,
                                 b.tex_coord.y, c.tex_coord.y);
      const auto texel = sample_texture(command.texture_levels,
                                        command.trilinear_filtering, u, v, 0.0F);
      const auto alpha =
          texel[3U] * interpolate(w0, w1, w2, inverse, a.color.a, b.color.a,
                                 c.color.a);
      if ((command.blend_mode == SceneBlendMode::opaque && alpha < 0.5F) ||
          (command.blend_mode != SceneBlendMode::opaque && alpha <= 0.0F)) {
        continue;
      }
      const auto normalized_cue =
          frame.fog_enabled
              ? std::clamp(
                    (static_cast<double>(depth) - frame.fog_start) /
                        std::max(0.0001, frame.fog_end - frame.fog_start),
                    0.0, 1.0)
              : 0.0;
      const auto clear_fraction = 1.0 - normalized_cue;
      const auto depth_cue = static_cast<float>(
          1.0 - clear_fraction * clear_fraction);
      const auto cue = depth_cue;
      const std::array<float, 3U> vertex_color{
          interpolate(w0, w1, w2, inverse, a.color.r, b.color.r, c.color.r),
          interpolate(w0, w1, w2, inverse, a.color.g, b.color.g, c.color.g),
          interpolate(w0, w1, w2, inverse, a.color.b, b.color.b, c.color.b)};
      const std::array<float, 3U> fog_color{
          frame.fog_color[0U] / 255.0F, frame.fog_color[1U] / 255.0F,
          frame.fog_color[2U] / 255.0F};
      for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const auto base = texel[channel] * vertex_color[channel];
        const auto shaded = command.blend_mode == SceneBlendMode::additive
                                ? base * (1.0F - cue)
                                : base * (1.0F - cue) + fog_color[channel] * cue;
        const auto destination =
            surface.color[pixel * 4U + channel] * (1.0F / 255.0F);
        auto output = shaded;
        if (command.blend_mode == SceneBlendMode::alpha) {
          output = shaded * alpha + destination * (1.0F - alpha);
        } else if (command.blend_mode == SceneBlendMode::additive) {
          output = destination + shaded * alpha;
        }
        surface.color[pixel * 4U + channel] = static_cast<std::uint8_t>(
            std::clamp(output, 0.0F, 1.0F) * 255.0F + 0.5F);
      }
      surface.color[pixel * 4U + 3U] = 255U;
      if (command.depth_test && command.blend_mode == SceneBlendMode::opaque) {
        surface.depth[pixel] = tested_depth;
      }
    }
  }
}

bool contains_xz(const VehicleShadowReceiverCell &cell, const double x,
                 const double z) {
  const auto triangle = [x, z](const auto &a, const auto &b, const auto &c) {
    const auto denominator =
        (b[1U] - c[1U]) * (a[0U] - c[0U]) +
        (c[0U] - b[0U]) * (a[1U] - c[1U]);
    if (std::abs(denominator) <= 1.0e-12) return false;
    const auto wa = ((b[1U] - c[1U]) * (x - c[0U]) +
                     (c[0U] - b[0U]) * (z - c[1U])) /
                    denominator;
    const auto wb = ((c[1U] - a[1U]) * (x - c[0U]) +
                     (a[0U] - c[0U]) * (z - c[1U])) /
                    denominator;
    const auto wc = 1.0 - wa - wb;
    return wa >= -1.0e-6 && wb >= -1.0e-6 && wc >= -1.0e-6;
  };
  return triangle(cell.world_xz[0U], cell.world_xz[1U], cell.world_xz[2U]) ||
         triangle(cell.world_xz[0U], cell.world_xz[2U], cell.world_xz[3U]);
}

std::array<double, 3U> reconstruct(const PerspectiveView &view, const int x,
                                  const int y, const float depth) {
  const auto vx = (static_cast<double>(x) + 0.5 - view.center_x) /
                  view.focal_length * depth;
  const auto vy = -(static_cast<double>(y) + 0.5 - view.center_y) /
                  view.focal_length * depth;
  std::array<double, 3U> world{};
  for (std::size_t axis = 0U; axis < world.size(); ++axis) {
    world[axis] = view.position[axis] + view.right[axis] * vx +
                  view.up[axis] * vy + view.forward[axis] * depth;
  }
  return world;
}

float cue_factor(const float depth, const double render_distance,
                 const double cue_start, const bool enabled) {
  if (!enabled || depth <= cue_start) return 0.0F;
  const auto linear = static_cast<float>(std::clamp(
      (depth - cue_start) / std::max(0.0001, render_distance - cue_start),
      0.0, 1.0));
  const auto clear_fraction = 1.0F - linear;
  return 1.0F - clear_fraction * clear_fraction * clear_fraction;
}

void apply_shadow(Surface &surface,
                  const VehicleShadowDepthProjection &projection,
                  const int clip_minimum_y, const int clip_maximum_y) {
  const auto first_y = std::max(projection.minimum_y, clip_minimum_y);
  const auto last_y = std::min(projection.maximum_y, clip_maximum_y);
  for (auto y = first_y; y <= last_y; ++y) {
    for (auto x = projection.minimum_x; x <= projection.maximum_x; ++x) {
      const auto pixel = static_cast<std::size_t>(y) * surface.width + x;
      const auto depth = surface.depth[pixel];
      if (!std::isfinite(depth) || depth <= 0.0F) continue;
      const auto world = reconstruct(projection.view, x, y, depth);
      if (world[0U] < projection.minimum_world_x ||
          world[0U] > projection.maximum_world_x ||
          world[2U] < projection.minimum_world_z ||
          world[2U] > projection.maximum_world_z) continue;
      auto accepted = false;
      for (const auto &cell : projection.cells) {
        if (cell.valid && world[1U] >= cell.minimum_receiver_y &&
            world[1U] <= cell.maximum_receiver_y &&
            contains_xz(cell, world[0U], world[2U])) {
          accepted = true;
          break;
        }
      }
      if (!accepted) continue;
      const auto cue = cue_factor(depth, projection.render_distance,
                                  projection.cue_start,
                                  projection.cue_enabled);
      for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const auto output = (10.0F / 255.0F) * (1.0F - cue) +
                            projection.cue_color[channel] / 255.0F * cue;
        surface.color[pixel * 4U + channel] = static_cast<std::uint8_t>(
            std::clamp(output, 0.0F, 1.0F) * 255.0F + 0.5F);
      }
      surface.color[pixel * 4U + 3U] = 255U;
    }
  }
}

void apply_headlights(Surface &surface,
                      const std::vector<HeadlightDepthProjection> &lights) {
  constexpr std::array<double, 7U> distances{0.35, 2.0, 5.0, 8.0,
                                             11.0, 15.0, 20.0};
  for (const auto &light : lights) {
    const auto center_z =
        (light.sources[0U].center[2U] + light.sources[1U].center[2U]) * 0.5;
    const auto direction_z = (light.sources[0U].direction[2U] +
                              light.sources[1U].direction[2U]) * 0.5;
    if (std::abs(direction_z) < 1.0e-9) continue;
    for (auto y = light.minimum_y; y <= light.maximum_y; ++y) {
      for (auto x = light.minimum_x; x <= light.maximum_x; ++x) {
        const auto pixel = static_cast<std::size_t>(y) * surface.width + x;
        const auto depth = surface.depth[pixel];
        if (!std::isfinite(depth) || depth <= 0.0F) continue;
        const auto world = reconstruct(light.view, x, y, depth);
        std::array<double, 3U> delta{};
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
          delta[axis] = world[axis] - light.vehicle.world_position[axis];
        }
        std::array<double, 3U> local{};
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
          for (std::size_t component = 0U; component < 3U; ++component) {
            local[axis] += delta[component] *
                           light.vehicle.body_basis[axis][component];
          }
        }
        const auto distance = (local[2U] - center_z) / direction_z;
        if (distance < distances.front() || distance > distances.back())
          continue;
        const auto left = light.sources[0U].center[0U] +
                          light.sources[0U].direction[0U] * distance;
        const auto right = light.sources[1U].center[0U] +
                           light.sources[1U].direction[0U] * distance;
        if (std::abs(right - left) < 1.0e-9) continue;
        constexpr double lateral_margin = 0.5;
        const auto authored_lateral = (local[0U] - left) / (right - left);
        const auto lateral =
            (authored_lateral + lateral_margin) / (1.0 + lateral_margin * 2.0);
        if (lateral < 0.0 || lateral > 1.0) continue;
        // A sampled five-column fan leaves a visible derivative seam on broad
        // road polygons.  Evaluate the same centre-weighted beam continuously
        // so adjacent pixels cannot expose the old authored column boundaries.
        const auto edge = std::clamp(1.0 - std::abs(lateral * 2.0 - 1.0),
                                     0.0, 1.0);
        const auto coverage = edge * edge * (3.0 - 2.0 * edge);
        const auto remaining =
            std::clamp(1.0 - distance / distances.back(), 0.0, 1.0);
        const auto fade = std::clamp((distance - distances.front()) /
                                         (distances[1U] - distances.front()),
                                     0.0, 1.0);
        auto intensity = static_cast<float>(fade * remaining * remaining *
                                            coverage * 0.46);
        intensity *= 1.0F - cue_factor(depth, light.render_distance,
                                       light.cue_start, light.cue_enabled);
        const auto addition = (150.0F / 255.0F) * intensity * 255.0F;
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
          surface.color[pixel * 4U + channel] =
              static_cast<std::uint8_t>(std::clamp(
                  surface.color[pixel * 4U + channel] + addition, 0.0F,
                  255.0F));
        }
      }
    }
  }
}

} // namespace

struct SoftwareRenderer::Impl {
  Surface surface;
  RasterPool pool;
  SDL_Texture *texture = nullptr;

  ~Impl() {
    if (texture != nullptr) SDL_DestroyTexture(texture);
  }

  void resize(SDL_Renderer *renderer, const int width, const int height) {
    if (surface.width == width && surface.height == height &&
        texture != nullptr) return;
    if (texture != nullptr) SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STREAMING, width, height);
    require(texture != nullptr, "create software scene texture");
    require(SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND),
            "set software scene blend mode");
    require(SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR),
            "set software scene scale mode");
    surface.width = width;
    surface.height = height;
  }
};

SoftwareRenderer::SoftwareRenderer() : impl_(std::make_unique<Impl>()) {}
SoftwareRenderer::~SoftwareRenderer() = default;

SoftwareRenderResult
SoftwareRenderer::render(SDL_Renderer *renderer, const SceneFrameView &frame,
                         const unsigned int requested_workers,
                         const bool checksum_enabled) {
  if (renderer == nullptr || frame.commands == nullptr ||
      frame.shadow_projections == nullptr ||
      frame.headlight_projections == nullptr || frame.width <= 0 ||
      frame.height <= 0) {
    throw std::runtime_error("software scene frame is incomplete");
  }
  impl_->resize(renderer, frame.width, frame.height);
  const auto pixels = static_cast<std::size_t>(frame.width) * frame.height;
  impl_->surface.color.assign(pixels * 4U, 0U);
  impl_->surface.depth.assign(pixels, std::numeric_limits<float>::infinity());
  const auto started = std::chrono::steady_clock::now();
  constexpr unsigned int maximum_automatic_workers = 20U;
  const auto detected = std::thread::hardware_concurrency();
  const auto available = requested_workers == 0U
                             ? std::min(detected == 0U ? 1U : detected,
                                        maximum_automatic_workers)
                             : requested_workers;
  const auto workers = std::clamp(
      available == 0U ? 1U : available, 1U,
      static_cast<unsigned int>(std::max(1, frame.height / 32)));
  impl_->pool.run(workers, [this, &frame, workers](const std::size_t worker) {
    const auto minimum_y = static_cast<int>(
        static_cast<std::uint64_t>(frame.height) * worker / workers);
    const auto maximum_y =
        static_cast<int>(static_cast<std::uint64_t>(frame.height) *
                         (worker + 1U) / workers) -
        1;
    for (const auto &command : *frame.commands) {
      if (command.maximum_y < minimum_y || command.minimum_y > maximum_y)
        continue;
      if (command.shadow_projection_index.has_value()) {
        apply_shadow(impl_->surface,
                     frame.shadow_projections->at(
                         *command.shadow_projection_index),
                     minimum_y, maximum_y);
        continue;
      }
      if (command.vertex_count < 3U || command.vertex_count > 4U) continue;
      raster_triangle(impl_->surface, frame, command, {0U, 1U, 2U}, minimum_y,
                       maximum_y);
      if (command.vertex_count == 4U) {
        raster_triangle(impl_->surface, frame, command, {0U, 2U, 3U}, minimum_y,
                         maximum_y);
      }
    }
  });
  apply_headlights(impl_->surface, *frame.headlight_projections);
  if (frame.tron_hidden_line) {
    for (std::size_t pixel = 0U; pixel < pixels; ++pixel) {
      if (!std::isfinite(impl_->surface.depth[pixel])) continue;
      impl_->surface.color[pixel * 4U] = 0U;
      impl_->surface.color[pixel * 4U + 1U] = 0U;
      impl_->surface.color[pixel * 4U + 2U] = 128U;
      impl_->surface.color[pixel * 4U + 3U] = 255U;
    }
  }
  SoftwareRenderResult result;
  result.active_workers = workers;
  if (checksum_enabled) {
    auto checksum = UINT64_C(14695981039346656037);
    for (const auto byte : impl_->surface.color) {
      checksum ^= byte;
      checksum *= UINT64_C(1099511628211);
    }
    result.color_checksum = checksum;
  }
  const auto raster_finished = std::chrono::steady_clock::now();
  require(SDL_UpdateTexture(impl_->texture, nullptr,
                            impl_->surface.color.data(), frame.width * 4),
          "upload software scene texture");
  require(SDL_RenderTexture(renderer, impl_->texture, nullptr, nullptr),
          "compose software scene texture");
  const auto upload_finished = std::chrono::steady_clock::now();
  result.raster_ms = std::chrono::duration<double, std::milli>(
                         raster_finished - started)
                         .count();
  result.upload_ms = std::chrono::duration<double, std::milli>(
                         upload_finished - raster_finished)
                         .count();
  return result;
}

} // namespace mh::render::software
