#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <content/formats/myo_object_model.hpp>

namespace mh::content {

struct CarVector {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

struct CarColor {
  std::uint8_t red = 0U;
  std::uint8_t green = 0U;
  std::uint8_t blue = 0U;

  [[nodiscard]] constexpr bool operator==(const CarColor &) const = default;
};

struct CarWheel {
  std::string model_base;
  std::string texture_path;
  CarVector center;
};

struct CarPhysicsDefinition {
  float weight = 0.0F;
  std::vector<float> gear_ratios;
  float minimum_rpm = 0.0F;
  float maximum_rpm = 0.0F;
  float acceleration_force = 0.0F;
  float brake_force = 0.0F;
  float turn_force = 0.0F;
  float spring_length = 0.0F;
  float spring_strength = 0.0F;
  std::array<CarVector, 4U> corners{}; // FL, FR, BL, BR
};

struct CarPerformanceLevels {
  std::int32_t speed = 0;
  std::int32_t acceleration = 0;
  std::int32_t grip = 0;
};

// Authored menu-display values consumed directly by the p3.1 car-selection
// renderer at runtime-record offsets 0x1214, 0x1228, and 0x122c.
struct CarMenuPerformance {
  float top_speed = 0.0F;
  float acceleration = 0.0F;
  float handling = 0.0F;
};

struct CarDefinition {
  std::string name;
  std::string model_base;
  std::string texture_path;
  std::string collision_path;
  std::string line_object_path;
  // Repeated CAR Halo records are ordered runtime sprite dependencies.  Every
  // shipping vehicle supplies its front and rear lamp halo images here.
  std::vector<std::string> halo_paths;
  std::uint32_t division = 0U;
  std::array<CarWheel, 4U> wheels{}; // FL, FR, BR, BL
  std::array<CarColor, 3U> colors{};
  // Four authored lateral rows of four local-space shadow vertices. p3.1 CAR
  // files name these shadowpoints1..4 and retain all 16 points.
  std::optional<std::array<std::array<CarVector, 4U>, 4U>> shadow_points;
  CarVector center_of_mass;
  std::optional<CarPhysicsDefinition> physics;
  std::optional<CarPerformanceLevels> performance_levels;
  std::optional<CarMenuPerformance> menu_performance;
  std::size_t recognized_fields = 0U;
  std::size_t unknown_fields = 0U;
};

struct CarWheelAnchor {
  CarVector outer_center;
  std::size_t attributed_faces = 0U;
};

struct CarColorSlotAudit {
  std::array<std::uint64_t, 3U> face_counts{};
  std::array<std::uint64_t, 3U> untextured_face_counts{};
  std::array<std::uint64_t, 3U> textured_face_counts{};
  std::array<std::uint64_t, 3U> environment_face_counts{};
  std::array<std::uint64_t, 3U> reflective_face_counts{};
  std::uint64_t unmatched_face_count = 0U;
  std::uint64_t unmatched_black_face_count = 0U;
  std::uint64_t ambiguous_face_count = 0U;
};

struct CarFaceBaseColor {
  std::array<std::uint8_t, 3U> rgb{};
  std::optional<std::size_t> slot;
  bool apply_object_lighting = true;
};

[[nodiscard]] CarDefinition
parse_car(std::span<const std::uint8_t> decoded_text);
[[nodiscard]] CarDefinition
read_car(const std::filesystem::path &path,
         std::uint64_t maximum_file_bytes = 1024ULL * 1024ULL);
[[nodiscard]] std::uint8_t exact_car_color_slot_mask(
    const std::array<std::uint8_t, 3U> &authored_color,
    const std::array<CarColor, 3U> &default_colors);
[[nodiscard]] std::optional<std::size_t> exact_car_color_slot(
    const std::array<std::uint8_t, 3U> &authored_color,
    const std::array<CarColor, 3U> &default_colors);
[[nodiscard]] CarColorSlotAudit
audit_car_color_slots(const MyoData &model,
                      const std::array<CarColor, 3U> &default_colors);
[[nodiscard]] CarFaceBaseColor select_car_face_base_color(
    const MyoFace &face,
    const std::array<CarColor, 3U> &default_colors,
    const std::array<CarColor, 3U> &active_colors,
    bool apply_color_slots);
[[nodiscard]] std::array<std::optional<CarWheelAnchor>, 4U>
recover_embedded_wheel_anchors(const MyoData &body,
                               const std::array<CarWheel, 4U> &wheels);

} // namespace mh::content
