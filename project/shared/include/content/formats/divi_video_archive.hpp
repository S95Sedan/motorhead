#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace mh::content {

struct SmackerFrameInfo {
  std::uint64_t offset = 0U;
  std::uint32_t byte_size = 0U;
  std::uint8_t size_flags = 0U;
  std::uint8_t type = 0U;
  bool keyframe = false;
  std::uint64_t presentation_time_units = 0U;
  std::uint32_t palette_packet_bytes = 0U;
  std::uint32_t audio_chunk_bytes = 0U;
  std::uint32_t audio_uncompressed_bytes = 0U;
  std::uint64_t audio_start_sample_frame = 0U;
  std::uint32_t audio_sample_frames = 0U;
  std::uint64_t audio_bits_consumed = 0U;
  std::uint64_t audio_trailing_bits = 0U;
  std::uint64_t audio_nonzero_trailing_bits = 0U;
  std::uint64_t video_offset = 0U;
  std::uint32_t video_bytes = 0U;
  std::uint64_t video_bits_consumed = 0U;
  std::uint64_t video_trailing_bits = 0U;
  std::uint64_t video_nonzero_trailing_bits = 0U;
  std::uint32_t block_command_count = 0U;
  std::array<std::uint32_t, 4U> block_counts{};
};

struct SmackerHuffmanTreeInfo {
  std::uint32_t declared_bytes = 0U;
  bool present = false;
  std::uint32_t low_symbol_count = 0U;
  std::uint32_t high_symbol_count = 0U;
  std::uint32_t value_node_count = 0U;
  std::uint32_t value_leaf_count = 0U;
  std::uint32_t maximum_depth = 0U;
  std::array<std::uint32_t, 3U> escape_leaf_counts{};
  std::uint64_t bits_consumed = 0U;
};

struct SmackerStreamInfo {
  std::string signature;
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::uint32_t frame_count = 0U;
  std::int32_t frame_rate_raw = 0;
  std::uint32_t timing_scale = 100000U;
  std::uint64_t frame_duration_units = 0U;
  std::uint64_t duration_units = 0U;
  std::uint32_t flags = 0U;
  std::array<std::uint32_t, 7U> audio_sizes{};
  std::array<std::uint32_t, 7U> audio_rates{};
  std::uint32_t tree_bytes = 0U;
  std::array<SmackerHuffmanTreeInfo, 4U> huffman_trees{};
  std::uint64_t tree_bits_consumed = 0U;
  std::uint64_t tree_padding_bits = 0U;
  std::uint64_t frame_table_bytes = 0U;
  std::uint64_t frame_type_bytes = 0U;
  std::uint64_t frame_data_offset = 0U;
  std::uint64_t frame_data_bytes = 0U;
  std::uint32_t audio_track_count = 0U;
  std::uint32_t maximum_frame_bytes = 0U;
  std::array<std::uint64_t, 4U> size_flag_counts{};
  std::array<std::uint64_t, 256U> frame_type_counts{};
  std::uint64_t keyframe_count = 0U;
  std::uint64_t palette_frame_count = 0U;
  std::uint64_t palette_packet_bytes = 0U;
  std::uint64_t palette_direct_colors = 0U;
  std::uint64_t palette_skip_colors = 0U;
  std::uint64_t palette_copy_colors = 0U;
  std::uint64_t audio_frame_count = 0U;
  std::uint64_t audio_chunk_bytes = 0U;
  std::uint32_t audio_sample_rate = 0U;
  std::uint8_t audio_flags = 0U;
  std::uint8_t audio_channels = 0U;
  std::uint8_t audio_bits_per_sample = 0U;
  std::uint64_t audio_sample_frames = 0U;
  std::uint32_t first_audio_frame_index = 0xffffffffU;
  std::uint32_t last_audio_frame_index = 0xffffffffU;
  std::uint32_t maximum_audio_packet_sample_frames = 0U;
  std::uint64_t maximum_audio_delivery_lag_samples = 0U;
  std::uint64_t maximum_audio_delivery_lead_samples = 0U;
  std::uint64_t audio_uncompressed_bytes = 0U;
  std::uint64_t audio_bits_consumed = 0U;
  std::uint64_t audio_trailing_bits = 0U;
  std::uint64_t audio_nonzero_trailing_bits = 0U;
  std::string decoded_pcm_sha256;
  std::uint64_t video_payload_bytes = 0U;
  std::uint64_t video_bits_consumed = 0U;
  std::uint64_t video_trailing_bits = 0U;
  std::uint64_t video_nonzero_trailing_bits = 0U;
  std::uint64_t block_command_count = 0U;
  std::array<std::uint64_t, 4U> block_counts{};
  std::uint32_t minimum_video_bytes = 0U;
  std::string decoded_indexed_sha256;
  std::string final_indexed_sha256;
  std::string final_palette_6bit_sha256;
  std::vector<SmackerFrameInfo> frames;
};

struct DiviEntry {
  std::uint32_t kind = 0U;
  std::uint32_t offset = 0U;
  std::uint32_t runtime_bytes = 0U;
  std::uint64_t byte_size = 0U;
  SmackerStreamInfo stream;
};

struct DiviArchive {
  std::uint32_t entry_count = 0U;
  std::uint32_t capacity = 0U;
  std::uint32_t directory_offset = 12U;
  std::uint32_t payload_offset = 0U;
  std::uint64_t file_bytes = 0U;
  std::uint64_t payload_bytes = 0U;
  std::uint64_t total_frames = 0U;
  std::uint32_t maximum_frame_bytes = 0U;
  std::array<std::uint64_t, 4U> size_flag_counts{};
  std::array<std::uint64_t, 256U> frame_type_counts{};
  std::uint64_t keyframe_count = 0U;
  std::uint64_t palette_frame_count = 0U;
  std::uint64_t palette_packet_bytes = 0U;
  std::uint64_t palette_direct_colors = 0U;
  std::uint64_t palette_skip_colors = 0U;
  std::uint64_t palette_copy_colors = 0U;
  std::uint64_t audio_frame_count = 0U;
  std::uint64_t audio_chunk_bytes = 0U;
  std::uint64_t audio_uncompressed_bytes = 0U;
  std::uint64_t audio_bits_consumed = 0U;
  std::uint64_t audio_trailing_bits = 0U;
  std::uint64_t audio_nonzero_trailing_bits = 0U;
  std::uint64_t video_payload_bytes = 0U;
  std::uint64_t video_bits_consumed = 0U;
  std::uint64_t video_trailing_bits = 0U;
  std::uint64_t video_nonzero_trailing_bits = 0U;
  std::uint64_t block_command_count = 0U;
  std::array<std::uint64_t, 4U> block_counts{};
  std::uint32_t minimum_video_bytes = 0U;
  std::vector<DiviEntry> entries;
};

struct SmackerDecodedFrame {
  std::uint32_t entry_index = 0U;
  std::uint32_t frame_index = 0U;
  std::uint32_t width = 0U;
  std::uint32_t height = 0U;
  std::vector<std::uint8_t> rgba;
  std::string rgba_sha256;
};

struct SmackerDecodedAudio {
  std::uint32_t entry_index = 0U;
  std::uint32_t sample_rate = 0U;
  std::uint8_t channels = 0U;
  std::uint8_t bits_per_sample = 0U;
  std::vector<std::uint8_t> pcm;
  std::string pcm_sha256;
};

struct SmackerStreamVisitors {
  std::function<bool(SmackerDecodedFrame)> video;
  std::function<bool(std::uint32_t sample_rate, std::uint8_t channels,
                     std::uint8_t bits_per_sample,
                     std::span<const std::uint8_t> pcm)>
      audio;
};

[[nodiscard]] DiviArchive parse_divi(std::span<const std::uint8_t> bytes);
[[nodiscard]] DiviArchive
read_divi(const std::filesystem::path &path,
          std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);
[[nodiscard]] SmackerDecodedFrame
decode_divi_frame(std::span<const std::uint8_t> bytes,
                  std::uint32_t entry_index, std::uint32_t frame_index);
[[nodiscard]] SmackerDecodedFrame
read_divi_frame(const std::filesystem::path &path, std::uint32_t entry_index,
                std::uint32_t frame_index,
                std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);
[[nodiscard]] SmackerDecodedAudio
decode_divi_audio(std::span<const std::uint8_t> bytes,
                  std::uint32_t entry_index);
[[nodiscard]] SmackerDecodedAudio
read_divi_audio(const std::filesystem::path &path, std::uint32_t entry_index,
                std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);
[[nodiscard]] bool decode_divi_stream(std::span<const std::uint8_t> bytes,
                                      std::uint32_t entry_index,
                                      const SmackerStreamVisitors &visitors);
[[nodiscard]] bool
read_divi_stream(const std::filesystem::path &path, std::uint32_t entry_index,
                 const SmackerStreamVisitors &visitors,
                 std::uint64_t maximum_file_bytes = 512ULL * 1024ULL * 1024ULL);
void write_smacker_frame_pam(const SmackerDecodedFrame &frame,
                             const std::filesystem::path &output,
                             bool overwrite);
void write_smacker_frame_bmp(const SmackerDecodedFrame &frame,
                             const std::filesystem::path &output,
                             bool overwrite);
void write_smacker_frame_png(const SmackerDecodedFrame &frame,
                             const std::filesystem::path &output,
                             bool overwrite);
void write_smacker_audio_wav(const SmackerDecodedAudio &audio,
                             const std::filesystem::path &output,
                             bool overwrite);
void extract_divi_smk(const std::filesystem::path &source_path,
                      const DiviArchive &archive,
                      const std::filesystem::path &output_directory,
                      bool overwrite);

} // namespace mh::content
