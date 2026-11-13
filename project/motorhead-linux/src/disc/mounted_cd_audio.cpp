#include <disc/cd_audio.hpp>
#include <stdexcept>

namespace mh::disc {
std::optional<MountedCddaDisc> find_mounted_motorhead_cdda() { return std::nullopt; }
std::optional<std::filesystem::path> find_mounted_motorhead_volume() { return std::nullopt; }
CddaPcmTrack read_mounted_cdda_track_pcm(const MountedCddaDisc &, int) {
  throw std::runtime_error("Use the packaged Music/trackNN.wav files on Steam Deck.");
}
}
