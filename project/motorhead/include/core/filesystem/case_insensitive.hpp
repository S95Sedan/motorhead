#pragma once

#include <filesystem>
#include <string_view>

namespace mh::common {

[[nodiscard]] std::filesystem::path
resolve_relative_case_insensitive(const std::filesystem::path &root,
                                  std::string_view reference);

} // namespace mh::common
