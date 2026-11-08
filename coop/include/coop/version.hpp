#pragma once

#include <cstdint>

namespace coop {

// Placeholder until coop/protocol lands. Returns the linked ENet version
// packed as ENET_VERSION_CREATE(major, minor, patch).
std::uint32_t enet_linked_version();

// Calls enet_initialize and enet_deinitialize. True when ENet starts.
bool enet_starts();

}  // namespace coop
