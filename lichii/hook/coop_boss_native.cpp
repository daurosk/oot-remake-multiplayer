// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_boss_native.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace CoopBossNative {

namespace {

template <typename T>
T Get(const Request& r, std::size_t at) {
    T v{};
    std::memcpy(&v, r.data() + at, sizeof v);
    return v;
}

template <typename T>
void Put(Request& r, std::size_t at, T v) {
    std::memcpy(r.data() + at, &v, sizeof v);
}

std::string Hex(std::uint64_t v) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    do {
        s.insert(s.begin(), digits[v & 15]);
        v >>= 4;
    } while (v != 0);
    return "0x" + s;
}

} // namespace

std::optional<BossAction> ActionFromRequest(const Request& r, bool* plug) {
    using namespace addr;
    if (Get<std::uint32_t>(r, kReqKind) != kKind || Get<std::uint32_t>(r, kReqKind2) != kKind2) {
        return std::nullopt;
    }
    BossAction a;
    a.slot = Get<std::int32_t>(r, kReqSlot);
    a.verb = Get<std::int32_t>(r, kReqVerb);
    a.flags = Get<std::uint16_t>(r, kReqFlags);
    const bool has_plug = r[kReqHasPlug] == 1;
    if (plug != nullptr) {
        *plug = has_plug;
    }
    if (!has_plug) {
        for (std::size_t i = 0; i < a.places.size(); ++i) {
            const coop::protocol::Vec3 p{Get<float>(r, kReqPlaces + 12 * i), Get<float>(r, kReqPlaces + 12 * i + 4),
                                         Get<float>(r, kReqPlaces + 12 * i + 8)};
            if (std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2])) {
                a.places[i] = p;
            }
        }
    }
    const std::array<float, 2> extra{Get<float>(r, kReqExtra), Get<float>(r, kReqExtra + 4)};
    if (std::isfinite(extra[0]) && std::isfinite(extra[1])) {
        a.extra = extra;
    }
    return a;
}

Request RequestFromAction(const BossAction& a, std::uint64_t node) {
    using namespace addr;
    Request r{};
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (std::size_t i = 0; i < a.places.size(); ++i) {
        const coop::protocol::Vec3 p = a.places[i].value_or(coop::protocol::Vec3{nan, 0.0f, 0.0f});
        for (std::size_t k = 0; k < 3; ++k) {
            Put<float>(r, kReqPlaces + 12 * i + 4 * k, p[k]);
        }
    }
    Put<std::uint32_t>(r, kReqKind, kKind);
    Put<std::uint32_t>(r, kReqKind2, kKind2);
    const std::array<float, 2> extra = a.extra.value_or(std::array<float, 2>{nan, nan});
    Put<float>(r, kReqExtra, extra[0]);
    Put<float>(r, kReqExtra + 4, extra[1]);
    Put<std::uint32_t>(r, kReqKind3, kKind3);
    Put<std::uint64_t>(r, kReqPlug, 0);
    r[kReqHasPlug] = 0;
    Put<std::int32_t>(r, kReqSlot, a.slot);
    Put<std::int32_t>(r, kReqVerb, a.verb);
    Put<std::uint16_t>(r, kReqFlags, a.flags);
    Put<std::uint64_t>(r, kReqNode, node);
    return r;
}

void Table::SetOn(bool brain, bool actions) {
    std::lock_guard lock(mutex_);
    brain_on_ = brain;
    actions_on_ = brain && actions;
}

bool Table::BrainOn() const {
    std::lock_guard lock(mutex_);
    return brain_on_;
}

bool Table::ActionsOn() const {
    std::lock_guard lock(mutex_);
    return actions_on_;
}

Table::Entry* Table::Find(std::uint64_t bsa) {
    const auto it = by_bsa_.find(bsa);
    return it == by_bsa_.end() ? nullptr : &it->second;
}

Table::Entry* Table::FindActor(std::uint64_t actor) {
    for (auto& [bsa, e] : by_bsa_) {
        if (e.actor == actor) {
            return &e;
        }
    }
    return nullptr;
}

const Table::Entry* Table::FindActor(std::uint64_t actor) const {
    for (const auto& [bsa, e] : by_bsa_) {
        if (e.actor == actor) {
            return &e;
        }
    }
    return nullptr;
}

void Table::Note(std::string line) {
    if (notes_.size() < 64) {
        notes_.push_back(std::move(line));
    }
}

void Table::Publish(const std::vector<Boss>& bosses) {
    std::lock_guard lock(mutex_);
    std::map<std::uint64_t, Entry> next;
    for (const Boss& b : bosses) {
        if (b.actor == 0 || b.bsa == 0) {
            continue;
        }
        Entry e;
        if (const auto it = by_bsa_.find(b.bsa); it != by_bsa_.end() && it->second.actor == b.actor) {
            e = std::move(it->second);
            if (e.role != b.role) {
                Note(b.name + ": now " + (b.role == Role::owner ? "ours: our game runs it (sends its verbs)"
                                                                 : "a copy: it takes the owner's verbs"));
                // Ours from here: we go on from where the copy is (the owner's
                // last verbs were put in). A copy: wait for the owner's state.
                e.have_mine = e.have_mine || e.wanted.has_value();
                if (e.wanted) {
                    e.mine = *e.wanted;
                }
                e.wanted.reset();
                e.brain_due = e.action_due = false;
            }
        } else {
            e.actor = b.actor;
            Note(b.name + ": native boss sync on (BSA " + Hex(b.bsa) + ", " +
                 (b.role == Role::owner ? "ours" : "a copy") + ")");
        }
        e.role = b.role;
        e.name = b.name;
        next.emplace(b.bsa, std::move(e));
    }
    by_bsa_ = std::move(next);
}

bool Table::OnBrainVerb(std::uint64_t bsa, std::int32_t verb, bool force, bool own) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        ++counts_.busy;  // (racy count, for the log only)
        return false;
    }
    if (!brain_on_) {
        return false;
    }
    Entry* e = Find(bsa);
    if (e == nullptr) {
        return false;  // not a boss we know
    }
    if (e->role == Role::owner) {
        e->mine.brain_verb = verb;
        e->mine.brain_force = force;
        ++e->mine.seq;
        e->have_mine = true;
        ++counts_.owner_brain;
        return false;
    }
    if (own || !e->wanted) {
        return false;  // ours, or no owner's state yet: the copy runs as before
    }
    ++counts_.copy_skipped;
    return true;
}

bool Table::OnActionVerb(std::uint64_t bsa, const Request& request, bool own) {
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        ++counts_.busy;
        return false;
    }
    if (!actions_on_) {
        return false;
    }
    Entry* e = Find(bsa);
    if (e == nullptr) {
        return false;
    }
    if (e->role == Role::owner) {
        bool plug = false;
        if (auto a = ActionFromRequest(request, &plug)) {
            e->mine.action = std::move(*a);
            ++e->mine.seq;
            e->have_mine = true;
            ++counts_.owner_action;
            if (plug) {
                ++counts_.plug_actions;
            }
        }
        return false;
    }
    if (own) {
        return false;
    }
    // A copy's own ask: its node is the one our calls name (the game's
    // callers always name the asking node).
    std::uint64_t node = 0;
    std::memcpy(&node, request.data() + addr::kReqNode, sizeof node);
    if (node != 0) {
        e->own_node = node;
    }
    if (!e->wanted) {
        return false;
    }
    ++counts_.copy_skipped;
    return true;
}

std::optional<BossBsa> Table::OwnerState(std::uint64_t actor) const {
    std::lock_guard lock(mutex_);
    const Entry* e = FindActor(actor);
    if (e == nullptr || e->role != Role::owner || !e->have_mine) {
        return std::nullopt;
    }
    return e->mine;
}

void Table::Receive(std::uint64_t actor, const BossBsa& state) {
    std::lock_guard lock(mutex_);
    if (!brain_on_) {
        return;
    }
    Entry* e = FindActor(actor);
    if (e == nullptr || e->role != Role::copy) {
        return;
    }
    const bool first = !e->wanted;
    if (!first && state.seq == e->wanted->seq) {
        return;  // the same state again (every sample carries it)
    }
    // A smaller seq is a new owner's count (a hand-over): taken as new.
    const bool brain_new = first || e->wanted->brain_verb != state.brain_verb;
    const bool action_new = state.action && (first || e->wanted->action != state.action);
    e->wanted = state;
    e->brain_due = e->brain_due || brain_new;
    e->action_due = actions_on_ && (e->action_due || action_new);
    if (first) {
        Note(e->name + ": the owner's verbs came (brain " + std::to_string(state.brain_verb) +
             "): the copy's own brain is skipped from now");
    }
}

std::optional<CoopPuppet::Call> Table::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                            std::uint64_t main_base) {
    std::unique_lock lock(mutex_);
    if (!after_call && pending_ != Pending::none) {
        // Last frame's call was not made (the runtime could not): ask again.
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
                Note("native boss sync: request block at " + Hex(result));
            }
            break;
        case Pending::brain:
            ++counts_.brain_applied;
            break;
        case Pending::action:
            ++counts_.action_applied;
            break;
        case Pending::none:
            break;
        }
    }
    if (!brain_on_ || main_base == 0) {
        return std::nullopt;
    }
    for (auto& [bsa, e] : by_bsa_) {
        if (e.role != Role::copy || !e.wanted) {
            continue;
        }
        if (e.brain_due) {
            e.brain_due = false;
            std::int32_t now = 0;
            const bool there = guest.Read(bsa + addr::kBsaBrainVerb, &now, sizeof now) && now == e.wanted->brain_verb;
            if (!there) {
                CoopPuppet::Call c;
                c.function = main_base + addr::kSetBrainVerb;
                c.x[0] = bsa;
                c.x[1] = static_cast<std::uint32_t>(e.wanted->brain_verb);
                c.x[2] = 1;  // force: the change is the owner's, already decided
                c.x[3] = 0;  // (the asking node: not read by the function)
                pending_ = Pending::brain;
                return c;
            }
        }
        if (e.action_due && e.wanted->action) {
            if (e.own_node == 0) {
                ++counts_.action_waiting;  // (asked again next frame)
                continue;
            }
            if (block_ == 0) {
                if (allocating_) {
                    return std::nullopt;
                }
                CoopPuppet::Call c;
                c.function = main_base + addr::kMalloc;
                c.x[0] = addr::kRequestSize;
                allocating_ = true;
                pending_ = Pending::malloc;
                return c;
            }
            const Request r = RequestFromAction(*e.wanted->action, e.own_node);
            if (!guest.Write(block_, r.data(), r.size())) {
                continue;
            }
            e.action_due = false;
            CoopPuppet::Call c;
            c.function = main_base + addr::kRequestActionVerb;
            c.x[0] = bsa;
            c.x[1] = block_;
            c.x[2] = 1;  // changed: the owner's new action
            pending_ = Pending::action;
            return c;
        }
    }
    return std::nullopt;
}

Counts Table::Totals() const {
    std::lock_guard lock(mutex_);
    return counts_;
}

std::vector<std::string> Table::TakeNotes() {
    std::lock_guard lock(mutex_);
    return std::exchange(notes_, {});
}

void Table::Reset() {
    std::lock_guard lock(mutex_);
    by_bsa_.clear();
    counts_ = {};
    notes_.clear();
    block_ = 0;  // (a new scene's heap: asked again)
    allocating_ = false;
    pending_ = Pending::none;
}

Table& TheTable() {
    static Table table;
    return table;
}

} // namespace CoopBossNative
