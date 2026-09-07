#include <network/protocol.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace mh::network {
namespace {

std::uint16_t read_u16(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(bytes[offset + 1U] << 8U);
}

std::uint32_t read_u32(const std::span<const std::uint8_t> bytes,
                       const std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

void write_u16(std::vector<std::uint8_t> &bytes, const std::size_t offset,
               const std::uint16_t value) noexcept {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
  bytes[offset + 1U] = static_cast<std::uint8_t>(value >> 8U);
}

void write_u32(std::vector<std::uint8_t> &bytes, const std::size_t offset,
               const std::uint32_t value) noexcept {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
  bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
  bytes[offset + 2U] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
  bytes[offset + 3U] = static_cast<std::uint8_t>(value >> 24U);
}

float read_float(const std::span<const std::uint8_t> bytes,
                 const std::size_t offset) noexcept {
  return std::bit_cast<float>(read_u32(bytes, offset));
}

void write_float(std::vector<std::uint8_t> &bytes, const std::size_t offset,
                 const float value) noexcept {
  write_u32(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

void write_fixed_string(std::vector<std::uint8_t> &bytes,
                        const std::size_t offset, const std::size_t capacity,
                        const std::string_view value) {
  const auto count =
      std::min(capacity == 0U ? 0U : capacity - 1U, value.size());
  std::copy_n(value.begin(), static_cast<std::ptrdiff_t>(count),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::string read_fixed_string(const std::span<const std::uint8_t> bytes,
                              const std::size_t offset,
                              const std::size_t capacity) {
  if (offset >= bytes.size()) {
    return {};
  }
  const auto available = std::min(capacity, bytes.size() - offset);
  const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
  const auto end = std::find(
      begin, begin + static_cast<std::ptrdiff_t>(available), std::uint8_t{0U});
  return {begin, end};
}

bool valid_type(const std::uint16_t value) noexcept {
  return value >= static_cast<std::uint16_t>(MessageType::join_request) &&
         value <= static_cast<std::uint16_t>(MessageType::race_data);
}

} // namespace

std::optional<std::size_t> fixed_packet_size(const MessageType type) noexcept {
  switch (type) {
  case MessageType::join_request:
    return 25U;
  case MessageType::join_accept:
  case MessageType::join_denied:
  case MessageType::disconnect:
  case MessageType::leave_game:
    return 5U;
  case MessageType::join_player:
  case MessageType::player_data:
    return 88U;
  case MessageType::player_list:
  case MessageType::ready_request:
    return 1103U;
  case MessageType::ping_request:
    return 8U;
  case MessageType::ping_response_lobby:
  case MessageType::ping_response_game:
    return 9U;
  case MessageType::car_data:
    return 40U;
  case MessageType::car_data2:
    return 16U;
  case MessageType::players_status:
    return 95U;
  case MessageType::start_request:
    return 4U;
  case MessageType::race_data:
    return 376U;
  case MessageType::lobby_message_request:
  case MessageType::lobby_message:
  case MessageType::horn:
  case MessageType::reserved20:
    return std::nullopt;
  }
  return std::nullopt;
}

PacketParseResult parse_packet(const std::span<const std::uint8_t> bytes) {
  if (bytes.size() < packet_prefix_size) {
    return {PacketError::incomplete, std::nullopt, 0U};
  }
  const auto raw_type = read_u16(bytes, 0U);
  if (!valid_type(raw_type)) {
    return {PacketError::invalid_type, std::nullopt, 0U};
  }
  const auto size = static_cast<std::size_t>(read_u16(bytes, 2U));
  if (size < packet_prefix_size) {
    return {PacketError::invalid_size, std::nullopt, 0U};
  }
  if (size > maximum_packet_size) {
    return {PacketError::oversized, std::nullopt, 0U};
  }
  if (bytes.size() < size) {
    return {PacketError::incomplete, std::nullopt, 0U};
  }
  const auto type = static_cast<MessageType>(raw_type);
  if (const auto fixed = fixed_packet_size(type);
      fixed.has_value() && *fixed != size) {
    return {PacketError::invalid_size, std::nullopt, size};
  }
  if ((type == MessageType::lobby_message_request ||
       type == MessageType::lobby_message) &&
      (size < 9U || read_u32(bytes, 5U) != size - 9U)) {
    return {PacketError::invalid_size, std::nullopt, size};
  }
  if (type == MessageType::horn && size != 4U && size != 5U) {
    return {PacketError::invalid_size, std::nullopt, size};
  }
  Packet packet;
  packet.type = type;
  packet.peer = size >= packet_peer_prefix_size ? bytes[4U] : 0U;
  packet.bytes.assign(bytes.begin(),
                      bytes.begin() + static_cast<std::ptrdiff_t>(size));
  return {PacketError::none, std::move(packet), size};
}

Packet make_packet(const MessageType type, const std::uint8_t peer) {
  const auto size = fixed_packet_size(type).value_or(packet_peer_prefix_size);
  Packet result{type, peer, std::vector<std::uint8_t>(size, 0U)};
  write_u16(result.bytes, 0U, static_cast<std::uint16_t>(type));
  write_u16(result.bytes, 2U, static_cast<std::uint16_t>(size));
  if (size >= packet_peer_prefix_size) {
    result.bytes[4U] = peer;
  }
  return result;
}

Packet make_join_request(const std::uint8_t provider,
                         const std::string_view password) {
  auto result = make_packet(MessageType::join_request, provider);
  write_float(result.bytes, 5U, protocol_version);
  write_fixed_string(result.bytes, 9U, 16U, password);
  return result;
}

float join_request_version(const Packet &packet) noexcept {
  return packet.bytes.size() == 25U ? read_float(packet.bytes, 5U) : 0.0F;
}

std::string join_request_password(const Packet &packet) {
  return packet.bytes.size() == 25U ? read_fixed_string(packet.bytes, 9U, 16U)
                                    : std::string{};
}

Packet make_player_data(const MessageType type, const std::uint8_t peer,
                        const PlayerDataFields &fields) {
  auto result = make_packet(type, peer);
  if (result.bytes.size() != 88U) {
    return result;
  }
  result.bytes[5U] = fields.flags;
  result.bytes[6U] = fields.selection;
  write_fixed_string(result.bytes, 7U, 16U, fields.name);
  write_fixed_string(result.bytes, 23U, 5U, fields.team);
  result.bytes[28U] = fields.automatic_gear ? 1U : 0U;
  write_fixed_string(result.bytes, 29U, 16U, fields.car_name);
  write_fixed_string(result.bytes, 45U, 16U, fields.horn_name);
  write_u32(result.bytes, 61U, fields.colour);
  for (std::size_t index = 0U; index < fields.car_colours.size(); ++index) {
    write_u32(result.bytes, 65U + index * 4U, fields.car_colours[index]);
  }
  write_u32(result.bytes, 77U, fields.races);
  write_u32(result.bytes, 81U, fields.score);
  result.bytes[85U] = fields.grid_position;
  result.bytes[86U] = fields.lap;
  result.bytes[87U] = fields.state;
  return result;
}

PlayerDataFields player_data_fields(const Packet &packet) {
  if (packet.bytes.size() != 88U) {
    return {};
  }
  PlayerDataFields result;
  result.flags = packet.bytes[5U];
  result.selection = packet.bytes[6U];
  result.name = read_fixed_string(packet.bytes, 7U, 16U);
  result.team = read_fixed_string(packet.bytes, 23U, 5U);
  result.automatic_gear = packet.bytes[28U] != 0U;
  result.car_name = read_fixed_string(packet.bytes, 29U, 16U);
  result.horn_name = read_fixed_string(packet.bytes, 45U, 16U);
  result.colour = read_u32(packet.bytes, 61U);
  for (std::size_t index = 0U; index < result.car_colours.size(); ++index) {
    result.car_colours[index] = read_u32(packet.bytes, 65U + index * 4U);
  }
  result.races = read_u32(packet.bytes, 77U);
  result.score = read_u32(packet.bytes, 81U);
  result.grid_position = packet.bytes[85U];
  result.lap = packet.bytes[86U];
  result.state = packet.bytes[87U];
  return result;
}

Packet make_player_data(const MessageType type, const std::uint8_t peer,
                        const std::string_view name,
                        const std::uint8_t selection,
                        const std::uint8_t flags) {
  PlayerDataFields fields;
  fields.flags = flags;
  fields.selection = selection;
  fields.name = std::string(name);
  return make_player_data(type, peer, fields);
}

std::string player_name(const Packet &packet) {
  return packet.bytes.size() == 88U ? read_fixed_string(packet.bytes, 7U, 16U)
                                    : std::string{};
}

std::uint8_t player_selection(const Packet &packet) noexcept {
  return packet.bytes.size() == 88U ? packet.bytes[6U] : 0U;
}

std::uint8_t player_flags(const Packet &packet) noexcept {
  return packet.bytes.size() == 88U ? packet.bytes[5U] : 0U;
}

Packet make_ready_request(const std::uint8_t peer, const bool ready) {
  auto result = make_packet(MessageType::ready_request, peer);
  result.bytes[5U] = ready ? 1U : 0U;
  return result;
}

bool ready_request_value(const Packet &packet) noexcept {
  return packet.bytes.size() == 1103U && packet.bytes[5U] != 0U;
}

Packet
make_players_status(const std::uint8_t sender,
                    const std::array<std::uint8_t, maximum_players> &status) {
  auto result = make_packet(MessageType::players_status, sender);
  std::copy(status.begin(), status.end(), result.bytes.begin() + 5);
  return result;
}

std::array<std::uint8_t, maximum_players>
players_status_values(const Packet &packet) noexcept {
  std::array<std::uint8_t, maximum_players> result{};
  if (packet.bytes.size() == 95U) {
    std::copy_n(packet.bytes.begin() + 5,
                static_cast<std::ptrdiff_t>(result.size()), result.begin());
  }
  return result;
}

Packet make_race_results(const std::uint8_t sender,
                         const std::span<const RaceResultData> results) {
  auto packet = make_packet(MessageType::players_status, sender);
  const auto count = std::min<std::size_t>(results.size(), maximum_players);
  packet.bytes[5U] = static_cast<std::uint8_t>(count);
  for (std::size_t index = 0U; index < count; ++index) {
    const auto base = 6U + index * 11U;
    packet.bytes[base] = results[index].peer;
    packet.bytes[base + 1U] = results[index].position;
    write_u32(packet.bytes, base + 2U, results[index].best_lap_ms);
    write_u32(packet.bytes, base + 6U, results[index].race_time_ms);
    packet.bytes[base + 10U] = results[index].finished ? 1U : 0U;
  }
  packet.bytes[94U] = 0x52U;
  return packet;
}

bool is_race_results(const Packet &packet) noexcept {
  return packet.type == MessageType::players_status &&
         packet.bytes.size() == 95U && packet.bytes[94U] == 0x52U;
}

std::vector<RaceResultData> race_results(const Packet &packet) {
  std::vector<RaceResultData> result;
  if (!is_race_results(packet)) {
    return result;
  }
  const auto count = std::min<std::size_t>(packet.bytes[5U], maximum_players);
  result.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    const auto base = 6U + index * 11U;
    result.push_back({packet.bytes[base], packet.bytes[base + 1U],
                      read_u32(packet.bytes, base + 2U),
                      read_u32(packet.bytes, base + 6U),
                      packet.bytes[base + 10U] != 0U});
  }
  return result;
}

Packet make_lobby_message(const MessageType type, const std::uint8_t peer,
                          const std::string_view text) {
  const auto count =
      std::min<std::size_t>(text.size(), maximum_packet_size - 9U);
  Packet result{type, peer, std::vector<std::uint8_t>(9U + count, 0U)};
  write_u16(result.bytes, 0U, static_cast<std::uint16_t>(type));
  write_u16(result.bytes, 2U, static_cast<std::uint16_t>(result.bytes.size()));
  result.bytes[4U] = peer;
  write_u32(result.bytes, 5U, static_cast<std::uint32_t>(count));
  std::copy_n(text.begin(), static_cast<std::ptrdiff_t>(count),
              result.bytes.begin() + 9);
  return result;
}

std::string lobby_message_text(const Packet &packet) {
  if (packet.bytes.size() < 9U) {
    return {};
  }
  const auto count = std::min<std::size_t>(read_u32(packet.bytes, 5U),
                                           packet.bytes.size() - 9U);
  return {packet.bytes.begin() + 9,
          packet.bytes.begin() + 9 + static_cast<std::ptrdiff_t>(count)};
}

Packet make_ping(const MessageType type, const std::uint8_t peer,
                 const std::uint32_t timestamp) {
  auto result = make_packet(type, peer);
  const auto offset = result.bytes.size() == 8U ? 4U : 5U;
  write_u32(result.bytes, offset, timestamp);
  return result;
}

std::uint32_t ping_timestamp(const Packet &packet) noexcept {
  if (packet.bytes.size() == 8U) {
    return read_u32(packet.bytes, 4U);
  }
  return packet.bytes.size() == 9U ? read_u32(packet.bytes, 5U) : 0U;
}

Packet make_race_configuration(const std::uint8_t peer,
                               const RaceConfiguration &value) {
  auto result = make_packet(MessageType::race_data, peer);
  write_u32(result.bytes, 5U, value.track_index);
  write_u32(result.bytes, 9U, value.lap_count);
  write_u32(result.bytes, 13U, value.mode);
  write_u32(result.bytes, 17U, value.seed);
  return result;
}

RaceConfiguration race_configuration(const Packet &packet) noexcept {
  if (packet.bytes.size() != 376U) {
    return {};
  }
  return {read_u32(packet.bytes, 5U), read_u32(packet.bytes, 9U),
          read_u32(packet.bytes, 13U), read_u32(packet.bytes, 17U)};
}

Packet make_race_input(const std::uint8_t peer, const RaceInputFrame &value) {
  auto result = make_packet(MessageType::car_data, peer);
  write_u32(result.bytes, 5U, value.tick);
  write_float(result.bytes, 9U, value.steering);
  write_float(result.bytes, 13U, value.throttle);
  write_float(result.bytes, 17U, value.brake);
  result.bytes[21U] = value.handbrake ? 1U : 0U;
  result.bytes[22U] = value.horn ? 1U : 0U;
  result.bytes[23U] = value.restart_requested ? 1U : 0U;
  result.bytes[24U] = value.pause_requested ? 1U : 0U;
  result.bytes[25U] = value.pause_state ? 1U : 0U;
  result.bytes[26U] = value.shift_up ? 1U : 0U;
  result.bytes[27U] = value.shift_down ? 1U : 0U;
  return result;
}

RaceInputFrame race_input(const Packet &packet) noexcept {
  if (packet.bytes.size() != 40U) {
    return {};
  }
  return {read_u32(packet.bytes, 5U),    read_float(packet.bytes, 9U),
          read_float(packet.bytes, 13U), read_float(packet.bytes, 17U),
          packet.bytes[21U] != 0U,       packet.bytes[22U] != 0U,
          packet.bytes[23U] != 0U,       packet.bytes[24U] != 0U,
          packet.bytes[25U] != 0U,       packet.bytes[26U] != 0U,
          packet.bytes[27U] != 0U};
}

Packet make_race_snapshot(const std::uint8_t peer, const RaceSnapshot &value) {
  auto result = make_packet(MessageType::race_data, peer);
  write_u32(result.bytes, 5U, value.tick);
  const auto count =
      std::min<std::size_t>(value.vehicles.size(), maximum_players);
  result.bytes[9U] = static_cast<std::uint8_t>(count);
  constexpr std::size_t stride = 40U;
  for (std::size_t index = 0U; index < count; ++index) {
    const auto base = 10U + index * stride;
    const auto &vehicle = value.vehicles[index];
    result.bytes[base] = vehicle.peer;
    std::size_t cursor = base + 1U;
    for (const auto component : vehicle.position) {
      write_float(result.bytes, cursor, component);
      cursor += 4U;
    }
    for (const auto component : vehicle.rotation) {
      const auto quantized = static_cast<std::int16_t>(
          std::lround(std::clamp(component, -1.0F, 1.0F) * 32767.0F));
      result.bytes[cursor] =
          static_cast<std::uint8_t>(static_cast<std::uint16_t>(quantized));
      result.bytes[cursor + 1U] = static_cast<std::uint8_t>(
          static_cast<std::uint16_t>(quantized) >> 8U);
      cursor += 2U;
    }
    for (const auto component : vehicle.velocity) {
      write_float(result.bytes, cursor, component);
      cursor += 4U;
    }
    write_float(result.bytes, cursor, vehicle.speed);
    cursor += 4U;
    result.bytes[cursor++] =
        static_cast<std::uint8_t>(std::min<std::uint32_t>(vehicle.lap, 255U));
    result.bytes[cursor++] = static_cast<std::uint8_t>(
        std::min<std::uint32_t>(vehicle.checkpoint, 255U));
    result.bytes[cursor] = vehicle.finished ? 1U : 0U;
  }
  // The retained RACEDATA record has 46 unused tail bytes after its eight
  // 40-byte vehicle records. Native synchronized control state lives there so
  // the original packet size and vehicle layout stay unchanged.
  write_u32(result.bytes, 330U, value.restart_generation);
  write_u32(result.bytes, 334U, value.pause_generation);
  result.bytes[338U] = value.paused ? 1U : 0U;
  return result;
}

RaceSnapshot race_snapshot(const Packet &packet) {
  RaceSnapshot result;
  if (packet.bytes.size() != 376U) {
    return result;
  }
  result.tick = read_u32(packet.bytes, 5U);
  result.restart_generation = read_u32(packet.bytes, 330U);
  result.pause_generation = read_u32(packet.bytes, 334U);
  result.paused = packet.bytes[338U] != 0U;
  const auto count = std::min<std::size_t>(packet.bytes[9U], maximum_players);
  constexpr std::size_t stride = 40U;
  result.vehicles.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    const auto base = 10U + index * stride;
    NetworkVehicleState vehicle;
    vehicle.peer = packet.bytes[base];
    std::size_t cursor = base + 1U;
    for (auto &component : vehicle.position) {
      component = read_float(packet.bytes, cursor);
      cursor += 4U;
    }
    for (auto &component : vehicle.rotation) {
      const auto bits =
          static_cast<std::uint16_t>(packet.bytes[cursor]) |
          (static_cast<std::uint16_t>(packet.bytes[cursor + 1U]) << 8U);
      component =
          static_cast<float>(static_cast<std::int16_t>(bits)) / 32767.0F;
      cursor += 2U;
    }
    for (auto &component : vehicle.velocity) {
      component = read_float(packet.bytes, cursor);
      cursor += 4U;
    }
    vehicle.speed = read_float(packet.bytes, cursor);
    cursor += 4U;
    vehicle.lap = packet.bytes[cursor++];
    vehicle.checkpoint = packet.bytes[cursor++];
    vehicle.finished = packet.bytes[cursor] != 0U;
    result.vehicles.push_back(vehicle);
  }
  return result;
}

} // namespace mh::network
