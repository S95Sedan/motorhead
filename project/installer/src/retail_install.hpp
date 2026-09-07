#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace motorhead {

struct ImportCancelled final : std::runtime_error {
  ImportCancelled() : std::runtime_error("Import cancelled") {}
};

// Ordered installation phases. The frontend renders one checklist row per
// entry, so the order matches the order perform_import announces them in.
enum class InstallPhase {
  extract_retail = 0,
  apply_update,
  copy_movies,
  rip_music,
  install_s40,
  validate,
  install_program
};

constexpr std::size_t install_phase_count = 7U;

struct InstallReport {
  std::function<void(InstallPhase)> phase;
  std::function<void(const std::wstring &)> status;
  std::function<void(int)> progress;
};

enum class RetailSourceKind { physical_disc, cue_image };

struct RetailSource {
  RetailSourceKind kind = RetailSourceKind::physical_disc;
  std::filesystem::path path; // Drive root or selected .cue file.
  std::wstring label;
  std::uint64_t bytes = 0U;
};

std::wstring widen(std::string_view value);
std::filesystem::path executable_directory();
std::filesystem::path default_installation_directory();

std::vector<RetailSource> discover_motorhead_drives();
RetailSource inspect_motorhead_cue(const std::filesystem::path &cue_path);
RetailSource inspect_s40_cue(const std::filesystem::path &cue_path);

bool free_space_bytes(const std::filesystem::path &destination,
                      std::uint64_t &available);
bool installation_exists(const std::filesystem::path &destination);
std::filesystem::path
inspect_motorhead_patch(const std::filesystem::path &patch_path);

void perform_import(const RetailSource &retail_source,
                    const std::filesystem::path &patch_path,
                    const std::filesystem::path &install_root,
                    bool install_music,
                    const std::filesystem::path &s40_cue_path,
                    bool clean_profile,
                    const std::atomic_bool &cancel, const InstallReport &report);

}  // namespace motorhead
