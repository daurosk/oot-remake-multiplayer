/* (oot-remake-multiplayer's patch 0028, player report 133) Which button Longshot's Skip title screen may choose, and when.
   The same file is the patch's host/hos_skip_title.h and the module repo's hookshot/src/hos_skip_title.h (unit tests:
   tests/hookshot/skip_title_test.cpp; tests/hookshot/module_test.cpp checks the two are one).

   Skip title screen goes the way the player's A would go to load the newest save, and no other: three screens, in order,
   each known by its class's vtable (read from main: each class's name getter, then its factory's constructor):
     0. the title, UITitleScreen (main+0x4bf06e8): its Continue, Pa_Button_02 (OpenTitle.module.ainb gives that pane
        LayoutMsg/Title_00's label 0001, "Continue"; 0000 "New Game" is Pa_Button_00, 0002 "Options" Pa_Button_01);
     1. the load list, UISystemLoadListScreen (main+0x4beeba0): the file it focuses as it opens, a Pa_ pane;
     2. its question, UISystemSaveLoadScreen (main+0x4bef840): Pa_CheckButton_00, its first answer.
   A screen of an earlier step still updating (the title behind the list) is waited out. Any other screen with a focused
   button, a known one out of order, or a known one focused on another button stops the skip for good: the rest is the
   player's. At most three choices. */
#ifndef HOS_SKIP_TITLE_H
#define HOS_SKIP_TITLE_H
#include <stdint.h>
#include <string.h>

#define HOS_SKIP_TITLE_SCREEN 0x84c106e8ULL       /* UITitleScreen (guest address: main at 0x80020000) */
#define HOS_SKIP_LOAD_LIST_SCREEN 0x84c0eba0ULL   /* UISystemLoadListScreen */
#define HOS_SKIP_LOAD_CHECK_SCREEN 0x84c0f840ULL  /* UISystemSaveLoadScreen */
#define HOS_SKIP_STEPS 3

enum { HOS_SKIP_WAIT = 0, HOS_SKIP_CHOOSE = 1, HOS_SKIP_STOP = 2 };

/* The step a screen's vtable is (0..2), -1 for any other screen */
static inline int hos_skip_title_step_of(uint64_t vtable) {
    return vtable == HOS_SKIP_TITLE_SCREEN ? 0 : vtable == HOS_SKIP_LOAD_LIST_SCREEN ? 1 : vtable == HOS_SKIP_LOAD_CHECK_SCREEN ? 2 : -1;
}

/* step: the choices made so far. vtable: a screen that has one focused button; name: that button's pane name ("" when it
   could not be read). HOS_SKIP_CHOOSE, HOS_SKIP_WAIT, or HOS_SKIP_STOP with *why (a static line for the log) */
static inline int hos_skip_title_next(int step, uint64_t vtable, const char *name, const char **why) {
    const int at = hos_skip_title_step_of(vtable);
    if (why) *why = "";
    if (!name) name = "";
    if (step >= HOS_SKIP_STEPS) {
        if (why) *why = "made its three choices";
        return HOS_SKIP_STOP;
    }
    if (at < 0) {
        if (why) *why = "a screen that is not the title, the load list or its question";
        return HOS_SKIP_STOP;
    }
    if (at < step) return HOS_SKIP_WAIT;   /* (an earlier step's screen, still updating behind the next one) */
    if (at > step) {
        if (why) *why = "a screen out of order";
        return HOS_SKIP_STOP;
    }
    if (at == 0 && strcmp(name, "Pa_Button_02") != 0) {
        if (why) *why = "the title's focus is not on Continue (Pa_Button_02)";
        return HOS_SKIP_STOP;
    }
    if (at == 1 && strncmp(name, "Pa_", 3) != 0) {
        if (why) *why = "the load list's focus is not on a file";
        return HOS_SKIP_STOP;
    }
    if (at == 2 && strcmp(name, "Pa_CheckButton_00") != 0) {
        if (why) *why = "the question's focus is not on its first answer (Pa_CheckButton_00)";
        return HOS_SKIP_STOP;
    }
    return HOS_SKIP_CHOOSE;
}
#endif
