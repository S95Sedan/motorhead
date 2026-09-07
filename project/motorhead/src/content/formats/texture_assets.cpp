#include <content/formats/texture_assets.hpp>

#include <core/error.hpp>
#include <core/crypto/sha256.hpp>
#include <content/formats/iff_image.hpp>
#include <content/catalog/content_inventory.hpp>
#include <content/formats/pdi_archive.hpp>
#include <content/formats/tga_image.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <system_error>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::string ascii_lower(std::string value) {
  for (auto &character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
  }
  return value;
}

std::string sha256_bytes(const std::span<const std::uint8_t> bytes) {
  mh::common::Sha256 hash;
  hash.update(bytes.data(), bytes.size());
  return hash.finish_hex();
}

void hash_manifest_field(mh::common::Sha256 &hash,
                         const std::string_view value) {
  constexpr std::uint8_t separator = 0U;
  hash.update(value);
  hash.update(&separator, 1U);
}

std::uint64_t decimal(const std::string_view value,
                      const std::string_view field) {
  if (value.empty()) {
    throw ToolError(ExitCode::format, "PAM omits " + std::string(field));
  }
  std::uint64_t result = 0U;
  for (const char character : value) {
    if (character < '0' || character > '9') {
      throw ToolError(ExitCode::format,
                      "PAM has a non-decimal " + std::string(field));
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      throw ToolError(ExitCode::format, "PAM decimal field overflows");
    }
    result = result * 10U + digit;
  }
  return result;
}

std::vector<std::uint8_t>
read_plain_file(const std::filesystem::path &path,
                const std::uint64_t maximum_file_bytes,
                const std::string_view label) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    throw ToolError(ExitCode::input,
                    std::string(label) + " is not a plain file");
  }
  const auto size = std::filesystem::file_size(path);
  if (size > maximum_file_bytes ||
      size > std::numeric_limits<std::size_t>::max()) {
    throw ToolError(ExitCode::format,
                    std::string(label) + " exceeds its size bound");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw ToolError(ExitCode::input,
                    "short read while opening " + std::string(label));
  }
  return bytes;
}

} // namespace

PamRgbaImage parse_pam_rgba(const std::span<const std::uint8_t> bytes) {
  constexpr std::string_view terminator = "ENDHDR\n";
  const auto found = std::search(bytes.begin(), bytes.end(), terminator.begin(),
                                 terminator.end());
  if (found == bytes.end()) {
    throw ToolError(ExitCode::format, "PAM header has no ENDHDR terminator");
  }
  const auto header_bytes =
      static_cast<std::size_t>(found - bytes.begin()) + terminator.size();
  if (header_bytes > 1024U) {
    throw ToolError(ExitCode::format, "PAM header exceeds its bound");
  }
  const std::string header(reinterpret_cast<const char *>(bytes.data()),
                           header_bytes);
  std::map<std::string, std::string> fields;
  std::size_t cursor = 0U;
  std::size_t line_index = 0U;
  while (cursor < header.size()) {
    const auto end = header.find('\n', cursor);
    if (end == std::string::npos) {
      throw ToolError(ExitCode::format, "PAM header line is unterminated");
    }
    const auto line = header.substr(cursor, end - cursor);
    cursor = end + 1U;
    if (line_index++ == 0U) {
      if (line != "P7") {
        throw ToolError(ExitCode::format, "PAM magic is not P7");
      }
      continue;
    }
    if (line == "ENDHDR") {
      break;
    }
    const auto separator = line.find(' ');
    if (separator == std::string::npos || separator == 0U ||
        separator + 1U >= line.size() ||
        !fields.emplace(line.substr(0U, separator), line.substr(separator + 1U))
             .second) {
      throw ToolError(ExitCode::format,
                      "PAM header field is malformed or duplicated");
    }
  }
  const std::set<std::string> expected{"WIDTH", "HEIGHT", "DEPTH", "MAXVAL",
                                       "TUPLTYPE"};
  if (fields.size() != expected.size()) {
    throw ToolError(ExitCode::format, "PAM header field set is incomplete");
  }
  for (const auto &[name, unused] : fields) {
    static_cast<void>(unused);
    if (!expected.contains(name)) {
      throw ToolError(ExitCode::format,
                      "PAM header contains an unsupported field");
    }
  }
  if (fields["DEPTH"] != "4" || fields["MAXVAL"] != "255" ||
      fields["TUPLTYPE"] != "RGB_ALPHA") {
    throw ToolError(ExitCode::format, "PAM is not canonical 8-bit RGBA");
  }
  const auto width = decimal(fields["WIDTH"], "WIDTH");
  const auto height = decimal(fields["HEIGHT"], "HEIGHT");
  if (width == 0U || height == 0U || width > 16384U || height > 16384U) {
    throw ToolError(ExitCode::format,
                    "PAM dimensions exceed the supported bound");
  }
  const auto pixels = width * height;
  if (pixels > (512ULL * 1024ULL * 1024ULL) / 4U) {
    throw ToolError(ExitCode::format,
                    "PAM pixel payload exceeds the supported bound");
  }
  const auto pixel_bytes = pixels * 4U;
  if (header_bytes + pixel_bytes != bytes.size()) {
    throw ToolError(ExitCode::format,
                    "PAM pixel payload does not consume the file");
  }
  PamRgbaImage image;
  image.width = static_cast<std::uint32_t>(width);
  image.height = static_cast<std::uint32_t>(height);
  image.rgba.assign(bytes.begin() + static_cast<std::ptrdiff_t>(header_bytes),
                    bytes.end());
  return image;
}

PamRgbaImage read_pam_rgba(const std::filesystem::path &path,
                           const std::uint64_t maximum_file_bytes) {
  return parse_pam_rgba(
      read_plain_file(path, maximum_file_bytes, "texture override"));
}

PamRgbaImage upscale_rgba_nearest(const PamRgbaImage &image,
                                  const std::uint32_t integer_scale) {
  const auto expected =
      static_cast<std::uint64_t>(image.width) * image.height * 4U;
  if (image.width == 0U || image.height == 0U ||
      expected != image.rgba.size()) {
    throw ToolError(ExitCode::format,
                    "RGBA image has inconsistent decoded pixels");
  }
  if (integer_scale < 2U || integer_scale > 16U ||
      image.width > 16384U / integer_scale ||
      image.height > 16384U / integer_scale) {
    throw ToolError(ExitCode::usage,
                    "nearest upscale must be an integer from 2 through 16 "
                    "and remain within 16384x16384");
  }
  PamRgbaImage result;
  result.width = image.width * integer_scale;
  result.height = image.height * integer_scale;
  result.rgba.resize(static_cast<std::size_t>(result.width) * result.height *
                     4U);
  if (!image.palette_indices.empty()) {
    if (image.palette_indices.size() !=
        static_cast<std::size_t>(image.width) * image.height) {
      throw ToolError(ExitCode::format,
                      "indexed RGBA image has inconsistent palette indices");
    }
    result.palette_indices.resize(static_cast<std::size_t>(result.width) *
                                  result.height);
  }
  for (std::uint32_t y = 0U; y < result.height; ++y) {
    const auto source_y = y / integer_scale;
    for (std::uint32_t x = 0U; x < result.width; ++x) {
      const auto source_x = x / integer_scale;
      const auto source =
          (static_cast<std::size_t>(source_y) * image.width + source_x) * 4U;
      const auto destination =
          (static_cast<std::size_t>(y) * result.width + x) * 4U;
      std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(source), 4U,
                  result.rgba.begin() +
                      static_cast<std::ptrdiff_t>(destination));
      if (!result.palette_indices.empty()) {
        result.palette_indices[static_cast<std::size_t>(y) * result.width + x] =
            image.palette_indices[static_cast<std::size_t>(source_y) *
                                      image.width +
                                  source_x];
      }
    }
  }
  return result;
}

void write_pam_rgba(const PamRgbaImage &image,
                    const std::filesystem::path &output, const bool overwrite) {
  if (image.width > std::numeric_limits<std::uint16_t>::max() ||
      image.height > std::numeric_limits<std::uint16_t>::max()) {
    throw ToolError(ExitCode::format,
                    "RGBA image exceeds the PAM writer dimension bound");
  }
  IffIlbmImage compatible;
  compatible.width = static_cast<std::uint16_t>(image.width);
  compatible.height = static_cast<std::uint16_t>(image.height);
  compatible.rgba = image.rgba;
  write_iff_pam(compatible, output, overwrite);
}

TextureCatalog build_texture_catalog(const std::filesystem::path &root) {
  const auto inventory = inventory_tree(root);
  TextureCatalog catalog;
  catalog.root = inventory.root;
  std::set<std::string> logical_ids;
  for (const auto &file : inventory.files) {
    const auto source =
        catalog.root / std::filesystem::path(file.relative_path);
    if (file.identification.format_id == "tga") {
      const auto image = read_tga(source);
      TextureAsset asset;
      asset.logical_id = "texture/loose/" + file.logical_id;
      asset.source_kind = "loose-tga";
      asset.source_path = file.relative_path;
      asset.source_sha256 = file.sha256;
      asset.content_sha256 = file.sha256;
      asset.source_bytes = file.bytes;
      asset.content_bytes = file.bytes;
      asset.width = image.width;
      asset.height = image.height;
      for (std::size_t index = 3U; index < image.rgba.size(); index += 4U) {
        if (image.rgba[index] != 0xffU) {
          ++asset.transparent_pixels;
        }
      }
      catalog.assets.push_back(std::move(asset));
      ++catalog.loose_tga_count;
    } else if (file.identification.format_id == "iff-ilbm") {
      const auto image = read_iff_ilbm(source);
      TextureAsset asset;
      asset.logical_id = "texture/loose/" + file.logical_id;
      asset.source_kind = "loose-iff";
      asset.source_path = file.relative_path;
      asset.source_sha256 = file.sha256;
      asset.content_sha256 = file.sha256;
      asset.source_bytes = file.bytes;
      asset.content_bytes = file.bytes;
      asset.width = image.width;
      asset.height = image.height;
      asset.transparent_pixels = image.transparent_pixels;
      catalog.assets.push_back(std::move(asset));
      ++catalog.loose_iff_count;
    } else if (file.identification.format_id == "motorhead-pdi-v1") {
      const auto archive = read_pdi(source);
      for (const auto &entry : archive.entries) {
        TextureAsset asset;
        asset.logical_id =
            "texture/pdi/" + file.logical_id + "/" + ascii_lower(entry.name);
        asset.source_kind = "pdi-iff";
        asset.source_path = file.relative_path;
        asset.entry_name = entry.name;
        asset.source_sha256 = file.sha256;
        const auto start = static_cast<std::size_t>(archive.payload_offset) +
                           entry.relative_offset;
        const auto content = std::span<const std::uint8_t>(archive.source_bytes)
                                 .subspan(start, entry.byte_size);
        asset.content_sha256 = sha256_bytes(content);
        asset.source_bytes = file.bytes;
        asset.content_bytes = entry.byte_size;
        asset.width = entry.width;
        asset.height = entry.height;
        asset.transparent_pixels = entry.transparent_pixels;
        catalog.assets.push_back(std::move(asset));
        ++catalog.pdi_texture_count;
      }
      ++catalog.pdi_archive_count;
    }
  }
  std::sort(catalog.assets.begin(), catalog.assets.end(),
            [](const auto &left, const auto &right) {
              return left.logical_id < right.logical_id;
            });
  mh::common::Sha256 manifest;
  for (const auto &asset : catalog.assets) {
    if (!logical_ids.insert(asset.logical_id).second) {
      throw ToolError(ExitCode::format,
                      "texture logical ID collision: " + asset.logical_id);
    }
    catalog.pixel_count +=
        static_cast<std::uint64_t>(asset.width) * asset.height;
    catalog.transparent_pixels += asset.transparent_pixels;
    hash_manifest_field(manifest, asset.logical_id);
    hash_manifest_field(manifest, asset.source_kind);
    hash_manifest_field(manifest, asset.source_sha256);
    hash_manifest_field(manifest, asset.content_sha256);
    hash_manifest_field(manifest, std::to_string(asset.width));
    hash_manifest_field(manifest, std::to_string(asset.height));
  }
  catalog.manifest_sha256 = manifest.finish_hex();
  return catalog;
}

PamRgbaImage load_texture_asset_rgba(const TextureCatalog &catalog,
                                     const TextureAsset &asset) {
  const auto source = catalog.root / std::filesystem::path(asset.source_path);
  PamRgbaImage result;
  if (asset.source_kind == "loose-tga") {
    const auto image = read_tga(source);
    result.width = image.width;
    result.height = image.height;
    result.rgba = image.rgba;
  } else if (asset.source_kind == "loose-iff") {
    const auto image = read_iff_ilbm(source);
    result.width = image.width;
    result.height = image.height;
    result.palette_indices = image.palette_indices;
    result.rgba = image.rgba;
  } else if (asset.source_kind == "pdi-iff") {
    const auto archive = read_pdi(source);
    const auto entry = std::find_if(
        archive.entries.begin(), archive.entries.end(),
        [&asset](const auto &candidate) {
          return ascii_lower(candidate.name) == ascii_lower(asset.entry_name);
        });
    if (entry == archive.entries.end()) {
      throw ToolError(ExitCode::format,
                      "cataloged PDI texture entry is no longer present: " +
                          asset.logical_id);
    }
    const auto start = static_cast<std::size_t>(archive.payload_offset) +
                       entry->relative_offset;
    const auto content = std::span<const std::uint8_t>(archive.source_bytes)
                             .subspan(start, entry->byte_size);
    const auto image = parse_iff_ilbm(content);
    result.width = image.width;
    result.height = image.height;
    result.palette_indices = image.palette_indices;
    result.rgba = image.rgba;
  } else {
    throw ToolError(ExitCode::format,
                    "catalog asset has an unsupported texture source kind: " +
                        asset.source_kind);
  }
  if (result.width != asset.width || result.height != asset.height) {
    throw ToolError(
        ExitCode::format,
        "decoded texture dimensions changed since catalog construction");
  }
  return result;
}

std::uint64_t apply_palette_zero_transparency(PamRgbaImage &image) {
  const auto pixel_count =
      static_cast<std::uint64_t>(image.width) * image.height;
  if (image.rgba.empty() ||
      image.rgba.size() != static_cast<std::size_t>(pixel_count) * 4U ||
      image.palette_indices.size() != static_cast<std::size_t>(pixel_count)) {
    return 0U;
  }
  static_cast<void>(make_rgba_opaque(image));
  std::uint64_t changed = 0U;
  for (std::size_t pixel = 0U; pixel < image.palette_indices.size(); ++pixel) {
    if (image.palette_indices[pixel] != 0U) {
      continue;
    }
    const auto index = pixel * 4U;
    image.rgba[index] = 0U;
    image.rgba[index + 1U] = 0U;
    image.rgba[index + 2U] = 0U;
    image.rgba[index + 3U] = 0U;
    ++changed;
  }
  return changed;
}

std::uint64_t make_rgba_opaque(PamRgbaImage &image) {
  const auto pixel_count =
      static_cast<std::uint64_t>(image.width) * image.height;
  if (image.rgba.empty() ||
      image.rgba.size() != static_cast<std::size_t>(pixel_count) * 4U) {
    return 0U;
  }
  std::uint64_t changed = 0U;
  for (std::size_t index = 3U; index < image.rgba.size(); index += 4U) {
    if (image.rgba[index] != 255U) {
      image.rgba[index] = 255U;
      ++changed;
    }
  }
  return changed;
}

TextureOverrideSet
resolve_texture_overrides(const TextureCatalog &catalog,
                          const std::filesystem::path &override_root) {
  std::error_code error;
  const auto root =
      std::filesystem::absolute(override_root, error).lexically_normal();
  const auto root_status = std::filesystem::symlink_status(root, error);
  if (error || std::filesystem::is_symlink(root_status) ||
      !std::filesystem::is_directory(root_status)) {
    throw ToolError(ExitCode::input,
                    "texture override root is not a directory");
  }
  std::map<std::string, const TextureAsset *> assets;
  for (const auto &asset : catalog.assets) {
    assets.emplace(asset.logical_id, &asset);
  }
  std::vector<std::filesystem::path> paths;
  std::filesystem::recursive_directory_iterator iterator(
      root, std::filesystem::directory_options::none, error),
      end;
  if (error) {
    throw ToolError(ExitCode::input, "cannot enumerate texture overrides");
  }
  while (iterator != end) {
    const auto status = iterator->symlink_status(error);
    if (error || std::filesystem::is_symlink(status)) {
      throw ToolError(ExitCode::format,
                      "texture overrides refuse symbolic links");
    }
    if (std::filesystem::is_regular_file(status)) {
      paths.push_back(iterator->path());
    }
    iterator.increment(error);
    if (error) {
      throw ToolError(ExitCode::input, "failed to enumerate texture overrides");
    }
  }
  std::sort(paths.begin(), paths.end(),
            [&root](const auto &left, const auto &right) {
              return std::filesystem::relative(left, root).generic_string() <
                     std::filesystem::relative(right, root).generic_string();
            });
  TextureOverrideSet result;
  result.root = root;
  result.catalog_manifest_sha256 = catalog.manifest_sha256;
  result.asset_count = catalog.assets.size();
  std::set<std::string> override_ids;
  mh::common::Sha256 manifest;
  hash_manifest_field(manifest, catalog.manifest_sha256);
  for (const auto &path : paths) {
    const auto relative =
        std::filesystem::relative(path, root).generic_string();
    auto folded = ascii_lower(relative);
    if (!folded.ends_with(".pam")) {
      throw ToolError(ExitCode::format,
                      "texture override is not a .pam file: " + relative);
    }
    folded.resize(folded.size() - 4U);
    if (!override_ids.insert(folded).second) {
      throw ToolError(ExitCode::format,
                      "texture override logical ID collision: " + folded);
    }
    const auto found = assets.find(folded);
    if (found == assets.end()) {
      throw ToolError(ExitCode::format,
                      "texture override has no original logical ID: " + folded);
    }
    const auto image = read_pam_rgba(path);
    const auto &original = *found->second;
    if (image.width % original.width != 0U ||
        image.height % original.height != 0U ||
        image.width / original.width != image.height / original.height) {
      throw ToolError(
          ExitCode::format,
          "texture override dimensions are not an integer uniform scale: " +
              folded);
    }
    TextureOverride override;
    override.logical_id = folded;
    override.relative_path = relative;
    override.sha256 = mh::common::sha256_file(path);
    override.bytes = std::filesystem::file_size(path);
    override.original_width = original.width;
    override.original_height = original.height;
    override.override_width = image.width;
    override.override_height = image.height;
    override.integer_scale = image.width / original.width;
    hash_manifest_field(manifest, override.logical_id);
    hash_manifest_field(manifest, override.sha256);
    hash_manifest_field(manifest, std::to_string(override.override_width));
    hash_manifest_field(manifest, std::to_string(override.override_height));
    result.overrides.push_back(std::move(override));
  }
  result.manifest_sha256 = manifest.finish_hex();
  return result;
}

} // namespace mh::content
