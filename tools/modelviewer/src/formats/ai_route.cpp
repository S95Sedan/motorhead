#include <formats/ai_route.hpp>

#include <core/error.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::size_t sample_bytes = 36U;
constexpr std::size_t group_bytes = 12U;
constexpr std::uint32_t maximum_group_count = 99U;

std::uint32_t u32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) {
        throw ToolError(ExitCode::format, "AI route field exceeds file bounds");
    }
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

float f32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    const auto value = std::bit_cast<float>(u32(bytes, offset));
    if (!std::isfinite(value)) {
        throw ToolError(ExitCode::format, "AI route contains a non-finite float");
    }
    return value;
}

std::vector<std::uint8_t> read_bytes(
    const std::filesystem::path& path, const std::uint64_t maximum_file_bytes) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size > maximum_file_bytes
        || size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw ToolError(ExitCode::input, "AI route file is missing or exceeds the size limit");
    }
    std::ifstream input(path, std::ios::binary);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !bytes.empty()) {
        throw ToolError(ExitCode::input, "failed to read AI route file");
    }
    return bytes;
}

} // namespace

AiRouteData parse_ai_route(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 4U) {
        throw ToolError(ExitCode::format, "AI route header is truncated");
    }
    const auto sample_count = u32(bytes, 0U);
    if (sample_count == 0U
        || sample_count > (bytes.size() - 4U) / sample_bytes) {
        throw ToolError(ExitCode::format, "AI route sample count exceeds file bounds");
    }
    const auto group_count_offset = 4U
        + static_cast<std::size_t>(sample_count) * sample_bytes;
    if (group_count_offset > bytes.size() || bytes.size() - group_count_offset < 4U) {
        throw ToolError(ExitCode::format, "AI route group header is truncated");
    }
    const auto group_count = u32(bytes, group_count_offset);
    if (group_count == 0U || group_count > maximum_group_count) {
        throw ToolError(ExitCode::format, "AI route group count is outside the retail bound");
    }
    const auto expected_size = group_count_offset + 4U
        + static_cast<std::size_t>(group_count) * group_bytes;
    if (expected_size != bytes.size()) {
        throw ToolError(ExitCode::format, "AI route records do not consume the complete file");
    }

    AiRouteData result;
    result.file_bytes = bytes.size();
    result.samples.reserve(sample_count);
    result.groups.resize(group_count);
    result.minimum_direction_length = std::numeric_limits<float>::infinity();
    std::uint32_t previous_group = 0U;
    for (std::uint32_t index = 0U; index < sample_count; ++index) {
        const auto base = 4U + static_cast<std::size_t>(index) * sample_bytes;
        AiRouteSample sample;
        sample.position = {f32(bytes, base), f32(bytes, base + 4U)};
        sample.surface_values = {f32(bytes, base + 8U), f32(bytes, base + 12U)};
        sample.direction = {
            f32(bytes, base + 16U), f32(bytes, base + 20U), f32(bytes, base + 24U)};
        sample.signed_turn_radius = f32(bytes, base + 28U);
        if (sample.signed_turn_radius == 0.0F) { ++result.straight_sample_count; }
        sample.group_index = u32(bytes, base + 32U);
        if (sample.group_index >= group_count
            || (index != 0U && sample.group_index < previous_group)) {
            throw ToolError(ExitCode::format,
                "AI route sample group indices are out of range or unordered");
        }
        previous_group = sample.group_index;
        const auto direction_length = std::sqrt(
            sample.direction[0U] * sample.direction[0U]
            + sample.direction[1U] * sample.direction[1U]
            + sample.direction[2U] * sample.direction[2U]);
        if (!std::isfinite(direction_length)
            || direction_length < 0.9F || direction_length > 1.1F) {
            throw ToolError(ExitCode::format, "AI route direction is not normalized");
        }
        result.minimum_direction_length = std::min(
            result.minimum_direction_length, direction_length);
        result.maximum_direction_length = std::max(
            result.maximum_direction_length, direction_length);
        auto& group = result.groups[sample.group_index];
        if (group.first_sample == std::numeric_limits<std::uint32_t>::max()) {
            group.first_sample = index;
        }
        ++group.observed_sample_count;
        result.samples.push_back(sample);
    }

    auto offset = group_count_offset + 4U;
    for (std::uint32_t index = 0U; index < group_count; ++index) {
        auto& group = result.groups[index];
        group.index = u32(bytes, offset);
        group.declared_sample_count = u32(bytes, offset + 4U);
        group.signed_turn_radius = f32(bytes, offset + 8U);
        if (group.index != index) {
            throw ToolError(ExitCode::format, "AI route metadata group indices are not sequential");
        }
        if (group.declared_sample_count != group.observed_sample_count) {
            ++result.metadata_count_mismatches;
        }
        offset += group_bytes;
    }

    double relation_sum = 0.0;
    for (std::size_t index = 0U; index + 1U < result.samples.size(); ++index) {
        const auto& first = result.samples[index];
        const auto& second = result.samples[index + 1U];
        if (first.group_index != second.group_index
            || std::abs(first.signed_turn_radius) < 0.001F) {
            continue;
        }
        const auto delta_x = static_cast<double>(second.position[0] - first.position[0]);
        const auto delta_z = static_cast<double>(second.position[1] - first.position[1]);
        const auto travel = std::sqrt(delta_x * delta_x + delta_z * delta_z);
        if (travel <= 1.0e-7) { continue; }
        const auto cross = static_cast<double>(first.direction[0]) * second.direction[2]
            - static_cast<double>(first.direction[2]) * second.direction[0];
        const auto dot = static_cast<double>(first.direction[0]) * second.direction[0]
            + static_cast<double>(first.direction[2]) * second.direction[2];
        const auto heading_delta = std::atan2(cross, dot);
        const auto relation = heading_delta * first.signed_turn_radius / travel;
        if (!std::isfinite(relation)) { continue; }
        relation_sum += relation;
        ++result.turn_radius_comparison_count;
        if (std::abs(relation + 1.0) <= 0.1) {
            ++result.turn_radius_relation_within_ten_percent;
        }
    }
    if (result.turn_radius_comparison_count != 0U) {
        result.mean_turn_radius_relation = relation_sum
            / static_cast<double>(result.turn_radius_comparison_count);
    }

    double group_error_sum = 0.0;
    for (const auto& group : result.groups) {
        if (group.signed_turn_radius == 0.0F) {
            ++result.straight_group_count;
            for (const auto& sample : result.samples) {
                if (sample.group_index == group.index
                    && sample.signed_turn_radius != 0.0F) {
                    ++result.zero_group_with_curved_samples_count;
                    break;
                }
            }
            continue;
        }
        ++result.curved_group_count;
        auto nearest_error = std::numeric_limits<float>::infinity();
        bool mixed_sign = false;
        for (const auto& sample : result.samples) {
            if (sample.group_index != group.index
                || sample.signed_turn_radius == 0.0F) {
                continue;
            }
            mixed_sign = mixed_sign
                || std::signbit(sample.signed_turn_radius)
                    != std::signbit(group.signed_turn_radius);
            nearest_error = std::min(nearest_error,
                std::abs(sample.signed_turn_radius - group.signed_turn_radius));
        }
        if (mixed_sign) { ++result.curved_group_mixed_sign_count; }
        if (std::isfinite(nearest_error)) {
            group_error_sum += nearest_error;
            result.maximum_group_target_error = std::max(
                result.maximum_group_target_error, nearest_error);
        }
    }
    if (result.curved_group_count != 0U) {
        result.mean_group_target_error = group_error_sum
            / static_cast<double>(result.curved_group_count);
    }
    return result;
}

AiRouteData read_ai_route(
    const std::filesystem::path& path, const std::uint64_t maximum_file_bytes) {
    const auto bytes = read_bytes(path, maximum_file_bytes);
    return parse_ai_route(bytes);
}

} // namespace mh::content
