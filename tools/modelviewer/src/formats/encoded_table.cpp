#include <formats/encoded_table.hpp>

#include <core/error.hpp>
#include <core/sha256.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::size_t primary_key_period = 0xe9U;
constexpr std::size_t name_key_period = 0xe3U;
constexpr std::array<std::uint8_t, 9> primary_key_source{
    0x01U, 0x03U, 0xfaU, 0x15U, 0x20U, 0xfaU, 0x2aU, 0x80U, 0xaaU};

std::uint32_t little_u32(const std::span<const std::uint8_t> bytes) {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

class BitReader {
public:
    explicit BitReader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    std::uint32_t bit() {
        if (byte_ >= bytes_.size()) {
            throw ToolError(ExitCode::format, "packed TBL bitstream is truncated");
        }
        const auto result = (bytes_[byte_] >> (7U - bit_)) & 1U;
        if (++bit_ == 8U) { bit_ = 0U; ++byte_; }
        return result;
    }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t byte_ = 0U;
    std::uint8_t bit_ = 0U;
};

struct HuffmanStream {
    std::vector<std::uint32_t> nodes;
    std::span<const std::uint8_t> bits;
};

HuffmanStream parse_huffman(const std::span<const std::uint8_t> source) {
    if (source.size() < 4U) {
        throw ToolError(ExitCode::format, "packed TBL Huffman header is truncated");
    }
    const auto header = little_u32(source.first(4U));
    const auto tree_bytes = static_cast<std::size_t>(header & 0x003fffffU);
    const auto root_offset = (header >> 22U) & 0xffU;
    const auto width = static_cast<std::size_t>((header >> 30U) + 1U);
    if (tree_bytes < 4U || tree_bytes > source.size()
        || (tree_bytes - 4U) % width != 0U) {
        throw ToolError(ExitCode::format, "packed TBL Huffman tree has invalid bounds");
    }
    const auto count = (tree_bytes - 4U) / width;
    if (count == 0U || count > 1U << 20U) {
        throw ToolError(ExitCode::format, "packed TBL Huffman tree has invalid size");
    }
    HuffmanStream result;
    result.nodes.reserve(count);
    auto cursor = std::size_t{4U};
    for (std::size_t index = 0U; index < count; ++index) {
        std::uint32_t value = 0U;
        for (std::size_t byte = 0U; byte < width; ++byte) {
            value |= static_cast<std::uint32_t>(source[cursor++]) << (byte * 8U);
        }
        if ((value & 1U) != 0U) {
            value = (((value >> 1U) + root_offset) << 1U) | 1U;
        }
        result.nodes.push_back(value);
    }
    result.bits = source.subspan(tree_bytes);
    return result;
}

std::uint8_t huffman_symbol(
    const std::vector<std::uint32_t>& nodes, BitReader& reader) {
    std::size_t node = 0U;
    for (std::size_t depth = 0U; depth <= nodes.size(); ++depth) {
        const auto branch = static_cast<std::size_t>(reader.bit());
        if (node > nodes.size() || branch >= nodes.size() - node) {
            throw ToolError(ExitCode::format, "packed TBL Huffman branch is out of bounds");
        }
        const auto value = nodes[node + branch];
        if ((value & 1U) != 0U) {
            if ((value >> 1U) > 0xffU) {
                throw ToolError(ExitCode::format, "packed TBL Huffman leaf exceeds one byte");
            }
            return static_cast<std::uint8_t>(value >> 1U);
        }
        if (value > nodes.size() - node) {
            throw ToolError(ExitCode::format, "packed TBL Huffman node offset is out of bounds");
        }
        node += value;
    }
    throw ToolError(ExitCode::format, "packed TBL Huffman tree has a cycle");
}

std::vector<std::uint8_t> decode_huffman(
    const std::span<const std::uint8_t> source,
    const std::size_t output_size,
    const std::uint8_t repeat_bits) {
    auto stream = parse_huffman(source);
    BitReader reader(stream.bits);
    std::vector<std::uint8_t> output;
    output.reserve(output_size);
    while (output.size() < output_size) {
        const auto symbol = huffman_symbol(stream.nodes, reader);
        output.push_back(symbol);
        if (repeat_bits == 0U) { continue; }
        if (reader.bit() == 0U) { continue; }
        std::size_t repeats = 0U;
        for (std::uint8_t index = 0U; index < repeat_bits; ++index) {
            repeats = (repeats << 1U) | reader.bit();
        }
        ++repeats;
        if (repeats > output_size - output.size()) {
            throw ToolError(ExitCode::format, "packed TBL repeat overruns declared output");
        }
        output.insert(output.end(), repeats, symbol);
    }
    return output;
}

std::vector<std::uint8_t> decode_rle1(
    const std::span<const std::uint8_t> source, const std::size_t output_size) {
    if (source.empty()) { throw ToolError(ExitCode::format, "packed TBL RLE stream is empty"); }
    const auto marker = source[0];
    std::size_t cursor = 1U;
    std::vector<std::uint8_t> output;
    output.reserve(output_size);
    while (output.size() < output_size) {
        if (cursor >= source.size()) {
            throw ToolError(ExitCode::format, "packed TBL RLE stream is truncated");
        }
        auto value = source[cursor++];
        std::size_t count = 1U;
        if (value == marker) {
            if (cursor >= source.size()) {
                throw ToolError(ExitCode::format, "packed TBL RLE escape is truncated");
            }
            value = source[cursor++];
            if (value != marker) {
                if (cursor >= source.size()) {
                    throw ToolError(ExitCode::format, "packed TBL RLE run is truncated");
                }
                count = static_cast<std::size_t>(source[cursor++]) + 4U;
            }
        }
        if (count > output_size - output.size()) {
            throw ToolError(ExitCode::format, "packed TBL RLE run overruns declared output");
        }
        output.insert(output.end(), count, value);
    }
    return output;
}

std::vector<std::uint8_t> decode_packed(
    const std::span<const std::uint8_t> source,
    const std::size_t output_size,
    const std::uint8_t algorithm) {
    switch (algorithm) {
    case 1U: return decode_rle1(source, output_size);
    case 2U: return decode_huffman(source, output_size, 0U);
    case 6U: return decode_huffman(source, output_size, 2U);
    case 8U: return decode_huffman(source, output_size, 4U);
    default: return {};
    }
}

std::vector<std::uint8_t> expand_key(
    std::span<const std::uint8_t> source, const std::size_t period) {
    if (period < 4U) {
        throw ToolError(ExitCode::internal, "TBL key period is too small");
    }
    const std::array<std::uint8_t, 2> default_source{'E', 'Q'};
    if (source.empty()) { source = default_source; }

    // The original allocator rounded these buffers and its loop touched one byte
    // beyond the nominal odd period. Keep one explicit scratch byte, then return
    // exactly the observed period without relying on allocator behavior.
    std::vector<std::uint8_t> current(period + 1U, 0U);
    std::vector<std::uint8_t> next(period + 1U, 0U);
    auto length = std::min(source.size(), period / 2U - 1U);
    std::copy_n(source.begin(), length, current.begin());

    std::uint16_t state = static_cast<std::uint16_t>(length);
    for (std::size_t round = 0U; round < 4U; ++round) {
        state = static_cast<std::uint16_t>(
            static_cast<std::uint32_t>(state) * static_cast<std::uint32_t>(state));
    }

    while (true) {
        const auto half = period / 2U;
        length = std::min(length, half);
        state = static_cast<std::uint16_t>(
            static_cast<std::uint32_t>(state) * static_cast<std::uint32_t>(length));
        for (std::size_t index = 0U; index < length; ++index) {
            const auto value = current[index];
            state = static_cast<std::uint16_t>(state + value);
            state = static_cast<std::uint16_t>(
                static_cast<std::uint32_t>(state) * static_cast<std::uint32_t>(state));
            state = static_cast<std::uint16_t>(state + value);
            next[index * 2U] = static_cast<std::uint8_t>(state & 0xffU);
            next[index * 2U + 1U] = static_cast<std::uint8_t>(state >> 8U);
        }
        length *= 2U;
        std::swap(current, next);
        if (length / 2U == half) { break; }
    }

    auto remaining = period - length;
    while (remaining > 0U) {
        current[length] = next[remaining];
        ++length;
        --remaining;
    }
    current.resize(period);
    return current;
}

std::vector<std::uint8_t> name_bytes(const std::string_view name) {
    if (name.empty()) {
        throw ToolError(ExitCode::format, "TBL transform key name is empty");
    }
    if (name.size() > 4096U || name.find('\0') != std::string_view::npos) {
        throw ToolError(ExitCode::format, "TBL transform key name is invalid or too long");
    }
    return std::vector<std::uint8_t>(name.begin(), name.end());
}

std::string hash_bytes(const std::span<const std::uint8_t> bytes) {
    mh::common::Sha256 hash;
    hash.update(bytes.data(), bytes.size());
    return hash.finish_hex();
}

std::filesystem::file_status symlink_status(
    const std::filesystem::path& path, const std::string_view description) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) {
        return std::filesystem::file_status(std::filesystem::file_type::not_found);
    }
    if (error) {
        throw ToolError(ExitCode::input,
            "cannot inspect " + std::string(description) + ": " + error.message());
    }
    return status;
}

} // namespace

std::vector<std::uint8_t> decode_tbl_transform(
    const std::span<const std::uint8_t> encoded, const std::string_view key_name) {
    auto result = std::vector<std::uint8_t>(encoded.begin(), encoded.end());
    const auto primary = expand_key(primary_key_source, primary_key_period);
    const auto name = name_bytes(key_name);
    const auto secondary = expand_key(name, name_key_period);

    std::uint8_t accumulator = 0U;
    for (std::size_t index = 0U; index < result.size(); ++index) {
        std::uint32_t value = result[index];
        value -= primary[index % primary.size()];
        value -= secondary[index % secondary.size()];
        value -= accumulator;
        const auto decoded = static_cast<std::uint8_t>(value);
        accumulator = static_cast<std::uint8_t>(accumulator + decoded);
        result[index] = decoded;
    }

    accumulator = 0U;
    for (std::size_t reverse = 0U; reverse < result.size(); ++reverse) {
        const auto index = result.size() - reverse - 1U;
        std::uint32_t value = result[index];
        value -= accumulator;
        const auto decoded = static_cast<std::uint8_t>(value);
        accumulator = static_cast<std::uint8_t>(accumulator + decoded);
        result[index] = decoded;
    }
    return result;
}

TblEnvelope read_tbl(
    const std::filesystem::path& path,
    std::string key_name,
    const std::uint64_t maximum_file_bytes,
    const std::uint64_t maximum_decoded_bytes) {
    if (maximum_file_bytes < 5U) {
        throw ToolError(ExitCode::usage, "TBL maximum file size must be at least five bytes");
    }
    if (maximum_decoded_bytes == 0U) {
        throw ToolError(ExitCode::usage, "TBL maximum decoded size must be nonzero");
    }
    const auto status = symlink_status(path, "TBL input");
    if (std::filesystem::is_symlink(status) || !std::filesystem::is_regular_file(status)) {
        throw ToolError(ExitCode::input, "TBL input is not a plain file: " + path.string());
    }
    const auto size = std::filesystem::file_size(path);
    if (size < 5U) {
        throw ToolError(ExitCode::format, "TBL file is shorter than header plus payload");
    }
    if (size > maximum_file_bytes
        || size - 4U > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        throw ToolError(ExitCode::format, "TBL file exceeds the configured size bound");
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input) { throw ToolError(ExitCode::input, "cannot open TBL input: " + path.string()); }
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
        throw ToolError(ExitCode::input, "short read while opening TBL input");
    }
    if (bytes[0] != 'T' || bytes[1] != 'B' || bytes[2] != 'L') {
        throw ToolError(ExitCode::format, "TBL input has no TBL envelope signature");
    }

    TblEnvelope result;
    result.source_path = std::filesystem::absolute(path).lexically_normal();
    result.flags = bytes[3];
    result.source_bytes = size;
    result.encoded_payload_bytes = size - 4U;
    result.transform_present = (result.flags & tbl_transform_flag) != 0U;
    result.packed_present = (result.flags & tbl_packed_flag) != 0U;
    result.source_sha256 = hash_bytes(bytes);
    std::vector<std::uint8_t> encoded(bytes.begin() + 4, bytes.end());
    result.encoded_payload_sha256 = hash_bytes(encoded);
    if (key_name.empty()) { key_name = path.filename().string(); }
    result.key_name = std::move(key_name);

    if (result.flags != 0x05U && result.flags != 0x07U) {
        result.decode_status = "unsupported_flags";
        result.payload = std::move(encoded);
        return result;
    }
    result.payload = result.transform_present
        ? decode_tbl_transform(encoded, result.key_name) : std::move(encoded);
    result.outer_transform_decoded = result.transform_present;
    result.transformed_payload_sha256 = hash_bytes(result.payload);
    result.transformed_header.assign(result.payload.begin(),
        result.payload.begin() + std::min<std::size_t>(32U, result.payload.size()));
    if (result.packed_present) {
        if (result.payload.size() < 4U) {
            throw ToolError(ExitCode::format,
                "packed TBL payload is shorter than its four-byte header");
        }
        const std::uint32_t packed_word = little_u32(result.payload);
        result.packed_output_bytes = packed_word & 0x07ffffffU;
        result.packed_algorithm = static_cast<std::uint8_t>((packed_word >> 27U) & 0x0fU);
        result.packed_postprocess = (packed_word & 0x80000000U) != 0U;
        if (result.packed_output_bytes == 0U) {
            throw ToolError(ExitCode::format, "packed TBL declares an empty output");
        }
        if (result.packed_output_bytes > maximum_decoded_bytes) {
            throw ToolError(ExitCode::format,
                "packed TBL output exceeds the configured decoded-size bound");
        }
        if (result.packed_postprocess) {
            result.decode_status = "unsupported_packed_postprocess";
            return result;
        }
        const auto decoded = decode_packed(
            std::span<const std::uint8_t>(result.payload).subspan(4U),
            result.packed_output_bytes, result.packed_algorithm);
        if (decoded.empty()) {
            result.decode_status = (result.packed_algorithm <= 8U
                ? "unsupported_packed_algorithm_" : "unknown_packed_algorithm_")
                + std::to_string(result.packed_algorithm);
            return result;
        }
        result.payload = decoded;
        result.decoded = true;
        result.decode_status = "decoded_transform_packed_codec_"
            + std::to_string(result.packed_algorithm);
        result.decoded_payload_sha256 = hash_bytes(result.payload);
        return result;
    }
    result.decoded = true;
    result.decode_status = "decoded_transform_v1";
    result.decoded_payload_sha256 = hash_bytes(result.payload);
    return result;
}

} // namespace mh::content
