#include <content/formats/spr_sprite_archive.hpp>

#include <content/export/image_export.hpp>

#include <core/error.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <utility>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::uint32_t palette_flag = 0x80000000U;
constexpr std::uint32_t count_mask = 0x7fffffffU;
constexpr std::uint32_t directory_record_bytes = 12U;
constexpr std::uint32_t palette_bytes = 256U * 3U;
constexpr std::uint32_t maximum_entries = 4096U;
constexpr std::uint32_t maximum_dimension = 4096U;
constexpr std::uint32_t maximum_frames = 4096U;

std::uint32_t u32(
    const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) {
        throw ToolError(ExitCode::format, "SPR 32-bit field is truncated");
    }
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::string ascii_lower(std::string_view value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const auto character : value) {
        lowered.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))));
    }
    return lowered;
}

std::size_t entry_end(const SprEntry& entry) {
    return static_cast<std::size_t>(entry.payload_offset) + entry.payload_bytes;
}

void write_indexed_bytes(
    SprFrame& frame, const SprArchive& archive, const std::uint32_t y,
    const std::int64_t x, const std::span<const std::uint8_t> indices) {
    if (x < 0 || x + static_cast<std::int64_t>(indices.size()) > frame.width) {
        throw ToolError(
            ExitCode::format, "SPR scanline write leaves the sprite bounds");
    }
    for (std::size_t index = 0U; index < indices.size(); ++index) {
        const auto palette_index = indices[index];
        const auto pixel =
            (static_cast<std::size_t>(y) * frame.width
                + static_cast<std::size_t>(x) + index)
            * 4U;
        frame.rgba[pixel] = archive.palette[palette_index][0];
        frame.rgba[pixel + 1U] = archive.palette[palette_index][1];
        frame.rgba[pixel + 2U] = archive.palette[palette_index][2];
        frame.rgba[pixel + 3U] = 0xffU;
    }
}

} // namespace

SprArchive parse_spr(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4U || bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw ToolError(ExitCode::format, "SPR file is truncated or exceeds 4 GiB");
    }
    const auto count_field = u32(bytes, 0U);
    const auto count = count_field & count_mask;
    if (count == 0U || count > maximum_entries) {
        throw ToolError(ExitCode::format, "SPR entry count is outside the observed bounds");
    }
    const auto directory_bytes =
        static_cast<std::uint64_t>(count) * directory_record_bytes;
    const bool has_palette = (count_field & palette_flag) != 0U;
    const auto payload_begin = 4ULL + directory_bytes
        + (has_palette ? palette_bytes : 0U);
    if (payload_begin > bytes.size()) {
        throw ToolError(ExitCode::format, "SPR directory or palette is truncated");
    }

    SprArchive archive;
    archive.has_palette = has_palette;
    archive.file_bytes = static_cast<std::uint32_t>(bytes.size());
    archive.directory_bytes = static_cast<std::uint32_t>(directory_bytes);
    archive.palette_offset = 4U + archive.directory_bytes;
    archive.source_bytes.assign(bytes.begin(), bytes.end());
    if (has_palette) {
        for (std::size_t index = 0U; index < archive.palette.size(); ++index) {
            const auto source = static_cast<std::size_t>(archive.palette_offset)
                + index * 3U;
            archive.palette[index] = {
                bytes[source], bytes[source + 1U], bytes[source + 2U]};
        }
    }

    archive.entries.reserve(count);
    for (std::uint32_t index = 0U; index < count; ++index) {
        const auto record =
            4U + static_cast<std::size_t>(index) * directory_record_bytes;
        std::size_t name_bytes = 0U;
        while (name_bytes < 8U && bytes[record + name_bytes] != 0U) {
            const auto character = bytes[record + name_bytes];
            if (character < 0x20U || character > 0x7eU) {
                throw ToolError(ExitCode::format, "SPR entry name is not printable ASCII");
            }
            ++name_bytes;
        }
        if (name_bytes == 0U) {
            throw ToolError(ExitCode::format, "SPR entry name is empty");
        }
        SprEntry entry;
        entry.name.assign(
            reinterpret_cast<const char*>(bytes.data() + record), name_bytes);
        entry.payload_offset = u32(bytes, record + 8U);
        if (entry.payload_offset < payload_begin || entry.payload_offset >= bytes.size()) {
            throw ToolError(ExitCode::format, "SPR payload offset is out of bounds");
        }
        if (!archive.entries.empty()
            && entry.payload_offset <= archive.entries.back().payload_offset) {
            throw ToolError(ExitCode::format, "SPR payload offsets are not increasing");
        }
        archive.entries.push_back(std::move(entry));
    }

    for (std::size_t index = 0U; index < archive.entries.size(); ++index) {
        auto& entry = archive.entries[index];
        const auto end = index + 1U < archive.entries.size()
            ? archive.entries[index + 1U].payload_offset
            : archive.file_bytes;
        entry.payload_bytes = end - entry.payload_offset;
        if (entry.payload_bytes < 16U) {
            throw ToolError(ExitCode::format, "SPR sprite header is truncated");
        }
        const auto start = static_cast<std::size_t>(entry.payload_offset);
        entry.frame_count = u32(bytes, start);
        entry.width = u32(bytes, start + 4U);
        entry.height = u32(bytes, start + 8U);
        if (entry.frame_count == 0U || entry.frame_count > maximum_frames
            || entry.width == 0U || entry.width > maximum_dimension
            || entry.height == 0U || entry.height > maximum_dimension) {
            throw ToolError(
                ExitCode::format, "SPR frame count or dimensions exceed observed bounds");
        }
        const auto table_end = 12ULL
            + static_cast<std::uint64_t>(entry.frame_count) * 4U;
        if (table_end > entry.payload_bytes) {
            throw ToolError(ExitCode::format, "SPR frame table is truncated");
        }
        entry.frame_offsets.reserve(entry.frame_count);
        for (std::uint32_t frame = 0U; frame < entry.frame_count; ++frame) {
            const auto offset = u32(bytes, start + 12U + frame * 4U);
            if (offset < table_end || offset >= entry.payload_bytes
                || (!entry.frame_offsets.empty()
                    && offset <= entry.frame_offsets.back())) {
                throw ToolError(ExitCode::format, "SPR frame offset is invalid");
            }
            const auto row_table_end = static_cast<std::uint64_t>(offset)
                + static_cast<std::uint64_t>(entry.height) * 4U;
            if (row_table_end > entry.payload_bytes) {
                throw ToolError(ExitCode::format, "SPR scanline table is truncated");
            }
            entry.frame_offsets.push_back(offset);
        }
    }
    return archive;
}

SprArchive read_spr(
    const std::filesystem::path& path, const std::uint64_t maximum_file_bytes) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status)) {
        throw ToolError(ExitCode::input, "SPR input is not a plain file");
    }
    const auto size = std::filesystem::file_size(path);
    if (size > maximum_file_bytes || size > std::numeric_limits<std::size_t>::max()) {
        throw ToolError(ExitCode::format, "SPR file exceeds the configured size bound");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(size)) {
        throw ToolError(ExitCode::input, "short read while opening SPR input");
    }
    return parse_spr(bytes);
}

const SprEntry* find_spr_entry(
    const SprArchive& archive, const std::string_view name) {
    const auto normalized_name = ascii_lower(name);
    const auto found = std::find_if(
        archive.entries.begin(), archive.entries.end(),
        [&normalized_name](const SprEntry& entry) {
            return ascii_lower(entry.name) == normalized_name;
        });
    return found == archive.entries.end() ? nullptr : &*found;
}

const SprFrame& decode_spr_frame(const SprArchive& archive, const SprEntry& entry,
    const std::uint32_t frame_index) {
    if (!archive.has_palette) {
        throw ToolError(
            ExitCode::format, "SPR frame decoding requires an embedded palette");
    }
    if (frame_index >= entry.frame_count
        || frame_index >= entry.frame_offsets.size()) {
        throw ToolError(ExitCode::usage, "SPR frame index is out of range");
    }
    const auto entry_iterator = std::find_if(
        archive.entries.begin(), archive.entries.end(),
        [&entry](const SprEntry& candidate) { return &candidate == &entry; });
    if (entry_iterator == archive.entries.end()) {
        throw ToolError(ExitCode::usage, "SPR entry does not belong to archive");
    }
    const auto entry_index = static_cast<std::size_t>(
        std::distance(archive.entries.begin(), entry_iterator));
    if (archive.decoded_frames.size() != archive.entries.size()) {
        archive.decoded_frames.clear();
        archive.decoded_frames.resize(archive.entries.size());
    }
    auto& entry_cache = archive.decoded_frames[entry_index];
    if (entry_cache.size() != entry.frame_count) {
        entry_cache.clear();
        entry_cache.resize(entry.frame_count);
    }
    auto& cached = entry_cache[frame_index];
    if (!cached.rgba.empty()) {
        return cached;
    }
    const auto& bytes = archive.source_bytes;
    const auto payload_start = static_cast<std::size_t>(entry.payload_offset);
    const auto payload_end = entry_end(entry);
    const auto frame_start =
        payload_start + entry.frame_offsets[frame_index];
    const auto frame_end = frame_index + 1U < entry.frame_offsets.size()
        ? payload_start + entry.frame_offsets[frame_index + 1U]
        : payload_end;
    if (frame_start > frame_end
        || frame_end > bytes.size()
        || static_cast<std::uint64_t>(entry.width) * entry.height
            > std::numeric_limits<std::size_t>::max() / 4U) {
        throw ToolError(ExitCode::format, "SPR frame bounds are invalid");
    }

    SprFrame frame;
    frame.name = entry.name;
    frame.frame_index = frame_index;
    frame.width = entry.width;
    frame.height = entry.height;
    frame.rgba.assign(
        static_cast<std::size_t>(frame.width) * frame.height * 4U, 0U);

    for (std::uint32_t y = 0U; y < frame.height; ++y) {
        const auto row_offset = u32(bytes, frame_start + y * 4U);
        if (row_offset < static_cast<std::uint64_t>(frame.height) * 4U
            || row_offset >= frame_end - frame_start) {
            throw ToolError(ExitCode::format, "SPR scanline offset is invalid");
        }
        auto cursor = frame_start + row_offset;
        std::int64_t base_x = 0;
        bool returned = false;
        while (cursor < frame_end) {
            const auto opcode = bytes[cursor];
            if (opcode == 0xc3U) {
                returned = true;
                break;
            }
            if (opcode == 0x05U) {
                const auto value = u32(bytes, cursor + 1U);
                base_x += static_cast<std::int32_t>(value);
                cursor += 5U;
                continue;
            }
            std::size_t prefix = 0U;
            std::size_t count = 0U;
            if (opcode == 0xc6U) {
                prefix = 3U;
                count = 1U;
            } else if (opcode == 0x66U
                && cursor + 2U < frame_end && bytes[cursor + 1U] == 0xc7U) {
                prefix = 4U;
                count = 2U;
            } else if (opcode == 0xc7U) {
                prefix = 3U;
                count = 4U;
            } else {
                throw ToolError(ExitCode::format,
                    "SPR scanline uses an unsupported compiled instruction");
            }
            if (cursor > frame_end || frame_end - cursor < prefix + count) {
                throw ToolError(ExitCode::format, "SPR scanline store is truncated");
            }
            const auto modrm = bytes[cursor + (opcode == 0x66U ? 2U : 1U)];
            if (modrm != 0x40U) {
                throw ToolError(ExitCode::format,
                    "SPR scanline store is outside the recovered instruction subset");
            }
            const auto displacement =
                static_cast<std::int8_t>(bytes[cursor + prefix - 1U]);
            write_indexed_bytes(frame, archive, y, base_x + displacement,
                std::span<const std::uint8_t>(bytes).subspan(cursor + prefix, count));
            cursor += prefix + count;
        }
        if (!returned) {
            throw ToolError(ExitCode::format, "SPR scanline has no return instruction");
        }
    }
    cached = std::move(frame);
    return cached;
}

void write_spr_frame_png(const SprFrame& frame,
    const std::filesystem::path& output, const bool overwrite) {
    write_rgba_png(frame.width, frame.height, frame.rgba, output, overwrite);
}

} // namespace mh::content
