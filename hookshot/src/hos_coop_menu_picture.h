/* SPDX-License-Identifier: GPL-3.0-or-later
   A picture beside a co-op module tab's page (oot-remake-multiplayer, docs/hookshot.md "The Multiplayer tab"; our patch
   0019): the module may hand Hookshot's menu coloured shapes, drawn in a square over the top of the help column with the
   help box under it. The Multiplayer tab's Customize page draws Link in the colours chosen with it.

   One optional export, hos_coop_menu_picture, asked right after hos_coop_menu_page on the same thread for the tab shown;
   the shapes belong to the module and hold until its next call on that thread (Hookshot draws them at once). A module
   without it, or a page that fills none: no picture, the menu as before (hos_coop_menu.h). Generic: any tab may draw a
   picture, with no further Hookshot build.
   The same file is in Hookshot's gpu/vk/ through our patch and in our repository (hookshot/src): keep them equal
   (tests/hookshot/module_test.cpp checks it). Plain C. */
#ifndef HOS_COOP_MENU_PICTURE_H
#define HOS_COOP_MENU_PICTURE_H
#include <stdint.h>

#define HOS_COOP_MENU_PICTURE_VERSION 1
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
} HosCoopMenuPicture;

typedef int (*hos_coop_menu_picture_fn)(int tab, HosCoopMenuPicture *out);   /* 1: filled */

#endif
