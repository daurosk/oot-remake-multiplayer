// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_pause.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/logging.h"
#include "coop_colors.h"
#include "coop_hook.h"
#include "coop_hookshot.h"
#include "coop_input.h"
#include "coop_lobby_tab.h"
#include "coop_look.h"
#include "coop_mods_tab.h"
#include "coop_pause_files.h"
#include "coop_title.h"
#include "coop_title_menu.h"
#include "coop_voice.h"
#include "hos_coop_host.h"
#include "nat_codes.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
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
std::string g_doing;         // what the tab started: "hosting", "joined", "" none
bool g_copied = false;       // this session's invite code was put on the clipboard
bool g_answer_copied = false;   // this join's answer code (WAIT_HOST) was put on the clipboard
std::string g_answer_note;   // what became of the friend's answer code the host pasted ("" when none)
std::string g_join_text;     // the code (or address) this join used: remembered once connected
bool g_join_saved = false;
std::vector<std::string> g_lan_lines;   // "Same network or VPN"'s lines while they are shown
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
std::vector<std::string> StatusLines(const HosCoopStatus& st) {
    CoopTitleMenu::HostView v;
    v.host = g_doing == "hosting";
    v.state = st.state;
    v.problem = st.problem;
    v.players = st.players;
    v.invite_done = st.invite_done != 0;
    v.invite = st.invite;
    v.answer = st.answer;
    v.copied = g_copied;
    if (v.host && v.state == HOS_COOP_HOSTING && !g_lan_lines.empty()) {
        return g_lan_lines;
    }
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
    if (CoopTitle::StartAutoLoad(host)) {
        g_wanted = HOS_COOP_MENU_WANT_CLOSE;
        LOG_INFO(Core, "coop: multiplayer tab: joined at the title: the host's save loads when it is here");
    } else {
        LOG_INFO(Core, "coop: multiplayer tab: joined in the game: the host's save loads at the title");
    }
}

// What co-op is doing, for the tab.
CoopPauseMenu::Session SessionNow() {
    const HosCoopStatus st = Status();
    CoopPauseMenu::Session s;
    switch (st.state) {
    case HOS_COOP_STARTING: s.state = State::kStarting; break;
    case HOS_COOP_HOSTING: s.state = State::kHosting; break;
    case HOS_COOP_JOINING: s.state = State::kJoining; break;
    case HOS_COOP_WAIT_HOST: s.state = State::kWaitHost; break;
    case HOS_COOP_CONNECTED: s.state = State::kJoined; break;
    case HOS_COOP_FAILED: s.state = State::kFailed; break;
    default: s.state = State::kOff; break;
    }
    s.hosting = g_doing == "hosting";
    s.by_launcher = g_coop_on;
    if (s.hosting) {
        s.players = st.players;
        s.code = st.invite;
    } else {
        s.players = static_cast<int>(CoopHook::PlayerList().size());
        s.code = st.state == HOS_COOP_WAIT_HOST ? st.answer : "";
    }
    s.lines = StatusLines(st);
    return s;
}

void RefreshSession(bool now) {
    if (now || NowMs() >= g_next_session_ms) {
        g_next_session_ms = NowMs() + 250;
        g_menu.SetSession(SessionNow());
    }
}

// What follows from co-op's state, whether the tab is shown or not: the invite code on the clipboard once there is
// one (a guest's answer code too), the message logged, a connected join remembered and the host's save loaded.
void Follow(const HosCoopStatus& st) {
    const bool host = g_doing == "hosting";
    if (host && st.invite[0] != 0 && !g_copied) {
        g_copied = SetClipboardText(st.invite);
        LOG_INFO(Core, "coop: multiplayer tab: host: the invite code is on the clipboard{}", g_copied ? "" : " (it could not be)");
    }
    if (!host && st.state == HOS_COOP_WAIT_HOST && st.answer[0] != 0 && !g_answer_copied) {
        g_answer_copied = SetClipboardText(st.answer);
    }
    if (st.message[0] != 0) {
        static std::string said;
        if (said != st.message) {
            said = st.message;
            LOG_INFO(Core, "coop: multiplayer tab: {}", said);   // (the English detail, for the log)
        }
    }
    if (g_doing == "joined" && st.state == HOS_COOP_CONNECTED && !g_join_saved && !g_join_text.empty()) {
        Joined();
    }
}
std::uint64_t g_next_follow_ms = 0;

// Host and Join through the module's own host (native-host-b, hos_coop_host.h): the co-op server or the
// invite code's join helper on the module's "coop host" thread, and this game's client connected to it.
bool Busy() {
    const HosCoopStatus st = Status();
    return g_coop_on || (st.state != HOS_COOP_IDLE && st.state != HOS_COOP_FAILED);
}

void Host() {
    if (Busy()) {
        g_menu.SetNote({"Co-op is on already in this game: stop it first, or start the game again."});
        return;
    }
    CoopHookshot::HostMenu().Leave();   // (a failed start: clear it)
    const CoopTitleMenu::Options o = g_menu.options();
    CoopFiles::SetCoopEnvironment(o);
    HosCoopHostOptions h{};
    h.size = sizeof h;
    h.name = o.name.c_str();
    h.password = nullptr;   // a new random one, inside the invite code: nobody types it
    h.port = 0;
    h.upnp = 1;             // the router forward (nothing to set up)
    h.invite = 1;
    h.pvp = o.pvp ? 1 : 0;
    h.max_players = static_cast<std::uint8_t>(o.max_players);
    g_doing = "hosting";
    g_copied = false;
    g_lan_lines.clear();
    g_answer_note.clear();
    if (!CoopHookshot::HostMenu().HostStart(&h)) {
        RefreshSession(true);
        return;
    }
    LOG_INFO(Core, "coop: multiplayer tab: host: started (up to {} players)", o.max_players);
    RefreshSession(true);
}

void Join(const std::string& text) {
    if (Busy()) {
        g_menu.SetNote({"Co-op is on already in this game: stop it first, or start the game again."});
        return;
    }
    namespace nat = coop::server::nat;
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
        return;
    }
    CoopHookshot::HostMenu().Leave();
    const CoopTitleMenu::Options o = g_menu.options();
    CoopFiles::SetCoopEnvironment(o);
    const bool invite = std::holds_alternative<CoopTitleMenu::JoinInvite>(target);
    const std::string where = invite ? text : std::get<CoopTitleMenu::JoinAddress>(target).server;
    const std::string password = invite ? std::string() : std::get<CoopTitleMenu::JoinAddress>(target).password;
    HosCoopJoinOptions j{};
    j.size = sizeof j;
    j.name = o.name.c_str();
    j.target = where.c_str();
    j.password = password.empty() ? nullptr : password.c_str();
    g_doing = "joined";
    g_join_text = text;
    g_join_saved = false;
    g_answer_copied = false;
    if (!CoopHookshot::HostMenu().Join(&j)) {
        RefreshSession(true);
        return;
    }
    LOG_INFO(Core, "coop: multiplayer tab: join: {}", invite ? std::string("an invite code") : "the address " + where);
    RefreshSession(true);
}

void Leave() {
    LOG_INFO(Core, "coop: multiplayer tab: {}", g_doing == "hosting" ? "stop hosting" : "leave the friend's game");
    CoopHookshot::HostMenu().Leave();
    CoopTitle::StopAutoLoad();
    g_doing.clear();
    g_lan_lines.clear();
    g_answer_note.clear();
    RefreshSession(true);
    g_menu.SetNote({"Co-op stopped. You play on alone."});
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
    CoopVoice::SetEnabled(g_menu.options().voice);   // voice chat on/off counts at once (ptt when on)
    CoopHookshot::SetChatKey(g_menu.options().chat_key);   // the chat key too
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

void CopyLan() {
    // The address and the join password for a friend on the host's home network or VPN (never logged).
    const HosCoopStatus st = Status();
    if (st.state != HOS_COOP_HOSTING || st.port == 0) {
        g_menu.SetNote({"The server is not running yet."});
        return;
    }
    const auto addresses = CoopTitleMenu::LanAddresses(CoopFiles::LocalIpv4s());
    const bool copied = !addresses.empty() &&
                        SetClipboardText(CoopTitleMenu::LanJoinText(addresses.front().address, st.port, st.password));
    g_lan_lines = CoopTitleMenu::LanLines(addresses, st.port, st.password, copied);
    LOG_INFO(Core, "coop: multiplayer tab: host: same network or VPN: {} address(es){}", addresses.size(),
             copied ? ", the join line is on the clipboard" : "");
    RefreshSession(true);
}

void PasteAnswer() {
    g_lan_lines.clear();
    const std::string clip = ClipboardText();
    if (!coop::server::nat::looks_like_code(clip)) {
        g_answer_note.clear();
        g_menu.SetNote({"Copy your friend's answer code first (they send it to you), then press this again."});
        return;
    }
    if (!CoopHookshot::HostMenu().Answer(clip.c_str())) {
        g_answer_note.clear();
        g_menu.SetNote({"That code didn't work: ask your friend to copy it again."});
        return;
    }
    LOG_INFO(Core, "coop: multiplayer tab: host: the friend's answer code was pasted");
    g_answer_note = "Answer code taken: connecting your friend...";
    RefreshSession(true);
}

void Copy(const std::string& text, const char* what) {
    if (text.empty()) {
        g_menu.SetNote({std::string("No ") + what + " yet."});
    } else if (SetClipboardText(text)) {
        g_menu.SetNote({std::string("Copied: ") + text});
    } else {
        g_menu.SetNote({"The clipboard could not be used: write the code down instead."});
    }
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
    case Request::Kind::kPaste: g_menu.Paste(ClipboardText()); break;
    case Request::Kind::kCopyInvite:
        g_lan_lines.clear();
        Copy(Status().invite, "invite code");
        break;
    case Request::Kind::kCopyAnswer: Copy(Status().answer, "answer code"); break;
    case Request::Kind::kPasteAnswer: PasteAnswer(); break;
    case Request::Kind::kCopyLan: CopyLan(); break;
    case Request::Kind::kController: CycleInput(true); break;
    case Request::Kind::kKeyboard: CycleInput(false); break;
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
    const std::string coop = ReadText(CoopIni());
    const auto last = CoopTitleMenu::IniGet(coop, "last_invite");
    g_menu.SetRejoin(CoopTitleMenu::IniGet(coop, "last_host").value_or(""), last.value_or(""));
    InputRows();   // (a controller plugged in while the menu was closed: its name)
    RefreshSession(true);
}

// The page for Hookshot, kept until this thread's next call (hos_coop_menu.h).
struct Drawn {
    CoopPauseMenu::View view;
    std::vector<HosCoopMenuRow> rows;
};

}  // namespace

void Init(bool coop_on) {
    std::lock_guard lock(g_mutex);
    g_coop_on = coop_on;
    LOG_INFO(Core, "coop: multiplayer tab: ready (the first tab of Hookshot's menu: F1, or Home on a pad){}",
             coop_on ? "; co-op came with the game's start" : "");
}

void Tick() {
    std::lock_guard lock(g_mutex);
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
    if (tab == kModsTab && CoopModsTab::Available()) {
        return CoopModsTab::TabName();
    }
    if (tab == kLobbyTab) {
        return CoopLobbyTab::TabName();
    }
    return tab == kMultiplayerTab ? "Multiplayer" : nullptr;
}

bool Page(int tab, HosCoopMenuPage* out) {
    if (tab == kModsTab) {
        return CoopModsTab::Page(out);
    }
    if (tab == kLobbyTab) {
        return CoopLobbyTab::Page(out);
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
    drawn.rows.resize(v.rows.size());
    for (std::size_t i = 0; i < v.rows.size(); ++i) {
        const CoopPauseMenu::Row& r = v.rows[i];
        HosCoopMenuRow& h = drawn.rows[i];
        h = HosCoopMenuRow{};
        h.name = r.name.c_str();
        h.value = r.value.c_str();
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
    out->help_title = v.help_title.c_str();
    out->help = v.help.c_str();
    out->hint = v.hint.c_str();
    out->gold = v.gold.c_str();
    out->change = v.change.c_str();
    out->confirm = v.confirm.c_str();
    out->back = v.back.c_str();
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

bool Input(int tab, int event, int arg, int arg2) {
    if (tab == kModsTab) {
        return CoopModsTab::MenuInput(event, arg, arg2);
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

bool Key(std::uint32_t vk, bool repeat) {
    if (CoopModsTab::Key(vk, repeat)) {  // the Mods tab's text field, while it is typed into
        return true;
    }
    std::lock_guard lock(g_mutex);
    if (!g_shown || !g_menu.typing()) {
        return false;
    }
    if (vk == 0x70 || vk == 0x2D) {   // F1, Insert: Hookshot's (the menu closes, the typing with it)
        return false;
    }
    const bool ctrl =
#ifdef _WIN32
        (GetKeyState(VK_CONTROL) & 0x8000) != 0;
#else
        false;
#endif
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
    if (!g_shown || !g_menu.typing()) {
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
    return g_shown && g_menu.typing();
}

void Open(CoopPauseMenu::Page page) {
    std::lock_guard lock(g_mutex);
    g_menu.Show(ReadOptions());
    g_menu.Open(page);
    if (page == CoopPauseMenu::Page::kJoin) {
        const std::string coop = ReadText(CoopIni());
        const auto last = CoopTitleMenu::IniGet(coop, "last_invite");
        g_menu.SetRejoin(CoopTitleMenu::IniGet(coop, "last_host").value_or(""), last.value_or(""));
        const std::string clip = ClipboardText();   // a new code copied already: ready to press Join
        if (coop::server::nat::looks_like_code(clip) && (!last || clip.find(*last) == std::string::npos)) {
            g_menu.Paste(clip);
        }
    }
    Settle();
    g_wanted = kMultiplayerTab;
    LOG_INFO(Core, "coop: multiplayer tab: opened from the title");
}

}  // namespace CoopPause
