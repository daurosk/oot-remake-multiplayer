// SPDX-License-Identifier: GPL-3.0-or-later
// hookshot_coop.dll's exports (hos_coop_api.h) and what is behind them.
#pragma once

#include <cstdint>
#include <string>

#include "hos_coop_api.h"

#if defined(_WIN32)
#define COOP_HOOKSHOT_EXPORT __declspec(dllexport)
#else
#define COOP_HOOKSHOT_EXPORT __attribute__((visibility("default")))
#endif

namespace CoopHookshot {

// hos_coop_start: checks the table and the game build, reads the settings and
// connects. False: co-op stays off (the bridge then calls nothing more).
bool Start(const HosCoopApi* api);
// hos_coop_frame: the game's calc thread, at a frame hook.
void Frame(void* ctx, int site);
// hos_coop_hold: a BSA component's calc bits (unchanged in this build).
std::uint32_t Hold(std::uint64_t component, std::uint32_t bits);
// hos_coop_stop: says goodbye and joins the network thread.
void Stop();

// Whether co-op knows the game whose main has this build id (hex, 16 or more
// digits; Hookshot gives 32).
bool BuildSupported(const std::string& build_id_hex);

}  // namespace CoopHookshot
