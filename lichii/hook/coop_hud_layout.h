// Where the co-op HUD's player blocks go (docs/overlay.md, "The look"): below
// the game's own top-left HUD, as a fraction of the game's picture height, so
// it is the same at any window size and at 16:9 or ultrawide (the picture is
// 16:9, centred). Header only, standard C++ only, so the tests build it.
//
// Row 141: the game's HUD is not one height. Its hearts (one or two rows),
// the magic bar and the d-pad item slots under them move down as the game
// lays them out (PlayerStatus_00's N_HeartPos_00 and N_MpGauge_00 move with
// the heart rows and the magic bar), so the blocks start below where the
// game's own panes are this frame (GameHudBottom, from their global matrices:
// hookshot/src/coop_map_page.*), and at the fixed kBlocksTop only when that
// is not known (an exe without patch 0018's screens, the emulator's window).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace CoopHudLayout {

// The lowest the game's own top-left HUD reaches with one heart row and no
// magic bar, as a fraction of the picture height (test shots: about 0.24 to
// 0.30 with the second heart row and the magic bar).
inline constexpr double kGameHudBottom = 0.30;

// Where the first player block's top is when the game's HUD is not known.
inline constexpr double kBlocksTop = 0.38;

// Between the game's HUD and the first block, as a fraction of the picture
// height (about 14 pixels of a 720-line picture).
inline constexpr double kGap = 0.02;

// A HUD bottom outside these is not a reading of the game's HUD (a pane
// matrix of another layout, a pane being freed): the fixed top is used.
inline constexpr double kLowestHudBottom = 0.08, kHighestHudBottom = 0.62;

// The game's UI layout: 3840 x 2160 units, y up from its middle; the
// picture shows all of it (it is 16:9 too), so a fraction of the layout's
// height is the same fraction of the picture's.
inline constexpr double kLayoutW = 3840.0, kLayoutH = 2160.0;

// One piece of the game's HUD: a pane's global matrix (3 x 4 by column: the
// scale at [0] and [4], the translation at [9] and [10], in the layout's
// units, y up from the middle), its alpha with its parents', and the piece's
// box in that pane's own units, y up from the pane's origin.
struct HudPiece {
    float m[12] = {};
    float alpha = 1.0f;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// A piece whose box is the pane's own w x h, centred on its origin.
inline HudPiece Centred(const float m[12], float alpha, float w, float h) {
    HudPiece p;
    std::copy(m, m + 12, p.m);
    p.alpha = alpha;
    p.x0 = -w / 2, p.x1 = w / 2, p.y0 = -h / 2, p.y1 = h / 2;
    return p;
}

// The lowest point the shown pieces reach, as a fraction of the layout's
// height down from its top; none when no piece is shown (all faded out: a
// cutscene, a menu over the HUD) or none can be read.
inline std::optional<double> HudBottom(const std::vector<HudPiece>& pieces) {
    std::optional<double> lowest;
    for (const auto& p : pieces) {
        const double sy = p.m[4], ty = p.m[10];
        if (!(p.alpha > 0.01f) || !std::isfinite(p.alpha) || !(sy > 1e-3) || !std::isfinite(sy) ||
            !std::isfinite(ty) || !(p.y1 > p.y0)) {
            continue;
        }
        const double bottom = (kLayoutH / 2 - (ty + sy * p.y0)) / kLayoutH;   // y up from the middle -> down from the top
        if (!std::isfinite(bottom)) {
            continue;
        }
        lowest = lowest ? (std::max)(*lowest, bottom) : bottom;
    }
    return lowest;
}

// The first block's top as a fraction of the picture height: kGap below the
// game's HUD bottom, or kBlocksTop when that is not known or not believable.
inline double TopFraction(std::optional<double> hud_bottom) {
    if (!hud_bottom || !std::isfinite(*hud_bottom) || *hud_bottom < kLowestHudBottom ||
        *hud_bottom > kHighestHudBottom) {
        return kBlocksTop;
    }
    return *hud_bottom + kGap;
}

// The HUD bottom to place by: the one read now, or while the game's HUD is
// not readable (faded out in a cutscene, its screen stopped behind a menu or
// a load: no reading) the last one read, so the blocks stay where they were
// instead of jumping to the fixed top and back; none before the first one.
class Hold {
public:
    std::optional<double> Update(std::optional<double> now) {
        if (now) {
            last_ = now;
        }
        return last_;
    }

private:
    std::optional<double> last_;
};

// The blocks' top in an area of w x h pixels holding the game's 16:9
// picture, centred (letterboxed above and below, or pillarboxed at the sides).
inline double BlocksTop(double w, double h, std::optional<double> hud_bottom = std::nullopt) {
    if (w <= 0 || h <= 0) {
        return 0.0;
    }
    const double pic_h = (std::min)(h, w * 9.0 / 16.0);   // ((std::min): windows.h's macro)
    return (h - pic_h) / 2.0 + TopFraction(hud_bottom) * pic_h;
}

// How many blocks (each `heights[i]` high, gap included) are drawn from `top`
// without passing `floor` (the chat's top): all of them when they fit, else
// as many as fit with room left for a `more` line (how many are left out)
// under them. Never below 0.
inline std::size_t BlocksThatFit(double top, const std::vector<double>& heights, double floor, double more) {
    double y = top;
    for (const double h : heights) {
        y += h;
    }
    if (y <= floor) {
        return heights.size();
    }
    std::size_t n = 0;
    y = top;
    while (n < heights.size() && y + heights[n] + more <= floor) {
        y += heights[n++];
    }
    return n;
}

} // namespace CoopHudLayout
