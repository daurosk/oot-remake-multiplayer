// NX2navi plugin interface, version 2: what a mod's native library (a DLL loaded into the game's process) can ask of the
// game. docs/plugins.md is the guide; runtime/plugins.c is the implementation.
//
// Copyright (c) 2026 chrg6.
//
// This one file is an exception to LICENSE-ADDITIONS. Permission is granted, free of charge, to any person to copy
// this file, unchanged or changed, into a mod for NX2navi, to compile it into that mod and to distribute it with that
// mod in source or binary form under the mod's own license, provided this notice stays in every copy of the file.
// The permission covers this file alone and nothing else of the repository it comes from.
// THE FILE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY ARISING FROM THE FILE OR ITS USE.
//
// Getting the interface, either way:
//   - export `void nx2navi_plugin_init(const Nx2PluginApi *api)` from the library. The game calls it once, on the
//     process's first thread, after the game's modules are in memory and before any of the game's code runs, for every
//     library named in the environment variable NX2NAVI_MOD_DLLS (the launcher sets it for the DLLs of the enabled mods);
//   - or ask the executable, at any time and from any thread:
//       Nx2PluginApiFn get = (Nx2PluginApiFn)GetProcAddress(GetModuleHandleW(NULL), NX2_PLUGIN_API_EXPORT);
//       const Nx2PluginApi *api = get ? get(NX2_PLUGIN_API_VERSION) : NULL;    // NULL: a game without the interface
//
// Compatibility: every structure starts with its size in bytes as the game built it. A later game only adds members at
// the end of a structure, so a member is there when `size` covers it (NX2_HAS). nx2navi_plugin_api(v) returns NULL when
// the game cannot serve version v (v is newer than the game, or older than the oldest it still serves). All functions
// use the platform's C calling convention; text is UTF-8.
//
// Version 2 added, all at the end of what version 1 had: function hooks (hook_function, call_original), map_range,
// on_exit, title menu buttons (add_title_button), the game's buttons as events (on_pad), the stack arguments of
// Nx2GuestRegs and the folders of Nx2GameInfo. A plugin built against version 1 runs
// unchanged.
//
// Threads, in short (docs/plugins.md has the rules): frame callbacks run on the game's thread between two frames; hook
// callbacks run on whichever thread of the game calls the hooked function; guest_call works in those two and nowhere
// else; overlay callbacks run on the thread that presents; key and text callbacks run on the window's thread. A
// library that registered a callback must stay loaded (the game pins it).
#ifndef NX2NAVI_PLUGIN_API_H
#define NX2NAVI_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NX2_PLUGIN_API_VERSION 2u
#define NX2_PLUGIN_API_EXPORT "nx2navi_plugin_api"     // exported by the game's executable
#define NX2_PLUGIN_INIT_EXPORT "nx2navi_plugin_init"   // optionally exported by a plugin

// 1 when the structure *p of the given type, whose first member is its size, holds the member m:
// NX2_HAS(api, Nx2PluginApi, capture_input).
#define NX2_HAS(p, type, m) ((p)->size >= offsetof(type, m) + sizeof(((const type *)0)->m))

// A colour for the drawing calls: red in the low byte, then green, blue and alpha (255 opaque).
#define NX2_RGBA(r, g, b, a) \
    ((uint32_t)(uint8_t)(r) | (uint32_t)(uint8_t)(g) << 8 | (uint32_t)(uint8_t)(b) << 16 | (uint32_t)(uint8_t)(a) << 24)

// What the game is. The pointers stay valid for the life of the process.
typedef struct Nx2GameInfo {
    uint32_t size;
    uint32_t game_version;        // 111 for game 1.1.1
    const char *runtime_version;  // NX2navi's own version, "0.9.0"
    const char *game_dir;         // the folder of the game's executable, without a trailing separator
    const char *mods_dir;         // the mods folder the game reads
    const char *build_id;         // hex of the build id of the game's main module (64 digits); "" when unknown
    uint64_t main_base;           // where main is mapped: a guest address is an address in this process
    uint64_t main_size;
    // ---- version 2
    const char *profile_dir;      // the profile folder the game keeps its saves, cache and account in
    const char *save_dir;         // the folder of the committed save, <profile_dir>/save/<account>; "" until the game has
                                  // opened its storage (some seconds after start): look again later
} Nx2GameInfo;

// One frame callback's view of the frame.
typedef struct Nx2Frame {
    uint32_t size;
    uint32_t reserved;
    uint64_t index;               // frames the game presented before this one
} Nx2Frame;

// Registers for guest_call_regs, in and out: x0-x7 are the integer arguments and x0, x1 the results; x8 is the address
// of a result returned in memory; v0-v7 are the floating-point and vector arguments and results, each as two 64-bit
// halves, low first (a float is the low 32 bits of the low half, a double the low half).
// Version 2: stack points at nstack 64-bit words (at most NX2_STACK_ARGS_MAX), the arguments the callee takes on the
// stack, the first at the lowest address; they are copied below the thread's stack pointer for the call. A block whose
// size stops before them (a version 1 plugin's) passes none.
#define NX2_STACK_ARGS_MAX 64
typedef struct Nx2GuestRegs {
    uint32_t size;
    uint32_t reserved;
    uint64_t x[9];
    uint64_t v[8][2];
    // ---- version 2
    const uint64_t *stack;
    uint64_t nstack;
} Nx2GuestRegs;

// One call of a hooked function of the game, as its hook callback gets it: on the thread that makes the call, before
// the function runs. x and v are the argument registers as the caller set them (as in Nx2GuestRegs); the callback may
// change them. What becomes of the function:
//   - nothing else done: it runs when the callback returns, with x and v as the callback left them;
//   - call_original(call): it runs now, with x and v as they are, and x and v then hold what it returned (x[0], x[1],
//     v[0..]); the callback goes on, may change those results, and the caller gets them when the callback returns;
//   - skip = 1 and no call_original: it does not run, and its caller gets x[0], x[1] and v[0..] as the callback left
//     them.
// The structure is valid only inside the callback that got it.
typedef struct Nx2HookCall {
    uint32_t size;
    uint32_t skip;                // set to 1 to leave the function out
    uint64_t addr;                // the hooked function's guest address
    uint64_t x[9];                // x0-x7, and x8 (the address of a result returned in memory)
    uint64_t v[8][2];             // v0-v7, two 64-bit halves each, low first
    uint64_t sp;                  // the stack pointer at the call: arguments past the registers are the 64-bit words at
                                  // sp, sp + 8, ... (guest memory: read or change them there). Read only.
    uint64_t lr;                  // the guest address the function returns to: who called. Read only.
} Nx2HookCall;

// The drawing calls of one overlay callback. Coordinates are pixels of the window's picture, the origin at its top
// left corner. The structure and its functions are valid only inside the callback that got them, on that thread.
typedef struct Nx2Draw {
    uint32_t size;
    uint32_t menu_open;           // 1 while the game's own settings menu is open (the drawing then shows under it)
    float width, height;          // the picture the overlay covers
    float scale;                  // 1 at a height of 1080 pixels; multiply sizes by it to keep their share of the picture
    float text_height;            // a readable text size in pixels at this scale (the size of the game's own overlay text)
    uint64_t frame;               // as Nx2Frame.index, for the frame under this overlay
    void (*text)(float x, float y, float size, uint32_t rgba, const char *utf8);    // top left corner; '\n' starts a line
    void (*text_size)(float size, const char *utf8, float out_wh[2]);
    void (*rect)(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness);   // an outline
    void (*rect_filled)(float x0, float y0, float x1, float y1, uint32_t rgba, float rounding);
    void (*line)(float x0, float y0, float x1, float y1, uint32_t rgba, float thickness);
} Nx2Draw;

// A key going down or up. key is an NX2_KEY_* value.
typedef struct Nx2KeyEvent {
    uint32_t size;
    uint32_t key;
    uint8_t down;                 // 1 pressed, 0 released
    uint8_t repeat;               // 1 for the repeats of a held key
    uint8_t reserved[6];
} Nx2KeyEvent;

// Key codes: letters and digits are their upper-case ASCII ('A', '7'); the rest are the values below.
enum {
    NX2_KEY_BACKSPACE = 0x08, NX2_KEY_TAB = 0x09, NX2_KEY_ENTER = 0x0D, NX2_KEY_SHIFT = 0x10, NX2_KEY_CTRL = 0x11,
    NX2_KEY_ALT = 0x12, NX2_KEY_ESCAPE = 0x1B, NX2_KEY_SPACE = 0x20, NX2_KEY_PAGE_UP = 0x21, NX2_KEY_PAGE_DOWN = 0x22,
    NX2_KEY_END = 0x23, NX2_KEY_HOME = 0x24, NX2_KEY_LEFT = 0x25, NX2_KEY_UP = 0x26, NX2_KEY_RIGHT = 0x27,
    NX2_KEY_DOWN = 0x28, NX2_KEY_INSERT = 0x2D, NX2_KEY_DELETE = 0x2E,
    NX2_KEY_F1 = 0x70,            // F2 to F12 follow: NX2_KEY_F1 + n - 1
    NX2_KEY_F12 = 0x7B,
};

typedef void (*Nx2FrameFn)(const Nx2Frame *frame, void *user);
typedef void (*Nx2OverlayFn)(const Nx2Draw *draw, void *user);
typedef int (*Nx2KeyFn)(const Nx2KeyEvent *event, void *user);   // return 1 to keep the key from the game's own menu keys
typedef void (*Nx2TextFn)(uint32_t codepoint, void *user);       // one typed character, a Unicode code point
typedef void (*Nx2HookFn)(Nx2HookCall *call, void *user);        // version 2: one call of a hooked function
typedef void (*Nx2ExitFn)(void *user);                           // version 2: the game is about to end
typedef void (*Nx2TitleFn)(void *user);                          // version 2: a title menu button was chosen

// A button for add_title_button: one more entry under the game's own on its title menu. The texts are copied.
typedef struct Nx2TitleButton {
    uint32_t size;
    int32_t order;                // buttons are listed by ascending order, equal ones in the order they were added
    const char *label;            // UTF-8, at most 95 bytes; in the player's language if the plugin has it
    const char *description;      // NULL, or a few words shown smaller after the label (at most 159 bytes)
    Nx2TitleFn on_select;         // called when the player chooses the button
    void *user;
} Nx2TitleButton;

// A button of the game's controls going down or up, for on_pad. button is an NX2_PAD_* value. It is what the game
// would read: the gamepad, and the keys the player bound to its buttons. The left stick pushed past half way counts
// as the direction button of that side, so a menu needs the four directions only.
typedef struct Nx2PadEvent {
    uint32_t size;
    uint32_t button;
    uint8_t down;                 // 1 pressed, 0 released
    uint8_t reserved[7];
} Nx2PadEvent;
enum {                            // the buttons by the names the game's own prompts use
    NX2_PAD_A, NX2_PAD_B, NX2_PAD_X, NX2_PAD_Y, NX2_PAD_STICK_L, NX2_PAD_STICK_R, NX2_PAD_L, NX2_PAD_R, NX2_PAD_ZL,
    NX2_PAD_ZR, NX2_PAD_PLUS, NX2_PAD_MINUS, NX2_PAD_LEFT, NX2_PAD_UP, NX2_PAD_RIGHT, NX2_PAD_DOWN,
};
typedef void (*Nx2PadFn)(const Nx2PadEvent *event, void *user);  // version 2

typedef struct Nx2PluginApi {
    uint32_t size;
    uint32_t version;             // the newest interface version this game serves

    // ---- any thread
    const Nx2GameInfo *(*game_info)(void);
    // A line in the game's log (logs/nx2navi.log next to the executable), prefixed with the calling library's file
    // name. printf formatting; a line longer than about 1900 bytes is cut.
    void (*log)(const char *fmt, ...);
    // Copies n bytes from addr; 1 on success, 0 (and buf untouched or partly written) when part of the range is not
    // readable. No fault on unmapped memory.
    int (*read)(uint64_t addr, void *buf, size_t n);
    // Copies n bytes to addr; 1 on success, 0 with nothing written when part of the range is not memory the game may
    // write (unmapped, or the game's code and constants).
    int (*write)(uint64_t addr, const void *buf, size_t n);

    // ---- callbacks. Each on_* returns a handle above 0, or 0 when it could not register (no free slot, fn NULL).
    // fn(frame, user) once per frame the game presents, on the game's thread, between two frames.
    int (*on_frame)(Nx2FrameFn fn, void *user);
    // fn(draw, user) for every picture the window shows, on the presenting thread.
    int (*on_overlay)(Nx2OverlayFn fn, void *user);
    // fn(event, user) for every key event of the game's window, on the window's thread.
    int (*on_key)(Nx2KeyFn fn, void *user);
    // fn(codepoint, user) for every character typed into the game's window while input is captured (capture_input).
    int (*on_text)(Nx2TextFn fn, void *user);
    // Takes a callback out by its handle. It may still run once more when it was already being called on its thread.
    void (*remove)(int handle);

    // ---- only inside a frame callback or (version 2) a hook callback, on its thread
    // Calls the game's function at the guest address addr with up to 8 integer arguments and returns its x0. Returns 0
    // and writes a log line when called outside those callbacks, with nargs outside 0..8 or for an address that is not
    // the start of a function of the game.
    uint64_t (*guest_call)(uint64_t addr, const uint64_t *args, int nargs);
    // The same with every argument register given and every result register returned in regs; 1 when the call ran.
    int (*guest_call_regs)(uint64_t addr, Nx2GuestRegs *regs);

    // ---- any thread
    // on = 1: the game gets no keyboard, mouse or gamepad input and typed characters go to the on_text callbacks, for a
    // text box of the plugin's own; on = 0 gives the input back once everything is released. Counted per call: every
    // 1 needs its 0.
    void (*capture_input)(int on);

    // ---- version 2: in a game that may be older, check with NX2_HAS(api, Nx2PluginApi, hook_function)
    // Hooks the game's function that starts at the guest address addr: fn(call, user) runs at every call of it, on the
    // calling thread, before the function (Nx2HookCall says what the callback can do). Any thread may register, at any
    // time; the hook holds from the function's next call. Several hooks on one function run in the order they were
    // registered, each one's call_original entering the next. Returns a handle for remove, or 0: fn NULL, no free slot
    // (NX2_HOOKS_MAX), or addr is not the start of a function of the game.
    int (*hook_function)(uint64_t addr, Nx2HookFn fn, void *user);
    // Inside a hook callback, with the call it got: runs the hooked function (through the hooks registered after this
    // one) with call->x and call->v as arguments, stores what it returned in them and returns its x0. It may be called
    // more than once. Returns 0 and does nothing outside the callback the call belongs to.
    uint64_t (*call_original)(Nx2HookCall *call);
    // addr as a pointer when the n bytes there are memory the game's own code reads (writable: and writes) with plain
    // loads and stores: its heap, its mapped stacks, its modules' code, constants and data. The plugin's own code may
    // then read (and write) the range directly, with no call per access, for as long as the game does not unmap it:
    // inside a frame or hook callback, until the callback returns. NULL for anything else, read-only memory asked for
    // writing included: read and write still serve what they can of that. Any thread.
    void *(*map_range)(uint64_t addr, size_t n, int writable);
    // fn(user) once before the game's process ends in an orderly way: the window was closed, the game restarts, or the
    // game itself exits. It runs on the thread that ends the process, while the game's other threads still run and
    // after that thread's last frame callback; guest_call does not work in it. Not called when the game crashes or is
    // killed. Returns a handle or 0, as the other on_* calls.
    int (*on_exit)(Nx2ExitFn fn, void *user);
    // Adds an entry to the game's title menu, under the game's own (continue, new game, settings), with no rebuild of
    // the game. The player reaches it by moving down past the game's last entry, or with the mouse; choosing it calls
    // button->on_select(button->user) on the game's thread, as a frame callback is called (guest_call works in it), with
    // the title menu still showing: open your own menu from it (on_overlay, capture_input, on_key, on_pad). The entry
    // shows only while the title menu's own entries do. Returns a handle for remove, or 0: no label or callback, all
    // NX2_TITLE_BUTTONS_MAX taken, or a game version whose title menu the runtime does not know (docs/plugins.md).
    int (*add_title_button)(const Nx2TitleButton *button);
    // fn(event, user) for every button of the game's controls going down or up, on the game's thread that reads the
    // controls (queue the event for your frame or overlay callback). It also comes while capture_input holds the game's
    // input, which is what a plugin's own menu needs it for. A handle or 0, as the other on_* calls.
    int (*on_pad)(Nx2PadFn fn, void *user);
} Nx2PluginApi;

#define NX2_HOOKS_MAX 64          // function hooks registered at a time, over all plugins
#define NX2_TITLE_BUTTONS_MAX 4   // title menu buttons at a time, over all plugins

// The executable's export NX2_PLUGIN_API_EXPORT.
typedef const Nx2PluginApi *(*Nx2PluginApiFn)(uint32_t version);
// A plugin's optional export NX2_PLUGIN_INIT_EXPORT.
typedef void (*Nx2PluginInitFn)(const Nx2PluginApi *api);

#ifdef __cplusplus
}
#endif

#endif
