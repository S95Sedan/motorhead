#include <disc/cd_audio.hpp>

#include <core/error.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <ntddcdrm.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace mh::disc {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::uint32_t cdda_sector_bytes = 2352U;
constexpr std::uint32_t logical_sector_bytes = 2048U;
constexpr std::uint32_t cdda_frames_per_second = 75U;
constexpr std::uint32_t cdda_lead_in_frames = 150U;

struct HandleCloser {
  void operator()(void *handle) const noexcept {
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(handle);
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

std::uint32_t address_lba(const TRACK_DATA &track) {
  const auto absolute =
      (static_cast<std::uint32_t>(track.Address[1]) * 60U +
       static_cast<std::uint32_t>(track.Address[2])) *
          cdda_frames_per_second +
      static_cast<std::uint32_t>(track.Address[3]);
  return absolute >= cdda_lead_in_frames
             ? absolute - cdda_lead_in_frames
             : 0U;
}

std::wstring device_path(const std::filesystem::path &root) {
  auto text = root.wstring();
  if (text.size() < 2U || text[1] != L':') {
    return {};
  }
  return L"\\\\.\\" + text.substr(0U, 2U);
}

UniqueHandle open_drive(const std::filesystem::path &root) {
  const auto device = device_path(root);
  if (device.empty()) {
    return UniqueHandle(nullptr);
  }
  return UniqueHandle(CreateFileW(device.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                  nullptr));
}

std::optional<MountedCddaDisc>
inspect_drive(const std::filesystem::path &root) {
  auto handle = open_drive(root);
  if (handle.get() == INVALID_HANDLE_VALUE || handle.get() == nullptr) {
    return std::nullopt;
  }
  CDROM_TOC toc{};
  DWORD returned = 0U;
  if (!DeviceIoControl(handle.get(), IOCTL_CDROM_READ_TOC, nullptr, 0U, &toc,
                       sizeof(toc), &returned, nullptr) ||
      returned < sizeof(toc.Length) + sizeof(toc.FirstTrack) +
                     sizeof(toc.LastTrack)) {
    return std::nullopt;
  }
  if (toc.FirstTrack > toc.LastTrack) {
    return std::nullopt;
  }
  const auto track_count = static_cast<std::size_t>(
      static_cast<unsigned int>(toc.LastTrack) -
      static_cast<unsigned int>(toc.FirstTrack) + 1U);
  if (track_count + 1U > std::size(toc.TrackData)) {
    return std::nullopt;
  }

  MountedCddaDisc result;
  result.root = root;
  for (std::size_t index = 0U; index < track_count; ++index) {
    const auto &entry = toc.TrackData[index];
    // Control bit 2 identifies a data track; clear means CDDA audio.
    if ((entry.Control & 0x04U) != 0U) {
      continue;
    }
    const auto start = address_lba(entry);
    const auto end = address_lba(toc.TrackData[index + 1U]);
    if (start >= end) {
      continue;
    }
    result.tracks.push_back(
        {static_cast<int>(entry.TrackNumber), start, end,
         (end - start) / cdda_frames_per_second});
  }
  auto has_retail_audio_set = true;
  for (auto number = 2; number <= 11; ++number) {
    has_retail_audio_set =
        has_retail_audio_set &&
        std::any_of(result.tracks.begin(), result.tracks.end(),
                    [number](const MountedCddaTrack &track) {
                      return track.track_number == number;
                    });
  }
  if (!has_retail_audio_set) {
    return std::nullopt;
  }
  const auto probe_track =
      std::find_if(result.tracks.begin(), result.tracks.end(),
                   [](const MountedCddaTrack &track) {
                     return track.track_number == 3;
                   });
  std::array<std::uint8_t, cdda_sector_bytes> probe{};
  RAW_READ_INFO request{};
  request.DiskOffset.QuadPart =
      static_cast<LONGLONG>(probe_track->start_lba) * logical_sector_bytes;
  request.SectorCount = 1U;
  request.TrackMode = CDDA;
  DWORD probe_bytes = 0U;
  if (!DeviceIoControl(handle.get(), IOCTL_CDROM_RAW_READ, &request,
                       sizeof(request), probe.data(),
                       static_cast<DWORD>(probe.size()), &probe_bytes,
                       nullptr) ||
      probe_bytes != static_cast<DWORD>(probe.size())) {
    return std::nullopt;
  }
  return result;
}

} // namespace

std::optional<MountedCddaDisc> find_mounted_motorhead_cdda() {
  const auto drives = GetLogicalDrives();
  for (unsigned int index = 0U; index < 26U; ++index) {
    if ((drives & (1U << index)) == 0U) {
      continue;
    }
    const std::wstring root{static_cast<wchar_t>(L'A' + index), L':', L'\\'};
    if (GetDriveTypeW(root.c_str()) != DRIVE_CDROM) {
      continue;
    }
    std::array<wchar_t, MAX_PATH + 1U> label{};
    if (!GetVolumeInformationW(root.c_str(), label.data(),
                               static_cast<DWORD>(label.size()), nullptr,
                               nullptr, nullptr, nullptr, 0U) ||
        _wcsicmp(label.data(), L"MOTORHEAD") != 0) {
      continue;
    }
    if (auto disc = inspect_drive(root); disc.has_value()) {
      return disc;
    }
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> find_mounted_motorhead_volume() {
  const auto drives = GetLogicalDrives();
  for (unsigned int index = 0U; index < 26U; ++index) {
    if ((drives & (1U << index)) == 0U) {
      continue;
    }
    std::filesystem::path root(
        std::wstring{static_cast<wchar_t>(L'A' + index), L':', L'\\'});
    if (GetDriveTypeW(root.c_str()) != DRIVE_CDROM) {
      continue;
    }
    std::array<wchar_t, MAX_PATH + 1U> label{};
    if (!GetVolumeInformationW(root.c_str(), label.data(),
                               static_cast<DWORD>(label.size()), nullptr,
                               nullptr, nullptr, nullptr, 0U) ||
        _wcsicmp(label.data(), L"MOTORHEAD") != 0) {
      continue;
    }
    if (std::filesystem::is_regular_file(root / "AUTORUN.INF") &&
        std::filesystem::is_regular_file(root / "MOTOR" / "DISK1" /
                                         "DATA1.CAB")) {
      return root;
    }
  }
  return std::nullopt;
}

CddaPcmTrack read_mounted_cdda_track_pcm(const MountedCddaDisc &disc,
                                         const int track_number) {
  const auto selected =
      std::find_if(disc.tracks.begin(), disc.tracks.end(),
                   [track_number](const MountedCddaTrack &track) {
                     return track.track_number == track_number;
                   });
  if (selected == disc.tracks.end()) {
    throw ToolError(ExitCode::usage,
                    "requested mounted CD track is absent or is not audio");
  }
  auto handle = open_drive(disc.root);
  if (handle.get() == INVALID_HANDLE_VALUE || handle.get() == nullptr) {
    throw ToolError(ExitCode::input, "could not open mounted CD audio drive");
  }
  const auto sector_count = selected->end_lba - selected->start_lba;
  const auto byte_count = static_cast<std::uint64_t>(sector_count) *
                          static_cast<std::uint64_t>(cdda_sector_bytes);
  if (byte_count > std::numeric_limits<std::size_t>::max()) {
    throw ToolError(ExitCode::format, "mounted CD audio track is too large");
  }

  CddaPcmTrack result;
  result.track_number = track_number;
  result.start_lba = selected->start_lba;
  result.end_lba = selected->end_lba;
  result.pcm.resize(static_cast<std::size_t>(byte_count));
  constexpr std::uint32_t chunk_sectors = 32U;
  std::uint32_t completed = 0U;
  while (completed < sector_count) {
    const auto count = std::min(chunk_sectors, sector_count - completed);
    RAW_READ_INFO request{};
    request.DiskOffset.QuadPart =
        static_cast<LONGLONG>(selected->start_lba + completed) *
        logical_sector_bytes;
    request.SectorCount = count;
    request.TrackMode = CDDA;
    DWORD returned = 0U;
    const auto offset = static_cast<std::size_t>(completed) * cdda_sector_bytes;
    const auto expected = count * cdda_sector_bytes;
    if (!DeviceIoControl(handle.get(), IOCTL_CDROM_RAW_READ, &request,
                         sizeof(request), result.pcm.data() + offset, expected,
                         &returned, nullptr) ||
        returned != expected) {
      throw ToolError(ExitCode::input,
                      "short read while reading mounted CD audio track");
    }
    completed += count;
  }
  return result;
}

} // namespace mh::disc
