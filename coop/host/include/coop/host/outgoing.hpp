#pragma once

#include <cstdint>
#include <vector>

namespace coop::host {

// One encoded message for the caller to send (coop::net::Client), on its
// channel. One message, one ENet packet.
struct Outgoing {
  std::uint8_t channel = 0;
  std::vector<std::uint8_t> bytes;
};

}  // namespace coop::host
