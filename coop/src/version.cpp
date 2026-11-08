#include "coop/version.hpp"

#include <enet/enet.h>

namespace coop {

std::uint32_t enet_linked_version() {
  return ::enet_linked_version();
}

bool enet_starts() {
  if (enet_initialize() != 0) {
    return false;
  }
  enet_deinitialize();
  return true;
}

}  // namespace coop
