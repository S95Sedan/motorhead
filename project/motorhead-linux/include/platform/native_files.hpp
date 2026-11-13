#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>

namespace mh::native {
namespace fs = std::filesystem;
inline fs::path path(std::string_view value) {
  std::string normalized(value);
  std::replace(normalized.begin(),normalized.end(),'\\','/');
  return fs::path(normalized);
}
inline fs::path path(const fs::path &value) { return path(std::string_view(value.string())); }
inline fs::path path(const std::string &value) { return path(std::string_view(value)); }
inline fs::path path(const char *value) { return path(std::string_view(value)); }

inline fs::path asset_path(const fs::path &requested) {
  const auto normalized=path(requested).lexically_normal();
  std::error_code error;
  if (fs::exists(normalized,error)) return normalized;
  auto current=normalized.is_absolute() ? normalized.root_path() : fs::current_path();
  const auto lower=[](std::string s) {
    for (char &c : s) if (c>='A' && c<='Z') c=static_cast<char>(c+'a'-'A');
    return s;
  };
  for (const auto &part : normalized.relative_path()) {
    auto exact=current/part;
    error.clear();
    if (fs::exists(exact,error) || part=="." || part=="..") { current=exact; continue; }
    error.clear();
    if (!fs::is_directory(current,error)) return normalized;
    const auto wanted=lower(part.string());
    fs::path match;
    for (const auto &entry : fs::directory_iterator(current,error)) {
      if (lower(entry.path().filename().string())!=wanted) continue;
      if (!match.empty()) throw fs::filesystem_error("Case-ambiguous retail asset",requested,std::make_error_code(std::errc::file_exists));
      match=entry.path();
    }
    if (match.empty()) return normalized;
    current=match;
  }
  return current;
}

inline bool exists(const fs::path &p) { return fs::exists(asset_path(p)); }
inline bool exists(const fs::path &p,std::error_code &e) { return fs::exists(asset_path(p),e); }
inline bool exists(fs::file_status s) noexcept { return fs::exists(s); }
inline bool is_regular_file(const fs::path &p) { return fs::is_regular_file(asset_path(p)); }
inline bool is_regular_file(const fs::path &p,std::error_code &e) { return fs::is_regular_file(asset_path(p),e); }
inline bool is_regular_file(fs::file_status s) noexcept { return fs::is_regular_file(s); }
inline auto file_size(const fs::path &p) { return fs::file_size(asset_path(p)); }
inline auto file_size(const fs::path &p,std::error_code &e) { return fs::file_size(asset_path(p),e); }
inline auto symlink_status(const fs::path &p) { return fs::symlink_status(asset_path(p)); }
inline auto symlink_status(const fs::path &p,std::error_code &e) { return fs::symlink_status(asset_path(p),e); }
template<class... Args> auto directory_iterator(const fs::path &p,Args&&... args) {
  return fs::directory_iterator(asset_path(p),std::forward<Args>(args)...);
}
template<class... Args> auto recursive_directory_iterator(const fs::path &p,Args&&... args) {
  return fs::recursive_directory_iterator(asset_path(p),std::forward<Args>(args)...);
}
class InputFile : public std::ifstream {
public:
  InputFile()=default;
  explicit InputFile(const fs::path &p,std::ios_base::openmode mode=std::ios_base::in)
      : std::ifstream(asset_path(p),mode) {}
  void open(const fs::path &p,std::ios_base::openmode mode=std::ios_base::in) {
    std::ifstream::open(asset_path(p),mode);
  }
};
}
