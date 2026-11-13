// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_dbno_draw.h"

#include <algorithm>
#include <cmath>

#include "coop_draw.h"

namespace CoopDbnoDraw {

std::size_t Lit(float progress, std::size_t dots) {
    if (!(progress > 0.0f)) {
        return 0;
    }
    if (progress >= 1.0f) {
        return dots;
    }
    return std::min(dots, static_cast<std::size_t>(progress * static_cast<float>(dots)));
}

std::string Caption(const CoopDbno::View& view) {
    using Kind = CoopDbno::View::Kind;
    if (view.kind == Kind::downed) {
        return view.name.empty() ? "You are down" : view.name + " is reviving you";
    }
    return (view.holding ? "Reviving " : "Hold A to revive ") + view.name;
}

void Draw(const HosCoopDraw& d, const CoopDbno::View& view) {
    using CoopDraw::Rgba;
    const float radius = std::clamp(d.height * 0.045f, 22.0f, 60.0f);
    const float cx = d.width * 0.5f;
    const float cy = d.height * 0.68f;
    const float dot = std::max(2.5f, radius * 0.09f);
    const std::size_t lit = Lit(view.progress);
    const bool downed = view.kind == CoopDbno::View::Kind::downed;
    d.circle(d.user, cx, cy, radius + dot * 2.5f, Rgba(0, 0, 0, 140), 0.0f);
    for (std::size_t i = 0; i < kDots; ++i) {
        // From the top, clockwise.
        const float a = static_cast<float>(i) / static_cast<float>(kDots) * 6.2831853f - 1.5707963f;
        const std::uint32_t c = i < lit ? Rgba(120, 230, 120, 255) : Rgba(90, 90, 90, 220);
        d.circle(d.user, cx + std::cos(a) * radius, cy + std::sin(a) * radius, dot, c, 0.0f);
    }
    const std::string clock = CoopDbno::Clock(view.left_us);
    const float px = std::clamp(radius * 0.6f, 14.0f, 34.0f);
    const std::uint32_t clock_colour =
        downed && view.left_us < 10'000'000 ? Rgba(255, 110, 90, 255) : Rgba(255, 255, 255, 255);
    d.text(d.user, cx - d.text_width(d.user, px, clock.c_str()) * 0.5f, cy - px * 0.55f, px, clock_colour,
           clock.c_str());
    const std::string caption = Caption(view);
    const float cpx = std::clamp(d.height * 0.022f, 14.0f, 28.0f);
    const float w = d.text_width(d.user, cpx, caption.c_str());
    const float top = cy + radius + dot * 3.0f + 6.0f;
    d.rect(d.user, cx - w * 0.5f - 10.0f, top - 4.0f, cx + w * 0.5f + 10.0f, top + cpx + 6.0f, Rgba(0, 0, 0, 150),
           6.0f);
    d.text(d.user, cx - w * 0.5f, top, cpx, Rgba(255, 255, 255, 255), caption.c_str());
}

}  // namespace CoopDbnoDraw
