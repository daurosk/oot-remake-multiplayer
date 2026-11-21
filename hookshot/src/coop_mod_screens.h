// SPDX-License-Identifier: GPL-3.0-or-later
// The game's menu screens as code mods see them (coop_mod_api.h version 2, docs/mods.md "Menu screens"): which screen
// an update is (its class's vtable, read from main) and its focused button's name. Read with a reader the caller gives
// (the game's memory in the module, a fake in the tests). No game calls here: the module makes the one call
// (kButtonDecide) on the screen's update (coop_mods_runtime.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace CoopModScreens {

// main offsets (build e0f3ccfa, main at 0x80020000), read from main: each class's name getter, then its factory's
// constructor (the same three as patch 0028's hos_skip_title.h).
inline constexpr std::uint64_t kTitleVtable = 0x4bf06e8;         // UITitleScreen
inline constexpr std::uint64_t kLoadListVtable = 0x4beeba0;      // UISystemLoadListScreen
inline constexpr std::uint64_t kLoadQuestionVtable = 0x4bef840;  // UISystemSaveLoadScreen
// Button::Decide(button, 0) -> bit 0: chosen. The call the game's screens make for a button chosen without its own
// input (main+0x3405280, +0x3405930); patch 0028's Skip title screen chooses with it too.
inline constexpr std::uint64_t kButtonDecide = 0x3771db0;

// A screen's buttons, as the game's screen walks them (patch 0028, host/oot_autoload.c focused_button): the focus
// manager at *(screen+32)+96 (set while the screen takes input), the list head at +136, its first node at +144, an
// entry's offset from its node at +156, the entry's button at +24; a button's state at +56 (0 not focused, 4 and 5
// decided).
inline constexpr std::uint64_t kScreenFocus = 32, kFocusManager = 96;
inline constexpr std::uint64_t kListHead = 136, kListFirst = 144, kListEntryOffset = 156, kEntryButton = 24;
inline constexpr std::uint64_t kButtonState = 56;
// A button's pane name: a pointer to its text at +0x18, ended by a 0, as the game's own screen request passes it on
// (main+0x3379160: x1 = *(button+0x18)). Seen in a game on the title: Pa_Button_00 (Continue) to Pa_Button_03, in the
// order of the title's buttons array (CoopTitle's +0x2d0).
inline constexpr std::uint64_t kNameText = 0x18;
// A button's flags at +0x40: Button::Decide says no unless both bits of kChoosable are set. Seen in a game: the
// title's Continue has 0x3b while the title comes in (its opening scene, the menu fading in) and 0x3f once it takes
// input.
inline constexpr std::uint64_t kButtonFlags = 0x40;
inline constexpr std::uint32_t kChoosable = 0x14;
inline constexpr std::size_t kNameMax = 63;
// The most buttons a screen's list is walked for (a list that never comes back to its head stops there). The pause
// menu's pages screen holds every page's buttons, well over 64 (seen in a run, 2026-11-19: System's five come after the
// Collection's and Equipment's), so the walk goes far enough for them.
inline constexpr int kMaxButtons = 256;

// (screen, const char* name) -> bit 0: the sound started. A screen's own menu sound by name: its xlink user at
// [screen+0x198]+8, then xlink2's searchAndEmit(name) ("searchAndEmit(%s)", main+0x43e400). Read from main and seen in
// a run, 2026-11-19: the title's Continue starts "Pa_Button_00_decide" and "button_decide", a cursor step "button_on",
// the pause menu's tabs "mc_TabToRight" and "mc_TabToLeft", all on that user.
inline constexpr std::uint64_t kScreenSound = 0x43e400;
inline constexpr std::size_t kSoundNameMax = 63;

// A sound name a mod may give: 1 to kSoundNameMax letters, digits or _.
bool SoundName(const char* name);

// Reads n bytes at a guest address; false when not mapped.
using Reader = std::function<bool(std::uint64_t address, void* out, std::size_t n)>;

// COOP_MOD_SCREEN_* of a screen whose vtable is at main+vtable_offset.
std::uint32_t KindOf(std::uint64_t vtable_offset);

// The screen's one focused button (0: none, more than one, or the screen takes no input now).
std::uint64_t FocusedButton(const Reader& read, std::uint64_t screen);

// A button's pane name ("" when it cannot be read or is not printable).
std::string ButtonName(const Reader& read, std::uint64_t button);

// A button's focus state byte as FocusedButton reads it (0: none; 4 and 5 are not focus), for traces.
std::uint8_t FocusState(const Reader& read, std::uint64_t button);

// Whether a button takes a choice now (its flags have kChoosable). False for no button or one that cannot be read.
bool Choosable(const Reader& read, std::uint64_t button);

// Each button of a screen's list, in the list's order (at most kMaxButtons). Unlike FocusedButton it does not ask
// whether the screen takes input now.
std::vector<std::uint64_t> Buttons(const Reader& read, std::uint64_t screen);

// A button's hit box in the layout's units (3840 x 2160 about the screen's centre, y up), where the game puts it now:
// its first box (button+0x58), whose global matrix is at +0x28 (3 x 4 by column: the scales at [0] and [4], the
// translation at [9] and [10]; all 0 while its page is not shown) and its rectangle at +0x84 (x, y, w, h about the
// box's origin). Longshot's mouse reads the boxes the same way (host/oot_menu.c, button_rect). Nothing when the button
// is not shown or cannot be read.
inline constexpr std::uint64_t kHitBox = 0x58, kBoxMatrix = 0x28, kBoxRect = 0x84;
struct Box {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};
std::optional<Box> HitBox(const Reader& read, std::uint64_t button);

}  // namespace CoopModScreens
