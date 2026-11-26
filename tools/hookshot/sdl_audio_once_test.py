"""Patch 0033 (tracker row 156): Longshot's microphones start SDL's audio once, not at every list and open.
Run on Longshot's source with our patches applied, with Longshot's own llvm-mingw (tools/hookshot/host_compile_check.ps1
runs it after the syntax check):
    python -I -B sdl_audio_once_test.py <patched source> <hookshot/patches folder> <llvm-mingw folder>
host/hos_sdl.c is built against a stand-in SDL3.dll that counts SDL_InitSubSystem's audio callers in 8 bits and, when the
count is 0 again, starts SDL's audio from scratch the way SDL 3.4's SDL_InitAudio does: the running audio quits first and
every SDL_AudioStream is destroyed. Then a microphone is opened and kept, and 1000 rounds of what host/nn_audio_in.c and
the co-op module do with it run: list the microphones (nn_audio_in.c's 5 s check of a chosen one, the co-op Settings row),
open and close another one (the co-op voice's push to talk), and read the kept one (pump). With patch 0033 SDL's audio is
started once and the kept stream stays valid all along. The same file without patch 0033 (reversed here) must fail the
same rounds, as the players' games did: so the stand-in is shown to be the crash.
Exit status 0 when both hold.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hs  # noqa: E402  (run_quiet: a console child with no window)

ROUNDS = 1000

HOS_H = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
void hos_log(const char *fmt, ...);
int64_t hos_now_ns(void);
'''

# The stand-in SDL3.dll. Every SDL_ name hos_sdl.c asks for is exported (unknown ones return 0); these act as SDL 3.4 does.
FAKE_SDL = r'''
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define API __declspec(dllexport)
typedef struct Stream { uint32_t magic; struct Stream *next; } Stream;
static Stream *streams;                 /* (SDL's existing streams) */
static uint8_t audio_count;             /* (SDL_SubsystemRefCount[audio]: 8 bits, as SDL's) */
static int driver_running, inits, quits, freed_uses, live;
static int valid(void *s) {
    for (Stream *p = streams; p; p = p->next)
        if (p == s && p->magic == 0x5354524du) return 1;
    return 0;
}
static void quit_audio(void) {          /* (SDL_QuitAudio: every stream destroyed) */
    while (streams) {
        Stream *s = streams;
        streams = s->next, s->magic = 0, live--;
        /* (kept allocated, as freed memory that is still mapped: a use is counted, not a crash) */
    }
    quits++;
}
API _Bool SDL_InitSubSystem(uint32_t flags) {
    if (flags & 0x10) {
        if (audio_count == 0) {          /* (SDL_ShouldInitSubsystem) */
            audio_count++;
            if (driver_running) quit_audio();   /* (SDL_InitAudio: "shutdown driver if already running") */
            driver_running = 1, inits++;
        } else {
            audio_count++;               /* (SDL_IncSubsystemRefCount: wraps at 256) */
        }
    }
    return 1;
}
API void SDL_QuitSubSystem(uint32_t flags) { (void)flags; }
API void *SDL_malloc(size_t n) { return malloc(n); }
API void SDL_free(void *p) { free(p); }
API const char *SDL_GetError(void) { return ""; }
API uint32_t *SDL_GetAudioRecordingDevices(int *count) {
    uint32_t *ids = (uint32_t *)malloc(2 * sizeof *ids);
    ids[0] = 7, ids[1] = 0;
    if (count) *count = 1;
    return ids;
}
API const char *SDL_GetAudioDeviceName(uint32_t id) { return id == 7 ? "Test microphone" : NULL; }
API void *SDL_OpenAudioDeviceStream(uint32_t id, const void *spec, void *cb, void *user) {
    (void)id, (void)spec, (void)cb, (void)user;
    Stream *s = (Stream *)calloc(1, sizeof *s);
    s->magic = 0x5354524du, s->next = streams, streams = s, live++;
    return s;
}
API _Bool SDL_ResumeAudioStreamDevice(void *s) { return valid(s); }
API int SDL_GetAudioStreamAvailable(void *s) {
    if (!valid(s)) { freed_uses++; return -1; }
    return 64;
}
API int SDL_GetAudioStreamData(void *s, void *buf, int len) {
    if (!valid(s)) { freed_uses++; return -1; }
    memset(buf, 0, (size_t)len);
    return len;
}
API _Bool SDL_ClearAudioStream(void *s) {
    if (!valid(s)) { freed_uses++; return 0; }
    return 1;
}
API void SDL_DestroyAudioStream(void *s) {
    if (!s) return;
    if (!valid(s)) { freed_uses++; return; }
    Stream **p = &streams;
    while (*p != s) p = &(*p)->next;
    *p = ((Stream *)s)->next, ((Stream *)s)->magic = 0, live--;
    free(s);
}
API void fake_sdl_counts(int *out) {
    out[0] = inits, out[1] = quits, out[2] = freed_uses, out[3] = live, out[4] = audio_count;
}
'''

HARNESS = r'''
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <windows.h>
#include "hos_sdl.h"
void hos_log(const char *fmt, ...) { (void)fmt; }
int64_t hos_now_ns(void) { return 0; }
int main(void) {
    void (*counts)(int *) = (void (*)(int *))GetProcAddress(GetModuleHandleA("SDL3.dll"), "fake_sdl_counts");
    int c[5];
    uint32_t ids[64];
    char names[64][128];
    unsigned char buf[256];
    if (hos_sdl_mic_list(ids, names, 64) != 1) { puts("no microphone listed"); return 2; }
    void *kept = hos_sdl_mic_open(7, 48000, 1, 0x8010);
    if (!kept || !(counts = counts ? counts : (void (*)(int *))GetProcAddress(LoadLibraryA("SDL3.dll"), "fake_sdl_counts"))) {
        puts("the microphone did not open");
        return 2;
    }
    for (int i = 0; i < %ROUNDS%; i++) {
        hos_sdl_mic_list(ids, names, 64);
        void *other = hos_sdl_mic_open(7, 48000, 1, 0x8010);
        hos_sdl_mic_close(other);
        if (hos_sdl_mic_available(kept) > 0) hos_sdl_mic_read(kept, buf, sizeof buf);
    }
    counts(c);
    printf("rounds %d: audio started %d, quit %d, freed streams used %d, live streams %d, count %d\n", %ROUNDS%, c[0], c[1],
           c[2], c[3], c[4]);
    return c[0] == 1 && c[1] == 0 && c[2] == 0 && c[3] == 1 ? 0 : 1;
}
'''


def sdl_names(code):
    return sorted(set(re.findall(r'"(SDL_[A-Za-z0-9_]+)"', code)))


def build_and_run(name, sdl_c, src, cc, work):
    """hos_sdl.c (sdl_c's text) and the stand-in SDL3.dll in work/name; the harness's exit status and line"""
    d = os.path.join(work, name)
    os.makedirs(d)
    with open(os.path.join(d, "hos.h"), "w", newline="\n") as f:
        f.write(HOS_H)
    with open(os.path.join(d, "hos_sdl.c"), "w", newline="\n") as f:
        f.write(sdl_c)
    defined = set(re.findall(r"API [^(]*?\b(SDL_\w+)\(", FAKE_SDL))
    stubs = "".join("API void *%s(void) { return 0; }\n" % n for n in sdl_names(sdl_c) if n not in defined)
    with open(os.path.join(d, "fake_sdl.c"), "w", newline="\n") as f:
        f.write(FAKE_SDL + stubs)
    with open(os.path.join(d, "harness.c"), "w", newline="\n") as f:
        f.write(HARNESS.replace("%ROUNDS%", str(ROUNDS)))
    run = lambda args: hs.run_quiet(args, cwd=d, capture_output=True, text=True)
    r = run([cc, "-shared", "-O1", "-w", "-o", "SDL3.dll", "fake_sdl.c"])
    if r.returncode:
        raise SystemExit("sdl_audio_once_test: the stand-in SDL3.dll did not build:\n" + r.stderr[-3000:])
    inc = ["-I", d, "-I", os.path.join(src, "host")]
    objs = []
    for c in ("hos_sdl.c", "harness.c"):
        o = c[:-2] + ".o"
        r = run([cc, "-c", "-O1", "-w", "-std=gnu11", *inc, "-o", o, c])
        if r.returncode:
            raise SystemExit("sdl_audio_once_test: %s did not compile:\n%s" % (c, r.stderr[-3000:]))
        objs.append(o)
    # hos_sdl.c's other parts (window, controllers, sound output) call more of Longshot: stand-ins for whatever the
    # link finds missing (never the microphones' own: those are in hos_sdl.c), then link again.
    for _ in range(3):
        r = run([cc, "-o", "harness.exe", *objs, "-luser32", "-lgdi32", "-lshell32", "-lole32", "-ladvapi32"])
        if r.returncode == 0:
            break
        # (Longshot 3.1: lld names a dllimport reference "__declspec(dllimport) <name>")
        missing = sorted(set(re.findall(r"undefined symbol: (?:__declspec\(dllimport\) )?(\w+)", r.stdout + r.stderr)))
        if not missing:
            raise SystemExit("sdl_audio_once_test: the harness did not link:\n" + (r.stdout + r.stderr)[-3000:])
        with open(os.path.join(d, "stubs.c"), "a", newline="\n") as f:
            for m in missing:
                f.write("void *%s(void) { return 0; }\n" % m if not m.startswith("__imp_") else "")
        rs = run([cc, "-c", "-O1", "-w", "-o", "stubs.o", "stubs.c"])
        if rs.returncode:
            raise SystemExit("sdl_audio_once_test: stand-ins did not compile:\n" + rs.stderr[-3000:])
        if "stubs.o" not in objs:
            objs.append("stubs.o")
    else:
        raise SystemExit("sdl_audio_once_test: the harness did not link:\n" + (r.stdout + r.stderr)[-3000:])
    r = run([os.path.join(d, "harness.exe")])
    line = (r.stdout.strip().splitlines() or ["(no output)"])[-1]
    print("%s: %s (exit %d)" % (name, line, r.returncode), flush=True)
    return r.returncode


def main():
    src, patches, mingw = (os.path.abspath(a) for a in sys.argv[1:4])
    cc = os.path.join(mingw, "bin", "x86_64-w64-mingw32-clang.exe")
    if not os.path.isfile(cc):
        raise SystemExit("sdl_audio_once_test: no compiler at %s" % cc)
    with open(os.path.join(src, "host", "hos_sdl.c"), encoding="utf-8", newline="") as f:
        patched = f.read()
    if "rec.audio_on" not in patched:
        raise SystemExit("sdl_audio_once_test: host/hos_sdl.c has no patch 0033")
    work = tempfile.mkdtemp(prefix="sdl-audio-once-")
    try:
        # The file without patch 0033: the patch reversed on a copy.
        old_tree = os.path.join(work, "old")
        os.makedirs(os.path.join(old_tree, "host"))
        shutil.copy(os.path.join(src, "host", "hos_sdl.c"), os.path.join(old_tree, "host", "hos_sdl.c"))
        p = os.path.join(patches, "0033-coop-sdl-audio-once.patch")
        r = hs.run_quiet(["git", "apply", "-R", "--unidiff-zero", p], cwd=old_tree, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("sdl_audio_once_test: patch 0033 did not reverse:\n" + r.stderr)
        with open(os.path.join(old_tree, "host", "hos_sdl.c"), encoding="utf-8", newline="") as f:
            old = f.read()
        now = build_and_run("with-0033", patched, src, cc, work)
        before = build_and_run("without-0033", old, src, cc, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if now != 0:
        raise SystemExit("sdl_audio_once_test: FAIL: with patch 0033 SDL's audio restarted or a stream was used freed")
    if before == 0:
        raise SystemExit("sdl_audio_once_test: FAIL: without patch 0033 the rounds passed too: the stand-in shows nothing")
    print("sdl_audio_once_test: OK (%d rounds: one start with 0033; without it the restart frees the kept stream)" % ROUNDS)


if __name__ == "__main__":
    main()
