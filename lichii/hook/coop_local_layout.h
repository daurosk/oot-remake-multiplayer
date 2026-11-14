// SPDX-License-Identifier: GPL-3.0-or-later
//
// Split screen: where each local player's view goes in the picture, its
// shape and the size it is rendered at (docs/local-coop.md). Plain maths, no
// game or emulator code.
//
// - Two players share the picture side by side or stacked. Three or four
//   get a quarter each; with three, the third quarter sits in the middle of
//   the bottom row, so every view keeps the same shape.
// - The views cover the picture without a gap or an overlap: when a side has
//   an odd number of pixels the second column or row is the one pixel larger.
// - A view is rendered at its own size times the view scale (a percentage),
//   in whole multiples of 8 pixels as the game's render targets are, so two
//   views at 100% cost about the pixels of one full picture.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace CoopLocalLayout {

inline constexpr int kMostViews = 4;
inline constexpr int kLeastScale = 50;    // percent of the view's own size
inline constexpr int kMostScale = 100;
inline constexpr int kRenderStep = 8;     // render sizes are multiples of this
inline constexpr int kLeastRenderW = 160;
inline constexpr int kLeastRenderH = 88;

enum class Split : std::uint8_t { Side = 0, Stacked = 1 };

struct Rect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool operator==(const Rect&) const = default;
};

struct Size {
    int w = 0;
    int h = 0;
    bool operator==(const Size&) const = default;
};

// A rect as fractions of the picture (0 to 1 across and down), for a
// viewport given that way.
struct Fraction {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct Views {
    int count = 0;                       // 0 when the picture or the count is not usable
    std::array<Rect, kMostViews> rect{};
};

// 1 to kMostViews.
constexpr bool ValidCount(int players) { return players >= 1 && players <= kMostViews; }

constexpr int ClampScale(int percent) { return std::clamp(percent, kLeastScale, kMostScale); }

// The views of `players` local players in a picture of out_w x out_h pixels.
// `split` matters for two players only. Nothing (count 0) for a count outside
// 1 to 4 or a picture too small to give every view 2 x 2 pixels.
constexpr Views Place(int players, Split split, int out_w, int out_h) {
    Views v;
    if (!ValidCount(players) || out_w < 4 || out_h < 4) return v;
    const int left = out_w / 2, right = out_w - left;
    const int top = out_h / 2, bottom = out_h - top;
    v.count = players;
    switch (players) {
    case 1:
        v.rect[0] = {0, 0, out_w, out_h};
        break;
    case 2:
        if (split == Split::Side) {
            v.rect[0] = {0, 0, left, out_h};
            v.rect[1] = {left, 0, right, out_h};
        } else {
            v.rect[0] = {0, 0, out_w, top};
            v.rect[1] = {0, top, out_w, bottom};
        }
        break;
    case 3:
        v.rect[0] = {0, 0, left, top};
        v.rect[1] = {left, 0, right, top};
        v.rect[2] = {(out_w - left) / 2, top, left, bottom};
        break;
    default:
        v.rect[0] = {0, 0, left, top};
        v.rect[1] = {left, 0, right, top};
        v.rect[2] = {0, top, left, bottom};
        v.rect[3] = {left, top, right, bottom};
        break;
    }
    return v;
}

// Width over height; 0 for an empty rect.
constexpr float Ratio(const Rect& r) { return r.w > 0 && r.h > 0 ? static_cast<float>(r.w) / static_cast<float>(r.h) : 0.0f; }

constexpr Fraction Of(const Rect& r, int out_w, int out_h) {
    if (out_w <= 0 || out_h <= 0) return {};
    const float w = static_cast<float>(out_w), h = static_cast<float>(out_h);
    return {static_cast<float>(r.x) / w, static_cast<float>(r.y) / h, static_cast<float>(r.w) / w,
            static_cast<float>(r.h) / h};
}

// The size a view is rendered at: its own size times `percent` (clamped to
// 50..100), each side rounded to the nearest multiple of 8, never under
// 160 x 88 and never over the view's own size rounded up to a multiple of 8.
constexpr Size RenderSize(const Rect& view, int percent) {
    if (view.w <= 0 || view.h <= 0) return {};
    const int scale = ClampScale(percent);
    const auto side = [scale](int pixels, int least) {
        const int most = (pixels + kRenderStep - 1) / kRenderStep * kRenderStep;
        const int scaled = (pixels * scale / 100 + kRenderStep / 2) / kRenderStep * kRenderStep;
        return std::clamp(scaled, std::min(least, most), most);
    };
    return {side(view.w, kLeastRenderW), side(view.h, kLeastRenderH)};
}

// The pixels all views render together, for the performance numbers and the
// settings page ("2 views at 70%: 49% of one full picture").
constexpr long long RenderPixels(const Views& views, int percent) {
    long long sum = 0;
    for (int i = 0; i < views.count; i++) {
        const Size s = RenderSize(views.rect[static_cast<std::size_t>(i)], percent);
        sum += static_cast<long long>(s.w) * s.h;
    }
    return sum;
}

} // namespace CoopLocalLayout
