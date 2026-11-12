// SPDX-License-Identifier: GPL-3.0-or-later
// hookshot_coop.dll's exports (hos_coop_api.h) and what is behind them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "coop_draw.h"
#include "coop_game_build.h"
#include "hos_coop_api.h"

#if defined(_WIN32)
#define COOP_HOOKSHOT_EXPORT __declspec(dllexport)
#else
#define COOP_HOOKSHOT_EXPORT __attribute__((visibility("default")))
#endif

namespace CoopHookshot {

// hos_coop_start: checks the table and the game build, reads the settings and
// starts lichii/hook's co-op (CoopHook::Start). False: co-op stays off (the
// bridge then calls nothing more).
bool Start(const HosCoopApi* api);
// hos_coop_frame: the game's calc thread, at a frame hook.
void Frame(void* ctx, int site);
// hos_coop_hold: a BSA component's calc bits (the emulator's hold cave's rule).
std::uint32_t Hold(std::uint64_t component, std::uint32_t bits);
// hos_coop_stop: CoopHook::Shutdown.
void Stop();

// The table's size in the first version, before pad_buttons: a Hookshot
// built from an older patch gives this much, and co-op still runs on it.
inline constexpr std::size_t kFirstApiSize = offsetof(HosCoopApi, pad_buttons);

// The table has pad_buttons (player 1's buttons word, Ocarina notes sent).
inline bool HasPadButtons(const HosCoopApi* api) {
    return api != nullptr &&
           api->size >= offsetof(HosCoopApi, pad_buttons) + sizeof(api->pad_buttons) &&
           api->pad_buttons != nullptr;
}

// What a call of hos_coop_frame does. CoopHook::OnFrame runs once per
// present, as the emulator runs it at the guest's present (loads and pause
// included); a Hookshot built from an older patch never calls the present
// site, so until one is seen it runs at the every-frame site. The callers'
// game calls need the game's context: the every-frame site only.
struct FrameWork {
    bool on_frame = false;
    bool calls = false;
};
inline FrameWork WorkAt(int site, bool present_seen) {
    FrameWork w;
    if (site == HOS_COOP_SITE_PRESENT) {
        w.on_frame = true;
    } else if (site == HOS_COOP_SITE_FRAME) {
        w.on_frame = !present_seen;
        w.calls = true;
    }
    return w;
}

// What hos_coop_draw shows now (chat, name tags; the player list while F6
// is held, the panel after F7 or with LICHII_COOP_DEBUG_PANEL=1).
CoopDraw::Scene DrawScene();

// hos_coop_draw2: the scene drawn, in the game look when the HUD pack's font
// and heart pictures load through the table's calls (the first call loads
// them; coop_draw.h, "The game look"). NoGameLook: hos_coop_draw was called
// instead (an older Hookshot), so the plain look for good.
void DrawWithLook(const HosCoopDraw& d);
void NoGameLook();
// The picture's size at this draw (the mouse's fractions are of it).
void NoteDrawn(const HosCoopDraw& d);

// The mouse (hos_coop_mouse): the cheats panel's rows. fx, fy: the cursor as
// a fraction of the window; event: HOS_COOP_MOUSE_*. True when taken.
bool Mouse(float fx, float fy, int event);

// Typing (hos_coop_key / hos_coop_char / hos_coop_keys_taken): the chat box.
bool TypeKey(std::uint32_t vk, bool repeat);
bool TypeChar(std::uint32_t byte);
bool KeysTaken();

// Whether co-op knows the game whose main has this build id (hex, 16 or more
// digits; Hookshot gives 32).
inline bool BuildSupported(const std::string& build_id_hex) {
    return build_id_hex.size() >= 16 && CoopGameBuild::Find(build_id_hex.substr(0, 16)) != nullptr;
}

// Hookshot crashes when the game makes a second Link model (any
// CoopPuppet_<Age>[_<Colour>], plain or coloured; the emulator does not).
// The game's create thread sets ns_tex_mtx0 on the model's Mt_Hairs
// (main+0x4b8240, from main+0x10aceb0, the call at main+0x4b8334) and reads a
// null material pointer: "CRASH ... guest 0x20" in BaseProcCreateAndDeleteThread
// (reports/hs-crash-b.md). Until that is fixed in Hookshot, the module refuses
// those creates: other players show as their Navi only. `allow`:
// LICHII_COOP_HOOKSHOT_LINK_PUPPETS=1, to try them again.
inline bool RefuseCreate(const std::string& actor, bool allow) {
    return !allow && actor.rfind("CoopPuppet_", 0) == 0;
}

}  // namespace CoopHookshot
