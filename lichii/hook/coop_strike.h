// SPDX-License-Identifier: GPL-3.0-or-later
// The play-test strike (the bridge's `kill` and `hit` acts, the script steps "kill NAME", "kill ID NAME", "hit NAME N"
// and "hit ID NAME N"): which actor of the objects walk a strike lands on, and the result the walk leaves for the
// bridge's `state` (docs/playtest.md). Pure, no game access: the walk (coop_objects.cpp) fills in the candidates.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopStrike {

// An actor the walk knows by name and place but does not share (enemy shots, the Scrub Brothers, extras the director
// has not bound): the world list shows it, a test strike cannot reach it.
struct Listed {
    std::uint64_t address = 0;
    std::string_view name;
};

// One actor of the walk's own list.
struct Candidate {
    std::uint64_t address = 0;
    std::string_view name;
    std::optional<std::int32_t> life;  // none: no Life component
    float metres = 0.0f;               // from our Link
};

inline constexpr std::string_view kDone = "done";
inline constexpr std::string_view kGone = "gone";
inline constexpr std::string_view kNoLife = "none with a life";
inline constexpr std::string_view kNotShared = "not shared";

struct Pick {
    std::string_view result = kNoLife;  // done, gone, not shared or none with a life
    std::size_t index = 0;              // into the candidates, when done
};

// `id` 0: the nearest actor of `name` with a life above 0 (the old "kill NAME"). `id` set: exactly the actor at that
// address, and only when it still has that name: no actor of the walk at that address, or one with another name, is
// `gone` (never another actor of the same name: a new actor at a freed address with the same name cannot be told
// apart from the old one, which docs/playtest.md accepts); the actor there with no life left is `none with a life`.
// `listed` (the actors the world list shows, optional): an id and name that are there but not among `actors` is
// `not shared`, not `gone`: the actor is right there and co-op does not share it.
inline Pick PickTarget(const std::vector<Candidate>& actors, std::string_view name, std::uint64_t id,
                       const std::vector<Listed>& listed = {}) {
    Pick pick;
    if (id != 0) {
        for (std::size_t i = 0; i < actors.size(); ++i) {
            if (actors[i].address != id || actors[i].name != name) continue;
            if (!actors[i].life || *actors[i].life <= 0) return pick;
            pick.result = kDone;
            pick.index = i;
            return pick;
        }
        pick.result = kGone;
        for (const Listed& l : listed) {
            if (l.address == id && l.name == name) {
                pick.result = kNotShared;
                break;
            }
        }
        return pick;
    }
    bool found = false;
    for (std::size_t i = 0; i < actors.size(); ++i) {
        if (actors[i].name != name || !actors[i].life || *actors[i].life <= 0) continue;
        if (!found || actors[i].metres < actors[pick.index].metres) {
            pick.index = i;
            found = true;
        }
    }
    if (found) pick.result = kDone;
    return pick;
}

// An actor co-op does not share that a test kill may still kill, in this game only: a Gold Skulltula in Not shared
// (race) mode is each game's own mob (coop_gold_mode.h), so a local kill is what a player's own hit does. Nothing else:
// the other unshared actors (enemy shots, the Scrub Brothers, unbound extras) stay `not shared`.
inline bool OwnGameMob(std::string_view name, bool gold_not_shared) {
    return gold_not_shared && name == "Enemy_Skulltula_Gold";
}

// What the walk left for the bridge about the newest strike it ran.
struct Result {
    std::uint64_t seq = 0;
    std::string result;  // done, gone, not shared or none with a life
    std::string name;
    std::uint64_t id = 0;  // the id asked for; 0: by name
};

class Book {
public:
    // A strike was asked for (any thread): its sequence number, which the walk's result carries.
    std::uint64_t Asked() { return ++asked_; }
    std::uint64_t LastAsked() const { return asked_.load(); }
    void Record(Result r) {
        std::lock_guard lock(mutex_);
        last_ = std::move(r);
    }
    std::optional<Result> Last() const {
        std::lock_guard lock(mutex_);
        return last_;
    }
    void Clear() {
        std::lock_guard lock(mutex_);
        last_.reset();
        asked_ = 0;
    }

private:
    std::atomic<std::uint64_t> asked_{0};
    mutable std::mutex mutex_;
    std::optional<Result> last_;
};

inline Book& TheBook() {
    static Book book;
    return book;
}

}  // namespace CoopStrike
