#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mh::disc {

struct CddaWavExport {
    int track_number = 0;
    std::uint32_t start_lba = 0U;
    std::uint32_t end_lba = 0U;
    std::uint64_t sector_count = 0U;
    std::uint64_t sample_frames = 0U;
    std::uint64_t pcm_bytes = 0U;
    std::uint64_t wav_bytes = 0U;
  std::string sha256;
};

struct CddaPcmTrack {
  int track_number = 0;
  std::uint32_t start_lba = 0U;
  std::uint32_t end_lba = 0U;
  std::uint32_t sample_rate = 44100U;
  std::uint16_t channels = 2U;
  std::uint16_t bits_per_sample = 16U;
  std::vector<std::uint8_t> pcm;
};

struct MountedCddaTrack {
  int track_number = 0;
  std::uint32_t start_lba = 0U;
  std::uint32_t end_lba = 0U;
  std::uint32_t duration_seconds = 0U;
};

struct MountedCddaDisc {
  std::filesystem::path root;
  std::vector<MountedCddaTrack> tracks;
};

// Returns a mounted MOTORHEAD CD only when the drive exposes the original
// audio tracks. A data-track-only ISO is deliberately rejected.
[[nodiscard]] std::optional<MountedCddaDisc>
find_mounted_motorhead_cdda();

// Finds the original MOTORHEAD data volume even when a native Windows ISO
// mount cannot expose the mixed-mode disc's audio TOC.
[[nodiscard]] std::optional<std::filesystem::path>
find_mounted_motorhead_volume();

[[nodiscard]] CddaPcmTrack
read_mounted_cdda_track_pcm(const MountedCddaDisc &disc, int track_number);

[[nodiscard]] CddaPcmTrack
read_cdda_track_pcm(const std::filesystem::path &cue_path, int track_number);

[[nodiscard]] CddaWavExport export_cdda_track_wav(
    const std::filesystem::path& cue_path, int track_number,
    const std::filesystem::path& output, bool overwrite);

} // namespace mh::disc
