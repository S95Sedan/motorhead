#include <ui/frontend/renderer.hpp>

#include <ui/frontend/layout.hpp>

#include <core/error.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <numeric>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace mh::ui {

FrontEndPresentationRect
front_end_presentation_rect(const std::uint32_t output_width,
                            const std::uint32_t output_height) noexcept {
  if (output_width == 0U || output_height == 0U) {
    return {};
  }
  constexpr float original_aspect = 4.0F / 3.0F;
  const auto width = static_cast<float>(output_width);
  const auto height = static_cast<float>(output_height);
  const auto available_aspect = width / height;
  if (available_aspect > original_aspect) {
    const auto presented_width = height * original_aspect;
    return {(width - presented_width) * 0.5F, 0.0F, presented_width, height};
  }
  const auto presented_height = width / original_aspect;
  return {0.0F, (height - presented_height) * 0.5F, width, presented_height};
}

FrontEndPresentationRect front_end_widescreen_presentation_rect(
    const std::uint32_t output_width,
    const std::uint32_t output_height) noexcept {
  if (output_width == 0U || output_height == 0U) {
    return {};
  }
  constexpr float widescreen_aspect = 16.0F / 9.0F;
  const auto width = static_cast<float>(output_width);
  const auto height = static_cast<float>(output_height);
  const auto available_aspect = width / height;
  if (available_aspect > widescreen_aspect) {
    const auto presented_width = height * widescreen_aspect;
    return {(width - presented_width) * 0.5F, 0.0F, presented_width, height};
  }
  const auto presented_height = width / widescreen_aspect;
  return {0.0F, (height - presented_height) * 0.5F, width, presented_height};
}

double front_end_menu_line_object_scale(const mh::content::LobData &line_object,
                                        const double target_radius) noexcept {
  if (!(target_radius > 0.0) || !std::isfinite(target_radius) ||
      line_object.vertices.empty()) {
    return 0.0;
  }
  const auto center_x =
      (line_object.summary.minimum[0U] + line_object.summary.maximum[0U]) * 0.5;
  const auto center_y =
      (line_object.summary.minimum[1U] + line_object.summary.maximum[1U]) * 0.5;
  const auto center_z =
      (line_object.summary.minimum[2U] + line_object.summary.maximum[2U]) * 0.5;
  const auto radius =
      std::max({std::abs(line_object.summary.minimum[0U] - center_x),
                std::abs(line_object.summary.maximum[0U] - center_x),
                std::abs(line_object.summary.minimum[1U] - center_y),
                std::abs(line_object.summary.maximum[1U] - center_y),
                std::abs(line_object.summary.minimum[2U] - center_z),
                std::abs(line_object.summary.maximum[2U] - center_z)});
  return radius > 0.0 ? target_radius / radius : 0.0;
}

namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::uint32_t retail_random(std::uint32_t &state) noexcept {
  state = state * 214013U + 2531011U;
  return (state >> 16U) & 0x7fffU;
}

std::uint8_t line_outcode(const double x, const double y,
                          const std::uint32_t width,
                          const std::uint32_t height) noexcept {
  std::uint8_t result = 0U;
  if (x < 0.0) {
    result |= 1U;
  } else if (x > static_cast<double>(width - 1U)) {
    result |= 2U;
  }
  if (y < 0.0) {
    result |= 4U;
  } else if (y > static_cast<double>(height - 1U)) {
    result |= 8U;
  }
  return result;
}

bool clip_front_end_line(double &x0, double &y0, double &x1, double &y1,
                         const std::uint32_t width,
                         const std::uint32_t height) noexcept {
  auto code0 = line_outcode(x0, y0, width, height);
  auto code1 = line_outcode(x1, y1, width, height);
  while (true) {
    if ((code0 | code1) == 0U) {
      return true;
    }
    if ((code0 & code1) != 0U) {
      return false;
    }
    const auto code = code0 != 0U ? code0 : code1;
    double x = 0.0;
    double y = 0.0;
    if ((code & 8U) != 0U) {
      y = static_cast<double>(height - 1U);
      x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
    } else if ((code & 4U) != 0U) {
      y = 0.0;
      x = x0 + (x1 - x0) * (y - y0) / (y1 - y0);
    } else if ((code & 2U) != 0U) {
      x = static_cast<double>(width - 1U);
      y = y0 + (y1 - y0) * (x - x0) / (x1 - x0);
    } else {
      x = 0.0;
      y = y0 + (y1 - y0) * (x - x0) / (x1 - x0);
    }
    if (code == code0) {
      x0 = x;
      y0 = y;
      code0 = line_outcode(x0, y0, width, height);
    } else {
      x1 = x;
      y1 = y;
      code1 = line_outcode(x1, y1, width, height);
    }
  }
}

void draw_front_end_line(mh::content::TgaImage &target, double x0, double y0,
                         double x1, double y1,
                         const std::uint8_t palette_increment,
                         FrontEndBackgroundRenderCache &cache) {
  if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) ||
      !std::isfinite(y1) ||
      !clip_front_end_line(x0, y0, x1, y1, target.width, target.height)) {
    return;
  }
  auto ax = static_cast<std::int32_t>(std::nearbyint(x0));
  auto ay = static_cast<std::int32_t>(std::nearbyint(y0));
  const auto bx = static_cast<std::int32_t>(std::nearbyint(x1));
  const auto by = static_cast<std::int32_t>(std::nearbyint(y1));
  const auto dx = std::abs(bx - ax);
  const auto sx = ax < bx ? 1 : -1;
  const auto dy = -std::abs(by - ay);
  const auto sy = ay < by ? 1 : -1;
  auto error = dx + dy;
  while (true) {
    const auto pixel = static_cast<std::size_t>(ay) * target.width +
                       static_cast<std::size_t>(ax);
    if (cache.dirty_marks[pixel] == 0U) {
      cache.dirty_marks[pixel] = 1U;
      cache.dirty_pixels.push_back(static_cast<std::uint32_t>(pixel));
    }
    const auto sum = static_cast<std::uint32_t>(target.palette_indices[pixel]) +
                     palette_increment;
    target.palette_indices[pixel] =
        static_cast<std::uint8_t>(std::min(sum, 255U));
    if (ax == bx && ay == by) {
      break;
    }
    const auto twice_error = error * 2;
    if (twice_error >= dy) {
      error += dy;
      ax += sx;
    }
    if (twice_error <= dx) {
      error += dx;
      ay += sy;
    }
  }
}

struct FrontEndOrbitSegment {
  std::array<double, 2U> first{};
  std::array<double, 2U> second{};
  std::array<double, 2U> outer{};
  bool outer_from_first = false;
  bool has_outer = false;
};

const std::array<FrontEndOrbitSegment, 24U> &front_end_orbit_geometry() {
  static const auto geometry = [] {
    std::array<FrontEndOrbitSegment, 24U> result{};
    constexpr std::uint32_t quarter_segments = 6U;
    constexpr double half_pi = 1.5707963267948966192313216916398;
    constexpr double step = half_pi / quarter_segments;
    constexpr double seam_offset = step * 0.1;
    for (std::uint32_t index = 0U; index < result.size(); ++index) {
      const auto quadrant = index % 4U;
      auto first_angle = static_cast<double>(index) * step;
      auto second_angle = static_cast<double>(index + 1U) * step;
      if (quadrant == 0U) {
        first_angle += seam_offset;
      }
      if (quadrant == 1U) {
        second_angle -= seam_offset;
      }
      auto &segment = result[index];
      segment.first = {std::cos(first_angle), std::sin(first_angle)};
      segment.second = {std::cos(second_angle), std::sin(second_angle)};
      if (quadrant == 2U) {
        segment.outer = {std::cos(first_angle - seam_offset),
                         std::sin(first_angle - seam_offset)};
        segment.outer_from_first = true;
        segment.has_outer = true;
      } else if (quadrant == 3U) {
        segment.outer = {std::cos(second_angle + seam_offset),
                         std::sin(second_angle + seam_offset)};
        segment.has_outer = true;
      }
    }
    return result;
  }();
  return geometry;
}

void draw_front_end_orbit(mh::content::TgaImage &target,
                          const std::int32_t center_x,
                          const std::int32_t center_y, const double radius,
                          const double phase_sine, const double phase_cosine,
                          const std::uint8_t palette_increment,
                          FrontEndBackgroundRenderCache &cache) {
  constexpr double vertical_scale = 0.8333333134651184;
  const auto inset = radius * 0.2;
  const auto point = [center_x, center_y, phase_sine, phase_cosine](
                         const std::array<double, 2U> &unit,
                         const double point_radius) {
    const auto rotated_cosine =
        unit[0U] * phase_cosine - unit[1U] * phase_sine;
    const auto rotated_sine =
        unit[1U] * phase_cosine + unit[0U] * phase_sine;
    return std::array<double, 2U>{
        static_cast<double>(center_x) + rotated_cosine * point_radius,
        static_cast<double>(center_y) + rotated_sine * point_radius *
                                            vertical_scale};
  };

  std::uint32_t index = 0U;
  for (const auto &segment : front_end_orbit_geometry()) {
    const auto ring_radius = radius - (((index / 2U) & 1U) != 0U ? inset : 0.0);
    const auto first = point(segment.first, ring_radius);
    const auto second = point(segment.second, ring_radius);
    draw_front_end_line(target, first[0U], first[1U], second[0U], second[1U],
                        palette_increment, cache);

    if (segment.has_outer) {
      const auto outer = point(segment.outer, radius);
      const auto &anchor = segment.outer_from_first ? first : second;
      draw_front_end_line(target, anchor[0U], anchor[1U], outer[0U], outer[1U],
                          palette_increment, cache);
    }
    ++index;
  }
}

struct FrontEndPhaseTrigonometry {
  std::array<double, 4096U> sine{};
  std::array<double, 4096U> cosine{};
};

const FrontEndPhaseTrigonometry &front_end_phase_trigonometry() {
  static const auto values = [] {
    FrontEndPhaseTrigonometry result;
    constexpr double tau = 6.283185307179586476925286766559;
    constexpr double scale = tau / 4096.0;
    for (std::size_t index = 0U; index < result.sine.size(); ++index) {
      const auto angle = static_cast<double>(index) * scale;
      result.sine[index] = std::sin(angle);
      result.cosine[index] = std::cos(angle);
    }
    return result;
  }();
  return values;
}

void blit_rgba(FrontEndFrame &target,
               const std::span<const std::uint8_t> source,
               const std::uint32_t width, const std::uint32_t height,
               const std::int32_t x, const std::int32_t y) {
  const auto expected = static_cast<std::uint64_t>(width) * height * 4U;
  if (expected != source.size()) {
    throw ToolError(ExitCode::format,
                    "front-end source image has inconsistent RGBA storage");
  }
  for (std::uint32_t source_y = 0U; source_y < height; ++source_y) {
    const auto destination_y = static_cast<std::int64_t>(y) + source_y;
    if (destination_y < 0 ||
        destination_y >= static_cast<std::int64_t>(target.height)) {
      continue;
    }
    for (std::uint32_t source_x = 0U; source_x < width; ++source_x) {
      const auto destination_x = static_cast<std::int64_t>(x) + source_x;
      if (destination_x < 0 ||
          destination_x >= static_cast<std::int64_t>(target.width)) {
        continue;
      }
      const auto source_pixel =
          (static_cast<std::size_t>(source_y) * width + source_x) * 4U;
      const auto alpha = source[source_pixel + 3U];
      if (alpha == 0U) {
        continue;
      }
      const auto destination_pixel =
          (static_cast<std::size_t>(destination_y) * target.width +
           static_cast<std::size_t>(destination_x)) *
          4U;
      if (alpha == 255U) {
        for (std::size_t channel = 0U; channel < 4U; ++channel) {
          target.rgba[destination_pixel + channel] =
              source[source_pixel + channel];
        }
        continue;
      }
      const auto inverse = 255U - alpha;
      for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const auto blended =
            static_cast<std::uint32_t>(source[source_pixel + channel]) * alpha +
            static_cast<std::uint32_t>(
                target.rgba[destination_pixel + channel]) *
                inverse;
        target.rgba[destination_pixel + channel] =
            static_cast<std::uint8_t>((blended + 127U) / 255U);
      }
      target.rgba[destination_pixel + 3U] = 255U;
    }
  }
}

void blit_rgba_inside_outline(
    FrontEndFrame &target, const std::span<const std::uint8_t> source,
    const std::uint32_t width, const std::uint32_t height,
    const std::span<const std::uint8_t> outline,
    const std::uint32_t outline_width, const std::uint32_t outline_height,
    const std::int32_t outline_x, const std::int32_t outline_y) {
  const auto source_bytes = static_cast<std::uint64_t>(width) * height * 4U;
  const auto outline_bytes =
      static_cast<std::uint64_t>(outline_width) * outline_height * 4U;
  if (source_bytes != source.size() || outline_bytes != outline.size() ||
      width > outline_width || height > outline_height) {
    throw ToolError(ExitCode::format,
                    "front-end outlined image has inconsistent RGBA storage");
  }

  // Derive the mask from the authored ring itself. This avoids guessing an
  // ellipse radius and remains correct if a later data set changes the frame.
  std::vector<std::int32_t> left_edges(
      outline_height, static_cast<std::int32_t>(outline_width));
  std::vector<std::int32_t> right_edges(outline_height, -1);
  for (std::uint32_t row = 0U; row < outline_height; ++row) {
    for (std::uint32_t column = 0U; column < outline_width; ++column) {
      const auto pixel =
          (static_cast<std::size_t>(row) * outline_width + column) * 4U;
      if (outline[pixel + 3U] == 0U) {
        continue;
      }
      left_edges[row] =
          std::min(left_edges[row], static_cast<std::int32_t>(column));
      right_edges[row] =
          std::max(right_edges[row], static_cast<std::int32_t>(column));
    }
  }

  const auto source_offset_x =
      static_cast<std::int32_t>((outline_width - width) / 2U);
  const auto source_offset_y =
      static_cast<std::int32_t>((outline_height - height) / 2U);
  for (std::uint32_t source_y = 0U; source_y < height; ++source_y) {
    const auto mask_y = static_cast<std::int32_t>(source_y) + source_offset_y;
    if (mask_y < 0 || mask_y >= static_cast<std::int32_t>(outline_height) ||
        right_edges[static_cast<std::size_t>(mask_y)] < 0) {
      continue;
    }
    const auto destination_y =
        static_cast<std::int64_t>(outline_y) + source_offset_y + source_y;
    if (destination_y < 0 ||
        destination_y >= static_cast<std::int64_t>(target.height)) {
      continue;
    }
    for (std::uint32_t source_x = 0U; source_x < width; ++source_x) {
      const auto mask_x = static_cast<std::int32_t>(source_x) + source_offset_x;
      if (mask_x < left_edges[static_cast<std::size_t>(mask_y)] ||
          mask_x > right_edges[static_cast<std::size_t>(mask_y)]) {
        continue;
      }
      const auto destination_x =
          static_cast<std::int64_t>(outline_x) + source_offset_x + source_x;
      if (destination_x < 0 ||
          destination_x >= static_cast<std::int64_t>(target.width)) {
        continue;
      }
      const auto source_pixel =
          (static_cast<std::size_t>(source_y) * width + source_x) * 4U;
      const auto alpha = source[source_pixel + 3U];
      if (alpha == 0U) {
        continue;
      }
      const auto destination_pixel =
          (static_cast<std::size_t>(destination_y) * target.width +
           static_cast<std::size_t>(destination_x)) *
          4U;
      const auto inverse = 255U - alpha;
      for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const auto blended =
            static_cast<std::uint32_t>(source[source_pixel + channel]) * alpha +
            static_cast<std::uint32_t>(
                target.rgba[destination_pixel + channel]) *
                inverse;
        target.rgba[destination_pixel + channel] =
            static_cast<std::uint8_t>((blended + 127U) / 255U);
      }
      target.rgba[destination_pixel + 3U] = 255U;
    }
  }
}

std::uint32_t glitch_source_row(const std::uint32_t height,
                                const std::uint32_t destination_y,
                                const std::uint32_t phase) noexcept {
  constexpr double full_turn = 6.283185307179586476925286766559;
  const auto sine = static_cast<float>(
      std::sin(full_turn * static_cast<double>(phase & 4095U) / 4096.0));
  // RVA 0x19884 negates the sine sample, truncates sine*1024, then
  // constructs one source-row entry for every destination scanline.
  const auto factor =
      static_cast<std::int32_t>(-static_cast<double>(sine) * 1024.0);
  const auto maximum_row = static_cast<std::int32_t>(height - 1U);
  const auto step = (factor * maximum_row) >> 10;
  const auto middle = static_cast<std::int32_t>(height / 2U);
  const auto distance = middle - static_cast<std::int32_t>(destination_y);
  const auto source_y =
      middle + (maximum_row == 0 ? 0 : (distance * step) / maximum_row);
  return static_cast<std::uint32_t>(std::clamp(source_y, 0, maximum_row));
}

void blit_rgba_glitched(FrontEndFrame &target,
                        const std::span<const std::uint8_t> source,
                        const std::uint32_t width, const std::uint32_t height,
                        const std::int32_t x, const std::int32_t y,
                        const std::uint32_t phase) {
  const auto expected = static_cast<std::uint64_t>(width) * height * 4U;
  if (height == 0U || expected != source.size()) {
    throw ToolError(ExitCode::format,
                    "front-end glitch source has inconsistent RGBA storage");
  }
  std::vector<std::uint8_t> remapped(source.size());
  const auto row_bytes = static_cast<std::size_t>(width) * 4U;
  for (std::uint32_t destination_y = 0U; destination_y < height;
       ++destination_y) {
    const auto source_y = glitch_source_row(height, destination_y, phase);
    const auto source_start = static_cast<std::size_t>(source_y) * row_bytes;
    const auto destination_start =
        static_cast<std::size_t>(destination_y) * row_bytes;
    std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(source_start),
                static_cast<std::ptrdiff_t>(row_bytes),
                remapped.begin() +
                    static_cast<std::ptrdiff_t>(destination_start));
  }
  blit_rgba(target, remapped, width, height, x, y);
}

void blit_rgba_transition_mode8(FrontEndFrame &target,
                                const std::span<const std::uint8_t> source,
                                const std::uint32_t width,
                                const std::uint32_t height,
                                const std::int32_t x, const std::int32_t y,
                                const std::uint32_t transition_phase,
                                const std::int32_t authored_scalar) {
  const auto expected = static_cast<std::uint64_t>(width) * height * 4U;
  if (height == 0U || expected != source.size()) {
    throw ToolError(
        ExitCode::format,
        "front-end transition source has inconsistent RGBA storage");
  }

  // Mode 8 in the p3.1 dispatcher computes
  // 1 + direct[3] * phase * (1 / 256000), then RVA 0x19884 builds a
  // centered source-scanline table from the negated 10-bit fixed-point
  // factor. A zero phase is therefore the identity mapping.
  constexpr double transition_scale = 1.0 / 256000.0;
  const auto factor = 1.0 + static_cast<double>(authored_scalar) *
                                static_cast<double>(transition_phase) *
                                transition_scale;
  const auto fixed_factor = static_cast<std::int32_t>(-factor * 1024.0);
  const auto maximum_row = static_cast<std::int32_t>(height - 1U);
  const auto step = (fixed_factor * maximum_row) >> 10;
  const auto middle = static_cast<std::int32_t>(height / 2U);

  std::vector<std::uint8_t> remapped(source.size());
  const auto row_bytes = static_cast<std::size_t>(width) * 4U;
  for (std::uint32_t destination_y = 0U; destination_y < height;
       ++destination_y) {
    const auto distance = middle - static_cast<std::int32_t>(destination_y);
    const auto source_row =
        middle + (maximum_row == 0 ? 0 : (distance * step) / maximum_row);
    const auto clamped_row = std::clamp(source_row, 0, maximum_row);
    const auto source_start = static_cast<std::size_t>(clamped_row) * row_bytes;
    const auto destination_start =
        static_cast<std::size_t>(destination_y) * row_bytes;
    std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(source_start),
                static_cast<std::ptrdiff_t>(row_bytes),
                remapped.begin() +
                    static_cast<std::ptrdiff_t>(destination_start));
  }
  blit_rgba(target, remapped, width, height, x, y);
}

void blit_rgba_transition_mode7(FrontEndFrame &target,
                                const std::span<const std::uint8_t> source,
                                const std::uint32_t width,
                                const std::uint32_t height,
                                const std::int32_t x, const std::int32_t y,
                                const std::uint32_t transition_phase,
                                const std::int32_t authored_scalar) {
  const auto expected = static_cast<std::uint64_t>(width) * height * 4U;
  if (height == 0U || expected != source.size()) {
    throw ToolError(
        ExitCode::format,
        "front-end transition source has inconsistent RGBA storage");
  }

  // Mode 7 reaches RVA 0x1976c. Unlike mode 8's centred scanline table,
  // this table is anchored at the first row:
  //   source_y = y * ((1 + scalar * phase / 256000) * (height - 1))
  //                  / (height - 1)
  // The original rasterizer clips table entries outside the source.
  constexpr double transition_scale = 1.0 / 256000.0;
  const auto factor = 1.0 + static_cast<double>(authored_scalar) *
                                static_cast<double>(transition_phase) *
                                transition_scale;
  const auto fixed_factor = static_cast<std::int32_t>(factor * 1024.0);
  const auto maximum_row = static_cast<std::int32_t>(height - 1U);
  const auto step = (fixed_factor * maximum_row) >> 10;

  std::vector<std::uint8_t> remapped(source.size());
  const auto row_bytes = static_cast<std::size_t>(width) * 4U;
  for (std::uint32_t destination_y = 0U; destination_y < height;
       ++destination_y) {
    const auto source_row =
        maximum_row == 0
            ? 0
            : (static_cast<std::int32_t>(destination_y) * step) / maximum_row;
    const auto clamped_row = std::clamp(source_row, 0, maximum_row);
    const auto source_start = static_cast<std::size_t>(clamped_row) * row_bytes;
    const auto destination_start =
        static_cast<std::size_t>(destination_y) * row_bytes;
    std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(source_start),
                static_cast<std::ptrdiff_t>(row_bytes),
                remapped.begin() +
                    static_cast<std::ptrdiff_t>(destination_start));
  }
  blit_rgba(target, remapped, width, height, x, y);
}

void blit_rgba_transition_mode6(FrontEndFrame &target,
                                const std::span<const std::uint8_t> source,
                                const std::uint32_t width,
                                const std::uint32_t height,
                                const std::int32_t x, const std::int32_t y,
                                const std::uint32_t transition_phase,
                                const std::int32_t authored_scalar) {
  const auto expected = static_cast<std::uint64_t>(width) * height * 4U;
  if (height == 0U || expected != source.size()) {
    throw ToolError(
        ExitCode::format,
        "front-end transition source has inconsistent RGBA storage");
  }

  // The p3.1 mode-6 branch at RVA 0x7e8f7 computes this factor and passes it
  // to RVA 0x199c4. That renderer builds a destination-row table whose count
  // is ((height - 1) * round(factor * 1024)) >> 10 and maps each expanded
  // row back to source row (destination * height) / count.
  constexpr double transition_scale = 1.0 / 256000.0;
  const auto factor = 1.0 + static_cast<double>(authored_scalar) *
                                static_cast<double>(transition_phase) *
                                transition_scale;
  const auto fixed_factor =
      static_cast<std::int64_t>(std::llround(factor * 1024.0));
  const auto expanded_height_64 =
      (static_cast<std::int64_t>(height - 1U) * fixed_factor) >> 10;
  if (expanded_height_64 <= 0) {
    return;
  }
  const auto expanded_height =
      static_cast<std::uint32_t>(std::min<std::int64_t>(
          expanded_height_64, std::numeric_limits<std::uint32_t>::max()));

  std::vector<std::uint8_t> remapped(static_cast<std::size_t>(width) *
                                     expanded_height * 4U);
  const auto row_bytes = static_cast<std::size_t>(width) * 4U;
  for (std::uint32_t destination_y = 0U; destination_y < expanded_height;
       ++destination_y) {
    const auto source_y = std::min<std::uint32_t>(
        height - 1U, static_cast<std::uint32_t>(
                         (static_cast<std::uint64_t>(destination_y) * height) /
                         expanded_height));
    const auto source_start = static_cast<std::size_t>(source_y) * row_bytes;
    const auto destination_start =
        static_cast<std::size_t>(destination_y) * row_bytes;
    std::copy_n(source.begin() + static_cast<std::ptrdiff_t>(source_start),
                static_cast<std::ptrdiff_t>(row_bytes),
                remapped.begin() +
                    static_cast<std::ptrdiff_t>(destination_start));
  }
  blit_rgba(target, remapped, width, expanded_height, x, y);
}

void blit_rgba_scanline_noise(FrontEndFrame &target,
                              const std::span<const std::uint8_t> source,
                              const std::uint32_t width,
                              const std::uint32_t height, const std::int32_t x,
                              const std::int32_t y,
                              const std::int32_t amplitude,
                              const std::uint32_t noise_seed) {
  const auto expected = static_cast<std::uint64_t>(width) * height * 4U;
  if (height == 0U || expected != source.size()) {
    throw ToolError(
        ExitCode::format,
        "front-end transition source has inconsistent RGBA storage");
  }

  std::uint32_t noise = noise_seed;
  const auto row_bytes = static_cast<std::size_t>(width) * 4U;
  for (std::uint32_t source_y = 0U; source_y < height; ++source_y) {
    noise = noise * 0x41c64e6dU + 0x3039U + source_y * 0x6c9U;
    const auto centred =
        static_cast<std::int32_t>((noise >> 19U) & 0x1fffU) - 4096;
    const auto displacement = static_cast<std::int32_t>(
        (static_cast<std::int64_t>(centred) * amplitude) >> 16);
    const auto source_start = static_cast<std::size_t>(source_y) * row_bytes;
    blit_rgba(target, source.subspan(source_start, row_bytes), width, 1U,
              x + displacement, y + static_cast<std::int32_t>(source_y));
  }
}

void blit_rgba_transition_mode13(
    FrontEndFrame &target, const std::span<const std::uint8_t> source,
    const std::uint32_t width, const std::uint32_t height, const std::int32_t x,
    const std::int32_t y, const std::uint32_t transition_phase,
    const std::int32_t authored_scalar, const std::uint32_t noise_seed,
    const std::int32_t recovered_amplitude = -1) {
  // Modes 1 and 3 share the same p3.1 path. It chooses an amplitude below
  // scalar*phase/256, then RVA 0x19b4c applies a different centred horizontal
  // displacement to every scanline. This is the characteristic striped
  // "flying in" treatment visible in the accepted Sound entry frame.
  const auto maximum_amplitude =
      static_cast<std::int64_t>(authored_scalar) * transition_phase / 256;
  const auto amplitude = recovered_amplitude >= 0
                             ? recovered_amplitude
                             : static_cast<std::int32_t>(std::max<std::int64_t>(
                                   0, maximum_amplitude * 3 / 4));
  blit_rgba_scanline_noise(target, source, width, height, x, y, amplitude,
                           noise_seed ^ (transition_phase * 0x9e3779b9U));
}

const mh::content::SprFrame &
decode_required(const mh::content::SprArchive &archive,
                const std::string_view name,
                const std::uint32_t frame_index = 0U) {
  const auto *entry = mh::content::find_spr_entry(archive, name);
  if (entry == nullptr) {
    throw ToolError(ExitCode::format, "required front-end sprite is absent: " +
                                          std::string(name));
  }
  if (frame_index >= entry->frame_count) {
    throw ToolError(ExitCode::format,
                    "required front-end sprite frame is absent: " +
                        std::string(name));
  }
  return mh::content::decode_spr_frame(archive, *entry, frame_index);
}

mh::content::SprPositionSample
sample(const mh::content::SprPositionData &positions,
       const MainMenuPositionRecord record, const std::uint32_t phase) {
  return mh::content::sample_spr_position(positions, main_menu_position_bank,
                                          position_record_index(record), phase);
}

mh::content::SprPositionSample
sample(const mh::content::SprPositionData &positions, const std::size_t bank,
       const std::size_t record, const std::uint32_t phase) {
  return mh::content::sample_spr_position(positions, bank, record, phase);
}

mh::content::SprPositionSample
sample(const mh::content::SprPositionData &positions,
       const OptionsMenuPositionRecord record, const std::uint32_t phase) {
  return mh::content::sample_spr_position(positions, options_menu_position_bank,
                                          position_record_index(record), phase);
}

mh::content::SprPositionSample
sample(const mh::content::SprPositionData &positions,
       const OnePlayerPositionRecord record, const std::uint32_t phase) {
  return mh::content::sample_spr_position(positions, one_player_position_bank,
                                          position_record_index(record), phase);
}

// Options is the accepted visual reference for the common menu dial. The
// other dial banks use the same 258x217 sprite but author it at different
// origins, so move each complete dial/label cluster onto Options' origin.
constexpr std::int32_t main_dial_offset_x = -9;
constexpr std::int32_t main_dial_offset_y = 27;
constexpr std::int32_t one_player_dial_offset_x = -2;
constexpr std::int32_t one_player_dial_offset_y = 17;
constexpr std::int32_t one_player_pointer_offset_x = -3;
constexpr std::int32_t one_player_pointer_offset_y = 15;
constexpr std::int32_t league_dial_offset_x = -2;
constexpr std::int32_t league_dial_offset_y = 22;
constexpr std::int32_t league_pointer_offset_x = -3;
constexpr std::int32_t league_pointer_offset_y = 21;
constexpr std::int32_t main_screen_badge_offset_x = 4;
constexpr std::int32_t main_screen_badge_offset_y = -9;
constexpr std::int32_t menu_information_text_offset_y = 4;

void place_sprite(FrontEndFrame &target, const mh::content::SprArchive &archive,
                  const mh::content::SprPositionData &positions,
                  const MainMenuPositionRecord record,
                  const std::string_view name, const std::uint32_t phase,
                  const std::uint32_t frame_index = 0U,
                  const std::int32_t offset_x = 0,
                  const std::int32_t offset_y = 0) {
  const auto &frame = decode_required(archive, name, frame_index);
  const auto position = sample(positions, record, phase);
  blit_rgba(target, frame.rgba, frame.width, frame.height,
            position.x + offset_x, position.y + offset_y);
}

void place_sprite(FrontEndFrame &target, const mh::content::SprArchive &archive,
                  const mh::content::SprPositionData &positions,
                  const OnePlayerPositionRecord record,
                  const std::string_view name, const std::uint32_t phase,
                  const std::uint32_t frame_index = 0U) {
  const auto &frame = decode_required(archive, name, frame_index);
  const auto position = sample(positions, record, phase);
  blit_rgba(target, frame.rgba, frame.width, frame.height, position.x,
            position.y);
}

void place_sprite(FrontEndFrame &target, const mh::content::SprArchive &archive,
                  const mh::content::SprPositionData &positions,
                  const std::size_t bank, const std::size_t record,
                  const std::string_view name, const std::uint32_t phase,
                  const std::uint32_t frame_index = 0U) {
  const auto &frame = decode_required(archive, name, frame_index);
  const auto position = sample(positions, bank, record, phase);
  blit_rgba(target, frame.rgba, frame.width, frame.height, position.x,
            position.y);
}

void place_sprite_at(FrontEndFrame &target,
                     const mh::content::SprArchive &archive,
                     const std::string_view name, const std::int32_t x,
                     const std::int32_t y,
                     const std::uint32_t frame_index = 0U) {
  const auto &frame = decode_required(archive, name, frame_index);
  blit_rgba(target, frame.rgba, frame.width, frame.height, x, y);
}

void place_numa_digit(FrontEndFrame &target,
                      const mh::content::SprArchive &archive,
                      const std::uint32_t digit, const std::int32_t x,
                      const std::int32_t y) {
  constexpr std::uint32_t digit_height = 20U;
  const auto &sheet = decode_required(archive, "numa");
  if (digit > 9U || sheet.height < (digit + 1U) * digit_height) {
    throw ToolError(ExitCode::format,
                    "front-end numeric sprite sheet is malformed");
  }
  const auto row_bytes = static_cast<std::size_t>(sheet.width) * 4U;
  std::vector<std::uint8_t> glyph(row_bytes * digit_height);
  const auto source =
      static_cast<std::size_t>(digit) * digit_height * row_bytes;
  std::copy_n(sheet.rgba.begin() + static_cast<std::ptrdiff_t>(source),
              static_cast<std::ptrdiff_t>(glyph.size()), glyph.begin());
  blit_rgba(target, glyph, sheet.width, digit_height, x, y);
}

void place_numa_two_digits(FrontEndFrame &target,
                           const mh::content::SprArchive &archive,
                           const std::uint32_t value, const std::int32_t x,
                           const std::int32_t y) {
  place_numa_digit(target, archive, (value / 10U) % 10U, x, y);
  place_numa_digit(target, archive, value % 10U, x + 19, y);
}

void draw_fnt_text(FrontEndFrame &target, const mh::content::FntData &font,
                   const std::string_view text, const std::int32_t x,
                   const std::int32_t y,
                   const std::array<std::uint8_t, 3U> color,
                   const std::uint8_t opacity = 255U) {
  std::int64_t pen_x = 0;
  std::int64_t minimum_x = 0;
  std::int64_t maximum_x = 0;
  std::uint32_t maximum_row = 0U;
  bool has_store = false;
  for (const auto character : text) {
    const auto codepoint =
        static_cast<std::uint8_t>(static_cast<unsigned char>(character));
    if (codepoint == 0U) {
      break;
    }
    const auto &glyph = font.glyphs[codepoint];
    std::uint32_t row = 0U;
    for (const auto &operation : glyph.operations) {
      std::uint32_t store_width = 0U;
      switch (operation.kind) {
      case mh::content::FntOperationKind::advance_row:
        ++row;
        continue;
      case mh::content::FntOperationKind::advance_two_rows:
        row += 2U;
        continue;
      case mh::content::FntOperationKind::store1:
        store_width = 1U;
        break;
      case mh::content::FntOperationKind::store2:
        store_width = 2U;
        break;
      case mh::content::FntOperationKind::store4:
        store_width = 4U;
        break;
      case mh::content::FntOperationKind::store8:
        store_width = 8U;
        break;
      }
      const auto store_x =
          pen_x + static_cast<std::int64_t>(operation.displacement);
      minimum_x = std::min(minimum_x, store_x);
      maximum_x =
          std::max(maximum_x, store_x + static_cast<std::int64_t>(store_width));
      maximum_row = std::max(maximum_row, row + 1U);
      has_store = true;
    }
    pen_x += glyph.advance_width;
    maximum_x = std::max(maximum_x, pen_x);
  }
  if (!has_store || maximum_x <= minimum_x || maximum_row == 0U) {
    return;
  }
  const auto local_width64 = static_cast<std::uint64_t>(maximum_x - minimum_x);
  const auto local_bytes64 = local_width64 * maximum_row;
  if (local_width64 > std::numeric_limits<std::uint32_t>::max() ||
      local_bytes64 > std::numeric_limits<std::size_t>::max()) {
    throw ToolError(ExitCode::format, "FNT UI text surface is too large");
  }
  const auto local_width = static_cast<std::uint32_t>(local_width64);
  std::vector<std::uint8_t> mask(static_cast<std::size_t>(local_bytes64));
  mh::content::render_fnt_text_8bit(
      font, text, mask, local_width, maximum_row, local_width,
      static_cast<std::uint32_t>(-minimum_x), 0U, 255U);

  const auto destination_x = static_cast<std::int64_t>(x) + minimum_x;
  for (std::uint32_t row = 0U; row < maximum_row; ++row) {
    const auto target_y = static_cast<std::int64_t>(y) + row;
    if (target_y < 0 || target_y >= target.height) {
      continue;
    }
    for (std::uint32_t column = 0U; column < local_width; ++column) {
      if (mask[static_cast<std::size_t>(row) * local_width + column] == 0U) {
        continue;
      }
      const auto target_x = destination_x + column;
      if (target_x < 0 || target_x >= target.width) {
        continue;
      }
      const auto destination =
          (static_cast<std::size_t>(target_y) * target.width +
           static_cast<std::size_t>(target_x)) *
          4U;
      const auto blend = [opacity](const std::uint8_t foreground,
                                   const std::uint8_t background) {
        return static_cast<std::uint8_t>(
            (static_cast<std::uint32_t>(foreground) * opacity +
             static_cast<std::uint32_t>(background) * (255U - opacity) + 127U) /
            255U);
      };
      target.rgba[destination] = blend(color[0U], target.rgba[destination]);
      target.rgba[destination + 1U] =
          blend(color[1U], target.rgba[destination + 1U]);
      target.rgba[destination + 2U] =
          blend(color[2U], target.rgba[destination + 2U]);
      target.rgba[destination + 3U] = 255U;
    }
  }
}

void fill_rectangle(FrontEndFrame &target, const std::uint32_t x,
                    const std::uint32_t y, const std::uint32_t width,
                    const std::uint32_t height,
                    const std::array<std::uint8_t, 3U> color) {
  for (std::uint32_t row = y; row < std::min(target.height, y + height);
       ++row) {
    for (std::uint32_t column = x; column < std::min(target.width, x + width);
         ++column) {
      const auto pixel =
          (static_cast<std::size_t>(row) * target.width + column) * 4U;
      target.rgba[pixel] = color[0U];
      target.rgba[pixel + 1U] = color[1U];
      target.rgba[pixel + 2U] = color[2U];
      target.rgba[pixel + 3U] = 255U;
    }
  }
}

void fill_solid_triangle(FrontEndFrame &target,
                         const std::array<double, 2U> &first,
                         const std::array<double, 2U> &second,
                         const std::array<double, 2U> &third,
                         const std::array<std::uint8_t, 3U> color) {
  // p3.1 RVA 0xa6270 is a float32 scanline filler. It orders the vertices by
  // Y (and the first equal-Y pair by X), starts each half at trunc(Y)+1,
  // truncates both edge intersections toward zero through helper 0xc3a46,
  // and writes the complete inclusive span. Keep those coverage rules here;
  // clipping is the sole reconstruction safety addition because the original
  // writes directly into its trusted 640x400 indexed surface.
  struct RasterPoint {
    float x;
    float y;
  };
  std::array<RasterPoint, 3U> points{{
      {static_cast<float>(first[0U]), static_cast<float>(first[1U])},
      {static_cast<float>(second[0U]), static_cast<float>(second[1U])},
      {static_cast<float>(third[0U]), static_cast<float>(third[1U])},
  }};
  if (!std::all_of(points.begin(), points.end(), [](const RasterPoint &point) {
        return std::isfinite(point.x) && std::isfinite(point.y);
      })) {
    return;
  }
  if (points[2U].y < points[1U].y) {
    std::swap(points[1U], points[2U]);
  }
  if (points[1U].y < points[0U].y) {
    std::swap(points[0U], points[1U]);
  }
  if (points[2U].y < points[1U].y) {
    std::swap(points[1U], points[2U]);
  }
  if (points[0U].y == points[1U].y && points[0U].x > points[1U].x) {
    std::swap(points[0U], points[1U]);
  }
  const auto &top = points[0U];
  const auto &middle = points[1U];
  const auto &bottom = points[2U];
  const auto total_height = bottom.y - top.y;
  if (!std::isfinite(total_height) || total_height == 0.0F) {
    return;
  }
  const auto truncate = [](const float value) {
    return static_cast<std::int32_t>(std::trunc(value));
  };
  const auto top_row = truncate(top.y) + 1;
  const auto middle_row = truncate(middle.y) + 1;
  const auto bottom_row = truncate(bottom.y) + 1;
  const auto upper_height = middle.y - top.y;
  const auto lower_height = bottom.y - middle.y;
  const auto long_step = (bottom.x - top.x) / total_height;
  const auto upper_step =
      upper_height != 0.0F ? (middle.x - top.x) / upper_height : 0.0F;
  const auto lower_step =
      lower_height != 0.0F ? (bottom.x - middle.x) / lower_height : 0.0F;
  const auto top_offset = top.y - static_cast<float>(top_row);
  const auto middle_offset = middle.y - static_cast<float>(middle_row);
  auto long_x = top.x - top_offset * long_step;
  auto upper_x = top.x - top_offset * upper_step;
  auto lower_x = middle.x - middle_offset * lower_step;
  const auto long_is_right = top.x + (middle.y - top.y) * long_step > middle.x;

  const auto draw_span = [&](const std::int32_t y, const float left,
                             const float right) {
    if (y < 0 || y >= static_cast<std::int32_t>(target.height)) {
      return;
    }
    const auto first_x = std::max<std::int32_t>(0, truncate(left));
    const auto last_x = std::min<std::int32_t>(
        static_cast<std::int32_t>(target.width) - 1, truncate(right));
    for (auto x = first_x; x <= last_x; ++x) {
      const auto destination = (static_cast<std::size_t>(y) * target.width +
                                static_cast<std::size_t>(x)) *
                               4U;
      target.rgba[destination] = color[0U];
      target.rgba[destination + 1U] = color[1U];
      target.rgba[destination + 2U] = color[2U];
      target.rgba[destination + 3U] = 255U;
    }
  };
  for (auto y = top_row; y < middle_row; ++y) {
    draw_span(y, long_is_right ? upper_x : long_x,
              long_is_right ? long_x : upper_x);
    upper_x += upper_step;
    long_x += long_step;
  }
  for (auto y = middle_row; y < bottom_row; ++y) {
    draw_span(y, long_is_right ? lower_x : long_x,
              long_is_right ? long_x : lower_x);
    lower_x += lower_step;
    long_x += long_step;
  }
}

void mask_car_performance_gauge(FrontEndFrame &target,
                                const mh::content::SprPositionData &positions,
                                const CarSetupPositionRecord record,
                                const std::int32_t offset_x,
                                const std::int32_t offset_y,
                                const double radius, const double argument,
                                const std::uint32_t transition_phase) {
  constexpr double angle_scale = 3.1415926535897932384626433832795 / 2048.0;
  constexpr double vertical_scale = 0.8333333;
  constexpr double full_sweep_units = 3740.0;
  constexpr double segment_step_units = 467.5;
  constexpr double segment_threshold_units = 468.0;
  // carx/cary/carz use this opaque lavender as their authored empty-sector
  // fill. Palette zero is black, while restoring the page makes it transparent.
  constexpr std::array<std::uint8_t, 3U> empty_sector_color{188U, 188U, 250U};
  const auto position = sample(positions, car_setup_position_bank,
                               position_record_index(record), transition_phase);
  const std::array<double, 2U> center{
      static_cast<double>(position.x + offset_x),
      static_cast<double>(position.y + offset_y)};
  auto remaining = (1.0 - std::clamp(argument, 0.0, 1.0)) * full_sweep_units;
  for (std::uint32_t index = 0U; index < 8U; ++index) {
    if (remaining >= segment_threshold_units) {
      const auto first_units = ((7U - index) * 468U + 3250U) & 0xfffU;
      const auto second_units = (first_units + 4565U) & 0xfffU;
      const auto point = [&](const std::uint32_t units) {
        const auto angle = static_cast<double>(units) * angle_scale;
        return std::array<double, 2U>{center[0U] + std::cos(angle) * radius,
                                      center[1U] + std::sin(angle) * radius *
                                                       vertical_scale};
      };
      fill_solid_triangle(target, center, point(first_units),
                          point(second_units), empty_sector_color);
    }
    remaining -= segment_step_units;
  }
}

void blend_front_end_pixel(FrontEndFrame &target, const std::int32_t x,
                           const std::int32_t y,
                           const std::array<std::uint8_t, 3U> color,
                           const std::uint8_t alpha) {
  if (x < 0 || y < 0 || x >= static_cast<std::int32_t>(target.width) ||
      y >= static_cast<std::int32_t>(target.height) || alpha == 0U) {
    return;
  }
  const auto pixel = (static_cast<std::size_t>(y) * target.width +
                      static_cast<std::size_t>(x)) *
                     4U;
  const auto inverse = 255U - static_cast<std::uint32_t>(alpha);
  for (std::size_t channel = 0U; channel < color.size(); ++channel) {
    target.rgba[pixel + channel] = static_cast<std::uint8_t>(
        (static_cast<std::uint32_t>(target.rgba[pixel + channel]) * inverse +
         static_cast<std::uint32_t>(color[channel]) * alpha + 127U) /
        255U);
  }
  target.rgba[pixel + 3U] = 255U;
}

void draw_front_end_rgba_line(FrontEndFrame &target, double x0, double y0,
                              double x1, double y1,
                              const std::array<std::uint8_t, 3U> color,
                              const std::uint8_t alpha) {
  if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) ||
      !std::isfinite(y1)) {
    return;
  }
  const auto dx = x1 - x0;
  const auto dy = y1 - y0;
  const auto steps = static_cast<std::int32_t>(
      std::ceil(std::max(std::abs(dx), std::abs(dy))));
  if (steps <= 0) {
    blend_front_end_pixel(target, static_cast<std::int32_t>(std::lround(x0)),
                          static_cast<std::int32_t>(std::lround(y0)), color,
                          alpha);
    return;
  }
  for (std::int32_t step = 0; step <= steps; ++step) {
    const auto amount = static_cast<double>(step) / steps;
    blend_front_end_pixel(
        target, static_cast<std::int32_t>(std::lround(x0 + dx * amount)),
        static_cast<std::int32_t>(std::lround(y0 + dy * amount)), color, alpha);
  }
}

void draw_track_setup_line_object(FrontEndFrame &target,
                                  const mh::content::LobData &line_object,
                                  const double model_scale,
                                  const mh::content::SprPositionData &positions,
                                  const std::uint32_t elapsed_ms,
                                  const std::uint32_t transition_phase) {
  if (!line_object.summary.edge_indices_in_bounds ||
      line_object.vertices.empty() || line_object.edges.empty()) {
    return;
  }
  // p3.1's selector renderer gets the object origin from bank 2 record 30;
  // it is independent of the static preview-frame position in record 4.
  const auto anchor =
      sample(positions, race_setup_position_bank,
             position_record_index(RaceSetupPositionRecord::line_object),
             transition_phase);
  const auto center_x = static_cast<double>(anchor.x);
  const auto center_y = static_cast<double>(anchor.y);
  const auto model_center_x =
      (line_object.summary.minimum[0U] + line_object.summary.maximum[0U]) * 0.5;
  const auto model_center_y =
      (line_object.summary.minimum[1U] + line_object.summary.maximum[1U]) * 0.5;
  const auto model_center_z =
      (line_object.summary.minimum[2U] + line_object.summary.maximum[2U]) * 0.5;
  if (!(model_scale > 0.0) || !std::isfinite(model_scale)) {
    return;
  }

  constexpr double tau = 6.283185307179586476925286766559;
  constexpr double angle_scale = tau / 4096.0;
  const auto base_angle = static_cast<double>(elapsed_ms) / 16.0;
  const auto rotation_z = std::nearbyint(base_angle * 13.0);
  const auto rotation_y = std::nearbyint(base_angle * 13.0 + 1024.0);
  const auto rotation_x = std::nearbyint(
      std::sin(std::fmod(rotation_y, 4096.0) * angle_scale) * 250.0);
  const auto sx = std::sin(rotation_x * angle_scale);
  const auto cx = std::cos(rotation_x * angle_scale);
  const auto sy = std::sin(rotation_y * angle_scale);
  const auto cy = std::cos(rotation_y * angle_scale);
  const auto sz = std::sin(rotation_z * angle_scale);
  const auto cz = std::cos(rotation_z * angle_scale);
  const std::array<std::array<double, 3U>, 3U> matrix{{
      {{(cz * cy + sz * sx * sy) * 288.0, (cz * sx * sy - sz * cy) * 288.0,
        cx * sy * 288.0}},
      {{sz * cx * 240.0, cz * cx * 240.0, -sx * 240.0}},
      {{sz * sx * cy - cz * sy, sz * sy - cz * sx * cy, cx * cy}},
  }};
  constexpr double translation_y = 105.0 * 240.0;
  constexpr double translation_z = 475.0 + 6.0;
  std::vector<std::array<double, 2U>> projected;
  projected.reserve(line_object.vertices.size());
  for (const auto &vertex : line_object.vertices) {
    const std::array<double, 3U> point{
        (static_cast<double>(vertex.position[0U]) - model_center_x) *
            model_scale,
        (static_cast<double>(vertex.position[1U]) - model_center_y) *
            model_scale,
        (static_cast<double>(vertex.position[2U]) - model_center_z) *
            model_scale};
    const auto transformed_x = point[0U] * matrix[0U][0U] +
                               point[1U] * matrix[0U][1U] +
                               point[2U] * matrix[0U][2U];
    const auto transformed_y = point[0U] * matrix[1U][0U] +
                               point[1U] * matrix[1U][1U] +
                               point[2U] * matrix[1U][2U] + translation_y;
    const auto transformed_z = point[0U] * matrix[2U][0U] +
                               point[1U] * matrix[2U][1U] +
                               point[2U] * matrix[2U][2U] + translation_z;
    projected.push_back({center_x + transformed_x / transformed_z,
                         center_y + transformed_y / transformed_z});
  }
  for (const auto &edge : line_object.edges) {
    const auto &first = projected[edge.first];
    const auto &second = projected[edge.second];
    draw_front_end_rgba_line(target, first[0U], first[1U], second[0U],
                             second[1U], {225U, 226U, 255U}, 178U);
  }
}

void draw_car_setup_line_object(FrontEndFrame &target,
                                const mh::content::LobData &line_object,
                                const double model_scale,
                                const mh::content::SprPositionData &positions,
                                const std::uint32_t elapsed_ms,
                                const std::uint32_t transition_phase) {
  if (!line_object.summary.edge_indices_in_bounds ||
      line_object.vertices.empty() || line_object.edges.empty()) {
    return;
  }
  const auto anchor =
      sample(positions, car_setup_position_bank,
             position_record_index(CarSetupPositionRecord::line_object),
             transition_phase);
  const auto center_x = static_cast<double>(anchor.x);
  const auto center_y = static_cast<double>(anchor.y);
  const auto model_center_x =
      (line_object.summary.minimum[0U] + line_object.summary.maximum[0U]) * 0.5;
  const auto model_center_y =
      (line_object.summary.minimum[1U] + line_object.summary.maximum[1U]) * 0.5;
  const auto model_center_z =
      (line_object.summary.minimum[2U] + line_object.summary.maximum[2U]) * 0.5;
  if (!(model_scale > 0.0) || !std::isfinite(model_scale)) {
    return;
  }
  constexpr double tau = 6.283185307179586476925286766559;
  constexpr double angle_scale = tau / 4096.0;
  const auto base_angle = static_cast<double>(elapsed_ms) / 16.0;
  const auto rotation_z = std::nearbyint(base_angle * 13.0);
  const auto rotation_y = std::nearbyint(base_angle * 13.0 + 1024.0);
  const auto rotation_x = std::nearbyint(
      std::sin(std::fmod(rotation_y, 4096.0) * angle_scale) * 250.0);
  const auto sx = std::sin(rotation_x * angle_scale);
  const auto cx = std::cos(rotation_x * angle_scale);
  const auto sy = std::sin(rotation_y * angle_scale);
  const auto cy = std::cos(rotation_y * angle_scale);
  const auto sz = std::sin(rotation_z * angle_scale);
  const auto cz = std::cos(rotation_z * angle_scale);
  const std::array<std::array<double, 3U>, 3U> matrix{{
      {{(cz * cy + sz * sx * sy) * 288.0, (cz * sx * sy - sz * cy) * 288.0,
        cx * sy * 288.0}},
      {{sz * cx * 240.0, cz * cx * 240.0, -sx * 240.0}},
      {{sz * sx * cy - cz * sy, sz * sy - cz * sx * cy, cx * cy}},
  }};
  constexpr double translation_y = 105.0 * 240.0;
  constexpr double translation_z = 400.0 + 6.0;
  std::vector<std::array<double, 2U>> projected;
  projected.reserve(line_object.vertices.size());
  for (const auto &vertex : line_object.vertices) {
    const auto x = (static_cast<double>(vertex.position[0U]) - model_center_x) *
                   model_scale;
    const auto y = (static_cast<double>(vertex.position[1U]) - model_center_y) *
                   model_scale;
    const auto z = (static_cast<double>(vertex.position[2U]) - model_center_z) *
                   model_scale;
    const auto transformed_x =
        x * matrix[0U][0U] + y * matrix[0U][1U] + z * matrix[0U][2U];
    const auto transformed_y = x * matrix[1U][0U] + y * matrix[1U][1U] +
                               z * matrix[1U][2U] + translation_y;
    const auto transformed_z = x * matrix[2U][0U] + y * matrix[2U][1U] +
                               z * matrix[2U][2U] + translation_z;
    projected.push_back({center_x + transformed_x / transformed_z,
                         center_y + transformed_y / transformed_z});
  }
  for (const auto &edge : line_object.edges) {
    const auto &first = projected[edge.first];
    const auto &second = projected[edge.second];
    draw_front_end_rgba_line(target, first[0U], first[1U], second[0U],
                             second[1U], {190U, 194U, 255U}, 72U);
  }
}

void place_transition_mode8_sprite(
    FrontEndFrame &target, const mh::content::SprArchive &archive,
    const mh::content::SprPositionData &positions,
    const MainMenuPositionRecord record, const std::string_view name,
    const std::uint32_t phase) {
  const auto &frame = decode_required(archive, name);
  const auto position = sample(positions, record, phase);
  blit_rgba_transition_mode8(target, frame.rgba, frame.width, frame.height,
                             position.x + main_dial_offset_x,
                             position.y + main_dial_offset_y, phase,
                             position.direct[3U]);
}

void place_selector_noise_sprite(FrontEndFrame &target,
                                 const mh::content::SprArchive &archive,
                                 const mh::content::SprPositionData &positions,
                                 const MainMenuPositionRecord record,
                                 const std::string_view name,
                                 const std::uint32_t amplitude) {
  const auto &frame = decode_required(archive, name);
  const auto position = sample(positions, record, 0U);
  blit_rgba_scanline_noise(
      target, frame.rgba, frame.width, frame.height,
      position.x + main_dial_offset_x, position.y + main_dial_offset_y,
      static_cast<std::int32_t>(amplitude),
      static_cast<std::uint32_t>(position_record_index(record)));
}

void place_selector_noise_sprite(FrontEndFrame &target,
                                 const mh::content::SprArchive &archive,
                                 const mh::content::SprPositionData &positions,
                                 const OnePlayerPositionRecord record,
                                 const std::string_view name,
                                 const std::uint32_t amplitude) {
  const auto &frame = decode_required(archive, name);
  const auto position = sample(positions, record, 0U);
  blit_rgba_scanline_noise(
      target, frame.rgba, frame.width, frame.height,
      position.x + one_player_dial_offset_x,
      position.y + one_player_dial_offset_y,
      static_cast<std::int32_t>(amplitude),
      static_cast<std::uint32_t>(position_record_index(record)));
}

void place_transition_frame(FrontEndFrame &target,
                            const mh::content::SprFrame &frame,
                            const mh::content::SprPositionSample &position,
                            const std::size_t bank, const std::size_t record,
                            const std::uint32_t phase,
                            const std::int32_t recovered_mode13_amplitude = -1,
                            const std::int32_t offset_x = 0,
                            const std::int32_t offset_y = 0) {
  const auto x = position.x + offset_x;
  const auto y = position.y + offset_y;
  if (phase == 0U) {
    blit_rgba(target, frame.rgba, frame.width, frame.height, x, y);
    return;
  }

  const auto mode = position.direct[1U];
  const auto scalar = position.direct[3U];
  if (mode == 1 || mode == 3) {
    blit_rgba_transition_mode13(target, frame.rgba, frame.width, frame.height,
                                x, y, phase, scalar,
                                static_cast<std::uint32_t>(bank * 32U + record),
                                recovered_mode13_amplitude);
  } else if (mode == 5) {
    // The retail dispatcher feeds 1024 + phase*12 through its sine table
    // before using the centred scanline mapper at RVA 0x19884.
    blit_rgba_glitched(target, frame.rgba, frame.width, frame.height, x, y,
                       1024U + phase * 12U);
  } else if (mode == 6) {
    blit_rgba_transition_mode6(target, frame.rgba, frame.width, frame.height, x,
                               y, phase, scalar);
  } else if (mode == 7) {
    blit_rgba_transition_mode7(target, frame.rgba, frame.width, frame.height, x,
                               y, phase, scalar);
  } else if (mode == 8) {
    blit_rgba_transition_mode8(target, frame.rgba, frame.width, frame.height, x,
                               y, phase, scalar);
  } else {
    blit_rgba(target, frame.rgba, frame.width, frame.height, x, y);
  }
}

void place_transition_sprite(
    FrontEndFrame &target, const mh::content::SprArchive &archive,
    const mh::content::SprPositionData &positions, const std::size_t bank,
    const std::size_t record, const std::string_view name,
    const std::uint32_t phase, const std::uint32_t frame_index = 0U,
    const std::int32_t recovered_mode13_amplitude = -1,
    const std::int32_t offset_x = 0, const std::int32_t offset_y = 0) {
  const auto &frame = decode_required(archive, name, frame_index);
  const auto position = sample(positions, bank, record, phase);
  place_transition_frame(target, frame, position, bank, record, phase,
                         recovered_mode13_amplitude, offset_x, offset_y);
}

mh::content::SprFrame
extend_sprite_lower_section(mh::content::SprFrame frame,
                            const std::uint32_t fixed_rows,
                            const std::uint32_t extra_rows) {
  if (fixed_rows >= frame.height || extra_rows == 0U) {
    return frame;
  }
  const auto source_height = frame.height;
  const auto destination_height = source_height + extra_rows;
  const auto row_bytes = static_cast<std::size_t>(frame.width) * 4U;
  std::vector<std::uint8_t> extended(
      static_cast<std::size_t>(destination_height) * row_bytes);
  std::copy_n(frame.rgba.begin(),
              static_cast<std::ptrdiff_t>(fixed_rows * row_bytes),
              extended.begin());
  const auto source_tail_rows = source_height - fixed_rows;
  const auto destination_tail_rows = destination_height - fixed_rows;
  for (std::uint32_t destination_y = fixed_rows;
       destination_y < destination_height; ++destination_y) {
    const auto source_y = std::min<std::uint32_t>(
        source_height - 1U,
        fixed_rows +
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(destination_y - fixed_rows) *
                 source_tail_rows) /
                destination_tail_rows));
    std::copy_n(frame.rgba.begin() +
                    static_cast<std::ptrdiff_t>(source_y * row_bytes),
                static_cast<std::ptrdiff_t>(row_bytes),
                extended.begin() +
                    static_cast<std::ptrdiff_t>(destination_y * row_bytes));
  }
  frame.height = destination_height;
  frame.rgba = std::move(extended);
  return frame;
}

void place_one_player_information_sprite(
    FrontEndFrame &target, const mh::content::SprArchive &archive,
    const mh::content::SprPositionData &positions,
    const OnePlayerPositionRecord record, const std::string_view name,
    const std::uint32_t fixed_rows, const std::uint32_t extra_rows,
    const std::uint32_t phase, const std::int32_t offset_x = 0,
    const std::int32_t offset_y = 0) {
  const auto frame = extend_sprite_lower_section(decode_required(archive, name),
                                                 fixed_rows, extra_rows);
  const auto position = sample(positions, record, phase);
  place_transition_frame(target, frame, position, one_player_position_bank,
                         position_record_index(record), phase, -1, offset_x,
                         offset_y);
}

void place_sound_sprite(FrontEndFrame &target,
                        const mh::content::SprArchive &archive,
                        const mh::content::SprPositionData &positions,
                        const std::size_t bank, const std::size_t record,
                        const std::string_view name, const std::uint32_t phase,
                        const SoundOptionsGlitchFrame &glitch,
                        const std::uint32_t frame_index = 0U) {
  const auto recovered_amplitude =
      record < glitch.transition_noise_active.size() &&
              glitch.transition_noise_active[record]
          ? static_cast<std::int32_t>(glitch.transition_noise_amplitude[record])
          : -1;
  place_transition_sprite(target, archive, positions, bank, record, name, phase,
                          frame_index, recovered_amplitude);
}

void place_transition_mode8_sprite(
    FrontEndFrame &target, const mh::content::SprArchive &archive,
    const mh::content::SprPositionData &positions,
    const OnePlayerPositionRecord record, const std::string_view name,
    const std::uint32_t phase) {
  const auto &frame = decode_required(archive, name);
  const auto position = sample(positions, record, phase);
  blit_rgba_transition_mode8(target, frame.rgba, frame.width, frame.height,
                             position.x + one_player_dial_offset_x,
                             position.y + one_player_dial_offset_y, phase,
                             position.direct[3U]);
}

void place_sprite(FrontEndFrame &target, const mh::content::SprArchive &archive,
                  const mh::content::SprPositionData &positions,
                  const OptionsMenuPositionRecord record,
                  const std::string_view name, const std::uint32_t phase,
                  const std::uint32_t frame_index = 0U) {
  const auto &frame = decode_required(archive, name, frame_index);
  const auto position = sample(positions, record, phase);
  blit_rgba(target, frame.rgba, frame.width, frame.height, position.x,
            position.y);
}

void place_glitched_sprite(
    FrontEndFrame &target, const mh::content::SprArchive &archive,
    const mh::content::SprPositionData &positions,
    const MainMenuPositionRecord record, const std::string_view name,
    const std::uint32_t transition_phase, const std::uint32_t glitch_phase,
    const std::int32_t offset_x = 0, const std::int32_t offset_y = 0) {
  const auto &frame = decode_required(archive, name);
  const auto position = sample(positions, record, transition_phase);
  blit_rgba_glitched(target, frame.rgba, frame.width, frame.height,
                     position.x + offset_x, position.y + offset_y,
                     glitch_phase);
}

void place_glitched_sprite(FrontEndFrame &target,
                           const mh::content::SprArchive &archive,
                           const mh::content::SprPositionData &positions,
                           const OnePlayerPositionRecord record,
                           const std::string_view name,
                           const std::uint32_t transition_phase,
                           const std::uint32_t glitch_phase) {
  const auto &frame = decode_required(archive, name);
  const auto position = sample(positions, record, transition_phase);
  blit_rgba_glitched(target, frame.rgba, frame.width, frame.height, position.x,
                     position.y, glitch_phase);
}

void place_glitched_sprite(FrontEndFrame &target,
                           const mh::content::SprArchive &archive,
                           const mh::content::SprPositionData &positions,
                           const OptionsMenuPositionRecord record,
                           const std::string_view name,
                           const std::uint32_t transition_phase,
                           const std::uint32_t glitch_phase) {
  const auto &frame = decode_required(archive, name);
  const auto position = sample(positions, record, transition_phase);
  blit_rgba_glitched(target, frame.rgba, frame.width, frame.height, position.x,
                     position.y, glitch_phase);
}

struct Point {
  std::int32_t x;
  std::int32_t y;
};

struct FloatPoint {
  double x;
  double y;
};

constexpr std::array<std::uint8_t, 3U> pointer_color{255U, 100U, 0U};
constexpr std::int32_t race_setup_screen_badge_offset_y = 27;
constexpr std::int32_t car_setup_screen_badge_offset_y = 25;

// Recovered from the five settled p3.1 selector positions after mapping the
// captured 640x480 presentation back to the game's 640x400 logical surface.
constexpr std::array<std::array<Point, 4U>, 5U> settled_pointer_polygons{{
    {{{260, 263}, {314, 204}, {345, 186}, {327, 211}}},
    {{{235, 181}, {317, 192}, {350, 205}, {308, 202}}},
    {{{302, 118}, {333, 188}, {335, 220}, {317, 190}}},
    {{{398, 141}, {345, 195}, {312, 213}, {331, 189}}},
    {{{422, 223}, {342, 210}, {306, 194}, {347, 198}}},
}};

void fill_pointer_polygon(FrontEndFrame &target, std::array<Point, 4U> polygon,
                          const std::int32_t offset_x = 0,
                          const std::int32_t offset_y = 0) {
  for (auto &point : polygon) {
    point.x += offset_x;
    point.y += offset_y;
  }
  auto minimum_y = polygon.front().y;
  auto maximum_y = polygon.front().y;
  for (const auto point : polygon) {
    minimum_y = std::min(minimum_y, point.y);
    maximum_y = std::max(maximum_y, point.y);
  }
  for (auto y = minimum_y; y <= maximum_y; ++y) {
    std::array<double, 4U> intersections{};
    std::size_t count = 0U;
    const auto scan_y = static_cast<double>(y) + 0.5;
    for (std::size_t index = 0U; index < polygon.size(); ++index) {
      const auto a = polygon[index];
      const auto b = polygon[(index + 1U) % polygon.size()];
      const auto low_y = std::min(a.y, b.y);
      const auto high_y = std::max(a.y, b.y);
      if (a.y == b.y || scan_y < low_y || scan_y >= high_y) {
        continue;
      }
      const auto fraction = (scan_y - a.y) / static_cast<double>(b.y - a.y);
      intersections[count++] =
          static_cast<double>(a.x) + fraction * (b.x - a.x);
    }
    if (count != 2U || y < 0 || y >= static_cast<std::int32_t>(target.height)) {
      continue;
    }
    std::sort(intersections.begin(),
              intersections.begin() + static_cast<std::ptrdiff_t>(count));
    const auto first_x =
        static_cast<std::int32_t>(std::ceil(intersections[0U] - 0.5));
    const auto last_x =
        static_cast<std::int32_t>(std::floor(intersections[1U] - 0.5));
    for (auto x = first_x; x <= last_x; ++x) {
      if (x < 0 || x >= static_cast<std::int32_t>(target.width)) {
        continue;
      }
      const auto pixel = (static_cast<std::size_t>(y) * target.width +
                          static_cast<std::size_t>(x)) *
                         4U;
      for (std::size_t channel = 0U; channel < pointer_color.size();
           ++channel) {
        target.rgba[pixel + channel] = pointer_color[channel];
      }
      target.rgba[pixel + 3U] = 255U;
    }
  }
}

void fill_dynamic_pointer(FrontEndFrame &target, const float pointer_units,
                          const double center_x = 329.0,
                          const double center_y = 200.0) {
  constexpr double radians_per_unit = 3.141592654 / 2048.0;
  constexpr double shape_scale = 2.6;
  constexpr double aspect_scale = 5.0 / 6.0;
  constexpr std::array<FloatPoint, 4U> base{{
      {0.0, -40.0},
      {-3.0, -5.0},
      {0.0, 10.0},
      {3.0, -5.0},
  }};
  const auto angle = static_cast<double>(pointer_units) * radians_per_unit;
  const auto sine = std::sin(angle) * shape_scale;
  const auto cosine = std::cos(angle) * shape_scale;
  std::array<FloatPoint, 4U> transformed{};
  for (std::size_t index = 0U; index < base.size(); ++index) {
    const auto point = base[index];
    transformed[index] = {
        center_x + sine * point.x + cosine * point.y,
        center_y + aspect_scale * (cosine * point.x - sine * point.y),
    };
  }
  fill_solid_triangle(target, {transformed[1U].x, transformed[1U].y},
                      {transformed[0U].x, transformed[0U].y},
                      {transformed[3U].x, transformed[3U].y}, pointer_color);
  fill_solid_triangle(target, {transformed[1U].x, transformed[1U].y},
                      {transformed[3U].x, transformed[3U].y},
                      {transformed[2U].x, transformed[2U].y}, pointer_color);
}

void place_main_menu_labels(FrontEndFrame &target,
                            const mh::content::SprArchive &menu_sprites,
                            const mh::content::SprPositionData &positions,
                            const MainMenuChoice selection,
                            const std::uint32_t selected_label_frame,
                            const std::uint32_t transition_phase = 0U) {
  constexpr std::array labels{
      std::pair{MainMenuPositionRecord::one_player, std::string_view("tone")},
      std::pair{MainMenuPositionRecord::multi_player, std::string_view("tmul")},
      std::pair{MainMenuPositionRecord::rankings, std::string_view("tran")},
      std::pair{MainMenuPositionRecord::options, std::string_view("topt")},
      std::pair{MainMenuPositionRecord::credits, std::string_view("tcre")},
  };
  for (std::size_t label = 0U; label < labels.size(); ++label) {
    const auto &[record, sprite] = labels[label];
    const auto label_choice = static_cast<MainMenuChoice>(label);
    place_transition_sprite(
        target, menu_sprites, positions, main_menu_position_bank,
        position_record_index(record), sprite, transition_phase,
        selection == label_choice ? selected_label_frame
                                  : main_menu_unselected_label_frame,
        -1, main_dial_offset_x, main_dial_offset_y);
  }
}

void compose_main_menu_base_with_glitch(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const std::uint32_t transition_phase, const MainMenuGlitchFrame &glitch,
    FrontEndFrame &result) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "main-menu background must be exactly 640x400 RGBA");
  }
  if (!menu_sprites.has_palette) {
    throw ToolError(ExitCode::format,
                    "main-menu sprites require their embedded palette");
  }

  result.rgba = background.rgba;
  place_transition_sprite(
      result, menu_sprites, positions, main_menu_position_bank,
      position_record_index(MainMenuPositionRecord::background_dots), "dots",
      transition_phase);
  if (transition_phase != 0U) {
    place_transition_sprite(
        result, menu_sprites, positions, main_menu_position_bank,
        position_record_index(MainMenuPositionRecord::screen_badge), "scr0",
        transition_phase, 0U, -1, main_screen_badge_offset_x,
        main_screen_badge_offset_y);
  } else if (glitch.screen_badge_active) {
    place_glitched_sprite(
        result, menu_sprites, positions, MainMenuPositionRecord::screen_badge,
        "scr0", transition_phase, glitch.screen_badge_phase,
        main_screen_badge_offset_x, main_screen_badge_offset_y);
  } else {
    place_sprite(result, menu_sprites, positions,
                 MainMenuPositionRecord::screen_badge, "scr0", transition_phase,
                 0U, main_screen_badge_offset_x, main_screen_badge_offset_y);
  }
  if (transition_phase == 0U && glitch.selector_noise_active) {
    place_selector_noise_sprite(result, menu_sprites, positions,
                                MainMenuPositionRecord::selector, "msel",
                                glitch.selector_noise_amplitude);
  } else {
    place_transition_mode8_sprite(result, menu_sprites, positions,
                                  MainMenuPositionRecord::selector, "msel",
                                  transition_phase);
  }
  if (transition_phase != 0U) {
    place_transition_sprite(
        result, menu_sprites, positions, main_menu_position_bank,
        position_record_index(MainMenuPositionRecord::header), "main",
        transition_phase);
  } else if (glitch.header_active) {
    place_glitched_sprite(result, menu_sprites, positions,
                          MainMenuPositionRecord::header, "main",
                          transition_phase, glitch.header_phase);
  } else {
    place_sprite(result, menu_sprites, positions,
                 MainMenuPositionRecord::header, "main", transition_phase);
  }
}

void place_options_menu_labels(FrontEndFrame &target,
                               const mh::content::SprArchive &menu_sprites,
                               const mh::content::SprArchive &menu_dial_sprites,
                               const mh::content::SprPositionData &positions,
                               const OptionsMenuChoice selection,
                               const std::uint32_t selected_label_frame,
                               const std::uint32_t transition_phase = 0U) {
  struct Label {
    OptionsMenuPositionRecord record;
    std::string_view sprite;
  };
  constexpr std::array labels{
      Label{OptionsMenuPositionRecord::personal_options, "tgro"},
      Label{OptionsMenuPositionRecord::graphic_options, "tso"},
      Label{OptionsMenuPositionRecord::control_options, ""},
      Label{OptionsMenuPositionRecord::sound_options, "tcon"},
      Label{OptionsMenuPositionRecord::difficulty, "tpers"},
      Label{OptionsMenuPositionRecord::difficulty, "tdif"},
  };
  constexpr std::array<Point, 6U> settled_positions{{
      {127, 306}, {108, 204}, {192, 72},
      {365, 72},  {460, 204}, {421, 306},
  }};
  constexpr std::array<Point, 6U> transition_sources{{
      {127, 306}, {108, 168}, {236, 72},
      {421, 114}, {446, 254}, {446, 254},
  }};
  for (std::size_t label = 0U; label < labels.size(); ++label) {
    const auto &[record, sprite] = labels[label];
    const auto label_choice = static_cast<OptionsMenuChoice>(label);
    const auto authored = sample(positions, record, transition_phase);
    const auto settled = settled_positions[label];
    const auto source = transition_sources[label];
    const auto x = transition_phase == 0U
                       ? settled.x
                       : authored.x + settled.x - source.x;
    const auto y = transition_phase == 0U
                       ? settled.y
                       : authored.y + settled.y - source.y;
    if (!sprite.empty()) {
      const auto &frame = decode_required(
          menu_sprites, sprite,
          selection == label_choice ? selected_label_frame
                                    : main_menu_unselected_label_frame);
      blit_rgba(target, frame.rgba, frame.width, frame.height, x, y);
    } else {
      const auto &frame = decode_required(
          menu_dial_sprites, "gameplay",
          selection == label_choice ? selected_label_frame
                                    : main_menu_unselected_label_frame);
      blit_rgba(target, frame.rgba, frame.width, frame.height, x, y);
    }
  }
}

void compose_options_menu_base_with_glitch(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprArchive &menu_dial_sprites,
    const mh::content::SprPositionData &positions,
    const std::uint32_t transition_phase, const MainMenuGlitchFrame &glitch,
    FrontEndFrame &result) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "options background must be exactly 640x400 RGBA");
  }
  result.rgba = background.rgba;
  place_transition_sprite(
      result, menu_sprites, positions, options_menu_position_bank,
      position_record_index(OptionsMenuPositionRecord::background_dots), "dots",
      transition_phase);
  if (transition_phase != 0U) {
    place_transition_sprite(
        result, menu_sprites, positions, options_menu_position_bank,
        position_record_index(OptionsMenuPositionRecord::header), "opt",
        transition_phase);
  } else if (glitch.header_active) {
    place_glitched_sprite(result, menu_sprites, positions,
                          OptionsMenuPositionRecord::header, "opt",
                          transition_phase, glitch.header_phase);
  } else {
    place_sprite(result, menu_sprites, positions,
                 OptionsMenuPositionRecord::header, "opt", transition_phase);
  }
  if (transition_phase != 0U) {
    place_transition_sprite(
        result, menu_sprites, positions, options_menu_position_bank,
        position_record_index(OptionsMenuPositionRecord::screen_badge), "scr1",
        transition_phase);
  } else if (glitch.screen_badge_active) {
    place_glitched_sprite(result, menu_sprites, positions,
                          OptionsMenuPositionRecord::screen_badge, "scr1",
                          transition_phase, glitch.screen_badge_phase);
  } else {
    place_sprite(result, menu_sprites, positions,
                 OptionsMenuPositionRecord::screen_badge, "scr1",
                 transition_phase);
  }
  if (transition_phase == 0U && glitch.selector_noise_active) {
    const auto &frame = decode_required(menu_dial_sprites, "optdial");
    const auto position = sample(positions, OptionsMenuPositionRecord::selector,
                                 transition_phase);
    blit_rgba_scanline_noise(
        result, frame.rgba, frame.width, frame.height, position.x, position.y,
        static_cast<std::int32_t>(glitch.selector_noise_amplitude),
        static_cast<std::uint32_t>(position_record_index(
            OptionsMenuPositionRecord::selector)));
  } else {
    const auto &frame = decode_required(menu_dial_sprites, "optdial");
    const auto position = sample(positions, OptionsMenuPositionRecord::selector,
                                 transition_phase);
    blit_rgba_transition_mode8(result, frame.rgba, frame.width, frame.height,
                               position.x, position.y, transition_phase,
                               position.direct[3U]);
  }
}

void place_one_player_menu_labels(FrontEndFrame &target,
                                  const mh::content::SprArchive &menu_sprites,
                                  const mh::content::SprPositionData &positions,
                                  const OnePlayerChoice selection,
                                  const std::uint32_t selected_label_frame,
                                  const std::uint32_t transition_phase) {
  constexpr std::array labels{
      std::pair{OnePlayerPositionRecord::quick_race, std::string_view("tquic")},
      std::pair{OnePlayerPositionRecord::single_race,
                std::string_view("tsing")},
      std::pair{OnePlayerPositionRecord::league_race,
                std::string_view("tleag")},
      std::pair{OnePlayerPositionRecord::time_attack,
                std::string_view("tatta")},
      std::pair{OnePlayerPositionRecord::ghost_mode, std::string_view("tghos")},
  };
  for (std::size_t label = 0U; label < labels.size(); ++label) {
    const auto &[record, sprite] = labels[label];
    place_transition_sprite(
        target, menu_sprites, positions, one_player_position_bank,
        position_record_index(record), sprite, transition_phase,
        selection == static_cast<OnePlayerChoice>(label)
            ? selected_label_frame
            : main_menu_unselected_label_frame,
        -1, one_player_dial_offset_x, one_player_dial_offset_y);
  }
}

void compose_one_player_base_with_glitch(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const std::uint32_t transition_phase, const MainMenuGlitchFrame &glitch,
    FrontEndFrame &result) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "One Player background must be exactly 640x400 RGBA");
  }
  result.rgba = background.rgba;
  place_transition_sprite(
      result, menu_sprites, positions, one_player_position_bank,
      position_record_index(OnePlayerPositionRecord::background_dots), "dots",
      transition_phase);
  if (transition_phase != 0U) {
    place_transition_sprite(
        result, menu_sprites, positions, one_player_position_bank,
        position_record_index(OnePlayerPositionRecord::screen_badge), "scr7",
        transition_phase);
  } else if (glitch.screen_badge_active) {
    place_glitched_sprite(result, menu_sprites, positions,
                          OnePlayerPositionRecord::screen_badge, "scr7",
                          transition_phase, glitch.screen_badge_phase);
  } else {
    place_sprite(result, menu_sprites, positions,
                 OnePlayerPositionRecord::screen_badge, "scr7",
                 transition_phase);
  }
  if (transition_phase == 0U && glitch.selector_noise_active) {
    place_selector_noise_sprite(result, menu_sprites, positions,
                                OnePlayerPositionRecord::selector, "msel",
                                glitch.selector_noise_amplitude);
  } else {
    place_transition_mode8_sprite(result, menu_sprites, positions,
                                  OnePlayerPositionRecord::selector, "msel",
                                  transition_phase);
  }
  if (transition_phase != 0U) {
    place_transition_sprite(
        result, menu_sprites, positions, one_player_position_bank,
        position_record_index(OnePlayerPositionRecord::header), "singl",
        transition_phase);
  } else if (glitch.header_active) {
    place_glitched_sprite(result, menu_sprites, positions,
                          OnePlayerPositionRecord::header, "singl",
                          transition_phase, glitch.header_phase);
  } else {
    place_sprite(result, menu_sprites, positions,
                 OnePlayerPositionRecord::header, "singl", transition_phase);
  }
  // Move the setup summary with Quick Race. Its shorter extension preserves
  // the accepted bottom edge after the complete dial cluster moves down.
  constexpr std::uint32_t information_extension = 7U;
  place_one_player_information_sprite(
      result, menu_sprites, positions,
      OnePlayerPositionRecord::information_left, "info1", 64U,
      information_extension, transition_phase, one_player_dial_offset_x,
      one_player_dial_offset_y);
  place_one_player_information_sprite(
      result, menu_sprites, positions,
      OnePlayerPositionRecord::information_right, "info2", 2U,
      information_extension, transition_phase, one_player_dial_offset_x,
      one_player_dial_offset_y);
}

} // namespace

void advance_front_end_background_animation(
    FrontEndBackgroundAnimationState &state,
    const std::uint32_t elapsed_ms) noexcept {
  state.elapsed_ms += elapsed_ms;
  for (auto &object : state.objects) {
    object.depth -= static_cast<float>(elapsed_ms) * object.depth_speed;
    if (object.depth > 200.0F) {
      continue;
    }

    object.model_index = retail_random(state.random_state) %
                         static_cast<std::uint32_t>(state.objects.size());
    object.depth_speed =
        static_cast<float>((retail_random(state.random_state) & 0x1fffU) +
                           0x200U) *
        (1.0F / 2048.0F);
    object.angular_x = static_cast<float>(
        static_cast<std::int32_t>(retail_random(state.random_state) & 0x7ffU) -
        0x400);
    object.angular_y = static_cast<float>(
        static_cast<std::int32_t>(retail_random(state.random_state) & 0x7ffU) -
        0x400);
    if (std::abs(object.angular_x) < 260.0F &&
        std::abs(object.angular_y) < 260.0F) {
      auto replacement =
          (retail_random(state.random_state) & 1U) == 0U ? -300.0F : 300.0F;
      if ((retail_random(state.random_state) & 1U) == 0U) {
        object.angular_x = replacement;
      } else {
        object.angular_y = replacement;
      }
    }
    object.angular_z = static_cast<float>(
        (retail_random(state.random_state) & 0x7ffU) + 0x400U);
    object.depth = object.angular_z;
    object.phase_x = retail_random(state.random_state);
    object.phase_y = retail_random(state.random_state);
    object.phase_z = retail_random(state.random_state);

    // The retail routine consumes one additional random value for a companion
    // lifetime field used by the same ambient subsystem.
    static_cast<void>(retail_random(state.random_state));
  }
}

namespace {

void compose_front_end_animated_background_canvas(
    const mh::content::TgaImage &background,
    const std::span<const mh::content::LobData> line_objects,
    const FrontEndBackgroundAnimationState &state,
    const std::array<std::array<std::uint8_t, 3U>, 256U> &menu_palette,
    const std::uint32_t canvas_width, FrontEndBackgroundRenderCache &cache,
    mh::content::TgaImage &result) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "front-end animated background must be 640x400 RGBA");
  }
  if (!background.has_palette || background.palette_length <= 31U) {
    throw ToolError(ExitCode::format,
                    "front-end ambient layer requires the Back.tga palette");
  }
  if (background.palette_indices.size() !=
      static_cast<std::size_t>(front_end_logical_width) *
          front_end_logical_height) {
    throw ToolError(
        ExitCode::format,
        "front-end ambient layer requires the Back.tga source indices");
  }
  if (line_objects.size() != state.objects.size()) {
    throw ToolError(ExitCode::format,
                    "front-end ambient layer requires lobj00 through lobj08");
  }

  if (canvas_width < front_end_logical_width ||
      canvas_width > std::numeric_limits<std::uint16_t>::max()) {
    throw ToolError(ExitCode::format,
                    "front-end ambient canvas width is unsupported");
  }
  const auto content_offset_x = (canvas_width - front_end_logical_width) / 2U;
  // MENU.SPR supplies the retained upper part of the display palette. Loading
  // Back.tga replaces only the range described by its 63-entry colour map.
  // The retail line routine then adds a colour index to each destination byte
  // with saturation; it does not paint a fixed RGB colour.
  auto combined_palette = menu_palette;
  for (std::size_t entry = 0U; entry < background.palette_length; ++entry) {
    const auto index =
        static_cast<std::size_t>(background.palette_first) + entry;
    combined_palette[index] = background.palette[index];
  }
  const auto rebuild_cache =
      cache.source != &background || cache.target != &result ||
      cache.width != canvas_width || cache.palette != combined_palette;
  if (rebuild_cache) {
    result = background;
    if (canvas_width != front_end_logical_width) {
      result.width = static_cast<std::uint16_t>(canvas_width);
      result.palette_indices.assign(
          static_cast<std::size_t>(canvas_width) * front_end_logical_height,
          0U);
      result.rgba.assign(static_cast<std::size_t>(canvas_width) *
                             front_end_logical_height * 4U,
                         0U);
      for (std::uint32_t y = 0U; y < front_end_logical_height; ++y) {
        const auto source_index =
            static_cast<std::size_t>(y) * front_end_logical_width;
        const auto destination_index =
            static_cast<std::size_t>(y) * canvas_width + content_offset_x;
        std::copy_n(background.palette_indices.begin() +
                        static_cast<std::ptrdiff_t>(source_index),
                    front_end_logical_width,
                    result.palette_indices.begin() +
                        static_cast<std::ptrdiff_t>(destination_index));
      }
    }
    result.palette = combined_palette;
    result.palette_first = 0U;
    result.palette_length = 256U;

    std::array<std::uint32_t, 256U> packed_palette{};
    for (std::size_t index = 0U; index < packed_palette.size(); ++index) {
      const auto &color = result.palette[index];
      packed_palette[index] = static_cast<std::uint32_t>(color[0U]) |
                              (static_cast<std::uint32_t>(color[1U]) << 8U) |
                              (static_cast<std::uint32_t>(color[2U]) << 16U) |
                              0xff000000U;
    }
    auto *destination = result.rgba.data();
    const auto content_end_x = content_offset_x + front_end_logical_width;
    std::uint32_t x = 0U;
    for (const auto palette_index : result.palette_indices) {
      const auto in_side_wing =
          content_offset_x != 0U &&
          (x < content_offset_x || x >= content_end_x);
      const auto pixel = in_side_wing && palette_index == 0U
                             ? 0xff000000U
                             : packed_palette[palette_index];
      std::memcpy(destination, &pixel, sizeof(pixel));
      destination += sizeof(pixel);
      if (++x == canvas_width) {
        x = 0U;
      }
    }
    cache.source = &background;
    cache.target = &result;
    cache.width = canvas_width;
    cache.palette = combined_palette;
    cache.base_indices = result.palette_indices;
    cache.base_rgba = result.rgba;
    cache.dirty_pixels.clear();
    cache.dirty_marks.assign(result.palette_indices.size(), 0U);
  } else {
    for (const auto pixel : cache.dirty_pixels) {
      cache.dirty_marks[pixel] = 0U;
      result.palette_indices[pixel] = cache.base_indices[pixel];
      std::memcpy(result.rgba.data() + static_cast<std::size_t>(pixel) * 4U,
                  cache.base_rgba.data() + static_cast<std::size_t>(pixel) * 4U,
                  4U);
    }
    cache.dirty_pixels.clear();
  }
  const auto orbit_phase = static_cast<double>(state.elapsed_ms) * 0.001;
  const auto orbit_sine = std::sin(orbit_phase);
  const auto orbit_cosine = std::cos(orbit_phase);
  draw_front_end_orbit(result,
                       100 + static_cast<std::int32_t>(content_offset_x), 150,
                       200.0, orbit_sine, orbit_cosine, 16U, cache);
  draw_front_end_orbit(result,
                       470 + static_cast<std::int32_t>(content_offset_x), 150,
                       200.0, -orbit_sine, orbit_cosine, 16U, cache);

  const auto &phase_trigonometry = front_end_phase_trigonometry();
  auto &projected = cache.projected_vertices;
  for (const auto &instance : state.objects) {
    if (instance.model_index >= line_objects.size()) {
      throw ToolError(ExitCode::format,
                      "front-end ambient model index is out of range");
    }
    const auto &model = line_objects[instance.model_index];
    if (!model.summary.edge_indices_in_bounds || model.vertices.empty()) {
      throw ToolError(ExitCode::format,
                      "front-end ambient LOB has invalid topology");
    }

    const auto center_x =
        (model.summary.minimum[0U] + model.summary.maximum[0U]) * 0.5F;
    const auto center_y =
        (model.summary.minimum[1U] + model.summary.maximum[1U]) * 0.5F;
    const auto center_z =
        (model.summary.minimum[2U] + model.summary.maximum[2U]) * 0.5F;
    const auto radius =
        std::max({std::abs(model.summary.minimum[0U] - center_x),
                  std::abs(model.summary.maximum[0U] - center_x),
                  std::abs(model.summary.minimum[1U] - center_y),
                  std::abs(model.summary.maximum[1U] - center_y),
                  std::abs(model.summary.minimum[2U] - center_z),
                  std::abs(model.summary.maximum[2U] - center_z)});
    if (!(radius > 0.0F)) {
      continue;
    }
    const auto model_scale = 160.0 / static_cast<double>(radius);

    // The retail call passes the three time-advanced integer phase fields as
    // rotations. The bounded random floats are the X/Y translation and depth;
    // treating the phases as translations makes the objects enormous and
    // causes them to sweep repeatedly across the entire screen.
    const auto angle_x = (instance.phase_x + state.elapsed_ms) & 0x0fffU;
    const auto angle_y = (instance.phase_y + state.elapsed_ms / 7U) & 0x0fffU;
    const auto angle_z = (instance.phase_z + state.elapsed_ms / 3U) & 0x0fffU;
    const auto sx = phase_trigonometry.sine[angle_x];
    const auto cx = phase_trigonometry.cosine[angle_x];
    const auto sy = phase_trigonometry.sine[angle_y];
    const auto cy = phase_trigonometry.cosine[angle_y];
    const auto sz = phase_trigonometry.sine[angle_z];
    const auto cz = phase_trigonometry.cosine[angle_z];

    const std::array<std::array<double, 3U>, 3U> matrix{{
        {{(sz * sy + cz * cx * cy) * 288.0, (sz * cx * cy - cz * sy) * 288.0,
          sx * cy * 288.0}},
        {{cz * sx * 240.0, sz * sx * 240.0, -cx * 240.0}},
        {{cz * cx * sy - sz * cy, cz * cy - sz * cx * sy, sx * sy}},
    }};
    const auto translation_x = static_cast<double>(instance.angular_x) * 288.0;
    const auto translation_y = static_cast<double>(instance.angular_y) * 240.0;
    const auto translation_z = static_cast<double>(instance.depth) + 6.0;

    projected.clear();
    projected.reserve(model.vertices.size());
    for (const auto &vertex : model.vertices) {
      const std::array<double, 3U> point{
          (static_cast<double>(vertex.position[0U]) - center_x) * model_scale,
          (static_cast<double>(vertex.position[1U]) - center_y) * model_scale,
          (static_cast<double>(vertex.position[2U]) - center_z) * model_scale};
      const auto transformed_x = point[0U] * matrix[0U][0U] +
                                 point[1U] * matrix[0U][1U] +
                                 point[2U] * matrix[0U][2U] + translation_x;
      const auto transformed_y = point[0U] * matrix[1U][0U] +
                                 point[1U] * matrix[1U][1U] +
                                 point[2U] * matrix[1U][2U] + translation_y;
      const auto transformed_z = point[0U] * matrix[2U][0U] +
                                 point[1U] * matrix[2U][1U] +
                                 point[2U] * matrix[2U][2U] + translation_z;
      if (std::abs(transformed_z) < 1.0e-9) {
        projected.push_back({std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::quiet_NaN()});
      } else {
        projected.push_back({transformed_x / transformed_z +
                                 static_cast<double>(canvas_width) * 0.5,
                             transformed_y / transformed_z + 200.0});
      }
    }

    const auto palette_index = static_cast<std::size_t>(std::clamp(
        static_cast<std::int32_t>(32.0F - instance.depth * (1.0F / 64.0F)), 2,
        31));
    for (const auto &edge : model.edges) {
      const auto &first = projected[edge.first];
      const auto &second = projected[edge.second];
      draw_front_end_line(result, first[0U], first[1U], second[0U], second[1U],
                          static_cast<std::uint8_t>(palette_index), cache);
    }
  }
  std::array<std::uint32_t, 256U> packed_palette{};
  for (std::size_t index = 0U; index < packed_palette.size(); ++index) {
    const auto &color = result.palette[index];
    packed_palette[index] = static_cast<std::uint32_t>(color[0U]) |
                            (static_cast<std::uint32_t>(color[1U]) << 8U) |
                            (static_cast<std::uint32_t>(color[2U]) << 16U) |
                            0xff000000U;
  }
  for (const auto pixel_index : cache.dirty_pixels) {
    const auto pixel = packed_palette[result.palette_indices[pixel_index]];
    std::memcpy(result.rgba.data() +
                    static_cast<std::size_t>(pixel_index) * 4U,
                &pixel, sizeof(pixel));
  }
}

} // namespace

void compose_front_end_animated_background(
    const mh::content::TgaImage &background,
    const std::span<const mh::content::LobData> line_objects,
    const FrontEndBackgroundAnimationState &state,
    const std::array<std::array<std::uint8_t, 3U>, 256U> &menu_palette,
    FrontEndBackgroundRenderCache &cache, mh::content::TgaImage &result) {
  compose_front_end_animated_background_canvas(
      background, line_objects, state, menu_palette, front_end_logical_width,
      cache, result);
}

void compose_front_end_widescreen_animated_background(
    const mh::content::TgaImage &background,
    const std::span<const mh::content::LobData> line_objects,
    const FrontEndBackgroundAnimationState &state,
    const std::array<std::array<std::uint8_t, 3U>, 256U> &menu_palette,
    FrontEndBackgroundRenderCache &cache, mh::content::TgaImage &result) {
  compose_front_end_animated_background_canvas(
      background, line_objects, state, menu_palette,
      front_end_widescreen_logical_width, cache, result);
}

namespace {

MainMenuGlitchFrame advance_menu_glitch(
    MainMenuGlitchState &state, const std::size_t screen_badge_slot,
    const std::size_t header_slot, const std::uint32_t elapsed_ms) noexcept {
  constexpr std::uint32_t multiplier = 0x41c64e6dU;
  constexpr std::uint32_t increment = 0x3039U;
  constexpr std::int32_t initial_countdown = 4096;
  constexpr std::int32_t countdown_step = 256;
  const auto next_random = [&state]() noexcept {
    state.random_state = state.random_state * multiplier + increment;
    return (state.random_state >> 16U) & 0x7fffU;
  };
  MainMenuGlitchFrame result;
  for (std::size_t slot = 1U; slot <= 10U; ++slot) {
    const auto random_value = next_random();
    const auto flagged = slot == screen_badge_slot || slot == header_slot;
    if (random_value < 10U || state.countdown[slot] > 20) {
      if (!flagged) {
        continue;
      }
      const auto phase =
          static_cast<std::uint32_t>(state.countdown[slot] + 1024) & 4095U;
      if (slot == screen_badge_slot) {
        result.screen_badge_active = true;
        result.screen_badge_phase = phase;
      } else {
        result.header_active = true;
        result.header_phase = phase;
      }
      if (state.countdown[slot] <= 0) {
        state.countdown[slot] = initial_countdown;
      } else {
        state.countdown[slot] -= countdown_step;
      }
      continue;
    }

    const auto secondary_random = next_random() & 0x7ffU;
    if (secondary_random >= 10U && state.countdown[slot] <= 0) {
      continue;
    }
    const auto amplitude = (next_random() & 0x3fU) + 1U;
    static_cast<void>(amplitude);
    if (state.countdown[slot] <= 0) {
      state.countdown[slot] = static_cast<std::int32_t>(next_random() % 20U);
    } else {
      --state.countdown[slot];
    }
  }

  const auto next_selector_random = [&state]() noexcept {
    state.selector_random_state =
        state.selector_random_state * multiplier + increment;
    return (state.selector_random_state >> 16U) & 0x7fffU;
  };
  const auto schedule_selector = [&]() noexcept {
    state.selector_delay_ms = 5000U + next_selector_random() % 5001U;
  };
  if (!state.selector_timer_initialized) {
    schedule_selector();
    state.selector_timer_initialized = true;
  }
  if (state.selector_pulse_ms != 0U) {
    result.selector_noise_active = true;
    result.selector_noise_amplitude = state.selector_pulse_amplitude;
    if (elapsed_ms >= state.selector_pulse_ms) {
      state.selector_pulse_ms = 0U;
      schedule_selector();
    } else {
      state.selector_pulse_ms -= elapsed_ms;
    }
  } else if (elapsed_ms >= state.selector_delay_ms) {
    state.selector_delay_ms = 0U;
    state.selector_pulse_ms = 120U;
    state.selector_pulse_amplitude = 1U + next_selector_random() % 64U;
    result.selector_noise_active = true;
    result.selector_noise_amplitude = state.selector_pulse_amplitude;
  } else {
    state.selector_delay_ms -= elapsed_ms;
  }
  return result;
}

} // namespace

MainMenuGlitchFrame
advance_main_menu_glitch(MainMenuGlitchState &state,
                         const std::uint32_t elapsed_ms) noexcept {
  return advance_menu_glitch(state, 2U, 5U, elapsed_ms);
}

MainMenuGlitchFrame
advance_options_menu_glitch(MainMenuGlitchState &state,
                            const std::uint32_t elapsed_ms) noexcept {
  // Personal Options flags the header in slot 2 and the SCREEN badge in slot
  // 3 at RVA 0x84f2d/0x84f4a.
  return advance_menu_glitch(state, 3U, 2U, elapsed_ms);
}

MainMenuGlitchFrame
advance_one_player_menu_glitch(MainMenuGlitchState &state,
                               const std::uint32_t elapsed_ms) noexcept {
  // Initializer RVA 0x89d7c marks slot 2 (scr7) and slot 4 (singl).
  return advance_menu_glitch(state, 2U, 4U, elapsed_ms);
}

CarSetupPresentation::Performance
advance_car_setup_performance(CarSetupPerformanceAnimationState &state,
                              const CarSetupPresentation::Performance &target,
                              const std::uint32_t elapsed_ms) noexcept {
  const auto factor = static_cast<float>(elapsed_ms) * 0.005F;
  state.current.top_speed +=
      (target.top_speed - state.current.top_speed) * factor;
  state.current.acceleration +=
      (target.acceleration - state.current.acceleration) * factor;
  state.current.handling += (target.handling - state.current.handling) * factor;
  return state.current;
}

SoundOptionsGlitchFrame advance_sound_options_transition(
    MainMenuGlitchState &state, const std::uint32_t transition_phase) noexcept {
  constexpr std::uint32_t multiplier = 0x41c64e6dU;
  constexpr std::uint32_t increment = 0x3039U;
  constexpr std::array<std::pair<std::size_t, std::int32_t>, 6U>
      stochastic_records{{
          {2U, 10240},
          {10U, 1024},
          {15U, 1024},
          {16U, 1024},
          {17U, 1024},
          {18U, 1280},
      }};
  SoundOptionsGlitchFrame result;
  for (const auto &[record, scalar] : stochastic_records) {
    const auto maximum = std::max<std::int32_t>(
        1, static_cast<std::int32_t>(
               (static_cast<std::int64_t>(scalar) * transition_phase) >> 8));
    state.random_state = state.random_state * multiplier + increment;
    const auto random_value = (state.random_state >> 16U) & 0x7fffU;
    result.transition_noise_active[record] = true;
    result.transition_noise_amplitude[record] =
        random_value % static_cast<std::uint32_t>(maximum);
  }
  return result;
}

std::uint32_t
main_menu_selected_label_frame(const std::uint32_t elapsed_ms) noexcept {
  constexpr double full_turn = 6.283185307179586476925286766559;
  constexpr std::uint32_t phase_mask = 4095U;
  constexpr double phase_count = 4096.0;
  const auto phase = (elapsed_ms * 8U) & phase_mask;
  // The retail table stores single-precision sine samples before the x87
  // double-precision scale/add and truncation.
  const auto sine = static_cast<float>(
      std::sin(full_turn * static_cast<double>(phase) / phase_count));
  return static_cast<std::uint32_t>(3.5 + 3.5 * static_cast<double>(sine));
}

std::uint32_t
front_end_transition_phase(const std::uint32_t screen_counter_ms) noexcept {
  if (screen_counter_ms >= front_end_transition_leg_ms) {
    return 0U;
  }
  const auto remaining = static_cast<std::uint64_t>(
                             front_end_transition_leg_ms - screen_counter_ms) *
                         256U;
  return static_cast<std::uint32_t>(remaining / front_end_transition_leg_ms);
}

CenteredMenuListWindow
centered_menu_list_window(const std::size_t item_count,
                          const std::size_t selected_index,
                          const std::size_t visible_rows) {
  if (item_count == 0U) {
    return {};
  }
  if (selected_index >= item_count) {
    throw ToolError(ExitCode::format,
                    "centered menu-list selected index is out of range");
  }
  if (visible_rows == 0U) {
    throw ToolError(ExitCode::format,
                    "centered menu-list visible row count is zero");
  }

  // p3.1 RVA 0x7fc98..0x7fcd4 starts at selected-floor(rows/2) and
  // displays at most `rows` strings. It deliberately does not back-fill the
  // window when the selected item is near the end.
  const auto half = visible_rows / 2U;
  const auto first = selected_index > half ? selected_index - half : 0U;
  return {first, std::min(item_count, first + visible_rows)};
}

FrontEndFrame
compose_main_menu_base(const mh::content::TgaImage &background,
                       const mh::content::SprArchive &menu_sprites,
                       const mh::content::SprPositionData &positions,
                       const std::uint32_t transition_phase) {
  FrontEndFrame result;
  compose_main_menu_base_with_glitch(background, menu_sprites, positions,
                                     transition_phase, {}, result);
  return result;
}

FrontEndFrame
compose_main_menu_frame(const mh::content::TgaImage &background,
                        const mh::content::SprArchive &menu_sprites,
                        const mh::content::SprPositionData &positions,
                        const MainMenuChoice selection,
                        const std::uint32_t selected_label_frame) {
  auto result = compose_main_menu_base(background, menu_sprites, positions, 0U);
  const auto index = static_cast<std::size_t>(selection);
  if (index >= settled_pointer_polygons.size()) {
    throw ToolError(ExitCode::usage,
                    "main-menu selection is outside the recovered range");
  }
  if (selected_label_frame >= 8U) {
    throw ToolError(ExitCode::usage,
                    "main-menu selected-label frame must be in 0..7");
  }
  place_main_menu_labels(result, menu_sprites, positions, selection,
                         selected_label_frame, 0U);
  fill_pointer_polygon(result, settled_pointer_polygons[index],
                       main_dial_offset_x, main_dial_offset_y);
  return result;
}

FrontEndFrame
compose_main_menu_frame_at_time(const mh::content::TgaImage &background,
                                const mh::content::SprArchive &menu_sprites,
                                const mh::content::SprPositionData &positions,
                                const MainMenuChoice selection,
                                const std::uint32_t elapsed_ms) {
  FrontEndFrame result;
  compose_main_menu_dynamic_frame(
      background, menu_sprites, positions, selection, elapsed_ms,
      main_menu_pointer_target_units(selection), {}, 0U, result);
  return result;
}

void compose_main_menu_dynamic_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const MainMenuChoice selection, const std::uint32_t elapsed_ms,
    const float pointer_units, const MainMenuGlitchFrame &glitch,
    const std::uint32_t transition_phase, FrontEndFrame &result) {
  const auto index = static_cast<std::size_t>(selection);
  if (index >= settled_pointer_polygons.size()) {
    throw ToolError(ExitCode::usage,
                    "main-menu selection is outside the recovered range");
  }
  compose_main_menu_base_with_glitch(background, menu_sprites, positions,
                                     transition_phase, glitch, result);
  place_main_menu_labels(result, menu_sprites, positions, selection,
                         main_menu_selected_label_frame(elapsed_ms),
                         transition_phase);
  const auto center = sample(positions, MainMenuPositionRecord::pointer_center,
                             transition_phase);
  fill_dynamic_pointer(result, pointer_units, center.x + main_dial_offset_x,
                       center.y + main_dial_offset_y);
}

FrontEndFrame compose_confirmation_overlay(
    FrontEndFrame base, const mh::content::SprArchive &menu_sprites,
    const mh::content::FntData &font, const ExitConfirmationChoice selection,
    const std::int32_t slide_x, const std::string_view first_line,
    const std::string_view second_line,
    const std::array<std::string_view, 2U> &choices) {
  // The common requester renderer at RVA 0x800c0 draws `req` at fixed Y=150
  // and animates X from -320 to 160. Its two message rows begin at Y=166
  // with a 12-pixel step and are centred at X+180.
  constexpr std::int32_t requester_y = 150;
  constexpr std::int32_t requester_center_offset = 180;
  constexpr std::array<std::uint8_t, 3U> orange{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> white{255U, 255U, 255U};
  constexpr std::array<std::uint8_t, 3U> black{0U, 0U, 0U};
  const auto fill_requester_well = [&](const std::int32_t local_y,
                                       const std::uint32_t height) {
    const auto left = std::max<std::int32_t>(0, slide_x + 74);
    const auto right = std::min<std::int32_t>(base.width, slide_x + 290);
    if (right > left) {
      fill_rectangle(base, static_cast<std::uint32_t>(left),
                     static_cast<std::uint32_t>(requester_y + local_y),
                     static_cast<std::uint32_t>(right - left), height, black);
    }
  };
  // `req` intentionally leaves these wells transparent; the common requester
  // paints them before drawing the lavender frame.
  fill_requester_well(10, 100U);
  const auto &requester = decode_required(menu_sprites, "req");
  blit_rgba(base, requester.rgba, requester.width, requester.height, slide_x,
            requester_y);

  const auto center_x = slide_x + requester_center_offset;
  const auto draw_centered = [&](const std::string_view text,
                                 const std::int32_t y,
                                 const std::array<std::uint8_t, 3U> color) {
    const auto width =
        static_cast<std::int32_t>(mh::content::measure_fnt_text(font, text));
    draw_fnt_text(base, font, text, center_x - width / 2, y, color);
  };
  draw_centered(first_line, 166, orange);
  draw_centered(second_line, 178, orange);

  // RVA 0x80180 measures every choice, adds 16 pixels of authored padding,
  // centres the complete row at X+180, and renders the active choice with the
  // common inverse-video flag.
  std::array<std::int32_t, 2U> cell_widths{};
  std::int32_t row_width = 0;
  for (std::size_t index = 0U; index < choices.size(); ++index) {
    cell_widths[index] =
        static_cast<std::int32_t>(
            mh::content::measure_fnt_text(font, choices[index])) +
        16;
    row_width += cell_widths[index];
  }
  auto choice_x = center_x - row_width / 2;
  const auto selected_index = static_cast<std::size_t>(selection);
  for (std::size_t index = 0U; index < choices.size(); ++index) {
    const auto selected = index == selected_index;
    if (selected && choice_x >= 0) {
      fill_rectangle(base, static_cast<std::uint32_t>(choice_x), 232U,
                     static_cast<std::uint32_t>(cell_widths[index]), 20U,
                     white);
    }
    const auto text_width = static_cast<std::int32_t>(
        mh::content::measure_fnt_text(font, choices[index]));
    draw_fnt_text(base, font, choices[index],
                  choice_x + (cell_widths[index] - text_width) / 2, 234,
                  selected ? black : white);
    choice_x += cell_widths[index];
  }
  return base;
}

FrontEndFrame compose_exit_confirmation_overlay(
    FrontEndFrame base, const mh::content::SprArchive &menu_sprites,
    const mh::content::FntData &font, const ExitConfirmationChoice selection,
    const std::int32_t slide_x) {
  return compose_confirmation_overlay(
      std::move(base), menu_sprites, font, selection, slide_x,
      "Exit to windows.", "Are you sure?", {"Yes, please", "No way!"});
}

FrontEndFrame compose_league_create_confirmation_overlay(
    FrontEndFrame base, const mh::content::SprArchive &menu_sprites,
    const mh::content::FntData &font, const ExitConfirmationChoice selection,
    const std::int32_t slide_x) {
  return compose_confirmation_overlay(std::move(base), menu_sprites, font,
                                      selection, slide_x, "Create this league.",
                                      "Are you sure?", {"Yes, keep it!", "No"});
}

FrontEndFrame compose_league_delete_confirmation_overlay(
    FrontEndFrame base, const mh::content::SprArchive &menu_sprites,
    const mh::content::FntData &font, const ExitConfirmationChoice selection,
    const std::int32_t slide_x) {
  return compose_confirmation_overlay(
      std::move(base), menu_sprites, font, selection, slide_x,
      "Delete this league.", "Are you sure?", {"Yes, delete", "Keep it!"});
}

void compose_options_menu_dynamic_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprArchive &menu_dial_sprites,
    const mh::content::SprPositionData &positions,
    const OptionsMenuChoice selection, const DifficultyChoice difficulty,
    const std::uint32_t elapsed_ms, const float pointer_units,
    const MainMenuGlitchFrame &glitch, const std::uint32_t transition_phase,
    FrontEndFrame &result) {
  const auto selection_index = static_cast<std::size_t>(selection);
  if (selection_index >
      static_cast<std::size_t>(OptionsMenuChoice::difficulty)) {
    throw ToolError(ExitCode::usage,
                    "options selection is outside the recovered range");
  }
  const auto difficulty_index = static_cast<std::uint32_t>(difficulty);
  if (difficulty_index > static_cast<std::uint32_t>(DifficultyChoice::hard)) {
    throw ToolError(ExitCode::usage,
                    "difficulty is outside the recovered range");
  }

  compose_options_menu_base_with_glitch(
      background, menu_sprites, menu_dial_sprites, positions, transition_phase,
      glitch, result);
  place_options_menu_labels(
      result, menu_sprites, menu_dial_sprites, positions, selection,
      main_menu_selected_label_frame(elapsed_ms), transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, options_menu_position_bank,
      position_record_index(OptionsMenuPositionRecord::difficulty_frame),
      "difin", transition_phase, 0U, -1, -17, 56);
  place_transition_sprite(
      result, menu_sprites, positions, options_menu_position_bank,
      position_record_index(OptionsMenuPositionRecord::difficulty_value), "emh",
      transition_phase, difficulty_index * 8U, -1, -17, 56);
  const auto center = sample(
      positions, OptionsMenuPositionRecord::pointer_center, transition_phase);
  fill_dynamic_pointer(result, pointer_units, center.x, center.y);
}

namespace {

void draw_one_player_information_text(
    FrontEndFrame &target, const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const OnePlayerPanelValues &values,
    const std::uint32_t transition_phase) {
  const auto panel = sample(
      positions, OnePlayerPositionRecord::information_left, transition_phase);
  const auto label_offset =
      sample(positions, OnePlayerPositionRecord::information_label_offset,
             transition_phase);
  const auto row_offset =
      sample(positions, OnePlayerPositionRecord::information_row_offset,
             transition_phase);
  const auto left_x = static_cast<std::uint32_t>(
      std::max(panel.x + label_offset.x + one_player_dial_offset_x, 0));
  const auto first_y = static_cast<std::uint32_t>(
      std::max(panel.y + label_offset.y + one_player_dial_offset_y +
                   menu_information_text_offset_y,
               0));
  const auto right_x = static_cast<std::uint32_t>(std::max(
      panel.x + label_offset.x + row_offset.x + one_player_dial_offset_x, 0));
  const auto row_step = static_cast<std::uint32_t>(std::max(row_offset.y, 1));
  constexpr std::array<std::uint8_t, 3U> label_color{96U, 96U, 192U};
  draw_fnt_text(target, font, "Mode:", left_x, first_y, label_color);
  draw_fnt_text(target, font, "Car:", left_x, first_y + row_step, label_color);
  const auto third_left =
      values.mode == OnePlayerMode::league_race  ? std::string_view("Division:")
      : values.mode == OnePlayerMode::ghost_mode ? std::string_view("Name:")
                                                 : std::string_view("Laps:");
  draw_fnt_text(target, font, third_left, left_x, first_y + row_step * 2U,
                label_color);
  draw_fnt_text(target, font, "Difficulty:", right_x, first_y, label_color);
  draw_fnt_text(target, font, "Transmission:", right_x, first_y + row_step,
                label_color);
  if (values.mode != OnePlayerMode::ghost_mode) {
    draw_fnt_text(target, font, "Track:", right_x, first_y + row_step * 2U,
                  label_color);
  }

  const auto value_offset =
      sample(positions, OnePlayerPositionRecord::information_value_offset,
             transition_phase);
  const auto left_value_x = static_cast<std::uint32_t>(std::max(
      panel.x + label_offset.x + value_offset.x + one_player_dial_offset_x, 0));
  const auto right_value_x = static_cast<std::uint32_t>(
      std::max(panel.x + label_offset.x + row_offset.x + value_offset.y +
                   one_player_dial_offset_x,
               0));
  constexpr std::array<std::uint8_t, 3U> value_color{255U, 100U, 0U};
  constexpr std::array mode_names{
      std::string_view("Single race"),
      std::string_view("League race"),
      std::string_view("Time attack"),
      std::string_view("Ghostmode"),
  };
  const auto mode_index = static_cast<std::size_t>(values.mode);
  draw_fnt_text(target, font, mode_names[mode_index], left_value_x, first_y,
                value_color);
  if (!values.car.empty()) {
    draw_fnt_text(target, font, values.car, left_value_x, first_y + row_step,
                  value_color);
  }
  if (!values.third_value.empty()) {
    draw_fnt_text(target, font, values.third_value, left_value_x,
                  first_y + row_step * 2U, value_color);
  }
  if (!values.difficulty.empty()) {
    draw_fnt_text(target, font, values.difficulty, right_value_x, first_y,
                  value_color);
  }
  if (!values.transmission.empty()) {
    draw_fnt_text(target, font, values.transmission, right_value_x,
                  first_y + row_step, value_color);
  }
  if (values.mode != OnePlayerMode::ghost_mode && !values.track.empty()) {
    draw_fnt_text(target, font, values.track, right_value_x,
                  first_y + row_step * 2U, value_color);
  }
}

} // namespace

void compose_one_player_menu_dynamic_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const OnePlayerChoice selection,
    const std::uint32_t elapsed_ms, const float pointer_units,
    const MainMenuGlitchFrame &glitch, const std::uint32_t transition_phase,
    const OnePlayerPanelValues &values, FrontEndFrame &result) {
  const auto selection_index = static_cast<std::size_t>(selection);
  if (selection_index > static_cast<std::size_t>(OnePlayerChoice::ghost_mode)) {
    throw ToolError(ExitCode::usage,
                    "One Player selection is outside the recovered range");
  }
  compose_one_player_base_with_glitch(background, menu_sprites, positions,
                                      transition_phase, glitch, result);
  place_one_player_menu_labels(result, menu_sprites, positions, selection,
                               main_menu_selected_label_frame(elapsed_ms),
                               transition_phase);

  draw_one_player_information_text(result, positions, font, values,
                                   transition_phase);

  const auto center = sample(positions, OnePlayerPositionRecord::pointer_center,
                             transition_phase);
  fill_dynamic_pointer(result, pointer_units,
                       center.x + one_player_pointer_offset_x,
                       center.y + one_player_pointer_offset_y);
}

void overlay_one_player_display_information(
    FrontEndFrame &display, const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const OnePlayerPanelValues &values,
    const std::uint32_t transition_phase) {
  constexpr std::uint32_t information_extension = 7U;
  place_one_player_information_sprite(
      display, menu_sprites, positions,
      OnePlayerPositionRecord::information_left, "info1", 64U,
      information_extension, transition_phase, one_player_dial_offset_x,
      one_player_dial_offset_y);
  place_one_player_information_sprite(
      display, menu_sprites, positions,
      OnePlayerPositionRecord::information_right, "info2", 2U,
      information_extension, transition_phase, one_player_dial_offset_x,
      one_player_dial_offset_y);
  draw_one_player_information_text(display, positions, font, values,
                                   transition_phase);
}

namespace {

void place_league_overview_information(
    FrontEndFrame &target, const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font,
    const LeagueOverviewPresentation &presentation,
    const std::uint32_t transition_phase) {
  // The League summary uses the same visual panel as Single. Reuse bank 10's
  // two bracket positions, lower extension, and column geometry so both pages
  // have identical bounds rather than bank 12's shorter, right-shifted panel.
  constexpr std::uint32_t information_extension = 7U;
  place_one_player_information_sprite(
      target, menu_sprites, positions,
      OnePlayerPositionRecord::information_left, "info1", 64U,
      information_extension, transition_phase, one_player_dial_offset_x,
      one_player_dial_offset_y);
  place_one_player_information_sprite(
      target, menu_sprites, positions,
      OnePlayerPositionRecord::information_right, "info2", 2U,
      information_extension, transition_phase, one_player_dial_offset_x,
      one_player_dial_offset_y);

  constexpr std::array<std::uint8_t, 3U> label_color{96U, 96U, 192U};
  constexpr std::array<std::uint8_t, 3U> value_color{255U, 100U, 0U};
  const auto panel = sample(
      positions, OnePlayerPositionRecord::information_left, transition_phase);
  const auto label_offset =
      sample(positions, OnePlayerPositionRecord::information_label_offset,
             transition_phase);
  const auto column_and_row =
      sample(positions, OnePlayerPositionRecord::information_row_offset,
             transition_phase);
  const auto value_offset =
      sample(positions, OnePlayerPositionRecord::information_value_offset,
             transition_phase);
  const auto left_x = panel.x + label_offset.x + one_player_dial_offset_x;
  const auto right_x = left_x + column_and_row.x;
  const auto first_y = panel.y + label_offset.y + one_player_dial_offset_y +
                       menu_information_text_offset_y;
  const auto row_step = std::max(column_and_row.y, 1);
  const auto left_value_x = left_x + value_offset.x;
  const auto right_value_x = right_x + value_offset.y;

  draw_fnt_text(target, font, "League:", left_x, first_y, label_color);
  draw_fnt_text(target, font, "Name:", left_x, first_y + row_step, label_color);
  draw_fnt_text(target, font, "Division:", left_x, first_y + row_step * 2,
                label_color);
  draw_fnt_text(target, font, "Difficulty:", right_x, first_y, label_color);
  draw_fnt_text(target, font, "Score:", right_x, first_y + row_step,
                label_color);
  draw_fnt_text(target, font, "Track:", right_x, first_y + row_step * 2,
                label_color);

  draw_fnt_text(target, font, presentation.league_name, left_value_x, first_y,
                value_color);
  draw_fnt_text(target, font, presentation.player_name, left_value_x,
                first_y + row_step, value_color);
  draw_fnt_text(target, font, std::to_string(presentation.division),
                left_value_x, first_y + row_step * 2, value_color);
  draw_fnt_text(target, font, presentation.difficulty, right_value_x, first_y,
                value_color);
  draw_fnt_text(target, font, std::to_string(presentation.score), right_value_x,
                first_y + row_step, value_color);
  draw_fnt_text(target, font, presentation.next_track, right_value_x,
                first_y + row_step * 2, value_color);
}

} // namespace

FrontEndFrame compose_league_overview_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font,
    const LeagueOverviewPresentation &presentation,
    const LeagueMenuChoice selection, const std::uint32_t elapsed_ms,
    const float pointer_units, const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "League overview background must be exactly 640x400 RGBA");
  }

  FrontEndFrame result;
  result.rgba = background.rgba;
  const auto record = [](const LeagueOverviewPositionRecord value) {
    return position_record_index(value);
  };

  // The accepted p3.1 frame fixes this bank-12 sprite correspondence. All
  // coordinates remain in the original logical 640x400 space; the front-end
  // presenter performs the retail 4:3 vertical presentation separately.
  place_transition_sprite(result, menu_sprites, positions,
                          league_overview_position_bank,
                          record(LeagueOverviewPositionRecord::background_dots),
                          "dots", transition_phase);
  place_transition_sprite(result, menu_sprites, positions,
                          league_overview_position_bank,
                          record(LeagueOverviewPositionRecord::screen_badge),
                          "scrc", transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, league_overview_position_bank,
      record(LeagueOverviewPositionRecord::header), "leagu", transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, league_overview_position_bank,
      record(LeagueOverviewPositionRecord::selector), "msel", transition_phase,
      0U, -1, league_dial_offset_x, league_dial_offset_y);
  place_transition_sprite(result, menu_sprites, positions,
                          league_overview_position_bank,
                          record(LeagueOverviewPositionRecord::continue_race),
                          "tlcon", transition_phase,
                          selection == LeagueMenuChoice::continue_race
                              ? main_menu_selected_label_frame(elapsed_ms)
                              : main_menu_unselected_label_frame,
                          -1, league_dial_offset_x, league_dial_offset_y);
  constexpr std::array league_labels{
      std::pair{LeagueOverviewPositionRecord::view_stats,
                std::string_view("tlvst")},
      std::pair{LeagueOverviewPositionRecord::select_league,
                std::string_view("tlloa")},
      std::pair{LeagueOverviewPositionRecord::new_league,
                std::string_view("tlnew")},
      std::pair{LeagueOverviewPositionRecord::delete_league,
                std::string_view("tldel")},
  };
  for (std::size_t label = 0U; label < league_labels.size(); ++label) {
    place_transition_sprite(
        result, menu_sprites, positions, league_overview_position_bank,
        record(league_labels[label].first), league_labels[label].second,
        transition_phase,
        selection == static_cast<LeagueMenuChoice>(label + 1U)
            ? main_menu_selected_label_frame(elapsed_ms)
            : main_menu_unselected_label_frame,
        -1, league_dial_offset_x, league_dial_offset_y);
  }
  place_league_overview_information(result, menu_sprites, positions, font,
                                    presentation, transition_phase);

  const auto center = sample(
      positions, league_overview_position_bank,
      record(LeagueOverviewPositionRecord::pointer_center), transition_phase);
  fill_dynamic_pointer(result, pointer_units,
                       center.x + league_pointer_offset_x,
                       center.y + league_pointer_offset_y);
  return result;
}

void overlay_league_overview_display_information(
    FrontEndFrame &display, const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font,
    const LeagueOverviewPresentation &presentation,
    const std::uint32_t transition_phase) {
  place_league_overview_information(display, menu_sprites, positions, font,
                                    presentation, transition_phase);
}

FrontEndFrame
compose_league_summary_frame(const mh::content::TgaImage &background,
                             const mh::content::SprArchive &menu_sprites,
                             const mh::content::SprPositionData &positions,
                             const mh::content::FntData &font,
                             const LeagueOverviewPresentation &presentation,
                             const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "League summary background must be exactly 640x400 RGBA");
  }
  FrontEndFrame result;
  result.rgba = background.rgba;
  const auto record = [](const LeagueSummaryPositionRecord value) {
    return position_record_index(value);
  };
  place_transition_sprite(result, menu_sprites, positions,
                          league_summary_position_bank,
                          record(LeagueSummaryPositionRecord::background_dots),
                          "dots", transition_phase);
  place_transition_sprite(result, menu_sprites, positions,
                          league_summary_position_bank,
                          record(LeagueSummaryPositionRecord::screen_badge),
                          "scrc", transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, league_summary_position_bank,
      record(LeagueSummaryPositionRecord::header), "leagu", transition_phase);

  constexpr std::array<std::uint8_t, 3U> label_color{96U, 96U, 192U};
  constexpr std::array<std::uint8_t, 3U> value_color{255U, 100U, 0U};
  const auto upper = sample(positions, league_summary_position_bank,
                            record(LeagueSummaryPositionRecord::upper_panel),
                            transition_phase);
  const auto columns = sample(
      positions, league_summary_position_bank,
      record(LeagueSummaryPositionRecord::upper_columns), transition_phase);
  const auto values = sample(positions, league_summary_position_bank,
                             record(LeagueSummaryPositionRecord::upper_values),
                             transition_phase);
  const auto right_x = upper.x + columns.x;
  const auto row_step = std::max(columns.y, 1);
  const auto left_value_x = upper.x + values.x;
  const auto right_value_x = right_x + values.x;

  draw_fnt_text(result, font, "League name:", upper.x, upper.y, label_color);
  draw_fnt_text(result, font, "Races done:", right_x, upper.y, label_color);
  draw_fnt_text(result, font, "Division:", upper.x, upper.y + row_step,
                label_color);
  draw_fnt_text(result, font, "Next track:", right_x, upper.y + row_step,
                label_color);
  draw_fnt_text(result, font, "Player name:", upper.x, upper.y + row_step * 2,
                label_color);
  draw_fnt_text(result, font, "Car:", right_x, upper.y + row_step * 2,
                label_color);
  draw_fnt_text(result, font, presentation.league_name, left_value_x, upper.y,
                value_color);
  draw_fnt_text(result, font, std::to_string(presentation.races_done),
                right_value_x, upper.y, value_color);
  draw_fnt_text(result, font, std::to_string(presentation.division),
                left_value_x, upper.y + row_step, value_color);
  draw_fnt_text(result, font, presentation.next_track, right_value_x,
                upper.y + row_step, value_color);
  draw_fnt_text(result, font, presentation.player_name, left_value_x,
                upper.y + row_step * 2, value_color);
  draw_fnt_text(result, font, presentation.car_name, right_value_x,
                upper.y + row_step * 2, value_color);

  const auto standings = sample(
      positions, league_summary_position_bank,
      record(LeagueSummaryPositionRecord::standings_panel), transition_phase);
  const auto standings_columns = sample(
      positions, league_summary_position_bank,
      record(LeagueSummaryPositionRecord::standings_columns), transition_phase);
  draw_fnt_text(result, font, "Pos:", standings.x, standings.y, label_color);
  draw_fnt_text(result, font, "Nick:", standings.x + standings_columns.x,
                standings.y, label_color);
  draw_fnt_text(result, font, "Score:", standings.x + standings_columns.x * 3,
                standings.y, label_color);
  const auto standings_row_step =
      static_cast<std::int32_t>(std::max(font.line_metric, 1U));
  const auto first_standings_y = standings.y + standings_row_step;
  constexpr std::array<std::uint8_t, 3U> opponent_color{160U, 160U, 224U};
  for (std::size_t index = 0U; index < presentation.standings.size(); ++index) {
    const auto &entry = presentation.standings[index];
    const auto y = first_standings_y +
                   static_cast<std::int32_t>(index) * standings_row_step;
    const auto color = entry.human ? value_color : opponent_color;
    draw_fnt_text(result, font, std::to_string(index + 1U), standings.x, y,
                  color);
    draw_fnt_text(result, font, entry.nickname,
                  standings.x + standings_columns.x, y, color);
    draw_fnt_text(result, font, std::to_string(entry.score),
                  standings.x + standings_columns.x * 3, y, color);
  }
  return result;
}

FrontEndFrame compose_league_select_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font,
    const std::span<const LeagueOverviewPresentation> leagues,
    const std::size_t selected_index, const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "League list background must be exactly 640x400 RGBA");
  }
  if (leagues.empty() || selected_index >= leagues.size()) {
    throw ToolError(ExitCode::usage,
                    "League list requires a valid selected league");
  }
  FrontEndFrame result;
  result.rgba = background.rgba;
  const auto record = [](const LeagueSelectPositionRecord value) {
    return position_record_index(value);
  };
  place_transition_sprite(result, menu_sprites, positions,
                          league_select_position_bank,
                          record(LeagueSelectPositionRecord::background_dots),
                          "dots", transition_phase);
  place_transition_sprite(result, menu_sprites, positions,
                          league_select_position_bank,
                          record(LeagueSelectPositionRecord::screen_badge),
                          "scrc", transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, league_select_position_bank,
      record(LeagueSelectPositionRecord::header), "leagu", transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, league_select_position_bank,
      record(LeagueSelectPositionRecord::selected_league_frame), "selco",
      transition_phase);

  constexpr std::array<std::uint8_t, 3U> title_color{96U, 96U, 192U};
  constexpr std::array<std::uint8_t, 3U> normal_color{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> selected_color{208U, 208U, 255U};
  const auto title = sample(positions, league_select_position_bank,
                            record(LeagueSelectPositionRecord::screen_title),
                            transition_phase);
  draw_fnt_text(result, font, "Select League", title.x, title.y, title_color);

  const auto frame =
      sample(positions, league_select_position_bank,
             record(LeagueSelectPositionRecord::selected_league_frame),
             transition_phase);
  const auto list_offset =
      sample(positions, league_select_position_bank,
             record(LeagueSelectPositionRecord::list_offset), transition_phase);
  const auto step =
      sample(positions, league_select_position_bank,
             record(LeagueSelectPositionRecord::row_step), transition_phase);
  const auto row_step = std::max(step.y, 1);
  const auto first = selected_index > 3U ? selected_index - 3U : 0U;
  const auto last = std::min(leagues.size(), selected_index + 4U);
  for (auto index = first; index < last; ++index) {
    const auto row = static_cast<std::int32_t>(index) -
                     static_cast<std::int32_t>(selected_index);
    draw_fnt_text(result, font, leagues[index].league_name,
                  frame.x + list_offset.x,
                  frame.y + list_offset.y + row * row_step,
                  index == selected_index ? selected_color : normal_color);
  }
  return result;
}

FrontEndFrame compose_league_create_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const LeagueOverviewPresentation &draft,
    const LeagueCreateField selection, const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "New League background must be exactly 640x400 RGBA");
  }
  FrontEndFrame result;
  result.rgba = background.rgba;
  place_transition_sprite(result, menu_sprites, positions,
                          league_create_position_bank, 1U, "dots",
                          transition_phase);
  place_transition_sprite(result, menu_sprites, positions,
                          league_create_position_bank, 2U, "scrc",
                          transition_phase);
  place_transition_sprite(result, menu_sprites, positions,
                          league_create_position_bank, 3U, "leagu",
                          transition_phase);

  constexpr std::array<std::uint8_t, 3U> label_color{96U, 96U, 192U};
  constexpr std::array<std::uint8_t, 3U> value_color{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> selected_color{208U, 208U, 255U};
  const auto panel =
      sample(positions, league_create_position_bank, 5U, transition_phase);
  const auto start =
      sample(positions, league_create_position_bank, 20U, transition_phase);
  const auto step =
      sample(positions, league_create_position_bank, 21U, transition_phase);
  const auto value_offset =
      sample(positions, league_create_position_bank, 23U, transition_phase);
  const auto label_x = panel.x + step.x;
  const auto first_y = panel.y + start.y + step.y;
  const auto row_step = std::max(step.y / 4, 1);

  draw_fnt_text(result, font, "Name:", label_x, first_y,
                selection == LeagueCreateField::name ? selected_color
                                                     : label_color);
  draw_fnt_text(result, font, "Division:", label_x, first_y + row_step * 2,
                selection == LeagueCreateField::division ? selected_color
                                                         : label_color);
  draw_fnt_text(result, font,
                draft.league_name.empty() ? "New League" : draft.league_name,
                panel.x + value_offset.x, first_y, value_color);
  draw_fnt_text(result, font, std::to_string(draft.division), label_x + 128,
                first_y + row_step * 2, value_color);
  return result;
}

FrontEndFrame compose_league_delete_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font,
    const std::span<const LeagueOverviewPresentation> leagues,
    const std::size_t selected_index, const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "Delete League background must be exactly 640x400 RGBA");
  }
  if (leagues.empty() || selected_index >= leagues.size()) {
    throw ToolError(ExitCode::usage,
                    "Delete League requires a valid selected league");
  }
  FrontEndFrame result;
  result.rgba = background.rgba;
  place_transition_sprite(result, menu_sprites, positions,
                          league_delete_position_bank, 1U, "dots",
                          transition_phase);
  place_transition_sprite(result, menu_sprites, positions,
                          league_delete_position_bank, 2U, "scrc",
                          transition_phase);
  place_transition_sprite(result, menu_sprites, positions,
                          league_delete_position_bank, 3U, "leagu",
                          transition_phase);
  place_transition_sprite(result, menu_sprites, positions,
                          league_delete_position_bank, 5U, "selco",
                          transition_phase);

  constexpr std::array<std::uint8_t, 3U> title_color{96U, 96U, 192U};
  constexpr std::array<std::uint8_t, 3U> normal_color{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> selected_color{208U, 208U, 255U};
  const auto title =
      sample(positions, league_delete_position_bank, 4U, transition_phase);
  draw_fnt_text(result, font, "Delete League", title.x, title.y, title_color);
  const auto frame =
      sample(positions, league_delete_position_bank, 5U, transition_phase);
  const auto offset =
      sample(positions, league_delete_position_bank, 21U, transition_phase);
  const auto step =
      sample(positions, league_delete_position_bank, 20U, transition_phase);
  const auto first = selected_index > 3U ? selected_index - 3U : 0U;
  const auto last = std::min(leagues.size(), selected_index + 4U);
  for (auto index = first; index < last; ++index) {
    const auto row = static_cast<std::int32_t>(index) -
                     static_cast<std::int32_t>(selected_index);
    draw_fnt_text(result, font, leagues[index].league_name, frame.x + offset.x,
                  frame.y + offset.y + row * std::max(step.y, 1),
                  index == selected_index ? selected_color : normal_color);
  }
  return result;
}

FrontEndFrame compose_ghost_setup_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font,
    const std::span<const std::string> demo_files,
    const GhostModeChoice selection, const bool file_focused,
    const std::size_t selected_file, const std::uint32_t elapsed_ms,
    const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "Ghost Mode background must be exactly 640x400 RGBA");
  }
  if (!demo_files.empty() && selected_file >= demo_files.size()) {
    throw ToolError(ExitCode::format,
                    "Ghost Mode selected demo index is out of range");
  }

  constexpr auto bank = ghost_setup_position_bank;
  FrontEndFrame result;
  result.rgba = background.rgba;
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(GhostSetupPositionRecord::background_dots), "dots",
      transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(GhostSetupPositionRecord::screen_badge), "scre",
      transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(GhostSetupPositionRecord::header), "ghost",
      transition_phase);

  const auto selected_frame = main_menu_selected_label_frame(elapsed_ms);
  const auto label_frame = [selection, file_focused,
                            selected_frame](const GhostModeChoice choice) {
    return !file_focused && selection == choice ? selected_frame : 0U;
  };
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(GhostSetupPositionRecord::ghost_race), "tgr",
      transition_phase, label_frame(GhostModeChoice::ghost_race));
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(GhostSetupPositionRecord::replay_race), "trpr",
      transition_phase, label_frame(GhostModeChoice::replay_race));
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(GhostSetupPositionRecord::benchmark_race), "tbr",
      transition_phase, label_frame(GhostModeChoice::benchmark_race));
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(GhostSetupPositionRecord::file_frame), "fghos",
      transition_phase, file_focused ? 1U : 0U);

  constexpr std::array<std::uint8_t, 3U> normal_color{96U, 96U, 192U};
  constexpr std::array<std::uint8_t, 3U> selected_color{208U, 208U, 255U};
  if (!demo_files.empty()) {
    const auto file_position =
        sample(positions, bank,
               position_record_index(GhostSetupPositionRecord::file_name),
               transition_phase);
    // The original passes 15 to its shared NUL-string list renderer. Record
    // 24 is the top-row anchor; the selected item remains on row 7 once the
    // list has scrolled far enough, and adjacent rows are exactly 15 logical
    // pixels apart.
    constexpr std::size_t visible_rows = 15U;
    constexpr std::int32_t row_step = 15;
    const auto window = centered_menu_list_window(demo_files.size(),
                                                  selected_file, visible_rows);
    for (auto index = window.first; index < window.last; ++index) {
      const auto row = static_cast<std::int32_t>(index - window.first);
      draw_fnt_text(result, font, demo_files[index], file_position.x,
                    file_position.y + row * row_step,
                    file_focused && index == selected_file ? selected_color
                                                           : normal_color);
    }

    const auto instruction_position = sample(
        positions, bank,
        position_record_index(GhostSetupPositionRecord::delete_instruction),
        transition_phase);
    draw_fnt_text(result, font, "Press: [Delete] To remove race file.",
                  instruction_position.x, instruction_position.y, normal_color);
  }

  if (selection == GhostModeChoice::benchmark_race) {
    const auto message_position = sample(
        positions, bank,
        position_record_index(GhostSetupPositionRecord::benchmark_message),
        transition_phase);
    draw_fnt_text(result, font, "Benchmark results will be saved in file:",
                  message_position.x, message_position.y, normal_color);
    draw_fnt_text(result, font, "'Fps.txt' in your Motorhead directory.",
                  message_position.x, message_position.y + 12, normal_color);
  }
  return result;
}

FrontEndFrame compose_multiplayer_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const MultiplayerPage page,
    const MultiplayerEntryChoice entry_choice,
    const MultiplayerJoinField join_field,
    const MultiplayerCreateField create_field,
    const MultiplayerLobbyField lobby_field,
    const MultiplayerConfiguration &configuration,
    const std::uint32_t elapsed_ms, const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "Multiplayer background must be exactly 640x400 RGBA");
  }

  constexpr auto bank = multiplayer_position_bank;
  const auto record = [](const MultiplayerPositionRecord value) {
    return position_record_index(value);
  };
  FrontEndFrame result;
  result.rgba = background.rgba;
  constexpr std::array<std::uint8_t, 3U> normal_color{96U, 96U, 192U};
  constexpr std::array<std::uint8_t, 3U> selected_color{208U, 208U, 255U};
  constexpr std::array<std::uint8_t, 3U> original_value_color{224U, 104U, 32U};
  if (page == MultiplayerPage::lobby) {
    // Client.tga/Server.tga are the original 640x400 motorm lobby canvases.
    // Their labels and F-key buttons are authored into the image; only the
    // runtime values, player rows, and chat/status line are drawn here.
    // motorm.exe sub_0008d044 calls the FONT1 renderer with these literal
    // coordinates. Values use 0xE06820; each player row uses PlayerColour.
    const auto value = [&](const std::string_view text, const int x,
                           const int y,
                           const std::array<std::uint8_t, 3U> color =
                               std::array<std::uint8_t, 3U>{224U, 104U, 32U}) {
      if (!text.empty()) {
        draw_fnt_text(result, font, text, x, y, color);
      }
    };
    value(configuration.session_name, 170, 28);
    value(configuration.session_password, 170, 40);
    value(configuration.lobby_track, 170, 52);
    value(std::to_string(configuration.lobby_laps), 170, 64);
    value(configuration.lobby_game_mode, 170, 76);
    const auto local = std::find_if(
        configuration.lobby_players.begin(), configuration.lobby_players.end(),
        [](const MultiplayerLobbyPlayer &player) { return player.local; });
    if (local != configuration.lobby_players.end()) {
      value(local->name, 450, 28);
      value(local->team, 450, 40);
    }
    value(configuration.lobby_gear, 450, 52);
    value(configuration.lobby_car.empty()
              ? std::string_view("-")
              : std::string_view(configuration.lobby_car),
          450, 64);
    std::vector<const MultiplayerLobbyPlayer *> ordered_players;
    ordered_players.reserve(configuration.lobby_players.size());
    for (const auto &player : configuration.lobby_players) {
      ordered_players.push_back(&player);
    }
    std::stable_sort(ordered_players.begin(), ordered_players.end(),
                     [](const MultiplayerLobbyPlayer *left,
                        const MultiplayerLobbyPlayer *right) {
                       const auto left_position = left->grid_position == 0U
                                                      ? 0xffU
                                                      : left->grid_position;
                       const auto right_position = right->grid_position == 0U
                                                       ? 0xffU
                                                       : right->grid_position;
                       return left_position < right_position;
                     });
    auto y = 160;
    for (const auto *player_pointer : ordered_players) {
      const auto &player = *player_pointer;
      const std::array<std::uint8_t, 3U> player_color{
          static_cast<std::uint8_t>((player.colour >> 16U) & 0xffU),
          static_cast<std::uint8_t>((player.colour >> 8U) & 0xffU),
          static_cast<std::uint8_t>(player.colour & 0xffU)};
      value(std::to_string(static_cast<unsigned>(player.grid_position)) + ".",
            30, y, player_color);
      value(player.ready ? "R" : "", 50, y + 2, player_color);
      value(player.name, 75, y, player_color);
      value(player.car_name, 220, y, player_color);
      value(player.automatic_gear ? "A" : "M", 305, y, player_color);
      value(player.team, 340, y, player_color);
      value(std::to_string(player.races), 400, y, player_color);
      value(std::to_string(player.score), 460, y, player_color);
      value(std::to_string(static_cast<unsigned>(player.lap)), 540, y,
            player_color);
      value(std::to_string(player.ping_ms), 580, y, player_color);
      y += 10;
    }
    if (configuration.lobby_chat_lines.empty()) {
      value(configuration.status, 50, 300);
    } else {
      constexpr std::size_t visible_lines = 5U;
      const auto first =
          configuration.lobby_chat_lines.size() > visible_lines
              ? configuration.lobby_chat_lines.size() - visible_lines
              : 0U;
      auto chat_y = 300;
      for (std::size_t index = first;
           index < configuration.lobby_chat_lines.size(); ++index) {
        const auto &line = configuration.lobby_chat_lines[index];
        const std::array<std::uint8_t, 3U> line_color{
            static_cast<std::uint8_t>((line.colour >> 16U) & 0xffU),
            static_cast<std::uint8_t>((line.colour >> 8U) & 0xffU),
            static_cast<std::uint8_t>(line.colour & 0xffU)};
        value(line.text, 50, chat_y, line_color);
        chat_y += 12;
      }
    }
    auto chat_input = configuration.lobby_chat_input;
    if ((elapsed_ms / 500U) % 2U == 0U) {
      chat_input.push_back('_');
    }
    value(chat_input, 50, 370);
    return result;
  }
  place_transition_sprite(result, menu_sprites, positions, bank,
                          record(MultiplayerPositionRecord::background_dots),
                          "dots", transition_phase);
  place_transition_sprite(result, menu_sprites, positions, bank,
                          record(MultiplayerPositionRecord::screen_badge),
                          "scrb", transition_phase);
  place_transition_sprite(result, menu_sprites, positions, bank,
                          record(MultiplayerPositionRecord::header), "multi",
                          transition_phase);

  const auto selected_frame = main_menu_selected_label_frame(elapsed_ms);
  const auto value_offset = sample(positions, bank, 24U, transition_phase);
  const auto draw_value = [&](const MultiplayerPositionRecord anchor,
                              const std::string_view value,
                              const bool selected) {
    const auto position =
        sample(positions, bank, record(anchor), transition_phase);
    draw_fnt_text(result, font, value, position.x + value_offset.x,
                  position.y + value_offset.y,
                  selected ? selected_color : normal_color);
  };

  if (page == MultiplayerPage::entry) {
    place_transition_sprite(
        result, menu_sprites, positions, bank,
        record(MultiplayerPositionRecord::join), "tjoin", transition_phase,
        entry_choice == MultiplayerEntryChoice::join ? selected_frame : 0U);
    place_transition_sprite(
        result, menu_sprites, positions, bank,
        record(MultiplayerPositionRecord::create), "tcrea", transition_phase,
        entry_choice == MultiplayerEntryChoice::create ? selected_frame : 0U);
    return result;
  }

  if (page == MultiplayerPage::join) {
    place_transition_sprite(
        result, menu_sprites, positions, bank,
        record(MultiplayerPositionRecord::session_list_frame), "flsea",
        transition_phase,
        join_field == MultiplayerJoinField::sessions ? 1U : 0U);
    place_transition_sprite(result, menu_sprites, positions, bank,
                            record(MultiplayerPositionRecord::address_frame),
                            "faddr", transition_phase,
                            join_field == MultiplayerJoinField::address ? 1U
                                                                        : 0U);
    const auto sessions = sample(
        positions, bank, record(MultiplayerPositionRecord::session_list_frame),
        transition_phase);
    const auto session_text = [&configuration]() {
      if (configuration.sessions.empty()) {
        return std::string("No sessions found");
      }
      const auto index = std::min<std::size_t>(
          configuration.selected_session, configuration.sessions.size() - 1U);
      const auto &session = configuration.sessions[index];
      return session.name + "  " + std::to_string(session.players) + "/" +
             std::to_string(session.capacity) +
             (session.password_required ? "  [password]" : "");
    }();
    draw_fnt_text(result, font, session_text, sessions.x + 8, sessions.y + 28,
                  join_field == MultiplayerJoinField::sessions ? selected_color
                                                               : normal_color);
    const auto address = sample(
        positions, bank, record(MultiplayerPositionRecord::address_frame),
        transition_phase);
    draw_fnt_text(result, font, configuration.address, address.x + 145,
                  address.y + 3,
                  join_field == MultiplayerJoinField::address ? selected_color
                                                              : normal_color);
    if (join_field == MultiplayerJoinField::password) {
      // motorm.exe sub_0008d044 owns this requester independently of the
      // Address book row. It draws both literal lines and the input buffer at
      // these exact logical coordinates using 0xE06820.
      draw_fnt_text(result, font, "This session require a password!", 150, 310,
                    original_value_color);
      draw_fnt_text(result, font, "Type password:", 150, 330,
                    original_value_color);
      draw_fnt_text(result, font, configuration.session_password, 290, 330,
                    original_value_color);
      return result;
    }
    const auto footer = sample(positions, bank, 22U, transition_phase);
    draw_fnt_text(result, font,
                  join_field == MultiplayerJoinField::refresh
                      ? "Refresh sessions"
                      : "Return joins - Esc goes back",
                  footer.x, footer.y,
                  join_field == MultiplayerJoinField::refresh ? selected_color
                                                              : normal_color);
    return result;
  }

  if (page == MultiplayerPage::lobby) {
    const auto origin = sample(
        positions, bank, record(MultiplayerPositionRecord::session_list_frame),
        transition_phase);
    draw_fnt_text(result, font, "MULTIPLAYER LOBBY", origin.x + 8, origin.y + 4,
                  selected_color);
    auto row_y = origin.y + 24;
    for (const auto &player : configuration.lobby_players) {
      const auto line =
          std::string(player.host ? "H " : "  ") + player.name + "   CAR " +
          std::to_string(static_cast<unsigned>(player.car_selection + 1U)) +
          (player.ready ? "   READY" : "   WAITING");
      draw_fnt_text(result, font, line, origin.x + 8, row_y,
                    player.local ? selected_color : normal_color);
      row_y += 13;
    }
    const auto footer = sample(positions, bank, 22U, transition_phase);
    draw_fnt_text(
        result, font,
        std::string(lobby_field == MultiplayerLobbyField::ready ? "> " : "  ") +
            (configuration.local_ready ? "Not ready" : "Ready"),
        footer.x, footer.y - 14,
        lobby_field == MultiplayerLobbyField::ready ? selected_color
                                                    : normal_color);
    if (configuration.lobby_host) {
      draw_fnt_text(result, font,
                    std::string(lobby_field == MultiplayerLobbyField::start
                                    ? "> "
                                    : "  ") +
                        "Start race",
                    footer.x + 150, footer.y - 14,
                    lobby_field == MultiplayerLobbyField::start ? selected_color
                                                                : normal_color);
    }
    draw_fnt_text(result, font,
                  configuration.status.empty()
                      ? std::string_view("Esc leaves session")
                      : std::string_view(configuration.status),
                  footer.x, footer.y, normal_color);
    return result;
  }

  const auto label_frame =
      [create_field, selected_frame](const MultiplayerCreateField field) {
        return create_field == field ? selected_frame : 0U;
      };
  place_transition_sprite(result, menu_sprites, positions, bank,
                          record(MultiplayerPositionRecord::start), "tmsta",
                          transition_phase,
                          label_frame(MultiplayerCreateField::start));
  place_transition_sprite(result, menu_sprites, positions, bank,
                          record(MultiplayerPositionRecord::session_name),
                          "tmssn", transition_phase,
                          label_frame(MultiplayerCreateField::session_name));
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      record(MultiplayerPositionRecord::session_password), "tmssp",
      transition_phase, label_frame(MultiplayerCreateField::session_password));
  place_transition_sprite(result, menu_sprites, positions, bank,
                          record(MultiplayerPositionRecord::protocol), "tmpro",
                          transition_phase,
                          label_frame(MultiplayerCreateField::protocol));
  auto session_name = configuration.session_name;
  if (create_field == MultiplayerCreateField::session_name &&
      (elapsed_ms / 500U) % 2U == 0U) {
    session_name.push_back('_');
  }
  auto session_password = configuration.session_password.empty()
                              ? std::string("<none>")
                              : configuration.session_password;
  if (create_field == MultiplayerCreateField::session_password &&
      (elapsed_ms / 500U) % 2U == 0U) {
    session_password.push_back('_');
  }
  draw_value(MultiplayerPositionRecord::session_name, session_name,
             create_field == MultiplayerCreateField::session_name);
  draw_value(MultiplayerPositionRecord::session_password, session_password,
             create_field == MultiplayerCreateField::session_password);
  draw_value(MultiplayerPositionRecord::protocol, configuration.protocol,
             create_field == MultiplayerCreateField::protocol);
  const auto footer = sample(positions, bank, 22U, transition_phase);
  draw_fnt_text(result, font,
                create_field == MultiplayerCreateField::start
                    ? "Press Return to host session"
                    : "Type to edit - Backspace deletes - Esc goes back",
                footer.x, footer.y,
                create_field == MultiplayerCreateField::start ? selected_color
                                                              : normal_color);
  return result;
}

FrontEndFrame compose_race_setup_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font,
    const mh::content::TgaImage &track_preview,
    const mh::content::LobData &track_line_object,
    const double track_line_object_scale,
    const RaceSetupPresentation &presentation, const RaceSetupMode mode,
    const RaceSetupField selection, const std::uint32_t laps,
    const bool catch_up, const std::uint32_t elapsed_ms,
    const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "race-setup background must be exactly 640x400 RGBA");
  }
  if (track_preview.rgba.size() !=
      static_cast<std::size_t>(track_preview.width) * track_preview.height *
          4U) {
    throw ToolError(ExitCode::format,
                    "race-setup preview has inconsistent RGBA storage");
  }

  constexpr auto bank = race_setup_position_bank;
  FrontEndFrame result;
  result.rgba = background.rgba;

  // The retail track loader supplies a 324x270 _menuimg.TGA beside the
  // 326x275 trsel oval. Recover the oval interior from trsel's own alpha
  // outline, clip the image to it, and then place the authored frame over it.
  const auto preview_position =
      sample(positions, bank,
             position_record_index(RaceSetupPositionRecord::preview_frame),
             transition_phase);
  const auto &preview_frame = decode_required(menu_sprites, "trsel");
  blit_rgba_inside_outline(result, track_preview.rgba, track_preview.width,
                           track_preview.height, preview_frame.rgba,
                           preview_frame.width, preview_frame.height,
                           preview_position.x, preview_position.y);

  draw_track_setup_line_object(result, track_line_object,
                               track_line_object_scale, positions, elapsed_ms,
                               transition_phase);

  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RaceSetupPositionRecord::background_dots), "dots",
      transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RaceSetupPositionRecord::header), "track",
      transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RaceSetupPositionRecord::screen_badge), "scra",
      transition_phase, 0U, -1, 0, race_setup_screen_badge_offset_y);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RaceSetupPositionRecord::preview_frame), "trsel",
      transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RaceSetupPositionRecord::option_frame), "carop",
      transition_phase);

  const auto selected_frame = main_menu_selected_label_frame(elapsed_ms);
  const auto label_frame = [selection,
                            selected_frame](const RaceSetupField field) {
    return selection == field ? selected_frame : 0U;
  };
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RaceSetupPositionRecord::track_label), "ttrac",
      transition_phase, label_frame(RaceSetupField::track));
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RaceSetupPositionRecord::laps_label), "tlaps",
      transition_phase, label_frame(RaceSetupField::laps));
  if (mode == RaceSetupMode::single_race) {
    place_transition_sprite(
        result, menu_sprites, positions, bank,
        position_record_index(RaceSetupPositionRecord::catch_up_label), "tcat",
        transition_phase, label_frame(RaceSetupField::catch_up));
    place_transition_sprite(
        result, menu_sprites, positions, bank,
        position_record_index(RaceSetupPositionRecord::catch_up_value), "onoff",
        transition_phase, catch_up ? 0U : 7U);
    place_transition_sprite(
        result, menu_sprites, positions, bank,
        position_record_index(RaceSetupPositionRecord::catch_up_frame), "caro2",
        transition_phase);
  }

  constexpr std::array<std::uint8_t, 3U> value_color{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> locked_color{128U, 80U, 64U};
  const auto track_position =
      sample(positions, bank,
             position_record_index(RaceSetupPositionRecord::track_name),
             transition_phase);
  const auto track_width =
      mh::content::measure_fnt_text(font, presentation.track_name);
  const auto track_x = static_cast<std::uint32_t>(std::max(
      track_position.x - static_cast<std::int32_t>(track_width / 2U), 0));
  const auto track_y =
      static_cast<std::uint32_t>(std::max(track_position.y, 0));
  draw_fnt_text(result, font, presentation.track_name, track_x, track_y,
                presentation.track_unlocked ? value_color : locked_color);

  // p3.1 uses font codes 0x87 and 0x88 for the bouncing previous/next
  // arrows. Visibility alternates on opposite halves of the low timer byte.
  const auto timer_byte = elapsed_ms & 0xffU;
  if (presentation.has_previous_track && timer_byte < 0x80U) {
    const std::string arrow(1U, static_cast<char>(0x87));
    const auto arrow_width = mh::content::measure_fnt_text(font, arrow);
    const auto x = static_cast<std::uint32_t>(std::max(
        track_position.x - static_cast<std::int32_t>(track_width / 2U) -
            static_cast<std::int32_t>(arrow_width) - 8,
        0));
    draw_fnt_text(result, font, arrow, x, track_y, value_color);
  }
  if (presentation.has_next_track && timer_byte > 0x80U) {
    const std::string arrow(1U, static_cast<char>(0x88));
    const auto x = static_cast<std::uint32_t>(std::max(
        track_position.x + static_cast<std::int32_t>(track_width / 2U) + 8, 0));
    draw_fnt_text(result, font, arrow, x, track_y, value_color);
  }

  const auto laps_position =
      sample(positions, bank,
             position_record_index(RaceSetupPositionRecord::laps_value),
             transition_phase);
  // The original two-column number owner right-aligns 19-pixel NUMA glyphs
  // in 24-pixel cells, starting six pixels inside record 18.
  place_numa_digit(result, menu_sprites, (laps / 10U) % 10U,
                   laps_position.x + 6, laps_position.y);
  place_numa_digit(result, menu_sprites, laps % 10U, laps_position.x + 30,
                   laps_position.y);

  if (!presentation.track_unlocked && (elapsed_ms & 0x3ffU) < 0x200U) {
    constexpr std::string_view unavailable = "Not yet available";
    const auto locked_position =
        sample(positions, bank,
               position_record_index(RaceSetupPositionRecord::locked_label),
               transition_phase);
    const auto width = mh::content::measure_fnt_text(font, unavailable);
    const auto x = static_cast<std::uint32_t>(
        std::max(locked_position.x - static_cast<std::int32_t>(width / 2U), 0));
    draw_fnt_text(result, font, unavailable, x,
                  static_cast<std::uint32_t>(std::max(locked_position.y, 0)),
                  locked_color);
  }
  return result;
}

FrontEndFrame compose_car_setup_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const mh::content::TgaImage &car_preview,
    const mh::content::LobData &car_line_object,
    const double car_line_object_scale,
    const CarSetupPresentation &presentation, const CarSetupField selection,
    const bool automatic_transmission, const bool record_race,
    const std::uint32_t elapsed_ms, const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "car-setup background must be exactly 640x400 RGBA");
  }
  if (car_preview.rgba.size() !=
      static_cast<std::size_t>(car_preview.width) * car_preview.height * 4U) {
    throw ToolError(ExitCode::format,
                    "car-setup preview has inconsistent RGBA storage");
  }

  constexpr auto bank = car_setup_position_bank;
  FrontEndFrame result;
  result.rgba = background.rgba;
  const auto preview_position =
      sample(positions, bank,
             position_record_index(CarSetupPositionRecord::preview_frame),
             transition_phase);
  // The retail car loader supplies a 324x270 _menuimg.TGA inside the authored
  // 326x270 carc oval. Derive the visible interior from carc's alpha ring so
  // preview pixels cannot escape through the transparent frame corners.
  const auto &preview_frame = decode_required(menu_sprites, "carc");
  blit_rgba_inside_outline(result, car_preview.rgba, car_preview.width,
                           car_preview.height, preview_frame.rgba,
                           preview_frame.width, preview_frame.height,
                           preview_position.x, preview_position.y);

  draw_car_setup_line_object(result, car_line_object, car_line_object_scale,
                             positions, elapsed_ms, transition_phase);

  constexpr std::array<std::pair<CarSetupPositionRecord, std::string_view>, 9U>
      base_sprites{{
          {CarSetupPositionRecord::background_dots, "dots"},
          {CarSetupPositionRecord::header, "car"},
          {CarSetupPositionRecord::screen_badge, "scr8"},
          {CarSetupPositionRecord::preview_frame, "carc"},
          {CarSetupPositionRecord::stat_swoosh, "cars"},
          {CarSetupPositionRecord::stat_speed, "carx"},
          {CarSetupPositionRecord::stat_acceleration, "cary"},
          {CarSetupPositionRecord::stat_grip, "carz"},
          {CarSetupPositionRecord::option_frame, "carop"},
      }};
  for (const auto &[record, sprite] : base_sprites) {
    const auto offset_y = record == CarSetupPositionRecord::screen_badge
                              ? car_setup_screen_badge_offset_y
                              : 0;
    place_transition_sprite(result, menu_sprites, positions, bank,
                            position_record_index(record), sprite,
                            transition_phase, 0U, -1, 0, offset_y);
  }

  const auto selected_frame = main_menu_selected_label_frame(elapsed_ms);
  const auto label_frame = [selection,
                            selected_frame](const CarSetupField field) {
    return selection == field ? selected_frame : 0U;
  };
  constexpr std::array<
      std::tuple<CarSetupPositionRecord, std::string_view, CarSetupField>, 4U>
      option_labels{{
          {CarSetupPositionRecord::car_label, "tcar", CarSetupField::car},
          {CarSetupPositionRecord::transmission_label, "ttran",
           CarSetupField::transmission},
          {CarSetupPositionRecord::horn_label, "thorn", CarSetupField::horn},
          {CarSetupPositionRecord::record_race_label, "trr",
           CarSetupField::record_race},
      }};
  for (const auto &[record, sprite, field] : option_labels) {
    place_transition_sprite(result, menu_sprites, positions, bank,
                            position_record_index(record), sprite,
                            transition_phase, label_frame(field));
  }

  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(CarSetupPositionRecord::transmission_value), "tman",
      transition_phase, automatic_transmission ? 7U : 0U);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(CarSetupPositionRecord::record_race_value), "onoff",
      transition_phase, record_race ? 0U : 7U);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(CarSetupPositionRecord::car_information_frame),
      "card", transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(CarSetupPositionRecord::option_value_frame),
      "caro2", transition_phase);

  constexpr std::array<std::uint8_t, 3U> value_color{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> locked_color{128U, 80U, 64U};
  constexpr std::uint8_t car_name_opacity = 176U;
  constexpr std::int32_t car_name_arrow_gap = 8;
  const auto name_position = sample(
      positions, bank, position_record_index(CarSetupPositionRecord::car_name),
      transition_phase);
  const auto name_center_x =
      preview_position.x + static_cast<std::int32_t>(preview_frame.width / 2U);
  const auto name_width =
      mh::content::measure_fnt_text(font, presentation.car_name);
  const auto name_y = static_cast<std::uint32_t>(std::max(name_position.y, 0));
  const auto timer_byte = elapsed_ms & 0xffU;
  const auto show_previous =
      presentation.has_previous_car && timer_byte < 0x80U;
  const auto show_next = presentation.has_next_car && timer_byte > 0x80U;
  const auto name_x =
      name_center_x - static_cast<std::int32_t>(name_width / 2U);
  if (show_previous) {
    const std::string arrow(1U, static_cast<char>(0x87));
    const auto arrow_width = mh::content::measure_fnt_text(font, arrow);
    const auto arrow_x =
        name_x - static_cast<std::int32_t>(arrow_width) - car_name_arrow_gap;
    draw_fnt_text(result, font, arrow, arrow_x, name_y, value_color,
                  car_name_opacity);
  }
  // The name owns the oval's center independently of the alternating arrow.
  // Only the arrow pixels blink; changing sides must never shift the label.
  draw_fnt_text(result, font, presentation.car_name, name_x, name_y,
                presentation.car_unlocked ? value_color : locked_color,
                car_name_opacity);
  if (show_next) {
    const std::string arrow(1U, static_cast<char>(0x88));
    const auto arrow_x =
        name_x + static_cast<std::int32_t>(name_width) + car_name_arrow_gap;
    draw_fnt_text(result, font, arrow, arrow_x, name_y, value_color,
                  car_name_opacity);
  }

  const auto horn_position =
      sample(positions, bank,
             position_record_index(CarSetupPositionRecord::horn_value),
             transition_phase);
  draw_fnt_text(result, font, presentation.horn_name,
                static_cast<std::uint32_t>(std::max(horn_position.x, 0)),
                static_cast<std::uint32_t>(std::max(horn_position.y, 0)),
                value_color);

  if (!presentation.car_unlocked && (elapsed_ms & 0x3ffU) < 0x200U) {
    constexpr std::string_view unavailable = "Not yet available";
    const auto locked_position =
        sample(positions, bank,
               position_record_index(CarSetupPositionRecord::preview_image),
               transition_phase);
    const auto width = mh::content::measure_fnt_text(font, unavailable);
    const auto x = static_cast<std::uint32_t>(
        std::max(locked_position.x - static_cast<std::int32_t>(width / 2U), 0));
    draw_fnt_text(result, font, unavailable, x,
                  static_cast<std::uint32_t>(std::max(locked_position.y, 0)),
                  locked_color);
  }

  // p3.1 RVA 0x8ce63..0x8d07a normalizes the authored TopSpeed,
  // Acceleration, and Handling fields and masks the three authored gauges with
  // eight opaque lavender pie segments through RVA 0xa69a0. Those segments
  // cover unused portions while the green/orange authored arcs remain visible.
  const auto normalize = [](const float value, const double minimum,
                            const double maximum) {
    return std::clamp(
        (static_cast<double>(value) - minimum) / (maximum - minimum), 0.0, 1.0);
  };
  const auto speed =
      normalize(presentation.performance.top_speed, 250.0, 320.0);
  const auto acceleration =
      normalize(presentation.performance.acceleration, 2.2, 3.2);
  const auto grip = normalize(presentation.performance.handling, 3.5, 8.5);
  mask_car_performance_gauge(result, positions,
                             CarSetupPositionRecord::stat_speed, 49, 44, 41.5,
                             speed, transition_phase);
  mask_car_performance_gauge(result, positions,
                             CarSetupPositionRecord::stat_acceleration, 42, 35,
                             35.5, 1.0 - acceleration, transition_phase);
  mask_car_performance_gauge(result, positions,
                             CarSetupPositionRecord::stat_grip, 32, 30, 25.5,
                             grip, transition_phase);

  // p3.1 RVA 0x8d07f submits authored records 14..16 only after the polygon
  // masks, so the S/A/G centre labels remain visible over the filled sectors.
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(CarSetupPositionRecord::speed_label), "cas",
      transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(CarSetupPositionRecord::acceleration_label), "caa",
      transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(CarSetupPositionRecord::grip_label), "cag",
      transition_phase);
  return result;
}

void overlay_race_setup_display_badge(
    FrontEndFrame &display, const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const std::uint32_t transition_phase) {
  constexpr auto record = RaceSetupPositionRecord::screen_badge;
  const auto &frame = decode_required(menu_sprites, "scra");
  const auto position = sample(positions, race_setup_position_bank,
                               position_record_index(record), transition_phase);
  place_transition_frame(display, frame, position, race_setup_position_bank,
                         position_record_index(record), transition_phase, -1, 0,
                         race_setup_screen_badge_offset_y);
}

void overlay_car_setup_display_badge(
    FrontEndFrame &display, const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const std::uint32_t transition_phase) {
  constexpr auto record = CarSetupPositionRecord::screen_badge;
  const auto &frame = decode_required(menu_sprites, "scr8");
  const auto position = sample(positions, car_setup_position_bank,
                               position_record_index(record), transition_phase);
  place_transition_frame(display, frame, position, car_setup_position_bank,
                         position_record_index(record), transition_phase, -1, 0,
                         car_setup_screen_badge_offset_y);
}

FrontEndFrame
compose_graphic_options_frame(const mh::content::TgaImage &background,
                              const mh::content::SprArchive &menu_sprites,
                              const mh::content::SprPositionData &positions,
                              const mh::content::FntData &font,
                              const GraphicOptionsField selection,
                              const GraphicOptionsConfiguration &configuration,
                              const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "Graphic Options background must be exactly 640x400 RGBA");
  }

  constexpr std::size_t graphic_options_bank = 5U;
  FrontEndFrame result;
  result.rgba = background.rgba;
  // Bank 5 records 1/2/3 are authored transition modes 7/3/5. Their
  // coordinates were already interpolated, but the previous compositor
  // skipped the corresponding retail raster effects.
  place_transition_sprite(result, menu_sprites, positions, graphic_options_bank,
                          1U, "dots", transition_phase);
  place_transition_sprite(result, menu_sprites, positions, graphic_options_bank,
                          2U, "opt", transition_phase);
  place_transition_sprite(result, menu_sprites, positions, graphic_options_bank,
                          3U, "scr4", transition_phase);

  const auto panel =
      sample(positions, graphic_options_bank, 4U, transition_phase);
  // p3.1 RVA 0x8607b..0x860a6 samples bank-5 record 24's Y as the
  // generated-row stride. Generated FNT rows expand from record 4 during
  // entry; they are not submitted to sprite dispatcher RVA 0x7e51c.
  const auto row_layout =
      sample(positions, graphic_options_bank, 24U, transition_phase);
  const auto origin_x = static_cast<std::uint32_t>(std::max(panel.x, 0));
  const auto origin_y = static_cast<std::uint32_t>(std::max(panel.y, 0));
  const auto row_step = static_cast<std::uint32_t>(std::max(row_layout.y, 0));
  // Leave enough space below Z Read to preview all three accessibility rows.
  constexpr std::uint32_t pinned_selection_row = 22U;
  constexpr std::uint32_t last_visible_row = 23U;
  const auto display_row = [&](const std::size_t field_index) {
    if (configuration.detail_mode != GraphicDetailMode::custom &&
        field_index ==
            static_cast<std::size_t>(GraphicOptionsField::motion_blur)) {
      return 10U;
    }
    return static_cast<std::uint32_t>(field_index);
  };
  const auto selected_display_row =
      display_row(static_cast<std::size_t>(selection));
  const auto scroll_rows = selected_display_row > pinned_selection_row
                               ? selected_display_row - pinned_selection_row
                               : 0U;
  const auto row_visible = [&](const std::uint32_t row) {
    return row >= scroll_rows && row <= scroll_rows + last_visible_row;
  };
  const auto row_y = [&](const std::uint32_t row) {
    if (row < scroll_rows) {
      return 0U;
    }
    return origin_y + (row - scroll_rows) * row_step;
  };
  constexpr std::array<std::uint8_t, 3U> label_color{96U, 96U, 192U};
  constexpr std::array<std::uint8_t, 3U> value_color{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> white{255U, 255U, 255U};
  constexpr std::array<std::uint8_t, 3U> black{0U, 0U, 0U};

  constexpr std::array<std::string_view, 24U> labels{
      "Render device:",
      "Screen size:",
      "Aspect ratio:",
      "Window mode:",
      "True Colour rendering:",
      "Triple buffer:",
      "Trilinear filtering:",
      "Texture format:",
      "Brightness:",
      "Graphic detail:",
      "Lensflares:",
      "Sparks:",
      "Smoke:",
      "Halos:",
      "Shadows:",
      "Skidmarks:",
      "Nameplates:",
      "Background:",
      "Trackdetail:",
      "Cardetail:",
      "Carshading:",
      "View distance:",
      "Z Read:",
      "Motion blur:",
  };
  const auto selected_index = static_cast<std::size_t>(selection);
  const auto draw_label = [&](const std::size_t index) {
    const auto row = display_row(index);
    if (!row_visible(row)) {
      return;
    }
    draw_fnt_text(result, font, labels[index], origin_x,
                  row_y(row),
                  index == selected_index ? white : label_color);
  };
  const auto custom_label_count = labels.size();
  const auto primary_label_count =
      configuration.detail_mode == GraphicDetailMode::custom
          ? custom_label_count
          : static_cast<std::size_t>(GraphicOptionsField::lens_flares);
  for (std::size_t index = 0U; index < primary_label_count; ++index) {
    draw_label(index);
  }
  if (configuration.detail_mode != GraphicDetailMode::custom) {
    draw_label(static_cast<std::size_t>(GraphicOptionsField::motion_blur));
  }

  const auto value_x = origin_x + 208U;
  const auto draw_choice =
      [&](const GraphicOptionsField field, const std::string_view text,
          const std::uint32_t x, const std::uint32_t y, const bool active) {
        const auto visible_bottom = origin_y + last_visible_row * row_step;
        if (y < origin_y || y > visible_bottom) {
          return;
        }
        const auto focused = selection == field && active;
        if (focused) {
          const auto text_width = mh::content::measure_fnt_text(font, text);
          fill_rectangle(result, x - 2U, y - 2U, text_width + 4U, 14U, white);
        }
        draw_fnt_text(result, font, text, x, y, focused ? black : value_color);
      };
  const auto draw_boolean = [&](const GraphicOptionsField field,
                                const std::uint32_t row, const bool value) {
    draw_choice(field, "Off", value_x, row_y(row), !value);
    draw_choice(field, "On", value_x + 60U, row_y(row), value);
  };
  const auto draw_percentage_bar =
      [&](const GraphicOptionsField field, const std::uint32_t row,
          const float value, const float minimum, const float maximum) {
        if (!row_visible(row)) {
          return;
        }
        const auto percent =
            static_cast<std::uint32_t>(std::lround(static_cast<double>(value)));
        draw_fnt_text(result, font, std::to_string(percent) + "%", value_x,
                      row_y(row), selection == field ? white : value_color);
        constexpr std::uint32_t bar_x_offset = 55U;
        constexpr std::uint32_t bar_width = 191U;
        constexpr std::uint32_t marker_range = 188U;
        fill_rectangle(result, value_x + bar_x_offset, row_y(row) + 7U,
                       bar_width, 2U, value_color);
        const auto fraction =
            std::clamp((value - minimum) / (maximum - minimum), 0.0F, 1.0F);
        const auto marker = static_cast<std::uint32_t>(
            std::lround(static_cast<double>(fraction) * marker_range));
        fill_rectangle(result, value_x + bar_x_offset + marker,
                       row_y(row) + 4U, 3U, 8U,
                       selection == field ? white : value_color);
      };

  constexpr std::array<GraphicRendererBackend, 6U> renderer_choices{
      GraphicRendererBackend::automatic, GraphicRendererBackend::d3d9,
      GraphicRendererBackend::d3d11, GraphicRendererBackend::d3d12,
      GraphicRendererBackend::glide, GraphicRendererBackend::software};
  const auto selected_renderer = static_cast<std::size_t>(
      std::find(renderer_choices.begin(), renderer_choices.end(),
                configuration.renderer_backend) -
      renderer_choices.begin());
  constexpr std::size_t visible_renderers = 3U;
  const auto first_renderer = std::clamp<std::size_t>(
      selected_renderer == 0U ? 0U : selected_renderer - 1U, 0U,
      renderer_choices.size() - visible_renderers);
  const std::array<std::uint32_t, visible_renderers> renderer_x{
      value_x, value_x + 92U, value_x + 190U};
  for (std::size_t visible = 0U; visible < visible_renderers; ++visible) {
    const auto backend = renderer_choices[first_renderer + visible];
    draw_choice(GraphicOptionsField::render_device,
                graphic_renderer_backend_label(backend), renderer_x[visible],
                row_y(0U), configuration.renderer_backend == backend);
  }
  constexpr std::size_t visible_screen_sizes = 3U;
  constexpr std::uint32_t screen_size_spacing = 98U;
  const auto selected_screen_size_tier =
      graphic_screen_size_tier(configuration.screen_size);
  const auto first_screen_size_tier = std::clamp<std::size_t>(
      selected_screen_size_tier == 0U ? 0U : selected_screen_size_tier - 1U, 0U,
      5U - visible_screen_sizes);
  for (std::size_t visible = 0U; visible < visible_screen_sizes; ++visible) {
    const auto tier = first_screen_size_tier + visible;
    const auto size =
        graphic_screen_size_for_aspect(configuration.aspect_ratio, tier);
    const auto dimensions = graphic_screen_dimensions(size);
    const auto text =
        std::to_string(dimensions[0U]) + "X" + std::to_string(dimensions[1U]);
    draw_choice(GraphicOptionsField::screen_size, text,
                value_x +
                    static_cast<std::uint32_t>(visible) * screen_size_spacing,
                row_y(1U), configuration.screen_size == size);
  }
  draw_choice(GraphicOptionsField::aspect_ratio, "4:3", value_x, row_y(2U),
              configuration.aspect_ratio == GraphicAspectRatio::classic_4_3);
  draw_choice(
      GraphicOptionsField::aspect_ratio, "16:9", value_x + 60U, row_y(2U),
      configuration.aspect_ratio == GraphicAspectRatio::widescreen_16_9);
  draw_choice(GraphicOptionsField::window_mode, "Windowed", value_x, row_y(3U),
              configuration.window_mode == GraphicWindowMode::windowed);
  draw_choice(GraphicOptionsField::window_mode, "Borderless", value_x + 90U,
              row_y(3U),
              configuration.window_mode == GraphicWindowMode::borderless);
  draw_choice(GraphicOptionsField::window_mode, "Fullscreen", value_x + 190U,
              row_y(3U),
              configuration.window_mode == GraphicWindowMode::fullscreen);
  draw_boolean(GraphicOptionsField::true_colour, 4U, configuration.true_colour);
  draw_boolean(GraphicOptionsField::triple_buffer, 5U,
               configuration.triple_buffer);
  draw_boolean(GraphicOptionsField::trilinear_filtering, 6U,
               configuration.trilinear_filtering);
  draw_choice(
      GraphicOptionsField::texture_format, "256 colours", value_x, row_y(7U),
      configuration.texture_format == GraphicTextureFormat::indexed_256);
  draw_choice(GraphicOptionsField::texture_format, "HiColour", value_x + 100U,
              row_y(7U),
              configuration.texture_format == GraphicTextureFormat::hi_colour);
  draw_choice(GraphicOptionsField::texture_format, "TrueColour", value_x + 195U,
              row_y(7U),
              configuration.texture_format ==
                  GraphicTextureFormat::true_colour);
  draw_percentage_bar(GraphicOptionsField::brightness, 8U,
                      configuration.brightness * 100.0F, 0.0F, 200.0F);

  draw_choice(GraphicOptionsField::graphic_detail, "Low", value_x, row_y(9U),
              configuration.detail_mode == GraphicDetailMode::low);
  draw_choice(GraphicOptionsField::graphic_detail, "Medium", value_x + 60U,
              row_y(9U),
              configuration.detail_mode == GraphicDetailMode::medium);
  draw_choice(GraphicOptionsField::graphic_detail, "High", value_x + 143U,
              row_y(9U), configuration.detail_mode == GraphicDetailMode::high);
  draw_choice(GraphicOptionsField::graphic_detail, "Max", value_x + 196U,
              row_y(9U),
              configuration.detail_mode == GraphicDetailMode::maximum);
  draw_choice(GraphicOptionsField::graphic_detail, "Custom", value_x + 246U,
              row_y(9U),
              configuration.detail_mode == GraphicDetailMode::custom);
  if (configuration.detail_mode == GraphicDetailMode::custom) {
    draw_boolean(GraphicOptionsField::lens_flares, 10U,
                 configuration.lens_flares);
    draw_boolean(GraphicOptionsField::sparks, 11U, configuration.sparks);
    draw_boolean(GraphicOptionsField::smoke, 12U, configuration.smoke);
    draw_boolean(GraphicOptionsField::halos, 13U, configuration.halos);
    draw_boolean(GraphicOptionsField::shadows, 14U, configuration.shadows);
    draw_boolean(GraphicOptionsField::skid_marks, 15U,
                 configuration.skid_marks);
    draw_choice(GraphicOptionsField::name_plates, "None", value_x, row_y(16U),
                configuration.name_plates == GraphicNamePlateMode::none);
    draw_choice(GraphicOptionsField::name_plates, "Flat", value_x + 60U,
                row_y(16U),
                configuration.name_plates == GraphicNamePlateMode::flat);
    draw_choice(
        GraphicOptionsField::name_plates, "Transparent", value_x + 110U,
        row_y(16U),
        configuration.name_plates == GraphicNamePlateMode::transparent);
    draw_boolean(GraphicOptionsField::background, 17U,
                 configuration.background);
    draw_choice(GraphicOptionsField::track_detail, "Medium", value_x,
                row_y(18U),
                configuration.track_detail == GraphicTrackDetail::medium);
    draw_choice(GraphicOptionsField::track_detail, "High", value_x + 82U,
                row_y(18U),
                configuration.track_detail == GraphicTrackDetail::high);
    draw_choice(GraphicOptionsField::car_detail, "Low", value_x, row_y(19U),
                configuration.car_detail == GraphicCarDetail::low);
    draw_choice(GraphicOptionsField::car_detail, "Medium", value_x + 60U,
                row_y(19U),
                configuration.car_detail == GraphicCarDetail::medium);
    draw_choice(GraphicOptionsField::car_detail, "High", value_x + 143U,
                row_y(19U),
                configuration.car_detail == GraphicCarDetail::high);
    draw_choice(GraphicOptionsField::car_shading, "Flat", value_x, row_y(20U),
                configuration.car_shading == GraphicCarShading::flat);
    draw_choice(GraphicOptionsField::car_shading, "Gouraud", value_x + 50U,
                row_y(20U),
                configuration.car_shading == GraphicCarShading::gouraud);
    draw_choice(GraphicOptionsField::car_shading, "Reflection", value_x + 125U,
                row_y(20U),
                configuration.car_shading == GraphicCarShading::reflection);
    draw_choice(GraphicOptionsField::car_shading, "Glenz", value_x + 215U,
                row_y(20U),
                configuration.car_shading == GraphicCarShading::glenz);
    draw_percentage_bar(GraphicOptionsField::view_distance, 21U,
                        configuration.view_distance, 30.0F, 150.0F);
    draw_boolean(GraphicOptionsField::z_read, 22U, configuration.z_read);
  }
  const auto motion_blur_row = display_row(
      static_cast<std::size_t>(GraphicOptionsField::motion_blur));
  draw_boolean(GraphicOptionsField::motion_blur, motion_blur_row,
               configuration.motion_blur);
  return result;
}

FrontEndFrame compose_gameplay_options_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const GameplayOptionsField selection,
    const GraphicOptionsConfiguration &configuration,
    const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "Gameplay Options background must be exactly 640x400 RGBA");
  }
  constexpr std::size_t graphic_options_bank = 5U;
  FrontEndFrame result;
  result.rgba = background.rgba;
  place_transition_sprite(result, menu_sprites, positions, graphic_options_bank,
                          1U, "dots", transition_phase);
  place_transition_sprite(result, menu_sprites, positions, graphic_options_bank,
                          2U, "opt", transition_phase);
  place_transition_sprite(result, menu_sprites, positions, graphic_options_bank,
                          3U, "scr4", transition_phase);

  const auto panel = sample(positions, graphic_options_bank, 4U, transition_phase);
  const auto row_layout =
      sample(positions, graphic_options_bank, 24U, transition_phase);
  const auto label_x = static_cast<std::uint32_t>(std::max(panel.x, 0));
  const auto first_y = static_cast<std::uint32_t>(std::max(panel.y, 0));
  const auto row_step =
      static_cast<std::uint32_t>(std::max(row_layout.y, 1));
  const auto value_x = label_x + 208U;
  constexpr std::array<std::uint8_t, 3U> label_color{96U, 96U, 192U};
  constexpr std::array<std::uint8_t, 3U> orange{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> white{255U, 255U, 255U};
  constexpr std::array<std::uint8_t, 3U> black{0U, 0U, 0U};
  constexpr std::array<std::string_view, 6U> labels{{
      "Info detail:", "Info map:", "Checkpoint info:",
      "Checkpoint displaytime:", "Camera shake:", "UI scale:",
  }};
  const auto selected = static_cast<std::size_t>(selection);
  for (std::size_t row = 0U; row < labels.size(); ++row) {
    draw_fnt_text(result, font, labels[row], label_x,
                  first_y + static_cast<std::uint32_t>(row) * row_step,
                  row == selected ? white : label_color);
  }
  const auto row_y = [&](const std::size_t row) {
    return first_y + static_cast<std::uint32_t>(row) * row_step;
  };
  const auto draw_choice = [&](const GameplayOptionsField field,
                               const std::string_view text,
                               const std::uint32_t x, const std::size_t row,
                               const bool active) {
    const auto focused = selection == field && active;
    if (focused) {
      const auto width = mh::content::measure_fnt_text(font, text);
      fill_rectangle(result, x - 2U, row_y(row) - 2U, width + 4U, 14U, white);
    }
    draw_fnt_text(result, font, text, x, row_y(row), focused ? black : orange);
  };
  const auto draw_info = [&](const GameplayOptionsField field,
                             const std::size_t row,
                             const GraphicInfoMode value) {
    draw_choice(field, "None", value_x, row, value == GraphicInfoMode::none);
    draw_choice(field, "Selective", value_x + 60U, row,
                value == GraphicInfoMode::selective);
    draw_choice(field, "All", value_x + 148U, row,
                value == GraphicInfoMode::all);
  };
  draw_info(GameplayOptionsField::info_detail, 0U, configuration.info_detail);
  draw_info(GameplayOptionsField::info_map, 1U, configuration.info_map);
  draw_info(GameplayOptionsField::checkpoint_info, 2U,
            configuration.checkpoint_info);

  const auto checkpoint_seconds =
      static_cast<double>(configuration.checkpoint_display_time_ms) / 1000.0;
  std::ostringstream checkpoint_text;
  checkpoint_text << std::fixed
                  << std::setprecision(
                         configuration.checkpoint_display_time_ms % 1000U == 0U
                             ? 0
                             : 1)
                  << checkpoint_seconds << 's';
  draw_fnt_text(result, font, checkpoint_text.str(), value_x, row_y(3U),
                selection == GameplayOptionsField::checkpoint_display_time
                    ? white
                    : orange);
  fill_rectangle(result, value_x + 28U, row_y(3U) + 7U, 218U, 2U, orange);
  const auto checkpoint_marker = static_cast<std::uint32_t>(std::lround(
      static_cast<double>(configuration.checkpoint_display_time_ms) / 10000.0 *
      215.0));
  fill_rectangle(result, value_x + 28U + checkpoint_marker, row_y(3U) + 4U,
                 3U, 8U,
                 selection == GameplayOptionsField::checkpoint_display_time
                     ? white
                     : orange);

  draw_choice(GameplayOptionsField::camera_shake, "Off", value_x, 4U,
              !configuration.camera_shake);
  draw_choice(GameplayOptionsField::camera_shake, "On", value_x + 60U, 4U,
              configuration.camera_shake);
  const auto scale = static_cast<std::uint32_t>(
      std::lround(static_cast<double>(configuration.ui_scale)));
  draw_fnt_text(result, font, std::to_string(scale) + "%", value_x, row_y(5U),
                selection == GameplayOptionsField::ui_scale ? white : orange);
  constexpr std::uint32_t bar_x_offset = 55U;
  constexpr std::uint32_t marker_range = 188U;
  fill_rectangle(result, value_x + bar_x_offset, row_y(5U) + 7U, 191U, 2U,
                 orange);
  const auto scale_fraction =
      std::clamp((configuration.ui_scale - 50.0F) / 100.0F, 0.0F, 1.0F);
  const auto scale_marker = static_cast<std::uint32_t>(std::lround(
      static_cast<double>(scale_fraction) * marker_range));
  fill_rectangle(result, value_x + bar_x_offset + scale_marker,
                 row_y(5U) + 4U, 3U, 8U,
                 selection == GameplayOptionsField::ui_scale ? white : orange);
  return result;
}

FrontEndFrame compose_personal_options_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const std::uint32_t transition_phase,
    const PersonalOptionsField selection,
    const PersonalOptionsConfiguration &configuration,
    const std::uint32_t selected_label_frame) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "Personal Options background must be exactly 640x400 RGBA");
  }

  constexpr std::size_t personal_options_bank = 17U;
  FrontEndFrame result;
  result.rgba = background.rgba;
  place_sprite(result, menu_sprites, positions, personal_options_bank, 1U,
               "dots", transition_phase);
  place_sprite(result, menu_sprites, positions, personal_options_bank, 2U,
               "opt", transition_phase);
  place_sprite(result, menu_sprites, positions, personal_options_bank, 3U,
               "scrd", transition_phase);

  const auto label_layout =
      sample(positions, personal_options_bank, 20U, transition_phase);
  const auto value_layout =
      sample(positions, personal_options_bank, 21U, transition_phase);
  const auto label_x = static_cast<std::uint32_t>(std::max(label_layout.x, 0));
  const auto first_y = static_cast<std::uint32_t>(std::max(label_layout.y, 0));
  const auto value_x = static_cast<std::uint32_t>(std::max(value_layout.x, 0));
  const auto row_step = static_cast<std::uint32_t>(std::max(value_layout.y, 1));

  constexpr std::array<std::uint8_t, 3U> label_color{96U, 96U, 192U};
  constexpr std::array<std::uint8_t, 3U> orange{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> white{255U, 255U, 255U};

  const auto field_color = [&](const PersonalOptionsField field) {
    return selection == field ? white : label_color;
  };
  const auto value_color = [&](const PersonalOptionsField field) {
    return selection == field ? white : orange;
  };
  draw_fnt_text(result, font, "Name:", label_x, first_y,
                field_color(PersonalOptionsField::name));
  draw_fnt_text(result, font, "Team:", label_x, first_y + row_step,
                field_color(PersonalOptionsField::team));
  draw_fnt_text(result, font, "Colour:", label_x, first_y + row_step * 2U,
                field_color(PersonalOptionsField::colour));
  draw_fnt_text(result, font, "System:", label_x, first_y + row_step * 3U,
                field_color(PersonalOptionsField::system));
  for (std::uint32_t key = 0U; key < 10U; ++key) {
    const std::string name = "Key " + std::to_string(key) + ":";
    const auto field = static_cast<PersonalOptionsField>(
        static_cast<std::uint8_t>(PersonalOptionsField::short_key_0) + key);
    draw_fnt_text(result, font, name, label_x, first_y + row_step * (4U + key),
                  field_color(field));
  }
  draw_fnt_text(result, font, "Car colours:", label_x, first_y + row_step * 14U,
                field_color(PersonalOptionsField::car_colours));

  const auto editable_value = [&](const PersonalOptionsField field,
                                  const std::string_view text,
                                  const std::uint32_t y) {
    std::string display(text);
    if (selection == field && (selected_label_frame & 1U) != 0U) {
      display += '|';
    }
    draw_fnt_text(result, font, display, value_x, y, value_color(field));
  };
  editable_value(PersonalOptionsField::name, configuration.player_name,
                 first_y);
  editable_value(PersonalOptionsField::team, configuration.team_name,
                 first_y + row_step);
  for (std::uint32_t key = 0U; key < 10U; ++key) {
    const auto field = static_cast<PersonalOptionsField>(
        static_cast<std::uint8_t>(PersonalOptionsField::short_key_0) + key);
    editable_value(field, configuration.short_keys[key],
                   first_y + row_step * (4U + key));
  }

  const auto metric =
      configuration.measurement_system == MeasurementSystem::metric;
  draw_fnt_text(result, font, "Metric (KPH)", value_x, first_y + row_step * 3U,
                metric ? value_color(PersonalOptionsField::system) : orange);
  draw_fnt_text(result, font, "Imperial (MPH)", value_x + 105U,
                first_y + row_step * 3U,
                metric ? orange : value_color(PersonalOptionsField::system));
  draw_fnt_text(result, font, configuration.custom_car_colours ? "On" : "Off",
                value_x, first_y + row_step * 14U,
                value_color(PersonalOptionsField::car_colours));

  // The captured p3.1 Main profile starts on the first colour. These are the
  // exact visible RGB swatches in its settled strip, kept as state data rather
  // than a captured bitmap.
  constexpr std::array<std::array<std::uint8_t, 3U>, 32U> swatches{{
      {{220U, 220U, 220U}}, {{174U, 174U, 174U}}, {{92U, 92U, 92U}},
      {{10U, 10U, 10U}},    {{20U, 200U, 220U}},  {{17U, 131U, 209U}},
      {{14U, 65U, 163U}},   {{10U, 20U, 116U}},   {{14U, 200U, 47U}},
      {{14U, 160U, 30U}},   {{13U, 120U, 19U}},   {{10U, 80U, 10U}},
      {{240U, 100U, 10U}},  {{220U, 24U, 10U}},   {{138U, 15U, 14U}},
      {{70U, 10U, 12U}},    {{230U, 210U, 20U}},  {{209U, 164U, 24U}},
      {{163U, 104U, 17U}},  {{117U, 57U, 10U}},   {{220U, 170U, 203U}},
      {{195U, 91U, 140U}},  {{142U, 33U, 91U}},   {{79U, 10U, 76U}},
      {{10U, 220U, 205U}},  {{14U, 193U, 173U}},  {{13U, 131U, 131U}},
      {{10U, 61U, 69U}},    {{195U, 173U, 148U}}, {{149U, 125U, 102U}},
      {{103U, 82U, 63U}},   {{56U, 41U, 29U}},
  }};
  const auto nearest_swatch = [&](const PersonalColour colour) {
    auto best = std::size_t{0U};
    auto best_distance = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t index = 0U; index < swatches.size(); ++index) {
      const auto red =
          static_cast<std::int32_t>(colour.red) - swatches[index][0U];
      const auto green =
          static_cast<std::int32_t>(colour.green) - swatches[index][1U];
      const auto blue =
          static_cast<std::int32_t>(colour.blue) - swatches[index][2U];
      const auto distance =
          static_cast<std::uint32_t>(red * red + green * green + blue * blue);
      if (distance < best_distance) {
        best = index;
        best_distance = distance;
      }
    }
    return best;
  };
  const auto draw_palette = [&](const PersonalOptionsField field,
                                const PersonalColour colour,
                                const std::uint32_t y) {
    const std::array<std::uint8_t, 3U> current{colour.red, colour.green,
                                               colour.blue};
    fill_rectangle(result, value_x, y - 1U, 32U, 10U,
                   selection == field ? white : orange);
    fill_rectangle(result, value_x + 1U, y, 30U, 8U, current);
    const auto selected_swatch = nearest_swatch(colour);
    auto swatch_x = value_x + 44U;
    for (std::size_t index = 0U; index < swatches.size(); ++index) {
      if (index == selected_swatch) {
        fill_rectangle(result, swatch_x - 1U, y - 1U, 12U, 10U,
                       selection == field ? white : orange);
      }
      fill_rectangle(result, swatch_x, y, 10U, 8U, swatches[index]);
      swatch_x += 12U;
    }
  };
  draw_palette(PersonalOptionsField::colour, configuration.player_colour,
               first_y + row_step * 2U);

  if (configuration.custom_car_colours) {
    constexpr std::array<std::string_view, 3U> car_labels{
        "Car colour 1:", "Car colour 2:", "Car colour 3:"};
    for (std::uint32_t index = 0U; index < 3U; ++index) {
      const auto field = static_cast<PersonalOptionsField>(
          static_cast<std::uint8_t>(PersonalOptionsField::car_colour_1) +
          index);
      const auto y = first_y + row_step * (15U + index);
      draw_fnt_text(result, font, car_labels[index], label_x, y,
                    field_color(field));
      draw_palette(field, configuration.car_colours[index], y);
    }
  }
  return result;
}

FrontEndFrame compose_control_options_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const std::uint32_t transition_phase,
    const ControlOptionsFocus focus, const ControlOptionsField selection,
    const ControlOptionsConfiguration &configuration,
    const bool binding_capture_active) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "Control Options background must be exactly 640x400 RGBA");
  }

  constexpr std::size_t control_options_bank = 8U;
  FrontEndFrame result;
  result.rgba = background.rgba;
  // Bank 8 records 1/2/3/4 use the recovered retail
  // mode-7/mode-3/mode-5/mode-6 raster paths.
  place_transition_sprite(result, menu_sprites, positions, control_options_bank,
                          1U, "dots", transition_phase);
  place_transition_sprite(result, menu_sprites, positions, control_options_bank,
                          2U, "opt", transition_phase);
  place_transition_sprite(result, menu_sprites, positions, control_options_bank,
                          3U, "scr3", transition_phase);
  place_transition_sprite(result, menu_sprites, positions, control_options_bank,
                          4U, "selco", transition_phase);

  // RVA 0x8819d renders the action table from records 24/25, using record
  // 30's 20-pixel stride. Record 26 supplies the two-pixel label inset.
  const auto label_origin =
      sample(positions, control_options_bank, 24U, transition_phase);
  const auto value_origin =
      sample(positions, control_options_bank, 25U, transition_phase);
  const auto label_inset =
      sample(positions, control_options_bank, 26U, transition_phase);
  const auto row_layout =
      sample(positions, control_options_bank, 30U, transition_phase);
  const auto row_step = std::max(row_layout.y, 1);
  const auto label_x = label_origin.x + label_inset.x;

  constexpr std::array<std::string_view, 14U> labels{{
      "tforf",
      "tmspx",
      "tmspy",
      "tmspz",
      "tlef",
      "trig",
      "tacc",
      "tbre",
      "tgup",
      "tgdo",
      "thorn",
      "thbr",
      "tsht",
      "tmenu",
  }};
  for (std::size_t row = 0U; row < labels.size(); ++row) {
    const auto y = label_origin.y + static_cast<std::int32_t>(row) * row_step;
    place_sprite_at(result, menu_sprites, "ctrla", label_origin.x, y);
    place_sprite_at(result, menu_sprites, labels[row], label_x, y);
    place_sprite_at(result, menu_sprites, "ctrlb", value_origin.x, y - 1);
  }

  constexpr std::array<std::uint8_t, 3U> orange{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> white{255U, 255U, 255U};
  constexpr std::array<std::uint8_t, 3U> black{0U, 0U, 0U};

  const auto profile_index =
      configuration.profiles.empty()
          ? 0U
          : std::min<std::size_t>(configuration.profile_index,
                                  configuration.profiles.size() - 1U);
  const ControlProfileConfiguration fallback_profile;
  const auto &profile = configuration.profiles.empty()
                            ? fallback_profile
                            : configuration.profiles[profile_index];
  // RVA 0x886ad..0x887e7 keeps the active profile at the bank-8 record-4
  // selector. Record 21 is the text inset and record 22 supplies both the
  // four-neighbour limit (32 / 8) and the exact 27-pixel vertical stride.
  // Therefore changing profiles scrolls the names through the fixed selco
  // frame; it does not move the frame or leave Keyboard in its centre.
  const auto profile_selector =
      sample(positions, control_options_bank, 4U, transition_phase);
  const auto profile_label =
      sample(positions, control_options_bank, 21U, transition_phase);
  const auto profile_layout =
      sample(positions, control_options_bank, 22U, transition_phase);
  const auto profile_x = profile_selector.x + profile_label.x;
  const auto profile_y = profile_selector.y + profile_label.y;
  const auto profile_step = std::max(profile_layout.y, 1);
  const auto profile_neighbours = std::max(profile_layout.x / 8, 0);
  const auto first_profile =
      profile_index > static_cast<std::size_t>(profile_neighbours)
          ? profile_index - static_cast<std::size_t>(profile_neighbours)
          : 0U;
  const auto last_profile = std::min(
      configuration.profiles.size(),
      profile_index + static_cast<std::size_t>(profile_neighbours) + 1U);
  for (auto index = first_profile; index < last_profile; ++index) {
    const auto relative = static_cast<std::int32_t>(index) -
                          static_cast<std::int32_t>(profile_index);
    const auto y = profile_y + relative * profile_step;
    if (profile_x >= 0 && y >= 0) {
      // Helper RVA 0x88140 selects font mode 2 for the current profile even
      // after focus has entered the settings table.
      draw_fnt_text(result, font, configuration.profiles[index].name,
                    static_cast<std::uint32_t>(profile_x),
                    static_cast<std::uint32_t>(y),
                    index == profile_index ? white : orange);
    }
  }

  const auto value_x =
      static_cast<std::uint32_t>(std::max(value_origin.x + 44, 0));
  const auto value_y =
      static_cast<std::uint32_t>(std::max(value_origin.y + 5, 0));

  const auto field_selected = [&](const ControlOptionsField field) {
    return focus == ControlOptionsFocus::field_list && selection == field;
  };
  const auto draw_force_choice = [&](const std::string_view text,
                                     const std::uint32_t x, const bool active) {
    if (active && field_selected(ControlOptionsField::force_feedback)) {
      fill_rectangle(result, x - 7U, value_y - 1U, 35U, 12U, orange);
      draw_fnt_text(result, font, text, x, value_y, black);
    } else {
      draw_fnt_text(result, font, text, x, value_y,
                    active ||
                            field_selected(ControlOptionsField::force_feedback)
                        ? white
                        : orange);
    }
  };
  draw_force_choice("Off", value_x, !profile.force_feedback);
  draw_force_choice("On", value_x + 40U, profile.force_feedback);

  for (std::uint32_t row = 1U; row <= 3U; ++row) {
    const auto y = value_y + row * static_cast<std::uint32_t>(row_step);
    const auto field = static_cast<ControlOptionsField>(row);
    const auto percentage = static_cast<std::uint32_t>(std::lround(
        static_cast<double>(profile.mouse_speeds[row - 1U]) * 100.0));
    draw_fnt_text(result, font, std::to_string(percentage) + "%", value_x, y,
                  field_selected(field) ? white : orange);

    const auto slider_x =
        static_cast<std::uint32_t>(std::max(value_origin.x + 107, 0));
    fill_rectangle(result, slider_x + 1U, y + 2U, 150U, 1U, orange);
    fill_rectangle(result, slider_x + 1U, y + 6U, 150U, 1U, orange);
    fill_rectangle(result, slider_x, y + 3U, 2U, 3U, orange);
    fill_rectangle(result, slider_x + 150U, y + 3U, 2U, 3U, orange);
    const auto marker = static_cast<std::uint32_t>(std::lround(
        std::clamp(static_cast<double>(profile.mouse_speeds[row - 1U]) / 10.0,
                   0.0, 1.0) *
        150.0));
    const auto marker_x = slider_x + marker;
    const auto marker_color = field_selected(field) ? white : orange;
    fill_rectangle(result, marker_x, y, 4U, 1U, marker_color);
    fill_rectangle(result, marker_x, y + 8U, 4U, 1U, marker_color);
    fill_rectangle(result, marker_x, y + 1U, 2U, 7U, marker_color);
    fill_rectangle(result, marker_x + 2U, y + 1U, 2U, 7U, marker_color);
  }

  for (std::size_t row = 0U; row < profile.bindings.size(); ++row) {
    const auto field = static_cast<ControlOptionsField>(
        static_cast<std::uint8_t>(ControlOptionsField::turn_left) + row);
    auto binding = profile.bindings[row];
    if (binding_capture_active && field_selected(field)) {
      binding += '|';
    }
    draw_fnt_text(result, font, binding, value_x,
                  value_y + static_cast<std::uint32_t>(row + 4U) *
                                static_cast<std::uint32_t>(row_step),
                  field_selected(field) ? white : orange);
  }
  return result;
}

FrontEndFrame compose_sound_options_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const std::uint32_t transition_phase,
    const SoundOptionsGlitchFrame &glitch, const SoundOptionsField selection,
    const SoundOptionsConfiguration &configuration,
    const SoundOptionsPresentation &presentation,
    const std::uint32_t selected_label_frame) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "Sound Options background must be exactly 640x400 RGBA");
  }

  constexpr std::size_t sound_options_bank = 6U;
  FrontEndFrame result;
  result.rgba = background.rgba;

  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 1U,
                     "dots", transition_phase, glitch);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 2U,
                     "opt", transition_phase, glitch);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 3U,
                     "scr2", transition_phase, glitch);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 4U,
                     "snd1", transition_phase, glitch);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 5U,
                     "snd4", transition_phase, glitch);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 6U,
                     "snd3", transition_phase, glitch);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 8U,
                     "sndm", transition_phase, glitch);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 9U,
                     "sndm", transition_phase, glitch);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 10U,
                     "onoff", transition_phase, glitch,
                     configuration.cd_loop ? 0U : 7U);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 12U,
                     "sndt", transition_phase, glitch);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 13U,
                     "snda", transition_phase, glitch);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 14U,
                     "sndc", transition_phase, glitch);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 15U,
                     "tsfx", transition_phase, glitch,
                     selection == SoundOptionsField::sound_effects_volume
                         ? selected_label_frame % 8U
                         : 0U);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 16U,
                     "tcdm", transition_phase, glitch,
                     selection == SoundOptionsField::cd_music_volume
                         ? selected_label_frame % 8U
                         : 0U);
  place_sound_sprite(
      result, menu_sprites, positions, sound_options_bank, 17U, "tcdl",
      transition_phase, glitch,
      selection == SoundOptionsField::cd_loop ? selected_label_frame % 8U : 0U);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 18U,
                     "sndb", transition_phase, glitch);
  const auto row_selected = selection == SoundOptionsField::cd_track;
  const auto row_frame = row_selected && selected_label_frame != 0U ? 1U : 0U;
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 19U,
                     "sndak", transition_phase, glitch, row_frame);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 20U,
                     "sndck", transition_phase, glitch, row_frame);
  place_sound_sprite(result, menu_sprites, positions, sound_options_bank, 21U,
                     "sndtk", transition_phase, glitch, row_frame);

  constexpr std::array<std::uint8_t, 3U> orange{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> white{255U, 255U, 255U};
  constexpr std::array<std::uint8_t, 3U> black{0U, 0U, 0U};
  constexpr std::array<std::uint8_t, 3U> muted{96U, 96U, 192U};

  const auto sound_device_position =
      sample(positions, sound_options_bank, 7U, transition_phase);
  const auto sound_device_delta_x = sound_device_position.x - 6;
  const auto sound_device_delta_y = sound_device_position.y - 86;
  const auto sound_device_x =
      static_cast<std::uint32_t>(std::max(0, 4 + sound_device_delta_x));
  const auto sound_device_y =
      static_cast<std::uint32_t>(std::max(0, 84 + sound_device_delta_y));
  const auto sound_driver_x =
      static_cast<std::uint32_t>(std::max(0, 132 + sound_device_delta_x));
  draw_fnt_text(result, font, "sound device:", sound_device_x, sound_device_y,
                muted);
  auto output_name = configuration.output_device_name().empty()
                         ? std::string("Windows default")
                         : std::string(configuration.output_device_name());
  if (mh::content::measure_fnt_text(font, output_name) > 164U) {
    while (!output_name.empty() &&
           mh::content::measure_fnt_text(font, output_name + "...") > 164U) {
      output_name.pop_back();
    }
    output_name += "...";
  }
  if (selection == SoundOptionsField::sound_device) {
    fill_rectangle(result, sound_driver_x, sound_device_y, 164U, 14U, white);
    draw_fnt_text(result, font, output_name, sound_driver_x,
                  sound_device_y, black);
  } else {
    draw_fnt_text(result, font, output_name, sound_driver_x,
                  sound_device_y, orange);
  }

  // The retail page keeps the selected authored song on the middle row and
  // shows its immediate neighbours above and below. At the first song this
  // naturally produces the accepted Goldbridge/Redrock two-row frame.
  if (!presentation.tracks.empty() &&
      !configuration.assigned_cd_tracks.empty()) {
    const auto selected_song = std::min<std::size_t>(
        configuration.song_index, presentation.tracks.size() - 1U);
    const auto first_song = selected_song == 0U ? 0U : selected_song - 1U;
    const auto last_song =
        std::min(presentation.tracks.size() - 1U, selected_song + 1U);
    for (auto song = first_song; song <= last_song; ++song) {
      const auto row = static_cast<std::int32_t>(song) -
                       static_cast<std::int32_t>(selected_song);
      const auto y = static_cast<std::int32_t>(317) + row * 30;
      const auto digit_y = static_cast<std::int32_t>(313) + row * 32;
      draw_fnt_text(result, font, presentation.tracks[song].name, 207U,
                    static_cast<std::uint32_t>(y), orange);
      const auto assigned_index =
          std::min(song, configuration.assigned_cd_tracks.size() - 1U);
      place_numa_two_digits(result, menu_sprites,
                            configuration.assigned_cd_tracks[assigned_index],
                            370, digit_y);
      const auto duration = presentation.tracks[song].duration_seconds;
      place_numa_two_digits(result, menu_sprites, duration / 60U, 491, digit_y);
      place_numa_two_digits(result, menu_sprites, duration % 60U, 547, digit_y);
    }
  }

  // p3.1 RVAs 0x891e8..0x89373: bank-6 records 24/25 center the SFX/music
  // needles and 26/27 hold angle endpoints. Helper 0xa676c uses pi/2048 and
  // two triangles at call-site half-width 3; sndm's fixed 9..10 redline stays.
  const auto angle_from =
      sample(positions, sound_options_bank, 26U, transition_phase);
  const auto angle_to =
      sample(positions, sound_options_bank, 27U, transition_phase);
  const auto draw_volume_indicator =
      [&](const std::int32_t center_x, const std::int32_t center_y,
          const std::uint8_t volume, const bool focused) {
        constexpr double pi = 3.14159265358979323846;
        const auto from_units = static_cast<double>(angle_from.x) * 8.0;
        const auto to_units = static_cast<double>(angle_to.x) * 8.0;
        const auto angle_units = from_units + (to_units - from_units) *
                                                  static_cast<double>(volume) /
                                                  255.0;
        const auto radians = angle_units * pi / 2048.0;
        const auto sine = std::sin(radians);
        const auto cosine = std::cos(radians);
        const std::array<std::uint8_t, 3U> indicator =
            focused ? orange : std::array<std::uint8_t, 3U>{220U, 90U, 0U};
        constexpr double recovered_projection = 5.0 / 6.0;
        const auto point = [&](const double x, const double y) {
          return std::array<double, 2U>{
              static_cast<double>(center_x) + sine * x + cosine * y,
              static_cast<double>(center_y) +
                  (cosine * x - sine * y) * recovered_projection};
        };
        const auto left = point(-3.0, -5.0);
        const auto tip = point(0.0, -40.0);
        const auto right = point(3.0, -5.0);
        const auto tail = point(0.0, 10.0);
        fill_solid_triangle(result, left, tip, right, indicator);
        fill_solid_triangle(result, left, right, tail, indicator);
      };
  const auto sfx_center =
      sample(positions, sound_options_bank, 24U, transition_phase);
  const auto music_center =
      sample(positions, sound_options_bank, 25U, transition_phase);
  draw_volume_indicator(sfx_center.x, sfx_center.y,
                        configuration.sound_effects_volume,
                        selection == SoundOptionsField::sound_effects_volume);
  draw_volume_indicator(music_center.x, music_center.y,
                        configuration.cd_music_volume,
                        selection == SoundOptionsField::cd_music_volume);
  return result;
}

FrontEndFrame
compose_rankings_frame(const mh::content::TgaImage &background,
                       const mh::content::SprArchive &menu_sprites,
                       const mh::content::SprPositionData &positions,
                       const mh::content::FntData &font,
                       const RankingsField selection,
                       const RankingsConfiguration &configuration,
                       const RankingsPresentation &presentation,
                       const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "Rankings background must be exactly 640x400 RGBA");
  }

  constexpr auto bank = rankings_position_bank;
  FrontEndFrame result;
  result.rgba = background.rgba;
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RankingsPositionRecord::background_dots), "dots",
      transition_phase);
  place_transition_sprite(result, menu_sprites, positions, bank,
                          position_record_index(RankingsPositionRecord::header),
                          "rank", transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RankingsPositionRecord::screen_badge), "scr6",
      transition_phase);

  const auto label_frame = [selection](const RankingsField field) {
    // RVA 0x8484f initializes all label states to frame 1 and changes only
    // the focused slot to frame 0.
    return selection == field ? 0U : 1U;
  };
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RankingsPositionRecord::type_label), "ranka",
      transition_phase, label_frame(RankingsField::type));
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RankingsPositionRecord::track_or_division_label),
      "rankb", transition_phase,
      label_frame(RankingsField::track_or_division) +
          (configuration.type == RankingsType::league ? 2U : 0U));
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RankingsPositionRecord::laps_label), "rankd",
      transition_phase, label_frame(RankingsField::laps));
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RankingsPositionRecord::difficulty_label), "ranke",
      transition_phase, label_frame(RankingsField::difficulty));
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RankingsPositionRecord::table_header), "rankz",
      transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RankingsPositionRecord::table_frame), "rankx",
      transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(RankingsPositionRecord::table_bracket), "mupp",
      transition_phase);

  constexpr std::array<std::string_view, 4U> type_names{
      "Best laptime", "Best racetime", "Single race", "League"};
  constexpr std::array<std::string_view, 3U> division_names{
      "Division 3", "Division 2&3", "Total"};
  constexpr std::array<std::string_view, 6U> lap_names{"1",  "3",  "5",
                                                       "10", "15", "25"};
  constexpr std::array<std::string_view, 3U> difficulty_names{"Easy", "Medium",
                                                              "Hard"};
  constexpr std::array<std::uint8_t, 3U> normal_color{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> focused_color{255U, 255U, 255U};
  // RVA 0x84832 samples the Y coordinate once from record 24 and reuses it
  // for all four filter values; records 25..27 contribute X only.
  const auto value_row =
      sample(positions, bank,
             position_record_index(RankingsPositionRecord::type_value),
             transition_phase)
          .y;

  const auto draw_value = [&](const RankingsPositionRecord record,
                              const RankingsField field,
                              const std::string_view text) {
    const auto position = sample(positions, bank, position_record_index(record),
                                 transition_phase);
    // RVA 0x848a4..0x84a20 passes each bank-11 record-24..27 X value
    // directly to the compiled-FNT/list renderer. The 0x00400000 option used
    // by three fields controls its auxiliary list treatment; it is not a
    // horizontal-centering flag.
    draw_fnt_text(result, font, text,
                  static_cast<std::uint32_t>(std::max(position.x, 0)),
                  static_cast<std::uint32_t>(std::max(value_row, 0)),
                  selection == field ? focused_color : normal_color);
  };

  const auto type_index = std::min<std::size_t>(
      static_cast<std::size_t>(configuration.type), type_names.size() - 1U);
  draw_value(RankingsPositionRecord::type_value, RankingsField::type,
             type_names[type_index]);
  const auto track_or_division =
      configuration.type == RankingsType::league
          ? division_names[std::min<std::size_t>(configuration.division_index,
                                                 division_names.size() - 1U)]
          : std::string_view(presentation.track_name);
  draw_value(RankingsPositionRecord::track_or_division_value,
             RankingsField::track_or_division, track_or_division);

  const auto suppress_laps = configuration.type == RankingsType::best_laptime ||
                             configuration.type == RankingsType::league;
  draw_value(RankingsPositionRecord::laps_value, RankingsField::laps,
             suppress_laps
                 ? std::string_view("----")
                 : lap_names[std::min<std::size_t>(configuration.laps_index,
                                                   lap_names.size() - 1U)]);
  const auto suppress_difficulty =
      configuration.type == RankingsType::best_laptime ||
      configuration.type == RankingsType::best_racetime;
  draw_value(RankingsPositionRecord::difficulty_value,
             RankingsField::difficulty,
             suppress_difficulty
                 ? std::string_view("----")
                 : difficulty_names[std::min<std::size_t>(
                       static_cast<std::size_t>(configuration.difficulty),
                       difficulty_names.size() - 1U)]);

  // RVA 0x84a32..0x84b45 iterates ten 0x38-byte records. Name and vehicle are
  // fixed retail fields of 24 and 20 bytes including the terminator.
  constexpr std::array<std::uint8_t, 3U> table_color{255U, 255U, 255U};
  const auto format_ranking_time = [](const std::uint32_t milliseconds) {
    const auto minutes = milliseconds / 60000U;
    const auto seconds = (milliseconds / 1000U) % 60U;
    const auto hundredths = (milliseconds / 10U) % 100U;
    std::ostringstream text;
    text << minutes << ':' << std::setfill('0') << std::setw(2) << seconds
         << ':' << std::setw(2) << hundredths;
    return text.str();
  };
  const auto bounded_text = [](const std::string &text,
                               const std::size_t maximum_bytes) {
    return text.substr(0U, std::min(text.size(), maximum_bytes));
  };
  const auto row_count =
      std::min<std::size_t>(presentation.entries.size(), 10U);
  for (std::size_t row = 0U; row < row_count; ++row) {
    const auto placement =
        rankings_row_placement(static_cast<std::uint32_t>(row));
    const auto &entry = presentation.entries[row];
    draw_fnt_text(result, font, bounded_text(entry.player_name, 23U),
                  placement.player_x, placement.y, table_color);
    draw_fnt_text(result, font, bounded_text(entry.vehicle_name, 19U),
                  placement.vehicle_x, placement.y, table_color);
    draw_fnt_text(result, font, format_ranking_time(entry.best_lap_ms),
                  placement.best_lap_x, placement.y, table_color);
    draw_fnt_text(result, font, format_ranking_time(entry.total_time_ms),
                  placement.total_time_x, placement.y, table_color);
  }
  return result;
}

RankingsRowPlacement rankings_row_placement(const std::uint32_t row_index) {
  if (row_index >= 10U) {
    throw ToolError(ExitCode::format,
                    "rankings row index must be zero through nine");
  }
  // Bank 11: record 16=(36,207), record 17=(187,15),
  // record 18=(333,0), record 19=(465,0).
  return {36, 187, 333, 465, 207 + static_cast<std::int32_t>(row_index * 15U)};
}

RaceResultPlacement
race_result_placement(const std::uint32_t finishing_position) {
  if (finishing_position == 0U || finishing_position > 8U) {
    throw ToolError(ExitCode::format,
                    "race-result finishing position must be 1 through 8");
  }

  const auto right_column = (finishing_position % 2U) == 0U;
  const auto row = right_column ? (finishing_position - 2U) / 2U
                                : (finishing_position - 1U) / 2U;
  const auto row_offset = static_cast<std::int32_t>(row * 80U);
  if (right_column) {
    // p3.1 second loop starts with edi=100, esi=62, ebx=2 and places the
    // 90x56 portrait through RVA 0x3ff00 at (327, 62).
    return {
        327,
        62 + row_offset,
        375,
        125 + row_offset,
        430,
        70 + row_offset,
        80 + row_offset,
        90 + row_offset,
        100 + row_offset,
        510,
    };
  }

  // p3.1 first loop starts with row origins 22/60 and position 1.
  return {
      20,
      22 + row_offset,
      60,
      85 + row_offset,
      125,
      30 + row_offset,
      40 + row_offset,
      50 + row_offset,
      60 + row_offset,
      210,
  };
}

std::vector<RankedRaceResultEntry>
make_race_result_field(const std::span<const RaceResultVehicleState> vehicles,
                       const std::uint32_t cutoff_time_ms,
                       const bool league_result) {
  if (vehicles.empty() || vehicles.size() > 8U) {
    throw ToolError(ExitCode::format,
                    "race-result field requires one through eight vehicles");
  }
  std::vector<std::size_t> standings(vehicles.size());
  std::iota(standings.begin(), standings.end(), 0U);
  std::stable_sort(
      standings.begin(), standings.end(),
      [&](const std::size_t left, const std::size_t right) {
        const auto &left_state = vehicles[left];
        const auto &right_state = vehicles[right];
        if (left_state.race_time_ms.has_value() !=
            right_state.race_time_ms.has_value()) {
          return left_state.race_time_ms.has_value();
        }
        if (left_state.race_time_ms.has_value()) {
          return *left_state.race_time_ms < *right_state.race_time_ms;
        }
        if (left_state.progress_samples != right_state.progress_samples) {
          return left_state.progress_samples > right_state.progress_samples;
        }
        return left < right;
      });

  static constexpr std::array<std::int32_t, 8U> score_by_position{12, 10, 8, 6,
                                                                  4,  3,  2, 1};
  std::vector<RankedRaceResultEntry> result;
  result.reserve(standings.size());
  for (std::size_t rank = 0U; rank < standings.size(); ++rank) {
    const auto source_index = standings[rank];
    const auto &state = vehicles[source_index];
    const auto starting_score = league_result ? state.starting_score : 0;
    const auto score = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(starting_score) + score_by_position[rank],
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max());
    result.push_back(
        {source_index,
         {static_cast<std::uint32_t>(rank + 1U), state.car_name, state.nickname,
          static_cast<std::int32_t>(score),
          state.race_time_ms.value_or(cutoff_time_ms), state.best_lap_ms}});
  }
  return result;
}

FrontEndFrame compose_race_results_frame(
    const mh::content::TgaImage &background, const mh::content::FntData &font,
    const std::span<const RaceResultEntry> entries,
    const std::span<const mh::content::TgaImage> portraits) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "race-results background must be exactly 640x400 RGBA");
  }
  if (entries.empty() || entries.size() > 8U ||
      portraits.size() != entries.size()) {
    throw ToolError(ExitCode::format, "race results require one through eight "
                                      "entries and matching portraits");
  }

  FrontEndFrame result;
  result.rgba = background.rgba;
  constexpr std::array<std::uint8_t, 3U> white{255U, 255U, 255U};
  const auto plus_advance = static_cast<std::int32_t>(
      font.glyphs[static_cast<std::uint8_t>('+')].advance_width);
  const auto winning_time =
      std::min_element(
          entries.begin(), entries.end(),
          [](const RaceResultEntry &left, const RaceResultEntry &right) {
            return left.finishing_position < right.finishing_position;
          })
          ->race_time_ms;

  const auto format_time = [](const std::uint32_t milliseconds,
                              const char prefix) {
    const auto minutes = milliseconds / 60000U;
    const auto seconds = (milliseconds / 1000U) % 60U;
    const auto hundredths = (milliseconds / 10U) % 100U;
    std::ostringstream text;
    text << prefix << minutes << ':' << std::setfill('0') << std::setw(2)
         << seconds << ':' << std::setw(2) << hundredths;
    return text.str();
  };

  for (std::size_t index = 0U; index < entries.size(); ++index) {
    const auto &entry = entries[index];
    const auto &portrait = portraits[index];
    if (portrait.width != 90U || portrait.height != 56U ||
        portrait.rgba.size() != 90U * 56U * 4U) {
      throw ToolError(ExitCode::format,
                      "race-result portraits must be exactly 90x56 RGBA");
    }
    const auto placement = race_result_placement(entry.finishing_position);
    blit_rgba(result, portrait.rgba, portrait.width, portrait.height,
              placement.portrait_x, placement.portrait_y);
    draw_fnt_text(result, font, entry.car_name, placement.car_name_x,
                  placement.car_name_y, white);
    draw_fnt_text(result, font, "Nick:", placement.label_x, placement.nick_y,
                  white);
    draw_fnt_text(result, font, entry.nickname,
                  placement.value_x + plus_advance, placement.nick_y, white);
    draw_fnt_text(result, font, "Score:", placement.label_x, placement.score_y,
                  white);
    draw_fnt_text(result, font, std::to_string(entry.score),
                  placement.value_x + plus_advance, placement.score_y, white);
    draw_fnt_text(result, font, "Race:", placement.label_x, placement.race_y,
                  white);
    const auto race_text =
        entry.finishing_position == 1U
            ? format_time(entry.race_time_ms, ' ')
            : format_time(entry.race_time_ms >= winning_time
                              ? entry.race_time_ms - winning_time
                              : 0U,
                          '+');
    draw_fnt_text(result, font, race_text, placement.value_x, placement.race_y,
                  white);
    draw_fnt_text(result, font, "Best Lap:", placement.label_x,
                  placement.best_lap_y, white);
    draw_fnt_text(result, font, format_time(entry.best_lap_ms, ' '),
                  placement.value_x, placement.best_lap_y, white);
  }
  return result;
}

FrontEndFrame compose_league_results_frame(
    const mh::content::TgaImage &background, const mh::content::FntData &font,
    const std::span<const RaceResultEntry> entries,
    const std::span<const mh::content::TgaImage> portraits) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "league-results background must be exactly 640x400 RGBA");
  }
  if (entries.empty() || entries.size() > 8U ||
      portraits.size() != entries.size()) {
    throw ToolError(ExitCode::format, "league results require one through "
                                      "eight entries and matching portraits");
  }

  FrontEndFrame result;
  result.rgba = background.rgba;
  constexpr std::array<std::uint8_t, 3U> white{255U, 255U, 255U};
  for (std::size_t index = 0U; index < entries.size(); ++index) {
    const auto &entry = entries[index];
    const auto &portrait = portraits[index];
    if (portrait.width != 90U || portrait.height != 56U ||
        portrait.rgba.size() != 90U * 56U * 4U) {
      throw ToolError(ExitCode::format,
                      "league-result portraits must be exactly 90x56 RGBA");
    }
    const auto placement = race_result_placement(entry.finishing_position);
    blit_rgba(result, portrait.rgba, portrait.width, portrait.height,
              placement.portrait_x, placement.portrait_y);
    draw_fnt_text(result, font, entry.car_name, placement.car_name_x,
                  placement.car_name_y, white);
    draw_fnt_text(result, font, "Nick:", placement.label_x, placement.nick_y,
                  white);
    draw_fnt_text(result, font, entry.nickname, placement.value_x - 10,
                  placement.nick_y, white);
    draw_fnt_text(result, font, "Score:", placement.label_x, placement.score_y,
                  white);
    draw_fnt_text(result, font, std::to_string(entry.score),
                  placement.value_x - 10, placement.score_y, white);
  }
  return result;
}

FrontEndFrame
compose_credits_frame(const mh::content::TgaImage &background,
                      const mh::content::SprArchive &menu_sprites,
                      const mh::content::SprPositionData &positions,
                      const mh::content::FntData &font,
                      const std::uint32_t transition_phase) {
  if (background.width != front_end_logical_width ||
      background.height != front_end_logical_height ||
      background.rgba.size() !=
          static_cast<std::size_t>(front_end_logical_width) *
              front_end_logical_height * 4U) {
    throw ToolError(ExitCode::format,
                    "Credits background must be exactly 640x400 RGBA");
  }

  constexpr auto bank = credits_position_bank;
  FrontEndFrame result;
  result.rgba = background.rgba;
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(CreditsPositionRecord::background_dots), "dots",
      transition_phase);
  place_transition_sprite(
      result, menu_sprites, positions, bank,
      position_record_index(CreditsPositionRecord::screen_badge), "scr9",
      transition_phase);
  place_transition_sprite(result, menu_sprites, positions, bank,
                          position_record_index(CreditsPositionRecord::header),
                          "credi", transition_phase);

  // RVA 0x8d489 reads record 24 as the centred text origin. Record 25's Y
  // component is divided by eight for the line step (120/8 = 15 settled).
  const auto origin =
      sample(positions, bank,
             position_record_index(CreditsPositionRecord::text_origin),
             transition_phase);
  const auto spacing =
      sample(positions, bank,
             position_record_index(CreditsPositionRecord::text_spacing),
             transition_phase);
  const auto line_step = spacing.y / 8;
  constexpr std::array<std::uint8_t, 3U> heading_color{255U, 100U, 0U};
  constexpr std::array<std::uint8_t, 3U> name_color{96U, 96U, 192U};

  const auto draw_centered = [&](const std::string_view text,
                                 const std::int32_t line,
                                 const std::array<std::uint8_t, 3U> color) {
    const auto width = mh::content::measure_fnt_text(font, text);
    const auto x = origin.x - static_cast<std::int32_t>(width / 2U);
    const auto y = origin.y + line * line_step;
    draw_fnt_text(result, font, text, x, y, color);
  };

  // Exact normal-page strings and line multipliers from RVA
  // 0x8d66f..0x8d7c3. The original byte 0x99 in Nystr<o-umlaut>m is retained
  // so FONT1.FNT, rather than the host code page, selects the authored glyph.
  draw_centered("Programming:", 0, heading_color);
  draw_centered("Joakim Grundwall - Andreas Axelsson", 2, name_color);
  draw_centered("Daniel Hansen - Daniel Hansevi - Vidar Nygren", 3, name_color);
  draw_centered("Mattias Gruvman - Mikael Rudberg - Mikael Kalms", 4,
                name_color);
  draw_centered("Graphics:", 6, heading_color);
  draw_centered("Per-Anders Gustafsson - Joakim Wejdermar", 8, name_color);
  draw_centered("Patrik Bergdahl - Andreas Hansevi - Jens Oras", 9, name_color);
  draw_centered("Kenny Magnusson - Markus Nystr\x99m - Nicholas Nolby", 10,
                name_color);
  draw_centered("Music & sfx:", 12, heading_color);
  draw_centered("Olof Gustafsson", 14, name_color);
  draw_centered("Produced by:", 16, heading_color);
  draw_centered("Fredrik Liliegren", 18, name_color);
  return result;
}

} // namespace mh::ui
