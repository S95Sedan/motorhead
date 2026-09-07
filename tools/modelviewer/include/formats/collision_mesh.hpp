#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace mh::content {

struct ColSection {
    std::uint16_t count = 0U;
    std::uint16_t stride = 0U;
    std::uint64_t offset = 0U;
    std::uint64_t byte_size = 0U;
};

struct ColFace {
    std::uint16_t surface = 0U;
    std::array<std::uint16_t, 5U> boundary_references{};
    std::uint16_t active_boundary_count = 0U;
    std::array<float, 3U> normal{};
    float distance = 0.0F;
};

struct ColPlane {
    std::array<float, 3U> normal{};
    float distance = 0.0F;
};

struct ColSpatialNode {
    std::array<std::uint16_t, 8U> children{};
};

struct ColCellList {
    std::uint16_t dword_offset = 0U;
    std::vector<std::uint16_t> face_indices;
};

struct ColPolygon {
    std::uint16_t face_index = 0U;
    std::uint16_t surface = 0U;
    std::array<float, 3U> normal{};
    std::vector<std::array<float, 3U>> vertices;
};

struct ColData {
    std::uint64_t file_bytes = 0U;
    std::uint16_t prefix_bytes = 0U;
    std::uint16_t version = 0U;
    std::array<std::uint16_t, 3U> grid_dimensions{};
    std::uint16_t spatial_reference_count = 0U;
    std::array<float, 3U> origin{};
    float scale = 0.0F;
    float inverse_scale = 0.0F;
    std::uint32_t prefix_word_count = 0U;
    std::array<ColSection, 4U> sections{};
    std::uint64_t grid_cell_count = 0U;
    std::uint64_t empty_grid_cells = 0U;
    std::uint64_t occupied_grid_cells = 0U;
    std::uint32_t reachable_internal_nodes = 0U;
    std::uint32_t leaf_list_count = 0U;
    std::uint32_t reachable_face_count = 0U;
    std::uint64_t nonzero_boundary_references = 0U;
    std::uint64_t active_boundary_references = 0U;
    std::uint64_t flipped_boundary_references = 0U;
    std::uint64_t nonzero_boundary_padding = 0U;
    std::uint16_t maximum_boundary_reference = 0U;
    std::uint64_t boundary_references_outside_plane_table = 0U;
    std::vector<std::uint16_t> grid_prefix;
    std::vector<ColSpatialNode> spatial_nodes;
    std::vector<ColCellList> cell_lists;
    std::vector<ColFace> faces;
    std::vector<ColPlane> planes;
};

[[nodiscard]] ColData parse_col(std::span<const std::uint8_t> bytes);
[[nodiscard]] ColData read_col(
    const std::filesystem::path& path,
    std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);
[[nodiscard]] std::vector<ColPolygon> reconstruct_col_polygons(const ColData& collision);

} // namespace mh::content
