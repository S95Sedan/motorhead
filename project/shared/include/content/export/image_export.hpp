#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace mh::content {

[[nodiscard]] std::vector<std::uint8_t> encode_rgba_png(
    std::uint32_t width, std::uint32_t height,
    std::span<const std::uint8_t> rgba);

void write_rgba_png(std::uint32_t width, std::uint32_t height,
                    std::span<const std::uint8_t> rgba,
                    const std::filesystem::path& output, bool overwrite);

} // namespace mh::content
