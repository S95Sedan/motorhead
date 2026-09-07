#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mh::disc {

struct IsoVolume {
    std::string system_id;
    std::string volume_id;
    std::uint32_t block_count = 0;
    std::uint16_t block_size = 0;
    std::uint32_t root_extent = 0;
    std::uint32_t root_size = 0;
};

struct IsoEntry {
    std::string path;
    bool is_directory = false;
    std::uint32_t extent = 0;
    std::uint32_t size = 0;
};

struct IsoReport {
    IsoVolume volume;
    std::vector<IsoEntry> entries;
};

struct ExtractedIsoFile {
    std::string path;
    std::uint32_t size = 0;
    std::string sha256;
};

struct MaterializedIsoImage {
    std::uint32_t blocks = 0;
    std::uint64_t bytes = 0;
    std::string sha256;
};

[[nodiscard]] IsoReport read_iso9660(
    const std::filesystem::path& raw_bin_path,
    std::uint32_t data_track_lba);

[[nodiscard]] std::vector<ExtractedIsoFile> extract_iso9660(
    const std::filesystem::path& raw_bin_path,
    std::uint32_t data_track_lba,
    const IsoReport& report,
    const std::filesystem::path& output_root,
    bool overwrite_existing_files);

[[nodiscard]] MaterializedIsoImage materialize_iso9660_image(
    const std::filesystem::path& raw_bin_path,
    std::uint32_t data_track_lba,
    const IsoReport& report,
    const std::filesystem::path& output_path,
    bool overwrite_existing_file);

} // namespace mh::disc
