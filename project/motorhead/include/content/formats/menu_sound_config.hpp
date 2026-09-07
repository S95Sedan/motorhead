#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace mh::content {

inline constexpr std::uint32_t menu_sound_event_count = 36U;

struct MenuSoundBinding {
  std::string sample_name;
  std::vector<std::uint32_t> events;
};

struct MenuSoundConfig {
  std::vector<MenuSoundBinding> bindings;
};

[[nodiscard]] MenuSoundConfig
parse_menu_sound_config(std::span<const std::uint8_t> bytes);
[[nodiscard]] MenuSoundConfig
read_menu_sound_config(const std::filesystem::path &path,
                       std::uint64_t maximum_file_bytes = 64ULL * 1024ULL);
[[nodiscard]] const MenuSoundBinding *
resolve_menu_sound_event(const MenuSoundConfig &config,
                         std::uint32_t event) noexcept;

} // namespace mh::content
