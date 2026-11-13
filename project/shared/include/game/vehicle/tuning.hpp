#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace mh::game {

// Neutral input boundary between imported CAR data and recovered runtime setup.
// It intentionally excludes fields whose frame-time use is not yet proven.
struct RecoveredVehicleSetupInput {
  std::vector<float> gear_values;
  float minimum_rpm = 0.0F;
  float maximum_rpm = 0.0F;
  float acceleration_force = 0.0F;
  float brake_force = 0.0F;
  float turn_force = 0.0F;
  std::int32_t speed_level = 0;
  std::int32_t acceleration_level = 0;
  std::int32_t grip_level = 0;
};

struct RecoveredVehicleRuntimeTuning {
  std::vector<float> gear_values;
  float minimum_rpm = 0.0F;
  float maximum_rpm = 0.0F;
  float acceleration_force = 0.0F;
  float brake_force = 0.0F;
  float turn_force = 0.0F;
  float grip_level = 0.0F;
  // Runtime drivetrain +0x8c. Primary player setup retains zero; the p3.1
  // Quick Race CPU alternate initializer writes two, selecting acceleration
  // mode B inside the otherwise shared automatic drivetrain path.
  std::uint32_t acceleration_selector = 0U;
  // Four neutral data-flow constants interpolated by the original primary
  // initializer. Their physical consumers remain intentionally unnamed.
  std::array<float, 4U> grip_response_constants{};
};

[[nodiscard]] RecoveredVehicleRuntimeTuning
make_recovered_vehicle_runtime_tuning(const RecoveredVehicleSetupInput &input);

} // namespace mh::game
