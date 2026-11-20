// SPDX-License-Identifier: GPL-3.0-or-later
//
// Player against player: the rules (coop_pvp.h).

#include "coop_pvp.h"

#include <algorithm>
#include <utility>
#include <cctype>
#include <cmath>

namespace CoopPvp {

namespace proto = coop::protocol;

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

// Whether `p` is within `reach` (level) of the body standing at `feet`.
bool Touches(const Vec3& p, const Vec3& feet, float reach) {
    const float up = p[1] - feet[1];
    return up >= kBodyLowM && up <= kBodyHighM && Level(p, feet) <= reach;
}

bool CanBeHit(const Target& t, std::string_view scene, std::uint8_t self_number) {
    return t.number >= 1 && t.number <= proto::kMaxPlayers && t.number != self_number &&
           t.scene && *t.scene == scene && !Guarded(t.status) && Finite(t.position);
}

} // namespace

std::optional<Command> Parse(std::string_view text) {
    const std::string_view t = Trim(text);
    if (t.size() < 4 || Lower(t.substr(0, 4)) != "/pvp") {
        return std::nullopt;
    }
    if (t.size() > 4 && !std::isspace(static_cast<unsigned char>(t[4]))) {
        return std::nullopt;  // "/pvpx": not ours, sent as chat
    }
    const std::string arg = Lower(Trim(t.substr(4)));
    Command c;
    if (arg.empty()) {
        c.kind = Command::Kind::show;
    } else if (arg == "on" || arg == "1" || arg == "yes") {
        c.kind = Command::Kind::on;
    } else if (arg == "off" || arg == "0" || arg == "no") {
        c.kind = Command::Kind::off;
    } else {
        c.kind = Command::Kind::bad;
    }
    return c;
}

std::int32_t AmountOf(proto::DamageCause cause) {
    switch (cause) {
    case proto::DamageCause::sword: return 2;
    case proto::DamageCause::arrow: return 4;
    case proto::DamageCause::fire_arrow: return 4;
    case proto::DamageCause::ice_arrow: return 4;
    case proto::DamageCause::light_arrow: return 8;
    case proto::DamageCause::seed: return 1;
    case proto::DamageCause::bomb: return 8;
    case proto::DamageCause::boomerang: return 1;
    case proto::DamageCause::hookshot: return 2;
    case proto::DamageCause::hammer: return 4;
    case proto::DamageCause::magic: return 4;
    case proto::DamageCause::nut:     // a nut stuns, it does not hurt
    case proto::DamageCause::killed:  // a lifeless copy's kill report (issue 133), never a PvP hit
    case proto::DamageCause::reflected:  // row 177: a notice to a shot's owner (amount 0), never a PvP hit
    case proto::DamageCause::unknown: return 0;
    }
    return 0;
}

float ReachOf(proto::DamageCause cause) {
    switch (cause) {
    case proto::DamageCause::arrow:
    case proto::DamageCause::fire_arrow:
    case proto::DamageCause::ice_arrow:
    case proto::DamageCause::light_arrow:
    case proto::DamageCause::boomerang:
    case proto::DamageCause::hookshot: return 1.0f;
    case proto::DamageCause::seed: return 0.8f;
    case proto::DamageCause::bomb: return 3.0f;
    default: return 0.0f;
    }
}

bool Guarded(std::uint8_t status_flags) {
    return (status_flags & proto::status::away) != 0;
}

bool Attacker::Ready(std::uint8_t number, std::uint64_t now_us) {
    std::uint64_t& last = last_hit_us_[number];
    if (last != 0 && now_us < last + kSameVictimUs) {
        return false;
    }
    last = now_us == 0 ? 1 : now_us;
    return true;
}

std::vector<proto::PvpHit> Attacker::Swing(const proto::GrassCut& cut, const Vec3& self,
                                           std::string_view scene, std::uint8_t self_number,
                                           std::span<const Target> targets, std::uint64_t now_us) {
    std::vector<proto::PvpHit> out;
    std::vector<Vec3> blade;
    for (const Vec3& p : cut.sweep) {
        if (Finite(p) && Level(p, self) <= kMaxSwingFromSelfM) {
            blade.push_back(p);
        }
    }
    if (blade.empty() || scene.empty() || cut.scene != scene) {
        return out;  // not a sword (a roll, a bomb), or not ours
    }
    for (const Target& t : targets) {
        if (!CanBeHit(t, scene, self_number)) {
            continue;
        }
        const auto at = std::find_if(blade.begin(), blade.end(),
                                     [&](const Vec3& p) { return Touches(p, t.position, kSwordReachM); });
        if (at == blade.end() || !Ready(t.number, now_us)) {
            continue;
        }
        proto::PvpHit h;
        h.victim = t.number;
        h.cause = static_cast<std::uint8_t>(proto::DamageCause::sword);
        h.amount = AmountOf(proto::DamageCause::sword);
        h.at = *at;
        h.from = self;
        out.push_back(h);
    }
    return out;
}

std::vector<proto::PvpHit> Attacker::Shot(proto::DamageCause cause, const Vec3& at, const Vec3& self,
                                          std::string_view scene, std::uint8_t self_number,
                                          std::span<const Target> targets, std::uint64_t now_us) {
    std::vector<proto::PvpHit> out;
    const float reach = ReachOf(cause);
    const std::int32_t amount = AmountOf(cause);
    if (reach <= 0.0f || amount <= 0 || scene.empty() || !Finite(at) || !Finite(self)) {
        return out;
    }
    for (const Target& t : targets) {
        if (!CanBeHit(t, scene, self_number) || !Touches(at, t.position, reach) ||
            !Ready(t.number, now_us)) {
            continue;
        }
        proto::PvpHit h;
        h.victim = t.number;
        h.cause = static_cast<std::uint8_t>(cause);
        h.amount = amount;
        h.at = at;
        h.from = self;
        out.push_back(h);
    }
    return out;
}

std::optional<Decision> Victim::Hit(const proto::PvpHit& hit, bool pvp_on, const Local& local,
                                    const Hitter& attacker, std::uint64_t now_us) {
    if (!pvp_on || local.self == 0 || hit.victim != local.self || hit.player_number == local.self ||
        hit.player_number < 1 || hit.player_number > proto::kMaxPlayers) {
        return std::nullopt;
    }
    Decision d;
    d.hurt.attacker = hit.player_number;
    const std::int32_t life = std::clamp<std::int32_t>(local.life.value_or(0), 0, proto::kMaxPvpLife);
    d.hurt.life = life;
    const bool mercy = last_hurt_us_ && now_us < *last_hurt_us_ + kMercyUs;
    const bool guarded = !local.life || life <= 0 || local.event || Guarded(local.status) ||
                         !local.scene || !attacker.known || Guarded(attacker.status) ||
                         !attacker.scene || *attacker.scene != *local.scene || mercy;
    if (guarded) {
        d.hurt.flags = proto::pvp_hurt::guarded;
        return d;
    }
    const std::int32_t take = std::clamp<std::int32_t>(hit.amount, 1, proto::kMaxPvpLife);
    d.take = std::min(take, life);
    d.hurt.amount = d.take;
    d.hurt.life = life - d.take;
    if (d.hurt.life == 0) {
        d.hurt.flags = proto::pvp_hurt::died;
    }
    last_hurt_us_ = now_us;
    return d;
}

std::string Hearts(std::int32_t quarters) {
    if (quarters < 0) {
        quarters = 0;
    }
    const std::int32_t whole = quarters / 4;
    const std::int32_t rest = quarters % 4;
    static constexpr const char* kParts[] = {"", "1/4", "1/2", "3/4"};
    std::string out;
    if (whole > 0) {
        out = std::to_string(whole);
    }
    if (rest > 0) {
        if (!out.empty()) {
            out += ' ';
        }
        out += kParts[rest];
    }
    if (out.empty()) {
        out = "0";
    }
    return out + (quarters > 4 ? " hearts" : " heart");
}

std::string ModeLine(const Mode& mode, std::uint8_t self, std::string_view by_name) {
    std::string line = mode.on ? "PvP is on: players can hurt each other" : "PvP is off";
    if (mode.set_by != 0) {
        line += mode.set_by == self ? " (you changed it)"
                                    : " (" + std::string(by_name.empty() ? "the host" : by_name) + " changed it)";
    }
    return line;
}

std::optional<std::string> HurtLine(const proto::PvpHurt& hurt, std::uint8_t self,
                                    std::string_view victim_name, std::string_view attacker_name) {
    if ((hurt.flags & proto::pvp_hurt::guarded) != 0 || hurt.amount <= 0) {
        return std::nullopt;
    }
    const std::uint8_t victim = hurt.player_number;
    const std::string who = hurt.attacker == self ? "you"
                            : attacker_name.empty() ? "P" + std::to_string(hurt.attacker)
                                                    : std::string(attacker_name);
    const std::string whom = victim == self ? "you"
                             : victim_name.empty() ? "P" + std::to_string(victim)
                                                   : std::string(victim_name);
    if ((hurt.flags & proto::pvp_hurt::died) != 0) {
        return who + " defeated " + whom;
    }
    return who + " hit " + whom + " (-" + Hearts(hurt.amount) + ")";
}

namespace {
void Lower(std::vector<PendingGive>& gives, std::int32_t by) {
    if (by <= 0) {
        return;
    }
    for (PendingGive& g : gives) {
        g.expect = std::max<std::int32_t>(0, g.expect - by);
    }
}
} // namespace

bool LinkLifeWatch::Follow(std::vector<PendingGive>& gives, std::int32_t before, std::int32_t now, bool own_hit,
                           bool replay_hit, bool id_read) {
    if (before < 0 || now < 0 || gives.empty()) {
        Reset();
        return false;
    }
    std::int32_t takes = 0;
    for (const PendingGive& g : gives) {
        takes += g.replayed ? g.take : 0;
    }
    const std::int32_t held = std::exchange(held_, 0);
    const std::int32_t carried = std::exchange(reached_, 0);
    if (now > before) {
        // Last read's held fall first (the landing if our replayed id came now, else harm), then the rise.
        Lower(gives, held - (replay_hit ? std::min(held, takes) : 0));
        for (PendingGive& g : gives) {
            g.expect += now - before;
        }
        return false;
    }
    const std::int32_t fall = before - now;
    if (replay_hit) {
        // Our replayed id came now: last read's held fall and this read's fall are one change, our replay's part
        // first and the rest harm, lowered once. (Was: the held fall took the landing, and this read's fall, with
        // the replay already used, was held again and lowered next read: the replay's own fall counted as harm.)
        const std::int32_t moved = held + fall;
        if (moved == 0) {
            reached_ = takes;  // its fall may be read next
            return false;
        }
        Lower(gives, moved - std::min(moved, takes));
        return false;
    }
    Lower(gives, held);  // no id came for last read's fall: harm with no hit record
    if (fall == 0 || !id_read) {
        return false;  // (id not read: a fall is the landing, life <= expect books it, as before)
    }
    // Our replayed id reached last read with no fall: this fall is its landing first.
    const std::int32_t rest = fall - std::min(fall, carried);
    if (own_hit) {
        Lower(gives, rest);  // a monster's hit: its id came with it
        return false;
    }
    if (rest > 0) {
        held_ = rest;  // no id moved: held one read for it
        return true;
    }
    return false;
}

std::int32_t NextExpect(const std::vector<PendingGive>& gives, std::int32_t life, std::int32_t take) {
    std::int32_t after = life;
    for (const PendingGive& g : gives) {
        after = std::min(after, g.expect);
    }
    return std::max<std::int32_t>(0, after - take);
}

} // namespace CoopPvp
