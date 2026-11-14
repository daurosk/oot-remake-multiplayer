// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer tab's own words, in the game's language: one table per
// language, as Longshot's own menu does it (gpu/vk/menu_i18n.h) and picked the
// same way (language.txt, or HOS_LANGUAGE when set). A text that is not in the
// table stays English, so a language can grow one line at a time.
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

// The table the game's language picks, or nullptr for English.
const Lang* Current();

// `en` in that language; `en` itself when the table has no line for it, and
// when `en` is built around something the table's shapes do not know.
std::string Tr(std::string_view en);

}  // namespace CoopI18n