#pragma once

#include <game/race/replay.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mh::game {

struct ReplayControlSegment {
  std::uint64_t first_frame_index = 0U;
  std::size_t frame_count = 0U;
  double elapsed_seconds = 0.0;
  ControlInput controls{};
};

// Produces maximal contiguous runs with bit-identical decoded controls. Input
// retains the strict zero-based frame and canonical control requirements of
// MHREPLAY; elapsed time is summed without applying the outer scheduler again.
[[nodiscard]] std::vector<ReplayControlSegment>
segment_replay_controls(std::span<const ReplayControlFrame> frames);

[[nodiscard]] bool
is_active_control_segment(const ReplayControlSegment &segment) noexcept;

} // namespace mh::game
