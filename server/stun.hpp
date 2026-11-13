#pragma once
// STUN Binding (RFC 5389), just enough to learn which internet address and
// port a router gives our UDP socket: one request, one answer. The request
// goes from the game server's own socket (or the join helper's), so the
// address in the answer is the one the friend's packets reach.
//
// Only an answer with our 96-bit transaction id is used (the caller also
// checks it came from the server it asked), and every length in it is checked
// before it is read.

#include <array>
#include <cstdint>
#include <span>

#include "nat_codes.hpp"

namespace coop::server::nat {

inline constexpr std::uint32_t kStunCookie = 0x2112A442u;
using StunId = std::array<std::uint8_t, 12>;

// The 20-byte Binding request.
std::array<std::uint8_t, 20> stun_request(const StunId& id);

enum class StunResult {
  kOk,
  kNotStun,     // not a STUN message at all (the caller hands it on)
  kOtherId,     // STUN, but not the answer to our request
  kMalformed,   // lengths that do not add up
  kNoAddress,   // a fine answer without an IPv4 address
  kErrorReply,  // the server answered with an error
};
struct StunAnswer {
  StunResult result = StunResult::kNotStun;
  Addr mapped;
};
// XOR-MAPPED-ADDRESS is used first, MAPPED-ADDRESS only when it is missing.
StunAnswer read_stun(std::span<const std::uint8_t> datagram, const StunId& id);

}  // namespace coop::server::nat
