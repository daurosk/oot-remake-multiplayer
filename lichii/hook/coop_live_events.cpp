// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_live_events.h"
#include "coop_boss.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace CoopLiveEvents {

namespace {

// Near one of the event's places in this scene; nullopt when it has none here.
std::optional<bool> NearPlace(std::string_view event, std::string_view scene,
                              const std::optional<std::array<float, 3>>& link) {
    bool any = false;
    for (const Place& p : Places()) {
        if (p.event != event || p.scene != scene) {
            continue;
        }
        any = true;
        if (!link) {
            continue;
        }
        const float dx = (*link)[0] - p.x;
        const float dy = (*link)[1] - p.y;
        const float dz = (*link)[2] - p.z;
        if (dx * dx + dy * dy + dz * dz <= kNearM * kNearM) {
            return true;
        }
    }
    return any ? std::optional<bool>(false) : std::nullopt;
}

} // namespace

std::optional<std::string> Decider::Next(const std::vector<std::string>& held, const std::string& scene,
                                         bool event_running, std::optional<std::array<float, 3>> link) {
    if (scene.empty()) {
        return std::nullopt;  // no scene read (a load): the same visit, nothing asked
    }
    if (scene != scene_) {
        scene_ = scene;
        asked_.clear();
    }
    if (event_running) {
        return std::nullopt;
    }
    for (const std::string& name : held) {
        const bool named = allowed_.contains(name);
        if (!allowed_.empty() && !named) {
            continue;
        }
        if (asked_.contains(name)) {
            continue;
        }
        const auto near = NearPlace(name, scene, link);
        if (near ? !*near : !named) {
            continue;  // not near it yet (it waits), or no place known and not asked by name
        }
        asked_.insert(name);
        return name;
    }
    return std::nullopt;
}

std::optional<std::string> SceneSync::Next(const std::vector<OtherPlayer>& others, const std::string& scene,
                                           bool event_running, std::optional<std::array<float, 3>> link,
                                           const std::optional<std::string>& own_cutscene) {
    if (!scene.empty() && scene != scene_) {
        scene_ = scene;
        asked_.clear();
    }
    if (own_cutscene) {
        asked_.insert(*own_cutscene);  // played here: never asked again this visit
    }
    std::optional<std::string> start;
    for (const OtherPlayer& o : others) {
        auto& last = last_[o.number];
        const bool began = o.cutscene.has_value() && o.cutscene != last;
        last = o.cutscene;
        if (!began || start || scene.empty() || event_running) {
            continue;  // seen, so a start we could not join is not joined later
        }
        if (!o.scene || *o.scene != scene || !link || !o.place) {
            continue;
        }
        if (CoopBoss::RoomAt(scene, CoopBoss::Vec{(*link)[0], (*link)[1], (*link)[2]}) != nullptr) {
            continue;  // a boss room: its scenes are the boss's own to start (coop_boss.h)
        }
        const float dx = (*link)[0] - (*o.place)[0];
        const float dy = (*link)[1] - (*o.place)[1];
        const float dz = (*link)[2] - (*o.place)[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) > kTogetherM) {
            continue;
        }
        if (asked_.insert(*o.cutscene).second) {
            start = *o.cutscene;
        }
    }
    return start;
}

std::optional<std::set<std::string>> ParseNames(std::string_view text) {
    std::set<std::string> out;
    const auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
        return s;
    };
    if (trim(text) == "all") {
        return out;
    }
    while (!text.empty()) {
        const auto comma = text.find(',');
        const std::string_view part = trim(text.substr(0, comma));
        if (!part.empty()) {
            out.emplace(part);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text.remove_prefix(comma + 1);
    }
    if (out.empty()) {
        return std::nullopt;  // no names: not "all"
    }
    return out;
}

void Requester::Want(std::string name) {
    if (name.empty()) {
        notes_.push_back("live event with no name not asked");
        return;
    }
    if (name.size() >= 0x80 || wanted_.size() >= 8) {
        notes_.push_back("live event " + name + " not asked (name too long or too many waiting)");
        return;
    }
    if (std::find(wanted_.begin(), wanted_.end(), name) == wanted_.end() && name != current_) {
        wanted_.push_back(std::move(name));
    }
}

std::vector<std::string> Requester::TakeNotes() {
    std::vector<std::string> out;
    out.swap(notes_);
    return out;
}

std::optional<CoopPuppet::Call> Requester::FreeFirst(CoopPuppet::Guest& guest, std::uint64_t main) {
    const std::uint64_t vtable = main + addr::kStringVtable;
    guest.Write(block_ + addr::kArgMember1, &vtable, 8);
    CoopPuppet::Call c;
    c.function = main + addr::kStringFree;
    c.x[0] = block_ + addr::kArgMember1;
    next_ = Next::free1;
    return c;
}

bool Requester::EventRunning(CoopPuppet::Guest& guest, std::uint64_t main) {
    std::uint64_t manager = 0;
    std::uint64_t session = 0;
    // docs/dungeons-boss.md: an event runs while [[main + 0x4e94840] + 0x68] is set.
    if (!guest.Read(main + addr::kEventStatePtr, &manager, 8) || manager == 0) {
        return false;
    }
    return guest.Read(manager + addr::kEventSession, &session, 8) && session != 0;
}

std::optional<CoopPuppet::Call> Requester::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                                std::uint64_t main) {
    CoopPuppet::Call c;
    if (after_call) {
        const Next done = next_;
        next_ = Next::none;
        switch (done) {
        case Next::malloc:
            if (result == 0) {
                malloc_failed_ = true;
                notes_.push_back("live events: the game's malloc gave nothing; live events off");
                return std::nullopt;
            }
            block_ = result;
            break;  // then build, below
        case Next::build: {
            // The name into the arg's own buffer, as the node copies it.
            std::uint64_t buffer = 0;
            std::uint32_t cap = 0;
            if (!guest.Read(block_ + addr::kArgName, &buffer, 8) || !guest.Read(block_ + addr::kArgNameCap, &cap, 4) ||
                buffer == 0 || cap == 0) {
                notes_.push_back("live event " + current_ + ": the request has no name buffer; not asked");
                return FreeFirst(guest, main);  // the build made its members: free them
            }
            const std::size_t n = std::min<std::size_t>(current_.size(), cap - 1);
            std::string text = current_.substr(0, n);
            text.push_back('\0');
            if (!guest.Write(buffer, text.data(), text.size())) {
                notes_.push_back("live event " + current_ + ": name not written; not asked");
                return FreeFirst(guest, main);
            }
            std::uint64_t holder = 0;
            std::uint64_t manager = 0;
            if (!guest.Read(main + addr::kManagerPtr, &holder, 8) || holder == 0 || !guest.Read(holder, &manager, 8) ||
                manager == 0) {
                notes_.push_back("live event " + current_ + ": no event manager; not asked");
                return FreeFirst(guest, main);  // still free what the build made
            }
            c.function = main + addr::kRequest;
            c.x[0] = manager;
            c.x[1] = block_;
            next_ = Next::request;
            return c;
        }
        case Next::request: {
            notes_.push_back("live event " + current_ + " asked of the game (result " + std::to_string(result & 0xff) +
                             ")");
            return FreeFirst(guest, main);
        }
        case Next::free1: {
            const std::uint64_t vtable = main + addr::kStringVtable;
            guest.Write(block_ + addr::kArgMember2, &vtable, 8);
            c.function = main + addr::kStringFree;
            c.x[0] = block_ + addr::kArgMember2;
            next_ = Next::free2;
            return c;
        }
        case Next::free2:
            current_.clear();
            return std::nullopt;  // one event a frame
        case Next::none:
            return std::nullopt;
        }
    }
    if (next_ != Next::none || malloc_failed_ || main == 0) {
        return std::nullopt;
    }
    if (block_ == 0) {
        if (wanted_.empty()) {
            return std::nullopt;
        }
        c.function = main + addr::kMalloc;
        c.x[0] = addr::kArgSize;
        next_ = Next::malloc;
        return c;
    }
    if (current_.empty() && wanted_.empty()) {
        return std::nullopt;
    }
    if (EventRunning(guest, main)) {
        return std::nullopt;  // an event began since it was asked: wait, it stays queued
    }
    if (current_.empty()) {
        current_ = std::move(wanted_.front());
        wanted_.pop_front();
    }
    c.function = main + addr::kArgBuild;
    c.x[0] = block_;
    next_ = Next::build;
    return c;
}

} // namespace CoopLiveEvents
