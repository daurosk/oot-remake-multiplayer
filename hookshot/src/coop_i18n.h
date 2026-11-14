// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer tab's own words, in the menu's language: one table per
// language, as Longshot's own menu does it (gpu/vk/menu_i18n.h). The language
// is the one that menu shows now (its Menu language row, menu_language= in
// longshot_options.ini, which it writes on every change; Auto: the game's
// own, HOS_LANGUAGE or language.txt), so the tabs match the menu they are
// drawn in and its glyphs. A text that is not in the table stays English, so a
// language can grow one line at a time.
//
// Tr() also knows the lines the tab builds around a number, a name or a code
// ("Hosting: 3 players", "Join Saria again", "Copied: abc"): those are looked
// up by their fixed ends, and the middle is put back where the table's line
// has its {}. So every string the module draws may go through Tr() as it is.
#pragma once

#include <string>
#include <string_view>

namespace CoopI18n {

struct Entry {
    const char* en;
    const char* text;
};
struct Lang {
    const char* code;   // language.txt's code, or its first part ("zh-Hans", "zh")
    const char* name;   // the language's own name
    const Entry* entries;
    int count;
};

extern const Lang kLangs[];
extern const int kLangCount;

// The table the menu's language picks now, or nullptr for English (asked
// each time: a change of the Menu language row shows at once).
const Lang* Current();

// The Menu language row's value in longshot_options.ini's text (0 Auto), -1
// when the text has no such line (empty, or cut short while Longshot writes
// it); the value after a read, `last` kept when the read had none (`good`
// false: read again next time); and the language code a value stands for
// (nullptr for Auto).
int MenuLanguageSetting(std::string_view ini);
int MenuLanguageAfterRead(int last, std::string_view ini, bool* good);
const char* MenuLanguageCode(int setting);

// The table for a language code (any case; "zh-Hans-CN" takes "zh-Hans"), or
// nullptr for English and codes with no table.
const Lang* ForCode(std::string_view code);

// `en` in that language; `en` itself when the table has no line for it, and
// when `en` is built around something the table's shapes do not know.
std::string Tr(std::string_view en);
// The same in `lang` (nullptr: English).
std::string Tr(const Lang* lang, std::string_view en);

}  // namespace CoopI18n