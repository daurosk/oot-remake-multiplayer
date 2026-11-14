// SPDX-License-Identifier: GPL-3.0-or-later
// The game's title screen, in hookshot_coop.dll (docs/hookshot.md, "The
// Multiplayer tab"). Co-op itself is the Multiplayer tab of Hookshot's menu
// (coop_pause.h); the title only leads there. Patch 0004 tells the module when
// the game's UITitleScreen is up (hos_coop_title) and hands it the pad first
// (hos_coop_pad). The puppet pack's title has its own Host, Join and Co-op
// Settings rows: each opens the tab at that page. Without them a
// "(-) Multiplayer" prompt shows at the bottom left and Minus opens the tab.
// After a join the title says what the host's save is doing (CoopTitleMenu::SaveWait); nothing is pressed.
// The module stays loaded, co-op idle, when coop.ini names no server.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "coop_draw.h"
#include "coop_look.h"
#include "hos_coop_api.h"

namespace CoopTitle {

// At the module's start: the table.
void Init(const HosCoopApi* api);
// The kit's coop-options.ini as the launcher's variables, for the ones not
// set (the game started without Host.bat or play.cmd). Before co-op starts.
// No kit above the game (FindKit): the coop-options.ini beside longshot.exe.
void ApplyKitOptions();
// The look the player chose in the pause menu (coop_look.h), written to the
// kit's coop-options.ini (else coop.ini) so the next start keeps it (the
// title's Settings page reads the files when it opens). The calc thread.
void SaveLook(const CoopLook::Look& look);

// hos_coop_title: the game's calc thread (ctx may call()).
void OnTitle(void* ctx, std::uint64_t screen, int event);
// hos_coop_pad: player 1's pad before the game gets it; what the game gets.
std::uint64_t OnPad(std::uint64_t buttons, std::int32_t stick[4]);

// A join connected: the title says what the host's save is doing until it is
// in (nothing is pressed: the player loads a save themselves when the chat
// says Success). True when the title is up now (the menu should close).
bool StartSaveWait(const std::string& host);
// Co-op stopped: the title's line goes.
void StopSaveWait();

// This PC's own id for rejoining (coop.ini client_id, made once): the module's
// join helper uses the same nonce and port for the same invite with it.
std::string ClientId();

// What to draw: the prompt, or the host's save loading; nothing when the
// title is not up.
std::optional<CoopDraw::TitleView> View();

}  // namespace CoopTitle
