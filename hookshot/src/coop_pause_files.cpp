// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_pause_files.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include "coop_runtime.h"

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

namespace CoopFiles {

namespace fs = std::filesystem;

#ifdef _WIN32
namespace {
std::wstring Wide(const std::string& s);
std::string Narrow(const wchar_t* w);
}  // namespace
#endif

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

namespace {

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
}  // namespace

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
namespace {
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
}  // namespace
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

void SetCoopEnvironment(const CoopTitleMenu::Options& o) {
    for (const auto& [k, v] : o.Environment()) {
        SetEnv(k, v);
    }
}

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

}  // namespace CoopFiles
