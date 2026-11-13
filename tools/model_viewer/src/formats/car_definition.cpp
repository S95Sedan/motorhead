#include <formats/car_definition.hpp>

#include <core/error.hpp>
#include <formats/encoded_table.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

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

CarVector parse_vector(const std::string &value, const std::string_view field) {
  std::istringstream input(value);
  input.imbue(std::locale::classic());
  CarVector result;
  std::string trailing;
  if (!(input >> result.x >> result.y >> result.z) || (input >> trailing) ||
      !std::isfinite(result.x) || !std::isfinite(result.y) ||
      !std::isfinite(result.z)) {
    throw ToolError(ExitCode::format,
                    "CAR field " + std::string(field) +
                        " must contain exactly three finite numbers");
  }
  return result;
}

CarColor parse_color(const std::string &value, const std::string_view field) {
  std::istringstream input(value);
  std::array<unsigned int, 3U> channels{};
  std::string trailing;
  if (!(input >> channels[0] >> channels[1] >> channels[2]) ||
      (input >> trailing) ||
      std::any_of(channels.begin(), channels.end(),
                  [](const unsigned int channel) { return channel > 255U; })) {
    throw ToolError(ExitCode::format,
                    "CAR field " + std::string(field) +
                        " must contain exactly three byte values");
  }
  return CarColor{static_cast<std::uint8_t>(channels[0]),
                  static_cast<std::uint8_t>(channels[1]),
                  static_cast<std::uint8_t>(channels[2])};
}

float parse_float_value(const std::string &value,
                        const std::string_view field) {
  std::istringstream input(value);
  input.imbue(std::locale::classic());
  float result = 0.0F;
  std::string trailing;
  if (!(input >> result) || (input >> trailing) || !std::isfinite(result)) {
    throw ToolError(ExitCode::format, "CAR field " + std::string(field) +
                                          " must contain one finite number");
  }
  return result;
}

std::vector<float> parse_float_list(const std::string &value,
                                    const std::string_view field) {
  std::istringstream input(value);
  input.imbue(std::locale::classic());
  std::vector<float> result;
  float item = 0.0F;
  while (input >> item) {
    if (!std::isfinite(item)) {
      throw ToolError(ExitCode::format, "CAR field " + std::string(field) +
                                            " contains a non-finite number");
    }
    result.push_back(item);
    if (result.size() > 16U) {
      throw ToolError(ExitCode::format, "CAR field " + std::string(field) +
                                            " exceeds 16 numbers");
    }
  }
  if (!input.eof() || result.size() < 2U) {
    throw ToolError(ExitCode::format, "CAR field " + std::string(field) +
                                          " must contain 2 through 16 numbers");
  }
  return result;
}

std::pair<std::string, std::string> parse_wheel(const std::string &value,
                                                const std::string_view field) {
  std::istringstream input(value);
  std::string model;
  std::string texture;
  std::string trailing;
  if (!(input >> model >> texture) || (input >> trailing)) {
    throw ToolError(ExitCode::format,
                    "CAR field " + std::string(field) +
                        " must contain a model and texture path");
  }
  return {model, texture};
}

std::uint32_t parse_u32_value(const std::string &value,
                              const std::string_view field) {
  std::istringstream input(value);
  std::uint64_t result = 0U;
  std::string trailing;
  if (!(input >> result) || (input >> trailing) || result > 0xffffffffULL) {
    throw ToolError(ExitCode::format, "CAR field " + std::string(field) +
                                          " must contain one unsigned integer");
  }
  return static_cast<std::uint32_t>(result);
}

std::int32_t parse_i32_value(const std::string &value,
                             const std::string_view field) {
  std::istringstream input(value);
  std::int64_t result = 0;
  std::string trailing;
  if (!(input >> result) || (input >> trailing) ||
      result < std::numeric_limits<std::int32_t>::min() ||
      result > std::numeric_limits<std::int32_t>::max()) {
    throw ToolError(ExitCode::format,
                    "CAR field " + std::string(field) +
                        " must contain one signed 32-bit integer");
  }
  return static_cast<std::int32_t>(result);
}

std::array<CarVector, 4U> parse_shadow_points(
    const std::string &value, const std::string_view field) {
  const auto values = parse_float_list(value, field);
  if (values.size() != 12U) {
    throw ToolError(ExitCode::format,
                    "CAR field " + std::string(field) +
                        " must contain four 3D points");
  }
  std::array<CarVector, 4U> result{};
  for (std::size_t point = 0U; point < result.size(); ++point) {
    result[point] = {values[point * 3U], values[point * 3U + 1U],
                     values[point * 3U + 2U]};
  }
  return result;
}

} // namespace

CarDefinition parse_car(const std::span<const std::uint8_t> decoded_text) {
  if (decoded_text.empty() || decoded_text.size() > 1024U * 1024U) {
    throw ToolError(ExitCode::format,
                    "CAR text size is outside supported bounds");
  }
  if (std::find(decoded_text.begin(), decoded_text.end(), 0U) !=
      decoded_text.end()) {
    throw ToolError(ExitCode::format, "CAR text contains a NUL byte");
  }
  const std::string text(decoded_text.begin(), decoded_text.end());
  std::istringstream lines(text);
  CarDefinition result;
  std::array<bool, 4U> wheel_models{};
  std::array<bool, 4U> wheel_centers{};
  bool have_name = false;
  bool have_model = false;
  bool have_texture = false;
  bool have_division = false;
  bool have_center_mass = false;
  std::array<bool, 3U> have_colors{};
  std::array<std::array<CarVector, 4U>, 4U> shadow_points{};
  std::array<bool, 4U> have_shadow_points{};
  CarPhysicsDefinition physics;
  std::array<bool, 13U> have_physics{};
  CarPerformanceLevels performance_levels;
  std::array<bool, 3U> have_performance_levels{};
  CarMenuPerformance menu_performance;
  std::array<bool, 3U> have_menu_performance{};
  const auto set_physics = [&](const std::size_t index,
                               const std::string_view field) {
    if (have_physics[index]) {
      throw ToolError(ExitCode::format,
                      "duplicate CAR physics field " + std::string(field));
    }
    have_physics[index] = true;
  };
  std::string line;
  std::size_t line_number = 0U;
  while (std::getline(lines, line)) {
    ++line_number;
    if (line.size() > 4096U) {
      throw ToolError(ExitCode::format, "CAR line exceeds 4096 bytes");
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
    const auto key = lower_ascii(line.substr(0U, separator));
    const auto value =
        separator == std::string::npos
            ? std::string{}
            : trim(std::string_view(line).substr(separator + 1U));
    if (value.empty()) {
      // Some community CAR exporters leave up to eight bytes of short records.
      // Only tolerate these at EOF; missing values on real fields stay errors.
      const auto position = lines.tellg();
      const auto remaining =
          position == std::streampos(-1)
              ? 0U
              : text.size() - static_cast<std::size_t>(position);
      if (key.size() <= 2U && line.size() + remaining <= 8U) {
        continue;
      }
      throw ToolError(ExitCode::format, "CAR field on line " +
                                            std::to_string(line_number) +
                                            " has no value");
    }
    auto recognized = true;
    if (key == "carname") {
      result.name = value;
      have_name = true;
    } else if (key == "division") {
      result.division = parse_u32_value(value, key);
      have_division = true;
    } else if (key == "filename") {
      result.model_base = value;
      have_model = true;
    } else if (key == "texturepath") {
      result.texture_path = value;
      have_texture = true;
    } else if (key == "collisionname") {
      result.collision_path = value;
    } else if (key == "lineobjectname") {
      result.line_object_path = value;
    } else if (key == "halo") {
      if (result.halo_paths.size() >= 64U) {
        throw ToolError(ExitCode::format,
                        "CAR definition has too many halo records");
      }
      result.halo_paths.push_back(value);
    } else if (key == "colour1" || key == "colour2" || key == "colour3") {
      const auto color_index = static_cast<std::size_t>(key.back() - '1');
      result.colors[color_index] = parse_color(value, key);
      have_colors[color_index] = true;
    } else if (key == "shadowpoints1" || key == "shadowpoints2" ||
               key == "shadowpoints3" || key == "shadowpoints4") {
      const auto row = static_cast<std::size_t>(key.back() - '1');
      if (have_shadow_points[row]) {
        throw ToolError(ExitCode::format,
                        "duplicate CAR shadow field " + key);
      }
      shadow_points[row] = parse_shadow_points(value, key);
      have_shadow_points[row] = true;
    } else if (key == "phycentermass") {
      result.center_of_mass = parse_vector(value, key);
      have_center_mass = true;
    } else if (key == "phyweight") {
      set_physics(0U, key);
      physics.weight = parse_float_value(value, key);
    } else if (key == "phygearratio") {
      set_physics(1U, key);
      physics.gear_ratios = parse_float_list(value, key);
    } else if (key == "phyminrpm") {
      set_physics(2U, key);
      physics.minimum_rpm = parse_float_value(value, key);
    } else if (key == "phymaxrpm") {
      set_physics(3U, key);
      physics.maximum_rpm = parse_float_value(value, key);
    } else if (key == "phyaccforce") {
      set_physics(4U, key);
      physics.acceleration_force = parse_float_value(value, key);
    } else if (key == "phybrakeforce") {
      set_physics(5U, key);
      physics.brake_force = parse_float_value(value, key);
    } else if (key == "phyturnforce") {
      set_physics(6U, key);
      physics.turn_force = parse_float_value(value, key);
    } else if (key == "physpringlength") {
      set_physics(7U, key);
      physics.spring_length = parse_float_value(value, key);
    } else if (key == "physpringstrength") {
      set_physics(8U, key);
      physics.spring_strength = parse_float_value(value, key);
    } else if (key == "phycornerfl" || key == "phycornerfr" ||
               key == "phycornerbl" || key == "phycornerbr") {
      const auto corner = key == "phycornerfl"   ? 0U
                          : key == "phycornerfr" ? 1U
                          : key == "phycornerbl" ? 2U
                                                 : 3U;
      set_physics(9U + corner, key);
      physics.corners[corner] = parse_vector(value, key);
    } else if (key == "speedlevel" || key == "acclevel" || key == "griplevel") {
      const auto level = key == "speedlevel" ? 0U : key == "acclevel" ? 1U : 2U;
      if (have_performance_levels[level]) {
        throw ToolError(ExitCode::format,
                        "duplicate CAR performance field " + key);
      }
      have_performance_levels[level] = true;
      const auto parsed = parse_i32_value(value, key);
      if (level == 0U) {
        performance_levels.speed = parsed;
      } else if (level == 1U) {
        performance_levels.acceleration = parsed;
      } else {
        performance_levels.grip = parsed;
      }
    } else if (key == "topspeed" || key == "acceleration" ||
               key == "handling") {
      const auto field = key == "topspeed"       ? 0U
                         : key == "acceleration" ? 1U
                                                 : 2U;
      if (have_menu_performance[field]) {
        throw ToolError(ExitCode::format,
                        "duplicate CAR menu-performance field " + key);
      }
      have_menu_performance[field] = true;
      const auto parsed = parse_float_value(value, key);
      if (field == 0U) {
        menu_performance.top_speed = parsed;
      } else if (field == 1U) {
        menu_performance.acceleration = parsed;
      } else {
        menu_performance.handling = parsed;
      }
    } else if (key == "wheel1" || key == "wheel2" || key == "wheel3" ||
               key == "wheel4") {
      const auto source_index = static_cast<std::size_t>(key.back() - '1');
      const auto target_index =
          source_index; // Original order is FL, FR, BR, BL.
      const auto [model, texture] = parse_wheel(value, key);
      result.wheels[target_index].model_base = model;
      result.wheels[target_index].texture_path = texture;
      wheel_models[target_index] = true;
    } else if (key == "phywheelfl" || key == "phywheelfr" ||
               key == "phywheelbr" || key == "phywheelbl") {
      const auto target_index = key == "phywheelfl"   ? 0U
                                : key == "phywheelfr" ? 1U
                                : key == "phywheelbr" ? 2U
                                                      : 3U;
      result.wheels[target_index].center = parse_vector(value, key);
      wheel_centers[target_index] = true;
    } else {
      recognized = false;
    }
    if (recognized) {
      ++result.recognized_fields;
    } else {
      ++result.unknown_fields;
    }
  }
  if (!have_name || !have_model || !have_texture || !have_division ||
      !have_center_mass ||
      !std::all_of(have_colors.begin(), have_colors.end(),
                   [](const bool value) { return value; }) ||
      !std::all_of(wheel_models.begin(), wheel_models.end(),
                   [](const bool value) { return value; }) ||
      !std::all_of(wheel_centers.begin(), wheel_centers.end(),
                   [](const bool value) { return value; })) {
    throw ToolError(
        ExitCode::format,
        "CAR definition is missing a required identity, model, or wheel field");
  }
  const auto physics_fields = static_cast<std::size_t>(
      std::count(have_physics.begin(), have_physics.end(), true));
  if (physics_fields != 0U && physics_fields != have_physics.size()) {
    throw ToolError(ExitCode::format,
                    "CAR definition has an incomplete authored physics block");
  }
  if (physics_fields == have_physics.size()) {
    const auto positive_ratios =
        std::all_of(physics.gear_ratios.begin(), physics.gear_ratios.end(),
                    [](const float ratio) { return ratio > 0.0F; });
    if (physics.weight <= 0.0F || physics.minimum_rpm < 0.0F ||
        physics.maximum_rpm <= physics.minimum_rpm ||
        physics.acceleration_force <= 0.0F || physics.brake_force < 0.0F ||
        physics.turn_force <= 0.0F || physics.spring_length <= 0.0F ||
        physics.spring_strength <= 0.0F || !positive_ratios) {
      throw ToolError(
          ExitCode::format,
          "CAR authored physics values are outside their structural bounds");
    }
    result.physics = std::move(physics);
  }
  const auto performance_fields = static_cast<std::size_t>(std::count(
      have_performance_levels.begin(), have_performance_levels.end(), true));
  if (performance_fields != 0U &&
      performance_fields != have_performance_levels.size()) {
    throw ToolError(ExitCode::format,
                    "CAR definition has an incomplete performance-level block");
  }
  if (performance_fields == have_performance_levels.size()) {
    result.performance_levels = performance_levels;
  }
  const auto menu_performance_fields = static_cast<std::size_t>(std::count(
      have_menu_performance.begin(), have_menu_performance.end(), true));
  if (menu_performance_fields != 0U &&
      menu_performance_fields != have_menu_performance.size()) {
    throw ToolError(ExitCode::format,
                    "CAR definition has an incomplete menu-performance block");
  }
  if (menu_performance_fields == have_menu_performance.size()) {
    if (menu_performance.top_speed <= 0.0F ||
        menu_performance.acceleration <= 0.0F ||
        menu_performance.handling <= 0.0F) {
      throw ToolError(ExitCode::format,
                      "CAR menu-performance values must be positive");
    }
    result.menu_performance = menu_performance;
  }
  const auto shadow_fields = static_cast<std::size_t>(std::count(
      have_shadow_points.begin(), have_shadow_points.end(), true));
  if (shadow_fields != 0U && shadow_fields != have_shadow_points.size()) {
    throw ToolError(ExitCode::format,
                    "CAR definition has an incomplete shadow-point block");
  }
  if (shadow_fields == have_shadow_points.size()) {
    result.shadow_points = shadow_points;
  }
  return result;
}

CarDefinition read_car(const std::filesystem::path &path,
                       const std::uint64_t maximum_file_bytes) {
  const auto envelope =
      read_tbl(path, "EQ", maximum_file_bytes, maximum_file_bytes);
  if (!envelope.decoded) {
    throw ToolError(ExitCode::format, "CAR TBL payload is not decoded: " +
                                          envelope.decode_status);
  }
  return parse_car(envelope.payload);
}

std::uint8_t exact_car_color_slot_mask(
    const std::array<std::uint8_t, 3U> &authored_color,
    const std::array<CarColor, 3U> &default_colors) {
  std::uint8_t result = 0U;
  for (std::size_t slot = 0U; slot < default_colors.size(); ++slot) {
    const auto &color = default_colors[slot];
    if (authored_color ==
        std::array<std::uint8_t, 3U>{color.red, color.green, color.blue}) {
      result = static_cast<std::uint8_t>(result | (1U << slot));
    }
  }
  return result;
}

std::optional<std::size_t> exact_car_color_slot(
    const std::array<std::uint8_t, 3U> &authored_color,
    const std::array<CarColor, 3U> &default_colors) {
  const auto mask = exact_car_color_slot_mask(authored_color, default_colors);
  if (mask == 0U || (mask & static_cast<std::uint8_t>(mask - 1U)) != 0U) {
    return std::nullopt;
  }
  for (std::size_t slot = 0U; slot < default_colors.size(); ++slot) {
    if ((mask & (1U << slot)) != 0U) {
      return slot;
    }
  }
  return std::nullopt;
}

CarFaceBaseColor select_car_face_base_color(
    const MyoFace &face,
    const std::array<CarColor, 3U> &default_colors,
    const std::array<CarColor, 3U> &active_colors,
    const bool apply_color_slots) {
  const auto environment_paint =
      face.primitive_type == 8U || face.primitive_type == 9U;
  if (face.has_texture_coordinates && !environment_paint) {
    return {{255U, 255U, 255U}, std::nullopt, true};
  }
  if (apply_color_slots) {
    const auto slot = exact_car_color_slot(face.color, default_colors);
    if (slot.has_value()) {
      const auto &color = active_colors[*slot];
      return {{color.red, color.green, color.blue}, slot, true};
    }
  }
  return {face.color, std::nullopt, true};
}

std::array<std::optional<CarWheelAnchor>, 4U>
recover_embedded_wheel_anchors(const MyoData &body,
                               const std::array<CarWheel, 4U> &wheels) {
  std::array<std::optional<CarWheelAnchor>, 4U> result{};
  for (std::size_t wheel_index = 0U; wheel_index < wheels.size();
       ++wheel_index) {
    const auto &wheel = wheels[wheel_index];
    std::vector<bool> used_positions(body.positions.size(), false);
    std::size_t attributed_faces = 0U;
    for (const auto &face : body.faces) {
      if (!face.has_texture_coordinates || face.vertex_count < 3U ||
          face.vertex_count > 4U) {
        continue;
      }
      std::array<float, 3U> minimum{std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max(),
                                    std::numeric_limits<float>::max()};
      std::array<float, 3U> maximum{std::numeric_limits<float>::lowest(),
                                    std::numeric_limits<float>::lowest(),
                                    std::numeric_limits<float>::lowest()};
      for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
        const auto &position = body.positions[face.position_indices[vertex]];
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
          minimum[axis] = std::min(minimum[axis], position[axis]);
          maximum[axis] = std::max(maximum[axis], position[axis]);
        }
      }
      const auto center_x = (minimum[0] + maximum[0]) * 0.5F;
      const auto center_z = (minimum[2] + maximum[2]) * 0.5F;
      const auto same_side = (center_x < 0.0F) == (wheel.center.x < 0.0F);
      if (maximum[0] - minimum[0] >= 0.25F || !same_side ||
          std::abs(center_x) < std::abs(wheel.center.x) * 0.9F ||
          std::abs(center_z - wheel.center.z) >= 0.7F || minimum[1] >= 0.45F ||
          maximum[1] >= 1.25F || maximum[1] - minimum[1] <= 0.15F ||
          maximum[2] - minimum[2] <= 0.15F) {
        continue;
      }
      ++attributed_faces;
      for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
        used_positions[face.position_indices[vertex]] = true;
      }
    }
    if (attributed_faces == 0U || attributed_faces > 8U) {
      continue;
    }
    std::array<float, 3U> minimum{std::numeric_limits<float>::max(),
                                  std::numeric_limits<float>::max(),
                                  std::numeric_limits<float>::max()};
    std::array<float, 3U> maximum{std::numeric_limits<float>::lowest(),
                                  std::numeric_limits<float>::lowest(),
                                  std::numeric_limits<float>::lowest()};
    for (std::size_t index = 0U; index < used_positions.size(); ++index) {
      if (!used_positions[index]) {
        continue;
      }
      for (std::size_t axis = 0U; axis < 3U; ++axis) {
        minimum[axis] = std::min(minimum[axis], body.positions[index][axis]);
        maximum[axis] = std::max(maximum[axis], body.positions[index][axis]);
      }
    }
    result[wheel_index] =
        CarWheelAnchor{CarVector{(minimum[0] + maximum[0]) * 0.5F,
                                 (minimum[1] + maximum[1]) * 0.5F,
                                 (minimum[2] + maximum[2]) * 0.5F},
                       attributed_faces};
  }
  return result;
}

} // namespace mh::content
