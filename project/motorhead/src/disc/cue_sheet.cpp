#include <core/error.hpp>
#include <disc/cue_sheet.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>

namespace mh::disc {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::string trim(std::string value) {
    const auto is_space = [](const unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

bool starts_with_word(const std::string& line, const std::string_view word) {
    return line.size() >= word.size()
        && line.compare(0, word.size(), word) == 0
        && (line.size() == word.size() || std::isspace(static_cast<unsigned char>(line[word.size()])) != 0);
}

std::uint32_t parse_time(const std::string& value, const std::size_t line_number) {
    int minutes = -1;
    int seconds = -1;
    int frames = -1;
    char first_colon = 0;
    char second_colon = 0;
    std::istringstream input(value);
    input >> minutes >> first_colon >> seconds >> second_colon >> frames;
    if (!input || !input.eof() || first_colon != ':' || second_colon != ':'
        || minutes < 0 || seconds < 0 || seconds >= 60 || frames < 0 || frames >= 75) {
        throw ToolError(ExitCode::format,
            "invalid CUE time at line " + std::to_string(line_number) + ": " + value);
    }
    return static_cast<std::uint32_t>((minutes * 60 + seconds) * 75 + frames);
}

} // namespace

CueSheet parse_cue(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw ToolError(ExitCode::input, "cannot open CUE file: " + path.string());
    }

    CueSheet sheet;
    sheet.cue_path = std::filesystem::absolute(path).lexically_normal();
    CueTrack* current_track = nullptr;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        line = trim(std::move(line));
        if (line.empty() || starts_with_word(line, "REM")) {
            continue;
        }

        if (starts_with_word(line, "FILE")) {
            if (!sheet.binary_path.empty()) {
                throw ToolError(ExitCode::format,
                    "multi-file CUE sheets are not supported yet (line " + std::to_string(line_number) + ")");
            }
            const auto first_quote = line.find('"');
            const auto second_quote = first_quote == std::string::npos
                ? std::string::npos
                : line.find('"', first_quote + 1U);
            if (first_quote == std::string::npos || second_quote == std::string::npos) {
                throw ToolError(ExitCode::format,
                    "CUE FILE path must be quoted at line " + std::to_string(line_number));
            }
            const auto relative = line.substr(first_quote + 1U, second_quote - first_quote - 1U);
            sheet.binary_type = trim(line.substr(second_quote + 1U));
            if (sheet.binary_type != "BINARY") {
                throw ToolError(ExitCode::format,
                    "only BINARY CUE files are supported at line " + std::to_string(line_number));
            }
            sheet.binary_path = (sheet.cue_path.parent_path() / relative).lexically_normal();
            continue;
        }

        if (starts_with_word(line, "TRACK")) {
            if (sheet.binary_path.empty()) {
                throw ToolError(ExitCode::format,
                    "CUE TRACK appears before FILE at line " + std::to_string(line_number));
            }
            std::istringstream fields(line);
            std::string keyword;
            std::string mode;
            CueTrack track;
            fields >> keyword >> track.number >> mode;
            if (!fields || track.number <= 0 || !fields.eof()) {
                throw ToolError(ExitCode::format,
                    "invalid CUE TRACK at line " + std::to_string(line_number));
            }
            if (mode == "MODE1/2352") {
                track.type = TrackType::mode1_2352;
            } else if (mode == "AUDIO") {
                track.type = TrackType::audio;
            } else {
                throw ToolError(ExitCode::format,
                    "unsupported CUE track mode at line " + std::to_string(line_number) + ": " + mode);
            }
            if (!sheet.tracks.empty() && track.number <= sheet.tracks.back().number) {
                throw ToolError(ExitCode::format, "CUE track numbers must increase");
            }
            sheet.tracks.push_back(std::move(track));
            current_track = &sheet.tracks.back();
            continue;
        }

        if (starts_with_word(line, "INDEX")) {
            if (current_track == nullptr) {
                throw ToolError(ExitCode::format,
                    "CUE INDEX appears before TRACK at line " + std::to_string(line_number));
            }
            std::istringstream fields(line);
            std::string keyword;
            std::string time;
            CueIndex index;
            fields >> keyword >> index.number >> time;
            if (!fields || index.number < 0 || !fields.eof()) {
                throw ToolError(ExitCode::format,
                    "invalid CUE INDEX at line " + std::to_string(line_number));
            }
            index.lba = parse_time(time, line_number);
            if (!current_track->indices.empty()
                && index.number <= current_track->indices.back().number) {
                throw ToolError(ExitCode::format, "CUE index numbers must increase within a track");
            }
            current_track->indices.push_back(index);
            continue;
        }

        // PREGAP/POSTGAP describe synthetic silence generated by the player;
        // their sectors are not stored in the referenced BIN. Validate the
        // directive but leave the file-backed INDEX positions unchanged.
        if (starts_with_word(line, "PREGAP") ||
            starts_with_word(line, "POSTGAP")) {
            if (current_track == nullptr) {
                throw ToolError(ExitCode::format,
                    "CUE gap appears before TRACK at line " + std::to_string(line_number));
            }
            std::istringstream fields(line);
            std::string keyword;
            std::string time;
            fields >> keyword >> time;
            if (!fields || !fields.eof()) {
                throw ToolError(ExitCode::format,
                    "invalid CUE gap at line " + std::to_string(line_number));
            }
            static_cast<void>(parse_time(time, line_number));
            continue;
        }

        // Metadata directives do not affect sector layout.
        if (starts_with_word(line, "TITLE") || starts_with_word(line, "PERFORMER")
            || starts_with_word(line, "SONGWRITER") || starts_with_word(line, "FLAGS")
            || starts_with_word(line, "ISRC") || starts_with_word(line, "CATALOG")) {
            continue;
        }

        throw ToolError(ExitCode::format,
            "unsupported CUE directive at line " + std::to_string(line_number) + ": " + line);
    }

    if (!input.eof()) {
        throw ToolError(ExitCode::input, "failed while reading CUE file: " + path.string());
    }
    if (sheet.binary_path.empty() || sheet.tracks.empty()) {
        throw ToolError(ExitCode::format, "CUE sheet must contain one FILE and at least one TRACK");
    }
    for (const auto& track : sheet.tracks) {
        if (!find_index_lba(track, 1).has_value()) {
            throw ToolError(ExitCode::format,
                "CUE track " + std::to_string(track.number) + " has no INDEX 01");
        }
    }
    if (!std::filesystem::is_regular_file(sheet.binary_path)) {
        throw ToolError(ExitCode::input,
            "CUE binary file does not exist: " + sheet.binary_path.string());
    }
    return sheet;
}

std::optional<std::uint32_t> find_index_lba(const CueTrack& track, const int index_number) {
    const auto found = std::find_if(track.indices.begin(), track.indices.end(),
        [index_number](const CueIndex& index) { return index.number == index_number; });
    if (found == track.indices.end()) {
        return std::nullopt;
    }
    return found->lba;
}

std::string track_type_name(const TrackType type) {
    switch (type) {
    case TrackType::mode1_2352: return "MODE1/2352";
    case TrackType::audio: return "AUDIO";
    }
    return "UNKNOWN";
}

} // namespace mh::disc
