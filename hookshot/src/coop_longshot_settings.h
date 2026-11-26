// SPDX-License-Identifier: GPL-3.0-or-later
// Longshot's own settings (its Insert menu's rows: graphics, audio, gameplay, controls), for the pause menu's Settings
// tab: patch 0032 hands the module the control bridge's two calls (HosCoopApi::setting, setting_set). Listed by index
// until the first that gives none; set by key as the menu's Left and Right set them (clamped to the row's range, the
// Quality preset to Custom, a restart noted, the options file saved). An older Hookshot without them: none.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "hos_coop_api.h"

namespace CoopLongshotSettings {

struct Setting {
    std::string key;    // its line in longshot_options.ini
    std::string name;   // as the menu names it
    std::string text;   // its value as the menu shows it
    std::string tab;    // the menu's tab (Graphics, Audio, ...)
    int value = 0, lo = 0, hi = 0, step = 1;
    bool restart = false;   // applies at the next start
    bool fixed = false;     // nothing to choose on this PC
};

// The game's table (coop_hookshot.cpp's Start); nullptr: none.
void Init(const HosCoopApi* api);
// Whether this Hookshot hands them (patch 0032: the table is long enough and both calls are there).
bool Available();
// The index-th row, nothing past the last (or without them).
std::optional<Setting> At(int index);
// Every row, in the menu's order (at most kMaxRows).
inline constexpr int kMaxRows = 512;
std::vector<Setting> All();
// The game's answers to a set (HosCoopApi::setting_set).
inline constexpr int kSet = 1, kNoSuchRow = 0, kFixed = -1, kUnreachable = -2;
// That key set to `value`: the row as it is after, nothing when it was not set: no such key (or without them), nothing
// to choose on this PC (kFixed), or no step of a list row reaches the value (kUnreachable, left as it was). *code, when
// given, gets the game's answer (kNoSuchRow without them).
std::optional<Setting> Set(const std::string& key, int value, int* code = nullptr);

// One row of the game's table as the module keeps it (a text field without its 0 at the end is cut at its size).
Setting FromGame(const HosCoopSetting& s);

}  // namespace CoopLongshotSettings
