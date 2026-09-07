#include <formats/model_geometry.hpp>

#include <core/error.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <system_error>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::size_t header_bytes = 124U;
constexpr std::array<std::uint32_t, 10U> primitive_bytes{
    16U, 20U, 24U, 24U, 24U, 32U, 32U, 36U, 20U, 24U};
constexpr std::array<std::uint32_t, 10U> primitive_index_offsets{
    8U, 8U, 8U, 8U, 16U, 20U, 16U, 20U, 8U, 8U};

std::uint32_t u32(const std::span<const std::uint8_t> bytes,
                  const std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint16_t u16(const std::span<const std::uint8_t> bytes,
                  const std::size_t offset) {
  return static_cast<std::uint16_t>(
      bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

float f32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
  return std::bit_cast<float>(u32(bytes, offset));
}

void require_exact_stride(const MyoSection &section,
                          const std::uint32_t stride) {
  if (static_cast<std::uint64_t>(section.count) * stride != section.byte_size) {
    throw ToolError(ExitCode::format,
                    "MYO fixed-record section has an inconsistent size");
  }
}

} // namespace

MyoData parse_myo(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < header_bytes) {
    throw ToolError(ExitCode::format, "MYO header is truncated");
  }
  MyoData result;
  if (bytes[0] == '!' && bytes[1] == 'O' && bytes[2] == 'Y' &&
      bytes[3] == 'M') {
    result.version = 1U;
  } else if (bytes[0] == '2' && bytes[1] == 'O' && bytes[2] == 'Y' &&
             bytes[3] == 'M') {
    result.version = 2U;
  } else {
    throw ToolError(ExitCode::format, "MYO signature is invalid");
  }
  result.file_bytes = bytes.size();
  for (std::size_t index = 0U; index < result.sections.size(); ++index) {
    const auto offset = 4U + index * 12U;
    auto &section = result.sections[index];
    section.count = u32(bytes, offset);
    section.byte_size = u32(bytes, offset + 4U);
    section.offset = u32(bytes, offset + 8U);
    if (section.offset < header_bytes ||
        static_cast<std::uint64_t>(section.offset) + section.byte_size >
            bytes.size()) {
      throw ToolError(ExitCode::format, "MYO section is outside the file");
    }
  }
  if (result.version == 2U) {
    result.extension_count = u32(bytes, 92U);
    result.extension_offset = u32(bytes, 100U);
  }

  require_exact_stride(result.sections[0], 12U);
  require_exact_stride(result.sections[2], 16U);
  require_exact_stride(result.sections[3], 12U);
  require_exact_stride(result.sections[4], 12U);

  std::array<std::size_t, 7U> order{};
  for (std::size_t index = 0U; index < order.size(); ++index) {
    order[index] = index;
  }
  std::sort(order.begin(), order.end(), [&](const auto left, const auto right) {
    return result.sections[left].offset < result.sections[right].offset;
  });
  std::uint64_t content_end = header_bytes;
  for (const auto index : order) {
    const auto &section = result.sections[index];
    if (section.byte_size == 0U) {
      continue;
    }
    if (section.offset != content_end) {
      throw ToolError(ExitCode::format,
                      "MYO sections are not a contiguous partition");
    }
    content_end += section.byte_size;
  }

  const auto &positions = result.sections[0];
  result.positions.reserve(positions.count);
  if (positions.count != 0U) {
    result.minimum.fill(std::numeric_limits<float>::infinity());
    result.maximum.fill(-std::numeric_limits<float>::infinity());
    for (std::uint32_t vertex = 0U; vertex < positions.count; ++vertex) {
      std::array<float, 3U> position{};
      for (std::size_t axis = 0U; axis < 3U; ++axis) {
        const auto value =
            f32(bytes, static_cast<std::size_t>(positions.offset) +
                           vertex * 12U + axis * 4U);
        if (!std::isfinite(value)) {
          throw ToolError(ExitCode::format, "MYO position is not finite");
        }
        result.minimum[axis] = std::min(result.minimum[axis], value);
        result.maximum[axis] = std::max(result.maximum[axis], value);
        position[axis] = value;
      }
      result.positions.push_back(position);
    }
  }

  const auto &primitives = result.sections[1];
  if (primitives.count > primitives.byte_size / primitive_bytes.front()) {
    throw ToolError(ExitCode::format,
                    "MYO primitive count exceeds its section bound");
  }
  std::uint64_t primitive_cursor = primitives.offset;
  const auto primitive_end = primitive_cursor + primitives.byte_size;
  std::vector<bool> used_material_records(result.sections[4].count, false);
  std::vector<bool> used_texture_bytes(result.sections[5].byte_size, false);
  for (std::uint32_t index = 0U; index < primitives.count; ++index) {
    if (primitive_cursor >= primitive_end) {
      throw ToolError(ExitCode::format, "MYO primitive stream is truncated");
    }
    const auto type = bytes[static_cast<std::size_t>(primitive_cursor)];
    if (type >= primitive_bytes.size() ||
        primitive_bytes[type] > primitive_end - primitive_cursor) {
      throw ToolError(ExitCode::format,
                      "MYO primitive type or record size is invalid");
    }
    ++result.primitive_type_counts[type];
    MyoFace face;
    face.primitive_type = type;
    face.flags = bytes[static_cast<std::size_t>(primitive_cursor) + 1U];
    face.vertex_count = (type & 1U) == 0U ? 3U : 4U;
    face.plane_index =
        u16(bytes, static_cast<std::size_t>(primitive_cursor) + 2U);
    if (face.plane_index >= result.sections[2].count) {
      throw ToolError(ExitCode::format,
                      "MYO primitive plane index is out of bounds");
    }
    for (std::size_t channel = 0U; channel < face.color.size(); ++channel) {
      face.color[channel] =
          bytes[static_cast<std::size_t>(primitive_cursor) + 4U + channel];
    }
    const auto index_offset = primitive_index_offsets[type];
    for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
      const auto index = u16(bytes, static_cast<std::size_t>(primitive_cursor) +
                                        index_offset + vertex * 2U);
      if (index >= positions.count) {
        throw ToolError(ExitCode::format,
                        "MYO primitive position index is out of bounds");
      }
      face.position_indices[vertex] = index;
    }
    const auto normal_offset =
        index_offset + static_cast<std::size_t>(face.vertex_count) * 2U;
    face.has_vertex_normals = type == 8U || type == 9U;
    if (face.has_vertex_normals) {
      for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
        face.normal_indices[vertex] =
            u16(bytes, static_cast<std::size_t>(primitive_cursor) +
                           normal_offset + vertex * 2U);
        if (face.normal_indices[vertex] >= result.sections[3].count) {
          throw ToolError(ExitCode::format,
                          "MYO primitive vertex normal index is out of bounds");
        }
      }
      face.normal_index = face.normal_indices[0U];
    } else {
      face.normal_index = u16(
          bytes, static_cast<std::size_t>(primitive_cursor) + normal_offset);
      if (face.normal_index >= result.sections[3].count) {
        throw ToolError(ExitCode::format,
                        "MYO primitive normal index is out of bounds");
      }
      std::fill_n(face.normal_indices.begin(), face.vertex_count,
                  face.normal_index);
    }
    if (type == 2U || type == 3U) {
      const auto texture_offset =
          u16(bytes, static_cast<std::size_t>(primitive_cursor) + 18U);
      const auto coordinate_bytes =
          static_cast<std::uint32_t>(face.vertex_count) * 2U;
      if (texture_offset > result.sections[5].byte_size ||
          coordinate_bytes > result.sections[5].byte_size - texture_offset) {
        throw ToolError(ExitCode::format,
                        "MYO primitive texture coordinates are out of bounds");
      }
      const auto texture_base =
          static_cast<std::size_t>(result.sections[5].offset) + texture_offset;
      for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
        const auto first_byte =
            static_cast<std::size_t>(texture_offset) + vertex * 2U;
        if (used_texture_bytes[first_byte] ||
            used_texture_bytes[first_byte + 1U]) {
          throw ToolError(ExitCode::format,
                          "MYO primitive texture-coordinate ranges overlap");
        }
        used_texture_bytes[first_byte] = true;
        used_texture_bytes[first_byte + 1U] = true;
        face.texture_coordinates[vertex] = {
            static_cast<float>(bytes[texture_base + vertex * 2U]) + 0.5F,
            static_cast<float>(bytes[texture_base + vertex * 2U + 1U]) + 0.5F};
      }
      const auto material_offset =
          u32(bytes, static_cast<std::size_t>(primitive_cursor) + 20U);
      if (material_offset % 12U != 0U ||
          material_offset > result.sections[4].byte_size ||
          4U > result.sections[4].byte_size - material_offset) {
        throw ToolError(ExitCode::format,
                        "MYO primitive material record is out of bounds");
      }
      const auto material_record = material_offset / 12U;
      if (used_material_records[material_record]) {
        throw ToolError(ExitCode::format,
                        "MYO material record is referenced more than once");
      }
      used_material_records[material_record] = true;
      face.material_name_index =
          u32(bytes, static_cast<std::size_t>(result.sections[4].offset) +
                         material_offset);
      if (face.material_name_index >= result.sections[6].count) {
        throw ToolError(ExitCode::format,
                        "MYO material name index is out of bounds");
      }
      face.has_texture_coordinates = true;
      ++result.textured_face_count;
      result.texture_coordinate_count += face.vertex_count;
    }
    if (face.vertex_count == 3U) {
      ++result.triangle_count;
    } else {
      ++result.quad_count;
    }
    result.vertex_reference_count += face.vertex_count;
    result.faces.push_back(face);
    primitive_cursor += primitive_bytes[type];
  }
  if (primitive_cursor != primitive_end) {
    throw ToolError(ExitCode::format,
                    "MYO primitive records do not consume their section");
  }
  if (result.textured_face_count != result.sections[4].count ||
      result.textured_face_count != result.sections[5].count ||
      std::find(used_material_records.begin(), used_material_records.end(),
                false) != used_material_records.end() ||
      std::find(used_texture_bytes.begin(), used_texture_bytes.end(), false) !=
          used_texture_bytes.end()) {
    throw ToolError(ExitCode::format, "MYO textured primitives do not consume "
                                      "their material/coordinate sections");
  }

  const auto &normal_section = result.sections[3];
  result.normals.reserve(normal_section.count);
  for (std::uint32_t index = 0U; index < normal_section.count; ++index) {
    result.normals.push_back(
        {f32(bytes, normal_section.offset + index * 12U),
         f32(bytes, normal_section.offset + index * 12U + 4U),
         f32(bytes, normal_section.offset + index * 12U + 8U)});
  }
  for (const auto &face : result.faces) {
    const auto &normal = result.normals[face.normal_index];
    if (!std::isfinite(normal[0]) || !std::isfinite(normal[1]) ||
        !std::isfinite(normal[2])) {
      ++result.invalid_referenced_normals;
    }
  }

  const auto &names = result.sections[6];
  if (names.count > names.byte_size / 2U) {
    throw ToolError(ExitCode::format,
                    "MYO name count exceeds its section bound");
  }
  std::size_t name_start = names.offset;
  const auto names_end = name_start + names.byte_size;
  for (std::size_t cursor = name_start; cursor < names_end; ++cursor) {
    if (bytes[cursor] == 0U) {
      if (cursor == name_start) {
        throw ToolError(ExitCode::format,
                        "MYO name table contains an empty entry");
      }
      result.names.emplace_back(
          reinterpret_cast<const char *>(bytes.data() + name_start),
          cursor - name_start);
      name_start = cursor + 1U;
    } else if (bytes[cursor] < 0x20U || bytes[cursor] > 0x7eU) {
      throw ToolError(ExitCode::format,
                      "MYO name table is not printable ASCII");
    }
  }
  if (name_start != names_end || result.names.size() != names.count) {
    throw ToolError(ExitCode::format,
                    "MYO name table count or termination is invalid");
  }

  if (result.version == 1U) {
    if (content_end != bytes.size()) {
      throw ToolError(ExitCode::format,
                      "MYO v1 has unexplained trailing bytes");
    }
  } else if (result.extension_offset != content_end ||
             static_cast<std::uint64_t>(result.extension_offset) +
                     static_cast<std::uint64_t>(result.extension_count) *
                         120U !=
                 bytes.size()) {
    throw ToolError(ExitCode::format,
                    "MYO v2 extension section is inconsistent");
  } else {
    result.light_records.reserve(result.extension_count);
    for (std::uint32_t index = 0U; index < result.extension_count; ++index) {
      const auto base = static_cast<std::size_t>(result.extension_offset) +
                        static_cast<std::size_t>(index) * 120U;
      MyoLightRecord record;
      record.type = u32(bytes, base);
      record.image_index = u32(bytes, base + 4U);
      for (std::size_t axis = 0U; axis < 3U; ++axis) {
        record.position[axis] = f32(bytes, base + 8U + axis * 4U);
        record.secondary_position[axis] = f32(bytes, base + 0x14U + axis * 4U);
        if (!std::isfinite(record.position[axis]) ||
            !std::isfinite(record.secondary_position[axis])) {
          throw ToolError(ExitCode::format,
                          "MYO light-record position is not finite");
        }
      }
      record.packed_color = u32(bytes, base + 0x30U);
      record.flags = u32(bytes, base + 0x34U);
      if (record.type != 2U) {
        throw ToolError(ExitCode::format,
                        "MYO v2 light-record type is invalid");
      }
      result.light_records.push_back(record);
    }
  }
  return result;
}

MyoData read_myo(const std::filesystem::path &path,
                 const std::uint64_t maximum_file_bytes) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    throw ToolError(ExitCode::input, "MYO input is not a plain file");
  }
  const auto size = std::filesystem::file_size(path);
  if (size > maximum_file_bytes ||
      size > std::numeric_limits<std::size_t>::max()) {
    throw ToolError(ExitCode::format,
                    "MYO file exceeds the configured size bound");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  std::ifstream stream(path, std::ios::binary);
  stream.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  if (!stream ||
      stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw ToolError(ExitCode::input, "short read while opening MYO input");
  }
  return parse_myo(bytes);
}

} // namespace mh::content
