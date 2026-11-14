// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op over Hookshot's picture (docs/hookshot.md): what the emulator's co-op
// window draws with Qt (lichii/hook/coop_window.cpp, its plain look), drawn
// here through Hookshot's Dear ImGui (hos_coop_draw.h): the chat lines, the
// players' name tags, the player list (F6 held) and the panel (F7). The chat
// box (T, coop_chatbox.h) and, with LICHII_COOP_CHEATS=1, the item cheats (F8,
// coop_cheats_panel.h) too.
//
// The game look (docs/overlay.md, "The look"), as the co-op window's: with
// the HUD pack's font and heart pictures (CoopHud::Folder, loaded through
// hos_coop_draw2's font and image calls: Look below) every co-op text is drawn
// like the game's HUD: no box, the game's font, a soft dark shadow, players'
// names in their colours, and the other players under your hearts with the
// game's heart pictures. Without them (an old pack, an older Hookshot,
// LICHII_COOP_HUD=0) the plain look: boxes, outlined name tags, no HUD blocks.
// The panel (F7) and the item cheats keep the plain look in both.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coop_dbno_game.h"
#include "coop_hook.h"
#include "hos_coop_draw.h"

namespace CoopDraw {

// As the co-op window: a chat line shows 8 s, then fades out in 1 s.
inline constexpr std::uint64_t kChatShowMs = 8000;
inline constexpr std::uint64_t kChatFadeMs = 1000;

// IM_COL32.
constexpr std::uint32_t Rgba(int r, int g, int b, int a) {
    return static_cast<std::uint32_t>(r & 0xFF) | static_cast<std::uint32_t>(g & 0xFF) << 8 |
           static_cast<std::uint32_t>(b & 0xFF) << 16 | static_cast<std::uint32_t>(a & 0xFF) << 24;
}

// The game look's pieces, as hos_coop_draw2's font and image calls gave them
// (0: not loaded). The co-op window's GameLook.
struct Look {
    int regular = 0;             // font-regular.otf
    int medium = 0;              // font-medium.otf (the regular one when it is missing)
    std::array<int, 5> hearts{};  // heart-0..4.png: 0 to 4 quarters filled
    bool On() const {
        for (const int h : hearts) {
            if (h == 0) {
                return false;
            }
        }
        return regular != 0 && medium != 0;
    }
};

// The game look's highlight colour (room notices, the list's header): the co-op window's kHighlight.
inline constexpr std::uint32_t kHighlight = Rgba(255, 206, 82, 255);

// A player's colour for names (CoopPuppet::NameColor, 0xRRGGBB) lightened as
// the co-op window's PlayerColour, so it reads on the dark shadow.
constexpr std::uint32_t Lifted(std::uint32_t rgb) {
    const auto lift = [](std::uint32_t c) { return static_cast<int>(c + (255 - c) * 35 / 100); };
    return Rgba(lift((rgb >> 16) & 0xFF), lift((rgb >> 8) & 0xFF), lift(rgb & 0xFF), 255);
}

struct ChatShown {
    std::string text;
    bool notice = false;
    float alpha = 1.0f;
};

// The chat lines still showing, oldest first, with their fade. While the
// chat box is open every kept line shows, unfaded (as the co-op window).
std::vector<ChatShown> ShownChat(const std::vector<CoopHook::ChatLine>& lines, bool box_open = false);

// The chat box, when it is open.
struct ChatBoxView {
    std::string text;
    bool failed = false;  // the last Enter was refused (not connected, or text the protocol refuses)
};

// What the box shows: the text with a caret, the start cut ("...") to fit
// `room` pixels; the placeholder when empty.
inline constexpr const char* kChatPlaceholder = "Say something (Enter sends, Esc closes)";

// One other player under your hearts (the co-op window's HudLayer).
struct HudBlock {
    std::string name;
    std::string place;        // the place's name, or ""
    std::vector<int> hearts;  // quarters filled in each (CoopHudHearts::Quarters)
    bool dim = false;         // not connected
};

// The item cheats panel, when it is open.
struct CheatsView {
    std::string filter;
    std::vector<std::string> rows;  // labels
    std::size_t selected = 0;
    std::string result;             // CoopCheats::LastResult
};

// Where the cheats panel is on a `width` x `height` picture, as DrawCheats
// draws it: its rectangle and its rows (row i's text at rows_top + i * line).
struct CheatsBox {
    float left = 0, top = 0, right = 0, bottom = 0;
    float rows_top = 0, line = 0, px = 0;
};
CheatsBox CheatsAt(float width, float height, const CheatsView& c);
// The row under (x, y), for the mouse; nothing outside the rows.
std::optional<std::size_t> CheatsRowAt(float width, float height, const CheatsView& c, float x, float y);

// A line on the title screen, bottom left, while it is up: today only the
// host's save loading (coop_title.cpp; no co-op hint there now, the
// Multiplayer tab is in the pause menu).
struct TitleView {
    std::string prompt;   // the words; "" for none
    bool button = false;  // the Minus button before them
};

// What to draw this present.
struct Scene {
    std::optional<TitleView> title;
    std::optional<CheatsView> cheats;
    std::vector<HudBlock> hud;
    std::optional<ChatBoxView> box;
    std::vector<ChatShown> chat;
    std::optional<CoopHook::Markers> markers;
    std::optional<std::vector<CoopHook::PlayerRow>> players;  // the player list key is held
    std::optional<std::vector<CoopHook::OverlayLine>> panel;  // the panel is on
    std::optional<CoopDbno::View> dbno;  // down but not out: we are down, or beside someone who is
    // The game look: players' colours (Lifted, alpha 255) by what tags and
    // chat lines call them: their names, "P1".."P4", "you". Nobody here: white.
    std::map<std::string, std::uint32_t, std::less<>> colours;
};

// Anything to draw.
bool Wanted(const Scene& scene);

// Draws the markers (scaled from the game's scene target to the picture),
// the HUD blocks, the panel at the top left, the chat lines at the bottom
// left, then the player list over the top middle. The game look when `look`
// is On and the table has hos_coop_draw2's calls; else the plain look (and no
// HUD blocks, as the co-op window).
void Draw(const HosCoopDraw& d, const Scene& scene, const Look& look = {});

// Whether Draw would use the game look with this table.
bool GameLook(const HosCoopDraw& d, const Look& look);

// The player list's cells: a header row, then one row per player (as the
// co-op window's: "P1", the name with " (you)", state, ping, scene; the
// game look calls the last column "Place", its rows carrying place names).
std::vector<std::vector<std::string>> PlayerCells(const std::vector<CoopHook::PlayerRow>& rows,
                                                  bool place = false);

// The keys (docs/hookshot.md): F6 held shows the player list, F7 turns the
// panel on and off. Tab and Enter, the emulator's keys, are the game's Plus
// on Hookshot. Update turns the panel over when F7 goes down.
struct Keys {
    bool list = false;
    bool panel = false;
    bool panel_was_down = false;
    void Update(bool list_down, bool panel_down) {
        list = list_down;
        if (panel_down && !panel_was_down) {
            panel = !panel;
        }
        panel_was_down = panel_down;
    }
};

}  // namespace CoopDraw
