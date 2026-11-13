#include <game/race/replay_analysis.hpp>

#include <cmath>
#include <stdexcept>

namespace mh::game {
namespace {

bool same_controls(const ControlInput &left, const ControlInput &right) {
  return left.throttle == right.throttle && left.brake == right.brake &&
         left.steering == right.steering && left.handbrake == right.handbrake;
}

void validate_frame(const ReplayControlFrame &frame, const std::size_t index) {
  const auto &controls = frame.controls;
  if (frame.frame_index != index || !std::isfinite(frame.elapsed_seconds) ||
      frame.elapsed_seconds < 0.0 || !std::isfinite(controls.throttle) ||
      !std::isfinite(controls.brake) || !std::isfinite(controls.steering) ||
      controls.throttle < -1.0 || controls.throttle > 1.0 ||
      controls.brake < 0.0 || controls.brake > 1.0 ||
      controls.steering < -1.0 || controls.steering > 1.0) {
    throw std::invalid_argument("replay analysis requires canonical frames");
  }
}

} // namespace

std::vector<ReplayControlSegment>
segment_replay_controls(const std::span<const ReplayControlFrame> frames) {
  std::vector<ReplayControlSegment> result;
  result.reserve(frames.size());
  for (std::size_t index = 0U; index < frames.size(); ++index) {
    const auto &frame = frames[index];
    validate_frame(frame, index);
    if (result.empty() ||
        !same_controls(result.back().controls, frame.controls)) {
      result.push_back({frame.frame_index, 0U, 0.0, frame.controls});
    }
    auto &segment = result.back();
    ++segment.frame_count;
    segment.elapsed_seconds += frame.elapsed_seconds;
  }
  return result;
}

bool is_active_control_segment(const ReplayControlSegment &segment) noexcept {
  return segment.controls.throttle != 0.0 || segment.controls.brake != 0.0 ||
         segment.controls.steering != 0.0 || segment.controls.handbrake;
}

} // namespace mh::game
