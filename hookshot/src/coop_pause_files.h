// SPDX-License-Identifier: GPL-3.0-or-later
// What the Multiplayer tab (coop_pause.h) and the title screen (coop_title.h)
// both need of this PC, in hookshot_coop.dll: the process's variables, the
// settings files (coop.ini beside longshot.exe, the kit's coop-options.ini),
// the clipboard and this PC's addresses. Moved here from coop_title.cpp when
// the menu moved into Hookshot's pause menu.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "coop_title_menu.h"

namespace CoopFiles {

// A variable of this process ("" when not set), and set (for this process
// and the children it starts; "" unsets it).
std::string Env(const char* name);
void SetEnv(const std::string& name, const std::string& value);

// A UTF-8 path (a file's text, a variable) as a path, and back.
std::filesystem::path PathOf(const std::string& s);
std::string Utf8Path(const std::filesystem::path& p);
// A file's bytes ("" when it cannot be read), and the file written whole (a
// new file beside it, then moved over the old one).
std::string ReadText(const std::filesystem::path& p);
bool WriteText(const std::filesystem::path& p, const std::string& text);

// The kit this game belongs to: OOT_COOP_KIT, the first line of coop-kit.txt
// beside longshot.exe, or the nearest folder above the game that has
// server\coop_server.exe and start-server.cmd (the kit's hookshot\game\). On Linux:
// server/coop_server and coop-options.ini (the Linux kit's data/, its game in data/game/).
std::optional<std::filesystem::path> FindKit();
// coop.ini beside longshot.exe.
std::filesystem::path CoopIni();

// The settings as the two files have them; a first-time player with no name
// in coop.ini gets their Windows name when the server would take it.
CoopTitleMenu::Options ReadOptions();
// The settings' LICHII_COOP_* variables for this run (co-op not started yet).
void SetCoopEnvironment(const CoopTitleMenu::Options& o);

// The clipboard's text (at most 4096 bytes; "" when none), and text put on it.
std::string ClipboardText();
bool SetClipboardText(const std::string& text);

// This PC's IPv4 addresses, as text (CoopTitleMenu::LanAddresses picks the
// ones a friend can use).
std::vector<std::string> LocalIpv4s();

}  // namespace CoopFiles
