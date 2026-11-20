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
// The title waits for the host's save (Longshot patch 0028's hold, hos_coop_title_hold).
bool SaveWaitWaiting();
// Player 1's buttons before the code mods' holds come off (coop_pad_order.h step 2), for the player's own A on the
// title (CoopTitleMenu::PlayerChoice): an A on a title button that is not a co-op row drops a guest's pending switch to
// the host's save (also when a code mod holds it: Save Profiles' file select takes that A), and one on the title's
// first button (Continue, or Start in the title's own menu) makes a code mod's button choice the player's for
// PlayerChoice::kWindowMs, so co-op being busy at the title does not refuse it (a mod's own choice with no A from the
// player before it, as at the game's start, is still refused). The window ends once Link is in the game, when a join
// starts (StartSaveWait) and when the host's save is offered again (the player backed out of their pick).
void NotePlayerPad(std::uint64_t buttons);
bool PlayerChoseLately();

// This PC's own id for rejoining (coop.ini client_id, made once): the module's
// join helper uses the same nonce and port for the same invite with it.
std::string ClientId();

// A code mod changed the save folder's files (coop_mod_api.h reload_save_list): the game re-reads its save list at
// the title's next step, as after co-op's own switch of the folder. False (nothing asked) when the title is not up:
// the read runs only at the title's step. Any thread.
bool AskSaveListRead();
// The game's save list is read: no re-read asked or running, and the game's own state says read. Any thread.
bool SaveListReady();

// What to draw: the prompt, or the host's save loading; nothing when the
// title is not up.
std::optional<CoopDraw::TitleView> View();

}  // namespace CoopTitle
