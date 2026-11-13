#include <game/physics/dynamic_vehicle_contact.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace mh::game {
namespace {

void require_finite(const CollisionVector3 &value, const char *label) {
  if (!std::all_of(value.begin(), value.end(), [](const double component) {
        return std::isfinite(component);
      })) {
    throw std::invalid_argument(std::string(label) + " must be finite");
  }
}

void require_shape(const OriginalDynamicVehicleContactShape &shape,
                   const char *label) {
  if (shape.local_points.size() < 3U || shape.local_planes.size() < 4U ||
      !std::isfinite(shape.bounding_radius) || shape.bounding_radius <= 0.0 ||
      !std::isfinite(shape.mass_properties.mass) ||
      shape.mass_properties.mass <= 0.0 ||
      !std::all_of(shape.mass_properties.principal_inertia.begin(),
                   shape.mass_properties.principal_inertia.end(),
                   [](const double value) {
                     return std::isfinite(value) && value > 0.0;
                   })) {
    throw std::invalid_argument(std::string(label) +
                                " has invalid hull or mass properties");
  }
  require_finite(shape.local_center_of_mass, label);
  for (const auto &point : shape.local_points) {
    require_finite(point, label);
  }
  for (const auto &plane : shape.local_planes) {
    require_finite(plane.local_normal, label);
    const auto normal_squared =
        plane.local_normal[0U] * plane.local_normal[0U] +
        plane.local_normal[1U] * plane.local_normal[1U] +
        plane.local_normal[2U] * plane.local_normal[2U];
    if (!std::isfinite(plane.distance) || normal_squared <= 1.0e-12) {
      throw std::invalid_argument(std::string(label) +
                                  " has an invalid convex plane");
    }
  }
}

double stored_float32(const long double value) {
  return static_cast<double>(static_cast<float>(value));
}

double dot(const CollisionVector3 &left, const CollisionVector3 &right) {
  auto result = static_cast<long double>(stored_float32(left[0U])) *
                static_cast<long double>(stored_float32(right[0U]));
  result += static_cast<long double>(stored_float32(left[1U])) *
            static_cast<long double>(stored_float32(right[1U]));
  result += static_cast<long double>(stored_float32(left[2U])) *
            static_cast<long double>(stored_float32(right[2U]));
  return static_cast<double>(result);
}

CollisionVector3 add(const CollisionVector3 &left,
                     const CollisionVector3 &right) {
  return {
      stored_float32(static_cast<long double>(stored_float32(left[0U])) +
                     static_cast<long double>(stored_float32(right[0U]))),
      stored_float32(static_cast<long double>(stored_float32(left[1U])) +
                     static_cast<long double>(stored_float32(right[1U]))),
      stored_float32(static_cast<long double>(stored_float32(left[2U])) +
                     static_cast<long double>(stored_float32(right[2U]))),
  };
}

CollisionVector3 subtract(const CollisionVector3 &left,
                          const CollisionVector3 &right) {
  return {
      stored_float32(static_cast<long double>(stored_float32(left[0U])) -
                     static_cast<long double>(stored_float32(right[0U]))),
      stored_float32(static_cast<long double>(stored_float32(left[1U])) -
                     static_cast<long double>(stored_float32(right[1U]))),
      stored_float32(static_cast<long double>(stored_float32(left[2U])) -
                     static_cast<long double>(stored_float32(right[2U]))),
  };
}

CollisionVector3 scaled(const CollisionVector3 &value, const double scale) {
  const auto stored_scale = static_cast<long double>(stored_float32(scale));
  return {
      stored_float32(static_cast<long double>(stored_float32(value[0U])) *
                     stored_scale),
      stored_float32(static_cast<long double>(stored_float32(value[1U])) *
                     stored_scale),
      stored_float32(static_cast<long double>(stored_float32(value[2U])) *
                     stored_scale),
  };
}

CollisionVector3 normalized_stored(const CollisionVector3 &value) {
  const auto x = static_cast<long double>(stored_float32(value[0U]));
  const auto y = static_cast<long double>(stored_float32(value[1U]));
  const auto z = static_cast<long double>(stored_float32(value[2U]));
  const auto length = std::sqrt(x * x + y * y + z * z);
  if (length == 0.0L) {
    throw std::invalid_argument("dynamic contact normalization input is zero");
  }
  const auto inverse_length = 1.0L / length;
  return {stored_float32(x * inverse_length),
          stored_float32(y * inverse_length),
          stored_float32(z * inverse_length)};
}

CollisionVector3 cross(const CollisionVector3 &left,
                       const CollisionVector3 &right) {
  const auto lx = static_cast<long double>(stored_float32(left[0U]));
  const auto ly = static_cast<long double>(stored_float32(left[1U]));
  const auto lz = static_cast<long double>(stored_float32(left[2U]));
  const auto rx = static_cast<long double>(stored_float32(right[0U]));
  const auto ry = static_cast<long double>(stored_float32(right[1U]));
  const auto rz = static_cast<long double>(stored_float32(right[2U]));
  return {stored_float32(ly * rz - lz * ry), stored_float32(lz * rx - lx * rz),
          stored_float32(lx * ry - ly * rx)};
}

CollisionVector3 body_to_world(const BodyBasis3 &basis,
                               const CollisionVector3 &value) {
  CollisionVector3 result{};
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    auto component = static_cast<long double>(stored_float32(basis[0U][axis])) *
                     static_cast<long double>(stored_float32(value[0U]));
    component += static_cast<long double>(stored_float32(basis[1U][axis])) *
                 static_cast<long double>(stored_float32(value[1U]));
    component += static_cast<long double>(stored_float32(basis[2U][axis])) *
                 static_cast<long double>(stored_float32(value[2U]));
    result[axis] = stored_float32(component);
  }
  return result;
}

CollisionVector3 world_to_body(const BodyBasis3 &basis,
                               const CollisionVector3 &value) {
  CollisionVector3 result{};
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    auto component = static_cast<long double>(stored_float32(basis[axis][0U])) *
                     static_cast<long double>(stored_float32(value[0U]));
    component += static_cast<long double>(stored_float32(basis[axis][1U])) *
                 static_cast<long double>(stored_float32(value[1U]));
    component += static_cast<long double>(stored_float32(basis[axis][2U])) *
                 static_cast<long double>(stored_float32(value[2U]));
    result[axis] = stored_float32(component);
  }
  return result;
}

CollisionVector3
project_world_point_to_body(const OriginalBodyPoseState &pose,
                            const CollisionVector3 &world_point) {
  const auto delta_x =
      static_cast<long double>(stored_float32(world_point[0U])) -
      static_cast<long double>(stored_float32(pose.world_position[0U]));
  const auto delta_y =
      static_cast<long double>(stored_float32(world_point[1U])) -
      static_cast<long double>(stored_float32(pose.world_position[1U]));
  const auto delta_z =
      static_cast<long double>(stored_float32(world_point[2U])) -
      static_cast<long double>(stored_float32(pose.world_position[2U]));
  const auto stored_delta_x =
      static_cast<long double>(stored_float32(delta_x));
  const auto stored_delta_y =
      static_cast<long double>(stored_float32(delta_y));
  const auto stored_delta_z =
      static_cast<long double>(stored_float32(delta_z));
  const auto component = [&](const std::size_t axis,
                             const bool first_component) {
    const auto x = first_component ? delta_x : stored_delta_x;
    const auto y = first_component ? delta_y : stored_delta_y;
    const auto z = first_component ? delta_z : stored_delta_z;
    auto value =
        y * static_cast<long double>(stored_float32(pose.body_basis[axis][1U]));
    value +=
        x * static_cast<long double>(stored_float32(pose.body_basis[axis][0U]));
    value +=
        z * static_cast<long double>(stored_float32(pose.body_basis[axis][2U]));
    return stored_float32(value);
  };
  return {component(0U, true), component(1U, false),
          component(2U, false)};
}

CollisionVector3 project_body_point_to_world_exact(
    const OriginalBodyPoseState &pose, const CollisionVector3 &local_point) {
  CollisionVector3 result{};
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    auto component =
        static_cast<long double>(stored_float32(local_point[1U])) *
        static_cast<long double>(stored_float32(pose.body_basis[1U][axis]));
    component +=
        static_cast<long double>(stored_float32(local_point[0U])) *
        static_cast<long double>(stored_float32(pose.body_basis[0U][axis]));
    component +=
        static_cast<long double>(stored_float32(local_point[2U])) *
        static_cast<long double>(stored_float32(pose.body_basis[2U][axis]));
    component +=
        static_cast<long double>(stored_float32(pose.world_position[axis]));
    result[axis] = stored_float32(component);
  }
  return result;
}

CollisionVector3
component_inverse_inertia(const CollisionVector3 &value,
                          const OriginalBodyMassProperties &properties) {
  return {
      stored_float32(
          (1.0L / static_cast<long double>(
                       stored_float32(properties.principal_inertia[0U]))) *
          static_cast<long double>(stored_float32(value[0U]))),
      stored_float32(
          (1.0L / static_cast<long double>(
                       stored_float32(properties.principal_inertia[1U]))) *
          static_cast<long double>(stored_float32(value[1U]))),
      stored_float32(
          (1.0L / static_cast<long double>(
                       stored_float32(properties.principal_inertia[2U]))) *
          static_cast<long double>(stored_float32(value[2U]))),
  };
}

BodyBasis3 relative_body_basis(const BodyBasis3 &first,
                               const BodyBasis3 &second) {
  BodyBasis3 result{};
  for (std::size_t first_axis = 0U; first_axis < 3U; ++first_axis) {
    for (std::size_t second_axis = 0U; second_axis < 3U; ++second_axis) {
      result[first_axis][second_axis] =
          stored_float32(dot(first[first_axis], second[second_axis]));
    }
  }
  return result;
}

CollisionVector3 relative_body_origin(const OriginalBodyPoseState &first,
                                      const OriginalBodyPoseState &second) {
  CollisionVector3 result{};
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    const auto first_projection =
        stored_float32(dot(first.world_position, second.body_basis[axis]));
    const auto second_projection =
        static_cast<long double>(
            stored_float32(second.world_position[1U])) *
        static_cast<long double>(
            stored_float32(second.body_basis[axis][1U]));
    auto second_sum =
        static_cast<long double>(
            stored_float32(second.world_position[0U])) *
            static_cast<long double>(
                stored_float32(second.body_basis[axis][0U])) +
        second_projection;
    second_sum +=
        static_cast<long double>(
            stored_float32(second.world_position[2U])) *
        static_cast<long double>(
            stored_float32(second.body_basis[axis][2U]));
    result[axis] = stored_float32(
        static_cast<long double>(first_projection) - second_sum);
  }
  return result;
}

CollisionVector3 filter_against_active_contacts(
    CollisionVector3 world_value,
    const std::vector<CollisionVector3> &active_world_normals) {
  for (const auto &world_normal : active_world_normals) {
    require_finite(world_normal, "dynamic contact constraint normal");
    const auto projection = dot(world_value, world_normal);
    // RVA 0x000a4f51 loads zero above the stored projection before fcompp,
    // then skips removal when zero is below-or-equal. Only a negative
    // projection is removed from the dynamic correction/linear impulse.
    if (projection < 0.0) {
      world_value =
          subtract(world_value, scaled(world_normal, projection));
    }
  }
  return world_value;
}

struct ConvexPointCandidate {
  CollisionVector3 world_point{};
  CollisionVector3 world_outward_normal{};
  double penetration = 0.0;
  std::uint16_t surface = 0U;
};

std::optional<ConvexPointCandidate> find_deepest_contained_point(
    const OriginalVehicleSceneState &point_body,
    const OriginalDynamicVehicleContactShape &point_shape,
    const OriginalVehicleSceneState &hull_body,
    const OriginalDynamicVehicleContactShape &hull_shape,
    const CollisionVector3 &world_exit_direction) {
  const auto local_exit_direction =
      world_to_body(hull_body.pose.body_basis, world_exit_direction);
  std::optional<ConvexPointCandidate> deepest;
  for (const auto &local_point : point_shape.local_points) {
    const auto world_point =
        project_body_point_to_world_exact(point_body.pose, local_point);
    const auto hull_point =
        project_world_point_to_body(hull_body.pose, world_point);
    const OriginalDynamicVehicleContactPlane *exit_plane = nullptr;
    auto minimum_exit_distance = std::numeric_limits<double>::max();
    auto contained = true;
    for (const auto &plane : hull_shape.local_planes) {
      const auto plane_value = stored_float32(
          static_cast<long double>(dot(plane.local_normal, hull_point)) +
          static_cast<long double>(stored_float32(plane.distance)));
      if (plane_value > 0.0) {
        contained = false;
        break;
      }
      auto direction_projection =
          stored_float32(dot(local_exit_direction, plane.local_normal));
      if (direction_projection <= 0.0) {
        continue;
      }
      // RVA 0x00026dee clamps a small positive divisor to the exact 0.01
      // single-precision constant before selecting the shortest exit plane.
      direction_projection = std::max(direction_projection, 0.01);
      const auto exit_distance = stored_float32(
          -static_cast<long double>(plane_value) /
          static_cast<long double>(direction_projection));
      if (exit_distance < minimum_exit_distance) {
        minimum_exit_distance = exit_distance;
        exit_plane = &plane;
      }
    }
    if (!contained || exit_plane == nullptr ||
        !std::isfinite(minimum_exit_distance)) {
      continue;
    }
    if (!deepest.has_value() || minimum_exit_distance > deepest->penetration) {
      const auto world_normal =
          body_to_world(hull_body.pose.body_basis, exit_plane->local_normal);
      if (dot(world_normal, world_normal) <= 1.0e-18) {
        throw std::runtime_error(
            "dynamic contact convex plane produced a zero world normal");
      }
      deepest =
          ConvexPointCandidate{world_point, world_normal, minimum_exit_distance,
                               exit_plane->surface};
    }
  }
  return deepest;
}

} // namespace

OriginalDynamicVehicleContactResponse
calculate_original_dynamic_vehicle_contact_response(
    const OriginalVehicleSceneState &first,
    const OriginalDynamicVehicleContactShape &first_shape,
    const OriginalVehicleSceneState &second,
    const OriginalDynamicVehicleContactShape &second_shape,
    const CollisionVector3 &world_normal,
    const CollisionVector3 &world_contact_point) {
  require_shape(first_shape, "first dynamic contact shape");
  require_shape(second_shape, "second dynamic contact shape");
  require_finite(world_normal, "dynamic contact normal");
  require_finite(world_contact_point, "dynamic contact point");

  const auto normal_length =
      std::sqrt(std::max(0.0, dot(world_normal, world_normal)));
  if (normal_length <= 1.0e-9) {
    throw std::invalid_argument("dynamic contact normal is zero");
  }
  // The accepted candidate owner supplies an already-normalized plane. The
  // response does not normalize it again; it first expresses the plane,
  // contact point, and both point velocities in the second body's frame.
  const CollisionVector3 normal{
      stored_float32(world_normal[0U]), stored_float32(world_normal[1U]),
      stored_float32(world_normal[2U])};
  const auto second_normal = world_to_body(second.pose.body_basis, normal);
  const auto second_contact =
      project_world_point_to_body(second.pose, world_contact_point);
  const auto relative_basis =
      relative_body_basis(first.pose.body_basis, second.pose.body_basis);
  const auto first_origin_in_second =
      relative_body_origin(first.pose, second.pose);
  const OriginalBodyPoseState relative_pose{relative_basis,
                                            first_origin_in_second};
  const auto first_contact =
      project_world_point_to_body(relative_pose, second_contact);

  // The p3.1 dynamic owner forms point velocity as r x omega rather than the
  // more usual omega x r. Preserve that stored-angular convention.
  const auto first_contact_local =
      add(first.velocity.local_linear,
          cross(first_contact, first.velocity.local_angular));
  const auto second_contact_local =
      add(second.velocity.local_linear,
          cross(second_contact, second.velocity.local_angular));
  const auto relative_in_second =
      subtract(body_to_world(relative_basis, first_contact_local),
               second_contact_local);

  OriginalDynamicVehicleContactResponse result;
  result.world_normal = normal;
  result.world_contact_point = world_contact_point;
  result.normal_relative_velocity =
      dot(relative_in_second, second_normal);
  result.collision_sound_scalar = stored_float32(
      -2.0L * static_cast<long double>(result.normal_relative_velocity));

  // RVA 0x000a519e..0x000a536d uses the same second-frame contact arm and
  // normal for both principal-inertia terms, then transforms only the first
  // body's applied deltas back through the relative basis.
  const auto shared_angular_arm = cross(second_contact, second_normal);
  const auto first_angular_axis = component_inverse_inertia(
      shared_angular_arm, first_shape.mass_properties);
  const auto second_angular_axis = component_inverse_inertia(
      shared_angular_arm, second_shape.mass_properties);
  const auto first_angular_contact =
      cross(first_angular_axis, second_contact);
  const auto second_angular_contact =
      cross(second_angular_axis, second_contact);
  const auto first_inverse_mass =
      stored_float32(1.0L / static_cast<long double>(stored_float32(
                                first_shape.mass_properties.mass)));
  const auto second_inverse_mass =
      stored_float32(1.0L / static_cast<long double>(stored_float32(
                                second_shape.mass_properties.mass)));
  const auto stored_inverse_mass_sum = stored_float32(
      static_cast<long double>(first_inverse_mass) +
      static_cast<long double>(second_inverse_mass));
  const auto angular_effective_mass =
      add(first_angular_contact, second_angular_contact);
  auto effective_inverse_mass =
      static_cast<long double>(
          dot(angular_effective_mass, second_normal)) +
      static_cast<long double>(stored_inverse_mass_sum);
  result.effective_inverse_mass = static_cast<double>(effective_inverse_mass);
  if (!std::isfinite(result.effective_inverse_mass) ||
      result.effective_inverse_mass <= 0.0) {
    throw std::runtime_error(
        "dynamic contact effective inverse mass is invalid");
  }

  constexpr double original_restitution_numerator = -2.0;
  const auto stored_numerator = stored_float32(
      static_cast<long double>(original_restitution_numerator) *
      static_cast<long double>(result.normal_relative_velocity));
  result.impulse = stored_float32(
      static_cast<long double>(stored_numerator) /
      static_cast<long double>(result.effective_inverse_mass));

  auto filtered_world_normal =
      body_to_world(second.pose.body_basis, second_normal);
  filtered_world_normal = filter_against_active_contacts(
      filtered_world_normal, second.dynamic_contact_world_normals);
  const auto filtered_second_normal =
      world_to_body(second.pose.body_basis, filtered_world_normal);
  const auto first_linear_in_second =
      scaled(filtered_second_normal,
             stored_float32(static_cast<long double>(result.impulse) *
                            static_cast<long double>(first_inverse_mass)));
  result.first_local_linear_velocity_delta =
      world_to_body(relative_basis, first_linear_in_second);
  result.second_local_linear_velocity_delta =
      scaled(filtered_second_normal,
             stored_float32(static_cast<long double>(result.impulse) *
                            -static_cast<long double>(second_inverse_mass)));
  constexpr double original_angular_application_scale = 0.1;
  const auto angular_scale = stored_float32(
      static_cast<long double>(result.impulse) *
      static_cast<long double>(original_angular_application_scale));
  result.first_local_angular_velocity_delta =
      world_to_body(relative_basis,
                    scaled(first_angular_axis, angular_scale));
  result.second_local_angular_velocity_delta =
      scaled(second_angular_axis, -angular_scale);
  return result;
}

bool resolve_original_dynamic_vehicle_contact(
    const OriginalVehicleSceneState &first,
    const OriginalDynamicVehicleContactShape &first_shape,
    const OriginalVehicleSceneState &second,
    const OriginalDynamicVehicleContactShape &second_shape,
    OriginalDynamicVehicleContactResponse &response) {
  require_shape(first_shape, "first dynamic contact shape");
  require_shape(second_shape, "second dynamic contact shape");
  const auto first_world_center = first.pose.world_position;
  const auto second_world_center = second.pose.world_position;
  const auto center_delta = subtract(second_world_center, first_world_center);
  const auto center_distance = std::sqrt(dot(center_delta, center_delta));
  if (center_distance >
      first_shape.bounding_radius + second_shape.bounding_radius) {
    return false;
  }
  if (center_distance <= 1.0e-9) {
    return false;
  }

  const auto first_to_second = normalized_stored(center_delta);
  const auto second_in_first = find_deepest_contained_point(
      second, second_shape, first, first_shape, first_to_second);
  const auto first_in_second = find_deepest_contained_point(
      first, first_shape, second, second_shape, scaled(first_to_second, -1.0));
  if (!second_in_first.has_value() && !first_in_second.has_value()) {
    return false;
  }

  const auto use_second_in_first =
      second_in_first.has_value() &&
      (!first_in_second.has_value() ||
       second_in_first->penetration >= first_in_second->penetration);
  const auto &candidate =
      use_second_in_first ? *second_in_first : *first_in_second;
  // The impulse owner expects its normal from the second body toward the
  // first. A point of the second inside the first therefore uses the inverse
  // of the first hull's outward exit plane; the reciprocal candidate directly
  // uses the second hull's outward plane.
  const auto world_normal = use_second_in_first
                                ? scaled(candidate.world_outward_normal, -1.0)
                                : candidate.world_outward_normal;
  response.penetration = candidate.penetration;
  constexpr double original_penetration_scale = 1.2;
  constexpr double dynamic_body_share = 0.5;
  const auto first_origin_in_second =
      relative_body_origin(first.pose, second.pose);
  const auto second_to_first_in_second =
      normalized_stored(first_origin_in_second);
  const auto full_correction_in_second = scaled(
      second_to_first_in_second,
      -response.penetration * original_penetration_scale);
  const auto full_correction =
      body_to_world(second.pose.body_basis, full_correction_in_second);
  const auto corrected_contact_point =
      subtract(candidate.world_point, full_correction);
  response = calculate_original_dynamic_vehicle_contact_response(
      first, first_shape, second, second_shape, world_normal,
      corrected_contact_point);
  response.penetration = candidate.penetration;
  const auto filtered_correction = filter_against_active_contacts(
      full_correction, second.dynamic_contact_world_normals);
  const auto shared_correction =
      scaled(filtered_correction, dynamic_body_share);
  response.first_world_position_delta =
      scaled(shared_correction, -1.0);
  response.second_world_position_delta = shared_correction;
  return true;
}

bool resolve_original_direct_body_contact(
    const OriginalVehicleSceneState &vehicle,
    const OriginalDynamicVehicleContactShape &vehicle_shape,
    const OriginalVehicleSceneState &direct_body,
    const OriginalDynamicVehicleContactShape &direct_body_shape,
    OriginalDynamicVehicleContactResponse &response) {
  require_shape(vehicle_shape, "vehicle direct contact shape");
  require_shape(direct_body_shape, "environment direct contact shape");
  const auto center_delta =
      subtract(direct_body.pose.world_position, vehicle.pose.world_position);
  const auto center_distance = std::sqrt(dot(center_delta, center_delta));
  if (center_distance >
      vehicle_shape.bounding_radius + direct_body_shape.bounding_radius) {
    return false;
  }
  if (center_distance <= 1.0e-9) {
    return false;
  }

  const auto vehicle_to_direct = normalized_stored(center_delta);
  const auto direct_in_vehicle = find_deepest_contained_point(
      direct_body, direct_body_shape, vehicle, vehicle_shape,
      vehicle_to_direct);
  const auto vehicle_in_direct = find_deepest_contained_point(
      vehicle, vehicle_shape, direct_body, direct_body_shape,
      scaled(vehicle_to_direct, -1.0));
  if (!direct_in_vehicle.has_value() && !vehicle_in_direct.has_value()) {
    return false;
  }

  const auto use_direct_in_vehicle =
      direct_in_vehicle.has_value() &&
      (!vehicle_in_direct.has_value() ||
       direct_in_vehicle->penetration >= vehicle_in_direct->penetration);
  const auto &candidate =
      use_direct_in_vehicle ? *direct_in_vehicle : *vehicle_in_direct;
  const auto world_normal =
      use_direct_in_vehicle
          ? scaled(candidate.world_outward_normal, -1.0)
          : candidate.world_outward_normal;
  constexpr double original_penetration_scale = 1.2;
  const auto vehicle_origin_in_direct =
      relative_body_origin(vehicle.pose, direct_body.pose);
  const auto direct_to_vehicle_in_direct =
      normalized_stored(vehicle_origin_in_direct);
  const auto full_correction_in_direct = scaled(
      direct_to_vehicle_in_direct,
      -candidate.penetration * original_penetration_scale);
  const auto full_correction =
      body_to_world(direct_body.pose.body_basis, full_correction_in_direct);
  const auto corrected_contact_point =
      subtract(candidate.world_point, full_correction);
  response = calculate_original_dynamic_vehicle_contact_response(
      vehicle, vehicle_shape, direct_body, direct_body_shape, world_normal,
      corrected_contact_point);
  response.penetration = candidate.penetration;
  response.first_world_position_delta = {};
  response.second_world_position_delta = filter_against_active_contacts(
      full_correction, direct_body.dynamic_contact_world_normals);
  return true;
}

} // namespace mh::game
