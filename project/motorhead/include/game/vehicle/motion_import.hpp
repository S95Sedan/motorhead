#pragma once

#include <game/physics/body_pose.hpp>

#include <array>
#include <cstddef>

namespace mh::content {
struct MotionData;
struct MotionPathFrame;
struct TrackStartGrid;
} // namespace mh::content

namespace mh::game {

struct OriginalTrackPoseOffsets {
  double lateral = 0.0;
  double local_up = 0.0;
  double longitudinal = 0.0;
};

// Converts the owned LWMO path frame into the complete row-axis body pose used
// by the recovered physics. An explicit local-up offset permits a later
// suspension/ride-height owner without embedding a guessed value here.
[[nodiscard]] OriginalBodyPoseState
make_original_track_pose(const mh::content::MotionPathFrame &frame,
                         double local_up_offset = 0.0);

// Applies explicit right/up/forward offsets in the authored path frame. The
// values are a grid-placement boundary; original race-slot spacing remains to
// be recovered before callers may treat particular offsets as canonical.
[[nodiscard]] OriginalBodyPoseState
make_original_track_pose(const mh::content::MotionPathFrame &frame,
                         const OriginalTrackPoseOffsets &offsets);

// Reproduces the audited p3.1 two-column start-grid formula for slots 0..7.
// Vertical settlement remains owned by the separate contact-placement path.
[[nodiscard]] OriginalTrackPoseOffsets
make_original_start_grid_offsets(const mh::content::TrackStartGrid &grid,
                                 std::size_t slot_index);

// Reproduces the track-spline direction selected by p3.1 sub_0003b710:
// choose the globally nearest authored key, project onto its adjacent spans,
// then sample symmetrically around that authored frame until the chord is at
// least 0.5 world units long.
[[nodiscard]] std::array<double, 3U>
original_race_recovery_spline_direction(
    const mh::content::MotionData &motion,
    const std::array<double, 3U> &world_position);

// Position on the same projected spline frame used by recovery.
[[nodiscard]] std::array<double, 3U>
race_recovery_spline_position(
    const mh::content::MotionData &motion,
    const std::array<double, 3U> &world_position);

// Returns the retained p3.1 race-spline projection time stored at motion +0x08
// by sub_00072250. projected_key is the companion motion +0x20 key selected by
// that same projection call.
[[nodiscard]] double original_race_projected_motion_frame(
    const mh::content::MotionData &motion,
    const std::array<double, 3U> &world_position,
    std::size_t projected_key);

// p3.1's race owner projects the vehicle position onto the track definition's
// SplineName motion and copies the evaluated first rotation channel into the
// player engine mixer. This is authored track data, not a throttle-release
// response. projected_key must come from the same retained race projection
// update, preserving the original branch at overlapping spline positions. The
// mixer clamps the returned value to [0, 1].
[[nodiscard]] double original_race_spline_engine_mix(
    const mh::content::MotionData &motion,
    const std::array<double, 3U> &world_position,
    std::size_t projected_key);

} // namespace mh::game
