// SPDX-License-Identifier: GPL-3.0-or-later
//
// /teleport <player>, the emulator side (coop_teleport.h, docs/setup.md
// "Chat commands"). The asking and answering go through JOIN_PARTNER
// (coop/net Client: request_join, accept_join, refuse_join); our Link is
// moved the way the boss rooms do it (CoopBoss::MoveLink;
// docs/dungeons-boss.md). Another scene is asked the way the game's Main
// sequence takes it from a loading zone: the zone's own event
// Dm_SY_BancChangeGo with the scene, its start point and the entrance's
// arrival event, started by our Link through CoopLiveEvents::Requester
// (teleport-direct-0116; LICHII_COOP_TELEPORT_EVENT=0: the request flags
// instead, taken when a talk or sign of the player's own ends, #197).
// LICHII_COOP_TELEPORT=0 turns it off: the command says so and requests from
// the others are refused (declined).

#include "coop_teleport_game.h"
#include "coop_caller_orders.h"

#include "coop/presence/presence.hpp"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <utility>
#include <variant>

#include "common/logging.h"
#include "core/core.h"
#include "core/memory.h"
#include "coop/host/flags.hpp"
#include "coop/host/game_flags.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_boss.h"
#include "coop_flags.h"
#include "coop_live_events.h"
#include "coop_svc.h"

namespace CoopTeleport {
namespace {

std::uint64_t SteadyUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

bool OnByEnv() {
    const char* v = std::getenv("LICHII_COOP_TELEPORT");
    return v == nullptr || std::string_view(v) != "0";
}

// Another scene is asked the game's own way, as an entrance does it (read from the game by Longshot's own
// teleport, host/hos_main.c oot_teleport): the event Dm_SY_BancChangeGo with the arguments NextBanc, NextStartPos,
// BootEventName (the entrance's arrival event) and IsCheckAir, started by our Link; the event calls the game's
// ReserveBancChange and the sequence loads the map when it ends; the arrival event hands Link back. Nothing is
// written into the request flags then. On by default since teleport-direct-0116 (it was off and, with
// Trip::kStartSceneEvent false, never asked at all: the switch did nothing). LICHII_COOP_TELEPORT_EVENT=0 goes
// back to the request flags taken by the player's own talk or sign (docs/hookshot.md "World hooks").
bool EventByEnv() {
    const char* v = std::getenv("LICHII_COOP_TELEPORT_EVENT");
    return v == nullptr || std::string_view(v) != "0";
}
constexpr const char* kBancChangeEvent = "Dm_SY_BancChangeGo";

// The arrival event's starter of a start with no entrance actor (a boss stage) is emptied after the game's event
// reserved it (ArrivalStarter, issue 289). LICHII_COOP_ARRIVAL_STARTER=keep leaves the game's reservation as it is:
// only for a proof run that shows the black arrival again.
bool StarterByEnv() {
    const char* v = std::getenv("LICHII_COOP_ARRIVAL_STARTER");
    return v == nullptr || std::string_view(v) != "keep";
}
const std::uint32_t kReservedStarter = coop::host::flag_hash("Sequence_ReservedEventStarterName");

// The map change event's arguments for `scene` at its start point `start`.
std::vector<CoopLiveEvents::Requester::Arg> BancChangeArgs(std::string_view scene, std::string_view start) {
    using Arg = CoopLiveEvents::Requester::Arg;
    return {Arg{"NextBanc", std::string(scene), false}, Arg{"NextStartPos", std::string(start), false},
            Arg{"BootEventName", ArrivalEvent(scene, start), false}, Arg{"IsCheckAir", std::nullopt, false}};
}

// A map-hiding area sets this saved Bool and clears it on its own exit; a teleport skips that exit, so it stays
// set and the minimap stays hidden in every later scene (Longshot issues 2217, 2588; its teleport clears it the
// same way). Cleared through the game's Bool setter (queued: live value and the save's copy) when we ask the map
// change, never during a load (#197).
const std::uint32_t kMapIconHidden = coop::host::flag_hash("MapIconDeactivateByAreaActor");

// Main.module's native selector (_LocalModule_cf31571495c4, 0x3303ca0) picks
// its BancChange case (ChangeBancOnMain.module) when this Bool is 1 (read at
// 0x3304154). Its name is in no file of the game; nothing in the game writes
// it (docs/scenes.md "Free-play scene change"). Not saved, no reset type.
// No longer set (a loading zone leaves it 0, and setting it loaded nothing in
// free play, #156); only cleared, for kits that set it before.
constexpr std::uint32_t kFreePlayBancChange = 0x3fc35576;

// The game's request flags: our scene change is these.
struct Request {
    std::uint32_t banc = coop::host::flag_hash("Sequence_ReservedNextBanc");
    std::uint32_t start = coop::host::flag_hash("Sequence_ReservedNextStartPosName");
    std::uint32_t loading = coop::host::flag_hash("Sequence_IsLoadingBanc");
};
const Request& RequestFlags() {
    static const Request r;
    return r;
}

// Ask: the start point of that scene (docs/scenes.md "The start point"),
// then the scene: what a loading zone's event reserves before it ends.
// `saved`: the start name the game had before the trip's first ask, read
// once, so a trip that never arrives can put it back.
bool AskScene(Core::System& system, std::uint64_t main, std::string_view scene, std::string_view start,
              std::optional<std::string>& saved, bool& saved_read) {
    CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main);
    if (!saved_read) {
        saved_read = true;
        saved = coop::host::GameFlags(memory).get_string64(RequestFlags().start);
    }
    const bool start_ok = coop::host::GameFlags::set_string64_now(memory, RequestFlags().start, start);
    const bool banc = start_ok && coop::host::GameFlags::set_string64_now(memory, RequestFlags().banc, scene);
    LOG_INFO(Core, "coop: teleport: asked the scene change to {} at {}: scene {}", scene, start,
             banc ? "set" : "NOT set");
    return banc;
}

// The switch off (the game took the request) and, for `all`, the request
// itself (arrived or given up), so no later event or frame takes it again.
// `restore`: not arrived, the start name from before the trip is put back
// (arrived: the start name is that scene's own, as after a normal load).
void ClearScene(Core::System& system, std::uint64_t main, bool all, bool restore,
                const std::optional<std::string>& saved) {
    CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main);
    // Written only when it is on (a kit before #156 set it): set_bool queues
    // the write in the game's own flag queue, and this runs on the emulator's
    // thread while the game loads and queues its own writes (#197: nothing of
    // ours goes into that queue during a load). Off: nothing to write.
    const bool on = coop::host::GameFlags(memory).get_bool(kFreePlayBancChange).value_or(false);
    const bool off = !on || coop::host::GameFlags::set_bool(memory, kFreePlayBancChange, false);
    if (!all) {
        LOG_INFO(Core, "coop: teleport: scene change taken: switch {}", !on ? "was off" : off ? "off" : "NOT off");
        return;
    }
    const bool banc = coop::host::GameFlags::set_string64_now(memory, RequestFlags().banc, "");
    std::string start = "kept";
    if (restore && saved) {
        start = coop::host::GameFlags::set_string64_now(memory, RequestFlags().start, *saved)
                    ? "put back to \"" + *saved + "\""
                    : "NOT put back";
    }
    LOG_INFO(Core, "coop: teleport: scene change cleared: switch {}, request {}, start {}", off ? "off" : "NOT off",
             banc ? "emptied" : "NOT emptied", start);
}

// What the network and chat threads hand to the present thread.
struct Answered {
    enum class Kind : std::uint8_t { accepted, refused, timed_out, left };
    Kind kind = Kind::accepted;
    std::uint8_t player = 0;  // left: 0 is our own connection
    coop::protocol::JoinTarget target;
    coop::protocol::JoinRefuseReason reason{};
};

struct Glue {
    bool on = OnByEnv();
    bool by_event = EventByEnv();
    // by_event: the scene and start point of the trip's last ask, for the event's arguments.
    std::string event_scene, event_start;
    std::mutex mutex;
    // Present thread -> network thread: what we would answer now.
    Answer answer;
    // Chat / network threads -> present thread.
    std::optional<std::pair<std::uint8_t, std::string>> want;
    std::vector<Answered> answers;
    // Down but not out's respawn (Beside, ToEntrance): the trip asked for it,
    // the entrance asked, and how the move went (taken by TakeRespawnMove).
    bool want_beside = false;
    bool want_entrance = false;
    // MapChange (the play-test bridge's warp): the scene and start point to ask the map change event for.
    std::optional<std::pair<std::string, std::string>> want_map;
    int map_frames_left = 0;   // frames a map change that could not start (an event or a load) is kept for
    std::optional<bool> respawn_moved;
    // Any thread -> the hook's chat box.
    std::vector<std::string> lines;
    // Present thread only.
    Trip trip{by_event};
    // The respawn's entrance (ToEntrance, by_event): its map change event may be asked until then (no trip carries
    // it); 0 none. Ended by a load.
    std::uint64_t entrance_until_us = 0;
    static constexpr std::uint64_t kEntranceOpenUs = 5'000'000;
    std::uint8_t beside_to = 0;  // the respawn's trip under way: to this player; 0 none
    // The start name before the trip's first ask (AskScene, ClearScene).
    std::optional<std::string> saved_start;
    bool saved_read = false;
    Grounded grounded;
    // The arrival's starter of the map change event asked last (present thread only).
    bool fix_starter = StarterByEnv();
    ArrivalStarter starter;

    // The map change event (Dm_SY_BancChangeGo): asked on the present thread,
    // started from the svc cave on the game's calc thread.
    std::mutex event_mutex;
    CoopLiveEvents::Requester events;

    Glue() {
        if (on) {
            CoopSvc::AddCaller(kEventCallerOrder, [this](CoopPuppet::Guest& guest, bool after_call,
                                                         std::uint64_t result, std::uint64_t) {
                std::lock_guard lock(event_mutex);
                // Only while a trip waits for its scene change and nothing
                // loads (#197): a queued event must never start in the middle
                // of a load or after the trip ended. A call chain under way
                // still finishes (it frees its block).
                if (!after_call && !events_open) {
                    events.Clear();
                }
                auto call = events.Step(guest, after_call, result, main);
                for (const auto& note : events.TakeNotes()) {
                    LOG_INFO(Core, "coop: teleport: {}", note);
                }
                return call;
            });
        }
        LOG_INFO(Core, "coop: teleport {}",
             !on        ? "off (LICHII_COOP_TELEPORT=0)"
             : by_event ? "on (/teleport <player> in chat; another scene by the game's map change event)"
                        : "on (/teleport <player> in chat; another scene waits for a talk or sign: "
                          "LICHII_COOP_TELEPORT_EVENT=0)");
    }

    static constexpr int kEventCallerOrder = CoopCallerOrders::kTeleportEvent;  // after the live events (60)
    std::uint64_t main = 0;  // set by Frame under event_mutex
    bool events_open = false;  // set by Frame under event_mutex: travelling, no load
    bool underway = false;     // set by Frame under event_mutex: a trip past its answer (Underway)

    void Line(std::string text) {  // with the mutex held
        LOG_INFO(Core, "coop: {}", text);
        lines.push_back(std::move(text));
    }
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

std::vector<Player> Players(const coop::net::Client& client) {
    std::vector<Player> out;
    for (const auto& p : client.players()) {
        out.push_back({p.number, p.nickname});
    }
    return out;
}

} // namespace

bool OnChat(std::string_view text, coop::host::Session& session) {
    const auto command = Parse(text);
    if (!command) {
        return false;
    }
    Glue& g = TheGlue();
    // The network lock first, then ours: the order OnEvent has them in.
    std::string line;
    std::optional<std::pair<std::uint8_t, std::string>> want;
    session.with_client([&](coop::net::Client& client) {
        if (client.state() != coop::net::State::connected) {
            line = "teleport: not connected";
            return;
        }
        const auto players = Players(client);
        const std::uint8_t self = client.player_number();
        if (command->kind == Command::Kind::list) {
            line = ListLine(players, self);
            return;
        }
        const Found found = FindPlayer(command->name, players, self);
        if (found.number == 0) {
            line = found.error;
            return;
        }
        want = std::make_pair(found.number, found.name);
    });
    std::lock_guard lock(g.mutex);
    if (!g.on) {
        g.Line("teleport: off in this game (LICHII_COOP_TELEPORT=0)");
    } else if (want) {
        g.want = std::move(want);
    } else {
        g.Line(std::move(line));
    }
    return true;
}

bool Beside(std::uint8_t number, std::string name) {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    if (!g.on || number == 0) {
        return false;
    }
    g.want = std::make_pair(number, std::move(name));
    g.want_beside = true;
    g.respawn_moved.reset();
    return true;
}

bool Underway() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.event_mutex);
    return g.underway;
}

bool ToEntrance() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    if (!g.on || !g.by_event) {
        // #197: no event of ours may carry it (DefaultParentEvent held Link after the load), and a request left for
        // a talk would load later, at a bad moment: not moved (the respawn's own fallback then).
        LOG_INFO(Core, "coop: teleport: respawn at the entrance: off (needs the map change event)");
        return false;
    }
    g.want_entrance = true;
    g.respawn_moved.reset();
    return true;
}

bool MapChange(std::string scene, std::string start) {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    if (!g.on || !g.by_event) {
        LOG_INFO(Core, "coop: teleport: map change to {}: off (needs the map change event)", scene);
        return false;
    }
    if (start.empty()) {
        const auto first = DefaultStart(scene);
        if (!first) {
            LOG_INFO(Core, "coop: teleport: map change to {}: no start point known", scene);
            return false;
        }
        start = *first;
    }
    g.want_map = std::make_pair(std::move(scene), std::move(start));
    g.map_frames_left = 300;   // about 5 s: an event or a load at this frame lets the next free frame start it
    return true;
}

bool WantsSoloFrame() {
    Glue& g = TheGlue();
    if (g.starter.active() || g.trip.phase() != Trip::Phase::idle) {
        return true;   // (present-thread state: read where Frame writes it)
    }
    {
        std::lock_guard lock(g.mutex);
        if (g.want_map) return true;
    }
    std::lock_guard lock(g.event_mutex);
    // Frame clears both on the call that ends the window: the svc cave's caller never Steps a left-open event.
    return g.events_open || g.underway || g.entrance_until_us != 0;
}

std::optional<bool> TakeRespawnMove() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.respawn_moved, std::nullopt);
}

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    using coop::net::EventKind;
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    switch (event.kind) {
    case EventKind::join_request: {
        std::string name = "P" + std::to_string(event.player);
        for (const auto& p : client.players()) {
            if (p.number == event.player && !p.nickname.empty()) {
                name = p.nickname;
            }
        }
        if (!g.on) {
            client.refuse_join(event.player, event.request_id, coop::protocol::JoinRefuseReason::declined);
            LOG_INFO(Core, "coop: teleport: {} asked to come here: refused (off)", name);
            break;
        }
        const Answer a = g.answer;
        if (a.accept) {
            const bool sent = client.accept_join(event.player, event.request_id, a.target);
            LOG_INFO(Core, "coop: teleport: {} asked to come here: {} at {:.1f} {:.1f} {:.1f}{}", name,
                     a.target.scene, a.target.position[0], a.target.position[1], a.target.position[2],
                     sent ? "" : " (not sent)");
            if (sent) {
                g.lines.push_back("teleport: " + name + " is coming to you");
            }
        } else {
            client.refuse_join(event.player, event.request_id, a.reason);
            LOG_INFO(Core, "coop: teleport: {} asked to come here: refused, reason {}", name,
                     static_cast<int>(a.reason));
        }
        break;
    }
    case EventKind::join_accepted:
        g.answers.push_back({Answered::Kind::accepted, event.player, event.join_target, {}});
        break;
    case EventKind::join_refused:
        g.answers.push_back({Answered::Kind::refused, event.player, {}, event.join_reason});
        break;
    case EventKind::join_timed_out:
        g.answers.push_back({Answered::Kind::timed_out, event.player, {}, {}});
        break;
    case EventKind::player_left:
        g.answers.push_back({Answered::Kind::left, event.player, {}, {}});
        break;
    case EventKind::disconnected:
        g.answers.push_back({Answered::Kind::left, 0, {}, {}});
        break;
    default:
        break;
    }
}

void Frame(Core::System& system, std::uint64_t main_base, const std::optional<coop::host::PlayerReading>& reading,
           coop::host::Session* session) {
    Glue& g = TheGlue();
    const std::uint64_t now = SteadyUs();
    Local local;
    if (main_base != 0 && reading) {
        local.scene = reading->scene;
        local.position = Vec{reading->position[0], reading->position[1], reading->position[2]};
        local.rotation = reading->rotation;
        local.age = reading->age;
        local.event = reading->event_running.value_or(false);  // G1: any event (the trip is an event of its own; the game takes one at a time)
        local.fighting = CoopBoss::Fighting();
        CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_base);
        coop::host::GameFlags flags(memory);
        local.reserved_banc = flags.get_string64(RequestFlags().banc);
        local.loading_banc = flags.get_bool(RequestFlags().loading).value_or(false);
    }
    g.grounded.Add(local, now);
    if (g.starter.active() && main_base != 0) {
        // Also during the load (no reading then): the game reads the starter right after it. A plain write into the
        // flag's own text, nothing queued (#197).
        CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_base);
        coop::host::GameFlags flags(memory);
        const bool write = g.starter.Tick(flags.get_string64(RequestFlags().banc), flags.get_string64(kReservedStarter),
                                          flags.get_bool(RequestFlags().loading).value_or(false), now);
        if (write) {
            const bool ok = coop::host::GameFlags::set_string64_now(memory, kReservedStarter, "");
            if (g.starter.writes() == 1 || !ok) {
                LOG_INFO(Core, "coop: teleport: {} has no entrance actor named {}: the arrival event's starter {} "
                         "(as the game's own entries to a boss stage reserve it)",
                         g.starter.scene(), g.starter.start(), ok ? "emptied" : "NOT emptied");
            }
        } else if (!g.starter.active()) {
            LOG_INFO(Core, "coop: teleport: the arrival at {} of {} is over: its starter was emptied {} time(s)",
                     g.starter.start(), g.starter.scene(), g.starter.writes());
        }
    }

    std::optional<std::pair<std::uint8_t, std::string>> want;
    std::vector<Answered> answers;
    bool want_beside = false;
    bool want_entrance = false;
    std::optional<std::pair<std::string, std::string>> want_map;
    {
        std::lock_guard lock(g.mutex);
        want_map = g.want_map;   // kept until it starts or its frames run out (below)
        g.answer = AnswerFor(local, g.grounded);
        want = std::exchange(g.want, std::nullopt);
        answers = std::exchange(g.answers, {});
        want_beside = std::exchange(g.want_beside, false);
        want_entrance = std::exchange(g.want_entrance, false);
    }

    // Down but not out's respawn: how its move goes (nothing: not known yet).
    std::optional<bool> respawn_moved;
    std::vector<Trip::Actions> all;
    if (want) {
        all.push_back(g.trip.Start(want->first, want->second, local, now));
        g.beside_to = want_beside ? want->first : 0;
        if (g.beside_to != 0 && !all.back().ask) {
            respawn_moved = false;  // refused at once (an event of ours, a boss fight, not connected)
            g.beside_to = 0;
        }
    }
    if (want_entrance) {
        // The game's own respawn point: our scene again, at its entrance nearest us.
        const auto start = local.scene && local.position ? NearestStart(*local.scene, *local.position)
                                                         : std::nullopt;
        // (row 329: nor in the gap between the game's own map change event and its load: LoadReserved)
        if (!start || main_base == 0 || local.loading_banc || local.event || LoadReserved(local)) {
            respawn_moved = false;
            LOG_INFO(Core, "coop: teleport: respawn at the entrance: not asked ({})",
                     !start                ? "no start point known here"
                     : main_base == 0      ? "no game"
                     : local.loading_banc  ? "a load runs"
                     : local.event         ? "an event runs"
                                           : "a scene load is reserved");
        } else {
            Trip::Actions entrance;
            entrance.scene = *local.scene;
            entrance.start = *start;
            entrance.start_event = true;  // the game's map change event: at once (no talk or sign needed)
            all.push_back(std::move(entrance));
            g.entrance_until_us = now + Glue::kEntranceOpenUs;
            respawn_moved = true;
            LOG_INFO(Core, "coop: teleport: respawn at the entrance {} of {}", *start, *local.scene);
        }
    }
    if (want_map) {
        // The bridge's warp: the same map change event as the respawn's entrance, with the scene and start asked for.
        if (main_base == 0 || local.loading_banc || local.event || LoadReserved(local)) {
            std::lock_guard lock(g.mutex);
            if (--g.map_frames_left <= 0) {
                g.want_map.reset();
                LOG_INFO(Core, "coop: teleport: map change to {} at {}: dropped, no free frame for 5 s ({})",
                         want_map->first, want_map->second,
                         main_base == 0         ? "no game"
                         : local.loading_banc   ? "a load runs"
                         : local.event          ? "an event runs"
                                                : "a scene load is reserved");
            }
        } else {
            {
                std::lock_guard lock(g.mutex);
                g.want_map.reset();
            }
            Trip::Actions map;
            map.scene = want_map->first;
            map.start = want_map->second;
            map.start_event = true;
            all.push_back(std::move(map));
            g.entrance_until_us = now + Glue::kEntranceOpenUs;
            LOG_INFO(Core, "coop: teleport: map change to {} at {} (play-test warp)", want_map->first, want_map->second);
        }
    }
    for (const auto& a : answers) {
        if (g.beside_to != 0 && a.player == g.beside_to && a.kind != Answered::Kind::accepted) {
            respawn_moved = false;  // refused, no answer, or they left
            g.beside_to = 0;
        }
        switch (a.kind) {
        case Answered::Kind::accepted:
        {
            std::optional<coop::protocol::Age> their_age;
            if (session != nullptr) {
                their_age = session->with_presence([&](coop::presence::Presence& p) {
                    const auto* pl = p.player(a.player);
                    return pl ? pl->age() : std::nullopt;
                });
            }
            all.push_back(g.trip.Accepted(a.player, a.target, local, now, their_age));
        }
            break;
        case Answered::Kind::refused:
            all.push_back(g.trip.Refused(a.player, a.reason));
            break;
        case Answered::Kind::timed_out:
            all.push_back(g.trip.TimedOut(a.player));
            break;
        case Answered::Kind::left:
            all.push_back(g.trip.Left(a.player));
            break;
        }
    }
    all.push_back(g.trip.Tick(local, now));

    {
        std::lock_guard lock(g.event_mutex);
        g.main = main_base;
        if (local.loading_banc || now >= g.entrance_until_us) {
            g.entrance_until_us = 0;  // the respawn's map change was taken, or not in time
        }
        const bool entrance = g.entrance_until_us != 0;
        g.events_open = (g.trip.phase() == Trip::Phase::travelling || entrance) && local.scene && !local.loading_banc;
        g.underway = g.trip.phase() == Trip::Phase::travelling || g.trip.phase() == Trip::Phase::settling ||
                     g.trip.phase() == Trip::Phase::asking_again || entrance;
    }
    for (auto& act : all) {
        if ((act.cancel_scene || act.release) && main_base != 0 && g.by_event) {
            // Nothing of ours is in the request flags: the game's own event reserved the scene, and clears it after
            // its arrival. Only a reservation of our scene the game made and never loaded (given up between its
            // event and its load) is emptied, or a later talk or sign would load it.
            if (act.release) {
                LOG_INFO(Core, "coop: teleport: scene change taken (the game's map change event)");
            }
            if (act.cancel_scene) {
                if (!local.loading_banc && !g.event_scene.empty() && local.reserved_banc &&
                    *local.reserved_banc == g.event_scene) {
                    ClearScene(system, main_base, true, false, std::nullopt);
                }
                g.event_scene.clear();
                g.event_start.clear();
            }
        } else if ((act.cancel_scene || act.release) && main_base != 0) {
            ClearScene(system, main_base, act.cancel_scene, act.restore_start, g.saved_start);
            if (act.cancel_scene) {
                g.saved_start.reset();  // the next trip reads it afresh
                g.saved_read = false;
            }
        }
        if (act.game_cleared) {
            LOG_INFO(Core, "coop: teleport: scene change cleared by the game after its arrival (nothing written)");
        }
        if (act.scene && g.by_event) {
            g.event_scene = *act.scene;
            g.event_start = act.start;
            LOG_INFO(Core, "coop: teleport: the scene change to {} at {} goes by the game's map change event",
                     *act.scene, act.start.empty() ? "-" : act.start);
        } else if (act.scene) {
            // Always with a start point of that scene: without one the game
            // keeps the old scene's start name and the picture stays black.
            if (main_base == 0 || act.start.empty() ||
                !AskScene(system, main_base, *act.scene, act.start, g.saved_start, g.saved_read)) {
                LOG_WARNING(Core, "coop: teleport: the scene change to {} at {} could not be asked", *act.scene,
                            act.start.empty() ? "-" : act.start);
            }
        }
        if (act.start_event && main_base != 0 && g.by_event) {
            CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_base);
            const std::uint64_t link = coop::host::PlayerReader(memory).find_player_actor().value_or(0);
            if (g.event_scene.empty() || g.event_start.empty() || link == 0) {
                LOG_WARNING(Core, "coop: teleport: the map change event not asked ({})",
                            link == 0 ? "our Link's actor was not found" : "no scene or start point");
            } else {
                LOG_INFO(Core, "coop: teleport: asking the game's map change event {} for {} at {} (arrival {})",
                         kBancChangeEvent, g.event_scene, g.event_start, ArrivalEvent(g.event_scene, g.event_start));
                if (coop::host::GameFlags(memory).get_bool(kMapIconHidden).value_or(false)) {
                    const bool cleared = coop::host::GameFlags::set_bool(memory, kMapIconHidden, false);
                    LOG_INFO(Core, "coop: teleport: the area left had hidden the minimap "
                             "(MapIconDeactivateByAreaActor): {}", cleared ? "restore queued" : "NOT cleared");
                }
                std::lock_guard lock(g.event_mutex);
                g.events.WantWith(kBancChangeEvent, BancChangeArgs(g.event_scene, g.event_start), link);
                if (g.fix_starter) {
                    g.starter.Begin(g.event_scene, g.event_start, now);
                } else if (!ArrivalHasStarter(g.event_scene, g.event_start)) {
                    LOG_INFO(Core, "coop: teleport: {} has no entrance actor named {}: the starter is kept "
                             "(LICHII_COOP_ARRIVAL_STARTER=keep)", g.event_scene, g.event_start);
                }
            }
        }
        if (g.beside_to != 0 && act.scene) {
            respawn_moved = true;  // the trip loads their scene: we arrive at its entrance, then beside them
            g.beside_to = 0;
        }
        if (g.beside_to != 0 && act.cancel_scene) {
            respawn_moved = false;  // the trip was given up before it moved us
            g.beside_to = 0;
        }
        if (act.move) {
            const bool ok = CoopBoss::MoveLink(system, main_base, CoopBoss::Vec{act.move->x, act.move->y, act.move->z});
            if (g.beside_to != 0) {
                respawn_moved = ok;
                g.beside_to = 0;
            }
            LOG_INFO(Core, "coop: teleport: move to {:.1f} {:.1f} {:.1f}: {}", act.move->x, act.move->y,
                     act.move->z, ok ? "asked" : "NOT asked (no Link, or LICHII_COOP_BOSS=0)");
            if (!ok) {
                act.lines.push_back("teleport: could not move Link (no Link yet, or boss rooms are off: "
                                    "LICHII_COOP_BOSS=0)");
            }
        }
        if (act.ask) {
            const std::uint8_t to = *act.ask;
            const bool sent = session != nullptr && session->with_client([&](coop::net::Client& c) {
                return c.request_join(to).has_value();
            });
            LOG_INFO(Core, "coop: teleport: asked player {} where they are{}", to, sent ? "" : " (not sent)");
            if (!sent) {
                act.lines.push_back("teleport: could not ask (not connected?)");
                (void)g.trip.Left(0);  // asking: nothing to clear in the game
            }
        }
    }
    std::lock_guard lock(g.mutex);
    for (auto& act : all) {
        for (auto& line : act.lines) {
            g.Line(std::move(line));
        }
    }
    if (respawn_moved) {
        g.respawn_moved = respawn_moved;
    }
}

std::vector<std::string> TakeLines() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.lines, {});
}

} // namespace CoopTeleport
