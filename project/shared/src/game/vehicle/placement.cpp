#include <game/vehicle/placement.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace mh::game {
namespace {

double dot(const CollisionVector3 &left, const CollisionVector3 &right) {
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

CollisionVector3 add_scaled(const CollisionVector3 &point,
                            const CollisionVector3 &direction,
                            const double scale) {
  return {point[0] + direction[0] * scale, point[1] + direction[1] * scale,
          point[2] + direction[2] * scale};
}

CollisionVector3 subtract(const CollisionVector3 &left,
                          const CollisionVector3 &right) {
  return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

void require_finite_pose(const OriginalBodyPoseState &pose) {
  for (const auto component : pose.world_position) {
    if (!std::isfinite(component)) {
      throw std::invalid_argument("vehicle placement pose must be finite");
    }
  }
  for (const auto &row : pose.body_basis) {
    for (const auto component : row) {
      if (!std::isfinite(component)) {
        throw std::invalid_argument("vehicle placement basis must be finite");
      }
    }
  }
}

} // namespace

std::optional<VehicleContactPlacement> place_vehicle_for_full_wheel_contact(
    const WheelProbeRig &rig, const OriginalBodyPoseState &base_pose,
    const CollisionWorld &world, const double maximum_local_up_adjustment) {
  require_finite_pose(base_pose);
  if (!std::isfinite(maximum_local_up_adjustment) ||
      maximum_local_up_adjustment <= 0.0) {
    throw std::invalid_argument(
        "vehicle placement adjustment bound must be positive and finite");
  }
  if (!std::isfinite(rig.authored_spring_length) ||
      rig.authored_spring_length <= 0.0) {
    throw std::invalid_argument(
        "vehicle placement spring length must be positive and finite");
  }

  const auto local_up =
      project_body_vector_to_world(base_pose.body_basis, {0.0, 1.0, 0.0});
  const auto up_length_squared = dot(local_up, local_up);
  if (!std::isfinite(up_length_squared) || up_length_squared <= 0.0) {
    throw std::invalid_argument("vehicle placement up axis must be nonzero");
  }
  const auto inverse_up_length = 1.0 / std::sqrt(up_length_squared);
  const CollisionVector3 world_up{local_up[0] * inverse_up_length,
                                  local_up[1] * inverse_up_length,
                                  local_up[2] * inverse_up_length};
  const CollisionVector3 world_down{-world_up[0], -world_up[1], -world_up[2]};
  const auto probe_half_span =
      maximum_local_up_adjustment + rig.authored_spring_length;

  VehicleContactPlacement result;
  auto feasible_minimum = -maximum_local_up_adjustment;
  auto feasible_maximum = maximum_local_up_adjustment;
  for (std::size_t index = 0U; index < rig.local_origins.size(); ++index) {
    const auto center =
        project_body_point_to_world(base_pose, rig.local_origins[index]);
    const auto origin = add_scaled(center, world_up, probe_half_span);
    const auto support =
        world.raycast(origin, world_down, probe_half_span * 2.0);
    if (!support.has_value()) {
      return std::nullopt;
    }
    result.support_hits[index] = *support;
    const auto signed_support_offset =
        dot(subtract(support->point, center), world_up);
    feasible_minimum = std::max(
        feasible_minimum, signed_support_offset - rig.authored_spring_length);
    feasible_maximum = std::min(
        feasible_maximum, signed_support_offset + rig.authored_spring_length);
  }
  if (feasible_minimum > feasible_maximum) {
    return std::nullopt;
  }

  result.feasible_adjustment_minimum = feasible_minimum;
  result.feasible_adjustment_maximum = feasible_maximum;
  result.local_up_adjustment = (feasible_minimum + feasible_maximum) * 0.5;
  result.pose = base_pose;
  result.pose.world_position = add_scaled(result.pose.world_position, world_up,
                                          result.local_up_adjustment);
  result.spring_segments =
      sample_wheel_spring_segments(rig, result.pose, world);
  if (!std::all_of(result.spring_segments.begin(), result.spring_segments.end(),
                   [](const WheelSpringSegmentSample &sample) {
                     return sample.hit.has_value() &&
                            sample.hit_fraction.has_value();
                   })) {
    return std::nullopt;
  }
  return result;
}

} // namespace mh::game
