#pragma once
// Where a server-library socket binds: one IPv4 address of this PC, or all of
// them. The test guard (coop/net/bind_guard.hpp) is asked first.

#include <cstdint>
#include <cstring>
#include <optional>

#include <enet/enet.h>

#include "coop/net/bind_guard.hpp"

namespace coop::server {

// ENet's address for `bind` (host order; none: all addresses) and `port`.
inline ENetAddress local_address(std::optional<std::uint32_t> bind, std::uint16_t port) {
  ENetAddress address{};
  address.host = ENET_HOST_ANY;
  if (bind) {
    const std::uint32_t v = *bind;
    const unsigned char b[4] = {static_cast<unsigned char>(v >> 24), static_cast<unsigned char>(v >> 16),
                                static_cast<unsigned char>(v >> 8), static_cast<unsigned char>(v)};
    std::memcpy(&address.host, b, 4);  // (ENet keeps the address in network byte order)
  }
  address.port = port;
  return address;
}

// enet_socket_bind for `bind` and `port`; -1 when the test guard refuses.
inline int bind_socket(ENetSocket socket, std::optional<std::uint32_t> bind, std::uint16_t port) {
  if (!net::bind_allowed(bind)) return -1;
  const ENetAddress address = local_address(bind, port);
  return enet_socket_bind(socket, &address);
}

}  // namespace coop::server
