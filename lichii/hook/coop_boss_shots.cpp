// SPDX-License-Identifier: GPL-3.0-or-later
//
// The owner's Twinrova shots known in every game (Twinrova hand-over): coop_boss_shots.h. Reads only.
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

bool Finite(const coop::protocol::Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

}  // namespace

bool Covers(std::string_view boss_name) { return boss_name == "Enemy_DungeonBoss_Twinrova"; }

bool RoomBoss(std::string_view boss_name) {
    return Covers(boss_name) || boss_name == "Enemy_DungeonBoss_Koume" || boss_name == "Enemy_DungeonBoss_Kotake";
}

bool IsBullet(std::string_view name) {
    return name.starts_with("Twinrova_Bullet_") && coop::protocol::ValidShotName(name);
}

void Table::SetOn(bool on) {
    std::lock_guard lock(mutex_);
    on_ = on;
    if (!on) {
        // (creates are not seen while off: the bullets known now may be a room's that is gone by the next on)
        entries_.clear();
        bullets_.clear();
        scene_.clear();
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

// Whether `actor` is the bullet `name`: the name, the actor class's vtable, then a state that is not a delete's.
// `state` is its actor state. An actor with no class or name yet is being made (or its memory is free).
Table::Look Table::LookAt(CoopPuppet::Guest& guest, std::uint64_t actor, const std::string& name,
                          std::uint32_t& state) {
    std::uint64_t vtable = 0, text = 0;
    if (!Get(guest, actor, vtable) || vtable == 0 || !Get(guest, actor + CoopPuppet::addr::kActorName, text) ||
        text == 0) {
        return Look::making;
    }
    std::string read(name.size() + 1, '\0');
    if (!guest.Read(text, read.data(), read.size()) || read.compare(0, name.size(), name) != 0 ||
        read[name.size()] != '\0') {
        return Look::other;
    }
    if (vtable != main_base_ + CoopPuppet::addr::kActorVtable) {
        if (!class_noted_) {
            class_noted_ = true;
            Note("shots: an actor named " + name + " is of another class: not used");
        }
        return Look::other;
    }
    if (!Get(guest, actor + addr::kActorState, state) ||
        (state >= addr::kStateGoneFirst && state <= addr::kStateGoneLast)) {
        return Look::other;
    }
    return Look::bullet;
}

// The game's take of a made actor clears the create's slot (docs/addresses.md, kRequestActor), and her bullets are
// made in the intro, before she is listed: each actor is read while its slot still names it, and the slot is not
// read again (it is used for other creates). Until the game writes this create's request the slot may still name
// another create's actor, and after a failed one it always does: neither is taken, and it is looked for until kReadUs.
void Table::ReadActors(CoopPuppet::Guest& guest, std::uint64_t now_us) {
    for (std::size_t i = 0; i < bullets_.size();) {
        Bullet& b = bullets_[i];
        if (b.actor == 0) {
            if (b.since_us == 0) {
                b.since_us = now_us;
            }
            const std::uint64_t actor = ActorOf(guest, b.handle);
            std::uint32_t state = 0;
            const bool held = actor != 0 && std::any_of(bullets_.begin(), bullets_.end(), [&](const Bullet& o) {
                return o.actor == actor;  // one actor is one bullet
            });
            if (actor != 0 && !held && LookAt(guest, actor, b.name, state) == Look::bullet) {
                b.actor = actor;
                b.handle = 0;
                b.awake = state == addr::kStateAwake;  // (awake already: no wake of it was seen)
            } else if (now_us > b.since_us + kReadUs) {
                ++counts_.lost;
                bullets_.erase(bullets_.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        }
        ++i;
    }
}

void Table::KeepBullet(std::uint64_t handle, std::string_view name) {
    if (handle == 0 || !IsBullet(name)) {
        return;
    }
    std::lock_guard lock(mutex_);
    Bullet made;
    made.handle = handle;
    made.name = std::string(name);
    for (Bullet& b : bullets_) {
        if (b.handle == handle) {
            b = std::move(made);  // (its actor was never read, and the slot is this create's now)
            return;
        }
    }
    if (bullets_.size() >= kMaxBullets) {
        ++counts_.lost;
        bullets_.erase(bullets_.begin());  // (an older room's)
    }
    bullets_.push_back(std::move(made));
}

void Table::ForgetShots() {
    for (Bullet& b : bullets_) {
        b.seq = 0;
        b.awake = true;  // (in flight or not: what counts is its next wake)
    }
}

void Table::Publish(std::span<const Member> members, std::string_view scene) {
    std::lock_guard lock(mutex_);
    if (!on_) {
        return;
    }
    if (!scene_.empty() && scene != scene_) {
        // Her room was left, in either phase. A bullet made since (the next room's) was never listed with her.
        const std::size_t before = bullets_.size();
        std::erase_if(bullets_, [](const Bullet& b) { return b.room; });
        counts_.lost += before - bullets_.size();
        Note("shots: her room " + scene_ + " left: " + std::to_string(before - bullets_.size()) + " bullets dropped");
        scene_.clear();
        entries_.clear();
    }
    std::map<std::uint64_t, Entry> next;
    for (const Member& m : members) {
        if (RoomBoss(m.name)) {
            scene_ = scene;
            for (Bullet& b : bullets_) {
                b.room = true;
            }
        }
        if (!Covers(m.name)) {
            continue;
        }
        auto it = entries_.find(m.actor);
        Entry e;
        if (it == entries_.end()) {
            ForgetShots();  // (listed first, or again after a leave)
        } else if (it->second.member.role != m.role || it->second.member.owner != m.owner ||
                   it->second.member.epoch != m.epoch) {
            Note(m.name + ": shots: " +
                 (m.role == Role::owner ? std::string("ours now: our own AI fires")
                                        : "player " + std::to_string(m.owner) + "'s now: its shots come"));
            ForgetShots();
            ++counts_.restarts;
        } else {
            e = std::move(it->second);
        }
        e.member = m;
        next.emplace(m.actor, std::move(e));
    }
    entries_ = std::move(next);
}

void Table::Leave() {
    std::lock_guard lock(mutex_);
    entries_.clear();
    ForgetShots();
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
    if (out.recent.empty() && now_us > it->second.ended_us + kKeepUs) {
        return std::nullopt;
    }
    return out;
}

// A shot of one side: with the oldest one of the other side that waits for it, or it waits itself.
void Table::Pair(std::vector<Pending>& with, std::vector<Pending>& wait, std::string_view name,
                 std::uint64_t now_us) {
    const auto it = std::find_if(with.begin(), with.end(), [&](const Pending& p) { return p.name == name; });
    if (it != with.end()) {
        with.erase(it);
        ++counts_.paired;
    } else if (wait.size() < kMaxPending) {
        wait.push_back({std::string(name), now_us});
    }
}

void Table::Expire(Entry& e, std::uint64_t now_us) {
    const auto drop = [&](std::vector<Pending>& list, std::uint64_t& count, bool counted, std::string_view what) {
        std::erase_if(list, [&](const Pending& p) {
            if (now_us <= p.at_us + kPairUs) {
                return false;
            }
            if (counted) {
                ++count;
                Note(e.member.name + ": shots: " + p.name + ": " + std::string(what));
            }
            return true;
        });
    };
    drop(e.owners, counts_.missing, true, "the owner's, with no bullet of ours");
    // (an owner with no samples of shots yet: an older game, or none fired: nothing to compare with)
    drop(e.owns, counts_.extra, e.owner_came, "ours, with no shot of the owner's");
}

void Table::Receive(std::uint64_t actor, const BossShots& shots, std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    const auto it = entries_.find(actor);
    if (!on_ || it == entries_.end() || it->second.member.role != Role::copy) {
        return;
    }
    Entry& e = it->second;
    if (!e.owner_came) {
        e.owner_came = true;
        e.through = shots.recent.empty() ? shots.seq : shots.recent.front().seq - 1;  // (what it fired before is past)
        Note(e.member.name + ": shots: the owner's came");
    }
    // Each seq once: an older list (a late packet) holds only seqs counted already. A new owner's count starts again
    // in Publish (its ownership has another epoch).
    std::uint32_t listed = 0;
    for (const BossShot& s : shots.recent) {
        if (s.seq > e.through) {
            ++listed;
            ++counts_.seen;
            Pair(e.owns, e.owners, s.name, now_us);
        }
    }
    if (shots.seq > e.through) {
        counts_.unseen += shots.seq - e.through - listed;
        e.through = shots.seq;
    }
}

// Each bullet's wakes: the owner's shots, or a copy's own. A bullet that is not itself any more is dropped.
void Table::ReadWakes(CoopPuppet::Guest& guest, Entry* e, std::uint64_t now_us) {
    const bool ours = e != nullptr && e->member.role == Role::owner;
    for (std::size_t i = 0; i < bullets_.size();) {
        Bullet& b = bullets_[i];
        if (b.actor == 0) {
            ++i;
            continue;
        }
        std::uint32_t state = 0;
        const bool itself = LookAt(guest, b.actor, b.name, state) == Look::bullet;
        const bool awake = itself && state == addr::kStateAwake;
        if (!awake && b.awake && b.seq != 0) {
            if (ours) {
                e->ended_us = now_us;
            }
            b.seq = 0;
        }
        if (!itself) {
            ++counts_.lost;
            bullets_.erase(bullets_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (awake && ours) {
            coop::protocol::Vec3 p{};
            if (guest.Read(b.actor + addr::kActorPosition, p.data(), sizeof p) && Finite(p)) {
                b.position = p;
            }
        }
        if (awake && !b.awake && ours) {
            b.seq = e->seq = ++newest_seq_;  // a wake: one shot
            ++counts_.owner_kept;
        } else if (awake && !b.awake && e != nullptr) {
            ++counts_.own;
            Pair(e->owners, e->owns, b.name, now_us);
        }
        b.awake = awake;
        ++i;
    }
}

std::optional<CoopPuppet::Call> Table::Step(CoopPuppet::Guest& guest, bool, std::uint64_t, std::uint64_t main_base,
                                            std::uint64_t now_us) {
    std::lock_guard lock(mutex_);
    if (main_base != 0 && main_base != main_base_) {
        if (main_base_ != 0) {
            Note("shots: a new game: bosses and bullets dropped");
            DropGame();
        }
        main_base_ = main_base;
    }
    if (!on_ || main_base_ == 0) {
        return std::nullopt;
    }
    ReadActors(guest, now_us);
    // (one Twinrova: her bullets are the room's)
    Entry* e = entries_.empty() ? nullptr : &entries_.begin()->second;
    ReadWakes(guest, e, now_us);
    if (e != nullptr) {
        Expire(*e, now_us);
    }
    return std::nullopt;
}

void Table::DropGame() {
    entries_.clear();
    bullets_.clear();
    scene_.clear();
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

std::size_t Table::Bullets() const {
    std::lock_guard lock(mutex_);
    return bullets_.size();
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
