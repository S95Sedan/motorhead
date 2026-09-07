#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <span>
#include <vector>

namespace mh::content {

struct AiRouteSample {
    std::array<float, 2U> position{};
    std::array<float, 2U> surface_values{};
    std::array<float, 3U> direction{};
    float signed_turn_radius = 0.0F;
    std::uint32_t group_index = 0U;
};

struct AiRouteGroup {
    std::uint32_t index = 0U;
    std::uint32_t declared_sample_count = 0U;
    float signed_turn_radius = 0.0F;
    std::uint32_t observed_sample_count = 0U;
    std::uint32_t first_sample = std::numeric_limits<std::uint32_t>::max();
};

struct AiRouteData {
    std::uint64_t file_bytes = 0U;
    std::vector<AiRouteSample> samples;
    std::vector<AiRouteGroup> groups;
    float minimum_direction_length = 0.0F;
    float maximum_direction_length = 0.0F;
    std::uint32_t metadata_count_mismatches = 0U;
    std::uint32_t straight_sample_count = 0U;
    std::uint32_t straight_group_count = 0U;
    std::uint32_t curved_group_count = 0U;
    std::uint32_t zero_group_with_curved_samples_count = 0U;
    std::uint32_t curved_group_mixed_sign_count = 0U;
    std::uint64_t turn_radius_comparison_count = 0U;
    std::uint64_t turn_radius_relation_within_ten_percent = 0U;
    double mean_turn_radius_relation = 0.0;
    double mean_group_target_error = 0.0;
    float maximum_group_target_error = 0.0F;
};

[[nodiscard]] AiRouteData parse_ai_route(std::span<const std::uint8_t> bytes);
[[nodiscard]] AiRouteData read_ai_route(
    const std::filesystem::path& path,
    std::uint64_t maximum_file_bytes = 64ULL * 1024ULL * 1024ULL);

} // namespace mh::content
