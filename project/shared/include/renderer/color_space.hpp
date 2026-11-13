#pragma once
#include <algorithm>
#include <cmath>

namespace mh::render {
inline float srgb_to_linear(float value) {
  value = std::clamp(value, 0.0F, 1.0F);
  return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}
} // namespace mh::render
