#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace mh::game {

// The low byte written by the p3.1 identity owner at RVA
// 0x0008edc8..0x0008f0e2.  Values deliberately retain their original bits;
// several independent physics, camera, and renderer owners consume them.
enum class OriginalPcCheat : std::uint8_t {
  none = 0x00U,
  mega_springs = 0x01U,
  moon_gravity = 0x02U,
  la_suspension = 0x04U,
  thunder = 0x08U,
  underwater = 0x10U,
  ignition_camera = 0x20U,
  tron = 0x40U,
  supercars_camera = 0x80U,
};

struct OriginalPcCheatState {
  std::uint8_t mask = 0U;
  bool avenger_map = false;
  bool retro_identity = false;

  [[nodiscard]] bool active(OriginalPcCheat cheat) const noexcept;
};

// Reproduces p3.1's ASCII case-insensitive player/team comparisons.  The
// TRON identity intentionally ignores the team name, just as the original
// branch does. Avenger is consumed directly by the HUD owner rather than the
// shared low-byte mask.
[[nodiscard]] OriginalPcCheatState
original_pc_cheat_state(std::string_view player_name,
                        std::string_view team_name) noexcept;

enum class OriginalPcLocationEasterEgg {
  none,
  black_lotus_club,
  atlantika_scroll,
};

// p3.1 RVA 0x00022f3f..0x0002305c performs these exact position tests for
// the local vehicle. RuhrStadt ignores height; Atlantika uses all three axes.
[[nodiscard]] OriginalPcLocationEasterEgg original_pc_location_easter_egg(
    std::string_view track_name,
    const std::array<double, 3U> &world_position) noexcept;

// The original deliberately leaves this story in both main PC executables.
// Keeping it reachable from the linked cheat owner preserves the same
// hex-editor Easter egg in the reconstruction executable.
[[nodiscard]] std::string_view original_pc_hidden_executable_story() noexcept;

} // namespace mh::game
