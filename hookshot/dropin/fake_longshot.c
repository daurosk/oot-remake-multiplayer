// SPDX-License-Identifier: GPL-3.0-or-later
/* A stand-in longshot.exe for testing the drop-in with no game (hookshot/dropin/test.ps1 builds it as
   out\fake\longshot.exe beside out\winmm.dll). It has what dropin.c looks for in a Longshot build: a COFF symbol table
   (mingw's default), a lifted-style `void f_83b6e300(Ctx *c)` with a push prologue, `hos_guest_base` returning a guest
   window whose word at guest 0x83b6e300 is pacibsp, and a static import of winmm (timeBeginPeriod / timeGetTime),
   so Windows loads winmm.dll from the exe's folder first: our proxy.
   It calls the frame function 1200 times and checks the game's own body ran each time (the trampoline), then that
   the forwarded winmm calls answered. Exit 0 = PASS; the drop-in's own lines are in out\fake\squadala-dropin.log.
   argv[1] = "badword": the guest word is not pacibsp (the hook must pass through, the body still runs). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct Ctx { uint64_t x[32]; } Ctx;
static uint8_t *g_base;
volatile uint64_t g_body_runs;

__attribute__((noinline)) uint8_t *hos_guest_base(void) { return g_base; }

/* (-O0 for this file: push rbp; mov rbp, rsp; sub rsp: a shape dropin.c's prologue decoder takes) */
__attribute__((noinline)) void f_83b6e300(Ctx *c) {
    volatile uint64_t keep[4] = {c->x[0], c->x[1], c->x[2], c->x[3]};
    g_body_runs += 1 + (keep[0] & 0);
}

int main(int argc, char **argv) {
    int bad = argc > 1 && !strcmp(argv[1], "badword");
    /* a 4 GB window like the real one (reserved), with the one page we read committed */
    g_base = VirtualAlloc(NULL, 0x100000000ull, MEM_RESERVE, PAGE_NOACCESS);
    if (!g_base) { printf("FAKE-FAIL no window\n"); return 2; }
    uint8_t *page = VirtualAlloc(g_base + 0x83b6e000u, 0x1000, MEM_COMMIT, PAGE_READWRITE);
    if (!page) { printf("FAKE-FAIL no page\n"); return 2; }
    *(uint32_t *)(g_base + 0x83b6e300u) = bad ? 0x12345678u : 0xd503237fu;
    Ctx c = {{0}};
    for (int i = 0; i < 1200; i++) { c.x[0] = 0x1000 + i; c.x[30] = 0x806e6dd8; f_83b6e300(&c); }
    MMRESULT r = timeBeginPeriod(1);
    DWORD t = timeGetTime();
    timeEndPeriod(1);
    int ok = g_body_runs == 1200 && r == TIMERR_NOERROR && t != 0;
    printf("body runs %llu, timeBeginPeriod %u, timeGetTime %lu: %s\n", (unsigned long long)g_body_runs, r, t,
           ok ? "FAKE-PASS" : "FAKE-FAIL");
    return ok ? 0 : 1;
}
