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

// The game's camera block from Hookshot (hos_coop_camera: a scene draw's vertex-stage uniform buffer 3, 0x130 bytes):
// the block the emulator's renderer reads for the markers (lichii/hook/coop_camera_track.h): the draw's target size at
// 0x120, a perspective projection's w row ending (-1, 0) at 0xa8. Then the world-to-clip matrix is the 16 floats at
// 0x30 (kCameraMatrix), row-major.
inline constexpr std::size_t kCameraMatrix = 0x30 / sizeof(float);
inline bool CameraBlockOk(const float* block, float width, float height) {
    return block != nullptr && width > 0 && height > 0 && block[0x120 / 4] == width && block[0x124 / 4] == height &&
           block[0xa8 / 4] == -1.0f && block[0xac / 4] == 0.0f;
}

// The table has key_down (keys held, read on Linux: F6, F7 and the closing
// keys' release; Windows asks its own GetAsyncKeyState).
inline bool HasKeyDown(const HosCoopApi* api) {
    return api != nullptr && api->size >= offsetof(HosCoopApi, key_down) + sizeof(api->key_down) &&
           api->key_down != nullptr;
}

// The table has pad_buttons (player 1's buttons word, Ocarina notes sent).
inline bool HasPadButtons(const HosCoopApi* api) {
    return api != nullptr &&
           api->size >= offsetof(HosCoopApi, pad_buttons) + sizeof(api->pad_buttons) &&
           api->pad_buttons != nullptr;
}

// The title's Multiplayer menu can keep the module loaded with no server in
// coop.ini: the table has patch 0004's scratch (its title hooks and pad are
// there too) and LICHII_COOP_TITLE_MENU is not 0.
inline bool TitleMenuPossible(const HosCoopApi* api, const std::string& env) {
    return api != nullptr && api->size >= offsetof(HosCoopApi, scratch) + sizeof(api->scratch) &&
           api->scratch != nullptr && env != "0";
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
// A co-op puppet is a stripped copy of Link. In its create, main+0x4b8240
// (the hair's setup) reads [[actor+0x270]+0x50]+0x20: component slot 10,
// which the puppet does not have, and more such null reads follow in its
// update. The emulator reads them as 0; Hookshot crashed ("CRASH ... guest
// 0x20" in BaseProcCreateAndDeleteThread, reports/hs-puppets-b.md). Our
// Hookshot patch 0002 (nx2/coop_puppet_guard.py, host/coop_null_page.c) makes
// them read 0 there too. AllowLinkPuppets: LICHII_COOP_HOOKSHOT_LINK_PUPPETS=1
// / 0 force it; unset, Link puppets are made when the guard is in this
// hookshot.exe (always on Linux, where the guest window is readable whole).
inline bool AllowLinkPuppets(const std::string& env, bool guard_in_exe) {
    if (!env.empty()) {
        return env != "0";
    }
    return guard_in_exe;
}

// Refused while not allowed: other players then show as their Navi only.
inline bool RefuseCreate(const std::string& actor, bool allow) {
    return !allow && actor.rfind("CoopPuppet_", 0) == 0;
}

}  // namespace CoopHookshot
