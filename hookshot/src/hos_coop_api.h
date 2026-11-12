/* SPDX-License-Identifier: GPL-3.0-or-later
   The co-op module's interface to Hookshot (oot-remake-multiplayer, docs/hookshot.md).

   Hookshot's host/coop_bridge.c loads hookshot_coop.dll (hookshot_coop.so on Linux) from beside the game when
   HOS_COOP=1 (or a coop.ini beside the game names a server), and calls its four exports. The same file is in
   Hookshot's host/ through our patch (hookshot/patches) and in our repository (hookshot/src): keep them equal
   (tests/hookshot/api_test.cpp checks it). Plain C, so both sides' compilers agree on the layout. */
#ifndef HOS_COOP_API_H
#define HOS_COOP_API_H
#include <stddef.h>
#include <stdint.h>

#define HOS_COOP_API_VERSION 1u

/* Where the frame hook ran (nx2/coop_hooks.py): the two calls of the frame function in moduleCalc_, as the emulator's
   cave has them (lichii/hook/coop_svc.h kHooks). */
#define HOS_COOP_SITE_RARE 0    /* main+0x6e6dd8 */
#define HOS_COOP_SITE_FRAME 1   /* main+0x6e70f8: every frame */
/* Added after the first version: the game's present (nvnQueuePresentTexture, the emulator's composite), loads and
   pause included; ctx is NULL there and call() may not be used. Only once the module is loaded. */
#define HOS_COOP_SITE_PRESENT 2

/* One game function call (CoopPuppet::Call): x0-x7, four words at sp, s0. */
typedef struct HosCoopCall {
    uint64_t function;   /* guest address */
    uint64_t x[8];
    uint64_t stack[4];
    float s0;
} HosCoopCall;

typedef struct HosCoopApi {
    uint32_t version;             /* HOS_COOP_API_VERSION */
    uint32_t size;                /* sizeof(HosCoopApi) */
    uint64_t main_base;           /* main's guest address (0x80020000) */
    const char *main_build_id;    /* main's build id, hex (32 digits) */
    const char *game_dir;         /* the folder of the game's executable, UTF-8, no trailing separator */
    /* Guest memory, any thread. 0 when any byte is outside mapped guest memory (nothing copied). */
    int (*read)(uint64_t address, void *out, size_t size);
    int (*write)(uint64_t address, const void *in, size_t size);   /* also marks the pages for the GPU mirror */
    /* Atomic compare-and-swap of a guest u32; 1 when mapped, *seen = the value that was there. */
    int (*cas32)(uint64_t address, uint32_t expected, uint32_t desired, uint32_t *seen);
    /* Calls a game function, only from inside hos_coop_frame (on the game's thread, with its context). 0 when there is
       no function at that address. *x0 = what it returned. */
    int (*call)(void *ctx, const HosCoopCall *call, uint64_t *x0);
    void (*log)(const char *line);   /* one line into hookshot_log.txt */
    /* Added after the first version (check size covers it): the buttons word the game was last given for player 1
       (Switch NpadButton bits, as nn::hid::GetNpadStates hands them, note keys included while the game waits for a
       note), any thread. */
    uint64_t (*pad_buttons)(void);
    /* Added with Linux's keys (check size covers it): 1 while the key (a Windows virtual-key code) is held in the
       game's window, any thread: GetAsyncKeyState's on Windows, the SDL window's keys on Linux (as nn_hid.c reads
       them: nothing while the window does not have the keyboard). */
    int (*key_down)(uint32_t vk);
} HosCoopApi;

/* The module's exports (extern "C"). */
typedef int (*hos_coop_start_fn)(const HosCoopApi *api);   /* 1: co-op on; 0: off (the bridge unloads it) */
typedef void (*hos_coop_frame_fn)(void *ctx, int site);   /* the game's calc thread at a frame hook; the presenting
                                                             thread at HOS_COOP_SITE_PRESENT */
typedef uint32_t (*hos_coop_hold_fn)(uint64_t component, uint32_t bits);   /* the AI hold: a BSA component's calc bits */
typedef void (*hos_coop_stop_fn)(void);                    /* at exit */
/* Typing, added after the first version (optional exports; an older module has none): Hookshot's window (display.c)
   offers each key press (virtual-key code, repeat 1 for an auto-repeat) and each character (WM_CHAR of the ANSI window:
   a byte of the active code page; on Linux, host/posix/display_sdl.c: SDL's typed text as Unicode code points) to the
   module first; 1 = taken, Hookshot does nothing more with it. While
   hos_coop_keys_taken says 1, the game reads no keys or mouse buttons (nn_hid.c key), as while the Insert menu is open. */
typedef int (*hos_coop_key_fn)(uint32_t vk, int repeat);   /* the window's thread */
typedef int (*hos_coop_char_fn)(uint32_t ch);              /* the window's thread */
typedef int (*hos_coop_keys_taken_fn)(void);               /* any thread, often: cheap */
/* The mouse, added with the cheats panel's clicks (an optional export): the cursor as a fraction of the window's client
   area (0 to 1 across and down), offered first while neither Hookshot's menu nor a game menu has the mouse; 1 = taken,
   Hookshot does nothing more with it (a click taken does not capture the mouse for the camera). */
enum { HOS_COOP_MOUSE_MOVE = 0, HOS_COOP_MOUSE_LEFT = 1, HOS_COOP_MOUSE_WHEEL_UP = 2, HOS_COOP_MOUSE_WHEEL_DOWN = 3 };
typedef int (*hos_coop_mouse_fn)(float fx, float fy, int event);   /* the window's thread */

#endif
