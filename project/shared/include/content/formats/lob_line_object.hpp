#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace mh::content {

struct LobSummary {
    std::uint32_t vertex_count = 0U;
    std::uint32_t edge_count = 0U;
    std::uint64_t expected_bytes = 0U;
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    std::uint32_t maximum_edge_index = 0U;
    bool edge_indices_in_bounds = false;
};

struct LobVertex {
    std::array<float, 3> position{};
    std::array<std::uint32_t, 5> preserved_words{};
};

struct LobEdge {
    std::uint32_t first = 0U;
    std::uint32_t second = 0U;
};

struct LobData {
    LobSummary summary;
    std::vector<LobVertex> vertices;
    std::vector<LobEdge> edges;
};

[[nodiscard]] LobSummary inspect_lob(std::span<const std::uint8_t> bytes);
[[nodiscard]] LobData parse_lob(std::span<const std::uint8_t> bytes);
// Retail menu line objects are a mixed set: most are TBL-wrapped with the
// UGP005 null-key fallback "EQ", while lobj07 is a raw LOB payload.
[[nodiscard]] LobData read_lob(
    const std::filesystem::path& path,
    std::string key_name = "EQ",
    std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);
void write_lob_obj(const LobData& lob, const std::filesystem::path& output, bool overwrite);

} // namespace mh::content
