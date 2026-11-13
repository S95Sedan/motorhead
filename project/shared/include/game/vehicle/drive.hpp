#pragma once

#include <game/physics/simulation.hpp>
#include <game/vehicle/scene.hpp>
#include <game/vehicle/tuning.hpp>

#include <optional>

namespace mh::game {

struct OriginalVehicleDriveState {
  std::size_t current_gear_index = 1U;
  double engine_scalar = 0.0;
  double steering_scalar = 0.0;
  bool engine_transition_active = false;
  bool direction_transition_active = false;
  bool first_shift_latched = true;
  bool second_shift_latched = true;
};

struct VehicleLaunchTractionFrame {
  ControlInput controls{};
  // Normalized straight-line tire slip used by presentation effects. Zero is
  // full grip and one is the strongest standing-start wheelspin.
  double wheelspin = 0.0;
};

// Models the short traction-limited interval after the starting signal. A
// preloaded engine and excessive accelerator request spin the driven tires,
// reducing the force that reaches the road. Modulating the accelerator below
// the slip threshold produces the quicker launch.
class VehicleLaunchTractionRuntime {
public:
  void begin(double engine_scalar, double minimum_rpm, double maximum_rpm);
  [[nodiscard]] VehicleLaunchTractionFrame step(const ControlInput &controls,
                                                double longitudinal_velocity,
                                                double slice_seconds);
  void reset() noexcept;

  [[nodiscard]] bool active() const noexcept;

private:
  double remaining_seconds_ = 0.0;
  double engine_preload_ = 0.0;
};

// Stateful owner for the recovered automatic mode-C drivetrain. It converts
// canonical player controls into one scene-step input and retains the gear/RPM
// transition produced by that step.
class OriginalVehicleDriveSystem {
public:
  explicit OriginalVehicleDriveSystem(RecoveredVehicleRuntimeTuning tuning);

  [[nodiscard]] OriginalVehicleModeCSceneStepInputs
  prepare_step(const ControlInput &controls, double longitudinal_velocity,
               double slice_seconds);
  void commit_step(const OriginalDrivetrainModeCForceResult &result);
  // Advances only the recovered gearbox/RPM state. Race staging uses this
  // while the start owner holds the physical body on its grid slot.
  void advance_state_only(const ControlInput &controls,
                          double longitudinal_velocity, double slice_seconds);
  // Applies p3.1's live catch-up factor to immutable post-initializer backups.
  // Ratio zero, brake, turn, grip and RPM values remain unchanged.
  void apply_performance_scale(float factor);
  // Selects the retail automatic or manual gear owner. Manual shifts use the
  // recovered +0x58/+0x5c latch state and therefore commit on button release.
  void set_automatic_transmission(bool automatic) noexcept;
  // Restores the exact retained mode-C state from a captured runtime sample.
  // This is used by one-step original/reconstruction physics comparators.
  void seed_captured_state(const OriginalVehicleDriveState &state);
  // Clears only the live drivetrain state. Unlike a full race reset, the
  // p3.1 race-level recovery path preserves any active catch-up scaling.
  void reset_retained_state() noexcept;
  void reset() noexcept;

  [[nodiscard]] const RecoveredVehicleRuntimeTuning &tuning() const noexcept;
  [[nodiscard]] const OriginalVehicleDriveState &state() const noexcept;
  // CAR Gear values are authored in kilometres per hour. Manual gears are
  // physical speed ranges, so the selected gear supplies a hard longitudinal
  // ceiling to the body owner. Automatic mode retains its recovered shift
  // path and therefore has no externally applied ceiling.
  [[nodiscard]] std::optional<double>
  active_manual_speed_ceiling() const noexcept;

private:
  RecoveredVehicleRuntimeTuning base_tuning_;
  RecoveredVehicleRuntimeTuning tuning_;
  OriginalVehicleDriveState state_{};
  bool automatic_transmission_ = true;
};

} // namespace mh::game
