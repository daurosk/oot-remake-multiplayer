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

// Whether co-op knows the game whose main has this build id (hex, 16 or more
// digits; Hookshot gives 32).
inline bool BuildSupported(const std::string& build_id_hex) {
    return build_id_hex.size() >= 16 && CoopGameBuild::Find(build_id_hex.substr(0, 16)) != nullptr;
}

}  // namespace CoopHookshot
