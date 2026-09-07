#include <formats/bitmap_font.hpp>

#include <core/error.hpp>

#include <fstream>
#include <initializer_list>
#include <limits>
#include <span>
#include <vector>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::uint32_t u32(const std::span<const std::uint8_t> bytes,
                  const std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < 4U) {
    throw ToolError(ExitCode::format, "FNT 32-bit field is truncated");
  }
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

bool bytes_equal(const std::span<const std::uint8_t> bytes,
                 const std::size_t cursor,
                 const std::initializer_list<std::uint8_t> expected) {
  if (cursor > bytes.size() || bytes.size() - cursor < expected.size()) {
    return false;
  }
  std::size_t index = 0U;
  for (const auto value : expected) {
    if (bytes[cursor + index++] != value) {
      return false;
    }
  }
  return true;
}

FntGlyph parse_glyph(const std::span<const std::uint8_t> bytes,
                     const std::uint32_t offset,
                     const std::uint32_t advance_width) {
  FntGlyph glyph;
  glyph.program_offset = offset;
  glyph.advance_width = advance_width;
  auto cursor = static_cast<std::size_t>(offset);
  for (std::size_t instruction = 0U; instruction < 65536U; ++instruction) {
    if (cursor >= bytes.size()) {
      throw ToolError(ExitCode::format, "FNT glyph program is unterminated");
    }
    if (bytes_equal(bytes, cursor, {0x01U, 0xf8U})) {
      glyph.operations.push_back({FntOperationKind::advance_row, 0});
      cursor += 2U;
    } else if (bytes_equal(bytes, cursor, {0x01U, 0xf0U})) {
      glyph.operations.push_back({FntOperationKind::advance_two_rows, 0});
      cursor += 2U;
    } else if (bytes_equal(bytes, cursor, {0x88U, 0x58U})) {
      glyph.operations.push_back(
          {FntOperationKind::store1,
           static_cast<std::int8_t>(bytes[cursor + 2U])});
      cursor += 3U;
    } else if (bytes_equal(bytes, cursor, {0x66U, 0x89U, 0x58U})) {
      glyph.operations.push_back(
          {FntOperationKind::store2,
           static_cast<std::int8_t>(bytes[cursor + 3U])});
      cursor += 4U;
    } else if (bytes_equal(bytes, cursor, {0x89U, 0x58U})) {
      glyph.operations.push_back(
          {FntOperationKind::store4,
           static_cast<std::int8_t>(bytes[cursor + 2U])});
      cursor += 3U;
    } else if (bytes_equal(bytes, cursor, {0xddU, 0x50U})) {
      glyph.operations.push_back(
          {FntOperationKind::store8,
           static_cast<std::int8_t>(bytes[cursor + 2U])});
      cursor += 3U;
    } else if (bytes[cursor] == 0xe9U) {
      if (bytes.size() - cursor < 5U) {
        throw ToolError(ExitCode::format, "FNT glyph tail jump is truncated");
      }
      glyph.program_bytes = static_cast<std::uint32_t>(cursor + 5U - offset);
      return glyph;
    } else {
      throw ToolError(ExitCode::format,
                      "FNT glyph uses an unsupported compiled instruction");
    }
  }
  throw ToolError(ExitCode::format, "FNT glyph exceeds the instruction bound");
}

FntData parse_fnt(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < fnt_header_bytes ||
      bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw ToolError(ExitCode::format, "FNT file is truncated or exceeds 4 GiB");
  }
  FntData font;
  font.line_metric = u32(bytes, 0U);
  if (font.line_metric == 0U || font.line_metric > 4096U) {
    throw ToolError(ExitCode::format,
                    "FNT line metric is outside observed bounds");
  }
  for (std::size_t index = 0U; index < font.glyphs.size(); ++index) {
    const auto record = 4U + index * 8U;
    const auto offset = u32(bytes, record);
    const auto width = u32(bytes, record + 4U);
    if (offset < fnt_header_bytes || offset >= bytes.size() || width == 0U ||
        width > 4096U) {
      throw ToolError(ExitCode::format, "FNT glyph record is invalid");
    }
    font.glyphs[index] = parse_glyph(bytes, offset, width);
  }
  return font;
}

} // namespace

FntData read_fnt(const std::filesystem::path &path,
                 const std::uint64_t maximum_file_bytes) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    throw ToolError(ExitCode::input, "FNT input is not a plain file");
  }
  const auto size = std::filesystem::file_size(path);
  if (size > maximum_file_bytes ||
      size > std::numeric_limits<std::size_t>::max()) {
    throw ToolError(ExitCode::format,
                    "FNT file exceeds the configured size bound");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  std::ifstream stream(path, std::ios::binary);
  stream.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  if (!stream ||
      stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw ToolError(ExitCode::input, "short read while opening FNT input");
  }
  return parse_fnt(bytes);
}

} // namespace mh::content
