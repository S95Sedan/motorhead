#include <platform/system.hpp>
#include <system_error>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <cctype>
namespace mh::platform {
void replace_file(const std::filesystem::path &source,
                  const std::filesystem::path &destination) {
  std::error_code error;
  std::filesystem::rename(source, destination, error);
  if (error) {
    throw std::system_error(error, "could not publish " + destination.string());
  }
}

void flush_file(const std::filesystem::path &path) {
  static_cast<void>(path);
}

std::tm local_time(std::time_t time) { std::tm result{};
localtime_r(&time, &result);
return result; }
void initialize_application(const char *) {}
}
