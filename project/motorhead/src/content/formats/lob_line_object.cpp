#include <content/formats/lob_line_object.hpp>

#include <core/error.hpp>
#include <content/formats/tbl_envelope.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::uint32_t u32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

float f32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return std::bit_cast<float>(u32(bytes, offset));
}

} // namespace

LobData parse_lob(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 8U) {
        throw ToolError(ExitCode::format, "LOB payload is shorter than its count header");
    }
    LobData data;
    auto& result = data.summary;
    result.vertex_count = u32(bytes, 0U);
    result.edge_count = u32(bytes, 4U);
    if (result.vertex_count == 0U || result.vertex_count > 1U << 24U
        || result.edge_count > 1U << 27U) {
        throw ToolError(ExitCode::format, "LOB record counts are outside supported bounds");
    }
    const auto vertex_bytes = static_cast<std::uint64_t>(result.vertex_count) * 32U;
    const auto edge_bytes = static_cast<std::uint64_t>(result.edge_count) * 8U;
    if (vertex_bytes > std::numeric_limits<std::uint64_t>::max() - edge_bytes - 8U) {
        throw ToolError(ExitCode::format, "LOB record-size calculation overflowed");
    }
    result.expected_bytes = 8U + vertex_bytes + edge_bytes;
    if (result.expected_bytes != bytes.size()) {
        throw ToolError(ExitCode::format, "LOB payload size does not match its 32/8-byte record counts");
    }

    data.vertices.reserve(result.vertex_count);
    for (std::uint32_t vertex = 0U; vertex < result.vertex_count; ++vertex) {
        const auto offset = 8U + static_cast<std::size_t>(vertex) * 32U;
        LobVertex value;
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            value.position[axis] = f32(bytes, offset + axis * 4U);
            if (!std::isfinite(value.position[axis])) {
                throw ToolError(ExitCode::format, "LOB vertex position is not finite");
            }
            if (vertex == 0U) {
                result.minimum[axis] = value.position[axis];
                result.maximum[axis] = value.position[axis];
            } else {
                result.minimum[axis] = std::min(result.minimum[axis], value.position[axis]);
                result.maximum[axis] = std::max(result.maximum[axis], value.position[axis]);
            }
        }
        for (std::size_t word = 0U; word < 5U; ++word) {
            value.preserved_words[word] = u32(bytes, offset + 12U + word * 4U);
        }
        data.vertices.push_back(value);
    }

    const auto edge_base = 8U + static_cast<std::size_t>(result.vertex_count) * 32U;
    result.edge_indices_in_bounds = true;
    for (std::uint32_t edge = 0U; edge < result.edge_count; ++edge) {
        const auto offset = edge_base + static_cast<std::size_t>(edge) * 8U;
        const auto first = u32(bytes, offset);
        const auto second = u32(bytes, offset + 4U);
        data.edges.push_back({first, second});
        result.maximum_edge_index = std::max({result.maximum_edge_index, first, second});
        if (first >= result.vertex_count || second >= result.vertex_count) {
            result.edge_indices_in_bounds = false;
        }
    }
    return data;
}

LobSummary inspect_lob(const std::span<const std::uint8_t> bytes) {
    return parse_lob(bytes).summary;
}

LobData read_lob(const std::filesystem::path& path, std::string key_name,
                 const std::uint64_t maximum_file_bytes) {
    const auto status = std::filesystem::symlink_status(path);
    if (std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status)) {
        throw ToolError(ExitCode::input, "LOB input is not a plain file: " + path.string());
    }
    const auto size = std::filesystem::file_size(path);
    if (size < 8U || size > maximum_file_bytes
        || size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw ToolError(ExitCode::format, "LOB input is outside the configured size bound");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw ToolError(ExitCode::input, "cannot open LOB input: " + path.string()); }
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw ToolError(ExitCode::input, "short read while opening LOB input");
    }
    if (bytes[0U] == 'T' && bytes[1U] == 'B' && bytes[2U] == 'L') {
        const auto envelope = read_tbl(path, std::move(key_name),
            maximum_file_bytes, maximum_file_bytes);
        if (!envelope.decoded) {
            throw ToolError(ExitCode::format,
                "LOB TBL payload is not decoded: " + envelope.decode_status);
        }
        return parse_lob(envelope.payload);
    }
    return parse_lob(bytes);
}

void write_lob_obj(
    const LobData& lob, const std::filesystem::path& output, const bool overwrite) {
    if (output.empty()) { throw ToolError(ExitCode::usage, "LOB OBJ output path is empty"); }
    const auto destination = std::filesystem::absolute(output).lexically_normal();
    const auto parent = destination.parent_path();
    std::error_code error;
    auto current = parent.root_path();
    for (const auto& component : parent.relative_path()) {
        current /= component;
        auto component_status = std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory) {
            error.clear();
            if (!std::filesystem::create_directory(current, error) || error) {
                throw ToolError(ExitCode::input, "cannot create LOB OBJ output directory");
            }
            component_status = std::filesystem::symlink_status(current, error);
        }
        if (error || std::filesystem::is_symlink(component_status)
            || !std::filesystem::is_directory(component_status)) {
            throw ToolError(ExitCode::input, "LOB OBJ output parent is not a plain directory");
        }
    }
    const auto status = std::filesystem::symlink_status(destination, error);
    const bool exists = !error && std::filesystem::exists(status);
    if (exists && (std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status))) {
        throw ToolError(ExitCode::input, "LOB OBJ output is not a plain file");
    }
    if (exists && !overwrite) {
        throw ToolError(ExitCode::input, "LOB OBJ output exists (use --overwrite)");
    }
    auto temporary = destination;
    temporary += ".mhtool-part";
    if (std::filesystem::exists(temporary)) {
        throw ToolError(ExitCode::input, "temporary LOB OBJ output already exists");
    }
    auto backup = destination;
    backup += ".mhtool-backup";
    if (exists && std::filesystem::exists(backup)) {
        throw ToolError(ExitCode::input, "LOB OBJ output backup already exists");
    }

    std::ostringstream text;
    text << "# Motorhead line-object export\n" << std::setprecision(9);
    for (const auto& vertex : lob.vertices) {
        text << "v " << vertex.position[0] << ' ' << vertex.position[1]
             << ' ' << vertex.position[2] << '\n';
    }
    for (const auto& edge : lob.edges) {
        text << "l " << edge.first + 1U << ' ' << edge.second + 1U << '\n';
    }
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << text.str();
        if (!stream) {
            std::filesystem::remove(temporary, error);
            throw ToolError(ExitCode::input, "cannot write LOB OBJ output");
        }
    }
    bool backup_created = false;
    if (exists) {
        std::filesystem::rename(destination, backup, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw ToolError(ExitCode::input, "cannot preserve LOB OBJ output");
        }
        backup_created = true;
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        if (backup_created) {
            std::error_code rollback_error;
            std::filesystem::rename(backup, destination, rollback_error);
        }
        throw ToolError(ExitCode::input, "cannot finalize LOB OBJ output");
    }
    if (backup_created) {
        std::filesystem::remove(backup, error);
        if (error) {
            throw ToolError(ExitCode::input, "cannot remove completed LOB OBJ backup");
        }
    }
}

} // namespace mh::content
