// SPDX-License-Identifier: GPL-3.0-or-later
// This game's controller and keyboard, for two games on one PC
// (docs/play-lan.md, "Two players on one PC"): each game reads its own pad,
// or one reads a pad and the other the keyboard and mouse.
//
// The switches, in each kit's coop-options.ini (the kit's launcher, or
// CoopTitle::ApplyKitOptions, makes them variables):
//   LICHII_COOP_PAD      = auto | none | 1 | 2 | ...   (default auto)
//     auto: every controller, only while the window is in front (Hookshot's
//     own way; its Insert menu's "Controller in background" still counts);
//     n: only the n-th controller (the order Windows connected them in), also
//     while the window is behind the other game; none: no controller.
//   LICHII_COOP_KEYBOARD = focus | always | off        (default focus)
//     focus: keyboard and mouse while the window is in front; always: the
//     keys also behind other windows (no mouse); off: neither.
//
// Hookshot's patch 0015 does the reading (host/pad_sdl.c, host/nn_hid.c): two
// exports in the exe, which the module binds (coop_hookshot.cpp,
// UseInputSelect). Without them every pad counts, as before.
//
// The Settings row of the Multiplayer tab (pause-tab-b): ControllerRow() is
// its text, CycleController() its press; KeyboardRow() / CycleKeyboard() the
// same for the keyboard. Each cycle applies at once; the caller saves the new
// value as LICHII_COOP_PAD = PadValue(...) (LICHII_COOP_KEYBOARD =
// KeyboardValue(...)) in coop-options.ini. Thread-safe.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopInput {

constexpr int kAuto = 0;    // every controller (Hookshot's own way)
constexpr int kNone = -1;   // no controller
constexpr int kMaxPad = 8;  // Hookshot opens at most 8 (pad_sdl.c MAX_PADS)

enum class Keyboard { Focus = 0, Always = 1, Off = 2 };  // the numbers patch 0015 takes

struct Setting {
    int pad = kAuto;  // kAuto, kNone or 1..kMaxPad
    Keyboard keyboard = Keyboard::Focus;
    bool operator==(const Setting&) const = default;
};

// "auto", "none" or a number 1..kMaxPad (spaces and case ignored; "" = auto).
std::optional<int> ParsePad(std::string_view value);
// "focus", "always" or "off" ("" = focus).
std::optional<Keyboard> ParseKeyboard(std::string_view value);
// Both switches; a value that does not parse is the default, with a line in
// *warnings saying so.
Setting FromValues(std::string_view pad, std::string_view keyboard, std::vector<std::string>* warnings);
// What to save in coop-options.ini.
std::string PadValue(int pad);
std::string KeyboardValue(Keyboard keyboard);

// The row's order: auto, 1 .. connected, none, auto. A pad past `connected`
// (unplugged) goes on to none.
int NextPad(int pad, int connected);
Keyboard NextKeyboard(Keyboard keyboard);
// "Controller: all", "Controller: none (keyboard + mouse)",
// "Controller: 2 - Xbox Wireless Controller", "Controller: 2 (not connected)".
std::string ControllerLabel(int pad, std::string_view name);
// "Keyboard + mouse: window in front" / "Keyboard: also in background" / "Keyboard + mouse: off".
std::string KeyboardLabel(Keyboard keyboard);

// ---- At run time (the module): patch 0015's exports.
struct Exports {
    void (*select)(int pad, int keyboard) = nullptr;
    int (*pad_name)(int n, char* name, std::size_t cap) = nullptr;  // 1 when the n-th pad is connected
};
// Binds the exports (both null: this Hookshot has no patch 0015).
void Bind(const Exports& exports);
bool Available();
// This game's setting from now on (remembered when patch 0015 is missing,
// so the row still shows it).
void Apply(const Setting& setting);
Setting Current();
// The n-th controller's name ("" when none is connected there, or no patch).
std::string PadName(int n);
// The highest n with a controller connected (0 none).
int Connected();

std::string ControllerRow();
std::string KeyboardRow();
Setting CycleController();
Setting CycleKeyboard();

}  // namespace CoopInput
