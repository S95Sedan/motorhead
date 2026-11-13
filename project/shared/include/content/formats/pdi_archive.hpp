#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace mh::content {

struct PdiEntry {
    std::uint16_t record_flags = 0U;
    std::int16_t previous = -1;
    std::int16_t next = -1;
    std::uint32_t relative_offset = 0U;
    std::uint32_t byte_size = 0U;
    std::string name;
    std::uint16_t width = 0U;
    std::uint16_t height = 0U;
    std::uint8_t planes = 0U;
    std::uint8_t masking = 0U;
    std::uint8_t compression = 0U;
    std::uint64_t transparent_pixels = 0U;
};

struct PdiArchive {
    std::uint32_t capacity = 0U;
    std::uint32_t entry_count = 0U;
    std::uint32_t header_bytes = 0U;
    std::uint32_t directory_offset = 0U;
    std::uint32_t payload_offset = 0U;
    std::uint32_t payload_bytes = 0U;
    std::uint64_t file_bytes = 0U;
    std::uint64_t pixel_count = 0U;
    std::uint64_t transparent_pixels = 0U;
    std::vector<PdiEntry> entries;
    std::vector<std::uint8_t> source_bytes;
};

[[nodiscard]] PdiArchive parse_pdi(std::span<const std::uint8_t> bytes);
[[nodiscard]] PdiArchive read_pdi(
    const std::filesystem::path& path,
    std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);
void extract_pdi_iff(
    const PdiArchive& archive, const std::filesystem::path& output_directory,
    bool overwrite);

} // namespace mh::content
