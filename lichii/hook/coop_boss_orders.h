// SPDX-License-Identifier: GPL-3.0-or-later
//
// One battle manager decides for every game (row 222, Twinrova; docs/dungeons-boss.md "Battle manager orders").
// No emulator code: tested in tests/lichii/boss_orders_test.cpp.
//
// Twinrova's fight is run by a third actor, Enemy_KotakeKoumeBattleManager (the group's root; no BSA, no Life). Its
// AI node ExecuteKotakeKoumeBattleManagerAI picks the fight from its own game's Link and tells each sister what to do
// through the game's message manager (main+0x379740): message 0x70210000 (battle state, roll type, attack start
// place, main move) and 0x70210001 (under the interval threshold). Each sister's ExecuteKotakeKoumeReceiveMessageProc
// takes them in its message handler, main+0x2723f80 (x0 the node, x1 the message), and writes them into its
// blackboard; the sisters' action sequences branch on those keys. So in every game the sisters' battle state came
// from that game's own manager, and the native boss sync (coop_boss_native.h) carried only their verbs: a copy's
// verbs were the owner's but its battle state its own manager's, and after a hand-over the new owner's sisters
// could start an attack that waited for an order their manager never gave (held in the attack).
//
// The handler is wrapped (Hookshot's hook table, HOS_COOP_WRAP_BOSS_ORDER, patch 0036):
// - the owner's game lets each order run and keeps it here; it goes out in that sister's own samples
//   (OBJECT_SAMPLE component 23, coop::protocol::BossOrders);
// - a copy game skips its own manager's orders once the owner's first has come, and puts the owner's orders
//   into its sister through the same handler, called by our caller on the game's calc thread, in the owner's
//   order, each once. Its sisters then live the owner's battle state.
// Until the owner's first order comes (a new copy, an older owner) the copy's own orders run, as before.
// Hand-over: the new owner's own manager decides from then on. Its own orders were skipped while it was a copy, so it
// believes its sisters took orders they never did (its next order may wait for a state it thinks it gave): made
// ours, its newest skipped order of each type is put into its sister first (and sent, as any order of ours), so the
// sisters stand where the manager that now decides thinks they do. A game that became a copy starts again from the
// new owner's newest order of each type.
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
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_caller_orders.h"
#include "coop_puppet.h"

namespace CoopBossOrders {

using coop::protocol::BossOrder;
using coop::protocol::BossOrders;
using Payload = std::array<std::uint8_t, coop::protocol::kBossOrderPayload>;

namespace addr {
// The sisters' message handler (ExecuteKotakeKoumeReceiveMessageProc, vtable main+0x49a8568, slot 39), build e0f3ccfa.
inline constexpr std::uint64_t kReceive = 0x2723f80;  // main+
// The message it reads: +0 the sender's actor id (u32), +4 the type, +0x10 a pointer to the payload.
inline constexpr std::uint64_t kMessageSender = 0x0;
inline constexpr std::uint64_t kMessageType = 0x4;
inline constexpr std::uint64_t kMessagePayload = 0x10;
// Our block in guest memory (made once with the game's malloc): the message, then the payload after it.
inline constexpr std::uint64_t kBlockPayload = 0x20;
inline constexpr std::size_t kBlockSize = 0x40;
inline constexpr std::uint64_t kMalloc = 0x14c6c90;  // main+, as coop_boss_native.h
// The node's actor: [[[node + 8] + 8] + 0x1f0] (main+0x974b10, the AI node's actor getter).
inline constexpr std::uint64_t kNodeContext = 0x8;
inline constexpr std::uint64_t kContextOwner = 0x8;
inline constexpr std::uint64_t kOwnerActor = 0x1f0;
// The sender id the handler kept from the last message (main+0x2fa43d0 writes node+0x40+8).
inline constexpr std::uint64_t kNodeLastSender = 0x48;
// The two orders (main+0x2fa43d0 takes both: type & ~1).
inline constexpr std::uint32_t kTypeState = 0x70210000;
inline constexpr std::uint32_t kTypeInterval = 0x70210001;
}  // namespace addr

constexpr bool IsOrder(std::uint32_t type) { return (type & ~1u) == addr::kTypeState; }

enum class Role : std::uint8_t { owner, copy };

// A boss part the walk saw this round (coop_objects.cpp, with the native boss list).
struct Member {
    std::uint64_t actor = 0;
    Role role = Role::owner;
    std::uint8_t owner = 0;  // the owning player's number (a new one: the copy starts again)
    std::string name;
};

struct Counts {
    std::uint64_t owner_kept = 0;     // orders our own manager gave our part (sent)
    std::uint64_t copy_skipped = 0;   // a copy's own manager's orders not run (the owner's come)
    std::uint64_t copy_own_ran = 0;   // a copy's own orders run before the owner's first came
    std::uint64_t delivered = 0;      // owner's orders put into a copy
    std::uint64_t dropped = 0;        // owner's orders dropped (too many queued, or the part's node is gone)
    std::uint64_t restarts = 0;       // a copy started again from the owner's newest orders (new owner, or a gap)
    std::uint64_t unknown = 0;        // an order to an actor that is no synced part (solo, or not seen yet): ran
    std::uint64_t replayed = 0;       // made ours: our manager's skipped orders put into its sister (and sent)
};

// The planner, without game memory: which of the owner's orders a copy puts in next (tested).
// `delivered`: the newest seq already put in (0: none since a start). Returns the orders to put in, in order: all
// newer than `delivered` when the owner's list reaches back to it, else (a start, or a gap the list no longer
// covers) the newest order of each type, so the copy takes the owner's current battle state once.
std::vector<BossOrder> Plan(const BossOrders& owner, std::uint32_t delivered);

class Table {
public:
    static constexpr int kCallerOrder = CoopCallerOrders::kBossOrders;
    static constexpr std::size_t kMaxQueued = 16;

    void SetOn(bool on);
    bool On() const;

    // The walk's parts this round. A part no longer listed is forgotten; a role or owner change starts it again.
    void Publish(std::span<const Member> members);

    // The wrap (phase 0) of the handler, on the game's thread: `node` is x0, `actor` its actor (0: not read),
    // `type` and `payload` the message's. True: skip it (the game's handler does not run; x0 1). `own_call`: our
    // caller's own delivery (always runs).
    bool OnReceive(std::uint64_t node, std::uint64_t actor, std::uint32_t type, const Payload& payload,
                   bool own_call);

    // Ours: the orders our manager gave this part, for its samples (nothing: none, or not ours).
    std::optional<BossOrders> OwnerOrders(std::uint64_t actor) const;
    // A copy's: the owner's orders from its samples (coop_objects_sync.cpp BossOrdersIn).
    void Receive(std::uint64_t actor, const BossOrders& orders);

    // The caller (coop_callers.h contract): the owner's orders put into our copies, through the game's handler.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main_base);

    // A new game process (coop_objects.cpp, as the boss table's): every part, the block (its heap's) and any call
    // in flight are dropped. Step does the same itself when main_base changes.
    void Reset();

    Counts GetCounts() const;
    std::vector<std::string> TakeNotes();

private:
    struct Entry {
        Member member;
        std::uint64_t node = 0;           // its receive node, as the wrap saw it (0: not seen yet)
        BossOrders mine;                  // ours: what our manager gave it (seq never goes back in this game)
        bool owner_came = false;          // a copy: the owner's orders have come (its own are skipped)
        std::uint32_t delivered = 0;      // a copy: the newest owner seq put in (0: start again)
        std::uint32_t queued_up_to = 0;   // a copy: the newest owner seq queued
        std::deque<BossOrder> queue;      // a copy: the owner's orders to put in, oldest first
        std::vector<BossOrder> own_skipped;  // a copy: our own manager's newest skipped order of each type, in order
        std::deque<BossOrder> replay;     // made ours: those, to put in (and keep as ours) before anything else
    };
    void Note(std::string text);
    void Forget(Entry& e);
    void DropGame();  // (mutex_ held)
    static bool NodeIsOf(CoopPuppet::Guest& guest, std::uint64_t node, std::uint64_t actor);

    mutable std::mutex mutex_;
    bool on_ = false;
    std::map<std::uint64_t, Entry> entries_;  // by actor
    Counts counts_{};
    std::vector<std::string> notes_;
    // Our block in guest memory and the call in flight.
    std::uint64_t main_base_ = 0;  // the game our block and nodes are of
    std::uint64_t block_ = 0;
    bool allocating_ = false;
    bool malloc_failed_ = false;  // this frame's malloc gave nothing: asked again next frame
    enum class Pending : std::uint8_t { none, malloc, deliver } pending_ = Pending::none;
    std::uint64_t pending_actor_ = 0;
    bool pending_replay_ = false;
};

Table& TheTable();

}  // namespace CoopBossOrders
