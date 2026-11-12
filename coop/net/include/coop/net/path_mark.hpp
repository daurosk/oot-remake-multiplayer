#pragma once

// What a path check keeps per peer (enet_path.hpp), without ENet's headers,
// so a class that keeps one need not include them.

#include <cstdint>
#include <optional>
#include <vector>

namespace coop::net {

// A reliable command as ENet numbers it, and how often it was sent.
struct StuckCommand {
  std::uint8_t channel = 0;
  std::uint16_t sequence = 0;
  std::uint16_t sends = 0;
};

// One per connection; a new connection starts from an empty one.
struct PathMark {
  // The big command marked as stuck, if any.
  std::optional<StuckCommand> marked;
  // A big command (one that cannot fit a small datagram) was acknowledged
  // on this connection: the path carries full datagrams.
  bool big_acked = false;
  // The big commands sent and waiting for their ACK at the last check, as
  // channel << 16 | sequence, sorted. One gone from the list was acknowledged.
  std::vector<std::uint32_t> big_waiting;
};

}  // namespace coop::net
