// SPDX-License-Identifier: GPL-3.0-or-later
//
// The play-test bridge's view of the world near our Link (docs/playtest.md, "State"): the actors the object walk
// sees, with a kind, a place, a life and an owner, sorted by distance. The walk (coop_objects.cpp) fills a snapshot
// about every 100 ms, only after the bridge asked for it (Enable); the bridge reads it from its own thread.
// This file is the game-free part: the kind table, the owner and alive rules, the sort-and-cut, the store and the
// freshness rule. Tested in tests/lichii/objects_world_test.cpp.
//
// Off (nothing called Enable): the walk does two relaxed atomic loads (one before the actor loop, one after it) and
// nothing else.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopWorldSnapshot {

enum class Kind { player, enemy, npc, chest, door, switch_, item, other };

inline const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::player: return "player";
    case Kind::enemy: return "enemy";
    case Kind::npc: return "npc";
    case Kind::chest: return "chest";
    case Kind::door: return "door";
    case Kind::switch_: return "switch";
    case Kind::item: return "item";
    case Kind::other: break;
    }
    return "other";
}

inline std::optional<Kind> KindFromName(std::string_view text) {
    for (const Kind k : {Kind::player, Kind::enemy, Kind::npc, Kind::chest, Kind::door, Kind::switch_, Kind::item,
                         Kind::other}) {
        if (text == KindName(k)) {
            return k;
        }
    }
    return std::nullopt;
}

// The kind of an actor by its name. The prefixes are the ones the code and docs/sync-coverage.md already use
// (Enemy_, Npc_, Player_, Obj_TBox_, Item_, Obj_*Door*, Obj_*Switch*). Door and switch names are matched by words in
// the name, not read from the game: a name this table does not know is "other" (never a wrong "enemy").
inline Kind KindOf(std::string_view name) {
    const auto starts = [&](std::string_view p) { return name.substr(0, p.size()) == p; };
    const auto has = [&](std::string_view w) { return name.find(w) != std::string_view::npos; };
    if (starts("Player_") || starts("CoopPuppet_")) return Kind::player;
    if (starts("Enemy_")) return Kind::enemy;
    if (starts("Npc_")) return Kind::npc;
    if (starts("Obj_TBox")) return Kind::chest;
    if (starts("Item_") || starts("Obj_Rupee") || starts("Obj_Heart") || starts("Obj_Dungeon_SmallKey")) {
        return Kind::item;
    }
    if (starts("Obj_") && (has("Door") || has("Shutter"))) return Kind::door;
    if (starts("Obj_") && has("Switch")) return Kind::switch_;
    return Kind::other;
}

// `OwnerOf` gives the player number, 0 for the server, nothing when the actor is not known to the sharing.
// JSON: "server", the number, or null (never "0 = none").
inline std::string OwnerJson(const std::optional<int>& owner) {
    if (!owner) return "null";
    if (*owner == 0) return "\"server\"";
    return std::to_string(*owner);
}

// Dead actors leave the list, and life is read only for enemies: "alive" is known only when life is.
inline std::optional<bool> AliveFrom(const std::optional<int>& life) {
    if (!life) return std::nullopt;
    return *life > 0;
}

struct WorldActor {
    std::uint64_t id = 0;
    std::string name;
    Kind kind = Kind::other;
    std::array<float, 3> pos{};
    float dist = 0;  // from our Link (0 when it is not known)
    std::optional<int> life, max_life;
    std::optional<int> owner;  // 0: the server
    std::optional<bool> alive;
};

struct Candidate {
    std::uint64_t id = 0;
    const std::string* name = nullptr;
    std::array<float, 3> pos{};
    std::optional<int> life, max_life;
    std::optional<int> owner;
};

// The kinds that matter first (not "other"), nearest first, ties by id; cut at `limit`. Names are copied only for
// what is kept.
inline std::vector<WorldActor> Select(const std::vector<Candidate>& in, const std::optional<std::array<float, 3>>& from,
                                      std::size_t limit) {
    struct Ranked {
        std::size_t index;
        Kind kind;
        float dist;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        float d = 0;
        if (from) {
            for (std::size_t k = 0; k < 3; ++k) {
                const float x = in[i].pos[k] - (*from)[k];
                d += x * x;
            }
            d = std::sqrt(d);
        }
        ranked.push_back({i, in[i].name ? KindOf(*in[i].name) : Kind::other, d});
    }
    const auto before = [&](const Ranked& a, const Ranked& b) {
        const bool ao = a.kind == Kind::other, bo = b.kind == Kind::other;
        if (ao != bo) return bo;
        if (a.dist != b.dist) return a.dist < b.dist;
        return in[a.index].id < in[b.index].id;
    };
    const std::size_t keep = std::min(limit, ranked.size());
    std::partial_sort(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(keep), ranked.end(), before);
    std::vector<WorldActor> out;
    out.reserve(keep);
    for (std::size_t i = 0; i < keep; ++i) {
        const Candidate& c = in[ranked[i].index];
        WorldActor a;
        a.id = c.id;
        a.name = c.name ? *c.name : std::string();
        a.kind = ranked[i].kind;
        a.pos = c.pos;
        a.dist = ranked[i].dist;
        a.life = c.life;
        a.max_life = c.max_life;
        a.owner = c.owner;
        a.alive = AliveFrom(c.life);
        out.push_back(std::move(a));
    }
    return out;
}

struct Snapshot {
    std::vector<WorldActor> actors;
    std::uint64_t frame = 0;     // the game frame (the bridge's count) when the walk began
    std::uint64_t taken_ms = 0;  // steady clock, ms
    std::uint64_t cost_us = 0;   // what building it took (the walk's own time, for the lane check)
    bool solo = false;  // from the solo walk (no session): names and places only; a strike by id is not run there
};

inline constexpr std::uint64_t kStaleMs = 1500;    // a snapshot older than this (wall clock) ...
inline constexpr std::uint64_t kStaleFrames = 30;  // ... and this many frames behind the game is not shown
inline constexpr std::size_t kLimit = 200;       // actors kept per snapshot (the dump lists all of them)
inline constexpr std::size_t kStateLimit = 40;   // actors of a known kind that `state` lists (nearest first)

// A snapshot is stale when the walk stopped (a load, a closed room): it is older than kStaleMs AND the game has run
// kStaleFrames frames past it. The walk runs on the game's presents, which a held game does not make, so a game held
// for minutes at the frame of its last snapshot still has a current list; the wall clock alone would call it stale.
inline bool IsStale(std::uint64_t snapshot_frame, std::uint64_t now_frame, std::uint64_t age_ms) {
    return age_ms > kStaleMs && now_frame > snapshot_frame && now_frame - snapshot_frame >= kStaleFrames;
}

enum class Freshness { current, behind, stale };
// stale: the walk stopped (a load, a closed room). behind: the game is further on than the snapshot (a held game
// catches up within a few presents). The snapshot is shown in both cases, with its frame.
inline Freshness Judge(std::uint64_t snapshot_frame, std::uint64_t now_frame, std::uint64_t age_ms) {
    if (IsStale(snapshot_frame, now_frame, age_ms)) return Freshness::stale;
    return snapshot_frame < now_frame ? Freshness::behind : Freshness::current;
}

// Whether a held `state` should wait for a newer snapshot: only when one exists, is not stale and is behind the frame.
// With none (solo: the walk does not run) or a stale one, no snapshot is coming, so waiting would only cost time.
inline bool WorthWaiting(const std::optional<Snapshot>& snap, std::uint64_t now_frame, std::uint64_t now_ms) {
    if (!snap) return false;
    const std::uint64_t age = now_ms >= snap->taken_ms ? now_ms - snap->taken_ms : 0;
    return !IsStale(snap->frame, now_frame, age) && snap->frame < now_frame;
}

// One writer (the walk, present thread), one reader (the bridge). Any thread may call Enabled.
class Store {
public:
    using FrameFn = std::uint64_t (*)();

    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }
    // The bridge turns it on once and gives the game-frame source.
    void Enable(FrameFn frame) {
        frame_.store(frame);
        enabled_.store(true);
    }
    void Disable() {
        enabled_.store(false);
        Clear();
    }
    std::uint64_t Frame() const {
        const FrameFn f = frame_.load();
        return f ? f() : 0;
    }
    void Put(Snapshot s) {
        std::lock_guard lock(mutex_);
        snapshot_ = std::move(s);
        has_ = true;
    }
    void Clear() {  // (called by Disable only; a scene change or a closed session shows as `stale` after 1.5 s)
        std::lock_guard lock(mutex_);
        snapshot_ = Snapshot{};
        has_ = false;
    }
    std::optional<Snapshot> Get() const {
        std::lock_guard lock(mutex_);
        if (!has_) return std::nullopt;
        return snapshot_;
    }

private:
    std::atomic<bool> enabled_{false};
    std::atomic<FrameFn> frame_{nullptr};
    mutable std::mutex mutex_;
    Snapshot snapshot_;
    bool has_ = false;
};

inline Store& TheStore() {
    static Store store;
    return store;
}

}  // namespace CoopWorldSnapshot
