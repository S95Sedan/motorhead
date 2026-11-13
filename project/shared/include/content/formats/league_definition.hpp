#pragma once

#include <content/formats/ai_driver_profile.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mh::content {

struct LeagueColor {
  std::uint8_t red = 0U;
  std::uint8_t green = 0U;
  std::uint8_t blue = 0U;
};

struct LeaguePlayer {
  bool human = false;
  std::string name;
  std::uint32_t division = 0U;
  std::array<std::string, 4U> division_cars{};
  LeagueColor color;
  std::string horn;
  std::uint32_t score = 0U;
  std::uint32_t wins = 0U;
  std::uint32_t seconds = 0U;
  std::uint32_t thirds = 0U;
};

struct LeagueDefinition {
  std::string name;
  std::string creation_date;
  std::uint32_t race_number = 0U;
  std::uint32_t races_done = 0U;
  std::uint32_t laps_done = 0U;
  std::vector<LeaguePlayer> players;
  std::size_t recognized_fields = 0U;
  std::size_t unknown_fields = 0U;
};

struct LeagueRaceRosterSlot {
  LeaguePlayer player;
  std::string active_car;
  std::optional<AiDriverProfile> ai_profile;
};

using LeagueDivisionFinishingOrders =
    std::array<std::vector<std::string>, 4U>;

struct LeagueRaceProgression {
  bool season_complete = false;
  // Exact result-state value returned by p3.1 RVA 0x0006e814. Zero means the
  // current division still has races remaining.
  std::uint32_t original_result_code = 0U;
  std::uint32_t previous_division = 0U;
  std::uint32_t current_division = 0U;
  // p3.1 transition RVA 0x0006f2c4 calls DeleteFileA instead of the normal
  // LGF save owner when the human is the sorted division-0 champion.
  bool original_deletes_league_file = false;
};

[[nodiscard]] LeagueDefinition
parse_league(std::span<const std::uint8_t> decoded_text);
[[nodiscard]] std::vector<std::uint8_t>
serialize_league(const LeagueDefinition &league);
[[nodiscard]] LeagueDefinition
read_league(const std::filesystem::path &path,
            std::uint64_t maximum_file_bytes = 1024ULL * 1024ULL);
[[nodiscard]] bool league_file_valid_for_name(
    const std::filesystem::path &path, std::string_view encoded_file_name,
    std::uint64_t maximum_file_bytes = 1024ULL * 1024ULL) noexcept;
// The retail LGF groups one race field by division. The local human is moved
// to live slot zero; AI records retain their authored file order.
[[nodiscard]] std::vector<LeaguePlayer>
league_race_field(const LeagueDefinition &league);
// Resolves the active division's playable race roster. The human occupies slot
// zero. Every CPU slot is matched case-insensitively by LGF player name to one
// retail ADP PlayerNick, and the active-division car must agree in both files.
[[nodiscard]] std::vector<LeagueRaceRosterSlot>
load_league_race_roster(const LeagueDefinition &league,
                        const std::filesystem::path &profile_directory);
// Commits the front-end vehicle selection to the human player's active
// division so the LGF roster and the selected CAR remain identical.
void set_league_human_vehicle(LeagueDefinition &league, std::string_view car,
                              std::string_view horn);
// Starts a fresh League in an available division while retaining the
// template's fixed division capacities. One AI exchanges divisions with the
// human; all race and score state is otherwise preserved.
void set_league_human_starting_division(LeagueDefinition &league,
                                        std::uint32_t division);
// Applies the p3.1 active-division post-race contract. The finishing order
// must name every racer in the human player's division exactly once.
void apply_league_race_result(LeagueDefinition &league,
                              std::span<const std::string> finishing_order);
// Applies all four p3.1 division results, stable score ordering, and the
// season-end exchange between adjacent divisions. Full League progression
// uses fixed 2/8/8/8 division capacities. The mutation is atomic on failure.
[[nodiscard]] LeagueRaceProgression apply_league_race_cycle(
    LeagueDefinition &league,
    const LeagueDivisionFinishingOrders &finishing_orders,
    std::uint32_t races_in_season);
// Maps p3.1 League result states 1..7 to the completion DIVI entry in the
// original MOVIES.PAK. Other values are invalid outside a completed season.
[[nodiscard]] std::uint32_t
original_league_completion_movie_entry(std::uint32_t result_code);
void write_league(const std::filesystem::path &path,
                  const LeagueDefinition &league);
// Replaces one existing plain LGF file through a sibling temporary and backup.
// This is intended for reconstruction-owned league progression only.
void replace_league(const std::filesystem::path &path,
                    const LeagueDefinition &league);

} // namespace mh::content
