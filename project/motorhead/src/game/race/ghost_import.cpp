#include <game/race/ghost_import.hpp>

#include <content/formats/mde_replay.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mh::game {

OriginalMdePresentationContract
original_mde_presentation_contract(
    const OriginalMdePlaybackMode mode) noexcept {
  if (mode == OriginalMdePlaybackMode::ghost_race) {
    return {1U, 0U, static_cast<std::uint8_t>(OriginalMdeRenderFlag::ghost),
            "Ghost play started!", "End of ghost play!"};
  }
  return {0U, 0U, static_cast<std::uint8_t>(OriginalMdeRenderFlag::demo),
          "Demo play started!", "End of demo play!"};
}

void OriginalMdeNoticeQueue::clear() noexcept { notices_.fill(std::nullopt); }

void OriginalMdeNoticeQueue::enqueue(
    const std::string_view text, const std::uint64_t milliseconds) noexcept {
  for (auto index = notices_.size() - 1U; index > 0U; --index) {
    notices_[index] = notices_[index - 1U];
  }
  notices_[0U] = Notice{text, milliseconds};
}

std::vector<OriginalMdeVisibleNotice>
OriginalMdeNoticeQueue::visible(const std::uint64_t milliseconds) const {
  std::vector<OriginalMdeVisibleNotice> result;
  result.reserve(notices_.size());
  for (auto index = notices_.size(); index > 0U; --index) {
    const auto record_index = index - 1U;
    const auto &notice = notices_[record_index];
    if (!notice.has_value() ||
        milliseconds - std::min(milliseconds, notice->milliseconds) >=
            original_mde_notice_lifetime_milliseconds) {
      continue;
    }
    result.push_back(
        {notice->text, notices_.size() - 1U - record_index});
  }
  return result;
}

std::optional<std::size_t> next_original_mde_camera_body_slot(
    const std::span<const OriginalMdeBodyPlaybackSample> samples,
    const std::size_t current_body_slot) noexcept {
  if (samples.empty()) {
    return std::nullopt;
  }
  const auto current = std::find_if(
      samples.begin(), samples.end(), [current_body_slot](const auto &sample) {
        return sample.body_slot == current_body_slot;
      });
  if (current == samples.end() || std::next(current) == samples.end()) {
    return samples.front().body_slot;
  }
  return std::next(current)->body_slot;
}

OriginalMdeStartAlignment make_original_mde_start_alignment(
    const mh::content::MdeV3Data &demo,
    const std::uint32_t countdown_duration_milliseconds) {
  if (demo.records.empty() || demo.stream_count == 0U) {
    throw std::invalid_argument("MDE start alignment requires recorded streams");
  }
  auto go_milliseconds =
      std::optional<std::uint32_t>{};
  for (std::size_t stream = 0U; stream < demo.stream_count; ++stream) {
    const auto candidate =
        mh::content::first_mde_stream_displacement_milliseconds(demo, stream);
    if (candidate.has_value() &&
        (!go_milliseconds.has_value() || *candidate < *go_milliseconds)) {
      go_milliseconds = candidate;
    }
  }
  const auto go =
      go_milliseconds.value_or(demo.records.front().milliseconds);
  const auto countdown_start =
      std::max(demo.records.front().milliseconds,
               go > countdown_duration_milliseconds
                   ? go - countdown_duration_milliseconds
                   : 0U);
  const auto recorded_countdown = go - countdown_start;
  const auto host_hold = countdown_duration_milliseconds > recorded_countdown
                             ? countdown_duration_milliseconds -
                                   recorded_countdown
                             : 0U;
  return {countdown_start, go, host_hold};
}

OriginalMdePlayback::OriginalMdePlayback(const mh::content::MdeV3Data &demo,
                                         const OriginalMdePlaybackMode mode)
    : demo_(&demo), mode_(mode) {
  if (demo.records.empty() || demo.stream_count == 0U) {
    throw std::invalid_argument("MDE playback requires recorded streams");
  }
}

void OriginalMdePlayback::reset(const std::uint32_t milliseconds) noexcept {
  milliseconds_ = milliseconds;
  active_ = milliseconds <= demo_->records.back().milliseconds;
}

bool OriginalMdePlayback::active() const noexcept { return active_; }

std::uint32_t OriginalMdePlayback::milliseconds() const noexcept {
  return milliseconds_;
}

std::vector<OriginalMdeBodyPlaybackSample>
OriginalMdePlayback::current_samples() const {
  if (!active_) {
    return {};
  }
  std::vector<OriginalMdeBodyPlaybackSample> result;
  result.reserve(demo_->stream_count);
  const auto first_body_slot =
      original_mde_presentation_contract(mode_).first_body_slot;
  for (std::size_t stream = 0U; stream < demo_->stream_count; ++stream) {
    const auto sample =
        mh::content::sample_mde_v3_stream(*demo_, stream, milliseconds_);
    result.push_back({stream, first_body_slot + stream, sample.state,
                      make_original_mde_ghost_pose(sample)});
  }
  return result;
}

std::vector<OriginalMdeBodyPlaybackSample>
OriginalMdePlayback::advance(const std::uint32_t elapsed_wall_milliseconds) {
  if (!active_) {
    return {};
  }
  const auto increment =
      mode_ == OriginalMdePlaybackMode::benchmark
          ? 30U
          : elapsed_wall_milliseconds;
  if (increment > std::numeric_limits<std::uint32_t>::max() - milliseconds_) {
    active_ = false;
    return {};
  }
  milliseconds_ += increment;
  if (milliseconds_ > demo_->records.back().milliseconds) {
    active_ = false;
    return {};
  }
  return current_samples();
}

OriginalMdeGhostPose make_original_mde_ghost_pose(
    const mh::content::MdeV3PlaybackSample &sample) {
  const auto x = static_cast<double>(
      sample.state.orientation_parameters[0U]);
  const auto y = static_cast<double>(
      sample.state.orientation_parameters[1U]);
  const auto z = static_cast<double>(
      sample.state.orientation_parameters[2U]);
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    throw std::invalid_argument("MDE ghost orientation is not finite");
  }

  const auto sine_x = std::sin(x);
  const auto cosine_x = std::cos(x);
  const auto sine_y = std::sin(y);
  const auto cosine_y = std::cos(y);
  const auto sine_z = std::sin(z);
  const auto cosine_z = std::cos(z);

  OriginalMdeGhostPose result;
  result.pose.body_basis = {{
      {sine_z * sine_x * sine_y + cosine_z * cosine_y,
       sine_z * cosine_x,
       sine_z * sine_x * cosine_y - cosine_z * sine_y},
      {cosine_z * sine_x * sine_y - sine_z * cosine_y,
       cosine_z * cosine_x,
       cosine_z * sine_x * cosine_y + sine_z * sine_y},
      {cosine_x * sine_y, -sine_x, cosine_x * cosine_y},
  }};
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    const auto position =
        static_cast<double>(sample.state.world_position[axis]);
    const auto orientation_velocity =
        static_cast<double>(sample.orientation_velocity[axis]);
    const auto world_velocity =
        static_cast<double>(sample.world_velocity[axis]);
    if (!std::isfinite(position) || !std::isfinite(orientation_velocity) ||
        !std::isfinite(world_velocity)) {
      throw std::invalid_argument("MDE ghost state is not finite");
    }
    result.pose.world_position[axis] = position;
    result.orientation_velocity[axis] = orientation_velocity;
    result.world_velocity[axis] = world_velocity;
  }
  return result;
}

} // namespace mh::game
