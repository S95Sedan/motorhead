#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mh::content {

struct SprEntry {
    std::string name;
    std::uint32_t payload_offset = 0U;
    std::uint32_t payload_bytes = 0U;
    std::uint32_t frame_count = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint32_t> frame_offsets;
};

struct SprFrame {
    std::string name;
    std::uint32_t frame_index = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> rgba;
};

struct SprArchive {
    bool has_palette = false;
    std::uint32_t file_bytes = 0U;
    std::uint32_t directory_bytes = 0U;
    std::uint32_t palette_offset = 0U;
    std::array<std::array<std::uint8_t, 3U>, 256U> palette{};
    std::vector<SprEntry> entries;
    std::vector<std::uint8_t> source_bytes;
    mutable std::vector<std::vector<SprFrame>> decoded_frames;
};

[[nodiscard]] SprArchive parse_spr(std::span<const std::uint8_t> bytes);
[[nodiscard]] SprArchive read_spr(
    const std::filesystem::path& path,
    std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);
[[nodiscard]] const SprEntry* find_spr_entry(
    const SprArchive& archive, std::string_view name);
[[nodiscard]] const SprFrame& decode_spr_frame(
    const SprArchive& archive, const SprEntry& entry, std::uint32_t frame_index);
void write_spr_frame_png(
    const SprFrame& frame, const std::filesystem::path& output, bool overwrite);

} // namespace mh::content
