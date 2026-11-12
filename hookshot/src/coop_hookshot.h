// SPDX-License-Identifier: GPL-3.0-or-later
// hookshot_coop.dll's exports (hos_coop_api.h) and what is behind them.
#pragma once

#include <cstdint>
#include <string>

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
