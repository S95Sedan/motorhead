#include <disc/cd_audio.hpp>

#include <core/error.hpp>
#include <core/crypto/sha256.hpp>
#include <disc/cue_sheet.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

namespace mh::disc {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::uint64_t raw_sector_bytes = 2352U;
constexpr std::uint64_t sample_frame_bytes = 4U;

void append_le16(std::vector<std::uint8_t>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append_le32(std::vector<std::uint8_t>& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void append_text(std::vector<std::uint8_t>& bytes, const std::string_view text) {
    bytes.insert(bytes.end(), text.begin(), text.end());
}

void write_bytes_guarded(const std::filesystem::path& output,
                         const std::span<const std::uint8_t> bytes,
                         const bool overwrite) {
    if (output.empty()) {
        throw ToolError(ExitCode::usage, "CDDA WAV output path is empty");
    }
    const auto destination = std::filesystem::absolute(output).lexically_normal();
    const auto parent = destination.parent_path();
    std::error_code error;
    auto current = parent.root_path();
    for (const auto& component : parent.relative_path()) {
        current /= component;
        auto status = std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory) {
            error.clear();
            if (!std::filesystem::create_directory(current, error) || error) {
                throw ToolError(ExitCode::input,
                                "cannot create CDDA WAV output directory");
            }
            status = std::filesystem::symlink_status(current, error);
        }
        if (error || std::filesystem::is_symlink(status) ||
            !std::filesystem::is_directory(status)) {
            throw ToolError(ExitCode::input,
                            "CDDA WAV output parent is not a plain directory");
        }
    }
    error.clear();
    const auto status = std::filesystem::symlink_status(destination, error);
    const auto exists = !error && std::filesystem::exists(status);
    if (exists && (std::filesystem::is_symlink(status) ||
                   !std::filesystem::is_regular_file(status))) {
        throw ToolError(ExitCode::input, "CDDA WAV output is not a plain file");
    }
    if (exists && !overwrite) {
        throw ToolError(ExitCode::input,
                        "CDDA WAV output exists (use --overwrite)");
    }
    auto temporary = destination;
    temporary += ".mhtool-part";
    auto backup = destination;
    backup += ".mhtool-backup";
    if (std::filesystem::exists(temporary) ||
        (exists && std::filesystem::exists(backup))) {
        throw ToolError(ExitCode::input,
                        "CDDA WAV temporary or backup output exists");
    }
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!stream) {
            std::filesystem::remove(temporary, error);
            throw ToolError(ExitCode::input, "cannot write CDDA WAV output");
        }
    }
    auto backed_up = false;
    if (exists) {
        std::filesystem::rename(destination, backup, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            throw ToolError(ExitCode::input,
                            "cannot preserve existing CDDA WAV output");
        }
        backed_up = true;
    }
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        if (backed_up) {
            std::error_code ignored;
            std::filesystem::rename(backup, destination, ignored);
        }
        throw ToolError(ExitCode::input, "cannot finalize CDDA WAV output");
    }
    if (backed_up) {
        std::filesystem::remove(backup, error);
        if (error) {
            throw ToolError(ExitCode::input,
                            "cannot remove preserved CDDA WAV output");
        }
    }
}

} // namespace

CddaPcmTrack read_cdda_track_pcm(const std::filesystem::path& cue_path,
                                 const int track_number) {
    const auto cue = parse_cue(cue_path);
    const auto selected =
        std::find_if(cue.tracks.begin(), cue.tracks.end(),
                     [track_number](const auto& track) {
                         return track.number == track_number;
                     });
    if (selected == cue.tracks.end() || selected->type != TrackType::audio) {
        throw ToolError(ExitCode::usage,
                        "requested CUE track is absent or is not audio");
    }
    const auto start = find_index_lba(*selected, 1);
    if (!start.has_value()) {
        throw ToolError(ExitCode::format, "audio track has no INDEX 01");
    }

    std::error_code error;
    const auto status = std::filesystem::symlink_status(cue.binary_path, error);
    if (error || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_regular_file(status)) {
        throw ToolError(ExitCode::input, "CUE binary is not a plain file");
    }
    const auto binary_bytes = std::filesystem::file_size(cue.binary_path);
    if (binary_bytes % raw_sector_bytes != 0U) {
        throw ToolError(ExitCode::format,
                        "raw BIN size is not divisible by 2352 bytes");
    }
    const auto total_sectors = binary_bytes / raw_sector_bytes;
    auto end = total_sectors;
    const auto next = std::next(selected);
    if (next != cue.tracks.end()) {
        const auto next_index_one = find_index_lba(*next, 1);
        if (!next_index_one.has_value()) {
            throw ToolError(ExitCode::format,
                            "following CUE track has no INDEX 01");
        }
        end = find_index_lba(*next, 0).value_or(*next_index_one);
    }
    if (*start >= end || end > total_sectors) {
        throw ToolError(ExitCode::format,
                        "audio track extent is outside the raw BIN");
    }
    const auto sectors = end - *start;
    const auto pcm_bytes = sectors * raw_sector_bytes;
    if (pcm_bytes > std::numeric_limits<std::uint32_t>::max() - 36U) {
        throw ToolError(ExitCode::format,
                        "audio track exceeds the canonical WAV size bound");
    }

    CddaPcmTrack result;
    result.track_number = track_number;
    result.start_lba = *start;
    result.end_lba = static_cast<std::uint32_t>(end);
    result.pcm.resize(static_cast<std::size_t>(pcm_bytes));
    std::ifstream input(cue.binary_path, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(*start * raw_sector_bytes));
    input.read(reinterpret_cast<char*>(result.pcm.data()),
               static_cast<std::streamsize>(pcm_bytes));
    if (!input || input.gcount() != static_cast<std::streamsize>(pcm_bytes)) {
        throw ToolError(ExitCode::input,
                        "short read while reading CDDA audio track");
    }
    return result;
}

CddaWavExport export_cdda_track_wav(
    const std::filesystem::path& cue_path, const int track_number,
    const std::filesystem::path& output, const bool overwrite) {
    const auto track = read_cdda_track_pcm(cue_path, track_number);
    const auto pcm_bytes = static_cast<std::uint64_t>(track.pcm.size());
    std::vector<std::uint8_t> wav;
    wav.reserve(static_cast<std::size_t>(pcm_bytes + 44U));
    append_text(wav, "RIFF");
    append_le32(wav, static_cast<std::uint32_t>(pcm_bytes + 36U));
    append_text(wav, "WAVEfmt ");
    append_le32(wav, 16U);
    append_le16(wav, 1U);
    append_le16(wav, track.channels);
    append_le32(wav, track.sample_rate);
    const auto frame_bytes = static_cast<std::uint32_t>(
        track.channels * (track.bits_per_sample / 8U));
    append_le32(wav, track.sample_rate * frame_bytes);
    append_le16(wav, static_cast<std::uint16_t>(frame_bytes));
    append_le16(wav, track.bits_per_sample);
    append_text(wav, "data");
    append_le32(wav, static_cast<std::uint32_t>(pcm_bytes));
    wav.insert(wav.end(), track.pcm.begin(), track.pcm.end());
    write_bytes_guarded(output, wav, overwrite);

    CddaWavExport result;
    result.track_number = track_number;
    result.start_lba = track.start_lba;
    result.end_lba = track.end_lba;
    result.sector_count = track.end_lba - track.start_lba;
    result.sample_frames = pcm_bytes / sample_frame_bytes;
    result.pcm_bytes = pcm_bytes;
    result.wav_bytes = pcm_bytes + 44U;
    result.sha256 = mh::common::sha256_file(output);
    return result;
}

} // namespace mh::disc
