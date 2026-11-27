// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_link_life.h"

#include <chrono>

#include "common/logging.h"
#include "coop_life_gauge.h"
#include "core/core.h"
#include "core/memory.h"

namespace CoopLinkLife {

bool GameLinkLife::Put(bool max, std::int32_t quarters) {
    if (!words_) words_ = CoopObjects::FindOurLinkLife(system_, main_);
    auto& memory = system_.ApplicationMemory();
    const std::uint64_t at = words_ ? (max ? words_->max : words_->life) : 0;
    const char* what = max ? "max life" : "life";
    if (at == 0 || quarters < 0 || !memory.IsValidVirtualAddressRange(at, 4)) {
        LOG_INFO(Core, "coop: test our Link's {}: NOT written: no Link's Life component found (no Link in play)", what);
        return false;
    }
    const auto was = static_cast<std::int32_t>(memory.Read32(at));
    memory.Write32(at, static_cast<std::uint32_t>(quarters));
    LOG_INFO(Core, "coop: test our Link's {} {} -> {} (his Life component; the flag is its mirror)", what, was,
             quarters);
    CoopLifeGauge::TheRefresher().Want(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()));
    return true;
}

}  // namespace CoopLinkLife
