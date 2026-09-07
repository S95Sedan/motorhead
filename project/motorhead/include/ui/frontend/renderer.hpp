#pragma once

#include <content/formats/fnt_bitmap_font.hpp>
#include <content/formats/lob_line_object.hpp>
#include <content/formats/spr_sprite_archive.hpp>
#include <content/formats/sprite_positions.hpp>
#include <content/formats/tga_image.hpp>
#include <ui/frontend/controller.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mh::ui {

inline constexpr std::uint32_t front_end_logical_width = 640U;
inline constexpr std::uint32_t front_end_logical_height = 400U;
// 640x400 is displayed with the retail 4:3 pixel-aspect correction. Extending
// that corrected canvas to 16:9 without changing the authored horizontal or
// vertical scale requires 853 1/3 logical pixels; 854 provides symmetric
// integer side wings around the original 640-pixel centre.
inline constexpr std::uint32_t front_end_widescreen_logical_width = 854U;

struct FrontEndPresentationRect {
  float x = 0.0F;
  float y = 0.0F;
  float width = 0.0F;
  float height = 0.0F;
};

// The 640x400 software surface is presented through the original 4:3 display
// extent. Modern aspect ratios fit that extent without stretching menu or
// result-screen pixels independently from the race HUD.
[[nodiscard]] FrontEndPresentationRect
front_end_presentation_rect(std::uint32_t output_width,
                            std::uint32_t output_height) noexcept;
[[nodiscard]] FrontEndPresentationRect
front_end_widescreen_presentation_rect(std::uint32_t output_width,
                                       std::uint32_t output_height) noexcept;

// Centre and normalize a menu LOB for the selector projection.
[[nodiscard]] double
front_end_menu_line_object_scale(const mh::content::LobData &line_object,
                                 double target_radius) noexcept;
inline constexpr std::uint32_t main_menu_unselected_label_frame = 0U;
// Frame 4 is the selected animation phase visible in the accepted initial
// One Player reference, not a universal settled frame.
inline constexpr std::uint32_t main_menu_reference_selected_label_frame = 4U;
inline constexpr std::uint32_t front_end_transition_leg_ms = 150U;

struct CenteredMenuListWindow {
  std::size_t first = 0U;
  std::size_t last = 0U;
};

// Shared p3.1 string-list window used by Ghost Mode at RVA 0x7fc5c.
[[nodiscard]] CenteredMenuListWindow
centered_menu_list_window(std::size_t item_count, std::size_t selected_index,
                          std::size_t visible_rows);

// p3.1 RVA 0x80430:
// trunc(256 - min(screen_counter / 150, 1) * 256).
[[nodiscard]] std::uint32_t
front_end_transition_phase(std::uint32_t screen_counter_ms) noexcept;

// Reproduces the p3.1 main-screen selected-label law:
// trunc(3.5 + 3.5 * sin(((elapsed_ms * 8) & 4095) * 2*pi/4096)).
[[nodiscard]] std::uint32_t
main_menu_selected_label_frame(std::uint32_t elapsed_ms) noexcept;

[[nodiscard]] constexpr std::uint32_t
main_menu_label_frame(const MainMenuChoice selection,
                      const MainMenuChoice label) noexcept {
  return selection == label ? main_menu_reference_selected_label_frame
                            : main_menu_unselected_label_frame;
}

struct FrontEndFrame {
  std::uint32_t width = front_end_logical_width;
  std::uint32_t height = front_end_logical_height;
  std::vector<std::uint8_t> rgba;
};

enum class OnePlayerMode : std::uint8_t {
  single_race,
  league_race,
  time_attack,
  ghost_mode,
};

// Persisted values read by the p3.1 One Player renderer from the active
// profile owner. Empty strings suppress only the corresponding value.
struct OnePlayerPanelValues {
  OnePlayerMode mode = OnePlayerMode::single_race;
  std::string car;
  std::string third_value;
  std::string difficulty;
  std::string transmission;
  std::string track;
};

struct LeagueStandingPresentation {
  std::string nickname;
  std::uint32_t score = 0U;
  bool human = false;
};

struct LeagueOverviewPresentation {
  std::string league_name;
  std::string player_name;
  std::string difficulty;
  std::string next_track;
  std::uint32_t division = 0U;
  std::uint32_t score = 0U;
  std::uint32_t races_done = 0U;
  std::string creation_date;
  std::string car_name;
  // Non-rendered local identity used by the shared race handoff.
  std::string source_path;
  std::vector<LeagueStandingPresentation> standings;
};

struct RaceSetupPresentation {
  std::string track_name;
  bool track_unlocked = true;
  bool has_previous_track = false;
  bool has_next_track = false;
};

struct CarSetupPresentation {
  std::string car_name;
  std::string horn_name;
  bool car_unlocked = true;
  bool has_previous_car = false;
  bool has_next_car = false;
  struct Performance {
    float top_speed = 0.0F;
    float acceleration = 0.0F;
    float handling = 0.0F;
  } performance;
};

struct RankingEntry {
  std::string player_name;
  std::string vehicle_name;
  std::uint32_t best_lap_ms = 0U;
  std::uint32_t total_time_ms = 0U;
};

struct RankingsPresentation {
  std::string track_name = "Goldbridge";
  std::vector<RankingEntry> entries;
};

struct RankingsRowPlacement {
  std::int32_t player_x = 0;
  std::int32_t vehicle_x = 0;
  std::int32_t best_lap_x = 0;
  std::int32_t total_time_x = 0;
  std::int32_t y = 0;
};

// p3.1 Rankings renderer RVA 0x84a32..0x84b45. The retail owner stores ten
// 0x38-byte records and advances bank-11 record 16's Y by record 17's Y.
[[nodiscard]] RankingsRowPlacement
rankings_row_placement(std::uint32_t row_index);

struct SoundTrackPresentation {
  std::string name;
  std::uint32_t duration_seconds = 0U;
};

struct SoundOptionsPresentation {
  std::vector<SoundTrackPresentation> tracks{{"Results", 0U},
                                             {"Menu", 0U},
                                             {"Goldbridge", 4U * 60U + 15U},
                                             {"Redrock", 4U * 60U + 26U}};
};

// Single-race result data consumed by the p3.1 ScreenResults reconstruction.
// The portrait is kept separate from this value object so callers can retain
// decoded original assets in a shared content cache.
struct RaceResultEntry {
  std::uint32_t finishing_position = 1U;
  std::string car_name;
  std::string nickname;
  std::int32_t score = 0;
  std::uint32_t race_time_ms = 0U;
  std::uint32_t best_lap_ms = 0U;
};

struct RaceResultVehicleState {
  std::string car_name;
  std::string nickname;
  std::int32_t starting_score = 0;
  std::optional<std::uint32_t> race_time_ms;
  std::int64_t progress_samples = 0;
  std::uint32_t best_lap_ms = 0U;
};

struct RankedRaceResultEntry {
  std::size_t source_index = 0U;
  RaceResultEntry entry;
};

// Shared playable handoff into both p3.1 result compositors. Finished vehicles
// sort by time; unfinished vehicles follow by unwrapped route progress and use
// the retained cutoff time. Equal states retain live-array order.
[[nodiscard]] std::vector<RankedRaceResultEntry>
make_race_result_field(std::span<const RaceResultVehicleState> vehicles,
                       std::uint32_t cutoff_time_ms, bool league_result);

struct RaceResultPlacement {
  std::int32_t portrait_x = 0;
  std::int32_t portrait_y = 0;
  std::int32_t car_name_x = 0;
  std::int32_t car_name_y = 0;
  std::int32_t label_x = 0;
  std::int32_t nick_y = 0;
  std::int32_t score_y = 0;
  std::int32_t race_y = 0;
  std::int32_t best_lap_y = 0;
  std::int32_t value_x = 0;
};

// Exact odd/even column layout recovered from p3.1 RVA
// 0x40420..0x40b64. Positions are one-based and limited to 1..8.
[[nodiscard]] RaceResultPlacement
race_result_placement(std::uint32_t finishing_position);

struct CarSetupPerformanceAnimationState {
  CarSetupPresentation::Performance current;
};

// p3.1 bank-3 renderer smooths runtime-record fields 0x1214/0x1228/0x122c
// toward the selected car with (target-current) * elapsed_ms * 0.005.
[[nodiscard]] CarSetupPresentation::Performance
advance_car_setup_performance(CarSetupPerformanceAnimationState &state,
                              const CarSetupPresentation::Performance &target,
                              std::uint32_t elapsed_ms) noexcept;

// p3.1 front-end ambient layer (RVA 0x8ea80). Nine independently moving
// instances select from Data/LINEOBJ/lobj00.lob through lobj08.lob. Their
// spawn, motion, rotation, perspective, and palette fade follow the retail
// executable; the source meshes remain the original owned-disc assets.
struct FrontEndLineObjectInstance {
  std::uint32_t model_index = 0U;
  float angular_x = 0.0F;
  float angular_y = 0.0F;
  float angular_z = 0.0F;
  float depth = 0.0F;
  float depth_speed = 0.0F;
  std::uint32_t phase_x = 0U;
  std::uint32_t phase_y = 0U;
  std::uint32_t phase_z = 0U;
};

struct FrontEndBackgroundAnimationState {
  std::uint32_t random_state = 1U;
  std::uint32_t elapsed_ms = 0U;
  std::array<FrontEndLineObjectInstance, 9U> objects{};
};

// Retains the unchanged background pixels between ambient animation frames.
struct FrontEndBackgroundRenderCache {
  const mh::content::TgaImage *source = nullptr;
  const mh::content::TgaImage *target = nullptr;
  std::uint32_t width = 0U;
  std::array<std::array<std::uint8_t, 3U>, 256U> palette{};
  std::vector<std::uint8_t> base_indices;
  std::vector<std::uint8_t> base_rgba;
  std::vector<std::uint32_t> dirty_pixels;
  std::vector<std::uint8_t> dirty_marks;
  std::vector<std::array<double, 2U>> projected_vertices;
};

void advance_front_end_background_animation(
    FrontEndBackgroundAnimationState &state, std::uint32_t elapsed_ms) noexcept;

void compose_front_end_animated_background(
    const mh::content::TgaImage &background,
    std::span<const mh::content::LobData> line_objects,
    const FrontEndBackgroundAnimationState &state,
    const std::array<std::array<std::uint8_t, 3U>, 256U> &menu_palette,
    FrontEndBackgroundRenderCache &cache, mh::content::TgaImage &result);

// Reconstruction 16:9 ambient layer. The original 640x400 background remains
// centred while the recovered flying line objects and rotating orbit geometry
// are clipped against the wider canvas at their unchanged scale and timing.
void compose_front_end_widescreen_animated_background(
    const mh::content::TgaImage &background,
    std::span<const mh::content::LobData> line_objects,
    const FrontEndBackgroundAnimationState &state,
    const std::array<std::array<std::uint8_t, 3U>, 256U> &menu_palette,
    FrontEndBackgroundRenderCache &cache, mh::content::TgaImage &result);

// State for the retail main-screen ambient distortion. The original advances
// its rand() generator once for each of ten sprite slots on every rendered
// frame. Slots 2 (screen badge) and 5 (MAIN plate) can start a 16-step
// scanline-remapping pulse when the generated value is below 10.
struct MainMenuGlitchState {
  std::uint32_t random_state = 1U;
  std::uint32_t selector_random_state = 1U;
  std::array<std::int32_t, 32U> countdown{};
  std::uint32_t selector_delay_ms = 0U;
  std::uint32_t selector_pulse_ms = 0U;
  std::uint32_t selector_pulse_amplitude = 1U;
  bool selector_timer_initialized = false;
};

struct MainMenuGlitchFrame {
  bool screen_badge_active = false;
  bool header_active = false;
  bool selector_noise_active = false;
  std::uint32_t screen_badge_phase = 1024U;
  std::uint32_t header_phase = 1024U;
  std::uint32_t selector_noise_amplitude = 1U;
};

// Advances one retail render iteration. Seed 1 reproduces the executable's
// default generator sequence and keeps automated output deterministic.
[[nodiscard]] MainMenuGlitchFrame
advance_main_menu_glitch(MainMenuGlitchState &state,
                         std::uint32_t elapsed_ms) noexcept;
[[nodiscard]] MainMenuGlitchFrame
advance_options_menu_glitch(MainMenuGlitchState &state,
                            std::uint32_t elapsed_ms) noexcept;
[[nodiscard]] MainMenuGlitchFrame
advance_one_player_menu_glitch(MainMenuGlitchState &state,
                               std::uint32_t elapsed_ms) noexcept;

struct SoundOptionsGlitchFrame {
  std::array<std::uint32_t, 22U> transition_noise_amplitude{};
  std::array<bool, 22U> transition_noise_active{};
};

[[nodiscard]] SoundOptionsGlitchFrame
advance_sound_options_transition(MainMenuGlitchState &state,
                                 std::uint32_t transition_phase) noexcept;

// Composes only the statically proven main-menu base layers. The orange pointer
// is excluded here; use compose_main_menu_frame for a recovered settled state.
[[nodiscard]] FrontEndFrame
compose_main_menu_base(const mh::content::TgaImage &background,
                       const mh::content::SprArchive &menu_sprites,
                       const mh::content::SprPositionData &positions,
                       std::uint32_t transition_phase = 0U);

// Adds the original authored labels using the captured initial reference phase
// (frame 4 selected, frame 0 unselected) and the capture-recovered pointer.
[[nodiscard]] FrontEndFrame
compose_main_menu_frame(const mh::content::TgaImage &background,
                        const mh::content::SprArchive &menu_sprites,
                        const mh::content::SprPositionData &positions,
                        MainMenuChoice selection,
                        std::uint32_t selected_label_frame =
                            main_menu_reference_selected_label_frame);

// Composes the same recovered pointer state while advancing the selected
// authored label with the original 512 ms animation cycle.
[[nodiscard]] FrontEndFrame
compose_main_menu_frame_at_time(const mh::content::TgaImage &background,
                                const mh::content::SprArchive &menu_sprites,
                                const mh::content::SprPositionData &positions,
                                MainMenuChoice selection,
                                std::uint32_t elapsed_ms);

// Dynamic compositor used by an interactive front end. pointer_units is the
// state advanced by advance_main_menu_pointer_units().
void compose_main_menu_dynamic_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions, MainMenuChoice selection,
    std::uint32_t elapsed_ms, float pointer_units,
    const MainMenuGlitchFrame &glitch, std::uint32_t transition_phase,
    FrontEndFrame &result);

// p3.1 network-type-4 Multiplayer front end (bank 9). This reconstructs the
// authored entry, session-browser, and create-session presentation; the native
// transport/session backend remains the separate Milestone 7 owner.
[[nodiscard]] FrontEndFrame compose_multiplayer_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, MultiplayerPage page,
    MultiplayerEntryChoice entry_choice, MultiplayerJoinField join_field,
    MultiplayerCreateField create_field, MultiplayerLobbyField lobby_field,
    const MultiplayerConfiguration &configuration, std::uint32_t elapsed_ms,
    std::uint32_t transition_phase = 0U);

// p3.1 common requester used by Escape on the main screen. The authored
// 320x138 `req` sprite is drawn at its settled X=160, fixed Y=150 position;
// slide_x permits the original -320 -> 160 entrance motion.
[[nodiscard]] FrontEndFrame compose_exit_confirmation_overlay(
    FrontEndFrame base, const mh::content::SprArchive &menu_sprites,
    const mh::content::FntData &font, ExitConfirmationChoice selection,
    std::int32_t slide_x = 160);

[[nodiscard]] FrontEndFrame compose_league_create_confirmation_overlay(
    FrontEndFrame base, const mh::content::SprArchive &menu_sprites,
    const mh::content::FntData &font, ExitConfirmationChoice selection,
    std::int32_t slide_x = 160);

[[nodiscard]] FrontEndFrame compose_league_delete_confirmation_overlay(
    FrontEndFrame base, const mh::content::SprArchive &menu_sprites,
    const mh::content::FntData &font, ExitConfirmationChoice selection,
    std::int32_t slide_x = 160);

void compose_options_menu_dynamic_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprArchive &menu_dial_sprites,
    const mh::content::SprPositionData &positions,
    OptionsMenuChoice selection, DifficultyChoice difficulty,
    std::uint32_t elapsed_ms, float pointer_units,
    const MainMenuGlitchFrame &glitch, std::uint32_t transition_phase,
    FrontEndFrame &result);

// p3.1 One Player screen (RVA 0x89d7c/0x89ea4/0x8a3f0), using the
// authored MENU.SPR set and sprpos.dta bank 10.
void compose_one_player_menu_dynamic_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, OnePlayerChoice selection,
    std::uint32_t elapsed_ms, float pointer_units,
    const MainMenuGlitchFrame &glitch, std::uint32_t transition_phase,
    const OnePlayerPanelValues &values, FrontEndFrame &result);

void overlay_one_player_display_information(
    FrontEndFrame &display, const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const OnePlayerPanelValues &values,
    std::uint32_t transition_phase = 0U);

// p3.1 League hub (bank 12, RVA 0x8a4f4/0x8a624/0x8a8b8). The screen's
// observed initial Continue state and Escape path are backed by the accepted
// 20260725T233957Z retail capture.
[[nodiscard]] FrontEndFrame
compose_league_overview_frame(const mh::content::TgaImage &background,
                              const mh::content::SprArchive &menu_sprites,
                              const mh::content::SprPositionData &positions,
                              const mh::content::FntData &font,
                              const LeagueOverviewPresentation &presentation,
                              LeagueMenuChoice selection,
                              std::uint32_t elapsed_ms, float pointer_units,
                              std::uint32_t transition_phase = 0U);

void overlay_league_overview_display_information(
    FrontEndFrame &display, const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font,
    const LeagueOverviewPresentation &presentation,
    std::uint32_t transition_phase = 0U);

[[nodiscard]] FrontEndFrame
compose_league_summary_frame(const mh::content::TgaImage &background,
                             const mh::content::SprArchive &menu_sprites,
                             const mh::content::SprPositionData &positions,
                             const mh::content::FntData &font,
                             const LeagueOverviewPresentation &presentation,
                             std::uint32_t transition_phase = 0U);

[[nodiscard]] FrontEndFrame
compose_league_select_frame(const mh::content::TgaImage &background,
                            const mh::content::SprArchive &menu_sprites,
                            const mh::content::SprPositionData &positions,
                            const mh::content::FntData &font,
                            std::span<const LeagueOverviewPresentation> leagues,
                            std::size_t selected_index,
                            std::uint32_t transition_phase = 0U);

[[nodiscard]] FrontEndFrame compose_league_create_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const LeagueOverviewPresentation &draft,
    LeagueCreateField selection, std::uint32_t transition_phase = 0U);

[[nodiscard]] FrontEndFrame
compose_league_delete_frame(const mh::content::TgaImage &background,
                            const mh::content::SprArchive &menu_sprites,
                            const mh::content::SprPositionData &positions,
                            const mh::content::FntData &font,
                            std::span<const LeagueOverviewPresentation> leagues,
                            std::size_t selected_index,
                            std::uint32_t transition_phase = 0U);

// p3.1 Ghost Mode destination (bank 19,
// RVA 0x806b0/0x80868/0x80a98). Demo deletion remains outside this renderer
// and must not target the read-only reference environment.
[[nodiscard]] FrontEndFrame compose_ghost_setup_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, std::span<const std::string> demo_files,
    GhostModeChoice selection, bool file_focused, std::size_t selected_file,
    std::uint32_t elapsed_ms, std::uint32_t transition_phase = 0U);

// p3.1 shared Single Race/Time Attack setup screen (bank 2,
// RVA 0x8bcfc/0x8bdd0/0x8c33c). The preview is the selected track's original
// Tracks/.../Data/_menuimg.TGA image.
[[nodiscard]] FrontEndFrame compose_race_setup_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font,
    const mh::content::TgaImage &track_preview,
    const mh::content::LobData &track_line_object,
    double track_line_object_scale, const RaceSetupPresentation &presentation,
    RaceSetupMode mode, RaceSetupField selection, std::uint32_t laps,
    bool catch_up, std::uint32_t elapsed_ms,
    std::uint32_t transition_phase = 0U);

void overlay_race_setup_display_badge(
    FrontEndFrame &display, const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    std::uint32_t transition_phase = 0U);

[[nodiscard]] FrontEndFrame compose_car_setup_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, const mh::content::TgaImage &car_preview,
    const mh::content::LobData &car_line_object, double car_line_object_scale,
    const CarSetupPresentation &presentation, CarSetupField selection,
    bool automatic_transmission, bool record_race, std::uint32_t elapsed_ms,
    std::uint32_t transition_phase = 0U);

void overlay_car_setup_display_badge(
    FrontEndFrame &display, const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    std::uint32_t transition_phase = 0U);

// Settled values reproduce the accepted p3.1 Graphic Options capture. The
// renderer exposes the exact standard and active-renderer Custom field sets,
// including all recovered discrete values and held-key sliders.
[[nodiscard]] FrontEndFrame compose_graphic_options_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font,
    GraphicOptionsField selection = GraphicOptionsField::render_device,
    const GraphicOptionsConfiguration &configuration = {},
    std::uint32_t transition_phase = 0U);

[[nodiscard]] FrontEndFrame compose_gameplay_options_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font,
    GameplayOptionsField selection = GameplayOptionsField::info_detail,
    const GraphicOptionsConfiguration &configuration = {},
    std::uint32_t transition_phase = 0U);

[[nodiscard]] FrontEndFrame compose_personal_options_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, std::uint32_t transition_phase = 0U,
    PersonalOptionsField selection = PersonalOptionsField::name,
    const PersonalOptionsConfiguration &configuration = {},
    std::uint32_t selected_label_frame = 0U);

[[nodiscard]] FrontEndFrame compose_control_options_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, std::uint32_t transition_phase = 0U,
    ControlOptionsFocus focus = ControlOptionsFocus::profile_list,
    ControlOptionsField selection = ControlOptionsField::force_feedback,
    const ControlOptionsConfiguration &configuration = {},
    bool binding_capture_active = false);

[[nodiscard]] FrontEndFrame compose_sound_options_frame(
    const mh::content::TgaImage &background,
    const mh::content::SprArchive &menu_sprites,
    const mh::content::SprPositionData &positions,
    const mh::content::FntData &font, std::uint32_t transition_phase = 0U,
    const SoundOptionsGlitchFrame &glitch = {},
    SoundOptionsField selection = SoundOptionsField::sound_device,
    const SoundOptionsConfiguration &configuration = {},
    const SoundOptionsPresentation &presentation = {},
    std::uint32_t selected_label_frame = 0U);

// p3.1 Credits destination (bank 18, descriptor VA 0x004fa04c,
// renderer RVA 0x8d464). This is the normal game-credit page; the executable's
// hidden modifier-chord demoscene page remains a separate guarded path.
[[nodiscard]] FrontEndFrame
compose_credits_frame(const mh::content::TgaImage &background,
                      const mh::content::SprArchive &menu_sprites,
                      const mh::content::SprPositionData &positions,
                      const mh::content::FntData &font,
                      std::uint32_t transition_phase = 0U);

[[nodiscard]] FrontEndFrame
compose_rankings_frame(const mh::content::TgaImage &background,
                       const mh::content::SprArchive &menu_sprites,
                       const mh::content::SprPositionData &positions,
                       const mh::content::FntData &font,
                       RankingsField selection,
                       const RankingsConfiguration &configuration,
                       const RankingsPresentation &presentation = {},
                       std::uint32_t transition_phase = 0U);

// p3.1 post-race ScreenResults compositor (RVA 0x3ffb8). The caller selects
// Data/results2.TGA for at most two racers and Data/Results.TGA otherwise.
// portraits must contain one original 90x56 Game/player.tga or
// League/Profiles/Gfx/*.TGA image for every result entry.
[[nodiscard]] FrontEndFrame
compose_race_results_frame(const mh::content::TgaImage &background,
                           const mh::content::FntData &font,
                           std::span<const RaceResultEntry> entries,
                           std::span<const mh::content::TgaImage> portraits);

// p3.1 league-result compositor (RVA 0x40c10). This screen reuses the same
// portrait grid but presents only Nick and accumulated Score.
[[nodiscard]] FrontEndFrame
compose_league_results_frame(const mh::content::TgaImage &background,
                             const mh::content::FntData &font,
                             std::span<const RaceResultEntry> entries,
                             std::span<const mh::content::TgaImage> portraits);

} // namespace mh::ui
