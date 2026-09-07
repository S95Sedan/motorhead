#include <game/physics/body_hull_contact.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace mh::game {
namespace {

double segment_length_squared(const CollisionVector3 &start,
                              const CollisionVector3 &end) {
  double squared = 0.0;
  for (std::size_t axis = 0U; axis < start.size(); ++axis) {
    const auto delta = end[axis] - start[axis];
    squared += delta * delta;
  }
  return squared;
}

void require_finite(const CollisionVector3 &value, const char *message) {
  if (!std::all_of(value.begin(), value.end(), [](const double component) {
        return std::isfinite(component);
      })) {
    throw std::invalid_argument(message);
  }
}

void require_finite(const BodyBasis3 &basis, const char *message) {
  for (const auto &axis : basis) {
    require_finite(axis, message);
  }
}

CollisionVector3 subtract(const CollisionVector3 &left,
                          const CollisionVector3 &right) {
  return {left[0U] - right[0U], left[1U] - right[1U], left[2U] - right[2U]};
}

CollisionVector3 add(const CollisionVector3 &left,
                     const CollisionVector3 &right) {
  return {left[0U] + right[0U], left[1U] + right[1U], left[2U] + right[2U]};
}

CollisionVector3 scaled(const CollisionVector3 &value, const double scale) {
  return {value[0U] * scale, value[1U] * scale, value[2U] * scale};
}

double dot(const CollisionVector3 &left, const CollisionVector3 &right) {
  return left[0U] * right[0U] + left[1U] * right[1U] + left[2U] * right[2U];
}

CollisionVector3 cross(const CollisionVector3 &left,
                       const CollisionVector3 &right) {
  return {left[1U] * right[2U] - left[2U] * right[1U],
          left[2U] * right[0U] - left[0U] * right[2U],
          left[0U] * right[1U] - left[1U] * right[0U]};
}

double stored_float32(const long double value) {
  return static_cast<double>(static_cast<float>(value));
}

CollisionVector3
project_original_hull_point_to_world(const OriginalBodyPoseState &pose,
                                     const CollisionVector3 &local_point) {
  CollisionVector3 result{};
  for (std::size_t world_axis = 0U; world_axis < result.size(); ++world_axis) {
    // RVA 0x000a3e74 evaluates local Y, X, Z, then translation through x87
    // before storing the query endpoint to binary32.
    auto component =
        static_cast<long double>(stored_float32(local_point[1U])) *
        static_cast<long double>(
            stored_float32(pose.body_basis[1U][world_axis]));
    component +=
        static_cast<long double>(stored_float32(local_point[0U])) *
        static_cast<long double>(
            stored_float32(pose.body_basis[0U][world_axis]));
    component +=
        static_cast<long double>(stored_float32(local_point[2U])) *
        static_cast<long double>(
            stored_float32(pose.body_basis[2U][world_axis]));
    component +=
        static_cast<long double>(stored_float32(pose.world_position[world_axis]));
    result[world_axis] = stored_float32(component);
  }
  return result;
}

} // namespace

std::optional<std::size_t>
select_original_body_hull_reaction_sample(const BodyHullContactFrame &frame) {
  std::optional<std::size_t> selected;
  auto selected_fraction = 1.1;
  for (std::size_t index = 0U; index < frame.samples.size(); ++index) {
    const auto &sample = frame.samples[index];
    if (!sample.hit.has_value() || !sample.hit_fraction.has_value()) {
      continue;
    }
    if (!std::isfinite(*sample.hit_fraction) || *sample.hit_fraction < 0.0 ||
        *sample.hit_fraction > 1.0) {
      throw std::invalid_argument(
          "body hull hit fraction must be within [0, 1]");
    }
    if (*sample.hit_fraction < selected_fraction) {
      selected = index;
      selected_fraction = *sample.hit_fraction;
    }
  }
  return selected;
}

OriginalBodyHullReactionResult calculate_original_body_hull_reaction(
    const OriginalBodyHullReactionInputs &inputs) {
  require_finite(inputs.body_basis, "body hull reaction basis must be finite");
  require_finite(inputs.local_point, "body hull reaction point must be finite");
  require_finite(inputs.local_center_of_mass,
                 "body hull reaction center of mass must be finite");
  require_finite(inputs.world_normal,
                 "body hull reaction normal must be finite");
  require_finite(inputs.world_sweep, "body hull reaction sweep must be finite");
  require_finite(inputs.velocity.local_linear,
                 "body hull reaction linear velocity must be finite");
  require_finite(inputs.velocity.local_angular,
                 "body hull reaction angular velocity must be finite");
  require_finite(inputs.mass_properties.principal_inertia,
                 "body hull reaction inertia must be finite");
  if (!std::isfinite(inputs.mass_properties.mass) ||
      inputs.mass_properties.mass <= 0.0 ||
      !std::all_of(inputs.mass_properties.principal_inertia.begin(),
                   inputs.mass_properties.principal_inertia.end(),
                   [](const double value) { return value > 0.0; }) ||
      !std::isfinite(inputs.hit_fraction) || inputs.hit_fraction < 0.0 ||
      inputs.hit_fraction > 1.0) {
    throw std::invalid_argument(
        "body hull reaction mass, inertia, or fraction is invalid");
  }

  OriginalBodyHullReactionResult result;
  result.body_normal =
      project_world_vector_to_body(inputs.body_basis, inputs.world_normal);
  result.local_lever =
      subtract(inputs.local_point, inputs.local_center_of_mass);
  result.local_contact_velocity =
      add(inputs.velocity.local_linear,
          cross(inputs.velocity.local_angular, result.local_lever));
  result.normal_velocity =
      dot(result.local_contact_velocity, result.body_normal);

  const auto angular_impulse_axis =
      cross(result.local_lever, result.body_normal);
  CollisionVector3 angular_response{};
  constexpr CollisionVector3 original_vehicle_inertia_addition{2000.0, 3000.0,
                                                               2000.0};
  for (std::size_t axis = 0U; axis < angular_response.size(); ++axis) {
    const auto inertia =
        inputs.mass_properties.principal_inertia[axis] +
        (inputs.vehicle_stabilization ? original_vehicle_inertia_addition[axis]
                                      : 0.0);
    angular_response[axis] = angular_impulse_axis[axis] / inertia;
  }
  const auto contact_angular_response =
      cross(angular_response, result.local_lever);
  result.effective_inverse_mass =
      1.0 / inputs.mass_properties.mass +
      dot(contact_angular_response, result.body_normal);
  if (!std::isfinite(result.effective_inverse_mass) ||
      result.effective_inverse_mass <= 0.0) {
    throw std::invalid_argument(
        "body hull reaction effective mass must be positive");
  }

  result.impulse = -result.normal_velocity / result.effective_inverse_mass;
  result.local_linear_velocity_delta =
      scaled(result.body_normal, result.impulse / inputs.mass_properties.mass);
  result.local_angular_velocity_delta =
      scaled(angular_response, result.impulse);
  if (inputs.vehicle_stabilization) {
    const auto vertical_normal_squared =
        inputs.world_normal[1U] * inputs.world_normal[1U];
    result.local_linear_velocity_delta[1U] *= vertical_normal_squared;
    result.local_angular_velocity_delta[0U] *= vertical_normal_squared;
    result.local_angular_velocity_delta[2U] *= vertical_normal_squared;
    if (result.local_linear_velocity_delta[1U] > 0.0) {
      result.local_linear_velocity_delta[1U] =
          std::sqrt(result.local_linear_velocity_delta[1U]);
    }
  }

  constexpr double original_position_bias = 1.03;
  const auto correction_scale = dot(inputs.world_sweep, inputs.world_normal) *
                                (inputs.hit_fraction - 1.0) *
                                original_position_bias;
  result.world_position_correction =
      scaled(inputs.world_normal, correction_scale);
  return result;
}

OriginalBodyHullReactionApplicationResult apply_original_body_hull_reaction(
    OriginalBodyPoseState &pose, OriginalBodyVelocityState &velocity,
    const OriginalBodyHullReactionResult &reaction,
    const double slice_seconds) {
  require_finite(reaction.world_position_correction,
                 "body hull position correction must be finite");
  require_finite(reaction.local_linear_velocity_delta,
                 "body hull linear velocity delta must be finite");
  require_finite(reaction.local_angular_velocity_delta,
                 "body hull angular velocity delta must be finite");

  OriginalBodyHullReactionApplicationResult result;
  result.world_position_correction = reaction.world_position_correction;
  for (std::size_t axis = 0U; axis < pose.world_position.size(); ++axis) {
    pose.world_position[axis] += reaction.world_position_correction[axis];
  }
  result.pose_delta = advance_original_body_pose_velocity_delta(
      pose, velocity,
      {reaction.local_linear_velocity_delta,
       reaction.local_angular_velocity_delta, slice_seconds});
  return result;
}

OriginalBodyHullPostStabilizationResult
apply_original_generic_body_hull_post_stabilization(
    OriginalBodyPoseState &pose, OriginalBodyVelocityState &velocity,
    const BodyHullContactFrame &frame,
    const CollisionVector3 &local_center_of_mass,
    const double slice_seconds) {
  require_finite(pose.body_basis,
                 "generic body hull post-stabilization basis must be finite");
  require_finite(pose.world_position,
                 "generic body hull post-stabilization position must be finite");
  require_finite(
      velocity.local_linear,
      "generic body hull post-stabilization velocity must be finite");
  require_finite(
      local_center_of_mass,
      "generic body hull post-stabilization center of mass must be finite");
  if (!std::isfinite(slice_seconds) || slice_seconds < 0.0) {
    throw std::invalid_argument(
        "generic body hull post-stabilization slice must be finite and "
        "non-negative");
  }

  OriginalBodyHullPostStabilizationResult result;
  for (const auto &sample : frame.samples) {
    if (!sample.hit.has_value()) {
      continue;
    }
    if (!sample.hit_fraction.has_value() ||
        !std::isfinite(*sample.hit_fraction) || *sample.hit_fraction < 0.0 ||
        *sample.hit_fraction > 1.0) {
      throw std::invalid_argument(
          "generic body hull post-stabilization fraction must be within [0, "
          "1]");
    }
    require_finite(sample.local_point,
                   "generic body hull point must be finite");
    require_finite(sample.hit->normal,
                   "generic body hull normal must be finite");

    ++result.crossing_count;
    const auto body_normal =
        project_world_vector_to_body(pose.body_basis, sample.hit->normal);
    velocity.local_linear =
        subtract(velocity.local_linear,
                 scaled(body_normal, dot(velocity.local_linear, body_normal)));

    const auto local_correction =
        scaled(body_normal, 1.0 - *sample.hit_fraction);
    result.accumulated_local_linear_correction =
        add(result.accumulated_local_linear_correction, local_correction);
    result.discarded_local_angular_correction =
        add(result.discarded_local_angular_correction,
            cross(subtract(sample.local_point, local_center_of_mass),
                  local_correction));
  }

  result.applied_local_linear_delta =
      scaled(result.accumulated_local_linear_correction, slice_seconds);
  velocity.local_linear =
      add(velocity.local_linear, result.applied_local_linear_delta);
  result.world_displacement = project_body_vector_to_world(
      pose.body_basis, result.applied_local_linear_delta);
  pose.world_position = add(pose.world_position, result.world_displacement);
  return result;
}

OriginalBodyHullPostStabilizationResult
apply_original_body_hull_post_stabilization(
    OriginalBodyPoseState &pose, OriginalBodyVelocityState &velocity,
    const BodyHullContactFrame &frame,
    const CollisionVector3 &local_center_of_mass, const double slice_seconds) {
  require_finite(pose.body_basis,
                 "body hull post-stabilization basis must be finite");
  require_finite(pose.world_position,
                 "body hull post-stabilization position must be finite");
  require_finite(velocity.local_linear,
                 "body hull post-stabilization velocity must be finite");
  require_finite(local_center_of_mass,
                 "body hull post-stabilization center of mass must be finite");
  if (!std::isfinite(slice_seconds) || slice_seconds < 0.0) {
    throw std::invalid_argument(
        "body hull post-stabilization slice must be finite and non-negative");
  }

  OriginalBodyHullPostStabilizationResult result;
  constexpr double original_crossing_correction = 50.0;
  constexpr double original_local_y_scale = 0.2;
  for (const auto &sample : frame.samples) {
    if (!sample.hit.has_value()) {
      continue;
    }
    if (!sample.hit_fraction.has_value() ||
        !std::isfinite(*sample.hit_fraction) || *sample.hit_fraction < 0.0 ||
        *sample.hit_fraction > 1.0) {
      throw std::invalid_argument(
          "body hull post-stabilization fraction must be within [0, 1]");
    }
    require_finite(sample.local_point,
                   "body hull post-stabilization point must be finite");
    require_finite(sample.hit->normal,
                   "body hull post-stabilization normal must be finite");

    ++result.crossing_count;
    const auto body_normal =
        project_world_vector_to_body(pose.body_basis, sample.hit->normal);
    velocity.local_linear =
        subtract(velocity.local_linear,
                 scaled(body_normal, dot(velocity.local_linear, body_normal)));

    const auto vertical_normal_squared =
        sample.hit->normal[1U] * sample.hit->normal[1U];
    if (velocity.local_linear[1U] > 0.0) {
      velocity.local_linear[1U] *= vertical_normal_squared;
    }

    auto local_correction =
        scaled(body_normal,
               (1.0 - *sample.hit_fraction) * original_crossing_correction);
    local_correction[1U] *= vertical_normal_squared * original_local_y_scale;
    result.accumulated_local_linear_correction =
        add(result.accumulated_local_linear_correction, local_correction);
    result.discarded_local_angular_correction =
        add(result.discarded_local_angular_correction,
            cross(subtract(sample.local_point, local_center_of_mass),
                  local_correction));
  }

  result.applied_local_linear_delta =
      scaled(result.accumulated_local_linear_correction, slice_seconds);
  velocity.local_linear =
      add(velocity.local_linear, result.applied_local_linear_delta);
  result.world_displacement = project_body_vector_to_world(
      pose.body_basis, result.applied_local_linear_delta);
  pose.world_position = add(pose.world_position, result.world_displacement);
  return result;
}

BodyHullContactSystem::BodyHullContactSystem(BodyHullRig rig)
    : rig_(std::move(rig)) {
  if (rig_.local_points.empty()) {
    throw std::invalid_argument("body hull requires at least one local point");
  }
  for (const auto &point : rig_.local_points) {
    for (const auto component : point) {
      if (!std::isfinite(component)) {
        throw std::invalid_argument("body hull local points must be finite");
      }
    }
  }
}

BodyHullContactFrame
BodyHullContactSystem::sample(const OriginalBodyPoseState &previous_pose,
                              const OriginalBodyPoseState &current_pose,
                              const CollisionWorld &world) {
  BodyHullContactFrame frame;
  frame.samples.reserve(rig_.local_points.size());
  for (const auto &local_point : rig_.local_points) {
    BodyHullContactSample sample;
    sample.local_point = local_point;
    sample.segment_start =
        project_original_hull_point_to_world(previous_pose, local_point);
    sample.segment_end =
        project_original_hull_point_to_world(current_pose, local_point);
    const auto length_squared =
        segment_length_squared(sample.segment_start, sample.segment_end);
    if (length_squared > collision_segment_length_squared_epsilon) {
      const auto length = std::sqrt(length_squared);
      // Retail submits the current/penetrated endpoint first and the retained
      // pre-slice endpoint second to the shared segment query.
      sample.hit =
          world.original_segment_cast(sample.segment_end, sample.segment_start);
      if (sample.hit.has_value()) {
        // The query distance is measured from endpoint A; the hull record
        // retains the complementary pre-slice-to-current sweep fraction in a
        // binary32 field at record +0x38.
        sample.hit_fraction =
            stored_float32(1.0 - sample.hit->distance / length);
        ++frame.retained_contact_count;
      }
    }
    frame.samples.push_back(sample);
  }
  retained_contact_count_ = frame.retained_contact_count;
  return frame;
}

std::size_t BodyHullContactSystem::retained_contact_count() const noexcept {
  return retained_contact_count_;
}

const BodyHullRig &BodyHullContactSystem::rig() const noexcept { return rig_; }

void BodyHullContactSystem::reset() noexcept { retained_contact_count_ = 0U; }

} // namespace mh::game
