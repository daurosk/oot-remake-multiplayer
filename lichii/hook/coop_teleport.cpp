// SPDX-License-Identifier: GPL-3.0-or-later
//
// /teleport <player> (coop_teleport.h). No emulator code.

#include "coop_teleport.h"

#include <cmath>
#include <utility>

#include "coop_dungeon.h"
#include "coop_rooms.h"

namespace CoopTeleport {
namespace {

using coop::protocol::Age;
using coop::protocol::JoinRefuseReason;

struct StartPoint {
    std::string_view scene;
    std::string_view name;
    float x;
    float y;
    float z;
};

constexpr StartPoint kStartPoints[] = {
#include "coop_start_points.inc"
};

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) {
        s.remove_suffix(1);
    }
    return s;
}

std::string Shown(const Player& p) {
    return p.name.empty() ? "P" + std::to_string(p.number) : p.name;
}

const char* AgeName(Age age) {
    return age == Age::child ? "child" : "adult";
}

} // namespace

std::optional<Command> Parse(std::string_view text) {
    text = Trim(text);
    if (text.empty() || text.front() != '/') {
        return std::nullopt;
    }
    text.remove_prefix(1);
    const auto space = text.find_first_of(" \t");
    const std::string word = Lower(text.substr(0, space));
    if (word != "teleport" && word != "tp") {
        return std::nullopt;
    }
    Command c;
    const std::string_view rest = space == std::string_view::npos ? std::string_view{} : Trim(text.substr(space));
    if (rest.empty()) {
        c.kind = Command::Kind::list;
    } else {
        c.kind = Command::Kind::go;
        c.name = std::string(rest);
    }
    return c;
}

std::string ListLine(std::span<const Player> players, std::uint8_t self) {
    std::string line;
    for (const auto& p : players) {
        if (p.number == self) {
            continue;
        }
        line += line.empty() ? "" : ", ";
        line += Shown(p) + " (" + std::to_string(p.number) + ")";
    }
    if (line.empty()) {
        return "teleport: nobody else is here";
    }
    return "teleport to: " + line + ". Type /teleport <name>";
}

Found FindPlayer(std::string_view wanted, std::span<const Player> players, std::uint8_t self) {
    Found f;
    const std::string want = Lower(Trim(wanted));
    if (want.empty()) {
        f.error = ListLine(players, self);
        return f;
    }
    const auto found = [&](const Player& p) {
        if (p.number == self) {
            f.error = "teleport: that is you";
            return f;
        }
        f.number = p.number;
        f.name = Shown(p);
        return f;
    };
    // The whole name.
    for (const auto& p : players) {
        if (!p.name.empty() && Lower(p.name) == want) {
            return found(p);
        }
    }
    // A unique start of a name (not ours).
    const Player* only = nullptr;
    std::string several;
    int count = 0;
    for (const auto& p : players) {
        if (p.number == self || p.name.empty()) {
            continue;
        }
        if (Lower(p.name).rfind(want, 0) == 0) {
            ++count;
            only = &p;
            several += several.empty() ? "" : ", ";
            several += Shown(p);
        }
    }
    if (count == 1) {
        return found(*only);
    }
    if (count > 1) {
        f.error = "teleport: \"" + std::string(Trim(wanted)) + "\" could be " + several + ": type more of the name";
        return f;
    }
    // The number: "2" or "p2".
    std::string_view digits = want;
    if (!digits.empty() && digits.front() == 'p') {
        digits.remove_prefix(1);
    }
    if (digits.size() == 1 && digits[0] >= '1' && digits[0] <= '4') {
        const auto n = static_cast<std::uint8_t>(digits[0] - '0');
        for (const auto& p : players) {
            if (p.number == n) {
                return found(p);
            }
        }
    }
    f.error = "teleport: nobody called \"" + std::string(Trim(wanted)) + "\" is here. " + ListLine(players, self);
    return f;
}

std::optional<Age> AgeOfScene(std::string_view scene) {
    const auto ends = [&](std::string_view tail) {
        return scene.size() > tail.size() && scene.substr(scene.size() - tail.size()) == tail;
    };
    if (ends("_Child")) {
        return Age::child;
    }
    if (ends("_Adult")) {
        return Age::adult;
    }
    return std::nullopt;
}

void Grounded::Add(const Local& local, std::uint64_t now_us) {
    if (!local.scene || !local.position || local.event) {
        prev_.reset();
        return;
    }
    if (*local.scene != scene_) {
        scene_ = *local.scene;
        prev_.reset();
    }
    const Vec p = *local.position;
    if (!prev_) {
        prev_ = p;
        prev_us_ = now_us;
        since_us_ = now_us;
        return;
    }
    const double dt = static_cast<double>(now_us - prev_us_) / 1e6;
    if (dt <= 0.0) {
        return;
    }
    const double climb = std::fabs(static_cast<double>(p.y - prev_->y)) / dt;
    prev_ = p;
    prev_us_ = now_us;
    if (climb > kMaxClimbMps) {
        since_us_ = now_us;  // a jump, a fall, a climb, a lift
        return;
    }
    if (now_us - since_us_ >= kSteadyUs) {
        last_ = p;
        last_scene_ = scene_;
    }
}

Answer AnswerFor(const Local& local, const Grounded& grounded) {
    Answer a;
    if (!local.scene || !local.position || local.event) {
        a.reason = JoinRefuseReason::busy;
        return a;
    }
    if (local.fighting) {
        a.reason = JoinRefuseReason::cannot_warp;
        return a;
    }
    const auto at = grounded.Last();
    if (!at || grounded.Scene() != *local.scene) {
        a.reason = JoinRefuseReason::busy;  // not stood still in this scene yet (just loaded, falling)
        return a;
    }
    a.accept = true;
    a.target.scene = *local.scene;
    a.target.position = {at->x, at->y, at->z};
    a.target.rotation = local.rotation.value_or(coop::protocol::Quat{0.0f, 0.0f, 0.0f, 1.0f});
    return a;
}

std::string RefusedLine(std::string_view name, JoinRefuseReason reason) {
    const std::string n(name);
    switch (reason) {
    case JoinRefuseReason::busy:
        return "teleport: " + n + " is busy (loading, a cutscene, a talk or in the air); try again soon";
    case JoinRefuseReason::declined:
        return "teleport: " + n + " said no";
    case JoinRefuseReason::cannot_warp:
        return "teleport: " + n + " is in a boss fight; no teleport there";
    case JoinRefuseReason::not_there:
        return "teleport: " + n + " is not in the game";
    }
    return "teleport: " + n + " refused (reason " + std::to_string(static_cast<int>(reason)) + ")";
}

std::optional<std::string> NearestStart(std::string_view scene, const Vec& to) {
    const StartPoint* best = nullptr;
    float best_d = 0.0f;
    for (const auto& p : kStartPoints) {
        if (p.scene != scene) {
            continue;
        }
        const float dx = p.x - to.x;
        const float dy = p.y - to.y;
        const float dz = p.z - to.z;
        const float d = dx * dx + dy * dy + dz * dz;
        if (best == nullptr || d < best_d) {
            best = &p;
            best_d = d;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return std::string(best->name);
}

Trip::Actions Trip::Start(std::uint8_t number, std::string name, const Local& local, std::uint64_t now_us) {
    Actions a;
    if (phase_ == Phase::travelling) {
        a.cancel_scene = true;  // the old one's scene change is no longer wanted
        a.restore_start = !loaded_;
    }
    phase_ = Phase::idle;
    if (!local.scene || !local.position) {
        a.lines.push_back("teleport: wait until your game is loaded");
        return a;
    }
    if (local.event) {
        a.lines.push_back("teleport: not during a cutscene or a talk");
        return a;
    }
    if (local.fighting) {
        a.lines.push_back("teleport: not during your boss fight");
        return a;
    }
    phase_ = Phase::asking;
    player_ = number;
    name_ = std::move(name);
    since_us_ = now_us;
    a.ask = number;
    a.lines.push_back("teleport: asking " + name_ + " where they are");
    return a;
}

Trip::Actions Trip::Accepted(std::uint8_t number, const coop::protocol::JoinTarget& target, const Local& local,
                             std::uint64_t now_us) {
    Actions a;
    if (number != player_ || (phase_ != Phase::asking && phase_ != Phase::asking_again)) {
        return a;
    }
    const Vec to{target.position[0], target.position[1], target.position[2]};
    const bool same = local.scene && *local.scene == target.scene;
    if (phase_ == Phase::asking_again) {
        phase_ = Phase::idle;
        if (same) {
            a.move = to;
            a.lines.push_back("teleport: next to " + name_);
        } else {
            a.move = at_;  // they left that scene meanwhile: where they were
            a.lines.push_back("teleport: " + name_ + " has gone on to " + target.scene +
                              "; you are where they were. Type /teleport again to follow");
        }
        return a;
    }
    phase_ = Phase::idle;
    if (!local.scene || !local.position || local.event || local.fighting) {
        a.lines.push_back("teleport: stopped: your game got busy (a load, a cutscene or a fight)");
        return a;
    }
    const auto theirs = AgeOfScene(target.scene);
    if (theirs && local.age && *theirs != *local.age) {
        a.lines.push_back("teleport: " + name_ + " is in " + target.scene + ", a place for " + AgeName(*theirs) +
                          " Link; yours is " + AgeName(*local.age));
        return a;
    }
    if (same) {
        a.move = to;
        a.lines.push_back("teleport: next to " + name_);
        return a;
    }
    const auto start = NearestStart(target.scene, to);
    if (!start) {
        a.lines.push_back("teleport: " + name_ + " is in " + target.scene +
                          ", a place /teleport cannot load (no start point known there)");
        return a;
    }
    phase_ = Phase::travelling;
    scene_ = target.scene;
    start_ = *start;
    at_ = to;
    since_us_ = now_us;
    asked_us_ = now_us;
    event_us_ = now_us;
    settle_us_ = 0;
    taken_ = false;
    seen_request_ = false;
    loaded_ = false;
    a.scene = scene_;
    a.start = start_;
    a.start_event = kStartSceneEvent;  // no event runs here (Accepted refuses during one)
    a.lines.push_back("teleport: going to " + name_ + " in " + scene_);
    if (!kStartSceneEvent) {
        a.lines.push_back("teleport: talk to someone or read a sign within a minute to go there");
    }
    if (CoopDungeon::IsDungeonScene(scene_)) {
        a.lines.push_back("teleport: " + name_ + " is in a dungeon: you may need its items to get on from there");
    }
    return a;
}

Trip::Actions Trip::Refused(std::uint8_t number, JoinRefuseReason reason) {
    Actions a;
    if (number != player_ || (phase_ != Phase::asking && phase_ != Phase::asking_again)) {
        return a;
    }
    if (phase_ == Phase::asking_again && reason == JoinRefuseReason::busy) {
        a.move = at_;  // we are in their scene: where they stood when they first answered
        a.lines.push_back("teleport: " + name_ + " is busy now; you are where they were");
    } else {
        a.lines.push_back(RefusedLine(name_, reason));
    }
    phase_ = Phase::idle;
    return a;
}

Trip::Actions Trip::TimedOut(std::uint8_t number) {
    Actions a;
    if (number != player_ || (phase_ != Phase::asking && phase_ != Phase::asking_again)) {
        return a;
    }
    if (phase_ == Phase::asking_again) {
        a.move = at_;
        a.lines.push_back("teleport: no answer from " + name_ + "; you are where they were");
    } else {
        a.lines.push_back("teleport: no answer from " + name_ + " (an older build, or their game is busy)");
    }
    phase_ = Phase::idle;
    return a;
}

Trip::Actions Trip::Left(std::uint8_t number) {
    Actions a;
    if (phase_ == Phase::idle || (number != 0 && number != player_)) {
        return a;
    }
    a.cancel_scene = phase_ == Phase::travelling;
    a.restore_start = a.cancel_scene && !loaded_;
    a.lines.push_back(number == 0 ? "teleport: stopped: the connection was lost"
                                  : "teleport: stopped: " + name_ + " left");
    phase_ = Phase::idle;
    return a;
}

Trip::Actions Trip::Tick(const Local& local, std::uint64_t now_us) {
    Actions a;
    if (phase_ == Phase::travelling) {
        if (local.scene && *local.scene == scene_) {
            phase_ = Phase::settling;
            settle_us_ = 0;
            // Arrived (by the request, or on foot): the request is cleared, so
            // the next talk or sign here does not load the scene again.
            a.cancel_scene = true;
        } else if (now_us - since_us_ > kGiveUpUs && !local.loading_banc && local.scene) {
            // Never in the middle of a load: that load reads the request.
            phase_ = Phase::idle;
            a.cancel_scene = true;
            a.restore_start = !loaded_;
            a.lines.push_back("teleport: the game did not change scene in time; stopped");
            return a;
        } else {
            // Taken: the load started (or the scene is gone), or the game
            // emptied the request. "Emptied" counts only once the game has
            // shown our request: in the frame that asks, the flag still holds
            // what was there before (often "", left by the last load).
            if (local.reserved_banc && *local.reserved_banc == scene_) {
                seen_request_ = true;
            }
            const bool took = local.loading_banc || !local.scene ||
                              (seen_request_ && local.reserved_banc && local.reserved_banc->empty());
            if (!taken_ && took) {
                taken_ = true;
                loaded_ = true;
                taken_us_ = now_us;
                a.release = true;
                return a;
            }
            if (taken_) {
                if (local.loading_banc || !local.scene || now_us - taken_us_ < kRearmUs) {
                    return a;
                }
                taken_ = false;  // it ended somewhere else: ask again
                seen_request_ = false;
            }
            if (now_us - asked_us_ >= kAskAgainUs) {
                asked_us_ = now_us;
                a.scene = scene_;
                a.start = start_;
            }
            // The game takes the request when an event ends: start one if
            // none runs and the last one did not take it.
            if (kStartSceneEvent && !local.event && !local.loading_banc && now_us - event_us_ >= kEventAgainUs) {
                event_us_ = now_us;
                a.start_event = true;
            }
            return a;
        }
    }
    if (phase_ == Phase::settling) {
        if (!local.scene || !local.position || local.event || local.loading_banc) {
            settle_us_ = 0;  // still loading, or the arrival's own event
        } else if (*local.scene != scene_) {
            phase_ = Phase::idle;
            a.lines.push_back("teleport: stopped: you left " + scene_);
        } else if (settle_us_ == 0) {
            settle_us_ = now_us;
        } else if (now_us - settle_us_ >= kSettleUs) {
            phase_ = Phase::asking_again;
            a.ask = player_;
        }
        if (phase_ == Phase::settling && now_us - since_us_ > 2 * kGiveUpUs) {
            phase_ = Phase::idle;
            a.lines.push_back("teleport: stopped: the new scene did not settle");
        }
    }
    return a;
}

MoveCheck CheckMove(const CoopRooms::Table& rooms, std::string_view scene, const Vec& from, const Vec& to) {
    if (!CoopDungeon::IsDungeonScene(scene)) {
        return MoveCheck::ok;
    }
    const auto a = rooms.Nearest(scene, CoopRooms::Vec3{from.x, from.y, from.z});
    const auto b = rooms.Nearest(scene, CoopRooms::Vec3{to.x, to.y, to.z});
    if (a && b) {
        return a->room == b->room ? MoveCheck::ok : MoveCheck::other_room;
    }
    const float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz) <= kNearMove ? MoveCheck::ok : MoveCheck::too_far;
}

} // namespace CoopTeleport
