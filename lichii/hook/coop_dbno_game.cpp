// SPDX-License-Identifier: GPL-3.0-or-later
//
// Down but not out, the hook's side (coop_dbno_game.h). The rules are in
// coop_dbno.cpp; this file hands them the game and the network:
// - the network thread keeps the session's rules, the others' states and the
//   revives that reach us;
// - the frame runs the Machine (our state) and the Reviver (theirs), sends
//   what they say, and leaves the guard, the life to write and the pad's
//   mode for the game's threads;
// - the Life step (Hookshot patch 0007) asks the guard for our Link only.

#include "coop_dbno_game.h"

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>
#include <variant>

#include "common/logging.h"
#include "coop/host/event_owner.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_actor_events.h"
#include "coop_dbno_body.h"
#include "coop_objects.h"
#include "coop_puppet.h"
#include "coop_fairy.h"
#include "coop_pvp_game.h"
#include "coop_rooms.h"
#include "coop_teleport.h"
#include "coop_teleport_game.h"

namespace CoopDbno {
namespace {

std::uint64_t SteadyUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// What the pad hook does with the pad (set by the frame).
enum class PadMode : std::uint8_t { game, downed, reviver, spectating };

const char* StateName(State s) {
    switch (s) {
    case State::up:
        return "up";
    case State::downed:
        return "down";
    case State::out:
        return "out";
    case State::spectating:
        return "spectating";
    }
    return "?";
}

struct Glue {
    std::mutex mutex;  // everything below that is not atomic
    Rules rules;       // the server's DBNO_MODE; off until it says
    bool heard = false;  // this connection has had its DBNO_MODE
    Roster roster;     // the others' DBNO_STATE
    struct HoldIn {
        std::uint8_t from;
        bool holding;
    };
    std::vector<HoldIn> holds;        // network thread -> frame
    struct ThreatIn {
        bool enemy_near = false;
        std::uint64_t at_us = 0;
    };
    std::array<std::optional<ThreatIn>, proto::kMaxPlayers + 1> threats{};  // their DBNO_THREAT
    std::optional<Vec3> spectate_offset;  // frame -> the camera
    std::vector<std::string> lines;   // any thread -> chat
    std::optional<View> view;         // frame -> the screen
    coop::host::Session* session = nullptr;  // the last frame's, for Change

    std::atomic<bool> tell_again{false};
    // The frame's own (no lock): only the frame touches them.
    Machine machine;
    // The machine's state and our player number, for other threads (StateOfPlayer).
    std::atomic<std::uint8_t> own_number{0};
    std::atomic<std::uint8_t> own_state{0};
    Reviver reviver;
    ThreatTeller threat_teller;
    bool was_button = false;  // A last frame (the spectator's switch)
    bool move_refused = false;  // the respawn's move could not even be asked

    // The game's threads.
    LifeGuard guard;
    std::atomic<std::uint64_t> life_component{0};
    std::atomic<std::uint64_t> bsa{0};      // our Link's BSA (the objects walk)
    std::atomic<std::uint64_t> emitter{0};  // our Link's PerceptionEmitter (the objects walk)
    std::atomic<bool> adult{false};         // our Link is Player_Adult (the objects walk)
    std::atomic<std::uint64_t> actor{0};    // our Link (the objects walk; the adult's lying pose)
    std::atomic<std::int32_t> life_write{-1};  // raise to this (-1: none); the objects walk writes it
    Lowering take_to;                          // lower to this; the Life step takes it
    std::atomic<PadMode> pad_mode{PadMode::game};
    std::atomic<bool> button{false};  // A as the player holds it (before we take it)
    std::atomic<bool> enemies_near{false};  // the objects walk
    std::atomic<bool> bottled_fairy{false};  // the hook's frame (row 162)

    void Line(std::string text) {
        if (lines.size() < 32) {
            lines.push_back(std::move(text));
        }
    }
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

std::string NameOf(const coop::net::Client& client, std::uint8_t number) {
    if (number == 0) {
        return {};
    }
    if (const auto* p = client.find_player(number); p != nullptr && !p->nickname.empty()) {
        return p->nickname;
    }
    return "P" + std::to_string(number);
}

// Whether the game's Life step asks the module (Hookshot patch 0007): only
// then can a lethal hit be held back.
bool GuardLive() {
    return CoopActorEvents::LifeStepSeen().load(std::memory_order_relaxed);
}

}  // namespace

// --- The Host settings page and /dbno ---

Rules Current() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return g.rules;
}

bool Change(const proto::DbnoMode& want) {
    Glue& g = TheGlue();
    coop::host::Session* session = nullptr;
    {
        std::lock_guard lock(g.mutex);
        session = g.session;
    }
    if (session == nullptr) {
        return false;
    }
    proto::DbnoMode ask = want;
    ask.set_by = 0;
    return session->with_client([&](coop::net::Client& client) {
        return client.state() == coop::net::State::connected && client.is_host() && client.send(ask);
    });
}

bool OnChat(std::string_view text, coop::host::Session& session) {
    const auto command = Parse(text);
    if (!command) {
        return false;
    }
    Glue& g = TheGlue();
    Rules now;
    {
        std::lock_guard lock(g.mutex);
        now = g.rules;
        g.session = &session;
    }
    std::string line;
    session.with_client([&](coop::net::Client& client) {
        if (client.state() != coop::net::State::connected) {
            line = "dbno: not connected";
            return;
        }
        switch (command->kind) {
        case Command::Kind::show:
            line = RulesLine(now, client.player_number(), NameOf(client, now.set_by)) +
                   (client.is_host() ? ". /dbno on, off, time 60, hold 3, hearts 3 change it" : "");
            return;
        case Command::Kind::bad:
            line = "dbno: /dbno on, /dbno off, /dbno time 60, /dbno hold 3, /dbno hearts 3, or /dbno alone";
            return;
        case Command::Kind::change:
            if (!client.is_host()) {
                line = "dbno: only the host can change it";
                return;
            }
            if (!client.send(Applied(now, *command))) {
                line = "dbno: could not be sent";
            }
            return;  // the server's DBNO_MODE to everyone shows the change
        }
    });
    if (!line.empty()) {
        std::lock_guard lock(g.mutex);
        g.Line(std::move(line));
    }
    return true;
}

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    using coop::net::EventKind;
    Glue& g = TheGlue();
    switch (event.kind) {
    case EventKind::welcomed:
    case EventKind::reconnected:
    case EventKind::disconnected: {
        // Each connection starts with the mode off; the server says it at join.
        std::lock_guard lock(g.mutex);
        g.rules = Rules{};
        g.heard = false;
        g.roster.Clear();
        g.threats.fill(std::nullopt);
        g.holds.clear();
        if (event.kind != EventKind::disconnected) {
            g.tell_again.store(true);
        }
        return;
    }
    case EventKind::player_joined:
        g.tell_again.store(true);  // the newcomer learns that we are down
        return;
    case EventKind::player_left: {
        std::lock_guard lock(g.mutex);
        g.roster.Left(event.player);
        if (event.player <= proto::kMaxPlayers) {
            g.threats[event.player].reset();
        }
        return;
    }
    case EventKind::message:
        break;
    default:
        return;
    }
    const std::uint64_t now_us = SteadyUs();
    if (const auto* m = std::get_if<proto::DbnoMode>(&event.message)) {
        std::lock_guard lock(g.mutex);
        const Rules rules = RulesOf(*m);
        // The host changed it, or it is off: worth a line. On with the
        // server's values is the usual start, said by nobody.
        const bool news = rules != g.rules && (rules.set_by != 0 || !rules.on);
        g.rules = rules;
        g.heard = true;
        if (news) {
            g.Line(RulesLine(rules, client.player_number(), NameOf(client, rules.set_by)));
        }
        LOG_INFO(Core, "coop: dbno: {} (bleed {} s, hold {} tenths, {} quarter hearts; set by {})",
                 rules.on ? "on" : "off", rules.bleed_s, rules.revive_tenths, rules.revive_life, rules.set_by);
    } else if (const auto* s = std::get_if<proto::DbnoStatus>(&event.message)) {
        std::lock_guard lock(g.mutex);
        const auto before = g.roster.Downed(s->player_number);
        std::optional<proto::DbnoStatus> was;
        if (before) {
            was = before->status;
        } else if (g.roster.StateOf(s->player_number) != State::up) {
            was = proto::DbnoStatus{s->player_number, g.roster.StateOf(s->player_number)};
        }
        g.roster.Set(*s, now_us);
        const std::uint8_t reviver = was ? was->reviver : 0;
        if (auto line = TheirLine(was, *s, NameOf(client, s->player_number), NameOf(client, reviver))) {
            g.Line(std::move(*line));
        }
        if (!was || was->state != s->state) {
            LOG_INFO(Core, "coop: dbno: P{} is {}", s->player_number, StateName(s->state));
        }
    } else if (const auto* t = std::get_if<proto::DbnoThreat>(&event.message)) {
        std::lock_guard lock(g.mutex);
        if (t->player_number >= 1 && t->player_number <= proto::kMaxPlayers) {
            g.threats[t->player_number] = Glue::ThreatIn{t->enemy_near, now_us};
        }
    } else if (const auto* r = std::get_if<proto::DbnoRevive>(&event.message)) {
        std::lock_guard lock(g.mutex);
        if (g.holds.size() < 64) {
            g.holds.push_back({r->player_number, r->holding});
        }
    }
}

void Frame(const std::optional<std::string>& scene, const std::optional<coop::host::PlayerReading>& reading,
           coop::host::Session* session, std::uint64_t server_now_us) {
    Glue& g = TheGlue();
    const std::uint64_t now_us = SteadyUs();
    Rules rules;
    bool heard = false;
    std::vector<Glue::HoldIn> holds;
    std::vector<Fallen> fallen;
    {
        std::lock_guard lock(g.mutex);
        if (session != nullptr) {
            g.session = session;
        }
        rules = g.rules;
        heard = g.heard;
        holds.swap(g.holds);
    }

    bool connected = false;
    std::uint8_t self = 0;
    if (session != nullptr) {
        session->with_client([&](coop::net::Client& client) {
            connected = client.state() == coop::net::State::connected;
            self = client.player_number();
        });
    }

    std::vector<Teammate> team;
    std::vector<CoopPuppet::Remote> remotes;
    if (connected) {
        remotes = CoopPuppet::Collect(session, server_now_us);
    }
    {
        std::lock_guard lock(g.mutex);
        for (const auto& r : remotes) {
            if (r.number == self) {
                continue;
            }
            Teammate t;
            t.number = r.number;
            t.state = g.roster.StateOf(r.number);
            t.in_game = r.scene.has_value() && !r.scene->empty() && (r.status & proto::status::loading) == 0;
            t.loading = (r.status & proto::status::loading) != 0;
            t.status = r.status;
            t.scene = r.scene;
            t.position = r.position;
            if (const auto& th = g.threats[r.number <= proto::kMaxPlayers ? r.number : 0];
                th && now_us < th->at_us + kThreatStaleUs) {
                t.threat = th->enemy_near ? Threat::enemy : Threat::clear;
            }
            team.push_back(std::move(t));
            if (g.roster.Downed(r.number)) {
                fallen.push_back(Fallen{r.number, r.scene, r.position});
            }
        }
    }

    for (const auto& h : holds) {
        g.machine.Hold(h.from, h.holding, now_us);
    }
    if (g.tell_again.exchange(false)) {
        g.machine.TellAgain();
    }

    Input in;
    in.now_us = now_us;
    in.rules = rules;
    // Not until this connection has its DBNO_MODE: before it, a downed
    // Link neither gets up (mode looks off) nor counts as alone.
    in.connected = connected && heard;
    in.guard_live = GuardLive();
    in.lethal = g.guard.TakeLethal();
    in.life = reading ? CoopObjects::OurLinkLife() : std::nullopt;
    if (const auto hearts = CoopObjects::OurLife()) {
        in.max_life = hearts->max;
    }
    in.scene = scene;
    in.position = reading ? reading->position : Vec3{};
    in.team = team;
    // Spectating: A (as the player pressed it) watches the next player.
    const bool button = g.button.load();
    in.next = g.machine.Now() == State::spectating && button && !g.was_button;
    g.was_button = button;
    // The respawn's move (asked by an earlier frame): how it went.
    if (const auto moved = CoopTeleport::TakeRespawnMove()) {
        in.moved = *moved ? Moved::done : Moved::failed;
    } else if (std::exchange(g.move_refused, false)) {
        in.moved = Moved::failed;
    }
    in.enemy_near_us = g.enemies_near.load(std::memory_order_relaxed);
    in.bottled_fairy = reading && g.bottled_fairy.load(std::memory_order_relaxed);
    const Output out = g.machine.Frame(in);
    g.own_number.store(self, std::memory_order_relaxed);
    g.own_state.store(static_cast<std::uint8_t>(g.machine.Now()), std::memory_order_relaxed);
    g.guard.Set(out.guard);
    Body::Raising& raise = Body::TheRaising();
    if (out.write_life) {
        // Both ways through the game's own Life step: less as its damage (its
        // death at 0), more (a revive, a respawn) as a negative sum, its heal
        // (coop_dbno_body.h). The step not taken in time: written after all
        // (it skips frames).
        if (in.life && *out.write_life < *in.life) {
            raise.Drop();
            if (const auto late = g.take_to.Ask(*out.write_life, now_us)) {
                g.life_write.store(*late);
                LOG_INFO(Core, "coop: dbno: the Life step did not lower our Link in time: written");
            }
        } else if (in.life && GuardLive()) {
            g.take_to.Drop();
            raise.Start(*out.write_life, now_us);
            LOG_INFO(Core, "coop: dbno: our Link's life {} -> {}: asked through the game's Life step (its heal)",
                     *in.life, *out.write_life);
        } else {
            g.take_to.Drop();
            g.life_write.store(*out.write_life);  // no Life step (or no life read): written
        }
    } else if (out.state == State::up) {
        g.take_to.Drop();  // up again (a fairy, the title screen): nothing left to lower
    }
    if (const auto late = raise.Frame(in.life, now_us)) {
        g.life_write.store(*late);
        LOG_INFO(Core, "coop: dbno: the Life step did not raise our Link in time: written");
    }

    // Our downed Link's body (coop_dbno_body.h): crawl, shield and unseen while
    // down; an event of ours drives our Link meanwhile. G1 (coop/host/event_owner.hpp):
    // only an event our Link is in, not another player's replayed here nor our free Ocarina.
    Body::Crawl::Wanted body;
    // Lying while downed and while spectating (down-spectate): the body stays down until the respawn.
    body.lying = out.state == State::downed || out.state == State::spectating;
    body.event = reading && coop::host::our_link_in_event(reading->event_running.value_or(false),
                                                           reading->event_whose);
    body.emitter = g.emitter.load(std::memory_order_acquire);
    body.bsa = g.bsa.load(std::memory_order_acquire);
    body.adult = g.adult.load(std::memory_order_acquire);
    body.actor = g.actor.load(std::memory_order_acquire);
    body.life = g.life_component.load(std::memory_order_acquire);
    // A teammate who cannot be fought now (downed, spectating, or PLAYER_STATUS away: paused, in a
    // menu, loading, in their own event, dead): their puppet is unseen in our game too (issue 150 with
    // row 113; review of perception-on).
    // Row 178: and every puppet while a boss whose own AI fights the Player only is here
    // (CoopBossNative::Table::FightsPlayerOnly: Phantom Ganon held his phase 2 with a puppet as his target).
    const auto player_only = CoopBossNative::TheTable().PlayerOnlyBoss();
    for (const Teammate& t : team) {
        if (auto why = Body::UnseenWhy(t.state == State::downed || t.state == State::spectating, t.status)) {
            body.unseen_puppets.push_back({t.number, std::move(*why)});
        } else if (player_only) {
            body.unseen_puppets.push_back({t.number, "fighting " + *player_only + ", whose AI fights the Player only"});
        }
    }
    Body::TheCrawl().Want(body);
    for (auto& note : Body::TheCrawl().TakeNotes()) {
        LOG_INFO(Core, "coop: dbno: {}", note);
    }

    // Beside a downed teammate: hold A.
    // G1 (coop/host/event_owner.hpp): no revive while the game's event holds our Link (ours, or the
    // free Ocarina); another player's event replayed here leaves our Link free, so it may revive.
    const bool event = reading && coop::host::event_blocks_revive(reading->event_running.value_or(false),
                                                                  reading->event_whose);
    const bool able = connected && rules.on && out.state == State::up && reading.has_value() && !event;
    const Reviver::Out rev =
        g.reviver.Frame(now_us, able, g.button.load(), in.position, scene, fallen);
    g.pad_mode.store(out.state == State::downed       ? PadMode::downed
                     : out.state == State::spectating ? PadMode::spectating
                     : g.reviver.TakesButton()        ? PadMode::reviver
                                                      : PadMode::game);

    // Enemies near our Link, for those who spectate (told only while we are up).
    // With PvP on, another player up beside us is a threat too: a player
    // downed in a duel does not come back in the middle of it.
    bool threatened = in.enemy_near_us;
    if (!threatened && CoopPvp::On() && reading) {
        std::vector<Vec3> rivals;
        for (const Teammate& t : team) {
            if (t.state == State::up && t.in_game && t.scene && scene && *t.scene == *scene && t.position) {
                rivals.push_back(*t.position);
            }
        }
        threatened = AnyNear(in.position, rivals);
    }
    const auto threat = g.threat_teller.Frame(
        now_us, connected && heard && rules.on && out.state == State::up && reading.has_value(), threatened);

    // Spectating: the camera follows the one watched while the game can draw
    // where they are: our scene, and in a dungeon our room (rooms load one at
    // a time: coop_teleport.h CheckMove; the camera moves, Link does not, so his room stays the one loaded).
    std::optional<Vec3> offset;
    if (out.state == State::spectating && reading && scene) {
        static const CoopRooms::Table rooms = CoopFairy::RoomTable();
        for (const Teammate& t : team) {
            if (t.number == out.watching && t.scene && *t.scene == *scene && t.position &&
                CoopTeleport::CheckMove(rooms, *scene, CoopTeleport::Vec{in.position[0], in.position[1], in.position[2]},
                                        CoopTeleport::Vec{(*t.position)[0], (*t.position)[1], (*t.position)[2]}) ==
                    CoopTeleport::MoveCheck::ok) {
                offset = Vec3{(*t.position)[0] - in.position[0], (*t.position)[1] - in.position[1],
                              (*t.position)[2] - in.position[2]};
            }
        }
    }
    const bool watched_here = offset.has_value();

    std::vector<std::string> lines;
    std::optional<View> view;
    std::optional<std::pair<std::uint8_t, std::string>> move_beside;
    if (session != nullptr && (out.send || rev.send || threat || !out.events.empty() ||
                               out.state == State::downed || out.state == State::spectating || rev.target != 0)) {
        session->with_client([&](coop::net::Client& client) {
            if (connected && out.send) {
                client.send(*out.send);
            }
            if (connected && rev.send) {
                client.send(*rev.send);
            }
            if (connected && threat) {
                client.send(*threat);
            }
            for (const Event e : out.events) {
                if (auto line = OwnLine(e, NameOf(client, out.by))) {
                    lines.push_back(std::move(*line));
                }
                LOG_INFO(Core, "coop: dbno: our Link: event {} (by P{})", static_cast<int>(e), out.by);
            }
            if (out.move_beside != 0) {
                move_beside = std::make_pair(out.move_beside, NameOf(client, out.move_beside));
            }
            if (out.state == State::downed) {
                view = View{View::Kind::downed, NameOf(client, out.reviver), out.left_us, out.progress, false};
            } else if (out.state == State::spectating) {
                view = View{View::Kind::spectating, NameOf(client, out.watching), 0, 0.0f, false};
                view->elsewhere = out.watching != 0 && !watched_here;
            } else if (rev.target != 0) {
                View v{View::Kind::reviving, NameOf(client, rev.target), 0, 0.0f, rev.holding};
                std::lock_guard lock(g.mutex);
                if (const auto e = g.roster.Downed(rev.target)) {
                    v.left_us = Roster::LeftUs(*e, now_us);
                    if (e->status.reviver == self) {
                        v.progress = static_cast<float>(e->status.progress) / 255.0f;
                    }
                }
                view = std::move(v);
            }
        });
    }
    // Coming back: our Link is moved first, the respawn follows once it is
    // (Input::moved). Beside the one watched: the /teleport trip (its own
    // scene load when they are elsewhere; the game's own move in our scene).
    // Else the entrance: the game's own scene start.
    if (move_beside) {
        LOG_INFO(Core, "coop: dbno: respawn: moving beside P{}", move_beside->first);
        if (!CoopTeleport::Beside(move_beside->first, std::move(move_beside->second))) {
            g.move_refused = true;  // teleport is off in this game: the entrance
        }
    }
    if (out.move_entrance) {
        LOG_INFO(Core, "coop: dbno: respawn: not beside them: to the entrance");
        if (!CoopTeleport::ToEntrance()) {
            g.move_refused = true;
        }
    }
    std::lock_guard lock(g.mutex);
    for (auto& l : lines) {
        g.Line(std::move(l));
    }
    g.view = std::move(view);
    g.spectate_offset = offset;
}

std::vector<std::string> TakeLines() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    std::vector<std::string> out;
    out.swap(g.lines);
    return out;
}

std::optional<View> Shown() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return g.view;
}

State StateOfPlayer(std::uint8_t number) {
    Glue& g = TheGlue();
    if (number != 0 && number == g.own_number.load(std::memory_order_relaxed)) {
        return static_cast<State>(g.own_state.load(std::memory_order_relaxed));
    }
    std::lock_guard lock(g.mutex);
    return g.roster.StateOf(number);
}

void NoteEnemiesNear(bool any) {
    TheGlue().enemies_near.store(any, std::memory_order_relaxed);
}

void NoteBottledFairy(bool held) {
    Glue& g = TheGlue();
    if (g.bottled_fairy.exchange(held, std::memory_order_relaxed) != held) {
        LOG_INFO(Core, "coop: dbno: {}", held ? "a fairy in our bottle: a lethal hit is the game's own death, its "
                                                "fairy revives us (not downed)"
                                              : "no fairy in our bottles: a lethal hit downs us");
    }
}

std::optional<Vec3> SpectateOffset() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return g.spectate_offset;
}

// --- The game's threads ---

LifeGuard& TheGuard() {
    return TheGlue().guard;
}

void SetOurLifeComponent(std::uint64_t component) {
    TheGlue().life_component.store(component, std::memory_order_release);
}

std::uint64_t OurLifeComponent() {
    return TheGlue().life_component.load(std::memory_order_acquire);
}

std::int32_t LifeStep(std::uint64_t component, std::int32_t life, std::int32_t sum) {
    Glue& g = TheGlue();
    if (component == 0 || component != g.life_component.load(std::memory_order_acquire)) {
        return sum;
    }
    sum = g.guard.Step(life, sum);
    sum = Lowered(life, sum, g.take_to.Take());
    // A raise (revive, respawn) as a negative sum: the game's heal. Kept until
    // the frame sees the life reached (Raising::Frame), as the step may not
    // apply it this frame.
    return Body::Raised(life, sum, Body::TheRaising().Peek());
}

void SetOurComponents(std::uint64_t bsa, std::uint64_t emitter, bool adult, std::uint64_t actor) {
    TheGlue().actor.store(actor, std::memory_order_release);
    TheGlue().adult.store(adult, std::memory_order_release);
    TheGlue().bsa.store(bsa, std::memory_order_release);
    TheGlue().emitter.store(emitter, std::memory_order_release);
}

std::optional<std::int32_t> TakeLifeWrite() {
    const std::int32_t want = TheGlue().life_write.exchange(-1);
    if (want < 0) {
        return std::nullopt;
    }
    return want;
}

Pad OnPad(const Pad& in) {
    Glue& g = TheGlue();
    g.button.store((in.buttons & pad::a) != 0, std::memory_order_relaxed);
    switch (g.pad_mode.load(std::memory_order_relaxed)) {
    case PadMode::downed:
        return DownedPad(in);
    case PadMode::reviver:
        return ReviverPad(in);
    case PadMode::spectating:
        return SpectatorPad(in);
    case PadMode::game:
        break;
    }
    return in;
}

}  // namespace CoopDbno
