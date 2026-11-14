/* SPDX-License-Identifier: GPL-3.0-or-later
   Loads hookshot_coop.so as Longshot's Linux build does (host/coop_bridge.c, our patch 0001: dlopen beside the game,
   then dlsym of each export) and says which of the exports the patched game looks up are there. RTLD_NOW: every
   symbol the module needs must resolve now, as in the game, or the load fails with dlerror's reason. CI runs it
   (.github/workflows/build.yml, linux-longshot) on the module it builds; the game itself needs the player's files.

     load_module <path to hookshot_coop.so>      exit 0: loaded, every export found */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    /* (what patches 0001, 0004, 0005, 0009, 0012 and 0015 look up: GetProcAddress on Windows, dlsym here) */
    static const char *const exports[] = {
        "hos_coop_start", "hos_coop_frame", "hos_coop_hold", "hos_coop_stop", "hos_coop_key", "hos_coop_char",
        "hos_coop_keys_taken", "hos_coop_mouse", "hos_coop_pad", "hos_coop_title", "hos_coop_draw_wanted",
        "hos_coop_draw", "hos_coop_draw2", "hos_coop_camera_wanted", "hos_coop_camera", "hos_coop_world",
        "hos_coop_wrap", "hos_coop_voice_attach", "hos_coop_voice_render", "hos_coop_menu_wanted", "hos_coop_menu_tabs",
        "hos_coop_menu_tab_name", "hos_coop_menu_page", "hos_coop_menu_input",
    };
    if (argc != 2) {
        fprintf(stderr, "usage: load_module <hookshot_coop.so>\n");
        return 2;
    }
    /* (as Longshot's host/posix LoadLibraryA: the full path, RTLD_LOCAL) */
    void *m = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!m) {
        fprintf(stderr, "load_module: %s did not load: %s\n", argv[1], dlerror());
        return 1;
    }
    /* (as the renderer's menu tabs and drawing, patches 0001 and 0012, and the world hooks, 0005: the loaded module by
       its bare name. glibc finds it so only by its SONAME; without one the Multiplayer tab never shows on Linux) */
    void *by_name = dlopen("hookshot_coop.so", RTLD_NOW | RTLD_NOLOAD);
    if (by_name != m) {
        fprintf(stderr, "load_module: dlopen(\"hookshot_coop.so\", RTLD_NOLOAD) %s: the renderer would not find the "
                        "module (its SONAME must be hookshot_coop.so)\n", by_name ? "found another library" : "found nothing");
        return 1;
    }
    printf("found by its bare name (RTLD_NOLOAD), as the renderer looks for it\n");
    int missing = 0;
    for (size_t i = 0; i < sizeof exports / sizeof exports[0]; ++i) {
        void *f = dlsym(m, exports[i]);
        printf("%-24s %s\n", exports[i], f ? "found" : "MISSING");
        missing += f == NULL;
    }
    printf("%s: loaded, %d of %zu exports missing\n", argv[1], missing, sizeof exports / sizeof exports[0]);
    return missing ? 1 : 0;
}
