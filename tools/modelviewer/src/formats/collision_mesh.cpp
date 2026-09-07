#include <formats/collision_mesh.hpp>

#include <core/error.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::size_t fixed_header_bytes = 40U;

std::uint16_t u16(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset]
        | (static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

std::uint32_t u32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

float f32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    return std::bit_cast<float>(u32(bytes, offset));
}

void require_finite(const float value, const char* message) {
    if (!std::isfinite(value)) { throw ToolError(ExitCode::format, message); }
}

using Vector3d = std::array<double, 3U>;

Vector3d cross(const Vector3d& left, const Vector3d& right) {
    return {left[1] * right[2] - left[2] * right[1],
        left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0]};
}

double dot(const Vector3d& left, const Vector3d& right) {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

Vector3d vector3d(const std::array<float, 3U>& value) {
    return {value[0], value[1], value[2]};
}

Vector3d normalized(const Vector3d& value) {
    const auto length = std::sqrt(dot(value, value));
    if (length <= 1.0e-12) {
        throw ToolError(ExitCode::format, "COL polygon uses a zero-length plane normal");
    }
    return {value[0] / length, value[1] / length, value[2] / length};
}

} // namespace

ColData parse_col(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < fixed_header_bytes) {
        throw ToolError(ExitCode::format, "COL fixed header is truncated");
    }

    ColData result;
    result.file_bytes = bytes.size();
    result.prefix_bytes = u16(bytes, 0U);
    result.version = u16(bytes, 2U);
    if (result.version != 0U && result.version != 2U && result.version != 3U) {
        throw ToolError(ExitCode::format, "COL version is not in the observed p3.1 set");
    }
    if (result.prefix_bytes < fixed_header_bytes
        || (result.prefix_bytes & 1U) != 0U
        || result.prefix_bytes > bytes.size()) {
        throw ToolError(ExitCode::format, "COL prefix size is invalid");
    }
    result.prefix_word_count = (result.prefix_bytes - fixed_header_bytes) / 2U;

    for (std::size_t axis = 0U; axis < result.grid_dimensions.size(); ++axis) {
        result.grid_dimensions[axis] = u16(bytes, 4U + axis * 2U);
        if (result.grid_dimensions[axis] == 0U) {
            throw ToolError(ExitCode::format, "COL grid dimension is zero");
        }
        result.origin[axis] = f32(bytes, 20U + axis * 4U);
        require_finite(result.origin[axis], "COL origin is not finite");
    }
    result.scale = f32(bytes, 32U);
    result.inverse_scale = f32(bytes, 36U);
    require_finite(result.scale, "COL scale is not finite");
    require_finite(result.inverse_scale, "COL inverse scale is not finite");
    if (result.scale <= 0.0F || result.inverse_scale <= 0.0F
        || std::fabs(result.scale * result.inverse_scale - 1.0F) > 0.0001F) {
        throw ToolError(ExitCode::format, "COL scale fields are not a positive reciprocal pair");
    }
    result.spatial_reference_count = u16(bytes, 16U);

    // The p3.1 loader at RVA 0x00027420 establishes this exact pointer order.
    result.sections[0] = ColSection{u16(bytes, 12U), 16U, result.prefix_bytes, 0U};
    result.sections[1] = ColSection{u16(bytes, 14U), 4U, 0U, 0U};
    result.sections[2] = ColSection{u16(bytes, 10U), 28U, 0U, 0U};
    result.sections[3] = ColSection{u16(bytes, 18U), 16U, 0U, 0U};
    std::uint64_t cursor = result.prefix_bytes;
    for (auto& section : result.sections) {
        section.offset = cursor;
        section.byte_size = static_cast<std::uint64_t>(section.count) * section.stride;
        if (section.byte_size > bytes.size() - cursor) {
            throw ToolError(ExitCode::format, "COL section exceeds the file bound");
        }
        cursor += section.byte_size;
    }
    if (cursor != bytes.size()) {
        throw ToolError(ExitCode::format, "COL sections do not consume the file exactly");
    }

    std::array<std::uint16_t, 3U> coarse_dimensions{};
    std::uint64_t expected_prefix_words = 1U;
    result.grid_cell_count = 1U;
    for (std::size_t axis = 0U; axis < result.grid_dimensions.size(); ++axis) {
        coarse_dimensions[axis] = static_cast<std::uint16_t>(
            result.grid_dimensions[axis] >> result.version);
        if (coarse_dimensions[axis] == 0U) {
            throw ToolError(ExitCode::format, "COL grid is smaller than its tree depth");
        }
        expected_prefix_words *= coarse_dimensions[axis];
        result.grid_cell_count *= result.grid_dimensions[axis];
    }
    if (expected_prefix_words != result.prefix_word_count) {
        throw ToolError(ExitCode::format, "COL grid prefix does not match its coarse dimensions");
    }
    result.grid_prefix.reserve(result.prefix_word_count);
    for (std::uint32_t index = 0U; index < result.prefix_word_count; ++index) {
        result.grid_prefix.push_back(u16(bytes, fixed_header_bytes + index * 2U));
    }

    const auto& node_section = result.sections[0];
    const auto& list_section = result.sections[1];
    if (node_section.count == 0U || list_section.count == 0U) {
        throw ToolError(ExitCode::format, "COL spatial tables omit their reserved record");
    }
    result.spatial_nodes.reserve(node_section.count);
    for (std::uint32_t index = 0U; index < node_section.count; ++index) {
        ColSpatialNode node;
        const auto offset = static_cast<std::size_t>(node_section.offset)
            + static_cast<std::size_t>(index) * node_section.stride;
        for (std::size_t child = 0U; child < node.children.size(); ++child) {
            node.children[child] = u16(bytes, offset + child * 2U);
        }
        result.spatial_nodes.push_back(node);
    }

    std::vector<bool> reached_nodes(node_section.count, false);
    std::set<std::uint16_t> reached_leaves;
    for (std::uint32_t x = 0U; x < result.grid_dimensions[0]; ++x) {
        for (std::uint32_t y = 0U; y < result.grid_dimensions[1]; ++y) {
            for (std::uint32_t z = 0U; z < result.grid_dimensions[2]; ++z) {
                const auto coarse_index =
                    ((static_cast<std::uint64_t>(x >> result.version)
                        * coarse_dimensions[1])
                        + (y >> result.version))
                    * coarse_dimensions[2]
                    + (z >> result.version);
                auto descriptor = result.grid_prefix[static_cast<std::size_t>(coarse_index)];
                auto level = result.version;
                while (descriptor != 0U && (descriptor & 0x8000U) == 0U) {
                    if (descriptor >= node_section.count || level == 0U) {
                        throw ToolError(ExitCode::format,
                            "COL spatial tree contains an invalid internal-node reference");
                    }
                    reached_nodes[descriptor] = true;
                    --level;
                    const auto child = static_cast<std::size_t>(
                        (((x >> level) & 1U) << 2U)
                        | (((y >> level) & 1U) << 1U)
                        | ((z >> level) & 1U));
                    descriptor = result.spatial_nodes[descriptor].children[child];
                }
                if (descriptor == 0U) {
                    ++result.empty_grid_cells;
                    continue;
                }
                ++result.occupied_grid_cells;
                const auto leaf = static_cast<std::uint16_t>(descriptor & 0x7fffU);
                if (leaf == 0U || leaf >= list_section.count) {
                    throw ToolError(ExitCode::format,
                        "COL spatial tree contains an invalid cell-list reference");
                }
                reached_leaves.insert(leaf);
            }
        }
    }
    for (std::uint32_t index = 1U; index < node_section.count; ++index) {
        if (!reached_nodes[index]) {
            throw ToolError(ExitCode::format, "COL spatial tree contains an unreachable node");
        }
        ++result.reachable_internal_nodes;
    }

    std::vector<bool> reached_faces(result.sections[2].count, false);
    std::uint32_t list_cursor = 1U;
    std::uint64_t decoded_spatial_references = 0U;
    for (const auto leaf : reached_leaves) {
        if (leaf != list_cursor) {
            throw ToolError(ExitCode::format, "COL cell-list table contains a gap");
        }
        const auto offset = static_cast<std::size_t>(list_section.offset)
            + static_cast<std::size_t>(leaf) * list_section.stride;
        const auto count = u16(bytes, offset);
        const auto dwords = static_cast<std::uint32_t>(count + 2U) / 2U;
        if (dwords > list_section.count - list_cursor) {
            throw ToolError(ExitCode::format, "COL cell list exceeds its table");
        }
        ColCellList list;
        list.dword_offset = leaf;
        list.face_indices.reserve(count);
        for (std::uint32_t index = 0U; index < count; ++index) {
            const auto face = u16(bytes, offset + 2U + index * 2U);
            if (face >= result.sections[2].count) {
                throw ToolError(ExitCode::format, "COL cell list references an invalid face");
            }
            list.face_indices.push_back(face);
            reached_faces[face] = true;
        }
        decoded_spatial_references += count;
        list_cursor += dwords;
        result.cell_lists.push_back(std::move(list));
    }
    result.leaf_list_count = static_cast<std::uint32_t>(result.cell_lists.size());
    if (list_cursor != list_section.count) {
        throw ToolError(ExitCode::format, "COL cell lists do not consume their table exactly");
    }
    if (decoded_spatial_references != result.spatial_reference_count) {
        throw ToolError(ExitCode::format, "COL cell-list total disagrees with the header");
    }
    for (const auto reached : reached_faces) {
        if (!reached) {
            throw ToolError(ExitCode::format, "COL face is unreachable from the spatial tree");
        }
        ++result.reachable_face_count;
    }

    const auto& face_section = result.sections[2];
    result.faces.reserve(face_section.count);
    for (std::uint32_t index = 0U; index < face_section.count; ++index) {
        const auto offset = static_cast<std::size_t>(face_section.offset)
            + static_cast<std::size_t>(index) * face_section.stride;
        ColFace face;
        face.surface = u16(bytes, offset);
        bool terminated = false;
        for (std::size_t boundary = 0U; boundary < face.boundary_references.size(); ++boundary) {
            const auto reference = u16(bytes, offset + 2U + boundary * 2U);
            face.boundary_references[boundary] = reference;
            if (reference != 0U) {
                ++result.nonzero_boundary_references;
                const auto decoded = static_cast<std::uint16_t>((reference - 1U) & 0x7fffU);
                result.maximum_boundary_reference =
                    std::max(result.maximum_boundary_reference, decoded);
                if (terminated) {
                    ++result.nonzero_boundary_padding;
                } else {
                    ++face.active_boundary_count;
                    ++result.active_boundary_references;
                    if ((reference & 0x8000U) != 0U) {
                        ++result.flipped_boundary_references;
                    }
                }
                if (terminated && decoded >= result.sections[3].count) {
                    ++result.boundary_references_outside_plane_table;
                }
            } else {
                terminated = true;
            }
        }
        for (std::size_t boundary = 0U; boundary < face.active_boundary_count; ++boundary) {
            const auto decoded = static_cast<std::uint16_t>(
                (face.boundary_references[boundary] - 1U) & 0x7fffU);
            if (decoded >= result.sections[3].count) {
                throw ToolError(ExitCode::format,
                    "COL active boundary references an invalid plane");
            }
        }
        for (std::size_t axis = 0U; axis < face.normal.size(); ++axis) {
            face.normal[axis] = f32(bytes, offset + 12U + axis * 4U);
            require_finite(face.normal[axis], "COL face normal is not finite");
        }
        face.distance = f32(bytes, offset + 24U);
        require_finite(face.distance, "COL face distance is not finite");
        result.faces.push_back(face);
    }

    const auto& plane_section = result.sections[3];
    result.planes.reserve(plane_section.count);
    for (std::uint32_t index = 0U; index < plane_section.count; ++index) {
        const auto offset = static_cast<std::size_t>(plane_section.offset)
            + static_cast<std::size_t>(index) * plane_section.stride;
        ColPlane plane;
        for (std::size_t axis = 0U; axis < plane.normal.size(); ++axis) {
            plane.normal[axis] = f32(bytes, offset + axis * 4U);
            require_finite(plane.normal[axis], "COL plane normal is not finite");
        }
        plane.distance = f32(bytes, offset + 12U);
        require_finite(plane.distance, "COL plane distance is not finite");
        result.planes.push_back(plane);
    }
    return result;
}

ColData read_col(const std::filesystem::path& path, const std::uint64_t maximum_file_bytes) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status)) {
        throw ToolError(ExitCode::input, "COL input is not a plain file");
    }
    const auto size = std::filesystem::file_size(path);
    if (size > maximum_file_bytes || size > std::numeric_limits<std::size_t>::max()) {
        throw ToolError(ExitCode::format, "COL file exceeds the configured size bound");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw ToolError(ExitCode::input, "short read while opening COL input");
    }
    return parse_col(bytes);
}

std::vector<ColPolygon> reconstruct_col_polygons(const ColData& collision) {
    std::vector<ColPolygon> polygons;
    polygons.reserve(collision.faces.size());
    for (std::size_t face_index = 0U; face_index < collision.faces.size(); ++face_index) {
        const auto& face = collision.faces[face_index];
        if (face.active_boundary_count < 3U) {
            throw ToolError(ExitCode::format, "COL face has fewer than three active boundaries");
        }
        struct Boundary {
            const ColPlane* plane = nullptr;
            double sign = 1.0;
        };
        std::vector<Boundary> boundaries;
        boundaries.reserve(face.active_boundary_count);
        for (std::size_t index = 0U; index < face.active_boundary_count; ++index) {
            const auto raw = face.boundary_references[index];
            const auto plane_index = static_cast<std::uint16_t>((raw - 1U) & 0x7fffU);
            if (plane_index >= collision.planes.size()) {
                throw ToolError(ExitCode::format, "COL polygon boundary is out of range");
            }
            boundaries.push_back(
                Boundary{&collision.planes[plane_index], (raw & 0x8000U) != 0U ? -1.0 : 1.0});
        }

        const auto face_normal = vector3d(face.normal);
        std::vector<Vector3d> candidates;
        for (std::size_t first = 0U; first < boundaries.size(); ++first) {
            for (std::size_t second = first + 1U; second < boundaries.size(); ++second) {
                const auto first_normal = vector3d(boundaries[first].plane->normal);
                const auto second_normal = vector3d(boundaries[second].plane->normal);
                const auto first_cross_second = cross(first_normal, second_normal);
                const auto determinant = dot(face_normal, first_cross_second);
                if (std::fabs(determinant) <= 1.0e-10) { continue; }
                const auto second_cross_face = cross(second_normal, face_normal);
                const auto face_cross_first = cross(face_normal, first_normal);
                Vector3d candidate{};
                for (std::size_t axis = 0U; axis < candidate.size(); ++axis) {
                    candidate[axis] = (-static_cast<double>(face.distance)
                            * first_cross_second[axis]
                        - static_cast<double>(boundaries[first].plane->distance)
                            * second_cross_face[axis]
                        - static_cast<double>(boundaries[second].plane->distance)
                            * face_cross_first[axis])
                        / determinant;
                }
                bool inside = true;
                for (const auto& boundary : boundaries) {
                    const auto value = dot(vector3d(boundary.plane->normal), candidate)
                        + boundary.plane->distance;
                    if (boundary.sign * value < -1.0e-3) {
                        inside = false;
                        break;
                    }
                }
                if (!inside) { continue; }
                const auto duplicate = std::any_of(candidates.begin(), candidates.end(),
                    [&](const Vector3d& existing) {
                        Vector3d delta{};
                        for (std::size_t axis = 0U; axis < delta.size(); ++axis) {
                            delta[axis] = existing[axis] - candidate[axis];
                        }
                        return dot(delta, delta) < 1.0e-8;
                    });
                if (!duplicate) { candidates.push_back(candidate); }
            }
        }
        if (candidates.size() < 3U) {
            throw ToolError(ExitCode::format, "COL face boundaries do not form a polygon");
        }

        Vector3d center{};
        for (const auto& candidate : candidates) {
            for (std::size_t axis = 0U; axis < center.size(); ++axis) {
                center[axis] += candidate[axis];
            }
        }
        for (auto& component : center) {
            component /= static_cast<double>(candidates.size());
        }
        const auto normal = normalized(face_normal);
        const Vector3d reference = std::fabs(normal[0]) < 0.9
            ? Vector3d{1.0, 0.0, 0.0}
            : Vector3d{0.0, 1.0, 0.0};
        const auto tangent = normalized(cross(reference, normal));
        const auto bitangent = cross(normal, tangent);
        std::sort(candidates.begin(), candidates.end(),
            [&](const Vector3d& left, const Vector3d& right) {
                Vector3d left_delta{};
                Vector3d right_delta{};
                for (std::size_t axis = 0U; axis < center.size(); ++axis) {
                    left_delta[axis] = left[axis] - center[axis];
                    right_delta[axis] = right[axis] - center[axis];
                }
                return std::atan2(dot(left_delta, bitangent), dot(left_delta, tangent))
                    < std::atan2(dot(right_delta, bitangent), dot(right_delta, tangent));
            });

        ColPolygon polygon;
        polygon.face_index = static_cast<std::uint16_t>(face_index);
        polygon.surface = face.surface;
        polygon.normal = {static_cast<float>(normal[0]), static_cast<float>(normal[1]),
            static_cast<float>(normal[2])};
        polygon.vertices.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            polygon.vertices.push_back({static_cast<float>(candidate[0]),
                static_cast<float>(candidate[1]), static_cast<float>(candidate[2])});
        }
        polygons.push_back(std::move(polygon));
    }
    return polygons;
}

} // namespace mh::content
