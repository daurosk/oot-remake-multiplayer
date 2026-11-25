// SPDX-License-Identifier: GPL-3.0-or-later
// A copy boss throws what its owner's boss prepared (coop_boss_shots.h).
#include "coop_boss_shots.h"

#include <cstdio>
#include <utility>

namespace CoopBossShots {

namespace {

std::string Hex(std::uint64_t v) {
    char text[24];
    std::snprintf(text, sizeof text, "0x%llx", static_cast<unsigned long long>(v));
    return text;
}

std::string Text(const BossShot& s) {
    return Hex(s.hash) + " x" + std::to_string(s.num);
}

}  // namespace

std::uint64_t ShooterOf(const Reader& read, std::uint64_t actor) {
    std::uint64_t components = 0;
    std::uint64_t shooter = 0;
    if (actor == 0 || !read || !read(actor + addr::kActorComponents, &components, sizeof components) ||
        components == 0 || !read(components + addr::kComponentShooter, &shooter, sizeof shooter)) {
        return 0;
    }
    return shooter;
}

std::vector<BossShot> Plan(const BossShots& owner, std::optional<std::uint32_t> made, std::uint32_t* lost) {
    std::vector<BossShot> out;
    if (lost != nullptr) {
        *lost = 0;
    }
    if (!made) {
        return owner.recent;  // a start: the fresh ones, all of them
    }
    if (owner.seq <= *made) {
        return out;
    }
    for (const BossShot& s : owner.recent) {
        if (s.seq > *made) {
            out.push_back(s);
        }
    }
    if (lost != nullptr) {
        const std::uint32_t news = owner.seq - *made;
        *lost = news > out.size() ? news - static_cast<std::uint32_t>(out.size()) : 0;
    }
    return out;
}

void Table::SetOn(bool on) {
    std::lock_guard lock(mutex_);
    on_ = on;
    if (!on) {
        entries_.clear();
        pending_ = false;
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

void Table::Publish(std::span<const Member> members) {
    std::lock_guard lock(mutex_);
    if (!on_) {
        return;
    }
    ++round_;
    std::map<std::uint64_t, Entry> next;
    for (const Member& m : members) {
        auto it = entries_.find(m.actor);
        Entry e = it != entries_.end() ? std::move(it->second) : Entry{};
        const bool seen = it != entries_.end();
        if (seen && (e.member.role != m.role || e.member.owner != m.owner)) {
            // A hand-over. Ours: our own AI asks from now on (kept and sent; our count goes on). A copy, or a new
            // owner: start at the new owner's count; what was queued was the old owner's.
            Note(m.name + ": shooter requests: " +
                 (m.role == Role::owner ? std::string("ours now: our own boss's requests run")
                                        : "player " + std::to_string(m.owner) + "'s now: its requests come"));
            counts_.dropped += e.queue.size();
            e.queue.clear();
            e.owner_came = false;
            e.queued_up_to = 0;
            e.mine.recent.clear();  // (ours: only what we ask from now on)
            e.mine_rounds.clear();
        }
        // Ours: a request stays in the list while it is fresh (the count goes on).
        while (!e.mine_rounds.empty() && round_ - e.mine_rounds.front() > kFreshRounds) {
            e.mine_rounds.pop_front();
            e.mine.recent.erase(e.mine.recent.begin());
        }
        e.member = m;
        next.emplace(m.actor, std::move(e));
    }
    entries_ = std::move(next);
}

Table::Entry* Table::OfShooter(std::uint64_t shooter, const Reader& read) {
    if (shooter == 0) {
        return nullptr;
    }
    for (auto& [actor, e] : entries_) {
        if (ShooterOf(read, actor) == shooter) {
            return &e;
        }
    }
    return nullptr;
}

bool Table::OnRequest(std::uint64_t shooter, std::uint32_t hash, std::uint32_t num, bool own_call,
                      const Reader& read) {
    std::lock_guard lock(mutex_);
    if (!on_) {
        return false;
    }
    ++counts_.asked;
    if (own_call || entries_.empty()) {
        counts_.asked_own += own_call ? 1 : 0;
        if (counts_.asked <= 6) {
            Note("shooter requests: probe: a request (" + Hex(hash) + " x" + std::to_string(num & 0xffu) + ") with " +
                 (own_call ? "our own call" : "no boss member in the table"));
        }
        return false;
    }
    Entry* e = OfShooter(shooter, read);
    if (e == nullptr) {
        ++counts_.no_entry;
        if (counts_.no_entry <= 6) {
            Note("shooter requests: probe: a request (" + Hex(hash) + " x" + std::to_string(num & 0xffu) +
                 ") from shooter " + Hex(shooter) + " that no boss of ours owns");
        }
        return false;
    }
    ++(e->member.role == Role::owner ? counts_.as_owner : counts_.as_copy);
    if (e->member.role != Role::copy) {
        return false;
    }
    if (!e->owner_came) {
        ++counts_.copy_own_ran;
        return false;
    }
    if (counts_.copy_skipped++ == 0) {
        Note(e->member.name + ": shooter requests: our copy's own (" + Hex(hash) + " x" + std::to_string(num & 0xffu) +
             ") is skipped: its owner's are made");
    }
    return true;
}

void Table::OnRan(std::uint64_t shooter, std::uint32_t hash, std::uint32_t num, bool found, bool own_call,
                  const Reader& read) {
    std::lock_guard lock(mutex_);
    if (!on_ || own_call || entries_.empty()) {
        return;
    }
    Entry* e = OfShooter(shooter, read);
    if (e == nullptr || e->member.role != Role::owner) {
        return;
    }
    if (!found) {
        ++counts_.ran_not_found;
        return;
    }
    if (!Asks(hash, num)) {
        ++counts_.ran_not_asked;
        if (counts_.ran_not_asked <= 6) {
            Note("shooter requests: probe: our boss's request (" + Hex(hash) + " x" + std::to_string(num & 0xffu) +
                 ") is not one a bullet asks");
        }
        return;
    }
    const BossShot s{e->mine.seq + 1, hash, static_cast<std::uint8_t>(num & 0xffu)};
    e->mine.seq = s.seq;
    e->mine.recent.push_back(s);
    e->mine_rounds.push_back(round_);
    if (e->mine.recent.size() > coop::protocol::kMaxBossShots) {
        e->mine.recent.erase(e->mine.recent.begin());
        e->mine_rounds.pop_front();
    }
    if (counts_.owner_kept++ == 0) {
        Note(e->member.name + ": shooter requests: our boss's first (" + Text(s) + ") is sent");
    }
}

std::optional<BossShots> Table::OwnerShots(std::uint64_t actor) const {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (!on_ || it == entries_.end() || it->second.member.role != Role::owner) {
        return std::nullopt;
    }
    return it->second.mine;
}

void Table::Receive(std::uint64_t actor, const BossShots& shots) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (!on_ || it == entries_.end() || it->second.member.role != Role::copy) {
        return;
    }
    Entry& e = it->second;
    std::optional<std::uint32_t> made = e.queued_up_to;
    if (!e.owner_came || shots.seq < e.queued_up_to) {
        // The owner's first list, or its count went back (another game's count: a hand-over the walk has not seen
        // yet): start with its list, which holds only its fresh requests.
        if (e.owner_came) {
            ++counts_.restarts;
            counts_.dropped += e.queue.size();
            e.queue.clear();
        } else {
            Note(e.member.name + ": shooter requests: the owner's came (at " + std::to_string(shots.seq) +
                 "): our copy's own are skipped");
        }
        e.owner_came = true;
        made.reset();
    }
    std::uint32_t lost = 0;
    const std::vector<BossShot> plan = Plan(shots, made, &lost);
    counts_.dropped += lost;
    e.queued_up_to = shots.seq;
    for (const BossShot& s : plan) {
        e.queue.push_back(s);
    }
    while (e.queue.size() > kMaxQueued) {
        e.queue.pop_front();
        ++counts_.dropped;
    }
}

std::optional<CoopPuppet::Call> Table::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                            std::uint64_t main_base) {
    std::lock_guard lock(mutex_);
    if (main_base != main_base_) {
        // A new game (or none): every boss was the old game's; a call in flight is not ours to finish.
        if (main_base_ != 0) {
            if (!entries_.empty()) {
                Note("shooter requests: a new game: bosses and call in flight dropped");
            }
            entries_.clear();
        }
        pending_ = false;
        main_base_ = main_base;
        after_call = false;
    }
    if (!after_call) {
        pending_ = false;  // (last frame's call was not made: the request stays queued)
    }
    if (after_call && std::exchange(pending_, false)) {
        if (const auto it = entries_.find(pending_actor_); it != entries_.end() && !it->second.queue.empty()) {
            Entry& e = it->second;
            const BossShot s = e.queue.front();
            e.queue.pop_front();
            if ((result & 0xffffffffu) != 0) {
                if (counts_.delivered++ == 0) {
                    Note(e.member.name + ": shooter requests: the owner's first (" + Text(s) + ") is made on our copy");
                }
            } else {
                if (counts_.missed++ == 0) {
                    Note(e.member.name + ": shooter requests: our copy's shooter does not know " + Text(s));
                }
            }
        }
    }
    if (!on_ || main_base == 0) {
        return std::nullopt;
    }
    const Reader read = [&guest](std::uint64_t a, void* out, std::size_t n) { return guest.Read(a, out, n); };
    for (auto& [actor, e] : entries_) {
        if (e.member.role != Role::copy || e.queue.empty()) {
            continue;
        }
        const std::uint64_t shooter = ShooterOf(read, actor);
        if (shooter == 0) {
            Note(e.member.name + ": shooter requests: our copy has no shooter: " + std::to_string(e.queue.size()) +
                 " dropped");
            counts_.dropped += e.queue.size();
            e.queue.clear();
            continue;
        }
        const BossShot& s = e.queue.front();
        CoopPuppet::Call c;
        c.function = main_base + addr::kRequestCreate;
        c.x[0] = shooter;
        c.x[1] = s.hash;
        c.x[2] = s.num;
        c.copy_game = true;  // (what the shooter makes from it is the copy boss's own, judged as any)
        pending_ = true;
        pending_actor_ = actor;
        return c;
    }
    return std::nullopt;
}

void Table::Reset() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    pending_ = false;
    pending_actor_ = 0;
    main_base_ = 0;
    round_ = 0;
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

}  // namespace CoopBossShots
