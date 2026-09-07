#include <network/session.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <deque>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace mh::network {
namespace {

#if defined(_WIN32)
using Socket = SOCKET;
inline constexpr Socket invalid_socket = INVALID_SOCKET;
using SocketLength = int;
#else
using Socket = int;
inline constexpr Socket invalid_socket = -1;
using SocketLength = socklen_t;
#endif

void close_socket(Socket &socket) noexcept {
  if (socket == invalid_socket) {
    return;
  }
#if defined(_WIN32)
  closesocket(socket);
#else
  close(socket);
#endif
  socket = invalid_socket;
}

int socket_error() noexcept {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool would_block(const int error) noexcept {
#if defined(_WIN32)
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS ||
         error == WSAEALREADY || error == WSAENOTCONN;
#else
  return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS ||
         error == EALREADY || error == ENOTCONN;
#endif
}

bool set_nonblocking(const Socket socket) noexcept {
#if defined(_WIN32)
  u_long enabled = 1UL;
  return ioctlsocket(socket, FIONBIO, &enabled) == 0;
#else
  const auto flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

std::string address_text(const sockaddr_in &address) {
#if defined(_WIN32)
  const auto *text = inet_ntoa(address.sin_addr);
  return text != nullptr ? std::string(text) : std::string{};
#else
  std::array<char, INET_ADDRSTRLEN> text{};
  if (inet_ntop(AF_INET, &address.sin_addr, text.data(),
                static_cast<socklen_t>(text.size())) == nullptr) {
    return {};
  }
  return text.data();
#endif
}

sockaddr_in make_address(const std::string_view text,
                         const std::uint16_t port) {
  sockaddr_in result{};
  result.sin_family = AF_INET;
  result.sin_port = htons(port);
  if (text.empty() || text == "*") {
    result.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
#if defined(_WIN32)
    auto source = std::string(text);
    auto length = static_cast<int>(sizeof(result));
    if (WSAStringToAddressA(source.data(), AF_INET, nullptr,
                            reinterpret_cast<sockaddr *>(&result),
                            &length) != 0) {
      throw std::runtime_error("invalid IPv4 address: " + source);
    }
    result.sin_port = htons(port);
#else
    if (inet_pton(AF_INET, std::string(text).c_str(), &result.sin_addr) != 1) {
      throw std::runtime_error("invalid IPv4 address: " + std::string(text));
    }
#endif
  }
  return result;
}

std::uint16_t read_u16(const std::uint8_t *bytes) noexcept {
  return static_cast<std::uint16_t>(bytes[0U]) |
         static_cast<std::uint16_t>(bytes[1U] << 8U);
}

void append_u16(std::vector<std::uint8_t> &bytes, const std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

std::string clean_name(const std::string_view name,
                       const std::string_view fallback) {
  std::string result;
  result.reserve(std::min<std::size_t>(name.size(), 15U));
  for (const auto character : name) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte >= 32U && byte <= 126U && result.size() < 15U) {
      result.push_back(character);
    }
  }
  return result.empty() ? std::string(fallback) : result;
}

std::string clean_team(const std::string_view team) {
  auto result = clean_name(team, "");
  result.resize(std::min<std::size_t>(result.size(), 4U));
  return result;
}

} // namespace

struct LanSession::Implementation {
  struct RuntimeGuard {
    RuntimeGuard() {
#if defined(_WIN32)
      WSADATA data{};
      if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("Winsock initialization failed");
      }
#endif
    }
    ~RuntimeGuard() {
#if defined(_WIN32)
      WSACleanup();
#endif
    }
  } runtime;

  struct Connection {
    Socket socket = invalid_socket;
    std::uint8_t peer = 0U;
    bool accepted = false;
    std::vector<std::uint8_t> incoming;
    std::vector<std::uint8_t> outgoing;
    std::size_t sent = 0U;
    std::uint64_t last_receive_ms = 0U;
    bool close_after_send = false;

    Connection() = default;
    Connection(Connection &&other) noexcept
        : socket(std::exchange(other.socket, invalid_socket)), peer(other.peer),
          accepted(other.accepted), incoming(std::move(other.incoming)),
          outgoing(std::move(other.outgoing)), sent(other.sent),
          last_receive_ms(other.last_receive_ms),
          close_after_send(other.close_after_send) {}
    Connection &operator=(Connection &&other) noexcept {
      if (this != &other) {
        close_socket(socket);
        socket = std::exchange(other.socket, invalid_socket);
        peer = other.peer;
        accepted = other.accepted;
        incoming = std::move(other.incoming);
        outgoing = std::move(other.outgoing);
        sent = other.sent;
        last_receive_ms = other.last_receive_ms;
        close_after_send = other.close_after_send;
      }
      return *this;
    }
    ~Connection() { close_socket(socket); }
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;
  };

  SessionState state = SessionState::offline;
  bool host_mode = false;
  std::uint8_t local_peer = 0U;
  std::uint16_t port = lan_session_port;
  std::string session_name;
  std::string password;
  std::string local_name = "Player";
  std::uint8_t local_car = 0U;
  std::string local_team;
  bool local_automatic_gear = true;
  std::string local_car_name;
  std::uint32_t local_colour = 0x00e06820U;
  std::string error;
  Socket discovery = invalid_socket;
  Socket listener = invalid_socket;
  std::vector<Connection> connections;
  std::vector<SessionInfo> sessions;
  std::vector<LobbyPlayer> players;
  std::vector<SessionEvent> events;
  std::vector<std::pair<std::uint8_t, RaceInputFrame>> inputs;
  std::optional<RaceConfiguration> pending_race;
  std::optional<RaceSnapshot> snapshot;
  std::optional<std::vector<RaceResultData>> results;
  std::uint64_t now_ms = 0U;
  std::uint64_t last_discovery_ms = 0U;
  std::uint64_t last_ping_ms = 0U;

  ~Implementation() {
    close_socket(listener);
    close_socket(discovery);
  }

  void fail(std::string message) {
    error = std::move(message);
    events.push_back({SessionEventKind::error, 0U, error});
    state = SessionState::failed;
  }

  bool open_discovery(const bool bind_receiver) {
    close_socket(discovery);
    discovery = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (discovery == invalid_socket) {
      fail("could not create LAN discovery socket");
      return false;
    }
    int enabled = 1;
    (void)setsockopt(discovery, SOL_SOCKET, SO_BROADCAST,
                     reinterpret_cast<const char *>(&enabled), sizeof(enabled));
    (void)setsockopt(discovery, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char *>(&enabled), sizeof(enabled));
    if (!set_nonblocking(discovery)) {
      fail("could not make LAN discovery non-blocking");
      return false;
    }
    if (bind_receiver) {
      const auto address = make_address("*", lan_discovery_port);
      if (::bind(discovery, reinterpret_cast<const sockaddr *>(&address),
                 sizeof(address)) != 0) {
        fail("could not bind LAN discovery port");
        return false;
      }
    }
    return true;
  }

  void queue(Connection &connection, const Packet &packet) {
    constexpr std::size_t maximum_buffered_bytes = 64U * 1024U;
    if (connection.outgoing.size() - connection.sent + packet.bytes.size() >
        maximum_buffered_bytes) {
      close_socket(connection.socket);
      return;
    }
    connection.outgoing.insert(connection.outgoing.end(), packet.bytes.begin(),
                               packet.bytes.end());
  }

  void broadcast(const Packet &packet, const Socket except = invalid_socket) {
    for (auto &connection : connections) {
      if (connection.accepted && connection.socket != except) {
        queue(connection, packet);
      }
    }
  }

  LobbyPlayer *find_player(const std::uint8_t peer) {
    const auto found = std::find_if(
        players.begin(), players.end(),
        [peer](const LobbyPlayer &player) { return player.peer == peer; });
    return found == players.end() ? nullptr : &*found;
  }

  static PlayerDataFields player_fields(const LobbyPlayer &player,
                                        const std::uint8_t flags = 0U) {
    PlayerDataFields fields;
    fields.flags = flags;
    fields.selection = player.car_selection;
    fields.name = player.name;
    fields.team = player.team;
    fields.automatic_gear = player.automatic_gear;
    fields.car_name = player.car_name;
    fields.colour = player.colour;
    fields.races = player.races;
    fields.score = player.score;
    fields.grid_position = player.grid_position;
    fields.lap = player.lap;
    return fields;
  }

  PlayerDataFields local_fields(const std::uint8_t flags = 0U) const {
    PlayerDataFields fields;
    fields.flags = flags;
    fields.selection = local_car;
    fields.name = local_name;
    fields.team = local_team;
    fields.automatic_gear = local_automatic_gear;
    fields.car_name = local_car_name;
    fields.colour = local_colour;
    const auto player = std::find_if(players.begin(), players.end(),
                                     [this](const LobbyPlayer &candidate) {
                                       return candidate.peer == local_peer;
                                     });
    fields.grid_position = player == players.end()
                               ? static_cast<std::uint8_t>(local_peer + 1U)
                               : player->grid_position;
    return fields;
  }

  void broadcast_status() {
    std::array<std::uint8_t, maximum_players> values{};
    for (const auto &player : players) {
      if (player.peer < values.size()) {
        values[player.peer] = static_cast<std::uint8_t>(
            1U | (player.ready ? 2U : 0U) | (player.host ? 4U : 0U));
      }
    }
    broadcast(make_players_status(local_peer, values));
  }

  void upsert_player(const Packet &packet, const bool joined) {
    const auto fields = player_data_fields(packet);
    auto *player = find_player(packet.peer);
    if (player == nullptr) {
      players.push_back({packet.peer, fields.name, fields.selection, false,
                         packet.peer == 0U, fields.team, fields.automatic_gear,
                         fields.car_name, fields.colour, fields.races,
                         fields.score, fields.grid_position, fields.lap, 0U});
      events.push_back(
          {SessionEventKind::player_joined, packet.peer, players.back().name});
    } else {
      player->name = fields.name;
      player->car_selection = fields.selection;
      player->team = fields.team;
      player->automatic_gear = fields.automatic_gear;
      player->car_name = fields.car_name;
      player->colour = fields.colour;
      player->races = fields.races;
      player->score = fields.score;
      player->grid_position = fields.grid_position;
      player->lap = fields.lap;
      events.push_back({joined ? SessionEventKind::player_joined
                               : SessionEventKind::player_changed,
                        packet.peer, player->name});
    }
    std::sort(players.begin(), players.end(),
              [](const LobbyPlayer &left, const LobbyPlayer &right) {
                return left.peer < right.peer;
              });
  }

  void process_host_packet(Connection &connection, const Packet &packet) {
    if (!connection.accepted) {
      if (packet.type != MessageType::join_request) {
        close_socket(connection.socket);
        return;
      }
      std::uint8_t denial = 0U;
      if (join_request_version(packet) != protocol_version) {
        denial = 3U;
      } else if (join_request_password(packet) != password) {
        denial = 1U;
      } else if (players.size() >= maximum_players) {
        denial = 2U;
      }
      if (denial != 0U) {
        queue(connection, make_packet(MessageType::join_denied, denial));
        connection.close_after_send = true;
        return;
      }
      std::array<bool, maximum_players> occupied{};
      for (const auto &player : players) {
        if (player.peer < occupied.size()) {
          occupied[player.peer] = true;
        }
      }
      // A newly accepted client has not sent PLAYERDATA yet, so reserve slots
      // from both the roster and the live connection table. Without this,
      // simultaneous joins can be assigned the same peer id.
      for (const auto &candidate : connections) {
        if (candidate.accepted && candidate.peer < occupied.size()) {
          occupied[candidate.peer] = true;
        }
      }
      const auto slot = std::find(occupied.begin() + 1, occupied.end(), false);
      if (slot == occupied.end()) {
        queue(connection, make_packet(MessageType::join_denied, 2U));
        connection.close_after_send = true;
        return;
      }
      connection.peer =
          static_cast<std::uint8_t>(std::distance(occupied.begin(), slot));
      connection.accepted = true;
      queue(connection, make_packet(MessageType::join_accept, connection.peer));
      for (const auto &player : players) {
        queue(connection,
              make_player_data(MessageType::join_player, player.peer,
                               player_fields(player, player.ready ? 1U : 0U)));
      }
      if (pending_race.has_value()) {
        queue(connection, make_race_configuration(local_peer, *pending_race));
      }
      return;
    }

    if (packet.peer != connection.peer &&
        packet.type != MessageType::start_request) {
      return;
    }
    switch (packet.type) {
    case MessageType::player_data: {
      const auto joining = find_player(connection.peer) == nullptr;
      auto fields = player_data_fields(packet);
      fields.grid_position =
          joining ? static_cast<std::uint8_t>(connection.peer + 1U)
                  : find_player(connection.peer)->grid_position;
      const auto authoritative =
          make_player_data(MessageType::join_player, connection.peer, fields);
      upsert_player(authoritative, joining);
      // Echo the host-owned grid assignment to the sender as well as every
      // existing peer. Otherwise each client assumes that it starts first.
      broadcast(authoritative);
      broadcast_status();
      break;
    }
    case MessageType::ready_request: {
      if (auto *player = find_player(connection.peer); player != nullptr) {
        player->ready = ready_request_value(packet);
        events.push_back({SessionEventKind::ready_changed, player->peer,
                          player->ready ? "ready" : "not ready"});
        broadcast_status();
      }
      break;
    }
    case MessageType::lobby_message_request: {
      const auto message = lobby_message_text(packet);
      if (!message.empty()) {
        events.push_back(
            {SessionEventKind::lobby_message_received, packet.peer, message});
        broadcast(make_lobby_message(MessageType::lobby_message, packet.peer,
                                     message));
      }
      break;
    }
    case MessageType::ping_request:
      queue(connection, make_ping(state == SessionState::racing
                                      ? MessageType::ping_response_game
                                      : MessageType::ping_response_lobby,
                                  local_peer, ping_timestamp(packet)));
      break;
    case MessageType::ping_response_lobby:
    case MessageType::ping_response_game:
      if (auto *player = find_player(connection.peer); player != nullptr) {
        const auto sent = ping_timestamp(packet);
        player->ping_ms =
            now_ms >= sent
                ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
                      now_ms - sent, std::numeric_limits<std::uint32_t>::max()))
                : 0U;
      }
      break;
    case MessageType::car_data:
      inputs.emplace_back(packet.peer, race_input(packet));
      break;
    case MessageType::disconnect:
      broadcast(make_packet(MessageType::disconnect, connection.peer),
                connection.socket);
      remove_peer(connection.peer, SessionEventKind::player_left, "left");
      close_socket(connection.socket);
      broadcast_status();
      break;
    case MessageType::leave_game:
      if (state == SessionState::racing) {
        for (auto &player : players) {
          player.ready = false;
        }
        state = SessionState::lobby;
        pending_race.reset();
        snapshot.reset();
        results.reset();
        inputs.clear();
        broadcast(make_packet(MessageType::leave_game, connection.peer));
        broadcast_status();
        events.push_back(
            {SessionEventKind::race_returned, connection.peer, "returned"});
      }
      break;
    default:
      break;
    }
  }

  void process_client_packet(Connection &connection, const Packet &packet) {
    connection.last_receive_ms = now_ms;
    switch (packet.type) {
    case MessageType::join_accept:
      local_peer = packet.peer;
      connection.peer = 0U;
      connection.accepted = true;
      state = SessionState::lobby;
      players.clear();
      players.push_back({local_peer, local_name, local_car, false,
                         local_peer == 0U, local_team, local_automatic_gear,
                         local_car_name, local_colour, 0U, 0U,
                         static_cast<std::uint8_t>(local_peer + 1U)});
      queue(connection, make_player_data(MessageType::player_data, local_peer,
                                         local_fields()));
      events.push_back({SessionEventKind::connected, local_peer, "connected"});
      break;
    case MessageType::join_denied:
      error = packet.peer == 1U   ? "wrong password"
              : packet.peer == 2U ? "server full"
              : packet.peer == 3U ? "protocol version mismatch"
                                  : "connection denied";
      state = SessionState::failed;
      events.push_back(
          {SessionEventKind::connection_denied, packet.peer, error});
      break;
    case MessageType::join_player:
    case MessageType::player_data:
      upsert_player(packet, find_player(packet.peer) == nullptr);
      break;
    case MessageType::lobby_message:
      events.push_back({SessionEventKind::lobby_message_received, packet.peer,
                        lobby_message_text(packet)});
      break;
    case MessageType::players_status: {
      if (is_race_results(packet)) {
        results = race_results(packet);
        events.push_back({SessionEventKind::results_received, packet.peer, {}});
        break;
      }
      const auto values = players_status_values(packet);
      for (auto &player : players) {
        if (player.peer < values.size()) {
          player.ready = (values[player.peer] & 2U) != 0U;
          player.host = (values[player.peer] & 4U) != 0U;
        }
      }
      events.push_back({SessionEventKind::ready_changed, packet.peer, {}});
      break;
    }
    case MessageType::race_data:
      if (state == SessionState::racing) {
        snapshot = race_snapshot(packet);
      } else {
        pending_race = race_configuration(packet);
        events.push_back(
            {SessionEventKind::configuration_changed, packet.peer, {}});
      }
      break;
    case MessageType::start_request:
      state = SessionState::racing;
      events.push_back({SessionEventKind::race_started, 0U, {}});
      break;
    case MessageType::leave_game:
      if (state == SessionState::racing) {
        for (auto &player : players) {
          player.ready = false;
        }
        state = SessionState::lobby;
        pending_race.reset();
        snapshot.reset();
        results.reset();
        inputs.clear();
        events.push_back(
            {SessionEventKind::race_returned, packet.peer, "returned"});
      }
      break;
    case MessageType::ping_request:
      queue(connection, make_ping(state == SessionState::racing
                                      ? MessageType::ping_response_game
                                      : MessageType::ping_response_lobby,
                                  local_peer, ping_timestamp(packet)));
      break;
    case MessageType::ping_response_lobby:
    case MessageType::ping_response_game:
      if (auto *player = find_player(packet.peer); player != nullptr) {
        const auto sent = ping_timestamp(packet);
        player->ping_ms =
            now_ms >= sent
                ? static_cast<std::uint32_t>(std::min<std::uint64_t>(
                      now_ms - sent, std::numeric_limits<std::uint32_t>::max()))
                : 0U;
      }
      break;
    case MessageType::disconnect:
      if (packet.peer == 0U || packet.peer == local_peer) {
        state = SessionState::offline;
        events.push_back({SessionEventKind::disconnected, packet.peer,
                          "server disconnected"});
        close_socket(connection.socket);
      } else {
        remove_peer(packet.peer, SessionEventKind::player_left, "left");
      }
      break;
    case MessageType::kick:
      if (packet.bytes.size() == 4U) {
        state = SessionState::offline;
        events.push_back(
            {SessionEventKind::disconnected, 0U, "kicked by server"});
        close_socket(connection.socket);
      }
      break;
    default:
      break;
    }
  }

  void remove_peer(const std::uint8_t peer, const SessionEventKind kind,
                   const std::string_view message) {
    const auto found = std::find_if(
        players.begin(), players.end(),
        [peer](const LobbyPlayer &player) { return player.peer == peer; });
    if (found != players.end()) {
      events.push_back(
          {kind, peer, message.empty() ? found->name : std::string(message)});
      players.erase(found);
    }
  }

  void receive(Connection &connection) {
    std::array<std::uint8_t, 4096U> bytes{};
    for (;;) {
      const auto count =
          recv(connection.socket, reinterpret_cast<char *>(bytes.data()),
               static_cast<int>(bytes.size()), 0);
      if (count > 0) {
        constexpr std::size_t maximum_buffered_bytes = 64U * 1024U;
        if (connection.incoming.size() + static_cast<std::size_t>(count) >
            maximum_buffered_bytes) {
          close_socket(connection.socket);
          break;
        }
        connection.last_receive_ms = now_ms;
        connection.incoming.insert(connection.incoming.end(), bytes.begin(),
                                   bytes.begin() + count);
        continue;
      }
      if (count == 0) {
        close_socket(connection.socket);
      } else if (!would_block(socket_error())) {
        close_socket(connection.socket);
      }
      break;
    }
    while (!connection.incoming.empty()) {
      const auto parsed = parse_packet(connection.incoming);
      if (parsed.error == PacketError::incomplete) {
        break;
      }
      if (!parsed.packet.has_value()) {
        close_socket(connection.socket);
        break;
      }
      if (host_mode) {
        process_host_packet(connection, *parsed.packet);
      } else {
        process_client_packet(connection, *parsed.packet);
      }
      connection.incoming.erase(
          connection.incoming.begin(),
          connection.incoming.begin() +
              static_cast<std::ptrdiff_t>(parsed.consumed));
    }
  }

  void send_queued(Connection &connection) {
    while (connection.socket != invalid_socket &&
           connection.sent < connection.outgoing.size()) {
      const auto remaining = connection.outgoing.size() - connection.sent;
      const auto count =
          send(connection.socket,
               reinterpret_cast<const char *>(connection.outgoing.data() +
                                              connection.sent),
               static_cast<int>(std::min<std::size_t>(
                   remaining,
                   static_cast<std::size_t>(std::numeric_limits<int>::max()))),
               0);
      if (count > 0) {
        connection.sent += static_cast<std::size_t>(count);
      } else if (count < 0 && would_block(socket_error())) {
        break;
      } else {
        close_socket(connection.socket);
        break;
      }
    }
    if (connection.sent == connection.outgoing.size()) {
      connection.outgoing.clear();
      connection.sent = 0U;
      if (connection.close_after_send) {
        close_socket(connection.socket);
      }
    }
  }

  void accept_connections() {
    if (listener == invalid_socket) {
      return;
    }
    for (;;) {
      sockaddr_in address{};
      SocketLength length = sizeof(address);
      const auto accepted =
          accept(listener, reinterpret_cast<sockaddr *>(&address), &length);
      if (accepted == invalid_socket) {
        break;
      }
      if (!set_nonblocking(accepted)) {
        Socket copy = accepted;
        close_socket(copy);
        continue;
      }
      Connection connection;
      connection.socket = accepted;
      connection.last_receive_ms = now_ms;
      connections.push_back(std::move(connection));
    }
  }

  void process_discovery() {
    if (discovery == invalid_socket) {
      return;
    }
    std::array<std::uint8_t, 256U> bytes{};
    for (;;) {
      sockaddr_in from{};
      SocketLength from_length = sizeof(from);
      const auto count =
          recvfrom(discovery, reinterpret_cast<char *>(bytes.data()),
                   static_cast<int>(bytes.size()), 0,
                   reinterpret_cast<sockaddr *>(&from), &from_length);
      if (count < 0) {
        break;
      }
      if (host_mode && count == 4 &&
          std::memcmp(bytes.data(), "MHQ1", 4U) == 0) {
        std::vector<std::uint8_t> reply{'M', 'H', 'A', '1'};
        append_u16(reply, port);
        reply.push_back(static_cast<std::uint8_t>(players.size()));
        reply.push_back(maximum_players);
        reply.push_back(password.empty() ? 0U : 1U);
        const auto name = clean_name(session_name, "Motorhead Session");
        reply.insert(reply.end(), name.begin(), name.end());
        (void)sendto(discovery, reinterpret_cast<const char *>(reply.data()),
                     static_cast<int>(reply.size()), 0,
                     reinterpret_cast<const sockaddr *>(&from), from_length);
      } else if (!host_mode && count >= 9 &&
                 std::memcmp(bytes.data(), "MHA1", 4U) == 0) {
        SessionInfo info;
        info.address = address_text(from);
        info.port = read_u16(bytes.data() + 4U);
        info.players = bytes[6U];
        info.capacity = bytes[7U];
        info.password_required = bytes[8U] != 0U;
        info.name.assign(reinterpret_cast<const char *>(bytes.data() + 9U),
                         static_cast<std::size_t>(count - 9));
        const auto found = std::find_if(
            sessions.begin(), sessions.end(), [&info](const SessionInfo &item) {
              return item.address == info.address && item.port == info.port;
            });
        if (found == sessions.end()) {
          sessions.push_back(std::move(info));
        } else {
          *found = std::move(info);
        }
        events.push_back({SessionEventKind::sessions_changed, 0U, {}});
      }
    }
  }

  void send_discovery_query() {
    if (discovery == invalid_socket) {
      return;
    }
    const std::array<char, 4U> query{'M', 'H', 'Q', '1'};
    for (const auto address_text_value : {"255.255.255.255", "127.0.0.1"}) {
      const auto address = make_address(address_text_value, lan_discovery_port);
      (void)sendto(discovery, query.data(), static_cast<int>(query.size()), 0,
                   reinterpret_cast<const sockaddr *>(&address),
                   sizeof(address));
    }
  }
};

LanSession::LanSession()
    : implementation_(std::make_unique<Implementation>()) {}
LanSession::~LanSession() = default;
LanSession::LanSession(LanSession &&) noexcept = default;
LanSession &LanSession::operator=(LanSession &&) noexcept = default;

bool LanSession::browse(const std::uint64_t now_ms) {
  disconnect();
  auto &impl = *implementation_;
  impl.now_ms = now_ms;
  impl.host_mode = false;
  impl.sessions.clear();
  // Queries originate from an ephemeral port and hosts reply to that source.
  // This is deterministic when a host and browser share one machine; binding
  // both sockets to the well-known host port makes Windows delivery ambiguous.
  if (!impl.open_discovery(false)) {
    return false;
  }
  impl.state = SessionState::browsing;
  impl.send_discovery_query();
  impl.last_discovery_ms = now_ms;
  return true;
}

bool LanSession::host(const HostOptions &options, const std::uint64_t now_ms) {
  disconnect();
  auto &impl = *implementation_;
  impl.now_ms = now_ms;
  impl.host_mode = true;
  impl.port = options.port;
  impl.session_name = clean_name(options.session_name, "Motorhead Session");
  impl.password = std::string(options.password.substr(0U, 15U));
  impl.local_name = clean_name(options.player_name, "Player");
  impl.local_car = options.car_selection;
  impl.local_team = clean_team(options.team);
  impl.local_automatic_gear = options.automatic_gear;
  impl.local_car_name = clean_name(options.car_name, "");
  if (options.car_name.empty()) {
    impl.local_car_name.clear();
  }
  impl.local_colour = options.colour;
  impl.listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (impl.listener == invalid_socket || !set_nonblocking(impl.listener)) {
    impl.fail("could not create multiplayer listener");
    return false;
  }
  int enabled = 1;
  (void)setsockopt(impl.listener, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char *>(&enabled), sizeof(enabled));
  const auto address = make_address("*", impl.port);
  if (::bind(impl.listener, reinterpret_cast<const sockaddr *>(&address),
             sizeof(address)) != 0 ||
      listen(impl.listener, maximum_players - 1U) != 0) {
    impl.fail("could not bind multiplayer session port");
    return false;
  }
  if (!impl.open_discovery(true)) {
    return false;
  }
  impl.local_peer = 0U;
  impl.players = {{0U, impl.local_name, impl.local_car, false, true,
                   impl.local_team, impl.local_automatic_gear,
                   impl.local_car_name, impl.local_colour, 0U, 0U, 1U}};
  impl.state = SessionState::lobby;
  impl.events.push_back({SessionEventKind::connected, 0U, "hosting"});
  return true;
}

bool LanSession::join(const JoinOptions &options, const std::uint64_t now_ms) {
  disconnect();
  auto &impl = *implementation_;
  impl.now_ms = now_ms;
  impl.host_mode = false;
  impl.port = options.port;
  impl.password = std::string(options.password.substr(0U, 15U));
  impl.local_name = clean_name(options.player_name, "Player");
  impl.local_car = options.car_selection;
  impl.local_team = clean_team(options.team);
  impl.local_automatic_gear = options.automatic_gear;
  impl.local_car_name = options.car_name.empty()
                            ? std::string{}
                            : clean_name(options.car_name, "");
  impl.local_colour = options.colour;
  Implementation::Connection connection;
  connection.socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (connection.socket == invalid_socket ||
      !set_nonblocking(connection.socket)) {
    impl.fail("could not create multiplayer connection");
    return false;
  }
  const auto address = make_address(options.address, options.port);
  const auto result =
      connect(connection.socket, reinterpret_cast<const sockaddr *>(&address),
              sizeof(address));
  if (result != 0 && !would_block(socket_error())) {
    impl.fail("could not connect to multiplayer host");
    return false;
  }
  connection.last_receive_ms = now_ms;
  impl.queue(connection, make_join_request(0U, impl.password));
  impl.connections.push_back(std::move(connection));
  impl.state = SessionState::connecting;
  return true;
}

void LanSession::tick(const std::uint64_t now_ms) {
  auto &impl = *implementation_;
  impl.now_ms = now_ms;
  if (impl.host_mode) {
    impl.accept_connections();
  }
  impl.process_discovery();
  if (impl.state == SessionState::browsing &&
      now_ms - impl.last_discovery_ms >= 1000U) {
    impl.send_discovery_query();
    impl.last_discovery_ms = now_ms;
  }
  for (auto &connection : impl.connections) {
    if (connection.socket != invalid_socket) {
      impl.send_queued(connection);
      impl.receive(connection);
      impl.send_queued(connection);
    }
    if (connection.socket == invalid_socket && connection.accepted) {
      if (impl.host_mode) {
        if (impl.find_player(connection.peer) != nullptr) {
          impl.broadcast(make_packet(MessageType::disconnect, connection.peer));
          impl.remove_peer(connection.peer, SessionEventKind::player_left,
                           "connection closed");
          impl.broadcast_status();
        }
      } else if (impl.state == SessionState::lobby ||
                 impl.state == SessionState::racing ||
                 impl.state == SessionState::connecting) {
        impl.state = SessionState::offline;
        impl.events.push_back(
            {SessionEventKind::disconnected, 0U, "server disconnected"});
      }
    }
  }
  for (auto &connection : impl.connections) {
    const auto timeout_ms =
        impl.state == SessionState::racing ? 60000U : 10000U;
    if (connection.socket != invalid_socket &&
        now_ms - connection.last_receive_ms > timeout_ms) {
      const auto peer = connection.peer;
      const auto accepted = connection.accepted;
      close_socket(connection.socket);
      if (impl.host_mode) {
        if (accepted) {
          impl.broadcast(make_packet(MessageType::disconnect, peer));
          impl.remove_peer(peer, SessionEventKind::timed_out, "timed out");
          impl.broadcast_status();
        }
      } else {
        impl.state = SessionState::offline;
        impl.events.push_back(
            {SessionEventKind::timed_out, 0U, "server timed out"});
      }
    }
  }
  impl.connections.erase(
      std::remove_if(impl.connections.begin(), impl.connections.end(),
                     [](const Implementation::Connection &connection) {
                       return connection.socket == invalid_socket &&
                              connection.outgoing.empty();
                     }),
      impl.connections.end());
  if ((impl.state == SessionState::lobby ||
       impl.state == SessionState::racing) &&
      now_ms - impl.last_ping_ms >= 1000U) {
    const auto ping = make_ping(MessageType::ping_request, impl.local_peer,
                                static_cast<std::uint32_t>(now_ms));
    if (impl.host_mode) {
      impl.broadcast(ping);
    } else if (!impl.connections.empty()) {
      impl.queue(impl.connections.front(), ping);
    }
    impl.last_ping_ms = now_ms;
  }
}

void LanSession::disconnect(const std::string_view reason) {
  auto &impl = *implementation_;
  if (impl.state == SessionState::lobby || impl.state == SessionState::racing ||
      impl.state == SessionState::connecting) {
    const auto packet = make_packet(impl.state == SessionState::racing
                                        ? MessageType::leave_game
                                        : MessageType::disconnect,
                                    impl.local_peer);
    if (impl.host_mode) {
      impl.broadcast(packet);
    } else if (!impl.connections.empty()) {
      impl.queue(impl.connections.front(), packet);
      impl.send_queued(impl.connections.front());
    }
  }
  impl.connections.clear();
  close_socket(impl.listener);
  close_socket(impl.discovery);
  impl.players.clear();
  impl.pending_race.reset();
  impl.snapshot.reset();
  impl.results.reset();
  impl.host_mode = false;
  impl.local_peer = 0U;
  impl.state = SessionState::offline;
  if (!reason.empty()) {
    impl.events.push_back(
        {SessionEventKind::disconnected, 0U, std::string(reason)});
  }
}

SessionState LanSession::state() const noexcept {
  return implementation_->state;
}
bool LanSession::is_host() const noexcept { return implementation_->host_mode; }
std::uint8_t LanSession::local_peer() const noexcept {
  return implementation_->local_peer;
}
const std::vector<SessionInfo> &LanSession::sessions() const noexcept {
  return implementation_->sessions;
}
const std::vector<LobbyPlayer> &LanSession::players() const noexcept {
  return implementation_->players;
}
std::vector<SessionEvent> LanSession::take_events() {
  return std::exchange(implementation_->events, {});
}
const std::string &LanSession::last_error() const noexcept {
  return implementation_->error;
}

bool LanSession::set_ready(const bool ready) {
  auto &impl = *implementation_;
  if (impl.state != SessionState::lobby) {
    return false;
  }
  if (auto *player = impl.find_player(impl.local_peer); player != nullptr) {
    player->ready = ready;
  }
  if (impl.host_mode) {
    impl.broadcast_status();
    impl.events.push_back({SessionEventKind::ready_changed, impl.local_peer,
                           ready ? "ready" : "not ready"});
    return true;
  }
  if (impl.connections.empty()) {
    return false;
  }
  impl.queue(impl.connections.front(),
             make_ready_request(impl.local_peer, ready));
  return true;
}

bool LanSession::send_lobby_message(const std::string_view message) {
  auto &impl = *implementation_;
  if (impl.state != SessionState::lobby) {
    return false;
  }
  std::string clean;
  clean.reserve(std::min<std::size_t>(message.size(), 255U));
  for (const auto character : message) {
    const auto byte = static_cast<unsigned char>(character);
    if (byte >= 0x20U && byte <= 0x7eU && clean.size() < 255U) {
      clean.push_back(character);
    }
  }
  if (clean.empty()) {
    return false;
  }
  if (impl.host_mode) {
    impl.events.push_back(
        {SessionEventKind::lobby_message_received, impl.local_peer, clean});
    impl.broadcast(
        make_lobby_message(MessageType::lobby_message, impl.local_peer, clean));
    return true;
  }
  if (impl.connections.empty()) {
    return false;
  }
  impl.queue(impl.connections.front(),
             make_lobby_message(MessageType::lobby_message_request,
                                impl.local_peer, clean));
  return true;
}

bool LanSession::kick(const std::uint8_t peer) {
  auto &impl = *implementation_;
  if (!impl.host_mode || impl.state != SessionState::lobby || peer == 0U) {
    return false;
  }
  const auto found =
      std::find_if(impl.connections.begin(), impl.connections.end(),
                   [peer](const Implementation::Connection &connection) {
                     return connection.accepted && connection.peer == peer;
                   });
  if (found == impl.connections.end()) {
    return false;
  }
  Packet packet{MessageType::kick, 0U, std::vector<std::uint8_t>(4U, 0U)};
  packet.bytes[0U] = static_cast<std::uint8_t>(MessageType::kick);
  packet.bytes[2U] = 4U;
  impl.queue(*found, packet);
  found->close_after_send = true;
  impl.broadcast(make_packet(MessageType::disconnect, peer), found->socket);
  impl.remove_peer(peer, SessionEventKind::player_left, "kicked");
  impl.broadcast_status();
  return true;
}

bool LanSession::clear_scores() {
  auto &impl = *implementation_;
  if (!impl.host_mode || impl.state != SessionState::lobby) {
    return false;
  }
  for (auto &player : impl.players) {
    player.score = 0U;
    impl.broadcast(
        make_player_data(MessageType::join_player, player.peer,
                         impl.player_fields(player, player.ready ? 1U : 0U)));
  }
  impl.events.push_back(
      {SessionEventKind::player_changed, impl.local_peer, "scores cleared"});
  return true;
}

bool LanSession::update_player(const std::string_view name,
                               const std::uint8_t car_selection) {
  auto &impl = *implementation_;
  if (impl.state != SessionState::lobby) {
    return false;
  }
  impl.local_name = clean_name(name, "Player");
  impl.local_car = car_selection;
  if (auto *player = impl.find_player(impl.local_peer); player != nullptr) {
    player->name = impl.local_name;
    player->car_selection = car_selection;
  }
  const auto packet = make_player_data(MessageType::player_data,
                                       impl.local_peer, impl.local_fields());
  if (impl.host_mode) {
    impl.broadcast(make_player_data(MessageType::join_player, impl.local_peer,
                                    impl.local_fields()));
  } else if (!impl.connections.empty()) {
    impl.queue(impl.connections.front(), packet);
  }
  return true;
}

bool LanSession::update_player_presentation(const std::string_view team,
                                            const bool automatic_gear,
                                            const std::string_view car_name,
                                            const std::uint32_t colour) {
  auto &impl = *implementation_;
  if (impl.state != SessionState::lobby) {
    return false;
  }
  impl.local_team = clean_team(team);
  impl.local_automatic_gear = automatic_gear;
  impl.local_car_name =
      car_name.empty() ? std::string{} : clean_name(car_name, "");
  impl.local_colour = colour;
  if (auto *player = impl.find_player(impl.local_peer); player != nullptr) {
    player->team = impl.local_team;
    player->automatic_gear = automatic_gear;
    player->car_name = impl.local_car_name;
    player->colour = colour;
  }
  const auto type =
      impl.host_mode ? MessageType::join_player : MessageType::player_data;
  const auto packet =
      make_player_data(type, impl.local_peer, impl.local_fields());
  if (impl.host_mode) {
    impl.broadcast(packet);
  } else if (!impl.connections.empty()) {
    impl.queue(impl.connections.front(), packet);
  }
  return true;
}

bool LanSession::start_race(const RaceConfiguration &configuration) {
  auto &impl = *implementation_;
  if (!impl.host_mode || impl.state != SessionState::lobby ||
      impl.players.size() < 2U ||
      std::any_of(impl.players.begin(), impl.players.end(),
                  [](const LobbyPlayer &player) { return !player.ready; })) {
    return false;
  }
  impl.pending_race = configuration;
  impl.broadcast(make_race_configuration(impl.local_peer, configuration));
  impl.broadcast(make_packet(MessageType::start_request));
  impl.state = SessionState::racing;
  impl.events.push_back({SessionEventKind::race_started, impl.local_peer, {}});
  return true;
}

bool LanSession::update_race_configuration(
    const RaceConfiguration &configuration) {
  auto &impl = *implementation_;
  if (!impl.host_mode || impl.state != SessionState::lobby) {
    return false;
  }
  impl.pending_race = configuration;
  impl.broadcast(make_race_configuration(impl.local_peer, configuration));
  return true;
}

std::optional<RaceConfiguration>
LanSession::pending_race_configuration() const noexcept {
  return implementation_->pending_race;
}

bool LanSession::send_input(const RaceInputFrame &input) {
  auto &impl = *implementation_;
  if (impl.state != SessionState::racing) {
    return false;
  }
  if (impl.host_mode) {
    impl.inputs.emplace_back(impl.local_peer, input);
  } else if (!impl.connections.empty()) {
    impl.queue(impl.connections.front(),
               make_race_input(impl.local_peer, input));
  } else {
    return false;
  }
  return true;
}

std::vector<std::pair<std::uint8_t, RaceInputFrame>> LanSession::take_inputs() {
  return std::exchange(implementation_->inputs, {});
}

bool LanSession::publish_snapshot(const RaceSnapshot &snapshot) {
  auto &impl = *implementation_;
  if (!impl.host_mode || impl.state != SessionState::racing) {
    return false;
  }
  impl.broadcast(make_race_snapshot(impl.local_peer, snapshot));
  return true;
}

std::optional<RaceSnapshot> LanSession::take_snapshot() {
  return std::exchange(implementation_->snapshot, std::nullopt);
}

bool LanSession::publish_results(
    const std::span<const RaceResultData> results) {
  auto &impl = *implementation_;
  if (!impl.host_mode || impl.state != SessionState::racing) {
    return false;
  }
  impl.results = std::vector<RaceResultData>(results.begin(), results.end());
  impl.broadcast(make_race_results(impl.local_peer, results));
  return true;
}

std::optional<std::vector<RaceResultData>> LanSession::take_results() {
  return std::exchange(implementation_->results, std::nullopt);
}

void LanSession::finish_race() {
  auto &impl = *implementation_;
  if (impl.state != SessionState::racing) {
    return;
  }
  for (auto &player : impl.players) {
    player.ready = false;
  }
  const auto packet = make_packet(MessageType::leave_game, impl.local_peer);
  if (impl.host_mode) {
    impl.broadcast(packet);
    impl.broadcast_status();
    for (auto &connection : impl.connections) {
      if (connection.accepted && connection.socket != invalid_socket) {
        impl.send_queued(connection);
      }
    }
    impl.events.push_back(
        {SessionEventKind::race_returned, impl.local_peer, "returned"});
  } else if (!impl.connections.empty()) {
    impl.queue(impl.connections.front(), packet);
    impl.send_queued(impl.connections.front());
  }
  impl.state = SessionState::lobby;
  impl.pending_race.reset();
  impl.snapshot.reset();
  impl.results.reset();
  impl.inputs.clear();
}

} // namespace mh::network
