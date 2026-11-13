#include <platform/native_socket.hpp>
#include <array>
#include <cerrno>
#include <stdexcept>
namespace mh::platform::sockets {
void close_socket(Socket &socket) noexcept {
  if (socket == invalid_socket) {
    return;
  }
  closesocket(socket);
  socket = invalid_socket;
}

int socket_error() noexcept {
  return WSAGetLastError();
}

bool would_block(const int error) noexcept {
  return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS ||
         error == WSAEALREADY || error == WSAENOTCONN;
}

bool set_nonblocking(const Socket socket) noexcept {
  u_long enabled = 1UL;
  return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

std::string address_text(const sockaddr_in &address) {
  const auto *text = inet_ntoa(address.sin_addr);
  return text != nullptr ? std::string(text) : std::string{};
}

sockaddr_in make_address(const std::string_view text,
                         const std::uint16_t port) {
  sockaddr_in result{};
  result.sin_family = AF_INET;
  result.sin_port = htons(port);
  if (text.empty() || text == "*") {
    result.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
    auto source = std::string(text);
    auto length = static_cast<int>(sizeof(result));
    if (WSAStringToAddressA(source.data(), AF_INET, nullptr,
                            reinterpret_cast<sockaddr *>(&result),
                            &length) != 0) {
      throw std::runtime_error("invalid IPv4 address: " + source);
    }
    result.sin_port = htons(port);
  }
  return result;
}

}
