#pragma once

#include <array>
#include <algorithm>
#include <cmath>

namespace mh::render {

struct CarSurfacePoint {
  std::array<float, 3U> position{};
  std::array<float, 3U> normal{};
};

// Phong tessellation from authored positions and smooth normals. A shared edge
// depends only on its two endpoints, so adjacent patches meet at identical
// positions. Keep the original vertices and bound expansion to 12 cm.
inline CarSurfacePoint interpolate_car_surface(
    const std::array<CarSurfacePoint, 3U> &corners,
    const std::array<float, 3U> &weights,
    const std::array<bool, 3U> &curved_edges = {true, true, true},
    const float curvature_strength = 0.7F) {
  CarSurfacePoint result;
  for (std::size_t i = 0U; i < 3U; ++i) {
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
      result.position[axis] += corners[i].position[axis] * weights[i];
      result.normal[axis] += corners[i].normal[axis] * weights[i];
    }
  }
  std::array<float, 3U> displacement{};
  for (std::size_t i = 0U; i < 3U; ++i) {
    for (std::size_t j = 0U; j < 3U; ++j) {
      if (i == j || !curved_edges[3U - i - j]) continue;
      float distance = 0.0F;
      float length_squared = 0.0F;
      for (std::size_t axis = 0U; axis < 3U; ++axis) {
        distance += (corners[i].position[axis] - corners[j].position[axis]) * corners[i].normal[axis];
        length_squared += corners[i].normal[axis] * corners[i].normal[axis];
      }
      if (!std::isfinite(length_squared) || length_squared < 1.0e-8F) continue;
      for (std::size_t axis = 0U; axis < 3U; ++axis) {
        displacement[axis] += weights[i] * weights[j] * distance * corners[i].normal[axis] / length_squared;
      }
    }
  }
  float distance_squared = 0.0F;
  for (const float value : displacement) distance_squared += value * value;
  const float strength = std::min(std::clamp(curvature_strength, 0.0F, 0.7F), 0.12F / std::sqrt(std::max(1.0e-12F, distance_squared)));
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    result.position[axis] += displacement[axis] * strength;
  }
  return result;
}

} // namespace mh::render
