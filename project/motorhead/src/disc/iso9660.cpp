#include <core/error.hpp>
#include <core/crypto/sha256.hpp>
#include <disc/iso9660.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <ostream>
#include <set>
#include <string_view>
#include <system_error>

namespace mh::disc {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::uint64_t raw_sector_size = 2352U;
constexpr std::uint64_t mode1_user_offset = 16U;
constexpr std::size_t iso_block_size = 2048U;
constexpr std::size_t maximum_directory_bytes = 64U * 1024U * 1024U;
constexpr std::size_t maximum_entries = 1'000'000U;
constexpr std::size_t maximum_depth = 64U;

std::uint16_t read_le16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(data[0])
        | static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint16_t read_be16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data[0]) << 8U)
        | static_cast<std::uint16_t>(data[1]);
}

std::uint32_t read_le32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0])
        | (static_cast<std::uint32_t>(data[1]) << 8U)
        | (static_cast<std::uint32_t>(data[2]) << 16U)
        | (static_cast<std::uint32_t>(data[3]) << 24U);
}

std::uint32_t read_be32(const std::uint8_t* data) {
    return (static_cast<std::uint32_t>(data[0]) << 24U)
        | (static_cast<std::uint32_t>(data[1]) << 16U)
        | (static_cast<std::uint32_t>(data[2]) << 8U)
        | static_cast<std::uint32_t>(data[3]);
}

std::string padded_text(const std::uint8_t* data, const std::size_t size) {
    std::string value(reinterpret_cast<const char*>(data), size);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) {
        value.pop_back();
    }
    return value;
}

class RawMode1Reader final {
public:
    RawMode1Reader(std::filesystem::path path, const std::uint32_t base_lba)
        : path_(std::move(path)), input_(path_, std::ios::binary), base_lba_(base_lba) {
        if (!input_) {
            throw ToolError(ExitCode::input, "cannot open raw BIN file: " + path_.string());
        }
        const auto bytes = std::filesystem::file_size(path_);
        if (bytes % raw_sector_size != 0U) {
            throw ToolError(ExitCode::format, "raw BIN size is not divisible by 2352 bytes");
        }
        raw_sector_count_ = bytes / raw_sector_size;
        if (base_lba_ >= raw_sector_count_) {
            throw ToolError(ExitCode::format, "data track starts beyond the end of the raw BIN");
        }
    }

    [[nodiscard]] std::array<std::uint8_t, iso_block_size> read(const std::uint32_t logical_lba) {
        const auto raw_lba = static_cast<std::uint64_t>(base_lba_) + logical_lba;
        if (raw_lba >= raw_sector_count_) {
            throw ToolError(ExitCode::format, "ISO read extends beyond the raw BIN");
        }
        const auto offset = raw_lba * raw_sector_size;
        std::array<std::uint8_t, 16> header{};
        input_.clear();
        input_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        input_.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
        if (input_.gcount() != static_cast<std::streamsize>(header.size())) {
            throw ToolError(ExitCode::input, "failed to read raw sector header");
        }
        constexpr std::array<std::uint8_t, 12> sync{
            0x00U, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
            0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x00U,
        };
        if (!std::equal(sync.begin(), sync.end(), header.begin()) || header[15] != 1U) {
            throw ToolError(ExitCode::format,
                "sector " + std::to_string(raw_lba) + " is not MODE1/2352");
        }

        std::array<std::uint8_t, iso_block_size> user_data{};
        input_.seekg(static_cast<std::streamoff>(offset + mode1_user_offset), std::ios::beg);
        input_.read(reinterpret_cast<char*>(user_data.data()), static_cast<std::streamsize>(user_data.size()));
        if (input_.gcount() != static_cast<std::streamsize>(user_data.size())) {
            throw ToolError(ExitCode::input, "failed to read raw sector user data");
        }
        return user_data;
    }

private:
    std::filesystem::path path_;
    std::ifstream input_;
    std::uint32_t base_lba_ = 0;
    std::uint64_t raw_sector_count_ = 0;
};

void assert_both_endian(
    const std::uint32_t little,
    const std::uint32_t big,
    const std::string_view field) {
    if (little != big) {
        throw ToolError(ExitCode::format,
            "ISO 9660 little/big-endian mismatch for " + std::string(field));
    }
}

std::string normalize_iso_name(std::string name) {
    const auto version = name.find(';');
    if (version != std::string::npos) {
        name.erase(version);
    }
    if (!name.empty() && name.back() == '.') {
        name.pop_back();
    }
    return name;
}

std::vector<std::uint8_t> read_extent(
    RawMode1Reader& reader,
    const IsoVolume& volume,
    const std::uint32_t extent,
    const std::uint32_t size) {
    if (size > maximum_directory_bytes) {
        throw ToolError(ExitCode::format, "ISO directory exceeds the safety size limit");
    }
    const auto block_count = (static_cast<std::uint64_t>(size) + iso_block_size - 1U) / iso_block_size;
    if (extent >= volume.block_count
        || static_cast<std::uint64_t>(extent) + block_count > volume.block_count) {
        throw ToolError(ExitCode::format, "ISO directory extent is outside the declared volume");
    }

    std::vector<std::uint8_t> bytes(size);
    std::size_t copied = 0;
    for (std::uint64_t block = 0; block < block_count; ++block) {
        const auto sector = reader.read(static_cast<std::uint32_t>(extent + block));
        const auto amount = std::min(sector.size(), bytes.size() - copied);
        std::copy_n(sector.begin(), amount, bytes.begin() + static_cast<std::ptrdiff_t>(copied));
        copied += amount;
    }
    return bytes;
}

void assert_file_extent(const IsoVolume& volume, const IsoEntry& entry) {
    const auto block_count = (static_cast<std::uint64_t>(entry.size) + iso_block_size - 1U)
        / iso_block_size;
    if (entry.extent >= volume.block_count
        || static_cast<std::uint64_t>(entry.extent) + block_count > volume.block_count) {
        throw ToolError(ExitCode::format,
            "ISO file extent is outside the declared volume: " + entry.path);
    }
}

std::string stream_file(
    RawMode1Reader& reader,
    const IsoVolume& volume,
    const IsoEntry& entry,
    std::ostream& output) {
    if (entry.is_directory) {
        throw ToolError(ExitCode::internal, "cannot stream an ISO directory as a file");
    }
    assert_file_extent(volume, entry);

    mh::common::Sha256 hasher;
    std::uint64_t remaining = entry.size;
    std::uint32_t block = 0U;
    while (remaining > 0U) {
        const auto sector = reader.read(entry.extent + block);
        const auto amount = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, sector.size()));
        output.write(reinterpret_cast<const char*>(sector.data()),
            static_cast<std::streamsize>(amount));
        if (!output) {
            throw ToolError(ExitCode::input, "failed while writing extracted file: " + entry.path);
        }
        hasher.update(sector.data(), amount);
        remaining -= amount;
        ++block;
    }
    return hasher.finish_hex();
}

std::filesystem::path validated_relative_path(const std::string& logical_path) {
    const std::filesystem::path relative(logical_path);
    if (logical_path.empty() || relative.is_absolute() || relative.has_root_name()
        || relative.has_root_directory()) {
        throw ToolError(ExitCode::format, "unsafe ISO extraction path: " + logical_path);
    }
    for (const auto& component : relative) {
        if (component.empty() || component == "." || component == "..") {
            throw ToolError(ExitCode::format, "unsafe ISO extraction path: " + logical_path);
        }
    }
    return relative;
}

std::filesystem::file_status read_symlink_status(
    const std::filesystem::path& path,
    const std::string_view subject) {
    std::error_code error;
    auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return std::filesystem::file_status(std::filesystem::file_type::not_found);
    }
    if (error) {
        throw ToolError(ExitCode::input,
            "cannot inspect " + std::string(subject) + ": " + path.string() + ": " + error.message());
    }
    return status;
}

void ensure_plain_directory(const std::filesystem::path& path) {
    const auto status = read_symlink_status(path, "extraction directory");
    if (std::filesystem::exists(status)) {
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
            throw ToolError(ExitCode::input,
                "extraction path is not a plain directory: " + path.string());
        }
        return;
    }
    std::error_code error;
    if (!std::filesystem::create_directory(path, error) || error) {
        throw ToolError(ExitCode::input,
            "cannot create extraction directory: " + path.string() + ": " + error.message());
    }
}

void ensure_plain_directory_tree(const std::filesystem::path& path) {
    const auto status = read_symlink_status(path, "extraction directory");
    if (std::filesystem::exists(status)) {
        ensure_plain_directory(path);
        return;
    }
    const auto parent = path.parent_path();
    if (parent.empty() || parent == path) {
        throw ToolError(ExitCode::input,
            "cannot determine extraction directory parent: " + path.string());
    }
    ensure_plain_directory_tree(parent);
    ensure_plain_directory(path);
}

void ensure_safe_parent_tree(
    const std::filesystem::path& root,
    const std::filesystem::path& relative_parent) {
    ensure_plain_directory_tree(root);
    auto current = root;
    for (const auto& component : relative_parent) {
        current /= component;
        ensure_plain_directory(current);
    }
}

void validate_existing_parent_tree(
    const std::filesystem::path& root,
    const std::filesystem::path& relative_parent) {
    auto current = root;
    for (const auto& component : relative_parent) {
        current /= component;
        const auto status = read_symlink_status(current, "extraction directory");
        if (!std::filesystem::exists(status)) {
            return;
        }
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
            throw ToolError(ExitCode::input,
                "extraction path is not a plain directory: " + current.string());
        }
    }
}

void walk_directory(
    RawMode1Reader& reader,
    const IsoVolume& volume,
    const std::uint32_t extent,
    const std::uint32_t size,
    const std::string& parent,
    const std::size_t depth,
    std::set<std::uint32_t>& ancestry,
    std::vector<IsoEntry>& output) {
    if (depth > maximum_depth) {
        throw ToolError(ExitCode::format, "ISO directory nesting exceeds the safety limit");
    }
    if (!ancestry.insert(extent).second) {
        throw ToolError(ExitCode::format, "ISO directory cycle detected");
    }

    const auto bytes = read_extent(reader, volume, extent, size);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto record_length = static_cast<std::size_t>(bytes[offset]);
        if (record_length == 0U) {
            offset = ((offset / iso_block_size) + 1U) * iso_block_size;
            continue;
        }
        if (record_length < 34U || offset + record_length > bytes.size()) {
            throw ToolError(ExitCode::format, "malformed ISO directory record");
        }
        const auto* record = bytes.data() + offset;
        const auto name_length = static_cast<std::size_t>(record[32]);
        if (33U + name_length > record_length) {
            throw ToolError(ExitCode::format, "malformed ISO directory identifier");
        }

        const bool special = name_length == 1U && (record[33] == 0U || record[33] == 1U);
        if (!special) {
            const auto child_extent_le = read_le32(record + 2U);
            const auto child_extent_be = read_be32(record + 6U);
            const auto child_size_le = read_le32(record + 10U);
            const auto child_size_be = read_be32(record + 14U);
            assert_both_endian(child_extent_le, child_extent_be, "directory extent");
            assert_both_endian(child_size_le, child_size_be, "file size");

            auto name = normalize_iso_name(std::string(
                reinterpret_cast<const char*>(record + 33U), name_length));
            if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) {
                throw ToolError(ExitCode::format, "unsafe or empty ISO entry name");
            }
            const bool is_directory = (record[25] & 0x02U) != 0U;
            const auto path = parent.empty() ? name : parent + '/' + name;
            output.push_back(IsoEntry{path, is_directory, child_extent_le, child_size_le});
            if (output.size() > maximum_entries) {
                throw ToolError(ExitCode::format, "ISO entry count exceeds the safety limit");
            }
            if (is_directory) {
                walk_directory(reader, volume, child_extent_le, child_size_le,
                    path, depth + 1U, ancestry, output);
            }
        }
        offset += record_length;
    }
    ancestry.erase(extent);
}

} // namespace

IsoReport read_iso9660(
    const std::filesystem::path& raw_bin_path,
    const std::uint32_t data_track_lba) {
    RawMode1Reader reader(raw_bin_path, data_track_lba);
    std::array<std::uint8_t, iso_block_size> primary{};
    bool found_primary = false;
    bool found_terminator = false;
    for (std::uint32_t lba = 16U; lba < 256U; ++lba) {
        const auto descriptor = reader.read(lba);
        const std::string_view identifier(
            reinterpret_cast<const char*>(descriptor.data() + 1U), 5U);
        if (identifier != "CD001" || descriptor[6] != 1U) {
            throw ToolError(ExitCode::format, "invalid ISO 9660 volume descriptor");
        }
        if (descriptor[0] == 1U && !found_primary) {
            primary = descriptor;
            found_primary = true;
        }
        if (descriptor[0] == 255U) {
            found_terminator = true;
            break;
        }
    }
    if (!found_primary || !found_terminator) {
        throw ToolError(ExitCode::format, "ISO 9660 primary descriptor or terminator is missing");
    }

    IsoReport report;
    report.volume.system_id = padded_text(primary.data() + 8U, 32U);
    report.volume.volume_id = padded_text(primary.data() + 40U, 32U);
    const auto block_count_le = read_le32(primary.data() + 80U);
    const auto block_count_be = read_be32(primary.data() + 84U);
    assert_both_endian(block_count_le, block_count_be, "volume block count");
    report.volume.block_count = block_count_le;

    const auto block_size_le = read_le16(primary.data() + 128U);
    const auto block_size_be = read_be16(primary.data() + 130U);
    assert_both_endian(block_size_le, block_size_be, "logical block size");
    report.volume.block_size = block_size_le;
    if (report.volume.block_size != iso_block_size) {
        throw ToolError(ExitCode::format, "only 2048-byte ISO logical blocks are supported");
    }

    const auto* root = primary.data() + 156U;
    if (root[0] < 34U || (root[25] & 0x02U) == 0U) {
        throw ToolError(ExitCode::format, "invalid ISO root directory record");
    }
    report.volume.root_extent = read_le32(root + 2U);
    report.volume.root_size = read_le32(root + 10U);
    assert_both_endian(report.volume.root_extent, read_be32(root + 6U), "root extent");
    assert_both_endian(report.volume.root_size, read_be32(root + 14U), "root size");

    std::set<std::uint32_t> ancestry;
    walk_directory(reader, report.volume, report.volume.root_extent, report.volume.root_size,
        "", 0U, ancestry, report.entries);
    std::sort(report.entries.begin(), report.entries.end(),
        [](const IsoEntry& left, const IsoEntry& right) { return left.path < right.path; });
    return report;
}

std::vector<ExtractedIsoFile> extract_iso9660(
    const std::filesystem::path& raw_bin_path,
    const std::uint32_t data_track_lba,
    const IsoReport& report,
    const std::filesystem::path& output_root,
    const bool overwrite_existing_files) {
    if (output_root.empty()) {
        throw ToolError(ExitCode::usage, "ISO extraction output directory is empty");
    }
    const auto root = std::filesystem::absolute(output_root).lexically_normal();
    ensure_plain_directory_tree(root);

    std::set<std::string> logical_paths;
    for (const auto& entry : report.entries) {
        if (!logical_paths.insert(entry.path).second) {
            throw ToolError(ExitCode::format, "duplicate ISO logical path: " + entry.path);
        }
        const auto relative = validated_relative_path(entry.path);
        validate_existing_parent_tree(root, relative.parent_path());
        const auto destination = (root / relative).lexically_normal();
        const auto status = read_symlink_status(destination, "extraction target");
        if (!std::filesystem::exists(status)) {
            continue;
        }
        if (entry.is_directory) {
            if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
                throw ToolError(ExitCode::input,
                    "extraction directory conflicts with an existing path: " + destination.string());
            }
        } else {
            if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
                throw ToolError(ExitCode::input,
                    "extraction target is not a plain file: " + destination.string());
            }
            if (!overwrite_existing_files) {
                throw ToolError(ExitCode::input,
                    "extraction target already exists (use --overwrite): " + destination.string());
            }
        }
    }

    RawMode1Reader reader(raw_bin_path, data_track_lba);
    logical_paths.clear();
    std::vector<ExtractedIsoFile> extracted;
    extracted.reserve(report.entries.size());
    for (const auto& entry : report.entries) {
        if (!logical_paths.insert(entry.path).second) {
            throw ToolError(ExitCode::format, "duplicate ISO logical path: " + entry.path);
        }
        const auto relative = validated_relative_path(entry.path);
        const auto destination = (root / relative).lexically_normal();
        if (entry.is_directory) {
            ensure_safe_parent_tree(root, relative.parent_path());
            ensure_plain_directory(destination);
            continue;
        }

        ensure_safe_parent_tree(root, relative.parent_path());
        std::error_code error;
        const auto destination_status = read_symlink_status(destination, "extraction target");
        const bool destination_exists = std::filesystem::exists(destination_status);
        if (destination_exists) {
            if (std::filesystem::is_symlink(destination_status)
                || !std::filesystem::is_regular_file(destination_status)) {
                throw ToolError(ExitCode::input,
                    "extraction target is not a plain file: " + destination.string());
            }
            if (!overwrite_existing_files) {
                throw ToolError(ExitCode::input,
                    "extraction target already exists (use --overwrite): " + destination.string());
            }
        }

        auto temporary = destination;
        temporary += ".mhtool-part";
        const auto temporary_status = read_symlink_status(temporary, "temporary extraction target");
        if (std::filesystem::exists(temporary_status)) {
            throw ToolError(ExitCode::input,
                "temporary extraction target already exists: " + temporary.string());
        }

        bool temporary_created = false;
        try {
            std::ofstream output(temporary, std::ios::binary | std::ios::out);
            if (!output) {
                throw ToolError(ExitCode::input,
                    "cannot create extracted file: " + temporary.string());
            }
            temporary_created = true;
            auto hash = stream_file(reader, report.volume, entry, output);
            output.close();
            if (!output) {
                throw ToolError(ExitCode::input,
                    "failed while closing extracted file: " + temporary.string());
            }

            if (destination_exists) {
                if (!std::filesystem::remove(destination, error) || error) {
                    throw ToolError(ExitCode::input,
                        "cannot replace extracted file: " + destination.string() + ": " + error.message());
                }
            }
            std::filesystem::rename(temporary, destination, error);
            if (error) {
                throw ToolError(ExitCode::input,
                    "cannot finalize extracted file: " + destination.string() + ": " + error.message());
            }
            temporary_created = false;
            extracted.push_back(ExtractedIsoFile{entry.path, entry.size, std::move(hash)});
        } catch (...) {
            if (temporary_created) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
            }
            throw;
        }
    }
    return extracted;
}

MaterializedIsoImage materialize_iso9660_image(
    const std::filesystem::path& raw_bin_path,
    const std::uint32_t data_track_lba,
    const IsoReport& report,
    const std::filesystem::path& output_path,
    const bool overwrite_existing_file) {
    if (report.volume.block_size != iso_block_size || report.volume.block_count == 0U) {
        throw ToolError(ExitCode::format, "ISO image has an unsupported logical block layout");
    }
    const auto parent = output_path.has_parent_path()
        ? output_path.parent_path() : std::filesystem::current_path();
    ensure_plain_directory_tree(parent);
    const auto destination_status = read_symlink_status(output_path, "ISO image target");
    const bool destination_exists = std::filesystem::exists(destination_status);
    if (destination_exists) {
        if (std::filesystem::is_symlink(destination_status)
            || !std::filesystem::is_regular_file(destination_status)) {
            throw ToolError(ExitCode::input, "ISO image target is not a plain file");
        }
        if (!overwrite_existing_file) {
            throw ToolError(ExitCode::input, "ISO image target exists (use --overwrite)");
        }
    }
    auto temporary = output_path;
    temporary += ".mhtool-part";
    if (std::filesystem::exists(read_symlink_status(temporary, "temporary ISO image"))) {
        throw ToolError(ExitCode::input, "temporary ISO image target already exists");
    }

    bool temporary_created = false;
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::out | std::ios::trunc);
        if (!output) { throw ToolError(ExitCode::input, "cannot create ISO image"); }
        temporary_created = true;
        RawMode1Reader reader(raw_bin_path, data_track_lba);
        mh::common::Sha256 hasher;
        for (std::uint32_t block = 0U; block < report.volume.block_count; ++block) {
            const auto data = reader.read(block);
            output.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
            if (!output) { throw ToolError(ExitCode::input, "failed while writing ISO image"); }
            hasher.update(data.data(), data.size());
        }
        output.close();
        if (!output) { throw ToolError(ExitCode::input, "failed while closing ISO image"); }
        std::error_code error;
        if (destination_exists && (!std::filesystem::remove(output_path, error) || error)) {
            throw ToolError(ExitCode::input,
                "cannot replace ISO image: " + output_path.string() + ": " + error.message());
        }
        std::filesystem::rename(temporary, output_path, error);
        if (error) {
            throw ToolError(ExitCode::input,
                "cannot finalize ISO image: " + output_path.string() + ": " + error.message());
        }
        temporary_created = false;
        return MaterializedIsoImage{report.volume.block_count,
            static_cast<std::uint64_t>(report.volume.block_count) * iso_block_size,
            hasher.finish_hex()};
    } catch (...) {
        if (temporary_created) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
        }
        throw;
    }
}

} // namespace mh::disc
