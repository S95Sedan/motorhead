#include <platform/native_socket.hpp>
#include <array>
#include <cerrno>
#include <stdexcept>
namespace mh::platform::sockets {
void close_socket(Socket &socket) noexcept {
  if (socket == invalid_socket) {
    return;
  }
  close(socket);
  socket = invalid_socket;
}

int socket_error() noexcept {
  return errno;
}

bool would_block(const int error) noexcept {
  return error == EWOULDBLOCK || error == EAGAIN || error == EINPROGRESS ||
         error == EALREADY || error == ENOTCONN;
}

bool set_nonblocking(const Socket socket) noexcept {
  const auto flags = fcntl(socket, F_GETFL, 0);
  return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
}

std::string address_text(const sockaddr_in &address) {
  std::array<char, INET_ADDRSTRLEN> text{};
  if (inet_ntop(AF_INET, &address.sin_addr, text.data(),
                static_cast<socklen_t>(text.size())) == nullptr) {
    return {};
  }
  return text.data();
}

sockaddr_in make_address(const std::string_view text,
                         const std::uint16_t port) {
  sockaddr_in result{};
  result.sin_family = AF_INET;
  result.sin_port = htons(port);
  if (text.empty() || text == "*") {
    result.sin_addr.s_addr = htonl(INADDR_ANY);
  } else {
    if (inet_pton(AF_INET, std::string(text).c_str(), &result.sin_addr) != 1) {
      throw std::runtime_error("invalid IPv4 address: " + std::string(text));
    }
  }
  return result;
}

}
