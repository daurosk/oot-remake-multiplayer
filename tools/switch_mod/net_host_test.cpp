// PC test of the ExeFS module's net code (switch/net). The same sources the
// module links (coop_join.cpp, enet_nx.cpp, ENet, Monocypher), built for the
// PC by tools/switch_mod/net_host.py, with nn::socket here on the PC's sockets
// (BSD sockaddr layout: u8 len, u8 family, like the guest library takes).
//
//   net_host_test selftest
//   net_host_test join <ip> <port> <password> <nickname> <protocol> <seconds> <want_others>
//
// selftest: SHA-256 and HMAC-SHA256 known answers (FIPS 180-2 "abc",
// a known HMAC-SHA256 over 96 bytes), HELLO layout. join: joins the server, exit 0 when WELCOME
// opened and at least <want_others> other players were seen, 1 otherwise.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>

#include "../../switch/net/coop_join.h"

// ---- the platform the net code asks for ----
extern "C" void coop_plat_log(const char* text, size_t len) {
    std::printf("%.*s\n", static_cast<int>(len), text);
    std::fflush(stdout);
}
extern "C" uint64_t coop_plat_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
extern "C" void coop_plat_random(void* out, size_t len) {
    std::random_device rd;
    auto* p = static_cast<unsigned char*>(out);
    for (size_t i = 0; i < len; ++i) p[i] = static_cast<unsigned char>(rd());
}

// ---- nn::socket on the PC (only the int forms enet_nx.cpp uses) ----
struct sockaddr_bsd {
    uint8_t sa_len;
    uint8_t sa_family;
    char sa_data[14];
};
struct pollfd_nx {
    int fd;
    uint16_t events;
    uint16_t revents;
};
// The guest names are ::sockaddr / ::pollfd; on the PC those are the system's
// types, so the test's nn::socket takes its own and enet_nx.cpp is built with
// -Dsockaddr=sockaddr_bsd -Dpollfd=pollfd_nx (net_host.py).
namespace nn::socket {
int Socket(int domain, int type, int protocol) {
    if (domain != 2 || type != 2) return -1;
    const auto s = ::socket(AF_INET, SOCK_DGRAM, protocol);
    return static_cast<int>(s);
}
static sockaddr_in to_native(const sockaddr_bsd* a) {
    sockaddr_in n{};
    n.sin_family = AF_INET;
    std::memcpy(&n.sin_port, a->sa_data, 2);
    std::memcpy(&n.sin_addr, a->sa_data + 2, 4);
    return n;
}
static void to_bsd(const sockaddr_in& n, sockaddr_bsd* a) {
    std::memset(a, 0, sizeof *a);
    a->sa_len = sizeof *a;
    a->sa_family = 2;
    std::memcpy(a->sa_data, &n.sin_port, 2);
    std::memcpy(a->sa_data + 2, &n.sin_addr, 4);
}
int Bind(int fd, const sockaddr_bsd* addr, unsigned int len) {
    if (len != sizeof(sockaddr_bsd) || addr->sa_len != sizeof(sockaddr_bsd) || addr->sa_family != 2) return -1;
    const sockaddr_in n = to_native(addr);
    return ::bind(fd, reinterpret_cast<const ::sockaddr*>(&n), sizeof n);
}
int GetSockName(int fd, sockaddr_bsd* addr, unsigned int* len) {
    sockaddr_in n{};
    socklen_t l = sizeof n;
    if (::getsockname(fd, reinterpret_cast<::sockaddr*>(&n), &l) != 0) return -1;
    to_bsd(n, addr);
    *len = sizeof *addr;
    return 0;
}
long SendTo(int fd, const void* data, unsigned long size, int flags, const sockaddr_bsd* to, unsigned int len) {
    if (len != sizeof(sockaddr_bsd) || to->sa_len != sizeof(sockaddr_bsd) || to->sa_family != 2) return -1;
    const sockaddr_in n = to_native(to);
    return ::sendto(fd, static_cast<const char*>(data), static_cast<int>(size), flags,
                    reinterpret_cast<const ::sockaddr*>(&n), sizeof n);
}
long RecvFrom(int fd, void* data, unsigned long size, int flags, sockaddr_bsd* from, unsigned int* len) {
    sockaddr_in n{};
    socklen_t l = sizeof n;
    const long r = ::recvfrom(fd, static_cast<char*>(data), static_cast<int>(size), flags,
                              reinterpret_cast<::sockaddr*>(&n), &l);
    if (r >= 0 && from) {
        to_bsd(n, from);
        *len = sizeof *from;
    }
    return r;
}
int Poll(pollfd_nx* fds, unsigned long count, int timeout_ms) {
    if (count != 1) return -1;
#ifdef _WIN32
    WSAPOLLFD p{static_cast<SOCKET>(fds->fd), static_cast<SHORT>(fds->events & 1 ? POLLRDNORM : 0), 0};
    const int r = WSAPoll(&p, 1, timeout_ms);
    fds->revents = (p.revents & (POLLRDNORM | POLLERR | POLLHUP)) ? 1 : 0;
#else
    ::pollfd p{fds->fd, static_cast<short>(fds->events & 1 ? POLLIN : 0), 0};
    const int r = ::poll(&p, 1, timeout_ms);
    fds->revents = (p.revents & (POLLIN | POLLERR | POLLHUP)) ? 1 : 0;
#endif
    return r;
}
int Close(int fd) {
#ifdef _WIN32
    return ::closesocket(static_cast<SOCKET>(fd));
#else
    return ::close(fd);
#endif
}
}  // namespace nn::socket

namespace {
bool hex_is(const uint8_t* d, const char* want) {
    char got[65];
    for (int i = 0; i < 32; ++i) std::snprintf(got + 2 * i, 3, "%02x", d[i]);
    if (std::strcmp(got, want) != 0) {
        std::printf("selftest: got %s want %s\n", got, want);
        return false;
    }
    return true;
}

int selftest() {
    bool ok = true;
    uint8_t h[32];
    coopnet::sha256(reinterpret_cast<const uint8_t*>("abc"), 3, h);
    ok &= hex_is(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char* long_msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    coopnet::sha256(reinterpret_cast<const uint8_t*>(long_msg), std::strlen(long_msg), h);
    ok &= hex_is(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // join_proof = HMAC-SHA256(password, nonce||c||s) over a fixed 96-byte message,
    // so the check uses one: key "Jefe", 96 x 0x61; value from Python: hmac.new(b"Jefe", b"a" * 96, sha256).
    uint8_t a[32], b[32], c[32];
    std::memset(a, 0x61, 32);
    std::memset(b, 0x61, 32);
    std::memset(c, 0x61, 32);
    coopnet::join_proof("Jefe", a, b, c, h);
    ok &= hex_is(h, "10ccb8cfcb8bdd7f46c2a2ce66a166dc757555c0d1a73ae25f1ef1bf76819dfc");
    // HELLO: type 1, body length, protocol, four strings, 32-byte share.
    const coopnet::JoinConfig cfg{{127, 0, 0, 1}, 1, "pw", "nick", "build", "e0f3ccfaf50065ae", "1.1.1", 18};
    uint8_t share[32] = {};
    uint8_t out[256];
    const size_t n = coopnet::encode_hello(cfg, share, out, sizeof out);
    const size_t want = 4 + 2 + (2 + 5) + (2 + 16) + (2 + 5) + (2 + 4) + 32;
    if (n != want || out[0] != 1 || out[1] != 0 || out[2] != (want - 4) || out[4] != 18) {
        std::printf("selftest: HELLO layout wrong (%zu bytes)\n", n);
        ok = false;
    }
    std::printf("selftest: %s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}

int join(int argc, char** argv) {
    if (argc != 9) return 2;
    coopnet::JoinConfig cfg{};
    unsigned ip[4];
    if (std::sscanf(argv[2], "%u.%u.%u.%u", &ip[0], &ip[1], &ip[2], &ip[3]) != 4) return 2;
    for (int i = 0; i < 4; ++i) cfg.ip[i] = static_cast<uint8_t>(ip[i]);
    cfg.port = static_cast<uint16_t>(std::atoi(argv[3]));
    cfg.password = argv[4];
    cfg.nickname = argv[5];
    cfg.build_id = "exefs-proto (PC test of switch/net)";
    cfg.game_build_id = "e0f3ccfaf50065ae";
    cfg.game_version = "1.1.1";
    cfg.protocol_version = static_cast<uint16_t>(std::atoi(argv[6]));
    const int seconds = std::atoi(argv[7]);
    const int want_others = std::atoi(argv[8]);
    if (!coopnet::join_start(cfg)) return 1;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end) {
        coopnet::join_tick();  // once a "frame"
        const auto s = coopnet::join_status();
        if (s.state == coopnet::JoinState::refused || s.state == coopnet::JoinState::failed ||
            s.state == coopnet::JoinState::closed)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    const auto s = coopnet::join_status();
    std::printf("join: state %s, player %u, others seen %u, pings answered %u\n", coopnet::to_string(s.state),
                s.player_number, s.players_seen, s.pings_answered);
    coopnet::join_stop();
    return s.state == coopnet::JoinState::welcomed && s.players_seen >= want_others ? 0 : 1;
}
}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    if (argc >= 2 && std::strcmp(argv[1], "selftest") == 0) return selftest();
    if (argc >= 2 && std::strcmp(argv[1], "join") == 0) return join(argc, argv);
    std::printf("usage: net_host_test selftest | join <ip> <port> <password> <nickname> <protocol> <seconds> <want_others>\n");
    return 2;
}
