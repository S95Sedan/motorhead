#include "retail_install.hpp"

#include <windows.h>

#include <cabinet.hpp>
#include <content/formats/tbl_envelope.hpp>
#include <core/crypto/sha256.hpp>
#include <disc/cd_audio.hpp>
#include <disc/cue_sheet.hpp>
#include <disc/iso9660.hpp>
#include <libunshield.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>

namespace motorhead {
namespace {

constexpr DWORD ioctl_cdrom_read_toc = 0x00024000U;
constexpr DWORD ioctl_cdrom_raw_read = 0x0002403EU;
constexpr std::size_t raw_sector_bytes = 2352U;
constexpr std::size_t cooked_sector_bytes = 2048U;
constexpr std::size_t wave_header_bytes = 44U;
constexpr int resource_default_league = 103;
constexpr int resource_motorhead_executable = 104;
constexpr int resource_default_configuration = 105;
constexpr int resource_menu_dial = 106;
constexpr std::uintmax_t menu_dial_size = 44937U;
constexpr std::string_view menu_dial_sha256 =
    "ee0849f61733715e454646f0b166abef207dc02e1bf09047dc4b459092429fbd";
constexpr std::uintmax_t motorhead_patch_size = 14025656U;
constexpr std::string_view motorhead_patch_sha256 =
    "3dce2edb9fdbeba76ee66616a65d93b19a7cffe93bf473fb05b12ac6e14fc5f4";
constexpr std::uintmax_t motorhead_patch_cabinet_size = 13560443U;
constexpr std::string_view motorhead_patch_cabinet_sha256 =
    "2430c8ee656980497e6006fb3f7535975abf7f8304d1e3d072b9c69ff88aafd8";
constexpr std::uintmax_t s40_cabinet_size = 5712248U;
constexpr std::string_view s40_cabinet_sha256 =
    "181bc7c35ec8e59cda28ff523d025fe8e8b5f96af4c20c0f643f8fd5a52ef090";
constexpr std::uintmax_t s40_movies_size = 11766960U;
constexpr std::string_view s40_movies_sha256 =
    "1ff70f47b2a37dc3f71af6c72220d1cb7e31c2d1cfa278a9cf34a913d7ddaa10";
constexpr std::uintmax_t s40_preview_size = 848694U;
constexpr std::string_view s40_preview_sha256 =
    "3082d9babd0a5946069e66a2e7a93e65c6d1f72680a86a8250a6b1c449ce2481";
// S40 builds these constants at runtime; this bridges them to Motorhead's CAR
// format and installed Car99 paths.
constexpr std::string_view s40_car_definition = R"car(carname Volvo S40
division 310
filename Cars\Car99\car99
texturepath Cars\Car99
collisionname Cars\Car99\car99.COL
halo Cars\Textures\halowhit.iff
halo Cars\Textures\haloblig.iff
colour1 255 255 255
colour2 210 24 20
colour3 32 32 32
shadowpoints1 -0.793931 0 2.051248 -0.484105 0 2.278390 0.484105 0 2.278390 0.793931 0 2.051248
shadowpoints2 -0.968209 0 1.460679 -0.484105 0 1.460679 0.484105 0 1.460679 0.968209 0 1.460679
shadowpoints3 -0.968209 0 -1.446739 -0.484105 0 -1.446739 0.484105 0 -1.446739 0.968209 0 -1.446739
shadowpoints4 -0.793931 0 -2.037308 -0.484105 0 -2.264450 0.484105 0 -2.264450 0.793931 0 -2.037308
phyweight 1350
phygearratio 50 80 130 180 230 300
phyminrpm 1000
phymaxrpm 10000
phyaccforce 2800
phybrakeforce 0.65
phyturnforce 12000
physpringlength 0.275
physpringstrength 300000
phycornerfl -0.968 0.20 2.20
phycornerfr 0.968 0.20 2.20
phycornerbl -0.968 0.20 -2.20
phycornerbr 0.968 0.20 -2.20
phycentermass 0 0.55 -0.10
wheel1 Cars\Car99\car99lf Cars\Car99
wheel2 Cars\Car99\car99rf Cars\Car99
wheel3 Cars\Car99\car99rb Cars\Car99
wheel4 Cars\Car99\car99lb Cars\Car99
phywheelfl -0.744 0.55 1.337174
phywheelfr 0.744 0.55 1.337174
phywheelbr 0.807 0.55 -1.263756
phywheelbl -0.807 0.55 -1.263756
speedlevel 4
acclevel 4
griplevel 6
topspeed 273
acceleration 2.84
handling 6
)car";

constexpr std::array<const wchar_t *, 10U> managed_content_directories{
    L"AddressBook", L"Cars",   L"Data",  L"Demos",  L"Game",
    L"League",      L"Movies", L"Music", L"Sounds", L"Tracks"};
constexpr std::array<const wchar_t *, 2U> managed_content_files{
    L"motor.txt", L"SMACKW32.DLL"};

struct CueMedia {
  mh::disc::CueSheet cue;
  mh::disc::IsoReport iso;
  std::uint32_t data_lba = 0U;
};

CueMedia read_motorhead_cue(const std::filesystem::path &cue_path) {
  auto cue = mh::disc::parse_cue(cue_path);
  const auto data =
      std::find_if(cue.tracks.begin(), cue.tracks.end(), [](const auto &track) {
        return track.number == 1 &&
               track.type == mh::disc::TrackType::mode1_2352;
      });
  if (data == cue.tracks.end()) {
    throw std::runtime_error("The selected CUE has no MODE1/2352 data track 1");
  }
  const auto data_lba = mh::disc::find_index_lba(*data, 1);
  if (!data_lba.has_value()) {
    throw std::runtime_error("The selected CUE data track has no INDEX 01");
  }
  for (int number = 2; number <= 11; ++number) {
    const auto audio = std::find_if(
        cue.tracks.begin(), cue.tracks.end(), [number](const auto &track) {
          return track.number == number &&
                 track.type == mh::disc::TrackType::audio &&
                 mh::disc::find_index_lba(track, 1).has_value();
        });
    if (audio == cue.tracks.end()) {
      throw std::runtime_error(
          "The selected CUE does not contain retail audio tracks 2 to 11");
    }
  }
  auto iso = mh::disc::read_iso9660(cue.binary_path, *data_lba);
  if (_stricmp(iso.volume.volume_id.c_str(), "MOTORHEAD") != 0) {
    throw std::runtime_error(
        "The selected CUE/BIN is not the retail Motorhead disc");
  }
  const auto has_entry = [&iso](const char *expected) {
    return std::any_of(iso.entries.begin(), iso.entries.end(),
                       [expected](const auto &entry) {
                         return !entry.is_directory &&
                                _stricmp(entry.path.c_str(), expected) == 0;
                       });
  };
  if (!has_entry("MOTOR/DISK1/DATA1.CAB") || !has_entry("MOVIES/MOVIES.PAK")) {
    throw std::runtime_error(
        "The selected image is missing the retail Motorhead install data");
  }
  return {std::move(cue), std::move(iso), *data_lba};
}

CueMedia read_s40_cue(const std::filesystem::path &cue_path) {
  auto cue = mh::disc::parse_cue(cue_path);
  const auto data =
      std::find_if(cue.tracks.begin(), cue.tracks.end(), [](const auto &track) {
        return track.number == 1 &&
               track.type == mh::disc::TrackType::mode1_2352;
      });
  if (data == cue.tracks.end()) {
    throw std::runtime_error(
        "The selected S40 Racing CUE has no MODE1/2352 data track 1");
  }
  const auto data_lba = mh::disc::find_index_lba(*data, 1);
  if (!data_lba.has_value()) {
    throw std::runtime_error(
        "The selected S40 Racing CUE data track has no INDEX 01");
  }
  for (int number = 2; number <= 4; ++number) {
    const auto audio = std::find_if(
        cue.tracks.begin(), cue.tracks.end(), [number](const auto &track) {
          return track.number == number &&
                 track.type == mh::disc::TrackType::audio &&
                 mh::disc::find_index_lba(track, 1).has_value();
        });
    if (audio == cue.tracks.end()) {
      throw std::runtime_error(
          "The selected S40 Racing CUE does not contain audio tracks 2 to 4");
    }
  }
  auto iso = mh::disc::read_iso9660(cue.binary_path, *data_lba);
  if (_stricmp(iso.volume.volume_id.c_str(), "S40_Racing") != 0) {
    throw std::runtime_error(
        "The selected CUE/BIN is not the supported S40 Racing disc");
  }
  const auto cabinet = std::find_if(
      iso.entries.begin(), iso.entries.end(), [](const auto &entry) {
        return !entry.is_directory &&
               _stricmp(entry.path.c_str(), "S40/DISK1/DATA1.CAB") == 0;
      });
  if (cabinet == iso.entries.end() || cabinet->size != s40_cabinet_size) {
    throw std::runtime_error(
        "The selected image is missing the supported S40 Racing install data");
  }
  const auto movies = std::find_if(
      iso.entries.begin(), iso.entries.end(), [](const auto &entry) {
        return !entry.is_directory &&
               _stricmp(entry.path.c_str(), "MOVIES/MOVIES.PAK") == 0;
      });
  if (movies == iso.entries.end() || movies->size != s40_movies_size) {
    throw std::runtime_error(
        "The selected image is missing the supported S40 Racing intro movie");
  }
  const auto preview = std::find_if(
      iso.entries.begin(), iso.entries.end(), [](const auto &entry) {
        return !entry.is_directory &&
               _stricmp(entry.path.c_str(), "MENU/S40NY.BMP") == 0;
      });
  if (preview == iso.entries.end() || preview->size != s40_preview_size) {
    throw std::runtime_error(
        "The selected image is missing the supported S40 Racing car artwork");
  }
  return {std::move(cue), std::move(iso), *data_lba};
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    std::array<wchar_t, MAX_PATH + 1U> parent{};
    if (GetTempPathW(static_cast<DWORD>(parent.size()), parent.data()) == 0U) {
      throw std::runtime_error("Could not locate the temporary directory");
    }
    std::array<wchar_t, MAX_PATH + 1U> candidate{};
    if (GetTempFileNameW(parent.data(), L"MHI", 0U, candidate.data()) == 0U ||
        DeleteFileW(candidate.data()) == FALSE ||
        CreateDirectoryW(candidate.data(), nullptr) == FALSE) {
      throw std::runtime_error(
          "Could not create a temporary directory for the disc image");
    }
    path_ = candidate.data();
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::filesystem::path
extract_patch_cabinet(const std::filesystem::path &patch_path,
                      const std::filesystem::path &temporary_root) {
  const auto outer = mh::cab::find_embedded_cabinet(patch_path);
  static_cast<void>(mh::cab::extract_cabinet(outer, temporary_root, false));
  const auto cabinet = temporary_root / "disk1" / "data1.cab";
  std::error_code error;
  if (!std::filesystem::is_regular_file(cabinet, error) || error ||
      std::filesystem::file_size(cabinet, error) !=
          motorhead_patch_cabinet_size ||
      error ||
      mh::common::sha256_file(cabinet) != motorhead_patch_cabinet_sha256) {
    throw std::runtime_error(
        "The verified mhp30.exe contained an unexpected update cabinet");
  }
  return cabinet;
}

std::filesystem::path checked_relative_path(std::string value) {
  std::replace(value.begin(), value.end(), '\\', '/');
  const std::filesystem::path result(value);
  if (result.empty() || result.is_absolute() || result.has_root_path()) {
    throw std::runtime_error("Retail cabinet contains an unsafe path");
  }
  for (const auto &component : result) {
    if (component == ".." || component == ".") {
      throw std::runtime_error("Retail cabinet contains path traversal");
    }
  }
  return result;
}

std::vector<char> read_stock_configuration(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error(
        "The original configuration baseline is missing: " + path.string());
  }
  std::vector<char> result{std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>()};
  // istreambuf_iterator reaches the stream buffer's end without necessarily
  // setting basic_istream::eofbit. Only a badbit indicates an actual read
  // failure here.
  if (result.empty() || stream.bad()) {
    throw std::runtime_error(
        "The original configuration baseline could not be read: " +
        path.string());
  }
  return result;
}

bool files_have_identical_contents(const std::filesystem::path &left,
                                   const std::filesystem::path &right) {
  std::error_code error;
  const auto left_size = std::filesystem::file_size(left, error);
  if (error) {
    return false;
  }
  const auto right_size = std::filesystem::file_size(right, error);
  if (error || left_size != right_size) {
    return false;
  }
  std::ifstream left_stream(left, std::ios::binary);
  std::ifstream right_stream(right, std::ios::binary);
  return left_stream && right_stream &&
         std::equal(std::istreambuf_iterator<char>(left_stream),
                    std::istreambuf_iterator<char>(),
                    std::istreambuf_iterator<char>(right_stream));
}

bool configuration_has_key(const std::vector<char> &configuration,
                           const std::string_view expected_key) {
  const std::string_view text(configuration.data(), configuration.size());
  for (std::size_t begin = 0U; begin < text.size();) {
    auto end = text.find('\n', begin);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    auto line = text.substr(begin, end - begin);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' ||
                             line.front() == '\r')) {
      line.remove_prefix(1U);
    }
    if (!line.empty() && line.front() != '#' && line.front() != ';' &&
        !line.starts_with("//")) {
      const auto token_end = line.find_first_of(" \t\r");
      auto token = std::string(line.substr(0U, token_end));
      std::transform(token.begin(), token.end(), token.begin(),
                     [](const unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                     });
      if (token == expected_key) {
        return true;
      }
    }
    begin = end == text.size() ? text.size() : end + 1U;
  }
  return false;
}

std::size_t configuration_display_settings_insert_position(
    const std::vector<char> &configuration, const std::size_t logical_end) {
  const std::string_view text(configuration.data(), logical_end);
  bool visual_section = false;
  for (std::size_t begin = 0U; begin < text.size();) {
    auto end = text.find('\n', begin);
    const auto has_newline = end != std::string_view::npos;
    if (!has_newline) {
      end = text.size();
    }
    auto line = text.substr(begin, end - begin);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' ||
                             line.front() == '\r')) {
      line.remove_prefix(1U);
    }
    while (!line.empty() &&
           (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
      line.remove_suffix(1U);
    }

    if (line.starts_with("//")) {
      line.remove_prefix(2U);
      while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1U);
      }
      auto section = std::string(line);
      std::transform(section.begin(), section.end(), section.begin(),
                     [](const unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                     });
      if (section == "visual") {
        visual_section = true;
      } else if (visual_section && !section.empty()) {
        visual_section = false;
      }
    } else if (visual_section && !line.empty() && line.front() != '#' &&
               line.front() != ';') {
      const auto token_end = line.find_first_of(" \t\r");
      auto token = std::string(line.substr(0U, token_end));
      std::transform(token.begin(), token.end(), token.begin(),
                     [](const unsigned char value) {
                       return static_cast<char>(std::tolower(value));
                     });
      if (token == "renderer") {
        return has_newline ? end + 1U : end;
      }
    }
    begin = has_newline ? end + 1U : text.size();
  }
  throw std::runtime_error(
      "The original configuration has no Renderer entry in its Visual section");
}

void restore_stock_configuration(const std::filesystem::path &path,
                                 const std::vector<char> &stock_configuration) {
  auto configuration = stock_configuration;
  const auto needs_aspect_ratio =
      !configuration_has_key(configuration, "aspectratio");
  const auto needs_window_mode =
      !configuration_has_key(configuration, "windowmode");
  if (needs_aspect_ratio || needs_window_mode) {
    auto insert_at = configuration.size();
    while (insert_at != 0U && configuration[insert_at - 1U] == '\x1a') {
      --insert_at;
    }
    insert_at = configuration_display_settings_insert_position(configuration,
                                                               insert_at);
    const auto windows_lines =
        std::string_view(configuration.data(), configuration.size())
            .find("\r\n") != std::string_view::npos;
    const std::string_view newline = windows_lines ? "\r\n" : "\n";
    std::string addition;
    if (insert_at != 0U && configuration[insert_at - 1U] != '\n' &&
        configuration[insert_at - 1U] != '\r') {
      addition.append(newline);
    }
    if (needs_aspect_ratio) {
      addition.append("AspectRatio\t4:3");
      addition.append(newline);
    }
    if (needs_window_mode) {
      addition.append("WindowMode\tBorderless");
      addition.append(newline);
    }
    configuration.insert(configuration.begin() +
                             static_cast<std::ptrdiff_t>(insert_at),
                         addition.begin(), addition.end());
  }

  const std::filesystem::path temporary = path.wstring() + L".motorhead-part";
  std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("Could not preserve " + path.string());
  }
  stream.write(configuration.data(),
               static_cast<std::streamsize>(configuration.size()));
  stream.flush();
  if (!stream) {
    throw std::runtime_error("Could not finish " + path.string());
  }
  stream.close();
  if (MoveFileExW(temporary.c_str(), path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
      FALSE) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    throw std::runtime_error("Could not publish " + path.string());
  }
}

void write_embedded_file(const int resource_id,
                         const std::filesystem::path &destination) {
  const auto resource =
      FindResourceW(nullptr, MAKEINTRESOURCEW(resource_id), RT_RCDATA);
  if (resource == nullptr) {
    throw std::runtime_error("A required installer resource is missing");
  }
  const auto size = SizeofResource(nullptr, resource);
  const auto loaded = LoadResource(nullptr, resource);
  const auto *bytes = loaded != nullptr ? LockResource(loaded) : nullptr;
  if (size == 0U || bytes == nullptr) {
    throw std::runtime_error("A required installer resource is invalid");
  }
  std::filesystem::create_directories(destination.parent_path());
  std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("Could not create " + destination.string());
  }
  stream.write(static_cast<const char *>(bytes),
               static_cast<std::streamsize>(size));
  stream.flush();
  if (!stream) {
    throw std::runtime_error("Could not finish " + destination.string());
  }
}

class Cabinet final {
public:
  explicit Cabinet(const std::filesystem::path &path)
      : value_(unshield_open(path.string().c_str())) {
    if (value_ == nullptr) {
      throw std::runtime_error("The InstallShield cabinet " + path.string() +
                               " could not be opened");
    }
  }

  Cabinet(const Cabinet &) = delete;
  Cabinet &operator=(const Cabinet &) = delete;

  ~Cabinet() { unshield_close(value_); }

  void extract_groups(const std::filesystem::path &destination,
                      const std::vector<const char *> &group_names,
                      const bool content_directories_only,
                      const int minimum_files, const int progress_begin,
                      const int progress_end, const std::atomic_bool &cancel,
                      const std::function<void(int)> &progress) const {
    std::vector<UnshieldFileGroup *> groups;
    int count = 0;
    for (const auto *name : group_names) {
      auto *group = unshield_file_group_find(value_, name);
      if (group == nullptr || group->first_file < 0 ||
          group->last_file < group->first_file) {
        throw std::runtime_error(
            std::string("The InstallShield cabinet has no ") + name +
            " file group");
      }
      groups.push_back(group);
      count += group->last_file - group->first_file + 1;
    }
    int extracted = 0;
    int processed = 0;
    for (const auto *group : groups) {
      for (int index = group->first_file; index <= group->last_file; ++index) {
        if (cancel.load()) {
          throw ImportCancelled();
        }
        ++processed;
        progress(progress_begin +
                 processed * (progress_end - progress_begin) / count);
        if (!unshield_file_is_valid(value_, index)) {
          continue;
        }
        const auto *file_name = unshield_file_name(value_, index);
        const auto directory_index = unshield_file_directory(value_, index);
        const auto *directory_name =
            directory_index >= 0
                ? unshield_directory_name(value_, directory_index)
                : "";
        if (file_name == nullptr || directory_name == nullptr) {
          throw std::runtime_error("The cabinet index is damaged");
        }
        auto relative = checked_relative_path(file_name);
        if (*directory_name != '\0') {
          relative = checked_relative_path(directory_name) / relative;
        }
        // Movies are decoded by the reconstruction itself. Never materialize
        // the obsolete RAD runtime carried by the retail/update cabinets.
        if (_wcsicmp(relative.filename().c_str(), L"SMACKW32.DLL") == 0) {
          continue;
        }
        if (content_directories_only) {
          auto component = relative.begin()->string();
          std::transform(component.begin(), component.end(), component.begin(),
                         [](const unsigned char value) {
                           return static_cast<char>(std::tolower(value));
                         });
          const std::array allowed{"addressbook", "cars",   "data",   "demos",
                                   "game",        "league", "sounds", "tracks"};
          if (std::find(allowed.begin(), allowed.end(), component) ==
              allowed.end()) {
            continue;
          }
        }
        const auto output = destination / relative;
        std::filesystem::create_directories(output.parent_path());
        if (!unshield_file_save(value_, index, output.string().c_str())) {
          throw std::runtime_error("Could not extract " + relative.string());
        }
        ++extracted;
      }
    }
    if (extracted < minimum_files) {
      throw std::runtime_error(
          "The cabinet extraction was unexpectedly incomplete");
    }
  }

private:
  Unshield *value_ = nullptr;
};

struct CdTrack {
  int number = 0;
  int lba = 0;
  int control = 0;
};

struct CdLayout {
  std::vector<CdTrack> tracks;
  int leadout = 0;
};

class Handle final {
public:
  explicit Handle(const HANDLE value) : value_(value) {}
  ~Handle() {
    if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
      CloseHandle(value_);
    }
  }
  Handle(const Handle &) = delete;
  Handle &operator=(const Handle &) = delete;
  Handle(Handle &&other) noexcept : value_(other.value_) {
    other.value_ = INVALID_HANDLE_VALUE;
  }
  Handle &operator=(Handle &&other) noexcept {
    if (this != &other) {
      if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) {
        CloseHandle(value_);
      }
      value_ = other.value_;
      other.value_ = INVALID_HANDLE_VALUE;
    }
    return *this;
  }
  [[nodiscard]] HANDLE get() const { return value_; }

private:
  HANDLE value_ = INVALID_HANDLE_VALUE;
};

CdLayout read_cd_layout(const HANDLE device) {
  std::array<std::uint8_t, 804U> toc{};
  DWORD returned = 0U;
  if (DeviceIoControl(device, ioctl_cdrom_read_toc, nullptr, 0U, toc.data(),
                      static_cast<DWORD>(toc.size()), &returned,
                      nullptr) == FALSE) {
    throw std::runtime_error(
        "Could not read the physical CD table of contents (Windows error " +
        std::to_string(GetLastError()) + ")");
  }
  const auto first = static_cast<int>(toc[2U]);
  const auto last = static_cast<int>(toc[3U]);
  if (first != 1 || last != 11 || returned < 100U) {
    throw std::runtime_error(
        "Expected the original 11-track Motorhead mixed-mode CD");
  }
  CdLayout result;
  for (int index = 0; index <= last - first + 1; ++index) {
    const auto offset = 4U + static_cast<std::size_t>(index) * 8U;
    const auto number = static_cast<int>(toc[offset + 2U]);
    const auto lba = ((static_cast<int>(toc[offset + 5U]) * 60 +
                       static_cast<int>(toc[offset + 6U])) *
                          75 +
                      static_cast<int>(toc[offset + 7U])) -
                     150;
    const auto control = static_cast<int>(toc[offset + 1U] & 0x0FU);
    if (number == 0xAA) {
      result.leadout = lba;
    } else {
      result.tracks.push_back({number, lba, control});
    }
  }
  if (result.tracks.size() != 11U || result.tracks.front().number != 1 ||
      (result.tracks.front().control & 4) == 0 ||
      result.leadout <= result.tracks.back().lba) {
    throw std::runtime_error("The loaded CD does not have the retail layout");
  }
  for (std::size_t index = 1U; index < result.tracks.size(); ++index) {
    if (result.tracks[index].number != static_cast<int>(index + 1U) ||
        (result.tracks[index].control & 4) != 0) {
      throw std::runtime_error(
          "Retail CD audio tracks 2 through 11 are missing");
    }
  }
  return result;
}

int track_sector_count(const CdLayout &layout, const std::size_t index) {
  const auto end = index + 1U < layout.tracks.size()
                       ? layout.tracks[index + 1U].lba
                       : layout.leadout;
  return end - layout.tracks[index].lba;
}

struct RawReadInfo {
  std::int64_t disk_offset = 0;
  std::uint32_t sector_count = 0U;
  std::int32_t track_mode = 2;
};
static_assert(sizeof(RawReadInfo) == 16U);

std::vector<std::uint8_t> read_audio(const HANDLE device, const int lba,
                                     const int sectors) {
  RawReadInfo request;
  request.disk_offset = static_cast<std::int64_t>(lba) *
                        static_cast<std::int64_t>(cooked_sector_bytes);
  request.sector_count = static_cast<std::uint32_t>(sectors);
  std::vector<std::uint8_t> output(static_cast<std::size_t>(sectors) *
                                   raw_sector_bytes);
  DWORD returned = 0U;
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (DeviceIoControl(device, ioctl_cdrom_raw_read, &request,
                        static_cast<DWORD>(sizeof(request)), output.data(),
                        static_cast<DWORD>(output.size()), &returned,
                        nullptr) != FALSE &&
        returned == output.size()) {
      return output;
    }
    Sleep(static_cast<DWORD>(15 * (attempt + 1)));
  }
  throw std::runtime_error("Could not read CD audio at sector " +
                           std::to_string(lba) + ": " +
                           std::to_string(GetLastError()));
}

void put_u16(std::array<std::uint8_t, wave_header_bytes> &header,
             const std::size_t offset, const std::uint16_t value) {
  header[offset] = static_cast<std::uint8_t>(value);
  header[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

void put_u32(std::array<std::uint8_t, wave_header_bytes> &header,
             const std::size_t offset, const std::uint32_t value) {
  for (std::size_t index = 0U; index < 4U; ++index) {
    header[offset + index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

std::array<std::uint8_t, wave_header_bytes>
wave_header(const std::uint32_t data_bytes) {
  std::array<std::uint8_t, wave_header_bytes> result{};
  std::copy_n("RIFF", 4U, result.begin());
  put_u32(result, 4U, data_bytes + 36U);
  std::copy_n("WAVEfmt ", 8U, result.begin() + 8U);
  put_u32(result, 16U, 16U);
  put_u16(result, 20U, 1U);
  put_u16(result, 22U, 2U);
  put_u32(result, 24U, 44100U);
  put_u32(result, 28U, 176400U);
  put_u16(result, 32U, 4U);
  put_u16(result, 34U, 16U);
  std::copy_n("data", 4U, result.begin() + 36U);
  put_u32(result, 40U, data_bytes);
  return result;
}

void write_all(const HANDLE output, const void *bytes, std::size_t size) {
  const auto *cursor = static_cast<const std::uint8_t *>(bytes);
  while (size != 0U) {
    DWORD written = 0U;
    const auto batch = static_cast<DWORD>(
        std::min<std::size_t>(size, static_cast<std::size_t>(0x40000000U)));
    if (WriteFile(output, cursor, batch, &written, nullptr) == FALSE ||
        written == 0U) {
      throw std::runtime_error("Could not write ripped CD audio");
    }
    cursor += written;
    size -= written;
  }
}

Handle open_cd_device(const std::wstring &drive) {
  const auto device_name = L"\\\\.\\" + drive;
  return Handle(CreateFileW(device_name.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0U, nullptr));
}

void rip_audio_tracks(const std::wstring &drive,
                      const std::filesystem::path &music_directory,
                      const std::atomic_bool &cancel,
                      const std::function<void(const std::wstring &)> &status,
                      const std::function<void(int)> &progress) {
  auto device = open_cd_device(drive);
  if (device.get() == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("Could not open the physical Motorhead CD drive");
  }
  const auto layout = read_cd_layout(device.get());
  std::filesystem::create_directories(music_directory);
  std::int64_t total_sectors = 0;
  for (std::size_t index = 1U; index < layout.tracks.size(); ++index) {
    total_sectors += track_sector_count(layout, index);
  }
  std::int64_t completed = 0;
  int preferred_batch = 128;
  for (std::size_t index = 1U; index < layout.tracks.size(); ++index) {
    if (cancel.load()) {
      throw ImportCancelled();
    }
    const auto track = layout.tracks[index];
    const auto sectors = track_sector_count(layout, index);
    const auto end = track.lba + sectors;
    const auto data_bytes64 =
        static_cast<std::uint64_t>(sectors) * raw_sector_bytes;
    if (sectors <= 0 || data_bytes64 > UINT32_MAX) {
      throw std::runtime_error("The retail CD audio layout is invalid");
    }
    std::wostringstream name;
    name << L"track";
    if (track.number < 10) {
      name << L'0';
    }
    name << track.number << L".wav";
    status(L"Reading CD audio track " + std::to_wstring(track.number) +
           L" of 11 (" + name.str() + L")");
    const auto final_path = music_directory / name.str();
    const auto part_path = final_path.wstring() + L".part";
    Handle output(CreateFileW(
        part_path.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (output.get() == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("Could not create a WAV output file");
    }
    const auto header = wave_header(static_cast<std::uint32_t>(data_bytes64));
    write_all(output.get(), header.data(), header.size());
    for (int lba = track.lba; lba < end;) {
      if (cancel.load()) {
        throw ImportCancelled();
      }
      auto batch = std::min(preferred_batch, end - lba);
      std::vector<std::uint8_t> bytes;
      while (true) {
        try {
          bytes = read_audio(device.get(), lba, batch);
          preferred_batch = batch;
          break;
        } catch (const std::runtime_error &) {
          if (batch <= 16) {
            throw;
          }
          batch = std::max(16, batch / 2);
        }
      }
      write_all(output.get(), bytes.data(), bytes.size());
      lba += batch;
      completed += batch;
      progress(20 + static_cast<int>(completed * 76 / total_sectors));
    }
    if (FlushFileBuffers(output.get()) == FALSE) {
      throw std::runtime_error("Could not flush the ripped WAV file");
    }
    output = Handle(INVALID_HANDLE_VALUE);
    std::filesystem::rename(part_path, final_path);
  }
}

void rip_audio_tracks_from_cue(
    const std::filesystem::path &cue_path,
    const std::filesystem::path &music_directory,
    const std::atomic_bool &cancel,
    const std::function<void(const std::wstring &)> &status,
    const std::function<void(int)> &progress) {
  std::filesystem::create_directories(music_directory);
  for (int track_number = 2; track_number <= 11; ++track_number) {
    if (cancel.load()) {
      throw ImportCancelled();
    }
    std::wostringstream name;
    name << L"track" << (track_number < 10 ? L"0" : L"") << track_number
         << L".wav";
    status(L"Reading image audio track " + std::to_wstring(track_number) +
           L" of 11 (" + name.str() + L")");
    auto pcm = mh::disc::read_cdda_track_pcm(cue_path, track_number);
    if (pcm.pcm.empty() || pcm.pcm.size() > UINT32_MAX) {
      throw std::runtime_error("The CUE/BIN audio track size is invalid");
    }
    if (cancel.load()) {
      throw ImportCancelled();
    }
    const auto final_path = music_directory / name.str();
    const auto part_path = final_path.wstring() + L".part";
    Handle output(CreateFileW(
        part_path.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (output.get() == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("Could not create a WAV output file");
    }
    const auto header = wave_header(static_cast<std::uint32_t>(pcm.pcm.size()));
    write_all(output.get(), header.data(), header.size());
    write_all(output.get(), pcm.pcm.data(), pcm.pcm.size());
    if (FlushFileBuffers(output.get()) == FALSE) {
      throw std::runtime_error("Could not flush the decoded WAV file");
    }
    output = Handle(INVALID_HANDLE_VALUE);
    std::error_code error;
    std::filesystem::remove(final_path, error);
    error.clear();
    std::filesystem::rename(part_path, final_path, error);
    if (error) {
      throw std::runtime_error("Could not finalize a decoded WAV file");
    }
    progress(20 + (track_number - 1) * 76 / 10);
  }
}

void rip_s40_audio_tracks_from_cue(
    const std::filesystem::path &cue_path,
    const std::filesystem::path &music_directory,
    const std::atomic_bool &cancel,
    const std::function<void(const std::wstring &)> &status,
    const std::function<void(int)> &progress) {
  std::filesystem::create_directories(music_directory);
  for (int track_number = 2; track_number <= 4; ++track_number) {
    if (cancel.load()) {
      throw ImportCancelled();
    }
    std::wostringstream name;
    name << L"track0" << track_number << L"a.wav";
    status(L"Reading S40 Racing audio track " + std::to_wstring(track_number) +
           L" of 4 (" + name.str() + L")");
    auto pcm = mh::disc::read_cdda_track_pcm(cue_path, track_number);
    if (pcm.pcm.empty() || pcm.pcm.size() > UINT32_MAX) {
      throw std::runtime_error(
          "The S40 Racing CUE/BIN audio track size is invalid");
    }
    const auto final_path = music_directory / name.str();
    const auto part_path = final_path.wstring() + L".part";
    Handle output(CreateFileW(
        part_path.c_str(), GENERIC_WRITE, 0U, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (output.get() == INVALID_HANDLE_VALUE) {
      throw std::runtime_error("Could not create an S40 Racing WAV file");
    }
    const auto header = wave_header(static_cast<std::uint32_t>(pcm.pcm.size()));
    write_all(output.get(), header.data(), header.size());
    write_all(output.get(), pcm.pcm.data(), pcm.pcm.size());
    if (FlushFileBuffers(output.get()) == FALSE) {
      throw std::runtime_error("Could not flush an S40 Racing WAV file");
    }
    output = Handle(INVALID_HANDLE_VALUE);
    std::error_code error;
    std::filesystem::remove(final_path, error);
    error.clear();
    std::filesystem::rename(part_path, final_path, error);
    if (error) {
      throw std::runtime_error("Could not finalize an S40 Racing WAV file");
    }
    progress(97 + (track_number - 1) / 3);
  }
}

void copy_required_file(const std::filesystem::path &source,
                        const std::filesystem::path &destination) {
  if (!std::filesystem::is_regular_file(source)) {
    throw std::runtime_error("S40 Racing cabinet is missing " +
                             source.string());
  }
  std::filesystem::create_directories(destination.parent_path());
  std::filesystem::copy_file(source, destination,
                             std::filesystem::copy_options::overwrite_existing);
}

void install_s40_car_preview(const std::filesystem::path &source,
                             const std::filesystem::path &destination) {
  if (!std::filesystem::is_regular_file(source) ||
      std::filesystem::file_size(source) != s40_preview_size ||
      mh::common::sha256_file(source) != s40_preview_sha256) {
    throw std::runtime_error(
        "The S40 Racing car-select artwork failed verification");
  }
  std::ifstream input(source, std::ios::binary);
  std::vector<std::uint8_t> bmp{std::istreambuf_iterator<char>(input),
                                std::istreambuf_iterator<char>()};
  const auto le16 = [&bmp](const std::size_t offset) {
    return static_cast<std::uint16_t>(bmp[offset]) |
           (static_cast<std::uint16_t>(bmp[offset + 1U]) << 8U);
  };
  const auto le32 = [&bmp](const std::size_t offset) {
    return static_cast<std::uint32_t>(bmp[offset]) |
           (static_cast<std::uint32_t>(bmp[offset + 1U]) << 8U) |
           (static_cast<std::uint32_t>(bmp[offset + 2U]) << 16U) |
           (static_cast<std::uint32_t>(bmp[offset + 3U]) << 24U);
  };
  if (input.bad() || bmp.size() < 54U || bmp[0U] != 'B' || bmp[1U] != 'M' ||
      le32(10U) != 54U || le32(14U) != 40U || le32(18U) != 640U ||
      le32(22U) != 442U || le16(26U) != 1U || le16(28U) != 24U ||
      le32(30U) != 0U || bmp.size() - 54U != 640U * 442U * 3U) {
    throw std::runtime_error(
        "The S40 Racing car-select artwork has an unexpected BMP layout");
  }

  // The verified BMP and an uncompressed 24-bit TGA both store bottom-up BGR
  // rows. Only the container header changes; no resampling or colour loss is
  // introduced before Motorhead fits the image inside its authored oval.
  std::array<std::uint8_t, 18U> tga{};
  tga[2U] = 2U;
  tga[12U] = 0x80U;
  tga[13U] = 0x02U;
  tga[14U] = 0xbaU;
  tga[15U] = 0x01U;
  tga[16U] = 24U;
  std::filesystem::create_directories(destination.parent_path());
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(tga.data()),
               static_cast<std::streamsize>(tga.size()));
  output.write(reinterpret_cast<const char *>(bmp.data() + 54U),
               static_cast<std::streamsize>(bmp.size() - 54U));
  output.flush();
  if (!output) {
    throw std::runtime_error(
        "Could not install the S40 Racing car-select artwork");
  }
}

void write_s40_car_definition(const std::filesystem::path &destination) {
  const auto bytes = std::span(
      reinterpret_cast<const std::uint8_t *>(s40_car_definition.data()),
      s40_car_definition.size());
  const auto encoded = mh::content::encode_tbl_transform(bytes, "EQ");
  std::filesystem::create_directories(destination.parent_path());
  std::ofstream stream(destination, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("Could not create the S40 Racing car definition");
  }
  constexpr std::array<std::uint8_t, 4U> header{'T', 'B', 'L', 0x05U};
  stream.write(reinterpret_cast<const char *>(header.data()),
               static_cast<std::streamsize>(header.size()));
  stream.write(reinterpret_cast<const char *>(encoded.data()),
               static_cast<std::streamsize>(encoded.size()));
  stream.flush();
  if (!stream) {
    throw std::runtime_error("Could not finish the S40 Racing car definition");
  }
}

void install_s40_content(
    const std::filesystem::path &cue_path,
    const std::filesystem::path &install_root, const std::atomic_bool &cancel,
    const std::function<void(const std::wstring &)> &status,
    const std::function<void(int)> &progress) {
  const auto media = read_s40_cue(cue_path);
  if (cancel.load()) {
    throw ImportCancelled();
  }

  TemporaryDirectory image_directory;
  auto source_report = media.iso;
  std::erase_if(source_report.entries, [](const auto &entry) {
    const auto cabinet =
        _stricmp(entry.path.c_str(), "S40/DISK1/DATA1.CAB") == 0;
    const auto movies = _stricmp(entry.path.c_str(), "MOVIES/MOVIES.PAK") == 0;
    const auto preview = _stricmp(entry.path.c_str(), "MENU/S40NY.BMP") == 0;
    return entry.is_directory || (!cabinet && !movies && !preview);
  });
  if (source_report.entries.size() != 3U) {
    throw std::runtime_error(
        "The S40 Racing image has an ambiguous installer payload");
  }
  static_cast<void>(mh::disc::extract_iso9660(media.cue.binary_path,
                                              media.data_lba, source_report,
                                              image_directory.path(), true));
  const auto cabinet = image_directory.path() / "S40" / "DISK1" / "DATA1.CAB";
  std::error_code cabinet_error;
  if (!std::filesystem::is_regular_file(cabinet, cabinet_error) ||
      cabinet_error ||
      std::filesystem::file_size(cabinet, cabinet_error) != s40_cabinet_size ||
      cabinet_error || mh::common::sha256_file(cabinet) != s40_cabinet_sha256) {
    throw std::runtime_error(
        "The S40 Racing installer cabinet failed verification");
  }
  const auto movies = image_directory.path() / "MOVIES" / "MOVIES.PAK";
  std::error_code movies_error;
  if (!std::filesystem::is_regular_file(movies, movies_error) || movies_error ||
      std::filesystem::file_size(movies, movies_error) != s40_movies_size ||
      movies_error || mh::common::sha256_file(movies) != s40_movies_sha256) {
    throw std::runtime_error(
        "The S40 Racing intro movie archive failed verification");
  }
  copy_required_file(movies, install_root / "Movies" / "movies1.pak");
  install_s40_car_preview(image_directory.path() / "MENU" / "S40NY.BMP",
                          install_root / "Cars" / "Car99" / "Data" /
                              "_menuimg.TGA");

  TemporaryDirectory extracted_directory;
  unshield_set_log_level(UNSHIELD_LOG_LEVEL_ERROR);
  Cabinet(cabinet).extract_groups(extracted_directory.path(), {"Data"}, false,
                                  200, 96, 97, cancel, progress);
  const auto extracted = extracted_directory.path();
  copy_required_file(extracted / "Menu" / "DATA" / "GFX" / "volvoback.tga",
                     install_root / "Data" / "back1.tga");

  const auto car_root = install_root / "Cars" / "Car99";
  copy_required_file(extracted / "Objects" / "S40.COL", car_root / "car99.COL");
  for (int lod = 0; lod <= 3; ++lod) {
    copy_required_file(extracted / "Objects" /
                           ("s40" + std::to_string(lod) + ".MYO"),
                       car_root / ("car99" + std::to_string(lod) + ".MYO"));
    for (const auto *wheel : {"s40lf", "s40rf", "s40rb", "s40lb"}) {
      const auto suffix = std::string_view(wheel).substr(3U);
      copy_required_file(
          extracted / "Objects" /
              (std::string(wheel) + std::to_string(lod) + ".MYO"),
          car_root /
              ("car99" + std::string(suffix) + std::to_string(lod) + ".MYO"));
    }
  }
  for (const auto &texture_directory : {extracted / "Textures" / "s40steg1",
                                        extracted / "Textures" / "s40felg"}) {
    if (!std::filesystem::is_directory(texture_directory)) {
      throw std::runtime_error(
          "S40 Racing cabinet has incomplete car textures");
    }
    for (const auto &entry :
         std::filesystem::directory_iterator(texture_directory)) {
      if (entry.is_regular_file()) {
        copy_required_file(entry.path(), car_root / entry.path().filename());
      }
    }
  }
  write_s40_car_definition(install_root / "Game" / "car99.car");

  rip_s40_audio_tracks_from_cue(cue_path, install_root / "Music", cancel,
                                status, progress);
}

void validate_s40_data(const std::filesystem::path &root) {
  const std::array required{root / "Data" / "back1.tga",
                            root / "Game" / "car99.car",
                            root / "Cars" / "Car99" / "Data" / "_menuimg.TGA",
                            root / "Cars" / "Car99" / "car990.MYO",
                            root / "Cars" / "Car99" / "car993.MYO",
                            root / "Cars" / "Car99" / "car99.COL",
                            root / "Cars" / "Car99" / "VMainA.iff",
                            root / "Cars" / "Car99" / "V40fel0.iff",
                            root / "Music" / "track02a.wav",
                            root / "Music" / "track03a.wav",
                            root / "Music" / "track04a.wav",
                            root / "Movies" / "movies1.pak"};
  for (const auto &path : required) {
    if (!std::filesystem::is_regular_file(path)) {
      throw std::runtime_error("S40 Racing import is missing " + path.string());
    }
  }
  for (const auto *name : {"track02a.wav", "track03a.wav", "track04a.wav"}) {
    if (std::filesystem::file_size(root / "Music" / name) <=
        wave_header_bytes) {
      throw std::runtime_error("S40 Racing audio import is invalid");
    }
  }
}

void validate_retail_data(const std::filesystem::path &root,
                          const bool install_music) {
  const std::array required{root / "Game" / "car01.car",
                            root / "Game" / "Track1.trk",
                            root / "Cars" / "car01",
                            root / "Data" / "MENU.SPR",
                            root / "Data" / "MenuDial.spr",
                            root / "Sounds",
                            root / "Tracks" / "track1",
                            root / "League" / "Default.LGF",
                            root / "Demos",
                            root / "Movies" / "MOVIES.PAK"};
  for (const auto &path : required) {
    if (!std::filesystem::exists(path)) {
      throw std::runtime_error("Retail import is missing " + path.string());
    }
  }
  const auto league = root / "League" / "Default.LGF";
  if (!std::filesystem::is_regular_file(league) ||
      std::filesystem::file_size(league) != 5218U) {
    throw std::runtime_error(
        "The required League\\Default.LGF is missing or invalid");
  }
  const auto menu_dial = root / "Data" / "MenuDial.spr";
  std::error_code menu_dial_error;
  if (std::filesystem::file_size(menu_dial, menu_dial_error) !=
          menu_dial_size ||
      menu_dial_error ||
      mh::common::sha256_file(menu_dial) != menu_dial_sha256) {
    throw std::runtime_error(
        "The reconstruction Data\\MenuDial.spr is missing or invalid");
  }
  for (int number = 1; number <= 14; ++number) {
    std::ostringstream name;
    name << "car";
    if (number < 10) {
      name << '0';
    }
    name << number << ".car";
    if (!std::filesystem::is_regular_file(root / "Game" / name.str())) {
      throw std::runtime_error("Motorhead 3.0 car definitions are incomplete");
    }
  }
  for (int number = 1; number <= 16; ++number) {
    const auto name = "Track" + std::to_string(number) + ".trk";
    if (!std::filesystem::is_regular_file(root / "Game" / name)) {
      throw std::runtime_error(
          "Motorhead 3.0 forward/reverse track definitions are incomplete");
    }
  }
  for (int number = 11; number <= 14; ++number) {
    const auto directory = root / "Cars" / ("Car" + std::to_string(number));
    if (!std::filesystem::is_directory(directory) ||
        !std::filesystem::exists(directory / "Data" / "_menuimg.TGA")) {
      throw std::runtime_error("Motorhead 3.0 car assets are incomplete");
    }
  }
  for (int number = 9; number <= 16; ++number) {
    const auto ai =
        root / "Tracks" / ("track" + std::to_string(number)) / "Ai" / "ai.dat";
    if (!std::filesystem::is_regular_file(ai)) {
      throw std::runtime_error(
          "Motorhead 3.0 reverse-track AI assets are incomplete");
    }
  }
  if (!install_music) {
    return;
  }
  for (int number = 2; number <= 11; ++number) {
    std::ostringstream name;
    name << "track";
    if (number < 10) {
      name << '0';
    }
    name << number << ".wav";
    const auto path = root / "Music" / name.str();
    if (!std::filesystem::is_regular_file(path) ||
        std::filesystem::file_size(path) <= wave_header_bytes) {
      throw std::runtime_error("Retail audio import is missing " +
                               path.string());
    }
  }
}

void install_program_files(const std::filesystem::path &install_root) {
  write_embedded_file(resource_motorhead_executable,
                      install_root / "Motorhead.exe");
}

void remove_exact_tree(const std::filesystem::path &path,
                       const std::filesystem::path &required_parent) {
  if (!std::filesystem::exists(path)) {
    return;
  }
  const auto normalized = std::filesystem::absolute(path).lexically_normal();
  const auto parent =
      std::filesystem::absolute(required_parent).lexically_normal();
  if (normalized.parent_path() != parent || normalized == parent) {
    throw std::runtime_error("Refusing to remove an unexpected directory");
  }
  const auto make_removable = [](const std::filesystem::path &entry) {
    const auto attributes = GetFileAttributesW(entry.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return;
    }
    auto updated =
        attributes & ~(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM |
                       FILE_ATTRIBUTE_HIDDEN);
    if (updated == 0U) {
      updated = FILE_ATTRIBUTE_NORMAL;
    }
    SetFileAttributesW(entry.c_str(), updated);
  };
  for (const auto &entry : std::filesystem::recursive_directory_iterator(
           normalized,
           std::filesystem::directory_options::skip_permission_denied)) {
    make_removable(entry.path());
  }
  make_removable(normalized);
  std::filesystem::remove_all(normalized);
}

void remove_exact_file(const std::filesystem::path &path,
                       const std::filesystem::path &required_parent) {
  if (!std::filesystem::exists(path)) {
    return;
  }
  const auto normalized = std::filesystem::absolute(path).lexically_normal();
  const auto parent =
      std::filesystem::absolute(required_parent).lexically_normal();
  if (normalized.parent_path() != parent || normalized == parent ||
      !std::filesystem::is_regular_file(normalized)) {
    throw std::runtime_error("Refusing to remove an unexpected file");
  }
  SetFileAttributesW(normalized.c_str(), FILE_ATTRIBUTE_NORMAL);
  std::filesystem::remove(normalized);
}

void remove_managed_content(const std::filesystem::path &install_root) {
  for (const auto *name : managed_content_directories) {
    remove_exact_tree(install_root / name, install_root);
  }
  for (const auto *name : managed_content_files) {
    remove_exact_file(install_root / name, install_root);
  }
}

void restore_content_backup(const std::filesystem::path &install_root,
                            const std::filesystem::path &backup) {
  if (!std::filesystem::exists(backup)) {
    return;
  }
  remove_managed_content(install_root);
  SetFileAttributesW(backup.c_str(), FILE_ATTRIBUTE_NORMAL);
  std::vector<std::filesystem::path> entries;
  for (const auto &entry : std::filesystem::directory_iterator(backup)) {
    entries.push_back(entry.path());
  }
  for (const auto &entry : entries) {
    std::filesystem::rename(entry, install_root / entry.filename());
  }
  remove_exact_tree(backup, install_root);
}

bool backup_existing_content(const std::filesystem::path &install_root,
                             const std::filesystem::path &backup) {
  restore_content_backup(install_root, backup);
  auto moved = false;
  try {
    for (const auto *name : managed_content_directories) {
      const auto source = install_root / name;
      if (!std::filesystem::exists(source)) {
        continue;
      }
      if (!moved) {
        std::filesystem::create_directories(backup);
        moved = true;
      }
      std::filesystem::rename(source, backup / name);
    }
    for (const auto *name : managed_content_files) {
      const auto source = install_root / name;
      if (!std::filesystem::exists(source)) {
        continue;
      }
      if (!moved) {
        std::filesystem::create_directories(backup);
        moved = true;
      }
      std::filesystem::rename(source, backup / name);
    }
  } catch (...) {
    if (std::filesystem::exists(backup)) {
      SetFileAttributesW(backup.c_str(), FILE_ATTRIBUTE_NORMAL);
      std::vector<std::filesystem::path> entries;
      for (const auto &entry : std::filesystem::directory_iterator(backup)) {
        entries.push_back(entry.path());
      }
      for (const auto &entry : entries) {
        const auto destination = install_root / entry.filename();
        if (!std::filesystem::exists(destination)) {
          std::filesystem::rename(entry, destination);
        }
      }
      remove_exact_tree(backup, install_root);
    }
    throw;
  }
  if (moved) {
    SetFileAttributesW(backup.c_str(), FILE_ATTRIBUTE_HIDDEN);
  }
  return moved;
}

void prepare_user_directory(const std::filesystem::path &install_root,
                            const bool clean_profile,
                            const bool flatten_profile) {
  const auto user = install_root / "User";
  if (clean_profile) {
    remove_exact_tree(user, install_root);
  }
  std::filesystem::create_directories(user);

  // Windows paths are case-insensitive, but preserve the canonical spelling
  // in Explorer when upgrading an older installation that used `user`.
  for (const auto &entry : std::filesystem::directory_iterator(install_root)) {
    const auto name = entry.path().filename().wstring();
    if (_wcsicmp(name.c_str(), L"User") != 0 || name == L"User") {
      continue;
    }
    const auto temporary = install_root / ".motorhead-user-case";
    remove_exact_tree(temporary, install_root);
    std::filesystem::rename(entry.path(), temporary);
    std::filesystem::rename(temporary, user);
    break;
  }

  const auto legacy = user / "Game";
  if (!flatten_profile) {
    return;
  }
  if (std::filesystem::is_directory(legacy)) {
    std::vector<std::filesystem::path> entries;
    for (const auto &entry : std::filesystem::directory_iterator(legacy)) {
      entries.push_back(entry.path());
    }
    for (const auto &entry : entries) {
      const auto destination = user / entry.filename();
      if (std::filesystem::exists(destination)) {
        throw std::runtime_error(
            "Could not flatten User\\Game without overwriting " +
            destination.string());
      }
      std::filesystem::rename(entry, destination);
    }
    remove_exact_tree(legacy, user);
  }

  // Keep the installed filename consistently lowercase even when upgrading a
  // profile created by an earlier development build. A two-step rename is
  // required because Windows paths are case-insensitive but case-preserving.
  constexpr std::array<const wchar_t *, 3U> configuration_names{
      L"motorhead.cfg", L"motorhead.cfg.motorhead-part", L"motorhead.cfg.bak"};
  for (const auto *expected : configuration_names) {
    std::filesystem::path incorrectly_cased;
    for (const auto &entry : std::filesystem::directory_iterator(user)) {
      if (entry.is_regular_file() &&
          _wcsicmp(entry.path().filename().c_str(), expected) == 0 &&
          entry.path().filename() != expected) {
        incorrectly_cased = entry.path();
        break;
      }
    }
    if (incorrectly_cased.empty()) {
      continue;
    }
    const auto temporary = user / (std::wstring(L".motorhead-config-case-") +
                                   std::to_wstring(wcslen(expected)));
    if (std::filesystem::exists(temporary)) {
      throw std::runtime_error(
          "Could not normalize the motorhead.cfg filename because " +
          temporary.string() + " already exists");
    }
    std::filesystem::rename(incorrectly_cased, temporary);
    std::filesystem::rename(temporary, user / expected);
  }

  const auto input = user / "Input";
  std::filesystem::create_directories(input);
  constexpr std::array<const wchar_t *, 5U> controls{
      L"custom.clo", L"Joystick.clo", L"Keyboard.clo", L"mouse.clo",
      L"Wheel.clo"};
  for (const auto *control : controls) {
    for (const auto &directory : {user, input}) {
      const auto legacy =
          directory / (std::wstring(control) + L".motorhead-backup");
      if (!std::filesystem::is_regular_file(legacy)) {
        continue;
      }
      const auto replacement = directory / (std::wstring(control) + L".bak");
      if (!std::filesystem::exists(replacement)) {
        std::filesystem::rename(legacy, replacement);
      } else if (std::filesystem::is_regular_file(replacement) &&
                 files_have_identical_contents(legacy, replacement)) {
        remove_exact_file(legacy, directory);
      } else {
        throw std::runtime_error(
            "Could not rename a legacy control backup without overwriting " +
            replacement.string());
      }
    }
  }
  constexpr std::array<const wchar_t *, 3U> suffixes{L"", L".motorhead-part",
                                                     L".bak"};
  for (const auto *control : controls) {
    for (const auto *suffix : suffixes) {
      const auto filename = std::wstring(control) + suffix;
      const auto source = user / filename;
      if (!std::filesystem::is_regular_file(source)) {
        continue;
      }
      const auto destination = input / filename;
      if (!std::filesystem::exists(destination)) {
        std::filesystem::rename(source, destination);
      } else if (std::filesystem::is_regular_file(destination) &&
                 files_have_identical_contents(source, destination)) {
        remove_exact_file(source, user);
      } else {
        throw std::runtime_error(
            "Could not move a legacy control layout into User\\Input "
            "without overwriting " +
            destination.string());
      }
    }

    // The freshly installed Game copy is only a default. Publish it directly
    // as writable player data when no preserved layout exists, then remove the
    // duplicate from immutable game content.
    const auto packaged = install_root / "Game" / control;
    if (!std::filesystem::is_regular_file(packaged)) {
      continue;
    }
    const auto destination = input / control;
    if (!std::filesystem::exists(destination)) {
      std::filesystem::rename(packaged, destination);
    } else {
      remove_exact_file(packaged, install_root / "Game");
    }
  }
}

void announce(const InstallReport &report, const InstallPhase phase,
              const std::wstring &status) {
  if (report.phase) {
    report.phase(phase);
  }
  if (report.status) {
    report.status(status);
  }
}

} // namespace

std::wstring widen(const std::string_view value) {
  if (value.empty()) {
    return {};
  }
  const auto count =
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), nullptr, 0);
  if (count <= 0) {
    return {value.begin(), value.end()};
  }
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), count);
  return result;
}

std::filesystem::path executable_directory() {
  std::wstring buffer(32768U, L'\0');
  const auto count = GetModuleFileNameW(nullptr, buffer.data(),
                                        static_cast<DWORD>(buffer.size()));
  if (count == 0U || count >= buffer.size()) {
    throw std::runtime_error("Could not determine the installer location");
  }
  buffer.resize(count);
  return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path default_installation_directory() {
  std::wstring value(32768U, L'\0');
  const auto length = GetEnvironmentVariableW(L"ProgramFiles", value.data(),
                                              static_cast<DWORD>(value.size()));
  if (length != 0U && length < value.size()) {
    value.resize(length);
    return std::filesystem::path(value) / "Motorhead";
  }
  return std::filesystem::path(L"C:\\Program Files") / "Motorhead";
}

std::vector<RetailSource> discover_motorhead_drives() {
  std::array<wchar_t, 512U> drives{};
  const auto length =
      GetLogicalDriveStringsW(static_cast<DWORD>(drives.size()), drives.data());
  if (length == 0U || length >= drives.size()) {
    throw std::runtime_error("Could not enumerate Windows drives");
  }
  std::vector<RetailSource> result;
  for (const wchar_t *entry = drives.data(); *entry != L'\0';
       entry += std::wcslen(entry) + 1U) {
    if (GetDriveTypeW(entry) != DRIVE_CDROM) {
      continue;
    }
    std::array<wchar_t, 128U> label{};
    if (GetVolumeInformationW(entry, label.data(),
                              static_cast<DWORD>(label.size()), nullptr,
                              nullptr, nullptr, nullptr, 0U) == FALSE ||
        _wcsicmp(label.data(), L"MOTORHEAD") != 0) {
      continue;
    }
    const std::filesystem::path root(entry);
    if (!std::filesystem::is_regular_file(root / "MOTOR" / "DISK1" /
                                          "DATA1.CAB") ||
        !std::filesystem::is_regular_file(root / "movies" / "movies.pak")) {
      continue;
    }
    RetailSource drive;
    drive.kind = RetailSourceKind::physical_disc;
    drive.path = root;
    drive.label =
        std::wstring(L"Motorhead CD-ROM (") + root.root_name().wstring() + L")";
    ULARGE_INTEGER total{};
    ULARGE_INTEGER free_bytes{};
    ULARGE_INTEGER available{};
    if (GetDiskFreeSpaceExW(entry, &available, &total, &free_bytes) != FALSE) {
      drive.bytes = total.QuadPart;
    }
    result.push_back(std::move(drive));
  }
  return result;
}

RetailSource inspect_motorhead_cue(const std::filesystem::path &cue_path) {
  const auto media = read_motorhead_cue(cue_path);
  RetailSource source;
  source.kind = RetailSourceKind::cue_image;
  source.path = std::filesystem::absolute(cue_path).lexically_normal();
  source.label = source.path.filename().wstring();
  source.bytes = std::filesystem::file_size(media.cue.binary_path);
  return source;
}

RetailSource inspect_s40_cue(const std::filesystem::path &cue_path) {
  const auto media = read_s40_cue(cue_path);
  RetailSource source;
  source.kind = RetailSourceKind::cue_image;
  source.path = std::filesystem::absolute(cue_path).lexically_normal();
  source.label = source.path.filename().wstring();
  source.bytes = std::filesystem::file_size(media.cue.binary_path);
  return source;
}

bool free_space_bytes(const std::filesystem::path &destination,
                      std::uint64_t &available) {
  auto probe = std::filesystem::absolute(destination).lexically_normal();
  std::error_code error;
  while (!probe.empty() && !std::filesystem::exists(probe, error)) {
    const auto parent = probe.parent_path();
    if (parent == probe) {
      return false;
    }
    probe = parent;
  }
  if (probe.empty()) {
    return false;
  }
  ULARGE_INTEGER free_for_caller{};
  ULARGE_INTEGER total{};
  ULARGE_INTEGER total_free{};
  if (GetDiskFreeSpaceExW(probe.wstring().c_str(), &free_for_caller, &total,
                          &total_free) == FALSE) {
    return false;
  }
  available = free_for_caller.QuadPart;
  return true;
}

bool installation_exists(const std::filesystem::path &destination) {
  std::error_code error;
  const auto flat =
      std::filesystem::is_directory(destination / "Game", error) &&
      std::filesystem::is_directory(destination / "Cars", error) &&
      std::filesystem::is_directory(destination / "Tracks", error);
  error.clear();
  const auto wrapped =
      std::filesystem::is_directory(destination / "data" / "Game", error) &&
      std::filesystem::is_directory(destination / "data" / "Cars", error) &&
      std::filesystem::is_directory(destination / "data" / "Tracks", error);
  return flat || wrapped;
}

std::filesystem::path
inspect_motorhead_patch(const std::filesystem::path &patch_path) {
  const auto normalized =
      std::filesystem::absolute(patch_path).lexically_normal();
  std::error_code error;
  if (!std::filesystem::is_regular_file(normalized, error) || error) {
    throw std::runtime_error(
        "Select the official Motorhead 3.0 update file named mhp30.exe");
  }
  if (_wcsicmp(normalized.filename().c_str(), L"mhp30.exe") != 0) {
    throw std::runtime_error(
        "The Motorhead 3.0 update file must be named mhp30.exe");
  }
  if (std::filesystem::file_size(normalized, error) != motorhead_patch_size ||
      error || mh::common::sha256_file(normalized) != motorhead_patch_sha256) {
    throw std::runtime_error(
        "This is not the exact supported European Motorhead 3.0 update. "
        "Expected SHA-256 " +
        std::string(motorhead_patch_sha256));
  }
  return normalized;
}

void perform_import(const RetailSource &retail_source,
                    const std::filesystem::path &patch_path,
                    const std::filesystem::path &install_root,
                    const bool install_music,
                    const std::filesystem::path &s40_cue_path,
                    const bool clean_profile, const std::atomic_bool &cancel,
                    const InstallReport &report) {
  const std::function<void(int)> progress =
      report.progress ? report.progress : [](int) {};
  const std::function<void(const std::wstring &)> status =
      report.status ? report.status : [](const std::wstring &) {};
  const auto verified_patch = inspect_motorhead_patch(patch_path);
  TemporaryDirectory patch_directory;
  const auto patch_cabinet =
      extract_patch_cabinet(verified_patch, patch_directory.path());
  std::unique_ptr<TemporaryDirectory> image_directory;
  std::filesystem::path retail_root;
  if (retail_source.kind == RetailSourceKind::cue_image) {
    announce(report, InstallPhase::extract_retail,
             L"Reading the retail CUE/BIN image");
    progress(1);
    const auto media = read_motorhead_cue(retail_source.path);
    if (cancel.load()) {
      throw ImportCancelled();
    }
    image_directory = std::make_unique<TemporaryDirectory>();
    static_cast<void>(mh::disc::extract_iso9660(media.cue.binary_path,
                                                media.data_lba, media.iso,
                                                image_directory->path(), true));
    retail_root = image_directory->path();
  } else {
    retail_root = retail_source.path;
  }
  const auto cabinet = retail_root / "MOTOR" / "DISK1" / "DATA1.CAB";
  const auto movies = retail_root / "movies" / "movies.pak";
  if (!std::filesystem::is_regular_file(cabinet) ||
      !std::filesystem::is_regular_file(movies)) {
    throw std::runtime_error(
        "The selected source is not the retail Motorhead CD");
  }
  std::filesystem::create_directories(install_root);
  const auto backup = install_root / ".motorhead-content-previous";
  const auto default_configuration =
      install_root / ".motorhead-default-configuration";
  remove_exact_file(default_configuration, install_root);
  auto content_install_started = false;
  try {
    static_cast<void>(backup_existing_content(install_root, backup));
    content_install_started = true;
    prepare_user_directory(install_root, false, false);
    announce(report, InstallPhase::extract_retail,
             L"Extracting the original retail game files");
    progress(1);
    unshield_set_log_level(UNSHIELD_LOG_LEVEL_ERROR);
    Cabinet(cabinet).extract_groups(install_root, {"Data"}, false, 1000, 2, 16,
                                    cancel, progress);
    // motorhead.cfg is generated by the original setup and is not a member of
    // the retail DATA1.CAB. Prepare the captured setup baseline privately;
    // writable configuration belongs only in User, never installed Game data.
    write_embedded_file(resource_default_configuration, default_configuration);
    const auto stock_configuration =
        read_stock_configuration(default_configuration);
    announce(report, InstallPhase::apply_update,
             L"Applying the official Motorhead 3.0 content update");
    Cabinet(patch_cabinet)
        .extract_groups(install_root, {"Data", "Game Misc", "DataNoDiamond"},
                        true, 200, 16, 20, cancel, progress);
    restore_stock_configuration(default_configuration, stock_configuration);
    announce(report, InstallPhase::copy_movies,
             L"Copying the original movie archive");
    std::filesystem::create_directories(install_root / "Movies");
    std::filesystem::copy_file(
        movies, install_root / "Movies" / "MOVIES.PAK",
        std::filesystem::copy_options::overwrite_existing);
    write_embedded_file(resource_default_league,
                        install_root / "League" / "Default.LGF");
    write_embedded_file(resource_menu_dial,
                        install_root / "Data" / "MenuDial.spr");
    progress(20);
    if (install_music) {
      announce(report, InstallPhase::rip_music,
               L"Reading the original CD soundtrack");
      if (retail_source.kind == RetailSourceKind::cue_image) {
        rip_audio_tracks_from_cue(retail_source.path, install_root / "Music",
                                  cancel, status, progress);
      } else {
        rip_audio_tracks(retail_source.path.root_name().wstring(),
                         install_root / "Music", cancel, status, progress);
      }
    } else {
      std::filesystem::create_directories(install_root / "Music");
      announce(report, InstallPhase::rip_music,
               L"Skipping the optional CD soundtrack");
      progress(96);
    }
    if (!s40_cue_path.empty()) {
      announce(report, InstallPhase::install_s40,
               L"Installing the optional S40 Racing content");
      install_s40_content(s40_cue_path, install_root, cancel, status, progress);
    } else {
      announce(report, InstallPhase::install_s40,
               L"Skipping the optional S40 Racing content");
      progress(98);
    }
    announce(report, InstallPhase::validate,
             L"Validating the installed game data");
    validate_retail_data(install_root, install_music);
    if (!s40_cue_path.empty()) {
      validate_s40_data(install_root);
    }
    progress(98);
    announce(report, InstallPhase::install_program,
             L"Installing the Motorhead program files");
    install_program_files(install_root);
    const auto installed_game_configuration =
        install_root / "Game" / "motorhead.cfg";
    prepare_user_directory(install_root, clean_profile, true);
    const auto user_configuration = install_root / "User" / "motorhead.cfg";
    const auto preserved_game_configuration = backup / "Game" / "motorhead.cfg";
    if (!clean_profile &&
        !std::filesystem::is_regular_file(user_configuration) &&
        std::filesystem::is_regular_file(preserved_game_configuration)) {
      std::filesystem::copy_file(preserved_game_configuration,
                                 user_configuration);
    }
    if (!std::filesystem::is_regular_file(user_configuration)) {
      std::filesystem::rename(default_configuration, user_configuration);
    } else {
      remove_exact_file(default_configuration, install_root);
    }
    remove_exact_file(installed_game_configuration, install_root / "Game");
    if (!std::filesystem::is_regular_file(user_configuration) ||
        read_stock_configuration(user_configuration).empty()) {
      throw std::runtime_error(
          "The User\\motorhead.cfg configuration was not installed");
    }
    remove_exact_tree(backup, install_root);
    progress(100);
  } catch (...) {
    try {
      remove_exact_file(default_configuration, install_root);
    } catch (...) {
    }
    if (content_install_started) {
      try {
        remove_managed_content(install_root);
        if (std::filesystem::exists(backup)) {
          restore_content_backup(install_root, backup);
        }
      } catch (...) {
      }
    }
    throw;
  }
}

} // namespace motorhead
