#pragma once
// nat::Addr and ENet's addresses, and finding STUN servers by name.

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <enet/enet.h>

#include "nat_codes.hpp"

namespace coop::server::nat {

// ENet keeps the IPv4 address in network byte order.
inline Addr from_enet(const ENetAddress& a) {
  unsigned char b[4];
  std::memcpy(b, &a.host, 4);
  return Addr{(std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) | (std::uint32_t{b[2]} << 8) | b[3], a.port};
}
inline ENetAddress to_enet(Addr a) {
  ENetAddress out{};
  const unsigned char b[4] = {static_cast<unsigned char>(a.ip >> 24), static_cast<unsigned char>(a.ip >> 16),
                              static_cast<unsigned char>(a.ip >> 8), static_cast<unsigned char>(a.ip)};
  std::memcpy(&out.host, b, 4);
  out.port = a.port;
  return out;
}

// The free public STUN servers asked when the command line names none. They
// see this PC's internet address (no game data).
inline const std::vector<std::string>& default_stun_servers() {
  static const std::vector<std::string> list = {"stun.l.google.com:19302", "stun.cloudflare.com:3478"};
  return list;
}

// "name:port" (or an IPv4 address) looked up once; nothing if it does not
// resolve. The lookup blocks (it runs before the game loop starts).
inline std::optional<Addr> resolve(std::string_view text, std::uint16_t default_port) {
  std::string host(text);
  std::uint16_t port = default_port;
  const auto colon = host.rfind(':');
  if (colon != std::string::npos) {
    const std::string digits = host.substr(colon + 1);
    host.resize(colon);
    if (digits.empty() || digits.size() > 5) return std::nullopt;
    unsigned long v = 0;
    for (char c : digits) {
      if (c < '0' || c > '9') return std::nullopt;
      v = v * 10 + static_cast<unsigned long>(c - '0');
    }
    if (v == 0 || v > 65535) return std::nullopt;
    port = static_cast<std::uint16_t>(v);
  }
  if (host.empty() || host.size() > 253) return std::nullopt;
  ENetAddress a{};
  if (enet_address_set_host(&a, host.c_str()) != 0) return std::nullopt;
  a.port = port;
  return from_enet(a);
}

}  // namespace coop::server::nat
