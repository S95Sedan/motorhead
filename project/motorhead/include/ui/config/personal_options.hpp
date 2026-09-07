#pragma once

#include <ui/frontend/controller.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace mh::ui {

[[nodiscard]] PersonalOptionsConfiguration load_personal_options_configuration(
    const std::filesystem::path &reference_root);
void save_personal_options_configuration(
    const std::filesystem::path &reference_root,
    const PersonalOptionsConfiguration &configuration);
[[nodiscard]] std::vector<std::string> playable_personal_arguments(
    const PersonalOptionsConfiguration &configuration);

} // namespace mh::ui
