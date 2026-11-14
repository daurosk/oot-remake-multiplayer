// SPDX-License-Identifier: GPL-3.0-or-later
// The desktop's clipboard on Linux (the invite and answer codes in the Multiplayer tab), through
// the desktop's own tools: wl-copy / wl-paste on Wayland, xclip or xsel on X11. Steam Deck Game
// Mode has none of them: there the codes are shown in the tab and typed with Steam's keyboard.
// Windows uses its own clipboard (coop_pause_files.cpp); this file is not built there.
#pragma once

#include <string>

namespace CoopClipboard {

// Puts `text` on the clipboard. False when no tool did it.
bool Copy(const std::string& text);
// The clipboard's text (at most 4096 bytes, trailing line ends dropped); "" when there is none.
std::string Paste();

}  // namespace CoopClipboard
