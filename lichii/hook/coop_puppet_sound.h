// SPDX-License-Identifier: GPL-3.0-or-later
//
// Sounds on a puppet (issues 152 and 131, docs/puppet.md "Sound" and
// "Ocarina"). No emulator code here: the puppet director (coop_puppet.h)
// asks this for its calls and passes the results back.
//
// How the game plays a named sound on an actor, read from the AI node
// OneShotXLinkSearchAndEmit (run main + 0x295b740) and xlink2's own code:
// - The actor's XLink component is [[actor + 0x270] + 0x68]. It holds two
//   xlink2 user instances: +0x70 the effects (ELink) user, +0x78 the sound
//   (SLink) user. Each user's u32 at +0x18 has bit 0x1000 set for ELink and
//   clear for SLink (0x37416c0 and 0x4409e0 pick the system by it).
// - A user's resource is [user + 0x58]: +0x10 the user's name (char*, the
//   ActorInfo SLinkUserName, "Player_Child"), +0x18 its asset accessor.
// - 0x43db00(accessor, &asset, name) finds the named asset (8 bytes out) and
//   returns true; 0x440630(user, &asset, &handle, 0) plays it and fills the
//   8-byte handle (u8 0xff, u16 0xffff at +2 before: the node's own init).
//   This is xlink2's searchAndEmit, the same code for both systems.
// - 0x7cc120(&handle) stops what a handle plays (a stale handle is checked
//   and left alone): the fade path of OneShotXLinkFade.
// - What a user plays now: a list at user + 0x28 (head), first link at
//   +0x30, the event at link - s32 [user + 0x3c], its asset's name at
//   [[event + 0x30]], the next link at [link + 8] (0x37416c0's walk).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace CoopPuppet {
class Guest;
struct Call;
}

namespace CoopPuppetSound {

namespace addr {
inline constexpr std::uint64_t kActorParts = 0x270;
inline constexpr std::uint64_t kPartsXLink = 0x68;
inline constexpr std::uint64_t kXLinkEffectUser = 0x70;
inline constexpr std::uint64_t kXLinkSoundUser = 0x78;
inline constexpr std::uint64_t kUserFlags = 0x18;
inline constexpr std::uint32_t kUserFlagEffect = 0x1000;
inline constexpr std::uint64_t kUserResource = 0x58;
inline constexpr std::uint64_t kResourceName = 0x10;
inline constexpr std::uint64_t kResourceAccessor = 0x18;
inline constexpr std::uint64_t kUserEvents = 0x28;       // list head
inline constexpr std::uint64_t kUserFirstLink = 0x30;
inline constexpr std::uint64_t kUserLinkOffset = 0x3c;   // s32
inline constexpr std::uint64_t kLinkNext = 0x08;
inline constexpr std::uint64_t kEventAsset = 0x30;       // -> {char* name, ...}
inline constexpr std::uint64_t kSearchAsset = 0x43db00;  // (accessor, &asset, name) -> bool
inline constexpr std::uint64_t kEmit = 0x440630;         // (user, &asset, &handle, 0)
inline constexpr std::uint64_t kKill = 0x7cc120;         // (&handle)
inline constexpr std::uint64_t kSoundRootGot = 0x4c7e678;
inline constexpr std::uint64_t kOcarinaModuleVtableGot = 0x4c8bfd0;  // + 0x10 = vtable
inline constexpr std::uint64_t kModuleUser = 0x40;  // 0xd1f360 stores the made user here
inline constexpr std::uint64_t kUserModuleBack = 0x138;  // ... and module + 0x38 here
inline constexpr std::uint64_t kRootSLinkSystem = 0x18;
}  // namespace addr

// Room in the puppet's block (CoopPuppet::block, after the gear pointers).
namespace block {
inline constexpr std::uint64_t kHandle = 0x6b0;  // 8 bytes
inline constexpr std::uint64_t kAsset = 0x6b8;   // 8 bytes
inline constexpr std::uint64_t kText = 0x6c0;    // the asset name
inline constexpr std::uint64_t kTextCapacity = 0x40;  // to 0x700, the block's end
}  // namespace block

// What the puppet's player asks for: the newest OCARINA_NOTE.
struct Wanted {
    std::uint8_t note = 0;      // 0 let go, 1..19 CoopOcarinaSound's table
    std::uint32_t serial = 0;   // goes up with each received note; 0 none yet
    std::uint64_t at_ms = 0;    // when it was received (steady ms)
};

// One puppet's sound state, kept in the director's slot (the calc thread).
struct State {
    enum class Step : std::uint8_t { none, kill, search, emit };
    std::uint64_t actor = 0;     // the puppet these are for; a new one starts silent
    std::uint32_t serial_done = 0;
    std::uint8_t want = 0;       // the note being started
    std::uint8_t sounding = 0;   // emitted; its handle may still play
    Step step = Step::none;      // the next call
    Step pending = Step::none;   // the call made, until Finish
    std::uint64_t user = 0;      // the sound user the note is emitted on
    std::uint64_t logged_user = 0;  // the puppet's own sound user, as last logged
    bool user_said = false;      // its name logged
    bool missing_said = false;   // "no sound user" logged
    // The game's own "Ocarina" SLink user, which holds the Ocarina_<note> assets (the
    // player's user has none of them): found once by walking the user lists from the
    // puppet's user (FindUser); 0 not looked for or not found (then the puppet's user).
    std::uint64_t ocarina_user = 0;
    bool ocarina_looked = false;
    std::string module_said;  // the last module line logged (said again only when it changes)
    std::string playing;         // the names last logged (debug)
    std::uint64_t playing_ms = 0;
};

// A note older than this when the puppet can play it is not played (the
// puppet was made later, or loading held it up); a let-go still is.
inline constexpr std::uint64_t kStaleMs = 1500;
// How often the debug log reads what the puppet's sound user plays.
inline constexpr std::uint64_t kPlayingEveryMs = 250;
inline constexpr int kMaxEvents = 16;

using Say = std::function<void(const std::string&)>;

// The next call for this puppet's sound, if any. `who` is "P<n>".
std::optional<CoopPuppet::Call> Next(State& state, const Wanted& wanted, CoopPuppet::Guest& guest,
                                     std::uint64_t main_base, std::uint64_t actor,
                                     std::uint64_t block, std::uint64_t now_ms,
                                     const std::string& who, const Say& say);
// The result (x0) of the call Next gave.
void Finish(State& state, std::uint64_t result, const std::string& who, const Say& say);
// A new actor for the slot (made again or lost): nothing sounds on it.
void Reset(State& state);

// The sound user of an actor (its SLink user instance), 0 if none.
std::uint64_t SoundUser(CoopPuppet::Guest& guest, std::uint64_t actor);
// The name of a user's resource ("Player_Child", "Ocarina"), empty if unreadable.
std::string UserName(CoopPuppet::Guest& guest, std::uint64_t user);
// Another user named `name`, found by walking every circular list that runs through
// `from` (each 8-byte field in its first kUserScan bytes taken as a list link): 0 if none.
// `link` gets the field's offset.
std::uint64_t FindUser(CoopPuppet::Guest& guest, std::uint64_t from, const std::string& name,
                       std::uint64_t* link = nullptr);
inline constexpr std::uint64_t kUserScan = 0x140;
// The game's SLink system, which makes the users (0xd1f360 makes "Ocarina" with its
// vfunc +0x28): [[[main + kSoundRootGot]] + kRootSLinkSystem].
std::uint64_t SLinkSystem(CoopPuppet::Guest& guest, std::uint64_t main_base);
// A user named `name` the system keeps: in a pointer array ({s32 count, s32 capacity,
// T** data}) or a circular list (any link offset below kUserScan) in the system's first
// kSystemScan bytes. `how` says where. 0 if none.
std::uint64_t FindUserInSystem(CoopPuppet::Guest& guest, std::uint64_t system,
                               const std::string& name, std::string* how = nullptr);
// The sound root ([[main + kSoundRootGot]]): its modules are in a pointer array (+0xc0:
// count, +0xc8: data, as 0xd0d214 walks it), and the "Ocarina" module (vtable
// main+0x4bd6df0, made at 0xd1f360) keeps its user at +0x40. A user named `name` held by
// any module of any array in the root's first kSystemScan bytes, in the module's first
// kModuleScan bytes. `how` says where. 0 if none.
std::uint64_t SoundRoot(CoopPuppet::Guest& guest, std::uint64_t main_base);
std::uint64_t FindUserInModules(CoopPuppet::Guest& guest, std::uint64_t root,
                                const std::string& name, std::string* how = nullptr);
inline constexpr std::uint64_t kModuleScan = 0x100;
// The "Ocarina" module itself, by its vtable: [main + addr::kOcarinaModuleVtableGot] + 0x10
// (its constructor 0xf17c30 stores that), held by the root directly or in an array, or one
// object further down (each object's first kRootScan bytes). `how` says where. 0 if none.
std::uint64_t FindOcarinaModule(CoopPuppet::Guest& guest, std::uint64_t main_base,
                                std::uint64_t root, std::string* how = nullptr);
inline constexpr std::uint64_t kRootScan = 0x200;
// One line on a module and the user at its +0x40 (name, accessor, user checks), for the log.
std::string DescribeModule(CoopPuppet::Guest& guest, std::uint64_t module);
// The checks FindUser makes on a candidate (debug: logged for the puppet's own user).
bool LooksLikeUser(CoopPuppet::Guest& guest, std::uint64_t user);
inline constexpr std::uint64_t kSystemScan = 0x400;
inline constexpr int kSystemListSteps = 4096;
inline constexpr int kListSteps = 2048;
inline constexpr const char* kOcarinaUser = "Ocarina";
// The asset names a user plays now (at most kMaxEvents), joined with ", ".
std::string Playing(CoopPuppet::Guest& guest, std::uint64_t user);
// Debug (LICHII_COOP_DEBUG_ANIM): every kPlayingEveryMs, a line when what the
// puppet's sound user plays changed ("P1 sounds: Ocarina_D" / "none").
void LogPlaying(State& state, CoopPuppet::Guest& guest, std::uint64_t actor,
                std::uint64_t now_ms, const std::string& who, const Say& say);

} // namespace CoopPuppetSound
