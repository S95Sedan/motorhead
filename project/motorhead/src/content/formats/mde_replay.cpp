#include <content/formats/mde_replay.hpp>

#include <core/error.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::uint32_t read_le32(const std::span<const std::uint8_t> bytes,
                        const std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U |
         static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U |
         static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U;
}

std::uint16_t read_le16(const std::span<const std::uint8_t> bytes,
                        const std::size_t offset) {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U;
}

std::string read_track_name(const std::span<const std::uint8_t> bytes) {
  constexpr std::size_t offset = 8U;
  constexpr std::size_t field_bytes = 16U;
  const auto end = std::find(bytes.begin() + offset,
                             bytes.begin() + offset + field_bytes, 0U);
  if (end == bytes.begin() + offset ||
      end == bytes.begin() + offset + field_bytes) {
    throw ToolError(ExitCode::format,
                    "MDE v3 track name is empty or lacks its terminator");
  }
  for (auto cursor = bytes.begin() + offset; cursor != end; ++cursor) {
    if (*cursor < 0x20U || *cursor > 0x7eU) {
      throw ToolError(ExitCode::format,
                      "MDE v3 track name is not printable ASCII");
    }
  }
  return {bytes.begin() + offset, end};
}

std::string read_fixed_ascii(const std::span<const std::uint8_t> bytes,
                             const std::size_t offset,
                             const std::size_t field_bytes,
                             const bool require_nonempty) {
  const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
  const auto limit = begin + static_cast<std::ptrdiff_t>(field_bytes);
  const auto end = std::find(begin, limit, 0U);
  if (end == limit || (require_nonempty && end == begin)) {
    throw ToolError(ExitCode::format,
                    "MDE v3 racer text is empty or lacks its terminator");
  }
  for (auto cursor = begin; cursor != end; ++cursor) {
    if (*cursor < 0x20U || *cursor > 0x7eU) {
      throw ToolError(ExitCode::format,
                      "MDE v3 racer text is not printable ASCII");
    }
  }
  return {begin, end};
}

} // namespace

MdeV3DecodedStreamState decode_mde_v3_stream_payload(
    const std::span<const std::uint8_t, mde_v3_stream_payload_bytes>
        packed_payload) {
  constexpr double nibble_scale = 1.0 / 15.0;
  constexpr double wheel_history_bias = 0.5;
  constexpr double signed_byte_scale = 1.0 / 127.0;
  constexpr double bounded_scale = 1.0 / 3.0;
  constexpr double bounded_bias = -40.0;
  constexpr double body_54_scale = 62.5;
  constexpr double half_step_scale = 0.5;
  constexpr double orientation_scale = 1.0 / 8192.0;

  MdeV3DecodedStreamState result;
  for (std::size_t index = 0U; index < result.wheel_history.size(); ++index) {
    const auto packed = packed_payload[index / 2U];
    const auto nibble =
        index % 2U == 0U ? packed & 0x0fU : (packed >> 4U) & 0x0fU;
    result.wheel_history[index] = static_cast<float>(
        static_cast<double>(nibble) * nibble_scale + wheel_history_bias);
  }
  result.body_d8_normalized =
      static_cast<float>((static_cast<double>(packed_payload[2U]) - 127.0) *
                         signed_byte_scale);
  result.body_e4_bounded =
      static_cast<float>(static_cast<double>(packed_payload[3U]) *
                             bounded_scale +
                         bounded_bias);
  result.body_e8_bounded =
      static_cast<float>(static_cast<double>(packed_payload[4U]) *
                             bounded_scale +
                         bounded_bias);
  result.body_54_scaled =
      static_cast<float>(static_cast<double>(packed_payload[5U]) *
                         body_54_scale);
  result.body_cc_half_step =
      static_cast<float>(static_cast<double>(packed_payload[6U] & 0x1fU) *
                         half_step_scale);
  result.body_4c_mode = packed_payload[6U] >> 5U;

  for (std::size_t index = 0U;
       index < result.normalized_auxiliary.size(); ++index) {
    const auto packed = packed_payload[7U + index / 2U];
    const auto nibble =
        index % 2U == 0U ? packed & 0x0fU : (packed >> 4U) & 0x0fU;
    result.normalized_auxiliary[index] =
        static_cast<float>(static_cast<double>(nibble) * nibble_scale);
  }

  result.body_170_mode = packed_payload[9U] & 0x07U;
  result.body_174_mode = (packed_payload[9U] >> 3U) & 0x07U;
  result.body_168_flag = ((packed_payload[9U] >> 6U) & 0x01U) != 0U;
  result.body_16c_flag = ((packed_payload[9U] >> 7U) & 0x01U) != 0U;
  result.body_188_mode = packed_payload[11U] & 0x03U;
  result.body_18c_mode = (packed_payload[11U] >> 2U) & 0x03U;
  result.body_flags =
      static_cast<std::uint32_t>(packed_payload[10U]) |
      (static_cast<std::uint32_t>(packed_payload[11U] & 0xf0U) << 4U);

  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    result.orientation_parameters[axis] = static_cast<float>(
        static_cast<double>(read_le16(packed_payload, 0x0cU + axis * 2U)) *
        orientation_scale);
    result.world_position[axis] = std::bit_cast<float>(
        read_le32(packed_payload, 0x12U + axis * 4U));
    if (!std::isfinite(result.world_position[axis])) {
      throw ToolError(ExitCode::format,
                      "MDE v3 stream position is not finite");
    }
  }
  return result;
}

MdeV3Data parse_mde(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < mde_v25_header_bytes) {
    throw ToolError(ExitCode::format, "MDE file is shorter than its header");
  }
  const auto version_bits = read_le32(bytes, 0U);
  const auto is_v25 =
      version_bits == std::bit_cast<std::uint32_t>(2.5F);
  const auto is_v3 = version_bits == std::bit_cast<std::uint32_t>(3.0F);
  if (!is_v25 && !is_v3) {
    throw ToolError(ExitCode::format,
                    "MDE parser supports only the proven p3.1 v2.5/v3 "
                    "framing");
  }
  const auto header_bytes =
      is_v25 ? mde_v25_header_bytes : mde_v3_header_bytes;
  if (bytes.size() < header_bytes + mde_v3_record_prefix_bytes +
                         mde_v3_stream_payload_bytes) {
    throw ToolError(ExitCode::format,
                    "MDE file is shorter than one framed record");
  }
  const auto stream_count = read_le32(bytes, 0x1cU);
  if (stream_count == 0U || stream_count > 13U) {
    throw ToolError(ExitCode::format,
                    "MDE stream count is outside the proven 1..13 range");
  }
  if (is_v25 && stream_count > mde_v25_racer_slots) {
    throw ToolError(ExitCode::format,
                    "MDE v2.5 stream count exceeds its eight racer slots");
  }
  const auto record_bytes =
      mde_v3_record_prefix_bytes +
      static_cast<std::size_t>(stream_count) * mde_v3_stream_payload_bytes;
  const auto payload_bytes = bytes.size() - header_bytes;
  if (payload_bytes % record_bytes != 0U) {
    throw ToolError(
        ExitCode::format,
        "MDE payload is not an exact variable-stream record array");
  }

  MdeV3Data result;
  result.version = is_v25 ? 2.5F : 3.0F;
  result.track_name = read_track_name(bytes);
  result.stream_count = stream_count;
  result.header_bytes = header_bytes;
  result.record_bytes = record_bytes;
  result.file_bytes = bytes.size();
  const auto racer_slot_count =
      is_v25 ? mde_v25_racer_slots : mde_v3_racer_slots;
  const auto racer_record_bytes =
      is_v25 ? mde_v25_racer_record_bytes : mde_v3_racer_record_bytes;
  auto active_racers = 0U;
  for (std::size_t slot = 0U; slot < racer_slot_count; ++slot) {
    constexpr std::size_t racer_array_offset = 0x24U;
    const auto offset = racer_array_offset + slot * racer_record_bytes;
    auto &racer = result.racer_slots[slot];
    racer.active = read_le32(bytes, offset) != 0U;
    racer.driver_name =
        read_fixed_ascii(bytes, offset + 0x04U, 16U, false);
    racer.team_name =
        read_fixed_ascii(bytes, offset + 0x14U, 5U, false);
    racer.car_name =
        read_fixed_ascii(bytes, offset + 0x19U, 16U, false);
    if (!is_v25) {
      racer.profile_name =
          read_fixed_ascii(bytes, offset + 0x29U, 16U, false);
    }
    const auto retained_offset = is_v25 ? 0x29U : 0x39U;
    for (std::size_t value = 0U; value < racer.retained_values.size();
         ++value) {
      racer.retained_values[value] =
          read_le32(bytes, offset + retained_offset + value * 4U);
    }
    active_racers += racer.active ? 1U : 0U;
  }
  if (active_racers != stream_count) {
    throw ToolError(
        ExitCode::format,
        "MDE active racer slots do not match its stream count");
  }
  const auto record_count = payload_bytes / record_bytes;
  result.records.reserve(record_count);
  std::uint32_t preceding_milliseconds = 0U;
  for (std::size_t index = 0U; index < record_count; ++index) {
    const auto offset = header_bytes + index * record_bytes;
    MdeV3Record record;
    record.flags = read_le32(bytes, offset);
    record.milliseconds = read_le32(bytes, offset + 4U);
    if (index != 0U && record.milliseconds < preceding_milliseconds) {
      throw ToolError(ExitCode::format, "MDE timestamps are not monotonic");
    }
    record.streams.resize(stream_count);
    for (std::size_t stream = 0U; stream < stream_count; ++stream) {
      const auto stream_offset = offset + mde_v3_record_prefix_bytes +
                                 stream * mde_v3_stream_payload_bytes;
      auto &sample = record.streams[stream];
      std::copy_n(
          std::next(bytes.begin(), static_cast<std::ptrdiff_t>(stream_offset)),
          sample.packed_payload.size(), sample.packed_payload.begin());
      sample.decoded = decode_mde_v3_stream_payload(sample.packed_payload);
      sample.world_position = sample.decoded.world_position;
    }
    result.records.push_back(record);
    preceding_milliseconds = record.milliseconds;
  }
  return result;
}

MdeV3Data read_mde(const std::filesystem::path &path,
                   const std::uint64_t maximum_file_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > maximum_file_bytes ||
      size >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw ToolError(ExitCode::input,
                    "MDE file is missing or exceeds the size limit");
  }
  std::ifstream input(path, std::ios::binary);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!input && !bytes.empty()) {
    throw ToolError(ExitCode::input, "failed to read MDE file");
  }
  return parse_mde(bytes);
}

MdeV3Data parse_mde_v3(const std::span<const std::uint8_t> bytes) {
  const auto result = parse_mde(bytes);
  if (result.version != 3.0F) {
    throw ToolError(ExitCode::format, "MDE is not a v3 recording");
  }
  return result;
}

MdeV3Data read_mde_v3(const std::filesystem::path &path,
                      const std::uint64_t maximum_file_bytes) {
  const auto result = read_mde(path, maximum_file_bytes);
  if (result.version != 3.0F) {
    throw ToolError(ExitCode::format, "MDE is not a v3 recording");
  }
  return result;
}

MdeV3PlaybackSample sample_mde_v3_stream(const MdeV3Data &demo,
                                         const std::size_t stream_index,
                                         const std::uint32_t milliseconds) {
  if (stream_index >= demo.stream_count || demo.records.empty()) {
    throw ToolError(ExitCode::format,
                    "MDE playback stream is empty or out of range");
  }

  auto upper = std::lower_bound(
      demo.records.begin(), demo.records.end(), milliseconds,
      [](const MdeV3Record &record, const std::uint32_t target) {
        return record.milliseconds < target;
      });
  if (upper == demo.records.begin()) {
    return {upper->streams[stream_index].decoded, {}, {}, 0U, 0U, 0.0F};
  }
  if (upper == demo.records.end()) {
    const auto final = demo.records.size() - 1U;
    return {demo.records[final].streams[stream_index].decoded,
            {},
            {},
            final,
            final,
            0.0F};
  }
  if (upper->milliseconds == milliseconds) {
    const auto exact =
        static_cast<std::size_t>(std::distance(demo.records.begin(), upper));
    return {upper->streams[stream_index].decoded,
            {},
            {},
            exact,
            exact,
            0.0F};
  }

  const auto upper_index =
      static_cast<std::size_t>(std::distance(demo.records.begin(), upper));
  const auto lower_index = upper_index - 1U;
  const auto &lower = demo.records[lower_index];
  const auto elapsed = milliseconds - lower.milliseconds;
  const auto interval = upper->milliseconds - lower.milliseconds;
  if (interval == 0U) {
    return {upper->streams[stream_index].decoded,
            {},
            {},
            upper_index,
            upper_index,
            0.0F};
  }
  const auto interpolation =
      static_cast<float>(static_cast<double>(elapsed) /
                         static_cast<double>(interval));
  const auto seconds = static_cast<double>(interval) * 0.001;
  const auto &left = lower.streams[stream_index].decoded;
  const auto &right = upper->streams[stream_index].decoded;

  MdeV3PlaybackSample result;
  result.state = left;
  result.lower_record = lower_index;
  result.upper_record = upper_index;
  result.interpolation = interpolation;
  const auto interpolate_float = [interpolation](const float first,
                                                 const float second) {
    return static_cast<float>(
        static_cast<double>(first) +
        (static_cast<double>(second) - static_cast<double>(first)) *
            static_cast<double>(interpolation));
  };
  for (std::size_t index = 0U; index < result.state.wheel_history.size();
       ++index) {
    result.state.wheel_history[index] =
        interpolate_float(left.wheel_history[index],
                          right.wheel_history[index]);
  }
  result.state.body_d8_normalized =
      interpolate_float(left.body_d8_normalized, right.body_d8_normalized);
  result.state.body_e4_bounded =
      interpolate_float(left.body_e4_bounded, right.body_e4_bounded);
  result.state.body_e8_bounded =
      interpolate_float(left.body_e8_bounded, right.body_e8_bounded);
  result.state.body_54_scaled =
      interpolate_float(left.body_54_scaled, right.body_54_scaled);
  result.state.body_cc_half_step =
      interpolate_float(left.body_cc_half_step, right.body_cc_half_step);
  for (std::size_t index = 0U;
       index < result.state.normalized_auxiliary.size(); ++index) {
    result.state.normalized_auxiliary[index] =
        interpolate_float(left.normalized_auxiliary[index],
                          right.normalized_auxiliary[index]);
  }

  constexpr double pi = 3.1415927410125732421875;
  constexpr double two_pi = 6.283185482025146484375;
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    auto first = static_cast<double>(left.orientation_parameters[axis]);
    auto second = static_cast<double>(right.orientation_parameters[axis]);
    const auto truncated_difference = std::trunc(second - first);
    if (std::abs(truncated_difference) > pi) {
      if (second >= first) {
        first += two_pi;
      } else {
        second += two_pi;
      }
    }
    const auto difference = second - first;
    result.state.orientation_parameters[axis] = static_cast<float>(
        first + difference * static_cast<double>(interpolation));
    result.orientation_velocity[axis] =
        static_cast<float>(difference / seconds);

    const auto position_difference =
        static_cast<double>(right.world_position[axis]) -
        static_cast<double>(left.world_position[axis]);
    result.state.world_position[axis] = static_cast<float>(
        static_cast<double>(left.world_position[axis]) +
        position_difference * static_cast<double>(interpolation));
    result.world_velocity[axis] =
        static_cast<float>(position_difference / seconds);
  }
  return result;
}

std::optional<std::uint32_t> first_mde_stream_displacement_milliseconds(
    const MdeV3Data &demo, const std::size_t stream_index,
    const float minimum_distance) {
  if (demo.records.empty() || stream_index >= demo.stream_count) {
    throw ToolError(ExitCode::format,
                    "MDE displacement query is empty or out of range");
  }
  if (!std::isfinite(minimum_distance) || minimum_distance <= 0.0F) {
    throw ToolError(ExitCode::format,
                    "MDE displacement distance must be finite and positive");
  }
  const auto &initial =
      demo.records.front().streams[stream_index].world_position;
  const auto threshold_squared = minimum_distance * minimum_distance;
  for (const auto &record : demo.records) {
    auto distance_squared = 0.0F;
    for (std::size_t axis = 0U; axis < initial.size(); ++axis) {
      const auto delta =
          record.streams[stream_index].world_position[axis] - initial[axis];
      distance_squared += delta * delta;
    }
    if (distance_squared >= threshold_squared) {
      return record.milliseconds;
    }
  }
  return std::nullopt;
}

MdePositionAlignment
align_mde_v3_positions(const MdeV3Data &demo, const std::size_t stream_index,
                       const std::span<const MdeTimedPosition> samples) {
  if (stream_index >= demo.stream_count || demo.records.empty() ||
      samples.empty()) {
    throw ToolError(ExitCode::format,
                    "MDE position alignment input is empty or out of range");
  }
  double preceding_seconds = -1.0;
  for (const auto &sample : samples) {
    if (!std::isfinite(sample.seconds) || sample.seconds < 0.0 ||
        sample.seconds < preceding_seconds ||
        !std::all_of(
            sample.world_position.begin(), sample.world_position.end(),
            [](const double value) { return std::isfinite(value); })) {
      throw ToolError(ExitCode::format,
                      "MDE alignment samples are non-finite or non-monotonic");
    }
    preceding_seconds = sample.seconds;
  }

  const auto duration_milliseconds = samples.back().seconds * 1000.0;
  auto best_squared_sum = std::numeric_limits<double>::infinity();
  MdePositionAlignment best;
  for (std::size_t start = 0U; start < demo.records.size(); ++start) {
    const auto final_time =
        static_cast<double>(demo.records[start].milliseconds) +
        duration_milliseconds;
    if (final_time > static_cast<double>(demo.records.back().milliseconds)) {
      break;
    }
    double squared_sum = 0.0;
    double maximum_error = 0.0;
    auto lower = demo.records.begin() + static_cast<std::ptrdiff_t>(start);
    for (const auto &sample : samples) {
      const auto target =
          static_cast<double>(demo.records[start].milliseconds) +
          sample.seconds * 1000.0;
      auto nearest = std::lower_bound(
          lower, demo.records.end(), target,
          [](const MdeV3Record &record, const double milliseconds) {
            return static_cast<double>(record.milliseconds) < milliseconds;
          });
      if (nearest == demo.records.end()) {
        nearest = std::prev(demo.records.end());
      } else if (nearest != lower) {
        const auto prior = std::prev(nearest);
        if (target - static_cast<double>(prior->milliseconds) <
            static_cast<double>(nearest->milliseconds) - target) {
          nearest = prior;
        }
      }
      lower = nearest;
      double sample_squared = 0.0;
      for (std::size_t axis = 0U; axis < 3U; ++axis) {
        const auto difference =
            sample.world_position[axis] -
            static_cast<double>(
                nearest->streams[stream_index].world_position[axis]);
        sample_squared += difference * difference;
      }
      squared_sum += sample_squared;
      maximum_error = std::max(maximum_error, std::sqrt(sample_squared));
    }
    if (squared_sum < best_squared_sum) {
      best_squared_sum = squared_sum;
      best = {
          stream_index,
          start,
          demo.records[start].milliseconds,
          samples.size(),
          std::sqrt(squared_sum / (static_cast<double>(samples.size()) * 3.0)),
          maximum_error};
    }
  }
  if (!std::isfinite(best_squared_sum)) {
    throw ToolError(ExitCode::format,
                    "MDE timeline is shorter than the alignment samples");
  }
  return best;
}

} // namespace mh::content
