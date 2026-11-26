// SPDX-License-Identifier: GPL-3.0-or-later
// The game window's name and picture: the title bar says "Squadala!" and the window (and so the taskbar) carries our logo,
// instead of Longshot's own title and icon. The module does it, in co-op or not (solo players too): Tick() runs from the
// every-frame hook and, about once a second, finds this process's game window and fixes the title and the icon when they
// are not ours. Windows only; on Linux the window is SDL's and the kit's start script names it instead (HOS_WINDOW_TITLE,
// HOS_WM_CLASS and the icon file longshot.png, see tools/release/kit/linux-longshot/scripts/start.sh).
// The pictures are pure functions here so they can be tested without a window.
// Since 0.11.6 (owner OK, 2026-11-19) the title carries the kit's release, "Squadala! v0.11.6": read from the kit's
// VERSION.txt ("kit: ... (release v0.11.6)", tools/release/make_kit_hookshot.ps1), the file the updater keeps; a
// build with no kit around it (a developer's) shows "Squadala!" alone. The pause menu's Multiplayer tab shows the
// same line (coop_pause_tab.h, SessionView::version). Solo too: nothing here needs a session.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace CoopWindowBrand {

inline constexpr const char* kTitle = "Squadala!";
// The icon's PNG, beside the game's exe (the kit's build and update scripts put it there).
inline constexpr const char* kIconFile = "squadala-icon.png";

// The game's own window classes (Longshot and Hookshot register their window under their name); the other windows of
// the process (the co-op item cheats window, dialogs) keep their own titles.
bool IsGameWindowClass(const std::string& cls);

// True when the title bar is not `want` yet. The game's title carries live numbers on some builds: any text that is
// not exactly ours is put back.
bool TitleNeedsSetting(const std::string& current, const std::string& want);

// The release named in VERSION.txt's first line ("kit: <name> (release <tag>)"), or "" when it names none. Only
// letters, digits and . - + _ are taken (at most 32): a hand-edited file never puts anything else on the title.
std::string ReleaseOf(const std::string& version_first_line);
// "Squadala! v0.11.6" for release "v0.11.6" (a bare "0.11.6" gets its v), "Squadala!" for "".
std::string TitleFor(const std::string& release);
// The kit's release: VERSION.txt in `game_dir` or the nearest of its four parents that has one whose first line is
// a kit's ("kit: "; Windows: <kit>\hookshot\game\, Linux: <kit>/data/game/), or "".
std::string KitRelease(const std::filesystem::path& game_dir);
// TitleFor(KitRelease(game_dir)), read once (the first call's folder) and kept: the file changes only with an update,
// which restarts the game.
const std::string& TitleOf(const std::filesystem::path& game_dir);

// RGBA (straight alpha, rows top to bottom) as the bottom-up premultiplied BGRA a Windows icon wants.
std::vector<std::uint8_t> RgbaToIconBgra(const std::uint8_t* rgba, int width, int height);

// Once about a second from the every-frame hook; cheap when nothing is to be done. Does nothing off Windows.
// game_dir = the exe folder the bridge reports (CoopRuntime::GameDir()), where the icon png sits.
void Tick(const std::filesystem::path& game_dir);

}  // namespace CoopWindowBrand
