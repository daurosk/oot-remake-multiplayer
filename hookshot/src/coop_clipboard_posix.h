// SPDX-License-Identifier: GPL-3.0-or-later
// The desktop's clipboard on Linux (the invite and answer codes in the Multiplayer tab and the title menu),
// through the desktop's own tools: wl-copy / wl-paste on Wayland, xclip or xsel on X11; and, for a paste on the
// game window's own thread (a Ctrl+V), through the game's SDL, which needs none of them (Steam Deck Game Mode
// has no tool: there SDL's clipboard is the one, else the codes are typed with Steam's keyboard). This is the module's
// one Linux clipboard: CoopFiles::ReadClipboard / SetClipboardText (coop_pause_files.cpp) call it; Windows has
// its own there, and this file is not built on Windows.
//
// The clipboard's text never reaches a shell: the shell runs fixed command strings, the copied text goes to
// the tool's standard input from a file, and what the tool prints is only read back from a pipe.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace CoopClipboard {

// Puts `text` on the clipboard. False when no tool did it. Waits for the tool (up to 2 s): never on the game's
// thread, which then stops drawing (Steam Deck Game Mode hosting fell to a few frames a second): CopyLater there.
bool Copy(const std::string& text);

// The same, off the calling thread: the tool runs on the clipboard's own thread (one job at a time, in the order
// given), and the result is taken later with Take(ticket). `keep` false: nobody asks, the result is not kept.
std::uint64_t CopyLater(std::string text, bool keep = true);
// The tools' paste (as Paste, without SDL's clipboard: that one is read now with PasteNow), off the calling thread.
std::uint64_t PasteLater();
// SDL's clipboard when this is SDL's thread (OnSdlThread) and it has text (trimmed as Paste); else nothing, at once.
std::optional<std::string> PasteNow();
// A job's result once it is done (and then forgotten); nothing while it runs. Copy: `ok`. Paste: `text`, as Paste.
struct Done {
    bool ok = false;
    std::optional<std::string> text;
};
std::optional<Done> Take(std::uint64_t ticket);

// The clipboard's text: at most 4096 bytes (at most 64 KiB is read from the tool, what was read stands),
// trailing line ends dropped. The tool is Wayland's (with $WAYLAND_DISPLAY), else xclip, else xsel (with
// $DISPLAY), each at most 2 s (`timeout`, when there is one). A tool that is there is believed even when it
// fails or is stopped: an empty clipboard makes wl-paste and xclip exit 1, and that is "" (empty). Nothing
// (std::nullopt) only when this desktop has none of the tools (or the tool could not be started).
std::optional<std::string> Paste();

// (on SDL's thread, OnSdlThread: SDL's clipboard first, PickPaste)

// A function of the game's SDL by name (SDL3's library, else SDL2's, as the game loaded it: Longshot loads it
// RTLD_LOCAL, so dlsym(RTLD_DEFAULT) never finds it); nullptr without SDL or without that function.
void* GameSdl(const char* name);

// While one lives, the calling thread is SDL's own: the game window's event loop, where Longshot hands the module
// its keys (hos_coop_key / hos_coop_char) on Linux. SDL's clipboard is read only there (SDL wants the window's
// thread): there Paste tries SDL first. Nested ones are fine.
class OnSdlThread {
public:
    OnSdlThread();
    ~OnSdlThread();
    OnSdlThread(const OnSdlThread&) = delete;
    OnSdlThread& operator=(const OnSdlThread&) = delete;

private:
    bool was_;
};

// What a paste gives: SDL's text when it has some (std::nullopt: SDL not read or not there); else the tools'
// (`tools`, asked only then); else SDL's "" (SDL answers "" for an empty clipboard and for one it cannot read,
// so a tool's answer wins over it). Nothing only when neither could read a clipboard.
std::optional<std::string> PickPaste(const std::optional<std::string>& sdl,
                                     const std::function<std::optional<std::string>()>& tools);

}  // namespace CoopClipboard
