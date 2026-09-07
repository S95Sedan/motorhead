#include <content/formats/divi_video_archive.hpp>
#include <content/export/image_export.hpp>

#include <core/error.hpp>
#include <core/crypto/sha256.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace mh::content {
namespace {

using mh::common::ExitCode;
using mh::common::ToolError;

constexpr std::uint64_t divi_header_bytes = 12U;
constexpr std::uint64_t divi_record_bytes = 12U;
constexpr std::uint64_t smacker_header_bytes = 104U;
constexpr std::uint32_t smacker_local_tree_max_depth = 27U;
constexpr std::uint32_t smacker_big_tree_max_depth = 500U;
constexpr std::array<std::uint32_t, 64U> smacker_block_runs = {
    1U,  2U,  3U,  4U,  5U,  6U,  7U,  8U,   9U,   10U,  11U,   12U,  13U,
    14U, 15U, 16U, 17U, 18U, 19U, 20U, 21U,  22U,  23U,  24U,   25U,  26U,
    27U, 28U, 29U, 30U, 31U, 32U, 33U, 34U,  35U,  36U,  37U,   38U,  39U,
    40U, 41U, 42U, 43U, 44U, 45U, 46U, 47U,  48U,  49U,  50U,   51U,  52U,
    53U, 54U, 55U, 56U, 57U, 58U, 59U, 128U, 256U, 512U, 1024U, 2048U};

std::uint32_t le32(const std::span<const std::uint8_t> bytes,
                   const std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

bool all_zero(const std::span<const std::uint8_t> bytes) {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](const std::uint8_t value) { return value == 0U; });
}

std::uint64_t checked_add(const std::uint64_t left, const std::uint64_t right,
                          const std::string &message) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    throw ToolError(ExitCode::format, message);
  }
  return left + right;
}

std::uint64_t checked_multiply(const std::uint64_t left,
                               const std::uint64_t right,
                               const std::string &message) {
  if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
    throw ToolError(ExitCode::format, message);
  }
  return left * right;
}

class LsbBitReader {
public:
  explicit LsbBitReader(const std::span<const std::uint8_t> bytes)
      : bytes_(bytes) {}

  [[nodiscard]] std::uint64_t position() const noexcept {
    return bit_position_;
  }
  [[nodiscard]] std::uint64_t size_bits() const noexcept {
    return static_cast<std::uint64_t>(bytes_.size()) * 8U;
  }

  std::uint32_t read(const std::uint32_t count) {
    if (count > 32U ||
        count > size_bits() - std::min(bit_position_, size_bits())) {
      throw ToolError(ExitCode::format, "Smacker LSB bitstream is truncated");
    }
    std::uint32_t value = 0U;
    for (std::uint32_t bit = 0U; bit < count; ++bit) {
      const auto byte_index = static_cast<std::size_t>(bit_position_ >> 3U);
      const auto bit_index = static_cast<unsigned>(bit_position_ & 7U);
      value |=
          static_cast<std::uint32_t>((bytes_[byte_index] >> bit_index) & 1U)
          << bit;
      ++bit_position_;
    }
    return value;
  }

private:
  std::span<const std::uint8_t> bytes_;
  std::uint64_t bit_position_ = 0U;
};

struct LocalHuffmanNode {
  bool leaf = false;
  std::uint8_t value = 0U;
  std::array<std::uint32_t, 2U> children{};
};

struct LocalHuffmanTree {
  bool present = false;
  std::uint8_t constant = 0U;
  std::vector<LocalHuffmanNode> nodes;
  std::uint32_t leaf_count = 0U;
};

struct ValueHuffmanNode {
  bool leaf = false;
  std::uint16_t value = 0U;
  std::int8_t escape_slot = -1;
  std::array<std::uint32_t, 2U> children{};
};

struct ValueHuffmanTree {
  std::vector<ValueHuffmanNode> nodes;
  std::array<std::uint16_t, 3U> history{};
};

std::uint32_t parse_local_node(LsbBitReader &bits, LocalHuffmanTree &tree,
                               const std::uint32_t depth) {
  if (depth > smacker_local_tree_max_depth || tree.nodes.size() >= 511U) {
    throw ToolError(ExitCode::format,
                    "Smacker local Huffman tree exceeds its bounds");
  }
  const auto index = static_cast<std::uint32_t>(tree.nodes.size());
  tree.nodes.emplace_back();
  if (bits.read(1U) == 0U) {
    if (tree.leaf_count >= 256U) {
      throw ToolError(ExitCode::format,
                      "Smacker local Huffman tree has too many leaves");
    }
    tree.nodes[index].leaf = true;
    tree.nodes[index].value = static_cast<std::uint8_t>(bits.read(8U));
    ++tree.leaf_count;
    return index;
  }
  const auto left = parse_local_node(bits, tree, depth + 1U);
  const auto right = parse_local_node(bits, tree, depth + 1U);
  tree.nodes[index].children = {left, right};
  return index;
}

LocalHuffmanTree parse_local_tree(LsbBitReader &bits) {
  LocalHuffmanTree tree;
  tree.present = bits.read(1U) != 0U;
  if (!tree.present) {
    return tree;
  }
  static_cast<void>(parse_local_node(bits, tree, 0U));
  static_cast<void>(bits.read(1U)); // Serialized-tree terminator.
  if (tree.leaf_count == 1U) {
    tree.constant = tree.nodes.front().value;
  }
  return tree;
}

std::uint8_t decode_local_symbol(LsbBitReader &bits,
                                 const LocalHuffmanTree &tree) {
  if (!tree.present || tree.leaf_count == 1U) {
    return tree.constant;
  }
  std::uint32_t node = 0U;
  for (std::uint32_t depth = 0U; depth <= smacker_local_tree_max_depth;
       ++depth) {
    if (node >= tree.nodes.size()) {
      throw ToolError(ExitCode::format,
                      "Smacker local Huffman code leaves its tree");
    }
    const auto &current = tree.nodes[node];
    if (current.leaf) {
      return current.value;
    }
    node = current.children[bits.read(1U)];
  }
  throw ToolError(ExitCode::format,
                  "Smacker local Huffman code exceeds maximum depth");
}

std::uint32_t parse_big_node(LsbBitReader &bits, const LocalHuffmanTree &low,
                             const LocalHuffmanTree &high,
                             const std::array<std::uint16_t, 3U> &escapes,
                             const std::uint32_t capacity,
                             SmackerHuffmanTreeInfo &info,
                             ValueHuffmanTree &tree,
                             const std::uint32_t depth) {
  if (depth > smacker_big_tree_max_depth ||
      info.value_node_count + info.value_leaf_count >= capacity) {
    throw ToolError(ExitCode::format,
                    "Smacker value Huffman tree exceeds its declared size");
  }
  const auto node_index = static_cast<std::uint32_t>(tree.nodes.size());
  tree.nodes.emplace_back();
  info.maximum_depth = std::max(info.maximum_depth, depth);
  if (bits.read(1U) == 0U) {
    const auto value = static_cast<std::uint16_t>(
        decode_local_symbol(bits, low) |
        (static_cast<std::uint16_t>(decode_local_symbol(bits, high)) << 8U));
    ++info.value_leaf_count;
    tree.nodes[node_index].leaf = true;
    tree.nodes[node_index].value = value;
    for (std::size_t index = 0U; index < escapes.size(); ++index) {
      if (value == escapes[index]) {
        ++info.escape_leaf_counts[index];
        tree.nodes[node_index].escape_slot = static_cast<std::int8_t>(index);
        break;
      }
    }
    return node_index;
  }
  ++info.value_node_count;
  const auto left = parse_big_node(bits, low, high, escapes, capacity, info,
                                   tree, depth + 1U);
  const auto right = parse_big_node(bits, low, high, escapes, capacity, info,
                                    tree, depth + 1U);
  tree.nodes[node_index].children = {left, right};
  return node_index;
}

std::array<ValueHuffmanTree, 4U>
parse_smacker_trees(const std::span<const std::uint8_t> bytes,
                    SmackerStreamInfo &stream) {
  LsbBitReader bits(bytes);
  std::array<ValueHuffmanTree, 4U> trees;
  for (std::size_t tree_index = 0U; tree_index < trees.size(); ++tree_index) {
    auto &info = stream.huffman_trees[tree_index];
    auto &tree = trees[tree_index];
    const auto start = bits.position();
    info.present = bits.read(1U) != 0U;
    if (info.present) {
      const auto low = parse_local_tree(bits);
      const auto high = parse_local_tree(bits);
      info.low_symbol_count = low.present ? low.leaf_count : 1U;
      info.high_symbol_count = high.present ? high.leaf_count : 1U;
      std::array<std::uint16_t, 3U> escapes{};
      for (auto &escape : escapes) {
        escape = static_cast<std::uint16_t>(bits.read(16U));
      }
      const auto capacity = (info.declared_bytes + 3U) >> 2U;
      static_cast<void>(
          parse_big_node(bits, low, high, escapes, capacity, info, tree, 0U));
      static_cast<void>(bits.read(1U)); // Serialized-tree terminator.
    } else {
      tree.nodes.push_back(ValueHuffmanNode{true, 0U, -1, {0U, 0U}});
    }
    info.bits_consumed = bits.position() - start;
  }
  stream.tree_bits_consumed = bits.position();
  stream.tree_padding_bits = bits.size_bits() - bits.position();
  while (bits.position() < bits.size_bits()) {
    if (bits.read(1U) != 0U) {
      throw ToolError(
          ExitCode::format,
          "Smacker Huffman tree section has nonzero trailing padding");
    }
  }
  return trees;
}

std::uint16_t decode_value(LsbBitReader &bits, ValueHuffmanTree &tree) {
  std::uint32_t node_index = 0U;
  for (std::uint32_t depth = 0U; depth <= smacker_big_tree_max_depth; ++depth) {
    if (node_index >= tree.nodes.size()) {
      throw ToolError(ExitCode::format,
                      "Smacker frame code leaves its value tree");
    }
    const auto &node = tree.nodes[node_index];
    if (node.leaf) {
      const auto value =
          node.escape_slot >= 0
              ? tree.history[static_cast<std::size_t>(node.escape_slot)]
              : node.value;
      if (value != tree.history[0]) {
        tree.history[2] = tree.history[1];
        tree.history[1] = tree.history[0];
        tree.history[0] = value;
      }
      return value;
    }
    node_index = node.children[bits.read(1U)];
  }
  throw ToolError(ExitCode::format,
                  "Smacker frame code exceeds maximum tree depth");
}

void decode_video_commands(const std::span<const std::uint8_t> bytes,
                           std::array<ValueHuffmanTree, 4U> &trees,
                           const std::uint32_t width,
                           const std::uint32_t height,
                           const std::span<std::uint8_t> indexed_frame,
                           SmackerFrameInfo &frame, SmackerStreamInfo &stream) {
  if ((width & 3U) != 0U || (height & 3U) != 0U) {
    throw ToolError(ExitCode::format,
                    "Smacker dimensions do not form complete 4x4 blocks");
  }
  for (auto &tree : trees) {
    tree.history.fill(0U);
  }
  const auto total_blocks =
      static_cast<std::uint64_t>(width / 4U) * (height / 4U);
  if (indexed_frame.size() != static_cast<std::uint64_t>(width) * height) {
    throw ToolError(ExitCode::format,
                    "Smacker indexed frame has an invalid allocation");
  }
  const auto block_width = width / 4U;
  std::uint64_t block = 0U;
  LsbBitReader bits(bytes);
  while (block < total_blocks) {
    const auto type = decode_value(bits, trees[3]);
    const auto kind = static_cast<std::size_t>(type & 3U);
    const auto declared_run = smacker_block_runs[(type >> 2U) & 0x3fU];
    const auto run = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(declared_run, total_blocks - block));
    ++frame.block_command_count;
    frame.block_counts[kind] += run;
    if (kind == 0U) {
      for (std::uint32_t index = 0U; index < run; ++index) {
        const auto colors = decode_value(bits, trees[1]);
        auto map = decode_value(bits, trees[0]);
        const auto low = static_cast<std::uint8_t>(colors & 0xffU);
        const auto high = static_cast<std::uint8_t>(colors >> 8U);
        const auto block_x =
            static_cast<std::uint32_t>(block % block_width) * 4U;
        const auto block_y =
            static_cast<std::uint32_t>(block / block_width) * 4U;
        for (std::uint32_t y = 0U; y < 4U; ++y) {
          for (std::uint32_t x = 0U; x < 4U; ++x) {
            indexed_frame[static_cast<std::size_t>(block_y + y) * width +
                          block_x + x] = (map & 1U) != 0U ? high : low;
            map >>= 1U;
          }
        }
        ++block;
      }
    } else if (kind == 1U) {
      for (std::uint32_t index = 0U; index < run; ++index) {
        const auto block_x =
            static_cast<std::uint32_t>(block % block_width) * 4U;
        const auto block_y =
            static_cast<std::uint32_t>(block / block_width) * 4U;
        for (std::uint32_t y = 0U; y < 4U; ++y) {
          const auto right = decode_value(bits, trees[2]);
          const auto left = decode_value(bits, trees[2]);
          const auto row =
              static_cast<std::size_t>(block_y + y) * width + block_x;
          indexed_frame[row] = static_cast<std::uint8_t>(left & 0xffU);
          indexed_frame[row + 1U] = static_cast<std::uint8_t>(left >> 8U);
          indexed_frame[row + 2U] = static_cast<std::uint8_t>(right & 0xffU);
          indexed_frame[row + 3U] = static_cast<std::uint8_t>(right >> 8U);
        }
        ++block;
      }
    } else if (kind == 2U) {
      block += run;
    } else {
      const auto color = static_cast<std::uint8_t>(type >> 8U);
      for (std::uint32_t index = 0U; index < run; ++index) {
        const auto block_x =
            static_cast<std::uint32_t>(block % block_width) * 4U;
        const auto block_y =
            static_cast<std::uint32_t>(block / block_width) * 4U;
        for (std::uint32_t y = 0U; y < 4U; ++y) {
          const auto row =
              static_cast<std::size_t>(block_y + y) * width + block_x;
          std::fill_n(indexed_frame.begin() + static_cast<std::ptrdiff_t>(row),
                      4U, color);
        }
        ++block;
      }
    }
  }
  frame.video_bits_consumed = bits.position();
  frame.video_trailing_bits = bits.size_bits() - bits.position();
  while (bits.position() < bits.size_bits()) {
    frame.video_nonzero_trailing_bits += bits.read(1U);
  }
  stream.video_bits_consumed += frame.video_bits_consumed;
  stream.video_trailing_bits += frame.video_trailing_bits;
  stream.video_nonzero_trailing_bits += frame.video_nonzero_trailing_bits;
  stream.block_command_count += frame.block_command_count;
  for (std::size_t kind = 0U; kind < frame.block_counts.size(); ++kind) {
    stream.block_counts[kind] += frame.block_counts[kind];
  }
}

void decode_audio_packet(const std::span<const std::uint8_t> packet,
                         const std::uint32_t maximum_uncompressed_bytes,
                         SmackerFrameInfo &frame, SmackerStreamInfo &stream,
                         mh::common::Sha256 &decoded_pcm_hash,
                         std::vector<std::uint8_t> *captured_pcm,
                         const SmackerStreamVisitors *visitors,
                         bool &continue_streaming) {
  if (packet.size() <= 4U) {
    throw ToolError(ExitCode::format, "Smacker audio packet is too small");
  }
  const auto uncompressed_bytes = le32(packet, 0U);
  if (uncompressed_bytes > maximum_uncompressed_bytes ||
      uncompressed_bytes > (1U << 24U)) {
    throw ToolError(ExitCode::format,
                    "Smacker audio output exceeds its declared bounds");
  }

  LsbBitReader bits(packet.subspan(4U));
  if (bits.read(1U) == 0U) {
    frame.audio_bits_consumed = bits.position();
    frame.audio_trailing_bits = bits.size_bits() - bits.position();
    while (bits.position() < bits.size_bits()) {
      frame.audio_nonzero_trailing_bits += bits.read(1U);
    }
    stream.audio_bits_consumed += frame.audio_bits_consumed;
    stream.audio_trailing_bits += frame.audio_trailing_bits;
    stream.audio_nonzero_trailing_bits += frame.audio_nonzero_trailing_bits;
    return;
  }

  const auto stereo = bits.read(1U) != 0U;
  const auto sixteen_bit = bits.read(1U) != 0U;
  if (!stereo || !sixteen_bit || stream.audio_channels != 2U ||
      stream.audio_bits_per_sample != 16U) {
    throw ToolError(ExitCode::format,
                    "Smacker audio packet is outside the stereo 16-bit subset");
  }
  if (uncompressed_bytes < 4U || (uncompressed_bytes & 3U) != 0U) {
    throw ToolError(ExitCode::format,
                    "Smacker audio output is not complete stereo samples");
  }

  std::array<LocalHuffmanTree, 4U> trees;
  for (auto &tree : trees) {
    static_cast<void>(bits.read(1U)); // Serialized-tree marker.
    tree.present = true;
    static_cast<void>(parse_local_node(bits, tree, 0U));
    static_cast<void>(bits.read(1U)); // Serialized-tree terminator.
    if (tree.leaf_count == 1U) {
      tree.constant = tree.nodes.front().value;
    }
  }

  std::array<std::uint16_t, 2U> predictors{};
  for (std::int32_t channel = 1; channel >= 0; --channel) {
    const auto encoded = static_cast<std::uint16_t>(bits.read(16U));
    predictors[static_cast<std::size_t>(channel)] =
        static_cast<std::uint16_t>((encoded >> 8U) | (encoded << 8U));
  }

  std::vector<std::uint8_t> pcm(uncompressed_bytes);
  auto write_sample = [&pcm](const std::size_t sample,
                             const std::uint16_t value) {
    pcm[sample * 2U] = static_cast<std::uint8_t>(value & 0xffU);
    pcm[sample * 2U + 1U] = static_cast<std::uint8_t>(value >> 8U);
  };
  write_sample(0U, predictors[0U]);
  write_sample(1U, predictors[1U]);
  const auto sample_count = static_cast<std::size_t>(uncompressed_bytes / 2U);
  for (std::size_t sample = 2U; sample < sample_count; ++sample) {
    const auto channel = sample & 1U;
    const auto tree_index = channel * 2U;
    const auto delta = static_cast<std::uint16_t>(
        decode_local_symbol(bits, trees[tree_index]) |
        (static_cast<std::uint16_t>(
             decode_local_symbol(bits, trees[tree_index + 1U]))
         << 8U));
    predictors[channel] =
        static_cast<std::uint16_t>(predictors[channel] + delta);
    write_sample(sample, predictors[channel]);
  }

  frame.audio_uncompressed_bytes = uncompressed_bytes;
  frame.audio_bits_consumed = bits.position();
  frame.audio_trailing_bits = bits.size_bits() - bits.position();
  while (bits.position() < bits.size_bits()) {
    frame.audio_nonzero_trailing_bits += bits.read(1U);
  }
  stream.audio_uncompressed_bytes += uncompressed_bytes;
  stream.audio_bits_consumed += frame.audio_bits_consumed;
  stream.audio_trailing_bits += frame.audio_trailing_bits;
  stream.audio_nonzero_trailing_bits += frame.audio_nonzero_trailing_bits;
  decoded_pcm_hash.update(pcm.data(), pcm.size());
  if (captured_pcm != nullptr) {
    captured_pcm->insert(captured_pcm->end(), pcm.begin(), pcm.end());
  }
  if (visitors != nullptr && visitors->audio &&
      !visitors->audio(stream.audio_sample_rate, stream.audio_channels,
                       stream.audio_bits_per_sample, pcm)) {
    continue_streaming = false;
  }
}

SmackerStreamInfo
parse_smacker_envelope(const std::span<const std::uint8_t> bytes,
                       const std::uint32_t capture_frame_index =
                           std::numeric_limits<std::uint32_t>::max(),
                       SmackerDecodedFrame *captured_frame = nullptr,
                       SmackerDecodedAudio *captured_audio = nullptr,
                       const SmackerStreamVisitors *visitors = nullptr,
                       bool *stream_completed = nullptr,
                       const std::uint32_t stream_entry_index = 0U) {
  if (stream_completed != nullptr) {
    *stream_completed = false;
  }
  if (bytes.size() < smacker_header_bytes ||
      (!std::equal(bytes.begin(), bytes.begin() + 4U, "SMK2") &&
       !std::equal(bytes.begin(), bytes.begin() + 4U, "SMK4"))) {
    throw ToolError(ExitCode::format,
                    "DIVI entry does not contain a bounded SMK2/SMK4 stream");
  }
  SmackerStreamInfo stream;
  stream.signature.assign(reinterpret_cast<const char *>(bytes.data()), 4U);
  stream.width = le32(bytes, 4U);
  stream.height = le32(bytes, 8U);
  stream.frame_count = le32(bytes, 12U);
  stream.frame_rate_raw = static_cast<std::int32_t>(le32(bytes, 16U));
  stream.flags = le32(bytes, 20U);
  if (stream.width == 0U || stream.height == 0U || stream.width > 16384U ||
      stream.height > 16384U || stream.frame_count == 0U ||
      stream.frame_count > 10000000U || stream.frame_rate_raw == 0) {
    throw ToolError(ExitCode::format, "Smacker dimensions, frame count, or "
                                      "timing are outside the bounded subset");
  }
  stream.frame_duration_units =
      stream.frame_rate_raw < 0
          ? static_cast<std::uint64_t>(
                -static_cast<std::int64_t>(stream.frame_rate_raw))
          : static_cast<std::uint64_t>(stream.frame_rate_raw) * 100U;
  stream.duration_units =
      checked_multiply(stream.frame_count, stream.frame_duration_units,
                       "Smacker presentation duration overflows");
  if (captured_frame != nullptr && capture_frame_index >= stream.frame_count) {
    throw ToolError(ExitCode::usage,
                    "requested Smacker frame index is out of range");
  }
  for (std::size_t index = 0U; index < stream.audio_sizes.size(); ++index) {
    stream.audio_sizes[index] = le32(bytes, 24U + index * 4U);
  }
  stream.tree_bytes = le32(bytes, 52U);
  for (std::size_t index = 0U; index < stream.huffman_trees.size(); ++index) {
    stream.huffman_trees[index].declared_bytes = le32(bytes, 56U + index * 4U);
  }
  for (std::size_t index = 0U; index < stream.audio_rates.size(); ++index) {
    stream.audio_rates[index] = le32(bytes, 72U + index * 4U);
    if (stream.audio_rates[index] != 0U || stream.audio_sizes[index] != 0U) {
      ++stream.audio_track_count;
    }
  }
  if (stream.audio_track_count != 0U) {
    if (stream.audio_track_count != 1U || stream.audio_sizes[0U] == 0U ||
        stream.audio_rates[0U] == 0U ||
        !std::all_of(stream.audio_sizes.begin() + 1U, stream.audio_sizes.end(),
                     [](const std::uint32_t value) { return value == 0U; }) ||
        !std::all_of(stream.audio_rates.begin() + 1U, stream.audio_rates.end(),
                     [](const std::uint32_t value) { return value == 0U; })) {
      throw ToolError(
          ExitCode::format,
          "Smacker audio tracks are outside the single-track subset");
    }
    stream.audio_sample_rate = stream.audio_rates[0U] & 0x00ffffffU;
    stream.audio_flags =
        static_cast<std::uint8_t>(stream.audio_rates[0U] >> 24U);
    if (stream.audio_flags != 0xf0U || stream.audio_sample_rate < 8000U ||
        stream.audio_sample_rate > 192000U) {
      throw ToolError(
          ExitCode::format,
          "Smacker audio header is outside the packed stereo 16-bit subset");
    }
    stream.audio_channels = 2U;
    stream.audio_bits_per_sample = 16U;
  }
  if (captured_audio != nullptr && stream.audio_track_count == 0U) {
    throw ToolError(ExitCode::format,
                    "requested Smacker stream has no audio track");
  }
  stream.frame_table_bytes = checked_multiply(
      stream.frame_count, 4U, "Smacker frame-size table overflows");
  stream.frame_type_bytes = stream.frame_count;
  stream.frame_data_offset =
      checked_add(smacker_header_bytes, stream.frame_table_bytes,
                  "Smacker frame-size table overflows");
  stream.frame_data_offset =
      checked_add(stream.frame_data_offset, stream.frame_type_bytes,
                  "Smacker frame-type table overflows");
  stream.frame_data_offset =
      checked_add(stream.frame_data_offset, stream.tree_bytes,
                  "Smacker tree section overflows");
  if (stream.frame_data_offset > bytes.size()) {
    throw ToolError(ExitCode::format,
                    "Smacker tables extend beyond the DIVI entry");
  }
  const auto tree_offset =
      static_cast<std::size_t>(smacker_header_bytes + stream.frame_table_bytes +
                               stream.frame_type_bytes);
  auto huffman_trees = parse_smacker_trees(
      bytes.subspan(tree_offset, stream.tree_bytes), stream);
  std::uint64_t frame_bytes = 0U;
  stream.frames.reserve(stream.frame_count);
  std::array<std::uint8_t, 256U * 3U> palette{};
  std::vector<std::uint8_t> indexed_frame(
      static_cast<std::size_t>(stream.width) * stream.height, 0U);
  mh::common::Sha256 decoded_indexed_hash;
  mh::common::Sha256 decoded_pcm_hash;
  auto frame_offset = stream.frame_data_offset;
  std::uint64_t audio_sample_cursor = 0U;
  bool continue_streaming = true;
  const auto frame_types_offset =
      smacker_header_bytes + stream.frame_table_bytes;
  for (std::uint32_t index = 0U; index < stream.frame_count; ++index) {
    const auto encoded_size = le32(
        bytes, static_cast<std::size_t>(smacker_header_bytes + index * 4ULL));
    const auto byte_size = encoded_size & ~std::uint32_t{3U};
    const auto size_flags = static_cast<std::uint8_t>(encoded_size & 3U);
    const auto type =
        bytes[static_cast<std::size_t>(frame_types_offset + index)];
    if (size_flags != 0U || (type & ~std::uint8_t{3U}) != 0U) {
      throw ToolError(
          ExitCode::format,
          "Smacker frame uses flags outside the observed Motorhead subset");
    }
    SmackerFrameInfo frame{frame_offset, byte_size, size_flags, type};
    frame.keyframe = index == 0U || (size_flags & 1U) != 0U;
    frame.presentation_time_units =
        checked_multiply(index, stream.frame_duration_units,
                         "Smacker frame presentation time overflows");
    frame.audio_start_sample_frame = audio_sample_cursor;
    if (frame.keyframe) {
      ++stream.keyframe_count;
    }
    const auto frame_start = static_cast<std::size_t>(frame_offset);
    std::uint64_t local_offset = 0U;
    if ((type & 1U) != 0U) {
      ++stream.palette_frame_count;
      const auto packet_bytes =
          static_cast<std::uint32_t>(bytes[frame_start]) * 4U;
      if (packet_bytes < 4U || packet_bytes > byte_size) {
        throw ToolError(ExitCode::format,
                        "Smacker palette packet is outside its frame");
      }
      frame.palette_packet_bytes = packet_bytes;
      stream.palette_packet_bytes += packet_bytes;
      const auto previous_palette = palette;
      auto cursor = frame_start + 1U;
      const auto packet_end = frame_start + packet_bytes;
      std::size_t color = 0U;
      while (color < 256U) {
        if (cursor >= packet_end) {
          throw ToolError(ExitCode::format,
                          "Smacker palette commands end before color 256");
        }
        const auto command = bytes[cursor++];
        if ((command & 0x80U) != 0U) {
          const auto count = static_cast<std::size_t>(command & 0x7fU) + 1U;
          if (count > 256U - color) {
            throw ToolError(ExitCode::format,
                            "Smacker palette skip exceeds color 256");
          }
          color += count;
          stream.palette_skip_colors += count;
        } else if ((command & 0x40U) != 0U) {
          const auto count = static_cast<std::size_t>(command & 0x3fU) + 1U;
          if (cursor >= packet_end || count > 256U - color) {
            throw ToolError(ExitCode::format,
                            "Smacker palette copy command is truncated");
          }
          const auto source = static_cast<std::size_t>(bytes[cursor++]);
          if (count > 256U - source) {
            throw ToolError(ExitCode::format,
                            "Smacker palette copy source exceeds color 256");
          }
          std::copy_n(previous_palette.begin() + source * 3U, count * 3U,
                      palette.begin() + color * 3U);
          color += count;
          stream.palette_copy_colors += count;
        } else {
          if (cursor + 2U > packet_end || bytes[cursor] > 63U ||
              bytes[cursor + 1U] > 63U) {
            throw ToolError(ExitCode::format, "Smacker direct palette color is "
                                              "truncated or outside 6-bit RGB");
          }
          palette[color * 3U] = command;
          palette[color * 3U + 1U] = bytes[cursor++];
          palette[color * 3U + 2U] = bytes[cursor++];
          ++color;
          ++stream.palette_direct_colors;
        }
      }
      if (packet_end - cursor > 3U ||
          !all_zero(bytes.subspan(cursor, packet_end - cursor))) {
        throw ToolError(ExitCode::format,
                        "Smacker palette packet has noncanonical padding");
      }
      local_offset += packet_bytes;
    }
    if ((type & 2U) != 0U) {
      ++stream.audio_frame_count;
      if (local_offset + 4U > byte_size) {
        throw ToolError(ExitCode::format,
                        "Smacker audio chunk header is outside its frame");
      }
      const auto chunk_bytes =
          le32(bytes, frame_start + static_cast<std::size_t>(local_offset));
      if (chunk_bytes < 4U || chunk_bytes > byte_size - local_offset) {
        throw ToolError(ExitCode::format,
                        "Smacker audio chunk is outside its frame");
      }
      frame.audio_chunk_bytes = chunk_bytes;
      stream.audio_chunk_bytes += chunk_bytes;
      if (stream.audio_track_count != 0U) {
        const auto expected_sample_cursor =
            checked_multiply(frame.presentation_time_units,
                             stream.audio_sample_rate,
                             "Smacker audio delivery time overflows") /
            stream.timing_scale;
        if (expected_sample_cursor > audio_sample_cursor) {
          stream.maximum_audio_delivery_lag_samples =
              std::max(stream.maximum_audio_delivery_lag_samples,
                       expected_sample_cursor - audio_sample_cursor);
        }
        decode_audio_packet(
            bytes.subspan(frame_start + static_cast<std::size_t>(local_offset) +
                              4U,
                          chunk_bytes - 4U),
            stream.audio_sizes[0U], frame, stream, decoded_pcm_hash,
            captured_audio == nullptr ? nullptr : &captured_audio->pcm,
            visitors, continue_streaming);
        if (!continue_streaming) {
          return stream;
        }
        frame.audio_sample_frames = frame.audio_uncompressed_bytes / 4U;
        if (frame.audio_sample_frames != 0U) {
          if (stream.first_audio_frame_index == 0xffffffffU) {
            stream.first_audio_frame_index = index;
          }
          stream.last_audio_frame_index = index;
          stream.maximum_audio_packet_sample_frames =
              std::max(stream.maximum_audio_packet_sample_frames,
                       frame.audio_sample_frames);
          audio_sample_cursor =
              checked_add(audio_sample_cursor, frame.audio_sample_frames,
                          "Smacker audio sample cursor overflows");
          const auto expected_end_sample_cursor =
              checked_multiply(frame.presentation_time_units +
                                   stream.frame_duration_units,
                               stream.audio_sample_rate,
                               "Smacker audio delivery time overflows") /
              stream.timing_scale;
          if (audio_sample_cursor > expected_end_sample_cursor) {
            stream.maximum_audio_delivery_lead_samples =
                std::max(stream.maximum_audio_delivery_lead_samples,
                         audio_sample_cursor - expected_end_sample_cursor);
          }
        }
      }
      local_offset += chunk_bytes;
    }
    if (local_offset >= byte_size) {
      throw ToolError(
          ExitCode::format,
          "Smacker frame has no video payload after palette/audio chunks");
    }
    frame.video_offset = frame_offset + local_offset;
    frame.video_bytes = static_cast<std::uint32_t>(byte_size - local_offset);
    decode_video_commands(
        bytes.subspan(static_cast<std::size_t>(frame.video_offset),
                      frame.video_bytes),
        huffman_trees, stream.width, stream.height, indexed_frame, frame,
        stream);
    decoded_indexed_hash.update(indexed_frame.data(), indexed_frame.size());
    SmackerDecodedFrame streamed_frame;
    auto *output_frame =
        captured_frame != nullptr && index == capture_frame_index
            ? captured_frame
        : visitors != nullptr && visitors->video ? &streamed_frame
                                                 : nullptr;
    if (output_frame != nullptr) {
      output_frame->entry_index = stream_entry_index;
      output_frame->frame_index = index;
      output_frame->width = stream.width;
      output_frame->height = stream.height;
      output_frame->rgba.resize(indexed_frame.size() * 4U);
      for (std::size_t pixel = 0U; pixel < indexed_frame.size(); ++pixel) {
        const auto palette_index =
            static_cast<std::size_t>(indexed_frame[pixel]);
        const auto source = palette_index * 3U;
        const auto destination = pixel * 4U;
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
          const auto value = palette[source + channel];
          output_frame->rgba[destination + channel] =
              static_cast<std::uint8_t>((value << 2U) | (value >> 4U));
        }
        output_frame->rgba[destination + 3U] = 0xffU;
      }
      mh::common::Sha256 rgba_hash;
      rgba_hash.update(output_frame->rgba.data(), output_frame->rgba.size());
      output_frame->rgba_sha256 = rgba_hash.finish_hex();
    }
    if (captured_frame != nullptr && index == capture_frame_index) {
      return stream;
    }
    if (visitors != nullptr && visitors->video &&
        !visitors->video(std::move(streamed_frame))) {
      return stream;
    }
    stream.video_payload_bytes += frame.video_bytes;
    stream.minimum_video_bytes =
        stream.minimum_video_bytes == 0U
            ? frame.video_bytes
            : std::min(stream.minimum_video_bytes, frame.video_bytes);
    stream.frames.push_back(frame);
    ++stream.size_flag_counts[size_flags];
    ++stream.frame_type_counts[type];
    stream.maximum_frame_bytes =
        std::max(stream.maximum_frame_bytes, byte_size);
    frame_offset =
        checked_add(frame_offset, byte_size, "Smacker frame offset overflows");
    frame_bytes = checked_add(frame_bytes, byte_size,
                              "Smacker frame payload total overflows");
  }
  if (checked_add(stream.frame_data_offset, frame_bytes,
                  "Smacker frame partition overflows") != bytes.size()) {
    throw ToolError(ExitCode::format, "Smacker header, tables, trees, and "
                                      "frames do not partition the DIVI entry");
  }
  stream.frame_data_bytes = frame_bytes;
  if (stream.audio_track_count != 0U) {
    const auto bytes_per_sample_frame =
        static_cast<std::uint64_t>(stream.audio_channels) *
        stream.audio_bits_per_sample / 8U;
    stream.audio_sample_frames =
        stream.audio_uncompressed_bytes / bytes_per_sample_frame;
    if (stream.audio_sample_frames != audio_sample_cursor ||
        stream.first_audio_frame_index == 0xffffffffU ||
        stream.last_audio_frame_index == 0xffffffffU) {
      throw ToolError(ExitCode::format,
                      "Smacker audio packet timeline is inconsistent");
    }
    const auto timing_units = -static_cast<std::int64_t>(stream.frame_rate_raw);
    if (timing_units <= 0) {
      throw ToolError(ExitCode::format,
                      "Smacker audio synchronization requires negative timing");
    }
    auto expected_numerator =
        checked_multiply(stream.frame_count, stream.audio_sample_rate,
                         "Smacker synchronized sample count overflows");
    expected_numerator = checked_multiply(
        expected_numerator, static_cast<std::uint64_t>(timing_units),
        "Smacker synchronized sample count overflows");
    if (expected_numerator % 100000U != 0U ||
        stream.audio_sample_frames != expected_numerator / 100000U) {
      throw ToolError(ExitCode::format,
                      "Smacker audio and video durations do not match");
    }
  }
  stream.decoded_indexed_sha256 = decoded_indexed_hash.finish_hex();
  if (stream.audio_track_count != 0U) {
    stream.decoded_pcm_sha256 = decoded_pcm_hash.finish_hex();
  }
  if (captured_audio != nullptr) {
    captured_audio->sample_rate = stream.audio_sample_rate;
    captured_audio->channels = stream.audio_channels;
    captured_audio->bits_per_sample = stream.audio_bits_per_sample;
    captured_audio->pcm_sha256 = stream.decoded_pcm_sha256;
    if (captured_audio->pcm.size() != stream.audio_uncompressed_bytes) {
      throw ToolError(ExitCode::format,
                      "captured Smacker audio size is inconsistent");
    }
  }
  mh::common::Sha256 final_indexed_hash;
  final_indexed_hash.update(indexed_frame.data(), indexed_frame.size());
  stream.final_indexed_sha256 = final_indexed_hash.finish_hex();
  mh::common::Sha256 palette_hash;
  palette_hash.update(palette.data(), palette.size());
  stream.final_palette_6bit_sha256 = palette_hash.finish_hex();
  if (stream_completed != nullptr) {
    *stream_completed = true;
  }
  return stream;
}

} // namespace

DiviArchive parse_divi(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < divi_header_bytes ||
      !std::equal(bytes.begin(), bytes.begin() + 4U, "DIVI")) {
    throw ToolError(ExitCode::format,
                    "DIVI input does not have the DIVI signature");
  }
  DiviArchive archive;
  archive.entry_count = le32(bytes, 4U);
  archive.payload_offset = le32(bytes, 8U);
  archive.file_bytes = bytes.size();
  if (archive.payload_offset < divi_header_bytes ||
      archive.payload_offset > bytes.size() ||
      (archive.payload_offset - divi_header_bytes) % divi_record_bytes != 0U) {
    throw ToolError(ExitCode::format,
                    "DIVI directory does not end at its payload offset");
  }
  archive.capacity = static_cast<std::uint32_t>(
      (archive.payload_offset - divi_header_bytes) / divi_record_bytes);
  if (archive.capacity == 0U || archive.capacity > 65536U ||
      archive.entry_count == 0U || archive.entry_count > archive.capacity) {
    throw ToolError(ExitCode::format,
                    "DIVI entry count or capacity is outside bounds");
  }
  archive.entries.reserve(archive.entry_count);
  for (std::uint32_t index = 0U; index < archive.capacity; ++index) {
    const auto record_offset = static_cast<std::size_t>(
        divi_header_bytes +
        static_cast<std::uint64_t>(index) * divi_record_bytes);
    const auto record = bytes.subspan(
        record_offset, static_cast<std::size_t>(divi_record_bytes));
    if (index >= archive.entry_count) {
      if (!all_zero(record)) {
        throw ToolError(ExitCode::format,
                        "DIVI inactive directory record is nonzero");
      }
      continue;
    }
    DiviEntry entry;
    entry.kind = le32(record, 0U);
    entry.offset = le32(record, 4U);
    entry.runtime_bytes = le32(record, 8U);
    if (entry.kind != 1U || entry.runtime_bytes == 0U ||
        entry.offset < archive.payload_offset || entry.offset >= bytes.size()) {
      throw ToolError(
          ExitCode::format,
          "DIVI active directory record is outside the observed subset");
    }
    if (index == 0U && entry.offset != archive.payload_offset) {
      throw ToolError(ExitCode::format,
                      "DIVI first entry does not begin at the payload offset");
    }
    if (!archive.entries.empty() &&
        entry.offset <= archive.entries.back().offset) {
      throw ToolError(ExitCode::format,
                      "DIVI entry offsets are not increasing");
    }
    archive.entries.push_back(entry);
  }
  for (std::size_t index = 0U; index < archive.entries.size(); ++index) {
    const auto end =
        index + 1U < archive.entries.size()
            ? static_cast<std::uint64_t>(archive.entries[index + 1U].offset)
            : static_cast<std::uint64_t>(bytes.size());
    auto &entry = archive.entries[index];
    if (end <= entry.offset) {
      throw ToolError(ExitCode::format,
                      "DIVI entry payload is empty or reversed");
    }
    entry.byte_size = end - entry.offset;
    entry.stream = parse_smacker_envelope(
        bytes.subspan(entry.offset, static_cast<std::size_t>(entry.byte_size)));
    archive.total_frames =
        checked_add(archive.total_frames, entry.stream.frame_count,
                    "DIVI total frame count overflows");
    archive.maximum_frame_bytes =
        std::max(archive.maximum_frame_bytes, entry.stream.maximum_frame_bytes);
    for (std::size_t value = 0U; value < archive.size_flag_counts.size();
         ++value) {
      archive.size_flag_counts[value] += entry.stream.size_flag_counts[value];
    }
    for (std::size_t value = 0U; value < archive.frame_type_counts.size();
         ++value) {
      archive.frame_type_counts[value] += entry.stream.frame_type_counts[value];
    }
    archive.keyframe_count += entry.stream.keyframe_count;
    archive.palette_frame_count += entry.stream.palette_frame_count;
    archive.palette_packet_bytes += entry.stream.palette_packet_bytes;
    archive.palette_direct_colors += entry.stream.palette_direct_colors;
    archive.palette_skip_colors += entry.stream.palette_skip_colors;
    archive.palette_copy_colors += entry.stream.palette_copy_colors;
    archive.audio_frame_count += entry.stream.audio_frame_count;
    archive.audio_chunk_bytes += entry.stream.audio_chunk_bytes;
    archive.audio_uncompressed_bytes += entry.stream.audio_uncompressed_bytes;
    archive.audio_bits_consumed += entry.stream.audio_bits_consumed;
    archive.audio_trailing_bits += entry.stream.audio_trailing_bits;
    archive.audio_nonzero_trailing_bits +=
        entry.stream.audio_nonzero_trailing_bits;
    archive.video_payload_bytes += entry.stream.video_payload_bytes;
    archive.video_bits_consumed += entry.stream.video_bits_consumed;
    archive.video_trailing_bits += entry.stream.video_trailing_bits;
    archive.video_nonzero_trailing_bits +=
        entry.stream.video_nonzero_trailing_bits;
    archive.block_command_count += entry.stream.block_command_count;
    for (std::size_t kind = 0U; kind < archive.block_counts.size(); ++kind) {
      archive.block_counts[kind] += entry.stream.block_counts[kind];
    }
    archive.minimum_video_bytes =
        archive.minimum_video_bytes == 0U
            ? entry.stream.minimum_video_bytes
            : std::min(archive.minimum_video_bytes,
                       entry.stream.minimum_video_bytes);
  }
  archive.payload_bytes = bytes.size() - archive.payload_offset;
  return archive;
}

namespace {

std::vector<std::uint8_t>
read_divi_bytes(const std::filesystem::path &path,
                const std::uint64_t maximum_file_bytes) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    throw ToolError(ExitCode::input, "DIVI input is not a plain file");
  }
  const auto file_bytes = std::filesystem::file_size(path, error);
  if (error || file_bytes > maximum_file_bytes ||
      file_bytes > std::numeric_limits<std::size_t>::max()) {
    throw ToolError(ExitCode::input,
                    "DIVI input exceeds the configured size limit");
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file_bytes));
  std::ifstream input(path, std::ios::binary);
  input.read(reinterpret_cast<char *>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  if (!input || input.gcount() != static_cast<std::streamsize>(bytes.size())) {
    throw ToolError(ExitCode::input, "cannot read complete DIVI input");
  }
  return bytes;
}

std::span<const std::uint8_t>
divi_entry_payload(const std::span<const std::uint8_t> bytes,
                   const std::uint32_t entry_index) {
  if (bytes.size() < divi_header_bytes ||
      !std::equal(bytes.begin(), bytes.begin() + 4U, "DIVI")) {
    throw ToolError(ExitCode::format,
                    "DIVI input does not have the DIVI signature");
  }
  const auto entry_count = le32(bytes, 4U);
  const auto payload_offset = le32(bytes, 8U);
  if (payload_offset < divi_header_bytes || payload_offset > bytes.size() ||
      (payload_offset - divi_header_bytes) % divi_record_bytes != 0U) {
    throw ToolError(ExitCode::format,
                    "DIVI directory does not end at its payload offset");
  }
  const auto capacity = static_cast<std::uint32_t>(
      (payload_offset - divi_header_bytes) / divi_record_bytes);
  if (capacity == 0U || capacity > 65536U || entry_count == 0U ||
      entry_count > capacity) {
    throw ToolError(ExitCode::format,
                    "DIVI entry count or capacity is outside bounds");
  }
  if (entry_index >= entry_count) {
    throw ToolError(ExitCode::usage,
                    "requested DIVI entry index is out of range");
  }

  std::vector<std::uint32_t> offsets;
  offsets.reserve(entry_count);
  for (std::uint32_t index = 0U; index < capacity; ++index) {
    const auto record_offset = static_cast<std::size_t>(
        divi_header_bytes +
        static_cast<std::uint64_t>(index) * divi_record_bytes);
    const auto record = bytes.subspan(
        record_offset, static_cast<std::size_t>(divi_record_bytes));
    if (index >= entry_count) {
      if (!all_zero(record)) {
        throw ToolError(ExitCode::format,
                        "DIVI inactive directory record is nonzero");
      }
      continue;
    }
    const auto kind = le32(record, 0U);
    const auto offset = le32(record, 4U);
    const auto runtime_bytes = le32(record, 8U);
    if (kind != 1U || runtime_bytes == 0U || offset < payload_offset ||
        offset >= bytes.size()) {
      throw ToolError(
          ExitCode::format,
          "DIVI active directory record is outside the observed subset");
    }
    if ((index == 0U && offset != payload_offset) ||
        (!offsets.empty() && offset <= offsets.back())) {
      throw ToolError(ExitCode::format,
                      "DIVI entry offsets are not an increasing partition");
    }
    offsets.push_back(offset);
  }
  const auto begin = static_cast<std::size_t>(offsets[entry_index]);
  const auto end = entry_index + 1U < offsets.size()
                       ? static_cast<std::size_t>(offsets[entry_index + 1U])
                       : bytes.size();
  if (end <= begin) {
    throw ToolError(ExitCode::format,
                    "DIVI entry payload is empty or reversed");
  }
  return bytes.subspan(begin, end - begin);
}

void write_smacker_bytes_guarded(const std::filesystem::path &output,
                                 const std::span<const std::uint8_t> bytes,
                                 const bool overwrite) {
  if (output.empty()) {
    throw ToolError(ExitCode::usage, "Smacker output path is empty");
  }
  const auto destination = std::filesystem::absolute(output).lexically_normal();
  const auto parent = destination.parent_path();
  std::error_code error;
  auto current = parent.root_path();
  for (const auto &component : parent.relative_path()) {
    current /= component;
    auto status = std::filesystem::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      if (!std::filesystem::create_directory(current, error) || error) {
        throw ToolError(ExitCode::input,
                        "cannot create Smacker output directory");
      }
      status = std::filesystem::symlink_status(current, error);
    }
    if (error || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_directory(status)) {
      throw ToolError(ExitCode::input,
                      "Smacker output parent is not a plain directory");
    }
  }
  error.clear();
  const auto status = std::filesystem::symlink_status(destination, error);
  const bool exists = !error && std::filesystem::exists(status);
  if (exists && (std::filesystem::is_symlink(status) ||
                 !std::filesystem::is_regular_file(status))) {
    throw ToolError(ExitCode::input, "Smacker output is not a plain file");
  }
  if (exists && !overwrite) {
    throw ToolError(ExitCode::input, "Smacker output exists (use --overwrite)");
  }
  auto temporary = destination;
  temporary += ".mhtool-part";
  auto backup = destination;
  backup += ".mhtool-backup";
  if (std::filesystem::exists(temporary) ||
      (exists && std::filesystem::exists(backup))) {
    throw ToolError(ExitCode::input,
                    "Smacker temporary or backup output exists");
  }
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
      std::filesystem::remove(temporary, error);
      throw ToolError(ExitCode::input, "cannot write Smacker output");
    }
  }
  bool backed_up = false;
  if (exists) {
    std::filesystem::rename(destination, backup, error);
    if (error) {
      std::filesystem::remove(temporary, error);
      throw ToolError(ExitCode::input, "cannot preserve Smacker output");
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
    throw ToolError(ExitCode::input, "cannot finalize Smacker output");
  }
  if (backed_up) {
    std::filesystem::remove(backup, error);
    if (error) {
      throw ToolError(ExitCode::input, "cannot remove Smacker output backup");
    }
  }
}

} // namespace

DiviArchive read_divi(const std::filesystem::path &path,
                      const std::uint64_t maximum_file_bytes) {
  const auto bytes = read_divi_bytes(path, maximum_file_bytes);
  return parse_divi(bytes);
}

SmackerDecodedFrame decode_divi_frame(const std::span<const std::uint8_t> bytes,
                                      const std::uint32_t entry_index,
                                      const std::uint32_t frame_index) {
  const auto archive = parse_divi(bytes);
  if (entry_index >= archive.entries.size()) {
    throw ToolError(ExitCode::usage,
                    "requested DIVI entry index is out of range");
  }
  SmackerDecodedFrame frame;
  frame.entry_index = entry_index;
  const auto &entry = archive.entries[entry_index];
  static_cast<void>(parse_smacker_envelope(
      bytes.subspan(entry.offset, static_cast<std::size_t>(entry.byte_size)),
      frame_index, &frame));
  return frame;
}

SmackerDecodedFrame read_divi_frame(const std::filesystem::path &path,
                                    const std::uint32_t entry_index,
                                    const std::uint32_t frame_index,
                                    const std::uint64_t maximum_file_bytes) {
  const auto bytes = read_divi_bytes(path, maximum_file_bytes);
  return decode_divi_frame(bytes, entry_index, frame_index);
}

SmackerDecodedAudio decode_divi_audio(const std::span<const std::uint8_t> bytes,
                                      const std::uint32_t entry_index) {
  const auto archive = parse_divi(bytes);
  if (entry_index >= archive.entries.size()) {
    throw ToolError(ExitCode::usage,
                    "requested DIVI entry index is out of range");
  }
  SmackerDecodedAudio audio;
  audio.entry_index = entry_index;
  const auto &entry = archive.entries[entry_index];
  audio.pcm.reserve(
      static_cast<std::size_t>(entry.stream.audio_uncompressed_bytes));
  static_cast<void>(parse_smacker_envelope(
      bytes.subspan(entry.offset, static_cast<std::size_t>(entry.byte_size)),
      std::numeric_limits<std::uint32_t>::max(), nullptr, &audio));
  return audio;
}

SmackerDecodedAudio read_divi_audio(const std::filesystem::path &path,
                                    const std::uint32_t entry_index,
                                    const std::uint64_t maximum_file_bytes) {
  const auto bytes = read_divi_bytes(path, maximum_file_bytes);
  return decode_divi_audio(bytes, entry_index);
}

bool decode_divi_stream(const std::span<const std::uint8_t> bytes,
                        const std::uint32_t entry_index,
                        const SmackerStreamVisitors &visitors) {
  if (!visitors.video && !visitors.audio) {
    throw ToolError(ExitCode::usage,
                    "DIVI stream decoder requires a video or audio visitor");
  }
  const auto payload = divi_entry_payload(bytes, entry_index);
  bool completed = false;
  static_cast<void>(parse_smacker_envelope(
      payload, std::numeric_limits<std::uint32_t>::max(), nullptr, nullptr,
      &visitors, &completed, entry_index));
  return completed;
}

bool read_divi_stream(const std::filesystem::path &path,
                      const std::uint32_t entry_index,
                      const SmackerStreamVisitors &visitors,
                      const std::uint64_t maximum_file_bytes) {
  const auto bytes = read_divi_bytes(path, maximum_file_bytes);
  return decode_divi_stream(bytes, entry_index, visitors);
}

void write_smacker_frame_pam(const SmackerDecodedFrame &frame,
                             const std::filesystem::path &output,
                             const bool overwrite) {
  const auto expected =
      static_cast<std::uint64_t>(frame.width) * frame.height * 4U;
  if (frame.width == 0U || frame.height == 0U ||
      expected != frame.rgba.size()) {
    throw ToolError(ExitCode::format,
                    "Smacker frame has inconsistent decoded pixels");
  }
  std::ostringstream header;
  header << "P7\nWIDTH " << frame.width << "\nHEIGHT " << frame.height
         << "\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n";
  const auto prefix = header.str();
  std::vector<std::uint8_t> pam(prefix.begin(), prefix.end());
  pam.insert(pam.end(), frame.rgba.begin(), frame.rgba.end());
  write_smacker_bytes_guarded(output, pam, overwrite);
}

void write_smacker_frame_bmp(const SmackerDecodedFrame &frame,
                             const std::filesystem::path &output,
                             const bool overwrite) {
  const auto expected =
      static_cast<std::uint64_t>(frame.width) * frame.height * 4U;
  const auto row_bytes =
      (static_cast<std::uint64_t>(frame.width) * 3U + 3U) & ~std::uint64_t{3U};
  const auto pixel_bytes = row_bytes * frame.height;
  const auto file_bytes = 54U + pixel_bytes;
  if (frame.width == 0U || frame.height == 0U ||
      expected != frame.rgba.size() ||
      file_bytes > std::numeric_limits<std::uint32_t>::max()) {
    throw ToolError(ExitCode::format,
                    "Smacker frame cannot form a bounded BMP image");
  }
  std::vector<std::uint8_t> bmp(static_cast<std::size_t>(file_bytes), 0U);
  const auto put16 = [&bmp](const std::size_t offset,
                            const std::uint16_t value) {
    bmp[offset] = static_cast<std::uint8_t>(value);
    bmp[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
  };
  const auto put32 = [&bmp](const std::size_t offset,
                            const std::uint32_t value) {
    bmp[offset] = static_cast<std::uint8_t>(value);
    bmp[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    bmp[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    bmp[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
  };
  bmp[0] = 'B';
  bmp[1] = 'M';
  put32(2U, static_cast<std::uint32_t>(file_bytes));
  put32(10U, 54U);
  put32(14U, 40U);
  put32(18U, frame.width);
  put32(22U, frame.height);
  put16(26U, 1U);
  put16(28U, 24U);
  put32(34U, static_cast<std::uint32_t>(pixel_bytes));
  for (std::uint32_t output_y = 0U; output_y < frame.height; ++output_y) {
    const auto source_y = frame.height - output_y - 1U;
    const auto destination_row =
        54U + static_cast<std::uint64_t>(output_y) * row_bytes;
    for (std::uint32_t x = 0U; x < frame.width; ++x) {
      const auto source =
          (static_cast<std::size_t>(source_y) * frame.width + x) * 4U;
      const auto destination = static_cast<std::size_t>(
          destination_row + static_cast<std::uint64_t>(x) * 3U);
      bmp[destination] = frame.rgba[source + 2U];
      bmp[destination + 1U] = frame.rgba[source + 1U];
      bmp[destination + 2U] = frame.rgba[source];
    }
  }
  write_smacker_bytes_guarded(output, bmp, overwrite);
}

void write_smacker_frame_png(const SmackerDecodedFrame &frame,
                             const std::filesystem::path &output,
                             const bool overwrite) {
  write_rgba_png(frame.width, frame.height, frame.rgba, output, overwrite);
}

void write_smacker_audio_wav(const SmackerDecodedAudio &audio,
                             const std::filesystem::path &output,
                             const bool overwrite) {
  const auto block_align =
      static_cast<std::uint32_t>(audio.channels) * audio.bits_per_sample / 8U;
  const auto byte_rate =
      static_cast<std::uint64_t>(audio.sample_rate) * block_align;
  if (audio.sample_rate == 0U || audio.channels != 2U ||
      audio.bits_per_sample != 16U || block_align != 4U || audio.pcm.empty() ||
      audio.pcm.size() % block_align != 0U ||
      audio.pcm.size() > std::numeric_limits<std::uint32_t>::max() - 36ULL ||
      byte_rate > std::numeric_limits<std::uint32_t>::max()) {
    throw ToolError(ExitCode::format,
                    "Smacker audio cannot form a bounded PCM WAV");
  }
  std::vector<std::uint8_t> wav(44U + audio.pcm.size(), 0U);
  const auto put16 = [&wav](const std::size_t offset,
                            const std::uint16_t value) {
    wav[offset] = static_cast<std::uint8_t>(value);
    wav[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
  };
  const auto put32 = [&wav](const std::size_t offset,
                            const std::uint32_t value) {
    wav[offset] = static_cast<std::uint8_t>(value);
    wav[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
    wav[offset + 2U] = static_cast<std::uint8_t>(value >> 16U);
    wav[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
  };
  std::copy_n("RIFF", 4U, wav.begin());
  put32(4U, static_cast<std::uint32_t>(36U + audio.pcm.size()));
  std::copy_n("WAVEfmt ", 8U, wav.begin() + 8U);
  put32(16U, 16U);
  put16(20U, 1U);
  put16(22U, audio.channels);
  put32(24U, audio.sample_rate);
  put32(28U, static_cast<std::uint32_t>(byte_rate));
  put16(32U, static_cast<std::uint16_t>(block_align));
  put16(34U, audio.bits_per_sample);
  std::copy_n("data", 4U, wav.begin() + 36U);
  put32(40U, static_cast<std::uint32_t>(audio.pcm.size()));
  std::copy(audio.pcm.begin(), audio.pcm.end(), wav.begin() + 44U);
  write_smacker_bytes_guarded(output, wav, overwrite);
}

void extract_divi_smk(const std::filesystem::path &source_path,
                      const DiviArchive &archive,
                      const std::filesystem::path &output_directory,
                      const bool overwrite) {
  std::error_code error;
  auto current = output_directory.root_path();
  for (const auto &component : output_directory.relative_path()) {
    current /= component;
    auto status = std::filesystem::symlink_status(current, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
      if (!std::filesystem::create_directory(current, error) || error) {
        throw ToolError(ExitCode::input,
                        "cannot create DIVI extraction directory");
      }
      status = std::filesystem::symlink_status(current, error);
    }
    if (error || std::filesystem::is_symlink(status) ||
        !std::filesystem::is_directory(status)) {
      throw ToolError(ExitCode::input,
                      "DIVI output parent is not a plain directory");
    }
  }
  std::vector<std::filesystem::path> destinations;
  destinations.reserve(archive.entries.size());
  for (std::size_t index = 0U; index < archive.entries.size(); ++index) {
    auto name = std::string("movie");
    if (index < 10U) {
      name += '0';
    }
    name += std::to_string(index) + ".smk";
    const auto destination = output_directory / name;
    const auto status = std::filesystem::symlink_status(destination, error);
    const auto exists = !error && std::filesystem::exists(status);
    error.clear();
    if (exists && (std::filesystem::is_symlink(status) ||
                   !std::filesystem::is_regular_file(status))) {
      throw ToolError(ExitCode::input,
                      "DIVI extraction target is not a plain file");
    }
    if (exists && !overwrite) {
      throw ToolError(ExitCode::input,
                      "DIVI extraction target exists (use --overwrite)");
    }
    auto temporary = destination;
    temporary += ".mhtool-part";
    auto backup = destination;
    backup += ".mhtool-backup";
    if (std::filesystem::exists(temporary) ||
        (exists && std::filesystem::exists(backup))) {
      throw ToolError(ExitCode::input,
                      "DIVI temporary or backup output exists");
    }
    destinations.push_back(destination);
  }

  std::ifstream input(source_path, std::ios::binary | std::ios::ate);
  if (!input || input.tellg() < 0 ||
      static_cast<std::uint64_t>(input.tellg()) != archive.file_bytes) {
    throw ToolError(ExitCode::input, "DIVI source changed before extraction");
  }
  constexpr std::size_t copy_buffer_bytes = 1024U * 1024U;
  std::vector<char> buffer(copy_buffer_bytes);
  for (std::size_t index = 0U; index < archive.entries.size(); ++index) {
    const auto &entry = archive.entries[index];
    const auto &destination = destinations[index];
    auto temporary = destination;
    temporary += ".mhtool-part";
    auto backup = destination;
    backup += ".mhtool-backup";
    const auto existed = std::filesystem::exists(destination);
    input.clear();
    input.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    auto remaining = entry.byte_size;
    while (remaining != 0U) {
      const auto count = static_cast<std::size_t>(
          std::min<std::uint64_t>(remaining, buffer.size()));
      input.read(buffer.data(), static_cast<std::streamsize>(count));
      output.write(buffer.data(), static_cast<std::streamsize>(count));
      if (!input || !output) {
        output.close();
        std::filesystem::remove(temporary, error);
        throw ToolError(ExitCode::input, "cannot copy complete DIVI entry");
      }
      remaining -= count;
    }
    output.close();
    bool backed_up = false;
    if (existed) {
      std::filesystem::rename(destination, backup, error);
      if (error) {
        std::filesystem::remove(temporary, error);
        throw ToolError(ExitCode::input,
                        "cannot preserve DIVI extraction target");
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
      throw ToolError(ExitCode::input,
                      "cannot finalize DIVI extraction output");
    }
    if (backed_up) {
      std::filesystem::remove(backup, error);
      if (error) {
        throw ToolError(ExitCode::input,
                        "cannot remove DIVI extraction backup");
      }
    }
  }
}

} // namespace mh::content
