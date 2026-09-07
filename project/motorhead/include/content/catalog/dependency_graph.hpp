#pragma once

#include <content/formats/texture_assets.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace mh::content {

struct TextureDependencyCandidate {
    std::string logical_id;
    std::string runtime_group_logical_id;
    std::string component_role;
    std::string content_sha256;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

struct MyoTextureDependency {
    std::string model_logical_id;
    std::string model_source_path;
    std::string model_sha256;
    std::string material_logical_id;
    std::string material_name;
    std::string scope;
    std::string resolution_model;
    std::uint64_t textured_face_count = 0U;
    std::vector<TextureDependencyCandidate> candidates;
};

struct AuthoredDependencyCandidate {
    std::string kind;
    std::string logical_id;
    std::string runtime_group_logical_id;
    std::string component_role;
    std::string sha256;
    std::uint64_t bytes = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

struct AuthoredContentDependency {
    std::string owner_kind;
    std::string owner_logical_id;
    std::string owner_source_path;
    std::string owner_sha256;
    std::string owner_name;
    std::string field;
    std::string reference;
    std::string resolution_model;
    std::vector<AuthoredDependencyCandidate> candidates;
};

struct ContentDependencyGraph {
    std::filesystem::path root;
    std::string texture_catalog_manifest_sha256;
    std::string manifest_sha256;
    std::uint64_t model_count = 0U;
    std::uint64_t textured_model_count = 0U;
    std::uint64_t dependency_count = 0U;
    std::uint64_t unique_material_count = 0U;
    std::uint64_t textured_face_count = 0U;
    std::uint64_t candidate_count = 0U;
    std::uint64_t scoped_variant_count = 0U;
    std::uint64_t global_variant_count = 0U;
    std::uint64_t global_fallback_count = 0U;
    std::uint64_t unique_resolution_count = 0U;
    std::uint64_t unresolved_count = 0U;
    std::uint64_t authored_source_count = 0U;
    std::uint64_t authored_dependency_count = 0U;
    std::uint64_t authored_candidate_count = 0U;
    std::uint64_t authored_unique_count = 0U;
    std::uint64_t authored_variant_count = 0U;
    std::uint64_t authored_bundle_count = 0U;
    std::uint64_t authored_inactive_selector_count = 0U;
    std::uint64_t authored_unresolved_count = 0U;
    std::vector<MyoTextureDependency> dependencies;
    std::vector<AuthoredContentDependency> authored_dependencies;
};

[[nodiscard]] ContentDependencyGraph build_content_dependency_graph(
    const std::filesystem::path& root);
[[nodiscard]] ContentDependencyGraph build_content_dependency_graph(
    const std::filesystem::path& root, const TextureCatalog& texture_catalog);

} // namespace mh::content
