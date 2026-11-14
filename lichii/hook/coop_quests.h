// SPDX-License-Identifier: GPL-3.0-or-later
//
// Threads of Time, the same for every player (issue 193, docs/flags.md
// "Threads of Time"). The pause menu's "Threads of Time" tab is the game's
// quest log: each quest of RSDB/Challenge is one saved Enum (ResetTypeValue 8)
// whose values are, in order, NotReady, the quest's steps, Complete. The tab
// lists the quests and the step each one is at.
//
// An Enum's value is the hash of a step's name, which has no order of its
// own. This file gives the shared quests their order: Store shows each one to
// the flag watcher as an Int, the step's place in the list (0 = NotReady), so
// the existing "keep the higher value" rule (FlagStore::is_max) shares them:
// every player's quest is at the furthest step any player reached, a guest who
// joins gets the group's steps, and the host's save load sets them exactly
// (FLAG_RESET), like any shared flag. No new message type.
//
// No emulator code: the game's Enum is read and written through the two
// functions given to Store, so this is tested on its own.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include "coop/host/flags.hpp"

namespace CoopQuests {

// One shared quest: its Enum's hash (GameDataList), its name in the Challenge
// table (for logs) and its values in order.
struct Quest {
    std::uint32_t hash;
    const char* name;
    std::vector<const char*> steps;  // NotReady, ..., Complete
    // A game whose quest is below this step takes no other player's step (a
    // normal write; the host's save load still sets it). Goto_HyruleCastle:
    // Step01 (2), set when Link leaves the Deku Tree, so a player still in the
    // opening is not moved on (time-story's CoopQuest DoorOfTime rule reads
    // our Goto, reports/review-time-story.md).
    std::int32_t floor = 0;
    // Only the end is shared (Get_Epona): a game not at Complete reads as
    // NotReady, and only Complete is ever written. The steps before it stay
    // each game's own.
    bool complete_only = false;
};

// Every shared quest, sorted by hash.
std::span<const Quest> Quests();

// The quest of `hash`, or nullptr.
const Quest* Find(std::uint32_t hash);

// The place of `value` (a step name's hash) in `quest`, or nothing.
std::optional<std::int32_t> Rank(const Quest& quest, std::uint32_t value);

// The value (step name's hash) at `rank`, or nothing out of range.
std::optional<std::uint32_t> ValueAt(const Quest& quest, std::int32_t rank);

// Adds the quests to `shared` when `on`, making the list when there is none.
// Returns how many it added.
std::size_t Merge(std::optional<coop::host::SharedFlags>& shared, bool on);

// False when LICHII_COOP_QUESTS=0.
bool On();

// Merge, on unless LICHII_COOP_QUESTS=0; logs what it did.
void AddShared(std::optional<coop::host::SharedFlags>& shared);

// A step one game's quest was moved to by another player's progress.
struct Raised {
    const char* quest;
    const char* from;  // "?" when the old value was unread
    const char* to;
};

// The flag store the watcher sees: a shared quest reads as an Int (its step's
// place) and is written back as the step's hash, never lowered except by
// write_exact (the host's save load). A write is not taken while our game's
// value is unread (false: tried again) or not in the list (left alone), or
// while our quest is below its floor. Everything else goes to `inner`.
class Store final : public coop::host::FlagStore {
 public:
    using GetEnum = std::function<std::optional<std::uint32_t>(std::uint32_t hash)>;
    using SetEnum = std::function<bool(std::uint32_t hash, std::uint32_t value)>;

    Store(coop::host::FlagStore& inner, GetEnum get, SetEnum set)
        : inner_(inner), get_(std::move(get)), set_(std::move(set)) {}

    std::optional<coop::protocol::FlagEntry> read(coop::host::FlagKey key) override;
    bool write(const coop::protocol::FlagEntry& entry) override;
    bool write_exact(const coop::protocol::FlagEntry& entry) override;
    bool is_count(coop::host::FlagKey key) override { return inner_.is_count(key); }
    bool is_max(coop::host::FlagKey key) override;
    bool is_one_way(coop::host::FlagKey key) override { return inner_.is_one_way(key); }
    bool holds(coop::host::FlagKey key) override { return inner_.holds(key); }
    // A complete_only quest our game is finishing itself counts as busy, so
    // the end waits (FlagWatcher unwritten_) instead of being dropped.
    bool busy(coop::host::FlagKey key) override;

    // Steps moved since the last call (for the log).
    std::vector<Raised> take_raised();
    // Writes refused: not an Int, or a step out of range.
    std::uint64_t refused() const { return refused_; }

 private:
    bool put(const Quest& quest, const coop::protocol::FlagEntry& entry, bool exact);

    coop::host::FlagStore& inner_;
    GetEnum get_;
    SetEnum set_;
    std::vector<Raised> raised_;
    std::uint64_t refused_ = 0;
};

} // namespace CoopQuests
