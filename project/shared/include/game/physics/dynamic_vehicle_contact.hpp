#pragma once

#include <game/vehicle/scene.hpp>

#include <cstdint>
#include <vector>

namespace mh::game {

struct OriginalDynamicVehicleContactPlane {
  CollisionVector3 local_normal{};
  double distance = 0.0;
  std::uint16_t surface = 0U;
};

struct OriginalDynamicVehicleContactShape {
  std::vector<CollisionVector3> local_points;
  std::vector<OriginalDynamicVehicleContactPlane> local_planes;
  CollisionVector3 local_center_of_mass{};
  OriginalBodyMassProperties mass_properties{};
  double bounding_radius = 0.0;
  double half_width = 0.0;
  double half_length = 0.0;
};

struct OriginalDynamicVehicleContactResponse {
  CollisionVector3 world_normal{};
  CollisionVector3 world_contact_point{};
  CollisionVector3 first_world_position_delta{};
  CollisionVector3 second_world_position_delta{};
  CollisionVector3 first_local_linear_velocity_delta{};
  CollisionVector3 first_local_angular_velocity_delta{};
  CollisionVector3 second_local_linear_velocity_delta{};
  CollisionVector3 second_local_angular_velocity_delta{};
  double normal_relative_velocity = 0.0;
  // p3.1 retains this pre-effective-mass numerator at body+0x734 for
  // ObjectColSound. It is -2 times closing speed, not the final impulse.
  double collision_sound_scalar = 0.0;
  double effective_inverse_mass = 0.0;
  double penetration = 0.0;
  double impulse = 0.0;
};

// Reproduces the accepted p3.1 dynamic-body response at
// RVA 0x000a5032..0x000a5598. The collision normal points from the second body
// toward the first body. The original uses a perfectly elastic -2*v numerator,
// both inverse masses, principal-inertia contact-arm terms, and a separate 0.1
// angular application scale. It applies that signed result even when the
// retained overlap is separating.
[[nodiscard]] OriginalDynamicVehicleContactResponse
calculate_original_dynamic_vehicle_contact_response(
    const OriginalVehicleSceneState &first,
    const OriginalDynamicVehicleContactShape &first_shape,
    const OriginalVehicleSceneState &second,
    const OriginalDynamicVehicleContactShape &second_shape,
    const CollisionVector3 &world_normal,
    const CollisionVector3 &world_contact_point);

// Reproduces the p3.1 candidate owner at RVA 0x000a4df6..0x000a4ebc: a
// bounding-sphere rejection followed by the original four authored CAR points
// tested in both directions against the convex planes imported from the car
// COL. The deepest contained point supplies the response plane and point. For
// two dynamic bodies, the exact 1.2 penetration correction is split equally
// before the velocity response.
[[nodiscard]] bool resolve_original_dynamic_vehicle_contact(
    const OriginalVehicleSceneState &first,
    const OriginalDynamicVehicleContactShape &first_shape,
    const OriginalVehicleSceneState &second,
    const OriginalDynamicVehicleContactShape &second_shape,
    OriginalDynamicVehicleContactResponse &response);

// Direct LWS bodies take the complete penetration correction while the
// vehicle takes none. Both bodies still receive the rigid velocity response.
[[nodiscard]] bool resolve_original_direct_body_contact(
    const OriginalVehicleSceneState &vehicle,
    const OriginalDynamicVehicleContactShape &vehicle_shape,
    const OriginalVehicleSceneState &direct_body,
    const OriginalDynamicVehicleContactShape &direct_body_shape,
    OriginalDynamicVehicleContactResponse &response);

} // namespace mh::game
