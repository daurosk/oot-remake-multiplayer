// Where the co-op HUD's player blocks go (docs/overlay.md, "The look"): below
// the game's own top-left HUD, as a fraction of the game's picture height, so
// it is the same at any window size and at 16:9 or ultrawide (the picture is
// 16:9, centred). Header only, standard C++ only, so the tests build it.
#pragma once

#include <algorithm>

namespace CoopHudLayout {

// The lowest the game's own top-left HUD reaches, as a fraction of the
// picture height: hearts (two rows at 20), the magic bar under them and the
// d-pad item row under that. Measured on test shots: one heart row and the
// d-pad row end at about 0.24; the second heart row and the magic bar add
// about 0.06.
inline constexpr double kGameHudBottom = 0.30;

// Where the first player block's top is: clearly below kGameHudBottom.
inline constexpr double kBlocksTop = 0.38;

// The blocks' top in an area of w x h pixels holding the game's 16:9
// picture, centred (letterboxed above and below, or pillarboxed at the sides).
inline double BlocksTop(double w, double h) {
    if (w <= 0 || h <= 0) {
        return 0.0;
    }
    const double pic_h = std::min(h, w * 9.0 / 16.0);
    return (h - pic_h) / 2.0 + kBlocksTop * pic_h;
}

} // namespace CoopHudLayout
