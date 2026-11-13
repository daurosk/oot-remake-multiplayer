// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native boss sync on Hookshot (docs/dungeons-boss.md, "Native boss
// sync"). No emulator code: tested in tests/lichii/boss_native_test.cpp.
//
// A boss is an enemy with the engine's BSA component (brain, sequence,
// action). Its brain AI picks a brain verb (the phase: Gohma's land battle,
// ceiling battle, wander) and asks for action verbs (walk, climb, attack,
// chance time, spawn), each through one game function:
//   set brain verb       main+0x6733b0 (x0 BSA, w1 verb, w2 force, x3 node)
//   request action verb  main+0xc08f0  (x0 BSA, x1 request, w2 changed)
// Both are wrapped (Hookshot's hook table, ids in hos_coop_bosses.h).
//
// The owner's game runs the boss: each change it makes is kept here and sent
// in the boss's samples (OBJECT_SAMPLE component 20, coop::protocol::BossBsa).
// A copy (another player's boss) gets the owner's changes through the same
// two functions, called by our caller on the game's calc thread; its own
// brain's changes are skipped at the wrap (the function does not run), once
// the owner's first state has come. So every game runs the same phases and
// action sequences: the game's own animations, movement and attacks.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_puppet.h"

namespace CoopBossNative {

using coop::protocol::BossAction;
using coop::protocol::BossBsa;
using coop::protocol::BossBrain;
using coop::protocol::BossChange;

namespace addr {
inline constexpr std::uint64_t kActorComponents = 0x270;  // actor -> component table
inline constexpr std::uint64_t kComponentBsa = 0x188;     // table -> BSA component
inline constexpr std::uint64_t kBsaBrainVerb = 0x28;      // s32, the current brain verb
inline constexpr std::uint64_t kSetBrainVerb = 0x6733b0;       // main+
inline constexpr std::uint64_t kRequestActionVerb = 0xc08f0;   // main+
inline constexpr std::uint64_t kMalloc = 0x14c6c90;            // main+, as coop_puppet.h
// What kRequestActionVerb returns for a request it took (its main path,
// main+0xc10f4; 2 and 3 are refusals). A skipped request answers the same.
inline constexpr std::uint64_t kActionTaken = 0;

// The action request (0x60 bytes, built on the asker's stack).
inline constexpr std::size_t kRequestSize = 0x60;
inline constexpr std::size_t kReqPlaces = 0x00;   // 3 x Vec3; the first float NaN: no place
inline constexpr std::size_t kReqKind = 0x24;     // u32 4 (checked by the game)
inline constexpr std::size_t kReqKind2 = 0x28;    // u32 0x11a (checked)
inline constexpr std::size_t kReqExtra = 0x2c;    // 2 x f32, NaN: none
inline constexpr std::size_t kReqKind3 = 0x34;    // u32, 0x10 as the AI nodes make it
inline constexpr std::size_t kReqPlug = 0x38;     // a plug pointer, used when the byte at 0x40 is 1
inline constexpr std::size_t kReqHasPlug = 0x40;
inline constexpr std::size_t kReqSlot = 0x48;     // s32
inline constexpr std::size_t kReqVerb = 0x4c;     // s32
inline constexpr std::size_t kReqFlags = 0x50;    // u16
inline constexpr std::size_t kReqNode = 0x58;     // the asking AI node
inline constexpr std::uint32_t kKind = 4;
inline constexpr std::uint32_t kKind2 = 0x11a;
inline constexpr std::uint32_t kKind3 = 0x10;
} // namespace addr

using Request = std::array<std::uint8_t, addr::kRequestSize>;

// The protocol's action from a request as the game made it; nothing when its
// checked words are not the game's. `plug` says the request named its places
// through a plug (those are not sent: the copy then has its own places).
std::optional<BossAction> ActionFromRequest(const Request& r, bool* plug = nullptr);
// A request for the game from the owner's action, asked by `node` (the copy's
// own AI node that last asked for an action: callers always name one).
Request RequestFromAction(const BossAction& a, std::uint64_t node);

enum class Role : std::uint8_t { owner, copy };

// One boss the Objects step saw in its walk.
struct Boss {
    std::uint64_t actor = 0;
    std::uint64_t bsa = 0;  // [[actor + 0x270] + 0x188]
    Role role = Role::owner;
    std::string name;
    std::uint8_t owner = 0;  // the owning player's number (a new one: a copy starts again)
};

struct Counts {
    std::uint64_t owner_brain = 0;    // the owner's brain verb changes kept
    std::uint64_t owner_action = 0;   // the owner's action requests kept
    std::uint64_t plug_actions = 0;   // of those, named through a plug (places not sent)
    std::uint64_t copy_skipped = 0;   // a copy's own changes not run
    std::uint64_t brain_applied = 0;  // owner's brain verbs put into a copy
    std::uint64_t action_applied = 0; // owner's actions put into a copy
    std::uint64_t action_waiting = 0; // owner's actions waiting for the copy's own node
    std::uint64_t resyncs = 0;        // a copy started again from the owner's state now
    std::uint64_t drift_fixes = 0;    // a copy's brain verb put back after it differed for long
    std::uint64_t busy = 0;           // a wrap that found the table busy (the game ran its own)
    std::uint64_t minions_stopped = 0; // a copy boss's own minion not made (the owner's comes)
};

class Table {
public:
    // Switches (LICHII_COOP_BOSS_NATIVE, LICHII_COOP_BOSS_ACTIONS): off -> no
    // skip, no apply, nothing kept (the old gohma-sync path alone).
    void SetOn(bool brain, bool actions);
    bool BrainOn() const;
    bool ActionsOn() const;

    // The Objects step, after each walk (the frame thread): the bosses now.
    // A boss gone from the walk is forgotten; one whose role changed starts
    // again from its current verb.
    void Publish(const std::vector<Boss>& bosses);

    // The wraps, phase 0 (any game thread). own: the call is our caller's.
    // True: skip the game's function (a copy's own change). Never blocks:
    // a busy table lets the game run its own. changed: the game's w2 (the
    // verb changed), sent with the owner's action and given to the copies.
    bool OnBrainVerb(std::uint64_t bsa, std::int32_t verb, bool force, bool own);
    bool OnActionVerb(std::uint64_t bsa, const Request& request, bool changed, bool own);

    // A create request (any game thread): true when a boss named `maker` is a
    // copy here that takes its owner's verbs, so its own minion is not made
    // (the owner's comes as a copy; CoopObjects::MinionMaker). Never blocks.
    bool CopyMakes(std::string_view maker);

    // The owner's state to send for one of our bosses; nothing before its
    // first change was seen.
    std::optional<BossBsa> OwnerState(std::uint64_t actor) const;

    // An owner's state for a copy (from its OBJECT_SAMPLE). Its changes the
    // copy has not had are queued in the owner's order and made by Step, one
    // call each. When the copy missed more than the sample holds, or the
    // owner is new (Publish), it starts from the state now. An older sample
    // than the newest had (seq not above it) is dropped.
    void Receive(std::uint64_t actor, const BossBsa& state);

    // The caller (coop_callers.h contract), order kCallerOrder.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call,
                                         std::uint64_t result, std::uint64_t main_base);

    Counts Totals() const;
    // Lines for the log since the last call (first sightings, first applies).
    std::vector<std::string> TakeNotes();
    // Everything forgotten, the block too (tests; co-op off).
    void Reset();

    static constexpr int kCallerOrder = 45;  // after the drops (40), before the boss rooms (50)
    // Steps a copy's brain verb may differ from the owner's (nothing queued)
    // before it is put back (about a second at the game's 60 frames).
    static constexpr int kDriftSteps = 60;

private:
    struct Entry {
        std::uint64_t actor = 0;
        Role role = Role::owner;
        std::string name;
        std::uint8_t owner = 0;
        // Owner: what we send.
        BossBsa mine{};
        bool have_mine = false;
        // Copy: the owner's latest, and its changes still to make here.
        std::optional<BossBsa> wanted;
        struct Due {
            BossChange change;
            bool resync = false;  // the state now, not a call the owner made: skipped when already so
        };
        std::deque<Due> due;
        std::uint32_t queued_seq = 0;  // the owner's newest change queued (or made)
        int drift = 0;                 // steps the brain verb has differed with nothing queued
        std::uint64_t own_node = 0;    // the copy's own AI node that last asked for an action
    };
    void Record(Entry& e, std::variant<BossBrain, BossAction> what);
    void Resync(Entry& e, const BossBsa& state);
    Entry* Find(std::uint64_t bsa);
    Entry* FindActor(std::uint64_t actor);
    const Entry* FindActor(std::uint64_t actor) const;
    void Note(std::string line);

    mutable std::mutex mutex_;
    std::map<std::uint64_t, Entry> by_bsa_;
    bool brain_on_ = false;
    bool actions_on_ = false;
    Counts counts_{};
    std::vector<std::string> notes_;
    // The caller's state (frame thread only).
    std::uint64_t block_ = 0;      // our request in guest memory (kRequestSize)
    std::uint64_t main_base_ = 0;  // the game the block belongs to
    bool allocating_ = false;
    enum class Pending : std::uint8_t { none, malloc, brain, action } pending_ = Pending::none;
};

Table& TheTable();

} // namespace CoopBossNative
