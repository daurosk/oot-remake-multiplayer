// SPDX-License-Identifier: GPL-3.0-or-later
/* Squadala drop-in proof (v1-longshot-dropin, P0): co-op's first hook on a STOCK longshot.exe, no Longshot rebuild.

   Loaded as winmm.dll beside the game's bin\longshot.exe (winmm_stubs.S forwards every winmm export to System32's own winmm.dll).
   At attach it reads longshot.exe's own COFF symbol table from the file on disk (the Longshot build links the lifted
   game with its symbols: f_<guest address> for every lifted function, plus the host's own functions), finds the lifted
   frame function f_83b6e300 (main+0x3b4e300, lichii/hook/coop_game_build.h frame_step; called twice by moduleCalc_,
   hookshot/patches/0001 FRAME_SITES) and the host's hos_guest_base, and puts a 5-byte jump at the frame function's
   entry. Every lifted function is `void f(Ctx *c)` (third_party/a64-kit/rt/a64.h A64Fn), so the hook is a plain C
   function of the same shape that runs before the game's own.

   The hook logs (squadala-dropin.log beside the exe): the first call, then every 600th, with one game value read
   through the guest window: the frame function's first instruction word, which must be 0xd503237f (pacibsp; the
   game's own word in its image), and the frame function's x0. A wrong word turns the hook into a pass-through.

   SQUADALA_DROPIN=0 in the environment: the proxy only forwards winmm, no hook. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

typedef struct Ctx { uint64_t x[32]; } Ctx;   /* (the head of a64.h's GuestContext: x[31] is SP) */
typedef void (*A64Fn)(Ctx *c);

#define FRAME_GUEST 0x83b6e300u      /* main+0x3b4e300 (MAIN 0x80020000, hookshot/patches/0001 coop_hooks.py) */
#define FRAME_WORD0 0xd503237fu      /* pacibsp: the game's own first word there */
#define LOG_EVERY 600

static FILE *g_log;
static uint8_t *(*g_guest_base)(void);
static A64Fn g_frame_orig;
static volatile LONG g_frames;
static int g_frame_ok = -1;          /* -1 not checked, 0 the word differs (pass-through), 1 checked */

static void dlog(const char *fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetSystemTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [pid %lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

/* ---- longshot.exe's COFF symbols, read from the file ------------------------------------------------------- */
typedef struct { const char *name; uint32_t rva; int found; } Want;

static int read_at(HANDLE f, uint64_t off, void *buf, DWORD n) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    DWORD got = 0;
    return SetFilePointerEx(f, li, NULL, FILE_BEGIN) && ReadFile(f, buf, n, &got, NULL) && got == n;
}

static int find_symbols(const wchar_t *exe, Want *want, int nwant) {
    HANDLE f = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { dlog("dropin: cannot open the exe (%lu)", GetLastError()); return 0; }
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    int ok = read_at(f, 0, &dos, sizeof dos) && read_at(f, dos.e_lfanew, &nt, sizeof nt);
    if (!ok || nt.FileHeader.PointerToSymbolTable == 0) {
        dlog("dropin: the exe has no COFF symbol table: left out");
        CloseHandle(f); return 0;
    }
    DWORD nsec = nt.FileHeader.NumberOfSections, nsym = nt.FileHeader.NumberOfSymbols;
    IMAGE_SECTION_HEADER *sec = HeapAlloc(GetProcessHeap(), 0, nsec * sizeof *sec);
    uint64_t secoff = dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    uint64_t symoff = nt.FileHeader.PointerToSymbolTable;
    DWORD symbytes = nsym * 18, strsize = 0;
    ok = sec && read_at(f, secoff, sec, nsec * sizeof *sec) && read_at(f, symoff + symbytes, &strsize, 4);
    uint8_t *syms = ok ? HeapAlloc(GetProcessHeap(), 0, symbytes) : NULL;
    char *strs = ok ? HeapAlloc(GetProcessHeap(), 0, strsize + 1) : NULL;
    ok = syms && strs && read_at(f, symoff, syms, symbytes) && read_at(f, symoff + symbytes, strs, strsize);
    CloseHandle(f);
    int found = 0;
    if (ok) {
        strs[strsize] = 0;
        for (DWORD i = 0; i < nsym; i++) {
            const uint8_t *e = syms + (size_t)i * 18;
            char shortname[9]; const char *name;
            if (*(const uint32_t *)e == 0) {
                uint32_t o = *(const uint32_t *)(e + 4);
                name = o < strsize ? strs + o : "";
            } else { memcpy(shortname, e, 8); shortname[8] = 0; name = shortname; }
            int16_t secno = *(const int16_t *)(e + 12);
            uint32_t value = *(const uint32_t *)(e + 8);
            if (secno > 0 && (DWORD)secno <= nsec)
                for (int w = 0; w < nwant; w++)
                    if (!want[w].found && strcmp(name, want[w].name) == 0) {
                        want[w].rva = sec[secno - 1].VirtualAddress + value; want[w].found = 1; found++;
                    }
            i += e[17];
        }
    }
    dlog("dropin: %lu COFF symbols read, %d of %d wanted found", (unsigned long)nsym, found, nwant);
    if (syms) HeapFree(GetProcessHeap(), 0, syms);
    if (strs) HeapFree(GetProcessHeap(), 0, strs);
    if (sec) HeapFree(GetProcessHeap(), 0, sec);
    return found == nwant;
}

/* ---- the entry hook ------------------------------------------------------------------------------------------ */
/* Length of the whole instructions at p that cover at least 5 bytes, from the shapes the lifted functions' prologues
   use (push r, push r8-r15, sub rsp imm8/imm32, mov reg,reg); 0 for anything else (the hook is left out). */
static int prologue_len(const uint8_t *p) {
    int n = 0;
    while (n < 5) {
        const uint8_t *q = p + n;
        if (q[0] >= 0x50 && q[0] <= 0x57) n += 1;
        else if (q[0] == 0x41 && q[1] >= 0x50 && q[1] <= 0x57) n += 2;
        else if (q[0] == 0x48 && q[1] == 0x83 && q[2] == 0xec) n += 4;
        else if (q[0] == 0x48 && q[1] == 0x81 && q[2] == 0xec) n += 7;
        else if ((q[0] == 0x48 || q[0] == 0x49 || q[0] == 0x4c || q[0] == 0x4d) && (q[1] == 0x89 || q[1] == 0x8b) &&
                 (q[2] & 0xc0) == 0xc0) n += 3;
        else if ((q[0] == 0x48 || q[0] == 0x4c) && (q[1] == 0x89 || q[1] == 0x8b) && (q[2] & 0xc7) == 0x44 &&
                 q[3] == 0x24) n += 5;   /* mov [rsp+disp8], reg / mov reg, [rsp+disp8] (never rip-relative) */
        else return 0;
    }
    return n;
}

static uint8_t *alloc_near(uint8_t *target, size_t size) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity, t = (uintptr_t)target;
    for (uintptr_t d = gran; d < 0x7ff00000u; d += gran)
        for (int s = -1; s <= 1; s += 2) {
            uintptr_t a = (s < 0 ? t - d : t + d) & ~(gran - 1);
            void *p = VirtualAlloc((void *)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return p;
        }
    return NULL;
}

static void put_abs_jmp(uint8_t *at, const void *to) {   /* jmp [rip+0]; dq to */
    at[0] = 0xff; at[1] = 0x25; memset(at + 2, 0, 4); memcpy(at + 6, &to, 8);
}

/* Returns the trampoline (the game's original entry) or NULL. */
static void *hook_entry(uint8_t *target, void *hook, const char *what) {
    int len = prologue_len(target);
    if (!len) {
        dlog("dropin: %s: prologue %02x %02x %02x %02x %02x %02x not a known shape: left out", what,
             target[0], target[1], target[2], target[3], target[4], target[5]);
        return NULL;
    }
    uint8_t *page = alloc_near(target, 64);
    if (!page) { dlog("dropin: %s: no memory within 2 GB: left out", what); return NULL; }
    uint8_t *relay = page, *tramp = page + 16;
    put_abs_jmp(relay, hook);
    memcpy(tramp, target, len);
    put_abs_jmp(tramp + len, target + len);
    DWORD old;
    if (!VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &old)) { dlog("dropin: %s: VirtualProtect failed", what); return NULL; }
    int32_t rel = (int32_t)((intptr_t)relay - (intptr_t)(target + 5));
    uint8_t jmp[5] = {0xe9}; memcpy(jmp + 1, &rel, 4);
    memcpy(target, jmp, 5);
    VirtualProtect(target, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, 5);
    dlog("dropin: %s: hooked at %p (%d bytes moved, trampoline %p)", what, (void *)target, len, (void *)tramp);
    return tramp;
}

static void frame_hook(Ctx *c) {
    if (g_frame_ok < 0) {
        uint8_t *mem = g_guest_base();
        uint32_t w = *(volatile uint32_t *)(mem + FRAME_GUEST);
        g_frame_ok = w == FRAME_WORD0;
        dlog("dropin: guest window %p, word at guest %#x = %08x (%s)", (void *)mem, FRAME_GUEST, w,
             g_frame_ok ? "the game's pacibsp: hook on" : "NOT the expected word: pass-through");
    }
    if (g_frame_ok) {
        LONG n = InterlockedIncrement(&g_frames);
        if (n == 1 || n % LOG_EVERY == 0)
            dlog("dropin: frame %ld (x0 %#llx, lr guest %#llx)", n, (unsigned long long)c->x[0], (unsigned long long)c->x[30]);
    }
    g_frame_orig(c);
}

/* argv[1] == "--crash-watch", read from the raw command line (kernel32 GetCommandLineW only: this runs in DllMain under
   the loader lock, so no shell32 CommandLineToArgvW, which would load and call another DLL there; review-mod-153-2).
   argv[0] as the CRT splits it: quoted to the next quote, else to the next space or tab. argv[1] by the CRT's rules
   (CommandLineToArgvW's): a '"' toggles quoting and is dropped, 2n backslashes + '"' give n backslashes, 2n+1 give n and
   a literal '"'. Longshot QUOTES every argument it starts the helper with: `"...\longshot.exe" "--crash-watch" "<pid>" ...`
   (run v1-dropin-43b1a3d9 07:19, the helper's command line in dropin-run.txt); 43b1a3d91 compared the raw text, so the
   quoted word never matched and the helper was hooked again. */
static int is_crash_watch(const wchar_t *c) {
    wchar_t a1[32];
    size_t n = 0;
    int q = 0;
    if (!c) return 0;
    if (*c == L'"') { c++; while (*c && *c != L'"') c++; if (*c) c++; }
    else while (*c && *c != L' ' && *c != L'\t') c++;
    while (*c == L' ' || *c == L'\t') c++;
    while (*c && (q || (*c != L' ' && *c != L'\t'))) {
        size_t bs = 0;
        while (*c == L'\') { bs++; c++; }
        if (*c == L'"') {
            for (size_t i = 0; i < bs / 2; i++) { if (n + 1 >= 32) return 0; a1[n++] = L'\'; }
            if (bs % 2) { if (n + 1 >= 32) return 0; a1[n++] = L'"'; } else q = !q;
            c++;
        } else {
            for (size_t i = 0; i < bs; i++) { if (n + 1 >= 32) return 0; a1[n++] = L'\'; }
            if (*c && (q || (*c != L' ' && *c != L'\t'))) { if (n + 1 >= 32) return 0; a1[n++] = *c++; }
        }
    }
    a1[n] = 0;
    return wcscmp(a1, L"--crash-watch") == 0;
}

static void attach(void) {
    char env[8];
    DWORD e = GetEnvironmentVariableA("SQUADALA_DROPIN", env, sizeof env);
    wchar_t exe[MAX_PATH], logp[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wcscpy(logp, exe);
    wchar_t *slash = wcsrchr(logp, L'\\');
    if (!slash || _wcsicmp(slash + 1, L"longshot.exe") != 0) return;   /* only Longshot's game (not its launcher) */
    wcscpy(slash + 1, L"squadala-dropin.log");
    g_log = _wfopen(logp, L"a");
    dlog("dropin: attached to %ls (proof build %s %s)", exe, __DATE__, __TIME__);
    /* The game starts a helper of ITSELF early, `longshot --crash-watch <pid> ...` (Longshot v3.2.0
       host/hos_crash_dialog.h HOS_CRASH_WATCH_ARG; also v3.1.1): same exe, same folder, so it loads this winmm.dll too.
       It never runs the guest, so it gets no hook (run 20261126-064809 hooked both processes). */
    if (is_crash_watch(GetCommandLineW())) { dlog("dropin: the game's --crash-watch helper: forwarding winmm only"); return; }
    if (e && env[0] == '0') { dlog("dropin: SQUADALA_DROPIN=0: forwarding winmm only"); return; }
    Want want[] = {{"f_83b6e300", 0, 0}, {"hos_guest_base", 0, 0}};
    if (!find_symbols(exe, want, 2)) { dlog("dropin: symbols missing: no hook"); return; }
    uint8_t *img = (uint8_t *)GetModuleHandleW(NULL);
    g_guest_base = (uint8_t *(*)(void))(img + want[1].rva);
    g_frame_orig = (A64Fn)hook_entry(img + want[0].rva, (void *)frame_hook, "frame function f_83b6e300");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    (void)r;
    if (why == DLL_PROCESS_ATTACH) { DisableThreadLibraryCalls(h); attach(); }
    return TRUE;
}
