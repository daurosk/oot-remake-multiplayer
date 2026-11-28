// SPDX-License-Identifier: GPL-3.0-or-later
//
// Down but not out on the screen (coop_dbno_game.h's View): one line of words at the lower middle of the picture,
// the bleed-out clock and the revive's progress in it ("You are down  0:32", "Reviving Saria  50%"). The downed
// player and the reviver see the same line. No drawn shapes (the owner, 2026-11-21: no handmade drawn assets; before
// 0.11.12 a ring of dots and a black box, ui-audit-01112 B9): only text in the game's font, with its soft outline.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "coop_dbno_game.h"
#include "hos_coop_draw.h"

namespace CoopDbnoDraw {

// Who and what, without the numbers: "You are down", "Saria is reviving you", "Hold B to revive Saria",
// "Reviving Saria", "Watching Saria  A: next player".
std::string Caption(const CoopDbno::View& view);

// The revive's progress as a share: "50%" (0 to 100, rounded down), "" before it starts.
std::string Progress(float progress);

// The whole line: the caption, then the clock while a clock runs (downed, reviving), then the progress while a revive
// is held. Spectating: the caption only.
std::string Line(const CoopDbno::View& view);

// The line for the game's banner (SimpleInfo_00, one line at most `width` wide, CoopNativeUi::Width: the game's font,
// kBannerWidth): always fits, and the live part (the clock, the revive's share, "A: next player") is never cut. Too long (a
// 16-letter name, a wide-letter name, a long translation): "(out of sight)" goes first, then the name is cut with
// "...", then the words before the live part.
std::string BannerLine(const CoopDbno::View& view, std::size_t width);

// Whether the game's banner is held for `view` now (`now_ms` any steady clock). Downed and reviving: always (short:
// the bleed-out clock). Spectating can last minutes, and notices wait while the banner is held, so it is held only
// for kSpectateHoldMs after spectating starts or the watched player (or their out of sight) changes; then let go.
inline constexpr std::uint64_t kSpectateHoldMs = 4000;
class BannerHold {
public:
    bool Want(const CoopDbno::View& view, std::uint64_t now_ms);
    void Reset() { spectating_ = false; }

private:
    bool spectating_ = false;
    std::string name_;
    bool elsewhere_ = false;
    std::uint64_t since_ms_ = 0;
};

// The line's size: the game look's size, smaller until it fits in 90% of the picture's width (never under 12 px).
float LinePx(float width, float height, float measured_at_px, float measured_width);

// Draws it at the lower middle of the picture (text only).
void Draw(const HosCoopDraw& d, const CoopDbno::View& view);

}  // namespace CoopDbnoDraw
