// SPDX-License-Identifier: GPL-3.0-or-later
// Going to another player (teleport.hpp).
#include "teleport.hpp"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <span>
#include <utility>

#include "coop/host/flags.hpp"
#include "coop/host/game_flags.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/presence/presence.hpp"
#include "coop_dungeon.h"
#include "coop_live_events.h"
#include "coop_teleport.h"
#include "overlay.hpp"
#include "third_party/plugin_api.h"

namespace CoopNx2navi::Teleport {
namespace {

using CoopTeleport::Trip;

// As lichii/hook/coop_boss_game.cpp moves Link.
constexpr std::uint64_t kActorState = 0x2c;
constexpr std::uint32_t kActorStatePresent = 4;
constexpr std::uint64_t kActorPosition = 0x31c;
constexpr std::uint64_t kActorRotation = 0x328;  // 3 x 3 floats, rows (coop/host addresses.hpp)
constexpr std::uint64_t kSetMatrix = 0x4c6580;  // (actor, 0)

// Where our Link is put when it goes to another player: this far behind them
// (the side they came from, so there is ground and no wall), facing as they
// do. On their very spot the two Links stand in each other and the camera
// behind ours looks through the other one (seen: a large translucent Link over
// the whole picture).
constexpr float kArriveBehind = 1.3f;
constexpr float kArriveAside = 0.5f;

// The scene event is not started again before this many game frames (5 s at 30 a second).
constexpr std::uint64_t kEventAgainFrames = 150;
constexpr const char* kArriveEvent = "Dm_SY_BancChangeArrive";

// The peers' samples count as arriving for this long after the last one.
constexpr std::uint64_t kFreshUs = 2'000'000;

std::uint64_t SteadyUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// The game's request flags: our scene change is these.
struct Request {
    std::uint32_t banc = coop::host::flag_hash("Sequence_ReservedNextBanc");
    std::uint32_t start = coop::host::flag_hash("Sequence_ReservedNextStartPosName");
    std::uint32_t loading = coop::host::flag_hash("Sequence_IsLoadingBanc");
    // What a loading zone's event also reserves: the event the new scene
    // starts with (Link walks out of the entrance and the picture fades in)
    // and the entrance that starts it.
    std::uint32_t boot = coop::host::flag_hash("Sequence_ReservedBootEventName");
    std::uint32_t starter = coop::host::flag_hash("Sequence_ReservedEventStarterName");
};
const Request& RequestFlags() {
    static const Request r;
    return r;
}

// coop/host's view of the game's memory over the game thread's access.
class GuestMemory final : public coop::host::WritableGameMemory {
public:
    GuestMemory(CoopPuppet::Guest& guest, std::uint64_t main) : guest_(guest), main_(main) {}
    std::uint64_t main_base() const override { return main_; }
    bool read(std::uint64_t address, std::span<std::uint8_t> out) override {
        return guest_.Read(address, out.data(), out.size());
    }
    bool write(std::uint64_t address, std::span<const std::uint8_t> data) override {
        return guest_.Write(address, data.data(), data.size());
    }
    // Not used by what is called here (the String64 writes and the reads).
    std::optional<std::uint32_t> compare_exchange_u32(std::uint64_t, std::uint32_t,
                                                      std::uint32_t) override {
        return std::nullopt;
    }

private:
    CoopPuppet::Guest& guest_;
    std::uint64_t main_;
};

struct Answered {
    enum class Kind : std::uint8_t { accepted, refused, timed_out, left };
    Kind kind = Kind::accepted;
    std::uint8_t player = 0;  // left: 0 is our own connection
    coop::protocol::JoinTarget target;
    coop::protocol::JoinRefuseReason reason{};
};

struct Glue {
    void (*log)(const char* line) = nullptr;
    bool configured = false;

    std::mutex mutex;
    // Frame thread -> network thread: what we would answer now.
    CoopTeleport::Answer answer;
    // Frame thread -> game thread.
    CoopTeleport::Local local;  // without the request flags
    std::vector<Peer> peers;
    bool self_host = false;
    bool connected = false;
    // Chat / network threads -> game thread.
    std::optional<std::pair<std::uint8_t, std::string>> want;
    std::vector<Answered> answers;
    // Game thread -> frame thread: JOIN_PARTNER requests to send.
    std::vector<std::uint8_t> asks;
    // Any thread -> the panel.
    std::vector<std::string> lines;
    bool game_thread = false;  // the frame thread's view, for OnChat

    // Frame thread only.
    CoopTeleport::Grounded grounded;

    // Game thread only.
    Trip trip;
    Auto automatic;
    bool trip_auto = false;  // the running trip was started by `automatic`
    bool died = false;       // Died() since the game thread last looked (under mutex)
    // The game took our scene change request (its load started): from then on
    // the request flags are the game's. It empties them itself at the very end
    // of its scene start, and a parent node of that sequence tests
    // Sequence_ReservedNextBanc every frame meanwhile: emptying it from here in
    // the middle (as "arrived" used to) cut the scene start short, with no
    // player position set, no boot event, the 'SceneBeginning' pause never
    // released and the scene's passes never switched back on: the black
    // picture after a follow (found by the runtime's trace of the sequence
    // nodes, NOTES.md 22:10).
    bool taken = false;
    std::optional<std::string> saved_start;
    bool saved_read = false;
    CoopLiveEvents::Requester events;
    bool in_events = false;  // the call made last was the requester's
    std::optional<CoopTeleport::Vec> move;
    // With `move`: the facing to give our Link there (the other player's, as a
    // quaternion x, y, z, w), when the move is next to a player.
    std::optional<coop::protocol::Quat> move_facing;
    // The other player's facing from their last answer, and where they stood.
    std::optional<coop::protocol::Quat> target_facing;
    CoopTeleport::Vec target_at{};
    std::uint64_t said_ask_us = 0;
    bool arrive_event = false;      // SetArriveEvent
    std::uint64_t frames = 0;       // game frames seen
    std::uint64_t event_frame = 0;  // `frames` when the scene event was last started; 0: not in this trip
    // TestGoto: the places still to go to, and since when our Link is in the game.
    struct Goto {
        std::uint64_t after_us = 0;
        CoopTeleport::Vec to;
    };
    std::vector<Goto> gotos;
    std::uint64_t in_game_us = 0;
    // TestScene: the scene change still to make.
    struct Scene {
        std::uint64_t after_us = 0;
        std::string scene;
        std::string start;
    };
    std::optional<Scene> test_scene;
    std::uint64_t scene_in_game_us = 0;

#if defined(__GNUC__)
    __attribute__((format(gnu_printf, 2, 3)))
#endif
    void Log(const char* format, ...) {
        if (log == nullptr) {
            return;
        }
        char line[768];
        va_list args;
        va_start(args, format);
        std::vsnprintf(line, sizeof(line), format, args);
        va_end(args);
        log(line);
    }
    // A notice for the player (and the log). Takes the mutex.
    void Line(std::string text) {
        Log("%s", text.c_str());
        std::lock_guard lock(mutex);
        lines.push_back(std::move(text));
    }
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

std::vector<CoopTeleport::Player> Players(const coop::net::Client& client) {
    std::vector<CoopTeleport::Player> out;
    for (const auto& p : client.players()) {
        out.push_back({p.number, p.nickname});
    }
    return out;
}

bool AskScene(Glue& g, GuestMemory& memory, std::string_view scene, std::string_view start) {
    if (!g.saved_read) {
        g.saved_read = true;
        g.saved_start = coop::host::GameFlags(memory).get_string64(RequestFlags().start);
    }
    const bool start_ok =
        coop::host::GameFlags::set_string64_now(memory, RequestFlags().start, start);
    if (g.arrive_event) {
        // As a loading zone leaves them (docs/scenes.md, the probe of a real
        // exit: boot="Dm_SY_BancChangeArrive", starter = the start name).
        coop::host::GameFlags::set_string64_now(memory, RequestFlags().boot, kArriveEvent);
        coop::host::GameFlags::set_string64_now(memory, RequestFlags().starter, start);
    }
    return start_ok && coop::host::GameFlags::set_string64_now(memory, RequestFlags().banc, scene);
}

void ClearScene(Glue& g, GuestMemory& memory, bool restore) {
    const bool banc = coop::host::GameFlags::set_string64_now(memory, RequestFlags().banc, "");
    if (g.arrive_event && restore) {
        coop::host::GameFlags::set_string64_now(memory, RequestFlags().boot, "");
        coop::host::GameFlags::set_string64_now(memory, RequestFlags().starter, "");
    }
    std::string start = "kept";
    if (restore && g.saved_start) {
        start = coop::host::GameFlags::set_string64_now(memory, RequestFlags().start, *g.saved_start)
                    ? "put back to \"" + *g.saved_start + "\""
                    : "NOT put back";
    }
    g.Log("teleport: scene change cleared: request %s, start %s", banc ? "emptied" : "NOT emptied",
          start.c_str());
}

// One frame of the game thread's side, before any call of this frame.
void Logic(Glue& g, CoopPuppet::Guest& guest, std::uint64_t main) {
    const std::uint64_t now = SteadyUs();
    CoopTeleport::Local local;
    std::vector<Peer> peers;
    bool self_host = false;
    bool connected = false;
    std::optional<std::pair<std::uint8_t, std::string>> want;
    std::vector<Answered> answers;
    {
        std::lock_guard lock(g.mutex);
        local = g.local;
        peers = g.peers;
        self_host = g.self_host;
        connected = g.connected;
        want = std::exchange(g.want, std::nullopt);
        answers = std::exchange(g.answers, {});
        if (std::exchange(g.died, false) && connected) {
            g.automatic.Died(now);
        }
    }
    GuestMemory memory(guest, main);
    const Trip::Phase phase = g.trip.phase();
    if (phase == Trip::Phase::travelling || phase == Trip::Phase::settling) {
        coop::host::GameFlags flags(memory);
        local.reserved_banc = flags.get_string64(RequestFlags().banc);
        local.loading_banc = flags.get_bool(RequestFlags().loading).value_or(false);
        // The new scene has settled only once the game's scene start is over,
        // which is when the game itself has emptied the request: until then it
        // counts as still loading (the trip's 1.5 s of standing start after it),
        // so our move next to the player never lands inside the scene start.
        if (phase == Trip::Phase::settling && g.taken && local.reserved_banc &&
            !local.reserved_banc->empty()) {
            local.loading_banc = true;
        }
    }

    if (!g.gotos.empty() && local.scene) {
        if (g.in_game_us == 0) {
            g.in_game_us = now;
        }
        for (auto it = g.gotos.begin(); it != g.gotos.end(); ++it) {
            if (now - g.in_game_us >= it->after_us) {
                g.Log("teleport: test goto %.1f %.1f %.1f", static_cast<double>(it->to.x),
                      static_cast<double>(it->to.y), static_cast<double>(it->to.z));
                g.move = it->to;
                g.gotos.erase(it);
                break;
            }
        }
    }

    if (g.test_scene && local.scene && main != 0 && g.trip.phase() == Trip::Phase::idle) {
        if (g.scene_in_game_us == 0) {
            g.scene_in_game_us = now;
        }
        if (now - g.scene_in_game_us >= g.test_scene->after_us && !local.event) {
            const bool asked = AskScene(g, memory, g.test_scene->scene, g.test_scene->start);
            g.Log("teleport: test scene change to %s at %s: %s", g.test_scene->scene.c_str(),
                  g.test_scene->start.c_str(), asked ? "asked" : "NOT asked");
            if (asked) {
                g.events.Want(Trip::kSceneEvent);
            }
            g.saved_start.reset();  // the game takes the request and empties it itself
            g.saved_read = false;
            g.test_scene.reset();
        }
    }

    ++g.frames;
    if (g.trip.phase() == Trip::Phase::idle) {
        g.event_frame = 0;  // a new trip starts its event at once
    }
    std::vector<Trip::Actions> all;
    bool quiet = false;  // a retry: its "asking" line is not said again
    if (!connected) {
        g.automatic.Reset();
    } else if (const auto go = g.automatic.Tick(local.scene, self_host, peers, now);
               go && !want && g.trip.phase() == Trip::Phase::idle) {
        if (go->first) {
            g.Line(go->why == Auto::Why::join
                       ? "teleport: joining " + go->name + " (the host)"
                       : go->why == Auto::Why::regroup
                             ? "teleport: back from Game Over; going to " + go->name
                             : "teleport: " + go->name + " took an exit; following");
        }
        quiet = true;
        g.trip_auto = true;
        all.push_back(g.trip.Start(go->player, go->name, local, now));
        if (!all.back().ask) {
            all.back().lines.clear();  // our own event or load: tried again soon
        }
    }
    if (want) {
        g.trip_auto = false;
        all.push_back(g.trip.Start(want->first, want->second, local, now));
    }
    for (const auto& a : answers) {
        const bool ours = g.trip_auto && a.player == g.trip.player() &&
                          g.trip.phase() == Trip::Phase::asking;
        switch (a.kind) {
        case Answered::Kind::accepted:
            g.target_facing = a.target.rotation;
            g.target_at = CoopTeleport::Vec{a.target.position[0], a.target.position[1], a.target.position[2]};
            all.push_back(g.trip.Accepted(a.player, a.target, local, now));
            // Moving, travelling, or refused for good (another age's scene,
            // no start point there): that want is done. "Your game got busy"
            // (no move, no scene asked, our Link gone or in an event) is
            // tried again.
            if (ours) {
                if (all.back().move || all.back().scene || (local.scene && !local.event)) {
                    g.automatic.Done(a.player);
                } else {
                    all.back().lines.clear();
                }
            }
            break;
        case Answered::Kind::refused:
            all.push_back(g.trip.Refused(a.player, a.reason));
            if (ours) {
                if (a.reason == coop::protocol::JoinRefuseReason::busy) {
                    all.back().lines.clear();  // tried again soon
                } else {
                    g.automatic.Done(a.player);
                }
            }
            break;
        case Answered::Kind::timed_out:
            all.push_back(g.trip.TimedOut(a.player));
            if (ours) {
                all.back().lines.clear();
            }
            break;
        case Answered::Kind::left:
            all.push_back(g.trip.Left(a.player));
            break;
        }
    }
    all.push_back(g.trip.Tick(local, now));

    for (auto& act : all) {
        if (act.cancel_scene && main != 0) {
            if (g.taken) {
                // Arrived, or stopped after the game took the request: nothing
                // is written (see `taken`); the game empties its own request.
                g.Log("teleport: the scene change is the game's now: its request flags are left "
                      "to the game (it empties them at the end of its scene start)");
            } else {
                ClearScene(g, memory, act.restore_start);
            }
            g.saved_start.reset();  // the next trip reads it afresh
            g.saved_read = false;
        }
        if (act.release) {
            g.taken = true;
            g.Log("teleport: the game took the scene change");
        }
        if (act.scene) {
            g.taken = false;  // written (again): the request is ours until the game takes it
            // Always with a start point of that scene: without one the game
            // keeps the old scene's start name and the picture stays black.
            const bool asked =
                main != 0 && !act.start.empty() && AskScene(g, memory, *act.scene, act.start);
            if (!asked || now - g.said_ask_us > 3'000'000) {
                g.said_ask_us = now;
                g.Log("teleport: scene change to %s at %s: %s", act.scene->c_str(),
                      act.start.empty() ? "-" : act.start.c_str(), asked ? "asked" : "NOT asked");
            }
        }
        // The trip asks for the event again every 3 s of the clock until the
        // game has taken the request. A slow game needs longer than that for
        // the first one (seen: 4.5 s at 4 frames a second), and a second event
        // asked meanwhile ran into the load: the new scene then stayed black.
        // So again only after kEventAgainFrames game frames as well.
        if (act.start_event && main != 0 && g.event_frame != 0 &&
            g.frames - g.event_frame < kEventAgainFrames) {
            act.start_event = false;
        }
        if (act.start_event && main != 0) {
            g.event_frame = g.frames;
            g.Log("teleport: starting the event %s so the game takes the scene change",
                  Trip::kSceneEvent);
            g.events.Want(Trip::kSceneEvent);
        }
        if (act.move) {
            g.move = *act.move;
            g.move_facing.reset();
            // Next to the player, not on them: a little behind and aside of
            // where they stood, facing as they did.
            const float dx = act.move->x - g.target_at.x;
            const float dy = act.move->y - g.target_at.y;
            const float dz = act.move->z - g.target_at.z;
            if (g.target_facing && dx * dx + dy * dy + dz * dz < 0.01f) {
                const auto& q = *g.target_facing;
                // Where their rotation takes the actor's +Z (Link's front), on the ground.
                float fx = 2.0f * (q[0] * q[2] + q[3] * q[1]);
                float fz = 1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]);
                const float length = std::sqrt(fx * fx + fz * fz);
                if (std::isfinite(length) && length > 0.1f) {
                    fx /= length;
                    fz /= length;
                    g.move->x += -fx * kArriveBehind + fz * kArriveAside;
                    g.move->z += -fz * kArriveBehind - fx * kArriveAside;
                    g.move_facing = coop::protocol::Quat{0.0f, std::sin(std::atan2(fx, fz) / 2.0f), 0.0f,
                                                         std::cos(std::atan2(fx, fz) / 2.0f)};
                }
            }
        }
        if (act.ask) {
            std::lock_guard lock(g.mutex);
            g.asks.push_back(*act.ask);
        }
        for (auto& line : act.lines) {
            if (quiet && line.find("asking") != std::string::npos) {
                continue;
            }
            g.Line(std::move(line));
        }
    }
}

}  // namespace

void Configure(const Auto::Options& options, void (*log)(const char* line)) {
    Glue& g = TheGlue();
    g.log = log;
    g.automatic = Auto(options);
    g.configured = true;
    g.Log("teleport: on (/teleport <player> in chat); join at the host: %s; follow through "
          "exits: %s",
          options.join_at_host ? "on" : "off",
          options.follow == FollowMode::host     ? "the host"
          : options.follow == FollowMode::anyone ? "anyone"
                                                 : "off");
}

void Died() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    g.died = true;
}

void SetArriveEvent(bool on) {
    TheGlue().arrive_event = on;
}

void TestGoto(std::string_view spec) {
    Glue& g = TheGlue();
    while (!spec.empty()) {
        const auto end = spec.find(';');
        const std::string part(spec.substr(0, end));
        spec = end == std::string_view::npos ? std::string_view{} : spec.substr(end + 1);
        double at = 0.0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        if (std::sscanf(part.c_str(), "%lf:%f,%f,%f", &at, &x, &y, &z) == 4 && at >= 0.0) {
            g.gotos.push_back({static_cast<std::uint64_t>(at * 1e6), CoopTeleport::Vec{x, y, z}});
        }
    }
    if (!g.gotos.empty()) {
        g.Log("teleport: %u test moves of our own Link (OOT_COOP_TEST_GOTO)",
              static_cast<unsigned>(g.gotos.size()));
    }
}

void TestScene(std::string_view spec) {
    Glue& g = TheGlue();
    const std::string text(spec);
    const auto first = text.find(':');
    const auto second = first == std::string::npos ? first : text.find(':', first + 1);
    if (second == std::string::npos || second + 1 >= text.size()) {
        return;
    }
    Glue::Scene scene;
    scene.after_us = static_cast<std::uint64_t>(std::atof(text.substr(0, first).c_str()) * 1e6);
    scene.scene = text.substr(first + 1, second - first - 1);
    scene.start = text.substr(second + 1);
    g.Log("teleport: a test scene change of our own game to %s at %s (OOT_COOP_TEST_SCENE)",
          scene.scene.c_str(), scene.start.c_str());
    g.test_scene = std::move(scene);
}

bool OnChat(std::string_view text, coop::host::Session& session) {
    const auto command = CoopTeleport::Parse(text);
    if (!command) {
        return false;
    }
    Glue& g = TheGlue();
    std::string line;
    std::optional<std::pair<std::uint8_t, std::string>> want;
    session.with_client([&](coop::net::Client& client) {
        if (client.state() != coop::net::State::connected) {
            line = "teleport: not connected";
            return;
        }
        const auto players = Players(client);
        const std::uint8_t self = client.player_number();
        if (command->kind == CoopTeleport::Command::Kind::list) {
            line = CoopTeleport::ListLine(players, self);
            return;
        }
        const CoopTeleport::Found found = CoopTeleport::FindPlayer(command->name, players, self);
        if (found.number == 0) {
            line = found.error;
            return;
        }
        want = std::make_pair(found.number, found.name);
    });
    if (want) {
        std::lock_guard lock(g.mutex);
        if (g.configured && g.game_thread) {
            g.want = std::move(want);
            return true;
        }
        line = "teleport: not possible in this game (it has no plugin interface)";
    }
    g.Line(std::move(line));
    return true;
}

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    using coop::net::EventKind;
    Glue& g = TheGlue();
    if (!g.configured) {
        return;
    }
    std::lock_guard lock(g.mutex);
    switch (event.kind) {
    case EventKind::join_request: {
        std::string name = "P" + std::to_string(event.player);
        for (const auto& p : client.players()) {
            if (p.number == event.player && !p.nickname.empty()) {
                name = p.nickname;
            }
        }
        const CoopTeleport::Answer a = g.answer;
        if (a.accept) {
            const bool sent = client.accept_join(event.player, event.request_id, a.target);
            g.Log("teleport: %s asked to come here: %s at %.1f %.1f %.1f%s", name.c_str(),
                  a.target.scene.c_str(), static_cast<double>(a.target.position[0]),
                  static_cast<double>(a.target.position[1]),
                  static_cast<double>(a.target.position[2]), sent ? "" : " (not sent)");
            if (sent) {
                g.lines.push_back("teleport: " + name + " is coming to you");
            }
        } else {
            client.refuse_join(event.player, event.request_id, a.reason);
            g.Log("teleport: %s asked to come here: refused, reason %d", name.c_str(),
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

void Frame(const std::optional<coop::host::PlayerReading>& reading, coop::host::Session& session,
           bool connected, std::uint64_t server_now_us, bool game_thread) {
    Glue& g = TheGlue();
    if (!g.configured) {
        return;
    }
    const std::uint64_t now = SteadyUs();
    CoopTeleport::Local local;
    if (reading) {
        local.scene = reading->scene;
        local.position =
            CoopTeleport::Vec{reading->position[0], reading->position[1], reading->position[2]};
        local.rotation = reading->rotation;
        local.age = reading->age;
        local.event = reading->event_running.value_or(false);
    }
    g.grounded.Add(local, now);

    // Who is where: the room from the client, then the scenes from presence
    // (one lock after the other, never both).
    struct Room {
        std::uint8_t self = 0;
        std::uint8_t host = 0;
        std::vector<coop::net::Player> players;
    };
    std::vector<Peer> peers;
    bool self_host = false;
    if (connected) {
        const Room room = session.with_client([](coop::net::Client& client) {
            return Room{client.player_number(), client.host_number(), client.players()};
        });
        self_host = room.host != 0 && room.host == room.self;
        session.with_presence([&](coop::presence::Presence& presence) {
            for (const auto& player : room.players) {
                if (player.self) {
                    continue;
                }
                Peer peer;
                peer.number = player.number;
                peer.name = player.nickname.empty() ? "P" + std::to_string(player.number)
                                                    : player.nickname;
                peer.host = player.number == room.host;
                if (const auto* p = presence.player(player.number)) {
                    peer.scene = p->scene();
                    if (!p->samples().empty()) {
                        const std::uint64_t last = p->samples().back().time.server_time_us;
                        peer.fresh =
                            server_now_us >= last ? server_now_us - last <= kFreshUs : true;
                    }
                }
                peers.push_back(std::move(peer));
            }
        });
    }

    std::vector<std::uint8_t> asks;
    {
        std::lock_guard lock(g.mutex);
        g.answer =
            game_thread ? CoopTeleport::AnswerFor(local, g.grounded) : CoopTeleport::Answer{};
        g.local = std::move(local);
        g.peers = std::move(peers);
        g.self_host = self_host;
        g.connected = connected;
        g.game_thread = game_thread;
        asks = std::exchange(g.asks, {});
    }
    for (const std::uint8_t to : asks) {
        const bool sent = session.with_client(
            [&](coop::net::Client& c) { return c.request_join(to).has_value(); });
        g.Log("teleport: asked player %u where they are%s", static_cast<unsigned>(to),
              sent ? "" : " (not sent)");
        if (!sent) {
            std::lock_guard lock(g.mutex);
            g.answers.push_back({Answered::Kind::timed_out, to, {}, {}});
        }
    }
}

std::vector<std::string> TakeLines() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.lines, {});
}

std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                     std::uint64_t main_base) {
    Glue& g = TheGlue();
    if (!g.configured || main_base == 0) {
        return std::nullopt;
    }
    if (after_call) {
        if (!g.in_events) {
            return std::nullopt;  // the move's call: nothing follows it
        }
        auto call = g.events.Step(guest, true, result, main_base);
        for (const auto& note : g.events.TakeNotes()) {
            g.Log("teleport: %s", note.c_str());
        }
        g.in_events = call.has_value();
        return call;
    }
    Logic(g, guest, main_base);
    if (g.move) {
        const CoopTeleport::Vec to = *g.move;
        const auto facing = std::exchange(g.move_facing, std::nullopt);
        g.move.reset();
        GuestMemory memory(guest, main_base);
        coop::host::PlayerReader reader(memory);
        const auto actor = reader.find_player_actor();
        std::uint32_t state = 0;
        const float place[3] = {to.x, to.y, to.z};
        if (!actor || !guest.Read(*actor + kActorState, &state, 4) || state != kActorStatePresent ||
            !guest.Write(*actor + kActorPosition, place, sizeof(place))) {
            g.Line("teleport: could not move Link (no Link in the game right now)");
        } else {
            bool turned = false;
            if (facing) {
                // A turn about the vertical: rows of the matrix that takes local to world.
                const float s = 2.0f * (*facing)[1] * (*facing)[3];
                const float c = 1.0f - 2.0f * (*facing)[1] * (*facing)[1];
                const float rows[9] = {c, 0.0f, s, 0.0f, 1.0f, 0.0f, -s, 0.0f, c};
                turned = guest.Write(*actor + kActorRotation, rows, sizeof(rows));
            }
            g.Log("teleport: moved Link to %.1f %.1f %.1f%s", static_cast<double>(to.x),
                  static_cast<double>(to.y), static_cast<double>(to.z),
                  turned ? " (beside the player, facing as they do)" : "");
#if NX2_PLUGIN_API_VERSION >= 5
            // Interface version 5: the game's own camera reset (the player's
            // camera button, pressed by the runtime at the game's next read of
            // its controls), so the follow camera comes round behind Link at
            // once instead of sitting at his head or in front of his face
            // where there is a wall or a ledge behind (runs f3, f4).
            if (turned) {
                const Nx2PluginApi* api = CoopNx2navi::Overlay::Api();
                // The size must be this header's own: version 5 was still being
                // built when this was written (NOTES.md), and a table of
                // another length is not known to have this member here.
                if (api != nullptr && api->version == 5 && api->size == sizeof(Nx2PluginApi) &&
                    api->camera_reset != nullptr) {
                    g.Log(api->camera_reset() == 1
                              ? "teleport: the camera is put behind Link (the plugin interface's "
                                "camera_reset)"
                              : "teleport: the game has no camera reset for this version; the camera "
                                "comes round by itself");
                }
            }
#endif
            CoopPuppet::Call c;
            c.function = main_base + kSetMatrix;
            c.x[0] = *actor;
            c.x[1] = 0;
            g.in_events = false;
            return c;
        }
    }
    auto call = g.events.Step(guest, false, 0, main_base);
    for (const auto& note : g.events.TakeNotes()) {
        g.Log("teleport: %s", note.c_str());
    }
    g.in_events = call.has_value();
    return call;
}

}  // namespace CoopNx2navi::Teleport
