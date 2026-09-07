#include <core/logging/runtime_log.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

namespace mh::common {
namespace {

constexpr std::uintmax_t maximum_log_size = 1024ULL * 1024ULL;
constexpr std::uintmax_t retained_log_size = 768ULL * 1024ULL;

struct RuntimeLogState {
  std::mutex mutex;
  std::filesystem::path path;
};

RuntimeLogState &runtime_log_state() {
  static RuntimeLogState state;
  return state;
}

std::string timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm local{};
#ifdef _WIN32
  localtime_s(&local, &time);
#else
  localtime_r(&time, &local);
#endif
  char text[24]{};
  if (std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local) == 0U) {
    return "unknown-time";
  }
  return text;
}

void compact_log(const std::filesystem::path &path) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error || size < maximum_log_size) {
    return;
  }

  const auto keep = std::min(size, retained_log_size);
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return;
  }
  input.seekg(static_cast<std::streamoff>(size - keep));
  std::string tail{std::istreambuf_iterator<char>(input),
                   std::istreambuf_iterator<char>()};
  if (size > keep) {
    const auto first_complete_line = tail.find('\n');
    if (first_complete_line != std::string::npos) {
      tail.erase(0U, first_complete_line + 1U);
    }
  }
  input.close();

  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    return;
  }
  output << "--- older log entries discarded ---\n" << tail;
}

void append_log_line(const std::filesystem::path &path,
                     const std::string_view level,
                     const std::string_view message) {
  compact_log(path);
  std::ofstream output(path, std::ios::binary | std::ios::app);
  if (!output) {
    return;
  }
  auto single_line = std::string(message);
  std::replace(single_line.begin(), single_line.end(), '\r', ' ');
  std::replace(single_line.begin(), single_line.end(), '\n', ' ');
  output << '[' << timestamp() << "] " << level << ' ' << single_line << '\n';
}

void write_runtime_log(const std::string_view level,
                       const std::string_view message) noexcept {
  try {
    auto &state = runtime_log_state();
    const std::scoped_lock lock(state.mutex);
    if (!state.path.empty()) {
      append_log_line(state.path, level, message);
    }
  } catch (...) {
    // Diagnostics must never turn a recoverable game failure into a crash.
  }
}

} // namespace

bool initialize_runtime_log(const std::filesystem::path &path) noexcept {
  try {
    if (path.empty()) {
      return false;
    }
    auto normalized = std::filesystem::absolute(path).lexically_normal();
    auto &state = runtime_log_state();
    const std::scoped_lock lock(state.mutex);
    if (state.path == normalized) {
      return true;
    }
    if (!normalized.parent_path().empty()) {
      std::filesystem::create_directories(normalized.parent_path());
    }
    compact_log(normalized);
    std::ofstream probe(normalized, std::ios::binary | std::ios::app);
    if (!probe) {
      return false;
    }
    probe.close();
    state.path = std::move(normalized);
    append_log_line(state.path, "INFO", "--- Motorhead session started ---");
    return true;
  } catch (...) {
    return false;
  }
}

std::filesystem::path runtime_log_path() noexcept {
  try {
    auto &state = runtime_log_state();
    const std::scoped_lock lock(state.mutex);
    return state.path;
  } catch (...) {
    return {};
  }
}

void log_runtime_info(const std::string_view message) noexcept {
  write_runtime_log("INFO", message);
}

void log_runtime_error(const std::string_view message) noexcept {
  write_runtime_log("ERROR", message);
}

} // namespace mh::common
