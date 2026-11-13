/* SPDX-License-Identifier: GPL-3.0-or-later
   The co-op module's tabs in Hookshot's own menu (oot-remake-multiplayer, docs/hookshot.md "The Multiplayer tab").

   Hookshot's menu (gpu/vk/vk_menu.cpp, from our patch 0012) asks hookshot_coop.dll for the tabs below once the bridge
   has loaded it, shows them FIRST in its tab strip (before Graphics), draws the shown one's page with its own cells
   and help box, and hands the player's presses on that tab to the module. The module says what the rows are and what
   a press does; Hookshot only draws and forwards. The exports: hos_coop_menu_tabs, hos_coop_menu_tab_name,
   hos_coop_menu_page, hos_coop_menu_input (all four, or no tabs) and the optional hos_coop_menu_wanted. A module
   without them: no tabs, Hookshot as it was.
   Generic: any number of tabs (Multiplayer, then Mods), with no further Hookshot build for a new tab or row.
   The same file is in Hookshot's gpu/vk/ through our patch and in our repository (hookshot/src): keep them equal
   (tests/hookshot/module_test.cpp checks it). Plain C, so both sides' compilers agree on the layout.

   Threads: Hookshot calls these from its menu tick and from the thread that records the present; the module guards
   itself. hos_coop_menu_page's strings belong to the module and hold until the next hos_coop_menu_page call on the
   same thread: Hookshot copies them at once. Text is UTF-8.

   Keyboard: while a page says `typing`, the module takes the keys itself through hos_coop_key / hos_coop_char
   (host/display.c offers them to it first, patch 0001), so Q, E and Tab type letters there instead of changing tabs.
   The pad's buttons still come here as events. */
#ifndef HOS_COOP_MENU_H
#define HOS_COOP_MENU_H
#include <stdint.h>

#define HOS_COOP_MENU_VERSION 1

/* A row's kind (HosCoopMenuRow.kind) */
enum {
    HOS_COOP_MENU_ACTION = 0,    /* a name and a value cell with no arrows: Confirm does it */
    HOS_COOP_MENU_CHOICE = 1,    /* a value with arrows (left / right: it can go that way): Left and Right change it */
    HOS_COOP_MENU_TOGGLE = 2,    /* Off / On halves (on) */
    HOS_COOP_MENU_HEADING = 3,   /* a section's title: not selectable */
    HOS_COOP_MENU_NOTE = 4       /* a line of text over the row's width: not selectable */
};

typedef struct HosCoopMenuRow {
    const char *name;
    const char *value;   /* ACTION, CHOICE: the value cell's text ("" none) */
    uint8_t kind;        /* HOS_COOP_MENU_* above */
    uint8_t on;          /* TOGGLE */
    uint8_t left, right; /* CHOICE: the arrows */
    uint8_t fixed;       /* it cannot be used now (drawn dim; a press says why in the help) */
    uint8_t pad_[3];
} HosCoopMenuRow;

typedef struct HosCoopMenuPage {
    uint32_t size;                 /* sizeof(HosCoopMenuPage), set by Hookshot */
    int32_t rows_n;
    const HosCoopMenuRow *rows;
    int32_t selected;              /* -1: none */
    int32_t typing;                /* the keyboard types into the selected row */
    const char *help_title;        /* the help box: its title, its paragraph, a line set off under it, one gold line */
    const char *help;
    const char *hint;
    const char *gold;
    const char *change;            /* the prompt strip's words for Left/Right, Confirm and Back ("" or NULL: none) */
    const char *confirm;
    const char *back;
} HosCoopMenuPage;

/* An event for hos_coop_menu_input */
enum {
    HOS_COOP_MENU_UP = 0,
    HOS_COOP_MENU_DOWN = 1,
    HOS_COOP_MENU_LEFT = 2,
    HOS_COOP_MENU_RIGHT = 3,
    HOS_COOP_MENU_CONFIRM = 4,
    HOS_COOP_MENU_BACK = 5,      /* returns 0 when the tab has nothing to go back to: Hookshot closes the menu */
    HOS_COOP_MENU_SHOW = 6,      /* the tab came to show (the menu opened on it, or the player went to it) */
    HOS_COOP_MENU_HIDE = 7,      /* it shows no more (another tab, or the menu closed) */
    HOS_COOP_MENU_SELECT = 8,    /* the mouse over row `arg` */
    HOS_COOP_MENU_CLICK = 9      /* a left click on row `arg`: arg2 -1 the value's left half, 1 its right half, 0 the name */
};

typedef int (*hos_coop_menu_tabs_fn)(void);                               /* how many tabs (0: none) */
typedef const char *(*hos_coop_menu_tab_name_fn)(int tab);                /* the strip's name, tab 0 first */
typedef int (*hos_coop_menu_page_fn)(int tab, HosCoopMenuPage *out);      /* 1: filled */
typedef int (*hos_coop_menu_input_fn)(int tab, int event, int arg, int arg2);   /* 1: taken */
/* Optional, asked at each menu tick: what the module wants of the menu now (once: the next call says the next thing).
   HOS_COOP_MENU_WANT_NOTHING, HOS_COOP_MENU_WANT_CLOSE (a join connected at the title: the game takes the pad again),
   or a tab (0 and up): the menu opens on it (the title screen's Host, Join and Co-op Settings rows). */
#define HOS_COOP_MENU_WANT_NOTHING (-1)
#define HOS_COOP_MENU_WANT_CLOSE (-2)
typedef int (*hos_coop_menu_wanted_fn)(void);

#endif
