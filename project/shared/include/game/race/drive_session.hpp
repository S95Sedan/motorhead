#pragma once

#include <game/race/camera.hpp>
#include <game/input/player_input.hpp>
#include <game/race/race_state.hpp>

namespace mh::game {

struct RaceStartEvents {
  bool countdown_three = false;
  bool countdown_two = false;
  bool countdown_one = false;
  bool go = false;
};

struct DriveSessionTick {
  ControlInput controls{};
  bool reset_vehicle = false;
  RaceStartEvents start_events{};
};

// Owns the player-facing lifecycle shared by a live game and deterministic
// replay: ready/start, race timing, restart, and camera selection. Vehicle
// physics remains separate and consumes the returned canonical controls.
class DriveSession {
public:
  explicit DriveSession(RaceConfig race_config);

  [[nodiscard]] DriveSessionTick step(const PlayerTickInput &input,
                                      SimulationDuration elapsed);
  [[nodiscard]] bool cross_checkpoint(std::size_t checkpoint_index);
  [[nodiscard]] bool cross_finish_line();
  void restart() noexcept;

  [[nodiscard]] const RaceSession &race() const noexcept;
  [[nodiscard]] VehicleCameraMode camera_mode() const noexcept;
  void set_camera_mode(VehicleCameraMode mode) noexcept;

private:
  RaceSession race_;
  VehicleCameraMode camera_mode_ = VehicleCameraMode::chase;
};

} // namespace mh::game
