// The mod loader inside hookshot_coop.dll (docs/mods.md): the one Loader (the
// Mods tab's rows) and the one plugin Host, started with the module, driven
// from its exports. coop_hookshot.cpp calls these and nothing else of the
// loader.
#pragma once

#include <cstdint>
#include <functional>

#include "coop_mods.h"
#include "coop_rando.h"
#include "hos_coop_api.h"
#include "hos_coop_draw.h"

namespace CoopModsRuntime {

// Once, when the module stays loaded (hos_coop_start answers 1): reads the mods folder (HOS_MODS, else "mods" beside
// hookshot.exe) and logs one line per mod. The code mods start at the first Frame (by then the exe has told its hooks).
// HOS_COOP_MODS_CODE=0: no code mod is loaded (game files and the tab still work).
void Start();
void Stop();

// nullptr before Start.
CoopMods::Loader* Loader();
CoopRando::Rando* Rando();  // the built-in randomizer: its status line for the tab

// The module's exports' parts. Each is cheap when no code mod registered for it.
void Frame();                                   // the calc thread, every frame (HOS_COOP_SITE_FRAME)
bool Key(std::uint32_t vk, bool repeat);        // the window's thread: true when a mod took it (down only on Hookshot)
bool OverlayWanted();
void Overlay(const HosCoopDraw& d);             // the render thread, after co-op's own drawing
// Hooks (hos_coop_world.h wraps, once patch 0005 is in): the exe's hooks as bits (HOS_COOP_WORLD_HELLO's b), then each
// wrapped call.
void SetHooks(std::uint64_t bits);
int Hook(std::uint32_t id, int phase, std::uint64_t* x);
bool Hooked(std::uint32_t id);

// (mod API v2, docs/mods.md "Menu screens") The game side, once at the module's start: its table (the game calls) and
// co-op's own answer whether a mod may choose a button now (nullptr: free; else why not: hosting, joining, the host's
// save on its way, as Skip title screen's hold).
void SetGame(const HosCoopApi* api, std::function<const char*()> coop_busy);
// Patch 0030's call for every menu screen's update (the game's thread; ctx may call()). Nothing is read when no mod
// asked for screens.
void Screen(void* ctx, std::uint64_t screen);
// hos_coop_pad: player 1's buttons as our pause page passed them on (the mods' screens see them), then the buttons the
// mods hold from the game now (COOP_MOD_HOLD_ALL: the sticks too), 0 none.
void NotePad(std::uint64_t buttons);
std::uint64_t HeldButtons();
// (mod API v2) Whether a save screen's file select runs now: a code mod whose mod.json says "file_select": true and
// that asked for the game's menu screens (the title's own menu then has one Start row, CoopTitleMenu::MenuFor). Any
// other menu-screen mod is not one. Takes no lock of co-op's.
bool FileSelectRuns();

}  // namespace CoopModsRuntime
