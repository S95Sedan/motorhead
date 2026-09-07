#include <game/ai/vehicle_runtime.hpp>

#include <content/formats/ai_route.hpp>
#include <content/formats/ai_driver_profile.hpp>
#include <content/formats/car_definition.hpp>
#include <game/vehicle/import.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace mh::game {
namespace {

std::size_t forward_sample_distance(const std::size_t from,
                                    const std::size_t to,
                                    const std::size_t count) {
  return to >= from ? to - from : count - from + to;
}

const mh::content::AiRouteGroup &
route_group(const mh::content::AiRouteData &route,
            const std::uint32_t group_index) {
  const auto found =
      std::find_if(route.groups.begin(), route.groups.end(),
                   [group_index](const mh::content::AiRouteGroup &group) {
                     return group.index == group_index;
                   });
  if (found == route.groups.end()) {
    throw std::runtime_error("AI route sample has no matching group");
  }
  return *found;
}

bool nonzero_radius(const float radius) {
  return (std::bit_cast<std::uint32_t>(radius) & 0x7fffffffU) != 0U;
}

float original_profile_percentage(const float authored_value) {
  if (!std::isfinite(authored_value)) {
    throw std::invalid_argument(
        "original AI driver profile requires finite percentages");
  }
  auto normalized =
      static_cast<float>(static_cast<double>(authored_value) * 0.01);
  if (normalized > 1.0F) {
    normalized = 1.0F;
  }
  if (normalized < 0.0F) {
    normalized = 0.0F;
  }
  return normalized;
}

float circular_forward_distance(const OriginalAiTrafficVehicle &self,
                                const OriginalAiTrafficVehicle &other,
                                const std::int32_t sample_count) {
  auto sample_delta =
      other.observation.route_sample - self.observation.route_sample;
  if (sample_delta < 0) {
    sample_delta += sample_count;
  }
  auto distance = static_cast<float>(sample_delta) +
                  other.observation.route_longitudinal_projection -
                  self.observation.route_longitudinal_projection;
  if (distance <= 0.0F) {
    distance += static_cast<float>(sample_count);
  }
  return distance;
}

} // namespace

float ai_competitor_lateral_preference(const std::size_t live_slot) {
  // Alternate adjacent grid rows; slot zero is the player.
  static constexpr std::array<float, 8U> preferences{
      0.0F, -0.48F, 0.42F, -0.22F, 0.24F, -0.36F, 0.52F, 0.08F};
  if (live_slot >= preferences.size()) {
    throw std::invalid_argument("AI competitor slot is out of range");
  }
  return preferences[live_slot];
}

bool ai_competitor_outside_route(const float measured_lateral_offset,
                                 const float route_lateral_extent_a,
                                 const float route_lateral_extent_b,
                                 const float half_width) {
  const std::array values{measured_lateral_offset, route_lateral_extent_a,
                          route_lateral_extent_b, half_width};
  if (!std::all_of(values.begin(), values.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      route_lateral_extent_a < 0.0F || route_lateral_extent_b < 0.0F ||
      half_width < 0.0F) {
    throw std::invalid_argument(
        "AI route recovery requires finite nonnegative extents");
  }
  constexpr float escape_margin = 0.5F;
  const auto limit = measured_lateral_offset < 0.0F
                         ? route_lateral_extent_a
                         : route_lateral_extent_b;
  return std::fabs(measured_lateral_offset) >
         limit + half_width + escape_margin;
}

OriginalAiVehicleTuning
original_ai_apply_driver_profile(OriginalAiVehicleTuning tuning,
                                 const float authored_aggressiveness,
                                 const float authored_eagerness) {
  tuning.stochastic_base_a =
      original_profile_percentage(authored_aggressiveness);
  tuning.stochastic_base_b = original_profile_percentage(authored_eagerness);
  return tuning;
}

OriginalAiVehicleTuning make_playable_post_finish_player_tuning(
    const float vehicle_weight, const OriginalCpuRaceProfile &profile,
    const float half_width, const float half_length) {
  if (!std::isfinite(profile.global_speed_scale) ||
      !std::isfinite(profile.route_group_scale) ||
      !std::isfinite(half_width) || !std::isfinite(half_length) ||
      half_width < 0.0F || half_length < 0.0F) {
    throw std::invalid_argument(
        "post-finish player AI tuning requires finite source values");
  }
  OriginalAiVehicleTuning tuning;
  tuning.route_spacing_control =
      original_ai_route_spacing_control(vehicle_weight);
  tuning.runtime_speed_scale = profile.global_speed_scale;
  tuning.runtime_group_scale = profile.route_group_scale;
  tuning.half_width = half_width;
  tuning.half_length = half_length;
  return tuning;
}

const std::array<OriginalAiInitializerSlot, 8U> &
original_goldbridge_quick_race_ai_initializer_slots() noexcept {
  static constexpr std::array<OriginalAiInitializerSlot, 8U> slots{{
      {0U,
       1350.0F,
       5.0F,
       0,
       6.63947439F,
       {-28000.0F / 1350.0F, 2.2F, 14.0F, 0.0F, 0.0F, 0.0F, 2.1F, 4.4F}},
      {1U,
       1340.0F,
       6.0F,
       0,
       6.645177F,
       {-28000.0F / 1340.0F, 2.2F, 14.0F, 0.5F, 0.78F, 0.0F, 2.1F, 4.4F}},
      {2U,
       1420.0F,
       4.0F,
       0,
       5.358391F,
       {-28000.0F / 1420.0F, 2.2F, 14.0F, 0.5F, 0.99F, 0.0F, 2.1F, 4.4F}},
      {3U,
       1420.0F,
       4.0F,
       0,
       6.63613224F,
       {-28000.0F / 1420.0F, 2.2F, 14.0F, 0.5F, 1.0F, 0.0F, 2.1F, 4.4F}},
      {4U,
       1350.0F,
       5.0F,
       0,
       5.363442F,
       {-28000.0F / 1350.0F, 2.2F, 14.0F, 0.5F, 0.8F, 0.0F, 2.1F, 4.4F}},
      {5U,
       1420.0F,
       4.0F,
       0,
       5.36009073F,
       {-28000.0F / 1420.0F, 2.2F, 14.0F, 0.5F, 0.99F, 0.0F, 2.1F, 4.4F}},
      {6U,
       1340.0F,
       6.0F,
       0,
       6.72956467F,
       {-28000.0F / 1340.0F, 2.2F, 14.0F, 0.5F, 0.99F, 0.0F, 2.1F, 4.4F}},
      {7U,
       1420.0F,
       4.0F,
       0,
       5.304962F,
       {-28000.0F / 1420.0F, 2.2F, 14.0F, 0.5F, 0.66F, 0.0F, 2.1F, 4.4F}},
  }};
  return slots;
}

const std::array<OriginalQuickRaceRosterSlot, 8U> &
original_goldbridge_quick_race_roster() noexcept {
  static constexpr std::array<OriginalQuickRaceRosterSlot, 8U> roster{{
      {0U,
       8U,
       "Main",
       "Main",
       "Resonic",
       "Honker1",
       "player.tga",
       {255U, 255U, 255U}},
      {1U,
       2U,
       "El Toro",
       "Rodriges Vasquesce",
       "Adder Mk2",
       "Raggare",
       "rodriges.tga",
       {100U, 150U, 127U}},
      {2U,
       7U,
       "Gangsta",
       "Rafaele Maoli",
       "ASC",
       "Raggare",
       "arnold.tga",
       {200U, 70U, 110U}},
      {3U,
       7U,
       "Barbie",
       "Anna Karen",
       "ASC",
       "Raggare",
       "melissa.tga",
       {100U, 150U, 127U}},
      {4U,
       8U,
       "Zeb",
       "Zebastian Storm",
       "Resonic",
       "Raggare",
       "jeepen.tga",
       {255U, 0U, 127U}},
      {5U,
       7U,
       "Spreddo",
       "Turbo Liljekvist",
       "ASC",
       "Raggare",
       "fredrik.tga",
       {0U, 127U, 255U}},
      {6U,
       2U,
       "Spitfire",
       "Charles Rudis",
       "Adder Mk2",
       "Raggare",
       "rudberg.tga",
       {192U, 255U, 64U}},
      {7U,
       7U,
       "Crazy Joe",
       "Joe Alexinoff",
       "ASC",
       "Raggare",
       "olle.tga",
       {255U, 127U, 127U}},
  }};
  return roster;
}

std::size_t
original_cpu_race_profile_index(const OriginalCpuRaceProfileFamily family,
                                const std::size_t selector) {
  switch (family) {
  case OriginalCpuRaceProfileFamily::quick_race:
    if (selector >= 3U) {
      throw std::invalid_argument(
          "Quick Race CPU profile selector is out of range");
    }
    return selector;
  case OriginalCpuRaceProfileFamily::league:
    if (selector >= 4U) {
      throw std::invalid_argument(
          "League CPU profile selector is out of range");
    }
    return 3U + selector;
  }
  throw std::invalid_argument("CPU race-profile family is invalid");
}

const std::array<OriginalCpuRaceProfile, 7U> &
original_cpu_race_profiles() noexcept {
  // Exact 44-byte records captured from master-object offsets
  // 0x1408..0x153b in canonical European p3.1. Records 0..2 are
  // Easy/Medium/Hard Quick Race; records 3..6 are League states 0..3.
  static constexpr std::array<OriginalCpuRaceProfile, 7U> profiles{{
      {7, 0.0F, 10, 0.1F, 10, 0.1F, 0.2F, 0.1F, 2.3F, 14.0F, 15.0F},
      {7, 0.0F, 10, 0.3F, 10, 0.2F, 0.5F, 0.2F, 2.7F, 16.0F, 10.0F},
      {7, 0.0F, 10, 0.8F, 10, 1.0F, 0.99F, 1.0F, 3.1F, 18.0F, 5.0F},
      {7, 0.0F, 10, 0.3F, 10, 1.0F, 0.99F, 1.0F, 3.1F, 18.0F, 0.0F},
      {7, 0.0F, 10, 0.8F, 10, 1.0F, 0.99F, 1.0F, 3.1F, 18.0F, 0.0F},
      {7, 0.0F, 10, 0.3F, 10, 0.2F, 0.5F, 0.2F, 2.7F, 16.0F, 0.0F},
      {7, 0.0F, 10, 0.3F, 10, 0.2F, 0.5F, 0.2F, 2.7F, 16.0F, 0.0F},
  }};
  return profiles;
}

const std::array<OriginalCpuRaceProfile, 3U> &
original_single_race_cpu_profiles() noexcept {
  // Exact 44-byte Easy/Medium/Hard records captured from master-object offsets
  // 0x1408..0x148b after p3.1 entered Single Race. The selected Medium record
  // and all seven alternate-initializer calls are paired in capture
  // 20260730T070807Z.
  static constexpr std::array<OriginalCpuRaceProfile, 3U> profiles{{
      {7, 0.0F, 7, 0.0F, 10, 0.2F, 0.2F, 0.1F, 2.1F, 14.0F, 15.0F},
      {7, 0.0F, 8, 0.1F, 10, 0.4F, 0.5F, 0.4F, 2.4F, 15.0F, 10.0F},
      {7, 0.0F, 10, 0.9F, 10, 1.0F, 0.99F, 1.0F, 2.9F, 18.0F, 5.0F},
  }};
  return profiles;
}

RecoveredVehicleRuntimeTuning
original_goldbridge_quick_race_runtime_drive_tuning(
    const std::size_t live_slot) {
  struct CapturedDriveTuning {
    std::size_t gear_value_count = 0U;
    std::array<std::uint32_t, 7U> gear_value_bits{};
    std::uint32_t acceleration_force_bits = 0U;
    std::uint32_t brake_force_bits = 0U;
    std::uint32_t turn_force_bits = 0U;
    std::array<std::uint32_t, 4U> grip_response_bits{};
  };
  static constexpr std::array<CapturedDriveTuning, 8U> captured{{
      {5U,
       {0x42480000U, 0x42990000U, 0x42ff0000U, 0x433b0000U, 0x437f0000U,
        0x00000000U, 0x00000000U},
       0x451f6000U,
       0x3f266666U,
       0x463b8000U,
       {0x3f833333U, 0x3f833333U, 0x3f833333U, 0x3f833333U}},
      {6U,
       {0x42480000U, 0x4270002dU, 0x42c30025U, 0x43070019U, 0x432c8020U,
        0x4361002aU, 0x00000000U},
       0x451ab01dU,
       0x3f466666U,
       0x465ac000U,
       {0x3fa66666U, 0x3fa66666U, 0x3f400000U, 0x3f733333U}},
      {6U,
       {0x42480000U, 0x428a0011U, 0x42e0401cU, 0x431b4014U, 0x43466019U,
        0x43816010U, 0x00000000U},
       0x452fd65fU,
       0x3f4cccccU,
       0x465ac000U,
       {0x3fa66666U, 0x3fa66666U, 0x3f400000U, 0x3f733333U}},
      {6U,
       {0x42480000U, 0x4296000aU, 0x42f3c011U, 0x4328c00cU, 0x4357a00fU,
        0x438ca00aU, 0x00000000U},
       0x453f20a0U,
       0x3f4cccccU,
       0x465ac000U,
       {0x3fa66666U, 0x3fa66666U, 0x3f400000U, 0x3f733333U}},
      {5U,
       {0x42480000U, 0x42938928U, 0x42f5e498U, 0x4334524dU, 0x4375e498U,
        0x00000000U, 0x00000000U},
       0x4546e24dU,
       0x3f466666U,
       0x465ac000U,
       {0x3fa66666U, 0x3fa66666U, 0x3f400000U, 0x3f733333U}},
      {6U,
       {0x42480000U, 0x42b9fff5U, 0x43171ff7U, 0x43513ff4U, 0x4385aff8U,
        0x43ae5ff6U, 0x00000000U},
       0x456cff60U,
       0x3f4cccccU,
       0x465ac000U,
       {0x3fa66666U, 0x3fa66666U, 0x3f400000U, 0x3f733333U}},
      {6U,
       {0x42480000U, 0x42bc9238U, 0x431936ceU, 0x4354247fU, 0x43878918U,
        0x43b0c914U, 0x00000000U},
       0x4573147cU,
       0x3f466666U,
       0x465ac000U,
       {0x3fa66666U, 0x3fa66666U, 0x3f400000U, 0x3f733333U}},
      {6U,
       {0x42480000U, 0x42d1ffe8U, 0x432a9fedU, 0x436c3fe5U, 0x4396efefU,
        0x43c4dfeaU, 0x00000000U},
       0x4585c9f1U,
       0x3f4cccccU,
       0x465ac000U,
       {0x3fa66666U, 0x3fa66666U, 0x3f400000U, 0x3f733333U}},
  }};
  if (live_slot >= captured.size()) {
    throw std::invalid_argument(
        "Goldbridge Quick Race drive-tuning slot is out of range");
  }
  const auto &source = captured[live_slot];
  RecoveredVehicleRuntimeTuning result;
  result.gear_values.reserve(source.gear_value_count);
  for (std::size_t index = 0U; index < source.gear_value_count; ++index) {
    result.gear_values.push_back(
        std::bit_cast<float>(source.gear_value_bits[index]));
  }
  result.minimum_rpm = std::bit_cast<float>(0x447a0000U);
  result.maximum_rpm = std::bit_cast<float>(0x461c4000U);
  result.acceleration_force =
      std::bit_cast<float>(source.acceleration_force_bits);
  result.brake_force = std::bit_cast<float>(source.brake_force_bits);
  result.turn_force = std::bit_cast<float>(source.turn_force_bits);
  result.grip_level = live_slot == 0U ? 5 : 10;
  result.acceleration_selector = live_slot == 0U ? 0U : 2U;
  for (std::size_t index = 0U; index < result.grip_response_constants.size();
       ++index) {
    result.grip_response_constants[index] =
        std::bit_cast<float>(source.grip_response_bits[index]);
  }
  return result;
}

RecoveredVehicleRuntimeTuning
original_cpu_race_profile_drive_tuning(const mh::content::CarDefinition &car,
                                       const std::size_t profile_index) {
  if (profile_index >= original_cpu_race_profiles().size()) {
    throw std::invalid_argument(
        "CPU alternate tuning profile index is out of range");
  }
  return original_cpu_race_profile_drive_tuning(
      car, original_cpu_race_profiles()[profile_index]);
}

RecoveredVehicleRuntimeTuning
original_cpu_race_profile_drive_tuning(const mh::content::CarDefinition &car,
                                       const OriginalCpuRaceProfile &profile) {
  if (!car.physics.has_value() || !car.performance_levels.has_value()) {
    throw std::invalid_argument(
        "CPU alternate tuning requires a recovered race profile and CAR "
        "physics");
  }
  const auto &authored_levels = *car.performance_levels;
  const auto interpolate_level = [](const std::int32_t authored,
                                    const std::int32_t target,
                                    const float blend) {
    auto value = static_cast<float>(
        static_cast<double>(authored) +
        (static_cast<double>(target) - static_cast<double>(authored)) *
            static_cast<double>(blend));
    // p3.1 resets values above ten to neutral level five; it does not clamp
    // them to ten (RVA 0x0001c4dd..0x0001c51e).
    if (static_cast<double>(value) > 10.0) {
      value = 5.0F;
    }
    return value;
  };
  const auto speed_level =
      interpolate_level(authored_levels.speed, profile.target_speed_level,
                        profile.speed_level_blend);
  const auto acceleration_level = interpolate_level(
      authored_levels.acceleration, profile.target_acceleration_level,
      profile.acceleration_level_blend);
  const auto grip_level =
      interpolate_level(authored_levels.grip, profile.target_grip_level,
                        profile.grip_level_blend);
  auto result = make_recovered_vehicle_runtime_tuning(car);
  const auto &authored_gears = car.physics->gear_ratios;
  const auto gear_scale =
      (static_cast<double>(speed_level) - 5.0) * 0.05 + 1.05;
  result.gear_values.resize(authored_gears.size());
  for (std::size_t index = 0U; index < authored_gears.size(); ++index) {
    result.gear_values[index] =
        index == 0U
            ? authored_gears[index]
            : static_cast<float>(static_cast<double>(authored_gears[index]) *
                                 gear_scale);
  }
  result.acceleration_force = static_cast<float>(
      static_cast<double>(car.physics->acceleration_force) +
      (static_cast<double>(acceleration_level) - 5.0) * 250.0);
  result.brake_force =
      static_cast<float>(static_cast<double>(car.physics->brake_force) +
                         (static_cast<double>(profile.target_brake_force) -
                          static_cast<double>(car.physics->brake_force)) *
                             static_cast<double>(profile.brake_force_blend));
  result.turn_force = 14000.0F;
  result.grip_level = grip_level;
  result.acceleration_selector = 2U;
  constexpr std::array<float, 4U> lower{0.75F, 0.75F, 1.3F, 1.1F};
  constexpr std::array<float, 4U> upper{1.3F, 1.3F, 0.75F, 0.95F};
  const auto grip_fraction = static_cast<double>(grip_level) * 0.1;
  for (std::size_t index = 0U; index < result.grip_response_constants.size();
       ++index) {
    const auto difference =
        static_cast<double>(upper[index]) - static_cast<double>(lower[index]);
    result.grip_response_constants[index] = static_cast<float>(
        static_cast<double>(lower[index]) + difference * grip_fraction);
  }
  return result;
}

std::vector<float>
original_cpu_catch_up_factors(const std::span<const float> progress_by_slot,
                              const std::size_t player_slot,
                              const float catch_up_percent) {
  if (progress_by_slot.empty() || progress_by_slot.size() > 8U ||
      player_slot >= progress_by_slot.size() ||
      !std::isfinite(catch_up_percent) || catch_up_percent < 0.0F ||
      !std::all_of(
          progress_by_slot.begin(), progress_by_slot.end(),
          [](const float value) { return std::isfinite(value); })) {
    throw std::invalid_argument(
        "CPU catch-up requires one through eight finite live-slot positions");
  }
  std::vector<std::size_t> ranked_slots(progress_by_slot.size());
  for (std::size_t slot = 0U; slot < ranked_slots.size(); ++slot) {
    ranked_slots[slot] = slot;
  }
  std::stable_sort(
      ranked_slots.begin(), ranked_slots.end(),
      [progress_by_slot](const std::size_t left, const std::size_t right) {
        return progress_by_slot[left] > progress_by_slot[right];
      });

  std::vector<float> offset_by_slot(progress_by_slot.size(), 0.0F);
  const auto leader =
      static_cast<double>(progress_by_slot[ranked_slots.front()]);
  const auto last = static_cast<double>(progress_by_slot[ranked_slots.back()]);
  const auto span = static_cast<float>(leader - last);
  if (ranked_slots.size() > 1U && span > 0.0) {
    const auto endpoint =
        static_cast<float>(std::min(1.0, static_cast<double>(span) * 0.02));
    for (const auto slot : ranked_slots) {
      const auto from_last = static_cast<double>(progress_by_slot[slot]) - last;
      offset_by_slot[slot] =
          static_cast<float>(static_cast<double>(endpoint) -
                             (from_last / static_cast<double>(span)) *
                                 (2.0 * static_cast<double>(endpoint)));
    }
  }

  const auto coefficient = static_cast<float>(
      static_cast<double>(catch_up_percent) *
      static_cast<double>(std::bit_cast<float>(0x3c23d70aU)));
  const auto player_offset = offset_by_slot[player_slot];
  std::vector<float> factors(progress_by_slot.size(), 1.0F);
  for (std::size_t slot = 0U; slot < factors.size(); ++slot) {
    const auto relative = std::clamp(static_cast<double>(offset_by_slot[slot]) -
                                         static_cast<double>(player_offset),
                                     -2.0, 2.0);
    factors[slot] =
        static_cast<float>(1.0 + relative * static_cast<double>(coefficient));
  }
  return factors;
}

std::array<std::size_t, 8U>
original_local_race_grid_permutation(OriginalAiRandomState &random) noexcept {
  std::array<std::size_t, 8U> result{};
  for (auto &grid_slot : result) {
    grid_slot = original_ai_random_next(random) % result.size();
  }

  for (std::size_t live_slot = 0U; live_slot < result.size(); ++live_slot) {
    for (;;) {
      auto duplicate = false;
      for (std::size_t peer_slot = 0U; peer_slot < result.size(); ++peer_slot) {
        if (peer_slot != live_slot && result[peer_slot] == result[live_slot]) {
          result[live_slot] = original_ai_random_next(random) % result.size();
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        break;
      }
    }
  }
  return result;
}

std::vector<std::size_t>
original_league_position_permutation(const std::size_t count,
                                     OriginalAiRandomState &random) {
  if (count == 0U || count > 8U) {
    throw std::invalid_argument(
        "original League position permutation requires one through eight records");
  }
  std::vector<std::size_t> result;
  result.reserve(count);
  while (result.size() < count) {
    const auto position = static_cast<std::size_t>(
                              original_ai_random_next(random) % count) +
                          1U;
    if (std::find(result.begin(), result.end(), position) == result.end()) {
      result.push_back(position);
    }
  }
  return result;
}

std::vector<std::size_t> original_league_track_schedule(
    const std::span<const std::uint32_t> track_divisions,
    const std::uint32_t human_division) {
  if (human_division > 3U) {
    throw std::invalid_argument(
        "original League track schedule requires division zero through three");
  }
  std::vector<std::size_t> result;
  for (std::size_t index = 0U; index < track_divisions.size(); ++index) {
    const auto authored = track_divisions[index];
    const auto group = authored / 100U;
    const auto variant = authored % 100U;
    if (variant >= 10U) {
      continue;
    }
    const auto selected = human_division == 3U   ? group == 1U
                          : human_division == 2U ? group <= 2U
                          : human_division == 1U ? group <= 3U
                                                 : group == 4U;
    if (selected) {
      result.push_back(index);
    }
  }
  if (result.empty()) {
    throw std::invalid_argument(
        "original League track schedule selected no authored normal tracks");
  }
  return result;
}

void original_single_race_player_overlay(
    std::array<std::size_t, 8U> &driver_indices,
    std::array<std::size_t, 8U> &grid_slots) {
  const auto player_record =
      std::find(grid_slots.begin(), grid_slots.end(), 7U);
  if (player_record == grid_slots.end()) {
    throw std::invalid_argument(
        "local-race grid producer omitted Single Race player slot 7");
  }

  const auto player_record_index = static_cast<std::size_t>(
      std::distance(grid_slots.begin(), player_record));
  driver_indices[player_record_index] = driver_indices.front();
  grid_slots[player_record_index] = grid_slots.front();
  grid_slots.front() = 7U;
}

std::array<std::size_t, 8U> original_local_race_driver_indices(
    const std::span<const mh::content::AiDriverProfile> catalog,
    OriginalAiRandomState &random) {
  if (catalog.size() < 8U) {
    throw std::invalid_argument(
        "local-race driver selection requires at least eight profiles");
  }

  std::array<std::size_t, 8U> result{};
  for (std::size_t slot = 0U; slot < result.size(); ++slot) {
    for (;;) {
      const auto candidate = static_cast<std::size_t>(
          original_ai_random_next(random) % catalog.size());
      auto duplicate = false;
      for (std::size_t prior = 0U; prior < slot; ++prior) {
        if (catalog[result[prior]].player_nick ==
            catalog[candidate].player_nick) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        result[slot] = candidate;
        break;
      }
    }
  }
  return result;
}

std::size_t original_local_race_driver_car_index(
    const std::uint32_t selected_car_division) {
  switch (selected_car_division / 100U) {
  case 1U:
    return 3U;
  case 2U:
    return 2U;
  case 3U:
  case 4U:
    return 1U;
  default:
    throw std::invalid_argument(
        "selected CAR division has no local-race ADP mapping");
  }
}

std::size_t
original_goldbridge_quick_race_grid_slot(const std::size_t live_slot) {
  static constexpr std::array<std::size_t, 8U> grid_slots{3U, 7U, 6U, 5U,
                                                          4U, 2U, 1U, 0U};
  if (live_slot >= grid_slots.size()) {
    throw std::invalid_argument(
        "Goldbridge Quick Race live slot is out of range");
  }
  return grid_slots[live_slot];
}

std::size_t
original_captured_single_race_grid_slot(const std::size_t live_slot) {
  static constexpr std::array<std::size_t, 8U> grid_slots{7U, 0U, 6U, 5U,
                                                          1U, 3U, 4U, 2U};
  if (live_slot >= grid_slots.size()) {
    throw std::invalid_argument(
        "captured Single Race live slot is out of range");
  }
  return grid_slots[live_slot];
}

const std::array<OriginalQuickRaceOpeningAiPose, 7U> &
original_goldbridge_quick_race_opening_ai_poses() noexcept {
  static constexpr std::array<OriginalQuickRaceOpeningAiPose, 7U> poses{{
      {1U, 105.9654F, -1.19378459F, 0.000431709166F, 0.9999999F},
      {2U, 93.9665146F, 0.80033493F, 0.000736516842F, 0.9999997F},
      {3U, 105.966438F, 10.8004351F, 0.000736516842F, 0.9999997F},
      {4U, 93.96621F, 12.8335619F, 0.000504812051F, 0.9999999F},
      {5U, 93.9663239F, 24.8003883F, 0.000736516842F, 0.9999997F},
      {6U, 105.965118F, 34.8062363F, 0.000431709166F, 0.9999999F},
      {7U, 93.96623F, 36.8002663F, 0.000736516842F, 0.9999997F},
  }};
  return poses;
}

const std::array<OriginalQuickRaceOpeningWheelState, 8U> &
original_goldbridge_quick_race_opening_wheel_states() noexcept {
  static constexpr auto wheel = [](const std::uint32_t bits) {
    return std::bit_cast<float>(bits);
  };
  static constexpr std::array<OriginalQuickRaceOpeningWheelState, 8U> states{{
      {0U,
       {wheel(0x3f4a744cU), wheel(0x3f4a7536U), wheel(0x3f4a761eU),
        wheel(0x3f4a761eU)},
       {wheel(0x00000000U), wheel(0xba00c319U), wheel(0x00000000U)},
       {wheel(0x3463df08U), wheel(0xae0aa26fU), wheel(0xb55d3d01U)}},
      {1U,
       {wheel(0x3f4ab2a0U), wheel(0x3f4ab2a0U), wheel(0x3f4ab2a0U),
        wheel(0x3f4ab2a0U)},
       {wheel(0x00000000U), wheel(0xb9bedafeU), wheel(0x00000000U)},
       {wheel(0x2d4c039fU), wheel(0xa9608172U), wheel(0x2bbd1061U)}},
      {2U,
       {wheel(0x3f48ce14U), wheel(0x3f48ce14U), wheel(0x3f48ce14U),
        wheel(0x3f48ce14U)},
       {wheel(0x00000000U), wheel(0x395eca62U), wheel(0x00000000U)},
       {wheel(0x2d3bfe2cU), wheel(0x2a06fe0eU), wheel(0xaabaa7d4U)}},
      {3U,
       {wheel(0x3f48ce14U), wheel(0x3f48ce14U), wheel(0x3f48ce14U),
        wheel(0x3f48ce14U)},
       {wheel(0x00000000U), wheel(0x395eca62U), wheel(0x00000000U)},
       {wheel(0x2d3bfe2cU), wheel(0x2a06fe0eU), wheel(0xaabaa7d4U)}},
      {4U,
       {wheel(0x3f4a744cU), wheel(0x3f4a744cU), wheel(0x3f4a761eU),
        wheel(0x3f4a761eU)},
       {wheel(0x00000000U), wheel(0xba3df2a8U), wheel(0x00000000U)},
       {wheel(0xb5c85a32U), wheel(0xae775763U), wheel(0xb26a455fU)}},
      {5U,
       {wheel(0x3f48ce14U), wheel(0x3f48ce14U), wheel(0x3f48ce14U),
        wheel(0x3f48ce14U)},
       {wheel(0x00000000U), wheel(0x395eca62U), wheel(0x00000000U)},
       {wheel(0x2d3bfe2cU), wheel(0x2a06fe0eU), wheel(0xaabaa7d4U)}},
      {6U,
       {wheel(0x3f4ab2a0U), wheel(0x3f4ab2a0U), wheel(0x3f4ab2a0U),
        wheel(0x3f4ab2a0U)},
       {wheel(0x00000000U), wheel(0xb9bedafeU), wheel(0x00000000U)},
       {wheel(0x2d4c039fU), wheel(0xa9608172U), wheel(0x2bbd1061U)}},
      {7U,
       {wheel(0x3f48ce14U), wheel(0x3f48ce14U), wheel(0x3f48ce14U),
        wheel(0x3f48ce14U)},
       {wheel(0x00000000U), wheel(0x395eca62U), wheel(0x00000000U)},
       {wheel(0x2d3bfe2cU), wheel(0x2a06fe0eU), wheel(0xaabaa7d4U)}},
  }};
  return states;
}

const std::array<OriginalQuickRaceOpeningPhysicsState, 8U> &
original_goldbridge_quick_race_opening_physics_states() noexcept {
  static constexpr auto f = [](const std::uint32_t bits) {
    return std::bit_cast<float>(bits);
  };
  static constexpr std::array<OriginalQuickRaceOpeningPhysicsState, 8U> states{{
      {0U,
       {{{f(0x3f7fffffU), f(0xafe5f921U), f(0xb98a771cU)},
         {f(0x36bc8b46U), f(0x3f7ff12aU), f(0x3cae47e3U)},
         {f(0x398a6f16U), f(0xbcae47e4U), f(0x3f7ff12aU)}}},
       {f(0x42d3eed3U), f(0xc21314b9U), f(0x41b69ce4U)},
       {f(0x00000000U), f(0x395c96efU), f(0x00000000U)},
       {f(0xb4ab4554U), f(0xaeaf54a6U), f(0xac1c8230U)},
       {f(0x00000000U), f(0x397b51ccU), f(0x00000000U)},
       {f(0x360eec12U), f(0xaeb36f0bU), f(0xac2012fdU)},
       {f(0x3f4a761eU), f(0x3f4a761eU), f(0x3f4a77f0U), f(0x3f4a77f0U)}},
      {1U,
       {{{f(0x3f7ffffdU), f(0x35835dcfU), f(0xba1cdb21U)},
         {f(0xb5835a53U), f(0x3f800000U), f(0x343641f0U)},
         {f(0x3a1cdb21U), f(0xb435a0f7U), f(0x3f7ffffdU)}}},
       {f(0x42d3ee3bU), f(0xc212d975U), f(0xbf990f62U)},
       {f(0x00000000U), f(0xb9a8ca2cU), f(0x00000000U)},
       {f(0xaca237e9U), f(0xa93d0f4dU), f(0x2b8c6151U)},
       {f(0x00000000U), f(0xb9b2dbbfU), f(0x00000000U)},
       {f(0xac99e8e0U), f(0xa930517eU), f(0x2b8d3d15U)},
       {f(0x3f4aae14U), f(0x3f4aae14U), f(0x3f4aae14U), f(0x3f4aae14U)}},
      {2U,
       {{{f(0x3f7ffffcU), f(0x34bc4ee7U), f(0xba328114U)},
         {f(0xb4bc4d6eU), f(0x3f800000U), f(0x328831b4U)},
         {f(0x3a328114U), f(0xb286247fU), f(0x3f7ffffcU)}}},
       {f(0x42bbee13U), f(0xc212db80U), f(0x3f4d865cU)},
       {f(0x00000000U), f(0xba1d4fb4U), f(0x00000000U)},
       {f(0xad46348cU), f(0x288bcf30U), f(0xabd87d7aU)},
       {f(0x00000000U), f(0xba141060U), f(0x00000000U)},
       {f(0xad44a365U), f(0x288c6ad5U), f(0xabd4c782U)},
       {f(0x3f48d2a0U), f(0x3f48d2a0U), f(0x3f48d2a0U), f(0x3f48d2a0U)}},
      {3U,
       {{{f(0x3f7ffffcU), f(0x34bc4ee7U), f(0xba328114U)},
         {f(0xb4bc4d6eU), f(0x3f800000U), f(0x328831b4U)},
         {f(0x3a328114U), f(0xb286247fU), f(0x3f7ffffcU)}}},
       {f(0x42d3ee09U), f(0xc212db80U), f(0x412cd8aeU)},
       {f(0x00000000U), f(0xba1d4fb4U), f(0x00000000U)},
       {f(0xad46348cU), f(0x288bcf30U), f(0xabd87d7aU)},
       {f(0x00000000U), f(0xba141060U), f(0x00000000U)},
       {f(0xad44a365U), f(0x288c6ad5U), f(0xabd4c782U)},
       {f(0x3f48d2a0U), f(0x3f48d2a0U), f(0x3f48d2a0U), f(0x3f48d2a0U)}},
      {4U,
       {{{f(0x3f7fffffU), f(0x2f8e0f9cU), f(0xb98773e3U)},
         {f(0x36b86b5fU), f(0x3f7ff12aU), f(0x3cae47deU)},
         {f(0x39876c0bU), f(0xbcae47dfU), f(0x3f7ff12aU)}}},
       {f(0x42bbeedeU), f(0xc21314baU), f(0x414d3876U)},
       {f(0x00000000U), f(0x3a56c131U), f(0x00000000U)},
       {f(0x35a5554fU), f(0x2e18cc91U), f(0x2c5846deU)},
       {f(0x00000000U), f(0x3a5c5b0eU), f(0x00000000U)},
       {f(0xb57e56bdU), f(0x2df56dabU), f(0x2c535790U)},
       {f(0x3f4a7536U), f(0x3f4a7536U), f(0x3f4a761eU), f(0x3f4a761eU)}},
      {5U,
       {{{f(0x3f7ffffcU), f(0x34bc4ee7U), f(0xba328114U)},
         {f(0xb4bc4d6eU), f(0x3f800000U), f(0x328831b4U)},
         {f(0x3a328114U), f(0xb286247fU), f(0x3f7ffffcU)}}},
       {f(0x42bbedfaU), f(0xc212db80U), f(0x41c66c4cU)},
       {f(0x00000000U), f(0xba1d4fb4U), f(0x00000000U)},
       {f(0xad46348cU), f(0x288bcf30U), f(0xabd87d7aU)},
       {f(0x00000000U), f(0xba141060U), f(0x00000000U)},
       {f(0xad44a365U), f(0x288c6ad5U), f(0xabd4c782U)},
       {f(0x3f48d2a0U), f(0x3f48d2a0U), f(0x3f48d2a0U), f(0x3f48d2a0U)}},
      {6U,
       {{{f(0x3f7ffffdU), f(0x35835dcfU), f(0xba1cdb21U)},
         {f(0xb5835a53U), f(0x3f800000U), f(0x343641f0U)},
         {f(0x3a1cdb21U), f(0xb435a0f7U), f(0x3f7ffffdU)}}},
       {f(0x42d3ee16U), f(0xc212d975U), f(0x420b376fU)},
       {f(0x00000000U), f(0xb9a8ca2cU), f(0x00000000U)},
       {f(0xaca237e9U), f(0xa93d0f4dU), f(0x2b8c6151U)},
       {f(0x00000000U), f(0xb9b2dbbfU), f(0x00000000U)},
       {f(0xac99e8e0U), f(0xa930517eU), f(0x2b8d3d15U)},
       {f(0x3f4aae14U), f(0x3f4aae14U), f(0x3f4aae14U), f(0x3f4aae14U)}},
      {7U,
       {{{f(0x3f7ffffcU), f(0x34bc4ee7U), f(0xba328114U)},
         {f(0xb4bc4d6eU), f(0x3f800000U), f(0x328831b4U)},
         {f(0x3a328114U), f(0xb286247fU), f(0x3f7ffffcU)}}},
       {f(0x42bbedeeU), f(0xc212db80U), f(0x42133619U)},
       {f(0x00000000U), f(0xba1d4fb4U), f(0x00000000U)},
       {f(0xad46348cU), f(0x288bcf30U), f(0xabd87d7aU)},
       {f(0x00000000U), f(0xba141060U), f(0x00000000U)},
       {f(0xad44a365U), f(0x288c6ad5U), f(0xabd4c782U)},
       {f(0x3f48d2a0U), f(0x3f48d2a0U), f(0x3f48d2a0U), f(0x3f48d2a0U)}},
  }};
  return states;
}

OriginalAiTrafficSafety original_ai_predict_traffic_safety(
    const OriginalAiTrafficSafetyInput &input,
    const std::span<const OriginalAiTrafficVehicle> vehicles) {
  const auto finite_vehicle = [](const OriginalAiTrafficVehicle &vehicle) {
    const auto &frame = vehicle.observation.route_frame;
    const std::array values{vehicle.current_speed,
                            vehicle.half_width,
                            vehicle.half_length,
                            vehicle.target_lateral_offset,
                            vehicle.observation.route_longitudinal_projection,
                            frame.measured_lateral_offset,
                            frame.direction_alignment,
                            frame.route_side_cross};
    return std::all_of(values.begin(), values.end(),
                       [](const float value) { return std::isfinite(value); });
  };
  if (input.route_sample_count <= 0 ||
      input.self.observation.route_sample < 0 ||
      input.self.observation.route_sample >= input.route_sample_count ||
      input.self.half_width < 0.0F || input.self.half_length < 0.0F ||
      !std::isfinite(input.route_lateral_extent_a) ||
      !std::isfinite(input.route_lateral_extent_b) ||
      input.route_lateral_extent_a < 0.0F ||
      input.route_lateral_extent_b < 0.0F || !finite_vehicle(input.self)) {
    throw std::invalid_argument(
        "AI traffic safety requires a bounded finite self state");
  }

  const auto &self_frame = input.self.observation.route_frame;
  auto nearest_distance = std::numeric_limits<float>::infinity();
  const OriginalAiTrafficVehicle *nearest = nullptr;
  auto nearest_lateral_clearance = 0.0F;
  auto nearest_longitudinal_clearance = 0.0F;
  for (const auto &other : vehicles) {
    if (other.slot_index == input.self.slot_index) {
      continue;
    }
    if (other.observation.route_sample < 0 ||
        other.observation.route_sample >= input.route_sample_count ||
        other.half_width < 0.0F || other.half_length < 0.0F ||
        !finite_vehicle(other)) {
      throw std::invalid_argument(
          "AI traffic safety requires bounded finite opponent states");
    }
    const auto distance =
        circular_forward_distance(input.self, other, input.route_sample_count);
    const auto projected_other_width = static_cast<float>(
        (1.0L - static_cast<long double>(
                    other.observation.route_frame.direction_alignment)) *
            static_cast<long double>(other.half_length) +
        static_cast<long double>(other.half_width) *
            static_cast<long double>(
                other.observation.route_frame.direction_alignment));
    const auto lateral_clearance =
        static_cast<float>((static_cast<long double>(input.self.half_width) +
                            static_cast<long double>(projected_other_width)) *
                               0.5L +
                           0.1L);
    const auto longitudinal_clearance =
        static_cast<float>((static_cast<long double>(input.self.half_length) +
                            static_cast<long double>(other.half_length)) *
                               0.5L +
                           0.35L);
    const auto lateral_separation =
        std::fabs(other.observation.route_frame.measured_lateral_offset -
                  self_frame.measured_lateral_offset);
    if (!(lateral_separation < lateral_clearance)) {
      continue;
    }

    const auto closure_speed =
        std::max(0.0F, input.self.current_speed - other.current_speed);
    const auto prediction_horizon =
        std::max(longitudinal_clearance + 1.5F,
                 longitudinal_clearance + input.self.current_speed * 1.25F +
                     closure_speed * 3.0F);
    if (distance > prediction_horizon || distance >= nearest_distance) {
      continue;
    }
    nearest_distance = distance;
    nearest = &other;
    nearest_lateral_clearance = lateral_clearance;
    nearest_longitudinal_clearance = longitudinal_clearance;
  }

  if (nearest == nullptr) {
    return {};
  }

  constexpr float lateral_step = 0.2F;
  const auto route_clearance = nearest_lateral_clearance + 0.2F;
  const auto positive_space_available =
      input.route_lateral_extent_b -
              (input.self.target_lateral_offset + lateral_step) >
          route_clearance &&
      !input.negative_side_occupied;
  const auto negative_space_available =
      input.route_lateral_extent_a +
              (input.self.target_lateral_offset - lateral_step) >
          route_clearance &&
      !input.positive_side_occupied;

  OriginalAiTrafficSafety result;
  result.blocking_slot = nearest->slot_index;
  const auto other_lateral =
      nearest->observation.route_frame.measured_lateral_offset;
  const auto self_lateral = self_frame.measured_lateral_offset;
  const auto prefer_positive =
      other_lateral < self_lateral ||
      (other_lateral == self_lateral && (input.self.slot_index & 1U) == 0U);
  if (prefer_positive) {
    if (positive_space_available) {
      result.signed_avoidance_offset = lateral_step;
    } else if (negative_space_available) {
      result.signed_avoidance_offset = -lateral_step;
    }
  } else {
    if (negative_space_available) {
      result.signed_avoidance_offset = -lateral_step;
    } else if (positive_space_available) {
      result.signed_avoidance_offset = lateral_step;
    }
  }

  // Retained only as an isolated diagnostic for the removed pre-contact host
  // guard. Live traffic control is owned by the recovered selector.
  result.force_full_brake =
      nearest_distance <= nearest_longitudinal_clearance + 0.75F;
  return result;
}

OriginalAiVehicleObservation
original_ai_observe_vehicle(const mh::content::AiRouteData &route,
                            OriginalAiRouteCursor &cursor,
                            const OriginalBodyPoseState &vehicle_pose) {
  const std::array<float, 2U> vehicle_xz{
      static_cast<float>(vehicle_pose.world_position[0U]),
      static_cast<float>(vehicle_pose.world_position[2U])};
  cursor.update_current(vehicle_xz);
  const auto current_index = cursor.current_sample();
  const auto &current = route.samples[current_index];
  OriginalAiRouteFrameInput input;
  input.route_x = current.position[0U];
  input.route_z = current.position[1U];
  input.route_tangent_x = current.direction[0U];
  input.route_tangent_z = current.direction[2U];
  input.vehicle_x = vehicle_xz[0U];
  input.vehicle_z = vehicle_xz[1U];
  input.vehicle_forward_x = static_cast<float>(vehicle_pose.body_basis[2U][0U]);
  input.vehicle_forward_z = static_cast<float>(vehicle_pose.body_basis[2U][2U]);
  const auto longitudinal_projection =
      (input.vehicle_x - input.route_x) * input.route_tangent_x +
      (input.vehicle_z - input.route_z) * input.route_tangent_z;
  return {static_cast<std::int32_t>(current_index),
          original_ai_measure_route_frame(input), longitudinal_projection};
}

OriginalAiVehicleController::OriginalAiVehicleController(
    const mh::content::AiRouteData &route, const OriginalAiVehicleTuning tuning,
    const std::array<float, 2U> &initial_vehicle_xz,
    OriginalAiRandomState &shared_random_state)
    : route_(&route), tuning_(tuning), cursor_(route, initial_vehicle_xz),
      stochastic_state_(original_ai_initial_stochastic_control(
          tuning.stochastic_base_a, tuning.stochastic_base_b,
          tuning.stochastic_base_c)),
      shared_random_state_(&shared_random_state) {
  controller_state_.positive_request_accumulator = 1.0F;
  const auto finite = {tuning.route_spacing_control,
                       tuning.runtime_speed_scale,
                       tuning.runtime_group_scale,
                       tuning.stochastic_base_a,
                       tuning.stochastic_base_b,
                       tuning.stochastic_base_c,
                       tuning.half_width,
                       tuning.half_length};
  if (!std::all_of(finite.begin(), finite.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      tuning.route_spacing_control == 0.0F ||
      tuning.runtime_speed_scale < 0.0F || tuning.half_width < 0.0F ||
      tuning.half_length < 0.0F ||
      route.samples.size() >
          static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
    throw std::invalid_argument(
        "original AI vehicle controller requires bounded finite tuning");
  }
}

OriginalAiControllerStep OriginalAiVehicleController::step(
    const OriginalBodyPoseState &vehicle_pose, const float current_speed,
    const float time_step_seconds,
    const std::span<const OriginalAiAvoidanceOpponent> opponents,
    const bool positive_side_occupied, const bool negative_side_occupied,
    const float external_signed_avoidance_offset,
    const bool external_force_full_brake) {
  if (!std::isfinite(current_speed) || !std::isfinite(time_step_seconds) ||
      !std::isfinite(external_signed_avoidance_offset) ||
      time_step_seconds < 0.0F) {
    throw std::invalid_argument(
        "original AI vehicle step requires finite speed and time");
  }

  const auto observation = observe(vehicle_pose);
  const auto current_index = static_cast<std::size_t>(observation.route_sample);
  const auto sample_count = route_->samples.size();
  const auto &current = route_->samples[current_index];

  const auto horizon_end = original_ai_initial_lookahead_sample(
      current_index, sample_count, current_speed,
      tuning_.route_spacing_control);
  const auto horizon_count =
      forward_sample_distance(current_index, horizon_end, sample_count);
  auto &horizon = lookahead_candidates_;
  horizon.clear();
  horizon.reserve(horizon_count);
  for (std::size_t offset = 0U; offset < horizon_count; ++offset) {
    const auto index = (current_index + offset) % sample_count;
    horizon.push_back({static_cast<std::int32_t>(offset),
                       route_->samples[index].signed_turn_radius});
  }

  const auto retained_offset = forward_sample_distance(
      current_index, cursor_.lookahead_sample(), sample_count);
  OriginalAiLookaheadSelectionInput lookahead;
  lookahead.retained_candidate = {
      static_cast<std::int32_t>(retained_offset),
      route_->samples[cursor_.lookahead_sample()].signed_turn_radius};
  lookahead.current_speed = current_speed;
  lookahead.route_spacing_control = tuning_.route_spacing_control;
  lookahead.curvature_state = tuning_.stochastic_base_b;
  lookahead.curvature_limit = stochastic_state_.channel_b;
  lookahead.global_speed_scale = tuning_.runtime_speed_scale;
  lookahead.vehicle_curve_coefficient = 1.0F;
  const auto selected =
      original_ai_select_lookahead_candidate(lookahead, horizon);
  const auto selected_offset =
      static_cast<std::size_t>(selected.selected_candidate.route_record_offset);
  if (selected_offset >= sample_count) {
    throw std::runtime_error(
        "original AI lookahead selected an invalid route offset");
  }
  const auto selected_index = (current_index + selected_offset) % sample_count;
  cursor_.select_lookahead_sample(selected_index);
  if (selected.braking_sample_delta_updated) {
    braking_sample_delta_ = selected.braking_sample_delta;
  }

  original_ai_update_stochastic_control(stochastic_state_,
                                        *shared_random_state_);
  const auto &group = route_group(*route_, current.group_index);
  const auto group_control = original_ai_derive_group_control(
      group.signed_turn_radius, tuning_.runtime_group_scale);

  const auto &measured = observation.route_frame;

  OriginalAiControllerInput controller;
  controller.time_step_seconds = time_step_seconds;
  controller.current_speed = current_speed;
  controller.current_signed_turn_radius = current.signed_turn_radius;
  controller.current_group_signed_turn_radius = group.signed_turn_radius;
  controller.lookahead_signed_turn_radius =
      route_->samples[selected_index].signed_turn_radius;
  controller.curvature_state = tuning_.stochastic_base_b;
  controller.curvature_limit = stochastic_state_.channel_b;
  controller.global_speed_scale = tuning_.runtime_speed_scale;
  controller.vehicle_curve_coefficient =
      group_control.safe_speed_curve_coefficient;
  controller.vehicle_feed_forward_coefficient =
      group_control.curve_feed_forward_coefficient;
  controller.route_lateral_extent_a = current.surface_values[0U];
  controller.route_lateral_extent_b = current.surface_values[1U];
  controller.lateral_gain = nonzero_radius(current.signed_turn_radius)
                                ? group_control.lateral_gain
                                : 0.5F;
  controller.lateral_rate_correction_scale =
      nonzero_radius(current.signed_turn_radius)
          ? group_control.lateral_rate_correction_scale
          : 3.9F;
  controller.measured_lateral_offset = measured.measured_lateral_offset;
  controller.direction_alignment = measured.direction_alignment;
  controller.route_side_cross = measured.route_side_cross;
  controller.half_width = tuning_.half_width;
  constexpr float edge_margin = 0.35F;
  if (lateral_preference_ < 0.0F) {
    controller.preferred_lateral_offset =
        lateral_preference_ *
        std::max(0.0F, current.surface_values[0U] - tuning_.half_width -
                           edge_margin);
  } else {
    controller.preferred_lateral_offset =
        lateral_preference_ *
        std::max(0.0F, current.surface_values[1U] - tuning_.half_width -
                           edge_margin);
  }
  controller.route_sample_count = static_cast<std::int32_t>(sample_count);
  controller.route_sample = static_cast<std::int32_t>(current_index);
  controller.forward_aligned = measured.forward_aligned;
  controller.lookahead_curve_available =
      nonzero_radius(controller.lookahead_signed_turn_radius) &&
      static_cast<std::int64_t>(selected_offset) -
              static_cast<std::int64_t>(braking_sample_delta_) <=
          0;
  // p3.1 reads the retained per-vehicle +0x950 flag before making this
  // frame's longitudinal decision. The same flag is updated later by the
  // stuck detector and also owns the final recovery-steering inversion.
  controller.recovery_brake = controller_state_.stuck_recovery.active;
  controller.positive_side_occupied = positive_side_occupied;
  controller.negative_side_occupied = negative_side_occupied;
  controller.external_signed_avoidance_offset =
      external_signed_avoidance_offset;
  controller.external_force_full_brake = external_force_full_brake;
  auto result =
      original_ai_controller_step(controller, opponents, controller_state_);

  // Off-road recovery already full-steers, but a centered backward car gets a
  // zero lateral request. At the recovered +/-0.5 alignment boundary, latch
  // one recovery side until forward alignment returns; this also spans the
  // exactly-opposite cross-product singularity.
  constexpr float turnaround_alignment = 0.5F;
  if (!turnaround_recovery_active_ &&
      measured.direction_alignment < -turnaround_alignment) {
    turnaround_recovery_active_ = true;
    if (measured.route_side_cross > 0.0F) {
      turnaround_recovery_steering_ = 1.0F;
    } else if (measured.route_side_cross < 0.0F) {
      turnaround_recovery_steering_ = -1.0F;
    } else {
      turnaround_recovery_steering_ =
          measured.measured_lateral_offset < 0.0F ? 1.0F : -1.0F;
    }
  }
  if (turnaround_recovery_active_) {
    if (measured.direction_alignment > turnaround_alignment) {
      turnaround_recovery_active_ = false;
      turnaround_recovery_steering_ = 0.0F;
    } else {
      result.controls.throttle = 1.0F;
      result.controls.brake = 0.0F;
      result.controls.steering = turnaround_recovery_steering_;
      result.signed_longitudinal_request = 1.0F;
      result.forced_brake_for_avoidance = false;
    }
  }
  return result;
}

OriginalAiVehicleObservation OriginalAiVehicleController::observe(
    const OriginalBodyPoseState &vehicle_pose) {
  return original_ai_observe_vehicle(*route_, cursor_, vehicle_pose);
}

void OriginalAiVehicleController::initialize_lateral_target(
    const OriginalBodyPoseState &vehicle_pose) {
  initialize_lateral_target(
      std::fabs(observe(vehicle_pose).route_frame.measured_lateral_offset));
}

void OriginalAiVehicleController::set_lateral_preference(
    const float normalized_preference) {
  if (!std::isfinite(normalized_preference) ||
      normalized_preference < -1.0F || normalized_preference > 1.0F) {
    throw std::invalid_argument(
        "AI lateral preference must be finite and within [-1, 1]");
  }
  lateral_preference_ = normalized_preference;
}

void OriginalAiVehicleController::initialize_lateral_target(
    const float retained_lateral_magnitude) {
  if (!std::isfinite(retained_lateral_magnitude) ||
      retained_lateral_magnitude < 0.0F) {
    throw std::invalid_argument(
        "original AI lateral target requires a finite magnitude");
  }
  controller_state_.target_lateral_offset = retained_lateral_magnitude;
  // p3.1 initialization stores the same unsigned perpendicular magnitude at
  // AI state +0x4c and +0x50. The latter is the retained previous-lateral
  // value consumed by the first runtime derivative.
  controller_state_.lateral_rate.previous_lateral_offset =
      retained_lateral_magnitude;
}

void OriginalAiVehicleController::reset(
    const std::array<float, 2U> &vehicle_xz) {
  cursor_.reset(vehicle_xz);
  controller_state_ = {};
  controller_state_.positive_request_accumulator = 1.0F;
  stochastic_state_ = original_ai_initial_stochastic_control(
      tuning_.stochastic_base_a, tuning_.stochastic_base_b,
      tuning_.stochastic_base_c);
  turnaround_recovery_active_ = false;
  turnaround_recovery_steering_ = 0.0F;
  braking_sample_delta_ = 0;
}

std::size_t OriginalAiVehicleController::current_sample() const noexcept {
  return cursor_.current_sample();
}

std::size_t OriginalAiVehicleController::lookahead_sample() const noexcept {
  return cursor_.lookahead_sample();
}

std::int32_t
OriginalAiVehicleController::braking_sample_delta() const noexcept {
  return braking_sample_delta_;
}

const OriginalAiVehicleTuning &
OriginalAiVehicleController::tuning() const noexcept {
  return tuning_;
}

const OriginalAiControllerState &
OriginalAiVehicleController::controller_state() const noexcept {
  return controller_state_;
}

const OriginalAiStochasticControlState &
OriginalAiVehicleController::stochastic_state() const noexcept {
  return stochastic_state_;
}

} // namespace mh::game
