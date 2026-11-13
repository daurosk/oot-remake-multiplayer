// SPDX-License-Identifier: GPL-3.0-or-later
// Local co-op's input (docs/local-coop.md, step 2): each extra player on this
// PC drives its own Player_<Age> with its own controller, through the game's
// own controller code.
//
// - Hookshot (patch 0016) makes a pad a local player took the game's Npad
//   No.2 to No.4, so the game's controller manager has that pad as one of its
//   controllers, with its own held / pressed / released words.
// - The Player reads its pad in two functions (wraps 16 and 17,
//   coop_player_input.h), each asking the manager's getController
//   (main+0x3b25e00, wrap 19) for controller 0. While the Player of local
//   player k reads, the ask is turned into controller k; every other reader
//   (menus, the Ocarina, the first Link) still gets controller 0.
//
// No game or Hookshot code here: what the wraps decide, for the tests.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>

namespace CoopLocalInput {

// Local players beyond the first (the first Link is player 0).
inline constexpr int kMostExtra = 3;

namespace addr {
// getController(manager, index, hash): index 0 is [manager+0xb0], 1 to 3 are
// [manager+0x110 + 8 * (index - 1)], 4 is [manager+0xb8] (main+0x3b25e10).
inline constexpr std::uint64_t kControllerFirst = 0xb0;
inline constexpr std::uint64_t kControllerList = 0x110;
inline constexpr std::uint64_t kControllerLast = 0xb8;
// main+: the pointer to the controller manager (its getController's x0).
inline constexpr std::uint64_t kControllerManager = 0x4e92f10;
}  // namespace addr

// Where the manager keeps controller `index`, as getController picks it.
constexpr std::uint64_t ControllerSlot(std::uint32_t index) {
    if (index == 0) return addr::kControllerFirst;
    if (index == 4) return addr::kControllerLast;
    if (index >= 1 && index <= 3) return addr::kControllerList + 8u * (index - 1u);
    return addr::kControllerList;   // (the game's own fallback for any other index)
}

// The controller local player k reads: Npad No.(k+1) is the manager's
// controller k. Read from the code, not yet seen in a run: the research run's
// log line ("local input: controller k is ...") proves it or not.
constexpr std::uint32_t ControllerFor(int player) { return static_cast<std::uint32_t>(player); }

// Which Player actor belongs to which local player. Set on the calc thread,
// read by the wraps on whichever game thread runs the Player.
class Players {
public:
    // player 1 to kMostExtra; actor 0 clears it.
    void Set(int player, std::uint64_t actor) {
        if (player >= 1 && player <= kMostExtra) actors_[Index(player)].store(actor, std::memory_order_relaxed);
    }
    void Clear() {
        for (auto& a : actors_) a.store(0, std::memory_order_relaxed);
    }
    // 0: not an extra local player's Player (the first Link, a remote player's).
    int Of(std::uint64_t actor) const {
        if (actor == 0) return 0;
        for (int k = 1; k <= kMostExtra; k++)
            if (actors_[Index(k)].load(std::memory_order_relaxed) == actor) return k;
        return 0;
    }
    bool Any() const {
        for (const auto& a : actors_)
            if (a.load(std::memory_order_relaxed) != 0) return true;
        return false;
    }

private:
    static std::size_t Index(int player) { return static_cast<std::size_t>(player - 1); }
    std::array<std::atomic<std::uint64_t>, kMostExtra> actors_{};
};

// One game thread's state between the wraps: the Player's read (16 or 17)
// is open from its phase 0 to its phase 1, and the getController calls it
// makes in between (19) are that Player's.
class Thread {
public:
    // Wrap 16 or 17. `player`: Players::Of the reading Player's actor (phase 0 only).
    // The two reads are not nested, and the wraps always give a phase 1.
    void Read(int phase, int player) { reading_ = phase == 0 ? player : 0; }
    // Wrap 19, phase 0: the index to ask for instead (nullopt: leave it).
    // `has`: whether the manager has that controller (its slot is not null).
    std::optional<std::uint32_t> ControllerGet(std::uint32_t index, const std::function<bool(std::uint32_t)>& has) const {
        if (reading_ == 0 || index != 0) return std::nullopt;
        const std::uint32_t want = ControllerFor(reading_);
        if (!has(want)) return std::nullopt;
        return want;
    }
    int Reading() const { return reading_; }

private:
    int reading_ = 0;
};

}  // namespace CoopLocalInput
