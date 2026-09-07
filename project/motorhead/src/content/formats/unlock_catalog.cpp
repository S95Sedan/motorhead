#include <content/formats/unlock_catalog.hpp>

#include <core/filesystem/atomic_file.hpp>
#include <core/error.hpp>
#include <content/formats/tbl_envelope.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <fstream>
#include <string>

namespace mh::content {
namespace {

std::size_t division_group(const std::uint32_t division) noexcept {
  const auto group = division < 100U ? 1U : (division / 100U) % 10U;
  return static_cast<std::size_t>(std::clamp(group, 1U, 4U) - 1U);
}

std::uint32_t bounded_u32(const std::size_t value) noexcept {
  return static_cast<std::uint32_t>(
      std::min(value, static_cast<std::size_t>(
                          std::numeric_limits<std::uint32_t>::max())));
}

std::string lower_ascii(std::string_view value) {
  std::string result(value);
  for (auto &character : result) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
  }
  return result;
}

std::vector<std::uint8_t> original_flag_payload(
    const std::string_view track_name) {
  const auto lowered = lower_ascii(track_name);
  std::size_t period = 0U;
  for (const auto character : track_name) {
    const auto byte = static_cast<unsigned char>(character);
    period += byte >= 'a' && byte <= 'z' ? byte - ('a' - 'A') : byte;
  }
  if (lowered.empty() || period < 4U) {
    throw mh::common::ToolError(mh::common::ExitCode::format,
                                "track FLG name is invalid");
  }

  std::vector<std::uint8_t> current(period + 1U, 0U);
  std::vector<std::uint8_t> next(period + 1U, 0U);
  auto length = std::min(lowered.size(), period / 2U - 1U);
  std::copy_n(lowered.begin(), length, current.begin());
  std::uint16_t state = static_cast<std::uint16_t>(length);
  for (std::size_t round = 0U; round < 4U; ++round) {
    state = static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(state) * state);
  }
  while (true) {
    const auto half = period / 2U;
    length = std::min(length, half);
    state = static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(state) *
        static_cast<std::uint32_t>(length));
    for (std::size_t index = 0U; index < length; ++index) {
      const auto value = current[index];
      state = static_cast<std::uint16_t>(state + value);
      state = static_cast<std::uint16_t>(
          static_cast<std::uint32_t>(state) * state);
      state = static_cast<std::uint16_t>(state + value);
      next[index * 2U] = static_cast<std::uint8_t>(state & 0xffU);
      next[index * 2U + 1U] = static_cast<std::uint8_t>(state >> 8U);
    }
    length *= 2U;
    std::swap(current, next);
    if (length / 2U == half) {
      break;
    }
  }
  auto remaining = period - length;
  while (remaining > 0U) {
    current[length++] = next[remaining--];
  }
  current.resize(period);
  return current;
}

std::vector<std::uint8_t> original_flag_file(
    const std::string_view track_name) {
  const auto key = lower_ascii(track_name);
  const auto payload = original_flag_payload(track_name);
  const auto encoded = encode_tbl_transform(payload, key);
  std::vector<std::uint8_t> result{'T', 'B', 'L', 0x05U};
  result.insert(result.end(), encoded.begin(), encoded.end());
  return result;
}

} // namespace

FrontEndUnlockCatalog derive_front_end_unlock_catalog(
    const std::span<const FrontEndUnlockTrack> tracks,
    const std::span<const FrontEndUnlockCar> cars, const bool all_unlock) {
  FrontEndUnlockCatalog result;
  result.track_order.resize(tracks.size());
  std::iota(result.track_order.begin(), result.track_order.end(), 0U);
  std::stable_sort(result.track_order.begin(), result.track_order.end(),
                   [tracks](const std::size_t left, const std::size_t right) {
                     return tracks[left].division < tracks[right].division;
                   });

  result.car_order.resize(cars.size());
  std::iota(result.car_order.begin(), result.car_order.end(), 0U);
  std::stable_sort(result.car_order.begin(), result.car_order.end(),
                   [cars](const std::size_t left, const std::size_t right) {
                     return cars[left].division < cars[right].division;
                   });

  std::array<std::int32_t, 4U> group_tracks{};
  std::array<std::int32_t, 4U> normal_tracks{};
  std::array<std::int32_t, 4U> available_normal_tracks{};
  for (const auto source_index : result.track_order) {
    const auto &track = tracks[source_index];
    const auto group = division_group(track.division);
    ++group_tracks[group];
    if (track.division % 100U < 10U) {
      ++normal_tracks[group];
      if (all_unlock || track.flag_available) {
        ++available_normal_tracks[group];
      }
    }
  }

  // Retail subtracts one availability unit for each reverse definition while
  // that division's forward set is incomplete. This makes reverse layouts and
  // the next division appear only after all current forward FLGs exist.
  for (const auto source_index : result.track_order) {
    const auto &track = tracks[source_index];
    if (track.division % 100U < 10U) {
      continue;
    }
    const auto group = division_group(track.division);
    if (available_normal_tracks[group] < normal_tracks[group]) {
      --available_normal_tracks[group];
    }
  }

  std::size_t first_incomplete_group = group_tracks.size();
  std::int32_t unlocked_tracks = 0;
  for (std::size_t group = 0U; group < group_tracks.size(); ++group) {
    if (normal_tracks[group] != available_normal_tracks[group]) {
      first_incomplete_group = group;
      break;
    }
    unlocked_tracks += group_tracks[group];
  }
  if (first_incomplete_group < group_tracks.size()) {
    unlocked_tracks += normal_tracks[first_incomplete_group];
  }

  // p3.1's ordinary menu initially spans divisions 1/2 plus the normal
  // division-3 layouts. The upper bound expands whenever progression reaches
  // farther, and becomes the complete corpus after all four divisions close.
  std::int32_t track_count = group_tracks[0U] + group_tracks[1U] +
                             normal_tracks[2U];
  track_count = std::max(track_count, unlocked_tracks);
  if (first_incomplete_group == group_tracks.size()) {
    track_count = unlocked_tracks;
  }
  track_count = std::clamp<std::int32_t>(
      track_count, tracks.empty() ? 0 : 1,
      static_cast<std::int32_t>(tracks.size()));
  unlocked_tracks = std::clamp(unlocked_tracks, 0, track_count);
  result.track_count = static_cast<std::uint32_t>(track_count);
  result.unlocked_track_count = static_cast<std::uint32_t>(unlocked_tracks);

  auto unlocked_car_division = 4U;
  for (const auto source_index : result.track_order) {
    const auto &track = tracks[source_index];
    if (track.division % 100U < 10U &&
        !(all_unlock || track.flag_available)) {
      unlocked_car_division =
          static_cast<unsigned int>(division_group(track.division) + 1U);
      break;
    }
  }

  std::size_t standard_car_count = 0U;
  std::size_t unlocked_car_count = 0U;
  for (const auto source_index : result.car_order) {
    const auto &car = cars[source_index];
    if (car.division % 100U < 10U) {
      ++standard_car_count;
    }
    if (division_group(car.division) + 1U <= unlocked_car_division) {
      ++unlocked_car_count;
    }
  }
  const auto complete_track_catalog =
      !tracks.empty() && result.track_count == tracks.size() &&
      result.unlocked_track_count == tracks.size();
  const auto car_count = complete_track_catalog ? cars.size()
                                                : standard_car_count;
  result.car_count = bounded_u32(car_count);
  result.unlocked_car_count =
      bounded_u32(std::min(unlocked_car_count, car_count));
  return result;
}

std::vector<std::uint32_t> unlocked_league_divisions(
    const std::span<const FrontEndUnlockCar> cars,
    const FrontEndUnlockCatalog &catalog) {
  std::vector<std::uint32_t> result;
  const auto unlocked = std::min<std::size_t>(
      catalog.unlocked_car_count, catalog.car_order.size());
  result.reserve(4U);
  for (std::size_t slot = 0U; slot < unlocked; ++slot) {
    const auto source_index = catalog.car_order[slot];
    if (source_index >= cars.size()) {
      throw mh::common::ToolError(
          mh::common::ExitCode::format,
          "front-end unlock catalog car order is outside its source list");
    }
    const auto league_division =
        static_cast<std::uint32_t>(3U - division_group(cars[source_index].division));
    if (std::find(result.begin(), result.end(), league_division) ==
        result.end()) {
      result.push_back(league_division);
    }
  }
  return result;
}

bool original_track_unlock_flag_valid(const std::filesystem::path &path,
                                      const std::string_view track_name) {
  if (!std::filesystem::is_regular_file(path)) {
    return false;
  }
  try {
    const auto envelope = read_tbl(path, lower_ascii(track_name), 4096U, 4096U);
    return envelope.flags == 0x05U && envelope.decoded &&
           envelope.payload == original_flag_payload(track_name);
  } catch (...) {
    return false;
  }
}

void write_original_track_unlock_flag(const std::filesystem::path &path,
                                      const std::string_view track_name) {
  if (original_track_unlock_flag_valid(path, track_name)) {
    return;
  }
  const auto parent = path.parent_path();
  std::filesystem::create_directories(parent);
  if (!std::filesystem::is_directory(parent)) {
    throw mh::common::ToolError(mh::common::ExitCode::input,
                                "track FLG parent directory is unavailable");
  }
  const auto existing = std::filesystem::symlink_status(path);
  if (std::filesystem::exists(existing) &&
      (std::filesystem::is_symlink(existing) ||
       !std::filesystem::is_regular_file(existing))) {
    throw mh::common::ToolError(mh::common::ExitCode::input,
                                "track FLG output is not a plain file");
  }
  const auto file = original_flag_file(track_name);
  mh::common::write_atomic_file(
      path, file, [name = std::string(track_name)](const auto &candidate) {
        return original_track_unlock_flag_valid(candidate, name);
      });
}

} // namespace mh::content
