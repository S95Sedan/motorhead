#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mh::disc {

enum class TrackType {
    mode1_2352,
    audio,
};

struct CueIndex {
    int number = 0;
    std::uint32_t lba = 0;
};

struct CueTrack {
    int number = 0;
    TrackType type = TrackType::audio;
    std::vector<CueIndex> indices;
};

struct CueSheet {
    std::filesystem::path cue_path;
    std::filesystem::path binary_path;
    std::string binary_type;
    std::vector<CueTrack> tracks;
};

[[nodiscard]] CueSheet parse_cue(const std::filesystem::path& path);
[[nodiscard]] std::optional<std::uint32_t> find_index_lba(const CueTrack& track, int index_number);
[[nodiscard]] std::string track_type_name(TrackType type);

} // namespace mh::disc

