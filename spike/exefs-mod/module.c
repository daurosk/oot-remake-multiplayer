// SPDX-License-Identifier: GPL-3.0-or-later
// exefs spike (subsdk9): a code mod that runs inside the game on an emulator that loads exefs mods (Fritchii).
//
// coop_init (DT_INIT, called by the game's rtld before the game starts) finds main, checks the build's per-frame call
// (main+0x6e70f8: `bl frame_step`, moduleCalc_; lichii/hook/coop_svc.h) and turns it into a call through a small cave
// to coop_frame_tramp (crt0.s). coop_frame runs once a game frame: it opens a UDP socket with the game's own
// nn::socket (sdk), sends one ENet CONNECT to coop_server on 127.0.0.1, and logs the reply (VERIFY_CONNECT) with
// svcOutputDebugString, which the emulator writes to its log (Debug_Emulated).
//
// Built freestanding (no libc): see build.ps1. Nothing here is Windows code.

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long usize;

#define HIDDEN __attribute__((visibility("hidden")))

// ---- the build this spike knows (Ocarina of Time 1.1.1, main build id e0f3ccfaf50065ae)
#define SITE_FRAME 0x6e70f8ull      // bl frame_step, every frame
#define SITE_WORD 0x94d19c82u       // its instruction
#define FRAME_STEP 0x3b4e300ull     // the frame function it calls
#define CAVE 0x3c46fe0ull           // main's code padding (rodata at 0x3c47000), after Fritchii's built-in routines
#define SERVER_PORT 27510           // coop/protocol kDefaultPort

// ---- SVCs (crt0.s)
typedef struct {
    u64 addr, size;
    u32 type, attr, perm, ipc_refs, device_refs, pad;
} MemoryInfo;
HIDDEN u32 svc_query_memory(MemoryInfo *out, u64 addr);
HIDDEN u32 svc_set_process_memory_permission(u32 handle, u64 addr, u64 size, u32 perm);
HIDDEN u32 svc_output_debug_string(const char *s, u64 len);
HIDDEN void flush_code(void *word);
HIDDEN void coop_frame_tramp(void);

#define CUR_PROCESS 0xffff8001u
#define PERM_RW 3u
#define PERM_RX 5u

// ---- the game's nn::socket (sdk exports; rtld binds them)
typedef struct {
    u8 len, family;
    u16 port;   // big-endian
    u32 addr;   // big-endian
    u8 zero[8];
} SockAddrIn;
int nn_socket_initialize(void *pool, usize pool_size, usize alloc_size, int concurrency)
    __asm__("_ZN2nn6socket10InitializeEPvmmi");
int nn_socket_socket(int family, int type, int proto) __asm__("_ZN2nn6socket6SocketEiii");
long nn_socket_sendto(int s, const void *buf, usize len, int flags, const SockAddrIn *to, u32 to_len)
    __asm__("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj");
long nn_socket_recvfrom(int s, void *buf, usize len, int flags, SockAddrIn *from, u32 *from_len)
    __asm__("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj");

#define AF_INET 2
#define SOCK_DGRAM 2
#define MSG_DONTWAIT 0x80

// ---- a small log
static void log_str(const char *s) {
    u64 n = 0;
    while (s[n]) n++;
    svc_output_debug_string(s, n);
}

typedef struct {
    char b[192];
    u32 n;
} Line;
static void put(Line *l, const char *s) {
    while (*s && l->n < sizeof l->b - 1) l->b[l->n++] = *s++;
}
static void put_hex(Line *l, u64 v, int digits) {
    for (int i = digits - 1; i >= 0 && l->n < sizeof l->b - 1; i--) l->b[l->n++] = "0123456789abcdef"[(v >> (i * 4)) & 15];
}
static void put_int(Line *l, long v) {
    char t[24];
    int i = 0;
    u64 u = v < 0 ? (u64)-v : (u64)v;
    do t[i++] = (char)('0' + u % 10); while ((u /= 10) && i < 23);
    if (v < 0 && l->n < sizeof l->b - 1) l->b[l->n++] = '-';
    while (i && l->n < sizeof l->b - 1) l->b[l->n++] = t[--i];
}
static void emit(Line *l) {
    l->b[l->n] = 0;
    log_str(l->b);
}

// ---- state
HIDDEN u64 g_frame_fn;   // main+FRAME_STEP (crt0.s tail-branches to it)
static u64 g_main;
static u32 g_frames;
static int g_sock = -1;
static int g_done;
static u32 g_sent;
static __attribute__((aligned(0x1000))) u8 g_socket_pool[0x100000];

// The base of the code region whose SITE_FRAME word is the frame call: main, whatever the loader's layout.
static u64 find_main(void) {
    u64 addr = 0;
    for (int i = 0; i < 8192; i++) {
        MemoryInfo mi;
        if (svc_query_memory(&mi, addr) != 0) break;
        if (mi.perm == PERM_RX && mi.size > SITE_FRAME + 4 &&
            *(volatile u32 *)(mi.addr + SITE_FRAME) == SITE_WORD)
            return mi.addr;
        if (mi.addr + mi.size <= addr) break;   // the end of the address space
        addr = mi.addr + mi.size;
    }
    return 0;
}

static int write_code(u64 at, const u32 *words, int count) {
    u64 page = at & ~0xfffull, end = (at + 4ull * count + 0xfff) & ~0xfffull;
    u32 r = svc_set_process_memory_permission(CUR_PROCESS, page, end - page, PERM_RW);
    if (r != 0) return (int)r;
    for (int i = 0; i < count; i++) {
        ((volatile u32 *)at)[i] = words[i];
        flush_code((void *)(at + 4ull * i));
    }
    return (int)svc_set_process_memory_permission(CUR_PROCESS, page, end - page, PERM_RX);
}

static u32 bl_to(u64 from, u64 to) { return 0x94000000u | (u32)(((to - from) >> 2) & 0x3ffffff); }

HIDDEN void coop_init(void) {
    Line l = {0};
    g_main = find_main();
    if (!g_main) {
        log_str("[coop-exefs] main not found (another build?): no hook");
        return;
    }
    g_frame_fn = g_main + FRAME_STEP;
    for (int i = 0; i < 4; i++) {
        if (((volatile u32 *)(g_main + CAVE))[i] != 0) {
            log_str("[coop-exefs] the cave is not empty (another code mod?): no hook");
            return;
        }
    }
    // cave: ldr x16, #8 ; br x16 ; .quad coop_frame_tramp
    u64 tramp = (u64)&coop_frame_tramp;
    u32 cave[4] = {0x58000050u, 0xd61f0200u, (u32)tramp, (u32)(tramp >> 32)};
    int r = write_code(g_main + CAVE, cave, 4);
    u32 hook = bl_to(g_main + SITE_FRAME, g_main + CAVE);
    if (r == 0) r = write_code(g_main + SITE_FRAME, &hook, 1);
    put(&l, "[coop-exefs] init: main=0x");
    put_hex(&l, g_main, 10);
    put(&l, " subsdk9=0x");
    put_hex(&l, tramp, 10);
    put(&l, r == 0 ? " frame hook on" : " hook FAILED svc result=");
    if (r != 0) put_hex(&l, (u32)r, 8);
    emit(&l);
}

// ENet CONNECT (protocol.h: header with sent time, then the command, all big-endian): what enet_host_connect sends.
static u32 connect_packet(u8 *p) {
    static const u8 tmpl[52] = {
        0x8f, 0xff, 0x00, 0x00,             // peer id 0xfff (none yet) | sent-time flag; sent time
        0x82, 0xff, 0x00, 0x01,             // CONNECT | ACKNOWLEDGE, channel 0xff, reliable sequence 1
        0x00, 0x00, 0xff, 0xff,             // outgoing peer id 0, session ids 0xff 0xff
        0x00, 0x00, 0x05, 0x78,             // mtu 1400
        0x00, 0x00, 0x80, 0x00,             // window 32768
        0x00, 0x00, 0x00, 0x01,             // channels 1
        0x00, 0x00, 0x00, 0x00,             // incoming bandwidth
        0x00, 0x00, 0x00, 0x00,             // outgoing bandwidth
        0x00, 0x00, 0x13, 0x88,             // throttle interval 5000
        0x00, 0x00, 0x00, 0x02,             // throttle acceleration
        0x00, 0x00, 0x00, 0x02,             // throttle deceleration
        0x5a, 0x71, 0xc0, 0x09,             // connect id
        0x00, 0x00, 0x00, 0x00,             // data
    };
    for (int i = 0; i < 52; i++) p[i] = tmpl[i];
    return 52;
}

static void send_hello(void) {
    u8 pkt[52];
    u32 n = connect_packet(pkt);
    SockAddrIn to = {16, AF_INET, (u16)((SERVER_PORT >> 8) | ((SERVER_PORT & 0xff) << 8)), 0x0100007fu, {0}};
    long r = nn_socket_sendto(g_sock, pkt, n, 0, &to, sizeof to);
    Line l = {0};
    put(&l, "[coop-exefs] hello sent to 127.0.0.1:27510 (ENet CONNECT, 52 bytes): ");
    put_int(&l, r);
    emit(&l);
    g_sent++;
}

HIDDEN void coop_frame(void) {
    g_frames++;
    if (g_done) return;
    if (g_frames == 1) log_str("[coop-exefs] first frame");
    if (g_sock < 0) {
        if (g_frames < 60) return;          // (a second in: the game's own start-up first)
        g_sock = nn_socket_socket(AF_INET, SOCK_DGRAM, 0);
        if (g_sock < 0) {                    // the game had not initialised sockets: do it once
            int ir = nn_socket_initialize(g_socket_pool, sizeof g_socket_pool, 0x20000, 4);
            g_sock = nn_socket_socket(AF_INET, SOCK_DGRAM, 0);
            Line l = {0};
            put(&l, "[coop-exefs] nn::socket::Initialize result=0x");
            put_hex(&l, (u32)ir, 8);
            put(&l, " socket=");
            put_int(&l, g_sock);
            emit(&l);
            if (g_sock < 0) {
                g_done = 1;
                return;
            }
        }
        send_hello();
        return;
    }
    u8 buf[256];
    SockAddrIn from;
    u32 from_len = sizeof from;
    long n = nn_socket_recvfrom(g_sock, buf, sizeof buf, MSG_DONTWAIT, &from, &from_len);
    if (n > 0) {
        Line l = {0};
        put(&l, "[coop-exefs] reply ");
        put_int(&l, n);
        put(&l, " bytes:");
        for (long i = 0; i < n && i < 32; i++) {
            put(&l, " ");
            put_hex(&l, buf[i], 2);
        }
        // the commands after the 4-byte header: ACKNOWLEDGE (1) of our CONNECT, then VERIFY_CONNECT (3)
        int verify = 0;
        for (long i = 4; i + 1 < n; i++)
            if ((buf[i] & 0x0f) == 3 && buf[i + 1] == 0xff) verify = 1;
        put(&l, verify ? " -> VERIFY_CONNECT: server answered" : " -> no VERIFY_CONNECT");
        emit(&l);
        g_done = 1;
        return;
    }
    if (g_frames % 120 == 0) {
        if (g_sent >= 10) {
            log_str("[coop-exefs] no reply after 10 hellos (is coop_server on 127.0.0.1:27510?)");
            g_done = 1;
            return;
        }
        send_hello();
    }
}
