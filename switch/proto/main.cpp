// ExeFS prototype (v1-exefs-proto): a code module (subsdk1) inside the game's own process.
//
// It proves four things on a Switch 2 emulator (Fritchii), nothing more:
//   1. rtld loads and starts it: "coop-proto: hello" in the log.
//   2. It hooks the game: the per-frame `bl frame_step` at main+0x6e70f8 goes
//      through coop_frame_entry (start.s); a log line every 600 frames.
//   3. It calls one of the game's own functions: the world clock setter
//      main+0x34522d0 (world manager, seconds), as lichii/hook/coop_time.cpp does.
//   4. It reads one game value: the current scene name (Banc module +0x40).
//   5. It joins a coop_server with the game's own nn::socket (from sdk), the way a
//      Longshot game does: ENet, HELLO, JOIN_PROOF, sealed WELCOME (switch/net,
//      coop_join.h), so an emulator player and a Longshot player share one session.
// No libc, no SDK headers: the game's modules are linked by rtld by name.
//
// Built so a recomp can lift the same binary as subsdk1 (docs/exefs.md, "Lifting"):
// unwind tables kept, entry points exported, and no indirect call except through
// the slots in the hook table (hooks.h), which a lifter turns into direct calls.

#include <stddef.h>
#include <stdint.h>

#include "../net/coop_join.h"
#include "../net/net_plat.h"
#include "coop_join_config.h"  // written by tools/switch_mod/build.py (--join, --password, --name)
#include "hooks.h"
#include "svc.h"

// ---- Imports, resolved by rtld from main and sdk ----
extern "C" {
void nnMain();             // defined only in main: finds main's base
void* malloc(size_t size);  // defined only in main: checks the build
}

namespace nn::socket {
uint32_t Initialize(void* pool, unsigned long pool_size, unsigned long alloc_pool_size,
                    int concurrency);  // _ZN2nn6socket10InitializeEPvmmi
bool IsInitialized();                  // _ZN2nn6socket13IsInitializedEv
}  // namespace nn::socket
namespace nn::os {
void GenerateRandomBytes(void* out, unsigned long size);  // _ZN2nn2os19GenerateRandomBytesEPvm (sdk)
}  // namespace nn::os

// ---- What this build of the game looks like (build e0f3ccfaf50065ae, 1.1.1) ----
namespace game {
constexpr uint64_t kNnMain = 0x1202cd0;     // main's dynsym
constexpr uint64_t kMalloc = 0x14c6c90;     // main's dynsym; coop_game_build.h .malloc
constexpr uint64_t kFrameSite = 0x6e70f8;   // coop_svc.h kHooks[0]
constexpr uint32_t kFrameSiteInsn = 0x94d19c82;  // bl 0x3b4e300 (frame_step)
constexpr uint64_t kWorldMgrPtr = 0x4e7eab0;  // coop_game_build.h .world_mgr_ptr
constexpr uint64_t kTimeObject = 0x510;     // coop_time.cpp
constexpr uint64_t kSetTime = 0x34522d0;    // coop_time.cpp kSetTime
constexpr uint64_t kBancModulePtr = 0x4e919d0;  // coop_game_build.h .banc_module_ptr
constexpr uint64_t kBancName = 0x40;        // player-state-map.md "Current scene": B+0x40 string pointer
}  // namespace game

// ---- Slots: the only way this module calls into the game (see hooks.h) ----
extern "C" {
COOP_EXPORT uint64_t coop_slot_frame_step = 0;  // the game's frame step; start.s continues there
COOP_EXPORT uint64_t coop_slot_set_time = 0;    // void set_time(world, seconds)
COOP_EXPORT void coop_frame_entry();            // start.s
COOP_EXPORT void coop_call_set_time(uint64_t world, uint64_t seconds);  // start.s, via coop_slot_set_time
}

extern "C" COOP_EXPORT constinit __attribute__((section(".coop_hooks"), used)) hooks::Table coop_hooks = {
    {'C', 'O', 'O', 'P', 'H', 'O', 'O', 'K'},
    1,
    sizeof(hooks::Entry),
    2,
    0,  // applied_by_host: a lifting recomp sets it to 1 in its copy
    0,
    {
        {hooks::kCallRedirect, 0, game::kFrameSiteInsn, game::kFrameSite, &coop_frame_entry,
         &coop_slot_frame_step},
        {hooks::kGameFn, 0, 0, game::kSetTime, nullptr, &coop_slot_set_time},
    },
};

namespace {

constexpr uint32_t kNoon = 12 * 3600;

uint64_t g_main = 0;
uint64_t g_frames = 0;
bool g_host = false;  // the hooks were applied by a lifting recomp: no SVCs
bool g_time_done = false;
bool g_net_done = false;
bool g_joining = false;
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
    if (g_host) return true;  // no QueryMemory on a recomp; the real core uses null-read guards
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
        Log().s("hook: ").hex(target).s(" out of bl range from ").hex(site);
        return false;
    }
    const uint32_t insn = 0x94000000u | (static_cast<uint32_t>(delta >> 2) & 0x03ffffffu);
    const uint64_t page = site & ~uint64_t{0xfff};
    uint32_t r = svc::SetProcessMemoryPermission(svc::kCurrentProcess, page, 0x1000, svc::kPermRW);
    if (r != 0) {
        Log().s("hook: make main writable failed, result ").hex(r);
        return false;
    }
    *reinterpret_cast<volatile uint32_t*>(site) = insn;
    __asm__ volatile("dc cvau, %0\n dsb ish\n ic ivau, %0\n dsb ish\n isb" : : "r"(site) : "memory");
    r = svc::SetProcessMemoryPermission(svc::kCurrentProcess, page, 0x1000, svc::kPermRX);
    if (r != 0) Log().s("hook: restore RX failed, result ").hex(r);
    return true;
}

bool IsBl(uint32_t insn) { return (insn & 0xfc000000u) == 0x94000000u; }

uint64_t BlTarget(uint64_t site, uint32_t insn) {
    const int64_t imm26 = static_cast<int32_t>(insn << 6) >> 6;  // sign-extend
    return site + static_cast<uint64_t>(imm26 * 4);
}

// What a lifting recomp does at build time, done here at run time (emulators, console).
void ApplyHooks() {
    const uint32_t count = coop_hooks.count;
    for (uint32_t i = 0; i < count && i < sizeof(coop_hooks.entries) / sizeof(coop_hooks.entries[0]); ++i) {
        const hooks::Entry& e = coop_hooks.entries[i];
        const uint64_t at = g_main + e.main_offset;
        const uint32_t insn = *reinterpret_cast<volatile uint32_t*>(at);
        if (e.expected_insn != 0 && insn != e.expected_insn) {
            Log().s("hook ").dec(i).s(": main+").hex(e.main_offset).s(" holds ").hex(insn)
                .s(", not ").hex(e.expected_insn).s(" (another mod?): skipped");
            continue;
        }
        switch (e.kind) {
            case hooks::kCallRedirect:
                if (!IsBl(insn)) {
                    Log().s("hook ").dec(i).s(": main+").hex(e.main_offset).s(" is not a bl: skipped");
                    break;
                }
                *e.slot = BlTarget(at, insn);
                if (PatchBl(at, reinterpret_cast<uint64_t>(e.replacement))) {
                    Log().s("hook ").dec(i).s(": frame hook in at main+").hex(e.main_offset)
                        .s(", continues at main+").hex(*e.slot - g_main);
                }
                break;
            case hooks::kGameFn:
                *e.slot = at;
                Log().s("hook ").dec(i).s(": game function slot = main+").hex(e.main_offset);
                break;
            default:
                Log().s("hook ").dec(i).s(": kind ").dec(e.kind).s(" not applied by the module");
                break;
        }
    }
}

void TrySetTime() {
    uint64_t world = 0, time = 0;
    if (coop_slot_set_time == 0) return;
    if (!ReadU64(g_main + game::kWorldMgrPtr, world) || world == 0) return;
    if (!ReadU64(world + game::kTimeObject, time) || time == 0) return;
    Log().s("game call: set_time(world ").hex(world).s(", ").dec(kNoon).s(") at frame ").dec(g_frames);
    coop_call_set_time(world, kNoon);
    Log().s("game call: set_time returned");
    g_time_done = true;
}

// One game value: the scene name, a path string at *(Banc module) + 0x40; logged when it changes.
char g_scene[64] = {};
void ReadScene() {
    uint64_t banc = 0, str = 0;
    if (!ReadU64(g_main + game::kBancModulePtr, banc) || banc == 0) return;
    if (!ReadU64(banc + game::kBancName, str) || str == 0 || !Mapped(str)) return;
    char now[64] = {};
    const volatile char* p = reinterpret_cast<const volatile char*>(str);
    for (size_t i = 0; i + 1 < sizeof now; ++i) {
        const char c = p[i];
        if (c == 0) break;
        if (c < 0x20 || c > 0x7e) return;  // not text: not the name
        now[i] = c;
    }
    if (now[0] == 0 || memcmp(now, g_scene, sizeof now) == 0) return;
    memcpy(g_scene, now, sizeof now);
    Log().s("game value: scene ").s(g_scene).s(" at frame ").dec(g_frames);
}

void StartJoin() {
    g_net_done = true;
    if (!nn::socket::IsInitialized()) {
        const uint32_t r = nn::socket::Initialize(g_socket_pool, sizeof(g_socket_pool), 0x20000, 4);
        Log().s("net: the game had no sockets yet; Initialize result ").hex(r);
        if (r != 0) return;
    } else {
        Log().s("net: using the game's socket library (already initialized)");
    }
    coopnet::JoinConfig c{};
    const uint8_t ip[4] = COOP_JOIN_IP;
    memcpy(c.ip, ip, 4);
    c.port = COOP_JOIN_PORT;
    c.password = COOP_JOIN_PASSWORD;
    c.nickname = COOP_JOIN_NAME;
    c.build_id = "exefs-proto subsdk1 (Fritchii)";
    c.game_build_id = "e0f3ccfaf50065ae";
    c.game_version = "1.1.1";
    c.protocol_version = COOP_PROTOCOL_VERSION;
    g_joining = coopnet::join_start(c);
}

}  // namespace

// ---- What switch/net asks of the platform (net_plat.h) ----
extern "C" void coop_plat_log(const char* text, size_t len) { svc::OutputDebugString(text, len); }
extern "C" uint64_t coop_plat_ms() {
    uint64_t ticks, freq;  // the CPU's generic timer (readable at EL0 on the console and in the JIT)
    asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq ? ticks / (freq / 1000) : 0;
}
extern "C" void coop_plat_random(void* out, size_t len) { nn::os::GenerateRandomBytes(out, len); }

// Every frame, before the game's frame step (start.s keeps all argument registers).
extern "C" COOP_EXPORT void coop_proto_frame() {
    ++g_frames;
    if (g_frames == 1 || g_frames % 600 == 0) Log().s("frame ").dec(g_frames);
    if (!g_time_done && g_frames >= 300 && g_frames % 60 == 0) TrySetTime();
    if (g_frames % 60 == 0) ReadScene();
    if (!g_net_done && g_frames >= 900) StartJoin();
    if (g_joining) {
        coopnet::join_tick();
        if (g_frames % 600 == 0) {
            const coopnet::JoinStatus st = coopnet::join_status();
            Log().s("net: join ").s(coopnet::to_string(st.state)).s(", player ").dec(st.player_number)
                .s(", others seen ").dec(st.players_seen).s(", pings answered ").dec(st.pings_answered);
        }
    }
}

// rtld calls this once, after nninitStartup and before nnMain (actor-map.md, Module _init).
extern "C" COOP_EXPORT void _init() {
    Log().s("hello from subsdk1 (ExeFS prototype)");
    // main's base without a kernel call: rtld linked nnMain and malloc from main.
    const uint64_t nn_main = reinterpret_cast<uint64_t>(&nnMain);
    g_main = nn_main - game::kNnMain;
    const uint64_t malloc_off = reinterpret_cast<uint64_t>(&malloc) - g_main;
    Log().s("main at ").hex(g_main).s(", malloc +").hex(malloc_off);
    if (malloc_off != game::kMalloc) {
        Log().s("not build e0f3ccfaf50065ae (1.1.1): no hooks");
        return;
    }
    if (*static_cast<volatile uint32_t*>(&coop_hooks.applied_by_host) != 0) {
        g_host = true;
        Log().s("hooks applied by the host (lifted module): nothing to patch");
        return;
    }
    ApplyHooks();
}
