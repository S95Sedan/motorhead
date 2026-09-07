#include <content/formats/track_definition.hpp>

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

bool is_reference_field(const std::string_view field) {
  constexpr std::array<std::string_view, 17U> fields{
      "filename",       "collisionname",    "texturepath",     "splinename",
      "materialsname",  "camerasplinename", "scenename",       "trackmapname",
      "lineobjectname", "aitrackname",      "envsoundname",    "startspline1",
      "startspline2",   "envmapname",       "zerotransparent", "halo",
      "backgroundname"};
  return std::find(fields.begin(), fields.end(), field) != fields.end();
}

std::uint32_t parse_u32(const std::string &value,
                        const std::string_view field) {
  std::istringstream input(value);
  std::uint64_t parsed = 0U;
  std::string trailing;
  if (!(input >> parsed) || (input >> trailing) || parsed > 0xffffffffULL) {
    throw ToolError(ExitCode::format, "track field " + std::string(field) +
                                          " must be one unsigned integer");
  }
  return static_cast<std::uint32_t>(parsed);
}

TrackStartGrid parse_start_grid(const std::string &value) {
  std::istringstream input(value);
  input.imbue(std::locale::classic());
  TrackStartGrid result;
  std::string trailing;
  if (!(input >> result.row >> result.goal >> result.space >> result.tint) ||
      (input >> trailing) || !std::isfinite(result.row) ||
      !std::isfinite(result.goal) || !std::isfinite(result.space) ||
      !std::isfinite(result.tint) || result.row <= 0.0F ||
      result.goal <= 0.0F || result.space <= 0.0F || result.tint <= 0.0F) {
    throw ToolError(ExitCode::format, "track field startgrid must contain "
                                      "exactly four positive finite numbers");
  }
  return result;
}

TrackRgbColor parse_rgb_color(const std::string &value,
                              const std::string_view field) {
  std::istringstream input(value);
  std::array<std::uint32_t, 3U> channels{};
  std::string trailing;
  if (!(input >> channels[0U] >> channels[1U] >> channels[2U]) ||
      (input >> trailing) ||
      std::any_of(channels.begin(), channels.end(),
                  [](const auto channel) { return channel > 255U; })) {
    throw ToolError(ExitCode::format, "track field " + std::string(field) +
                                          " must contain exactly three "
                                          "integers in the range 0..255");
  }
  return TrackRgbColor{static_cast<std::uint8_t>(channels[0U]),
                       static_cast<std::uint8_t>(channels[1U]),
                       static_cast<std::uint8_t>(channels[2U])};
}

float parse_unit_float(const std::string &value, const std::string_view field) {
  std::istringstream input(value);
  input.imbue(std::locale::classic());
  float parsed = 0.0F;
  std::string trailing;
  if (!(input >> parsed) || (input >> trailing) || !std::isfinite(parsed) ||
      parsed < 0.0F || parsed > 1.0F) {
    throw ToolError(ExitCode::format, "track field " + std::string(field) +
                                          " must be one finite number in "
                                          "the range 0..1");
  }
  return parsed;
}

float parse_nonnegative_float(const std::string &value,
                              const std::string_view field) {
  std::istringstream input(value);
  input.imbue(std::locale::classic());
  float parsed = 0.0F;
  std::string trailing;
  if (!(input >> parsed) || (input >> trailing) || !std::isfinite(parsed) ||
      parsed < 0.0F) {
    throw ToolError(ExitCode::format, "track field " + std::string(field) +
                                          " must be one nonnegative finite "
                                          "number");
  }
  return parsed;
}

std::array<float, 3U> parse_nonnegative_vector(const std::string &value,
                                               const std::string_view field) {
  std::istringstream input(value);
  input.imbue(std::locale::classic());
  std::array<float, 3U> parsed{};
  std::string trailing;
  if (!(input >> parsed[0U] >> parsed[1U] >> parsed[2U]) ||
      (input >> trailing) ||
      std::any_of(parsed.begin(), parsed.end(), [](const auto component) {
        return !std::isfinite(component) || component < 0.0F;
      })) {
    throw ToolError(ExitCode::format, "track field " + std::string(field) +
                                          " must contain exactly three "
                                          "nonnegative finite numbers");
  }
  return parsed;
}

std::array<float, 3U> parse_float_rgb_color(const std::string &value,
                                            const std::string_view field) {
  const auto parsed = parse_nonnegative_vector(value, field);
  if (std::any_of(parsed.begin(), parsed.end(),
                  [](const auto component) { return component > 255.0F; })) {
    throw ToolError(ExitCode::format, "track field " + std::string(field) +
                                          " components must not exceed 255");
  }
  return parsed;
}

} // namespace

TrackDefinition
parse_track_definition(const std::span<const std::uint8_t> decoded_text) {
  if (decoded_text.empty() || decoded_text.size() > 1024U * 1024U) {
    throw ToolError(ExitCode::format,
                    "track-definition text size is outside supported bounds");
  }
  if (std::find(decoded_text.begin(), decoded_text.end(), 0U) !=
      decoded_text.end()) {
    throw ToolError(ExitCode::format,
                    "track-definition text contains a NUL byte");
  }
  const std::string text(decoded_text.begin(), decoded_text.end());
  std::istringstream lines(text);
  TrackDefinition result;
  bool have_name = false;
  bool have_base = false;
  bool have_world = false;
  bool have_division = false;
  bool have_start_grid = false;
  std::string line;
  std::size_t line_number = 0U;
  while (std::getline(lines, line)) {
    ++line_number;
    if (line.size() > 4096U) {
      throw ToolError(ExitCode::format,
                      "track-definition line exceeds 4096 bytes");
    }
    const auto comment = line.find("//");
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }
    const auto separator = line.find_first_of(" \t");
    const auto field = lower_ascii(line.substr(0U, separator));
    const auto value =
        separator == std::string::npos
            ? std::string{}
            : trim(std::string_view(line).substr(separator + 1U));
    if (value.empty()) {
      throw ToolError(ExitCode::format, "track field on line " +
                                            std::to_string(line_number) +
                                            " has no value");
    }
    ++result.field_count;
    if (field == "trackname") {
      if (have_name) {
        throw ToolError(ExitCode::format, "duplicate track name");
      }
      result.name = value;
      have_name = true;
    } else if (field == "basepath") {
      if (have_base) {
        throw ToolError(ExitCode::format, "duplicate track base path");
      }
      result.base_path = value;
      have_base = true;
    } else if (field == "filename") {
      if (have_world) {
        throw ToolError(ExitCode::format, "duplicate track world filename");
      }
      result.world_filename = value;
      have_world = true;
    } else if (field == "division") {
      if (have_division) {
        throw ToolError(ExitCode::format, "duplicate track division");
      }
      result.division = parse_u32(value, field);
      have_division = true;
    } else if (field == "startgrid") {
      if (have_start_grid) {
        throw ToolError(ExitCode::format, "duplicate track start grid");
      }
      result.start_grid = parse_start_grid(value);
      have_start_grid = true;
    } else if (field == "viewdistance") {
      if (result.environment.view_distance.has_value()) {
        throw ToolError(ExitCode::format, "duplicate track view distance");
      }
      const auto distance = parse_u32(value, field);
      if (distance == 0U) {
        throw ToolError(ExitCode::format,
                        "track view distance must be greater than zero");
      }
      result.environment.view_distance = distance;
    } else if (field == "topcolour") {
      if (result.environment.top_color.has_value()) {
        throw ToolError(ExitCode::format, "duplicate track top colour");
      }
      result.environment.top_color = parse_rgb_color(value, field);
    } else if (field == "bottomcolour") {
      if (result.environment.bottom_color.has_value()) {
        throw ToolError(ExitCode::format, "duplicate track bottom colour");
      }
      result.environment.bottom_color = parse_rgb_color(value, field);
    } else if (field == "horizon") {
      if (result.environment.horizon.has_value()) {
        throw ToolError(ExitCode::format, "duplicate track horizon");
      }
      result.environment.horizon = parse_unit_float(value, field);
    } else if (field == "worldbrightness") {
      if (result.environment.world_brightness.has_value()) {
        throw ToolError(ExitCode::format, "duplicate track world brightness");
      }
      result.environment.world_brightness =
          parse_nonnegative_vector(value, field);
    } else if (field == "objectbrightness") {
      if (result.environment.object_brightness.has_value()) {
        throw ToolError(ExitCode::format, "duplicate track object brightness");
      }
      result.environment.object_brightness =
          parse_nonnegative_vector(value, field);
    } else if (field == "3daccbrightness") {
      if (result.environment.accelerated_brightness.has_value()) {
        throw ToolError(ExitCode::format,
                        "duplicate track accelerated brightness");
      }
      result.environment.accelerated_brightness =
          parse_nonnegative_vector(value, field);
    } else if (field == "objectambient") {
      if (result.environment.object_ambient.has_value()) {
        throw ToolError(ExitCode::format, "duplicate track object ambient");
      }
      result.environment.object_ambient =
          parse_nonnegative_vector(value, field);
    } else if (field == "cuecolour") {
      if (result.environment.cue_color.has_value()) {
        throw ToolError(ExitCode::format, "duplicate track cue colour");
      }
      result.environment.cue_color = parse_float_rgb_color(value, field);
    } else if (field == "cuestart") {
      if (result.environment.cue_start.has_value()) {
        throw ToolError(ExitCode::format, "duplicate track cue start");
      }
      result.environment.cue_start = parse_unit_float(value, field);
    } else if (field == "specularfactor") {
      if (result.environment.specular_factor.has_value()) {
        throw ToolError(ExitCode::format, "duplicate track specular factor");
      }
      result.environment.specular_factor =
          parse_nonnegative_float(value, field);
    } else if (field == "lightintensity") {
      if (result.environment.light_intensity.has_value()) {
        throw ToolError(ExitCode::format, "duplicate track light intensity");
      }
      result.environment.light_intensity =
          parse_nonnegative_float(value, field);
    }
    if (is_reference_field(field)) {
      result.references.push_back(TrackReference{field, value});
    }
  }
  if (!have_name || !have_base || !have_world || !have_division) {
    throw ToolError(
        ExitCode::format,
        "track definition is missing its name, division, base path, or world");
  }
  return result;
}

TrackDefinition read_track_definition(const std::filesystem::path &path,
                                      const std::uint64_t maximum_file_bytes) {
  const auto envelope =
      read_tbl(path, "EQ", maximum_file_bytes, maximum_file_bytes);
  if (!envelope.decoded) {
    throw ToolError(ExitCode::format, "track TBL payload is not decoded: " +
                                          envelope.decode_status);
  }
  return parse_track_definition(envelope.payload);
}

} // namespace mh::content
