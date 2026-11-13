#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace mh::content {

struct TextureAsset {
  std::string logical_id;
  std::string source_kind;
  std::string source_path;
  std::string entry_name;
  std::string source_sha256;
  std::string content_sha256;
  std::uint64_t source_bytes = 0U;
  std::uint64_t content_bytes = 0U;
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::uint64_t transparent_pixels = 0U;
};

struct TextureCatalog {
  std::filesystem::path root;
  std::vector<TextureAsset> assets;
  std::uint64_t loose_tga_count = 0U;
  std::uint64_t loose_iff_count = 0U;
  std::uint64_t pdi_archive_count = 0U;
  std::uint64_t pdi_texture_count = 0U;
  std::uint64_t pixel_count = 0U;
  std::uint64_t transparent_pixels = 0U;
  std::string manifest_sha256;
};

struct PamRgbaImage {
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::vector<std::uint8_t> palette_indices;
  std::vector<std::uint8_t> rgba;
};

struct TextureOverride {
  std::string logical_id;
  std::string relative_path;
  std::string sha256;
  std::uint64_t bytes = 0U;
  std::uint32_t original_width = 0U;
  std::uint32_t original_height = 0U;
  std::uint32_t override_width = 0U;
  std::uint32_t override_height = 0U;
  std::uint32_t integer_scale = 0U;
};

struct TextureOverrideSet {
  std::filesystem::path root;
  std::string catalog_manifest_sha256;
  std::uint64_t asset_count = 0U;
  std::vector<TextureOverride> overrides;
  std::string manifest_sha256;
};

[[nodiscard]] PamRgbaImage parse_pam_rgba(std::span<const std::uint8_t> bytes);
[[nodiscard]] PamRgbaImage
read_pam_rgba(const std::filesystem::path &path,
              std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);
[[nodiscard]] TextureCatalog
build_texture_catalog(const std::filesystem::path &root);
[[nodiscard]] PamRgbaImage
load_texture_asset_rgba(const TextureCatalog &catalog,
                        const TextureAsset &asset);
[[nodiscard]] TextureOverrideSet
resolve_texture_overrides(const TextureCatalog &catalog,
                          const std::filesystem::path &override_root);

} // namespace mh::content
