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
    std::uint64_t user = 0;      // the sound user found last
    bool user_said = false;      // its name logged
    bool missing_said = false;   // "no sound user" logged
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
// The asset names a user plays now (at most kMaxEvents), joined with ", ".
std::string Playing(CoopPuppet::Guest& guest, std::uint64_t user);
// Debug (LICHII_COOP_DEBUG_ANIM): every kPlayingEveryMs, a line when what the
// puppet's sound user plays changed ("P1 sounds: Ocarina_D" / "none").
void LogPlaying(State& state, CoopPuppet::Guest& guest, std::uint64_t actor,
                std::uint64_t now_ms, const std::string& who, const Say& say);

} // namespace CoopPuppetSound
