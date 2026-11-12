// SPDX-License-Identifier: GPL-3.0-or-later
//
// Player against player: the rules (coop_pvp.h).

#include "coop_pvp.h"

#include <algorithm>
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
    case proto::DamageCause::nut:  // a nut stuns, it does not hurt
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
    constexpr std::uint8_t kOut = proto::status::paused | proto::status::loading |
                                  proto::status::in_cutscene | proto::status::in_menu |
                                  proto::status::dead;
    return (status_flags & kOut) != 0;
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

} // namespace CoopPvp
