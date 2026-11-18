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

TEST_CASE("coop i18n: the boss room's leave notices around a name (#50)") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    CHECK(CoopI18n::Tr(hans, "Saria took the blue warp") == "Saria 进入了蓝色传送光");
    CHECK(CoopI18n::Tr(hant, "Saria left the boss room") == "Saria 離開了 Boss 房間");
    // A name that starts like another shape's fixed start is still this line.
    CHECK(CoopI18n::Tr(hans, "Player1 took the blue warp") == "Player1 进入了蓝色传送光");
    CHECK(CoopI18n::Tr(nullptr, "Saria took the blue warp") == "Saria took the blue warp");
}

TEST_CASE("coop i18n: the chat's join and leave notices around a name") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // The player's name is the shape's middle; the rest is the notice. Without a shape for
    // these two, the whole line missed the table and stayed English ("Saria left").
    CHECK(CoopI18n::Tr(hans, "Saria joined") == "Saria 加入了");
    CHECK(CoopI18n::Tr(hans, "Saria left") == "Saria 离开了");
    CHECK(CoopI18n::Tr(hant, "Saria left") == "Saria 離開了");
    // " left the boss room" is its own line, not "<name> left" plus a tail.
    CHECK(CoopI18n::Tr(hans, "Saria left the boss room") == "Saria 离开了 Boss 房间");
    // A name that reads like the end of a line is still the name.
    CHECK(CoopI18n::Tr(hans, "left joined") == "left 加入了");
    // A longer line that happens to end " left" is not "<name> left": the
    // teleport's stop notice keeps its own sentence (coop_teleport.cpp).
    CHECK(CoopI18n::Tr(hans, "teleport: stopped: Saria left") == "传送：已停下：Saria 离开了");
    CHECK(CoopI18n::Tr(hant, "teleport: stopped: Saria left") == "傳送：已停下：Saria 離開了");
    CHECK(CoopI18n::Tr(hans, "teleport: stopped: you left Hyrule Field") ==
          "传送：已停下：你离开了 海拉鲁平原");
    // The chat's own notices with a number or a note (the player saw "joined as
    // player 2" in English: the table's key had no shape to reach it).
    CHECK(CoopI18n::Tr(hans, "joined as player 2") == "已加入：玩家 2");
    CHECK(CoopI18n::Tr(hans, "reconnected as player 2") == "已重新连上：玩家 2");
    CHECK(CoopI18n::Tr(hant, "joined as player 2") == "已加入：玩家 2");
    // The refusal arrives as "server refused: <reason>" (coop_hook.cpp puts
    // note_, which already starts "refused: ", after "server ").
    CHECK(CoopI18n::Tr(hans, "server refused: wrong join password: ask the host for it, "
                              "then start Join.bat again and type it") ==
          "服务器：被拒绝：加入密码不对：向房主要密码，然后重新启动 Join.bat 输入");
    CHECK(CoopI18n::Tr(hans, "connection lost, reconnecting") == "连接断了，正在重连");
}

TEST_CASE("coop i18n: the difficulty line fits the chat's room") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const char* en =
        "Difficulty: Automatic (2 players): life 140%, damage 120%, enemies 125%; bosses: life 200%, damage 120%";
    const std::string zh = CoopI18n::Tr(hans, en);
    CHECK(zh == "难度 自动（2人）生命140% 伤害120% 敌人125% · Boss生命200% 伤害120%");
    // The room is the game's own line's width (Elided() in coop_draw.cpp cuts what is wider, and
    // the HUD showed "伤..." before this): a CJK glyph is about two Latin ones, so the Chinese
    // line must stay well under the English one.
    const auto width = [](const std::string& s) {
        float w = 0.0f;
        for (std::size_t i = 0; i < s.size();) {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            const int bytes = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
            w += bytes == 1 ? 1.0f : 2.0f;
            i += static_cast<std::size_t>(bytes);
        }
        return w;
    };
    CHECK(width(zh) <= width(en) * 0.75f);
}

TEST_CASE("coop i18n: the chat's refusal line says its reason too") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // The chat writes "refused: <reason>" (coop_lobby_game.cpp) and the tab keeps the part before
    // ':' (RefusedLine). The reason was the shape's middle read as data, so the line read
    // "被拒绝：wrong join password: ask the host...": the reason is a line of the table now.
    CHECK(CoopI18n::Tr(hans, "refused: server full") == "被拒绝：房间满了");
    CHECK(CoopI18n::Tr(hans, "refused: wrong join password: ask the host for it, then start Join.bat again and type it") ==
          "被拒绝：加入密码不对：向房主要密码，然后重新启动 Join.bat 输入");
    CHECK(CoopI18n::Tr(hant, "refused: the host banned you from this game: you cannot join it again until the host stops hosting") ==
          "被拒絕：房主在這個房間封禁了你：房主停止建立房間後，你才能再加入");
    // A reason no table has: the line's start still reads in the language.
    CHECK(CoopI18n::Tr(hans, "refused: something new") == "被拒绝：something new");
}

TEST_CASE("coop i18n: the pickup notices' place and item, one set of brackets") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // A place's own line: "got the map (Deku Tree)" must not read as the bare
    // " got the " with "map (Deku Tree)" as an item name.
    CHECK(CoopI18n::Tr(hans, "Saria got the map (Deku Tree)") == "Saria 拿到了地图（德库大树）");
    CHECK(CoopI18n::Tr(hans, "Saria got the compass (Deku Tree)") == "Saria 拿到了指南针（德库大树）");
    CHECK(CoopI18n::Tr(hans, "Saria got the Boss Key (Deku Tree)") == "Saria 拿到了 Boss 钥匙（德库大树）");
    CHECK(CoopI18n::Tr(hant, "Saria got the map (Deku Tree)") == "Saria 拿到了地圖（德庫大樹）");
    // The token line: the table's key has the brackets, so the line gets one
    // pair, not two ("(12))").
    CHECK(CoopI18n::Tr(hans, "Saria got a Gold Skulltula token (12)") ==
          "Saria 得到一枚金色骷髅蜘蛛徽章（12）");
    CHECK(CoopI18n::Tr(hant, "Saria got a Gold Skulltula token (12)") ==
          "Saria 得到一枚金色骷髏蜘蛛徽章（12）");
    // The heart container's own end is part of the line's table form.
    CHECK(CoopI18n::Tr(hans, "Saria got a Heart Container - your max health went up") ==
          "Saria 得到一颗心之容器 —— 最大生命值提升了");
    // The plain item line still reads as it did.
    CHECK(CoopI18n::Tr(hans, "Saria got the Silver Scale - you got one too!") ==
          "Saria 拿到了银鳞 —— 你也得到了一个！");
}

TEST_CASE("coop i18n: a language code picks its table") {
    // Longshot's menu codes (MenuLanguageCode) and language.txt's,
    // any case, and the first part of a longer one ("zh-Hans-CN").
    CHECK(CoopI18n::ForCode("zh-Hans") == ByCode("zh-Hans"));
    CHECK(CoopI18n::ForCode("ZH-hant") == ByCode("zh-Hant"));
    CHECK(CoopI18n::ForCode("zh-Hans-CN") == ByCode("zh-Hans"));
    CHECK(CoopI18n::ForCode("zh") == nullptr);   // (no table is plain zh)
    // Chinese named by region with no script in the code ("zh-CN"):
    // the regions stand for the scripts there:
    // the mainland, Singapore and Malaysia are Simplified, Taiwan, Hong Kong
    // and Macau Traditional, and any other "zh-..." is taken as Simplified.
    CHECK(CoopI18n::ForCode("zh-CN") == ByCode("zh-Hans"));
    CHECK(CoopI18n::ForCode("zh-SG") == ByCode("zh-Hans"));
    CHECK(CoopI18n::ForCode("zh-TW") == ByCode("zh-Hant"));
    CHECK(CoopI18n::ForCode("zh-HK") == ByCode("zh-Hant"));
    CHECK(CoopI18n::ForCode("zh-MO") == ByCode("zh-Hant"));
    CHECK(CoopI18n::ForCode("en") == nullptr);   // (English: the text as it is)
    CHECK(CoopI18n::ForCode("ja") == nullptr);   // (a language this table does not have)
    // Other languages' regions are never Chinese (Longshot's menu codes
    // pt-BR and es-419, a language.txt's en-US).
    CHECK(CoopI18n::ForCode("en-US") == nullptr);
    CHECK(CoopI18n::ForCode("pt-BR") == nullptr);
    CHECK(CoopI18n::ForCode("es-419") == nullptr);
    CHECK(CoopI18n::ForCode("ja-JP") == nullptr);
    CHECK(CoopI18n::ForCode("ZH-tw") == ByCode("zh-Hant"));
    CHECK(CoopI18n::ForCode("") == nullptr);
}

TEST_CASE("coop i18n: the Menu language row from longshot_options.ini") {
    // Longshot writes menu_language=N (0 Auto, else its kLangs[N - 1]) with
    // the other rows; the last line wins; no line: -1 (a read mid-write).
    CHECK(CoopI18n::MenuLanguageSetting("") == -1);
    CHECK(CoopI18n::MenuLanguageSetting("vsync=1\nmenu_language=13\nfps=60\n") == 13);
    CHECK(CoopI18n::MenuLanguageSetting("menu_language=14\r\n") == 14);
    CHECK(CoopI18n::MenuLanguageSetting("menu_language=13\nmenu_language=0\n") == 0);
    CHECK(CoopI18n::MenuLanguageSetting("game_menu_language=13\n") == -1);
    CHECK(CoopI18n::MenuLanguageSetting("menu_language=x\n") == -1);
    // A read while Longshot rewrites the file (empty, or cut before the line)
    // keeps the last value and is not taken as read; a whole file is.
    bool good = true;
    CHECK(CoopI18n::MenuLanguageAfterRead(13, "", &good) == 13);
    CHECK_FALSE(good);
    CHECK(CoopI18n::MenuLanguageAfterRead(14, "vsync=1\nresolution=2\nmenu_lan", &good) == 14);
    CHECK_FALSE(good);
    CHECK(CoopI18n::MenuLanguageAfterRead(14, "vsync=1\nmenu_language=1", &good) == 14);   // (cut inside 13)
    CHECK_FALSE(good);
    CHECK(CoopI18n::MenuLanguageAfterRead(13, "vsync=1\nmenu_language=0\n", &good) == 0);
    CHECK(good);
    CHECK(CoopI18n::MenuLanguageAfterRead(0, "menu_language=14\n", nullptr) == 14);
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
                               repo / "coop" / "net" / "src",
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


// The names a pickup notice says are the module's own list (coop/host/src/notify.cpp's
// kItems, kSongs, kDungeons) plus what its Spell() makes out of an id it does not
// know ("Pocket Cucco"). The first three are a line of that file, so Sources() has
// them; the ones Spell() builds are not a literal anywhere, so the check for "every
// key is still a line of the module" leaves them out by name.
std::string NotifyNames() {
    namespace fs = std::filesystem;
    const fs::path repo = fs::path(COOP_TEST_SOURCE_DIR) / "..";
    std::ifstream in(repo / "coop" / "host" / "src" / "notify.cpp", std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool Spelled(const std::string& key) {
    // a name Spell() can make: words, or a capitalised single word
    return key.find(' ') != std::string::npos || (key.size() > 2 && key[0] >= 'A' && key[0] <= 'Z');
}
}  // namespace


TEST_CASE("coop i18n: a place's name, and a name the table does not have") {
    // places.json's names (lichii/hook/coop_hud_places.h) are the game's data, not lines
    // of the module, so they are a table of their own; Tr() must not have them, or the
    // check that every key is still a line of the module would fail on a name it cannot see.
    const CoopI18n::Lang& hans = *ByCode("zh-Hans");
    const CoopI18n::Lang& hant = *ByCode("zh-Hant");
    CHECK(Line(hans, "Hyrule Field").empty());
    CHECK(Line(hant, "Hyrule Field").empty());

    // Place() in English is the name as it is (a name in no table stays English too).
    CHECK(CoopI18n::Place("Hyrule Field") == "Hyrule Field");
    CHECK(CoopI18n::Place("Not A Place Anyone Knows") == "Not A Place Anyone Knows");
}

TEST_CASE("coop i18n: every key is still a line of the module") {
    // A text changed in the code no longer finds its line, and that language
    // shows English there: the table is re-keyed with it. (Shapes, with {},
    // are matched by their ends at run time and are left out here.)
    const std::string sources = Sources();
    REQUIRE(sources.size() > 100000);
    const CoopI18n::Lang& hans = *ByCode("zh-Hans");
    for (int i = 0; i < hans.count; ++i) {
        const std::string en = hans.entries[i].en;
        if (Braces(en) > 0) {
            continue;
        }
        bool found = sources.find("\"" + Escaped(en)) != std::string::npos ||
                     sources.find(Escaped(en) + "\"") != std::string::npos;
        if (!found) {
            // a pickup's name: notify.cpp's own list, or one Spell() builds from an id
            const std::string names = NotifyNames();
            found = names.find("\"" + Escaped(en) + "\"") != std::string::npos || Spelled(en);
        }
        CHECK_MESSAGE(found, en);
    }
}
