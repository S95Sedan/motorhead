#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

namespace mh::render {
// Renderer-only acceleration; retail physics collision ordering is unchanged.
class LightingBvh {
public:
  using Point = std::array<double, 3U>;
  struct Triangle { Point a, b, c; std::size_t source = 0U, fan = 1U; };
  struct Hit { double distance, u, v; std::size_t source, fan; };
  explicit LightingBvh(std::vector<Triangle> triangles) : triangles_(std::move(triangles)) {
    indices_.resize(triangles_.size());
    std::iota(indices_.begin(), indices_.end(), std::size_t{0U});
    if (!indices_.empty()) build(0U, indices_.size());
  }
  [[nodiscard]] std::optional<Hit> trace(const Point &origin, const Point &direction,
                                        double maximum, double minimum = 0.0) const {
    if (nodes_.empty()) return std::nullopt;
    std::optional<Hit> hit;
    std::array<std::size_t, 128U> stack{};
    std::size_t size = 1U;
    while (size != 0U) {
      const auto &node = nodes_[stack[--size]];
      double near = minimum, far = maximum;
      bool intersects = true;
      for (std::size_t axis = 0U; axis < 3U; ++axis) {
        if (std::abs(direction[axis]) < 1e-12) {
          if (origin[axis] < node.low[axis] || origin[axis] > node.high[axis]) intersects = false;
        } else {
          const double a = (node.low[axis] - origin[axis]) / direction[axis];
          const double b = (node.high[axis] - origin[axis]) / direction[axis];
          near = std::max(near, std::min(a, b));
          far = std::min(far, std::max(a, b));
        }
      }
      if (!intersects || near > far) continue;
      if (node.count == 0U) {
        // Median splits bound depth by log2(triangle count); 128 entries exceed
        // the maximum representable size_t tree depth.
        stack[size++] = node.left;
        stack[size++] = node.right;
        continue;
      }
      for (std::size_t i = node.first; i < node.first + node.count; ++i) {
        const auto &triangle = triangles_[indices_[i]];
        const auto e0 = subtract(triangle.b, triangle.a), e1 = subtract(triangle.c, triangle.a);
        const auto q = cross(direction, e1);
        const double determinant = dot(e0, q);
        if (std::abs(determinant) < 1e-9) continue;
        const auto t = subtract(origin, triangle.a);
        const double u = dot(t, q) / determinant;
        const auto r = cross(t, e0);
        const double v = dot(direction, r) / determinant;
        const double distance = dot(e1, r) / determinant;
        if (u < 0.0 || v < 0.0 || u + v > 1.0 || distance < minimum || distance > maximum) continue;
        if (hit && distance == maximum && triangle.source >= hit->source) continue;
        maximum = distance;
        hit = Hit{distance, u, v, triangle.source, triangle.fan};
      }
    }
    return hit;
  }
private:
  struct Node { Point low{}, high{}; std::size_t first=0U, count=0U, left=0U, right=0U; };
  static Point subtract(const Point &a, const Point &b) { return {a[0]-b[0],a[1]-b[1],a[2]-b[2]}; }
  static Point cross(const Point &a, const Point &b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
  static double dot(const Point &a, const Point &b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
  std::size_t build(std::size_t first, std::size_t count) {
    const auto index = nodes_.size();
    Node node;
    node.low.fill(std::numeric_limits<double>::infinity());
    node.high.fill(-std::numeric_limits<double>::infinity());
    for (std::size_t i=first; i<first+count; ++i) {
      const auto &t = triangles_[indices_[i]];
      for (std::size_t c=0U; c<3U; ++c) {
        node.low[c] = std::min({node.low[c],t.a[c],t.b[c],t.c[c]});
        node.high[c] = std::max({node.high[c],t.a[c],t.b[c],t.c[c]});
      }
    }
    nodes_.push_back(node);
    if (count <= 8U) { nodes_[index].first=first; nodes_[index].count=count; return index; }
    std::size_t axis = 0U;
    for (std::size_t c=1U; c<3U; ++c)
      if (node.high[c]-node.low[c] > node.high[axis]-node.low[axis]) axis=c;
    const auto middle = first + count/2U;
    std::nth_element(indices_.begin()+first,indices_.begin()+middle,indices_.begin()+first+count,
        [&](std::size_t a, std::size_t b) {
          const auto &x=triangles_[a], &y=triangles_[b];
          return x.a[axis]+x.b[axis]+x.c[axis] < y.a[axis]+y.b[axis]+y.c[axis];
        });
    const auto left=build(first,count/2U), right=build(middle,count-count/2U);
    nodes_[index].left=left; nodes_[index].right=right;
    return index;
  }
  std::vector<Triangle> triangles_;
  std::vector<std::size_t> indices_;
  std::vector<Node> nodes_;
};
} // namespace mh::render
