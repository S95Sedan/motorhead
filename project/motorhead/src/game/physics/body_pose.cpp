#include <game/physics/body_pose.hpp>

#include <array>
#include <cmath>
#include <stdexcept>

namespace mh::game {
namespace {

void require_finite(const CollisionVector3 &value, const char *message) {
  for (const auto component : value) {
    if (!std::isfinite(component)) {
      throw std::invalid_argument(message);
    }
  }
}

void require_finite(const BodyBasis3 &basis, const char *message) {
  for (const auto &row : basis) {
    require_finite(row, message);
  }
}

double stored_float32(const long double value) {
  return static_cast<double>(static_cast<float>(value));
}

long double original_x87_cosine(const float angle) {
  // p3.1 executes FCOS under its 53-bit x87 precision control. Retaining the
  // binary64 result for the helper's first changed component, then reloading
  // the float32 copy for the remaining components, reproduces all 20,718
  // canonical pose-corpus basis stores. A native 64-bit-significand x87 call
  // in the reconstruction process uses a different control word and can move
  // tiny opening rotations by one ULP.
  return static_cast<long double>(std::cos(static_cast<double>(angle)));
}

long double original_x87_sine(const float angle) {
  return static_cast<long double>(std::sin(static_cast<double>(angle)));
}

void rotate_local_x(BodyBasis3 &basis, const double angle) {
  const auto stored_angle = static_cast<float>(angle);
  const auto extended_cosine = original_x87_cosine(stored_angle);
  const auto extended_sine = original_x87_sine(stored_angle);
  const auto stored_cosine =
      static_cast<long double>(stored_float32(extended_cosine));
  const auto stored_sine =
      static_cast<long double>(stored_float32(extended_sine));
  const auto old_y = basis[1];
  const auto old_z = basis[2];
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    const auto cosine = axis == 0U ? extended_cosine : stored_cosine;
    const auto sine = axis == 0U ? extended_sine : stored_sine;
    basis[1][axis] = stored_float32(
        cosine * static_cast<long double>(stored_float32(old_y[axis])) +
        sine * static_cast<long double>(stored_float32(old_z[axis])));
    basis[2][axis] = stored_float32(
        stored_cosine * static_cast<long double>(stored_float32(old_z[axis])) -
        stored_sine * static_cast<long double>(stored_float32(old_y[axis])));
  }
}

void rotate_local_y(BodyBasis3 &basis, const double angle) {
  const auto stored_angle = static_cast<float>(angle);
  const auto extended_cosine = original_x87_cosine(stored_angle);
  const auto extended_sine = original_x87_sine(stored_angle);
  const auto stored_cosine =
      static_cast<long double>(stored_float32(extended_cosine));
  const auto stored_sine =
      static_cast<long double>(stored_float32(extended_sine));
  const auto old_x = basis[0];
  const auto old_z = basis[2];
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    const auto cosine = axis == 0U ? extended_cosine : stored_cosine;
    const auto sine = axis == 0U ? extended_sine : stored_sine;
    basis[0][axis] = stored_float32(
        cosine * static_cast<long double>(stored_float32(old_x[axis])) -
        sine * static_cast<long double>(stored_float32(old_z[axis])));
    basis[2][axis] = stored_float32(
        stored_sine * static_cast<long double>(stored_float32(old_x[axis])) +
        stored_cosine * static_cast<long double>(stored_float32(old_z[axis])));
  }
}

void rotate_local_z(BodyBasis3 &basis, const double angle) {
  const auto stored_angle = static_cast<float>(angle);
  const auto extended_cosine = original_x87_cosine(stored_angle);
  const auto extended_sine = original_x87_sine(stored_angle);
  const auto stored_cosine =
      static_cast<long double>(stored_float32(extended_cosine));
  const auto stored_sine =
      static_cast<long double>(stored_float32(extended_sine));
  const auto old_x = basis[0];
  const auto old_y = basis[1];
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    const auto cosine = axis == 0U ? extended_cosine : stored_cosine;
    const auto sine = axis == 0U ? extended_sine : stored_sine;
    basis[0][axis] = stored_float32(
        cosine * static_cast<long double>(stored_float32(old_x[axis])) +
        sine * static_cast<long double>(stored_float32(old_y[axis])));
    basis[1][axis] = stored_float32(
        stored_cosine * static_cast<long double>(stored_float32(old_y[axis])) -
        stored_sine * static_cast<long double>(stored_float32(old_x[axis])));
  }
}

void translate_local_axis(OriginalBodyPoseState &pose,
                          const std::size_t local_axis, const double distance) {
  const auto stored_distance =
      static_cast<long double>(stored_float32(distance));
  for (std::size_t world_axis = 0U; world_axis < 3U; ++world_axis) {
    pose.world_position[world_axis] = stored_float32(
        static_cast<long double>(
            stored_float32(pose.world_position[world_axis])) +
        stored_distance * static_cast<long double>(stored_float32(
                              pose.body_basis[local_axis][world_axis])));
  }
}

CollisionVector3 subtract_vectors(const CollisionVector3 &left,
                                  const CollisionVector3 &right) {
  return {left[0U] - right[0U], left[1U] - right[1U], left[2U] - right[2U]};
}

CollisionVector3
project_world_vector_to_body_float32(const BodyBasis3 &basis,
                                     const CollisionVector3 &world_vector) {
  CollisionVector3 result{};
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    auto component = static_cast<long double>(stored_float32(basis[axis][0U])) *
                     static_cast<long double>(stored_float32(world_vector[0U]));
    component += static_cast<long double>(stored_float32(basis[axis][1U])) *
                 static_cast<long double>(stored_float32(world_vector[1U]));
    component += static_cast<long double>(stored_float32(basis[axis][2U])) *
                 static_cast<long double>(stored_float32(world_vector[2U]));
    result[axis] = stored_float32(component);
  }
  return result;
}

BodyBasis3 identity_basis() {
  return {{{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}}};
}

} // namespace

CollisionVector3
project_body_vector_to_world(const BodyBasis3 &basis,
                             const CollisionVector3 &body_vector) {
  require_finite(basis, "body basis must be finite");
  require_finite(body_vector, "body vector must be finite");
  return {basis[0][0] * body_vector[0] + basis[1][0] * body_vector[1] +
              basis[2][0] * body_vector[2],
          basis[0][1] * body_vector[0] + basis[1][1] * body_vector[1] +
              basis[2][1] * body_vector[2],
          basis[0][2] * body_vector[0] + basis[1][2] * body_vector[1] +
              basis[2][2] * body_vector[2]};
}

CollisionVector3
project_body_point_to_world(const OriginalBodyPoseState &pose,
                            const CollisionVector3 &body_point) {
  require_finite(pose.world_position, "body position must be finite");
  const auto projected =
      project_body_vector_to_world(pose.body_basis, body_point);
  return {pose.world_position[0U] + projected[0U],
          pose.world_position[1U] + projected[1U],
          pose.world_position[2U] + projected[2U]};
}

OriginalBodyPoseStepResult
advance_original_body_pose(OriginalBodyPoseState &pose,
                           OriginalBodyVelocityState &velocity,
                           const OriginalBodyPoseStepInputs &inputs) {
  require_finite(pose.body_basis, "body pose basis must be finite");
  require_finite(pose.world_position, "body position must be finite");
  require_finite(velocity.local_linear, "body linear velocity must be finite");
  require_finite(velocity.local_angular,
                 "body angular velocity must be finite");
  if (!std::isfinite(inputs.slice_seconds) || inputs.slice_seconds <= 0.0 ||
      inputs.slice_seconds > 0.04) {
    throw std::invalid_argument(
        "body pose slice must be within the recovered (0, 0.04] range");
  }

  OriginalBodyPoseStepResult result;
  const auto has_contacts = inputs.retained_contact_count != 0U;
  result.linear_damping =
      std::pow(has_contacts ? 0.02 : 0.9, inputs.slice_seconds);
  result.angular_damping =
      std::pow(has_contacts ? 0.3 : 0.9, inputs.slice_seconds);
  for (auto &component : velocity.local_linear) {
    component *= result.linear_damping;
  }
  for (auto &component : velocity.local_angular) {
    component *= result.angular_damping;
  }

  const CollisionVector3 body_displacement{
      velocity.local_linear[0] * inputs.slice_seconds,
      velocity.local_linear[1] * inputs.slice_seconds,
      velocity.local_linear[2] * inputs.slice_seconds};
  result.world_displacement =
      project_body_vector_to_world(pose.body_basis, body_displacement);
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    pose.world_position[axis] += result.world_displacement[axis];
  }

  const CollisionVector3 angles{
      velocity.local_angular[0] * inputs.slice_seconds,
      velocity.local_angular[1] * inputs.slice_seconds,
      velocity.local_angular[2] * inputs.slice_seconds};
  rotate_local_x(pose.body_basis, angles[0]);
  rotate_local_y(pose.body_basis, angles[1]);
  rotate_local_z(pose.body_basis, angles[2]);

  auto incremental_basis = identity_basis();
  rotate_local_x(incremental_basis, angles[0]);
  rotate_local_y(incremental_basis, angles[1]);
  rotate_local_z(incremental_basis, angles[2]);
  velocity.local_linear =
      project_world_vector_to_body(incremental_basis, velocity.local_linear);
  result.world_linear_velocity =
      project_body_vector_to_world(pose.body_basis, velocity.local_linear);
  return result;
}

OriginalBodyPoseVelocityDeltaResult advance_original_body_pose_velocity_delta(
    OriginalBodyPoseState &pose, OriginalBodyVelocityState &velocity,
    const OriginalBodyPoseVelocityDeltaInputs &inputs) {
  require_finite(pose.body_basis, "body pose basis must be finite");
  require_finite(pose.world_position, "body position must be finite");
  require_finite(velocity.local_linear, "body linear velocity must be finite");
  require_finite(velocity.local_angular,
                 "body angular velocity must be finite");
  require_finite(inputs.local_linear_velocity_delta,
                 "body linear velocity delta must be finite");
  require_finite(inputs.local_angular_velocity_delta,
                 "body angular velocity delta must be finite");
  if (!std::isfinite(inputs.slice_seconds) || inputs.slice_seconds <= 0.0 ||
      inputs.slice_seconds > 0.04) {
    throw std::invalid_argument(
        "body pose delta slice must be within the recovered (0, 0.04] range");
  }

  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    velocity.local_linear[axis] += inputs.local_linear_velocity_delta[axis];
    velocity.local_angular[axis] += inputs.local_angular_velocity_delta[axis];
  }

  OriginalBodyPoseVelocityDeltaResult result;
  const CollisionVector3 body_displacement{
      inputs.local_linear_velocity_delta[0U] * inputs.slice_seconds,
      inputs.local_linear_velocity_delta[1U] * inputs.slice_seconds,
      inputs.local_linear_velocity_delta[2U] * inputs.slice_seconds};
  result.world_displacement =
      project_body_vector_to_world(pose.body_basis, body_displacement);
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    pose.world_position[axis] += result.world_displacement[axis];
  }

  const CollisionVector3 angles{
      inputs.local_angular_velocity_delta[0U] * inputs.slice_seconds,
      inputs.local_angular_velocity_delta[1U] * inputs.slice_seconds,
      inputs.local_angular_velocity_delta[2U] * inputs.slice_seconds};
  rotate_local_x(pose.body_basis, angles[0U]);
  rotate_local_y(pose.body_basis, angles[1U]);
  rotate_local_z(pose.body_basis, angles[2U]);

  auto incremental_basis = identity_basis();
  rotate_local_x(incremental_basis, angles[0U]);
  rotate_local_y(incremental_basis, angles[1U]);
  rotate_local_z(incremental_basis, angles[2U]);
  velocity.local_linear =
      project_world_vector_to_body(incremental_basis, velocity.local_linear);
  result.world_linear_velocity =
      project_body_vector_to_world(pose.body_basis, velocity.local_linear);
  return result;
}

OriginalBodyPoseVelocityDeltaResult
apply_original_vehicle_pose_history_correction(
    OriginalBodyPoseState &pose, OriginalBodyVelocityState &velocity,
    const OriginalBodyVelocityState &previous_velocity,
    const double slice_seconds) {
  require_finite(pose.body_basis, "vehicle pose basis must be finite");
  require_finite(pose.world_position, "vehicle position must be finite");
  require_finite(velocity.local_linear,
                 "vehicle linear velocity must be finite");
  require_finite(velocity.local_angular,
                 "vehicle angular velocity must be finite");
  require_finite(previous_velocity.local_linear,
                 "previous vehicle linear velocity must be finite");
  require_finite(previous_velocity.local_angular,
                 "previous vehicle angular velocity must be finite");
  if (!std::isfinite(slice_seconds) || slice_seconds <= 0.0 ||
      slice_seconds > 0.04) {
    throw std::invalid_argument(
        "vehicle history slice must be within the recovered (0, 0.04] range");
  }

  CollisionVector3 local_linear_correction{};
  CollisionVector3 local_angular_correction{};
  constexpr double original_history_scale = 0.5;
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    local_linear_correction[axis] =
        (velocity.local_linear[axis] - previous_velocity.local_linear[axis]) *
        original_history_scale;
    local_angular_correction[axis] =
        (velocity.local_angular[axis] - previous_velocity.local_angular[axis]) *
        original_history_scale;
  }

  const CollisionVector3 angles{
      stored_float32(local_angular_correction[0U] * slice_seconds),
      stored_float32(local_angular_correction[1U] * slice_seconds),
      stored_float32(local_angular_correction[2U] * slice_seconds)};
  rotate_local_x(pose.body_basis, angles[0U]);
  rotate_local_y(pose.body_basis, angles[1U]);
  rotate_local_z(pose.body_basis, angles[2U]);

  OriginalBodyPoseVelocityDeltaResult result;
  const auto old_position = pose.world_position;
  translate_local_axis(
      pose, 0U, stored_float32(local_linear_correction[0U] * slice_seconds));
  translate_local_axis(
      pose, 1U, stored_float32(local_linear_correction[1U] * slice_seconds));
  translate_local_axis(
      pose, 2U, stored_float32(local_linear_correction[2U] * slice_seconds));
  result.world_displacement =
      subtract_vectors(pose.world_position, old_position);

  auto incremental_basis = identity_basis();
  rotate_local_x(incremental_basis, angles[0U]);
  rotate_local_y(incremental_basis, angles[1U]);
  rotate_local_z(incremental_basis, angles[2U]);
  velocity.local_linear = project_world_vector_to_body_float32(
      incremental_basis, velocity.local_linear);
  result.world_linear_velocity =
      project_body_vector_to_world(pose.body_basis, velocity.local_linear);
  return result;
}

OriginalBodyPoseVelocityDeltaResult
advance_original_vehicle_body_pose_substep(OriginalBodyPoseState &pose,
                                           OriginalBodyVelocityState &velocity,
                                           const double substep_seconds) {
  require_finite(pose.body_basis, "vehicle pose basis must be finite");
  require_finite(pose.world_position, "vehicle position must be finite");
  require_finite(velocity.local_linear,
                 "vehicle linear velocity must be finite");
  require_finite(velocity.local_angular,
                 "vehicle angular velocity must be finite");
  if (!std::isfinite(substep_seconds) || substep_seconds <= 0.0 ||
      substep_seconds > 0.04) {
    throw std::invalid_argument(
        "vehicle pose substep must be within the recovered (0, 0.04] range");
  }

  const CollisionVector3 angles{
      stored_float32(velocity.local_angular[0U] * substep_seconds),
      stored_float32(velocity.local_angular[1U] * substep_seconds),
      stored_float32(velocity.local_angular[2U] * substep_seconds)};
  rotate_local_x(pose.body_basis, angles[0U]);
  rotate_local_y(pose.body_basis, angles[1U]);
  rotate_local_z(pose.body_basis, angles[2U]);

  OriginalBodyPoseVelocityDeltaResult result;
  const auto old_position = pose.world_position;
  translate_local_axis(
      pose, 0U, stored_float32(velocity.local_linear[0U] * substep_seconds));
  translate_local_axis(
      pose, 1U, stored_float32(velocity.local_linear[1U] * substep_seconds));
  translate_local_axis(
      pose, 2U, stored_float32(velocity.local_linear[2U] * substep_seconds));
  result.world_displacement =
      subtract_vectors(pose.world_position, old_position);

  auto incremental_basis = identity_basis();
  rotate_local_x(incremental_basis, angles[0U]);
  rotate_local_y(incremental_basis, angles[1U]);
  rotate_local_z(incremental_basis, angles[2U]);
  velocity.local_linear = project_world_vector_to_body_float32(
      incremental_basis, velocity.local_linear);
  result.world_linear_velocity =
      project_body_vector_to_world(pose.body_basis, velocity.local_linear);
  return result;
}

} // namespace mh::game
