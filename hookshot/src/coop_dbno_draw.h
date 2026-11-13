// SPDX-License-Identifier: GPL-3.0-or-later
//
// Down but not out on the screen (coop_dbno_game.h's View): a ring of dots
// that fills as the revive is held, the bleed-out clock in it, and a line
// under it. The downed player and the reviver see the same ring.
#pragma once

#include <cstddef>
#include <string>

#include "coop_dbno_game.h"
#include "hos_coop_draw.h"

namespace CoopDbnoDraw {

inline constexpr std::size_t kDots = 24;

// How many of the ring's `dots` are lit at `progress` (0 to 1).
std::size_t Lit(float progress, std::size_t dots = kDots);

// The line under the ring: "You are down", "Saria is reviving you",
// "Hold A to revive Saria", "Reviving Saria".
std::string Caption(const CoopDbno::View& view);

// Draws it at the lower middle of the picture.
void Draw(const HosCoopDraw& d, const CoopDbno::View& view);

}  // namespace CoopDbnoDraw
