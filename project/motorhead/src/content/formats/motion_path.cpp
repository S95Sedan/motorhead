#include <content/formats/motion_path.hpp>

#include <core/error.hpp>
#include <content/formats/tbl_envelope.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::uint32_t supported_version = 1U;
constexpr std::uint32_t supported_value_count = 9U;
constexpr std::uint32_t maximum_keyframes = 100000U;

std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path,
                                     const std::uint64_t maximum_file_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > maximum_file_bytes ||
      size >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw ToolError(ExitCode::input,
                    "motion file is missing or exceeds the size limit");
  }
  std::ifstream input(path, std::ios::binary);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!input && !bytes.empty()) {
    throw ToolError(ExitCode::input, "failed to read motion file");
  }
  return bytes;
}

double read_finite(std::istringstream &input, const char *field) {
  double value = 0.0;
  if (!(input >> value) || !std::isfinite(value)) {
    throw ToolError(ExitCode::format,
                    std::string("motion ") + field + " must be finite");
  }
  return value;
}

std::int32_t read_i32(std::istringstream &input, const char *field) {
  std::int64_t value = 0;
  if (!(input >> value) || value < std::numeric_limits<std::int32_t>::min() ||
      value > std::numeric_limits<std::int32_t>::max()) {
    throw ToolError(ExitCode::format, std::string("motion ") + field +
                                          " is outside signed 32-bit range");
  }
  return static_cast<std::int32_t>(value);
}

std::array<double, 3U> subtract(const std::array<double, 3U> &left,
                                const std::array<double, 3U> &right) {
  return {left[0U] - right[0U], left[1U] - right[1U], left[2U] - right[2U]};
}

double squared_length(const std::array<double, 3U> &value) {
  return value[0U] * value[0U] + value[1U] * value[1U] + value[2U] * value[2U];
}

std::array<double, 3U> normalized(const std::array<double, 3U> &value,
                                  const char *message) {
  const auto length_squared = squared_length(value);
  if (!std::isfinite(length_squared) || length_squared <= 1.0e-18) {
    throw ToolError(ExitCode::format, message);
  }
  const auto inverse_length = 1.0 / std::sqrt(length_squared);
  return {value[0U] * inverse_length, value[1U] * inverse_length,
          value[2U] * inverse_length};
}

std::array<double, 3U> cross(const std::array<double, 3U> &left,
                             const std::array<double, 3U> &right) {
  return {left[1U] * right[2U] - left[2U] * right[1U],
          left[2U] * right[0U] - left[0U] * right[2U],
          left[0U] * right[1U] - left[1U] * right[0U]};
}

} // namespace

MotionData parse_motion(const std::span<const std::uint8_t> decoded_text) {
  if (decoded_text.empty() ||
      std::find(decoded_text.begin(), decoded_text.end(), 0U) !=
          decoded_text.end()) {
    throw ToolError(ExitCode::format,
                    "motion text is empty or contains a NUL byte");
  }
  const std::string text(decoded_text.begin(), decoded_text.end());
  std::istringstream input(text);
  std::string signature;
  std::uint64_t version = 0U;
  std::uint64_t value_count = 0U;
  std::uint64_t keyframe_count = 0U;
  if (!(input >> signature >> version >> value_count >> keyframe_count) ||
      signature != "LWMO" || version != supported_version ||
      value_count != supported_value_count || keyframe_count == 0U ||
      keyframe_count > maximum_keyframes) {
    throw ToolError(ExitCode::format,
                    "motion header is outside the supported LWMO contract");
  }

  MotionData result;
  result.version = static_cast<std::uint32_t>(version);
  result.value_count = static_cast<std::uint32_t>(value_count);
  result.keyframes.reserve(static_cast<std::size_t>(keyframe_count));
  for (std::uint64_t index = 0U; index < keyframe_count; ++index) {
    MotionKeyframe keyframe;
    for (auto &component : keyframe.position) {
      component = read_finite(input, "position");
    }
    for (auto &component : keyframe.rotation) {
      component = read_finite(input, "rotation");
    }
    for (auto &component : keyframe.attributes) {
      component = read_finite(input, "attribute");
    }
    keyframe.frame = read_i32(input, "frame");
    keyframe.linear = read_i32(input, "linear flag");
    keyframe.tension = read_finite(input, "tension");
    keyframe.continuity = read_finite(input, "continuity");
    keyframe.bias = read_finite(input, "bias");
    result.keyframes.push_back(keyframe);
  }
  std::string trailing;
  if (input >> trailing) {
    throw ToolError(ExitCode::format, "motion text contains trailing records");
  }
  return result;
}

MotionData read_motion(const std::filesystem::path &path,
                       const std::uint64_t maximum_file_bytes) {
  const auto bytes = read_bytes(path, maximum_file_bytes);
  const auto wrapped = bytes.size() >= 4U && bytes[0U] == 'T' &&
                       bytes[1U] == 'B' && bytes[2U] == 'L';
  if (!wrapped) {
    return parse_motion(bytes);
  }
  const auto envelope =
      read_tbl(path, "EQ", maximum_file_bytes, maximum_file_bytes);
  if (!envelope.decoded) {
    throw ToolError(ExitCode::format, "motion TBL payload is not decoded: " +
                                          envelope.decode_status);
  }
  auto result = parse_motion(envelope.payload);
  result.tbl_wrapped = true;
  return result;
}

MotionPathFrame motion_path_frame(const MotionData &motion,
                                  const std::size_t keyframe_index) {
  if (keyframe_index >= motion.keyframes.size()) {
    throw ToolError(ExitCode::format,
                    "motion path-frame index is outside the keyframe array");
  }
  const auto &keyframe = motion.keyframes[keyframe_index];
  std::array<double, 3U> direction{};
  bool found_direction = false;
  // Installed race paths repeat their first point as the final key. The
  // original parameter-zero spline matrix therefore receives both the
  // incoming and outgoing spans. A one-sided first-to-second chord rotates
  // rear StartGrid rows sideways on gently curving start straights.
  constexpr double closed_endpoint_epsilon_squared = 1.0e-6;
  const auto closed_path =
      motion.keyframes.size() >= 4U &&
      squared_length(subtract(motion.keyframes.front().position,
                              motion.keyframes.back().position)) <=
          closed_endpoint_epsilon_squared;
  if (closed_path &&
      (keyframe_index == 0U ||
       keyframe_index + 1U == motion.keyframes.size())) {
    auto next = std::size_t{1U};
    while (next + 1U < motion.keyframes.size() &&
           squared_length(subtract(motion.keyframes[next].position,
                                   motion.keyframes.front().position)) <=
               1.0e-18) {
      ++next;
    }
    auto previous = motion.keyframes.size() - 2U;
    while (previous > 0U &&
           squared_length(subtract(motion.keyframes[previous].position,
                                   motion.keyframes.front().position)) <=
               1.0e-18) {
      --previous;
    }
    direction = subtract(motion.keyframes[next].position,
                         motion.keyframes[previous].position);
    found_direction = squared_length(direction) > 1.0e-18;
  }
  for (auto index = keyframe_index + 1U;
       !found_direction && index < motion.keyframes.size(); ++index) {
    direction = subtract(motion.keyframes[index].position, keyframe.position);
    if (squared_length(direction) > 1.0e-18) {
      found_direction = true;
      break;
    }
  }
  if (!found_direction) {
    for (auto index = keyframe_index; index > 0U; --index) {
      direction =
          subtract(keyframe.position, motion.keyframes[index - 1U].position);
      if (squared_length(direction) > 1.0e-18) {
        found_direction = true;
        break;
      }
    }
  }
  if (!found_direction) {
    throw ToolError(ExitCode::format,
                    "motion path has no distinct point for a tangent");
  }

  MotionPathFrame result;
  result.keyframe_index = keyframe_index;
  result.authored_frame = keyframe.frame;
  result.position = keyframe.position;
  result.forward = normalized(direction, "motion path tangent is degenerate");
  constexpr std::array<double, 3U> world_up{0.0, 1.0, 0.0};
  result.right = normalized(cross(world_up, result.forward),
                            "motion path tangent is parallel to world up");
  result.up = normalized(cross(result.forward, result.right),
                         "motion path up axis is degenerate");
  return result;
}

MotionSample motion_sample(const MotionData &motion,
                           const double authored_frame) {
  if (motion.keyframes.empty() || !std::isfinite(authored_frame)) {
    throw ToolError(ExitCode::format,
                    "motion sample requires keys and a finite frame");
  }
  if (!std::is_sorted(
          motion.keyframes.begin(), motion.keyframes.end(),
          [](const MotionKeyframe &left, const MotionKeyframe &right) {
            return left.frame < right.frame;
          })) {
    throw ToolError(ExitCode::format,
                    "motion sample keyframes are not frame ordered");
  }

  const auto sample_key = [authored_frame](const MotionKeyframe &key) {
    return MotionSample{authored_frame, key.position, key.rotation,
                        key.attributes};
  };
  if (authored_frame <= static_cast<double>(motion.keyframes.front().frame)) {
    return sample_key(motion.keyframes.front());
  }
  if (authored_frame >= static_cast<double>(motion.keyframes.back().frame)) {
    return sample_key(motion.keyframes.back());
  }

  const auto upper = std::upper_bound(
      motion.keyframes.begin(), motion.keyframes.end(), authored_frame,
      [](const double frame, const MotionKeyframe &key) {
        return frame < static_cast<double>(key.frame);
      });
  const auto &right = *upper;
  const auto &left = *(upper - 1);
  const auto frame_span = static_cast<double>(right.frame - left.frame);
  if (frame_span <= 0.0) {
    throw ToolError(ExitCode::format,
                    "motion sample segment has no positive frame span");
  }
  const auto alpha =
      (authored_frame - static_cast<double>(left.frame)) / frame_span;
  const auto interpolate_linear =
      [alpha](const std::array<double, 3U> &first,
              const std::array<double, 3U> &second) {
    std::array<double, 3U> result{};
    for (std::size_t axis = 0U; axis < result.size(); ++axis) {
      result[axis] = first[axis] + (second[axis] - first[axis]) * alpha;
    }
    return result;
  };
  if (right.linear != 0) {
    return {authored_frame,
            interpolate_linear(left.position, right.position),
            interpolate_linear(left.rotation, right.rotation),
            interpolate_linear(left.attributes, right.attributes)};
  }

  const auto left_index =
      static_cast<std::size_t>(std::distance(motion.keyframes.begin(), upper) -
                               1);
  const auto interpolate_tcb =
      [&](const std::array<double, 3U> &first,
          const std::array<double, 3U> &second,
          const std::array<double, 3U> MotionKeyframe::*member) {
        std::array<double, 3U> result{};
        const auto alpha_squared = alpha * alpha;
        const auto alpha_cubed = alpha_squared * alpha;
        const auto h1 = 2.0 * alpha_cubed - 3.0 * alpha_squared + 1.0;
        const auto h2 = -2.0 * alpha_cubed + 3.0 * alpha_squared;
        const auto h3 = alpha_cubed - 2.0 * alpha_squared + alpha;
        const auto h4 = alpha_cubed - alpha_squared;
        const auto outgoing_a = (1.0 - left.tension) *
                                (1.0 + left.continuity) * (1.0 + left.bias);
        const auto outgoing_b = (1.0 - left.tension) *
                                (1.0 - left.continuity) * (1.0 - left.bias);
        const auto incoming_a = (1.0 - right.tension) *
                                (1.0 - right.continuity) * (1.0 + right.bias);
        const auto incoming_b = (1.0 - right.tension) *
                                (1.0 + right.continuity) * (1.0 - right.bias);
        for (std::size_t axis = 0U; axis < result.size(); ++axis) {
          const auto difference = second[axis] - first[axis];
          auto outgoing = outgoing_b * difference;
          if (left_index != 0U) {
            const auto &previous = motion.keyframes[left_index - 1U];
            const auto time_scale =
                frame_span /
                static_cast<double>(right.frame - previous.frame);
            outgoing =
                time_scale *
                (outgoing_a * (first[axis] - (previous.*member)[axis]) +
                 outgoing_b * difference);
          }
          auto incoming = incoming_a * difference;
          if (left_index + 2U < motion.keyframes.size()) {
            const auto &next = motion.keyframes[left_index + 2U];
            const auto time_scale =
                frame_span / static_cast<double>(next.frame - left.frame);
            incoming =
                time_scale *
                (incoming_b * ((next.*member)[axis] - second[axis]) +
                 incoming_a * difference);
          }
          result[axis] = h1 * first[axis] + h2 * second[axis] +
                         h3 * outgoing + h4 * incoming;
        }
        return result;
      };
  return {authored_frame,
          interpolate_tcb(left.position, right.position,
                          &MotionKeyframe::position),
          interpolate_tcb(left.rotation, right.rotation,
                          &MotionKeyframe::rotation),
          interpolate_tcb(left.attributes, right.attributes,
                          &MotionKeyframe::attributes)};
}

} // namespace mh::content
