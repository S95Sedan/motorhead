#include "ui/config/user_data.hpp"

#include "core/filesystem/atomic_file.hpp"
#include "content/formats/league_definition.hpp"
#include "content/formats/unlock_catalog.hpp"
#include "ui/config/game_config.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mh::ui {
namespace {

using Validator = mh::common::AtomicFileValidator;

void canonicalize_user_directory(const std::filesystem::path &user_root) {
  if (user_root.filename() != "User") {
    return;
  }
  const auto parent = user_root.parent_path();
  if (!std::filesystem::is_directory(parent)) {
    return;
  }

  std::filesystem::path incorrectly_cased;
  for (const auto &entry : std::filesystem::directory_iterator(parent)) {
    auto name = entry.path().filename().string();
    if (name == "User") {
      return;
    }
    std::transform(name.begin(), name.end(), name.begin(),
                   [](const unsigned char value) {
                     return static_cast<char>(std::tolower(value));
                   });
    if (name == "user" && entry.is_directory()) {
      incorrectly_cased = entry.path();
      break;
    }
  }

  const auto temporary = parent / ".motorhead-user-case";
  if (incorrectly_cased.empty()) {
    if (std::filesystem::is_directory(temporary)) {
      std::filesystem::rename(temporary, user_root);
    }
    return;
  }
  if (std::filesystem::exists(temporary)) {
    throw std::runtime_error(
        "could not normalize the User directory because " +
        temporary.string() + " already exists");
  }

  std::filesystem::rename(incorrectly_cased, temporary);
  try {
    std::filesystem::rename(temporary, user_root);
  } catch (...) {
    std::error_code ignored;
    std::filesystem::rename(temporary, incorrectly_cased, ignored);
    throw;
  }
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("could not read migration source: " +
                             path.string());
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

bool contains_line_prefix(const std::filesystem::path &path,
                          const std::string_view prefix) {
  std::ifstream stream(path);
  for (std::string line; std::getline(stream, line);) {
    if (line.starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

const Validator &config_validator() {
  static const Validator value = [](const auto &path) {
    return !configuration_file_value(path, "GameType").empty();
  };
  return value;
}

const Validator &control_validator() {
  static const Validator value = [](const auto &path) {
    return !configuration_file_value(path, "LayoutName").empty();
  };
  return value;
}

const Validator &rankings_validator() {
  static const Validator value = [](const auto &path) {
    std::ifstream stream(path);
    std::string line;
    if (!std::getline(stream, line) ||
        line != "# Motorhead reconstruction rankings v1") {
      return false;
    }
    while (std::getline(stream, line)) {
      if (line.empty() || line.starts_with('#')) {
        continue;
      }
      std::array<std::string, 8U> fields;
      std::istringstream row(line);
      for (auto &field : fields) {
        if (!std::getline(row, field, '\t')) {
          return false;
        }
      }
      std::string trailing;
      if (std::getline(row, trailing, '\t') || fields[4U].empty() ||
          fields[5U].empty()) {
        return false;
      }
      try {
        for (const auto index : {0U, 1U, 2U, 3U, 6U, 7U}) {
          std::size_t parsed = 0U;
          static_cast<void>(std::stoull(fields[index], &parsed));
          if (parsed != fields[index].size()) {
            return false;
          }
        }
      } catch (...) {
        return false;
      }
    }
    return stream.eof();
  };
  return value;
}

Validator hidden_list_validator(const std::string_view kind) {
  return [expected = "# Reconstruction-owned " + std::string(kind)](
             const auto &path) { return contains_line_prefix(path, expected); };
}

Validator league_validator(const std::filesystem::path &primary) {
  return [name = primary.filename().string()](const auto &path) {
    return mh::content::league_file_valid_for_name(path, name);
  };
}

void flatten_legacy_game_directory(const std::filesystem::path &user_root) {
  const auto legacy = user_root / "Game";
  if (!std::filesystem::is_directory(legacy)) {
    return;
  }
  const auto move_contents = [&](const auto &self,
                                 const std::filesystem::path &source,
                                 const std::filesystem::path &destination)
      -> void {
    std::filesystem::create_directories(destination);
    std::vector<std::filesystem::path> entries;
    for (const auto &entry : std::filesystem::directory_iterator(source)) {
      entries.push_back(entry.path());
    }
    for (const auto &entry : entries) {
      const auto target = destination / entry.filename();
      if (!std::filesystem::exists(target)) {
        std::filesystem::rename(entry, target);
      } else if (std::filesystem::is_directory(entry) &&
                 std::filesystem::is_directory(target)) {
        self(self, entry, target);
      } else if (std::filesystem::is_regular_file(entry) &&
                 std::filesystem::is_regular_file(target) &&
                 read_bytes(entry) == read_bytes(target)) {
        std::filesystem::remove(entry);
      } else {
        throw std::runtime_error(
            "cannot flatten legacy player data without overwriting " +
            target.string());
      }
    }
    if (std::filesystem::is_empty(source)) {
      std::filesystem::remove(source);
    }
  };
  move_contents(move_contents, legacy, user_root);
}

void canonicalize_configuration_filename(
    const std::filesystem::path &user_root) {
  constexpr std::array<std::string_view, 3U> filenames{
      "motorhead.cfg", "motorhead.cfg.motorhead-part",
      "motorhead.cfg.bak"};
  for (const auto expected : filenames) {
    std::filesystem::path incorrectly_cased;
    for (const auto &entry : std::filesystem::directory_iterator(user_root)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      auto name = entry.path().filename().string();
      std::transform(name.begin(), name.end(), name.begin(),
                     [](const unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                     });
      if (name == expected && entry.path().filename().string() != expected) {
        incorrectly_cased = entry.path();
        break;
      }
    }
    if (incorrectly_cased.empty()) {
      continue;
    }
    const auto destination = user_root / expected;
    const auto temporary = user_root / (".motorhead-config-case-" +
                                        std::to_string(expected.size()));
    if (std::filesystem::exists(temporary)) {
      throw std::runtime_error("cannot normalize motorhead.cfg because " +
                               temporary.string() + " already exists");
    }
    if (std::filesystem::exists(destination) &&
        !std::filesystem::equivalent(incorrectly_cased, destination)) {
      throw std::runtime_error(
          "cannot normalize motorhead.cfg without overwriting " +
          destination.string());
    }
    std::filesystem::rename(incorrectly_cased, temporary);
    std::filesystem::rename(temporary, destination);
  }
}

void remove_obsolete_development_metadata(
    const std::filesystem::path &user_root) {
  for (const auto filename : {
           "Motorhead-UserData.version",
           "Motorhead-UserData.version.motorhead-part",
           "Motorhead-UserData.version.motorhead-backup"}) {
    std::error_code error;
    std::filesystem::remove(user_root / filename, error);
    if (error) {
      throw std::system_error(error,
                              "could not remove obsolete player-data marker");
    }
  }

  const auto configuration = user_root / "motorhead.cfg";
  if (!std::filesystem::is_regular_file(configuration)) {
    return;
  }
  std::ifstream stream(configuration, std::ios::binary);
  const std::string contents{std::istreambuf_iterator<char>(stream),
                             std::istreambuf_iterator<char>()};
  if (!stream.eof() && stream.fail()) {
    throw std::runtime_error("could not inspect " + configuration.string());
  }
  stream.close();
  const auto crlf = contents.find("\r\n") != std::string::npos;
  const auto trailing_newline =
      !contents.empty() && (contents.back() == '\n' || contents.back() == '\r');
  std::istringstream lines_stream(contents);
  std::vector<std::string> lines;
  auto removed = false;
  for (std::string line; std::getline(lines_stream, line);) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    auto trimmed = line;
    const auto first = trimmed.find_first_not_of(" \t");
    trimmed = first == std::string::npos ? std::string{} : trimmed.substr(first);
    const auto token_end = trimmed.find_first_of(" \t");
    auto token = trimmed.substr(0U, token_end);
    std::transform(token.begin(), token.end(), token.begin(),
                   [](const unsigned char value) {
                     return static_cast<char>(std::tolower(value));
                   });
    if (token == "reconstructionsaveversion" || trimmed == "[Internal]" ||
        trimmed == "// Reconstruction-supported settings") {
      removed = true;
      continue;
    }
    if (line.empty() && !lines.empty() && lines.back().empty()) {
      continue;
    }
    lines.push_back(std::move(line));
  }
  if (!removed) {
    return;
  }
  while (!lines.empty() && lines.back().empty()) {
    lines.pop_back();
  }
  std::ostringstream cleaned;
  const auto newline = crlf ? "\r\n" : "\n";
  for (std::size_t index = 0U; index < lines.size(); ++index) {
    cleaned << lines[index];
    if (index + 1U < lines.size() || trailing_newline) {
      cleaned << newline;
    }
  }
  mh::common::write_atomic_text(configuration, cleaned.str(),
                                config_validator());
  std::error_code ignored;
  std::filesystem::remove(mh::common::atomic_backup_path(configuration),
                          ignored);
}

void remove_obsolete_diagnostics(const std::filesystem::path &user_root) {
  const auto directory = user_root / "diagnostics";
  std::error_code ignored;
  std::filesystem::remove(directory / "front-end-selection.txt", ignored);
  ignored.clear();
  // remove() only removes an empty directory. Any unrelated user file is
  // therefore preserved rather than being swept up by this migration.
  std::filesystem::remove(directory, ignored);
}

void move_control_files_to_input(const std::filesystem::path &user_root) {
  const auto input = user_root / "Input";
  constexpr std::array<std::string_view, 5U> controls{
      "custom.clo", "Joystick.clo", "Keyboard.clo", "mouse.clo",
      "Wheel.clo"};
  for (const auto control : controls) {
    const auto legacy = user_root / (std::string(control) +
                                     ".motorhead-backup");
    if (!std::filesystem::is_regular_file(legacy)) {
      continue;
    }
    const auto replacement = user_root / (std::string(control) + ".bak");
    if (!std::filesystem::exists(replacement)) {
      std::filesystem::rename(legacy, replacement);
    } else if (std::filesystem::is_regular_file(replacement) &&
               read_bytes(legacy) == read_bytes(replacement)) {
      std::filesystem::remove(legacy);
    } else {
      throw std::runtime_error(
          "cannot rename legacy control backup without overwriting " +
          replacement.string());
    }
  }
  constexpr std::array<std::string_view, 3U> suffixes{
      "", ".motorhead-part", ".bak"};
  for (const auto control : controls) {
    for (const auto suffix : suffixes) {
      const auto filename = std::string(control) + std::string(suffix);
      const auto source = user_root / filename;
      if (!std::filesystem::is_regular_file(source)) {
        continue;
      }
      std::filesystem::create_directories(input);
      const auto destination = input / filename;
      if (!std::filesystem::exists(destination)) {
        std::filesystem::rename(source, destination);
      } else if (std::filesystem::is_regular_file(destination) &&
                 read_bytes(source) == read_bytes(destination)) {
        std::filesystem::remove(source);
      } else {
        throw std::runtime_error(
            "cannot move legacy control layout without overwriting " +
            destination.string());
      }
    }
  }
}

void import_file(const std::filesystem::path &source,
                 const std::filesystem::path &destination,
                 const Validator &validator) {
  if (mh::common::recover_atomic_file(destination, validator)) {
    return;
  }
  if (std::filesystem::exists(destination) ||
      std::filesystem::exists(mh::common::atomic_part_path(destination)) ||
      std::filesystem::exists(mh::common::atomic_backup_path(destination))) {
    throw std::runtime_error(
        "player data is damaged and has no valid recovery copy: " +
        destination.string());
  }
  if (!std::filesystem::is_regular_file(source)) {
    return;
  }
  const auto bytes = read_bytes(source);
  mh::common::write_atomic_file(destination, bytes, validator);
}

void import_game_files(const std::filesystem::path &content_root,
                       const std::filesystem::path &user_root) {
  const auto source = content_root / "Game";
  const auto destination = user_root;
  import_file(source / "motorhead.cfg", destination / "motorhead.cfg",
              config_validator());
  constexpr std::array<std::string_view, 5U> controls{
      "custom.clo", "Joystick.clo", "Keyboard.clo", "mouse.clo",
      "Wheel.clo"};
  for (const auto file : controls) {
    import_file(source / file, control_configuration_path(user_root, file),
                control_validator());
  }

  import_file(source / "HiScores.tsv", destination / "HiScores.tsv",
              rankings_validator());
  import_file(source / "MotorHead-Reconstruction-Hidden-Leagues.txt",
              destination / "MotorHead-Reconstruction-Hidden-Leagues.txt",
              hidden_list_validator("League Delete list"));
  import_file(source / "MotorHead-Reconstruction-Hidden-Demos.txt",
              destination / "MotorHead-Reconstruction-Hidden-Demos.txt",
              hidden_list_validator("Ghost Delete list"));

  constexpr std::array<std::string_view, 4U> legacy{
      "MotorHead-Reconstruction.CFG",
      "MotorHead-Reconstruction-Graphic.CFG",
      "MotorHead-Reconstruction-Personal.CFG",
      "MotorHead-Reconstruction-Control.CFG"};
  const Validator nonempty = [](const auto &path) {
    std::error_code error;
    return std::filesystem::file_size(path, error) != 0U && !error;
  };
  for (const auto file : legacy) {
    import_file(source / file, destination / file, nonempty);
  }
}

void import_leagues(const std::filesystem::path &content_root,
                    const std::filesystem::path &user_root) {
  const auto destination = user_root / "Leagues";
  const auto import_directory = [&](const std::filesystem::path &directory,
                                    const bool skip_default) {
    if (!std::filesystem::is_directory(directory)) {
      return;
    }
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".LGF" ||
          (skip_default && entry.path().filename() == "Default.LGF")) {
        continue;
      }
      const auto output = destination / entry.path().filename();
      import_file(entry.path(), output, league_validator(output));
    }
  };
  import_directory(content_root / "League", true);
}

void import_unlocks(const std::filesystem::path &content_root,
                    const std::filesystem::path &user_root) {
  const auto tracks = content_root / "Tracks";
  if (!std::filesystem::is_directory(tracks)) {
    return;
  }
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(tracks)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".FLG") {
      continue;
    }
    const auto relative = std::filesystem::relative(entry.path(), content_root);
    const auto destination = user_root / relative;
    const auto track_name = entry.path().stem().string();
    const Validator validator = [track_name](const auto &path) {
      return mh::content::original_track_unlock_flag_valid(path, track_name);
    };
    import_file(entry.path(), destination, validator);
  }
}

std::filesystem::path artifact_primary(std::filesystem::path path) {
  auto name = path.filename().string();
  for (const std::string_view suffix : {
           std::string_view(".motorhead-part"), std::string_view(".bak"),
           std::string_view(".motorhead-backup")}) {
    if (name.ends_with(suffix)) {
      name.resize(name.size() - suffix.size());
      return path.parent_path() / name;
    }
  }
  return path;
}

void recover_user_leagues(const std::filesystem::path &user_root) {
  const auto directory = user_root / "Leagues";
  if (!std::filesystem::is_directory(directory)) {
    return;
  }
  std::vector<std::filesystem::path> primaries;
  for (const auto &entry : std::filesystem::directory_iterator(directory)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto primary = artifact_primary(entry.path());
    if (primary.extension() == ".LGF" &&
        std::find(primaries.begin(), primaries.end(), primary) ==
            primaries.end()) {
      primaries.push_back(primary);
    }
  }
  for (const auto &primary : primaries) {
    if (!mh::common::recover_atomic_file(primary, league_validator(primary))) {
      throw std::runtime_error(
          "League save is damaged and has no valid recovery copy: " +
          primary.string());
    }
  }
}

void recover_user_unlocks(const std::filesystem::path &user_root) {
  const auto tracks = user_root / "Tracks";
  if (!std::filesystem::is_directory(tracks)) {
    return;
  }
  std::vector<std::filesystem::path> primaries;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(tracks)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto primary = artifact_primary(entry.path());
    if (primary.extension() == ".FLG" &&
        std::find(primaries.begin(), primaries.end(), primary) ==
            primaries.end()) {
      primaries.push_back(primary);
    }
  }
  for (const auto &primary : primaries) {
    const auto name = primary.stem().string();
    const Validator validator = [name](const auto &candidate) {
      return mh::content::original_track_unlock_flag_valid(candidate, name);
    };
    if (!mh::common::recover_atomic_file(primary, validator)) {
      throw std::runtime_error(
          "unlock save is damaged and has no valid recovery copy: " +
          primary.string());
    }
  }
}

} // namespace

void prepare_user_data(const std::filesystem::path &content_root,
                       const std::filesystem::path &user_root) {
  if (content_root.empty() || user_root.empty()) {
    throw std::invalid_argument("content and user data roots are required");
  }
  canonicalize_user_directory(user_root);
  std::filesystem::create_directories(user_root);
  flatten_legacy_game_directory(user_root);
  canonicalize_configuration_filename(user_root);
  remove_obsolete_development_metadata(user_root);
  remove_obsolete_diagnostics(user_root);
  move_control_files_to_input(user_root);

  try {
    std::filesystem::create_directories(user_root);
  } catch (const std::filesystem::filesystem_error &error) {
    throw std::runtime_error("could not create the player-data directory " +
                             user_root.string() + ": " + error.what());
  }
  import_game_files(content_root, user_root);
  import_leagues(content_root, user_root);
  import_unlocks(content_root, user_root);
  recover_user_leagues(user_root);
  recover_user_unlocks(user_root);
  migrate_legacy_motorhead_configurations(user_root);
}

} // namespace mh::ui
