#include <game/race/sky.hpp>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace mh::game {

CylindricalSkyWindow make_cylindrical_sky_window(const double forward_x,
                                                 const double forward_z,
                                                 const double texture_width,
                                                 const double viewport_width,
                                                 const double focal_length) {
  const auto horizontal_length_squared =
      forward_x * forward_x + forward_z * forward_z;
  if (!std::isfinite(horizontal_length_squared) ||
      horizontal_length_squared <= 1.0e-12) {
    throw std::invalid_argument(
        "cylindrical sky camera direction must be finite and horizontal");
  }
  if (!std::isfinite(texture_width) || texture_width <= 0.0 ||
      !std::isfinite(viewport_width) || viewport_width <= 0.0 ||
      !std::isfinite(focal_length) || focal_length <= 0.0) {
    throw std::invalid_argument(
        "cylindrical sky dimensions must be positive and finite");
  }

  constexpr double pi = 3.14159265358979323846;
  constexpr double full_turn = 2.0 * pi;
  const auto yaw = std::atan2(forward_x, forward_z);
  auto center_fraction = 0.5 + yaw / full_turn;
  center_fraction -= std::floor(center_fraction);
  const auto half_horizontal_fov =
      std::atan(viewport_width * 0.5 / focal_length);
  const auto source_span = texture_width * half_horizontal_fov / pi;
  auto source_start = center_fraction * texture_width - source_span * 0.5;
  source_start = std::fmod(source_start, texture_width);
  if (source_start < 0.0) {
    source_start += texture_width;
  }
  return {source_start, source_span};
}

std::int32_t
original_accelerated_horizon_offset(const float authored_horizon,
                                    const std::uint32_t viewport_height) {
  if (!std::isfinite(authored_horizon) || authored_horizon < 0.0F ||
      authored_horizon > 1.0F || viewport_height == 0U ||
      viewport_height > static_cast<std::uint32_t>(
                            std::numeric_limits<std::int32_t>::max())) {
    throw std::invalid_argument(
        "accelerated sky horizon requires a unit value and finite viewport");
  }

  const auto horizon_row = static_cast<std::int32_t>(
      std::nearbyint(static_cast<double>(viewport_height) *
                     static_cast<double>(authored_horizon)));
  const auto half_height = static_cast<std::int32_t>(viewport_height >> 1U);
  return horizon_row - half_height;
}

} // namespace mh::game
