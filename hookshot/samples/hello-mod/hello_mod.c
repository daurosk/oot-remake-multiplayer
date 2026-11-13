/* SPDX-License-Identifier: GPL-3.0-or-later
   hello-mod: the smallest code mod (docs/mods.md "Writing a code mod"). It says hello in the log, counts frames,
   shows its "message" setting in the corner while F7 has it on, and says goodbye at exit. The unit tests load the
   built library through the real loader; the integrator drops it into mods\hello-mod\ to check code mods in game. */
#include <stdio.h>
#include <string.h>

#include "coop_mod_api.h"

static const CoopModApi *api;
static volatile long frames;
static volatile int shown;
static char message[64] = "hello";

static void frame(void *user) {
    (void)user;
    frames = frames + 1;
}

static void overlay(void *user, const CoopModDraw *d) {
    char line[96];
    (void)user;
    if (!shown) return;
    snprintf(line, sizeof line, "%s (%ld frames)", message, (long)frames);
    d->rect(d->user, 8.0f, 8.0f, 16.0f + d->text_width(d->user, 18.0f, line), 34.0f, 0xA0000000u, 4.0f);
    d->text(d->user, 12.0f, 12.0f, 18.0f, 0xFFFFFFFFu, line);
}

static int key(void *user, uint32_t vk, int down, int repeat) {
    (void)user;
    if (vk != 0x76 || !down || repeat) return 0; /* F7 */
    shown = !shown;
    api->log(api, shown ? "shown" : "hidden");
    return 1;
}

static void setting(void *user, const char *k, const char *v) {
    (void)user;
    if (strcmp(k, "message") == 0) {
        snprintf(message, sizeof message, "%s", v);
        api->log(api, "message changed");
    }
}

COOP_MOD_EXPORT int coop_mod_start(const CoopModApi *a) {
    if (a->version < 1 || a->size < sizeof(CoopModApi)) return 0;
    api = a;
    if (api->setting(api, "message", message, sizeof message) < 0) {
        api->log(api, "mod.json has no \"message\" setting");
        return 0;
    }
    api->on_frame(api, frame, 0);
    api->on_overlay(api, overlay, 0);
    api->on_key(api, key, 0);
    api->on_setting(api, setting, 0);
    {
        char line[80];
        snprintf(line, sizeof line, "hello from %s", api->runtime);
        api->log(api, line);
    }
    return 1;
}

COOP_MOD_EXPORT void coop_mod_stop(void) {
    if (api != 0) api->log(api, "goodbye");
}
