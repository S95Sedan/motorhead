#include <game/race/distance_cue.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mh::game {

double original_distance_cue_factor(const double view_depth,
                                    const double view_distance,
                                    const double cue_start) {
  if (!std::isfinite(view_depth) || !std::isfinite(view_distance) ||
      !std::isfinite(cue_start) || view_depth < 0.0 || view_distance <= 0.0 ||
      cue_start < 0.0 || cue_start >= 1.0) {
    throw std::invalid_argument("distance-cue inputs are outside valid bounds");
  }
  const auto start_depth = view_distance * cue_start;
  return std::clamp((view_depth - start_depth) / (view_distance - start_depth),
                    0.0, 1.0);
}

double dense_distance_cue_factor(const double view_depth,
                                 const double view_distance,
                                 const double cue_start) {
  const auto linear =
      original_distance_cue_factor(view_depth, view_distance, cue_start);
  const auto clear_fraction = 1.0 - linear;
  return 1.0 - clear_fraction * clear_fraction;
}

} // namespace mh::game
