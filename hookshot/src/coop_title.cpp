// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_title.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "coop_hook.h"
#include "coop_hookshot.h"
#include "coop_runtime.h"
#include "coop_save.h"
#include "coop_title_menu.h"
#include "nat_codes.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#endif

namespace CoopTitle {

namespace {

namespace fs = std::filesystem;
using CoopTitleMenu::Input;
using CoopTitleMenu::Page;
using CoopTitleMenu::Request;
using CoopTitleMenu::Sound;

// --- The game (build e0f3ccfa..., the only one the module runs on) ---
constexpr std::uint64_t kTitleVtable = 0x84c106e8;    // UITitleScreen
constexpr std::uint64_t kTitleActive = 0x1a8;         // u8: 1 while its step runs its work (f_83426d70)
constexpr std::uint64_t kTitleCount = 0x2c8;          // s32: its buttons
constexpr std::uint64_t kTitleButtons = 0x2d0;        // its buttons' array
constexpr std::uint64_t kTitleFocused = 0x2e0;        // the focused button (f_83426f40)
constexpr std::uint64_t kTitleSounds = 0x198;         // *(+0x198)+8: its UI sound player
constexpr std::uint64_t kPlaySound = 0x80ac65d0;      // (player, name; x8 the result's place)
constexpr std::uint64_t kTitleLayout = 0x28;          // the screen's layout
// A pane's text from a message, as UISystemWindowScreen sets its buttons' (f_83425bc0):
constexpr std::uint64_t kMsgRefInit = 0x80a86f70;     // (ref): {0, 0, -1}, 24 bytes
constexpr std::uint64_t kMsgRefSet = 0x80a8afe0;      // (ref, &"LayoutMsg/...", &"label", 1)
constexpr std::uint64_t kPaneText = 0x80a87d30;       // (layout, "pane/T_Btn_00", ref, 1, 0)
// The title's own co-op rows (tools/puppet_pack/title_menu.py adds them to Title_00 and their
// texts to LayoutMsg/Title_00): Pa_Button_04 Host, _05 Join, _06 Co-op Settings.
constexpr int kFirstRow = 4;
constexpr std::array<const char*, 3> kRowLabels{"0100", "0101", "0102"};
struct SoundName {
    Sound sound;
    std::uint64_t address;   // the name in the game's read-only data
    const char* name;
};
// The game's own names (docs/hookshot.md "The title menu"): the title's focus
// sound for the cursor, the save list's choice and start sounds, the detail
// panels' open and back.
constexpr std::array<SoundName, 6> kSounds{{{Sound::kMove, 0x83e3e4e4, "mc_button_on"},
                                            {Sound::kDecide, 0x83ce321f, "mc_NormalDecide"},
                                            {Sound::kStart, 0x83d53c8a, "mc_GameStartDecide"},
                                            {Sound::kOpen, 0x83db9ea6, "mc_OpenDetail"},
                                            {Sound::kBack, 0x83dfc1cd, "mc_ReturnFromDetail"},
                                            {Sound::kRefuse, 0x83dfc1cd, "mc_ReturnFromDetail"}}};

// --- Switch pad bits (nn::hid::NpadButton) ---
constexpr std::uint64_t kA = 1ull << 0, kB = 1ull << 1, kX = 1ull << 2, kY = 1ull << 3;
constexpr std::uint64_t kMinus = 1ull << 11;
constexpr std::uint64_t kLeft = 1ull << 12, kUp = 1ull << 13, kRight = 1ull << 14, kDown = 1ull << 15;
constexpr std::int32_t kStickPress = 16000;   // of 32767: a stick pushed this far is a d-pad press
constexpr std::uint64_t kRepeatFirstMs = 400, kRepeatMs = 110;

const HosCoopApi* g_api = nullptr;
bool g_coop_on = false;
bool g_native = false;   // LICHII_COOP_TITLE_NATIVE=1: the 4th title button (Pa_Button_03) opens the menu too

std::atomic<std::uint64_t> g_screen{0};
std::atomic<std::uint64_t> g_last_step_ms{0};
std::atomic<bool> g_step_seen{false};
std::atomic<bool> g_rows{false};
std::atomic<bool> g_focus_continue{false};
std::atomic<bool> g_reveal_continue{false};   // the title has our three rows (the puppet pack's Title_00), texts set

std::mutex g_mutex;   // everything below
CoopTitleMenu::Menu g_menu;
std::vector<Sound> g_sounds;
std::uint64_t g_pad_last = 0;
bool g_pad_eat = false;          // the press that opened or closed the menu: kept from the game until let go
std::uint64_t g_held_dir = 0;    // the direction held (pad bit), for repeats
std::uint64_t g_next_repeat_ms = 0;
std::string g_doing;             // what co-op does since the menu started it: "hosting", "joined"
int g_focus_logged = -2;

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::uint64_t Read64(std::uint64_t a) {
    std::uint64_t v = 0;
    return CoopRuntime::Read(a, &v, sizeof v) ? v : 0;
}

bool HasScratch() {
    return g_api != nullptr && g_api->size >= offsetof(HosCoopApi, scratch) + sizeof(g_api->scratch) &&
           g_api->scratch != nullptr;
}

// The title screen, while it is up and showing its buttons: the object is
// still a UITitleScreen, its step's flag is on, and (once its step has been
// seen) a step came in the last 300 ms.
bool TitleUp() {
    const std::uint64_t s = g_screen.load();
    if (s == 0 || Read64(s) != kTitleVtable) {
        return false;
    }
    std::uint8_t active = 0;
    if (!CoopRuntime::Read(s + kTitleActive, &active, 1) || active != 1) {
        return false;
    }
    return !g_step_seen.load() || NowMs() - g_last_step_ms.load() < 300;
}

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

void SetEnv(const std::string& name, const std::string& value) {
#ifdef _WIN32
    _putenv_s(name.c_str(), value.c_str());   // the C library's copy (getenv)
    SetEnvironmentVariableA(name.c_str(), value.empty() ? nullptr : value.c_str());   // the process's (children)
#else
    if (value.empty()) {
        unsetenv(name.c_str());
    } else {
        setenv(name.c_str(), value.c_str(), 1);
    }
#endif
}

// A UTF-8 path (a file's text, a variable) as a path.
fs::path PathOf(const std::string& s) {
    return fs::path(std::u8string(s.begin(), s.end()));
}

std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

// Written whole: a new file beside it, then moved over the old one.
bool WriteText(const fs::path& p, const std::string& text) {
    const fs::path tmp = p.string() + ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !(f << text) || !f.flush()) {
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// The kit this game belongs to: OOT_COOP_KIT, the first line of coop-kit.txt
// beside longshot.exe, or the nearest folder above the game that has
// server\coop_server.exe and start-server.cmd (the kit's hookshot\game\).
std::optional<fs::path> FindKit() {
    auto is_kit = [](const fs::path& d) {
        std::error_code ec;
        return fs::exists(d / "server" / "coop_server.exe", ec) && fs::exists(d / "start-server.cmd", ec);
    };
    std::vector<fs::path> tries;
    if (const std::string e = Env("OOT_COOP_KIT"); !e.empty()) {
        tries.emplace_back(PathOf(e));
    }
    const fs::path game = CoopRuntime::GameDir();
    {
        std::string line = ReadText(game / "coop-kit.txt");
        line = line.substr(0, line.find_first_of("\r\n"));
        if (!line.empty()) {
            tries.emplace_back(PathOf(line));
        }
    }
    fs::path d = game;
    for (int i = 0; i < 5 && !d.empty(); ++i) {
        tries.push_back(d);
        if (d.parent_path() == d) {
            break;
        }
        d = d.parent_path();
    }
    for (const auto& t : tries) {
        if (is_kit(t)) {
            return t;
        }
    }
    return std::nullopt;
}

fs::path CoopIni() {
    return CoopRuntime::GameDir() / "coop.ini";
}

std::string Utf8Path(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

#ifdef _WIN32
std::string Narrow(const wchar_t* w);
#endif

// The player's Windows name, for a first-time player with no name in coop.ini ("" when it will not do).
std::string WindowsName() {
#ifdef _WIN32
    wchar_t w[257] = {};
    DWORD n = 257;
    if (!GetUserNameW(w, &n)) {
        return {};
    }
    std::string name = Narrow(w);
    while (!name.empty() && !CoopTitleMenu::NameOk(name)) {   // too long: cut to 16 characters
        do {
            name.pop_back();
        } while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80);
    }
    return name;
#else
    const char* u = std::getenv("USER");
    return u != nullptr && CoopTitleMenu::NameOk(u) ? std::string(u) : std::string();
#endif
}

CoopTitleMenu::Options ReadOptions() {
    const auto kit = FindKit();
    const std::string coop = ReadText(CoopIni());
    auto o = CoopTitleMenu::Options::Read(coop, kit ? ReadText(*kit / "coop-options.ini") : std::string());
    if (!CoopTitleMenu::IniGet(coop, "name")) {
        if (const std::string w = WindowsName(); !w.empty()) {
            o.name = w;
        }
    }
    return o;
}

#ifdef _WIN32
std::wstring Wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) {
        return {};
    }
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
#endif

std::string ClipboardText() {
#ifdef _WIN32
    std::string out;
    if (!OpenClipboard(nullptr)) {
        return out;
    }
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* w = static_cast<const wchar_t*>(GlobalLock(h))) {
            out = Narrow(w);
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return out.size() > 4096 ? out.substr(0, 4096) : out;
#else
    return {};
#endif
}

bool SetClipboardText(const std::string& text) {
#ifdef _WIN32
    const std::wstring w = Wide(text);
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
        if (auto* p = static_cast<wchar_t*>(GlobalLock(h))) {
            std::memcpy(p, w.c_str(), (w.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(h);
            ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr;
        }
        if (!ok) {
            GlobalFree(h);
        }
    }
    CloseClipboard();
    return ok;
#else
    (void)text;
    return false;
#endif
}

// --- What the menu asked for (under g_mutex) ---

void SetCoopEnvironment(const CoopTitleMenu::Options& o) {
    for (const auto& [k, v] : o.Environment()) {
        SetEnv(k, v);
    }
}

bool g_copied = false;   // this session's invite code was put on the clipboard
std::string g_answer_note;  // what became of the friend's answer code the host pasted ("" when none)
std::string g_join_text;    // the code (or address) this join used: remembered once connected
CoopTitleMenu::AutoLoad g_autoload;   // Join -> the host's save loaded in one step
constexpr int kContinueButton = CoopTitleMenu::AutoLoad::kContinue;
constexpr std::uint64_t kFocus = 0x8088ffc0;   // the screens' focus setter (screen, button, 0): Hookshot's oot_menu.c
// The title's row count: its layout's BtnNum animation at a frame (frame N shows N rows), as UISystemWindowScreen
// sets its own (f_83425740 takes it with kAnim, f_83425ad0.. sets it with kAnimFrame and s0).
constexpr std::uint64_t kAnim = 0x8045c7a0;        // (layout, name): the layout's animation of that name
constexpr std::uint64_t kAnimFrame = 0x802f2d90;   // (animation, s0 = frame)
constexpr std::uint64_t kBtnNumName = 0x83e3e439;  // "BtnNum" in the game's read-only data
constexpr float kRowsWithContinue = 3.0f;          // New Game, Options, Continue
bool g_join_saved = false;

// The Host dialog's "Same network or VPN" lines while they are shown ("Copy the code again" brings the code back).
std::vector<std::string> g_lan_lines;

// This PC's IPv4 addresses, as text (CoopTitleMenu::LanAddresses picks the ones a friend can use).
std::vector<std::string> LocalIpv4s() {
    std::vector<std::string> out;
    const auto add = [&out](const in_addr& a) {
        char text[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &a, text, sizeof text) != nullptr) {
            out.emplace_back(text);
        }
    };
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return out;
    }
    char name[256] = {};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* list = nullptr;
    if (gethostname(name, sizeof name - 1) == 0 && getaddrinfo(name, nullptr, &hints, &list) == 0) {
        for (const addrinfo* i = list; i != nullptr; i = i->ai_next) {
            if (i->ai_family == AF_INET && i->ai_addr != nullptr && i->ai_addrlen >= sizeof(sockaddr_in)) {
                add(reinterpret_cast<const sockaddr_in*>(i->ai_addr)->sin_addr);
            }
        }
        freeaddrinfo(list);
    }
    WSACleanup();
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (const ifaddrs* i = list; i != nullptr; i = i->ifa_next) {
            if (i->ifa_addr != nullptr && i->ifa_addr->sa_family == AF_INET) {
                add(reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr);
            }
        }
        freeifaddrs(list);
    }
#endif
    return out;
}

// What the module's host is doing, in plain words (CoopTitleMenu::PlainStatus). The invite code goes on the
// clipboard by itself, once, as soon as there is one: the host has nothing to copy or type.
std::vector<std::string> HostStatusLines() {
    HosCoopStatus st{};
    st.size = sizeof st;
    CoopHookshot::HostMenu().Status(&st);
    CoopTitleMenu::HostView v;
    v.host = g_doing == "hosting";
    v.state = st.state;
    v.problem = st.problem;
    v.players = st.players;
    v.invite_done = st.invite_done != 0;
    v.invite = st.invite;
    v.answer = st.answer;
    if (!v.host && v.state == HOS_COOP_CONNECTED && !g_join_saved && !g_join_text.empty()) {
        g_join_saved = true;   // Join offers "Join <host> again" next time
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
        // The host's save next, in one step (AutoLoad): the dialog goes, the title shows what happens.
        g_autoload.Start(NowMs(), host);
        g_menu.Close();   // (no press to keep from the game: the presses that follow are AutoLoad's)
        LOG_INFO(Core, "coop: title menu: joined: loading the host's save when it is here");
    }
    if (v.host && !v.invite.empty() && !g_copied) {
        g_copied = SetClipboardText(v.invite);
        LOG_INFO(Core, "coop: title menu: host: the invite code is on the clipboard{}", g_copied ? "" : " (it could not be)");
    }
    v.copied = g_copied;
    if (st.message[0] != 0) {
        static std::string said;
        if (said != st.message) {
            said = st.message;
            LOG_INFO(Core, "coop: title menu: {}", said);   // (the English detail, for the log)
        }
    }
    if (v.host && v.state == HOS_COOP_HOSTING && !g_lan_lines.empty()) {
        return g_lan_lines;
    }
    std::vector<std::string> lines = CoopTitleMenu::PlainStatus(v);
    if (!g_answer_note.empty() && v.problem != HOS_COOP_PROBLEM_BAD_CODE) {
        lines.push_back(v.players > 1 ? "Connected!" : g_answer_note);
    }
    return lines;
}

// The host pastes the friend's ANSWER CODE (the friend's game could not reach this one: their dialog shows it).
void PasteAnswer() {
    const std::string clip = ClipboardText();
    if (!coop::server::nat::looks_like_code(clip)) {
        g_answer_note.clear();
        g_menu.SetStatus({"Copy your friend's answer code first (they send it to you), then press this again."});
        g_sounds.push_back(Sound::kRefuse);
        return;
    }
    if (!CoopHookshot::HostMenu().Answer(clip.c_str())) {
        g_answer_note.clear();
        g_menu.SetStatus({"That code didn't work: ask your friend to copy it again."});
        g_sounds.push_back(Sound::kRefuse);
        return;
    }
    LOG_INFO(Core, "coop: title menu: host: the friend's answer code was pasted");
    g_answer_note = "Answer code taken: connecting your friend...";
    g_menu.SetStatus(HostStatusLines());
}

// The Hosting / Joining page follows the host's status.
void RefreshStatus() {
    if (g_menu.page() == Page::kHosting || g_menu.page() == Page::kJoining) {
        static std::uint64_t next = 0;
        if (NowMs() >= next) {
            next = NowMs() + 250;
            g_menu.SetStatus(HostStatusLines());
        }
    }
}

std::string HostInvite() {
    HosCoopStatus st{};
    st.size = sizeof st;
    CoopHookshot::HostMenu().Status(&st);
    return st.invite;
}

// Host and Join through the module's own host (native-host-b, hos_coop_host.h): the co-op server or the
// invite code's join helper on the module's "coop host" thread, and this game's client connected to it.
// No windows, no scripts; the menu shows its status (HostStatusLines).
bool Busy() {
    HosCoopStatus st{};
    st.size = sizeof st;
    CoopHookshot::HostMenu().Status(&st);
    return g_coop_on || (st.state != HOS_COOP_IDLE && st.state != HOS_COOP_FAILED);
}

void Host() {
    if (Busy()) {
        g_menu.SetStatus({"Co-op is on already in this game: quit the game to host or join another."});
        g_sounds.push_back(Sound::kRefuse);
        return;
    }
    CoopHookshot::HostMenu().Leave();   // (a failed start: clear it)
    const CoopTitleMenu::Options o = g_menu.options();
    SetCoopEnvironment(o);
    HosCoopHostOptions h{};
    h.size = sizeof h;
    h.name = o.name.c_str();
    h.password = nullptr;   // a new random one, inside the invite code: nobody types it
    h.port = 0;
    h.upnp = 1;             // the router forward (dauro: nothing to set up)
    h.invite = 1;
    h.pvp = o.pvp ? 1 : 0;
    h.max_players = static_cast<std::uint8_t>(o.max_players);
    if (!CoopHookshot::HostMenu().HostStart(&h)) {
        g_menu.SetStatus(HostStatusLines());
        g_sounds.push_back(Sound::kRefuse);
        return;
    }
    LOG_INFO(Core, "coop: title menu: host: started (up to {} players)", o.max_players);
    g_doing = "hosting";
    g_copied = false;
    g_lan_lines.clear();
    g_answer_note.clear();
    g_menu.ShowPage(Page::kHosting);
    g_menu.SetStatus(HostStatusLines());
}

void Join(const std::string& text) {
    if (Busy()) {
        g_menu.SetStatus({"Co-op is on already in this game: quit the game to host or join another."});
        g_sounds.push_back(Sound::kRefuse);
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
        g_menu.SetStatus({*err});
        g_sounds.push_back(Sound::kRefuse);
        return;
    }
    CoopHookshot::HostMenu().Leave();
    const CoopTitleMenu::Options o = g_menu.options();
    SetCoopEnvironment(o);
    const bool invite = std::holds_alternative<CoopTitleMenu::JoinInvite>(target);
    const std::string where = invite ? text : std::get<CoopTitleMenu::JoinAddress>(target).server;
    const std::string password = invite ? std::string() : std::get<CoopTitleMenu::JoinAddress>(target).password;
    HosCoopJoinOptions j{};
    j.size = sizeof j;
    j.name = o.name.c_str();
    j.target = where.c_str();
    j.password = password.empty() ? nullptr : password.c_str();
    if (!CoopHookshot::HostMenu().Join(&j)) {
        g_menu.SetStatus(HostStatusLines());
        g_sounds.push_back(Sound::kRefuse);
        return;
    }
    LOG_INFO(Core, "coop: title menu: join: {}", invite ? std::string("an invite code") : "the address " + where);
    g_doing = "joined";
    g_join_text = std::string(text);
    g_join_saved = false;
    g_menu.ShowPage(Page::kJoining);
    g_menu.SetStatus(HostStatusLines());
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
    if (!g_coop_on) {
        SetCoopEnvironment(g_menu.options());   // co-op has not started: they count for this run
    }
    LOG_INFO(Core, "coop: title menu: settings saved{}{}", ok ? "" : " (a file could not be written)",
             kit ? "" : " (no kit found: coop.ini only)");
    std::vector<std::string> status{ok ? "Settings saved." : "A settings file could not be written."};
    if (!kit) {
        status.push_back("No co-op kit found with this game: only your name was saved (coop.ini).");
    }
    if (g_coop_on) {
        status.push_back("Co-op is on: the new settings count the next time the game starts.");
    }
    g_menu.SetStatus(status);
}

void Act(const Request& r) {
    switch (r.kind) {
    case Request::Kind::kHost:
        Host();
        break;
    case Request::Kind::kJoin:
        Join(r.text);
        break;
    case Request::Kind::kSave:
        Save();
        break;
    case Request::Kind::kPaste:
        g_menu.Paste(ClipboardText());
        break;
    case Request::Kind::kPasteAnswer:
        g_lan_lines.clear();
        PasteAnswer();
        break;
    case Request::Kind::kCopyLan: {
        // The address and the join password for a friend on the host's home network or VPN (never logged).
        HosCoopStatus st{};
        st.size = sizeof st;
        CoopHookshot::HostMenu().Status(&st);
        if (st.state != HOS_COOP_HOSTING || st.port == 0) {
            g_menu.SetStatus({"The server is not running yet."});
            break;
        }
        const auto addresses = CoopTitleMenu::LanAddresses(LocalIpv4s());
        const bool copied = !addresses.empty() &&
                            SetClipboardText(CoopTitleMenu::LanJoinText(addresses.front().address, st.port, st.password));
        g_lan_lines = CoopTitleMenu::LanLines(addresses, st.port, st.password, copied);
        LOG_INFO(Core, "coop: title menu: host: same network or VPN: {} address(es){}", addresses.size(),
                 copied ? ", the join line is on the clipboard" : "");
        g_menu.SetStatus(g_lan_lines);
        break;
    }
    case Request::Kind::kCopyInvite: {
        g_lan_lines.clear();
        const std::string invite = HostInvite();
        if (invite.empty()) {
            g_menu.SetStatus({"No invite code yet."});
        } else if (SetClipboardText(invite)) {
            g_menu.SetStatus({"Invite code copied: " + invite, "Send it to your friends; they choose Join and paste it."});
        }
        break;
    }
    }
}

void Press(Input in) {
    const Sound s = g_menu.Press(in);
    if (s != Sound::kNone) {
        g_sounds.push_back(s);
    }
    if (const auto r = g_menu.TakeRequest()) {
        Act(*r);
    }
}

void OpenMenu() {
    g_menu.Open(ReadOptions());
    g_sounds.push_back(Sound::kOpen);
    if (g_coop_on) {
        g_menu.SetStatus({g_doing.empty() ? "Co-op is on in this game." : g_doing == "hosting" ? "You are hosting."
                                                                                              : "You joined a game."});
    }
    LOG_INFO(Core, "coop: title menu: opened");
}

void LogButtons(std::uint64_t screen) {
    std::int32_t count = 0;
    CoopRuntime::Read(screen + kTitleCount, &count, sizeof count);
    const std::uint64_t buttons = Read64(screen + kTitleButtons);
    std::string line = "coop: title menu: the title screen " + fmt::format("{:#x}", screen) + ", " +
                       std::to_string(count) + " buttons:";
    for (std::int32_t i = 0; i < count && i < 8; ++i) {
        const std::uint64_t b = Read64(buttons + 8ull * static_cast<std::uint64_t>(i));
        std::uint32_t flags = 0;
        CoopRuntime::Read(b + 0x40, &flags, sizeof flags);
        line += fmt::format(" [{}] {:#x} flags {:#x}", i, b, flags);
    }
    LOG_INFO(Core, "{}", line);
    static bool dumped = false;   // (the first time only: what the 4th button's state looks like, for the native button)
    if (dumped || count < 1) {
        return;
    }
    dumped = true;
    for (std::int32_t i = 0; i < count && i < 4; ++i) {
        const std::uint64_t b = Read64(buttons + 8ull * static_cast<std::uint64_t>(i));
        std::array<std::uint8_t, 0x80> bytes{};
        if (!CoopRuntime::Read(b, bytes.data(), bytes.size())) {
            continue;
        }
        std::string hex;
        for (std::size_t k = 0; k < bytes.size(); ++k) {
            hex += fmt::format("{:02x}{}", bytes[k], (k % 8 == 7) ? " " : "");
        }
        LOG_INFO(Core, "coop: title menu: button {} at {:#x}: {}", i, b, hex);
        const std::uint64_t pane = Read64(b + 0x10);
        if (pane != 0 && CoopRuntime::Read(pane, bytes.data(), bytes.size())) {
            hex.clear();
            for (std::size_t k = 0; k < bytes.size(); ++k) {
                hex += fmt::format("{:02x}{}", bytes[k], (k % 8 == 7) ? " " : "");
            }
            LOG_INFO(Core, "coop: title menu: button {} +0x10 -> {:#x}: {}", i, pane, hex);
        }
    }
}

// The focused button's number (LICHII_COOP_TITLE_NATIVE and the log), -1 when none.
int FocusedIndex(std::uint64_t screen) {
    std::int32_t count = 0;
    CoopRuntime::Read(screen + kTitleCount, &count, sizeof count);
    const std::uint64_t buttons = Read64(screen + kTitleButtons), focused = Read64(screen + kTitleFocused);
    for (std::int32_t i = 0; i < count && i < 8; ++i) {
        if (focused != 0 && Read64(buttons + 8ull * static_cast<std::uint64_t>(i)) == focused) {
            return i;
        }
    }
    return -1;
}

std::int32_t ButtonCount(std::uint64_t screen) {
    std::int32_t count = 0;
    CoopRuntime::Read(screen + kTitleCount, &count, sizeof count);
    return count;
}

// The rows' texts, from the game's message files (LayoutMsg/Title_00: labels 0100..0102), set the way
// the game sets its own buttons' (kPaneText). Each needs the strings and the message ref in the game's
// memory: one scratch block (patch 0004) for a row's calls. True when all three were set.
bool SetRowTexts(void* ctx, std::uint64_t screen) {
    const std::uint64_t layout = Read64(screen + kTitleLayout);
    if (ctx == nullptr || layout == 0 || !HasScratch()) {
        return false;
    }
    int set = 0;
    for (std::size_t i = 0; i < kRowLabels.size(); ++i) {
        // [0x00] the ref, [0x20] &path, [0x28] &label, [0x30] path, [0x50] label, [0x60] pane
        std::array<std::uint8_t, 0x90> b{};
        const std::uint64_t base = g_api->scratch(ctx, b.data(), b.size());   // (where it goes: the same each time)
        if (base == 0) {
            return false;
        }
        const std::string path = "LayoutMsg/Title_00", label = kRowLabels[i];
        const std::string pane = fmt::format("Pa_Button_{:02d}/T_Btn_00", kFirstRow + static_cast<int>(i));
        const std::uint64_t at_path = base + 0x30, at_label = base + 0x50, at_pane = base + 0x60;
        std::memcpy(b.data() + 0x20, &at_path, 8);
        std::memcpy(b.data() + 0x28, &at_label, 8);
        std::memcpy(b.data() + 0x30, path.c_str(), path.size() + 1);
        std::memcpy(b.data() + 0x50, label.c_str(), label.size() + 1);
        std::memcpy(b.data() + 0x60, pane.c_str(), pane.size() + 1);
        if (g_api->scratch(ctx, b.data(), b.size()) != base) {
            return false;
        }
        std::uint64_t x0 = 0;
        HosCoopCall k{};
        k.function = kMsgRefInit;
        k.x[0] = base;
        if (!g_api->call(ctx, &k, &x0)) {
            return false;
        }
        k = HosCoopCall{};
        k.function = kMsgRefSet;
        k.x[0] = base, k.x[1] = base + 0x20, k.x[2] = base + 0x28, k.x[3] = 1;
        if (!g_api->call(ctx, &k, &x0)) {
            return false;
        }
        k = HosCoopCall{};
        k.function = kPaneText;
        k.x[0] = layout, k.x[1] = at_pane, k.x[2] = base, k.x[3] = 1, k.x[4] = 0;
        if (!g_api->call(ctx, &k, &x0)) {
            return false;
        }
        ++set;
    }
    return set == static_cast<int>(kRowLabels.size());
}

// The sounds waiting, played with the title screen's own sound player.
void PlaySounds(void* ctx) {
    std::vector<Sound> sounds;
    {
        std::lock_guard lock(g_mutex);
        sounds.swap(g_sounds);
    }
    if (sounds.empty() || ctx == nullptr || !HasScratch()) {
        return;
    }
    const std::uint64_t s = g_screen.load();
    if (s == 0 || Read64(s) != kTitleVtable) {
        return;
    }
    const std::uint64_t player = Read64(Read64(s + kTitleSounds) + 8);
    if (player == 0) {
        return;
    }
    int played = 0;
    for (const Sound sound : sounds) {
        if (played == 3) {
            break;
        }
        for (const auto& n : kSounds) {
            if (n.sound != sound) {
                continue;
            }
            char have[32] = {};
            if (!CoopRuntime::Read(n.address, have, std::strlen(n.name) + 1) || std::strcmp(have, n.name) != 0) {
                static bool said = false;
                if (!said) {
                    said = true;
                    LOG_WARNING(Core, "coop: title menu: the game's sound name {} is not at {:#x}: no sounds", n.name,
                                n.address);
                }
                return;
            }
            HosCoopCall k{};
            k.function = kPlaySound;
            k.x[0] = player;
            k.x[1] = n.address;
            std::uint64_t x0 = 0;
            g_api->call(ctx, &k, &x0);
            ++played;
            break;
        }
    }
}

// Pad bits for the menu's moves (the d-pad, or the left stick pushed).
std::uint64_t Directions(std::uint64_t buttons, const std::int32_t stick[4]) {
    std::uint64_t d = buttons & (kLeft | kUp | kRight | kDown);
    if (stick[0] < -kStickPress) d |= kLeft;
    if (stick[0] > kStickPress) d |= kRight;
    if (stick[1] > kStickPress) d |= kUp;
    if (stick[1] < -kStickPress) d |= kDown;
    return d;
}

}  // namespace

void Init(const HosCoopApi* api, bool coop_on) {
    g_api = api;
    g_coop_on = coop_on;
    g_native = Env("LICHII_COOP_TITLE_NATIVE") == "1";
    LOG_INFO(Core, "coop: title menu: {}{}", HasScratch() ? "on (Minus at the title)" : "off: this Hookshot has no patch 0004 (build it again)",
             g_native ? "; the 4th title button opens it too (LICHII_COOP_TITLE_NATIVE)" : "");
}

void ApplyKitOptions() {
    const auto kit = FindKit();
    if (!kit) {
        return;
    }
    int set = 0;
    for (auto [k, v] : CoopTitleMenu::KitVariables(ReadText(*kit / "coop-options.ini"))) {
        if (std::getenv(k.c_str()) != nullptr) {
            continue;   // the launcher set it (or the player did)
        }
        std::error_code ec;
        if (!v.empty() && !PathOf(v).is_absolute() && fs::exists(*kit / PathOf(v), ec)) {
            v = Utf8Path(fs::absolute(*kit / PathOf(v), ec));   // as play.ps1: a file of the kit
        }
        SetEnv(k, v);
        ++set;
    }
    if (set != 0) {
        LOG_INFO(Core, "coop: {} switch(es) from {}\\coop-options.ini (the game was started without the kit's launcher)",
                 set, Utf8Path(*kit));
    }
}

void OnTitle(void* ctx, std::uint64_t screen, int event) {
    if (event == HOS_COOP_TITLE_GONE) {
        std::uint64_t was = screen;
        g_screen.compare_exchange_strong(was, 0);
        std::lock_guard lock(g_mutex);
        if (g_menu.open()) {
            g_menu.Close();
        }
        return;
    }
    g_screen = screen;
    if (event == HOS_COOP_TITLE_SETUP) {
        LogButtons(screen);
        const std::int32_t count = ButtonCount(screen);
        if (count >= kFirstRow + static_cast<std::int32_t>(kRowLabels.size())) {
            const bool ok = SetRowTexts(ctx, screen);
            g_rows = ok;
            LOG_INFO(Core, "coop: title menu: the title's own rows Host, Join, Co-op Settings (buttons {} to {}){}",
                     kFirstRow, kFirstRow + 2, ok ? "" : ": their texts could not be set");
        } else {
            g_rows = false;
            LOG_INFO(Core, "coop: title menu: the title has {} buttons, not the puppet pack's {}: the Minus prompt "
                     "instead (build the puppet pack again for the title's own rows)",
                     count, kFirstRow + static_cast<int>(kRowLabels.size()));
        }
    }
    if (event == HOS_COOP_TITLE_STEP && g_reveal_continue.exchange(false) && ctx != nullptr) {
        // Continue shown: a joiner with no save of their own got a title without it, and the host's save came after.
        char name[8] = {};
        const std::uint64_t layout = Read64(screen + kTitleLayout);
        if (layout != 0 && CoopRuntime::Read(kBtnNumName, name, 7) && std::strcmp(name, "BtnNum") == 0) {
            HosCoopCall k{};
            k.function = kAnim;
            k.x[0] = layout, k.x[1] = kBtnNumName;
            std::uint64_t anim = 0;
            if (g_api->call(ctx, &k, &anim) && anim != 0) {
                k = HosCoopCall{};
                k.function = kAnimFrame;
                k.x[0] = anim;
                k.s0 = kRowsWithContinue;
                std::uint64_t x0 = 0;
                g_api->call(ctx, &k, &x0);
                LOG_INFO(Core, "coop: title menu: the title's Continue shown for the host's save");
            }
        }
    }
    if (event == HOS_COOP_TITLE_STEP && g_focus_continue.exchange(false) && ctx != nullptr) {
        // The cursor to Continue for the host's save (AutoLoad), as the game's own screens move it.
        std::int32_t count = 0;
        CoopRuntime::Read(screen + kTitleCount, &count, sizeof count);
        const std::uint64_t button =
            count > kContinueButton ? Read64(Read64(screen + kTitleButtons) + 8ull * kContinueButton) : 0;
        if (button != 0) {
            HosCoopCall k{};
            k.function = kFocus;
            k.x[0] = screen, k.x[1] = button, k.x[2] = 0;
            std::uint64_t x0 = 0;
            g_api->call(ctx, &k, &x0);
            LOG_INFO(Core, "coop: title menu: the cursor on Continue for the host's save");
        }
    }
    if (event == HOS_COOP_TITLE_STEP) {
        if (!g_step_seen.exchange(true)) {
            LOG_INFO(Core, "coop: title menu: the title screen's step runs");
        }
        g_last_step_ms = NowMs();
        const int f = FocusedIndex(screen);
        std::lock_guard lock(g_mutex);
        if (f != g_focus_logged) {
            g_focus_logged = f;
            LOG_INFO(Core, "coop: title menu: the title's focus is on button {}", f);
        }
        RefreshStatus();
    }
    PlaySounds(ctx);
}

void OnFrame(void* ctx) {
    {
        std::lock_guard lock(g_mutex);
        RefreshStatus();
    }
    PlaySounds(ctx);
}

// Link is in the game: this game's own row has a scene (CoopHook::PlayerList).
bool InGame() {
    for (const auto& row : CoopHook::PlayerList()) {
        if (row.self) {
            return row.scene.has_value() && !row.scene->empty();
        }
    }
    return false;
}

std::uint64_t OnPad(std::uint64_t buttons, std::int32_t stick[4]) {
    std::lock_guard lock(g_mutex);
    if (g_autoload.active()) {
        const auto tick = g_autoload.Step(NowMs(), CoopSave::HostSaveReady(), TitleUp(), InGame());
        if (tick.reveal_continue) {
            g_reveal_continue = true;   // (made at the title's next step, with its game context)
        }
        if (tick.focus_continue) {
            g_focus_continue = true;   // (made at the title's next step, with its game context)
        }
        // Only on Continue: if the title kept it hidden (no save when it opened), the cursor stays elsewhere
        // (New Game) and no A goes in; the give-up line then says to press Continue.
        const bool title = TitleUp();
        if (tick.press_a && CoopTitleMenu::AutoLoad::PressAllowed(title, title ? FocusedIndex(g_screen.load()) : -1)) {
            buttons |= kA;
        }
        if (!g_autoload.active()) {
            LOG_INFO(Core, "coop: title menu: host's save: {}", g_autoload.Line().empty() ? "loaded" : g_autoload.Line());
        }
    }
    const std::uint64_t dirs = Directions(buttons, stick);
    const std::uint64_t word = (buttons & ~(kLeft | kUp | kRight | kDown)) | dirs;
    const std::uint64_t down = word & ~g_pad_last;
    g_pad_last = word;
    auto hush = [&] {
        for (int i = 0; i < 4; ++i) {
            stick[i] = 0;
        }
        return std::uint64_t{0};
    };
    if (g_menu.open()) {
        const std::uint64_t now = NowMs();
        std::uint64_t moves = down & (kLeft | kUp | kRight | kDown);
        const std::uint64_t held = dirs & (kLeft | kUp | kRight | kDown);
        if (moves != 0) {
            g_held_dir = moves & (~moves + 1);   // the lowest one
            g_next_repeat_ms = now + kRepeatFirstMs;
        } else if (g_held_dir != 0 && (held & g_held_dir) != 0 && now >= g_next_repeat_ms) {
            moves = g_held_dir;
            g_next_repeat_ms = now + kRepeatMs;
        } else if ((held & g_held_dir) == 0) {
            g_held_dir = 0;
        }
        if (moves & kUp) Press(Input::kUp);
        if (moves & kDown) Press(Input::kDown);
        if (moves & kLeft) Press(Input::kLeft);
        if (moves & kRight) Press(Input::kRight);
        if (down & kA) Press(Input::kDecide);
        if (down & (kB | kMinus)) Press(Input::kBack);
        if (down & kX) Press(Input::kPaste);
        if (down & kY) Press(Input::kErase);
        g_pad_eat = true;   // (the press that closes it stays the menu's until let go)
        return hush();
    }
    if (g_pad_eat) {
        if (word == 0) {
            g_pad_eat = false;
        } else {
            return hush();
        }
    }
    if (!HasScratch() || !TitleUp()) {
        return buttons;
    }
    if (g_rows && (down & kA) != 0) {
        // The title's own rows: the game takes the A too (the button's own press look and sound), and its
        // title sequence has nothing for these buttons; ours opens.
        const int f = FocusedIndex(g_screen.load());
        if (f >= kFirstRow && f < kFirstRow + static_cast<int>(kRowLabels.size())) {
            LOG_INFO(Core, "coop: title menu: the title's row {} pressed",
                     kRowLabels[static_cast<std::size_t>(f - kFirstRow)]);
            const auto options = ReadOptions();
            if (f == kFirstRow) {
                g_menu.Open(options, Page::kHosting);
                Host();
            } else if (f == kFirstRow + 1) {
                g_menu.Open(options, Page::kJoin);
                const std::string coop = ReadText(CoopIni());
                const auto last = CoopTitleMenu::IniGet(coop, "last_invite");
                if (last && !last->empty()) {   // "Join <host> again", first
                    g_menu.SetRejoin(CoopTitleMenu::IniGet(coop, "last_host").value_or(""), *last);
                }
                const std::string clip = ClipboardText();   // a new code copied already: ready to press OK
                if (coop::server::nat::looks_like_code(clip) && (!last || clip.find(*last) == std::string::npos)) {
                    g_menu.Paste(clip);
                }
            } else {
                g_menu.Open(options, Page::kSettings);
            }
            g_pad_eat = true;
        }
        return buttons;
    }
    bool open = !g_rows && (down & kMinus) != 0;
    if (!open && g_native && (down & kA) != 0 && FocusedIndex(g_screen.load()) == 3) {
        open = true;   // the 4th title button (LICHII_COOP_TITLE_NATIVE)
    }
    if (open) {
        OpenMenu();
        g_pad_eat = true;
        return hush();
    }
    return buttons;
}

bool Key(std::uint32_t vk, bool repeat) {
    std::lock_guard lock(g_mutex);
    if (!g_menu.open()) {
        return false;
    }
    (void)repeat;
    const bool ctrl =
#ifdef _WIN32
        (GetKeyState(VK_CONTROL) & 0x8000) != 0;
#else
        false;
#endif
    switch (vk) {
    case 0x26: Press(Input::kUp); break;
    case 0x28: Press(Input::kDown); break;
    case 0x25: Press(Input::kLeft); break;
    case 0x27: Press(Input::kRight); break;
    case 0x0D: Press(Input::kDecide); break;
    case 0x1B: Press(Input::kBack); break;
    case 0x08: Press(Input::kErase); break;
    case 'V':
        if (ctrl) {
            Press(Input::kPaste);
        }
        break;
    default: break;
    }
    return true;   // every key while it is open
}

bool Char(std::uint32_t cp) {
    std::lock_guard lock(g_mutex);
    if (!g_menu.open()) {
        return false;
    }
    if (cp >= 0x20 && cp != 0x7F) {
        g_menu.Char(cp);
    }
    return true;
}

bool KeysTaken() {
    std::lock_guard lock(g_mutex);
    return g_menu.open();
}

std::string ClientId() {
    static std::mutex m;
    std::lock_guard lock(m);
    static std::string id;
    if (!id.empty()) {
        return id;
    }
    std::string coop = ReadText(CoopIni());
    if (const auto have = CoopTitleMenu::IniGet(coop, "client_id"); have && have->size() >= 16) {
        id = *have;
        return id;
    }
    std::random_device rd;
    for (int i = 0; i < 4; ++i) {
        id += fmt::format("{:08x}", rd());
    }
    coop = CoopTitleMenu::IniSet(coop, "client_id", id, true);
    WriteText(CoopIni(), coop);
    return id;
}

std::optional<CoopDraw::TitleView> View() {
    std::lock_guard lock(g_mutex);
    if (g_menu.open()) {
        CoopTitleMenu::View v = g_menu.view();
        if (g_coop_on && (g_menu.page() == Page::kHosting || g_menu.page() == Page::kJoining)) {
            const auto lines = CoopHook::OverlayLines();   // "Co-op: connected ..." as it goes
            if (!lines.empty()) {
                v.status.push_back(lines.front().text);
            }
        }
        return CoopDraw::TitleView{"", std::move(v)};
    }
    if (const std::string line = g_autoload.Line(); !line.empty() && HasScratch() && TitleUp()) {
        return CoopDraw::TitleView{line, std::nullopt};   // ("Loading Saria's save...")
    }
    if (!HasScratch() || !TitleUp() || g_rows) {   // (the title's own rows: no prompt)
        return std::nullopt;
    }
    return CoopDraw::TitleView{g_doing.empty() ? std::string("Multiplayer") : "Multiplayer: " + g_doing,
                              std::nullopt};
}


}  // namespace CoopTitle
