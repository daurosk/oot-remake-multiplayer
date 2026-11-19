// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer tab of Hookshot's pause menu, in hookshot_coop.dll
// (docs/hookshot.md, "The Multiplayer tab"). Patch 0012 makes Hookshot's menu
// (F1, Insert, the pad's Home or Select + Start) ask the module for its tabs
// (hos_coop_menu.h) and show them first in its strip: Multiplayer, with the
// pages Host, Join, Local co-op and Settings (coop_pause_menu.h, the logic).
// This file carries out what the tab asks for: Host and Join through the
// module's own host (hos_coop_host.h: the server or the invite code's join
// helper inside the game, no windows), Stop hosting and Leave, the settings
// written to coop.ini and the kit's coop-options.ini, the clipboard, and
// what co-op is doing, shown on the tab while a session runs.
// The title screen's Host, Join and Co-op Settings rows (coop_title.h) open
// the menu on the tab's page. Thread-safe: Hookshot's menu tick, its present
// thread and the window's key thread all call in.
#pragma once

#include <cstdint>
#include <string>

#include "coop_pause_menu.h"
#include "hos_coop_menu.h"
#include "hos_coop_menu_picture.h"

namespace CoopPause {

// At the module's start: whether co-op runs already (coop.ini named a server,
// or the kit's Host.bat / Join.bat started it): the tab then shows that and
// leaves Host and Join alone.
void Init(bool coop_on);

// Each game frame (HOS_COOP_SITE_FRAME), shown or not: what follows from
// co-op's state (the invite code copied, a connected join remembered and the
// host's save loaded at the title). At most every 250 ms.
void Tick();

// hos_coop_menu.h's exports.
int Tabs();
const char* TabName(int tab);
bool Page(int tab, HosCoopMenuPage* out);
// hos_coop_menu_picture.h's export (patch 0019): the Customize page's live window onto your own Link (the camera:
// coop_look_view.h), else its flat picture of him.
bool Picture(int tab, HosCoopMenuPicture* out);
bool Input(int tab, int event, int arg, int arg2);
int Wanted();

// The window's keys and characters (Unicode) while the tab types into a
// field: true when taken. Hookshot's own keys (F1, Insert) are never taken.
bool Key(std::uint32_t vk, bool repeat);
bool Char(std::uint32_t cp);
bool KeysTaken();

// The menu opened on the tab at this page (the title's rows, Minus at the
// title): Host hosts at once, Join pastes a code already copied.
void Open(CoopPauseMenu::Page page);

// The pause menu's Multiplayer page (coop_pause_tab.h): its copies, done on the page itself (co-op's menu stays shut): the host's room name (else its relay join code,
// else its invite code), a waiting guest's answer code, the host's same-network or VPN join line (as the menu's rows).
// PageNote: what became of the last one ("Copied the room name", or why not) for 4 s after it, else "".
enum class PageCopy { kCode, kAnswer, kLan, kVpn };
void PageCopyNow(PageCopy what);
std::string PageNote();
// The page's own actions, through the menu's requests and its rules: hosting at once with the kit's settings (its
// room name, relay, port, approval), as the menu's Host does; and leaving (or stopping hosting), as its Leave does.
// Co-op that came with the game's start (ByLauncher) is neither started nor stopped: the menu opens on its main page,
// which says why. A Host refused or failed opens it too (its note, or the session's lines say why).
void HostNow();
void LeaveNow();
// Co-op came with the game's start (Host.bat, Join.bat, coop.ini): the page's rows open the menu instead.
bool ByLauncher();
// The Multiplayer page's Join view: joining with the code (or address line) on the clipboard, as the Join page's
// paste then Join; and the last game again ("Join <host> again", its code kept in coop.ini). Refused (co-op on, the
// code not readable) or failed: the menu opens on its main page or Join page, whose note says why.
void JoinPasteNow();
void RejoinNow();
// A last game's code is kept (coop.ini last_invite); its host's name into `host` (may be null; "" when not known).
bool Rejoin(std::string* host);
// The host's Paste answer (a friend's answer code from the clipboard), as the menu's row does it; the menu opened on
// its main page, whose note says what became of it.
void PasteAnswerNow();

// The autotest's `menu customize|close` step (LICHII_COOP_TEST_COMMANDS only, coop_testinput.h): the menu opened
// on the Customize page, or closed. Never asked in a player's game (no commands file there).
bool TestMenu(const std::string& page);

}  // namespace CoopPause
