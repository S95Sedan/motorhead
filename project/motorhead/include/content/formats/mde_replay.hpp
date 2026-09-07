#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mh::content {

inline constexpr std::size_t mde_v25_header_bytes = 0x0facU;
inline constexpr std::size_t mde_v3_header_bytes = 0x1a31U;
inline constexpr std::size_t mde_v3_record_prefix_bytes = 8U;
inline constexpr std::size_t mde_v3_stream_payload_bytes = 30U;
inline constexpr std::size_t mde_v25_racer_slots = 8U;
inline constexpr std::size_t mde_v25_racer_record_bytes = 0x41U;
inline constexpr std::size_t mde_v3_racer_slots = 13U;
inline constexpr std::size_t mde_v3_racer_record_bytes = 0x51U;

// Exact intermediate state produced by the stock p3.1 decoder at RVA
// 0x0001b984. Names retain their proven destination offsets where gameplay
// semantics have not yet been independently established.
struct MdeV3DecodedStreamState {
  std::array<float, 4U> wheel_history{};
  float body_d8_normalized = 0.0F;
  float body_e4_bounded = 0.0F;
  float body_e8_bounded = 0.0F;
  float body_54_scaled = 0.0F;
  float body_cc_half_step = 0.0F;
  std::uint32_t body_4c_mode = 0U;
  // Packed byte-7/8 nibble order is applied to body
  // +0x180/+0x184/+0x178/+0x17c respectively.
  std::array<float, 4U> normalized_auxiliary{};
  std::uint32_t body_170_mode = 0U;
  std::uint32_t body_174_mode = 0U;
  bool body_168_flag = false;
  bool body_16c_flag = false;
  std::uint32_t body_188_mode = 0U;
  std::uint32_t body_18c_mode = 0U;
  std::uint32_t body_flags = 0U;
  std::array<float, 3U> orientation_parameters{};
  std::array<float, 3U> world_position{};

  bool operator==(const MdeV3DecodedStreamState &) const = default;
};

struct MdeV3StreamSample {
  std::array<std::uint8_t, mde_v3_stream_payload_bytes> packed_payload{};
  MdeV3DecodedStreamState decoded{};
  // Compatibility view retained for existing position-only consumers.
  std::array<float, 3U> world_position{};

  bool operator==(const MdeV3StreamSample &) const = default;
};

struct MdeV3Record {
  std::uint32_t flags = 0U;
  std::uint32_t milliseconds = 0U;
  std::vector<MdeV3StreamSample> streams;

  bool operator==(const MdeV3Record &) const = default;
};

struct MdeV3RacerMetadata {
  bool active = false;
  std::string driver_name;
  std::string team_name;
  std::string car_name;
  std::string profile_name;
  std::array<std::uint32_t, 6U> retained_values{};

  bool operator==(const MdeV3RacerMetadata &) const = default;
};

// Versioned framing and exact packed-stream decoding for the owned p3.1 MDE
// corpus. The compatibility name is retained for existing consumers.
struct MdeV3Data {
  float version = 0.0F;
  std::string track_name;
  std::uint32_t stream_count = 0U;
  std::size_t header_bytes = 0U;
  std::size_t record_bytes = 0U;
  std::size_t file_bytes = 0U;
  std::array<MdeV3RacerMetadata, mde_v3_racer_slots> racer_slots{};
  std::vector<MdeV3Record> records;
};

struct MdeTimedPosition {
  double seconds = 0.0;
  std::array<double, 3U> world_position{};
};

struct MdePositionAlignment {
  std::size_t stream_index = 0U;
  std::size_t start_record = 0U;
  std::uint32_t start_milliseconds = 0U;
  std::size_t compared_samples = 0U;
  double rms_error = 0.0;
  double maximum_error = 0.0;
};

struct MdeV3PlaybackSample {
  MdeV3DecodedStreamState state{};
  std::array<float, 3U> orientation_velocity{};
  std::array<float, 3U> world_velocity{};
  std::size_t lower_record = 0U;
  std::size_t upper_record = 0U;
  float interpolation = 0.0F;

  bool operator==(const MdeV3PlaybackSample &) const = default;
};

[[nodiscard]] MdeV3DecodedStreamState decode_mde_v3_stream_payload(
    std::span<const std::uint8_t, mde_v3_stream_payload_bytes> packed_payload);
[[nodiscard]] MdeV3Data parse_mde(std::span<const std::uint8_t> bytes);
[[nodiscard]] MdeV3Data
read_mde(const std::filesystem::path &path,
         std::uint64_t maximum_file_bytes = 64ULL * 1024ULL * 1024ULL);
[[nodiscard]] MdeV3Data parse_mde_v3(std::span<const std::uint8_t> bytes);
[[nodiscard]] MdeV3Data
read_mde_v3(const std::filesystem::path &path,
            std::uint64_t maximum_file_bytes = 64ULL * 1024ULL * 1024ULL);

[[nodiscard]] MdeV3PlaybackSample
sample_mde_v3_stream(const MdeV3Data &demo, std::size_t stream_index,
                     std::uint32_t milliseconds);

// Returns the first recorded time at which the selected stream has moved at
// least minimum_distance from its initial grid pose. This exposes the retained
// pre-race interval instead of forcing runtime callers to guess a fixed skip.
[[nodiscard]] std::optional<std::uint32_t>
first_mde_stream_displacement_milliseconds(
    const MdeV3Data &demo, std::size_t stream_index,
    float minimum_distance = 0.1F);

[[nodiscard]] MdePositionAlignment
align_mde_v3_positions(const MdeV3Data &demo, std::size_t stream_index,
                       std::span<const MdeTimedPosition> samples);

} // namespace mh::content
