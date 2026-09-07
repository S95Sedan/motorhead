#include <formats/tga_image.hpp>

#include <core/error.hpp>

#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::uint16_t u16(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]
        | (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

} // namespace

TgaImage parse_tga(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 18U) { throw ToolError(ExitCode::format, "TGA header is truncated"); }
    const auto id_length = static_cast<std::size_t>(bytes[0]);
    const auto color_map_type = bytes[1];
    TgaImage image;
    image.image_type = bytes[2];
    image.palette_first = u16(bytes, 3U);
    image.palette_length = u16(bytes, 5U);
    const auto palette_depth = bytes[7];
    image.width = u16(bytes, 12U);
    image.height = u16(bytes, 14U);
    image.pixel_depth = bytes[16];
    image.descriptor = bytes[17];
    if (image.width == 0U || image.height == 0U) {
        throw ToolError(ExitCode::format, "TGA dimensions must be nonzero");
    }
    if (image.image_type != 1U && image.image_type != 2U && image.image_type != 3U) {
        throw ToolError(ExitCode::format, "TGA image type is not in the observed subset");
    }
    if ((image.image_type == 1U && (color_map_type != 1U || image.pixel_depth != 8U
            || palette_depth != 24U || image.palette_length == 0U))
        || (image.image_type == 2U && (color_map_type != 0U || image.pixel_depth != 24U))
        || (image.image_type == 3U && (color_map_type != 0U || image.pixel_depth != 8U))) {
        throw ToolError(ExitCode::format, "TGA header fields do not match the observed subset");
    }
    const auto pixels = static_cast<std::uint64_t>(image.width) * image.height;
    if (pixels > (512ULL * 1024ULL * 1024ULL) / 4U) {
        throw ToolError(ExitCode::format, "TGA decoded pixels exceed the size bound");
    }
    std::size_t cursor = 18U;
    if (id_length > bytes.size() - cursor) {
        throw ToolError(ExitCode::format, "TGA identification field is truncated");
    }
    cursor += id_length;
    std::vector<std::uint8_t> palette;
    if (color_map_type == 1U) {
        image.has_palette = true;
        const auto palette_bytes = static_cast<std::size_t>(image.palette_length) * 3U;
        if (palette_bytes > bytes.size() - cursor) {
            throw ToolError(ExitCode::format, "TGA palette is truncated");
        }
        palette.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
            bytes.begin() + static_cast<std::ptrdiff_t>(cursor + palette_bytes));
        for (std::size_t entry = 0U; entry < image.palette_length; ++entry) {
            const auto destination =
                static_cast<std::size_t>(image.palette_first) + entry;
            if (destination >= image.palette.size()) {
                throw ToolError(ExitCode::format, "TGA palette exceeds 256 entries");
            }
            image.palette[destination] = {
                palette[entry * 3U + 2U],
                palette[entry * 3U + 1U],
                palette[entry * 3U]};
        }
        cursor += palette_bytes;
    }
    const auto source_pixel_bytes = image.image_type == 2U ? 3U : 1U;
    const auto pixel_bytes = static_cast<std::uint64_t>(pixels) * source_pixel_bytes;
    if (pixel_bytes > bytes.size() - cursor) {
        throw ToolError(ExitCode::format, "TGA pixel data is truncated");
    }
    image.rgba.resize(static_cast<std::size_t>(pixels) * 4U);
    if (image.image_type == 1U) {
        image.palette_indices.resize(static_cast<std::size_t>(pixels));
    }
    const bool top = (image.descriptor & 0x20U) != 0U;
    const bool right = (image.descriptor & 0x10U) != 0U;
    for (std::uint32_t source_y = 0U; source_y < image.height; ++source_y) {
        for (std::uint32_t source_x = 0U; source_x < image.width; ++source_x) {
            const auto x = right ? image.width - source_x - 1U : source_x;
            const auto y = top ? source_y : image.height - source_y - 1U;
            const auto destination = (static_cast<std::size_t>(y) * image.width + x) * 4U;
            if (image.image_type == 1U) {
                const auto index = static_cast<std::uint32_t>(bytes[cursor++]);
                if (index < image.palette_first
                    || index - image.palette_first >= image.palette_length) {
                    throw ToolError(ExitCode::format, "TGA palette index is out of bounds");
                }
                image.palette_indices[destination / 4U] =
                    static_cast<std::uint8_t>(index);
                const auto entry = static_cast<std::size_t>(index - image.palette_first) * 3U;
                image.rgba[destination] = palette[entry + 2U];
                image.rgba[destination + 1U] = palette[entry + 1U];
                image.rgba[destination + 2U] = palette[entry];
            } else if (image.image_type == 2U) {
                image.rgba[destination + 2U] = bytes[cursor++];
                image.rgba[destination + 1U] = bytes[cursor++];
                image.rgba[destination] = bytes[cursor++];
            } else {
                const auto gray = bytes[cursor++];
                image.rgba[destination] = gray;
                image.rgba[destination + 1U] = gray;
                image.rgba[destination + 2U] = gray;
            }
            image.rgba[destination + 3U] = 0xffU;
        }
    }
    image.consumed_bytes = cursor;
    image.trailing_bytes = bytes.size() - cursor;
    return image;
}

TgaImage read_tga(const std::filesystem::path& path, const std::uint64_t maximum_file_bytes) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status)) {
        throw ToolError(ExitCode::input, "TGA input is not a plain file");
    }
    const auto size = std::filesystem::file_size(path);
    if (size > maximum_file_bytes || size > std::numeric_limits<std::size_t>::max()) {
        throw ToolError(ExitCode::format, "TGA file exceeds the configured size bound");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw ToolError(ExitCode::input, "short read while opening TGA input");
    }
    return parse_tga(bytes);
}

} // namespace mh::content
