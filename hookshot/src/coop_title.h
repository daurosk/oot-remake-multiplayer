// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer menu on the game's title screen, in hookshot_coop.dll
// (docs/hookshot.md, "The title menu"). Patch 0004 tells the module when the
// game's UITitleScreen is up (hos_coop_title) and hands it the pad first
// (hos_coop_pad). While the title is up a "(-) Multiplayer" prompt shows at
// the bottom left; Minus opens the menu (coop_title_menu.h), drawn in the
// game look, with the game's own UI sounds. Host and Join go through the
// module's own host (native-host-b, hos_coop_host.h: the server or the invite
// code's join helper inside the game, no windows), then this game's client
// connects. Settings writes coop.ini and the kit's coop-options.ini.
// The module stays loaded, co-op idle, when coop.ini names no server.
#pragma once

#include <cstdint>
#include <optional>

#include "coop_draw.h"
#include "hos_coop_api.h"

namespace CoopTitle {

// At the module's start: the table, and whether co-op runs already (coop.ini
// named a server: the menu then only shows what it is doing, and Settings).
void Init(const HosCoopApi* api, bool coop_on);
// The kit's coop-options.ini as the launcher's variables, for the ones not
// set (the game started without Host.bat or play.cmd). Before co-op starts.
void ApplyKitOptions();

// hos_coop_title: the game's calc thread (ctx may call()).
void OnTitle(void* ctx, std::uint64_t screen, int event);
// hos_coop_frame's every-frame site (ctx may call()): sounds still to play.
void OnFrame(void* ctx);
// hos_coop_pad: player 1's pad before the game gets it; what the game gets.
std::uint64_t OnPad(std::uint64_t buttons, std::int32_t stick[4]);
// The window's keys and characters (Unicode) while the menu is open: true
// when taken.
bool Key(std::uint32_t vk, bool repeat);
bool Char(std::uint32_t cp);
// The menu has the keyboard (the game reads no keys).
bool KeysTaken();
// This PC's own id for rejoining (coop.ini client_id, made once): the module's
// join helper uses the same nonce and port for the same invite with it.
std::string ClientId();

// What to draw: the prompt or the menu; nothing when the title is not up.
std::optional<CoopDraw::TitleView> View();


}  // namespace CoopTitle
