#include <content/formats/tga_image.hpp>
#include <content/export/image_export.hpp>

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

void write_bytes_guarded(
    const std::filesystem::path& output,
    const std::span<const std::uint8_t> bytes,
    const bool overwrite) {
    if (output.empty()) { throw ToolError(ExitCode::usage, "TGA PPM output path is empty"); }
    const auto destination = std::filesystem::absolute(output).lexically_normal();
    const auto parent = destination.parent_path();
    std::error_code error;
    auto current = parent.root_path();
    for (const auto& component : parent.relative_path()) {
        current /= component;
        auto status = std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory) {
            error.clear();
            if (!std::filesystem::create_directory(current, error) || error) {
                throw ToolError(ExitCode::input, "cannot create TGA PPM output directory");
            }
            status = std::filesystem::symlink_status(current, error);
        }
        if (error || std::filesystem::is_symlink(status)
            || !std::filesystem::is_directory(status)) {
            throw ToolError(ExitCode::input, "TGA PPM output parent is not a plain directory");
        }
    }
    error.clear();
    const auto status = std::filesystem::symlink_status(destination, error);
    const bool exists = !error && std::filesystem::exists(status);
    if (exists && (std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status))) {
        throw ToolError(ExitCode::input, "TGA PPM output is not a plain file");
    }
    if (exists && !overwrite) {
        throw ToolError(ExitCode::input, "TGA PPM output exists (use --overwrite)");
    }
    auto temporary = destination; temporary += ".mhtool-part";
    auto backup = destination; backup += ".mhtool-backup";
    if (std::filesystem::exists(temporary) || (exists && std::filesystem::exists(backup))) {
        throw ToolError(ExitCode::input, "TGA PPM temporary or backup output exists");
    }
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            std::filesystem::remove(temporary, error);
            throw ToolError(ExitCode::input, "cannot write TGA PPM output");
        }
    }
    bool backed_up = false;
    if (exists) {
        std::filesystem::rename(destination, backup, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw ToolError(ExitCode::input, "cannot preserve TGA PPM output");
        }
        backed_up = true;
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        if (backed_up) {
            std::error_code ignored;
            std::filesystem::rename(backup, destination, ignored);
        }
        throw ToolError(ExitCode::input, "cannot finalize TGA PPM output");
    }
    if (backed_up) {
        std::filesystem::remove(backup, error);
        if (error) { throw ToolError(ExitCode::input, "cannot remove TGA PPM backup"); }
    }
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

void write_tga_ppm(const TgaImage& image, const std::filesystem::path& output, const bool overwrite) {
    const auto expected_rgba = static_cast<std::uint64_t>(image.width) * image.height * 4U;
    if (image.width == 0U || image.height == 0U
        || expected_rgba != image.rgba.size()) {
        throw ToolError(ExitCode::format, "TGA image has inconsistent decoded pixels");
    }
    std::ostringstream header;
    header << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    const auto prefix = header.str();
    std::vector<std::uint8_t> ppm(prefix.begin(), prefix.end());
    ppm.reserve(ppm.size() + static_cast<std::size_t>(image.width) * image.height * 3U);
    for (std::size_t pixel = 0U; pixel < image.rgba.size(); pixel += 4U) {
        ppm.insert(ppm.end(), image.rgba.begin() + static_cast<std::ptrdiff_t>(pixel),
            image.rgba.begin() + static_cast<std::ptrdiff_t>(pixel + 3U));
    }
    write_bytes_guarded(output, ppm, overwrite);
}

void write_tga_png(
    const TgaImage& image, const std::filesystem::path& output, const bool overwrite) {
    write_rgba_png(image.width, image.height, image.rgba, output, overwrite);
}

} // namespace mh::content
