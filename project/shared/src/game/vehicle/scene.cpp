#include <game/vehicle/scene.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mh::game {
namespace {

void translate_pose_local_y(OriginalBodyPoseState &pose,
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

struct GroundedMaterialAggregate {
  std::size_t count = 0U;
  double lateral_threshold = 0.0;
  double yaw_threshold = 0.0;
  double alternate_lateral = 0.0;
  double lateral = 0.0;
  double alternate_longitudinal = 0.0;
  double longitudinal = 0.0;
  double alternate_angular_y = 0.0;
  double angular_y = 0.0;
};

GroundedMaterialAggregate aggregate_grounded_materials(
    const WheelContactFrame &contacts,
    const OriginalVehicleGroundedMaterialTable &materials) {
  GroundedMaterialAggregate result;
  for (const auto &wheel : contacts.wheels) {
    if (!wheel.hit.has_value()) {
      continue;
    }
    // p3.1 clamps a query material above 25 to row zero before multiplying
    // the index by the 0x4c0-byte row stride.
    const auto material_index =
        wheel.hit->material < materials.size()
            ? static_cast<std::size_t>(wheel.hit->material)
            : 0U;
    const auto &material = materials[material_index];
    const auto normal_y = static_cast<float>(wheel.hit->normal[1U]);
    const auto normal_y_cube =
        static_cast<float>(normal_y * normal_y * normal_y);
    const auto normal_weight =
        normal_y_cube == 0.0F ? 10000.0F : 1.0F / normal_y_cube;
    result.lateral_threshold += material.lateral_threshold;
    result.yaw_threshold += material.yaw_threshold;
    result.alternate_lateral +=
        static_cast<double>(material.alternate_lateral_base) *
        static_cast<double>(normal_weight);
    result.lateral += static_cast<double>(material.lateral_base) *
                      static_cast<double>(normal_weight);
    result.alternate_longitudinal += material.alternate_longitudinal_base;
    result.longitudinal += material.longitudinal_base;
    result.alternate_angular_y += material.alternate_angular_y_base;
    result.angular_y += material.angular_y_base;
    ++result.count;
  }
  return result;
}

} // namespace

OriginalVehicleGroundedDampingProfile
calculate_original_vehicle_grounded_damping_profile(
    const WheelContactFrame &contacts,
    const std::array<float, 4U> &grip_response_constants,
    const OriginalVehicleGroundedMaterialTable &materials) {
  auto profile = OriginalVehicleGroundedDampingProfile{};
  if (std::any_of(grip_response_constants.begin(),
                  grip_response_constants.end(), [](const float value) {
                    return !std::isfinite(value) || value <= 0.0F;
                  })) {
    throw std::invalid_argument("grounded damping grip constants are invalid");
  }

  const auto aggregate = aggregate_grounded_materials(contacts, materials);
  if (aggregate.count == 0U) {
    return profile;
  }

  const auto inverse_count = 1.0 / static_cast<double>(aggregate.count);
  const auto lateral_owner_coefficient =
      static_cast<double>(grip_response_constants[2U]);
  const auto angular_owner_coefficient =
      static_cast<double>(grip_response_constants[3U]);
  profile.lateral_linear_base =
      aggregate.lateral * inverse_count * lateral_owner_coefficient;
  profile.longitudinal_linear_base = aggregate.longitudinal * inverse_count;
  profile.angular_bases[1U] =
      aggregate.angular_y * inverse_count * angular_owner_coefficient;
  if (!std::isfinite(profile.lateral_linear_base)) {
    std::ostringstream detail;
    detail << "grounded wheel-normal aggregate is non-finite:";
    for (std::size_t index = 0U; index < contacts.wheels.size(); ++index) {
      const auto &hit = contacts.wheels[index].hit;
      detail << " wheel" << index << '=';
      if (!hit.has_value()) {
        detail << "miss";
        continue;
      }
      detail << '[' << hit->normal[0U] << ',' << hit->normal[1U] << ','
             << hit->normal[2U] << "] material=" << hit->material
             << " surface=" << hit->surface_index;
    }
    throw std::invalid_argument(detail.str());
  }
  return profile;
}

OriginalVehicleGroundedDampingSelection
select_original_vehicle_grounded_damping_profile(
    const WheelContactFrame &contacts,
    const std::array<float, 4U> &grip_response_constants,
    const OriginalBodyVelocityState &velocity,
    const std::size_t current_gear_index, const bool alternate_state_a,
    const bool alternate_state_b, const bool alternate_state_c,
    const OriginalVehicleGroundedMaterialTable &materials) {
  if (std::any_of(velocity.local_linear.begin(), velocity.local_linear.end(),
                  [](const double value) { return !std::isfinite(value); }) ||
      std::any_of(velocity.local_angular.begin(), velocity.local_angular.end(),
                  [](const double value) { return !std::isfinite(value); }) ||
      std::any_of(grip_response_constants.begin(),
                  grip_response_constants.end(), [](const float value) {
                    return !std::isfinite(value) || value <= 0.0F;
                  })) {
    throw std::invalid_argument("grounded damping selection input is invalid");
  }

  auto result = OriginalVehicleGroundedDampingSelection{};
  result.alternate_state_a = alternate_state_a;
  result.alternate_state_b = alternate_state_b;
  result.alternate_state_c = alternate_state_c;
  result.profile = calculate_original_vehicle_grounded_damping_profile(
      contacts, grip_response_constants, materials);
  const auto aggregate = aggregate_grounded_materials(contacts, materials);
  if (aggregate.count == 0U) {
    return result;
  }

  const auto inverse_count = 1.0 / static_cast<double>(aggregate.count);
  result.lateral_threshold = aggregate.lateral_threshold * inverse_count *
                             static_cast<double>(grip_response_constants[0U]);
  result.lateral_excess =
      std::fabs(velocity.local_linear[0U]) - result.lateral_threshold;
  const auto speed_squared =
      velocity.local_linear[0U] * velocity.local_linear[0U] +
      velocity.local_linear[2U] * velocity.local_linear[2U];
  result.yaw_metric = speed_squared * std::fabs(velocity.local_angular[1U]);
  result.yaw_threshold = aggregate.yaw_threshold * inverse_count *
                         static_cast<double>(grip_response_constants[1U]);

  if (result.yaw_metric > result.yaw_threshold) {
    result.retained_traction_accumulator =
        result.yaw_metric - result.yaw_threshold;
  }
  if (result.lateral_excess > 0.0) {
    constexpr double original_lateral_accumulator_scale = 0.001;
    result.retained_traction_accumulator =
        result.lateral_excess * original_lateral_accumulator_scale;
  }
  if (alternate_state_b) {
    constexpr double original_direction_accumulator_scale = 0.3;
    result.retained_traction_accumulator =
        (std::fabs(velocity.local_linear[2U]) +
         std::fabs(velocity.local_linear[0U])) *
        original_direction_accumulator_scale;
  }
  result.alternate_branch =
      current_gear_index > 0U &&
      (alternate_state_a || alternate_state_b || alternate_state_c ||
       result.lateral_excess > 0.0 || result.yaw_metric > result.yaw_threshold);
  if (!result.alternate_branch) {
    return result;
  }

  result.profile.lateral_linear_base =
      aggregate.alternate_lateral * inverse_count *
      static_cast<double>(grip_response_constants[2U]);
  result.profile.longitudinal_linear_base =
      aggregate.alternate_longitudinal * inverse_count;
  result.profile.angular_bases[1U] =
      aggregate.alternate_angular_y * inverse_count *
      static_cast<double>(grip_response_constants[3U]);
  return result;
}

OriginalVehicleSceneSystem::OriginalVehicleSceneSystem(
    OriginalVehicleResponseConfig config)
    : response_(std::move(config)) {}

OriginalVehicleSceneSystem::OriginalVehicleSceneSystem(
    OriginalVehicleResponseConfig config, BodyHullRig body_hull)
    : response_(std::move(config)), body_hull_(std::move(body_hull)) {}

OriginalVehicleSceneFrame
OriginalVehicleSceneSystem::step(OriginalVehicleSceneState &state,
                                 const OriginalVehicleSceneStepInputs &inputs,
                                 const CollisionWorld &world) {
  auto stage = begin_collision_step(state, inputs, world);
  constexpr std::size_t original_static_track_pass_limit = 3U;
  for (std::size_t pass = 0U; pass < original_static_track_pass_limit; ++pass) {
    if (!apply_static_collision_pass(state, stage, world)) {
      break;
    }
  }
  return finish_collision_step(state, stage, world);
}

OriginalVehicleSceneCollisionStage
OriginalVehicleSceneSystem::begin_collision_step(
    OriginalVehicleSceneState &state,
    const OriginalVehicleSceneStepInputs &inputs, const CollisionWorld &world) {
  OriginalVehicleSceneCollisionStage stage;
  stage.inputs = inputs;
  auto &frame = stage.frame;
  // Keep the preceding retained body+0x2e8 normals alive. The dynamic solver
  // reads those records until this outer body reaches its final current-to-
  // retained copy; bodies later in list order therefore still expose their
  // preceding-frame records.
  constexpr double original_car_physics_local_y_shift = 0.32;
  translate_pose_local_y(state.pose, -original_car_physics_local_y_shift);
  const auto previous_pose = state.pose;
  stage.physics_previous_pose = previous_pose;
  frame.physics_previous_pose = previous_pose;
  const auto velocity_snapshot = state.velocity;
  const auto initial_base_forces = calculate_original_wheel_base_forces(
      {state.pose.body_basis, response_.config().body.mass,
       select_original_global_vertical_scale(inputs.global_flags),
       velocity_snapshot.local_linear[2U]});
  frame.vehicle_history_correction =
      apply_original_vehicle_pose_history_correction(state.pose, state.velocity,
                                                     previous_vehicle_velocity_,
                                                     inputs.slice_seconds);
  frame.pre_wheel_angular_seed =
      calculate_original_vehicle_velocity_stabilizer_seed(
          {state.velocity, response_.config().wheel_rig.local_origins.size(),
           previous_wheel_contact_history_sum_,
           inputs.reduced_stabilizer_coefficient});
  const auto load_transfer = apply_original_wheel_longitudinal_load_transfer(
      initial_base_forces,
      {previous_vehicle_velocity_.local_linear[2U],
       state.velocity.local_linear[2U], inputs.slice_seconds,
       previous_wheel_contact_history_sum_ > 3U});
  frame.prepared_base_forces = load_transfer.base_forces;
  frame.longitudinal_load_transfer_candidate = load_transfer.candidate;
  previous_vehicle_velocity_ = velocity_snapshot;

  constexpr std::size_t original_vehicle_substep_count = 3U;
  // RVA 0x000A1E64 divides the stored outer-slice float by the integer
  // substep count on x87, then RVA 0x000A1ECF stores the quotient to the
  // float stack slot consumed by response and pose integration.
  const auto substep_seconds = static_cast<double>(static_cast<float>(
      static_cast<long double>(static_cast<float>(inputs.slice_seconds)) /
      static_cast<long double>(original_vehicle_substep_count)));
  frame.pose.linear_damping = 1.0;
  frame.pose.angular_damping = 1.0;
  frame.pose.world_displacement =
      frame.vehicle_history_correction.world_displacement;
  response_.begin_response_group();
  for (std::size_t substep = 0U; substep < original_vehicle_substep_count;
       ++substep) {
    frame.vehicle_response_substeps[substep] = response_.step(
        state.velocity,
        {state.pose, inputs.dynamic_vector, inputs.surface_scales,
         inputs.global_flags, substep_seconds, frame.prepared_base_forces,
         frame.pre_wheel_angular_seed,
         substep == original_vehicle_substep_count - 1U,
         inputs.reduced_stabilizer_coefficient,
         inputs.retained_traction_accumulator},
        world);
    frame.response = frame.vehicle_response_substeps[substep];
    frame.vehicle_pose_substeps[substep] =
        advance_original_vehicle_body_pose_substep(state.pose, state.velocity,
                                                   substep_seconds);
    frame.vehicle_post_pose_states[substep] = state.pose;
    frame.vehicle_post_pose_velocities[substep] = state.velocity;
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
      frame.pose.world_displacement[axis] +=
          frame.vehicle_pose_substeps[substep].world_displacement[axis];
    }
  }
  frame.pose.world_linear_velocity =
      frame.vehicle_pose_substeps.back().world_linear_velocity;
  std::array<bool, 4U> current_wheel_contact_flags{};
  for (const auto &substep : frame.vehicle_response_substeps) {
    for (std::size_t wheel = 0U; wheel < current_wheel_contact_flags.size();
         ++wheel) {
      current_wheel_contact_flags[wheel] =
          current_wheel_contact_flags[wheel] ||
          substep.contacts.wheels[wheel].hit.has_value();
    }
  }
  const auto current_wheel_contact_count = static_cast<std::size_t>(
      std::count_if(frame.response.contacts.wheels.begin(),
                    frame.response.contacts.wheels.end(),
                    [](const WheelSpringSegmentSample &sample) {
                      return sample.hit.has_value();
                    }));
  if (current_wheel_contact_count != 0U) {
    const auto linear_base_scale =
        calculate_original_vehicle_grounded_linear_base_scale(
            state.velocity.local_linear[2U], inputs.grounded_brake_force,
            inputs.authored_brake_factor, inputs.global_flags);
    frame.grounded_damping_selection =
        select_original_vehicle_grounded_damping_profile(
            frame.response.contacts, inputs.grip_response_constants,
            state.velocity, inputs.current_gear_index,
            inputs.alternate_damping_state_a, inputs.alternate_damping_state_b,
            inputs.alternate_damping_state_c,
            response_.config().grounded_materials);
    auto damping_profile = frame.grounded_damping_selection->profile;
    if (inputs.longitudinal_damping_modifier_index != -1) {
      constexpr double original_longitudinal_modifier = 0.4;
      damping_profile.longitudinal_linear_base *=
          original_longitudinal_modifier;
    }
    frame.grounded_damping = apply_original_vehicle_grounded_damping(
        state.velocity, damping_profile, inputs.slice_seconds,
        linear_base_scale);
  }
  previous_wheel_contact_history_sum_ = static_cast<std::size_t>(
      std::count(current_wheel_contact_flags.begin(),
                 current_wheel_contact_flags.end(), true));
  return stage;
}

bool OriginalVehicleSceneSystem::apply_static_collision_pass(
    OriginalVehicleSceneState &state, OriginalVehicleSceneCollisionStage &stage,
    const CollisionWorld &world) {
  if (stage.finalized) {
    throw std::logic_error("vehicle collision stage is already finalized");
  }
  constexpr std::size_t original_static_track_pass_limit = 3U;
  if (stage.static_passes_attempted >= original_static_track_pass_limit) {
    throw std::logic_error("vehicle collision stage exceeded three passes");
  }
  ++stage.static_passes_attempted;
  if (!body_hull_.has_value()) {
    return false;
  }

  auto pass_frame =
      body_hull_->sample(stage.physics_previous_pose, state.pose, world);
  if (!stage.retained_body_hull.has_value()) {
    stage.retained_body_hull = pass_frame;
  } else {
    for (std::size_t index = 0U; index < pass_frame.samples.size(); ++index) {
      if (pass_frame.samples[index].hit.has_value()) {
        stage.retained_body_hull->samples[index] = pass_frame.samples[index];
      }
    }
    stage.retained_body_hull->retained_contact_count = static_cast<std::size_t>(
        std::count_if(stage.retained_body_hull->samples.begin(),
                      stage.retained_body_hull->samples.end(),
                      [](const BodyHullContactSample &sample) {
                        return sample.hit.has_value();
                      }));
  }

  const auto selected = select_original_body_hull_reaction_sample(pass_frame);
  if (!selected.has_value()) {
    return false;
  }
  const auto &sample = pass_frame.samples[*selected];
  stage.frame.body_hull_reaction_samples.push_back(sample);
  const CollisionVector3 world_sweep{
      sample.segment_end[0U] - sample.segment_start[0U],
      sample.segment_end[1U] - sample.segment_start[1U],
      sample.segment_end[2U] - sample.segment_start[2U]};
  stage.frame.body_hull_reactions.push_back(
      calculate_original_body_hull_reaction(
          {state.pose.body_basis, sample.local_point,
           response_.config().center_of_mass, sample.hit->normal, world_sweep,
           state.velocity, response_.config().body, *sample.hit_fraction,
           true}));
  static_cast<void>(apply_original_body_hull_reaction(
      state.pose, state.velocity, stage.frame.body_hull_reactions.back(),
      stage.inputs.slice_seconds));
  const auto &reaction = stage.frame.body_hull_reactions.back();
  const auto absolute_impulse = std::fabs(reaction.impulse);
  auto &audio_record = stage.frame.hull_audio_record;
  if (absolute_impulse > audio_record.maximum_absolute_impulse) {
    audio_record.maximum_absolute_impulse = absolute_impulse;
    audio_record.scratch_candidate =
        std::fabs(reaction.body_normal[0U]) * state.velocity.local_linear[2U];
    audio_record.material_index =
        sample.hit->material < 26U
            ? static_cast<std::size_t>(sample.hit->material)
            : 0U;
    audio_record.hull_point_index = *selected;
    audio_record.active = true;
  }
  return true;
}

OriginalVehicleSceneFrame OriginalVehicleSceneSystem::finish_collision_step(
    OriginalVehicleSceneState &state, OriginalVehicleSceneCollisionStage &stage,
    const CollisionWorld &world) {
  if (stage.finalized) {
    throw std::logic_error("vehicle collision stage is already finalized");
  }
  stage.finalized = true;
  auto &frame = stage.frame;
  if (body_hull_.has_value()) {
    frame.body_hull = std::move(stage.retained_body_hull);
    // Scheduler 0x000a5a41 clears only the active flags in current records
    // body+0x508. Each 0x000a3e20 static pass then overwrites the record for
    // every point that hit, leaving successful records from earlier passes
    // active. After final stabilization, 0x000a5bc2 copies those accumulated
    // current records to retained body+0x2e8. The fresh 0x000a35a0 crossing
    // queries below are not the records that the next dynamic scan consumes.
    state.dynamic_contact_world_normals.clear();
    if (frame.body_hull.has_value()) {
      for (const auto &sample : frame.body_hull->samples) {
        if (sample.hit.has_value()) {
          state.dynamic_contact_world_normals.push_back(sample.hit->normal);
        }
      }
    }
    const auto post_reaction_frame =
        body_hull_->sample(stage.physics_previous_pose, state.pose, world);
    frame.body_hull_post_stabilization =
        apply_original_body_hull_post_stabilization(
            state.pose, state.velocity, post_reaction_frame,
            response_.config().center_of_mass, stage.inputs.slice_seconds);
  }
  frame.physics_final_pose = state.pose;
  constexpr double original_car_physics_local_y_shift = 0.32;
  translate_pose_local_y(state.pose, original_car_physics_local_y_shift);
  return std::move(frame);
}

OriginalVehicleModeCSceneFrame OriginalVehicleSceneSystem::step_mode_c(
    OriginalVehicleSceneState &state,
    const OriginalVehicleModeCSceneStepInputs &inputs,
    const CollisionWorld &world) {
  OriginalVehicleModeCSceneFrame frame;
  frame.drivetrain =
      calculate_original_drivetrain_mode_c_force(inputs.drivetrain);
  frame.scene =
      step(state,
           {frame.drivetrain.wheel_dynamic_input, inputs.surface_scales,
            inputs.retained_body_contact_count, inputs.global_flags,
            inputs.drivetrain.state.slice_seconds,
            frame.drivetrain.controls.brake_force,
            inputs.drivetrain.authored_brake_force,
            inputs.longitudinal_damping_modifier_index,
            inputs.drivetrain.suppress_negative_body_reversal, 0.0,
            inputs.grip_response_constants,
            inputs.drivetrain.state.current_gear_index,
            frame.drivetrain.state.primary_drive_transition_active,
            frame.drivetrain.state.direction_transition_active,
            inputs.handbrake_active},
           world);
  return frame;
}

const OriginalVehicleResponseConfig &
OriginalVehicleSceneSystem::config() const noexcept {
  return response_.config();
}

void OriginalVehicleSceneSystem::seed_wheel_contact_history(
    const std::array<WheelContactScalarState, 4U> &current) {
  response_.seed_wheel_contact_history(current);
}

void OriginalVehicleSceneSystem::seed_previous_vehicle_velocity(
    const OriginalBodyVelocityState &velocity) {
  const auto finite = [](const auto &values) {
    return std::all_of(values.begin(), values.end(),
                       [](const double value) { return std::isfinite(value); });
  };
  if (!finite(velocity.local_linear) || !finite(velocity.local_angular)) {
    throw std::invalid_argument("previous vehicle velocity must be finite");
  }
  previous_vehicle_velocity_ = velocity;
}

void OriginalVehicleSceneSystem::seed_previous_wheel_contact_count(
    const std::size_t count) {
  if (count > 4U) {
    throw std::invalid_argument("previous wheel-contact count exceeds four");
  }
  previous_wheel_contact_history_sum_ = count;
}

void OriginalVehicleSceneSystem::seed_previous_wheel_contact_flags(
    const std::array<std::uint32_t, 4U> &flags) {
  if (std::any_of(flags.begin(), flags.end(),
                  [](const std::uint32_t value) { return value > 1U; })) {
    throw std::invalid_argument("previous wheel-contact flag is outside 0..1");
  }
  previous_wheel_contact_history_sum_ =
      static_cast<std::size_t>(std::count(flags.begin(), flags.end(), 1U));
}

void OriginalVehicleSceneSystem::reset() noexcept {
  response_.reset();
  previous_vehicle_velocity_ = {};
  previous_wheel_contact_history_sum_ = 0U;
  if (body_hull_.has_value()) {
    body_hull_->reset();
  }
}

} // namespace mh::game
