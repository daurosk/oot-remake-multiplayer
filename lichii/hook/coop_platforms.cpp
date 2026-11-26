// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_platforms.h"

#include "coop_room_objects.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>

namespace CoopPlatforms {
namespace {

// The actor's own place and turn (+0x31c .. +0x34b): the game writes them
// from the clock; they are not the clock.
constexpr std::uint64_t kTransformFrom = 0x318;
constexpr std::uint64_t kTransformTo = 0x350;

bool LooksLikePointer(std::uint64_t w) {
    const std::uint64_t high = w >> 32;
    return high >= 1 && high <= 0xffff && w % 8 == 0;
}

float AsFloat(std::uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return f;
}

float Distance(const coop::protocol::Vec3& a, const coop::protocol::Vec3& b) {
    const float x = a[0] - b[0];
    const float y = a[1] - b[1];
    const float z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

// A rate that reads as seconds (1 a second) or frames (30 or 60 a second):
// the likeliest clocks.
bool ClockLike(float rate) {
    const float r = std::fabs(rate);
    return std::fabs(r - 1.0f) < 0.1f || std::fabs(r - 30.0f) < 3.0f || std::fabs(r - 60.0f) < 6.0f;
}

}  // namespace

std::optional<float> WavePhaseCycle(const Floats& floats, std::uint64_t key, float rate) {
    if (const std::uint64_t region = key >> 32; region == kRegionWaveNode || region == kRegionRotateNode) {
        return NodePhaseCycle(floats, key);  // known from the node's code, whatever the frame time's unit
    }
    if ((key & 0xffffffffULL) < kWavePhaseFromCycle) {
        return std::nullopt;  // no cycle word before it in the same region
    }
    const auto c = floats.find(key - kWavePhaseFromCycle);
    if (c == floats.end() || !std::isfinite(c->second) || c->second <= 0.0f || c->second > 3600.0f || rate <= 0.0f) {
        return std::nullopt;
    }
    const float want = kTwoPi / c->second;
    if (std::fabs(rate - want) > ClockFinder::kRateSlack * want) {
        return std::nullopt;
    }
    return kTwoPi;
}

std::optional<float> NodePhaseCycle(const Floats& floats, std::uint64_t key) {
    const std::uint64_t region = key >> 32;
    const std::uint64_t offset = key & 0xffffffffu;
    const auto word = [&floats, region](std::uint64_t at) -> std::optional<float> {
        const auto i = floats.find(Key(region, at));
        if (i == floats.end() || !std::isfinite(i->second)) {
            return std::nullopt;
        }
        return i->second;
    };
    if (region == kRegionWaveNode && offset >= GroundNode::kWavePhaseFirst && offset <= GroundNode::kWavePhaseLast) {
        const std::uint64_t cycle_at = offset - (offset < GroundNode::kWaveAngularFirst ? 48 : 36);
        const auto cycle = word(cycle_at);
        return cycle && *cycle > 0.0f ? std::optional<float>(kTwoPi) : std::nullopt;
    }
    if (region == kRegionRotateNode && offset == GroundNode::kRotateAngle) {
        const auto speed = word(GroundNode::kRotateSpeed);
        return speed && *speed != 0.0f ? std::optional<float>(kTwoPi) : std::nullopt;
    }
    return std::nullopt;
}

namespace GroundNode {

std::uint64_t RegionOf(const ReadWord& read, std::uint64_t node, std::uint64_t actor, std::uint64_t main) {
    if (node == 0 || actor == 0 || main == 0) {
        return 0;
    }
    const auto vtable = read(node);
    const auto owner = read(node + kActor);
    if (!vtable || !owner || *owner != actor) {
        return 0;
    }
    if (*vtable == main + kWaveVtable) {
        return kRegionWaveNode;
    }
    if (*vtable == main + kRotateVtable) {
        return kRegionRotateNode;
    }
    return 0;
}

Search::Search(std::uint64_t actor, std::uint64_t main) : actor_(actor), main_(main) {
    objects_.push_back({actor, 0, 0, 0});
    seen_.insert(actor);
}

std::string Search::PathOf(std::size_t i) const {
    std::vector<std::uint64_t> vias;
    for (; i != 0; i = objects_[i].parent) {
        vias.push_back(objects_[i].via);
    }
    std::string text = "actor";
    for (auto v = vias.rbegin(); v != vias.rend(); ++v) {
        char part[24];
        std::snprintf(part, sizeof part, "%s+0x%llx", v == vias.rbegin() ? "" : " > ",
                      static_cast<unsigned long long>(*v));
        text += part;
    }
    return text;
}

std::optional<Hit> Search::Step(const ReadWord& read, std::size_t budget) {
    std::size_t used = 0;
    const ReadWord counted = [&read, &used, this](std::uint64_t a) {
        ++used;
        ++reads_;
        return read(a);
    };
    const auto next = [this] {
        ++i_;
        j_ = 0;
        checked_ = false;
    };
    while (!done_ && used < budget) {
        if (i_ >= objects_.size() || i_ >= kMaxObjects) {
            done_ = true;
            looked_ = i_;
            break;
        }
        const Object o = objects_[i_];
        if (!checked_) {
            checked_ = true;
            if (i_ != 0) {
                if (const std::uint64_t region = RegionOf(counted, o.address, actor_, main_); region != 0) {
                    done_ = true;
                    looked_ = i_ + 1;
                    hit_ = Hit{o.address, region, PathOf(i_)};
                    break;
                }
            }
            if (o.depth >= kMaxDepth) {
                next();
            }
            continue;  // the budget is looked at again before the object's words
        }
        if (j_ >= (i_ == 0 ? kActorWords : kObjectWords)) {
            next();
            continue;
        }
        const auto w = counted(o.address + 8 * j_);
        if (!w) {
            next();  // the object's end (unmapped)
            continue;
        }
        if (LooksLikePointer(*w) && seen_.insert(*w).second) {
            objects_.push_back({*w, o.depth + 1, i_, 8 * j_});
        }
        ++j_;
    }
    if (!done_) {
        looked_ = i_;
    }
    return hit_;
}

std::optional<Hit> Find(const ReadWord& read, std::uint64_t actor, std::uint64_t main, std::size_t* looked) {
    Search search(actor, main);
    auto hit = search.Step(read, static_cast<std::size_t>(-1));
    if (looked != nullptr) {
        *looked = search.Looked();
    }
    return hit;
}

std::map<std::uint64_t, std::uint64_t> Words(const ReadWord& read, const Hit& hit) {
    std::map<std::uint64_t, std::uint64_t> out;
    for (std::uint64_t at = kFrom; at < kTo; at += 8) {
        if (const auto w = read(hit.node + at)) {
            out[Key(hit.region, at)] = *w;
        }
    }
    return out;
}

}  // namespace GroundNode

bool IsDriver(std::string_view name) {
    if (name.starts_with("GroundControlActor_") || name.starts_with("GrouondControlActor_")) {
        return true;  // the game's movers of floors (GrouondControl: the game's own spelling)
    }
    if (!name.starts_with("Obj_") && !name.starts_with("Land_")) {
        return false;
    }
    static constexpr std::array<std::string_view, 11> kParts{
        "ElevatorLift", "_Elevator_", "MoveFloor", "RotateFloor", "RotateBlade", "MoveSpike",
        "ExtendingPlatform", "_lift_", "ElevatorMirror", "ElevatorPit", "UpDown"};
    for (const std::string_view p : kParts) {
        if (name.find(p) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

bool PlatformSync::OnFootprint(const coop::protocol::Vec3& platform, const coop::protocol::Vec3& who) {
    const float dx = who[0] - platform[0];
    const float dz = who[2] - platform[2];
    const float dy = who[1] - platform[1];
    return dx * dx + dz * dz <= kRideRadiusM * kRideRadiusM && dy >= -kRideBelowM && dy <= kRideAboveM;
}

bool PlatformSync::OurLinkRides(const Driver& d, const coop::protocol::Vec3& player) {
    if (d.ride_plug == false) {
        return false;  // the game says no player is on it
    }
    if (d.name.starts_with("GroundControlActor_") || d.name.starts_with("GrouondControlActor_")) {
        return d.ride_plug.value_or(false);
    }
    const bool ours_on = OnFootprint(d.position, player);
    if (d.ride_plug == true) {
        return ours_on || !d.other_on;  // only another player's puppet on it is not ours
    }
    return ours_on;
}

std::string Where(std::uint64_t key) {
    const std::uint64_t region = key >> 32;
    const auto offset = static_cast<unsigned long long>(key & 0xffffffffu);
    char text[48];
    if (region == kRegionAsFrame) {
        std::snprintf(text, sizeof text, "as-frame");
    } else if (region == kRegionWaveNode) {
        std::snprintf(text, sizeof text, "wave-node+0x%llx", offset);
    } else if (region == kRegionRotateNode) {
        std::snprintf(text, sizeof text, "rotate-node+0x%llx", offset);
    } else if (region == kRegionActor) {
        std::snprintf(text, sizeof text, "actor+0x%llx", offset);
    } else {
        std::snprintf(text, sizeof text, "c+0x%llx+0x%llx",
                      static_cast<unsigned long long>((region - kRegionComponent) * 8), offset);
    }
    return text;
}

Floats FloatsOf(const std::map<std::uint64_t, std::uint64_t>& words) {
    Floats out;
    for (const auto& [key, w] : words) {
        const std::uint64_t region = key >> 32;
        const std::uint64_t offset = key & 0xffffffffu;
        if (region == 1) {
            continue;  // the component table: pointers
        }
        if (region == kRegionAsFrame) {
            out[key] = AsFloat(static_cast<std::uint32_t>(w));  // the frame is in the low half
            continue;
        }
        if (LooksLikePointer(w)) {
            continue;
        }
        for (std::uint64_t half = 0; half < 2; ++half) {
            const std::uint64_t at = offset + 4 * half;
            if (region == kRegionActor && at >= kTransformFrom && at < kTransformTo) {
                continue;
            }
            out[Key(region, at)] = AsFloat(static_cast<std::uint32_t>(w >> (32 * half)));
        }
    }
    return out;
}

void ClockFinder::Add(std::uint64_t now_us, const Floats& floats) {
    jumped_ = false;
    if (!looks_.empty() && now_us <= looks_.back().at_us) {
        return;
    }
    looks_.push_back({now_us, floats});
    while (looks_.size() > kLooks) {
        looks_.pop_front();
    }
    if (looks_.size() < 3) {
        return;
    }
    const Look& a = looks_[looks_.size() - 3];
    const Look& b = looks_[looks_.size() - 2];
    const Look& c = looks_[looks_.size() - 1];
    const float dt1 = static_cast<float>(b.at_us - a.at_us) / 1e6f;
    const float dt2 = static_cast<float>(c.at_us - b.at_us) / 1e6f;
    std::map<std::uint64_t, Candidate> next;
    const auto ban = [&](std::uint64_t key, float value, const char* why) {
        if (banned_.insert(key).second) {
            dropped_.push_back({key, value, why});
        }
        if (chosen_ && *chosen_ == key) {
            chosen_.reset();
        }
    };
    for (const auto& [key, v2] : c.floats) {
        if (banned_.count(key) != 0) {
            continue;
        }
        const auto i0 = a.floats.find(key);
        const auto i1 = b.floats.find(key);
        if (i0 == a.floats.end() || i1 == b.floats.end()) {
            continue;
        }
        const float v0 = i0->second;
        const float v1 = i1->second;
        if (!std::isfinite(v0) || !std::isfinite(v1) || !std::isfinite(v2) ||
            std::fabs(v0) > kMaxValue || std::fabs(v1) > kMaxValue || std::fabs(v2) > kMaxValue) {
            continue;
        }
        const float r1 = (v1 - v0) / dt1;
        const float r2 = (v2 - v1) / dt2;
        const auto in_range = [](float r) {
            return std::fabs(r) >= kMinRate && std::fabs(r) <= kMaxRate;
        };
        const auto it = candidates_.find(key);
        const bool known = it != candidates_.end();
        Candidate cand = known ? it->second : Candidate{};
        const bool agree = in_range(r1) && in_range(r2) && (r1 > 0) == (r2 > 0) &&
                           std::fabs(r1 - r2) <= kRateSlack * std::max(std::fabs(r1), std::fabs(r2));
        if (agree && cand.wave && r2 < 0.0f && (key >> 32) != kRegionRotateNode) {
            ban(key, v2, "went back other than by looping: a wave's offset, not its phase");
            continue;
        }
        if (agree) {
            cand.rate = r2;
            cand.good = std::min(cand.good + 1, 1000);
            cand.bad = 0;
            cand.stalled = 0;
            cand.top = std::max(cand.top, std::max(v1, v2));
            if (const auto loop = WavePhaseCycle(c.floats, key, r2)) {
                if (!InLoop(v0, *loop) || !InLoop(v1, *loop) || !InLoop(v2, *loop)) {
                    ban(key, v2, "outside [0, 2 pi): worked out from another word, not the wave's phase");
                    continue;
                }
                cand.wave = true;
                cand.cycle = *loop;  // known from the game's code: no loop needs to be seen first
            }
            next[key] = cand;
            continue;
        }
        if (!known || cand.good < kKeepAfter) {
            continue;  // a word that does not step steadily: not a clock (yet)
        }
        // A kept clock that did not step at its rate this time. Gone back
        // past where it was: it looped (or a switch reset it), and where it
        // loops is the value it would have had, less the value it has. A
        // stall (a pause, a hitch: about no step) is neither a loop nor a
        // jump to send; the finder lets go of it if it lasts.
        const bool backwards = cand.rate > 0 ? v2 < v1 : v2 > v1;
        const bool stalled = std::fabs(r2) < kMinRate;
        // #196: a wave phase goes back only by looping (from near 2 pi to near 0); any other step back is an offset
        // that swings (sin of the phase), not the phase. A step back to its start (a switch or a reload setting the
        // phase to 0 again, review-platform-phase-r1) is a jump, not a swing.
        const bool to_start = cand.rate > 0 ? v2 < kWaveResetSlack : v2 > cand.cycle - kWaveResetSlack;
        if (cand.wave && !stalled &&
            (!InLoop(v2, cand.cycle) ||
             (backwards && !to_start &&
              !(cand.rate > 0 ? v1 > 0.5f * cand.cycle && v2 < 0.5f * cand.cycle
                              : v1 < 0.5f * cand.cycle && v2 > 0.5f * cand.cycle)))) {
            ban(key, v2, "went back other than by looping: a wave's offset, not its phase");
            continue;
        }
        if (stalled) {
            // Held as it is (not a bad step) while the game stands still,
            // but not Found(): nothing frozen is sent.
            if (++cand.stalled <= kMaxStalled) {
                next[key] = cand;
            }
            continue;
        }
        if (!cand.wave && backwards && in_range(r1) &&
            std::fabs(r1 - cand.rate) <= kRateSlack * std::fabs(cand.rate)) {
            const float would = v1 + cand.rate * dt2;
            const float cycle = cand.rate > 0 ? would - v2 : v2 - would;
            if (cycle > 0.0f && cycle >= std::fabs(v1) * 0.5f) {
                cand.cycle = cycle;
            }
        }
        if (chosen_ && *chosen_ == key) {
            jumped_ = true;
        }
        if (++cand.bad < kDropAfter) {
            next[key] = cand;  // kept: it may go on stepping from where it jumped to
        }
    }
    candidates_ = std::move(next);
    if (chosen_) {
        const auto it = candidates_.find(*chosen_);
        if (it == candidates_.end()) {
            chosen_.reset();
        }
    }
    if (!chosen_) {
        std::optional<std::uint64_t> best;
        for (const auto& [key, cand] : candidates_) {
            if (cand.good < kKeepAfter || cand.bad != 0 || !Trusted(key, cand.wave)) {
                continue;  // room-objects: a guessed word is never the clock (it was a place or a frame counter)
            }
            if (!best) {
                best = key;
                continue;
            }
            const Candidate& b2 = candidates_.at(*best);
            if (cand.wave != b2.wave) {
                if (cand.wave) {
                    best = key;  // a wave mover's phase before any other stepping word
                }
                continue;
            }
            const bool like = ClockLike(cand.rate);
            const bool best_like = ClockLike(b2.rate);
            if (like != best_like) {
                if (like) {
                    best = key;
                }
            } else if (cand.good > b2.good) {
                best = key;
            }
        }
        chosen_ = best;
    }
}

bool ClockFinder::Trusted(std::uint64_t key, bool wave) {
    return wave || (key >> 32) == kRegionAsFrame;
}

std::optional<std::string> PlatformSync::OwnLogic(const std::string& scene, const Driver& d) {
    if (d.ride_plug.has_value()) {
        return std::string("RideOnPlayer, live");  // a rider lift: CoopLiftRide drives it
    }
    if (const auto* p = CoopRoomObjects::Nearest(scene, d.name, d.position); p != nullptr && !p->why.empty()) {
        return std::string(p->why);
    }
    return std::nullopt;
}

std::optional<std::uint64_t> ClockFinder::StalledKey() const {
    if (!chosen_) {
        return std::nullopt;
    }
    const auto it = candidates_.find(*chosen_);
    if (it == candidates_.end() || it->second.stalled == 0) {
        return std::nullopt;
    }
    return *chosen_;
}

std::optional<ClockFinder::Clock> ClockFinder::Found() const {
    if (!chosen_ || looks_.empty()) {
        return std::nullopt;
    }
    const auto it = candidates_.find(*chosen_);
    const auto v = looks_.back().floats.find(*chosen_);
    if (it == candidates_.end() || v == looks_.back().floats.end() || it->second.stalled != 0) {
        return std::nullopt;
    }
    Clock c;
    c.key = *chosen_;
    c.rate = it->second.rate;
    c.value = v->second;
    c.cycle = it->second.cycle;
    c.at_us = looks_.back().at_us;
    return c;
}

void ClockFinder::Reset() {
    looks_.clear();
    candidates_.clear();
    chosen_.reset();
    jumped_ = false;
    banned_.clear();
    dropped_.clear();
}

bool ClockFinder::InLoop(float value, float cycle) {
    return cycle <= 0.0f || (value >= -0.01f && value < cycle + 0.01f);
}

float Advance(float value, float rate, float cycle, std::uint64_t sent_us, std::uint64_t now_us) {
    const double elapsed = now_us > sent_us ? static_cast<double>(now_us - sent_us) / 1e6 : 0.0;
    double v = static_cast<double>(value) + static_cast<double>(rate) * elapsed;
    if (cycle > 0.0f) {
        v = std::fmod(v, static_cast<double>(cycle));
        if (v < 0.0) {
            v += cycle;
        }
    }
    return static_cast<float>(v);
}

float DriftSeconds(float ours, float theirs, float rate, float cycle) {
    double d = static_cast<double>(theirs) - static_cast<double>(ours);
    if (cycle > 0.0f) {
        d = std::remainder(d, static_cast<double>(cycle));
    }
    const double r = std::fabs(static_cast<double>(rate));
    return static_cast<float>(std::fabs(d) / (r > 0.0 ? r : 1.0));
}

void PlatformSync::Receive(const coop::protocol::PlatformClock& message) {
    if (message.scene != scene_ || message.player_number == 0) {
        return;
    }
    for (const auto& t : message.ticks) {
        auto& by_player = heard_[t.hash];
        auto [it, fresh] = by_player.try_emplace(message.player_number);
        Heard& h = it->second;
        if (!fresh && message.server_time_us <= h.sent_us) {
            continue;  // late: an older tick than one we have
        }
        if (fresh || message.server_time_us > h.sent_us + kOwnerQuietUs) {
            h.since_us = message.server_time_us;  // a new run of sends
        }
        h.player = message.player_number;
        h.sent_us = message.server_time_us;
        h.got_us = message.server_time_us;  // server time: comparable with ours
        h.tick = t;
    }
}

std::uint8_t PlatformSync::OwnerOf(std::uint64_t hash, std::uint8_t self, std::uint64_t server_us) const {
    // The lowest number of the games that send this driver's clock, ourselves included: the same in every game
    // (as CoopCounters' pause leader, PR 279). The earlier rule (the longest sender keeps it, the lower number only
    // when two began together) let two games each count themselves the owner when their runs were seen at
    // different times; in the Deku Tree water room (bigrun-0115) each game gave up following the other and then
    // sent its own clock, so the other followed it in turn. A game that sends nothing (no clock found, gone quiet
    // for kOwnerQuietUs) is no candidate, so a lower number with no clock never stops the others.
    std::uint8_t owner = self;
    if (const auto it = heard_.find(hash); it != heard_.end()) {
        for (const auto& [player, h] : it->second) {
            if (player != 0 && player != self && server_us <= h.got_us + kOwnerQuietUs && player < owner) {
                owner = player;
            }
        }
    }
    return owner;
}

void PlatformSync::Tick(const std::string& scene, std::uint8_t self, std::uint64_t steady_us,
                        std::uint64_t server_us, const std::vector<Driver>& drivers,
                        const std::optional<coop::protocol::Vec3>& player, const ReadDriver& read,
                        bool probe_only, Out& out) {
    if (scene != scene_) {
        Reset();
        scene_ = scene;
    }
    for (auto it = heard_.begin(); it != heard_.end();) {
        std::erase_if(it->second, [&](const auto& p) { return server_us > p.second.got_us + kStaleUs; });
        it = it->second.empty() ? heard_.erase(it) : std::next(it);
    }
    if (!player || self == 0) {
        return;
    }
    std::vector<std::pair<float, const Driver*>> near;
    for (const Driver& d : drivers) {
        if (d.hash == 0 || d.hash == ~std::uint64_t{0} || !IsDriver(d.name)) {
            continue;
        }
        // room-objects: an object its own logic drives (a switch, a sensor, a rider) is never written from outside.
        if (const auto why = OwnLogic(scene, d)) {
            if (told_own_logic_.insert(d.hash).second) {
                char text[200];
                std::snprintf(text, sizeof text, "%016llx %s: driven by its own logic (%s): its clock is not shared",
                              static_cast<unsigned long long>(d.hash), d.name.c_str(), why->c_str());
                out.notes.emplace_back(text);
            }
            continue;
        }
        const float m = Distance(d.position, *player);
        if (m <= kNearM) {
            near.emplace_back(m, &d);
        }
    }
    std::sort(near.begin(), near.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    if (near.size() > kMaxWatched) {
        near.resize(kMaxWatched);
    }
    coop::protocol::PlatformClock message;
    message.scene = scene;
    message.server_time_us = server_us;
    for (const auto& [m, d] : near) {
        State& s = states_[d->hash];
        // Issue 194: no clock write moves our Link; a jump on it (the plug 0 in the air) keeps the hold.
        if (OurLinkRides(*d, *player)) {
            s.ride_until_us = steady_us + kRideGraceUs;
        }
        const bool ridden = steady_us < s.ride_until_us;
        if (s.held_for_ride && !ridden) {
            s.held_for_ride = false;
            char text[160];
            std::snprintf(text, sizeof text, "%016llx %s: our Link is off it: its clock may be put in step again",
                          static_cast<unsigned long long>(d->hash), d->name.c_str());
            out.notes.emplace_back(text);
        }
        if (s.last_look_us != 0 && steady_us < s.last_look_us + ClockFinder::kLookEveryUs) {
            continue;
        }
        const auto words = read(*d);
        if (words.empty()) {
            continue;
        }
        s.last_look_us = steady_us;
        const Floats floats = FloatsOf(words);
        s.finder.Add(steady_us, floats);
        for (const auto& drop : s.finder.TakeDropped()) {
            char text[240];
            std::snprintf(text, sizeof text, "%016llx %s: %s (%.3f) is no clock: %s",
                          static_cast<unsigned long long>(d->hash), d->name.c_str(), Where(drop.key).c_str(),
                          static_cast<double>(drop.value), drop.why);
            out.notes.emplace_back(text);
        }
        const std::uint8_t owner = OwnerOf(d->hash, self, server_us);
        if (owner == self) {
            auto clock = s.finder.Found();
            if (!clock) {
                if (const auto key = s.finder.StalledKey(); key && !s.paused_at) {
                    if (const auto v = floats.find(*key); v != floats.end()) {
                        s.paused_at = v->second;  // our game stands still (a pause menu): the shared clock does not
                    }
                }
                continue;
            }
            bool resumed = false;
            if (const auto stood = std::exchange(s.paused_at, std::nullopt);
                stood && s.sent && s.sent->key == clock->key && !probe_only && !s.gave_up && !ridden &&
                DriftSeconds(clock->value, *stood, clock->rate, clock->cycle) <= kResumeS) {
                const float want = Advance(s.sent->value, s.sent->rate, s.sent->cycle, s.sent_us, server_us);
                if (DriftSeconds(clock->value, want, clock->rate, clock->cycle) > kDriftS) {
                    char text[200];
                    std::snprintf(text, sizeof text,
                                  "%016llx %s: our game stood still: clock %s put where the shared one is (%.3f -> %.3f)",
                                  static_cast<unsigned long long>(d->hash), d->name.c_str(), Where(clock->key).c_str(),
                                  static_cast<double>(clock->value), static_cast<double>(want));
                    out.notes.emplace_back(text);
                    out.writes.push_back({d->address, d->hash, clock->key, want, clock->value});
                    ++writes_;
                    clock->value = want;
                    resumed = true;
                }
            }
            if (!s.logged_clock) {
                s.logged_clock = true;
                char text[200];
                std::snprintf(text, sizeof text, "%016llx %s: clock %s, %.3f a second, cycle %.3f: ours to send",
                              static_cast<unsigned long long>(d->hash), d->name.c_str(),
                              Where(clock->key).c_str(), static_cast<double>(clock->rate),
                              static_cast<double>(clock->cycle));
                out.notes.emplace_back(text);
            }
            if (resumed || s.finder.Jumped() || s.last_send_us == 0 || server_us >= s.last_send_us + kSendEveryUs) {
                if (message.ticks.size() < coop::protocol::kMaxPlatformTicks) {
                    if (s.since_us == 0 || server_us > s.last_send_us + kOwnerQuietUs) {
                        s.since_us = server_us;
                    }
                    s.last_send_us = server_us;
                    message.ticks.push_back({d->hash, clock->key, clock->value, clock->rate, clock->cycle});
                    s.sent = message.ticks.back();
                    s.sent_us = server_us;
                }
            }
            continue;
        }
        s.since_us = 0;  // another game has it
        s.paused_at.reset();
        if (probe_only || s.gave_up) {
            continue;
        }
        const auto hit = heard_.find(d->hash);
        if (hit == heard_.end()) {
            continue;
        }
        const auto hp = hit->second.find(owner);
        if (hp == hit->second.end()) {
            continue;
        }
        const Heard& h = hp->second;
        const auto mine = floats.find(h.tick.key);
        if (mine == floats.end() || !std::isfinite(mine->second)) {
            continue;
        }
        // #196: our own copy of that word is no clock here (our finder dropped it, or it is a wave phase by the
        // game's layout and stands outside [0, 2 pi)): a word the game works out again every frame. Writing it only
        // fights the game. Only a wave's loop is known from the code; any other clock's cycle is an estimate from
        // the one jump the owner saw (a frame lift's top can lie a step past it), so it is not judged by it.
        const bool wave = WavePhaseCycle(floats, h.tick.key, h.tick.rate).has_value();
        if (s.finder.Banned(h.tick.key) || (wave && !ClockFinder::InLoop(mine->second, kTwoPi))) {
            s.gave_up = true;
            char text[240];
            std::snprintf(text, sizeof text,
                          "%016llx %s: our %s (%.3f) is no clock here (outside the owner's loop %.3f or dropped): "
                          "not followed",
                          static_cast<unsigned long long>(d->hash), d->name.c_str(), Where(h.tick.key).c_str(),
                          static_cast<double>(mine->second), static_cast<double>(h.tick.cycle));
            out.notes.emplace_back(text);
            continue;
        }
        // Our word stands still after it was seen moving: our game is paused (#35). No write (it could not
        // hold) and nothing counted against us; once it moves again the first write puts it in step.
        const bool same_key = s.mine_key == h.tick.key && s.mine_was;
        const bool still = same_key && std::fabs(mine->second - *s.mine_was) <
                                           ClockFinder::kMinRate * (ClockFinder::kLookEveryUs / 1e6f);
        s.mine_moved = same_key && (s.mine_moved || !still);
        s.mine_key = h.tick.key;
        s.mine_was = mine->second;
        if (still && s.mine_moved) {
            s.written.reset();
            continue;
        }
        const float want = Advance(h.tick.value, h.tick.rate, h.tick.cycle, h.sent_us, server_us);
        const float drift = DriftSeconds(mine->second, want, h.tick.rate, h.tick.cycle);
        if (drift <= kDriftS) {
            s.no_hold = 0;
            s.written.reset();
            continue;
        }
        if (ridden) {
            // Issue 194: written now, the platform would jump with our Link on it. Put in step once he is
            // off; nothing was written, so the next write is not one the game "took back".
            s.written.reset();
            if (!s.held_for_ride) {
                s.held_for_ride = true;
                char text[200];
                std::snprintf(text, sizeof text,
                              "%016llx %s: our Link rides it (%s): its clock is left %.2f s off until he is off",
                              static_cast<unsigned long long>(d->hash), d->name.c_str(),
                              d->ride_plug ? "RideOnPlayer" : "footprint", static_cast<double>(drift));
                out.notes.emplace_back(text);
            }
            continue;
        }
        if (s.last_write_us != 0 && steady_us < s.last_write_us + kWriteEveryUs) {
            continue;
        }
        if (s.written) {
            // Off again since our last write: the game took it back.
            if (++s.no_hold >= kMaxNoHold) {
                s.gave_up = true;
                char text[200];
                std::snprintf(text, sizeof text,
                              "%016llx %s: our writes of %s do not hold (%d in a row): stopped following",
                              static_cast<unsigned long long>(d->hash), d->name.c_str(),
                              Where(h.tick.key).c_str(), s.no_hold);
                out.notes.emplace_back(text);
                continue;
            }
        }
        if (!s.logged_follow) {
            s.logged_follow = true;
            char text[200];
            std::snprintf(text, sizeof text,
                          "%016llx %s: following player %u's clock %s (ours %.3f, theirs %.3f, %.2f s off)",
                          static_cast<unsigned long long>(d->hash), d->name.c_str(),
                          static_cast<unsigned>(owner), Where(h.tick.key).c_str(),
                          static_cast<double>(mine->second), static_cast<double>(want),
                          static_cast<double>(drift));
            out.notes.emplace_back(text);
        }
        out.writes.push_back({d->address, d->hash, h.tick.key, want, mine->second});
        s.written = want;
        s.last_write_us = steady_us;
        ++writes_;
    }
    if (!message.ticks.empty()) {
        out.send.emplace_back(std::move(message));
    }
}

void PlatformSync::Reset() {
    scene_.clear();
    heard_.clear();
    states_.clear();
    told_own_logic_.clear();
}

}  // namespace CoopPlatforms
