#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mh::content {

struct TrackReference {
  std::string field;
  std::string value;
};

// Names are retained from the original TRK source comment. Their runtime
// placement equation remains deliberately unrecovered.
struct TrackStartGrid {
  float row = 0.0F;
  float goal = 0.0F;
  float space = 0.0F;
  float tint = 0.0F;
};

struct TrackRgbColor {
  std::uint8_t red = 0U;
  std::uint8_t green = 0U;
  std::uint8_t blue = 0U;
};

struct TrackEnvironment {
  std::optional<std::uint32_t> view_distance;
  std::optional<TrackRgbColor> top_color;
  std::optional<TrackRgbColor> bottom_color;
  std::optional<float> horizon;
  std::optional<std::array<float, 3U>> world_brightness;
  std::optional<std::array<float, 3U>> object_brightness;
  std::optional<std::array<float, 3U>> accelerated_brightness;
  std::optional<std::array<float, 3U>> object_ambient;
  std::optional<std::array<float, 3U>> cue_color;
  std::optional<float> cue_start;
  std::optional<float> specular_factor;
  std::optional<float> light_intensity;
};

struct TrackDefinition {
  std::string name;
  std::string base_path;
  std::string world_filename;
  std::uint32_t division = 0U;
  std::size_t field_count = 0U;
  std::vector<TrackReference> references;
  std::optional<TrackStartGrid> start_grid;
  TrackEnvironment environment;
};

[[nodiscard]] TrackDefinition
parse_track_definition(std::span<const std::uint8_t> decoded_text);
[[nodiscard]] TrackDefinition
read_track_definition(const std::filesystem::path &path,
                      std::uint64_t maximum_file_bytes = 1024ULL * 1024ULL);

} // namespace mh::content
