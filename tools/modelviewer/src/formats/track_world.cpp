#include <formats/track_world.hpp>

#include <core/error.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <string_view>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::uint32_t signature = 0x4d595721U;
constexpr std::size_t header_bytes = 240U;
constexpr std::array<std::uint32_t, 10U> primitive_record_sizes{
    16U, 20U, 24U, 24U, 24U, 32U, 32U, 36U, 20U, 24U};

std::uint16_t u16(std::span<const std::uint8_t> bytes,
                  const std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < 2U) {
    throw ToolError(ExitCode::format, "MYW field exceeds file bounds");
  }
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(
             static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t u32(std::span<const std::uint8_t> bytes,
                  const std::size_t offset) {
  if (offset > bytes.size() || bytes.size() - offset < 4U) {
    throw ToolError(ExitCode::format, "MYW field exceeds file bounds");
  }
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t u64(std::span<const std::uint8_t> bytes,
                  const std::size_t offset) {
  return static_cast<std::uint64_t>(u32(bytes, offset)) |
         (static_cast<std::uint64_t>(u32(bytes, offset + 4U)) << 32U);
}

float f32(std::span<const std::uint8_t> bytes, const std::size_t offset) {
  return std::bit_cast<float>(u32(bytes, offset));
}

double f64(std::span<const std::uint8_t> bytes, const std::size_t offset) {
  return std::bit_cast<double>(u64(bytes, offset));
}

template <std::size_t Count>
std::array<float, Count> float_record(const std::span<const std::uint8_t> bytes,
                                      const std::size_t offset,
                                      const char *const diagnostic) {
  std::array<float, Count> result{};
  for (std::size_t index = 0U; index < Count; ++index) {
    result[index] = f32(bytes, offset + index * 4U);
    if (!std::isfinite(result[index])) {
      throw ToolError(ExitCode::format, diagnostic);
    }
  }
  return result;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path,
                                     const std::uint64_t maximum_file_bytes) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size > maximum_file_bytes ||
      size >
          static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw ToolError(ExitCode::input,
                    "MYW file is missing or exceeds the size limit");
  }
  std::ifstream input(path, std::ios::binary);
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!input && !bytes.empty()) {
    throw ToolError(ExitCode::input, "failed to read MYW file");
  }
  return bytes;
}

} // namespace

MywData parse_myw(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < header_bytes || u32(bytes, 0U) != signature) {
    throw ToolError(ExitCode::format, "MYW signature or header is invalid");
  }
  if (u32(bytes, 4U) != bytes.size()) {
    throw ToolError(ExitCode::format,
                    "MYW declared file size does not match input");
  }
  MywData result;
  result.file_bytes = bytes.size();
  result.grid_width = u32(bytes, 8U);
  result.grid_height = u32(bytes, 12U);
  result.grid_cell_size_x = f64(bytes, 0x10U);
  result.grid_cell_size_z = f64(bytes, 0x18U);
  result.grid_offset = u32(bytes, 0x58U);
  if (result.grid_width == 0U || result.grid_height == 0U ||
      !std::isfinite(result.grid_cell_size_x) ||
      !std::isfinite(result.grid_cell_size_z) ||
      result.grid_cell_size_x <= 0.0 || result.grid_cell_size_z <= 0.0) {
    throw ToolError(ExitCode::format,
                    "MYW grid dimensions or cell sizes are invalid");
  }
  for (std::size_t axis = 0U; axis < 3U; ++axis) {
    result.minimum[axis] = f32(bytes, 0x20U + axis * 4U);
    result.maximum[axis] = f32(bytes, 0x2cU + axis * 4U);
    if (!std::isfinite(result.minimum[axis]) ||
        !std::isfinite(result.maximum[axis]) ||
        result.minimum[axis] > result.maximum[axis]) {
      throw ToolError(ExitCode::format, "MYW world bounds are invalid");
    }
  }
  std::copy_n(bytes.begin() + 0x38U, result.ambient_color.size(),
              result.ambient_color.begin());
  result.ambient_intensity = f64(bytes, 0x3cU);
  std::copy_n(bytes.begin() + 0x44U, result.directional_color.size(),
              result.directional_color.begin());
  result.directional_intensity = f32(bytes, 0x48U);
  result.directional_direction = float_record<3U>(
      bytes, 0x4cU, "MYW directional-light direction is not finite");
  if (!std::isfinite(result.ambient_intensity) ||
      result.ambient_intensity < 0.0 ||
      !std::isfinite(result.directional_intensity) ||
      result.directional_intensity < 0.0F) {
    throw ToolError(ExitCode::format,
                    "MYW scene-light intensity is invalid");
  }
  constexpr std::array<std::uint32_t, 12U> fixed_strides{
      228U, 12U, 24U, 0U, 16U, 12U, 12U, 0U, 196U, 8U, 0U, 120U};
  struct Interval {
    std::uint64_t start;
    std::uint64_t end;
  };
  std::vector<Interval> intervals;
  intervals.push_back(Interval{0U, header_bytes});
  for (std::size_t index = 0U; index < result.sections.size(); ++index) {
    const auto base = 0x5cU + index * 12U;
    auto &section = result.sections[index];
    section.count = u32(bytes, base);
    section.byte_size = u32(bytes, base + 4U);
    section.offset = u32(bytes, base + 8U);
    const auto end =
        static_cast<std::uint64_t>(section.offset) + section.byte_size;
    if (section.offset < header_bytes || end > bytes.size()) {
      throw ToolError(ExitCode::format, "MYW section exceeds file bounds");
    }
    if (fixed_strides[index] != 0U &&
        static_cast<std::uint64_t>(section.count) * fixed_strides[index] !=
            section.byte_size) {
      throw ToolError(ExitCode::format,
                      "MYW fixed-record section size is inconsistent");
    }
    intervals.push_back(Interval{section.offset, end});
  }
  const auto grid_cells =
      static_cast<std::uint64_t>(result.grid_width) * result.grid_height;
  const auto grid_end =
      static_cast<std::uint64_t>(result.grid_offset) + grid_cells * 24U;
  if (result.grid_offset < header_bytes || grid_end > bytes.size()) {
    throw ToolError(ExitCode::format, "MYW grid section exceeds file bounds");
  }
  intervals.push_back(Interval{result.grid_offset, grid_end});
  std::sort(intervals.begin(), intervals.end(),
            [](const auto &left, const auto &right) {
              return left.start < right.start;
            });
  std::uint64_t cursor = 0U;
  for (const auto &interval : intervals) {
    if (interval.start != cursor || interval.end < interval.start) {
      throw ToolError(ExitCode::format,
                      "MYW sections do not form an exact partition");
    }
    cursor = interval.end;
  }
  if (cursor != bytes.size()) {
    throw ToolError(ExitCode::format,
                    "MYW sections do not consume the complete file");
  }

  const auto &position_section = result.sections[1U];
  result.positions.reserve(position_section.count);
  for (std::size_t index = 0U; index < position_section.count; ++index) {
    result.positions.push_back(float_record<3U>(
        bytes, static_cast<std::size_t>(position_section.offset) + index * 12U,
        "MYW position is not finite"));
  }
  const auto &plane_section = result.sections[4U];
  result.planes.reserve(plane_section.count);
  for (std::size_t index = 0U; index < plane_section.count; ++index) {
    result.planes.push_back(float_record<4U>(
        bytes, static_cast<std::size_t>(plane_section.offset) + index * 16U,
        "MYW plane is not finite"));
  }
  const auto &normal_section = result.sections[5U];
  result.normals.reserve(normal_section.count);
  for (std::size_t index = 0U; index < normal_section.count; ++index) {
    result.normals.push_back(float_record<3U>(
        bytes, static_cast<std::size_t>(normal_section.offset) + index * 12U,
        "MYW normal is not finite"));
  }

  const auto &name_section = result.sections[10U];
  auto name_cursor = static_cast<std::size_t>(name_section.offset);
  const auto name_end = name_cursor + name_section.byte_size;
  result.material_names.reserve(name_section.count);
  while (name_cursor < name_end) {
    const auto begin = name_cursor;
    while (name_cursor < name_end && bytes[name_cursor] != 0U) {
      ++name_cursor;
    }
    if (name_cursor == name_end || name_cursor == begin) {
      throw ToolError(ExitCode::format, "MYW material-name table is malformed");
    }
    result.material_names.emplace_back(
        reinterpret_cast<const char *>(bytes.data() + begin),
        name_cursor - begin);
    ++name_cursor;
  }
  if (result.material_names.size() != name_section.count) {
    throw ToolError(ExitCode::format,
                    "MYW material-name count is inconsistent");
  }

  const auto &object_section = result.sections[0U];
  const auto &primitive_section = result.sections[3U];
  const auto &material_section = result.sections[6U];
  const auto &texture_coordinate_section = result.sections[7U];
  std::vector<bool> used_material_records(material_section.count, false);
  std::vector<bool> used_texture_coordinate_bytes(
      texture_coordinate_section.byte_size, false);
  result.objects.reserve(object_section.count);
  result.primitives.reserve(primitive_section.count);
  for (std::size_t object_index = 0U; object_index < object_section.count;
       ++object_index) {
    const auto base =
        static_cast<std::size_t>(object_section.offset) + object_index * 228U;
    MywObject object;
    object.bounding_sphere = float_record<4U>(
        bytes, base + 0x70U, "MYW object bounding sphere is not finite");
    if (object.bounding_sphere[3U] < 0.0F) {
      throw ToolError(ExitCode::format,
                      "MYW object bounding sphere radius is invalid");
    }
    object.position_count = u16(bytes, base + 0x80U);
    object.normal_count = u16(bytes, base + 0x82U);
    object.plane_count = u16(bytes, base + 0x84U);
    object.primitive_count = u16(bytes, base + 0x86U);
    object.texture_coordinate_bytes = u32(bytes, base + 0x88U);
    const auto position_offset = u32(bytes, base + 0x8cU);
    const auto plane_offset = u32(bytes, base + 0x90U);
    const auto normal_offset = u32(bytes, base + 0x94U);
    object.texture_coordinate_offset = u32(bytes, base + 0x98U);
    object.primitive_offset = u32(bytes, base + 0x9cU);
    if (position_offset % 12U != 0U || plane_offset % 16U != 0U ||
        normal_offset % 12U != 0U ||
        static_cast<std::uint64_t>(position_offset) +
                object.position_count * 12U >
            position_section.byte_size ||
        static_cast<std::uint64_t>(plane_offset) + object.plane_count * 16U >
            plane_section.byte_size ||
        static_cast<std::uint64_t>(normal_offset) + object.normal_count * 12U >
            normal_section.byte_size ||
        static_cast<std::uint64_t>(object.texture_coordinate_offset) +
                object.texture_coordinate_bytes >
            texture_coordinate_section.byte_size ||
        object.primitive_offset > primitive_section.byte_size) {
      throw ToolError(ExitCode::format, "MYW object stream range is invalid");
    }
    object.first_position = position_offset / 12U;
    object.first_plane = plane_offset / 16U;
    object.first_normal = normal_offset / 12U;
    object.first_primitive =
        static_cast<std::uint32_t>(result.primitives.size());
    auto primitive_cursor = static_cast<std::size_t>(primitive_section.offset) +
                            object.primitive_offset;
    for (std::size_t primitive_index = 0U;
         primitive_index < object.primitive_count; ++primitive_index) {
      if (primitive_cursor >=
          static_cast<std::size_t>(primitive_section.offset) +
              primitive_section.byte_size) {
        throw ToolError(ExitCode::format, "MYW primitive stream is truncated");
      }
      MywPrimitive primitive;
      primitive.object_index = static_cast<std::uint32_t>(object_index);
      primitive.primitive_type = bytes[primitive_cursor];
      primitive.flags = bytes[primitive_cursor + 1U];
      if (primitive.primitive_type >= primitive_record_sizes.size()) {
        throw ToolError(ExitCode::format, "MYW primitive type is invalid");
      }
      const auto record_size = primitive_record_sizes[primitive.primitive_type];
      const auto section_end =
          static_cast<std::size_t>(primitive_section.offset) +
          primitive_section.byte_size;
      if (primitive_cursor > section_end ||
          section_end - primitive_cursor < record_size) {
        throw ToolError(ExitCode::format,
                        "MYW primitive record exceeds section bounds");
      }
      ++result.primitive_type_counts[primitive.primitive_type];
      std::size_t position_index_offset = 0U;
      std::size_t normal_index_offset = 0U;
      std::size_t texture_coordinate_offset = 0U;
      std::size_t material_offset = 0U;
      if (primitive.primitive_type <= 3U) {
        position_index_offset = 8U;
        normal_index_offset = (primitive.primitive_type & 1U) == 0U ? 14U : 16U;
        if (primitive.primitive_type >= 2U) {
          texture_coordinate_offset = 18U;
          material_offset = 20U;
        }
      } else if (primitive.primitive_type <= 7U) {
        position_index_offset =
            (primitive.primitive_type & 1U) == 0U ? 16U : 20U;
        normal_index_offset = (primitive.primitive_type & 1U) == 0U ? 22U : 28U;
        if (primitive.primitive_type >= 6U) {
          texture_coordinate_offset =
              (primitive.primitive_type & 1U) == 0U ? 26U : 30U;
          material_offset = (primitive.primitive_type & 1U) == 0U ? 28U : 32U;
        }
      } else {
        ++result.unsupported_primitive_count;
      }
      if (position_index_offset != 0U) {
        primitive.vertex_count =
            (primitive.primitive_type & 1U) == 0U ? 3U : 4U;
        const auto color_count = primitive.primitive_type <= 3U
                                     ? 1U
                                     : primitive.vertex_count;
        for (std::size_t color = 0U; color < color_count; ++color) {
          std::copy_n(bytes.begin() +
                          static_cast<std::ptrdiff_t>(primitive_cursor + 4U +
                                                      color * 4U),
                      4U, primitive.vertex_colors[color].begin());
        }
        if (color_count == 1U) {
          std::fill(primitive.vertex_colors.begin() + 1U,
                    primitive.vertex_colors.begin() + primitive.vertex_count,
                    primitive.vertex_colors[0U]);
        }
        const auto local_plane = u16(bytes, primitive_cursor + 2U);
        const auto local_normal =
            u16(bytes, primitive_cursor + normal_index_offset);
        if (local_plane >= object.plane_count ||
            local_normal >= object.normal_count) {
          throw ToolError(ExitCode::format,
                          "MYW primitive plane or normal index is invalid");
        }
        primitive.plane_index = object.first_plane + local_plane;
        primitive.normal_index = object.first_normal + local_normal;
        for (std::size_t vertex = 0U; vertex < primitive.vertex_count;
             ++vertex) {
          const auto local_position = u16(
              bytes, primitive_cursor + position_index_offset + vertex * 2U);
          if (local_position >= object.position_count) {
            throw ToolError(ExitCode::format,
                            "MYW primitive position index is invalid");
          }
          primitive.position_indices[vertex] =
              object.first_position + local_position;
        }
        if (primitive.vertex_count == 3U) {
          ++result.triangle_count;
        } else {
          ++result.quad_count;
        }
        result.vertex_reference_count += primitive.vertex_count;
      }
      if (material_offset != 0U) {
        const auto local_texture_offset =
            u16(bytes, primitive_cursor + texture_coordinate_offset);
        const auto coordinate_bytes =
            static_cast<std::uint32_t>(primitive.vertex_count) * 2U;
        if (local_texture_offset > object.texture_coordinate_bytes ||
            coordinate_bytes >
                object.texture_coordinate_bytes - local_texture_offset) {
          throw ToolError(
              ExitCode::format,
              "MYW primitive texture coordinates exceed the object range");
        }
        const auto global_texture_offset =
            object.texture_coordinate_offset + local_texture_offset;
        if (global_texture_offset > texture_coordinate_section.byte_size ||
            coordinate_bytes >
                texture_coordinate_section.byte_size - global_texture_offset) {
          throw ToolError(
              ExitCode::format,
              "MYW primitive texture coordinates exceed the section");
        }
        const auto texture_base =
            static_cast<std::size_t>(texture_coordinate_section.offset) +
            global_texture_offset;
        for (std::size_t vertex = 0U; vertex < primitive.vertex_count;
             ++vertex) {
          const auto byte_index =
              static_cast<std::size_t>(global_texture_offset) + vertex * 2U;
          if (used_texture_coordinate_bytes[byte_index] ||
              used_texture_coordinate_bytes[byte_index + 1U]) {
            throw ToolError(ExitCode::format,
                            "MYW texture-coordinate ranges overlap");
          }
          used_texture_coordinate_bytes[byte_index] = true;
          used_texture_coordinate_bytes[byte_index + 1U] = true;
          primitive.texture_coordinates[vertex] = {
              static_cast<float>(bytes[texture_base + vertex * 2U]) + 0.5F,
              static_cast<float>(bytes[texture_base + vertex * 2U + 1U]) +
                  0.5F};
        }
        const auto material_record_offset =
            u32(bytes, primitive_cursor + material_offset);
        if (material_record_offset % 12U != 0U ||
            material_record_offset >= material_section.byte_size) {
          throw ToolError(ExitCode::format,
                          "MYW primitive material record is invalid");
        }
        primitive.material_record_index = material_record_offset / 12U;
        if (used_material_records[primitive.material_record_index]) {
          throw ToolError(ExitCode::format,
                          "MYW material record is referenced more than once");
        }
        used_material_records[primitive.material_record_index] = true;
        primitive.material_name_index =
            u32(bytes, static_cast<std::size_t>(material_section.offset) +
                           material_record_offset);
        if (primitive.material_name_index >= result.material_names.size()) {
          throw ToolError(ExitCode::format,
                          "MYW primitive material name index is invalid");
        }
        primitive.has_texture_coordinates = true;
        ++result.material_primitive_count;
        result.texture_coordinate_count += primitive.vertex_count;
      }
      result.primitives.push_back(primitive);
      primitive_cursor += record_size;
    }
    result.objects.push_back(object);
  }
  if (result.primitives.size() != primitive_section.count) {
    throw ToolError(ExitCode::format, "MYW primitive count is inconsistent");
  }
  if (result.material_primitive_count != material_section.count ||
      result.material_primitive_count != texture_coordinate_section.count ||
      std::find(used_material_records.begin(), used_material_records.end(),
                false) != used_material_records.end() ||
      std::find(used_texture_coordinate_bytes.begin(),
                used_texture_coordinate_bytes.end(),
                false) != used_texture_coordinate_bytes.end()) {
    throw ToolError(
        ExitCode::format,
        "MYW textured primitives do not consume their attributed sections");
  }

  const auto grid_end_offset = static_cast<std::size_t>(grid_end);
  const auto &linked_record_section = result.sections[8U];
  const auto &grid_link_section = result.sections[9U];
  const auto &auxiliary_section = result.sections[11U];
  result.lights.reserve(linked_record_section.count);
  for (std::size_t index = 0U; index < linked_record_section.count; ++index) {
    const auto base =
        static_cast<std::size_t>(linked_record_section.offset) + index * 196U;
    MywPointLight light;
    light.type = bytes[base];
    if (light.type != 1U) {
      throw ToolError(ExitCode::format, "MYW point-light type is invalid");
    }
    light.position = float_record<3U>(bytes, base + 4U,
                                      "MYW point-light position is not finite");
    light.direction = float_record<3U>(
        bytes, base + 0x10U, "MYW point-light direction is not finite");
    light.color = float_record<4U>(bytes, base + 0x1cU,
                                   "MYW point-light color is not finite");
    light.range = f32(bytes, base + 0x2cU);
    light.coefficients = float_record<3U>(
        bytes, base + 0x30U, "MYW point-light coefficients are not finite");
    if (!std::isfinite(light.range) || light.range <= 0.0F) {
      throw ToolError(ExitCode::format, "MYW point-light range is invalid");
    }
    result.lights.push_back(light);
  }
  result.lens_flares.reserve(auxiliary_section.count);
  for (std::size_t index = 0U; index < auxiliary_section.count; ++index) {
    const auto base =
        static_cast<std::size_t>(auxiliary_section.offset) + index * 120U;
    MywLensFlare flare;
    flare.type = u32(bytes, base);
    flare.image_index = u32(bytes, base + 4U);
    flare.position = float_record<3U>(bytes, base + 8U,
                                      "MYW lens-flare position is not finite");
    flare.secondary_position = float_record<3U>(
        bytes, base + 0x14U, "MYW lens-flare secondary position is not finite");
    flare.packed_color = u32(bytes, base + 0x30U);
    flare.flags = u32(bytes, base + 0x34U);
    if (flare.type != 2U) {
      throw ToolError(ExitCode::format, "MYW lens-flare type is invalid");
    }
    result.lens_flares.push_back(flare);
  }
  std::vector<bool> used_grid_links(grid_link_section.count, false);
  std::vector<bool> referenced_linked_records(linked_record_section.count,
                                              false);
  std::vector<bool> used_auxiliary_records(auxiliary_section.count, false);
  result.grid_cells.reserve(static_cast<std::size_t>(grid_cells));
  result.grid_object_indices.reserve(static_cast<std::size_t>(grid_cells));
  for (auto grid_cursor = static_cast<std::size_t>(result.grid_offset);
       grid_cursor < grid_end_offset; grid_cursor += 24U) {
    MywGridCell cell;
    if (u32(bytes, grid_cursor + 4U) !=
            std::numeric_limits<std::uint32_t>::max() ||
        u32(bytes, grid_cursor + 12U) !=
            std::numeric_limits<std::uint32_t>::max()) {
      throw ToolError(ExitCode::format, "MYW grid reserved fields are invalid");
    }
    const auto object_offset = u32(bytes, grid_cursor);
    if (object_offset == std::numeric_limits<std::uint32_t>::max()) {
      result.grid_object_indices.push_back(object_offset);
    } else {
      if (object_offset % 228U != 0U ||
          object_offset >= object_section.byte_size) {
        throw ToolError(ExitCode::format,
                        "MYW grid object reference is invalid");
      }
      cell.object_index = object_offset / 228U;
      result.grid_object_indices.push_back(cell.object_index);
      ++result.occupied_grid_cells;
    }

    auto link_offset = u32(bytes, grid_cursor + 8U);
    while (link_offset != std::numeric_limits<std::uint32_t>::max()) {
      if (link_offset % 8U != 0U ||
          link_offset >= grid_link_section.byte_size) {
        throw ToolError(ExitCode::format, "MYW grid link offset is invalid");
      }
      const auto link_index = link_offset / 8U;
      if (used_grid_links[link_index]) {
        throw ToolError(ExitCode::format, "MYW grid link is cyclic or shared");
      }
      used_grid_links[link_index] = true;
      const auto link_base =
          static_cast<std::size_t>(grid_link_section.offset) + link_offset;
      const auto record_offset = u32(bytes, link_base);
      if (record_offset % 196U != 0U ||
          record_offset >= linked_record_section.byte_size) {
        throw ToolError(ExitCode::format,
                        "MYW linked spatial-record reference is invalid");
      }
      const auto record_index = record_offset / 196U;
      referenced_linked_records[record_index] = true;
      cell.light_indices.push_back(record_index);
      ++result.grid_link_count;
      link_offset = u32(bytes, link_base + 4U);
    }

    cell.auxiliary_record_count = u32(bytes, grid_cursor + 16U);
    const auto auxiliary_offset = u32(bytes, grid_cursor + 20U);
    if (cell.auxiliary_record_count == 0U) {
      if (auxiliary_offset != std::numeric_limits<std::uint32_t>::max()) {
        throw ToolError(ExitCode::format,
                        "MYW empty grid auxiliary span has an offset");
      }
    } else {
      if (auxiliary_offset % 120U != 0U ||
          static_cast<std::uint64_t>(auxiliary_offset) +
                  static_cast<std::uint64_t>(cell.auxiliary_record_count) *
                      120U >
              auxiliary_section.byte_size) {
        throw ToolError(ExitCode::format, "MYW grid auxiliary span is invalid");
      }
      cell.first_auxiliary_record = auxiliary_offset / 120U;
      for (std::uint32_t index = 0U; index < cell.auxiliary_record_count;
           ++index) {
        const auto record_index = cell.first_auxiliary_record + index;
        if (used_auxiliary_records[record_index]) {
          throw ToolError(ExitCode::format, "MYW grid auxiliary spans overlap");
        }
        used_auxiliary_records[record_index] = true;
      }
      result.grid_auxiliary_record_count += cell.auxiliary_record_count;
    }
    result.grid_cells.push_back(std::move(cell));
  }
  const auto has_unused = [](const std::vector<bool> &values) {
    return std::find(values.begin(), values.end(), false) != values.end();
  };
  if (has_unused(used_grid_links) || has_unused(referenced_linked_records) ||
      has_unused(used_auxiliary_records)) {
    throw ToolError(ExitCode::format,
                    "MYW grid does not consume all linked spatial records");
  }
  return result;
}

MywData read_myw(const std::filesystem::path &path,
                 const std::uint64_t maximum_file_bytes) {
  return parse_myw(read_bytes(path, maximum_file_bytes));
}

std::optional<std::size_t> myw_grid_cell_index(const MywData &world,
                                               const double x, const double z) {
  if (!std::isfinite(x) || !std::isfinite(z) ||
      !std::isfinite(world.grid_cell_size_x) ||
      !std::isfinite(world.grid_cell_size_z) || world.grid_cell_size_x <= 0.0 ||
      world.grid_cell_size_z <= 0.0 || world.grid_width == 0U ||
      world.grid_height == 0U) {
    return std::nullopt;
  }
  const auto column_value = std::floor(
      (x - static_cast<double>(world.minimum[0U])) / world.grid_cell_size_x);
  const auto row_value = std::floor(
      (z - static_cast<double>(world.minimum[2U])) / world.grid_cell_size_z);
  if (column_value < 0.0 || row_value < 0.0 ||
      column_value >= static_cast<double>(world.grid_width) ||
      row_value >= static_cast<double>(world.grid_height)) {
    return std::nullopt;
  }
  const auto column = static_cast<std::size_t>(column_value);
  const auto row = static_cast<std::size_t>(row_value);
  const auto index = row * static_cast<std::size_t>(world.grid_width) + column;
  if (index >= world.grid_object_indices.size()) {
    return std::nullopt;
  }
  return index;
}

std::optional<std::uint32_t>
myw_grid_object_index_at(const MywData &world, const double x, const double z) {
  const auto cell = myw_grid_cell_index(world, x, z);
  if (!cell.has_value()) {
    return std::nullopt;
  }
  const auto object_index = world.grid_object_indices[*cell];
  if (object_index == std::numeric_limits<std::uint32_t>::max() ||
      object_index >= world.objects.size()) {
    return std::nullopt;
  }
  return object_index;
}

MywPointLighting
evaluate_original_myw_point_lighting(const MywData &world,
                                     const std::array<float, 3U> &position) {
  constexpr float minimum_range = 0.1F;
  constexpr float light_scale = 1.75F;
  constexpr float negative_scale = 0.25F;
  MywPointLighting result;
  const auto cell_index =
      myw_grid_cell_index(world, position[0U], position[2U]);
  if (!cell_index.has_value() || *cell_index >= world.grid_cells.size()) {
    return result;
  }
  for (const auto light_index : world.grid_cells[*cell_index].light_indices) {
    if (light_index >= world.lights.size()) {
      continue;
    }
    const auto &light = world.lights[light_index];
    if (light.range <= minimum_range) {
      continue;
    }
    const auto dx = light.position[0U] - position[0U];
    const auto dy = light.position[1U] - position[1U];
    const auto dz = light.position[2U] - position[2U];
    const auto distance_squared = dx * dx + dy * dy + dz * dz;
    if (distance_squared >= light.range * light.range) {
      continue;
    }
    const auto falloff = 1.0F - std::sqrt(distance_squared) / light.range;
    auto intensity = light.color[3U] * falloff * falloff * light_scale;
    // p3.1 RVA 0x0004e482 compares zero with the signed result. The branch
    // skips the 0.25 multiply when zero <= result, so only negative lights
    // receive the post-scale.
    if (intensity < 0.0F) {
      intensity *= negative_scale;
    }
    if (intensity <= 0.0F) {
      continue;
    }
    for (std::size_t channel = 0U; channel < result.contribution.size();
         ++channel) {
      result.contribution[channel] +=
          std::clamp(light.color[channel], 0.0F, 1.0F) * intensity;
    }
    ++result.contributing_lights;
  }
  return result;
}

MywPointLighting evaluate_original_myw_object_point_lighting(
    const MywData &world, const std::array<float, 3U> &object_position,
    const std::array<float, 3U> &normal,
    const std::array<float, 3U> &object_view_direction,
    const float specular_factor,
    const float light_intensity) {
  constexpr float minimum_range = 0.1F;
  constexpr float maximum_positive_intensity = 0.4F;
  constexpr float negative_scale = 0.25F;
  constexpr float specular_threshold = 0.5F;
  MywPointLighting result;
  if (!std::isfinite(specular_factor) || specular_factor < 0.0F ||
      !std::isfinite(light_intensity) || light_intensity < 0.0F) {
    return result;
  }
  const auto normalized = [](const std::array<float, 3U> &value) {
    const auto length_squared = value[0U] * value[0U] +
                                value[1U] * value[1U] +
                                value[2U] * value[2U];
    if (!std::isfinite(length_squared) || length_squared <= 1.0e-12F) {
      return std::array<float, 3U>{};
    }
    const auto reciprocal = 1.0F / std::sqrt(length_squared);
    return std::array<float, 3U>{value[0U] * reciprocal,
                                value[1U] * reciprocal,
                                value[2U] * reciprocal};
  };
  const auto dot = [](const std::array<float, 3U> &left,
                      const std::array<float, 3U> &right) {
    return left[0U] * right[0U] + left[1U] * right[1U] +
           left[2U] * right[2U];
  };
  const auto unit_normal = normalized(normal);
  const auto unit_view = normalized(object_view_direction);
  const auto cell_index =
      myw_grid_cell_index(world, object_position[0U], object_position[2U]);
  if (!cell_index.has_value() || *cell_index >= world.grid_cells.size()) {
    return result;
  }
  for (const auto light_index : world.grid_cells[*cell_index].light_indices) {
    if (light_index >= world.lights.size()) {
      continue;
    }
    const auto &light = world.lights[light_index];
    if (light.range <= minimum_range) {
      continue;
    }
    const std::array<float, 3U> offset{
        light.position[0U] - object_position[0U],
        light.position[1U] - object_position[1U],
        light.position[2U] - object_position[2U]};
    const auto distance_squared = dot(offset, offset);
    if (distance_squared >= light.range * light.range ||
        distance_squared <= 1.0e-12F) {
      continue;
    }
    const auto distance = std::sqrt(distance_squared);
    const std::array<float, 3U> light_direction{
        offset[0U] / distance, offset[1U] / distance, offset[2U] / distance};
    const auto diffuse = dot(unit_normal, light_direction);
    if (diffuse <= 0.0F) {
      continue;
    }
    auto response = diffuse;
    // p3.1 uses positive alpha to enable the cubic view highlight. The
    // precompute owner at RVA 0x0004c1e8 preserves alpha's sign in the RGB
    // strength, caps only the positive side at 0.4, and quarters only the
    // negative side.
    if (light.color[3U] > 0.0F) {
      const std::array<float, 3U> reflection{
          unit_normal[0U] * (2.0F * diffuse) - light_direction[0U],
          unit_normal[1U] * (2.0F * diffuse) - light_direction[1U],
          unit_normal[2U] * (2.0F * diffuse) - light_direction[2U]};
      const auto alignment = dot(reflection, unit_view);
      if (alignment > specular_threshold) {
        response += alignment * alignment * alignment * specular_factor;
      }
    }
    const auto falloff = 1.0F - distance / light.range;
    auto intensity = light.color[3U] * falloff * falloff * light_intensity;
    intensity = std::min(intensity, maximum_positive_intensity);
    if (intensity < 0.0F) {
      intensity *= negative_scale;
    }
    intensity *= response;
    for (std::size_t channel = 0U; channel < result.contribution.size();
         ++channel) {
      result.contribution[channel] +=
          std::clamp(light.color[channel], 0.0F, 1.0F) * intensity;
    }
    ++result.contributing_lights;
  }
  return result;
}

std::array<float, 3U> evaluate_original_myw_directional_lighting(
    const MywData &world, const std::array<float, 3U> &normal) {
  if (std::any_of(normal.begin(), normal.end(),
                  [](const auto value) { return !std::isfinite(value); })) {
    throw ToolError(ExitCode::format,
                    "MYW object normal is not finite");
  }
  const auto diffuse =
      normal[0U] * world.directional_direction[0U] +
      normal[1U] * world.directional_direction[1U] +
      normal[2U] * world.directional_direction[2U];
  if (diffuse <= 0.0F) {
    return {};
  }
  std::array<float, 3U> result{};
  for (std::size_t channel = 0U; channel < result.size(); ++channel) {
    result[channel] =
        diffuse * world.directional_intensity *
        (static_cast<float>(world.directional_color[channel]) / 255.0F);
  }
  return result;
}

std::array<float, 3U> evaluate_original_myw_base_lighting(
    const std::array<std::uint8_t, 3U> &top_color,
    const float light_intensity) {
  constexpr float channel_scale = 1.0F / 255.0F;
  constexpr float base_brightness_scale = 2.5F;
  std::array<float, 3U> result{};
  for (std::size_t channel = 0U; channel < result.size(); ++channel) {
    result[channel] = static_cast<float>(top_color[channel]) * channel_scale *
                      light_intensity * base_brightness_scale;
  }
  return result;
}

} // namespace mh::content
