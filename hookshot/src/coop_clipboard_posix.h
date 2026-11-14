// SPDX-License-Identifier: GPL-3.0-or-later
// The desktop's clipboard on Linux (the invite and answer codes in the Multiplayer tab and the title menu),
// through the desktop's own tools: wl-copy / wl-paste on Wayland, xclip or xsel on X11. Steam Deck Game Mode
// has none of them: there the codes are shown in the tab and typed with Steam's keyboard. This is the module's
// one Linux clipboard: CoopFiles::ReadClipboard / SetClipboardText (coop_pause_files.cpp) call it; Windows has
// its own there, and this file is not built on Windows.
//
// The clipboard's text never reaches a shell: the shell runs fixed command strings, the copied text goes to
// the tool's standard input from a file, and what the tool prints is only read back from a pipe.
#pragma once

#include <optional>
#include <string>

namespace CoopClipboard {

// Puts `text` on the clipboard. False when no tool did it.
bool Copy(const std::string& text);

// The clipboard's text: at most 4096 bytes (at most 64 KiB is read from the tool, what was read stands),
// trailing line ends dropped. The tool is Wayland's (with $WAYLAND_DISPLAY), else xclip, else xsel (with
// $DISPLAY), each at most 2 s (`timeout`, when there is one). A tool that is there is believed even when it
// fails or is stopped: an empty clipboard makes wl-paste and xclip exit 1, and that is "" (empty). Nothing
// (std::nullopt) only when this desktop has none of the tools (or the tool could not be started).
std::optional<std::string> Paste();

}  // namespace CoopClipboard
