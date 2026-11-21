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
// The same pages on the title's own rows (coop_title.cpp, its Host, Join and Co-op Settings): the menu opened on the
// page without the overlay; the title shows NativeView's rows on its buttons and sends its presses through
// NativeInput (HOS_COOP_MENU_* events: they act as the overlay's, Settle included); Key and Char type into a field as
// the overlay's. CloseNative when the title leaves the page (its Back).
void OpenNative(CoopPauseMenu::Page page);
bool NativeInput(int event, int arg = 0, int arg2 = 0);
CoopPauseMenu::View NativeView();
bool NativeOpen();
CoopPauseMenu::Page NativePage();   // (the page the menu shows now)
void CloseNative();
// Another page of co-op's menu while it shows natively (the pause page's Settings and look): the page only, no files
// read again (OpenNative read them as it opened).
void NativeGo(CoopPauseMenu::Page page);

// The pause menu's Multiplayer page (coop_pause_tab.h): its copies, done on the page itself (co-op's menu stays shut): the host's room name (else its relay join code,
// else its invite code), a waiting guest's answer code, the host's same-network or VPN join line (as the menu's rows).
// PageNote: what became of the last one ("Copied the room name", or why not) for 4 s after it, else "".
enum class PageCopy { kCode, kAnswer, kLan, kVpn };
void PageCopyNow(PageCopy what);
std::string PageNote();
// What the page's last Host, Join, Leave or Paste answer came to when co-op's menu would have said it (refused: the
// menu's note, else co-op's status line; a Paste answer: its result), for the page's Session view; `gen` (may be null)
// goes up with each one, so the page shows the newest once. "" none yet.
std::string PageSays(std::uint64_t* gen);
// The page's own actions, through the menu's requests and its rules: hosting at once with the kit's settings (its
// room name, relay, port, approval), as the menu's Host does; and leaving (or stopping hosting), as its Leave does.
// Co-op that came with the game's start (ByLauncher) is neither started nor stopped (PageSays why). A Host refused or
// failed: PageSays why.
void HostNow();
void LeaveNow();
// Co-op came with the game's start (Host.bat, Join.bat, coop.ini): the page's rows open the menu instead.
bool ByLauncher();
// The tab's own Host started co-op (hosting, or still starting to).
bool HostStarted();
// The Multiplayer page's Join view: joining with the code (or address line) on the clipboard, as the Join page's
// paste then Join; and the last game again ("Join <host> again", its code kept in coop.ini). Refused (co-op on, the
// code not readable) or failed: PageSays why.
void JoinPasteNow();
// The page's Join view: join `room` (a room name) on the kit's relay (coop-options.ini's relay), with `password` (the
// host's join password, "" none). No relay set, refused or failed: PageSays why.
void JoinRoomNow(const std::string& room, const std::string& password);
// A relay is set in coop-options.ini (relay = ...): a room name can be joined through it.
bool RelaySet();
// Auto-rejoin (coop_auto_rejoin.h; coop-options.ini auto_rejoin): on or off, saved at once; and whether a dropped
// connection is being tried again now.
bool AutoRejoin();
void SetAutoRejoin(bool on);
bool AutoRejoining();
// Hosting with a relay room puts it on the relay's public room list (coop-options.ini list_room), or not; saved at once.
bool ListRoom();
// The kit's relay ("host" or "host:port", coop-options.ini relay), "" none: the files' setting.
std::string Relay();
void SetListRoom(bool on);
// The page's own typing (the details' Router line, the browser's online list): the port when hosting, saved at once
// (coop-options.ini server_port) and, while hosting, hosting again on it; the relay's address (relay), saved at once.
// False when refused (not a port, not a relay address): the page's note says why.
bool SetHostPort(int port);
bool SetRelay(const std::string& relay);
// This game's room name when it hosts (coop-options.ini relay_room; "" none: a join code), and set from the pause
// menu's Multiplayer page (saved; the next Host uses it). False: not a room name (3 to 24 letters, digits, - or _).
std::string RoomName();
bool SetRoomName(const std::string& name);
// The page's "Games on this network": join the game at that address and port, with `password` (the Join view's,
// "" none). Refused or failed: PageSays why.
void JoinLanNow(const std::string& address, std::uint16_t port, const std::string& password);
void RejoinNow();
// A last game's code is kept (coop.ini last_invite); its host's name into `host` (may be null; "" when not known).
bool Rejoin(std::string* host);
// The host's Paste answer (a friend's answer code from the clipboard), as the menu's row does it; PageSays what became
// of it.
void PasteAnswerNow();

// The autotest's `menu customize|close` step (LICHII_COOP_TEST_COMMANDS only, coop_testinput.h): the menu opened
// on the Customize page, or closed. Never asked in a player's game (no commands file there).
bool TestMenu(const std::string& page);

}  // namespace CoopPause
