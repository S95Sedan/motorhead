#pragma once

#include <game/physics/body_response.hpp>
#include <game/physics/wheel_contact.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace mh::game {

struct OriginalVehicleResponseConfig {
  WheelProbeRig wheel_rig{};
  CollisionVector3 center_of_mass{};
  OriginalBodyMassProperties body{};
  OriginalWheelResponseProfile response_profile =
      OriginalWheelResponseProfile::standard;
  OriginalVehicleGroundedMaterialTable grounded_materials{};
};

struct OriginalVehicleResponseStepInputs {
  VehicleTransform transform{};
  OriginalWheelDynamicVectorInputs dynamic_vector{};
  std::array<OriginalWheelSurfaceScaleInputs, 4U> surface_scales{};
  std::uint8_t global_flags = 0U;
  double slice_seconds = 0.0;
  CollisionVector3 initial_angular_force{};
};

struct OriginalVehicleResponsePoseStepInputs {
  OriginalBodyPoseState pose{};
  OriginalWheelDynamicVectorInputs dynamic_vector{};
  std::array<OriginalWheelSurfaceScaleInputs, 4U> surface_scales{};
  std::uint8_t global_flags = 0U;
  double slice_seconds = 0.0;
  std::optional<std::array<CollisionVector3, 4U>> prepared_base_forces;
  CollisionVector3 initial_angular_force{};
  bool reuse_retained_wheel_surfaces = false;
  bool use_reduced_surface_scale = false;
  double retained_traction_accumulator = 0.0;
};

struct OriginalVehicleResponseFrame {
  WheelContactFrame contacts{};
  std::array<OriginalWheelForceResult, 4U> wheel_forces{};
  BodyForceAccumulator accumulated_force{};
  OriginalBodyVelocityState velocity{};
};

// Owns the stateful four-wheel contact history and applies one recovered
// force-response slice. The vehicle scene invokes this boundary three times
// per outer slice and advances the complete three-axis pose after each call.
class OriginalVehicleResponseSystem {
public:
  explicit OriginalVehicleResponseSystem(OriginalVehicleResponseConfig config);

  [[nodiscard]] OriginalVehicleResponseFrame
  step(OriginalBodyVelocityState &velocity,
       const OriginalVehicleResponseStepInputs &inputs,
       const CollisionWorld &world);

  [[nodiscard]] OriginalVehicleResponseFrame
  step(OriginalBodyVelocityState &velocity,
       const OriginalVehicleResponsePoseStepInputs &inputs,
       const CollisionWorld &world);

  [[nodiscard]] const OriginalVehicleResponseConfig &config() const noexcept;
  void seed_wheel_contact_history(
      const std::array<WheelContactScalarState, 4U> &current);
  void begin_response_group() noexcept;
  void reset() noexcept;

private:
  OriginalVehicleResponseConfig config_;
  WheelContactSystem contacts_;
};

} // namespace mh::game
