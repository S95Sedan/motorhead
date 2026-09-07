#include <core/filesystem/case_insensitive.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>

namespace mh::common {
namespace {

std::string ascii_lower(std::string value) {
  for (auto &character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character + ('a' - 'A'));
    }
  }
  return value;
}

} // namespace

std::filesystem::path
resolve_relative_case_insensitive(const std::filesystem::path &root,
                                  const std::string_view reference) {
  auto authored = std::string(reference);
  std::replace(authored.begin(), authored.end(), '\\', '/');
  const auto relative = std::filesystem::path(authored).lexically_normal();
  if (relative.empty() || relative.is_absolute()) {
    throw std::runtime_error("authored path is not relative: " + authored);
  }

  auto current = root;
  for (const auto &part : relative) {
    const auto part_text = part.string();
    if (part_text.empty() || part_text == ".") {
      continue;
    }
    if (part_text == "..") {
      throw std::runtime_error("authored path leaves its content root: " +
                               authored);
    }

    const auto wanted = ascii_lower(part_text);
    std::optional<std::filesystem::path> match;
    for (const auto &entry : std::filesystem::directory_iterator(current)) {
      if (ascii_lower(entry.path().filename().string()) != wanted) {
        continue;
      }
      if (match.has_value()) {
        throw std::runtime_error("authored path is case-ambiguous: " +
                                 authored);
      }
      match = entry.path();
    }
    if (!match.has_value()) {
      throw std::runtime_error("authored path was not found: " + authored);
    }
    current = *match;
  }
  return current;
}

} // namespace mh::common
