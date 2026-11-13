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
//     items;
//   - DoorOfTime (time travel stays each game's own, docs/flags.md), AllSage
//     (the Chamber of Sages; its Step01_00_01 to _03 are not in an order),
//     Defeat_Ganon and EscapeFromCrumblingGanonCastle (the final battle).
//   - for now, Get_StoneOfFire, SageOfShadow, SageOfSpirit and SageOfWater:
//     their RawValues look out of order or branched (KokiriForestFire before
//     KokiriForestIn, GotGlassTruth before GetGlassTruth, Step07 before
//     Step07_pre00, Step07_1_Safety), and with the max rule a wrong order skips
//     a step. Back in once checked against their event flows.
//
// Get_Epona (0x92431c55, not listed in the tab) is shared by its end only
// (Quest::complete_only): its steps are one player's own (the song as a
// child, the race against Ingo, which goes back to ReadyForGambleRace when
// lost), but once one player has won Epona every game's quest is Complete,
// so every adult can call and ride their own Epona (each game has its own
// horse: docs/puppet.md, "Riding"). A game in the race itself
// (GambleRacePlaying, AfterWonGambleRace) is left to finish it.
//
// Also writing quest Enums: CoopQuest (coop_quest.cpp, time-story, issue 173)
// raises DoorOfTime from our Goto_HyruleCastle; both only raise.
const std::vector<Quest>& Table() {
    static const std::vector<Quest> kQuests = {
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

// A complete_only quest our game is finishing itself (its last two steps
// before the end: GambleRacePlaying, AfterWonGambleRace for Get_Epona).
bool Racing(const Quest& quest, std::optional<std::int32_t> now) {
    return quest.complete_only && now && *now != Last(quest) && *now >= Last(quest) - 2;
}

} // namespace

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
        if (Racing(quest, now)) {
            // In the race, or riding out after winning it: left to finish, and
            // held (busy) so it is tried again: a lost race goes back below.
            return false;
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

bool Store::busy(coop::host::FlagKey key) {
    const Quest* quest = Plain(key) ? Find(key.hash) : nullptr;
    if (quest != nullptr && quest->complete_only) {
        const auto value = get_(quest->hash);
        if (Racing(*quest, value ? Rank(*quest, *value) : std::nullopt)) return true;
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
