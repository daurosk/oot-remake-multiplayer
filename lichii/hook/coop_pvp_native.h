// SPDX-License-Identifier: GPL-3.0-or-later
//
// PvP on real bodies (native puppets phase 4, docs/native-puppets.md section
// 5): the rules, with no game code in them. The attacker's game finds a hit on
// another player's copy with the game's own collision and sends PVP_STRIKE;
// the victim's game decides here whether its real Link takes it (5.6), then
// hands it to the game's own hit path (A, the sensor path, or B, the life
// replay behind a guard check) and answers PVP_RESULT.
//
// - Which copy actor: the plain CoopPuppet_* while PvP is off, the _Pvp one
//   only while PvP is on and both games are PVP_NATIVE with a pvp pack (5.0).
//   A copy that is lifted or holding a bomb is not re-made until that ends.
// - Which hit records are ours to send: our Link's, or a shot or blast our
//   game made (5.4). An enemy's hit on a copy never is.
// - The victim's checks: seq (newer per attacker, forgotten when the attacker
//   leaves, so one who comes back starts again), kept off, time, the victim's
//   own place ring, the attacker's place ring and reach by source.
// - Path B only: the amount from the victim's own tables and the mercy time.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>
#include <string>
#include <string_view>

#include "coop/protocol/messages.hpp"
#include "coop/protocol/scene_place.hpp"
#include "coop_pvp.h"

namespace CoopPvpNative {

namespace proto = coop::protocol;
using Vec3 = proto::Vec3;

// --- Which copy actor the mode asks for (5.0) ---

enum class CopyActor : std::uint8_t { plain, pvp };

struct CopyMode {
    bool pvp_on = false;       // PVP_MODE, the session's value
    bool glue_on = false;      // LICHII_COOP_PVP_NATIVE=1 in our game
    bool pack_pvp = false;     // the pack's manifest says "pvp": true
    bool peer_native = false;  // the copy's player said PVP_NATIVE 1
};

// The _Pvp copy only when everything says so; else the plain one (no sensor,
// no Life, no attention sender: nothing to lock on to or hit).
inline CopyActor WantedCopy(const CopyMode& m) {
    return m.pvp_on && m.glue_on && m.pack_pvp && m.peer_native ? CopyActor::pvp : CopyActor::plain;
}

// Whether a copy of `have` is re-made now. A copy that is lifted (phase 3) or
// holding a bomb (phase 2) keeps its actor until the lift or hold ends: the
// re-make would drop the carried player or bomb (review LOW note 2). The
// director asks again every frame, so the change lands the frame it ends.
inline bool RemakeNow(CopyActor have, CopyActor want, bool lifted_or_holding) {
    return have != want && !lifted_or_holding;
}

// The _Pvp copy's actor beside a puppet's (tools/puppet_pack/build.py pvp_name). Only the pack's own
// puppets have one: any other actor (a test actor, a real Player) is left as it is.
inline constexpr std::string_view kPvpSuffix = "_Pvp";

inline bool IsPvpActor(std::string_view actor) {
    return actor.size() > kPvpSuffix.size() && actor.substr(actor.size() - kPvpSuffix.size()) == kPvpSuffix;
}

inline std::string PvpActor(std::string_view actor) {
    if (actor.substr(0, 11) != "CoopPuppet_" || IsPvpActor(actor)) return std::string(actor);
    return std::string(actor) + std::string(kPvpSuffix);
}

// Whether `a` and `b` are one puppet, one of them its _Pvp copy (a mode change, not a new look).
inline bool OnlyPvpDiffers(std::string_view a, std::string_view b) {
    return a != b && (PvpActor(a) == b || PvpActor(b) == a);
}

// The pack's manifest says "pvp": true (build.py --puppet-pvp). Each actor's own "pvp" entry is an object,
// never true, so every "pvp" key is looked at.
inline bool PackHasPvp(std::string_view manifest) {
    constexpr std::string_view key = "\"pvp\"";
    for (auto at = manifest.find(key); at != std::string_view::npos; at = manifest.find(key, at + 1)) {
        const auto v = manifest.find_first_not_of(" \t\r\n:", at + key.size());
        if (v != std::string_view::npos && manifest.substr(v, 4) == "true") return true;
    }
    return false;
}

// Our game makes PVP_NATIVE 1 only with the glue on and a pvp pack.
inline bool SayNative(bool glue_on, bool pack_pvp) { return glue_on && pack_pvp; }

// --- Which hit records are ours to send (5.4) ---

struct RecordFrom {
    bool our_link = false;                 // the record's attacker is our real Link
    std::optional<proto::ObjectId> made;   // else: the shot's or blast's object, if our spawn sync has it
    bool blast = false;                    // that object is a bomb's, a bombchu's or Din's Fire's blast
};

// The strike's source, or nothing when the record is not ours (an enemy's,
// another player's shot, an object of no one).
inline std::optional<proto::PvpSource> StrikeSource(const RecordFrom& r, std::uint8_t self) {
    if (r.our_link) return proto::PvpSource::sword;
    if (r.made && r.made->kind == proto::ObjectKind::made && self != 0 && r.made->maker == self) {
        return r.blast ? proto::PvpSource::blast : proto::PvpSource::shot;
    }
    return std::nullopt;
}

// --- Rings of places (5.6) ---

inline constexpr std::uint64_t kRingUs = 1'000'000;  // a ring keeps this long

// Places over the last second, oldest first: the victim's own Link (each
// frame) and its copy of each player (each sample it received).
class PlaceRing {
public:
    void Push(std::uint64_t t_us, const Vec3& at) {
        if (!places_.empty() && t_us < places_.back().t_us) places_.clear();  // a clock step: start again
        places_.push_back({t_us, at});
        while (!places_.empty() && t_us - places_.front().t_us > kRingUs) places_.pop_front();
    }
    void Clear() { places_.clear(); }
    // The place nearest `t_us`, within `slack_us`; nothing when the ring has
    // none that near.
    std::optional<Vec3> At(std::uint64_t t_us, std::uint64_t slack_us) const {
        std::optional<Vec3> best;
        std::uint64_t best_gap = 0;
        for (const auto& p : places_) {
            const std::uint64_t gap = p.t_us > t_us ? p.t_us - t_us : t_us - p.t_us;
            if (gap <= slack_us && (!best || gap < best_gap)) {
                best = p.at;
                best_gap = gap;
            }
        }
        return best;
    }
    bool Empty() const { return places_.empty(); }
    // The point of the ring's path nearest `point`, among its places from `from_us` on and the straight
    // lines between two neighbours no more than kJoinUs apart (a warp or a gap is not a path). Nothing when
    // the ring has no place from then on. The attacker's place is checked with it: the strike's `from` is
    // where the attacker stood when it HIT, which is later than `seen_us` by the copy's display delay and one
    // trip, so a running attacker is not where it was at `seen_us` (at 7 m/s and 200 ms: 1.4 m).
    std::optional<Vec3> NearestSince(std::uint64_t from_us, const Vec3& point) const {
        std::optional<Vec3> best;
        float best_d2 = 0.0f;
        const auto take = [&](const Vec3& c) {
            const float dx = c[0] - point[0], dy = c[1] - point[1], dz = c[2] - point[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (!best || d2 < best_d2) {
                best = c;
                best_d2 = d2;
            }
        };
        for (std::size_t i = 0; i < places_.size(); ++i) {
            const Place& p = places_[i];
            if (p.t_us < from_us) continue;
            take(p.at);
            if (i + 1 >= places_.size() || places_[i + 1].t_us - p.t_us > kJoinUs) continue;
            const Vec3& q = places_[i + 1].at;
            const float ex = q[0] - p.at[0], ey = q[1] - p.at[1], ez = q[2] - p.at[2];
            const float len2 = ex * ex + ey * ey + ez * ez;
            if (!(len2 > 1e-8f)) continue;
            const float u = std::clamp(
                ((point[0] - p.at[0]) * ex + (point[1] - p.at[1]) * ey + (point[2] - p.at[2]) * ez) / len2, 0.0f,
                1.0f);
            take(Vec3{p.at[0] + u * ex, p.at[1] + u * ey, p.at[2] + u * ez});
        }
        return best;
    }
    static constexpr std::uint64_t kJoinUs = 250'000;

private:
    struct Place {
        std::uint64_t t_us;
        Vec3 at;
    };
    std::deque<Place> places_;
};

// --- Replays (5.6): a strike's seq must be newer than the last taken ---

class SeqGate {
public:
    // True when `seq` is newer than the last one seen from `attacker`; then it
    // is the last one. A repeated or older seq is false (dropped, no result).
    bool Take(std::uint8_t attacker, std::uint32_t seq) {
        auto it = last_.find(attacker);
        if (it != last_.end() && seq <= it->second) return false;
        last_[attacker] = seq;
        return true;
    }
    // The attacker left (PLAYER_LEFT) or joined again (PLAYER_JOINED): its
    // game counts from 1 again (review LOW note 1).
    void Forget(std::uint8_t attacker) { last_.erase(attacker); }
    void Reset() { last_.clear(); }

private:
    std::map<std::uint8_t, std::uint32_t> last_;
};

// --- The victim's checks (5.6) ---

// How old a strike's `seen_us` may be when it reaches the victim. What passes between the two is: the victim's
// sample going to the attacker (one trip: both games' way to the server), the attacker's display delay for that
// copy (presence's show delay, 100 ms or more), and the strike coming back (one trip). With both games at a
// 200 ms ping (100 ms each way to the server) a trip is 200 ms: 200 + 100 + 200 = 500 ms, plus 50 ms for jitter
// and a frame on each side. (350 ms, the first value, answered `late` to every strike from two games at 150 ms
// pings or more: not fair at 200 ms.) Past it the strike is `late`: nobody is hit from more than 0.55 s ago.
inline constexpr std::uint64_t kStrikeLateUs = 550'000;
inline constexpr std::uint64_t kOwnRingSlackUs = 50'000;  // our own ring has a place each frame
inline constexpr std::uint64_t kCopyRingSlackUs = 100'000;
inline constexpr float kJitterM = 0.5f;      // sample interpolation and jitter
inline constexpr float kBodyLevelM = 1.0f;   // the body, level (today's numbers)
inline constexpr float kSwordReachFloorM = CoopPvp::kSwordReachM;  // a weapon without a Range: 1.3 m
inline constexpr float kShotReachM = 1.5f;
inline constexpr float kBlastReachM = 3.0f;  // today's bomb reach

// What the victim's game knows for one strike.
struct StrikeView {
    CoopPvp::Local local;                 // our game this frame
    CoopPvp::Hitter attacker;             // the attacker as presence knows it
    bool downed = false;                  // our Link is downed or dead (coop_dbno)
    const PlaceRing* own = nullptr;       // our Link's places
    const PlaceRing* attacker_ring = nullptr;  // our copy of the attacker's places
    // The weapon's reach from the game's data (WeaponParam.Range of the
    // weapon PLAYER_EQUIP says the attacker holds); nothing: it has none.
    std::optional<float> weapon_range;
    // A shot or blast: the last place our game has for the strike's object;
    // nothing when our game does not have that object.
    std::optional<Vec3> object_at;
    // The server time at which we first saw the attacker's status say it is away (paused, loading, a cutscene,
    // a menu, dead); 0: it is not away, or since when is not known.
    std::uint64_t attacker_away_us = 0;
};

// Both hit at once (5.6): two players swing in the same moment and one of the two strikes downs its victim. The
// downed game's own strike was swung before it fell and is on its way; it counts (a trade, as two of the game's
// enemies trade), also when the news of the fall (channel 0) overtakes the strike (channel 2, sent again after a
// lost packet). So an attacker whose status went away no longer than this ago is not kept off for it.
inline constexpr std::uint64_t kTradeGraceUs = 200'000;

inline bool AttackerAway(const StrikeView& v, std::uint64_t now_us) {
    if (!CoopPvp::Guarded(v.attacker.status)) return false;
    return !(v.attacker_away_us != 0 && now_us >= v.attacker_away_us &&
             now_us - v.attacker_away_us <= kTradeGraceUs);
}

inline float Distance(const Vec3& a, const Vec3& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

inline bool Finite(const Vec3& v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}

// The hit's place on the body standing at `feet` (y up), with the jitter.
inline bool OnBody(const Vec3& hit, const Vec3& feet) {
    const float dx = hit[0] - feet[0], dz = hit[2] - feet[2];
    const float dy = hit[1] - feet[1];
    return std::sqrt(dx * dx + dz * dz) <= kBodyLevelM + kJitterM && dy >= CoopPvp::kBodyLowM - kJitterM &&
           dy <= CoopPvp::kBodyHighM + kJitterM;
}

// The sword's or body's reach: the weapon's Range, never less than 1.3 m,
// plus the jitter.
inline float SwordReach(std::optional<float> weapon_range) {
    const float range = weapon_range && std::isfinite(*weapon_range) ? *weapon_range : 0.0f;
    return std::max(range, kSwordReachFloorM) + kJitterM;
}

// The kept-off cases of today's PvP (coop_pvp.cpp Victim::Hit) plus a
// downed or dead Link, and a copy of the attacker we do not know.
inline bool KeptOff(const StrikeView& v, std::uint64_t now_us) {
    const auto& l = v.local;
    const auto& a = v.attacker;
    return v.downed || !l.life || *l.life <= 0 || l.event || CoopPvp::Guarded(l.status) || !l.scene ||
           !a.known || AttackerAway(v, now_us) || !a.scene || !coop::protocol::same_place(*a.scene, *l.scene);
}

// The checks after the seq: nothing when the strike passes (the game's hit
// path decides the rest: taken, guarded, guard broken, invincible); else the
// outcome to answer with.
inline std::optional<proto::PvpOutcome> Refusal(const proto::PvpStrike& s, const StrikeView& v,
                                                std::uint64_t now_us) {
    if (KeptOff(v, now_us) || v.own == nullptr || v.attacker_ring == nullptr) return proto::PvpOutcome::kept_off;
    // Never wrapped: a seen_us after now is as late as one too old.
    if (s.seen_us > now_us || now_us - s.seen_us > kStrikeLateUs) return proto::PvpOutcome::late;
    if (!Finite(s.at) || !Finite(s.from)) return proto::PvpOutcome::out_of_reach;
    // Where we were when the attacker saw us.
    const auto we = v.own->At(s.seen_us, kOwnRingSlackUs);
    if (!we || !OnBody(s.at, *we)) return proto::PvpOutcome::out_of_reach;
    // Where we saw the attacker between then and now: the strike's `from` is its place when it hit, later than
    // `seen_us` by its display delay of us and a trip (a running or jumping attacker moved meanwhile). `they` is
    // the point of our own samples' path nearest to what it says; it must be within the jitter of it.
    const auto they = v.attacker_ring->NearestSince(
        s.seen_us > kCopyRingSlackUs ? s.seen_us - kCopyRingSlackUs : 0, s.from);
    if (!they || Distance(s.from, *they) > kJitterM) return proto::PvpOutcome::out_of_reach;
    switch (s.source) {
        case proto::PvpSource::shot:
        case proto::PvpSource::blast: {
            if (!v.object_at) return proto::PvpOutcome::out_of_reach;
            const float reach = s.source == proto::PvpSource::shot ? kShotReachM : kBlastReachM;
            if (Distance(*v.object_at, s.at) > reach) return proto::PvpOutcome::out_of_reach;
            break;
        }
        case proto::PvpSource::sword:
        case proto::PvpSource::other:  // the body's own reach, as a sword without a Range
        default:
            if (Distance(*they, s.at) > SwordReach(s.source == proto::PvpSource::sword ? v.weapon_range
                                                                                     : std::nullopt)) {
                return proto::PvpOutcome::out_of_reach;
            }
            break;
    }
    return std::nullopt;
}

// --- Path B only: the amount from the victim's own tables (5.4) ---

// The kind's key: its first two words (the attack kind, as the record's
// header carries it: CoopHitReplay::kRecKind).
inline std::uint64_t KindKey(const proto::PvpKind& kind) {
    return (static_cast<std::uint64_t>(kind[0]) << 32) | kind[1];
}

// The victim's own copy of the game's tables, read from its romfs.
struct Tables {
    // WeaponParam BaseAttack by the weapon's id as PLAYER_EQUIP names it.
    std::map<std::uint32_t, std::int32_t> base_attack;
    // GameBalance/AttackParam/Player(_Child): DamageMultiplier by kind key.
    std::map<std::uint64_t, float> multiplier;
    // Link's DamageRates for the kind's damage type (DamageParameters/Player:
    // every one 1.0).
    float damage_rate = 1.0f;
    // Life units per point of attack (the game's own; U7 reads it).
    std::int32_t life_per_attack = 1;
};

// The amount our game counts for a strike's kind with the attacker's weapon,
// or nothing when the tables do not name the weapon or the kind (answered
// kept off). The attacker's own amount is never used.
inline std::optional<std::int32_t> PathBAmount(const Tables& t, std::uint32_t weapon,
                                               const proto::PvpKind& kind) {
    const auto w = t.base_attack.find(weapon);
    const auto k = t.multiplier.find(KindKey(kind));
    if (w == t.base_attack.end() || k == t.multiplier.end()) return std::nullopt;
    if (!std::isfinite(k->second) || !std::isfinite(t.damage_rate)) return std::nullopt;
    const double raw = static_cast<double>(w->second) * t.life_per_attack * k->second * t.damage_rate;
    const auto amount = static_cast<std::int32_t>(std::lround(std::clamp(raw, 0.0, double(proto::kMaxPvpLife))));
    return amount;
}

// Path B's invincible time: today's kMercyUs after a taken strike, until U7
// shows the life replay sets the game's own (5.5).
class Mercy {
public:
    bool Refuses(std::uint64_t now_us) const { return last_ && now_us < *last_ + CoopPvp::kMercyUs; }
    void Taken(std::uint64_t now_us) { last_ = now_us; }
    void Reset() { last_.reset(); }

private:
    std::optional<std::uint64_t> last_;
};

// --- One strike, path B, start to end ---

struct PathBInput {
    std::uint32_t weapon = 0;     // the attacker's weapon id (PLAYER_EQUIP)
    bool guarding = false;        // our Link's main AS is a guard one (U5), facing the attacker within 120 deg
    bool guard_fails = false;     // the shield fails for this kind (BadConditionAttackGuard) or GuardBreakPower
};

struct PathBDecision {
    proto::PvpResult result;      // to send (player field 0)
    std::int32_t take = 0;        // life to take off our Link through the hit replay
    bool replay = false;          // replay the kind on our Link (knockback, hurt reaction)
};

// Path B: the checks, the guard, the mercy, the amount. Nothing when the
// strike is dropped with no result (another victim, our own, a repeated seq,
// PvP off).
inline std::optional<PathBDecision> DecidePathB(const proto::PvpStrike& s, bool pvp_on, const StrikeView& v,
                                                const PathBInput& in, const Tables& t, SeqGate& seq,
                                                Mercy& mercy, std::uint64_t now_us) {
    const std::uint8_t self = v.local.self;
    if (!pvp_on || self == 0 || s.victim != self || s.player_number == self || s.player_number < 1 ||
        s.player_number > proto::kMaxPlayers || !seq.Take(s.player_number, s.seq)) {
        return std::nullopt;
    }
    PathBDecision d;
    d.result.attacker = s.player_number;
    d.result.seq = s.seq;
    d.result.kind = s.kind;
    d.result.at = s.at;
    const std::int32_t life = std::clamp<std::int32_t>(v.local.life.value_or(0), 0, proto::kMaxPvpLife);
    d.result.life = life;
    if (const auto no = Refusal(s, v, now_us)) {
        d.result.outcome = *no;
        return d;
    }
    if (in.guarding && !in.guard_fails) {
        d.result.outcome = proto::PvpOutcome::guarded;
        return d;
    }
    if (mercy.Refuses(now_us)) {
        d.result.outcome = proto::PvpOutcome::invincible;
        return d;
    }
    const auto amount = PathBAmount(t, in.weapon, s.kind);
    if (!amount) {
        d.result.outcome = proto::PvpOutcome::kept_off;
        return d;
    }
    d.take = std::min(*amount, life);
    d.replay = true;
    d.result.outcome = in.guarding ? proto::PvpOutcome::guard_broken : proto::PvpOutcome::taken;
    d.result.amount = d.take;
    d.result.life = life - d.take;
    mercy.Taken(now_us);
    return d;
}

// --- Glue part 2: the pieces the hook's glue runs each frame ---

// The weapon ids path B's tables use, from the weapon PLAYER_EQUIP names (the puppet's gear "weapon" slot, its
// actor name: "Weapon_Sword_Kokiri", "..._Alb", "..._Look", "..._Blade5" all one weapon). 0: not known.
enum WeaponId : std::uint32_t {
    kWeaponNone = 0,
    kWeaponKokiri = 1,
    kWeaponMaster = 2,
    kWeaponBiggoron = 3,
    kWeaponGiant = 4,
    kWeaponBroken = 5,  // the Broken Giant's Knife / Broken Biggoron: no BaseAttack read yet
    kWeaponHammer = 6,
    kWeaponStick = 7,
};

inline bool Has(std::string_view text, std::string_view word) { return text.find(word) != std::string_view::npos; }

inline std::uint32_t WeaponOf(std::string_view weapon) {
    if (weapon.substr(0, 7) != "Weapon_") return kWeaponNone;
    if (Has(weapon, "Broken")) return kWeaponBroken;  // before Biggoron and Giant: "BrokenBiggoron", "BrokenGiant"
    if (Has(weapon, "Kokiri")) return kWeaponKokiri;
    if (Has(weapon, "Master")) return kWeaponMaster;
    if (Has(weapon, "Biggoron")) return kWeaponBiggoron;
    if (Has(weapon, "Giant")) return kWeaponGiant;
    if (Has(weapon, "Hammer")) return kWeaponHammer;
    if (Has(weapon, "Stick")) return kWeaponStick;
    return kWeaponNone;
}

// Path B's first tables (5.4): the weapons' BaseAttack as docs 5.4 lists them from WeaponParam (Kokiri Sword 1,
// Master Sword 2, Biggoron / Giant's Knife 4, Megaton Hammer 3, Deku Stick 2). The kinds' multipliers are NOT
// filled: their keys (the record's kind words) are read by U2/U7, so until then every strike is answered
// `kept off` and takes nothing (PathBAmount); the broken knife has no BaseAttack read and is kept off too.
inline Tables FirstTables() {
    Tables t;
    t.base_attack = {{kWeaponKokiri, 1}, {kWeaponMaster, 2}, {kWeaponBiggoron, 4},
                     {kWeaponGiant, 4},  {kWeaponHammer, 3}, {kWeaponStick, 2}};
    return t;
}

// Path B can take life only once its tables name a kind: until then the old PVP_HIT (242) path stays the one
// that hurts, also between two native games (no PvP lost while U2/U7 are open); from then on the victim drops
// 242 from a native attacker, so a hit never counts twice (proof 9).
inline bool PathBLive(const Tables& t) { return !t.multiplier.empty() && !t.base_attack.empty(); }

// Our Link guards toward the attacker (path B, 5.3): his main AS names a guard one (any case: "guard"; U5 reads
// the game's names, the glue logs each one once) and the attacker stands within kGuardHalfDeg of his facing.
// The game's GuardableAngleMaxDeg is 120 (DamageParameters/Player); it is taken as the whole cone (60 each side)
// until U5's run shows otherwise.
inline constexpr float kGuardHalfDeg = 60.0f;

inline bool GuardAnim(std::string_view name) {
    constexpr std::string_view word = "guard";
    for (std::size_t i = 0; i + word.size() <= name.size(); ++i) {
        bool same = true;
        for (std::size_t k = 0; k < word.size() && same; ++k) {
            char c = name[i + k];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            same = c == word[k];
        }
        if (same) return true;
    }
    return false;
}

// Link's facing on the ground: his rotation (x, y, z, w) turning +z (the game's forward), y dropped.
inline std::optional<std::array<float, 2>> FacingXZ(const proto::Quat& q) {
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w)) return std::nullopt;
    // The rotation's third column: q (0, 0, 1) q*.
    const float fx = 2.0f * (x * z + w * y);
    const float fz = 1.0f - 2.0f * (x * x + y * y);
    const float len = std::sqrt(fx * fx + fz * fz);
    if (!(len > 1e-4f)) return std::nullopt;
    return std::array<float, 2>{fx / len, fz / len};
}

inline bool FacesToward(const proto::Quat& rotation, const Vec3& self, const Vec3& other) {
    const auto f = FacingXZ(rotation);
    const float dx = other[0] - self[0], dz = other[2] - self[2];
    const float len = std::sqrt(dx * dx + dz * dz);
    if (!f || !(len > 1e-4f)) return false;
    const float cos = ((*f)[0] * dx + (*f)[1] * dz) / len;
    return cos >= std::cos(kGuardHalfDeg * 3.14159265f / 180.0f);
}

inline bool Guarding(const std::optional<std::string_view>& main_anim, const std::optional<proto::Quat>& rotation,
                     const Vec3& self, const Vec3& attacker) {
    return main_anim && rotation && GuardAnim(*main_anim) && FacesToward(*rotation, self, attacker);
}

// Whether our Link guarded toward a place, each frame over the last second (5.6, "a shield raised just before"):
// the attacker hit what its copy of us showed at `seen_us`, and our Link may have raised or lowered the shield
// since. Path B's rule, the same on both screens: the strike is guarded when our Link guards toward the attacker
// NOW (a blade never goes through a shield our own screen shows raised) or DID at `seen_us` (the attacker's
// game saw its blade meet the copy's raised shield, U6). So a shield raised up to kStrikeLateUs after the swing
// still guards: lag is paid by the attacker, never by a hit through a shield.
class GuardRing {
public:
    void Push(std::uint64_t t_us, bool guard_anim, const std::optional<proto::Quat>& rotation, const Vec3& at) {
        if (!states_.empty() && t_us < states_.back().t_us) states_.clear();  // a clock step: start again
        states_.push_back({t_us, guard_anim && rotation.has_value(), rotation.value_or(proto::Quat{}), at});
        while (!states_.empty() && t_us - states_.front().t_us > kRingUs) states_.pop_front();
    }
    void Clear() { states_.clear(); }
    // Our Link guarded toward `attacker` at the frame nearest `t_us` (within `slack_us`).
    bool Toward(std::uint64_t t_us, std::uint64_t slack_us, const Vec3& attacker) const {
        const State* best = nullptr;
        std::uint64_t best_gap = 0;
        for (const auto& st : states_) {
            const std::uint64_t gap = st.t_us > t_us ? st.t_us - t_us : t_us - st.t_us;
            if (gap <= slack_us && (best == nullptr || gap < best_gap)) {
                best = &st;
                best_gap = gap;
            }
        }
        return best != nullptr && best->guard && FacesToward(best->rotation, best->at, attacker);
    }

private:
    struct State {
        std::uint64_t t_us;
        bool guard;
        proto::Quat rotation;
        Vec3 at;
    };
    std::deque<State> states_;
};

// Path B's `guarding` for one strike: now, or when the attacker saw us.
inline bool GuardsStrike(bool guarding_now, const GuardRing& ring, const proto::PvpStrike& s) {
    return guarding_now || (Finite(s.from) && ring.Toward(s.seen_us, kOwnRingSlackUs, s.from));
}

// The attacker's side: a _Pvp copy's Life component took a new hit (its last hit id, +0x1b18, moved to one that
// is not our own third-game replay). One watch per player number; a new actor (a re-make, a new scene) starts
// again without a strike.
class CopyHitWatch {
public:
    // True when `id` is a new hit of our game's own on player `number`'s copy `actor`.
    bool Moved(std::uint8_t number, std::uint64_t actor, std::uint32_t id, std::uint32_t replayed) {
        auto [it, fresh] = last_.try_emplace(number, Last{actor, id});
        if (fresh || it->second.actor != actor) {
            it->second = Last{actor, id};
            return false;
        }
        if (it->second.id == id) return false;
        it->second.id = id;
        return id != replayed;
    }
    void Forget(std::uint8_t number) { last_.erase(number); }
    void Reset() { last_.clear(); }

private:
    struct Last {
        std::uint64_t actor = 0;
        std::uint32_t id = 0;
    };
    std::map<std::uint8_t, Last> last_;
};

// --- The copy's Life is invincible from its creation (5.1; review M1 of glue part 2) ---

// The game's own named invincibility, as the AI node OneShotLifeSetInvincible gives it (coop_dbno_body.h read it:
// IsNoReaction picks the list and so the function). IsNoReaction 0, the list a hit still REACTS on (flash,
// sparks, sound, hit-stop) while nothing is taken: main+0xb34800 (Life+0x1c28), (x0 the Life component, w1 1 adds
// the key, x2 -> the u32 key). Design r2 named main+0x8d75e0 for it, which is the no-reaction list
// (IsNoReaction 1, the downed Link's): not this one. Whether the reaction shows is U1.
inline constexpr std::uint64_t kInvincibleReacting = 0xb34800;  // main+
inline constexpr std::uint32_t kCopyShieldKey = 0xC0DB0D02u;   // ours (no game name hashes to it); dbno uses ...01
inline constexpr unsigned kCopyShieldTries = 3;               // calls per copy before it is given up (logged)

// Which live _Pvp copies' Life components still need our key: each (actor, Life) once. A copy that goes (a
// re-make, a load, PvP off) is forgotten with its component; a new one at the same address is shielded again.
class CopyShield {
public:
    using Copy = std::pair<std::uint64_t, std::uint64_t>;  // (actor, its Life component)
    // The live copies this frame; the next one to shield, if any.
    std::optional<Copy> Next(const std::vector<Copy>& live) {
        for (auto it = done_.begin(); it != done_.end();) {
            it = std::find(live.begin(), live.end(), *it) == live.end() ? done_.erase(it) : std::next(it);
        }
        for (auto it = tries_.begin(); it != tries_.end();) {
            it = std::find(live.begin(), live.end(), it->first) == live.end() ? tries_.erase(it) : std::next(it);
        }
        for (const Copy& c : live) {
            if (c.first == 0 || c.second == 0 || done_.count(c) != 0) continue;
            if (tries_[c] >= kCopyShieldTries) continue;  // given up: the glue logged it
            return c;
        }
        return std::nullopt;
    }
    // A call was made for `c` (true: the game returned; false: no block, or the call failed).
    void Called(const Copy& c, bool ok) {
        if (ok) {
            done_.insert(c);
            tries_.erase(c);
        } else {
            ++tries_[c];
        }
    }
    bool Shielded(const Copy& c) const { return done_.count(c) != 0; }
    bool GivenUp(const Copy& c) const {
        const auto it = tries_.find(c);
        return it != tries_.end() && it->second >= kCopyShieldTries;
    }
    void Reset() {
        done_.clear();
        tries_.clear();
    }

private:
    std::set<Copy> done_;
    std::map<Copy, unsigned> tries_;
};

// One new hit on a copy as the walk read it (the record words as CoopHitReplay::Record lays them out).
struct CopyHit {
    std::uint8_t victim = 0;
    RecordFrom from;
    proto::PvpKind kind{};
    std::int32_t amount = 0;  // Life +0x228: what our game counted (compared on the victim, never used)
    Vec3 at{};                // the record's hit place
    Vec3 attacker_at{};       // the record's attacker place (our Link)
    bool own_shot = false;    // a shot or blast of ours whose made object the walk cannot name yet (logged)
};

// The strike for one copy hit, or nothing when the record is not ours (5.4: an enemy's, another player's shot) or
// is a shot or blast whose made object our spawn sync did not name (the victim could not check it).
// `seen_us`/`copy_at`: the victim's sample our copy showed this frame (presence's shown pose).
inline std::optional<proto::PvpStrike> MakeStrike(const CopyHit& hit, std::uint8_t self, std::uint32_t seq,
                                                   std::uint64_t seen_us, const Vec3& copy_at) {
    const auto source = StrikeSource(hit.from, self);
    if (!source || hit.victim == 0 || hit.victim == self || hit.victim > proto::kMaxPlayers || seen_us == 0) {
        return std::nullopt;
    }
    if (!Finite(hit.at) || !Finite(hit.attacker_at) || !Finite(copy_at)) return std::nullopt;
    proto::PvpStrike s;
    s.player_number = self;
    s.victim = hit.victim;
    s.seq = seq;
    s.seen_us = seen_us;
    s.kind = hit.kind;
    s.amount = std::clamp<std::int32_t>(hit.amount, 0, proto::kMaxPvpLife);
    s.source = *source;
    if (*source != proto::PvpSource::sword && hit.from.made) s.object = *hit.from.made;
    s.at = hit.at;
    s.copy_at = copy_at;
    s.from = hit.attacker_at;
    return s;
}

// What a game that is not the victim does with a PVP_RESULT (5.4): the attacker plays the guard's recoil when
// guarded (U6: its own game may already have clanged), everyone else replays a landed kind on its copy of the
// victim (flash, sparks, sound; the copy's Life is invincible); nothing for the other outcomes.
enum class ResultPlay : std::uint8_t { none, replay_on_copy, attacker_recoil };

inline ResultPlay PlayResult(const proto::PvpResult& r, std::uint8_t self) {
    if (self == 0 || r.player_number == self || r.player_number == 0) return ResultPlay::none;
    const bool landed = r.outcome == proto::PvpOutcome::taken || r.outcome == proto::PvpOutcome::guard_broken;
    if (r.attacker == self) {
        return r.outcome == proto::PvpOutcome::guarded ? ResultPlay::attacker_recoil : ResultPlay::none;
    }
    return landed ? ResultPlay::replay_on_copy : ResultPlay::none;
}

}  // namespace CoopPvpNative
