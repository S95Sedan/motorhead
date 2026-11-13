#pragma once

#include <cstdint>

namespace mh::game {

struct CylindricalSkyWindow {
  double source_start = 0.0;
  double source_span = 0.0;
};

// Maps a horizontal world-facing camera direction and its perspective
// projection to a wrapped source interval in a full-width cylindrical sky
// texture.
[[nodiscard]] CylindricalSkyWindow
make_cylindrical_sky_window(double forward_x, double forward_z,
                            double texture_width, double viewport_width,
                            double focal_length);

// The p3.1 accelerated background backend multiplies the output height by the
// track's authored Horizon value, rounds that screen row with the x87 default
// nearest-even mode, and stores its signed displacement from half-height.
[[nodiscard]] std::int32_t
original_accelerated_horizon_offset(float authored_horizon,
                                    std::uint32_t viewport_height);

} // namespace mh::game
