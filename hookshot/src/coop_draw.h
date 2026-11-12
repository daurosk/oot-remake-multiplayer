// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op over Hookshot's picture (docs/hookshot.md): the chat lines and the
// players' name tags the emulator's co-op window draws with Qt
// (lichii/hook/coop_window.cpp, its plain look), drawn here through
// Hookshot's Dear ImGui (hos_coop_draw.h). The player list, the chat box,
// the HUD hearts and the cheats window are not drawn on Hookshot yet.
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

// Anything to draw: a chat line showing, or a marker.
bool Wanted(const std::vector<ChatShown>& chat, const std::optional<CoopHook::Markers>& markers);

// Draws the markers (scaled from the game's scene target to the picture),
// then the chat lines at the bottom left.
void Draw(const HosCoopDraw& d, const std::vector<ChatShown>& chat, const std::optional<CoopHook::Markers>& markers);

}  // namespace CoopDraw
