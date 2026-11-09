#include "coop_hook.h"

#include <atomic>

#include "common/logging.h"
#include "coop/version.hpp"

namespace CoopHook {

void OnFrame(Core::System& /*system*/) {
  static std::atomic<bool> logged{false};
  if (logged.exchange(true, std::memory_order_relaxed)) {
    return;
  }
  const auto v = coop::enet_linked_version();
  LOG_INFO(Core, "coop: loaded (ENet {}.{}.{})", (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
}

}  // namespace CoopHook
