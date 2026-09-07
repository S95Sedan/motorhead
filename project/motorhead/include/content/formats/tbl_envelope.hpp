#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mh::content {

constexpr std::uint8_t tbl_transform_flag = 0x01U;
constexpr std::uint8_t tbl_packed_flag = 0x02U;
constexpr std::uint8_t tbl_observed_base_flag = 0x04U;

struct TblEnvelope {
    std::filesystem::path source_path;
    std::uint8_t flags = 0U;
    std::string key_name;
    std::uint64_t source_bytes = 0U;
    std::uint64_t encoded_payload_bytes = 0U;
    bool transform_present = false;
    bool packed_present = false;
    bool outer_transform_decoded = false;
    std::uint32_t packed_output_bytes = 0U;
    std::uint8_t packed_algorithm = 0U;
    bool packed_postprocess = false;
    bool decoded = false;
    std::string decode_status;
    std::string source_sha256;
    std::string encoded_payload_sha256;
    std::string transformed_payload_sha256;
    std::string decoded_payload_sha256;
    std::vector<std::uint8_t> transformed_header;
    std::vector<std::uint8_t> payload;
};

[[nodiscard]] std::vector<std::uint8_t> decode_tbl_transform(
    std::span<const std::uint8_t> encoded, std::string_view key_name);

[[nodiscard]] std::vector<std::uint8_t> encode_tbl_transform(
    std::span<const std::uint8_t> decoded, std::string_view key_name);

[[nodiscard]] TblEnvelope read_tbl(
    const std::filesystem::path& path,
    std::string key_name = {},
    std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL,
    std::uint64_t maximum_decoded_bytes = 512ULL * 1024ULL * 1024ULL);

void write_tbl_payload(
    const TblEnvelope& envelope,
    const std::filesystem::path& output,
    bool overwrite);

} // namespace mh::content
