// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam_net.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "steam_model.hpp"
#include "third_party/plugin_api.h"

namespace CoopNx2navi::SteamNet {
namespace {

constexpr std::size_t kMaxInvites = 8;
constexpr std::uint64_t kHelloEveryMs = 1000;
constexpr std::uint64_t kHostWaitMs = 15000;
constexpr int kFriendsMax = 512;

std::mutex g_m;     // everything below
std::mutex g_life;  // signing in and out, one at a time
const Nx2PluginApi* g_api = nullptr;
const Nx2Steam* g_steam = nullptr;
void (*g_log)(const char*) = nullptr;
bool g_wsa = false;
bool g_started = false;
std::uint64_t g_generation = 0;  // counts Start calls: a sign-out asked before a newer one is dropped
bool g_host = false;
std::string g_secret;
std::uint16_t g_server_port = 0;
std::uint64_t g_owner = 0;
Link g_link = Link::none;
Fail g_fail = Fail::none;
std::uint16_t g_port = 0;
std::vector<Invitation> g_invites;
std::string g_asked;
std::uint64_t g_serial = 0;
std::uint64_t g_hello_at = 0;
std::uint64_t g_ask_since = 0;

// One UDP socket on 127.0.0.1 for each game on the other side: the host has
// one per member, connected to its server; a joiner has one, which its own
// client sends to (the client's address is learnt from its first datagram).
struct Peer {
    SOCKET socket = INVALID_SOCKET;
    bool closing = false;  // the relay thread closes it
    SteamModel::Joiner joiner;
    std::uint16_t number = 0;
};
std::map<std::uint64_t, Peer> g_peers;
sockaddr_in g_client{};
bool g_have_client = false;
std::thread g_relay;
std::atomic<bool> g_relay_stop{false};
std::uint64_t g_relayed_in = 0, g_relayed_out = 0;

void Log(const char* format, ...) {
    if (g_log == nullptr) {
        return;
    }
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_log(line);
}

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::wstring Env(const wchar_t* name) {
    std::wstring value(512, L'\0');
    const DWORD size = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    value.resize(size < value.size() ? size : 0);
    return value;
}

Avail AvailOf(const Nx2SteamStatus& st) {
    switch (st.state) {
    case NX2_STEAM_READY: return Avail::ready;
    case NX2_STEAM_STARTING: return Avail::checking;
    default: break;
    }
    switch (st.why) {
    case NX2_STEAM_WHY_NO_LIBRARY: return Avail::no_library;
    case NX2_STEAM_WHY_OLD_LIBRARY: return Avail::old_library;
    case NX2_STEAM_WHY_NO_CLIENT: return Avail::no_client;
    case NX2_STEAM_WHY_GAME_COOP: return Avail::game_coop;
    case NX2_STEAM_WHY_IN_USE: return Avail::in_use;
    case NX2_STEAM_WHY_NONE: return st.state == NX2_STEAM_OFF ? Avail::checking : Avail::other;
    default: return Avail::other;
    }
}

Nx2SteamStatus Status() {
    Nx2SteamStatus st{};
    st.size = sizeof(st);
    if (g_steam != nullptr) {
        g_steam->status(&st);
    }
    return st;
}

Fail FailOf(int code) {
    switch (code) {
    case NX2_STEAM_FAIL_CREATE: return Fail::create;
    case NX2_STEAM_FAIL_NO_ROOM: return Fail::no_room;
    case NX2_STEAM_FAIL_FULL: return Fail::full;
    case NX2_STEAM_FAIL_REFUSED: return Fail::refused;
    case NX2_STEAM_FAIL_NOT_OURS: return Fail::not_ours;
    default: return Fail::other;
    }
}

// A UDP socket on 127.0.0.1 at any free port, not blocking; `to_port` above 0:
// connected to that port of this PC.
SOCKET LocalSocket(std::uint16_t to_port, std::uint16_t* bound) {
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        return s;
    }
    sockaddr_in self{};
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int size = sizeof(self);
    u_long on = 1;
    // A datagram sent to a port nobody listens on (the server, before it is up)
    // must not make the next receive fail: SIO_UDP_CONNRESET off.
    BOOL reset = FALSE;
    DWORD got = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &reset, sizeof(reset), nullptr, 0, &got, nullptr, nullptr);
    bool ok = bind(s, reinterpret_cast<const sockaddr*>(&self), sizeof(self)) == 0 &&
              getsockname(s, reinterpret_cast<sockaddr*>(&self), &size) == 0 &&
              ioctlsocket(s, FIONBIO, &on) == 0;
    if (ok && to_port != 0) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        to.sin_port = htons(to_port);
        ok = connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0;
    }
    if (!ok) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    if (bound != nullptr) {
        *bound = ntohs(self.sin_port);
    }
    return s;
}

std::uint16_t FreePort() {
    std::uint16_t port = 0;
    const SOCKET s = LocalSocket(0, &port);
    if (s != INVALID_SOCKET) {
        closesocket(s);
    }
    return port;
}

bool Random(std::array<std::uint8_t, 16>& out) {
    HCRYPTPROV provider = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        return false;
    }
    const bool ok = CryptGenRandom(provider, static_cast<DWORD>(out.size()), out.data()) != FALSE;
    CryptReleaseContext(provider, 0);
    return ok;
}

// Under g_m.
void Failed(Fail fail) {
    g_link = Link::failed;
    g_fail = fail;
    ++g_serial;
}

void ClosePeers() {
    // Under g_m: the relay thread closes the sockets.
    for (auto& [id, peer] : g_peers) {
        peer.closing = true;
    }
    g_have_client = false;
}

// Under g_m: the host's socket for a member, made at its first datagram.
Peer* PeerOf(std::uint64_t id) {
    const auto found = g_peers.find(id);
    if (found != g_peers.end()) {
        return found->second.closing ? nullptr : &found->second;
    }
    if (!g_host || g_peers.size() >= NX2_STEAM_ROOM_MAX * 2) {
        return nullptr;
    }
    Peer peer;
    peer.socket = LocalSocket(g_server_port, nullptr);
    if (peer.socket == INVALID_SOCKET) {
        Log("steam: no local socket for a member (Windows error %d)", WSAGetLastError());
        return nullptr;
    }
    return &g_peers.emplace(id, std::move(peer)).first->second;
}

void SendWrapped(std::uint64_t to, Peer& peer, const std::uint8_t* bytes, std::size_t size) {
    for (const auto& datagram : SteamModel::Wrap({bytes, size}, peer.number)) {
        g_steam->send(to, datagram.data(), datagram.size(), 0);
    }
    ++peer.number;
}

void AddInvite(std::uint64_t room, std::uint64_t from, bool asked) {
    // Under g_m.
    char code[NX2_STEAM_CODE_CAP] = "";
    if (!g_steam->room_code(room, code) || code[0] == 0) {
        return;
    }
    Invitation invite;
    invite.room = code;
    invite.asked = asked;
    static Nx2SteamFriend friends[kFriendsMax];
    const int n = g_steam->friends(friends, kFriendsMax);
    for (int i = 0; i < n; ++i) {
        if (friends[i].id == from) {
            invite.from = friends[i].name;
        }
    }
    std::erase_if(g_invites, [&](const Invitation& i) { return i.room == invite.room; });
    g_invites.insert(g_invites.begin(), invite);
    if (g_invites.size() > kMaxInvites) {
        g_invites.resize(kMaxInvites);
    }
    if (asked) {
        g_asked = invite.room;
    }
    Log("steam: an invitation to a room came%s (%u waiting)", asked ? ", accepted in Steam" : "",
        static_cast<unsigned>(g_invites.size()));
    ++g_serial;
}

void OnMessage(std::uint64_t from, const std::uint8_t* bytes, std::size_t size) {
    // Under g_m, on the game's Steam thread.
    const auto parsed = SteamModel::Parse({bytes, size});
    if (!parsed) {
        return;
    }
    if (parsed->kind == SteamModel::Kind::hello) {
        if (g_host && g_link == Link::ready) {
            const auto answer = SteamModel::Secret(g_secret);
            g_steam->send(from, answer.data(), answer.size(), 1);
        }
        return;
    }
    if (parsed->kind == SteamModel::Kind::secret) {
        if (g_host || from != g_owner || g_link != Link::asking) {
            return;
        }
        if (parsed->version != SteamModel::kVersion) {
            Log("steam: the host's mod speaks version %u of this, ours %u", parsed->version, SteamModel::kVersion);
            Failed(Fail::version);
            return;
        }
        g_secret = parsed->secret;
        g_link = Link::ready;
        ++g_serial;
        Log("steam: the host answered; the session goes through Steam (this game's client connects to "
            "127.0.0.1:%u)", static_cast<unsigned>(g_port));
        return;
    }
    if (g_link != Link::ready || (!g_host && from != g_owner)) {
        return;
    }
    Peer* peer = g_host ? PeerOf(from) : g_peers.empty() ? nullptr : &g_peers.begin()->second;
    if (peer == nullptr || peer->closing) {
        return;
    }
    std::vector<std::uint8_t> whole;
    std::span<const std::uint8_t> enet = parsed->bytes;
    if (parsed->kind == SteamModel::Kind::part) {
        auto joined = peer->joiner.Add(*parsed);
        if (!joined) {
            return;
        }
        whole = std::move(*joined);
        enet = whole;
    }
    ++g_relayed_in;
    if (g_host) {
        send(peer->socket, reinterpret_cast<const char*>(enet.data()), static_cast<int>(enet.size()), 0);
    } else if (g_have_client) {
        sendto(peer->socket, reinterpret_cast<const char*>(enet.data()), static_cast<int>(enet.size()), 0,
               reinterpret_cast<const sockaddr*>(&g_client), sizeof(g_client));
    }
}

void OnSteam(const Nx2SteamEvent* e, void*) {
    std::lock_guard lock(g_m);
    switch (e->kind) {
    case NX2_STEAM_EV_STATE: {
        const Nx2SteamStatus st = Status();
        Log("steam: %s", st.state == NX2_STEAM_READY
                             ? (st.local ? "ready (the game's stand-in for Steam: two games on one PC)"
                                         : "signed in")
                             : st.says);
        ++g_serial;
        break;
    }
    case NX2_STEAM_EV_ROOM_ENTERED: {
        if (g_link != Link::entering) {
            break;
        }
        const Nx2SteamStatus st = Status();
        g_owner = st.owner;
        if (g_host) {
            g_link = Link::ready;
            g_port = g_server_port;
            Log("steam: the room is made (its ID is on the Host over Steam page); members reach the "
                "server on 127.0.0.1:%u through Steam", static_cast<unsigned>(g_server_port));
        } else {
            Peer peer;
            peer.socket = LocalSocket(0, &g_port);
            if (peer.socket == INVALID_SOCKET) {
                Log("steam: no local socket (Windows error %d)", WSAGetLastError());
                Failed(Fail::other);
                break;
            }
            g_peers.clear();
            g_peers.emplace(g_owner, std::move(peer));
            g_have_client = false;
            g_link = Link::asking;
            g_ask_since = NowMs();
            g_hello_at = 0;
            Log("steam: in the host's room; asking the host for the session");
        }
        ++g_serial;
        break;
    }
    case NX2_STEAM_EV_ROOM_FAILED:
        if (g_link == Link::entering) {
            Log("steam: the room was not %s (reason %d)", g_host ? "made" : "entered", e->code);
            Failed(FailOf(e->code));
        }
        break;
    case NX2_STEAM_EV_MEMBER_JOINED:
        ++g_serial;
        break;
    case NX2_STEAM_EV_MEMBER_LEFT:
        if (g_host) {
            if (const auto found = g_peers.find(e->a); found != g_peers.end()) {
                found->second.closing = true;
            }
        } else if (e->a == g_owner && g_link != Link::none) {
            Log("steam: the host left the room");
            Failed(Fail::lost);
        }
        ++g_serial;
        break;
    case NX2_STEAM_EV_OWNER_CHANGED:
        if (!g_host && g_link != Link::none && g_link != Link::failed) {
            Log("steam: the room changed hands: the host is gone");
            Failed(Fail::lost);
        }
        break;
    case NX2_STEAM_EV_INVITED: AddInvite(e->a, e->b, false); break;
    case NX2_STEAM_EV_JOIN_ASKED: AddInvite(e->a, e->b, true); break;
    case NX2_STEAM_EV_MESSAGE:
        OnMessage(e->a, static_cast<const std::uint8_t*>(e->data), e->n);
        break;
    case NX2_STEAM_EV_LOST:
        if (g_link != Link::none) {
            Log("steam: the room is lost (Steam closed or lost its connection)");
            Failed(Fail::lost);
        }
        break;
    default: break;
    }
}

// Moves the datagrams of the local sockets to Steam; a joiner's also says
// hello to the host until the secret came.
void Relay() {
    std::vector<std::uint8_t> buffer(4096);
    std::vector<std::pair<std::uint64_t, SOCKET>> sockets;
    std::uint64_t said_at = NowMs();
    while (!g_relay_stop.load()) {
        sockets.clear();
        {
            std::lock_guard lock(g_m);
            for (auto it = g_peers.begin(); it != g_peers.end();) {
                if (it->second.closing) {
                    closesocket(it->second.socket);
                    it = g_peers.erase(it);
                } else {
                    sockets.emplace_back(it->first, it->second.socket);
                    ++it;
                }
            }
            const std::uint64_t now = NowMs();
            if (!g_host && g_link == Link::asking) {
                if (now - g_ask_since > kHostWaitMs) {
                    Log("steam: the room's host did not answer in %u s", static_cast<unsigned>(kHostWaitMs / 1000));
                    Failed(Fail::host_silent);
                } else if (now - g_hello_at >= kHelloEveryMs) {
                    const auto hello = SteamModel::Hello();
                    g_steam->send(g_owner, hello.data(), hello.size(), 1);
                    g_hello_at = now;
                }
            }
            if (now - said_at >= 60000 && g_link == Link::ready) {
                said_at = now;
                Log("steam: %llu datagrams from Steam, %llu to Steam so far, %u local socket%s",
                    static_cast<unsigned long long>(g_relayed_in), static_cast<unsigned long long>(g_relayed_out),
                    static_cast<unsigned>(g_peers.size()), g_peers.size() == 1 ? "" : "s");
            }
        }
        if (sockets.empty()) {
            Sleep(20);
            continue;
        }
        fd_set readable;
        FD_ZERO(&readable);
        for (const auto& [id, s] : sockets) {
            FD_SET(s, &readable);
        }
        timeval wait{0, 20000};
        if (select(0, &readable, nullptr, nullptr, &wait) <= 0) {
            continue;
        }
        for (const auto& [id, s] : sockets) {
            if (!FD_ISSET(s, &readable)) {
                continue;
            }
            for (int burst = 0; burst < 64; ++burst) {
                sockaddr_in from{};
                int from_size = sizeof(from);
                const int got = recvfrom(s, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()),
                                         0, reinterpret_cast<sockaddr*>(&from), &from_size);
                if (got <= 0) {
                    break;
                }
                std::lock_guard lock(g_m);
                const auto found = g_peers.find(id);
                if (found == g_peers.end() || found->second.closing) {
                    break;
                }
                if (g_link != Link::ready) {
                    continue;  // nobody to carry it to: dropped, as a network drops a datagram
                }
                if (!g_host) {
                    // Only this PC's own client talks to the joiner's socket.
                    if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
                        continue;
                    }
                    g_client = from;
                    g_have_client = true;
                }
                ++g_relayed_out;
                SendWrapped(id, found->second, buffer.data(), static_cast<std::size_t>(got));
            }
        }
    }
    std::lock_guard lock(g_m);
    for (auto& [id, peer] : g_peers) {
        closesocket(peer.socket);
    }
    g_peers.clear();
}

void EnsureRelay() {
    // Under g_life.
    if (!g_relay.joinable()) {
        g_relay_stop = false;
        g_relay = std::thread(Relay);
    }
}

void StopRelay() {
    // Under g_life, not under g_m.
    if (g_relay.joinable()) {
        g_relay_stop = true;
        g_relay.join();
    }
}

void SignOut() {
    // Under g_life, not under g_m; never on the game's Steam thread.
    StopRelay();
    g_steam->stop();
    std::lock_guard lock(g_m);
    g_started = false;
    g_link = Link::none;
    g_fail = Fail::none;
    g_host = false;
    g_secret.clear();
    g_invites.clear();
    g_asked.clear();
    ++g_serial;
    Log("steam: signed out");
}

}  // namespace

void Init(const Nx2PluginApi* api, void (*log)(const char* line)) {
    std::lock_guard lock(g_m);
    if (g_api != nullptr || api == nullptr) {
        return;
    }
    g_api = api;
    g_log = log;
    if (NX2_HAS(api, Nx2PluginApi, steam) && api->steam != nullptr && Env(L"OOT_COOP_STEAM") != L"0") {
        g_steam = api->steam();
    }
    if (!g_wsa) {
        WSADATA data{};
        g_wsa = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
}

Avail Precheck() {
    {
        std::lock_guard lock(g_m);
        if (g_steam == nullptr || !g_wsa) {
            return Avail::old_game;
        }
        if (g_started) {
            return AvailOf(Status());
        }
    }
    if (!Env(L"NX2NAVI_PLUGIN_STEAM_LOCAL").empty()) {
        return Avail::ready;  // the game's stand-in for Steam
    }
    const Nx2GameInfo* info = g_api->game_info();
    if (info != nullptr && info->game_dir != nullptr) {
        std::wstring path(1024, L'\0');
        const int size = MultiByteToWideChar(CP_UTF8, 0, info->game_dir, -1, path.data(),
                                             static_cast<int>(path.size()));
        path.resize(size > 0 ? static_cast<std::size_t>(size - 1) : 0);
        path += L"\\steam_api64.dll";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return Avail::no_library;
        }
    }
    // Steam's own note of its running client: the process id, 0 when it is closed.
    DWORD pid = 0;
    DWORD size = sizeof(pid);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"pid", RRF_RT_REG_DWORD,
                     nullptr, &pid, &size) != ERROR_SUCCESS ||
        pid == 0) {
        return Avail::no_client;
    }
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED ? Avail::ready : Avail::no_client;
    }
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running ? Avail::ready : Avail::no_client;
}

bool Start() {
    std::lock_guard life(g_life);
    {
        std::lock_guard lock(g_m);
        if (g_steam == nullptr) {
            return false;
        }
        ++g_generation;
        if (g_started) {
            return true;
        }
    }
    const bool ok = g_steam->start(SteamModel::kRoomTag, OnSteam, nullptr) == 1;
    std::lock_guard lock(g_m);
    g_started = ok;
    ++g_serial;
    if (!ok) {
        const Nx2SteamStatus st = Status();
        Log("steam: the game did not start Steam for this mod: %s", st.says);
    } else {
        Log("steam: signing in (Steam shows you as playing Spacewar while co-op over Steam is in use)");
    }
    return ok;
}

void StopIfIdle() {
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(g_m);
        if (!g_started || (g_link != Link::none && g_link != Link::failed)) {
            return;
        }
        generation = g_generation;
    }
    // Not on the caller's thread (the game's): signing out waits for Steam.
    std::thread([generation] {
        std::lock_guard life(g_life);
        {
            std::lock_guard lock(g_m);
            if (!g_started || generation != g_generation || (g_link != Link::none && g_link != Link::failed)) {
                return;
            }
        }
        SignOut();
    }).detach();
}

void Shutdown() {
    std::lock_guard life(g_life);
    {
        std::lock_guard lock(g_m);
        if (!g_started) {
            return;
        }
        ++g_generation;
    }
    SignOut();
}

View Get() {
    std::lock_guard lock(g_m);
    View v;
    v.serial = g_serial;
    if (g_steam == nullptr) {
        return v;
    }
    v.started = g_started;
    if (!g_started) {
        v.avail = Avail::checking;  // not known: Precheck says what can be said
        return v;
    }
    const Nx2SteamStatus st = Status();
    v.avail = AvailOf(st);
    v.local = st.local != 0;
    v.name = st.name;
    v.owner = st.room != 0 && st.owner == st.self;
    v.room = st.room_code;
    Nx2SteamMember members[NX2_STEAM_ROOM_MAX];
    const int n = g_steam->members(members, NX2_STEAM_ROOM_MAX);
    for (int i = 0; i < n; ++i) {
        v.members.push_back({members[i].id, members[i].name, members[i].self != 0, members[i].owner != 0});
    }
    v.invites = g_invites;
    static Nx2SteamFriend friends[kFriendsMax];
    v.friends = static_cast<std::size_t>(std::max(0, g_steam->friends(friends, kFriendsMax)));
    v.link = g_link;
    v.fail = g_fail;
    v.port = g_port;
    return v;
}

std::vector<Menu::SteamFriend> Friends() {
    std::lock_guard lock(g_m);
    std::vector<Menu::SteamFriend> out;
    if (g_steam == nullptr || !g_started) {
        return out;
    }
    static Nx2SteamFriend friends[kFriendsMax];
    const int n = g_steam->friends(friends, kFriendsMax);
    for (int i = 0; i < n; ++i) {
        out.push_back({friends[i].id, friends[i].name, friends[i].online != 0, friends[i].in_app != 0});
    }
    return out;
}

bool Invite(std::uint64_t friend_id) {
    std::lock_guard lock(g_m);
    const bool ok = g_steam != nullptr && g_started && g_steam->invite(friend_id) == 1;
    Log("steam: an invitation to this room was %s", ok ? "handed to Steam" : "not taken by the game");
    return ok;
}

std::string TakeAsked() {
    std::lock_guard lock(g_m);
    std::string out;
    out.swap(g_asked);
    return out;
}

bool Host(std::uint16_t server_port, std::string secret) {
    std::lock_guard life(g_life);
    {
        std::lock_guard lock(g_m);
        if (g_steam == nullptr || !g_started || g_link == Link::entering || g_link == Link::asking ||
            g_link == Link::ready) {
            return false;
        }
        g_host = true;
        g_secret = std::move(secret);
        g_server_port = server_port;
        g_link = Link::entering;
        g_fail = Fail::none;
        ClosePeers();
        ++g_serial;
    }
    EnsureRelay();
    if (g_steam->room_create(NX2_STEAM_ROOM_MAX) != 1) {
        std::lock_guard lock(g_m);
        Log("steam: the game did not take the request for a room");
        Failed(Fail::create);
        return false;
    }
    return true;
}

bool Join(const std::string& room) {
    std::lock_guard life(g_life);
    {
        std::lock_guard lock(g_m);
        if (g_steam == nullptr || !g_started || g_link == Link::entering || g_link == Link::asking ||
            g_link == Link::ready) {
            return false;
        }
        g_host = false;
        g_secret.clear();
        g_link = Link::entering;
        g_fail = Fail::none;
        ClosePeers();
        ++g_serial;
    }
    EnsureRelay();
    if (g_steam->room_join(room.c_str()) != 1) {
        std::lock_guard lock(g_m);
        Log("steam: that is no room ID, or the game did not take the request");
        Failed(Fail::no_room);
        return false;
    }
    return true;
}

void Leave() {
    std::lock_guard lock(g_m);
    if (g_steam == nullptr || !g_started) {
        return;
    }
    g_steam->room_leave();
    ClosePeers();
    g_link = Link::none;
    g_fail = Fail::none;
    ++g_serial;
}

std::string Secret() {
    std::lock_guard lock(g_m);
    return g_link == Link::ready ? g_secret : std::string();
}

namespace {

// Signed in, waiting up to 20 s for Steam's answer. The reason when not.
Fail Ready(const std::function<bool(unsigned)>& sleep) {
    if (!Start()) {
        return Fail::unavailable;
    }
    for (int tries = 0; tries < 200; ++tries) {
        const Avail avail = Get().avail;
        if (avail == Avail::ready) {
            return Fail::none;
        }
        if (avail != Avail::checking || sleep(100)) {
            return Fail::unavailable;
        }
    }
    return Fail::unavailable;
}

Session Wait(unsigned seconds, const std::function<bool(unsigned)>& sleep) {
    Session out;
    for (unsigned tries = 0; tries < seconds * 20; ++tries) {
        const View v = Get();
        if (v.link == Link::ready) {
            out.ok = true;
            out.port = v.port;
            out.secret = Secret();
            return out;
        }
        if (v.link == Link::failed || v.link == Link::none) {
            out.fail = v.fail == Fail::none ? Fail::other : v.fail;
            return out;
        }
        if (sleep(50)) {
            break;
        }
    }
    out.fail = Fail::other;
    return out;
}

}  // namespace

Session HostSession(const std::function<bool(std::uint16_t, const std::string&)>& start_server,
                    const std::function<bool(unsigned)>& sleep) {
    Session out;
    out.fail = Ready(sleep);
    if (out.fail != Fail::none) {
        return out;
    }
    std::array<std::uint8_t, 16> random{};
    const std::uint16_t port = FreePort();
    if (port == 0 || !Random(random)) {
        Log("steam: no free local port, or no random bytes from Windows");
        out.fail = Fail::server;
        return out;
    }
    const std::string secret = SteamModel::SecretFrom(random);
    if (!start_server(port, secret)) {
        out.fail = Fail::server;
        return out;
    }
    if (!Host(port, secret)) {
        out.fail = Get().fail == Fail::none ? Fail::create : Get().fail;
        return out;
    }
    out = Wait(20, sleep);
    if (!out.ok) {
        Leave();
    }
    return out;
}

Session JoinSession(const std::string& room, const std::function<bool(unsigned)>& sleep) {
    Session out;
    out.fail = Ready(sleep);
    if (out.fail != Fail::none) {
        return out;
    }
    if (!Join(room)) {
        out.fail = Get().fail == Fail::none ? Fail::no_room : Get().fail;
        Leave();
        return out;
    }
    out = Wait(40, sleep);
    if (!out.ok) {
        Leave();
    }
    return out;
}

}  // namespace CoopNx2navi::SteamNet
