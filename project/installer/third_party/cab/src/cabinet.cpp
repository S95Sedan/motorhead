#include <cabinet.hpp>
#include <core/error.hpp>
#include <core/crypto/sha256.hpp>

#include <zlib.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <string_view>
#include <system_error>
#include <vector>

namespace mh::cab {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::uint64_t maximum_input_bytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t maximum_expanded_bytes = 512ULL * 1024ULL * 1024ULL;
constexpr std::size_t maximum_name_bytes = 4096U;
constexpr std::size_t mszip_window_bytes = 32768U;

class Reader final {
public:
    explicit Reader(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] const std::uint8_t* data(const std::size_t offset) const {
        require(offset, 1U, "data");
        return bytes_.data() + offset;
    }
    [[nodiscard]] std::uint16_t u16(const std::size_t offset) const {
        require(offset, 2U, "16-bit field");
        return static_cast<std::uint16_t>(bytes_[offset])
            | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes_[offset + 1U]) << 8U);
    }
    [[nodiscard]] std::uint32_t u32(const std::size_t offset) const {
        require(offset, 4U, "32-bit field");
        return static_cast<std::uint32_t>(bytes_[offset])
            | (static_cast<std::uint32_t>(bytes_[offset + 1U]) << 8U)
            | (static_cast<std::uint32_t>(bytes_[offset + 2U]) << 16U)
            | (static_cast<std::uint32_t>(bytes_[offset + 3U]) << 24U);
    }
    [[nodiscard]] std::string cstring(const std::size_t offset, std::size_t& end) const {
        require(offset, 1U, "file name");
        end = offset;
        const auto limit = std::min(bytes_.size(), offset + maximum_name_bytes);
        while (end < limit && bytes_[end] != 0U) { ++end; }
        if (end == limit) { throw ToolError(ExitCode::format, "CAB file name is unterminated or oversized"); }
        return std::string(reinterpret_cast<const char*>(bytes_.data() + offset), end - offset);
    }
    void require(const std::size_t offset, const std::size_t amount, const std::string_view field) const {
        if (offset > bytes_.size() || amount > bytes_.size() - offset) {
            throw ToolError(ExitCode::format, "CAB " + std::string(field) + " extends beyond the input");
        }
    }

private:
    std::vector<std::uint8_t> bytes_;
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path)) {
        throw ToolError(ExitCode::input, "CAB input is not a regular file: " + path.string());
    }
    const auto size = std::filesystem::file_size(path);
    if (size > maximum_input_bytes || size > std::numeric_limits<std::size_t>::max()) {
        throw ToolError(ExitCode::format, "CAB input exceeds the safety limit");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw ToolError(ExitCode::input, "cannot open CAB input: " + path.string()); }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) { throw ToolError(ExitCode::input, "cannot read CAB input: " + path.string()); }
    return bytes;
}

std::string safe_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (!path.empty() && path.front() == '/') { path.erase(path.begin()); }
    const std::filesystem::path candidate(path);
    if (path.empty() || candidate.is_absolute() || candidate.has_root_name()) {
        throw ToolError(ExitCode::format, "CAB contains an unsafe empty or absolute path");
    }
    for (const auto& part : candidate) {
        const auto text = part.string();
        if (part == ".." || part == "." || part.empty()
            || text.find(':') != std::string::npos) {
            throw ToolError(ExitCode::format, "CAB contains an unsafe path component");
        }
    }
    return candidate.generic_string();
}

std::filesystem::file_status read_symlink_status(
    const std::filesystem::path& path,
    const std::string_view subject) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
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
    const auto status = read_symlink_status(path, "CAB extraction directory");
    if (std::filesystem::exists(status)) {
        if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
            throw ToolError(ExitCode::input,
                "CAB extraction path is not a plain directory: " + path.string());
        }
        return;
    }
    std::error_code error;
    if (!std::filesystem::create_directory(path, error) || error) {
        throw ToolError(ExitCode::input,
            "cannot create CAB extraction directory: " + path.string() + ": " + error.message());
    }
}

void ensure_plain_directory_tree(const std::filesystem::path& path) {
    const auto status = read_symlink_status(path, "CAB extraction directory");
    if (std::filesystem::exists(status)) {
        ensure_plain_directory(path);
        return;
    }
    const auto parent = path.parent_path();
    if (parent.empty() || parent == path) {
        throw ToolError(ExitCode::input,
            "cannot determine CAB extraction directory parent: " + path.string());
    }
    ensure_plain_directory_tree(parent);
    ensure_plain_directory(path);
}

void ensure_safe_parent_tree(
    const std::filesystem::path& root,
    const std::filesystem::path& relative_parent) {
    auto current = root;
    for (const auto& component : relative_parent) {
        current /= component;
        ensure_plain_directory(current);
    }
}

bool valid_header(const Reader& reader, const std::size_t base) {
    if (base > reader.size() || reader.size() - base < 44U) { return false; }
    if (reader.u32(base) != 0x4643534dU) { return false; }
    const auto cabinet_size = reader.u32(base + 8U);
    if (cabinet_size < 44U || cabinet_size > reader.size() - base) { return false; }
    if (reader.u32(base + 4U) != 0U || reader.u32(base + 12U) != 0U
        || reader.u32(base + 20U) != 0U) { return false; }
    const auto file_table = reader.u32(base + 16U);
    return file_table >= 44U && file_table < cabinet_size
        && reader.u16(base + 26U) == 1U && reader.u16(base + 30U) == 0U;
}

std::uint32_t cab_checksum(
    const std::uint8_t* data,
    std::size_t size,
    std::uint32_t checksum) {
    while (size >= 4U) {
        checksum ^= static_cast<std::uint32_t>(data[0])
            | (static_cast<std::uint32_t>(data[1]) << 8U)
            | (static_cast<std::uint32_t>(data[2]) << 16U)
            | (static_cast<std::uint32_t>(data[3]) << 24U);
        data += 4U;
        size -= 4U;
    }
    std::uint32_t tail = 0U;
    if (size == 3U) {
        tail = (static_cast<std::uint32_t>(data[0]) << 16U)
            | (static_cast<std::uint32_t>(data[1]) << 8U)
            | static_cast<std::uint32_t>(data[2]);
    } else if (size == 2U) {
        tail = (static_cast<std::uint32_t>(data[0]) << 8U)
            | static_cast<std::uint32_t>(data[1]);
    } else if (size == 1U) {
        tail = data[0];
    }
    return checksum ^ tail;
}

std::vector<std::uint8_t> expand_folder(const Cabinet& cabinet, const Reader& reader) {
    const auto base = static_cast<std::size_t>(cabinet.cabinet_offset);
    if (base > reader.size() || cabinet.cabinet_size > reader.size() - base) {
        throw ToolError(ExitCode::format, "CAB extent exceeds the input");
    }
    const auto cabinet_end = base + cabinet.cabinet_size;
    if (cabinet.folder_data_offset > cabinet.cabinet_size) {
        throw ToolError(ExitCode::format, "CAB folder data offset exceeds the cabinet");
    }
    auto cursor = base + cabinet.folder_data_offset;
    std::vector<std::uint8_t> expanded;
    for (std::uint16_t block = 0; block < cabinet.data_block_count; ++block) {
        if (cursor > cabinet_end || 8U > cabinet_end - cursor) {
            throw ToolError(ExitCode::format, "CAB data block header exceeds the cabinet");
        }
        reader.require(cursor, 8U, "data block header");
        const auto block_header = cursor;
        const auto expected_checksum = reader.u32(cursor);
        const auto compressed_size = reader.u16(cursor + 4U);
        const auto uncompressed_size = reader.u16(cursor + 6U);
        cursor += 8U;
        if (cursor > cabinet_end || compressed_size > cabinet_end - cursor) {
            throw ToolError(ExitCode::format, "CAB data block exceeds the cabinet");
        }
        reader.require(cursor, compressed_size, "data block");
        if (expected_checksum != 0U) {
            const auto seed = cab_checksum(reader.data(block_header + 4U), 4U, 0U);
            const auto actual_checksum = cab_checksum(
                reader.data(cursor), compressed_size, seed);
            if (actual_checksum != expected_checksum) {
                throw ToolError(ExitCode::format, "CAB data block checksum mismatch");
            }
        }
        if (expanded.size() + uncompressed_size > maximum_expanded_bytes) {
            throw ToolError(ExitCode::format, "expanded CAB data exceeds the safety limit");
        }
        if ((cabinet.compression & 0x000fU) == 0U) {
            if (compressed_size != uncompressed_size) {
                throw ToolError(ExitCode::format, "uncompressed CAB block size mismatch");
            }
            expanded.insert(expanded.end(), reader.data(cursor), reader.data(cursor) + compressed_size);
        } else {
            if (uncompressed_size == 0U || compressed_size <= 2U
                || reader.data(cursor)[0] != 'C' || reader.data(cursor)[1] != 'K') {
                throw ToolError(ExitCode::format, "MSZIP CAB block is missing its CK signature");
            }
            std::vector<std::uint8_t> output(uncompressed_size);
            z_stream stream{};
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
                throw ToolError(ExitCode::internal, "cannot initialize MSZIP decompression");
            }
            const auto cleanup = [&stream] { static_cast<void>(inflateEnd(&stream)); };
            if (!expanded.empty()) {
                const auto dictionary_size = std::min(expanded.size(), mszip_window_bytes);
                const auto* dictionary = expanded.data() + expanded.size() - dictionary_size;
                if (inflateSetDictionary(&stream, dictionary, static_cast<uInt>(dictionary_size)) != Z_OK) {
                    cleanup();
                    throw ToolError(ExitCode::format, "cannot set the MSZIP history dictionary");
                }
            }
            stream.next_in = const_cast<Bytef*>(reader.data(cursor + 2U));
            stream.avail_in = static_cast<uInt>(compressed_size - 2U);
            stream.next_out = output.data();
            stream.avail_out = static_cast<uInt>(output.size());
            const auto result = inflate(&stream, Z_FINISH);
            const auto produced = stream.total_out;
            cleanup();
            if (result != Z_STREAM_END || produced != uncompressed_size) {
                throw ToolError(ExitCode::format, "MSZIP CAB block did not expand to its declared size");
            }
            expanded.insert(expanded.end(), output.begin(), output.end());
        }
        cursor += compressed_size;
    }
    return expanded;
}

} // namespace

Cabinet find_embedded_cabinet(const std::filesystem::path& path) {
    Reader reader(read_file(path));
    std::vector<std::size_t> candidates;
    for (std::size_t offset = 0; offset + 4U <= reader.size(); ++offset) {
        if (reader.data(offset)[0] == 'M' && reader.data(offset)[1] == 'S'
            && reader.data(offset)[2] == 'C' && reader.data(offset)[3] == 'F'
            && valid_header(reader, offset)) {
            candidates.push_back(offset);
        }
    }
    if (candidates.size() != 1U) {
        throw ToolError(ExitCode::format,
            "expected exactly one valid embedded CAB, found " + std::to_string(candidates.size()));
    }
    const auto base = candidates.front();
    Cabinet cabinet;
    cabinet.source_path = path;
    cabinet.cabinet_offset = base;
    cabinet.cabinet_size = reader.u32(base + 8U);
    cabinet.file_table_offset = reader.u32(base + 16U);
    cabinet.folder_data_offset = reader.u32(base + 36U);
    cabinet.data_block_count = reader.u16(base + 40U);
    cabinet.compression = reader.u16(base + 42U);
    if ((cabinet.compression & 0x000fU) > 1U) {
        throw ToolError(ExitCode::format, "CAB compression is not uncompressed or MSZIP");
    }
    auto cursor = base + cabinet.file_table_offset;
    const auto file_count = reader.u16(base + 28U);
    std::uint64_t maximum_end = 0U;
    for (std::uint16_t index = 0; index < file_count; ++index) {
        reader.require(cursor, 17U, "file entry");
        CabFile file;
        file.size = reader.u32(cursor);
        file.folder_offset = reader.u32(cursor + 4U);
        file.folder_index = reader.u16(cursor + 8U);
        file.attributes = reader.u16(cursor + 14U);
        if (file.folder_index != 0U) {
            throw ToolError(ExitCode::format, "multi-folder or continued CAB file is unsupported");
        }
        std::size_t name_end = 0U;
        file.path = safe_path(reader.cstring(cursor + 16U, name_end));
        cursor = name_end + 1U;
        const auto end = static_cast<std::uint64_t>(file.folder_offset) + file.size;
        if (end > maximum_expanded_bytes) {
            throw ToolError(ExitCode::format, "CAB file extent exceeds the safety limit");
        }
        maximum_end = std::max(maximum_end, end);
        cabinet.files.push_back(std::move(file));
    }
    static_cast<void>(maximum_end);
    return cabinet;
}

std::vector<ExtractedCabFile> extract_cabinet(
    const Cabinet& cabinet,
    const std::filesystem::path& output_root,
    const bool overwrite) {
    if (output_root.empty()) {
        throw ToolError(ExitCode::usage, "CAB extraction output directory is empty");
    }
    Reader reader(read_file(cabinet.source_path));
    const auto expanded = expand_folder(cabinet, reader);
    const auto root = std::filesystem::absolute(output_root).lexically_normal();
    ensure_plain_directory_tree(root);
    std::vector<std::filesystem::path> targets;
    std::set<std::string> paths;
    for (const auto& file : cabinet.files) {
        if (static_cast<std::uint64_t>(file.folder_offset) + file.size > expanded.size()) {
            throw ToolError(ExitCode::format, "CAB file extent exceeds expanded folder data");
        }
        const auto logical_path = safe_path(file.path);
        if (!paths.insert(logical_path).second) {
            throw ToolError(ExitCode::format, "duplicate CAB path: " + logical_path);
        }
        const std::filesystem::path relative(logical_path);
        auto current = root;
        for (const auto& component : relative.parent_path()) {
            current /= component;
            const auto status = read_symlink_status(current, "CAB extraction directory");
            if (!std::filesystem::exists(status)) { break; }
            if (std::filesystem::is_symlink(status) || !std::filesystem::is_directory(status)) {
                throw ToolError(ExitCode::input,
                    "CAB extraction path is not a plain directory: " + current.string());
            }
        }
        const auto target = (root / relative).lexically_normal();
        const auto status = read_symlink_status(target, "CAB extraction target");
        if (std::filesystem::exists(status)) {
            if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
                throw ToolError(ExitCode::input,
                    "CAB extraction target is not a plain file: " + target.string());
            }
            if (!overwrite) {
                throw ToolError(ExitCode::input,
                    "CAB extraction target already exists (use --overwrite): " + target.string());
            }
        }
        targets.push_back(target);
    }
    std::vector<ExtractedCabFile> result;
    for (std::size_t index = 0; index < cabinet.files.size(); ++index) {
        const auto& file = cabinet.files[index];
        const auto& target = targets[index];
        const auto relative = std::filesystem::path(safe_path(file.path));
        ensure_safe_parent_tree(root, relative.parent_path());
        const bool target_exists = std::filesystem::exists(
            read_symlink_status(target, "CAB extraction target"));
        auto temporary = target;
        temporary += ".mhtool-part";
        if (std::filesystem::exists(read_symlink_status(temporary, "temporary CAB target"))) {
            throw ToolError(ExitCode::input,
                "temporary CAB extraction target already exists: " + temporary.string());
        }
        bool temporary_created = false;
        try {
            std::ofstream output(temporary, std::ios::binary | std::ios::out);
            if (!output) { throw ToolError(ExitCode::input, "cannot create CAB extraction output"); }
            temporary_created = true;
            output.write(reinterpret_cast<const char*>(expanded.data() + file.folder_offset), file.size);
            output.close();
            if (!output) { throw ToolError(ExitCode::input, "cannot write CAB extraction output"); }
            std::error_code error;
            if (target_exists && (!std::filesystem::remove(target, error) || error)) {
                throw ToolError(ExitCode::input,
                    "cannot replace CAB extraction target: " + target.string() + ": " + error.message());
            }
            std::filesystem::rename(temporary, target, error);
            if (error) {
                throw ToolError(ExitCode::input,
                    "cannot finalize CAB extraction target: " + target.string() + ": " + error.message());
            }
            temporary_created = false;
            result.push_back(ExtractedCabFile{
                relative.generic_string(), file.size, mh::common::sha256_file(target)});
        } catch (...) {
            if (temporary_created) {
                std::error_code ignored;
                std::filesystem::remove(temporary, ignored);
            }
            throw;
        }
    }
    return result;
}

} // namespace mh::cab
