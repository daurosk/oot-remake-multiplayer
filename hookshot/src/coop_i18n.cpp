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
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

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
    {"Clear the code", "清空邀请码"},
    {"Clear", "清空"},
    {"Empties the code field. Then paste or type your friend's code again.",
     "清空邀请码栏。然后再粘贴或输入朋友的邀请码。"},
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
    {"Your name, Link's look, how many players can join your game, voice chat, the chat key, which controller this game reads, and players on the map.",
     "名字、林克外观、房间人数上限、语音聊天、聊天键、本游戏读取哪个手柄，以及地图上的玩家。"},
    {"The chat's commands, and the key that opens the chat.", "聊天命令，以及打开聊天的按键。"},
    {"Puts your code on the clipboard again. Share it in DMs only: it contains your internet address.",
     "把邀请码再放进剪贴板。只在私信里发：它含有你的网络地址。"},
    {"Only when a friend's game shows an answer code (their way in was blocked): copy the code they "
     "send you, then press this.",
     "仅当朋友的游戏显示「应答码」（他被路由器挡住）时：复制他发来的码，再按这里。"},
    {"For friends in the same house, on your home network: copies one line with this PC's address. They paste it in Join a game instead of the code.",
     "给同屋、同一家庭网络的朋友：复制含本机地址的一行，他在「加入房间」粘贴这一行而不是邀请码。"},
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
    {"Copy the code from your friend's message, then press this: it is pasted. On the same home network or a VPN with your friend? Paste the line from Same network or from VPN on their Host page instead (address and password). You can also type it.",
     "复制朋友发来的邀请码后按这里即可粘贴。同屋或同 VPN？改为粘贴他「创建房间」页上「同一网络」或「VPN」那一行（地址+密码）。也可以手动输入。"},
    {"Paste your friend's code or enter an IP address first.", "请先粘贴朋友的邀请码，或输入 IP 地址。"},
    {"Joins your friend's game.", "加入朋友的房间。"},
    {"Split screen (experimental): two full games on this PC.", "分屏（实验性）：在这台电脑上运行两个完整游戏。"},
    {"1. Two kits (or Nucleus Co-op), each started once.", "1. 两个工具包（或 Nucleus Co-op），各启动一次。"},
    {"2. Each game its own controller: Settings > Controller.", "2. 每个游戏用自己的手柄：设置 > 手柄。"},
    {"3. Host in one game, Join in the other (Same network row).", "3. 一个游戏里创建房间，另一个里加入（同一网络那一行）。"},
    {"Steps: PLAY-LAN.md. Native split screen comes later.", "步骤：PLAY-LAN.md。原生分屏以后提供。"},
    {"The chat key is off. To chat, choose a key in Settings, Chat key.", "聊天键已关闭。要聊天，请在「设置 → 聊天键」里选一个键。"},

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
    {"Your clipboard is empty: copy your friend's answer code first, then press this again.",
     "剪贴板是空的：请先复制朋友发来的应答码，再按一次这里。"},
    {"That answer code could not be used: host again and ask your friend to join again.", "这个应答码用不了：请重新开房，并让朋友重新加入。"},
    {"The clipboard could not be used: write the code down instead.", "剪贴板用不了：请把码记下来。"},
    {"answer code taken: connecting to your friend", "已接受应答码：正在连接朋友"},
    {"Connected!", "已连接！"},
    {"A settings file could not be written.", "有一个设置文件写不进去。"},
    {"No co-op kit found with this game: only your name was saved (coop.ini).",
     "这个游戏旁边没找到联机套件：只保存了你的名字（coop.ini）。"},
    {"Saved. Co-op is on: some settings count the next time you host or join.",
     "已保存。本局联机在跑：部分设置下次开房/加入时才生效。"},
    {"Link wears it when this menu closes.", "关闭此菜单后林克就会换上。"},

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
    {"{}'s save is not here yet (it still loads by itself if it comes): Continue plays your own save, New Game starts a new one.",
     "{} 的存档还没到（到了仍会自动载入）：继续游戏用自己的存档，新游戏开一个新的。"},
    {"{} has no save yet: New Game starts one, Continue plays your own.",
     "{} 还没有存档：新游戏开一个，继续游戏用自己的。"},
    {"Press Continue to play {}'s save.", "按「继续游戏」玩 {} 的存档。"},
    {"Press {} in the game to open the chat box; Enter sends, Esc closes. A line that starts with / is a command, not chat. To use another key: Settings, Chat key.",
     "游戏中按 {} 打开聊天框；回车发送，Esc 关闭；以 / 开头的行是命令。改键：「设置 → 聊天键」。"},
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
    {"Invite code copied to your clipboard. Share it in DMs only: it contains your internet address.",
     "邀请码已复制到剪贴板。只在私信里发：它含有你的网络地址。"},
    {"It is copied: paste it into your chat with them (Ctrl+V).",
     "已复制：粘贴到和朋友的聊天里（Ctrl+V）。"},
    {"Press \"Copy the code again\" to put it on the clipboard.", "按「再复制一次邀请码」把它放进剪贴板。"},
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
    {"Same home network or VPN as them? Ask for the line from \"Same network\" or \"VPN\" on their Host page.",
     "和他同屋或同一 VPN？让他给你「同一网络」或「VPN」那一行。"},

    // ---- Same network or VPN (LanLines) ----------------------------------
    {"No home-network or VPN address was found on this PC.",
     "这台电脑上没找到内网或 VPN 地址。"},
    {"On a VPN? Start your VPN program (Radmin VPN, Hamachi), then press this again.",
     "在用 VPN？先启动 VPN 程序（Radmin VPN、Hamachi），再按一次这里。"},
    {"Type it into your chat with them, exactly as it is.",
     "原样敲进和朋友的聊天里。"},
    {"If that does not work, try another of this PC's addresses in its place:", "如果不行，换用本机的另一个地址："},
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
    {"That is not an invite code or a server address.", "这不是邀请码，也不是服务器地址。"},

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

    // ---- 0.11: the rows and lines added since (pr224 merge) -------------
    {"Same network", "同一网络"},
    {"Paste friend's answer (or Ctrl+V)", "粘贴朋友的应答码（或 Ctrl+V）"},
    {"For friends on a VPN with you (Radmin VPN, Hamachi, ZeroTier, Tailscale, WireGuard): copies one line with this PC's VPN address. They paste it in Join a game instead of the code.",
     "给同一 VPN（Radmin VPN、Hamachi、ZeroTier、Tailscale、WireGuard）的朋友：复制含本机 VPN 地址的一行，他在「加入房间」粘贴这一行而不是邀请码。"},
    {"Your invite code is below. Share it in DMs only: it contains your internet address.",
     "你的邀请码在下面。只在私信里发：它含有你的网络地址。"},
    {"Customize Link", "自定义林克"},
    {"A colour for each part of your tunics, shields, swords, boots and gauntlets, with a picture of the result. Your Link wears it when you close the menu; the other players see your tunic's colour.",
     "为束衣、盾、剑、靴子和护手的每个部位选颜色，附效果图。关闭菜单时林克换上；其他玩家能看到你束衣的颜色。"},
    {"Chat key", "聊天键"},
    {"The key that opens the chat box in the game. Off: no chat box.", "游戏中打开聊天框的键。关闭：没有聊天框。"},
    {"Controller", "手柄"},
    {"Which controller this game reads: all of them (only while its window is in front), one of them (also behind another window: two games on one PC, each with its own), or none.",
     "本游戏读取哪个手柄：全部（仅当窗口在前台时）、其中一个（窗口在后台也读：一台电脑两个游戏各用各的），或不读。"},
    {"Keyboard and mouse", "键盘和鼠标"},
    {"Window in front: the keyboard and mouse count while this game's window is in front. Also in background: the keys count behind other windows too. Off: neither (a controller only).",
     "窗口在前台：本游戏窗口在前台时键盘和鼠标有效。后台也有效：在其他窗口后面按键也有效。关闭：都不用（只用手柄）。"},
    {"Show players on map", "在地图上显示玩家"},
    {"The other players as arrows in their colours on the minimap and on the Map page's area and dungeon maps (not the world map). Off: neither shows them.",
     "其他玩家以各自颜色的箭头显示在小地图和地图页的区域图、迷宫图上（世界地图不显示）。关闭：都不显示。"},
    {"Names on map", "地图上的名字"},
    {"Each player's name beside their arrow on the maps. Off: the arrows only.", "地图上每个箭头旁显示玩家名字。关闭：只显示箭头。"},
    {"Split screen (experimental) runs two full games on this PC, one per player: about 10 GB of RAM each and a strong graphics card. Start two kits (or use Nucleus Co-op), give each game its own controller in Settings, Controller (controller 1, controller 2 or the keyboard), then Host a game in one and Join a game in the other with the Same network line. The steps are in PLAY-LAN.md. Native split screen, one game for both, comes in a later release.",
     "分屏（实验性）会在这台电脑上为每位玩家运行一个完整游戏：每个约需 10 GB 内存，还要一张强劲的显卡。启动两个工具包（或使用 Nucleus Co-op），在「设置」的「手柄」里给每个游戏各选一个手柄（手柄 1、手柄 2 或键盘），然后在一个游戏里「创建房间」，在另一个里用「同一网络」那一行「加入房间」。步骤见 PLAY-LAN.md。原生分屏（两人一个游戏）将在以后的版本中提供。"},
    {"No home-network address was found on this PC.", "这台电脑上没找到家庭网络地址。"},
    {"Friends somewhere else join with your invite code; on a VPN with you, they use the VPN row.",
     "不在同屋的朋友用邀请码加入；和你同一 VPN 的朋友用「VPN」那一行。"},
    {"No VPN found.", "没找到 VPN。"},
    {"Using one (Radmin VPN, Hamachi, ZeroTier, Tailscale, WireGuard)? Start it, then press this again.",
     "在用 VPN（Radmin VPN、Hamachi、ZeroTier、Tailscale、WireGuard）？先启动它，再按一次这里。"},
    {"Host a game first: a friend's answer code is pasted while you host.", "请先开房：朋友的应答码要在开房时粘贴。"},
    {"There is no invite code this time, so no answer code: friends on the same network or a VPN use the Same network or the VPN row.",
     "这次没有邀请码，所以也没有应答码：同屋或同 VPN 的朋友请用「同一网络」或「VPN」那一行。"},
    {"No co-op kit found with this game: the choice counts until the game closes.", "这个游戏旁边没找到联机套件：这个选择到游戏关闭前有效。"},
    {"A settings file could not be written: the choice counts until the game closes.", "有一个设置文件写不进去：这个选择到游戏关闭前有效。"},
    {"The clipboard could not be read (another program had it open): press this again.", "剪贴板读不了（被别的程序占用）：再按一次这里。"},
    {"That is your own invite code: copy the answer code your friend sends back to you.", "这是你自己的邀请码：请复制朋友回传给你的应答码。"},
    {"Getting {}'s save: it loads by itself when it is here...", "正在获取 {} 的存档：到了会自动载入…"},
    {"{}'s game ended before the save came: Continue plays your own save, New Game starts a new one.",
     "{} 的游戏在存档送到前结束了：继续游戏用自己的存档，新游戏开一个新的。"},
    {"Host save loading, don't load a save.", "房主存档载入中，先别载入存档。"},
    {"Success, load any save.", "成功，载入任意存档。"},
    {"Success, load any save for changes to occur.", "成功，载入任意存档以生效。"},
    {"You joined with a file loaded: the host's save is coming. To play from the host's progress, go back to the "
     "title and load a save once the chat says Success.",
     "你在已载入存档时加入：房主的存档正在传来。要从房主的进度游玩，请回到标题画面，等聊天栏显示“成功”后再载入存档。"},
    {"That save loaded before the host's save was in: go back to the title and load a save after Success.",
     "这个存档在房主存档送到前就载入了：回到标题画面，等「成功」后再载入存档。"},
    {"The host's save did not come: play on, or join again to get it.", "房主的存档没有送到：继续玩，或重新加入来获取。"},
    {"The host has no save yet: New Game starts one, Continue plays your own.", "房主还没有存档：新游戏开一个，继续游戏用自己的。"},
    {"Getting {}'s save: don't load a save yet...", "正在获取 {} 的存档：先别载入存档…"},
    {"{}'s save is in: load any save.", "{} 的存档已到：载入任意存档。"},
    {"{}'s save is not here yet (the chat says Success when it is): Continue plays your own save, New Game starts a new one.",
     "{} 的存档还没到（到了聊天会显示「成功」）：继续游戏用自己的存档，新游戏开一个新的。"},

    // ---- The Lobby and Mods tabs (0.11) --------------------------------
    {"Lobby", "大厅"},
    {"Mods", "模组"},
    {"You are not in a game.", "你不在任何游戏里。"},
    {"Host one or join a friend's on the Multiplayer tab; the players show here.", "在「联机」页创建房间或加入朋友的房间，玩家会显示在这里。"},
    {"The players show here once this game is in the session.", "这局进了游戏后，玩家会显示在这里。"},
    {"Load your file to play: the others see you once you do.", "载入存档才能玩：载入之后其他玩家才看得到你。"},
    {"Puts the invite code on the clipboard. Share it in DMs only (it contains your internet address); your friend pastes it in Join a game.",
     "把邀请码复制到剪贴板。只私聊发给朋友（里面有你的公网地址）；他把它粘贴到「加入房间」里。"},
    {"Co-op is starting...", "联机正在启动…"},
    {"Open place", "空位"},
    {"Invite", "邀请"},
    {"Copy the invite code", "复制邀请码"},
    
    {"The invite code is not ready yet (see the Multiplayer tab).", "邀请码还没准备好（见「联机」页）。"},
    {"Only the host has the invite code: ask them for it.", "只有房主有邀请码：向他要。"},
    {"Host's game", "房主的游戏"},
    {"Not known yet", "还不知道"},
    {"Host", "房主"},
    {"Mute their voice (only you)", "屏蔽他的语音（只对你）"},
    {"You stop hearing them. Only in your game: nobody is told.", "你不再听到他说话。只在你这边生效，不会通知任何人。"},
    {"Mute their chat (only you)", "屏蔽他的聊天（只对你）"},
    {"Their chat lines stop showing in your game. Nobody is told.", "他的聊天不再显示在你的游戏里。不会通知任何人。"},
    {"Teleport to them", "传送到他身边"},
    {"Your Link goes to where they stand, as /teleport does (they must not be busy or in a boss fight).",
     "你的林克传送到他站的位置，效果同 /teleport（他不能正在忙碌或打 BOSS）。"},
    {"This is you. Pick another player to mute them or go to them.", "这是你自己。选择其他玩家可以屏蔽他或传送到他那里。"},
    {"Mute their voice for everyone", "对所有人屏蔽他的语音"},
    {"The server stops passing their voice on to anyone.", "服务器不再把他的语音转发给任何人。"},
    {"Mute their chat for everyone", "对所有人屏蔽他的聊天"},
    {"The server drops their chat lines.", "服务器会丢弃他的聊天消息。"},
    {"Kick", "踢出"},
    {"Takes them out of the game. They can join again.", "把他踢出游戏，他还能再加入。"},
    {"Ban for this session", "本局封禁"},
    {"Takes them out; they cannot join again, under any name, until you stop hosting.", "把他踢出；在你停止开房之前，他换任何名字都进不来。"},
    {"Press again to kick", "再按一次踢出"},
    {"Press again to ban", "再按一次封禁"},
    {"The invite code is on the clipboard.", "邀请码已放进剪贴板。"},
    {"Resync from host", "从房主重新同步"},
    {"Gets the host's save again. Back on the title, it is switched to once the chat says Success; while you play nothing changes until then. Your own saves are never touched.", "重新获取房主的存档。回到标题画面，聊天显示 Success 后切换过去；游戏中在那之前什么都不变。你自己的存档不会被改动。"},
    {"Asked the host for its save: the chat says how it goes.", "已向房主要存档：进展见聊天。"},
    {"The players of your game show here once you host or join.", "开房或加入后，你游戏里的玩家会显示在这里。"},
    {"downed", "倒地"},
    {"out", "出局"},
    {"fainted", "力尽"},
    {"talking", "说话中"},
    {"muted", "已静音"},
    {"voice", "语音"},
    {"chat", "聊天"},
    {"voice and chat", "语音和聊天"},
    {"Off / On", "关 / 开"},
    {"red", "红色"},
    {"orange", "橙色"},
    {"yellow", "黄色"},
    {"green", "绿色"},
    {"cyan", "青色"},
    {"blue", "蓝色"},
    {"purple", "紫色"},
    {"pink", "粉色"},
    {"white", "白色"},
    {"black", "黑色"},
    {"brown", "棕色"},
    {"gray", "灰色"},
    {"grey", "灰色"},
    {"This mod is gone from the folder.", "这个模组已从文件夹里消失。"},
    {"Randomizer", "随机模式"},
    {"Off: the game leaves this mod out from its next start.", "关闭：游戏下次启动时不会加载这个模组。"},
    {"Move up", "上移"},
    {"Up: it wins a file it shares with the mods under it.", "上移：与下面模组共用的文件，以它为准。"},
    {"Move down", "下移"},
    {"Down: the mods above it win a file they share.", "下移：共用的文件以它上面的模组为准。"},
    {"(empty)", "（空）"},
    {"In this co-op game the host sets this.", "联机时由房主设置。"},
    {"Counts after a restart.", "重启后生效。"},
    {"Type or Ctrl+V, then Enter. Esc: as it was.", "输入或按 Ctrl+V，然后按 Enter。Esc：保持原样。"},
    {"Restart the game to use the changes.", "重启游戏以应用改动。"},
    {"Switch", "切换"},
    {"Co-op: waiting for the host's list of mods.", "联机：正在等待房主的模组列表。"},
    {"Co-op: your mods match the host's.", "联机：你的模组与房主一致。"},
    {"Host: {}", "房主：{}"},
    {"Host: {} (you)", "房主：{}（你）"},
    {"{} (you)", "{}（你）"},
    {"Players  {}", "玩家  {}"},
    {"Player {}", "玩家 {}"},
    {", the host", "，房主"},
    {"{} hearts", "{} 颗心"},
    {"{} ms", "{} 毫秒"},
    {"{} / {} hearts", "{} / {} 颗心"},
    {"{} more player can join", "还能进 {} 人"},
    {"{} more players can join", "还能进 {} 人"},
    {"The host muted their {} for everyone.", "房主对所有人屏蔽了他的{}。"},
    {"Teleporting to {}: the chat says how it goes.", "正在传送到 {}：过程会显示在聊天里。"},
    {"Kicking {}.", "正在踢出 {}。"},
    {"Banning {}.", "正在封禁 {}。"},
    {"Restart needed: {}", "需要重启：{}"},
    {"No mods in {}. Put each mod in its own folder there.", "{} 里没有模组。每个模组放一个自己的文件夹。"},
    {"Randomizer: {}", "随机模式：{}"},
    {"Co-op: {}", "联机：{}"},
    {"Player {} (you)", "玩家 {}（你）"},
    {"Player {}. Press to see what you can do.", "玩家 {}。按这里查看能做些什么。"},
    {"Player {}. Press to see yourself.", "玩家 {}。按这里查看自己的情况。"},
    {"Voice mode", "语音模式"},
    {"Open mic: you are heard whenever you speak (silence and quiet noise are not sent). Push to talk: only while you hold {}. On speakers, not headphones? Push to talk keeps the others' voices from coming back to them through your microphone.",
     "自由麦：你说话时大家都能听到（静音和轻声不会发送）。按键说话：只有按住 {} 时才发送。用音箱而不是耳机？用按键说话可以避免别人的声音从你的麦克风传回去。"},
    {"Starts a game on this PC and makes a code. Send the code to your friends; they choose Join a game and paste it. They play on your save.",
     "在本机开房并生成邀请码。把码发给朋友，他们选「加入房间」粘贴。他们玩的是你的存档。"},
    {"Joins a friend's game with the code they sent you. You play on their save; your own saves are not touched.",
     "用朋友发来的邀请码加入他的房间。你玩他的存档，自己的存档不受影响。"},
    {"Only when a friend's game shows an answer code (their way in was blocked): copy the code they send you, then press this.",
     "仅当朋友的游戏显示「应答码」（他被路由器挡住）时：复制他发来的码，再按这里。"},
    {"Your friend's router blocks the way in: send them this answer code (it is copied already); they paste it on their Host page.",
     "朋友的游戏被路由器挡住了：把这个应答码发给他（已复制），他粘贴到自己的「创建房间」页。"},
    {"Joins the last game you were in, with the same code. It works while your friend is still hosting that game.",
     "用同一邀请码回到上次的房间（朋友仍在开房时有效）。"},
    {"The colour of your Link's tunic and cap, as you and the other players see it. By player number: each player's own colour.",
     "你的林克束衣和帽子的颜色，你自己和其他玩家看到的样子。按玩家编号：每人用自己的颜色。"},
    {"Talk with players near your Link through your microphone; they sound quieter the farther away they are. Off: you neither send nor hear voices.",
     "用麦克风与林克附近的玩家说话；离得越远声音越小。关闭：不发送也不接收语音。"},
    {"Push to talk", "按键说话"},
    {"Open mic", "自由麦"},
    {"The host only: takes that player out of the game. They can join again.", "仅房主：把该玩家踢出游戏。他还能再加入。"},
    {"The host only: takes that player out, and they cannot join again, under any name, until the host stops hosting.",
     "仅房主：把他踢出；在房主停止开房之前，他换任何名字都进不来。"},
    {"The host: nobody hears that player's voice or sees their chat (the server stops passing them on); add voice or chat for only that (/mute 2 voice). Anyone else: you stop hearing and seeing them yourself, and nobody is told.",
     "房主：没人再听到他的语音或看到他的聊天（服务器停止转发）；只针对语音或聊天就加 voice 或 chat（/mute 2 voice）。其他人：你自己不再听到他、看到他的聊天，不会通知任何人。"},
    {"Undoes /mute (add voice or chat for only that).", "撤销 /mute（只对语音或聊天就加 voice 或 chat）。"},
    {"Takes you to that player: the whole name, the start of it, or the player's number (/teleport 2). In another place, talk to someone or read a sign within a minute to travel. Not during a cutscene, a talk or a boss fight.",
     "把你传送到该玩家身边：全名、名字开头，或玩家编号（/teleport 2）。在别的地点时，一分钟内与人对话或读告示即可传送。过场动画、对话或 BOSS 战中不行。"},
    
    
    {"VPN", "VPN"},
    {"Your friend's router blocks the way in: send them this answer code (it is copied already); they paste it on their Host page.",
     "朋友的游戏被路由器挡住了：把这个应答码发给他（已复制），他粘贴到自己的「创建房间」页。"},
    
    {"You", "你"},
    {"You are down", "你倒下了"},
    {"is reviving you", "正在救你"},
    {"Reviving", "正在救"},
    {"Hold A to revive", "按住 A 施救"},
    {"Watching", "正在观看"},
    {"A: next player", "A：下一位玩家"},
    {"(out of sight)", "（不在视野内）"},
    {"all", "全部"},
    {"none (keyboard + mouse)", "不读（键盘和鼠标）"},
    {" (not connected)", "（未连接）"},
    {"window in front", "窗口在前台"},
    {"also in background", "后台也有效"},
    {"off", "关闭"},
    {"Tunic", "束衣"},
    {"Kokiri Tunic", "科奇里束衣"},
    {"Goron Tunic", "哥隆束衣"},
    {"Zora Tunic", "佐拉束衣"},
    {"Hylian Shield", "海利亚盾"},
    {"Mirror Shield", "镜盾"},
    {"Deku Shield", "德库盾"},
    {"Kokiri Sword", "科奇里剑"},
    {"Master Sword", "大师之剑"},
    {"Biggoron's Sword", "大古隆之剑"},
    {"Boots", "靴子"},
    {"Gauntlets", "护手"},
    {"Frame", "外框"},
    {"Face", "面"},
    {"Emblem", "纹章"},
    {"Rim", "边"},
    {"Mirror", "镜面"},
    {"Wood", "木"},
    {"Blade", "剑身"},
    {"Hilt", "剑柄"},
    {"Leather", "皮革"},
    {"Metal", "金属"},
    {"The tunic and the cap (one piece on adult Link).", "束衣和帽子（成年林克是连体的）。"},
    {"The red cloth; the gold trim and the plates stay.", "红色布料；金色滚边和甲片不变。"},
    {"The blue cloth and the scale pattern.", "蓝色布料和鳞片花纹。"},
    {"The metal rim, and the cross on the back.", "金属外框，以及背面的十字。"},
    {"The blue face.", "蓝色的盾面。"},
    {"The red bird; the gold Triforce stays.", "红色鸟纹；金色三角不变。"},
    {"The red rim.", "红色边。"},
    {"The mirror.", "镜面。"},
    {"The whole shield: it is one piece of wood.", "整个盾：它是一整块木头。"},
    {"The blade only: the grip is the Hilt below.", "仅剑身：握柄是下面的剑柄。"},
    {"The blade only: the grip and wings are the Hilt below; the gold stays.", "仅剑身：握柄和翼是下面的剑柄；金色部分不变。"},
    {"The leather grip.", "皮革握柄。"},
    {"The blue grip and wings; the gold and the gem stay.", "蓝色握柄和翼；金色部分与宝石不变。"},
    {"The blue grip.", "蓝色握柄。"},
    {"The leather of every pair: the Kokiri, Hover and Iron Boots (young Link's belts too).",
     "每一双的皮革：科奇里靴、浮空靴和铁靴（幼年林克的腰带也是）。"},
    {"The metal of the Silver and the Golden Gauntlets.", "银色护手和金色护手的金属部分。"},
    {"Left and Right try the colours; Confirm mixes your own. Your Link wears it when you close the menu (the other players see only the tunic's colour for now).",
     "左右方向键试颜色；确认则自己调色。关闭菜单时林克就换上（其他玩家目前只能看到你束衣的颜色）。"},
    {"All parts back to the game's own", "全部恢复游戏自带"},
    {"Every part in the game's own colours again (the tunic: your player number's colour for the others).",
     "所有部位都恢复游戏自带的颜色（束衣：在其他玩家眼里是你编号对应的颜色）。"},
    {"Game's own colour", "游戏自带的颜色"},
    {"This part back to the game's own colour.", "把这个部位恢复成游戏自带的颜色。"},
    {"Back to the list of parts.", "回到部位列表。"},
    {"Game's own", "游戏自带"},
    {"Preset", "预设"},
    {"Left and Right: the preset colours.", "左右：预设颜色。"},
    {"Mixed", "自定义"},
    {"Colour", "颜色"},
    {"Mix", "调色"},
    {"Red", "红"},
    {"Green", "绿"},
    {"Blue", "蓝"},
    
    {"OoT Remake Randomizer", "OoT Remake 随机模式"},
    {"Seed", "种子"},
    {"Game folder", "游戏文件夹"},
    {"OOTRR program", "OOTRR 程序"},
    {"On: the game plays a randomized seed (start a new save for it).", "开启：游戏按随机种子运行（需要开一个新存档）。"},
    {"Letters and digits. Empty: a new one is made. In co-op the host's seed is everyone's.",
     "字母和数字。留空：自动生成。联机时所有人用房主的种子。"},
    
    {"Only for other game files (1.1.1, with Pack, Banc, Event and System). Empty: the kit's own (romfs beside the game).",
     "只在用别的游戏文件时填（1.1.1，含 Pack、Banc、Event 和 System）。留空：用工具包自带的（游戏旁边的 romfs）。"},
    {"Only for another OOTRR. Empty: the one the kit brings (tools\\ootrr beside the game).",
     "只在用别的 OOTRR 时填。留空：用工具包自带的（游戏旁边的 tools\\ootrr）。"},
    {"Shuffles chests, dungeon items and rewards (OOTRR). In co-op everyone plays the host's seed and every pickup is shared. A new seed needs a restart of the game, never a game build.",
     "打乱宝箱、迷宫道具和奖励（OOTRR）。联机时所有人玩房主的种子，拾取共享。换种子只需重启游戏，不用重新构建。"},
    
    {"By {}", "作者：{}"},
    {", version {}", "，版本 {}"},
    {"The order of the mods changed: it counts from the next start.", "模组顺序已改变：下次启动生效。"},
    {"changes at the next start.", "将在下次启动生效。"},
    
    {"below", "下层"},
    {"above", "上层"},
    {"By", "作者"},
    {", version", "，版本"},
    {"Push to talk: only while you hold", "按键说话：只有按住"},
    {"On speakers, not headphones? Push to talk keeps the others' voices from coming back to them through your microphone.", "用音箱而不是耳机？用按键说话可以避免别人的声音从你的麦克风传回去。"},
    {". Press to see ", "。按这里查看"},
    {"yourself.", "自己的情况。"},
    {"what you can do.", "能做些什么。"},
    {"Open mic: you are heard whenever you speak (silence and quiet noise are not sent).", "自由麦：你说话时大家都能听到（静音和轻声不会发送）。"},
    {", ", "，"},
    {"Host: ", "房主："},
    {": ", "："},
    {"Default", "默认"},
    {"Easy", "简单"},
    {"Open world", "开放世界"},
    {"Keysanity", "钥匙乱序"},
    {"Full shuffle", "完全乱序"},

    {"IP address", "IP 地址"},
    {"Port", "端口"},
    {"Port when you host", "建房端口"},
    {"The UDP port other players reach you on (default 27510). Type a number from 1 to 65535; changing it while hosting restarts hosting: friends join again with the new code.",
     "其他玩家连接你使用的 UDP 端口（默认 27510）。输入 1 到 65535 之间的数字；建房期间修改会重新建房：朋友要用新的邀请码重新加入。"},
    {"Type the host's IP address or host name directly (no invite code needed).",
     "直接输入房主的 IP 地址或主机名（不需要邀请码）。"},
    {"The host's UDP port (default 27510). Type a number from 1 to 65535.",
     "房主的 UDP 端口（默认 27510）。输入 1 到 65535 之间的数字。"},
    {"The UDP port your game listens on when you host (default 27510). Type a number from 1 to 65535.",
     "你建房时游戏监听的 UDP 端口（默认 27510）。输入 1 到 65535 之间的数字。"},
    {"IP address: enter a valid IP address or host name (with optional :port). It was not changed.",
     "IP 地址：请输入有效的 IP 地址或主机名（可带 :端口），未修改。"},
    {"Port: enter a number from 1 to 65535. It was not changed.",
     "端口：请输入 1 到 65535 之间的数字，未修改。"},
    {"Password", "密码"},
    {"The host's join password, for an IP address: the word after the address on their Same network or VPN line. An invite code has it inside.",
     "用 IP 地址加入时房主的加入密码：房主「同一网络」或「VPN」那一行里地址后面的词。邀请码里已经包含密码。"},
    {"Password: no spaces, at most 128 characters. It was not changed.",
     "密码：不能有空格，最多 128 个字符，未修改。"},
    // The boss room's chat notices (lichii/hook/coop_boss.cpp, #50): "<name> took the blue warp".
    {"{} took the blue warp", "{} 进入了蓝色传送光"},
    {"{} left the boss room", "{} 离开了 Boss 房间"},
    // ---- Ours, on top of the co-op release before this one (the tabs, the chat's notices, the
    //      pickup notices, the item and place names) ----
    {"The blade.", "剑身。"},
    {"connection lost, reconnecting", "连接断了，正在重连"},
    {"the host muted your chat: nobody sees it", "房主屏蔽了你的聊天：现在没人看得到"},
    {"you host now: the Lobby's host controls are yours", "你现在是房主：「大厅」页的房主操作归你了"},
    {"Difficulty: could not be sent", "难度：没能发出去"},
    {"teleport: wait until your game is loaded", "传送：等你的游戏载入好"},
    {"teleport: not during a cutscene or a talk", "传送：过场或对话时不行"},
    {"teleport: not during your boss fight", "传送：打 Boss 的时候不行"},
    {"teleport: stopped: your game got busy (a load, a cutscene or a fight)", "传送：已停下：你的游戏忙起来了（载入、过场或战斗）"},
    {"teleport: talk to someone or read a sign within a minute to go there", "传送：一分钟内和某人说话或读一个告示，就能过去"},
    {"teleport: the game did not change scene in time; stopped", "传送：游戏没能及时换场景，已停下"},
    {"teleport: stopped: the new scene did not settle", "传送：已停下：新场景没稳定下来"},
    {"teleport: off in this game (LICHII_COOP_TELEPORT=0)", "传送：本局关掉了（LICHII_COOP_TELEPORT=0）"},
    {"teleport: could not ask (not connected?)", "传送：问不到对方（没连上？）"},
    {"teleport: stopped: the connection was lost", "传送：已停下：连接断了"},
    {"lobby: not connected", "大厅：没连上"},
    {"lobby: could not be sent", "大厅：没能发出去"},
    {"Mods: yours now match the host's", "模组：你的和房主一致了"},
    {"child", "少年"},
    {"adult", "成年"},
    {"Normal", "普通"},
    {"Hard", "困难"},
    {"Custom", "自定义"},
    {"joined as player {}", "已加入：玩家 {}"},
    {"reconnected as player {}", "已重新连上：玩家 {}"},
    {"{} joined", "{} 加入了"},
    {"{} left", "{} 离开了"},
    {"server {}", "服务器：{}"},
    {"and {} more", "还有 {} 个"},
    {"teleport: next to {}", "传送：已经到 {} 身边"},
    {"teleport: asking {} where they are", "传送：在问 {} 在哪里"},
    {"teleport: {} is coming to you", "传送：{} 正在过来"},
    {"teleport: stopped: you left {}", "传送：已停下：你离开了 {}"},
    {"teleport: stopped: {} left", "传送：已停下：{} 离开了"},
    {"Difficulty: Automatic ({} players): life {}%, damage {}%, enemies {}%; bosses: life {}%, damage {}%", "难度 自动（{}人）生命{}% 伤害{}% 敌人{}% · Boss生命{}% 伤害{}%"},
    {"Difficulty: Automatic ({} player): life {}%, damage {}%, enemies {}%; bosses: life {}%, damage {}%", "难度 自动（{}人）生命{}% 伤害{}% 敌人{}% · Boss生命{}% 伤害{}%"},
    {"Difficulty: {}: life {}%, damage {}%, enemies {}%", "难度 {}：生命{}% 伤害{}% 敌人{}%"},
    {"teleport: going to {} in {}", "传送：去 {} 所在的 {}"},
    {"teleport: {} has gone on to {}; you are where they were. Type /teleport again to follow", "传送：{} 已经去了 {}；你到了他刚才的位置。再输一次 /teleport 就能跟过去"},
    {"teleport: {} is in {}, a place for {} Link; yours is {}", "传送：{} 在 {}，那是{}的林克去的地方；你这边是{}"},
    {"teleport: {} is {} Link and you are {}: no teleport to another age's Link", "传送：{} 是{}的林克，你是{}：不能传送到另一个时代的林克那里"},
    {"teleport: {} is busy now; you are where they were", "传送：{} 现在正忙；你到了他刚才的位置"},
    {"teleport: no answer from {}; you are where they were", "传送：{} 没回应；你到了他刚才的位置"},
    {"teleport: no answer from {} (an older build, or their game is busy)", "传送：{} 没回应（他的版本较旧，或者游戏正忙）"},
    {"teleport: {} is in a dungeon: you may need its items to get on from there", "传送：{} 在迷宫里：要从那里继续，可能需要里面的道具"},
    {"teleport: {} is in {}, a place /teleport cannot load (no start point known there)", "传送：{} 在 {}，那个地方 /teleport 载入不了（不知道起点）"},
    {"{}: not connected", "{}：没连上"},
    {"{}: only the host can do that", "{}：只有房主能做"},
    {"{}: could not be sent", "{}：没能发出去"},
    {"refused: {}", "被拒绝：{}"},
    {"Mods: yours differ from the host's ({}): see the Mods tab", "模组：你的和房主不一样（{}）：看「模组」页"},
    {"Mods: player {} ({}) has {} mods unlike yours", "模组：玩家 {}（{}）有 {} 个模组和你不一样"},
    {"Mods: player {} has {} mods unlike yours", "模组：玩家 {} 有 {} 个模组和你不一样"},
    {"Mods: player {} has {} mod unlike yours", "模组：玩家 {} 有 {} 个模组和你不一样"},
    {"Mods: player {} ({}) has {} mod unlike yours", "模组：玩家 {}（{}）有 {} 个模组和你不一样"},
    {"{} got the {} - you got one too!", "{} 拿到了{} —— 你也得到了一个！"},
    {"{} got the {} - you got them too!", "{} 拿到了{} —— 你也得到了！"},
    {"{} got the {}", "{} 拿到了{}"},
    {"{} found a Small Key ({}) - the group has {}", "{} 找到一把小钥匙（{}）—— 队伍现在有 {} 把"},
    {"{} got the map ({})", "{} 拿到了地图（{}）"},
    {"{} got the compass ({})", "{} 拿到了指南针（{}）"},
    {"{} got the Boss Key ({})", "{} 拿到了 Boss 钥匙（{}）"},
    {"{} got a Heart Container - your max health went up", "{} 得到一颗心之容器 —— 最大生命值提升了"},
    {"{} got a Heart Container", "{} 得到一颗心之容器"},
    {"{} got a Gold Skulltula token ({})", "{} 得到一枚金色骷髅蜘蛛徽章（{}）"},
    {"{} learned {}", "{} 学会了{}"},
    {"Synced with the group: {}", "已与队伍同步：{}"},
    {"{} loaded a save - the group follows it ({} changed)", "{} 载入了存档 —— 队伍跟着切换（{} 项变化）"},
    {"...and {} more from the group", "……还有 {} 条队伍的消息"},
    {"items", "道具"},
    {"songs", "歌曲"},
    {"keys", "钥匙"},
    {"maps and compasses", "地图和指南针"},
    {"heart containers", "心之容器"},
    {"tokens", "徽章"},
    {"other things", "其他东西"},
    {"Someone", "某人"},
    {"Pocket Cucco", "口袋咕咕鸡"},
    {"Pocket Egg", "口袋蛋"},
    {"Bomb Bag Small", "小炸弹袋"},
    {"Bomb Bag Medium", "中炸弹袋"},
    {"Bomb Bag Large", "大炸弹袋"},
    {"Bombchus", "炸弹鼠"},
    {"Piece of Heart", "心之碎片"},
    {"Heart Container", "心之容器"},
    {"Small Key", "小钥匙"},
    {"Boss Key", "Boss 钥匙"},
    {"Longshot", "长钩索"},
    {"Boomerang", "回旋镖"},
    {"Lens of Truth", "真实之镜"},
    {"Magic Bean", "魔法豆"},
    {"Magic Bean Pack", "魔法豆袋"},
    {"Mask Keaton", "基顿面具"},
    {"Mask Skull", "骷髅面具"},
    {"Mask Spooky", "恐怖面具"},
    {"Mask Gerudo", "格鲁德面具"},
    {"Mask Truth", "真实面具"},
    {"Mask Zora", "佐拉面具"},
    {"Mask Goron", "哥隆面具"},
    {"Mask Bunny", "兔子头巾"},
    {"Weird Egg", "奇怪的蛋"},
    {"Chicken", "咕咕鸡"},
    {"Zelda Letter", "塞尔达的信"},
    {"Weird Bottle", "奇怪的瓶子"},
    {"Empty Bottle", "空瓶"},
    {"Red Potion", "红药水"},
    {"Green Potion", "绿药水"},
    {"Blue Potion", "蓝药水"},
    {"Fairy", "妖精"},
    {"Fish", "鱼"},
    {"Milk", "牛奶"},
    {"Rutos Letter", "露多公主的信"},
    {"Poacher's Saw", "盗猎者的锯"},
    {"Broken Goron's Sword", "折断的大哥隆之剑"},
    {"Prescription", "处方"},
    {"Eyeball Frog", "眼球蛙"},
    {"Eyedrops", "眼药水"},
    {"Claim Check", "取货单"},
    {"Ocarina", "陶笛"},
    {"Stick", "木棒"},
    {"Nut", "坚果"},
    {"Bean", "豆子"},
    {"Hammer", "锤子"},
    {"Slingshot", "弹弓"},
    {"Bow", "弓"},
    {"Arrow", "箭"},
    {"Scale", "鳞"},
    {"Gauntlet", "护手"},
    {"Shield", "盾"},
    {"Sword", "剑"},
    {"Stone", "宝石"},
    {"Medal", "徽章"},
    {"Fire", "火"},
    {"Ice", "冰"},
    {"Light", "光"},
    {"Forest", "森"},
    {"Water", "水"},
    {"Shadow", "暗"},
    {"Spirit", "魂"},
    {"Fairy Slingshot", "妖精弹弓"},
    {"Fairy Bow", "妖精弓"},
    {"Fire Arrows", "火箭"},
    {"Ice Arrows", "冰箭"},
    {"Light Arrows", "光箭"},
    {"Fairy Ocarina", "妖精陶笛"},
    {"Ocarina of Time", "时之陶笛"},
    {"Bombs", "炸弹"},
    {"Deku Nuts", "德库坚果"},
    {"Deku Seeds", "德库种子"},
    {"Deku Sticks", "德库木棒"},
    {"Megaton Hammer", "巨力锤"},
    {"Nayru's Love", "娜露之爱"},
    {"Din's Fire", "汀之炎"},
    {"Farore's Wind", "法罗之风"},
    {"Silver Scale", "银鳞"},
    {"Golden Scale", "金鳞"},
    {"Kokiri's Emerald", "科吉里的翠玉"},
    {"Goron's Ruby", "哥隆的红玉"},
    {"Zora's Sapphire", "佐拉的蓝玉"},
    {"Forest Medallion", "森之徽章"},
    {"Fire Medallion", "火之徽章"},
    {"Water Medallion", "水之徽章"},
    {"Shadow Medallion", "暗之徽章"},
    {"Spirit Medallion", "魂之徽章"},
    {"Light Medallion", "光之徽章"},
    {"Gerudo Membership Card", "格鲁德会员卡"},
    {"Giant's Knife", "巨人的小刀"},
    {"Hover Boots", "浮空靴"},
    {"Iron Boots", "铁靴"},
    {"Silver Gauntlets", "银护手"},
    {"Golden Gauntlets", "金护手"},
    {"Zelda's Lullaby", "塞尔达的摇篮曲"},
    {"Epona's Song", "艾波娜之歌"},
    {"Saria's Song", "萨莉亚之歌"},
    {"Sun's Song", "太阳之歌"},
    {"Song of Time", "时之歌"},
    {"Scarecrow's Song", "稻草人之歌"},
    {"Minuet of Forest", "森之小步舞曲"},
    {"Bolero of Fire", "火之波丽露"},
    {"Serenade of Water", "水之小夜曲"},
    {"Nocturne of Shadow", "暗之夜曲"},
    {"Requiem of Spirit", "魂之安魂曲"},
    {"Prelude of Light", "光之前奏曲"},
    {"Deku Tree", "德库大树"},
    {"Dodongo's Cavern", "多多哥洞窟"},
    {"Jabu-Jabu's Belly", "加布加布大人腹中"},
    {"Ganon's Castle", "加农城堡"},
    {"got the {} - you got one too!", "拿到了{} —— 你也得到了一个！"},
    {"got the {} - you got them too!", "拿到了{} —— 你也得到了！"},
    {"got the {}", "拿到了{}"},
    {"found a Small Key ({})", "找到一把小钥匙（{}）"},
    {"got the map ({})", "拿到了地图（{}）"},
    {"got the compass ({})", "拿到了指南针（{}）"},
    {"got the Boss Key ({})", "拿到了 Boss 钥匙（{}）"},
    {"got a Heart Container", "得到一颗心之容器"},
    {"got a Gold Skulltula token ({})", "得到一枚金色骷髅蜘蛛徽章（{}）"},
    {"learned {}", "学会了{}"},
    {", the ", "、"},
    {"Joining your friend's game...", "正在加入朋友的房间…"},
    {"Press Leave, then join again.", "按「离开」，然后再加入一次。"},
    {"Press Leave.", "按「离开」。"},
    {"wrong join password", "加入密码不对"},
    {"wrong game build", "游戏版本不一致"},
    {"server full", "房间满了"},
    {"name taken", "名字被占了"},
    {"bad name", "名字不合法"},
    {"wrong protocol version", "协议版本不一致"},
    {"the host took you out of the game", "房主把你踢出了游戏"},
    {"the host banned you from this game", "房主在这个房间封禁了你"},
    // The refusal's full sentence (coop/net/src/client.cpp): the tab takes the part before ':'
    // (RefusedLine), and the chat writes the whole "refused: <reason>" line, so the sentence
    // itself has a line here too.
    {"wrong protocol version: this game and the server are different releases; run Update.bat in every kit, the host's too",
     "协议版本不一致：游戏和服务器不是同一版本；请在每份套件里都运行 Update.bat，房主的也要"},
    {"wrong join password: ask the host for it, then start Join.bat again and type it",
     "加入密码不对：向房主要密码，然后重新启动 Join.bat 输入"},
    {"the host took you out of the game: you can join again", "房主把你踢出了游戏：可以再加入"},
    {"the host banned you from this game: you cannot join it again until the host stops hosting",
     "房主在这个房间封禁了你：房主停止创建房间后，你才能再加入"},
    {"the server did not ask for the join password, so the game did not join it unencrypted (every session has a password and is encrypted): the host's server is an older build, or someone on the way answered for it",
     "服务器没有索取加入密码，所以游戏没有以未加密的方式连入（每个房间都有密码并且加密）：房主的服务器是旧版本，或者途中有程序替它应答了"},
    {"the host restarted its server (it has a new join password now): ask the host for the new invite, then start Join.bat again with it",
     "房主重启了服务器（现在有新的加入密码）：向房主要新的邀请，然后重新用新密码启动 Join.bat"},
    {"unknown reason", "未知原因"},
    {"{} items", "{} 件道具"},
    {"{} songs", "{} 首歌曲"},
    {"{} keys", "{} 把钥匙"},
    {"{} maps and compasses", "{} 份地图和指南针"},
    {"{} heart containers", "{} 颗心之容器"},
    {"{} tokens", "{} 枚徽章"},
    {"{} other things", "{} 样其他东西"},
    {"Your friend's game turned this one away: {}", "朋友的房间拒绝了这次加入：{}。"},
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
    {"Clear the code", "清除邀請碼"},
    {"Clear", "清除"},
    {"Empties the code field. Then paste or type your friend's code again.",
     "清空邀請碼欄。然後再貼上或輸入朋友的邀請碼。"},
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
    {"Your name, Link's look, how many players can join your game, voice chat, the chat key, which controller this game reads, and players on the map.",
     "名字、林克外觀、房間人數上限、語音聊天、聊天鍵、本遊戲讀取哪個手把，以及地圖上的玩家。"},
    {"The chat's commands, and the key that opens the chat.", "聊天指令，以及開啟聊天的按鍵。"},
    {"Puts your code on the clipboard again. Share it in DMs only: it contains your internet address.",
     "把邀請碼再放進剪貼簿。只在私訊裡傳：它含有你的網路位址。"},
    {"Only when a friend's game shows an answer code (their way in was blocked): copy the code they "
     "send you, then press this.",
     "僅當朋友的遊戲顯示「應答碼」（他被路由器擋住）時：複製他傳來的碼，再按這裡。"},
    {"For friends in the same house, on your home network: copies one line with this PC's address. They paste it in Join a game instead of the code.",
     "給同屋、同一家用網路的朋友：複製含本機位址的一行，他在「加入房間」貼上這一行而不是邀請碼。"},
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
    {"Copy the code from your friend's message, then press this: it is pasted. On the same home network or a VPN with your friend? Paste the line from Same network or from VPN on their Host page instead (address and password). You can also type it.",
     "複製朋友傳來的邀請碼後按這裡即可貼上。同屋或同 VPN？改為貼上他「建立房間」頁上「同一網路」或「VPN」那一行（位址+密碼）。也可以手動輸入。"},
    {"Paste your friend's code or enter an IP address first.", "請先貼上朋友的邀請碼，或輸入 IP 位址。"},
    {"Joins your friend's game.", "加入朋友的房間。"},
    {"Split screen (experimental): two full games on this PC.", "分割畫面（實驗性）：在這台電腦上執行兩個完整遊戲。"},
    {"1. Two kits (or Nucleus Co-op), each started once.", "1. 兩個工具包（或 Nucleus Co-op），各啟動一次。"},
    {"2. Each game its own controller: Settings > Controller.", "2. 每個遊戲用自己的手把：設定 > 手把。"},
    {"3. Host in one game, Join in the other (Same network row).", "3. 一個遊戲裡建立房間，另一個裡加入（同一網路那一列）。"},
    {"Steps: PLAY-LAN.md. Native split screen comes later.", "步驟：PLAY-LAN.md。原生分割畫面以後提供。"},
    {"The chat key is off. To chat, choose a key in Settings, Chat key.", "聊天鍵已關閉。要聊天，請在「設定 → 聊天鍵」裡選一個鍵。"},

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
    {"Your clipboard is empty: copy your friend's answer code first, then press this again.",
     "剪貼簿是空的：請先複製朋友傳來的應答碼，再按一次這裡。"},
    {"That answer code could not be used: host again and ask your friend to join again.", "這個應答碼用不了：請重新開房，並讓朋友重新加入。"},
    {"The clipboard could not be used: write the code down instead.", "剪貼簿用不了：請把碼記下來。"},
    {"answer code taken: connecting to your friend", "已接受應答碼：正在連線朋友"},
    {"Connected!", "已連線！"},
    {"A settings file could not be written.", "有一個設定檔寫不進去。"},
    {"No co-op kit found with this game: only your name was saved (coop.ini).",
     "這個遊戲旁邊沒找到連線套件：只儲存了你的名字（coop.ini）。"},
    {"Saved. Co-op is on: some settings count the next time you host or join.",
     "已儲存。本局連線在跑：部分設定下次開房/加入時才生效。"},
    {"Link wears it when this menu closes.", "關閉此選單後林克就會換上。"},

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
    {"{}'s save is not here yet (it still loads by itself if it comes): Continue plays your own save, New Game starts a new one.",
     "{} 的存檔還沒到（到了仍會自動載入）：繼續遊戲用自己的存檔，新遊戲開一個新的。"},
    {"{} has no save yet: New Game starts one, Continue plays your own.",
     "{} 還沒有存檔：新遊戲開一個，繼續遊戲用自己的。"},
    {"Press Continue to play {}'s save.", "按「繼續遊戲」玩 {} 的存檔。"},
    {"Press {} in the game to open the chat box; Enter sends, Esc closes. A line that starts with / is a command, not chat. To use another key: Settings, Chat key.",
     "遊戲中按 {} 打開聊天框；Enter 傳送，Esc 關閉；以 / 開頭的行是指令。改鍵：「設定 → 聊天鍵」。"},
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
    {"Invite code copied to your clipboard. Share it in DMs only: it contains your internet address.",
     "邀請碼已複製到剪貼簿。只在私訊裡傳：它含有你的網路位址。"},
    {"It is copied: paste it into your chat with them (Ctrl+V).",
     "已複製：貼到和朋友的聊天裡（Ctrl+V）。"},
    {"Press \"Copy the code again\" to put it on the clipboard.", "按「再複製一次邀請碼」把它放進剪貼簿。"},
    {"Waiting for your friends to join...", "正在等朋友加入…"},
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
    {"Same home network or VPN as them? Ask for the line from \"Same network\" or \"VPN\" on their Host page.",
     "和他同屋或同一 VPN？請他給你「同一網路」或「VPN」那一行。"},

    {"No home-network or VPN address was found on this PC.",
     "這台電腦上沒找到內網或 VPN 位址。"},
    {"On a VPN? Start your VPN program (Radmin VPN, Hamachi), then press this again.",
     "在用 VPN？先啟動 VPN 程式（Radmin VPN、Hamachi），再按一次這裡。"},
    {"Type it into your chat with them, exactly as it is.",
     "原樣打進和朋友的聊天裡。"},
    {"If that does not work, try another of this PC's addresses in its place:", "如果不行，換用本機的另一個位址："},
    {"If Windows asks about the firewall, allow the game on private networks.",
     "如果 Windows 問防火牆，允許遊戲在專用網路上通行。"},
    {"home network", "內網"},

    {"Paste or type the invite code your host sent you first.",
     "請先貼上或輸入房主傳來的邀請碼。"},
    {"That invite code is not right; ask the host to copy it again.",
     "這個邀請碼不對：請房主再複製一次。"},
    {"The join password after the address: no spaces, at most 128 characters.",
     "位址後面的加入密碼：不能有空格，最多 128 個字元。"},
    {"That is not an invite code or a server address.", "這不是邀請碼，也不是伺服器位址。"},
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

    // ---- 0.11: the rows and lines added since (pr224 merge) -------------
    {"Same network", "同一網路"},
    {"Paste friend's answer (or Ctrl+V)", "貼上朋友的應答碼（或 Ctrl+V）"},
    {"For friends on a VPN with you (Radmin VPN, Hamachi, ZeroTier, Tailscale, WireGuard): copies one line with this PC's VPN address. They paste it in Join a game instead of the code.",
     "給同一 VPN（Radmin VPN、Hamachi、ZeroTier、Tailscale、WireGuard）的朋友：複製含本機 VPN 位址的一行，他在「加入房間」貼上這一行而不是邀請碼。"},
    {"Your invite code is below. Share it in DMs only: it contains your internet address.",
     "你的邀請碼在下面。只在私訊裡傳：它含有你的網路位址。"},
    {"Customize Link", "自訂林克"},
    {"A colour for each part of your tunics, shields, swords, boots and gauntlets, with a picture of the result. Your Link wears it when you close the menu; the other players see your tunic's colour.",
     "為束衣、盾、劍、靴子和護手的每個部位選顏色，附效果圖。關閉選單時林克換上；其他玩家能看到你束衣的顏色。"},
    {"Chat key", "聊天鍵"},
    {"The key that opens the chat box in the game. Off: no chat box.", "遊戲中打開聊天框的鍵。關閉：沒有聊天框。"},
    {"Controller", "手把"},
    {"Which controller this game reads: all of them (only while its window is in front), one of them (also behind another window: two games on one PC, each with its own), or none.",
     "本遊戲讀取哪個手把：全部（僅當視窗在前景時）、其中一個（視窗在背景也讀：一台電腦兩個遊戲各用各的），或不讀。"},
    {"Keyboard and mouse", "鍵盤和滑鼠"},
    {"Window in front: the keyboard and mouse count while this game's window is in front. Also in background: the keys count behind other windows too. Off: neither (a controller only).",
     "視窗在前景：本遊戲視窗在前景時鍵盤和滑鼠有效。背景也有效：在其他視窗後面按鍵也有效。關閉：都不用（只用手把）。"},
    {"Show players on map", "在地圖上顯示玩家"},
    {"The other players as arrows in their colours on the minimap and on the Map page's area and dungeon maps (not the world map). Off: neither shows them.",
     "其他玩家以各自顏色的箭頭顯示在小地圖和地圖頁的區域圖、迷宮圖上（世界地圖不顯示）。關閉：都不顯示。"},
    {"Names on map", "地圖上的名字"},
    {"Each player's name beside their arrow on the maps. Off: the arrows only.", "地圖上每個箭頭旁顯示玩家名字。關閉：只顯示箭頭。"},
    {"Split screen (experimental) runs two full games on this PC, one per player: about 10 GB of RAM each and a strong graphics card. Start two kits (or use Nucleus Co-op), give each game its own controller in Settings, Controller (controller 1, controller 2 or the keyboard), then Host a game in one and Join a game in the other with the Same network line. The steps are in PLAY-LAN.md. Native split screen, one game for both, comes in a later release.",
     "分割畫面（實驗性）會在這台電腦上為每位玩家執行一個完整遊戲：每個約需 10 GB 記憶體，還要一張強勁的顯示卡。啟動兩個工具包（或使用 Nucleus Co-op），在「設定」的「手把」裡給每個遊戲各選一個手把（手把 1、手把 2 或鍵盤），然後在一個遊戲裡「建立房間」，在另一個裡用「同一網路」那一列「加入房間」。步驟見 PLAY-LAN.md。原生分割畫面（兩人一個遊戲）將在以後的版本中提供。"},
    {"No home-network address was found on this PC.", "這台電腦上沒找到家用網路位址。"},
    {"Friends somewhere else join with your invite code; on a VPN with you, they use the VPN row.",
     "不在同屋的朋友用邀請碼加入；和你同一 VPN 的朋友用「VPN」那一行。"},
    {"No VPN found.", "沒找到 VPN。"},
    {"Using one (Radmin VPN, Hamachi, ZeroTier, Tailscale, WireGuard)? Start it, then press this again.",
     "在用 VPN（Radmin VPN、Hamachi、ZeroTier、Tailscale、WireGuard）？先啟動它，再按一次這裡。"},
    {"Host a game first: a friend's answer code is pasted while you host.", "請先開房：朋友的應答碼要在開房時貼上。"},
    {"There is no invite code this time, so no answer code: friends on the same network or a VPN use the Same network or the VPN row.",
     "這次沒有邀請碼，所以也沒有應答碼：同屋或同 VPN 的朋友請用「同一網路」或「VPN」那一行。"},
    {"No co-op kit found with this game: the choice counts until the game closes.", "這個遊戲旁邊沒找到連線套件：這個選擇到遊戲關閉前有效。"},
    {"A settings file could not be written: the choice counts until the game closes.", "有一個設定檔寫不進去：這個選擇到遊戲關閉前有效。"},
    {"The clipboard could not be read (another program had it open): press this again.", "剪貼簿讀不了（被別的程式占用）：再按一次這裡。"},
    {"That is your own invite code: copy the answer code your friend sends back to you.", "這是你自己的邀請碼：請複製朋友回傳給你的應答碼。"},
    {"Getting {}'s save: it loads by itself when it is here...", "正在取得 {} 的存檔：到了會自動載入…"},
    {"{}'s game ended before the save came: Continue plays your own save, New Game starts a new one.",
     "{} 的遊戲在存檔送到前結束了：繼續遊戲用自己的存檔，新遊戲開一個新的。"},
    {"Host save loading, don't load a save.", "房主存檔載入中，先別載入存檔。"},
    {"Success, load any save.", "成功，載入任意存檔。"},
    {"Success, load any save for changes to occur.", "成功，載入任意存檔以生效。"},
    {"You joined with a file loaded: the host's save is coming. To play from the host's progress, go back to the "
     "title and load a save once the chat says Success.",
     "你在已載入存檔時加入：房主的存檔正在傳來。要從房主的進度遊玩，請回到標題畫面，等聊天欄顯示「成功」後再載入存檔。"},
    {"That save loaded before the host's save was in: go back to the title and load a save after Success.",
     "這個存檔在房主存檔送到前就載入了：回到標題畫面，等「成功」後再載入存檔。"},
    {"The host's save did not come: play on, or join again to get it.", "房主的存檔沒有送到：繼續玩，或重新加入來取得。"},
    {"The host has no save yet: New Game starts one, Continue plays your own.", "房主還沒有存檔：新遊戲開一個，繼續遊戲用自己的。"},
    {"Getting {}'s save: don't load a save yet...", "正在取得 {} 的存檔：先別載入存檔…"},
    {"{}'s save is in: load any save.", "{} 的存檔已到：載入任意存檔。"},
    {"{}'s save is not here yet (the chat says Success when it is): Continue plays your own save, New Game starts a new one.",
     "{} 的存檔還沒到（到了聊天會顯示「成功」）：繼續遊戲用自己的存檔，新遊戲開一個新的。"},

    // ---- The Lobby and Mods tabs (0.11) --------------------------------
    {"Lobby", "大廳"},
    {"Mods", "模組"},
    {"You are not in a game.", "你不在任何遊戲裡。"},
    {"Host one or join a friend's on the Multiplayer tab; the players show here.", "在「連線」頁建立房間或加入朋友的房間，玩家會顯示在這裡。"},
    {"The players show here once this game is in the session.", "這局進了遊戲後，玩家會顯示在這裡。"},
    {"Load your file to play: the others see you once you do.", "載入存檔才能玩：載入之後其他玩家才看得到你。"},
    {"Puts the invite code on the clipboard. Share it in DMs only (it contains your internet address); your friend pastes it in Join a game.",
     "把邀請碼複製到剪貼簿。只私訊發給朋友（裡面有你的公網位址）；他把它貼上到「加入房間」裡。"},
    {"Co-op is starting...", "連線正在啟動…"},
    {"Open place", "空位"},
    {"Invite", "邀請"},
    {"Copy the invite code", "複製邀請碼"},
    
    {"The invite code is not ready yet (see the Multiplayer tab).", "邀請碼還沒準備好（見「連線」頁）。"},
    {"Only the host has the invite code: ask them for it.", "只有房主有邀請碼：向他要。"},
    {"Host's game", "房主的遊戲"},
    {"Not known yet", "還不知道"},
    {"Host", "房主"},
    {"Mute their voice (only you)", "屏蔽他的語音（只對你）"},
    {"You stop hearing them. Only in your game: nobody is told.", "你不再聽到他說話。只在你這邊生效，不會通知任何人。"},
    {"Mute their chat (only you)", "屏蔽他的聊天（只對你）"},
    {"Their chat lines stop showing in your game. Nobody is told.", "他的聊天不再顯示在你的遊戲裡。不會通知任何人。"},
    {"Teleport to them", "傳送到他身邊"},
    {"Your Link goes to where they stand, as /teleport does (they must not be busy or in a boss fight).",
     "你的林克傳送到他站的位置，效果同 /teleport（他不能正在忙碌或打 BOSS）。"},
    {"This is you. Pick another player to mute them or go to them.", "這是你自己。選擇其他玩家可以屏蔽他或傳送到他那裡。"},
    {"Mute their voice for everyone", "對所有人屏蔽他的語音"},
    {"The server stops passing their voice on to anyone.", "伺服器不再把他的語音轉發給任何人。"},
    {"Mute their chat for everyone", "對所有人屏蔽他的聊天"},
    {"The server drops their chat lines.", "伺服器會丟棄他的聊天訊息。"},
    {"Kick", "踢出"},
    {"Takes them out of the game. They can join again.", "把他踢出遊戲，他還能再加入。"},
    {"Ban for this session", "本局封禁"},
    {"Takes them out; they cannot join again, under any name, until you stop hosting.", "把他踢出；在你停止開房之前，他換任何名字都進不來。"},
    {"Press again to kick", "再按一次踢出"},
    {"Press again to ban", "再按一次封禁"},
    {"The invite code is on the clipboard.", "邀請碼已放進剪貼簿。"},
    {"Resync from host", "從房主重新同步"},
    {"Gets the host's save again. Back on the title, it is switched to once the chat says Success; while you play nothing changes until then. Your own saves are never touched.", "重新取得房主的存檔。回到標題畫面，聊天顯示 Success 後切換過去；遊戲中在那之前什麼都不變。你自己的存檔不會被改動。"},
    {"Asked the host for its save: the chat says how it goes.", "已向房主要存檔：進展見聊天。"},
    {"The players of your game show here once you host or join.", "開房或加入後，你遊戲裡的玩家會顯示在這裡。"},
    {"downed", "倒地"},
    {"out", "出局"},
    {"fainted", "力盡"},
    {"talking", "說話中"},
    {"muted", "已靜音"},
    {"voice", "語音"},
    {"chat", "聊天"},
    {"voice and chat", "語音和聊天"},
    {"Off / On", "關 / 開"},
    {"red", "紅色"},
    {"orange", "橙色"},
    {"yellow", "黃色"},
    {"green", "綠色"},
    {"cyan", "青色"},
    {"blue", "藍色"},
    {"purple", "紫色"},
    {"pink", "粉色"},
    {"white", "白色"},
    {"black", "黑色"},
    {"brown", "棕色"},
    {"gray", "灰色"},
    {"grey", "灰色"},
    {"This mod is gone from the folder.", "這個模組已從資料夾裡消失。"},
    {"Randomizer", "隨機模式"},
    {"Off: the game leaves this mod out from its next start.", "關閉：遊戲下次啟動時不會載入這個模組。"},
    {"Move up", "上移"},
    {"Up: it wins a file it shares with the mods under it.", "上移：與下面模組共用的檔案，以它為準。"},
    {"Move down", "下移"},
    {"Down: the mods above it win a file they share.", "下移：共用的檔案以它上面的模組為準。"},
    {"(empty)", "（空）"},
    {"In this co-op game the host sets this.", "連線時由房主設定。"},
    {"Counts after a restart.", "重新啟動後生效。"},
    {"Type or Ctrl+V, then Enter. Esc: as it was.", "輸入或按 Ctrl+V，然後按 Enter。Esc：保持原樣。"},
    {"Restart the game to use the changes.", "重新啟動遊戲以套用變更。"},
    {"Switch", "切換"},
    {"Co-op: waiting for the host's list of mods.", "連線：正在等待房主的模組清單。"},
    {"Co-op: your mods match the host's.", "連線：你的模組與房主一致。"},
    {"Host: {}", "房主：{}"},
    {"Host: {} (you)", "房主：{}（你）"},
    {"{} (you)", "{}（你）"},
    {"Players  {}", "玩家  {}"},
    {"Player {}", "玩家 {}"},
    {", the host", "，房主"},
    {"{} hearts", "{} 顆心"},
    {"{} ms", "{} 毫秒"},
    {"{} / {} hearts", "{} / {} 顆心"},
    {"{} more player can join", "還能進 {} 人"},
    {"{} more players can join", "還能進 {} 人"},
    {"The host muted their {} for everyone.", "房主對所有人屏蔽了他的{}。"},
    {"Teleporting to {}: the chat says how it goes.", "正在傳送到 {}：過程會顯示在聊天裡。"},
    {"Kicking {}.", "正在踢出 {}。"},
    {"Banning {}.", "正在封禁 {}。"},
    {"Restart needed: {}", "需要重新啟動：{}"},
    {"No mods in {}. Put each mod in its own folder there.", "{} 裡沒有模組。每個模組放一個自己的資料夾。"},
    {"Randomizer: {}", "隨機模式：{}"},
    {"Co-op: {}", "連線：{}"},
    {"Player {} (you)", "玩家 {}（你）"},
    {"Player {}. Press to see what you can do.", "玩家 {}。按這裡查看能做什麼。"},
    {"Player {}. Press to see yourself.", "玩家 {}。按這裡查看自己的情況。"},
    {"Voice mode", "語音模式"},
    {"Open mic: you are heard whenever you speak (silence and quiet noise are not sent). Push to talk: only while you hold {}. On speakers, not headphones? Push to talk keeps the others' voices from coming back to them through your microphone.",
     "自由麥：你說話時大家都能聽到（靜音和輕聲不會發送）。按鍵說話：只有按住 {} 時才發送。用喇叭而不是耳機？用按鍵說話可以避免別人的聲音從你的麥克風傳回去。"},
    {"Starts a game on this PC and makes a code. Send the code to your friends; they choose Join a game and paste it. They play on your save.",
     "在本機開房並產生邀請碼。把碼發給朋友，他們選「加入房間」貼上。他們玩的是你的存檔。"},
    {"Joins a friend's game with the code they sent you. You play on their save; your own saves are not touched.",
     "用朋友傳來的邀請碼加入他的房間。你玩他的存檔，自己的存檔不受影響。"},
    {"Only when a friend's game shows an answer code (their way in was blocked): copy the code they send you, then press this.",
     "僅當朋友的遊戲顯示「應答碼」（他被路由器擋住）時：複製他傳來的碼，再按這裡。"},
    {"Your friend's router blocks the way in: send them this answer code (it is copied already); they paste it on their Host page.",
     "朋友的遊戲被路由器擋住了：把這個應答碼發給他（已複製），他貼到自己的「建立房間」頁。"},
    {"Joins the last game you were in, with the same code. It works while your friend is still hosting that game.",
     "用同一邀請碼回到上次的房間（朋友仍在開房時有效）。"},
    {"The colour of your Link's tunic and cap, as you and the other players see it. By player number: each player's own colour.",
     "你的林克束衣和帽子的顏色，你自己和其他玩家看到的樣子。按玩家編號：每人用自己的顏色。"},
    {"Talk with players near your Link through your microphone; they sound quieter the farther away they are. Off: you neither send nor hear voices.",
     "用麥克風與林克附近的玩家說話；離得越遠聲音越小。關閉：不傳送也不接收語音。"},
    {"Push to talk", "按鍵說話"},
    {"Open mic", "自由麥"},
    {"The host only: takes that player out of the game. They can join again.", "僅房主：把該玩家踢出遊戲。他還能再加入。"},
    {"The host only: takes that player out, and they cannot join again, under any name, until the host stops hosting.",
     "僅房主：把他踢出；在房主停止開房之前，他換任何名字都進不來。"},
    {"The host: nobody hears that player's voice or sees their chat (the server stops passing them on); add voice or chat for only that (/mute 2 voice). Anyone else: you stop hearing and seeing them yourself, and nobody is told.",
     "房主：沒人再聽到他的語音或看到他的聊天（伺服器停止轉發）；只針對語音或聊天就加 voice 或 chat（/mute 2 voice）。其他人：你自己不再聽到他、看到他的聊天，不會通知任何人。"},
    {"Undoes /mute (add voice or chat for only that).", "撤銷 /mute（只對語音或聊天就加 voice 或 chat）。"},
    {"Takes you to that player: the whole name, the start of it, or the player's number (/teleport 2). In another place, talk to someone or read a sign within a minute to travel. Not during a cutscene, a talk or a boss fight.",
     "把你傳送到該玩家身邊：全名、名字開頭，或玩家編號（/teleport 2）。在別的地點時，一分鐘內與人對話或讀告示即可傳送。過場動畫、對話或 BOSS 戰中不行。"},
    
    
    {"VPN", "VPN"},
    {"Your friend's router blocks the way in: send them this answer code (it is copied already); they paste it on their Host page.",
     "朋友的遊戲被路由器擋住了：把這個應答碼發給他（已複製），他貼到自己的「建立房間」頁。"},
    
    {"You", "你"},
    {"You are down", "你倒下了"},
    {"is reviving you", "正在救你"},
    {"Reviving", "正在救"},
    {"Hold A to revive", "按住 A 施救"},
    {"Watching", "正在觀看"},
    {"A: next player", "A：下一位玩家"},
    {"(out of sight)", "（不在視野內）"},
    {"all", "全部"},
    {"none (keyboard + mouse)", "不讀（鍵盤和滑鼠）"},
    {" (not connected)", "（未連接）"},
    {"window in front", "視窗在前景"},
    {"also in background", "背景也有效"},
    {"off", "關閉"},
    {"Tunic", "束衣"},
    {"Kokiri Tunic", "科奇里束衣"},
    {"Goron Tunic", "哥隆束衣"},
    {"Zora Tunic", "佐拉束衣"},
    {"Hylian Shield", "海利亞盾"},
    {"Mirror Shield", "鏡盾"},
    {"Deku Shield", "德庫盾"},
    {"Kokiri Sword", "科奇里劍"},
    {"Master Sword", "大師之劍"},
    {"Biggoron's Sword", "大古隆之劍"},
    {"Boots", "靴子"},
    {"Gauntlets", "護手"},
    {"Frame", "外框"},
    {"Face", "面"},
    {"Emblem", "紋章"},
    {"Rim", "邊"},
    {"Mirror", "鏡面"},
    {"Wood", "木"},
    {"Blade", "劍身"},
    {"Hilt", "劍柄"},
    {"Leather", "皮革"},
    {"Metal", "金屬"},
    {"The tunic and the cap (one piece on adult Link).", "束衣和帽子（成年林克是連體的）。"},
    {"The red cloth; the gold trim and the plates stay.", "紅色布料；金色滾邊和甲片不變。"},
    {"The blue cloth and the scale pattern.", "藍色布料和鱗片花紋。"},
    {"The metal rim, and the cross on the back.", "金屬外框，以及背面的十字。"},
    {"The blue face.", "藍色的盾面。"},
    {"The red bird; the gold Triforce stays.", "紅色鳥紋；金色三角不變。"},
    {"The red rim.", "紅色邊。"},
    {"The mirror.", "鏡面。"},
    {"The whole shield: it is one piece of wood.", "整個盾：它是一整塊木頭。"},
    {"The blade only: the grip is the Hilt below.", "僅劍身：握柄是下面的劍柄。"},
    {"The blade only: the grip and wings are the Hilt below; the gold stays.", "僅劍身：握柄和翼是下面的劍柄；金色部分不變。"},
    {"The leather grip.", "皮革握柄。"},
    {"The blue grip and wings; the gold and the gem stay.", "藍色握柄和翼；金色部分與寶石不變。"},
    {"The blue grip.", "藍色握柄。"},
    {"The leather of every pair: the Kokiri, Hover and Iron Boots (young Link's belts too).",
     "每一雙的皮革：科奇里靴、浮空靴和鐵靴（幼年林克的腰帶也是）。"},
    {"The metal of the Silver and the Golden Gauntlets.", "銀色護手和金色護手的金屬部分。"},
    {"Left and Right try the colours; Confirm mixes your own. Your Link wears it when you close the menu (the other players see only the tunic's colour for now).",
     "左右方向鍵試顏色；確認則自己調色。關閉選單時林克就換上（其他玩家目前只能看到你束衣的顏色）。"},
    {"All parts back to the game's own", "全部恢復遊戲自帶"},
    {"Every part in the game's own colours again (the tunic: your player number's colour for the others).",
     "所有部位都恢復遊戲自帶的顏色（束衣：在其他玩家眼裡是你編號對應的顏色）。"},
    {"Game's own colour", "遊戲自帶的顏色"},
    {"This part back to the game's own colour.", "把這個部位恢復成遊戲自帶的顏色。"},
    {"Back to the list of parts.", "回到部位列表。"},
    {"Game's own", "遊戲自帶"},
    {"Preset", "預設"},
    {"Left and Right: the preset colours.", "左右：預設顏色。"},
    {"Mixed", "自訂"},
    {"Colour", "顏色"},
    {"Mix", "調色"},
    {"Red", "紅"},
    {"Green", "綠"},
    {"Blue", "藍"},
    
    {"OoT Remake Randomizer", "OoT Remake 隨機模式"},
    {"Seed", "種子"},
    {"Game folder", "遊戲資料夾"},
    {"OOTRR program", "OOTRR 程式"},
    {"On: the game plays a randomized seed (start a new save for it).", "開啟：遊戲按隨機種子執行（需要開一個新存檔）。"},
    {"Letters and digits. Empty: a new one is made. In co-op the host's seed is everyone's.",
     "字母和數字。留空：自動產生。連線時所有人用房主的種子。"},
    
    {"Only for other game files (1.1.1, with Pack, Banc, Event and System). Empty: the kit's own (romfs beside the game).",
     "只在用別的遊戲檔案時填（1.1.1，含 Pack、Banc、Event 和 System）。留空：用工具包自帶的（遊戲旁邊的 romfs）。"},
    {"Only for another OOTRR. Empty: the one the kit brings (tools\\ootrr beside the game).",
     "只在用別的 OOTRR 時填。留空：用工具包自帶的（遊戲旁邊的 tools\\ootrr）。"},
    {"Shuffles chests, dungeon items and rewards (OOTRR). In co-op everyone plays the host's seed and every pickup is shared. A new seed needs a restart of the game, never a game build.",
     "打亂寶箱、迷宮道具和獎勵（OOTRR）。連線時所有人玩房主的種子，拾取共享。換種子只需重新啟動遊戲，不用重新建置。"},
    
    {"By {}", "作者：{}"},
    {", version {}", "，版本 {}"},
    {"The order of the mods changed: it counts from the next start.", "模組順序已改變：下次啟動生效。"},
    {"changes at the next start.", "將在下次啟動生效。"},
    
    {"below", "下層"},
    {"above", "上層"},
    {"By", "作者"},
    {", version", "，版本"},
    {"Push to talk: only while you hold", "按鍵說話：只有按住"},
    {"On speakers, not headphones? Push to talk keeps the others' voices from coming back to them through your microphone.", "用喇叭而不是耳機？用按鍵說話可以避免別人的聲音從你的麥克風傳回去。"},
    {". Press to see ", "。按這裡查看"},
    {"yourself.", "自己的情況。"},
    {"what you can do.", "能做些什麼。"},
    {"Open mic: you are heard whenever you speak (silence and quiet noise are not sent).", "自由麥：你說話時大家都能聽到（靜音和輕聲不會發送）。"},
    {", ", "，"},
    {"Host: ", "房主："},
    {": ", "："},
    {"Default", "預設"},
    {"Easy", "簡單"},
    {"Open world", "開放世界"},
    {"Keysanity", "鑰匙亂序"},
    {"Full shuffle", "完全亂序"},
    {"IP address", "IP 位址"},
    {"Port", "連接埠"},
    {"Port when you host", "建房連接埠"},
    {"The UDP port other players reach you on (default 27510). Type a number from 1 to 65535; changing it while hosting restarts hosting: friends join again with the new code.",
     "其他玩家連線至你使用的 UDP 連接埠（預設 27510）。輸入 1 到 65535 之間的數字；建房期間修改會重新建房：朋友要用新的邀請碼重新加入。"},
    {"Type the host's IP address or host name directly (no invite code needed).",
     "直接輸入房主的 IP 位址或主機名稱（不需要邀請碼）。"},
    {"The host's UDP port (default 27510). Type a number from 1 to 65535.",
     "房主的 UDP 連接埠（預設 27510）。輸入 1 到 65535 之間的數字。"},
    {"The UDP port your game listens on when you host (default 27510). Type a number from 1 to 65535.",
     "你建房時遊戲監聽的 UDP 連接埠（預設 27510）。輸入 1 到 65535 之間的數字。"},
    {"IP address: enter a valid IP address or host name (with optional :port). It was not changed.",
     "IP 位址：請輸入有效的 IP 位址或主機名稱（可附帶 :連接埠），未修改。"},
    {"Port: enter a number from 1 to 65535. It was not changed.",
     "連接埠：請輸入 1 到 65535 之間的數字，未修改。"},
    {"Password", "密碼"},
    {"The host's join password, for an IP address: the word after the address on their Same network or VPN line. An invite code has it inside.",
     "用 IP 位址加入時房主的加入密碼：房主「同一網路」或「VPN」那一行裡位址後面的詞。邀請碼裡已經包含密碼。"},
    {"Password: no spaces, at most 128 characters. It was not changed.",
     "密碼：不能有空格，最多 128 個字元，未修改。"},
    // The boss room's chat notices (lichii/hook/coop_boss.cpp, #50): "<name> took the blue warp".
    {"{} took the blue warp", "{} 進入了藍色傳送光"},
    {"{} left the boss room", "{} 離開了 Boss 房間"},
    // ---- Ours, on top of the co-op release before this one (the tabs, the chat's notices, the
    //      pickup notices, the item and place names) ----
    {"The blade.", "劍身。"},
    {"connection lost, reconnecting", "連線斷了，正在重連"},
    {"the host muted your chat: nobody sees it", "房主封鎖了你的聊天：現在沒人看得到"},
    {"you host now: the Lobby's host controls are yours", "你現在是房主：「大廳」頁的房主操作歸你了"},
    {"Difficulty: could not be sent", "難度：沒能發出去"},
    {"teleport: wait until your game is loaded", "傳送：等你的遊戲載入好"},
    {"teleport: not during a cutscene or a talk", "傳送：過場或對話時不行"},
    {"teleport: not during your boss fight", "傳送：打 Boss 的時候不行"},
    {"teleport: stopped: your game got busy (a load, a cutscene or a fight)", "傳送：已停下：你的遊戲忙起來了（載入、過場或戰鬥）"},
    {"teleport: talk to someone or read a sign within a minute to go there", "傳送：一分鐘內和某人說話或讀一個告示，就能過去"},
    {"teleport: the game did not change scene in time; stopped", "傳送：遊戲沒能及時換場景，已停下"},
    {"teleport: stopped: the new scene did not settle", "傳送：已停下：新場景沒穩定下來"},
    {"teleport: off in this game (LICHII_COOP_TELEPORT=0)", "傳送：本局關掉了（LICHII_COOP_TELEPORT=0）"},
    {"teleport: could not ask (not connected?)", "傳送：問不到對方（沒連上？）"},
    {"teleport: stopped: the connection was lost", "傳送：已停下：連線斷了"},
    {"lobby: not connected", "大廳：沒連上"},
    {"lobby: could not be sent", "大廳：沒能發出去"},
    {"Mods: yours now match the host's", "模組：你的和房主一致了"},
    {"child", "少年"},
    {"adult", "成年"},
    {"Normal", "普通"},
    {"Hard", "困難"},
    {"Custom", "自訂"},
    {"joined as player {}", "已加入：玩家 {}"},
    {"reconnected as player {}", "已重新連上：玩家 {}"},
    {"{} joined", "{} 加入了"},
    {"{} left", "{} 離開了"},
    {"server {}", "伺服器：{}"},
    {"and {} more", "還有 {} 個"},
    {"teleport: next to {}", "傳送：已經到 {} 身邊"},
    {"teleport: asking {} where they are", "傳送：在問 {} 在哪裡"},
    {"teleport: {} is coming to you", "傳送：{} 正在過來"},
    {"teleport: stopped: you left {}", "傳送：已停下：你離開了 {}"},
    {"teleport: stopped: {} left", "傳送：已停下：{} 離開了"},
    {"Difficulty: Automatic ({} players): life {}%, damage {}%, enemies {}%; bosses: life {}%, damage {}%", "難度 自動（{}人）生命{}% 傷害{}% 敵人{}% · Boss生命{}% 傷害{}%"},
    {"Difficulty: Automatic ({} player): life {}%, damage {}%, enemies {}%; bosses: life {}%, damage {}%", "難度 自動（{}人）生命{}% 傷害{}% 敵人{}% · Boss生命{}% 傷害{}%"},
    {"Difficulty: {}: life {}%, damage {}%, enemies {}%", "難度 {}：生命{}% 傷害{}% 敵人{}%"},
    {"teleport: going to {} in {}", "傳送：去 {} 所在的 {}"},
    {"teleport: {} has gone on to {}; you are where they were. Type /teleport again to follow", "傳送：{} 已經去了 {}；你到了他剛才的位置。再輸入一次 /teleport 就能跟過去"},
    {"teleport: {} is in {}, a place for {} Link; yours is {}", "傳送：{} 在 {}，那是{}的林克去的地方；你這邊是{}"},
    {"teleport: {} is {} Link and you are {}: no teleport to another age's Link", "傳送：{} 是{}的林克，你是{}：不能傳送到另一個時代的林克那裡"},
    {"teleport: {} is busy now; you are where they were", "傳送：{} 現在正忙；你到了他剛才的位置"},
    {"teleport: no answer from {}; you are where they were", "傳送：{} 沒回應；你到了他剛才的位置"},
    {"teleport: no answer from {} (an older build, or their game is busy)", "傳送：{} 沒回應（他的版本較舊，或者遊戲正忙）"},
    {"teleport: {} is in a dungeon: you may need its items to get on from there", "傳送：{} 在迷宮裡：要從那裡繼續，可能需要裡面的道具"},
    {"teleport: {} is in {}, a place /teleport cannot load (no start point known there)", "傳送：{} 在 {}，那個地方 /teleport 載入不了（不知道起點）"},
    {"{}: not connected", "{}：沒連上"},
    {"{}: only the host can do that", "{}：只有房主能做"},
    {"{}: could not be sent", "{}：沒能發出去"},
    {"refused: {}", "被拒絕：{}"},
    {"Mods: yours differ from the host's ({}): see the Mods tab", "模組：你的和房主不一樣（{}）：看「模組」頁"},
    {"Mods: player {} ({}) has {} mods unlike yours", "模組：玩家 {}（{}）有 {} 個模組和你不一樣"},
    {"Mods: player {} has {} mods unlike yours", "模組：玩家 {} 有 {} 個模組和你不一樣"},
    {"Mods: player {} has {} mod unlike yours", "模組：玩家 {} 有 {} 個模組和你不一樣"},
    {"Mods: player {} ({}) has {} mod unlike yours", "模組：玩家 {}（{}）有 {} 個模組和你不一樣"},
    {"{} got the {} - you got one too!", "{} 拿到了{} —— 你也得到了一個！"},
    {"{} got the {} - you got them too!", "{} 拿到了{} —— 你也得到了！"},
    {"{} got the {}", "{} 拿到了{}"},
    {"{} found a Small Key ({}) - the group has {}", "{} 找到一把小鑰匙（{}）—— 隊伍現在有 {} 把"},
    {"{} got the map ({})", "{} 拿到了地圖（{}）"},
    {"{} got the compass ({})", "{} 拿到了指南針（{}）"},
    {"{} got the Boss Key ({})", "{} 拿到了 Boss 鑰匙（{}）"},
    {"{} got a Heart Container - your max health went up", "{} 得到一顆心之容器 —— 最大生命值提升了"},
    {"{} got a Heart Container", "{} 得到一顆心之容器"},
    {"{} got a Gold Skulltula token ({})", "{} 得到一枚金色骷髏蜘蛛徽章（{}）"},
    {"{} learned {}", "{} 學會了{}"},
    {"Synced with the group: {}", "已與隊伍同步：{}"},
    {"{} loaded a save - the group follows it ({} changed)", "{} 載入了存檔 —— 隊伍跟著切換（{} 項變化）"},
    {"...and {} more from the group", "……還有 {} 條隊伍的消息"},
    {"items", "道具"},
    {"songs", "歌曲"},
    {"keys", "鑰匙"},
    {"maps and compasses", "地圖和指南針"},
    {"heart containers", "心之容器"},
    {"tokens", "徽章"},
    {"other things", "其他東西"},
    {"Someone", "某人"},
    {"Pocket Cucco", "口袋咕咕雞"},
    {"Pocket Egg", "口袋蛋"},
    {"Bomb Bag Small", "小炸彈袋"},
    {"Bomb Bag Medium", "中炸彈袋"},
    {"Bomb Bag Large", "大炸彈袋"},
    {"Bombchus", "炸彈鼠"},
    {"Piece of Heart", "心之碎片"},
    {"Heart Container", "心之容器"},
    {"Small Key", "小鑰匙"},
    {"Boss Key", "Boss 鑰匙"},
    {"Longshot", "長鉤索"},
    {"Boomerang", "迴旋鏢"},
    {"Lens of Truth", "真實之鏡"},
    {"Magic Bean", "魔法豆"},
    {"Magic Bean Pack", "魔法豆袋"},
    {"Mask Keaton", "基頓面具"},
    {"Mask Skull", "骷髏面具"},
    {"Mask Spooky", "恐怖面具"},
    {"Mask Gerudo", "格魯德面具"},
    {"Mask Truth", "真實面具"},
    {"Mask Zora", "佐拉面具"},
    {"Mask Goron", "哥隆面具"},
    {"Mask Bunny", "兔子頭巾"},
    {"Weird Egg", "奇怪的蛋"},
    {"Chicken", "咕咕雞"},
    {"Zelda Letter", "薩爾達的信"},
    {"Weird Bottle", "奇怪的瓶子"},
    {"Empty Bottle", "空瓶"},
    {"Red Potion", "紅藥水"},
    {"Green Potion", "綠藥水"},
    {"Blue Potion", "藍藥水"},
    {"Fairy", "妖精"},
    {"Fish", "魚"},
    {"Milk", "牛奶"},
    {"Rutos Letter", "露多公主的信"},
    {"Poacher's Saw", "盜獵者的鋸"},
    {"Broken Goron's Sword", "折斷的大哥隆之劍"},
    {"Prescription", "處方"},
    {"Eyeball Frog", "眼球蛙"},
    {"Eyedrops", "眼藥水"},
    {"Claim Check", "取貨單"},
    {"Ocarina", "陶笛"},
    {"Stick", "木棒"},
    {"Nut", "堅果"},
    {"Bean", "豆子"},
    {"Hammer", "鎚子"},
    {"Slingshot", "彈弓"},
    {"Bow", "弓"},
    {"Arrow", "箭"},
    {"Scale", "鱗"},
    {"Gauntlet", "護手"},
    {"Shield", "盾"},
    {"Sword", "劍"},
    {"Stone", "寶石"},
    {"Medal", "徽章"},
    {"Fire", "火"},
    {"Ice", "冰"},
    {"Light", "光"},
    {"Forest", "森"},
    {"Water", "水"},
    {"Shadow", "暗"},
    {"Spirit", "魂"},
    {"Fairy Slingshot", "妖精彈弓"},
    {"Fairy Bow", "妖精弓"},
    {"Fire Arrows", "火箭"},
    {"Ice Arrows", "冰箭"},
    {"Light Arrows", "光箭"},
    {"Fairy Ocarina", "妖精陶笛"},
    {"Ocarina of Time", "時之陶笛"},
    {"Bombs", "炸彈"},
    {"Deku Nuts", "德庫堅果"},
    {"Deku Seeds", "德庫種子"},
    {"Deku Sticks", "德庫木棒"},
    {"Megaton Hammer", "巨力鎚"},
    {"Nayru's Love", "娜露之愛"},
    {"Din's Fire", "汀之炎"},
    {"Farore's Wind", "法羅之風"},
    {"Silver Scale", "銀鱗"},
    {"Golden Scale", "金鱗"},
    {"Kokiri's Emerald", "科吉里的翠玉"},
    {"Goron's Ruby", "哥隆的紅玉"},
    {"Zora's Sapphire", "佐拉的藍玉"},
    {"Forest Medallion", "森之徽章"},
    {"Fire Medallion", "火之徽章"},
    {"Water Medallion", "水之徽章"},
    {"Shadow Medallion", "暗之徽章"},
    {"Spirit Medallion", "魂之徽章"},
    {"Light Medallion", "光之徽章"},
    {"Gerudo Membership Card", "格魯德會員卡"},
    {"Giant's Knife", "巨人的小刀"},
    {"Hover Boots", "浮空靴"},
    {"Iron Boots", "鐵靴"},
    {"Silver Gauntlets", "銀護手"},
    {"Golden Gauntlets", "金護手"},
    {"Zelda's Lullaby", "薩爾達的搖籃曲"},
    {"Epona's Song", "艾波娜之歌"},
    {"Saria's Song", "薩莉亞之歌"},
    {"Sun's Song", "太陽之歌"},
    {"Song of Time", "時之歌"},
    {"Scarecrow's Song", "稻草人之歌"},
    {"Minuet of Forest", "森之小步舞曲"},
    {"Bolero of Fire", "火之波麗露"},
    {"Serenade of Water", "水之小夜曲"},
    {"Nocturne of Shadow", "暗之夜曲"},
    {"Requiem of Spirit", "魂之安魂曲"},
    {"Prelude of Light", "光之前奏曲"},
    {"Deku Tree", "德庫大樹"},
    {"Dodongo's Cavern", "多多哥洞窟"},
    {"Jabu-Jabu's Belly", "加布加布大人腹中"},
    {"Ganon's Castle", "加農城堡"},
    {"got the {} - you got one too!", "拿到了{} —— 你也得到了一個！"},
    {"got the {} - you got them too!", "拿到了{} —— 你也得到了！"},
    {"got the {}", "拿到了{}"},
    {"found a Small Key ({})", "找到一把小鑰匙（{}）"},
    {"got the map ({})", "拿到了地圖（{}）"},
    {"got the compass ({})", "拿到了指南針（{}）"},
    {"got the Boss Key ({})", "拿到了 Boss 鑰匙（{}）"},
    {"got a Heart Container", "得到一顆心之容器"},
    {"got a Gold Skulltula token ({})", "得到一枚金色骷髏蜘蛛徽章（{}）"},
    {"learned {}", "學會了{}"},
    {", the ", "、"},
    {"Joining your friend's game...", "正在加入朋友的房間…"},
    {"Press Leave, then join again.", "按「離開」，然後再加入一次。"},
    {"Press Leave.", "按「離開」。"},
    {"wrong join password", "加入密碼不對"},
    {"wrong game build", "遊戲版本不一致"},
    {"server full", "房間滿了"},
    {"name taken", "名字被佔了"},
    {"bad name", "名字不合法"},
    {"wrong protocol version", "協定版本不一致"},
    {"the host took you out of the game", "房主把你踢出了遊戲"},
    {"the host banned you from this game", "房主在這個房間封禁了你"},
    // (the refusal's full sentence: see the Simplified table)
    {"wrong protocol version: this game and the server are different releases; run Update.bat in every kit, the host's too",
     "協定版本不一致：遊戲和伺服器不是同一版本；請在每份套件裡都執行 Update.bat，房主的也要"},
    {"wrong join password: ask the host for it, then start Join.bat again and type it",
     "加入密碼不對：向房主要密碼，然後重新啟動 Join.bat 輸入"},
    {"the host took you out of the game: you can join again", "房主把你踢出了遊戲：可以再加入"},
    {"the host banned you from this game: you cannot join it again until the host stops hosting",
     "房主在這個房間封禁了你：房主停止建立房間後，你才能再加入"},
    {"the server did not ask for the join password, so the game did not join it unencrypted (every session has a password and is encrypted): the host's server is an older build, or someone on the way answered for it",
     "伺服器沒有索取加入密碼，所以遊戲沒有以未加密的方式連入（每個房間都有密碼並且加密）：房主的伺服器是舊版本，或者途中有程式替它應答了"},
    {"the host restarted its server (it has a new join password now): ask the host for the new invite, then start Join.bat again with it",
     "房主重啟了伺服器（現在有新的加入密碼）：向房主要新的邀請，然後重新用新密碼啟動 Join.bat"},
    {"unknown reason", "未知原因"},
    {"{} items", "{} 件道具"},
    {"{} songs", "{} 首歌曲"},
    {"{} keys", "{} 把鑰匙"},
    {"{} maps and compasses", "{} 份地圖和指南針"},
    {"{} heart containers", "{} 顆心之容器"},
    {"{} tokens", "{} 枚徽章"},
    {"{} other things", "{} 樣其他东西"},
    {"Your friend's game turned this one away: {}", "朋友的房間拒絕了這次加入：{}。"},
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

// The game's folder: the exe's, or the one above it when the exe is in a
// "bin" folder beside image.bin (Longshot's game_module_path, vk_paths.h).
// Longshot keeps language.txt and longshot_options.ini there.
std::string GameFolder() {
#ifdef _WIN32
    char exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    std::string dir(exe, n);
    const std::size_t cut = dir.find_last_of("\\/");
    if (cut == std::string::npos) {
        return {};
    }
    dir.resize(cut);
    const std::size_t up = dir.find_last_of("\\/");
    if (up != std::string::npos && dir.size() - up - 1 == 3 && _strnicmp(dir.c_str() + up + 1, "bin", 3) == 0 &&
        GetFileAttributesA((dir.substr(0, up) + "\\image.bin").c_str()) != INVALID_FILE_ATTRIBUTES) {
        dir.resize(up);
    }
    return dir;
#else
    return {};
#endif
}

// The game's language, which the Menu language row's Auto follows: HOS_LANGUAGE
// when the game was started with it, else language.txt's first line that is
// not a comment (a UTF-8 mark at its start skipped), else English: the game
// and Longshot's Auto run in en-US then and never ask the system, so the tabs
// match them (longshot-src host/nn_applet.c, gpu/vk/vk_menu_i18n.h
// game_language()). Read once: the game reads it at its start too, and Auto
// keeps that one until a restart.
std::string ReadLanguage() {
    if (const char* env = std::getenv("HOS_LANGUAGE"); env != nullptr && *env != '\0') {
        return Trim(env);
    }
    const std::string folder = GameFolder();
    if (!folder.empty()) {
        std::ifstream file(folder + "\\language.txt");
        std::string line;
        bool first = true;
        while (std::getline(file, line)) {
            if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) {
                line.erase(0, 3);
            }
            first = false;
            const std::string text = Trim(line);
            if (!text.empty() && text.front() != '#') {
                return text;
            }
        }
    }
    return "en";
}

// longshot_options.ini as it was last read: Longshot's menu writes it on
// every change of a row, so a change of Menu language shows here within a
// moment (its write time is looked at a few times a second, the file read
// only when that changed).
struct MenuIni {
    std::mutex mutex;
    std::chrono::steady_clock::time_point checked{};
    std::filesystem::file_time_type written{};
    bool seen = false;
    int value = 0;   // (menu_language=: 0 Auto)
};

int MenuLanguageNow() {
    static MenuIni ini;
    std::lock_guard lock(ini.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (ini.seen && now - ini.checked < std::chrono::milliseconds(250)) {
        return ini.value;
    }
    ini.checked = now;
    ini.seen = true;
    const std::string folder = GameFolder();
    if (folder.empty()) {
        return ini.value = 0;
    }
    // (the ANSI path GetModuleFileNameA gave, as language.txt's)
    const std::filesystem::path path(folder + "\\longshot_options.ini");
    std::error_code error;
    const auto written = std::filesystem::last_write_time(path, error);
    if (error) {
        return ini.value;   // (no options file yet: Auto; or it is being replaced: the last value)
    }
    if (written == ini.written) {
        return ini.value;
    }
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    bool good = false;
    ini.value = MenuLanguageAfterRead(ini.value, text.str(), &good);
    if (good) {
        ini.written = written;   // (else read again at the next look: Longshot was writing it)
    }
    return ini.value;
}

bool SameCode(std::string_view a, std::string_view b) {   // (pt-br is pt-BR, as Longshot's same_code)
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
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

}  // namespace


// ---------------------------------------------------------------------------
// The place names. They are not the tab's words but the game's own:
// places.json, which the puppet pack writes from the player's files
// (lichii/hook/coop_hud_places.h), names a scene ("Hyrule Field") and the HUD
// and the lobby draw that name. So they are a table of their own, apart from
// the tab's lines: a name the table does not have stays as the game wrote it,
// and the lines above are still checked to be lines of the module's source,
// which a place name is not.

struct PlaceName {
    const char* en;
    const char* hans;
    const char* hant;
};

constexpr PlaceName kPlaces[] = {
    {"Bottom of the Well", "井底", "井底"},
    {"Death Mountain Crater", "死亡火山口", "死亡火山口"},
    {"Death Mountain Trail", "死亡山道", "死亡山道"},
    {"Dodongo Cavern", "多多哥洞窟", "多多哥洞窟"},
    {"Evil Desert Colossus", "邪恶沙漠巨像", "邪惡沙漠巨像"},
    {"Fire Temple", "火之神殿", "火之神殿"},
    {"Forest Temple", "森之神殿", "森之神殿"},
    {"Ganondorf's Castle", "加农多夫城堡", "加農多夫城堡"},
    {"Gerudo Fortress", "格鲁德要塞", "格魯德要塞"},
    {"Gerudo Proving Grounds", "格鲁德试炼场", "格魯德試煉場"},
    {"Gerudo Valley", "格鲁德峡谷", "格魯德峽谷"},
    {"Goron City", "哥隆城", "哥隆城"},
    {"Graveyard", "墓地", "墓地"},
    {"Great Deku Tree", "德库大树", "德庫大樹"},
    {"Hall of Fallen Foes", "阵亡敌人之间", "陣亡敵人之間"},
    {"Hyrule Castle", "海拉鲁城堡", "海拉魯城堡"},
    {"Hyrule Castle Town", "海拉鲁城下町", "海拉魯城下町"},
    {"Hyrule Field", "海拉鲁平原", "海拉魯平原"},
    {"Ice Cavern", "冰之洞窟", "冰之洞窟"},
    {"Ingo Ranch", "英戈牧场", "英戈牧場"},
    {"Inside Ganondorf's Castle", "加农多夫城堡内部", "加農多夫城堡內部"},
    {"Kakariko Village", "卡卡利科村", "卡卡利科村"},
    {"Kokiri Forest", "科吉里森林", "科吉里森林"},
    {"Lake Hylia", "海利亚湖", "海利亞湖"},
    {"Lon Lon Ranch", "隆隆牧场", "隆隆牧場"},
    {"Lord Jabu-Jabu's Belly", "加布加布大人腹中", "加布加布大人腹中"},
    {"Lost Woods", "迷途森林", "迷途森林"},
    {"Mirage Desert", "蜃景沙漠", "蜃景沙漠"},
    {"Sacred Grove", "圣地之林", "聖地之林"},
    {"Shadow Temple", "暗之神殿", "暗之神殿"},
    {"Spirit Temple", "魂之神殿", "魂之神殿"},
    {"Temple of Time", "时之神殿", "時之神殿"},
    {"Water Temple", "水之神殿", "水之神殿"},
    {"Zora Domain", "佐拉领地", "佐拉領地"},
    {"Zora Fountain", "佐拉泉", "佐拉泉"},
    {"Zora River", "佐拉河", "佐拉河"},
};

const Lang kLangs[] = {
    {"en", "English", nullptr, 0},
    {"zh-Hans", "简体中文", kzh_Hans, int(sizeof kzh_Hans / sizeof kzh_Hans[0])},
    {"zh-Hant", "繁體中文", kzh_Hant, int(sizeof kzh_Hant / sizeof kzh_Hant[0])},
};
const int kLangCount = int(sizeof kLangs / sizeof kLangs[0]);

const Lang* ForCode(std::string_view code) {
    // The code itself, else its first part (zh-Hans-CN: zh-Hans; as the
    // menu's Auto does it), any case. English and unknown codes: nullptr.
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < kLangCount; ++i) {
            const std::string_view entry = kLangs[i].code;
            if (SameCode(code, entry) ||
                (pass == 1 && code.size() > entry.size() && code[entry.size()] == '-' &&
                 SameCode(code.substr(0, entry.size()), entry))) {
                return kLangs[i].entries != nullptr ? &kLangs[i] : nullptr;
            }
        }
    }
    // Chinese named by its region, with no script in the code ("zh-CN",
    // "zh-TW", "zh-HK", as a language.txt may have it): the script
    // is what the tables are apart by, so the regions stand for them here.
    // Simplified: the mainland, Singapore and Malaysia; traditional: Taiwan,
    // Hong Kong and Macau. Any other "zh-..." is taken as Simplified; other
    // languages' regions ("en-US", "pt-BR", "es-419") are not Chinese.
    if (code.size() > 3 && code[2] == '-' && SameCode(code.substr(0, 2), "zh")) {
        const std::string_view region = code.substr(3);
        const bool traditional = SameCode(region, "TW") || SameCode(region, "HK") || SameCode(region, "MO") ||
                                 SameCode(region, "Hant");
        return ForCode(traditional ? "zh-Hant" : "zh-Hans");
    }
    return nullptr;
}

int MenuLanguageSetting(std::string_view ini) {
    int value = -1;
    std::size_t at = 0;
    while (at < ini.size()) {
        std::size_t end = ini.find('\n', at);
        if (end == std::string_view::npos) {
            break;   // (a last line with no line end: cut while Longshot writes it, "menu_language=1" of 13)
        }
        std::string_view line = ini.substr(at, end - at);
        at = end + 1;
        constexpr std::string_view kKey = "menu_language=";
        if (line.substr(0, kKey.size()) != kKey) {
            continue;
        }
        line.remove_prefix(kKey.size());
        int n = 0;
        std::size_t digits = 0;
        while (digits < line.size() && digits < 4 && std::isdigit(static_cast<unsigned char>(line[digits]))) {
            n = n * 10 + (line[digits] - '0');
            ++digits;
        }
        value = digits > 0 ? n : -1;   // (the last line wins, as Longshot's own reader)
    }
    return value;
}

int MenuLanguageAfterRead(int last, std::string_view ini, bool* good) {
    // Longshot writes the whole file again on each change (fopen "w"): read
    // in between, it is empty or cut short. Such a read keeps the last value.
    const int value = MenuLanguageSetting(ini);
    if (good != nullptr) {
        *good = value >= 0;
    }
    return value >= 0 ? value : last;
}

const char* MenuLanguageCode(int setting) {
    // Longshot's menu_i18n.h kLangs in order (rc.3 0c74253d, same on rc.4 53f96d78; check again at
    // each re-pin): the Menu language row is 0 Auto, then kLangs[setting - 1].
    static constexpr const char* kMenuLangs[] = {"en", "pt-BR", "es-419", "es", "fr", "de", "it",
                                                 "nl", "pl",    "ru",     "ja", "ko", "zh-Hans", "zh-Hant"};
    constexpr int kCount = int(sizeof kMenuLangs / sizeof kMenuLangs[0]);
    if (setting <= 0) {
        return nullptr;   // (Auto: the game's language)
    }
    return setting <= kCount ? kMenuLangs[setting - 1] : "en";   // (a newer Longshot's language: none of ours)
}

const Lang* Current() {
    // Longshot's Menu language row: the tabs are drawn in that menu, with the
    // glyphs it merged for that language (vk_imgui.cpp script_fonts).
    if (const char* menu = MenuLanguageCode(MenuLanguageNow())) {
        return ForCode(menu);
    }
    static const Lang* const game = ForCode(ReadLanguage());   // (Auto)
    return game;
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
               "not chat. To use another key: Settings, Chat key.",
     "Press {} in the game to open the chat box; Enter sends, Esc closes. A line that starts with / is a command, "
     "not chat. To use another key: Settings, Chat key.", false},
    {"Co-op is on: ", " players", "Co-op is on: {} players", false},
    {"Hosting: ", " players", "Hosting: {} players", false},
    {"Join ", " again", "Join {} again", false},
    {"No ", " yet.", "No {} yet.", true},
    {"Copied: ", "", "Copied: {}", false},
    {"", " friend(s) joined.", "{} friend(s) joined.", false},
    // The boss room's chat notices (lichii/hook/coop_boss.cpp, #50): "<name> took the blue warp". Before
    // the shapes with an empty end below ("Player {}"), which a name could also fit.
    {"", " took the blue warp", "{} took the blue warp", false},
    {"", " left the boss room", "{} left the boss room", false},
    // The chat's join and leave notices (lichii/hook/coop_hook.cpp): "<name> joined" / "<name> left".
    // They had no shape, so the whole line missed the table and stayed English ("Saria left").
    // After " left the boss room": longest end first.
    {"", " joined", "{} joined", false},
    {"", " left", "{} left", false},
    // The chat's own notices with a number or a note (lichii/hook/coop_hook.cpp:
    // "joined as player 2", "server <refusal>"). They are counted lines, not
    // names, so they come after the name forms.
    {"joined as player ", "", "joined as player {}", false},
    {"reconnected as player ", "", "reconnected as player {}", false},
    // The note after "server " is the refusal line (it starts "refused: "), and
    // that has its own shape: the middle here is not a word of the table.
    {"server ", "", "server {}", false},
    {"Getting ", "'s save...", "Getting {}'s save...", false},
    {"Loading ", "'s save...", "Loading {}'s save...", false},
    {"Multiplayer: ", "", "Multiplayer: {}", true},
    // The host's save at the title (the AutoLoad lines of before 0.11; SaveWait::Line in coop_title_menu.h now).
    {"", "'s save is not here yet (it still loads by itself if it comes): Continue plays your own save, New Game "
         "starts a new one.",
     "{}'s save is not here yet (it still loads by itself if it comes): Continue plays your own save, New Game "
     "starts a new one.", false},
    {"", "'s game ended before the save came: Continue plays your own save, New Game starts a new one.",
     "{}'s game ended before the save came: Continue plays your own save, New Game starts a new one.", false},
    {"Getting ", "'s save: it loads by itself when it is here...",
     "Getting {}'s save: it loads by itself when it is here...", false},
    // (branch join-continue's lines, 0.11)
    {"", "'s save is not here yet (the chat says Success when it is): Continue plays your own save, New Game "
         "starts a new one.",
     "{}'s save is not here yet (the chat says Success when it is): Continue plays your own save, New Game "
     "starts a new one.", false},
    {"Getting ", "'s save: don't load a save yet...", "Getting {}'s save: don't load a save yet...", false},
    {"", "'s save is in: load any save.", "{}'s save is in: load any save.", false},
    {"", " has no save yet: New Game starts one, Continue plays your own.",
     "{} has no save yet: New Game starts one, Continue plays your own.", false},
    {"Press Continue to play ", "'s save.", "Press Continue to play {}'s save.", false},
    {"Host: ", " (you)",
     "Host: {} (you)", false},
    {"Host: ", "",
     "Host: {}", false},
    {"Player ", " (you)",
     "Player {} (you)", false},
    {"", " (you)",
     "{} (you)", false},
    {"Players  ", "",
     "Players  {}", false},
    {"", " hearts",
     "{} hearts", false},
    {"", " ms",
     "{} ms", false},
    {"", " hearts",
     "{} / {} hearts", false},
    {"", " more players can join",
     "{} more players can join", false},
    {"", " more player can join",
     "{} more player can join", false},
    {"The host muted their ", " for everyone.",
     "The host muted their {} for everyone.", true},
    {"Teleporting to ", ": the chat says how it goes.",
     "Teleporting to {}: the chat says how it goes.", false},
    {"Kicking ", ".",
     "Kicking {}.", false},
    {"Banning ", ".",
     "Banning {}.", false},
    {"Player ", ". Press to see what you can do.",
     "Player {}. Press to see what you can do.", false},
    {"Player ", ". Press to see yourself.",
     "Player {}. Press to see yourself.", false},
    {"Restart needed: ", "",
     "Restart needed: {}", false},
    {"No mods in ", ". Put each mod in its own folder there.",
     "No mods in {}. Put each mod in its own folder there.", false},
    {"Randomizer: ", "",
     "Randomizer: {}", false},
    {"Co-op: ", "",
     "Co-op: {}", false},
    {"Player ", "",
     "Player {}", false},
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
    return Tr(Current(), en);
}


// ---- Ours, on top of the co-op release before this one: lines built from two or more pieces ----
std::string PlaceIn(const Lang* lang, std::string_view en);

struct SplitShape {
    const char* pieces[8];
    int words;    // bit i: part i is a word of the table too (a preset's name, an age)
    int count;    // pieces used
    const char* key;
};
bool SplitAll(std::string_view text, const SplitShape& shape, std::string_view* parts) {
    if (shape.count < 2 || shape.count > 8) {
        return false;
    }
    const std::string_view first = shape.pieces[0];
    const std::string_view last = shape.pieces[shape.count - 1];
    if (text.size() < first.size() + last.size()) {
        return false;
    }
    if (!first.empty() && text.substr(0, first.size()) != first) {
        return false;
    }
    if (!last.empty() && text.substr(text.size() - last.size()) != last) {
        return false;
    }
    std::size_t at = first.size();
    const std::size_t end = text.size() - last.size();
    for (int i = 1; i < shape.count - 1; ++i) {
        const std::string_view piece = shape.pieces[i];
        const std::size_t found = text.find(piece, at);
        if (found == std::string_view::npos || found >= end) {
            return false;
        }
        parts[i - 1] = text.substr(at, found - at);
        at = found + piece.size();
    }
    parts[shape.count - 2] = text.substr(at, end - at);
    return true;
}

std::string FillAll(const char* text, const std::string_view* parts, int count);

bool CountsLine(const Lang* lang, std::string_view text, std::string& out) {
    static constexpr const char* kWords[] = {"items", "songs", "keys", "maps and compasses",
                                             "heart containers", "tokens", "other things"};
    std::string result;
    std::size_t at = 0;
    bool any = false;
    while (at < text.size()) {
        std::size_t comma = text.find(", ", at);
        if (comma == std::string_view::npos) {
            comma = text.size();
        }
        const std::string_view piece = text.substr(at, comma - at);
        const std::size_t space = piece.find(' ');
        if (space == std::string_view::npos || space == 0) {
            return false;
        }
        for (char c : piece.substr(0, space)) {
            if (c < '0' || c > '9') {
                return false;
            }
        }
        const std::string_view word = piece.substr(space + 1);
        bool known = false;
        for (const char* w : kWords) {
            if (word == w) {
                known = true;
                break;
            }
        }
        if (!known) {
            return false;
        }
        std::string key = "{} ";
        key += std::string(word);
        const Entry* entry = Find(*lang, key);
        if (entry == nullptr) {
            return false;
        }
        const std::string counted(piece.substr(0, space));
        result += any ? Tr(lang, ", ") : std::string();
        result += Fill(entry->text, counted);
        any = true;
        at = comma == text.size() ? text.size() : comma + 2;
    }
    if (!any) {
        return false;
    }
    out = std::move(result);
    return true;
}

bool NoticeLine(const Lang* lang, std::string_view text, std::string& out) {
    const auto find = [&](std::string_view key) { return Find(*lang, key); };

    // "Your friend's game turned this one away: <reason>."
    {
        const std::string_view before = "Your friend's game turned this one away: ";
        const std::string_view after = ".";
        if (text.size() > before.size() + after.size() && text.substr(0, before.size()) == before &&
            text.substr(text.size() - after.size()) == after) {
            const std::string reason(text.substr(before.size(), text.size() - before.size() - after.size()));
            if (const Entry* e = find("Your friend's game turned this one away: {}")) {
                std::string said = reason;
                if (const Entry* r = Find(*lang, reason)) {
                    said = r->text;
                }
                out = Fill(e->text, said);
                return true;
            }
        }
    }

    // "<name> got the ..." / "<name> found a Small Key ..." / "<name> learned ..." etc.
    const std::string_view tail = [&]() -> std::string_view {
        // The heart container's own end too (notify.cpp: the "got a Heart
        // Container" notice carries it): without it the whole end stayed in the
        // middle, and the line matched neither form.
        for (const char* t : {" - your max health went up", " - you got one too!", " - you got them too!"}) {
            if (text.size() > std::string_view(t).size() && text.substr(text.size() - std::string_view(t).size()) == t) {
                return t;
            }
        }
        return {};
    }();
    const std::size_t cut = tail.empty() ? text.size() : text.size() - tail.size();
    const std::string_view head = text.substr(0, cut);

    struct Tail {
        const char* after;
        const char* key;
        int words;   // 1: the single data after it is a word (a place)
    };
    // The map, compass, Boss Key and token lines come first: each has its own
    // "got" form with a place, and a bare " got the " ahead of them took them
    // ("Saria got the map (Deku Tree)" read as "Saria 拿到了 map (Deku Tree)").
    // Their place (words = 1) is looked up in the table too. The token line is
    // "got a Gold Skulltula token (12)" and the table's key adds no bracket of
    // its own (it had "({})", which made the line "(12))").
    static constexpr Tail kTails[] = {
        {" got a Gold Skulltula token (", "{} got a Gold Skulltula token ({})", 1},
        {" got the Boss Key (", "{} got the Boss Key ({})", 1},
        {" got the compass (", "{} got the compass ({})", 1},
        {" got the map (", "{} got the map ({})", 1},
        {" found a Small Key (", "{} found a Small Key ({})", 1},
        {" got a Heart Container", "{} got a Heart Container", 0},
        {" got the ", "{} got the {}", 1},
        {" learned ", "{} learned {}", 1},
    };
    // "<name> found a Small Key (<place>) - the group has <n>": three data
    {
        const std::string_view a = " found a Small Key (";
        const std::string_view b = ") - the group has ";
        const std::size_t i = head.find(a);
        const std::size_t j = i == std::string_view::npos ? i : head.find(b, i + a.size());
        if (i != std::string_view::npos && i > 0 && j != std::string_view::npos) {
            const std::string who(head.substr(0, i));
            const std::string place(head.substr(i + a.size(), j - i - a.size()));
            const std::string count(head.substr(j + b.size()));
            if (const Entry* e = find("{} found a Small Key ({}) - the group has {}")) {
                std::string said = place;
                if (const Entry* p2 = Find(*lang, place)) {
                    said = p2->text;
                }
                const std::string_view parts_in[3] = {who, said, count};
                out = FillAll(e->text, parts_in, 3);
                return true;
            }
        }
    }
    // "<name> loaded a save - the group follows it (<n> changed)": two data
    {
        const std::string_view a = " loaded a save - the group follows it (";
        const std::string_view b = " changed)";
        const std::size_t i = head.find(a);
        if (i != std::string_view::npos && i > 0 && head.size() >= b.size() &&
            head.substr(head.size() - b.size()) == b) {
            const std::string who(head.substr(0, i));
            const std::string count(head.substr(i + a.size(), head.size() - i - a.size() - b.size()));
            if (const Entry* e = find("{} loaded a save - the group follows it ({} changed)")) {
                std::string said = count;
                if (std::string counts; CountsLine(lang, count, counts)) {
                    said = counts;   // "3 items" in the table's words
                }
                const std::string_view parts_in[2] = {who, said};
                out = FillAll(e->text, parts_in, 2);
                return true;
            }
        }
    }
    for (const Tail& t : kTails) {
        const std::size_t at = head.find(t.after);
        if (at == std::string_view::npos || at == 0) {
            continue;
        }
        const std::string who(head.substr(0, at));
        std::string rest(head.substr(at + std::string_view(t.after).size()));
        const bool paren = !rest.empty() && rest.back() == ')';
        if (paren) {
            rest.pop_back();
        }
        std::string said = rest;
        if (t.words != 0) {
            if (const Entry* e = Find(*lang, rest)) {
                said = e->text;
            } else if (const std::string place = PlaceIn(lang, rest); place != rest) {
                said = place;
            }
        }
        if (const Entry* e = find(t.key); e != nullptr) {
            // two data: the name, then the rest; the line's own tables carry the
            // sentence's end ("- you got one too!"), so the key has both forms.
            const std::string_view parts_in[2] = {who, said};
            std::string filled = FillAll(e->text, parts_in, 2);
            // The key's own line already has the parens when it wants them
            // ("{} got the map ({})"): a second one made "(12))". Only a key
            // without them gets the closing one ("{} got the {}").
            if (paren && std::string_view(t.key).find('(') == std::string_view::npos) {
                filled += "）";   // the dungeon's name is inside the line's parens
            }
            if (!tail.empty()) {
                const std::string key = std::string(t.key) + std::string(tail);
                if (const Entry* with_tail = Find(*lang, key)) {
                    filled = FillAll(with_tail->text, parts_in, 2);
                } else {
                    filled += Tr(lang, tail);
                }
            }
            out = std::move(filled);
            return true;
        }
    }
    return false;
}

std::string FillAll(const char* text, const std::string_view* parts, int count) {
    std::string out;
    int used = 0;
    const std::string_view t(text);
    for (std::size_t i = 0; i < t.size();) {
        if (t[i] == '{' && i + 1 < t.size() && t[i + 1] == '}') {
            if (used < count) {
                out += parts[used++];
            }
            i += 2;
        } else {
            out += t[i];
            ++i;
        }
    }
    return out;
}

constexpr SplitShape kSplitShapes[] = {
    // the host's difficulty notice (coop_difficulty.cpp Describe): numbers, plus the preset or the player count
    {{"Difficulty: Automatic (", " players): life ", "%, damage ", "%, enemies ", "%; bosses: life ", "%, damage ", "%"}, 0, 7, "Difficulty: Automatic ({} players): life {}%, damage {}%, enemies {}%; bosses: life {}%, damage {}%"},
    {{"Difficulty: Automatic (", " player): life ", "%, damage ", "%, enemies ", "%; bosses: life ", "%, damage ", "%"}, 0, 7, "Difficulty: Automatic ({} player): life {}%, damage {}%, enemies {}%; bosses: life {}%, damage {}%"},
    {{"Difficulty: ", ": life ", "%, damage ", "%, enemies ", "%"}, 1, 5, "Difficulty: {}: life {}%, damage {}%, enemies {}%"},
    // /teleport's lines (coop_teleport.cpp)
    {{"teleport: going to ", " in ", ""}, 0, 3, "teleport: going to {} in {}"},
    {{"teleport: ", " has gone on to ", "; you are where they were. Type /teleport again to follow", ""}, 0, 3, "teleport: {} has gone on to {}; you are where they were. Type /teleport again to follow"},
    {{"teleport: ", " is in ", ", a place for ", " Link; yours is ", "", ""}, 12, 5, "teleport: {} is in {}, a place for {} Link; yours is {}"},
    {{"teleport: ", " is ", " Link and you are ", ": no teleport to another age's Link", ""}, 6, 4, "teleport: {} is {} Link and you are {}: no teleport to another age's Link"},
    {{"teleport: ", " is busy now; you are where they were", ""}, 0, 2, "teleport: {} is busy now; you are where they were"},
    {{"teleport: no answer from ", "; you are where they were", ""}, 0, 2, "teleport: no answer from {}; you are where they were"},
    {{"teleport: no answer from ", " (an older build, or their game is busy)", ""}, 0, 2, "teleport: no answer from {} (an older build, or their game is busy)"},
    {{"teleport: ", " is in a dungeon: you may need its items to get on from there", ""}, 0, 2, "teleport: {} is in a dungeon: you may need its items to get on from there"},
    {{"teleport: ", " is in ", ", a place /teleport cannot load (no start point known there)", ""}, 0, 3, "teleport: {} is in {}, a place /teleport cannot load (no start point known there)"},
    {{"teleport: stopped: you left ", ""}, 0, 2, "teleport: stopped: you left {}"},
    {{"teleport: stopped: ", " left", ""}, 0, 2, "teleport: stopped: {} left"},
    {{"teleport: ", " is coming to you", ""}, 0, 2, "teleport: {} is coming to you"},
    {{"teleport: asking ", " where they are", ""}, 0, 2, "teleport: asking {} where they are"},
    {{"teleport: next to ", ""}, 0, 2, "teleport: next to {}"},
    // the lobby's and the mods' notes
    {{"", ": not connected", ""}, 0, 2, "{}: not connected"},
    {{"", ": only the host can do that", ""}, 0, 2, "{}: only the host can do that"},
    {{"", ": could not be sent", ""}, 0, 2, "{}: could not be sent"},
    // the reason is a line of the table too (coop/net/src/client.cpp's full sentences)
    {{"refused: ", ""}, 1, 2, "refused: {}"},
    {{"Mods: yours differ from the host's (", "): see the Mods tab", ""}, 0, 2, "Mods: yours differ from the host's ({}): see the Mods tab"},
    {{"Mods: player ", " (", ") has ", " mods unlike yours", ""}, 0, 4, "Mods: player {} ({}) has {} mods unlike yours"},
    {{"Mods: player ", " has ", " mods unlike yours", ""}, 0, 3, "Mods: player {} has {} mods unlike yours"},
    {{"Mods: player ", " has ", " mod unlike yours", ""}, 0, 3, "Mods: player {} has {} mod unlike yours"},
    {{"Mods: player ", " (", ") has ", " mod unlike yours", ""}, 0, 4, "Mods: player {} ({}) has {} mod unlike yours"},
    // what a pickup notice says (coop/host/src/notify.cpp): the player's name, then
    // the item's. These are read by NoticeLine (the tails above), not by SplitAll:
    // the item is a word of the table, and the end may be one of three.
};
std::string PlaceIn(const Lang* lang, std::string_view en) {
    if (lang == nullptr || lang->entries == nullptr) {
        return std::string(en);
    }
    const bool traditional = std::string_view(lang->code) == "zh-Hant";
    for (const PlaceName& p : kPlaces) {
        if (en == p.en) {
            return traditional ? p.hant : p.hans;
        }
    }
    return std::string(en);
}


// A line of a name and an end ("Saria left"): the bare ends are the shortest
// shapes there are, so a line whose own form starts before them is never
// theirs. The teleport's "<name> left" (coop_teleport.cpp: "teleport: stopped:
// <name> left") reads as one if these are tried first: it was translated as
// "传送：已停下：Saria 离开了" with the name as the middle.
bool NamedLine(const std::string_view text) {
    // Every notice the tab writes as "<name> ..." is a name the game's own
    // players have: a name never holds a colon, and every line that only looks
    // like one has its own fixed start.
    constexpr std::string_view kNotNames[] = {
        "teleport: ", "lobby: ", "Mods: ", "refused: ", "Difficulty: ", "Your friend's game turned this one away: ",
        "Co-op is on: ", "Hosting: ", "Join ", "Copied: ", "Getting ", "Loading ", "Multiplayer: ", "Press ",
        "Friends on your ", "No ", "server ", "and ", "room ", "pack ", "chat ", "game ", "the ",
    };
    for (const std::string_view p : kNotNames) {
        if (text.starts_with(p)) {
            return false;
        }
    }
    return true;
}

std::string Tr(const Lang* lang, std::string_view en) {
    if (lang == nullptr || lang->entries == nullptr) {
        return std::string(en);
    }
    if (const Entry* entry = Find(*lang, en)) {
        return entry->text;
    }
    // "server <note>": the note is itself a line of the tab (the refusal line,
    // "refused: <reason>", made by this same Tr), so it is translated again and
    // put after the table's "server". The result of that recursion is a line Tr
    // made, not a key: it is not looked up a third time.
    if (en.size() > 7 && en.substr(0, 7) == "server ") {
        const std::string note = Tr(lang, en.substr(7));
        if (const Entry* e = Find(*lang, "server {}")) {
            return std::string(e->text).replace(std::string(e->text).find("{}"), 2, note);
        }
    }
    // A line built around a number, a name or a code: the table's shape for it
    // fills in what the tab put there. Its middle, when it is a word of the
    // table too ("invite code", "hosting"), is translated as well.
    for (const Shape& shape : kShapes) {
        std::string_view middle;
        if (!Split(en, shape, &middle)) {
            continue;
        }
        // "<name> joined" / "<name> left" (a bare end): only for a line that is
        // a name and that end, never for a longer line that happens to end so
        // (the teleport's "stopped: <name> left").
        if (shape.before[0] == '\0' && (std::string_view(shape.after) == " joined" ||
                                        std::string_view(shape.after) == " left") &&
            !NamedLine(en)) {
            continue;
        }
        std::string_view text = middle;
        std::string kept;
        if (shape.words) {
            if (const Entry* word = Find(*lang, middle)) {
                text = word->text;
            }
        } else if (kept = PlaceIn(lang, middle); kept != middle) {
            text = kept;   // a scene the game names ("Hyrule Field")
        }
        if (const Entry* entry = Find(*lang, shape.key)) {
            return Fill(entry->text, text);
        }
        // (the table knows the ends but not the whole line: the ends in that
        // language, the middle as it is)
        return Tr(lang, shape.before) + std::string(text) + Tr(lang, shape.after);
    }
    if (std::string notice; NoticeLine(lang, en, notice)) {
        return notice;
    }
    for (const SplitShape& shape : kSplitShapes) {
        std::string_view parts[7];
        if (!SplitAll(en, shape, parts)) {
            continue;
        }
        std::string filled[7];
        std::string_view views[7];
        for (int i = 0; i < shape.count - 1; ++i) {
            filled[i] = std::string(parts[i]);
            if ((shape.words & (1 << i)) != 0) {
                filled[i] = Tr(lang, parts[i]);
            } else if (std::string counts; CountsLine(lang, parts[i], counts)) {
                filled[i] = counts;   // "2 items, 1 keys": each count in its own words
            } else if (const std::string place = PlaceIn(lang, parts[i]); place != parts[i]) {
                filled[i] = place;   // a scene the game names ("Hyrule Field")
            }
            views[i] = filled[i];
        }
        if (const Entry* entry = Find(*lang, shape.key)) {
            return FillAll(entry->text, views, shape.count - 1);
        }
        return std::string(en);
    }
    // A place's name the game wrote (places.json): its own table (see below), so a
    // chat line or a label that carries one reads in the same language as the HUD.
    if (const std::string place = PlaceIn(lang, en); place != en) {
        return place;
    }
    return std::string(en);
}


// A place's name in the table's language (places.json's English); the name as
// it is when the table has no line for it, and for English.
std::string Place(std::string_view en) {
    const Lang* lang = Current();
    if (lang == nullptr || lang->entries == nullptr) {
        return std::string(en);
    }
    const bool traditional = std::string_view(lang->code) == "zh-Hant";
    for (const PlaceName& p : kPlaces) {
        if (en == p.en) {
            return traditional ? p.hant : p.hans;
        }
    }
    return std::string(en);
}

}  // namespace CoopI18n