#include <game/race/replay.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace mh::game {
namespace {

constexpr double pi = 3.141592653589793238462643383279502884;
constexpr std::array<std::uint8_t, 8U> replay_magic{'M', 'H', 'R', 'E',
                                                    'P', 'L', 'A', 'Y'};
constexpr std::array<std::uint8_t, 8U> snapshot_magic{'M', 'H', 'S', 'T',
                                                      'A', 'T', 'E', '1'};
constexpr std::size_t replay_header_bytes = 40U;
constexpr std::size_t replay_record_bytes = 40U;
constexpr std::size_t snapshot_header_bytes = 40U;
constexpr std::size_t snapshot_record_bytes = 160U;
constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

void require_finite(const double value, const char *name) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(name) + " must be finite");
  }
}

void validate_controls(const ControlInput &controls) {
  require_finite(controls.throttle, "replay throttle");
  require_finite(controls.brake, "replay brake");
  require_finite(controls.steering, "replay steering");
  if (controls.throttle < -1.0 || controls.throttle > 1.0 ||
      controls.brake < 0.0 || controls.brake > 1.0 ||
      controls.steering < -1.0 || controls.steering > 1.0) {
    throw std::invalid_argument("replay controls are not normalized");
  }
}

void validate_replay_frames(const std::span<const ReplayControlFrame> frames) {
  for (std::size_t index = 0U; index < frames.size(); ++index) {
    const auto &frame = frames[index];
    if (frame.frame_index != index) {
      throw std::invalid_argument(
          "replay frame indices must start at zero and remain contiguous");
    }
    require_finite(frame.elapsed_seconds, "replay elapsed seconds");
    validate_controls(frame.controls);
  }
}

void validate_scene_state(const OriginalVehicleSceneState &state) {
  for (const auto &row : state.pose.body_basis) {
    for (const auto value : row) {
      require_finite(value, "snapshot body basis");
    }
  }
  for (const auto value : state.pose.world_position) {
    require_finite(value, "snapshot world position");
  }
  for (const auto value : state.velocity.local_linear) {
    require_finite(value, "snapshot local linear velocity");
  }
  for (const auto value : state.velocity.local_angular) {
    require_finite(value, "snapshot local angular velocity");
  }
}

void validate_snapshots(
    const std::span<const OriginalVehicleReplaySnapshot> snapshots) {
  std::uint64_t preceding_index = 0U;
  double preceding_time = 0.0;
  for (std::size_t index = 0U; index < snapshots.size(); ++index) {
    const auto &snapshot = snapshots[index];
    require_finite(snapshot.simulation_seconds, "snapshot simulation seconds");
    if (snapshot.simulation_seconds < 0.0 ||
        (index != 0U && (snapshot.timeline_slice_index <= preceding_index ||
                         snapshot.simulation_seconds < preceding_time))) {
      throw std::invalid_argument(
          "snapshot indices and time must increase monotonically");
    }
    validate_scene_state(snapshot.state);
    preceding_index = snapshot.timeline_slice_index;
    preceding_time = snapshot.simulation_seconds;
  }
}

void append_u16(std::vector<std::uint8_t> &output, const std::uint16_t value) {
  output.push_back(static_cast<std::uint8_t>(value));
  output.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_u32(std::vector<std::uint8_t> &output, const std::uint32_t value) {
  for (std::size_t byte = 0U; byte < 4U; ++byte) {
    output.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
  }
}

void append_u64(std::vector<std::uint8_t> &output, const std::uint64_t value) {
  for (std::size_t byte = 0U; byte < 8U; ++byte) {
    output.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
  }
}

void append_double(std::vector<std::uint8_t> &output, const double value) {
  append_u64(output, std::bit_cast<std::uint64_t>(value));
}

std::uint64_t checksum(const std::span<const std::uint8_t> bytes) noexcept {
  auto value = fnv_offset_basis;
  for (const auto byte : bytes) {
    value ^= byte;
    value *= fnv_prime;
  }
  return value;
}

class ByteReader {
public:
  explicit ByteReader(const std::span<const std::uint8_t> bytes)
      : bytes_(bytes) {}

  [[nodiscard]] std::uint16_t read_u16() {
    require(2U);
    const auto value = static_cast<std::uint16_t>(bytes_[offset_]) |
                       static_cast<std::uint16_t>(bytes_[offset_ + 1U]) << 8U;
    offset_ += 2U;
    return value;
  }

  [[nodiscard]] std::uint32_t read_u32() {
    require(4U);
    std::uint32_t value = 0U;
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
      value |= static_cast<std::uint32_t>(bytes_[offset_ + byte])
               << (byte * 8U);
    }
    offset_ += 4U;
    return value;
  }

  [[nodiscard]] std::uint64_t read_u64() {
    require(8U);
    std::uint64_t value = 0U;
    for (std::size_t byte = 0U; byte < 8U; ++byte) {
      value |= static_cast<std::uint64_t>(bytes_[offset_ + byte])
               << (byte * 8U);
    }
    offset_ += 8U;
    return value;
  }

  [[nodiscard]] double read_double() {
    return std::bit_cast<double>(read_u64());
  }

private:
  void require(const std::size_t count) const {
    if (count > bytes_.size() - offset_) {
      throw std::invalid_argument("replay container is truncated");
    }
  }

  std::span<const std::uint8_t> bytes_;
  std::size_t offset_ = 0U;
};

std::uint64_t absolute_tick_difference(const std::uint64_t left,
                                       const std::uint64_t right) noexcept {
  return left >= right ? left - right : right - left;
}

} // namespace

OriginalReplaySchedule
schedule_original_replay(const std::span<const ReplayControlFrame> frames) {
  validate_replay_frames(frames);
  OriginalReplaySchedule result;
  result.slices.reserve(frames.size());

  for (std::size_t index = 0U; index < frames.size(); ++index) {
    const auto &frame = frames[index];
    const auto frame_schedule =
        make_original_physics_slice_schedule(frame.elapsed_seconds);
    result.normalized_frame_seconds += frame_schedule.frame_elapsed_seconds;
    result.simulated_seconds += frame_schedule.simulated_seconds;
    result.discarded_seconds += frame_schedule.discarded_seconds;

    for (std::size_t slice_index = 0U; slice_index < frame_schedule.slice_count;
         ++slice_index) {
      result.slices.push_back({frame.frame_index, slice_index,
                               static_cast<std::uint64_t>(result.slices.size()),
                               frame_schedule.slices[slice_index],
                               frame.controls});
    }
  }
  return result;
}

std::vector<std::uint8_t>
encode_replay_frames(const std::span<const ReplayControlFrame> frames) {
  validate_replay_frames(frames);
  if (frames.size() >
      (std::numeric_limits<std::size_t>::max() - replay_header_bytes) /
          replay_record_bytes) {
    throw std::length_error("replay frame count is too large");
  }

  std::vector<std::uint8_t> payload;
  payload.reserve(frames.size() * replay_record_bytes);
  for (const auto &frame : frames) {
    append_u64(payload, frame.frame_index);
    append_double(payload, frame.elapsed_seconds);
    append_double(payload, frame.controls.throttle);
    append_double(payload, frame.controls.brake);
    append_double(payload, frame.controls.steering);
  }

  std::vector<std::uint8_t> output;
  output.reserve(replay_header_bytes + payload.size());
  output.insert(output.end(), replay_magic.begin(), replay_magic.end());
  append_u16(output, replay_container_version);
  append_u16(output, static_cast<std::uint16_t>(replay_header_bytes));
  append_u32(output, static_cast<std::uint32_t>(replay_record_bytes));
  append_u64(output, static_cast<std::uint64_t>(frames.size()));
  append_u64(output, static_cast<std::uint64_t>(payload.size()));
  append_u64(output, checksum(payload));
  output.insert(output.end(), payload.begin(), payload.end());
  return output;
}

std::vector<ReplayControlFrame>
decode_replay_frames(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < replay_header_bytes ||
      !std::equal(replay_magic.begin(), replay_magic.end(), bytes.begin())) {
    throw std::invalid_argument("replay container magic or header is invalid");
  }

  ByteReader header(bytes.subspan(replay_magic.size(),
                                  replay_header_bytes - replay_magic.size()));
  const auto version = header.read_u16();
  const auto header_bytes = header.read_u16();
  const auto record_bytes = header.read_u32();
  const auto frame_count = header.read_u64();
  const auto payload_bytes = header.read_u64();
  const auto expected_checksum = header.read_u64();
  if (version != replay_container_version ||
      header_bytes != replay_header_bytes ||
      record_bytes != replay_record_bytes) {
    throw std::invalid_argument(
        "replay container version or layout is unsupported");
  }
  if (frame_count > std::numeric_limits<std::size_t>::max() ||
      payload_bytes > std::numeric_limits<std::size_t>::max() ||
      frame_count >
          std::numeric_limits<std::uint64_t>::max() / replay_record_bytes ||
      payload_bytes != frame_count * replay_record_bytes ||
      payload_bytes != bytes.size() - replay_header_bytes) {
    throw std::invalid_argument("replay container size fields are invalid");
  }

  const auto payload = bytes.subspan(replay_header_bytes);
  if (checksum(payload) != expected_checksum) {
    throw std::invalid_argument("replay container checksum changed");
  }

  std::vector<ReplayControlFrame> frames;
  frames.reserve(static_cast<std::size_t>(frame_count));
  ByteReader records(payload);
  for (std::uint64_t index = 0U; index < frame_count; ++index) {
    frames.push_back({records.read_u64(),
                      records.read_double(),
                      {records.read_double(), records.read_double(),
                       records.read_double()}});
  }
  validate_replay_frames(frames);
  return frames;
}

std::vector<std::uint8_t> encode_vehicle_snapshots(
    const std::span<const OriginalVehicleReplaySnapshot> snapshots) {
  validate_snapshots(snapshots);
  if (snapshots.size() >
      (std::numeric_limits<std::size_t>::max() - snapshot_header_bytes) /
          snapshot_record_bytes) {
    throw std::length_error("vehicle snapshot count is too large");
  }

  std::vector<std::uint8_t> payload;
  payload.reserve(snapshots.size() * snapshot_record_bytes);
  for (const auto &snapshot : snapshots) {
    append_u64(payload, snapshot.timeline_slice_index);
    append_double(payload, snapshot.simulation_seconds);
    for (const auto &row : snapshot.state.pose.body_basis) {
      for (const auto value : row) {
        append_double(payload, value);
      }
    }
    for (const auto value : snapshot.state.pose.world_position) {
      append_double(payload, value);
    }
    for (const auto value : snapshot.state.velocity.local_linear) {
      append_double(payload, value);
    }
    for (const auto value : snapshot.state.velocity.local_angular) {
      append_double(payload, value);
    }
  }

  std::vector<std::uint8_t> output;
  output.reserve(snapshot_header_bytes + payload.size());
  output.insert(output.end(), snapshot_magic.begin(), snapshot_magic.end());
  append_u16(output, vehicle_snapshot_container_version);
  append_u16(output, static_cast<std::uint16_t>(snapshot_header_bytes));
  append_u32(output, static_cast<std::uint32_t>(snapshot_record_bytes));
  append_u64(output, static_cast<std::uint64_t>(snapshots.size()));
  append_u64(output, static_cast<std::uint64_t>(payload.size()));
  append_u64(output, checksum(payload));
  output.insert(output.end(), payload.begin(), payload.end());
  return output;
}

std::vector<OriginalVehicleReplaySnapshot>
decode_vehicle_snapshots(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < snapshot_header_bytes ||
      !std::equal(snapshot_magic.begin(), snapshot_magic.end(),
                  bytes.begin())) {
    throw std::invalid_argument(
        "vehicle snapshot container magic or header is invalid");
  }

  ByteReader header(bytes.subspan(
      snapshot_magic.size(), snapshot_header_bytes - snapshot_magic.size()));
  const auto version = header.read_u16();
  const auto header_bytes = header.read_u16();
  const auto record_bytes = header.read_u32();
  const auto snapshot_count = header.read_u64();
  const auto payload_bytes = header.read_u64();
  const auto expected_checksum = header.read_u64();
  if (version != vehicle_snapshot_container_version ||
      header_bytes != snapshot_header_bytes ||
      record_bytes != snapshot_record_bytes) {
    throw std::invalid_argument(
        "vehicle snapshot container version or layout is unsupported");
  }
  if (snapshot_count > std::numeric_limits<std::size_t>::max() ||
      payload_bytes > std::numeric_limits<std::size_t>::max() ||
      snapshot_count >
          std::numeric_limits<std::uint64_t>::max() / snapshot_record_bytes ||
      payload_bytes != snapshot_count * snapshot_record_bytes ||
      payload_bytes != bytes.size() - snapshot_header_bytes) {
    throw std::invalid_argument(
        "vehicle snapshot container size fields are invalid");
  }

  const auto payload = bytes.subspan(snapshot_header_bytes);
  if (checksum(payload) != expected_checksum) {
    throw std::invalid_argument("vehicle snapshot container checksum changed");
  }

  std::vector<OriginalVehicleReplaySnapshot> snapshots;
  snapshots.reserve(static_cast<std::size_t>(snapshot_count));
  ByteReader records(payload);
  for (std::uint64_t index = 0U; index < snapshot_count; ++index) {
    OriginalVehicleReplaySnapshot snapshot;
    snapshot.timeline_slice_index = records.read_u64();
    snapshot.simulation_seconds = records.read_double();
    for (auto &row : snapshot.state.pose.body_basis) {
      for (auto &value : row) {
        value = records.read_double();
      }
    }
    for (auto &value : snapshot.state.pose.world_position) {
      value = records.read_double();
    }
    for (auto &value : snapshot.state.velocity.local_linear) {
      value = records.read_double();
    }
    for (auto &value : snapshot.state.velocity.local_angular) {
      value = records.read_double();
    }
    snapshots.push_back(snapshot);
  }
  validate_snapshots(snapshots);
  return snapshots;
}

ReplayNumericComparison
compare_replay_numeric(const double expected, const double actual,
                       const ReplayNumericTolerance &tolerance) {
  require_finite(expected, "expected replay value");
  require_finite(actual, "actual replay value");
  require_finite(tolerance.absolute, "absolute replay tolerance");
  require_finite(tolerance.relative, "relative replay tolerance");
  if (tolerance.absolute < 0.0 || tolerance.relative < 0.0) {
    throw std::invalid_argument("replay tolerances must be non-negative");
  }

  const auto absolute_error = std::fabs(actual - expected);
  const auto allowed_error =
      std::max(tolerance.absolute, std::fabs(expected) * tolerance.relative);
  return {expected, actual, absolute_error, allowed_error,
          absolute_error <= allowed_error};
}

VehicleStateComparison
compare_vehicle_state(const VehicleState &expected, const VehicleState &actual,
                      const VehicleStateTolerance &tolerance) {
  VehicleStateComparison comparison;
  comparison.position_x = compare_replay_numeric(
      expected.position_x, actual.position_x, tolerance.position);
  comparison.position_z = compare_replay_numeric(
      expected.position_z, actual.position_z, tolerance.position);
  comparison.heading = compare_replay_numeric(
      0.0,
      std::remainder(actual.heading_radians - expected.heading_radians,
                     2.0 * pi),
      tolerance.heading);
  comparison.speed =
      compare_replay_numeric(expected.speed, actual.speed, tolerance.speed);
  comparison.tick_error = absolute_tick_difference(expected.tick, actual.tick);
  comparison.within_tolerance = comparison.position_x.within_tolerance &&
                                comparison.position_z.within_tolerance &&
                                comparison.heading.within_tolerance &&
                                comparison.speed.within_tolerance &&
                                comparison.tick_error <= tolerance.tick;
  return comparison;
}

OriginalVehicleSnapshotComparison
compare_vehicle_snapshot(const OriginalVehicleReplaySnapshot &expected,
                         const OriginalVehicleReplaySnapshot &actual,
                         const OriginalVehicleSnapshotTolerance &tolerance) {
  require_finite(expected.simulation_seconds,
                 "expected snapshot simulation seconds");
  require_finite(actual.simulation_seconds,
                 "actual snapshot simulation seconds");
  if (expected.simulation_seconds < 0.0 || actual.simulation_seconds < 0.0) {
    throw std::invalid_argument(
        "snapshot simulation seconds must be non-negative");
  }
  validate_scene_state(expected.state);
  validate_scene_state(actual.state);

  OriginalVehicleSnapshotComparison comparison;
  comparison.simulation_time = compare_replay_numeric(
      expected.simulation_seconds, actual.simulation_seconds,
      tolerance.simulation_time);
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    comparison.position[axis] = compare_replay_numeric(
        expected.state.pose.world_position[axis],
        actual.state.pose.world_position[axis], tolerance.position);
    comparison.local_linear_velocity[axis] =
        compare_replay_numeric(expected.state.velocity.local_linear[axis],
                               actual.state.velocity.local_linear[axis],
                               tolerance.local_linear_velocity);
    comparison.local_angular_velocity[axis] =
        compare_replay_numeric(expected.state.velocity.local_angular[axis],
                               actual.state.velocity.local_angular[axis],
                               tolerance.local_angular_velocity);
    for (std::size_t component = 0U; component < 3U; ++component) {
      const auto basis_index = axis * 3U + component;
      comparison.basis[basis_index] = compare_replay_numeric(
          expected.state.pose.body_basis[axis][component],
          actual.state.pose.body_basis[axis][component], tolerance.basis);
    }
  }
  comparison.slice_index_error = absolute_tick_difference(
      expected.timeline_slice_index, actual.timeline_slice_index);

  const auto all_within = [](const auto &values) {
    return std::all_of(values.begin(), values.end(), [](const auto &value) {
      return value.within_tolerance;
    });
  };
  comparison.within_tolerance =
      comparison.simulation_time.within_tolerance &&
      all_within(comparison.position) && all_within(comparison.basis) &&
      all_within(comparison.local_linear_velocity) &&
      all_within(comparison.local_angular_velocity) &&
      comparison.slice_index_error <= tolerance.slice_index;
  return comparison;
}

} // namespace mh::game
