#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mh::content {

struct MywSection {
  std::uint32_t count = 0U;
  std::uint32_t byte_size = 0U;
  std::uint32_t offset = 0U;
};

struct MywObject {
  std::array<float, 4U> bounding_sphere{};
  std::uint32_t first_position = 0U;
  std::uint32_t position_count = 0U;
  std::uint32_t first_plane = 0U;
  std::uint32_t plane_count = 0U;
  std::uint32_t first_normal = 0U;
  std::uint32_t normal_count = 0U;
  std::uint32_t primitive_offset = 0U;
  std::uint32_t first_primitive = 0U;
  std::uint32_t primitive_count = 0U;
  std::uint32_t texture_coordinate_offset = 0U;
  std::uint32_t texture_coordinate_bytes = 0U;
};

struct MywPrimitive {
  std::uint32_t object_index = 0U;
  std::uint8_t primitive_type = 0U;
  std::uint8_t flags = 0U;
  std::uint8_t vertex_count = 0U;
  // Software primitives carry one packed RGBA value at +4. Accelerated
  // primitives carry one value per vertex from the same offset. The parser
  // expands the flat form so renderers can consume one uniform interface.
  std::array<std::array<std::uint8_t, 4U>, 4U> vertex_colors{};
  std::array<std::uint32_t, 4U> position_indices{};
  std::uint32_t plane_index = 0U;
  std::uint32_t normal_index = 0U;
  bool has_texture_coordinates = false;
  std::array<std::array<float, 2U>, 4U> texture_coordinates{};
  std::uint32_t material_record_index =
      std::numeric_limits<std::uint32_t>::max();
  std::uint32_t material_name_index = std::numeric_limits<std::uint32_t>::max();
};

struct MywGridCell {
  std::uint32_t object_index = std::numeric_limits<std::uint32_t>::max();
  std::vector<std::uint32_t> light_indices;
  std::uint32_t first_auxiliary_record =
      std::numeric_limits<std::uint32_t>::max();
  std::uint32_t auxiliary_record_count = 0U;
};

struct MywPointLight {
  std::uint8_t type = 0U;
  std::array<float, 3U> position{};
  std::array<float, 3U> direction{};
  std::array<float, 4U> color{};
  float range = 0.0F;
  std::array<float, 3U> coefficients{};
};

struct MywLensFlare {
  std::uint32_t type = 0U;
  std::uint32_t image_index = 0U;
  std::array<float, 3U> position{};
  std::array<float, 3U> secondary_position{};
  std::uint32_t packed_color = 0U;
  std::uint32_t flags = 0U;
};

struct MywPointLighting {
  std::array<float, 3U> contribution{};
  std::uint32_t contributing_lights = 0U;
};

struct MywData {
  std::uint64_t file_bytes = 0U;
  std::uint32_t grid_width = 0U;
  std::uint32_t grid_height = 0U;
  std::uint32_t grid_offset = 0U;
  double grid_cell_size_x = 0.0;
  double grid_cell_size_z = 0.0;
  std::array<float, 3U> minimum{};
  std::array<float, 3U> maximum{};
  // The shipping MYW header carries the two scene-light records consumed by
  // p3.1's world/object render setup.  The first record is the world ambient
  // source.  The second is the directional source used by smooth objects,
  // including vehicles.
  std::array<std::uint8_t, 3U> ambient_color{};
  double ambient_intensity = 0.0;
  std::array<std::uint8_t, 3U> directional_color{};
  float directional_intensity = 0.0F;
  std::array<float, 3U> directional_direction{};
  std::array<MywSection, 12U> sections{};
  std::uint64_t occupied_grid_cells = 0U;
  std::uint64_t grid_link_count = 0U;
  std::uint64_t grid_auxiliary_record_count = 0U;
  std::array<std::uint64_t, 10U> primitive_type_counts{};
  std::uint64_t triangle_count = 0U;
  std::uint64_t quad_count = 0U;
  std::uint64_t vertex_reference_count = 0U;
  std::uint64_t material_primitive_count = 0U;
  std::uint64_t texture_coordinate_count = 0U;
  std::uint64_t unsupported_primitive_count = 0U;
  std::vector<std::array<float, 3U>> positions;
  std::vector<std::array<float, 4U>> planes;
  std::vector<std::array<float, 3U>> normals;
  std::vector<MywObject> objects;
  std::vector<MywPrimitive> primitives;
  std::vector<MywPointLight> lights;
  std::vector<MywLensFlare> lens_flares;
  std::vector<MywGridCell> grid_cells;
  std::vector<std::uint32_t> grid_object_indices;
  std::vector<std::string> material_names;
};

struct MywGlbExport {
  std::uint64_t triangle_count = 0U;
  std::uint64_t vertex_count = 0U;
  std::uint64_t material_count = 0U;
  std::uint64_t file_bytes = 0U;
};

struct MywGlbTexture {
  std::string material_name;
  std::string runtime_group_logical_id;
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::vector<std::uint8_t> rgba;
};

[[nodiscard]] MywData parse_myw(std::span<const std::uint8_t> bytes);
[[nodiscard]] MywData
read_myw(const std::filesystem::path &path,
         std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);
[[nodiscard]] std::optional<std::size_t>
myw_grid_cell_index(const MywData &world, double x, double z);
[[nodiscard]] std::optional<std::uint32_t>
myw_grid_object_index_at(const MywData &world, double x, double z);
[[nodiscard]] MywPointLighting
evaluate_original_myw_point_lighting(const MywData &world,
                                     const std::array<float, 3U> &position);
[[nodiscard]] MywPointLighting evaluate_original_myw_object_point_lighting(
    const MywData &world, const std::array<float, 3U> &object_position,
    const std::array<float, 3U> &normal,
    const std::array<float, 3U> &object_view_direction, float specular_factor,
    float light_intensity);
[[nodiscard]] std::array<float, 3U>
evaluate_original_myw_directional_lighting(
    const MywData &world, const std::array<float, 3U> &normal);
[[nodiscard]] std::array<float, 3U> evaluate_original_myw_base_lighting(
    const std::array<std::uint8_t, 3U> &top_color, float light_intensity);
void write_myw_obj(const MywData &world, const std::filesystem::path &output,
                   bool overwrite);
[[nodiscard]] MywGlbExport write_myw_glb(const MywData &world,
                                         const std::filesystem::path &output,
                                         bool overwrite);
[[nodiscard]] MywGlbExport
write_myw_glb(const MywData &world, std::span<const MywGlbTexture> textures,
              const std::filesystem::path &output, bool overwrite);

} // namespace mh::content
