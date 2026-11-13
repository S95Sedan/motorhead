#pragma once

#include <array>
#include <cstddef>

namespace mh::content {
struct AiRouteData;
}

namespace mh::game {

// Reproduces the first lookahead target calculation at p3.1
// RVA 0x00016ebf..0x00016eef. The retail helper at RVA 0x000c3a46
// temporarily selects x87 truncation (toward zero), so this is not a
// nearest-integer operation. Later curvature checks may replace this seed.
[[nodiscard]] std::size_t original_ai_initial_lookahead_sample(
    std::size_t current_sample, std::size_t sample_count, float speed_like,
    float route_spacing_control);

// Returns the first globally nearest sample using the same binary32 distance
// comparison used when the original p3.1 AI cursor is initialized.  This is
// also the route ownership needed by non-physical recorded vehicles, such as
// Ghost Race minimap markers.
[[nodiscard]] std::size_t original_ai_nearest_route_sample(
    const mh::content::AiRouteData &route,
    const std::array<float, 2U> &vehicle_xz);

// Exact route-cursor subset of p3.1 AI update RVA 0x00016810. Runtime
// selection scans increasing addresses first, then decreasing addresses,
// accepting a sample only while squared X/Z distance strictly improves.
// This does not synthesize steering, throttle, braking, collision, or
// catch-up behavior.
class OriginalAiRouteCursor {
public:
  OriginalAiRouteCursor(const mh::content::AiRouteData &route,
                        const std::array<float, 2U> &vehicle_xz);

  void reset(const std::array<float, 2U> &vehicle_xz);
  void update_current(const std::array<float, 2U> &vehicle_xz);
  void update_lookahead_seed(float speed_like, float route_spacing_control);
  void select_lookahead_sample(std::size_t sample);

  [[nodiscard]] std::size_t current_sample() const noexcept;
  [[nodiscard]] std::size_t lookahead_sample() const noexcept;

private:
  const mh::content::AiRouteData *route_ = nullptr;
  std::size_t current_sample_ = 0U;
  std::size_t lookahead_sample_ = 0U;
};

} // namespace mh::game
