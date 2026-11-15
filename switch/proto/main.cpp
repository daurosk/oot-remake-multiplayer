// ExeFS prototype (v1-exefs-proto): a code module (subsdk9) inside the game's own process.
//
// It proves four things on a Switch 2 emulator (Fritchii), nothing more:
//   1. rtld loads and starts it: "coop-proto: hello" in the log.
//   2. It hooks the game: the per-frame `bl frame_step` at main+0x6e70f8 goes
//      through coop_frame_entry (start.s); a log line every 600 frames.
//   3. It calls one of the game's own functions: the world clock setter
//      main+0x34522d0 (world manager, seconds), as lichii/hook/coop_time.cpp does.
//   4. It opens a socket with the game's own nn::socket (from sdk) and sends a
//      UDP "hello" to 127.0.0.1:27520 (tools/switch_mod/listen.py prints it).
// No libc, no SDK headers: the game's modules are linked by rtld by name.

#include <stddef.h>
#include <stdint.h>

#include "svc.h"

// ---- Imports, resolved by rtld from main and sdk ----
extern "C" {
void nnMain();             // defined only in main: finds main's base
void* malloc(size_t size);  // defined only in main: checks the build
}

struct sockaddr {
    uint8_t sa_len;
    uint8_t sa_family;
    char sa_data[14];
};

namespace nn::socket {
uint32_t Initialize(void* pool, unsigned long pool_size, unsigned long alloc_pool_size,
                    int concurrency);  // _ZN2nn6socket10InitializeEPvmmi
bool IsInitialized();                  // _ZN2nn6socket13IsInitializedEv
int Socket(int domain, int type, int protocol);                      // _ZN2nn6socket6SocketEiii
int Connect(int fd, const ::sockaddr* addr, unsigned int len);       // _ZN2nn6socket7ConnectEiPK8sockaddrj
long Send(int fd, const void* data, unsigned long size, int flags);  // _ZN2nn6socket4SendEiPKvmi
int Close(int fd);                                                   // _ZN2nn6socket5CloseEi
}  // namespace nn::socket

// ---- What this build of the game looks like (build e0f3ccfaf50065ae, 1.1.1) ----
namespace game {
constexpr uint64_t kNnMain = 0x1202cd0;     // main's dynsym
constexpr uint64_t kMalloc = 0x14c6c90;     // main's dynsym; coop_game_build.h .malloc
constexpr uint64_t kFrameSite = 0x6e70f8;   // coop_svc.h kHooks[0]
constexpr uint32_t kFrameSiteInsn = 0x94d19c82;  // bl 0x3b4e300
constexpr uint64_t kFrameStep = 0x3b4e300;  // coop_game_build.h .frame_step
constexpr uint64_t kWorldMgrPtr = 0x4e7eab0;  // coop_game_build.h .world_mgr_ptr
constexpr uint64_t kTimeObject = 0x510;     // coop_time.cpp
constexpr uint64_t kSetTime = 0x34522d0;    // coop_time.cpp kSetTime
}  // namespace game

namespace {

constexpr uint16_t kPort = 27520;  // LANE1's port (27510 + 10 * 1)
constexpr uint32_t kNoon = 12 * 3600;

uint64_t g_main = 0;
uint64_t g_frames = 0;
bool g_time_done = false;
bool g_net_done = false;
alignas(0x1000) uint8_t g_socket_pool[0x400000];

// ---- A tiny log: text and numbers into one line, then the kernel's debug string ----
struct Line {
    char buf[200];
    size_t n = 0;
    Line& s(const char* t) {
        while (*t && n < sizeof(buf) - 1) buf[n++] = *t++;
        return *this;
    }
    Line& hex(uint64_t v) {
        s("0x");
        char tmp[16];
        int k = 0;
        do {
            tmp[k++] = "0123456789abcdef"[v & 0xf];
            v >>= 4;
        } while (v && k < 16);
        while (k && n < sizeof(buf) - 1) buf[n++] = tmp[--k];
        return *this;
    }
    Line& dec(uint64_t v) {
        char tmp[20];
        int k = 0;
        do {
            tmp[k++] = static_cast<char>('0' + v % 10);
            v /= 10;
        } while (v && k < 20);
        while (k && n < sizeof(buf) - 1) buf[n++] = tmp[--k];
        return *this;
    }
    ~Line() { svc::OutputDebugString(buf, n); }
};

Line Log() {
    Line l;
    l.s("coop-proto: ");
    return l;
}

bool Mapped(uint64_t addr) {
    svc::MemoryInfo mi{};
    if (svc::QueryMemory(&mi, addr) != 0) return false;
    return mi.perm != 0 && addr >= mi.addr && addr - mi.addr < mi.size;
}

bool ReadU64(uint64_t addr, uint64_t& out) {
    if (addr == 0 || (addr & 7) || !Mapped(addr)) return false;
    out = *reinterpret_cast<volatile uint64_t*>(addr);
    return true;
}

// Point the game's `bl` at `target` (one instruction, same page permissions after).
bool PatchBl(uint64_t site, uint64_t target) {
    const int64_t delta = static_cast<int64_t>(target - site);
    if ((delta & 3) || delta >= (int64_t{1} << 27) || delta < -(int64_t{1} << 27)) {
        Log().s("frame hook: ").hex(target).s(" out of bl range from ").hex(site);
        return false;
    }
    const uint32_t insn = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
    const uint64_t page = site & ~uint64_t{0xfff};
    uint32_t r = svc::SetProcessMemoryPermission(svc::kCurrentProcess, page, 0x1000, svc::kPermRW);
    if (r != 0) {
        Log().s("frame hook: make main writable failed, result ").hex(r);
        return false;
    }
    *reinterpret_cast<volatile uint32_t*>(site) = insn;
    __asm__ volatile("dc cvau, %0\n dsb ish\n ic ivau, %0\n dsb ish\n isb" : : "r"(site) : "memory");
    r = svc::SetProcessMemoryPermission(svc::kCurrentProcess, page, 0x1000, svc::kPermRX);
    if (r != 0) Log().s("frame hook: restore RX failed, result ").hex(r);
    return true;
}

void TrySetTime() {
    uint64_t world = 0, time = 0;
    if (!ReadU64(g_main + game::kWorldMgrPtr, world) || world == 0) return;
    if (!ReadU64(world + game::kTimeObject, time) || time == 0) return;
    using SetTime = void (*)(uint64_t world, uint64_t seconds);
    Log().s("game call: set_time(world ").hex(world).s(", ").dec(kNoon).s(") at frame ").dec(g_frames);
    reinterpret_cast<SetTime>(g_main + game::kSetTime)(world, kNoon);
    Log().s("game call: set_time returned");
    g_time_done = true;
}

void SayHello() {
    g_net_done = true;
    if (!nn::socket::IsInitialized()) {
        const uint32_t r = nn::socket::Initialize(g_socket_pool, sizeof(g_socket_pool), 0x20000, 4);
        Log().s("net: the game had no sockets yet; Initialize result ").hex(r);
        if (r != 0) return;
    } else {
        Log().s("net: using the game's socket library (already initialized)");
    }
    const int fd = nn::socket::Socket(2 /*AF_INET*/, 2 /*SOCK_DGRAM*/, 0);
    if (fd < 0) {
        Log().s("net: socket failed");
        return;
    }
    sockaddr addr{};
    addr.sa_len = sizeof(addr);
    addr.sa_family = 2;
    addr.sa_data[0] = static_cast<char>(kPort >> 8);  // port, network order
    addr.sa_data[1] = static_cast<char>(kPort & 0xff);
    addr.sa_data[2] = 127;  // 127.0.0.1
    addr.sa_data[5] = 1;
    const int c = nn::socket::Connect(fd, &addr, sizeof(addr));
    static const char kHello[] = "coop-proto hello from subsdk9";
    long sent = -1;
    for (int i = 0; i < 3 && c == 0; ++i) sent = nn::socket::Send(fd, kHello, sizeof(kHello) - 1, 0);
    Log().s("net: connect ").dec(c == 0 ? 0 : 1).s(", sent ").dec(sent < 0 ? 0 : static_cast<uint64_t>(sent))
        .s(" bytes x3 to 127.0.0.1:").dec(kPort);
    nn::socket::Close(fd);
}

}  // namespace

// Where coop_frame_entry continues: the game's own frame step.
extern "C" uint64_t g_frame_step = 0;

// Every frame, before the game's frame step (start.s keeps all argument registers).
extern "C" void coop_proto_frame() {
    ++g_frames;
    if (g_frames == 1 || g_frames % 600 == 0) Log().s("frame ").dec(g_frames);
    if (!g_time_done && g_frames >= 300 && g_frames % 60 == 0) TrySetTime();
    if (!g_net_done && g_frames >= 900) SayHello();
}

// rtld calls this once, after nninitStartup and before nnMain (actor-map.md, Module _init).
extern "C" void _init() {
    Log().s("hello from subsdk9 (ExeFS prototype)");
    svc::MemoryInfo mi{};
    const uint64_t nn_main = reinterpret_cast<uint64_t>(&nnMain);
    if (svc::QueryMemory(&mi, nn_main) != 0) {
        Log().s("cannot query main");
        return;
    }
    g_main = mi.addr;
    const uint64_t malloc_off = reinterpret_cast<uint64_t>(&malloc) - g_main;
    Log().s("main at ").hex(g_main).s(", nnMain +").hex(nn_main - g_main).s(", malloc +").hex(malloc_off);
    if (nn_main - g_main != game::kNnMain || malloc_off != game::kMalloc) {
        Log().s("not build e0f3ccfaf50065ae (1.1.1): no hooks");
        return;
    }
    const uint64_t site = g_main + game::kFrameSite;
    const uint32_t insn = *reinterpret_cast<volatile uint32_t*>(site);
    if (insn != game::kFrameSiteInsn) {
        Log().s("frame site holds ").hex(insn).s(", not the game's bl (another mod?): no hooks");
        return;
    }
    g_frame_step = g_main + game::kFrameStep;
    extern void coop_frame_entry() __asm__("coop_frame_entry");
    if (PatchBl(site, reinterpret_cast<uint64_t>(&coop_frame_entry))) {
        Log().s("frame hook in at main+").hex(game::kFrameSite);
    }
}
