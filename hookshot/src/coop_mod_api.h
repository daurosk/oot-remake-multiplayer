/* SPDX-License-Identifier: GPL-3.0-or-later
   The plugin interface for code mods (oot-remake-multiplayer, docs/mods.md "Writing a code mod").

   A code mod is a library (a .dll on Windows) named by "code" in its mod.json. The co-op module (hookshot_coop.dll)
   loads it once, when the game starts, first row of the Mods tab first, and calls its export coop_mod_start with this
   table. Everything a mod does goes through the table: no Hookshot or co-op header is needed, so a mod built against
   this file works on any game runtime that serves it (Hookshot today; written to port to Longshot).

   Rules:
   - Register callbacks only inside coop_mod_start. Each register call answers 1 when taken, 0 when this runtime does
     not have it (a mod should then say so in the log and carry on without it).
   - Callbacks run on the game's threads (said at each one): be quick, never wait on a lock the game holds, never
     keep a pointer the call handed you past the call.
   - A crash inside a mod's call (an access violation, a C++ exception) is caught: that mod is switched off for this
     run with one line in hookshot_log.txt and the game goes on. Its library stays loaded (its code may still be on a
     thread's stack).
   - Strings are UTF-8 with a 0 at the end.

   Versions: COOP_MOD_API_VERSION rises by one when the table grows; old entries never move or change. A mod reads
   `version` and `size` before using a newer entry. mod.json "api" says the version the mod was written for: the
   loader refuses a mod that wants a newer one than it serves. Plain C, header only.
   - Version 1: the log, memory, settings, frame, overlay, key, setting and hook callbacks.
   - Version 2: the game's menu screens (on_screen) and three things a mod may do inside that call: choose the focused
     button, have the game read its save list again, hold the game's input. They are said in the game's own terms
     (screens, buttons, the save list), never as addresses: the runtime finds the game's functions itself.
   - Version 3: inside on_screen, start one of the game's own menu sounds (play_sound); and the screen says whether its
     focused button takes a choice now (CoopModScreen's focused_ready). A mod that uses either says "api": 3, so an
     older co-op refuses it cleanly instead of it reading past that co-op's table. */
#ifndef COOP_MOD_API_H
#define COOP_MOD_API_H
#include <stddef.h>
#include <stdint.h>

#define COOP_MOD_API_VERSION 3u

#ifdef _WIN32
#define COOP_MOD_EXPORT __declspec(dllexport)
#else
#define COOP_MOD_EXPORT __attribute__((visibility("default")))
#endif

/* Drawing over the picture (the overlay callback). Coordinates in pixels of the window's image, (0, 0) top left;
   colours 0xAABBGGRR (as ImGui). Only valid during the callback. */
typedef struct CoopModDraw {
    float width, height;
    void *user;   /* pass it back to each call */
    void (*rect)(void *user, float x0, float y0, float x1, float y1, uint32_t rgba, float rounding);
    void (*text)(void *user, float x, float y, float px, uint32_t rgba, const char *utf8);
    float (*text_width)(void *user, float px, const char *utf8);
} CoopModDraw;

/* Function hooks: a hook id names a game function of the runtime's hook list (on Hookshot the wrap ids of
   hos_coop_world.h, HOS_COOP_WRAP_*). phase 0 runs before the game's function, phase 1 after it. x = the guest
   registers x0-x31 (x[31] is SP), read and written in place: the arguments at phase 0, x0 the result at phase 1.
   Phase 0 returning 1 means the mod did the function's work and the game's function is not run (phase 1 still comes);
   return 0 to let it run. The thread is the one that called the function. */
typedef int (*coop_mod_hook_fn)(void *user, uint32_t hook, int phase, uint64_t *x);
typedef void (*coop_mod_frame_fn)(void *user);                              /* the game's calc thread, each frame */
typedef void (*coop_mod_overlay_fn)(void *user, const CoopModDraw *draw);  /* the render thread, each frame */
/* A key (a Windows virtual-key code) went down (down = 1, repeat = 1 for a held key) or up (down = 0; only where the
   runtime tells key-ups: Hookshot tells downs only). Return 1 to take it: later mods and the game do not see it. Co-op's
   own boxes (chat, menus) get a key first. The window's thread. */
typedef int (*coop_mod_key_fn)(void *user, uint32_t vk, int down, int repeat);
/* One of the mod's settings (mod.json "settings") changed in the Mods tab: its key and the new value. A setting with
   "restart" is told too; it only counts after a restart. The thread that made the change. */
typedef void (*coop_mod_setting_fn)(void *user, const char *key, const char *value);

/* (version 2) The game's menu screens, as the runtime names them. A screen the runtime does not name is OTHER. */
enum {
    COOP_MOD_SCREEN_OTHER = 0,
    COOP_MOD_SCREEN_TITLE = 1,          /* the title: Continue, New Game, Options */
    COOP_MOD_SCREEN_LOAD_LIST = 2,      /* the list of saves to load (the title's Continue, the System page's Load) */
    COOP_MOD_SCREEN_LOAD_QUESTION = 3   /* its question after a save is picked */
};
/* (version 2) One menu screen's update, as on_screen hands it over. Only valid during the call. */
typedef struct CoopModScreen {
    uint32_t size;          /* sizeof this struct as the runtime knows it */
    uint32_t kind;          /* COOP_MOD_SCREEN_* */
    const char *focused;    /* the focused button's name in the screen's layout ("Pa_Button_00": the title's Continue),
                               "" when no button is focused or the screen takes no input now */
    int save_list_ready;    /* 1 when the game's save list is read and no new read is asked or running */
    uint64_t buttons;       /* player 1's buttons now (Switch NpadButton bits: A 1<<0, B 1<<1, X 1<<2, Y 1<<3, + 1<<10,
                               - 1<<11, the d-pad 1<<12 to 1<<15), as the game would get them before any mod's hold: a
                               pad, the keyboard's bindings and a click on the game's menus alike */
    int focused_ready;      /* (version 3) 1 when the focused button takes a choice now: its screen is open and not
                               still coming in (the title has Continue focused while its opening scene plays and its
                               menu fades in: 0 then, and the game's own A goes to the title, not to Continue). Read it
                               only when the table's version is 3 or more (or this struct's size covers it). */
} CoopModScreen;
/* (version 2) hold_input's buttons: all of them and the sticks too */
#define COOP_MOD_HOLD_ALL (~(uint64_t)0)
/* (version 2) A menu screen updates: on the game's thread that updates it, before its own update, for every menu screen
   (the HUD is one too, as OTHER), one at a time across all mods. choose_focused, reload_save_list and hold_input work
   only inside this call. */
typedef void (*coop_mod_screen_fn)(void *user, const CoopModScreen *screen);

typedef struct CoopModApi {
    uint32_t size;      /* sizeof this table as the loader knows it */
    uint32_t version;   /* COOP_MOD_API_VERSION of the loader */
    const char *mod_id; /* this mod's id (mod.json "id") */
    const char *mod_folder;   /* this mod's folder, a full path */
    const char *runtime;      /* "hookshot" (later "longshot") */

    /* The calls below that take `self` want this very table (each mod has its own). */

    /* One line into the game's log, as "coop: mod <id>: <line>". Any thread. */
    void (*log)(const struct CoopModApi *self, const char *line);

    /* The game's memory (guest addresses). 1 on success; 0 when a byte of the range is not mapped (nothing done). The
       game runs at the same time: a value read can be stale at once. Any thread. */
    uint64_t (*main_base)(void);   /* the game's main module: offsets in the docs are from here */
    int (*read)(uint64_t address, void *out, size_t size);
    int (*write)(uint64_t address, const void *in, size_t size);

    /* A setting's value now (mod.json "settings"): its length, and as much as fits into out with a 0 (cap > 0). -1 when
       the mod has no such setting. In a co-op game a "shared" setting is the host's value. Any thread. */
    int (*setting)(const struct CoopModApi *self, const char *key, char *out, size_t cap);

    /* Registration, only inside coop_mod_start. `user` is handed back to the callback. */
    int (*on_frame)(const struct CoopModApi *self, coop_mod_frame_fn fn, void *user);
    int (*on_overlay)(const struct CoopModApi *self, coop_mod_overlay_fn fn, void *user);
    int (*on_key)(const struct CoopModApi *self, coop_mod_key_fn fn, void *user);
    int (*on_setting)(const struct CoopModApi *self, coop_mod_setting_fn fn, void *user);
    /* 0 also for a hook this run of the game does not have (has_hook) */
    int (*hook)(const struct CoopModApi *self, uint32_t hook, coop_mod_hook_fn fn, void *user);

    /* Whether this run of the game has hook `hook` (the exe was built with it). Any thread, after coop_mod_start too. */
    int (*has_hook)(uint32_t hook);

    /* The mod changed something only a new start shows (it wrote game files): the Mods tab says "restart needed" with
       the reason. Any thread. */
    void (*restart_needed)(const struct CoopModApi *self, const char *reason);

    /* ---- Version 2: read `version` >= 2 (or `size`) before using these. ---- */

    /* Registration, only inside coop_mod_start, as the others. */
    int (*on_screen)(const struct CoopModApi *self, coop_mod_screen_fn fn, void *user);

    /* The three calls below work only inside this mod's own on_screen call, on its thread, for the screen it was handed:
       anywhere else they do nothing and answer 0 (with a line in the log, once). */

    /* Chooses the screen's focused button the game's own way, as the player's A would (its press, its sound, what it
       opens). 1 when the game took it; 0 when nothing is focused, the button cannot be chosen now (its opening animation,
       another choice running: try at a later update), or co-op is busy: hosting, joining, in a session or the host's
       save on its way (on every screen, not only the title). */
    int (*choose_focused)(const struct CoopModApi *self);

    /* Has the game read its save list (the files' captions the title and the load list show) again from the save
       folder, after the mod changed the files in it. The game reads it at the title's next update; save_list_ready is 0
       until it is done. 1 when asked; 0 when the title is not up (the game reads its list only there: on the pause
       menu's System > Load, ask again once the title shows). */
    int (*reload_save_list)(const struct CoopModApi *self);

    /* The buttons the game does not get from player 1 while held (NpadButton bits, as CoopModScreen's `buttons`):
       COOP_MOD_HOLD_ALL for all of them and the sticks (the mod's own screen is in front), a few to take one choice
       (A on a focused button: the mod sees the press in `buttons` and acts itself), 0 to give everything back. Each call
       replaces this mod's last and ties it to the screen it is made on; the mods' holds add up. The hold ends by itself
       when that screen has not updated for half a second (it closed; other screens such as the HUD do not keep it) or
       updates as another kind of screen, and when the mod is switched off. A mod keeps a hold across screens by
       calling hold_input again on the next one. 1 when done. */
    int (*hold_input)(const struct CoopModApi *self, uint64_t buttons);

    /* ---- Version 3: read `version` >= 3 (or `size`) before using these. ---- */

    /* One of the game's own menu sounds, on the screen being updated, as the game's screens start theirs: `name` is a
       sound of that screen's set ("button_on" a cursor step, "button_decide" a choice, "Pa_Button_00_decide" the
       title's Continue, "mc_TabToRight" the pause menu's tabs), letters, digits and _ only, at most 63. Only inside
       on_screen, as the version 2 calls; 1 when the game found the sound and started it. */
    int (*play_sound)(const struct CoopModApi *self, const char *name);
} CoopModApi;

/* The mod's exports. coop_mod_start: once, when the co-op module starts, before any callback; return 1 to stay on,
   0 to be switched off (say why in the log first). The table lives until the game ends. coop_mod_stop (optional): once
   at exit, after the last callback. */
typedef int (*coop_mod_start_fn)(const CoopModApi *api);
typedef void (*coop_mod_stop_fn)(void);
#define COOP_MOD_START_EXPORT "coop_mod_start"
#define COOP_MOD_STOP_EXPORT "coop_mod_stop"

#endif
