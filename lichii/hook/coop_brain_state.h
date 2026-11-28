// SPDX-License-Identifier: GPL-3.0-or-later
//
// An actor's move state goes with the actor (row 149, King Dodongo rolling in the lava; sync step S2 `brain-carry`,
// hand-over only; docs/dungeons-boss.md
// "Blackboard move state"). No emulator code: tested in tests/lichii/brain_state_test.cpp.
//
// A boss's AI keeps the state its moves are chosen from in the actor's blackboard, which is each game's own. King
// Dodongo's roll does not follow the walls by itself: it rolls STRAIGHT to a corner number kept there
// (DungeonBossDodongo_CurrentTargetCornerNum, the way round in DungeonBossDodongo_IsReverse; pack
// Enemy_DungeonBoss_Dodongo, AI/DungeonBossDodongo.action.spinattack.module.ainb: SelectNextCornerNumber,
// GetCornerPosFromNumber, OneShotTargetSetFromPosAndRot). The native boss sync (coop_boss_native.h) carried his
// verbs and the mover his place, but never that number: a copy counted corners by itself, and the game made his owner
// rolled to ITS stale corner from wherever he stood, a straight line across the lava pit, where no wall ends the
// roll (run 20261123-043946; row 325's fix removed one way in, not this cause).
//
// So the values go with the boss:
// - the owner's game reads the keys of kKeys its boss has, each frame, and sends them in the boss's samples
//   (OBJECT_SAMPLE component 25 `brain`, coop::protocol::KeyedValues, owner 0; the server keeps them for
//   HANDOVER_STATE);
// - a copy keeps the owner's last values and changes nothing in its own game (its place and verbs are the owner's);
// - a game made the owner puts those values into its own blackboard first, through the game's own setters, called
//   by our caller on the game's calc thread before the boss table's caller (kBrainState < kBossTable), so its brain
//   decides from where the old owner's stood. Until they are in, it sends them on unchanged, never its stale own.
// Generic: a key is an actor's name and a blackboard key's name, and goes on the wire as the hash of the key's
// name, so any actor can be listed in kKeys. Listed now: every boss with such state (Twinrova's battle state is
// carried as orders, coop_boss_orders.h).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_caller_orders.h"
#include "coop_move_state.h"
#include "coop_puppet.h"

namespace CoopBrainState {

// One carried value: the key's number (KeyId of its name) and 32 bits, an s32 as it is, a bool as 0 or 1.
struct BrainValue {
    std::uint32_t key = 0;
    std::int32_t value = 0;
    bool operator==(const BrainValue&) const = default;
};
// An actor's carried values, by rising key.
struct BrainState {
    std::vector<BrainValue> values;
    bool operator==(const BrainState&) const = default;
};
// No actor carries more keys than this.
inline constexpr std::size_t kMaxBrainValues = 32;

// The wire's form (component 25: keyed values, owner 0 for the actor itself, name = the key's number, type s32).
// Ours is always the full list. From the wire: the actor's own s32 values; a part's (another owner), another type
// and a remove are passed over.
coop::protocol::KeyedValues ToKeyed(const BrainState& state);
BrainState FromKeyed(const coop::protocol::KeyedValues& keyed);

namespace addr {
// Build e0f3ccfaf50065ae. An actor's blackboard: [[[actor + 0x270]] + 0x10], none when 0 or -1 (main+0xd6d110).
inline constexpr std::uint64_t kActorComponents = 0x270;
inline constexpr std::uint64_t kHolderBlackboard = 0x10;
// Its s32 group: name count (s32), names (char* array), value count (s32), values (s32 array).
inline constexpr std::uint64_t kS32NameCount = 0x98;
inline constexpr std::uint64_t kS32Names = 0xa0;
inline constexpr std::uint64_t kS32ValueCount = 0xa8;
inline constexpr std::uint64_t kS32Values = 0xb0;
// Its bool group, the same with one byte per value.
inline constexpr std::uint64_t kBoolNameCount = 0x188;
inline constexpr std::uint64_t kBoolNames = 0x190;
inline constexpr std::uint64_t kBoolValueCount = 0x198;
inline constexpr std::uint64_t kBoolValues = 0x1a0;
// The game's setters: x0 the blackboard, x1 a pointer to the key's name pointer, x2 a pointer to the value (s32,
// or one byte). They find the key by name, leave a locked key alone, store and count the change.
inline constexpr std::uint64_t kSetS32 = 0x264f170;  // main+
inline constexpr std::uint64_t kSetBool = 0xd6cb40;  // main+
// Our value block in guest memory (made once with the game's malloc).
inline constexpr std::uint64_t kMalloc = 0x14c6c90;  // main+, as coop_boss_native.h
inline constexpr std::size_t kValueBytes = 0x10;
// Row 287: behind the value, the node block the game's own move-state change is called with (coop_move_state.h).
inline constexpr std::size_t kNodeAt = kValueBytes;
inline constexpr std::size_t kBlockSize = kValueBytes + CoopMoveState::kBlockBytes;
// A group with more names than this is not read (a wrong pointer).
inline constexpr std::int32_t kMaxNames = 1024;
}  // namespace addr

// The kinds carried now (all go as the wire's s32). cct_move (row 287) is no blackboard key: the index of the move
// state the actor's physics character controller is in (coop_move_probe.h's chain), put in through the game's own
// change (coop_move_state.h).
enum class Kind : std::uint8_t { s32, boolean, cct_move };

// A key's number on the wire (a keyed value's name): Murmur3 (x86_32, seed 0) of its name.
constexpr std::uint32_t KeyId(std::string_view name) {
    std::uint32_t h = 0;
    const std::size_t blocks = name.size() / 4;
    const auto byte = [name](std::size_t i) { return static_cast<std::uint32_t>(static_cast<unsigned char>(name[i])); };
    const auto rotl = [](std::uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
    for (std::size_t b = 0; b < blocks; ++b) {
        std::uint32_t k = byte(b * 4) | (byte(b * 4 + 1) << 8) | (byte(b * 4 + 2) << 16) | (byte(b * 4 + 3) << 24);
        k *= 0xcc9e2d51u;
        k = rotl(k, 15);
        k *= 0x1b873593u;
        h ^= k;
        h = rotl(h, 13);
        h = h * 5 + 0xe6546b64u;
    }
    std::uint32_t k = 0;
    const std::size_t tail = blocks * 4;
    switch (name.size() & 3) {
    case 3:
        k ^= byte(tail + 2) << 16;
        [[fallthrough]];
    case 2:
        k ^= byte(tail + 1) << 8;
        [[fallthrough]];
    case 1:
        k ^= byte(tail);
        k *= 0xcc9e2d51u;
        k = rotl(k, 15);
        k *= 0x1b873593u;
        h ^= k;
        break;
    default:
        break;
    }
    h ^= static_cast<std::uint32_t>(name.size());
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}
static_assert(KeyId("") == 0 && KeyId("abc") == 0xb3dd93fau && KeyId("Hello, world!") == 0xc0363e43u);

// One blackboard key that is carried: of any actor, by the actor's name and the key's name.
struct Key {
    std::uint32_t id;       // KeyId(name)
    std::string_view boss;  // the actor's name
    std::string_view name;  // the key's name in its blackboard
    Kind kind;
};
constexpr Key MakeKey(std::string_view actor, std::string_view name, Kind kind) {
    return Key{KeyId(name), actor, name, kind};
}

// Row 287: every actor of the walk carries its CCT move state (an actor with no CCT carries none). A boss's moves
// change it in steps (Phantom Ganon's fall-down: 1 at its start, back to 2 only in its last step); a copy's own AI
// runs those moves cut short, so its body can be left in another state than its owner's, which shows only when
// the copy is made ours: its body then obeys that state (he fell through the floor). The name is ours, not a
// blackboard key's; an older build passes the value over (Plan: no key of its own has that number).
inline constexpr Key kCctMoveKey = MakeKey("", "Coop_PhysicsCctMoveState", Kind::cct_move);

// What a boss chooses its next move from: counters, indexes, the way round, a phase, a forced next move. Not the
// one-frame triggers its actions raise from its own contacts, not what follows from its place or animation (those
// are the copy's own, from the owner's place and verbs), no actor or perception ids (each game's own).
inline constexpr std::array kKeys{
    // King Dodongo: the corner his roll goes to and the way round; a baby Dodongo spawn owed after the roll.
    MakeKey("Enemy_DungeonBoss_Dodongo", "DungeonBossDodongo_CurrentTargetCornerNum", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Dodongo", "DungeonBossDodongo_IsReverse", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Dodongo", "DungeonBossDodongo_IsAfterRollingSpawn", Kind::boolean),
    // Volvagia: the battle state, the hole it holds, its ground moves in a row, the forced sky attack, the turn.
    MakeKey("Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_BattleState", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_ContinuousGroundActionNum", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_HoldingAppearHoleIndex", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_ForceSkyAttackType", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_IsTurnLeft", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_IsActFirstSkyAttack", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_IsInfinitySkyAttack", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_IsAppearFromCenter", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_IsOnceAppearSign", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Volvagia", "DungeonBossVolvagia_IsActFallRockOnce", Kind::boolean),
    // Gohma: the egg she lays next, the life her ground mode began at, the battle she is forced into.
    MakeKey("Enemy_DungeonBoss_Gohma", "DungeonBossGohma_EggIndex", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Gohma", "DungeonBossGohma_GroundModeEnterLife", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Gohma", "DungeonBossGohma_IsForceLandBattle", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Gohma", "DungeonBossGohma_IsForceClimb", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Gohma", "DungeonBossGohma_IsForceCeilingBattle", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Gohma", "DungeonBossGohma_IsClimbFromOutside", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Gohma", "DungeonBossGohma_IsJumpOffImmediately", Kind::boolean),
    // Phantom Ganon: the light ball rally, the aura, his hits taken on the ground, the forced special attack.
    MakeKey("Enemy_DungeonBoss_PhantomGanon", "Ganondorf_Common_ShootLioghtBallIndex", Kind::s32),
    MakeKey("Enemy_DungeonBoss_PhantomGanon", "Ganondorf_Common_CurrentDarknessAuraType", Kind::s32),
    MakeKey("Enemy_DungeonBoss_PhantomGanon", "DamageCountInLandBattle", Kind::s32),
    MakeKey("Enemy_DungeonBoss_PhantomGanon", "Ganondorf_Common_IsCompletedFirstRally", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_PhantomGanon", "Ganondorf_Common_IsFirstAttackSecondHalfBattle", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_PhantomGanon", "Ganondorf_Common_IsDoFirstAttackAfterForceSpecialAttack",
        Kind::boolean),
    MakeKey("Enemy_DungeonBoss_PhantomGanon", "Ganondorf_Common_IsForceSpecialAttack", Kind::boolean),
    // Ganondorf: the same common state, his own light ball index, the final phase and its attack count.
    MakeKey("Enemy_DungeonBoss_Ganondorf", "Ganondorf_Common_ShootLioghtBallIndex", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Ganondorf", "Ganondorf_Common_CurrentDarknessAuraType", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBossGanondorf_ShootLioghtBallIndex", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBossGanondorf_FinalPhaseAttackActionNum", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Ganondorf", "Ganondorf_Common_IsCompletedFirstRally", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Ganondorf", "Ganondorf_Common_IsFirstAttackSecondHalfBattle", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Ganondorf", "Ganondorf_Common_IsDoFirstAttackAfterForceSpecialAttack",
        Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Ganondorf", "Ganondorf_Common_IsForceSpecialAttack", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBoss_Ganondorf_UseForceSpecialAttack", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBoss_Ganondorf_IsFinalPhaseStart", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Ganondorf", "Enemy_DungeonBoss_Ganondorf_IsUnlockedBasicAttack", Kind::boolean),
    // Morpha: the half phase, its attack counters and which tentacle attack is next.
    MakeKey("Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_CheapCount", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_NextTentacleAttack", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_NowTentacleAttack", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_IsHalfPhase", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_IsHalfPhaseStart", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_IsHalfPhaseStartExecuted", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_IsFirstAttackExeced", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_IsSlamCleared", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Morpha", "Enemy_DungeonBoss_Morpha_IsSpinCleared", Kind::boolean),
    // Bongo Bongo: the attack he is forced to next and with which hand.
    MakeKey("Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_BongoBongo_ForceAttackCommand", Kind::s32),
    MakeKey("Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_BongoBongo_IsForceRightHand", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_BongoBongo_IsForceLeftHand", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_BongoBongo", "Enemy_DungeonBoss_BongoBongo_IsForceWait", Kind::boolean),
    // Ganon: his sequence, the final rush's hits, each weak point's life and whether it is broken, his charge.
    MakeKey("Enemy_DungeonBoss_Ganon", "CurrentSequenceType", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Ganon", "DungeonBossGanon_FinalRushCurrentHitNum", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Ganon", "LifeAtChanceWaitStart", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Ganon", "DungeonBossGanon_RightHandWeakPointLife", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Ganon", "DungeonBossGanon_LeftHandWeakPointLife", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Ganon", "DungeonBossGanon_ThirdEyeWeakPointLife", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Ganon", "DungeonBossGanon_IsBrokenRightHandWeakPoint", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Ganon", "DungeonBossGanon_IsBrokenLeftHandWeakPoint", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Ganon", "DungeonBossGanon_IsBrokenThirdEyeWeakPoint", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Ganon", "DungeonBossGanon_IsForbidChargeAttack", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Ganon", "DungeonBossGanon_CanRushAttack", Kind::boolean),
    // Barinade: its form, the jellyfish move and the arms left.
    MakeKey("Enemy_DungeonBoss_Barinade", "CurrentFormType", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Barinade", "CurrentJellyfishActionType", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Barinade", "RemainArmCount", Kind::s32),
    MakeKey("Enemy_DungeonBoss_Barinade", "IsBeamAttackAfterChanceTime", Kind::boolean),
    MakeKey("Enemy_DungeonBoss_Barinade", "IsKnockBackAfterChanceTime", Kind::boolean),
};

// An actor's keys stand in one run, no more than one component holds, and no two of them share a hash.
constexpr bool KeysFit() {
    std::size_t start = 0;
    for (std::size_t i = 0; i < kKeys.size(); ++i) {
        if (i != 0 && kKeys[i].boss != kKeys[i - 1].boss) start = i;
        if (i - start + 2 > kMaxBrainValues) return false;  // (+ kCctMoveKey)
        if (kKeys[i].id == kCctMoveKey.id) return false;
        for (std::size_t j = start; j < i; ++j) {
            if (kKeys[j].id == kKeys[i].id) return false;
        }
    }
    return true;
}
static_assert(KeysFit());

// The keys of one actor, in id order, the wire's: its blackboard keys of kKeys and kCctMoveKey.
std::vector<Key> KeysOf(std::string_view boss);

// The planner, without game memory (tested): which of the old owner's values a game made the owner puts in, in
// order. Only keys of this boss, each once; a value our game already has is left alone (`ours`: what our blackboard
// holds now, by key id; a key not in it could not be read and is passed over). A bool goes in as 0 or 1. The CCT
// move state goes in by CoopMoveState::AtMadeOurs (never a state the game has no slot for).
std::vector<BrainValue> Plan(std::span<const Key> keys, const BrainState& theirs, const BrainState& ours);

enum class Role : std::uint8_t { owner, copy };

// A boss the walk saw this round (coop_objects.cpp, with the native boss list).
struct Member {
    std::uint64_t actor = 0;
    Role role = Role::owner;
    std::uint8_t owner = 0;
    std::string name;
};

struct Counts {
    std::uint64_t handed = 0;    // bosses made ours whose old owner's values were looked at
    std::uint64_t put = 0;       // values put into our blackboard (they differed)
    std::uint64_t same = 0;      // values our game already had
    std::uint64_t refused = 0;   // values the game's setter left alone (a locked key)
    std::uint64_t unread = 0;    // a boss whose blackboard or keys were not found
    std::uint64_t moves = 0;     // row 287: CCT move states put in through the game's change (counted in `put` too)
};

class Table {
public:
    static constexpr int kCallerOrder = CoopCallerOrders::kBrainState;

    // The walk's bosses this round. One no longer listed is forgotten. A copy made ours: the old owner's last
    // values are put in first (Step).
    void Publish(std::span<const Member> members);

    // The owner's values from its samples or the hand-over state (coop_objects_sync.cpp BrainStateIn). For a copy
    // they are kept; for one of ours (a hand-over state that came after the news) they are put in as well.
    void Receive(std::uint64_t actor, const BrainState& board);

    // Ours: its values for its samples. While the old owner's are still to put in: those. Nothing: no key of it is
    // carried, or none was read yet.
    std::optional<BrainState> OwnerBoard(std::uint64_t actor) const;

    // The caller (coop_callers.h contract): reads our bosses' values; puts the old owner's into a boss made ours.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main_base);

    // A new game process: every boss, the block and any call in flight are dropped.
    void Reset();

    Counts GetCounts() const;
    std::vector<std::string> TakeNotes();
    // Bosses of ours whose hand-over put in at least one value that differed from ours (since the last call): their AI
    // may go on with a move chosen from the stale ones (CoopBossNative::Table::StateChanged).
    std::vector<std::uint64_t> TakeChanged();

    // Row 323 probe (LICHII_COOP_DEBUG_BOSS only; reads, never writes): Gohma's climb keys that kKeys does not
    // carry, found by scanning her blackboard's groups for their names (so the vec3 group's place is read, not
    // assumed), noted at each owner change and once a second, on owner and copy alike.
    void SetProbe(bool on);

private:
    struct ProbeKey {
        std::string_view name;
        std::uint64_t group = 0;  // the group's offset in the blackboard (0: not found)
        std::uint32_t index = 0;
    };
    struct ProbeEntry {
        std::uint64_t blackboard = 0;
        std::vector<ProbeKey> keys;
        Role role = Role::copy;
        std::uint32_t owner = 0;
        bool seen = false;
        std::string last;  // the last values noted
    };
    void ProbeStep(CoopPuppet::Guest& guest);  // (mutex_ held)
    bool probe_ = false;
    std::uint64_t probe_frames_ = 0;
    std::map<std::uint64_t, ProbeEntry> probes_;  // by actor
    struct Slot {
        Key key;
        std::uint64_t name_at = 0;  // where the group's names array holds this key's name pointer (0: not found)
        std::uint32_t index = 0;
    };
    struct Entry {
        Member member;
        std::vector<Slot> slots;         // its keys (KeysOf), found in `blackboard`
        std::uint64_t blackboard = 0;    // the one the slots were found in (0: not yet)
        bool unread_said = false;
        BrainState in;                    // the owner's last values (a copy's; kept when made ours)
        bool due = false;                // ours: `in` is still to put in
        std::vector<BrainValue> todo;  // the plan being put in, front first
        bool planned = false;
        std::optional<BrainState> mine;   // ours: as last read
        std::uint32_t put = 0;           // this hand-over's counts, for its note
        std::uint32_t same = 0;
        bool board_put = false;          // a blackboard value was put in (TakeChanged; not the CCT move state)
    };
    void Note(std::string text);
    void DropGame();  // (mutex_ held)
    // Finds what the entry carries: its blackboard and its keys in it, and its CCT (false: neither now). Reads only.
    bool Find(CoopPuppet::Guest& guest, Entry& e);
    bool FindBoard(CoopPuppet::Guest& guest, Entry& e);
    // The index of the move state the actor's CCT is in (nothing: no CCT, or a link of its chain is not there).
    static std::optional<std::int32_t> MoveOf(CoopPuppet::Guest& guest, std::uint64_t actor);
    static std::optional<BrainState> ReadValues(CoopPuppet::Guest& guest, const Entry& e);

    mutable std::mutex mutex_;
    std::map<std::uint64_t, Entry> entries_;  // by actor
    Counts counts_{};
    std::vector<std::string> notes_;
    std::vector<std::uint64_t> changed_;  // (TakeChanged)
    std::uint64_t main_base_ = 0;
    std::uint64_t block_ = 0;
    bool allocating_ = false;
    bool malloc_failed_ = false;
    enum class Pending : std::uint8_t { none, malloc, set } pending_ = Pending::none;
    std::uint64_t pending_actor_ = 0;
};

Table& TheTable();

}  // namespace CoopBrainState
