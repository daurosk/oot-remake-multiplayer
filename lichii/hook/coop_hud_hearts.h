// How the co-op HUD draws a player's hearts (docs/overlay.md, "The look"):
// one picture per heart, each with 0 to 4 quarters filled, from PLAYER_LIFE's
// quarter hearts. Header only, standard C++ only, so the tests build it.
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace CoopHudHearts {

// Hearts in a row before the next row starts, as the game draws its own.
inline constexpr int kPerRow = 10;
// The most hearts drawn (a bad value cannot fill the screen).
inline constexpr int kMostHearts = 40;

// Quarters filled (0 to 4) in each heart: max_life / 4 hearts, rounded up;
// life beyond the max is drawn as the max.
inline std::vector<int> Quarters(int life, int max_life) {
    const int hearts = std::clamp((max_life + 3) / 4, 0, kMostHearts);
    life = std::clamp(life, 0, std::max(0, max_life));
    std::vector<int> out(static_cast<std::size_t>(hearts));
    for (int i = 0; i < hearts; ++i) {
        out[static_cast<std::size_t>(i)] = std::clamp(life - 4 * i, 0, 4);
    }
    return out;
}

} // namespace CoopHudHearts
