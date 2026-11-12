// SPDX-License-Identifier: GPL-3.0-or-later
//
// The story the same for every player (issue 173, docs/flags.md "Story and
// cutscenes"). A story scene one player has seen (Saria at the forest exit,
// the owl in Hyrule Field) is a saved `HavePlayedEvent.<scene>` flag; the
// world's story gates (the invisible wall at the forest exit, Mido at the
// Deku Tree path) are named flags that the overworld's placements test
// (`Presence.FlagName` in Banc/<area>/*.bcett.byml). Sharing them makes a
// scene seen once count for everyone and opens a gate in every game, and,
// like every shared flag, the host's save decides (FLAG_SNAPSHOT for a player
// who joins, FLAG_RESET when the host loads a save). No emulator code: tested
// on its own.
#pragma once

#include <cstdint>
#include <optional>
#include <span>

#include "coop/host/flags.hpp"

namespace CoopStory {

enum class Kind : std::uint8_t {
    scene,  // a story scene or story event has played (HavePlayedEvent.)
    owl,    // a talk with the owl (Kaepora Gaebora)
    world,  // a story gate placements test: an invisible wall, a blocker, a water plane
    talk,   // a talk shared on request (the Kokiri Shop keeper's)
};

// One story flag: the Murmur3 hash of its name, what it is, and the name, for
// logs and tests. All are saved (ResetTypeValue 8) Bools.
struct Flag {
    std::uint32_t hash;
    Kind kind;
    const char* name;
};

// Every story flag shared, sorted by hash.
std::span<const Flag> Flags();

// The flag of `hash`, or nullptr.
const Flag* Find(std::uint32_t hash);

// Adds the story flags to `shared` when `on`, making the list when there is
// none. Flags already there are kept once. Returns how many it added.
std::size_t Merge(std::optional<coop::host::SharedFlags>& shared, bool on);

// False when LICHII_COOP_STORY=0.
bool On();

// Merge, on unless LICHII_COOP_STORY=0; logs what it did.
void AddShared(std::optional<coop::host::SharedFlags>& shared);

// Issue 130: the game's quest "Meet_DekuTree" (Challenge table), a saved Enum
// (hash 0x3372689b from GameDataList; its name is not in the romfs). Its
// steps: NotReady, Ready (Navi woke Link), Step01 (the opening), Step02 (Mido
// asked for a sword and shield), Step03 (Mido let Link by), Complete (the Deku
// Tree scene Dm_Kokiri_0075). The Kokiri Shop sells only from Step02 on
// (Banc Presence `Meet_DekuTree_IsAfter_Step02_Exp`, a BoolExp "step >=
// Step02"): before that its keeper is the ShortStock one with sold-out boards.
// Only Mido's talk moves Step02 and Step03, so a player whose Mido moved
// aside because another player got past him (the shared Mido blocker) could
// never buy the Deku Shield. The quest itself is not shared (its value is an
// Enum hash, not ordered, and each game's opening writes its own step);
// instead each game raises its own quest to what the shared story says.
namespace quest {
inline constexpr std::uint32_t kMeetDekuTree = 0x3372689bu;
inline constexpr std::uint32_t kNotReady = 0x57b849f0u;  // flag_hash of each step's name
inline constexpr std::uint32_t kReady = 0x17bfaa46u;
inline constexpr std::uint32_t kStep01 = 0xeb62add9u;
inline constexpr std::uint32_t kStep02 = 0x0e5afad8u;
inline constexpr std::uint32_t kStep03 = 0x6f917dfeu;
inline constexpr std::uint32_t kComplete = 0x4c0a63f4u;
// Shared story flags (kFlags): Mido's blocker, 1 until he lets Link by; the
// Deku Tree scene that completes the quest.
inline constexpr std::uint32_t kMidoBlocker = 0x502f6ceau;  // KokiriForest_Npc_Kokiri_Mido_Child_Blocker
inline constexpr std::uint32_t kMetDekuTree = 0x5ab867ffu;  // HavePlayedEvent.Dm_Kokiri_0075
} // namespace quest

// The step to write into our quest, or nothing: Complete once the Deku Tree
// scene played, else Step03 once Mido's blocker is 0. Only ever raised, and
// only from Ready, Step01 or Step02 (a game before its opening, or with an
// unknown value, is left alone). Unknown inputs count as "not yet".
std::optional<std::uint32_t> QuestCatchUp(std::optional<std::uint32_t> step,
                                          std::optional<bool> mido_blocker,
                                          std::optional<bool> met_deku_tree);

} // namespace CoopStory
