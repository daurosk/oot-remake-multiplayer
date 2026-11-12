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
#include <sstream>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"
#include "coop_hook.h"
#include "coop_runtime.h"
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
std::atomic<bool> g_start_coop{false};

std::mutex g_mutex;   // everything below
CoopTitleMenu::Menu g_menu;
std::vector<Sound> g_sounds;
std::uint64_t g_pad_last = 0;
bool g_pad_eat = false;          // the press that opened or closed the menu: kept from the game until let go
std::uint64_t g_held_dir = 0;    // the direction held (pad bit), for repeats
std::uint64_t g_next_repeat_ms = 0;
std::string g_invite;            // this session's invite code (Host), once seen
std::uint64_t g_watch_until_ms = 0;   // until when the clipboard is watched for the server's invite code
std::string g_clip_before;       // the clipboard when Host was chosen
std::string g_doing;             // the prompt's words while co-op runs ("Multiplayer: hosting")
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
// beside hookshot.exe, or the nearest folder above the game that has
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

CoopTitleMenu::Options ReadOptions() {
    const auto kit = FindKit();
    return CoopTitleMenu::Options::Read(ReadText(CoopIni()), kit ? ReadText(*kit / "coop-options.ini") : std::string());
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

// One of the kit's .cmd files in its own minimised window (`start /min`), with
// these variables (secrets go here, never on a command line). False with why.
bool LaunchKit(const fs::path& kit, const std::string& cmd, const std::vector<std::string>& args,
               const std::vector<std::pair<std::string, std::string>>& env, const std::string& title,
               std::string& why) {
#ifdef _WIN32
    std::wstring line = L"cmd.exe /d /c start \"" + Wide(title) + L"\" /min \"" + (kit / cmd).wstring() + L"\"";
    for (const auto& a : args) {
        line += L" " + Wide(a);   // (addresses and ports only: no spaces or quotes)
    }
    std::vector<std::pair<std::string, std::string>> before;
    for (const auto& [k, v] : env) {
        before.emplace_back(k, Env(k.c_str()));
        SetEnv(k, v);
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;   // (the cmd.exe that runs `start`; the window it starts is minimised)
    PROCESS_INFORMATION pi{};
    std::wstring dir = kit.wstring();
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                   dir.c_str(), &si, &pi);
    const DWORD err = GetLastError();
    for (const auto& [k, v] : before) {
        SetEnv(k, v);
    }
    if (!ok) {
        why = "could not start " + cmd + " (Windows error " + std::to_string(err) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    (void)kit, (void)cmd, (void)args, (void)env, (void)title;
    why = "hosting and joining from the title are Windows only for now: use Host.sh or Join.sh";
    return false;
#endif
}

// --- What the menu asked for (under g_mutex) ---

void SetCoopEnvironment(const CoopTitleMenu::Options& o) {
    for (const auto& [k, v] : o.Environment()) {
        SetEnv(k, v);
    }
}

bool WriteCoopIni(const std::string& server, const std::string& name, const std::string& password) {
    const fs::path ini = CoopIni();
    const std::string text = CoopTitleMenu::CoopIniFor(ReadText(ini), server, name, password);
    if (!WriteText(ini, text)) {
        LOG_ERROR(Core, "coop: title menu: {} could not be written", ini.string());
        return false;
    }
    return true;
}

void Host() {
    if (g_coop_on) {
        g_menu.SetStatus({"Co-op is on already in this game.", "To host or join another, quit the game and start it again."});
        g_sounds.push_back(Sound::kRefuse);
        return;
    }
    const auto kit = FindKit();
    if (!kit) {
        g_menu.SetStatus({"No co-op kit found with this game (server\\coop_server.exe).",
                          "Start the game from the kit (Host-Hookshot.bat), or put the kit's folder in coop-kit.txt."});
        g_sounds.push_back(Sound::kRefuse);
        return;
    }
    const CoopTitleMenu::Options o = g_menu.options();
    const bool made = o.password.empty();
    const std::string password = made ? coop::server::nat::make_password() : o.password;
    const std::string options = ReadText(*kit / "coop-options.ini");
    std::vector<std::string> args{"27510"};
    const auto allow = CoopTitleMenu::IniGet(options, "server_allow");
    if (allow && !allow->empty() && allow->find_first_not_of("0123456789./,") == std::string::npos) {
        args.insert(args.end(), {"-Allow", *allow});
    }
    const auto bind = CoopTitleMenu::IniGet(options, "server_bind");
    if (bind && !bind->empty() && bind->find_first_not_of("0123456789.") == std::string::npos) {
        args.insert(args.end(), {"-Bind", *bind});
    }
    std::vector<std::pair<std::string, std::string>> env;
    if (made) {
        env.emplace_back("OOT_COOP_SESSION_PASSWORD", password);
    }
    g_clip_before = ClipboardText();
    std::string why;
    if (!LaunchKit(*kit, "start-server.cmd", args, env, "Co-op server", why)) {
        LOG_ERROR(Core, "coop: title menu: host: {}", why);
        g_menu.SetStatus({"The server did not start: " + why + "."});
        g_sounds.push_back(Sound::kRefuse);
        return;
    }
    if (!WriteCoopIni("127.0.0.1:27510", o.name, password)) {
        g_menu.SetStatus({"coop.ini beside the game could not be written."});
        g_sounds.push_back(Sound::kRefuse);
        return;
    }
    SetCoopEnvironment(o);
    LOG_INFO(Core, "coop: title menu: host: the server started from {} (its own window, minimised); joining it",
             Utf8Path(*kit));
    g_watch_until_ms = NowMs() + 60000;
    g_invite.clear();
    g_doing = "Multiplayer: hosting";
    g_menu.ShowPage(Page::kHosting);
    g_menu.SetStatus({"The server runs in its own window (minimised on the taskbar): keep it open while you play.",
                      "Waiting for the invite code..."});
    g_start_coop = true;
}

void Join(const std::string& text) {
    if (g_coop_on) {
        g_menu.SetStatus({"Co-op is on already in this game.", "To host or join another, quit the game and start it again."});
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
    const CoopTitleMenu::Options o = g_menu.options();
    std::string server, password, shown;
    if (const auto* inv = std::get_if<CoopTitleMenu::JoinInvite>(&target)) {
        const auto kit = FindKit();
        if (!kit) {
            g_menu.SetStatus({"No co-op kit found with this game (server\\coop_server.exe): an invite code needs its join helper."});
            g_sounds.push_back(Sound::kRefuse);
            return;
        }
        std::string why;
        if (!LaunchKit(*kit, "start-join.cmd", {"-HostAddress", inv->host, "-GamePort", "27512"},
                       {{"OOT_COOP_JOIN_PASSWORD", inv->password}}, "Co-op join helper", why)) {
            LOG_ERROR(Core, "coop: title menu: join: {}", why);
            g_menu.SetStatus({"The join helper did not start: " + why + "."});
            g_sounds.push_back(Sound::kRefuse);
            return;
        }
        server = "127.0.0.1:27512";
        password = inv->password;
        shown = "the host (through the join helper window, minimised)";
    } else {
        const auto& a = std::get<CoopTitleMenu::JoinAddress>(target);
        server = a.server;
        password = a.password;
        shown = a.server;
    }
    if (!WriteCoopIni(server, o.name, password)) {
        g_menu.SetStatus({"coop.ini beside the game could not be written."});
        g_sounds.push_back(Sound::kRefuse);
        return;
    }
    SetCoopEnvironment(o);
    LOG_INFO(Core, "coop: title menu: join: {}", std::holds_alternative<CoopTitleMenu::JoinInvite>(target)
                                                    ? std::string("an invite code, through the join helper")
                                                    : "the address " + server);
    g_doing = "Multiplayer: joined";
    g_menu.ShowPage(Page::kJoining);
    g_menu.SetStatus({"Joining " + shown + "...", "Choose your file and play: the others show up when connected."});
    g_start_coop = true;
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
    case Request::Kind::kCopyInvite:
        if (g_invite.empty()) {
            g_menu.SetStatus({"No invite code yet: the server window shows it when it has one."});
        } else if (SetClipboardText(g_invite)) {
            g_menu.SetStatus({"Invite code copied: " + g_invite, "Send it to your friends; they choose Join a game and paste it."});
        }
        break;
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
        g_menu.SetStatus({g_doing.empty() ? "Co-op is on in this game." : g_doing.substr(13) == "hosting"
                                                                              ? "You are hosting."
                                                                              : "You joined a game."});
    }
    LOG_INFO(Core, "coop: title menu: opened");
}

// The server's invite code, once it shows on the clipboard (start-server.ps1
// copies it there; it never writes it to a file).
void WatchInvite() {
    if (g_watch_until_ms == 0 || !g_invite.empty()) {
        return;
    }
    if (NowMs() > g_watch_until_ms) {
        g_watch_until_ms = 0;
        g_menu.SetStatus({"The server window shows the invite code (or an address to send): open it from the taskbar."});
        return;
    }
    static std::uint64_t next = 0;
    if (NowMs() < next) {
        return;
    }
    next = NowMs() + 500;
    const std::string clip = ClipboardText();
    if (clip.empty() || clip == g_clip_before) {
        return;
    }
    const auto r = coop::server::nat::read_invite(clip);
    if (r.error != coop::server::nat::CodeError::kOk) {
        return;
    }
    g_invite = clip.substr(0, clip.find_first_of("\r\n"));
    g_watch_until_ms = 0;
    LOG_INFO(Core, "coop: title menu: host: the invite code is on the clipboard");
    if (g_menu.page() == Page::kHosting) {
        g_menu.SetStatus({"Invite code (copied to the clipboard):", g_invite,
                          "Send it to your friends; they choose Join a game and paste it."});
    }
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
    if (g_screen.exchange(screen) != screen || event == HOS_COOP_TITLE_SETUP) {
        if (event == HOS_COOP_TITLE_SETUP) {
            LogButtons(screen);
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
        WatchInvite();
    }
    PlaySounds(ctx);
}

void OnFrame(void* ctx) {
    {
        std::lock_guard lock(g_mutex);
        WatchInvite();
    }
    PlaySounds(ctx);
}

std::uint64_t OnPad(std::uint64_t buttons, std::int32_t stick[4]) {
    std::lock_guard lock(g_mutex);
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
    bool open = (down & kMinus) != 0;
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
    if (!HasScratch() || !TitleUp()) {
        return std::nullopt;
    }
    return CoopDraw::TitleView{g_doing.empty() ? std::string("Multiplayer") : g_doing, std::nullopt};
}

bool TakeStartCoop() {
    return g_start_coop.exchange(false);
}

void CoopStarted(bool on) {
    std::lock_guard lock(g_mutex);
    g_coop_on = on;
    if (!on) {
        g_doing.clear();
        g_menu.SetStatus({"Co-op did not start: hookshot_log.txt says why.", "Quit the game and start it again to try once more."});
        g_sounds.push_back(Sound::kRefuse);
    }
}

}  // namespace CoopTitle
