#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mh::content {

struct ImportCacheEntry {
    std::string logical_id;
    std::string source_relative_path;
    std::string source_sha256;
    std::uint64_t source_bytes = 0U;
    std::string format_id;
    std::string status;
    std::string object_sha256;
    std::string object_relative_path;
    std::uint64_t object_bytes = 0U;
    std::string parsed_importer;
    std::string parsed_object_sha256;
    std::string parsed_object_relative_path;
    std::uint64_t parsed_object_bytes = 0U;
};

struct ImportCacheResult {
    std::filesystem::path content_root;
    std::filesystem::path cache_root;
    std::string schema = "motorhead.import-cache-manifest.v3";
    std::string inventory_manifest_sha256;
    std::string manifest_sha256;
    std::string manifest_relative_path;
    std::string index_sha256;
    std::string index_relative_path;
    std::uint64_t index_bytes = 0U;
    std::vector<ImportCacheEntry> entries;
    std::uint64_t source_bytes = 0U;
    std::uint64_t object_bytes = 0U;
    std::uint64_t parsed_entries = 0U;
    std::uint64_t parsed_error_entries = 0U;
    std::uint64_t parsed_object_bytes = 0U;
    std::uint64_t written_objects = 0U;
    std::uint64_t reused_objects = 0U;
    std::uint64_t written_parsed_objects = 0U;
    std::uint64_t reused_parsed_objects = 0U;
    std::uint64_t written_indexes = 0U;
    std::uint64_t reused_indexes = 0U;
    std::uint64_t stale_files = 0U;
    std::uint64_t stale_bytes = 0U;
    bool verified = false;
};

[[nodiscard]] ImportCacheResult build_import_cache(
    const std::filesystem::path& content_root,
    const std::filesystem::path& cache_root);

[[nodiscard]] ImportCacheResult verify_import_cache(
    const std::filesystem::path& content_root,
    const std::filesystem::path& cache_root);

[[nodiscard]] std::optional<ImportCacheEntry> find_import_cache_entry(
    const ImportCacheResult& cache, std::string logical_id);

struct OfflineImportCacheLookup {
    std::string cache_schema;
    std::string manifest_sha256;
    ImportCacheEntry entry;
};

[[nodiscard]] std::optional<OfflineImportCacheLookup>
find_import_cache_entry_offline(const std::filesystem::path& cache_root,
    std::string manifest_sha256, std::string logical_id);

struct ImportCachePruneResult {
    std::filesystem::path content_root;
    std::filesystem::path cache_root;
    std::string manifest_sha256;
    std::vector<std::string> candidates;
    std::uint64_t candidate_bytes = 0U;
    std::uint64_t removed_files = 0U;
    std::uint64_t removed_bytes = 0U;
    bool applied = false;
    bool verified = false;
};

[[nodiscard]] ImportCachePruneResult prune_import_cache(
    const std::filesystem::path& content_root,
    const std::filesystem::path& cache_root,
    std::optional<std::string> apply_manifest_sha256 = std::nullopt);

} // namespace mh::content
