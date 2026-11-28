// SPDX-License-Identifier: GPL-3.0-or-later
//
// PvP on real bodies, the hook's side (coop_pvp_native_game.h).

#include "coop_pvp_native_game.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <utility>
#include <variant>
#include <vector>

#include "common/logging.h"
#include "coop/host/event_owner.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/presence/presence.hpp"
#include "coop_dbno_game.h"
#include "coop_objects.h"
#include "coop_puppet.h"
#include "coop_pvp_game.h"

namespace CoopPvpNativeGame {
namespace {

namespace proto = coop::protocol;
using CoopPvpNative::PlaceRing;

constexpr std::uint64_t kNativeEveryUs = 5'000'000;  // PVP_NATIVE again (docs/protocol.md)
constexpr std::size_t kQueued = 32;                  // strikes, results and copy hits waiting for a frame

std::uint64_t SteadyUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

template <typename T>
void Queue(std::deque<T>& q, const T& v) {
    if (q.size() < kQueued) q.push_back(v);
}

struct Glue {
    std::mutex mutex;
    // Under mutex (network thread, the walk, the present thread).
    bool say_now = true;            // a new connection: say PVP_NATIVE at once
    bool reset = true;              // a new connection: rings, seq and mercy start again
    std::uint32_t native_peers = 0; // bit n: player n said PVP_NATIVE 1
    std::uint32_t forget = 0;       // bit n: player n left or joined: its seq starts again (review LOW 1)
    std::deque<proto::PvpStrike> strikes;
    std::deque<proto::PvpResult> results;
    std::deque<CoopPvpNative::CopyHit> copy_hits;
    std::atomic<bool> pack_pvp{false};
    // Present thread only.
    std::uint64_t say_at_us = 0;
    std::uint32_t wants_logged = 0;
    std::uint32_t seq = 0;  // our strikes' count; the victim's SeqGate forgets us when we leave
    PlaceRing own;
    std::array<PlaceRing, proto::kMaxPlayers + 1> copies;
    std::array<std::uint64_t, proto::kMaxPlayers + 1> copy_pushed_us{};
    CoopPvpNative::GuardRing guard;     // our Link's guard toward a place, each frame (a shield raised just before)
    std::optional<std::string> own_scene;  // the rings are one scene's: a load starts them again
    std::array<std::optional<std::string>, proto::kMaxPlayers + 1> copy_scene;
    // The sequence of the sample that set a copy's scene: older samples are the last scene's (SampleOfScene).
    std::array<std::optional<std::uint32_t>, proto::kMaxPlayers + 1> copy_from_seq;
    std::array<CoopPvpNative::FallWatch, proto::kMaxPlayers + 1> falls;  // since when a player is fallen (a trade)
    CoopPvpNative::SeqGate seq_gate;
    CoopPvpNative::Mercy mercy;
    CoopPvpNative::Tables tables = CoopPvpNative::FirstTables();
    std::set<std::string> guard_names;  // U5: each main AS name logged once while a strike is judged
    std::uint64_t not_ours = 0;         // copy hits not ours (an enemy's, ...): counted, logged on powers of 2
};

Glue& TheGlue() {
    static Glue g;
    return g;
}

std::uint32_t Bit(std::uint8_t number) {
    return number >= 1 && number <= proto::kMaxPlayers ? 1u << number : 0u;
}

// Another player as presence shows it this frame: the pose our copy draws and its sample's server time.
struct Shown {
    bool known = false;
    std::uint64_t at_us = 0;
    proto::Vec3 position{};
    std::optional<std::string> scene;
    std::uint8_t status = 0;
    std::optional<std::string> weapon;  // PLAYER_EQUIP's weapon (the puppet's gear)
};

} // namespace

bool On() {
    static const bool on = [] {
        const char* v = std::getenv("LICHII_COOP_PVP_NATIVE");
        return v != nullptr && std::strcmp(v, "1") == 0;
    }();
    return on;
}

void SetPackPvp(bool on) {
    TheGlue().pack_pvp.store(on, std::memory_order_relaxed);
    if (On()) {
        LOG_INFO(Core, "coop: pvp native: on; the puppet pack {} _Pvp copies{}", on ? "has" : "has no",
                 on ? "" : " (rebuild it with --puppet-pvp): today's PvP is kept");
    }
}

bool Wants(std::uint8_t number) {
    return On() && (CoopPuppet::PvpCopies() & Bit(number)) != 0;
}

bool TakesStrikesFrom(std::uint8_t number) {
    // The tables are fixed at start (FirstTables), so reading them from any thread is safe.
    return Wants(number) && CoopPvpNative::PathBLive(TheGlue().tables);
}

void NoteCopyHit(const CoopPvpNative::CopyHit& hit) {
    if (!On()) {
        return;
    }
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    Queue(g.copy_hits, hit);
}

void OnEvent(const coop::net::Event& event, coop::net::Client& /*client*/) {
    if (!On()) {
        return;
    }
    using coop::net::EventKind;
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    if (event.kind == EventKind::welcomed || event.kind == EventKind::reconnected ||
        event.kind == EventKind::disconnected) {
        g.native_peers = 0;
        g.say_now = true;
        g.reset = true;  // review LOW 1: the seq gate (and the rings, mercy) start again on a new connection
        g.strikes.clear();
        g.results.clear();
        g.copy_hits.clear();
        return;
    }
    if (event.kind != EventKind::message) {
        return;
    }
    if (const auto* n = std::get_if<proto::PvpNative>(&event.message)) {
        g.native_peers = n->on ? g.native_peers | Bit(n->player_number) : g.native_peers & ~Bit(n->player_number);
    } else if (const auto* left = std::get_if<proto::PlayerLeft>(&event.message)) {
        // A player who comes back says PVP_NATIVE again; until then its copy is the plain one. Its game counts
        // its strikes from 1 again (review LOW 1).
        g.native_peers &= ~Bit(left->player_number);
        g.forget |= Bit(left->player_number);
    } else if (const auto* joined = std::get_if<proto::PlayerJoined>(&event.message)) {
        g.forget |= Bit(joined->player_number);
    } else if (const auto* s = std::get_if<proto::PvpStrike>(&event.message)) {
        Queue(g.strikes, *s);
    } else if (const auto* r = std::get_if<proto::PvpResult>(&event.message)) {
        Queue(g.results, *r);
    }
}

void Frame(const std::optional<std::string>& scene, const std::optional<coop::host::PlayerReading>& reading,
           coop::host::Session* session, std::uint64_t server_now_us) {
    if (!On()) {
        return;
    }
    Glue& g = TheGlue();
    const bool pack_pvp = g.pack_pvp.load(std::memory_order_relaxed);
    if (session == nullptr) {
        CoopPuppet::SetPvpCopies(0);
        return;
    }
    std::uint32_t peers = 0;
    std::uint32_t forget = 0;
    bool say_now = false;
    bool reset = false;
    std::deque<proto::PvpStrike> strikes;
    std::deque<proto::PvpResult> results;
    std::deque<CoopPvpNative::CopyHit> copy_hits;
    {
        std::lock_guard lock(g.mutex);
        peers = g.native_peers;
        say_now = std::exchange(g.say_now, false);
        reset = std::exchange(g.reset, false);
        forget = std::exchange(g.forget, 0u);
        strikes.swap(g.strikes);
        results.swap(g.results);
        copy_hits.swap(g.copy_hits);
    }
    if (reset) {
        g.seq_gate.Reset();
        g.seq = 0;  // the victims forget us when we leave or join again: we count from 1 again
        g.mercy.Reset();
        g.own.Clear();
        g.guard.Clear();
        for (auto& ring : g.copies) ring.Clear();
        g.copy_pushed_us.fill(0);
        g.copy_scene.fill(std::nullopt);
        g.copy_from_seq.fill(std::nullopt);
        g.falls.fill({});
    }
    for (std::uint8_t n = 1; n <= proto::kMaxPlayers; ++n) {
        if ((forget & Bit(n)) != 0) {
            g.seq_gate.Forget(n);
            g.copies[n].Clear();
            g.copy_pushed_us[n] = 0;
            // A new player on this number starts with nothing of the last one: no scene, no fall.
            g.copy_scene[n].reset();
            g.copy_from_seq[n].reset();
            g.falls[n] = {};
        }
    }

    // Which copies are the _Pvp actor: PvP on (the session's value), our glue and pack, their PVP_NATIVE, and
    // the player up (a downed, out or spectating player is no target: docs/native-puppets.md 5.1).
    const bool pvp_on = CoopPvp::On();
    std::uint32_t wants = 0;
    for (std::uint8_t n = 1; n <= proto::kMaxPlayers; ++n) {
        const bool up = CoopDbno::StateOfPlayer(n) == CoopDbno::State::up;
        const CoopPvpNative::CopyMode mode{pvp_on, true, pack_pvp, (peers & Bit(n)) != 0, up};
        if (CoopPvpNative::WantedCopy(mode) == CoopPvpNative::CopyActor::pvp) wants |= Bit(n);
    }
    CoopPuppet::SetPvpCopies(wants);  // the director makes those copies the _Pvp actor
    if (wants != g.wants_logged) {
        LOG_INFO(Core, "coop: pvp native: _Pvp copies for players {:#x} (PvP {})", wants >> 1, pvp_on ? "on" : "off");
        g.wants_logged = wants;
    }

    // Who we are, and every other player as our copies show them this frame (5.6: the rings).
    std::uint8_t self = 0;
    std::uint8_t status = 0;
    session->with_client([&](coop::net::Client& client) {
        self = client.player_number();
        if (const auto* me = client.find_player(self)) {
            status = me->status.value_or(0);
        }
    });
    std::array<Shown, proto::kMaxPlayers + 1> shown{};
    if (server_now_us != 0) {
        session->with_presence([&](coop::presence::Presence& p) {
            for (const std::uint8_t n : p.players()) {
                const auto pose = n <= proto::kMaxPlayers ? p.shown(n, server_now_us) : std::nullopt;
                if (!pose) {
                    continue;
                }
                Shown& s = shown[n];
                s.known = true;
                s.at_us = pose->at_us;
                s.position = pose->position;
                if (const auto* player = p.player(n)) {
                    s.scene = player->scene();
                    s.status = player->flags();
                    g.falls[n].See(s.status, server_now_us);
                    // The attacker's ring: every sample we RECEIVED, also those not drawn yet (the copy is
                    // drawn a show delay late; a strike's `from` is the attacker's place when it hit, which
                    // on a short trip is newer than what our copy shows). A new scene starts the ring again:
                    // a place of the last scene is no place of this one (a room change mid-swing). The
                    // last scene's samples that came in this same tick are still in the queue and newer
                    // than what we pushed: only samples from the one that set the scene on are taken.
                    if (s.scene != g.copy_scene[n]) {
                        g.copy_scene[n] = s.scene;
                        g.copy_from_seq[n] = player->scene_sequence();
                        g.copies[n].Clear();
                    }
                    for (const auto& sample : player->samples()) {
                        const std::uint64_t t = sample.time.server_time_us;
                        if (!CoopPvpNative::SampleOfScene(sample.time.sequence, g.copy_from_seq[n])) continue;
                        if (t != 0 && t > g.copy_pushed_us[n]) {
                            g.copies[n].Push(t, sample.position);
                            g.copy_pushed_us[n] = t;
                        }
                    }
                }
            }
        });
        if (scene != g.own_scene) {
            g.own_scene = scene;
            g.own.Clear();
            g.guard.Clear();
        }
        if (reading) {
            g.own.Push(server_now_us, reading->position);
            g.guard.Push(server_now_us, reading->animation && CoopPvpNative::GuardAnim(reading->animation->name),
                         reading->rotation, reading->position);
        }
    }
    // The attacker's weapon as PLAYER_EQUIP names it (path B's BaseAttack).
    const auto weapon_of = [&](std::uint8_t number) -> std::uint32_t {
        std::uint32_t id = CoopPvpNative::kWeaponNone;
        session->with_client([&](coop::net::Client& client) {
            if (const auto* p = client.find_player(number); p != nullptr && p->equipment && p->equipment->weapon) {
                id = CoopPvpNative::WeaponOf(*p->equipment->weapon);
            }
        });
        return id;
    };

    std::vector<proto::Message> send;

    // 1. Ours hitting them: our game's own hit on a _Pvp copy (the walk read its record) is a strike.
    for (const auto& hit : copy_hits) {
        const Shown& v = shown[hit.victim <= proto::kMaxPlayers ? hit.victim : 0];
        if (!pvp_on || (wants & Bit(hit.victim)) == 0) {
            continue;  // the mode changed under it: the server would drop it too
        }
        if (hit.own_shot) {
            LOG_INFO(Core, "coop: pvp native: our shot hit P{}'s copy: not sent (the shot's made object is not "
                           "named yet; today's PvP gives shots)", hit.victim);
            continue;
        }
        const auto strike = CoopPvpNative::MakeStrike(hit, self, g.seq + 1, v.at_us, v.position);
        if (!strike) {
            const auto n = ++g.not_ours;
            if ((n & (n - 1)) == 0) {
                LOG_INFO(Core, "coop: pvp native: a hit on P{}'s copy is not ours ({} so far): not sent", hit.victim,
                         g.not_ours);
            }
            continue;
        }
        g.seq = strike->seq;
        LOG_INFO(Core, "coop: pvp native: strike {} on P{} (source {}, our game counted {}) seen at {}; U7: our main AS "
                       "{}, weapon {}", strike->seq, strike->victim, static_cast<int>(strike->source), strike->amount,
                 strike->seen_us, reading && reading->animation ? reading->animation->name : std::string("?"),
                 weapon_of(self));
        send.emplace_back(*strike);
    }

    // 2. Theirs hitting us: path B, our game decides (5.6).
    if (!strikes.empty()) {
        CoopPvp::Local local;
        local.self = self;
        local.scene = scene;
        local.status = status;
        local.event = reading && coop::host::our_link_in_event(reading->event_running.value_or(false),
                                                               reading->event_whose);
        local.life = CoopObjects::OurLinkLife();
        if (!reading) {
            local.status |= proto::status::loading;  // no Link to read: a load
        }
        const auto dbno = CoopDbno::Shown();
        const bool downed = dbno && dbno->kind == CoopDbno::View::Kind::downed;
        std::optional<std::string_view> anim;
        if (reading && reading->animation) {
            anim = reading->animation->name;
        }
        const std::uint64_t now_us = server_now_us;
        for (const auto& s : strikes) {
            const std::uint8_t a = s.player_number <= proto::kMaxPlayers ? s.player_number : 0;
            CoopPvpNative::StrikeView view;
            view.local = local;
            view.attacker = CoopPvp::Hitter{shown[a].scene, shown[a].status, a != 0 && shown[a].known};
            view.downed = downed;
            view.own = &g.own;
            view.attacker_ring = a != 0 ? &g.copies[a] : nullptr;
            view.attacker_fell_us = g.falls[a].Since();
            // weapon_range: WeaponParam.Range is not read yet (the 1.3 m floor); object_at: shots are not sent yet.
            CoopPvpNative::PathBInput in;
            in.weapon = weapon_of(a);
            const proto::Vec3 at_self = reading ? reading->position : proto::Vec3{};
            // Now, or when the attacker's copy of us showed it (5.6: a shield raised or lowered just before).
            in.guarding = CoopPvpNative::GuardsStrike(
                reading && CoopPvpNative::Guarding(anim, reading->rotation, at_self, s.from), g.guard, s);
            if (anim && g.guard_names.size() < 64 && g.guard_names.insert(std::string(*anim)).second) {
                LOG_INFO(Core, "coop: pvp native: U5: our Link's main AS at a strike: {} (guard {})", *anim,
                         CoopPvpNative::GuardAnim(*anim) ? "yes" : "no");
            }
            // The U5 probe of the game's own guard bits (reports/fable-puppet-pvp-2.md step 2), every strike (at most
            // 256 lines a run): measured against the AS name above before Guarding uses them.
            static std::uint32_t guard_bit_lines = 0;
            if (guard_bit_lines < 256) {
                ++guard_bit_lines;
                if (const auto w = CoopObjects::OurLinkGuardWords()) {
                    const auto part = static_cast<std::uint32_t>(*w & 0xff);
                    const auto eu = static_cast<std::uint32_t>((*w >> 8) & 0xffffffffu);
                    const bool as_guard = anim && CoopPvpNative::GuardAnim(*anim);
                    LOG_INFO(Core, "coop: pvp native: U5 bits at P{}'s strike {}: player +0x32 {:#04x} (bit 4 {}), "
                                   "EquipmentUser +0x13a0 {:#010x} (bit 5 {}), AS guard {}", a, s.seq, part,
                             (part >> 4) & 1u, eu, (eu >> 5) & 1u, as_guard ? "yes" : "no");
                } else {
                    LOG_INFO(Core, "coop: pvp native: U5 bits at P{}'s strike {}: not read", a, s.seq);
                }
            }
            const auto d = CoopPvpNative::DecidePathB(s, pvp_on, view, in, g.tables, g.seq_gate, g.mercy, now_us);
            if (!d) {
                continue;  // not ours, a repeated seq, PvP off: no answer
            }
            if (d->replay) {
                CoopObjects::HurtOurLinkNative(d->take, s.at, s.from, s.kind);
                local.life = d->result.life;
            }
            LOG_INFO(Core, "coop: pvp native: P{}'s strike {}: outcome {}, take {} (attacker said {}), life {}", a,
                     s.seq, static_cast<int>(d->result.outcome), d->take, s.amount, d->result.life);
            send.emplace_back(d->result);
            if (d->replay && d->take > 0) {
                // The chat line (PVP_HURT 243, docs 5.7).
                proto::PvpHurt hurt;
                hurt.attacker = a;
                hurt.amount = d->take;
                hurt.life = d->result.life;
                hurt.flags = d->result.life <= 0 ? proto::pvp_hurt::died : 0;
                send.emplace_back(hurt);
            }
        }
    }

    // 3. Results about others: the third game replays a landed kind on its copy of the victim; the attacker
    //    notes a guard (U6: its own game may have clanged on the copy's shield already).
    for (const auto& r : results) {
        switch (CoopPvpNative::PlayResult(r, self)) {
        case CoopPvpNative::ResultPlay::replay_on_copy:
            if (const std::uint64_t actor = CoopPuppet::TheDirector().LivePvpActor(r.player_number)) {
                CoopObjects::ReplayOnCopy(actor, r.amount, r.at, r.kind);
                LOG_INFO(Core, "coop: pvp native: P{} hit P{} (outcome {}): replayed on our copy", r.attacker,
                         r.player_number, static_cast<int>(r.outcome));
            }
            break;
        case CoopPvpNative::ResultPlay::attacker_recoil:
            LOG_INFO(Core, "coop: pvp native: P{} guarded our strike {} (U6: the recoil is the game's own clang "
                           "on the copy's shield, if it played)", r.player_number, r.seq);
            break;
        case CoopPvpNative::ResultPlay::none:
            if (r.attacker == self) {
                LOG_INFO(Core, "coop: pvp native: P{} answered our strike {}: outcome {}, took {}, life {}",
                         r.player_number, r.seq, static_cast<int>(r.outcome), r.amount, r.life);
            }
            break;
        }
    }

    const std::uint64_t now_us = SteadyUs();
    if (say_now || now_us - g.say_at_us >= kNativeEveryUs) {
        g.say_at_us = now_us;
        send.emplace_back(proto::PvpNative{proto::kPlayerSelf, CoopPvpNative::SayNative(true, pack_pvp)});
    }
    if (send.empty()) {
        return;
    }
    session->with_client([&](coop::net::Client& client) {
        for (const auto& m : send) {
            client.send(m);
        }
    });
}

} // namespace CoopPvpNativeGame
