#include <content/formats/sprite_positions.hpp>

#include <core/error.hpp>

#include <fstream>
#include <limits>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

std::int32_t i32(
    const std::span<const std::uint8_t> bytes, const std::size_t offset) {
    const auto value = static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
        | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
        | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
    return static_cast<std::int32_t>(value);
}

std::int32_t interpolate(
    const std::int32_t from, const std::int32_t to,
    const std::uint32_t phase) {
    const auto difference =
        static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
    const auto product = difference * phase;
    const auto shifted = product >= 0
        ? product / 256
        : -((-product + 255) / 256);
    const auto result = static_cast<std::int64_t>(from) + shifted;
    if (result < std::numeric_limits<std::int32_t>::min()
        || result > std::numeric_limits<std::int32_t>::max()) {
        throw ToolError(
            ExitCode::format, "SPR position interpolation overflows int32");
    }
    return static_cast<std::int32_t>(result);
}

} // namespace

SprPositionData parse_spr_positions(
    const std::span<const std::uint8_t> bytes) {
    if (bytes.size() != spr_position_file_bytes) {
        throw ToolError(
            ExitCode::format, "SPR position file must be exactly 32768 bytes");
    }
    SprPositionData data;
    std::size_t cursor = 0U;
    for (auto& bank : data.banks) {
        for (auto& record : bank.records) {
            record.x_from = i32(bytes, cursor);
            record.x_to = i32(bytes, cursor + 4U);
            record.y_from = i32(bytes, cursor + 8U);
            record.y_to = i32(bytes, cursor + 12U);
            for (std::size_t index = 0U; index < record.direct.size(); ++index) {
                record.direct[index] = i32(bytes, cursor + 16U + index * 4U);
            }
            cursor += spr_position_record_fields * sizeof(std::int32_t);
        }
    }
    return data;
}

SprPositionData read_spr_positions(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error || std::filesystem::is_symlink(status)
        || !std::filesystem::is_regular_file(status)) {
        throw ToolError(ExitCode::input, "SPR position input is not a plain file");
    }
    if (std::filesystem::file_size(path) != spr_position_file_bytes) {
        throw ToolError(
            ExitCode::format, "SPR position file must be exactly 32768 bytes");
    }
    std::array<std::uint8_t, spr_position_file_bytes> bytes{};
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!stream || stream.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw ToolError(ExitCode::input, "short read while opening SPR position input");
    }
    return parse_spr_positions(bytes);
}

SprPositionSample sample_spr_position(const SprPositionData& data,
    const std::size_t bank_index, const std::size_t record_index,
    const std::uint32_t phase) {
    if (bank_index >= data.banks.size()
        || record_index >= data.banks.front().records.size()) {
        throw ToolError(ExitCode::usage, "SPR position bank or record is out of range");
    }
    if (phase > 256U) {
        throw ToolError(
            ExitCode::usage, "SPR position phase must be in the range 0 through 256");
    }
    const auto& record = data.banks[bank_index].records[record_index];
    return SprPositionSample{
        interpolate(record.x_from, record.x_to, phase),
        interpolate(record.y_from, record.y_to, phase),
        record.direct};
}

bool is_empty_spr_position(const SprPositionRecord& record) {
    if (record.x_from != 0 || record.x_to != 0
        || record.y_from != 0 || record.y_to != 0) {
        return false;
    }
    for (const auto value : record.direct) {
        if (value != 0) {
            return false;
        }
    }
    return true;
}

} // namespace mh::content
