// SPDX-License-Identifier: GPL-3.0-or-later
// "Original game": one switch that makes the game as it came, as if this kit were not there (coop-options.ini
// `original_game = on`, the Co-op settings page's row).
//
// From the next start: the module stays off (no co-op, no pages or buttons of ours in the pause menu or on the title,
// no look on Link, no HUD, no networking, no code mods such as Save Profiles), and Longshot loads no mod at all: the
// mods list (coop-mods.list, hos_coop_mods.h) names every mod in the mods folder as off, the co-op pack too. The list
// as it was is kept beside it (coop-mods.list.before-original) and put back when the switch goes off again, so each
// mod comes back as the player had it.
//
// The way back stays in the game: while original, the module gives Longshot's F1 menu one tab, "Original game", with
// this switch alone. What Longshot itself was built with (our patches in longshot.exe) stays.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "hos_coop_menu.h"

namespace CoopOriginal {

inline constexpr const char* kOptionKey = "original_game";
inline constexpr const char* kBackupFile = "coop-mods.list.before-original";

// The switch in coop-options.ini's text.
bool OnIn(std::string_view options_ini);
// The text with the switch set (on) or taken out (off).
std::string With(std::string_view options_ini, bool on);

// The mods Longshot would load from this mods folder, by their list names (hos_coop_mods.h: the path under mods with
// '/'): each folder at the top but `contents`, and each one under contents/<title id>/. Folders starting with '_' or
// '-' (already left out) are not named. Sorted.
std::vector<std::string> ModNames(const std::filesystem::path& mods);
// The list that leaves every one of them out.
std::string OffList(const std::vector<std::string>& mods);

// Puts the mods folder's list in step with the switch: on, the list as it is kept once (a first time only: a second
// on does not overwrite the player's own list with ours) and every mod off; off, the kept list back (or none, when
// there was none). False: a file could not be written (`why` says which).
bool ApplyMods(const std::filesystem::path& mods, bool on, std::string& why);

// The F1 menu while original (the module off): one tab with the switch. `options` is the coop-options.ini to write.
class Tab {
public:
    void Open(std::filesystem::path options, std::filesystem::path mods);
    bool Shown() const { return !options_.empty(); }
    const char* Name() const { return "Original game"; }
    bool Page(HosCoopMenuPage* out);
    bool Input(int event, int arg, int arg2);
    bool on() const { return on_; }

private:
    void Flip();
    std::filesystem::path options_, mods_;
    bool on_ = true;        // the switch as the file has it now
    bool changed_ = false;  // flipped in this run: a restart finishes it
    std::string note_;      // a write that failed
    std::vector<HosCoopMenuRow> rows_;
    std::string status_;
};

}  // namespace CoopOriginal
