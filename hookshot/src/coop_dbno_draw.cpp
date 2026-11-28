// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_dbno_draw.h"

#include <algorithm>
#include <cmath>

#include "coop_draw.h"
#include "coop_i18n.h"
#include "coop_native_ui.h"

namespace CoopDbnoDraw {

std::string Caption(const CoopDbno::View& view) {
    using Kind = CoopDbno::View::Kind;
    // The table's words around the name (data): "You are down", "Saria is reviving you", "Hold A to revive Saria".
    if (view.kind == Kind::downed) {
        return view.name.empty() ? CoopI18n::Tr("You are down") : view.name + " " + CoopI18n::Tr("is reviving you");
    }
    if (view.kind == Kind::spectating) {
        return CoopI18n::Tr("Watching") + " " + view.name +
               (view.elsewhere ? " " + CoopI18n::Tr("(out of sight)") : std::string()) + "  " +
               CoopI18n::Tr("A: next player");
    }
    return CoopI18n::Tr(view.holding ? "Reviving" : "Hold A to revive") + " " + view.name;
}

std::string Progress(float progress) {
    if (!(progress > 0.0f)) {
        return {};
    }
    const int share = progress >= 1.0f ? 100 : static_cast<int>(progress * 100.0f);
    return std::to_string(share) + "%";
}

namespace {

// Row 330, downed with nobody reviving: "Hold A: give up", then the hold's share ("Giving up 50%"); "" otherwise.
std::string GiveUp(const CoopDbno::View& view) {
    if (view.kind != CoopDbno::View::Kind::downed || !view.name.empty() || !view.can_give_up) {
        return {};
    }
    const std::string p = Progress(view.give_up);
    return p.empty() ? CoopI18n::Tr("Hold A: give up") : CoopI18n::Tr("Giving up") + " " + p;
}

}  // namespace

std::string Line(const CoopDbno::View& view) {
    std::string line = Caption(view);
    if (view.kind == CoopDbno::View::Kind::spectating) {
        return line;
    }
    line += "  " + CoopDbno::Clock(view.left_us);
    const bool reviving = view.kind == CoopDbno::View::Kind::downed ? !view.name.empty() : view.holding;
    if (const std::string p = Progress(view.progress); reviving && !p.empty()) {
        line += "  " + p;
    }
    if (const std::string g = GiveUp(view); !g.empty()) {
        line += "  " + g;
    }
    return line;
}

namespace {

// The words before the live part, with `name` in place of the view's name and "(out of sight)" only with `sight`.
std::string Head(const CoopDbno::View& view, const std::string& name, bool sight) {
    CoopDbno::View v = view;
    v.name = name;
    v.elsewhere = view.elsewhere && sight;
    if (v.kind == CoopDbno::View::Kind::spectating) {
        return CoopI18n::Tr("Watching") + " " + v.name + (v.elsewhere ? " " + CoopI18n::Tr("(out of sight)") : std::string());
    }
    return Caption(v);
}

// The live part: the clock and the share, or "A: next player".
std::string Tail(const CoopDbno::View& view) {
    if (view.kind == CoopDbno::View::Kind::spectating) {
        return CoopI18n::Tr("A: next player");
    }
    std::string tail = CoopDbno::Clock(view.left_us);
    const bool reviving = view.kind == CoopDbno::View::Kind::downed ? !view.name.empty() : view.holding;
    if (const std::string p = Progress(view.progress); reviving && !p.empty()) {
        tail += "  " + p;
    }
    if (const std::string g = GiveUp(view); !g.empty()) {
        tail += "  " + g;
    }
    return tail;
}

// `s` without its last UTF-8 character.
void PopChar(std::string& s) {
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
        s.pop_back();
    }
    if (!s.empty()) {
        s.pop_back();
    }
}

}  // namespace

std::string BannerLine(const CoopDbno::View& view, std::size_t width) {
    const std::string tail = Tail(view);
    const auto fits = [&](const std::string& head) { return CoopNativeUi::Width(head + "  " + tail) <= width; };
    std::string head = Head(view, view.name, true);
    if (!fits(head)) {
        head = Head(view, view.name, false);
    }
    std::string name = view.name;
    while (!fits(head) && !name.empty()) {
        PopChar(name);
        head = Head(view, name.empty() ? std::string("...") : name + "...", false);
        if (name.empty()) break;
    }
    while (!fits(head) && !head.empty()) {   // (a translation too long by itself: its end goes, never the live part)
        PopChar(head);
    }
    if (head.empty()) {
        return tail;
    }
    return head + "  " + tail;
}

bool BannerHold::Want(const CoopDbno::View& view, std::uint64_t now_ms) {
    if (view.kind != CoopDbno::View::Kind::spectating) {
        spectating_ = false;
        return true;
    }
    if (!spectating_ || view.name != name_ || view.elsewhere != elsewhere_) {
        spectating_ = true;
        name_ = view.name;
        elsewhere_ = view.elsewhere;
        since_ms_ = now_ms;
    }
    return now_ms - since_ms_ < kSpectateHoldMs;
}

float LinePx(float width, float height, float measured_at_px, float measured_width) {
    const float px = std::clamp(height * 0.03f, 14.0f, 40.0f);
    if (!(measured_at_px > 0.0f) || !(measured_width > 0.0f)) {
        return px;
    }
    const float fit = 0.9f * width * measured_at_px / measured_width;   // text width scales with its size
    return std::max(12.0f, std::min(px, fit));
}

void Draw(const HosCoopDraw& d, const CoopDbno::View& view) {
    using CoopDraw::Rgba;
    const std::string line = Line(view);
    const float probe = 20.0f;
    const float px = LinePx(d.width, d.height, probe, d.text_width(d.user, probe, line.c_str()));
    const float w = d.text_width(d.user, px, line.c_str());
    const float x = d.width * 0.5f - w * 0.5f;
    const float y = d.height * (view.kind == CoopDbno::View::Kind::spectating ? 0.86f : 0.78f);
    // The clock's last ten seconds in the game's warning red; otherwise white. A soft dark outline, as the game's
    // own HUD text has, so it reads on sky and on grass (no box).
    const bool late = view.kind == CoopDbno::View::Kind::downed && view.left_us < 10'000'000;
    const std::uint32_t fill = late ? Rgba(255, 110, 90, 255) : Rgba(255, 255, 255, 255);
    const float o = std::max(1.0f, px / 12.0f);
    static constexpr float kAround[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (const auto& k : kAround) {
        d.text(d.user, x + k[0] * o, y + k[1] * o, px, Rgba(0, 0, 0, 200), line.c_str());
    }
    d.text(d.user, x, y, px, fill, line.c_str());
}

}  // namespace CoopDbnoDraw
