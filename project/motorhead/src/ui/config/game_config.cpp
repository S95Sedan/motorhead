#include <ui/config/game_config.hpp>

#include <core/filesystem/atomic_file.hpp>

#include "game_config_template.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mh::ui {
namespace {

std::string trim_copy(const std::string_view value) {
  const auto first = std::find_if_not(value.begin(), value.end(),
                                      [](const unsigned char character) {
                                        return std::isspace(character) != 0;
                                      });
  const auto last = std::find_if_not(value.rbegin(), value.rend(),
                                     [](const unsigned char character) {
                                       return std::isspace(character) != 0;
                                     })
                        .base();
  return first >= last ? std::string{} : std::string(first, last);
}

bool ascii_equal_case_insensitive(const std::string_view left,
                                  const std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.size(); ++index) {
    if (std::tolower(static_cast<unsigned char>(left[index])) !=
        std::tolower(static_cast<unsigned char>(right[index]))) {
      return false;
    }
  }
  return true;
}

bool contains_line_prefix(const std::filesystem::path &path,
                          const std::string_view text) {
  std::ifstream stream(path);
  for (std::string line; std::getline(stream, line);) {
    if (line.find(text) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool line_key_matches(const std::string_view line, const std::string_view key) {
  auto content = line;
  if (const auto comment = content.find("//");
      comment != std::string_view::npos) {
    content = content.substr(0U, comment);
  }
  content = std::string_view(content.data(), content.size());
  while (!content.empty() &&
         std::isspace(static_cast<unsigned char>(content.front())) != 0) {
    content.remove_prefix(1U);
  }
  if (content.size() < key.size() ||
      !ascii_equal_case_insensitive(content.substr(0U, key.size()), key)) {
    return false;
  }
  return content.size() == key.size() ||
         std::isspace(static_cast<unsigned char>(content[key.size()])) != 0;
}

std::vector<std::string> read_lines(const std::filesystem::path &path,
                                    bool &crlf, bool &trailing_newline) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("could not open configuration file: " +
                             path.string());
  }
  const std::string bytes((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
  crlf = bytes.find("\r\n") != std::string::npos;
  trailing_newline = !bytes.empty() && bytes.back() == '\n';
  std::vector<std::string> lines;
  std::size_t start = 0U;
  while (start < bytes.size()) {
    const auto end = bytes.find('\n', start);
    const auto length =
        end == std::string::npos ? bytes.size() - start : end - start;
    auto line = bytes.substr(start, length);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    if (end == std::string::npos) {
      break;
    }
    start = end + 1U;
  }
  return lines;
}

std::vector<std::pair<std::string, std::string>>
read_configuration_entries(const std::filesystem::path &path) {
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("could not open legacy configuration file: " +
                             path.string());
  }
  std::vector<std::pair<std::string, std::string>> result;
  std::string line;
  while (std::getline(stream, line)) {
    if (const auto comment = line.find("//"); comment != std::string::npos) {
      line.erase(comment);
    }
    line = trim_copy(line);
    if (line.empty() || line.front() == '[' ||
        !std::isalnum(static_cast<unsigned char>(line.front()))) {
      continue;
    }
    const auto separator = std::find_if(line.begin(), line.end(),
                                        [](const unsigned char character) {
                                          return std::isspace(character) != 0;
                                        });
    const auto key = std::string(line.begin(), separator);
    const auto value =
        separator == line.end()
            ? std::string{}
            : trim_copy(std::string_view(
                  &*separator,
                  static_cast<std::size_t>(line.end() - separator)));
    result.emplace_back(key, value);
  }
  return result;
}

std::vector<std::string> split_lines(const std::string_view contents) {
  std::vector<std::string> lines;
  std::size_t start = 0U;
  while (start < contents.size()) {
    const auto end = contents.find('\n', start);
    const auto length =
        end == std::string_view::npos ? contents.size() - start : end - start;
    auto line = std::string(contents.substr(start, length));
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(std::move(line));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1U;
  }
  return lines;
}

std::optional<std::pair<std::string, std::string>>
configuration_entry(const std::string_view source) {
  auto line = source;
  if (const auto comment = line.find("//"); comment != std::string_view::npos) {
    line = line.substr(0U, comment);
  }
  const auto trimmed = trim_copy(line);
  if (trimmed.empty() || trimmed.front() == '[' ||
      !std::isalnum(static_cast<unsigned char>(trimmed.front()))) {
    return std::nullopt;
  }
  const auto separator =
      std::find_if(trimmed.begin(), trimmed.end(), [](const unsigned char c) {
        return std::isspace(c) != 0;
      });
  const auto key = std::string(trimmed.begin(), separator);
  const auto value =
      separator == trimmed.end()
          ? std::string{}
          : trim_copy(std::string_view(
                &*separator,
                static_cast<std::size_t>(trimmed.end() - separator)));
  return std::pair{key, value};
}

auto find_entry(std::vector<std::pair<std::string, std::string>> &entries,
                const std::string_view key) {
  return std::find_if(entries.begin(), entries.end(), [&key](const auto &entry) {
    return ascii_equal_case_insensitive(entry.first, key);
  });
}

bool contains_key(const std::vector<std::string> &keys,
                  const std::string_view key) {
  return std::any_of(keys.begin(), keys.end(), [&key](const auto &candidate) {
    return ascii_equal_case_insensitive(candidate, key);
  });
}

std::string canonical_configuration_line(const std::string_view template_line,
                                         const std::string_view key,
                                         const std::string_view value) {
  constexpr std::size_t value_column = 24U;
  constexpr std::size_t comment_column = 49U;
  const auto comment_position = template_line.find("//");
  const auto comment =
      comment_position == std::string_view::npos
          ? std::string_view{}
          : template_line.substr(comment_position);
  std::string result(key);
  if (!value.empty()) {
    result.append(std::max(value_column, result.size() + 1U) - result.size(),
                  ' ');
    result.append(value);
  }
  if (!comment.empty()) {
    result.append(std::max(comment_column, result.size() + 1U) - result.size(),
                  ' ');
    result.append(comment);
  }
  return result;
}

std::string canonical_motorhead_configuration(
    const std::filesystem::path &path,
    const ConfigurationUpdates &normalized_updates) {
  auto values = read_configuration_entries(path);
  for (const auto &[key, value] : normalized_updates) {
    const auto existing = find_entry(values, key);
    if (existing == values.end()) {
      values.emplace_back(key, value);
    } else {
      existing->second = value;
    }
  }

  auto lines = split_lines(generated::motorhead_configuration_template);
  std::vector<std::string> known_keys;
  for (auto &line : lines) {
    const auto template_entry = configuration_entry(line);
    if (!template_entry.has_value()) {
      continue;
    }
    const auto &[key, default_value] = *template_entry;
    known_keys.push_back(key);
    const auto current = find_entry(values, key);
    line = canonical_configuration_line(
        line, key, current == values.end() ? default_value : current->second);
  }

  std::vector<std::pair<std::string, std::string>> compatibility;
  for (const auto &entry : values) {
    if (!contains_key(known_keys, entry.first)) {
      compatibility.push_back(entry);
    }
  }

  if (!compatibility.empty()) {
    auto insertion =
        std::find_if(lines.begin(), lines.end(), [](const auto &line) {
          return line.find("EOF CONFIG FILE") != std::string::npos;
        });
    std::vector<std::string> block{
        {}, "[Compatibility]",
        "// Settings retained from a newer or externally modified file."};
    block.reserve(block.size() + compatibility.size());
    for (const auto &[key, value] : compatibility) {
      std::string line(key);
      if (!value.empty()) {
        line.append(std::max<std::size_t>(24U, line.size() + 1U) - line.size(),
                    ' ');
        line.append(value);
      }
      block.push_back(std::move(line));
    }
    lines.insert(insertion, block.begin(), block.end());
  }

  std::ostringstream contents;
  for (const auto &line : lines) {
    contents << line << '\n';
  }
  return contents.str();
}

std::string legacy_value(const std::filesystem::path &path,
                         const std::string &key) {
  return configuration_file_value(path, key);
}

} // namespace

std::filesystem::path control_configuration_path(
    const std::filesystem::path &reference_root,
    const std::string_view filename) {
  return reference_root / "Input" / filename;
}

std::filesystem::path
motorhead_configuration_path(const std::filesystem::path &reference_root) {
  return reference_root / "motorhead.cfg";
}

std::string configuration_file_value(const std::filesystem::path &path,
                                     const std::string_view key) {
  std::ifstream stream(path);
  std::string line;
  while (std::getline(stream, line)) {
    if (const auto comment = line.find("//"); comment != std::string::npos) {
      line.erase(comment);
    }
    line = trim_copy(line);
    if (!line_key_matches(line, key)) {
      continue;
    }
    return trim_copy(std::string_view(line).substr(key.size()));
  }
  return {};
}

IntroMovieMode intro_movie_mode(const std::filesystem::path &reference_root) {
  const auto value = configuration_file_value(
      motorhead_configuration_path(reference_root), "SkipIntroMovies");
  if (ascii_equal_case_insensitive(value, "On")) {
    return IntroMovieMode::skip;
  }
  if (ascii_equal_case_insensitive(value, "Single")) {
    return IntroMovieMode::play_key_skips_single;
  }
  // Off and missing legacy values both preserve movie playback. A key skips
  // the complete sequence, matching the least repetitive opt-out behaviour.
  return IntroMovieMode::play_key_skips_all;
}

bool intro_movie_key_skips_all(const IntroMovieMode mode) noexcept {
  return mode == IntroMovieMode::play_key_skips_all;
}

void update_configuration_file(const std::filesystem::path &path,
                               const ConfigurationUpdates &updates) {
  if (updates.empty()) {
    return;
  }
  auto normalized_updates = updates;
  for (auto &[key, value] : normalized_updates) {
    static_cast<void>(key);
    // The original CFG grammar treats leading/trailing whitespace as syntax,
    // not as part of a value. Persist and verify the same canonical value the
    // reader returns (notably when a text field ends with a typed space).
    value = trim_copy(value);
  }
  if (ascii_equal_case_insensitive(path.filename().string(),
                                   "motorhead.cfg")) {
    // A pre-reconstruction retail CFG can be a valid recovery generation even
    // though it does not yet contain the canonical section headings. Accept it
    // while staging the update so the first canonical rewrite retains a .bak.
    // The published result is checked against the stricter layout below.
    const auto recoverable_validator = [](const auto &candidate) {
      std::error_code error;
      return std::filesystem::file_size(candidate, error) != 0U && !error &&
             !configuration_file_value(candidate, "GameType").empty() &&
             contains_line_prefix(candidate, "EOF CONFIG FILE");
    };
    const auto canonical_validator = [&recoverable_validator](
                                         const auto &candidate) {
      return recoverable_validator(candidate) &&
             contains_line_prefix(candidate, "[Player]") &&
             contains_line_prefix(candidate, "[Menu]");
    };
    mh::common::write_atomic_text(
        path, canonical_motorhead_configuration(path, normalized_updates),
        recoverable_validator);
    if (!canonical_validator(path) ||
        !std::all_of(normalized_updates.begin(), normalized_updates.end(),
                     [&path](const auto &update) {
                       return configuration_file_value(path, update.first) ==
                              update.second;
                     })) {
      throw std::runtime_error("configuration update did not persist: " +
                               path.string());
    }
    return;
  }
  bool crlf = false;
  bool trailing_newline = true;
  auto lines = read_lines(path, crlf, trailing_newline);
  std::vector<bool> written(normalized_updates.size(), false);
  for (auto &line : lines) {
    for (std::size_t index = 0U; index < normalized_updates.size(); ++index) {
      const auto &[key, value] = normalized_updates[index];
      if (!line_key_matches(line, key)) {
        continue;
      }
      const auto comment = line.find("//");
      auto replacement = key + "\t" + value;
      if (comment != std::string::npos) {
        replacement += "\t\t" + line.substr(comment);
      }
      line = std::move(replacement);
      written[index] = true;
      break;
    }
  }

  std::vector<std::string> additions;
  for (std::size_t index = 0U; index < normalized_updates.size(); ++index) {
    if (!written[index]) {
      additions.push_back(normalized_updates[index].first + "\t" +
                          normalized_updates[index].second);
    }
  }
  if (!additions.empty()) {
    auto insertion =
        std::find_if(lines.begin(), lines.end(), [](const auto &line) {
          return line.find("EOF CONFIG FILE") != std::string::npos;
        });
    std::vector<std::string> block;
    block.reserve(additions.size() + 2U);
    block.emplace_back();
    block.emplace_back("// Reconstruction-supported settings");
    block.insert(block.end(), additions.begin(), additions.end());
    lines.insert(insertion, block.begin(), block.end());
  }

  std::ostringstream contents;
  const auto newline = crlf ? "\r\n" : "\n";
  for (std::size_t index = 0U; index < lines.size(); ++index) {
    contents << lines[index];
    if (index + 1U < lines.size() || trailing_newline) {
      contents << newline;
    }
  }
  const auto control_file = path.extension() == ".clo";
  const auto validator = [control_file](const auto &candidate) {
    std::error_code error;
    if (std::filesystem::file_size(candidate, error) == 0U || error) {
      return false;
    }
    if (control_file) {
      return !configuration_file_value(candidate, "LayoutName").empty();
    }
    return !configuration_file_value(candidate, "GameType").empty() ||
           !configuration_file_value(candidate, "PlayerName").empty() ||
           contains_line_prefix(candidate, "EOF CONFIG FILE");
  };
  mh::common::write_atomic_text(path, contents.str(), validator);
  if (!std::all_of(normalized_updates.begin(), normalized_updates.end(),
                   [&path](const auto &update) {
                     return configuration_file_value(path, update.first) ==
                            update.second;
                   })) {
    throw std::runtime_error("configuration update did not persist: " +
                             path.string());
  }
}

void update_motorhead_configuration(const std::filesystem::path &reference_root,
                                    const ConfigurationUpdates &updates) {
  update_configuration_file(motorhead_configuration_path(reference_root),
                            updates);
}

void migrate_legacy_motorhead_configurations(
    const std::filesystem::path &reference_root) {
  const auto game = reference_root;
  const std::array general_files{
      game / "MotorHead-Reconstruction.CFG",
      game / "MotorHead-Reconstruction-Graphic.CFG",
      game / "MotorHead-Reconstruction-Personal.CFG",
  };
  const auto control = game / "MotorHead-Reconstruction-Control.CFG";
  auto found = std::filesystem::is_regular_file(control);
  ConfigurationUpdates main_updates;
  for (const auto &path : general_files) {
    if (!std::filesystem::is_regular_file(path)) {
      continue;
    }
    found = true;
    const auto entries = read_configuration_entries(path);
    main_updates.insert(main_updates.end(), entries.begin(), entries.end());
  }
  if (std::filesystem::is_regular_file(control)) {
    if (const auto selected = legacy_value(control, "ControlName");
        !selected.empty()) {
      main_updates.emplace_back("ControlName", selected);
    }
  }
  if (!found) {
    return;
  }
  if (!main_updates.empty()) {
    update_motorhead_configuration(reference_root, main_updates);
  }

  if (std::filesystem::is_regular_file(control)) {
    constexpr std::array<std::string_view, 5U> files{
        "custom.clo", "Joystick.clo", "Keyboard.clo", "mouse.clo", "Wheel.clo"};
    constexpr std::array<std::string_view, 10U> binding_keys{
        "TurnLeft",      "TurnRight", "Accelerate", "Brake",    "ShiftGearUp",
        "ShiftGearDown", "Horn",      "HandBrake",  "RearView", "GameMenu"};
    constexpr std::array<std::string_view, 3U> speed_keys{
        "MouseSpeedH", "MouseSpeedV", "MouseSpeedZ"};
    for (std::size_t profile = 0U; profile < files.size(); ++profile) {
      const auto prefix = "Profile_" + std::to_string(profile) + "_";
      ConfigurationUpdates profile_updates;
      if (const auto value = legacy_value(control, prefix + "ForceFeedback");
          !value.empty()) {
        profile_updates.emplace_back("ForceFeedback", value);
      }
      for (std::size_t speed = 0U; speed < speed_keys.size(); ++speed) {
        if (const auto value = legacy_value(control, prefix + "MouseSpeed" +
                                                         std::to_string(speed));
            !value.empty()) {
          profile_updates.emplace_back(speed_keys[speed], value);
        }
      }
      for (std::size_t binding = 0U; binding < binding_keys.size(); ++binding) {
        if (const auto value = legacy_value(
                control, prefix + "Binding" + std::to_string(binding));
            !value.empty()) {
          profile_updates.emplace_back(binding_keys[binding], value);
        }
      }
      if (!profile_updates.empty()) {
        update_configuration_file(
            control_configuration_path(reference_root, files[profile]),
            profile_updates);
      }
    }
  }

  for (const auto &path : general_files) {
    std::error_code error;
    std::filesystem::remove(path, error);
    if (error) {
      throw std::system_error(error, "could not remove legacy configuration");
    }
  }
  std::error_code error;
  std::filesystem::remove(control, error);
  if (error) {
    throw std::system_error(error,
                            "could not remove legacy control configuration");
  }
}

} // namespace mh::ui
