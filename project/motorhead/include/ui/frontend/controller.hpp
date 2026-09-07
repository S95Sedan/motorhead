#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mh::ui {

enum class FrontEndScreen : std::uint8_t {
  main_menu,
  multiplayer,
  one_player,
  ghost_setup,
  league_overview,
  league_summary,
  league_select,
  league_create,
  league_delete,
  race_setup,
  car_setup,
  options,
  personal_options,
  graphic_options,
  gameplay_options,
  control_options,
  sound_options,
  rankings,
  credits,
};

// The values preserve the observed clockwise order around the p3.1 selector,
// beginning at its startup choice.
enum class MainMenuChoice : std::uint8_t {
  one_player,
  multi_player,
  rankings,
  options,
  credits,
};

// Original bank-9 front-end and lobby state exposed by the retail p3.1
// executable and backed by the reconstruction-owned native LAN session.
enum class MultiplayerPage : std::uint8_t {
  entry,
  join,
  create,
  lobby,
};

enum class MultiplayerEntryChoice : std::uint8_t {
  join,
  create,
};

enum class MultiplayerJoinField : std::uint8_t {
  sessions,
  address,
  password,
  refresh,
};

enum class MultiplayerCreateField : std::uint8_t {
  start,
  session_name,
  session_password,
  protocol,
};

enum class MultiplayerLobbyField : std::uint8_t {
  ready,
  start,
};

struct MultiplayerSessionEntry {
  std::string name;
  std::string address;
  std::uint8_t players = 0U;
  std::uint8_t capacity = 8U;
  bool password_required = false;
};

struct MultiplayerLobbyPlayer {
  std::uint8_t peer = 0U;
  std::string name;
  std::uint8_t car_selection = 0U;
  bool ready = false;
  bool host = false;
  bool local = false;
  std::string team;
  bool automatic_gear = true;
  std::string car_name;
  std::uint32_t colour = 0x00e06820U;
  std::uint32_t races = 0U;
  std::uint32_t score = 0U;
  std::uint8_t grid_position = 0U;
  std::uint8_t lap = 0U;
  std::uint32_t ping_ms = 0U;
};

struct MultiplayerChatLine {
  std::string text;
  std::uint32_t colour = 0x00e06820U;
};

struct MultiplayerConfiguration {
  std::string session_name = "Motorhead Session";
  std::string session_password;
  std::string protocol = "TCP/IP";
  std::string address = "127.0.0.1";
  std::vector<MultiplayerSessionEntry> sessions;
  std::uint32_t selected_session = 0U;
  std::vector<MultiplayerLobbyPlayer> lobby_players;
  std::vector<MultiplayerChatLine> lobby_chat_lines;
  std::string status;
  std::string lobby_chat_input;
  bool lobby_host = false;
  bool local_ready = false;
  std::string lobby_track = "Goldbridge";
  std::uint32_t lobby_laps = 5U;
  std::string lobby_game_mode = "Normal";
  std::string lobby_car;
  std::string lobby_gear = "Automatic";
};

// The common p3.1 requester stores a zero-based option index. The main-menu
// Escape requester passes default option 2, so "No way!" is selected when it
// first opens even though "Yes, please" is the first displayed choice.
enum class ExitConfirmationChoice : std::uint8_t {
  yes,
  no,
};

// p3.1 One Player handler RVA 0x8a3f0 and MENU.SPR names
// tquic/tsing/tleag/tatta/tghos establish this authored order.
enum class OnePlayerChoice : std::uint8_t {
  quick_race,
  single_race,
  league_race,
  time_attack,
  ghost_mode,
};

// p3.1 League hub handler RVA 0x8a8b8 and MENU.SPR names
// tlcon/tlvst/tlloa/tlnew/tldel establish this authored order.
enum class LeagueMenuChoice : std::uint8_t {
  continue_race,
  view_stats,
  select_league,
  new_league,
  delete_league,
};

enum class LeagueCreateField : std::uint8_t {
  name,
  division,
};

// p3.1 bank-19 handler RVA 0x80a98. The fourth internal selector value (8)
// is the demo-file list focus and is represented separately by the state.
enum class GhostModeChoice : std::uint8_t {
  ghost_race,
  replay_race,
  benchmark_race,
};

enum class OptionsMenuChoice : std::uint8_t {
  graphic_options,
  sound_options,
  gameplay_options,
  control_options,
  personal_options,
  difficulty,
};

enum class DifficultyChoice : std::uint8_t {
  easy,
  medium,
  hard,
};

// p3.1 Rankings handler RVA 0x84b4c. The five internal selector slots are
// Type, Track/Division, Laps, Difficulty, and road orientation.
enum class RankingsField : std::uint8_t {
  type,
  track_or_division,
  laps,
  difficulty,
  orientation,
};

enum class RankingsType : std::uint8_t {
  best_laptime,
  best_racetime,
  single_race,
  league,
};

struct RankingsConfiguration {
  RankingsType type = RankingsType::single_race;
  std::uint32_t track_index = 0U;
  std::uint32_t track_count = 1U;
  std::uint32_t division_index = 0U;
  std::uint32_t laps_index = 2U; // 1, 3, 5, 10, 15, 25
  DifficultyChoice difficulty = DifficultyChoice::medium;
  bool mirror = false;
};

// p3.1 Graphic Options handler RVA 0x87330. DetailMode values other than
// Custom expose the retail rows. The owned renderer type exposes 13
// additional Custom rows; its mutually exclusive perspective rows belong to
// the other original renderer path. Aspect ratio and window mode are
// reconstruction-owned extensions placed directly after Screen size.
enum class GraphicOptionsField : std::uint8_t {
  render_device,
  screen_size,
  aspect_ratio,
  window_mode,
  true_colour,
  triple_buffer,
  trilinear_filtering,
  texture_format,
  brightness,
  graphic_detail,
  lens_flares,
  sparks,
  smoke,
  halos,
  shadows,
  skid_marks,
  name_plates,
  background,
  track_detail,
  car_detail,
  car_shading,
  view_distance,
  z_read,
  motion_blur,
};

enum class GameplayOptionsField : std::uint8_t {
  info_detail,
  info_map,
  checkpoint_info,
  checkpoint_display_time,
  camera_shake,
  ui_scale,
};

enum class GraphicScreenSize : std::uint8_t {
  size_640x480,
  size_800x600,
  size_1024x768,
  size_1280x1024,
  size_1600x1200,
  size_960x540,
  size_1280x720,
  size_1360x768,
  size_1600x900,
  size_1920x1080,
};

enum class GraphicAspectRatio : std::uint8_t {
  classic_4_3,
  widescreen_16_9,
};

[[nodiscard]] GraphicAspectRatio
graphic_aspect_ratio_from_config(std::string_view value) noexcept;
[[nodiscard]] std::string_view
graphic_aspect_ratio_config_name(GraphicAspectRatio aspect_ratio) noexcept;
[[nodiscard]] GraphicAspectRatio
graphic_screen_aspect_ratio(GraphicScreenSize size) noexcept;
[[nodiscard]] std::size_t
graphic_screen_size_tier(GraphicScreenSize size) noexcept;
[[nodiscard]] GraphicScreenSize
graphic_screen_size_for_aspect(GraphicAspectRatio aspect_ratio,
                               std::size_t tier) noexcept;

// The retail configuration calls its two renderer drivers `display` and
// `cliff`. The reconstruction presents their actual execution paths by name.
enum class GraphicRendererBackend : std::uint8_t {
  automatic,
  d3d9,
  d3d11,
  d3d12,
  glide,
  software,
};

[[nodiscard]] GraphicRendererBackend
graphic_renderer_backend_from_config(std::string_view value) noexcept;
[[nodiscard]] std::string_view
graphic_renderer_backend_config_name(GraphicRendererBackend backend) noexcept;
[[nodiscard]] std::string_view
graphic_renderer_backend_label(GraphicRendererBackend backend) noexcept;

// Reconstruction window ownership. The retail Graphic Options screen has no
// authored row for it, so the reconstruction exposes it as an adjacent row.
enum class GraphicWindowMode : std::uint8_t {
  windowed,
  borderless,
  fullscreen,
};

[[nodiscard]] GraphicWindowMode
graphic_window_mode_from_config(std::string_view value) noexcept;
[[nodiscard]] std::string_view
graphic_window_mode_config_name(GraphicWindowMode mode) noexcept;

std::array<std::uint32_t, 2U>
graphic_screen_dimensions(GraphicScreenSize size) noexcept;

enum class GraphicTextureFormat : std::uint8_t {
  indexed_256,
  hi_colour,
  true_colour,
};

enum class GraphicInfoMode : std::uint8_t {
  none,
  selective,
  all,
};

enum class GraphicDetailMode : std::uint8_t {
  low,
  medium,
  high,
  maximum,
  custom,
};

enum class GraphicNamePlateMode : std::uint8_t {
  none,
  flat,
  transparent,
};

enum class GraphicTrackDetail : std::uint8_t {
  medium,
  high,
};

enum class GraphicCarDetail : std::uint8_t {
  low,
  medium,
  high,
};

enum class GraphicCarShading : std::uint8_t {
  flat,
  gouraud,
  reflection,
  glenz,
};

struct GraphicOptionsConfiguration {
  GraphicRendererBackend renderer_backend = GraphicRendererBackend::automatic;
  GraphicScreenSize screen_size = GraphicScreenSize::size_800x600;
  GraphicAspectRatio aspect_ratio = GraphicAspectRatio::classic_4_3;
  GraphicWindowMode window_mode = GraphicWindowMode::windowed;
  bool true_colour = false;
  bool triple_buffer = false;
  bool trilinear_filtering = false;
  GraphicTextureFormat texture_format = GraphicTextureFormat::indexed_256;
  float brightness = 1.0F;
  GraphicInfoMode info_detail = GraphicInfoMode::all;
  GraphicInfoMode info_map = GraphicInfoMode::all;
  GraphicInfoMode checkpoint_info = GraphicInfoMode::all;
  std::uint32_t checkpoint_display_time_ms = 4000U;
  GraphicDetailMode detail_mode = GraphicDetailMode::high;
  bool lens_flares = true;
  bool sparks = true;
  bool smoke = true;
  bool halos = true;
  bool shadows = true;
  bool skid_marks = true;
  GraphicNamePlateMode name_plates = GraphicNamePlateMode::transparent;
  bool background = true;
  GraphicTrackDetail track_detail = GraphicTrackDetail::high;
  GraphicCarDetail car_detail = GraphicCarDetail::high;
  GraphicCarShading car_shading = GraphicCarShading::reflection;
  float view_distance = 80.0F;
  bool z_read = false;
  bool motion_blur = false;
  bool camera_shake = true;
  float ui_scale = 100.0F;
};

// The retail DetailMode selector represents complete renderer profiles.  The
// Custom value leaves the individual fields untouched; every other value
// resolves them to one deterministic set before the race handoff.
void apply_graphic_detail_preset(GraphicOptionsConfiguration &configuration,
                                 GraphicDetailMode mode) noexcept;

// Preserve the p3.1 renderer-creation choices at the front-end/race boundary.
// The playable renderer owns their backend interpretation; keeping the values
// explicit prevents unsupported SDL approximations from being applied here.
[[nodiscard]] std::vector<std::string> playable_graphic_renderer_arguments(
    const GraphicOptionsConfiguration &configuration);

// p3.1 Personal Options renderer RVA 0x85304 and input owner RVA
// 0x85754 expose 15 rows unconditionally. Enabling CustomCarColours extends
// the selector to the three authored car-colour rows.
enum class PersonalOptionsField : std::uint8_t {
  name,
  team,
  colour,
  system,
  short_key_0,
  short_key_1,
  short_key_2,
  short_key_3,
  short_key_4,
  short_key_5,
  short_key_6,
  short_key_7,
  short_key_8,
  short_key_9,
  car_colours,
  car_colour_1,
  car_colour_2,
  car_colour_3,
};

enum class MeasurementSystem : std::uint8_t {
  metric,
  imperial,
};

struct PersonalColour {
  std::uint8_t red = 255U;
  std::uint8_t green = 255U;
  std::uint8_t blue = 255U;

  friend bool operator==(const PersonalColour &,
                         const PersonalColour &) = default;
};

struct PersonalOptionsConfiguration {
  std::string player_name = "Player";
  std::string team_name;
  PersonalColour player_colour{};
  MeasurementSystem measurement_system = MeasurementSystem::metric;
  std::array<std::string, 10U> short_keys{};
  bool custom_car_colours = false;
  std::array<PersonalColour, 3U> car_colours{{
      {32U, 16U, 64U},
      {64U, 16U, 32U},
      {208U, 208U, 208U},
  }};
};

// p3.1 Control Options input owner RVA 0x887f4..0x88f97 switches between
// a control-layout list and fourteen authored setting rows.
enum class ControlOptionsFocus : std::uint8_t {
  profile_list,
  field_list,
};

enum class ControlOptionsField : std::uint8_t {
  force_feedback,
  mouse_speed_x,
  mouse_speed_y,
  mouse_speed_z,
  turn_left,
  turn_right,
  accelerate,
  brake,
  gear_up,
  gear_down,
  horn,
  hand_brake,
  rear_view,
  game_menu,
};

struct ControlProfileConfiguration {
  std::string name = "Keyboard";
  bool force_feedback = false;
  std::array<float, 3U> mouse_speeds{1.0F, 1.0F, 1.0F};
  std::array<std::string, 10U> bindings{{
      "LeftArrow",
      "RightArrow",
      "UpArrow",
      "DownArrow",
      "S",
      "A",
      "Space",
      "Shift",
      "Ctrl",
      "Esc",
  }};
  // The retail CLO stores these below the menu-editable rows. Replay Race
  // still consumes them exactly as authored by the selected layout.
  std::array<std::string, 4U> fixed_race_bindings{{"F1", "F2", "F3", "F4"}};
};

struct ControlOptionsConfiguration {
  std::vector<ControlProfileConfiguration> profiles{
      ControlProfileConfiguration{"Custom"},
      ControlProfileConfiguration{"Joystick"},
      ControlProfileConfiguration{"Keyboard"},
      ControlProfileConfiguration{"Mouse"},
      ControlProfileConfiguration{"SteeringWheel"},
  };
  std::uint32_t profile_index = 2U;
};

// p3.1 Sound Options handler RVA 0x89744. The selector owns five positions:
// sound device, the two 0..255 volume gauges, CD looping, and the CD-track
// assignment for the selected song row.
enum class SoundOptionsField : std::uint8_t {
  sound_device,
  sound_effects_volume,
  cd_music_volume,
  cd_loop,
  cd_track,
};

struct SoundOptionsConfiguration {
  std::vector<std::string> output_devices{std::string{}};
  std::uint32_t output_device_index = 0U;
  [[nodiscard]] std::string_view output_device_name() const noexcept {
    return output_device_index < output_devices.size()
               ? std::string_view(output_devices[output_device_index])
               : std::string_view{};
  }
  // The owned p3.1 motorhead.cfg stores 99.61% (254/255). The reconstruction
  // intentionally starts a clean profile at gauge level 5 so the owner can
  // evaluate equal headroom in either direction; subsequent choices persist.
  std::uint8_t sound_effects_volume = 128U;
  std::uint8_t cd_music_volume = 128U;
  bool cd_loop = false;
  std::uint32_t song_index = 0U;
  std::uint8_t first_cd_track = 2U;
  std::uint8_t last_cd_track = 11U;
  // Results (02) and Menu (03) are visible/previewable but immutable. Race
  // assignments follow and remain editable.
  std::uint32_t fixed_cd_track_count = 2U;
  std::vector<std::uint8_t> assigned_cd_tracks{2U, 3U, 11U, 4U};
};

// p3.1 bank-2 descriptor (VA 0x004f9f18) is shared by Single Race and
// Time Attack. Time Attack suppresses the catch-up row.
enum class RaceSetupMode : std::uint8_t {
  single_race,
  time_attack,
  league_race,
  ghost_race,
};

enum class RaceSetupField : std::uint8_t {
  track,
  laps,
  catch_up,
};

struct RaceSetupConfiguration {
  RaceSetupMode mode = RaceSetupMode::single_race;
  std::uint32_t track_index = 0U;
  std::uint32_t track_count = 1U;
  std::uint32_t unlocked_track_count = 1U;
  std::uint32_t laps = 5U;
  bool catch_up = false;
};

enum class CarSetupField : std::uint8_t {
  car,
  transmission,
  horn,
  record_race,
};

struct CarSetupConfiguration {
  std::uint32_t car_index = 0U;
  std::uint32_t car_count = 1U;
  std::uint32_t unlocked_car_count = 1U;
  bool automatic_transmission = true;
  std::uint32_t horn_index = 0U;
  std::uint32_t horn_count = 1U;
  bool record_race = false;
};

enum class MenuDirection : std::uint8_t {
  left,
  right,
  up,
  down,
};

// Graphic Options and the nested in-race GFX page share the same held
// ViewDistance owner. Values move in 5% steps within the retail 30% through
// 150% range; longer held-input frames may consume multiple steps.
[[nodiscard]] float
adjust_original_view_distance(float current, MenuDirection direction,
                              std::uint32_t elapsed_ms) noexcept;

enum class MenuTransitionEvidence : std::uint8_t {
  confirmed_change,
  confirmed_no_change,
  unknown,
};

enum class MenuSoundEvent : std::uint8_t {
  escape = 1U,
  requester_open = 2U,
  page_out = 3U,
  arrow_left = 4U,
  arrow_right = 5U,
  arrow_error = 6U,
  select = 11U,
  difficulty_change = 10U,
  select_left = 12U,
  select_right = 13U,
  binding_capture = 14U,
  select_error = 15U,
  volume_right = 16U,
  volume_left = 17U,
  volume_error = 18U,
  field_up = 7U,
  field_down = 8U,
  field_error = 9U,
  catch_up_on = 19U,
  catch_up_off = 20U,
  catch_up_error = 21U,
  laps_right = 22U,
  laps_right_error = 23U,
  laps_left = 24U,
  laps_left_error = 25U,
  track_locked = 37U,
  car_locked = 36U,
  field_left = 26U,
  field_right = 27U,
  field_navigation_error = 28U,
  binding_assigned = 29U,
  binding_swapped = 30U,
  binding_reserved = 31U,
};

// Values preserve the p3.1 front-end output contract at global 0x007eaf44.
// Quick Race's terminal callback at RVA 0x80618 writes 2.
enum class FrontEndAction : std::uint8_t {
  none = 0U,
  exit_application = 1U,
  start_race = 2U,
  quick_race = start_race,
};

enum class RaceResultsAction : std::uint8_t {
  wait,
  return_to_front_end,
};

// Both ScreenResults (RVA 0x40bc7) and the league-result screen
// (RVA 0x4125f) leave their display loop on DIK_Escape or DIK_Return.
[[nodiscard]] constexpr RaceResultsAction
race_results_action(const bool escape_pressed,
                    const bool return_pressed) noexcept {
  return escape_pressed || return_pressed
             ? RaceResultsAction::return_to_front_end
             : RaceResultsAction::wait;
}

struct MenuTransitionResult {
  MenuTransitionEvidence evidence = MenuTransitionEvidence::unknown;
  bool selection_changed = false;
  bool screen_changed = false;
  FrontEndAction action = FrontEndAction::none;
  std::array<MenuSoundEvent, 3U> sound_events{};
  std::uint8_t sound_event_count = 0U;
  bool preview_horn = false;
  bool sound_settings_changed = false;
  bool personal_settings_changed = false;
  bool control_settings_changed = false;
  bool graphic_settings_changed = false;
  bool capture_control_binding = false;
  bool preview_music = false;
  bool create_league = false;
  bool delete_league = false;
  bool multiplayer_refresh = false;
  bool multiplayer_join = false;
  bool multiplayer_host = false;
  bool multiplayer_ready = false;
  bool multiplayer_start = false;
  bool multiplayer_leave = false;
};

// Evidence-backed p3.1 front-end state. Unknown edges deliberately do not
// mutate state: callers must not silently invent unobserved menu behavior.
class FrontEndState {
public:
  [[nodiscard]] FrontEndScreen screen() const noexcept;
  [[nodiscard]] MainMenuChoice main_choice() const noexcept;
  [[nodiscard]] MultiplayerPage multiplayer_page() const noexcept;
  [[nodiscard]] MultiplayerEntryChoice
  multiplayer_entry_choice() const noexcept;
  [[nodiscard]] MultiplayerJoinField multiplayer_join_field() const noexcept;
  [[nodiscard]] MultiplayerCreateField
  multiplayer_create_field() const noexcept;
  [[nodiscard]] MultiplayerLobbyField multiplayer_lobby_field() const noexcept;
  [[nodiscard]] const MultiplayerConfiguration &
  multiplayer_configuration() const noexcept;
  void set_multiplayer_sessions(std::vector<MultiplayerSessionEntry> sessions);
  void prompt_multiplayer_password();
  void enter_multiplayer_lobby(bool host);
  void
  set_multiplayer_lobby_players(std::vector<MultiplayerLobbyPlayer> players,
                                bool local_ready);
  void set_multiplayer_status(std::string status);
  void add_multiplayer_chat_line(std::string text, std::uint32_t colour);
  void clear_multiplayer_chat_input() noexcept;
  void set_multiplayer_lobby_configuration(std::string track,
                                           std::uint32_t laps, std::string car,
                                           std::string gear,
                                           std::string game_mode = "Normal");
  void leave_multiplayer_lobby() noexcept;
  [[nodiscard]] bool exit_confirmation_open() const noexcept;
  [[nodiscard]] ExitConfirmationChoice
  exit_confirmation_choice() const noexcept;
  [[nodiscard]] bool league_create_confirmation_open() const noexcept;
  [[nodiscard]] ExitConfirmationChoice
  league_create_confirmation_choice() const noexcept;
  [[nodiscard]] bool league_delete_confirmation_open() const noexcept;
  [[nodiscard]] ExitConfirmationChoice
  league_delete_confirmation_choice() const noexcept;
  [[nodiscard]] OnePlayerChoice one_player_choice() const noexcept;
  [[nodiscard]] GhostModeChoice ghost_mode_choice() const noexcept;
  [[nodiscard]] bool ghost_file_focused() const noexcept;
  [[nodiscard]] std::uint32_t ghost_file_index() const noexcept;
  [[nodiscard]] std::uint32_t ghost_file_count() const noexcept;
  [[nodiscard]] LeagueMenuChoice league_menu_choice() const noexcept;
  [[nodiscard]] std::uint32_t league_index() const noexcept;
  [[nodiscard]] std::uint32_t league_count() const noexcept;
  [[nodiscard]] LeagueCreateField league_create_field() const noexcept;
  [[nodiscard]] std::uint32_t league_create_division_index() const noexcept;
  [[nodiscard]] std::uint32_t league_create_division_count() const noexcept;
  [[nodiscard]] OptionsMenuChoice options_choice() const noexcept;
  [[nodiscard]] DifficultyChoice difficulty() const noexcept;
  [[nodiscard]] RankingsField rankings_field() const noexcept;
  [[nodiscard]] const RankingsConfiguration &
  rankings_configuration() const noexcept;
  [[nodiscard]] GraphicOptionsField graphic_options_field() const noexcept;
  [[nodiscard]] GameplayOptionsField gameplay_options_field() const noexcept;
  [[nodiscard]] const GraphicOptionsConfiguration &
  graphic_options_configuration() const noexcept;
  [[nodiscard]] PersonalOptionsField personal_options_field() const noexcept;
  [[nodiscard]] const PersonalOptionsConfiguration &
  personal_options_configuration() const noexcept;
  [[nodiscard]] ControlOptionsFocus control_options_focus() const noexcept;
  [[nodiscard]] ControlOptionsField control_options_field() const noexcept;
  [[nodiscard]] const ControlOptionsConfiguration &
  control_options_configuration() const noexcept;
  [[nodiscard]] bool control_binding_capture_active() const noexcept;
  [[nodiscard]] SoundOptionsField sound_options_field() const noexcept;
  [[nodiscard]] const SoundOptionsConfiguration &
  sound_options_configuration() const noexcept;
  [[nodiscard]] RaceSetupMode race_setup_mode() const noexcept;
  [[nodiscard]] RaceSetupField race_setup_field() const noexcept;
  [[nodiscard]] std::uint32_t race_setup_track_index() const noexcept;
  [[nodiscard]] std::uint32_t race_setup_track_count() const noexcept;
  [[nodiscard]] std::uint32_t race_setup_unlocked_track_count() const noexcept;
  [[nodiscard]] std::uint32_t race_setup_laps() const noexcept;
  [[nodiscard]] bool race_setup_catch_up() const noexcept;
  [[nodiscard]] CarSetupField car_setup_field() const noexcept;
  [[nodiscard]] std::uint32_t car_setup_car_index() const noexcept;
  [[nodiscard]] std::uint32_t car_setup_car_count() const noexcept;
  [[nodiscard]] std::uint32_t car_setup_unlocked_car_count() const noexcept;
  [[nodiscard]] bool car_setup_automatic_transmission() const noexcept;
  [[nodiscard]] std::uint32_t car_setup_horn_index() const noexcept;
  [[nodiscard]] std::uint32_t car_setup_horn_count() const noexcept;
  [[nodiscard]] bool car_setup_record_race() const noexcept;

  void configure_race_setup(RaceSetupConfiguration configuration) noexcept;
  void configure_car_setup(CarSetupConfiguration configuration) noexcept;
  void configure_rankings(std::uint32_t track_count) noexcept;
  void
  configure_graphic_options(GraphicOptionsConfiguration configuration) noexcept;
  void configure_personal_options(PersonalOptionsConfiguration configuration);
  void configure_control_options(ControlOptionsConfiguration configuration);
  void configure_sound_options(SoundOptionsConfiguration configuration);
  void configure_ghost_files(std::uint32_t current_index,
                             std::uint32_t file_count) noexcept;
  void configure_one_player_league_available(bool available) noexcept;
  [[nodiscard]] MenuTransitionResult delete_current_ghost_file() noexcept;
  void configure_leagues(std::uint32_t current_index,
                         std::uint32_t league_count) noexcept;
  void configure_league_create_divisions(std::uint32_t current_index,
                                         std::uint32_t division_count) noexcept;
  void show_main_menu_first_choice() noexcept;

  [[nodiscard]] MenuTransitionResult navigate(MenuDirection direction) noexcept;
  [[nodiscard]] MenuTransitionResult confirm() noexcept;
  [[nodiscard]] MenuTransitionResult cancel() noexcept;
  [[nodiscard]] bool append_personal_text(std::string_view text);
  [[nodiscard]] bool backspace_personal_text() noexcept;
  [[nodiscard]] bool append_multiplayer_text(std::string_view text);
  [[nodiscard]] bool backspace_multiplayer_text() noexcept;
  [[nodiscard]] MenuTransitionResult bind_control_input(std::string_view input);
  [[nodiscard]] MenuTransitionResult
  adjust_control_analog(MenuDirection direction,
                        std::uint32_t elapsed_ms) noexcept;
  [[nodiscard]] MenuTransitionResult
  adjust_graphic_held(MenuDirection direction,
                      std::uint32_t elapsed_ms) noexcept;
  [[nodiscard]] MenuTransitionResult
  adjust_gameplay_held(MenuDirection direction,
                       std::uint32_t elapsed_ms) noexcept;

private:
  FrontEndScreen screen_ = FrontEndScreen::main_menu;
  MainMenuChoice main_choice_ = MainMenuChoice::one_player;
  MultiplayerPage multiplayer_page_ = MultiplayerPage::entry;
  MultiplayerEntryChoice multiplayer_entry_choice_ =
      MultiplayerEntryChoice::join;
  MultiplayerJoinField multiplayer_join_field_ = MultiplayerJoinField::sessions;
  MultiplayerCreateField multiplayer_create_field_ =
      MultiplayerCreateField::start;
  MultiplayerLobbyField multiplayer_lobby_field_ = MultiplayerLobbyField::ready;
  MultiplayerConfiguration multiplayer_{};
  bool exit_confirmation_open_ = false;
  ExitConfirmationChoice exit_confirmation_choice_ = ExitConfirmationChoice::no;
  bool league_create_confirmation_open_ = false;
  ExitConfirmationChoice league_create_confirmation_choice_ =
      ExitConfirmationChoice::yes;
  bool league_delete_confirmation_open_ = false;
  ExitConfirmationChoice league_delete_confirmation_choice_ =
      ExitConfirmationChoice::no;
  OnePlayerChoice one_player_choice_ = OnePlayerChoice::quick_race;
  bool one_player_league_available_ = true;
  GhostModeChoice ghost_mode_choice_ = GhostModeChoice::ghost_race;
  bool ghost_file_focused_ = false;
  std::uint32_t ghost_file_index_ = 0U;
  std::uint32_t ghost_file_count_ = 0U;
  LeagueMenuChoice league_menu_choice_ = LeagueMenuChoice::continue_race;
  std::uint32_t league_index_ = 0U;
  std::uint32_t league_count_ = 1U;
  LeagueCreateField league_create_field_ = LeagueCreateField::name;
  std::uint32_t league_create_division_index_ = 0U;
  std::uint32_t league_create_division_count_ = 1U;
  OptionsMenuChoice options_choice_ = OptionsMenuChoice::graphic_options;
  DifficultyChoice difficulty_ = DifficultyChoice::medium;
  RankingsField rankings_field_ = RankingsField::type;
  RankingsConfiguration rankings_{};
  GraphicOptionsField graphic_options_field_ =
      GraphicOptionsField::render_device;
  GameplayOptionsField gameplay_options_field_ =
      GameplayOptionsField::info_detail;
  GraphicOptionsConfiguration graphic_options_{};
  PersonalOptionsField personal_options_field_ = PersonalOptionsField::name;
  PersonalOptionsConfiguration personal_options_{};
  ControlOptionsFocus control_options_focus_ =
      ControlOptionsFocus::profile_list;
  ControlOptionsField control_options_field_ =
      ControlOptionsField::force_feedback;
  ControlOptionsConfiguration control_options_{};
  bool control_binding_capture_active_ = false;
  SoundOptionsField sound_options_field_ = SoundOptionsField::sound_device;
  SoundOptionsConfiguration sound_options_{};
  RaceSetupConfiguration race_setup_{};
  RaceSetupField race_setup_field_ = RaceSetupField::track;
  CarSetupConfiguration car_setup_{};
  CarSetupField car_setup_field_ = CarSetupField::car;
};

[[nodiscard]] std::string_view
main_menu_choice_name(MainMenuChoice choice) noexcept;
[[nodiscard]] std::string_view
one_player_choice_name(OnePlayerChoice choice) noexcept;
[[nodiscard]] std::string_view
league_menu_choice_name(LeagueMenuChoice choice) noexcept;
[[nodiscard]] std::string_view
options_menu_choice_name(OptionsMenuChoice choice) noexcept;

// Angular units authored by p3.1 sprpos.dta bank 0 records 24..28. The
// renderer converts these to radians with pi/2048.
[[nodiscard]] float
main_menu_pointer_target_units(MainMenuChoice choice) noexcept;
[[nodiscard]] float
one_player_pointer_target_units(OnePlayerChoice choice) noexcept;
[[nodiscard]] float
league_menu_pointer_target_units(LeagueMenuChoice choice) noexcept;
[[nodiscard]] float
options_menu_pointer_target_units(OptionsMenuChoice choice) noexcept;

// Reproduces the p3.1 pointer easing update performed once per rendered frame.
[[nodiscard]] float
advance_main_menu_pointer_units(float current_units,
                                MainMenuChoice target_choice,
                                std::uint32_t elapsed_ms) noexcept;
[[nodiscard]] float
advance_one_player_pointer_units(float current_units,
                                 OnePlayerChoice target_choice,
                                 std::uint32_t elapsed_ms) noexcept;
[[nodiscard]] float
advance_league_menu_pointer_units(float current_units,
                                  LeagueMenuChoice target_choice,
                                  std::uint32_t elapsed_ms) noexcept;
[[nodiscard]] float
advance_options_menu_pointer_units(float current_units,
                                   OptionsMenuChoice target_choice,
                                   std::uint32_t elapsed_ms) noexcept;

} // namespace mh::ui
