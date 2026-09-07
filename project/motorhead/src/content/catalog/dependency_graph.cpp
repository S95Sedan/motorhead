#include <content/catalog/dependency_graph.hpp>

#include <core/error.hpp>
#include <core/crypto/sha256.hpp>
#include <content/formats/car_definition.hpp>
#include <content/catalog/content_inventory.hpp>
#include <content/formats/myo_object_model.hpp>
#include <content/formats/myw_world_model.hpp>
#include <content/formats/track_definition.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::string ascii_lower(std::string value) {
    for (auto& character : value) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return value;
}

std::string filename_from_logical_id(const std::string_view logical_id) {
    const auto separator = logical_id.find_last_of('/');
    return std::string(logical_id.substr(
        separator == std::string_view::npos ? 0U : separator + 1U));
}

std::string model_track_scope(const std::string_view model_logical_id) {
    constexpr std::string_view prefix = "tracks/";
    if (!model_logical_id.starts_with(prefix)) { return {}; }
    const auto end = model_logical_id.find('/', prefix.size());
    if (end == std::string_view::npos) { return {}; }
    const auto scope = model_logical_id.substr(prefix.size(), end - prefix.size());
    constexpr std::string_view track_prefix = "track";
    if (!scope.starts_with(track_prefix)
        || scope.size() == track_prefix.size()) { return {}; }
    std::uint32_t number = 0U;
    for (const auto character : scope.substr(track_prefix.size())) {
        if (character < '0' || character > '9') { return {}; }
        number = number * 10U + static_cast<std::uint32_t>(character - '0');
    }
    if (number < 1U || number > 16U
        || (scope.size() > track_prefix.size() + 1U
            && scope[track_prefix.size()] == '0')) { return {}; }
    return std::string(scope);
}

bool candidate_matches_scope(const TextureAsset& asset, const std::string_view scope) {
    return asset.source_kind == "pdi-iff"
        && ascii_lower(asset.source_path).starts_with("tracks/" + std::string(scope) + "/");
}

std::pair<std::string, std::string> runtime_texture_identity(
    const TextureAsset& asset) {
    if (asset.source_kind != "pdi-iff") {
        return {asset.logical_id, "standalone"};
    }
    auto group = asset.logical_id;
    constexpr std::string_view source_prefix = "texture/pdi/";
    constexpr std::string_view group_prefix = "texture/runtime/";
    if (!group.starts_with(source_prefix)) {
        throw ToolError(ExitCode::format,
            "PDI texture has an invalid logical ID prefix");
    }
    group.replace(0U, source_prefix.size(), group_prefix);
    const auto tex1 = group.find("/tex1.pdi/");
    const auto tex2 = group.find("/tex2.pdi/");
    if ((tex1 == std::string::npos) == (tex2 == std::string::npos)) {
        throw ToolError(ExitCode::format,
            "PDI texture is outside a tex1/tex2 runtime pair");
    }
    const auto marker = tex1 != std::string::npos ? tex1 : tex2;
    group.erase(marker, std::string_view("/tex1.pdi").size());
    return {std::move(group), tex1 != std::string::npos
        ? "tex1-palette-mask-component" : "tex2-eight-plane-color-component"};
}

void hash_field(mh::common::Sha256& hash, const std::string_view value) {
    constexpr std::uint8_t separator = 0U;
    hash.update(value);
    hash.update(&separator, 1U);
}

std::optional<std::string> normalized_reference_path(std::string value) {
    value = ascii_lower(std::move(value));
    std::replace(value.begin(), value.end(), '\\', '/');
    while (value.starts_with("./")) { value.erase(0U, 2U); }
    while (!value.empty() && value.back() == '/') { value.pop_back(); }
    if (value.empty() || value.front() == '/' || value.find(':') != std::string::npos
        || value.find("//") != std::string::npos) {
        return std::nullopt;
    }
    std::istringstream segments(value);
    std::string segment;
    while (std::getline(segments, segment, '/')) {
        if (segment.empty() || segment == "." || segment == "..") {
            return std::nullopt;
        }
    }
    return value;
}

std::optional<std::string> join_reference(
    const std::string& base_path, const std::string& reference) {
    auto normalized = normalized_reference_path(reference);
    if (!normalized.has_value()) { return std::nullopt; }
    if (normalized->starts_with("tracks/") || normalized->starts_with("game/")
        || normalized->starts_with("league/") || normalized->starts_with("demos/")) {
        return normalized;
    }
    const auto base = normalized_reference_path(base_path);
    if (!base.has_value()) { return std::nullopt; }
    return *base + '/' + *normalized;
}

AuthoredDependencyCandidate file_candidate(const InventoryFile& file) {
    return AuthoredDependencyCandidate{
        "file", file.logical_id, "", "", file.sha256, file.bytes, 0U, 0U};
}

AuthoredDependencyCandidate texture_candidate(const TextureAsset& asset) {
    const auto [group, role] = runtime_texture_identity(asset);
    return AuthoredDependencyCandidate{"texture", asset.logical_id, group, role,
        asset.content_sha256, asset.content_bytes, asset.width, asset.height};
}

std::map<std::string, std::string> read_selected_config(
    const std::filesystem::path& path) {
    std::error_code error;
    const auto bytes = std::filesystem::file_size(path, error);
    if (error || bytes == 0U || bytes > 1024ULL * 1024ULL) {
        throw ToolError(ExitCode::format,
            "configuration size is outside dependency-parser bounds");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw ToolError(ExitCode::input, "cannot open dependency configuration");
    }
    std::string text(static_cast<std::size_t>(bytes), '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (input.gcount() != static_cast<std::streamsize>(text.size())
        || text.find('\0') != std::string::npos) {
        throw ToolError(ExitCode::format,
            "configuration is truncated or contains a NUL byte");
    }
    const std::set<std::string> selected{
        "carname", "trackname", "leaguename", "demofilename", "controlname"};
    std::map<std::string, std::string> values;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        const auto comment = line.find("//");
        if (comment != std::string::npos) { line.erase(comment); }
        std::istringstream fields(line);
        std::string key;
        if (!(fields >> key)) { continue; }
        key = ascii_lower(std::move(key));
        if (!selected.contains(key)) { continue; }
        std::string value;
        std::getline(fields, value);
        const auto begin = value.find_first_not_of(" \t\r");
        const auto end = value.find_last_not_of(" \t\r");
        if (begin == std::string::npos || values.contains(key)) {
            throw ToolError(ExitCode::format,
                "selected configuration dependency is empty or duplicated");
        }
        values.emplace(std::move(key), value.substr(begin, end - begin + 1U));
    }
    return values;
}

} // namespace

ContentDependencyGraph build_content_dependency_graph(const std::filesystem::path& root) {
    const auto catalog = build_texture_catalog(root);
    return build_content_dependency_graph(root, catalog);
}

ContentDependencyGraph build_content_dependency_graph(
    const std::filesystem::path& root, const TextureCatalog& texture_catalog) {
    const auto inventory = inventory_tree(root);
    if (inventory.root != texture_catalog.root) {
        throw ToolError(ExitCode::input,
            "dependency graph root does not match the texture catalog root");
    }

    std::map<std::string, std::vector<const TextureAsset*>> textures_by_name;
    for (const auto& asset : texture_catalog.assets) {
        const auto name = ascii_lower(asset.entry_name.empty()
            ? filename_from_logical_id(asset.logical_id) : asset.entry_name);
        textures_by_name[name].push_back(&asset);
    }
    for (auto& [unused, candidates] : textures_by_name) {
        static_cast<void>(unused);
        std::sort(candidates.begin(), candidates.end(), [](const auto* left, const auto* right) {
            return left->logical_id < right->logical_id;
        });
    }
    std::map<std::string, const InventoryFile*> files_by_logical_id;
    for (const auto& file : inventory.files) {
        files_by_logical_id.emplace(file.logical_id, &file);
    }

    ContentDependencyGraph graph;
    graph.root = inventory.root;
    graph.texture_catalog_manifest_sha256 = texture_catalog.manifest_sha256;
    std::set<std::string> materials;
    for (const auto& file : inventory.files) {
        if (file.identification.format_id != "motorhead-myo-v1"
            && file.identification.format_id != "motorhead-myo-v2") {
            continue;
        }
        ++graph.model_count;
        const auto model = read_myo(graph.root / std::filesystem::path(file.relative_path));
        std::vector<std::uint64_t> face_counts(model.names.size(), 0U);
        for (const auto& face : model.faces) {
            if (!face.has_texture_coordinates || face.material_name_index >= face_counts.size()) {
                continue;
            }
            ++face_counts[face.material_name_index];
        }
        bool textured = false;
        const auto scope = model_track_scope(file.logical_id);
        for (std::size_t name_index = 0U; name_index < model.names.size(); ++name_index) {
            if (face_counts[name_index] == 0U) { continue; }
            textured = true;
            MyoTextureDependency dependency;
            dependency.model_logical_id = "model/myo/" + file.logical_id;
            dependency.model_source_path = file.relative_path;
            dependency.model_sha256 = file.sha256;
            dependency.material_name = ascii_lower(model.names[name_index]);
            dependency.material_logical_id = "material/myo/" + dependency.material_name;
            dependency.scope = scope;
            dependency.textured_face_count = face_counts[name_index];
            materials.insert(dependency.material_logical_id);

            std::vector<const TextureAsset*> selected;
            const auto found = textures_by_name.find(dependency.material_name);
            if (found != textures_by_name.end()) {
                if (!scope.empty()) {
                    std::copy_if(found->second.begin(), found->second.end(),
                        std::back_inserter(selected), [&scope](const auto* asset) {
                            return candidate_matches_scope(*asset, scope);
                        });
                }
                if (selected.empty()) { selected = found->second; }
            }
            if (selected.empty()) {
                dependency.resolution_model = "unresolved";
                ++graph.unresolved_count;
            } else if (selected.size() == 1U) {
                dependency.resolution_model = "unique";
                ++graph.unique_resolution_count;
            } else if (!scope.empty()
                && candidate_matches_scope(*selected.front(), scope)) {
                dependency.resolution_model = "scoped-variant-set";
                ++graph.scoped_variant_count;
            } else if (!scope.empty()) {
                dependency.resolution_model = "global-fallback-set";
                ++graph.global_fallback_count;
            } else {
                dependency.resolution_model = "global-variant-set";
                ++graph.global_variant_count;
            }
            for (const auto* asset : selected) {
                const auto [group, role] = runtime_texture_identity(*asset);
                dependency.candidates.push_back(TextureDependencyCandidate{
                    asset->logical_id, group, role, asset->content_sha256,
                    asset->width, asset->height});
            }
            graph.textured_face_count += dependency.textured_face_count;
            graph.candidate_count += dependency.candidates.size();
            graph.dependencies.push_back(std::move(dependency));
        }
        if (textured) { ++graph.textured_model_count; }
    }
    std::sort(graph.dependencies.begin(), graph.dependencies.end(), [](const auto& left,
                                                                       const auto& right) {
        if (left.model_logical_id != right.model_logical_id) {
            return left.model_logical_id < right.model_logical_id;
        }
        return left.material_logical_id < right.material_logical_id;
    });
    graph.dependency_count = graph.dependencies.size();
    graph.unique_material_count = materials.size();

    const auto add_authored_dependency = [&graph](AuthoredContentDependency dependency,
                                             const bool bundle = false) {
        std::sort(dependency.candidates.begin(), dependency.candidates.end(),
            [](const auto& left, const auto& right) {
                if (left.kind != right.kind) { return left.kind < right.kind; }
                return left.logical_id < right.logical_id;
            });
        dependency.candidates.erase(std::unique(dependency.candidates.begin(),
            dependency.candidates.end(), [](const auto& left, const auto& right) {
                return left.kind == right.kind && left.logical_id == right.logical_id;
            }), dependency.candidates.end());
        if (dependency.candidates.empty()
            && dependency.resolution_model == "inactive-selector") {
            ++graph.authored_inactive_selector_count;
        } else if (dependency.candidates.empty()) {
            dependency.resolution_model = "unresolved";
            ++graph.authored_unresolved_count;
        } else if (bundle) {
            if (dependency.resolution_model.empty()) {
                dependency.resolution_model = "bundle";
            }
            ++graph.authored_bundle_count;
        } else if (dependency.candidates.size() == 1U) {
            if (dependency.resolution_model.empty()) {
                dependency.resolution_model = "unique";
            }
            ++graph.authored_unique_count;
        } else {
            if (dependency.resolution_model.empty()) {
                dependency.resolution_model = "variant-set";
            }
            ++graph.authored_variant_count;
        }
        graph.authored_candidate_count += dependency.candidates.size();
        graph.authored_dependencies.push_back(std::move(dependency));
    };

    std::map<std::string, std::vector<const InventoryFile*>> cars_by_name;
    for (const auto& file : inventory.files) {
        if (file.logical_id.starts_with("game/")
            && file.logical_id.ends_with(".car")) {
            const auto car = read_car(inventory.root / file.relative_path);
            cars_by_name[ascii_lower(car.name)].push_back(&file);
        }
    }

    std::map<std::string, std::vector<const InventoryFile*>> tracks_by_name;
    for (const auto& file : inventory.files) {
        if (!file.logical_id.starts_with("game/track")
            || !file.logical_id.ends_with(".trk")) {
            continue;
        }
        const auto track = read_track_definition(inventory.root / file.relative_path);
        tracks_by_name[ascii_lower(track.name)].push_back(&file);
        ++graph.authored_source_count;
        const auto base = normalized_reference_path(track.base_path);
        if (!base.has_value() || !base->starts_with("tracks/")) {
            throw ToolError(ExitCode::format,
                "track definition has an invalid dependency base path");
        }
        const auto slash = base->find('/', 7U);
        const auto scope = slash == std::string::npos
            ? base->substr(7U) : base->substr(7U, slash - 7U);
        std::set<std::string> world_materials;
        if (std::any_of(track.references.begin(), track.references.end(),
                [](const auto& reference) {
                    return reference.field == "zerotransparent";
                })) {
            const auto world_reference =
                join_reference(track.base_path, track.world_filename);
            if (world_reference.has_value()) {
                if (const auto found = files_by_logical_id.find(*world_reference);
                    found != files_by_logical_id.end()) {
                    const auto world = read_myw(inventory.root / found->second->relative_path);
                    for (const auto& name : world.material_names) {
                        world_materials.insert(ascii_lower(name));
                    }
                }
            }
        }
        for (const auto& reference : track.references) {
            AuthoredContentDependency dependency;
            dependency.owner_kind = "track-definition";
            dependency.owner_logical_id = "track/definition/" + file.logical_id;
            dependency.owner_source_path = file.relative_path;
            dependency.owner_sha256 = file.sha256;
            dependency.owner_name = track.name;
            dependency.field = reference.field;
            dependency.reference = reference.value;
            auto bundle = false;
            if (reference.field == "filename") {
                dependency.resolution_model = "runtime-selected-variant";
                const auto direct = join_reference(track.base_path, reference.value);
                if (direct.has_value()) {
                    if (const auto found = files_by_logical_id.find(*direct);
                        found != files_by_logical_id.end()) {
                        dependency.candidates.push_back(file_candidate(*found->second));
                    }
                    const auto separator = direct->find_last_of('/');
                    const auto hardware = direct->substr(0U, separator) + "/hardware/"
                        + direct->substr(separator + 1U);
                    if (const auto found = files_by_logical_id.find(hardware);
                        found != files_by_logical_id.end()) {
                        dependency.candidates.push_back(file_candidate(*found->second));
                    }
                }
            } else if (reference.field == "texturepath") {
                bundle = true;
                dependency.resolution_model = "runtime-required-bundle";
                const auto directory = normalized_reference_path(reference.value);
                if (directory.has_value()) {
                    const auto prefix = *directory + "/packed/";
                    for (const auto& candidate : inventory.files) {
                        if (candidate.logical_id.starts_with(prefix)
                            && candidate.identification.format_id
                                == "motorhead-pdi-v1") {
                            dependency.candidates.push_back(file_candidate(candidate));
                        }
                    }
                }
            } else if (reference.field == "envmapname") {
                const auto normalized = normalized_reference_path(reference.value);
                const auto name = normalized.has_value()
                    ? filename_from_logical_id(*normalized) : std::string{};
                if (const auto found = textures_by_name.find(name);
                    found != textures_by_name.end()) {
                    std::vector<const TextureAsset*> selected;
                    for (const auto* candidate : found->second) {
                        if (candidate_matches_scope(*candidate, scope)) {
                            selected.push_back(candidate);
                        }
                    }
                    const auto scoped = !selected.empty();
                    if (!scoped) { selected = found->second; }
                    dependency.resolution_model = scoped
                        ? (selected.size() == 1U ? "scoped-unique"
                                                : "scoped-variant-set")
                        : (selected.size() == 1U ? "global-unique"
                                                : "global-fallback-set");
                    for (const auto* candidate : selected) {
                        dependency.candidates.push_back(texture_candidate(*candidate));
                    }
                }
            } else if (reference.field == "zerotransparent") {
                const auto normalized = normalized_reference_path(reference.value);
                const auto name = normalized.has_value()
                    ? filename_from_logical_id(*normalized) : std::string{};
                if (!world_materials.contains(name)) {
                    dependency.resolution_model = "inactive-selector";
                } else if (const auto found = textures_by_name.find(name);
                    found != textures_by_name.end()) {
                    std::vector<const TextureAsset*> selected;
                    for (const auto* candidate : found->second) {
                        if (candidate_matches_scope(*candidate, scope)) {
                            selected.push_back(candidate);
                        }
                    }
                    dependency.resolution_model = selected.size() == 1U
                        ? "scoped-unique" : "scoped-variant-set";
                    for (const auto* candidate : selected) {
                        dependency.candidates.push_back(texture_candidate(*candidate));
                    }
                }
            } else if (reference.field == "trackmapname") {
                bundle = true;
                dependency.resolution_model = "authored-bundle";
                const auto prefix = join_reference(track.base_path, reference.value);
                if (prefix.has_value()) {
                    for (const auto& candidate : inventory.files) {
                        if (candidate.logical_id == *prefix + ".iff"
                            || (candidate.logical_id.starts_with(*prefix)
                                && candidate.logical_id.ends_with(".dta"))) {
                            dependency.candidates.push_back(file_candidate(candidate));
                        }
                    }
                }
            } else {
                auto target = join_reference(track.base_path, reference.value);
                if (reference.field == "aitrackname" && target.has_value()) {
                    *target += "/ai/ai.dat";
                }
                if (target.has_value()) {
                    if (const auto found = files_by_logical_id.find(*target);
                        found != files_by_logical_id.end()) {
                        dependency.candidates.push_back(file_candidate(*found->second));
                    }
                }
            }
            add_authored_dependency(std::move(dependency), bundle);
        }
    }

    const auto config_found = files_by_logical_id.find("game/motorhead.cfg");
    if (config_found != files_by_logical_id.end()) {
        const auto& config_file = *config_found->second;
        const auto values = read_selected_config(
            inventory.root / config_file.relative_path);
        ++graph.authored_source_count;
        for (const auto& [field, reference] : values) {
            AuthoredContentDependency dependency;
            dependency.owner_kind = "configuration";
            dependency.owner_logical_id = "config/" + config_file.logical_id;
            dependency.owner_source_path = config_file.relative_path;
            dependency.owner_sha256 = config_file.sha256;
            dependency.owner_name = "MotorHead";
            dependency.field = field;
            dependency.reference = reference;
            if (field == "carname") {
                if (const auto found = cars_by_name.find(ascii_lower(reference));
                    found != cars_by_name.end()) {
                    for (const auto* candidate : found->second) {
                        dependency.candidates.push_back(file_candidate(*candidate));
                    }
                }
            } else if (field == "trackname") {
                if (const auto found = tracks_by_name.find(ascii_lower(reference));
                    found != tracks_by_name.end()) {
                    for (const auto* candidate : found->second) {
                        dependency.candidates.push_back(file_candidate(*candidate));
                    }
                }
            } else {
                const auto prefix = field == "leaguename" ? "league/"
                    : field == "demofilename" ? "demos/" : "game/";
                const auto extension = field == "leaguename" ? ".lgf"
                    : field == "demofilename" ? ".mde" : ".clo";
                const auto logical = prefix + ascii_lower(reference) + extension;
                if (const auto found = files_by_logical_id.find(logical);
                    found != files_by_logical_id.end()) {
                    dependency.candidates.push_back(file_candidate(*found->second));
                }
            }
            add_authored_dependency(std::move(dependency));
        }
    }
    std::sort(graph.authored_dependencies.begin(), graph.authored_dependencies.end(),
        [](const auto& left, const auto& right) {
            if (left.owner_logical_id != right.owner_logical_id) {
                return left.owner_logical_id < right.owner_logical_id;
            }
            if (left.field != right.field) { return left.field < right.field; }
            return left.reference < right.reference;
        });
    graph.authored_dependency_count = graph.authored_dependencies.size();

    mh::common::Sha256 manifest;
    hash_field(manifest, graph.texture_catalog_manifest_sha256);
    for (const auto& dependency : graph.dependencies) {
        hash_field(manifest, dependency.model_logical_id);
        hash_field(manifest, dependency.model_sha256);
        hash_field(manifest, dependency.material_logical_id);
        hash_field(manifest, dependency.scope);
        hash_field(manifest, dependency.resolution_model);
        hash_field(manifest, std::to_string(dependency.textured_face_count));
        for (const auto& candidate : dependency.candidates) {
            hash_field(manifest, candidate.logical_id);
            hash_field(manifest, candidate.runtime_group_logical_id);
            hash_field(manifest, candidate.component_role);
            hash_field(manifest, candidate.content_sha256);
        }
    }
    for (const auto& dependency : graph.authored_dependencies) {
        hash_field(manifest, dependency.owner_kind);
        hash_field(manifest, dependency.owner_logical_id);
        hash_field(manifest, dependency.owner_sha256);
        hash_field(manifest, dependency.owner_name);
        hash_field(manifest, dependency.field);
        hash_field(manifest, dependency.reference);
        hash_field(manifest, dependency.resolution_model);
        for (const auto& candidate : dependency.candidates) {
            hash_field(manifest, candidate.kind);
            hash_field(manifest, candidate.logical_id);
            hash_field(manifest, candidate.runtime_group_logical_id);
            hash_field(manifest, candidate.component_role);
            hash_field(manifest, candidate.sha256);
            hash_field(manifest, std::to_string(candidate.bytes));
        }
    }
    graph.manifest_sha256 = manifest.finish_hex();
    return graph;
}

} // namespace mh::content
