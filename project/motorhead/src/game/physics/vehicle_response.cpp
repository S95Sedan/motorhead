#include <game/physics/vehicle_response.hpp>

#include <cmath>
#include <stdexcept>
#include <utility>

namespace mh::game {
namespace {

void require_finite(const CollisionVector3 &value, const char *message) {
  for (const auto component : value) {
    if (!std::isfinite(component)) {
      throw std::invalid_argument(message);
    }
  }
}

void validate_config(const OriginalVehicleResponseConfig &config) {
  require_finite(config.center_of_mass,
                 "vehicle response center of mass must be finite");
  require_finite(config.body.principal_inertia,
                 "vehicle response inertia must be finite");
  if (!std::isfinite(config.body.mass) || config.body.mass <= 0.0) {
    throw std::invalid_argument("vehicle response mass must be positive");
  }
  for (const auto inertia : config.body.principal_inertia) {
    if (inertia <= 0.0) {
      throw std::invalid_argument("vehicle response inertia must be positive");
    }
  }
}

} // namespace

OriginalVehicleResponseSystem::OriginalVehicleResponseSystem(
    OriginalVehicleResponseConfig config)
    : config_(std::move(config)),
      contacts_(config_.wheel_rig, config_.response_profile) {
  validate_config(config_);
}

OriginalVehicleResponseFrame OriginalVehicleResponseSystem::step(
    OriginalBodyVelocityState &velocity,
    const OriginalVehicleResponseStepInputs &inputs,
    const CollisionWorld &world) {
  return step(velocity,
              {make_original_body_pose(inputs.transform), inputs.dynamic_vector,
               inputs.surface_scales, inputs.global_flags,
               inputs.slice_seconds, std::nullopt,
               inputs.initial_angular_force},
              world);
}

OriginalVehicleResponseFrame OriginalVehicleResponseSystem::step(
    OriginalBodyVelocityState &velocity,
    const OriginalVehicleResponsePoseStepInputs &inputs,
    const CollisionWorld &world) {
  if (!std::isfinite(inputs.slice_seconds) || inputs.slice_seconds <= 0.0 ||
      inputs.slice_seconds > 0.04) {
    throw std::invalid_argument(
        "vehicle response slice must be within the recovered (0, 0.04] range");
  }

  OriginalVehicleResponseFrame frame;
  require_finite(inputs.initial_angular_force,
                 "initial angular force must be finite");
  frame.accumulated_force.angular = inputs.initial_angular_force;
  frame.contacts = contacts_.sample(inputs.pose, world,
                                    inputs.reuse_retained_wheel_surfaces);
  const auto &basis = inputs.pose.body_basis;
  const auto dynamic_vectors =
      calculate_original_wheel_dynamic_vectors(inputs.dynamic_vector);
  if (!std::isfinite(velocity.local_linear[2])) {
    throw std::invalid_argument("wheel base-force input must be finite");
  }
  const auto base_forces = inputs.prepared_base_forces.has_value()
                               ? *inputs.prepared_base_forces
                               : calculate_original_wheel_base_forces(
                                     {basis, config_.body.mass,
                                      select_original_global_vertical_scale(
                                          inputs.global_flags),
                                      velocity.local_linear[2]});

  for (std::size_t index = 0U; index < frame.wheel_forces.size(); ++index) {
    auto &wheel_force = frame.wheel_forces[index];
    wheel_force.force = base_forces[index];
    const auto &wheel = frame.contacts.wheels[index];
    if (wheel.hit.has_value()) {
      auto surface_scale = inputs.surface_scales[index];
      if (!surface_scale.has_retained_surface) {
        const auto material_index =
            wheel.hit->material < config_.grounded_materials.size()
                ? static_cast<std::size_t>(wheel.hit->material)
                : 0U;
        surface_scale.has_retained_surface = true;
        if (inputs.use_reduced_surface_scale) {
          // RVA 0x000a1a79 selects material row +0x40 (SpinTurnReduce)
          // multiplied by clamp(10 / car+0xcc, 0, 1).
          surface_scale.active_factor =
              config_.grounded_materials[material_index].spin_turn_reduce;
          surface_scale.active_divisor =
              inputs.retained_traction_accumulator;
          surface_scale.use_active_factor = true;
        } else {
          // The ordinary p3.1 owner+0x90 == 0 branch at
          // 0x000a1ae0 selects material row +0x44 (NoSpinTurnReduce).
          surface_scale.static_factor =
              config_.grounded_materials[material_index].no_spin_turn_reduce;
        }
      }
      wheel_force = calculate_original_wheel_force(
          {basis, wheel.hit->normal, dynamic_vectors[index], base_forces[index],
           calculate_original_wheel_dynamic_x_scale(surface_scale),
           -contacts_.response().spring_length,
           frame.contacts.response_scalars[index].spring,
           frame.contacts.response_scalars[index].state_delta});
    }
    accumulate_body_force_at_point(frame.accumulated_force, wheel_force.force,
                                   config_.wheel_rig.local_origins[index],
                                   config_.center_of_mass);
  }

  apply_original_body_force_response(
      velocity, {frame.accumulated_force, config_.body.mass,
                 config_.body.principal_inertia, inputs.slice_seconds});
  frame.velocity = velocity;
  return frame;
}

const OriginalVehicleResponseConfig &
OriginalVehicleResponseSystem::config() const noexcept {
  return config_;
}

void OriginalVehicleResponseSystem::seed_wheel_contact_history(
    const std::array<WheelContactScalarState, 4U> &current) {
  contacts_.seed_previous_state(current);
}

void OriginalVehicleResponseSystem::begin_response_group() noexcept {
  contacts_.begin_response_group();
}

void OriginalVehicleResponseSystem::reset() noexcept { contacts_.reset(); }

} // namespace mh::game
