#pragma once

#include <network/protocol.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mh::network {

struct RaceRestartRouting {
  bool send_request = false;
  bool apply_local = false;
};

// A client-owned restart edge is a request and must not mutate its simulation.
// A generation received from the host is the inverse: apply it locally without
// echoing another request. Keeping those paths disjoint prevents restart loops.
[[nodiscard]] constexpr RaceRestartRouting
route_race_restart(const bool host, const bool restart_pressed,
                   const bool authoritative_commit) noexcept {
  if (!restart_pressed) {
    return {};
  }
  if (host || authoritative_commit) {
    return {false, true};
  }
  return {true, false};
}

inline constexpr std::uint16_t lan_session_port = 23098U;
inline constexpr std::uint16_t lan_discovery_port = 23099U;

struct SessionInfo {
  std::string name;
  std::string address;
  std::uint16_t port = lan_session_port;
  std::uint8_t players = 0U;
  std::uint8_t capacity = maximum_players;
  bool password_required = false;
};

struct LobbyPlayer {
  std::uint8_t peer = 0U;
  std::string name;
  std::uint8_t car_selection = 0U;
  bool ready = false;
  bool host = false;
  std::string team;
  bool automatic_gear = true;
  std::string car_name;
  std::uint32_t colour = 0x00e06820U;
  std::uint32_t races = 0U;
  std::uint32_t score = 0U;
  std::uint8_t grid_position = 0U;
  std::uint8_t lap = 0U;
  std::uint32_t ping_ms = 0U;
};

enum class SessionState : std::uint8_t {
  offline,
  browsing,
  connecting,
  lobby,
  racing,
  failed,
};

enum class SessionEventKind : std::uint8_t {
  sessions_changed,
  connected,
  connection_denied,
  player_joined,
  player_changed,
  player_left,
  lobby_message_received,
  ready_changed,
  configuration_changed,
  race_started,
  race_returned,
  results_received,
  disconnected,
  timed_out,
  error,
};

struct SessionEvent {
  SessionEventKind kind = SessionEventKind::error;
  std::uint8_t peer = 0U;
  std::string message;
};

struct HostOptions {
  std::string session_name = "Motorhead Session";
  std::string password;
  std::string player_name = "Player";
  std::uint8_t car_selection = 0U;
  std::uint16_t port = lan_session_port;
  std::string team;
  bool automatic_gear = true;
  std::string car_name;
  std::uint32_t colour = 0x00e06820U;
};

struct JoinOptions {
  std::string address = "127.0.0.1";
  std::string password;
  std::string player_name = "Player";
  std::uint8_t car_selection = 0U;
  std::uint16_t port = lan_session_port;
  std::string team;
  bool automatic_gear = true;
  std::string car_name;
  std::uint32_t colour = 0x00e06820U;
};

class LanSession {
public:
  LanSession();
  ~LanSession();
  LanSession(LanSession &&) noexcept;
  LanSession &operator=(LanSession &&) noexcept;
  LanSession(const LanSession &) = delete;
  LanSession &operator=(const LanSession &) = delete;

  [[nodiscard]] bool browse(std::uint64_t now_ms);
  [[nodiscard]] bool host(const HostOptions &options, std::uint64_t now_ms);
  [[nodiscard]] bool join(const JoinOptions &options, std::uint64_t now_ms);
  void tick(std::uint64_t now_ms);
  void disconnect(std::string_view reason = {});

  [[nodiscard]] SessionState state() const noexcept;
  [[nodiscard]] bool is_host() const noexcept;
  [[nodiscard]] std::uint8_t local_peer() const noexcept;
  [[nodiscard]] const std::vector<SessionInfo> &sessions() const noexcept;
  [[nodiscard]] const std::vector<LobbyPlayer> &players() const noexcept;
  [[nodiscard]] std::vector<SessionEvent> take_events();
  [[nodiscard]] const std::string &last_error() const noexcept;

  [[nodiscard]] bool set_ready(bool ready);
  [[nodiscard]] bool send_lobby_message(std::string_view message);
  [[nodiscard]] bool kick(std::uint8_t peer);
  [[nodiscard]] bool clear_scores();
  [[nodiscard]] bool update_player(std::string_view name,
                                   std::uint8_t car_selection);
  [[nodiscard]] bool update_player_presentation(std::string_view team,
                                                bool automatic_gear,
                                                std::string_view car_name,
                                                std::uint32_t colour);
  [[nodiscard]] bool start_race(const RaceConfiguration &configuration);
  [[nodiscard]] bool
  update_race_configuration(const RaceConfiguration &configuration);
  [[nodiscard]] std::optional<RaceConfiguration>
  pending_race_configuration() const noexcept;

  [[nodiscard]] bool send_input(const RaceInputFrame &input);
  [[nodiscard]] std::vector<std::pair<std::uint8_t, RaceInputFrame>>
  take_inputs();
  [[nodiscard]] bool publish_snapshot(const RaceSnapshot &snapshot);
  [[nodiscard]] std::optional<RaceSnapshot> take_snapshot();
  [[nodiscard]] bool publish_results(std::span<const RaceResultData> results);
  [[nodiscard]] std::optional<std::vector<RaceResultData>> take_results();
  void finish_race();

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

} // namespace mh::network
