/* SPDX-License-Identifier: GPL-3.0-or-later
   A picture beside a co-op module tab's page (oot-remake-multiplayer, docs/hookshot.md "The Multiplayer tab"; our patch
   0019): the module may hand Hookshot's menu coloured shapes, drawn in a square over the top of the help column with the
   help box under it, or ask for the square `live`: a clear window onto the game behind the menu (no dim there), the
   shapes over it. The Multiplayer tab's Customize page shows the player's own Link there, the game's camera turning
   around him (hos_coop_view.h), and draws a flat Link when there is none to show.

   One optional export, hos_coop_menu_picture, asked right after hos_coop_menu_page on the same thread for the tab shown;
   the shapes belong to the module and hold until its next call on that thread (Hookshot draws them at once). A module
   without it, or a page that fills none: no picture, the menu as before (hos_coop_menu.h). Generic: any tab may draw a
   picture, with no further Hookshot build.
   The same file is in Hookshot's gpu/vk/ through our patch and in our repository (hookshot/src): keep them equal
   (tests/hookshot/module_test.cpp checks it). Plain C. */
#ifndef HOS_COOP_MENU_PICTURE_H
#define HOS_COOP_MENU_PICTURE_H
#include <stdint.h>

#define HOS_COOP_MENU_PICTURE_VERSION 2
#define HOS_COOP_MENU_SHAPE_POINTS 8
#define HOS_COOP_MENU_MAX_SHAPES 256

/* A convex polygon of 3 to 8 points, x and y from 0 to 1 over the picture's square (y down), filled, or only its outline
   (`outline` 1). Colour 0xRRGGBBAA. Drawn in order: later shapes lie over earlier ones. */
typedef struct HosCoopMenuShape {
    uint32_t rgba;
    uint8_t n;
    uint8_t outline;
    uint8_t pad_[2];
    float xy[2 * HOS_COOP_MENU_SHAPE_POINTS];
} HosCoopMenuShape;

typedef struct HosCoopMenuPicture {
    uint32_t size;                    /* sizeof(HosCoopMenuPicture), set by Hookshot */
    int32_t shapes_n;                 /* at most HOS_COOP_MENU_MAX_SHAPES are drawn */
    const HosCoopMenuShape *shapes;
    /* (version 2) */
    int32_t live;                     /* the module: 1 = the square is a clear window onto the game (shapes may be 0) */
    float window[4];                  /* Hookshot: the square in the window, x0 y0 x1 y1 as fractions of it (y down) */
    float aspect;                     /* Hookshot: the window's width over its height */
} HosCoopMenuPicture;

typedef int (*hos_coop_menu_picture_fn)(int tab, HosCoopMenuPicture *out);   /* 1: filled */

#endif
