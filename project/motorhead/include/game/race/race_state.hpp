#pragma once

#include <game/physics/simulation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mh::game {

enum class RacePhase { ready, countdown, racing, finished };

struct RaceConfig {
  std::uint32_t lap_count = 3U;
  std::size_t checkpoint_count = 0U;
  SimulationDuration countdown_duration = std::chrono::seconds(3);
  SimulationDuration countdown_lead_in_duration{};
};

struct RaceTiming {
  SimulationDuration total{};
  SimulationDuration current_lap{};
  std::vector<SimulationDuration> completed_laps;
};

struct RaceProgress {
  RacePhase phase = RacePhase::ready;
  std::uint32_t current_lap = 0U;
  std::size_t next_checkpoint = 0U;
  SimulationDuration countdown_remaining{};
  RaceTiming timing{};
};

// p3.1 RVA 0x76c1a..0x76c93 starts this owner when the local vehicle first
// reports its completed-race flag. The stored threshold base is current time
// plus the binary64 constant at VA 0x0050cb57 (-25,000 ms). RVA
// 0x76d5b..0x76da1 then retains the live race while current_time is at most
// that base plus 0x88b8 (35,000 ms). The resulting post-finish interval is
// therefore 10,000 ms; the strict comparison is intentional.
inline constexpr auto original_post_finish_duration =
    std::chrono::milliseconds(0x88b8 - 25000);

// Playable presentation policy requested for the reconstruction. The recovered
// retail owner above remains documented and available as evidence, while the
// result screen now appears after five seconds of AI-controlled coast-out.
inline constexpr auto playable_post_finish_results_delay =
    std::chrono::seconds(5);

class OriginalPostFinishSchedule {
public:
  void begin() noexcept;
  void advance(SimulationDuration elapsed);
  void reset() noexcept;

  [[nodiscard]] bool active() const noexcept;
  [[nodiscard]] bool results_due() const noexcept;
  [[nodiscard]] SimulationDuration elapsed() const noexcept;

private:
  bool active_ = false;
  SimulationDuration elapsed_{};
};

struct DirectionalRaceGate2D {
  std::array<double, 2U> center{};
  std::array<double, 2U> forward{0.0, 1.0};
  double half_width = 1.0;
};

struct RaceCourse2D {
  DirectionalRaceGate2D finish{};
  std::vector<DirectionalRaceGate2D> checkpoints;
};

// Tests a complete previous-to-current X/Z movement segment against a finite
// directional gate. Only travel from the gate's back side to its front side is
// accepted; reverse travel and crossings outside the authored width are not.
[[nodiscard]] bool
crosses_directional_race_gate(const DirectionalRaceGate2D &gate,
                              const std::array<double, 2U> &previous_position,
                              const std::array<double, 2U> &current_position);

class RaceSession {
public:
  explicit RaceSession(RaceConfig config);

  void begin_countdown();
  void start();
  void advance(SimulationDuration elapsed);
  [[nodiscard]] bool cross_checkpoint(std::size_t checkpoint_index);
  [[nodiscard]] bool cross_finish_line();
  void restart() noexcept;

  [[nodiscard]] const RaceConfig &config() const noexcept;
  [[nodiscard]] const RaceProgress &progress() const noexcept;

private:
  RaceConfig config_;
  RaceProgress progress_{};
};

} // namespace mh::game
