// SPDX-License-Identifier: GPL-3.0-or-later
// The Multiplayer tab's words in the game's language (hookshot/src/coop_i18n.h):
// the table's lines, the lines the tab builds around a number, a name or a
// code (Tr()'s shapes), and the English a table does not have.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "coop_i18n.h"
#include "coop_lobby_tab.h"
#include "coop_pause_menu.h"
#include "coop_pause_tab.h"
#include "coop_title_native.h"

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

TEST_CASE("coop i18n: the most specific shape wins over a short end") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // coop_teleport.cpp's "teleport: stopped: <name> left" also ends in " left": the shape
    // with more fixed text is the line ("{} left" made it "teleport: stopped: Saria 离开了").
    CHECK(CoopI18n::Tr(hans, "teleport: stopped: Saria left") == "传送：已停下：Saria 离开了");
    CHECK(CoopI18n::Tr(hant, "teleport: stopped: Saria left") == "傳送：已停下：Saria 離開了");
    CHECK(CoopI18n::Tr(hans, "Saria left") == "Saria 离开了");
    CHECK(CoopI18n::Tr(hans, "teleport: stopped: the connection was lost") == "传送：已停下：连接断了");
    // The ages are words of the table: 少年/成年 Link, as the game's own Chinese names them.
    CHECK(CoopI18n::Tr(hans, "teleport: Saria is child Link and you are adult: no teleport to another age's Link") ==
          "传送：Saria 是少年林克，你是成年：不能传送到另一个时代的林克那里");
    CHECK(CoopI18n::Tr(nullptr, "teleport: stopped: Saria left") == "teleport: stopped: Saria left");
}

TEST_CASE("coop i18n: a pickup notice, clause by clause (coop/host/src/notify.cpp's lines)") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // The map, the compass and the Boss Key are their own clauses, not "got the " and an item
    // called "map (Deku Tree"; the key holds the brackets, so there is one of each.
    CHECK(CoopI18n::Tr(hans, "Saria got the map (Deku Tree)") == "Saria 拿到了地图（德库大树）");
    CHECK(CoopI18n::Tr(hant, "Saria got the map (Deku Tree)") == "Saria 拿到了地圖（德庫大樹）");
    CHECK(CoopI18n::Tr(hans, "Saria got the compass (Deku Tree)") == "Saria 拿到了指南针（德库大树）");
    CHECK(CoopI18n::Tr(hant, "Saria got the compass (Deku Tree)") == "Saria 拿到了指南針（德庫大樹）");
    CHECK(CoopI18n::Tr(hans, "Saria got the Boss Key (Deku Tree)") == "Saria 拿到了 Boss 钥匙（德库大树）");
    CHECK(CoopI18n::Tr(hant, "Saria got the Boss Key (Deku Tree)") == "Saria 拿到了 Boss 鑰匙（德庫大樹）");
    CHECK(CoopI18n::Tr(hans, "Saria got a Gold Skulltula token (12)") == "Saria 得到一枚金色骷髅蜘蛛徽章（12）");
    CHECK(CoopI18n::Tr(hant, "Saria got a Gold Skulltula token (12)") == "Saria 得到一枚金色骷髏蜘蛛徽章（12）");
    // The Heart Container's whole line (it stayed English); "Player 3" is notify.cpp's stand-in.
    CHECK(CoopI18n::Tr(hans, "Player 3 got a Heart Container - your max health went up") ==
          "玩家 3 得到一颗心之容器 —— 最大生命值提升了");
    CHECK(CoopI18n::Tr(hant, "Saria got a Heart Container - your max health went up") ==
          "Saria 得到一顆心之容器 —— 最大生命值提升了");
    // An item, a group of items, a song, a Small Key with the group's count.
    CHECK(CoopI18n::Tr(hans, "Saria got the Fairy Slingshot - you got one too!") == "Saria 拿到了妖精弹弓 —— 你也得到了一个！");
    CHECK(CoopI18n::Tr(hans, "Saria got the Bombs, the Deku Nuts - you got them too!") ==
          "Saria 拿到了炸弹、德库坚果 —— 你也得到了！");
    CHECK(CoopI18n::Tr(hant, "Someone learned Zelda's Lullaby") == "某人 學會了薩爾達的搖籃曲");
    CHECK(CoopI18n::Tr(hans, "Saria found a Small Key (Forest Temple) - the group has 2") ==
          "Saria 找到一把小钥匙（森之神殿）—— 队伍现在有 2 把");
    // Several pickups of one kind: each clause, then the last one's end.
    CHECK(CoopI18n::Tr(hans, "Saria got the map (Deku Tree), got the compass (Deku Tree)") ==
          "Saria 拿到了地图（德库大树），拿到了指南针（德库大树）");
    CHECK(CoopI18n::Tr(hant, "Saria found a Small Key (Forest Temple), found a Small Key (Forest Temple) - the group has 3") ==
          "Saria 找到一把小鑰匙（森之神殿），找到一把小鑰匙（森之神殿）—— 隊伍現在有 3 把");
    // English stays as notify.cpp wrote it; a line that is no pickup is left alone.
    CHECK(CoopI18n::Tr(nullptr, "Saria got the map (Deku Tree)") == "Saria got the map (Deku Tree)");
    CHECK(CoopI18n::Tr(hans, "Saria got lost") == "Saria got lost");
}

TEST_CASE("coop i18n: a chest opened elsewhere, and the one heart container") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // The notice's extra_ (lichii/hook/coop_hook.cpp) had no line at all.
    CHECK(CoopI18n::Tr(hans, "ROGX opened a chest") == "ROGX 打开了一个宝箱");
    CHECK(CoopI18n::Tr(hant, "ROGX opened a chest") == "ROGX 打開了一個寶箱");
    // The boss room's chat line (coop_boss_scenes.cpp) had none either.
    CHECK(CoopI18n::Tr(hans, "ROGX got the heart container for everyone") ==
          "ROGX 拿到了心之容器，人人有份");
    CHECK(CoopI18n::Tr(hant, "ROGX got the heart container for everyone") ==
          "ROGX 拿到了心之容器，人人有份");
}

TEST_CASE("coop i18n: the pickup names Spell() would have misspelled") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // These reached ItemName (a line of kItems each, now) and were spelled from
    // the id before: "4090 拿到了 Gauntlet Goron", "Quiver Small, the Arrow
    // Normal". A pickup that is one notice with two names has each of them
    // translated, and the ", the " between them too.
    CHECK(CoopI18n::Tr(hans, "4090 got the Goron's Bracelet - you got one too!") ==
          "4090 拿到了哥隆手环 —— 你也得到了一个！");
    CHECK(CoopI18n::Tr(hans, "4090 got the Small Quiver, the Arrows - you got them too!") ==
          "4090 拿到了小箭袋、箭 —— 你也得到了！");
    CHECK(CoopI18n::Tr(hant, "4090 got the Small Quiver, the Arrows - you got them too!") ==
          "4090 拿到了小箭袋、箭 —— 你也得到了！");
    // A name the table does not know stays whole, in its own spelling: the join
    // is the table's, the name is not.
    CHECK(CoopI18n::Tr(hans, "4090 got the Small Quiver, the Nothing At All - you got them too!") ==
          "4090 拿到了小箭袋、Nothing At All —— 你也得到了！");
}

TEST_CASE("coop i18n: 0.11.7's rows and help bodies") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // The rows the 0.11.7 menu added (coop_pause_menu.cpp: PageRow("Local co-op", "2 players"),
    // PageRow("Customize Link", "Colours"), Choice("Saves", "Solo")).
    CHECK(CoopI18n::Tr(hans, "2 players") == "2 人");
    CHECK(CoopI18n::Tr(hant, "2 players") == "2 人");
    CHECK(CoopI18n::Tr(hans, "Colours") == "颜色");
    CHECK(CoopI18n::Tr(hant, "Colours") == "顏色");
    CHECK(CoopI18n::Tr(hans, "Solo") == "单人");
    CHECK(CoopI18n::Tr(hant, "Solo") == "單人");
    CHECK(CoopI18n::Tr(hans, "Co-op Settings") == "联机设置");
    CHECK(CoopI18n::Tr(hant, "Co-op Settings") == "連線設定");
    CHECK(CoopI18n::Tr(hans, "Options") == "选项");
    CHECK(CoopI18n::Tr(hant, "Options") == "選項");
    // New Game+'s two help bodies: the locked one is a line, the unlocked one is built
    // around the hearts kept and the level now (a split shape).
    CHECK(CoopI18n::Tr(hans, "Beat Ganon to unlock it: then play again with the hearts you had and tougher enemies.") ==
          "击败加农后解锁：之后可以用当时的心数重玩，敌人也更强。");
    CHECK(CoopI18n::Tr(hant, "Locked: beat Ganon") == "未解鎖：擊敗加農");
    const std::string ng = CoopI18n::Tr(
        hans, "Start a new file with this on: you keep your 4+1 hearts, and enemies and bosses get 50% more life "
              "and 25% more damage for each level (up to NG+ 2 now). In a game you host your level counts for "
              "everyone. Off: the game as it is.");
    CHECK(ng.find("保留 4+1 颗心") != std::string::npos);
    CHECK(ng.find("最高 NG+ 2") != std::string::npos);
    CHECK(ng.find("Start a new file") == std::string::npos);
    // The Saves row's help, whole.
    const std::string saves = CoopI18n::Tr(
        hans, "Solo: your own saves. Multiplayer: a separate set of saves for playing together (made from your "
              "solo saves the first time), so co-op progress never mixes with your own. When you join a friend "
              "from the title screen you play the host's save, copied beside your multiplayer saves; your saves "
              "are not touched. Switched on the title screen with no file loaded.");
    CHECK(saves.rfind("单人：用你的存档。", 0) == 0);
    CHECK(saves.find("联机：独立的联机存档") != std::string::npos);
}

TEST_CASE("coop i18n: the pause menu's Multiplayer tab (the game's own page)") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // The rows (coop_pause_tab.cpp's RowTexts, translated as they are handed to the game): the words
    // are the tab's own, one line each, and the rows with a value are the table's shapes.
    CHECK(CoopI18n::Tr(hans, "Hosting") == "房间已开");
    CHECK(CoopI18n::Tr(hant, "Hosting") == "房間已開");
    CHECK(CoopI18n::Tr(hans, "Copy invite code") == "复制邀请码");
    CHECK(CoopI18n::Tr(hans, "Players (just you)") == "人数：只有你");
    CHECK(CoopI18n::Tr(hans, "Stop hosting") == "停止房间");
    CHECK(CoopI18n::Tr(hans, "Host a game") == "创建房间");
    CHECK(CoopI18n::Tr(hans, "Join a game") == "加入房间");
    // The rows with a value: the whole row is the shape's line.
    CHECK(CoopI18n::Tr(hans, "Room: 7QK-2") == "房间：7QK-2");
    CHECK(CoopI18n::Tr(hans, "Copy room: 7QK-2") == "复制房间：7QK-2");
    CHECK(CoopI18n::Tr(hans, "Players (3)") == "人数：3");
    CHECK(CoopI18n::Tr(hans, "Let in Saria") == "放行 Saria");
    CHECK(CoopI18n::Tr(hans, "Let in Saria (+2)") == "放行 Saria（+2）");
    CHECK(CoopI18n::Tr(hant, "Let in Saria (+2)") == "放行 Saria（+2）");
    CHECK(CoopI18n::Tr(hans, "More players (2/3)") == "更多玩家（2/3）");
    CHECK(CoopI18n::Tr(hans, "Local games (2)") == "本机房间（2）");
    CHECK(CoopI18n::Tr(hans, "Next page (2/3)") == "下一页（2/3）");
    CHECK(CoopI18n::Tr(hans, "Kick Saria") == "踢出 Saria");
    CHECK(CoopI18n::Tr(hans, "A again: ban Saria") == "再按A：封禁 Saria");
    CHECK(CoopI18n::Tr(hans, "Refuse Saria") == "拒绝 Saria");
    CHECK(CoopI18n::Tr(hans, "+2 more ask") == "还有 2 人申请");
    CHECK(CoopI18n::Tr(hans, "Join Saria again") == "再次加入 Saria");
    CHECK(CoopI18n::Tr(hans, "Joined Saria") == "已加入 Saria");
    CHECK(CoopI18n::Tr(hans, "With 3 players") == "与 3 人");
    CHECK(CoopI18n::Tr(hans, "With 1 player") == "与 1 人");
    CHECK(CoopI18n::Tr(hans, "Teleport to Saria") == "传送到 Saria");
    CHECK(CoopI18n::Tr(hans, "Saria (you)") == "Saria（你）");
}

TEST_CASE("coop i18n: the commands' help, the save notices and the teleport refusals") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // The Longshot menu's Commands page (coop_pause_menu.cpp's Commands): the two new in 0.11.7.
    CHECK(CoopI18n::Tr(hans, "The host only: turns away the player who asks to join. Add always to keep them out "
                              "until you stop hosting.") .rfind("仅房主：", 0) == 0);
    CHECK(CoopI18n::Tr(hant, "The host only, when a player asks to join (approve_joins = on in coop-options.ini): "
                              "lets them in (the one who asked first, or by name). Add always to let them in "
                              "without asking until you stop hosting.").rfind("僅房主：", 0) == 0);
    // The guest's save lines (coop/host/src/save_transfer.cpp's GuestNotices; the chat draws them).
    CHECK(CoopI18n::Tr(hans, "The host's save is coming: load a save once the chat says Success.") ==
          "房主的存档马上到：等聊天显示 Success 再载入存档。");
    CHECK(CoopI18n::Tr(hant, "The host's save did not come: play on, or join again to get it.") ==
          "房主的存檔沒有送到：繼續玩，或重新加入來取得。");
    // The teleport's refusals (lichii/hook/coop_teleport.cpp's RefusedLine) had no lines at all.
    CHECK(CoopI18n::Tr(hans, "teleport: 4090 is busy (loading, a cutscene, a talk or in the air); try again soon") ==
          "传送：4090 正忙（读盘、过场、对话或空中）；稍后再试");
    CHECK(CoopI18n::Tr(hans, "teleport: 4090 said no") == "传送：4090 拒绝了");
    CHECK(CoopI18n::Tr(hans, "teleport: 4090 is in a boss fight; no teleport there") ==
          "传送：4090 正在打 BOSS；那里不能传送");
    CHECK(CoopI18n::Tr(hans, "teleport: 4090 is not in the game") == "传送：4090 不在游戏里");
    CHECK(CoopI18n::Tr(hans, "teleport: 4090 refused (reason 3)") == "传送：4090 拒绝了（原因 3）");
}

TEST_CASE("coop i18n: the saves that switch folders, and the settings' own labels") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // lichii/hook/coop_save.cpp's chat lines (the save set switches at the title).
    CHECK(CoopI18n::Tr(hans, "Go back to the title screen first: the saves switch there.") ==
          "先回标题画面：存档是在那里切换的。");
    CHECK(CoopI18n::Tr(hans, "Multiplayer saves: load a save to play them.") == "联机存档：载入一个存档来玩。");
    CHECK(CoopI18n::Tr(hant, "Resync from host: the host's save is already on its way.") ==
          "重新取得房主存檔：房主的存檔已經在路上。");
    CHECK(CoopI18n::Tr(hans, "The saves could not be switched (see the log).") == "存档切换不了（见日志）。");
    // The Settings page's own row labels (Choice("Saves"...), Choice("New Game+"...), and the
    // microphone row), which the page titles around them had translated before them.
    CHECK(CoopI18n::Tr(hans, "Saves") == "存档");
    CHECK(CoopI18n::Tr(hans, "New Game+") == "新游戏+");
    CHECK(CoopI18n::Tr(hant, "Microphone") == "麥克風");
    CHECK(CoopI18n::Tr(hans, "New Game+ off.") == "新游戏+：关");
    CHECK(CoopI18n::Tr(hans, "New Game+ 2: start a new file to play it (your hearts carry over).") ==
          "新游戏+ 2：开个新存档才能玩（心数会带过去）。");
    // The game browser's own cells (coop_pause_tab.cpp: ListLines, ListValue, coop_room_list.cpp).
    CHECK(CoopI18n::Tr(hans, "Game") == "游戏");
    CHECK(CoopI18n::Tr(hans, "Looking for games...") == "正在找房间…");
    CHECK(CoopI18n::Tr(hans, "Other version") == "版本不同");
    CHECK(CoopI18n::Tr(hans, "Full") == "已满");
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

TEST_CASE("coop i18n: the group's sync lines, with their counts") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // coop/host/src/notify.cpp: "Synced with the group: " + its counts, and the line for the notices held back.
    CHECK(CoopI18n::Tr(hans, "Synced with the group: 2 items, 1 songs") == "已与队伍同步：2 件道具，1 首歌曲");
    CHECK(CoopI18n::Tr(hant, "Synced with the group: 3 items") == "已與隊伍同步：3 件道具");
    CHECK(CoopI18n::Tr(hans, "...and 4 more from the group") == "……还有 4 条队伍的消息");
    CHECK(CoopI18n::Tr(hant, "...and 4 more from the group") == "……還有 4 條隊伍的消息");
    // English stays as the host wrote it.
    CHECK(CoopI18n::Tr(nullptr, "Synced with the group: 2 items") == "Synced with the group: 2 items");
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

TEST_CASE("coop i18n: 0.11.8's screens (PR #308): the Multiplayer page, co-op's menu, the Original game switch") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // the page's rows, whole and built around a value
    CHECK(CoopI18n::Tr(hans, "Browse games") == "浏览房间");
    CHECK(CoopI18n::Tr(hant, "Browse games") == "瀏覽房間");
    CHECK(CoopI18n::Tr(hans, "Settings and look") == "设置与外观");
    CHECK(CoopI18n::Tr(hans, "Hosting: just you") == "房间中：只有你");
    CHECK(CoopI18n::Tr(hans, "Hosting: 3 players") == "房间中：3 人");
    CHECK(CoopI18n::Tr(hans, "Search: A to type") == "搜索：按A输入");
    CHECK(CoopI18n::Tr(hans, "Search: kokiri") == "搜索：kokiri");
    CHECK(CoopI18n::Tr(hans, "Show: Open games") == "显示：可加入的房间");
    CHECK(CoopI18n::Tr(hant, "Sort: Most players") == "排序：人多的在前");
    CHECK(CoopI18n::Tr(hans, "Room or IP: 192.168.1.5") == "房间或IP：192.168.1.5");
    CHECK(CoopI18n::Tr(hans, "Again: kick Saria") == "再点：踢出 Saria");
    CHECK(CoopI18n::Tr(hans, "Join (port 27510)") == "加入（端口 27510）");
    CHECK(CoopI18n::Tr(hans, "Forward UDP 27510 to this PC") == "把 UDP 27510 转发到这台电脑");
    CHECK(CoopI18n::Tr(hans, "Public games (4)") == "公开游戏（4）");
    CHECK(CoopI18n::Tr(hant, "This network (2)") == "本網路（2）");
    CHECK(CoopI18n::Tr(hans, "Letter: < Space >") == "字母：< 空格 >");
    // co-op's menu and its notes
    CHECK(CoopI18n::Tr(hans, "Turns Saria away, as /deny in the chat does. They can ask again.") ==
          "拒绝 Saria，和聊天里的 /deny 一样。对方可以再申请。");
    CHECK(CoopI18n::Tr(hans, "Letting Saria in.") == "正在放行 Saria。");
    CHECK(CoopI18n::Tr(hans, "Saria was turned away.") == "已拒绝 Saria。");
    CHECK(CoopI18n::Tr(hans, "Copied the VPN line") == "已复制 VPN 那一行");
    CHECK(CoopI18n::Tr(hans, "No network line yet") == "还没有 同一网络那一行");
    CHECK(CoopI18n::Tr(hans, "NG+ 2") == "NG+ 2");
    CHECK(CoopI18n::Tr(hans, "Joining kokiri-1...") == "正在加入 kokiri-1…");
    CHECK(CoopI18n::Tr(hans, "Pasted: 12 characters.") == "已粘贴：12 个字符。");
    const std::string pc = CoopI18n::Tr(hans, "No PC called LIVINGROOM was found. To join a room by its name, set a relay "
                                              "first: Co-op Settings, Relay. Or type the host's IP address.");
    CHECK(pc.rfind("没找到叫 LIVINGROOM 的电脑", 0) == 0);
    // the Original game switch
    CHECK(CoopI18n::Tr(hans, "Original game") == "原版游戏");
    CHECK(CoopI18n::Tr(hant, "Start the game again to finish: it comes back as the original game.") ==
          "重新啟動遊戲即可完成：它會以原版遊戲回來。");
    CHECK(CoopI18n::Tr(hans, "Start the game again to finish: co-op and its extras come back.") ==
          "重启游戏即可完成：联机和附加功能会回来。");
    // one word for the relay in each table
    CHECK(CoopI18n::Tr(hans, "Relay") == "中转");
    CHECK(CoopI18n::Tr(hant, "Relay") == "中轉");
    CHECK(CoopI18n::Tr(hans, "The game list's server in Co-op Settings (Relay) is not right. Clear it to use the kit's own.")
              .find("中继") == std::string::npos);
}

TEST_CASE("coop i18n: 0.11.9's - menu screens and Customize (ui-tabs)") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    CHECK(CoopI18n::Tr(hans, "Eyes") == "眼睛");
    CHECK(CoopI18n::Tr(hant, "Hair") == "頭髮");
    CHECK(CoopI18n::Tr(hans, "Boots and gauntlets") == "靴子与护手");
    CHECK(CoopI18n::Tr(hans, "Hue") == "色相");
    CHECK(CoopI18n::Tr(hans, "120 degrees") == "120 度");
    CHECK(CoopI18n::Tr(hans, "Left and Right: from grey (0%) to the full colour (100%), 5% a press.") ==
          "左右键：从灰色（0%）到全彩（100%），每按一次 5%。");
    CHECK(CoopI18n::Tr(hans, "Shields (page 2 of 3)") == "盾牌（第 2/3 页）");
    CHECK(CoopI18n::Tr(hant, "Swords (page 1 of 2)") == "劍（第 1/2 頁）");
    CHECK(CoopI18n::Tr(hans, "Your game") == "你的房间");
    CHECK(CoopI18n::Tr(hans, "Your game: Saria asks") == "你的房间：Saria 申请加入");
    CHECK(CoopI18n::Tr(hans, "Your game: 3 ask") == "你的房间：3 人申请加入");
    CHECK(CoopI18n::Tr(hans, "Saria (host)") == "Saria（房主）");
    CHECK(CoopI18n::Tr(hans, "Find a game") == "找房间");
    CHECK(CoopI18n::Tr(hant, "Debug") == "偵錯");
}

TEST_CASE("coop i18n: a character not downloaded (Snail's Character Select, coop_char_slots.h)") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    CHECK(CoopI18n::Tr(hans, "This character is not downloaded: press N at the kit's start to get Snail's NPC characters.") ==
          "这个角色还没下载：在套件启动时按 N，即可获取 Snail 的 NPC 角色。");
    const std::string other = " plays a character you have not downloaded, shown as Link (press N at the kit's start to "
                              "get Snail's NPC characters).";
    CHECK(CoopI18n::Tr(hans, "A player" + other) ==
          "一名玩家 用的角色你还没下载，显示为林克（在套件启动时按 N，即可获取 Snail 的 NPC 角色）。");
    // (the only caller writes "Player N": the number as every other Player line, 玩家 N)
    CHECK(CoopI18n::Tr(hans, "Player 3" + other) ==
          "玩家 3 用的角色你还没下载，显示为林克（在套件启动时按 N，即可获取 Snail 的 NPC 角色）。");
    CHECK(CoopI18n::Tr(hant, "Player 3" + other).rfind("玩家 3 用的角色你還沒下載", 0) == 0);
}

// menu-parity-0119 (the owner: "no English left on zh games"): every row the pause menu's native co-op pages show, in
// every view and every state of co-op the rows have words for, is a line (or a shape) of both Chinese tables, and so
// is every row and help of Co-op settings, host rows included. A row left English fails here with its words.
TEST_CASE("coop i18n: every native co-op row in every view and state has its Chinese") {
    using namespace CoopPauseTab;
    const CoopI18n::Lang* langs[] = {ByCode("zh-Hans"), ByCode("zh-Hant")};
    const std::vector<std::string> names = {"Saria", "Mido"};
    std::vector<SessionView> states;
    {
        SessionView v;   // nothing yet
        states.push_back(v);
        v.s.rejoin = true, v.rejoin_host = "Saria";
        states.push_back(v);
        v.rejoin_host.clear();
        states.push_back(v);
        v.room_field = "forest", v.room_ready = v.s.room_ready = true;
        for (const bool relay : {false, true}) {
            v.relay_set = relay;
            states.push_back(v);
        }
        v.join_address = true, v.join_port = 27510;
        states.push_back(v);
        v.typing = 0;
        states.push_back(v);
        v.typing = 1;
        states.push_back(v);
        SessionView f;
        f.s.failed = true;
        states.push_back(f);
    }
    {
        SessionView h;   // hosting
        h.s.hosting = h.s.in_one = h.s.code = true;
        h.connecting = true;
        states.push_back(h);
        h.connecting = false;
        h.room = "forest", h.players = 2, h.port = 27510;
        states.push_back(h);
        h.asking = {"Mido"};
        states.push_back(h);
        h.asking = {"Mido", "Saria"}, h.s.asking = true, h.s.invite = true;
        states.push_back(h);
        h.confirm_leave = true;
        states.push_back(h);
        h.confirm_click = true;
        states.push_back(h);
        h.confirm_leave = h.confirm_click = false;
        h.player_name = "Mido", h.names = names;
        states.push_back(h);
        h.confirm_kick = true;
        states.push_back(h);
    }
    {
        SessionView g;   // in a friend's game
        g.s.in_one = true;
        g.connecting = true;
        states.push_back(g);
        g.connecting = false, g.players = 3, g.host_name = "Saria", g.s.can_resync = true;
        states.push_back(g);
        g.confirm_leave = true;
        states.push_back(g);
        g.confirm_leave = false, g.s.wait_host = true;
        states.push_back(g);
        g.s.wait_host = false, g.turned_away = "full";
        states.push_back(g);
        SessionView l;
        l.s.by_launcher = true;
        states.push_back(l);
    }
    const auto english = [&](const std::string& row) {   // (a row that only holds a name is the name in any language)
        if (row.empty() || std::find(names.begin(), names.end(), row) != names.end()) return false;
        if (row.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") == std::string::npos) return false;
        for (const CoopI18n::Lang* lang : langs) {
            if (CoopI18n::Tr(lang, row) == row) return true;
        }
        return false;
    };
    const View views[] = {View::kMain, View::kPlayers, View::kLetIn, View::kJoin, View::kSession, View::kPlayer,
                          View::kPlayerHost, View::kLan, View::kLanGame, View::kPassword, View::kMenu, View::kHost};
    for (const SessionView& v : states) {
        for (const View view : views) {
            for (const std::string& row : RowTexts(view, v)) CHECK_MESSAGE(!english(row), row);
        }
        for (const MinusPage page : {MinusPage::kMultiplayer, MinusPage::kLobby}) {
            for (const std::string& row : RowTexts(page, v)) CHECK_MESSAGE(!english(row), row);
        }
    }
    // Co-op settings, as the hub's list shows it: not in a session, hosting with the rules in force, and as a guest.
    std::vector<CoopPauseMenu::Session> sessions(3);
    sessions[1].state = CoopPauseMenu::Session::State::kHosting, sessions[1].hosting = true, sessions[1].rules = true;
    sessions[2].state = CoopPauseMenu::Session::State::kJoined, sessions[2].rules = true;
    for (const CoopPauseMenu::Session& ss : sessions) {
        CoopPauseMenu::Menu m;
        m.Show(CoopTitleMenu::Options{});
        m.SetSession(ss);
        m.Open(CoopPauseMenu::Page::kSettings);
        const std::size_t rows = m.view().rows.size();
        for (std::size_t i = 0; i < rows; ++i) {
            m.Select(i);
            const CoopPauseMenu::View mv = m.view();
            const CoopPauseMenu::Row& r = mv.rows[i];
            const bool host_row = r.name == "Game" || r.name == "Approve joins" || r.name == "PvP" ||
                                  r.name == "Down but not out" || r.name == "Bleed-out time" ||
                                  r.name == "Revive hold" || r.name == "Revive hearts";
            if (!host_row) continue;   // (the other rows' words: the 0.11.7 and 0.11.8 cases above)
            CHECK_MESSAGE(!english(r.name), r.name);
            if (static_cast<std::size_t>(mv.selected) == i) CHECK_MESSAGE(!english(mv.help), mv.help);
        }
    }
}

TEST_CASE("coop i18n: Difficulty's rows, /difficulty and the Commands page's new lines (row 226)") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    for (const char* row : {"Difficulty", "Automatic", "Enemy life", "Enemy damage", "Enemies"}) {
        INFO(row);
        CHECK(CoopI18n::Tr(hans, row) != row);
        CHECK(CoopI18n::Tr(hant, row) != row);
    }
    CHECK(CoopI18n::Tr(hans, "Difficulty") == "难度");
    CHECK(CoopI18n::Tr(hant, "Enemy damage") == "敵人傷害");
    CHECK(CoopI18n::Tr(hans, "Only the host can change it.") == "只有房主能更改。");
    CHECK(CoopI18n::Tr(hant, "The host only: how long a downed player waits for a revive, 10 to 240 seconds.")
              .rfind("僅房主：", 0) == 0);
    CHECK(CoopI18n::Tr(hans, "difficulty: only the host can change it") == "难度：只有房主能更改");
    // The values line the help starts with, the chat's own shape (a preset's name translated in it).
    CHECK(CoopI18n::Tr(hans, "Difficulty: Hard: life 200%, damage 150%, enemies 150%") !=
          "Difficulty: Hard: life 200%, damage 150%, enemies 150%");
}

TEST_CASE("coop i18n: the title's Host and Join pages, Who can join (title-join, title-toggle) and the voice words (hud-native)") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    for (const CoopI18n::Lang* lang : {hans, hant}) {
        REQUIRE(lang != nullptr);
        // each is a line of the table, not English
        for (const char* en : {"Who can join", "Private", "Public", "Public, password", "Code", "Press again",
                               "Enter a code or address", "Paste a code", "Type an address",
                               "A: Join   B: Back   Y: Refresh", "Joins the game with this password.",
                               "Paste a code or type an address first.", "talking", "muted"}) {
            const std::string tr = CoopI18n::Tr(lang, en);
            CHECK_MESSAGE(tr != en, en);
        }
    }
    CHECK(CoopI18n::Tr(hans, "Who can join") == "谁能加入");
    CHECK(CoopI18n::Tr(hant, "Who can join") == "誰能加入");
    CHECK(CoopI18n::Tr(hans, "Private") == "私人");
    CHECK(CoopI18n::Tr(hant, "Public") == "公開");
    CHECK(CoopI18n::Tr(hans, "Page 2/3") == "第 2/3 页");   // (row 264: the page line; the title adds "(ZL/ZR)" after)
    CHECK(CoopI18n::Tr(hant, "Page 1/2") == "第 1/2 頁");
    CHECK(CoopI18n::Tr(hans, "Page") == "页");
    CHECK(CoopI18n::Tr(hant, "Turn the page") == "翻頁");
    CHECK(CoopI18n::Tr(hans, "Room  forest-4821") == "房间  forest-4821");
    CHECK(CoopI18n::Tr(hant, "Room  forest-4821") == "房間  forest-4821");
    CHECK(CoopI18n::Tr(hans, "Join Saria again") == "再次加入 Saria");
    CHECK(CoopI18n::Tr(hans, "Let in Saria") == "放行 Saria");
    // (the short page header: the name column cuts at 30 bytes and the full "第1页，共3页" shape was cut off mid-word)
    CHECK(CoopI18n::Tr(hans, "Co-op settings  < 1 of 3 >") == "联机设置  < 1/3 >");
    CHECK(CoopI18n::Tr(hant, "Customize Link  < 4 of 4 >") == "自訂林克  < 4/4 >");
}

TEST_CASE("coop i18n (help-lines): every row's help on the - page has a line in both Chinese tables and fits the caption") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    REQUIRE(hans != nullptr);
    REQUIRE(hant != nullptr);
    std::vector<std::string> helps;
    // Every action but kNone (the enum's last is kViewHostSettings) has a help.
    for (int a = static_cast<int>(CoopPauseTab::RowAction::kNone) + 1;
         a <= static_cast<int>(CoopPauseTab::RowAction::kViewHostSettings); ++a) {
        const std::string h = CoopPauseTab::RowHelp(static_cast<CoopPauseTab::RowAction>(a));
        CHECK_MESSAGE(!h.empty(), "no help for RowAction " << a);
        helps.push_back(h);
    }
    CHECK(std::string(CoopPauseTab::RowHelp(CoopPauseTab::RowAction::kNone)).empty());
    for (int tab = 0; tab < 5; ++tab) helps.push_back(CoopPauseTab::MenuTabHelp(tab));
    helps.push_back(CoopPauseTab::kGameHelp);
    helps.push_back(CoopPauseTab::kTypingHelp);
    for (const std::string& en : helps) {
        if (en.empty()) continue;
        for (const CoopI18n::Lang* lang : {hans, hant}) {
            const std::string zh = Line(*lang, en.c_str());
            CHECK_MESSAGE(!zh.empty(), lang->code << " has no line for: " << en);
            // The caption holds a few short lines: the help, wrapped, is not cut with "..." in any language.
            CHECK_MESSAGE(CoopPauseTab::HelpText(zh).find("...") == std::string::npos, lang->code << " too long: " << en);
        }
        CHECK_MESSAGE(CoopPauseTab::HelpText(en).find("...") == std::string::npos, "English too long: " << en);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// A player's, a room's or a mod's own text is never looked up in the table (PR #313 review): a player called "Link"
// must not read 林克, a name or room typed "On" must not become 开启. Only our own words are translated.
namespace {

// The Menu language row pinned to Simplified Chinese for a test (Current() reads the game's settings otherwise).
struct Hans {
    Hans() { CoopI18n::ForceLanguage("zh-Hans"); }
    ~Hans() { CoopI18n::ForceLanguage(nullptr); }
};

CoopPauseMenu::Row Field(std::string name, std::string value, bool own_value = true) {
    CoopPauseMenu::Row r;
    r.name = std::move(name);
    r.value = std::move(value);
    r.own_value = own_value;
    return r;
}

}  // namespace

TEST_CASE("coop i18n (own text): the menu's text fields keep what the player typed, the words beside them are ours") {
    const Hans hans;
    // (the table does have these words: they are translated where they are the tab's own)
    REQUIRE(CoopI18n::Tr("On") == "开启");
    REQUIRE(CoopI18n::Tr("Link") == "林克");
    // Not a text field: the tab's own value word is looked up, as before.
    CHECK(CoopPauseTab::MenuValue(Field("Some row", "On", false), false) == "开启");
    // A text field: the value as it is, typed or not.
    CHECK(CoopPauseTab::MenuValue(Field("Your name", "Link"), false) == "Link");
    CHECK(CoopPauseTab::MenuValue(Field("Room name", "On"), false) == "On");
    CHECK(CoopPauseTab::MenuValue(Field("Room name", "On"), true) == "On_");
    CoopPauseMenu::Row choice = Field("Mod setting", "On");
    choice.kind = CoopPauseMenu::RowKind::kChoice;
    choice.left = choice.right = true;
    CHECK(CoopPauseTab::MenuValue(choice, false) == "< On >");   // (a mod's own choice word)

    CoopPauseMenu::View v;
    v.rows = {Field("Your name", "Link"), Field("Room name", "On"), Field("Port", "Off")};
    CoopPauseTab::MenuList list = CoopPauseTab::MenuLines(v, 0, 0, false, 8);
    CHECK(list.lines[1].name == "你的名字");
    CHECK(list.lines[1].value == "Link");
    CHECK(list.lines[2].name == "房间名");
    CHECK(list.lines[2].value == "On");
    CHECK(list.lines[3].value == "Off");
    // typed "On" into the name: "On_", not 开启_
    v.typing = true;
    v.selected = 0;
    v.rows[0].value = "On";
    list = CoopPauseTab::MenuLines(v, 0, 0, true, 8);
    CHECK(list.lines[1].value == "On_");
    // A row of a player or a room (a bare name): its name stays as it is, whatever the table says about the word.
    CoopPauseMenu::View names;
    names.rows = {Field("Link", "", false), Field("Link", "", false)};
    names.rows[0].own_name = true;
    list = CoopPauseTab::MenuLines(names, 0, 0, false, 8);
    CHECK(list.lines[1].name == "Link");
    CHECK(list.lines[2].name == "林克");   // (the tab's own word "Link" is still the table's)
    CHECK(list.lines[1].own_name);        // (the module draws the lines as they are: no second look-up)
    CHECK(list.lines[1].own_value);
}

TEST_CASE("coop i18n (own text): the title's buttons and the typing look keep a player's name and a typed value") {
    const Hans hans;
    CHECK(CoopTitleNative::RowText(Field("Your name", "Link")) == "你的名字: Link");
    CHECK(CoopTitleNative::RowText(Field("Room name", "On")) == "房间名: On");
    CoopPauseMenu::Row bare = Field("Link", "", false);
    CHECK(CoopTitleNative::RowText(bare) == "林克");   // (the tab's own word)
    bare.own_name = true;
    CHECK(CoopTitleNative::RowText(bare) == "Link");   // (a player's name)
    // While typing: the field shows what was typed, as typed.
    CoopPauseMenu::View v;
    v.rows = {Field("Your name", "On_")};
    v.selected = 0;
    v.typing = true;
    const CoopTitleNative::Look look = CoopTitleNative::TypingLook(v, 'a', CoopTitleNative::kFirstKey);
    CHECK(look.texts[static_cast<std::size_t>(CoopTitleNative::Key::kField)] == "你的名字: On_");
    // the keys are the module's own words, in the player's language here (the caller looks nothing up again)
    CHECK(look.texts[static_cast<std::size_t>(CoopTitleNative::Key::kDelete)] == "删除");
    CHECK(look.texts[static_cast<std::size_t>(CoopTitleNative::Key::kLetter)].rfind("字母", 0) == 0);
}

TEST_CASE("coop i18n (own text): the Players view's player rows are names, not lines of the table") {
    const Hans hans;
    CoopPauseTab::SessionView v;
    v.names = {"Link", "Saria (host)", "Link (you)"};
    const std::vector<std::string> rows = CoopPauseTab::RowTexts(CoopPauseTab::View::kPlayers, v);
    CHECK(rows[0] == "Link");              // (not 林克)
    CHECK(rows[1] == "Saria（房主）");     // (what is added after the name is the table's)
    CHECK(rows[2] == "Link（你）");
    CHECK(rows[CoopPauseTab::kRows - 1] == CoopI18n::Tr("Back"));
}

TEST_CASE("coop i18n (own text): the details list and the browser's list keep a room's, a host's and a game's own name") {
    const Hans hans;
    CoopPauseTab::SessionView idle;
    idle.s.auto_rejoin = true;
    idle.room_name = "Link";
    std::vector<CoopPauseTab::ListLine> d = CoopPauseTab::DetailLines(CoopPauseTab::View::kMain, idle);
    CHECK(d[1].name == "房间名");
    CHECK(d[1].value == "Link");
    CHECK(d[1].own_value);   // (the module draws the lines as they are)
    idle.room_name = "On";
    CHECK(CoopPauseTab::DetailLines(CoopPauseTab::View::kMain, idle)[1].value == "On");
    idle.room_name.clear();
    CHECK(CoopPauseTab::DetailLines(CoopPauseTab::View::kMain, idle)[1].value == "无");   // (the placeholder is ours)

    CoopPauseTab::SessionView hosting;
    hosting.s.hosting = true;
    hosting.s.in_one = true;
    hosting.room = "Kokiri Forest";
    hosting.players = 1;
    d = CoopPauseTab::DetailLines(CoopPauseTab::View::kMain, hosting);
    CHECK(d[1].name == CoopI18n::Tr("Room"));
    CHECK(d[1].value == "Kokiri Forest");   // (a place of the game's, but this one is the player's room name)

    CoopPauseTab::SessionView browse;
    browse.s.online = true;
    browse.relay_set = true;
    browse.s.online_listed = 2;
    browse.online_list = {CoopPauseTab::ListLine{"Link", "Public game", CoopPauseTab::RowAction::kNone, false, true, true},
                          CoopPauseTab::ListLine{"On", "Full", CoopPauseTab::RowAction::kNone, false, true, true}};
    const std::vector<CoopPauseTab::ListLine> lines = CoopPauseTab::ListLines(browse);
    CHECK(lines[1].name == "Link");   // (a room called Link)
    CHECK(lines[2].name == "On");
    CHECK(lines[1].own_name);
}

TEST_CASE("coop i18n (own text): the Lobby tab's players, hosts and public games are shown as they are") {
    const Hans hans;
    CoopLobbyTab::Tab t;
    t.Show();
    CoopLobbyTab::Lobby l;
    l.games = {CoopLobbyTab::PublicGame{"link", "Link", "Public game, Players 1/8", true, true}};
    t.Set(l);
    CoopPauseMenu::View v = t.view();
    bool flagged = false;
    for (const CoopPauseMenu::Row& r : v.rows) {
        if (r.name == "Link") flagged = r.own_name;
    }
    CHECK(flagged);
    CoopPauseTab::MenuList list = CoopPauseTab::MenuLines(v, 0, 0, false, 8);
    bool seen = false;
    for (const CoopPauseTab::ListLine& line : list.lines) {
        CHECK(line.name != "林克");
        seen = seen || line.name == "Link";
    }
    CHECK(seen);

    CoopLobbyTab::Lobby in;
    in.connected = true;
    in.max_players = 4;
    CoopLobbyTab::Player me;
    me.number = 1, me.name = "Link", me.self = true, me.host = true;
    CoopLobbyTab::Player other;
    other.number = 2, other.name = "On";
    in.players = {me, other};
    t.Set(in);
    v = t.view();
    list = CoopPauseTab::MenuLines(v, 0, 0, false, 8);
    bool named = false;
    for (const CoopPauseTab::ListLine& line : list.lines) {
        CHECK(line.name != "开启");
        CHECK(line.name != "林克");
        named = named || line.name == "On";
    }
    CHECK(named);
}

TEST_CASE("coop i18n (own text): a line built around a name takes the template only, the name goes in as it is") {
    const Hans hans;
    // The names that are also words of the table, or places of the game, or read like the end of a line.
    for (const char* name : {"Link", "On", "Saria", "Hyrule Field", "Kokiri Forest", "x left", "2 items", "Someone"}) {
        CAPTURE(name);
        CHECK(CoopI18n::TrAround("Let in ", name) == std::string("放行 ") + name);
        CHECK(CoopI18n::TrAround("", name, " (you)") == std::string(name) + "（你）");
        // (the same lines as Tr() of the finished line, for the names no word or place of the game reads like)
    }
    CHECK(CoopI18n::TrAround("Let in ", "Saria") == CoopI18n::Tr("Let in Saria"));
    // Tr() itself keeps a name that is a word or a place as it is, in the chat's notices and the shapes
    CHECK(CoopI18n::Tr("Link joined") == "Link 加入了");
    CHECK(CoopI18n::Tr("On left") == "On 离开了");
    CHECK(CoopI18n::Tr("Hyrule Field joined") == "Hyrule Field 加入了");
    CHECK(CoopI18n::Tr("Let in Hyrule Field") == "放行 Hyrule Field");
    CHECK(CoopI18n::Tr("Let in Link") == "放行 Link");
    CHECK(CoopI18n::Tr("teleport: Link is coming to you") == CoopI18n::TrAround("teleport: ", "Link", " is coming to you"));
    CHECK(CoopI18n::Tr("teleport: Link is coming to you").find("林克") == std::string::npos);
    // The game's own stand-ins for no name are still the table's words, and a scene the game names is still translated.
    CHECK(CoopI18n::Tr("Someone joined") == "某人 加入了");
    CHECK(CoopI18n::Tr("teleport: stopped: you left Hyrule Field") == "传送：已停下：你离开了 海拉鲁平原");
    CHECK(CoopI18n::Tr("teleport: Hyrule Field is coming to you").find("海拉鲁平原") == std::string::npos);   // (a player's name)

    // The page's rows built around a name (Let in, Refuse, Teleport to, Kick, Joined, Room, Your game ...)
    CoopPauseTab::SessionView v;
    v.s.hosting = true;
    v.asking = {"Link"};
    std::vector<std::string> rows = CoopPauseTab::RowTexts(CoopPauseTab::View::kLetIn, v);
    CHECK(rows[0] == "放行 Link");
    CHECK(rows[1] == CoopI18n::TrAround("Refuse ", "Link"));
    CHECK(rows[1].find("林克") == std::string::npos);
    v.asking = {"Hyrule Field"};
    CHECK(CoopPauseTab::RowTexts(CoopPauseTab::View::kLetIn, v)[0] == "放行 Hyrule Field");
    CoopPauseTab::SessionView who;
    who.s.player = 2;
    who.player_name = "Link";
    CHECK(CoopPauseTab::RowTexts(CoopPauseTab::View::kPlayer, who)[0] == CoopI18n::TrAround("Teleport to ", "Link"));
    CHECK(CoopPauseTab::RowTexts(CoopPauseTab::View::kPlayer, who)[0].find("林克") == std::string::npos);
    who.s.player = 1;
    CHECK(CoopPauseTab::RowTexts(CoopPauseTab::View::kPlayer, who)[0] == "Link（你）");
    CoopPauseTab::SessionView in;
    in.s.in_one = true;
    in.host_name = "Link";
    in.room = "On";
    CHECK(CoopPauseTab::RowTexts(CoopPauseTab::View::kSession, in)[0] == CoopI18n::TrAround("Joined ", "Link"));
    CoopPauseTab::SessionView room;
    room.s.hosting = true;
    room.s.in_one = true;
    room.room = "On";
    CHECK(CoopPauseTab::RowTexts(CoopPauseTab::View::kSession, room)[1] == CoopI18n::TrAround("Copy room: ", "On"));
}

TEST_CASE("coop i18n: native rows show a table only when the game's own font set has its letters (row 139)") {
    using CoopI18n::FontSet;
    // The set the game loads for its language (romfs Font2: base, CNzh, TWzh, KRko, EUru).
    CHECK(CoopI18n::FontSetFor("en") == FontSet::kBase);
    CHECK(CoopI18n::FontSetFor("en-US") == FontSet::kBase);
    CHECK(CoopI18n::FontSetFor("ja") == FontSet::kBase);   // (the base set holds Japanese: OT-UDTypos510)
    CHECK(CoopI18n::FontSetFor("fr") == FontSet::kBase);
    CHECK(CoopI18n::FontSetFor("zh-Hans") == FontSet::kCNzh);
    CHECK(CoopI18n::FontSetFor("zh-CN") == FontSet::kCNzh);
    CHECK(CoopI18n::FontSetFor("zh-Hant") == FontSet::kTWzh);
    CHECK(CoopI18n::FontSetFor("zh-TW") == FontSet::kTWzh);
    CHECK(CoopI18n::FontSetFor("zh-HK") == FontSet::kTWzh);
    CHECK(CoopI18n::FontSetFor("ko") == FontSet::kKRko);
    CHECK(CoopI18n::FontSetFor("KO-kr") == FontSet::kKRko);
    CHECK(CoopI18n::FontSetFor("ru") == FontSet::kEUru);
    CHECK(CoopI18n::FontSetFor("rus") == FontSet::kBase);   // (not "ru")
    const CoopI18n::Lang* hans = CoopI18n::ForCode("zh-Hans");
    const CoopI18n::Lang* hant = CoopI18n::ForCode("zh-Hant");
    REQUIRE(hans != nullptr);
    REQUIRE(hant != nullptr);
    // The 0.11.12 proof: Menu language 13 (zh-Hans) on an English game drew boxes; now English.
    CHECK(CoopI18n::Shown(hans, "en") == nullptr);
    CHECK(CoopI18n::Shown(hans, "ja") == nullptr);
    CHECK(CoopI18n::Shown(hans, "ko") == nullptr);
    CHECK(CoopI18n::Shown(hans, "zh-Hant") == nullptr);
    CHECK(CoopI18n::Shown(hant, "zh-CN") == nullptr);
    // The game in that language: its font set has the letters, the table shows.
    CHECK(CoopI18n::Shown(hans, "zh-Hans") == hans);
    CHECK(CoopI18n::Shown(hans, "zh-CN") == hans);
    CHECK(CoopI18n::Shown(hant, "zh-TW") == hant);
    CHECK(CoopI18n::Shown(nullptr, "zh-CN") == nullptr);   // (English: Latin is in every set)
    // Every table we ship names a set the game has (a new table must be added to Shown's list).
    for (int i = 0; i < CoopI18n::kLangCount; ++i) {
        if (CoopI18n::kLangs[i].entries == nullptr) continue;
        CHECK(CoopI18n::Shown(&CoopI18n::kLangs[i], CoopI18n::kLangs[i].code) == &CoopI18n::kLangs[i]);
    }
}

TEST_CASE("coop i18n: the Gold Skulltulas row's joined help lines are translated whole") {
    // Review of f36a8852: HostRuleLines joins two table lines; Tr has a line for each joined shape.
    for (const char* code : {"zh-Hans", "zh-Hant"}) {
        CAPTURE(code);
        const CoopI18n::Lang* lang = ByCode(code);
        REQUIRE(lang != nullptr);
        for (const char* first : {"Applies at your next Host: stop hosting, then start again.",
                                  "Your own setting for the next game you host; the game you are in keeps its host's."}) {
            for (const char* now : {"This game: Shared.", "This game: Not shared."}) {
                const std::string en = std::string(first) + " " + now;
                CAPTURE(en);
                const std::string zh = CoopI18n::Tr(lang, en);
                CHECK(zh == CoopI18n::Tr(lang, first) + CoopI18n::Tr(lang, now));
            }
        }
    }
}
