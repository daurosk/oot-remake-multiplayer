// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer tab's words in the game's language (hookshot/src/coop_i18n.h):
// the table's lines, the lines the tab builds around a number, a name or a
// code (Tr()'s shapes), and the English a table does not have.
#include <doctest/doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
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

TEST_CASE("coop i18n: every shape in both tables keeps its {}") {
    // Tr() fills a shape's {} with what the tab put there; a translated shape
    // with fewer braces would drop it, and one with more would leave a stray.
    for (const char* code : {"zh-Hans", "zh-Hant"}) {
        const CoopI18n::Lang& lang = *ByCode(code);
        for (int i = 0; i < lang.count; ++i) {
            const std::string en = lang.entries[i].en;
            CHECK_MESSAGE(Braces(lang.entries[i].text) == Braces(en), code, ": ", en);
        }
    }
}

TEST_CASE("coop i18n: Tr fills a built line") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    CHECK(CoopI18n::Tr(hans, "Multiplayer") == "联机");
    CHECK(CoopI18n::Tr(hans, "Hosting: 3 players") == "房间中：3 人");
    // The middle is data: a name is copied, not looked up and not read as a format.
    CHECK(CoopI18n::Tr(hans, "Join Saria again") == "再次加入 Saria");
    CHECK(CoopI18n::Tr(hans, "Join {}%s again") == "再次加入 {}%s");
    // A shape whose middle is a word of the table too.
    CHECK(CoopI18n::Tr(hans, "No invite code yet.") == "还没有邀请码。");
    CHECK(CoopI18n::Tr(hans, "Getting Saria's save: don't load a save yet...") == "正在获取 Saria 的存档：先别载入存档…");
    // A line the table does not have, and English: as they are.
    CHECK(CoopI18n::Tr(hans, "Not a line of the tab") == "Not a line of the tab");
    CHECK(CoopI18n::Tr(nullptr, "Multiplayer") == "Multiplayer");
    CHECK(CoopI18n::Tr(hans, "") == "");
}

TEST_CASE("coop i18n: a language code picks its table") {
    // Longshot's menu codes (MenuLanguageCode) and language.txt's,
    // any case, and the first part of a longer one ("zh-Hans-CN").
    CHECK(CoopI18n::ForCode("zh-Hans") == ByCode("zh-Hans"));
    CHECK(CoopI18n::ForCode("ZH-hant") == ByCode("zh-Hant"));
    CHECK(CoopI18n::ForCode("zh-Hans-CN") == ByCode("zh-Hans"));
    CHECK(CoopI18n::ForCode("zh") == nullptr);   // (no table is plain zh)
    CHECK(CoopI18n::ForCode("zh-Hansx") == nullptr);
    CHECK(CoopI18n::ForCode("en") == nullptr);   // (English: the text as it is)
    CHECK(CoopI18n::ForCode("ja") == nullptr);   // (a language this table does not have)
    CHECK(CoopI18n::ForCode("") == nullptr);
}

TEST_CASE("coop i18n: the Menu language row from longshot_options.ini") {
    // Longshot writes menu_language=N (0 Auto, else its kLangs[N - 1]) with
    // the other rows; a missing line is Auto, and the last line wins.
    CHECK(CoopI18n::MenuLanguageSetting("") == 0);
    CHECK(CoopI18n::MenuLanguageSetting("vsync=1\nmenu_language=13\nfps=60\n") == 13);
    CHECK(CoopI18n::MenuLanguageSetting("menu_language=14\r\n") == 14);
    CHECK(CoopI18n::MenuLanguageSetting("menu_language=13\nmenu_language=0\n") == 0);
    CHECK(CoopI18n::MenuLanguageSetting("game_menu_language=13\n") == 0);
    CHECK(CoopI18n::MenuLanguageSetting("menu_language=x\n") == 0);
    CHECK(CoopI18n::MenuLanguageCode(0) == nullptr);   // (Auto: the game's language)
    CHECK(std::string(CoopI18n::MenuLanguageCode(1)) == "en");
    CHECK(std::string(CoopI18n::MenuLanguageCode(13)) == "zh-Hans");
    CHECK(std::string(CoopI18n::MenuLanguageCode(14)) == "zh-Hant");
    CHECK(std::string(CoopI18n::MenuLanguageCode(99)) == "en");
    CHECK(CoopI18n::ForCode(CoopI18n::MenuLanguageCode(13)) == ByCode("zh-Hans"));
    CHECK(CoopI18n::ForCode(CoopI18n::MenuLanguageCode(11)) == nullptr);   // (ja: no table, English)
}

namespace {

// The module's sources as one text, adjacent string literals joined, so a key
// split over lines in the code is found whole.
std::string Sources() {
    namespace fs = std::filesystem;
    const fs::path repo = fs::path(COOP_TEST_SOURCE_DIR) / "..";
    std::string all;
    for (const fs::path& dir : {repo / "hookshot" / "src", repo / "lichii" / "hook", repo / "coop" / "host" / "src",
                               repo / "coop" / "host" / "include" / "coop" / "host"}) {
        for (const auto& e : fs::directory_iterator(dir)) {
            const std::string name = e.path().filename().string();
            const std::string ext = e.path().extension().string();
            if (name.rfind("coop_i18n", 0) == 0 || (ext != ".cpp" && ext != ".h" && ext != ".hpp")) {
                continue;
            }
            std::ifstream in(e.path(), std::ios::binary);
            std::ostringstream text;
            text << in.rdbuf();
            all += std::regex_replace(text.str(), std::regex("\"[ \\t\\r]*\\n[ \\t]*\""), "");
            all += '\n';
        }
    }
    return all;
}

std::string Escaped(const std::string& s) {   // (as the key is written in C: \" and \\)
    std::string out;
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

}  // namespace

TEST_CASE("coop i18n: every key is still a line of the module") {
    // A text changed in the code no longer finds its line, and that language
    // shows English there: the table is re-keyed with it. (Shapes, with {},
    // are matched by their ends at run time and are left out here.)
    // Lines of branches that come in the same release (join-continue: the
    // join's chat lines) are listed until they are in.
    const char* const coming[] = {
        "Host save loading, don't load a save.",
        "Success, load any save.",
        "Success, load any save for changes to occur.",
        "That save loaded before the host's save was in: go back to the title and load a save after Success.",
        "The host has no save yet: New Game starts one, Continue plays your own.",
    };
    const std::string sources = Sources();
    REQUIRE(sources.size() > 100000);
    const CoopI18n::Lang& hans = *ByCode("zh-Hans");
    for (int i = 0; i < hans.count; ++i) {
        const std::string en = hans.entries[i].en;
        if (Braces(en) > 0) {
            continue;
        }
        bool later = false;
        for (const char* c : coming) {
            later = later || en == c;
        }
        if (!later) {
            const bool found = sources.find("\"" + Escaped(en)) != std::string::npos ||
                               sources.find(Escaped(en) + "\"") != std::string::npos;
            CHECK_MESSAGE(found, en);
        }
    }
}
