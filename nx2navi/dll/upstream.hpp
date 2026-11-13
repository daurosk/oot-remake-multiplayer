// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op inside NX2navi the way upstream runs it inside Hookshot (docs/hookshot.md):
// every file of lichii/hook is built unchanged (lichii/hook/core.cmake), with
// hookshot/compat answering the emulator API slice they use, and this file
// answering hookshot/compat's CoopRuntime from NX2navi's plugin interface. So
// chests, shared items and flags, dungeon state, world objects, bombs and
// projectiles, time, drops, riding, PvP and the puppets are upstream's own
// CoopHook, in step with upstream.
//
// What differs from Hookshot is how the hook is driven:
// - CoopHook::OnFrame runs in the interface's frame callback (once per
//   presented frame, loads included: Hookshot's present site);
// - the callers' game calls (CoopCallers::RunFrame) run in a hook of the frame
//   step main+0x3b4e300, taken only for the two calls from moduleCalc_ (the
//   emulator cave's own sites), with guest_call_regs;
// - what the hook would write into main's code padding (the AI hold table, the
//   platform counters' ring pointer) is kept here: NX2navi's main is read-only;
// - the AI hold is this port's own (objects/brain.hpp: a held copy's AI
//   update left out), fed from that same table;
// - three of the hook's modules are answered here instead of built: the
//   emulator's cave (coop_svc.cpp, as on Hookshot), the game side of teleport
//   (coop_teleport_game.cpp: this port has its own, with the join at the host
//   and the follow through exits) and the save transfer (coop_save.cpp: this
//   port's multiplayer saves). Through them the port's own features get the
//   session, each frame's reading and every network event.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "coop/host/player_reader.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "puppet_fx.hpp"
#include "settings.hpp"
#include "step_switch.hpp"

struct Nx2PluginApi;

namespace CoopNx2navi::Up {

struct Hooks {
    // Once per presented frame, inside CoopHook::OnFrame, on the game's
    // presenting thread. `session` is null while not connected.
    void (*frame)(const std::optional<coop::host::PlayerReading>& reading, coop::host::Session* session,
                  void* user) = nullptr;
    // The network thread, under the session's lock.
    void (*event)(const coop::net::Event& event, coop::net::Client& client, void* user) = nullptr;
    void* user = nullptr;
};

// Before anything else: the settings become the hook's switches (environment
// variables, read by the hook's files on first use).
void Configure(const Settings& settings, const std::filesystem::path& mod_folder, void (*log)(const char* line),
               const Hooks& hooks);
// Once the game's interface and main are known: registers the callbacks and
// starts the hook. False: the interface is too old (version 2 is needed).
bool Attach(const Nx2PluginApi* api, std::uint64_t main_base);
bool Attached();
// Chat typed into the box (the hook's own commands, /teleport and /pvp, too).
bool SendChat(const std::string& text);

// A named sound of the player's sound set ("Damage_S_01") emitted by another player's puppet: the
// game's own sound system plays it on the puppet actor's sound user, so it comes from where the puppet
// stands (upstream.cpp, "A puppet's own voice"). Any thread; made at a later frame step. Dropped when
// that player has no live puppet here.
void PuppetSound(std::uint8_t player, const std::string& asset,
                 std::vector<std::pair<std::uint8_t, std::uint32_t>> props = {});
// Floor switches (step_switch.hpp): null when the hook is off (OOT_COOP_SWITCHES=0) or was refused.
StepSwitch::Switches* Switches();
std::uint64_t SwitchCalls();  // calls of the switch selector seen (status line)
// The item-get moment (item_get.hpp): the game's get scene for this item actor beside our Link, or its fanfare.
void AskItemScene(const std::string& actor, const std::array<float, 3>& at);
void AskFanfare(int type);
int TakeItemDone();          // 0 nothing, 1 the scene played, 2 it failed
void NoteEvent(bool event);  // our Link is in an event (each frame)
// A player was struck: their puppet's model turns red for a moment as the game's own Link does
// (puppet_fx.hpp). Any thread.
void PuppetHurt(std::uint8_t player);
// Our own Link's sound user (0 none), told every frame; the names of the sounds it started since the
// last call (a hook on the game's emit), for the other games.
void SetOwnSoundUser(std::uint64_t user);
std::vector<PuppetFx::SoundProps> TakeOwnSounds();  // each with its variation values (may be none)
// Our own Link's life (quarter hearts), each time it is read: at 0 he is dead, and the enemy rules
// that look for a living Link here (objects/brain.hpp, the burrow) do not count him. Any thread.
void NoteOurLife(int life);
bool OurLinkAlive();
std::string StatusText();
void Stop();

}  // namespace CoopNx2navi::Up
