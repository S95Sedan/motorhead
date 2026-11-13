#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mh::content {

struct FormatIdentification {
    std::string format_id;
    std::string category;
    std::string status;
    double confidence = 0.0;
    std::string evidence;
};

struct InventoryFile {
    std::string relative_path;
    std::string logical_id;
    std::string extension;
    std::uint64_t bytes = 0;
    std::string sha256;
    std::string header_hex;
    FormatIdentification identification;
};

struct Inventory {
    std::filesystem::path root;
    std::vector<InventoryFile> files;
    std::uint64_t total_bytes = 0;
    std::uint64_t recognized_files = 0;
    std::uint64_t provisional_files = 0;
    std::uint64_t invalid_files = 0;
    std::uint64_t unknown_files = 0;
    std::map<std::string, std::uint64_t> format_counts;
    std::map<std::string, std::uint64_t> extension_counts;
    std::string manifest_sha256;
};

[[nodiscard]] FormatIdentification identify_format(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& header,
    std::uint64_t file_size);

[[nodiscard]] Inventory inventory_tree(const std::filesystem::path& root);

} // namespace mh::content
