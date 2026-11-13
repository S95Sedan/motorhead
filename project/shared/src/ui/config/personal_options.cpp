#include <ui/config/personal_options.hpp>
#include <ui/config/game_config.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mh::ui {
namespace {

bool ascii_equal_case_insensitive(const std::string_view left,
                                  const std::string_view right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.size(); ++index) {
    const auto lower = [](const unsigned char character) {
      return static_cast<unsigned char>(std::tolower(character));
    };
    if (lower(static_cast<unsigned char>(left[index])) !=
        lower(static_cast<unsigned char>(right[index]))) {
      return false;
    }
  }
  return true;
}

PersonalColour parse_personal_colour(const std::string &value,
                                     const PersonalColour fallback) {
  std::istringstream stream(value);
  std::int32_t red = 0;
  std::int32_t green = 0;
  std::int32_t blue = 0;
  if (!(stream >> red >> green >> blue)) {
    return fallback;
  }
  return {
      static_cast<std::uint8_t>(std::clamp(red, 0, 255)),
      static_cast<std::uint8_t>(std::clamp(green, 0, 255)),
      static_cast<std::uint8_t>(std::clamp(blue, 0, 255)),
  };
}

} // namespace

PersonalOptionsConfiguration load_personal_options_configuration(
    const std::filesystem::path &reference_root) {
  const auto source = motorhead_configuration_path(reference_root);
  PersonalOptionsConfiguration result;
  if (const auto value = configuration_file_value(source, "PlayerName");
      !value.empty()) {
    result.player_name = value;
  }
  result.team_name = configuration_file_value(source, "TeamName");
  result.player_colour = parse_personal_colour(
      configuration_file_value(source, "PlayerColour"), result.player_colour);
  result.measurement_system =
      ascii_equal_case_insensitive(configuration_file_value(source, "System"),
                                   "Imperial")
          ? MeasurementSystem::imperial
          : MeasurementSystem::metric;
  for (std::size_t index = 0U; index < result.short_keys.size(); ++index) {
    result.short_keys[index] =
        configuration_file_value(source, "ShortKey" + std::to_string(index));
  }
  result.custom_car_colours = ascii_equal_case_insensitive(
      configuration_file_value(source, "CustomCarColours"), "On");
  for (std::size_t index = 0U; index < result.car_colours.size(); ++index) {
    result.car_colours[index] = parse_personal_colour(
        configuration_file_value(source,
                                 "CarColour" + std::to_string(index + 1U)),
        result.car_colours[index]);
  }
  return result;
}

void save_personal_options_configuration(
    const std::filesystem::path &reference_root,
    const PersonalOptionsConfiguration &configuration) {
  ConfigurationUpdates updates;
  const auto colour = [&updates](const std::string &name,
                                 const PersonalColour value) {
    updates.emplace_back(
        name, std::to_string(static_cast<std::uint32_t>(value.red)) + " " +
                  std::to_string(static_cast<std::uint32_t>(value.green)) +
                  " " +
                  std::to_string(static_cast<std::uint32_t>(value.blue)));
  };
  updates.emplace_back("PlayerName", configuration.player_name);
  updates.emplace_back("TeamName", configuration.team_name);
  colour("PlayerColour", configuration.player_colour);
  updates.emplace_back(
      "System", configuration.measurement_system == MeasurementSystem::metric
                    ? "Metric"
                    : "Imperial");
  for (std::size_t index = 0U; index < configuration.short_keys.size();
       ++index) {
    updates.emplace_back("ShortKey" + std::to_string(index),
                         configuration.short_keys[index]);
  }
  updates.emplace_back("CustomCarColours",
                       configuration.custom_car_colours ? "On" : "Off");
  for (std::size_t index = 0U; index < configuration.car_colours.size();
       ++index) {
    colour("CarColour" + std::to_string(index + 1U),
           configuration.car_colours[index]);
  }
  update_motorhead_configuration(reference_root, updates);
}

std::vector<std::string> playable_personal_arguments(
    const PersonalOptionsConfiguration &configuration) {
  std::vector<std::string> result{
      "--player-name",
      configuration.player_name,
      "--team-name",
      configuration.team_name,
      "--player-colour",
      std::to_string(configuration.player_colour.red) + "," +
          std::to_string(configuration.player_colour.green) + "," +
          std::to_string(configuration.player_colour.blue),
      "--custom-car-colours",
      configuration.custom_car_colours ? "on" : "off",
  };
  result.reserve(result.size() + configuration.short_keys.size() * 2U +
                 configuration.car_colours.size() * 2U);
  for (std::size_t index = 0U; index < configuration.short_keys.size();
       ++index) {
    result.push_back("--short-key" + std::to_string(index));
    result.push_back(configuration.short_keys[index]);
  }
  for (std::size_t index = 0U; index < configuration.car_colours.size();
       ++index) {
    const auto &colour = configuration.car_colours[index];
    result.push_back("--car-colour" + std::to_string(index + 1U));
    result.push_back(std::to_string(colour.red) + "," +
                     std::to_string(colour.green) + "," +
                     std::to_string(colour.blue));
  }
  return result;
}

} // namespace mh::ui
