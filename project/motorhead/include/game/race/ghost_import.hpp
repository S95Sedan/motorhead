#pragma once

#include <game/physics/body_pose.hpp>
#include <content/formats/mde_replay.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mh::game {

struct OriginalMdeGhostPose {
  OriginalBodyPoseState pose{};
  std::array<double, 3U> orientation_velocity{};
  std::array<double, 3U> world_velocity{};
};

// p3.1 playback owner RVA 0x0002fd74 has two body-slot mappings:
// - ordinary ghost race (mode 3): recorded stream i -> live body i + 1;
// - replay/benchmark (mode 6): recorded stream i -> live body i.
// Benchmark playback advances the owned clock by exactly 30 ms per presented
// frame; the other two modes use elapsed wall-clock milliseconds.
enum class OriginalMdePlaybackMode : std::uint8_t {
  ghost_race,
  replay,
  benchmark,
};

enum class OriginalMdeRenderFlag : std::uint8_t {
  ghost = 0x01U,
  demo = 0x04U,
};

struct OriginalMdePresentationContract {
  std::size_t first_body_slot = 0U;
  std::size_t camera_body_slot = 0U;
  std::uint8_t active_render_flags = 0U;
  std::string_view start_notice;
  std::string_view end_notice;
};

inline constexpr std::array<std::uint8_t, 3U>
    original_mde_notice_rgb{224U, 104U, 32U};
inline constexpr std::uint32_t original_mde_notice_lifetime_milliseconds =
    5000U;
inline constexpr std::size_t original_mde_visible_notice_slots = 5U;
inline constexpr std::uint32_t original_mde_notice_line_metric = 8U;
inline constexpr std::uint32_t original_mde_notice_stack_top_y = 424U;

// Recovered from the p3.1 playback owner at RVA 0x0002fd74 and renderer at
// 0x00079390. Mode 3 maps stream zero to body one and sets render bit 0;
// mode 6 maps it to body zero and sets render bit 2. Frame owner RVA 0x767c8
// contains no mode-6 camera branch: the normal selected-vehicle camera remains
// attached to live body zero. The message owner selects the corresponding
// Ghost/Demo text and queues it in orange for five seconds.
[[nodiscard]] OriginalMdePresentationContract
original_mde_presentation_contract(OriginalMdePlaybackMode mode) noexcept;

struct OriginalMdeVisibleNotice {
  std::string_view text;
  std::size_t vertical_slot = 0U;
};

// The retail queue retains 100 records, but its race renderer walks only the
// newest five in reverse record order. This bounded owner preserves that exact
// visible behavior without carrying 95 presentation-inaccessible records.
class OriginalMdeNoticeQueue {
public:
  void clear() noexcept;
  void enqueue(std::string_view text, std::uint64_t milliseconds) noexcept;
  [[nodiscard]] std::vector<OriginalMdeVisibleNotice>
  visible(std::uint64_t milliseconds) const;

private:
  struct Notice {
    std::string_view text;
    std::uint64_t milliseconds = 0U;
  };
  std::array<std::optional<Notice>, original_mde_visible_notice_slots> notices_{};
};

struct OriginalMdeBodyPlaybackSample {
  std::size_t stream_index = 0U;
  std::size_t body_slot = 0U;
  // Retained because the original body application writes these drivetrain,
  // wheel-history, mode, and flag fields in addition to the visible pose.
  mh::content::MdeV3DecodedStreamState decoded_state{};
  OriginalMdeGhostPose ghost{};
};

// Replay action CyclePlayers advances to the next active recorded body and
// wraps at the end of the body array. The p3.1 frame owner suppresses this
// action in Benchmark mode.
[[nodiscard]] std::optional<std::size_t>
next_original_mde_camera_body_slot(
    std::span<const OriginalMdeBodyPlaybackSample> samples,
    std::size_t current_body_slot) noexcept;

struct OriginalMdeStartAlignment {
  std::uint32_t countdown_start_milliseconds = 0U;
  std::uint32_t go_milliseconds = 0U;
  // MDE recording begins with the six-second numbered signal sequence, while
  // the race owner begins six seconds earlier with the authored flyby hold.
  // Keep the first recorded grid sample frozen for this difference so the
  // recording clock and host GO boundary meet without a reset/jump.
  std::uint32_t host_hold_milliseconds = 0U;
};

// Retail recordings retain their stationary grid pre-roll. Aligns the first
// visible recorded displacement with the host GO boundary while preserving the
// preceding authored countdown interval.
[[nodiscard]] OriginalMdeStartAlignment make_original_mde_start_alignment(
    const mh::content::MdeV3Data &demo,
    std::uint32_t countdown_duration_milliseconds);

class OriginalMdePlayback {
public:
  OriginalMdePlayback(const mh::content::MdeV3Data &demo,
                      OriginalMdePlaybackMode mode);

  void reset(std::uint32_t milliseconds = 0U) noexcept;
  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] std::uint32_t milliseconds() const noexcept;
  [[nodiscard]] std::vector<OriginalMdeBodyPlaybackSample>
  current_samples() const;

  // Mirrors the original owner's time update at 0x0002fe1c..0x0002fe4b.
  // The benchmark branch ignores elapsed_wall_milliseconds and adds 30.
  [[nodiscard]] std::vector<OriginalMdeBodyPlaybackSample>
  advance(std::uint32_t elapsed_wall_milliseconds);

private:
  const mh::content::MdeV3Data *demo_ = nullptr;
  OriginalMdePlaybackMode mode_ = OriginalMdePlaybackMode::ghost_race;
  std::uint32_t milliseconds_ = 0U;
  bool active_ = true;
};

// Converts the exact stock MDE playback intermediate to the pose basis built by
// p3.1 RVA 0x00043d1c. This bridge is presentation-only until the remaining
// body-state destinations are connected and verified against canonical playback.
[[nodiscard]] OriginalMdeGhostPose make_original_mde_ghost_pose(
    const mh::content::MdeV3PlaybackSample &sample);

} // namespace mh::game
