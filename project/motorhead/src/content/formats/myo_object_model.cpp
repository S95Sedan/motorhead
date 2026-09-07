#include <content/formats/myo_object_model.hpp>

#include <core/error.hpp>
#include <core/serialization/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

void append_le32(std::vector<std::uint8_t> &bytes, const std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void append_float(std::vector<std::uint8_t> &bytes, const float value) {
  append_le32(bytes, std::bit_cast<std::uint32_t>(value));
}

std::array<float, 3U> normalized_face_normal(const MyoData &model,
                                             const MyoFace &face) {
  auto normal = model.normals[face.normal_index];
  auto length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                          normal[2] * normal[2]);
  if (!std::isfinite(length) || length <= 1.0e-12F) {
    const auto &first = model.positions[face.position_indices[0]];
    const auto &second = model.positions[face.position_indices[1]];
    const auto &third = model.positions[face.position_indices[2]];
    const std::array<float, 3U> left{second[0] - first[0], second[1] - first[1],
                                     second[2] - first[2]};
    const std::array<float, 3U> right{third[0] - first[0], third[1] - first[1],
                                      third[2] - first[2]};
    normal = {left[1] * right[2] - left[2] * right[1],
              left[2] * right[0] - left[0] * right[2],
              left[0] * right[1] - left[1] * right[0]};
    length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                       normal[2] * normal[2]);
  }
  if (!std::isfinite(length) || length <= 1.0e-12F) {
    return {0.0F, 0.0F, 1.0F};
  }
  for (auto &component : normal) {
    component /= length;
  }
  return normal;
}

void write_myo_bytes_guarded(const std::filesystem::path &output,
                             const std::span<const std::uint8_t> bytes,
                             const bool overwrite) {
  if (output.empty()) {
    throw ToolError(ExitCode::usage, "MYO GLB output path is empty");
  }
  const auto destination = std::filesystem::absolute(output).lexically_normal();
  const auto parent = destination.parent_path();
  std::error_code error;
  auto current = parent.root_path();
  for (const auto &component : parent.relative_path()) {
    current /= component;
    auto status = std::filesystem::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      if (!std::filesystem::create_directory(current, error) || error) {
        throw ToolError(ExitCode::input,
                        "cannot create MYO GLB output directory");
      }
      status = std::filesystem::symlink_status(current, error);
    }
    if (error || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_directory(status)) {
      throw ToolError(ExitCode::input,
                      "MYO GLB output parent is not a plain directory");
    }
  }
  error.clear();
  const auto status = std::filesystem::symlink_status(destination, error);
  const bool exists = !error && std::filesystem::exists(status);
  if (exists && (std::filesystem::is_symlink(status) ||
                 !std::filesystem::is_regular_file(status))) {
    throw ToolError(ExitCode::input, "MYO GLB output is not a plain file");
  }
  if (exists && !overwrite) {
    throw ToolError(ExitCode::input, "MYO GLB output exists (use --overwrite)");
  }
  auto temporary = destination;
  temporary += ".mhtool-part";
  auto backup = destination;
  backup += ".mhtool-backup";
  if (std::filesystem::exists(temporary) ||
      (exists && std::filesystem::exists(backup))) {
    throw ToolError(ExitCode::input,
                    "MYO GLB temporary or backup output exists");
  }
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
      std::filesystem::remove(temporary, error);
      throw ToolError(ExitCode::input, "cannot write MYO GLB output");
    }
  }
  bool backed_up = false;
  if (exists) {
    std::filesystem::rename(destination, backup, error);
    if (error) {
      std::filesystem::remove(temporary, error);
      throw ToolError(ExitCode::input, "cannot preserve MYO GLB output");
    }
    backed_up = true;
  }
  std::filesystem::rename(temporary, destination, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    if (backed_up) {
      std::error_code ignored;
      std::filesystem::rename(backup, destination, ignored);
    }
    throw ToolError(ExitCode::input, "cannot finalize MYO GLB output");
  }
  if (backed_up) {
    std::filesystem::remove(backup, error);
    if (error) {
      throw ToolError(ExitCode::input, "cannot remove MYO GLB backup");
    }
  }
}

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

void write_myo_obj(const MyoData &model, const std::filesystem::path &output,
                   const bool overwrite) {
  if (output.empty()) {
    throw ToolError(ExitCode::usage, "MYO OBJ output path is empty");
  }
  const auto destination = std::filesystem::absolute(output).lexically_normal();
  const auto parent = destination.parent_path();
  std::error_code error;
  auto current = parent.root_path();
  for (const auto &component : parent.relative_path()) {
    current /= component;
    auto status = std::filesystem::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      if (!std::filesystem::create_directory(current, error) || error) {
        throw ToolError(ExitCode::input,
                        "cannot create MYO OBJ output directory");
      }
      status = std::filesystem::symlink_status(current, error);
    }
    if (error || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_directory(status)) {
      throw ToolError(ExitCode::input,
                      "MYO OBJ output parent is not a plain directory");
    }
  }
  const auto status = std::filesystem::symlink_status(destination, error);
  const bool exists = !error && std::filesystem::exists(status);
  if (exists && (std::filesystem::is_symlink(status) ||
                 !std::filesystem::is_regular_file(status))) {
    throw ToolError(ExitCode::input, "MYO OBJ output is not a plain file");
  }
  if (exists && !overwrite) {
    throw ToolError(ExitCode::input, "MYO OBJ output exists (use --overwrite)");
  }
  auto temporary = destination;
  temporary += ".mhtool-part";
  auto backup = destination;
  backup += ".mhtool-backup";
  if (std::filesystem::exists(temporary) ||
      (exists && std::filesystem::exists(backup))) {
    throw ToolError(ExitCode::input,
                    "MYO OBJ temporary or backup output exists");
  }
  std::ostringstream text;
  text << "# Motorhead MYO attributed topology export\n"
       << std::setprecision(9);
  for (const auto &position : model.positions) {
    text << "v " << position[0] << ' ' << position[1] << ' ' << position[2]
         << '\n';
  }
  for (const auto &face : model.faces) {
    auto normal = model.normals[face.normal_index];
    auto length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                            normal[2] * normal[2]);
    if (!std::isfinite(length) || length <= 1.0e-12F) {
      const auto &first = model.positions[face.position_indices[0]];
      const auto &second = model.positions[face.position_indices[1]];
      const auto &third = model.positions[face.position_indices[2]];
      const std::array<float, 3U> left{
          second[0] - first[0], second[1] - first[1], second[2] - first[2]};
      const std::array<float, 3U> right{
          third[0] - first[0], third[1] - first[1], third[2] - first[2]};
      normal = {left[1] * right[2] - left[2] * right[1],
                left[2] * right[0] - left[0] * right[2],
                left[0] * right[1] - left[1] * right[0]};
      length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] +
                         normal[2] * normal[2]);
    }
    if (!std::isfinite(length) || length <= 1.0e-12F) {
      normal = {0.0F, 0.0F, 1.0F};
    } else {
      for (auto &component : normal) {
        component /= length;
      }
    }
    text << "vn " << normal[0] << ' ' << normal[1] << ' ' << normal[2] << '\n';
  }
  for (const auto &face : model.faces) {
    if (!face.has_texture_coordinates) {
      continue;
    }
    for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
      text << "vt " << face.texture_coordinates[vertex][0] << ' '
           << face.texture_coordinates[vertex][1] << '\n';
    }
  }
  std::uint64_t texture_index = 1U;
  for (std::size_t face_index = 0U; face_index < model.faces.size();
       ++face_index) {
    const auto &face = model.faces[face_index];
    text << "# primitive " << face_index << " type "
         << static_cast<unsigned int>(face.primitive_type) << " flags "
         << static_cast<unsigned int>(face.flags) << " rgb "
         << static_cast<unsigned int>(face.color[0]) << ' '
         << static_cast<unsigned int>(face.color[1]) << ' '
         << static_cast<unsigned int>(face.color[2]);
    if (face.material_name_index < model.names.size()) {
      text << " material " << model.names[face.material_name_index];
    }
    text << "\nf";
    for (std::size_t vertex = 0U; vertex < face.vertex_count; ++vertex) {
      text << ' ' << face.position_indices[vertex] + 1U << '/';
      if (face.has_texture_coordinates) {
        text << texture_index + vertex;
      }
      text << '/' << face_index + 1U;
    }
    text << '\n';
    if (face.has_texture_coordinates) {
      texture_index += face.vertex_count;
    }
  }
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream << text.str();
    if (!stream) {
      std::filesystem::remove(temporary, error);
      throw ToolError(ExitCode::input, "cannot write MYO OBJ output");
    }
  }
  bool backed_up = false;
  if (exists) {
    std::filesystem::rename(destination, backup, error);
    if (error) {
      std::filesystem::remove(temporary, error);
      throw ToolError(ExitCode::input, "cannot preserve MYO OBJ output");
    }
    backed_up = true;
  }
  std::filesystem::rename(temporary, destination, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    if (backed_up) {
      std::error_code ignored;
      std::filesystem::rename(backup, destination, ignored);
    }
    throw ToolError(ExitCode::input, "cannot finalize MYO OBJ output");
  }
  if (backed_up) {
    std::filesystem::remove(backup, error);
    if (error) {
      throw ToolError(ExitCode::input, "cannot remove MYO OBJ backup");
    }
  }
}

MyoGlbExport write_myo_glb(const MyoData &model,
                           const std::filesystem::path &output,
                           const bool overwrite) {
  struct PrimitiveGroup {
    std::string material;
    std::vector<const MyoFace *> faces;
    std::uint32_t byte_offset = 0U;
    std::uint32_t vertex_count = 0U;
    std::array<float, 3U> minimum{std::numeric_limits<float>::max(),
                                  std::numeric_limits<float>::max(),
                                  std::numeric_limits<float>::max()};
    std::array<float, 3U> maximum{std::numeric_limits<float>::lowest(),
                                  std::numeric_limits<float>::lowest(),
                                  std::numeric_limits<float>::lowest()};
  };
  std::vector<PrimitiveGroup> groups;
  std::map<std::string, std::size_t> group_indices;
  for (const auto &face : model.faces) {
    const auto material = face.material_name_index < model.names.size()
                              ? model.names[face.material_name_index]
                              : std::string("untextured");
    auto [found, inserted] = group_indices.emplace(material, groups.size());
    if (inserted) {
      groups.emplace_back();
      groups.back().material = material;
    }
    groups[found->second].faces.push_back(&face);
  }
  if (groups.empty()) {
    throw ToolError(ExitCode::format,
                    "MYO GLB export requires at least one face");
  }

  constexpr std::uint32_t vertex_stride = 40U;
  constexpr std::array<std::array<std::size_t, 3U>, 2U> corners{
      {{0U, 1U, 2U}, {0U, 2U, 3U}}};
  std::vector<std::uint8_t> binary;
  std::uint64_t triangle_count = 0U;
  for (auto &group : groups) {
    if (binary.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw ToolError(ExitCode::format,
                      "MYO GLB binary offset exceeds 32 bits");
    }
    group.byte_offset = static_cast<std::uint32_t>(binary.size());
    for (const auto *face : group.faces) {
      const auto normal = normalized_face_normal(model, *face);
      const auto face_triangle_count = face->vertex_count == 3U ? 1U : 2U;
      triangle_count += face_triangle_count;
      for (std::size_t triangle = 0U; triangle < face_triangle_count;
           ++triangle) {
        for (const auto corner : corners[triangle]) {
          const auto &position =
              model.positions[face->position_indices[corner]];
          for (std::size_t axis = 0U; axis < 3U; ++axis) {
            append_float(binary, position[axis]);
            group.minimum[axis] = std::min(group.minimum[axis], position[axis]);
            group.maximum[axis] = std::max(group.maximum[axis], position[axis]);
          }
          for (const auto value : normal) {
            append_float(binary, value);
          }
          const auto use_white = face->has_texture_coordinates ||
                                 (face->color[0] == 0U &&
                                  face->color[1] == 0U && face->color[2] == 0U);
          binary.insert(
              binary.end(),
              use_white
                  ? std::initializer_list<std::uint8_t>{255U, 255U, 255U, 255U}
                  : std::initializer_list<std::uint8_t>{
                        face->color[0], face->color[1], face->color[2], 255U});
          binary.insert(binary.end(), {face->color[0], face->color[1],
                                       face->color[2], face->flags});
          const auto texel = face->has_texture_coordinates
                                 ? face->texture_coordinates[corner]
                                 : std::array<float, 2U>{0.0F, 0.0F};
          append_float(binary, texel[0]);
          append_float(binary, texel[1]);
          ++group.vertex_count;
        }
      }
    }
  }
  if (binary.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw ToolError(ExitCode::format, "MYO GLB binary payload exceeds 32 bits");
  }

  std::ostringstream json;
  json << std::setprecision(9)
       << "{\"asset\":{\"version\":\"2.0\",\"generator\":\"Motorhead mhtool\"},"
       << "\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
       << "\"nodes\":[{\"mesh\":0,\"name\":\"Motorhead MYO\"}],"
       << "\"buffers\":[{\"byteLength\":" << binary.size() << "}],"
       << "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":"
       << binary.size() << ",\"byteStride\":" << vertex_stride
       << ",\"target\":34962}],\"accessors\":[";
  bool first = true;
  for (const auto &group : groups) {
    const auto append_accessor = [&](const std::uint32_t offset,
                                     const std::uint32_t component_type,
                                     const std::string_view type,
                                     const bool normalized,
                                     const bool include_bounds) {
      if (!first) {
        json << ',';
      }
      first = false;
      json << "{\"bufferView\":0,\"byteOffset\":" << group.byte_offset + offset
           << ",\"componentType\":" << component_type
           << ",\"count\":" << group.vertex_count
           << ",\"type\":" << mh::common::json_string(type);
      if (normalized) {
        json << ",\"normalized\":true";
      }
      if (include_bounds) {
        json << ",\"min\":[" << group.minimum[0] << ',' << group.minimum[1]
             << ',' << group.minimum[2] << "],\"max\":[" << group.maximum[0]
             << ',' << group.maximum[1] << ',' << group.maximum[2] << ']';
      }
      json << '}';
    };
    append_accessor(0U, 5126U, "VEC3", false, true);
    append_accessor(12U, 5126U, "VEC3", false, false);
    append_accessor(24U, 5121U, "VEC4", true, false);
    append_accessor(28U, 5121U, "VEC4", false, false);
    append_accessor(32U, 5126U, "VEC2", false, false);
  }
  json << "],\"materials\":[";
  for (std::size_t index = 0U; index < groups.size(); ++index) {
    if (index != 0U) {
      json << ',';
    }
    json << "{\"name\":" << mh::common::json_string(groups[index].material)
         << ",\"doubleSided\":true,\"pbrMetallicRoughness\":{"
         << "\"baseColorFactor\":[1,1,1,1],\"metallicFactor\":0,"
         << "\"roughnessFactor\":1},\"extras\":{\"motorheadMaterialName\":"
         << mh::common::json_string(groups[index].material) << "}}";
  }
  json << "],\"meshes\":[{\"name\":\"Motorhead MYO geometry\",\"primitives\":[";
  for (std::size_t index = 0U; index < groups.size(); ++index) {
    if (index != 0U) {
      json << ',';
    }
    const auto accessor = index * 5U;
    json << "{\"attributes\":{\"POSITION\":" << accessor
         << ",\"NORMAL\":" << accessor + 1U << ",\"COLOR_0\":" << accessor + 2U
         << ",\"_MOTORHEAD_FACE\":" << accessor + 3U
         << ",\"_MOTORHEAD_TEXEL\":" << accessor + 4U
         << "},\"material\":" << index << ",\"mode\":4,\"extras\":{"
         << "\"rawTexelCoordinates\":true}}";
  }
  json << "],\"extras\":{\"sourceFormat\":\"Motorhead MYO\","
       << "\"texelCoordinateUnit\":\"source pixels\"}}]}";
  auto json_text = json.str();
  while ((json_text.size() & 3U) != 0U) {
    json_text.push_back(' ');
  }
  while ((binary.size() & 3U) != 0U) {
    binary.push_back(0U);
  }
  const auto total_size =
      12ULL + 8ULL + json_text.size() + 8ULL + binary.size();
  if (json_text.size() > std::numeric_limits<std::uint32_t>::max() ||
      total_size > std::numeric_limits<std::uint32_t>::max()) {
    throw ToolError(ExitCode::format,
                    "MYO GLB output exceeds its 32-bit container");
  }
  std::vector<std::uint8_t> glb;
  glb.reserve(static_cast<std::size_t>(total_size));
  glb.insert(glb.end(), {'g', 'l', 'T', 'F'});
  append_le32(glb, 2U);
  append_le32(glb, static_cast<std::uint32_t>(total_size));
  append_le32(glb, static_cast<std::uint32_t>(json_text.size()));
  append_le32(glb, 0x4e4f534aU);
  glb.insert(glb.end(), json_text.begin(), json_text.end());
  append_le32(glb, static_cast<std::uint32_t>(binary.size()));
  append_le32(glb, 0x004e4942U);
  glb.insert(glb.end(), binary.begin(), binary.end());
  write_myo_bytes_guarded(output, glb, overwrite);
  return MyoGlbExport{triangle_count, triangle_count * 3U, groups.size(),
                      glb.size()};
}

} // namespace mh::content
