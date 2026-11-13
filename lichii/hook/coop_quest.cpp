// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_quest.h"

#include <array>

namespace CoopQuest {
namespace {

// Murmur3 x86 32, seed 0, of each step name.
constexpr std::uint32_t kNotReady = 0x57b849f0u;
constexpr std::uint32_t kReady = 0x17bfaa46u;
constexpr std::uint32_t kStep01 = 0xeb62add9u;
constexpr std::uint32_t kStep02 = 0x0e5afad8u;
constexpr std::uint32_t kStep02_01 = 0x608637dfu;
constexpr std::uint32_t kStep2_1 = 0xd984a45eu;
constexpr std::uint32_t kSrep2_1_1 = 0x38170256u;  // sic, the game's name
constexpr std::uint32_t kStep2_2 = 0xb5294bddu;
constexpr std::uint32_t kStep03 = 0x6f917dfeu;
constexpr std::uint32_t kStep03_01 = 0x14376953u;
constexpr std::uint32_t kStep03_02 = 0xd932a31cu;
constexpr std::uint32_t kStep04 = 0x27f660feu;
constexpr std::uint32_t kStep05 = 0x5cde171fu;
constexpr std::uint32_t kStep06 = 0x21d6fa38u;
constexpr std::uint32_t kComplete = 0x4c0a63f4u;

// Shared story flags (coop_story.cpp).
constexpr std::uint32_t kOwlToCastle = 0x98bf10c1u;  // HavePlayedEvent.HyruleField_Goto_HyruleCastle_Kaepora
constexpr std::uint32_t kOwlScene = 0xd4ef7853u;     // HavePlayedEvent.Dm_HrlField_0010
constexpr std::uint32_t kTalonWoke = 0xb9d7d709u;    // HavePlayedEvent.Dm_HrlCastle_0020
constexpr std::uint32_t kImpaCame = 0x8e15ffd6u;     // HavePlayedEvent.Dm_HrlCastle_0060

constexpr std::uint32_t kGotoHyruleCastle = 0xdb89c1a9u;
constexpr std::uint32_t kDoorOfTime = 0x378913cau;

constexpr std::array kGotoSteps{kNotReady, kReady,     kStep01,    kStep02, kStep02_01,
                                kStep03,   kStep03_01, kStep03_02, kStep04, kComplete};
constexpr std::array kDoorSteps{kNotReady, kReady,  kStep01, kStep02, kStep2_1, kSrep2_1_1,
                                kStep2_2,  kStep03, kStep04, kStep05, kStep06,  kComplete};

// Goto_HyruleCastle: the owl at the field entrance (event flow
// Goto_HyruleCastle_Kaepora_00) gives Step02; Talon woken (Dm_HrlCastle_0020)
// Step04; the meeting with Zelda (Goto_HyruleCastle_MeetZelda: scenes 0030,
// 0035, 0045, 0050, then Impa's 0060) Complete.
constexpr std::array kGotoRules{
    Rule{kOwlToCastle, kStep02, "the owl at the Hyrule Field entrance"},
    Rule{kOwlScene, kStep02, "the owl at the Hyrule Field entrance"},
    Rule{kTalonWoke, kStep04, "Talon woken at the castle"},
    Rule{kImpaCame, kComplete, "Zelda met in the courtyard"},
};
// DoorOfTime: the same meeting gives Step01 (Impa waits in the courtyard and
// stops Link when he leaves, then takes him out: Zelda's Lullaby). Nothing
// later is raised: each player then meets Impa, who moves the quest on.
constexpr std::array kDoorRules{
    Rule{kImpaCame, kStep01, "Zelda met in the courtyard (Impa comes)"},
};

constexpr std::array kQuests{
    // Goto_HyruleCastle only after our own Deku Tree (Step01 comes from the
    // escape from the dungeon): never for a game still before it.
    Quest{kGotoHyruleCastle, "Goto_HyruleCastle", kGotoSteps, kGotoRules, 2},
    // DoorOfTime is NotReady until Goto_HyruleCastle is complete; the
    // meeting with Zelda moves it from there.
    Quest{kDoorOfTime, "DoorOfTime", kDoorSteps, kDoorRules, 0, kGotoHyruleCastle, kComplete},
};

} // namespace

std::span<const Quest> Quests() {
    return kQuests;
}

std::optional<std::size_t> Rank(const Quest& quest, std::uint32_t value) {
    for (std::size_t i = 0; i < quest.steps.size(); ++i) {
        if (quest.steps[i] == value) return i;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> CatchUp(const Quest& quest, const ReadBool& flag,
                                     const ReadEnum& value) {
    const auto now = value(quest.hash);
    if (!now) return std::nullopt;
    const auto rank = Rank(quest, *now);
    if (!rank || *rank < quest.floor) return std::nullopt;
    if (quest.needs_quest != 0) {
        const Quest* other = nullptr;
        for (const auto& q : kQuests) {
            if (q.hash == quest.needs_quest) other = &q;
        }
        const auto have = value(quest.needs_quest);
        const auto have_rank = other && have ? Rank(*other, *have) : std::nullopt;
        const auto need_rank = other ? Rank(*other, quest.needs_step) : std::nullopt;
        if (!have_rank || !need_rank || *have_rank < *need_rank) return std::nullopt;
    }
    std::optional<std::size_t> best;
    for (const auto& r : quest.rules) {
        if (flag(r.flag) != true) continue;
        const auto to = Rank(quest, r.step);
        if (to && (!best || *to > *best)) best = to;
    }
    if (!best || *best <= *rank) return std::nullopt;
    return quest.steps[*best];
}

} // namespace CoopQuest
