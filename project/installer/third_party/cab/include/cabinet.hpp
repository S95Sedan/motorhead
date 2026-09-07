#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mh::cab {

struct CabFile {
    std::string path;
    std::uint32_t size = 0;
    std::uint32_t folder_offset = 0;
    std::uint16_t folder_index = 0;
    std::uint16_t attributes = 0;
};

struct Cabinet {
    std::filesystem::path source_path;
    std::uint64_t cabinet_offset = 0;
    std::uint32_t cabinet_size = 0;
    std::uint32_t file_table_offset = 0;
    std::uint32_t folder_data_offset = 0;
    std::uint16_t data_block_count = 0;
    std::uint16_t compression = 0;
    std::vector<CabFile> files;
};

struct ExtractedCabFile {
    std::string path;
    std::uint64_t size = 0;
    std::string sha256;
};

[[nodiscard]] Cabinet find_embedded_cabinet(const std::filesystem::path& path);
[[nodiscard]] std::vector<ExtractedCabFile> extract_cabinet(
    const Cabinet& cabinet,
    const std::filesystem::path& output_root,
    bool overwrite);

} // namespace mh::cab
