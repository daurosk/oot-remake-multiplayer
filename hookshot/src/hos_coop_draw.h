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
    /* Since the game look (hos_coop_draw2 below): the HUD pack's font and heart pictures. Only a module's hos_coop_draw2
       is handed these fields; hos_coop_draw gets the table above them alone (an older Hookshot fills nothing more). */
    int (*font)(void *user, const char *utf8_path);   /* a TTF/OTF file loaded once (the same path again: the same id); its id, 0 on failure */
    void (*use_font)(void *user, int id);             /* text and text_width draw with it from now on; 0: Hookshot's own */
    int (*image)(void *user, int w, int h, const uint8_t *rgba);   /* a picture (w*h*4 bytes, rows top down, straight alpha) copied in; its id, 0 on failure */
    void (*draw_image)(void *user, int id, float x0, float y0, float x1, float y1, uint32_t tint);   /* the whole picture into the rectangle */
} HosCoopDraw;

/* The module's exports (extern "C"), all optional (an older module has none: nothing is drawn). Hookshot calls
   hos_coop_draw2 when the module has it, else hos_coop_draw. */
typedef int (*hos_coop_draw_wanted_fn)(void);             /* 1: something to draw now (Hookshot then makes an ImGui frame) */
typedef void (*hos_coop_draw_fn)(const HosCoopDraw *d);   /* draws it (the table up to text_width) */
typedef void (*hos_coop_draw2_fn)(const HosCoopDraw *d);  /* draws it (the whole table) */
/* The game's camera, for co-op's name tags and edge arrows (optional exports; an older module has none). For each draw
   into the scene's colour target (B10G11R11 with depth), on the thread that records it: hos_coop_camera_wanted says
   whether this frame (Hookshot's frame number) still wants a camera from a target this size (the largest of a frame
   wins); if so, hos_coop_camera gets the draw's vertex-stage uniform buffer 3, its first 0x130 bytes (the game's
   camera block: view-projection at 0x30, the target's size at 0x120), and checks it itself. */
typedef int (*hos_coop_camera_wanted_fn)(uint64_t frame, float width, float height);
typedef void (*hos_coop_camera_fn)(uint64_t frame, const float *block, float width, float height);

#endif
