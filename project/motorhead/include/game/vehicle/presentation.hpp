#pragma once

#include <game/physics/body_pose.hpp>
#include <game/physics/body_response.hpp>
#include <game/physics/simulation.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace mh::game {

// Presentation-only state for the recovered p3.1 engine mixer. The
// reconstructed drivetrain remains authoritative for gear/RPM inputs. The
// track SplineName's first evaluated rotation channel crossfades the two
// central loops into the two hard-panned stereo pairs.
struct EngineAudioMixState {
  bool initialized = false;
  std::size_t gear_index = 1U;
};

struct EngineAudioMixFrame {
  double frequency_ratio = 0.5;
  double gason_level = 0.0;
  double gasrele_level = 0.0;
  double gas_ot_level = 0.0;
  double gas_rt_level = 0.0;
  bool gear_changed = false;
};

struct OriginalHudViewport {
  double scale = 1.0;
  double offset_x = 0.0;
  double offset_y = 0.0;
};

struct OriginalRaceProjection {
  double center_x = 0.0;
  double center_y = 0.0;
  double focal_length = 1.0;
  double near_plane = 0.5;
};

// The race HUD is authored directly in the p3.1 640x480 transformed-vertex
// coordinate space. Modern output sizes use one uniform scale and a centred
// safe area so glyph and marker geometry cannot be stretched independently.
[[nodiscard]] OriginalHudViewport
original_hud_viewport(std::uint32_t output_width, std::uint32_t output_height);

// Accessibility-owned multiplier for every race interface layer. The 3D
// projection deliberately retains the original 100% viewport.
void set_original_hud_ui_scale(double multiplier) noexcept;

// Edge-owned race HUD groups move into the additional horizontal area on
// widescreen outputs while centre-owned overlays remain centred. The glyph
// scale stays uniform, so neither the retail font nor its sprites are
// stretched.
[[nodiscard]] double original_hud_x(const OriginalHudViewport &viewport,
                                    double logical_x) noexcept;

// Top-, centre-, and bottom-owned HUD elements remain attached to their
// respective screen edge while the accessibility scale changes.
[[nodiscard]] double original_hud_y(const OriginalHudViewport &viewport,
                                    double logical_y) noexcept;

// p3.1 initializes a 60-degree horizontal field of view for its authored 4:3
// viewport, stores cot(fov/2), and clips at 0.5 world units. At modern aspect
// ratios the fitted 4:3 extent retains that exact framing while exposing the
// otherwise unused output area without stretching the world.
[[nodiscard]] OriginalRaceProjection
original_race_projection(std::uint32_t output_width,
                         std::uint32_t output_height);

[[nodiscard]] EngineAudioMixFrame
update_engine_audio_mix(EngineAudioMixState &state, std::size_t gear_index,
                        double engine_scalar, double authored_track_mix);

// p3.1's DirectSound backend converts each recovered mixer level into
// hundredths of a decibel before submitting it to IDirectSoundBuffer. This
// returns the equivalent linear SDL stream gain for a player-attached source.
[[nodiscard]] double retail_engine_audio_linear_gain(double recovered_level);

// Spatial sound instances take the other branch of the same p3.1
// DirectSound backend. Their recovered base level is not multiplied by the
// non-spatial 0.6 constant; DirectSound applies 3D distance rolloff after the
// base buffer volume has been converted.
[[nodiscard]] double retail_spatial_audio_linear_gain(double recovered_level);

// The p3.1 countdown, GO, and normal-finish initializers submit zero for every
// optional per-instance parameter. Their PCM therefore reaches the shared SFX
// mixer without an additional cue-local attenuation.
[[nodiscard]] double original_race_cue_linear_gain() noexcept;

// Converts the listener-relative azimuth submitted to p3.1's DirectSound 3D
// buffer into the equivalent phase-matched hard-left/right gains used by the
// SDL backend. The listener right vector is normalized here; a coincident
// source remains centred.
[[nodiscard]] std::array<double, 2U> direct_sound_3d_stereo_channel_gains(
    const std::array<double, 3U> &source_position,
    const std::array<double, 3U> &listener_position,
    const std::array<double, 3U> &listener_right);

// sub_000b370c adds 30 to both authored distance bounds. The DirectSound
// manager measures source/listener distance at its recovered 0.8 world scale,
// keeps sources strictly inside the adjusted maximum, and assigns a linear
// factor for active-source priority.
[[nodiscard]] double original_environment_sound_priority_factor(
    const std::array<double, 3U> &source_position,
    const std::array<double, 3U> &listener_position,
    double authored_minimum_distance, double authored_maximum_distance);

// Generic p3.1 3D-buffer rolloff for already initialized minimum/maximum
// distances. Unlike ESD values, these bounds do not receive the initializer's
// +30 bias; the non-player ENEMYENG loop uses this path with 15 and 250.
[[nodiscard]] double original_spatial_sound_distance_gain(
    const std::array<double, 3U> &source_position,
    const std::array<double, 3U> &listener_position,
    double initialized_minimum_distance, double initialized_maximum_distance);

// Equivalent gain from DirectSound's default real-world rolloff: full volume
// through the adjusted minimum, inverse-distance attenuation after it, and
// silence at the original manager's strict adjusted-maximum boundary.
[[nodiscard]] double original_environment_sound_distance_gain(
    const std::array<double, 3U> &source_position,
    const std::array<double, 3U> &listener_position,
    double authored_minimum_distance, double authored_maximum_distance);

[[nodiscard]] double original_environment_sound_level(double authored_volume,
                                                      double distance_gain);

[[nodiscard]] double original_environment_sound_pitch(double authored_pitch);

// The p3.1 per-car sound owner replaces the ObjectColSound template's authored
// level at dispatch with 0.1 + 0.03 times the retained collision scalar.
[[nodiscard]] double
original_object_collision_sound_level(double collision_scalar);

// The live p3.1 model owner applies body +0xd8 only to front wheel slots
// zero and one, multiplying the retained float32 state by binary64 0.7.
[[nodiscard]] double
original_front_wheel_visual_angle(double retained_steering);

// Reproduces the p3.1 wheel-object translation owner at
// 0x0001c2e1..0x0001c339. The authored lateral center receives the original
// 1.25 widening factor, then the retained suspension state displaces the
// object along its initialized suspension axis. Each component is stored to
// float32 before the object transform consumes it.
[[nodiscard]] CollisionVector3
original_wheel_visual_translation(const CollisionVector3 &authored_center,
                                  const CollisionVector3 &suspension_axis,
                                  double retained_wheel_state);

// Advances either retained front/rear wheel-object rolling phase using the
// p3.1 +0xdc/+0xe0 owner and its +/-40*pi bounded range.
[[nodiscard]] double original_wheel_visual_spin_phase(double retained_phase,
                                                      double wheel_spin_rate,
                                                      double elapsed_seconds);

// p3.1's skid-mark producer does not use the broad alternate-damping result.
// It admits a wheel only when car +0x94 or +0xa0 is active, or when the
// retained +0xcc traction accumulator is strictly greater than 8.
[[nodiscard]] bool
original_skid_mark_active(bool alternate_state_a, bool alternate_state_b,
                          double retained_traction_accumulator);

// The accelerated wheel-smoke owner follows the active grounded grip-break
// branch. It is deliberately separate from the stricter skid-mark threshold:
// handbrake/lateral/yaw slip can emit smoke before a mark is laid.
[[nodiscard]] bool original_tire_smoke_active(
    const OriginalVehicleGroundedDampingSelection &selection) noexcept;

// The rear lamp halo keeps its authored running-light pass. Applying either
// braking control requests a second additive pass from the renderer.
[[nodiscard]] bool
vehicle_rear_lamps_bright(const ControlInput &controls) noexcept;

// The 50-entry ring advances at a speed-dependent squared-distance boundary:
// min(9, ((vx^2 + vz^2 + 650) * 0.0015)^2). A partial record is extended only
// after its squared displacement is strictly greater than 0.01; a gap beyond
// 144 terminates the active strip.
[[nodiscard]] double
original_skid_mark_advance_distance_squared(double local_lateral_velocity,
                                            double local_longitudinal_velocity);

enum class OriginalSkidMarkPointAction {
  retain,
  extend,
  advance,
  break_strip,
};

[[nodiscard]] OriginalSkidMarkPointAction
original_skid_mark_point_action(double distance_squared,
                                double advance_distance_squared);

// Exact RGB cases selected by Material.mat SkidMark values 1..4. A zero or
// out-of-range selector is rejected by the runtime owner.
[[nodiscard]] std::array<std::uint8_t, 3U>
original_skid_mark_color(std::uint8_t material_mark);

struct OriginalSkidMarkRibbon {
  std::array<CollisionVector3, 4U> corners{};
};

// The accelerated p3.1 skid renderer at RVA 0x0005389c constructs one quad
// from each retained ring record. It extends both ends by 0.01, crosses the
// stored contact normal with the segment direction, uses 0.1 as the half
// width, and raises the submitted vertices by 0.01 on world Y. Degenerate
// records are rejected by the original renderer before primitive submission.
[[nodiscard]] std::optional<OriginalSkidMarkRibbon>
original_accelerated_skid_mark_ribbon(const CollisionVector3 &start,
                                      const CollisionVector3 &end,
                                      const CollisionVector3 &contact_normal);

// The p3.1 race-audio owner chooses one grounded wheel material by the
// SndContact second integer. It retains the earlier wheel on equal priority
// and ignores wheels without a valid material or contact sound.
[[nodiscard]] std::optional<std::size_t> select_original_material_contact_sound(
    const std::array<std::optional<std::size_t>, 4U> &wheel_materials,
    const OriginalVehicleGroundedMaterialTable &materials);

// Exact recovered levels written to the retail sound-instance +0x20 field.
// Conversion from this level to the backend's gain is owned separately by
// retail_engine_audio_linear_gain(). The spatial-mode branch is explicit:
// p3.1 uses 1.0 when enabled and 1.2 otherwise.
[[nodiscard]] double
original_material_contact_sound_level(double local_longitudinal_velocity,
                                      double authored_scalar,
                                      bool spatial_mode_enabled);

[[nodiscard]] double
original_material_spin_sound_level(double retained_traction_accumulator,
                                   double authored_scalar);

[[nodiscard]] double
original_material_slide_sound_level(double retained_traction_accumulator,
                                    std::uint8_t slide_type);

[[nodiscard]] double
original_material_weak_impulse_sound_level(double hull_impulse,
                                           double authored_scalar);

[[nodiscard]] double
original_material_hard_impulse_sound_level(double authored_scalar);

// The primary retained hull record is handed off with its signed scratch
// candidate. p3.1 disables scratch for a negative candidate, otherwise scales
// it by 0.03, caps it at one, and applies the material scalar and 1.2 mix.
[[nodiscard]] double
original_material_scratch_sound_level(double scratch_candidate,
                                      double authored_scalar);

// Selected body-hull points 0 and 3 use -0.7; the other points use +0.7.
// The concrete DirectSound backend maps either value beyond its full-pan clamp.
[[nodiscard]] double
original_material_scratch_pan(std::size_t hull_point_index);

struct OriginalMaterialSoundSampleWindow {
  std::size_t leading_trim_frames = 0U;
  std::size_t trailing_trim_frames = 0U;
};

// The last two Material.mat sound integers are sample-frame trims. Each value
// below 100 becomes zero, each is capped to the WAV frame count, and an empty
// range resets both trims. Invalid overlapping ranges are rejected.
[[nodiscard]] std::optional<OriginalMaterialSoundSampleWindow>
original_material_sound_sample_window(std::size_t sample_frame_count,
                                      std::int32_t authored_leading,
                                      std::int32_t authored_trailing);

} // namespace mh::game
