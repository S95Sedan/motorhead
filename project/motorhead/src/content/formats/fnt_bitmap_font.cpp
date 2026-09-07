#include <content/formats/fnt_bitmap_font.hpp>

#include <core/error.hpp>

#include <algorithm>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <string_view>
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
    if (bytes[cursor + index] != value) {
      return false;
    }
    ++index;
  }
  return true;
}

FntGlyph parse_glyph_program(const std::span<const std::uint8_t> bytes,
                             const std::uint32_t program_offset,
                             const std::uint32_t advance_width) {
  FntGlyph glyph;
  glyph.program_offset = program_offset;
  glyph.advance_width = advance_width;
  auto cursor = static_cast<std::size_t>(program_offset);
  constexpr std::size_t maximum_instructions = 65536U;
  for (std::size_t instruction = 0U; instruction < maximum_instructions;
       ++instruction) {
    if (cursor >= bytes.size()) {
      throw ToolError(ExitCode::format, "FNT glyph program is unterminated");
    }
    if (bytes_equal(bytes, cursor, {0x01U, 0xf8U})) {
      ++glyph.edi_advances;
      glyph.operations.push_back({FntOperationKind::advance_row, 0});
      cursor += 2U;
    } else if (bytes_equal(bytes, cursor, {0x01U, 0xf0U})) {
      ++glyph.esi_advances;
      glyph.operations.push_back({FntOperationKind::advance_two_rows, 0});
      cursor += 2U;
    } else if (bytes_equal(bytes, cursor, {0x88U, 0x58U})) {
      ++glyph.store1_count;
      glyph.operations.push_back(
          {FntOperationKind::store1,
           static_cast<std::int8_t>(bytes[cursor + 2U])});
      cursor += 3U;
    } else if (bytes_equal(bytes, cursor, {0x66U, 0x89U, 0x58U})) {
      ++glyph.store2_count;
      glyph.operations.push_back(
          {FntOperationKind::store2,
           static_cast<std::int8_t>(bytes[cursor + 3U])});
      cursor += 4U;
    } else if (bytes_equal(bytes, cursor, {0x89U, 0x58U})) {
      ++glyph.store4_count;
      glyph.operations.push_back(
          {FntOperationKind::store4,
           static_cast<std::int8_t>(bytes[cursor + 2U])});
      cursor += 3U;
    } else if (bytes_equal(bytes, cursor, {0xddU, 0x50U})) {
      ++glyph.store8_count;
      glyph.operations.push_back(
          {FntOperationKind::store8,
           static_cast<std::int8_t>(bytes[cursor + 2U])});
      cursor += 3U;
    } else if (bytes[cursor] == 0xe9U) {
      if (cursor > bytes.size() || bytes.size() - cursor < 5U) {
        throw ToolError(ExitCode::format, "FNT glyph tail jump is truncated");
      }
      cursor += 5U;
      glyph.program_bytes = static_cast<std::uint32_t>(cursor - program_offset);
      return glyph;
    } else {
      throw ToolError(ExitCode::format,
                      "FNT glyph uses an unsupported compiled instruction");
    }
  }
  throw ToolError(ExitCode::format, "FNT glyph exceeds the instruction bound");
}

} // namespace

FntData parse_fnt(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < fnt_header_bytes ||
      bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw ToolError(ExitCode::format, "FNT file is truncated or exceeds 4 GiB");
  }
  FntData data;
  data.line_metric = u32(bytes, 0U);
  data.file_bytes = static_cast<std::uint32_t>(bytes.size());
  if (data.line_metric == 0U || data.line_metric > 4096U) {
    throw ToolError(ExitCode::format,
                    "FNT line metric is outside observed bounds");
  }
  std::set<std::uint32_t> unique_programs;
  for (std::size_t index = 0U; index < data.glyphs.size(); ++index) {
    const auto record = 4U + index * 8U;
    const auto offset = u32(bytes, record);
    const auto width = u32(bytes, record + 4U);
    if (offset < fnt_header_bytes || offset >= bytes.size()) {
      throw ToolError(ExitCode::format,
                      "FNT glyph program offset is out of bounds");
    }
    if (width == 0U || width > 4096U) {
      throw ToolError(ExitCode::format, "FNT glyph advance width is invalid");
    }
    data.glyphs[index] = parse_glyph_program(bytes, offset, width);
    unique_programs.insert(offset);
  }
  data.unique_program_count =
      static_cast<std::uint32_t>(unique_programs.size());
  return data;
}

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

std::uint32_t measure_fnt_text(const FntData &font,
                               const std::string_view text) {
  std::uint64_t width = 0U;
  for (const auto character : text) {
    const auto codepoint =
        static_cast<std::uint8_t>(static_cast<unsigned char>(character));
    if (codepoint == 0U) {
      break;
    }
    width += font.glyphs[codepoint].advance_width;
    if (width > std::numeric_limits<std::uint32_t>::max()) {
      throw ToolError(ExitCode::format, "FNT text width overflows uint32");
    }
  }
  return static_cast<std::uint32_t>(width);
}

void render_fnt_text_8bit(const FntData &font, const std::string_view text,
                          const std::span<std::uint8_t> target,
                          const std::uint32_t target_width,
                          const std::uint32_t target_height,
                          const std::uint32_t target_pitch,
                          const std::uint32_t x, const std::uint32_t y,
                          const std::uint8_t color) {
  if (target_width == 0U || target_height == 0U ||
      target_pitch < target_width || x > target_width || y >= target_height) {
    throw ToolError(ExitCode::usage, "FNT render target is invalid");
  }
  const auto required_bytes =
      static_cast<std::uint64_t>(target_pitch) * target_height;
  if (required_bytes > target.size()) {
    throw ToolError(ExitCode::usage, "FNT render target is truncated");
  }

  std::uint64_t pen_x = x;
  for (const auto character : text) {
    const auto codepoint =
        static_cast<std::uint8_t>(static_cast<unsigned char>(character));
    if (codepoint == 0U) {
      break;
    }
    if (pen_x > target_width) {
      throw ToolError(ExitCode::format, "FNT pen escaped the render target");
    }
    const auto &glyph = font.glyphs[codepoint];
    auto row = static_cast<std::uint64_t>(y);
    for (const auto &operation : glyph.operations) {
      std::uint32_t store_width = 0U;
      switch (operation.kind) {
      case FntOperationKind::advance_row:
        ++row;
        continue;
      case FntOperationKind::advance_two_rows:
        row += 2U;
        continue;
      case FntOperationKind::store1:
        store_width = 1U;
        break;
      case FntOperationKind::store2:
        store_width = 2U;
        break;
      case FntOperationKind::store4:
        store_width = 4U;
        break;
      case FntOperationKind::store8:
        store_width = 8U;
        break;
      }
      const auto store_x =
          static_cast<std::int64_t>(pen_x) + operation.displacement;
      if (row >= target_height || store_x < 0 ||
          static_cast<std::uint64_t>(store_x) + store_width > target_width) {
        throw ToolError(ExitCode::format,
                        "FNT glyph store escapes the render target");
      }
      const auto offset =
          row * target_pitch + static_cast<std::uint64_t>(store_x);
      std::fill_n(target.begin() + static_cast<std::ptrdiff_t>(offset),
                  store_width, color);
    }
    pen_x += glyph.advance_width;
    if (pen_x > std::numeric_limits<std::uint32_t>::max()) {
      throw ToolError(ExitCode::format, "FNT pen overflows uint32");
    }
  }
}

} // namespace mh::content
