#pragma once

namespace mh::game {

// Motorhead's software renderer begins cueing at cue_start times the
// configured view distance and reaches the cue colour at the far limit.
[[nodiscard]] double original_distance_cue_factor(double view_depth,
                                                   double view_distance,
                                                   double cue_start);

// A cheap exponential-style buildup for modern extended view distances. It
// preserves the same start and end points as the original linear cue while
// making the intervening volume read as dense atmospheric fog.
[[nodiscard]] double dense_distance_cue_factor(double view_depth,
                                                double view_distance,
                                                double cue_start);

} // namespace mh::game
