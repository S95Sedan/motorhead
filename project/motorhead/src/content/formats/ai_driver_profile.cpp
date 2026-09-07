#include <content/formats/ai_driver_profile.hpp>

#include <core/error.hpp>
#include <content/formats/tbl_envelope.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <locale>
#include <sstream>
#include <string_view>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::string trim(const std::string_view value) {
  const auto whitespace = [](const char character) {
    return character == ' ' || character == '\t' || character == '\r';
  };
  auto first = value.begin();
  auto last = value.end();
  while (first != last && whitespace(*first)) {
    ++first;
  }
  while (last != first && whitespace(*(last - 1))) {
    --last;
  }
  return std::string(first, last);
}

std::string lower_ascii(std::string value) {
  for (auto &character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
  }
  return value;
}

float parse_float(const std::string_view value, const std::string_view field) {
  std::istringstream input(trim(value));
  input.imbue(std::locale::classic());
  float result = 0.0F;
  std::string trailing;
  if (!(input >> result) || (input >> trailing) || !std::isfinite(result)) {
    throw ToolError(ExitCode::format, "ADP field " + std::string(field) +
                                          " must contain one finite number");
  }
  return result;
}

AiProfileColor parse_color(const std::string_view value,
                           const std::string_view field) {
  std::istringstream input(trim(value));
  std::array<unsigned int, 3U> channels{};
  std::string trailing;
  if (!(input >> channels[0] >> channels[1] >> channels[2]) ||
      (input >> trailing) ||
      std::any_of(channels.begin(), channels.end(),
                  [](const unsigned int channel) { return channel > 255U; })) {
    throw ToolError(ExitCode::format,
                    "ADP field " + std::string(field) +
                        " must contain exactly three byte values");
  }
  return {static_cast<std::uint8_t>(channels[0]),
          static_cast<std::uint8_t>(channels[1]),
          static_cast<std::uint8_t>(channels[2])};
}

} // namespace

AiDriverProfile
parse_ai_driver_profile(const std::span<const std::uint8_t> decoded_text) {
  if (decoded_text.empty()) {
    throw ToolError(ExitCode::format, "ADP payload is empty");
  }
  if (std::find(decoded_text.begin(), decoded_text.end(), 0U) !=
      decoded_text.end()) {
    throw ToolError(ExitCode::format, "ADP payload contains a NUL byte");
  }

  AiDriverProfile result;
  std::array<bool, 15U> fields{};
  std::size_t line_start = 0U;
  while (line_start < decoded_text.size()) {
    auto line_end = line_start;
    while (line_end < decoded_text.size() &&
           decoded_text[line_end] != static_cast<std::uint8_t>('\n')) {
      ++line_end;
    }
    auto line = std::string(
        reinterpret_cast<const char *>(decoded_text.data() + line_start),
        line_end - line_start);
    if (const auto comment = line.find("//"); comment != std::string::npos) {
      line.erase(comment);
    }
    line = trim(line);
    line_start = line_end + (line_end < decoded_text.size() ? 1U : 0U);
    if (line.empty()) {
      continue;
    }
    const auto separator = line.find_first_of(" \t");
    const auto key = lower_ascii(
        separator == std::string::npos ? line : line.substr(0U, separator));
    const auto value = separator == std::string::npos
                           ? std::string{}
                           : trim(line.substr(separator + 1U));
    std::size_t field = fields.size();
    if (key == "playername") {
      field = 0U;
      result.player_name = value;
    } else if (key == "playernick") {
      field = 1U;
      result.player_nick = value;
    } else if (key == "playerpicture") {
      field = 2U;
      result.player_picture = value;
    } else if (key == "playerborn") {
      field = 3U;
      result.player_born = value;
    } else if (key == "playercolour") {
      field = 4U;
      result.player_color = parse_color(value, key);
    } else if (key == "aggressivness") {
      field = 5U;
      result.aggressiveness = parse_float(value, key);
    } else if (key == "eagerness") {
      field = 6U;
      result.eagerness = parse_float(value, key);
    } else if (key.starts_with("cardivision") && key.size() == 12U &&
               key.back() >= '0' && key.back() <= '3') {
      field = 7U + static_cast<std::size_t>(key.back() - '0');
      result.division_cars[field - 7U] = value;
    } else if (key == "carhorn") {
      field = 11U;
      result.horn = value;
    } else if (key.starts_with("carcolour") && key.size() == 10U &&
               key.back() >= '1' && key.back() <= '3') {
      field = 12U + static_cast<std::size_t>(key.back() - '1');
      result.car_colors[field - 12U] = parse_color(value, key);
    } else {
      ++result.unknown_fields;
      continue;
    }
    if (fields[field]) {
      throw ToolError(ExitCode::format, "duplicate ADP field " + key);
    }
    fields[field] = true;
    ++result.recognized_fields;
  }

  if (!std::all_of(fields.begin(), fields.end(),
                   [](const bool present) { return present; }) ||
      result.player_name.empty() || result.player_nick.empty() ||
      result.player_picture.empty() || result.player_born.empty() ||
      result.horn.empty() ||
      std::any_of(result.division_cars.begin(), result.division_cars.end(),
                  [](const std::string &car) { return car.empty(); })) {
    throw ToolError(ExitCode::format, "ADP profile is missing required fields");
  }
  return result;
}

AiDriverProfile read_ai_driver_profile(const std::filesystem::path &path,
                                       const std::uint64_t maximum_file_bytes) {
  const auto envelope =
      read_tbl(path, "EQ", maximum_file_bytes, maximum_file_bytes);
  if (!envelope.decoded) {
    throw ToolError(ExitCode::format, "ADP TBL envelope could not be decoded");
  }
  return parse_ai_driver_profile(envelope.payload);
}

std::vector<AiDriverProfile>
read_ai_driver_profile_catalog(const std::filesystem::path &profile_root) {
  if (!std::filesystem::is_directory(profile_root)) {
    throw ToolError(ExitCode::input,
                    "AI driver profile catalog directory was not found: " +
                        profile_root.string());
  }

  std::vector<AiDriverProfile> result;
  for (const auto &entry : std::filesystem::directory_iterator(profile_root)) {
    if (!entry.is_regular_file() ||
        lower_ascii(entry.path().extension().string()) != ".adp") {
      continue;
    }
    result.push_back(read_ai_driver_profile(entry.path()));
  }
  return result;
}

} // namespace mh::content
