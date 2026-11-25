// SPDX-License-Identifier: GPL-3.0-or-later
//
// A copy boss shows the owner's shots (Twinrova hand-over): coop_boss_shots.h.
#include "coop_boss_shots.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace CoopBossShots {

namespace {

template <typename T>
bool Get(CoopPuppet::Guest& guest, std::uint64_t address, T& out) {
    return address != 0 && guest.Read(address, &out, sizeof out);
}

// The bullet's actor now (0: its request or actor is gone).
std::uint64_t ActorOf(CoopPuppet::Guest& guest, std::uint64_t handle) {
    std::uint64_t request = 0, actor = 0;
    return Get(guest, handle, request) && Get(guest, request + addr::kRequestActor, actor) ? actor : 0;
}

void Put(CoopPuppet::Guest& guest, std::uint64_t actor, std::uint32_t state) {
    guest.Write(actor + addr::kActorState, &state, sizeof state);
}

bool Finite(const coop::protocol::Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

}  // namespace

bool Covers(std::string_view boss_name) { return boss_name == "Enemy_DungeonBoss_Twinrova"; }

bool IsBullet(std::string_view name) {
    return name.starts_with("Twinrova_Bullet_") && coop::protocol::ValidShotName(name);
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

// The bullet's actor when it is in `state`; 0 when not, or not read yet.
std::uint64_t Table::Live(CoopPuppet::Guest& guest, Bullet& b, std::uint32_t state) {
    std::uint32_t now = 0;
    return b.actor != 0 && Get(guest, b.actor + addr::kActorState, now) && now == state ? b.actor : 0;
}

// The game's take of a made actor clears the create's slot (docs/addresses.md, kRequestActor), and her bullets are
// made in the intro, before she is listed: each actor is read while its slot still names it, and the slot is not
// read again (it is used for other creates). Her room's bullets go with her room (Publish).
void Table::ReadActors(CoopPuppet::Guest& guest) {
    for (Bullet& b : bullets_) {
        if (b.actor == 0) {
            b.actor = ActorOf(guest, b.handle);
        }
    }
}

void Table::KeepBullet(std::uint64_t handle, std::string_view name) {
    if (handle == 0 || !IsBullet(name)) {
        return;
    }
    std::lock_guard lock(mutex_);
    for (Bullet& b : bullets_) {
        if (b.handle == handle) {
            b = Bullet{handle, std::string(name)};  // (the room made again: the same place, a new bullet)
            return;
        }
    }
    if (bullets_.size() >= kMaxBullets) {
        bullets_.erase(bullets_.begin());  // (an older room's)
    }
    bullets_.push_back({handle, std::string(name)});
}

void Table::Publish(std::span<const Member> members) {
    std::lock_guard lock(mutex_);
    if (!on_) {
        return;
    }
    std::map<std::uint64_t, Entry> next;
    for (const Member& m : members) {
        if (!Covers(m.name)) {
            continue;
        }
        auto it = entries_.find(m.actor);
        Entry e = it != entries_.end() ? std::move(it->second) : Entry{};
        if (it != entries_.end() && (e.member.role != m.role || e.member.owner != m.owner)) {
            Note(m.name + ": shots: " +
                 (m.role == Role::owner ? std::string("ours now: our own AI fires")
                                        : "player " + std::to_string(m.owner) + "'s now: its shots come"));
            e = Entry{};
            for (Bullet& b : bullets_) {
                b.seq = 0;      // (bullets in flight finish on their own and are no one's shot;
                b.awake = true; //  ours count from the next wake)
            }
            ++counts_.restarts;
        }
        e.member = m;
        next.emplace(m.actor, std::move(e));
    }
    if (next.empty() && !entries_.empty()) {
        bullets_.clear();  // her room is gone: its bullets with it (a new room makes its own)
    }
    entries_ = std::move(next);
}

std::optional<BossShots> Table::OwnerShots(std::uint64_t actor, std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (it == entries_.end() || it->second.member.role != Role::owner || it->second.seq == 0) {
        return std::nullopt;
    }
    BossShots out;
    out.seq = it->second.seq;
    for (const Bullet& b : bullets_) {
        if (b.awake && b.seq != 0) {
            out.recent.push_back({b.seq, b.name, b.position});
        }
    }
    std::sort(out.recent.begin(), out.recent.end(),
              [](const BossShot& a, const BossShot& b) { return a.seq < b.seq; });
    if (out.recent.size() > coop::protocol::kMaxBossShots) {
        out.recent.erase(out.recent.begin(), out.recent.end() - coop::protocol::kMaxBossShots);
    }
    if (out.recent.empty() && now_us - it->second.ended_us > kKeepUs) {
        return std::nullopt;
    }
    return out;
}

void Table::Receive(std::uint64_t actor, const BossShots& shots) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (!on_ || it == entries_.end() || it->second.member.role != Role::copy) {
        return;
    }
    Entry& e = it->second;
    if (!e.owner_came) {
        e.owner_came = true;
        Note(e.member.name + ": shots: the owner's came: our own AI's wakes are put back");
    }
    if (shots.seq < e.through) {
        // An owner whose count is behind ours: another game's count (a hand-over we have not published yet).
        e.through = 0;
        ++counts_.restarts;
    }
    e.owner = shots;
}

void Table::StepOwner(CoopPuppet::Guest& guest, Entry& e, std::uint64_t now_us) {
    for (Bullet& b : bullets_) {
        const std::uint64_t actor = Live(guest, b, addr::kStateAwake);
        if (actor != 0) {
            coop::protocol::Vec3 p{};
            if (guest.Read(actor + addr::kActorPosition, p.data(), sizeof p) && Finite(p)) {
                b.position = p;
            }
            if (!b.awake) {
                b.seq = ++e.seq;  // a wake: one shot
                ++counts_.owner_kept;
            }
        } else if (b.awake) {
            e.ended_us = now_us;
        }
        b.awake = actor != 0;
    }
}

void Table::StepCopy(CoopPuppet::Guest& guest, Entry& e) {
    const auto owner_has = [&](std::uint32_t seq) -> const BossShot* {
        for (const BossShot& s : e.owner.recent) {
            if (s.seq == seq) {
                return &s;
            }
        }
        return nullptr;
    };
    // Ours showing an owner shot: moved to its place, or put back when it has ended there.
    for (Bullet& b : bullets_) {
        if (b.seq == 0) {
            continue;
        }
        const std::uint64_t actor = Live(guest, b, addr::kStateAwake);
        if (const BossShot* s = owner_has(b.seq)) {
            if (actor != 0) {
                guest.Write(actor + addr::kActorPosition, s->position.data(), sizeof s->position);
            }
            continue;  // (ended here first, on our Link: stays ended)
        }
        if (actor != 0) {
            Put(guest, actor, addr::kStateKept);
        }
        b.seq = 0;
    }
    // Our own AI's wakes: put back once the owner's shots come, before the owner's are woken, so a bullet our AI
    // took in this frame is free for the owner's shot.
    if (e.owner_came) {
        for (Bullet& b : bullets_) {
            if (b.seq != 0) {
                continue;
            }
            if (const std::uint64_t actor = Live(guest, b, addr::kStateAwake)) {
                Put(guest, actor, addr::kStateKept);
                ++counts_.copy_stopped;
            }
        }
    }
    // The owner's new shots: a kept bullet of the same name woken on its place (the newest room's first).
    for (const BossShot& s : e.owner.recent) {
        if (s.seq <= e.through) {
            continue;
        }
        e.through = s.seq;
        bool woken = false;
        for (auto it = bullets_.rbegin(); it != bullets_.rend(); ++it) {
            Bullet& b = *it;
            if (b.seq != 0 || b.name != s.name) {
                continue;
            }
            const std::uint64_t actor = Live(guest, b, addr::kStateKept);
            if (actor == 0) {
                continue;
            }
            guest.Write(actor + addr::kActorPosition, s.position.data(), sizeof s.position);
            Put(guest, actor, addr::kStateAwake);
            b.seq = s.seq;
            woken = true;
            ++counts_.fired;
            break;
        }
        if (!woken) {
            ++counts_.dropped;
            Note(e.member.name + ": shots: no kept " + s.name + " free for the owner's shot " +
                 std::to_string(s.seq));
        }
    }
}

std::optional<CoopPuppet::Call> Table::Step(CoopPuppet::Guest& guest, bool, std::uint64_t, std::uint64_t main_base,
                                            std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    if (main_base != main_base_) {
        if (main_base_ != 0) {
            Note("shots: a new game: bosses and bullets dropped");
            DropGame();
        }
        main_base_ = main_base;
    }
    if (!on_ || main_base == 0) {
        return std::nullopt;
    }
    ReadActors(guest);
    if (entries_.empty()) {
        return std::nullopt;
    }
    Entry& e = entries_.begin()->second;  // (one Twinrova: her bullets are the room's)
    if (e.member.role == Role::owner) {
        StepOwner(guest, e, now_us);
    } else {
        StepCopy(guest, e);
    }
    return std::nullopt;
}

void Table::DropGame() {
    entries_.clear();
    bullets_.clear();
}

void Table::Reset() {
    std::lock_guard lock(mutex_);
    DropGame();
    main_base_ = 0;
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
