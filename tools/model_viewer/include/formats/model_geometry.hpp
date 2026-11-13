#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace mh::content {

struct MyoSection {
  std::uint32_t count = 0U;
  std::uint32_t byte_size = 0U;
  std::uint32_t offset = 0U;
};

struct MyoFace {
  std::uint8_t primitive_type = 0U;
  std::uint8_t flags = 0U;
  std::uint8_t vertex_count = 0U;
  std::uint16_t plane_index = 0U;
  std::array<std::uint8_t, 3U> color{};
  std::array<std::uint16_t, 4U> position_indices{};
  std::uint16_t normal_index = 0U;
  std::array<std::uint16_t, 4U> normal_indices{};
  bool has_vertex_normals = false;
  bool has_texture_coordinates = false;
  std::array<std::array<float, 2U>, 4U> texture_coordinates{};
  std::uint32_t material_name_index = std::numeric_limits<std::uint32_t>::max();
};

// MYO v2 appends the same 120-byte type-2 light/flare record used by MYW.
// Vehicle bodies use image indices 0 and 1 for their ordered CAR front/rear
// Halo dependencies.  The two positions are the authored diagonal corners of
// the lamp source, not an inferred face centre.
struct MyoLightRecord {
  std::uint32_t type = 0U;
  std::uint32_t image_index = 0U;
  std::array<float, 3U> position{};
  std::array<float, 3U> secondary_position{};
  std::uint32_t packed_color = 0U;
  std::uint32_t flags = 0U;
};

struct MyoData {
  std::uint8_t version = 0U;
  std::uint64_t file_bytes = 0U;
  std::array<MyoSection, 7U> sections{};
  std::uint32_t extension_count = 0U;
  std::uint32_t extension_offset = 0U;
  std::array<std::uint64_t, 10U> primitive_type_counts{};
  std::array<float, 3U> minimum{};
  std::array<float, 3U> maximum{};
  std::uint64_t triangle_count = 0U;
  std::uint64_t quad_count = 0U;
  std::uint64_t vertex_reference_count = 0U;
  std::uint64_t textured_face_count = 0U;
  std::uint64_t texture_coordinate_count = 0U;
  std::uint64_t invalid_referenced_normals = 0U;
  std::vector<std::array<float, 3U>> positions;
  std::vector<std::array<float, 3U>> normals;
  std::vector<MyoFace> faces;
  std::vector<std::string> names;
  std::vector<MyoLightRecord> light_records;
};

[[nodiscard]] MyoData parse_myo(std::span<const std::uint8_t> bytes);
[[nodiscard]] MyoData
read_myo(const std::filesystem::path &path,
         std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);

} // namespace mh::content
