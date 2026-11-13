#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace mh::content {

struct FrontEndUnlockTrack {
  std::uint32_t division = 0U;
  bool flag_available = false;
};

struct FrontEndUnlockCar {
  std::uint32_t division = 0U;
};

// Source-backed p3.1 car/track menu catalog. The order vectors map each retail
// menu slot to its input definition index. Counts are exclusive upper bounds,
// matching the globals consumed by the original bank-2/bank-3 handlers.
struct FrontEndUnlockCatalog {
  std::vector<std::size_t> track_order;
  std::vector<std::size_t> car_order;
  std::uint32_t track_count = 0U;
  std::uint32_t unlocked_track_count = 0U;
  std::uint32_t car_count = 0U;
  std::uint32_t unlocked_car_count = 0U;
};

// Reproduces the p3.1 setup owner at RVA 0x0007f1a0..0x0007f7a6. Definitions
// are stable-sorted by their authored Division field. A track FLG is the
// original progression input; all_unlock is the separately recovered retail
// player/team identity bypass.
[[nodiscard]] FrontEndUnlockCatalog derive_front_end_unlock_catalog(
    std::span<const FrontEndUnlockTrack> tracks,
    std::span<const FrontEndUnlockCar> cars, bool all_unlock = false);

// Maps the currently unlocked retail car groups to selectable League
// divisions. The returned order follows League progression: 3, 2, 1, 0.
[[nodiscard]] std::vector<std::uint32_t> unlocked_league_divisions(
    std::span<const FrontEndUnlockCar> cars,
    const FrontEndUnlockCatalog &catalog);

// p3.1 persists a completed League schedule by creating encoded
// Tracks/<track>/Data/<TrackName>.FLG files. The writer creates missing parent
// directories below the caller-selected root. These helpers reproduce and
// validate that deterministic source format rather than treating arbitrary
// files with the extension as progress.
[[nodiscard]] bool original_track_unlock_flag_valid(
    const std::filesystem::path &path, std::string_view track_name);
void write_original_track_unlock_flag(const std::filesystem::path &path,
                                      std::string_view track_name);

} // namespace mh::content
