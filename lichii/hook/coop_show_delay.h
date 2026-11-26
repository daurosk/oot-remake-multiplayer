// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// How late each other player's game is drawn (player report #37), the same
// for everything that player sends: its puppet's place (coop/presence,
// show_delay.hpp, where the delay is figured from its samples' ages), its
// pose (CoopPose::Track) and the enemies it owns (CoopObjects::Sync). The
// puppets' Collect writes it every frame; the others read it. Before a
// player's first frame, or for player 0: kFloorUs, the old fixed delay.

#include <array>
#include <atomic>
#include <cstdint>

#include "coop/protocol/messages.hpp"

namespace CoopShowDelay {

inline constexpr std::uint64_t kFloorUs = 100'000;  // coop/presence Settings::show_delay_us

inline std::array<std::atomic<std::uint64_t>, coop::protocol::kMaxPlayers + 1>& Table() {
    static std::array<std::atomic<std::uint64_t>, coop::protocol::kMaxPlayers + 1> table{};
    return table;
}

inline void Set(std::uint8_t player, std::uint64_t delay_us) {
    if (player != 0 && player < Table().size()) {
        Table()[player].store(delay_us, std::memory_order_relaxed);
    }
}

inline std::uint64_t Us(std::uint8_t player) {
    if (player == 0 || player >= Table().size()) return kFloorUs;
    const std::uint64_t d = Table()[player].load(std::memory_order_relaxed);
    return d < kFloorUs ? kFloorUs : d;
}

} // namespace CoopShowDelay
