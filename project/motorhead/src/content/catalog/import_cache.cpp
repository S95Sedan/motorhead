#include <content/catalog/import_cache.hpp>

#include <core/error.hpp>
#include <core/serialization/json.hpp>
#include <core/crypto/sha256.hpp>
#include <content/formats/ai_route.hpp>
#include <content/formats/col_collision_mesh.hpp>
#include <content/formats/iff_image.hpp>
#include <content/catalog/content_inventory.hpp>
#include <content/formats/myo_object_model.hpp>
#include <content/formats/myw_world_model.hpp>
#include <content/formats/pdi_archive.hpp>
#include <content/formats/tga_image.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::string_view object_schema = "motorhead.import-cache-object.v1";
constexpr std::string_view importer_id = "inventory-descriptor-v1";
constexpr std::string_view parsed_object_schema =
    "motorhead.import-cache-parsed-object.v1";
constexpr std::string_view parsed_importer_id = "bounded-parser-summary-v1";
constexpr std::uint64_t maximum_descriptor_bytes = 1024ULL * 1024ULL;
constexpr std::uint64_t maximum_manifest_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t maximum_index_bytes = 64ULL * 1024ULL * 1024ULL;
constexpr std::array<char, 8U> index_magic{
    'M', 'H', 'I', 'D', 'X', '1', '\r', '\n'};

struct CachePlan {
    ImportCacheResult result;
    std::vector<std::string> object_contents;
    std::vector<std::string> parsed_object_contents;
    std::string index_content;
    std::string manifest_content;
};

std::filesystem::path normalized_absolute(const std::filesystem::path& path) {
    std::error_code error;
    const auto result = std::filesystem::absolute(path, error).lexically_normal();
    if (error || result.empty()) {
        throw ToolError(ExitCode::input, "cannot resolve import-cache path");
    }
    return result;
}

bool is_within(const std::filesystem::path& candidate,
    const std::filesystem::path& root) {
    const auto relative = candidate.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute()
        && *relative.begin() != "..";
}

void validate_roots(const std::filesystem::path& content_root,
    const std::filesystem::path& cache_root, const bool cache_must_exist) {
    if (!std::filesystem::is_directory(content_root)) {
        throw ToolError(ExitCode::input, "import-cache content root is not a directory");
    }
    if (content_root == cache_root || is_within(cache_root, content_root)
        || is_within(content_root, cache_root)) {
        throw ToolError(ExitCode::usage,
            "import-cache content and cache roots must not overlap");
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(cache_root, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw ToolError(ExitCode::input, "cannot inspect import-cache root");
    }
    if (!error && std::filesystem::is_symlink(status)) {
        throw ToolError(ExitCode::format, "import cache refuses a symbolic-link root");
    }
    if (cache_must_exist && !std::filesystem::is_directory(cache_root)) {
        throw ToolError(ExitCode::input, "import-cache root is not a directory");
    }
    if (!cache_must_exist && std::filesystem::exists(cache_root)
        && !std::filesystem::is_directory(cache_root)) {
        throw ToolError(ExitCode::input, "import-cache root is not a directory");
    }
}

std::string read_bounded_text(const std::filesystem::path& path,
    const std::uint64_t maximum_bytes) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status)) {
        throw ToolError(ExitCode::verification,
            "import-cache file is missing, not regular, or symbolic: "
                + path.string());
    }
    const auto bytes = std::filesystem::file_size(path, error);
    if (error || bytes > maximum_bytes) {
        throw ToolError(ExitCode::verification,
            "import-cache file is missing or exceeds its size bound: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw ToolError(ExitCode::verification,
            "cannot read import-cache file: " + path.string());
    }
    std::string content(static_cast<std::size_t>(bytes), '\0');
    input.read(content.data(), static_cast<std::streamsize>(content.size()));
    if (input.gcount() != static_cast<std::streamsize>(content.size())) {
        throw ToolError(ExitCode::verification,
            "short read from import-cache file: " + path.string());
    }
    return content;
}

std::string object_path_for(const std::string_view sha256) {
    return "objects/sha256/" + std::string(sha256.substr(0U, 2U)) + '/'
        + std::string(sha256.substr(2U)) + ".json";
}

std::string manifest_path_for(const std::string_view sha256) {
    return "manifests/sha256/" + std::string(sha256.substr(0U, 2U)) + '/'
        + std::string(sha256.substr(2U)) + ".json";
}

std::string parsed_object_path_for(const std::string_view sha256) {
    return "parsed/sha256/" + std::string(sha256.substr(0U, 2U)) + '/'
        + std::string(sha256.substr(2U)) + ".json";
}

std::string index_path_for(const std::string_view sha256) {
    return "indexes/sha256/" + std::string(sha256.substr(0U, 2U)) + '/'
        + std::string(sha256.substr(2U)) + ".mhidx";
}

void append_u32(std::string& output, const std::uint32_t value) {
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        output.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

void append_u64(std::string& output, const std::uint64_t value) {
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
        output.push_back(static_cast<char>((value >> shift) & 0xffU));
    }
}

void append_index_string(std::string& output, const std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw ToolError(ExitCode::format, "import-cache index string is too large");
    }
    append_u32(output, static_cast<std::uint32_t>(value.size()));
    output.append(value);
}

std::string make_index_content(const std::vector<ImportCacheEntry>& entries) {
    if (entries.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw ToolError(ExitCode::format, "import-cache index has too many entries");
    }
    std::vector<const ImportCacheEntry*> sorted;
    sorted.reserve(entries.size());
    for (const auto& entry : entries) { sorted.push_back(&entry); }
    std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
        return left->logical_id < right->logical_id;
    });
    std::string output(index_magic.begin(), index_magic.end());
    append_u32(output, static_cast<std::uint32_t>(sorted.size()));
    for (const auto* entry : sorted) {
        append_index_string(output, entry->logical_id);
        append_index_string(output, entry->source_relative_path);
        append_index_string(output, entry->source_sha256);
        append_u64(output, entry->source_bytes);
        append_index_string(output, entry->format_id);
        append_index_string(output, entry->status);
        append_index_string(output, entry->object_sha256);
        append_index_string(output, entry->object_relative_path);
        append_u64(output, entry->object_bytes);
        append_index_string(output, entry->parsed_importer);
        append_index_string(output, entry->parsed_object_sha256);
        append_index_string(output, entry->parsed_object_relative_path);
        append_u64(output, entry->parsed_object_bytes);
    }
    return output;
}

template <typename Value, std::size_t Size>
void write_array(std::ostringstream& output, const std::array<Value, Size>& values) {
    output << '[';
    for (std::size_t index = 0U; index < Size; ++index) {
        if (index != 0U) { output << ','; }
        output << values[index];
    }
    output << ']';
}

std::optional<std::string> make_parsed_object_content(
    const InventoryFile& file, const std::filesystem::path& source) {
    std::ostringstream summary;
    summary << std::setprecision(std::numeric_limits<double>::max_digits10);
    const auto& format = file.identification.format_id;
    try {
    if (format == "motorhead-myo-v1" || format == "motorhead-myo-v2") {
        const auto model = read_myo(source);
        summary << "{\"version\":" << static_cast<unsigned>(model.version)
                << ",\"positions\":" << model.positions.size()
                << ",\"normals\":" << model.normals.size()
                << ",\"faces\":" << model.faces.size()
                << ",\"names\":" << model.names.size()
                << ",\"triangles\":" << model.triangle_count
                << ",\"quads\":" << model.quad_count
                << ",\"textured_faces\":" << model.textured_face_count
                << ",\"invalid_referenced_normals\":"
                << model.invalid_referenced_normals << ",\"minimum\":";
        write_array(summary, model.minimum);
        summary << ",\"maximum\":";
        write_array(summary, model.maximum);
        summary << '}';
    } else if (format == "motorhead-myw-v1") {
        const auto world = read_myw(source);
        summary << "{\"grid_width\":" << world.grid_width
                << ",\"grid_height\":" << world.grid_height
                << ",\"occupied_grid_cells\":" << world.occupied_grid_cells
                << ",\"grid_links\":" << world.grid_link_count
                << ",\"positions\":" << world.positions.size()
                << ",\"planes\":" << world.planes.size()
                << ",\"normals\":" << world.normals.size()
                << ",\"objects\":" << world.objects.size()
                << ",\"primitives\":" << world.primitives.size()
                << ",\"triangles\":" << world.triangle_count
                << ",\"quads\":" << world.quad_count
                << ",\"lights\":" << world.lights.size()
                << ",\"lens_flares\":" << world.lens_flares.size()
                << ",\"materials\":" << world.material_names.size()
                << ",\"unsupported_primitives\":"
                << world.unsupported_primitive_count << ",\"minimum\":";
        write_array(summary, world.minimum);
        summary << ",\"maximum\":";
        write_array(summary, world.maximum);
        summary << '}';
    } else if (format.starts_with("motorhead-col-v")) {
        const auto collision = read_col(source);
        summary << "{\"version\":" << collision.version
                << ",\"grid_dimensions\":";
        write_array(summary, collision.grid_dimensions);
        summary << ",\"grid_cells\":" << collision.grid_cell_count
                << ",\"occupied_grid_cells\":" << collision.occupied_grid_cells
                << ",\"spatial_nodes\":" << collision.spatial_nodes.size()
                << ",\"leaf_lists\":" << collision.leaf_list_count
                << ",\"faces\":" << collision.faces.size()
                << ",\"planes\":" << collision.planes.size()
                << ",\"reachable_faces\":" << collision.reachable_face_count
                << ",\"active_boundary_references\":"
                << collision.active_boundary_references
                << ",\"out_of_range_boundary_references\":"
                << collision.boundary_references_outside_plane_table << '}';
    } else if (format == "tga") {
        const auto image = read_tga(source);
        summary << "{\"width\":" << image.width
                << ",\"height\":" << image.height
                << ",\"image_type\":" << static_cast<unsigned>(image.image_type)
                << ",\"pixel_depth\":" << static_cast<unsigned>(image.pixel_depth)
                << ",\"palette_entries\":" << image.palette_length
                << ",\"rgba_bytes\":" << image.rgba.size()
                << ",\"consumed_bytes\":" << image.consumed_bytes
                << ",\"trailing_bytes\":" << image.trailing_bytes << '}';
    } else if (format == "iff-ilbm") {
        const auto image = read_iff_ilbm(source);
        summary << "{\"width\":" << image.width
                << ",\"height\":" << image.height
                << ",\"planes\":" << static_cast<unsigned>(image.plane_count)
                << ",\"masking\":" << static_cast<unsigned>(image.masking)
                << ",\"compression\":" << static_cast<unsigned>(image.compression)
                << ",\"palette_entries\":" << image.palette_entries
                << ",\"chunks\":" << image.chunk_count
                << ",\"body_bytes\":" << image.body_bytes
                << ",\"rgba_bytes\":" << image.rgba.size()
                << ",\"transparent_pixels\":" << image.transparent_pixels << '}';
    } else if (format == "motorhead-pdi-v1") {
        const auto archive = read_pdi(source);
        summary << "{\"capacity\":" << archive.capacity
                << ",\"entries\":" << archive.entry_count
                << ",\"header_bytes\":" << archive.header_bytes
                << ",\"directory_offset\":" << archive.directory_offset
                << ",\"payload_offset\":" << archive.payload_offset
                << ",\"payload_bytes\":" << archive.payload_bytes
                << ",\"pixels\":" << archive.pixel_count
                << ",\"transparent_pixels\":" << archive.transparent_pixels << '}';
    } else if (file.logical_id.ends_with("/ai/ai.dat")) {
        const auto route = read_ai_route(source);
        std::array<float, 2U> minimum{
            std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        std::array<float, 2U> maximum{
            std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};
        for (const auto& sample : route.samples) {
            for (std::size_t axis = 0U; axis < 2U; ++axis) {
                minimum[axis] = std::min(minimum[axis], sample.position[axis]);
                maximum[axis] = std::max(maximum[axis], sample.position[axis]);
            }
        }
        summary << "{\"samples\":" << route.samples.size()
                << ",\"groups\":" << route.groups.size()
                << ",\"metadata_count_mismatches\":"
                << route.metadata_count_mismatches
                << ",\"minimum_direction_length\":"
                << route.minimum_direction_length
                << ",\"maximum_direction_length\":"
                << route.maximum_direction_length << ",\"minimum_xz\":";
        write_array(summary, minimum);
        summary << ",\"maximum_xz\":";
        write_array(summary, maximum);
        summary << '}';
    } else {
        return std::nullopt;
    }
    } catch (const ToolError& error) {
        summary.str({});
        summary.clear();
        summary << "{\"parse_status\":\"error\",\"error_code\":"
                << static_cast<int>(error.code()) << '}';
    }
    std::ostringstream output;
    output << "{\"schema\":" << mh::common::json_string(parsed_object_schema)
           << ",\"importer\":" << mh::common::json_string(parsed_importer_id)
           << ",\"logical_id\":" << mh::common::json_string(file.logical_id)
           << ",\"source_sha256\":" << mh::common::json_string(file.sha256)
           << ",\"source_bytes\":" << file.bytes
           << ",\"format_id\":" << mh::common::json_string(format)
           << ",\"summary\":" << summary.str() << "}\n";
    return output.str();
}

std::string make_object_content(const InventoryFile& file) {
    std::ostringstream output;
    output << "{\"schema\":" << mh::common::json_string(object_schema)
           << ",\"importer\":" << mh::common::json_string(importer_id)
           << ",\"logical_id\":" << mh::common::json_string(file.logical_id)
           << ",\"source_path\":" << mh::common::json_string(file.relative_path)
           << ",\"source_sha256\":" << mh::common::json_string(file.sha256)
           << ",\"source_bytes\":" << file.bytes
           << ",\"format_id\":"
           << mh::common::json_string(file.identification.format_id)
           << ",\"category\":"
           << mh::common::json_string(file.identification.category)
           << ",\"status\":" << mh::common::json_string(file.identification.status)
           << ",\"header_hex\":" << mh::common::json_string(file.header_hex)
           << "}\n";
    return output.str();
}

CachePlan make_plan(const std::filesystem::path& content_root,
    const std::filesystem::path& cache_root) {
    const auto inventory = inventory_tree(content_root);
    CachePlan plan;
    plan.result.content_root = content_root;
    plan.result.cache_root = cache_root;
    plan.result.inventory_manifest_sha256 = inventory.manifest_sha256;
    plan.result.source_bytes = inventory.total_bytes;
    plan.result.entries.reserve(inventory.files.size());
    plan.object_contents.reserve(inventory.files.size());
    plan.parsed_object_contents.reserve(inventory.files.size());
    for (const auto& file : inventory.files) {
        auto content = make_object_content(file);
        const auto object_sha256 = mh::common::sha256(content);
        ImportCacheEntry entry;
        entry.logical_id = file.logical_id;
        entry.source_relative_path = file.relative_path;
        entry.source_sha256 = file.sha256;
        entry.source_bytes = file.bytes;
        entry.format_id = file.identification.format_id;
        entry.status = file.identification.status;
        entry.object_sha256 = object_sha256;
        entry.object_relative_path = object_path_for(object_sha256);
        entry.object_bytes = content.size();
        const auto parsed = make_parsed_object_content(file, content_root / file.relative_path);
        if (parsed.has_value()) {
            entry.parsed_importer = parsed_importer_id;
            entry.parsed_object_sha256 = mh::common::sha256(*parsed);
            entry.parsed_object_relative_path =
                parsed_object_path_for(entry.parsed_object_sha256);
            entry.parsed_object_bytes = parsed->size();
            ++plan.result.parsed_entries;
            if (parsed->find("\"parse_status\":\"error\"")
                != std::string::npos) {
                ++plan.result.parsed_error_entries;
            }
            plan.result.parsed_object_bytes += entry.parsed_object_bytes;
            plan.parsed_object_contents.push_back(*parsed);
        } else {
            plan.parsed_object_contents.emplace_back();
        }
        plan.result.object_bytes += entry.object_bytes;
        plan.result.entries.push_back(std::move(entry));
        plan.object_contents.push_back(std::move(content));
    }
    plan.index_content = make_index_content(plan.result.entries);
    plan.result.index_sha256 = mh::common::sha256(plan.index_content);
    plan.result.index_relative_path = index_path_for(plan.result.index_sha256);
    plan.result.index_bytes = plan.index_content.size();
    std::ostringstream manifest;
    manifest << "{\"schema\":\"motorhead.import-cache-manifest.v3\""
             << ",\"cache_layout\":\"sha256-v3\""
             << ",\"importer\":" << mh::common::json_string(importer_id)
             << ",\"inventory_manifest_sha256\":"
             << mh::common::json_string(inventory.manifest_sha256)
             << ",\"entry_count\":" << plan.result.entries.size()
             << ",\"source_bytes\":" << inventory.total_bytes
             << ",\"object_bytes\":" << plan.result.object_bytes
             << ",\"parsed_entries\":" << plan.result.parsed_entries
             << ",\"parsed_object_bytes\":" << plan.result.parsed_object_bytes
             << ",\"index_sha256\":"
             << mh::common::json_string(plan.result.index_sha256)
             << ",\"index_path\":"
             << mh::common::json_string(plan.result.index_relative_path)
             << ",\"index_bytes\":" << plan.result.index_bytes
             << ",\"entries\":[";
    for (std::size_t index = 0U; index < plan.result.entries.size(); ++index) {
        if (index != 0U) { manifest << ','; }
        const auto& entry = plan.result.entries[index];
        manifest << "{\"logical_id\":" << mh::common::json_string(entry.logical_id)
                 << ",\"source_path\":"
                 << mh::common::json_string(entry.source_relative_path)
                 << ",\"source_sha256\":"
                 << mh::common::json_string(entry.source_sha256)
                 << ",\"source_bytes\":" << entry.source_bytes
                 << ",\"format_id\":" << mh::common::json_string(entry.format_id)
                 << ",\"status\":" << mh::common::json_string(entry.status)
                 << ",\"object_sha256\":"
                 << mh::common::json_string(entry.object_sha256)
                 << ",\"object_path\":"
                 << mh::common::json_string(entry.object_relative_path)
                 << ",\"object_bytes\":" << entry.object_bytes;
        if (!entry.parsed_object_relative_path.empty()) {
            manifest << ",\"parsed_importer\":"
                     << mh::common::json_string(entry.parsed_importer)
                     << ",\"parsed_object_sha256\":"
                     << mh::common::json_string(entry.parsed_object_sha256)
                     << ",\"parsed_object_path\":"
                     << mh::common::json_string(entry.parsed_object_relative_path)
                     << ",\"parsed_object_bytes\":" << entry.parsed_object_bytes;
        }
        manifest << '}';
    }
    manifest << "]}\n";
    plan.manifest_content = manifest.str();
    plan.result.manifest_sha256 = mh::common::sha256(plan.manifest_content);
    plan.result.manifest_relative_path =
        manifest_path_for(plan.result.manifest_sha256);
    return plan;
}

bool write_immutable(const std::filesystem::path& destination,
    const std::string& content, const std::uint64_t maximum_bytes) {
    if (std::filesystem::exists(destination)) {
        if (read_bounded_text(destination, maximum_bytes) != content) {
            throw ToolError(ExitCode::verification,
                "content-addressed import-cache file is corrupt: "
                    + destination.string());
        }
        return false;
    }
    std::filesystem::create_directories(destination.parent_path());
    static std::atomic<std::uint64_t> sequence{0U};
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    auto temporary = destination;
    temporary += ".tmp-" + std::to_string(token) + '-'
        + std::to_string(sequence.fetch_add(1U));
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw ToolError(ExitCode::input,
                "cannot create import-cache temporary file");
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            std::filesystem::remove(temporary);
            throw ToolError(ExitCode::input,
                "cannot finish import-cache temporary file");
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        if (std::filesystem::exists(destination)
            && read_bounded_text(destination, maximum_bytes) == content) {
            std::filesystem::remove(temporary);
            return false;
        }
        std::filesystem::remove(temporary);
        throw ToolError(ExitCode::input,
            "cannot commit immutable import-cache file");
    }
    return true;
}

void verify_live_files(CachePlan& plan) {
    for (std::size_t index = 0U; index < plan.result.entries.size(); ++index) {
        const auto path = plan.result.cache_root
            / plan.result.entries[index].object_relative_path;
        if (read_bounded_text(path, maximum_descriptor_bytes)
            != plan.object_contents[index]) {
            throw ToolError(ExitCode::verification,
                "import-cache object content does not match its source");
        }
        const auto& entry = plan.result.entries[index];
        if (!entry.parsed_object_relative_path.empty()) {
            const auto parsed_path = plan.result.cache_root
                / entry.parsed_object_relative_path;
            if (read_bounded_text(parsed_path, maximum_descriptor_bytes)
                != plan.parsed_object_contents[index]) {
                throw ToolError(ExitCode::verification,
                    "import-cache parsed object content does not match its source");
            }
        }
    }
    const auto index_path = plan.result.cache_root
        / plan.result.index_relative_path;
    if (read_bounded_text(index_path, maximum_index_bytes)
        != plan.index_content) {
        throw ToolError(ExitCode::verification,
            "import-cache index content does not match its source tree");
    }
    const auto manifest_path = plan.result.cache_root
        / plan.result.manifest_relative_path;
    if (read_bounded_text(manifest_path, maximum_manifest_bytes)
        != plan.manifest_content) {
        throw ToolError(ExitCode::verification,
            "import-cache manifest content does not match its source tree");
    }
    plan.result.verified = true;
}

void audit_stale_files(ImportCacheResult& result) {
    std::set<std::string> live;
    live.insert(result.manifest_relative_path);
    live.insert(result.index_relative_path);
    for (const auto& entry : result.entries) {
        live.insert(entry.object_relative_path);
        if (!entry.parsed_object_relative_path.empty()) {
            live.insert(entry.parsed_object_relative_path);
        }
    }
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        result.cache_root, std::filesystem::directory_options::none, error), end;
    if (error) {
        throw ToolError(ExitCode::input, "cannot enumerate import-cache root");
    }
    while (iterator != end) {
        const auto status = iterator->symlink_status(error);
        if (error) {
            throw ToolError(ExitCode::input, "cannot inspect import-cache entry");
        }
        if (std::filesystem::is_symlink(status)) {
            throw ToolError(ExitCode::format, "import cache refuses symbolic links");
        }
        if (std::filesystem::is_regular_file(status)) {
            const auto relative = std::filesystem::relative(
                iterator->path(), result.cache_root).generic_string();
            const auto recognized = relative.starts_with("objects/sha256/")
                || relative.starts_with("parsed/sha256/")
                || relative.starts_with("indexes/sha256/")
                || relative.starts_with("manifests/sha256/");
            if (!recognized) {
                throw ToolError(ExitCode::format,
                    "unexpected file inside import-cache root: " + relative);
            }
            if (!live.contains(relative)) {
                ++result.stale_files;
                result.stale_bytes += std::filesystem::file_size(iterator->path());
            }
        }
        iterator.increment(error);
        if (error) {
            throw ToolError(ExitCode::input, "failed while enumerating import cache");
        }
    }
}

bool valid_sha256(const std::string_view value) {
    return value.size() == 64U
        && std::all_of(value.begin(), value.end(), [](const char character) {
            return (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f');
        });
}

std::string normalized_logical_id(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](const unsigned char character) {
            if (character == static_cast<unsigned char>('\\')) { return '/'; }
            if (character >= static_cast<unsigned char>('A')
                && character <= static_cast<unsigned char>('Z')) {
                return static_cast<char>(character
                    + static_cast<unsigned char>('a' - 'A'));
            }
            return static_cast<char>(character);
        });
    return value;
}

std::uint32_t read_index_u32(const std::string_view input, std::size_t& offset) {
    if (offset > input.size() || input.size() - offset < 4U) {
        throw ToolError(ExitCode::verification, "truncated import-cache index");
    }
    std::uint32_t value = 0U;
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(
            static_cast<unsigned char>(input[offset + index])) << (index * 8U);
    }
    offset += 4U;
    return value;
}

std::uint64_t read_index_u64(const std::string_view input, std::size_t& offset) {
    if (offset > input.size() || input.size() - offset < 8U) {
        throw ToolError(ExitCode::verification, "truncated import-cache index");
    }
    std::uint64_t value = 0U;
    for (std::uint32_t index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(
            static_cast<unsigned char>(input[offset + index])) << (index * 8U);
    }
    offset += 8U;
    return value;
}

std::string read_index_string(const std::string_view input, std::size_t& offset) {
    const auto bytes = read_index_u32(input, offset);
    if (bytes > maximum_descriptor_bytes || offset > input.size()
        || input.size() - offset < bytes) {
        throw ToolError(ExitCode::verification,
            "invalid string span in import-cache index");
    }
    std::string value(input.substr(offset, bytes));
    offset += bytes;
    return value;
}

std::optional<ImportCacheEntry> parse_index_entry(
    const std::string_view content, const std::string_view wanted) {
    if (content.size() < index_magic.size() + 4U
        || !std::equal(index_magic.begin(), index_magic.end(), content.begin())) {
        throw ToolError(ExitCode::verification,
            "invalid import-cache index signature");
    }
    std::size_t offset = index_magic.size();
    const auto count = read_index_u32(content, offset);
    std::optional<ImportCacheEntry> result;
    std::string previous;
    for (std::uint32_t index = 0U; index < count; ++index) {
        ImportCacheEntry entry;
        entry.logical_id = read_index_string(content, offset);
        entry.source_relative_path = read_index_string(content, offset);
        entry.source_sha256 = read_index_string(content, offset);
        entry.source_bytes = read_index_u64(content, offset);
        entry.format_id = read_index_string(content, offset);
        entry.status = read_index_string(content, offset);
        entry.object_sha256 = read_index_string(content, offset);
        entry.object_relative_path = read_index_string(content, offset);
        entry.object_bytes = read_index_u64(content, offset);
        entry.parsed_importer = read_index_string(content, offset);
        entry.parsed_object_sha256 = read_index_string(content, offset);
        entry.parsed_object_relative_path = read_index_string(content, offset);
        entry.parsed_object_bytes = read_index_u64(content, offset);
        if (entry.logical_id.empty()
            || normalized_logical_id(entry.logical_id) != entry.logical_id
            || (!previous.empty() && previous >= entry.logical_id)
            || !valid_sha256(entry.source_sha256)
            || !valid_sha256(entry.object_sha256)
            || entry.object_relative_path != object_path_for(entry.object_sha256)) {
            throw ToolError(ExitCode::verification,
                "invalid entry in import-cache index");
        }
        const auto has_parsed = !entry.parsed_object_sha256.empty();
        if (has_parsed != !entry.parsed_object_relative_path.empty()
            || has_parsed != !entry.parsed_importer.empty()
            || (has_parsed && (!valid_sha256(entry.parsed_object_sha256)
                || entry.parsed_object_relative_path
                    != parsed_object_path_for(entry.parsed_object_sha256)))) {
            throw ToolError(ExitCode::verification,
                "invalid parsed-object entry in import-cache index");
        }
        previous = entry.logical_id;
        if (entry.logical_id == wanted) { result = std::move(entry); }
    }
    if (offset != content.size()) {
        throw ToolError(ExitCode::verification,
            "trailing bytes in import-cache index");
    }
    return result;
}

std::string manifest_string_field(
    const std::string_view manifest, const std::string_view name) {
    const auto prefix = "\"" + std::string(name) + "\":\"";
    const auto start = manifest.find(prefix);
    if (start == std::string_view::npos) {
        throw ToolError(ExitCode::verification,
            "required field is absent from import-cache manifest");
    }
    const auto value_start = start + prefix.size();
    const auto end = manifest.find('"', value_start);
    if (end == std::string_view::npos) {
        throw ToolError(ExitCode::verification,
            "unterminated field in import-cache manifest");
    }
    return std::string(manifest.substr(value_start, end - value_start));
}

std::uint64_t manifest_u64_field(
    const std::string_view manifest, const std::string_view name) {
    const auto prefix = "\"" + std::string(name) + "\":";
    const auto start = manifest.find(prefix);
    if (start == std::string_view::npos) {
        throw ToolError(ExitCode::verification,
            "required numeric field is absent from import-cache manifest");
    }
    auto offset = start + prefix.size();
    if (offset >= manifest.size() || manifest[offset] < '0'
        || manifest[offset] > '9') {
        throw ToolError(ExitCode::verification,
            "invalid numeric field in import-cache manifest");
    }
    std::uint64_t value = 0U;
    while (offset < manifest.size() && manifest[offset] >= '0'
        && manifest[offset] <= '9') {
        const auto digit = static_cast<std::uint64_t>(manifest[offset] - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            throw ToolError(ExitCode::verification,
                "numeric field overflows in import-cache manifest");
        }
        value = value * 10U + digit;
        ++offset;
    }
    return value;
}

void verify_index_object(const std::filesystem::path& cache_root,
    const std::string& sha256, const std::string& relative_path,
    const std::uint64_t expected_bytes, const std::uint64_t maximum_bytes) {
    if (!valid_sha256(sha256)) {
        throw ToolError(ExitCode::verification,
            "invalid object hash in import-cache index");
    }
    const auto content = read_bounded_text(cache_root / relative_path, maximum_bytes);
    if (content.size() != expected_bytes || mh::common::sha256(content) != sha256) {
        throw ToolError(ExitCode::verification,
            "offline import-cache object verification failed");
    }
}

std::set<std::string> live_cache_paths(const ImportCacheResult& cache) {
    std::set<std::string> live{
        cache.manifest_relative_path, cache.index_relative_path};
    for (const auto& entry : cache.entries) {
        live.insert(entry.object_relative_path);
        if (!entry.parsed_object_relative_path.empty()) {
            live.insert(entry.parsed_object_relative_path);
        }
    }
    return live;
}

std::pair<std::string, std::uint64_t> authenticate_cache_file(
    const std::filesystem::path& cache_root, const std::string& relative) {
    std::string_view prefix;
    std::string_view extension;
    std::uint64_t maximum = maximum_descriptor_bytes;
    if (relative.starts_with("objects/sha256/")) {
        prefix = "objects/sha256/";
        extension = ".json";
    } else if (relative.starts_with("parsed/sha256/")) {
        prefix = "parsed/sha256/";
        extension = ".json";
    } else if (relative.starts_with("indexes/sha256/")) {
        prefix = "indexes/sha256/";
        extension = ".mhidx";
        maximum = maximum_index_bytes;
    } else if (relative.starts_with("manifests/sha256/")) {
        prefix = "manifests/sha256/";
        extension = ".json";
        maximum = maximum_manifest_bytes;
    } else {
        throw ToolError(ExitCode::format,
            "unexpected file inside import-cache root: " + relative);
    }
    const std::string_view suffix(relative.data() + prefix.size(),
        relative.size() - prefix.size());
    if (suffix.size() != 2U + 1U + 62U + extension.size()
        || suffix[2U] != '/' || !suffix.ends_with(extension)) {
        throw ToolError(ExitCode::format,
            "malformed content-addressed import-cache path: " + relative);
    }
    const auto hash = std::string(suffix.substr(0U, 2U))
        + std::string(suffix.substr(3U, 62U));
    if (!valid_sha256(hash)) {
        throw ToolError(ExitCode::format,
            "invalid content hash in import-cache path: " + relative);
    }
    const auto content = read_bounded_text(cache_root / relative, maximum);
    if (mh::common::sha256(content) != hash) {
        throw ToolError(ExitCode::verification,
            "stale import-cache candidate does not match its path hash: "
                + relative);
    }
    return {hash, content.size()};
}

std::vector<std::pair<std::string, std::uint64_t>> collect_prune_candidates(
    const ImportCacheResult& cache) {
    const auto live = live_cache_paths(cache);
    std::vector<std::pair<std::string, std::uint64_t>> candidates;
    std::error_code error;
    std::filesystem::recursive_directory_iterator iterator(
        cache.cache_root, std::filesystem::directory_options::none, error), end;
    if (error) {
        throw ToolError(ExitCode::input, "cannot enumerate import-cache root");
    }
    while (iterator != end) {
        const auto status = iterator->symlink_status(error);
        if (error) {
            throw ToolError(ExitCode::input, "cannot inspect import-cache entry");
        }
        if (std::filesystem::is_symlink(status)) {
            throw ToolError(ExitCode::format, "import cache refuses symbolic links");
        }
        if (std::filesystem::is_regular_file(status)) {
            const auto relative = std::filesystem::relative(
                iterator->path(), cache.cache_root).generic_string();
            if (!live.contains(relative)) {
                const auto authenticated = authenticate_cache_file(
                    cache.cache_root, relative);
                candidates.emplace_back(relative, authenticated.second);
            }
        }
        iterator.increment(error);
        if (error) {
            throw ToolError(ExitCode::input,
                "failed while enumerating import cache for pruning");
        }
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates;
}

} // namespace

ImportCacheResult build_import_cache(const std::filesystem::path& content_root,
    const std::filesystem::path& cache_root) {
    const auto content = normalized_absolute(content_root);
    const auto cache = normalized_absolute(cache_root);
    validate_roots(content, cache, false);
    auto plan = make_plan(content, cache);
    std::filesystem::create_directories(cache);
    for (std::size_t index = 0U; index < plan.result.entries.size(); ++index) {
        const auto written = write_immutable(cache
                / plan.result.entries[index].object_relative_path,
            plan.object_contents[index], maximum_descriptor_bytes);
        plan.result.written_objects += written ? 1U : 0U;
        plan.result.reused_objects += written ? 0U : 1U;
        const auto& entry = plan.result.entries[index];
        if (!entry.parsed_object_relative_path.empty()) {
            const auto parsed_written = write_immutable(cache
                    / entry.parsed_object_relative_path,
                plan.parsed_object_contents[index], maximum_descriptor_bytes);
            plan.result.written_parsed_objects += parsed_written ? 1U : 0U;
            plan.result.reused_parsed_objects += parsed_written ? 0U : 1U;
        }
    }
    const auto index_written = write_immutable(cache
            / plan.result.index_relative_path,
        plan.index_content, maximum_index_bytes);
    plan.result.written_indexes = index_written ? 1U : 0U;
    plan.result.reused_indexes = index_written ? 0U : 1U;
    static_cast<void>(write_immutable(cache / plan.result.manifest_relative_path,
        plan.manifest_content, maximum_manifest_bytes));
    verify_live_files(plan);
    audit_stale_files(plan.result);
    return plan.result;
}

ImportCacheResult verify_import_cache(const std::filesystem::path& content_root,
    const std::filesystem::path& cache_root) {
    const auto content = normalized_absolute(content_root);
    const auto cache = normalized_absolute(cache_root);
    validate_roots(content, cache, true);
    auto plan = make_plan(content, cache);
    verify_live_files(plan);
    plan.result.reused_objects = plan.result.entries.size();
    plan.result.reused_parsed_objects = plan.result.parsed_entries;
    plan.result.reused_indexes = 1U;
    audit_stale_files(plan.result);
    return plan.result;
}

std::optional<ImportCacheEntry> find_import_cache_entry(
    const ImportCacheResult& cache, std::string logical_id) {
    logical_id = normalized_logical_id(std::move(logical_id));
    const auto found = std::find_if(cache.entries.begin(), cache.entries.end(),
        [&logical_id](const ImportCacheEntry& entry) {
            return entry.logical_id == logical_id;
        });
    if (found == cache.entries.end() || found->logical_id != logical_id) {
        return std::nullopt;
    }
    return *found;
}

std::optional<OfflineImportCacheLookup> find_import_cache_entry_offline(
    const std::filesystem::path& cache_root, std::string manifest_sha256,
    std::string logical_id) {
    if (!valid_sha256(manifest_sha256)) {
        throw ToolError(ExitCode::usage,
            "offline import-cache lookup requires a lowercase SHA-256 manifest ID");
    }
    const auto cache = normalized_absolute(cache_root);
    std::error_code error;
    const auto status = std::filesystem::symlink_status(cache, error);
    if (error || std::filesystem::is_symlink(status)
        || !std::filesystem::is_directory(status)) {
        throw ToolError(ExitCode::input,
            "offline import-cache root is missing, not a directory, or symbolic");
    }
    const auto manifest = read_bounded_text(
        cache / manifest_path_for(manifest_sha256), maximum_manifest_bytes);
    if (mh::common::sha256(manifest) != manifest_sha256) {
        throw ToolError(ExitCode::verification,
            "offline import-cache manifest hash mismatch");
    }
    auto schema = manifest_string_field(manifest, "schema");
    if (schema != "motorhead.import-cache-manifest.v3") {
        throw ToolError(ExitCode::verification,
            "offline lookup requires an import-cache v3 manifest");
    }
    const auto index_sha256 = manifest_string_field(manifest, "index_sha256");
    const auto index_relative_path = manifest_string_field(manifest, "index_path");
    const auto index_bytes = manifest_u64_field(manifest, "index_bytes");
    if (!valid_sha256(index_sha256)
        || index_relative_path != index_path_for(index_sha256)) {
        throw ToolError(ExitCode::verification,
            "invalid index reference in import-cache manifest");
    }
    const auto index_content = read_bounded_text(
        cache / index_relative_path, maximum_index_bytes);
    if (index_content.size() != index_bytes
        || mh::common::sha256(index_content) != index_sha256) {
        throw ToolError(ExitCode::verification,
            "offline import-cache index hash mismatch");
    }
    logical_id = normalized_logical_id(std::move(logical_id));
    auto entry = parse_index_entry(index_content, logical_id);
    if (!entry.has_value()) { return std::nullopt; }
    verify_index_object(cache, entry->object_sha256,
        entry->object_relative_path, entry->object_bytes, maximum_descriptor_bytes);
    if (!entry->parsed_object_sha256.empty()) {
        verify_index_object(cache, entry->parsed_object_sha256,
            entry->parsed_object_relative_path, entry->parsed_object_bytes,
            maximum_descriptor_bytes);
    }
    return OfflineImportCacheLookup{
        std::move(schema), std::move(manifest_sha256), std::move(*entry)};
}

ImportCachePruneResult prune_import_cache(
    const std::filesystem::path& content_root,
    const std::filesystem::path& cache_root,
    std::optional<std::string> apply_manifest_sha256) {
    const auto verified_cache = verify_import_cache(content_root, cache_root);
    ImportCachePruneResult result;
    result.content_root = verified_cache.content_root;
    result.cache_root = verified_cache.cache_root;
    result.manifest_sha256 = verified_cache.manifest_sha256;
    result.verified = verified_cache.verified;
    if (apply_manifest_sha256.has_value()) {
        if (!valid_sha256(*apply_manifest_sha256)
            || *apply_manifest_sha256 != result.manifest_sha256) {
            throw ToolError(ExitCode::verification,
                "prune apply manifest does not match the freshly verified cache");
        }
    }
    const auto candidates = collect_prune_candidates(verified_cache);
    result.candidates.reserve(candidates.size());
    for (const auto& [relative, bytes] : candidates) {
        result.candidates.push_back(relative);
        result.candidate_bytes += bytes;
    }
    if (!apply_manifest_sha256.has_value()) { return result; }
    result.applied = true;
    for (const auto& [relative, bytes] : candidates) {
        const auto authenticated = authenticate_cache_file(
            result.cache_root, relative);
        if (authenticated.second != bytes) {
            throw ToolError(ExitCode::verification,
                "import-cache prune candidate changed after planning");
        }
        std::error_code error;
        if (!std::filesystem::remove(result.cache_root / relative, error) || error) {
            throw ToolError(ExitCode::input,
                "cannot remove authenticated stale import-cache file: " + relative);
        }
        ++result.removed_files;
        result.removed_bytes += bytes;
    }
    const auto after = verify_import_cache(content_root, cache_root);
    if (after.manifest_sha256 != result.manifest_sha256
        || after.stale_files != 0U) {
        throw ToolError(ExitCode::verification,
            "import-cache changed while guarded pruning was applied");
    }
    return result;
}

} // namespace mh::content
