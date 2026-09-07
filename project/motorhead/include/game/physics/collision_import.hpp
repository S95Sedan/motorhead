#pragma once

#include <game/physics/collision.hpp>

namespace mh::content {
struct ColData;
}

namespace mh::game {

// Import boundary kept in a separate library so the deterministic game core
// does not depend on filesystem readers or proprietary-format parsing.
[[nodiscard]] CollisionWorld
make_collision_world(const mh::content::ColData &collision);

// Recovers the unique local vertices consumed by the original body-hull point
// query. Kept in the content adapter so the game core remains format-neutral.
[[nodiscard]] BodyHullRig
make_body_hull_rig(const mh::content::ColData &collision);

} // namespace mh::game
