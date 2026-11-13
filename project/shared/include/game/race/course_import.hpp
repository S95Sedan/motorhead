#pragma once

#include <game/physics/body_pose.hpp>
#include <game/race/race_state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mh::content {
struct AiRouteData;
struct MotionData;
}

namespace mh::game {

struct RouteDerivedRaceCourse {
  RaceCourse2D course{};
  std::size_t start_sample = 0U;
  int traversal_step = 1;
  std::vector<std::size_t> checkpoint_samples;
};

// Builds deterministic race gates from the authored circular AI route. This is
// a reconstruction integration boundary, not yet a claim that these are the
// retail game's original checkpoint records.
[[nodiscard]] RouteDerivedRaceCourse make_route_derived_race_course(
    const mh::content::AiRouteData &route,
    const OriginalBodyPoseState &start_pose,
    std::size_t checkpoint_count = 7U);

struct OriginalSplineRaceUpdate {
  bool lap_completed = false;
  bool race_completed = false;
  std::size_t current_lap = 1U;
  std::size_t current_key = 0U;
  std::size_t current_sector = 0U;
  std::size_t visited_sectors = 0U;
  std::size_t sector_count = 0U;
  bool checkpoint_crossed = false;
  std::uint32_t checkpoint_id = 0U;
};

// Reconstructs the p3.1 race-progress owner at RVA 0x0003b950. The retail
// owner consumes the track definition's SplineName motion, projects onto its
// authored keys, divides the circular key sequence into 16-key sectors, and
// requires adjacent forward traversal of every sector before a forward
// crossing of the frame-zero plane can complete a lap.
class OriginalSplineRaceProgress {
public:
  OriginalSplineRaceProgress(const mh::content::MotionData &motion,
                             std::size_t lap_count);

  [[nodiscard]] OriginalSplineRaceUpdate
  update(const std::array<double, 3U> &world_position);

  [[nodiscard]] std::size_t current_lap() const noexcept {
    return current_lap_;
  }
  [[nodiscard]] std::size_t current_key() const noexcept {
    return current_key_;
  }
  [[nodiscard]] std::size_t current_sector() const noexcept {
    return current_sector_;
  }
  [[nodiscard]] std::size_t visited_sectors() const noexcept {
    return visited_sector_count_;
  }
  [[nodiscard]] std::size_t sector_count() const noexcept {
    return visited_.size();
  }
  [[nodiscard]] bool complete() const noexcept { return complete_; }

private:
  [[nodiscard]] std::size_t
  nearest_key(const std::array<double, 3U> &world_position);
  [[nodiscard]] double
  signed_finish_distance(const std::array<double, 3U> &world_position) const;

  std::vector<std::array<float, 3U>> frame_positions_;
  const mh::content::MotionData *motion_ = nullptr;
  std::vector<std::uint8_t> visited_;
  std::array<bool, 4U> recorded_checkpoints_{};
  std::array<float, 3U> finish_normal_{};
  float finish_offset_ = 0.0F;
  std::array<float, 3U> previous_position_{};
  std::size_t target_laps_ = 1U;
  std::size_t current_lap_ = 1U;
  std::size_t current_key_ = 0U;
  std::size_t current_sector_ = 0U;
  std::size_t visited_sector_count_ = 0U;
  double previous_projected_frame_ = 0.0;
  bool has_frame_hint_ = false;
  bool complete_ = false;
};

} // namespace mh::game
