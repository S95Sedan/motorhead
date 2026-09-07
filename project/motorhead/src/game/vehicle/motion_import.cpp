#include <game/vehicle/motion_import.hpp>

#include <content/formats/motion_path.hpp>
#include <content/formats/track_definition.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mh::game {
namespace {

double squared_distance(const std::array<double, 3U> &left,
                        const std::array<double, 3U> &right) {
  auto result = 0.0L;
  for (std::size_t axis = 0U; axis < left.size(); ++axis) {
    const auto delta =
        static_cast<long double>(static_cast<float>(left[axis])) -
        static_cast<long double>(static_cast<float>(right[axis]));
    result += delta * delta;
  }
  return static_cast<double>(static_cast<float>(result));
}

double projected_authored_frame(
    const mh::content::MotionKeyframe &first,
    const mh::content::MotionKeyframe &second,
    const std::array<double, 3U> &world_position,
    const double first_frame, const double second_frame) {
  std::array<float, 3U> segment{};
  std::array<float, 3U> relative{};
  auto dot = 0.0L;
  auto squared_length = 0.0L;
  for (std::size_t axis = 0U; axis < segment.size(); ++axis) {
    segment[axis] = static_cast<float>(
        static_cast<float>(second.position[axis]) -
        static_cast<float>(first.position[axis]));
    relative[axis] = static_cast<float>(
        static_cast<float>(world_position[axis]) -
        static_cast<float>(first.position[axis]));
    dot += static_cast<long double>(segment[axis]) *
           static_cast<long double>(relative[axis]);
    squared_length += static_cast<long double>(segment[axis]) *
                      static_cast<long double>(segment[axis]);
  }
  if (squared_length == 0.0L) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const auto fraction =
      static_cast<float>(dot / squared_length);
  if (fraction < 0.0F || fraction >= 1.0F) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return static_cast<double>(static_cast<float>(
      static_cast<long double>(static_cast<float>(first_frame)) +
      (static_cast<long double>(static_cast<float>(second_frame)) -
       static_cast<long double>(static_cast<float>(first_frame))) *
          static_cast<long double>(fraction)));
}

double wrap_motion_frame(const mh::content::MotionData &motion,
                         double authored_frame) {
  const auto first =
      static_cast<double>(motion.keyframes.front().frame);
  const auto last =
      static_cast<double>(motion.keyframes.back().frame);
  const auto period = last - first;
  const auto closed =
      squared_distance(motion.keyframes.front().position,
                       motion.keyframes.back().position) <= 1.0e-8;
  if (!closed || period <= 0.0) {
    return std::clamp(authored_frame, first, last);
  }
  while (authored_frame < first) {
    authored_frame += period;
  }
  while (authored_frame > last) {
    authored_frame -= period;
  }
  return authored_frame;
}

double projected_motion_frame_impl(
    const mh::content::MotionData &motion,
    const std::array<double, 3U> &world_position,
    const std::size_t projected_key) {
  const auto closed =
      squared_distance(motion.keyframes.front().position,
                       motion.keyframes.back().position) <= 1.0e-8;
  const auto projection_count =
      closed ? motion.keyframes.size() - 1U : motion.keyframes.size();
  if (projection_count < 2U) {
    throw std::invalid_argument(
        "race spline projection requires two distinct authored keys");
  }

  if (projected_key >= projection_count) {
    throw std::invalid_argument(
        "race spline projection key is outside the authored motion");
  }
  const auto nearest = projected_key;

  const auto previous =
      (nearest + projection_count - 1U) % projection_count;
  const auto next = (nearest + 1U) % projection_count;
  const auto first_frame =
      static_cast<double>(motion.keyframes.front().frame);
  const auto last_frame =
      static_cast<double>(motion.keyframes.back().frame);
  const auto period = last_frame - first_frame;
  auto previous_frame =
      static_cast<double>(motion.keyframes[previous].frame);
  const auto nearest_frame =
      static_cast<double>(motion.keyframes[nearest].frame);
  auto next_frame = static_cast<double>(motion.keyframes[next].frame);
  if (nearest == 0U) {
    previous_frame -= period;
  }
  if (next == 0U) {
    next_frame += period;
  }

  auto projected = projected_authored_frame(
      motion.keyframes[previous], motion.keyframes[nearest], world_position,
      previous_frame, nearest_frame);
  if (!std::isfinite(projected)) {
    projected = projected_authored_frame(
        motion.keyframes[nearest], motion.keyframes[next], world_position,
        nearest_frame, next_frame);
  }
  return std::isfinite(projected) ? projected : nearest_frame;
}

double recovery_projected_frame(
    const mh::content::MotionData &motion,
    const std::array<double, 3U> &world_position) {
  if (motion.keyframes.size() < 2U ||
      std::any_of(world_position.begin(), world_position.end(),
                  [](const double value) { return !std::isfinite(value); })) {
    throw std::invalid_argument(
        "race recovery spline requires finite position and two keys");
  }

  auto nearest = std::size_t{0U};
  auto nearest_distance = std::numeric_limits<double>::infinity();
  for (std::size_t index = 0U; index < motion.keyframes.size(); ++index) {
    const auto distance =
        squared_distance(motion.keyframes[index].position, world_position);
    if (distance < nearest_distance) {
      nearest = index;
      nearest_distance = distance;
    }
  }

  const auto count = motion.keyframes.size();
  const auto previous = nearest == 0U ? count - 1U : nearest - 1U;
  const auto next = nearest + 1U == count ? 0U : nearest + 1U;
  const auto first_frame = static_cast<double>(motion.keyframes.front().frame);
  const auto last_frame = static_cast<double>(motion.keyframes.back().frame);
  const auto period = last_frame - first_frame;
  auto previous_frame = static_cast<double>(motion.keyframes[previous].frame);
  const auto nearest_frame =
      static_cast<double>(motion.keyframes[nearest].frame);
  auto next_frame = static_cast<double>(motion.keyframes[next].frame);
  if (nearest == 0U) {
    previous_frame -= period;
  }
  if (next == 0U) {
    next_frame += period;
  }

  auto projected = projected_authored_frame(
      motion.keyframes[previous], motion.keyframes[nearest], world_position,
      previous_frame, nearest_frame);
  if (!std::isfinite(projected)) {
    projected = projected_authored_frame(
        motion.keyframes[nearest], motion.keyframes[next], world_position,
        nearest_frame, next_frame);
  }
  return std::isfinite(projected) ? projected : nearest_frame;
}

} // namespace

OriginalBodyPoseState
make_original_track_pose(const mh::content::MotionPathFrame &frame,
                         const double local_up_offset) {
  return make_original_track_pose(
      frame, OriginalTrackPoseOffsets{0.0, local_up_offset, 0.0});
}

OriginalBodyPoseState
make_original_track_pose(const mh::content::MotionPathFrame &frame,
                         const OriginalTrackPoseOffsets &offsets) {
  if (!std::isfinite(offsets.lateral) || !std::isfinite(offsets.local_up) ||
      !std::isfinite(offsets.longitudinal)) {
    throw std::invalid_argument("track-pose offsets must be finite");
  }
  OriginalBodyPoseState result;
  result.body_basis = {frame.right, frame.up, frame.forward};
  for (std::size_t axis = 0U; axis < result.world_position.size(); ++axis) {
    result.world_position[axis] = frame.position[axis] +
                                  frame.right[axis] * offsets.lateral +
                                  frame.up[axis] * offsets.local_up +
                                  frame.forward[axis] * offsets.longitudinal;
  }
  return result;
}

OriginalTrackPoseOffsets
make_original_start_grid_offsets(const mh::content::TrackStartGrid &grid,
                                 const std::size_t slot_index) {
  if (slot_index >= 8U) {
    throw std::invalid_argument(
        "original start-grid slot must be within [0, 7]");
  }
  if (!std::isfinite(grid.row) || !std::isfinite(grid.goal) ||
      !std::isfinite(grid.space) || !std::isfinite(grid.tint) ||
      grid.row <= 0.0F || grid.goal <= 0.0F || grid.space <= 0.0F ||
      grid.tint <= 0.0F) {
    throw std::invalid_argument(
        "original start-grid parameters must be positive and finite");
  }
  const auto odd_slot = (slot_index & 1U) != 0U;
  const auto row_index = slot_index / 2U;
  return OriginalTrackPoseOffsets{
      odd_slot ? static_cast<double>(grid.space)
               : -static_cast<double>(grid.space),
      0.0,
      -static_cast<double>(grid.goal) -
          static_cast<double>(grid.row) * static_cast<double>(row_index) -
          (odd_slot ? static_cast<double>(grid.tint) : 0.0)};
}

std::array<double, 3U> original_race_recovery_spline_direction(
    const mh::content::MotionData &motion,
    const std::array<double, 3U> &world_position) {
  const auto projected = recovery_projected_frame(motion, world_position);

  auto half_span = 0.5F;
  std::array<float, 3U> chord{};
  auto chord_squared = 0.0F;
  for (std::size_t attempt = 0U; attempt < 100U; ++attempt) {
    const auto before = mh::content::motion_sample(
        motion, wrap_motion_frame(motion, projected - half_span));
    const auto after = mh::content::motion_sample(
        motion, wrap_motion_frame(motion, projected + half_span));
    auto extended_squared = 0.0L;
    for (std::size_t axis = 0U; axis < chord.size(); ++axis) {
      chord[axis] = static_cast<float>(
          static_cast<float>(after.position[axis]) -
          static_cast<float>(before.position[axis]));
      extended_squared += static_cast<long double>(chord[axis]) *
                          static_cast<long double>(chord[axis]);
    }
    chord_squared = static_cast<float>(extended_squared);
    if (static_cast<double>(chord_squared) >= 0.25) {
      break;
    }
    half_span = static_cast<float>(
        static_cast<double>(half_span) + static_cast<double>(0.5F));
  }
  if (chord_squared < 0.25F) {
    throw std::runtime_error(
        "race recovery spline has no usable local direction");
  }
  const auto inverse_length =
      static_cast<float>(1.0L /
                         std::sqrt(static_cast<long double>(chord_squared)));
  return {static_cast<double>(
              static_cast<float>(chord[0U] * inverse_length)),
          static_cast<double>(
              static_cast<float>(chord[1U] * inverse_length)),
          static_cast<double>(
              static_cast<float>(chord[2U] * inverse_length))};
}

std::array<double, 3U> race_recovery_spline_position(
    const mh::content::MotionData &motion,
    const std::array<double, 3U> &world_position) {
  const auto projected = recovery_projected_frame(motion, world_position);
  return mh::content::motion_sample(
             motion, wrap_motion_frame(motion, projected))
      .position;
}

double original_race_spline_engine_mix(
    const mh::content::MotionData &motion,
    const std::array<double, 3U> &world_position,
    const std::size_t projected_key) {
  if (motion.keyframes.size() < 3U ||
      std::any_of(world_position.begin(), world_position.end(),
                  [](const double value) { return !std::isfinite(value); })) {
    throw std::invalid_argument(
        "race spline engine mix requires finite position and a closed motion");
  }
  const auto authored_frame = original_race_projected_motion_frame(
      motion, world_position, projected_key);
  const auto sample = mh::content::motion_sample(
      motion, wrap_motion_frame(motion, authored_frame));
  const auto value = static_cast<float>(sample.rotation[0U]);
  if (!std::isfinite(value)) {
    throw std::runtime_error("race spline engine mix is not finite");
  }
  return static_cast<double>(value);
}

double original_race_projected_motion_frame(
    const mh::content::MotionData &motion,
    const std::array<double, 3U> &world_position,
    const std::size_t projected_key) {
  if (motion.keyframes.size() < 3U ||
      std::any_of(world_position.begin(), world_position.end(),
                  [](const double value) { return !std::isfinite(value); })) {
    throw std::invalid_argument(
        "race spline projection requires finite position and a closed motion");
  }
  return projected_motion_frame_impl(motion, world_position, projected_key);
}

} // namespace mh::game
