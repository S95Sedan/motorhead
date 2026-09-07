#include <content/formats/menu_sound_config.hpp>

#include <core/error.hpp>

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <vector>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::string lower_ascii(std::string value) {
  for (auto &character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
  }
  return value;
}

std::string trim(std::string_view value) {
  const auto whitespace = [](const char character) {
    return character == ' ' || character == '\t' || character == '\r';
  };
  while (!value.empty() && whitespace(value.front())) {
    value.remove_prefix(1U);
  }
  while (!value.empty() && whitespace(value.back())) {
    value.remove_suffix(1U);
  }
  return std::string(value);
}

} // namespace

MenuSoundConfig
parse_menu_sound_config(const std::span<const std::uint8_t> bytes) {
  if (bytes.empty() || bytes.size() > 64U * 1024U) {
    throw ToolError(ExitCode::format,
                    "menu-sound configuration size is outside bounds");
  }
  if (std::find(bytes.begin(), bytes.end(), 0U) != bytes.end()) {
    throw ToolError(ExitCode::format,
                    "menu-sound configuration contains a NUL byte");
  }

  const std::string text(bytes.begin(), bytes.end());
  std::istringstream lines(text);
  MenuSoundConfig result;
  std::string pending_sample;
  std::string line;
  std::size_t line_number = 0U;
  while (std::getline(lines, line)) {
    ++line_number;
    const auto comment = line.find("//");
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    std::istringstream fields(line);
    std::string key;
    fields >> key;
    key = lower_ascii(key);
    if (key == "samplename") {
      if (!pending_sample.empty()) {
        throw ToolError(
            ExitCode::format,
            "menu-sound SampleName has no SampleEvent before line " +
                std::to_string(line_number));
      }
      std::string trailing;
      if (!(fields >> pending_sample) || (fields >> trailing) ||
          pending_sample.size() > 128U ||
          std::filesystem::path(pending_sample).is_absolute() ||
          pending_sample == "." || pending_sample == ".." ||
          pending_sample.find('/') != std::string::npos ||
          pending_sample.find('\\') != std::string::npos) {
        throw ToolError(ExitCode::format,
                        "menu-sound SampleName must be one relative filename");
      }
      continue;
    }
    if (key != "sampleevent") {
      throw ToolError(ExitCode::format,
                      "unknown menu-sound directive on line " +
                          std::to_string(line_number));
    }
    if (pending_sample.empty()) {
      throw ToolError(ExitCode::format,
                      "menu-sound SampleEvent has no preceding SampleName");
    }

    MenuSoundBinding binding;
    binding.sample_name = std::move(pending_sample);
    std::set<std::uint32_t> unique_events;
    std::uint64_t event = 0U;
    while (fields >> event) {
      if (event >= menu_sound_event_count) {
        throw ToolError(ExitCode::format,
                        "menu-sound event is outside the recovered 0..35 "
                        "range");
      }
      const auto narrowed = static_cast<std::uint32_t>(event);
      if (!unique_events.insert(narrowed).second) {
        throw ToolError(ExitCode::format,
                        "menu-sound binding repeats an event");
      }
      binding.events.push_back(narrowed);
    }
    if (!fields.eof() || binding.events.empty()) {
      throw ToolError(ExitCode::format,
                      "menu-sound SampleEvent requires integer event IDs");
    }
    result.bindings.push_back(std::move(binding));
  }
  if (!pending_sample.empty()) {
    throw ToolError(ExitCode::format,
                    "menu-sound configuration ends after SampleName");
  }
  if (result.bindings.empty()) {
    throw ToolError(ExitCode::format,
                    "menu-sound configuration contains no bindings");
  }
  return result;
}

MenuSoundConfig read_menu_sound_config(const std::filesystem::path &path,
                                       const std::uint64_t maximum_file_bytes) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    throw ToolError(ExitCode::input,
                    "menu-sound configuration input is not a plain file");
  }
  const auto size = std::filesystem::file_size(path);
  if (size > maximum_file_bytes ||
      size > std::numeric_limits<std::size_t>::max()) {
    throw ToolError(ExitCode::format,
                    "menu-sound configuration exceeds its size bound");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  std::ifstream stream(path, std::ios::binary);
  stream.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  if (!stream ||
      stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw ToolError(ExitCode::input,
                    "short read while opening menu-sound configuration");
  }
  return parse_menu_sound_config(bytes);
}

const MenuSoundBinding *
resolve_menu_sound_event(const MenuSoundConfig &config,
                         const std::uint32_t event) noexcept {
  if (event >= menu_sound_event_count) {
    return nullptr;
  }
  const MenuSoundBinding *result = nullptr;
  for (const auto &binding : config.bindings) {
    if (std::find(binding.events.begin(), binding.events.end(), event) !=
        binding.events.end()) {
      result = &binding;
    }
  }
  return result;
}

} // namespace mh::content
