// SPDX-License-Identifier: GPL-3.0-or-later
// One table per language, in the shape Longshot's own menu uses
// (gpu/vk/menu_i18n.h): {"English", "its text"}. Machine-made at first, so a
// speaker's correction is one line. The tab builds a few lines around a number
// or a name ("Hosting: 3 players", "Join Saria again"): those match by their
// ends (Around, below), as the menu's own lines with printf conversions do.
//
// Which strings hit which table: everything the module draws itself -- the
// tab's strip name, its rows, the help box, the gold line, the notes, the
// plain status lines of Host and Join, the title screen's prompt, the chat
// box's placeholder, the cheats panel's header, the player list's cells --
// goes through Tr() here. Texts Hookshot itself draws (the prompt strip's
// "Choose", "Tabs", "Close"; a toggle's Off and On; the invalidity of a
// table line) are looked up by Hookshot's own menu_i18n table first, since
// gpu/vk/vk_ui.h passes every string it draws through i18n::tr(). The lines
// this table does not know stay English.
#include "coop_i18n.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace CoopI18n {
namespace {

// ---------------------------------------------------------------------------
// Simplified Chinese.

constexpr Entry kzh_Hans[] = {
    // ---- The tab and its pages -------------------------------------------
    {"Multiplayer", "联机"},
    {"Host a game", "创建房间"},
    {"Join a game", "加入房间"},
    {"Local co-op", "本地双人"},
    {"Settings", "设置"},
    {"Commands", "命令"},

    // ---- The rows' names and values --------------------------------------
    {"Hosting", "房间已开"},
    {"Starting...", "启动中…"},
    {"Joined", "已加入"},
    {"Joining...", "加入中…"},
    {"Copy the code again", "再复制一次邀请码"},
    {"Copy", "复制"},
    {"Paste friend's answer", "粘贴朋友的应答码"},
    {"Paste", "粘贴"},
    {"Same network or VPN", "同一网络或 VPN"},
    {"Copy the line", "复制这一行"},
    {"Stop hosting", "停止房间"},
    {"Start hosting", "创建房间"},
    {"Stop", "停止"},
    {"Start", "开始"},
    {"Press again to confirm", "再按一次确认"},
    {"Players", "人数"},
    {"Copy your answer code", "复制你的应答码"},
    {"Leave the game", "离开房间"},
    {"Leave", "离开"},
    {"Join", "加入"},
    {"Friend's code", "朋友的邀请码"},
    {"Or a new code", "或新的邀请码"},
    {"Open the chat", "打开聊天"},
    {"Your name", "你的名字"},
    {"Link's look", "林克外观"},
    {"By player number", "按玩家编号"},
    {"Players when you host", "开房人数"},
    {"Proximity voice chat", "附近语音"},
    {"Off", "关闭"},
    {"On", "开启"},

    // ---- Bottom prompts the module hands over ----------------------------
    // (Hookshot looks the ones it draws itself up too; these cover the module's
    // own page words, which reach the strip through HosCoopMenuPage.)
    {"Open", "打开"},
    {"Type", "输入"},
    {"Choose", "执行"},
    {"Change", "更改"},
    {"Back", "返回"},
    {"Close", "关闭"},
    {"Done", "完成"},
    {"Cancel", "取消"},

    // ---- The pages' help -------------------------------------------------
    {"Starts a game on this PC and makes a code. Send the code to your friends; they choose Join a "
     "game and paste it. They play on your save.",
     "在本机开房并生成邀请码。发给朋友，他们选「加入房间」粘贴即可，在你的存档上玩。"},
    {"Joins a friend's game with the code they sent you. You play on their save; your own saves "
     "are not touched.",
     "用朋友发来的邀请码加入。你在他存档上玩，自己的存档不动。"},
    {"You are in a friend's game. To host your own, leave it first: Join a game, then Leave the game.",
     "你在朋友的房间里。想自己开房，先离开：加入房间 → 离开房间。"},
    {"Your game: the code for your friends, who joined, and Stop hosting.",
     "你的房间：邀请码、已加入的人、停止房间。"},
    {"You are hosting. To join a friend's game, stop yours first: Host a game, then Stop hosting.",
     "你正在开房。想加入朋友的房间，先停止自己的：创建房间 → 停止房间。"},
    {"The game you joined, and Leave the game.", "你加入的房间，以及离开房间。"},
    {"Two players on this PC.", "同一台电脑两人游玩。"},
    {"Your name, Link's look, how many players can join your game, and voice chat.",
     "名字、林克外观、房间人数上限、语音聊天。"},
    {"The chat's commands, and the key that opens the chat.", "聊天命令，以及打开聊天的按键。"},
    {"Puts your code on the clipboard again, to paste into your chat with your friends.",
     "把邀请码再放进剪贴板，方便发给朋友。"},
    {"Only when a friend's game shows an answer code (their way in was blocked): copy the code they "
     "send you, then press this.",
     "仅当朋友的游戏显示「应答码」（他被路由器挡住）时：复制他发来的码，再按这里。"},
    {"For friends on your home network or on a VPN with you (Radmin VPN, Hamachi, Tailscale): copies "
     "one line with this PC's address. They paste it in Join a game instead of the code.",
     "给同屋或同一 VPN（Radmin VPN、Hamachi、Tailscale）的朋友：复制含本机地址的一行，他加入时粘贴这一行而不是邀请码。"},
    {"Ends your game for everyone in it.", "结束房间，所有人都退出。"},
    {"Starts a game on this PC and makes a code for your friends.", "在本机开房并生成邀请码。"},
    {"The most players in your game, you included.", "房间人数上限（含你自己）。"},
    {"Your friend's router blocks the way in: send them this answer code (it is copied "
     "already); they paste it on their Host page.",
     "朋友的路由器挡住了入口：把这个应答码发给他（已复制），他在「创建房间」页粘贴。"},
    {"Leaves your friend's game. Their game goes on without you.", "离开朋友的房间，他的房间继续。"},
    {"Joins the last game you were in, with the same code. It works while your friend is still "
     "hosting that game.",
     "用同一邀请码回到上次的房间（朋友仍在开房时有效）。"},
    {"Copy the code from your friend's message, then press this: it is pasted. On the same home "
     "network or VPN as your friend? Paste the line from Same network or VPN on their Host page "
     "instead (address and password). You can also type it.",
     "复制朋友发来的邀请码后按这里即可粘贴。同屋或同 VPN？改为粘贴他「同一网络或 VPN」那一行（地址+密码）。也可以手动输入。"},
    {"Paste your friend's code first.", "请先粘贴朋友的邀请码。"},
    {"Joins your friend's game.", "加入朋友的房间。"},
    {"Split screen: two players on this PC.", "分屏：同一台电脑两人游玩。"},
    {"Coming in the next test release.", "下个测试版提供。"},
    {"The chat key is off (LICHII_COOP_CHAT_KEY=off in coop-options.ini): set a key "
     "there (a letter, a digit, or F1 to F12) to chat.",
     "聊天键已关闭（coop-options.ini 里 LICHII_COOP_CHAT_KEY=off）：在那里设一个键（字母、数字或 F1–F12）即可聊天。"},

    // ---- The Commands page -----------------------------------------------
    {"Lists the other players, with their numbers.", "列出其他玩家及编号。"},
    {"Takes you to that player: the whole name, the start of it, or the player's number (/teleport 2). In another "
     "place, talk to someone or read a sign within a minute to travel. Not during a cutscene, a talk or a boss "
     "fight.",
     "传送到该玩家：全名、名字开头或编号（/teleport 2）。跨区域时，先在一分钟内和某人对话或读告示牌。过场、对话、Boss 战中不可用。"},
    {"The same as /teleport.", "同 /teleport。"},
    {"Says whether players can hurt each other (PvP) in this game.", "查看本房间是否允许玩家互相伤害（PvP）。"},
    {"The host only: lets players hurt each other, or stops it, for everyone.",
     "仅房主：为所有人开启或关闭玩家互相伤害。"},

    // ---- The Settings page ----------------------------------------------
    {"The name the other players see: 1 to 16 characters, typed with the keyboard.",
     "其他玩家看到的名字：1–16 个字符，用键盘输入。"},
    {"The colour of your Link's tunic and cap, as you and the other players see it. By player "
     "number: each player's own colour.",
     "你的林克衣服和帽子的颜色（你和别人看到的）。按玩家编号：每人一个颜色。"},
    {"The most players in a game you host, you included.", "你开房时的人数上限（含你自己）。"},
    {"Talk with players near your Link through your microphone; they sound quieter the farther away "
     "they are. Off: you neither send nor hear voices.",
     "用麦克风和靠近你的玩家说话；距离越远声音越小。关闭：不收也不发。"},

    // ---- Notes, the gold line, the banner --------------------------------
    {"Your name: 1 to 16 characters. It was not changed.", "名字需 1–16 个字符，未修改。"},
    {"Nothing to paste: copy the code from your friend's message first. You can also type it.",
     "没有可粘贴的内容：先复制朋友的邀请码；也可以手动输入。"},
    {"Co-op is on", "联机已开启"},
    {"Starting your game...", "正在创建房间…"},
    {"Hosting: waiting for friends", "房间已开：等待朋友加入"},
    {"In a friend's game", "已在朋友房间"},
    {"Co-op started with the game (Host.bat, Join.bat or coop.ini). To host or join another game, quit and start the "
     "game again.",
     "本局联机是随游戏启动的（Host.bat、Join.bat 或 coop.ini）。要另开或加入其他房间，请退出游戏重开。"},

    // ---- Notes from the tab's own actions -------------------------------
    {"Co-op is on already in this game: stop it first, or start the game again.",
     "本局联机已经在跑了：先停止，或者重开游戏。"},
    {"Co-op stopped. You play on alone.", "联机已停止，你独自游玩。"},
    {"The server is not running yet.", "服务器还没起来。"},
    {"Copy your friend's answer code first (they send it to you), then press this again.",
     "请先复制朋友发来的应答码（他会发给你），再按一次这里。"},
    {"That code didn't work: ask your friend to copy it again.", "这个码无效：请朋友再复制一次。"},
    {"The clipboard could not be used: write the code down instead.", "剪贴板用不了：请把码记下来。"},
    {"Answer code taken: connecting your friend...", "已接受应答码：正在连接朋友…"},
    {"Connected!", "已连接！"},
    {"A settings file could not be written.", "有一个设置文件写不进去。"},
    {"No co-op kit found with this game: only your name was saved (coop.ini).",
     "这个游戏旁边没找到联机套件：只保存了你的名字（coop.ini）。"},
    {"Saved. Co-op is on: some settings count the next time you host or join.",
     "已保存。本局联机在跑：部分设置下次开房/加入时才生效。"},
    {"Link wears it the next time the game starts.", "林克下次启动游戏时换上这个颜色。"},

    // ---- Lines built around a code, a name or a number -------------------
    // Tr()'s shapes fill in the middle where the {} is: "Hosting: 3 players"
    // is looked up as "Hosting: {} players". A middle that is a word of the
    // table too ("invite code", "hosting") is translated with it.
    {"No {} yet.", "还没有{}。"},
    {"Copied: {}", "已复制：{}"},
    {"Join {} again", "再次加入 {}"},
    {"Hosting: {} players", "房间中：{} 人"},
    {"Co-op is on: {} players", "联机已开启：{} 人"},
    {"{} friend(s) joined.", "{} 位朋友已加入。"},
    {"Friends on your {} paste this line in Join (address and password):",
     "同属{}的朋友把这一行粘贴到「加入房间」（地址+密码）："},
    {"Getting {}'s save...", "正在获取 {} 的存档…"},
    {"Loading {}'s save...", "正在载入 {} 的存档…"},
    {"{}'s save did not arrive: Continue plays your own save, New Game starts a new one.",
     "{} 的存档没有送到：继续游戏用自己的存档，新游戏开一个新的。"},
    {"{} has no save yet: New Game starts one, Continue plays your own.",
     "{} 还没有存档：新游戏开一个，继续游戏用自己的。"},
    {"Press Continue to play {}'s save.", "按「继续游戏」玩 {} 的存档。"},
    {"Press {} in the game to open the chat box; Enter sends, Esc closes. A line that starts with / is a command, "
     "not chat. Another key: LICHII_COOP_CHAT_KEY in coop-options.ini.",
     "游戏中按 {} 打开聊天框；回车发送，Esc 关闭；以 / 开头的行是命令。改键：coop-options.ini 的 LICHII_COOP_CHAT_KEY。"},
    {"invite code", "邀请码"},
    {"answer code", "应答码"},

    // ---- Host's and Join's plain status (PlainStatus) --------------------
    {"That answer code didn't work: ask your friend to copy it again.",
     "这个应答码无效：请朋友再复制一次。"},
    {"Your code for friends is still:", "给朋友的邀请码仍是："},
    {"That code is not right.", "这个码不对。"},
    {"Ask your friend to copy their code again and send it to you.",
     "请朋友再复制一次他的码发给你。"},
    {"Another co-op window is already open on this PC.", "这台电脑上已经开着一个联机窗口。"},
    {"Close it (or restart the PC), then press Host again.",
     "关掉它（或重启电脑），再按一次「创建房间」。"},
    {"Close it (or restart the PC), then press Join again.",
     "关掉它（或重启电脑），再按一次「加入房间」。"},
    {"Co-op could not start.", "联机没能启动。"},
    {"Check that this PC is online, then try again. If it keeps failing, restart the game.",
     "先确认这台电脑联网，再试一次。一直失败就重启游戏。"},
    {"Your name is too long.", "你的名字太长。"},
    {"Pick a shorter one in Multiplayer, Settings.", "在「联机 → 设置」里换一个短一点的。"},
    {"Send this code to your friends:", "把这个邀请码发给朋友："},
    {"It is copied: paste it into your chat with them (Ctrl+V).",
     "已复制：粘贴到和朋友的聊天里（Ctrl+V）。"},
    {"Press \"Copy the code again\", then paste it into your chat with them.",
     "按「再复制一次邀请码」，再粘贴到和朋友的聊天里。"},
    {"Waiting for your friends to join...", "正在等朋友加入…"},
    {"Making your code...", "正在生成邀请码…"},
    {"No code could be made: your internet connection blocks it.",
     "生成不了邀请码：你的网络挡住了。"},
    {"Friends on the same home network or VPN can still join.",
     "同屋或同一 VPN 的朋友仍然能加入。"},
    {"Your router may block friends: if they cannot join, turn on UPnP in the router's settings.",
     "路由器可能挡住朋友：如果他们进不来，在路由器设置里打开 UPnP。"},
    {"Hosting did not start.", "房间没有开起来。"},
    {"Check that this PC is online, then press Host again.",
     "先确认这台电脑联网，再按一次「创建房间」。"},
    {"Joining your friend...", "正在加入朋友的房间…"},
    {"Your friend's router blocks the way in.", "朋友的路由器挡住了入口。"},
    {"Send your friend this answer code; they paste it into their game:",
     "把这个应答码发给朋友，让他粘贴进游戏："},
    {"Or ask them to turn on UPnP in their router's settings and host again.",
     "或者请他打开路由器的 UPnP 再开一次房。"},
    {"Connected! Choose your file and play.", "已连接！选择存档开始玩。"},
    {"Your friend could not be reached.", "联系不上你的朋友。"},
    {"Check that they are hosting right now and that the code is their newest one.",
     "确认他此刻正在开房，而且这个码是他最新的。"},
    {"Same home network or VPN as them? Ask for the line from \"Same network or VPN\" on their Host page.",
     "和他同屋或同一 VPN？让他给你「同一网络或 VPN」那一行。"},

    // ---- Same network or VPN (LanLines) ----------------------------------
    {"No home-network or VPN address was found on this PC.",
     "这台电脑上没找到内网或 VPN 地址。"},
    {"On a VPN? Start your VPN program (Radmin VPN, Hamachi), then press this again.",
     "已复制：粘贴到和朋友的聊天里（Ctrl+V）。"},
    {"Type it into your chat with them, exactly as it is.",
     "原样敲进和朋友的聊天里。"},
    {"Or with another address in front:", "或者用另一个地址开头："},
    {"If Windows asks about the firewall, allow the game on private networks.",
     "如果 Windows 问防火墙，允许游戏在专用网络上通行。"},
    {"home network", "内网"},

    // ---- Join's checks, and the host's save at the title -----------------
    {"Paste or type the invite code your host sent you first.",
     "请先粘贴或输入房主发来的邀请码。"},
    {"That invite code is not right; ask the host to copy it again.",
     "这个邀请码不对：请房主再复制一次。"},
    {"The join password after the address: no spaces, at most 128 characters.",
     "地址后面的加入密码：不能有空格，最多 128 个字符。"},
    {"That is not an invite code or a server address.", "按「继续游戏」玩 {} 的存档。"},

    // ---- The title screen's prompt ---------------------------------------
    {"Multiplayer: {}", "联机：{}"},
    {"hosting", "开房中"},
    {"joining", "加入中"},
    {"joined", "已加入"},

    // ---- The chat box and the player list --------------------------------
    {"Say something (Enter sends, Esc closes)", "说点什么（回车发送，Esc 关闭）"},
    {"Player", "玩家"},
    {"State", "状态"},
    {"Ping", "延迟"},
    {"Place", "地点"},
    {"Scene", "场景"},
    {" (you)", "（你）"},
    {"?", "?"},

    // ---- The cheats panel ------------------------------------------------
    {"Co-op item cheats (debug): a click or Enter gives, F8 or Esc closes",
     "联机物品作弊（调试）：点击或回车给予，F8 或 Esc 关闭"},
    {"Find: ", "查找："},
    {"Quick gifts (type to find any item flag):", "快捷礼物（输入可查找任意物品标记）："},
    {"Item flags with that in the name:", "名字里含该字串的物品标记："},
    {"(none)", "（无）"},
    {"Gold Skulltula token (+1)", "金骷髅徽章（+1）"},
    {"Heart piece (+1, count only, no heart shown)", "心之碎片（+1，只计数，不显示心）"},
    {"Heart container (+1 heart)", "心之容器（+1 颗心）"},
    {"Small key, Forest Temple (+1)", "小钥匙，森之神殿（+1）"},
    {"Hookshot", "钩索"},
    {"Song of Storms", "暴风之歌"},
};

// ---------------------------------------------------------------------------
// Traditional Chinese.

constexpr Entry kzh_Hant[] = {
    {"Multiplayer", "連線"},
    {"Host a game", "建立房間"},
    {"Join a game", "加入房間"},
    {"Local co-op", "本機雙人"},
    {"Settings", "設定"},
    {"Commands", "指令"},

    {"Hosting", "房間已開"},
    {"Starting...", "啟動中…"},
    {"Joined", "已加入"},
    {"Joining...", "加入中…"},
    {"Copy the code again", "再複製一次邀請碼"},
    {"Copy", "複製"},
    {"Paste friend's answer", "貼上朋友的應答碼"},
    {"Paste", "貼上"},
    {"Same network or VPN", "同一網路或 VPN"},
    {"Copy the line", "複製這一行"},
    {"Stop hosting", "停止房間"},
    {"Start hosting", "建立房間"},
    {"Stop", "停止"},
    {"Start", "開始"},
    {"Press again to confirm", "再按一次確認"},
    {"Players", "人數"},
    {"Copy your answer code", "複製你的應答碼"},
    {"Leave the game", "離開房間"},
    {"Leave", "離開"},
    {"Join", "加入"},
    {"Friend's code", "朋友的邀請碼"},
    {"Or a new code", "或新的邀請碼"},
    {"Open the chat", "開啟聊天"},
    {"Your name", "你的名字"},
    {"Link's look", "林克外觀"},
    {"By player number", "依玩家編號"},
    {"Players when you host", "開房人數"},
    {"Proximity voice chat", "鄰近語音"},
    {"Off", "關閉"},
    {"On", "開啟"},

    {"Open", "打開"},
    {"Type", "輸入"},
    {"Choose", "執行"},
    {"Change", "更改"},
    {"Back", "返回"},
    {"Close", "關閉"},
    {"Done", "完成"},
    {"Cancel", "取消"},

    {"Starts a game on this PC and makes a code. Send the code to your friends; they choose Join a "
     "game and paste it. They play on your save.",
     "在本機開房並產生邀請碼。傳給朋友，他們選「加入房間」貼上即可，在你的存檔上玩。"},
    {"Joins a friend's game with the code they sent you. You play on their save; your own saves "
     "are not touched.",
     "用朋友傳來的邀請碼加入。你在他存檔上玩，自己的存檔不動。"},
    {"You are in a friend's game. To host your own, leave it first: Join a game, then Leave the game.",
     "你在朋友的房間裡。想自己開房，先離開：加入房間 → 離開房間。"},
    {"Your game: the code for your friends, who joined, and Stop hosting.",
     "你的房間：邀請碼、已加入的人、停止房間。"},
    {"You are hosting. To join a friend's game, stop yours first: Host a game, then Stop hosting.",
     "你正在開房。想加入朋友的房間，先停止自己的：建立房間 → 停止房間。"},
    {"The game you joined, and Leave the game.", "你加入的房間，以及離開房間。"},
    {"Two players on this PC.", "同一台電腦兩人遊玩。"},
    {"Your name, Link's look, how many players can join your game, and voice chat.",
     "名字、林克外觀、房間人數上限、語音聊天。"},
    {"The chat's commands, and the key that opens the chat.", "聊天指令，以及開啟聊天的按鍵。"},
    {"Puts your code on the clipboard again, to paste into your chat with your friends.",
     "把邀請碼再放進剪貼簿，方便傳給朋友。"},
    {"Only when a friend's game shows an answer code (their way in was blocked): copy the code they "
     "send you, then press this.",
     "僅當朋友的遊戲顯示「應答碼」（他被路由器擋住）時：複製他傳來的碼，再按這裡。"},
    {"For friends on your home network or on a VPN with you (Radmin VPN, Hamachi, Tailscale): copies "
     "one line with this PC's address. They paste it in Join a game instead of the code.",
     "給同屋或同一 VPN（Radmin VPN、Hamachi、Tailscale）的朋友：複製含本機位址的一行，他加入時貼上這一行而不是邀請碼。"},
    {"Ends your game for everyone in it.", "結束房間，所有人都退出。"},
    {"Starts a game on this PC and makes a code for your friends.", "在本機開房並產生邀請碼。"},
    {"The most players in your game, you included.", "房間人數上限（含你自己）。"},
    {"Your friend's router blocks the way in: send them this answer code (it is copied "
     "already); they paste it on their Host page.",
     "朋友的路由器擋住了入口：把這個應答碼傳給他（已複製），他在「建立房間」頁貼上。"},
    {"Leaves your friend's game. Their game goes on without you.", "離開朋友的房間，他的房間繼續。"},
    {"Joins the last game you were in, with the same code. It works while your friend is still "
     "hosting that game.",
     "用同一邀請碼回到上次的房間（朋友仍在開房時有效）。"},
    {"Copy the code from your friend's message, then press this: it is pasted. On the same home "
     "network or VPN as your friend? Paste the line from Same network or VPN on their Host page "
     "instead (address and password). You can also type it.",
     "複製朋友傳來的邀請碼後按這裡即可貼上。同屋或同 VPN？改為貼上他「同一網路或 VPN」那一行（位址+密碼）。也可以手動輸入。"},
    {"Paste your friend's code first.", "請先貼上朋友的邀請碼。"},
    {"Joins your friend's game.", "加入朋友的房間。"},
    {"Split screen: two players on this PC.", "分割畫面：同一台電腦兩人遊玩。"},
    {"Coming in the next test release.", "下個測試版提供。"},
    {"The chat key is off (LICHII_COOP_CHAT_KEY=off in coop-options.ini): set a key "
     "there (a letter, a digit, or F1 to F12) to chat.",
     "聊天鍵已關閉（coop-options.ini 裡 LICHII_COOP_CHAT_KEY=off）：在那裡設一個鍵（字母、數字或 F1–F12）即可聊天。"},

    {"Lists the other players, with their numbers.", "列出其他玩家及編號。"},
    {"Takes you to that player: the whole name, the start of it, or the player's number (/teleport 2). In another "
     "place, talk to someone or read a sign within a minute to travel. Not during a cutscene, a talk or a boss "
     "fight.",
     "傳送到該玩家：全名、名字開頭或編號（/teleport 2）。跨區域時，先在一分鐘內和某人對話或讀告示牌。過場、對話、Boss 戰中不可用。"},
    {"The same as /teleport.", "同 /teleport。"},
    {"Says whether players can hurt each other (PvP) in this game.", "查看本房間是否允許玩家互相傷害（PvP）。"},
    {"The host only: lets players hurt each other, or stops it, for everyone.",
     "僅房主：為所有人開啟或關閉玩家互相傷害。"},

    {"The name the other players see: 1 to 16 characters, typed with the keyboard.",
     "其他玩家看到的名字：1–16 個字元，用鍵盤輸入。"},
    {"The colour of your Link's tunic and cap, as you and the other players see it. By player "
     "number: each player's own colour.",
     "你的林克衣服和帽子的顏色（你和別人看到的）。依玩家編號：每人一個顏色。"},
    {"The most players in a game you host, you included.", "你開房時的人數上限（含你自己）。"},
    {"Talk with players near your Link through your microphone; they sound quieter the farther away "
     "they are. Off: you neither send nor hear voices.",
     "用麥克風和靠近你的玩家說話；距離越遠聲音越小。關閉：不收也不發。"},

    {"Your name: 1 to 16 characters. It was not changed.", "名字需 1–16 個字元，未修改。"},
    {"Nothing to paste: copy the code from your friend's message first. You can also type it.",
     "沒有可貼上的內容：先複製朋友的邀請碼；也可以手動輸入。"},
    {"Co-op is on", "連線已開啟"},
    {"Starting your game...", "正在建立房間…"},
    {"Hosting: waiting for friends", "房間已開：等待朋友加入"},
    {"In a friend's game", "已在朋友房間"},
    {"Co-op started with the game (Host.bat, Join.bat or coop.ini). To host or join another game, quit and start the "
     "game again.",
     "本局連線是隨遊戲啟動的（Host.bat、Join.bat 或 coop.ini）。要另開或加入其他房間，請退出遊戲重開。"},

    {"Co-op is on already in this game: stop it first, or start the game again.",
     "本局連線已經在跑了：先停止，或者重開遊戲。"},
    {"Co-op stopped. You play on alone.", "連線已停止，你獨自遊玩。"},
    {"The server is not running yet.", "伺服器還沒起來。"},
    {"Copy your friend's answer code first (they send it to you), then press this again.",
     "請先複製朋友傳來的應答碼（他會傳給你），再按一次這裡。"},
    {"That code didn't work: ask your friend to copy it again.", "這個碼無效：請朋友再複製一次。"},
    {"The clipboard could not be used: write the code down instead.", "剪貼簿用不了：請把碼記下來。"},
    {"Answer code taken: connecting your friend...", "已接受應答碼：正在連線朋友…"},
    {"Connected!", "已連線！"},
    {"A settings file could not be written.", "有一個設定檔寫不進去。"},
    {"No co-op kit found with this game: only your name was saved (coop.ini).",
     "這個遊戲旁邊沒找到連線套件：只儲存了你的名字（coop.ini）。"},
    {"Saved. Co-op is on: some settings count the next time you host or join.",
     "已儲存。本局連線在跑：部分設定下次開房/加入時才生效。"},
    {"Link wears it the next time the game starts.", "林克下次啟動遊戲時換上這個顏色。"},

    {"No {} yet.", "還沒有{}。"},
    {"Copied: {}", "已複製：{}"},
    {"Join {} again", "再次加入 {}"},
    {"Hosting: {} players", "房間中：{} 人"},
    {"Co-op is on: {} players", "連線已開啟：{} 人"},
    {"{} friend(s) joined.", "{} 位朋友已加入。"},
    {"Friends on your {} paste this line in Join (address and password):",
     "同屬{}的朋友把這一行貼到「加入房間」（位址+密碼）："},
    {"Getting {}'s save...", "正在取得 {} 的存檔…"},
    {"Loading {}'s save...", "正在載入 {} 的存檔…"},
    {"{}'s save did not arrive: Continue plays your own save, New Game starts a new one.",
     "{} 的存檔沒有送到：繼續遊戲用自己的存檔，新遊戲開一個新的。"},
    {"{} has no save yet: New Game starts one, Continue plays your own.",
     "{} 還沒有存檔：新遊戲開一個，繼續遊戲用自己的。"},
    {"Press Continue to play {}'s save.", "按「繼續遊戲」玩 {} 的存檔。"},
    {"Press {} in the game to open the chat box; Enter sends, Esc closes. A line that starts with / is a command, "
     "not chat. Another key: LICHII_COOP_CHAT_KEY in coop-options.ini.",
     "遊戲中按 {} 開啟聊天框；Enter 傳送，Esc 關閉；以 / 開頭的行是指令。改鍵：coop-options.ini 的 LICHII_COOP_CHAT_KEY。"},
    {"invite code", "邀請碼"},
    {"answer code", "應答碼"},
    {"Multiplayer: {}", "連線：{}"},

    {"That answer code didn't work: ask your friend to copy it again.",
     "這個應答碼無效：請朋友再複製一次。"},
    {"Your code for friends is still:", "給朋友的邀請碼仍是："},
    {"That code is not right.", "這個碼不對。"},
    {"Ask your friend to copy their code again and send it to you.",
     "請朋友再複製一次他的碼傳給你。"},
    {"Another co-op window is already open on this PC.", "這台電腦上已經開著一個連線視窗。"},
    {"Close it (or restart the PC), then press Host again.",
     "關掉它（或重開電腦），再按一次「建立房間」。"},
    {"Close it (or restart the PC), then press Join again.",
     "關掉它（或重開電腦），再按一次「加入房間」。"},
    {"Co-op could not start.", "連線沒能啟動。"},
    {"Check that this PC is online, then try again. If it keeps failing, restart the game.",
     "先確認這台電腦連網，再試一次。一直失敗就重開遊戲。"},
    {"Your name is too long.", "你的名字太長。"},
    {"Pick a shorter one in Multiplayer, Settings.", "在「連線 → 設定」裡換一個短一點的。"},
    {"Send this code to your friends:", "把這個邀請碼傳給朋友："},
    {"It is copied: paste it into your chat with them (Ctrl+V).",
     "已複製：貼到和朋友的聊天裡（Ctrl+V）。"},
    {"Press \"Copy the code again\", then paste it into your chat with them.",
     "按「再複製一次邀請碼」，再貼到和朋友的聊天裡。"},
    {"Waiting for your friends to join...", "{} 位朋友已加入。"},
    {"Making your code...", "正在產生邀請碼…"},
    {"No code could be made: your internet connection blocks it.",
     "產生不了邀請碼：你的網路擋住了。"},
    {"Friends on the same home network or VPN can still join.",
     "同屋或同一 VPN 的朋友仍然能加入。"},
    {"Your router may block friends: if they cannot join, turn on UPnP in the router's settings.",
     "路由器可能擋住朋友：如果他們進不來，在路由器設定裡打開 UPnP。"},
    {"Hosting did not start.", "房間沒有開起來。"},
    {"Check that this PC is online, then press Host again.",
     "先確認這台電腦連網，再按一次「建立房間」。"},
    {"Joining your friend...", "正在加入朋友的房間…"},
    {"Your friend's router blocks the way in.", "朋友的路由器擋住了入口。"},
    {"Send your friend this answer code; they paste it into their game:",
     "把這個應答碼傳給朋友，讓他貼進遊戲："},
    {"Or ask them to turn on UPnP in their router's settings and host again.",
     "或者請他打開路由器的 UPnP 再開一次房。"},
    {"Connected! Choose your file and play.", "已連線！選擇存檔開始玩。"},
    {"Your friend could not be reached.", "聯絡不上你的朋友。"},
    {"Check that they are hosting right now and that the code is their newest one.",
     "確認他此刻正在開房，而且這個碼是他最新的。"},
    {"Same home network or VPN as them? Ask for the line from \"Same network or VPN\" on their Host page.",
     "和他同屋或同一 VPN？讓他給你「同一網路或 VPN」那一行。"},

    {"No home-network or VPN address was found on this PC.",
     "這台電腦上沒找到內網或 VPN 位址。"},
    {"On a VPN? Start your VPN program (Radmin VPN, Hamachi), then press this again.",
     "已複製：貼到和朋友的聊天裡（Ctrl+V）。"},
    {"Type it into your chat with them, exactly as it is.",
     "原樣打進和朋友的聊天裡。"},
    {"Or with another address in front:", "或者用另一個位址開頭："},
    {"If Windows asks about the firewall, allow the game on private networks.",
     "如果 Windows 問防火牆，允許遊戲在專用網路上通行。"},
    {"home network", "內網"},

    {"Paste or type the invite code your host sent you first.",
     "請先貼上或輸入房主傳來的邀請碼。"},
    {"That invite code is not right; ask the host to copy it again.",
     "這個邀請碼不對：請房主再複製一次。"},
    {"The join password after the address: no spaces, at most 128 characters.",
     "位址後面的加入密碼：不能有空格，最多 128 個字元。"},
    {"That is not an invite code or a server address.", "連線：{}"},
    {"hosting", "開房中"},
    {"joining", "加入中"},
    {"joined", "已加入"},

    {"Say something (Enter sends, Esc closes)", "說點什麼（Enter 傳送，Esc 關閉）"},
    {"Player", "玩家"},
    {"State", "狀態"},
    {"Ping", "延遲"},
    {"Place", "地點"},
    {"Scene", "場景"},
    {" (you)", "（你）"},
    {"?", "?"},

    {"Co-op item cheats (debug): a click or Enter gives, F8 or Esc closes",
     "連線物品作弊（除錯）：點擊或 Enter 給予，F8 或 Esc 關閉"},
    {"Find: ", "尋找："},
    {"Quick gifts (type to find any item flag):", "快捷禮物（輸入可尋找任意物品標記）："},
    {"Item flags with that in the name:", "名字裡含該字串的物品標記："},
    {"(none)", "（無）"},
    {"Gold Skulltula token (+1)", "金骷髏徽章（+1）"},
    {"Heart piece (+1, count only, no heart shown)", "心之碎片（+1，只計數，不顯示心）"},
    {"Heart container (+1 heart)", "心之容器（+1 顆心）"},
    {"Small key, Forest Temple (+1)", "小鑰匙，森之神殿（+1）"},
    {"Hookshot", "鉤索"},
    {"Song of Storms", "暴風之歌"},
};

std::string Trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
    return s;
}

// language.txt beside the game (the file Longshot itself reads), then
// HOS_LANGUAGE when the game was started with it.
std::string ReadLanguage() {
    std::vector<std::string> sources;
    if (const char* env = std::getenv("HOS_LANGUAGE")) {
        sources.emplace_back(env);
    }
#ifdef _WIN32
    char exe[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, exe, MAX_PATH) != 0) {
        std::string path = exe;
        const std::size_t cut = path.find_last_of("\\/");
        if (cut != std::string::npos) {
            std::ifstream file(path.substr(0, cut) + "\\language.txt");
            std::string line;
            while (std::getline(file, line)) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                const std::string text = Trim(line);
                if (!text.empty() && text.front() != '#') {
                    sources.push_back(text);
                    break;
                }
            }
        }
    }
#endif
    for (const std::string& code : sources) {
        if (!code.empty()) {
            return code;
        }
    }
    return "en";
}

const Entry* Find(const Lang& lang, std::string_view en) {
    for (int i = 0; i < lang.count; ++i) {
        if (en == lang.entries[i].en) {
            return &lang.entries[i];
        }
    }
    return nullptr;
}

// The text of `entry` with every "{}" filled by `middle` (left to right: the
// same middle when there is more than one).
std::string Fill(const char* text, std::string_view middle) {
    std::string out;
    const std::string_view t(text);
    for (std::size_t i = 0; i < t.size();) {
        if (t[i] == '{' && i + 1 < t.size() && t[i + 1] == '}') {
            out += middle;
            i += 2;
        } else {
            out += t[i];
            ++i;
        }
    }
    return out;
}

// The spaces at the text's ends: how much of Around()'s middle is data.
std::size_t LeftTrim(std::string_view s) {
    std::size_t n = 0;
    while (n < s.size() && s[n] == ' ') {
        ++n;
    }
    return n;
}
std::size_t RightTrim(std::string_view s) {
    std::size_t n = 0;
    while (n < s.size() && s[s.size() - 1 - n] == ' ') {
        ++n;
    }
    return n;
}

}  // namespace

const Lang kLangs[] = {
    {"en", "English", nullptr, 0},
    {"zh-Hans", "简体中文", kzh_Hans, int(sizeof kzh_Hans / sizeof kzh_Hans[0])},
    {"zh-Hant", "繁體中文", kzh_Hant, int(sizeof kzh_Hant / sizeof kzh_Hant[0])},
};
const int kLangCount = int(sizeof kLangs / sizeof kLangs[0]);

const Lang* Current() {
    static const Lang* const lang = [] {
        const std::string code = ReadLanguage();
        for (int i = 0; i < kLangCount; ++i) {
            const std::string entry = kLangs[i].code;
            // "zh-Hans" matches "zh-Hans" and "zh-Hans-CN"; "zh" matches "zh-Hant".
            if (code == entry || code.rfind(entry + "-", 0) == 0) {
                return &kLangs[i];
            }
        }
        return static_cast<const Lang*>(nullptr);
    }();
    return lang;
}

namespace {

// A line the tab builds around a number, a name or a code: the fixed ends,
// what the table's line for it looks like, and whether the middle is itself a
// word of the table ("invite code", "hosting").
struct Shape {
    const char* before;   // the fixed start, as the tab writes it
    const char* after;    // the fixed end
    const char* key;      // the table's line, with {} where the middle goes
    bool words;           // the middle is a word the table has too
};

// Longest ends first: "Loading X's save..." must not be taken for "Getting".
constexpr Shape kShapes[] = {
    {"Friends on your ", " paste this line in Join (address and password):",
     "Friends on your {} paste this line in Join (address and password):", true},
    {"Press ", " in the game to open the chat box; Enter sends, Esc closes. A line that starts with / is a command, "
               "not chat. Another key: LICHII_COOP_CHAT_KEY in coop-options.ini.",
     "Press {} in the game to open the chat box; Enter sends, Esc closes. A line that starts with / is a command, "
     "not chat. Another key: LICHII_COOP_CHAT_KEY in coop-options.ini.", false},
    {"Co-op is on: ", " players", "Co-op is on: {} players", false},
    {"Hosting: ", " players", "Hosting: {} players", false},
    {"Join ", " again", "Join {} again", false},
    {"No ", " yet.", "No {} yet.", true},
    {"Copied: ", "", "Copied: {}", false},
    {"", " friend(s) joined.", "{} friend(s) joined.", false},
    {"Getting ", "'s save...", "Getting {}'s save...", false},
    {"Loading ", "'s save...", "Loading {}'s save...", false},
    {"Multiplayer: ", "", "Multiplayer: {}", true},
    // The host's save at the title (coop_title_menu.h AutoLoad::Line).
    {"", "'s save did not arrive: Continue plays your own save, New Game starts a new one.",
     "{}'s save did not arrive: Continue plays your own save, New Game starts a new one.", false},
    {"", " has no save yet: New Game starts one, Continue plays your own.",
     "{} has no save yet: New Game starts one, Continue plays your own.", false},
    {"Press Continue to play ", "'s save.", "Press Continue to play {}'s save.", false},
};

// The middle of `text` when it fits `shape`; false when it does not.
bool Split(const std::string_view text, const Shape& shape, std::string_view* middle) {
    const std::string_view before = shape.before, after = shape.after;
    if (text.size() < before.size() + after.size()) {
        return false;
    }
    if (text.substr(0, before.size()) != before) {
        return false;
    }
    if (after.empty() ? false : text.substr(text.size() - after.size()) != after) {
        return false;
    }
    *middle = text.substr(before.size(), text.size() - before.size() - after.size());
    return !middle->empty();
}

}  // namespace

std::string Tr(std::string_view en) {
    const Lang* lang = Current();
    if (lang == nullptr || lang->entries == nullptr) {
        return std::string(en);
    }
    if (const Entry* entry = Find(*lang, en)) {
        return entry->text;
    }
    // A line built around a number, a name or a code: the table's shape for it
    // fills in what the tab put there. Its middle, when it is a word of the
    // table too ("invite code", "hosting"), is translated as well.
    for (const Shape& shape : kShapes) {
        std::string_view middle;
        if (!Split(en, shape, &middle)) {
            continue;
        }
        std::string_view text = middle;
        if (shape.words) {
            if (const Entry* word = Find(*lang, middle)) {
                text = word->text;
            }
        }
        if (const Entry* entry = Find(*lang, shape.key)) {
            return Fill(entry->text, text);
        }
        // (the table knows the ends but not the whole line: the ends in that
        // language, the middle as it is)
        return std::string(Tr(shape.before)) + std::string(text) + std::string(Tr(shape.after));
    }
    return std::string(en);
}

}  // namespace CoopI18n