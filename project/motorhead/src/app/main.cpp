#include <content/formats/car_definition.hpp>
#include <content/formats/s40_content.hpp>
#include <content/formats/divi_video_archive.hpp>
#include <content/formats/fnt_bitmap_font.hpp>
#include <content/formats/league_definition.hpp>
#include <content/formats/lob_line_object.hpp>
#include <content/formats/mde_replay.hpp>
#include <content/formats/menu_sound_config.hpp>
#include <content/formats/spr_sprite_archive.hpp>
#include <content/formats/sprite_positions.hpp>
#include <content/formats/tga_image.hpp>
#include <content/formats/track_definition.hpp>
#include <content/formats/unlock_catalog.hpp>
#include <core/filesystem/atomic_file.hpp>
#include <core/filesystem/case_insensitive.hpp>
#include <core/logging/runtime_log.hpp>
#include <disc/cd_audio.hpp>
#include <disc/cue_sheet.hpp>
#include <game/ai/vehicle_runtime.hpp>
#include <network/session.hpp>
#include <platform/sdl_input.hpp>
#include <platform/sdl_audio_output.hpp>
#include <ui/config/game_config.hpp>
#include <ui/config/personal_options.hpp>
#include <ui/config/user_data.hpp>
#include <ui/frontend/controller.hpp>
#include <ui/frontend/renderer.hpp>

#include "runtime/race_runtime.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using mh::common::resolve_relative_case_insensitive;

// The three S40 CD tracks are mastered roughly 3.5 dB below Motorhead's
// soundtrack. Compensate only while playing those sources so both modes share
// the same saved volume scale and perceived balance.
constexpr float s40_music_gain = 1.4962357F;

template <typename T, void (*Destroy)(T *)>
using SdlPointer = std::unique_ptr<T, decltype(Destroy)>;

struct Options {
  std::filesystem::path reference_root;
  std::filesystem::path user_data_root;
  std::filesystem::path movies_path;
  std::filesystem::path music_path;
  std::filesystem::path disc_cue_path;
  std::optional<mh::disc::MountedCddaDisc> mounted_cdda;
  std::filesystem::path startup_handoff_report_path;
  std::uint32_t maximum_frames = 0U;
  bool audio = true;
  bool startup_movies = true;
  bool startup_movies_unpaced = false;
  bool embedded_race_smoke = false;
};

enum class TransitionLeg : std::uint8_t {
  none,
  outgoing,
  incoming,
};

struct ScreenTransition {
  TransitionLeg leg = TransitionLeg::none;
  mh::ui::FrontEndScreen from = mh::ui::FrontEndScreen::main_menu;
  mh::ui::FrontEndScreen to = mh::ui::FrontEndScreen::main_menu;
  std::uint32_t counter_ms = 0U;

  [[nodiscard]] bool active() const noexcept {
    return leg != TransitionLeg::none;
  }

  void begin(const mh::ui::FrontEndScreen old_screen,
             const mh::ui::FrontEndScreen new_screen) noexcept {
    from = old_screen;
    to = new_screen;
    leg = TransitionLeg::outgoing;
    counter_ms = mh::ui::front_end_transition_leg_ms;
  }

  [[nodiscard]] mh::ui::FrontEndScreen visible_screen() const noexcept {
    return leg == TransitionLeg::outgoing ? from : to;
  }

  [[nodiscard]] std::uint32_t phase() const noexcept {
    return mh::ui::front_end_transition_phase(counter_ms);
  }

  void advance(std::uint32_t elapsed_ms) noexcept {
    while (active() && elapsed_ms != 0U) {
      if (leg == TransitionLeg::outgoing) {
        const auto consumed = std::min(counter_ms, elapsed_ms);
        counter_ms -= consumed;
        elapsed_ms -= consumed;
        if (counter_ms == 0U) {
          leg = TransitionLeg::incoming;
        }
      } else {
        const auto remaining = mh::ui::front_end_transition_leg_ms - counter_ms;
        const auto consumed = std::min(remaining, elapsed_ms);
        counter_ms += consumed;
        elapsed_ms -= consumed;
        if (counter_ms == mh::ui::front_end_transition_leg_ms) {
          leg = TransitionLeg::none;
        }
      }
    }
  }
};

void require(const bool condition, const std::string_view operation) {
  if (!condition) {
    throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
  }
}

std::string
required_track_reference(const mh::content::TrackDefinition &definition,
                         const std::string_view field) {
  std::optional<std::string> result;
  for (const auto &reference : definition.references) {
    if (reference.field != field) {
      continue;
    }
    if (result.has_value()) {
      throw std::runtime_error("track has multiple " + std::string(field) +
                               " references: " + definition.name);
    }
    result = reference.value;
  }
  if (!result.has_value()) {
    throw std::runtime_error("track has no " + std::string(field) +
                             " reference: " + definition.name);
  }
  return *result;
}

std::filesystem::path executable_directory(const char *executable) {
  if (const auto *base_path = SDL_GetBasePath();
      base_path != nullptr && *base_path != '\0') {
    auto directory = std::filesystem::path(base_path).lexically_normal();
    // SDL_GetBasePath() is a directory and includes a trailing separator.
    // std::filesystem retains that empty final component, which makes a
    // subsequent parent_path() return the executable directory itself.
    if (directory.filename().empty()) {
      directory = directory.parent_path();
    }
    return directory;
  }
  return std::filesystem::absolute(executable).lexically_normal().parent_path();
}

std::filesystem::path
default_user_data_root(const std::filesystem::path &executable_root) {
  if (const auto *override_root = std::getenv("MOTORHEAD_USER_DATA");
      override_root != nullptr && *override_root != '\0') {
    return std::filesystem::absolute(override_root).lexically_normal();
  }
  return (executable_root / "User").lexically_normal();
}

std::filesystem::path startup_user_data_root(const int argc, char **argv) {
  for (int index = 1; index + 1 < argc; ++index) {
    if (std::string_view(argv[index]) == "--user-data-root" ||
        std::string_view(argv[index]) == "--configuration-root") {
      return std::filesystem::absolute(argv[index + 1]).lexically_normal();
    }
  }
  return default_user_data_root(executable_directory(argv[0]));
}

Options parse_options(const int argc, char **argv) {
  const auto executable_root = executable_directory(argv[0]);
  const auto is_reference_root = [](const std::filesystem::path &candidate) {
    return std::filesystem::is_directory(candidate / "Game") &&
           std::filesystem::is_directory(candidate / "Cars") &&
           std::filesystem::is_directory(candidate / "Data") &&
           std::filesystem::is_directory(candidate / "Tracks");
  };
  const auto portable_defaults = [&]() {
    Options result;
    result.user_data_root = default_user_data_root(executable_root);
    if (is_reference_root(executable_root)) {
      result.reference_root = executable_root.lexically_normal();
    }
    if (result.reference_root.empty()) {
      throw std::runtime_error(
          "runtime data was not found beside Motorhead.exe; expected the "
          "original Game, Cars, Data, and Tracks folders beside it");
    }
    const auto music_directory = executable_root / "Music";
    const auto menu_track = music_directory / "track03.wav";
    if (std::filesystem::is_regular_file(menu_track)) {
      result.music_path = menu_track.lexically_normal();
    }
    const auto complete_local_soundtrack = [&]() {
      for (auto track = 2U; track <= 11U; ++track) {
        std::ostringstream name;
        name << "track" << std::setw(2) << std::setfill('0') << track << ".wav";
        if (!std::filesystem::is_regular_file(music_directory / name.str())) {
          return false;
        }
      }
      return true;
    }();
    // A complete installed soundtrack is self-contained. Avoid even opening
    // the optical drive in this case so launching Motorhead does not spin up
    // a mounted retail disc that will never be used.
    if (!complete_local_soundtrack) {
      result.mounted_cdda = mh::disc::find_mounted_motorhead_cdda();
      if (!result.mounted_cdda.has_value() &&
          mh::disc::find_mounted_motorhead_volume().has_value()) {
        for (const auto &candidate :
             {executable_root / "MOTORHEAD.cue",
              music_directory / "MOTORHEAD.cue",
              executable_root.parent_path() / "sources" / "iso" /
                  "MOTORHEAD.cue"}) {
          if (std::filesystem::is_regular_file(candidate)) {
            result.disc_cue_path = candidate.lexically_normal();
            break;
          }
        }
      }
    }
    return result;
  };

  Options result;
  result.user_data_root = default_user_data_root(executable_root);
  int first_option = 1;
  if (argc >= 2 && !std::string_view(argv[1]).starts_with("--")) {
    result.reference_root =
        std::filesystem::absolute(argv[1]).lexically_normal();
    first_option = 2;
  } else {
    result = portable_defaults();
  }
  for (int index = first_option; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--frames") {
      if (++index >= argc) {
        throw std::runtime_error("--frames requires a value");
      }
      const auto parsed = std::stoull(argv[index]);
      if (parsed == 0U || parsed > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("--frames is outside the supported range");
      }
      result.maximum_frames = static_cast<std::uint32_t>(parsed);
    } else if (argument == "--no-audio") {
      result.audio = false;
    } else if (argument == "--movies") {
      if (++index >= argc) {
        throw std::runtime_error("--movies requires a path");
      }
      result.movies_path =
          std::filesystem::absolute(argv[index]).lexically_normal();
    } else if (argument == "--music") {
      if (++index >= argc) {
        throw std::runtime_error("--music requires a WAV path");
      }
      result.music_path =
          std::filesystem::absolute(argv[index]).lexically_normal();
    } else if (argument == "--disc-cue") {
      if (++index >= argc) {
        throw std::runtime_error("--disc-cue requires a CUE path");
      }
      result.disc_cue_path =
          std::filesystem::absolute(argv[index]).lexically_normal();
    } else if (argument == "--user-data-root") {
      if (++index >= argc) {
        throw std::runtime_error("--user-data-root requires a path");
      }
      result.user_data_root =
          std::filesystem::absolute(argv[index]).lexically_normal();
    } else if (argument == "--startup-handoff-report") {
      if (++index >= argc) {
        throw std::runtime_error("--startup-handoff-report requires a path");
      }
      result.startup_handoff_report_path =
          std::filesystem::absolute(argv[index]).lexically_normal();
      result.startup_movies_unpaced = true;
    } else if (argument == "--skip-intro") {
      result.startup_movies = false;
    } else if (argument == "--embedded-race-smoke") {
      result.embedded_race_smoke = true;
    } else {
      throw std::runtime_error("unknown option: " + std::string(argument));
    }
  }
  return result;
}

std::string trim_copy(std::string value) {
  const auto is_space = [](const unsigned char character) {
    return std::isspace(character) != 0;
  };
  const auto first = std::find_if_not(value.begin(), value.end(), is_space);
  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), is_space).base();
  if (first >= last) {
    return {};
  }
  return {first, last};
}

mh::ui::OnePlayerPanelValues
load_one_player_panel_values(const std::filesystem::path &reference_root) {
  const auto path = mh::ui::motorhead_configuration_path(reference_root);
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("motorhead.cfg was not found: " + path.string());
  }
  std::unordered_map<std::string, std::string> fields;
  std::string line;
  while (std::getline(stream, line)) {
    if (const auto comment = line.find("//"); comment != std::string::npos) {
      line.erase(comment);
    }
    line = trim_copy(std::move(line));
    if (line.empty()) {
      continue;
    }
    const auto separator = std::find_if(line.begin(), line.end(),
                                        [](const unsigned char character) {
                                          return std::isspace(character) != 0;
                                        });
    const std::string key(line.begin(), separator);
    const std::string value =
        separator == line.end() ? std::string{}
                                : trim_copy(std::string(separator, line.end()));
    fields.insert_or_assign(key, value);
  }

  mh::ui::OnePlayerPanelValues result;
  const auto get = [&fields](const std::string_view key) {
    const auto found = fields.find(std::string(key));
    return found == fields.end() ? std::string{} : found->second;
  };
  const auto game_type = get("GameType");
  if (game_type == "League") {
    result.mode = mh::ui::OnePlayerMode::league_race;
  } else if (game_type == "TimeAttack") {
    result.mode = mh::ui::OnePlayerMode::time_attack;
  } else if (game_type == "GhostMode") {
    result.mode = mh::ui::OnePlayerMode::ghost_mode;
  } else if (game_type == "Single") {
    result.mode = mh::ui::OnePlayerMode::single_race;
  } else {
    throw std::runtime_error("unsupported GameType in motorhead.cfg: " +
                             game_type);
  }
  result.car = get("CarName");
  result.third_value = result.mode == mh::ui::OnePlayerMode::ghost_mode
                           ? get("DemoFileName")
                           : (result.mode == mh::ui::OnePlayerMode::league_race
                                  ? std::string{}
                                  : get("TrackLaps"));
  result.difficulty = get("Difficulty");
  result.transmission = get("CarTransmission");
  result.track = get("TrackName");
  return result;
}

bool ascii_equal_case_insensitive(std::string_view left,
                                  std::string_view right);

void reset_league_progress(mh::content::LeagueDefinition &league) noexcept {
  league.race_number = 0U;
  league.races_done = 0U;
  league.laps_done = 0U;
  for (auto &player : league.players) {
    player.score = 0U;
    player.wins = 0U;
    player.seconds = 0U;
    player.thirds = 0U;
  }
}

void apply_player_to_league(
    mh::content::LeagueDefinition &league,
    const mh::ui::OnePlayerPanelValues &one_player,
    const mh::ui::PersonalOptionsConfiguration &personal) {
  const auto human = std::find_if(
      league.players.begin(), league.players.end(),
      [](const mh::content::LeaguePlayer &player) { return player.human; });
  if (human == league.players.end()) {
    throw std::runtime_error("league template has no human player");
  }
  human->name = personal.player_name;
  human->color = {personal.player_colour.red, personal.player_colour.green,
                  personal.player_colour.blue};
  if (!one_player.car.empty()) {
    human->division_cars[human->division] = one_player.car;
  }
}

std::vector<mh::ui::LeagueOverviewPresentation>
load_league_overview_presentations(
    const std::filesystem::path &content_root,
    const std::filesystem::path &user_root,
    const mh::ui::OnePlayerPanelValues &profile) {
  const auto default_path =
      std::filesystem::absolute(content_root / "League" / "Default.LGF")
          .lexically_normal();
  std::vector<std::filesystem::path> paths;
  for (const auto &directory :
       {content_root / "League", user_root / "Leagues"}) {
    if (!std::filesystem::is_directory(directory)) {
      continue;
    }
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
      if (entry.is_regular_file() &&
          ascii_equal_case_insensitive(entry.path().extension().string(),
                                       ".LGF") &&
          (directory != content_root / "League" ||
           ascii_equal_case_insensitive(entry.path().filename().string(),
                                        "Default.LGF"))) {
        paths.push_back(entry.path());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  std::vector<mh::ui::LeagueOverviewPresentation> result;
  for (const auto &path : paths) {
    auto league = mh::content::read_league(path);
    if (std::filesystem::absolute(path).lexically_normal() == default_path) {
      reset_league_progress(league);
    }
    const auto player =
        std::find_if(league.players.begin(), league.players.end(),
                     [](const mh::content::LeaguePlayer &candidate) {
                       return candidate.human;
                     });
    if (player == league.players.end()) {
      throw std::runtime_error("league has no human player: " + path.string());
    }
    const auto division = std::min<std::size_t>(
        player->division, player->division_cars.size() - 1U);
    result.push_back({league.name, player->name, "-", profile.track,
                      player->division, player->score, league.races_done,
                      league.creation_date, player->division_cars[division],
                      path.string(), {}});
  }
  const auto default_overview = std::find_if(
      result.begin(), result.end(), [&default_path](const auto &overview) {
        return std::filesystem::absolute(overview.source_path)
                   .lexically_normal() == default_path;
      });
  if (default_overview != result.end()) {
    const auto shadowed = std::any_of(
        result.begin(), result.end(), [&default_overview](const auto &overview) {
          return &overview != &*default_overview &&
                 ascii_equal_case_insensitive(
                     overview.league_name, default_overview->league_name) &&
                 overview.creation_date == default_overview->creation_date;
        });
    if (shadowed) {
      result.erase(default_overview);
    }
  }
  if (result.empty()) {
    throw std::runtime_error("no League/*.LGF files were found");
  }
  return result;
}

std::string current_league_date() {
  const auto now = std::time(nullptr);
  std::tm local{};
  if (localtime_s(&local, &now) != 0) {
    throw std::runtime_error("could not determine the local league date");
  }
  std::ostringstream result;
  result << std::put_time(&local, "%Y%m%d");
  return result.str();
}

std::string league_file_stem(const std::string_view name) {
  std::string result;
  result.reserve(name.size());
  for (const auto character : name) {
    const auto byte = static_cast<unsigned char>(character);
    if (std::isalnum(byte) != 0) {
      result.push_back(character);
    } else if (!result.empty() && result.back() != '_') {
      result.push_back('_');
    }
  }
  while (!result.empty() && result.back() == '_') {
    result.pop_back();
  }
  if (result.empty()) {
    result = "League";
  }
  return result;
}

std::filesystem::path next_reconstruction_league_path(
    const std::filesystem::path &user_root, const std::string_view name) {
  const auto directory = user_root / "Leagues";
  const auto stem = league_file_stem(name);
  for (std::uint32_t suffix = 0U; suffix <= 9999U; ++suffix) {
    std::ostringstream file_name;
    file_name << stem << std::setw(4) << std::setfill('0') << suffix << ".LGF";
    const auto candidate = directory / file_name.str();
    if (!std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  throw std::runtime_error(
      "all reconstruction league filename slots are in use");
}

mh::ui::LeagueOverviewPresentation create_reconstruction_league(
    const std::filesystem::path &content_root,
    const std::filesystem::path &user_root,
    const mh::ui::LeagueOverviewPresentation &draft,
    const mh::ui::OnePlayerPanelValues &one_player,
    const mh::ui::PersonalOptionsConfiguration &personal) {
  auto league =
      mh::content::read_league(content_root / "League" / "Default.LGF");
  reset_league_progress(league);
  mh::content::set_league_human_starting_division(league, draft.division);
  apply_player_to_league(league, one_player, personal);
  league.name = draft.league_name;
  league.creation_date = current_league_date();

  const auto output = next_reconstruction_league_path(user_root, league.name);
  mh::content::write_league(output, league);
  const auto human = std::find_if(
      league.players.begin(), league.players.end(),
      [](const mh::content::LeaguePlayer &player) { return player.human; });
  if (human == league.players.end()) {
    throw std::runtime_error("created league has no human player");
  }
  const auto division =
      std::min<std::size_t>(human->division, human->division_cars.size() - 1U);
  return {
      league.name,       human->name,          "-",
      one_player.track,  human->division,      human->score,
      league.races_done, league.creation_date, human->division_cars[division],
      output.string(),   {}};
}

bool is_reconstruction_owned_league(const std::filesystem::path &user_root,
                                    const std::filesystem::path &candidate) {
  const auto owned_directory = user_root / "Leagues";
  std::error_code error;
  const auto same_parent = std::filesystem::equivalent(
      std::filesystem::absolute(candidate).lexically_normal().parent_path(),
      std::filesystem::absolute(owned_directory).lexically_normal(), error);
  return !error && same_parent;
}

void archive_completed_league(const std::filesystem::path &user_root,
                              const std::filesystem::path &source) {
  const auto directory = user_root / "Leagues" / "Archive";
  std::filesystem::create_directories(directory);
  auto destination = directory / source.filename();
  for (std::uint32_t suffix = 1U; std::filesystem::exists(destination);
       ++suffix) {
    destination =
        directory / (source.stem().string() + "-" + std::to_string(suffix) +
                     source.extension().string());
  }
  std::error_code error;
  std::filesystem::rename(source, destination, error);
  if (error) {
    throw std::system_error(error, "could not archive completed League");
  }
  const auto backup = mh::common::atomic_backup_path(source);
  if (std::filesystem::is_regular_file(backup)) {
    auto archived_backup = destination;
    archived_backup += ".bak";
    std::filesystem::rename(backup, archived_backup, error);
    if (error) {
      throw std::system_error(error,
                              "could not archive completed League backup");
    }
  }
}

std::filesystem::path
hidden_ghost_demo_path(const std::filesystem::path &reference_root) {
  return reference_root / "MotorHead-Reconstruction-Hidden-Demos.txt";
}

std::vector<std::string>
load_hidden_ghost_demos(const std::filesystem::path &reference_root) {
  std::vector<std::string> result;
  std::ifstream stream(hidden_ghost_demo_path(reference_root));
  for (std::string line; std::getline(stream, line);) {
    line = trim_copy(std::move(line));
    if (!line.empty() && !line.starts_with('#')) {
      result.push_back(std::move(line));
    }
  }
  return result;
}

void save_hidden_ghost_demos(const std::filesystem::path &reference_root,
                             const std::vector<std::string> &files) {
  const auto path = hidden_ghost_demo_path(reference_root);
  std::ostringstream stream;
  stream << "# Reconstruction-owned Ghost Delete list.\n"
         << "# Original Demos/*.MDE files are never modified.\n";
  for (const auto &file : files) {
    stream << file << '\n';
  }
  mh::common::write_atomic_text(path, stream.str(), [](const auto &candidate) {
    std::ifstream input(candidate);
    std::string header;
    return static_cast<bool>(std::getline(input, header)) &&
           header == "# Reconstruction-owned Ghost Delete list.";
  });
  if (load_hidden_ghost_demos(reference_root) != files) {
    throw std::runtime_error("hidden demo list did not persist: " +
                             path.string());
  }
}

std::filesystem::path
hidden_leagues_path(const std::filesystem::path &reference_root) {
  return reference_root / "MotorHead-Reconstruction-Hidden-Leagues.txt";
}

std::vector<std::string>
load_hidden_leagues(const std::filesystem::path &reference_root) {
  std::vector<std::string> result;
  std::ifstream stream(hidden_leagues_path(reference_root));
  for (std::string line; std::getline(stream, line);) {
    line = trim_copy(std::move(line));
    if (!line.empty() && !line.starts_with('#')) {
      result.push_back(std::move(line));
    }
  }
  return result;
}

void save_hidden_leagues(const std::filesystem::path &reference_root,
                         const std::vector<std::string> &leagues) {
  const auto path = hidden_leagues_path(reference_root);
  std::ostringstream stream;
  stream << "# Reconstruction-owned League Delete list.\n"
         << "# Original League/*.LGF files are never modified.\n";
  for (const auto &league : leagues) {
    stream << league << '\n';
  }
  mh::common::write_atomic_text(path, stream.str(), [](const auto &candidate) {
    std::ifstream input(candidate);
    std::string header;
    return static_cast<bool>(std::getline(input, header)) &&
           header == "# Reconstruction-owned League Delete list.";
  });
  if (load_hidden_leagues(reference_root) != leagues) {
    throw std::runtime_error("hidden League list did not persist: " +
                             path.string());
  }
}

struct StoredRankingEntry {
  std::uint32_t track_index = 0U;
  std::uint32_t laps = 5U;
  mh::ui::DifficultyChoice difficulty = mh::ui::DifficultyChoice::medium;
  bool mirror = false;
  mh::ui::RankingEntry result;
};

std::filesystem::path
rankings_path(const std::filesystem::path &reference_root) {
  return reference_root / "HiScores.tsv";
}

std::string ranking_text_field(std::string value,
                               const std::size_t maximum_bytes) {
  std::replace_if(
      value.begin(), value.end(),
      [](const char character) {
        return character == '\t' || character == '\r' || character == '\n';
      },
      ' ');
  if (value.size() > maximum_bytes) {
    value.resize(maximum_bytes);
  }
  return value;
}

std::vector<StoredRankingEntry>
load_rankings_file(const std::filesystem::path &path) {
  std::vector<StoredRankingEntry> result;
  std::ifstream stream(path);
  std::string line;
  if (!std::getline(stream, line)) {
    return result;
  }
  if (trim_copy(std::move(line)) != "# Motorhead reconstruction rankings v1") {
    return result;
  }
  while (std::getline(stream, line)) {
    if (line.empty() || line.starts_with('#')) {
      continue;
    }
    std::array<std::string, 8U> fields;
    std::istringstream row(line);
    bool complete = true;
    for (std::size_t index = 0U; index < fields.size(); ++index) {
      if (!std::getline(row, fields[index], '\t')) {
        complete = false;
        break;
      }
    }
    if (!complete) {
      continue;
    }
    try {
      const auto difficulty = std::stoul(fields[2U]);
      if (difficulty > static_cast<unsigned>(mh::ui::DifficultyChoice::hard)) {
        continue;
      }
      StoredRankingEntry entry;
      entry.track_index = static_cast<std::uint32_t>(std::stoul(fields[0U]));
      entry.laps = static_cast<std::uint32_t>(std::stoul(fields[1U]));
      entry.difficulty = static_cast<mh::ui::DifficultyChoice>(difficulty);
      entry.mirror = std::stoul(fields[3U]) != 0U;
      entry.result.player_name = ranking_text_field(fields[4U], 23U);
      entry.result.vehicle_name = ranking_text_field(fields[5U], 19U);
      entry.result.best_lap_ms =
          static_cast<std::uint32_t>(std::stoul(fields[6U]));
      entry.result.total_time_ms =
          static_cast<std::uint32_t>(std::stoul(fields[7U]));
      if (!entry.result.player_name.empty() &&
          !entry.result.vehicle_name.empty() &&
          entry.result.best_lap_ms != 0U && entry.result.total_time_ms != 0U) {
        result.push_back(std::move(entry));
      }
    } catch (...) {
      // A partial/corrupt sidecar row must not make the original front end
      // unusable. Valid preceding and following rows remain available.
    }
  }
  return result;
}

std::vector<StoredRankingEntry>
load_rankings(const std::filesystem::path &reference_root) {
  return load_rankings_file(rankings_path(reference_root));
}

void save_rankings(const std::filesystem::path &reference_root,
                   const std::vector<StoredRankingEntry> &entries) {
  const auto path = rankings_path(reference_root);
  std::ostringstream stream;
  stream << "# Motorhead reconstruction rankings v1\n"
         << "# track\tlaps\tdifficulty\tmirror\tplayer\tvehicle\tbest_ms"
            "\ttotal_ms\n";
  for (const auto &entry : entries) {
    stream << entry.track_index << '\t' << entry.laps << '\t'
           << static_cast<unsigned>(entry.difficulty) << '\t'
           << (entry.mirror ? 1 : 0) << '\t'
           << ranking_text_field(entry.result.player_name, 23U) << '\t'
           << ranking_text_field(entry.result.vehicle_name, 19U) << '\t'
           << entry.result.best_lap_ms << '\t' << entry.result.total_time_ms
           << '\n';
  }
  mh::common::write_atomic_text(path, stream.str(), [](const auto &candidate) {
    const auto loaded = load_rankings_file(candidate);
    std::ifstream input(candidate);
    std::string header;
    if (!std::getline(input, header) ||
        header != "# Motorhead reconstruction rankings v1") {
      return false;
    }
    std::size_t rows = 0U;
    for (std::string line; std::getline(input, line);) {
      if (!line.empty() && !line.starts_with('#')) {
        ++rows;
      }
    }
    return input.eof() && rows == loaded.size();
  });
  const auto loaded = load_rankings(reference_root);
  const auto same =
      loaded.size() == entries.size() &&
      std::equal(
          loaded.begin(), loaded.end(), entries.begin(),
          [](const auto &left, const auto &right) {
            return left.track_index == right.track_index &&
                   left.laps == right.laps &&
                   left.difficulty == right.difficulty &&
                   left.mirror == right.mirror &&
                   left.result.player_name == right.result.player_name &&
                   left.result.vehicle_name == right.result.vehicle_name &&
                   left.result.best_lap_ms == right.result.best_lap_ms &&
                   left.result.total_time_ms == right.result.total_time_ms;
          });
  if (!same) {
    throw std::runtime_error("rankings did not persist: " + path.string());
  }
}

std::vector<std::string>
load_ghost_demo_files(const std::filesystem::path &reference_root,
                      const std::vector<std::string> &hidden_files) {
  std::vector<std::string> result;
  const auto directory = reference_root / "Demos";
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file() &&
        ascii_equal_case_insensitive(entry.path().extension().string(),
                                     ".MDE") &&
        std::none_of(hidden_files.begin(), hidden_files.end(),
                     [&entry](const std::string &hidden) {
                       return ascii_equal_case_insensitive(
                           hidden, entry.path().filename().string());
                     })) {
      result.push_back(entry.path().filename().string());
    }
  }
  std::sort(result.begin(), result.end(),
            [](const std::string &left, const std::string &right) {
              const auto lower = [](std::string value) {
                std::transform(value.begin(), value.end(), value.begin(),
                               [](const unsigned char character) {
                                 return static_cast<char>(
                                     std::tolower(character));
                               });
                return value;
              };
              return lower(left) < lower(right);
            });
  return result;
}

struct RaceSetupTrackAsset {
  std::string name;
  mh::content::TgaImage preview;
  mh::content::LobData line_object;
  double line_object_scale = 0.0;
  std::filesystem::path definition_path;
  mh::content::TrackDefinition definition;
  std::uint32_t source_number = 0U;
};

struct CarSetupAsset {
  std::string name;
  mh::content::TgaImage preview;
  mh::content::LobData line_object;
  double line_object_scale = 0.0;
  std::filesystem::path definition_path;
  mh::content::CarPerformanceLevels performance;
  mh::content::CarMenuPerformance menu_performance;
  std::uint32_t division = 0U;
  std::uint32_t source_number = 0U;
};

std::vector<CarSetupAsset>
load_car_setup_assets(const std::filesystem::path &reference_root) {
  std::vector<CarSetupAsset> result;
  const auto game_directory = reference_root / "Game";
  // Car99 is reserved for the optional Volvo; add-on slots remain selectable.
  for (std::uint32_t number = 1U; number < 99U; ++number) {
    const auto definition_path =
        game_directory / ("car" + (number < 10U ? std::string("0") : "") +
                          std::to_string(number) + ".car");
    if (!std::filesystem::is_regular_file(definition_path)) {
      continue;
    }
    const auto definition = mh::content::read_car(definition_path);
    if (mh::content::is_s40_car(definition)) {
      continue;
    }
    if (definition.name.empty() || definition.texture_path.empty() ||
        !definition.performance_levels.has_value() ||
        !definition.menu_performance.has_value()) {
      throw std::runtime_error("car definition lacks its menu data: " +
                               definition_path.string());
    }
    const auto preview_path = reference_root /
                              std::filesystem::path(definition.texture_path) /
                              "data" / "_menuimg.TGA";
    const auto line_object_path =
        reference_root / std::filesystem::path(definition.line_object_path);
    result.push_back({definition.name, mh::content::read_tga(preview_path),
                      mh::content::read_lob(line_object_path), 0.0,
                      definition_path,
                      *definition.performance_levels,
                      *definition.menu_performance, definition.division,
                      number});
  }
  if (result.empty()) {
    throw std::runtime_error("no carNN.car definitions were found");
  }
  for (auto &asset : result) {
    asset.line_object_scale =
        mh::ui::front_end_menu_line_object_scale(asset.line_object, 160.0);
  }
  return result;
}

mh::content::TgaImage make_car_setup_preview(
    const mh::content::TgaImage &source) {
  constexpr std::uint32_t target_width = 324U;
  constexpr std::uint32_t target_height = 270U;
  if (source.width == 0U || source.height == 0U ||
      source.rgba.size() !=
          static_cast<std::size_t>(source.width) * source.height * 4U) {
    throw std::runtime_error("optional car preview image is malformed");
  }

  auto result = source;
  result.width = static_cast<std::uint16_t>(target_width);
  result.height = static_cast<std::uint16_t>(target_height);
  result.has_palette = false;
  result.palette_indices.clear();
  result.rgba.assign(static_cast<std::size_t>(target_width) * target_height *
                         4U,
                     0U);
  for (std::size_t alpha = 3U; alpha < result.rgba.size(); alpha += 4U) {
    result.rgba[alpha] = 255U;
  }

  // Fill the oval without letterboxing; crop the excess around the centre.
  const auto scale = std::max(static_cast<double>(target_width) / source.width,
                              static_cast<double>(target_height) /
                                  source.height);
  const auto scaled_width = std::max<std::uint32_t>(
      1U, static_cast<std::uint32_t>(std::lround(source.width * scale)));
  const auto scaled_height = std::max<std::uint32_t>(
      1U, static_cast<std::uint32_t>(std::lround(source.height * scale)));
  const auto crop_x = (scaled_width - target_width) / 2U;
  const auto crop_y = (scaled_height - target_height) / 2U;
  for (std::uint32_t y = 0U; y < target_height; ++y) {
    const auto source_y = std::min<std::uint32_t>(
        static_cast<std::uint32_t>((static_cast<std::uint64_t>(y + crop_y) *
                                    source.height) /
                                   scaled_height),
        source.height - 1U);
    for (std::uint32_t x = 0U; x < target_width; ++x) {
      const auto source_x = std::min<std::uint32_t>(
          static_cast<std::uint32_t>((static_cast<std::uint64_t>(x + crop_x) *
                                      source.width) /
                                     scaled_width),
          source.width - 1U);
      const auto source_pixel =
          (static_cast<std::size_t>(source_y) * source.width + source_x) * 4U;
      const auto destination_pixel =
          (static_cast<std::size_t>(y) * target_width + x) * 4U;
      std::copy_n(source.rgba.begin() +
                      static_cast<std::ptrdiff_t>(source_pixel),
                  4U, result.rgba.begin() +
                          static_cast<std::ptrdiff_t>(destination_pixel));
    }
  }
  return result;
}

std::optional<std::filesystem::path>
s40_definition_path(const std::filesystem::path &reference_root) {
  // Older installs used Car15. Never mistake an add-on in that slot for S40.
  for (const auto filename : {"car99.car", "car15.car"}) {
    const auto path = reference_root / "Game" / filename;
    if (std::filesystem::is_regular_file(path) &&
        mh::content::is_s40_car(mh::content::read_car(path))) {
      return path;
    }
  }
  return std::nullopt;
}

std::optional<CarSetupAsset>
load_s40_car_setup_asset(const std::filesystem::path &reference_root) {
  const auto installed = s40_definition_path(reference_root);
  if (!installed.has_value()) {
    return std::nullopt;
  }
  const auto definition_path = *installed;
  const auto definition = mh::content::read_car(definition_path);
  const auto preview_path = reference_root /
                            std::filesystem::path(definition.texture_path) /
                            "Data" / "_menuimg.TGA";
  if (!std::filesystem::is_regular_file(preview_path)) {
    throw std::runtime_error(
        "installed S40 Racing content is missing its menu or car definition");
  }
  if (definition.name.empty() || !definition.performance_levels.has_value() ||
      !definition.menu_performance.has_value() ||
      definition.model_base.empty()) {
    throw std::runtime_error("S40 Racing car definition lacks its menu data");
  }
  CarSetupAsset result{"Volvo S40",
                       make_car_setup_preview(
                           mh::content::read_tga(preview_path)),
                       {},
                       0.0,
                       definition_path,
                       *definition.performance_levels,
                       *definition.menu_performance,
                       definition.division,
                       0U};
  return result;
}

struct HornSetupAsset {
  std::string name;
  std::filesystem::path sample_path;
};

std::string configuration_file_value(const std::filesystem::path &path,
                                     const std::string_view key) {
  std::ifstream stream(path);
  std::string line;
  while (std::getline(stream, line)) {
    if (const auto comment = line.find("//"); comment != std::string::npos) {
      line.erase(comment);
    }
    line = trim_copy(std::move(line));
    if (!line.starts_with(key)) {
      continue;
    }
    const auto boundary = key.size();
    if (line.size() == boundary ||
        std::isspace(static_cast<unsigned char>(line[boundary])) != 0) {
      return trim_copy(line.substr(boundary));
    }
  }
  return {};
}

std::vector<HornSetupAsset>
load_horn_setup_assets(const std::filesystem::path &reference_root) {
  const auto directory = reference_root / "Sounds" / "CarHorns";
  std::vector<std::filesystem::path> configurations;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (entry.is_regular_file() &&
        ascii_equal_case_insensitive(entry.path().extension().string(),
                                     ".chf")) {
      configurations.push_back(entry.path());
    }
  }
  std::sort(configurations.begin(), configurations.end(),
            [](const auto &left, const auto &right) {
              return left.filename().string() < right.filename().string();
            });
  std::vector<HornSetupAsset> result;
  for (const auto &path : configurations) {
    const auto name = configuration_file_value(path, "Name");
    const auto sample = configuration_file_value(path, "SampleName");
    if (name.empty() || sample.empty()) {
      continue;
    }
    result.push_back(
        {name, reference_root / "Sounds" / std::filesystem::path(sample)});
  }
  if (result.empty()) {
    throw std::runtime_error("no valid CarHorns/*.chf files were found");
  }
  return result;
}

std::vector<RaceSetupTrackAsset>
load_race_setup_tracks(const std::filesystem::path &reference_root) {
  // The retail track selector owns one authored menu line object for each of
  // the eight physical circuits.  Reverse definitions reuse the same object.
  // The association follows the TrackMapName identities in TrackN.trk; it is
  // deliberately kept data-facing instead of relying on the display name.
  static constexpr std::array<std::pair<std::string_view, std::string_view>, 8U>
      menu_line_objects{{
          {"waste", "BWASTE.LOB"},
          {"pluto", "bana.lob"},
          {"okkcity", "BCITY.LOB"},
          {"okksea", "BSEA.LOB"},
          {"volvo3", "BMAX.LOB"},
          {"grekpat", "BGREK.LOB"},
          {"u96", "BU96.LOB"},
          {"gold", "BTRIO.LOB"},
      }};
  std::vector<RaceSetupTrackAsset> result;
  const auto game_directory = reference_root / "Game";
  for (std::uint32_t number = 1U;; ++number) {
    const auto definition_path =
        game_directory / ("Track" + std::to_string(number) + ".trk");
    if (!std::filesystem::is_regular_file(definition_path)) {
      break;
    }
    auto definition = mh::content::read_track_definition(definition_path);
    if (definition.name.empty() || definition.base_path.empty()) {
      throw std::runtime_error("track definition lacks its menu identity: " +
                               definition_path.string());
    }
    const auto preview_path = reference_root /
                              std::filesystem::path(definition.base_path) /
                              "Data" / "_menuimg.TGA";
    const auto map_reference = std::find_if(
        definition.references.begin(), definition.references.end(),
        [](const mh::content::TrackReference &reference) {
          return ascii_equal_case_insensitive(reference.field, "trackmapname");
        });
    if (map_reference == definition.references.end() ||
        map_reference->value.empty()) {
      throw std::runtime_error("track definition lacks its menu map: " +
                               definition_path.string());
    }
    auto map_identity =
        std::filesystem::path(map_reference->value).filename().string();
    std::transform(map_identity.begin(), map_identity.end(),
                   map_identity.begin(), [](const unsigned char character) {
                     return static_cast<char>(std::tolower(character));
                   });
    const auto line_object =
        std::find_if(menu_line_objects.begin(), menu_line_objects.end(),
                     [&map_identity](const auto &entry) {
                       return entry.first == map_identity;
                     });
    if (line_object == menu_line_objects.end()) {
      throw std::runtime_error("track definition has no retail menu line "
                               "object mapping: " +
                               definition_path.string());
    }
    const auto line_object_path = reference_root / "Data" / "LINEOBJ" /
                                  std::filesystem::path(line_object->second);
    result.push_back({definition.name, mh::content::read_tga(preview_path),
                      mh::content::read_lob(line_object_path), 0.0,
                      definition_path, std::move(definition), number});
  }
  if (result.empty()) {
    throw std::runtime_error("no TrackN.trk definitions were found");
  }
  for (auto &asset : result) {
    asset.line_object_scale =
        mh::ui::front_end_menu_line_object_scale(asset.line_object, 170.0);
  }
  return result;
}

template <typename Asset>
std::vector<Asset> reorder_assets(std::vector<Asset> assets,
                                  const std::vector<std::size_t> &order) {
  if (assets.size() != order.size()) {
    throw std::runtime_error("retail front-end catalog order is incomplete");
  }
  std::vector<Asset> result;
  result.reserve(assets.size());
  for (const auto source_index : order) {
    if (source_index >= assets.size()) {
      throw std::runtime_error("retail front-end catalog index is invalid");
    }
    result.push_back(std::move(assets[source_index]));
  }
  return result;
}

bool original_all_unlock_identity(
    const mh::ui::PersonalOptionsConfiguration &personal) {
  // p3.1 RVA 0x0007f2ec decodes and compares this separately from ordinary
  // FLG-backed progression. Keep it an identity bypass, never a clean default.
  return ascii_equal_case_insensitive(personal.player_name, "r pettersson") &&
         ascii_equal_case_insensitive(personal.team_name, "Swe");
}

bool s40_racing_identity(
    const mh::ui::PersonalOptionsConfiguration &personal) {
  return ascii_equal_case_insensitive(personal.player_name, "Bonnier") &&
         ascii_equal_case_insensitive(personal.team_name, "Swe");
}

bool s40_racing_progression_reward(
    const std::vector<mh::content::FrontEndUnlockTrack> &tracks,
    const std::vector<mh::content::FrontEndUnlockCar> &cars) {
  return mh::content::s40_progression_reward(tracks, cars);
}

std::vector<RaceSetupTrackAsset> s40_racing_track_assets(
    const std::vector<RaceSetupTrackAsset> &retail_tracks) {
  static constexpr std::array<std::string_view, 4U> names{
      "NolbyHills", "NolbyHillsR", "OkkunSpeedway", "OkkunSpeedwayR"};
  static constexpr std::array<std::string_view, 4U> display_names{
      "Nolby Hills", "Nolby Hills Reverse", "Okkun Speedway",
      "Okkun Speedway Reverse"};
  std::vector<RaceSetupTrackAsset> result;
  result.reserve(names.size());
  for (std::size_t index = 0U; index < names.size(); ++index) {
    const auto name = names[index];
    const auto found =
        std::find_if(retail_tracks.begin(), retail_tracks.end(),
                     [name](const RaceSetupTrackAsset &track) {
                       return ascii_equal_case_insensitive(track.name, name);
                     });
    if (found == retail_tracks.end()) {
      throw std::runtime_error("S40 Racing requires the Motorhead track " +
                               std::string(name));
    }
    result.push_back(*found);
    result.back().name = display_names[index];
  }
  return result;
}

bool s40_racing_uses_special_assets(const mh::ui::FrontEndScreen screen,
                                    const mh::ui::RaceSetupMode mode) {
  switch (screen) {
  case mh::ui::FrontEndScreen::one_player:
    return true;
  case mh::ui::FrontEndScreen::race_setup:
  case mh::ui::FrontEndScreen::car_setup:
    return mode != mh::ui::RaceSetupMode::league_race &&
           mode != mh::ui::RaceSetupMode::ghost_race;
  default:
    return false;
  }
}

bool original_track_flag_available(const std::filesystem::path &user_root,
                                   const RaceSetupTrackAsset &track) {
  const auto relative = std::filesystem::path(track.definition.base_path) /
                        "Data" / (track.name + ".FLG");
  return mh::content::original_track_unlock_flag_valid(user_root / relative,
                                                       track.name);
}

std::size_t
normal_track_asset_index(const std::vector<RaceSetupTrackAsset> &tracks,
                         const std::size_t normal_source_index) {
  const auto source_number = normal_source_index + 1U;
  const auto found = std::find_if(tracks.begin(), tracks.end(),
                                  [source_number](const auto &track) {
                                    return track.source_number == source_number;
                                  });
  if (found == tracks.end()) {
    throw std::runtime_error("normal retail track definition is unavailable");
  }
  return static_cast<std::size_t>(std::distance(tracks.begin(), found));
}

constexpr std::size_t retail_normal_track_count = 8U;
constexpr std::array<std::size_t, retail_normal_track_count>
    retail_sound_track_order{0U, 1U, 2U, 3U, 6U, 5U, 4U, 7U};

std::size_t sound_options_track_count(
    const std::vector<RaceSetupTrackAsset> &tracks) noexcept {
  // Track1..Track8 are the normal layouts. Track9..Track16 are their reverse
  // counterparts and share the corresponding normal layout's CD assignment.
  return static_cast<std::size_t>(
      std::count_if(tracks.begin(), tracks.end(), [](const auto &track) {
        return track.source_number >= 1U &&
               track.source_number <= retail_normal_track_count;
      }));
}

mh::ui::RankingsPresentation
make_rankings_presentation(const std::vector<StoredRankingEntry> &history,
                           const mh::ui::RankingsConfiguration &configuration,
                           const std::vector<RaceSetupTrackAsset> &tracks) {
  mh::ui::RankingsPresentation result;
  const auto normal_track_count =
      std::min<std::size_t>(tracks.size(), retail_normal_track_count);
  const auto track_index =
      std::min<std::size_t>(configuration.track_index,
                            std::max<std::size_t>(normal_track_count, 1U) - 1U);
  if (track_index < normal_track_count) {
    result.track_name =
        tracks[normal_track_asset_index(tracks, track_index)].name;
  }
  if (configuration.type == mh::ui::RankingsType::league) {
    return result;
  }
  constexpr std::array<std::uint32_t, 6U> lap_values{1U, 3U, 5U, 10U, 15U, 25U};
  const auto selected_laps = lap_values[std::min<std::size_t>(
      configuration.laps_index, lap_values.size() - 1U)];
  for (const auto &entry : history) {
    if (entry.track_index != track_index ||
        entry.mirror != configuration.mirror) {
      continue;
    }
    if (configuration.type != mh::ui::RankingsType::best_laptime &&
        entry.laps != selected_laps) {
      continue;
    }
    if (configuration.type == mh::ui::RankingsType::single_race &&
        entry.difficulty != configuration.difficulty) {
      continue;
    }
    result.entries.push_back(entry.result);
  }
  const auto best_lap =
      configuration.type == mh::ui::RankingsType::best_laptime;
  std::stable_sort(result.entries.begin(), result.entries.end(),
                   [best_lap](const mh::ui::RankingEntry &left,
                              const mh::ui::RankingEntry &right) {
                     const auto left_primary =
                         best_lap ? left.best_lap_ms : left.total_time_ms;
                     const auto right_primary =
                         best_lap ? right.best_lap_ms : right.total_time_ms;
                     if (left_primary != right_primary) {
                       return left_primary < right_primary;
                     }
                     const auto left_secondary =
                         best_lap ? left.total_time_ms : left.best_lap_ms;
                     const auto right_secondary =
                         best_lap ? right.total_time_ms : right.best_lap_ms;
                     return left_secondary < right_secondary;
                   });
  if (result.entries.size() > 10U) {
    result.entries.resize(10U);
  }
  return result;
}

std::size_t
sound_options_track_index(const std::vector<RaceSetupTrackAsset> &tracks,
                          const std::size_t row,
                          const std::size_t normal_count) {
  if (row >= normal_count) {
    throw std::runtime_error("Sound Options track row is out of range");
  }
  const auto ordered = retail_sound_track_order[row];
  return normal_track_asset_index(tracks,
                                  ordered < normal_count ? ordered : row);
}

std::size_t sound_assignment_index(const std::uint32_t track_source_number,
                                   const std::size_t assignment_count,
                                   const std::size_t fixed_count) {
  if (fixed_count > assignment_count) {
    throw std::runtime_error("invalid fixed Sound Options assignment count");
  }
  const auto race_assignment_count = assignment_count - fixed_count;
  if (race_assignment_count == 0U) {
    throw std::runtime_error(
        "no Sound Options track assignments are available");
  }
  const auto normal_track_index =
      (std::max<std::uint32_t>(track_source_number, 1U) - 1U) %
      race_assignment_count;
  for (std::size_t row = 0U; row < race_assignment_count; ++row) {
    const auto ordered = retail_sound_track_order[row];
    if ((ordered < race_assignment_count ? ordered : row) ==
        normal_track_index) {
      return fixed_count + row;
    }
  }
  throw std::runtime_error("race track has no Sound Options assignment");
}

float parse_float_or(const std::string &value, const float fallback) {
  if (value.empty()) {
    return fallback;
  }
  try {
    const auto parsed = std::stof(value);
    return std::isfinite(parsed) ? parsed : fallback;
  } catch (...) {
    return fallback;
  }
}

mh::ui::ControlOptionsConfiguration load_control_options_configuration(
    const std::filesystem::path &reference_root) {
  constexpr std::array<std::string_view, 5U> files{
      "custom.clo", "Joystick.clo", "Keyboard.clo", "mouse.clo", "Wheel.clo"};
  constexpr std::array<std::string_view, 10U> binding_keys{
      "TurnLeft",      "TurnRight", "Accelerate", "Brake",    "ShiftGearUp",
      "ShiftGearDown", "Horn",      "HandBrake",  "RearView", "GameMenu"};
  constexpr std::array<std::string_view, 4U> fixed_race_binding_keys{
      "InCarView", "OutCarView", "CameraView", "CyclePlayers"};
  mh::ui::ControlOptionsConfiguration result;
  result.profiles.clear();
  result.profiles.reserve(files.size());
  for (const auto file : files) {
    const auto path = mh::ui::control_configuration_path(reference_root, file);
    mh::ui::ControlProfileConfiguration profile;
    if (const auto name = configuration_file_value(path, "LayoutName");
        !name.empty()) {
      profile.name = name;
    }
    profile.mouse_speeds[0U] =
        parse_float_or(configuration_file_value(path, "MouseSpeedH"), 1.0F);
    profile.mouse_speeds[1U] =
        parse_float_or(configuration_file_value(path, "MouseSpeedV"), 1.0F);
    profile.mouse_speeds[2U] =
        parse_float_or(configuration_file_value(path, "MouseSpeedZ"), 1.0F);
    profile.force_feedback = ascii_equal_case_insensitive(
        configuration_file_value(path, "ForceFeedback"), "On");
    for (std::size_t index = 0U; index < binding_keys.size(); ++index) {
      if (const auto binding =
              configuration_file_value(path, binding_keys[index]);
          !binding.empty()) {
        profile.bindings[index] = binding;
      }
    }
    for (std::size_t index = 0U; index < fixed_race_binding_keys.size();
         ++index) {
      if (const auto binding =
              configuration_file_value(path, fixed_race_binding_keys[index]);
          !binding.empty()) {
        profile.fixed_race_bindings[index] = binding;
      }
    }
    result.profiles.push_back(std::move(profile));
  }

  const auto selected_name = configuration_file_value(
      mh::ui::motorhead_configuration_path(reference_root), "ControlName");
  const auto selected = std::find_if(
      result.profiles.begin(), result.profiles.end(),
      [&selected_name](const mh::ui::ControlProfileConfiguration &profile) {
        return ascii_equal_case_insensitive(profile.name, selected_name);
      });
  result.profile_index = selected == result.profiles.end()
                             ? 0U
                             : static_cast<std::uint32_t>(std::distance(
                                   result.profiles.begin(), selected));

  return result;
}

void save_control_options_configuration(
    const std::filesystem::path &reference_root,
    const mh::ui::ControlOptionsConfiguration &configuration) {
  constexpr std::array<std::string_view, 5U> files{
      "custom.clo", "Joystick.clo", "Keyboard.clo", "mouse.clo", "Wheel.clo"};
  constexpr std::array<std::string_view, 10U> binding_keys{
      "TurnLeft",      "TurnRight", "Accelerate", "Brake",    "ShiftGearUp",
      "ShiftGearDown", "Horn",      "HandBrake",  "RearView", "GameMenu"};
  constexpr std::array<std::string_view, 3U> speed_keys{
      "MouseSpeedH", "MouseSpeedV", "MouseSpeedZ"};
  if (configuration.profiles.empty()) {
    throw std::runtime_error("Control Options has no profiles to save");
  }
  const auto profile_index = std::min<std::size_t>(
      configuration.profile_index, configuration.profiles.size() - 1U);
  mh::ui::update_motorhead_configuration(
      reference_root,
      {{"ControlName", configuration.profiles[profile_index].name}});
  const auto count = std::min(configuration.profiles.size(), files.size());
  for (std::size_t index = 0U; index < count; ++index) {
    const auto &profile = configuration.profiles[index];
    mh::ui::ConfigurationUpdates updates{
        {"ForceFeedback", profile.force_feedback ? "On" : "Off"}};
    for (std::size_t speed = 0U; speed < profile.mouse_speeds.size(); ++speed) {
      std::ostringstream value;
      value << std::fixed << std::setprecision(6)
            << profile.mouse_speeds[speed];
      updates.emplace_back(speed_keys[speed], value.str());
    }
    for (std::size_t binding = 0U; binding < profile.bindings.size();
         ++binding) {
      updates.emplace_back(binding_keys[binding], profile.bindings[binding]);
    }
    mh::ui::update_configuration_file(
        mh::ui::control_configuration_path(reference_root, files[index]),
        updates);
  }
}

std::string retail_control_key_name(const SDL_Keycode key) {
  switch (key) {
  case SDLK_LEFT:
    return "LeftArrow";
  case SDLK_RIGHT:
    return "RightArrow";
  case SDLK_UP:
    return "UpArrow";
  case SDLK_DOWN:
    return "DownArrow";
  case SDLK_LSHIFT:
  case SDLK_RSHIFT:
    return "Shift";
  case SDLK_LCTRL:
  case SDLK_RCTRL:
    return "Ctrl";
  case SDLK_LALT:
  case SDLK_RALT:
    return "Alt";
  case SDLK_ESCAPE:
    return "Esc";
  case SDLK_BACKSPACE:
    return "Backspace";
  case SDLK_RETURN:
  case SDLK_KP_ENTER:
    return "Return";
  case SDLK_SPACE:
    return "Space";
  default:
    break;
  }
  const auto *name = SDL_GetKeyName(key);
  if (name == nullptr || *name == '\0') {
    return {};
  }
  std::string result(name);
  result.erase(std::remove_if(result.begin(), result.end(),
                              [](const char value) {
                                return std::isspace(static_cast<unsigned char>(
                                           value)) != 0;
                              }),
               result.end());
  return result;
}

mh::ui::GraphicInfoMode
parse_graphic_info_mode(const std::string &value,
                        const mh::ui::GraphicInfoMode fallback) {
  if (ascii_equal_case_insensitive(value, "None")) {
    return mh::ui::GraphicInfoMode::none;
  }
  if (ascii_equal_case_insensitive(value, "Selective")) {
    return mh::ui::GraphicInfoMode::selective;
  }
  if (ascii_equal_case_insensitive(value, "All")) {
    return mh::ui::GraphicInfoMode::all;
  }
  return fallback;
}

mh::ui::GraphicOptionsConfiguration load_graphic_options_configuration(
    const std::filesystem::path &reference_root) {
  const auto source = mh::ui::motorhead_configuration_path(reference_root);
  mh::ui::GraphicOptionsConfiguration result;
  result.renderer_backend = mh::ui::graphic_renderer_backend_from_config(
      configuration_file_value(source, "Renderer"));
  {
    std::istringstream dimensions(
        configuration_file_value(source, "ScreenDimension"));
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    if (dimensions >> width >> height) {
      if (width == 640U && height == 480U) {
        result.screen_size = mh::ui::GraphicScreenSize::size_640x480;
      } else if (width == 800U && height == 600U) {
        result.screen_size = mh::ui::GraphicScreenSize::size_800x600;
      } else if (width == 1024U && height == 768U) {
        result.screen_size = mh::ui::GraphicScreenSize::size_1024x768;
      } else if (width == 1280U && height == 1024U) {
        result.screen_size = mh::ui::GraphicScreenSize::size_1280x1024;
      } else if (width == 1600U && height == 1200U) {
        result.screen_size = mh::ui::GraphicScreenSize::size_1600x1200;
      } else if (width == 960U && height == 540U) {
        result.screen_size = mh::ui::GraphicScreenSize::size_960x540;
      } else if (width == 1280U && height == 720U) {
        result.screen_size = mh::ui::GraphicScreenSize::size_1280x720;
      } else if (width == 1360U && height == 768U) {
        result.screen_size = mh::ui::GraphicScreenSize::size_1360x768;
      } else if (width == 1600U && height == 900U) {
        result.screen_size = mh::ui::GraphicScreenSize::size_1600x900;
      } else if (width == 1920U && height == 1080U) {
        result.screen_size = mh::ui::GraphicScreenSize::size_1920x1080;
      } else {
        result.screen_size = mh::ui::GraphicScreenSize::size_800x600;
      }
    }
  }
  result.aspect_ratio = mh::ui::graphic_screen_aspect_ratio(result.screen_size);
  const auto configured_aspect =
      configuration_file_value(source, "AspectRatio");
  if (!configured_aspect.empty()) {
    result.aspect_ratio =
        mh::ui::graphic_aspect_ratio_from_config(configured_aspect);
    result.screen_size = mh::ui::graphic_screen_size_for_aspect(
        result.aspect_ratio,
        mh::ui::graphic_screen_size_tier(result.screen_size));
  }
  const auto window_mode = configuration_file_value(source, "WindowMode");
  result.window_mode = mh::ui::graphic_window_mode_from_config(window_mode);
  if (window_mode.empty() &&
      ascii_equal_case_insensitive(
          configuration_file_value(source, "Fullscreen"), "On")) {
    // Accept the simple boolean spelling used by early reconstruction builds.
    result.window_mode = mh::ui::GraphicWindowMode::fullscreen;
  }
  result.true_colour = ascii_equal_case_insensitive(
      configuration_file_value(source, "TrueColour"), "On");
  result.triple_buffer = ascii_equal_case_insensitive(
      configuration_file_value(source, "TrippleBuffer"), "On");
  result.trilinear_filtering = ascii_equal_case_insensitive(
      configuration_file_value(source, "TriLinearFiltering"), "On");
  const auto texture = configuration_file_value(source, "TextureFormat");
  result.texture_format =
      texture == "16"   ? mh::ui::GraphicTextureFormat::hi_colour
      : texture == "32" ? mh::ui::GraphicTextureFormat::true_colour
                        : mh::ui::GraphicTextureFormat::indexed_256;
  {
    std::istringstream brightness(
        configuration_file_value(source, "Brightness"));
    float percent = 100.0F;
    if (brightness >> percent) {
      result.brightness = percent / 100.0F;
    }
  }
  result.info_detail = parse_graphic_info_mode(
      configuration_file_value(source, "InfoDetail"), result.info_detail);
  result.info_map = parse_graphic_info_mode(
      configuration_file_value(source, "RoadMap"), result.info_map);
  result.checkpoint_info = parse_graphic_info_mode(
      configuration_file_value(source, "CheckpointInfo"),
      result.checkpoint_info);
  const auto checkpoint_seconds =
      parse_float_or(configuration_file_value(source, "CheckpointDelay"), 4.0F);
  result.checkpoint_display_time_ms = static_cast<std::uint32_t>(
      std::clamp(std::lround(static_cast<double>(checkpoint_seconds) * 1000.0),
                 0L, 10000L));
  const auto detail = configuration_file_value(source, "DetailMode");
  result.detail_mode = ascii_equal_case_insensitive(detail, "Low")
                           ? mh::ui::GraphicDetailMode::low
                       : ascii_equal_case_insensitive(detail, "Medium")
                           ? mh::ui::GraphicDetailMode::medium
                       : ascii_equal_case_insensitive(detail, "Max")
                           ? mh::ui::GraphicDetailMode::maximum
                       : ascii_equal_case_insensitive(detail, "Custom")
                           ? mh::ui::GraphicDetailMode::custom
                           : mh::ui::GraphicDetailMode::high;
  const auto on = [&source](const std::string_view key, const bool fallback) {
    const auto value = configuration_file_value(source, key);
    return value.empty() ? fallback : ascii_equal_case_insensitive(value, "On");
  };
  result.lens_flares = on("LensFlares", result.lens_flares);
  result.sparks = on("Sparks", result.sparks);
  result.smoke = on("Smoke", result.smoke);
  result.halos = on("Halos", result.halos);
  result.shadows = on("Shadow", result.shadows);
  result.skid_marks = on("SkidMarks", result.skid_marks);
  const auto name_plates = configuration_file_value(source, "NamePlates");
  result.name_plates = ascii_equal_case_insensitive(name_plates, "None")
                           ? mh::ui::GraphicNamePlateMode::none
                       : ascii_equal_case_insensitive(name_plates, "Flat")
                           ? mh::ui::GraphicNamePlateMode::flat
                           : mh::ui::GraphicNamePlateMode::transparent;
  result.background = on("SkyImage", result.background);
  result.track_detail =
      ascii_equal_case_insensitive(
          configuration_file_value(source, "TrackDetail"), "Medium")
          ? mh::ui::GraphicTrackDetail::medium
          : mh::ui::GraphicTrackDetail::high;
  const auto car_detail = configuration_file_value(source, "CarDetail");
  result.car_detail = ascii_equal_case_insensitive(car_detail, "Low")
                          ? mh::ui::GraphicCarDetail::low
                      : ascii_equal_case_insensitive(car_detail, "Medium")
                          ? mh::ui::GraphicCarDetail::medium
                          : mh::ui::GraphicCarDetail::high;
  const auto car_shading = configuration_file_value(source, "CarShading");
  result.car_shading = ascii_equal_case_insensitive(car_shading, "Flat")
                           ? mh::ui::GraphicCarShading::flat
                       : ascii_equal_case_insensitive(car_shading, "Gouraud")
                           ? mh::ui::GraphicCarShading::gouraud
                       : ascii_equal_case_insensitive(car_shading, "Glenz")
                           ? mh::ui::GraphicCarShading::glenz
                           : mh::ui::GraphicCarShading::reflection;
  result.view_distance = parse_float_or(
      configuration_file_value(source, "ViewDistance"), result.view_distance);
  result.motion_blur = on("MotionBlur", result.motion_blur);
  result.z_read = on("ZRead", result.z_read);
  result.camera_shake = on("CameraShake", result.camera_shake);
  result.ui_scale = parse_float_or(configuration_file_value(source, "UIScale"),
                                   result.ui_scale);
  if (result.detail_mode != mh::ui::GraphicDetailMode::custom) {
    mh::ui::apply_graphic_detail_preset(result, result.detail_mode);
  }
  return result;
}

void save_graphic_options_configuration(
    const std::filesystem::path &reference_root,
    const mh::ui::GraphicOptionsConfiguration &configuration) {
  const auto screen_dimensions =
      mh::ui::graphic_screen_dimensions(configuration.screen_size);
  const auto dimensions = std::to_string(screen_dimensions[0U]) + " " +
                          std::to_string(screen_dimensions[1U]);
  const auto texture =
      configuration.texture_format == mh::ui::GraphicTextureFormat::hi_colour
          ? 16U
      : configuration.texture_format ==
              mh::ui::GraphicTextureFormat::true_colour
          ? 32U
          : 8U;
  const auto info_name = [](const mh::ui::GraphicInfoMode value) {
    switch (value) {
    case mh::ui::GraphicInfoMode::none:
      return "None";
    case mh::ui::GraphicInfoMode::selective:
      return "Selective";
    case mh::ui::GraphicInfoMode::all:
      return "All";
    }
    return "All";
  };
  const auto detail_name = [](const mh::ui::GraphicDetailMode value) {
    switch (value) {
    case mh::ui::GraphicDetailMode::low:
      return "Low";
    case mh::ui::GraphicDetailMode::medium:
      return "Medium";
    case mh::ui::GraphicDetailMode::high:
      return "High";
    case mh::ui::GraphicDetailMode::maximum:
      return "Max";
    case mh::ui::GraphicDetailMode::custom:
      return "Custom";
    }
    return "High";
  };
  const auto name_plate_name = [](const mh::ui::GraphicNamePlateMode value) {
    switch (value) {
    case mh::ui::GraphicNamePlateMode::none:
      return "None";
    case mh::ui::GraphicNamePlateMode::flat:
      return "Flat";
    case mh::ui::GraphicNamePlateMode::transparent:
      return "Transparent";
    }
    return "Transparent";
  };
  const auto car_detail_name = [](const mh::ui::GraphicCarDetail value) {
    switch (value) {
    case mh::ui::GraphicCarDetail::low:
      return "Low";
    case mh::ui::GraphicCarDetail::medium:
      return "Medium";
    case mh::ui::GraphicCarDetail::high:
      return "High";
    }
    return "High";
  };
  const auto car_shading_name = [](const mh::ui::GraphicCarShading value) {
    switch (value) {
    case mh::ui::GraphicCarShading::flat:
      return "Flat";
    case mh::ui::GraphicCarShading::gouraud:
      return "Gouraud";
    case mh::ui::GraphicCarShading::reflection:
      return "Reflection";
    case mh::ui::GraphicCarShading::glenz:
      return "Glenz";
    }
    return "Reflection";
  };
  const auto on_off = [](const bool value) { return value ? "On" : "Off"; };
  const auto window_mode =
      mh::ui::graphic_window_mode_config_name(configuration.window_mode);
  const auto aspect_ratio =
      mh::ui::graphic_aspect_ratio_config_name(configuration.aspect_ratio);
  const auto renderer_backend = mh::ui::graphic_renderer_backend_config_name(
      configuration.renderer_backend);
  const auto fixed_value = [](const double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
  };
  const auto brightness =
      fixed_value(static_cast<double>(configuration.brightness) * 100.0);
  mh::ui::update_motorhead_configuration(
      reference_root,
      {{"Renderer", std::string(renderer_backend)},
       {"AspectRatio", std::string(aspect_ratio)},
       {"WindowMode", std::string(window_mode)},
       {"ScreenDimension", dimensions},
       {"TrueColour", on_off(configuration.true_colour)},
       {"TrippleBuffer", on_off(configuration.triple_buffer)},
       {"TriLinearFiltering", on_off(configuration.trilinear_filtering)},
       {"TextureFormat", std::to_string(texture)},
       {"Brightness", brightness + " " + brightness + " " + brightness},
       {"InfoDetail", info_name(configuration.info_detail)},
       {"RoadMap", info_name(configuration.info_map)},
       {"CheckpointInfo", info_name(configuration.checkpoint_info)},
       {"CheckpointDelay",
        fixed_value(
            static_cast<double>(configuration.checkpoint_display_time_ms) /
            1000.0)},
       {"DetailMode", detail_name(configuration.detail_mode)},
       {"LensFlares", on_off(configuration.lens_flares)},
       {"Sparks", on_off(configuration.sparks)},
       {"Smoke", on_off(configuration.smoke)},
       {"Halos", on_off(configuration.halos)},
       {"Shadow", on_off(configuration.shadows)},
       {"SkidMarks", on_off(configuration.skid_marks)},
       {"NamePlates", name_plate_name(configuration.name_plates)},
       {"SkyImage", on_off(configuration.background)},
       {"TrackDetail",
        configuration.track_detail == mh::ui::GraphicTrackDetail::medium
            ? "Medium"
            : "High"},
       {"CarDetail", car_detail_name(configuration.car_detail)},
       {"CarShading", car_shading_name(configuration.car_shading)},
       {"ViewDistance", fixed_value(configuration.view_distance)},
       {"MotionBlur", on_off(configuration.motion_blur)},
       {"ZRead", on_off(configuration.z_read)},
       {"CameraShake", on_off(configuration.camera_shake)},
       {"UIScale", fixed_value(configuration.ui_scale)}});
}

void apply_graphic_window_mode(
    SDL_Window *window,
    const mh::ui::GraphicOptionsConfiguration &configuration) {
  if (window == nullptr) {
    throw std::invalid_argument("Graphic window mode requires a window");
  }
  const auto dimensions =
      mh::ui::graphic_screen_dimensions(configuration.screen_size);
  const auto flags = SDL_GetWindowFlags(window);
  const auto fullscreen = (flags & SDL_WINDOW_FULLSCREEN) != 0U;

  if (configuration.window_mode == mh::ui::GraphicWindowMode::windowed) {
    if (fullscreen) {
      require(SDL_SetWindowFullscreen(window, false),
              "leave fullscreen window mode");
    }
    require(SDL_SetWindowFullscreenMode(window, nullptr),
            "clear exclusive fullscreen mode");
    require(SDL_SetWindowBordered(window, true), "restore window border");
    require(SDL_SetWindowResizable(window, true),
            "restore resizable window mode");
    int current_width = 0;
    int current_height = 0;
    require(SDL_GetWindowSize(window, &current_width, &current_height),
            "query windowed Graphic Options size");
    if (current_width != static_cast<int>(dimensions[0U]) ||
        current_height != static_cast<int>(dimensions[1U])) {
      require(SDL_SetWindowSize(window, static_cast<int>(dimensions[0U]),
                                static_cast<int>(dimensions[1U])),
              "apply windowed Graphic Options size");
    }
    return;
  }

  if (configuration.window_mode == mh::ui::GraphicWindowMode::borderless) {
    const auto *active_mode = SDL_GetWindowFullscreenMode(window);
    if (!fullscreen || active_mode != nullptr) {
      require(SDL_SetWindowFullscreenMode(window, nullptr),
              "select borderless desktop mode");
      require(SDL_SetWindowFullscreen(window, true),
              "enter borderless desktop mode");
    }
    return;
  }

  auto display = SDL_GetDisplayForWindow(window);
  if (display == 0U) {
    display = SDL_GetPrimaryDisplay();
  }
  require(display != 0U, "find display for exclusive fullscreen mode");
  SDL_DisplayMode closest{};
  require(SDL_GetClosestFullscreenDisplayMode(
              display, static_cast<int>(dimensions[0U]),
              static_cast<int>(dimensions[1U]), 0.0F, false, &closest),
          "find exclusive fullscreen display mode");
  const auto *active_mode = SDL_GetWindowFullscreenMode(window);
  if (!fullscreen || active_mode == nullptr || active_mode->w != closest.w ||
      active_mode->h != closest.h ||
      active_mode->pixel_density != closest.pixel_density ||
      active_mode->refresh_rate != closest.refresh_rate) {
    require(SDL_SetWindowFullscreenMode(window, &closest),
            "select exclusive fullscreen display mode");
    require(SDL_SetWindowFullscreen(window, true),
            "enter exclusive fullscreen mode");
  }
}

SDL_Renderer *create_front_end_renderer(SDL_Window *window) {
  constexpr std::array drivers{"direct3d12", "direct3d11", "direct3d"};
  for (const auto *driver : drivers) {
    SDL_ClearError();
    if (auto *renderer = SDL_CreateRenderer(window, driver);
        renderer != nullptr) {
      return renderer;
    }
  }
  return SDL_CreateRenderer(window, nullptr);
}

std::uint8_t parse_volume_byte(const std::string &value,
                               const std::uint8_t fallback) {
  if (value.empty()) {
    return fallback;
  }
  try {
    const auto percent = std::stod(value);
    return static_cast<std::uint8_t>(
        std::clamp(std::lround(percent * 255.0 / 100.0), 0L, 255L));
  } catch (...) {
    return fallback;
  }
}

std::uint32_t parse_u32_or(const std::string &value,
                           const std::uint32_t fallback) {
  if (value.empty()) {
    return fallback;
  }
  try {
    const auto parsed = std::stoull(value);
    return parsed > std::numeric_limits<std::uint32_t>::max()
               ? fallback
               : static_cast<std::uint32_t>(parsed);
  } catch (...) {
    return fallback;
  }
}

mh::ui::SoundOptionsConfiguration
load_sound_options_configuration(const std::filesystem::path &reference_root,
                                 const std::vector<RaceSetupTrackAsset> &tracks,
                                 const std::filesystem::path &cue_path) {
  const auto source = mh::ui::motorhead_configuration_path(reference_root);
  mh::ui::SoundOptionsConfiguration result;
  result.output_devices = mh::platform::audio_output_names();
  const auto output_name = configuration_file_value(source, "AudioOutputDevice");
  const auto output = std::find(result.output_devices.begin(),
                                result.output_devices.end(), output_name);
  if (output != result.output_devices.end()) {
    result.output_device_index = static_cast<std::uint32_t>(
        output - result.output_devices.begin());
  }
  result.sound_effects_volume =
      parse_volume_byte(configuration_file_value(source, "SFXVolume"), 128U);
  result.cd_music_volume =
      parse_volume_byte(configuration_file_value(source, "MusicVolume"), 128U);
  result.cd_loop = ascii_equal_case_insensitive(
      configuration_file_value(source, "CDLoop"), "On");

  if (!cue_path.empty() && std::filesystem::is_regular_file(cue_path)) {
    const auto cue = mh::disc::parse_cue(cue_path);
    std::vector<std::uint8_t> audio_tracks;
    for (const auto &track : cue.tracks) {
      if (track.type == mh::disc::TrackType::audio && track.number >= 2 &&
          track.number <= 255) {
        audio_tracks.push_back(static_cast<std::uint8_t>(track.number));
      }
    }
    if (!audio_tracks.empty()) {
      result.first_cd_track =
          *std::min_element(audio_tracks.begin(), audio_tracks.end());
      result.last_cd_track =
          *std::max_element(audio_tracks.begin(), audio_tracks.end());
    }
  }

  result.assigned_cd_tracks.clear();
  result.fixed_cd_track_count = 2U;
  result.assigned_cd_tracks.push_back(2U);
  result.assigned_cd_tracks.push_back(3U);
  const auto normal_count = sound_options_track_count(tracks);
  result.assigned_cd_tracks.reserve(result.fixed_cd_track_count + normal_count);
  for (std::size_t row = 0U; row < normal_count; ++row) {
    const auto &track =
        tracks[sound_options_track_index(tracks, row, normal_count)];
    const auto key = "CDTrack_" + std::to_string(track.definition.division);
    const auto assigned = parse_u32_or(configuration_file_value(source, key),
                                       result.first_cd_track);
    result.assigned_cd_tracks.push_back(
        static_cast<std::uint8_t>(std::clamp<std::uint32_t>(
            assigned, result.first_cd_track, result.last_cd_track)));
  }
  return result;
}

void save_sound_options_configuration(
    const std::filesystem::path &reference_root,
    const std::vector<RaceSetupTrackAsset> &tracks,
    const mh::ui::SoundOptionsConfiguration &configuration) {
  const auto to_percent = [](const std::uint8_t value) {
    constexpr std::uint32_t levels = 20U;
    const auto level = static_cast<std::uint32_t>(
        std::lround(static_cast<double>(value) * levels / 255.0));
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2)
           << static_cast<double>(level * 5U);
    return stream.str();
  };
  mh::ui::ConfigurationUpdates updates{
      {"AudioOutputDevice", std::string(configuration.output_device_name())},
      {"SFXVolume", to_percent(configuration.sound_effects_volume)},
      {"MusicVolume", to_percent(configuration.cd_music_volume)},
      {"CDLoop", configuration.cd_loop ? "On" : "Off"}};
  const auto normal_count = sound_options_track_count(tracks);
  const auto fixed_count =
      std::min<std::size_t>(configuration.fixed_cd_track_count,
                            configuration.assigned_cd_tracks.size());
  const auto editable_count =
      configuration.assigned_cd_tracks.size() - fixed_count;
  const auto count = std::min(normal_count, editable_count);
  for (std::size_t row = 0U; row < count; ++row) {
    const auto track_index =
        sound_options_track_index(tracks, row, normal_count);
    updates.emplace_back(
        "CDTrack_" + std::to_string(tracks[track_index].definition.division),
        std::to_string(static_cast<std::uint32_t>(
            configuration.assigned_cd_tracks[fixed_count + row])));
  }
  mh::ui::update_motorhead_configuration(reference_root, updates);
}

mh::ui::SoundOptionsPresentation make_sound_options_presentation(
    const std::vector<RaceSetupTrackAsset> &tracks,
    const std::filesystem::path &cue_path,
    const std::filesystem::path &music_directory,
    const std::optional<mh::disc::MountedCddaDisc> &mounted_cdda,
    const mh::ui::SoundOptionsConfiguration &configuration) {
  mh::ui::SoundOptionsPresentation result;
  result.tracks.clear();
  std::unordered_map<std::uint32_t, std::uint32_t> durations;
  // Installed WAVs are the normal production source. Query the physical CD
  // and an explicit CUE only for tracks that are absent locally.
  if (std::filesystem::is_directory(music_directory)) {
    const auto read_le32 = [](const std::array<std::uint8_t, 44U> &header,
                              const std::size_t offset) {
      return static_cast<std::uint32_t>(header[offset]) |
             (static_cast<std::uint32_t>(header[offset + 1U]) << 8U) |
             (static_cast<std::uint32_t>(header[offset + 2U]) << 16U) |
             (static_cast<std::uint32_t>(header[offset + 3U]) << 24U);
    };
    for (auto track = 2U; track <= 11U; ++track) {
      std::ostringstream name;
      name << "track" << std::setw(2) << std::setfill('0') << track << ".wav";
      const auto path = music_directory / name.str();
      std::ifstream stream(path, std::ios::binary);
      std::array<std::uint8_t, 44U> header{};
      stream.read(reinterpret_cast<char *>(header.data()),
                  static_cast<std::streamsize>(header.size()));
      if (!stream ||
          std::string_view(reinterpret_cast<const char *>(header.data()), 4U) !=
              "RIFF" ||
          std::string_view(reinterpret_cast<const char *>(header.data() + 8U),
                           4U) != "WAVE" ||
          std::string_view(reinterpret_cast<const char *>(header.data() + 12U),
                           4U) != "fmt " ||
          std::string_view(reinterpret_cast<const char *>(header.data() + 36U),
                           4U) != "data") {
        continue;
      }
      const auto byte_rate = read_le32(header, 28U);
      const auto data_bytes = read_le32(header, 40U);
      if (byte_rate != 0U) {
        durations[track] = data_bytes / byte_rate;
      }
    }
  }
  if (mounted_cdda.has_value()) {
    for (const auto &track : mounted_cdda->tracks) {
      durations.try_emplace(static_cast<std::uint32_t>(track.track_number),
                            track.duration_seconds);
    }
  }
  if (!cue_path.empty() && std::filesystem::is_regular_file(cue_path)) {
    const auto cue = mh::disc::parse_cue(cue_path);
    const auto total_sectors =
        std::filesystem::file_size(cue.binary_path) / 2352U;
    for (auto track = cue.tracks.begin(); track != cue.tracks.end(); ++track) {
      if (track->type != mh::disc::TrackType::audio) {
        continue;
      }
      const auto start = mh::disc::find_index_lba(*track, 1);
      if (!start.has_value()) {
        continue;
      }
      auto end = total_sectors;
      const auto next = std::next(track);
      if (next != cue.tracks.end()) {
        const auto next_start = mh::disc::find_index_lba(*next, 1);
        if (next_start.has_value()) {
          end = mh::disc::find_index_lba(*next, 0).value_or(*next_start);
        }
      }
      if (*start < end) {
        durations.try_emplace(static_cast<std::uint32_t>(track->number),
                              static_cast<std::uint32_t>((end - *start) / 75U));
      }
    }
  }
  const auto fixed_count =
      std::min<std::size_t>(configuration.fixed_cd_track_count,
                            configuration.assigned_cd_tracks.size());
  if (fixed_count >= 1U) {
    result.tracks.push_back(
        {"Results", durations[configuration.assigned_cd_tracks[0U]]});
  }
  if (fixed_count >= 2U) {
    result.tracks.push_back(
        {"Menu", durations[configuration.assigned_cd_tracks[1U]]});
  }
  const auto normal_count = sound_options_track_count(tracks);
  for (std::size_t row = 0U; row < normal_count; ++row) {
    const auto assignment_index = fixed_count + row;
    const auto assigned =
        assignment_index < configuration.assigned_cd_tracks.size()
            ? configuration.assigned_cd_tracks[assignment_index]
            : configuration.first_cd_track;
    const auto track_index =
        sound_options_track_index(tracks, row, normal_count);
    result.tracks.push_back({tracks[track_index].name,
                             durations[static_cast<std::uint32_t>(assigned)]});
  }
  return result;
}

std::uint32_t wav_duration_seconds(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  std::array<std::uint8_t, 44U> header{};
  stream.read(reinterpret_cast<char *>(header.data()),
              static_cast<std::streamsize>(header.size()));
  if (!stream ||
      std::string_view(reinterpret_cast<const char *>(header.data()), 4U) !=
          "RIFF" ||
      std::string_view(reinterpret_cast<const char *>(header.data() + 8U),
                       4U) != "WAVE" ||
      std::string_view(reinterpret_cast<const char *>(header.data() + 12U),
                       4U) != "fmt " ||
      std::string_view(reinterpret_cast<const char *>(header.data() + 36U),
                       4U) != "data") {
    return 0U;
  }
  const auto read_le32 = [&header](const std::size_t offset) {
    return static_cast<std::uint32_t>(header[offset]) |
           (static_cast<std::uint32_t>(header[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(header[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(header[offset + 3U]) << 24U);
  };
  const auto byte_rate = read_le32(28U);
  return byte_rate == 0U ? 0U : read_le32(40U) / byte_rate;
}

mh::ui::SoundOptionsConfiguration s40_sound_options_configuration(
    const mh::ui::SoundOptionsConfiguration &retail,
    const std::filesystem::path &user_data_root, const bool editable) {
  auto result = retail;
  result.song_index = 0U;
  result.first_cd_track = 2U;
  result.last_cd_track = 4U;
  result.fixed_cd_track_count = editable ? 0U : 3U;
  result.assigned_cd_tracks = {2U, 3U, 4U};
  if (editable) {
    const auto source = mh::ui::motorhead_configuration_path(user_data_root);
    constexpr std::array<std::string_view, 3U> keys{
        "S40CDTrack_Menu", "S40CDTrack_Nolby", "S40CDTrack_Okkun"};
    for (std::size_t index = 0U; index < keys.size(); ++index) {
      result.assigned_cd_tracks[index] = static_cast<std::uint8_t>(
          std::clamp<std::uint32_t>(
              parse_u32_or(configuration_file_value(source, keys[index]),
                           result.assigned_cd_tracks[index]),
              result.first_cd_track, result.last_cd_track));
    }
  }
  return result;
}

std::filesystem::path s40_audio_track_path(
    const std::filesystem::path &music_directory,
    const std::uint8_t track) {
  std::ostringstream name;
  name << "track" << std::setw(2) << std::setfill('0')
       << static_cast<unsigned int>(std::clamp<std::uint8_t>(track, 2U, 4U))
       << "a.wav";
  return music_directory / name.str();
}

mh::ui::SoundOptionsPresentation s40_sound_options_presentation(
    const std::filesystem::path &music_directory,
    const mh::ui::SoundOptionsConfiguration &configuration) {
  const auto duration = [&](const std::size_t row) {
    const auto track = row < configuration.assigned_cd_tracks.size()
                           ? configuration.assigned_cd_tracks[row]
                           : static_cast<std::uint8_t>(row + 2U);
    return wav_duration_seconds(s40_audio_track_path(music_directory, track));
  };
  mh::ui::SoundOptionsPresentation result;
  result.tracks = {{"Menu", duration(0U)},
                   {"Nolby Hills", duration(1U)},
                   {"Okkun Speedway", duration(2U)}};
  return result;
}

void save_s40_sound_assignments(
    const std::filesystem::path &user_data_root,
    const mh::ui::SoundOptionsConfiguration &configuration) {
  if (configuration.assigned_cd_tracks.size() < 3U) {
    throw std::runtime_error("S40 sound configuration is incomplete");
  }
  mh::ui::update_motorhead_configuration(
      user_data_root,
      {{"S40CDTrack_Menu",
        std::to_string(configuration.assigned_cd_tracks[0U])},
       {"S40CDTrack_Nolby",
        std::to_string(configuration.assigned_cd_tracks[1U])},
       {"S40CDTrack_Okkun",
        std::to_string(configuration.assigned_cd_tracks[2U])}});
}

std::string selection_mode(const mh::ui::FrontEndState &state,
                           const bool quick_race) {
  return state.race_setup_mode() == mh::ui::RaceSetupMode::ghost_race
             ? state.ghost_mode_choice() ==
                       mh::ui::GhostModeChoice::benchmark_race
                   ? "ghost-benchmark"
               : state.ghost_mode_choice() ==
                       mh::ui::GhostModeChoice::replay_race
                   ? "ghost-replay"
                   : "ghost-race"
         : quick_race ? "quick"
         : state.race_setup_mode() == mh::ui::RaceSetupMode::time_attack
             ? "time"
         : state.race_setup_mode() == mh::ui::RaceSetupMode::league_race
             ? "league"
             : "single";
}

void log_race_selection(const mh::ui::FrontEndState &state,
                        const std::vector<RaceSetupTrackAsset> &tracks,
                        const std::vector<CarSetupAsset> &cars,
                        const bool quick_race) {
  std::ostringstream message;
  message << "Starting race: mode=" << selection_mode(state, quick_race)
          << ", track=" << tracks.at(state.race_setup_track_index()).name
          << ", car=" << cars.at(state.car_setup_car_index()).name
          << ", laps=" << state.race_setup_laps() << ", transmission="
          << (state.car_setup_automatic_transmission() ? "automatic"
                                                       : "manual");
  mh::common::log_runtime_info(message.str());
}

bool ascii_equal_case_insensitive(const std::string_view left,
                                  const std::string_view right) {
  return left.size() == right.size() &&
         std::equal(left.begin(), left.end(), right.begin(),
                    [](const unsigned char a, const unsigned char b) {
                      return std::tolower(a) == std::tolower(b);
                    });
}

bool load_catch_up_setting(const std::filesystem::path &reference_root) {
  return ascii_equal_case_insensitive(
      configuration_file_value(
          mh::ui::motorhead_configuration_path(reference_root), "CatchUp"),
      "On");
}

struct AudioClip {
  SDL_AudioSpec specification{};
  std::vector<std::uint8_t> bytes;
};

AudioClip load_wav_clip(const std::filesystem::path &path,
                        const std::string_view description) {
  SDL_AudioSpec specification{};
  std::uint8_t *buffer = nullptr;
  std::uint32_t byte_count = 0U;
  require(
      SDL_LoadWAV(path.string().c_str(), &specification, &buffer, &byte_count),
      "load " + std::string(description));
  std::unique_ptr<void, decltype(&SDL_free)> guard(buffer, SDL_free);
  const auto frame_size = SDL_AUDIO_FRAMESIZE(specification);
  if (frame_size <= 0 || byte_count == 0U ||
      byte_count % static_cast<std::uint32_t>(frame_size) != 0U) {
    throw std::runtime_error(std::string(description) +
                             " has an empty or partial sample frame");
  }
  AudioClip result;
  result.specification = specification;
  result.bytes.assign(buffer, buffer + byte_count);
  return result;
}

AudioClip preconvert_sample_rate(AudioClip clip, const int target_frequency) {
  if (target_frequency <= 0) {
    throw std::runtime_error("playback device reported an invalid sample rate");
  }
  if (clip.specification.freq == target_frequency) {
    return clip;
  }
  if (clip.bytes.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("menu music is too large to convert");
  }
  auto target = clip.specification;
  target.freq = target_frequency;
  std::uint8_t *buffer = nullptr;
  int byte_count = 0;
  require(SDL_ConvertAudioSamples(&clip.specification, clip.bytes.data(),
                                  static_cast<int>(clip.bytes.size()), &target,
                                  &buffer, &byte_count),
          "preconvert complete menu-music sample rate");
  std::unique_ptr<void, decltype(&SDL_free)> guard(buffer, SDL_free);
  if (byte_count <= 0 || byte_count % SDL_AUDIO_FRAMESIZE(target) != 0) {
    throw std::runtime_error(
        "converted menu music has an empty or partial sample frame");
  }
  AudioClip result;
  result.specification = target;
  result.bytes.assign(buffer, buffer + byte_count);
  return result;
}

struct LoopingMusicState {
  AudioClip clip;
  std::size_t cursor = 0U;
  bool looping = true;
  std::atomic_bool failed = false;

  void queue(SDL_AudioStream *stream, std::size_t remaining) noexcept {
    while (remaining != 0U) {
      if (cursor == clip.bytes.size()) {
        if (!looping) {
          return;
        }
        cursor = 0U;
      }
      const auto contiguous = clip.bytes.size() - cursor;
      const auto chunk = std::min(
          {remaining, contiguous,
           static_cast<std::size_t>(std::numeric_limits<int>::max())});
      if (!SDL_PutAudioStreamData(stream, clip.bytes.data() + cursor,
                                  static_cast<int>(chunk))) {
        failed.store(true, std::memory_order_relaxed);
        return;
      }
      cursor += chunk;
      remaining -= chunk;
    }
  }
};

void SDLCALL refill_menu_music(void *userdata, SDL_AudioStream *stream,
                               const int additional_amount,
                               const int /*total_amount*/) {
  auto *state = static_cast<LoopingMusicState *>(userdata);
  if (additional_amount > 0) {
    state->queue(stream, static_cast<std::size_t>(additional_amount));
  }
}

class MenuMusic {
public:
  MenuMusic(const std::filesystem::path &path,
            const std::filesystem::path &cue_path,
            std::optional<mh::disc::MountedCddaDisc> mounted_cdda,
            const bool available, const std::string &output_name)
      : menu_path_(path), menu_cue_path_(cue_path),
        mounted_cdda_(std::move(mounted_cdda)), available_(available),
        enabled_(available), output_name_(output_name) {}

  void set_output(const std::string &name) {
    stop();
    output_name_ = name;
  }

  void play_menu_track() {
    if (!available_) {
      return;
    }
    constexpr std::uint8_t retail_front_end_cd_track = 3U;
    play_track(retail_front_end_cd_track, true);
  }

  void play_track(const std::uint8_t track_number, const bool looping) {
    if (!available_) {
      return;
    }
    if (!menu_path_.empty()) {
      std::ostringstream name;
      name << "track" << std::setw(2) << std::setfill('0')
           << static_cast<unsigned int>(track_number) << ".wav";
      const auto path = menu_path_.parent_path() / name.str();
      if (std::filesystem::is_regular_file(path)) {
        open_clip(load_wav_clip(path, "installed CD soundtrack WAV"), looping);
        return;
      }
    }
    std::optional<mh::disc::CddaPcmTrack> track;
    if (mounted_cdda_.has_value()) {
      try {
        track =
            mh::disc::read_mounted_cdda_track_pcm(*mounted_cdda_, track_number);
      } catch (const std::exception &) {
        // A drive can disappear or reject raw reads after discovery. Continue
        // through the deterministic CUE fallback in that case.
        mounted_cdda_.reset();
      }
    }
    if (!track.has_value() && !menu_cue_path_.empty()) {
      if (!std::filesystem::is_regular_file(menu_cue_path_)) {
        throw std::runtime_error("original disc CUE was not found: " +
                                 menu_cue_path_.string());
      }
      track = mh::disc::read_cdda_track_pcm(menu_cue_path_, track_number);
    }
    if (!track.has_value()) {
      return;
    }
    AudioClip clip;
    clip.specification.format = SDL_AUDIO_S16LE;
    clip.specification.channels = static_cast<int>(track->channels);
    clip.specification.freq = static_cast<int>(track->sample_rate);
    clip.bytes = std::move(track->pcm);
    open_clip(std::move(clip), looping);
  }

  void play_file(const std::filesystem::path &path, const bool looping,
                 const float source_gain = 1.0F) {
    if (!available_) {
      return;
    }
    if (!std::filesystem::is_regular_file(path)) {
      throw std::runtime_error("menu music WAV was not found: " +
                               path.string());
    }
    open_clip(load_wav_clip(path, "installed menu music WAV"), looping,
              source_gain);
  }

  void check() const {
    if (state_ != nullptr && state_->failed.load(std::memory_order_relaxed)) {
      throw std::runtime_error("queue front-end music: " +
                               std::string(SDL_GetError()));
    }
  }

  void set_enabled(const bool enabled) {
    enabled_ = available_ && enabled;
    apply_gain();
  }

  void set_volume(const std::uint8_t volume) {
    volume_ = volume;
    apply_gain();
  }

  void set_looping(const bool looping) {
    if (state_ != nullptr) {
      state_->looping = looping;
    }
  }

  void stop() {
    stream_.reset();
    state_.reset();
  }

private:
  void open_clip(AudioClip clip, const bool looping,
                 const float source_gain = 1.0F) {
    SDL_AudioSpec preferred{};
    int sample_frames = 0;
    require(mh::platform::audio_output_format(output_name_,
                                     &preferred, &sample_frames),
            "query preferred menu-music device format");
    clip = preconvert_sample_rate(std::move(clip), preferred.freq);
    stream_.reset();
    state_ = std::make_unique<LoopingMusicState>();
    state_->clip = std::move(clip);
    state_->looping = looping;
    source_gain_ = source_gain;
    stream_.reset(mh::platform::open_audio_output_stream(output_name_,
                                            &state_->clip.specification,
                                            refill_menu_music, state_.get()));
    require(stream_ != nullptr, "open front-end music stream");
    apply_gain();
    const auto initial_bytes = std::max<std::size_t>(
        32768U, static_cast<std::size_t>(sample_frames) *
                    static_cast<std::size_t>(
                        SDL_AUDIO_FRAMESIZE(state_->clip.specification)) *
                    2U);
    state_->queue(stream_.get(), initial_bytes);
    check();
    require(SDL_ResumeAudioStreamDevice(stream_.get()),
            "start front-end music");
  }

  void apply_gain() {
    if (stream_ != nullptr) {
      const auto gain =
          enabled_ ? 0.22F * source_gain_ * static_cast<float>(volume_) /
                         255.0F
                   : 0.0F;
      require(SDL_SetAudioStreamGain(stream_.get(), gain),
              "set front-end music gain");
    }
  }

  std::filesystem::path menu_path_;
  std::filesystem::path menu_cue_path_;
  std::optional<mh::disc::MountedCddaDisc> mounted_cdda_;
  bool available_ = false;
  bool enabled_ = false;
  std::string output_name_;
  std::uint8_t volume_ = 254U;
  float source_gain_ = 1.0F;
  // Destroy the stream before the callback state it refers to.
  std::unique_ptr<LoopingMusicState> state_;
  SdlPointer<SDL_AudioStream, SDL_DestroyAudioStream> stream_{
      nullptr, SDL_DestroyAudioStream};
};

struct OneShot {
  std::vector<std::uint8_t> bytes;
  SdlPointer<SDL_AudioStream, SDL_DestroyAudioStream> stream{
      nullptr, SDL_DestroyAudioStream};

  void trigger() {
    require(SDL_ClearAudioStream(stream.get()), "clear menu audio stream");
    require(SDL_PutAudioStreamData(stream.get(), bytes.data(),
                                   static_cast<int>(bytes.size())),
            "queue menu audio");
    require(SDL_FlushAudioStream(stream.get()), "flush menu audio");
    require(SDL_ResumeAudioStreamDevice(stream.get()), "resume menu audio");
  }
};

std::unique_ptr<OneShot> load_one_shot(const std::filesystem::path &path,
                                      const std::string &output_name) {
  auto clip = load_wav_clip(path, "original menu WAV");
  if (clip.bytes.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::runtime_error("menu WAV has an unsupported byte count");
  }
  auto result = std::make_unique<OneShot>();
  result->bytes = std::move(clip.bytes);
  result->stream.reset(
      mh::platform::open_audio_output_stream(output_name,
                                &clip.specification, nullptr, nullptr));
  require(result->stream != nullptr, "open menu audio stream");
  return result;
}

class MenuAudio {
public:
  MenuAudio(const std::filesystem::path &directory, const bool enabled,
             const std::string &output_name) {
    if (!enabled) {
      return;
    }
    const auto config =
        mh::content::read_menu_sound_config(directory / "MenuSound.CFG");
    for (std::uint32_t event = 1U; event < mh::content::menu_sound_event_count;
         ++event) {
      const auto index = static_cast<std::size_t>(event);
      const auto *binding =
          mh::content::resolve_menu_sound_event(config, event);
      if (binding == nullptr) {
        continue;
      }
      events_[index] = load_one_shot(directory / binding->sample_name, output_name);
    }
  }

  void dispatch(const mh::ui::MenuTransitionResult &transition) {
    for (std::uint8_t index = 0U; index < transition.sound_event_count;
         ++index) {
      const auto event =
          static_cast<std::size_t>(transition.sound_events[index]);
      if (event < events_.size() && events_[event] != nullptr) {
        events_[event]->trigger();
      }
    }
  }

  void set_volume(const std::uint8_t volume) {
    volume_ = volume;
    const auto gain = static_cast<float>(volume_) / 255.0F;
    for (auto &event : events_) {
      if (event != nullptr) {
        require(SDL_SetAudioStreamGain(event->stream.get(), gain),
                "set menu sound-effect gain");
      }
    }
  }

private:
  std::uint8_t volume_ = 254U;
  std::array<std::unique_ptr<OneShot>, mh::content::menu_sound_event_count>
      events_{};
};

class HornPreviewAudio {
public:
  HornPreviewAudio(const bool enabled, const std::string &output_name)
      : enabled_(enabled), output_name_(output_name) {}

  void play(const std::filesystem::path &path) {
    if (!enabled_) {
      return;
    }
    current_ = load_one_shot(path, output_name_);
    set_volume(volume_);
    current_->trigger();
  }

  void set_volume(const std::uint8_t volume) {
    volume_ = volume;
    if (current_ != nullptr) {
      require(SDL_SetAudioStreamGain(current_->stream.get(),
                                     static_cast<float>(volume_) / 255.0F),
              "set horn-preview sound-effect gain");
    }
  }

private:
  bool enabled_ = false;
  std::uint8_t volume_ = 254U;
  std::string output_name_;
  std::unique_ptr<OneShot> current_;
};

SdlPointer<SDL_Texture, SDL_DestroyTexture>
make_frame_texture(SDL_Renderer *renderer, const std::uint32_t width,
                   const std::uint32_t height) {
  SdlPointer<SDL_Texture, SDL_DestroyTexture> result(
      SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32,
                        SDL_TEXTUREACCESS_STREAMING, static_cast<int>(width),
                        static_cast<int>(height)),
      SDL_DestroyTexture);
  require(result != nullptr, "create front-end texture");
  require(SDL_SetTextureScaleMode(result.get(), SDL_SCALEMODE_LINEAR),
          "set original front-end scaling");
  return result;
}

SdlPointer<SDL_Texture, SDL_DestroyTexture>
make_frame_texture(SDL_Renderer *renderer) {
  return make_frame_texture(renderer, mh::ui::front_end_logical_width, 480U);
}

constexpr std::uint32_t front_end_wing_width =
    (mh::ui::front_end_widescreen_logical_width -
     mh::ui::front_end_logical_width) /
    2U;

SdlPointer<SDL_Texture, SDL_DestroyTexture>
make_widescreen_ambient_texture(SDL_Renderer *renderer) {
  return make_frame_texture(renderer, front_end_wing_width * 2U,
                            mh::ui::front_end_logical_height);
}

void extract_front_end_wings(const mh::content::TgaImage &source,
                             std::vector<std::uint8_t> &result) {
  if (source.width != mh::ui::front_end_widescreen_logical_width ||
      source.height != mh::ui::front_end_logical_height ||
      source.rgba.size() !=
          static_cast<std::size_t>(source.width) * source.height * 4U) {
    throw std::runtime_error("widescreen front-end background is malformed");
  }
  const auto destination_width = front_end_wing_width * 2U;
  result.resize(static_cast<std::size_t>(destination_width) * source.height *
                4U);
  for (std::uint32_t y = 0U; y < source.height; ++y) {
    const auto source_row = static_cast<std::size_t>(y) * source.width * 4U;
    const auto destination_row =
        static_cast<std::size_t>(y) * destination_width * 4U;
    std::copy_n(source.rgba.begin() +
                    static_cast<std::ptrdiff_t>(source_row),
                front_end_wing_width * 4U,
                result.begin() +
                    static_cast<std::ptrdiff_t>(destination_row));
    std::copy_n(
        source.rgba.begin() + static_cast<std::ptrdiff_t>(
                                  source_row +
                                  (source.width - front_end_wing_width) * 4U),
        front_end_wing_width * 4U,
        result.begin() + static_cast<std::ptrdiff_t>(
                             destination_row + front_end_wing_width * 4U));
  }
}

void extract_front_end_center_background(
    const mh::content::TgaImage &source, mh::content::TgaImage &result) {
  if (source.width < mh::ui::front_end_logical_width ||
      source.height != mh::ui::front_end_logical_height ||
      source.rgba.size() !=
          static_cast<std::size_t>(source.width) * source.height * 4U ||
      source.palette_indices.size() !=
          static_cast<std::size_t>(source.width) * source.height) {
    throw std::runtime_error("widescreen front-end background is malformed");
  }

  const auto offset_x = (static_cast<std::uint32_t>(source.width) -
                         mh::ui::front_end_logical_width) /
                        2U;
  result = source;
  result.width = mh::ui::front_end_logical_width;
  result.palette_indices.clear();
  result.rgba.resize(static_cast<std::size_t>(result.width) * result.height *
                     4U);
  for (std::uint32_t y = 0U; y < mh::ui::front_end_logical_height; ++y) {
    const auto source_pixel =
        static_cast<std::size_t>(y) * source.width + offset_x;
    const auto destination_pixel =
        static_cast<std::size_t>(y) * mh::ui::front_end_logical_width;
    std::copy_n(source.rgba.begin() +
                    static_cast<std::ptrdiff_t>(source_pixel * 4U),
                mh::ui::front_end_logical_width * 4U,
                result.rgba.begin() +
                    static_cast<std::ptrdiff_t>(destination_pixel * 4U));
  }
}

void make_retail_front_end_display_frame(
    const mh::ui::FrontEndFrame &composed,
    const mh::content::TgaImage &background, mh::ui::FrontEndFrame &result) {
  constexpr std::uint32_t display_height = 480U;
  if (composed.width != mh::ui::front_end_logical_width ||
      composed.height != mh::ui::front_end_logical_height ||
      composed.rgba.size() !=
          static_cast<std::size_t>(composed.width) * composed.height * 4U ||
      background.width != composed.width ||
      background.height != composed.height ||
      background.rgba.size() != composed.rgba.size()) {
    throw std::runtime_error("front-end display frame is malformed");
  }

  result.width = composed.width;
  result.height = display_height;
  result.rgba.resize(
      static_cast<std::size_t>(result.width) * result.height * 4U);

  // Expand the 640x400 Back.tga backdrop to the full 640x480 display, then
  // preserve MENU.SPR/FNT artwork at its authored proportions. This keeps the
  // Track/Car previews and gauges oval without reintroducing letterbox bars.
  std::uint32_t source_y = 0U;
  std::uint32_t row_fraction = 0U;
  for (std::uint32_t y = 0U; y < result.height; ++y) {
    const auto source_offset =
        static_cast<std::size_t>(source_y) * background.width * 4U;
    const auto destination_offset =
        static_cast<std::size_t>(y) * result.width * 4U;
    std::copy_n(
        background.rgba.begin() + static_cast<std::ptrdiff_t>(source_offset),
        static_cast<std::ptrdiff_t>(result.width * 4U),
        result.rgba.begin() + static_cast<std::ptrdiff_t>(destination_offset));
    row_fraction += background.height;
    if (row_fraction >= result.height) {
      row_fraction -= result.height;
      source_y = std::min(source_y + 1U,
                          static_cast<std::uint32_t>(background.height - 1U));
    }
  }

  // Preserve every authored overlay pixel at its original Y coordinate.  A
  // pixel equal to the source backdrop is transparent menu space and should
  // continue to reveal the expanded background.
  for (std::uint32_t y = 0U; y < composed.height; ++y) {
    for (std::uint32_t x = 0U; x < composed.width; ++x) {
      const auto pixel =
          (static_cast<std::size_t>(y) * composed.width + x) * 4U;
      std::uint32_t composed_pixel = 0U;
      std::uint32_t background_pixel = 0U;
      std::memcpy(&composed_pixel, composed.rgba.data() + pixel, 4U);
      std::memcpy(&background_pixel, background.rgba.data() + pixel, 4U);
      if (composed_pixel != background_pixel) {
        std::memcpy(result.rgba.data() + pixel, &composed_pixel, 4U);
      }
    }
  }
}

SDL_FRect presentation_rect(const int width, const int height) {
  const auto result = mh::ui::front_end_presentation_rect(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  return {result.x, result.y, result.width, result.height};
}

SDL_FRect widescreen_presentation_rect(const int width, const int height) {
  const auto result = mh::ui::front_end_widescreen_presentation_rect(
      static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
  return {result.x, result.y, result.width, result.height};
}

enum class MoviePacketKind : std::uint8_t {
  video,
  audio,
  finished,
  failed,
};

struct MoviePacket {
  MoviePacketKind kind = MoviePacketKind::finished;
  mh::content::SmackerDecodedFrame frame;
  std::vector<std::uint8_t> pcm;
  std::uint32_t sample_rate = 0U;
  std::uint8_t channels = 0U;
  std::uint8_t bits_per_sample = 0U;
  std::string error;
};

class MoviePacketQueue {
public:
  bool push(MoviePacket packet) {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] {
      return cancelled_ || packets_.size() < maximum_packets;
    });
    if (cancelled_) {
      return false;
    }
    packets_.push_back(std::move(packet));
    condition_.notify_all();
    return true;
  }

  bool try_pop(MoviePacket &packet) {
    std::lock_guard lock(mutex_);
    if (packets_.empty()) {
      return false;
    }
    packet = std::move(packets_.front());
    packets_.pop_front();
    condition_.notify_all();
    return true;
  }

  void cancel() {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    packets_.clear();
    condition_.notify_all();
  }

private:
  static constexpr std::size_t maximum_packets = 12U;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<MoviePacket> packets_;
  bool cancelled_ = false;
};

enum class StartupMovieResult : std::uint8_t {
  completed,
  skipped,
  quit,
};

constexpr std::uint32_t startup_movie_buffer_width = 640U;
constexpr std::uint32_t startup_movie_buffer_height = 480U;

SDL_FRect startup_movie_destination(const SDL_FRect presentation,
                                    const std::uint32_t frame_width,
                                    const std::uint32_t frame_height,
                                    const bool fill_original_buffer) {
  if (frame_width > startup_movie_buffer_width ||
      frame_height > startup_movie_buffer_height) {
    throw std::runtime_error(
        "startup movie frame exceeds the original 640x480 Smacker buffer");
  }
  // The retail half-height Smacker streams are interlaced/anamorphic content
  // intended for the original 640x480 movie buffer. Present those frames at
  // the buffer height instead of exposing their stored 640x240 field size.
  // Other authored sizes (notably the 640x400 opening logo) remain centred at
  // their native dimensions.
  const auto fills_interlaced_buffer =
      frame_width == startup_movie_buffer_width &&
      frame_height * 2U == startup_movie_buffer_height;
  if (fill_original_buffer || fills_interlaced_buffer) {
    return presentation;
  }
  const auto logical_scale =
      presentation.w / static_cast<float>(startup_movie_buffer_width);
  return {presentation.x +
              (static_cast<float>(startup_movie_buffer_width - frame_width) *
               0.5F * logical_scale),
          presentation.y +
              (static_cast<float>(startup_movie_buffer_height - frame_height) *
               0.5F * logical_scale),
          static_cast<float>(frame_width) * logical_scale,
          static_cast<float>(frame_height) * logical_scale};
}

StartupMovieResult
play_startup_movie(SDL_Window *window, SDL_Renderer *renderer,
                   const std::filesystem::path &movies_path,
                   const std::uint32_t entry_index, const bool audio_enabled,
                   const bool unpaced, const bool fill_original_buffer,
                   const std::string &output_name) {
  MoviePacketQueue queue;
  std::thread decoder([&queue, &movies_path, entry_index] {
    try {
      const auto completed = mh::content::read_divi_stream(
          movies_path, entry_index,
          mh::content::SmackerStreamVisitors{
              [&queue](mh::content::SmackerDecodedFrame frame) {
                MoviePacket packet;
                packet.kind = MoviePacketKind::video;
                packet.frame = std::move(frame);
                return queue.push(std::move(packet));
              },
              [&queue](const std::uint32_t sample_rate,
                       const std::uint8_t channels,
                       const std::uint8_t bits_per_sample,
                       const std::span<const std::uint8_t> pcm) {
                MoviePacket packet;
                packet.kind = MoviePacketKind::audio;
                packet.sample_rate = sample_rate;
                packet.channels = channels;
                packet.bits_per_sample = bits_per_sample;
                packet.pcm.assign(pcm.begin(), pcm.end());
                return queue.push(std::move(packet));
              }});
      if (completed) {
        MoviePacket packet;
        packet.kind = MoviePacketKind::finished;
        static_cast<void>(queue.push(std::move(packet)));
      }
    } catch (const std::exception &error) {
      MoviePacket packet;
      packet.kind = MoviePacketKind::failed;
      packet.error = error.what();
      static_cast<void>(queue.push(std::move(packet)));
    }
  });

  auto stop_decoder = [&queue, &decoder] {
    queue.cancel();
    if (decoder.joinable()) {
      decoder.join();
    }
  };

  SdlPointer<SDL_Texture, SDL_DestroyTexture> movie_texture{nullptr,
                                                            SDL_DestroyTexture};
  std::uint32_t movie_texture_width = 0U;
  std::uint32_t movie_texture_height = 0U;
  SdlPointer<SDL_AudioStream, SDL_DestroyAudioStream> movie_audio{
      nullptr, SDL_DestroyAudioStream};
  std::deque<mh::content::SmackerDecodedFrame> pending_frames;
  mh::content::SmackerDecodedFrame current_frame;
  bool have_current_frame = false;
  bool stream_finished = false;
  bool audio_started = false;
  std::uint64_t next_frame_ticks = 0U;

  try {
    while (true) {
      SDL_Event event{};
      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
          stop_decoder();
          return StartupMovieResult::quit;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
          stop_decoder();
          return StartupMovieResult::skipped;
        }
      }

      MoviePacket packet;
      while (pending_frames.size() < 4U && queue.try_pop(packet)) {
        if (packet.kind == MoviePacketKind::video) {
          pending_frames.push_back(std::move(packet.frame));
        } else if (packet.kind == MoviePacketKind::audio && audio_enabled &&
                   !packet.pcm.empty()) {
          if (packet.bits_per_sample != 16U || packet.channels == 0U) {
            throw std::runtime_error(
                "startup movie audio format is unsupported");
          }
          if (movie_audio == nullptr) {
            const SDL_AudioSpec specification{
                SDL_AUDIO_S16LE, packet.channels,
                static_cast<int>(packet.sample_rate)};
            movie_audio.reset(
                mh::platform::open_audio_output_stream(output_name,
                                          &specification, nullptr, nullptr));
            require(movie_audio != nullptr, "open startup movie audio stream");
          }
          if (packet.pcm.size() >
              static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            throw std::runtime_error("startup movie audio packet is too large");
          }
          require(SDL_PutAudioStreamData(movie_audio.get(), packet.pcm.data(),
                                         static_cast<int>(packet.pcm.size())),
                  "queue startup movie audio");
        } else if (packet.kind == MoviePacketKind::finished) {
          stream_finished = true;
        } else if (packet.kind == MoviePacketKind::failed) {
          throw std::runtime_error(packet.error);
        }
      }

      const auto ticks = SDL_GetTicks();
      if (!pending_frames.empty() &&
          (unpaced || !have_current_frame || ticks >= next_frame_ticks)) {
        current_frame = std::move(pending_frames.front());
        pending_frames.pop_front();
        have_current_frame = true;
        next_frame_ticks =
            unpaced ? ticks
                    : (next_frame_ticks == 0U || ticks > next_frame_ticks + 200U
                           ? ticks + 40U
                           : next_frame_ticks + 40U);
        if (movie_texture == nullptr ||
            current_frame.width != movie_texture_width ||
            current_frame.height != movie_texture_height) {
          movie_texture.reset(SDL_CreateTexture(
              renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING,
              static_cast<int>(current_frame.width),
              static_cast<int>(current_frame.height)));
          require(movie_texture != nullptr, "create startup movie texture");
          require(SDL_SetTextureScaleMode(movie_texture.get(),
                                          SDL_SCALEMODE_LINEAR),
                  "set startup movie scaling");
          movie_texture_width = current_frame.width;
          movie_texture_height = current_frame.height;
        }
        require(SDL_UpdateTexture(movie_texture.get(), nullptr,
                                  current_frame.rgba.data(),
                                  static_cast<int>(current_frame.width * 4U)),
                "upload startup movie frame");
        if (movie_audio != nullptr && !audio_started) {
          require(SDL_ResumeAudioStreamDevice(movie_audio.get()),
                  "start startup movie audio");
          audio_started = true;
        }
      }

      int width = 0;
      int height = 0;
      require(SDL_GetWindowSizeInPixels(window, &width, &height),
              "query startup movie window size");
      require(SDL_SetRenderDrawColor(renderer, 0U, 0U, 0U, 255U),
              "set startup movie border color");
      require(SDL_RenderClear(renderer), "clear startup movie frame");
      if (have_current_frame) {
        const auto presentation = presentation_rect(width, height);
        const auto destination = startup_movie_destination(
            presentation, current_frame.width, current_frame.height,
            fill_original_buffer);
        require(SDL_RenderTexture(renderer, movie_texture.get(), nullptr,
                                  &destination),
                "render startup movie frame");
      }
      require(SDL_RenderPresent(renderer), "present startup movie frame");

      if (stream_finished && pending_frames.empty() && have_current_frame &&
          (unpaced || ticks >= next_frame_ticks)) {
        stop_decoder();
        return StartupMovieResult::completed;
      }
      SDL_Delay(1U);
    }
  } catch (...) {
    stop_decoder();
    throw;
  }
}

StartupMovieResult play_startup_movies(SDL_Window *window,
                                       SDL_Renderer *renderer,
                                       const std::filesystem::path &movies_path,
                                       const std::filesystem::path &third_movie_path,
                                       const bool audio_enabled,
                                       const bool unpaced,
                                       const bool key_skips_all,
                                       const std::string &output_name) {
  for (const std::uint32_t entry : {0U, 1U, 2U}) {
    const auto use_third_movie = entry == 2U && !third_movie_path.empty();
    const auto result = play_startup_movie(window, renderer,
                                           use_third_movie ? third_movie_path : movies_path,
                                           use_third_movie ? 0U : entry,
                                           audio_enabled, unpaced, false,
                                           output_name);
    if (result == StartupMovieResult::quit ||
        (result == StartupMovieResult::skipped && key_skips_all)) {
      return result;
    }
  }
  return StartupMovieResult::completed;
}

int run(const Options &options) {
  mh::ui::prepare_user_data(options.reference_root, options.user_data_root);
  mh::common::log_runtime_info("Content root: " +
                               options.reference_root.string());
  mh::common::log_runtime_info("User data root: " +
                               options.user_data_root.string());
  // --skip-intro remains the command-line override used by automated runs and
  // launchers. The persistent setting owns normal installed launches and is
  // evaluated before MOVIES.PAK is located or any decoder thread is started.
  const auto configured_intro_mode =
      mh::ui::intro_movie_mode(options.user_data_root);
  const auto startup_movies_enabled =
      options.startup_movies &&
      configured_intro_mode != mh::ui::IntroMovieMode::skip;
  const auto data_directory = options.reference_root / "Data";
  const auto sound_directory = options.reference_root / "Sounds" / "Menu";
  const auto initial_graphic_options =
      load_graphic_options_configuration(options.user_data_root);
  const auto menu_joystick_enabled = ascii_equal_case_insensitive(
      configuration_file_value(
          mh::ui::motorhead_configuration_path(options.user_data_root),
          "MenuJoystick"),
      "On");
  const auto initial_window_size =
      mh::ui::graphic_screen_dimensions(initial_graphic_options.screen_size);
  {
    std::ostringstream message;
    message << "Front end: renderer="
            << mh::ui::graphic_renderer_backend_config_name(
                   initial_graphic_options.renderer_backend)
            << ", resolution=" << initial_window_size[0U] << 'x'
            << initial_window_size[1U] << ", aspect="
            << mh::ui::graphic_aspect_ratio_config_name(
                   initial_graphic_options.aspect_ratio)
            << ", window="
            << mh::ui::graphic_window_mode_config_name(
                   initial_graphic_options.window_mode);
    mh::common::log_runtime_info(message.str());
  }
  const auto background = mh::content::read_tga(data_directory / "back.tga");
  const auto s40_background =
      s40_definition_path(options.reference_root).has_value()
          ? std::optional<mh::content::TgaImage>(mh::content::read_tga(
                data_directory / "back1.tga"))
          : std::nullopt;
  const auto s40_intro_movies =
      options.reference_root / "Movies" / "movies1.pak";
  auto personal_options =
      mh::ui::load_personal_options_configuration(options.user_data_root);
  auto audio_output_name = configuration_file_value(
      mh::ui::motorhead_configuration_path(options.user_data_root),
      "AudioOutputDevice");
  const auto multiplayer_client_background =
      mh::content::read_tga(data_directory / "Client.tga");
  const auto multiplayer_server_background =
      mh::content::read_tga(data_directory / "Server.tga");
  const auto sprites = mh::content::read_spr(data_directory / "MENU.SPR");
  const auto menu_dial_sprites =
      mh::content::read_spr(data_directory / "MenuDial.spr");
  const auto positions =
      mh::content::read_spr_positions(data_directory / "sprpos.dta");
  const auto font = mh::content::read_fnt(data_directory / "FONT1.FNT");
  std::array<mh::content::LobData, 9U> line_objects;
  for (std::size_t index = 0U; index < line_objects.size(); ++index) {
    auto name = std::string("lobj00.lob");
    name[5U] = static_cast<char>('0' + index);
    line_objects[index] =
        mh::content::read_lob(data_directory / "LINEOBJ" / name);
  }

  require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD |
                   (options.audio ? SDL_INIT_AUDIO : 0U)),
          "initialize SDL");
  struct SdlGuard {
    ~SdlGuard() { SDL_Quit(); }
  } sdl_guard;

  // Resource identifier 1 is the original icon embedded by motorhead.rc; the
  // hint makes SDL's window class use it for the title bar and taskbar.
  SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
  SDL_Window *raw_window = SDL_CreateWindow(
      "Motorhead", static_cast<int>(initial_window_size[0U]),
      static_cast<int>(initial_window_size[1U]), SDL_WINDOW_RESIZABLE);
  require(raw_window != nullptr, "create main-menu window");
  SDL_Renderer *raw_renderer = create_front_end_renderer(raw_window);
  require(raw_renderer != nullptr, "create main-menu renderer");
  SdlPointer<SDL_Window, SDL_DestroyWindow> window(raw_window,
                                                   SDL_DestroyWindow);
  SdlPointer<SDL_Renderer, SDL_DestroyRenderer> renderer(raw_renderer,
                                                         SDL_DestroyRenderer);
  SdlPointer<SDL_Joystick, SDL_CloseJoystick> menu_joystick(nullptr,
                                                            SDL_CloseJoystick);
  SDL_JoystickID menu_joystick_id = 0U;
  const auto open_first_menu_joystick = [&]() {
    if (!menu_joystick_enabled || menu_joystick != nullptr) {
      return;
    }
    int count = 0;
    SDL_JoystickID *identifiers = SDL_GetJoysticks(&count);
    if (identifiers != nullptr) {
      if (count > 0) {
        menu_joystick.reset(SDL_OpenJoystick(identifiers[0]));
        if (menu_joystick != nullptr) {
          menu_joystick_id = SDL_GetJoystickID(menu_joystick.get());
        }
      }
      SDL_free(identifiers);
    }
  };
  open_first_menu_joystick();
  apply_graphic_window_mode(window.get(), initial_graphic_options);
  // The race runtime already presents with vertical sync; pace the front end
  // the same way so the menu does not recompose software frames faster than
  // the display. Frame-counted smokes and unpaced intro runs must stay
  // unthrottled.
  if (options.maximum_frames == 0U && !options.startup_movies_unpaced) {
    require(SDL_SetRenderVSync(renderer.get(), 1),
            "enable front-end vertical sync");
  }
  require(SDL_SetRenderColorScale(renderer.get(),
                                  initial_graphic_options.brightness),
          "apply initial Graphic Options brightness");
  require(SDL_StartTextInput(window.get()),
          "enable Personal Options text input");
  if (startup_movies_enabled) {
    auto movies_path = options.movies_path;
    if (movies_path.empty()) {
      for (const auto &candidate :
           {options.reference_root / "MOVIES" / "MOVIES.PAK",
            options.reference_root / "Movies" / "MOVIES.PAK",
            options.reference_root / "MOVIES.PAK"}) {
        if (std::filesystem::is_regular_file(candidate)) {
          movies_path = candidate;
          break;
        }
      }
    }
    if (movies_path.empty() || !std::filesystem::is_regular_file(movies_path)) {
      throw std::runtime_error(
          "MOVIES.PAK was not found; pass --movies MOVIES.PAK or "
          "--skip-intro");
    }
    const auto movie_result = play_startup_movies(
        window.get(), renderer.get(), movies_path,
        s40_background.has_value() && s40_racing_identity(personal_options)
            ? s40_intro_movies
            : std::filesystem::path{},
        options.audio && !options.startup_movies_unpaced,
        options.startup_movies_unpaced,
        mh::ui::intro_movie_key_skips_all(configured_intro_mode),
        audio_output_name);
    if (movie_result == StartupMovieResult::quit) {
      return 0;
    }
  }
  auto texture = make_frame_texture(renderer.get());
  auto widescreen_ambient_texture =
      make_widescreen_ambient_texture(renderer.get());
  // Start the owner-validated retail menu track after the startup-movie
  // sequence (including individually skipped clips), and restart it at EOF.
  MenuMusic music(options.music_path, options.disc_cue_path,
                  options.mounted_cdda, options.audio, audio_output_name);
  MenuAudio audio(sound_directory, options.audio, audio_output_name);
  HornPreviewAudio horn_audio(options.audio, audio_output_name);
  auto one_player_values = load_one_player_panel_values(options.user_data_root);
  auto hidden_league_names = load_hidden_leagues(options.user_data_root);
  auto league_overviews = load_league_overview_presentations(
      options.reference_root, options.user_data_root, one_player_values);
  std::erase_if(
      league_overviews,
      [&hidden_league_names](const mh::ui::LeagueOverviewPresentation &league) {
        return std::any_of(
            hidden_league_names.begin(), hidden_league_names.end(),
            [&league](const std::string &hidden) {
              return ascii_equal_case_insensitive(hidden, league.league_name);
            });
      });
  // A manually edited reconstruction metadata file must never make the
  // original league set unusable. Restore visibility when it hides all LGFs.
  if (league_overviews.empty()) {
    hidden_league_names.clear();
    save_hidden_leagues(options.user_data_root, hidden_league_names);
    league_overviews = load_league_overview_presentations(
        options.reference_root, options.user_data_root, one_player_values);
  }
  auto hidden_ghost_demo_files =
      load_hidden_ghost_demos(options.user_data_root);
  auto ghost_demo_files =
      load_ghost_demo_files(options.reference_root, hidden_ghost_demo_files);
  auto race_setup_tracks = load_race_setup_tracks(options.reference_root);
  auto car_setup_assets = load_car_setup_assets(options.reference_root);
  const auto s40_car_setup_asset =
      load_s40_car_setup_asset(options.reference_root);
  std::vector<mh::content::FrontEndUnlockTrack> unlock_tracks;
  unlock_tracks.reserve(race_setup_tracks.size());
  for (const auto &track : race_setup_tracks) {
    unlock_tracks.push_back(
        {track.definition.division,
         original_track_flag_available(options.user_data_root, track)});
  }
  std::vector<mh::content::FrontEndUnlockCar> unlock_cars;
  unlock_cars.reserve(car_setup_assets.size());
  for (const auto &car : car_setup_assets) {
    unlock_cars.push_back({car.division});
  }
  auto unlock_catalog = mh::content::derive_front_end_unlock_catalog(
      unlock_tracks, unlock_cars,
      original_all_unlock_identity(personal_options));
  race_setup_tracks =
      reorder_assets(std::move(race_setup_tracks), unlock_catalog.track_order);
  car_setup_assets =
      reorder_assets(std::move(car_setup_assets), unlock_catalog.car_order);
  // Keep the unlock inputs in the same canonical retail order as the menu
  // assets. Progression refreshes rebuild track availability from the ordered
  // menu list, so retaining the discovery-order metadata here would make the
  // next refresh appear to change an otherwise stable asset order.
  unlock_tracks =
      reorder_assets(std::move(unlock_tracks), unlock_catalog.track_order);
  unlock_cars =
      reorder_assets(std::move(unlock_cars), unlock_catalog.car_order);
  unlock_catalog = mh::content::derive_front_end_unlock_catalog(
      unlock_tracks, unlock_cars,
      original_all_unlock_identity(personal_options));
  const auto retail_race_setup_tracks = race_setup_tracks;
  auto retail_car_setup_assets = car_setup_assets;
  const auto s40_race_setup_tracks =
      s40_car_setup_asset.has_value()
          ? s40_racing_track_assets(retail_race_setup_tracks)
          : std::vector<RaceSetupTrackAsset>{};
  auto s40_progression_reward =
      s40_racing_progression_reward(unlock_tracks, unlock_cars);
  const auto s40_reward_available = [&]() {
    return s40_car_setup_asset.has_value() && s40_progression_reward;
  };
  const auto normal_car_count = [&]() {
    return unlock_catalog.car_count + (s40_reward_available() ? 1U : 0U);
  };
  const auto normal_unlocked_car_count = [&]() {
    return unlock_catalog.unlocked_car_count + (s40_reward_available() ? 1U : 0U);
  };
  if (s40_reward_available()) {
    retail_car_setup_assets.push_back(*s40_car_setup_asset);
    car_setup_assets = retail_car_setup_assets;
  }
  auto s40_mode = s40_car_setup_asset.has_value() &&
                  s40_racing_identity(personal_options);
  // A newly entered identity does not switch the presentation until its
  // intro has completed. A saved identity starts directly in its theme.
  auto s40_presentation_active = s40_mode;
  auto s40_intro_pending = false;
  auto s40_assets_active = false;
  const auto horn_setup_assets = load_horn_setup_assets(options.reference_root);
  const auto refresh_league_overview =
      [&retail_race_setup_tracks](
          mh::ui::LeagueOverviewPresentation &overview,
          const mh::content::LeagueDefinition &league) {
        const auto human =
            std::find_if(league.players.begin(), league.players.end(),
                         [](const mh::content::LeaguePlayer &player) {
                           return player.human;
                         });
        if (human == league.players.end()) {
          throw std::runtime_error("league has no human player: " +
                                   overview.source_path);
        }
        overview.score = human->score;
        overview.player_name = human->name;
        overview.division = human->division;
        overview.races_done = league.races_done;
        const auto division = std::min<std::size_t>(
            human->division, human->division_cars.size() - 1U);
        overview.car_name = human->division_cars[division];
        overview.standings.clear();
        for (const auto &player : league.players) {
          if (player.division == human->division) {
            overview.standings.push_back(
                {player.name, player.score, player.human});
          }
        }
        std::stable_sort(
            overview.standings.begin(), overview.standings.end(),
            [](const auto &left, const auto &right) {
              return left.score > right.score;
            });
        std::vector<std::uint32_t> track_divisions;
        track_divisions.reserve(retail_race_setup_tracks.size());
        for (const auto &track : retail_race_setup_tracks) {
          track_divisions.push_back(track.definition.division);
        }
        const auto schedule = mh::game::original_league_track_schedule(
            track_divisions, human->division);
        if (league.race_number >= schedule.size()) {
          throw std::runtime_error(
              "league RaceNumber is outside its original track schedule: " +
              overview.source_path);
        }
        overview.next_track =
            retail_race_setup_tracks
                .at(schedule[static_cast<std::size_t>(league.race_number)])
                .name;
      };
  for (auto &overview : league_overviews) {
    auto league = mh::content::read_league(overview.source_path);
    if (!is_reconstruction_owned_league(options.user_data_root,
                                        overview.source_path)) {
      reset_league_progress(league);
      apply_player_to_league(league, one_player_values, personal_options);
    }
    refresh_league_overview(overview, league);
  }
  const auto default_league = mh::content::read_league(
      options.reference_root / "League" / "Default.LGF");
  const auto default_league_human = std::find_if(
      default_league.players.begin(), default_league.players.end(),
      [](const mh::content::LeaguePlayer &player) { return player.human; });
  if (default_league_human == default_league.players.end()) {
    throw std::runtime_error("Default.LGF has no human player");
  }
  const auto default_league_division = default_league_human->division;
  auto league_draft = league_overviews.front();
  league_draft.league_name = "League";
  league_draft.division = default_league_division;
  league_draft.races_done = 0U;
  league_draft.score = 0U;

  mh::ui::FrontEndState state;
  state.configure_one_player_league_available(!s40_presentation_active);
  auto league_create_divisions =
      mh::content::unlocked_league_divisions(unlock_cars, unlock_catalog);
  if (league_create_divisions.empty()) {
    throw std::runtime_error(
        "front-end unlock catalog exposes no League division");
  }
  state.configure_league_create_divisions(
      0U, static_cast<std::uint32_t>(league_create_divisions.size()));
  league_draft.division = league_create_divisions.front();
  state.configure_graphic_options(initial_graphic_options);
  const auto apply_graphic_screen_size = [&]() {
    apply_graphic_window_mode(window.get(),
                              state.graphic_options_configuration());
  };
  state.configure_personal_options(personal_options);
  mh::network::LanSession multiplayer_session;
  bool multiplayer_launch_pending = false;
  const auto multiplayer_player_colour = [&]() {
    const auto &colour = state.personal_options_configuration().player_colour;
    return (static_cast<std::uint32_t>(colour.red) << 16U) |
           (static_cast<std::uint32_t>(colour.green) << 8U) |
           static_cast<std::uint32_t>(colour.blue);
  };
  const auto sync_multiplayer_lobby = [&]() {
    std::vector<mh::ui::MultiplayerLobbyPlayer> players;
    players.reserve(multiplayer_session.players().size());
    auto local_ready = false;
    for (const auto &player : multiplayer_session.players()) {
      const auto local = player.peer == multiplayer_session.local_peer();
      players.push_back({player.peer, player.name, player.car_selection,
                         player.ready, player.host, local, player.team,
                         player.automatic_gear, player.car_name, player.colour,
                         player.races, player.score, player.grid_position,
                         player.lap, player.ping_ms});
      if (local) {
        local_ready = player.ready;
      }
    }
    state.set_multiplayer_lobby_players(std::move(players), local_ready);
  };
  const auto sync_multiplayer_sessions = [&]() {
    std::vector<mh::ui::MultiplayerSessionEntry> sessions;
    sessions.reserve(multiplayer_session.sessions().size());
    for (const auto &session : multiplayer_session.sessions()) {
      sessions.push_back({session.name, session.address, session.players,
                          session.capacity, session.password_required});
    }
    state.set_multiplayer_sessions(std::move(sessions));
  };
  const auto sync_multiplayer_configuration = [&]() {
    auto track_index = state.race_setup_track_index();
    auto laps = state.race_setup_laps();
    auto mode = 0U;
    if (const auto configuration =
            multiplayer_session.pending_race_configuration();
        configuration.has_value()) {
      track_index = configuration->track_index;
      laps = configuration->lap_count;
      mode = configuration->mode;
    }
    track_index = std::min<std::uint32_t>(
        track_index, static_cast<std::uint32_t>(race_setup_tracks.size() - 1U));
    state.set_multiplayer_lobby_configuration(
        race_setup_tracks[track_index].name, laps,
        car_setup_assets[state.car_setup_car_index()].name,
        state.car_setup_automatic_transmission() ? "Automatic" : "Manual",
        mode == 0U ? "Normal" : "Elimination");
  };
  state.configure_control_options(
      load_control_options_configuration(options.user_data_root));
  {
    const auto &controls = state.control_options_configuration();
    if (!controls.profiles.empty()) {
      const auto index = std::min<std::size_t>(controls.profile_index,
                                               controls.profiles.size() - 1U);
      mh::common::log_runtime_info("Input profile: " +
                                   controls.profiles[index].name);
    }
  }
  auto retail_sound_options_configuration = load_sound_options_configuration(
      options.user_data_root, retail_race_setup_tracks, options.disc_cue_path);
  state.configure_sound_options(
      s40_presentation_active
          ? s40_sound_options_configuration(retail_sound_options_configuration,
                                            options.user_data_root,
                                            s40_progression_reward)
          : retail_sound_options_configuration);
  auto sound_options_presentation =
      s40_presentation_active
          ? s40_sound_options_presentation(
                options.reference_root / "Music",
                state.sound_options_configuration())
          : make_sound_options_presentation(
                retail_race_setup_tracks, options.disc_cue_path,
                options.music_path.parent_path(), options.mounted_cdda,
                state.sound_options_configuration());
  const auto apply_menu_audio_levels = [&] {
    const auto &sound = state.sound_options_configuration();
    music.set_volume(sound.cd_music_volume);
    audio.set_volume(sound.sound_effects_volume);
    horn_audio.set_volume(sound.sound_effects_volume);
  };
  apply_menu_audio_levels();
  if (!options.startup_handoff_report_path.empty()) {
    if (!startup_movies_enabled || !options.startup_movies_unpaced ||
        !options.audio ||
        (options.music_path.empty() && options.disc_cue_path.empty())) {
      throw std::runtime_error(
          "startup handoff report requires movies and menu audio");
    }
    std::filesystem::create_directories(
        options.startup_handoff_report_path.parent_path());
    std::ofstream report(options.startup_handoff_report_path,
                         std::ios::binary | std::ios::trunc);
    if (!report) {
      throw std::runtime_error("could not create startup handoff report");
    }
    report
        << "{\"schema\":\"motorhead.front-end-startup-handoff.v1\""
        << ",\"startup_entries\":[0,1,2]"
        << ",\"startup_movies_completed\":true"
        << ",\"startup_buffer\":[640,480]"
        << ",\"native_presentations\":["
           "{\"entry\":0,\"source\":[640,400],\"destination\":[0,40,640,400]},"
           "{\"entry\":1,\"source\":[640,240],\"destination\":[0,120,640,240]},"
           "{\"entry\":2,\"source\":[640,240],\"destination\":[0,120,640,240]}]"
        << ",\"native_movie_scaling\":true"
        << ",\"verification_unpaced\":true"
        << ",\"verification_movie_audio\":false"
        << ",\"menu_music_opened\":true"
        << ",\"menu_music_looping\":true"
        << ",\"menu_frame_pending\":true}\n";
    if (!report) {
      throw std::runtime_error("could not write startup handoff report");
    }
  }
  bool preview_music_active = false;
  bool control_settings_dirty = false;
  bool graphic_settings_dirty = false;
  const auto configured_league = std::find_if(
      league_overviews.begin(), league_overviews.end(),
      [](const mh::ui::LeagueOverviewPresentation &league) {
        return ascii_equal_case_insensitive(league.league_name, "Default");
      });
  state.configure_leagues(
      configured_league == league_overviews.end()
          ? 0U
          : static_cast<std::uint32_t>(
                std::distance(league_overviews.begin(), configured_league)),
      static_cast<std::uint32_t>(league_overviews.size()));
  const auto configured_demo =
      std::find_if(ghost_demo_files.begin(), ghost_demo_files.end(),
                   [&one_player_values](const std::string &file) {
                     return ascii_equal_case_insensitive(
                         std::filesystem::path(file).stem().string(),
                         one_player_values.third_value);
                   });
  state.configure_ghost_files(
      configured_demo == ghost_demo_files.end()
          ? 0U
          : static_cast<std::uint32_t>(
                std::distance(ghost_demo_files.begin(), configured_demo)),
      static_cast<std::uint32_t>(ghost_demo_files.size()));
  const auto configured_track =
      std::find_if(race_setup_tracks.begin(), race_setup_tracks.end(),
                   [&one_player_values](const RaceSetupTrackAsset &track) {
                     return ascii_equal_case_insensitive(
                         track.name, one_player_values.track);
                   });
  auto configured_laps = 5U;
  try {
    configured_laps =
        static_cast<std::uint32_t>(std::stoul(one_player_values.third_value));
  } catch (...) {
    configured_laps = 5U;
  }
  const auto configured_track_index = std::min<std::uint32_t>(
      configured_track == race_setup_tracks.end()
          ? 0U
          : static_cast<std::uint32_t>(
                std::distance(race_setup_tracks.begin(), configured_track)),
      unlock_catalog.unlocked_track_count == 0U
          ? 0U
          : unlock_catalog.unlocked_track_count - 1U);
  state.configure_race_setup(
      {one_player_values.mode == mh::ui::OnePlayerMode::time_attack
           ? mh::ui::RaceSetupMode::time_attack
       : one_player_values.mode == mh::ui::OnePlayerMode::league_race
           ? mh::ui::RaceSetupMode::league_race
           : mh::ui::RaceSetupMode::single_race,
       configured_track_index, unlock_catalog.track_count,
       unlock_catalog.unlocked_track_count, configured_laps,
       load_catch_up_setting(options.user_data_root)});
  state.configure_rankings(
      static_cast<std::uint32_t>(
          sound_options_track_count(retail_race_setup_tracks)));
  auto ranking_history = load_rankings(options.user_data_root);
  const auto configured_car = std::find_if(
      car_setup_assets.begin(), car_setup_assets.end(),
      [&one_player_values](const CarSetupAsset &car) {
        return ascii_equal_case_insensitive(car.name, one_player_values.car);
      });
  const auto config_path =
      mh::ui::motorhead_configuration_path(options.user_data_root);
  const auto configured_horn_name =
      configuration_file_value(config_path, "CarHorn");
  const auto configured_horn = std::find_if(
      horn_setup_assets.begin(), horn_setup_assets.end(),
      [&configured_horn_name](const HornSetupAsset &horn) {
        return ascii_equal_case_insensitive(horn.name, configured_horn_name);
      });
  const auto configured_car_index = std::min<std::uint32_t>(
      configured_car == car_setup_assets.end()
          ? 0U
          : static_cast<std::uint32_t>(
                std::distance(car_setup_assets.begin(), configured_car)),
      normal_unlocked_car_count() == 0U
          ? 0U
          : normal_unlocked_car_count() - 1U);
  state.configure_car_setup(
      {configured_car_index, normal_car_count(),
       normal_unlocked_car_count(),
       ascii_equal_case_insensitive(one_player_values.transmission, "Auto"),
       configured_horn == horn_setup_assets.end()
           ? 0U
           : static_cast<std::uint32_t>(
                 std::distance(horn_setup_assets.begin(), configured_horn)),
       static_cast<std::uint32_t>(horn_setup_assets.size()),
       ascii_equal_case_insensitive(
           configuration_file_value(config_path, "RecordRace"), "On")});
  auto retail_track_selection = state.race_setup_track_index();
  auto retail_car_selection = state.car_setup_car_index();
  auto s40_track_selection = 0U;
  const auto apply_s40_asset_selection = [&](const bool enabled) {
    const auto activate = enabled && s40_car_setup_asset.has_value();
    if (activate == s40_assets_active) {
      return;
    }
    if (s40_assets_active) {
      s40_track_selection = state.race_setup_track_index();
    } else {
      retail_track_selection = state.race_setup_track_index();
      retail_car_selection = state.car_setup_car_index();
    }
    s40_assets_active = activate;
    if (activate) {
      race_setup_tracks = s40_race_setup_tracks;
      car_setup_assets = {*s40_car_setup_asset};
      state.configure_race_setup(
          {state.race_setup_mode(),
           std::min<std::uint32_t>(
               s40_track_selection,
               static_cast<std::uint32_t>(race_setup_tracks.size() - 1U)),
           static_cast<std::uint32_t>(race_setup_tracks.size()),
           static_cast<std::uint32_t>(race_setup_tracks.size()),
           state.race_setup_laps(), state.race_setup_catch_up()});
      state.configure_car_setup(
          {0U, 1U, 1U, state.car_setup_automatic_transmission(),
           state.car_setup_horn_index(), state.car_setup_horn_count(),
           state.car_setup_record_race()});
      return;
    }
    race_setup_tracks = retail_race_setup_tracks;
    car_setup_assets = retail_car_setup_assets;
    state.configure_race_setup(
        {state.race_setup_mode(),
         std::min<std::uint32_t>(
             retail_track_selection,
             unlock_catalog.unlocked_track_count == 0U
                 ? 0U
                 : unlock_catalog.unlocked_track_count - 1U),
         unlock_catalog.track_count, unlock_catalog.unlocked_track_count,
         state.race_setup_laps(), state.race_setup_catch_up()});
    state.configure_car_setup(
        {std::min<std::uint32_t>(
             retail_car_selection,
             normal_unlocked_car_count() == 0U
                 ? 0U
                 : normal_unlocked_car_count() - 1U),
         normal_car_count(), normal_unlocked_car_count(),
         state.car_setup_automatic_transmission(), state.car_setup_horn_index(),
         state.car_setup_horn_count(), state.car_setup_record_race()});
  };
  const auto refresh_active_sound_options = [&]() {
    const auto &current = state.sound_options_configuration();
    retail_sound_options_configuration.sound_effects_volume =
        current.sound_effects_volume;
    retail_sound_options_configuration.cd_music_volume =
        current.cd_music_volume;
    retail_sound_options_configuration.cd_loop = current.cd_loop;
    state.configure_sound_options(
        s40_presentation_active
            ? s40_sound_options_configuration(
                  retail_sound_options_configuration, options.user_data_root,
                  s40_progression_reward)
            : retail_sound_options_configuration);
    sound_options_presentation =
        s40_presentation_active
            ? s40_sound_options_presentation(
                  options.reference_root / "Music",
                  state.sound_options_configuration())
            : make_sound_options_presentation(
                  retail_race_setup_tracks, options.disc_cue_path,
                  options.music_path.parent_path(), options.mounted_cdda,
                  state.sound_options_configuration());
  };
  const auto play_active_menu_music = [&]() {
    if (s40_presentation_active) {
      const auto &sound = state.sound_options_configuration();
      const auto track = sound.assigned_cd_tracks.empty()
                             ? 2U
                             : sound.assigned_cd_tracks[0U];
      music.play_file(s40_audio_track_path(
                          options.reference_root / "Music",
                          static_cast<std::uint8_t>(track)),
                      true, s40_music_gain);
    } else {
      music.play_menu_track();
    }
  };
  const auto refresh_unlock_catalog = [&]() {
    const auto refreshed = mh::content::derive_front_end_unlock_catalog(
        unlock_tracks, unlock_cars,
        original_all_unlock_identity(personal_options));
    // all-unlock changes only the visible/available bounds. Division ordering
    // is independent of the bypass and the already-reordered asset arrays must
    // therefore remain valid.
    if (refreshed.track_order != unlock_catalog.track_order ||
        refreshed.car_order != unlock_catalog.car_order) {
      throw std::runtime_error(
          "front-end unlock refresh changed the retail asset order");
    }
    unlock_catalog = refreshed;
    const auto reward_was_unlocked = s40_progression_reward;
    s40_progression_reward =
        s40_racing_progression_reward(unlock_tracks, unlock_cars);
    if (!reward_was_unlocked && s40_reward_available()) {
      retail_car_setup_assets.push_back(*s40_car_setup_asset);
      if (!s40_assets_active) {
        car_setup_assets = retail_car_setup_assets;
      }
      refresh_active_sound_options();
    }
    const auto previous_draft_division = league_draft.division;
    league_create_divisions =
        mh::content::unlocked_league_divisions(unlock_cars, unlock_catalog);
    if (league_create_divisions.empty()) {
      throw std::runtime_error(
          "front-end unlock refresh exposes no League division");
    }
    const auto retained_division = std::find(league_create_divisions.begin(),
                                             league_create_divisions.end(),
                                             previous_draft_division);
    const auto division_index =
        retained_division == league_create_divisions.end()
            ? 0U
            : static_cast<std::uint32_t>(std::distance(
                  league_create_divisions.begin(), retained_division));
    state.configure_league_create_divisions(
        division_index,
        static_cast<std::uint32_t>(league_create_divisions.size()));
    league_draft.division =
        league_create_divisions[state.league_create_division_index()];
    if (!s40_assets_active) {
      const auto track_index = std::min<std::uint32_t>(
          state.race_setup_track_index(),
          unlock_catalog.unlocked_track_count == 0U
              ? 0U
              : unlock_catalog.unlocked_track_count - 1U);
      state.configure_race_setup(
          {state.race_setup_mode(), track_index, unlock_catalog.track_count,
           unlock_catalog.unlocked_track_count, state.race_setup_laps(),
           state.race_setup_catch_up()});
      const auto car_index = std::min<std::uint32_t>(
          state.car_setup_car_index(),
          normal_unlocked_car_count() == 0U
              ? 0U
              : normal_unlocked_car_count() - 1U);
      state.configure_car_setup(
          {car_index, normal_car_count(),
           normal_unlocked_car_count(),
           state.car_setup_automatic_transmission(),
           state.car_setup_horn_index(), state.car_setup_horn_count(),
           state.car_setup_record_race()});
    }
  };
  const auto save_personal_options_and_refresh_unlocks = [&]() {
    personal_options = state.personal_options_configuration();
    mh::ui::save_personal_options_configuration(options.user_data_root,
                                                personal_options);
    for (auto &overview : league_overviews) {
      if (is_reconstruction_owned_league(options.user_data_root,
                                         overview.source_path)) {
        continue;
      }
      auto league = mh::content::read_league(overview.source_path);
      reset_league_progress(league);
      apply_player_to_league(league, one_player_values, personal_options);
      refresh_league_overview(overview, league);
    }
    refresh_unlock_catalog();
    const auto updated_s40_mode =
        s40_car_setup_asset.has_value() &&
        s40_racing_identity(personal_options);
    if (updated_s40_mode != s40_mode) {
      s40_intro_pending = updated_s40_mode && !s40_mode &&
                          state.screen() ==
                              mh::ui::FrontEndScreen::personal_options;
      s40_mode = updated_s40_mode;
      if (!s40_mode && s40_presentation_active) {
        s40_presentation_active = false;
        state.configure_one_player_league_available(true);
        apply_s40_asset_selection(false);
        refresh_active_sound_options();
        play_active_menu_music();
        music.set_volume(state.sound_options_configuration().cd_music_volume);
      }
    }
  };
  apply_s40_asset_selection(
      s40_presentation_active && s40_racing_uses_special_assets(
                                     state.screen(), state.race_setup_mode()));
  // Start exactly one theme after the saved identity has been loaded. Starting
  // the retail track in MenuMusic's constructor made an existing S40 profile
  // audibly switch from track03 to track02a during front-end initialization.
  play_active_menu_music();
  auto main_pointer_units =
      mh::ui::main_menu_pointer_target_units(state.main_choice());
  auto one_player_pointer_units =
      mh::ui::one_player_pointer_target_units(state.one_player_choice());
  auto league_menu_pointer_units =
      mh::ui::league_menu_pointer_target_units(state.league_menu_choice());
  auto options_pointer_units =
      mh::ui::options_menu_pointer_target_units(state.options_choice());
  mh::ui::MainMenuGlitchState main_glitch_state;
  mh::ui::MainMenuGlitchState one_player_glitch_state;
  mh::ui::MainMenuGlitchState options_glitch_state;
  mh::ui::MainMenuGlitchState sound_glitch_state;
  mh::ui::CarSetupPerformanceAnimationState car_performance_state;
  mh::ui::FrontEndBackgroundAnimationState background_animation_state;
  mh::ui::FrontEndBackgroundRenderCache background_render_cache;
  ScreenTransition screen_transition;
  auto terminal_action = mh::ui::FrontEndAction::none;
  bool terminal_quick_race = false;
  std::uint32_t terminal_transition_ms = 0U;
  std::uint64_t exit_confirmation_opened_ticks = 0U;
  std::uint64_t exit_confirmation_closed_ticks = 0U;
  bool exit_confirmation_closing = false;
  auto closing_exit_confirmation_choice = mh::ui::ExitConfirmationChoice::no;
  auto previous_ticks = SDL_GetTicks();
  bool front_end_focus_lost = false;
  bool front_end_renderer_recovery_pending = false;
  bool running = true;
  int exit_code = 0;
  std::uint32_t rendered_frames = 0U;
  mh::content::TgaImage animated_background_storage;
  std::optional<mh::content::TgaImage> widescreen_animated_background;
  std::vector<std::uint8_t> widescreen_ambient_wings;
  mh::ui::FrontEndFrame frame;
  mh::ui::FrontEndFrame display_frame;
  bool widescreen_ambient_texture_ready = false;
  const auto run_selected_race = [&]() {
    auto selected_track_index =
        static_cast<std::size_t>(state.race_setup_track_index());
    std::optional<std::filesystem::path> selected_league_path;
    std::optional<mh::content::LeagueDefinition> selected_league;
    std::optional<std::vector<std::size_t>> selected_league_track_schedule;
    std::optional<std::filesystem::path> selected_ghost_path;
    std::string selected_ghost_mode;
    if (state.race_setup_mode() == mh::ui::RaceSetupMode::league_race) {
      auto &selected_league_overview =
          league_overviews.at(static_cast<std::size_t>(state.league_index()));
      selected_league_path = selected_league_overview.source_path;
      if (!std::filesystem::is_regular_file(*selected_league_path)) {
        throw std::runtime_error("selected local LGF is unavailable: " +
                                 selected_league_path->string());
      }
      selected_league = mh::content::read_league(*selected_league_path);
      const auto human = std::find_if(
          selected_league->players.begin(), selected_league->players.end(),
          [](const mh::content::LeaguePlayer &player) { return player.human; });
      if (human == selected_league->players.end()) {
        throw std::runtime_error("selected league has no human player");
      }
      if (!is_reconstruction_owned_league(options.user_data_root,
                                          *selected_league_path)) {
        reset_league_progress(*selected_league);
        apply_player_to_league(*selected_league, one_player_values,
                               state.personal_options_configuration());
        selected_league_path = next_reconstruction_league_path(
            options.user_data_root, selected_league->name);
        mh::content::write_league(*selected_league_path, *selected_league);
        selected_league_overview.source_path = selected_league_path->string();
      }
      std::vector<std::uint32_t> track_divisions;
      track_divisions.reserve(race_setup_tracks.size());
      for (const auto &track : race_setup_tracks) {
        track_divisions.push_back(track.definition.division);
      }
      selected_league_track_schedule = mh::game::original_league_track_schedule(
          track_divisions, human->division);
      if (selected_league->race_number >=
          selected_league_track_schedule->size()) {
        throw std::runtime_error("selected league RaceNumber is outside its "
                                 "original track schedule");
      }
      selected_track_index = selected_league_track_schedule->at(
          static_cast<std::size_t>(selected_league->race_number));
    }
    if (state.race_setup_mode() == mh::ui::RaceSetupMode::ghost_race) {
      if (ghost_demo_files.empty()) {
        throw std::runtime_error(
            "ghost race was selected but no retail MDE files are available");
      }
      selected_ghost_path = options.reference_root / "Demos" /
                            ghost_demo_files.at(state.ghost_file_index());
      const auto demo = mh::content::read_mde(*selected_ghost_path);
      const auto matching_track =
          std::find_if(race_setup_tracks.begin(), race_setup_tracks.end(),
                       [&demo](const RaceSetupTrackAsset &candidate) {
                         return ascii_equal_case_insensitive(candidate.name,
                                                             demo.track_name);
                       });
      if (matching_track == race_setup_tracks.end()) {
        throw std::runtime_error("selected MDE names an unavailable track: " +
                                 demo.track_name);
      }
      selected_track_index = static_cast<std::size_t>(
          std::distance(race_setup_tracks.begin(), matching_track));
      selected_ghost_mode =
          state.ghost_mode_choice() == mh::ui::GhostModeChoice::benchmark_race
              ? "benchmark"
          : state.ghost_mode_choice() == mh::ui::GhostModeChoice::replay_race
              ? "replay"
              : "race";
    }
    const auto &selected_track = race_setup_tracks.at(selected_track_index);
    auto selected_car_index =
        static_cast<std::size_t>(state.car_setup_car_index());
    auto selected_horn_index =
        static_cast<std::size_t>(state.car_setup_horn_index());
    if (selected_league.has_value()) {
      mh::content::set_league_human_vehicle(
          *selected_league, car_setup_assets.at(selected_car_index).name,
          horn_setup_assets.at(selected_horn_index).name);
      mh::content::replace_league(*selected_league_path, *selected_league);
      league_overviews
          .at(static_cast<std::size_t>(state.league_index()))
          .car_name = car_setup_assets.at(selected_car_index).name;
    }
    const auto &selected_horn = horn_setup_assets.at(selected_horn_index);
    // Car definitions retain their source paths after Division sorting.
    const auto car_definition_path =
        car_setup_assets.at(selected_car_index).definition_path;
    const auto s40_race =
        s40_mode &&
        car_setup_assets.at(selected_car_index).source_number == 0U &&
        state.race_setup_mode() != mh::ui::RaceSetupMode::league_race;
    const auto collision_path = resolve_relative_case_insensitive(
        options.reference_root,
        required_track_reference(selected_track.definition, "collisionname"));
    const auto motion_path = resolve_relative_case_insensitive(
        options.reference_root,
        required_track_reference(selected_track.definition, "splinename"));
    const auto ai_root = resolve_relative_case_insensitive(
        options.reference_root,
        required_track_reference(selected_track.definition, "aitrackname"));
    const auto ai_path =
        resolve_relative_case_insensitive(ai_root, "AI/ai.dat");
    const auto &sound = state.sound_options_configuration();
    const auto &graphics = state.graphic_options_configuration();
    const auto &personal = state.personal_options_configuration();
    const auto &controls = state.control_options_configuration();
    const auto control_profile_index = std::min<std::size_t>(
        controls.profile_index, controls.profiles.size() - 1U);
    const auto &control_profile = controls.profiles[control_profile_index];
    const auto race_window_size =
        mh::ui::graphic_screen_dimensions(graphics.screen_size);
    if (graphics.window_mode == mh::ui::GraphicWindowMode::windowed) {
      require(SDL_SetWindowSize(window.get(),
                                static_cast<int>(race_window_size[0U]),
                                static_cast<int>(race_window_size[1U])),
              "apply selected Graphic Options screen size");
    }
    std::vector<std::string> arguments{
        "Motorhead race runtime",
        collision_path.string(),
        car_definition_path.string(),
        motion_path.string(),
        ai_path.string(),
        "--track-definition",
        selected_track.definition_path.string(),
        "--laps",
        std::to_string(state.race_setup_laps()),
        "--difficulty",
        std::to_string(static_cast<std::uint32_t>(state.difficulty())),
        "--sfx-volume",
        std::to_string(sound.sound_effects_volume),
        "--music-volume",
        std::to_string(sound.cd_music_volume),
        "--size",
        std::to_string(race_window_size[0U]) + "x" +
            std::to_string(race_window_size[1U]),
        "--window-mode",
        std::string(
            mh::ui::graphic_window_mode_config_name(graphics.window_mode)),
        "--configuration-root",
        options.user_data_root.string(),
        "--measurement",
        personal.measurement_system == mh::ui::MeasurementSystem::metric
            ? "metric"
            : "imperial",
        "--bind-left",
        control_profile.bindings[0U],
        "--bind-right",
        control_profile.bindings[1U],
        "--bind-accelerate",
        control_profile.bindings[2U],
        "--bind-brake",
        control_profile.bindings[3U],
        "--bind-gear-up",
        control_profile.bindings[4U],
        "--bind-gear-down",
        control_profile.bindings[5U],
        "--bind-handbrake",
        control_profile.bindings[7U],
        "--bind-rear-view",
        control_profile.bindings[8U],
        "--bind-horn",
        control_profile.bindings[6U],
        "--bind-in-car-view",
        control_profile.fixed_race_bindings[0U],
        "--bind-out-car-view",
        control_profile.fixed_race_bindings[1U],
        "--bind-camera-view",
        control_profile.fixed_race_bindings[2U],
        "--bind-cycle-players",
        control_profile.fixed_race_bindings[3U],
        "--automatic-transmission",
        state.car_setup_automatic_transmission() ? "on" : "off",
    };
    const auto personal_arguments =
        mh::ui::playable_personal_arguments(personal);
    arguments.insert(arguments.end(), personal_arguments.begin(),
                     personal_arguments.end());
    const auto renderer_arguments =
        mh::ui::playable_graphic_renderer_arguments(graphics);
    arguments.insert(arguments.end(), renderer_arguments.begin(),
                     renderer_arguments.end());
    if (selected_ghost_path.has_value()) {
      arguments.insert(arguments.end(),
                       {"--ghost-demo", selected_ghost_path->string(),
                        "--ghost-mode", selected_ghost_mode});
    }
    if (state.race_setup_mode() == mh::ui::RaceSetupMode::league_race) {
      arguments.insert(arguments.end(),
                       {"--result-style", "league", "--league-definition",
                        selected_league_path->string()});
    } else if (state.race_setup_mode() == mh::ui::RaceSetupMode::single_race) {
      arguments.emplace_back("--live-opponents");
      if (s40_race) {
        arguments.emplace_back("--one-make-opponents");
      }
      if (!terminal_quick_race) {
        arguments.emplace_back("--single-race-cpu-profile");
      }
      if (state.race_setup_catch_up()) {
        arguments.emplace_back("--cpu-catch-up");
      }
    }
    if (options.audio) {
      std::optional<std::filesystem::path> installed_track;
      std::uint8_t music_track = 0U;
      if (s40_race) {
        const auto assignment =
            selected_track.source_number == 2U ||
                    selected_track.source_number == 10U
                ? 1U
                : 2U;
        const auto &sound = state.sound_options_configuration();
        music_track = assignment < sound.assigned_cd_tracks.size()
                          ? sound.assigned_cd_tracks[assignment]
                          : static_cast<std::uint8_t>(assignment + 2U);
        const auto wav = s40_audio_track_path(
            options.reference_root / "Music", music_track);
        if (std::filesystem::is_regular_file(wav)) {
          installed_track = wav;
        }
      } else {
        const auto assignment_index = sound_assignment_index(
            selected_track.source_number,
            retail_sound_options_configuration.assigned_cd_tracks.size(),
            retail_sound_options_configuration.fixed_cd_track_count);
        music_track = retail_sound_options_configuration
                          .assigned_cd_tracks[assignment_index];
        if (!options.music_path.empty()) {
          std::ostringstream name;
          name << "track" << std::setw(2) << std::setfill('0')
               << static_cast<unsigned int>(music_track) << ".wav";
          const auto wav = options.music_path.parent_path() / name.str();
          if (std::filesystem::is_regular_file(wav)) {
            installed_track = wav;
          }
        }
      }
      if (installed_track.has_value()) {
        arguments.insert(arguments.end(),
                         {"--music", installed_track->string()});
        if (s40_race) {
          arguments.insert(arguments.end(),
                           {"--music-gain", std::to_string(s40_music_gain)});
        }
      } else if (!s40_race && options.mounted_cdda.has_value()) {
        arguments.insert(arguments.end(),
                         {"--music-cd-drive",
                          options.mounted_cdda->root.string(), "--music-track",
                          std::to_string(music_track)});
      } else if (!s40_race && !options.disc_cue_path.empty()) {
        if (!std::filesystem::is_regular_file(options.disc_cue_path)) {
          throw std::runtime_error("owned disc CUE was not found: " +
                                   options.disc_cue_path.string());
        }
        arguments.insert(arguments.end(),
                         {"--music-cue", options.disc_cue_path.string(),
                          "--music-track", std::to_string(music_track)});
      }
    }
    const auto sound_root = options.reference_root / "Sounds";
    if (options.audio && std::filesystem::is_directory(sound_root)) {
      arguments.insert(arguments.end(),
                       {"--engine-sounds", sound_root.string(), "--race-sounds",
                        sound_root.string(), "--horn-sound",
                        selected_horn.sample_path.string()});
    }
    if (options.embedded_race_smoke) {
      arguments.insert(arguments.end(), {"--frames", "1"});
    }
    std::vector<char *> mutable_arguments;
    mutable_arguments.reserve(arguments.size());
    for (auto &argument : arguments) {
      mutable_arguments.push_back(argument.data());
    }
    music.set_enabled(false);
    // Keep the original front-end window throughout the race. The selected
    // native API still receives its own SDL renderer, but replacing that
    // renderer in-place avoids a second title-bar/taskbar window appearing.
    texture.reset();
    widescreen_ambient_texture.reset();
    renderer.reset();
    std::optional<mh::ui::RaceResultEntry> completed_player_result;
    std::vector<mh::ui::RaceResultEntry> completed_results;
    mh::content::LeagueDivisionFinishingOrders completed_league_results;
    const auto result = mh::app::run_motorhead_race(
        static_cast<int>(mutable_arguments.size()), mutable_arguments.data(),
        window.get(), nullptr, &completed_player_result, &completed_results,
        selected_league.has_value() ? &completed_league_results : nullptr,
        multiplayer_session.state() == mh::network::SessionState::racing
            ? &multiplayer_session
            : nullptr,
        true);
    renderer.reset(
        create_front_end_renderer(window.get()));
    require(renderer != nullptr, "restore main-menu renderer");
    if (options.maximum_frames == 0U && !options.startup_movies_unpaced) {
      require(SDL_SetRenderVSync(renderer.get(), 1),
              "restore front-end vertical sync");
    }
    texture = make_frame_texture(renderer.get());
    widescreen_ambient_texture =
        make_widescreen_ambient_texture(renderer.get());
    widescreen_ambient_texture_ready = false;
    require(SDL_SetWindowTitle(window.get(), "Motorhead"),
            "restore front-end window title");
    const auto refreshed_graphics =
        load_graphic_options_configuration(options.user_data_root);
    state.configure_graphic_options(refreshed_graphics);
    apply_graphic_window_mode(window.get(), refreshed_graphics);
    require(
        SDL_SetRenderColorScale(renderer.get(), refreshed_graphics.brightness),
        "restore front-end Graphic Options brightness");
    retail_sound_options_configuration = load_sound_options_configuration(
        options.user_data_root, retail_race_setup_tracks,
        options.disc_cue_path);
    state.configure_sound_options(retail_sound_options_configuration);
    refresh_active_sound_options();
    apply_menu_audio_levels();
    if (multiplayer_session.state() == mh::network::SessionState::racing) {
      multiplayer_session.finish_race();
    }
    if (multiplayer_session.state() == mh::network::SessionState::lobby) {
      state.enter_multiplayer_lobby(multiplayer_session.is_host());
      sync_multiplayer_lobby();
      sync_multiplayer_configuration();
    }
    if (selected_league.has_value() && selected_league_path.has_value() &&
        selected_league_track_schedule.has_value() &&
        !completed_results.empty() &&
        is_reconstruction_owned_league(options.user_data_root,
                                       *selected_league_path)) {
      const auto progression = mh::content::apply_league_race_cycle(
          *selected_league, completed_league_results,
          static_cast<std::uint32_t>(selected_league_track_schedule->size()));
      if (progression.season_complete &&
          progression.original_result_code >= 1U &&
          progression.original_result_code <= 4U) {
        // The p3.1 result owner persists both layouts belonging to every
        // completed normal-track entry. Unlock derivation itself consumes the
        // forward FLGs, while retaining the reverse files preserves retail's
        // complete on-disk progression state.
        for (const auto track_index : *selected_league_track_schedule) {
          const auto write_flag = [&options](const RaceSetupTrackAsset &track) {
            const auto path =
                options.user_data_root /
                std::filesystem::path(track.definition.base_path) / "Data" /
                (track.name + ".FLG");
            mh::content::write_original_track_unlock_flag(path, track.name);
          };
          const auto &normal_track = race_setup_tracks.at(track_index);
          write_flag(normal_track);
          const auto reverse_source_number = normal_track.source_number + 8U;
          const auto reverse = std::find_if(
              race_setup_tracks.begin(), race_setup_tracks.end(),
              [reverse_source_number](const RaceSetupTrackAsset &candidate) {
                return candidate.source_number == reverse_source_number;
              });
          if (reverse != race_setup_tracks.end()) {
            write_flag(*reverse);
          }
        }

        unlock_tracks.clear();
        for (const auto &track : race_setup_tracks) {
          unlock_tracks.push_back(
              {track.definition.division,
               original_track_flag_available(options.user_data_root, track)});
        }
        // Assets are already in retail Division order, so only the recovered
        // bounds change here. Existing menu selections and player options stay
        // intact while newly earned content becomes available immediately.
        refresh_unlock_catalog();
      }
      auto completion_movie_result = StartupMovieResult::completed;
      if (progression.season_complete) {
        auto movies_path = options.movies_path;
        if (movies_path.empty()) {
          for (const auto &candidate :
               {options.reference_root / "MOVIES" / "MOVIES.PAK",
                options.reference_root / "Movies" / "MOVIES.PAK",
                options.reference_root / "MOVIES.PAK"}) {
            if (std::filesystem::is_regular_file(candidate)) {
              movies_path = candidate;
              break;
            }
          }
        }
        if (!std::filesystem::is_regular_file(movies_path)) {
          throw std::runtime_error(
              "League completion requires the original MOVIES.PAK");
        }
        completion_movie_result = play_startup_movie(
            window.get(), renderer.get(), movies_path,
            mh::content::original_league_completion_movie_entry(
                progression.original_result_code),
            options.audio, false, true, audio_output_name);
      }

      const auto league_index = static_cast<std::size_t>(state.league_index());
      if (progression.original_deletes_league_file) {
        // Retail deletes the champion LGF. The reconstruction removes it from
        // the active list but archives the validated save and its recovery
        // generation so a release upgrade can never destroy the achievement.
        archive_completed_league(options.user_data_root, *selected_league_path);
        league_overviews.erase(league_overviews.begin() +
                               static_cast<std::ptrdiff_t>(league_index));
        if (league_overviews.empty()) {
          hidden_league_names.clear();
          save_hidden_leagues(options.user_data_root, hidden_league_names);
          league_overviews = load_league_overview_presentations(
              options.reference_root, options.user_data_root,
              one_player_values);
          for (auto &overview : league_overviews) {
            refresh_league_overview(
                overview, mh::content::read_league(overview.source_path));
          }
        }
        state.configure_leagues(
            static_cast<std::uint32_t>(std::min<std::size_t>(
                league_index, league_overviews.size() - 1U)),
            static_cast<std::uint32_t>(league_overviews.size()));
      } else {
        mh::content::replace_league(*selected_league_path, *selected_league);
        refresh_league_overview(league_overviews.at(league_index),
                                *selected_league);
      }
      if (completion_movie_result == StartupMovieResult::quit) {
        require(SDL_SetWindowTitle(window.get(), "Motorhead"),
                "restore front-end window title");
        music.set_enabled(true);
        return 0;
      }
    }
    if (completed_player_result.has_value()) {
      const auto normal_track_index =
          (std::max<std::uint32_t>(selected_track.source_number, 1U) - 1U) %
          retail_normal_track_count;
      ranking_history.push_back(
          {static_cast<std::uint32_t>(normal_track_index),
           state.race_setup_laps(),
           state.difficulty(),
           selected_track.source_number > retail_normal_track_count,
           {completed_player_result->nickname,
            completed_player_result->car_name,
            completed_player_result->best_lap_ms,
            completed_player_result->race_time_ms}});
      // Bound the project-owned history without disturbing its stable order.
      if (ranking_history.size() > 1024U) {
        ranking_history.erase(
            ranking_history.begin(),
            ranking_history.begin() +
                static_cast<std::ptrdiff_t>(ranking_history.size() - 1024U));
      }
      save_rankings(options.user_data_root, ranking_history);
    }
    require(SDL_SetWindowTitle(window.get(), "Motorhead"),
            "restore front-end window title");
    music.set_enabled(true);
    previous_ticks = SDL_GetTicks();
    return result;
  };
  if (options.embedded_race_smoke) {
    return run_selected_race();
  }
  std::array<std::int8_t, 3U> menu_joystick_axis_direction{};
  const auto bind_joystick_control = [&](const std::string &binding) {
    const auto transition = state.bind_control_input(binding);
    audio.dispatch(transition);
    if (transition.control_settings_changed) {
      save_control_options_configuration(options.user_data_root,
                                         state.control_options_configuration());
    }
  };
  const auto push_menu_key = [](const SDL_Keycode key,
                                const SDL_Scancode scancode) {
    SDL_Event key_event{};
    key_event.type = SDL_EVENT_KEY_DOWN;
    key_event.key.key = key;
    key_event.key.scancode = scancode;
    key_event.key.down = true;
    key_event.key.repeat = false;
    static_cast<void>(SDL_PushEvent(&key_event));
  };
  while (running) {
    music.check();
    SDL_Event event{};
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT) {
        running = false;
      } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST &&
                 event.window.windowID == SDL_GetWindowID(window.get())) {
        front_end_focus_lost = true;
      } else if ((event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
                  event.type == SDL_EVENT_WINDOW_RESTORED) &&
                 event.window.windowID == SDL_GetWindowID(window.get())) {
        // Recreate the Direct3D 11 presentation owner after an actual focus
        // round trip. Some Windows drivers leave a previously occluded swap
        // chain presenting its last buffer even though the CPU menu continues
        // composing frames.
        front_end_renderer_recovery_pending = front_end_focus_lost;
        front_end_focus_lost = false;
      } else if ((event.type == SDL_EVENT_RENDER_DEVICE_RESET ||
                  event.type == SDL_EVENT_RENDER_DEVICE_LOST) &&
                 event.render.windowID == SDL_GetWindowID(window.get())) {
        // SDL has already discarded the D3D11 device resources. Streaming
        // textures created by the old renderer must not be reused.
        front_end_renderer_recovery_pending = true;
      } else if (event.type == SDL_EVENT_JOYSTICK_ADDED) {
        if (menu_joystick_enabled && menu_joystick == nullptr) {
          menu_joystick.reset(SDL_OpenJoystick(event.jdevice.which));
          if (menu_joystick != nullptr) {
            menu_joystick_id = SDL_GetJoystickID(menu_joystick.get());
            menu_joystick_axis_direction.fill(0);
          }
        }
      } else if (event.type == SDL_EVENT_JOYSTICK_REMOVED &&
                 event.jdevice.which == menu_joystick_id) {
        menu_joystick.reset();
        menu_joystick_id = 0U;
        menu_joystick_axis_direction.fill(0);
        open_first_menu_joystick();
      } else if (event.type == SDL_EVENT_JOYSTICK_AXIS_MOTION &&
                 menu_joystick_enabled &&
                 event.jaxis.which == menu_joystick_id &&
                 event.jaxis.axis < menu_joystick_axis_direction.size()) {
        const auto axis = static_cast<std::size_t>(event.jaxis.axis);
        const auto direction = static_cast<std::int8_t>(
            event.jaxis.value <
                    mh::platform::original_negative_joystick_threshold
                ? -1
            : event.jaxis.value >
                    mh::platform::original_positive_joystick_threshold
                ? 1
                : 0);
        const auto previous = menu_joystick_axis_direction[axis];
        menu_joystick_axis_direction[axis] = direction;
        if (direction != 0 && direction != previous &&
            !screen_transition.active() && !exit_confirmation_closing &&
            terminal_action == mh::ui::FrontEndAction::none) {
          if (state.screen() == mh::ui::FrontEndScreen::control_options &&
              state.control_binding_capture_active()) {
            const auto binding =
                axis == 0U   ? (direction < 0 ? "JoyLeft" : "JoyRight")
                : axis == 1U ? (direction < 0 ? "JoyUp" : "JoyDown")
                             : (direction < 0 ? "JoyIn" : "JoyOut");
            bind_joystick_control(binding);
          } else if (axis == 0U) {
            push_menu_key(direction < 0 ? SDLK_LEFT : SDLK_RIGHT,
                          direction < 0 ? SDL_SCANCODE_LEFT
                                        : SDL_SCANCODE_RIGHT);
          } else if (axis == 1U) {
            push_menu_key(direction < 0 ? SDLK_UP : SDLK_DOWN,
                          direction < 0 ? SDL_SCANCODE_UP : SDL_SCANCODE_DOWN);
          }
        }
      } else if (event.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN &&
                 menu_joystick_enabled &&
                 event.jbutton.which == menu_joystick_id &&
                 event.jbutton.button < 8U && !screen_transition.active() &&
                 !exit_confirmation_closing &&
                 terminal_action == mh::ui::FrontEndAction::none) {
        if (state.screen() == mh::ui::FrontEndScreen::control_options &&
            state.control_binding_capture_active()) {
          bind_joystick_control("JoyButton" +
                                std::to_string(event.jbutton.button + 1U));
        } else if (event.jbutton.button == 0U) {
          push_menu_key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        } else if (event.jbutton.button == 1U) {
          push_menu_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
        }
      } else if (event.type == SDL_EVENT_TEXT_INPUT) {
        if (!screen_transition.active() && !exit_confirmation_closing &&
            terminal_action == mh::ui::FrontEndAction::none) {
          if (state.screen() == mh::ui::FrontEndScreen::personal_options &&
              state.append_personal_text(event.text.text)) {
            save_personal_options_and_refresh_unlocks();
          } else if (state.screen() == mh::ui::FrontEndScreen::multiplayer) {
            (void)state.append_multiplayer_text(event.text.text);
          } else if (state.screen() == mh::ui::FrontEndScreen::league_create &&
                     state.league_create_field() ==
                         mh::ui::LeagueCreateField::name &&
                     !state.league_create_confirmation_open()) {
            for (const auto character : std::string_view(event.text.text)) {
              const auto byte = static_cast<unsigned char>(character);
              if (byte >= 32U && byte <= 126U &&
                  league_draft.league_name.size() < 31U) {
                league_draft.league_name.push_back(character);
              }
            }
          }
        }
      } else if (event.type == SDL_EVENT_KEY_DOWN &&
                 (!event.key.repeat ||
                  (state.screen() == mh::ui::FrontEndScreen::personal_options &&
                   event.key.key == SDLK_BACKSPACE) ||
                  (state.screen() == mh::ui::FrontEndScreen::multiplayer &&
                   event.key.key == SDLK_BACKSPACE) ||
                  (state.screen() == mh::ui::FrontEndScreen::sound_options &&
                   (state.sound_options_field() ==
                        mh::ui::SoundOptionsField::sound_effects_volume ||
                    state.sound_options_field() ==
                        mh::ui::SoundOptionsField::cd_music_volume) &&
                   (event.key.key == SDLK_LEFT ||
                    event.key.key == SDLK_RIGHT)) ||
                  (state.screen() == mh::ui::FrontEndScreen::control_options &&
                   state.control_options_focus() ==
                       mh::ui::ControlOptionsFocus::field_list &&
                   state.control_options_field() >=
                       mh::ui::ControlOptionsField::mouse_speed_x &&
                   state.control_options_field() <=
                       mh::ui::ControlOptionsField::mouse_speed_z &&
                   (event.key.key == SDLK_LEFT ||
                    event.key.key == SDLK_RIGHT)) ||
                  (state.screen() == mh::ui::FrontEndScreen::graphic_options &&
                   (state.graphic_options_field() ==
                        mh::ui::GraphicOptionsField::brightness ||
                    state.graphic_options_field() ==
                        mh::ui::GraphicOptionsField::view_distance) &&
                   (event.key.key == SDLK_LEFT ||
                    event.key.key == SDLK_RIGHT)) ||
                  (state.screen() == mh::ui::FrontEndScreen::gameplay_options &&
                   (state.gameplay_options_field() ==
                        mh::ui::GameplayOptionsField::checkpoint_display_time ||
                    state.gameplay_options_field() ==
                        mh::ui::GameplayOptionsField::ui_scale) &&
                   (event.key.key == SDLK_LEFT ||
                    event.key.key == SDLK_RIGHT)))) {
        if (screen_transition.active() || exit_confirmation_closing ||
            terminal_action != mh::ui::FrontEndAction::none) {
          continue;
        }
        if (state.screen() == mh::ui::FrontEndScreen::personal_options &&
            event.key.key == SDLK_BACKSPACE) {
          if (state.backspace_personal_text()) {
            save_personal_options_and_refresh_unlocks();
          }
          continue;
        }
        if (state.screen() == mh::ui::FrontEndScreen::multiplayer &&
            event.key.key == SDLK_BACKSPACE) {
          (void)state.backspace_multiplayer_text();
          continue;
        }
        if (state.screen() == mh::ui::FrontEndScreen::multiplayer &&
            state.multiplayer_page() == mh::ui::MultiplayerPage::lobby) {
          const auto update_host_configuration = [&]() {
            const auto mode =
                multiplayer_session.pending_race_configuration().has_value()
                    ? multiplayer_session.pending_race_configuration()->mode
                    : 0U;
            (void)multiplayer_session.update_race_configuration(
                {state.race_setup_track_index(), state.race_setup_laps(), mode,
                 static_cast<std::uint32_t>(SDL_GetTicks())});
            sync_multiplayer_configuration();
          };
          if (event.key.key == SDLK_F1 && multiplayer_session.is_host()) {
            state.configure_race_setup(
                {mh::ui::RaceSetupMode::single_race,
                 (state.race_setup_track_index() + 1U) %
                     static_cast<std::uint32_t>(race_setup_tracks.size()),
                 static_cast<std::uint32_t>(race_setup_tracks.size()),
                 static_cast<std::uint32_t>(race_setup_tracks.size()),
                 state.race_setup_laps(), false});
            update_host_configuration();
            continue;
          }
          if ((event.key.key == SDLK_F2 || event.key.key == SDLK_F3) &&
              multiplayer_session.is_host()) {
            constexpr std::array<std::uint32_t, 6U> laps{1U,  3U,  5U,
                                                         10U, 15U, 25U};
            const auto current =
                std::find(laps.begin(), laps.end(), state.race_setup_laps());
            auto index = current == laps.end()
                             ? 2U
                             : static_cast<std::size_t>(
                                   std::distance(laps.begin(), current));
            if (event.key.key == SDLK_F2) {
              index = index == 0U ? laps.size() - 1U : index - 1U;
            } else {
              index = (index + 1U) % laps.size();
            }
            state.configure_race_setup(
                {mh::ui::RaceSetupMode::single_race,
                 state.race_setup_track_index(),
                 static_cast<std::uint32_t>(race_setup_tracks.size()),
                 static_cast<std::uint32_t>(race_setup_tracks.size()),
                 laps[index], false});
            update_host_configuration();
            continue;
          }
          if (event.key.key == SDLK_F5 && multiplayer_session.is_host()) {
            const auto mode =
                multiplayer_session.pending_race_configuration().has_value()
                    ? multiplayer_session.pending_race_configuration()->mode
                    : 0U;
            if (!multiplayer_session.start_race(
                    {state.race_setup_track_index(), state.race_setup_laps(),
                     mode, static_cast<std::uint32_t>(SDL_GetTicks())})) {
              state.set_multiplayer_status(
                  "All players must be ready before starting");
            }
            continue;
          }
          if (event.key.key == SDLK_F4 && multiplayer_session.is_host()) {
            (void)multiplayer_session.clear_scores();
            sync_multiplayer_lobby();
            continue;
          }
          if (event.key.key == SDLK_F6) {
            const auto next = (state.car_setup_car_index() + 1U) %
                              state.car_setup_car_count();
            state.configure_car_setup({next, state.car_setup_car_count(),
                                       state.car_setup_unlocked_car_count(),
                                       state.car_setup_automatic_transmission(),
                                       state.car_setup_horn_index(),
                                       state.car_setup_horn_count(),
                                       state.car_setup_record_race()});
            (void)multiplayer_session.update_player(
                state.personal_options_configuration().player_name,
                static_cast<std::uint8_t>(next));
            (void)multiplayer_session.update_player_presentation(
                state.personal_options_configuration().team_name,
                state.car_setup_automatic_transmission(),
                car_setup_assets[next].name, multiplayer_player_colour());
            sync_multiplayer_configuration();
            sync_multiplayer_lobby();
            continue;
          }
          if (event.key.key == SDLK_F7) {
            state.configure_car_setup(
                {state.car_setup_car_index(), state.car_setup_car_count(),
                 state.car_setup_unlocked_car_count(),
                 !state.car_setup_automatic_transmission(),
                 state.car_setup_horn_index(), state.car_setup_horn_count(),
                 state.car_setup_record_race()});
            (void)multiplayer_session.update_player_presentation(
                state.personal_options_configuration().team_name,
                state.car_setup_automatic_transmission(),
                car_setup_assets[state.car_setup_car_index()].name,
                multiplayer_player_colour());
            sync_multiplayer_configuration();
            sync_multiplayer_lobby();
            continue;
          }
          if (event.key.key == SDLK_F8 && multiplayer_session.is_host()) {
            const auto current =
                multiplayer_session.pending_race_configuration().has_value()
                    ? multiplayer_session.pending_race_configuration()->mode
                    : 0U;
            (void)multiplayer_session.update_race_configuration(
                {state.race_setup_track_index(), state.race_setup_laps(),
                 current == 0U ? 1U : 0U,
                 static_cast<std::uint32_t>(SDL_GetTicks())});
            sync_multiplayer_configuration();
            continue;
          }
        }
        if (state.screen() == mh::ui::FrontEndScreen::league_create &&
            state.league_create_field() == mh::ui::LeagueCreateField::name &&
            !state.league_create_confirmation_open() &&
            event.key.key == SDLK_BACKSPACE) {
          if (!league_draft.league_name.empty()) {
            league_draft.league_name.pop_back();
          }
          continue;
        }
        if (state.screen() == mh::ui::FrontEndScreen::control_options &&
            state.control_binding_capture_active()) {
          const auto binding = retail_control_key_name(event.key.key);
          const auto transition = state.bind_control_input(binding);
          audio.dispatch(transition);
          if (transition.control_settings_changed) {
            save_control_options_configuration(
                options.user_data_root, state.control_options_configuration());
          }
          continue;
        }
        const auto previous_screen = state.screen();
        const auto previous_exit_confirmation = state.exit_confirmation_open();
        const auto previous_exit_confirmation_choice =
            state.exit_confirmation_choice();
        const auto previous_league_create_confirmation =
            state.league_create_confirmation_open();
        const auto previous_league_create_confirmation_choice =
            state.league_create_confirmation_choice();
        const auto previous_league_delete_confirmation =
            state.league_delete_confirmation_open();
        const auto previous_league_delete_confirmation_choice =
            state.league_delete_confirmation_choice();
        const auto previous_league_index = state.league_index();
        mh::ui::MenuTransitionResult transition;
        if (event.key.key == SDLK_LEFT) {
          transition = state.navigate(mh::ui::MenuDirection::left);
        } else if (event.key.key == SDLK_RIGHT) {
          transition = state.navigate(mh::ui::MenuDirection::right);
        } else if (event.key.key == SDLK_UP) {
          transition = state.navigate(mh::ui::MenuDirection::up);
        } else if (event.key.key == SDLK_DOWN) {
          transition = state.navigate(mh::ui::MenuDirection::down);
        } else if (event.key.key == SDLK_RETURN ||
                   event.key.key == SDLK_KP_ENTER) {
          if (state.screen() == mh::ui::FrontEndScreen::multiplayer &&
              state.multiplayer_page() == mh::ui::MultiplayerPage::lobby &&
              !state.multiplayer_configuration().lobby_chat_input.empty()) {
            if (multiplayer_session.send_lobby_message(
                    state.multiplayer_configuration().lobby_chat_input)) {
              state.clear_multiplayer_chat_input();
              transition.evidence =
                  mh::ui::MenuTransitionEvidence::confirmed_change;
              transition.sound_events[0U] = mh::ui::MenuSoundEvent::select;
              transition.sound_event_count = 1U;
            } else {
              transition.evidence =
                  mh::ui::MenuTransitionEvidence::confirmed_no_change;
              transition.sound_events[0U] =
                  mh::ui::MenuSoundEvent::select_error;
              transition.sound_event_count = 1U;
            }
          } else if (state.screen() == mh::ui::FrontEndScreen::league_create &&
                     !state.league_create_confirmation_open() &&
                     trim_copy(league_draft.league_name).empty()) {
            transition.evidence =
                mh::ui::MenuTransitionEvidence::confirmed_no_change;
            transition.sound_events[0U] = mh::ui::MenuSoundEvent::select_error;
            transition.sound_event_count = 1U;
          } else {
            transition = state.confirm();
          }
        } else if (event.key.key == SDLK_DELETE &&
                   state.screen() == mh::ui::FrontEndScreen::ghost_setup &&
                   state.ghost_file_focused() && !ghost_demo_files.empty()) {
          const auto deleted_index =
              static_cast<std::size_t>(state.ghost_file_index());
          const auto deleted_file = ghost_demo_files.at(deleted_index);
          transition = state.delete_current_ghost_file();
          const auto already_hidden = std::any_of(
              hidden_ghost_demo_files.begin(), hidden_ghost_demo_files.end(),
              [&deleted_file](const std::string &hidden) {
                return ascii_equal_case_insensitive(hidden, deleted_file);
              });
          if (!already_hidden) {
            hidden_ghost_demo_files.push_back(deleted_file);
          }
          save_hidden_ghost_demos(options.user_data_root,
                                  hidden_ghost_demo_files);
          ghost_demo_files.erase(ghost_demo_files.begin() +
                                 static_cast<std::ptrdiff_t>(deleted_index));
        } else if (event.key.key == SDLK_ESCAPE) {
          transition = state.cancel();
        }
        audio.dispatch(transition);
        if (state.screen() == mh::ui::FrontEndScreen::league_create &&
            !league_create_divisions.empty()) {
          league_draft.division =
              league_create_divisions.at(
                  state.league_create_division_index());
        }
        if (transition.multiplayer_refresh) {
          if (multiplayer_session.browse(SDL_GetTicks())) {
            state.set_multiplayer_status("Searching for LAN sessions...");
          } else {
            state.set_multiplayer_status(multiplayer_session.last_error());
          }
        }
        if (transition.multiplayer_host) {
          const auto &configuration = state.multiplayer_configuration();
          const auto &personal = state.personal_options_configuration();
          mh::network::HostOptions host_options;
          host_options.session_name = configuration.session_name;
          host_options.password = configuration.session_password;
          host_options.player_name = personal.player_name;
          host_options.car_selection =
              static_cast<std::uint8_t>(state.car_setup_car_index());
          host_options.team = personal.team_name;
          host_options.automatic_gear =
              state.car_setup_automatic_transmission();
          host_options.car_name =
              car_setup_assets[state.car_setup_car_index()].name;
          host_options.colour = multiplayer_player_colour();
          if (multiplayer_session.host(host_options, SDL_GetTicks())) {
            state.enter_multiplayer_lobby(true);
            (void)multiplayer_session.update_race_configuration(
                {state.race_setup_track_index(), state.race_setup_laps(), 0U,
                 static_cast<std::uint32_t>(SDL_GetTicks())});
            sync_multiplayer_lobby();
            sync_multiplayer_configuration();
          } else {
            state.set_multiplayer_status(multiplayer_session.last_error());
          }
        }
        if (transition.multiplayer_join) {
          const auto &configuration = state.multiplayer_configuration();
          auto address = configuration.address;
          auto port = mh::network::lan_session_port;
          if (!configuration.sessions.empty() &&
              configuration.selected_session < configuration.sessions.size()) {
            address =
                configuration.sessions[configuration.selected_session].address;
            const auto found = std::find_if(
                multiplayer_session.sessions().begin(),
                multiplayer_session.sessions().end(),
                [&address](const mh::network::SessionInfo &session) {
                  return session.address == address;
                });
            if (found != multiplayer_session.sessions().end()) {
              port = found->port;
            }
          }
          mh::network::JoinOptions join_options;
          join_options.address = address;
          join_options.password = configuration.session_password;
          join_options.player_name =
              state.personal_options_configuration().player_name;
          join_options.car_selection =
              static_cast<std::uint8_t>(state.car_setup_car_index());
          join_options.port = port;
          join_options.team = state.personal_options_configuration().team_name;
          join_options.automatic_gear =
              state.car_setup_automatic_transmission();
          join_options.car_name =
              car_setup_assets[state.car_setup_car_index()].name;
          join_options.colour = multiplayer_player_colour();
          if (multiplayer_session.join(join_options, SDL_GetTicks())) {
            state.set_multiplayer_status("Trying to connect...");
          } else {
            state.set_multiplayer_status(multiplayer_session.last_error());
          }
        }
        if (transition.multiplayer_ready) {
          (void)multiplayer_session.set_ready(
              !state.multiplayer_configuration().local_ready);
        }
        if (transition.multiplayer_start) {
          const auto mode =
              multiplayer_session.pending_race_configuration().has_value()
                  ? multiplayer_session.pending_race_configuration()->mode
                  : 0U;
          const mh::network::RaceConfiguration configuration{
              state.race_setup_track_index(), state.race_setup_laps(), mode,
              static_cast<std::uint32_t>(SDL_GetTicks())};
          if (!multiplayer_session.start_race(configuration)) {
            state.set_multiplayer_status(
                "All players must be ready before starting");
          }
        }
        if (transition.multiplayer_leave) {
          multiplayer_session.disconnect("left session");
        }
        if (transition.create_league) {
          auto created = create_reconstruction_league(
              options.reference_root, options.user_data_root, league_draft,
              one_player_values, state.personal_options_configuration());
          refresh_league_overview(
              created, mh::content::read_league(created.source_path));
          league_overviews.push_back(created);
        }
        if (transition.delete_league) {
          const auto deleted_index =
              static_cast<std::size_t>(previous_league_index);
          const auto deleted_name =
              league_overviews.at(deleted_index).league_name;
          const auto already_hidden = std::any_of(
              hidden_league_names.begin(), hidden_league_names.end(),
              [&deleted_name](const std::string &hidden) {
                return ascii_equal_case_insensitive(hidden, deleted_name);
              });
          if (!already_hidden) {
            hidden_league_names.push_back(deleted_name);
          }
          save_hidden_leagues(options.user_data_root, hidden_league_names);
          league_overviews.erase(league_overviews.begin() +
                                 static_cast<std::ptrdiff_t>(deleted_index));
        }
        const auto previous_confirmation =
            previous_exit_confirmation || previous_league_create_confirmation ||
            previous_league_delete_confirmation;
        const auto confirmation_open =
            state.exit_confirmation_open() ||
            state.league_create_confirmation_open() ||
            state.league_delete_confirmation_open();
        const auto previous_confirmation_choice =
            previous_exit_confirmation
                ? previous_exit_confirmation_choice
                : (previous_league_create_confirmation
                       ? previous_league_create_confirmation_choice
                       : previous_league_delete_confirmation_choice);
        if (!previous_confirmation && confirmation_open) {
          exit_confirmation_opened_ticks = SDL_GetTicks();
          exit_confirmation_closing = false;
        } else if (previous_confirmation && !confirmation_open) {
          exit_confirmation_closed_ticks = SDL_GetTicks();
          exit_confirmation_closing = true;
          closing_exit_confirmation_choice = previous_confirmation_choice;
        }
        if (transition.sound_settings_changed) {
          const auto &sound = state.sound_options_configuration();
          if (sound.output_device_name() != audio_output_name) {
            const auto selected_output = std::string(sound.output_device_name());
            auto next_audio = MenuAudio(sound_directory, options.audio,
                                         selected_output);
            next_audio.set_volume(sound.sound_effects_volume);
            audio = std::move(next_audio);
            horn_audio = HornPreviewAudio(options.audio, selected_output);
            audio_output_name = selected_output;
            music.set_output(audio_output_name);
            play_active_menu_music();
            preview_music_active = false;
          }
          apply_menu_audio_levels();
          if (preview_music_active) {
            music.set_looping(sound.cd_loop);
          }
          if (s40_presentation_active) {
            retail_sound_options_configuration.output_devices =
                sound.output_devices;
            retail_sound_options_configuration.output_device_index =
                sound.output_device_index;
            retail_sound_options_configuration.sound_effects_volume =
                sound.sound_effects_volume;
            retail_sound_options_configuration.cd_music_volume =
                sound.cd_music_volume;
            retail_sound_options_configuration.cd_loop = sound.cd_loop;
            save_sound_options_configuration(
                options.user_data_root, retail_race_setup_tracks,
                retail_sound_options_configuration);
            if (s40_progression_reward) {
              save_s40_sound_assignments(options.user_data_root, sound);
            }
            sound_options_presentation = s40_sound_options_presentation(
                options.reference_root / "Music", sound);
          } else {
            retail_sound_options_configuration = sound;
            save_sound_options_configuration(options.user_data_root,
                                             retail_race_setup_tracks, sound);
            sound_options_presentation = make_sound_options_presentation(
                retail_race_setup_tracks, options.disc_cue_path,
                options.music_path.parent_path(), options.mounted_cdda,
                sound);
          }
        }
        if (transition.personal_settings_changed) {
          save_personal_options_and_refresh_unlocks();
        }
        if (transition.control_settings_changed) {
          save_control_options_configuration(
              options.user_data_root, state.control_options_configuration());
          control_settings_dirty = false;
        }
        if (transition.graphic_settings_changed) {
          save_graphic_options_configuration(
              options.user_data_root, state.graphic_options_configuration());
          apply_graphic_screen_size();
          graphic_settings_dirty = false;
        }
        if (transition.screen_changed &&
            previous_screen == mh::ui::FrontEndScreen::control_options &&
            control_settings_dirty) {
          save_control_options_configuration(
              options.user_data_root, state.control_options_configuration());
          control_settings_dirty = false;
        }
        if (transition.screen_changed &&
            (previous_screen == mh::ui::FrontEndScreen::graphic_options ||
             previous_screen == mh::ui::FrontEndScreen::gameplay_options) &&
            graphic_settings_dirty) {
          save_graphic_options_configuration(
              options.user_data_root, state.graphic_options_configuration());
          graphic_settings_dirty = false;
        }
        if (transition.screen_changed) {
          apply_s40_asset_selection(
              s40_presentation_active && s40_racing_uses_special_assets(
                                             state.screen(),
                                             state.race_setup_mode()));
        }
        if (transition.preview_music) {
          if (s40_presentation_active) {
            const auto &sound = state.sound_options_configuration();
            const auto row = std::min<std::size_t>(sound.song_index, 2U);
            const auto track = row < sound.assigned_cd_tracks.size()
                                   ? sound.assigned_cd_tracks[row]
                                   : static_cast<std::uint8_t>(row + 2U);
            music.play_file(s40_audio_track_path(
                                options.reference_root / "Music", track),
                            sound.cd_loop, s40_music_gain);
            music.set_volume(sound.cd_music_volume);
            preview_music_active = true;
          } else if (options.mounted_cdda.has_value() ||
                     !options.music_path.empty() ||
                     !options.disc_cue_path.empty()) {
            const auto &sound = state.sound_options_configuration();
            const auto track = sound.assigned_cd_tracks.at(sound.song_index);
            music.play_track(track, sound.cd_loop);
            music.set_volume(sound.cd_music_volume);
            preview_music_active = true;
          }
        }
        if (transition.screen_changed &&
            state.screen() == mh::ui::FrontEndScreen::sound_options) {
          auto sound = state.sound_options_configuration();
          const auto selected_output = std::string(sound.output_device_name());
          sound.output_devices = mh::platform::audio_output_names();
          const auto selected = std::find(sound.output_devices.begin(),
                                          sound.output_devices.end(),
                                          selected_output);
          sound.output_device_index =
              selected == sound.output_devices.end()
                  ? 0U
                  : static_cast<std::uint32_t>(selected - sound.output_devices.begin());
          state.configure_sound_options(std::move(sound));
        }
        if (preview_music_active && transition.screen_changed &&
            previous_screen == mh::ui::FrontEndScreen::sound_options &&
            state.screen() != mh::ui::FrontEndScreen::sound_options) {
          // A Sound Options selection is only a page-local preview. Restore
          // the active retail or S40 menu track as soon as the page closes.
          play_active_menu_music();
          music.set_volume(state.sound_options_configuration().cd_music_volume);
          preview_music_active = false;
        }
        if (transition.preview_horn) {
          horn_audio.play(
              horn_setup_assets.at(state.car_setup_horn_index()).sample_path);
        }
        if (transition.screen_changed &&
            previous_screen == mh::ui::FrontEndScreen::league_summary &&
            state.screen() == mh::ui::FrontEndScreen::car_setup &&
            state.race_setup_mode() == mh::ui::RaceSetupMode::league_race) {
          const auto &overview =
              league_overviews.at(static_cast<std::size_t>(state.league_index()));
          const auto league = mh::content::read_league(overview.source_path);
          const auto human = std::find_if(
              league.players.begin(), league.players.end(),
              [](const mh::content::LeaguePlayer &player) {
                return player.human;
              });
          if (human == league.players.end() ||
              human->division >= human->division_cars.size()) {
            throw std::runtime_error(
                "selected league has no valid human vehicle");
          }
          const auto active_car = human->division_cars[human->division];
          const auto car = std::find_if(
              car_setup_assets.begin(), car_setup_assets.end(),
              [&active_car](const CarSetupAsset &candidate) {
                return ascii_equal_case_insensitive(candidate.name,
                                                    active_car);
              });
          const auto horn = std::find_if(
              horn_setup_assets.begin(), horn_setup_assets.end(),
              [&human](const HornSetupAsset &candidate) {
                return ascii_equal_case_insensitive(candidate.name,
                                                    human->horn);
              });
          if (car == car_setup_assets.end() ||
              horn == horn_setup_assets.end()) {
            throw std::runtime_error(
                "selected league vehicle is unavailable in car setup");
          }
          state.configure_car_setup(
              {static_cast<std::uint32_t>(
                   std::distance(car_setup_assets.begin(), car)),
               state.car_setup_car_count(),
               state.car_setup_unlocked_car_count(),
               state.car_setup_automatic_transmission(),
               static_cast<std::uint32_t>(
                   std::distance(horn_setup_assets.begin(), horn)),
               state.car_setup_horn_count(), state.car_setup_record_race()});
        }
        if (transition.screen_changed &&
            (state.screen() == mh::ui::FrontEndScreen::race_setup ||
             previous_screen == mh::ui::FrontEndScreen::race_setup)) {
          one_player_values.mode =
              state.race_setup_mode() == mh::ui::RaceSetupMode::time_attack
                  ? mh::ui::OnePlayerMode::time_attack
                  : mh::ui::OnePlayerMode::single_race;
          one_player_values.third_value =
              std::to_string(state.race_setup_laps());
          one_player_values.track =
              race_setup_tracks[state.race_setup_track_index()].name;
        }
        if (state.screen() == mh::ui::FrontEndScreen::car_setup ||
            previous_screen == mh::ui::FrontEndScreen::car_setup) {
          one_player_values.car =
              car_setup_assets[state.car_setup_car_index()].name;
          one_player_values.transmission =
              state.car_setup_automatic_transmission() ? "Auto" : "Manual";
        }
        if (transition.screen_changed &&
            previous_screen == mh::ui::FrontEndScreen::car_setup &&
            state.screen() == mh::ui::FrontEndScreen::league_summary &&
            state.race_setup_mode() == mh::ui::RaceSetupMode::league_race) {
          auto &overview =
              league_overviews.at(static_cast<std::size_t>(state.league_index()));
          auto league = mh::content::read_league(overview.source_path);
          const auto selected_car =
              car_setup_assets.at(state.car_setup_car_index()).name;
          const auto selected_horn =
              horn_setup_assets.at(state.car_setup_horn_index()).name;
          const auto human = std::find_if(
              league.players.begin(), league.players.end(),
              [](const mh::content::LeaguePlayer &player) {
                return player.human;
              });
          if (human == league.players.end() ||
              human->division >= human->division_cars.size()) {
            throw std::runtime_error(
                "selected league has no valid human vehicle");
          }
          const auto vehicle_changed =
              !ascii_equal_case_insensitive(
                  human->division_cars[human->division], selected_car) ||
              !ascii_equal_case_insensitive(human->horn, selected_horn);
          if (vehicle_changed) {
            if (!is_reconstruction_owned_league(options.user_data_root,
                                                overview.source_path)) {
              reset_league_progress(league);
              apply_player_to_league(
                  league, one_player_values,
                  state.personal_options_configuration());
              const auto owned_path = next_reconstruction_league_path(
                  options.user_data_root, league.name);
              mh::content::set_league_human_vehicle(
                  league, selected_car, selected_horn);
              mh::content::write_league(owned_path, league);
              overview.source_path = owned_path.string();
            } else {
              mh::content::set_league_human_vehicle(
                  league, selected_car, selected_horn);
              mh::content::replace_league(overview.source_path, league);
            }
            refresh_league_overview(overview, league);
          }
        }
        if (state.screen() == mh::ui::FrontEndScreen::ghost_setup ||
            previous_screen == mh::ui::FrontEndScreen::ghost_setup) {
          one_player_values.mode = mh::ui::OnePlayerMode::ghost_mode;
          one_player_values.third_value =
              ghost_demo_files.empty()
                  ? "-"
                  : std::filesystem::path(
                        ghost_demo_files.at(state.ghost_file_index()))
                        .stem()
                        .string();
        }
        auto s40_intro_handoff = false;
        if (transition.screen_changed &&
            previous_screen == mh::ui::FrontEndScreen::personal_options &&
            s40_intro_pending && s40_mode) {
          if (!std::filesystem::is_regular_file(s40_intro_movies)) {
            throw std::runtime_error(
                "installed S40 Racing content is missing its intro movie");
          }
          music.stop();
          const auto movie_result = play_startup_movie(
              window.get(), renderer.get(), s40_intro_movies, 0U,
              options.audio, false, false, audio_output_name);
          if (movie_result == StartupMovieResult::quit) {
            return 0;
          }
          state.show_main_menu_first_choice();
          main_pointer_units =
              mh::ui::main_menu_pointer_target_units(state.main_choice());
          main_glitch_state = {};
          background_animation_state = {};
          s40_presentation_active = true;
          state.configure_one_player_league_available(false);
          refresh_active_sound_options();
          apply_s40_asset_selection(
              s40_presentation_active && s40_racing_uses_special_assets(
                                             state.screen(),
                                             state.race_setup_mode()));
          s40_intro_pending = false;
          play_active_menu_music();
          music.set_volume(
              state.sound_options_configuration().cd_music_volume);
          s40_intro_handoff = true;
        }
        if (transition.action != mh::ui::FrontEndAction::none) {
          const auto from_one_player =
              previous_screen == mh::ui::FrontEndScreen::one_player;
          const auto quick_race =
              from_one_player &&
              state.one_player_choice() == mh::ui::OnePlayerChoice::quick_race;
          if (transition.action == mh::ui::FrontEndAction::start_race) {
            log_race_selection(state, race_setup_tracks, car_setup_assets,
                               quick_race);
          }
          terminal_action = transition.action;
          terminal_quick_race = quick_race;
          terminal_transition_ms = mh::ui::front_end_transition_leg_ms;
        } else if (transition.screen_changed && !s40_intro_handoff) {
          if (state.screen() == mh::ui::FrontEndScreen::league_create) {
            state.configure_league_create_divisions(
                0U,
                static_cast<std::uint32_t>(league_create_divisions.size()));
            league_draft = league_overviews.at(
                static_cast<std::size_t>(previous_league_index));
            league_draft.league_name = "League";
            league_draft.division = league_create_divisions.front();
            league_draft.races_done = 0U;
            league_draft.score = 0U;
          }
          screen_transition.begin(previous_screen, state.screen());
        }
      }
    }

    if (front_end_renderer_recovery_pending) {
      texture.reset();
      widescreen_ambient_texture.reset();
      renderer.reset();
      renderer.reset(
          create_front_end_renderer(window.get()));
      require(renderer != nullptr,
              "recover main-menu renderer after focus change");
      if (options.maximum_frames == 0U && !options.startup_movies_unpaced) {
        require(SDL_SetRenderVSync(renderer.get(), 1),
                "recover front-end vertical sync");
      }
      require(
          SDL_SetRenderColorScale(
              renderer.get(), state.graphic_options_configuration().brightness),
          "recover front-end Graphic Options brightness");
      texture = make_frame_texture(renderer.get());
      widescreen_ambient_texture =
          make_widescreen_ambient_texture(renderer.get());
      widescreen_ambient_texture_ready = false;
      previous_ticks = SDL_GetTicks();
      front_end_renderer_recovery_pending = false;
    }

    const auto ticks = SDL_GetTicks();
    multiplayer_session.tick(ticks);
    for (const auto &network_event : multiplayer_session.take_events()) {
      switch (network_event.kind) {
      case mh::network::SessionEventKind::sessions_changed:
        sync_multiplayer_sessions();
        state.set_multiplayer_status(multiplayer_session.sessions().empty()
                                         ? "No sessions found"
                                         : "Select a session and press Return");
        break;
      case mh::network::SessionEventKind::connected:
        state.enter_multiplayer_lobby(multiplayer_session.is_host());
        sync_multiplayer_lobby();
        sync_multiplayer_configuration();
        break;
      case mh::network::SessionEventKind::player_joined:
      case mh::network::SessionEventKind::player_changed:
      case mh::network::SessionEventKind::player_left:
      case mh::network::SessionEventKind::ready_changed:
        sync_multiplayer_lobby();
        break;
      case mh::network::SessionEventKind::lobby_message_received: {
        const auto sender = std::find_if(
            multiplayer_session.players().begin(),
            multiplayer_session.players().end(),
            [&network_event](const mh::network::LobbyPlayer &player) {
              return player.peer == network_event.peer;
            });
        const auto sender_name = sender == multiplayer_session.players().end()
                                     ? std::string("Player")
                                     : sender->name;
        const auto sender_colour = sender == multiplayer_session.players().end()
                                       ? 0x00e06820U
                                       : sender->colour;
        state.add_multiplayer_chat_line(
            sender_name + ": " + network_event.message, sender_colour);
        break;
      }
      case mh::network::SessionEventKind::configuration_changed:
        sync_multiplayer_configuration();
        break;
      case mh::network::SessionEventKind::race_started:
        if (const auto configuration =
                multiplayer_session.pending_race_configuration();
            configuration.has_value()) {
          state.configure_race_setup(
              {mh::ui::RaceSetupMode::single_race, configuration->track_index,
               static_cast<std::uint32_t>(race_setup_tracks.size()),
               static_cast<std::uint32_t>(race_setup_tracks.size()),
               configuration->lap_count, false});
        }
        sync_multiplayer_lobby();
        multiplayer_launch_pending = true;
        break;
      case mh::network::SessionEventKind::race_returned:
        state.enter_multiplayer_lobby(multiplayer_session.is_host());
        sync_multiplayer_lobby();
        sync_multiplayer_configuration();
        break;
      case mh::network::SessionEventKind::results_received:
        break;
      case mh::network::SessionEventKind::connection_denied:
        if (network_event.message == "wrong password") {
          state.prompt_multiplayer_password();
        }
        state.set_multiplayer_status(network_event.message);
        break;
      case mh::network::SessionEventKind::timed_out:
      case mh::network::SessionEventKind::error:
        state.set_multiplayer_status(network_event.message);
        break;
      case mh::network::SessionEventKind::disconnected:
        state.leave_multiplayer_lobby();
        state.set_multiplayer_status(network_event.message);
        break;
      }
    }
    if (multiplayer_launch_pending &&
        terminal_action == mh::ui::FrontEndAction::none) {
      terminal_action = mh::ui::FrontEndAction::start_race;
      terminal_quick_race = false;
      terminal_transition_ms = mh::ui::front_end_transition_leg_ms;
      multiplayer_launch_pending = false;
    }
    const auto elapsed = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        ticks - previous_ticks, std::numeric_limits<std::uint32_t>::max()));
    previous_ticks = ticks;
    mh::ui::advance_front_end_background_animation(background_animation_state,
                                                   elapsed);
    const auto visible_screen = screen_transition.active()
                                    ? screen_transition.visible_screen()
                                    : state.screen();
    auto displayed_one_player_values = one_player_values;
    if (s40_presentation_active && s40_car_setup_asset.has_value() &&
        !s40_race_setup_tracks.empty()) {
      const auto track_index = s40_assets_active
                                   ? state.race_setup_track_index()
                                   : s40_track_selection;
      displayed_one_player_values.mode =
          mh::ui::OnePlayerMode::single_race;
      displayed_one_player_values.car = s40_car_setup_asset->name;
      displayed_one_player_values.third_value =
          std::to_string(state.race_setup_laps());
      displayed_one_player_values.track =
          s40_race_setup_tracks.at(std::min<std::size_t>(
                                    track_index,
                                    s40_race_setup_tracks.size() - 1U))
              .name;
    }
    const auto static_selector_background =
        visible_screen == mh::ui::FrontEndScreen::race_setup ||
        visible_screen == mh::ui::FrontEndScreen::car_setup;
    const auto widescreen_front_end =
        state.graphic_options_configuration().aspect_ratio ==
        mh::ui::GraphicAspectRatio::widescreen_16_9;
    const auto &active_background =
        s40_presentation_active ? *s40_background : background;
    const mh::content::TgaImage *animated_background_source = nullptr;
    bool widescreen_animated_background_active = false;
    bool animated_background_refreshed = false;
    if (s40_presentation_active) {
      // The S40 artwork is a complete authored backdrop. Keep Motorhead's
      // dial and menu overlays, but do not composite the retail ambient MYO
      // layer over the Volvo image.
      animated_background_source = &active_background;
    } else if (widescreen_front_end && !static_selector_background) {
      if (!widescreen_animated_background.has_value()) {
        widescreen_animated_background.emplace();
      }
      mh::ui::compose_front_end_widescreen_animated_background(
          active_background, line_objects, background_animation_state,
          sprites.palette, background_render_cache,
          *widescreen_animated_background);
      extract_front_end_center_background(*widescreen_animated_background,
                                          animated_background_storage);
      extract_front_end_wings(*widescreen_animated_background,
                              widescreen_ambient_wings);
      animated_background_refreshed = true;
      animated_background_source = &animated_background_storage;
      widescreen_animated_background_active = true;
    } else if (!static_selector_background) {
      mh::ui::compose_front_end_animated_background(
          active_background, line_objects, background_animation_state,
          sprites.palette, background_render_cache,
          animated_background_storage);
      animated_background_source = &animated_background_storage;
    } else {
      animated_background_source = &active_background;
    }
    const auto &animated_background = *animated_background_source;
    const auto transition_phase =
        terminal_action != mh::ui::FrontEndAction::none
            ? mh::ui::front_end_transition_phase(terminal_transition_ms)
            : (screen_transition.active() ? screen_transition.phase() : 0U);
    if (visible_screen == mh::ui::FrontEndScreen::main_menu) {
      main_pointer_units = mh::ui::advance_main_menu_pointer_units(
          main_pointer_units, state.main_choice(), elapsed);
      const auto glitch =
          mh::ui::advance_main_menu_glitch(main_glitch_state, elapsed);
      mh::ui::compose_main_menu_dynamic_frame(
          animated_background, sprites, positions, state.main_choice(),
          static_cast<std::uint32_t>(ticks), main_pointer_units, glitch,
          transition_phase, frame);
      if (state.exit_confirmation_open()) {
        const auto requester_elapsed =
            ticks >= exit_confirmation_opened_ticks
                ? ticks - exit_confirmation_opened_ticks
                : 0U;
        const auto requester_x =
            static_cast<std::int32_t>(
                std::min<std::uint64_t>(480U, requester_elapsed * 6U)) -
            320;
        frame = mh::ui::compose_exit_confirmation_overlay(
            std::move(frame), sprites, font, state.exit_confirmation_choice(),
            requester_x);
      } else if (exit_confirmation_closing) {
        const auto requester_elapsed =
            ticks >= exit_confirmation_closed_ticks
                ? ticks - exit_confirmation_closed_ticks
                : 0U;
        if (requester_elapsed >= 80U) {
          exit_confirmation_closing = false;
        } else {
          const auto requester_x =
              160 - static_cast<std::int32_t>(
                        std::min<std::uint64_t>(480U, requester_elapsed * 6U));
          frame = mh::ui::compose_exit_confirmation_overlay(
              std::move(frame), sprites, font, closing_exit_confirmation_choice,
              requester_x);
        }
      }
    } else if (visible_screen == mh::ui::FrontEndScreen::multiplayer) {
      const auto &multiplayer_background =
          state.multiplayer_page() == mh::ui::MultiplayerPage::lobby
              ? (state.multiplayer_configuration().lobby_host
                     ? multiplayer_server_background
                     : multiplayer_client_background)
              : animated_background;
      frame = mh::ui::compose_multiplayer_frame(
          multiplayer_background, sprites, positions, font,
          state.multiplayer_page(), state.multiplayer_entry_choice(),
          state.multiplayer_join_field(), state.multiplayer_create_field(),
          state.multiplayer_lobby_field(), state.multiplayer_configuration(),
          static_cast<std::uint32_t>(ticks), transition_phase);
    } else if (visible_screen == mh::ui::FrontEndScreen::one_player) {
      one_player_pointer_units = mh::ui::advance_one_player_pointer_units(
          one_player_pointer_units, state.one_player_choice(), elapsed);
      const auto glitch = mh::ui::advance_one_player_menu_glitch(
          one_player_glitch_state, elapsed);
      mh::ui::compose_one_player_menu_dynamic_frame(
          animated_background, sprites, positions, font,
          state.one_player_choice(), static_cast<std::uint32_t>(ticks),
          one_player_pointer_units, glitch, transition_phase,
          displayed_one_player_values, frame);
    } else if (visible_screen == mh::ui::FrontEndScreen::ghost_setup) {
      frame = mh::ui::compose_ghost_setup_frame(
          animated_background, sprites, positions, font, ghost_demo_files,
          state.ghost_mode_choice(), state.ghost_file_focused(),
          state.ghost_file_index(), static_cast<std::uint32_t>(ticks),
          transition_phase);
    } else if (visible_screen == mh::ui::FrontEndScreen::league_overview) {
      league_menu_pointer_units = mh::ui::advance_league_menu_pointer_units(
          league_menu_pointer_units, state.league_menu_choice(), elapsed);
      frame = mh::ui::compose_league_overview_frame(
          animated_background, sprites, positions, font,
          league_overviews.at(state.league_index()), state.league_menu_choice(),
          static_cast<std::uint32_t>(ticks), league_menu_pointer_units,
          transition_phase);
    } else if (visible_screen == mh::ui::FrontEndScreen::league_summary) {
      frame = mh::ui::compose_league_summary_frame(
          animated_background, sprites, positions, font,
          league_overviews.at(state.league_index()), transition_phase);
    } else if (visible_screen == mh::ui::FrontEndScreen::league_select) {
      frame = mh::ui::compose_league_select_frame(
          animated_background, sprites, positions, font, league_overviews,
          state.league_index(), transition_phase);
    } else if (visible_screen == mh::ui::FrontEndScreen::league_create) {
      frame = mh::ui::compose_league_create_frame(
          animated_background, sprites, positions, font, league_draft,
          state.league_create_field(), transition_phase);
      if (state.league_create_confirmation_open()) {
        const auto requester_elapsed =
            ticks >= exit_confirmation_opened_ticks
                ? ticks - exit_confirmation_opened_ticks
                : 0U;
        const auto requester_x =
            static_cast<std::int32_t>(
                std::min<std::uint64_t>(480U, requester_elapsed * 6U)) -
            320;
        frame = mh::ui::compose_league_create_confirmation_overlay(
            std::move(frame), sprites, font,
            state.league_create_confirmation_choice(), requester_x);
      } else if (exit_confirmation_closing) {
        const auto requester_elapsed =
            ticks >= exit_confirmation_closed_ticks
                ? ticks - exit_confirmation_closed_ticks
                : 0U;
        if (requester_elapsed >= 80U) {
          exit_confirmation_closing = false;
        } else {
          const auto requester_x =
              160 - static_cast<std::int32_t>(
                        std::min<std::uint64_t>(480U, requester_elapsed * 6U));
          frame = mh::ui::compose_league_create_confirmation_overlay(
              std::move(frame), sprites, font, closing_exit_confirmation_choice,
              requester_x);
        }
      }
    } else if (visible_screen == mh::ui::FrontEndScreen::league_delete) {
      frame = mh::ui::compose_league_delete_frame(
          animated_background, sprites, positions, font, league_overviews,
          state.league_index(), transition_phase);
      if (state.league_delete_confirmation_open()) {
        const auto requester_elapsed =
            ticks >= exit_confirmation_opened_ticks
                ? ticks - exit_confirmation_opened_ticks
                : 0U;
        const auto requester_x =
            static_cast<std::int32_t>(
                std::min<std::uint64_t>(480U, requester_elapsed * 6U)) -
            320;
        frame = mh::ui::compose_league_delete_confirmation_overlay(
            std::move(frame), sprites, font,
            state.league_delete_confirmation_choice(), requester_x);
      } else if (exit_confirmation_closing) {
        const auto requester_elapsed =
            ticks >= exit_confirmation_closed_ticks
                ? ticks - exit_confirmation_closed_ticks
                : 0U;
        if (requester_elapsed >= 80U) {
          exit_confirmation_closing = false;
        } else {
          const auto requester_x =
              160 - static_cast<std::int32_t>(
                        std::min<std::uint64_t>(480U, requester_elapsed * 6U));
          frame = mh::ui::compose_league_delete_confirmation_overlay(
              std::move(frame), sprites, font, closing_exit_confirmation_choice,
              requester_x);
        }
      }
    } else if (visible_screen == mh::ui::FrontEndScreen::race_setup) {
      const auto track_index = state.race_setup_track_index();
      const auto &track = race_setup_tracks.at(track_index);
      frame = mh::ui::compose_race_setup_frame(
          animated_background, sprites, positions, font, track.preview,
          track.line_object, track.line_object_scale,
          {track.name, track_index < state.race_setup_unlocked_track_count(),
           track_index != 0U,
           track_index + 1U < state.race_setup_track_count()},
          state.race_setup_mode(), state.race_setup_field(),
          state.race_setup_laps(), state.race_setup_catch_up(),
          static_cast<std::uint32_t>(ticks), transition_phase);
    } else if (visible_screen == mh::ui::FrontEndScreen::car_setup) {
      const auto car_index = state.car_setup_car_index();
      const auto &car = car_setup_assets.at(car_index);
      const auto &horn = horn_setup_assets.at(state.car_setup_horn_index());
      const auto performance = mh::ui::advance_car_setup_performance(
          car_performance_state,
          {car.menu_performance.top_speed, car.menu_performance.acceleration,
           car.menu_performance.handling},
          elapsed);
      frame = mh::ui::compose_car_setup_frame(
          animated_background, sprites, positions, font, car.preview,
          car.line_object, car.line_object_scale,
          {car.name, horn.name,
           car_index < state.car_setup_unlocked_car_count(), car_index != 0U,
           car_index + 1U < state.car_setup_car_count(), performance},
          state.car_setup_field(), state.car_setup_automatic_transmission(),
          state.car_setup_record_race(), static_cast<std::uint32_t>(ticks),
          transition_phase);
    } else if (visible_screen == mh::ui::FrontEndScreen::options) {
      options_pointer_units = mh::ui::advance_options_menu_pointer_units(
          options_pointer_units, state.options_choice(), elapsed);
      const auto glitch =
          mh::ui::advance_options_menu_glitch(options_glitch_state, elapsed);
      mh::ui::compose_options_menu_dynamic_frame(
          animated_background, sprites, menu_dial_sprites, positions,
          state.options_choice(), state.difficulty(),
          static_cast<std::uint32_t>(ticks), options_pointer_units, glitch,
          transition_phase, frame);
    } else if (visible_screen == mh::ui::FrontEndScreen::personal_options) {
      frame = mh::ui::compose_personal_options_frame(
          animated_background, sprites, positions, font, transition_phase,
          state.personal_options_field(),
          state.personal_options_configuration(),
          static_cast<std::uint32_t>(ticks / 500U));
    } else if (visible_screen == mh::ui::FrontEndScreen::graphic_options) {
      frame = mh::ui::compose_graphic_options_frame(
          animated_background, sprites, positions, font,
          state.graphic_options_field(), state.graphic_options_configuration(),
          transition_phase);
    } else if (visible_screen == mh::ui::FrontEndScreen::gameplay_options) {
      frame = mh::ui::compose_gameplay_options_frame(
          animated_background, sprites, positions, font,
          state.gameplay_options_field(), state.graphic_options_configuration(),
          transition_phase);
    } else if (visible_screen == mh::ui::FrontEndScreen::control_options) {
      frame = mh::ui::compose_control_options_frame(
          animated_background, sprites, positions, font, transition_phase,
          state.control_options_focus(), state.control_options_field(),
          state.control_options_configuration(),
          state.control_binding_capture_active());
    } else if (visible_screen == mh::ui::FrontEndScreen::sound_options) {
      const auto glitch = transition_phase == 0U
                              ? mh::ui::SoundOptionsGlitchFrame{}
                              : mh::ui::advance_sound_options_transition(
                                    sound_glitch_state, transition_phase);
      frame = mh::ui::compose_sound_options_frame(
          animated_background, sprites, positions, font, transition_phase,
          glitch, state.sound_options_field(),
          state.sound_options_configuration(), sound_options_presentation,
          mh::ui::main_menu_selected_label_frame(
              static_cast<std::uint32_t>(ticks)));
    } else if (visible_screen == mh::ui::FrontEndScreen::rankings) {
      const auto rankings_presentation = make_rankings_presentation(
          ranking_history, state.rankings_configuration(), race_setup_tracks);
      frame = mh::ui::compose_rankings_frame(
          animated_background, sprites, positions, font, state.rankings_field(),
          state.rankings_configuration(), rankings_presentation,
          transition_phase);
    } else {
      frame = mh::ui::compose_credits_frame(animated_background, sprites,
                                            positions, font, transition_phase);
    }
    const auto &display_background =
        visible_screen == mh::ui::FrontEndScreen::multiplayer &&
                state.multiplayer_page() == mh::ui::MultiplayerPage::lobby
            ? (state.multiplayer_configuration().lobby_host
                   ? multiplayer_server_background
                   : multiplayer_client_background)
            : animated_background;
    make_retail_front_end_display_frame(frame, display_background, display_frame);
    if (visible_screen == mh::ui::FrontEndScreen::one_player) {
      mh::ui::overlay_one_player_display_information(
          display_frame, sprites, positions, font, displayed_one_player_values,
          transition_phase);
    } else if (visible_screen == mh::ui::FrontEndScreen::league_overview) {
      mh::ui::overlay_league_overview_display_information(
          display_frame, sprites, positions, font,
          league_overviews.at(state.league_index()), transition_phase);
    } else if (visible_screen == mh::ui::FrontEndScreen::race_setup) {
      mh::ui::overlay_race_setup_display_badge(display_frame, sprites, positions,
                                               transition_phase);
    } else if (visible_screen == mh::ui::FrontEndScreen::car_setup) {
      mh::ui::overlay_car_setup_display_badge(display_frame, sprites, positions,
                                              transition_phase);
    }
    require(SDL_UpdateTexture(texture.get(), nullptr, display_frame.rgba.data(),
                              static_cast<int>(display_frame.width * 4U)),
            "upload front-end frame");

    int width = 0;
    int height = 0;
    require(SDL_GetWindowSizeInPixels(window.get(), &width, &height),
            "query main-menu window size");
    require(SDL_SetRenderDrawColor(renderer.get(), 0U, 0U, 0U, 255U),
            "set front-end border color");
    require(
        SDL_SetRenderColorScale(
            renderer.get(), state.graphic_options_configuration().brightness),
        "apply Graphic Options brightness");
    require(SDL_RenderClear(renderer.get()), "clear main-menu window");
    if (widescreen_animated_background_active &&
        (animated_background_refreshed ||
         !widescreen_ambient_texture_ready)) {
      require(SDL_UpdateTexture(
                  widescreen_ambient_texture.get(), nullptr,
                  widescreen_ambient_wings.data(),
                  static_cast<int>(front_end_wing_width * 2U * 4U)),
              "upload widescreen front-end ambient layer");
      widescreen_ambient_texture_ready = true;
    }
    if (widescreen_animated_background_active) {
      const auto widescreen_destination =
          widescreen_presentation_rect(width, height);
      const auto center_destination = presentation_rect(width, height);
      const auto side_source_width =
          static_cast<float>(front_end_wing_width);
      const auto left_width =
          std::max(0.0F, center_destination.x - widescreen_destination.x);
      if (left_width > 0.0F) {
        const SDL_FRect source{0.0F, 0.0F, side_source_width,
                               static_cast<float>(
                                   mh::ui::front_end_logical_height)};
        const SDL_FRect destination{widescreen_destination.x,
                                    widescreen_destination.y, left_width,
                                    widescreen_destination.h};
        require(SDL_RenderTexture(renderer.get(),
                                  widescreen_ambient_texture.get(), &source,
                                  &destination),
                "render left widescreen front-end ambient wing");
      }
      const auto center_right = center_destination.x + center_destination.w;
      const auto widescreen_right =
          widescreen_destination.x + widescreen_destination.w;
      const auto right_width = std::max(0.0F, widescreen_right - center_right);
      if (right_width > 0.0F) {
        const SDL_FRect source{
            side_source_width, 0.0F, side_source_width,
            static_cast<float>(mh::ui::front_end_logical_height)};
        const SDL_FRect destination{center_right, widescreen_destination.y,
                                    right_width, widescreen_destination.h};
        require(SDL_RenderTexture(renderer.get(),
                                  widescreen_ambient_texture.get(), &source,
                                  &destination),
                "render right widescreen front-end ambient wing");
      }
    }
    const auto destination = presentation_rect(width, height);
    require(
        SDL_RenderTexture(renderer.get(), texture.get(), nullptr, &destination),
        "render main-menu frame");
    require(SDL_RenderPresent(renderer.get()), "present main-menu frame");
    screen_transition.advance(elapsed);
    if (terminal_action != mh::ui::FrontEndAction::none) {
      const auto consumed = std::min(terminal_transition_ms, elapsed);
      terminal_transition_ms -= consumed;
      if (terminal_transition_ms == 0U) {
        if (terminal_action == mh::ui::FrontEndAction::start_race) {
          const auto race_result = run_selected_race();
          if (race_result == 4) {
            terminal_action = mh::ui::FrontEndAction::none;
            screen_transition = {};
          } else {
            exit_code = race_result;
            running = false;
          }
        } else {
          exit_code =
              terminal_action == mh::ui::FrontEndAction::exit_application
                  ? 0
                  : static_cast<int>(terminal_action);
          running = false;
        }
      }
    }

    ++rendered_frames;
    if (options.maximum_frames != 0U &&
        rendered_frames >= options.maximum_frames) {
      running = false;
    }
  }
  return exit_code;
}

} // namespace

int main(const int argc, char **argv) {
  try {
    static_cast<void>(mh::common::initialize_runtime_log(
        startup_user_data_root(argc, argv) / "motorhead.log"));
    if (argc >= 2 && std::string_view(argv[1]) == "--race-runtime") {
      return mh::app::run_motorhead_race(argc - 1, argv + 1);
    }
    const auto options = parse_options(argc, argv);
    static_cast<void>(mh::common::initialize_runtime_log(
        options.user_data_root / "motorhead.log"));
    mh::common::log_runtime_info("Application started");
    const auto result = run(options);
    mh::common::log_runtime_info("Application stopped normally");
    return result;
  } catch (const std::exception &error) {
    std::cerr << "Motorhead front end failed: " << error.what() << '\n';
    mh::common::log_runtime_error(std::string("Front end failed: ") +
                                  error.what());
    auto message = std::string(error.what());
    if (const auto log_path = mh::common::runtime_log_path();
        !log_path.empty()) {
      message += "\n\nDetails were written to:\n" + log_path.string();
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Motorhead front end failed",
                             message.c_str(), nullptr);
    return 1;
  } catch (...) {
    constexpr auto message = "Motorhead failed with an unknown error.";
    std::cerr << message << '\n';
    mh::common::log_runtime_error(message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Motorhead failed", message,
                             nullptr);
    return 1;
  }
}
