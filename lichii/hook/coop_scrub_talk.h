// SPDX-License-Identifier: GPL-3.0-or-later
//
// Issue 144 (docs/dungeon.md "A scrub's talk"): a softlock while our Link
// talks to a beaten Deku Scrub and another player beats the same scrub.
//
// Read from the romfs (build e0f3ccfaf50065ae): the Deku Tree's room graphs
// delete their scrubs from a shared flag, not from the scrub's own life:
// - Logic/InsideTheDekuTree_Level_Room_B_Prefab_b62e: HoldGameDataGetBoolSimple
//   InsideTheDekuTree_Dungeon_EnemyScrubDekuDestroy_001 -> Logic_Delete of
//   Enemy_Scrub_Deku_5fdb and its leaf (RoomB_Cleared from the same room).
// - Logic/InsideTheDekuTree_Level_Room_F_Prefab_bdf4: _002 (and RoomF_Cleared)
//   for Enemy_Scrub_Deku_7a6e.
// - Logic/InsideTheDekuTree_Level_Room_K_Prefab_6708: _003 and RoomK_Cleared
//   for the three Scrub Brothers.
// The talk that ends the fight (Event/EventFlow/Get_StoneOfForest,
// Talk_Deku_0N_Attacked) sets the flag. These flags are shared dungeon flags
// (coop_dungeon.cpp), so the first player's talk set it in every game, and in
// a game whose Link was in the same scrub's talk the room graph deleted the
// scrub the event was playing on: the event never ended. coop_guard.h
// (event-hold) holds the object sync's kills and removes near an event, but
// this flag goes through the flag sync, where nothing waited.
//
// Here: while our Link is in an event, and a moment after it, these flags
// from another player wait (coop_hook's HoldFlagStore, the same hold as the
// room gimmick events); the flag watcher writes them once the hold lets go,
// and the room graph then deletes the scrub, as in the first game.
// Pure: tested in tests/lichii/scrub_talk_test.cpp.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace CoopScrubTalk {

struct TalkFlag {
    std::uint32_t hash;
    std::string_view name;
};

// The flags a Deku Tree room graph deletes a scrub from (hashes as in
// coop_dungeon.cpp's list).
inline constexpr std::array<TalkFlag, 6> kTalkFlags{{
    {0x1c594551, "InsideTheDekuTree_Dungeon_EnemyScrubDekuDestroy_001"},
    {0x1d493033, "InsideTheDekuTree_Dungeon_EnemyScrubDekuDestroy_002"},
    {0x60c22ed7, "InsideTheDekuTree_Dungeon_EnemyScrubDekuDestroy_003"},
    {0x64edd2f3, "InsideTheDekuTree_RoomB_Cleared"},
    {0xc90d4ed1, "InsideTheDekuTree_RoomF_Cleared"},
    {0xd2f46f66, "InsideTheDekuTree_RoomK_Cleared"},
}};

inline bool IsTalkFlag(std::uint32_t hash) {
    for (const TalkFlag& f : kTalkFlags) {
        if (f.hash == hash) return true;
    }
    return false;
}

// Our Link's event, with a tail: a scrub's talk is followed by its run away
// (RunAwayEvent) and the event flag may drop for a frame between them.
class Hold {
public:
    static constexpr std::uint64_t kTailUs = 2'000'000;

    // Once a frame. Returns true on the frame the hold starts or ends (for a log line).
    bool Frame(bool in_event, std::uint64_t now_us) {
        if (in_event) {
            last_event_us_ = now_us;
            seen_ = true;
        }
        const bool holding = seen_ && now_us < last_event_us_ + kTailUs;
        const bool changed = holding != holding_;
        holding_ = holding;
        return changed;
    }
    bool Holding() const { return holding_; }
    // Holds this flag now (called by the HoldFlagStore for each write).
    bool Holds(std::uint32_t hash) const { return holding_ && IsTalkFlag(hash); }

private:
    std::uint64_t last_event_us_ = 0;
    bool seen_ = false;
    bool holding_ = false;
};

} // namespace CoopScrubTalk
