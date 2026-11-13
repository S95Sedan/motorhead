#pragma once

#include <game/physics/simulation.hpp>
#include <game/vehicle/scene.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mh::game {

struct ReplayControlFrame {
  std::uint64_t frame_index = 0U;
  double elapsed_seconds = 0.0;
  ControlInput controls{};
};

struct ReplayPhysicsSlice {
  std::uint64_t frame_index = 0U;
  std::size_t frame_slice_index = 0U;
  std::uint64_t timeline_slice_index = 0U;
  float slice_seconds = 0.0F;
  ControlInput controls{};
};

struct OriginalReplaySchedule {
  std::vector<ReplayPhysicsSlice> slices;
  double normalized_frame_seconds = 0.0;
  double simulated_seconds = 0.0;
  double discarded_seconds = 0.0;
};

// Replay frames are canonical rather than forgiving: indices start at zero
// and remain contiguous, while controls must already be in their normalized
// ranges. Each frame holds its controls across every recovered physics slice.
[[nodiscard]] OriginalReplaySchedule
schedule_original_replay(std::span<const ReplayControlFrame> frames);

inline constexpr std::uint16_t replay_container_version = 1U;

// Stable little-endian container for owned replay input records. Encoding is
// independent of compiler struct layout and includes a payload checksum.
[[nodiscard]] std::vector<std::uint8_t>
encode_replay_frames(std::span<const ReplayControlFrame> frames);

[[nodiscard]] std::vector<ReplayControlFrame>
decode_replay_frames(std::span<const std::uint8_t> bytes);

struct ReplayNumericTolerance {
  double absolute = 0.0;
  double relative = 0.0;
};

struct ReplayNumericComparison {
  double expected = 0.0;
  double actual = 0.0;
  double absolute_error = 0.0;
  double allowed_error = 0.0;
  bool within_tolerance = false;
};

[[nodiscard]] ReplayNumericComparison
compare_replay_numeric(double expected, double actual,
                       const ReplayNumericTolerance &tolerance);

struct VehicleStateTolerance {
  ReplayNumericTolerance position{};
  ReplayNumericTolerance heading{};
  ReplayNumericTolerance speed{};
  std::uint64_t tick = 0U;
};

struct VehicleStateComparison {
  ReplayNumericComparison position_x{};
  ReplayNumericComparison position_z{};
  ReplayNumericComparison heading{};
  ReplayNumericComparison speed{};
  std::uint64_t tick_error = 0U;
  bool within_tolerance = false;
};

[[nodiscard]] VehicleStateComparison
compare_vehicle_state(const VehicleState &expected, const VehicleState &actual,
                      const VehicleStateTolerance &tolerance);

struct OriginalVehicleReplaySnapshot {
  std::uint64_t timeline_slice_index = 0U;
  double simulation_seconds = 0.0;
  OriginalVehicleSceneState state{};
};

inline constexpr std::uint16_t vehicle_snapshot_container_version = 1U;

[[nodiscard]] std::vector<std::uint8_t> encode_vehicle_snapshots(
    std::span<const OriginalVehicleReplaySnapshot> snapshots);

[[nodiscard]] std::vector<OriginalVehicleReplaySnapshot>
decode_vehicle_snapshots(std::span<const std::uint8_t> bytes);

struct OriginalVehicleSnapshotTolerance {
  ReplayNumericTolerance simulation_time{};
  ReplayNumericTolerance position{};
  ReplayNumericTolerance basis{};
  ReplayNumericTolerance local_linear_velocity{};
  ReplayNumericTolerance local_angular_velocity{};
  std::uint64_t slice_index = 0U;
};

struct OriginalVehicleSnapshotComparison {
  ReplayNumericComparison simulation_time{};
  std::array<ReplayNumericComparison, 3U> position{};
  std::array<ReplayNumericComparison, 9U> basis{};
  std::array<ReplayNumericComparison, 3U> local_linear_velocity{};
  std::array<ReplayNumericComparison, 3U> local_angular_velocity{};
  std::uint64_t slice_index_error = 0U;
  bool within_tolerance = false;
};

[[nodiscard]] OriginalVehicleSnapshotComparison
compare_vehicle_snapshot(const OriginalVehicleReplaySnapshot &expected,
                         const OriginalVehicleReplaySnapshot &actual,
                         const OriginalVehicleSnapshotTolerance &tolerance);

} // namespace mh::game
