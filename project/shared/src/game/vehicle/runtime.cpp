#include <game/vehicle/runtime.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace mh::game {
namespace {

void translate_local_y(OriginalBodyPoseState &pose, const double distance) {
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    pose.world_position[axis] += pose.body_basis[1U][axis] * distance;
  }
}

void translate_local_y_stored(OriginalBodyPoseState &pose,
                              const double distance) {
  const auto stored_distance =
      static_cast<long double>(static_cast<float>(distance));
  for (std::size_t axis = 0U; axis < pose.world_position.size(); ++axis) {
    pose.world_position[axis] = static_cast<double>(static_cast<float>(
        static_cast<long double>(
            static_cast<float>(pose.world_position[axis])) +
        stored_distance * static_cast<long double>(
                              static_cast<float>(pose.body_basis[1U][axis]))));
  }
}

double vector_length(const std::array<double, 3U> &value) {
  return std::sqrt(value[0U] * value[0U] + value[1U] * value[1U] +
                   value[2U] * value[2U]);
}

std::array<double, 3U> cross(const std::array<double, 3U> &left,
                             const std::array<double, 3U> &right) {
  return {left[1U] * right[2U] - left[2U] * right[1U],
          left[2U] * right[0U] - left[0U] * right[2U],
          left[0U] * right[1U] - left[1U] * right[0U]};
}

std::array<double, 3U> normalized(const std::array<double, 3U> &value) {
  const auto length = vector_length(value);
  if (!std::isfinite(length) || length <= 1.0e-8) {
    return {};
  }
  return {value[0U] / length, value[1U] / length, value[2U] / length};
}

} // namespace

OriginalVehicleRuntime::OriginalVehicleRuntime(
    CollisionWorld world, OriginalVehicleResponseConfig response_config,
    BodyHullRig body_hull, RecoveredVehicleRuntimeTuning drive_tuning,
    OriginalBodyPoseState start_pose, const double maximum_start_adjustment)
    : world_(std::move(world)),
      scene_(std::move(response_config), std::move(body_hull)),
      drive_(std::move(drive_tuning)), start_pose_(start_pose),
      maximum_start_adjustment_(maximum_start_adjustment) {
  if (!std::isfinite(maximum_start_adjustment_) ||
      maximum_start_adjustment_ < 0.0) {
    throw std::invalid_argument(
        "vehicle start adjustment must be finite and nonnegative");
  }
  for (const auto &surface : world_.surfaces()) {
    for (const auto &vertex : surface.vertices) {
      track_floor_ = track_floor_ ? std::min(*track_floor_, vertex[1U])
                                 : vertex[1U];
    }
  }
  reset();
}

OriginalVehicleModeCSceneFrame
OriginalVehicleRuntime::step(const ControlInput &controls,
                             const double slice_seconds) {
  if (pending_interleaved_collision_step_.has_value()) {
    throw std::logic_error(
        "cannot run a complete vehicle step during an interleaved step");
  }
  const auto implicit_outer_frame = !outer_frame_active_;
  if (implicit_outer_frame) {
    begin_outer_frame();
  }
  previous_pose_ = state_.pose;
  auto input = drive_.prepare_step(controls, state_.velocity.local_linear[2U],
                                   slice_seconds);
  input.global_flags = original_global_flags_;
  input.drivetrain.suppress_negative_body_reversal =
      reduced_stabilizer_selector_;
  OriginalVehicleModeCSceneFrame frame;
  frame.drivetrain =
      calculate_original_drivetrain_mode_c_force(input.drivetrain);

  // The captured normal mode-C oracle contains no reverse-gear samples. Using
  // that forward branch verbatim at gear zero applies both its negative drive
  // scalar and its final gear-zero sign reversal, producing forward force.
  // Keep the audited function unchanged and correct only the live integration
  // boundary until a focused retail reverse oracle replaces this provisional
  // ownership rule.
  if (input.drivetrain.state.current_gear_index == 0U) {
    frame.drivetrain.drivetrain_output = -frame.drivetrain.drivetrain_output;
    frame.drivetrain.wheel_dynamic_input.secondary_scalar =
        -frame.drivetrain.wheel_dynamic_input.secondary_scalar;
  }
  frame.scene = scene_.step(
      state_,
      {frame.drivetrain.wheel_dynamic_input, input.surface_scales,
       input.retained_body_contact_count, input.global_flags,
       input.drivetrain.state.slice_seconds,
       frame.drivetrain.controls.brake_force,
       input.drivetrain.authored_brake_force,
       longitudinal_damping_modifier_index_, reduced_stabilizer_selector_,
       retained_traction_accumulator_, input.grip_response_constants,
       input.drivetrain.state.current_gear_index,
       frame.drivetrain.state.primary_drive_transition_active,
       frame.drivetrain.state.direction_transition_active,
       input.handbrake_active},
      world_);
  reduced_stabilizer_selector_ =
      frame.scene.grounded_damping_selection.has_value() &&
      frame.scene.grounded_damping_selection->alternate_branch;
  retained_traction_accumulator_ =
      frame.scene.grounded_damping_selection.has_value()
          ? frame.scene.grounded_damping_selection
                ->retained_traction_accumulator
          : 0.0;
  // sub_000a5e64 clears body+0x72c at the start of an outer frame. The hull
  // query at 0x000a3fb4 replaces it whenever a closer active sample is
  // selected. The pre-frame owner at 0x000a5da2 copies that final material to
  // car+0x154 for the following outer frame.
  for (const auto &sample : frame.scene.body_hull_reaction_samples) {
    if (sample.hit.has_value()) {
      retained_hull_material_index_ =
          static_cast<std::int32_t>(sample.hit->material);
    }
  }
  drive_.commit_step(frame.drivetrain);
  apply_manual_gear_overspeed_response(input.drivetrain.state.slice_seconds);
  if (implicit_outer_frame) {
    end_outer_frame();
  }
  return frame;
}

void OriginalVehicleRuntime::begin_interleaved_collision_step(
    const ControlInput &controls, const double slice_seconds) {
  if (pending_interleaved_collision_step_.has_value()) {
    throw std::logic_error("vehicle interleaved step is already active");
  }
  PendingInterleavedCollisionStep pending;
  pending.implicit_outer_frame = !outer_frame_active_;
  if (pending.implicit_outer_frame) {
    begin_outer_frame();
  }
  previous_pose_ = state_.pose;
  auto input = drive_.prepare_step(controls, state_.velocity.local_linear[2U],
                                   slice_seconds);
  input.global_flags = original_global_flags_;
  input.drivetrain.suppress_negative_body_reversal =
      reduced_stabilizer_selector_;
  pending.frame.drivetrain =
      calculate_original_drivetrain_mode_c_force(input.drivetrain);
  if (input.drivetrain.state.current_gear_index == 0U) {
    pending.frame.drivetrain.drivetrain_output =
        -pending.frame.drivetrain.drivetrain_output;
    pending.frame.drivetrain.wheel_dynamic_input.secondary_scalar =
        -pending.frame.drivetrain.wheel_dynamic_input.secondary_scalar;
  }
  pending.scene_stage = scene_.begin_collision_step(
      state_,
      {pending.frame.drivetrain.wheel_dynamic_input, input.surface_scales,
       input.retained_body_contact_count, input.global_flags,
       input.drivetrain.state.slice_seconds,
       pending.frame.drivetrain.controls.brake_force,
       input.drivetrain.authored_brake_force,
       longitudinal_damping_modifier_index_, reduced_stabilizer_selector_,
       retained_traction_accumulator_, input.grip_response_constants,
       input.drivetrain.state.current_gear_index,
       pending.frame.drivetrain.state.primary_drive_transition_active,
       pending.frame.drivetrain.state.direction_transition_active,
       input.handbrake_active},
      world_);
  // Scene staging owns the physics-origin matrix. Dynamic vehicle contact and
  // all public consumers use the car object origin, so restore it between
  // static passes.
  constexpr double original_car_physics_local_y_shift = 0.32;
  translate_local_y_stored(state_.pose, original_car_physics_local_y_shift);
  pending_interleaved_collision_step_ = std::move(pending);
}

void OriginalVehicleRuntime::set_original_global_flags(
    const std::uint8_t flags) noexcept {
  original_global_flags_ = flags;
}

bool OriginalVehicleRuntime::apply_interleaved_static_collision_pass() {
  if (!pending_interleaved_collision_step_.has_value()) {
    throw std::logic_error("vehicle interleaved step is not active");
  }
  constexpr double original_car_physics_local_y_shift = 0.32;
  translate_local_y_stored(state_.pose, -original_car_physics_local_y_shift);
  try {
    const auto result = scene_.apply_static_collision_pass(
        state_, pending_interleaved_collision_step_->scene_stage, world_);
    translate_local_y_stored(state_.pose, original_car_physics_local_y_shift);
    return result;
  } catch (...) {
    translate_local_y_stored(state_.pose, original_car_physics_local_y_shift);
    throw;
  }
}

OriginalVehicleModeCSceneFrame
OriginalVehicleRuntime::finish_interleaved_collision_step() {
  if (!pending_interleaved_collision_step_.has_value()) {
    throw std::logic_error("vehicle interleaved step is not active");
  }
  auto pending = std::move(*pending_interleaved_collision_step_);
  pending_interleaved_collision_step_.reset();
  constexpr double original_car_physics_local_y_shift = 0.32;
  translate_local_y_stored(state_.pose, -original_car_physics_local_y_shift);
  pending.frame.scene =
      scene_.finish_collision_step(state_, pending.scene_stage, world_);
  reduced_stabilizer_selector_ =
      pending.frame.scene.grounded_damping_selection.has_value() &&
      pending.frame.scene.grounded_damping_selection->alternate_branch;
  retained_traction_accumulator_ =
      pending.frame.scene.grounded_damping_selection.has_value()
          ? pending.frame.scene.grounded_damping_selection
                ->retained_traction_accumulator
          : 0.0;
  for (const auto &sample : pending.frame.scene.body_hull_reaction_samples) {
    if (sample.hit.has_value()) {
      retained_hull_material_index_ =
          static_cast<std::int32_t>(sample.hit->material);
    }
  }
  drive_.commit_step(pending.frame.drivetrain);
  apply_manual_gear_overspeed_response(
      pending.scene_stage.inputs.slice_seconds);
  if (pending.implicit_outer_frame) {
    end_outer_frame();
  }
  return std::move(pending.frame);
}

bool OriginalVehicleRuntime::interleaved_collision_step_active()
    const noexcept {
  return pending_interleaved_collision_step_.has_value();
}

void OriginalVehicleRuntime::apply_manual_gear_overspeed_response(
    const double slice_seconds) {
  const auto ceiling = drive_.active_manual_speed_ceiling();
  if (!ceiling.has_value() || !std::isfinite(slice_seconds) ||
      slice_seconds <= 0.0) {
    return;
  }
  auto &longitudinal_velocity = state_.velocity.local_linear[2U];
  const auto speed = std::fabs(longitudinal_velocity);
  if (speed <= *ceiling) {
    return;
  }
  // A downshift does not teleport the body to the lower ratio's road speed.
  // Retain one percent of the excess per second: this removes 90 percent in
  // half a second while remaining continuous at every fixed-step size.
  constexpr double overspeed_retention_per_second = 0.01;
  const auto retained_overspeed =
      (speed - *ceiling) *
      std::pow(overspeed_retention_per_second, slice_seconds);
  longitudinal_velocity = std::copysign(*ceiling + retained_overspeed,
                                        longitudinal_velocity);
  // The scene retains the preceding body velocity for its next correction
  // pass. Keep that history at the committed gearbox boundary as well, or the
  // following slice can restore part of the discarded overspeed.
  scene_.seed_previous_vehicle_velocity(state_.velocity);
}

void OriginalVehicleRuntime::begin_outer_frame() {
  if (outer_frame_active_) {
    throw std::logic_error("vehicle outer frame is already active");
  }
  longitudinal_damping_modifier_index_ = retained_hull_material_index_;
  retained_hull_material_index_ = -1;
  outer_frame_active_ = true;
}

void OriginalVehicleRuntime::end_outer_frame() {
  if (!outer_frame_active_) {
    throw std::logic_error("vehicle outer frame is not active");
  }
  outer_frame_active_ = false;
}

void OriginalVehicleRuntime::advance_drive_state_only(
    const ControlInput &controls, const double slice_seconds) {
  drive_.advance_state_only(controls, state_.velocity.local_linear[2U],
                            slice_seconds);
}

void OriginalVehicleRuntime::apply_drive_performance_scale(const float factor) {
  drive_.apply_performance_scale(factor);
}

void OriginalVehicleRuntime::set_automatic_transmission(
    const bool automatic) noexcept {
  drive_.set_automatic_transmission(automatic);
}

void OriginalVehicleRuntime::settle_suspension_on_grid(
    const double slice_seconds) {
  if (!std::isfinite(slice_seconds) || slice_seconds <= 0.0 ||
      slice_seconds > 0.04) {
    throw std::invalid_argument(
        "grid suspension settlement slice must be within (0, 0.04]");
  }
  const auto held_pose = state_.pose;
  static_cast<void>(step({}, slice_seconds));
  state_.pose.body_basis = held_pose.body_basis;
  state_.pose.world_position[0U] = held_pose.world_position[0U];
  state_.pose.world_position[2U] = held_pose.world_position[2U];
  state_.velocity.local_linear[0U] = 0.0;
  state_.velocity.local_linear[2U] = 0.0;
  state_.velocity.local_angular = {};
  previous_pose_ = state_.pose;
}

void OriginalVehicleRuntime::seed_wheel_contact_history(
    const std::array<float, 4U> &state_fractions) {
  seed_wheel_contact_history(state_fractions, {1U, 1U, 1U, 1U});
}

void OriginalVehicleRuntime::seed_wheel_contact_history(
    const std::array<float, 4U> &state_fractions,
    const std::array<std::uint32_t, 4U> &previous_contact_flags) {
  std::array<WheelContactScalarState, 4U> history{};
  std::transform(
      state_fractions.begin(), state_fractions.end(), history.begin(),
      [](const float fraction) {
        if (!std::isfinite(fraction) || fraction < 0.0F || fraction > 1.0F) {
          throw std::invalid_argument(
              "wheel contact history fraction must be within [0, 1]");
        }
        return WheelContactScalarState{
            static_cast<double>(fraction),
            static_cast<double>(static_cast<float>(1.0F - fraction))};
      });
  if (std::any_of(previous_contact_flags.begin(), previous_contact_flags.end(),
                  [](const std::uint32_t flag) { return flag > 1U; })) {
    throw std::invalid_argument(
        "previous wheel contact flags must be zero or one");
  }
  scene_.seed_wheel_contact_history(history);
  scene_.seed_previous_wheel_contact_flags(previous_contact_flags);
}

void OriginalVehicleRuntime::seed_captured_drive_state(
    const OriginalVehicleDriveState &state) {
  drive_.seed_captured_state(state);
}

void OriginalVehicleRuntime::seed_reduced_stabilizer_selector(
    const bool active) noexcept {
  reduced_stabilizer_selector_ = active;
}

void OriginalVehicleRuntime::seed_retained_traction_accumulator(
    const double value) {
  if (!std::isfinite(value) || value < 0.0) {
    throw std::invalid_argument(
        "retained traction accumulator must be finite and nonnegative");
  }
  retained_traction_accumulator_ = value;
}

void OriginalVehicleRuntime::seed_longitudinal_damping_modifier_index(
    const std::int32_t material_index) {
  if (material_index < -1) {
    throw std::invalid_argument(
        "longitudinal damping material index must be -1 or nonnegative");
  }
  if (outer_frame_active_) {
    throw std::logic_error(
        "cannot seed damping material during an active outer frame");
  }
  retained_hull_material_index_ = material_index;
}

void OriginalVehicleRuntime::seed_captured_body_state(
    const OriginalBodyPoseState &body_pose,
    const OriginalBodyVelocityState &velocity,
    const OriginalBodyVelocityState &previous_velocity) {
  const auto finite = [](const auto &values) {
    return std::all_of(values.begin(), values.end(),
                       [](const double value) { return std::isfinite(value); });
  };
  if (!finite(body_pose.world_position) ||
      !std::all_of(body_pose.body_basis.begin(), body_pose.body_basis.end(),
                   finite) ||
      !finite(velocity.local_linear) || !finite(velocity.local_angular) ||
      !finite(previous_velocity.local_linear) ||
      !finite(previous_velocity.local_angular)) {
    throw std::invalid_argument(
        "captured vehicle physics state must be finite");
  }
  state_.pose = body_pose;
  state_.velocity = velocity;
  previous_pose_ = state_.pose;
  scene_.seed_previous_vehicle_velocity(previous_velocity);
}

OriginalVehicleRaceRecoveryResult OriginalVehicleRuntime::update_race_recovery(
    const OriginalVehicleModeCSceneFrame &frame,
    const OriginalVehicleRaceRecoveryInput &input) {
  if (!std::isfinite(input.slice_seconds) || input.slice_seconds <= 0.0) {
    throw std::invalid_argument("race recovery slice must be positive");
  }
  if (std::any_of(input.observer_position.begin(),
                  input.observer_position.end(),
                  [](const double value) { return !std::isfinite(value); })) {
    throw std::invalid_argument("race recovery observer must be finite");
  }
  if (std::any_of(input.recovery_spline_direction.begin(),
                  input.recovery_spline_direction.end(),
                  [](const double value) { return !std::isfinite(value); })) {
    throw std::invalid_argument(
        "race recovery spline direction must be finite");
  }

  // p3.1 stores this input as binary32 and caps only values above one-half.
  const auto slice = static_cast<float>(
      std::min(input.slice_seconds, static_cast<double>(0.5F)));
  race_recovery_.external_recovery_active = input.external_recovery_active;
  OriginalVehicleRaceRecoveryResult result;
  const auto request_reset =
      [&](const OriginalVehicleRaceRecoveryReason reason) {
        result.reason = reason;
        if (input.reset_enabled && !input.reset_inhibited) {
          result.reset_applied =
              apply_race_recovery_reset(input.recovery_spline_direction,
                                        input.recovery_spline_position);
        }
      };

  if (input.recover_below_track && track_floor_) {
    const auto pose = physics_pose();
    const auto &wheels = frame.scene.response.contacts.wheels;
    const auto grounded = std::all_of(
        wheels.begin(), wheels.end(),
        [](const auto &wheel) { return wheel.hit.has_value(); });
    if (grounded && pose.body_basis[1U][1U] > 0.8 &&
        pose.world_position[1U] >= *track_floor_ - 1.0) {
      last_grounded_pose_ = state_.pose;
    }
    // Include invisible collision surfaces; jumps and shortcuts remain valid.
    const auto unsupported = std::none_of(
        wheels.begin(), wheels.end(),
        [](const auto &wheel) { return wheel.hit.has_value(); });
    if (unsupported && pose.world_position[1U] < *track_floor_ - 10.0 &&
        last_grounded_pose_) {
      result.reason = OriginalVehicleRaceRecoveryReason::below_world;
      if (input.reset_enabled && !input.reset_inhibited) {
        const auto safe_pose = *last_grounded_pose_;
        result.reset_applied = apply_race_recovery_reset(
            safe_pose.body_basis[2U], safe_pose.world_position);
        if (result.reset_applied) {
          // Restore the grounded object transform, including banked roads.
          state_.pose = safe_pose;
          state_.pose.world_position[1U] += 0.05;
          previous_pose_ = state_.pose;
        }
      }
      return result;
    }
  }

  // The caller at 0x0007826c performs this lower-world check before invoking
  // the timer owner.
  if (!input.recover_below_track &&
      static_cast<float>(physics_pose().world_position[1U]) < -100.0F) {
    request_reset(OriginalVehicleRaceRecoveryReason::below_world);
    return result;
  }

  if (race_recovery_.external_recovery_active) {
    race_recovery_.external_recovery_seconds = static_cast<float>(
        static_cast<double>(race_recovery_.external_recovery_seconds) +
        static_cast<double>(slice));
  }
  if (static_cast<double>(race_recovery_.external_recovery_seconds) > 2.0) {
    request_reset(OriginalVehicleRaceRecoveryReason::external_state_timeout);
    return result;
  }

  std::array<std::uint32_t, 4U> contact_flags{};
  for (std::size_t wheel = 0U; wheel < contact_flags.size(); ++wheel) {
    contact_flags[wheel] =
        frame.scene.response.contacts.wheels[wheel].hit.has_value() ? 1U : 0U;
  }
  const auto contact_count =
      std::accumulate(contact_flags.begin(), contact_flags.end(), 0U);
  auto accumulate_insufficient_contact = contact_count < 2U;
  if (contact_count == 2U) {
    const auto left_side = contact_flags[0U] + contact_flags[3U];
    const auto right_side = contact_flags[1U] + contact_flags[2U];
    accumulate_insufficient_contact = left_side != 2U && right_side != 2U;
  }
  if (accumulate_insufficient_contact) {
    race_recovery_.insufficient_contact_seconds = static_cast<float>(
        static_cast<double>(race_recovery_.insufficient_contact_seconds) +
        static_cast<double>(slice));
  }
  if (contact_count > 2U) {
    // Ground contact must not cancel an explicit off-route relocation.
    if (!input.recovery_spline_position.has_value()) {
      race_recovery_.external_recovery_seconds = 0.0F;
    }
    race_recovery_.insufficient_contact_seconds = 0.0F;
  }
  if (static_cast<double>(race_recovery_.insufficient_contact_seconds) > 8.0) {
    request_reset(
        OriginalVehicleRaceRecoveryReason::insufficient_contact_timeout);
    return result;
  }

  if (!input.ai_vehicle) {
    return result;
  }
  if (std::abs(static_cast<float>(state_.velocity.local_linear[2U])) > 15.0F) {
    race_recovery_.ai_recovery_phase_active = false;
  }
  if (input.ai_stuck_recovery_active &&
      !race_recovery_.ai_recovery_phase_active) {
    race_recovery_.ai_recovery_phase_active = true;
    race_recovery_.ai_recovery_phase_seconds = 0.0F;
  }
  if (!race_recovery_.ai_recovery_phase_active) {
    return result;
  }
  race_recovery_.ai_recovery_phase_seconds = static_cast<float>(
      static_cast<double>(race_recovery_.ai_recovery_phase_seconds) +
      static_cast<double>(slice));
  if (static_cast<double>(race_recovery_.ai_recovery_phase_seconds) <= 8.0) {
    return result;
  }

  const auto pose = physics_pose();
  auto distance_squared = 0.0L;
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    const auto delta = static_cast<long double>(
                           static_cast<float>(pose.world_position[axis])) -
                       static_cast<long double>(
                           static_cast<float>(input.observer_position[axis]));
    distance_squared += delta * delta;
  }
  if (distance_squared > 22500.0L) {
    race_recovery_.ai_recovery_phase_active = false;
    request_reset(OriginalVehicleRaceRecoveryReason::distant_ai_stuck_timeout);
  }
  return result;
}

void OriginalVehicleRuntime::reset() {
  last_grounded_pose_.reset();
  pending_interleaved_collision_step_.reset();
  scene_.reset();
  drive_.reset();
  race_recovery_ = {};
  longitudinal_damping_modifier_index_ = -1;
  retained_hull_material_index_ = -1;
  reduced_stabilizer_selector_ = false;
  retained_traction_accumulator_ = 0.0;
  outer_frame_active_ = false;
  state_ = {};
  if (maximum_start_adjustment_ == 0.0) {
    // The p3.1 primary vehicle initializer retains the parameter-zero motion
    // spline plus authored StartGrid transform verbatim. The response owner
    // performs its temporary -0.32/+0.32 object-origin shift during each
    // slice; startup itself does not snap the public object matrix to wheel
    // contact.
    state_.pose = start_pose_;
    previous_pose_ = state_.pose;
    scene_.seed_previous_vehicle_velocity({});
    scene_.seed_previous_wheel_contact_count(0U);
    return;
  }
  const auto placement = place_vehicle_for_full_wheel_contact(
      scene_.config().wheel_rig, start_pose_, world_,
      maximum_start_adjustment_);
  if (!placement.has_value()) {
    throw std::runtime_error(
        "vehicle start pose did not admit full-wheel placement");
  }
  state_.pose = placement->pose;
  constexpr double original_car_object_origin_y = 0.32;
  translate_local_y(state_.pose, original_car_object_origin_y);
  previous_pose_ = state_.pose;

  std::array<WheelContactScalarState, 4U> wheel_history{};
  std::transform(placement->spring_segments.begin(),
                 placement->spring_segments.end(), wheel_history.begin(),
                 [](const WheelSpringSegmentSample &sample) {
                   return sample.scalar_state;
                 });
  scene_.seed_wheel_contact_history(wheel_history);
  scene_.seed_previous_vehicle_velocity({});
  scene_.seed_previous_wheel_contact_count(4U);
}

const OriginalVehicleSceneState &
OriginalVehicleRuntime::state() const noexcept {
  return state_;
}

OriginalBodyPoseState OriginalVehicleRuntime::physics_pose() const noexcept {
  auto result = state_.pose;
  constexpr double original_car_object_origin_y = -0.32;
  translate_local_y(result, original_car_object_origin_y);
  return result;
}

OriginalBodyPoseState OriginalVehicleRuntime::ai_pose() const {
  auto result = state_.pose;
  const auto forward_x = static_cast<float>(result.body_basis[2U][0U]);
  const auto forward_z = static_cast<float>(result.body_basis[2U][2U]);
  const auto x = static_cast<long double>(forward_x);
  const auto z = static_cast<long double>(forward_z);
  const auto horizontal_length = static_cast<float>(std::sqrt(x * x + z * z));
  if (!std::isfinite(horizontal_length) || horizontal_length == 0.0F) {
    throw std::runtime_error(
        "vehicle AI pose has no finite horizontal forward vector");
  }
  result.body_basis[2U][0U] = static_cast<double>(
      static_cast<float>(x / static_cast<long double>(horizontal_length)));
  result.body_basis[2U][2U] = static_cast<double>(
      static_cast<float>(z / static_cast<long double>(horizontal_length)));
  return result;
}

std::array<WheelSpringSegmentSample, 4U>
OriginalVehicleRuntime::current_wheel_contacts() const {
  const auto &config = scene_.config();
  return sample_wheel_spring_segments(
      config.wheel_rig,
      make_original_wheel_response(config.wheel_rig.authored_spring_length,
                                   config.wheel_rig.authored_spring_strength,
                                   config.response_profile),
      physics_pose(), world_);
}

const OriginalBodyPoseState &
OriginalVehicleRuntime::previous_pose() const noexcept {
  return previous_pose_;
}

const OriginalVehicleDriveSystem &
OriginalVehicleRuntime::drive() const noexcept {
  return drive_;
}

std::int32_t
OriginalVehicleRuntime::longitudinal_damping_modifier_index() const noexcept {
  return retained_hull_material_index_;
}

const OriginalVehicleRaceRecoveryState &
OriginalVehicleRuntime::race_recovery_state() const noexcept {
  return race_recovery_;
}

const CollisionWorld &OriginalVehicleRuntime::world() const noexcept {
  return world_;
}

bool OriginalVehicleRuntime::apply_race_recovery_reset(
    const std::array<double, 3U> &spline_direction,
    const std::optional<std::array<double, 3U>> &spline_position) {
  auto pose = physics_pose();
  const auto forward = normalized(spline_direction);
  if (vector_length(forward) <= 1.0e-8) {
    return false;
  }
  auto right =
      normalized(std::array<double, 3U>{forward[2U], 0.0, -forward[0U]});
  auto up = normalized(cross(forward, right));
  if (vector_length(right) <= 1.0e-8 || vector_length(up) <= 1.0e-8) {
    return false;
  }
  pose.body_basis = {right, up, forward};
  if (spline_position.has_value()) {
    if (std::any_of(spline_position->begin(), spline_position->end(),
                    [](const double value) { return !std::isfinite(value); })) {
      return false;
    }
    pose.world_position = *spline_position;
  }

  scene_.reset();
  drive_.reset_retained_state();
  race_recovery_ = {};
  longitudinal_damping_modifier_index_ = -1;
  retained_hull_material_index_ = -1;
  state_.velocity = {};
  // sub_0001d230 clears retained drivetrain/body state. sub_0003b710 then
  // preserves the object's position and points it along the selected track
  // spline tangent through sub_000433d8.
  state_.pose = pose;
  constexpr double original_car_object_origin_y = 0.32;
  translate_local_y(state_.pose, original_car_object_origin_y);
  previous_pose_ = state_.pose;
  scene_.seed_previous_vehicle_velocity({});
  scene_.seed_previous_wheel_contact_count(0U);
  return true;
}

void OriginalVehicleRuntime::apply_dynamic_contact(
    const CollisionVector3 &world_position_delta,
    const CollisionVector3 &local_linear_velocity_delta,
    const CollisionVector3 &local_angular_velocity_delta) {
  const auto finite = [](const CollisionVector3 &value) {
    return std::all_of(value.begin(), value.end(), [](const double component) {
      return std::isfinite(component);
    });
  };
  if (!finite(world_position_delta) || !finite(local_linear_velocity_delta) ||
      !finite(local_angular_velocity_delta)) {
    throw std::invalid_argument("dynamic vehicle contact delta must be finite");
  }
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    state_.pose.world_position[axis] = static_cast<double>(static_cast<float>(
        static_cast<float>(state_.pose.world_position[axis]) +
        static_cast<float>(world_position_delta[axis])));
    state_.velocity.local_linear[axis] = static_cast<double>(static_cast<float>(
        static_cast<float>(state_.velocity.local_linear[axis]) +
        static_cast<float>(local_linear_velocity_delta[axis])));
    state_.velocity.local_angular[axis] =
        static_cast<double>(static_cast<float>(
            static_cast<float>(state_.velocity.local_angular[axis]) +
            static_cast<float>(local_angular_velocity_delta[axis])));
  }
}

std::vector<OriginalDynamicVehicleContactScheduleEvent>
apply_original_dynamic_vehicle_contact_schedule(
    const std::span<const OriginalDynamicVehicleContactScheduleParticipant>
        participants) {
  for (const auto &participant : participants) {
    if (participant.vehicle == nullptr || participant.shape == nullptr) {
      throw std::invalid_argument(
          "dynamic contact schedule participant must be complete");
    }
  }

  std::vector<OriginalDynamicVehicleContactScheduleEvent> events;
  constexpr std::size_t original_contact_pass_limit = 3U;
  for (std::size_t first_index = 0U; first_index < participants.size();
       ++first_index) {
    const auto &first = participants[first_index];
    bool preceding_dynamic_contact = true;
    for (std::size_t pass = 0U; pass < original_contact_pass_limit; ++pass) {
      const auto preceding_static_contact = pass < first.static_reaction_passes;
      if (pass != 0U && !preceding_static_contact &&
          !preceding_dynamic_contact) {
        break;
      }
      preceding_dynamic_contact = false;
      for (std::size_t second_index = 0U; second_index < participants.size();
           ++second_index) {
        if (first_index == second_index) {
          continue;
        }
        const auto &second = participants[second_index];
        OriginalDynamicVehicleContactResponse response;
        const auto first_before = first.vehicle->state();
        const auto second_before = second.vehicle->state();
        if (!resolve_original_dynamic_vehicle_contact(
                first_before, *first.shape, second_before, *second.shape,
                response)) {
          continue;
        }
        preceding_dynamic_contact = true;
        first.vehicle->apply_dynamic_contact(
            response.first_world_position_delta,
            response.first_local_linear_velocity_delta,
            response.first_local_angular_velocity_delta);
        second.vehicle->apply_dynamic_contact(
            response.second_world_position_delta,
            response.second_local_linear_velocity_delta,
            response.second_local_angular_velocity_delta);
        events.push_back({pass, first_index, second_index, first_before,
                          second_before, response, first.vehicle->state(),
                          second.vehicle->state()});
      }
    }
  }
  return events;
}

OriginalInterleavedVehicleContactScheduleResult
apply_original_interleaved_vehicle_contact_schedule(
    const std::span<const OriginalDynamicVehicleContactScheduleParticipant>
        participants) {
  for (const auto &participant : participants) {
    if (participant.vehicle == nullptr || participant.shape == nullptr) {
      throw std::invalid_argument(
          "interleaved contact schedule participant must be complete");
    }
    if (!participant.vehicle->interleaved_collision_step_active()) {
      throw std::logic_error(
          "interleaved contact participant has no active staged step");
    }
  }

  OriginalInterleavedVehicleContactScheduleResult result;
  result.frames.resize(participants.size());
  constexpr std::size_t original_contact_pass_limit = 3U;
  for (std::size_t first_index = 0U; first_index < participants.size();
       ++first_index) {
    const auto &first = participants[first_index];
    bool preceding_static_contact = true;
    bool preceding_dynamic_contact = true;
    for (std::size_t pass = 0U; pass < original_contact_pass_limit; ++pass) {
      if (pass != 0U && !preceding_static_contact &&
          !preceding_dynamic_contact) {
        break;
      }
      preceding_static_contact =
          first.vehicle->apply_interleaved_static_collision_pass();
      preceding_dynamic_contact = false;
      for (std::size_t second_index = 0U; second_index < participants.size();
           ++second_index) {
        if (first_index == second_index) {
          continue;
        }
        const auto &second = participants[second_index];
        OriginalDynamicVehicleContactResponse response;
        const auto first_before = first.vehicle->state();
        const auto second_before = second.vehicle->state();
        if (!resolve_original_dynamic_vehicle_contact(
                first_before, *first.shape, second_before, *second.shape,
                response)) {
          continue;
        }
        preceding_dynamic_contact = true;
        first.vehicle->apply_dynamic_contact(
            response.first_world_position_delta,
            response.first_local_linear_velocity_delta,
            response.first_local_angular_velocity_delta);
        second.vehicle->apply_dynamic_contact(
            response.second_world_position_delta,
            response.second_local_linear_velocity_delta,
            response.second_local_angular_velocity_delta);
        result.events.push_back(
            {pass, first_index, second_index, first_before, second_before,
             response, first.vehicle->state(), second.vehicle->state()});
      }
    }
    // Scheduler 0x000a5b8f..0x000a5c06 performs final hull stabilization,
    // record retention, and outer-body advancement in this order.
    result.frames[first_index] =
        first.vehicle->finish_interleaved_collision_step();
  }
  return result;
}

OriginalVehicleEnvironmentContactScheduleResult
apply_original_vehicle_environment_contact_schedule(
    const std::span<const OriginalDynamicVehicleContactScheduleParticipant>
        vehicles,
    const std::span<const OriginalEnvironmentDynamicContactParticipant>
        environment_bodies) {
  for (const auto &vehicle : vehicles) {
    if (vehicle.vehicle == nullptr || vehicle.shape == nullptr) {
      throw std::invalid_argument(
          "environment contact vehicle participant must be complete");
    }
    if (vehicle.vehicle->interleaved_collision_step_active()) {
      throw std::logic_error(
          "environment contact requires finalized vehicle steps");
    }
  }
  for (const auto &environment : environment_bodies) {
    if (environment.body == nullptr || environment.shape == nullptr) {
      throw std::invalid_argument(
          "environment direct contact participant must be complete");
    }
  }

  OriginalVehicleEnvironmentContactScheduleResult result;
  result.vehicle_collisions.resize(vehicles.size());
  const auto apply_environment_delta =
      [](OriginalVehicleSceneState &state,
         const CollisionVector3 &world_position_delta,
         const CollisionVector3 &local_linear_velocity_delta,
         const CollisionVector3 &local_angular_velocity_delta) {
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
          state.pose.world_position[axis] =
              static_cast<double>(static_cast<float>(
                  static_cast<float>(state.pose.world_position[axis]) +
                  static_cast<float>(world_position_delta[axis])));
          state.velocity.local_linear[axis] =
              static_cast<double>(static_cast<float>(
                  static_cast<float>(state.velocity.local_linear[axis]) +
                  static_cast<float>(local_linear_velocity_delta[axis])));
          state.velocity.local_angular[axis] =
              static_cast<double>(static_cast<float>(
                  static_cast<float>(state.velocity.local_angular[axis]) +
                  static_cast<float>(local_angular_velocity_delta[axis])));
        }
      };

  for (std::size_t vehicle_index = 0U; vehicle_index < vehicles.size();
       ++vehicle_index) {
    const auto &vehicle = vehicles[vehicle_index];
    for (std::size_t environment_index = 0U;
         environment_index < environment_bodies.size(); ++environment_index) {
      const auto &environment = environment_bodies[environment_index];
      const auto vehicle_before = vehicle.vehicle->state();
      const auto environment_before = *environment.body;
      OriginalDynamicVehicleContactResponse response;
      if (!resolve_original_direct_body_contact(vehicle_before, *vehicle.shape,
                                                environment_before,
                                                *environment.shape, response)) {
        continue;
      }

      vehicle.vehicle->apply_dynamic_contact(
          response.first_world_position_delta,
          response.first_local_linear_velocity_delta,
          response.first_local_angular_velocity_delta);
      apply_environment_delta(*environment.body,
                              response.second_world_position_delta,
                              response.second_local_linear_velocity_delta,
                              response.second_local_angular_velocity_delta);

      auto &collision = result.vehicle_collisions[vehicle_index];
      collision.active = true;
      collision.collision_identifier = environment.collision_identifier;
      collision.maximum_collision_sound_scalar =
          std::max(collision.maximum_collision_sound_scalar,
                   response.collision_sound_scalar);
      result.events.push_back({vehicle_index, environment_index, vehicle_before,
                               environment_before, response,
                               vehicle.vehicle->state(), *environment.body});
    }
  }
  return result;
}

OriginalEnvironmentBodyStaticContactResult
apply_original_environment_body_static_contact_schedule(
    OriginalVehicleSceneState &body,
    const OriginalDynamicVehicleContactShape &shape,
    const OriginalBodyPoseState &physics_previous_pose,
    const CollisionWorld &world, const double slice_seconds) {
  if (shape.local_points.empty()) {
    throw std::invalid_argument(
        "environment static-contact shape has no hull points");
  }
  if (!std::isfinite(slice_seconds) || slice_seconds < 0.0) {
    throw std::invalid_argument(
        "environment static-contact slice must be finite and non-negative");
  }

  BodyHullRig rig;
  rig.local_points = shape.local_points;
  BodyHullContactSystem hull(std::move(rig));
  OriginalEnvironmentBodyStaticContactResult result;
  std::optional<BodyHullContactFrame> retained;
  constexpr std::size_t original_static_track_pass_limit = 3U;
  auto last_pass_hit = false;
  for (std::size_t pass = 0U; pass < original_static_track_pass_limit; ++pass) {
    ++result.passes_attempted;
    auto pass_frame = hull.sample(physics_previous_pose, body.pose, world);
    if (!retained.has_value()) {
      retained = pass_frame;
    } else {
      for (std::size_t index = 0U; index < pass_frame.samples.size(); ++index) {
        if (pass_frame.samples[index].hit.has_value()) {
          retained->samples[index] = pass_frame.samples[index];
        }
      }
      retained->retained_contact_count = static_cast<std::size_t>(
          std::count_if(retained->samples.begin(), retained->samples.end(),
                        [](const BodyHullContactSample &sample) {
                          return sample.hit.has_value();
                        }));
    }

    const auto selected = select_original_body_hull_reaction_sample(pass_frame);
    last_pass_hit = selected.has_value();
    if (!selected.has_value()) {
      break;
    }
    const auto &sample = pass_frame.samples[*selected];
    result.reaction_samples.push_back(sample);
    const CollisionVector3 world_sweep{
        sample.segment_end[0U] - sample.segment_start[0U],
        sample.segment_end[1U] - sample.segment_start[1U],
        sample.segment_end[2U] - sample.segment_start[2U]};
    result.reactions.push_back(calculate_original_body_hull_reaction(
        {body.pose.body_basis, sample.local_point, shape.local_center_of_mass,
         sample.hit->normal, world_sweep, body.velocity, shape.mass_properties,
         *sample.hit_fraction, false}));
    static_cast<void>(apply_original_body_hull_reaction(
        body.pose, body.velocity, result.reactions.back(), slice_seconds));
  }

  if (retained.has_value()) {
    result.retained_body_hull = std::move(*retained);
  } else {
    result.retained_body_hull.samples.resize(shape.local_points.size());
  }
  if (last_pass_hit) {
    const auto post_frame =
        hull.sample(physics_previous_pose, body.pose, world);
    result.post_stabilization =
        apply_original_generic_body_hull_post_stabilization(
            body.pose, body.velocity, post_frame, shape.local_center_of_mass,
            slice_seconds);
  }
  return result;
}

} // namespace mh::game
