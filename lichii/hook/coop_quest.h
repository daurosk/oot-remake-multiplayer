// SPDX-License-Identifier: GPL-3.0-or-later
//
// Each game's own quests catch up with the shared story (issue 173, docs/flags.md
// "Story and cutscenes"). The story scenes are shared flags (coop_story), but the
// game's quests ("Challenge" table: Goto_HyruleCastle, DoorOfTime, ...) are saved
// Enums each game keeps for itself: their values are hashes of step names, not
// ordered, so they cannot be shared as a max. A scene that moves a quest on
// (Dm_HrlCastle_0020 sets Goto_HyruleCastle Step04, the meeting with Zelda sets
// Goto_HyruleCastle Complete and DoorOfTime Step01) is skipped in a game where
// another player already played it, and that game's quest stays behind: after one
// player met Zelda, the other player's Impa never came (Impa's courtyard schedule
// and her "can be stopped" event belong to DoorOfTime Step01). So each game raises
// its own quests to what the shared scenes say. No emulator code: tested on its
// own (tests/lichii/quest_test.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace CoopQuest {

// A shared story flag (a saved Bool) that, once 1, means the quest is at least
// at `step`.
struct Rule {
    std::uint32_t flag;
    std::uint32_t step;  // Murmur3 of the step name
    const char* why;
};

// One quest: its Enum (hash from GameDataList, matched by its value list), its
// steps in the game's order (GameDataList Values = the Challenge table's Steps,
// NotReady first, Complete last), and its rules.
struct Quest {
    std::uint32_t hash;
    const char* name;
    std::span<const std::uint32_t> steps;
    std::span<const Rule> rules;
    // The lowest step our own value must already have for a raise (index into
    // steps): a game that has not got this far in its own story is left alone.
    std::size_t floor;
    // Raised only when this other quest of ours is at `needs_step` or later
    // (0 = no such need).
    std::uint32_t needs_quest = 0;
    std::uint32_t needs_step = 0;
};

std::span<const Quest> Quests();

// Index of `value` in `quest.steps`, or nullopt for a value we do not know.
std::optional<std::size_t> Rank(const Quest& quest, std::uint32_t value);

using ReadBool = std::function<std::optional<bool>(std::uint32_t)>;
using ReadEnum = std::function<std::optional<std::uint32_t>(std::uint32_t)>;

// The step to write into `quest`, or nothing: the highest step a set rule
// flag gives, if it is above our value. Never lowers; an unread or unknown
// value, a value under `floor` or an unmet need leaves the quest alone.
std::optional<std::uint32_t> CatchUp(const Quest& quest, const ReadBool& flag,
                                     const ReadEnum& value);

} // namespace CoopQuest
