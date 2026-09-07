#include <game/physics/collision.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace mh::game {
namespace {

constexpr double vector_epsilon = collision_segment_length_squared_epsilon;
// Reconstructed COL vertices are stored as single precision and can be far from
// the origin; this matches the parser's proven polygon containment tolerance.
constexpr double plane_epsilon = 1.0e-3;

double stored_float32(const long double value) {
  return static_cast<double>(static_cast<float>(value));
}

void require_finite(const double value, const char *name) {
  if (!std::isfinite(value)) {
    throw std::invalid_argument(std::string(name) + " must be finite");
  }
}

void require_finite(const CollisionVector3 &value, const char *name) {
  for (const auto component : value) {
    require_finite(component, name);
  }
}

double dot(const CollisionVector3 &left, const CollisionVector3 &right) {
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

CollisionVector3 subtract(const CollisionVector3 &left,
                          const CollisionVector3 &right) {
  return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

CollisionVector3 cross(const CollisionVector3 &left,
                       const CollisionVector3 &right) {
  return {left[1] * right[2] - left[2] * right[1],
          left[2] * right[0] - left[0] * right[2],
          left[0] * right[1] - left[1] * right[0]};
}

CollisionVector3 normalized(const CollisionVector3 &value, const char *name) {
  require_finite(value, name);
  const auto length_squared = dot(value, value);
  if (length_squared <= vector_epsilon) {
    throw std::invalid_argument(std::string(name) + " must be nonzero");
  }
  const auto inverse_length = 1.0 / std::sqrt(length_squared);
  return {value[0] * inverse_length, value[1] * inverse_length,
          value[2] * inverse_length};
}

bool contains_point(const CollisionSurface &surface,
                    const CollisionVector3 &point) {
  if (!surface.authored_boundaries.empty()) {
    for (const auto &boundary : surface.authored_boundaries) {
      if (boundary.sign * (dot(boundary.normal, point) + boundary.distance) <
          -plane_epsilon) {
        return false;
      }
    }
    return true;
  }
  double reference_sign = 0.0;
  for (std::size_t index = 0U; index < surface.vertices.size(); ++index) {
    const auto &start = surface.vertices[index];
    const auto &end = surface.vertices[(index + 1U) % surface.vertices.size()];
    const auto side = dot(cross(subtract(end, start), subtract(point, start)),
                          surface.normal);
    if (std::fabs(side) <= plane_epsilon) {
      continue;
    }
    if (reference_sign == 0.0) {
      reference_sign = side;
    } else if ((side > 0.0) != (reference_sign > 0.0)) {
      return false;
    }
  }
  return true;
}

double original_plane_value(const CollisionVector3 &normal,
                            const double plane_distance,
                            const CollisionVector3 &point,
                            const double point_offset = 0.0) {
  auto value =
      static_cast<long double>(stored_float32(normal[0U])) *
          static_cast<long double>(stored_float32(point[0U])) +
      static_cast<long double>(stored_float32(plane_distance));
  value += static_cast<long double>(stored_float32(normal[1U])) *
           static_cast<long double>(stored_float32(point[1U]));
  value += static_cast<long double>(stored_float32(normal[2U])) *
           static_cast<long double>(stored_float32(point[2U]));
  value += static_cast<long double>(stored_float32(point_offset));
  return stored_float32(value);
}

bool contains_original_point(const CollisionSurface &surface,
                             const CollisionVector3 &point) {
  if (surface.authored_boundaries.empty()) {
    return contains_point(surface, point);
  }
  for (const auto &boundary : surface.authored_boundaries) {
    const auto value =
        original_plane_value(boundary.normal, boundary.distance, point);
    if (boundary.sign * value < -plane_epsilon) {
      return false;
    }
  }
  return true;
}

void validate_surface(CollisionSurface &surface) {
  if (surface.vertices.size() < 3U) {
    throw std::invalid_argument(
        "collision surface requires at least three vertices");
  }
  if (surface.authored_plane_distance.has_value()) {
    require_finite(*surface.authored_plane_distance,
                   "collision surface authored plane distance");
    const auto length_squared = dot(surface.normal, surface.normal);
    if (length_squared <= vector_epsilon ||
        std::fabs(length_squared - 1.0) > 1.0e-3) {
      throw std::invalid_argument(
          "authored collision surface normal is not unit length");
    }
  } else {
    surface.normal = normalized(surface.normal, "collision surface normal");
  }
  for (const auto &boundary : surface.authored_boundaries) {
    require_finite(boundary.normal, "collision surface boundary normal");
    require_finite(boundary.distance, "collision surface boundary distance");
    if (boundary.sign != -1.0 && boundary.sign != 1.0) {
      throw std::invalid_argument("collision surface boundary sign is invalid");
    }
  }
  for (const auto &vertex : surface.vertices) {
    require_finite(vertex, "collision surface vertex");
  }
  const auto plane_distance = -dot(surface.normal, surface.vertices.front());
  for (const auto &vertex : surface.vertices) {
    if (std::fabs(dot(surface.normal, vertex) + plane_distance) >
        plane_epsilon) {
      throw std::invalid_argument(
          "collision surface vertices are not coplanar");
    }
  }
  double turn_sign = 0.0;
  for (std::size_t index = 0U; index < surface.vertices.size(); ++index) {
    const auto edge =
        subtract(surface.vertices[(index + 1U) % surface.vertices.size()],
                 surface.vertices[index]);
    const auto next_edge =
        subtract(surface.vertices[(index + 2U) % surface.vertices.size()],
                 surface.vertices[(index + 1U) % surface.vertices.size()]);
    if (dot(edge, edge) <= vector_epsilon) {
      throw std::invalid_argument(
          "collision surface has duplicate consecutive vertices");
    }
    const auto turn = dot(cross(edge, next_edge), surface.normal);
    if (std::fabs(turn) <= plane_epsilon) {
      continue;
    }
    if (turn_sign == 0.0) {
      turn_sign = turn;
    } else if ((turn > 0.0) != (turn_sign > 0.0)) {
      throw std::invalid_argument("collision surface is not convex");
    }
  }
  if (turn_sign == 0.0) {
    throw std::invalid_argument("collision surface has zero area");
  }
}

std::size_t flatten_cell(const CollisionSpatialIndex &index,
                         const std::size_t x, const std::size_t y,
                         const std::size_t z) {
  return (x * index.dimensions[1U] + y) * index.dimensions[2U] + z;
}

std::vector<std::array<std::size_t, 3U>>
traverse_segment_cells(const CollisionSpatialIndex &index,
                       const CollisionVector3 &first,
                       const CollisionVector3 &second) {
  std::array<double, 3U> start{};
  std::array<double, 3U> end{};
  std::array<long long, 3U> current{};
  std::array<long long, 3U> destination{};
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    start[axis] = stored_float32(
        (static_cast<long double>(stored_float32(first[axis])) +
         static_cast<long double>(stored_float32(index.origin[axis]))) *
        static_cast<long double>(stored_float32(index.scale)));
    end[axis] = stored_float32(
        (static_cast<long double>(stored_float32(second[axis])) +
         static_cast<long double>(stored_float32(index.origin[axis]))) *
        static_cast<long double>(stored_float32(index.scale)));
    current[axis] = static_cast<long long>(std::floor(start[axis]));
    destination[axis] = static_cast<long long>(std::floor(end[axis]));
  }

  struct Crossing {
    double fraction = 0.0;
    std::size_t axis = 0U;
    long long step = 0;
    std::size_t insertion_order = 0U;
  };
  std::vector<Crossing> crossings;
  auto insertion_order = std::size_t{0U};
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    const auto delta = end[axis] - start[axis];
    if (delta > 0.0) {
      for (auto boundary = current[axis] + 1; boundary <= destination[axis];
           ++boundary) {
        crossings.push_back(
            {(static_cast<double>(boundary) - start[axis]) / delta, axis, 1,
             insertion_order++});
      }
    } else if (delta < 0.0) {
      for (auto boundary = current[axis]; boundary > destination[axis];
           --boundary) {
        crossings.push_back(
            {(static_cast<double>(boundary) - start[axis]) / delta, axis, -1,
             insertion_order++});
      }
    }
  }
  std::stable_sort(crossings.begin(), crossings.end(),
                   [](const Crossing &left, const Crossing &right) {
                     return left.fraction < right.fraction;
                   });

  std::vector<std::array<std::size_t, 3U>> result;
  const auto append_current = [&]() {
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
      if (current[axis] < 0 ||
          current[axis] >= static_cast<long long>(index.dimensions[axis])) {
        return;
      }
    }
    result.push_back({static_cast<std::size_t>(current[0U]),
                      static_cast<std::size_t>(current[1U]),
                      static_cast<std::size_t>(current[2U])});
  };
  append_current();
  for (const auto &crossing : crossings) {
    current[crossing.axis] += crossing.step;
    append_current();
  }
  return result;
}

} // namespace

CollisionWorld::CollisionWorld(std::vector<CollisionSurface> surfaces)
    : surfaces_(std::move(surfaces)) {
  for (auto &surface : surfaces_) {
    validate_surface(surface);
  }
}

CollisionWorld::CollisionWorld(std::vector<CollisionSurface> surfaces,
                               CollisionSpatialIndex spatial_index)
    : CollisionWorld(std::move(surfaces)) {
  require_finite(spatial_index.origin, "collision spatial origin");
  require_finite(spatial_index.scale, "collision spatial scale");
  if (spatial_index.scale <= 0.0 ||
      std::any_of(spatial_index.dimensions.begin(),
                  spatial_index.dimensions.end(),
                  [](const std::size_t value) { return value == 0U; })) {
    throw std::invalid_argument("collision spatial index is invalid");
  }
  const auto expected_cells = spatial_index.dimensions[0U] *
                              spatial_index.dimensions[1U] *
                              spatial_index.dimensions[2U];
  if (spatial_index.cell_surface_indices.size() != expected_cells) {
    throw std::invalid_argument("collision spatial cell count is invalid");
  }
  for (const auto &cell : spatial_index.cell_surface_indices) {
    if (std::any_of(cell.begin(), cell.end(), [this](const std::size_t index) {
          return index >= surfaces_.size();
        })) {
      throw std::invalid_argument(
          "collision spatial cell references an invalid surface");
    }
  }
  spatial_index_ = std::move(spatial_index);
}

const std::vector<CollisionSurface> &CollisionWorld::surfaces() const noexcept {
  return surfaces_;
}

std::optional<CollisionHit>
CollisionWorld::raycast(const CollisionVector3 &origin,
                        const CollisionVector3 &direction,
                        const double maximum_distance) const {
  require_finite(origin, "collision ray origin");
  require_finite(maximum_distance, "collision ray maximum distance");
  if (maximum_distance < 0.0) {
    throw std::invalid_argument(
        "collision ray maximum distance cannot be negative");
  }
  const auto ray_direction = normalized(direction, "collision ray direction");

  std::optional<CollisionHit> nearest;
  auto nearest_distance = maximum_distance;
  for (std::size_t index = 0U; index < surfaces_.size(); ++index) {
    const auto &surface = surfaces_[index];
    const auto denominator = dot(surface.normal, ray_direction);
    if (std::fabs(denominator) <= vector_epsilon) {
      continue;
    }
    const auto plane_distance = -dot(surface.normal, surface.vertices.front());
    const auto distance =
        -(dot(surface.normal, origin) + plane_distance) / denominator;
    if (distance < 0.0 || distance > maximum_distance ||
        (nearest.has_value() && distance >= nearest_distance)) {
      continue;
    }
    const CollisionVector3 point{origin[0] + ray_direction[0] * distance,
                                 origin[1] + ray_direction[1] * distance,
                                 origin[2] + ray_direction[2] * distance};
    if (!contains_point(surface, point)) {
      continue;
    }
    nearest_distance = distance;
    nearest =
        CollisionHit{distance, point, surface.normal, surface.material, index};
  }
  return nearest;
}

std::optional<CollisionHit>
CollisionWorld::original_segment_cast(const CollisionVector3 &first,
                                      const CollisionVector3 &second) const {
  constexpr double endpoint_epsilon = 0.08;
  require_finite(first, "collision segment first endpoint");
  require_finite(second, "collision segment second endpoint");
  const auto delta = subtract(second, first);
  const auto length_squared = dot(delta, delta);
  if (length_squared <= vector_epsilon) {
    throw std::invalid_argument("collision segment endpoints must be distinct");
  }
  const auto length = std::sqrt(length_squared);
  const auto stored_length = stored_float32(std::sqrt(
      static_cast<long double>(stored_float32(delta[1U])) *
          static_cast<long double>(stored_float32(delta[1U])) +
      static_cast<long double>(stored_float32(delta[0U])) *
          static_cast<long double>(stored_float32(delta[0U])) +
      static_cast<long double>(stored_float32(delta[2U])) *
          static_cast<long double>(stored_float32(delta[2U]))));

  std::optional<CollisionHit> nearest;
  auto nearest_fraction = stored_float32(10000.0);
  std::vector<std::size_t> candidates;
  if (spatial_index_.has_value()) {
    const auto cells = traverse_segment_cells(*spatial_index_, first, second);
    for (const auto &cell : cells) {
      const auto &indices = spatial_index_->cell_surface_indices.at(
          flatten_cell(*spatial_index_, cell[0U], cell[1U], cell[2U]));
      candidates.insert(candidates.end(), indices.begin(), indices.end());
    }
  } else {
    candidates.resize(surfaces_.size());
    for (std::size_t index = 0U; index < candidates.size(); ++index) {
      candidates[index] = index;
    }
  }
  for (const auto index : candidates) {
    const auto &surface = surfaces_[index];
    const auto plane_distance =
        surface.authored_plane_distance.has_value()
            ? *surface.authored_plane_distance
            : -dot(surface.normal, surface.vertices.front());
    auto second_distance =
        original_plane_value(surface.normal, plane_distance, second);
    if (second_distance >=
        stored_length + stored_float32(endpoint_epsilon)) {
      continue;
    }
    auto point_offset = 0.0;
    if (second_distance <= 0.0 &&
        second_distance > -stored_float32(endpoint_epsilon)) {
      point_offset = stored_float32(endpoint_epsilon);
      second_distance =
          stored_float32(second_distance + point_offset);
    }
    if (second_distance <= 0.0) {
      continue;
    }
    auto first_distance = original_plane_value(
        surface.normal, plane_distance, first, point_offset);
    if (first_distance > 0.0 &&
        first_distance < stored_float32(endpoint_epsilon)) {
      first_distance = stored_float32(
          first_distance - stored_float32(endpoint_epsilon));
    }
    if (first_distance > 0.0) {
      continue;
    }
    const auto fraction = stored_float32(
        static_cast<long double>(second_distance) /
        (static_cast<long double>(second_distance) -
         static_cast<long double>(first_distance)));
    if (fraction >= nearest_fraction) {
      continue;
    }
    CollisionVector3 point{};
    for (std::size_t axis = 0U; axis < point.size(); ++axis) {
      const auto difference = stored_float32(
          static_cast<long double>(stored_float32(first[axis])) -
          static_cast<long double>(stored_float32(second[axis])));
      const auto scaled_difference = stored_float32(
          static_cast<long double>(fraction) *
          static_cast<long double>(difference));
      point[axis] = stored_float32(
          static_cast<long double>(stored_float32(second[axis])) +
          static_cast<long double>(scaled_difference) +
          static_cast<long double>(stored_float32(surface.normal[axis])) *
              static_cast<long double>(point_offset));
    }
    if (!contains_original_point(surface, point)) {
      continue;
    }
    nearest_fraction = fraction;
    nearest = CollisionHit{length * (1.0 - fraction), point, surface.normal,
                           surface.material, index};
  }
  return nearest;
}

std::optional<CollisionHit> CollisionWorld::original_retained_segment_cast(
    const CollisionVector3 &first, const CollisionVector3 &second,
    const CollisionHit &retained) const {
  constexpr double endpoint_epsilon = 0.08;
  require_finite(first, "retained collision segment first endpoint");
  require_finite(second, "retained collision segment second endpoint");
  require_finite(retained.normal, "retained collision normal");
  if (retained.surface_index >= surfaces_.size()) {
    throw std::invalid_argument(
        "retained collision surface index is outside the world");
  }
  const auto delta = subtract(second, first);
  const auto length_squared = dot(delta, delta);
  if (length_squared <= vector_epsilon) {
    throw std::invalid_argument(
        "retained collision segment endpoints must be distinct");
  }
  const auto length = std::sqrt(length_squared);
  const auto &surface = surfaces_[retained.surface_index];
  const auto plane_distance =
      surface.authored_plane_distance.has_value()
          ? *surface.authored_plane_distance
          : -dot(retained.normal, surface.vertices.front());

  auto second_distance = dot(retained.normal, second) + plane_distance;
  auto point_offset = 0.0;
  if (second_distance <= 0.0 && second_distance > -endpoint_epsilon) {
    point_offset = endpoint_epsilon;
    second_distance += endpoint_epsilon;
  }
  if (second_distance <= 0.0) {
    return std::nullopt;
  }
  auto first_distance =
      dot(retained.normal, first) + plane_distance + point_offset;
  if (first_distance > 0.0 && first_distance < endpoint_epsilon) {
    first_distance -= endpoint_epsilon;
  }
  if (first_distance > 0.0) {
    return std::nullopt;
  }
  const auto fraction = second_distance / (second_distance - first_distance);
  return CollisionHit{length * (1.0 - fraction), retained.point,
                      retained.normal, retained.material,
                      retained.surface_index};
}

} // namespace mh::game
