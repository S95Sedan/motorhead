#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mh::platform::sockets {
using Socket = int;
inline constexpr Socket invalid_socket = -1;
using SocketLength = socklen_t;

void close_socket(Socket &) noexcept;
int socket_error() noexcept;
bool would_block(int) noexcept;
bool set_nonblocking(Socket) noexcept;
std::string address_text(const sockaddr_in &);
sockaddr_in make_address(std::string_view, std::uint16_t);
  struct RuntimeGuard {
    RuntimeGuard() {
    }
    ~RuntimeGuard() {
    }
  };
}
