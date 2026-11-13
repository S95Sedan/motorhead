#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mh::game {

inline constexpr std::uint16_t drivetrain_trace_container_version = 2U;
inline constexpr std::size_t legacy_drivetrain_capture_bytes = 352U;
inline constexpr std::size_t original_drivetrain_capture_bytes = 360U;
inline constexpr std::size_t original_control_capture_bytes = 36U;

// Lossless private-oracle record. Keeping the bounded original bytes avoids
// assigning semantics to fields before their producers have been proven.
struct OriginalDrivetrainTraceRecord {
  std::uint64_t timeline_slice_index = 0U;
  double simulation_seconds = 0.0;
  std::uint32_t entry_ebx = 0U;
  std::array<std::uint8_t, original_drivetrain_capture_bytes> bytes{};
  std::array<std::uint8_t, original_control_capture_bytes> control_bytes{};
  std::size_t captured_drivetrain_bytes = original_drivetrain_capture_bytes;
};

// Structured view of only the fields already locked by the p3.1 instruction
// audit. The maximum gear index at +0x00 bounds an inclusive table at +0x04.
struct OriginalDrivetrainTraceState {
  std::uint32_t control_flags = 0U;
  double control_a = 0.0;
  double control_b = 0.0;
  double steering = 0.0;
  bool external_transition_active = false;
  std::uint32_t maximum_gear_index = 0U;
  std::array<double, 8U> gear_values{};
  double authored_minimum = 0.0;
  double authored_maximum = 0.0;
  double authored_brake_force = 0.0;
  double authored_turn_force = 0.0;
  double steering_ramp_limit = 0.0;
  double authored_acceleration_force = 0.0;
  bool alternate_mode = false;
  std::uint32_t drive_mode_selector = 0U;
  bool primary_drive_candidate_active = false;
  std::uint32_t current_gear_index = 0U;
  double engine_scalar = 0.0;
  bool engine_transition_active = false;
  double reference_value = 0.0;
  bool first_shift_latched = false;
  bool second_shift_latched = false;
  bool suppress_negative_body_reversal = false;
  bool direction_transition_active = false;
  bool transition_source_a_active = false;
  bool transition_source_b_active = false;
  // The contact owner sums these four flags before its pre-wheel load-transfer
  // and velocity-stabilizer gates, then clears them for the next outer frame.
  std::array<std::uint32_t, 4U> retained_wheel_contact_flags{};
  // The grounded-car owner checks this retained state against -1 and applies
  // its separate longitudinal damping modifier when any index is active.
  std::int32_t longitudinal_damping_modifier_index = -1;
  std::array<double, 4U> previous_wheel_states{};
  bool complete_wheel_history = false;
};

[[nodiscard]] std::vector<std::uint8_t>
encode_drivetrain_trace(std::span<const OriginalDrivetrainTraceRecord> records);

[[nodiscard]] std::vector<OriginalDrivetrainTraceRecord>
decode_drivetrain_trace(std::span<const std::uint8_t> bytes);

[[nodiscard]] OriginalDrivetrainTraceState
inspect_drivetrain_trace_record(const OriginalDrivetrainTraceRecord &record);

} // namespace mh::game
