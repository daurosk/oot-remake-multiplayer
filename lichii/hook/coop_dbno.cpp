// SPDX-License-Identifier: GPL-3.0-or-later
//
// Down but not out: the rules (coop_dbno.h).

#include "coop_dbno.h"
#include "coop/protocol/scene_place.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <iterator>

#include "coop_pvp.h"  // Hearts

namespace CoopDbno {

namespace {

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

bool Finite(const Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

float Level(const Vec3& a, const Vec3& b) {
    const float dx = a[0] - b[0];
    const float dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

bool ValidPlayer(std::uint8_t n) {
    return n >= 1 && n <= proto::kMaxPlayers;
}

// "3", "2.5", "0.5" as a count of `per_one` (tenths: 10, quarter hearts: 4,
// seconds: 1), when it is a whole count of them; nothing for anything else.
std::optional<int> Count(std::string_view text, int per_one) {
    std::string_view whole = text;
    std::string_view part;
    if (const auto dot = text.find('.'); dot != std::string_view::npos) {
        whole = text.substr(0, dot);
        part = text.substr(dot + 1);
        if (part.empty() || part.size() > 2) {
            return std::nullopt;
        }
    }
    if (whole.empty() || whole.size() > 4) {
        return std::nullopt;
    }
    int w = 0;
    if (const auto r = std::from_chars(whole.data(), whole.data() + whole.size(), w);
        r.ec != std::errc{} || r.ptr != whole.data() + whole.size() || w < 0) {
        return std::nullopt;
    }
    int hundredths = 0;
    if (!part.empty()) {
        int p = 0;
        if (const auto r = std::from_chars(part.data(), part.data() + part.size(), p);
            r.ec != std::errc{} || r.ptr != part.data() + part.size() || p < 0) {
            return std::nullopt;
        }
        hundredths = part.size() == 1 ? p * 10 : p;
    }
    const int scaled = hundredths * per_one;
    if (scaled % 100 != 0) {
        return std::nullopt;  // not a whole count: "/dbno time 2.5"
    }
    return w * per_one + scaled / 100;
}

}  // namespace

// --- The rules ---

Rules RulesOf(const proto::DbnoMode& mode) {
    return Rules{mode.on, mode.set_by, mode.bleed_s, mode.revive_tenths, mode.revive_life};
}

proto::DbnoMode Ask(bool on, int bleed_s, int revive_tenths, int revive_life) {
    proto::DbnoMode m;
    m.set_by = 0;
    m.on = on;
    m.bleed_s = static_cast<std::uint8_t>(
        std::clamp<int>(bleed_s, proto::kDbnoMinBleedS, proto::kDbnoMaxBleedS));
    m.revive_tenths = static_cast<std::uint8_t>(
        std::clamp<int>(revive_tenths, proto::kDbnoMinReviveTenths, proto::kDbnoMaxReviveTenths));
    m.revive_life = static_cast<std::uint8_t>(
        std::clamp<int>(revive_life, proto::kDbnoMinReviveLife, proto::kDbnoMaxReviveLife));
    return m;
}

std::optional<Command> Parse(std::string_view text) {
    const std::string_view t = Trim(text);
    if (t.size() < 5 || Lower(t.substr(0, 5)) != "/dbno") {
        return std::nullopt;
    }
    if (t.size() > 5 && !std::isspace(static_cast<unsigned char>(t[5]))) {
        return std::nullopt;  // "/dbnox": not ours, sent as chat
    }
    const std::string arg = Lower(Trim(t.substr(5)));
    Command c;
    if (arg.empty()) {
        return c;  // show
    }
    c.kind = Command::Kind::change;
    if (arg == "on" || arg == "1" || arg == "yes") {
        c.on = true;
        return c;
    }
    if (arg == "off" || arg == "0" || arg == "no") {
        c.on = false;
        return c;
    }
    const auto space = arg.find_first_of(" \t");
    if (space != std::string::npos) {
        const std::string_view word = std::string_view(arg).substr(0, space);
        const std::string_view value = Trim(std::string_view(arg).substr(space));
        if (word == "time") {
            c.bleed_s = Count(value, 1);
        } else if (word == "hold") {
            c.revive_tenths = Count(value, 10);
        } else if (word == "hearts") {
            c.revive_life = Count(value, 4);
        }
    }
    if (!c.bleed_s && !c.revive_tenths && !c.revive_life) {
        c.kind = Command::Kind::bad;
    }
    return c;
}

proto::DbnoMode Applied(const Rules& now, const Command& change) {
    return Ask(change.on.value_or(now.on), change.bleed_s.value_or(now.bleed_s),
               change.revive_tenths.value_or(now.revive_tenths),
               change.revive_life.value_or(now.revive_life));
}

// --- The life guard ---

std::int32_t LifeGuard::Step(std::int32_t life, std::int32_t sum) noexcept {
    const Taken taken = Guarded(guard_.load(std::memory_order_acquire), life, sum);
    if (taken.lethal) {
        guard_.store(Guard::shielded, std::memory_order_release);
        lethal_.store(true, std::memory_order_release);
    }
    return taken.sum;
}

// --- The downed player's side ---

bool InReach(const Vec3& self, const Vec3& at, float slack) {
    if (!Finite(self) || !Finite(at)) {
        return false;
    }
    return std::fabs(self[1] - at[1]) <= kReviveHighM * slack &&
           Level(self, at) <= kReviveReachM * slack;
}

void Machine::Hold(std::uint8_t from, bool holding, std::uint64_t now_us) {
    if (ValidPlayer(from)) {
        hold_us_[from] = holding ? std::max<std::uint64_t>(now_us, 1) : 0;
    }
}

void Machine::Enter(State state, std::uint64_t now_us) {
    state_ = state;
    held_us_ = 0;
    reviver_ = 0;
    alone_us_ = 0;
    left_us_ = 0;
    out_seen_dead_ = false;
    out_written_us_ = now_us;
    hold_us_.fill(0);
    watch_ = 0;
    spectate_since_us_ = now_us;
    quiet_us_ = 0;
    move_ = Move::none;
    move_to_ = 0;
    move_since_us_ = 0;
    dying_since_us_ = 0;
}

std::uint8_t Machine::PickWatch(const Input& in) const {
    // Who may be watched: up and in the game, in the order of their numbers.
    std::vector<const Teammate*> up;
    for (const Teammate& t : in.team) {
        if (ValidPlayer(t.number) && t.state == State::up && t.in_game) {
            up.push_back(&t);
        }
    }
    if (up.empty()) {
        return 0;
    }
    std::sort(up.begin(), up.end(), [](const Teammate* a, const Teammate* b) { return a->number < b->number; });
    const auto at = std::find_if(up.begin(), up.end(), [&](const Teammate* t) { return t->number == watch_; });
    if (at != up.end()) {
        // The one watched stays, until the switch button: then the next.
        if (!in.next) {
            return watch_;
        }
        return std::next(at) == up.end() ? up.front()->number : (*std::next(at))->number;
    }
    // A first pick (or the one watched is no longer up): the nearest in our
    // scene, else the lowest number elsewhere.
    const Teammate* best = nullptr;
    float best_m = 0.0f;
    for (const Teammate* t : up) {
        if (!t->scene || !in.scene || !coop::protocol::same_place(*t->scene, *in.scene) || !t->position ||
            !Finite(*t->position)) {
            continue;
        }
        const float m = Level(in.position, *t->position);
        if (best == nullptr || m < best_m) {
            best = t;
            best_m = m;
        }
    }
    return (best != nullptr ? best : up.front())->number;
}

bool Machine::Quiet(const Input& in) const {
    // The one watched, and everyone up in its scene: no enemy near any of them.
    const Teammate* watched = nullptr;
    for (const Teammate& t : in.team) {
        if (t.number == watch_) {
            watched = &t;
        }
    }
    if (watched == nullptr || watched->state != State::up || !watched->in_game) {
        return false;
    }
    for (const Teammate& t : in.team) {
        if (t.state != State::up || !t.in_game) {
            continue;
        }
        const bool with_them =
            &t == watched || (t.scene && watched->scene && coop::protocol::same_place(*t.scene, *watched->scene));
        if (with_them && t.threat == Threat::enemy) {
            return false;
        }
    }
    return true;
}

std::uint8_t Machine::PickReviver(const Input& in) const {
    std::uint8_t best = 0;
    for (const Teammate& t : in.team) {
        if (!ValidPlayer(t.number) || t.state != State::up || !t.in_game || !t.scene || !in.scene ||
            !coop::protocol::same_place(*t.scene, *in.scene) || !t.position ||
            !InReach(in.position, *t.position, kReviveSlack)) {
            continue;
        }
        const std::uint64_t at = hold_us_[t.number];
        if (at == 0 || in.now_us > at + kHoldLiveUs) {
            continue;
        }
        if (t.number == reviver_) {
            return reviver_;  // whoever began goes on
        }
        if (best == 0 || t.number < best) {
            best = t.number;
        }
    }
    return best;
}

Output Machine::Frame(const Input& in) {
    Output out;
    // A frame with no Link (a load) takes no time off anything.
    const std::uint64_t step =
        last_us_ == 0 || in.now_us <= last_us_ || !in.life ? 0
                                                           : std::min(in.now_us - last_us_, kMaxStepUs);
    last_us_ = in.now_us;
    const bool active = in.rules.on && in.connected && in.guard_live;
    // Up counts while in the game or loading (a door, a loading zone, a
    // teleport, however long it takes): a teammate in `team` is connected; a
    // stuck or gone one leaves the session (player_left) and the team. The
    // title screen (no scene, not loading) is not up.
    const bool someone_up =
        in.connected && std::any_of(in.team.begin(), in.team.end(), [](const Teammate& t) {
            return ValidPlayer(t.number) && t.state == State::up && (t.in_game || t.loading);
        });
    const std::int32_t revive_life =
        std::max<std::int32_t>(kDownedLife, in.max_life && *in.max_life > 0
                                                ? std::min<std::int32_t>(in.rules.revive_life, *in.max_life)
                                                : in.rules.revive_life);

    if (in.life) {
        gone_since_us_ = 0;
    } else if (gone_since_us_ == 0) {
        gone_since_us_ = std::max<std::uint64_t>(in.now_us, 1);
    }

    switch (state_) {
    case State::up:
        if (active && in.lethal) {
            Enter(State::downed, in.now_us);
            left_us_ = in.rules.BleedUs();
            out.events.push_back(Event::went_down);
        } else if (active && dying_since_us_ != 0 &&
                   (!in.life || (*in.life <= 0 && in.now_us >= dying_since_us_ + kDyingUs))) {
            // The last one standing (the guard was off) or a pit: the game's
            // own death ran and no fairy saved him (the Game Over took the
            // Link, or the life stayed 0). Out, so the others know nobody is up.
            Enter(State::out, in.now_us);
            out_seen_dead_ = true;
            out.events.push_back(Event::died);
        } else if (active && in.life && *in.life <= 0) {
            if (dying_since_us_ == 0) {
                dying_since_us_ = std::max<std::uint64_t>(in.now_us, 1);  // still up: a fairy may save him
            }
        } else if (!active || (in.life && *in.life > 0)) {
            dying_since_us_ = 0;
        }
        break;
    case State::downed:
        if (in.life && *in.life <= 0) {
            // The game took the life anyway: its death runs.
            Enter(State::out, in.now_us);
            out_seen_dead_ = true;
            out.events.push_back(Event::died);
            break;
        }
        if (in.connected && !in.rules.on) {
            Enter(State::up, in.now_us);
            out.write_life = revive_life;
            out.events.push_back(Event::got_up);
            break;
        }
        if (gone_since_us_ != 0 && in.now_us >= gone_since_us_ + kGoneUs) {
            Enter(State::up, in.now_us);  // left the game: the next save's life stands
            break;
        }
        // Offline (a blip, or the rules not yet heard) nobody is counted:
        // the wipe waits, the bleed-out runs on.
        alone_us_ = someone_up || !in.connected ? 0 : alone_us_ + step;
        reviver_ = PickReviver(in);
        if (reviver_ != 0) {
            held_us_ += step;  // the bleed-out stands still meanwhile
        } else {
            held_us_ = 0;
            left_us_ -= std::min(left_us_, step);
        }
        if (reviver_ != 0 && held_us_ >= in.rules.ReviveUs()) {
            out.by = reviver_;
            Enter(State::up, in.now_us);
            mercy_until_us_ = in.now_us + kMercyUs;
            out.write_life = revive_life;
            out.events.push_back(Event::revived);
        } else if (alone_us_ >= kWipeGraceUs) {
            out.events.push_back(Event::wiped);
            Enter(State::out, in.now_us);
            out.write_life = 0;
        } else if (left_us_ == 0) {
            // Not revived in time: watch the others until the fight is over.
            Enter(State::spectating, in.now_us);
            watch_ = PickWatch(in);
            out.by = watch_;
            out.events.push_back(Event::bled_out);
            if (in.life && *in.life != kDownedLife) {
                out.write_life = kDownedLife;
            }
        } else if (in.life && *in.life != kDownedLife) {
            out.write_life = kDownedLife;
        }
        break;
    case State::spectating:
        if (in.life && *in.life <= 0) {
            Enter(State::out, in.now_us);
            out_seen_dead_ = true;
            out.events.push_back(Event::died);
            break;
        }
        if (in.connected && !in.rules.on) {
            Enter(State::up, in.now_us);
            out.write_life = revive_life;
            out.events.push_back(Event::got_up);
            break;
        }
        if (gone_since_us_ != 0 && in.now_us >= gone_since_us_ + kGoneUs) {
            Enter(State::up, in.now_us);
            break;
        }
        alone_us_ = someone_up || !in.connected ? 0 : alone_us_ + step;
        if (alone_us_ >= kWipeGraceUs) {
            out.events.push_back(Event::wiped);
            Enter(State::out, in.now_us);
            out.write_life = 0;
            break;
        }
        watch_ = PickWatch(in);
        quiet_us_ = in.connected && Quiet(in) ? quiet_us_ + step : 0;
        {
            // Coming back: first our Link is moved (beside the one watched,
            // else to the entrance), then up. Never up where we lay with an
            // enemy near us.
            bool respawn = false;
            if (move_ != Move::none) {
                const bool late = in.now_us >= move_since_us_ + kRespawnWaitUs;
                if (in.moved == Moved::done) {
                    respawn = true;
                } else if (in.moved == Moved::failed || late) {
                    if (move_ == Move::beside) {
                        move_ = Move::entrance;
                        move_since_us_ = in.now_us;
                        out.move_entrance = true;
                    } else if (!in.enemy_near_us) {
                        respawn = true;  // up where we lay: nothing near
                    } else {
                        move_ = Move::none;  // try again once it is quiet again
                        quiet_us_ = 0;
                    }
                }
            } else if (quiet_us_ >= kQuietUs && in.now_us >= spectate_since_us_ + kSpectateMinUs) {
                move_ = Move::beside;
                move_to_ = watch_;
                move_since_us_ = in.now_us;
                out.move_beside = watch_;
            }
            if (respawn) {
                const std::uint8_t beside = move_to_;
                Enter(State::up, in.now_us);
                mercy_until_us_ = in.now_us + kMercyUs;
                out.write_life = std::max<std::int32_t>(
                    kDownedLife,
                    in.max_life && *in.max_life > 0 ? std::min(kRespawnLife, *in.max_life) : kRespawnLife);
                out.by = beside;
                out.events.push_back(Event::respawned);
                break;
            }
        }
        if (in.life && *in.life != kDownedLife) {
            out.write_life = kDownedLife;
        }
        break;
    case State::out:
        if (!in.life || *in.life <= 0) {
            out_seen_dead_ = true;  // dead, or no Link (the game over screen, a reload)
        } else if (out_seen_dead_) {
            Enter(State::up, in.now_us);
            out.events.push_back(Event::back);
        } else if (in.now_us >= out_written_us_ + kOutAgainUs) {
            out.write_life = 0;  // the first write did not hold
            out_written_us_ = in.now_us;
        }
        break;
    }

    out.state = state_;
    switch (state_) {
    case State::up:
        // A bottled fairy: the guard stays off, so the game's own death runs and
        // its fairy revives us (row 162); the dying watch above sees him back up.
        out.guard = in.now_us < mercy_until_us_                   ? Guard::shielded
                    : active && someone_up && !in.bottled_fairy ? Guard::last_stand
                                                                : Guard::off;
        break;
    case State::downed:
        out.guard = Guard::shielded;
        out.left_us = left_us_;
        out.reviver = reviver_;
        out.progress = in.rules.ReviveUs() == 0
                           ? 0.0f
                           : std::min(1.0f, static_cast<float>(held_us_) /
                                                static_cast<float>(in.rules.ReviveUs()));
        break;
    case State::spectating:
        out.guard = Guard::shielded;
        out.watching = watch_;
        break;
    case State::out:
        out.guard = Guard::off;
        break;
    }

    // What the others are told.
    if (!in.connected) {
        sent_.reset();  // a new connection knows nothing of us
        tell_again_ = false;
        return out;
    }
    proto::DbnoStatus status;
    status.state = state_;
    if (state_ == State::downed) {
        status.left_tenths = static_cast<std::uint16_t>(
            std::min<std::uint64_t>((left_us_ + 99'999) / 100'000, proto::kDbnoMaxLeftTenths));
        status.reviver = reviver_;
        status.progress = reviver_ == 0 ? 0 : static_cast<std::uint8_t>(out.progress * 255.0f);
    }
    bool due = false;
    if (!sent_) {
        due = state_ != State::up;
    } else if (sent_->state != status.state || sent_->reviver != status.reviver) {
        due = true;
    } else if (*sent_ != status) {
        due = in.now_us >= sent_us_ + (reviver_ != 0 ? kStateSendRevivingUs : kStateSendUs);
    }
    if (tell_again_ && state_ != State::up) {
        due = true;
    }
    tell_again_ = false;
    if (due) {
        sent_ = status;
        sent_us_ = in.now_us;
        out.send = status;
    }
    return out;
}

// --- The other players' states ---

void Roster::Set(const proto::DbnoStatus& status, std::uint64_t now_us) {
    if (ValidPlayer(status.player_number)) {
        entries_[status.player_number] = Entry{status, now_us};
    }
}

void Roster::Left(std::uint8_t number) {
    if (ValidPlayer(number)) {
        entries_[number].reset();
    }
}

void Roster::Clear() {
    entries_.fill(std::nullopt);
}

State Roster::StateOf(std::uint8_t number) const {
    if (!ValidPlayer(number) || !entries_[number]) {
        return State::up;
    }
    return entries_[number]->status.state;
}

std::optional<Roster::Entry> Roster::Downed(std::uint8_t number) const {
    if (StateOf(number) != State::downed) {
        return std::nullopt;
    }
    return entries_[number];
}

std::uint64_t Roster::LeftUs(const Entry& entry, std::uint64_t now_us) {
    const std::uint64_t said = std::uint64_t{entry.status.left_tenths} * 100'000;
    if (entry.status.reviver != 0 || now_us <= entry.at_us) {
        return said;
    }
    return said - std::min(said, now_us - entry.at_us);
}

// --- Enemies near our Link ---

bool AnyNear(const Vec3& self, std::span<const Vec3> foes) {
    if (!Finite(self)) {
        return false;
    }
    return std::any_of(foes.begin(), foes.end(), [&](const Vec3& f) {
        return Finite(f) && std::fabs(f[1] - self[1]) <= kReviveHighM * 4.0f && Level(self, f) <= kThreatReachM;
    });
}

std::optional<proto::DbnoThreat> ThreatTeller::Frame(std::uint64_t now_us, bool able, bool enemy_near) {
    if (!able) {
        sent_.reset();
        return std::nullopt;
    }
    if (sent_ && *sent_ == enemy_near && now_us < sent_us_ + kThreatSendUs) {
        return std::nullopt;
    }
    sent_ = enemy_near;
    sent_us_ = now_us;
    return proto::DbnoThreat{0, enemy_near};
}

// --- The reviver's side ---

void Reviver::Reset() {
    target_ = 0;
    holding_ = false;
    sent_us_ = 0;
}

Reviver::Out Reviver::Frame(std::uint64_t now_us, bool able, bool button, const Vec3& self,
                            const std::optional<std::string>& scene, std::span<const Fallen> fallen) {
    // The one we are at stays ours a little past the reach, so standing on
    // its edge does not flicker; else the nearest (the lower number on a tie).
    std::uint8_t pick = 0;
    float pick_m = 0.0f;
    if (able && scene) {
        for (const Fallen& f : fallen) {
            if (!ValidPlayer(f.number) || !f.scene || !coop::protocol::same_place(*f.scene, *scene)) {
                continue;
            }
            if (f.number == target_ && InReach(self, f.position, 1.2f)) {
                pick = f.number;
                break;
            }
            if (!InReach(self, f.position)) {
                continue;
            }
            const float m = Level(self, f.position);
            if (pick == 0 || m < pick_m || (m == pick_m && f.number < pick)) {
                pick = f.number;
                pick_m = m;
            }
        }
    }

    Out out;
    if (holding_ && (pick != target_ || !button)) {
        // Let go of the one we held, before anything else.
        out.send = proto::DbnoRevive{0, target_, false};
        holding_ = false;
        target_ = pick;
        out.target = target_;
        return out;
    }
    target_ = pick;
    out.target = target_;
    if (target_ != 0 && button) {
        if (!holding_ || now_us >= sent_us_ + kHoldSendUs) {
            out.send = proto::DbnoRevive{0, target_, true};
            sent_us_ = now_us;
        }
        holding_ = true;
    }
    out.holding = holding_;
    return out;
}

// --- The pad ---

Pad DownedPad(const Pad& in) {
    Pad out = in;
    out.buttons &= pad::plus | pad::minus | pad::stick_l_dirs | pad::stick_r_dirs;
    out.stick[0] = in.stick[0] * kCrawlStickPercent / 100;
    out.stick[1] = in.stick[1] * kCrawlStickPercent / 100;
    return out;
}

Pad SpectatorPad(const Pad& in) {
    Pad out;
    out.buttons = in.buttons & (pad::plus | pad::minus | pad::stick_r_dirs);
    out.stick[2] = in.stick[2];
    out.stick[3] = in.stick[3];
    return out;
}

Pad ReviverPad(const Pad& in) {
    Pad out = in;
    out.buttons &= ~pad::a;
    return out;
}

// --- What the screen shows ---

std::string Clock(std::uint64_t left_us) {
    const std::uint64_t s = (left_us + 999'999) / 1'000'000;
    const std::uint64_t rest = s % 60;
    return std::to_string(s / 60) + (rest < 10 ? ":0" : ":") + std::to_string(rest);
}

std::string RulesLine(const Rules& rules, std::uint8_t self, std::string_view by_name) {
    std::string line;
    if (!rules.on) {
        line = "Down but not out is off: at no hearts you die";
    } else {
        const int tenths = rules.revive_tenths;
        line = "Down but not out is on: " + Clock(rules.BleedUs()) + " to be revived, hold A " +
               std::to_string(tenths / 10) + (tenths % 10 != 0 ? "." + std::to_string(tenths % 10) : "") +
               " s, back with " + CoopPvp::Hearts(rules.revive_life);
    }
    if (rules.set_by != 0) {
        line += rules.set_by == self ? " (you changed it)"
                                     : " (" + std::string(by_name.empty() ? "the host" : by_name) + " changed it)";
    }
    return line;
}

std::optional<std::string> OwnLine(Event event, std::string_view by_name) {
    switch (event) {
    case Event::went_down:
        return "You are down. A teammate can revive you";
    case Event::revived:
        return std::string(by_name.empty() ? "A teammate" : by_name) + " revived you";
    case Event::bled_out:
        return "Nobody reached you in time: you watch " + std::string(by_name.empty() ? "a teammate" : by_name) +
               " and come back once no enemy is near (A: watch the next player)";
    case Event::respawned:
        return "No enemy is near: you are back beside " + std::string(by_name.empty() ? "a teammate" : by_name);
    case Event::wiped:
        return "Nobody is left standing";
    case Event::got_up:
        return "Down but not out was turned off: you are up again";
    case Event::died:
    case Event::back:
        break;
    }
    return std::nullopt;
}

std::optional<std::string> TheirLine(const std::optional<proto::DbnoStatus>& before,
                                     const proto::DbnoStatus& now, std::string_view name,
                                     std::string_view reviver_name) {
    const State was = before ? before->state : State::up;
    if (was == now.state) {
        return std::nullopt;
    }
    const std::string who(name.empty() ? "P" + std::to_string(now.player_number) : std::string(name));
    switch (now.state) {
    case State::downed:
        return who + " is down: hold A beside them to revive";
    case State::out:
        return who + " is out";
    case State::spectating:
        return who + " is watching: back once no enemy is near";
    case State::up:
        if (was == State::downed && before->reviver != 0) {
            return std::string(reviver_name.empty() ? "P" + std::to_string(before->reviver)
                                                    : std::string(reviver_name)) +
                   " revived " + who;
        }
        if (was == State::downed || was == State::spectating) {
            return who + " is up again";
        }
        break;  // back after out: the game's own business
    }
    return std::nullopt;
}

}  // namespace CoopDbno
