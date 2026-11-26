// SPDX-License-Identifier: GPL-3.0-or-later
// One cursor model for every list of co-op's own on the game's menu pages (menu-polish-01113, the owner: "Our custom
// menus are pretty buggy ... The quality should be as good as in game menus"). Before it, each list (the - page's
// rows and list, the + page's Find a game cells, Settings, Settings and look, the typing keys) worked out its own
// presses, ends and sounds: holding a direction moved one step only (the typing keys alone repeated), and a list that
// went round at its ends did so again and again while held.
//
// The rules, the game's own lists':
// - A press moves at once. Held, the same direction moves again after kRepeatDelayMs, then every kRepeatEveryMs
//   (the typing keys' timing since 0.11.5, which matches the game's Options list when held).
// - Only a fresh press may go round from the last line to the first (where a list goes round at all); a held
//   direction stops at the end (Presses tells the two apart: `repeats`).
// - Lines that cannot be picked (headings, notes, blank lines) are stepped over; with none in that direction the
//   cursor stays (Step).
// - One sound each: kSoundMove when the cursor really moved, kSoundDecide on a choice, kSoundBack on B / back.
//   Sounds are the game's own, played by name on the screen's xlink user (coop_pause_tab.h kScreenSound).
//
// Logic only (no game calls): tests/hookshot/menu_nav_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace CoopMenuNav {

inline constexpr std::uint64_t kRepeatDelayMs = 400;   // held this long: the first repeat
inline constexpr std::uint64_t kRepeatEveryMs = 90;    // then one step this often

// The game's menu sounds by name (heard in runs, 2026-11-19: "button_on" at a cursor step, "button_decide" at a
// choice; the strip's "mc_TabToRight" / "mc_TabToLeft"). Back is "mc_ReturnFromDetail", the game's own back from a
// detail (docs/game-ui.md "Sounds"): the "button_cancel" our pages played since 0.11.9 is in no table of the game's
// (the sound link file SLink2/slink2.Product.111.bslnk.zs, read 2026-11-24: button_on, button_decide,
// button_decide_Cancel, mc_OpenDetail, mc_ReturnFromDetail, SE_Sys_Cancel00; no button_cancel), so B was silent.
inline constexpr const char* kSoundMove = "button_on";
inline constexpr const char* kSoundDecide = "button_decide";
inline constexpr const char* kSoundBack = "mc_ReturnFromDetail";
inline constexpr const char* kSoundTabRight = "mc_TabToRight";
inline constexpr const char* kSoundTabLeft = "mc_TabToLeft";

// Presses from the buttons held at each pad read: the buttons just pressed, plus the held `repeat_bits` again and
// again (one at a time, as the game's lists scroll). `repeats` (optional) gets the bits that are repeats only, not
// fresh presses: a list that goes round at its ends does so on a fresh press only.
class Repeat {
public:
    std::uint64_t Presses(std::uint64_t held, std::uint64_t now_ms, std::uint64_t repeat_bits,
                          std::uint64_t* repeats = nullptr) {
        const std::uint64_t fresh = held & ~last_;
        last_ = held;
        const std::uint64_t dirs = held & repeat_bits;
        std::uint64_t again = 0;
        if ((fresh & repeat_bits) != 0) {   // (a new direction pressed: its own delay from now)
            dirs_ = dirs, since_ = now_ms, last_step_ = now_ms, armed_ = true;
        } else if (dirs != dirs_) {   // (one of two let go: the rest waits for a press again)
            dirs_ = dirs, armed_ = false;
        } else if (armed_ && dirs != 0 && now_ms - since_ >= kRepeatDelayMs && now_ms - last_step_ >= kRepeatEveryMs) {
            again = dirs;
            last_step_ = now_ms;
        }
        if (repeats != nullptr) *repeats = again;
        return fresh | again;
    }
    // Everything held now counts as already pressed (a page just opened, or the pad given to another list): no press
    // and no repeat until it is let go and pressed again.
    void Forget(std::uint64_t held) {
        last_ = held;
        dirs_ = held, armed_ = false;
    }

private:
    std::uint64_t last_ = 0, dirs_ = 0, since_ = 0, last_step_ = 0;
    bool armed_ = false;   // a direction pressed while on this list: its hold repeats
};

// The next line from `at` that `pickable(i)` allows, going `dir` (-1 or +1) among `count` lines; `at` when there is
// none that way. `wrap`: past the end, on round from the other end (only for a fresh press: see Presses).
template <typename Pickable>
int Step(int at, int dir, int count, bool wrap, Pickable pickable) {
    if (count <= 0 || dir == 0) return at;
    dir = dir < 0 ? -1 : 1;
    for (int k = 1; k <= count; ++k) {
        int i = at + dir * k;
        if (i < 0 || i >= count) {
            if (!wrap) return at;
            i = ((i % count) + count) % count;
        }
        if (i == at) return at;
        if (pickable(i)) return i;
    }
    return at;
}

// Whether a step from `at` going `dir` would move without going round (a held direction's repeat may be sent).
template <typename Pickable>
bool CanStep(int at, int dir, int count, Pickable pickable) {
    return Step(at, dir, count, false, pickable) != at;
}

// The typing keys' cursor (the - page's and the + page's on-screen keys: `keys` keys up and down, the Letter key
// `letter_key`'s letter Left and Right among `letters`): `dy` / `dx` one step (-1, 0, 1). A fresh press goes round the
// keys, a held one (`dy_held`) stops at the ends, as every list; the letter wheel goes round held too (a value turned,
// as the game's own number wheels). True when something moved (its sound then).
inline bool TypingKeysStep(int dy, int dx, bool dy_held, int keys, int letter_key, std::size_t letters, int* key,
                           std::size_t* letter) {
    bool moved = false;
    if (dy != 0 && keys > 0) {
        const int to = *key + (dy < 0 ? -1 : 1);
        if (to >= 0 && to < keys) {
            *key = to, moved = true;
        } else if (!dy_held && keys > 1) {
            *key = (to % keys + keys) % keys, moved = true;
        }
    }
    if (dx != 0 && *key == letter_key && letters > 0) {
        *letter = (*letter % letters + (dx < 0 ? letters - 1 : 1)) % letters;
        moved = true;
    }
    return moved;
}

// What a list owned by someone else (the Mods tab's) last showed: which lines can be picked and the selected one, as
// read at the page's own refresh (its view is built then, never at each pad read: review-01112-h). `sent` counts the
// moves given to that list; a reading is only trusted while no move was given after it (Fill takes the count read
// BEFORE the view was built, so a refresh that raced a move is stale, never fresh). A held direction repeats only on a
// trusted reading; a stale one waits for the next refresh (asked for at each move).
struct ListReading {
    std::vector<bool> pickable;
    int selected = -1;
    std::uint64_t sent = ~std::uint64_t{0};   // (none yet: never trusted)

    void Fill(std::uint64_t sent_before_view, std::vector<bool> lines, int picked) {
        pickable = std::move(lines), selected = picked, sent = sent_before_view;
    }
    bool Trusted(std::uint64_t sent_now) const {
        return sent == sent_now && selected >= 0 && selected < static_cast<int>(pickable.size());
    }
    // A held Up (dir -1) or Down (+1) may go on: trusted, and a pickable line that way without going round.
    bool CanRepeat(int dir, std::uint64_t sent_now) const {
        if (!Trusted(sent_now)) return false;
        const auto& p = pickable;
        return CanStep(selected, dir, static_cast<int>(p.size()), [&p](int i) { return p[static_cast<std::size_t>(i)]; });
    }
};

// A bit mask of pickable lines (bit i: line i), as the - page keeps its rows and lines.
inline int StepMask(std::uint32_t mask, int at, int dir, bool wrap = false) {
    return Step(at, dir, 32, wrap, [mask](int i) { return (mask & (1u << i)) != 0; });
}

}  // namespace CoopMenuNav
