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
  std::vector<FntOperation> operations;
};

struct FntData {
  std::uint32_t line_metric = 0U;
  std::array<FntGlyph, fnt_glyph_count> glyphs{};
};

[[nodiscard]] FntData read_fnt(const std::filesystem::path &path,
                               std::uint64_t maximum_file_bytes =
                                   16ULL * 1024ULL * 1024ULL);

} // namespace mh::content
