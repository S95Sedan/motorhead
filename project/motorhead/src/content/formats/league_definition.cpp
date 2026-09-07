#include <content/formats/league_definition.hpp>

#include <core/filesystem/atomic_file.hpp>
#include <core/error.hpp>
#include <content/formats/tbl_envelope.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::string lower_ascii(std::string value) {
  for (auto &character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
  }
  return value;
}

std::string trim(std::string_view value) {
  const auto whitespace = [](const char character) {
    return character == ' ' || character == '\t' || character == '\r';
  };
  while (!value.empty() && whitespace(value.front())) {
    value.remove_prefix(1U);
  }
  while (!value.empty() && whitespace(value.back())) {
    value.remove_suffix(1U);
  }
  return std::string(value);
}

std::uint32_t parse_u32(const std::string &value,
                        const std::string_view field) {
  std::istringstream input(value);
  std::uint64_t result = 0U;
  std::string trailing;
  if (!(input >> result) || (input >> trailing) ||
      result > std::numeric_limits<std::uint32_t>::max()) {
    throw ToolError(ExitCode::format, "LGF field " + std::string(field) +
                                          " must contain one unsigned integer");
  }
  return static_cast<std::uint32_t>(result);
}

LeagueColor parse_color(const std::string &value) {
  std::istringstream input(value);
  std::array<unsigned int, 3U> channels{};
  std::string trailing;
  if (!(input >> channels[0U] >> channels[1U] >> channels[2U]) ||
      (input >> trailing) ||
      std::any_of(channels.begin(), channels.end(),
                  [](const auto channel) { return channel > 255U; })) {
    throw ToolError(ExitCode::format,
                    "LGF Colour must contain exactly three byte values");
  }
  return {static_cast<std::uint8_t>(channels[0U]),
          static_cast<std::uint8_t>(channels[1U]),
          static_cast<std::uint8_t>(channels[2U])};
}

void require_player_complete(const LeaguePlayer &player,
                             const std::array<bool, 11U> &fields) {
  if (!std::all_of(fields.begin(), fields.end(),
                   [](const bool value) { return value; })) {
    throw ToolError(ExitCode::format, "LGF player " + player.name +
                                          " has an incomplete authored record");
  }
}

void require_single_line(const std::string_view value,
                         const std::string_view field) {
  if (value.empty() || value.find_first_of("\r\n") != std::string_view::npos) {
    throw ToolError(ExitCode::format, "LGF " + std::string(field) +
                                          " must contain one non-empty line");
  }
}

} // namespace

LeagueDefinition
parse_league(const std::span<const std::uint8_t> decoded_text) {
  if (decoded_text.empty() || decoded_text.size() > 1024U * 1024U) {
    throw ToolError(ExitCode::format,
                    "LGF text size is outside supported bounds");
  }
  if (std::find(decoded_text.begin(), decoded_text.end(), 0U) !=
      decoded_text.end()) {
    throw ToolError(ExitCode::format, "LGF text contains a NUL byte");
  }

  const std::string text(decoded_text.begin(), decoded_text.end());
  std::istringstream lines(text);
  LeagueDefinition result;
  std::array<bool, 5U> league_fields{};
  std::uint32_t current_division = 0U;
  bool have_division = false;
  LeaguePlayer current_player;
  std::array<bool, 11U> player_fields{};
  bool have_player = false;

  const auto finish_player = [&]() {
    if (!have_player) {
      return;
    }
    require_player_complete(current_player, player_fields);
    if (result.players.size() >= 256U) {
      throw ToolError(ExitCode::format,
                      "LGF player count exceeds supported bounds");
    }
    result.players.push_back(current_player);
    current_player = {};
    player_fields.fill(false);
    have_player = false;
  };

  std::string line;
  while (std::getline(lines, line)) {
    if (line.size() > 4096U) {
      throw ToolError(ExitCode::format, "LGF line exceeds supported bounds");
    }
    const auto cleaned = trim(line);
    if (cleaned.empty()) {
      continue;
    }
    if (cleaned.starts_with("//")) {
      const auto marker = cleaned.find("Division ");
      if (marker != std::string::npos) {
        const auto begin = marker + std::string_view("Division ").size();
        const auto end = cleaned.find_first_not_of("0123456789", begin);
        const auto value = cleaned.substr(begin, end - begin);
        current_division = parse_u32(value, "Division comment");
        if (current_division > 3U) {
          throw ToolError(ExitCode::format,
                          "LGF division comment is outside 0 through 3");
        }
        have_division = true;
      }
      continue;
    }

    const auto separator = cleaned.find_first_of(" \t");
    if (separator == std::string::npos) {
      throw ToolError(ExitCode::format, "LGF field has no value: " + cleaned);
    }
    const auto key = lower_ascii(cleaned.substr(0U, separator));
    const auto value = trim(std::string_view(cleaned).substr(separator + 1U));
    if (value.empty()) {
      throw ToolError(ExitCode::format, "LGF field has an empty value: " + key);
    }

    if (key == "aiplayer" || key == "humanplayer") {
      finish_player();
      if (!have_division) {
        throw ToolError(ExitCode::format,
                        "LGF player appears before a division marker");
      }
      current_player.human = key == "humanplayer";
      current_player.name = value;
      current_player.division = current_division;
      player_fields[0U] = true;
      have_player = true;
      ++result.recognized_fields;
      continue;
    }

    if (!have_player) {
      const auto field = key == "leaguename"   ? 0U
                         : key == "createdate" ? 1U
                         : key == "racenumber" ? 2U
                         : key == "racesdone"  ? 3U
                         : key == "lapsdone"   ? 4U
                                               : 5U;
      if (field == 5U) {
        ++result.unknown_fields;
        continue;
      }
      if (league_fields[field]) {
        throw ToolError(ExitCode::format, "duplicate LGF league field " + key);
      }
      league_fields[field] = true;
      if (field == 0U) {
        result.name = value;
      } else if (field == 1U) {
        if (value.size() != 8U ||
            !std::all_of(value.begin(), value.end(), [](const char character) {
              return std::isdigit(static_cast<unsigned char>(character)) != 0;
            })) {
          throw ToolError(ExitCode::format,
                          "LGF CreateDate must contain exactly eight digits");
        }
        result.creation_date = value;
      } else if (field == 2U) {
        result.race_number = parse_u32(value, key);
      } else if (field == 3U) {
        result.races_done = parse_u32(value, key);
      } else {
        result.laps_done = parse_u32(value, key);
      }
      ++result.recognized_fields;
      continue;
    }

    std::size_t field = 11U;
    if (key.starts_with("cardivision") && key.size() == 12U &&
        key.back() >= '0' && key.back() <= '3') {
      field = 1U + static_cast<std::size_t>(key.back() - '0');
    } else if (key == "colour") {
      field = 5U;
    } else if (key == "carhorn") {
      field = 6U;
    } else if (key == "score") {
      field = 7U;
    } else if (key == "wins") {
      field = 8U;
    } else if (key == "seconds") {
      field = 9U;
    } else if (key == "thirds") {
      field = 10U;
    }
    if (field == 11U) {
      ++result.unknown_fields;
      continue;
    }
    if (player_fields[field]) {
      throw ToolError(ExitCode::format, "duplicate LGF player field " + key);
    }
    player_fields[field] = true;
    if (field >= 1U && field <= 4U) {
      current_player.division_cars[field - 1U] = value;
    } else if (field == 5U) {
      current_player.color = parse_color(value);
    } else if (field == 6U) {
      current_player.horn = value;
    } else {
      auto *destination = field == 7U   ? &current_player.score
                          : field == 8U ? &current_player.wins
                          : field == 9U ? &current_player.seconds
                                        : &current_player.thirds;
      *destination = parse_u32(value, key);
    }
    ++result.recognized_fields;
  }
  finish_player();

  if (!std::all_of(league_fields.begin(), league_fields.end(),
                   [](const bool value) { return value; }) ||
      result.players.empty()) {
    throw ToolError(ExitCode::format,
                    "LGF is missing its header or player records");
  }
  return result;
}

std::vector<std::uint8_t> serialize_league(const LeagueDefinition &league) {
  require_single_line(league.name, "LeagueName");
  if (league.creation_date.size() != 8U ||
      !std::all_of(league.creation_date.begin(), league.creation_date.end(),
                   [](const char character) {
                     return std::isdigit(
                                static_cast<unsigned char>(character)) != 0;
                   })) {
    throw ToolError(ExitCode::format,
                    "LGF CreateDate must contain exactly eight digits");
  }
  if (league.players.empty() || league.players.size() > 256U) {
    throw ToolError(ExitCode::format,
                    "LGF player count is outside supported bounds");
  }

  std::ostringstream text;
  text << "// LeagueFile\n"
       << "//---------- League ---------\n"
       << "LeagueName " << league.name << '\n'
       << "CreateDate " << league.creation_date << '\n'
       << "RaceNumber " << league.race_number << '\n'
       << "RacesDone " << league.races_done << '\n'
       << "LapsDone " << league.laps_done << '\n';
  std::size_t serialized_players = 0U;
  for (std::uint32_t division = 0U; division < 4U; ++division) {
    text << "//---------- Division " << division << " --------\n";
    for (const auto &player : league.players) {
      if (player.division != division) {
        continue;
      }
      require_single_line(player.name, "player name");
      require_single_line(player.horn, "CarHorn");
      for (const auto &car : player.division_cars) {
        require_single_line(car, "CarDivision");
      }
      text << (player.human ? "HumanPlayer " : "AIPlayer ") << player.name
           << '\n';
      for (std::size_t car = 0U; car < player.division_cars.size(); ++car) {
        text << "CarDivision" << car << ' ' << player.division_cars[car]
             << '\n';
      }
      text << "Colour " << static_cast<unsigned int>(player.color.red) << ' '
           << static_cast<unsigned int>(player.color.green) << ' '
           << static_cast<unsigned int>(player.color.blue) << '\n'
           << "CarHorn " << player.horn << '\n'
           << "Score " << player.score << '\n'
           << "Wins " << player.wins << '\n'
           << "Seconds " << player.seconds << '\n'
           << "Thirds " << player.thirds << '\n';
      ++serialized_players;
    }
  }
  if (serialized_players != league.players.size()) {
    throw ToolError(ExitCode::format,
                    "LGF player division is outside 0 through 3");
  }
  const auto encoded = text.str();
  std::vector<std::uint8_t> result(encoded.begin(), encoded.end());
  // Keep the encoder bounded by the same strict parser used for retail LGFs.
  static_cast<void>(parse_league(result));
  return result;
}

LeagueDefinition read_league(const std::filesystem::path &path,
                             const std::uint64_t maximum_file_bytes) {
  const auto envelope =
      read_tbl(path, {}, maximum_file_bytes, maximum_file_bytes);
  if (!envelope.decoded) {
    throw ToolError(ExitCode::format, "LGF TBL payload is not decoded: " +
                                          envelope.decode_status);
  }
  return parse_league(envelope.payload);
}

bool league_file_valid_for_name(const std::filesystem::path &path,
                                const std::string_view encoded_file_name,
                                const std::uint64_t maximum_file_bytes) noexcept {
  try {
    const auto envelope =
        read_tbl(path, std::string(encoded_file_name), maximum_file_bytes,
                 maximum_file_bytes);
    return envelope.decoded && !parse_league(envelope.payload).players.empty();
  } catch (...) {
    return false;
  }
}

std::vector<LeaguePlayer> league_race_field(const LeagueDefinition &league) {
  const auto human_count =
      std::count_if(league.players.begin(), league.players.end(),
                    [](const LeaguePlayer &player) { return player.human; });
  if (human_count != 1) {
    throw ToolError(ExitCode::format,
                    "league race requires exactly one human player");
  }
  const auto human =
      std::find_if(league.players.begin(), league.players.end(),
                   [](const LeaguePlayer &player) { return player.human; });
  std::vector<LeaguePlayer> result;
  result.push_back(*human);
  for (const auto &player : league.players) {
    if (!player.human && player.division == human->division) {
      result.push_back(player);
    }
  }
  if (result.size() < 2U || result.size() > 8U) {
    throw ToolError(ExitCode::format,
                    "league division must contain two through eight racers");
  }
  return result;
}

std::vector<LeagueRaceRosterSlot>
load_league_race_roster(const LeagueDefinition &league,
                        const std::filesystem::path &profile_directory) {
  const auto field = league_race_field(league);
  const auto division = static_cast<std::size_t>(field.front().division);
  if (division >= field.front().division_cars.size()) {
    throw ToolError(ExitCode::format,
                    "league human division is outside zero through three");
  }

  std::vector<AiDriverProfile> profiles;
  for (const auto &entry :
       std::filesystem::directory_iterator(profile_directory)) {
    if (entry.is_regular_file() &&
        lower_ascii(entry.path().extension().string()) == ".adp") {
      profiles.push_back(read_ai_driver_profile(entry.path()));
    }
  }

  std::vector<LeagueRaceRosterSlot> result;
  result.reserve(field.size());
  result.push_back({field.front(), field.front().division_cars[division],
                    std::nullopt});
  for (const auto &player :
       std::span<const LeaguePlayer>(field).subspan(1U)) {
    const auto matches_profile =
        [&player](const AiDriverProfile &profile) {
          return lower_ascii(profile.player_nick) ==
                 lower_ascii(player.name);
        };
    const auto match =
        std::find_if(profiles.begin(), profiles.end(), matches_profile);
    if (match == profiles.end()) {
      throw ToolError(ExitCode::format,
                      "league opponent has no matching ADP profile: " +
                          player.name);
    }
    if (std::count_if(profiles.begin(), profiles.end(), matches_profile) != 1) {
      throw ToolError(ExitCode::format,
                      "league opponent has an ambiguous ADP profile: " +
                          player.name);
    }
    const auto &active_car = player.division_cars[division];
    if (lower_ascii(match->division_cars[division]) !=
        lower_ascii(active_car)) {
      throw ToolError(ExitCode::format,
                      "league opponent ADP car does not match its LGF car: " +
                          player.name);
    }
    result.push_back({player, active_car, *match});
  }
  return result;
}

void set_league_human_vehicle(LeagueDefinition &league,
                              const std::string_view car,
                              const std::string_view horn) {
  if (car.empty() || horn.empty()) {
    throw ToolError(ExitCode::usage,
                    "league human car and horn must not be empty");
  }
  const auto human = std::find_if(
      league.players.begin(), league.players.end(),
      [](const LeaguePlayer &player) { return player.human; });
  if (human == league.players.end() ||
      std::count_if(league.players.begin(), league.players.end(),
                    [](const LeaguePlayer &player) { return player.human; }) !=
          1) {
    throw ToolError(ExitCode::format,
                    "league must contain exactly one human player");
  }
  if (human->division >= human->division_cars.size()) {
    throw ToolError(ExitCode::format,
                    "league human division is outside zero through three");
  }
  human->division_cars[human->division] = car;
  human->horn = horn;
}

void set_league_human_starting_division(LeagueDefinition &league,
                                        const std::uint32_t division) {
  if (division >= 4U) {
    throw ToolError(ExitCode::usage,
                    "league starting division is outside zero through three");
  }
  auto updated = league;
  const auto human = std::find_if(
      updated.players.begin(), updated.players.end(),
      [](const LeaguePlayer &player) { return player.human; });
  if (human == updated.players.end() ||
      std::count_if(updated.players.begin(), updated.players.end(),
                    [](const LeaguePlayer &player) { return player.human; }) !=
          1) {
    throw ToolError(ExitCode::format,
                    "league must contain exactly one human player");
  }
  if (human->division >= 4U) {
    throw ToolError(ExitCode::format,
                    "league human division is outside zero through three");
  }
  if (human->division == division) {
    return;
  }
  const auto displaced = std::find_if(
      updated.players.begin(), updated.players.end(),
      [division](const LeaguePlayer &player) {
        return !player.human && player.division == division;
      });
  if (displaced == updated.players.end()) {
    throw ToolError(ExitCode::format,
                    "league starting division has no AI exchange slot");
  }
  displaced->division = human->division;
  human->division = division;
  league = std::move(updated);
}

void apply_league_race_result(
    LeagueDefinition &league,
    const std::span<const std::string> finishing_order) {
  static constexpr std::array<std::uint32_t, 8U> points_by_position{
      12U, 10U, 8U, 6U, 4U, 3U, 2U, 1U};
  const auto field = league_race_field(league);
  if (finishing_order.size() != field.size()) {
    throw ToolError(
        ExitCode::format,
        "league result must contain every active-division racer exactly once");
  }

  const auto human =
      std::find_if(league.players.begin(), league.players.end(),
                   [](const LeaguePlayer &player) { return player.human; });
  std::vector<LeaguePlayer *> ordered_players;
  ordered_players.reserve(finishing_order.size());
  for (const auto &name : finishing_order) {
    const auto lowered_name = lower_ascii(name);
    LeaguePlayer *match = nullptr;
    for (auto &player : league.players) {
      if (player.division != human->division ||
          lower_ascii(player.name) != lowered_name) {
        continue;
      }
      if (match != nullptr) {
        throw ToolError(ExitCode::format,
                        "league result name is ambiguous: " + name);
      }
      match = &player;
    }
    if (match == nullptr) {
      throw ToolError(ExitCode::format,
                      "league result names an inactive racer: " + name);
    }
    if (std::find(ordered_players.begin(), ordered_players.end(), match) !=
        ordered_players.end()) {
      throw ToolError(ExitCode::format, "league result repeats racer: " + name);
    }
    ordered_players.push_back(match);
  }

  for (std::size_t rank = 0U; rank < ordered_players.size(); ++rank) {
    auto &player = *ordered_players[rank];
    player.score += points_by_position[rank];
    if (rank == 0U) {
      ++player.wins;
    } else if (rank == 1U) {
      ++player.seconds;
    } else if (rank == 2U) {
      ++player.thirds;
    }
  }
  ++league.race_number;
}

LeagueRaceProgression apply_league_race_cycle(
    LeagueDefinition &league,
    const LeagueDivisionFinishingOrders &finishing_orders,
    const std::uint32_t races_in_season) {
  static constexpr std::array<std::uint32_t, 8U> points_by_position{
      12U, 10U, 8U, 6U, 4U, 3U, 2U, 1U};
  static constexpr std::array<std::size_t, 4U> division_capacities{
      2U, 8U, 8U, 8U};
  if (races_in_season == 0U || league.race_number >= races_in_season) {
    throw ToolError(ExitCode::format,
                    "LGF RaceNumber is outside the active League schedule");
  }

  auto updated = league;
  std::array<std::vector<std::size_t>, 4U> division_indices;
  std::size_t human_count = 0U;
  for (std::size_t index = 0U; index < updated.players.size(); ++index) {
    const auto division = updated.players[index].division;
    if (division >= division_indices.size()) {
      throw ToolError(ExitCode::format,
                      "LGF player division is outside zero through three");
    }
    division_indices[division].push_back(index);
    human_count += updated.players[index].human ? 1U : 0U;
  }
  if (human_count != 1U) {
    throw ToolError(ExitCode::format,
                    "full League progression requires exactly one human");
  }
  for (std::size_t division = 0U; division < division_indices.size();
       ++division) {
    if (division_indices[division].size() != division_capacities[division] ||
        finishing_orders[division].size() != division_capacities[division]) {
      throw ToolError(ExitCode::format,
                      "full League progression requires 2/8/8/8 division fields");
    }

    std::vector<LeaguePlayer *> ordered_players;
    ordered_players.reserve(finishing_orders[division].size());
    for (const auto &name : finishing_orders[division]) {
      const auto lowered_name = lower_ascii(name);
      LeaguePlayer *match = nullptr;
      for (const auto player_index : division_indices[division]) {
        auto &player = updated.players[player_index];
        if (lower_ascii(player.name) != lowered_name) {
          continue;
        }
        if (match != nullptr) {
          throw ToolError(ExitCode::format,
                          "League division result name is ambiguous: " + name);
        }
        match = &player;
      }
      if (match == nullptr) {
        throw ToolError(ExitCode::format,
                        "League division result names an inactive racer: " +
                            name);
      }
      if (std::find(ordered_players.begin(), ordered_players.end(), match) !=
          ordered_players.end()) {
        throw ToolError(ExitCode::format,
                        "League division result repeats racer: " + name);
      }
      ordered_players.push_back(match);
    }

    for (std::size_t rank = 0U; rank < ordered_players.size(); ++rank) {
      auto &player = *ordered_players[rank];
      player.score += points_by_position[rank];
      if (rank == 0U) {
        ++player.wins;
      } else if (rank == 1U) {
        ++player.seconds;
      } else if (rank == 2U) {
        ++player.thirds;
      }
    }
  }

  // p3.1 bubble-sorts each fixed division only when the following score is
  // strictly greater, preserving authored order for tied totals.
  for (std::size_t division = 0U; division < division_indices.size();
       ++division) {
    std::vector<LeaguePlayer> ranked;
    ranked.reserve(division_indices[division].size());
    for (const auto index : division_indices[division]) {
      ranked.push_back(updated.players[index]);
    }
    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const LeaguePlayer &left, const LeaguePlayer &right) {
                       return left.score > right.score;
                     });
    for (std::size_t rank = 0U; rank < ranked.size(); ++rank) {
      updated.players[division_indices[division][rank]] =
          std::move(ranked[rank]);
    }
  }

  const auto find_human = [&updated]() {
    return std::find_if(updated.players.begin(), updated.players.end(),
                        [](const LeaguePlayer &player) { return player.human; });
  };
  auto human = find_human();
  const auto previous_division = human->division;
  ++updated.race_number;
  ++updated.races_done;
  if (updated.race_number < races_in_season) {
    league = std::move(updated);
    return {false, 0U, previous_division, previous_division, false};
  }

  const auto &human_division_indices = division_indices[previous_division];
  const auto human_rank = static_cast<std::size_t>(std::distance(
      human_division_indices.begin(),
      std::find(human_division_indices.begin(), human_division_indices.end(),
                static_cast<std::size_t>(
                    std::distance(updated.players.begin(), human)))));
  std::uint32_t result_code = 5U;
  if (previous_division == 3U && human_rank <= 1U) {
    result_code = 1U;
  } else if (previous_division == 2U && human_rank <= 1U) {
    result_code = 2U;
  } else if (previous_division == 1U && human_rank == 0U) {
    result_code = 3U;
  } else if (previous_division == 1U && human_rank >= 6U) {
    result_code = 6U;
  } else if (previous_division == 0U && human_rank == 0U) {
    result_code = 4U;
  } else if (previous_division == 0U && human_rank == 1U) {
    result_code = 7U;
  }

  for (auto &player : updated.players) {
    player.score = 0U;
    player.wins = 0U;
    player.seconds = 0U;
    player.thirds = 0U;
  }
  std::swap(updated.players[division_indices[0U][1U]],
            updated.players[division_indices[1U][0U]]);
  std::swap(updated.players[division_indices[1U][6U]],
            updated.players[division_indices[2U][0U]]);
  std::swap(updated.players[division_indices[1U][7U]],
            updated.players[division_indices[2U][1U]]);
  std::swap(updated.players[division_indices[2U][6U]],
            updated.players[division_indices[3U][0U]]);
  std::swap(updated.players[division_indices[2U][7U]],
            updated.players[division_indices[3U][1U]]);
  for (std::size_t division = 0U; division < division_indices.size();
       ++division) {
    for (const auto index : division_indices[division]) {
      updated.players[index].division = static_cast<std::uint32_t>(division);
    }
  }
  updated.race_number = 0U;
  human = find_human();
  const auto current_division = human->division;
  league = std::move(updated);
  return {true, result_code, previous_division, current_division,
          result_code == 4U};
}

std::uint32_t
original_league_completion_movie_entry(const std::uint32_t result_code) {
  switch (result_code) {
  case 1U:
    return 3U;
  case 2U:
    return 4U;
  case 3U:
    return 5U;
  case 4U:
    return 8U;
  case 5U:
    return 6U;
  case 6U:
  case 7U:
    return 7U;
  default:
    throw ToolError(ExitCode::format,
                    "League completion result code is outside one through seven");
  }
}

namespace {

std::vector<std::uint8_t>
encoded_league_file(const std::filesystem::path &destination,
                    const LeagueDefinition &league) {
  const auto plain = serialize_league(league);
  const auto encoded =
      encode_tbl_transform(plain, destination.filename().string());
  std::vector<std::uint8_t> file{'T', 'B', 'L', 0x05U};
  file.insert(file.end(), encoded.begin(), encoded.end());
  return file;
}

} // namespace

void write_league(const std::filesystem::path &path,
                  const LeagueDefinition &league) {
  if (path.empty() || path.filename().empty()) {
    throw ToolError(ExitCode::usage, "LGF output path is empty");
  }
  const auto destination = std::filesystem::absolute(path).lexically_normal();
  if (!destination.has_parent_path()) {
    throw ToolError(ExitCode::usage, "LGF output has no parent directory");
  }
  std::filesystem::create_directories(destination.parent_path());
  if (std::filesystem::exists(destination)) {
    throw ToolError(ExitCode::input, "LGF output already exists");
  }

  const auto file = encoded_league_file(destination, league);

  const auto validator = [name = destination.filename().string()](
                             const auto &candidate) {
    return league_file_valid_for_name(candidate, name);
  };
  mh::common::write_atomic_file(destination, file, validator);
}

void replace_league(const std::filesystem::path &path,
                    const LeagueDefinition &league) {
  if (path.empty() || path.filename().empty()) {
    throw ToolError(ExitCode::usage, "LGF replacement path is empty");
  }
  const auto destination = std::filesystem::absolute(path).lexically_normal();
  std::error_code error;
  const auto status = std::filesystem::symlink_status(destination, error);
  if (error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    throw ToolError(ExitCode::input,
                    "LGF replacement target is not a plain file");
  }

  const auto validator = [name = destination.filename().string()](
                             const auto &candidate) {
    return league_file_valid_for_name(candidate, name);
  };
  mh::common::write_atomic_file(
      destination, encoded_league_file(destination, league), validator);
}

} // namespace mh::content
