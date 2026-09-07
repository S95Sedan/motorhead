#include <game/physics/collision_import.hpp>

#include <content/formats/col_collision_mesh.hpp>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mh::game {
namespace {

std::size_t flatten_cell(const std::array<std::size_t, 3U> &dimensions,
                         const std::size_t x, const std::size_t y,
                         const std::size_t z) {
  return (x * dimensions[1U] + y) * dimensions[2U] + z;
}

std::uint16_t resolve_cell_descriptor(
    const mh::content::ColData &collision, const std::size_t x,
    const std::size_t y, const std::size_t z) {
  const auto depth = static_cast<std::size_t>(collision.version);
  const std::array<std::size_t, 3U> coarse{
      static_cast<std::size_t>(collision.grid_dimensions[0U]) >> depth,
      static_cast<std::size_t>(collision.grid_dimensions[1U]) >> depth,
      static_cast<std::size_t>(collision.grid_dimensions[2U]) >> depth};
  const auto root = flatten_cell(coarse, x >> depth, y >> depth, z >> depth);
  auto descriptor = collision.grid_prefix.at(root);
  auto level = depth;
  while (descriptor != 0U && (descriptor & 0x8000U) == 0U) {
    if (level != 0U) {
      --level;
    }
    const auto child = (((x >> level) & 1U) << 2U) |
                       (((y >> level) & 1U) << 1U) |
                       ((z >> level) & 1U);
    descriptor = collision.spatial_nodes.at(descriptor).children.at(child);
  }
  return descriptor;
}

std::optional<CollisionSpatialIndex>
make_spatial_index(const mh::content::ColData &collision) {
  if (collision.grid_prefix.empty()) {
    return std::nullopt;
  }
  CollisionSpatialIndex result;
  for (std::size_t axis = 0U; axis < result.dimensions.size(); ++axis) {
    result.dimensions[axis] = collision.grid_dimensions[axis];
    result.origin[axis] = static_cast<double>(collision.origin[axis]);
  }
  result.scale = static_cast<double>(collision.scale);
  const auto cell_count =
      result.dimensions[0U] * result.dimensions[1U] * result.dimensions[2U];
  result.cell_surface_indices.resize(cell_count);

  std::unordered_map<std::uint16_t, const mh::content::ColCellList *> lists;
  lists.reserve(collision.cell_lists.size());
  for (const auto &list : collision.cell_lists) {
    lists.emplace(list.dword_offset, &list);
  }
  for (std::size_t x = 0U; x < result.dimensions[0U]; ++x) {
    for (std::size_t y = 0U; y < result.dimensions[1U]; ++y) {
      for (std::size_t z = 0U; z < result.dimensions[2U]; ++z) {
        const auto descriptor = resolve_cell_descriptor(collision, x, y, z);
        if ((descriptor & 0x8000U) == 0U) {
          continue;
        }
        const auto offset = static_cast<std::uint16_t>(descriptor & 0x7fffU);
        const auto found = lists.find(offset);
        if (found == lists.end()) {
          throw std::invalid_argument(
              "COL spatial leaf does not resolve to a cell list");
        }
        auto &indices = result.cell_surface_indices.at(
            flatten_cell(result.dimensions, x, y, z));
        indices.reserve(found->second->face_indices.size());
        for (const auto face : found->second->face_indices) {
          indices.push_back(face);
        }
      }
    }
  }
  return result;
}

} // namespace

CollisionWorld make_collision_world(const mh::content::ColData &collision) {
  const auto polygons = mh::content::reconstruct_col_polygons(collision);
  std::vector<CollisionSurface> surfaces;
  surfaces.reserve(polygons.size());
  for (const auto &polygon : polygons) {
    CollisionSurface surface;
    surface.material = polygon.surface;
    surface.normal = {static_cast<double>(polygon.normal[0]),
                      static_cast<double>(polygon.normal[1]),
                      static_cast<double>(polygon.normal[2])};
    surface.vertices.reserve(polygon.vertices.size());
    for (const auto &vertex : polygon.vertices) {
      surface.vertices.push_back({static_cast<double>(vertex[0]),
                                  static_cast<double>(vertex[1]),
                                  static_cast<double>(vertex[2])});
    }
    const auto &face = collision.faces.at(polygon.face_index);
    surface.authored_plane_distance = static_cast<double>(face.distance);
    surface.authored_boundaries.reserve(face.active_boundary_count);
    for (std::size_t index = 0U; index < face.active_boundary_count; ++index) {
      const auto reference = face.boundary_references[index];
      const auto plane_index =
          static_cast<std::uint16_t>((reference - 1U) & 0x7fffU);
      const auto &plane = collision.planes.at(plane_index);
      surface.authored_boundaries.push_back(
          {{{static_cast<double>(plane.normal[0U]),
             static_cast<double>(plane.normal[1U]),
             static_cast<double>(plane.normal[2U])}},
           static_cast<double>(plane.distance),
           (reference & 0x8000U) != 0U ? -1.0 : 1.0});
    }
    surfaces.push_back(std::move(surface));
  }
  auto spatial_index = make_spatial_index(collision);
  if (spatial_index.has_value()) {
    return CollisionWorld(std::move(surfaces), std::move(*spatial_index));
  }
  return CollisionWorld(std::move(surfaces));
}

BodyHullRig make_body_hull_rig(const mh::content::ColData &collision) {
  constexpr double duplicate_epsilon_squared = 1.0e-12;
  BodyHullRig result;
  for (const auto &polygon : mh::content::reconstruct_col_polygons(collision)) {
    for (const auto &source : polygon.vertices) {
      const CollisionVector3 point{static_cast<double>(source[0U]),
                                   static_cast<double>(source[1U]),
                                   static_cast<double>(source[2U])};
      const auto duplicate = std::any_of(
          result.local_points.begin(), result.local_points.end(),
          [&point](const CollisionVector3 &existing) {
            double distance_squared = 0.0;
            for (std::size_t axis = 0U; axis < point.size(); ++axis) {
              const auto delta = point[axis] - existing[axis];
              distance_squared += delta * delta;
            }
            return distance_squared <= duplicate_epsilon_squared;
          });
      if (!duplicate) {
        result.local_points.push_back(point);
      }
    }
  }
  std::sort(result.local_points.begin(), result.local_points.end());
  return result;
}

} // namespace mh::game
