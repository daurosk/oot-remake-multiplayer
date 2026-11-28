// SPDX-License-Identifier: GPL-3.0-or-later
// ENet's platform layer on the game's own nn::socket (sdk), for the ExeFS module.
// Replaces ENet's unix.c and callbacks.c; ENet's host/peer/packet/list/protocol.c
// are compiled unchanged (fetched at the pin in tools/switch_mod/deps.py).
//
// Only what ENet's host code calls is here (enet_host_create with no bind
// address, enet_host_connect, enet_host_service, enet_peer_send). No errno is
// read: a receive is only tried after Poll said the socket is readable, so a
// blocking socket never blocks and no EWOULDBLOCK value is needed.
//
// Constants, with their source (Fritchii, src/core/hle/service/sockets/sockets.h):
// Domain::INET = 2 (:60), Type::DGRAM = 2 (:66), PollEvents::In = 1 (:239),
// PollFD {s32 fd; u16 events; u16 revents} (:252). The address is the BSD
// sockaddr_in the guest library takes: u8 len, u8 family, u16 port (network
// order), u32 address (network order), 8 zero bytes.
#include "net_plat.h"

extern "C" {
#include <enet/enet.h>
}

struct sockaddr {
    uint8_t sa_len;
    uint8_t sa_family;
    char sa_data[14];
};
struct pollfd {
    int fd;
    uint16_t events;
    uint16_t revents;
};

namespace nn::socket {
int Socket(int domain, int type, int protocol);                                          // _ZN2nn6socket6SocketEiii
int Bind(int fd, const ::sockaddr* addr, unsigned int len);                              // _ZN2nn6socket4BindEiPK8sockaddrj
int GetSockName(int fd, ::sockaddr* addr, unsigned int* len);                            // _ZN2nn6socket11GetSockNameEiP8sockaddrPj
long SendTo(int fd, const void* data, unsigned long size, int flags, const ::sockaddr* to,
            unsigned int len);                                                           // _ZN2nn6socket6SendToEiPKvmiPK8sockaddrj
long RecvFrom(int fd, void* data, unsigned long size, int flags, ::sockaddr* from,
              unsigned int* len);                                                        // _ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj
int Poll(::pollfd* fds, unsigned long count, int timeout_ms);                            // _ZN2nn6socket4PollEP6pollfdmi
int Close(int fd);                                                                       // _ZN2nn6socket5CloseEi
}  // namespace nn::socket

namespace {
constexpr int kInet = 2;
constexpr int kDgram = 2;
constexpr uint16_t kPollIn = 1;

void to_sockaddr(const ENetAddress& a, sockaddr& out) {
    memset(&out, 0, sizeof out);
    out.sa_len = sizeof out;
    out.sa_family = kInet;
    const uint16_t port = ENET_HOST_TO_NET_16(a.port);
    memcpy(out.sa_data, &port, 2);
    memcpy(out.sa_data + 2, &a.host, 4);  // ENet keeps the address in network order
}

void from_sockaddr(const sockaddr& in, ENetAddress& out) {
    uint16_t port = 0;
    memcpy(&port, in.sa_data, 2);
    memcpy(&out.host, in.sa_data + 2, 4);
    out.port = ENET_NET_TO_HOST_16(port);
}

// One datagram out: ENet hands a few buffers (header + commands); nn::socket
// has no gather send in the int form, so they go through one buffer.
alignas(16) uint8_t g_send[ENET_PROTOCOL_MAXIMUM_MTU];
}  // namespace

extern "C" {

void* enet_malloc(size_t size) { return malloc(size); }
void enet_free(void* p) { free(p); }

int enet_initialize(void) { return 0; }
void enet_deinitialize(void) {}

enet_uint32 enet_host_random_seed(void) {
    enet_uint32 seed = 0;
    coop_plat_random(&seed, sizeof seed);
    return seed;
}

static enet_uint32 g_time_base = 0;
enet_uint32 enet_time_get(void) { return static_cast<enet_uint32>(coop_plat_ms()) - g_time_base; }
void enet_time_set(enet_uint32 t) { g_time_base = static_cast<enet_uint32>(coop_plat_ms()) - t; }

ENetSocket enet_socket_create(ENetSocketType type) {
    if (type != ENET_SOCKET_TYPE_DATAGRAM) return ENET_SOCKET_NULL;
    const int fd = nn::socket::Socket(kInet, kDgram, 0);
    return fd < 0 ? ENET_SOCKET_NULL : fd;
}

int enet_socket_bind(ENetSocket s, const ENetAddress* address) {
    ENetAddress any{};
    if (address) any = *address;
    sockaddr sa;
    to_sockaddr(any, sa);
    return nn::socket::Bind(s, &sa, sizeof sa) < 0 ? -1 : 0;
}

int enet_socket_get_address(ENetSocket s, ENetAddress* address) {
    sockaddr sa{};
    unsigned int len = sizeof sa;
    if (nn::socket::GetSockName(s, &sa, &len) < 0) return -1;
    from_sockaddr(sa, *address);
    return 0;
}

// Non-blocking, broadcast and buffer sizes: not set (see the top of the file:
// a receive only follows a readable Poll). ENet ignores the result.
int enet_socket_set_option(ENetSocket, ENetSocketOption, int) { return 0; }
int enet_socket_get_option(ENetSocket, ENetSocketOption, int* value) {
    *value = 0;
    return 0;
}

void enet_socket_destroy(ENetSocket s) {
    if (s != ENET_SOCKET_NULL) nn::socket::Close(s);
}

int enet_socket_send(ENetSocket s, const ENetAddress* address, const ENetBuffer* buffers,
                     size_t count) {
    size_t n = 0;
    for (size_t i = 0; i < count; ++i) {
        if (n + buffers[i].dataLength > sizeof g_send) return -1;
        memcpy(g_send + n, buffers[i].data, buffers[i].dataLength);
        n += buffers[i].dataLength;
    }
    sockaddr sa;
    to_sockaddr(*address, sa);
    const long sent = nn::socket::SendTo(s, g_send, n, 0, &sa, sizeof sa);
    return sent < 0 ? -1 : static_cast<int>(sent);
}

int enet_socket_receive(ENetSocket s, ENetAddress* address, ENetBuffer* buffers, size_t count) {
    if (count != 1) return -1;  // ENet's host code always passes one buffer
    pollfd p{s, kPollIn, 0};
    const int ready = nn::socket::Poll(&p, 1, 0);
    if (ready < 0) return -1;
    if (ready == 0 || !(p.revents & kPollIn)) return 0;
    sockaddr sa{};
    unsigned int len = sizeof sa;
    const long got = nn::socket::RecvFrom(s, buffers[0].data, buffers[0].dataLength, 0, &sa, &len);
    if (got < 0) return -1;
    if (address) from_sockaddr(sa, *address);
    return static_cast<int>(got);
}

int enet_socket_wait(ENetSocket s, enet_uint32* condition, enet_uint32 timeout) {
    pollfd p{s, 0, 0};
    if (*condition & ENET_SOCKET_WAIT_RECEIVE) p.events |= kPollIn;
    const int ready = nn::socket::Poll(&p, 1, static_cast<int>(timeout));
    if (ready < 0) return -1;
    *condition = ENET_SOCKET_WAIT_NONE;
    if (ready > 0 && (p.revents & kPollIn)) *condition |= ENET_SOCKET_WAIT_RECEIVE;
    return 0;
}

}  // extern "C"
