#pragma once

#include <filesystem>

namespace mh::ui {

// Creates or upgrades the per-user writable overlay. Existing valid user files
// always win; source files are imported only during first-run/legacy migration.
void prepare_user_data(const std::filesystem::path &content_root,
                       const std::filesystem::path &user_root);

} // namespace mh::ui
