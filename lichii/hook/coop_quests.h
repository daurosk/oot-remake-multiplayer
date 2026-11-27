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
#include <string>
#include <vector>

#include "coop/host/flags.hpp"
#include "coop_quest.h"

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

// Row 294: owning Epona is not Get_Epona's step but the save's OwnedHorseList (GameDataList Struct
// 0x4920b64f, 10 horses, saved). Its only writer is Animal_Epona_Adult.EventCopyAndRegisterOwnedHorse in
// Get_Epona.bfevfl's escape subflows, just before Complete. Epona's Song (EventQueryHasOwnedHorse), Gerudo
// Valley's bridge (EventTriggerRequestOwnedHorseForceChangeBanc) and horseback archery use the owned horse.
// In two real won saves (the owner's wrld_03, a defeated-Ganon save) the registration left exactly one
// trace in the save: ActorName[0] = "Animal_Epona_Adult"; every other member (Name, Mane, Saddle, Reins,
// ColorType, FootType, HorseType, Speed, HorsePower, RoomID, ...) is at its GameDataList default.
inline constexpr std::uint32_t kGetEpona = 0x92431c55u;          // Enum Step_Get_Epona
inline constexpr std::uint32_t kGetEponaComplete = 0x4c0a63f4u;  // its Complete
inline constexpr std::uint32_t kOwnedHorseActorName = 0x7bde80e9u;  // OwnedHorseList.ActorName, String64Array
inline constexpr std::uint32_t kOwnedHorses = 10;                   // its TotalSize
inline constexpr const char* kEponaActor = "Animal_Epona_Adult";

enum class EponaHorse {
    owned,    // her name is in the list
    missing,  // not there; `free` is the first empty element
    full,     // not there and no empty element (10 other horses): left alone
    unread,   // an element could not be read: nothing decided
};
struct EponaCheck {
    EponaHorse state = EponaHorse::unread;
    std::uint32_t free = 0;
};
using ReadHorse = std::function<std::optional<std::string>(std::uint32_t index)>;
EponaCheck CheckEpona(const ReadHorse& read);

// Adds the quests to `shared` when `on`, making the list when there is none.
// Returns how many it added.
std::size_t Merge(std::optional<coop::host::SharedFlags>& shared, bool on);

// False when LICHII_COOP_QUESTS=0.
bool On();

// Merge, on unless LICHII_COOP_QUESTS=0; logs what it did.
void AddShared(std::optional<coop::host::SharedFlags>& shared);

// Issue 94: the shared quests' steps for the game's own ChallengeMgr. The
// watcher writes another player's step into our GameData Enum at once, but the
// Threads of Time tab and the game's logic read the manager, which takes the
// GameData steps only when a save loads: a step got by another player showed
// only after a reload. So each shared quest's GameData step also goes to the
// manager (CoopQuest::Challenges, the game's ChallengeMgr::setStep), as
// Get_StoneOfFire and DoorOfTime already do. The challenge is flag_hash of the
// quest's name. Nothing at Ready or below (Ready comes from the manager
// itself) or below the quest's floor; a complete_only quest (Get_Epona) only
// at Complete; a quest CoopQuest already sends (live) is left to it. The
// Challenges guards stay: never lowered, never from NotReady, never into or
// out of a boss fight or minigame step, never while our Link is in an event.
std::vector<CoopQuest::StepWant> LiveWants(const CoopQuest::ReadEnum& value);

// False when LICHII_COOP_QUEST_LIVE=0 (then only CoopQuest's own quests reach
// the manager, as in 0.11.2).
bool LiveOn();

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
    using GetBool = std::function<std::optional<bool>(std::uint32_t hash)>;
    // Row 294: true once this game owns Epona (her name in OwnedHorseList.ActorName), registering her
    // first when she is not there (EnsureEpona); false: not yet (tried again).
    using OwnHorse = std::function<bool()>;

    // `get_bool` reads a saved Bool (row 150: LonLonRanch_PlayingGambleRace, the game's own "a race
    // against Ingo runs"); none: every Get_Epona race step counts as a race running. `own_horse`
    // (row 294): Get_Epona's Complete is written only once it says true; none: written without.
    Store(coop::host::FlagStore& inner, GetEnum get, SetEnum set, GetBool get_bool = nullptr,
          OwnHorse own_horse = nullptr)
        : inner_(inner), get_(std::move(get)), set_(std::move(set)), get_bool_(std::move(get_bool)),
          own_horse_(std::move(own_horse)) {}

    std::optional<coop::protocol::FlagEntry> read(coop::host::FlagKey key) override;
    bool write(const coop::protocol::FlagEntry& entry) override;
    bool write_exact(const coop::protocol::FlagEntry& entry) override;
    bool write_load_back(const coop::protocol::FlagEntry& entry) override;
    bool is_count(coop::host::FlagKey key) override { return inner_.is_count(key); }
    bool is_max(coop::host::FlagKey key) override;
    bool is_one_way(coop::host::FlagKey key) override { return inner_.is_one_way(key); }
    bool holds(coop::host::FlagKey key) override { return inner_.holds(key); }
    // A complete_only quest our game is finishing itself counts as busy, so
    // the end waits (FlagWatcher unwritten_) instead of being dropped. Row 150:
    // only while its race runs in our game (Racing); a game left at a race
    // step with no race (AfterWonGambleRace from a scene another player's win
    // played here, or an ocarina warp out of the ranch) takes the group's end.
    bool busy(coop::host::FlagKey key) override;

    // Steps moved since the last call (for the log).
    std::vector<Raised> take_raised();
    // Writes refused: not an Int, or a step out of range.
    std::uint64_t refused() const { return refused_; }

 private:
    bool put(const Quest& quest, const coop::protocol::FlagEntry& entry, bool exact);
    bool racing(const Quest& quest, std::optional<std::int32_t> now) const;

    coop::host::FlagStore& inner_;
    GetEnum get_;
    SetEnum set_;
    GetBool get_bool_;
    OwnHorse own_horse_;
    std::vector<Raised> raised_;
    std::uint64_t refused_ = 0;
};

} // namespace CoopQuests
