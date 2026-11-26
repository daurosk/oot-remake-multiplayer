// SPDX-License-Identifier: GPL-3.0-or-later
// Whether a player's HUD block (top left: name, hearts, place) is drawn dim,
// held steady. The player list calls a player "quiet" once their newest
// sample is a second old (CoopHook::PlayerList). A game that runs slowly or
// unevenly (a second window on the same PC, a load, a hitch) sends its
// keep-alive samples about a second apart, so that state flips between
// "connected" and "quiet" from one frame to the next, and the block flashed
// between full and dim. Hold changes the look only once the new state has
// lasted kSteadyUs without a break; a flip back inside that time keeps the
// look as it was.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace CoopHudDim {

inline constexpr std::uint64_t kSteadyUs = 750'000;

class Hold {
public:
    // Player `number` (1..kSlots) is connected now (false: quiet, joining,
    // no samples); `now_us` a steady clock. Returns whether to draw it dim.
    // A player seen for the first time takes its state at once.
    bool Dim(std::uint8_t number, bool connected, std::uint64_t now_us) {
        if (number < 1 || number > kSlots) {
            return !connected;
        }
        Slot& s = slots_[number - 1];
        const bool dim = !connected;
        if (!s.seen) {
            s.seen = true;
            s.shown = dim;
            s.pending = false;
            return dim;
        }
        if (dim == s.shown) {
            s.pending = false;  // a flip that did not last
        } else if (!s.pending || s.pending_dim != dim) {
            s.pending = true;
            s.pending_us = now_us;
            s.pending_dim = dim;
        } else if (now_us - s.pending_us >= kSteadyUs) {
            s.shown = dim;
            s.pending = false;
        }
        return s.shown;
    }

    // Player `number` is gone (left, or no longer in the list): the next
    // block for that number starts fresh.
    void Forget(std::uint8_t number) {
        if (number >= 1 && number <= kSlots) {
            slots_[number - 1] = Slot{};
        }
    }

    static constexpr std::uint8_t kSlots = 16;  // coop::protocol::kMaxPlayers (protocol 11)

private:
    // Plain fields, all initialized (no std::optional: gcc 13 at -O2 calls its value maybe-uninitialized here).
    struct Slot {
        bool seen = false;    // a state was taken
        bool shown = false;   // the look drawn: dim
        bool pending = false; // the other state began at pending_us
        std::uint64_t pending_us = 0;
        bool pending_dim = false;
    };
    std::array<Slot, kSlots> slots_{};
};

}  // namespace CoopHudDim
