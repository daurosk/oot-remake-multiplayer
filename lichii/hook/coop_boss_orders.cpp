// SPDX-License-Identifier: GPL-3.0-or-later
//
// One battle manager decides for every game (row 222): coop_boss_orders.h.
#include "coop_boss_orders.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace CoopBossOrders {

namespace {

std::string Hex(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "%#llx", static_cast<unsigned long long>(v));
    return text;
}

}  // namespace

std::vector<BossOrder> Plan(const BossOrders& owner, std::uint32_t delivered) {
    std::vector<BossOrder> out;
    if (owner.recent.empty() || owner.seq <= delivered) {
        return out;  // nothing new (an owner whose count is behind ours is a new owner: Receive starts again)
    }
    // The list reaches back to what we have: every order after it, in order.
    if (delivered != 0 && owner.recent.front().seq <= delivered + 1) {
        for (const BossOrder& o : owner.recent) {
            if (o.seq > delivered) {
                out.push_back(o);
            }
        }
        return out;
    }
    // A start or a gap: the newest of each type, in the owner's order.
    const BossOrder* state = nullptr;
    const BossOrder* interval = nullptr;
    for (const BossOrder& o : owner.recent) {
        if (o.type == addr::kTypeState) {
            state = &o;
        } else if (o.type == addr::kTypeInterval) {
            interval = &o;
        }
    }
    for (const BossOrder& o : owner.recent) {
        if (&o == state || &o == interval) {
            out.push_back(o);
        }
    }
    return out;
}

void Table::SetOn(bool on) {
    std::lock_guard lock(mutex_);
    on_ = on;
    if (!on) {
        entries_.clear();
    }
}

bool Table::On() const {
    std::lock_guard lock(mutex_);
    return on_;
}

void Table::Note(std::string text) {
    if (notes_.size() < 64) {
        notes_.push_back(std::move(text));
    }
}

void Table::Forget(Entry& e) {
    e.owner_came = false;
    e.delivered = 0;
    e.queued_up_to = 0;
    e.queue.clear();
}

void Table::Publish(std::span<const Member> members) {
    std::lock_guard lock(mutex_);
    if (!on_) {
        return;
    }
    std::map<std::uint64_t, Entry> next;
    for (const Member& m : members) {
        auto it = entries_.find(m.actor);
        Entry e = it != entries_.end() ? std::move(it->second) : Entry{};
        const bool seen = it != entries_.end();
        if (seen && (e.member.role != m.role || e.member.owner != m.owner)) {
            // A hand-over. Made ours: our own manager decides from now on (its orders run and are sent).
            // Made a copy, or a new owner: start again from the owner's newest orders.
            Note(m.name + ": battle orders: " +
                 (m.role == Role::owner ? std::string("ours now: our own manager's orders run")
                                        : "player " + std::to_string(m.owner) + "'s now: its orders come"));
            std::vector<BossOrder> skipped = std::move(e.own_skipped);
            Forget(e);
            e.own_skipped.clear();
            e.replay.clear();
            if (m.role == Role::owner && e.member.role == Role::copy && !skipped.empty()) {
                // What our manager thinks it told the sister: put in first, so the two agree.
                Note(m.name + ": battle orders: " + std::to_string(skipped.size()) +
                     " skipped order(s) of our own manager put in first");
                e.replay.assign(skipped.begin(), skipped.end());
            }
        }
        e.member = m;
        next.emplace(m.actor, std::move(e));
    }
    entries_ = std::move(next);
}

bool Table::OnReceive(std::uint64_t node, std::uint64_t actor, std::uint32_t type, const Payload& payload,
                      bool own_call) {
    if (!IsOrder(type)) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (!on_ || own_call) {
        return false;
    }
    const auto it = entries_.find(actor);
    if (actor == 0 || it == entries_.end()) {
        ++counts_.unknown;
        return false;
    }
    Entry& e = it->second;
    if (e.node != node) {
        e.node = node;
        Note(e.member.name + ": battle orders: its receive node " + Hex(node));
    }
    if (e.member.role == Role::owner) {
        BossOrder o{e.mine.seq + 1, type, payload};
        e.mine.seq = o.seq;
        e.mine.recent.push_back(o);
        if (e.mine.recent.size() > coop::protocol::kMaxBossOrders) {
            e.mine.recent.erase(e.mine.recent.begin());
        }
        ++counts_.owner_kept;
        if (o.seq == 1) {
            Note(e.member.name + ": battle orders: our manager's first order (" + Hex(type) + ") is sent");
        }
        return false;
    }
    if (!e.owner_came) {
        ++counts_.copy_own_ran;
        return false;
    }
    // Skipped; the newest of its type is kept for a hand-over to us (Publish).
    std::erase_if(e.own_skipped, [type](const BossOrder& o) { return o.type == type; });
    e.own_skipped.push_back(BossOrder{0, type, payload});
    ++counts_.copy_skipped;
    return true;
}

std::optional<BossOrders> Table::OwnerOrders(std::uint64_t actor) const {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (!on_ || it == entries_.end() || it->second.member.role != Role::owner || it->second.mine.seq == 0) {
        return std::nullopt;
    }
    return it->second.mine;
}

void Table::Receive(std::uint64_t actor, const BossOrders& orders) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (!on_ || it == entries_.end() || it->second.member.role != Role::copy || orders.recent.empty()) {
        return;
    }
    Entry& e = it->second;
    if (orders.seq < e.queued_up_to) {
        // The owner's count went back: another game's count (a hand-over the walk has not seen yet). Start again.
        ++counts_.restarts;
        Forget(e);
    }
    const std::vector<BossOrder> plan = Plan(orders, e.queued_up_to);
    if (plan.empty()) {
        return;
    }
    if (e.queued_up_to == 0 || plan.front().seq > e.queued_up_to + 1) {
        if (e.queued_up_to != 0) {
            ++counts_.restarts;  // a gap the owner's list no longer covers: its newest state
        }
        e.queue.clear();
    }
    if (!e.owner_came) {
        Note(e.member.name + ": battle orders: the owner's came: our own manager's orders are skipped");
    }
    e.owner_came = true;
    for (const BossOrder& o : plan) {
        e.queue.push_back(o);
    }
    e.queued_up_to = plan.back().seq;
    while (e.queue.size() > kMaxQueued) {
        e.queue.pop_front();
        ++counts_.dropped;
    }
}

bool Table::NodeIsOf(CoopPuppet::Guest& guest, std::uint64_t node, std::uint64_t actor) {
    std::uint64_t context = 0;
    std::uint64_t owner = 0;
    std::uint64_t got = 0;
    return node != 0 && guest.Read(node + addr::kNodeContext, &context, sizeof context) && context != 0 &&
           guest.Read(context + addr::kContextOwner, &owner, sizeof owner) && owner != 0 &&
           guest.Read(owner + addr::kOwnerActor, &got, sizeof got) && got == actor;
}

std::optional<CoopPuppet::Call> Table::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                            std::uint64_t main_base) {
    std::lock_guard lock(mutex_);
    if (main_base != main_base_) {
        // A new game (or none): our block was its heap's and every node its; a call in flight is not ours to
        // finish (as coop_boss_native's block).
        if (main_base_ != 0) {
            Note("battle orders: a new game: block, parts and call in flight dropped");
            DropGame();
        }
        main_base_ = main_base;
        after_call = false;
    }
    if (!after_call) {
        malloc_failed_ = false;
    }
    if (!after_call && pending_ != Pending::none) {
        // Last frame's call was not made: a malloc is asked again; an order stays queued.
        if (pending_ == Pending::malloc) {
            allocating_ = false;
        }
        pending_ = Pending::none;
    }
    if (after_call) {
        switch (std::exchange(pending_, Pending::none)) {
        case Pending::malloc:
            allocating_ = false;
            if (result != 0) {
                block_ = result;
                Note("battle orders: message block at " + Hex(result));
            } else {
                malloc_failed_ = true;
            }
            break;
        case Pending::deliver:
            if (const auto it = entries_.find(pending_actor_); it != entries_.end()) {
                Entry& e = it->second;
                if (pending_replay_ && !e.replay.empty() && e.member.role == Role::owner) {
                    // Ours: kept and sent as any order our manager gives (the wrap let our own call run unkept).
                    BossOrder o = e.replay.front();
                    e.replay.pop_front();
                    o.seq = e.mine.seq + 1;
                    e.mine.seq = o.seq;
                    e.mine.recent.push_back(o);
                    if (e.mine.recent.size() > coop::protocol::kMaxBossOrders) {
                        e.mine.recent.erase(e.mine.recent.begin());
                    }
                    ++counts_.replayed;
                } else if (!pending_replay_ && !e.queue.empty()) {
                    e.delivered = e.queue.front().seq;
                    e.queue.pop_front();
                    ++counts_.delivered;
                }
            }
            break;
        case Pending::none:
            break;
        }
    }
    if (!on_ || main_base == 0) {
        return std::nullopt;
    }
    for (auto& [actor, e] : entries_) {
        const bool replay = e.member.role == Role::owner && !e.replay.empty();
        if (!replay && (e.member.role != Role::copy || e.queue.empty())) {
            continue;
        }
        if (e.node == 0) {
            continue;  // (a copy whose node the wrap has not seen yet: its own manager's first order names it)
        }
        std::deque<BossOrder>& from = replay ? e.replay : e.queue;
        if (!NodeIsOf(guest, e.node, actor)) {
            Note(e.member.name + ": battle orders: its node " + Hex(e.node) + " is no longer its: " +
                 std::to_string(from.size()) + " dropped");
            counts_.dropped += from.size();
            from.clear();
            e.node = 0;
            continue;
        }
        if (block_ == 0) {
            if (allocating_ || malloc_failed_) {
                return std::nullopt;
            }
            CoopPuppet::Call c;
            c.function = main_base + addr::kMalloc;
            c.x[0] = addr::kBlockSize;
            allocating_ = true;
            pending_ = Pending::malloc;
            return c;
        }
        const BossOrder& o = from.front();
        // The sender: the id our own manager's last order named (the handler keeps it), so the message reads as
        // our manager's.
        std::uint32_t sender = 0xffffffffu;
        (void)guest.Read(e.node + addr::kNodeLastSender, &sender, sizeof sender);
        std::array<std::uint8_t, addr::kBlockSize> block{};
        const std::uint32_t type = o.type;
        const std::uint64_t payload_at = block_ + addr::kBlockPayload;
        std::copy_n(reinterpret_cast<const std::uint8_t*>(&sender), sizeof sender, block.data() + addr::kMessageSender);
        std::copy_n(reinterpret_cast<const std::uint8_t*>(&type), sizeof type, block.data() + addr::kMessageType);
        std::copy_n(reinterpret_cast<const std::uint8_t*>(&payload_at), sizeof payload_at,
                    block.data() + addr::kMessagePayload);
        std::copy(o.payload.begin(), o.payload.end(), block.data() + addr::kBlockPayload);
        if (!guest.Write(block_, block.data(), block.size())) {
            return std::nullopt;
        }
        CoopPuppet::Call c;
        c.function = main_base + addr::kReceive;
        c.x[0] = e.node;
        c.x[1] = block_;
        c.copy_game = true;
        pending_ = Pending::deliver;
        pending_actor_ = actor;
        pending_replay_ = replay;
        return c;
    }
    return std::nullopt;
}

void Table::DropGame() {
    entries_.clear();
    block_ = 0;
    allocating_ = false;
    malloc_failed_ = false;
    pending_ = Pending::none;
    pending_actor_ = 0;
    pending_replay_ = false;
}

void Table::Reset() {
    std::lock_guard lock(mutex_);
    DropGame();
    main_base_ = 0;
    counts_ = {};
    notes_.clear();
}

Counts Table::GetCounts() const {
    std::lock_guard lock(mutex_);
    return counts_;
}

std::vector<std::string> Table::TakeNotes() {
    std::lock_guard lock(mutex_);
    return std::exchange(notes_, {});
}

Table& TheTable() {
    static Table table;
    return table;
}

}  // namespace CoopBossOrders
