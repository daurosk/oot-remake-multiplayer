// SPDX-License-Identifier: GPL-3.0-or-later
//
// /teleport <player>, the emulator side (coop_teleport.h, docs/setup.md
// "Chat commands"). The asking and answering go through JOIN_PARTNER
// (coop/net Client: request_join, accept_join, refuse_join); our Link is
// moved the way the boss rooms do it (CoopBoss::MoveLink;
// docs/dungeons-boss.md). Another scene is asked the way the game's Main
// sequence takes a scene change in free play (docs/scenes.md "Free-play
// scene change"): Sequence_ReservedNextBanc and its start name, then a short
// game event (Trip::kSceneEvent through CoopLiveEvents::Requester), since the
// game takes the request when an event ends, as after a loading zone's event.
// LICHII_COOP_TELEPORT=0 turns it off: the command says so and requests from
// the others are refused (declined).

#include "coop_teleport_game.h"

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
#include "coop_fairy.h"
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
    std::mutex mutex;
    // Present thread -> network thread: what we would answer now.
    Answer answer;
    // Chat / network threads -> present thread.
    std::optional<std::pair<std::uint8_t, std::string>> want;
    std::vector<Answered> answers;
    // Any thread -> the hook's chat box.
    std::vector<std::string> lines;
    // Present thread only.
    Trip trip;
    // The start name before the trip's first ask (AskScene, ClearScene).
    std::optional<std::string> saved_start;
    bool saved_read = false;
    Grounded grounded;

    // The scene event (Trip::kSceneEvent): asked on the present thread,
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
        LOG_INFO(Core, "coop: teleport {}", on ? "on (/teleport <player> in chat)" : "off (LICHII_COOP_TELEPORT=0)");
    }

    static constexpr int kEventCallerOrder = 61;  // after the live events (60)
    std::uint64_t main = 0;  // set by Frame under event_mutex
    bool events_open = false;  // set by Frame under event_mutex: travelling, no load

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
        local.event = reading->event_running.value_or(false);
        local.fighting = CoopBoss::Fighting();
        CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_base);
        coop::host::GameFlags flags(memory);
        local.reserved_banc = flags.get_string64(RequestFlags().banc);
        local.loading_banc = flags.get_bool(RequestFlags().loading).value_or(false);
    }
    g.grounded.Add(local, now);

    std::optional<std::pair<std::uint8_t, std::string>> want;
    std::vector<Answered> answers;
    {
        std::lock_guard lock(g.mutex);
        g.answer = AnswerFor(local, g.grounded);
        want = std::exchange(g.want, std::nullopt);
        answers = std::exchange(g.answers, {});
    }

    std::vector<Trip::Actions> all;
    if (want) {
        all.push_back(g.trip.Start(want->first, want->second, local, now));
    }
    for (const auto& a : answers) {
        switch (a.kind) {
        case Answered::Kind::accepted:
            all.push_back(g.trip.Accepted(a.player, a.target, local, now));
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
        g.events_open = g.trip.phase() == Trip::Phase::travelling && local.scene && !local.loading_banc;
    }
    for (auto& act : all) {
        if ((act.cancel_scene || act.release) && main_base != 0) {
            ClearScene(system, main_base, act.cancel_scene, act.restore_start, g.saved_start);
            if (act.cancel_scene) {
                g.saved_start.reset();  // the next trip reads it afresh
                g.saved_read = false;
            }
        }
        if (act.game_cleared) {
            LOG_INFO(Core, "coop: teleport: scene change cleared by the game after its arrival (nothing written)");
        }
        if (act.scene) {
            // Always with a start point of that scene: without one the game
            // keeps the old scene's start name and the picture stays black.
            if (main_base == 0 || act.start.empty() ||
                !AskScene(system, main_base, *act.scene, act.start, g.saved_start, g.saved_read)) {
                LOG_WARNING(Core, "coop: teleport: the scene change to {} at {} could not be asked", *act.scene,
                            act.start.empty() ? "-" : act.start);
            }
        }
        if (act.start_event && main_base != 0) {
            LOG_INFO(Core, "coop: teleport: starting the event {} so the game takes the scene change",
                     Trip::kSceneEvent);
            std::lock_guard lock(g.event_mutex);
            g.events.Want(Trip::kSceneEvent);
        }
        if (act.move && local.scene && local.position) {
            // Issue 197: never into another room of a dungeon (Link stays held).
            // Read once, at the first move: a pack made (again) while the game runs is
            // seen after a restart; until then no table means short moves only.
            static const CoopRooms::Table rooms = CoopFairy::RoomTable();
            const auto check = CheckMove(rooms, *local.scene, *local.position, *act.move);
            if (check != MoveCheck::ok) {
                LOG_INFO(Core, "coop: teleport: not moved to {:.1f} {:.1f} {:.1f}: {} (issue 197)", act.move->x,
                         act.move->y, act.move->z,
                         check == MoveCheck::other_room ? "another room of this dungeon"
                                                        : "far, and no room table for this dungeon");
                bool told = false;
                for (auto& line : act.lines) {
                    if (line.rfind("teleport: next to ", 0) == 0) {
                        line = NotMovedLine(check, line.substr(18), act.after_load);
                        told = true;
                    }
                }
                if (!told) {
                    act.lines.push_back(NotMovedLine(check, "", act.after_load));
                }
                act.move.reset();
            }
        }
        if (act.move) {
            const bool ok = CoopBoss::MoveLink(system, main_base, CoopBoss::Vec{act.move->x, act.move->y, act.move->z});
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
}

std::vector<std::string> TakeLines() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.lines, {});
}

} // namespace CoopTeleport
