#include <game/race/course_import.hpp>

#include <content/formats/ai_route.hpp>
#include <content/formats/motion_path.hpp>
#include <game/vehicle/motion_import.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace mh::game {
namespace {

std::size_t advance_index(const std::size_t index, const int step,
                          const std::size_t size) {
  return step > 0 ? (index + 1U) % size : (index + size - 1U) % size;
}

double distance(const mh::content::AiRouteSample &first,
                const mh::content::AiRouteSample &second) {
  return std::hypot(static_cast<double>(second.position[0U]) -
                        static_cast<double>(first.position[0U]),
                    static_cast<double>(second.position[1U]) -
                        static_cast<double>(first.position[1U]));
}

DirectionalRaceGate2D make_gate(const mh::content::AiRouteData &route,
                                const std::size_t sample_index,
                                const int traversal_step) {
  const auto &sample = route.samples[sample_index];
  auto next_index =
      advance_index(sample_index, traversal_step, route.samples.size());
  while (next_index != sample_index &&
         distance(sample, route.samples[next_index]) <= 1.0e-7) {
    next_index =
        advance_index(next_index, traversal_step, route.samples.size());
  }
  const auto &next = route.samples[next_index];
  const auto delta_x = static_cast<double>(next.position[0U]) -
                       static_cast<double>(sample.position[0U]);
  const auto delta_z = static_cast<double>(next.position[1U]) -
                       static_cast<double>(sample.position[1U]);
  const auto length = std::hypot(delta_x, delta_z);
  if (length <= 1.0e-7) {
    throw std::invalid_argument("AI route has no usable gate tangent");
  }
  constexpr double gate_margin = 1.5;
  const auto half_width =
      std::max({std::abs(static_cast<double>(sample.surface_values[0U])),
                std::abs(static_cast<double>(sample.surface_values[1U])),
                4.0}) +
      gate_margin;
  return {{sample.position[0U], sample.position[1U]},
          {delta_x / length, delta_z / length}, half_width};
}

std::array<float, 3U>
float_position(const std::array<double, 3U> &position) {
  return {static_cast<float>(position[0U]),
          static_cast<float>(position[1U]),
          static_cast<float>(position[2U])};
}

float squared_distance(const std::array<float, 3U> &left,
                       const std::array<float, 3U> &right) {
  auto result = 0.0F;
  for (std::size_t axis = 0U; axis < left.size(); ++axis) {
    const auto delta = static_cast<float>(left[axis] - right[axis]);
    result = static_cast<float>(
        result + static_cast<float>(delta * delta));
  }
  return result;
}

} // namespace

RouteDerivedRaceCourse make_route_derived_race_course(
    const mh::content::AiRouteData &route,
    const OriginalBodyPoseState &start_pose,
    const std::size_t checkpoint_count) {
  if (route.samples.size() < 3U || checkpoint_count == 0U ||
      checkpoint_count > 64U ||
      checkpoint_count + 1U >= route.samples.size()) {
    throw std::invalid_argument(
        "route-derived race course has unsupported cardinality");
  }

  auto start_sample = std::size_t{0U};
  auto nearest_squared = std::numeric_limits<double>::infinity();
  for (std::size_t index = 0U; index < route.samples.size(); ++index) {
    const auto delta_x = static_cast<double>(route.samples[index].position[0U]) -
                         start_pose.world_position[0U];
    const auto delta_z = static_cast<double>(route.samples[index].position[1U]) -
                         start_pose.world_position[2U];
    const auto squared = delta_x * delta_x + delta_z * delta_z;
    if (squared < nearest_squared) {
      nearest_squared = squared;
      start_sample = index;
    }
  }
  const auto &start = route.samples[start_sample];
  const auto alignment =
      static_cast<double>(start.direction[0U]) *
          start_pose.body_basis[2U][0U] +
      static_cast<double>(start.direction[2U]) *
          start_pose.body_basis[2U][2U];
  const auto traversal_step = alignment >= 0.0 ? 1 : -1;

  std::vector<std::size_t> traversal;
  std::vector<double> cumulative;
  traversal.reserve(route.samples.size() + 1U);
  cumulative.reserve(route.samples.size() + 1U);
  traversal.push_back(start_sample);
  cumulative.push_back(0.0);
  auto current = start_sample;
  for (std::size_t edge = 0U; edge < route.samples.size(); ++edge) {
    const auto next =
        advance_index(current, traversal_step, route.samples.size());
    cumulative.push_back(cumulative.back() +
                         distance(route.samples[current], route.samples[next]));
    traversal.push_back(next);
    current = next;
  }
  const auto total_length = cumulative.back();
  if (!std::isfinite(total_length) || total_length <= 1.0) {
    throw std::invalid_argument("AI route loop length is invalid");
  }

  RouteDerivedRaceCourse result;
  result.start_sample = start_sample;
  result.traversal_step = traversal_step;
  result.course.finish = make_gate(route, start_sample, traversal_step);
  result.course.checkpoints.reserve(checkpoint_count);
  result.checkpoint_samples.reserve(checkpoint_count);
  for (std::size_t checkpoint = 1U; checkpoint <= checkpoint_count;
       ++checkpoint) {
    const auto target = total_length * static_cast<double>(checkpoint) /
                        static_cast<double>(checkpoint_count + 1U);
    const auto found =
        std::lower_bound(cumulative.begin(), cumulative.end(), target);
    const auto traversal_index = static_cast<std::size_t>(
        std::distance(cumulative.begin(), found));
    const auto sample_index = traversal[traversal_index];
    result.checkpoint_samples.push_back(sample_index);
    result.course.checkpoints.push_back(
        make_gate(route, sample_index, traversal_step));
  }
  return result;
}

OriginalSplineRaceProgress::OriginalSplineRaceProgress(
    const mh::content::MotionData &motion, const std::size_t lap_count)
    : motion_(&motion), target_laps_(lap_count) {
  if (lap_count == 0U || motion.keyframes.size() < 2U) {
    throw std::invalid_argument(
        "original spline race requires laps and at least two motion keys");
  }
  const auto first_frame = motion.keyframes.front().frame;
  const auto last_frame = motion.keyframes.back().frame;
  if (first_frame != 0 || last_frame <= first_frame ||
      last_frame > 1'000'000) {
    throw std::invalid_argument(
        "original spline race requires a bounded frame-zero motion");
  }

  frame_positions_.reserve(motion.keyframes.size());
  for (const auto &keyframe : motion.keyframes) {
    if (std::any_of(keyframe.position.begin(), keyframe.position.end(),
                    [](const double value) { return !std::isfinite(value); })) {
      throw std::invalid_argument(
          "original spline race motion contains a non-finite key");
    }
    frame_positions_.push_back(float_position(keyframe.position));
  }
  if (frame_positions_.size() < 32U) {
    throw std::invalid_argument(
        "original spline race motion has fewer than two authored-key sectors");
  }

  // The motion object's +0x18 field is its authored-key count: sub_000715bc
  // indexes the variable-width key records with it, and sub_00071e7c stores
  // the selected key index at motion +0x20. The race owner divides that same
  // key count by 16 at RVA 0x0003baec and allocates one byte per key at
  // RVA 0x0003bf58.
  visited_.assign(frame_positions_.size() >> 4U, std::uint8_t{0U});
  if (visited_.size() < 2U) {
    throw std::invalid_argument(
        "original spline race motion produced too few sectors");
  }

  const auto direction = original_race_recovery_spline_direction(
      motion, motion.keyframes.front().position);
  finish_normal_ = float_position(direction);
  const auto &origin = frame_positions_.front();
  finish_offset_ = static_cast<float>(
      -static_cast<float>(
          static_cast<float>(origin[0U] * finish_normal_[0U]) +
          static_cast<float>(origin[1U] * finish_normal_[1U]) +
          static_cast<float>(origin[2U] * finish_normal_[2U])));
}

std::size_t OriginalSplineRaceProgress::nearest_key(
    const std::array<double, 3U> &world_position) {
  const auto position = float_position(world_position);

  // The wrapper at RVA 0x00072250 temporarily decrements the authored-key
  // count before projection so the duplicated terminal key is excluded.
  const auto projection_count = frame_positions_.size() - 1U;
  const auto distance_at = [&](const std::size_t frame) {
    return squared_distance(frame_positions_[frame], position);
  };

  // The caller at RVA 0x0003b986 passes the vehicle's persistent +0xf4
  // projected-key field. sub_0003b710 initializes it to -1; sub_000715bc
  // performs a global nearest-key search in that state, then updates and
  // reuses the key as a bidirectional local-search hint.
  if (!has_frame_hint_) {
    auto best = std::size_t{0U};
    auto best_distance = distance_at(best);
    for (std::size_t candidate = 1U; candidate < projection_count;
         ++candidate) {
      const auto candidate_distance = distance_at(candidate);
      if (candidate_distance < best_distance) {
        best = candidate;
        best_distance = candidate_distance;
      }
    }
    current_key_ = best;
    has_frame_hint_ = true;
    return best;
  }

  auto best = current_key_ % projection_count;
  auto best_distance = distance_at(best);
  for (std::size_t step = 0U; step < projection_count; ++step) {
    const auto candidate_frame = (best + 1U) % projection_count;
    const auto candidate_distance = distance_at(candidate_frame);
    if (candidate_distance > best_distance) {
      break;
    }
    best = candidate_frame;
    best_distance = candidate_distance;
  }
  for (std::size_t step = 0U; step < projection_count; ++step) {
    const auto candidate_frame =
        (best + projection_count - 1U) % projection_count;
    const auto candidate_distance = distance_at(candidate_frame);
    if (candidate_distance > best_distance) {
      break;
    }
    best = candidate_frame;
    best_distance = candidate_distance;
  }
  current_key_ = best;
  return best;
}

double OriginalSplineRaceProgress::signed_finish_distance(
    const std::array<double, 3U> &world_position) const {
  const auto position = float_position(world_position);
  return static_cast<double>(static_cast<float>(
      static_cast<float>(
          static_cast<float>(position[0U] * finish_normal_[0U]) +
          static_cast<float>(position[1U] * finish_normal_[1U])) +
      static_cast<float>(position[2U] * finish_normal_[2U]) +
      finish_offset_));
}

OriginalSplineRaceUpdate OriginalSplineRaceProgress::update(
    const std::array<double, 3U> &world_position) {
  if (std::any_of(world_position.begin(), world_position.end(),
                  [](const double value) { return !std::isfinite(value); })) {
    throw std::invalid_argument(
        "original spline race position must be finite");
  }
  if (complete_) {
    const auto key = nearest_key(world_position);
    previous_projected_frame_ = original_race_projected_motion_frame(
        *motion_, world_position, key);
    previous_position_ = float_position(world_position);
    return {false, true, current_lap_, current_key_, current_sector_,
            visited_sector_count_, visited_.size()};
  }

  const auto previous_finish_distance =
      signed_finish_distance({previous_position_[0U],
                              previous_position_[1U],
                              previous_position_[2U]});
  const auto current_finish_distance =
      signed_finish_distance(world_position);
  const auto key = nearest_key(world_position);
  const auto projected_frame = original_race_projected_motion_frame(
      *motion_, world_position, key);
  const auto checkpoint_marker = static_cast<long>(std::lround(
      static_cast<double>(static_cast<float>(
          static_cast<float>(motion_->keyframes[key].rotation[1U]) + 1.0F))));
  auto checkpoint_crossed = false;
  auto checkpoint_id = std::uint32_t{0U};
  if (checkpoint_marker > 0L && checkpoint_marker < 4L &&
      previous_projected_frame_ <
          static_cast<double>(motion_->keyframes[key].frame) &&
      projected_frame >= static_cast<double>(motion_->keyframes[key].frame) &&
      !recorded_checkpoints_[static_cast<std::size_t>(checkpoint_marker)]) {
    checkpoint_id = static_cast<std::uint32_t>(checkpoint_marker);
    recorded_checkpoints_[checkpoint_id] = true;
    checkpoint_crossed = true;
  }
  previous_projected_frame_ = projected_frame;
  auto sector = key >> 4U;
  if (sector >= visited_.size()) {
    sector = visited_.size() - 1U;
  }

  // This is the adjacency/undo owner at RVAs 0x0003baf5-0x0003bb8e.
  if (sector == 0U) {
    visited_[0U] = 1U;
    if (visited_sector_count_ == 0U) {
      ++visited_sector_count_;
    }
  }
  const auto next_sector = (current_sector_ + 1U) % visited_.size();
  if (sector == next_sector && visited_[current_sector_] != 0U) {
    visited_[sector] = 1U;
    ++visited_sector_count_;
  }
  const auto previous_sector =
      (current_sector_ + visited_.size() - 1U) % visited_.size();
  if (sector == previous_sector) {
    visited_[current_sector_] = 0U;
    if (visited_sector_count_ != 0U) {
      --visited_sector_count_;
    }
  }
  current_sector_ = sector;

  auto lap_completed = false;
  if (previous_finish_distance < 0.0 && current_finish_distance >= 0.0 &&
      visited_sector_count_ >= visited_.size()) {
    lap_completed = true;
    ++current_lap_;
    std::fill(visited_.begin(), visited_.end(), std::uint8_t{0U});
    recorded_checkpoints_.fill(false);
    visited_sector_count_ = 0U;
    complete_ = target_laps_ < current_lap_;
  }
  previous_position_ = float_position(world_position);
  return {lap_completed, complete_, current_lap_, current_key_,
          current_sector_, visited_sector_count_, visited_.size(),
          checkpoint_crossed, checkpoint_id};
}

} // namespace mh::game
