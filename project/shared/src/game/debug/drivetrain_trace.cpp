#include <game/debug/drivetrain_trace.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mh::game {
namespace {

constexpr std::array<std::uint8_t, 8U> magic{'M', 'H', 'D', 'R',
                                             'I', 'V', 'E', '1'};
constexpr std::size_t header_bytes = 40U;
constexpr std::size_t record_bytes =
    20U + original_drivetrain_capture_bytes + original_control_capture_bytes;
constexpr std::size_t legacy_record_bytes =
    20U + legacy_drivetrain_capture_bytes + original_control_capture_bytes;
constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

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

std::uint64_t read_u64(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) {
  std::uint64_t value = 0U;
  for (std::size_t byte = 0U; byte < 8U; ++byte) {
    value |= static_cast<std::uint64_t>(bytes[offset + byte]) << (byte * 8U);
  }
  return value;
}

std::uint32_t read_u32(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) {
  std::uint32_t value = 0U;
  for (std::size_t byte = 0U; byte < 4U; ++byte) {
    value |= static_cast<std::uint32_t>(bytes[offset + byte]) << (byte * 8U);
  }
  return value;
}

std::uint16_t read_u16(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U;
}

float read_float(const std::span<const std::uint8_t> bytes,
                 const std::size_t offset) {
  return std::bit_cast<float>(read_u32(bytes, offset));
}

std::uint64_t checksum(const std::span<const std::uint8_t> bytes) noexcept {
  auto value = fnv_offset_basis;
  for (const auto byte : bytes) {
    value ^= byte;
    value *= fnv_prime;
  }
  return value;
}

void validate_records(
    const std::span<const OriginalDrivetrainTraceRecord> records) {
  std::uint64_t previous_index = 0U;
  double previous_time = 0.0;
  for (std::size_t index = 0U; index < records.size(); ++index) {
    const auto &record = records[index];
    if (!std::isfinite(record.simulation_seconds) ||
        record.simulation_seconds < 0.0 ||
        (record.captured_drivetrain_bytes !=
             original_drivetrain_capture_bytes &&
         record.captured_drivetrain_bytes != legacy_drivetrain_capture_bytes) ||
        (index != 0U && (record.timeline_slice_index <= previous_index ||
                         record.simulation_seconds < previous_time))) {
      throw std::invalid_argument(
          "drivetrain trace indices and time must increase monotonically");
    }
    previous_index = record.timeline_slice_index;
    previous_time = record.simulation_seconds;
  }
}

} // namespace

std::vector<std::uint8_t> encode_drivetrain_trace(
    const std::span<const OriginalDrivetrainTraceRecord> records) {
  validate_records(records);
  if (!std::all_of(records.begin(), records.end(), [](const auto &record) {
        return record.captured_drivetrain_bytes ==
               original_drivetrain_capture_bytes;
      })) {
    throw std::invalid_argument(
        "encoding legacy-width drivetrain records is unsupported");
  }
  if (records.size() >
      (std::numeric_limits<std::size_t>::max() - header_bytes) / record_bytes) {
    throw std::length_error("drivetrain trace record count is too large");
  }
  std::vector<std::uint8_t> payload;
  payload.reserve(records.size() * record_bytes);
  for (const auto &record : records) {
    append_u64(payload, record.timeline_slice_index);
    append_u64(payload,
               std::bit_cast<std::uint64_t>(record.simulation_seconds));
    append_u32(payload, record.entry_ebx);
    payload.insert(payload.end(), record.bytes.begin(), record.bytes.end());
    payload.insert(payload.end(), record.control_bytes.begin(),
                   record.control_bytes.end());
  }

  std::vector<std::uint8_t> output;
  output.reserve(header_bytes + payload.size());
  output.insert(output.end(), magic.begin(), magic.end());
  append_u16(output, drivetrain_trace_container_version);
  append_u16(output, static_cast<std::uint16_t>(header_bytes));
  append_u32(output, static_cast<std::uint32_t>(record_bytes));
  append_u64(output, static_cast<std::uint64_t>(records.size()));
  append_u64(output, static_cast<std::uint64_t>(payload.size()));
  append_u64(output, checksum(payload));
  output.insert(output.end(), payload.begin(), payload.end());
  return output;
}

std::vector<OriginalDrivetrainTraceRecord>
decode_drivetrain_trace(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < header_bytes ||
      !std::equal(magic.begin(), magic.end(), bytes.begin())) {
    throw std::invalid_argument(
        "drivetrain trace container magic or header is invalid");
  }
  const auto version = read_u16(bytes, 8U);
  const auto encoded_header_bytes = read_u16(bytes, 10U);
  const auto encoded_record_bytes = read_u32(bytes, 12U);
  const auto count = read_u64(bytes, 16U);
  const auto payload_bytes = read_u64(bytes, 24U);
  const auto expected_checksum = read_u64(bytes, 32U);
  const auto legacy_layout =
      version == 1U && encoded_record_bytes == legacy_record_bytes;
  const auto current_layout =
      version == drivetrain_trace_container_version &&
      encoded_record_bytes == record_bytes;
  if (encoded_header_bytes != header_bytes ||
      (!legacy_layout && !current_layout)) {
    throw std::invalid_argument(
        "drivetrain trace container version or layout is unsupported");
  }
  if (count > std::numeric_limits<std::size_t>::max() ||
      payload_bytes > std::numeric_limits<std::size_t>::max() ||
      count > std::numeric_limits<std::uint64_t>::max() /
                  encoded_record_bytes ||
      payload_bytes != count * encoded_record_bytes ||
      payload_bytes != bytes.size() - header_bytes) {
    throw std::invalid_argument(
        "drivetrain trace container size fields are invalid");
  }
  const auto payload = bytes.subspan(header_bytes);
  if (checksum(payload) != expected_checksum) {
    throw std::invalid_argument("drivetrain trace container checksum changed");
  }

  std::vector<OriginalDrivetrainTraceRecord> records;
  records.reserve(static_cast<std::size_t>(count));
  for (std::size_t index = 0U; index < count; ++index) {
    const auto offset = index * encoded_record_bytes;
    OriginalDrivetrainTraceRecord record;
    record.timeline_slice_index = read_u64(payload, offset);
    record.simulation_seconds =
        std::bit_cast<double>(read_u64(payload, offset + 8U));
    record.entry_ebx = read_u32(payload, offset + 16U);
    record.captured_drivetrain_bytes =
        legacy_layout ? legacy_drivetrain_capture_bytes
                      : original_drivetrain_capture_bytes;
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(offset + 20U),
                record.captured_drivetrain_bytes, record.bytes.begin());
    std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(
                                      offset + 20U +
                                      record.captured_drivetrain_bytes),
                record.control_bytes.size(), record.control_bytes.begin());
    records.push_back(record);
  }
  validate_records(records);
  return records;
}

OriginalDrivetrainTraceState
inspect_drivetrain_trace_record(const OriginalDrivetrainTraceRecord &record) {
  const std::span<const std::uint8_t> bytes(record.bytes);
  const std::span<const std::uint8_t> controls(record.control_bytes);
  OriginalDrivetrainTraceState state;
  state.control_flags = read_u32(controls, 0x04U);
  state.control_a = read_float(controls, 0x08U);
  state.control_b = read_float(controls, 0x0cU);
  state.steering = read_float(controls, 0x10U);
  state.external_transition_active = (record.entry_ebx & 0x01U) != 0U;
  state.maximum_gear_index = read_u32(bytes, 0x00U);
  if (state.maximum_gear_index >= state.gear_values.size()) {
    throw std::invalid_argument("drivetrain gear-table bound is invalid");
  }
  for (std::size_t index = 0U; index <= state.maximum_gear_index; ++index) {
    state.gear_values[index] = read_float(bytes, 0x04U + index * 4U);
  }
  state.authored_minimum = read_float(bytes, 0x20U);
  state.authored_maximum = read_float(bytes, 0x24U);
  state.authored_brake_force = read_float(bytes, 0x28U);
  state.authored_turn_force = read_float(bytes, 0x2cU);
  state.steering_ramp_limit = read_float(bytes, 0x30U);
  state.authored_acceleration_force = read_float(bytes, 0x34U);
  state.alternate_mode = read_u32(bytes, 0x38U) != 0U;
  state.drive_mode_selector = read_u32(bytes, 0x8cU);
  state.primary_drive_candidate_active = read_u32(bytes, 0x98U) != 0U;
  state.current_gear_index = read_u32(bytes, 0x4cU);
  state.engine_scalar = read_float(bytes, 0x54U);
  state.engine_transition_active = read_u32(bytes, 0x9cU) != 0U;
  state.reference_value = read_float(bytes, 0xd4U);
  state.first_shift_latched = read_u32(bytes, 0x58U) != 0U;
  state.second_shift_latched = read_u32(bytes, 0x5cU) != 0U;
  state.suppress_negative_body_reversal = read_u32(bytes, 0x90U) != 0U;
  state.direction_transition_active = read_u32(bytes, 0xa0U) != 0U;
  state.transition_source_a_active = read_u32(bytes, 0x13cU) != 0U;
  state.transition_source_b_active = read_u32(bytes, 0x140U) != 0U;
  for (std::size_t index = 0U;
       index < state.retained_wheel_contact_flags.size(); ++index) {
    state.retained_wheel_contact_flags[index] =
        read_u32(bytes, 0x134U + index * sizeof(std::uint32_t));
    if (state.retained_wheel_contact_flags[index] > 1U) {
      throw std::invalid_argument(
          "retained wheel-contact flag is outside 0..1");
    }
  }
  state.longitudinal_damping_modifier_index =
      std::bit_cast<std::int32_t>(read_u32(bytes, 0x154U));
  for (std::size_t index = 0U; index < state.previous_wheel_states.size();
       ++index) {
    const auto offset = 0x158U + index * sizeof(float);
    if (offset + sizeof(float) <= record.captured_drivetrain_bytes) {
      state.previous_wheel_states[index] = read_float(bytes, offset);
    }
  }
  state.complete_wheel_history =
      record.captured_drivetrain_bytes >= original_drivetrain_capture_bytes;
  const std::array finite_values{state.control_a,
                                 state.control_b,
                                 state.steering,
                                 state.authored_minimum,
                                 state.authored_maximum,
                                 state.authored_brake_force,
                                 state.authored_turn_force,
                                 state.steering_ramp_limit,
                                 state.authored_acceleration_force,
                                 state.engine_scalar,
                                 state.reference_value,
                                 state.previous_wheel_states[0U],
                                 state.previous_wheel_states[1U]};
  if (!std::all_of(finite_values.begin(), finite_values.end(),
                   [](const double value) { return std::isfinite(value); }) ||
      state.current_gear_index > state.maximum_gear_index) {
    throw std::invalid_argument("drivetrain trace state is invalid");
  }
  if (state.complete_wheel_history &&
      !std::all_of(state.previous_wheel_states.begin(),
                   state.previous_wheel_states.end(), [](const double value) {
                     return std::isfinite(value) && value >= 0.0 &&
                            value <= 1.0;
                   })) {
    throw std::invalid_argument("complete wheel history is invalid");
  }
  for (std::size_t index = 0U; index <= state.maximum_gear_index; ++index) {
    if (!std::isfinite(state.gear_values[index])) {
      throw std::invalid_argument("drivetrain gear value is non-finite");
    }
  }
  return state;
}

} // namespace mh::game
