#include <game/physics/wheel_contact.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace mh::game {
namespace {

void require_finite(const double value, const char *name) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(name);
  }
}

double stored_float32(const double value) {
  return static_cast<double>(static_cast<float>(value));
}

void validate_rig(const WheelProbeRig &rig) {
  require_finite(rig.query_distance, "wheel query distance must be finite");
  require_finite(rig.authored_spring_length,
                 "authored spring length must be finite");
  require_finite(rig.authored_spring_strength,
                 "authored spring strength must be finite");
  if (rig.query_distance <= 0.0 || rig.authored_spring_length <= 0.0 ||
      rig.authored_spring_strength <= 0.0) {
    throw std::invalid_argument("wheel-probe rig values must be positive");
  }
  for (const auto &origin : rig.local_origins) {
    for (const auto component : origin) {
      require_finite(component, "wheel-probe origin must be finite");
    }
  }
}

void validate_transform(const VehicleTransform &transform) {
  for (const auto component : transform.position) {
    require_finite(component, "vehicle transform position must be finite");
  }
  require_finite(transform.heading_radians,
                 "vehicle transform heading must be finite");
}

void validate_pose(const OriginalBodyPoseState &pose) {
  for (const auto component : pose.world_position) {
    require_finite(component, "body pose position must be finite");
  }
  for (const auto &row : pose.body_basis) {
    for (const auto component : row) {
      require_finite(component, "body pose basis must be finite");
    }
  }
}

CollisionVector3 transform_vector(const CollisionVector3 &local,
                                  const OriginalBodyPoseState &pose) {
  return project_body_vector_to_world(pose.body_basis, local);
}

CollisionVector3 transform_vector_retail(const CollisionVector3 &local,
                                         const OriginalBodyPoseState &pose) {
  CollisionVector3 world{};
  constexpr std::array<std::size_t, 3U> retail_term_order{1U, 0U, 2U};
  for (std::size_t world_axis = 0U; world_axis < world.size(); ++world_axis) {
    auto component = static_cast<long double>(0.0);
    for (const auto local_axis : retail_term_order) {
      component += static_cast<long double>(stored_float32(
                       pose.body_basis[local_axis][world_axis])) *
                   static_cast<long double>(stored_float32(local[local_axis]));
    }
    world[world_axis] = stored_float32(component);
  }
  return world;
}

CollisionVector3 transform_point(const CollisionVector3 &local,
                                 const OriginalBodyPoseState &pose) {
  const auto world = transform_vector(local, pose);
  return {pose.world_position[0] + world[0], pose.world_position[1] + world[1],
          pose.world_position[2] + world[2]};
}

CollisionVector3 transform_point_retail(const CollisionVector3 &local,
                                        const OriginalBodyPoseState &pose) {
  const auto world = transform_vector_retail(local, pose);
  return {stored_float32(stored_float32(pose.world_position[0U]) + world[0U]),
          stored_float32(stored_float32(pose.world_position[1U]) + world[1U]),
          stored_float32(stored_float32(pose.world_position[2U]) + world[2U])};
}

void validate_scalar_states(
    const std::array<WheelContactScalarState, 4U> &states) {
  for (const auto &state : states) {
    require_finite(state.state_fraction, "wheel scalar state must be finite");
    require_finite(state.complement_fraction,
                   "wheel scalar state complement must be finite");
    if (state.state_fraction < 0.0 || state.state_fraction > 1.0 ||
        state.complement_fraction < 0.0 || state.complement_fraction > 1.0 ||
        std::fabs(state.state_fraction + state.complement_fraction - 1.0) >
            2.5e-7) {
      throw std::invalid_argument(
          "wheel scalar state is outside its valid range");
    }
  }
}

} // namespace

OriginalBodyPoseState
make_original_body_pose(const VehicleTransform &transform) {
  validate_transform(transform);
  const auto sine = std::sin(transform.heading_radians);
  const auto cosine = std::cos(transform.heading_radians);
  return {{{{cosine, 0.0, -sine}, {0.0, 1.0, 0.0}, {sine, 0.0, cosine}}},
          transform.position};
}

std::array<WheelProbeSample, 4U>
sample_wheel_probes(const WheelProbeRig &rig, const VehicleTransform &transform,
                    const CollisionWorld &world) {
  return sample_wheel_probes(rig, make_original_body_pose(transform), world);
}

std::array<WheelProbeSample, 4U>
sample_wheel_probes(const WheelProbeRig &rig, const OriginalBodyPoseState &pose,
                    const CollisionWorld &world) {
  validate_rig(rig);
  validate_pose(pose);

  constexpr std::array corners{WheelCorner::front_left,
                               WheelCorner::front_right,
                               WheelCorner::back_right, WheelCorner::back_left};
  std::array<WheelProbeSample, 4U> result{};
  for (std::size_t index = 0U; index < result.size(); ++index) {
    result[index].corner = corners[index];
    result[index].world_origin =
        transform_point(rig.local_origins[index], pose);
    const auto world_down =
        transform_vector(CollisionVector3{0.0, -1.0, 0.0}, pose);
    result[index].hit = world.raycast(result[index].world_origin, world_down,
                                      rig.query_distance);
  }
  return result;
}

std::array<WheelSpringSegmentSample, 4U>
sample_wheel_spring_segments(const WheelProbeRig &rig,
                             const VehicleTransform &transform,
                             const CollisionWorld &world) {
  return sample_wheel_spring_segments(rig, make_original_body_pose(transform),
                                      world);
}

std::array<WheelSpringSegmentSample, 4U>
sample_wheel_spring_segments(const WheelProbeRig &rig,
                             const OriginalBodyPoseState &pose,
                             const CollisionWorld &world) {
  return sample_wheel_spring_segments(
      rig,
      make_original_wheel_response(rig.authored_spring_length,
                                   rig.authored_spring_strength,
                                   OriginalWheelResponseProfile::standard),
      pose, world);
}

std::array<WheelSpringSegmentSample, 4U> sample_wheel_spring_segments(
    const WheelProbeRig &rig, const OriginalWheelResponseParameters &response,
    const VehicleTransform &transform, const CollisionWorld &world) {
  return sample_wheel_spring_segments(
      rig, response, make_original_body_pose(transform), world);
}

std::array<WheelSpringSegmentSample, 4U> sample_wheel_spring_segments(
    const WheelProbeRig &rig, const OriginalWheelResponseParameters &response,
    const OriginalBodyPoseState &pose, const CollisionWorld &world) {
  validate_rig(rig);
  validate_pose(pose);
  require_finite(response.spring_length,
                 "wheel response spring length must be finite");
  require_finite(response.spring_strength,
                 "wheel response spring strength must be finite");
  require_finite(response.previous_minus_current_scale,
                 "wheel response state-delta scale must be finite");
  if (response.spring_length <= 0.0 || response.spring_strength <= 0.0) {
    throw std::invalid_argument(
        "wheel response spring values must be positive");
  }

  constexpr std::array corners{WheelCorner::front_left,
                               WheelCorner::front_right,
                               WheelCorner::back_right, WheelCorner::back_left};
  const CollisionVector3 local_axis{0.0, -response.spring_length, 0.0};
  const auto world_axis = transform_vector_retail(local_axis, pose);

  std::array<WheelSpringSegmentSample, 4U> result{};
  for (std::size_t index = 0U; index < result.size(); ++index) {
    auto &sample = result[index];
    sample.corner = corners[index];
    sample.world_center =
        transform_point_retail(rig.local_origins[index], pose);
    // Retail submits the lower endpoint first and the upper endpoint second.
    for (std::size_t axis = 0U; axis < sample.segment_start.size(); ++axis) {
      sample.segment_start[axis] =
          stored_float32(sample.world_center[axis] + world_axis[axis]);
      sample.segment_end[axis] =
          stored_float32(sample.world_center[axis] - world_axis[axis]);
    }
    sample.hit =
        world.original_segment_cast(sample.segment_start, sample.segment_end);
    if (sample.hit.has_value()) {
      // The query record stores the complement of travel from endpoint A.
      auto segment_length_squared = 0.0;
      for (std::size_t axis = 0U; axis < sample.segment_start.size(); ++axis) {
        const auto delta =
            sample.segment_end[axis] - sample.segment_start[axis];
        segment_length_squared += delta * delta;
      }
      sample.hit_fraction = stored_float32(
          1.0 - sample.hit->distance / std::sqrt(segment_length_squared));
      sample.scalar_state =
          map_original_wheel_contact_fraction(sample.hit_fraction);
    }
  }
  return result;
}

WheelContactScalarState
map_original_wheel_contact_fraction(const std::optional<double> hit_fraction) {
  if (!hit_fraction.has_value()) {
    return {};
  }
  require_finite(*hit_fraction, "wheel contact fraction must be finite");
  if (*hit_fraction < 0.0 || *hit_fraction > 1.0) {
    throw std::invalid_argument("wheel contact fraction must be within [0, 1]");
  }

  const auto state =
      *hit_fraction <= 0.5
          ? 0.5
          : std::clamp(stored_float32((*hit_fraction - 0.5) * 2.0), 0.0, 1.0);
  return WheelContactScalarState{state, stored_float32(1.0 - state)};
}

OriginalWheelResponseParameters
make_original_wheel_response(const double authored_spring_length,
                             const double authored_spring_strength,
                             const OriginalWheelResponseProfile profile) {
  require_finite(authored_spring_length,
                 "authored wheel spring length must be finite");
  require_finite(authored_spring_strength,
                 "authored wheel spring strength must be finite");
  if (authored_spring_length <= 0.0 || authored_spring_strength <= 0.0) {
    throw std::invalid_argument(
        "authored wheel spring values must be positive");
  }

  switch (profile) {
  case OriginalWheelResponseProfile::standard:
    return {authored_spring_length, authored_spring_strength, 65000.0};
  case OriginalWheelResponseProfile::demon_grem:
    return {authored_spring_length * 3.0, authored_spring_strength * 0.16,
            21666.0};
  case OriginalWheelResponseProfile::g_ride_west:
    return {authored_spring_length * 1.2, authored_spring_strength * 0.5,
            -50000.0};
  }
  throw std::invalid_argument("unknown original wheel response profile");
}

OriginalWheelResponseScalars calculate_original_wheel_response_scalars(
    const OriginalWheelResponseParameters &response,
    const WheelContactScalarState &state,
    const WheelContactScalarTransition &transition) {
  require_finite(response.spring_length,
                 "wheel response spring length must be finite");
  require_finite(response.spring_strength,
                 "wheel response spring strength must be finite");
  require_finite(response.previous_minus_current_scale,
                 "wheel response state-delta scale must be finite");
  require_finite(state.state_fraction, "wheel scalar state must be finite");
  require_finite(state.complement_fraction,
                 "wheel scalar state complement must be finite");
  require_finite(transition.previous_state_fraction,
                 "previous wheel scalar state must be finite");
  require_finite(transition.current_state_fraction,
                 "current wheel scalar state must be finite");
  require_finite(transition.previous_minus_current,
                 "wheel scalar transition must be finite");
  if (response.spring_length <= 0.0 || response.spring_strength <= 0.0 ||
      state.state_fraction < 0.0 || state.state_fraction > 1.0 ||
      state.complement_fraction < 0.0 || state.complement_fraction > 1.0 ||
      std::fabs(state.state_fraction + state.complement_fraction - 1.0) >
          2.5e-7 ||
      transition.previous_state_fraction < 0.0 ||
      transition.previous_state_fraction > 1.0 ||
      transition.current_state_fraction < 0.0 ||
      transition.current_state_fraction > 1.0 ||
      std::fabs(transition.current_state_fraction - state.state_fraction) >
          2.5e-7 ||
      std::fabs(transition.previous_state_fraction -
                transition.current_state_fraction -
                transition.previous_minus_current) > 2.5e-7) {
    throw std::invalid_argument(
        "wheel response inputs are outside their valid range");
  }
  return OriginalWheelResponseScalars{
      stored_float32(response.spring_strength * state.complement_fraction *
                     (state.complement_fraction + 0.1)),
      transition.previous_minus_current *
          response.previous_minus_current_scale};
}

std::array<WheelContactScalarTransition, 4U> WheelContactScalarHistory::update(
    const std::array<WheelContactScalarState, 4U> &current) {
  validate_scalar_states(current);

  std::array<WheelContactScalarTransition, 4U> transitions{};
  for (std::size_t index = 0U; index < current.size(); ++index) {
    transitions[index] = WheelContactScalarTransition{
        previous_[index], current[index].state_fraction,
        previous_[index] - current[index].state_fraction};
    previous_[index] = current[index].state_fraction;
  }
  return transitions;
}

void WheelContactScalarHistory::seed(
    const std::array<WheelContactScalarState, 4U> &current) {
  validate_scalar_states(current);
  std::transform(current.begin(), current.end(), previous_.begin(),
                 [](const WheelContactScalarState &state) {
                   return state.state_fraction;
                 });
}

void WheelContactScalarHistory::reset() noexcept { previous_.fill(1.0); }

WheelContactSystem::WheelContactSystem(
    WheelProbeRig rig, const OriginalWheelResponseProfile profile)
    : rig_(rig),
      response_(make_original_wheel_response(rig_.authored_spring_length,
                                             rig_.authored_spring_strength,
                                             profile)) {
  validate_rig(rig_);
}

WheelContactFrame WheelContactSystem::sample(const VehicleTransform &transform,
                                             const CollisionWorld &world) {
  return sample(make_original_body_pose(transform), world, false);
}

WheelContactFrame WheelContactSystem::sample(const OriginalBodyPoseState &pose,
                                             const CollisionWorld &world) {
  return sample(pose, world, false);
}

WheelContactFrame
WheelContactSystem::sample(const OriginalBodyPoseState &pose,
                           const CollisionWorld &world,
                           const bool reuse_retained_surfaces) {
  auto frame = WheelContactFrame{};
  frame.wheels = sample_wheel_spring_segments(rig_, response_, pose, world);
  for (std::size_t index = 0U; index < frame.wheels.size(); ++index) {
    auto &wheel = frame.wheels[index];
    if (reuse_retained_surfaces && retained_group_hits_[index].has_value()) {
      wheel.hit = world.original_retained_segment_cast(
          wheel.segment_start, wheel.segment_end, *retained_group_hits_[index]);
      wheel.hit_fraction.reset();
      wheel.scalar_state = {};
      if (wheel.hit.has_value()) {
        auto length_squared = 0.0;
        for (std::size_t axis = 0U; axis < wheel.segment_start.size(); ++axis) {
          const auto delta =
              wheel.segment_end[axis] - wheel.segment_start[axis];
          length_squared += delta * delta;
        }
        wheel.hit_fraction = stored_float32(
            1.0 - wheel.hit->distance / std::sqrt(length_squared));
        wheel.scalar_state =
            map_original_wheel_contact_fraction(wheel.hit_fraction);
      }
    }
    if (wheel.hit.has_value()) {
      retained_group_hits_[index] = wheel.hit;
    }
  }
  std::array<WheelContactScalarState, 4U> current{};
  std::transform(
      frame.wheels.begin(), frame.wheels.end(), current.begin(),
      [](const WheelSpringSegmentSample &wheel) { return wheel.scalar_state; });
  frame.transitions = history_.update(current);
  for (std::size_t index = 0U; index < frame.wheels.size(); ++index) {
    frame.response_scalars[index] = calculate_original_wheel_response_scalars(
        response_, frame.wheels[index].scalar_state, frame.transitions[index]);
  }
  return frame;
}

const WheelProbeRig &WheelContactSystem::rig() const noexcept { return rig_; }

const OriginalWheelResponseParameters &
WheelContactSystem::response() const noexcept {
  return response_;
}

void WheelContactSystem::seed_previous_state(
    const std::array<WheelContactScalarState, 4U> &current) {
  history_.seed(current);
}

void WheelContactSystem::begin_response_group() noexcept {
  retained_group_hits_.fill(std::nullopt);
}

void WheelContactSystem::reset() noexcept {
  history_.reset();
  begin_response_group();
}

} // namespace mh::game
