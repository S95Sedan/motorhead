#include <ui/frontend/controller.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace mh::ui {
namespace {

MenuTransitionResult result_with_sound(const MenuTransitionEvidence evidence,
                                       const bool selection_changed,
                                       const bool screen_changed,
                                       const MenuSoundEvent first) noexcept {
  MenuTransitionResult result{evidence, selection_changed, screen_changed,
                              FrontEndAction::none};
  result.sound_events[0U] = first;
  result.sound_event_count = 1U;
  return result;
}

MenuTransitionResult result_with_sounds(const MenuTransitionEvidence evidence,
                                        const bool selection_changed,
                                        const bool screen_changed,
                                        const MenuSoundEvent first,
                                        const MenuSoundEvent second) noexcept {
  auto result =
      result_with_sound(evidence, selection_changed, screen_changed, first);
  result.sound_events[1U] = second;
  result.sound_event_count = 2U;
  return result;
}

MenuTransitionResult result_with_action(const FrontEndAction action,
                                        const MenuSoundEvent first,
                                        const MenuSoundEvent second) noexcept {
  MenuTransitionResult result{MenuTransitionEvidence::confirmed_change, false,
                              false, action};
  result.sound_events[0U] = first;
  result.sound_events[1U] = second;
  result.sound_event_count = 2U;
  return result;
}

MenuTransitionResult result_with_three_sounds(
    const MenuTransitionEvidence evidence, const bool selection_changed,
    const bool screen_changed, const MenuSoundEvent first,
    const MenuSoundEvent second, const MenuSoundEvent third) noexcept {
  auto result = result_with_sounds(evidence, selection_changed, screen_changed,
                                   first, second);
  result.sound_events[2U] = third;
  result.sound_event_count = 3U;
  return result;
}

constexpr std::array<std::uint32_t, 6U> race_lap_values{1U,  3U,  5U,
                                                        10U, 15U, 25U};

constexpr std::array<PersonalColour, 32U> personal_colour_palette{{
    {220U, 220U, 220U}, {174U, 174U, 174U}, {92U, 92U, 92U},   {10U, 10U, 10U},
    {20U, 200U, 220U},  {17U, 131U, 209U},  {14U, 65U, 163U},  {10U, 20U, 116U},
    {14U, 200U, 47U},   {14U, 160U, 30U},   {13U, 120U, 19U},  {10U, 80U, 10U},
    {240U, 100U, 10U},  {220U, 24U, 10U},   {138U, 15U, 14U},  {70U, 10U, 12U},
    {230U, 210U, 20U},  {209U, 164U, 24U},  {163U, 104U, 17U}, {117U, 57U, 10U},
    {220U, 170U, 203U}, {195U, 91U, 140U},  {142U, 33U, 91U},  {79U, 10U, 76U},
    {10U, 220U, 205U},  {14U, 193U, 173U},  {13U, 131U, 131U}, {10U, 61U, 69U},
    {195U, 173U, 148U}, {149U, 125U, 102U}, {103U, 82U, 63U},  {56U, 41U, 29U},
}};

std::size_t nearest_personal_colour(const PersonalColour colour) noexcept {
  auto best = std::size_t{0U};
  auto best_distance = std::numeric_limits<std::uint32_t>::max();
  for (std::size_t index = 0U; index < personal_colour_palette.size();
       ++index) {
    const auto &candidate = personal_colour_palette[index];
    const auto red = static_cast<std::int32_t>(colour.red) - candidate.red;
    const auto green =
        static_cast<std::int32_t>(colour.green) - candidate.green;
    const auto blue = static_cast<std::int32_t>(colour.blue) - candidate.blue;
    const auto distance =
        static_cast<std::uint32_t>(red * red + green * green + blue * blue);
    if (distance < best_distance) {
      best = index;
      best_distance = distance;
    }
  }
  return best;
}

void step_personal_colour(PersonalColour &colour,
                          const MenuDirection direction) noexcept {
  auto index = nearest_personal_colour(colour);
  if (direction == MenuDirection::left) {
    index = index == 0U ? personal_colour_palette.size() - 1U : index - 1U;
  } else {
    index = (index + 1U) % personal_colour_palette.size();
  }
  colour = personal_colour_palette[index];
}

std::size_t race_lap_index(const std::uint32_t laps) noexcept {
  const auto found =
      std::find(race_lap_values.begin(), race_lap_values.end(), laps);
  return found == race_lap_values.end()
             ? 2U
             : static_cast<std::size_t>(
                   std::distance(race_lap_values.begin(), found));
}

int adjust_five_percent_index(const int current_percent,
                              const MenuDirection direction,
                              const int step_count) noexcept {
  constexpr int step = 5;
  if (direction == MenuDirection::left) {
    const auto ceiling_index = (current_percent + step - 1) / step;
    return (ceiling_index - step_count) * step;
  }
  if (direction == MenuDirection::right) {
    const auto floor_index = current_percent / step;
    return (floor_index + step_count) * step;
  }
  return current_percent;
}

} // namespace

std::array<std::uint32_t, 2U>
graphic_screen_dimensions(const GraphicScreenSize size) noexcept {
  switch (size) {
  case GraphicScreenSize::size_640x480:
    return {640U, 480U};
  case GraphicScreenSize::size_800x600:
    return {800U, 600U};
  case GraphicScreenSize::size_1024x768:
    return {1024U, 768U};
  case GraphicScreenSize::size_1280x1024:
    return {1280U, 1024U};
  case GraphicScreenSize::size_1600x1200:
    return {1600U, 1200U};
  case GraphicScreenSize::size_960x540:
    return {960U, 540U};
  case GraphicScreenSize::size_1280x720:
    return {1280U, 720U};
  case GraphicScreenSize::size_1360x768:
    return {1360U, 768U};
  case GraphicScreenSize::size_1600x900:
    return {1600U, 900U};
  case GraphicScreenSize::size_1920x1080:
    return {1920U, 1080U};
  }
  return {800U, 600U};
}

GraphicAspectRatio
graphic_aspect_ratio_from_config(const std::string_view value) noexcept {
  const auto equals = [value](const std::string_view expected) {
    return value.size() == expected.size() &&
           std::equal(value.begin(), value.end(), expected.begin(),
                      [](const char left, const char right) {
                        return std::tolower(static_cast<unsigned char>(left)) ==
                               std::tolower(static_cast<unsigned char>(right));
                      });
  };
  return equals("16:9") || equals("16x9") || equals("widescreen")
             ? GraphicAspectRatio::widescreen_16_9
             : GraphicAspectRatio::classic_4_3;
}

std::string_view graphic_aspect_ratio_config_name(
    const GraphicAspectRatio aspect_ratio) noexcept {
  return aspect_ratio == GraphicAspectRatio::widescreen_16_9 ? "16:9" : "4:3";
}

GraphicAspectRatio
graphic_screen_aspect_ratio(const GraphicScreenSize size) noexcept {
  return static_cast<std::uint8_t>(size) >=
                 static_cast<std::uint8_t>(GraphicScreenSize::size_960x540)
             ? GraphicAspectRatio::widescreen_16_9
             : GraphicAspectRatio::classic_4_3;
}

std::size_t graphic_screen_size_tier(const GraphicScreenSize size) noexcept {
  const auto raw = static_cast<std::size_t>(size);
  constexpr auto family_size = std::size_t{5U};
  return raw >= family_size ? raw - family_size : raw;
}

GraphicScreenSize
graphic_screen_size_for_aspect(const GraphicAspectRatio aspect_ratio,
                               const std::size_t tier) noexcept {
  constexpr auto family_size = std::size_t{5U};
  const auto bounded_tier = std::min(tier, family_size - 1U);
  const auto family_offset =
      aspect_ratio == GraphicAspectRatio::widescreen_16_9 ? family_size : 0U;
  return static_cast<GraphicScreenSize>(family_offset + bounded_tier);
}

GraphicRendererBackend
graphic_renderer_backend_from_config(const std::string_view value) noexcept {
  const auto equals = [value](const std::string_view expected) {
    return value.size() == expected.size() &&
           std::equal(value.begin(), value.end(), expected.begin(),
                      [](const char left, const char right) {
                        return std::tolower(static_cast<unsigned char>(left)) ==
                               std::tolower(static_cast<unsigned char>(right));
                      });
  };
  if (equals("auto") || equals("automatic") || equals("display")) {
    return GraphicRendererBackend::automatic;
  }
  if (equals("cliff") || equals("software")) {
    return GraphicRendererBackend::software;
  }
  if (equals("glide") || equals("3dfx")) {
    return GraphicRendererBackend::glide;
  }
  if (equals("d3d9") || equals("direct3d9")) {
    return GraphicRendererBackend::d3d9;
  }
  if (equals("d3d11") || equals("direct3d11")) {
    return GraphicRendererBackend::d3d11;
  }
  if (equals("d3d12") || equals("direct3d12")) {
    return GraphicRendererBackend::d3d12;
  }
  return GraphicRendererBackend::automatic;
}

std::string_view graphic_renderer_backend_config_name(
    const GraphicRendererBackend backend) noexcept {
  switch (backend) {
  case GraphicRendererBackend::automatic:
    return "display";
  case GraphicRendererBackend::d3d9:
    return "d3d9";
  case GraphicRendererBackend::d3d11:
    return "d3d11";
  case GraphicRendererBackend::d3d12:
    return "d3d12";
  case GraphicRendererBackend::glide:
    return "glide";
  case GraphicRendererBackend::software:
    return "cliff";
  }
  return "display";
}

std::string_view
graphic_renderer_backend_label(const GraphicRendererBackend backend) noexcept {
  switch (backend) {
  case GraphicRendererBackend::automatic:
    return "Auto";
  case GraphicRendererBackend::d3d9:
    return "D3D9";
  case GraphicRendererBackend::d3d11:
    return "D3D11";
  case GraphicRendererBackend::d3d12:
    return "D3D12";
  case GraphicRendererBackend::glide:
    return "Glide";
  case GraphicRendererBackend::software:
    return "Software";
  }
  return "Auto";
}

GraphicWindowMode
graphic_window_mode_from_config(const std::string_view value) noexcept {
  const auto equals = [value](const std::string_view expected) {
    return value.size() == expected.size() &&
           std::equal(value.begin(), value.end(), expected.begin(),
                      [](const char left, const char right) {
                        return std::tolower(static_cast<unsigned char>(left)) ==
                               std::tolower(static_cast<unsigned char>(right));
                      });
  };
  if (equals("Borderless")) {
    return GraphicWindowMode::borderless;
  }
  if (equals("Fullscreen")) {
    return GraphicWindowMode::fullscreen;
  }
  return GraphicWindowMode::windowed;
}

std::string_view
graphic_window_mode_config_name(const GraphicWindowMode mode) noexcept {
  switch (mode) {
  case GraphicWindowMode::windowed:
    return "Windowed";
  case GraphicWindowMode::borderless:
    return "Borderless";
  case GraphicWindowMode::fullscreen:
    return "Fullscreen";
  }
  return "Windowed";
}

std::vector<std::string> playable_graphic_renderer_arguments(
    const GraphicOptionsConfiguration &configuration) {
  const auto on_off = [](const bool enabled) {
    return enabled ? std::string("on") : std::string("off");
  };
  const auto texture_format =
      configuration.texture_format == GraphicTextureFormat::hi_colour     ? "16"
      : configuration.texture_format == GraphicTextureFormat::true_colour ? "32"
                                                                          : "8";
  const auto renderer_backend = [&configuration]() -> std::string {
    switch (configuration.renderer_backend) {
    case GraphicRendererBackend::automatic:
      return "auto";
    case GraphicRendererBackend::d3d9:
      return "d3d9";
    case GraphicRendererBackend::d3d11:
      return "d3d11";
    case GraphicRendererBackend::d3d12:
      return "d3d12";
    case GraphicRendererBackend::glide:
      return "glide";
    case GraphicRendererBackend::software:
      return "software";
    }
    return "auto";
  }();
  const auto info_mode = [](const GraphicInfoMode mode) {
    return mode == GraphicInfoMode::none        ? std::string("none")
           : mode == GraphicInfoMode::selective ? std::string("selective")
                                                : std::string("all");
  };
  const auto detail_mode = [](const GraphicDetailMode mode) {
    switch (mode) {
    case GraphicDetailMode::low:
      return std::string("low");
    case GraphicDetailMode::medium:
      return std::string("medium");
    case GraphicDetailMode::high:
      return std::string("high");
    case GraphicDetailMode::maximum:
      return std::string("maximum");
    case GraphicDetailMode::custom:
      return std::string("custom");
    }
    return std::string("high");
  };
  const auto name_plates = [&configuration] {
    return configuration.name_plates == GraphicNamePlateMode::none
               ? std::string("none")
           : configuration.name_plates == GraphicNamePlateMode::flat
               ? std::string("flat")
               : std::string("transparent");
  };
  const auto car_detail = [&configuration] {
    return configuration.car_detail == GraphicCarDetail::low
               ? std::string("low")
           : configuration.car_detail == GraphicCarDetail::medium
               ? std::string("medium")
               : std::string("high");
  };
  const auto car_shading = [&configuration] {
    switch (configuration.car_shading) {
    case GraphicCarShading::flat:
      return std::string("flat");
    case GraphicCarShading::gouraud:
      return std::string("gouraud");
    case GraphicCarShading::reflection:
      return std::string("reflection");
    case GraphicCarShading::glenz:
      return std::string("glenz");
    }
    return std::string("reflection");
  };
  return {
      "--renderer-backend",
      renderer_backend,
      "--true-colour",
      on_off(configuration.true_colour),
      "--triple-buffer",
      on_off(configuration.triple_buffer),
      "--trilinear-filtering",
      on_off(configuration.trilinear_filtering),
      "--texture-format",
      texture_format,
      "--info-detail",
      info_mode(configuration.info_detail),
      "--info-map",
      info_mode(configuration.info_map),
      "--checkpoint-info",
      info_mode(configuration.checkpoint_info),
      "--checkpoint-delay-ms",
      std::to_string(configuration.checkpoint_display_time_ms),
      "--detail-mode",
      detail_mode(configuration.detail_mode),
      "--lens-flares",
      on_off(configuration.lens_flares),
      "--sparks",
      on_off(configuration.sparks),
      "--smoke",
      on_off(configuration.smoke),
      "--halos",
      on_off(configuration.halos),
      "--shadows",
      on_off(configuration.shadows),
      "--skid-marks",
      on_off(configuration.skid_marks),
      "--name-plates",
      name_plates(),
      "--background",
      on_off(configuration.background),
      "--track-detail",
      configuration.track_detail == GraphicTrackDetail::medium ? "medium"
                                                                : "high",
      "--car-detail",
      car_detail(),
      "--car-shading",
      car_shading(),
      "--view-distance-percent",
      std::to_string(std::clamp(
          static_cast<int>(std::lround(configuration.view_distance)), 30,
          150)),
      "--brightness",
      std::to_string(configuration.brightness),
      "--motion-blur",
      on_off(configuration.motion_blur),
      "--camera-shake",
      on_off(configuration.camera_shake),
      "--ui-scale-percent",
      std::to_string(std::clamp(
          static_cast<int>(std::lround(configuration.ui_scale)), 50, 150)),
      "--z-read",
      on_off(configuration.z_read),
  };
}

void apply_graphic_detail_preset(GraphicOptionsConfiguration &configuration,
                                 const GraphicDetailMode mode) noexcept {
  configuration.detail_mode = mode;
  if (mode == GraphicDetailMode::custom) {
    return;
  }

  // DetailMode is interpreted as a complete profile by the original renderer;
  // the individual Custom values are not independently selected on those four
  // rows.  Resolve the profile here so all reconstructed backends receive the
  // same concrete settings.
  configuration.background = true;
  configuration.z_read = false;
  switch (mode) {
  case GraphicDetailMode::low:
    configuration.lens_flares = false;
    configuration.sparks = false;
    configuration.smoke = false;
    configuration.halos = false;
    configuration.shadows = false;
    configuration.skid_marks = false;
    configuration.name_plates = GraphicNamePlateMode::none;
    configuration.track_detail = GraphicTrackDetail::medium;
    configuration.car_detail = GraphicCarDetail::low;
    configuration.car_shading = GraphicCarShading::flat;
    configuration.view_distance = 50.0F;
    break;
  case GraphicDetailMode::medium:
    configuration.lens_flares = false;
    configuration.sparks = true;
    configuration.smoke = true;
    configuration.halos = true;
    configuration.shadows = true;
    configuration.skid_marks = true;
    configuration.name_plates = GraphicNamePlateMode::none;
    configuration.track_detail = GraphicTrackDetail::medium;
    configuration.car_detail = GraphicCarDetail::medium;
    configuration.car_shading = GraphicCarShading::gouraud;
    configuration.view_distance = 70.0F;
    break;
  case GraphicDetailMode::high:
    configuration.lens_flares = true;
    configuration.sparks = true;
    configuration.smoke = true;
    configuration.halos = true;
    configuration.shadows = true;
    configuration.skid_marks = true;
    configuration.name_plates = GraphicNamePlateMode::transparent;
    configuration.track_detail = GraphicTrackDetail::high;
    configuration.car_detail = GraphicCarDetail::high;
    configuration.car_shading = GraphicCarShading::reflection;
    configuration.view_distance = 100.0F;
    break;
  case GraphicDetailMode::maximum:
    configuration.lens_flares = true;
    configuration.sparks = true;
    configuration.smoke = true;
    configuration.halos = true;
    configuration.shadows = true;
    configuration.skid_marks = true;
    configuration.name_plates = GraphicNamePlateMode::transparent;
    configuration.track_detail = GraphicTrackDetail::high;
    configuration.car_detail = GraphicCarDetail::high;
    configuration.car_shading = GraphicCarShading::reflection;
    configuration.view_distance = 150.0F;
    break;
  case GraphicDetailMode::custom:
    break;
  }
}

float adjust_original_view_distance(const float current,
                                    const MenuDirection direction,
                                    const std::uint32_t elapsed_ms) noexcept {
  const auto bounded = std::clamp(current, 30.0F, 150.0F);
  constexpr auto units_per_ms = 0.5F;
  constexpr auto step = 5.0F;
  if (direction == MenuDirection::left || direction == MenuDirection::right) {
    const auto step_count = std::max(
        1, static_cast<int>(std::lround(static_cast<float>(elapsed_ms) *
                                        units_per_ms / step)));
    const auto current_percent = static_cast<int>(std::lround(bounded));
    return static_cast<float>(std::clamp(
        adjust_five_percent_index(current_percent, direction, step_count), 30,
        150));
  }
  return bounded;
}

FrontEndScreen FrontEndState::screen() const noexcept { return screen_; }

MainMenuChoice FrontEndState::main_choice() const noexcept {
  return main_choice_;
}

MultiplayerPage FrontEndState::multiplayer_page() const noexcept {
  return multiplayer_page_;
}

MultiplayerEntryChoice
FrontEndState::multiplayer_entry_choice() const noexcept {
  return multiplayer_entry_choice_;
}

MultiplayerJoinField FrontEndState::multiplayer_join_field() const noexcept {
  return multiplayer_join_field_;
}

MultiplayerCreateField
FrontEndState::multiplayer_create_field() const noexcept {
  return multiplayer_create_field_;
}

MultiplayerLobbyField FrontEndState::multiplayer_lobby_field() const noexcept {
  return multiplayer_lobby_field_;
}

const MultiplayerConfiguration &
FrontEndState::multiplayer_configuration() const noexcept {
  return multiplayer_;
}

void FrontEndState::set_multiplayer_sessions(
    std::vector<MultiplayerSessionEntry> sessions) {
  multiplayer_.sessions = std::move(sessions);
  if (multiplayer_.sessions.empty()) {
    multiplayer_.selected_session = 0U;
  } else {
    multiplayer_.selected_session = std::min<std::uint32_t>(
        multiplayer_.selected_session,
        static_cast<std::uint32_t>(multiplayer_.sessions.size() - 1U));
  }
}

void FrontEndState::prompt_multiplayer_password() {
  if (screen_ != FrontEndScreen::multiplayer ||
      multiplayer_page_ != MultiplayerPage::join) {
    return;
  }
  multiplayer_.session_password.clear();
  multiplayer_join_field_ = MultiplayerJoinField::password;
}

void FrontEndState::enter_multiplayer_lobby(const bool host) {
  const auto returning_to_existing_lobby =
      multiplayer_page_ == MultiplayerPage::lobby;
  multiplayer_page_ = MultiplayerPage::lobby;
  multiplayer_lobby_field_ = MultiplayerLobbyField::ready;
  multiplayer_.lobby_host = host;
  multiplayer_.status =
      host ? "Waiting for players..." : "Connection accepted.";
  if (!returning_to_existing_lobby) {
    multiplayer_.lobby_chat_lines.clear();
    multiplayer_.lobby_chat_lines.push_back({multiplayer_.status, 0x00e06820U});
  }
}

void FrontEndState::set_multiplayer_lobby_players(
    std::vector<MultiplayerLobbyPlayer> players, const bool local_ready) {
  multiplayer_.lobby_players = std::move(players);
  multiplayer_.local_ready = local_ready;
}

void FrontEndState::set_multiplayer_status(std::string status) {
  multiplayer_.status = std::move(status);
}

void FrontEndState::add_multiplayer_chat_line(std::string text,
                                              const std::uint32_t colour) {
  if (text.empty()) {
    return;
  }
  constexpr std::size_t retained_history = 96U;
  multiplayer_.lobby_chat_lines.push_back({std::move(text), colour});
  if (multiplayer_.lobby_chat_lines.size() > retained_history) {
    multiplayer_.lobby_chat_lines.erase(
        multiplayer_.lobby_chat_lines.begin(),
        multiplayer_.lobby_chat_lines.begin() +
            static_cast<std::ptrdiff_t>(multiplayer_.lobby_chat_lines.size() -
                                        retained_history));
  }
}

void FrontEndState::clear_multiplayer_chat_input() noexcept {
  multiplayer_.lobby_chat_input.clear();
}

void FrontEndState::set_multiplayer_lobby_configuration(
    std::string track, const std::uint32_t laps, std::string car,
    std::string gear, std::string game_mode) {
  multiplayer_.lobby_track = std::move(track);
  multiplayer_.lobby_laps = laps;
  multiplayer_.lobby_car = std::move(car);
  multiplayer_.lobby_gear = std::move(gear);
  multiplayer_.lobby_game_mode = std::move(game_mode);
}

void FrontEndState::leave_multiplayer_lobby() noexcept {
  multiplayer_page_ = MultiplayerPage::entry;
  multiplayer_entry_choice_ = MultiplayerEntryChoice::join;
  multiplayer_.lobby_players.clear();
  multiplayer_.lobby_chat_lines.clear();
  multiplayer_.lobby_chat_input.clear();
  multiplayer_.local_ready = false;
  multiplayer_.lobby_host = false;
  multiplayer_.status.clear();
}

bool FrontEndState::exit_confirmation_open() const noexcept {
  return exit_confirmation_open_;
}

ExitConfirmationChoice
FrontEndState::exit_confirmation_choice() const noexcept {
  return exit_confirmation_choice_;
}

bool FrontEndState::league_create_confirmation_open() const noexcept {
  return league_create_confirmation_open_;
}

ExitConfirmationChoice
FrontEndState::league_create_confirmation_choice() const noexcept {
  return league_create_confirmation_choice_;
}

bool FrontEndState::league_delete_confirmation_open() const noexcept {
  return league_delete_confirmation_open_;
}

ExitConfirmationChoice
FrontEndState::league_delete_confirmation_choice() const noexcept {
  return league_delete_confirmation_choice_;
}

OnePlayerChoice FrontEndState::one_player_choice() const noexcept {
  return one_player_choice_;
}

GhostModeChoice FrontEndState::ghost_mode_choice() const noexcept {
  return ghost_mode_choice_;
}

bool FrontEndState::ghost_file_focused() const noexcept {
  return ghost_file_focused_;
}

std::uint32_t FrontEndState::ghost_file_index() const noexcept {
  return ghost_file_index_;
}

std::uint32_t FrontEndState::ghost_file_count() const noexcept {
  return ghost_file_count_;
}

LeagueMenuChoice FrontEndState::league_menu_choice() const noexcept {
  return league_menu_choice_;
}

std::uint32_t FrontEndState::league_index() const noexcept {
  return league_index_;
}

std::uint32_t FrontEndState::league_count() const noexcept {
  return league_count_;
}

LeagueCreateField FrontEndState::league_create_field() const noexcept {
  return league_create_field_;
}

std::uint32_t FrontEndState::league_create_division_index() const noexcept {
  return league_create_division_index_;
}

std::uint32_t FrontEndState::league_create_division_count() const noexcept {
  return league_create_division_count_;
}

OptionsMenuChoice FrontEndState::options_choice() const noexcept {
  return options_choice_;
}

DifficultyChoice FrontEndState::difficulty() const noexcept {
  return difficulty_;
}

RankingsField FrontEndState::rankings_field() const noexcept {
  return rankings_field_;
}

const RankingsConfiguration &
FrontEndState::rankings_configuration() const noexcept {
  return rankings_;
}

GraphicOptionsField FrontEndState::graphic_options_field() const noexcept {
  return graphic_options_field_;
}

GameplayOptionsField FrontEndState::gameplay_options_field() const noexcept {
  return gameplay_options_field_;
}

const GraphicOptionsConfiguration &
FrontEndState::graphic_options_configuration() const noexcept {
  return graphic_options_;
}

PersonalOptionsField FrontEndState::personal_options_field() const noexcept {
  return personal_options_field_;
}

const PersonalOptionsConfiguration &
FrontEndState::personal_options_configuration() const noexcept {
  return personal_options_;
}

ControlOptionsFocus FrontEndState::control_options_focus() const noexcept {
  return control_options_focus_;
}

ControlOptionsField FrontEndState::control_options_field() const noexcept {
  return control_options_field_;
}

const ControlOptionsConfiguration &
FrontEndState::control_options_configuration() const noexcept {
  return control_options_;
}

bool FrontEndState::control_binding_capture_active() const noexcept {
  return control_binding_capture_active_;
}

SoundOptionsField FrontEndState::sound_options_field() const noexcept {
  return sound_options_field_;
}

const SoundOptionsConfiguration &
FrontEndState::sound_options_configuration() const noexcept {
  return sound_options_;
}

RaceSetupMode FrontEndState::race_setup_mode() const noexcept {
  return race_setup_.mode;
}

RaceSetupField FrontEndState::race_setup_field() const noexcept {
  return race_setup_field_;
}

std::uint32_t FrontEndState::race_setup_track_index() const noexcept {
  return race_setup_.track_index;
}

std::uint32_t FrontEndState::race_setup_track_count() const noexcept {
  return race_setup_.track_count;
}

std::uint32_t FrontEndState::race_setup_unlocked_track_count() const noexcept {
  return race_setup_.unlocked_track_count;
}

std::uint32_t FrontEndState::race_setup_laps() const noexcept {
  return race_setup_.laps;
}

bool FrontEndState::race_setup_catch_up() const noexcept {
  return race_setup_.catch_up;
}

CarSetupField FrontEndState::car_setup_field() const noexcept {
  return car_setup_field_;
}

std::uint32_t FrontEndState::car_setup_car_index() const noexcept {
  return car_setup_.car_index;
}

std::uint32_t FrontEndState::car_setup_car_count() const noexcept {
  return car_setup_.car_count;
}

std::uint32_t FrontEndState::car_setup_unlocked_car_count() const noexcept {
  return car_setup_.unlocked_car_count;
}

bool FrontEndState::car_setup_automatic_transmission() const noexcept {
  return car_setup_.automatic_transmission;
}

std::uint32_t FrontEndState::car_setup_horn_index() const noexcept {
  return car_setup_.horn_index;
}

std::uint32_t FrontEndState::car_setup_horn_count() const noexcept {
  return car_setup_.horn_count;
}

bool FrontEndState::car_setup_record_race() const noexcept {
  return car_setup_.record_race;
}

void FrontEndState::configure_race_setup(
    RaceSetupConfiguration configuration) noexcept {
  configuration.track_count = std::max(configuration.track_count, 1U);
  configuration.unlocked_track_count =
      std::min(configuration.unlocked_track_count, configuration.track_count);
  configuration.track_index =
      std::min(configuration.track_index, configuration.track_count - 1U);
  configuration.laps = race_lap_values[race_lap_index(configuration.laps)];
  race_setup_ = configuration;
}

void FrontEndState::configure_car_setup(
    CarSetupConfiguration configuration) noexcept {
  configuration.car_count = std::max(configuration.car_count, 1U);
  configuration.unlocked_car_count =
      std::min(configuration.unlocked_car_count, configuration.car_count);
  configuration.car_index =
      std::min(configuration.car_index, configuration.car_count - 1U);
  configuration.horn_count = std::max(configuration.horn_count, 1U);
  configuration.horn_index =
      std::min(configuration.horn_index, configuration.horn_count - 1U);
  car_setup_ = configuration;
}

void FrontEndState::configure_rankings(
    const std::uint32_t track_count) noexcept {
  rankings_.track_count = std::max(track_count, 1U);
  rankings_.track_index =
      std::min(rankings_.track_index, rankings_.track_count - 1U);
}

void FrontEndState::configure_graphic_options(
    GraphicOptionsConfiguration configuration) noexcept {
  configuration.brightness = std::clamp(configuration.brightness, 0.0F, 2.0F);
  configuration.brightness =
      std::round(configuration.brightness / 0.05F) * 0.05F;
  configuration.checkpoint_display_time_ms =
      std::min(configuration.checkpoint_display_time_ms, 10000U);
  configuration.checkpoint_display_time_ms =
      ((configuration.checkpoint_display_time_ms + 250U) / 500U) * 500U;
  configuration.view_distance =
      std::clamp(configuration.view_distance, 30.0F, 150.0F);
  configuration.view_distance =
      std::round(configuration.view_distance / 5.0F) * 5.0F;
  configuration.ui_scale = std::clamp(configuration.ui_scale, 50.0F, 150.0F);
  configuration.ui_scale =
      std::round(configuration.ui_scale / 5.0F) * 5.0F;
  graphic_options_ = configuration;
}

void FrontEndState::configure_personal_options(
    PersonalOptionsConfiguration configuration) {
  configuration.player_name.resize(
      std::min<std::size_t>(configuration.player_name.size(), 16U));
  configuration.team_name.resize(
      std::min<std::size_t>(configuration.team_name.size(), 5U));
  for (auto &short_key : configuration.short_keys) {
    short_key.resize(std::min<std::size_t>(short_key.size(), 128U));
  }
  personal_options_ = std::move(configuration);
}

void FrontEndState::configure_control_options(
    ControlOptionsConfiguration configuration) {
  if (configuration.profiles.empty()) {
    configuration.profiles.push_back(ControlProfileConfiguration{});
  }
  configuration.profile_index =
      std::min(configuration.profile_index,
               static_cast<std::uint32_t>(configuration.profiles.size() - 1U));
  for (auto &profile : configuration.profiles) {
    if (profile.name.empty()) {
      profile.name = "Custom";
    }
    for (auto &speed : profile.mouse_speeds) {
      speed = std::clamp(speed, 0.05F, 10.0F);
      speed = std::round(speed / 0.05F) * 0.05F;
    }
  }
  control_options_ = std::move(configuration);
}

void FrontEndState::configure_sound_options(
    SoundOptionsConfiguration configuration) {
  if (configuration.output_devices.empty()) {
    configuration.output_devices.emplace_back();
  }
  if (configuration.output_device_index >= configuration.output_devices.size()) {
    configuration.output_device_index = 0U;
  }
  const auto quantize_volume = [](const std::uint8_t volume) {
    constexpr std::uint32_t levels = 20U;
    const auto level = static_cast<std::uint32_t>(
        std::lround(static_cast<double>(volume) * levels / 255.0));
    return static_cast<std::uint8_t>(
        (level * 255U + levels / 2U) / levels);
  };
  configuration.sound_effects_volume =
      quantize_volume(configuration.sound_effects_volume);
  configuration.cd_music_volume = quantize_volume(configuration.cd_music_volume);
  configuration.first_cd_track =
      std::max<std::uint8_t>(configuration.first_cd_track, 2U);
  configuration.last_cd_track =
      std::max(configuration.last_cd_track, configuration.first_cd_track);
  if (configuration.assigned_cd_tracks.empty()) {
    configuration.assigned_cd_tracks.push_back(configuration.first_cd_track);
  }
  configuration.fixed_cd_track_count = std::min(
      configuration.fixed_cd_track_count,
      static_cast<std::uint32_t>(configuration.assigned_cd_tracks.size()));
  for (auto &track : configuration.assigned_cd_tracks) {
    track = std::clamp(track, configuration.first_cd_track,
                       configuration.last_cd_track);
  }
  for (std::uint32_t index = 0U; index < configuration.fixed_cd_track_count;
       ++index) {
    configuration.assigned_cd_tracks[index] =
        static_cast<std::uint8_t>(std::min<std::uint32_t>(
            static_cast<std::uint32_t>(configuration.first_cd_track) + index,
            configuration.last_cd_track));
  }
  configuration.song_index = std::min(
      configuration.song_index,
      static_cast<std::uint32_t>(configuration.assigned_cd_tracks.size() - 1U));
  sound_options_ = std::move(configuration);
}

void FrontEndState::configure_ghost_files(
    const std::uint32_t current_index,
    const std::uint32_t file_count) noexcept {
  ghost_file_count_ = file_count;
  ghost_file_index_ =
      file_count == 0U ? 0U : std::min(current_index, file_count - 1U);
  if (file_count == 0U) {
    ghost_file_focused_ = false;
  }
}

void FrontEndState::configure_one_player_league_available(
    const bool available) noexcept {
  one_player_league_available_ = available;
}

MenuTransitionResult FrontEndState::delete_current_ghost_file() noexcept {
  if (screen_ != FrontEndScreen::ghost_setup || !ghost_file_focused_ ||
      ghost_file_count_ == 0U) {
    return result_with_sound(MenuTransitionEvidence::confirmed_no_change, false,
                             false, MenuSoundEvent::select_error);
  }
  --ghost_file_count_;
  if (ghost_file_count_ == 0U) {
    ghost_file_index_ = 0U;
    ghost_file_focused_ = false;
  } else if (ghost_file_index_ >= ghost_file_count_) {
    ghost_file_index_ = ghost_file_count_ - 1U;
  }
  return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                           false, MenuSoundEvent::select);
}

void FrontEndState::configure_leagues(
    const std::uint32_t current_index,
    const std::uint32_t league_count) noexcept {
  league_count_ = std::max(league_count, 1U);
  league_index_ = std::min(current_index, league_count_ - 1U);
}

void FrontEndState::configure_league_create_divisions(
    const std::uint32_t current_index,
    const std::uint32_t division_count) noexcept {
  league_create_division_count_ = std::max(division_count, 1U);
  league_create_division_index_ =
      std::min(current_index, league_create_division_count_ - 1U);
}

void FrontEndState::show_main_menu_first_choice() noexcept {
  screen_ = FrontEndScreen::main_menu;
  main_choice_ = MainMenuChoice::one_player;
  exit_confirmation_open_ = false;
  league_create_confirmation_open_ = false;
  league_delete_confirmation_open_ = false;
}

bool FrontEndState::append_personal_text(const std::string_view text) {
  std::string *destination = nullptr;
  auto limit = std::size_t{0U};
  if (personal_options_field_ == PersonalOptionsField::name) {
    destination = &personal_options_.player_name;
    limit = 16U;
  } else if (personal_options_field_ == PersonalOptionsField::team) {
    destination = &personal_options_.team_name;
    limit = 5U;
  } else {
    const auto index = static_cast<std::uint8_t>(personal_options_field_);
    const auto first =
        static_cast<std::uint8_t>(PersonalOptionsField::short_key_0);
    const auto last =
        static_cast<std::uint8_t>(PersonalOptionsField::short_key_9);
    if (index >= first && index <= last) {
      destination = &personal_options_.short_keys[index - first];
      limit = 128U;
    }
  }
  if (destination == nullptr || destination->size() >= limit) {
    return false;
  }
  const auto old_size = destination->size();
  for (const auto value : text) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte >= 0x20U && byte <= 0x7eU && destination->size() < limit) {
      destination->push_back(static_cast<char>(byte));
    }
  }
  return destination->size() != old_size;
}

bool FrontEndState::backspace_personal_text() noexcept {
  std::string *destination = nullptr;
  if (personal_options_field_ == PersonalOptionsField::name) {
    destination = &personal_options_.player_name;
  } else if (personal_options_field_ == PersonalOptionsField::team) {
    destination = &personal_options_.team_name;
  } else {
    const auto index = static_cast<std::uint8_t>(personal_options_field_);
    const auto first =
        static_cast<std::uint8_t>(PersonalOptionsField::short_key_0);
    const auto last =
        static_cast<std::uint8_t>(PersonalOptionsField::short_key_9);
    if (index >= first && index <= last) {
      destination = &personal_options_.short_keys[index - first];
    }
  }
  if (destination == nullptr || destination->empty()) {
    return false;
  }
  destination->pop_back();
  return true;
}

bool FrontEndState::append_multiplayer_text(const std::string_view text) {
  if (screen_ != FrontEndScreen::multiplayer) {
    return false;
  }
  std::string *destination = nullptr;
  auto limit = std::size_t{0U};
  if (multiplayer_page_ == MultiplayerPage::join &&
      multiplayer_join_field_ == MultiplayerJoinField::address) {
    destination = &multiplayer_.address;
    limit = 63U;
  } else if (multiplayer_page_ == MultiplayerPage::join &&
             multiplayer_join_field_ == MultiplayerJoinField::password) {
    destination = &multiplayer_.session_password;
    limit = 15U;
  } else if (multiplayer_page_ == MultiplayerPage::create &&
             multiplayer_create_field_ ==
                 MultiplayerCreateField::session_name) {
    destination = &multiplayer_.session_name;
    limit = 15U;
  } else if (multiplayer_page_ == MultiplayerPage::create &&
             multiplayer_create_field_ ==
                 MultiplayerCreateField::session_password) {
    destination = &multiplayer_.session_password;
    limit = 15U;
  } else if (multiplayer_page_ == MultiplayerPage::lobby) {
    destination = &multiplayer_.lobby_chat_input;
    limit = 63U;
  }
  if (destination == nullptr || destination->size() >= limit) {
    return false;
  }
  const auto old_size = destination->size();
  for (const auto value : text) {
    const auto byte = static_cast<unsigned char>(value);
    if (byte >= 0x20U && byte <= 0x7eU && destination->size() < limit) {
      destination->push_back(static_cast<char>(byte));
    }
  }
  return destination->size() != old_size;
}

bool FrontEndState::backspace_multiplayer_text() noexcept {
  std::string *destination = nullptr;
  if (screen_ == FrontEndScreen::multiplayer &&
      multiplayer_page_ == MultiplayerPage::join &&
      multiplayer_join_field_ == MultiplayerJoinField::address) {
    destination = &multiplayer_.address;
  } else if (screen_ == FrontEndScreen::multiplayer &&
             multiplayer_page_ == MultiplayerPage::join &&
             multiplayer_join_field_ == MultiplayerJoinField::password) {
    destination = &multiplayer_.session_password;
  } else if (screen_ == FrontEndScreen::multiplayer &&
             multiplayer_page_ == MultiplayerPage::create &&
             multiplayer_create_field_ ==
                 MultiplayerCreateField::session_name) {
    destination = &multiplayer_.session_name;
  } else if (screen_ == FrontEndScreen::multiplayer &&
             multiplayer_page_ == MultiplayerPage::create &&
             multiplayer_create_field_ ==
                 MultiplayerCreateField::session_password) {
    destination = &multiplayer_.session_password;
  } else if (screen_ == FrontEndScreen::multiplayer &&
             multiplayer_page_ == MultiplayerPage::lobby) {
    destination = &multiplayer_.lobby_chat_input;
  }
  if (destination == nullptr || destination->empty()) {
    return false;
  }
  destination->pop_back();
  return true;
}

MenuTransitionResult
FrontEndState::bind_control_input(const std::string_view input) {
  if (!control_binding_capture_active_ ||
      control_options_focus_ != ControlOptionsFocus::field_list) {
    return {};
  }
  control_binding_capture_active_ = false;
  const auto field = static_cast<std::uint8_t>(control_options_field_);
  const auto first = static_cast<std::uint8_t>(ControlOptionsField::turn_left);
  if (field < first || input.empty()) {
    return result_with_sound(MenuTransitionEvidence::confirmed_no_change, false,
                             false, MenuSoundEvent::binding_reserved);
  }
  auto &bindings =
      control_options_.profiles[control_options_.profile_index].bindings;
  const auto target = static_cast<std::size_t>(field - first);
  const auto old = bindings[target];
  auto duplicate = bindings.end();
  for (auto iterator = bindings.begin(); iterator != bindings.end();
       ++iterator) {
    if (static_cast<std::size_t>(std::distance(bindings.begin(), iterator)) !=
            target &&
        *iterator == input) {
      duplicate = iterator;
      break;
    }
  }
  bindings[target] = std::string(input);
  MenuTransitionResult result;
  result.evidence = MenuTransitionEvidence::confirmed_change;
  result.selection_changed = true;
  if (duplicate != bindings.end()) {
    *duplicate = old;
    result.sound_events[0U] = MenuSoundEvent::binding_swapped;
    result.sound_events[1U] = MenuSoundEvent::binding_assigned;
    result.sound_event_count = 2U;
  } else {
    result.sound_events[0U] = MenuSoundEvent::binding_assigned;
    result.sound_event_count = 1U;
  }
  result.control_settings_changed = true;
  return result;
}

MenuTransitionResult
FrontEndState::adjust_control_analog(const MenuDirection direction,
                                     const std::uint32_t elapsed_ms) noexcept {
  const auto index = static_cast<std::uint8_t>(control_options_field_);
  const auto first =
      static_cast<std::uint8_t>(ControlOptionsField::mouse_speed_x);
  const auto last =
      static_cast<std::uint8_t>(ControlOptionsField::mouse_speed_z);
  if (control_options_focus_ != ControlOptionsFocus::field_list ||
      index < first || index > last ||
      (direction != MenuDirection::left && direction != MenuDirection::right) ||
      elapsed_ms == 0U) {
    return {};
  }
  auto &value = control_options_.profiles[control_options_.profile_index]
                    .mouse_speeds[index - first];
  const auto old = value;
  const auto current_percent =
      static_cast<int>(std::lround(static_cast<double>(value) * 100.0));
  value = static_cast<float>(std::clamp(
              adjust_five_percent_index(current_percent, direction, 1),
              5, 1000)) /
          100.0F;
  if (value == old) {
    return result_with_sound(MenuTransitionEvidence::confirmed_no_change, false,
                             false, MenuSoundEvent::volume_error);
  }
  auto result = result_with_sound(
      MenuTransitionEvidence::confirmed_change, true, false,
      direction == MenuDirection::left ? MenuSoundEvent::volume_left
                                       : MenuSoundEvent::volume_right);
  result.control_settings_changed = true;
  return result;
}

MenuTransitionResult
FrontEndState::adjust_graphic_held(const MenuDirection direction,
                                   const std::uint32_t elapsed_ms) noexcept {
  if (screen_ != FrontEndScreen::graphic_options ||
      (direction != MenuDirection::left && direction != MenuDirection::right) ||
      elapsed_ms == 0U) {
    return {};
  }
  auto changed = result_with_sound(
      MenuTransitionEvidence::confirmed_change, true, false,
      direction == MenuDirection::left ? MenuSoundEvent::volume_left
                                       : MenuSoundEvent::volume_right);
  changed.graphic_settings_changed = true;
  if (graphic_options_field_ == GraphicOptionsField::brightness) {
    const auto old = graphic_options_.brightness;
    constexpr auto step = 0.05F;
    constexpr auto units_per_ms = 0.005F;
    const auto step_count = std::max(
        1, static_cast<int>(std::lround(static_cast<float>(elapsed_ms) *
                                        units_per_ms / step)));
    const auto current_percent =
        static_cast<int>(std::lround(static_cast<double>(old) * 100.0));
    graphic_options_.brightness =
        static_cast<float>(std::clamp(adjust_five_percent_index(
                                          current_percent, direction, step_count),
                                      0, 200)) /
        100.0F;
    if (graphic_options_.brightness != old) {
      return changed;
    }
  } else if (graphic_options_field_ == GraphicOptionsField::view_distance) {
    const auto old = graphic_options_.view_distance;
    // Quantize the recovered held-input response so both Graphics pages expose
    // stable 5% values instead of frame-time-dependent fractional percentages.
    graphic_options_.view_distance =
        adjust_original_view_distance(old, direction, elapsed_ms);
    if (graphic_options_.view_distance != old) {
      return changed;
    }
  } else {
    return {};
  }
  return result_with_sound(MenuTransitionEvidence::confirmed_no_change, false,
                           false, MenuSoundEvent::volume_error);
}

MenuTransitionResult
FrontEndState::adjust_gameplay_held(const MenuDirection direction,
                                    const std::uint32_t elapsed_ms) noexcept {
  if (screen_ != FrontEndScreen::gameplay_options ||
      (direction != MenuDirection::left && direction != MenuDirection::right) ||
      elapsed_ms == 0U) {
    return {};
  }
  auto changed = result_with_sound(
      MenuTransitionEvidence::confirmed_change, true, false,
      direction == MenuDirection::left ? MenuSoundEvent::volume_left
                                       : MenuSoundEvent::volume_right);
  changed.graphic_settings_changed = true;
  if (gameplay_options_field_ == GameplayOptionsField::checkpoint_display_time) {
    const auto old = graphic_options_.checkpoint_display_time_ms;
    constexpr std::uint32_t step_ms = 500U;
    if (direction == MenuDirection::left) {
      graphic_options_.checkpoint_display_time_ms =
          old > step_ms ? old - step_ms : 0U;
    } else {
      graphic_options_.checkpoint_display_time_ms =
          std::min<std::uint32_t>(old + step_ms, 10000U);
    }
    if (graphic_options_.checkpoint_display_time_ms != old) {
      return changed;
    }
  } else if (gameplay_options_field_ == GameplayOptionsField::ui_scale) {
    const auto old = graphic_options_.ui_scale;
    constexpr auto step = 5.0F;
    constexpr auto units_per_ms = 0.5F;
    const auto step_count = std::max(
        1, static_cast<int>(std::lround(static_cast<float>(elapsed_ms) *
                                        units_per_ms / step)));
    const auto current_percent = static_cast<int>(std::lround(old));
    graphic_options_.ui_scale = static_cast<float>(std::clamp(
        adjust_five_percent_index(current_percent, direction, step_count), 50,
        150));
    if (graphic_options_.ui_scale != old) {
      return changed;
    }
  } else {
    return {};
  }
  return result_with_sound(MenuTransitionEvidence::confirmed_no_change, false,
                           false, MenuSoundEvent::volume_error);
}

MenuTransitionResult
FrontEndState::navigate(const MenuDirection direction) noexcept {
  if (exit_confirmation_open_) {
    if (direction == MenuDirection::up || direction == MenuDirection::down) {
      return {MenuTransitionEvidence::confirmed_no_change, false, false};
    }
    if (direction == MenuDirection::left) {
      if (exit_confirmation_choice_ == ExitConfirmationChoice::yes) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::select_error);
      }
      exit_confirmation_choice_ = ExitConfirmationChoice::yes;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::select_left);
    }
    if (exit_confirmation_choice_ == ExitConfirmationChoice::no) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::select_error);
    }
    exit_confirmation_choice_ = ExitConfirmationChoice::no;
    return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                             false, MenuSoundEvent::select_right);
  }
  if (league_create_confirmation_open_) {
    if (direction == MenuDirection::up || direction == MenuDirection::down) {
      return {MenuTransitionEvidence::confirmed_no_change, false, false};
    }
    if (direction == MenuDirection::left) {
      if (league_create_confirmation_choice_ == ExitConfirmationChoice::yes) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::select_error);
      }
      league_create_confirmation_choice_ = ExitConfirmationChoice::yes;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::select_left);
    }
    if (league_create_confirmation_choice_ == ExitConfirmationChoice::no) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::select_error);
    }
    league_create_confirmation_choice_ = ExitConfirmationChoice::no;
    return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                             false, MenuSoundEvent::select_right);
  }
  if (league_delete_confirmation_open_) {
    if (direction == MenuDirection::up || direction == MenuDirection::down) {
      return {MenuTransitionEvidence::confirmed_no_change, false, false};
    }
    if (direction == MenuDirection::left) {
      if (league_delete_confirmation_choice_ == ExitConfirmationChoice::yes) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::select_error);
      }
      league_delete_confirmation_choice_ = ExitConfirmationChoice::yes;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::select_left);
    }
    if (league_delete_confirmation_choice_ == ExitConfirmationChoice::no) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::select_error);
    }
    league_delete_confirmation_choice_ = ExitConfirmationChoice::no;
    return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                             false, MenuSoundEvent::select_right);
  }
  if (screen_ == FrontEndScreen::multiplayer) {
    const auto boundary = [](const MenuSoundEvent sound) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, sound);
    };
    const auto changed = [](const MenuSoundEvent sound) {
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, sound);
    };
    if (multiplayer_page_ == MultiplayerPage::entry) {
      if (direction == MenuDirection::up) {
        if (multiplayer_entry_choice_ == MultiplayerEntryChoice::join) {
          return boundary(MenuSoundEvent::field_error);
        }
        multiplayer_entry_choice_ = MultiplayerEntryChoice::join;
        return changed(MenuSoundEvent::field_up);
      }
      if (direction == MenuDirection::down) {
        if (multiplayer_entry_choice_ == MultiplayerEntryChoice::create) {
          return boundary(MenuSoundEvent::field_error);
        }
        multiplayer_entry_choice_ = MultiplayerEntryChoice::create;
        return changed(MenuSoundEvent::field_down);
      }
      return boundary(MenuSoundEvent::field_navigation_error);
    }
    if (multiplayer_page_ == MultiplayerPage::join) {
      if (multiplayer_join_field_ == MultiplayerJoinField::password) {
        return boundary(MenuSoundEvent::field_navigation_error);
      }
      if (direction == MenuDirection::up) {
        if (multiplayer_join_field_ == MultiplayerJoinField::sessions) {
          return boundary(MenuSoundEvent::field_error);
        }
        multiplayer_join_field_ =
            multiplayer_join_field_ == MultiplayerJoinField::refresh
                ? MultiplayerJoinField::address
                : MultiplayerJoinField::sessions;
        return changed(MenuSoundEvent::field_up);
      }
      if (direction == MenuDirection::down) {
        if (multiplayer_join_field_ == MultiplayerJoinField::refresh) {
          return boundary(MenuSoundEvent::field_error);
        }
        multiplayer_join_field_ =
            multiplayer_join_field_ == MultiplayerJoinField::sessions
                ? MultiplayerJoinField::address
                : MultiplayerJoinField::refresh;
        return changed(MenuSoundEvent::field_down);
      }
      if (multiplayer_join_field_ == MultiplayerJoinField::sessions &&
          !multiplayer_.sessions.empty() &&
          (direction == MenuDirection::left ||
           direction == MenuDirection::right)) {
        const auto count =
            static_cast<std::uint32_t>(multiplayer_.sessions.size());
        if (direction == MenuDirection::left) {
          multiplayer_.selected_session =
              multiplayer_.selected_session == 0U
                  ? count - 1U
                  : multiplayer_.selected_session - 1U;
        } else {
          multiplayer_.selected_session =
              (multiplayer_.selected_session + 1U) % count;
        }
        multiplayer_.address =
            multiplayer_.sessions[multiplayer_.selected_session].address;
        multiplayer_.session_password.clear();
        return changed(direction == MenuDirection::left
                           ? MenuSoundEvent::select_left
                           : MenuSoundEvent::select_right);
      }
      return boundary(MenuSoundEvent::select_error);
    }
    if (multiplayer_page_ == MultiplayerPage::lobby) {
      if (direction == MenuDirection::up &&
          multiplayer_lobby_field_ == MultiplayerLobbyField::start) {
        multiplayer_lobby_field_ = MultiplayerLobbyField::ready;
        return changed(MenuSoundEvent::field_up);
      }
      if (direction == MenuDirection::down && multiplayer_.lobby_host &&
          multiplayer_lobby_field_ == MultiplayerLobbyField::ready) {
        multiplayer_lobby_field_ = MultiplayerLobbyField::start;
        return changed(MenuSoundEvent::field_down);
      }
      return boundary(MenuSoundEvent::field_error);
    }
    const auto index = static_cast<std::uint8_t>(multiplayer_create_field_);
    if (direction == MenuDirection::up) {
      if (index == 0U) {
        return boundary(MenuSoundEvent::field_error);
      }
      multiplayer_create_field_ =
          static_cast<MultiplayerCreateField>(index - 1U);
      return changed(MenuSoundEvent::field_up);
    }
    if (direction == MenuDirection::down) {
      if (multiplayer_create_field_ == MultiplayerCreateField::protocol) {
        return boundary(MenuSoundEvent::field_error);
      }
      multiplayer_create_field_ =
          static_cast<MultiplayerCreateField>(index + 1U);
      return changed(MenuSoundEvent::field_down);
    }
    // The imported p3.1 network object is type 4 (TCP/IP), so no unsupported
    // legacy DirectPlay/IPX transport is invented by the reconstruction.
    return boundary(MenuSoundEvent::select_error);
  }
  if (screen_ == FrontEndScreen::rankings) {
    // p3.1 RVA 0x84b71..0x84cbf: Up/Down change the value owned by the
    // focused slot. Any successful change reloads the ranking payload.
    if (direction == MenuDirection::up || direction == MenuDirection::down) {
      std::uint32_t limit = std::max(rankings_.track_count, 1U);
      switch (rankings_field_) {
      case RankingsField::type:
        limit = 4U;
        break;
      case RankingsField::track_or_division:
        if (rankings_.type == RankingsType::league) {
          limit = 3U;
        }
        break;
      case RankingsField::laps:
        limit = 6U;
        break;
      case RankingsField::difficulty:
        limit = 3U;
        break;
      case RankingsField::orientation:
        // The fifth internal slot is initialized from profile +0x918.
        limit = 2U;
        break;
      }
      // Avoid aliasing enum/bool storage while preserving the original
      // per-slot numeric bounds.
      const auto read_value = [&]() -> std::uint32_t {
        switch (rankings_field_) {
        case RankingsField::type:
          return static_cast<std::uint32_t>(rankings_.type);
        case RankingsField::track_or_division:
          return rankings_.type == RankingsType::league
                     ? rankings_.division_index
                     : rankings_.track_index;
        case RankingsField::laps:
          return rankings_.laps_index;
        case RankingsField::difficulty:
          return static_cast<std::uint32_t>(rankings_.difficulty);
        case RankingsField::orientation:
          return rankings_.mirror ? 1U : 0U;
        }
        return 0U;
      };
      const auto write_value = [&](const std::uint32_t next) {
        switch (rankings_field_) {
        case RankingsField::type:
          rankings_.type = static_cast<RankingsType>(next);
          break;
        case RankingsField::track_or_division:
          if (rankings_.type == RankingsType::league) {
            rankings_.division_index = next;
          } else {
            rankings_.track_index = next;
          }
          break;
        case RankingsField::laps:
          rankings_.laps_index = next;
          break;
        case RankingsField::difficulty:
          rankings_.difficulty = static_cast<DifficultyChoice>(next);
          break;
        case RankingsField::orientation:
          rankings_.mirror = next != 0U;
          break;
        }
      };
      const auto current = read_value();
      if (direction == MenuDirection::up) {
        if (current == 0U) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false, MenuSoundEvent::field_error);
        }
        write_value(current - 1U);
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::field_up);
      }
      if (current + 1U >= limit) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      write_value(current + 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_down);
    }

    const auto current = static_cast<std::uint8_t>(rankings_field_);
    if (direction == MenuDirection::left) {
      if (current == 0U) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false,
                                 MenuSoundEvent::field_navigation_error);
      }
      std::uint8_t next = current - 1U;
      if (current == 3U && (rankings_.type == RankingsType::best_laptime ||
                            rankings_.type == RankingsType::league)) {
        next = 1U;
      } else if (current == 4U) {
        next = rankings_.type == RankingsType::best_laptime    ? 1U
               : rankings_.type == RankingsType::best_racetime ? 2U
               : rankings_.type == RankingsType::single_race   ? 3U
                                                               : 1U;
      }
      rankings_field_ = static_cast<RankingsField>(next);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_left);
    }

    std::uint8_t next = current;
    if (current == 0U) {
      next = 1U;
    } else if (current == 1U) {
      if (rankings_.type == RankingsType::best_laptime) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false,
                                 MenuSoundEvent::field_navigation_error);
      }
      next = rankings_.type == RankingsType::league ? 3U : 2U;
    } else if (current == 2U) {
      if (rankings_.type == RankingsType::best_laptime ||
          rankings_.type == RankingsType::best_racetime) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false,
                                 MenuSoundEvent::field_navigation_error);
      }
      next = 3U;
    } else {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false,
                               MenuSoundEvent::field_navigation_error);
    }
    rankings_field_ = static_cast<RankingsField>(next);
    return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                             false, MenuSoundEvent::field_right);
  }
  if (screen_ == FrontEndScreen::ghost_setup) {
    // p3.1 bank-19 input callback RVA 0x80a98. Internal selector value 8 is
    // the demo-file list; values 0..2 are the three vertically clamped modes.
    if (ghost_file_focused_) {
      if (direction == MenuDirection::left) {
        ghost_file_focused_ = false;
        ghost_mode_choice_ = GhostModeChoice::ghost_race;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::select_right);
      }
      if (direction == MenuDirection::right) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::select_error);
      }
      if (direction == MenuDirection::up) {
        if (ghost_file_index_ == 0U) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false, MenuSoundEvent::field_error);
        }
        --ghost_file_index_;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::field_up);
      }
      if (ghost_file_index_ + 1U >= ghost_file_count_) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      ++ghost_file_index_;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_down);
    }

    if (direction == MenuDirection::left) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::select_error);
    }
    if (direction == MenuDirection::right) {
      if (ghost_file_count_ == 0U) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::select_error);
      }
      ghost_file_focused_ = true;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::select_left);
    }
    const auto index = static_cast<std::uint8_t>(ghost_mode_choice_);
    if (direction == MenuDirection::up) {
      if (index == 0U) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      ghost_mode_choice_ = static_cast<GhostModeChoice>(index - 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_up);
    }
    if (index >= static_cast<std::uint8_t>(GhostModeChoice::benchmark_race)) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::field_error);
    }
    ghost_mode_choice_ = static_cast<GhostModeChoice>(index + 1U);
    return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                             false, MenuSoundEvent::field_down);
  }

  if (screen_ == FrontEndScreen::car_setup) {
    // p3.1 bank-3 input callback RVA 0x8d094.
    if (direction == MenuDirection::up) {
      const auto index = static_cast<std::uint8_t>(car_setup_field_);
      if (index == 0U) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      car_setup_field_ = static_cast<CarSetupField>(index - 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_up);
    }
    if (direction == MenuDirection::down) {
      const auto index = static_cast<std::uint8_t>(car_setup_field_);
      if (index >= static_cast<std::uint8_t>(CarSetupField::record_race)) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      car_setup_field_ = static_cast<CarSetupField>(index + 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_down);
    }
    if (car_setup_field_ == CarSetupField::car) {
      if (direction == MenuDirection::left) {
        if (car_setup_.car_index == 0U) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false, MenuSoundEvent::select_error);
        }
        --car_setup_.car_index;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::select_left);
      }
      if (direction == MenuDirection::right) {
        if (car_setup_.car_index + 1U >= car_setup_.car_count) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false, MenuSoundEvent::select_error);
        }
        ++car_setup_.car_index;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::select_right);
      }
    }
    if (car_setup_field_ == CarSetupField::transmission) {
      if (direction == MenuDirection::left) {
        if (!car_setup_.automatic_transmission) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false,
                                   MenuSoundEvent::catch_up_error);
        }
        car_setup_.automatic_transmission = false;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::catch_up_off);
      }
      if (direction == MenuDirection::right) {
        if (car_setup_.automatic_transmission) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false,
                                   MenuSoundEvent::catch_up_error);
        }
        car_setup_.automatic_transmission = true;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::catch_up_on);
      }
    }
    if (car_setup_field_ == CarSetupField::horn) {
      if (direction == MenuDirection::left) {
        if (car_setup_.horn_index == 0U) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false, MenuSoundEvent::select_error);
        }
        --car_setup_.horn_index;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::select_left);
      }
      if (direction == MenuDirection::right) {
        if (car_setup_.horn_index + 1U >= car_setup_.horn_count) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false, MenuSoundEvent::select_error);
        }
        ++car_setup_.horn_index;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::select_right);
      }
    }
    if (car_setup_field_ == CarSetupField::record_race) {
      if (direction == MenuDirection::left) {
        if (!car_setup_.record_race) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false,
                                   MenuSoundEvent::catch_up_error);
        }
        car_setup_.record_race = false;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::catch_up_off);
      }
      if (direction == MenuDirection::right) {
        if (car_setup_.record_race) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false,
                                   MenuSoundEvent::catch_up_error);
        }
        car_setup_.record_race = true;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::catch_up_on);
      }
    }
    return {MenuTransitionEvidence::confirmed_no_change, false, false};
  }
  if (screen_ == FrontEndScreen::race_setup) {
    // p3.1 bank-2 input callback RVA 0x8c33c. Up/Down selects fields;
    // Left/Right edits the selected value. Time Attack has no catch-up row.
    if (direction == MenuDirection::up) {
      const auto index = static_cast<std::uint8_t>(race_setup_field_);
      if (index == 0U) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      race_setup_field_ = static_cast<RaceSetupField>(index - 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_up);
    }
    if (direction == MenuDirection::down) {
      const auto index = static_cast<std::uint8_t>(race_setup_field_);
      const auto maximum =
          race_setup_.mode == RaceSetupMode::time_attack
              ? static_cast<std::uint8_t>(RaceSetupField::laps)
              : static_cast<std::uint8_t>(RaceSetupField::catch_up);
      if (index >= maximum) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      race_setup_field_ = static_cast<RaceSetupField>(index + 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_down);
    }
    if (race_setup_field_ == RaceSetupField::track) {
      if (direction == MenuDirection::left) {
        if (race_setup_.track_index == 0U) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false, MenuSoundEvent::select_error);
        }
        --race_setup_.track_index;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::select_left);
      }
      if (direction == MenuDirection::right) {
        if (race_setup_.track_index + 1U >= race_setup_.track_count) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false, MenuSoundEvent::select_error);
        }
        ++race_setup_.track_index;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::select_right);
      }
    }
    if (race_setup_field_ == RaceSetupField::laps) {
      const auto index = race_lap_index(race_setup_.laps);
      if (direction == MenuDirection::left) {
        if (index == 0U) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false,
                                   MenuSoundEvent::laps_left_error);
        }
        race_setup_.laps = race_lap_values[index - 1U];
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::laps_left);
      }
      if (direction == MenuDirection::right) {
        if (index + 1U >= race_lap_values.size()) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false,
                                   MenuSoundEvent::laps_right_error);
        }
        race_setup_.laps = race_lap_values[index + 1U];
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::laps_right);
      }
    }
    if (race_setup_field_ == RaceSetupField::catch_up) {
      if (direction == MenuDirection::left) {
        if (!race_setup_.catch_up) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false,
                                   MenuSoundEvent::catch_up_error);
        }
        race_setup_.catch_up = false;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::catch_up_off);
      }
      if (direction == MenuDirection::right) {
        if (race_setup_.catch_up) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false,
                                   MenuSoundEvent::catch_up_error);
        }
        race_setup_.catch_up = true;
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::catch_up_on);
      }
    }
    return {MenuTransitionEvidence::confirmed_no_change, false, false};
  }
  if (screen_ == FrontEndScreen::one_player) {
    // p3.1 RVA 0x8a3f0 polls only DIK_LEFT, DIK_RIGHT, DIK_RETURN, and
    // DIK_ESCAPE. Its five-choice selector is clamped at both ends.
    if (direction == MenuDirection::up || direction == MenuDirection::down) {
      return {MenuTransitionEvidence::confirmed_no_change, false, false};
    }
    const auto index = static_cast<std::uint8_t>(one_player_choice_);
    if (direction == MenuDirection::left) {
      if (index == 0U) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::arrow_error);
      }
      one_player_choice_ = static_cast<OnePlayerChoice>(index - 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_left);
    }
    if (direction == MenuDirection::right) {
      if (index >= static_cast<std::uint8_t>(OnePlayerChoice::ghost_mode)) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::arrow_error);
      }
      one_player_choice_ = static_cast<OnePlayerChoice>(index + 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_right);
    }
    return {};
  }
  if (screen_ == FrontEndScreen::league_summary) {
    // p3.1 RVA 0x8b13c polls Return and Escape only.
    return {MenuTransitionEvidence::confirmed_no_change, false, false};
  }
  if (screen_ == FrontEndScreen::league_select) {
    // p3.1 RVA 0x8b5a0 polls DIK_UP/DIK_DOWN and clamps to the recovered
    // league count before calling its current-league setter.
    if (direction == MenuDirection::left || direction == MenuDirection::right) {
      return {MenuTransitionEvidence::confirmed_no_change, false, false};
    }
    if (direction == MenuDirection::up) {
      if (league_index_ == 0U) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      --league_index_;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_up);
    }
    if (league_index_ + 1U >= league_count_) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::field_error);
    }
    ++league_index_;
    return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                             false, MenuSoundEvent::field_down);
  }
  if (screen_ == FrontEndScreen::league_delete) {
    // Delete League reuses the same current-league list law at RVA 0x8bbb4.
    if (direction == MenuDirection::left || direction == MenuDirection::right) {
      return {MenuTransitionEvidence::confirmed_no_change, false, false};
    }
    if (direction == MenuDirection::up) {
      if (league_index_ == 0U) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      --league_index_;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_up);
    }
    if (league_index_ + 1U >= league_count_) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::field_error);
    }
    ++league_index_;
    return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                             false, MenuSoundEvent::field_down);
  }
  if (screen_ == FrontEndScreen::league_create) {
    // New League input RVA 0x8b908 has exactly two clamped vertical fields.
    if (direction == MenuDirection::left || direction == MenuDirection::right) {
      if (league_create_field_ != LeagueCreateField::division) {
        return {MenuTransitionEvidence::confirmed_no_change, false, false};
      }
      if (direction == MenuDirection::left) {
        if (league_create_division_index_ == 0U) {
          return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                   false, false, MenuSoundEvent::field_error);
        }
        --league_create_division_index_;
        return result_with_sound(MenuTransitionEvidence::confirmed_change,
                                 true, false, MenuSoundEvent::field_left);
      }
      if (league_create_division_index_ + 1U >=
          league_create_division_count_) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      ++league_create_division_index_;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_right);
    }
    if (direction == MenuDirection::up) {
      if (league_create_field_ == LeagueCreateField::name) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      league_create_field_ = LeagueCreateField::name;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_up);
    }
    if (league_create_field_ == LeagueCreateField::division) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::field_error);
    }
    league_create_field_ = LeagueCreateField::division;
    return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                             false, MenuSoundEvent::field_down);
  }
  if (screen_ == FrontEndScreen::league_overview) {
    // p3.1 RVA 0x8a8b8 polls only DIK_LEFT, DIK_RIGHT, DIK_RETURN, and
    // DIK_ESCAPE. The five League-hub choices are clamped at both ends.
    if (direction == MenuDirection::up || direction == MenuDirection::down) {
      return {MenuTransitionEvidence::confirmed_no_change, false, false};
    }
    const auto index = static_cast<std::uint8_t>(league_menu_choice_);
    if (direction == MenuDirection::left) {
      if (index == 0U) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::arrow_error);
      }
      league_menu_choice_ = static_cast<LeagueMenuChoice>(index - 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_left);
    }
    if (direction == MenuDirection::right) {
      if (index >= static_cast<std::uint8_t>(LeagueMenuChoice::delete_league)) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::arrow_error);
      }
      league_menu_choice_ = static_cast<LeagueMenuChoice>(index + 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_right);
    }
    return {};
  }
  if (screen_ == FrontEndScreen::options) {
    // The reconstruction's Options-only dial extends the original horizontal
    // selector to six clockwise choices and still does not poll Up or Down.
    if (direction == MenuDirection::up || direction == MenuDirection::down) {
      return {MenuTransitionEvidence::confirmed_no_change, false, false};
    }
    const auto index = static_cast<std::uint8_t>(options_choice_);
    if (direction == MenuDirection::left) {
      if (index == 0U) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::arrow_error);
      }
      options_choice_ = static_cast<OptionsMenuChoice>(index - 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_left);
    }
    if (direction == MenuDirection::right) {
      if (index >= static_cast<std::uint8_t>(OptionsMenuChoice::difficulty)) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::arrow_error);
      }
      options_choice_ = static_cast<OptionsMenuChoice>(index + 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_right);
    }
    return {};
  }
  if (screen_ == FrontEndScreen::personal_options) {
    // Exact selector/value ownership recovered from p3.1 RVA
    // 0x85754..0x85ca9. Rows 15..17 only exist while CustomCarColours is On.
    const auto boundary = [](const MenuSoundEvent sound) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, sound);
    };
    const auto changed = [](const MenuSoundEvent sound) {
      auto result = result_with_sound(MenuTransitionEvidence::confirmed_change,
                                      true, false, sound);
      result.personal_settings_changed = true;
      return result;
    };
    const auto index = static_cast<std::uint8_t>(personal_options_field_);
    const auto last =
        personal_options_.custom_car_colours
            ? static_cast<std::uint8_t>(PersonalOptionsField::car_colour_3)
            : static_cast<std::uint8_t>(PersonalOptionsField::car_colours);
    if (direction == MenuDirection::up) {
      if (index == 0U) {
        return boundary(MenuSoundEvent::field_error);
      }
      personal_options_field_ = static_cast<PersonalOptionsField>(index - 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_up);
    }
    if (direction == MenuDirection::down) {
      if (index >= last) {
        return boundary(MenuSoundEvent::field_error);
      }
      personal_options_field_ = static_cast<PersonalOptionsField>(index + 1U);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_down);
    }

    const auto selected = personal_options_field_;
    if (selected == PersonalOptionsField::colour) {
      step_personal_colour(personal_options_.player_colour, direction);
      return changed(direction == MenuDirection::left
                         ? MenuSoundEvent::select_left
                         : MenuSoundEvent::select_right);
    }
    if (selected == PersonalOptionsField::system) {
      if (direction == MenuDirection::left) {
        if (personal_options_.measurement_system == MeasurementSystem::metric) {
          return boundary(MenuSoundEvent::select_error);
        }
        personal_options_.measurement_system = MeasurementSystem::metric;
        return changed(MenuSoundEvent::select_left);
      }
      if (personal_options_.measurement_system == MeasurementSystem::imperial) {
        return boundary(MenuSoundEvent::select_error);
      }
      personal_options_.measurement_system = MeasurementSystem::imperial;
      return changed(MenuSoundEvent::select_right);
    }
    if (selected == PersonalOptionsField::car_colours) {
      if (direction == MenuDirection::left) {
        if (!personal_options_.custom_car_colours) {
          return boundary(MenuSoundEvent::select_error);
        }
        personal_options_.custom_car_colours = false;
        personal_options_field_ = PersonalOptionsField::car_colours;
        return changed(MenuSoundEvent::select_left);
      }
      if (personal_options_.custom_car_colours) {
        return boundary(MenuSoundEvent::select_error);
      }
      personal_options_.custom_car_colours = true;
      return changed(MenuSoundEvent::select_right);
    }
    if (index >=
            static_cast<std::uint8_t>(PersonalOptionsField::car_colour_1) &&
        index <=
            static_cast<std::uint8_t>(PersonalOptionsField::car_colour_3) &&
        personal_options_.custom_car_colours) {
      auto &colour =
          personal_options_
              .car_colours[index - static_cast<std::uint8_t>(
                                       PersonalOptionsField::car_colour_1)];
      step_personal_colour(colour, direction);
      return changed(direction == MenuDirection::left
                         ? MenuSoundEvent::select_left
                         : MenuSoundEvent::select_right);
    }
    return boundary(MenuSoundEvent::select_error);
  }
  if (screen_ == FrontEndScreen::control_options) {
    // p3.1 RVA 0x887f4..0x88f97 owns two focus regions. The profile list
    // changes the active .clo layout; Right enters the fourteen-row editor.
    const auto boundary = [](const MenuSoundEvent sound) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, sound);
    };
    const auto changed = [](const MenuSoundEvent sound,
                            const bool settings_changed = false) {
      auto result = result_with_sound(MenuTransitionEvidence::confirmed_change,
                                      true, false, sound);
      result.control_settings_changed = settings_changed;
      return result;
    };
    if (control_binding_capture_active_) {
      return {MenuTransitionEvidence::confirmed_no_change, false, false};
    }
    if (control_options_focus_ == ControlOptionsFocus::profile_list) {
      if (direction == MenuDirection::up) {
        if (control_options_.profile_index == 0U) {
          return boundary(MenuSoundEvent::field_error);
        }
        --control_options_.profile_index;
        return changed(MenuSoundEvent::field_up, true);
      }
      if (direction == MenuDirection::down) {
        if (control_options_.profile_index + 1U >=
            control_options_.profiles.size()) {
          return boundary(MenuSoundEvent::field_error);
        }
        ++control_options_.profile_index;
        return changed(MenuSoundEvent::field_down, true);
      }
      if (direction == MenuDirection::left) {
        return boundary(MenuSoundEvent::field_navigation_error);
      }
      control_options_focus_ = ControlOptionsFocus::field_list;
      return changed(MenuSoundEvent::field_right);
    }

    const auto index = static_cast<std::uint8_t>(control_options_field_);
    if (direction == MenuDirection::up) {
      if (index == 0U) {
        return boundary(MenuSoundEvent::field_error);
      }
      control_options_field_ = static_cast<ControlOptionsField>(index - 1U);
      return changed(MenuSoundEvent::field_up);
    }
    if (direction == MenuDirection::down) {
      if (index >= static_cast<std::uint8_t>(ControlOptionsField::game_menu)) {
        return boundary(MenuSoundEvent::field_error);
      }
      control_options_field_ = static_cast<ControlOptionsField>(index + 1U);
      return changed(MenuSoundEvent::field_down);
    }
    if (control_options_field_ == ControlOptionsField::force_feedback) {
      auto &enabled = control_options_.profiles[control_options_.profile_index]
                          .force_feedback;
      if (direction == MenuDirection::left) {
        if (!enabled) {
          return boundary(MenuSoundEvent::select_error);
        }
        enabled = false;
        return changed(MenuSoundEvent::select_left, true);
      }
      if (enabled) {
        return boundary(MenuSoundEvent::select_error);
      }
      enabled = true;
      return changed(MenuSoundEvent::select_right, true);
    }
    if (index <=
        static_cast<std::uint8_t>(ControlOptionsField::mouse_speed_z)) {
      // Percentage controls use a deterministic 5% step per key event. This
      // makes exact values selectable without depending on frame timing.
      return adjust_control_analog(direction, 1U);
    }
    if (direction == MenuDirection::left) {
      control_options_focus_ = ControlOptionsFocus::profile_list;
      return changed(MenuSoundEvent::field_left);
    }
    return boundary(MenuSoundEvent::field_navigation_error);
  }
  if (screen_ == FrontEndScreen::graphic_options) {
    // p3.1 Graphic Options input RVA 0x87330 clamps its selector against the
    // field count written by renderer RVA 0x85f2c. Gameplay-facing interface
    // controls live on the reconstruction-owned Gameplay Options page.
    const auto index = static_cast<std::uint8_t>(graphic_options_field_);
    if (direction == MenuDirection::up) {
      if (index == 0U) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      const auto previous =
          graphic_options_.detail_mode != GraphicDetailMode::custom &&
                  index == static_cast<std::uint8_t>(
                               GraphicOptionsField::motion_blur)
              ? static_cast<std::uint8_t>(GraphicOptionsField::graphic_detail)
              : static_cast<std::uint8_t>(index - 1U);
      graphic_options_field_ = static_cast<GraphicOptionsField>(previous);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_up);
    }
    if (direction == MenuDirection::down) {
      const auto last = GraphicOptionsField::motion_blur;
      if (index >= static_cast<std::uint8_t>(last)) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      const auto next =
          graphic_options_.detail_mode != GraphicDetailMode::custom &&
                  index >= static_cast<std::uint8_t>(
                               GraphicOptionsField::graphic_detail) &&
                  index < static_cast<std::uint8_t>(
                              GraphicOptionsField::motion_blur)
              ? static_cast<std::uint8_t>(GraphicOptionsField::motion_blur)
              : static_cast<std::uint8_t>(index + 1U);
      graphic_options_field_ = static_cast<GraphicOptionsField>(next);
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::field_down);
    }

    // The two common p3.1 value helpers at RVAs 0x85258/0x8527c increment or
    // decrement an enum, clamp to its bounds, and emit events 13/12/15.
    const auto boundary = [] {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::select_error);
    };
    const auto changed = [direction] {
      auto result = result_with_sound(
          MenuTransitionEvidence::confirmed_change, true, false,
          direction == MenuDirection::left ? MenuSoundEvent::select_left
                                           : MenuSoundEvent::select_right);
      result.graphic_settings_changed = true;
      return result;
    };
    const auto step_enum = [direction, &boundary, &changed](auto &value,
                                                            const auto last) {
      using Value = std::remove_reference_t<decltype(value)>;
      const auto current = static_cast<std::uint8_t>(value);
      if (direction == MenuDirection::left) {
        if (current == 0U) {
          return boundary();
        }
        value = static_cast<Value>(current - 1U);
        return changed();
      }
      if (current >= static_cast<std::uint8_t>(last)) {
        return boundary();
      }
      value = static_cast<Value>(current + 1U);
      return changed();
    };
    const auto step_bool = [direction, &boundary, &changed](bool &value) {
      if (direction == MenuDirection::left) {
        if (!value) {
          return boundary();
        }
        value = false;
        return changed();
      }
      if (value) {
        return boundary();
      }
      value = true;
      return changed();
    };

    switch (graphic_options_field_) {
    case GraphicOptionsField::render_device:
      return step_enum(graphic_options_.renderer_backend,
                       GraphicRendererBackend::software);
    case GraphicOptionsField::screen_size: {
      const auto tier = graphic_screen_size_tier(graphic_options_.screen_size);
      if (direction == MenuDirection::left) {
        if (tier == 0U) {
          return boundary();
        }
        graphic_options_.screen_size = graphic_screen_size_for_aspect(
            graphic_options_.aspect_ratio, tier - 1U);
      } else {
        if (tier >= 4U) {
          return boundary();
        }
        graphic_options_.screen_size = graphic_screen_size_for_aspect(
            graphic_options_.aspect_ratio, tier + 1U);
      }
      return changed();
    }
    case GraphicOptionsField::aspect_ratio: {
      const auto tier = graphic_screen_size_tier(graphic_options_.screen_size);
      const auto result = step_enum(graphic_options_.aspect_ratio,
                                    GraphicAspectRatio::widescreen_16_9);
      if (result.selection_changed) {
        graphic_options_.screen_size =
            graphic_screen_size_for_aspect(graphic_options_.aspect_ratio, tier);
      }
      return result;
    }
    case GraphicOptionsField::window_mode:
      return step_enum(graphic_options_.window_mode,
                       GraphicWindowMode::fullscreen);
    case GraphicOptionsField::true_colour:
      return step_bool(graphic_options_.true_colour);
    case GraphicOptionsField::triple_buffer:
      return step_bool(graphic_options_.triple_buffer);
    case GraphicOptionsField::trilinear_filtering:
      return step_bool(graphic_options_.trilinear_filtering);
    case GraphicOptionsField::texture_format:
      return step_enum(graphic_options_.texture_format,
                       GraphicTextureFormat::true_colour);
    case GraphicOptionsField::brightness:
      // Percentage controls use one deterministic 5% step per key event.
      return adjust_graphic_held(direction, 1U);
    case GraphicOptionsField::view_distance:
      // View Distance moves by 5% per key event and remains easy to set exactly
      // independent of FPS.
      return adjust_graphic_held(direction, 1U);
    case GraphicOptionsField::graphic_detail:
      // Retail changes only DetailMode here. The runtime resolves the selected
      // profile; hidden Custom fields retain their independently saved values.
      return step_enum(graphic_options_.detail_mode, GraphicDetailMode::custom);
    case GraphicOptionsField::lens_flares:
      return step_bool(graphic_options_.lens_flares);
    case GraphicOptionsField::sparks:
      return step_bool(graphic_options_.sparks);
    case GraphicOptionsField::smoke:
      return step_bool(graphic_options_.smoke);
    case GraphicOptionsField::halos:
      return step_bool(graphic_options_.halos);
    case GraphicOptionsField::shadows:
      return step_bool(graphic_options_.shadows);
    case GraphicOptionsField::skid_marks:
      return step_bool(graphic_options_.skid_marks);
    case GraphicOptionsField::name_plates:
      return step_enum(graphic_options_.name_plates,
                       GraphicNamePlateMode::transparent);
    case GraphicOptionsField::background:
      return step_bool(graphic_options_.background);
    case GraphicOptionsField::track_detail:
      return step_enum(graphic_options_.track_detail, GraphicTrackDetail::high);
    case GraphicOptionsField::car_detail:
      return step_enum(graphic_options_.car_detail, GraphicCarDetail::high);
    case GraphicOptionsField::car_shading:
      // Renderer type 1 retains the fourth Glenz choice at RVA 0x87c9e.
      return step_enum(graphic_options_.car_shading, GraphicCarShading::glenz);
    case GraphicOptionsField::motion_blur:
      return step_bool(graphic_options_.motion_blur);
    case GraphicOptionsField::z_read:
      return step_bool(graphic_options_.z_read);
    }
    return {};
  }
  if (screen_ == FrontEndScreen::gameplay_options) {
    const auto index = static_cast<std::uint8_t>(gameplay_options_field_);
    if (direction == MenuDirection::up || direction == MenuDirection::down) {
      const auto last = static_cast<std::uint8_t>(GameplayOptionsField::ui_scale);
      if ((direction == MenuDirection::up && index == 0U) ||
          (direction == MenuDirection::down && index == last)) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::field_error);
      }
      gameplay_options_field_ = static_cast<GameplayOptionsField>(
          direction == MenuDirection::up ? index - 1U : index + 1U);
      return result_with_sound(
          MenuTransitionEvidence::confirmed_change, true, false,
          direction == MenuDirection::up ? MenuSoundEvent::field_up
                                         : MenuSoundEvent::field_down);
    }
    const auto boundary = [] {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::select_error);
    };
    const auto changed = [direction] {
      auto result = result_with_sound(
          MenuTransitionEvidence::confirmed_change, true, false,
          direction == MenuDirection::left ? MenuSoundEvent::select_left
                                           : MenuSoundEvent::select_right);
      result.graphic_settings_changed = true;
      return result;
    };
    const auto step_enum = [direction, &boundary, &changed](auto &value,
                                                            const auto last) {
      using Value = std::remove_reference_t<decltype(value)>;
      const auto current = static_cast<std::uint8_t>(value);
      if ((direction == MenuDirection::left && current == 0U) ||
          (direction == MenuDirection::right &&
           current >= static_cast<std::uint8_t>(last))) {
        return boundary();
      }
      value = static_cast<Value>(direction == MenuDirection::left
                                     ? current - 1U
                                     : current + 1U);
      return changed();
    };
    switch (gameplay_options_field_) {
    case GameplayOptionsField::info_detail:
      return step_enum(graphic_options_.info_detail, GraphicInfoMode::all);
    case GameplayOptionsField::info_map:
      return step_enum(graphic_options_.info_map, GraphicInfoMode::all);
    case GameplayOptionsField::checkpoint_info:
      return step_enum(graphic_options_.checkpoint_info, GraphicInfoMode::all);
    case GameplayOptionsField::checkpoint_display_time:
    case GameplayOptionsField::ui_scale:
      return adjust_gameplay_held(direction, 1U);
    case GameplayOptionsField::camera_shake:
      if ((direction == MenuDirection::left && !graphic_options_.camera_shake) ||
          (direction == MenuDirection::right && graphic_options_.camera_shake)) {
        return boundary();
      }
      graphic_options_.camera_shake = direction == MenuDirection::right;
      return changed();
    }
  }
  if (screen_ == FrontEndScreen::sound_options) {
    // Recovered p3.1 input owner RVA 0x89744..0x89d46. Unlike the other
    // option pages, the bottom CD-track field uses Up/Down to walk the song
    // list until the first row is reached.
    const auto boundary = [](const MenuSoundEvent sound) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, sound);
    };
    const auto changed = [](const MenuSoundEvent sound) {
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, sound);
    };
    const auto song_count =
        static_cast<std::uint32_t>(sound_options_.assigned_cd_tracks.size());

    if (direction == MenuDirection::up) {
      if (sound_options_field_ == SoundOptionsField::cd_track &&
          sound_options_.song_index != 0U) {
        --sound_options_.song_index;
        return changed(MenuSoundEvent::field_up);
      }
      const auto index = static_cast<std::uint8_t>(sound_options_field_);
      if (index == 0U) {
        return boundary(MenuSoundEvent::field_error);
      }
      sound_options_field_ = static_cast<SoundOptionsField>(index - 1U);
      return changed(MenuSoundEvent::field_up);
    }
    if (direction == MenuDirection::down) {
      if (sound_options_field_ == SoundOptionsField::cd_track) {
        if (sound_options_.song_index + 1U >= song_count) {
          return boundary(MenuSoundEvent::field_error);
        }
        ++sound_options_.song_index;
        return changed(MenuSoundEvent::field_down);
      }
      const auto index = static_cast<std::uint8_t>(sound_options_field_);
      if (index >= static_cast<std::uint8_t>(SoundOptionsField::cd_track)) {
        return boundary(MenuSoundEvent::field_error);
      }
      sound_options_field_ = static_cast<SoundOptionsField>(index + 1U);
      return changed(MenuSoundEvent::field_down);
    }

    if (sound_options_field_ == SoundOptionsField::sound_device) {
      auto &index = sound_options_.output_device_index;
      const auto count = sound_options_.output_devices.size();
      if ((direction == MenuDirection::left && index == 0U) ||
          (direction == MenuDirection::right && index + 1U >= count)) {
        return boundary(MenuSoundEvent::select_error);
      }
      if (direction == MenuDirection::left) {
        --index;
      } else {
        ++index;
      }
      auto result = changed(direction == MenuDirection::left
                                ? MenuSoundEvent::volume_left
                                : MenuSoundEvent::volume_right);
      result.sound_settings_changed = true;
      return result;
    }

    if (sound_options_field_ == SoundOptionsField::sound_effects_volume ||
        sound_options_field_ == SoundOptionsField::cd_music_volume) {
      auto &volume =
          sound_options_field_ == SoundOptionsField::sound_effects_volume
              ? sound_options_.sound_effects_volume
              : sound_options_.cd_music_volume;
      constexpr std::uint32_t half_steps = 20U;
      const auto level = static_cast<std::uint32_t>(
          std::lround(static_cast<double>(volume) * half_steps / 255.0));
      if (direction == MenuDirection::left) {
        if (level == 0U) {
          return boundary(MenuSoundEvent::volume_error);
        }
        volume = static_cast<std::uint8_t>(
            ((level - 1U) * 255U + half_steps / 2U) / half_steps);
        auto result = changed(MenuSoundEvent::volume_left);
        result.sound_settings_changed = true;
        return result;
      }
      if (level >= half_steps) {
        return boundary(MenuSoundEvent::volume_error);
      }
      volume = static_cast<std::uint8_t>(
          ((level + 1U) * 255U + half_steps / 2U) / half_steps);
      auto result = changed(MenuSoundEvent::volume_right);
      result.sound_settings_changed = true;
      return result;
    }

    if (sound_options_field_ == SoundOptionsField::cd_loop) {
      if (direction == MenuDirection::left) {
        if (!sound_options_.cd_loop) {
          return boundary(MenuSoundEvent::catch_up_error);
        }
        sound_options_.cd_loop = false;
        auto result = changed(MenuSoundEvent::catch_up_off);
        result.sound_settings_changed = true;
        return result;
      }
      if (sound_options_.cd_loop) {
        return boundary(MenuSoundEvent::catch_up_error);
      }
      sound_options_.cd_loop = true;
      auto result = changed(MenuSoundEvent::catch_up_on);
      result.sound_settings_changed = true;
      return result;
    }

    if (sound_options_.song_index < sound_options_.fixed_cd_track_count) {
      return boundary(MenuSoundEvent::select_error);
    }
    auto &track = sound_options_.assigned_cd_tracks[sound_options_.song_index];
    if (direction == MenuDirection::left) {
      if (track == sound_options_.first_cd_track) {
        return boundary(MenuSoundEvent::laps_left_error);
      }
      --track;
      auto result = changed(MenuSoundEvent::laps_left);
      result.sound_settings_changed = true;
      return result;
    }
    if (track == sound_options_.last_cd_track) {
      return boundary(MenuSoundEvent::laps_right_error);
    }
    ++track;
    auto result = changed(MenuSoundEvent::laps_right);
    result.sound_settings_changed = true;
    return result;
  }
  if (screen_ == FrontEndScreen::credits) {
    // Credits input RVA 0x8d7d0 polls only Escape and Return.
    return {MenuTransitionEvidence::confirmed_no_change, false, false};
  }
  if (screen_ != FrontEndScreen::main_menu) {
    return {};
  }

  // The p3.1 main-menu handler at RVA 0x812e0 polls only DIK_LEFT,
  // DIK_RIGHT, DIK_RETURN, and DIK_ESCAPE. Up and Down therefore have no
  // effect at every main-menu selection, not merely at the two positions
  // exercised by the capture.
  if (direction == MenuDirection::up || direction == MenuDirection::down) {
    return {MenuTransitionEvidence::confirmed_no_change, false, false};
  }

  if (direction == MenuDirection::right) {
    switch (main_choice_) {
    case MainMenuChoice::one_player:
      main_choice_ = MainMenuChoice::multi_player;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_right);
    case MainMenuChoice::multi_player:
      main_choice_ = MainMenuChoice::rankings;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_right);
    case MainMenuChoice::rankings:
      main_choice_ = MainMenuChoice::options;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_right);
    case MainMenuChoice::options:
      main_choice_ = MainMenuChoice::credits;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_right);
    case MainMenuChoice::credits:
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::arrow_error);
    }
  }

  if (direction == MenuDirection::left) {
    switch (main_choice_) {
    case MainMenuChoice::one_player:
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::arrow_error);
    case MainMenuChoice::multi_player:
      main_choice_ = MainMenuChoice::one_player;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_left);
    case MainMenuChoice::rankings:
      main_choice_ = MainMenuChoice::multi_player;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_left);
    case MainMenuChoice::options:
      main_choice_ = MainMenuChoice::rankings;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_left);
    case MainMenuChoice::credits:
      main_choice_ = MainMenuChoice::options;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::arrow_left);
    }
  }

  return {};
}

MenuTransitionResult FrontEndState::confirm() noexcept {
  if (exit_confirmation_open_) {
    if (exit_confirmation_choice_ == ExitConfirmationChoice::yes) {
      exit_confirmation_open_ = false;
      return result_with_action(FrontEndAction::exit_application,
                                MenuSoundEvent::select,
                                MenuSoundEvent::page_out);
    }
    exit_confirmation_open_ = false;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, true,
                              false, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (league_create_confirmation_open_) {
    league_create_confirmation_open_ = false;
    screen_ = FrontEndScreen::league_overview;
    if (league_create_confirmation_choice_ == ExitConfirmationChoice::no) {
      return result_with_sounds(MenuTransitionEvidence::confirmed_change, true,
                                true, MenuSoundEvent::select,
                                MenuSoundEvent::page_out);
    }
    ++league_count_;
    league_index_ = league_count_ - 1U;
    auto result =
        result_with_sounds(MenuTransitionEvidence::confirmed_change, true, true,
                           MenuSoundEvent::select, MenuSoundEvent::page_out);
    result.create_league = true;
    return result;
  }
  if (league_delete_confirmation_open_) {
    league_delete_confirmation_open_ = false;
    if (league_delete_confirmation_choice_ == ExitConfirmationChoice::no) {
      return result_with_sounds(MenuTransitionEvidence::confirmed_change, true,
                                false, MenuSoundEvent::select,
                                MenuSoundEvent::page_out);
    }
    if (league_count_ <= 1U) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::select_error);
    }
    --league_count_;
    if (league_index_ >= league_count_) {
      league_index_ = league_count_ - 1U;
    }
    screen_ = FrontEndScreen::league_overview;
    auto result =
        result_with_sounds(MenuTransitionEvidence::confirmed_change, true, true,
                           MenuSoundEvent::select, MenuSoundEvent::page_out);
    result.delete_league = true;
    return result;
  }
  if (screen_ == FrontEndScreen::personal_options) {
    // Return at RVA 0x85c60 exits through the Personal Options descriptor.
    screen_ = FrontEndScreen::options;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::control_options) {
    if (control_options_focus_ == ControlOptionsFocus::profile_list) {
      screen_ = FrontEndScreen::options;
      return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                                true, MenuSoundEvent::select,
                                MenuSoundEvent::page_out);
    }
    auto result =
        result_with_sound(MenuTransitionEvidence::confirmed_change, true, false,
                          MenuSoundEvent::binding_capture);
    if (control_options_field_ >= ControlOptionsField::turn_left) {
      control_binding_capture_active_ = true;
      result.capture_control_binding = true;
    }
    return result;
  }
  if (screen_ == FrontEndScreen::sound_options) {
    if (sound_options_field_ == SoundOptionsField::cd_loop) {
      sound_options_.cd_loop = !sound_options_.cd_loop;
      auto result = result_with_sound(
          MenuTransitionEvidence::confirmed_change, true, false,
          sound_options_.cd_loop ? MenuSoundEvent::catch_up_on
                                 : MenuSoundEvent::catch_up_off);
      result.sound_settings_changed = true;
      return result;
    }
    if (sound_options_field_ == SoundOptionsField::cd_track) {
      MenuTransitionResult result{MenuTransitionEvidence::confirmed_no_change,
                                  false, false};
      result.preview_music = true;
      return result;
    }
    return result_with_sound(MenuTransitionEvidence::confirmed_no_change, false,
                             false, MenuSoundEvent::select_error);
  }
  if (screen_ == FrontEndScreen::rankings) {
    // The Rankings input callback polls Escape and the four cursor keys only.
    return {MenuTransitionEvidence::confirmed_no_change, false, false};
  }
  if (screen_ == FrontEndScreen::multiplayer) {
    if (multiplayer_page_ == MultiplayerPage::entry) {
      multiplayer_page_ =
          multiplayer_entry_choice_ == MultiplayerEntryChoice::join
              ? MultiplayerPage::join
              : MultiplayerPage::create;
      multiplayer_join_field_ = MultiplayerJoinField::sessions;
      if (multiplayer_page_ == MultiplayerPage::join) {
        multiplayer_.session_password.clear();
      }
      multiplayer_create_field_ = MultiplayerCreateField::start;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::select);
    }
    if (multiplayer_page_ == MultiplayerPage::join &&
        multiplayer_join_field_ == MultiplayerJoinField::refresh) {
      auto result =
          result_with_sound(MenuTransitionEvidence::confirmed_no_change, false,
                            false, MenuSoundEvent::select);
      result.multiplayer_refresh = true;
      return result;
    }
    if (multiplayer_page_ == MultiplayerPage::join &&
        multiplayer_join_field_ == MultiplayerJoinField::sessions &&
        !multiplayer_.sessions.empty()) {
      const auto index = std::min<std::size_t>(
          multiplayer_.selected_session, multiplayer_.sessions.size() - 1U);
      if (multiplayer_.sessions[index].password_required) {
        prompt_multiplayer_password();
        return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                                 false, MenuSoundEvent::select);
      }
    }
    if (multiplayer_page_ == MultiplayerPage::join &&
        multiplayer_join_field_ == MultiplayerJoinField::password &&
        multiplayer_.session_password.empty()) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::select_error);
    }
    if (multiplayer_page_ == MultiplayerPage::join) {
      auto result =
          result_with_sound(MenuTransitionEvidence::confirmed_no_change, false,
                            false, MenuSoundEvent::select);
      result.multiplayer_join = true;
      return result;
    }
    if (multiplayer_page_ == MultiplayerPage::create &&
        multiplayer_create_field_ == MultiplayerCreateField::start) {
      auto result =
          result_with_sound(MenuTransitionEvidence::confirmed_no_change, false,
                            false, MenuSoundEvent::select);
      result.multiplayer_host = true;
      return result;
    }
    if (multiplayer_page_ == MultiplayerPage::lobby) {
      auto result =
          result_with_sound(MenuTransitionEvidence::confirmed_no_change, false,
                            false, MenuSoundEvent::select);
      if (multiplayer_lobby_field_ == MultiplayerLobbyField::ready) {
        result.multiplayer_ready = true;
      } else {
        result.multiplayer_start = true;
      }
      return result;
    }
    return result_with_sound(MenuTransitionEvidence::confirmed_no_change, false,
                             false, MenuSoundEvent::select_error);
  }
  if (screen_ == FrontEndScreen::main_menu &&
      main_choice_ == MainMenuChoice::one_player) {
    screen_ = FrontEndScreen::one_player;
    one_player_choice_ = OnePlayerChoice::quick_race;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::main_menu &&
      main_choice_ == MainMenuChoice::multi_player) {
    screen_ = FrontEndScreen::multiplayer;
    multiplayer_page_ = MultiplayerPage::entry;
    multiplayer_entry_choice_ = MultiplayerEntryChoice::join;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::main_menu &&
      main_choice_ == MainMenuChoice::rankings) {
    screen_ = FrontEndScreen::rankings;
    rankings_field_ = RankingsField::type;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::main_menu &&
      main_choice_ == MainMenuChoice::options) {
    screen_ = FrontEndScreen::options;
    options_choice_ = OptionsMenuChoice::graphic_options;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::main_menu &&
      main_choice_ == MainMenuChoice::credits) {
    screen_ = FrontEndScreen::credits;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::credits) {
    // Return at RVA 0x8d84c dispatches event 11 and selects the main-screen
    // descriptor. The common screen transition contributes page-out event 3.
    screen_ = FrontEndScreen::main_menu;
    main_choice_ = MainMenuChoice::credits;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::one_player &&
      one_player_choice_ == OnePlayerChoice::quick_race) {
    // The destination descriptor at VA 0x004fa0d8 has no init or render
    // callbacks. Its input callback at RVA 0x80618 writes output value 2;
    // the common transition path emits select and page-out first.
    return result_with_action(FrontEndAction::quick_race,
                              MenuSoundEvent::select, MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::one_player &&
      (one_player_choice_ == OnePlayerChoice::single_race ||
       one_player_choice_ == OnePlayerChoice::time_attack)) {
    // One Player cleanup RVA 0x8a4d8 stores choice-1 in profile field +0x90c.
    // Both choices then enter the same bank-2 descriptor at VA 0x004f9f18.
    race_setup_.mode = one_player_choice_ == OnePlayerChoice::time_attack
                           ? RaceSetupMode::time_attack
                           : RaceSetupMode::single_race;
    race_setup_field_ = RaceSetupField::track;
    screen_ = FrontEndScreen::race_setup;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::one_player &&
      one_player_choice_ == OnePlayerChoice::league_race) {
    if (!one_player_league_available_) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false,
                               MenuSoundEvent::select_error);
    }
    race_setup_.mode = RaceSetupMode::league_race;
    league_menu_choice_ = LeagueMenuChoice::continue_race;
    screen_ = FrontEndScreen::league_overview;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::one_player &&
      one_player_choice_ == OnePlayerChoice::ghost_mode) {
    ghost_mode_choice_ = GhostModeChoice::ghost_race;
    ghost_file_focused_ = false;
    race_setup_.mode = RaceSetupMode::ghost_race;
    screen_ = FrontEndScreen::ghost_setup;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::ghost_setup) {
    if (!ghost_file_focused_) {
      if (ghost_file_count_ == 0U) {
        return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                                 false, false, MenuSoundEvent::select_error);
      }
      ghost_file_focused_ = true;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::select_left);
    }
    if (ghost_mode_choice_ == GhostModeChoice::ghost_race) {
      car_setup_field_ = CarSetupField::car;
      screen_ = FrontEndScreen::car_setup;
      return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                                true, MenuSoundEvent::select,
                                MenuSoundEvent::page_out);
    }
    // Replay Race and Benchmark Race use the same retail output-2 descriptor
    // as Quick Race after the selected demo has been committed.
    return result_with_action(FrontEndAction::start_race,
                              MenuSoundEvent::select, MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::league_overview) {
    if (league_menu_choice_ == LeagueMenuChoice::continue_race ||
        league_menu_choice_ == LeagueMenuChoice::view_stats) {
      // Both choices enter descriptor 0x004f9fc0. Its Return handler uses the
      // originating hub choice to distinguish Continue from View Stats.
      screen_ = FrontEndScreen::league_summary;
      return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                                true, MenuSoundEvent::select,
                                MenuSoundEvent::page_out);
    }
    if (league_menu_choice_ == LeagueMenuChoice::select_league) {
      screen_ = FrontEndScreen::league_select;
      return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                                true, MenuSoundEvent::select,
                                MenuSoundEvent::page_out);
    }
    if (league_menu_choice_ == LeagueMenuChoice::new_league) {
      league_create_field_ = LeagueCreateField::name;
      screen_ = FrontEndScreen::league_create;
      return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                                true, MenuSoundEvent::select,
                                MenuSoundEvent::page_out);
    }
    screen_ = FrontEndScreen::league_delete;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::league_summary) {
    if (league_menu_choice_ == LeagueMenuChoice::continue_race) {
      screen_ = FrontEndScreen::car_setup;
      car_setup_field_ = CarSetupField::car;
    } else {
      screen_ = FrontEndScreen::league_overview;
    }
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::league_select) {
    screen_ = FrontEndScreen::league_overview;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::league_delete) {
    if (league_count_ <= 1U) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::select_error);
    }
    league_delete_confirmation_open_ = true;
    league_delete_confirmation_choice_ = ExitConfirmationChoice::no;
    return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                             false, MenuSoundEvent::requester_open);
  }
  if (screen_ == FrontEndScreen::league_create) {
    // p3.1 RVA 0x8ba4d calls the shared two-choice requester with the
    // authored `Yes, keep it!` / `No` table and default choice one.
    league_create_confirmation_open_ = true;
    league_create_confirmation_choice_ = ExitConfirmationChoice::yes;
    return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                             false, MenuSoundEvent::requester_open);
  }
  if (screen_ == FrontEndScreen::race_setup) {
    if (race_setup_.track_index >= race_setup_.unlocked_track_count) {
      return result_with_sound(MenuTransitionEvidence::confirmed_no_change,
                               false, false, MenuSoundEvent::track_locked);
    }
    screen_ = FrontEndScreen::car_setup;
    car_setup_field_ = CarSetupField::car;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::car_setup) {
    if (car_setup_field_ == CarSetupField::horn) {
      MenuTransitionResult result{MenuTransitionEvidence::confirmed_no_change,
                                  false, false};
      result.preview_horn = true;
      return result;
    }
    if (car_setup_.car_index >= car_setup_.unlocked_car_count) {
      return {MenuTransitionEvidence::confirmed_no_change, false, false};
    }
    // RVA 0x8d3b3 emits select and enters the same terminal output-2
    // descriptor used by Quick Race.
    return result_with_action(FrontEndAction::quick_race,
                              MenuSoundEvent::select, MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::options &&
      options_choice_ == OptionsMenuChoice::difficulty) {
    difficulty_ = static_cast<DifficultyChoice>(
        (static_cast<std::uint8_t>(difficulty_) + 1U) % 3U);
    return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                             false, MenuSoundEvent::difficulty_change);
  }
  if (screen_ == FrontEndScreen::options &&
      options_choice_ == OptionsMenuChoice::personal_options) {
    screen_ = FrontEndScreen::personal_options;
    personal_options_field_ = PersonalOptionsField::name;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::options &&
      options_choice_ == OptionsMenuChoice::graphic_options) {
    screen_ = FrontEndScreen::graphic_options;
    graphic_options_field_ = GraphicOptionsField::render_device;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::options &&
      options_choice_ == OptionsMenuChoice::gameplay_options) {
    screen_ = FrontEndScreen::gameplay_options;
    gameplay_options_field_ = GameplayOptionsField::info_detail;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::options &&
      options_choice_ == OptionsMenuChoice::control_options) {
    screen_ = FrontEndScreen::control_options;
    control_options_focus_ = ControlOptionsFocus::profile_list;
    control_options_field_ = ControlOptionsField::force_feedback;
    control_binding_capture_active_ = false;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::options &&
      options_choice_ == OptionsMenuChoice::sound_options) {
    screen_ = FrontEndScreen::sound_options;
    sound_options_field_ = SoundOptionsField::sound_device;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::select,
                              MenuSoundEvent::page_out);
  }
  return {};
}

MenuTransitionResult FrontEndState::cancel() noexcept {
  if (exit_confirmation_open_) {
    exit_confirmation_open_ = false;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, true,
                              false, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (league_create_confirmation_open_) {
    league_create_confirmation_open_ = false;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, true,
                              false, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (league_delete_confirmation_open_) {
    league_delete_confirmation_open_ = false;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, true,
                              false, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::main_menu) {
    // Main input RVA 0x8138e calls the common requester at RVA 0x80340 with
    // two choices and default result 2 ("No way!"). The requester itself
    // emits menu event 2 as it opens.
    exit_confirmation_open_ = true;
    exit_confirmation_choice_ = ExitConfirmationChoice::no;
    return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                             false, MenuSoundEvent::requester_open);
  }
  if (screen_ == FrontEndScreen::rankings) {
    // Escape at RVA 0x84b51 returns to the main descriptor with Rankings
    // retained as the selected main-screen destination.
    screen_ = FrontEndScreen::main_menu;
    main_choice_ = MainMenuChoice::rankings;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::credits) {
    // Escape at RVA 0x8d7ee selects the same main-screen descriptor while
    // retaining Credits as the active main-menu choice.
    screen_ = FrontEndScreen::main_menu;
    main_choice_ = MainMenuChoice::credits;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::league_summary ||
      screen_ == FrontEndScreen::league_select ||
      screen_ == FrontEndScreen::league_create ||
      screen_ == FrontEndScreen::league_delete) {
    screen_ = FrontEndScreen::league_overview;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::league_overview) {
    screen_ = FrontEndScreen::one_player;
    one_player_choice_ = OnePlayerChoice::league_race;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::car_setup) {
    const auto was_locked =
        car_setup_.car_index >= car_setup_.unlocked_car_count;
    if (was_locked && car_setup_.unlocked_car_count != 0U) {
      car_setup_.car_index = car_setup_.unlocked_car_count - 1U;
    }
    if (race_setup_.mode == RaceSetupMode::league_race) {
      screen_ = FrontEndScreen::league_summary;
    } else if (race_setup_.mode == RaceSetupMode::ghost_race) {
      screen_ = FrontEndScreen::ghost_setup;
      ghost_file_focused_ = true;
    } else {
      screen_ = FrontEndScreen::race_setup;
      race_setup_field_ = RaceSetupField::track;
    }
    if (was_locked) {
      return result_with_three_sounds(MenuTransitionEvidence::confirmed_change,
                                      true, true, MenuSoundEvent::car_locked,
                                      MenuSoundEvent::escape,
                                      MenuSoundEvent::page_out);
    }
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::ghost_setup) {
    if (ghost_file_focused_) {
      ghost_file_focused_ = false;
      ghost_mode_choice_ = GhostModeChoice::ghost_race;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::select_right);
    }
    screen_ = FrontEndScreen::one_player;
    one_player_choice_ = OnePlayerChoice::ghost_mode;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::race_setup) {
    const auto was_locked =
        race_setup_.track_index >= race_setup_.unlocked_track_count;
    if (was_locked && race_setup_.unlocked_track_count != 0U) {
      race_setup_.track_index = race_setup_.unlocked_track_count - 1U;
    }
    screen_ = FrontEndScreen::one_player;
    one_player_choice_ = race_setup_.mode == RaceSetupMode::time_attack
                             ? OnePlayerChoice::time_attack
                             : OnePlayerChoice::single_race;
    if (was_locked) {
      return result_with_three_sounds(MenuTransitionEvidence::confirmed_change,
                                      true, true, MenuSoundEvent::track_locked,
                                      MenuSoundEvent::escape,
                                      MenuSoundEvent::page_out);
    }
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::multiplayer) {
    if (multiplayer_page_ == MultiplayerPage::lobby) {
      leave_multiplayer_lobby();
      auto result = result_with_sound(MenuTransitionEvidence::confirmed_change,
                                      true, false, MenuSoundEvent::escape);
      result.multiplayer_leave = true;
      return result;
    }
    if (multiplayer_page_ == MultiplayerPage::join &&
        multiplayer_join_field_ == MultiplayerJoinField::password) {
      multiplayer_.session_password.clear();
      multiplayer_join_field_ = MultiplayerJoinField::sessions;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::escape);
    }
    if (multiplayer_page_ != MultiplayerPage::entry) {
      multiplayer_page_ = MultiplayerPage::entry;
      return result_with_sound(MenuTransitionEvidence::confirmed_change, true,
                               false, MenuSoundEvent::escape);
    }
    screen_ = FrontEndScreen::main_menu;
    main_choice_ = MainMenuChoice::multi_player;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::one_player) {
    screen_ = FrontEndScreen::main_menu;
    main_choice_ = MainMenuChoice::one_player;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::personal_options) {
    screen_ = FrontEndScreen::options;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::graphic_options) {
    screen_ = FrontEndScreen::options;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::gameplay_options) {
    screen_ = FrontEndScreen::options;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::control_options) {
    control_binding_capture_active_ = false;
    screen_ = FrontEndScreen::options;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::sound_options) {
    screen_ = FrontEndScreen::options;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  if (screen_ == FrontEndScreen::options) {
    screen_ = FrontEndScreen::main_menu;
    main_choice_ = MainMenuChoice::options;
    return result_with_sounds(MenuTransitionEvidence::confirmed_change, false,
                              true, MenuSoundEvent::escape,
                              MenuSoundEvent::page_out);
  }
  return {};
}

std::string_view main_menu_choice_name(const MainMenuChoice choice) noexcept {
  switch (choice) {
  case MainMenuChoice::one_player:
    return "one player";
  case MainMenuChoice::multi_player:
    return "multi player";
  case MainMenuChoice::rankings:
    return "rankings";
  case MainMenuChoice::options:
    return "options";
  case MainMenuChoice::credits:
    return "credits";
  }
  return {};
}

std::string_view one_player_choice_name(const OnePlayerChoice choice) noexcept {
  switch (choice) {
  case OnePlayerChoice::quick_race:
    return "quick race";
  case OnePlayerChoice::single_race:
    return "single race";
  case OnePlayerChoice::league_race:
    return "league race";
  case OnePlayerChoice::time_attack:
    return "time attack";
  case OnePlayerChoice::ghost_mode:
    return "ghost mode";
  }
  return {};
}

std::string_view
league_menu_choice_name(const LeagueMenuChoice choice) noexcept {
  switch (choice) {
  case LeagueMenuChoice::continue_race:
    return "continue";
  case LeagueMenuChoice::view_stats:
    return "view stats";
  case LeagueMenuChoice::select_league:
    return "select league";
  case LeagueMenuChoice::new_league:
    return "new league";
  case LeagueMenuChoice::delete_league:
    return "delete league";
  }
  return {};
}

std::string_view
options_menu_choice_name(const OptionsMenuChoice choice) noexcept {
  switch (choice) {
  case OptionsMenuChoice::personal_options:
    return "personal options";
  case OptionsMenuChoice::graphic_options:
    return "graphic options";
  case OptionsMenuChoice::control_options:
    return "control options";
  case OptionsMenuChoice::sound_options:
    return "sound options";
  case OptionsMenuChoice::gameplay_options:
    return "gameplay options";
  case OptionsMenuChoice::difficulty:
    return "difficulty";
  }
  return {};
}

float main_menu_pointer_target_units(const MainMenuChoice choice) noexcept {
  // For each choice the original computes x * 8 + y from the corresponding
  // bank-0 record. Keeping the results here makes the recovered p3.1 state law
  // independent of private retail content at runtime.
  constexpr std::array targets{
      4638.0F, // 543 * 8 + 294
      3935.0F, // 453 * 8 + 311
      3240.0F, // 368 * 8 + 296
      2568.0F, // 293 * 8 + 224
      1857.0F, // 188 * 8 + 353
  };
  const auto index = static_cast<std::size_t>(choice);
  return index < targets.size() ? targets[index] : targets.front();
}

float one_player_pointer_target_units(const OnePlayerChoice choice) noexcept {
  // p3.1 sprpos.dta bank 10 records 24..28, encoded as x*8+y.
  constexpr std::array targets{
      4626.0F, // 572 * 8 + 50
      3931.0F, // 485 * 8 + 51
      3232.0F, // 398 * 8 + 48
      2552.0F, // 315 * 8 + 32
      1851.0F, // 228 * 8 + 27
  };
  const auto index = static_cast<std::size_t>(choice);
  return index < targets.size() ? targets[index] : targets.front();
}

float league_menu_pointer_target_units(const LeagueMenuChoice choice) noexcept {
  // p3.1 sprpos.dta bank 12 records 24..28, encoded as x*8+y.
  constexpr std::array targets{
      4630.0F, // 578 * 8 + 6
      3927.0F, // 489 * 8 + 15
      3240.0F, // 401 * 8 + 32
      2568.0F, // 317 * 8 + 32
      1857.0F, // 229 * 8 + 25
  };
  const auto index = static_cast<std::size_t>(choice);
  return index < targets.size() ? targets[index] : targets.front();
}

float options_menu_pointer_target_units(
    const OptionsMenuChoice choice) noexcept {
  // Solved from the six mark centres in the authored Options dial.
  // The pointer rasterizer uses 4096 units per turn and compresses its Y axis
  // by 5/6, so screen-space clock angles do not land on the authored marks.
  constexpr std::array targets{
      4636.4F, // Graphic, lower-left mark
      4079.3F, // Sound, left mark
      3409.5F, // Gameplay, upper-left mark
      2734.5F, // Control, upper-right mark
      2064.6F, // Personal, right mark
      1511.8F, // Difficulty, lower-right mark
  };
  const auto index = static_cast<std::size_t>(choice);
  return index < targets.size() ? targets[index] : targets.front();
}

namespace {

float advance_pointer_units(const float current_units, const float target,
                            const std::uint32_t elapsed_ms) noexcept {
  constexpr float speed = 32.0F;
  constexpr double time_scale = 1.0 / 3000.0;
  const auto difference = static_cast<float>(target - current_units);
  const auto updated = static_cast<double>(current_units) +
                       static_cast<double>(difference) *
                           static_cast<double>(elapsed_ms) *
                           static_cast<double>(speed) * time_scale;
  return static_cast<float>(updated);
}

} // namespace

float advance_main_menu_pointer_units(const float current_units,
                                      const MainMenuChoice target_choice,
                                      const std::uint32_t elapsed_ms) noexcept {
  const auto target = main_menu_pointer_target_units(target_choice);
  return advance_pointer_units(current_units, target, elapsed_ms);
}

float advance_one_player_pointer_units(
    const float current_units, const OnePlayerChoice target_choice,
    const std::uint32_t elapsed_ms) noexcept {
  return advance_pointer_units(current_units,
                               one_player_pointer_target_units(target_choice),
                               elapsed_ms);
}

float advance_league_menu_pointer_units(
    const float current_units, const LeagueMenuChoice target_choice,
    const std::uint32_t elapsed_ms) noexcept {
  return advance_pointer_units(current_units,
                               league_menu_pointer_target_units(target_choice),
                               elapsed_ms);
}

float advance_options_menu_pointer_units(
    const float current_units, const OptionsMenuChoice target_choice,
    const std::uint32_t elapsed_ms) noexcept {
  return advance_pointer_units(current_units,
                               options_menu_pointer_target_units(target_choice),
                               elapsed_ms);
}

} // namespace mh::ui
