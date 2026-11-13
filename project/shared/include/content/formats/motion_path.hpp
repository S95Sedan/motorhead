#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace mh::content {

struct MotionKeyframe {
  std::array<double, 3U> position{};
  std::array<double, 3U> rotation{};
  std::array<double, 3U> attributes{};
  std::int32_t frame = 0;
  std::int32_t linear = 0;
  double tension = 0.0;
  double continuity = 0.0;
  double bias = 0.0;

  bool operator==(const MotionKeyframe &) const = default;
};

struct MotionData {
  std::uint32_t version = 0U;
  std::uint32_t value_count = 0U;
  bool tbl_wrapped = false;
  std::vector<MotionKeyframe> keyframes;
};

// A renderer-independent orthonormal frame derived from an authored motion
// keyframe and the next distinct point on its path. The axes use Motorhead's
// world convention: X right, Y up, and Z forward.
struct MotionPathFrame {
  std::size_t keyframe_index = 0U;
  std::int32_t authored_frame = 0;
  std::array<double, 3U> position{};
  std::array<double, 3U> right{};
  std::array<double, 3U> up{};
  std::array<double, 3U> forward{};
};

struct MotionSample {
  double authored_frame = 0.0;
  std::array<double, 3U> position{};
  std::array<double, 3U> rotation{};
  std::array<double, 3U> attributes{};
};

[[nodiscard]] MotionData
parse_motion(std::span<const std::uint8_t> decoded_text);
[[nodiscard]] MotionData
read_motion(const std::filesystem::path &path,
            std::uint64_t maximum_file_bytes = 16ULL * 1024ULL * 1024ULL);

[[nodiscard]] MotionPathFrame motion_path_frame(const MotionData &motion,
                                                std::size_t keyframe_index);

// Samples the version-1 LightWave TCB/linear span used by the retail motion
// corpus. The right-hand key owns the incoming span's Linear/T/C/B values.
// Values outside the authored range clamp to the end keys.
[[nodiscard]] MotionSample motion_sample(const MotionData &motion,
                                         double authored_frame);

} // namespace mh::content
