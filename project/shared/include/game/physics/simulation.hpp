#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace mh::game {

using SimulationDuration = std::chrono::nanoseconds;
inline constexpr auto fixed_step = std::chrono::milliseconds(10);
// Recovered from the p3.1 frame scheduler. This is a maximum accepted slice,
// not evidence that the original game ran at a fixed 25 Hz.
inline constexpr auto original_maximum_physics_slice =
    std::chrono::milliseconds(40);
static_assert(fixed_step <= original_maximum_physics_slice);

// The p3.1 frame owner derives a variable elapsed interval from QPC, converts
// it to seconds, and clamps it before entering the bounded physics scheduler.
inline constexpr float original_minimum_frame_elapsed_seconds = 0.00001F;
inline constexpr float original_maximum_frame_elapsed_seconds = 1.0F;
inline constexpr float original_maximum_physics_slice_seconds = 0.04F;
inline constexpr std::size_t original_maximum_physics_slice_count = 10U;

struct OriginalPhysicsSliceSchedule {
  float frame_elapsed_seconds = 0.0F;
  std::array<float, original_maximum_physics_slice_count> slices{};
  std::size_t slice_count = 0U;
  float simulated_seconds = 0.0F;
  float discarded_seconds = 0.0F;
};

[[nodiscard]] float
clamp_original_frame_elapsed_seconds(double elapsed_seconds);

// Reproduces the original outer-frame clamp and the scheduler's at-most-ten
// min(remaining, 0.04f) slices. Time beyond the ten-slice guard is reported
// explicitly instead of silently becoming part of the modern fixed-step clock.
[[nodiscard]] OriginalPhysicsSliceSchedule
make_original_physics_slice_schedule(double elapsed_seconds);

struct ControlInput {
  double throttle = 0.0;
  double brake = 0.0;
  double steering = 0.0;
  bool handbrake = false;
  bool shift_up = false;
  bool shift_down = false;
};

struct VehicleParameters {
  double forward_acceleration = 0.0;
  double reverse_acceleration = 0.0;
  double braking_deceleration = 0.0;
  double linear_drag = 0.0;
  double wheelbase = 1.0;
  double maximum_steering_angle = 0.0;
  double maximum_forward_speed = 0.0;
  double maximum_reverse_speed = 0.0;
};

struct VehicleState {
  double position_x = 0.0;
  double position_z = 0.0;
  double heading_radians = 0.0;
  double speed = 0.0;
  std::uint64_t tick = 0U;
};

class FixedStepClock {
public:
  explicit FixedStepClock(SimulationDuration step = fixed_step);

  [[nodiscard]] std::uint64_t consume(SimulationDuration elapsed);
  [[nodiscard]] SimulationDuration step() const noexcept;
  [[nodiscard]] SimulationDuration remainder() const noexcept;
  [[nodiscard]] double interpolation_alpha() const noexcept;
  void reset() noexcept;

private:
  SimulationDuration step_;
  SimulationDuration remainder_{};
};

class VehicleSimulation {
public:
  explicit VehicleSimulation(VehicleParameters parameters);

  void reset(const VehicleState &state = {});
  void step(const ControlInput &input);

  [[nodiscard]] const VehicleParameters &parameters() const noexcept;
  [[nodiscard]] const VehicleState &state() const noexcept;

private:
  VehicleParameters parameters_;
  VehicleState state_;
};

} // namespace mh::game
