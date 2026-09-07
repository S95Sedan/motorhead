#include <game/race/camera.hpp>

#include <content/formats/motion_path.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
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

void require_finite(const OriginalBodyPoseState &pose) {
  require_finite(pose.world_position, "vehicle camera position must be finite");
  for (const auto &row : pose.body_basis) {
    require_finite(row, "vehicle camera basis must be finite");
  }
}

double dot(const CollisionVector3 &left, const CollisionVector3 &right) {
  return left[0U] * right[0U] + left[1U] * right[1U] + left[2U] * right[2U];
}

CollisionVector3 subtract(const CollisionVector3 &left,
                          const CollisionVector3 &right) {
  return {left[0U] - right[0U], left[1U] - right[1U], left[2U] - right[2U]};
}

CollisionVector3 add(const CollisionVector3 &left,
                     const CollisionVector3 &right) {
  return {left[0U] + right[0U], left[1U] + right[1U], left[2U] + right[2U]};
}

CollisionVector3 scale(const CollisionVector3 &value, const double factor) {
  return {value[0U] * factor, value[1U] * factor, value[2U] * factor};
}

CollisionVector3 cross(const CollisionVector3 &left,
                       const CollisionVector3 &right) {
  return {left[1U] * right[2U] - left[2U] * right[1U],
          left[2U] * right[0U] - left[0U] * right[2U],
          left[0U] * right[1U] - left[1U] * right[0U]};
}

CollisionVector3 normalized(const CollisionVector3 &value,
                            const char *message) {
  const auto length_squared = dot(value, value);
  if (!std::isfinite(length_squared) || length_squared <= 1.0e-12) {
    throw std::invalid_argument(message);
  }
  return scale(value, 1.0 / std::sqrt(length_squared));
}

CollisionVector3 lerp(const CollisionVector3 &left,
                      const CollisionVector3 &right, const double alpha) {
  return {left[0U] + (right[0U] - left[0U]) * alpha,
          left[1U] + (right[1U] - left[1U]) * alpha,
          left[2U] + (right[2U] - left[2U]) * alpha};
}

BodyBasis3
original_camera_euler_basis_radians(const std::array<double, 3U> &angles) {
  // Exact component order and matrix layout of p3.1 helper 0x43d1c.
  const auto sine_x = std::sin(angles[0U]);
  const auto cosine_x = std::cos(angles[0U]);
  const auto sine_y = std::sin(angles[1U]);
  const auto cosine_y = std::cos(angles[1U]);
  const auto sine_z = std::sin(angles[2U]);
  const auto cosine_z = std::cos(angles[2U]);
  const auto stored = [](const double value) {
    return static_cast<double>(static_cast<float>(value));
  };
  return {{{stored(sine_z * sine_x * sine_y + cosine_z * cosine_y),
            stored(sine_z * cosine_x),
            stored(sine_z * sine_x * cosine_y - cosine_z * sine_y)},
           {stored(cosine_z * sine_x * sine_y - sine_z * cosine_y),
            stored(cosine_z * cosine_x),
            stored(cosine_z * sine_x * cosine_y + sine_z * sine_y)},
           {stored(cosine_x * sine_y), stored(-sine_x),
            stored(cosine_x * cosine_y)}}};
}

BodyBasis3 original_camera_euler_basis(const std::array<double, 3U> &degrees) {
  constexpr double original_pi = 3.1415926;
  constexpr double degrees_to_radians = original_pi / 180.0;
  return original_camera_euler_basis_radians(
      {degrees[0U] * degrees_to_radians, degrees[1U] * degrees_to_radians,
       degrees[2U] * degrees_to_radians});
}

std::array<double, 3U> original_camera_basis_euler(const BodyBasis3 &basis) {
  // Inverse of 0x43d1c following p3.1 helper 0x43b84. The ordinary branch
  // derives X from -row2.y, then Y and Z from the remaining normalized axes.
  const auto x = std::asin(std::clamp(-basis[2U][1U], -1.0, 1.0));
  const auto cosine_x = std::cos(x);
  if (std::abs(cosine_x) <= 1.0e-8) {
    return {x, 0.0, std::atan2(-basis[0U][2U], basis[0U][0U])};
  }
  return {x, std::atan2(basis[2U][0U], basis[2U][2U]),
          std::atan2(basis[0U][1U], basis[1U][1U])};
}

VehicleCameraPose oriented_camera(const CollisionVector3 &position,
                                  const BodyBasis3 &basis) {
  return {position, add(position, scale(basis[2U], 10.0)), basis[1U]};
}

VehicleCameraPose look_at_camera(const CollisionVector3 &position,
                                 const CollisionVector3 &target) {
  const auto forward = normalized(subtract(target, position),
                                  "camera-spline target is degenerate");
  auto right = cross({0.0, 1.0, 0.0}, forward);
  if (dot(right, right) <= 1.0e-12) {
    right = {1.0, 0.0, 0.0};
  } else {
    right = normalized(right, "camera-spline right axis is degenerate");
  }
  return {
      position, target,
      normalized(cross(forward, right), "camera-spline up axis is degenerate")};
}

CollisionVector3
project_original_outside_position(const OriginalBodyPoseState &vehicle_pose,
                                  const CollisionVector3 &position_local) {
  auto result = project_body_point_to_world(vehicle_pose, position_local);
  // p3.1 RVA 0x000436b6..0x000436f4 first transforms the complete position
  // vector, then replaces only Y with body.y + local.y - body.forward.y * 3.
  // This keeps chassis roll out of the camera height without discarding the
  // original horizontal placement response.
  result[1U] = vehicle_pose.world_position[1U] + position_local[1U] -
               vehicle_pose.body_basis[2U][1U] * 3.0;
  return result;
}

const CollisionVector3 &
outside_position_local(const VehicleCameraMode mode,
                       const VehicleCameraConfig &config) {
  switch (mode) {
  case VehicleCameraMode::chase:
    return config.chase_position_local;
  case VehicleCameraMode::far_chase:
    return config.far_chase_position_local;
  case VehicleCameraMode::in_car:
  case VehicleCameraMode::bumper:
    break;
  }
  throw std::invalid_argument("body-mounted camera has no outside position");
}

bool is_outside_camera(const VehicleCameraMode mode) noexcept {
  return mode == VehicleCameraMode::chase ||
         mode == VehicleCameraMode::far_chase;
}

const std::optional<CameraModeTuning> &
camera_tuning(const VehicleCameraMode mode, const VehicleCameraTuning &tuning) {
  switch (mode) {
  case VehicleCameraMode::chase:
    return tuning.chase;
  case VehicleCameraMode::far_chase:
    return tuning.far_chase;
  case VehicleCameraMode::in_car:
    return tuning.in_car;
  case VehicleCameraMode::bumper:
    return tuning.bumper;
  }
  throw std::invalid_argument("unknown vehicle camera mode");
}

void require_valid_tuning(const CameraModeTuning &tuning) {
  if (!std::isfinite(tuning.height) || !std::isfinite(tuning.distance) ||
      !std::isfinite(tuning.angle_degrees) || tuning.height < -100.0 ||
      tuning.height > 100.0 || tuning.distance < -100.0 ||
      tuning.distance > 100.0 || tuning.angle_degrees < -90.0 ||
      tuning.angle_degrees > 90.0) {
    throw std::invalid_argument(
        "camera tuning must use finite height/distance in [-100,100] and "
        "angle in [-90,90]");
  }
}

VehicleCameraPose make_tuned_camera(const OriginalBodyPoseState &vehicle_pose,
                                    const VehicleCameraMode mode,
                                    const CameraModeTuning &tuning) {
  require_valid_tuning(tuning);
  const CollisionVector3 local_position{0.0, tuning.height, -tuning.distance};
  const auto outside = is_outside_camera(mode);
  const auto position =
      outside ? project_original_outside_position(vehicle_pose, local_position)
              : project_body_point_to_world(vehicle_pose, local_position);
  const auto angle = tuning.angle_degrees * std::numbers::pi / 180.0;
  const auto cosine = std::cos(angle);
  const auto sine = std::sin(angle);

  CollisionVector3 direction;
  CollisionVector3 right;
  if (outside) {
    const auto horizontal_forward = normalized(
        {vehicle_pose.body_basis[2U][0U], 0.0, vehicle_pose.body_basis[2U][2U]},
        "tuned outside-camera forward axis is degenerate");
    right = normalized({horizontal_forward[2U], 0.0, -horizontal_forward[0U]},
                       "tuned outside-camera right axis is degenerate");
    direction = normalized({horizontal_forward[0U] * cosine, -sine,
                            horizontal_forward[2U] * cosine},
                           "tuned outside-camera direction is degenerate");
  } else {
    const auto forward = normalized(vehicle_pose.body_basis[2U],
                                    "tuned camera forward axis is degenerate");
    const auto body_up = normalized(vehicle_pose.body_basis[1U],
                                    "tuned camera up axis is degenerate");
    right = normalized(vehicle_pose.body_basis[0U],
                       "tuned camera right axis is degenerate");
    direction = normalized(add(scale(forward, cosine), scale(body_up, -sine)),
                           "tuned body-camera direction is degenerate");
  }
  const auto up = normalized(cross(direction, right),
                             "tuned camera level axis is degenerate");
  return {position, add(position, scale(direction, 10.0)), up};
}

VehicleCameraPose
make_tuned_hidden_camera(const OriginalBodyPoseState &vehicle_pose,
                         const CameraModeTuning &tuning,
                         const bool compensate_pitch) {
  require_valid_tuning(tuning);
  const auto &body_forward = vehicle_pose.body_basis[2U];
  const auto horizontal_forward =
      normalized({body_forward[0U], 0.0, body_forward[2U]},
                 "hidden camera horizontal direction is degenerate");
  CollisionVector3 position{vehicle_pose.world_position[0U] -
                                horizontal_forward[0U] * tuning.distance,
                            vehicle_pose.world_position[1U] + tuning.height,
                            vehicle_pose.world_position[2U] -
                                horizontal_forward[2U] * tuning.distance};
  if (compensate_pitch) {
    position[1U] -= body_forward[1U] * 3.0;
  }
  const auto angle = tuning.angle_degrees * std::numbers::pi / 180.0;
  const CollisionVector3 direction{horizontal_forward[0U] * std::cos(angle),
                                   -std::sin(angle),
                                   horizontal_forward[2U] * std::cos(angle)};
  const auto right =
      normalized({horizontal_forward[2U], 0.0, -horizontal_forward[0U]},
                 "hidden camera right axis is degenerate");
  const auto up = normalized(cross(direction, right),
                             "hidden camera up axis is degenerate");
  const auto vertical_drop = position[1U] - vehicle_pose.world_position[1U];
  const auto sine = std::sin(angle);
  const auto target_distance =
      std::abs(sine) > 1.0e-9 && vertical_drop * sine > 0.0
          ? vertical_drop / sine
          : 10.0;
  return {position, add(position, scale(direction, target_distance)), up};
}

} // namespace

VehicleCameraPose
make_original_supercars_camera(const OriginalBodyPoseState &vehicle_pose,
                               const CameraModeTuning &tuning) {
  require_finite(vehicle_pose);
  // p3.1 RVA 0x00051395 pushes 12 then 25 into RVA 0x00043988. That owner
  // installs a fixed top-down basis and follows a point twelve units along
  // the car's horizontal forward axis. Preserve that owner and requested
  // framing, while using the finalized 40-unit playable height.
  return make_tuned_hidden_camera(vehicle_pose, tuning, false);
}

VehicleCameraPose
make_original_ignition_camera(const OriginalBodyPoseState &vehicle_pose,
                              const CameraModeTuning &tuning) {
  require_finite(vehicle_pose);
  // p3.1's alternate owner at RVA 0x000439ec supplies the camera basis,
  // pitch compensation and target-basis construction. The finalized playable
  // rear anchor is fifteen units high, twenty behind, and pitched down by
  // twenty-five degrees.
  return make_tuned_hidden_camera(vehicle_pose, tuning, true);
}

VehicleCameraPose
OriginalIgnitionCameraRuntime::step(const OriginalBodyPoseState &vehicle_pose,
                                    const CameraModeTuning &tuning) {
  auto result = make_original_ignition_camera(vehicle_pose, tuning);
  if (!retained_position_valid_) {
    retained_position_ = result.world_position;
    retained_position_valid_ = true;
    return result;
  }

  // RVA 0x000439ec first constrains the retained output translation to the
  // length of its local target vector: exactly two units from the body.
  auto relative = subtract(retained_position_, vehicle_pose.world_position);
  const auto relative_length_squared = dot(relative, relative);
  if (relative_length_squared > 1.0e-12) {
    relative = scale(relative, 2.0 / std::sqrt(relative_length_squared));
  } else {
    relative = {0.0, 0.0, -2.0};
  }
  const auto constrained = add(vehicle_pose.world_position, relative);

  // The original literal vector is {0.05, 0.9, 1.0}; these factors apply to
  // world X/Y/Z respectively and are not elapsed-time-scaled.
  retained_position_ = {
      constrained[0U] + (result.world_position[0U] - constrained[0U]) * 0.05,
      constrained[1U] + (result.world_position[1U] - constrained[1U]) * 0.9,
      constrained[2U] + (result.world_position[2U] - constrained[2U])};
  result.world_position = retained_position_;

  const auto view =
      normalized(subtract(result.world_target, result.world_position),
                 "retained Ignition camera direction is degenerate");
  const auto right =
      normalized({view[2U], 0.0, -view[0U]},
                 "retained Ignition camera horizontal axis is degenerate");
  result.world_up = normalized(
      cross(view, right), "retained Ignition camera up axis is degenerate");
  return result;
}

void OriginalIgnitionCameraRuntime::reset() noexcept {
  retained_position_valid_ = false;
  retained_position_ = {};
}

double player_engine_camera_gain(const VehicleCameraMode mode) noexcept {
  switch (mode) {
  case VehicleCameraMode::chase:
    return 1.0;
  case VehicleCameraMode::far_chase:
    return 1.0;
  case VehicleCameraMode::in_car:
    return 1.15;
  case VehicleCameraMode::bumper:
    return 1.3;
  }
  return 1.0;
}

OriginalBodyPoseState
interpolate_vehicle_pose(const OriginalBodyPoseState &previous,
                         const OriginalBodyPoseState &current,
                         const double alpha) {
  require_finite(previous);
  require_finite(current);
  if (!std::isfinite(alpha) || alpha < 0.0 || alpha > 1.0) {
    throw std::invalid_argument(
        "vehicle pose interpolation alpha must be in [0, 1]");
  }

  OriginalBodyPoseState result;
  result.world_position =
      lerp(previous.world_position, current.world_position, alpha);
  auto forward =
      normalized(lerp(previous.body_basis[2U], current.body_basis[2U], alpha),
                 "interpolated vehicle forward axis is degenerate");
  auto right_seed =
      lerp(previous.body_basis[0U], current.body_basis[0U], alpha);
  auto right =
      normalized(subtract(right_seed, scale(forward, dot(right_seed, forward))),
                 "interpolated vehicle right axis is degenerate");
  auto up = normalized(cross(forward, right),
                       "interpolated vehicle up axis is degenerate");
  right = normalized(cross(up, forward),
                     "interpolated vehicle basis is degenerate");
  result.body_basis = {right, up, forward};
  return result;
}

VehicleCameraPose make_vehicle_camera(const OriginalBodyPoseState &vehicle_pose,
                                      const VehicleCameraMode mode,
                                      const VehicleCameraConfig &config,
                                      const VehicleCameraTuning &tuning) {
  require_finite(vehicle_pose);
  require_finite(config.chase_position_local,
                 "chase camera position must be finite");
  require_finite(config.chase_target_local,
                 "chase camera target must be finite");
  require_finite(config.far_chase_position_local,
                 "far chase camera position must be finite");
  require_finite(config.far_chase_target_local,
                 "far chase camera target must be finite");
  require_finite(config.in_car_position_local,
                 "in-car camera position must be finite");
  require_finite(config.in_car_target_local,
                 "in-car camera target must be finite");
  require_finite(config.bumper_position_local,
                 "bumper camera position must be finite");
  require_finite(config.bumper_target_local,
                 "bumper camera target must be finite");
  if (const auto &selected = camera_tuning(mode, tuning);
      selected.has_value()) {
    return make_tuned_camera(vehicle_pose, mode, *selected);
  }
  const CollisionVector3 *position_local = nullptr;
  const CollisionVector3 *target_local = nullptr;
  switch (mode) {
  case VehicleCameraMode::chase:
    position_local = &config.chase_position_local;
    target_local = &config.chase_target_local;
    break;
  case VehicleCameraMode::far_chase:
    position_local = &config.far_chase_position_local;
    target_local = &config.far_chase_target_local;
    break;
  case VehicleCameraMode::in_car:
    position_local = &config.in_car_position_local;
    target_local = &config.in_car_target_local;
    break;
  case VehicleCameraMode::bumper:
    position_local = &config.bumper_position_local;
    target_local = &config.bumper_target_local;
    break;
  }
  VehicleCameraPose result;
  if (mode == VehicleCameraMode::chase ||
      mode == VehicleCameraMode::far_chase) {
    result.world_position =
        project_original_outside_position(vehicle_pose, *position_local);
    // p3.1 RVA 0x0004372c..0x00043757 adds its target vector directly to the
    // vehicle translation. The playable high-view target retains that
    // world-space ownership and the recovered pitch while matching the
    // owner-approved rear-view distance.
    result.world_target = add(vehicle_pose.world_position, *target_local);
    result.world_up = {0.0, 1.0, 0.0};
  } else {
    result.world_position =
        project_body_point_to_world(vehicle_pose, *position_local);
    result.world_target =
        project_body_point_to_world(vehicle_pose, *target_local);
    result.world_up = normalized(vehicle_pose.body_basis[1U],
                                 "vehicle camera up axis is degenerate");
  }
  const auto view = subtract(result.world_target, result.world_position);
  if (dot(view, view) <= 1.0e-12) {
    throw std::invalid_argument("vehicle camera position and target coincide");
  }
  return result;
}

VehicleCameraPose VehicleCameraRuntime::step(
    const OriginalBodyPoseState &vehicle_pose, const VehicleCameraMode mode,
    const double elapsed_seconds, const VehicleCameraConfig &config,
    const VehicleCameraTuning &tuning) {
  if (!std::isfinite(elapsed_seconds) || elapsed_seconds < 0.0) {
    throw std::invalid_argument(
        "vehicle camera elapsed seconds must be finite and non-negative");
  }

  auto result = make_vehicle_camera(vehicle_pose, mode, config, tuning);
  if (!is_outside_camera(mode)) {
    seed(result);
    return result;
  }
  if (!retained_position_valid_) {
    seed(result);
    return result;
  }

  // Keep the retained rear camera at its authored horizontal distance. The
  // original full-vector normalization also shortened its height whenever
  // the car moved forward between frames, which made uneven frame times show
  // up as high-speed vertical bobbing.
  const auto &selected_tuning = camera_tuning(mode, tuning);
  const auto desired_distance =
      selected_tuning.has_value()
          ? std::abs(selected_tuning->distance)
          : std::hypot(outside_position_local(mode, config)[0U],
                       outside_position_local(mode, config)[2U]);
  auto relative = subtract(retained_position_, vehicle_pose.world_position);
  const auto horizontal_length = std::hypot(relative[0U], relative[2U]);
  if (horizontal_length > 1.0e-6) {
    const auto scale = desired_distance / horizontal_length;
    relative[0U] *= scale;
    relative[2U] *= scale;
  }
  retained_position_ = add(vehicle_pose.world_position, relative);

  // p3.1 RVA 0x0004360d..0x0004366f derives X/Z and Y response independently:
  // sqrt(elapsed * 60) * (0.15, 0.9, 0.15), clamped component-wise to
  // [0.05, 0.9]. The factors are retained as float32 before interpolation.
  const auto elapsed_float = static_cast<float>(elapsed_seconds);
  const auto response_root =
      std::sqrt(static_cast<double>(elapsed_float) * 60.0);
  const auto horizontal_response = static_cast<double>(
      std::clamp(static_cast<float>(response_root * 0.15), 0.05F, 0.9F));
  const auto vertical_response = static_cast<double>(
      std::clamp(static_cast<float>(response_root * 0.9), 0.05F, 0.9F));
  retained_position_ = {retained_position_[0U] + (result.world_position[0U] -
                                                  retained_position_[0U]) *
                                                     horizontal_response,
                        retained_position_[1U] + (result.world_position[1U] -
                                                  retained_position_[1U]) *
                                                     vertical_response,
                        retained_position_[2U] + (result.world_position[2U] -
                                                  retained_position_[2U]) *
                                                     horizontal_response};
  result.world_position = retained_position_;
  return result;
}

void VehicleCameraRuntime::seed(const VehicleCameraPose &camera) {
  require_finite(camera.world_position,
                 "retained vehicle camera position must be finite");
  retained_position_ = camera.world_position;
  retained_position_valid_ = true;
}

void VehicleCameraRuntime::reset() noexcept {
  retained_position_valid_ = false;
  retained_position_ = {};
}

VehicleCameraPose
make_original_rear_view_camera(const OriginalBodyPoseState &vehicle_pose) {
  require_finite(vehicle_pose);
  const CollisionVector3 local_position{0.0, 2.0, 5.0};
  const auto position =
      project_body_point_to_world(vehicle_pose, local_position);
  const auto direction = scale(vehicle_pose.body_basis[2U], -1.0);
  return {position, add(position, scale(direction, 10.0)),
          vehicle_pose.body_basis[1U]};
}

VehicleCameraPose
make_original_camera_spline_camera(const mh::content::MotionData &camera_motion,
                                   const OriginalBodyPoseState &vehicle_pose) {
  require_finite(vehicle_pose);
  if (camera_motion.keyframes.empty()) {
    throw std::invalid_argument("camera spline has no authored keys");
  }

  auto nearest = std::size_t{0U};
  auto nearest_distance = std::numeric_limits<double>::infinity();
  for (std::size_t index = 0U; index < camera_motion.keyframes.size();
       ++index) {
    const auto difference = subtract(vehicle_pose.world_position,
                                     camera_motion.keyframes[index].position);
    const auto distance = dot(difference, difference);
    if (distance < nearest_distance) {
      nearest = index;
      nearest_distance = distance;
    }
  }

  const auto &key = camera_motion.keyframes[nearest];
  const auto from_key = subtract(vehicle_pose.world_position, key.position);
  const auto distance = std::sqrt(dot(from_key, from_key));
  const auto direction = distance > 1.0e-12 ? scale(from_key, 1.0 / distance)
                                            : CollisionVector3{0.0, 0.0, 1.0};
  // p3.1 rounds the signed key selector, then performs an unsigned remainder.
  const auto selector = static_cast<std::uint32_t>(
      static_cast<std::int32_t>(std::nearbyint(key.linear)));
  const auto mode = selector % 7U;

  switch (mode) {
  case 0U:
    return look_at_camera(key.position, vehicle_pose.world_position);
  case 1U: {
    const auto radial = key.rotation[0U] * distance;
    const CollisionVector3 position{
        key.position[0U] + direction[0U] * radial,
        key.position[1U] + direction[1U] * key.rotation[1U] * radial,
        key.position[2U] + direction[2U] * radial};
    return look_at_camera(position, vehicle_pose.world_position);
  }
  case 2U:
    return oriented_camera(key.position,
                           original_camera_euler_basis(key.rotation));
  case 3U: {
    const auto position = add(
        vehicle_pose.world_position,
        project_body_vector_to_world(vehicle_pose.body_basis, key.rotation));
    auto angles = original_camera_basis_euler(vehicle_pose.body_basis);
    constexpr double original_pi = 3.1415926;
    constexpr double degrees_to_radians = original_pi / 180.0;
    for (std::size_t axis = 0U; axis < angles.size(); ++axis) {
      angles[axis] += key.attributes[axis] * degrees_to_radians;
    }
    return oriented_camera(position,
                           original_camera_euler_basis_radians(angles));
  }
  case 4U:
    return look_at_camera(add(vehicle_pose.world_position, key.rotation),
                          vehicle_pose.world_position);
  case 5U: {
    const auto radial = key.rotation[0U] * distance;
    const auto angle = radial * 0.1;
    const CollisionVector3 position{
        vehicle_pose.world_position[0U] + key.rotation[1U] * std::sin(angle),
        vehicle_pose.world_position[1U] + key.rotation[2U],
        vehicle_pose.world_position[2U] + key.rotation[1U] * std::cos(angle)};
    return look_at_camera(position, vehicle_pose.world_position);
  }
  case 6U: {
    const auto radial = key.rotation[0U] * distance;
    const CollisionVector3 position{
        key.position[0U] + direction[0U] * radial,
        key.position[1U] + direction[1U] * key.rotation[1U] * radial,
        key.position[2U] + direction[2U] * radial};
    return oriented_camera(position,
                           original_camera_euler_basis(key.attributes));
  }
  }
  throw std::logic_error("camera spline mode is outside modulo-seven range");
}

} // namespace mh::game
