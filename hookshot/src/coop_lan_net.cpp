// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_lan_net.h"

#include <mutex>
#include <set>
#include <string>

#include "common/logging.h"
#include "local_net.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace CoopLanNet {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
bool NonBlocking(Socket s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void CloseSocket(Socket s) { close(s); }
bool NonBlocking(Socket s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

constexpr std::uint64_t kLookLingerMs = 10000;    // the listener stays this long after the last look
constexpr std::uint64_t kNetworksEveryMs = 30000; // this PC's networks read again this often (a cable, a VPN)

std::mutex g_mutex;
Socket g_send = kNoSocket, g_listen = kNoSocket;
bool g_started = false;
std::uint64_t g_next_send_ms = 0, g_networks_ms = 0, g_last_look_ms = 0;
std::set<std::uint32_t> g_targets;   // broadcast addresses (host order)
CoopLan::List g_list;
bool g_said_send = false, g_said_listen = false;

bool Started() {
    if (g_started) return true;
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;   // (counted: the module's ENet has its own)
#endif
    g_started = true;
    return true;
}

Socket Open(bool listen) {
    const Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return kNoSocket;
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
    if (listen) {
        // (every game window on this PC hears the announcements: each binds the port, the system gives each a copy)
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
#ifdef SO_REUSEPORT
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<const char*>(&on), sizeof on);
#endif
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_port = htons(CoopLan::kPort);
        at.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, reinterpret_cast<const sockaddr*>(&at), sizeof at) != 0) {
            CloseSocket(s);
            return kNoSocket;
        }
    }
    if (!NonBlocking(s)) {
        CloseSocket(s);
        return kNoSocket;
    }
    return s;
}

void ReadNetworks(std::uint64_t now_ms) {
    if (g_networks_ms != 0 && now_ms - g_networks_ms < kNetworksEveryMs) return;
    g_networks_ms = now_ms;
    g_targets.clear();
    g_targets.insert(0xffffffffu);   // (255.255.255.255: this network, whatever it is)
    g_targets.insert(0x7f000001u);   // (127.0.0.1: another game window on this PC)
    for (const auto& net : coop::server::nat::local_networks()) {
        if (const std::uint32_t b = coop::server::nat::broadcast_of(net); b != 0) g_targets.insert(b);
    }
}

}  // namespace

void Announce(const CoopLan::Game& game, std::uint64_t now_ms) {
    std::lock_guard lock(g_mutex);
    if (now_ms < g_next_send_ms) return;
    g_next_send_ms = now_ms + CoopLan::kEveryMs;
    if (!Started()) return;
    if (g_send == kNoSocket) {
        g_send = Open(false);
        if (g_send == kNoSocket) {
            if (!g_said_send) LOG_INFO(Core, "coop: lan: no socket to announce this game on the local network");
            g_said_send = true;
            return;
        }
        LOG_INFO(Core, "coop: lan: this game is announced on the local network (port {}, every {} s)", CoopLan::kPort,
                 CoopLan::kEveryMs / 1000);
    }
    ReadNetworks(now_ms);
    const std::string datagram = CoopLan::Encode(game);
    for (const std::uint32_t target : g_targets) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(CoopLan::kPort);
        to.sin_addr.s_addr = htonl(target);
        sendto(g_send, datagram.data(), static_cast<int>(datagram.size()), 0, reinterpret_cast<const sockaddr*>(&to),
               sizeof to);   // (a network that cannot be reached: that one only)
    }
}

void Look(bool on, std::uint64_t now_ms) {
    std::lock_guard lock(g_mutex);
    if (on) g_last_look_ms = now_ms;
    if (!on) {
        if (g_listen != kNoSocket && now_ms - g_last_look_ms > kLookLingerMs) {
            CloseSocket(g_listen);
            g_listen = kNoSocket;
            g_list.Clear();
        }
        return;
    }
    if (g_listen == kNoSocket) {
        if (!Started()) return;
        g_listen = Open(true);
        if (g_listen == kNoSocket) {
            if (!g_said_listen) LOG_INFO(Core, "coop: lan: the local network's games cannot be heard (port {} taken)", CoopLan::kPort);
            g_said_listen = true;
            return;
        }
        LOG_INFO(Core, "coop: lan: looking for games on the local network (port {})", CoopLan::kPort);
    }
    char buf[CoopLan::kMaxDatagram + 1];
    for (int i = 0; i < 64; ++i) {   // (at most this many a frame)
        sockaddr_in from{};
#ifdef _WIN32
        int from_size = sizeof from;
#else
        socklen_t from_size = sizeof from;
#endif
        const auto n = recvfrom(g_listen, buf, static_cast<int>(sizeof buf), 0, reinterpret_cast<sockaddr*>(&from),
                                &from_size);
        if (n <= 0) break;   // (nothing more now)
        const auto game = CoopLan::Decode(std::string_view(buf, static_cast<std::size_t>(n)));
        if (!game) continue;
        char address[INET_ADDRSTRLEN] = {};
        if (inet_ntop(AF_INET, &from.sin_addr, address, sizeof address) == nullptr) continue;
        g_list.Add(address, *game, now_ms);
    }
}

void Forget() {
    std::lock_guard lock(g_mutex);
    g_list.Clear();
}

std::vector<CoopLan::Heard> Games(std::uint64_t now_ms) {
    std::lock_guard lock(g_mutex);
    return g_list.Now(now_ms);
}

}  // namespace CoopLanNet
