// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_draw.h"

#include <algorithm>
#include <cmath>
#include <string_view>

#include "coop_dbno_draw.h"
#include "coop_hud_hearts.h"
#include "coop_hud_layout.h"

namespace CoopDraw {
namespace {

constexpr float kPad = 8.0f;
constexpr float kMargin = 16.0f;
constexpr float kChatWidth = 520.0f;

// The co-op window's sizes for the game look, in the game's 720-line picture.
constexpr float kGameHeight = 720.0f;
constexpr float kHudLeft = 40.0f;
constexpr float kHudNamePx = 25.0f;
constexpr float kHudPlacePx = 19.0f;
constexpr float kHudGap = 16.0f;
// The heart pictures are 96 x 96 with the heart about half as wide in the
// middle: drawn kHeartPx big, kHeartStep apart (the co-op window's HudLayer).
constexpr float kHeartPx = 38.0f;
constexpr float kHeartStep = 0.55f;
constexpr float kChatGamePx = 23.0f;
constexpr float kListGamePx = 24.0f;

int Alpha(float a) {
    return std::clamp(static_cast<int>(a * 255.0f), 0, 255);
}

std::uint32_t WithAlpha(std::uint32_t rgba, int a) {
    return (rgba & 0x00FFFFFFu) | static_cast<std::uint32_t>(std::clamp(a, 0, 255)) << 24;
}

// The game's 16:9 picture inside the window, centred, and its unit (one of
// its 720 lines), as the co-op window's Picture.
struct Pic {
    float left = 0, width = 0, height = 0, u = 1;
};
Pic Picture(const HosCoopDraw& d) {
    const float s = std::min(d.width / 16.0f, d.height / 9.0f);
    return {(d.width - 16.0f * s) / 2, 16.0f * s, 9.0f * s, 9.0f * s / kGameHeight};
}

// The font text and text_width use from here on (0: Hookshot's own). Only
// the game look changes it.
void Font(const HosCoopDraw& d, int id) {
    if (d.use_font) {
        d.use_font(d.user, id);
    }
}

// Text as the game draws it on its HUD (the co-op window's DrawGameText):
// the fill over a soft dark rim and a drop shadow a little down and right,
// no box and no hard outline.
void GameText(const HosCoopDraw& d, float x, float y, float px, std::uint32_t fill, const char* s) {
    const int a = static_cast<int>(fill >> 24);
    const float o = std::max(1.0f, px / 16.0f);
    const std::uint32_t rim = Rgba(0, 0, 0, 70 * a / 255);
    static constexpr float kAround[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (const auto& k : kAround) {
        d.text(d.user, x + k[0] * o * 0.8f, y + k[1] * o * 0.8f, px, rim, s);
    }
    d.text(d.user, x + o, y + o * 1.3f, px, Rgba(0, 0, 0, 150 * a / 255), s);
    d.text(d.user, x, y, px, fill, s);
}

// A player's name colour in the game look; white for anyone not known.
std::uint32_t ColourOf(const Scene& s, std::string_view name) {
    if (const auto it = s.colours.find(name); it != s.colours.end()) {
        return it->second;
    }
    return Rgba(255, 255, 255, 255);
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

// `look`: the game look's fonts, or nullptr for the plain look.
void DrawMarkers(const HosCoopDraw& d, const CoopHook::Markers& m, const Scene& scene, const Look* look) {
    if (m.width <= 0 || m.height <= 0) {
        return;
    }
    // The game's picture keeps its aspect inside the window.
    const float s = std::min(d.width / m.width, d.height / m.height);
    const float ox = (d.width - m.width * s) / 2;
    const float oy = (d.height - m.height * s) / 2;
    float px = std::clamp(m.height * s * 0.035f, 14.0f, 32.0f);  // 3.5% of the picture, 14 to 32 px
    if (look) {
        Font(d, look->medium);
        px *= 1.1f;  // the game's font is a little small for its size
    }
    for (const auto& k : m.items) {
        const int a = Alpha(k.alpha);
        if (a == 0) {
            continue;
        }
        std::uint32_t fill = k.self ? Rgba(255, 210, 60, a) : Rgba(90, 200, 255, a);
        if (look && !k.self) {
            fill = WithAlpha(ColourOf(scene, k.tag), a);
        }
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
        if (look) {
            // The game's HUD style: the player's colour over a soft shadow.
            GameText(d, x, y, px, fill, k.tag.c_str());
            continue;
        }
        Outlined(d, x, y, px, k.self ? Rgba(255, 225, 120, a) : Rgba(255, 255, 255, a), a, k.tag.c_str());
    }
}

// The end of `text` that fits `room` pixels, "..." in front when cut.
std::string Tail(const HosCoopDraw& d, float px, const std::string& text, float room) {
    if (d.text_width(d.user, px, text.c_str()) <= room) {
        return text;
    }
    std::size_t at = 0;
    while (at < text.size()) {
        ++at;
        while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80) {
            ++at;
        }
        const std::string t = "..." + text.substr(at);
        if (d.text_width(d.user, px, t.c_str()) <= room) {
            return t;
        }
    }
    return "...";
}

float ChatPx(const HosCoopDraw& d) {
    return std::clamp(d.height * 0.025f, 14.0f, 32.0f);
}

// Where the chat lines and the box sit: their left edge, width and text size.
struct ChatPlace {
    float left = kMargin;
    float width = kChatWidth;
    float px = 14.0f;
};
ChatPlace ChatAt(const HosCoopDraw& d, const Look* look) {
    ChatPlace c;
    if (look) {
        // The game's font, sized with the picture; wider lines, and the same
        // left edge as the HUD blocks (the co-op window's ChatLayer).
        const Pic p = Picture(d);
        c.px = std::clamp(kChatGamePx * p.u, 14.0f, 40.0f);
        c.left = std::max(kMargin, p.left + (kHudLeft - kPad) * p.u);
        c.width = std::min(std::max(200.0f, p.width * 0.5f), d.width - 2 * c.left);
        return c;
    }
    c.px = ChatPx(d);
    c.width = std::min(kChatWidth * c.px / 14.0f, d.width - 2 * kMargin);
    return c;
}

// The chat box's height (0 when closed), drawn at the bottom left.
float DrawBox(const HosCoopDraw& d, const std::optional<ChatBoxView>& box, const Look* look) {
    if (!box) {
        return 0.0f;
    }
    const ChatPlace at = ChatAt(d, look);
    const float px = at.px;
    if (look) {
        // No box: the typed text in the game's font with its shadow; red text
        // when it was not sent (the co-op window's ChatBox).
        Font(d, look->regular);
        const float height = px * 1.4f + 6.0f;
        const float top = d.height - kMargin - height;
        const float room = at.width - 2 * kPad;
        const float y = top + (height - px) / 2;
        if (box->text.empty()) {
            GameText(d, at.left + kPad, y, px, Rgba(255, 255, 255, 160), Elided(d, px, kChatPlaceholder, room).c_str());
        } else {
            GameText(d, at.left + kPad, y, px, box->failed ? Rgba(255, 122, 106, 255) : Rgba(255, 255, 255, 255),
                     Tail(d, px, box->text + "_", room).c_str());
        }
        return height;
    }
    const float width = at.width;
    const float height = px + 12.0f;
    const float top = d.height - kMargin - height;
    if (box->failed) {
        d.rect(d.user, kMargin - 2, top - 2, kMargin + width + 2, top + height + 2, Rgba(220, 60, 60, 230), 7.0f);
    }
    d.rect(d.user, kMargin, top, kMargin + width, top + height, Rgba(0, 0, 0, 190), 6.0f);
    const float room = width - 2 * kPad;
    if (box->text.empty()) {
        d.text(d.user, kMargin + kPad, top + 6, px, Rgba(255, 255, 255, 160),
               Elided(d, px, kChatPlaceholder, room).c_str());
    } else {
        d.text(d.user, kMargin + kPad, top + 6, px, Rgba(255, 255, 255, 255), Tail(d, px, box->text + "_", room).c_str());
    }
    return height;
}

// The game look's chat lines: no box; "Name: text" with the name in the
// player's colour and the text in white, room notices in the highlight
// colour, all with the game's shadow (the co-op window's ChatLayer).
void DrawChatGame(const HosCoopDraw& d, const std::vector<ChatShown>& chat, float above, const Scene& scene,
                  const Look& look) {
    const ChatPlace at = ChatAt(d, &look);
    const float px = at.px;
    Font(d, look.regular);
    const float line = px * 1.3f;
    const float bottom = d.height - kMargin - (above > 0 ? above + 4.0f : 0.0f);
    float y = bottom - kPad - line * static_cast<float>(chat.size());
    const float left = at.left + kPad;
    const float room = at.width - 2 * kPad;
    for (const auto& l : chat) {
        const int a = Alpha(l.alpha);
        float x = left;
        std::string rest = l.text;
        if (const auto colon = l.text.find(": "); !l.notice && colon != std::string::npos && colon > 0) {
            const std::string name = l.text.substr(0, colon + 1);
            rest = l.text.substr(colon + 1);
            GameText(d, x, y, px, WithAlpha(ColourOf(scene, std::string_view(l.text).substr(0, colon)), a),
                     name.c_str());
            x += d.text_width(d.user, px, name.c_str());
        }
        const std::string text = Elided(d, px, rest, std::max(10.0f, room - (x - left)));
        GameText(d, x, y, px, WithAlpha(l.notice ? kHighlight : Rgba(255, 255, 255, 255), a), text.c_str());
        y += line;
    }
}

void DrawChat(const HosCoopDraw& d, const std::vector<ChatShown>& chat, float above, const Scene& scene,
              const Look* look) {
    if (chat.empty()) {
        return;
    }
    if (look) {
        DrawChatGame(d, chat, above, scene, *look);
        return;
    }
    const float px = ChatPx(d);
    const float room = std::min(kChatWidth * px / 14.0f, d.width - 2 * kMargin) - 2 * kPad;
    const float line = px + 8.0f;
    float y = d.height - kMargin - (above > 0 ? above + 6.0f : 0.0f) - line * static_cast<float>(chat.size());
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

void DrawPlayers(const HosCoopDraw& d, const std::vector<CoopHook::PlayerRow>& rows, const Look* look) {
    const auto cells = PlayerCells(rows, look != nullptr);
    float px = std::clamp(d.height * 0.022f, 14.0f, 28.0f);
    float line = px + 6.0f;
    if (look) {
        Font(d, look->regular);
        px = std::clamp(kListGamePx * Picture(d).u, 14.0f, 40.0f);
        line = px * 1.25f;
    }
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
    if (!look) {  // the game look has no box
        d.rect(d.user, left, top, left + width, top + height, Rgba(0, 0, 0, 170), 8.0f);
    }
    float y = top + 2 * kPad;
    for (std::size_t r = 0; r < cells.size(); ++r) {
        const bool dim = r > 0 && rows[r - 1].state != "connected";
        const std::uint32_t colour = r == 0 ? (look ? kHighlight : Rgba(255, 220, 120, 255))
                                     : dim  ? Rgba(170, 170, 170, 255)
                                            : Rgba(255, 255, 255, 255);
        float x = left + 2 * kPad;
        for (std::size_t c = 0; c < cells[r].size(); ++c) {
            if (look) {
                GameText(d, x, y, px, colour, cells[r][c].c_str());
            } else {
                d.text(d.user, x, y, px, colour, cells[r][c].c_str());
            }
            x += widths[c] + gap;
        }
        y += line;
    }
}

// The other players under the game's own hearts, top left, as the game would
// draw them (the co-op window's HudLayer): per player their name in the
// game's font, a row of their hearts with the game's heart pictures, and the
// game's name of the place they are in. No box. Game look only.
void DrawHud(const HosCoopDraw& d, const std::vector<HudBlock>& blocks, const Look& look) {
    const Pic p = Picture(d);
    const float u = p.u;
    const float x = p.left + kHudLeft * u;
    float y = static_cast<float>(CoopHudLayout::BlocksTop(d.width, d.height));
    const float heart = kHeartPx * u;
    const float name_px = kHudNamePx * u;
    const float place_px = kHudPlacePx * u;
    for (const auto& b : blocks) {
        const int a = b.dim ? 150 : 255;
        Font(d, look.medium);
        GameText(d, x, y, name_px, Rgba(255, 255, 255, a), b.name.c_str());
        y += name_px + 2 * u;
        if (!b.hearts.empty()) {
            for (std::size_t i = 0; i < b.hearts.size(); ++i) {
                const int col = static_cast<int>(i) % CoopHudHearts::kPerRow;
                const int row = static_cast<int>(i) / CoopHudHearts::kPerRow;
                const float hx = x - heart * 0.22f + col * heart * kHeartStep;
                const float hy = y - heart * 0.2f + row * heart * kHeartStep;
                d.draw_image(d.user, look.hearts[std::clamp(b.hearts[i], 0, 4)], hx, hy, hx + heart, hy + heart,
                             Rgba(255, 255, 255, a));
            }
            const int rows = (static_cast<int>(b.hearts.size()) + CoopHudHearts::kPerRow - 1) / CoopHudHearts::kPerRow;
            y += rows * heart * kHeartStep + 3 * u;
        }
        if (!b.place.empty()) {
            Font(d, look.regular);
            GameText(d, x, y, place_px, Rgba(225, 225, 225, a), b.place.c_str());
            y += place_px;
        }
        y += kHudGap * u;
    }
}

void DrawCheats(const HosCoopDraw& d, const CheatsView& c) {
    const CheatsBox b = CheatsAt(d.width, d.height, c);
    const float px = b.px;
    const float line = b.line;
    const float left = b.left;
    const float width = b.right - b.left;
    d.rect(d.user, b.left, b.top, b.right, b.bottom, Rgba(0, 0, 0, 200), 8.0f);
    const float x = left + kPad;
    const float room = width - 2 * kPad;
    float y = kMargin + kPad;
    d.text(d.user, x, y, px, Rgba(255, 220, 120, 255),
           Elided(d, px, "Co-op item cheats (debug): a click or Enter gives, F8 or Esc closes", room).c_str());
    y += line;
    d.text(d.user, x, y, px, Rgba(255, 255, 255, 255),
           Tail(d, px, "Find: " + c.filter + "_", room).c_str());
    y += line;
    d.text(d.user, x, y, px, Rgba(170, 170, 170, 255),
           c.filter.empty() ? "Quick gifts (type to find any item flag):" : "Item flags with that in the name:");
    y += line;
    if (c.rows.empty()) {
        d.text(d.user, x, y, px, Rgba(170, 170, 170, 255), "(none)");
        y += line;
    }
    for (std::size_t i = 0; i < c.rows.size(); ++i) {
        if (i == c.selected) {
            d.rect(d.user, left + 4, y - 2, left + width - 4, y + px + 3, Rgba(90, 200, 255, 90), 4.0f);
        }
        d.text(d.user, x, y, px, Rgba(255, 255, 255, 255), Elided(d, px, c.rows[i], room).c_str());
        y += line;
    }
    if (!c.result.empty()) {
        d.text(d.user, x, y, px, Rgba(255, 220, 120, 255), Elided(d, px, c.result, room).c_str());
    }
}

}  // namespace

CheatsBox CheatsAt(float width, float height, const CheatsView& c) {
    CheatsBox b;
    b.px = std::clamp(height * 0.022f, 14.0f, 28.0f);
    b.line = b.px + 6.0f;
    const float w = std::min(560.0f * b.px / 14.0f, width - 2 * kMargin);
    b.left = width - kMargin - w;
    b.right = b.left + w;
    b.top = kMargin;
    const std::size_t lines = 3 + std::max<std::size_t>(c.rows.size(), 1) + (c.result.empty() ? 0 : 1);
    b.bottom = b.top + static_cast<float>(lines) * b.line + 2 * kPad;
    b.rows_top = kMargin + kPad + 3 * b.line;
    return b;
}

std::optional<std::size_t> CheatsRowAt(float width, float height, const CheatsView& c, float x, float y) {
    const CheatsBox b = CheatsAt(width, height, c);
    if (x < b.left || x >= b.right || y < b.rows_top - 3.0f) {
        return std::nullopt;
    }
    const auto i = static_cast<std::size_t>((y - (b.rows_top - 3.0f)) / b.line);
    if (i >= c.rows.size()) {
        return std::nullopt;
    }
    return i;
}

std::vector<std::vector<std::string>> PlayerCells(const std::vector<CoopHook::PlayerRow>& rows, bool place) {
    std::vector<std::vector<std::string>> cells{{"#", "Player", "State", "Ping", place ? "Place" : "Scene"}};
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

std::vector<ChatShown> ShownChat(const std::vector<CoopHook::ChatLine>& lines, bool box_open) {
    std::vector<ChatShown> out;
    for (const auto& l : lines) {
        float alpha = 1.0f;
        if (!box_open && l.age_ms >= kChatShowMs) {
            alpha = 1.0f - static_cast<float>(l.age_ms - kChatShowMs) / static_cast<float>(kChatFadeMs);
        }
        if (alpha > 0.0f) {
            out.push_back({l.text, l.notice, alpha});
        }
    }
    return out;
}

// The title's Multiplayer prompt (the menu itself is the Multiplayer tab of
// Hookshot's menu, coop_pause.h): the Minus button and the words, bottom left,
// in the game's font with its soft shadow; the plain look (no pack): the same
// with Hookshot's font.
static void DrawTitle(const HosCoopDraw& d, const TitleView& t, const Look* look) {
    const Pic p = Picture(d);
    const float u = p.u;
    auto text = [&](float x, float y, float px, std::uint32_t fill, const std::string& s) {
        if (look) {
            GameText(d, x, y, px, fill, s.c_str());
        } else {
            d.text(d.user, x, y, px, fill, s.c_str());
        }
    };
    if (t.prompt.empty()) {
        return;
    }
    // The control guide's place, bottom left: the Minus button and the words.
    const std::uint32_t white = Rgba(255, 255, 255, 255);
    Font(d, look ? look->regular : 0);
    const float px = std::max(14.0f, 24.0f * u);
    const float r = px * 0.62f;
    const float cx = p.left + 48.0f * u + r, cy = p.height + (d.height - p.height) / 2 - 44.0f * u;
    d.circle(d.user, cx, cy, r, Rgba(0, 0, 0, 130), 0.0f);
    d.circle(d.user, cx, cy, r, Rgba(255, 255, 255, 230), std::max(1.5f, 2.0f * u));
    d.rect(d.user, cx - r * 0.5f, cy - std::max(1.0f, 1.5f * u), cx + r * 0.5f, cy + std::max(1.0f, 1.5f * u), white, 0.0f);
    text(cx + r + 10.0f * u, cy - px * 0.62f, px, white, t.prompt);
}

bool Wanted(const Scene& s) {
    if (s.title && !s.title->prompt.empty()) {
        return true;
    }
    return !s.chat.empty() || !s.hud.empty() || s.box.has_value() || s.cheats.has_value() || (s.markers && !s.markers->items.empty()) || s.players.has_value() ||
           (s.panel && !s.panel->empty()) || s.dbno.has_value();
}

bool GameLook(const HosCoopDraw& d, const Look& look) {
    return look.On() && d.use_font && d.draw_image;
}

void Draw(const HosCoopDraw& d, const Scene& s, const Look& look) {
    if (d.width <= 0 || d.height <= 0 || !d.rect || !d.circle || !d.triangle || !d.text || !d.text_width) {
        return;
    }
    const Look* game = GameLook(d, look) ? &look : nullptr;
    if (s.markers) {
        DrawMarkers(d, *s.markers, s, game);
    }
    if (game) {
        DrawHud(d, s.hud, *game);
    }
    if (s.panel) {
        Font(d, 0);
        DrawPanel(d, *s.panel);
    }
    if (s.dbno) {
        Font(d, game ? game->regular : 0);
        CoopDbnoDraw::Draw(d, *s.dbno);
    }
    DrawChat(d, s.chat, DrawBox(d, s.box, game), s, game);
    if (s.players) {
        DrawPlayers(d, *s.players, game);
    }
    if (s.cheats) {
        Font(d, 0);
        DrawCheats(d, *s.cheats);
    }
    if (s.title) {
        DrawTitle(d, *s.title, game);
    }
    Font(d, 0);
}

}  // namespace CoopDraw
