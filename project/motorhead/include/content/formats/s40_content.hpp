#pragma once

#include <content/formats/car_definition.hpp>
#include <content/formats/unlock_catalog.hpp>

#include <algorithm>
#include <cctype>
#include <string>

namespace mh::content {

[[nodiscard]] inline bool s40_progression_reward(
    const std::span<const FrontEndUnlockTrack> tracks,
    const std::span<const FrontEndUnlockCar> cars) {
  const auto earned = derive_front_end_unlock_catalog(tracks, cars, false);
  return !tracks.empty() && !cars.empty() &&
         earned.track_count == tracks.size() &&
         earned.unlocked_track_count == tracks.size() &&
         earned.car_count == cars.size() &&
         earned.unlocked_car_count == earned.car_count;
}

[[nodiscard]] inline bool is_s40_car(const CarDefinition &car) {
  auto model = car.model_base;
  std::transform(model.begin(), model.end(), model.begin(), [](unsigned char c) {
    return c == '\\' ? '/' : static_cast<char>(std::tolower(c));
  });
  auto name = car.name;
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return name == "volvo s40" &&
         (model == "cars/car99/car99" || model == "cars/car15/car15" ||
          model == "cars/s40/s40");
}

} // namespace mh::content
