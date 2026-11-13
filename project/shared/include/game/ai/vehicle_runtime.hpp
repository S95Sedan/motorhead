#pragma once

#include <game/ai/control_import.hpp>
#include <game/ai/route_import.hpp>
#include <game/physics/body_pose.hpp>
#include <game/vehicle/tuning.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mh::content {
struct AiDriverProfile;
struct AiRouteData;
struct CarDefinition;
} // namespace mh::content

namespace mh::game {

struct OriginalAiVehicleTuning {
  float route_spacing_control = 0.0F;
  float runtime_speed_scale = 0.0F;
  float runtime_group_scale = 0.0F;
  float stochastic_base_a = 0.0F;
  float stochastic_base_b = 0.0F;
  float stochastic_base_c = 0.0F;
  float half_width = 0.0F;
  float half_length = 0.0F;
};

// Exact p3.1 ADP conversion at RVA 0x0006a369..0x0006a43b and live-AI
// handoff at RVA 0x0008df1a..0x0008df59. The authored percentages become
// stochastic bases A/B after a double-precision 0.01 scale and [0, 1] clamp.
[[nodiscard]] OriginalAiVehicleTuning
original_ai_apply_driver_profile(OriginalAiVehicleTuning tuning,
                                 float authored_aggressiveness,
                                 float authored_eagerness);

// Per-slot lane preference normalized to the usable route width.
[[nodiscard]] float
ai_competitor_lateral_preference(std::size_t live_slot);

// Reports excursions beyond the authored route corridor.
[[nodiscard]] bool ai_competitor_outside_route(
    float measured_lateral_offset, float route_lateral_extent_a,
    float route_lateral_extent_b, float half_width);

// Hash-pinned p3.1 initializer result from the canonical one-player Quick
// Race/Goldbridge capture. Slot 0 is the player; slots 1..7 are opponents.
// Car identity is intentionally not part of this record because the captured
// initializer does not prove the roster mapping.
struct OriginalAiInitializerSlot {
  std::uint32_t slot_index = 0U;
  float live_car_field_0x4f3 = 0.0F;
  float difficulty_value = 0.0F;
  std::int32_t braking_sample_delta = 0;
  float captured_initial_lateral_target = 0.0F;
  OriginalAiVehicleTuning tuning{};
};

[[nodiscard]] const std::array<OriginalAiInitializerSlot, 8U> &
original_goldbridge_quick_race_ai_initializer_slots() noexcept;

struct OriginalQuickRaceRosterSlot {
  std::uint32_t slot_index = 0U;
  std::uint32_t retail_car_number = 0U;
  std::string_view driver_nick;
  std::string_view driver_name;
  std::string_view car_name;
  std::string_view team_name;
  std::string_view portrait_name;
  std::array<std::uint8_t, 3U> driver_color{};
};

[[nodiscard]] const std::array<OriginalQuickRaceRosterSlot, 8U> &
original_goldbridge_quick_race_roster() noexcept;

// p3.1 selects one shared 44-byte CPU performance profile before it iterates
// the vehicle array. Quick Race indexes records 0..2 by Easy/Medium/Hard;
// League indexes records 3..6 by its four-state division selector.
enum class OriginalCpuRaceProfileFamily : std::uint8_t {
  quick_race,
  league,
};

struct OriginalCpuRaceProfile {
  std::int32_t target_speed_level = 0;
  float speed_level_blend = 0.0F;
  std::int32_t target_acceleration_level = 0;
  float acceleration_level_blend = 0.0F;
  std::int32_t target_grip_level = 0;
  float grip_level_blend = 0.0F;
  float target_brake_force = 0.0F;
  float brake_force_blend = 0.0F;
  float global_speed_scale = 0.0F;
  float route_group_scale = 0.0F;
  float catch_up_percent = 0.0F;
};

// Builds the local vehicle's route-controller state for the retained live
// interval after it finishes. The player slot contributes zero stochastic
// bases; every nonzero field comes from the active CPU profile, selected CAR,
// or selected vehicle collision shape rather than the canonical Goldbridge
// capture.
[[nodiscard]] OriginalAiVehicleTuning
make_playable_post_finish_player_tuning(
    float vehicle_weight, const OriginalCpuRaceProfile &profile,
    float half_width, float half_length);

[[nodiscard]] const std::array<OriginalCpuRaceProfile, 7U> &
original_cpu_race_profiles() noexcept;

// Single Race replaces the three difficulty records before selecting one.
// These exact records are distinct from Quick Race Easy/Medium/Hard.
[[nodiscard]] const std::array<OriginalCpuRaceProfile, 3U> &
original_single_race_cpu_profiles() noexcept;

[[nodiscard]] std::size_t
original_cpu_race_profile_index(OriginalCpuRaceProfileFamily family,
                                std::size_t selector);

// Exact immutable drivetrain setup read from the complete live-car records in
// the paired p3.1 Goldbridge Quick Race capture. CPU cars pass through the
// original alternate initializer after their CAR's primary setup, so their
// gear, acceleration, brake, turn and grip values are not the raw CAR values.
[[nodiscard]] RecoveredVehicleRuntimeTuning
original_goldbridge_quick_race_runtime_drive_tuning(std::size_t live_slot);

// Applies p3.1's alternate initializer at RVA 0x0001c3fc using the selected
// shared race profile and the vehicle's authored CAR values. This is the
// immutable setup before the separate live catch-up owner at RVA 0x0001cd78.
[[nodiscard]] RecoveredVehicleRuntimeTuning
original_cpu_race_profile_drive_tuning(const mh::content::CarDefinition &car,
                                       std::size_t profile_index);

[[nodiscard]] RecoveredVehicleRuntimeTuning
original_cpu_race_profile_drive_tuning(const mh::content::CarDefinition &car,
                                       const OriginalCpuRaceProfile &profile);

// p3.1 RVAs 0x1cc58/0x1cd78 derive progress from 36-byte route-sample indices
// and spread the field across -1..+1, fading below 50 samples. The race owner
// supplies prior-frame progress. After subtracting the player and clamping to
// -2..+2, catch_up_percent * 0.01 scales CPU gears and acceleration by slot.
[[nodiscard]] std::vector<float>
original_cpu_catch_up_factors(std::span<const float> progress_by_slot,
                              std::size_t player_slot, float catch_up_percent);

// Builds the eight-entry local-race StartGrid permutation used by p3.1. The
// frontend first assigns rand()%8 independently to every roster record, then
// visits each record in order and rerolls that record until its value differs
// from all seven peers. The completed values are copied unchanged into each
// record's live+0xc9 field.
[[nodiscard]] std::array<std::size_t, 8U>
original_local_race_grid_permutation(OriginalAiRandomState &random) noexcept;

// Assigns the off-division League finishing positions used by p3.1. Records
// are visited in authored order. Each draws rand()%count + 1 and rerolls until
// its one-based position differs from every preceding record.
[[nodiscard]] std::vector<std::size_t>
original_league_position_permutation(std::size_t count,
                                     OriginalAiRandomState &random);

// Selects the normal (non-reverse) League race schedule from authored TRK
// Division values. Division 3 races group 1, division 2 groups 1..2,
// division 1 groups 1..3, and division 0 group 4.
[[nodiscard]] std::vector<std::size_t> original_league_track_schedule(
    std::span<const std::uint32_t> track_divisions,
    std::uint32_t human_division);

// Applies the mode-0/2 player handoff after local driver and grid generation.
// Retail copies intermediate record zero over the record that owns grid slot
// seven, then replaces live record zero with player-owned fields and forces its
// grid to seven. Driver indices stand in for the copied authored record here.
void original_single_race_player_overlay(
    std::array<std::size_t, 8U> &driver_indices,
    std::array<std::size_t, 8U> &grid_slots);

// Selects the eight intermediate local-race driver records that precede grid
// generation. Each candidate is rand()%catalog_count; duplicate PlayerNick
// values are rejected without advancing the output slot.
[[nodiscard]] std::array<std::size_t, 8U> original_local_race_driver_indices(
    std::span<const mh::content::AiDriverProfile> catalog,
    OriginalAiRandomState &random);

// Maps the selected CAR's authored Division/100 group to the ADP CarDivision
// field copied by the local-race roster builder.
[[nodiscard]] std::size_t
original_local_race_driver_car_index(std::uint32_t selected_car_division);

// Maps a live-racer slot in the captured p3.1 Quick Race roster to its authored
// two-column StartGrid position. The race owner stores this permutation at
// live+0xc9 before the track-independent grid consumer runs; the generic
// StartGrid offset formula remains unchanged.
[[nodiscard]] std::size_t
original_goldbridge_quick_race_grid_slot(std::size_t live_slot);

// Maps the live-racer slots from the complete p3.1 manual Single Race capture
// to authored StartGrid positions. The mode-0/2 setup branch independently
// proves the player selection by searching for live+0xc9 == 7. These values
// remain as a paired-capture oracle; normal races use the recovered producer.
[[nodiscard]] std::size_t
original_captured_single_race_grid_slot(std::size_t live_slot);

// AI-visible post-staging body X/Z and normalized horizontal forward vector
// captured at the first p3.1 controller update. Only type-2 slots have an
// output-side body snapshot; the non-AI player remains a separate owner.
struct OriginalQuickRaceOpeningAiPose {
  std::uint32_t slot_index = 0U;
  float vehicle_x = 0.0F;
  float vehicle_z = 0.0F;
  float vehicle_forward_x = 0.0F;
  float vehicle_forward_z = 1.0F;
};

[[nodiscard]] const std::array<OriginalQuickRaceOpeningAiPose, 7U> &
original_goldbridge_quick_race_opening_ai_poses() noexcept;

// Retained suspension/contact state captured at the same first p3.1 control
// update. These are the four state fractions at drivetrain offsets
// +0x158..+0x164; all four retained contact flags are one.
struct OriginalQuickRaceOpeningWheelState {
  std::uint32_t slot_index = 0U;
  std::array<float, 4U> previous_wheel_states{};
  std::array<float, 3U> local_linear_velocity{};
  std::array<float, 3U> local_angular_velocity{};
};

[[nodiscard]] const std::array<OriginalQuickRaceOpeningWheelState, 8U> &
original_goldbridge_quick_race_opening_wheel_states() noexcept;

// Full outer body matrices captured from a separate paired 40 ms p3.1
// opening trace. This diagnostic oracle preserves ride height, pitch, roll,
// residual velocity, and suspension history from the same controller run.
struct OriginalQuickRaceOpeningPhysicsState {
  std::uint32_t slot_index = 0U;
  std::array<std::array<float, 3U>, 3U> body_basis{};
  std::array<float, 3U> body_position{};
  std::array<float, 3U> local_linear_velocity{};
  std::array<float, 3U> local_angular_velocity{};
  std::array<float, 3U> previous_local_linear_velocity{};
  std::array<float, 3U> previous_local_angular_velocity{};
  std::array<float, 4U> previous_wheel_states{};
};

[[nodiscard]] const std::array<OriginalQuickRaceOpeningPhysicsState, 8U> &
original_goldbridge_quick_race_opening_physics_states() noexcept;

struct OriginalAiVehicleObservation {
  std::int32_t route_sample = 0;
  OriginalAiRouteFrame route_frame{};
  float route_longitudinal_projection = 0.0F;
};

struct OriginalAiTrafficVehicle {
  std::size_t slot_index = 0U;
  OriginalAiVehicleObservation observation{};
  float current_speed = 0.0F;
  float half_width = 0.0F;
  float half_length = 0.0F;
  float target_lateral_offset = 0.0F;
};

struct OriginalAiTrafficSafetyInput {
  std::int32_t route_sample_count = 0;
  OriginalAiTrafficVehicle self{};
  float route_lateral_extent_a = 0.0F;
  float route_lateral_extent_b = 0.0F;
  bool positive_side_occupied = false;
  bool negative_side_occupied = false;
};

struct OriginalAiTrafficSafety {
  float signed_avoidance_offset = 0.0F;
  bool force_full_brake = false;
  std::optional<std::size_t> blocking_slot;
};

// Isolated race-level pre-contact diagnostic retained for deterministic tests.
// The live host does not inject this result into the recovered controller or
// dynamic-contact path.
[[nodiscard]] OriginalAiTrafficSafety original_ai_predict_traffic_safety(
    const OriginalAiTrafficSafetyInput &input,
    std::span<const OriginalAiTrafficVehicle> vehicles);

// Updates a route cursor from a live body pose and returns the exact route
// frame consumed by the p3.1 avoidance owner. Hosts use this once per tick for
// all eight slots before any controller is stepped, so every opponent observes
// one coherent race-state snapshot.
[[nodiscard]] OriginalAiVehicleObservation
original_ai_observe_vehicle(const mh::content::AiRouteData &route,
                            OriginalAiRouteCursor &cursor,
                            const OriginalBodyPoseState &vehicle_pose);

// Complete route controller for one p3.1 AI slot. Controllers share one RNG
// and are stepped in stable descending route/longitudinal order.
class OriginalAiVehicleController {
public:
  OriginalAiVehicleController(const mh::content::AiRouteData &route,
                              OriginalAiVehicleTuning tuning,
                              const std::array<float, 2U> &initial_vehicle_xz,
                              OriginalAiRandomState &shared_random_state);

  [[nodiscard]] OriginalAiControllerStep
  step(const OriginalBodyPoseState &vehicle_pose, float current_speed,
       float time_step_seconds,
       std::span<const OriginalAiAvoidanceOpponent> opponents = {},
       bool positive_side_occupied = false, bool negative_side_occupied = false,
       float external_signed_avoidance_offset = 0.0F,
       bool external_force_full_brake = false);
  [[nodiscard]] OriginalAiVehicleObservation
  observe(const OriginalBodyPoseState &vehicle_pose);
  // Quick Race seeds the retained target from the grid's lateral magnitude.
  void initialize_lateral_target(const OriginalBodyPoseState &vehicle_pose);
  void initialize_lateral_target(float retained_lateral_magnitude);
  void set_lateral_preference(float normalized_preference);
  void reset(const std::array<float, 2U> &vehicle_xz);

  [[nodiscard]] std::size_t current_sample() const noexcept;
  [[nodiscard]] std::size_t lookahead_sample() const noexcept;
  [[nodiscard]] std::int32_t braking_sample_delta() const noexcept;
  [[nodiscard]] const OriginalAiVehicleTuning &tuning() const noexcept;
  [[nodiscard]] const OriginalAiControllerState &
  controller_state() const noexcept;
  [[nodiscard]] const OriginalAiStochasticControlState &
  stochastic_state() const noexcept;

private:
  const mh::content::AiRouteData *route_ = nullptr;
  OriginalAiVehicleTuning tuning_{};
  OriginalAiRouteCursor cursor_;
  OriginalAiControllerState controller_state_{};
  OriginalAiStochasticControlState stochastic_state_{};
  std::vector<OriginalAiLookaheadCandidate> lookahead_candidates_;
  OriginalAiRandomState *shared_random_state_ = nullptr;
  float lateral_preference_ = 0.0F;
  // A collision can rotate a live car past the recovered controller's
  // centered-road steering dead zone. Retain one turn direction until the
  // body points along the route again; otherwise a perfectly centered
  // 180-degree spin keeps receiving straight-ahead throttle and drives the
  // course backwards indefinitely.
  bool turnaround_recovery_active_ = false;
  float turnaround_recovery_steering_ = 0.0F;
  std::int32_t braking_sample_delta_ = 0;
};

} // namespace mh::game
