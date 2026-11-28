// SPDX-License-Identifier: GPL-3.0-or-later
//
// Native puppets phase 3: lifting another player, the hook's side (coop_lift_player.h has the rules).
//
// Lifter: the director keeps where the game had each copy (CoopPuppet::WatchGamePlaces); Lifter sees the game's
// own lift on a copy and asks; from then on the director leaves that copy to the game (CoopPuppet::SetGameHolds)
// and its places go to the lifted game. Lifted: Victim decides; while lifted our Link is put on the lifter's
// places by the object walk with the game's own setter (CoopObjects::PlaceOurLink) and its input check answers
// "off" (OurInputOff, the input-off wrap).

#include "coop_lift_player_game.h"

#include "coop/protocol/scene_place.hpp"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "common/logging.h"
#include "coop/host/event_owner.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_lift_player.h"
#include "coop_objects.h"
#include "coop_puppet.h"

namespace CoopLiftPlayerGame {
namespace {

namespace proto = coop::protocol;
using namespace CoopLiftPlayer;

constexpr std::uint64_t kShareEveryUs = 5'000'000;

std::uint64_t SteadyUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

const char* RefusalText(proto::LiftRefusal r) {
    switch (r) {
    case proto::LiftRefusal::none: return "yes";
    case proto::LiftRefusal::busy: return "busy (lifted already, or lifting)";
    case proto::LiftRefusal::event: return "in an event, a load or a menu";
    case proto::LiftRefusal::down: return "down";
    case proto::LiftRefusal::not_sharing: return "their game does not share lifts";
    case proto::LiftRefusal::freed: return "freed";
    case proto::LiftRefusal::too_far: return "too far";
    case proto::LiftRefusal::not_young: return "not a young Link";
    }
    return "?";
}

const char* PhaseText(proto::LiftPhase p) {
    switch (p) {
    case proto::LiftPhase::held: return "held";
    case proto::LiftPhase::flying: return "flying";
    case proto::LiftPhase::landed: return "landed";
    }
    return "?";
}

struct Glue {
    std::mutex mutex;
    std::deque<proto::LiftAsk> asks;        // network thread -> present thread
    std::deque<proto::LiftAnswer> answers;
    std::deque<proto::LiftPlace> places;
    bool share_now = true;                  // a new connection: say LIFT_SHARE at once
    // Our Link's input is off while this stamp (SteadyUs, 0 none) is fresh (InputOffFresh): the frame renews it
    // each frame while lifted, so a frame that stops being called gives the input back by itself.
    std::atomic<std::uint64_t> input_off_us{0};
    // Our Link is lifted (Victim::Carried): Read() gives its samples the carried pose (CarriedPose).
    std::atomic<bool> carried{false};
    // Break free: the pad reads' struggle bits (NotePad, the pad thread) and the new presses not yet taken.
    std::atomic<std::uint64_t> pad_bits{0};
    std::atomic<int> presses{0};
    Lifter lifter;                          // present thread
    Victim victim;                          // present thread
    std::uint64_t share_at_us = 0;          // present thread
    proto::LiftPhase last_phase = proto::LiftPhase::landed;
};

Glue& TheGlue() {
    static Glue g;
    return g;
}

} // namespace

bool On() {
    // Row 317 (0.12.1): on by default (lifting a young player as Ruto); LICHII_COOP_LIFT_PLAYER=0 turns it off.
    // Solo and co-op idle: no session, so Frame only resets (nothing is watched, asked or sent).
    static const bool on = [] {
        const char* v = std::getenv("LICHII_COOP_LIFT_PLAYER");
        return v == nullptr || std::strcmp(v, "0") != 0;
    }();
    return on;
}

void CarriedPose(std::optional<coop::host::PlayerReading>& reading) {
    if (!On() || !reading) {
        return;
    }
    // The probe for our own screen (docs/native-puppets.md, UNKNOWN 7): what our own Link plays while carried,
    // logged once per change (its own action AI picks it; the copies play the carried pose).
    static std::string own_logged;
    if (!TheGlue().carried.load(std::memory_order_relaxed)) {
        own_logged.clear();
        return;
    }
    const std::string own = reading->animation ? reading->animation->name : std::string("(not read)");
    if (own != own_logged) {
        LOG_INFO(Core, "coop: lift: carried: our own Link plays '{}' (its copies play '{}')", own, kCarriedPose);
        own_logged = own;
    }
    reading->animation = CarriedAnimation();
}

bool Carried() {
    return On() && TheGlue().carried.load(std::memory_order_relaxed);
}

bool OurInputOff() {
    return On() && InputOffFresh(TheGlue().input_off_us.load(std::memory_order_relaxed), SteadyUs());
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
        g.asks.clear();
        g.answers.clear();
        g.places.clear();
        g.share_now = true;
        return;
    }
    if (event.kind != EventKind::message) {
        return;
    }
    if (const auto* a = std::get_if<proto::LiftAsk>(&event.message)) {
        if (g.asks.size() < 8) g.asks.push_back(*a);
    } else if (const auto* b = std::get_if<proto::LiftAnswer>(&event.message)) {
        if (g.answers.size() < 8) g.answers.push_back(*b);
    } else if (const auto* p = std::get_if<proto::LiftPlace>(&event.message)) {
        if (g.places.size() < 64) g.places.push_back(*p);
    }
}

void Frame(const std::optional<std::string>& scene, const std::optional<coop::host::PlayerReading>& reading,
           coop::host::Session* session, std::uint64_t server_now_us) {
    if (!On()) {
        return;
    }
    Glue& g = TheGlue();
    // (only while connected: solo and co-op idle keep the director's place watch off)
    CoopPuppet::WatchGamePlaces(session != nullptr);
    std::deque<proto::LiftAsk> asks;
    std::deque<proto::LiftAnswer> answers;
    std::deque<proto::LiftPlace> places;
    bool share_now = false;
    {
        std::lock_guard lock(g.mutex);
        asks.swap(g.asks);
        answers.swap(g.answers);
        places.swap(g.places);
        share_now = std::exchange(g.share_now, false);
    }
    if (session == nullptr) {
        if (g.lifter.Held() != 0 || g.victim.By() != 0) {
            LOG_INFO(Core, "coop: lift: not connected: the lift ends");
        }
        g.lifter.Reset();
        g.victim.Reset();
        CoopPuppet::SetGameHolds(0);
        g.input_off_us.store(0);
        g.carried.store(false);
        return;
    }
    const std::uint64_t now_us = SteadyUs();

    std::uint8_t self = 0;
    std::uint8_t status = 0;
    session->with_client([&](coop::net::Client& client) {
        self = client.player_number();
        if (const auto* me = client.find_player(self)) {
            status = me->status.value_or(0);
        }
    });

    std::vector<proto::Message> send;
    if (share_now || now_us - g.share_at_us >= kShareEveryUs) {
        g.share_at_us = now_us;
        send.emplace_back(proto::LiftShare{true});
    }

    // Lifter: our Link and the copies the director drew.
    for (const auto& a : answers) {
        const std::uint8_t was = g.lifter.Held();
        g.lifter.Answer(a, now_us);
        if (was == a.player_number) {
            LOG_INFO(Core, "coop: lift: P{} answered our lift: {}", a.player_number, RefusalText(a.refusal));
        }
    }
    // Each remote player's presence (age, scene, place): only a young Link's copy is liftable, re-read every frame
    // (the age can change, as at the Master Sword).
    std::vector<CoopPuppet::Remote> remotes;
    bool have_remotes = false;
    auto Remotes = [&]() -> const std::vector<CoopPuppet::Remote>& {
        if (!have_remotes) {
            remotes = CoopPuppet::Collect(session, server_now_us);
            have_remotes = true;
        }
        return remotes;
    };
    {
        std::vector<CopyView> copies;
        if (reading && scene) {
            for (const auto& p : CoopPuppet::GamePlaces()) {
                bool young = false;
                for (const auto& r : Remotes()) {
                    if (r.number == p.number) young = r.young;
                }
                copies.push_back(CopyView{p.number, p.drawn, p.game, young});
            }
        }
        const Vec3 our_link = reading ? Vec3{reading->position[0], reading->position[1], reading->position[2]}
                                      : Vec3{};
        const std::optional<std::string_view> our_animation =
            reading && reading->animation ? std::optional<std::string_view>(reading->animation->name) : std::nullopt;
        g.lifter.SetOurLinkLifts(OurLinkLifts(our_animation));
        const auto out = g.lifter.Frame(our_link, copies, now_us);
        if (out.rose_alone != 0) {
            // The probe for the lift animation's names (docs/native-puppets.md): what our Link played then.
            LOG_INFO(Core, "coop: lift: P{}'s copy rose but our Link's animation '{}' is no lift: not asked",
                     out.rose_alone, our_animation ? std::string(*our_animation) : std::string("(not read)"));
        }
        if (out.ask) {
            LOG_INFO(Core, "coop: lift: our Link lifted P{}'s copy: asking", out.ask->lifted);
            send.emplace_back(*out.ask);
        }
        if (out.place) {
            if (out.place->phase != g.last_phase) {
                LOG_INFO(Core, "coop: lift: P{} {} at {:.2f} {:.2f} {:.2f}", out.place->lifted,
                         PhaseText(out.place->phase), out.place->position[0], out.place->position[1],
                         out.place->position[2]);
                g.last_phase = out.place->phase;
            }
            send.emplace_back(*out.place);
        }
        if (CoopPuppet::GameHolds() != g.lifter.Held()) {
            LOG_INFO(Core, "coop: lift: the director {} P{}'s copy",
                     g.lifter.Held() != 0 ? "leaves to the game" : "places again",
                     g.lifter.Held() != 0 ? g.lifter.Held() : CoopPuppet::GameHolds());
            CoopPuppet::SetGameHolds(g.lifter.Held());
        }
    }

    // Lifted: our game decides.
    Local local;
    local.self = self;
    local.event = (reading && coop::host::our_link_in_event(reading->event_running.value_or(false),
                                                             reading->event_whose)) ||
                  (status & (proto::status::away & ~proto::status::dead)) != 0;
    const auto life = CoopObjects::OurLinkLife();
    local.down = (status & proto::status::dead) != 0 || (life && *life <= 0);
    local.lifting = g.lifter.Held() != 0;
    local.young = reading && reading->age == proto::Age::child;
    if (reading) {
        local.our_link = Vec3{reading->position[0], reading->position[1], reading->position[2]};
    }
    if (!asks.empty()) {
        for (const auto& a : asks) {
            std::optional<Vec3> lifter_at;
            for (const auto& r : Remotes()) {
                // Either era of one place: both games draw the other's copy there (row 221).
                if (scene && r.number == a.player_number && r.scene && coop::protocol::same_place(*r.scene, *scene)) {
                    lifter_at = Vec3{r.position[0], r.position[1], r.position[2]};
                }
            }
            const auto answer = g.victim.Ask(a, local, lifter_at, now_us);
            LOG_INFO(Core, "coop: lift: P{} lifts our Link: {}", a.player_number, RefusalText(answer.refusal));
            send.emplace_back(answer);
        }
    }
    for (const auto& p : places) {
        g.victim.Place(p, now_us);
    }
    const std::uint8_t by = g.victim.By();
    g.victim.Struggle(g.presses.exchange(0), now_us);
    const auto step = g.victim.Frame(local, now_us);
    if (step.put) {
        CoopObjects::PlaceOurLink(*step.put);
    }
    const bool was_off = g.input_off_us.exchange(step.input_off ? now_us : 0) != 0;
    if (was_off != step.input_off) {
        LOG_INFO(Core, "coop: lift: our Link's input {} (held by P{})", step.input_off ? "off" : "back", by);
    }
    g.carried.store(g.victim.Carried());
    if (step.freed) {
        LOG_INFO(Core, "coop: lift: our Link freed itself from P{}{}", step.freed->lifter,
                 step.freed->refusal == proto::LiftRefusal::not_young ? " (not a young Link now)"
                 : step.struggled                                     ? " (struggled free)"
                                                                      : "");
        send.emplace_back(*step.freed);
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

void NotePad(std::uint64_t struggle_bits) {
    Glue& g = TheGlue();
    const std::uint64_t before = g.pad_bits.exchange(struggle_bits, std::memory_order_relaxed);
    if (g.carried.load(std::memory_order_relaxed)) {
        g.presses.fetch_add(NewPresses(before, struggle_bits), std::memory_order_relaxed);
    }
}

} // namespace CoopLiftPlayerGame
