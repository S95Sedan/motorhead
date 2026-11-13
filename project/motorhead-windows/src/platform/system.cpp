#include <platform/system.hpp>
#include <system_error>
#include <stdexcept>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <cctype>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
namespace mh::platform {
void replace_file(const std::filesystem::path &source,
                  const std::filesystem::path &destination) {
  if (MoveFileExW(source.c_str(), destination.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(),
                            "could not publish " + destination.string());
  }
}

void flush_file(const std::filesystem::path &path) {
  const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(),
                            "could not open for flush " + path.string());
  }
  const auto flushed = FlushFileBuffers(handle) != 0;
  const auto error = flushed ? ERROR_SUCCESS : GetLastError();
  CloseHandle(handle);
  if (!flushed) {
    throw std::system_error(static_cast<int>(error), std::system_category(),
                            "could not flush " + path.string());
  }
}

std::tm local_time(std::time_t time) { std::tm result{};
localtime_s(&result, &time);
return result; }
void initialize_application(const char *executable) {
  if (executable == nullptr || std::getenv("MOTORHEAD_D3D12_ENHANCED") != nullptr) return;
  auto name=std::filesystem::path(executable).filename().string();
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if(name == "motorhead-d3d12-preview.exe" && _putenv_s("MOTORHEAD_D3D12_ENHANCED", "1") != 0)
    throw std::runtime_error("Could not enable the D3D12 preview profile");
}
}
