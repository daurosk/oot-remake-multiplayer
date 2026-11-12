// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_draw.h"

#include <algorithm>
#include <cmath>

namespace CoopDraw {
namespace {

constexpr float kPad = 8.0f;
constexpr float kMargin = 16.0f;
constexpr float kChatWidth = 520.0f;

int Alpha(float a) {
    return std::clamp(static_cast<int>(a * 255.0f), 0, 255);
}

// Text with a dark outline, so it reads on sky and on grass alike.
void Outlined(const HosCoopDraw& d, float x, float y, float px, std::uint32_t fill, int a, const char* s) {
    const float o = std::max(1.0f, px / 12.0f);
    const std::uint32_t dark = Rgba(0, 0, 0, a * 4 / 5);
    static constexpr float kAround[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (const auto& k : kAround) {
        d.text(d.user, x + k[0] * o, y + k[1] * o, px, dark, s);
    }
    d.text(d.user, x, y, px, fill, s);
}

// `text` cut to `room` pixels, with "..." when cut (never inside a UTF-8 character).
std::string Elided(const HosCoopDraw& d, float px, const std::string& text, float room) {
    if (d.text_width(d.user, px, text.c_str()) <= room) {
        return text;
    }
    std::string s = text;
    while (!s.empty()) {
        s.pop_back();
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) {
            s.pop_back();
        }
        if (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0xC0) {
            s.pop_back();
        }
        if (d.text_width(d.user, px, (s + "...").c_str()) <= room) {
            return s + "...";
        }
    }
    return "...";
}

void DrawMarkers(const HosCoopDraw& d, const CoopHook::Markers& m) {
    if (m.width <= 0 || m.height <= 0) {
        return;
    }
    // The game's picture keeps its aspect inside the window.
    const float s = std::min(d.width / m.width, d.height / m.height);
    const float ox = (d.width - m.width * s) / 2;
    const float oy = (d.height - m.height * s) / 2;
    const float px = std::clamp(m.height * s * 0.035f, 14.0f, 32.0f);  // 3.5% of the picture, 14 to 32 px
    for (const auto& k : m.items) {
        const int a = Alpha(k.alpha);
        if (a == 0) {
            continue;
        }
        const std::uint32_t fill = k.self ? Rgba(255, 210, 60, a) : Rgba(90, 200, 255, a);
        const std::uint32_t dark = Rgba(0, 0, 0, a * 3 / 4);
        const float cx = ox + k.x * s, cy = oy + k.y * s;
        if (k.edge) {
            // A triangle at the edge pointing at the player.
            const float r = std::max(6.0f, k.size * s / 2);
            const float ca = std::cos(k.arrow_angle), sa = std::sin(k.arrow_angle);
            d.triangle(d.user, cx + ca * r, cy + sa * r,                                        //
                       cx - ca * r * 0.6f - sa * r * 0.8f, cy - sa * r * 0.6f + ca * r * 0.8f,  //
                       cx - ca * r * 0.6f + sa * r * 0.8f, cy - sa * r * 0.6f - ca * r * 0.8f, fill);
        } else if (!k.tag_head) {
            // No head height known: a dot under the tag.
            const float r = std::max(3.0f, k.size * s / 2);
            d.circle(d.user, cx, cy, r + 1.5f, dark, 0.0f);
            d.circle(d.user, cx, cy, r, fill, 0.0f);
        }
        if (k.tag.empty()) {
            continue;
        }
        const float tw = d.text_width(d.user, px, k.tag.c_str());
        const float x = std::clamp(ox + k.tag_x * s - tw / 2, 2.0f, std::max(2.0f, d.width - tw - 2.0f));
        const float y = k.tag_below ? oy + k.tag_y * s : oy + k.tag_y * s - px;
        Outlined(d, x, y, px, k.self ? Rgba(255, 225, 120, a) : Rgba(255, 255, 255, a), a, k.tag.c_str());
    }
}

void DrawChat(const HosCoopDraw& d, const std::vector<ChatShown>& chat) {
    if (chat.empty()) {
        return;
    }
    const float px = std::clamp(d.height * 0.025f, 14.0f, 32.0f);
    const float room = std::min(kChatWidth * px / 14.0f, d.width - 2 * kMargin) - 2 * kPad;
    const float line = px + 8.0f;
    float y = d.height - kMargin - line * static_cast<float>(chat.size());
    for (const auto& l : chat) {
        const int a = Alpha(l.alpha);
        const std::string text = Elided(d, px, l.text, room);
        const float tw = d.text_width(d.user, px, text.c_str());
        d.rect(d.user, kMargin - 4, y - 2, kMargin + tw + 4, y + px + 2, Rgba(0, 0, 0, a * 150 / 255), 4.0f);
        d.text(d.user, kMargin, y, px, l.notice ? Rgba(255, 220, 120, a) : Rgba(255, 255, 255, a), text.c_str());
        y += line;
    }
}

void DrawPanel(const HosCoopDraw& d, const std::vector<CoopHook::OverlayLine>& lines) {
    if (lines.empty()) {
        return;
    }
    const float px = std::clamp(d.height * 0.018f, 13.0f, 24.0f);
    const float line = px + 4.0f;
    float width = 0;
    for (const auto& l : lines) {
        width = std::max(width, d.text_width(d.user, px, l.text.c_str()));
    }
    width = std::min(width, d.width - 2 * kMargin - 2 * kPad);
    d.rect(d.user, kMargin, kMargin, kMargin + width + 2 * kPad, kMargin + line * lines.size() + 2 * kPad - 4,
           Rgba(0, 0, 0, 150), 6.0f);
    float y = kMargin + kPad;
    for (const auto& l : lines) {
        const std::string text = Elided(d, px, l.text, width);
        d.text(d.user, kMargin + kPad, y, px, l.dim ? Rgba(170, 170, 170, 255) : Rgba(255, 255, 255, 255),
               text.c_str());
        y += line;
    }
}

void DrawPlayers(const HosCoopDraw& d, const std::vector<CoopHook::PlayerRow>& rows) {
    const auto cells = PlayerCells(rows);
    const float px = std::clamp(d.height * 0.022f, 14.0f, 28.0f);
    const float line = px + 6.0f;
    const float gap = 18.0f * px / 14.0f;
    std::vector<float> widths(cells[0].size(), 0.0f);
    for (const auto& r : cells) {
        for (std::size_t c = 0; c < r.size(); ++c) {
            widths[c] = std::max(widths[c], d.text_width(d.user, px, r[c].c_str()));
        }
    }
    float width = 4 * kPad - gap;
    for (const float w : widths) {
        width += w + gap;
    }
    const float height = static_cast<float>(cells.size()) * line + 4 * kPad;
    const float left = std::max(0.0f, (d.width - width) / 2);
    const float top = d.height / 6;
    d.rect(d.user, left, top, left + width, top + height, Rgba(0, 0, 0, 170), 8.0f);
    float y = top + 2 * kPad;
    for (std::size_t r = 0; r < cells.size(); ++r) {
        const bool dim = r > 0 && rows[r - 1].state != "connected";
        const std::uint32_t colour = r == 0 ? Rgba(255, 220, 120, 255)
                                     : dim  ? Rgba(170, 170, 170, 255)
                                            : Rgba(255, 255, 255, 255);
        float x = left + 2 * kPad;
        for (std::size_t c = 0; c < cells[r].size(); ++c) {
            d.text(d.user, x, y, px, colour, cells[r][c].c_str());
            x += widths[c] + gap;
        }
        y += line;
    }
}

}  // namespace

std::vector<std::vector<std::string>> PlayerCells(const std::vector<CoopHook::PlayerRow>& rows) {
    std::vector<std::vector<std::string>> cells{{"#", "Player", "State", "Ping", "Scene"}};
    for (const auto& row : rows) {
        std::string ms = "-";
        if (row.ms) {
            ms = (row.self ? "" : "~") + std::to_string(*row.ms) + " ms";
        }
        cells.push_back({row.number ? "P" + std::to_string(row.number) : std::string("-"),
                         row.name + (row.self ? " (you)" : ""), row.state, ms, row.scene.value_or("?")});
    }
    return cells;
}

std::vector<ChatShown> ShownChat(const std::vector<CoopHook::ChatLine>& lines) {
    std::vector<ChatShown> out;
    for (const auto& l : lines) {
        float alpha = 1.0f;
        if (l.age_ms >= kChatShowMs) {
            alpha = 1.0f - static_cast<float>(l.age_ms - kChatShowMs) / static_cast<float>(kChatFadeMs);
        }
        if (alpha > 0.0f) {
            out.push_back({l.text, l.notice, alpha});
        }
    }
    return out;
}

bool Wanted(const Scene& s) {
    return !s.chat.empty() || (s.markers && !s.markers->items.empty()) || s.players.has_value() ||
           (s.panel && !s.panel->empty());
}

void Draw(const HosCoopDraw& d, const Scene& s) {
    if (d.width <= 0 || d.height <= 0 || !d.rect || !d.circle || !d.triangle || !d.text || !d.text_width) {
        return;
    }
    if (s.markers) {
        DrawMarkers(d, *s.markers);
    }
    if (s.panel) {
        DrawPanel(d, *s.panel);
    }
    DrawChat(d, s.chat);
    if (s.players) {
        DrawPlayers(d, *s.players);
    }
}

}  // namespace CoopDraw
