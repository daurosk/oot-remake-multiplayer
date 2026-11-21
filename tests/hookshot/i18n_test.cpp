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
    CHECK(ng.find("保留你的 4+1 颗心") != std::string::npos);
    CHECK(ng.find("最高 NG+ 2") != std::string::npos);
    CHECK(ng.find("Start a new file") == std::string::npos);
    // The Saves row's help, whole.
    const std::string saves = CoopI18n::Tr(
        hans, "Solo: your own saves. Multiplayer: a separate set of saves for playing together (made from your "
              "solo saves the first time), so co-op progress never mixes with your own. When you join a friend "
              "from the title screen you play the host's save, copied beside your multiplayer saves; your saves "
              "are not touched. Switched on the title screen with no file loaded.");
    CHECK(saves.rfind("单人：你自己的存档。", 0) == 0);
    CHECK(saves.find("联机：一套单独用于一起游玩的存档") != std::string::npos);
}

TEST_CASE("coop i18n: the pause menu's Multiplayer tab (the game's own page)") {
    const CoopI18n::Lang* hans = ByCode("zh-Hans");
    const CoopI18n::Lang* hant = ByCode("zh-Hant");
    // The rows (coop_pause_tab.cpp's RowTexts, translated as they are handed to the game): the words
    // are the tab's own, one line each, and the rows with a value are the table's shapes.
    CHECK(CoopI18n::Tr(hans, "Hosting") == "房间已开");
    CHECK(CoopI18n::Tr(hant, "Hosting") == "房間已開");
    CHECK(CoopI18n::Tr(hans, "Copy invite code") == "复制邀请码");
    CHECK(CoopI18n::Tr(hans, "Players: just you") == "人数：只有你");
    CHECK(CoopI18n::Tr(hans, "Stop hosting") == "停止房间");
    CHECK(CoopI18n::Tr(hans, "Host a game") == "创建房间");
    CHECK(CoopI18n::Tr(hans, "Join a game") == "加入房间");
    // The rows with a value: the whole row is the shape's line.
    CHECK(CoopI18n::Tr(hans, "Room: 7QK-2") == "房间：7QK-2");
    CHECK(CoopI18n::Tr(hans, "Copy room: 7QK-2") == "复制房间：7QK-2");
    CHECK(CoopI18n::Tr(hans, "Players: 3") == "人数：3");
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
    CHECK(pc.rfind("没有找到叫 LIVINGROOM 的电脑", 0) == 0);
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
