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

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

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
// The Deku Tree's mouth, a shared story flag: Dm_Kokiri_0075 sets it true
// and, in the next event of the same cut, moves the quest to Complete.
inline constexpr std::uint32_t kDekuTreeMouth = 0x9a60a460u;  // KokiriForest_Meet_DekuTree_OpenDekuTreeMouth
} // namespace quest

// The step to write into our quest, or nothing: Complete once the Deku Tree
// scene played, else Step03 once Mido's blocker is 0. Only ever raised, and
// only from Ready, Step01 or Step02 (a game before its opening, or with an
// unknown value, is left alone). Unknown inputs count as "not yet".
std::optional<std::uint32_t> QuestCatchUp(std::optional<std::uint32_t> step,
                                          std::optional<bool> mido_blocker,
                                          std::optional<bool> met_deku_tree);

// Issue 84: true when Mido's blocker (kMidoBlocker) is 1 in a game that is
// past him: our Meet_DekuTree at Step03 or later, or the Deku Tree scene
// played. The game itself never sets the blocker back to 1 (GameDataList
// default 1; both writes in Npc_Kokiri_Mido_Child.bfevfl set it false, the
// same talk that moves the quest to Step03), and Mido's talks after Step03
// have no branch that clears it. Only a shared value can bring a 1 back (a
// host save or snapshot behind the group), and then Mido blocks the Deku
// Tree path in every game for good, also after Gohma. Mended to 0 through
// the game's own Bool setter. Unknown inputs count as "not past him".
bool MidoBlockerMend(std::optional<std::uint32_t> step, std::optional<bool> mido_blocker,
                     std::optional<bool> met_deku_tree);

// Issue 84 follow-up: true when the Deku Tree's mouth (kDekuTreeMouth) is
// closed in a game whose Meet_DekuTree is Complete. Only Dm_Kokiri_0075 sets
// the quest to Complete, right after it opens the mouth, and no flow or logic
// closes the mouth again; the scene never plays again once the quest is
// Complete. So a 0 there came from a shared value behind the group, and the
// Deku Tree is shut for good. Unknown inputs count as "not Complete".
bool DekuTreeMouthMend(std::optional<std::uint32_t> step, std::optional<bool> mouth);

// A shared story flag the game moves only one way (GameDataList default, and
// every write in Event/EventFlow and Logic goes the other way only). Co-op
// never moves it back over a game that is past it (FlagWatcher::
// share_down_only / share_up_only): a 1 the game only lowers, or a 0 the game
// only raises, from a host save, a join snapshot or a load's write-back would
// block a path the game never opens again (issue 84, docs/flags.md).
enum class Way : std::uint8_t {
    down,  // default 1, the game only writes 0: a blocker that steps aside
    up,    // default 0, the game only writes 1: a gate a story scene opens
};
struct OneWay {
    std::uint32_t hash;
    Way way;
    const char* name;
};
std::span<const OneWay> OneWays();

// A one-way flag our own quest proves (the game's own invariant), set back
// by Coop through the game's Bool setter after a short hold.
struct Mend {
    std::uint32_t flag;
    bool value;        // what the quest proves
    const char* what;  // for the log: what was wrong
    bool (*due)(std::optional<std::uint32_t> step, std::optional<bool> now, std::optional<bool> met_deku_tree);
};
inline constexpr std::size_t kMends = 2;
std::span<const Mend, kMends> Mends();

// Row 145: a quest step the game's own flows set back below a step that a
// played story scene proves. Get_StoneOfWater (romfs Event/EventFlow/
// Get_StoneOfWater.bfevfl): StoneOfWater_ClearDivingGame sets Step05,
// StoneOfWater_GetRutoletter Step05_01 and StoneOfWater_UseRutoletter Step06,
// each only checking Get_StoneOfWater_IsAfter_Step03_Exp, never a later step;
// Dm_SY_BancChangeArrive's Goto_ZoraDomain sets Step03 or Step03_Impa with no
// check of the quest at all. King Zora stands aside from Step07 on (the
// scene Dm_Zora_0010, StoneOfWater_ShowLetter), so the diving game, a letter
// got or read, or that arrival after he moved put him back in front of the
// fountain with the letter already spent. Solo too; co-op makes it likelier
// (a shared Step07 in a game that never did the diving game or still holds
// a letter). Raised back to the floor through the game's Enum setter, after
// a hold and only outside events; never from NotReady or an unknown value.
// The same shape, SageOfWater (review of row 145): the Lake Hylia door into
// the Water Temple (HyruleField banc, NextEventName
// SageOfWater_Arrival_WaterTemple = Dm_SY_BancChangeArrive:WaterTemple_Arrival)
// plays at EVERY entry and sets Step06 whenever SageOfWater_IsAfter_Step04_Exp
// holds, with no upper check: re-entering after Ruto's scene (Step07), Dark
// Link (Step07_1, which Room 23 on the way to Morpha reads), Morpha's intro
// (Step07_2) or his death (Step08) set the quest back to Step06. Each of those
// has its own played flag; the highest one proven wins (rows of one quest are
// listed highest first, and MendStory raises only the first row due).
struct StepFloor {
    std::uint32_t quest;  // the quest's Enum, Step_<quest>
    std::uint32_t proof;  // a played-scene Bool only the floor step's own flow sets
    std::uint32_t floor;  // the step that scene proves
    std::span<const std::uint32_t> back;  // the lower steps the game's flows set with no check against it
    const char* what;     // for the log
};
inline constexpr std::size_t kStepFloors = 5;
std::span<const StepFloor, kStepFloors> StepFloors();
// The floor to write, or nothing: the proof is 1 and our step is one of `back`.
std::optional<std::uint32_t> StepFloorDue(const StepFloor& floor, std::optional<std::uint32_t> step,
                                          std::optional<bool> proof);

// Row 129 (GitHub #173): a story event whose played flag is shared never
// runs in a game behind the group (the game counts it as played), so what
// its own flow sets there never happens. The owl in Hyrule Field is the only
// setter of HyruleField_ArriveEntrance, and the game's clock stays stopped
// until that flag is 1 (docs/time.md, "Clock gate"). Each effect is a saved
// Bool such an event sets on its unconditional path, one way only in every
// event flow and AI graph of the game, and not shared itself
// (tools/puppet_pack/story_effects.py writes the list from the romfs): once
// the event has played it holds that value for good, so a game with the
// played flag and not the value is given it through the game's own Bool
// setter (co-op idle too: a save that got the played flag in a session).
struct Effect {
    std::uint32_t played;  // HavePlayedEvent.<setting>
    std::uint32_t flag;
    bool value;
    const char* setting;
    const char* name;
};
std::span<const Effect> Effects();

// Review of owl-time (should-fix): each effect's flag is given at most once per load. A flag set back after that in
// the same load (no such writer is known: story_effects.py reads the romfs, not the game's code) is then left alone,
// said once, instead of being fought every 3 s with a log line each time. A new load (an older save reloaded, the
// flag 0 there again) may give it once more. The caller's hold and the "due" rule (EffectsDue) are unchanged.
// Take() only says whether to set it; Given() records it once set_bool succeeded, so a failed write is tried again
// on the next pass instead of counting as given (and later as a false "set back").
class EffectsGiven {
public:
    enum class Verdict : std::uint8_t {
        give,      // not given since this load: set it
        set_back,  // given since this load and due again: left alone, say so (once)
        quiet,     // left alone, said already
    };
    Verdict Take(const Effect& due);
    void Given(const Effect& due);  // set_bool succeeded
    void NewLoad();  // the title or a load: nothing read now

private:
    std::vector<std::uint32_t> given_;  // flags given since this load
    std::vector<std::uint32_t> said_;   // of those, set back and said
};
inline constexpr std::size_t kEffectsMax = 32;  // Effects().size() stays under it (story_test)

// The effects due now: the played flag is 1 and the effect's flag is known
// and not its value. `read` gives a Bool's value, nothing when unread
// (unknown inputs count as "not due"). Each flag is due at most once: several
// events can set the same flag (DeathMtCrater_IsFirstEntry from both crater
// entrances, MelodyGet_Time_EndConversationWithZelda from both Song of Time
// scenes; story_test checks every flag has one value), and the first played
// row stands for all of them.
template <class ReadBool>
std::size_t EffectsDue(ReadBool&& read, std::span<const Effect*> out) {
    std::size_t n = 0;
    for (const Effect& e : Effects()) {
        if (n == out.size()) break;
        const std::optional<bool> played = read(e.played);
        if (!played || !*played) continue;
        const std::optional<bool> now = read(e.flag);
        if (!now || *now == e.value) continue;
        bool listed = false;
        for (std::size_t i = 0; i < n; ++i) listed = listed || out[i]->flag == e.flag;
        if (!listed) out[n++] = &e;
    }
    return n;
}

// Rows 195/204 (ocarina-0119, docs/items.md "Saria's gift spent"): a one-time
// gift whose scene co-op can mark played without the gift ever running here.
// Read from the romfs: Saria's bridge scene is the setting
// KokiriForest_ArriveLostWood (flow Goto_HyruleCastle), which plays the
// timeline Dm_Kokiri_0150 and then Dm_Kokiri_0155, the only one with the
// pouch get of Item_Ocarina_Fairy. The bridge's trigger zone
// (Logic/KokiriForest_Child_Level_All_02_ddcd) is deleted once
// HavePlayedEvent.Dm_Kokiri_0150 is 1, and Saria's village actor
// (KokiriForest_Child_Level_Npc_Kokiri_Saria_Child_a766, Npc_Kokiri_Saria_Child_ed73)
// once it is 1 with Get_StoneOfForest after GoToWarpPoint (or the castle
// quest complete): after that the game offers the ocarina nowhere. All three
// played flags are shared story flags and Dm_Kokiri_0150 is a co-watched
// scene, played alone (timeline only, no gift) in a game that joins it. So a
// game where the flag came in, or was set by a start that never reached
// Dm_Kokiri_0155 anywhere (a scene started from a leftover name, a starter
// that left or crashed mid-scene, a host save or join snapshot holding it),
// had no Saria and no ocarina for good. A spent gift with none of `held`
// in the pouch is given through the game's own setters, as the gift's get
// writes it; in a session and alone, so a save stuck that way is mended at its
// next load.
struct Gift {
    std::span<const std::uint32_t> played;  // HavePlayedEvent.<setting>: any one set spends the gift here
    std::uint32_t count;                    // PouchData.<item>.StockNum, set to 1
    std::span<const std::uint32_t> held;    // StockNums: any above 0 means the gift (or better) is held
    const char* item;
    const char* where;  // for the log
    // Given by MendGifts when spent and not held. Off for the Ocarina of Time: under the randomizer its spot gives a
    // progressive ocarina (OOTRR flow_rules.rs pins "HF Ocarina of Time Item" to "Ocarina"), so the item a spent
    // scene stands for is not always that one; its played flag is only held (GiftHolds).
    bool mend;
};
std::span<const Gift> Gifts();
inline constexpr std::size_t kGiftsMax = 4;  // Gifts().size() stays under it (story_test)
// Whether the gift (or better) is in the pouch: any held count above 0 is
// true, all read and 0 is false, otherwise not known (a load, the title).
template <class ReadInt>
std::optional<bool> GiftHeld(const Gift& gift, ReadInt&& read_int) {
    bool all_read = true;
    for (const std::uint32_t h : gift.held) {
        const std::optional<std::int32_t> n = read_int(h);
        if (n && *n > 0) return true;
        all_read = all_read && n.has_value();
    }
    return all_read ? std::optional<bool>(false) : std::nullopt;
}
// Whether `gift` is due: one of its played flags reads 1 and every held count
// reads 0. An unread flag or count is never due (a load, the title).
template <class ReadBool, class ReadInt>
bool GiftDue(const Gift& gift, ReadBool&& read_bool, ReadInt&& read_int) {
    bool spent = false;
    for (const std::uint32_t p : gift.played) {
        const std::optional<bool> v = read_bool(p);
        spent = spent || (v && *v);
    }
    return spent && GiftHeld(gift, read_int) == std::optional<bool>(false);
}
// The cause (rows 195/204): a gift's played flag from another game is held
// (coop_hook's HoldFlagStore) while our pouch is not known to hold the gift.
// It lands once the item came (it is a shared item: the game that ran the gift
// scene shares it), so a game where no gift ran keeps its own giver and plays
// the scene itself; a 1 that comes with no gift anywhere never takes Saria
// from the bridge. The index of the gift `hash` is a played flag of, or none.
std::optional<std::size_t> GiftOf(std::uint32_t hash);
// Holds `hash` now: a gift's played flag while that gift is not known held
// (`held[i]`: GiftHeld of Gifts()[i] is true).
bool GiftHolds(std::uint32_t hash, const std::array<bool, kGiftsMax>& held);

} // namespace CoopStory
