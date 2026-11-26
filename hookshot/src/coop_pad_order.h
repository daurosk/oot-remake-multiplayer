// SPDX-License-Identifier: GPL-3.0-or-later
// The order hos_coop_pad hands player 1's buttons on in (coop_hookshot.cpp), header only so the tests run it with
// stand-ins (tests/hookshot/pad_order_test.cpp):
//   1. our pause menu page (coop_pause_tab.h): it takes the pad while it shows, so it is first and nothing before it
//      can block it;
//   2. the code mods (coop_mod_api.h version 2): they see what our page passed on (NotePad), then their holds come
//      off; COOP_MOD_HOLD_ALL zeroes the buttons and the sticks. A throw in the mods' part holds nothing;
//   3. the title's watch (coop_title.h: its A and Minus edges) and down but not out (coop_dbno_game.h) always run,
//      on the buttons the game gets, so their edges never go stale under a hold.
#pragma once

#include <cstdint>

namespace CoopPadOrder {

inline constexpr std::uint64_t kHoldAll = ~std::uint64_t{0};   // (COOP_MOD_HOLD_ALL)

// page(b, stick) -> b, note(b), held() -> bits, title(b, stick) -> b, dbno(b, stick) -> b.
template <class Page, class Note, class Held, class Title, class Dbno>
std::uint64_t Run(std::uint64_t buttons, std::int32_t stick[4], Page&& page, Note&& note, Held&& held, Title&& title,
                  Dbno&& dbno) {
    buttons = page(buttons, stick);
    std::uint64_t hold = 0;
    try {
        note(buttons);
        hold = held();
    } catch (...) {
        hold = 0;   // (the mods' part failed: the game gets the pad as our page passed it)
    }
    if (hold == kHoldAll) {
        buttons = 0;
        stick[0] = stick[1] = stick[2] = stick[3] = 0;
    } else {
        buttons &= ~hold;
    }
    buttons = title(buttons, stick);
    return dbno(buttons, stick);
}

}  // namespace CoopPadOrder
