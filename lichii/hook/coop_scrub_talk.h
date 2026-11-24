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
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
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

// The scene of every scrub kTalkFlags deletes. Row 275: the event hold applies only there. Anywhere else no scrub of
// these flags is loaded, so none can be deleted mid-talk; held there (a boss room's intro, a join's own events) the
// host's save values waited behind an event that may not end for minutes.
inline constexpr std::string_view kScene = "InsideTheDekuTree";

// The event hold for this flag in our Link's scene (none known: a load, nothing of these scrubs is loaded).
inline bool HoldsIn(const Hold& hold, std::optional<std::string_view> scene, std::uint32_t hash) {
    return scene && *scene == kScene && hold.Holds(hash);
}

// Issue 144, second guard: our copy of the scrub a flag deletes is near our
// Link. The hold above trusts the game's event flag (event_running), which
// has never been seen during a scrub's talk. So the flags of a scrub also
// wait while that scrub is alive within kNearM of our Link (the objects walk
// sees it, coop_objects.cpp): a talk always starts next to the scrub, so a
// talk can never lose its scrub, whatever the event flag says. Once our Link
// walks away (or the scrub is gone: our own talk ended and the room graph
// deleted it) the flag lands, and the room graph deletes the scrub as in the
// first game. A scrub we have not beaten yet waits the same way: we may beat
// it and talk to it ourselves; our own talk sets the same flag.
// The root guard (review of scrub-sale): a scrub that is an event member in
// our Link's event (CoopEventMember::InSameEvent, the game's own member test)
// holds its flags at any distance. A Deku Scrub's talk starts when a nut it
// spat is deflected back into it (Talk_Deku_0N_Attacked), so Link can stand
// far from it, and event_running has not been seen during a scrub's event:
// the objects walk reads the event member for these scrubs whatever
// event_running says.
//
// Read from the romfs (Banc/InsideTheDekuTree/InsideTheDekuTree_Static,
// build e0f3ccfaf50065ae): the placement hashes of the scrubs, by group.
enum class Group : std::uint8_t { room_b = 0, room_f = 1, brothers = 2 };
inline constexpr std::size_t kGroups = 3;

struct NearScrub {
    std::uint64_t hash;
    Group group;
    std::string_view name;
};
inline constexpr std::array<NearScrub, 5> kNearScrubs{{
    {0xaa6f22763e99e851ULL, Group::room_b, "room B's Deku Scrub (Talk_Deku_01_Attacked)"},
    {0x36baa28896fca5cdULL, Group::room_f, "room F's Deku Scrub (Talk_Deku_02_Attacked)"},
    {0x927ab536d2e86500ULL, Group::brothers, "Scrub Brother Two"},
    {0x079a5f22c7ad91c1ULL, Group::brothers, "Scrub Brother Three"},
    {0x9b04ae25f50c7fcaULL, Group::brothers, "Scrub Brother One"},
}};

inline const NearScrub* FindNearScrub(std::uint64_t hash) {
    for (const NearScrub& s : kNearScrubs) {
        if (s.hash == hash) return &s;
    }
    return nullptr;
}

// The scrubs' group a talk flag deletes (the order of kTalkFlags: _001, _002,
// _003, RoomB, RoomF, RoomK).
inline std::optional<Group> GroupOf(std::uint32_t hash) {
    switch (hash) {
    case 0x1c594551: case 0x64edd2f3: return Group::room_b;
    case 0x1d493033: case 0xc90d4ed1: return Group::room_f;
    case 0x60c22ed7: case 0xd2f46f66: return Group::brothers;
    default: return std::nullopt;
    }
}

inline const char* GroupName(Group g) {
    switch (g) {
    case Group::room_b: return "room B's scrub";
    case Group::room_f: return "room F's scrub";
    case Group::brothers: return "the Scrub Brothers";
    }
    return "?";
}

// Written by the objects walk, read by the flag hold (another thread): the
// last time each group was seen alive near our Link.
class Near {
public:
    // How far a talk starts, from the game data: the scrub's EventOn
    // attention fan reaches 2.0 m from Link, plus its CharacterBoundingRadius
    // 2.3 m, plus AutoMargin up to 1.0 m: about 5.3 m from the actor place.
    // Seen in lane1-a's scrub-near2 run (20261116-002801): B talked to its One
    // from 5.29 m (Link at -23.29 52.88 34.69, One at -23.72 52.97 39.96), and
    // A, 5.25 m away, was not held under the old 5 m. 8 m covers a talk and
    // still lets a player who only stands in the brothers' room go: the entry
    // and the aiming spots are 10.5 to 12.4 m from the nearest brother. An
    // event that starts further away (a deflected nut) is held by `in_event`.
    static constexpr float kNearM = 8.0f;
    static constexpr std::uint64_t kFreshUs = 1'500'000;  // a walk is ~4 a second; it may stop

    // One scrub of the walk: its placement hash, its distance to our Link and
    // whether it is an event member in our Link's event (nullopt: not read).
    void See(std::uint64_t hash, float distance_m, std::uint64_t now_us,
             std::optional<bool> in_event = std::nullopt) {
        const NearScrub* s = FindNearScrub(hash);
        if (s == nullptr) return;
        const bool near = std::isfinite(distance_m) && distance_m <= kNearM;
        if (!near && !in_event.value_or(false)) return;
        last_us_[static_cast<std::size_t>(s->group)].store(now_us, std::memory_order_relaxed);
    }
    // The groups held now, a bit each (bit = Group).
    std::uint8_t Mask(std::uint64_t now_us) const {
        std::uint8_t m = 0;
        for (std::size_t g = 0; g < kGroups; ++g) {
            const std::uint64_t at = last_us_[g].load(std::memory_order_relaxed);
            if (at != 0 && now_us >= at && now_us < at + kFreshUs) m |= static_cast<std::uint8_t>(1u << g);
        }
        return m;
    }
    void Reset() {
        for (auto& a : last_us_) a.store(0, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<std::uint64_t>, kGroups> last_us_{};
};

// Is this flag held by a Mask()?
inline bool NearHolds(std::uint8_t mask, std::uint32_t hash) {
    const auto g = GroupOf(hash);
    return g && (mask & (1u << static_cast<unsigned>(*g))) != 0;
}

// The one the hook and the objects walk share.
inline Near& TheNear() {
    static Near near;
    return near;
}

} // namespace CoopScrubTalk
