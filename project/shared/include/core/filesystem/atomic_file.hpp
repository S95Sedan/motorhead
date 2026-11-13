#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

namespace mh::common {

using AtomicFileValidator =
    std::function<bool(const std::filesystem::path &)>;

[[nodiscard]] std::filesystem::path
atomic_part_path(const std::filesystem::path &path);
[[nodiscard]] std::filesystem::path
atomic_backup_path(const std::filesystem::path &path);

// Returns true when a valid primary is available after recovery. A valid
// interrupted part is preferred over the older backup. Invalid artifacts are
// retained only when no valid recovery source exists, so diagnostics are not
// destroyed.
bool recover_atomic_file(const std::filesystem::path &path,
                         const AtomicFileValidator &validator);

void write_atomic_file(const std::filesystem::path &path,
                       std::span<const std::uint8_t> bytes,
                       const AtomicFileValidator &validator);
void write_atomic_text(const std::filesystem::path &path,
                       std::string_view text,
                       const AtomicFileValidator &validator);

} // namespace mh::common
