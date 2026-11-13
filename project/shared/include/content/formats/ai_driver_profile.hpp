#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace mh::content {

struct AiProfileColor {
  std::uint8_t red = 0U;
  std::uint8_t green = 0U;
  std::uint8_t blue = 0U;
};

struct AiDriverProfile {
  std::string player_name;
  std::string player_nick;
  std::string player_picture;
  std::string player_born;
  AiProfileColor player_color;
  float aggressiveness = 0.0F;
  float eagerness = 0.0F;
  std::array<std::string, 4U> division_cars{};
  std::string horn;
  std::array<AiProfileColor, 3U> car_colors{};
  std::size_t recognized_fields = 0U;
  std::size_t unknown_fields = 0U;
};

[[nodiscard]] AiDriverProfile
parse_ai_driver_profile(std::span<const std::uint8_t> decoded_text);

// Retail ADP profiles use the direct-reader "EQ" TBL transform key, not their
// basename. The caller retains the source path as the portrait identity.
[[nodiscard]] AiDriverProfile
read_ai_driver_profile(const std::filesystem::path &path,
                       std::uint64_t maximum_file_bytes = 1024ULL * 1024ULL);

// Reproduces the p3.1 profile catalog's FindFirstFileA/FindNextFileA order for
// league/profiles/*.adp. The original loader does not sort the results.
[[nodiscard]] std::vector<AiDriverProfile>
read_ai_driver_profile_catalog(const std::filesystem::path &profile_root);

} // namespace mh::content
