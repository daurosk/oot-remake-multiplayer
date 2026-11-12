/* SPDX-License-Identifier: GPL-3.0-or-later
   The co-op module's drawing over the picture (oot-remake-multiplayer, docs/hookshot.md).

   Hookshot's renderer (gpu/vk/vk_coop.cpp, from our patch) finds hookshot_coop.dll's two exports below once the bridge
   has loaded it, and calls them inside its Dear ImGui frame (vk_menu.cpp menu_record), on the thread that records the
   present. The same file is in Hookshot's gpu/vk/ through our patch and in our repository (hookshot/src): keep them
   equal (tests/hookshot/module_test.cpp checks it). Plain C, so both sides' compilers agree on the layout.
   Coordinates: pixels of the window's image from its top left. Colours: Dear ImGui's IM_COL32 (red in the low byte,
   alpha in the high one). */
#ifndef HOS_COOP_DRAW_H
#define HOS_COOP_DRAW_H
#include <stdint.h>

typedef struct HosCoopDraw {
    float width, height;   /* the window's image */
    void *user;            /* Hookshot's, passed back */
    void (*rect)(void *user, float x0, float y0, float x1, float y1, uint32_t rgba, float rounding);   /* filled */
    void (*circle)(void *user, float x, float y, float radius, uint32_t rgba, float thickness);   /* thickness 0: filled */
    void (*triangle)(void *user, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgba);   /* filled */
    void (*text)(void *user, float x, float y, float px, uint32_t rgba, const char *utf8);   /* x, y: the top left */
    float (*text_width)(void *user, float px, const char *utf8);
} HosCoopDraw;

/* The module's exports (extern "C"), both optional (an older module has neither: nothing is drawn). */
typedef int (*hos_coop_draw_wanted_fn)(void);             /* 1: something to draw now (Hookshot then makes an ImGui frame) */
typedef void (*hos_coop_draw_fn)(const HosCoopDraw *d);   /* draws it */

#endif
