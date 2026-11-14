// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer tab's words in the game's language (hookshot/src/coop_i18n.h):
// the table's lines, the lines the tab builds around a number, a name or a
// code (Tr()'s shapes), and the English a table does not have.
#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

#include "coop_i18n.h"

namespace {

// The part of a language's table the tests look at. Current() reads the
// language once per process, so a test says what a table holds instead of
// switching languages under it.
const CoopI18n::Lang* ByCode(const char* code) {
    for (int i = 0; i < CoopI18n::kLangCount; ++i) {
        if (std::string(CoopI18n::kLangs[i].code) == code) {
            return &CoopI18n::kLangs[i];
        }
    }
    return nullptr;
}

int Braces(const std::string& s) {
    int n = 0;
    for (std::size_t i = 0; i + 1 < s.size(); ++i) {
        n += s[i] == '{' && s[i + 1] == '}';
    }
    return n;
}

// A key in a language's table ("" when the table has no line for it).
std::string Line(const CoopI18n::Lang& lang, const char* en) {
    for (int i = 0; i < lang.count; ++i) {
        if (std::string(en) == lang.entries[i].en) {
            return lang.entries[i].text;
        }
    }
    return {};
}

}  // namespace

TEST_CASE("coop i18n: both tables hold the same lines") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    REQUIRE(hans != nullptr);
    REQUIRE(hant != nullptr);
    CHECK(hans->entries != nullptr);
    CHECK(hant->entries != nullptr);
    CHECK(hans->count > 150);
    CHECK(hans->count == hant->count);   // (a line in one is a line in both)

    // English has a name and no table: every line stays as it is.
    const CoopI18n::Lang* en = ByCode("en");
    REQUIRE(en != nullptr);
    CHECK(en->entries == nullptr);
    CHECK(en->count == 0);
}

TEST_CASE("coop i18n: a page's own words") {
    const CoopI18n::Lang hans = *ByCode("zh-Hans");
    CHECK(Line(hans, "Multiplayer") == "联机");
    CHECK(Line(hans, "Host a game") == "创建房间");
    CHECK(Line(hans, "Join a game") == "加入房间");
    CHECK(Line(hans, "Local co-op") == "本地双人");
    CHECK(Line(hans, "Settings") == "设置");
    CHECK(Line(hans, "Commands") == "命令");
    // The prompt strip's words, and the one that must not read like the D-pad's.
    CHECK(Line(hans, "Choose") == "执行");
    CHECK(Line(hans, "Change") == "更改");
    CHECK(Line(hans, "Back") == "返回");
}

TEST_CASE("coop i18n: a built line's shape keeps its {}") {
    // Tr() fills a shape's {} with what the tab put there; a translated shape
    // with fewer braces would drop it, and one with more would leave a stray.
    const CoopI18n::Lang hans = *ByCode("zh-Hans");
    const char* const shapes[] = {
        "Hosting: {} players",
        "Co-op is on: {} players",
        "Join {} again",
        "Copied: {}",
        "No {} yet.",
        "{} friend(s) joined.",
        "Getting {}'s save...",
        "Loading {}'s save...",
        "Friends on your {} paste this line in Join (address and password):",
        "{}'s save did not arrive: Continue plays your own save, New Game starts a new one.",
        "{} has no save yet: New Game starts one, Continue plays your own.",
        "Press Continue to play {}'s save.",
        "Press {} in the game to open the chat box; Enter sends, Esc closes. A line that starts with / is a "
        "command, not chat. Another key: LICHII_COOP_CHAT_KEY in coop-options.ini.",
        "Multiplayer: {}",
    };
    for (const char* en : shapes) {
        const std::string text = Line(hans, en);
        REQUIRE_MESSAGE(!text.empty(), en);
        CHECK_MESSAGE(Braces(text) == Braces(en), en);
    }
}

TEST_CASE("coop i18n: a language the game may say") {
    // language.txt's own codes, and the first part of one ("zh-Hans-CN").
    CHECK(ByCode("zh-Hans") != nullptr);
    CHECK(ByCode("zh-Hant") != nullptr);
    CHECK(ByCode("en") != nullptr);
    CHECK(ByCode("ja") == nullptr);   // (a language this table does not have)
}