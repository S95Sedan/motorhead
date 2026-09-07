#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace mh::content {

inline constexpr std::size_t fnt_glyph_count = 256U;
inline constexpr std::size_t fnt_header_bytes =
    sizeof(std::uint32_t) + fnt_glyph_count * 2U * sizeof(std::uint32_t);

enum class FntOperationKind : std::uint8_t {
  advance_row,
  advance_two_rows,
  store1,
  store2,
  store4,
  store8,
};

struct FntOperation {
  FntOperationKind kind = FntOperationKind::advance_row;
  std::int8_t displacement = 0;
};

struct FntGlyph {
  std::uint32_t program_offset = 0U;
  std::uint32_t advance_width = 0U;
  std::uint32_t program_bytes = 0U;
  std::uint32_t edi_advances = 0U;
  std::uint32_t esi_advances = 0U;
  std::uint32_t store1_count = 0U;
  std::uint32_t store2_count = 0U;
  std::uint32_t store4_count = 0U;
  std::uint32_t store8_count = 0U;
  std::vector<FntOperation> operations;
};

struct FntData {
  std::uint32_t line_metric = 0U;
  std::uint32_t file_bytes = 0U;
  std::uint32_t unique_program_count = 0U;
  std::array<FntGlyph, fnt_glyph_count> glyphs{};
};

[[nodiscard]] FntData parse_fnt(std::span<const std::uint8_t> bytes);
[[nodiscard]] FntData
read_fnt(const std::filesystem::path &path,
         std::uint64_t maximum_file_bytes = 16ULL * 1024ULL * 1024ULL);
[[nodiscard]] std::uint32_t measure_fnt_text(const FntData &font,
                                             std::string_view text);
void render_fnt_text_8bit(const FntData &font, std::string_view text,
                          std::span<std::uint8_t> target,
                          std::uint32_t target_width,
                          std::uint32_t target_height,
                          std::uint32_t target_pitch, std::uint32_t x,
                          std::uint32_t y, std::uint8_t color);

} // namespace mh::content
