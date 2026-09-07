#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace mh::content {

struct IffIlbmImage {
  std::uint16_t width = 0U;
  std::uint16_t height = 0U;
  std::int16_t x = 0;
  std::int16_t y = 0;
  std::uint8_t plane_count = 0U;
  std::uint8_t masking = 0U;
  std::uint8_t compression = 0U;
  std::uint16_t transparent_color = 0U;
  std::uint8_t x_aspect = 0U;
  std::uint8_t y_aspect = 0U;
  std::int16_t page_width = 0;
  std::int16_t page_height = 0;
  std::uint16_t palette_entries = 0U;
  std::uint32_t chunk_count = 0U;
  std::uint64_t file_bytes = 0U;
  std::uint64_t body_bytes = 0U;
  std::uint64_t decoded_planar_bytes = 0U;
  std::uint64_t transparent_pixels = 0U;
  std::vector<std::uint8_t> palette_indices;
  std::vector<std::uint8_t> rgba;
};

[[nodiscard]] IffIlbmImage parse_iff_ilbm(std::span<const std::uint8_t> bytes);
[[nodiscard]] IffIlbmImage
read_iff_ilbm(const std::filesystem::path &path,
              std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);

} // namespace mh::content
