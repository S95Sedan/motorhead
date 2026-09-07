#include <content/formats/pdi_archive.hpp>

#include <core/error.hpp>
#include <content/formats/iff_image.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <system_error>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::uint64_t record_bytes = 52U;

std::uint16_t le16(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset])
        | (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t le32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

bool all_zero(const std::span<const std::uint8_t> bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](const std::uint8_t value) {
        return value == 0U;
    });
}

std::string ascii_lower(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return value;
}

void ensure_plain_directory(const std::filesystem::path& directory) {
    std::error_code error;
    auto current = directory.root_path();
    for (const auto& component : directory.relative_path()) {
        current /= component;
        auto status = std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory) {
            error.clear();
            if (!std::filesystem::create_directory(current, error) || error) {
                throw ToolError(ExitCode::input, "cannot create PDI extraction directory");
            }
            status = std::filesystem::symlink_status(current, error);
        }
        if (error || std::filesystem::is_symlink(status)
            || !std::filesystem::is_directory(status)) {
            throw ToolError(ExitCode::input, "PDI output parent is not a plain directory");
        }
    }
}

void write_entry(const std::filesystem::path& destination,
    const std::span<const std::uint8_t> bytes, const bool overwrite) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(destination, error);
    const bool exists = !error && std::filesystem::exists(status);
    if (exists && (std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status))) {
        throw ToolError(ExitCode::input, "PDI extraction target is not a plain file");
    }
    if (exists && !overwrite) {
        throw ToolError(ExitCode::input, "PDI extraction target exists (use --overwrite)");
    }
    auto temporary = destination; temporary += ".mhtool-part";
    auto backup = destination; backup += ".mhtool-backup";
    if (std::filesystem::exists(temporary) || (exists && std::filesystem::exists(backup))) {
        throw ToolError(ExitCode::input, "PDI temporary or backup output exists");
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            std::filesystem::remove(temporary, error);
            throw ToolError(ExitCode::input, "cannot write PDI extraction output");
        }
    }
    bool backed_up = false;
    if (exists) {
        std::filesystem::rename(destination, backup, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw ToolError(ExitCode::input, "cannot preserve PDI extraction target");
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
        throw ToolError(ExitCode::input, "cannot finalize PDI extraction output");
    }
    if (backed_up) {
        std::filesystem::remove(backup, error);
        if (error) { throw ToolError(ExitCode::input, "cannot remove PDI extraction backup"); }
    }
}

} // namespace

PdiArchive parse_pdi(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 32U || !std::equal(bytes.begin(), bytes.begin() + 4, "PDI1")) {
        throw ToolError(ExitCode::format, "PDI input does not have the PDI1 signature");
    }
    PdiArchive archive;
    archive.capacity = le32(bytes, 4U);
    archive.entry_count = le32(bytes, 8U);
    archive.header_bytes = le32(bytes, 12U);
    archive.directory_offset = le32(bytes, 16U);
    archive.payload_offset = le32(bytes, 20U);
    archive.payload_bytes = le32(bytes, 24U);
    archive.file_bytes = bytes.size();
    if (archive.capacity == 0U || archive.capacity > 65536U
        || archive.entry_count > archive.capacity || archive.header_bytes != 32U
        || le32(bytes, 28U) != 0U) {
        throw ToolError(ExitCode::format, "PDI header fields are outside the observed subset");
    }
    const auto bitmap_bytes = (static_cast<std::uint64_t>(archive.capacity) + 7U) / 8U;
    const auto expected_directory = static_cast<std::uint64_t>(archive.header_bytes)
        + bitmap_bytes;
    const auto expected_payload = expected_directory
        + static_cast<std::uint64_t>(archive.capacity) * record_bytes;
    const auto expected_file = expected_payload + archive.payload_bytes;
    if (archive.directory_offset != expected_directory
        || archive.payload_offset != expected_payload || expected_file != bytes.size()) {
        throw ToolError(ExitCode::format, "PDI section offsets do not partition the file");
    }

    std::uint32_t bitmap_count = 0U;
    const auto bitmap = bytes.subspan(archive.header_bytes,
        static_cast<std::size_t>(bitmap_bytes));
    for (std::uint32_t index = 0U; index < archive.capacity; ++index) {
        bitmap_count += static_cast<std::uint32_t>(
            (bitmap[index / 8U] >> (index & 7U)) & 1U);
    }
    if (bitmap_count != archive.entry_count) {
        throw ToolError(ExitCode::format, "PDI allocation bitmap disagrees with entry count");
    }

    std::set<std::string> logical_names;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> intervals;
    archive.entries.reserve(archive.entry_count);
    for (std::uint32_t index = 0U; index < archive.capacity; ++index) {
        const bool active = ((bitmap[index / 8U] >> (index & 7U)) & 1U) != 0U;
        const auto record_offset = static_cast<std::uint64_t>(archive.directory_offset)
            + static_cast<std::uint64_t>(index) * record_bytes;
        const auto record = bytes.subspan(static_cast<std::size_t>(record_offset),
            static_cast<std::size_t>(record_bytes));
        if (!active) {
            if (!all_zero(record)) {
                throw ToolError(ExitCode::format, "PDI inactive directory record is nonzero");
            }
            continue;
        }
        PdiEntry entry;
        entry.record_flags = le16(record, 0U);
        entry.previous = static_cast<std::int16_t>(le16(record, 2U));
        entry.next = static_cast<std::int16_t>(le16(record, 4U));
        if (le16(record, 6U) != 0xffffU || le16(record, 8U) != 0xffffU
            || le16(record, 10U) != 0xffffU) {
            throw ToolError(ExitCode::format, "PDI active record has unknown link fields");
        }
        entry.relative_offset = le32(record, 12U);
        entry.byte_size = le32(record, 16U);
        const auto name_data = record.subspan(20U, 32U);
        const auto terminator = std::find(name_data.begin(), name_data.end(), 0U);
        const auto length = static_cast<std::size_t>(terminator - name_data.begin());
        if (length == 0U || length >= name_data.size()
            || entry.record_flags != 8U
            || !all_zero(name_data.subspan(length))) {
            throw ToolError(ExitCode::format, "PDI entry name is not canonical NUL-padded ASCII");
        }
        entry.name.assign(reinterpret_cast<const char*>(name_data.data()), length);
        for (const auto character : entry.name) {
            if (character < 0x20 || character > 0x7e || character == '/'
                || character == '\\' || character == ':') {
                throw ToolError(ExitCode::format, "PDI entry name is unsafe");
            }
        }
        if (!logical_names.insert(ascii_lower(entry.name)).second) {
            throw ToolError(ExitCode::format, "PDI entry names collide after case folding");
        }
        const auto end = static_cast<std::uint64_t>(entry.relative_offset) + entry.byte_size;
        if (entry.byte_size == 0U || end > archive.payload_bytes) {
            throw ToolError(ExitCode::format, "PDI entry payload is out of bounds");
        }
        const auto embedded = bytes.subspan(
            static_cast<std::size_t>(archive.payload_offset + entry.relative_offset),
            entry.byte_size);
        const auto image = parse_iff_ilbm(embedded);
        entry.width = image.width;
        entry.height = image.height;
        entry.planes = image.plane_count;
        entry.masking = image.masking;
        entry.compression = image.compression;
        entry.transparent_pixels = image.transparent_pixels;
        archive.pixel_count += static_cast<std::uint64_t>(image.width) * image.height;
        archive.transparent_pixels += image.transparent_pixels;
        intervals.emplace_back(entry.relative_offset, entry.byte_size);
        archive.entries.push_back(std::move(entry));
    }
    if (archive.entries.size() != archive.entry_count) {
        throw ToolError(ExitCode::format, "PDI active record count is inconsistent");
    }
    std::sort(intervals.begin(), intervals.end());
    std::uint64_t cursor = 0U;
    for (const auto& [offset, size] : intervals) {
        if (offset != cursor) {
            throw ToolError(ExitCode::format, "PDI entry payloads are not a contiguous partition");
        }
        cursor += size;
    }
    if (cursor != archive.payload_bytes) {
        throw ToolError(ExitCode::format, "PDI entries do not consume the payload");
    }
    archive.source_bytes.assign(bytes.begin(), bytes.end());
    return archive;
}

PdiArchive read_pdi(
    const std::filesystem::path& path, const std::uint64_t maximum_file_bytes) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status)) {
        throw ToolError(ExitCode::input, "PDI input is not a plain file");
    }
    const auto size = std::filesystem::file_size(path);
    if (size > maximum_file_bytes || size > std::numeric_limits<std::size_t>::max()) {
        throw ToolError(ExitCode::format, "PDI input exceeds its size bound");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw ToolError(ExitCode::input, "short read while opening PDI input");
    }
    return parse_pdi(bytes);
}

void extract_pdi_iff(const PdiArchive& archive,
    const std::filesystem::path& output_directory, const bool overwrite) {
    if (archive.source_bytes.size() != archive.file_bytes || archive.entries.empty()) {
        throw ToolError(ExitCode::format, "PDI archive has inconsistent retained bytes");
    }
    const auto root = std::filesystem::absolute(output_directory).lexically_normal();
    ensure_plain_directory(root);
    for (const auto& entry : archive.entries) {
        const auto destination = root / entry.name;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(destination, error);
        const bool exists = !error && std::filesystem::exists(status);
        if (exists && (std::filesystem::is_symlink(status)
            || !std::filesystem::is_regular_file(status))) {
            throw ToolError(ExitCode::input, "PDI extraction target is not a plain file");
        }
        if (exists && !overwrite) {
            throw ToolError(ExitCode::input, "PDI extraction target exists (use --overwrite)");
        }
        auto temporary = destination; temporary += ".mhtool-part";
        auto backup = destination; backup += ".mhtool-backup";
        if (std::filesystem::exists(temporary) || std::filesystem::exists(backup)) {
            throw ToolError(ExitCode::input, "PDI extraction temporary target exists");
        }
    }
    for (const auto& entry : archive.entries) {
        const auto start = static_cast<std::size_t>(archive.payload_offset)
            + entry.relative_offset;
        write_entry(root / entry.name,
            std::span<const std::uint8_t>(archive.source_bytes).subspan(start, entry.byte_size),
            overwrite);
    }
}

} // namespace mh::content
