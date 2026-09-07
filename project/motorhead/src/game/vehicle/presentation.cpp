#include <game/vehicle/presentation.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace mh::game {
namespace {

double hud_ui_scale = 1.0;

OriginalHudViewport base_hud_viewport(const std::uint32_t output_width,
                                      const std::uint32_t output_height,
                                      const double ui_scale) {
  constexpr double logical_width = 640.0;
  constexpr double logical_height = 480.0;
  const auto fitted_scale =
      std::min(static_cast<double>(output_width) / logical_width,
               static_cast<double>(output_height) / logical_height);
  const auto scale = fitted_scale * ui_scale;
  return {scale,
          (static_cast<double>(output_width) - logical_width * scale) * 0.5,
          (static_cast<double>(output_height) - logical_height * scale) * 0.5};
}

void require_finite(const double value, const char *label) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(label) + " must be finite");
  }
}

} // namespace

OriginalHudViewport original_hud_viewport(const std::uint32_t output_width,
                                          const std::uint32_t output_height) {
  if (output_width == 0U || output_height == 0U) {
    throw std::invalid_argument("HUD output dimensions must be nonzero");
  }
  return base_hud_viewport(output_width, output_height, hud_ui_scale);
}

void set_original_hud_ui_scale(const double multiplier) noexcept {
  hud_ui_scale = std::clamp(multiplier, 0.5, 1.5);
}

double original_hud_x(const OriginalHudViewport &viewport,
                      const double logical_x) noexcept {
  constexpr double left_anchor_limit = 640.0 / 3.0;
  constexpr double right_anchor_limit = 640.0 * 2.0 / 3.0;
  if (logical_x <= left_anchor_limit) {
    return logical_x * viewport.scale;
  }
  if (logical_x >= right_anchor_limit) {
    return logical_x * viewport.scale + viewport.offset_x * 2.0;
  }
  return logical_x * viewport.scale + viewport.offset_x;
}

double original_hud_y(const OriginalHudViewport &viewport,
                      const double logical_y) noexcept {
  constexpr double top_anchor_limit = 480.0 / 3.0;
  constexpr double bottom_anchor_limit = 480.0 * 2.0 / 3.0;
  if (logical_y <= top_anchor_limit) {
    return logical_y * viewport.scale;
  }
  if (logical_y >= bottom_anchor_limit) {
    return logical_y * viewport.scale + viewport.offset_y * 2.0;
  }
  return logical_y * viewport.scale + viewport.offset_y;
}

OriginalRaceProjection
original_race_projection(const std::uint32_t output_width,
                         const std::uint32_t output_height) {
  if (output_width == 0U || output_height == 0U) {
    throw std::invalid_argument("race projection dimensions must be nonzero");
  }

  constexpr double logical_width = 640.0;
  constexpr double original_horizontal_half_fov_radians =
      30.0 * 3.14159265358979323846 / 180.0;
  constexpr double original_near_plane = 0.5;
  const auto fitted = base_hud_viewport(output_width, output_height, 1.0);
  const auto fitted_width = logical_width * fitted.scale;
  return {static_cast<double>(output_width) * 0.5,
          static_cast<double>(output_height) * 0.5,
          fitted_width * 0.5 / std::tan(original_horizontal_half_fov_radians),
          original_near_plane};
}

EngineAudioMixFrame update_engine_audio_mix(EngineAudioMixState &state,
                                            const std::size_t gear_index,
                                            const double engine_scalar,
                                            const double authored_track_mix) {
  require_finite(engine_scalar, "engine scalar");
  require_finite(authored_track_mix, "engine authored track mix");

  const auto gear_changed = state.initialized && gear_index != state.gear_index;
  if (!state.initialized) {
    state.initialized = true;
    state.gear_index = gear_index;
  }
  state.gear_index = gear_index;

  const auto track_mix = std::clamp(authored_track_mix, 0.0, 1.0);

  double low_band = 0.0;
  double high_band = 0.0;
  if (engine_scalar < 0.0) {
    low_band = 0.26656;
  } else if (engine_scalar < 14500.0) {
    const auto blend = std::max((engine_scalar - 9000.0) / 5500.0, 0.0);
    low_band = (1.0 - blend) * 0.26656;
    high_band = blend * 0.168 + 0.2;
  } else {
    high_band = 0.168;
  }

  const auto frequency_ratio = engine_scalar * 0.00018 + 0.32;
  return {
      frequency_ratio,
      (1.0 - track_mix) * low_band,
      (1.0 - track_mix) * high_band,
      low_band * track_mix * 10.0,
      high_band * track_mix * 3.0,
      gear_changed,
  };
}

namespace {

double retail_audio_linear_gain(const double recovered_level,
                                const double backend_scale) {
  require_finite(recovered_level, "recovered engine audio level");
  if (recovered_level < 0.0) {
    throw std::invalid_argument(
        "recovered engine audio level must not be negative");
  }
  if (recovered_level == 0.0) {
    return 0.0;
  }

  // p3.1 maps the retained level through
  // 10000*ln(1 + level*backend_scale*10)-10000, then submits the clamped
  // hundredths-of-a-decibel value to DirectSound.
  const auto direct_sound_level = std::clamp(
      10000.0 * std::log1p(recovered_level * backend_scale * 10.0) - 10000.0,
      -10000.0, 0.0);
  return std::pow(10.0, direct_sound_level / 2000.0);
}

} // namespace

double retail_engine_audio_linear_gain(const double recovered_level) {
  // All seven captured player-engine objects have backend flag bit 1 clear.
  return retail_audio_linear_gain(recovered_level, 0.6);
}

double retail_spatial_audio_linear_gain(const double recovered_level) {
  // Environmental instances have backend flag bit 1 set and jump over the
  // non-spatial 0.6 multiply at RVA 0x000b03d2..0x000b04f0.
  return retail_audio_linear_gain(recovered_level, 1.0);
}

double original_race_cue_linear_gain() noexcept { return 1.0; }

std::array<double, 2U> direct_sound_3d_stereo_channel_gains(
    const std::array<double, 3U> &source_position,
    const std::array<double, 3U> &listener_position,
    const std::array<double, 3U> &listener_right) {
  std::array<double, 3U> offset{};
  auto offset_length_squared = 0.0;
  auto right_length_squared = 0.0;
  for (std::size_t axis = 0U; axis < offset.size(); ++axis) {
    require_finite(source_position[axis], "spatial sound source");
    require_finite(listener_position[axis], "spatial sound listener");
    require_finite(listener_right[axis], "spatial sound listener right");
    offset[axis] = source_position[axis] - listener_position[axis];
    offset_length_squared += offset[axis] * offset[axis];
    right_length_squared += listener_right[axis] * listener_right[axis];
  }
  if (right_length_squared <= 1.0e-12) {
    throw std::invalid_argument(
        "spatial sound listener right vector has zero length");
  }
  auto pan = 0.0;
  if (offset_length_squared > 1.0e-12) {
    auto projection = 0.0;
    for (std::size_t axis = 0U; axis < offset.size(); ++axis) {
      projection += offset[axis] * listener_right[axis];
    }
    pan = projection / std::sqrt(offset_length_squared * right_length_squared);
  }
  pan = std::clamp(pan, -1.0, 1.0);
  return {std::sqrt((1.0 - pan) * 0.5), std::sqrt((1.0 + pan) * 0.5)};
}

namespace {

struct OriginalEnvironmentDistanceState {
  double distance = 0.0;
  double minimum = 0.0;
  double maximum = 0.0;
};

OriginalEnvironmentDistanceState original_environment_distance_state(
    const std::array<double, 3U> &source_position,
    const std::array<double, 3U> &listener_position,
    const double initialized_minimum_distance,
    const double initialized_maximum_distance) {
  require_finite(initialized_minimum_distance,
                 "spatial sound minimum distance");
  require_finite(initialized_maximum_distance,
                 "spatial sound maximum distance");
  if (initialized_minimum_distance < 0.0 ||
      initialized_maximum_distance <= initialized_minimum_distance) {
    throw std::invalid_argument("spatial sound distance bounds are invalid");
  }
  auto squared_distance = 0.0;
  for (std::size_t axis = 0U; axis < source_position.size(); ++axis) {
    require_finite(source_position[axis], "environment sound source");
    require_finite(listener_position[axis], "environment sound listener");
    const auto delta = source_position[axis] - listener_position[axis];
    squared_distance += delta * delta;
  }
  constexpr double original_world_distance_scale = 0.8;
  return {std::sqrt(squared_distance) * original_world_distance_scale,
          std::max(initialized_minimum_distance, 1.0),
          initialized_maximum_distance};
}

} // namespace

double original_environment_sound_priority_factor(
    const std::array<double, 3U> &source_position,
    const std::array<double, 3U> &listener_position,
    const double authored_minimum_distance,
    const double authored_maximum_distance) {
  const auto state = original_environment_distance_state(
      source_position, listener_position, authored_minimum_distance + 30.0,
      authored_maximum_distance + 30.0);
  if (state.distance >= state.maximum) {
    return 0.0;
  }
  if (state.distance <= state.minimum) {
    return 1.0;
  }
  return (state.maximum - state.distance) / (state.maximum - state.minimum);
}

double original_spatial_sound_distance_gain(
    const std::array<double, 3U> &source_position,
    const std::array<double, 3U> &listener_position,
    const double initialized_minimum_distance,
    const double initialized_maximum_distance) {
  const auto state = original_environment_distance_state(
      source_position, listener_position, initialized_minimum_distance,
      initialized_maximum_distance);
  if (state.distance >= state.maximum) {
    return 0.0;
  }
  if (state.distance <= state.minimum) {
    return 1.0;
  }
  return state.minimum / state.distance;
}

double original_environment_sound_distance_gain(
    const std::array<double, 3U> &source_position,
    const std::array<double, 3U> &listener_position,
    const double authored_minimum_distance,
    const double authored_maximum_distance) {
  const auto state = original_environment_distance_state(
      source_position, listener_position, authored_minimum_distance + 30.0,
      authored_maximum_distance + 30.0);
  if (state.distance >= state.maximum) {
    return 0.0;
  }
  if (state.distance <= state.minimum) {
    return 1.0;
  }
  return state.minimum / state.distance;
}

double original_environment_sound_level(const double authored_volume,
                                        const double distance_gain) {
  require_finite(authored_volume, "environment sound volume");
  require_finite(distance_gain, "environment sound distance gain");
  if (authored_volume < 0.0 || distance_gain < 0.0 || distance_gain > 1.0) {
    throw std::invalid_argument("environment sound level input is invalid");
  }
  constexpr double original_environment_volume_scale = 0.14;
  return authored_volume * original_environment_volume_scale * distance_gain;
}

double original_environment_sound_pitch(const double authored_pitch) {
  require_finite(authored_pitch, "environment sound pitch");
  if (authored_pitch < 0.0) {
    throw std::invalid_argument("environment sound pitch must not be negative");
  }
  return authored_pitch == 0.0 ? 1.0 : authored_pitch;
}

double original_object_collision_sound_level(const double collision_scalar) {
  require_finite(collision_scalar, "object collision sound scalar");
  if (collision_scalar < 0.0) {
    throw std::invalid_argument(
        "object collision sound scalar must not be negative");
  }
  constexpr double original_collision_scalar_scale = 0.03;
  constexpr double original_collision_level_bias = 0.1;
  return collision_scalar * original_collision_scalar_scale +
         original_collision_level_bias;
}

double original_front_wheel_visual_angle(const double retained_steering) {
  require_finite(retained_steering, "retained steering");
  constexpr double original_front_wheel_steering_scale = 0.7;
  return retained_steering * original_front_wheel_steering_scale;
}

CollisionVector3
original_wheel_visual_translation(const CollisionVector3 &authored_center,
                                  const CollisionVector3 &suspension_axis,
                                  const double retained_wheel_state) {
  for (const auto component : authored_center) {
    require_finite(component, "authored wheel center");
  }
  for (const auto component : suspension_axis) {
    require_finite(component, "wheel suspension axis");
  }
  require_finite(retained_wheel_state, "retained wheel state");

  constexpr double original_lateral_center_scale = 1.25;
  const auto retained = static_cast<float>(retained_wheel_state);
  CollisionVector3 result{};
  for (std::size_t axis = 0U; axis < result.size(); ++axis) {
    const auto base =
        axis == 0U ? authored_center[axis] * original_lateral_center_scale
                   : authored_center[axis];
    result[axis] = static_cast<double>(static_cast<float>(
        suspension_axis[axis] * static_cast<double>(retained) + base));
  }
  return result;
}

double original_wheel_visual_spin_phase(const double retained_phase,
                                        const double wheel_spin_rate,
                                        const double elapsed_seconds) {
  require_finite(retained_phase, "retained wheel spin phase");
  require_finite(wheel_spin_rate, "wheel spin rate");
  require_finite(elapsed_seconds, "wheel spin elapsed time");
  if (elapsed_seconds < 0.0) {
    throw std::invalid_argument("wheel spin elapsed time must not be negative");
  }

  auto phase = static_cast<float>(
      static_cast<double>(static_cast<float>(wheel_spin_rate)) *
          static_cast<double>(static_cast<float>(elapsed_seconds)) +
      static_cast<double>(static_cast<float>(retained_phase)));
  constexpr double original_phase_limit = 125.6637;
  constexpr float original_phase_wrap = 125.6637F;
  if (static_cast<double>(phase) > original_phase_limit) {
    phase = static_cast<float>(phase - original_phase_wrap);
  }
  if (static_cast<double>(phase) < -original_phase_limit) {
    phase = static_cast<float>(phase + original_phase_wrap);
  }
  return static_cast<double>(phase);
}

bool original_skid_mark_active(const bool alternate_state_a,
                               const bool alternate_state_b,
                               const double retained_traction_accumulator) {
  require_finite(retained_traction_accumulator,
                 "retained skid-mark traction accumulator");
  return alternate_state_a || alternate_state_b ||
         retained_traction_accumulator > 8.0;
}

bool original_tire_smoke_active(
    const OriginalVehicleGroundedDampingSelection &selection) noexcept {
  return selection.alternate_branch;
}

bool vehicle_rear_lamps_bright(const ControlInput &controls) noexcept {
  return controls.brake > 0.0 || controls.handbrake;
}

double original_skid_mark_advance_distance_squared(
    const double local_lateral_velocity,
    const double local_longitudinal_velocity) {
  require_finite(local_lateral_velocity, "skid-mark lateral velocity");
  require_finite(local_longitudinal_velocity,
                 "skid-mark longitudinal velocity");
  constexpr double original_speed_bias = 650.0;
  constexpr double original_distance_scale = 0.0015;
  constexpr double original_maximum_advance_distance_squared = 9.0;
  const auto scaled =
      (local_lateral_velocity * local_lateral_velocity +
       local_longitudinal_velocity * local_longitudinal_velocity +
       original_speed_bias) *
      original_distance_scale;
  return std::min(scaled * scaled, original_maximum_advance_distance_squared);
}

OriginalSkidMarkPointAction
original_skid_mark_point_action(const double distance_squared,
                                const double advance_distance_squared) {
  require_finite(distance_squared, "skid-mark squared distance");
  require_finite(advance_distance_squared,
                 "skid-mark advance squared distance");
  if (distance_squared < 0.0 || advance_distance_squared < 0.0) {
    throw std::invalid_argument(
        "skid-mark squared distances must not be negative");
  }
  constexpr double original_break_distance_squared = 144.0;
  constexpr double original_extend_distance_squared = 0.01;
  if (distance_squared > original_break_distance_squared) {
    return OriginalSkidMarkPointAction::break_strip;
  }
  if (distance_squared > advance_distance_squared) {
    return OriginalSkidMarkPointAction::advance;
  }
  if (distance_squared > original_extend_distance_squared) {
    return OriginalSkidMarkPointAction::extend;
  }
  return OriginalSkidMarkPointAction::retain;
}

std::array<std::uint8_t, 3U>
original_skid_mark_color(const std::uint8_t material_mark) {
  switch (material_mark) {
  case 1U:
  case 2U:
    return {};
  case 3U:
    return {80U, 60U, 20U};
  case 4U:
    return {40U, 35U, 20U};
  default:
    throw std::invalid_argument(
        "skid-mark material selector must be from one through four");
  }
}

std::optional<OriginalSkidMarkRibbon>
original_accelerated_skid_mark_ribbon(const CollisionVector3 &start,
                                      const CollisionVector3 &end,
                                      const CollisionVector3 &contact_normal) {
  for (const auto value : start) {
    require_finite(value, "skid-mark start");
  }
  for (const auto value : end) {
    require_finite(value, "skid-mark end");
  }
  for (const auto value : contact_normal) {
    require_finite(value, "skid-mark contact normal");
  }

  CollisionVector3 direction{};
  auto direction_length_squared = 0.0;
  for (std::size_t axis = 0U; axis < direction.size(); ++axis) {
    direction[axis] = end[axis] - start[axis];
    direction_length_squared += direction[axis] * direction[axis];
  }
  if (direction_length_squared <= 0.0) {
    return std::nullopt;
  }
  const auto inverse_direction_length =
      1.0 / std::sqrt(direction_length_squared);
  for (auto &value : direction) {
    value *= inverse_direction_length;
  }

  CollisionVector3 lateral{
      contact_normal[1U] * direction[2U] - contact_normal[2U] * direction[1U],
      contact_normal[2U] * direction[0U] - contact_normal[0U] * direction[2U],
      contact_normal[0U] * direction[1U] - contact_normal[1U] * direction[0U]};
  auto lateral_length_squared = 0.0;
  for (const auto value : lateral) {
    lateral_length_squared += value * value;
  }
  if (lateral_length_squared <= 0.0) {
    return std::nullopt;
  }
  constexpr double original_half_width = 0.1;
  const auto lateral_scale =
      original_half_width / std::sqrt(lateral_length_squared);
  for (auto &value : lateral) {
    value *= lateral_scale;
  }

  constexpr double original_endpoint_extension = 0.01;
  constexpr double original_surface_y_offset = 0.01;
  CollisionVector3 start_center{};
  CollisionVector3 end_center{};
  for (std::size_t axis = 0U; axis < direction.size(); ++axis) {
    start_center[axis] =
        start[axis] - direction[axis] * original_endpoint_extension;
    end_center[axis] =
        end[axis] + direction[axis] * original_endpoint_extension;
  }
  start_center[1U] += original_surface_y_offset;
  end_center[1U] += original_surface_y_offset;

  OriginalSkidMarkRibbon result;
  for (std::size_t axis = 0U; axis < direction.size(); ++axis) {
    result.corners[0U][axis] = start_center[axis] - lateral[axis];
    result.corners[1U][axis] = start_center[axis] + lateral[axis];
    result.corners[2U][axis] = end_center[axis] + lateral[axis];
    result.corners[3U][axis] = end_center[axis] - lateral[axis];
  }
  return result;
}

std::optional<std::size_t> select_original_material_contact_sound(
    const std::array<std::optional<std::size_t>, 4U> &wheel_materials,
    const OriginalVehicleGroundedMaterialTable &materials) {
  std::optional<std::size_t> selected;
  for (const auto material : wheel_materials) {
    if (!material.has_value() || *material >= materials.size() ||
        !materials[*material].contact_sound.has_value()) {
      continue;
    }
    if (!selected.has_value() ||
        materials[*material].contact_sound->parameter_2 >
            materials[*selected].contact_sound->parameter_2) {
      selected = *material;
    }
  }
  return selected;
}

double
original_material_contact_sound_level(const double local_longitudinal_velocity,
                                      const double authored_scalar,
                                      const bool spatial_mode_enabled) {
  require_finite(local_longitudinal_velocity,
                 "material contact longitudinal velocity");
  require_finite(authored_scalar, "material contact authored scalar");
  if (authored_scalar < 0.0) {
    throw std::invalid_argument(
        "material contact authored scalar must not be negative");
  }
  constexpr double original_velocity_scale = 0.1;
  const auto spatial_scale = spatial_mode_enabled ? 1.0 : 1.2;
  const auto velocity_level =
      std::min(std::fabs(local_longitudinal_velocity) * spatial_scale *
                   original_velocity_scale,
               1.0);
  return std::min(velocity_level * authored_scalar, 1.0);
}

double
original_material_spin_sound_level(const double retained_traction_accumulator,
                                   const double authored_scalar) {
  require_finite(retained_traction_accumulator,
                 "material spin traction accumulator");
  require_finite(authored_scalar, "material spin authored scalar");
  if (retained_traction_accumulator < 0.0 || authored_scalar < 0.0) {
    throw std::invalid_argument(
        "material spin sound inputs must not be negative");
  }
  constexpr double original_spin_scale = 0.3;
  return std::min(retained_traction_accumulator * original_spin_scale *
                      authored_scalar,
                  1.0);
}

double
original_material_slide_sound_level(const double retained_traction_accumulator,
                                    const std::uint8_t slide_type) {
  require_finite(retained_traction_accumulator,
                 "material slide traction accumulator");
  if (retained_traction_accumulator < 0.0) {
    throw std::invalid_argument(
        "material slide traction accumulator must not be negative");
  }
  constexpr double original_output_scale = 0.9;
  if (slide_type == 1U) {
    constexpr double original_special_scale = 0.9;
    constexpr double original_special_bias = 0.4;
    return std::min((retained_traction_accumulator * original_special_scale +
                     original_special_bias) *
                        original_output_scale,
                    1.0);
  }
  constexpr double original_authored_scale = 0.035;
  constexpr double original_authored_bias = 0.3;
  return std::min((retained_traction_accumulator * original_authored_scale +
                   original_authored_bias) *
                      original_output_scale,
                  1.0);
}

double
original_material_weak_impulse_sound_level(const double hull_impulse,
                                           const double authored_scalar) {
  require_finite(hull_impulse, "material weak hull impulse");
  require_finite(authored_scalar, "material weak authored scalar");
  if (hull_impulse < 0.0 || hull_impulse >= 18500.0 || authored_scalar < 0.0) {
    throw std::invalid_argument(
        "material weak impulse input is outside its recovered range");
  }
  constexpr double original_hard_impulse_threshold = 18500.0;
  constexpr double original_impulse_output_scale = 0.7;
  return authored_scalar * hull_impulse / original_hard_impulse_threshold *
         original_impulse_output_scale;
}

double
original_material_hard_impulse_sound_level(const double authored_scalar) {
  require_finite(authored_scalar, "material hard authored scalar");
  if (authored_scalar < 0.0) {
    throw std::invalid_argument(
        "material hard authored scalar must not be negative");
  }
  constexpr double original_impulse_output_scale = 0.7;
  return authored_scalar * original_impulse_output_scale;
}

double original_material_scratch_sound_level(const double scratch_candidate,
                                             const double authored_scalar) {
  require_finite(scratch_candidate, "material scratch candidate");
  require_finite(authored_scalar, "material scratch authored scalar");
  if (authored_scalar < 0.0) {
    throw std::invalid_argument(
        "material scratch authored scalar must not be negative");
  }
  if (scratch_candidate < 0.0) {
    return 0.0;
  }
  constexpr double original_candidate_scale = 0.03;
  constexpr double original_output_scale = 1.2;
  return std::min(scratch_candidate * original_candidate_scale, 1.0) *
         authored_scalar * original_output_scale;
}

double original_material_scratch_pan(const std::size_t hull_point_index) {
  constexpr double original_pan = 0.7;
  return hull_point_index == 0U || hull_point_index == 3U ? -original_pan
                                                          : original_pan;
}

std::optional<OriginalMaterialSoundSampleWindow>
original_material_sound_sample_window(const std::size_t sample_frame_count,
                                      const std::int32_t authored_leading,
                                      const std::int32_t authored_trailing) {
  constexpr std::uint32_t original_minimum_trim = 100U;
  const auto normalize = [sample_frame_count](const std::int32_t authored) {
    const auto unsigned_authored = static_cast<std::uint32_t>(authored);
    return unsigned_authored < original_minimum_trim
               ? std::size_t{0U}
               : std::min(sample_frame_count,
                          static_cast<std::size_t>(unsigned_authored));
  };
  auto leading = normalize(authored_leading);
  auto trailing = normalize(authored_trailing);
  if (sample_frame_count - trailing < leading) {
    std::swap(leading, trailing);
  }
  if (sample_frame_count - trailing == leading) {
    leading = 0U;
    trailing = 0U;
  }
  if (leading > sample_frame_count - trailing) {
    return std::nullopt;
  }
  return OriginalMaterialSoundSampleWindow{leading, trailing};
}

} // namespace mh::game
