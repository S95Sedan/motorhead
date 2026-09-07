#include <game/gameplay/cheats.hpp>

#include <cstddef>

namespace mh::game {
namespace {

char ascii_lower(const char value) noexcept {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A'))
                                      : value;
}

bool ascii_equal_case_insensitive(const std::string_view left,
                                  const std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.size(); ++index) {
    if (ascii_lower(left[index]) != ascii_lower(right[index])) {
      return false;
    }
  }
  return true;
}

bool identity(const std::string_view player_name,
              const std::string_view team_name,
              const std::string_view expected_player,
              const std::string_view expected_team) noexcept {
  return ascii_equal_case_insensitive(player_name, expected_player) &&
         ascii_equal_case_insensitive(team_name, expected_team);
}

constexpr std::string_view hidden_executable_story =
    "Welcome to this kewl part. This is nicho and Chevron hitting the keys "
    "on this A500. The time is almost 5 in the morning and its only 12 min. "
    "to deadline. We have been drinking a huge amount of coke. we have been "
    "awake for 2 days and we have only been eatin pizza and potato crisps. "
    "estrella 4ever. This party has been a really kewl one with a lot of "
    "elite guys like fuzzac, alta, sincoz, tip & mantronix, firefox, mr gurk, "
    "terminator, 4042, mogwie, karl IIX, dezed, mr defect, kyd & balle, "
    "celebrandil, bit'n'bytes, uno, slayer and more. wow. We have been here "
    "in Eskilstuna in almost 3 days and have not been out for a second, exept "
    "to buy some chips. And these damn germans with big stereo's. The trip "
    "to this dump where quite awful, we had to travel by train because the "
    "driver lost his license on the way from his place to mine. major "
    "fuckup. so he had to carry all the monitors himself. hehe. What happen "
    "the first day on the party? Ofcourse someone stole macke's 40Mb-harddrive "
    "on which we kept all the gfx to the demo, he has been running around "
    "digging peoples bags and sleepingbags. he have not found it yet and "
    "don't think he will, but he has stolen alot of candy while searching. "
    "The toilets and the showers have been closed since 1:00 pm this morning "
    "so outside it looks like there have been a flooding. before i sat down "
    "here writing i planed a little nap, but NO...... something did not smell "
    "like it's suppose to. It was my sleepingbag, someone had built a landmark "
    "in it, a MAJOR dump, and what a stench. What also stank was the music "
    "compo, a f..king lamer cheated so he won with a real crappy tune made of "
    "only st-00 sounds. But as usual, slash was there and copied it before it "
    "was released, so we had already heard the shit. Ohhh damn i forgot... "
    "the deadline is in 2 min. Ok, time for some greetings: Alcatraz Anarchy "
    "Andromeda Angels Black Robes Budbrain Byterapers Carilian Carnage Classic "
    "Complex Crusaders Cryptoburners Crystal Def Jam Dexion Dual Crew Elite "
    "Faglight Flash prod. J-Beam Kefrens Madness Mythos Ninja Cracking Crew "
    "North star Noxious Onsala Sector Paradox Phenomena Pure metal coders "
    "Quartex Razor 1911 Rebels Red sector S.O.S Sanity Sciense 451 Scoopex "
    "Shining Silents Spaceballs SPECTRE Spreadpoint Stellar The Link Tommy "
    "knockers Top swap Tri Star Triad Triangle UnitA Vaxine Vision Vortex 42 "
    "And all the groups that we've forgot. Text restarts! 9 - 8 - 7 - 6 - "
    "etc etc";

} // namespace

bool OriginalPcCheatState::active(const OriginalPcCheat cheat) const noexcept {
  return (mask & static_cast<std::uint8_t>(cheat)) != 0U;
}

OriginalPcCheatState
original_pc_cheat_state(const std::string_view player_name,
                        const std::string_view team_name) noexcept {
  OriginalPcCheatState result;
  if (identity(player_name, team_name, "demon", "grem")) {
    result.mask = static_cast<std::uint8_t>(OriginalPcCheat::mega_springs);
  } else if (identity(player_name, team_name, "buzz aldrin", "nasa")) {
    result.mask = static_cast<std::uint8_t>(OriginalPcCheat::moon_gravity);
  } else if (identity(player_name, team_name, "g-ride", "west")) {
    result.mask = static_cast<std::uint8_t>(OriginalPcCheat::la_suspension);
  } else if (identity(player_name, team_name, "lemmy", "ace")) {
    result.mask = static_cast<std::uint8_t>(OriginalPcCheat::thunder);
  } else if (identity(player_name, team_name, "ramlosa", "h2o")) {
    result.mask = static_cast<std::uint8_t>(OriginalPcCheat::underwater);
  } else if (identity(player_name, team_name, "supercars", "grem")) {
    result.mask =
        static_cast<std::uint8_t>(OriginalPcCheat::supercars_camera);
  } else if (identity(player_name, team_name, "ignition", "uds")) {
    result.mask = static_cast<std::uint8_t>(OriginalPcCheat::ignition_camera);
  } else if (ascii_equal_case_insensitive(player_name, "tribute to tron")) {
    result.mask = static_cast<std::uint8_t>(OriginalPcCheat::tron);
  }

  result.avenger_map = identity(player_name, team_name, "avenger", "zx");
  result.retro_identity = identity(player_name, team_name, "r3tr0", "a500");
  return result;
}

OriginalPcLocationEasterEgg original_pc_location_easter_egg(
    const std::string_view track_name,
    const std::array<double, 3U> &world_position) noexcept {
  const auto is_track = [&](const std::string_view normal,
                            const std::string_view reverse) {
    return ascii_equal_case_insensitive(track_name, normal) ||
           ascii_equal_case_insensitive(track_name, reverse);
  };
  if (is_track("ruhrstadt", "ruhrstadtr")) {
    const auto x = world_position[0U] - 476.0;
    const auto z = world_position[2U] + 188.0;
    if (x * x + z * z < 400.0) {
      return OriginalPcLocationEasterEgg::black_lotus_club;
    }
  } else if (is_track("atlantika", "atlantikar")) {
    const auto x = world_position[0U] - 159.0;
    const auto y = world_position[1U] - 12.0;
    const auto z = world_position[2U] + 436.0;
    if (x * x + y * y + z * z < 4.0) {
      return OriginalPcLocationEasterEgg::atlantika_scroll;
    }
  }
  return OriginalPcLocationEasterEgg::none;
}

std::string_view original_pc_hidden_executable_story() noexcept {
  return hidden_executable_story;
}

} // namespace mh::game
