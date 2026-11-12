// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op over Hookshot's picture (docs/hookshot.md): what the emulator's co-op
// window draws with Qt (lichii/hook/coop_window.cpp, its plain look), drawn
// here through Hookshot's Dear ImGui (hos_coop_draw.h): the chat lines, the
// players' name tags, the player list (F6 held) and the panel (F7). The chat
// box, the HUD hearts and the cheats window are not drawn on Hookshot yet.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

struct ChatShown {
    std::string text;
    bool notice = false;
    float alpha = 1.0f;
};

// The chat lines still showing, oldest first, with their fade.
std::vector<ChatShown> ShownChat(const std::vector<CoopHook::ChatLine>& lines);

// What to draw this present.
struct Scene {
    std::vector<ChatShown> chat;
    std::optional<CoopHook::Markers> markers;
    std::optional<std::vector<CoopHook::PlayerRow>> players;  // the player list key is held
    std::optional<std::vector<CoopHook::OverlayLine>> panel;  // the panel is on
};

// Anything to draw.
bool Wanted(const Scene& scene);

// Draws the markers (scaled from the game's scene target to the picture),
// the panel at the top left, the chat lines at the bottom left, then the
// player list over the top middle.
void Draw(const HosCoopDraw& d, const Scene& scene);

// The player list's cells: a header row, then one row per player (as the
// co-op window's: "P1", the name with " (you)", state, ping, scene).
std::vector<std::vector<std::string>> PlayerCells(const std::vector<CoopHook::PlayerRow>& rows);

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
