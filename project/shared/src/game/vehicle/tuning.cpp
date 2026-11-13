#include <game/vehicle/tuning.hpp>

#include <cmath>
#include <stdexcept>
#include <string>

namespace mh::game {
namespace {

void require_finite(const float value, const char *name) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(name) + " must be finite");
  }
}

void require_retail_level(const std::int32_t value, const char *name) {
  if (value < 0 || value > 10) {
    throw std::invalid_argument(std::string(name) +
                                " is outside the recovered retail range");
  }
}

void validate_input(const RecoveredVehicleSetupInput &input) {
  if (input.gear_values.empty() || input.gear_values.size() > 7U) {
    throw std::invalid_argument(
        "recovered vehicle setup requires one through seven gear values");
  }
  for (const auto value : input.gear_values) {
    require_finite(value, "gear value");
    if (value <= 0.0F) {
      throw std::invalid_argument("gear values must be positive");
    }
  }
  require_finite(input.minimum_rpm, "minimum RPM");
  require_finite(input.maximum_rpm, "maximum RPM");
  require_finite(input.acceleration_force, "acceleration force");
  require_finite(input.brake_force, "brake force");
  require_finite(input.turn_force, "turn force");
  if (input.minimum_rpm < 0.0F ||
      input.maximum_rpm <= input.minimum_rpm ||
      input.acceleration_force < 0.0F || input.brake_force < 0.0F ||
      input.turn_force < 0.0F) {
    throw std::invalid_argument(
        "recovered vehicle setup values are outside their valid range");
  }
  require_retail_level(input.speed_level, "speed level");
  require_retail_level(input.acceleration_level, "acceleration level");
  require_retail_level(input.grip_level, "grip level");
}

} // namespace

RecoveredVehicleRuntimeTuning make_recovered_vehicle_runtime_tuning(
    const RecoveredVehicleSetupInput &input) {
  validate_input(input);

  const auto gear_scale =
      static_cast<float>(input.speed_level - 5) * 0.05F + 1.05F;
  RecoveredVehicleRuntimeTuning output;
  output.gear_values.reserve(input.gear_values.size());
  for (std::size_t index = 0U; index < input.gear_values.size(); ++index) {
    // Both p3.1 initializers copy the first authored ratio directly. Their
    // indexed scale loop begins at ratio one (RVA 0x0001c779 in the primary
    // initializer and 0x0001c53a in the CPU alternate initializer).
    output.gear_values.push_back(index == 0U
                                     ? input.gear_values[index]
                                     : input.gear_values[index] * gear_scale);
  }
  output.minimum_rpm = input.minimum_rpm;
  output.maximum_rpm = input.maximum_rpm;
  output.acceleration_force =
      input.acceleration_force +
      static_cast<float>(input.acceleration_level - 5) * 250.0F;
  output.brake_force = input.brake_force;
  output.turn_force = input.turn_force;
  output.grip_level = input.grip_level;
  constexpr std::array<double, 4U> lower{0.75, 0.75, 1.3, 1.1};
  constexpr std::array<double, 4U> upper{1.3, 1.3, 0.75, 0.95};
  const auto grip_fraction = static_cast<double>(input.grip_level) * 0.1;
  for (std::size_t index = 0U;
       index < output.grip_response_constants.size(); ++index) {
    output.grip_response_constants[index] = static_cast<float>(
        lower[index] + (upper[index] - lower[index]) * grip_fraction);
  }
  return output;
}

} // namespace mh::game
