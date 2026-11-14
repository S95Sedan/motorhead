#pragma once

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <network/protocol.hpp>

namespace mh::network {

// Unsigned serial comparison also handles sequence wrap without signed casts.
inline bool newer_sequence(std::uint32_t a, std::uint32_t b) noexcept {
  return a != b && (a - b) < 0x80000000U;
}

class RacePhysicsClock {
public:
  std::size_t consume(double elapsed, bool frozen) {
    remainder_ = frozen ? 0.0 : std::clamp(remainder_ + elapsed, 0.0, 0.2);
    const auto count = static_cast<std::size_t>((remainder_ + 1.0e-9) / 0.02);
    remainder_ = std::max(0.0, remainder_ - static_cast<double>(count) * 0.02);
    return count;
  }
  double remainder() const { return remainder_; }

 private:
  double remainder_ = 0.0;
};

class HostClock {
public:
  void observe(std::uint64_t sent, std::uint64_t received, std::uint64_t host) {
    if (received < sent || received - sent > 2000U)
      return;
    const auto rtt = received - sent;
    // Queued/bursty replies must not drag the clock backwards. Prefer the
    // minimum RTT sample; refresh after 10 seconds to track oscillator drift.
    if (!ready_ || rtt <= best_rtt_ || received - updated_ > 10000U) {
      const auto measured_offset =
          static_cast<double>(host) -
          (static_cast<double>(sent) + static_cast<double>(received)) * 0.5;
      // A newly selected latency sample must not jump the race clock or the
      // timestamps used for prediction history. Initial synchronization is
      // immediate; subsequent oscillator corrections slew by at most 1 ms.
      offset_ = ready_
                    ? offset_ + std::clamp(measured_offset - offset_, -1.0, 1.0)
                    : measured_offset;
      best_rtt_ = rtt;
      updated_ = received;
      ready_ = true;
    }
  }
  bool ready() const noexcept { return ready_; }
  std::uint64_t host_time(std::uint64_t local) const noexcept {
    return static_cast<std::uint64_t>(
        std::max(0.0, static_cast<double>(local) + offset_));
  }

private:
  bool ready_ = false;
  double offset_ = 0.0;
  std::uint64_t best_rtt_ = 0U, updated_ = 0U;
};

class RaceTimeline {
public:
  void reset() { start_ = paused_at_ = 0U; }
  void set(std::uint64_t start, std::uint64_t paused_at = 0U) {
    start_ = start;
    paused_at_ = paused_at;
  }
  void pause(bool paused, std::uint64_t now) {
    if (!start_)
      return;
    if (paused && !paused_at_)
      paused_at_ = now;
    if (!paused && paused_at_) {
      start_ += now - paused_at_;
      paused_at_ = 0U;
    }
  }
  std::optional<std::uint64_t> elapsed(std::uint64_t now) const {
    if (!start_)
      return std::nullopt;
    now = paused_at_ ? paused_at_ : now;
    if (now < start_)
      return std::nullopt;
    return now - start_;
  }
  std::uint64_t start() const { return start_; }
  std::uint64_t paused_at() const { return paused_at_; }

private:
  std::uint64_t start_ = 0U, paused_at_ = 0U;
};

inline std::array<float, 4U> blend_rotation(std::array<float, 4U> a,
                                            std::array<float, 4U> b, float t) {
  float dot = 0.0F, norm = 0.0F;
  for (std::size_t i = 0; i < 4; ++i)
    dot += a[i] * b[i];
  for (std::size_t i = 0; i < 4; ++i) {
    a[i] += ((dot < 0.0F ? -b[i] : b[i]) - a[i]) * t;
    norm += a[i] * a[i];
  }
  if (norm < 1.0e-12F)
    return {0, 0, 0, 1};
  for (auto &v : a)
    v /= std::sqrt(norm);
  return a;
}

inline NetworkVehicleState interpolate_state(const NetworkVehicleState &a,
                                             const NetworkVehicleState &b,
                                             float t) {
  auto result = b;
  for (std::size_t i = 0; i < 3; ++i) {
    result.position[i] = a.position[i] + (b.position[i] - a.position[i]) * t;
    result.velocity[i] = a.velocity[i] + (b.velocity[i] - a.velocity[i]) * t;
  }
  result.rotation = blend_rotation(a.rotation, b.rotation, t);
  result.speed = a.speed + (b.speed - a.speed) * t;
  return result;
}

inline NetworkVehicleState extrapolate_state(NetworkVehicleState value,
                                             double seconds) {
  // The retained velocity is body-local and the game's basis stores axes in
  // rows. Apply the transpose of the quaternion matrix, not world velocity.
  seconds = std::clamp(seconds, 0.0, 0.1);
  const auto q = blend_rotation(value.rotation, value.rotation, 0.0F);
  const auto x = q[0], y = q[1], z = q[2], w = q[3];
  const std::array<std::array<float, 3>, 3> basis{
      {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
       {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
       {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}}};
  for (std::size_t i = 0; i < 3; ++i)
    for (std::size_t j = 0; j < 3; ++j)
      value.position[i] +=
          static_cast<float>(seconds) * basis[j][i] * value.velocity[j];
  return value;
}

class SnapshotBuffer {
public:
  void reset() { frames_.clear(); }
  bool push(const RaceSnapshot &snapshot) {
    if (!valid_race_snapshot(snapshot))
      return false;
    if (!frames_.empty()) {
      const auto &last = frames_.back();
      if (snapshot.restart_generation != last.restart_generation) {
        if (!newer_sequence(snapshot.restart_generation,
                            last.restart_generation))
          return false;
        reset();
      } else if (!newer_sequence(snapshot.tick, last.tick) ||
                 snapshot.host_time_ms < last.host_time_ms)
        return false;
      else if (snapshot.pause_generation != last.pause_generation)
        reset();
    }
    frames_.push_back(snapshot);
    while (frames_.size() > 32U)
      frames_.pop_front();
    return true;
  }
  std::optional<NetworkVehicleState> sample(std::uint8_t peer,
                                            std::uint64_t host_time) const {
    const RaceSnapshot *before = nullptr;
    const NetworkVehicleState *a = nullptr;
    for (const auto &frame : frames_) {
      const auto found =
          std::find_if(frame.vehicles.begin(), frame.vehicles.end(),
                       [peer](const auto &v) { return v.peer == peer; });
      if (found == frame.vehicles.end())
        continue;
      if (frame.host_time_ms >= host_time) {
        if (!a || frame.host_time_ms == before->host_time_ms)
          return *found;
        return interpolate_state(
            *a, *found,
            static_cast<float>(host_time - before->host_time_ms) /
                static_cast<float>(frame.host_time_ms - before->host_time_ms));
      }
      a = &*found;
      before = &frame;
    }
    if (!a)
      return std::nullopt;
    return extrapolate_state(
        *a,
        before->paused
            ? 0.0
            : static_cast<double>(host_time - before->host_time_ms) / 1000.0);
  }

  std::optional<NetworkVehicleState> sample_for_presentation(
      std::uint8_t peer, std::uint64_t host_time,
      const std::array<double, 3U> &local_position,
      double local_radius, double remote_radius) const {
    // Local prediction renders one 20 ms physics step behind the clock.
    // A nearby pushed car must use that same time: the usual 100 ms remote
    // delay otherwise puts it behind the pushing car as their speed rises.
    const auto aligned_time = host_time > 20U ? host_time - 20U : 0U;
    const auto aligned = sample(peer, aligned_time);
    if (!aligned)
      return std::nullopt;
    double distance_squared = 0.0;
    for (std::size_t axis = 0; axis < 3U; ++axis) {
      const auto delta = local_position[axis] - aligned->position[axis];
      distance_squared += delta * delta;
    }
    const auto contact_distance =
        std::max(0.0, local_radius) + std::max(0.0, remote_radius) + 0.5;
    auto blend = std::clamp((std::sqrt(distance_squared) - contact_distance) /
                                6.0,
                            0.0, 1.0);
    blend = blend * blend * (3.0 - 2.0 * blend);
    const auto delay = 20U + static_cast<std::uint64_t>(80.0 * blend);
    return sample(peer, host_time > delay ? host_time - delay : 0U);
  }

private:
  std::deque<RaceSnapshot> frames_;
};

// Compare authority to prediction at the same instant. Applying that error to
// the CURRENT car preserves the movement already predicted since the packet.
// Rebase history too, otherwise every new packet applies the same error again.
class PredictionHistory {
public:
  void reset() { frames_.clear(); }
  void record(std::uint64_t time, const NetworkVehicleState &state) {
    if (!frames_.empty() && time <= frames_.back().time)
      return;
    frames_.push_back({time, state});
    while (frames_.size() > 128U)
      frames_.pop_front();
  }
  NetworkVehicleState reconcile(const NetworkVehicleState &authority,
                                std::uint64_t time,
                                NetworkVehicleState current) {
    if (frames_.empty() || time < frames_.front().time ||
        time > frames_.back().time + 100U)
      return current;
    auto predicted = frames_.back().state;
    for (std::size_t i = 1; i < frames_.size(); ++i) {
      if (frames_[i].time >= time) {
        predicted = interpolate_state(
            frames_[i - 1].state, frames_[i].state,
            static_cast<float>(time - frames_[i - 1].time) /
                static_cast<float>(frames_[i].time - frames_[i - 1].time));
        break;
      }
    }
    float distance2 = 0.0F;
    for (std::size_t i = 0; i < 3; ++i) {
      const auto d = authority.position[i] - predicted.position[i];
      distance2 += d * d;
    }
    const auto gain = distance2 > 64.0F ? 1.0F : 0.2F;
    std::array<float, 3> delta{}, velocity_delta{};
    for (std::size_t i = 0; i < 3; ++i) {
      delta[i] = distance2 < 0.0025F
                     ? 0.0F
                     : (authority.position[i] - predicted.position[i]) * gain;
      velocity_delta[i] =
          (authority.velocity[i] - predicted.velocity[i]) * gain;
    }
    // Apply the small historical quaternion error to the current orientation.
    const auto target =
        blend_rotation(predicted.rotation, authority.rotation, gain);
    const auto multiply = [](const auto &a, const auto &b) {
      return std::array<float, 4>{
          a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
          a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
          a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    };
    auto inverse = blend_rotation(predicted.rotation, predicted.rotation, 0.0F);
    for (std::size_t i = 0; i < 3; ++i)
      inverse[i] = -inverse[i];
    const auto rotation_delta = multiply(target, inverse);
    const auto apply = [&](NetworkVehicleState &v) {
      for (std::size_t i = 0; i < 3; ++i) {
        v.position[i] += delta[i];
        v.velocity[i] += velocity_delta[i];
      }
      v.rotation = multiply(rotation_delta, v.rotation);
      v.rotation = blend_rotation(v.rotation, v.rotation, 0.0F);
    };
    apply(current);
    for (auto &frame : frames_)
      apply(frame.state);
    return current;
  }

private:
  struct Frame {
    std::uint64_t time;
    NetworkVehicleState state;
  };
  std::deque<Frame> frames_;
};

} // namespace mh::network
