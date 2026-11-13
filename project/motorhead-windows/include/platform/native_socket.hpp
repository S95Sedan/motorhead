#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <stdexcept>
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

namespace mh::platform::sockets {
using Socket = SOCKET;
inline constexpr Socket invalid_socket = INVALID_SOCKET;
using SocketLength = int;

void close_socket(Socket &) noexcept;
int socket_error() noexcept;
bool would_block(int) noexcept;
bool set_nonblocking(Socket) noexcept;
std::string address_text(const sockaddr_in &);
sockaddr_in make_address(std::string_view, std::uint16_t);
  struct RuntimeGuard {
    RuntimeGuard() {
      WSADATA data{};
      if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        throw std::runtime_error("Winsock initialization failed");
      }
    }
    ~RuntimeGuard() {
      WSACleanup();
    }
  };
}
