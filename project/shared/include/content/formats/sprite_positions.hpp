#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>

namespace mh::content {

inline constexpr std::size_t spr_position_bank_count = 32U;
inline constexpr std::size_t spr_position_records_per_bank = 32U;
inline constexpr std::size_t spr_position_record_fields = 8U;
inline constexpr std::size_t spr_position_file_bytes =
    spr_position_bank_count * spr_position_records_per_bank
    * spr_position_record_fields * sizeof(std::int32_t);

struct SprPositionRecord {
    std::int32_t x_from = 0;
    std::int32_t x_to = 0;
    std::int32_t y_from = 0;
    std::int32_t y_to = 0;
    std::array<std::int32_t, 4U> direct{};
};

struct SprPositionBank {
    std::array<SprPositionRecord, spr_position_records_per_bank> records{};
};

struct SprPositionData {
    std::array<SprPositionBank, spr_position_bank_count> banks{};
};

struct SprPositionSample {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::array<std::int32_t, 4U> direct{};
};

[[nodiscard]] SprPositionData parse_spr_positions(
    std::span<const std::uint8_t> bytes);
[[nodiscard]] SprPositionData read_spr_positions(
    const std::filesystem::path& path);
[[nodiscard]] SprPositionSample sample_spr_position(
    const SprPositionData& data, std::size_t bank_index,
    std::size_t record_index, std::uint32_t phase);
[[nodiscard]] bool is_empty_spr_position(const SprPositionRecord& record);

} // namespace mh::content
