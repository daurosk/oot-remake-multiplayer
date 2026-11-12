#pragma once

// The stuck command a path check has marked (enet_path.hpp), without ENet's
// headers, so a class that keeps one need not include them.

#include <cstdint>
#include <optional>

namespace coop::net {

// A reliable command as ENet numbers it, and how often it was sent.
struct StuckCommand {
  std::uint8_t channel = 0;
  std::uint16_t sequence = 0;
  std::uint16_t sends = 0;
};

// One per peer: the command marked as stuck, if any.
using PathMark = std::optional<StuckCommand>;

}  // namespace coop::net
