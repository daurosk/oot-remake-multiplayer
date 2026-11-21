// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_pause.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common/logging.h"
#include "coop/protocol/messages.hpp"
#include "coop_auto_rejoin.h"
#include "coop_colors.h"
#include "coop_hook.h"
#include "coop_hookshot.h"
#include "coop_host_menu.h"
#include "coop_i18n.h"
#include "coop_input.h"
#include "coop_lan.h"
#include "coop_lan_net.h"
#include "coop_lobby_game.h"
#include "coop_lobby_tab.h"
#include "coop_look.h"
#include "coop_look_view.h"
#include "coop_mods_tab.h"
#include "coop_pause_files.h"
#include "coop_ngplus.h"
#include "coop_save.h"
#include <cstdlib>
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
bool g_native = false;       // the title's own rows show its pages instead (OpenNative): no overlay
std::string g_doing;         // what the tab started: "hosting", "joined", "" none
bool g_hosted_public = false;  // the game hosting now was started as a public game (the Host page's Game row)
// Auto-rejoin (coop_auto_rejoin.h): the planner, and what the player did since its last update (its own Host or Join,
// a Leave), which stop what it planned; g_auto_join: the join being made is the planner's, not the player's.
CoopAutoRejoin::Planner g_rejoin;
bool g_player_acted = false, g_player_left = false, g_auto_join = false;
std::uint64_t g_last_game_ms = 0;   // when coop.ini's last_invite and the setting were last read
bool g_last_game = false;           // a last game is kept (coop.ini last_invite)
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

HosCoopStatus Status() {
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
    std::vector<std::string> lines = CoopTitleMenu::PlainStatus(v);
    if (v.host && v.state == HOS_COOP_HOSTING && st.relay_room[0] != '\0') {
        lines.push_back(std::string("Room ") + st.relay_room +
                        ": friends type it in Join a game (IP address), with your join password.");
    }
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
        LOG_INFO(Core, "coop: multiplayer tab: joined at the title: load a save once the chat says Success");
    } else {
        LOG_INFO(Core, "coop: multiplayer tab: joined in the game: the host's save is used from the title on");
    }
}

// What co-op is doing, for the tab.
CoopPauseMenu::Session SessionNow() {
    const HosCoopStatus st = Status();
    const CoopLobby::Seen seen = CoopLobby::Now();   // what the Lobby tab shows too
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
    s.by_launcher = g_coop_on;
    if (s.hosting) {
        s.players = st.players;
        s.code = st.invite;
    } else {
        s.players = static_cast<int>(CoopHook::PlayerList().size());
        s.code = st.state == HOS_COOP_WAIT_HOST ? st.answer : "";
    }
    s.lines = StatusLines(st, seen);
    return s;
}

void RefreshSession(bool now) {
    if (now || NowMs() >= g_next_session_ms) {
        g_next_session_ms = NowMs() + 250;
        g_menu.SetSession(SessionNow());
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

// What follows from co-op's state, whether the tab is shown or not: the invite code on the clipboard once there is
// one (a guest's answer code too), the message logged, a connected join remembered and the host's save loaded.
void Follow(const HosCoopStatus& st) {
    const bool host = g_doing == "hosting";
    const std::uint64_t session = g_session;
    if (host && st.invite[0] != 0 && !g_copied && !g_copy_tried) {
#ifndef _WIN32
        g_copy_tried = true;   // one try per code (Windows tries again each round: a busy clipboard frees up)
#endif
        CopyThen(st.invite, [session](bool ok) {
            if (session != g_session) {
                return;
            }
            g_copied = ok;
            LOG_INFO(Core, "coop: multiplayer tab: host: the invite code is on the clipboard{}", ok ? "" : " (it could not be)");
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
        if (g_doing != "joined" || CoopLobby::Now().connected) {
            g_room_fallback.clear();
        } else if (NowMs() - g_room_fallback_ms >= kPcTryMs) {
            const std::string line = std::exchange(g_room_fallback, std::string());
            const auto bare = CoopTitleMenu::BareJoinName(line.substr(0, line.find('@')));
            LOG_INFO(Core, "coop: multiplayer tab: join: no game answered at that PC: as a room on the relay");
            CoopHookshot::HostMenu().Leave();
            g_doing.clear();
            g_menu.SetNote({"No game answered at a PC called " + bare.value_or("that") + ": trying the room " +
                            bare.value_or("") + " on your relay."});
            Join(line);
            return;
        }
    }
    // Once the session took this game in: the host's name is known then (PlayerList).
    if (g_doing == "joined" && st.state == HOS_COOP_CONNECTED && !g_join_saved && !g_join_text.empty() &&
        CoopLobby::Now().connected) {
        Joined();
    }
    const std::uint64_t now = NowMs();
    // Games on this network (coop_lan.h): a hosting game says it is there (its name, port, players, room; never the
    // join password), every CoopLan::kEveryMs.
    if (host && st.state == HOS_COOP_HOSTING) {
        const CoopTitleMenu::Options& o = g_menu.options();
        CoopLan::Game g;
        g.name = o.name;
        for (const auto& row : CoopHook::PlayerList()) {   // (the name co-op plays this game under, once it is in)
            if (row.self && !row.name.empty()) g.name = row.name;
        }
        g.port = o.host_port != 0 ? o.host_port : CoopTitleMenu::kDefaultPort;
        const CoopLobby::Seen seen = CoopLobby::Now();
        g.players = seen.connected ? static_cast<int>(seen.players.size()) : 1;
        g.max = o.max_players;
        std::string room = st.relay_room;
        if (const auto at = room.find('@'); at != std::string::npos) room.resize(at);
        g.room = room;
        g.protocol = coop::protocol::kProtocolVersion;
        static const std::string id = CoopLan::NewId();   // (one for this game's run: one row however many networks)
        g.id = id;
        CoopLanNet::Announce(g, now);
    }
    // Auto-rejoin (coop_auto_rejoin.h): the last friend's game again, at the start and after a drop.
    if (g_last_game_ms == 0 || now - g_last_game_ms > 5000) {   // (coop.ini read at most every 5 s)
        g_last_game_ms = now;
        const auto code = CoopTitleMenu::IniGet(ReadText(CoopIni()), "last_invite");
        g_last_game = code && !code->empty();
        g_auto_rejoin_on = ReadOptions().auto_rejoin;
    }
    CoopAutoRejoin::Facts f;
    f.enabled = g_auto_rejoin_on;
    f.code = g_last_game;
    f.launcher = g_coop_on;
    f.idle = !Busy();
    f.guest_in = g_doing == "joined" && st.state == HOS_COOP_CONNECTED;
    f.player_left = std::exchange(g_player_left, false);
    f.player_acted = std::exchange(g_player_acted, false);
    {   // (a refusal counted since the last update: CoopLobby's count only grows, so none is missed)
        static std::uint32_t refusals_seen = 0;
        const std::uint32_t refusals = CoopLobby::Now().refusals;
        f.turned_away = refusals != refusals_seen;
        refusals_seen = refusals;
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
std::uint64_t g_next_follow_ms = 0;

// Host and Join through the module's own host (native-host-b, hos_coop_host.h): the co-op server or the
// invite code's join helper on the module's "coop host" thread, and this game's client connected to it.
bool Busy() {
    const HosCoopStatus st = Status();
    return g_coop_on || (st.state != HOS_COOP_IDLE && st.state != HOS_COOP_FAILED);
}

void Save();

// False when it did not start: refused (co-op is on already; the menu's note says so) or the host could not start
// (the session's lines say why).
bool Host() {
    g_player_acted = true;
    if (g_doing == "hosting" && !g_coop_on) {
        Save();
        CoopHookshot::HostMenu().Leave();
        g_doing.clear();
    } else if (Busy()) {
        g_menu.SetNote({"Co-op is on already in this game: stop it first, or start the game again."});
        return false;
    }
    CoopHookshot::HostMenu().Leave();   // (a failed start: clear it)
    const CoopTitleMenu::Options o = g_menu.options();
    CoopFiles::SetCoopEnvironment(o);
    HosCoopHostOptions h{};
    h.size = sizeof h;
    h.name = o.name.c_str();
    h.password = o.password.empty() ? nullptr : o.password.c_str();
    h.port = o.host_port;
    h.upnp = 1;             // the router forward (nothing to set up)
    h.invite = 1;
    h.pvp = o.pvp ? 1 : 0;
    h.approve_joins = o.approve_joins ? 1 : 0;
    h.same_network_open = o.same_network_open ? 1 : 0;   // (off, the default: this network's players need the password too)
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
    if (!g_auto_join) g_player_acted = true;
    if (Busy()) {
        g_menu.SetNote({"Co-op is on already in this game: stop it first, or start the game again."});
        return false;
    }
    namespace nat = coop::server::nat;
    // A bare name ("mypc", "forest-gang"): a PC on the home network first, then a room on the relay.
    std::string text = text_in;
    std::string fallback;   // (a bare name joined as a PC: its room, should no game answer there)
    const auto name = CoopTitleMenu::BareJoinName(text);
    const std::string relay(CoopTitleMenu::Trim(g_menu.options().relay));
    if (name && !nat::looks_like_code(text) && PcFound(*name)) {
        if (!relay.empty()) fallback = CoopTitleMenu::RoomJoinLine(text, relay);
    } else if (name && !nat::looks_like_code(text)) {
        if (relay.empty()) {
            g_menu.SetNote({"No PC called " + *name + " was found. To join a room by its name, set a relay first: "
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
    HosCoopJoinOptions j{};
    j.size = sizeof j;
    j.name = o.name.c_str();
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

// The Settings page's Microphone row (voice on): the microphones there now, listed only while voice is on (none
// opened); none when this game cannot choose one (an older patch 0009).
void MicRows() {
    g_menu.SetMicrophones(g_menu.options().voice ? CoopVoice::Microphones() : std::nullopt);
}

void Save() {
    const auto kit = FindKit();
    std::string coop = ReadText(CoopIni());
    std::string options = kit ? ReadText(*kit / "coop-options.ini") : std::string();
    g_menu.options().Write(coop, options);
    bool ok = WriteText(CoopIni(), coop);
    if (kit) {
        ok = WriteText(*kit / "coop-options.ini", options) && ok;
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
    const auto kit = FindKit();
    if (!kit) {
        g_menu.SetNote({"No co-op kit found with this game: the choice counts until the game closes."});
        return;
    }
    const auto file = *kit / "coop-options.ini";
    const std::string before = ReadText(file);
    const std::string after =
        usual ? CoopTitleMenu::IniUnset(before, key) : CoopTitleMenu::IniSet(before, key, value, false);
    if (!WriteText(file, after)) {
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
    const CoopTitleMenu::LanSplit split = CoopTitleMenu::SplitLan(CoopFiles::LocalCards());
    const auto& addresses = vpn ? split.vpn : split.home;
    const std::uint64_t session = g_session;
    const auto show = [vpn, session, addresses, port = st.port, password = std::string(st.password)](bool copied) {
        if (session != g_session) {
            return;   // (hosted again or joined since: these lines are old)
        }
        g_lan_password = password;
        g_lan_lines = vpn ? CoopTitleMenu::VpnLines(addresses, port, password, copied)
                          : CoopTitleMenu::HomeLines(addresses, port, password, copied);
        LOG_INFO(Core, "coop: multiplayer tab: host: {}: {} address(es){}", vpn ? "VPN" : "same network",
                 addresses.size(), copied ? ", the join line is on the clipboard" : "");
        RefreshSession(true);
    };
    if (addresses.empty()) {
        show(false);
        return;
    }
    CopyThen(CoopTitleMenu::LanJoinText(addresses.front().address, st.port, st.password), show);
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
    g_menu.Show(ReadOptions());
    g_menu.SetLook(CoopLook::Shown());   // the Customize page opens on the look waiting, else the one worn
    g_menu.SetNgPlus(CoopNgPlus::Level(), CoopNgPlus::Unlocked(), CoopNgPlus::Hearts());   // (Navi) New Game+
    g_menu.SetSaveSet(CoopSave::MultiplayerSaves());   // (Navi) the Saves row
    const std::string coop = ReadText(CoopIni());
    const auto last = CoopTitleMenu::IniGet(coop, "last_invite");
    g_menu.SetRejoin(CoopTitleMenu::IniGet(coop, "last_host").value_or(""), last.value_or(""));
    InputRows();   // (a controller plugged in while the menu was closed: its name)
    MicRows();     // (and a microphone plugged in)
    RefreshSession(true);
}

// The page for Hookshot, kept until this thread's next call (hos_coop_menu.h).
struct Drawn {
    CoopPauseMenu::View view;
    std::vector<HosCoopMenuRow> rows;
};

// The page's text in the player's language (coop_i18n.h). Hookshot keeps the
// pointers until its next call, so they point into this thread's copy; it is
// reserved up front so no push moves a string an earlier row points at.
thread_local std::vector<std::string> g_page_text;
const char* Keep(const std::string& text) {
    g_page_text.push_back(CoopI18n::Tr(text));
    return g_page_text.back().c_str();
}

// The page's prompt words ("Choose", "Close"...). A null pointer means "no
// prompt"; an empty one is a prompt with no words, which Hookshot reads as its
// own default, so the words go through Keep like the rest.
const char* KeepWords(const std::string& text) {
    return text.empty() ? nullptr : Keep(text);
}

// Another tab's page (Mods, Lobby) in the player's language: its rows copied
// with their words looked up, as the Multiplayer page's (a null stays null;
// the hint, a code or a path, as it is).
bool Translated(bool filled, HosCoopMenuPage* out) {
    if (!filled || out == nullptr || CoopI18n::Current() == nullptr) {
        return filled;
    }
    thread_local std::vector<HosCoopMenuRow> rows;
    const int n = out->rows != nullptr && out->rows_n > 0 ? out->rows_n : 0;
    rows.assign(out->rows, out->rows + n);
    g_page_text.clear();
    g_page_text.reserve(std::size_t(n) * 2 + 16);
    const auto keep = [](const char* text) { return text == nullptr ? nullptr : Keep(text); };
    for (HosCoopMenuRow& r : rows) {
        r.name = keep(r.name);
        r.value = keep(r.value);
    }
    out->rows = n > 0 ? rows.data() : out->rows;
    out->help_title = keep(out->help_title);
    out->help = keep(out->help);
    out->gold = keep(out->gold);
    out->change = keep(out->change);
    out->confirm = keep(out->confirm);
    out->back = keep(out->back);
    return true;
}

}  // namespace

void Init(bool coop_on) {
    std::lock_guard lock(g_mutex);
    g_coop_on = coop_on;
    LOG_INFO(Core, "coop: multiplayer tab: ready (the first tab of Hookshot's menu: F1, or Home on a pad){}",
             coop_on ? "; co-op came with the game's start" : "");
}

void Tick() {
    std::lock_guard lock(g_mutex);
    Delivered();   // (each frame: a finished copy or paste shows at once)
    const std::uint64_t now = NowMs();
    if (now < g_next_follow_ms) {
        return;
    }
    g_next_follow_ms = now + 250;
    Follow(Status());
}

// Tab 1: the Lobby (coop_lobby_tab.h), always: who is in the game, the host's controls.
// Tab 2: Mods (coop_mods_tab.h), once the mod loader runs.
constexpr int kLobbyTab = 1;
constexpr int kModsTab = 2;

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

bool Page(int tab, HosCoopMenuPage* out) {
    if (tab == kModsTab) {
        return Translated(CoopModsTab::Page(out), out);
    }
    if (tab == kLobbyTab) {
        return Translated(CoopLobbyTab::Page(out), out);
    }
    if (tab != kMultiplayerTab || out == nullptr || out->size < sizeof(HosCoopMenuPage)) {
        return false;
    }
    thread_local Drawn drawn;
    {
        std::lock_guard lock(g_mutex);
        RefreshSession(false);   // (reads only: no file is written from Hookshot's present thread)
        drawn.view = g_menu.view();
    }
    const CoopPauseMenu::View& v = drawn.view;
    g_page_text.clear();
    g_page_text.reserve(v.rows.size() * 3 + 16);   // (+ the help, the gold line and the prompts)
    drawn.rows.resize(v.rows.size());
    for (std::size_t i = 0; i < v.rows.size(); ++i) {
        const CoopPauseMenu::Row& r = v.rows[i];
        HosCoopMenuRow& h = drawn.rows[i];
        h = HosCoopMenuRow{};
        h.name = Keep(r.name);
        h.value = Keep(r.value);
        h.kind = static_cast<std::uint8_t>(r.kind);
        h.on = r.on ? 1 : 0;
        h.left = r.left ? 1 : 0;
        h.right = r.right ? 1 : 0;
        h.fixed = r.fixed ? 1 : 0;
    }
    out->rows_n = static_cast<std::int32_t>(drawn.rows.size());
    out->rows = drawn.rows.data();
    out->selected = v.selected;
    out->typing = v.typing ? 1 : 0;
    out->help_title = Keep(v.help_title);
    out->help = Keep(v.help);
    out->hint = v.hint.c_str();   // (a code or an address: never translated)
    out->gold = Keep(v.gold);
    out->change = KeepWords(v.change);
    out->confirm = KeepWords(v.confirm);
    out->back = KeepWords(v.back);
    return true;
}

bool Picture(int tab, HosCoopMenuPicture* out) {
    if (tab != kMultiplayerTab || out == nullptr || out->size < sizeof(HosCoopMenuPicture)) {
        return false;
    }
    thread_local std::vector<HosCoopMenuShape> shapes;   // (Hookshot copies them before the next call on its thread)
    std::vector<CoopLookMenu::Shape> picture;
    {
        std::lock_guard lock(g_mutex);
        picture = g_menu.Picture();
    }
    shapes.clear();
    if (picture.empty()) {
        return false;   // (not the Customize page)
    }
    // Your own Link in the game, through a clear window, the camera around him (coop_look_view.h); the flat picture
    // when he cannot be shown (the title screen, a cutscene).
    const CoopLookView::Window window{out->window[0], out->window[1], out->window[2], out->window[3], out->aspect};
    if (window.Valid() && CoopLookView::LinkShown()) {
        CoopLookView::Ask(window);
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

// The Mods tab's paste: the clipboard (PasteThen: on Linux it may come a few frames later, then on Tick) goes to
// the field that asked, if the player is still on it (Tab::Paste drops it otherwise). Under g_mutex, as the
// Multiplayer tab's own paste is; the tab's own lock is taken inside, never the other way round.
static void ModsPaste(bool into_typing) {
    std::lock_guard lock(g_mutex);
    PasteThen([into_typing](std::optional<std::string> clip) { CoopModsTab::Paste(clip, into_typing); });
}

bool Input(int tab, int event, int arg, int arg2) {
    if (tab == kModsTab) {
        const bool taken = CoopModsTab::MenuInput(event, arg, arg2);
        if (CoopModsTab::TakePasteRequest()) {   // a text row's Paste (pad: Left / Right; mouse: its sides)
            ModsPaste(false);
        }
        return taken;
    }
    if (tab == kLobbyTab) {
        return CoopLobbyTab::MenuInput(event, arg, arg2);
    }
    if (tab != kMultiplayerTab) {
        return false;
    }
    std::lock_guard lock(g_mutex);
    bool taken = true;
    switch (event) {
    case HOS_COOP_MENU_UP: g_menu.Press(Input::kUp); break;
    case HOS_COOP_MENU_DOWN: g_menu.Press(Input::kDown); break;
    case HOS_COOP_MENU_LEFT: g_menu.Press(Input::kLeft); break;
    case HOS_COOP_MENU_RIGHT: g_menu.Press(Input::kRight); break;
    case HOS_COOP_MENU_CONFIRM: g_menu.Press(Input::kDecide); break;
    case HOS_COOP_MENU_BACK: taken = g_menu.Press(Input::kBack); break;
    case HOS_COOP_MENU_SHOW: ShowTab(); break;
    case HOS_COOP_MENU_HIDE:
        g_shown = false;
        g_menu.Hide();
        WearChosenLook();
        break;
    case HOS_COOP_MENU_SELECT:
        if (arg >= 0) {
            g_menu.Select(static_cast<std::size_t>(arg));
        }
        break;
    case HOS_COOP_MENU_CLICK:
        if (arg >= 0) {
            g_menu.Click(static_cast<std::size_t>(arg), arg2 < 0 ? -1 : arg2 > 0 ? 1 : 0);
        }
        break;
    default: return false;
    }
    Settle();
    return taken;
}

int Wanted() {
    return g_wanted.exchange(HOS_COOP_MENU_WANT_NOTHING);
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
    std::lock_guard lock(g_mutex);
    if (!g_shown && !g_native) {
        return false;
    }
    const auto [ctrl_down, shift_down] = Modifiers();
    // Ctrl+V and Shift+Insert paste, typing or not: on the Host page that is Paste friend's answer (players press
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
    std::lock_guard lock(g_mutex);
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
    std::lock_guard lock(g_mutex);
    return (g_shown || g_native) && g_menu.typing();
}

void OpenNative(CoopPauseMenu::Page page) {
    {
        std::lock_guard lock(g_mutex);
        g_native = true;
    }
    Open(page);
    g_wanted = HOS_COOP_MENU_WANT_NOTHING;   // (no overlay: the title's own rows show the page)
    LOG_INFO(Core, "coop: multiplayer tab: its page on the title's own rows (no overlay)");
}

bool NativeInput(int event, int arg, int arg2) {
    {
        std::lock_guard lock(g_mutex);
        if (!g_native) return false;
    }
    return CoopPause::Input(kMultiplayerTab, event, arg, arg2);
}

CoopPauseMenu::View NativeView() {
    std::lock_guard lock(g_mutex);
    return g_native ? g_menu.view() : CoopPauseMenu::View{};
}

CoopPauseMenu::Page NativePage() {
    std::lock_guard lock(g_mutex);
    return g_menu.page();
}

bool NativeOpen() {
    std::lock_guard lock(g_mutex);
    return g_native;
}

void CloseNative() {
    std::lock_guard lock(g_mutex);
    if (!g_native) return;
    g_native = false;
    if (!g_shown) g_menu.Hide();
}

void Open(CoopPauseMenu::Page page) {
    std::lock_guard lock(g_mutex);
    g_menu.Show(ReadOptions());
    g_menu.Open(page);
    if (page == CoopPauseMenu::Page::kJoin) {
        const std::string coop = ReadText(CoopIni());
        const auto last = CoopTitleMenu::IniGet(coop, "last_invite");
        g_menu.SetRejoin(CoopTitleMenu::IniGet(coop, "last_host").value_or(""), last.value_or(""));
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
    std::lock_guard lock(g_mutex);
    return g_coop_on;
}

void HostNow() {
    std::lock_guard lock(g_mutex);
    FreshMain();
    LOG_INFO(Core, "coop: multiplayer tab: host from the pause menu's Multiplayer page");
    if (g_coop_on) {   // (the main page's Host row says why: kByLauncher)
        LOG_INFO(Core, "coop: multiplayer tab: not hosting: co-op came with the game's start");
        SayOnPage(kSayLauncher);
        return;
    }
    if (!Host()) {
        SayOnPage(kSayRefused);
    }
}

void LeaveNow() {
    std::lock_guard lock(g_mutex);
    if (g_coop_on) {   // (as the menu: no Leave row for it; its main page says to start the game again)
        FreshMain();
        LOG_INFO(Core, "coop: multiplayer tab: not leaving: co-op came with the game's start");
        SayOnPage(kSayLauncher);
        return;
    }
    Act(Request{Request::Kind::kLeave, {}});
}

void JoinPasteNow() {
    std::lock_guard lock(g_mutex);
    FreshMain();
    LOG_INFO(Core, "coop: multiplayer tab: join with the clipboard's code from the pause menu's Multiplayer page");
    if (g_coop_on) {
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
        if (!Join(line)) SayOnPage(kSayRefused);
    });
}

bool AutoRejoin() {
    std::lock_guard lock(g_mutex);
    return g_auto_rejoin_on;
}

std::string Relay() {
    return ReadOptions().relay;
}

bool ListRoom() {
    return ReadOptions().list_room;   // (the files' setting: the menu's are defaults until it opens)
}

void SetListRoom(bool on) {
    std::lock_guard lock(g_mutex);
    CoopTitleMenu::Options o = ReadOptions();
    o.list_room = on;
    g_menu.Show(o);   // (the menu's settings, as its own rows change them)
    LOG_INFO(Core, "coop: multiplayer tab: this game's room {} the relay's public list when it hosts (from the pause "
                   "menu's Multiplayer page)", on ? "goes on" : "stays off");
    Save();
}

bool AutoRejoining() {
    std::lock_guard lock(g_mutex);
    return g_rejoin.Retrying();
}

void SetAutoRejoin(bool on) {
    std::lock_guard lock(g_mutex);
    CoopTitleMenu::Options o = ReadOptions();   // (the files' settings: the menu's are defaults until it opens)
    o.auto_rejoin = on;
    g_menu.Show(o);   // (the menu's settings, as its own rows change them)
    g_auto_rejoin_on = on;
    LOG_INFO(Core, "coop: multiplayer tab: auto-rejoin {} from the pause menu's Multiplayer page", on ? "on" : "off");
    Save();
}

void JoinLanNow(const std::string& address, std::uint16_t port, const std::string& password) {
    std::lock_guard lock(g_mutex);
    FreshMain();
    LOG_INFO(Core, "coop: multiplayer tab: join a game on this network from the pause menu's Multiplayer page");
    if (g_coop_on) {
        SayOnPage(kSayLauncher);
        return;
    }
    // (the same-network line: "<address>:<port> <password>", as the host's Copy same-network line gives it)
    const std::string line = address + ":" + std::to_string(port) + (password.empty() ? std::string() : " " + password);
    if (!Join(line)) SayOnPage(kSayRefused);
}

bool RelaySet() {
    // (the files' settings: the menu's own are its defaults until it opens; read only while the Join view shows)
    return !ReadOptions().relay.empty();
}

void JoinRoomNow(const std::string& room, const std::string& password) {
    std::lock_guard lock(g_mutex);
    FreshMain();
    LOG_INFO(Core, "coop: multiplayer tab: join a room by its name from the pause menu's Multiplayer page");
    if (g_coop_on) {
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
    std::lock_guard lock(g_mutex);
    FreshMain();
    LOG_INFO(Core, "coop: multiplayer tab: the last game again from the pause menu's Multiplayer page");
    const auto code = CoopTitleMenu::IniGet(ReadText(CoopIni()), "last_invite");
    if (g_coop_on || !code || code->empty()) {
        if (!g_coop_on) g_menu.SetNote({"No last game to join again: paste your friend's code."});
        SayOnPage(g_coop_on ? kSayLauncher : kSayRefused);
        return;
    }
    if (!Join(*code)) SayOnPage(kSayRefused);
}

void PasteAnswerNow() {
    std::lock_guard lock(g_mutex);
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
    std::lock_guard lock(g_mutex);
    const HosCoopStatus st = Status();
    std::string text;
    std::string words;
    switch (what) {
    case PageCopy::kCode:
        // (a relay room's name first: friends type it; then the relay's join code, then the invite code)
        text = st.relay_room[0] != 0 ? st.relay_room : st.relay_code[0] != 0 ? st.relay_code : st.invite;
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
        const CoopTitleMenu::LanSplit split = CoopTitleMenu::SplitLan(CoopFiles::LocalCards());
        const auto& addresses = vpn ? split.vpn : split.home;
        if (addresses.empty()) {
            SetPageNote(vpn ? "No VPN address found" : "No network address found");
            return;
        }
        text = CoopTitleMenu::LanJoinText(addresses.front().address, st.port, st.password);
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
    std::lock_guard lock(g_mutex);
    return NowMs() - g_page_note_ms < kPageNoteMs ? g_page_note : std::string();
}

std::string PageSays(std::uint64_t* gen) {
    std::lock_guard lock(g_mutex);
    if (gen != nullptr) *gen = g_page_says_gen;
    return g_page_says;
}

bool TestMenu(const std::string& page) {
    if (page == "customize") {
        Open(CoopPauseMenu::Page::kCustomize);
        return true;
    }
    if (page == "close") {
        g_wanted = HOS_COOP_MENU_WANT_CLOSE;
        return true;
    }
    return false;
}

}  // namespace CoopPause
