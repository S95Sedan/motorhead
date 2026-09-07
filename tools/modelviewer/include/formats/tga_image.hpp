#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace mh::content {

struct TgaImage {
    std::uint16_t width = 0U;
    std::uint16_t height = 0U;
    std::uint8_t image_type = 0U;
    std::uint8_t pixel_depth = 0U;
    std::uint8_t descriptor = 0U;
    std::uint16_t palette_first = 0U;
    std::uint16_t palette_length = 0U;
    bool has_palette = false;
    std::array<std::array<std::uint8_t, 3U>, 256U> palette{};
    // Top-left-oriented source indices for colour-mapped images. Retaining
    // these is required for the retail front-end's saturating indexed
    // additive line rasterizer.
    std::vector<std::uint8_t> palette_indices;
    std::uint64_t consumed_bytes = 0U;
    std::uint64_t trailing_bytes = 0U;
    std::vector<std::uint8_t> rgba;
};

[[nodiscard]] TgaImage parse_tga(std::span<const std::uint8_t> bytes);
[[nodiscard]] TgaImage read_tga(
    const std::filesystem::path& path,
    std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);

} // namespace mh::content
