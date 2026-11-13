#include <services/asset_inventory.hpp>

#include <core/error.hpp>
#include <core/sha256.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <string_view>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::string ascii_lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char value) {
        return static_cast<char>(value >= static_cast<unsigned char>('A')
                && value <= static_cast<unsigned char>('Z')
            ? value + static_cast<unsigned char>('a' - 'A') : value);
    });
    return text;
}

bool begins_with(const std::vector<std::uint8_t>& bytes, const std::string_view signature) {
    return bytes.size() >= signature.size()
        && std::equal(signature.begin(), signature.end(), bytes.begin());
}

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset])
        | (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint32_t read_be32(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U)
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U)
        | bytes[offset + 3U];
}

FormatIdentification recognized(
    std::string format, std::string category, std::string evidence,
    const double confidence = 1.0) {
    return FormatIdentification{
        std::move(format), std::move(category), "recognized", confidence, std::move(evidence)};
}

FormatIdentification provisional(
    std::string format, std::string category, std::string evidence) {
    return FormatIdentification{
        std::move(format), std::move(category), "provisional", 0.5, std::move(evidence)};
}

bool looks_textual(const std::vector<std::uint8_t>& bytes) {
    if (bytes.empty()) { return false; }
    for (const auto value : bytes) {
        if (value == 0U) { return false; }
        if (value == 9U || value == 10U || value == 13U) { continue; }
        if (value < 0x20U || value == 0x7fU) { return false; }
    }
    return true;
}

std::string hex_bytes(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto value : bytes) {
        output << std::setw(2) << static_cast<unsigned>(value);
    }
    return output.str();
}

std::vector<std::uint8_t> read_prefix(
    const std::filesystem::path& path, const std::uint64_t file_size) {
    constexpr std::uint64_t maximum = 4096U;
    const auto amount = static_cast<std::size_t>(std::min(file_size, maximum));
    std::vector<std::uint8_t> bytes(amount);
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw ToolError(ExitCode::input, "cannot open content file: " + path.string()); }
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw ToolError(ExitCode::input, "short read while scanning content file: " + path.string());
    }
    return bytes;
}

std::string logical_id(const std::filesystem::path& relative) {
    return ascii_lower(relative.generic_string());
}

} // namespace

FormatIdentification identify_format(
    const std::filesystem::path& path,
    const std::vector<std::uint8_t>& header,
    const std::uint64_t file_size) {
    const auto extension = ascii_lower(path.extension().string());
    if (begins_with(header, "DIVI")) {
        return recognized("motorhead-divi", "movie_container",
            "exact DIVI file signature (observed MOVIES.PAK container)");
    }
    if (begins_with(header, "2OYM")) {
        return recognized("motorhead-myo-v2", "model",
            "exact little-endian MYO v2 signature 2OYM");
    }
    if (begins_with(header, "!OYM")) {
        return recognized("motorhead-myo-v1", "model",
            "exact little-endian MYO v1 signature !OYM");
    }
    if (begins_with(header, "PDI1")) {
        return recognized("motorhead-pdi-v1", "packed_texture",
            "exact PDI1 packed-texture signature");
    }
    if (header.size() >= 4U && header[0] == 'T' && header[1] == 'B' && header[2] == 'L') {
        return recognized("motorhead-tbl-flags-" + std::to_string(header[3]), "file_envelope",
            "exact TBL envelope signature with flags byte " + std::to_string(header[3]));
    }
    if (begins_with(header, "OggS")) {
        return recognized("ogg", "audio", "exact OggS capture signature");
    }
    if (header.size() >= 12U && begins_with(header, "RIFF")
        && std::equal(header.begin() + 8, header.begin() + 12, "WAVE")) {
        return recognized("riff-wave", "audio", "exact RIFF/WAVE signature");
    }
    if (begins_with(header, "MZ")) {
        return recognized("dos-mz-or-pe", "executable", "exact MZ executable signature");
    }
    if (begins_with(header, "MSCF")) {
        return recognized("microsoft-cab", "archive", "exact MSCF cabinet signature");
    }
    if (header.size() >= 4U && header[0] == 'P' && header[1] == 'K'
        && header[2] == 3U && header[3] == 4U) {
        return recognized("zip", "archive", "exact ZIP local-header signature");
    }
    if (begins_with(header, "BM")) {
        return recognized("bmp", "image", "exact bitmap signature");
    }
    if (begins_with(header, "SMK2") || begins_with(header, "SMK4")) {
        return recognized("smacker", "movie", "exact Smacker stream signature");
    }
    if (header.size() >= 12U && begins_with(header, "FORM")
        && std::equal(header.begin() + 8, header.begin() + 12, "ILBM")) {
        const auto form_size = static_cast<std::uint64_t>(read_be32(header, 4U));
        const auto expected_size = 8ULL + form_size + (form_size & 1U);
        if (form_size < 4U || expected_size != file_size) {
            return FormatIdentification{"iff-ilbm", "image", "invalid", 1.0,
                "FORM/ILBM signature with a mismatched bounded FORM size"};
        }
        return recognized("iff-ilbm", "image",
            "exact FORM/ILBM signature and bounded big-endian FORM size");
    }

    if (extension == ".tga") {
        if (header.size() < 18U) {
            return FormatIdentification{"tga", "image", "invalid", 1.0,
                "TGA extension but file is shorter than the 18-byte header"};
        }
        const auto color_map_type = header[1];
        const auto image_type = header[2];
        const auto width = read_u16(header, 12U);
        const auto height = read_u16(header, 14U);
        const auto depth = header[16];
        const std::set<std::uint8_t> image_types{1U, 2U, 3U, 9U, 10U, 11U};
        const std::set<std::uint8_t> depths{8U, 15U, 16U, 24U, 32U};
        const auto color_map_bytes = static_cast<std::uint64_t>(read_u16(header, 5U))
            * ((static_cast<std::uint64_t>(header[7]) + 7U) / 8U);
        const auto first_pixel = 18ULL + header[0] + color_map_bytes;
        if (color_map_type > 1U || !image_types.contains(image_type)
            || !depths.contains(depth) || width == 0U || height == 0U
            || first_pixel > file_size) {
            return FormatIdentification{"tga", "image", "invalid", 1.0,
                "TGA extension with an invalid or out-of-bounds structural header"};
        }
        return recognized("tga", "image",
            "bounds-checked TGA header: " + std::to_string(width) + "x"
                + std::to_string(height) + "x" + std::to_string(depth),
            0.95);
    }

    if (extension == ".col") {
        if (header.size() < 40U) {
            return FormatIdentification{"motorhead-col", "collision", "invalid", 1.0,
                "COL extension but file is shorter than the fixed header"};
        }
        const auto prefix_bytes = read_u16(header, 0U);
        const auto version = read_u16(header, 2U);
        const auto face_count = read_u16(header, 10U);
        const auto spatial_16_count = read_u16(header, 12U);
        const auto spatial_4_count = read_u16(header, 14U);
        const auto plane_count = read_u16(header, 18U);
        const auto expected = static_cast<std::uint64_t>(prefix_bytes)
            + static_cast<std::uint64_t>(spatial_16_count) * 16U
            + static_cast<std::uint64_t>(spatial_4_count) * 4U
            + static_cast<std::uint64_t>(face_count) * 28U
            + static_cast<std::uint64_t>(plane_count) * 16U;
        if (prefix_bytes < 40U || (prefix_bytes & 1U) != 0U
            || (version != 0U && version != 2U && version != 3U)
            || read_u16(header, 4U) == 0U || read_u16(header, 6U) == 0U
            || read_u16(header, 8U) == 0U || expected != file_size) {
            return FormatIdentification{"motorhead-col", "collision", "invalid", 1.0,
                "COL extension with an invalid loader-derived size equation"};
        }
        return recognized("motorhead-col-v" + std::to_string(version), "collision",
            "p3.1-loader-derived prefix and four-section size equation", 0.95);
    }

    if (extension == ".myw") {
        if (header.size() < 240U || read_u32(header, 0U) != 0x4d595721U) {
            return FormatIdentification{"motorhead-myw-v1", "world", "invalid", 1.0,
                "MYW extension with an invalid MYW! header"};
        }
        const auto width = read_u32(header, 8U);
        const auto height = read_u32(header, 12U);
        const auto grid_offset = read_u32(header, 0x58U);
        const auto grid_end = static_cast<std::uint64_t>(grid_offset)
            + static_cast<std::uint64_t>(width) * height * 24U;
        bool valid = read_u32(header, 4U) == file_size && width != 0U && height != 0U
            && grid_offset >= 240U && grid_end <= file_size;
        constexpr std::array<std::uint32_t, 12U> fixed_strides{
            228U, 12U, 24U, 0U, 16U, 12U, 12U, 0U, 196U, 8U, 0U, 120U};
        for (std::size_t index = 0U; index < fixed_strides.size(); ++index) {
            const auto base = 0x5cU + index * 12U;
            const auto count = read_u32(header, base);
            const auto size = read_u32(header, base + 4U);
            const auto offset = read_u32(header, base + 8U);
            valid = valid && offset >= 240U
                && static_cast<std::uint64_t>(offset) + size <= file_size
                && (fixed_strides[index] == 0U
                    || static_cast<std::uint64_t>(count) * fixed_strides[index] == size);
        }
        if (!valid) {
            return FormatIdentification{"motorhead-myw-v1", "world", "invalid", 1.0,
                "MYW extension with invalid loader-derived sections"};
        }
        return recognized("motorhead-myw-v1", "world",
            "MYW! signature, declared size, grid, and twelve bounded sections", 0.98);
    }

    if (extension == ".act" && file_size == 768U) {
        return recognized("rgb-palette-256", "palette",
            "ACT extension and exact 256-entry RGB palette byte count", 0.95);
    }

    if (looks_textual(header)) {
        const bool configuration = extension == ".cfg" || extension == ".ini"
            || extension == ".conf";
        return recognized(configuration ? "text-config" : "text", configuration
                ? "configuration" : "text",
            "prefix contains only printable text and whitespace", 0.9);
    }

    const std::map<std::string, std::pair<std::string, std::string>> known_extensions{
        {".myo", {"motorhead-myo-unknown-version", "model"}},
        {".lob", {"motorhead-lob", "model"}},
        {".trk", {"motorhead-track", "track"}},
        {".spr", {"motorhead-sprite", "sprite"}},
        {".fnt", {"motorhead-font", "font"}},
        {".mde", {"motorhead-ghost", "replay"}},
        {".pdi", {"motorhead-pdi-unknown-version", "packed_texture"}},
        {".iff", {"motorhead-iff-non-ilbm", "asset"}},
        {".pak", {"motorhead-pak-unknown", "archive"}},
        {".dta", {"motorhead-data", "data"}},
        {".mot", {"motorhead-motion", "animation"}},
        {".chf", {"motorhead-chassis", "vehicle"}},
        {".car", {"motorhead-car-config", "vehicle"}},
        {".srf", {"motorhead-surface", "material"}},
        {".sbk", {"motorhead-sound-bank", "audio"}},
    };
    if (const auto known = known_extensions.find(extension); known != known_extensions.end()) {
        return provisional(known->second.first, known->second.second,
            "extension-only classification awaiting a bounds-checked format parser");
    }
    const auto stem = ascii_lower(path.stem().string());
    if (extension == ".dat" && stem == "ai") {
        return provisional("motorhead-ai-path-data", "ai",
            "AI.DAT location/name classification awaiting a bounds-checked parser");
    }
    if (extension == ".bin" && stem.rfind("remap", 0U) == 0U) {
        return provisional("motorhead-track-remap", "track",
            "remap*.BIN naming classification awaiting a bounds-checked parser");
    }
    return FormatIdentification{"unknown", "unknown", "unknown", 0.0,
        extension.empty() ? "no recognized signature or extension"
                          : "unrecognized signature and extension " + extension};
}

Inventory inventory_tree(const std::filesystem::path& root) {
    std::error_code error;
    const auto absolute = std::filesystem::absolute(root, error).lexically_normal();
    if (error || !std::filesystem::is_directory(absolute)) {
        throw ToolError(ExitCode::input, "content inventory root is not a directory");
    }
    std::vector<std::filesystem::path> paths;
    std::filesystem::recursive_directory_iterator iterator(
        absolute, std::filesystem::directory_options::none, error), end;
    if (error) {
        throw ToolError(ExitCode::input, "cannot enumerate content inventory root");
    }
    while (iterator != end) {
        const auto status = iterator->symlink_status(error);
        if (error) { throw ToolError(ExitCode::input, "cannot inspect content path status"); }
        if (std::filesystem::is_symlink(status)) {
            throw ToolError(ExitCode::format, "content inventory refuses symbolic links");
        }
        if (std::filesystem::is_regular_file(status)) { paths.push_back(iterator->path()); }
        iterator.increment(error);
        if (error) {
            throw ToolError(ExitCode::input, "failed while enumerating content inventory root");
        }
    }
    std::sort(paths.begin(), paths.end(), [&absolute](const auto& left, const auto& right) {
        return std::filesystem::relative(left, absolute).generic_string()
            < std::filesystem::relative(right, absolute).generic_string();
    });

    Inventory inventory;
    inventory.root = absolute;
    mh::common::Sha256 manifest;
    std::set<std::string> logical_ids;
    for (const auto& path : paths) {
        const auto relative = std::filesystem::relative(path, absolute);
        const auto id = logical_id(relative);
        if (!logical_ids.insert(id).second) {
            throw ToolError(ExitCode::format,
                "case-folded logical asset ID collision: " + id);
        }
        const auto size = std::filesystem::file_size(path);
        const auto prefix = read_prefix(path, size);
        const auto header_size = std::min<std::size_t>(32U, prefix.size());
        const std::vector<std::uint8_t> header(prefix.begin(), prefix.begin() + header_size);
        InventoryFile file;
        file.relative_path = relative.generic_string();
        file.logical_id = id;
        file.extension = ascii_lower(path.extension().string());
        file.bytes = size;
        file.sha256 = mh::common::sha256_file(path);
        file.header_hex = hex_bytes(header);
        file.identification = identify_format(path, prefix, size);
        inventory.total_bytes += size;
        ++inventory.format_counts[file.identification.format_id];
        ++inventory.extension_counts[file.extension.empty() ? "<none>" : file.extension];
        if (file.identification.status == "recognized") { ++inventory.recognized_files; }
        else if (file.identification.status == "provisional") { ++inventory.provisional_files; }
        else if (file.identification.status == "invalid") { ++inventory.invalid_files; }
        else { ++inventory.unknown_files; }
        const auto canonical = file.relative_path + "\0" + std::to_string(file.bytes) + "\0"
            + file.sha256 + "\0" + file.identification.format_id + "\0"
            + file.identification.status + "\n";
        manifest.update(canonical);
        inventory.files.push_back(std::move(file));
    }
    inventory.manifest_sha256 = manifest.finish_hex();
    return inventory;
}

} // namespace mh::content
