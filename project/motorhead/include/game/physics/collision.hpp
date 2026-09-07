#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace mh::game {

using CollisionVector3 = std::array<double, 3U>;

inline constexpr double collision_segment_length_squared_epsilon = 1.0e-12;

struct BodyHullRig {
  std::vector<CollisionVector3> local_points;
};

struct CollisionSurface {
  CollisionSurface() = default;
  CollisionSurface(const std::uint16_t source_material,
                   CollisionVector3 source_normal,
                   std::vector<CollisionVector3> source_vertices)
      : material(source_material), normal(source_normal),
        vertices(std::move(source_vertices)) {}

  std::uint16_t material = 0U;
  CollisionVector3 normal{};
  std::vector<CollisionVector3> vertices;
  // COL-backed surfaces retain their authored plane and signed convex
  // boundaries. Synthetic/shipping-neutral surfaces may omit these and use
  // the reconstructed polygon plane instead.
  std::optional<double> authored_plane_distance;
  struct Boundary {
    CollisionVector3 normal{};
    double distance = 0.0;
    double sign = 1.0;
  };
  std::vector<Boundary> authored_boundaries;
};

struct CollisionSpatialIndex {
  std::array<std::size_t, 3U> dimensions{};
  CollisionVector3 origin{};
  double scale = 0.0;
  // X-major, then Y, then Z. Each finest cell preserves the retail COL leaf
  // face order; empty cells contain no indices.
  std::vector<std::vector<std::size_t>> cell_surface_indices;
};

struct CollisionHit {
  double distance = 0.0;
  CollisionVector3 point{};
  CollisionVector3 normal{};
  std::uint16_t material = 0U;
  std::size_t surface_index = 0U;
};

class CollisionWorld {
public:
  explicit CollisionWorld(std::vector<CollisionSurface> surfaces);
  CollisionWorld(std::vector<CollisionSurface> surfaces,
                 CollisionSpatialIndex spatial_index);

  [[nodiscard]] const std::vector<CollisionSurface> &surfaces() const noexcept;
  [[nodiscard]] std::optional<CollisionHit>
  raycast(const CollisionVector3 &origin, const CollisionVector3 &direction,
          double maximum_distance) const;
  [[nodiscard]] std::optional<CollisionHit>
  original_segment_cast(const CollisionVector3 &first,
                        const CollisionVector3 &second) const;
  [[nodiscard]] std::optional<CollisionHit>
  original_retained_segment_cast(const CollisionVector3 &first,
                                 const CollisionVector3 &second,
                                 const CollisionHit &retained) const;

private:
  std::vector<CollisionSurface> surfaces_;
  std::optional<CollisionSpatialIndex> spatial_index_;
};

} // namespace mh::game
