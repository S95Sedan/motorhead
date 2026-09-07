#include "core/filesystem/atomic_file.hpp"

#include <fstream>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace mh::common {
namespace {

bool valid(const std::filesystem::path &path,
           const AtomicFileValidator &validator) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    return false;
  }
  try {
    return validator(path);
  } catch (...) {
    return false;
  }
}

std::filesystem::path legacy_backup_path(
    const std::filesystem::path &path) {
  auto result = path;
  result += ".motorhead-backup";
  return result;
}

void migrate_legacy_backup(const std::filesystem::path &path,
                           const AtomicFileValidator &validator) {
  const auto legacy = legacy_backup_path(path);
  if (!std::filesystem::is_regular_file(legacy)) {
    return;
  }
  const auto backup = atomic_backup_path(path);
  if (valid(backup, validator)) {
    std::error_code error;
    std::filesystem::remove(legacy, error);
    if (error) {
      throw std::system_error(error, "could not remove legacy backup " +
                                         legacy.string());
    }
    return;
  }
  if (!std::filesystem::exists(backup)) {
    std::error_code error;
    std::filesystem::rename(legacy, backup, error);
    if (error) {
      throw std::system_error(error, "could not rename legacy backup " +
                                         legacy.string());
    }
  }
}

void replace_file(const std::filesystem::path &source,
                  const std::filesystem::path &destination) {
#ifdef _WIN32
  if (MoveFileExW(source.c_str(), destination.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(),
                            "could not publish " + destination.string());
  }
#else
  std::error_code error;
  std::filesystem::rename(source, destination, error);
  if (error) {
    throw std::system_error(error, "could not publish " + destination.string());
  }
#endif
}

void flush_file(const std::filesystem::path &path) {
#ifdef _WIN32
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
#else
  static_cast<void>(path);
#endif
}

void copy_to_part(const std::filesystem::path &source,
                  const std::filesystem::path &part) {
  std::error_code error;
  std::filesystem::remove(part, error);
  error.clear();
  std::filesystem::copy_file(source, part,
                             std::filesystem::copy_options::overwrite_existing,
                             error);
  if (error) {
    throw std::system_error(error, "could not stage " + source.string());
  }
  flush_file(part);
}

void write_part(const std::filesystem::path &part,
                const std::span<const std::uint8_t> bytes) {
  std::ofstream stream(part, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("could not create " + part.string());
  }
  stream.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  stream.flush();
  if (!stream) {
    stream.close();
    std::error_code ignored;
    std::filesystem::remove(part, ignored);
    throw std::runtime_error("could not write " + part.string());
  }
  stream.close();
  if (!stream) {
    std::error_code ignored;
    std::filesystem::remove(part, ignored);
    throw std::runtime_error("could not close " + part.string());
  }
  flush_file(part);
}

} // namespace

std::filesystem::path atomic_part_path(const std::filesystem::path &path) {
  auto result = path;
  result += ".motorhead-part";
  return result;
}

std::filesystem::path atomic_backup_path(const std::filesystem::path &path) {
  auto result = path;
  result += ".bak";
  return result;
}

bool recover_atomic_file(const std::filesystem::path &path,
                         const AtomicFileValidator &validator) {
  migrate_legacy_backup(path, validator);
  const auto part = atomic_part_path(path);
  const auto backup = atomic_backup_path(path);
  if (valid(path, validator)) {
    std::error_code ignored;
    std::filesystem::remove(part, ignored);
    return true;
  }
  if (valid(part, validator)) {
    replace_file(part, path);
    return valid(path, validator);
  }
  if (valid(backup, validator)) {
    std::filesystem::create_directories(path.parent_path());
    copy_to_part(backup, part);
    replace_file(part, path);
    return valid(path, validator);
  }
  return false;
}

void write_atomic_file(const std::filesystem::path &path,
                       const std::span<const std::uint8_t> bytes,
                       const AtomicFileValidator &validator) {
  if (path.empty() || path.filename().empty()) {
    throw std::invalid_argument("atomic output path is empty");
  }
  std::filesystem::create_directories(path.parent_path());
  const auto part = atomic_part_path(path);
  const auto backup = atomic_backup_path(path);
  std::error_code ignored;
  std::filesystem::remove(part, ignored);
  write_part(part, bytes);
  if (!valid(part, validator)) {
    std::filesystem::remove(part, ignored);
    throw std::runtime_error("atomic output failed validation: " +
                             path.string());
  }

  if (valid(path, validator)) {
    auto backup_part = atomic_part_path(backup);
    copy_to_part(path, backup_part);
    if (!valid(backup_part, validator)) {
      std::filesystem::remove(backup_part, ignored);
      std::filesystem::remove(part, ignored);
      throw std::runtime_error("atomic backup failed validation: " +
                               path.string());
    }
    replace_file(backup_part, backup);
  }

  replace_file(part, path);
  if (!valid(path, validator)) {
    if (!recover_atomic_file(path, validator)) {
      throw std::runtime_error("published output failed validation: " +
                               path.string());
    }
    throw std::runtime_error("published output was restored from backup: " +
                             path.string());
  }
}

void write_atomic_text(const std::filesystem::path &path,
                       const std::string_view text,
                       const AtomicFileValidator &validator) {
  const auto bytes = std::span(
      reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
  write_atomic_file(path, bytes, validator);
}

} // namespace mh::common
