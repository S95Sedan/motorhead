#include <game/race/drive_session.hpp>

namespace mh::game {

DriveSession::DriveSession(const RaceConfig race_config) : race_(race_config) {}

DriveSessionTick DriveSession::step(const PlayerTickInput &input,
                                    const SimulationDuration elapsed) {
  if (input.restart_pressed ||
      (input.start_pressed && race_.progress().phase == RacePhase::finished)) {
    restart();
    return {{}, true, {}};
  }

  RaceStartEvents start_events;
  if (input.toggle_camera_pressed) {
    switch (camera_mode_) {
    case VehicleCameraMode::chase:
      camera_mode_ = VehicleCameraMode::far_chase;
      break;
    case VehicleCameraMode::far_chase:
      camera_mode_ = VehicleCameraMode::in_car;
      break;
    case VehicleCameraMode::in_car:
      camera_mode_ = VehicleCameraMode::bumper;
      break;
    case VehicleCameraMode::bumper:
      camera_mode_ = VehicleCameraMode::chase;
      break;
    }
  }
  // p3.1 exposes direct InCarView and OutCarView actions in addition to any
  // front-end CarView default. Keep the reconstruction's extra camera cycle,
  // but let the original direct actions select their exact two stable states.
  if (input.select_in_car_camera_pressed) {
    camera_mode_ = VehicleCameraMode::in_car;
  }
  if (input.select_out_car_camera_pressed) {
    camera_mode_ = VehicleCameraMode::chase;
  }
  if (input.select_bumper_camera_pressed) {
    camera_mode_ = VehicleCameraMode::bumper;
  }
  if (input.select_far_camera_pressed) {
    camera_mode_ = VehicleCameraMode::far_chase;
  }
  if (input.start_pressed && race_.progress().phase == RacePhase::ready) {
    race_.begin_countdown();
    start_events.countdown_three =
        race_.config().countdown_lead_in_duration ==
        SimulationDuration::zero();
  }

  const auto previous_phase = race_.progress().phase;
  const auto previous_countdown = race_.progress().countdown_remaining;
  race_.advance(elapsed);
  if (previous_phase == RacePhase::countdown) {
    const auto current_countdown =
        race_.progress().phase == RacePhase::countdown
            ? race_.progress().countdown_remaining
            : SimulationDuration::zero();
    const auto countdown_phase =
        (race_.config().countdown_duration -
         race_.config().countdown_lead_in_duration) /
        3;
    const auto crossed = [previous_countdown, current_countdown](
                             const SimulationDuration boundary) {
      return previous_countdown > boundary && current_countdown <= boundary;
    };
    start_events.countdown_three =
        start_events.countdown_three || crossed(countdown_phase * 3);
    start_events.countdown_two = crossed(countdown_phase * 2);
    start_events.countdown_one = crossed(countdown_phase);
    start_events.go = race_.progress().phase == RacePhase::racing;
  }
  const auto driving = race_.progress().phase == RacePhase::racing;
  return {driving ? input.controls : ControlInput{}, false, start_events};
}

bool DriveSession::cross_checkpoint(const std::size_t checkpoint_index) {
  return race_.cross_checkpoint(checkpoint_index);
}

bool DriveSession::cross_finish_line() { return race_.cross_finish_line(); }

void DriveSession::restart() noexcept {
  race_.restart();
  camera_mode_ = VehicleCameraMode::chase;
}

const RaceSession &DriveSession::race() const noexcept { return race_; }

VehicleCameraMode DriveSession::camera_mode() const noexcept {
  return camera_mode_;
}

void DriveSession::set_camera_mode(const VehicleCameraMode mode) noexcept {
  camera_mode_ = mode;
}

} // namespace mh::game
