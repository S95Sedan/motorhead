#pragma once

#include <cstddef>
#include <cstdint>

namespace mh::ui {

// Named p3.1 sprpos.dta indices. These identify logical 640x400 positions;
// presentation scaling to the output viewport is a separate concern.
inline constexpr std::size_t main_menu_position_bank = 0U;
inline constexpr std::size_t one_player_position_bank = 10U;
inline constexpr std::size_t options_menu_position_bank = 7U;
inline constexpr std::size_t race_setup_position_bank = 2U;
inline constexpr std::size_t car_setup_position_bank = 3U;
inline constexpr std::size_t league_overview_position_bank = 12U;
inline constexpr std::size_t league_create_position_bank = 13U;
inline constexpr std::size_t league_select_position_bank = 14U;
inline constexpr std::size_t league_delete_position_bank = 15U;
inline constexpr std::size_t league_summary_position_bank = 16U;
inline constexpr std::size_t rankings_position_bank = 11U;
inline constexpr std::size_t credits_position_bank = 18U;
inline constexpr std::size_t ghost_setup_position_bank = 19U;
inline constexpr std::size_t multiplayer_position_bank = 9U;

enum class MainMenuPositionRecord : std::uint8_t {
  background_dots = 1U,
  screen_badge = 2U,
  selector = 3U,
  header = 5U,
  one_player = 6U,
  multi_player = 7U,
  rankings = 8U,
  options = 9U,
  credits = 10U,
  pointer_center = 30U,
};

enum class OptionsMenuPositionRecord : std::uint8_t {
  background_dots = 1U,
  header = 2U,
  screen_badge = 3U,
  selector = 4U,
  personal_options = 5U,
  graphic_options = 6U,
  control_options = 7U,
  sound_options = 8U,
  difficulty = 9U,
  difficulty_frame = 10U,
  difficulty_value = 11U,
  pointer_center = 30U,
};

enum class OnePlayerPositionRecord : std::uint8_t {
  background_dots = 1U,
  screen_badge = 2U,
  selector = 3U,
  header = 4U,
  quick_race = 5U,
  single_race = 6U,
  league_race = 7U,
  time_attack = 8U,
  ghost_mode = 9U,
  information_left = 10U,
  information_right = 11U,
  information_label_offset = 20U,
  information_row_offset = 21U,
  information_value_offset = 22U,
  pointer_center = 30U,
};

enum class GhostSetupPositionRecord : std::uint8_t {
  background_dots = 1U,
  screen_badge = 2U,
  header = 3U,
  ghost_race = 4U,
  replay_race = 5U,
  benchmark_race = 6U,
  file_frame = 10U,
  benchmark_message = 12U,
  delete_instruction = 21U,
  file_name = 24U,
};

// p3.1 network-type-4 Multiplayer destination (initializer RVA 0x81bdc,
// renderer RVA 0x81db8, input owner RVA 0x82e58). Records 0..20 are backed
// by the exact MENU.SPR identities loaded by the initializer.
enum class MultiplayerPositionRecord : std::uint8_t {
  port = 0U,
  background_dots = 1U,
  screen_badge = 2U,
  header = 3U,
  join = 4U,
  create = 5U,
  start = 6U,
  session_name = 7U,
  session_password = 8U,
  protocol = 9U,
  serial_port = 10U,
  baud_rate = 11U,
  parity = 12U,
  stop_bits = 13U,
  flow_control = 14U,
  description = 15U,
  ip_address = 16U,
  phone_number = 17U,
  address_frame = 18U,
  session_list_frame = 19U,
  list_marker = 20U,
  selected_session_frame = 28U,
  player_list = 29U,
  ip_frame = 30U,
};

enum class LeagueOverviewPositionRecord : std::uint8_t {
  background_dots = 1U,
  screen_badge = 2U,
  selector = 3U,
  header = 5U,
  continue_race = 6U,
  view_stats = 7U,
  select_league = 8U,
  new_league = 9U,
  delete_league = 10U,
  information_left = 11U,
  information_right = 12U,
  pointer_target = 24U,
  pointer_center = 30U,
};

enum class LeagueSelectPositionRecord : std::uint8_t {
  background_dots = 1U,
  screen_badge = 2U,
  header = 3U,
  screen_title = 4U,
  selected_league_frame = 5U,
  row_step = 20U,
  list_offset = 21U,
  information_offset = 22U,
  information_row = 23U,
  information_value = 24U,
};

enum class LeagueSummaryPositionRecord : std::uint8_t {
  background_dots = 1U,
  screen_badge = 2U,
  header = 3U,
  upper_panel = 16U,
  upper_columns = 17U,
  upper_values = 18U,
  standings_panel = 19U,
  standings_columns = 20U,
};

enum class RaceSetupPositionRecord : std::uint8_t {
  background_dots = 1U,
  header = 2U,
  screen_badge = 3U,
  preview_frame = 4U,
  option_frame = 5U,
  track_label = 6U,
  laps_label = 7U,
  catch_up_label = 8U,
  catch_up_value = 9U,
  catch_up_frame = 10U,
  laps_value = 18U,
  track_name = 19U,
  locked_label = 24U,
  line_object = 30U,
};

enum class CarSetupPositionRecord : std::uint8_t {
  background_dots = 1U,
  header = 2U,
  screen_badge = 3U,
  preview_frame = 4U,
  stat_swoosh = 5U,
  stat_speed = 6U,
  stat_acceleration = 7U,
  stat_grip = 8U,
  option_frame = 9U,
  car_label = 10U,
  transmission_label = 11U,
  horn_label = 12U,
  record_race_label = 13U,
  speed_label = 14U,
  acceleration_label = 15U,
  grip_label = 16U,
  transmission_value = 17U,
  record_race_value = 18U,
  horn_value = 19U,
  car_name = 24U,
  car_information_frame = 27U,
  line_object = 29U,
  preview_image = 30U,
  option_value_frame = 31U,
};

// p3.1 Credits descriptor at VA 0x004fa04c, renderer RVA 0x8d464.
enum class CreditsPositionRecord : std::uint8_t {
  background_dots = 1U,
  screen_badge = 2U,
  header = 3U,
  text_origin = 24U,
  text_spacing = 25U,
};

// p3.1 Rankings descriptor at VA 0x004fa030, renderer RVA 0x8482c.
enum class RankingsPositionRecord : std::uint8_t {
  background_dots = 1U,
  header = 2U,
  screen_badge = 3U,
  type_label = 4U,
  track_or_division_label = 5U,
  laps_label = 6U,
  difficulty_label = 7U,
  table_header = 8U,
  table_frame = 9U,
  table_bracket = 10U,
  first_name_column = 16U,
  row_step = 17U,
  vehicle_column = 18U,
  best_time_column = 19U,
  type_value = 24U,
  track_or_division_value = 25U,
  laps_value = 26U,
  difficulty_value = 27U,
};

[[nodiscard]] constexpr std::size_t
position_record_index(const MainMenuPositionRecord record) noexcept {
  return static_cast<std::size_t>(record);
}

[[nodiscard]] constexpr std::size_t
position_record_index(const OptionsMenuPositionRecord record) noexcept {
  return static_cast<std::size_t>(record);
}

[[nodiscard]] constexpr std::size_t
position_record_index(const OnePlayerPositionRecord record) noexcept {
  return static_cast<std::size_t>(record);
}

[[nodiscard]] constexpr std::size_t
position_record_index(const GhostSetupPositionRecord record) noexcept {
  return static_cast<std::size_t>(record);
}

[[nodiscard]] constexpr std::size_t
position_record_index(const MultiplayerPositionRecord record) noexcept {
  return static_cast<std::size_t>(record);
}

[[nodiscard]] constexpr std::size_t
position_record_index(const LeagueOverviewPositionRecord record) noexcept {
  return static_cast<std::size_t>(record);
}

[[nodiscard]] constexpr std::size_t
position_record_index(const LeagueSelectPositionRecord record) noexcept {
  return static_cast<std::size_t>(record);
}

[[nodiscard]] constexpr std::size_t
position_record_index(const LeagueSummaryPositionRecord record) noexcept {
  return static_cast<std::size_t>(record);
}

[[nodiscard]] constexpr std::size_t
position_record_index(const RaceSetupPositionRecord record) noexcept {
  return static_cast<std::size_t>(record);
}

[[nodiscard]] constexpr std::size_t
position_record_index(const CarSetupPositionRecord record) noexcept {
  return static_cast<std::size_t>(record);
}

[[nodiscard]] constexpr std::size_t
position_record_index(const CreditsPositionRecord record) noexcept {
  return static_cast<std::size_t>(record);
}

[[nodiscard]] constexpr std::size_t
position_record_index(const RankingsPositionRecord record) noexcept {
  return static_cast<std::size_t>(record);
}

} // namespace mh::ui
