// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_quests.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <utility>

#ifdef LICHII_COOP
#include "common/logging.h"
#endif

namespace CoopQuests {
namespace {

// Read from the romfs: RSDB/Challenge.Product.111.rstbl.byml (each quest's
// steps in order) and GameData/GameDataList.Product.110.byml (the Enum whose
// RawValues are NotReady, those steps, Complete; ResetTypeValue 8, saved).
// Every value's hash is flag_hash of its name (checked in the tests).
// Meet_DekuTree's step list is also that of two minigames; 0x3372689b is the
// quest's (its BoolExp Meet_DekuTree_IsAfter_Step02_Exp, the Kokiri Shop).
//
// Shared: the quests the Threads of Time tab lists (no IsListAdventureLog
// false) whose steps only go forward. Left out, each game's own:
//   - minigames and every quest the tab does not list (their steps go back
//     and forth: Ready, Playing, Retry, Finish);
//   - SubEvent_GetSwordBigGoron (the trading sequence and the egg's timer),
//     SubEvent_MaskSalesman (the mask rentals): steps of one player's own
//     items (the trade's step follows the shared trade item held instead:
//     coop_hook.cpp FollowTradeItem, row 117);
//   - DoorOfTime (time travel stays each game's own, docs/flags.md), AllSage
//     (the Chamber of Sages; its Step01_00_01 to _03 are not in an order),
//     Defeat_Ganon and EscapeFromCrumblingGanonCastle (the final battle).
//   - for now, SageOfShadow, SageOfSpirit and SageOfWater: their RawValues
//     look out of order or branched (GotGlassTruth before GetGlassTruth,
//     Step07 before Step07_pre00, Step07_1_Safety), and with the max rule a
//     wrong order skips a step. Back in once checked against their event flows.
//     (Checked for the mid-bosses: the RawValues are the RSDB order, but the
//     game sets SageOfSpirit Step07_00 at the adult's arrival whenever it
//     plays, so a max would still move steps the wrong way. The steps a
//     mid-boss's death gives, Dark Link's SageOfWater Step07_1 and the Iron
//     Knuckle's SageOfSpirit Step08_01, are raised by CoopQuest from the
//     shared death records instead: coop_quest.cpp, "Mid-bosses".)
//
// Get_StoneOfFire (the Goron Ruby) is shared since issue 60: its RawValues are
// the RSDB row's steps in the story's order (KokiriForestFire before
// KokiriForestIn is the story: Darunia sends Link to Saria), set one after the
// other by its event flows. Left out, a player who was not there for Darunia
// never got Complete, and Get_StoneOfFire_IsCompleted_Exp is what the Door of
// Time's DoorOfTime_CheckStoneNum reads (CoopQuest then moves DoorOfTime on).
// Floor Ready: Ready comes from the game's quest manager, never from a scene,
// so only a game still at NotReady (a new save's opening) is left alone.
//
// Get_Epona (0x92431c55, not listed in the tab) is shared by its end only
// (Quest::complete_only): its steps are one player's own (the song as a
// child, the race against Ingo, which goes back to ReadyForGambleRace when
// lost), but once one player has won Epona every game's quest is Complete,
// so every adult can call and ride their own Epona (each game has its own
// horse: docs/puppet.md, "Riding"). A game in the race itself
// (GambleRacePlaying, AfterWonGambleRace while LonLonRanch_PlayingGambleRace
// is 1) is left to finish it; row 150: at those steps with no race running it
// takes the end too.
//
// Also writing quest Enums: CoopQuest (coop_quest.cpp, time-story, issue 173)
// raises DoorOfTime from our Goto_HyruleCastle; both only raise.
const std::vector<Quest>& Table() {
    static const std::vector<Quest> kQuests = {
        {0x1002b466u, "Get_StoneOfFire",
         {"NotReady", "Ready", "KakarikoVillageNpc", "KakarikoVillageLetter", "DeathMtTrailIn",
          "DeathMtTrailNpc", "GoronCityIn", "GoronCityDarniaroomNpcTalk01", "GoronCityDarniaroomOpen",
          "GoronCityDarniaTalk", "DarniaRoomKokiriTalk", "KokiriForestNPCTalk", "KokiriForestFire",
          "KokiriForestIn", "SariaMelodyGet", "SariaMelodyPlay", "DodongoCavernOpne", "DungeonStart",
          "BossBattle", "EscapeDungeon", "Complete"},
         1},  // floor: Ready (the manager's own, never a scene's)
        {0x3372689bu, "Meet_DekuTree", {"NotReady", "Ready", "Step01", "Step02", "Step03", "Complete"}},
        {0x44063c4eu, "SubEvent_GetMelodySun", {"NotReady", "Ready", "GetMelodySun", "Complete"}},
        {0x5bca02a6u, "Get_StoneOfWater",
         {"NotReady", "Ready", "Step01", "Step02", "Step03", "Step03_Impa", "Step04", "Step05",
          "Step05_01", "Step06", "Step07", "Step08", "Step09", "Step10", "Step11", "Step12", "Step13",
          "Step13_01", "Step14", "Complete"}},
        {0x606c3207u, "SageOfLight", {"NotReady", "Ready", "Complete"}},
        {0x682d1169u, "Meet_GreatFairy",
         {"NotReady", "Ready", "GoToTopOfMountain", "GoToFountain", "GetFairyBreath", "GoToHyruleCastle",
          "Complete"}},
        {0x7a5b9537u, "SubEvent_GetStormSong",
         {"NotReady", "Ready", "ReadyForGetStormSong", "step02", "step03", "BeforeBossRoom", "step04",
          "step05", "Complete"}},
        {0x7d776b61u, "Get_StoneOfForest",
         {"NotReady", "Ready", "GoInside", "MeetBoss", "GoToBossRoom", "DefeatBoss", "GoToWarpPoint",
          "Complete"}},
        {0x92431c55u, "Get_Epona",
         {"NotReady", "Ready", "GuideToLonLonRanch", "GuideToLonLonRanchAdult", "ReadyForGetEponaSong",
          "AfterGetEponaSong", "ReadyForGambleRace", "AfterTalkMalon", "GambleRacePlaying",
          "AfterWonGambleRace", "Complete"},
         0, true},
        {0x9472302bu, "SubEvent_GetScarecrow",
         {"NotReady", "Ready", "MeetPierre", "MeetBonooru", "IntoThePast", "PlayScarecrowSong",
          "IntoTheFuture", "Complete"}},
        {0xa9632ccau, "SageOfForest",
         {"NotReady", "Ready", "Step01", "Step02", "Step03", "Step04", "Step05", "Step06", "Step06_1_0",
          "Step06_1_1", "Step06_1_2", "Step06_2", "Step07", "Step08", "Step09", "Complete"}},
        {0xdb89c1a9u, "Goto_HyruleCastle",
         {"NotReady", "Ready", "Step01", "Step02", "Step02_01", "Step03", "Step03_01", "Step03_02",
          "Step04", "Complete"},
         2},  // floor: Step01 (Dm_SY_BancChange_EscapeDungeon sets it as Link leaves the Deku Tree)
        {0xeea87aecu, "SageOfFire",
         {"NotReady", "Ready", "StopDaruniaJr", "TalkDaruniaJr", "MoveStatue", "EnterCrater", "MeetSheik",
          "EnterTemple", "MeetDarunia", "GetBossKey", "MeetBoss", "DefeatBoss", "GoToWarpPoint",
          "Complete"}},
    };
    return kQuests;
}

bool EnvOff(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return false;
    }
    const bool off = std::string_view(value) == "0";
    std::free(value);
    return off;
#else
    const char* value = std::getenv(name);
    return value != nullptr && std::string_view(value) == "0";
#endif
}

std::int32_t Last(const Quest& quest) {
    return static_cast<std::int32_t>(quest.steps.size()) - 1;
}

bool Plain(coop::host::FlagKey key) {
    return key.kind == coop::host::FlagKeyKind::named && key.index == 0;
}

} // namespace

std::span<const Quest> Quests() {
    return Table();
}

const Quest* Find(std::uint32_t hash) {
    const auto& t = Table();
    const auto it = std::lower_bound(t.begin(), t.end(), hash,
                                     [](const Quest& q, std::uint32_t h) { return q.hash < h; });
    return it != t.end() && it->hash == hash ? &*it : nullptr;
}

std::optional<std::int32_t> Rank(const Quest& quest, std::uint32_t value) {
    for (std::size_t i = 0; i < quest.steps.size(); ++i) {
        if (coop::host::flag_hash(quest.steps[i]) == value) {
            return static_cast<std::int32_t>(i);
        }
    }
    return std::nullopt;
}

std::optional<std::uint32_t> ValueAt(const Quest& quest, std::int32_t rank) {
    if (rank < 0 || static_cast<std::size_t>(rank) >= quest.steps.size()) {
        return std::nullopt;
    }
    return coop::host::flag_hash(quest.steps[static_cast<std::size_t>(rank)]);
}

std::size_t Merge(std::optional<coop::host::SharedFlags>& shared, bool on) {
    if (!on) {
        return 0;
    }
    std::vector<coop::host::FlagKey> keys;
    if (shared) {
        keys = shared->keys();
    }
    const std::size_t before = shared ? shared->size() : 0;
    for (const Quest& q : Table()) {
        keys.push_back(coop::host::FlagKey(q.hash, std::uint16_t{0}));
    }
    shared = coop::host::SharedFlags(std::move(keys));  // sorts, drops repeats
    return shared->size() - before;
}

bool On() {
    return !EnvOff("LICHII_COOP_QUESTS");
}

std::vector<CoopQuest::StepWant> LiveWants(const CoopQuest::ReadEnum& value) {
    std::vector<CoopQuest::StepWant> wants;
    for (const Quest& quest : Table()) {
        const bool sent = std::any_of(CoopQuest::Quests().begin(), CoopQuest::Quests().end(),
                                      [&](const CoopQuest::Quest& q) { return q.hash == quest.hash && q.live != 0; });
        if (sent) continue;
        const auto now = value(quest.hash);
        const auto rank = now ? Rank(quest, *now) : std::nullopt;
        if (!rank || *rank < std::max<std::int32_t>(quest.floor, 2)) continue;
        if (quest.complete_only && *rank != Last(quest)) continue;
        CoopQuest::StepWant w{coop::host::flag_hash(quest.name), *now, {}, quest.name};
        for (const char* step : quest.steps) w.steps.push_back(coop::host::flag_hash(step));
        wants.push_back(std::move(w));
    }
    return wants;
}

bool LiveOn() {
    return !EnvOff("LICHII_COOP_QUEST_LIVE");
}

void AddShared(std::optional<coop::host::SharedFlags>& shared) {
    const bool on = On();
    const std::size_t added = Merge(shared, on);
#ifdef LICHII_COOP
    LOG_INFO(Core, "coop: quests: sharing {} quests (Threads of Time, and Epona won){}", added,
             on ? "" : " (off: LICHII_COOP_QUESTS=0)");
#else
    (void)added;
#endif
}

std::optional<coop::protocol::FlagEntry> Store::read(coop::host::FlagKey key) {
    const Quest* quest = Plain(key) ? Find(key.hash) : nullptr;
    if (quest == nullptr) {
        return inner_.read(key);
    }
    const auto value = get_(quest->hash);
    if (!value) {
        return std::nullopt;
    }
    const auto rank = Rank(*quest, *value);
    if (!rank) {
        return std::nullopt;  // a value not in the list: left alone
    }
    const std::int32_t shown = quest->complete_only && *rank != Last(*quest) ? 0 : *rank;
    return coop::protocol::FlagEntry{key, coop::protocol::FlagKind::int32,
                                     static_cast<std::uint32_t>(shown)};
}

namespace {

// LonLonRanch_PlayingGambleRace (GameDataList Bool): Get_Epona's own flow sets it as a race against Ingo
// starts (Sub_Get_Epona_GambleRace_Start, Sub_Get_Epona_RambleRace_Ready) and clears it as each race ends
// (Sub_Get_Epona_RambleRace_Clean, Sub_Get_Epona_GambleRace_End).
constexpr std::uint32_t kRaceRunning = 0xa253721cu;

} // namespace

// A complete_only quest our game is finishing itself: at one of its last two steps before the end
// (GambleRacePlaying, AfterWonGambleRace for Get_Epona) while its race runs in our game. Row 150: those
// steps alone held the group's end for good: Dm_LonLon_0040 (Ingo beaten) joined in another player's
// ranch left that game at AfterWonGambleRace with no race and no horse, so Complete never came. Unread
// counts as running (held, tried again).
bool Store::racing(const Quest& quest, std::optional<std::int32_t> now) const {
    if (!quest.complete_only || !now || *now == Last(quest) || *now < Last(quest) - 2) return false;
    if (!get_bool_) return true;
    return get_bool_(kRaceRunning).value_or(true);
}

EponaCheck CheckEpona(const ReadHorse& read) {
    EponaCheck out{EponaHorse::full, 0};
    bool free_seen = false;
    for (std::uint32_t i = 0; i < kOwnedHorses; ++i) {
        const auto name = read(i);
        if (!name) return EponaCheck{};
        if (*name == kEponaActor) return EponaCheck{EponaHorse::owned, i};
        if (name->empty() && !free_seen) {
            free_seen = true;
            out = EponaCheck{EponaHorse::missing, i};
        }
    }
    return out;
}

bool Store::put(const Quest& quest, const coop::protocol::FlagEntry& entry, bool exact) {
    if (entry.kind != coop::protocol::FlagKind::int32) {
        ++refused_;
        return false;
    }
    std::int32_t want = static_cast<std::int32_t>(entry.value);
    const auto value = ValueAt(quest, want);
    if (!value) {
        ++refused_;
        return false;
    }
    const auto now_value = get_(quest.hash);
    const auto now = now_value ? Rank(quest, *now_value) : std::nullopt;
    if (quest.complete_only) {
        if (want != Last(quest)) {
            return true;  // only the end is shared: a save load or a lower value leaves ours
        }
        if (racing(quest, now)) {
            // In the race, or riding out after winning it: left to finish, and
            // held (busy) so it is tried again: a lost race goes back below.
            return false;
        }
        // Row 294: Complete without the horse left a game that could never win her (at Complete the
        // field Epona is gone and Ingo no longer races). The horse is registered first, the end after.
        if (quest.hash == kGetEpona && now && *now != Last(quest) && own_horse_ && !own_horse_()) {
            return false;  // not registered yet: tried again
        }
        exact = false;  // never written blind, never over a value not in the list
    }
    if (!exact) {
        if (!now_value) {
            return false;  // unread (memory, GameData not up): not written blind
        }
        if (!now) {
            return true;  // a value not in the list: left alone, like read()
        }
        if (*now >= want) {
            return true;  // ours is as far or further: kept
        }
        if (*now < quest.floor) {
            // Before the quest's floor (Goto_HyruleCastle before Step01: still
            // in the opening): ours, left alone. Once our game reaches the
            // floor and sends it, the host's higher value comes back.
            return true;
        }
    }
    if (now && *now == want) {
        return true;
    }
    if (!set_(quest.hash, *value)) {
        return false;
    }
    raised_.push_back(Raised{quest.name, now ? quest.steps[static_cast<std::size_t>(*now)] : "?",
                             quest.steps[static_cast<std::size_t>(want)]});
    return true;
}

bool Store::write(const coop::protocol::FlagEntry& entry) {
    const Quest* quest = Plain(entry.key) ? Find(entry.key.hash) : nullptr;
    return quest != nullptr ? put(*quest, entry, false) : inner_.write(entry);
}

bool Store::write_exact(const coop::protocol::FlagEntry& entry) {
    const Quest* quest = Plain(entry.key) ? Find(entry.key.hash) : nullptr;
    return quest != nullptr ? put(*quest, entry, true) : inner_.write_exact(entry);
}

bool Store::write_load_back(const coop::protocol::FlagEntry& entry) {
    const Quest* quest = Plain(entry.key) ? Find(entry.key.hash) : nullptr;
    return quest != nullptr ? put(*quest, entry, true) : inner_.write_load_back(entry);
}

bool Store::busy(coop::host::FlagKey key) {
    const Quest* quest = Plain(key) ? Find(key.hash) : nullptr;
    if (quest != nullptr && quest->complete_only) {
        const auto value = get_(quest->hash);
        if (racing(*quest, value ? Rank(*quest, *value) : std::nullopt)) return true;
    }
    return inner_.busy(key);
}

bool Store::is_max(coop::host::FlagKey key) {
    return (Plain(key) && Find(key.hash) != nullptr) || inner_.is_max(key);
}

std::vector<Raised> Store::take_raised() {
    return std::exchange(raised_, {});
}

} // namespace CoopQuests
