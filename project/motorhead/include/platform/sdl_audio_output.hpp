#pragma once

#include <SDL3/SDL_audio.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mh::platform {

inline std::vector<std::string> audio_output_names() {
  std::vector<std::string> names{std::string{}};
  int count = 0;
  const std::unique_ptr<SDL_AudioDeviceID, decltype(&SDL_free)> devices(
      SDL_GetAudioPlaybackDevices(&count), SDL_free);
  for (int index = 0; devices && index < count; ++index) {
    const auto *name = SDL_GetAudioDeviceName(devices.get()[index]);
    if (name != nullptr && *name != '\0' &&
        std::find(names.begin(), names.end(), name) == names.end()) {
      names.emplace_back(name);
    }
  }
  return names;
}

// Physical SDL IDs change between launches; persist the device name instead.
inline SDL_AudioDeviceID audio_output_device(const std::string_view name) {
  if (!name.empty()) {
    int count = 0;
    const std::unique_ptr<SDL_AudioDeviceID, decltype(&SDL_free)> devices(
        SDL_GetAudioPlaybackDevices(&count), SDL_free);
    for (int index = 0; devices && index < count; ++index) {
      const auto *candidate = SDL_GetAudioDeviceName(devices.get()[index]);
      if (candidate != nullptr && name == candidate) {
        return devices.get()[index];
      }
    }
  }
  return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
}

inline bool audio_output_format(const std::string_view name,
                                SDL_AudioSpec *spec, int *frames) {
  const auto device = audio_output_device(name);
  return SDL_GetAudioDeviceFormat(device, spec, frames) ||
         (device != SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK &&
          SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, spec,
                                   frames));
}

inline SDL_AudioDeviceID open_audio_output(const std::string_view name,
                                          const SDL_AudioSpec *spec) {
  const auto device = audio_output_device(name);
  auto opened = SDL_OpenAudioDevice(device, spec);
  if (opened == 0U && device != SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
    opened = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, spec);
  }
  return opened;
}

inline SDL_AudioStream *open_audio_output_stream(
    const std::string_view name, const SDL_AudioSpec *spec,
    SDL_AudioStreamCallback callback, void *userdata) {
  const auto device = audio_output_device(name);
  auto *stream = SDL_OpenAudioDeviceStream(device, spec, callback, userdata);
  if (stream == nullptr && device != SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, spec,
                                       callback, userdata);
  }
  return stream;
}

} // namespace mh::platform
