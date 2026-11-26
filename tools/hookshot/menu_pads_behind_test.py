"""Patch 0039 (player report 261): Longshot's Insert menu takes the pads the game would, whichever window is in front.
Run on Longshot's source with our patches applied (the linux-longshot job: the kit's data/builder/source after its
build.sh --renderer-only):
    python -I -B menu_pads_behind_test.py <patched source> [--cc gcc]
Adapted from Longshot's own tests/test_pads_after_menu.py (GPL-3.0, as this repo), which the Linux AppImage the kit
patches does not ship. host/nn_hid.c's pads_read, bound_pad_buttons, pads_to_game (read_input's pad block, in front and
behind other windows), test_presses_to_menu and hos_pad_raw are compiled with fakes for SDL, XInput and the clock; no
device is read. Checked: after the menu each pad input waits until let go; behind other windows the open menu's pads
are its own and never the game's; a test's presses (HOS_INPUT_SCRIPT, the control bridge) while it is open are the
menu's, as the pad inputs bound to them; and vk_menu.cpp reads the pads with no focus test of its own.
Exit status 0 when all hold.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path



def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


PRELUDE = r"""#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d %s\n", __LINE__, #x); exit(1); } } while (0)
typedef long LONG;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef void *HMODULE;
#define WINAPI
#define ERROR_SUCCESS 0
#define XUSER_MAX_COUNT 4
enum { XINPUT_GAMEPAD_DPAD_UP = 1, XINPUT_GAMEPAD_DPAD_DOWN = 2, XINPUT_GAMEPAD_DPAD_LEFT = 4, XINPUT_GAMEPAD_DPAD_RIGHT = 8,
       XINPUT_GAMEPAD_START = 16, XINPUT_GAMEPAD_BACK = 32, XINPUT_GAMEPAD_LEFT_THUMB = 64, XINPUT_GAMEPAD_RIGHT_THUMB = 128,
       XINPUT_GAMEPAD_LEFT_SHOULDER = 256, XINPUT_GAMEPAD_RIGHT_SHOULDER = 512, XINPUT_GAMEPAD_A = 4096,
       XINPUT_GAMEPAD_B = 8192, XINPUT_GAMEPAD_X = 16384, XINPUT_GAMEPAD_Y = 32768 };
typedef struct { WORD wButtons; uint8_t bLeftTrigger, bRightTrigger; int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XINPUT_GAMEPAD;
typedef struct { DWORD dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XINPUT_STATE *);
static XInputGetStateFn xinput_get;   /* (no XInput: SDL has the pads) */
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x800
static HMODULE LoadLibraryA(const char *name) { (void)name; return NULL; }
static HMODULE LoadLibraryExA(const char *name, void *file, unsigned flags) { (void)name; (void)file; (void)flags; return NULL; }
static void *GetProcAddress(HMODULE m, const char *name) { (void)m; (void)name; return NULL; }
static LONG InterlockedExchange(volatile LONG *p, LONG v) { LONG o = *p; *p = v; return o; }
typedef int SRWLOCK;
#define SRWLOCK_INIT 0
static void AcquireSRWLockShared(SRWLOCK *p) { (void)p; }
static void ReleaseSRWLockShared(SRWLOCK *p) { (void)p; }
static void AcquireSRWLockExclusive(SRWLOCK *p) { (void)p; }
static void ReleaseSRWLockExclusive(SRWLOCK *p) { (void)p; }
typedef void *HANDLE;
typedef DWORD(WINAPI *LPTHREAD_START_ROUTINE)(void *);
static HANDLE CreateThread(void *a, size_t s, LPTHREAD_START_ROUTINE fn, void *arg, DWORD f, DWORD *id) { (void)a; (void)s; (void)fn; (void)arg; (void)f; (void)id; return NULL; }
static int CloseHandle(HANDLE h) { (void)h; return 1; }
static void Sleep(DWORD ms) { (void)ms; }
void hos_host_thread_register_self(const char *name) { (void)name; }
int hos_input_xinput_counts(int p) { (void)p; return 1; }   /* (our patch 0015, pad_sdl.c: every XInput pad is this game's) */
static void hos_log(const char *format, ...) { (void)format; }
static int64_t now_ns = 1000000000LL;
static int64_t hos_now_ns(void) { return now_ns; }
static int menu_open;
int rx_option(int id, int otherwise) { return id == 1 ? menu_open : id == 11 || id == 16 || id == 17 ? 0 : otherwise; }
static uint64_t pads_down;      /* (by position, 1 << HOS_PAD_*: every pad SDL has, merged as pad_sdl.c merges them) */
static int32_t pads_stick[4];
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4], int *deck_builtin_input) { (void)deck_builtin_input; *buttons |= pads_down; memcpy(stick, pads_stick, sizeof pads_stick); return 2; }
static void player_used(void) {}
static void binds_load(void) {}
static SRWLOCK pad_bind_lock = SRWLOCK_INIT;
static int pad_bind[HOS_KEY_N][2] = {[HOS_KEY_TOGGLE_HUD] = {-1, -1}};
static int sampled_pad(int i, int k) { return pad_bind[i][k]; }
static LONG hud_bind_generation;
static uint64_t oc_consumed_pads;
static int sprint_now;   /* (Sprint's gamepad input, read with the pads: tests/test_sprint.py) */
@@SPRINT_ENUM@@
static int sprint_read(void) { return sprint_now; }
"""

MAIN = r"""
/* one read of the game's: what the pads give it */
static uint64_t game(int32_t stick[4]) {
    now_ns += 16000000;   /* (past pads_read's 2 ms cache) */
    uint64_t buttons = 0;
    memset(stick, 0, 4 * sizeof *stick);
    sprint_now = 0;   /* read_input clears the sampled state before its pad block */
    game_pads(&buttons, stick);
    return buttons;
}
/* one read of the game's with the window behind another one (Controller in background on) */
static uint64_t game_behind(int32_t stick[4]) {
    now_ns += 16000000;
    uint64_t buttons = 0;
    memset(stick, 0, 4 * sizeof *stick);
    sprint_now = 0;
    (void)pads_to_game(&buttons, stick, NULL, 0);
    return buttons;
}
/* one read of the game's with a test's presses (the script's or the control bridge's) and no pad */
static uint64_t game_test(uint64_t pressed, int32_t stick[4]) {
    uint64_t buttons = pressed;
    stick[0] = pressed ? 20000 : 0, stick[1] = stick[2] = stick[3] = 0;
    test_presses_to_menu(&buttons, stick);
    return buttons;
}
#define PAD(p) (1ULL << HOS_PAD_##p)
#define SW(b) (1ULL << HOS_SW_##b)

int main(void) {
    for (int i = 0; i < HOS_SW_N; i++) pad_bind[i][0] = pad_default[i], pad_bind[i][1] = -1;
    int32_t s[4];
    CHECK(game(s) == 0);
    pads_down = PAD(SOUTH); CHECK(game(s) == SW(B));   /* (by position: the bottom face button is the Switch's B) */
    pads_down = 0; CHECK(game(s) == 0);

    /* Sprint held through closing the menu waits for release, even while moving. */
    pad_bind[HOS_SW_LS][0] = -1;   /* the automatic layout moved L3 from LS to Sprint */
    pad_bind[HOS_KEY_SPRINT][0] = HOS_PAD_LSTICK;
    pad_bind[HOS_KEY_SPRINT][1] = -1;
    menu_open = 1; pads_down = PAD(LSTICK); pads_stick[0] = 30000; CHECK(game(s) == 0);
    menu_open = 0; CHECK(game(s) == 0 && sprint_now == 0 && s[0] == 30000);
    CHECK(game(s) == 0 && sprint_now == 0);
    pads_down = 0; CHECK(game(s) == 0 && sprint_now == 0);
    pads_down = PAD(LSTICK); CHECK(game(s) == 0 && (sprint_now & SPRINT_PAD));   /* (and SPRINT_L3: on the left stick's press) */
    pads_down = 0; pads_stick[0] = 0; CHECK(game(s) == 0);

    /* A trigger that stays down through the menu (worn, or past its rest), the menu closed with East (A) */
    menu_open = 1;
    pads_down = PAD(LTRIGGER); CHECK(game(s) == 0);
    pads_stick[0] = 30000; pads_down |= PAD(EAST); CHECK(game(s) == 0 && s[0] == 0);   /* (the menu's own, sticks too) */
    menu_open = 0;
    CHECK(game(s) == 0);                    /* the press that closed it and the trigger wait, */
    CHECK(s[0] == 30000);                   /* the sticks count at once, */
    pads_down = PAD(LTRIGGER); pads_stick[0] = 0;
    CHECK(game(s) == 0);
    pads_down |= PAD(SOUTH); CHECK(game(s) == SW(B));   /* and another button counts with the trigger still down, */
    pads_down = PAD(LTRIGGER) | PAD(EAST); CHECK(game(s) == SW(A));   /* also the one that closed it, pressed again */
    pads_down = 0; CHECK(game(s) == 0);
    pads_down = PAD(LTRIGGER); CHECK(game(s) == SW(ZL));   /* the trigger once it was let go */
    pads_down = 0; CHECK(game(s) == 0);

    /* The menu closed by a press no read saw while it was open: it waits too */
    menu_open = 1; CHECK(game(s) == 0);
    menu_open = 0; pads_down = PAD(EAST); CHECK(game(s) == 0);
    CHECK(game(s) == 0);
    pads_down = 0; CHECK(game(s) == 0);
    pads_down = PAD(EAST); CHECK(game(s) == SW(A));
    pads_down = 0; CHECK(game(s) == 0);

    /* Something resting on a second pad: its button waits, the first pad's count */
    menu_open = 1; pads_down = PAD(NORTH); CHECK(game(s) == 0);
    menu_open = 0; CHECK(game(s) == 0);
    pads_down |= PAD(WEST) | PAD(DOWN); CHECK(game(s) == (SW(Y) | SW(DOWN)));
    pads_down = 0; CHECK(game(s) == 0);

    /* Behind other windows (patch 0039): the same, the open menu's pads never reach the game; the menu has them */
    menu_open = 1; pads_down = PAD(SOUTH); CHECK(game_behind(s) == 0 && hos_pad_raw() == PAD(SOUTH));
    menu_open = 0; CHECK(game_behind(s) == 0);   /* (the B that closed it waits until let go) */
    pads_down = 0; CHECK(game_behind(s) == 0);
    pads_down = PAD(SOUTH); CHECK(game_behind(s) == SW(B));
    pads_down = 0; CHECK(game_behind(s) == 0);

    /* A test's presses while the menu is open: the menu's, as the pad inputs bound to them, not the game's (sticks too) */
    menu_open = 1;
    CHECK(game_test(SW(B), s) == 0 && s[0] == 0 && hos_pad_raw() == PAD(SOUTH));
    CHECK(game_test(SW(A) | SW(DOWN), s) == 0 && hos_pad_raw() == (PAD(EAST) | PAD(DOWN)));
    menu_open = 0;
    CHECK(game_test(SW(A), s) == 0 && hos_pad_raw() == 0);   /* the press that closed it waits until let go, */
    CHECK(game_test(0, s) == 0);
    CHECK(game_test(SW(A), s) == SW(A) && s[0] == 20000);   /* then counts again, */
    CHECK(game_test(SW(B), s) == SW(B) && hos_pad_raw() == 0);   /* and with the menu shut never reaches it */
    CHECK(game_test(0, s) == 0);
    puts("PASS: after the Insert menu each pad input waits until let go; the others and the sticks count at once");
    puts("PASS: behind other windows the open menu's pads are its own, and a test's presses are the menu's while it is open");
    return 0;
}
"""


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", help="Longshot's source with our patches applied")
    p.add_argument("--cc", default="gcc")
    a = p.parse_args()
    ROOT = Path(a.source)
    ENV_ON_H = str(ROOT / "gpu" / "vk" / "compat" / "longshot_env.h")
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    if "const uint32_t raw = host && host->pad_raw ? host->pad_raw() : 0, pad_press = raw & ~pad_held;" not in menu:
        raise SystemExit("vk_menu.cpp reads the pads with a focus test of its own again (or patch 0039 is not applied)")
    hid = (ROOT / "host" / "nn_hid.c").read_text(encoding="utf-8")
    api = (ROOT / "gpu" / "vk" / "vk_api.h").read_text(encoding="utf-8")
    read = block(hid, "static void read_input(")
    if "down = pads_to_game(buttons, stick, &deck_builtin_input, 1);" not in read:
        raise SystemExit("read_input no longer gives the pads through pads_to_game")
    if "test_presses_to_menu(buttons, stick);" not in read:
        raise SystemExit("read_input no longer gives a test's presses to the open menu")
    state = hid[hid.index("static volatile LONG pad_raw_now;"):hid.index("uint32_t hos_pad_raw(")]
    code = "\n".join((
        block(api, "enum { HOS_SW_A") + ";", block(api, "enum { HOS_PAD_SOUTH") + ";",
        block(api, "enum { HOS_KEY_MOVE_UP") + ";",
        re.search(r"^enum \{ HOS_PAD_TOUCHPAD = \d+ \};", api, re.M).group(0),
        PRELUDE.replace("@@SPRINT_ENUM@@", re.search(r"^enum \{ SPRINT_KEY = .*$", hid, re.M).group(0)),
        block(hid, "static const signed char pad_default[HOS_SW_N] =") + ";",
        block(hid, "static void stick_max("), block(hid, "static uint64_t hud_pad_mask("),
        block(hid, "static uint64_t bound_pad_buttons("), state,
        block(hid, "static int pad_sprint_down("),
        block(hid, "void hos_filter_stick("),
        # (the XInput code before pads_read: its thread and its reading; never started here, SDL has the pads)
        hid[hid.index("static volatile int xinput_pads_now;"):hid.index("static void pads_read(")],
        block(hid, "static void pads_read("),
        block(hid, "uint32_t hos_pad_raw("),
        block(hid, "static uint64_t pads_to_game("),
        block(hid, "static void test_presses_to_menu("),
        "static void game_pads(uint64_t *buttons, int32_t stick[4]) {\n    int deck_builtin_input = 0;\n"
        "    (void)pads_to_game(buttons, stick, &deck_builtin_input, 1);\n}",
        MAIN))
    with tempfile.TemporaryDirectory(prefix="coop-menu-pads-behind-") as tmp:
        src = Path(tmp) / "t.c"
        src.write_text(code, encoding="utf-8")
        exe = Path(tmp) / "t.exe"
        subprocess.run([a.cc, "-std=c11", "-include", ENV_ON_H, "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-o", str(exe), str(src)], check=True)
        r = subprocess.run([str(exe)], capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        if r.returncode:
            raise SystemExit(1)


if __name__ == "__main__":
    main()
