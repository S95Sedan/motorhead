#pragma once

#include <game/physics/dynamic_vehicle_contact.hpp>
#include <game/physics/vehicle_response.hpp>
#include <game/vehicle/tuning.hpp>

#include <filesystem>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace mh::content {
struct CarDefinition;
struct ColData;
struct MyoData;
struct MotionSample;
} // namespace mh::content

namespace mh::game {

struct OriginalEnvironmentLoopSound {
  std::string wave_file;
  std::int32_t priority = 0;
  float volume = 0.0F;
  float pitch = 1.0F;
  float minimum_distance = 0.0F;
  float maximum_distance = 0.0F;
  std::array<float, 3U> position{};
};

struct OriginalObjectEnvironmentLoopSound {
  std::int32_t object_number = 0;
  std::string wave_file;
  std::int32_t priority = 0;
  float volume = 0.0F;
  float pitch = 1.0F;
  float minimum_distance = 0.0F;
  float maximum_distance = 0.0F;
};

struct OriginalObjectCollisionSound {
  std::int32_t object_number = 0;
  std::string wave_file;
  std::int32_t priority = 0;
  float volume = 0.0F;
  float pitch = 1.0F;
};

struct OriginalEnvironmentSoundTable {
  std::vector<OriginalEnvironmentLoopSound> fixed_loops;
  std::vector<OriginalObjectEnvironmentLoopSound> object_loops;
  std::vector<OriginalObjectCollisionSound> object_collisions;
};

struct OriginalEnvironmentScenePositionKey {
  std::array<double, 3U> position{};
  std::array<double, 3U> rotation{};
  std::array<double, 3U> attributes{};
  std::int32_t frame = 0;
  std::int32_t linear = 0;
  double tension = 0.0;
  double continuity = 0.0;
  double bias = 0.0;
};

struct OriginalEnvironmentSceneObject {
  std::string source_object;
  std::vector<OriginalEnvironmentScenePositionKey> keys;
  std::optional<std::uint16_t> environment_identifier;
  std::optional<std::uint16_t> collision_identifier;
  // LightWave PolygonSize is the authored particle/body size consumed by the
  // p3.1 collision-body initializer. When absent or zero, retail derives the
  // mass from the MYO bounding radius instead.
  std::optional<double> polygon_size;
  bool unseen_by_rays = false;
  std::int32_t end_behavior = 1;
};

struct OriginalEnvironmentScene {
  double frames_per_second = 30.0;
  std::vector<OriginalEnvironmentSceneObject> objects;
};

// Parses the three record forms declared by every shipping ESounds.esd. The
// p3.1 token comparison is ASCII case-insensitive; unknown records are ignored
// by the original owner and remain ignored here.
[[nodiscard]] OriginalEnvironmentSoundTable
parse_original_environment_sounds(std::string_view text);

[[nodiscard]] OriginalEnvironmentSoundTable
read_original_environment_sounds(const std::filesystem::path &path);

// The environmental object identifier is not MYW or LWS array order. The
// p3.1 stores the first two authored ObjEdgeColor integers at object +0xcc
// and +0xce. ObjectEnvSound searches the first; the loader copies the second
// into the object's collision body for ObjectColSound dispatch.
[[nodiscard]] OriginalEnvironmentScene
read_original_environment_scene(const std::filesystem::path &path);

[[nodiscard]] std::array<double, 3U>
sample_original_environment_scene_position(
    const OriginalEnvironmentSceneObject &object, double authored_frame);

[[nodiscard]] mh::content::MotionSample
sample_original_environment_scene_motion(
    const OriginalEnvironmentSceneObject &object, double authored_frame);

// Attribute-only keys animate presentation state (for example Atlantika's
// blinking cone lamps) without making the prop a kinematic path object.
// Position or rotation changes retain authored transform ownership.
[[nodiscard]] bool original_environment_scene_has_transform_motion(
    const OriginalEnvironmentSceneObject &object) noexcept;

// Reproduces the LWS object's collision transform built at
// 0x000a03c6..0x000a0442: LightWave H/P/B degrees become the p3.1 matrix in
// P/H/B argument order, while translation is stored at matrix +0x24.
[[nodiscard]] OriginalBodyPoseState make_original_environment_collision_pose(
    const OriginalEnvironmentSceneObject &object, double authored_frame);

// Builds the same movable-scene convex body initialized by the p3.1 LWS
// loader: eight MYO AABB corners, the MYO-derived bounding radius, all COL
// face planes, and either PolygonSize*100 or the retail sphere fallback mass.
// UnseenByRays ownership and source-path resolution remain with the scene
// owner; this function is deliberately asset/name agnostic.
[[nodiscard]] OriginalDynamicVehicleContactShape
make_original_environment_collision_shape(
    const mh::content::MyoData &model,
    const mh::content::ColData &collision,
    std::optional<double> polygon_size = std::nullopt);

// The caller supplies a query bound explicitly. The recovered spring values are
// retained on the rig but are not yet interpreted as a ray length or force law.
[[nodiscard]] WheelProbeRig
make_wheel_probe_rig(const mh::content::CarDefinition &car,
                     double query_distance);

[[nodiscard]] OriginalVehicleResponseConfig
make_original_vehicle_response_config(
    const mh::content::CarDefinition &car, double query_distance,
    OriginalWheelResponseProfile profile =
        OriginalWheelResponseProfile::standard);

// Resolves the global Material.mat definitions through a track's local SRF
// ordering. Collision face material N indexes the returned slot N directly.
[[nodiscard]] OriginalVehicleGroundedMaterialTable
make_original_track_grounded_material_table(
    const std::filesystem::path &material_definition,
    const std::filesystem::path &track_surface_definition);

// Reproduces the primary initializer's body+0x284 count of four and direct
// body+0x288 points from authored PhyCorner X/Z in FL, FR, BR, BL order;
// every local Y is replaced with authored PhyCenterMass Y.
[[nodiscard]] BodyHullRig
make_original_vehicle_body_hull_rig(const mh::content::CarDefinition &car);

// Uses the same initialized body+0x288 point array as static hull reaction:
// authored PhyCorner X/Z with every Y replaced by PhyCenterMass Y. The car COL
// supplies the reciprocal convex planes used by dynamic-body candidates.
[[nodiscard]] OriginalDynamicVehicleContactShape
make_original_dynamic_vehicle_contact_shape(
    const mh::content::CarDefinition &car,
    const mh::content::ColData &collision);

// Promotes the authored CAR physics and performance levels into the recovered
// runtime initializer without exposing proprietary content types to mh_game.
[[nodiscard]] RecoveredVehicleRuntimeTuning
make_recovered_vehicle_runtime_tuning(const mh::content::CarDefinition &car);

} // namespace mh::game
