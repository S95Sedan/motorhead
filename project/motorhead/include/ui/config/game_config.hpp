#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mh::ui {

using ConfigurationUpdates =
    std::vector<std::pair<std::string, std::string>>;

enum class IntroMovieMode {
  skip,
  play_key_skips_all,
  play_key_skips_single,
};

[[nodiscard]] std::filesystem::path motorhead_configuration_path(
    const std::filesystem::path &reference_root);
[[nodiscard]] std::filesystem::path control_configuration_path(
    const std::filesystem::path &reference_root, std::string_view filename);
[[nodiscard]] std::string configuration_file_value(
    const std::filesystem::path &path, std::string_view key);
[[nodiscard]] IntroMovieMode intro_movie_mode(
    const std::filesystem::path &reference_root);
[[nodiscard]] bool intro_movie_key_skips_all(IntroMovieMode mode) noexcept;
void update_configuration_file(const std::filesystem::path &path,
                               const ConfigurationUpdates &updates);
void update_motorhead_configuration(
    const std::filesystem::path &reference_root,
    const ConfigurationUpdates &updates);

// Imports settings written by older reconstruction builds, then removes their
// four MotorHead-Reconstruction*.CFG overlays. Control-layout data returns to
// the original .clo files below Input; all other settings return to
// motorhead.cfg.
void migrate_legacy_motorhead_configurations(
    const std::filesystem::path &reference_root);

} // namespace mh::ui
