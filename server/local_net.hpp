#pragma once
// This PC's IPv4 networks: each network card's address, netmask and router
// (gateway). The join helper sends its local-network probe to each one's
// broadcast address (a PC with a VPN card has several), and the router
// forward (pcp.hpp) asks each router.

#include <cstdint>
#include <vector>

namespace coop::server::nat {

struct LocalNet {
  std::uint32_t ip = 0;       // this PC's address on it (first part highest)
  std::uint32_t mask = 0;     // 255.255.255.0 = 0xFFFFFF00
  std::uint32_t gateway = 0;  // its router; 0 when it has none
  bool default_route = false; // the card the system sends internet traffic through
};

// Its router lies in its own network (a gateway elsewhere is not one we ask).
bool gateway_on_link(const LocalNet& net);

// The cards that are up, without loopback ones. Empty when the system does
// not say (or on a system this does not know).
std::vector<LocalNet> local_networks();

// The broadcast address of a network (ip with every host bit set); nothing
// useful for /31 and /32 networks, which have no broadcast address: 0.
std::uint32_t broadcast_of(const LocalNet& net);

// Netmask from a prefix length (24 -> 255.255.255.0).
std::uint32_t mask_of(unsigned prefix_length);

}  // namespace coop::server::nat
