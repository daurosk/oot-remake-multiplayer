// SPDX-License-Identifier: GPL-3.0-or-later
// What an actor's animation name (its AS slot 0, as a walk reads it and samples carry) says it is doing, for the
// rules that must never judge a death or a cutscene as a move: the objects walk (an enemy that left while dying,
// IsDeathAnimation, IsPreBattleAnimation), the native boss table's stall check and the server's (row 149: CoopBossNative::StallJudged,
// Session::note_hold).
#pragma once

#include <cstddef>
#include <string_view>

namespace coop::protocol::anim {

// An animation that plays as the actor dies ("Die", "Dead_Fall", "Damage_Death"): a word Die, Dead or Death at the
// start or after '_', ending there, at '_' or at a digit.
constexpr bool IsDeath(std::string_view name) noexcept {
    for (const std::string_view word : {std::string_view("Die"), std::string_view("Dead"), std::string_view("Death")}) {
        for (std::size_t at = name.find(word); at != std::string_view::npos; at = name.find(word, at + 1)) {
            const bool starts = at == 0 || name[at - 1] == '_';
            const std::size_t end = at + word.size();
            const bool ends = end == name.size() || name[end] == '_' || (name[end] >= '0' && name[end] <= '9');
            if (starts && ends) {
                return true;
            }
        }
    }
    return false;
}

// An animation an event (a cutscene) plays on the actor, never one of its own moves. Names read in the games
// (2026-11-19 lane logs): a cut's "C01-Enemy_DungeonBoss_Gohma-A-0" / "C03_00-...", an AS file's
// "AS-C10-Dm_FireTemple_BossIn_Enemy_DungeonBoss_Volvagia-A-0", and any name with a demo's "Dm_". Own moves
// ("CeilingMove", "Climb_All", "Down_Loop") start with a letter after the C.
constexpr bool IsEvent(std::string_view name) noexcept {
    if (name.starts_with("AS-") || name.find("Dm_") != std::string_view::npos) {
        return true;
    }
    if (name.size() < 3 || name[0] != 'C' || name[1] < '0' || name[1] > '9') {
        return false;
    }
    std::size_t at = 1;
    while (at < name.size() && name[at] >= '0' && name[at] <= '9') {
        ++at;
    }
    return at < name.size() && (name[at] == '-' || name[at] == '_');
}

// A stun or knock-down the actor lies in (its own chance time: Gohma's "Stun_St_Attack_Chance", "Down_Start",
// "Fall_St"): the first word Stun, Down, Fall or Faint, as the hook's stun rule reads it (docs/dungeons-boss.md
// "Stuns"). Boss turns never hand an enemy over in one.
constexpr bool IsStun(std::string_view name) noexcept {
    for (const std::string_view word :
         {std::string_view("Stun"), std::string_view("Down"), std::string_view("Fall"), std::string_view("Faint")}) {
        if (name.starts_with(word) && (name.size() == word.size() || name[word.size()] == '_')) {
            return true;
        }
    }
    return false;
}

// An animation of a boss that waits for its fight to begin (row 189): Gohma's `Sleep` (far from her room's
// middle) and `Pre_Battle_*` (on the ceiling, before her drop), as CoopObjects::IsPreBattleAnimation reads them.
constexpr bool IsPreBattle(std::string_view name) noexcept {
    return name == "Sleep" || name.starts_with("Pre_Battle_");
}

} // namespace coop::protocol::anim
