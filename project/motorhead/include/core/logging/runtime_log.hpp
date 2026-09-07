#pragma once

#include <filesystem>
#include <string_view>

namespace mh::common {

// Opens the process-wide runtime log. Repeated initialization with the same
// path is ignored so the embedded race runtime continues the front-end
// session instead of creating a second one.
[[nodiscard]] bool
initialize_runtime_log(const std::filesystem::path &path) noexcept;

[[nodiscard]] std::filesystem::path runtime_log_path() noexcept;

void log_runtime_info(std::string_view message) noexcept;
void log_runtime_error(std::string_view message) noexcept;

} // namespace mh::common
