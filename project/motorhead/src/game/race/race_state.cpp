#include <game/race/race_state.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mh::game {

void OriginalPostFinishSchedule::begin() noexcept {
  if (!active_) {
    active_ = true;
    elapsed_ = SimulationDuration::zero();
  }
}

void OriginalPostFinishSchedule::advance(const SimulationDuration elapsed) {
  if (elapsed < SimulationDuration::zero()) {
    throw std::invalid_argument("post-finish elapsed time cannot be negative");
  }
  if (!active_) {
    return;
  }
  if (elapsed > SimulationDuration::max() - elapsed_) {
    throw std::overflow_error("post-finish timer overflow");
  }
  elapsed_ += elapsed;
}

void OriginalPostFinishSchedule::reset() noexcept {
  active_ = false;
  elapsed_ = SimulationDuration::zero();
}

bool OriginalPostFinishSchedule::active() const noexcept { return active_; }

bool OriginalPostFinishSchedule::results_due() const noexcept {
  return active_ && elapsed_ > playable_post_finish_results_delay;
}

SimulationDuration OriginalPostFinishSchedule::elapsed() const noexcept {
  return elapsed_;
}

bool crosses_directional_race_gate(
    const DirectionalRaceGate2D &gate,
    const std::array<double, 2U> &previous_position,
    const std::array<double, 2U> &current_position) {
  const auto finite2 = [](const std::array<double, 2U> &values) {
    return std::isfinite(values[0U]) && std::isfinite(values[1U]);
  };
  const auto forward_length = std::hypot(gate.forward[0U], gate.forward[1U]);
  if (!finite2(gate.center) || !finite2(gate.forward) ||
      !finite2(previous_position) || !finite2(current_position) ||
      !std::isfinite(gate.half_width) || gate.half_width <= 0.0 ||
      forward_length <= 1.0e-12) {
    throw std::invalid_argument("directional race gate is invalid");
  }
  const std::array<double, 2U> forward{gate.forward[0U] / forward_length,
                                       gate.forward[1U] / forward_length};
  const auto side = [&gate, &forward](const auto &position) {
    return (position[0U] - gate.center[0U]) * forward[0U] +
           (position[1U] - gate.center[1U]) * forward[1U];
  };
  const auto previous_side = side(previous_position);
  const auto current_side = side(current_position);
  if (previous_side > 0.0 || current_side <= 0.0) {
    return false;
  }
  const auto denominator = current_side - previous_side;
  if (denominator <= 1.0e-12) {
    return false;
  }
  const auto fraction = -previous_side / denominator;
  const std::array<double, 2U> crossing{
      previous_position[0U] +
          (current_position[0U] - previous_position[0U]) * fraction,
      previous_position[1U] +
          (current_position[1U] - previous_position[1U]) * fraction};
  const std::array<double, 2U> lateral{-forward[1U], forward[0U]};
  const auto lateral_distance = (crossing[0U] - gate.center[0U]) * lateral[0U] +
                                (crossing[1U] - gate.center[1U]) * lateral[1U];
  return std::abs(lateral_distance) <= gate.half_width;
}

RaceSession::RaceSession(const RaceConfig config) : config_(config) {
  if (config_.lap_count == 0U || config_.checkpoint_count > 1'000'000U ||
      config_.countdown_duration <= SimulationDuration::zero() ||
      config_.countdown_duration > std::chrono::seconds(12) ||
      config_.countdown_lead_in_duration < SimulationDuration::zero() ||
      config_.countdown_lead_in_duration >= config_.countdown_duration) {
    throw std::invalid_argument(
        "race configuration is outside supported bounds");
  }
  progress_.timing.completed_laps.reserve(config_.lap_count);
}

void RaceSession::begin_countdown() {
  if (progress_.phase != RacePhase::ready) {
    throw std::logic_error(
        "race countdown can only begin from the ready phase");
  }
  progress_.phase = RacePhase::countdown;
  progress_.countdown_remaining = config_.countdown_duration;
}

void RaceSession::start() {
  if (progress_.phase != RacePhase::ready) {
    throw std::logic_error("race can only start from the ready phase");
  }
  progress_.phase = RacePhase::racing;
  progress_.current_lap = 1U;
  progress_.countdown_remaining = SimulationDuration::zero();
}

void RaceSession::advance(const SimulationDuration elapsed) {
  if (elapsed.count() < 0) {
    throw std::invalid_argument("race elapsed time cannot be negative");
  }
  auto race_elapsed = elapsed;
  if (progress_.phase == RacePhase::countdown) {
    if (race_elapsed < progress_.countdown_remaining) {
      progress_.countdown_remaining -= race_elapsed;
      return;
    }
    race_elapsed -= progress_.countdown_remaining;
    progress_.countdown_remaining = SimulationDuration::zero();
    progress_.phase = RacePhase::racing;
    progress_.current_lap = 1U;
  }
  if (progress_.phase != RacePhase::racing) {
    return;
  }
  if (race_elapsed > SimulationDuration::max() - progress_.timing.total ||
      race_elapsed > SimulationDuration::max() - progress_.timing.current_lap) {
    throw std::overflow_error("race timer overflow");
  }
  progress_.timing.total += race_elapsed;
  progress_.timing.current_lap += race_elapsed;
}

bool RaceSession::cross_checkpoint(const std::size_t checkpoint_index) {
  if (progress_.phase != RacePhase::racing ||
      checkpoint_index != progress_.next_checkpoint ||
      checkpoint_index >= config_.checkpoint_count) {
    return false;
  }
  ++progress_.next_checkpoint;
  return true;
}

bool RaceSession::cross_finish_line() {
  if (progress_.phase != RacePhase::racing ||
      progress_.next_checkpoint != config_.checkpoint_count) {
    return false;
  }
  progress_.timing.completed_laps.push_back(progress_.timing.current_lap);
  progress_.timing.current_lap = SimulationDuration::zero();
  progress_.next_checkpoint = 0U;
  if (progress_.current_lap == config_.lap_count) {
    progress_.phase = RacePhase::finished;
  } else {
    ++progress_.current_lap;
  }
  return true;
}

void RaceSession::restart() noexcept { progress_ = {}; }

const RaceConfig &RaceSession::config() const noexcept { return config_; }

const RaceProgress &RaceSession::progress() const noexcept { return progress_; }

} // namespace mh::game
