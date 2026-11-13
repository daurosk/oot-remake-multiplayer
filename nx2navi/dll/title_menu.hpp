// SPDX-License-Identifier: GPL-3.0-or-later
// "Multiplayer" on the game's title screen (NX2navi's plugin interface,
// version 3): one entry of the title menu, which opens the game's own settings
// screen with our pages (menu_model.hpp): the four ways to play, Host over
// Local Network, Join over Local Network, and the multiplayer saves
// (mp_saves.hpp: listed, made, copied from the player's own, renamed and
// deleted here, on the game's thread). Nothing here connects: choosing
// Start or Join hands a Request to the DLL's own thread (coop_dll.cpp), which
// starts the server or fetches the host's save and reports back with SetPhase.
//
// Until the player chooses Start or Join, the title entry is all the DLL has
// in the game: the page's callbacks (frame, key, text, pad) are registered
// when the page opens and taken out when it closes.
//
// Threads: the title entry's on_select and the page's events come on the
// game's thread; keys and typed text on the window's thread; the game's
// buttons on the thread that reads the controls; SetPhase and TakeRequest on
// the DLL's thread. One mutex guards the state; update_menu is only called on
// the game's thread (in an event, or in the frame callback).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "menu_model.hpp"

struct Nx2PluginApi;

namespace CoopNx2navi::TitleMenu {

struct Request {
    bool host = false;
    std::string name;
    std::string server;  // Join: the host's address; Host: empty
    std::uint16_t port = Menu::kDefaultPort;
    std::string password;  // "" none
    // Host: the folder name of the multiplayer save to host on (under
    // MpSaves::Root of the mod folder); "" in a game that cannot be started
    // on a save folder: the session then runs on the player's own saves.
    std::string save;
    bool save_empty = false;  // that save has no slot yet: a game to begin
};

// Adds the title entry when the interface has it (version 3 with open_menu).
// False, with a line in the log, when it does not. Once.
bool Register(const Nx2PluginApi* api, const std::filesystem::path& mod_folder,
              void (*log)(const char* line));
bool Registered();

// The DLL's thread: what the player chose since the last call.
std::optional<Request> TakeRequest();
// The DLL's thread: how far the request got, shown on the page. `message` is a
// whole sentence in the menu's language (Menu::T), or "".
void SetPhase(Menu::Phase phase, std::string message = {}, std::uint8_t player = 0);
// Closes the page, as the player's "back" does. Any thread.
void Close();
// The menu's language (the game's, unless the player chose another).
Menu::Lang Language();

// Takes everything out. Any thread.
void Shutdown();

// The join password as the mod's settings keep it between two starts of the
// game: Windows' data protection for this user (never clear text), base64.
// Empty when it cannot be done. Also used for the restart into the co-op save.
std::string Protect(const std::string& secret);
std::string Unprotect(const std::string& stored);

}  // namespace CoopNx2navi::TitleMenu
