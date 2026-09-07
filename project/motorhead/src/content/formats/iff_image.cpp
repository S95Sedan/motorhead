#include <content/formats/iff_image.hpp>
#include <content/export/image_export.hpp>

#include <core/error.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::uint16_t be16(const std::span<const std::uint8_t> bytes,
                   const std::size_t offset) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[offset]) << 8U) | bytes[offset + 1U]);
}

std::uint32_t be32(const std::span<const std::uint8_t> bytes,
                   const std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U) |
         bytes[offset + 3U];
}

bool fourcc(const std::span<const std::uint8_t> bytes, const std::size_t offset,
            const std::string_view value) {
  return value.size() == 4U && offset <= bytes.size() &&
         4U <= bytes.size() - offset &&
         std::equal(value.begin(), value.end(),
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

void decode_byterun1_row(const std::span<const std::uint8_t> source,
                         std::size_t &cursor, const std::size_t expected_bytes,
                         std::vector<std::uint8_t> &decoded) {
  const auto output_begin = decoded.size();
  while (cursor < source.size() &&
         decoded.size() - output_begin < expected_bytes) {
    const auto control = static_cast<std::int8_t>(source[cursor++]);
    if (control >= 0) {
      const auto count = static_cast<std::size_t>(control) + 1U;
      if (count > source.size() - cursor ||
          count > expected_bytes - (decoded.size() - output_begin)) {
        throw ToolError(ExitCode::format,
                        "IFF ByteRun1 literal exceeds its bounds");
      }
      decoded.insert(
          decoded.end(), source.begin() + static_cast<std::ptrdiff_t>(cursor),
          source.begin() + static_cast<std::ptrdiff_t>(cursor + count));
      cursor += count;
    } else if (control != std::numeric_limits<std::int8_t>::min()) {
      if (cursor >= source.size()) {
        throw ToolError(ExitCode::format,
                        "IFF ByteRun1 repeat omits its value");
      }
      const auto count =
          static_cast<std::size_t>(1 - static_cast<int>(control));
      if (count > expected_bytes - (decoded.size() - output_begin)) {
        throw ToolError(ExitCode::format,
                        "IFF ByteRun1 repeat exceeds its output");
      }
      decoded.insert(decoded.end(), count, source[cursor++]);
    }
  }
  if (decoded.size() - output_begin != expected_bytes) {
    throw ToolError(ExitCode::format,
                    "IFF ByteRun1 row does not decode to its exact size");
  }
}

void write_bytes_guarded(const std::filesystem::path &output,
                         const std::span<const std::uint8_t> bytes,
                         const bool overwrite) {
  if (output.empty()) {
    throw ToolError(ExitCode::usage, "IFF PAM output path is empty");
  }
  const auto destination = std::filesystem::absolute(output).lexically_normal();
  const auto parent = destination.parent_path();
  std::error_code error;
  auto current = parent.root_path();
  for (const auto &component : parent.relative_path()) {
    current /= component;
    auto status = std::filesystem::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      if (!std::filesystem::create_directory(current, error) || error) {
        throw ToolError(ExitCode::input,
                        "cannot create IFF PAM output directory");
      }
      status = std::filesystem::symlink_status(current, error);
    }
    if (error || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_directory(status)) {
      throw ToolError(ExitCode::input,
                      "IFF PAM output parent is not a plain directory");
    }
  }
  error.clear();
  const auto status = std::filesystem::symlink_status(destination, error);
  const bool exists = !error && std::filesystem::exists(status);
  if (exists && (std::filesystem::is_symlink(status) ||
                 !std::filesystem::is_regular_file(status))) {
    throw ToolError(ExitCode::input, "IFF PAM output is not a plain file");
  }
  if (exists && !overwrite) {
    throw ToolError(ExitCode::input, "IFF PAM output exists (use --overwrite)");
  }
  auto temporary = destination;
  temporary += ".mhtool-part";
  auto backup = destination;
  backup += ".mhtool-backup";
  if (std::filesystem::exists(temporary) ||
      (exists && std::filesystem::exists(backup))) {
    throw ToolError(ExitCode::input,
                    "IFF PAM temporary or backup output exists");
  }
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
      std::filesystem::remove(temporary, error);
      throw ToolError(ExitCode::input, "cannot write IFF PAM output");
    }
  }
  bool backed_up = false;
  if (exists) {
    std::filesystem::rename(destination, backup, error);
    if (error) {
      std::filesystem::remove(temporary, error);
      throw ToolError(ExitCode::input, "cannot preserve IFF PAM output");
    }
    backed_up = true;
  }
  std::filesystem::rename(temporary, destination, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    if (backed_up) {
      std::error_code ignored;
      std::filesystem::rename(backup, destination, ignored);
    }
    throw ToolError(ExitCode::input, "cannot finalize IFF PAM output");
  }
  if (backed_up) {
    std::filesystem::remove(backup, error);
    if (error) {
      throw ToolError(ExitCode::input, "cannot remove IFF PAM backup");
    }
  }
}

} // namespace

IffIlbmImage parse_iff_ilbm(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 12U || !fourcc(bytes, 0U, "FORM") ||
      !fourcc(bytes, 8U, "ILBM")) {
    throw ToolError(ExitCode::format, "IFF input is not a FORM/ILBM image");
  }
  const auto form_size = static_cast<std::uint64_t>(be32(bytes, 4U));
  const auto form_end = 8ULL + form_size;
  const auto padded_end = form_end + (form_size & 1U);
  if (form_size < 4U || padded_end != bytes.size()) {
    throw ToolError(ExitCode::format,
                    "IFF FORM size does not consume the file");
  }
  if (padded_end != form_end &&
      bytes[static_cast<std::size_t>(form_end)] != 0U) {
    throw ToolError(ExitCode::format, "IFF FORM pad byte is nonzero");
  }

  IffIlbmImage image;
  image.file_bytes = bytes.size();
  std::span<const std::uint8_t> bitmap_header;
  std::span<const std::uint8_t> palette;
  std::span<const std::uint8_t> body;
  const std::set<std::string> accepted_chunks{"ANNO", "BMHD", "GRAB", "CAMG",
                                              "CMAP", "BODY", "CRNG", "DRNG",
                                              "DPI ", "TINY"};
  std::set<std::string> seen_chunks;
  std::size_t cursor = 12U;
  while (cursor < form_end) {
    if (8U > static_cast<std::size_t>(form_end) - cursor) {
      throw ToolError(ExitCode::format, "IFF chunk header is truncated");
    }
    const std::string identifier(
        reinterpret_cast<const char *>(bytes.data() + cursor), 4U);
    const auto chunk_size =
        static_cast<std::uint64_t>(be32(bytes, cursor + 4U));
    const auto data_offset = static_cast<std::uint64_t>(cursor) + 8U;
    const auto chunk_end = data_offset + chunk_size;
    const auto next = chunk_end + (chunk_size & 1U);
    const bool final_pad_outside_form =
        chunk_end == form_end && next == padded_end;
    if (!accepted_chunks.contains(identifier) || chunk_end > form_end ||
        (next > form_end && !final_pad_outside_form)) {
      throw ToolError(ExitCode::format,
                      "IFF contains an unknown or out-of-bounds chunk");
    }
    const bool repeatable_metadata =
        identifier == "CRNG" || identifier == "DRNG";
    if (!seen_chunks.insert(identifier).second && !repeatable_metadata) {
      throw ToolError(ExitCode::format, "IFF contains a duplicate chunk");
    }
    if (next != chunk_end && bytes[static_cast<std::size_t>(chunk_end)] != 0U) {
      throw ToolError(ExitCode::format, "IFF chunk pad byte is nonzero");
    }
    const auto data = bytes.subspan(static_cast<std::size_t>(data_offset),
                                    static_cast<std::size_t>(chunk_size));
    if (identifier == "BMHD") {
      bitmap_header = data;
    }
    if (identifier == "CMAP") {
      palette = data;
    }
    if (identifier == "BODY") {
      body = data;
    }
    cursor = static_cast<std::size_t>(std::min(next, form_end));
    ++image.chunk_count;
  }
  if (cursor != form_end || bitmap_header.size() != 20U || palette.empty() ||
      body.empty()) {
    throw ToolError(ExitCode::format,
                    "IFF omits a required BMHD, CMAP, or BODY chunk");
  }
  if (palette.size() % 3U != 0U || palette.size() > 256U * 3U) {
    throw ToolError(ExitCode::format, "IFF CMAP size is invalid");
  }

  image.width = be16(bitmap_header, 0U);
  image.height = be16(bitmap_header, 2U);
  image.x = static_cast<std::int16_t>(be16(bitmap_header, 4U));
  image.y = static_cast<std::int16_t>(be16(bitmap_header, 6U));
  image.plane_count = bitmap_header[8U];
  image.masking = bitmap_header[9U];
  image.compression = bitmap_header[10U];
  // The nominal BMHD pad byte is 0x80 in 97 shipped files. It is retained as
  // an observed producer quirk and has no role in planar decoding.
  image.transparent_color = be16(bitmap_header, 12U);
  image.x_aspect = bitmap_header[14U];
  image.y_aspect = bitmap_header[15U];
  image.page_width = static_cast<std::int16_t>(be16(bitmap_header, 16U));
  image.page_height = static_cast<std::int16_t>(be16(bitmap_header, 18U));
  image.palette_entries = static_cast<std::uint16_t>(palette.size() / 3U);
  if (image.width == 0U || image.height == 0U || image.plane_count == 0U ||
      image.plane_count > 8U ||
      (image.masking != 0U && image.masking != 2U && image.masking != 3U) ||
      image.compression > 1U || image.palette_entries == 0U ||
      image.palette_entries > (1U << image.plane_count)) {
    throw ToolError(ExitCode::format,
                    "IFF BMHD fields are outside the observed subset");
  }
  const auto pixels = static_cast<std::uint64_t>(image.width) * image.height;
  if (pixels > (512ULL * 1024ULL * 1024ULL) / 4U) {
    throw ToolError(ExitCode::format,
                    "IFF decoded pixels exceed the size bound");
  }
  const auto row_bytes =
      static_cast<std::uint64_t>(((image.width + 15U) / 16U) * 2U);
  const auto planar_bytes = row_bytes * image.height * image.plane_count;
  if (planar_bytes > std::numeric_limits<std::size_t>::max()) {
    throw ToolError(ExitCode::format,
                    "IFF decoded planar data exceeds the platform bound");
  }
  image.body_bytes = body.size();
  image.decoded_planar_bytes = planar_bytes;
  std::vector<std::uint8_t> planar;
  if (image.compression == 0U) {
    if (body.size() != planar_bytes) {
      throw ToolError(ExitCode::format,
                      "IFF uncompressed BODY size is inconsistent");
    }
    planar.assign(body.begin(), body.end());
  } else {
    planar.reserve(static_cast<std::size_t>(planar_bytes));
    std::size_t body_cursor = 0U;
    const auto scanline_count =
        static_cast<std::uint64_t>(image.height) * image.plane_count;
    for (std::uint64_t scanline = 0U; scanline < scanline_count; ++scanline) {
      decode_byterun1_row(body, body_cursor,
                          static_cast<std::size_t>(row_bytes), planar);
    }
    while (body_cursor < body.size() &&
           body[body_cursor] == static_cast<std::uint8_t>(0x80U)) {
      ++body_cursor;
    }
    // Thirty-eight shipped files append one zero byte to the compressed
    // BODY payload. Accept only that exact, bounded producer quirk.
    if (body.size() - body_cursor == 1U && body[body_cursor] == 0U) {
      ++body_cursor;
    }
    if (body_cursor != body.size()) {
      throw ToolError(ExitCode::format,
                      "IFF ByteRun1 stream has non-NOP trailing bytes");
    }
  }

  image.palette_indices.resize(static_cast<std::size_t>(pixels));
  image.rgba.resize(static_cast<std::size_t>(pixels) * 4U);
  for (std::uint32_t y = 0U; y < image.height; ++y) {
    for (std::uint32_t x = 0U; x < image.width; ++x) {
      std::uint16_t color = 0U;
      for (std::uint32_t plane = 0U; plane < image.plane_count; ++plane) {
        const auto source =
            (static_cast<std::uint64_t>(y) * image.plane_count + plane) *
                row_bytes +
            x / 8U;
        const auto bit = static_cast<std::uint8_t>(0x80U >> (x & 7U));
        if ((planar[static_cast<std::size_t>(source)] & bit) != 0U) {
          color = static_cast<std::uint16_t>(color | (1U << plane));
        }
      }
      if (color >= image.palette_entries) {
        throw ToolError(ExitCode::format,
                        "IFF pixel palette index is out of bounds");
      }
      const auto pixel = static_cast<std::size_t>(y) * image.width + x;
      image.palette_indices[pixel] = static_cast<std::uint8_t>(color);
      const auto destination = pixel * 4U;
      const auto palette_offset = static_cast<std::size_t>(color) * 3U;
      image.rgba[destination] = palette[palette_offset];
      image.rgba[destination + 1U] = palette[palette_offset + 1U];
      image.rgba[destination + 2U] = palette[palette_offset + 2U];
      const bool transparent =
          image.masking == 2U && color == image.transparent_color;
      image.rgba[destination + 3U] = transparent ? 0U : 0xffU;
      if (transparent) {
        ++image.transparent_pixels;
      }
    }
  }
  return image;
}

IffIlbmImage read_iff_ilbm(const std::filesystem::path &path,
                           const std::uint64_t maximum_file_bytes) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    throw ToolError(ExitCode::input, "IFF input is not a plain file");
  }
  const auto size = std::filesystem::file_size(path);
  if (size > maximum_file_bytes ||
      size > std::numeric_limits<std::size_t>::max()) {
    throw ToolError(ExitCode::format,
                    "IFF file exceeds the configured size bound");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  std::ifstream stream(path, std::ios::binary);
  stream.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  if (!stream ||
      stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw ToolError(ExitCode::input, "short read while opening IFF input");
  }
  return parse_iff_ilbm(bytes);
}

void write_iff_pam(const IffIlbmImage &image,
                   const std::filesystem::path &output, const bool overwrite) {
  const auto expected =
      static_cast<std::uint64_t>(image.width) * image.height * 4U;
  if (image.width == 0U || image.height == 0U ||
      expected != image.rgba.size()) {
    throw ToolError(ExitCode::format,
                    "IFF image has inconsistent decoded pixels");
  }
  std::ostringstream header;
  header << "P7\nWIDTH " << image.width << "\nHEIGHT " << image.height
         << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
  const auto prefix = header.str();
  std::vector<std::uint8_t> pam(prefix.begin(), prefix.end());
  pam.insert(pam.end(), image.rgba.begin(), image.rgba.end());
  write_bytes_guarded(output, pam, overwrite);
}

void write_iff_png(const IffIlbmImage &image,
                   const std::filesystem::path &output, const bool overwrite) {
  write_rgba_png(image.width, image.height, image.rgba, output, overwrite);
}

} // namespace mh::content
