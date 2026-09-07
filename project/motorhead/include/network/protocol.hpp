#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mh::network {

// DINM values recovered from the p3.1 client and dedicated-server dispatch
// tables. Values 1..21 are deliberately kept stable even where a message is
// directional or unused in a particular phase.
enum class MessageType : std::uint16_t {
  join_request = 1,
  join_accept = 2,
  join_denied = 3,
  join_player = 4,
  player_data = 5,
  player_list = 6,
  lobby_message_request = 7,
  lobby_message = 8,
  ready_request = 9,
  ping_request = 10,
  ping_response_lobby = 11,
  ping_response_game = 12,
  car_data = 13,
  car_data2 = 14,
  players_status = 15,
  start_request = 16,
  disconnect = 17,
  leave_game = 18,
  horn = 19,
  kick = horn,
  reserved20 = 20,
  race_data = 21,
};

inline constexpr std::size_t packet_prefix_size = 4U;
inline constexpr std::size_t packet_peer_prefix_size = 5U;
inline constexpr std::size_t maximum_packet_size = 1103U;
inline constexpr float protocol_version = 3.1F;
inline constexpr std::uint8_t maximum_players = 8U;

struct Packet {
  MessageType type = MessageType::disconnect;
  std::uint8_t peer = 0U;
  std::vector<std::uint8_t> bytes;
};

enum class PacketError : std::uint8_t {
  none,
  incomplete,
  invalid_type,
  invalid_size,
  oversized,
};

struct PacketParseResult {
  PacketError error = PacketError::none;
  std::optional<Packet> packet;
  std::size_t consumed = 0U;
};

[[nodiscard]] std::optional<std::size_t>
fixed_packet_size(MessageType type) noexcept;
[[nodiscard]] PacketParseResult
parse_packet(std::span<const std::uint8_t> bytes);
[[nodiscard]] Packet make_packet(MessageType type, std::uint8_t peer = 0U);
[[nodiscard]] Packet make_join_request(std::uint8_t provider,
                                       std::string_view password);
[[nodiscard]] float join_request_version(const Packet &packet) noexcept;
[[nodiscard]] std::string join_request_password(const Packet &packet);

// The 88-byte PLAYERDATA/JOINPLAYER record is retained byte-for-byte. The
// reconstruction only names fields whose source accesses are locked: flags at
// +5, selection at +6, name at +7, team at +23, gear at +28, car at +29,
// horn at +45, colours at +61..+76, races at +77, score at +81, and the
// trailing grid/lap/state bytes at +85..+87. The lobby renderer orders the
// field by +85 and prints +86 in its Lap column.
struct PlayerDataFields {
  std::uint8_t flags = 0U;
  std::uint8_t selection = 0U;
  std::string name;
  std::string team;
  bool automatic_gear = true;
  std::string car_name;
  std::string horn_name;
  std::uint32_t colour = 0x00e06820U;
  std::array<std::uint32_t, 3U> car_colours{};
  std::uint32_t races = 0U;
  std::uint32_t score = 0U;
  std::uint8_t grid_position = 0U;
  std::uint8_t lap = 0U;
  std::uint8_t state = 0U;
};

[[nodiscard]] Packet make_player_data(MessageType type, std::uint8_t peer,
                                      const PlayerDataFields &fields);
[[nodiscard]] PlayerDataFields player_data_fields(const Packet &packet);
[[nodiscard]] Packet make_player_data(MessageType type, std::uint8_t peer,
                                      std::string_view player_name,
                                      std::uint8_t selection,
                                      std::uint8_t flags = 0U);
[[nodiscard]] std::string player_name(const Packet &packet);
[[nodiscard]] std::uint8_t player_selection(const Packet &packet) noexcept;
[[nodiscard]] std::uint8_t player_flags(const Packet &packet) noexcept;

[[nodiscard]] Packet make_ready_request(std::uint8_t peer, bool ready);
[[nodiscard]] bool ready_request_value(const Packet &packet) noexcept;
[[nodiscard]] Packet
make_players_status(std::uint8_t sender,
                    const std::array<std::uint8_t, maximum_players> &status);
[[nodiscard]] std::array<std::uint8_t, maximum_players>
players_status_values(const Packet &packet) noexcept;

struct RaceResultData {
  std::uint8_t peer = 0U;
  std::uint8_t position = 0U;
  std::uint32_t best_lap_ms = 0U;
  std::uint32_t race_time_ms = 0U;
  bool finished = false;
};

[[nodiscard]] Packet make_race_results(std::uint8_t sender,
                                       std::span<const RaceResultData> results);
[[nodiscard]] bool is_race_results(const Packet &packet) noexcept;
[[nodiscard]] std::vector<RaceResultData> race_results(const Packet &packet);

[[nodiscard]] Packet make_lobby_message(MessageType type, std::uint8_t peer,
                                        std::string_view text);
[[nodiscard]] std::string lobby_message_text(const Packet &packet);

[[nodiscard]] Packet make_ping(MessageType type, std::uint8_t peer,
                               std::uint32_t timestamp);
[[nodiscard]] std::uint32_t ping_timestamp(const Packet &packet) noexcept;

struct RaceConfiguration {
  std::uint32_t track_index = 0U;
  std::uint32_t lap_count = 3U;
  std::uint32_t mode = 0U;
  std::uint32_t seed = 0U;
};

[[nodiscard]] Packet make_race_configuration(std::uint8_t peer,
                                             const RaceConfiguration &value);
[[nodiscard]] RaceConfiguration
race_configuration(const Packet &packet) noexcept;

struct RaceInputFrame {
  std::uint32_t tick = 0U;
  float steering = 0.0F;
  float throttle = 0.0F;
  float brake = 0.0F;
  bool handbrake = false;
  bool horn = false;
  bool restart_requested = false;
  bool pause_requested = false;
  bool pause_state = false;
  bool shift_up = false;
  bool shift_down = false;
};

[[nodiscard]] Packet make_race_input(std::uint8_t peer,
                                     const RaceInputFrame &value);
[[nodiscard]] RaceInputFrame race_input(const Packet &packet) noexcept;

struct NetworkVehicleState {
  std::uint8_t peer = 0U;
  std::array<float, 3U> position{};
  std::array<float, 4U> rotation{};
  std::array<float, 3U> velocity{};
  float speed = 0.0F;
  std::uint32_t lap = 0U;
  std::uint32_t checkpoint = 0U;
  bool finished = false;
};

struct RaceSnapshot {
  std::uint32_t tick = 0U;
  std::uint32_t restart_generation = 0U;
  std::uint32_t pause_generation = 0U;
  bool paused = false;
  std::vector<NetworkVehicleState> vehicles;
};

[[nodiscard]] Packet make_race_snapshot(std::uint8_t peer,
                                        const RaceSnapshot &value);
[[nodiscard]] RaceSnapshot race_snapshot(const Packet &packet);

} // namespace mh::network
