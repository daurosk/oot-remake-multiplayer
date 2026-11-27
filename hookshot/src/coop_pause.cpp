// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_pause.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/logging.h"
#include "coop/protocol/messages.hpp"
#include "coop_auto_rejoin.h"
#include "coop_char_colours.h"
#include "coop_colors.h"
#include "coop_dbno_game.h"
#include "coop_difficulty.h"
#include "coop_gold_mode.h"
#include "coop_race_mode.h"
#include "coop_switch_stand_mode.h"
#include "coop_hook.h"
#include "coop_hookshot.h"
#include "coop_host_menu.h"
#include "coop_i18n.h"
#include "coop_input.h"
#include "coop_lan.h"
#include "coop_lan_net.h"
#include "coop_lobby_game.h"
#include "coop_lobby_tab.h"
#include "coop_local_start.h"
#include "coop_menu_handoff.h"
#include "coop_menu_tabs_rule.h"
#include "coop_look.h"
#include "coop_look_view.h"
#include "coop_mods_tab.h"
#include "coop_original.h"
#include "coop_pause_files.h"
#include "coop_pause_tab.h"
#include "coop_pvp_game.h"
#include "coop_room_list.h"
#include "coop_runtime.h"
#include "coop_char_ages.h"
#include "coop_charsel_names.h"
#include "coop_ngplus.h"
#include "coop_puppet_config.h"
#include "coop_save.h"
#include <cstdlib>
#include <filesystem>
#include "coop_title.h"
#include "coop_title_menu.h"
#include "coop_voice.h"
#include "hos_coop_host.h"
#include "nat_codes.hpp"
#include "nat_net.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include "coop_clipboard_posix.h"
#endif

namespace CoopPause {

namespace {

using CoopFiles::ClipboardText;
using CoopFiles::CoopIni;
using CoopFiles::FindKit;
using CoopFiles::ReadOptions;
using CoopFiles::ReadText;
using CoopFiles::SetClipboardText;
using CoopFiles::WriteText;

// A paste that found no clipboard to read (Request::Kind::kPaste): what is missing and what still works.
#ifdef _WIN32
constexpr const char* kClipboardUnread = "The clipboard could not be read (another program had it open): press this again.";
#else
constexpr const char* kClipboardUnread =
    "The clipboard could not be read: no clipboard tool. Press Ctrl+V while typing, install wl-clipboard (Wayland) "
    "or xclip (X11), or type the code.";
#endif
// Copy LAN / Copy VPN before this PC's networks were first read (CoopFiles::KeptLocalCards, off the frame).
constexpr const char* kNetworksReading = "Reading this PC's networks: press this again in a moment.";
using CoopPauseMenu::Input;
using CoopPauseMenu::Request;
using State = CoopPauseMenu::Session::State;

constexpr int kMultiplayerTab = 0;   // the strip's first; a Mods tab can come after it

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::mutex g_mutex;   // everything below
CoopPauseMenu::Menu g_menu;
bool g_coop_on = false;      // co-op came with the game's start (coop.ini named a server): not started or stopped here
bool g_shown = false;        // the tab shows in Hookshot's menu
std::atomic<bool> g_shown_any{false};   // g_shown, for the present's and window's threads (TabShown: no co-op lock)
bool g_native = false;       // the title's own rows show its pages instead (OpenNative): no overlay
std::string g_doing;         // what the tab started: "hosting", "joined", "" none
bool g_hosted_public = false;  // the game hosting now was started as a public game (the Host page's Game row)
// Auto-rejoin (coop_auto_rejoin.h): the planner, and what the player did since its last update (its own Host or Join,
// a Leave), which stop what it planned; g_auto_join: the join being made is the planner's, not the player's.
CoopAutoRejoin::Planner g_rejoin;
bool g_player_acted = false, g_player_left = false, g_auto_join = false;
std::uint64_t g_last_game_ms = 0;   // when coop.ini's last_invite and the setting were last read
bool g_last_game = false;           // a last game is kept (coop.ini last_invite)
// Row 281: a restart to play the host's seed is due (RejoinAfterRestart, any thread; coop.ini rejoin_once follows it at
// the next update); this start follows one (read once at the first update, then cleared).
std::atomic<bool> g_rejoin_due{false};
CoopAutoRejoin::RestartMark g_rejoin_mark;
bool g_rejoin_once_read = false, g_rejoin_once = false;
// The setting as the files have it (ReadOptions, with last_invite): g_menu's own settings are read only when co-op's
// menu opens, so at the game's start they are the defaults (a run: the switch saved them over coop.ini's name).
bool g_auto_rejoin_on = false;
bool g_copied = false;       // this session's invite code was put on the clipboard
bool g_answer_copied = false;   // this join's answer code (WAIT_HOST) was put on the clipboard
// Linux: the automatic copy is tried once per code (CopyThen, off the game's thread). A desktop where the tool
// fails (Steam Deck Game Mode) fails again, and each try forks the whole game.
bool g_copy_tried = false;
bool g_answer_copy_tried = false;
std::uint64_t g_session = 0;   // one more for each Host or Join: a clipboard result of an earlier one is dropped
std::string g_answer_note;   // what became of the friend's answer code the host pasted ("" when none)
// A bare name joined as a PC on the home network (CoopTitleMenu::BareJoinName) with a relay set: its room line, joined
// next when no game there answered within kPcTryMs (a router or ISP that answers any name). Empty: none.
std::string g_room_fallback;
std::uint64_t g_room_fallback_ms = 0;
constexpr std::uint64_t kPcTryMs = 6000;
std::string g_join_text;     // the code (or address) this join used: remembered once connected
bool g_join_saved = false;
std::vector<std::string> g_lan_lines;   // "Same network"'s or "VPN"'s lines while they are shown
std::string g_lan_password;             // the join password those lines hold (a new one: they are old)
std::uint64_t g_next_session_ms = 0;
std::atomic<int> g_wanted{HOS_COOP_MENU_WANT_NOTHING};

// Row 236 (and row 279's other half): g_mutex guards this file's state only. The game's own threads take it (the
// - menu's FollowMenu: CloseNative, NativeBackLeaves, NativeView, NativeInput, Key...; the title's rows), so Tick
// never holds it across network, file or device work: what it needs from co-op's other parts (the host's status, the
// session, the player list, the rules, coop.ini) is read before it takes g_mutex (Gather) and read from there
// (Status, Lobby, Players...). A Host, Join or Leave made meanwhile reads live again (Fresh).
struct Outside {
    HosCoopStatus st{};
    CoopLobby::Seen seen;
    decltype(CoopHook::PlayerList()) players;
    std::vector<std::string> asking;
    CoopDifficulty::State difficulty;
    CoopDifficulty::Choice next_lobby;   // (the next lobby's: its first read is a file)
    std::uint16_t character = 0;
    std::optional<std::string> ini;                  // coop.ini (every 5 s: Follow's launcher port and last game)
    std::optional<CoopTitleMenu::Options> options;   // the files' settings, with ini
    CoopVoice::MicStatus mic = CoopVoice::MicStatus::unknown;   // (row 229) the voice's microphone, its own lock
};
Outside Gather(bool files) {
    Outside o;
    o.st.size = sizeof o.st;
    CoopHookshot::HostMenu().Status(&o.st);   // (from any thread: coop_host_menu.h)
    o.seen = CoopLobby::Now();
    o.players = CoopHook::PlayerList();
    o.asking = CoopLobby::Asking();
    o.difficulty = CoopDifficulty::Get();
    o.next_lobby = CoopDifficulty::NextLobby();
    o.character = CoopCharColours::ShownCharacter();
    o.mic = CoopVoice::Status();
    if (files) {
        o.ini = ReadText(CoopIni());
        o.options = ReadOptions();
    }
    return o;
}
const Outside* g_outside = nullptr;   // (under g_mutex, set by Tick only: its Gather; null: read live)
void Fresh() { g_outside = nullptr; }   // a Host, Join or Leave: what was gathered is old
CoopLobby::Seen Lobby() { return g_outside != nullptr ? g_outside->seen : CoopLobby::Now(); }
decltype(CoopHook::PlayerList()) Players() { return g_outside != nullptr ? g_outside->players : CoopHook::PlayerList(); }
std::vector<std::string> Asking() { return g_outside != nullptr ? g_outside->asking : CoopLobby::Asking(); }
CoopDifficulty::State Difficulty() { return g_outside != nullptr ? g_outside->difficulty : CoopDifficulty::Get(); }
CoopDifficulty::Choice NextLobby() { return g_outside != nullptr ? g_outside->next_lobby : CoopDifficulty::NextLobby(); }
std::uint16_t Character() { return g_outside != nullptr ? g_outside->character : CoopCharColours::ShownCharacter(); }

// Co-op that came with the game's start (g_coop_on: coop.ini's server line or LICHII_COOP_SERVER, as Nucleus's
// handler, an older kit's Host/Join .bat or a test kit write them) holds the Multiplayer page only while it is in a
// game: the server took this game in (WELCOME). A line that names no server running (a Nucleus instance folder or a
// copied game started by hand, row 170: "The menu items for mp in game don't work at all") never gets there, and
// the page's Host, Join and Find a game are the player's again.
bool LauncherLive() {
    return g_coop_on && Lobby().connected;
}

// The player ends co-op that came with the game's start: Leave while it is in a game, or their own Host or Join while
// it is not. The client goes as the menu's own does (CoopHook::End; CoopHook::Begin may run again), and coop.ini's
// server and join password lines are commented out, so the next start of this game by hand is solo (a launcher
// writes them again at its own start).
void EndLauncher(const char* why) {
    if (!g_coop_on) return;
    g_coop_on = false;
    CoopHook::End();
    CoopLobby::Forget();   // (the Lobby tab shows nobody once co-op is left, as the menu's own Leave)
    const std::string before = ReadText(CoopIni());
    const std::string after = CoopTitleMenu::IniUnset(CoopTitleMenu::IniUnset(before, "server"), "password");
    const bool kept = after == before || WriteText(CoopIni(), after);
    LOG_INFO(Core, "coop: multiplayer tab: co-op that came with the game's start ended: {}{}", why,
             after == before ? "" : kept ? " (coop.ini's server line commented out)" : " (coop.ini could not be written)");
}
// What a page reads from the files and devices as it opens (ShowTab, Open): read before g_mutex too.
struct Opening {
    CoopTitleMenu::Options options;
    bool saves = false;    // CoopSave::MultiplayerSaves
    std::string coop;      // coop.ini (the Join page's last game)
    bool input = false;    // CoopInput::Available, and its rows
    std::string controller, keyboard;
    std::optional<std::vector<std::string>> microphones;   // (only while voice is on: none opened)
    std::string characters;   // Character Select's list and the files this PC has (FeedCharacterAges)
    decltype(CoopPuppetConfig::KnownCharacterFiles()) known;
};
Opening GatherOpening() {
    Opening o;
    o.options = ReadOptions();
    o.saves = CoopSave::MultiplayerSaves();
    o.coop = ReadText(CoopIni());
    o.input = CoopInput::Available();
    if (o.input) o.controller = CoopInput::ControllerRow(), o.keyboard = CoopInput::KeyboardRow();
    if (o.options.voice) o.microphones = CoopVoice::Microphones();
    o.characters = CoopPuppetConfig::CharacterListText();
    o.known = CoopPuppetConfig::KnownCharacterFiles();
    return o;
}
const Opening* g_opening = nullptr;   // (under g_mutex, set by MenuStep for a SHOW: ShowTab reads it; null: live)

// The LAN line Follow made (the hosting game is there), said by FollowStep after g_mutex: it sends on the network.
std::optional<CoopLan::Game> g_announce;

// g_mutex, timed (row 236's proof): a wait or a hold over kSlowMs is logged with the function's name, again only when
// it is twice the longest logged for that name (a few lines per run, never one per frame).
constexpr std::uint64_t kSlowMs = 20;
class MenuLock {
public:
    explicit MenuLock(const char* what) : what_(what) {
        const auto t0 = std::chrono::steady_clock::now();
        lock_ = std::unique_lock<std::mutex>(g_mutex);
        from_ = std::chrono::steady_clock::now();
        Slow("waited", Ms(from_ - t0));
    }
    ~MenuLock() { Slow("held", Ms(std::chrono::steady_clock::now() - from_)); }
    MenuLock(const MenuLock&) = delete;
    MenuLock& operator=(const MenuLock&) = delete;

private:
    static std::uint64_t Ms(std::chrono::steady_clock::duration d) {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
    }
    void Slow(const char* how, std::uint64_t ms) {   // (g_mutex held)
        if (ms <= kSlowMs) return;
        static std::map<std::string, std::uint64_t> logged;
        if (!CoopMenuHandoff::SlowToLog(logged[std::string(how) + " " + what_], ms, kSlowMs)) return;
        LOG_WARNING(Core, "coop: multiplayer tab: the menu's lock {} {} ms in {}", how, ms, what_);
    }
    const char* what_;
    std::unique_lock<std::mutex> lock_;
    std::chrono::steady_clock::time_point from_;
};

HosCoopStatus Status() {
    if (g_outside != nullptr) return g_outside->st;
    HosCoopStatus st{};
    st.size = sizeof st;
    CoopHookshot::HostMenu().Status(&st);
    return st;
}

// Link's look (coop_look.h): CoopLook::SetPending when the player picks, ApplyOnUnpause when the menu closes; the
// look is worn on the game's next frame (CoopLook::Frame). These two are the only places it is wired.
std::vector<std::string> ChooseLook(const std::string& colour) {
    LOG_INFO(Core, "coop: multiplayer tab: Link's look: {}", colour.empty() ? std::string("by player number") : colour);
    CoopLook::Look look = CoopLook::Shown();
    look[CoopLook::Part::tunic] = CoopColors::Parse(colour);   // ("" or an unknown name: by player number)
    CoopLook::SetPending(look);
    return {"Link wears it when this menu closes."};
}
void WearChosenLook() {
    CoopLook::ApplyOnUnpause();
}
// The Customize page's look (coop_look_menu.h): every part at once.
std::vector<std::string> ChooseLookParts(const CoopLook::Look& look) {
    LOG_INFO(Core, "coop: multiplayer tab: customize: {} part colour(s) chosen", CoopLook::Wire(look).size());
    CoopLook::SetPending(look);
    return {"Link wears it when this menu closes."};
}


// What the module's host is doing, in plain words (CoopTitleMenu::PlainStatus). Only reads: what follows from
// the state (codes on the clipboard, the host's save loaded) is Tick's.
std::vector<std::string> StatusLines(const HosCoopStatus& st, const CoopLobby::Seen& seen) {
    CoopTitleMenu::HostView v;
    v.in_session = seen.connected;
    v.refused = seen.refused;
    v.banned = seen.banned;
    v.host = g_doing == "hosting";
    v.state = st.state;
    v.problem = st.problem;
    v.players = st.players;
    v.invite_done = st.invite_done != 0;
    v.invite = st.invite;
    v.answer = st.answer;
    v.copied = g_copied;
    if (v.host && v.state == HOS_COOP_HOSTING && !g_lan_lines.empty() && g_lan_password == st.password) {
        return g_lan_lines;
    }
    // (the room is the Host page's Room row: no line of its own here, which said it a second time)
    std::vector<std::string> lines = CoopTitleMenu::PlainStatus(v);
    if (!g_answer_note.empty() && v.problem != HOS_COOP_PROBLEM_BAD_CODE) {
        lines.push_back(v.players > 1 ? "Connected!" : g_answer_note);
    }
    return lines;
}

// A join that connected: remembered for "Join <host> again", and the host's save loaded at the title in one step
// (the menu closes there, so the game takes the presses). From Tick (the game's frame): writes coop.ini.
void Joined() {
    g_join_saved = true;
    std::string host = "your friend";
    for (const auto& row : CoopHook::PlayerList()) {
        if (row.number == 1 && !row.self && !row.name.empty()) {
            host = row.name;
        }
    }
    std::string coop = ReadText(CoopIni());
    coop = CoopTitleMenu::IniSet(coop, "last_invite", g_join_text, true);
    coop = CoopTitleMenu::IniSet(coop, "last_host", host, true);
    WriteText(CoopIni(), coop);
    if (CoopTitle::StartSaveWait(host)) {
        g_wanted = HOS_COOP_MENU_WANT_CLOSE;
        CoopTitle::LoadHostSaveOnTitle();   // (the host's save, once in, loads by itself: Continue pressed for them)
        LOG_INFO(Core, "coop: multiplayer tab: joined at the title: the host's save loads once it is in");
    } else if (!g_auto_join) {
        // (the player's own join with a file loaded: back to the title by itself, where the host's save loads. An
        // auto-rejoin after a drop keeps the game being played)
        LOG_INFO(Core, "coop: multiplayer tab: joined in the game: back to the title for the host's save");
        CoopPauseTab::ToTitle();
        CoopTitle::LoadHostSaveOnTitle();
    } else {
        LOG_INFO(Core, "coop: multiplayer tab: joined in the game: the host's save is used from the title on");
    }
}

// What co-op is doing, for the tab.
std::string ShareCode(const HosCoopStatus& st, bool copied);   // (below: the code a host gives its friends)

CoopPauseMenu::Session SessionNow() {
    const HosCoopStatus st = Status();
    const CoopLobby::Seen seen = Lobby();   // what the Lobby tab shows too
    CoopPauseMenu::Session s;
    switch (CoopTitleMenu::ShownState(st.state, g_doing == "hosting", seen.connected)) {
    case HOS_COOP_STARTING: s.state = State::kStarting; break;
    case HOS_COOP_HOSTING: s.state = State::kHosting; break;
    case HOS_COOP_JOINING: s.state = State::kJoining; break;
    case HOS_COOP_WAIT_HOST: s.state = State::kWaitHost; break;
    case HOS_COOP_CONNECTED: s.state = State::kJoined; break;
    case HOS_COOP_FAILED: s.state = State::kFailed; break;
    default: s.state = State::kOff; break;
    }
    s.hosting = g_doing == "hosting";
    s.public_game = s.hosting && g_hosted_public;
    s.by_launcher = LauncherLive();
    if (s.hosting) {
        s.players = st.players;
        s.code = ShareCode(st, false);   // (shown: never with the join password)
        s.code_done = st.invite_done != 0;
        // (the Host page's Join password row, shown on A, and its Let in and Refuse rows; the password is never logged)
        if (st.state == HOS_COOP_HOSTING) s.password.assign(st.password, strnlen(st.password, sizeof st.password));
        s.asking = Asking();
    } else {
        s.players = static_cast<int>(Players().size());
        s.code = st.state == HOS_COOP_WAIT_HOST ? st.answer : "";
    }
    s.lines = StatusLines(st, seen);
    // (the session's rules in force, the server's: Co-op settings' host rows show them, and change them for a host)
    if (seen.connected) {
        const CoopDbno::Rules rules = CoopDbno::Current();
        s.rules = true;
        s.pvp = CoopPvp::On();
        s.gold_own = CoopGoldMode::NotShared();
        s.race = CoopRaceMode::InForce();
        s.floor_switches = CoopSwitchStandMode::InForce();
        s.dbno = rules.on;
        s.dbno_bleed_s = rules.bleed_s;
        s.dbno_revive_tenths = rules.revive_tenths;
        s.dbno_revive_life = rules.revive_life;
    }
    return s;
}

}  // namespace

const std::string& ExefsList() {
    static const std::string text = [] {
        std::string t;
        std::error_code ec;
        const std::filesystem::path p = CoopRuntime::GameDir() / "exefs_patches.txt";
        if (std::filesystem::is_regular_file(p, ec)) {
            if (std::FILE* f = std::fopen(p.string().c_str(), "rb")) {
                char buf[4096];
                for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0 && t.size() < (1u << 20);) t.append(buf, n);
                std::fclose(f);
            }
        }
        return t;
    }();
    return text;
}

// The Local co-op page's Start (below, with the local co-op host).
void LocalAsked(int games);

namespace {

void RefreshSession(bool now) {
    if (now || NowMs() >= g_next_session_ms) {
        g_next_session_ms = NowMs() + 250;
        g_menu.SetSession(SessionNow());
        const CoopDifficulty::State d = Difficulty();
        g_menu.SetDifficulty({d.connected, d.can_edit, d.choice, d.values, NextLobby()},
                             static_cast<int>(Players().size()));
        g_menu.SetCharacter(Character() != 0);   // (#308's Customize page: Link's colours wait)
        // (one home for Link's look: Character Select's Customize screen when his code is in the game)
        g_menu.SetSnailHome(CoopPauseTab::CharselCodeBuiltIn(ExefsList()));
    }
}

// The clipboard without stopping the game. On Linux a copy or a paste runs wl-copy / wl-paste, xclip or xsel and
// waits for it (up to 2 s); done on the game's thread, that stopped the frames (hosting in Steam Deck Game Mode,
// where the tool fails, fell to a few frames a second). There it runs on the clipboard's own thread
// (CoopClipboard::CopyLater / PasteLater) and `done` runs on a later Tick (Delivered), under g_mutex; SDL's own
// clipboard (a paste on the window's thread) is read at once. Windows: at once, as before (OpenClipboard is quick).
using CopyDone = std::function<void(bool)>;
using PasteDone = std::function<void(std::optional<std::string>)>;
#ifndef _WIN32
std::vector<std::pair<std::uint64_t, CopyDone>> g_copying;
std::vector<std::pair<std::uint64_t, PasteDone>> g_pasting;
#endif

void CopyThen(const std::string& text, CopyDone done) {
#ifdef _WIN32
    done(SetClipboardText(text));
#else
    g_copying.emplace_back(CoopClipboard::CopyLater(text), std::move(done));
#endif
}

void PasteThen(PasteDone done) {
#ifdef _WIN32
    done(CoopFiles::ReadClipboard());
#else
    if (std::optional<std::string> now = CoopClipboard::PasteNow()) {
        done(std::move(now));
        return;
    }
    g_pasting.emplace_back(CoopClipboard::PasteLater(), std::move(done));
#endif
}

// The clipboard's finished jobs, handed to their `done` (each may start another job).
void Delivered() {
#ifndef _WIN32
    for (std::size_t i = 0; i < g_copying.size();) {
        if (const auto d = CoopClipboard::Take(g_copying[i].first)) {
            CopyDone done = std::move(g_copying[i].second);
            g_copying.erase(g_copying.begin() + static_cast<std::ptrdiff_t>(i));
            done(d->ok);
        } else {
            ++i;
        }
    }
    for (std::size_t i = 0; i < g_pasting.size();) {
        if (auto d = CoopClipboard::Take(g_pasting[i].first)) {
            PasteDone done = std::move(g_pasting[i].second);
            g_pasting.erase(g_pasting.begin() + static_cast<std::ptrdiff_t>(i));
            done(std::move(d->text));
        } else {
            ++i;
        }
    }
#endif
}

bool Busy();
bool Join(const std::string& text);
// A join the host turned away because the name is in use there (two PCs' Nucleus players both "Player1", a friend of
// the same name): joined again as "<name> (2)", (3)... for this session only, the saved name kept. g_mutex.
std::string g_name_suffix;
int g_name_tries = 0;
bool g_name_retry = false;   // (the Join now is that one: its suffix kept)

// The code a host gives its friends (CoopTitleMenu::HostShareText): its room on the relay ("<room>@<relay>"), else the
// relay's join code ("<CODE>@<relay>"), else the invite code; "" none yet. Join takes each of them. `copied` (the
// clipboard): the join password after a relay line, so a friend's paste joins (a line without it never did); shown on
// the Host page: bare (the password only when the player asks for it).
constexpr std::uint64_t kRelayWaitMs = 6000;
std::string ShareCode(const HosCoopStatus& st, bool copied) {
    return CoopTitleMenu::HostShareText(std::string_view(st.relay_room, strnlen(st.relay_room, sizeof st.relay_room)),
                                        std::string_view(st.relay_code, strnlen(st.relay_code, sizeof st.relay_code)),
                                        std::string_view(st.invite, strnlen(st.invite, sizeof st.invite)),
                                        copied ? std::string_view(st.password, strnlen(st.password, sizeof st.password))
                                               : std::string_view(),
                                        copied);
}

// What follows from co-op's state, whether the tab is shown or not: the code to give friends on the clipboard once
// there is one (ShareCode; a guest's answer code too), the message logged, a connected join remembered and the host's
// save loaded.
void Leave();                          // (below)
void SayOnPage(const char* otherwise);   // (below)

void Follow(const HosCoopStatus& st) {
    const bool host = g_doing == "hosting";
    if (st.state == HOS_COOP_HOSTING) {
        // fps-spike-0121: this PC's network cards read once, off the frame, for the Copy LAN / Copy VPN rows
        (void)CoopFiles::KeptLocalCards(NowMs(), true);
    }
    const std::uint64_t session = g_session;
    // (the relay's room or code first, which needs no router: it is asked at once with the built-in relay, and the
    // invite code is only taken alone when the relay has not answered in kRelayWaitMs or said why not)
    static std::uint64_t waited_session = 0, wait_from = 0;
    if (waited_session != session) waited_session = session, wait_from = NowMs();
    const bool relay_in = st.relay_room[0] != 0 || st.relay_code[0] != 0;
    const bool relay_out = st.problem == HOS_COOP_PROBLEM_RELAY_FAILED || NowMs() - wait_from > kRelayWaitMs;
    const std::string share = ShareCode(st, true);
    if (host && !share.empty() && (relay_in || relay_out) && !g_copied && !g_copy_tried) {
#ifndef _WIN32
        g_copy_tried = true;   // one try per code (Windows tries again each round: a busy clipboard frees up)
#endif
        CopyThen(share, [session, relay_in](bool ok) {
            if (session != g_session) {
                return;
            }
            g_copied = ok;
            LOG_INFO(Core, "coop: multiplayer tab: host: the {} is on the clipboard{}",
                     relay_in ? "relay's room or join code" : "invite code", ok ? "" : " (it could not be)");
        });
    }
    if (!host && st.state == HOS_COOP_WAIT_HOST && st.answer[0] != 0 && !g_answer_copied && !g_answer_copy_tried) {
#ifndef _WIN32
        g_answer_copy_tried = true;
#endif
        CopyThen(st.answer, [session](bool ok) {
            if (session == g_session) {
                g_answer_copied = ok;
            }
        });
    }
    if (st.message[0] != 0) {
        static std::string said;
        if (said != st.message) {
            said = st.message;
            LOG_INFO(Core, "coop: multiplayer tab: {}", said);   // (the English detail, for the log)
        }
    }
    // A bare name tried as a PC: no game answered there in time, so it is the room of that name on the relay.
    if (!g_room_fallback.empty()) {
        if (g_doing != "joined" || Lobby().connected) {
            g_room_fallback.clear();
        } else if (NowMs() - g_room_fallback_ms >= kPcTryMs) {
            const std::string line = std::exchange(g_room_fallback, std::string());
            const auto bare = CoopTitleMenu::BareJoinName(line.substr(0, line.find('@')));
            LOG_INFO(Core, "coop: multiplayer tab: join: no game answered at that PC: as a room on the relay");
            CoopHookshot::HostMenu().Leave();
            Fresh();
            g_doing.clear();
            g_menu.SetNote({"No game answered at a PC called " + bare.value_or("that") + ": trying the room " +
                            bare.value_or("") + " on your relay."});
            Join(line);
            return;
        }
    }
    // Once the session took this game in: the host's name is known then (PlayerList).
    if (g_doing == "joined" && st.state == HOS_COOP_CONNECTED && !g_join_saved && !g_join_text.empty() &&
        Lobby().connected) {
        Joined();
    }
    const std::uint64_t now = NowMs();
    // Games on this network (coop_lan.h): a hosting game says it is there (its name, port, players, room; never the
    // join password), every CoopLan::kEveryMs.
    // A game the launcher started on a server of this very PC (Nucleus Co-op's host PC, Host.bat: coop.ini's server
    // 127.0.0.1) says so too, once it is the session's host: without it Nucleus's "Join a game on this network" on
    // another PC found nothing (a tester's two PCs, 2026-11-20). Its port is coop.ini's.
    std::uint16_t launcher_port = 0;
    if (g_coop_on) {
        static std::uint16_t local_port = 0;
        if (g_outside != nullptr && g_outside->ini) {   // (coop.ini, read by Gather every 5 s)
            const std::string& ini = *g_outside->ini;
            std::string server(CoopTitleMenu::Trim(CoopTitleMenu::IniGet(ini, "server").value_or("")));
            std::uint16_t port = CoopTitleMenu::kDefaultPort;
            if (const auto colon = server.rfind(':'); colon != std::string::npos && server.find(':') == colon) {
                if (const auto p = CoopTitleMenu::ParsePort(server.substr(colon + 1))) port = *p;
                server.resize(colon);
            } else if (const auto p = CoopTitleMenu::ParsePort(CoopTitleMenu::IniGet(ini, "port").value_or(""))) {
                port = *p;
            }
            local_port = server == "127.0.0.1" || server == "localhost" || server == "::1" ? port : 0;
        }
        const CoopLobby::Seen seen = Lobby();
        if (local_port != 0 && seen.connected && seen.self != 0 && seen.self == seen.host) launcher_port = local_port;
    }
    // (a public game, in the game or Host.bat's (coop-options.ini public_game): the relay only, no LAN line)
    if ((host && st.state == HOS_COOP_HOSTING && !g_hosted_public) ||
        (launcher_port != 0 && !g_menu.options().public_game)) {
        const CoopTitleMenu::Options& o = g_menu.options();
        CoopLan::Game g;
        g.name = o.name;
        for (const auto& row : Players()) {   // (the name co-op plays this game under, once it is in)
            if (row.self && !row.name.empty()) g.name = row.name;
        }
        g.port = launcher_port != 0 ? launcher_port : o.host_port != 0 ? o.host_port : CoopTitleMenu::kDefaultPort;
        const CoopLobby::Seen seen = Lobby();
        g.players = seen.connected ? static_cast<int>(seen.players.size()) : 1;
        g.max = o.max_players;
        std::string room = st.relay_room;
        if (const auto at = room.find('@'); at != std::string::npos) room.resize(at);
        g.room = room;
        g.protocol = coop::protocol::kProtocolVersion;
        static const std::string id = CoopLan::NewId();   // (one for this game's run: one row however many networks)
        g.id = id;
        g_announce = std::move(g);   // (said after g_mutex: FollowStep)
    }
    // Auto-rejoin (coop_auto_rejoin.h): the last friend's game again, at the start and after a drop.
    if (g_outside != nullptr && g_outside->ini) {   // (coop.ini read at most every 5 s, by Gather)
        g_last_game_ms = now;
        const auto code = CoopTitleMenu::IniGet(*g_outside->ini, "last_invite");
        g_last_game = code && !code->empty();
        g_auto_rejoin_on = g_outside->options->auto_rejoin;
    }
    // (once, from what Gather read, with last_invite; cleared at once, as Joined writes: a later start does not join
    // again by itself)
    if (!g_rejoin_once_read && g_outside != nullptr && g_outside->ini) {
        g_rejoin_once_read = true;
        if (const auto once = CoopTitleMenu::IniGet(*g_outside->ini, "rejoin_once"); once && *once == "1") {
            g_rejoin_once = true;
            WriteText(CoopIni(), CoopTitleMenu::IniSet(ReadText(CoopIni()), "rejoin_once", "0", true));
            LOG_INFO(Core, "coop: multiplayer tab: auto-rejoin: this start follows the restart for the host's "
                           "randomizer seed: the last game is joined again once");
        }
    }
    CoopAutoRejoin::Facts f;
    f.enabled = g_auto_rejoin_on;
    f.once = g_rejoin_once;
    f.code = g_last_game;
    f.launcher = g_coop_on;
    f.idle = !Busy();
    f.guest_in = g_doing == "joined" && st.state == HOS_COOP_CONNECTED;
    f.player_left = std::exchange(g_player_left, false);
    f.player_acted = std::exchange(g_player_acted, false);
    {   // (a refusal counted since the last update: CoopLobby's count only grows, so none is missed)
        static std::uint32_t refusals_seen = 0;
        static std::uint32_t names_taken_seen = 0;
        const auto seen = Lobby();
        f.turned_away = seen.refusals != refusals_seen;
        refusals_seen = seen.refusals;
        const bool name_taken = seen.names_taken != names_taken_seen;
        names_taken_seen = seen.names_taken;
        // A join the host turned away (a wrong join password, a full game, a ban): co-op stops by itself and the Join
        // rows come back with why. Kept on, the title's Join page offered only Leave the game, a tester's "janky".
        // The name in use there (another PC's player of the same name: two Nucleus PCs' "Player1"), after the
        // client's own retries ran out: joined again as "<name> (2)", (3)... A game started by the launcher keeps its
        // name (its server and name are the launcher's).
        if (name_taken && g_doing == "joined" && !seen.connected && !g_coop_on && g_name_tries < 8 &&
            !g_join_text.empty()) {
            ++g_name_tries;
            g_name_suffix = " (" + std::to_string(g_name_tries + 1) + ")";
            LOG_INFO(Core, "coop: multiplayer tab: the name is in use in that game: joining again with \"{}\" after it "
                           "(this session only)",
                     g_name_suffix);
            const std::string again = g_join_text;
            CoopHookshot::HostMenu().Leave();   // (the refused client stopped; quietly: no "Co-op stopped" note)
            Fresh();
            g_doing.clear();
            g_name_retry = true;
            g_auto_join = true;   // (not the player's own join: auto-rejoin's plans stay)
            const bool joined = Join(again);
            g_auto_join = false;
            if (joined) {
                g_menu.SetNote({"Your name is in use in that game: joined with" + g_name_suffix + " after it."});
            }
            g_name_retry = false;
        } else if (f.turned_away && g_doing == "joined" && !seen.connected && !g_coop_on) {
            const std::string why = CoopTitleMenu::RefusedLine(seen.refused);
            LOG_INFO(Core, "coop: multiplayer tab: the join was turned away ({}): co-op stopped, the Join rows again",
                     CoopTitleMenu::ShortReason(seen.refused));
            Leave();
            g_menu.SetNote({"Turned away: " + CoopTitleMenu::ShortReason(seen.refused),
                            seen.banned ? "You cannot join that game again." : "Check the room and the join password, then Join again."});
            // (the pause page's Session view says it only for a join made there: one made on the title said it on the
            // title's Join page, and the page opened later in the game on that old line)
            if (!g_native || !CoopTitle::Up()) SayOnPage(why.c_str());
        }
    }
    if (const std::optional<bool> mark = g_rejoin_mark.Tick(g_rejoin_due.load(), f)) {
        WriteText(CoopIni(), CoopTitleMenu::IniSet(ReadText(CoopIni()), "rejoin_once", *mark ? "1" : "0", true));
        LOG_INFO(Core, "coop: multiplayer tab: auto-rejoin: {}",
                 *mark ? "the randomizer asked for a restart for the host's seed: the next start joins this game again "
                         "once"
                       : "no restart for the host's seed any more (or the player's own choice): the next start does "
                         "not join by itself");
    }
    if (g_rejoin.Tick(f, now)) {
        const auto code = CoopTitleMenu::IniGet(ReadText(CoopIni()), "last_invite");
        LOG_INFO(Core, "coop: multiplayer tab: auto-rejoin: the last game again ({})", g_rejoin.Why());
        if (code && !code->empty()) {
            g_menu.Show(ReadOptions());   // (the files' settings: the name, the relay; not the menu's defaults)
            g_auto_join = true;
            Join(*code);   // (refused or failed: the next try, if any, comes on its own)
            g_auto_join = false;
        }
    } else if (!g_rejoin.Why().empty()) {
        static std::string said;
        if (said != g_rejoin.Why() && g_rejoin.Why().rfind("gave up", 0) == 0) {
            said = g_rejoin.Why();
            LOG_INFO(Core, "coop: multiplayer tab: auto-rejoin: {}", said);
        }
    }
}

// Host and Join through the module's own host (native-host-b, hos_coop_host.h): the co-op server or the
// invite code's join helper on the module's "coop host" thread, and this game's client connected to it.
bool Busy() {
    const HosCoopStatus st = Status();
    return g_coop_on || (st.state != HOS_COOP_IDLE && st.state != HOS_COOP_FAILED);
}

void Save();

// False when it did not start: refused (co-op is on already; the menu's note says so) or the host could not start
// (the session's lines say why). `o`: the menu's options (the Host row), or the autotest's (TestHost: no router port,
// no invite).
bool Host(const CoopTitleMenu::Options& o, bool test) {
    Fresh();
    g_player_acted = true;
    if (!LauncherLive()) EndLauncher("the player hosts while it is not in a game");
    if (g_doing == "hosting" && !g_coop_on) {
        Save();
        CoopHookshot::HostMenu().Leave();
        g_doing.clear();
    } else if (Busy()) {
        g_menu.SetNote({"Co-op is on already in this game: stop it first, or start the game again."});
        return false;
    }
    CoopHookshot::HostMenu().Leave();   // (a failed start: clear it)
    CoopFiles::SetCoopEnvironment(o);
    CoopGoldMode::SetChoice(o.gold_own ? CoopGoldMode::Mode::own : CoopGoldMode::Mode::shared);   // (row 63)
    CoopRaceMode::SetChoice(o.race);
    CoopSwitchStandMode::SetChoice(o.floor_switches);
    HosCoopHostOptions h{};
    h.size = sizeof h;
    h.name = o.name.c_str();
    h.password = o.password.empty() ? nullptr : o.password.c_str();
    h.port = o.host_port;
    h.upnp = test ? 0 : 1;             // the router forward (nothing to set up)
    h.invite = test ? 0 : 1;
    h.pvp = o.pvp ? 1 : 0;
    // The lobby's difficulty, picked on the Host page (or Host settings) as it is made: the server starts with it, and
    // the game lists show it (CoopDifficulty::NextLobby: automatic when none was picked).
    const CoopDifficulty::Choice lobby = CoopDifficulty::NextLobby();
    h.difficulty = lobby.auto_scale ? 0 : static_cast<std::uint8_t>(static_cast<int>(lobby.preset) + 1);
    h.difficulty_life = lobby.custom.hp_pct;
    h.difficulty_damage = lobby.custom.damage_pct;
    h.difficulty_enemies = lobby.custom.count_pct;
    h.approve_joins = o.approve_joins ? 1 : 0;
    h.same_network_open = o.same_network_open ? 1 : 0;   // (on, the default: this network's players need no password)
    h.max_players = static_cast<std::uint8_t>(o.max_players);
    // coop-options.ini's relay lines, as Host.bat uses them (issue 261: a room name friends type in Join a game).
    h.relay_via = o.relay.empty() ? nullptr : o.relay.c_str();
    h.relay_key = o.relay_key.empty() ? nullptr : o.relay_key.c_str();
    h.relay_room = o.relay_room.empty() ? nullptr : o.relay_room.c_str();
    // (the room on the relay's public list only when they turned it on, list_room: shown as list_name, or as the
    // room name without one; never the player's name unless they typed it there)
    const std::string& shown = o.list_name.empty() ? o.relay_room : o.list_name;
    h.relay_list = o.list_room && !o.relay_room.empty() ? shown.c_str() : nullptr;
    // A public game (the Host page's Game row): anyone joins with no password, relay only, always listed.
    h.public_game = o.public_game ? 1u : 0u;
    // The host's kept bans (by player key), beside coop.ini: a player banned once stays out of every game hosted here.
    const std::string bans_file = CoopFiles::Utf8Path(CoopRuntime::GameDir() / "coop-bans.txt");
    h.bans_file = bans_file.c_str();
    if (o.public_game) h.relay_list = shown.c_str();
    g_hosted_public = o.public_game;
    g_doing = "hosting";
    ++g_session;
    g_copied = false;
    g_copy_tried = false;
    g_lan_lines.clear();
    g_answer_note.clear();
    if (!CoopHookshot::HostMenu().HostStart(&h)) {
        RefreshSession(true);
        return false;
    }
    LOG_INFO(Core, "coop: multiplayer tab: host: started (up to {} players, port {})", o.max_players, o.host_port);
    RefreshSession(true);
    return true;
}

bool Host() {
    return Host(g_menu.options(), false);
}

// False when it did not start: refused (co-op on already, the code not readable; the menu's note says why) or the
// join could not start (the session's lines say why).
// Whether this PC finds `name` as a PC (a name on the home network, or in DNS). The lookup blocks, as the plain
// address join's own lookup does (CoopHostMenu::Menu::Join).
bool PcFound(const std::string& name) {
    if (enet_initialize() != 0) {
        return true;   // (no network: the join itself says so)
    }
    const bool found = coop::server::nat::resolve(name, CoopTitleMenu::kDefaultPort).has_value();
    enet_deinitialize();
    return found;
}

bool Join(const std::string& text_in) {
    Fresh();
    if (!g_auto_join) g_player_acted = true;
    // (the player's own Join; auto-rejoin never ends co-op that came with the game's start: it may be connecting)
    if (!g_auto_join && !LauncherLive()) EndLauncher("the player joins while it is not in a game");
    if (Busy()) {
        g_menu.SetNote({"Co-op is on already in this game: stop it first, or start the game again."});
        return false;
    }
    namespace nat = coop::server::nat;
    // A bare name ("mypc", "forest-gang"): a PC on the home network first, then a room on the relay.
    std::string text = text_in;
    std::string fallback;   // (a bare name joined as a PC: its room, should no game answer there)
    const auto bare = CoopTitleMenu::BareJoinName(text);
    const std::string relay(CoopTitleMenu::Trim(g_menu.options().relay));
    if (bare && !nat::looks_like_code(text) && PcFound(*bare)) {
        if (!relay.empty()) fallback = CoopTitleMenu::RoomJoinLine(text, relay);
    } else if (bare && !nat::looks_like_code(text)) {
        if (relay.empty()) {
            g_menu.SetNote({"No PC called " + *bare + " was found. To join a room by its name, set a relay first: "
                            "Co-op Settings, Relay. Or type the host's IP address."});
            LOG_INFO(Core, "coop: multiplayer tab: join: a bare name is no PC here and no relay is set");
            return false;
        }
        text = CoopTitleMenu::RoomJoinLine(text, relay);
        LOG_INFO(Core, "coop: multiplayer tab: join: a bare name is no PC here: as a room on the relay");
    }
    CoopTitleMenu::CodeResult code;
    code.looks_like_code = nat::looks_like_code(text);
    if (code.looks_like_code) {
        const auto r = nat::read_invite(text);
        if (r.error == nat::CodeError::kOk) {
            code.invite = CoopTitleMenu::JoinInvite{nat::show(r.invite.host), r.invite.password};
        } else {
            code.error = nat::explain(r.error);
        }
    }
    const auto target = CoopTitleMenu::ReadJoin(text, code);
    if (const auto* err = std::get_if<std::string>(&target)) {
        g_menu.SetNote({*err});
        return false;
    }
    CoopHookshot::HostMenu().Leave();
    const CoopTitleMenu::Options o = g_menu.options();
    CoopFiles::SetCoopEnvironment(o);
    const bool invite = std::holds_alternative<CoopTitleMenu::JoinInvite>(target);
    const std::string where = invite ? text : std::get<CoopTitleMenu::JoinAddress>(target).server;
    // Only what the player gave (the invite code, or the password after the address): never this game's own
    // server_password, which is its host secret and not the other server's.
    const std::string password = invite ? std::string() : std::get<CoopTitleMenu::JoinAddress>(target).password;
    if (!std::exchange(g_name_retry, false)) g_name_suffix.clear(), g_name_tries = 0;   // (a join of the player's own)
    std::string name = o.name;
    if (!g_name_suffix.empty()) {   // (at most 16 bytes in all: the name cut before a UTF-8 letter's start)
        std::size_t n = (std::min)(name.size(), std::size_t{16} > g_name_suffix.size() ? 16 - g_name_suffix.size() : 0);
        while (n > 0 && n < name.size() && (static_cast<unsigned char>(name[n]) & 0xc0) == 0x80) --n;
        name = std::string(CoopTitleMenu::Trim(name.substr(0, n))) + g_name_suffix;
    }
    HosCoopJoinOptions j{};
    j.size = sizeof j;
    j.name = name.c_str();
    j.target = where.c_str();
    j.password = password.empty() ? nullptr : password.c_str();
    g_doing = "joined";
    g_join_text = text;
    g_room_fallback = fallback;
    g_room_fallback_ms = NowMs();
    g_join_saved = false;
    ++g_session;
    g_answer_copied = false;
    g_answer_copy_tried = false;
    if (!CoopHookshot::HostMenu().Join(&j)) {
        RefreshSession(true);
        return false;
    }
    LOG_INFO(Core, "coop: multiplayer tab: join: {}", invite ? std::string("an invite code") : "the address " + where);
    RefreshSession(true);
    return true;
}

void Leave() {
    Fresh();
    g_player_left = true;
    g_room_fallback.clear();
    LOG_INFO(Core, "coop: multiplayer tab: {}", g_doing == "hosting" ? "stop hosting" : "leave the friend's game");
    CoopHookshot::HostMenu().Leave();
    CoopTitle::StopSaveWait();
    g_doing.clear();
    g_lan_lines.clear();
    g_answer_note.clear();
    RefreshSession(true);
    g_menu.SetNote({"Co-op stopped. You play on alone."});
}

// Row 229: what the voice's microphone gives now, on the Voice rows (the Microphone row's words, the Proximity voice
// chat row's help). g_mutex. Only FollowStep's Gather reads CoopVoice::Status (off g_mutex, row 236); without a
// fresh Gather (a settings change) the rows say what the last one saw; the next one (250 ms) brings them up to date.
CoopVoice::MicStatus g_mic_seen = CoopVoice::MicStatus::unknown;   // (g_mutex: the last Gather's microphone)
void VoiceStatusRows() {
    if (g_outside != nullptr) g_mic_seen = g_outside->mic;
    const CoopVoice::MicStatus s = !g_menu.options().voice ? CoopVoice::MicStatus::unknown : g_mic_seen;
    g_menu.SetVoiceStatus(CoopVoice::MicStatusWords(s), CoopVoice::MicStatusNote(s));
}

// The Settings page's Microphone row (voice on): the microphones there now, listed only while voice is on (none
// opened); none when this game cannot choose one (an older patch 0009).
void MicRows() {
    g_menu.SetMicrophones(g_menu.options().voice ? CoopVoice::Microphones() : std::nullopt);
    VoiceStatusRows();
}

void Save() {
    // (the coop-options.ini the game reads at its start: the kit's, or the one beside longshot.exe in a game with no kit
    // of its own, Nucleus Co-op's instances: before, those saved nothing but coop.ini, a tester's "not saving")
    // (with this game's coop-player.ini, local co-op and Nucleus: its controller and look there, the rest in the kit's)
    const bool kit = CoopTitle::KitOptionsFile() || CoopTitle::PlayerOptionsFile();
    std::string coop = ReadText(CoopIni());
    std::string options = kit ? CoopTitle::ReadKitOptions() : std::string();
    g_menu.options().Write(coop, options);
    bool ok = WriteText(CoopIni(), coop);
    if (kit) {
        ok = CoopTitle::WriteKitOptions(options) && ok;
    }
    const bool live = g_coop_on || g_doing == "hosting" || g_doing == "joined";
    if (!live) {
        CoopFiles::SetCoopEnvironment(g_menu.options());   // co-op has not started: they count for this run
    }
    CoopVoice::SetMode(!g_menu.options().voice     ? coop::voice::Mode::off
                       : g_menu.options().voice_ptt ? coop::voice::Mode::push_to_talk
                                                    : coop::voice::Mode::open_mic);   // voice chat and its mode count at once
    CoopVoice::SetMicrophone(g_menu.options().voice_mic);   // and its microphone
    MicRows();   // (voice just turned on: the Microphone row's list)
    CoopHookshot::SetChatKey(g_menu.options().chat_key);   // the chat key too
    CoopHookshot::SetMapOptions(g_menu.options().map_players, g_menu.options().map_names);   // at once too
    g_auto_rejoin_on = g_menu.options().auto_rejoin;   // (the Auto-rejoin row: counts at once, as the files have it now)
    {   // Original game (coop_original.h): the mods list follows the switch now; the rest counts from the next start
        std::string why;
        if (!CoopOriginal::ApplyMods(CoopRuntime::GameDir() / "mods", g_menu.options().original_game, why)) {
            LOG_WARNING(Core, "coop: original game: {}", why);
            ok = false;
        }
    }
    LOG_INFO(Core, "coop: multiplayer tab: settings saved{}{}", ok ? "" : " (a file could not be written)",
             kit ? "" : " (no kit found: coop.ini only)");
    std::vector<std::string> note;
    if (!ok) {
        note.push_back("A settings file could not be written.");
    }
    if (!kit) {
        note.push_back("No co-op kit found with this game: only your name was saved (coop.ini).");
    }
    if (live && ok) {
        note.push_back("Saved. Co-op is on: some settings count the next time you host or join.");
    }
    if (!note.empty()) {
        g_menu.SetNote(note);
    }
}

// The Settings page's Controller and Keyboard rows (coop_input.h): none without patch 0015.
void InputRows() {
    if (CoopInput::Available()) {
        g_menu.SetInputRows(CoopInput::ControllerRow(), CoopInput::KeyboardRow());
    } else {
        g_menu.SetInputRows({}, {});
    }
}

// The next controller (or keyboard) choice: at once, and in coop-options.ini for the next start.
void CycleInput(bool controller) {
    const CoopInput::Setting s = controller ? CoopInput::CycleController() : CoopInput::CycleKeyboard();
    const char* key = controller ? "LICHII_COOP_PAD" : "LICHII_COOP_KEYBOARD";
    const std::string value = controller ? CoopInput::PadValue(s.pad) : CoopInput::KeyboardValue(s.keyboard);
    const bool usual = controller ? s.pad == CoopInput::kAuto : s.keyboard == CoopInput::Keyboard::Focus;
    InputRows();
    LOG_INFO(Core, "coop: multiplayer tab: {} = {}", key, value);
    // (the file the game reads at its start, as Save: the kit's, or this game's coop-player.ini where several games of
    // this PC share the kit, so player 2's controller is not player 1's)
    if (!CoopTitle::KitOptionsFile() && !CoopTitle::PlayerOptionsFile()) {
        g_menu.SetNote({"No co-op kit found with this game: the choice counts until the game closes."});
        return;
    }
    const std::string before = CoopTitle::ReadKitOptions();
    const std::string after =
        usual ? CoopTitleMenu::IniUnset(before, key) : CoopTitleMenu::IniSet(before, key, value, false);
    if (!CoopTitle::WriteKitOptions(after)) {
        g_menu.SetNote({"A settings file could not be written: the choice counts until the game closes."});
    }
}

// The address and the join password for a friend on the host's home network (Same network) or on a VPN with the
// host (VPN): the first address's line on the clipboard (never logged).
void CopyLan(bool vpn) {
    const HosCoopStatus st = Status();
    if (st.state != HOS_COOP_HOSTING || st.port == 0) {
        g_menu.SetNote({"The server is not running yet."});
        return;
    }
    const auto cards = CoopFiles::KeptLocalCards(NowMs());   // (fps-spike-0121: read off the frame)
    if (!cards) {
        g_menu.SetNote({kNetworksReading});
        return;
    }
    const CoopTitleMenu::LanSplit split = CoopTitleMenu::SplitLan(*cards);
    const auto& addresses = vpn ? split.vpn : split.home;
    const std::uint64_t session = g_session;
    const bool open = st.same_network_open != 0;   // (this network's players need no password: none on the line)
    const auto show = [vpn, session, addresses, open, port = st.port, password = std::string(st.password)](bool copied) {
        if (session != g_session) {
            return;   // (hosted again or joined since: these lines are old)
        }
        g_lan_password = password;
        g_lan_lines = vpn ? CoopTitleMenu::VpnLines(addresses, port, password, copied, open)
                          : CoopTitleMenu::HomeLines(addresses, port, password, copied, open);
        LOG_INFO(Core, "coop: multiplayer tab: host: {}: {} address(es){}", vpn ? "VPN" : "same network",
                 addresses.size(), copied ? ", the join line is on the clipboard" : "");
        RefreshSession(true);
    };
    if (addresses.empty()) {
        show(false);
        return;
    }
    CopyThen(CoopTitleMenu::LanJoinText(addresses.front().address, st.port,
                                        CoopTitleMenu::LanLinePassword(addresses.front().address, st.password, open)),
             show);
}

void TakeAnswer(const std::optional<std::string>& clip);   // (below)

void PasteAnswer() {
    g_lan_lines.clear();
    const HosCoopStatus st = Status();
    if (st.state != HOS_COOP_HOSTING || st.invite[0] == '\0') {
        g_answer_note.clear();
        g_menu.SetNote({st.state != HOS_COOP_HOSTING
                            ? "Host a game first: a friend's answer code is pasted while you host."
                            : "There is no invite code this time, so no answer code: friends on the same network or "
                              "a VPN use the Same network or the VPN row."});
        return;
    }
    const std::uint64_t session = g_session;
    PasteThen([session](std::optional<std::string> clip) {
        if (session == g_session) {
            TakeAnswer(clip);
        }
    });
}

// The clipboard's text for PasteAnswer: the friend's answer code, given to the server.
void TakeAnswer(const std::optional<std::string>& clip) {
    const HosCoopStatus st = Status();
    if (st.state != HOS_COOP_HOSTING || st.invite[0] == '\0') {
        return;   // (the server stopped while the clipboard was read)
    }
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    const CoopHostMenu::PastedAnswer paste = CoopHostMenu::ReadPastedAnswer(clip, st.invite, st.password, now);
    if (!paste.ok || !CoopHookshot::HostMenu().Answer(paste.text.c_str())) {
        g_answer_note.clear();
        g_menu.SetNote({paste.ok ? "That answer code could not be used: host again and ask your friend to join again."
                                 : paste.line});
        LOG_INFO(Core, "coop: multiplayer tab: host: a pasted answer code was not taken: {}", paste.line);
        return;
    }
    LOG_INFO(Core, "coop: multiplayer tab: host: the friend's answer code was pasted");
    g_answer_note = paste.line;
    RefreshSession(true);
}

void Copy(const std::string& text, const char* what) {
    if (text.empty()) {
        g_menu.SetNote({std::string("No ") + what + " yet."});
        return;
    }
    CopyThen(text, [text](bool ok) {
        g_menu.SetNote({ok ? std::string("Copied: ") + text
                           : std::string("The clipboard could not be used: write the code down instead.")});
    });
}

// The Settings page's Character by age rows (coop_char_ages.h): the picks and the characters this PC can show.
void FeedCharacterAges(const std::string& characters, const decltype(CoopPuppetConfig::KnownCharacterFiles())& known) {
    const auto all = CoopCharAges::ParseCharacters(characters);
    const auto list = CoopCharAges::Pickable(all, known);
    const auto stored = CoopCharAges::Current();
    // (his model ids, "Hylia_MN_005", by the names his picker shows after the kit's merge: numbered by the ids among his
    // whole list, not among the characters this PC can show, nor by any list's order; coop_charsel_names.h)
    std::vector<std::string> names;
    for (const auto& c : all) names.push_back(c.name);
    const auto readable = CoopCharselNames::Readable(names);
    std::map<int, std::string> shown;
    for (std::size_t i = 0; i < all.size(); ++i) shown.emplace(all[i].number, readable[i]);
    std::vector<std::pair<int, std::string>> rows;
    for (const auto& c : list) {
        const auto it = shown.find(c.number);
        rows.emplace_back(c.number, it != shown.end() ? it->second : c.name);
    }
    g_menu.SetCharacterAges(list.size() > 1, stored.on, stored.child.value_or(-1), stored.adult.value_or(-1),
                            std::move(rows));
}

void FeedCharacterAges() { FeedCharacterAges(CoopPuppetConfig::CharacterListText(), CoopPuppetConfig::KnownCharacterFiles()); }

// A page opens: the menu takes what was read (Opening) for its rows.
void OpenRows(const Opening& o) {
    g_menu.Show(o.options);
    g_menu.SetLook(CoopLook::Shown());   // the Customize page opens on the look waiting, else the one worn
    FeedCharacterAges(o.characters, o.known);
    g_menu.SetNgPlus(CoopNgPlus::Level(), CoopNgPlus::Unlocked(), CoopNgPlus::Hearts());   // (Navi) New Game+
    g_menu.SetSaveSet(o.saves);   // (Navi) the Saves row
    g_menu.SetInputRows(o.input ? o.controller : std::string(), o.input ? o.keyboard : std::string());
    g_menu.SetMicrophones(o.options.voice ? o.microphones : std::nullopt);
}

void Act(const Request& r) {
    switch (r.kind) {
    case Request::Kind::kHost: Host(); break;
    case Request::Kind::kJoin: Join(r.text); break;
    case Request::Kind::kLeave: Leave(); break;
    case Request::Kind::kSave: Save(); break;
    case Request::Kind::kLook: {
        Save();
        const auto note = ChooseLook(r.text);
        g_menu.SetNote(note);
        g_menu.SetLook(CoopLook::Shown());   // (the Customize page shows the tunic chosen here)
        break;
    }
    case Request::Kind::kLookParts: g_menu.SetNote(ChooseLookParts(g_menu.look())); break;
    case Request::Kind::kPaste:
        // Into the field that asked only: on Linux the text may come a moment later (PasteThen), and a field the
        // player moved to meanwhile (another row or page) does not get it.
        PasteThen([page = g_menu.page(), row = g_menu.selected(), typing = g_menu.typing()](std::optional<std::string> clip) {
            if (g_menu.page() != page || g_menu.selected() != row || g_menu.typing() != typing) {
                LOG_INFO(Core, "coop: multiplayer tab: a paste came after the field changed: not used");
                return;
            }
            g_menu.Paste(clip.value_or(std::string()));
            if (!clip) {   // (said, not a silent nothing: what is missing, and the ways that still work)
                g_menu.SetNote({kClipboardUnread});
                LOG_INFO(Core, "coop: multiplayer tab: a paste found no clipboard to read");
            }
        });
        break;
    case Request::Kind::kCopyInvite:
        g_lan_lines.clear();
        Copy(Status().invite, "invite code");
        break;
    case Request::Kind::kCopyAnswer: Copy(Status().answer, "answer code"); break;
    case Request::Kind::kPasteAnswer: PasteAnswer(); break;
    case Request::Kind::kCopyLan: CopyLan(false); break;
    case Request::Kind::kCopyVpn: CopyLan(true); break;
    case Request::Kind::kController: CycleInput(true); break;
    case Request::Kind::kKeyboard: CycleInput(false); break;
    case Request::Kind::kSaveSet:   // (Navi) solo or multiplayer saves (coop_save.h)
        g_menu.SetNote({CoopSave::ChooseSaveSet(r.text == "multiplayer")});
        g_menu.SetSaveSet(CoopSave::MultiplayerSaves());
        break;
    case Request::Kind::kSetPassword: {   // (the host's new join password, typed on the Host page: never logged)
        const bool taken = CoopHookshot::HostMenu().SetPassword(r.text.c_str()) != 0;
        LOG_INFO(Core, "coop: multiplayer tab: host: a new join password typed: {}", taken ? "handed to the host" : "not taken");
        if (taken) {
            g_menu.SetNote({"New join password: players who join from now on need it."});
        } else {
            const HosCoopStatus st = Status();   // (why, in plain words: the status' message)
            const std::string why(st.message, strnlen(st.message, sizeof st.message));
            g_menu.SetNote({"The join password was not changed.", why});
        }
        RefreshSession(true);
        break;
    }
    case Request::Kind::kAllow:
    case Request::Kind::kDeny: {   // (as the chat's /allow and /deny: the oldest asking; the chat says what came of it)
        const bool allow = r.kind == Request::Kind::kAllow;
        const bool sent = CoopHook::SendChat(allow ? "/allow" : "/deny");
        LOG_INFO(Core, "coop: multiplayer tab: host: {} the player asking to join{}", allow ? "let in" : "turned away",
                 sent ? "" : " (not sent)");
        g_menu.SetNote({!sent  ? std::string("Not sent: co-op is not connected.")
                        : allow ? "Letting " + r.text + " in."
                                : r.text + " was turned away."});
        RefreshSession(true);
        break;
    }
    case Request::Kind::kChat: {   // (a host row of Co-op settings: its chat command, one path with the chat box)
        const bool sent = CoopHook::SendChat(r.text);
        LOG_INFO(Core, "coop: multiplayer tab: host rule {}{}", r.text, sent ? "" : " (not sent)");
        Save();   // (PvP's choice is the next Host's too)
        g_menu.SetNote({sent ? std::string("Sent: the chat says what changed.") : std::string("Not sent: co-op is not connected.")});
        RefreshSession(true);
        break;
    }
    case Request::Kind::kRefreshFound:   // (the Find page's list asked again: the relay's rooms, or this network's games)
        if (g_native && g_menu.title()) {   // (the title's list: both kinds)
            CoopRoomList::AskNow();
            CoopLanNet::Forget();
        } else if (g_menu.find_online()) {
            CoopRoomList::AskNow();
        } else {
            CoopLanNet::Forget();
        }
        break;
    case Request::Kind::kDifficulty: {   // the host's choice, for everyone (the chat says it once the server has); with
                                         // no session, the next lobby's (HostStart gives it to the server)
        const auto choice = CoopDifficulty::ParseChoice(r.text);
        const bool live = choice && CoopDifficulty::Set(*choice);
        const bool lobby = choice && !live && CoopDifficulty::SetNextLobby(*choice);
        if (!live && !lobby) {
            g_menu.SetNote({"Only the host can change the difficulty, while hosting."});
        }
        LOG_INFO(Core, "coop: multiplayer tab: difficulty {}", live ? "asked" : lobby ? "kept for the next lobby" : "refused");
        g_next_session_ms = 0;   // (the rows show the kept one at the next refresh)
        break;
    }
    case Request::Kind::kCharAges: {   // "on", "off", "child=N" or "adult=N" (coop_char_ages.h)
        if (r.text == "on" || r.text == "off") {
            CoopCharAges::SetOn(r.text == "on");
        } else if (const bool child = r.text.rfind("child=", 0) == 0; child || r.text.rfind("adult=", 0) == 0) {
            CoopCharAges::SetPick(child, std::atoi(r.text.c_str() + 6));
        }
        FeedCharacterAges();
        break;
    }
    case Request::Kind::kLocalStart:   // (the Local co-op page's Start: this PC's other games, into this game's session)
        LocalAsked(std::atoi(r.text.c_str()));
        break;
    case Request::Kind::kNgPlus: {   // (Navi) New Game+ (coop_ngplus.h)
        CoopNgPlus::SetLevel(std::atoi(r.text.c_str()));
        g_menu.SetNgPlus(CoopNgPlus::Level(), CoopNgPlus::Unlocked(), CoopNgPlus::Hearts());
        g_menu.SetNote({CoopNgPlus::Level() == 0 ? std::string("New Game+ off.")
                                                 : "New Game+ " + std::to_string(CoopNgPlus::Level()) +
                                                       ": start a new file to play it (your hearts carry over)."});
        break;
    }
    }
}

// The requests a press left, carried out (one can lead to another: a typed name ended, then saved).
void Settle() {
    for (int i = 0; i < 4; ++i) {
        const auto r = g_menu.TakeRequest();
        if (!r) {
            return;
        }
        Act(*r);
    }
}

void ShowTab() {
    g_shown = true;
    g_shown_any = true;
    // (the files, the saves, a controller or microphone plugged in while the menu was closed: read by MenuStep before
    // g_mutex, row 236; live only when none was)
    const Opening o = g_opening != nullptr ? *g_opening : GatherOpening();
    OpenRows(o);
    const auto last = CoopTitleMenu::IniGet(o.coop, "last_invite");
    g_menu.SetRejoin(CoopTitleMenu::IniGet(o.coop, "last_host").value_or(""), last.value_or(""));
    RefreshSession(true);
}

}  // namespace

// Nucleus's "Join a game on this network" (tools/nucleus/OoTCoop.js): coop.ini's join_lan ("first", or a host's
// name) and no server: at the start the games on this network are listened for, and the first that can be joined (or
// that host's) is joined, with no password (a host lets its own network in). None in kLanJoinMs: said, and the player
// picks one in Browse games. g_mutex.
std::string g_lan_join;            // what to join, "" none (or done)
std::uint64_t g_lan_join_since = 0;
constexpr std::uint64_t kLanJoinMs = 60000;

void LanJoinStep(std::uint64_t now) {
    if (g_lan_join.empty()) return;
    if (g_lan_join_since == 0) {
        g_lan_join_since = now;
        LOG_INFO(Core, "coop: multiplayer tab: join_lan: listening for a game on this network to join ({})",
                 g_lan_join == "first" ? std::string("the first that can be joined") : "the host " + g_lan_join);
    }
    if (const HosCoopStatus st = Status(); st.state != HOS_COOP_IDLE && st.state != HOS_COOP_FAILED) {
        g_lan_join.clear();   // (a session already: the player's own)
        return;
    }
    CoopLanNet::Look(true, now);
    if (const auto pick = CoopLan::PickToJoin(CoopLanNet::Games(now), g_lan_join, coop::protocol::kProtocolVersion)) {
        LOG_INFO(Core, "coop: multiplayer tab: join_lan: joining {}'s game on this network ({} ms after the start)",
                 pick->game.name, now - g_lan_join_since);
        g_lan_join.clear();
        CoopLanNet::Look(false, now);
        // (the files' settings first, the player's name above all: the menu's own are its defaults until it opens,
        // and a join as "Player" was refused, the host's own name, a test run 2026-11-20; as auto-rejoin does)
        g_menu.Show(ReadOptions());
        const std::string line = pick->address + ":" + std::to_string(pick->game.port);
        if (!Join(line)) LOG_INFO(Core, "coop: multiplayer tab: join_lan: the join did not start");
        return;
    }
    if (now - g_lan_join_since > kLanJoinMs) {
        LOG_INFO(Core, "coop: multiplayer tab: join_lan: no game found on this network in {} s",
                 kLanJoinMs / 1000);
        g_lan_join.clear();
        CoopLanNet::Look(false, now);
        g_menu.SetNote({"No game found on this network. Pick one in Browse games, or host one."});
    }
}

// Local co-op (the kit's Start Squadala with 2 to 4 players on this PC, scripts\local-coop.ps1): game 1 hosts the
// session itself at its start, on the port and with the password the launcher gave in OOT_COOP_LOCAL_HOST
// (CoopHostMenu::kLocalHostEnv), as the Multiplayer page's Host does: a private game, no relay, router port or
// invite, the listener on 127.0.0.1 only (Menu::LoopbackNextHost: this host alone). The other games of this PC join it from their coop.ini. The
// host is this game: when it closes, the others get the usual "the host left". Nothing is pressed on the title (the
// players load their saves themselves). g_mutex.
std::optional<std::pair<std::uint16_t, std::string>> g_local_host;

void LocalHostStep() {
    if (!g_local_host) return;
    const auto [port, password] = *g_local_host;
    g_local_host.reset();
    if (Busy()) {
        LOG_INFO(Core, "coop: multiplayer tab: local co-op: co-op is on already in this game: not hosting");
        return;
    }
    g_menu.Show(ReadOptions());   // (the files' settings, the player's name above all, as LanJoinStep)
    CoopTitleMenu::Options o = g_menu.options();
    o.host_port = port;
    o.password = password;
    o.public_game = false;
    o.approve_joins = false;
    o.relay.clear();
    o.relay_key.clear();
    o.relay_room.clear();
    o.list_room = false;
    // (every game of this PC fits: the session is this PC's alone, and a player's own "Players when you host" of 2
    // left games 3 and 4 out)
    o.max_players = std::max(o.max_players, CoopPauseMenu::kLocalMost);
    LOG_INFO(Core, "coop: multiplayer tab: local co-op: this game hosts the session of this PC's games (port {}, "
                   "127.0.0.1 only)", port);
    CoopHookshot::HostMenu().LoopbackNextHost();   // (this host only: a later Host from the menu is a usual one)
    if (!Host(o, true)) LOG_WARNING(Core, "coop: multiplayer tab: local co-op: the host did not start");
}

// Local co-op from the game (the Local co-op page's Start, coop_local_start.h): this game hosts the session (as the
// launcher's game 1 does, LocalHostStep) and then runs the kit's launcher, which starts the games of players 2 to N
// that do not run into it. g_mutex, so no process, file or port work here (row 236): what it needs is read before
// (GatherLocal), and what it starts runs after (LocalOrder, LocalAct), as Gather and g_announce.
struct LocalRun {
    std::optional<int> ask;           // Start was pressed for so many games: taken at the next Follow (LocalStep)
    int games = 0;                    // the local session's games, this one included (0: none in this game)
    CoopLocalStart::Session session;  // its port and password
    bool launch = false;              // the launcher runs once this game hosts
    bool waiting = false;             // its answer (CoopLocalStart::kResultFile) is awaited, since asked_ms
    std::uint64_t asked_ms = 0;
    std::uint64_t counted_ms = 0;     // this PC's games last counted (every 2 s while the page shows)
    std::string note, problem;
};
LocalRun g_local;

// What LocalStep needs from outside, read before g_mutex (GatherLocal); nothing is read while none of it is wanted.
struct LocalOutside {
    bool offered = false;
    bool counted = false;           // this PC's games were counted: running
    std::vector<int> running;
    bool asked = false;             // a Start waits: the session it would host
    CoopLocalStart::Session session;   // a session the running games keep (port 0: none), else a free port to make one on
    std::uint16_t free_port = 0;
    std::optional<std::string> result;   // the launcher's answer file, while it is awaited
    std::optional<int> test_ask;         // (local-start.now)
};

// What LocalStep decided, carried out by LocalAct after g_mutex.
struct LocalOrder {
    bool pad = false;      // this game reads controller 1 and the keyboard
    bool launch = false;   // the launcher runs, for `games` into `session`
    int games = 0;
    CoopLocalStart::Session session;
};
LocalOrder g_local_order;

std::filesystem::path LocalRoot() {
    return CoopRuntime::GameDir() / "local-players";
}

bool LocalOffered() {
    static const bool offered = CoopLocalStart::Offered(CoopFiles::FindKit(), CoopRuntime::GameDir());
    return offered;
}

// (a test's trigger, LICHII_COOP_TEST_LOCAL=1 only: a file "local-start.now" in the game's folder holding N asks what
// the page's Start asks, and the page's state is counted and logged even with the page closed)
bool TestLocal() {
    static const bool on = [] {
        const char* v = std::getenv("LICHII_COOP_TEST_LOCAL");
        const bool set = v != nullptr && v[0] == '1';
        if (set) LOG_INFO(Core, "coop: multiplayer tab: test: local-start.now in {} asks Start (local co-op)",
                          CoopRuntime::GameDir().string());
        return set;
    }();
    return on;
}

// The Local co-op page's Start: taken at the next Follow. g_mutex.
void LocalAsked(int games) {
    g_local.ask = games;
}

// Not under g_mutex (Tick's thread, before LocalStep).
LocalOutside GatherLocal(std::uint64_t now) {
    LocalOutside o;
    const std::filesystem::path game = CoopRuntime::GameDir();
    if (TestLocal()) {
        const std::filesystem::path asked = game / "local-start.now";
        const std::string n = ReadText(asked);
        std::error_code ec;
        if (!n.empty() && std::filesystem::remove(asked, ec)) o.test_ask = std::atoi(n.c_str());
    }
    bool waiting = false;
    {
        MenuLock lock(__func__);
        o.asked = g_local.ask || o.test_ask;
        waiting = g_local.waiting;
        const bool shown = (g_shown || g_native) && g_menu.page() == CoopPauseMenu::Page::kLocal;
        o.counted = (shown || TestLocal()) && (o.asked || g_local.counted_ms == 0 || now - g_local.counted_ms >= 2000);
    }
    if (waiting) o.result = ReadText(LocalRoot() / CoopLocalStart::kResultFile);
    if (!o.asked && !o.counted) return o;
    o.offered = LocalOffered();
    if (!o.offered) return o;
    o.running = CoopLocalStart::RunningPlayers(game);
    if (!o.asked) return o;
    // (this game was closed and started again while the others ran: they keep their session, so this game hosts that
    // one again and they can join it again; else a new one)
    for (const int p : o.running) {
        const auto old = CoopLocalStart::SessionOf(ReadText(LocalRoot() / ("player" + std::to_string(p)) / "coop.ini"));
        if (old && CoopLocalStart::PortFree(old->port)) {
            o.session = *old;
            break;
        }
    }
    if (o.session.port == 0) o.free_port = CoopLocalStart::FreePort();
    return o;
}

void LocalStart(int games, const LocalOutside& o) {
    games = std::clamp(games, CoopPauseMenu::kLocalFewest, CoopPauseMenu::kLocalMost);
    if (!o.offered) {
        g_local.note = "This game cannot start the others.";
        return;
    }
    const bool ours = g_local.games > 0 && g_doing == "hosting";
    if (!ours) {
        if (Busy()) {
            g_local.note = "Leave this game first.";
            return;
        }
        CoopLocalStart::Session s = o.session;
        if (s.port == 0 && o.free_port != 0) {
            s.port = o.free_port;
            const auto wall = static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
            s.password = CoopLocalStart::NewPassword(
                NowMs() ^ wall ^ (static_cast<std::uint64_t>(CoopLocalStart::ThisPid()) << 32));
        }
        if (s.port == 0) {
            g_local.note = "No free port on this PC for the session.";
            LOG_WARNING(Core, "coop: multiplayer tab: local co-op: no free UDP port from {}", CoopLocalStart::kFirstPort);
            return;
        }
        g_local.session = s;
        g_local_host = std::make_pair(s.port, s.password);   // (LocalHostStep hosts it at the next Follow)
        g_local_order.pad = true;
    }
    g_local.games = games;
    g_local.launch = true;
    g_local.note.clear();
    g_local.problem.clear();
    LOG_INFO(Core, "coop: multiplayer tab: local co-op: {} games on this PC asked ({})", games,
             ours ? "this game hosts them already" : "this game hosts them first");
}

// Each Follow: a Start taken, the launcher ordered once this game hosts, its answer read, and this PC's games counted
// for the page. g_mutex; `o` was read before it.
void LocalStep(std::uint64_t now, const LocalOutside& o) {
    if (o.test_ask) {
        LOG_INFO(Core, "coop: multiplayer tab: test: Start asked for {} games (local-start.now)", *o.test_ask);
        LocalAsked(*o.test_ask);
    }
    if (g_local.games > 0 && !g_local_host) {
        const HosCoopStatus st = Status();
        const bool hosting = g_doing == "hosting" && st.state != HOS_COOP_IDLE && st.state != HOS_COOP_FAILED;
        if (g_local.launch && hosting && st.state == HOS_COOP_HOSTING) {
            g_local.launch = false;
            g_local.waiting = true;   // (LocalAct: not, when the launcher does not start)
            g_local.asked_ms = now;
            g_local_order.launch = true;
            g_local_order.games = g_local.games;
            g_local_order.session = g_local.session;
        } else if (!hosting && !g_local.waiting) {   // (the host did not start, or was stopped: no local session)
            if (g_local.launch) g_local.note = "This game could not host: see Host a game.";
            g_local.games = 0;
            g_local.launch = false;
        }
    }
    if (g_local.waiting && o.result) {
        if (const auto r = CoopLocalStart::ParseResult(*o.result)) {
            g_local.waiting = false;
            g_local.note = CoopLocalStart::ResultNote(*r);
            g_local.problem = r->problem;
            LOG_INFO(Core, "coop: multiplayer tab: local co-op: launcher: {} started, {} ran already, {} did not start{}{}",
                     r->started.size(), r->running.size(), r->failed.size(), r->problem.empty() ? "" : ": ", r->problem);
        } else if (now - g_local.asked_ms > CoopLocalStart::kAnswerMs) {
            g_local.waiting = false;
            g_local.note = "The launcher did not answer.";
            LOG_WARNING(Core, "coop: multiplayer tab: local co-op: no answer from the launcher in {} s",
                        CoopLocalStart::kAnswerMs / 1000);
        }
    }
    if (g_local.ask && o.asked) {   // (one pressed since GatherLocal waits for the next Follow)
        LocalStart(*std::exchange(g_local.ask, std::nullopt), o);
    }
    if (!o.counted) return;
    g_local.counted_ms = now;
    CoopPauseMenu::LocalGames l;
    const int player = CoopLocalStart::PlayerOf(CoopRuntime::GameDir());
    l.player = player >= CoopPauseMenu::kLocalFewest ? player : 0;
    l.offered = l.player == 0 && o.offered;
    l.asked = g_local.games;
    if (l.offered) {
        int running = 1;   // (this game; then the others of this session's count)
        for (const int p : o.running) {
            if (g_local.games == 0 || p <= g_local.games) ++running;
        }
        l.running = running;
    }
    if (g_local.games > 0) l.joined = std::max(1, static_cast<int>(Status().players));
    l.starting = g_local.launch || g_local.waiting;
    l.note = g_local.note;
    l.problem = g_local.problem;
    if (TestLocal() && !(l == g_menu.local())) {
        LOG_INFO(Core, "coop: multiplayer tab: test: local page: asked {} running {} joined {} starting {} note '{}' "
                       "problem '{}'", l.asked, l.running, l.joined, l.starting, l.note, l.problem);
    }
    g_menu.SetLocal(std::move(l));
}

// Not under g_mutex (Tick's thread, after LocalStep): what it ordered.
void LocalAct(const LocalOrder& order) {
    // This game is player 1: controller 1 and the keyboard, for this run (its own choice kept when it made one; with
    // every controller, the other players' pads moved this Link too)
    if (order.pad && CoopInput::Current().pad == CoopInput::kAuto) {
        CoopInput::Apply({1, CoopInput::Keyboard::Focus});
        LOG_INFO(Core, "coop: multiplayer tab: local co-op: this game reads controller 1 and the keyboard (this run)");
    }
    if (!order.launch) return;
    const auto kit = CoopFiles::FindKit();
    const std::filesystem::path exe = CoopLocalStart::ThisExe();
    std::string note;
    if (!kit || exe.empty()) {
        note = "The launcher was not found.";
    } else {
        std::error_code ec;
        std::filesystem::create_directories(LocalRoot(), ec);
        const std::filesystem::path result = LocalRoot() / CoopLocalStart::kResultFile;
        std::filesystem::remove(result, ec);   // (before the next GatherLocal, this thread's too)
        if (!CoopLocalStart::Launch(CoopLocalStart::CommandLine(*kit, exe, order.games, order.session,
                                                               CoopLocalStart::ThisPid(), result))) {
            note = "The launcher did not start.";
            LOG_WARNING(Core, "coop: multiplayer tab: local co-op: powershell did not start (kit {})",
                        CoopFiles::Utf8Path(*kit));
        }
    }
    if (note.empty()) {
        LOG_INFO(Core, "coop: multiplayer tab: local co-op: the launcher starts the games of players 2 to {} that do "
                       "not run (port {})", order.games, order.session.port);
        return;
    }
    MenuLock lock(__func__);
    g_local.waiting = false;
    g_local.note = note;
}

void Init(bool coop_on) {
    const char* local = std::getenv(CoopHostMenu::kLocalHostEnv);
    const std::string listed = local != nullptr && *local != 0 ? ReadText(LocalRoot() / "instances.txt") : std::string();
    MenuLock lock(__func__);
    g_coop_on = coop_on;
    if (local != nullptr && *local != 0) {
        g_local_host = coop_on ? std::nullopt : CoopHostMenu::ParseTestHost(local);
        if (g_local_host) {   // (the launcher's game 1: its session is the page's, so Start missing games adds to it)
            g_local.session = {g_local_host->first, g_local_host->second};
            g_local.games = std::clamp(CoopLocalStart::GamesListed(listed),
                                       CoopPauseMenu::kLocalFewest, CoopPauseMenu::kLocalMost);
        }
        if (!g_local_host) {
            LOG_WARNING(Core, "coop: multiplayer tab: local co-op: {} {}: not hosting", CoopHostMenu::kLocalHostEnv,
                        coop_on ? "with a server in coop.ini" : "is not PORT:PASSWORD");
        }
    }
    if (!coop_on) {
        if (const auto want = CoopTitleMenu::IniGet(ReadText(CoopIni()), "join_lan"); want && !CoopTitleMenu::Trim(*want).empty()) {
            g_lan_join = std::string(CoopTitleMenu::Trim(*want));
        }
    }
    LOG_INFO(Core, "coop: multiplayer tab: ready (the first tab of Hookshot's menu: F1, or Home on a pad){}",
             coop_on ? "; co-op came with the game's start" : "");
}

// The Find a game page (the title's Join page's first row): while it shows, the games on this network or the relay's
// public rooms are heard and handed to it, as the pause menu's browser lists them. g_mutex.
bool g_finding = false;
void RefreshFound(std::uint64_t now) {
    const bool want = (g_shown || g_native) && g_menu.page() == CoopPauseMenu::Page::kFind;
    if (!want) {
        if (g_finding) CoopLanNet::Look(false, now), CoopRoomList::Look(CoopRoomList::Viewer::kMenu, false, now, {});
        g_finding = false;
        return;
    }
    g_finding = true;
    // (the title's list: both kinds at once, this network's first; the overlay's: the one its List row says)
    const bool both = g_native && g_menu.title();
    const bool online = g_menu.find_online();
    CoopLanNet::Look(both || !online, now);
    CoopRoomList::Look(CoopRoomList::Viewer::kMenu, both || online, now, g_menu.options().relay);
    std::vector<CoopPauseMenu::Menu::Found> games;
    std::string state;
    const int mine = coop::protocol::kProtocolVersion;
    if (both) {
        for (const CoopLan::Heard& h : CoopLanNet::Games(now)) {
            const bool full = h.game.max > 0 && h.game.players >= h.game.max;
            games.push_back({h.game.name + "'s game", full ? std::string("Full") : CoopPauseTab::ListValue(h.game.players, h.game.max, h.game.protocol == mine),
                             h.address, h.game.port, CoopLan::Joinable(h, mine), h.game.name, h.game.players,
                             h.game.max, h.game.protocol == mine, false, false});
        }
        for (const CoopRoomList::Room& r : CoopRoomList::Now().rooms) {
            const bool full = r.max > 0 && r.players >= r.max;
            const std::string count = r.protocol != mine ? CoopI18n::Tr("Other version")
                                      : full             ? CoopI18n::Tr("Full")
                                      : std::to_string(r.players) + "/" + std::to_string(r.max);
            games.push_back({CoopPauseTab::ListName(r.host, r.room), count, r.room, 0, CoopRoomList::Joinable(r, mine),
                             r.host, r.players, r.max, r.protocol == mine, r.is_public, true});
        }
        // (a run's screenshots, LICHII_COOP_TEST_TITLE_GAMES=1: three made-up rooms after the real ones, a public one,
        // one that asks a password and a full one, never joinable for real: their rooms are not on any relay)
        static const bool test_games = [] {
            const char* e = std::getenv("LICHII_COOP_TEST_TITLE_GAMES");
            return e != nullptr && std::string(e) == "1";
        }();
        if (test_games) {
            games.push_back({"forest-4821 (dauro)", "2/4", "test-forest-4821", 0, true, "dauro", 2, 4, true, true, true});
            games.push_back({"kokiri (Mido)", "3/4", "test-kokiri", 0, true, "Mido", 3, 4, true, false, true});
            games.push_back({"lost-woods (Link2)", CoopI18n::Tr("Full"), "test-lost-woods", 0, false, "Link2", 4, 4, true,
                             true, true});
        }
        g_menu.SetFound(std::move(games), {});
        return;
    }
    if (online) {
        const CoopRoomList::Snapshot rooms = CoopRoomList::Now();
        for (const CoopRoomList::Room& r : rooms.rooms) {
            // (the same words as the Lobby tab's and the pause tab's Public games: CoopRoomList::DetailText)
            games.push_back({CoopPauseTab::ListName(r.host, r.room), CoopI18n::Tr(CoopRoomList::DetailText(r, mine)),
                             r.room, 0, CoopRoomList::Joinable(r, mine), r.host, r.players, r.max, r.protocol == mine,
                             r.is_public});
        }
        state = rooms.state.empty() ? std::string() : CoopI18n::Tr(rooms.state);
    } else {
        for (const CoopLan::Heard& h : CoopLanNet::Games(now)) {
            games.push_back({h.game.name + "'s game" + (h.game.room.empty() ? "" : " (" + h.game.room + ")"),
                             CoopPauseTab::ListValue(h.game.players, h.game.max, h.game.protocol == mine), h.address,
                             h.game.port, CoopLan::Joinable(h, mine), h.game.name, h.game.players, h.game.max,
                             h.game.protocol == mine, false});
        }
    }
    g_menu.SetFound(std::move(games), std::move(state));
}

// The follow-ups of the Multiplayer tab and the session (Tick, the game's main thread).
// Tick's thread only. What Follow needs from outside is gathered before g_mutex, and the LAN line is said after it.
static void FollowStep() {
    static std::uint64_t next_ms = 0, next_files_ms = 0;
    const std::uint64_t due = NowMs();
    std::optional<Outside> outside;
    LocalOutside local;
    if (due >= next_ms) {
        next_ms = due + 250;
        const bool files = due >= next_files_ms;
        if (files) next_files_ms = due + 5000;
        outside = Gather(files);
        local = GatherLocal(due);
    }
    std::optional<CoopLan::Game> announce;
    LocalOrder order;
    std::uint64_t now = 0;
    {
        MenuLock lock(__func__);
        Delivered();   // (each frame: a finished copy or paste shows at once)
        if (!outside) {
            return;
        }
        g_outside = &*outside;
        now = NowMs();
        VoiceStatusRows();   // (row 229: a microphone found silent while the page shows says so there)
        Follow(Status());
        RefreshFound(now);
        LocalHostStep();
        LocalStep(now, local);
        LanJoinStep(now);
        g_outside = nullptr;
        announce = std::exchange(g_announce, std::nullopt);
        order = std::exchange(g_local_order, LocalOrder{});
    }
    LocalAct(order);
    if (announce) {
        CoopLanNet::Announce(*announce, now);
    }
    // (a test's trigger, LICHII_COOP_TEST_TO_TITLE=1 only: a file "to-title.now" in the game's folder starts the way
    // back to the title a join with a file loaded takes, with no host to join)
    static const bool test_to_title = [] {
        const char* v = std::getenv("LICHII_COOP_TEST_TO_TITLE");
        const bool on = v != nullptr && v[0] == '1';
        if (on) LOG_INFO(Core, "coop: multiplayer tab: test: to-title.now in {} starts the way back to the title",
                         CoopRuntime::GameDir().string());
        return on;
    }();
    if (test_to_title) {
        std::error_code ec;
        if (std::filesystem::remove(CoopRuntime::GameDir() / "to-title.now", ec)) {
            LOG_INFO(Core, "coop: multiplayer tab: test: back to the title asked (to-title.now)");
            CoopPauseTab::ToTitle();
            CoopTitle::LoadHostSaveOnTitle(true);
        }
    }
}

// Tab 1: the Lobby (coop_lobby_tab.h), always: who is in the game, the host's controls.
// Tab 2: Mods (coop_mods_tab.h), once the mod loader runs.
constexpr int kLobbyTab = 1;
constexpr int kModsTab = 2;
constexpr int kTabsMax = 3;

// Hookshot's menu asks the pages and hands the presses on its present's thread, under its menu lock (row 279): there
// they only go through these (coop_menu_handoff.h). Tick carries the presses out and publishes the pages.
static CoopMenuHandoff::Handoff g_handoff[kTabsMax];
static bool g_tab_shown[kTabsMax] = {};   // (Tick only) the Lobby and Mods tabs: SHOW came and no HIDE since

int Tabs() {
    return CoopModsTab::Available() ? 3 : 2;
}

const char* TabName(int tab) {
    const char* en = nullptr;
    if (tab == kModsTab && CoopModsTab::Available()) {
        en = CoopModsTab::TabName();
    } else if (tab == kLobbyTab) {
        en = CoopLobbyTab::TabName();
    } else if (tab == kMultiplayerTab) {
        en = "Multiplayer";
    }
    if (en == nullptr) {
        return nullptr;
    }
    // The strip's name in the menu's language now (coop_i18n.h). Hookshot
    // copies it at once, so this thread's copy holds until the next call.
    thread_local std::string name;
    name = CoopI18n::Tr(en);
    return name.c_str();
}

namespace {

// The page's words in the player's language (coop_i18n.h), looked up here on the game's main thread so the present's
// thread copies them as they are. The hint (a code, an address, a path) as it is.
CoopMenuHandoff::Page InLanguage(CoopPauseMenu::View v, bool back_taken, bool empty_words_none) {
    if (CoopI18n::Current() != nullptr) {
        const auto tr = [](std::string& text) {
            if (!text.empty()) text = CoopI18n::Tr(text);
        };
        for (CoopPauseMenu::Row& r : v.rows) {
            tr(r.name);
            tr(r.value);
        }
        tr(v.help_title);
        tr(v.help);
        tr(v.gold);
        tr(v.change);
        tr(v.confirm);
        tr(v.back);
    }
    CoopMenuHandoff::Page page;
    page.view = std::move(v);
    page.back_taken = back_taken;
    page.empty_words_none = empty_words_none;
    return page;
}

// One press on the Multiplayer tab, carried out; whether it was taken (Back: false on the first page, nothing to go
// back to). Under g_mutex.
bool MultiplayerPress(const CoopMenuHandoff::Event& e) {
    bool taken = true;
    switch (e.event) {
    case HOS_COOP_MENU_UP: g_menu.Press(Input::kUp); break;
    case HOS_COOP_MENU_DOWN: g_menu.Press(Input::kDown); break;
    case HOS_COOP_MENU_LEFT: g_menu.Press(Input::kLeft); break;
    case HOS_COOP_MENU_RIGHT: g_menu.Press(Input::kRight); break;
    case HOS_COOP_MENU_CONFIRM: g_menu.Press(Input::kDecide); break;
    case HOS_COOP_MENU_BACK: taken = g_menu.Press(Input::kBack); break;
    case HOS_COOP_MENU_SHOW: ShowTab(); break;
    case HOS_COOP_MENU_HIDE:
        g_shown = false;
        g_shown_any = false;
        g_menu.Hide();
        WearChosenLook();
        break;
    case HOS_COOP_MENU_SELECT:
        if (e.arg >= 0) {
            g_menu.Select(static_cast<std::size_t>(e.arg));
        }
        break;
    case HOS_COOP_MENU_CLICK:
        if (e.arg >= 0) {
            g_menu.Click(static_cast<std::size_t>(e.arg), e.arg2 < 0 ? -1 : e.arg2 > 0 ? 1 : 0);
        }
        break;
    default: return false;
    }
    Settle();
    return taken;
}

// Whether the tab shows after these presses (SHOW / HIDE).
bool ShownAfter(bool shown, const std::vector<CoopMenuHandoff::Event>& events) {
    for (const CoopMenuHandoff::Event& e : events) {
        if (e.event == HOS_COOP_MENU_SHOW) shown = true;
        if (e.event == HOS_COOP_MENU_HIDE) shown = false;
    }
    return shown;
}

}  // namespace

// The Mods tab's paste: the clipboard (PasteThen: on Linux it may come a few frames later, then on Tick) goes to
// the field that asked, if the player is still on it (Tab::Paste drops it otherwise). Under g_mutex, as the
// Multiplayer tab's own paste is; the tab's own lock is taken inside, never the other way round.
static void ModsPaste(bool into_typing) {
    MenuLock lock(__func__);
    PasteThen([into_typing](std::optional<std::string> clip) { CoopModsTab::Paste(clip, into_typing); });
}

// Hookshot's menu, on the game's main thread (Tick): the presses its present's thread queued, carried out, and each
// tab's page published while it shows (and once after a press). Not under g_mutex: the Lobby tab's presses take it
// (JoinRoomNow) and its reads take the network lock; the Multiplayer tab's part takes it, the Mods paste too.
static bool Showing() {
    MenuLock lock(__func__);
    return g_shown || g_native;
}

static void MenuStep() {
    {
        const std::vector<CoopMenuHandoff::Event> events = g_handoff[kMultiplayerTab].Take();
        // (the session as the page shows it, every 250 ms while a page shows: gathered before g_mutex, row 236)
        static std::uint64_t next_ms = 0;
        std::optional<Outside> outside;
        if (NowMs() >= next_ms && (Showing() || !events.empty())) {
            next_ms = NowMs() + 250;
            outside = Gather(false);
        }
        std::optional<Opening> opening;   // (a SHOW: the page opens, ShowTab)
        for (const CoopMenuHandoff::Event& e : events) {
            if (e.event == HOS_COOP_MENU_SHOW && !opening) opening = GatherOpening();
        }
        MenuLock lock(__func__);
        g_outside = outside ? &*outside : nullptr;
        g_opening = opening ? &*opening : nullptr;
        for (const CoopMenuHandoff::Event& e : events) {
            MultiplayerPress(e);   // (a Host, Join or Leave reads live from there on: Fresh)
        }
        g_opening = nullptr;
        if (outside && (g_shown || g_native || !events.empty())) {
            RefreshSession(true);   // (also what NativeView reads: the title's rows and the - menu's page)
        }
        g_outside = nullptr;
        if (g_shown || !events.empty()) {
            CoopMenuHandoff::Page page = InLanguage(g_menu.view(), g_menu.BackTaken(), true);
            page.picture = g_menu.Picture();
            g_handoff[kMultiplayerTab].Publish(std::move(page));
        }
    }
    {
        const std::vector<CoopMenuHandoff::Event> events = g_handoff[kLobbyTab].Take();
        g_tab_shown[kLobbyTab] = ShownAfter(g_tab_shown[kLobbyTab], events);
        for (const CoopMenuHandoff::Event& e : events) {
            CoopLobbyTab::MenuInput(e.event, e.arg, e.arg2);
        }
        CoopPauseMenu::View view;
        bool back_taken = false;
        if ((g_tab_shown[kLobbyTab] || !events.empty()) && CoopLobbyTab::MenuView(&view, &back_taken)) {
            g_handoff[kLobbyTab].Publish(InLanguage(std::move(view), back_taken, false));
        }
    }
    {
        const std::vector<CoopMenuHandoff::Event> events = g_handoff[kModsTab].Take();
        g_tab_shown[kModsTab] = ShownAfter(g_tab_shown[kModsTab], events);
        for (const CoopMenuHandoff::Event& e : events) {
            CoopModsTab::MenuInput(e.event, e.arg, e.arg2);
            if (CoopModsTab::TakePasteRequest()) {   // a text row's Paste (pad: Left / Right; mouse: its sides)
                ModsPaste(false);
            }
        }
        if (g_tab_shown[kModsTab] || !events.empty()) {
            CoopPauseMenu::View view;
            bool on_mod_page = false;
            if (CoopModsTab::View(&view, &on_mod_page)) {
                const bool back_taken = on_mod_page || view.typing;   // (Tab::Press: Back ends typing, then the list)
                g_handoff[kModsTab].Publish(InLanguage(std::move(view), back_taken, false));
            } else {
                g_handoff[kModsTab].Clear();
            }
        }
    }
}

void Tick() {
    FollowStep();
    MenuStep();
}

// The present's thread (hos_coop_menu.h): the page last published, with no co-op lock (coop_menu_handoff.h).
bool Page(int tab, HosCoopMenuPage* out) {
    if (tab < 0 || tab >= kTabsMax || (tab == kModsTab && !CoopModsTab::Available())) {
        return false;
    }
    thread_local CoopMenuHandoff::Held held[kTabsMax];
    return CoopMenuHandoff::Fill(g_handoff[tab], held[tab], out);
}

// The present's thread: the Customize page's picture from the page last published; the live window's questions go to
// CoopLookView, whose own lock is held around its state only.
bool Picture(int tab, HosCoopMenuPicture* out) {
    if (tab != kMultiplayerTab || out == nullptr || out->size < sizeof(HosCoopMenuPicture)) {
        return false;
    }
    thread_local std::vector<HosCoopMenuShape> shapes;   // (Hookshot copies them before the next call on its thread)
    const std::shared_ptr<const CoopMenuHandoff::Page> page = g_handoff[kMultiplayerTab].Read();
    static const std::vector<CoopLookMenu::Shape> kNone;
    const std::vector<CoopLookMenu::Shape>& picture = page != nullptr ? page->picture : kNone;
    shapes.clear();
    if (picture.empty()) {
        return false;   // (not the Customize page)
    }
    // Your own Link in the game, through a clear window, the camera around him (coop_look_view.h); the flat picture
    // when he cannot be shown (the title screen, a cutscene), or while the camera's eye is blocked with no clear
    // view yet (it is still asked for, so the camera keeps turning toward one).
    const CoopLookView::Window window{out->window[0], out->window[1], out->window[2], out->window[3], out->aspect};
    bool clear = true;
    const bool shown = window.Valid() && CoopLookView::LinkShown(&clear);
    if (shown) {
        CoopLookView::Ask(window);
    }
    if (shown && clear) {
        out->live = 1;
        out->shapes_n = 0;
        out->shapes = nullptr;
        return true;
    }
    for (const CoopLookMenu::Shape& p : picture) {
        if (shapes.size() == HOS_COOP_MENU_MAX_SHAPES) {
            break;
        }
        HosCoopMenuShape s{};
        s.rgba = p.rgba;
        s.outline = p.outline ? 1 : 0;
        s.n = static_cast<std::uint8_t>(std::min<std::size_t>(p.points.size(), HOS_COOP_MENU_SHAPE_POINTS));
        for (std::size_t k = 0; k < s.n; ++k) {
            s.xy[2 * k] = p.points[k].first;
            s.xy[2 * k + 1] = p.points[k].second;
        }
        shapes.push_back(s);
    }
    out->shapes_n = static_cast<std::int32_t>(shapes.size());
    out->shapes = shapes.data();
    return !shapes.empty();
}

// The present's thread: the press queued for Tick; the answer from the page last published (coop_menu_handoff.h).
bool Input(int tab, int event, int arg, int arg2) {
    if (tab < 0 || tab >= kTabsMax || (tab == kModsTab && !CoopModsTab::Available())) {
        return false;
    }
    return g_handoff[tab].Press(event, arg, arg2);
}

int Wanted() {
    return g_wanted.exchange(HOS_COOP_MENU_WANT_NOTHING);
}

void WantLongshotMenu() {
    g_wanted = kWantLongshotMenu;
}

void WantClose() {
    g_wanted = HOS_COOP_MENU_WANT_CLOSE;
}

void WantMultiplayerTab() {
    g_wanted = kMultiplayerTab;
}

bool TabShown() {
    return g_shown_any.load();
}

// Ctrl and Shift held now. Windows: GetKeyState. Linux: the game's own SDL (SDL2 or SDL3, the library the game
// loaded: CoopClipboard::GameSdl; Longshot loads it RTLD_LOCAL, so RTLD_DEFAULT never saw it and Ctrl+V did not
// paste on Linux), whose KMOD_CTRL / KMOD_SHIFT bits are the same in both; without SDL, neither.
static std::pair<bool, bool> Modifiers() {
#ifdef _WIN32
    return {(GetKeyState(VK_CONTROL) & 0x8000) != 0, (GetKeyState(VK_SHIFT) & 0x8000) != 0};
#else
    using GetModState = unsigned int (*)();
    const auto get = reinterpret_cast<GetModState>(CoopClipboard::GameSdl("SDL_GetModState"));
    if (get == nullptr) {
        return {false, false};
    }
    const unsigned int mod = get() & 0xFFFFu;
    return {(mod & 0x00C0u) != 0, (mod & 0x0003u) != 0};
#endif
}

bool Key(std::uint32_t vk, bool repeat) {
    if (CoopModsTab::KeysTaken()) {   // Ctrl+V / Shift+Insert into the Mods tab's text field, as on the Join page
        const auto [ctrl_down, shift_down] = Modifiers();
        if ((vk == 'V' && ctrl_down) || (vk == 0x2D && shift_down && !ctrl_down)) {
            if (!repeat) {
                ModsPaste(true);
            }
            return true;
        }
    }
    if (CoopModsTab::Key(vk, repeat)) {  // the Mods tab's text field, while it is typed into
        return true;
    }
    MenuLock lock(__func__);
    if (!g_shown && !g_native) {
        return false;
    }
    const auto [ctrl_down, shift_down] = Modifiers();
    // Ctrl+V and Shift+Insert paste, typing or not: on the Host page that is Paste answer code (players press
    // Ctrl+V there), on Join the code. A plain Insert stays Hookshot's.
    if (!repeat && ((vk == 'V' && ctrl_down) || (vk == 0x2D && shift_down && !ctrl_down))) {
        const bool taken = g_menu.Press(Input::kPaste);
        Settle();
        if (taken || g_menu.typing()) {
            return true;
        }
    }
    if (!g_menu.typing()) {
        return false;
    }
    if (vk == 0x70 || vk == 0x2D) {   // F1, Insert: Hookshot's (the menu closes, the typing with it)
        return false;
    }
    const bool ctrl = ctrl_down;
    switch (vk) {
    case 0x26: g_menu.Press(Input::kUp); break;
    case 0x28: g_menu.Press(Input::kDown); break;
    case 0x0D: g_menu.Press(Input::kDecide); break;
    case 0x1B: g_menu.Press(Input::kBack); break;
    case 0x08: g_menu.Press(Input::kErase); break;
    case 'V':
        if (ctrl) {
            g_menu.Press(Input::kPaste);
        }
        break;
    default: break;
    }
    Settle();
    return true;   // every other key while typing: its character comes as Char (Q, E and Tab type, too)
}

bool Char(std::uint32_t cp) {
    if (CoopModsTab::Char(cp)) {
        return true;
    }
    MenuLock lock(__func__);
    if ((!g_shown && !g_native) || !g_menu.typing()) {
        return false;
    }
    if (cp >= 0x20 && cp != 0x7F) {
        g_menu.Char(cp);
    }
    return true;
}

bool KeysTaken() {
    if (CoopModsTab::KeysTaken()) {
        return true;
    }
    MenuLock lock(__func__);
    return (g_shown || g_native) && g_menu.typing();
}

void OpenNative(CoopPauseMenu::Page page) {
    {
        MenuLock lock(__func__);
        g_native = true;
    }
    Open(page);
    g_wanted = HOS_COOP_MENU_WANT_NOTHING;   // (no overlay: the title's own rows show the page)
    LOG_INFO(Core, "coop: multiplayer tab: its page on the title's own rows (no overlay)");
}

// (the game's main thread: carried out at once, not through Hookshot's queue)
bool NativeInput(int event, int arg, int arg2) {
    MenuLock lock(__func__);
    if (!g_native) return false;
    return MultiplayerPress(CoopMenuHandoff::Event{event, arg, arg2});
}

CoopPauseMenu::View NativeView() {
    MenuLock lock(__func__);
    if (!g_native) return CoopPauseMenu::View{};
    // (the session is kept fresh by Tick while the page shows (MenuStep): without it the title's rows kept the page of
    // the moment Start hosting was pressed, title-join-shots 2026-11-21. Read here, it was network work on the game's
    // threads under g_mutex: row 236)
    return g_menu.view();
}

CoopPauseMenu::Page NativePage() {
    MenuLock lock(__func__);
    return g_menu.page();
}

bool SnailHome() {
    MenuLock lock(__func__);
    return g_menu.SnailHome();
}

bool NativeOpen() {
    MenuLock lock(__func__);
    return g_native;
}

void NativeGo(CoopPauseMenu::Page page) {
    MenuLock lock(__func__);
    if (!g_native) return;
    g_menu.Open(page);
    Settle();
}

void NativeSelectHostRules() {
    MenuLock lock(__func__);
    if (!g_native || g_menu.page() != CoopPauseMenu::Page::kSettings) return;
    g_menu.Select(g_menu.HostRulesRow());
}

void CloseNative() {
    MenuLock lock(__func__);
    if (!g_native) return;
    g_native = false;
    g_menu.SetTitle(false);
    if (!g_shown) g_menu.Hide();
}

bool NativeBackLeaves() {
    MenuLock lock(__func__);
    return !g_native || g_menu.BackLeaves();
}

void NativeRefresh() {
    MenuLock lock(__func__);
    if (!g_native || g_menu.page() != CoopPauseMenu::Page::kFind) return;
    g_menu.RefreshFound();
}

void OpenJoinRoom(const std::string& room) {
    Open(CoopPauseMenu::Page::kJoin);
    MenuLock lock(__func__);
    g_menu.PrefillJoin(room);
    LOG_INFO(Core, "coop: multiplayer tab: a private game from the Lobby's Public games: the Join page, its password next");
}

void Open(CoopPauseMenu::Page page) {
    const Opening o = GatherOpening();   // (before g_mutex: files and devices, row 236)
    MenuLock lock(__func__);
    // (as the overlay's tab, ShowTab: the pause page's Settings and look opens Customize and Co-op settings here)
    OpenRows(o);
    g_menu.SetTitle(g_native);   // (the title's own buttons: its pages, docs/hookshot.md "The title menu")
    g_menu.Open(page);
    if (page == CoopPauseMenu::Page::kJoin) {
        const auto last = CoopTitleMenu::IniGet(o.coop, "last_invite");
        g_menu.SetRejoin(CoopTitleMenu::IniGet(o.coop, "last_host").value_or(""), last.value_or(""));
        // A new code copied already: ready to press Join (if the Join page is still open when the clipboard is read).
        PasteThen([last](std::optional<std::string> got) {
            const std::string clip = got.value_or(std::string());
            if (g_menu.page() == CoopPauseMenu::Page::kJoin && coop::server::nat::looks_like_code(clip) &&
                (!last || clip.find(*last) == std::string::npos)) {
                g_menu.Paste(clip);
            }
        });
    }
    Settle();
    g_wanted = kMultiplayerTab;
    LOG_INFO(Core, "coop: multiplayer tab: opened on a page (the title's rows, or the pause menu's Multiplayer page)");
}

namespace {

// The pause menu's Multiplayer page's actions go through the menu's own requests (Act), with the menu's rules: co-op
// that came with the game's start (Host.bat, Join.bat, coop.ini) is not started or stopped here, and what was refused
// or went wrong is said on the page itself (PageSays: the menu's note, else co-op's status line), in the game's own
// rows; co-op's menu stays shut. Under g_mutex.
std::string g_page_says;
std::uint64_t g_page_says_gen = 0;

void SayOnPage(const char* otherwise) {
    std::string t;
    for (const std::string& line : g_menu.NoteLines()) {
        if (line.empty()) continue;
        if (!t.empty()) t += ' ';
        t += line;
    }
    if (t.empty()) {
        const HosCoopStatus st = Status();
        t.assign(st.message, strnlen(st.message, sizeof st.message));
    }
    if (t.empty()) t = otherwise;
    g_page_says = t;
    ++g_page_says_gen;
    LOG_INFO(Core, "coop: multiplayer tab: the pause menu's Multiplayer page says: {}", t);
}

constexpr const char* kSayLauncher = "Co-op came with the game's start: start the game again to change it.";
constexpr const char* kSayRefused = "Co-op did not start.";

// The menu on its main page, the settings as the files have them now (Host reads them from the menu), no note.
void FreshMain() {
    g_menu.Show(ReadOptions());
    g_menu.Open(CoopPauseMenu::Page::kMain);
    RefreshSession(true);
}

}  // namespace

bool ByLauncher() {
    MenuLock lock(__func__);
    return LauncherLive();
}

bool HostStarted() {
    MenuLock lock(__func__);
    return g_doing == "hosting";
}

void HostNow() {
    MenuLock lock(__func__);
    FreshMain();
    LOG_INFO(Core, "coop: multiplayer tab: host from the pause menu's Multiplayer page");
    if (LauncherLive()) {   // (the main page's Host row says why: kByLauncher; not in a game: Host() ends it first)
        LOG_INFO(Core, "coop: multiplayer tab: not hosting: co-op came with the game's start");
        SayOnPage(kSayLauncher);
        return;
    }
    if (!Host()) {
        SayOnPage(kSayRefused);
    }
}

void LeaveNow() {
    MenuLock lock(__func__);
    if (g_coop_on) {   // co-op the launcher started (Nucleus Co-op, Host.bat): left here as any join, its server stays
        FreshMain();
        LOG_INFO(Core, "coop: multiplayer tab: leaving the session the game started with (the launcher's): co-op "
                       "stops here, the server and the other players go on");
        EndLauncher("the player left");   // (its client goes; coop.ini's server line too: the next start by hand is solo)
        g_player_left = true;
        g_doing.clear();
        RefreshSession(true);
        g_menu.SetNote({"Co-op stopped. You play on alone.", "Host or Join to play together again."});
        return;
    }
    Act(Request{Request::Kind::kLeave, {}});
}

void JoinPasteNow() {
    MenuLock lock(__func__);
    FreshMain();
    LOG_INFO(Core, "coop: multiplayer tab: join with the clipboard's code from the pause menu's Multiplayer page");
    if (LauncherLive()) {   // (not in a game: Join() ends it first, row 170)
        SayOnPage(kSayLauncher);
        return;
    }
    const std::uint64_t session = g_session;
    // (on Linux the text may come a moment later, under g_mutex: a Host or Join started meanwhile wins)
    PasteThen([session](std::optional<std::string> clip) {
        if (session != g_session) return;
        std::string line = CoopTitleMenu::OneLine(clip.value_or(std::string()));
        const auto first = line.find_first_not_of(' ');
        line = first == std::string::npos ? std::string() : line.substr(first, line.find_last_not_of(' ') - first + 1);
        if (line.empty()) {
            g_menu.SetNote({clip ? "Copy your friend's code first, then choose Paste code and join." : kClipboardUnread});
            SayOnPage(kSayRefused);
            return;
        }
        if (CoopTitleMenu::RelayLineWithoutPassword(line)) {
            // (an older host's room line: the room, not its password; Join alone would only say "type the password")
            LOG_INFO(Core, "coop: multiplayer tab: join: the pasted relay line has no join password after it");
            g_menu.SetNote({"That room line has no password after it. Ask the host for the join password, then "
                            "type the room and the password in Join a game."});
            SayOnPage(kSayRefused);
            return;
        }
        if (!Join(line)) SayOnPage(kSayRefused);
    });
}

bool AutoRejoin() {
    MenuLock lock(__func__);
    return g_auto_rejoin_on;
}

std::string Relay() {
    return ReadOptions().relay;
}

bool ListRoom() {
    return ReadOptions().list_room;   // (the files' setting: the menu's are defaults until it opens)
}

void SetListRoom(bool on) {
    MenuLock lock(__func__);
    CoopTitleMenu::Options o = ReadOptions();
    o.list_room = on;
    g_menu.Show(o);   // (the menu's settings, as its own rows change them)
    LOG_INFO(Core, "coop: multiplayer tab: this game's room {} the relay's public list when it hosts (from the pause "
                   "menu's Multiplayer page)", on ? "goes on" : "stays off");
    Save();
}

void RejoinAfterRestart(bool due) {
    g_rejoin_due = due;
}

bool AutoRejoining() {
    MenuLock lock(__func__);
    return g_rejoin.Retrying();
}

bool SetHostPort(int port) {
    MenuLock lock(__func__);
    if (port < 1 || port > 65535) return false;
    CoopTitleMenu::Options o = ReadOptions();
    o.host_port = static_cast<std::uint16_t>(port);
    g_menu.Show(o);
    Save();
    const bool again = g_doing == "hosting" && !g_coop_on;
    LOG_INFO(Core, "coop: multiplayer tab: the port when hosting is {} now (from the pause menu's Multiplayer page){}", port,
             again ? ": hosting again on it" : "");
    if (again && !Host()) SayOnPage(kSayRefused);
    return true;
}

std::string RoomName() {
    return ReadOptions().relay_room;
}

bool SetRoomName(const std::string& name) {
    MenuLock lock(__func__);
    if (!name.empty() && !CoopTitleMenu::RoomNameOk(name)) return false;
    CoopTitleMenu::Options o = ReadOptions();
    o.relay_room = name;
    g_menu.Show(o);
    Save();
    LOG_INFO(Core, "coop: multiplayer tab: the room name {} (from the pause menu's Multiplayer page; from the next "
                   "Host on)", name.empty() ? "cleared" : "set");
    return true;
}

bool SetRelay(const std::string& relay) {
    MenuLock lock(__func__);
    if (!relay.empty() && !CoopTitleMenu::RelayOk(relay)) return false;
    CoopTitleMenu::Options o = ReadOptions();
    o.relay = relay;
    g_menu.Show(o);
    Save();
    LOG_INFO(Core, "coop: multiplayer tab: the relay {} (from the pause menu's Multiplayer page)", relay.empty() ? "cleared" : "set");
    return true;
}

void SetAutoRejoin(bool on) {
    MenuLock lock(__func__);
    CoopTitleMenu::Options o = ReadOptions();   // (the files' settings: the menu's are defaults until it opens)
    o.auto_rejoin = on;
    g_menu.Show(o);   // (the menu's settings, as its own rows change them)
    g_auto_rejoin_on = on;
    LOG_INFO(Core, "coop: multiplayer tab: auto-rejoin {} from the pause menu's Multiplayer page", on ? "on" : "off");
    Save();
}

void JoinLanNow(const std::string& address, std::uint16_t port, const std::string& password) {
    MenuLock lock(__func__);
    FreshMain();
    LOG_INFO(Core, "coop: multiplayer tab: join a game on this network from the pause menu's Multiplayer page");
    if (LauncherLive()) {   // (not in a game: Join() ends it first, row 170)
        SayOnPage(kSayLauncher);
        return;
    }
    // (the same-network line: "<address>:<port> <password>", as the host's Copy network address gives it)
    const std::string line = address + ":" + std::to_string(port) + (password.empty() ? std::string() : " " + password);
    if (!Join(line)) SayOnPage(kSayRefused);
}

bool RelaySet() {
    // (the files' settings: the menu's own are its defaults until it opens; read only while the Join view shows)
    return !ReadOptions().relay.empty();
}

void JoinRoomNow(const std::string& room, const std::string& password) {
    MenuLock lock(__func__);
    FreshMain();
    LOG_INFO(Core, "coop: multiplayer tab: join a room by its name from the pause menu's Multiplayer page");
    if (LauncherLive()) {   // (not in a game: Join() ends it first, row 170)
        SayOnPage(kSayLauncher);
        return;
    }
    const std::string relay = g_menu.options().relay;
    if (relay.empty()) {
        g_menu.SetNote({"No relay is set in coop-options.ini (relay = ...), so a room name has nowhere to be found. "
                        "Ask your host for their whole room@relay line and paste it."});
        SayOnPage(kSayRefused);
        return;
    }
    // (the line friends type into Join: "<room>@<relay> <password>", docs/play-online.md)
    if (!Join(room + "@" + relay + (password.empty() ? std::string() : " " + password))) SayOnPage(kSayRefused);
}

bool Rejoin(std::string* host) {
    const std::string coop = ReadText(CoopIni());
    const auto code = CoopTitleMenu::IniGet(coop, "last_invite");
    if (host != nullptr) *host = CoopTitleMenu::IniGet(coop, "last_host").value_or("");
    return code && !code->empty();
}

void RejoinNow() {
    MenuLock lock(__func__);
    FreshMain();
    LOG_INFO(Core, "coop: multiplayer tab: the last game again from the pause menu's Multiplayer page");
    const auto code = CoopTitleMenu::IniGet(ReadText(CoopIni()), "last_invite");
    const bool live = LauncherLive();   // (not in a game: Join() ends it first, row 170)
    if (live || !code || code->empty()) {
        if (!live) g_menu.SetNote({"No last game to join again: paste your friend's code."});
        SayOnPage(live ? kSayLauncher : kSayRefused);
        return;
    }
    if (!Join(*code)) SayOnPage(kSayRefused);
}

void PasteAnswerNow() {
    MenuLock lock(__func__);
    FreshMain();   // (first: opening clears the page's note)
    LOG_INFO(Core, "coop: multiplayer tab: paste answer from the pause menu's Multiplayer page");
    Act(Request{Request::Kind::kPasteAnswer, {}});
    SayOnPage("The answer code was taken.");
}

namespace {

// The page's own note (PageCopyNow's result) and when it was set; under g_mutex.
std::string g_page_note;
std::uint64_t g_page_note_ms = 0;
inline constexpr std::uint64_t kPageNoteMs = 4000;

void SetPageNote(std::string note) {
    g_page_note = std::move(note);
    g_page_note_ms = NowMs();
}

}  // namespace

void PageCopyNow(PageCopy what) {
    MenuLock lock(__func__);
    const HosCoopStatus st = Status();
    std::string text;
    std::string words;
    switch (what) {
    case PageCopy::kCode:
        // (a relay room's name first: friends type it; then the relay's join code, then the invite code)
        text = ShareCode(st, true);   // (a relay line with the join password after it: a friend's paste joins at once)
        words = st.relay_room[0] != 0 ? "room name" : st.relay_code[0] != 0 ? "join code" : "invite code";
        break;
    case PageCopy::kAnswer:
        text = st.answer;
        words = "answer code";
        break;
    case PageCopy::kLan:
    case PageCopy::kVpn: {
        const bool vpn = what == PageCopy::kVpn;
        if (st.state != HOS_COOP_HOSTING || st.port == 0) {
            SetPageNote("The server is not up yet");
            return;
        }
        // (as the menu's Same network / VPN rows: the first address of that kind, the port and the password)
        const auto cards = CoopFiles::KeptLocalCards(NowMs());   // (fps-spike-0121: read off the frame)
        if (!cards) {
            SetPageNote(kNetworksReading);
            return;
        }
        const CoopTitleMenu::LanSplit split = CoopTitleMenu::SplitLan(*cards);
        const auto& addresses = vpn ? split.vpn : split.home;
        if (addresses.empty()) {
            SetPageNote(vpn ? "No VPN address found" : "No network address found");
            return;
        }
        text = CoopTitleMenu::LanJoinText(
            addresses.front().address, st.port,
            CoopTitleMenu::LanLinePassword(addresses.front().address, st.password, st.same_network_open != 0));
        words = vpn ? "VPN line" : "network line";
        break;
    }
    }
    if (text.empty()) {
        SetPageNote("No " + words + " yet");
        return;
    }
    LOG_INFO(Core, "coop: multiplayer tab: the {} copied from the pause menu's Multiplayer page", words);
    const std::uint64_t session = g_session;
    // (on Linux the result comes a moment later, under g_mutex: a Host or Join started meanwhile drops it)
    CopyThen(text, [session, words](bool ok) {
        if (session != g_session) return;
        SetPageNote(ok ? "Copied the " + words : "No clipboard here");   // (the rows fit 24 letters: kRowChars)
    });
}

std::string PageNote() {
    MenuLock lock(__func__);
    return NowMs() - g_page_note_ms < kPageNoteMs ? g_page_note : std::string();
}

std::string PageSays(std::uint64_t* gen) {
    MenuLock lock(__func__);
    if (gen != nullptr) *gen = g_page_says_gen;
    return g_page_says;
}

// The autotest's in-game host (CoopHostMenu::ParseTestHost): the Multiplayer page's Host, on the harness's port and
// password, a private game with no router port or invite, and no relay unless the harness names its relay room. The host thread listens on 127.0.0.1 only.
bool TestHost() {
    const char* text = std::getenv(CoopHostMenu::kTestHostEnv);
    const auto test = text != nullptr ? CoopHostMenu::ParseTestHost(text) : std::nullopt;
    if (!test) {
        LOG_WARNING(Core, "coop: multiplayer tab: test host: {} is not PORT:PASSWORD: not hosting", CoopHostMenu::kTestHostEnv);
        return false;
    }
    MenuLock lock(__func__);
    FreshMain();
    // (as the page's Host row, HostNow: refused only while that co-op is in a game; a server line naming no server
    // running, row 170, is ended by Host() and the test hosts, as a player's press there does)
    if (LauncherLive()) {
        LOG_WARNING(Core, "coop: multiplayer tab: test host: co-op came with the game's start and is in a game: not hosting");
        return false;
    }
    CoopTitleMenu::Options o = g_menu.options();
    o.host_port = test->first;
    o.password = test->second;
    o.public_game = false;
    o.approve_joins = false;
    o.relay.clear();
    o.relay_key.clear();
    o.relay_room.clear();
    o.list_room = false;
    // (the harness's relay room, both or neither: a private room on its loopback relay, as a player's Room name row)
    const char* relay = std::getenv(CoopHostMenu::kTestRelayEnv);
    const char* room = std::getenv(CoopHostMenu::kTestRelayRoomEnv);
    if (relay != nullptr || room != nullptr) {
        if (relay == nullptr || room == nullptr || *relay == 0 || !CoopTitleMenu::RelayOk(relay) ||
            !CoopTitleMenu::RoomNameOk(room)) {
            LOG_WARNING(Core, "coop: multiplayer tab: test host: {} and {} want a relay host:port and a room name: not hosting",
                        CoopHostMenu::kTestRelayEnv, CoopHostMenu::kTestRelayRoomEnv);
            return false;
        }
        o.relay = relay;
        o.relay_room = room;
        LOG_INFO(Core, "coop: multiplayer tab: test host: private room {} on relay {}", o.relay_room, o.relay);
    }
    LOG_INFO(Core, "coop: multiplayer tab: test host on port {} (Multiplayer > Host, as the page's Host row)", o.host_port);
    return Host(o, true);
}

std::string MenuState() {
    MenuLock lock(__func__);
    if (!g_shown && !g_native) return "closed";
    return CoopPauseMenu::PageWord(g_menu.page());
}

CoopMenuState::Tab MenuTab() {
    MenuLock lock(__func__);
    CoopMenuState::Tab tab;
    if (!g_shown && !g_native) return tab;
    tab.open = true;
    tab.page = CoopPauseMenu::PageWord(g_menu.page());
    tab.view = g_menu.view();
    return tab;
}

// A test's page opened where a player has it now (C #145), by F1's rule (coop_menu_tabs_rule.h OnF1, the same parts as
// coop_hookshot.cpp MenuNow): in free play co-op's home is the game's - menu, so the Multiplayer tab there on the page's
// view (CoopPauseTab::OpenMultiplayerOn, F1's own run); elsewhere (the title, an event, a load) or with a pack without
// our - tabs, Longshot's menu on co-op's tab (Open). The f9f3b9c26 gate: in free play Open's want was lost, because
// Longshot's menu carries no co-op tab then (LongshotTabs 0) and takes a want only below its tab count (Longshot
// gpu/vk/vk_menu.cpp coop_want < coop_tabs), so host170's and test_menu_pages' pages never came.
static void OpenAsPlayer(CoopPauseMenu::Page page) {
    static const bool has_pack = CoopPuppetConfig::HasPack();
    CoopMenuTabsRule::Now now;
    now.minus_tabs = has_pack && !CoopPauseTab::NoMinusTabs();
    now.minus_now = now.minus_tabs && CoopPauseTab::MinusMenuNow();
    now.tab_shown = TabShown();
    if (CoopMenuTabsRule::OnF1(now) == CoopMenuTabsRule::F1::kMinusMenu && CoopPauseTab::OpenMultiplayerOn(page)) {
        LOG_INFO(Core, "coop: test menu: {}: the - menu's Multiplayer tab (free play: co-op's home there)",
                 CoopPauseMenu::PageWord(page));
        return;
    }
    // (why not the - menu, for the test's log: gate-ui-epona-0121 had only 'opened on a page' to go by)
    LOG_INFO(Core, "coop: test menu: {}: Longshot's menu on co-op's tab (our - tabs {}, the - menu can open now {}, "
                   "a co-op tab shows {})",
             CoopPauseMenu::PageWord(page), now.minus_tabs ? "yes" : "no", now.minus_now ? "yes" : "no",
             now.tab_shown ? "yes" : "no");
    Open(page);
}

bool TestMenu(const std::string& page) {
    if (page == "host") {
        return TestHost();
    }
    if (page == "paste") {   // (the Multiplayer page's "Paste code and join", with this PC's clipboard)
        // (the row is on the Multiplayer page: a player has it open, and the refusal note is drawn there, so the test
        // opens it too; the note is then on screen for a shot)
        OpenAsPlayer(CoopPauseMenu::Page::kMain);
        JoinPasteNow();
        return true;
    }
    if (page == "customize") {
        OpenAsPlayer(CoopPauseMenu::Page::kCustomize);
        return true;
    }
    if (page == "main") {   // (the Multiplayer page opened, nothing pressed: its rows for a shot, row 170)
        OpenAsPlayer(CoopPauseMenu::Page::kMain);
        return true;
    }
    // `menu open PAGE` (B #138, port-g7; CoopTestInput::kMenuPages): any page of the tab opened and nothing pressed, for
    // its shot, by the path the title's rows open a page (coop_title.cpp, kRowPages -> CoopPause::Open): Menu::Open, the
    // main page and then the page's own row decided, as a player's press of it (coop_pause_menu.cpp Menu::Open).
    if (page.rfind("open ", 0) == 0) {
        static const std::pair<std::string_view, CoopPauseMenu::Page> kPages[] = {
            {"main", CoopPauseMenu::Page::kMain},         {"host", CoopPauseMenu::Page::kHost},
            {"join", CoopPauseMenu::Page::kJoin},         {"local", CoopPauseMenu::Page::kLocal},
            {"settings", CoopPauseMenu::Page::kSettings}, {"commands", CoopPauseMenu::Page::kCommands},
            {"customize", CoopPauseMenu::Page::kCustomize}, {"find", CoopPauseMenu::Page::kFind}};
        const std::string_view which = std::string_view(page).substr(5);
        for (const auto& [word, at] : kPages) {
            if (which == word) {
                OpenAsPlayer(at);
                return true;
            }
        }
        return false;
    }
    if (page == "close") {
        g_wanted = HOS_COOP_MENU_WANT_CLOSE;
        return true;
    }
    if (page == "leave") {   // (remount-leave: the Multiplayer page's Leave row, as a player leaves)
        LeaveNow();
        return true;
    }
    // gear-link: "look KEY=VALUE" (coop_testinput's `menu look`): that part chosen as the Customize page chooses a
    // look (ChooseLookParts' SetPending) and worn as when the menu closes (ApplyOnUnpause); VALUE "none" unsets it.
    if (page.rfind("look ", 0) == 0) {
        const std::string spec = page.substr(5);
        const std::size_t eq = spec.find('=');
        if (eq == std::string::npos) return false;
        const std::string key = spec.substr(0, eq), value = spec.substr(eq + 1);
        for (const CoopLook::PartInfo& info : CoopLook::Parts()) {
            if (info.part == CoopLook::Part::tunic || key != info.key) continue;
            CoopLook::Look look = CoopLook::Shown();
            if (value == "none") {
                look[info.part] = std::nullopt;
            } else {
                std::vector<std::string> errors;
                const CoopLook::Look one = CoopLook::ReadParts("look_" + key + " = " + value + "\n", &errors);
                if (!one[info.part]) return false;
                look[info.part] = one[info.part];
            }
            ChooseLookParts(look);
            WearChosenLook();
            return true;
        }
        return false;
    }
    return false;
}

}  // namespace CoopPause
