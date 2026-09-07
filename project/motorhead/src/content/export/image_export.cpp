#include <content/export/image_export.hpp>

#include <core/error.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <system_error>
#include <vector>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

void append_be32(std::vector<std::uint8_t>& output, const std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
    output.push_back(static_cast<std::uint8_t>(value >> 16U));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t crc32(const std::span<const std::uint8_t> bytes) {
    auto crc = std::numeric_limits<std::uint32_t>::max();
    for (const auto byte : bytes) {
        crc ^= byte;
        for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
            const auto mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

void append_chunk(std::vector<std::uint8_t>& png,
    const std::array<std::uint8_t, 4U>& type,
    const std::span<const std::uint8_t> payload) {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw ToolError(ExitCode::format, "PNG chunk exceeds its 32-bit length field");
    }
    append_be32(png, static_cast<std::uint32_t>(payload.size()));
    const auto crc_begin = png.size();
    png.insert(png.end(), type.begin(), type.end());
    png.insert(png.end(), payload.begin(), payload.end());
    append_be32(png, crc32(std::span<const std::uint8_t>(png).subspan(crc_begin)));
}

void write_bytes_guarded(const std::filesystem::path& output,
    const std::span<const std::uint8_t> bytes, const bool overwrite) {
    if (output.empty()) { throw ToolError(ExitCode::usage, "PNG output path is empty"); }
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
                throw ToolError(ExitCode::input, "cannot create PNG output directory");
            }
            status = std::filesystem::symlink_status(current, error);
        }
        if (error || std::filesystem::is_symlink(status)
            || !std::filesystem::is_directory(status)) {
            throw ToolError(ExitCode::input, "PNG output parent is not a plain directory");
        }
    }
    error.clear();
    const auto status = std::filesystem::symlink_status(destination, error);
    const bool exists = !error && std::filesystem::exists(status);
    if (exists && (std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status))) {
        throw ToolError(ExitCode::input, "PNG output is not a plain file");
    }
    if (exists && !overwrite) {
        throw ToolError(ExitCode::input, "PNG output exists (use --overwrite)");
    }
    auto temporary = destination; temporary += ".mhtool-part";
    auto backup = destination; backup += ".mhtool-backup";
    if (std::filesystem::exists(temporary) || (exists && std::filesystem::exists(backup))) {
        throw ToolError(ExitCode::input, "PNG temporary or backup output exists");
    }
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            std::filesystem::remove(temporary, error);
            throw ToolError(ExitCode::input, "cannot write PNG output");
        }
    }
    bool backed_up = false;
    if (exists) {
        std::filesystem::rename(destination, backup, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw ToolError(ExitCode::input, "cannot preserve PNG output");
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
        throw ToolError(ExitCode::input, "cannot finalize PNG output");
    }
    if (backed_up) {
        std::filesystem::remove(backup, error);
        if (error) { throw ToolError(ExitCode::input, "cannot remove PNG backup"); }
    }
}

} // namespace

std::vector<std::uint8_t> encode_rgba_png(
    const std::uint32_t width, const std::uint32_t height,
    const std::span<const std::uint8_t> rgba) {
    const auto row_bytes = static_cast<std::uint64_t>(width) * 4U;
    const auto pixel_bytes = row_bytes * height;
    const auto filtered_bytes = pixel_bytes + height;
    if (width == 0U || height == 0U || pixel_bytes != rgba.size()
        || filtered_bytes > std::numeric_limits<std::size_t>::max()) {
        throw ToolError(ExitCode::format, "PNG image has inconsistent or oversized RGBA pixels");
    }

    std::vector<std::uint8_t> filtered;
    filtered.reserve(static_cast<std::size_t>(filtered_bytes));
    for (std::uint32_t y = 0U; y < height; ++y) {
        filtered.push_back(0U);
        const auto begin = static_cast<std::size_t>(y * row_bytes);
        filtered.insert(filtered.end(), rgba.begin() + static_cast<std::ptrdiff_t>(begin),
            rgba.begin() + static_cast<std::ptrdiff_t>(begin + row_bytes));
    }

    std::vector<std::uint8_t> zlib;
    zlib.reserve(filtered.size() + filtered.size() / 65535U * 5U + 16U);
    zlib.push_back(0x78U);
    zlib.push_back(0x01U);
    std::size_t cursor = 0U;
    while (cursor < filtered.size()) {
        const auto count = std::min<std::size_t>(65535U, filtered.size() - cursor);
        const bool final = cursor + count == filtered.size();
        zlib.push_back(final ? 0x01U : 0x00U);
        const auto length = static_cast<std::uint16_t>(count);
        const auto inverse = static_cast<std::uint16_t>(~length);
        zlib.push_back(static_cast<std::uint8_t>(length));
        zlib.push_back(static_cast<std::uint8_t>(length >> 8U));
        zlib.push_back(static_cast<std::uint8_t>(inverse));
        zlib.push_back(static_cast<std::uint8_t>(inverse >> 8U));
        zlib.insert(zlib.end(), filtered.begin() + static_cast<std::ptrdiff_t>(cursor),
            filtered.begin() + static_cast<std::ptrdiff_t>(cursor + count));
        cursor += count;
    }
    std::uint32_t adler_a = 1U;
    std::uint32_t adler_b = 0U;
    for (const auto byte : filtered) {
        adler_a = (adler_a + byte) % 65521U;
        adler_b = (adler_b + adler_a) % 65521U;
    }
    append_be32(zlib, (adler_b << 16U) | adler_a);

    std::vector<std::uint8_t> png{0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    std::vector<std::uint8_t> ihdr;
    ihdr.reserve(13U);
    append_be32(ihdr, width);
    append_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8U, 6U, 0U, 0U, 0U});
    append_chunk(png, {'I', 'H', 'D', 'R'}, ihdr);
    append_chunk(png, {'I', 'D', 'A', 'T'}, zlib);
    append_chunk(png, {'I', 'E', 'N', 'D'}, {});
    return png;
}

void write_rgba_png(const std::uint32_t width, const std::uint32_t height,
    const std::span<const std::uint8_t> rgba,
    const std::filesystem::path& output, const bool overwrite) {
    const auto png = encode_rgba_png(width, height, rgba);
    write_bytes_guarded(output, png, overwrite);
}

} // namespace mh::content
