// SPDX-License-Identifier: GPL-3.0-or-later
//
// Boss rooms together, the emulator side (coop_boss.h, docs/dungeons-boss.md).
// Build e0f3ccfaf50065ae; offsets into main.
//
// Moving Link into the boss room, the game's own ways:
// - While our own event runs, the sequence's scene change takes a request
//   (as tools/autotest's warp): String64 Sequence_ReservedNextStartPosName =
//   the room's StartPos, then Sequence_ReservedNextBanc = the dungeon's scene.
//   The dungeon loads again with Link at that StartPos.
// - Otherwise (free play, where the scene change is not taken), what the AI
//   node ExecuteActorForceSetMtx does (run 0x2622e50): write the actor's
//   place (+0x31c), then call vtable slot 0x218 = 0x4c6580(actor, 0), the
//   engine's warp of the whole physics body (docs/world.md). Same scene, so
//   no load. Called from the svc cave on the game's calc thread.
// Skipping the intro: SeqOneShotEventRequestForceSkip (0x32d1890) does
//   E = event manager; if IsEventRunning(E): S = [E+0x68],
//   event = [[S+0xeb8] + 8 * u32 [S+0xf18]]; 0x3b3af30(event).
// We do the same from the cave.

#include "coop_boss.h"
#include "coop_boss_beaten.h"
#include "coop_caller_orders.h"
#include "coop_boss_native.h"
#include "coop_boss_scenes.h"
#include "coop_live_events.h"
#include "coop_session_gate.h"
#include "coop_objects.h"

#include <cmath>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "common/logging.h"
#include "core/core.h"
#include "core/memory.h"
#include "coop/host/flags.hpp"
#include "coop/host/game_flags.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_flags.h"
#include "coop_puppet.h"
#include "coop_svc.h"

namespace CoopBoss {
namespace {

constexpr std::uint64_t kEventManagerPtr = 0x4e94840;
constexpr std::uint64_t kEventSession = 0x68;
constexpr std::uint64_t kSessionEvents = 0xeb8;
constexpr std::uint64_t kSessionCount = 0xeb0;
constexpr std::uint64_t kSessionCurrent = 0xf18;
constexpr std::uint64_t kForceSkip = 0x3b3af30;
constexpr std::uint64_t kActorState = 0x2c;
constexpr std::uint32_t kActorStatePresent = 4;
constexpr std::uint64_t kActorPosition = 0x31c;
constexpr std::uint64_t kActorRotation = 0x328;  // 9 x f32, rows of 3 (player-state-map.md)
constexpr std::uint64_t kSetMatrix = 0x4c6580;  // vtable slot 0x218 of every actor seen
constexpr int kCallerOrder = CoopCallerOrders::kBossRooms;  // after the puppets, spawn, fairy, objects, time
// Ten times a second on the steady clock, not by presented frames (issue
// 106: with the emulator's speed-up, frames come faster).
constexpr std::uint64_t kTickEveryUs = 100'000;
constexpr std::uint64_t kDebugEveryUs = 1'000'000;

std::string Env(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

// The calc thread side: moves Link and skips the event when asked.
class Mover {
public:
    void WantMove(std::uint64_t main, std::uint64_t actor, Vec to) {
        std::lock_guard lock(mutex_);
        main_ = main;
        move_ = Move{actor, to};
    }
    // Test only (`face X Z`, row 175): our Link turned to face the world point (x, z), its rotation (actor
    // +0x328, rows of 3; column 2 is the way it faces, as CoopObjects reads it) written as a plain turn about
    // the up axis, then its matrix set from it as a move does.
    void WantFace(std::uint64_t main, std::uint64_t actor, float x, float z) {
        std::lock_guard lock(mutex_);
        main_ = main;
        face_ = Face{actor, x, z};
    }
    void WantSkip(std::uint64_t main) {
        std::lock_guard lock(mutex_);
        main_ = main;
        skip_ = true;
    }

    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call) {
        std::lock_guard lock(mutex_);
        if (after_call || main_ == 0) {
            return std::nullopt;
        }
        if (move_) {
            const Move m = *move_;
            move_.reset();
            std::uint32_t state = 0;
            if (!guest.Read(m.actor + kActorState, &state, 4) || state != kActorStatePresent) {
                LOG_WARNING(Core, "coop: boss: Link's actor is gone; not moved");
            } else {
                const float place[3] = {m.to.x, m.to.y, m.to.z};
                if (guest.Write(m.actor + kActorPosition, place, sizeof(place))) {
                    CoopPuppet::Call c;
                    c.function = main_ + kSetMatrix;
                    c.x[0] = m.actor;
                    c.x[1] = 0;
                    LOG_INFO(Core, "coop: boss: moved Link to {:.1f} {:.1f} {:.1f}", m.to.x, m.to.y, m.to.z);
                    return c;
                }
            }
        }
        if (face_) {
            const Face f = *face_;
            face_.reset();
            std::uint32_t state = 0;
            float at[3] = {};
            if (!guest.Read(f.actor + kActorState, &state, 4) || state != kActorStatePresent ||
                !guest.Read(f.actor + kActorPosition, at, sizeof(at))) {
                LOG_WARNING(Core, "coop: test face: Link's actor is gone; not turned");
            } else {
                const float yaw = std::atan2(f.x - at[0], f.z - at[2]);
                const float s = std::sin(yaw);
                const float c = std::cos(yaw);
                const float m[9] = {c, 0.0f, s, 0.0f, 1.0f, 0.0f, -s, 0.0f, c};
                if (guest.Write(f.actor + kActorRotation, m, sizeof(m))) {
                    CoopPuppet::Call call;
                    call.function = main_ + kSetMatrix;
                    call.x[0] = f.actor;
                    call.x[1] = 0;
                    LOG_INFO(Core, "coop: test face {:.2f} {:.2f}: Link at {:.2f} {:.2f} turned to yaw {:.1f} deg",
                             f.x, f.z, at[0], at[2], yaw * 57.29578f);
                    return call;
                }
            }
        }
        if (skip_) {
            skip_ = false;
            std::uint64_t manager = 0;
            std::uint64_t session = 0;
            std::uint64_t events = 0;
            std::uint32_t count = 0;
            std::uint32_t current = 0;
            std::uint64_t event = 0;
            if (guest.Read(main_ + kEventManagerPtr, &manager, 8) && manager != 0 &&
                guest.Read(manager + kEventSession, &session, 8) && session != 0 &&
                guest.Read(session + kSessionEvents, &events, 8) && events != 0 &&
                guest.Read(session + kSessionCount, &count, 4) &&
                guest.Read(session + kSessionCurrent, &current, 4) && current < count &&
                guest.Read(events + 8ull * current, &event, 8) && event != 0) {
                CoopPuppet::Call c;
                c.function = main_ + kForceSkip;
                c.x[0] = event;
                LOG_INFO(Core, "coop: boss: force skip of the event {:#x}", event);
                return c;
            }
            LOG_INFO(Core, "coop: boss: no event to skip");
        }
        return std::nullopt;
    }

private:
    struct Move {
        std::uint64_t actor = 0;
        Vec to;
    };
    struct Face {
        std::uint64_t actor = 0;
        float x = 0.0f;
        float z = 0.0f;
    };
    std::optional<Face> face_;
    std::mutex mutex_;
    std::uint64_t main_ = 0;
    std::optional<Move> move_;
    bool skip_ = false;
};

struct Glue {
    bool on = true;
    bool by_scene = true;  // LICHII_COOP_BOSS_MOVE=matrix: never
    bool debug = false;
    std::uint64_t tick_at = 0;  // steady clock, us
    CoopSessionGate::Gate gate;  // the session's end seen before the tick (coop_session_gate.h)
    bool unready_logged_ = true;  // the last state logged: ready
    Readiness readiness;          // boss-copy-b: may our game own the room's boss
    std::uint64_t debug_at = 0;
    Sync sync;
    Mover mover;
    // Boss cutscenes for the area (coop_boss_scenes.h): on where our game's own event requests are seen by name
    // (Hookshot's event request wrap); LICHII_COOP_BOSS_SCENES=0 turns it off (the older rules then move and skip).
    CoopBossScenes::Director scenes;
    bool scenes_on = true;
    bool scenes_live = false;  // logged once
    std::uint64_t requests_cursor = 0;
    std::optional<std::int32_t> max_life;          // set by the hook (SetMaxLife)
    std::optional<std::int32_t> max_life_write;    // for the hook (TakeMaxLifeWrite)
    std::optional<std::int32_t> max_life_cap;      // for the hook (MaxLifeCap)
    std::vector<std::string> wanted;               // events for the live-event requester (TakeWantedEvents)
    std::mutex searched_mutex;
    std::vector<std::string> searched;             // our boss intro/death read with no name (Sync's intro_here/beaten_here)
    std::vector<std::string> lines;                // chat lines (TakeLines)

    Glue() {
        on = Env("LICHII_COOP_BOSS") != "0";
        scenes_on = Env("LICHII_COOP_BOSS_SCENES") != "0";
        by_scene = Env("LICHII_COOP_BOSS_MOVE") != "matrix";
        const auto d = Env("LICHII_COOP_DEBUG_BOSS");
        debug = !d.empty() && d != "0";
        if (on) {
            CoopSvc::AddCaller(kCallerOrder, [this](CoopPuppet::Guest& guest, bool after_call,
                                                    std::uint64_t, std::uint64_t) {
                return mover.Step(guest, after_call);
            });
        }
        LOG_INFO(Core, "coop: boss rooms {}", on ? "shared" : "off (LICHII_COOP_BOSS=0)");
    }

    // The game's scene change, as tools/autotest's warp asks it.
    bool AskScene(Core::System& system, std::uint64_t main, const Room& room) {
        const bool banc = SetScene(system, main, room.scene, room.start);
        LOG_INFO(Core, "coop: boss: asked the scene change to {} at {}: {}", room.scene, room.start,
                 banc ? "set" : "NOT set");
        return banc;
    }

    static bool SetScene(Core::System& system, std::uint64_t main, std::string_view scene,
                         std::string_view start) {
        CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main);
        // No start: left alone, as tools/autotest's warp does.
        const bool start_ok = start.empty() || coop::host::GameFlags::set_string64_now(
                                                   memory, coop::host::flag_hash("Sequence_ReservedNextStartPosName"),
                                                   start);
        return start_ok && coop::host::GameFlags::set_string64_now(
                               memory, coop::host::flag_hash("Sequence_ReservedNextBanc"), scene);
    }

    bool AskMove(Core::System& system, std::uint64_t main, const Vec& to) {
        CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main);
        coop::host::PlayerReader reader(memory);
        const auto actor = reader.find_player_actor();
        if (!actor) {
            LOG_INFO(Core, "coop: boss: Link's actor not found; not moved");
            return false;
        }
        mover.WantMove(main, *actor, to);
        return true;
    }

    bool AskFace(Core::System& system, std::uint64_t main, float x, float z) {
        CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main);
        coop::host::PlayerReader reader(memory);
        const auto actor = reader.find_player_actor();
        if (!actor) {
            LOG_INFO(Core, "coop: test face: Link's actor not found; not turned");
            return false;
        }
        mover.WantFace(main, *actor, x, z);
        return true;
    }

    bool Scenes(Core::System& system, std::uint64_t main, const Local& local, bool event,
                coop::host::Session* session, std::uint64_t now_us);

    void Frame(Core::System& system, std::uint64_t main, const std::optional<std::string>& scene,
               const std::optional<coop::protocol::Vec3>& position, bool event, std::optional<bool> shut_in,
               coop::host::Session* session, std::uint64_t now_us) {
        if (!on) {
            return;
        }
        const auto steady_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        const auto step = gate.Next(main != 0 && now_us != 0 && session != nullptr, [&] {
            if (tick_at != 0 && steady_us - tick_at < kTickEveryUs) {
                return false;
            }
            tick_at = steady_us;
            return true;
        });
        if (step == CoopSessionGate::Step::forget) {
            sync.Reset();
            scenes.Reset();
            CoopBossBeaten::TheRecord().ForgetSession();  // out of the session: the server tells us again on a join
            CoopBossBeaten::TheHostAlive().Clear();
            CoopBossScenes::RefusedDeaths().store(0, std::memory_order_release);
            readiness.Reset();
            max_life_cap.reset();
        }
        if (step != CoopSessionGate::Step::join && step != CoopSessionGate::Step::run) {
            return;
        }
        Local local;
        local.scene = scene;
        if (position) {
            local.position = Vec{(*position)[0], (*position)[1], (*position)[2]};
        }
        local.event = event;
        local.shut_in = shut_in;
        if (scene) {
            if (const Room* room = RoomOfScene(*scene)) {
                local.boss_alive = CoopObjects::LastBossAlive(Bodies(*room));
                local.boss_read_dead = CoopObjects::LastBossReadDead(Bodies(*room));
                local.boss_hurt = CoopObjects::LastBossHurt(Bodies(*room));
                // A Game Over here is no boss's death (boss-gameover-b): Link's own Life at 0.
                if (const auto life = CoopObjects::OurLinkLife()) {
                    local.link_dead = *life <= 0;
                }
                // Not ready to own the boss until our game has played its
                // intro: before it our boss is still in its pre-battle phase
                // (Gohma on the ceiling), and as owner it would hold every
                // game's boss there (the eye shut, no fight).
                CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main);
                coop::host::GameFlags flags(memory);
                auto intro = flags.get_bool(coop::host::flag_hash("HavePlayedEvent." + std::string(room->intro)));
                // Our own intro ended here (not skipped): played, though the game sets that flag later or never
                // (Gohma's), else the intro's own owner reads unready and the server hands her away.
                if (sync.OwnIntroPlayed()) {
                    intro = true;
                }
                // boss-copy-b: also ready once our boss left its pre-battle
                // phase here (a late joiner's copy following the fight).
                const bool ready =
                    readiness.Step(room->index, intro, CoopObjects::LastBossPreBattle(Bodies(*room)));
                if (readiness.TakeFoughtNews() && intro.has_value() && !*intro) {
                    LOG_INFO(Core, "coop: boss: our boss left its pre-battle phase here: ready to own it without our intro");
                }
                std::vector<std::string> unready;
                if (!ready) {
                    for (const auto body : Bodies(*room)) unready.emplace_back(body);
                }
                if (unready.empty() != unready_logged_) {
                    unready_logged_ = unready.empty();
                    LOG_INFO(Core, "coop: boss: {} ({})",
                             unready.empty() ? "ready to own the boss" : "not ready to own the boss: its intro is not played here",
                             room->intro);
                }
                CoopObjects::SetUnreadyBosses(std::move(unready));
            } else {
                readiness.Reset();  // out of the boss room: her next visit starts over
                CoopObjects::SetUnreadyBosses({});
            }
        }
        local.self = session->with_client([](coop::net::Client& c) { return c.player_number(); });
        Actions act = sync.Tick(local, now_us);
        // With the area's rules on (Scenes below returns true then) the older dungeon-wide pull is dropped, so its
        // "moving in (try N)" / "could not move into" lines would tell of a move that never happens (big-run-0115
        // hs-gohma-native: B at the dungeon's entrance, outside the boss's area, logged 5 tries and stayed put).
        const bool area_rules = scenes_on && CoopLiveEvents::TheGameRequests().Live();
        for (const auto& line : act.log) {
            if (area_rules && (line.rfind("another player is in the boss room of ", 0) == 0 ||
                               line.rfind("could not move into the boss room of ", 0) == 0)) {
                continue;
            }
            LOG_INFO(Core, "coop: boss: {}", line);
        }
        {
            // Gohma's intro is no event the game asks by name (only the door's ShutIn is): the older rules' own
            // reading of our event in the boss room tells the area rules, guarded as a search's find.
            std::lock_guard lock(searched_mutex);
            if (act.intro_here != nullptr) {
                searched.emplace_back(act.intro_here->intro);
            }
            if (act.beaten_here != nullptr) {
                searched.emplace_back(act.beaten_here->outro);
            }
        }
        if (Scenes(system, main, local, event, session, now_us)) {
            // The area's rules move and skip (boss-cutscenes): the older dungeon-wide pull and its "any intro
            // over" skip would move players from anywhere in the dungeon.
            act.pull.reset();
            act.skip_intro = false;
        }
        if (debug && now_us - debug_at >= kDebugEveryUs) {
            debug_at = now_us;
            LOG_INFO(Core, "coop: boss: phase {}, scene {}, at {:.1f} {:.1f} {:.1f}, event {}, shut in {}",
                     PhaseName(sync.phase()), scene.value_or("?"), position ? (*position)[0] : 0.0f,
                     position ? (*position)[1] : 0.0f, position ? (*position)[2] : 0.0f, event,
                     shut_in ? (*shut_in ? "1" : "0") : "-");
        }
        if (act.pull) {
            const Room& room = *act.pull->room;
            bool asked = false;
            if (act.pull->by_event && by_scene && !room.start.empty()) {
                asked = AskScene(system, main, room);
            }
            if (!asked) {
                AskMove(system, main, act.pull->to);
            }
        }
        if (act.skip_intro) {
            mover.WantSkip(main);
        }
        if (!act.send.empty()) {
            session->with_client([&](coop::net::Client& c) {
                for (auto& m : act.send) {
                    const bool sent = c.send(coop::protocol::Message{m});
                    if (debug || m.state != coop::protocol::BossState::in_room) {
                        LOG_INFO(Core, "coop: boss: sent {} for {}{}", static_cast<int>(m.state), m.scene,
                                 sent ? "" : " (not sent)");
                    }
                }
            });
        }
        if (!act.chat.empty()) {  // "<name> took the blue warp" (#50); the chat draws it in the menu's language
            session->with_client([&](coop::net::Client& c) {
                for (auto& line : act.chat) {
                    const auto* p = c.find_player(line.player);
                    lines.push_back((p != nullptr ? p->nickname : "player " + std::to_string(line.player)) + " " +
                                    line.text);
                }
                return 0;
            });
        }
    }
};

// Boss cutscenes (coop_boss_scenes.h). True when its rules are the ones in force.
bool Glue::Scenes(Core::System& system, std::uint64_t main, const Local& local, bool event,
                  coop::host::Session* session, std::uint64_t now_us) {
    auto& requests = CoopLiveEvents::TheGameRequests();
    if (!scenes_on || !requests.Live()) {
        max_life_cap.reset();
        CoopBossNative::TheTable().SetDying({});
        CoopObjects::SetDyingBosses({});
        CoopBossScenes::RefusedDeaths().store(0, std::memory_order_release);  // (no Director: nothing refused)
        std::lock_guard lock(searched_mutex);
        searched.clear();  // nothing takes them: none applied late when the rules come on
        return false;
    }
    if (!scenes_live) {
        scenes_live = true;
        LOG_INFO(Core, "coop: boss: intros and deaths are shared with the boss's area (our game's event requests "
                       "are seen by name)");
    }
    std::vector<CoopBossScenes::Request> ours;
    for (const auto& r : requests.Since(requests_cursor)) {
        if (debug) {
            // Every event the game asked, as the wrap names it (the boss's own may be a setting, not its flow).
            LOG_INFO(Core, "coop: boss: the game asked the event '{}' ({})", r.name, r.taken ? "taken" : "not taken");
        }
        ours.push_back({r.name, r.taken});
    }
    {
        std::lock_guard lock(searched_mutex);
        for (auto& name : searched) {
            LOG_INFO(Core, "coop: boss: our game runs {} (seen without a request naming it)", name);
            ours.push_back({std::move(name), true, true});
        }
        searched.clear();
    }
    CoopBossScenes::Local here;
    here.scene = local.scene;
    here.position = local.position;
    here.event = event;
    here.self = local.self;
    here.max_life = max_life;
    if (local.scene) {
        for (const auto& room : CoopBoss::Rooms()) {  // the scene's boss (a dungeon has one boss room)
            if (room.scene == *local.scene) {
                here.boss_ours = CoopObjects::LastBossOurs(CoopBoss::Bodies(room));
                here.boss_alive = CoopObjects::LastBossAlive(CoopBoss::Bodies(room));
                break;
            }
        }
    }
    CoopBossScenes::Actions a = scenes.Tick(here, ours, now_us);
    // (one death cutscene per game: the event request refuses a played death while we are in its area)
    CoopBossScenes::RefusedDeaths().store(a.refuse_deaths, std::memory_order_release);
    // Player report #105 (a black screen after Gohma's death): once her death has begun in any game, our copy is
    // no copy any more; its own game runs its death (the setting we ask it, its warp and its container).
    std::vector<std::string> dying;
    if (local.scene) {
        for (const auto& room : CoopBoss::Rooms()) {
            if (room.scene == *local.scene && scenes.phase(room.index) >= CoopBossScenes::Director::Phase::outro) {
                for (const auto body : CoopBoss::Bodies(room)) dying.emplace_back(body);
            }
        }
    }
    CoopObjects::SetDyingBosses(dying);
    CoopBossNative::TheTable().SetDying(std::move(dying));
    for (const auto& line : a.log) {
        LOG_INFO(Core, "coop: boss: {}", line);
    }
    if (a.play) {
        wanted.push_back(std::move(*a.play));
    }
    if (a.move_to) {
        AskMove(system, main, *a.move_to);
    }
    if (a.skip) {
        mover.WantSkip(main);
    }
    if (a.set_max_life) {
        max_life_write = a.set_max_life;
    }
    if (a.max_life_cap != max_life_cap) {
        LOG_INFO(Core, "coop: boss: our maximum life {}", a.max_life_cap
                     ? "may not rise past " + std::to_string(*a.max_life_cap) + " here (the boss's one heart container)"
                     : std::string("cap lifted"));
    }
    max_life_cap = a.max_life_cap;
    session->with_client([&](coop::net::Client& c) {
        for (auto& m : a.send) {
            const coop::protocol::Message msg{m};
            const bool sent = c.send(msg);
            LOG_INFO(Core, "coop: boss: sent cutscene {} of boss {}{}", static_cast<int>(m.kind), m.boss,
                     sent ? "" : " (not sent)");
            if (const auto room = CoopBossBeaten::ProvenDeath(msg)) {
                CoopBossBeaten::TheRecord().Add(*room);  // our game took the death itself (G2)
            }
        }
        for (auto& m : a.claims) {
            const bool sent = c.send(coop::protocol::Message{m});
            LOG_INFO(Core, "coop: boss: heart container claim for boss {}{}", m.boss, sent ? "" : " (not sent)");
        }
        for (auto& line : a.chat) {
            const auto* p = c.find_player(line.player);
            lines.push_back((p != nullptr ? p->nickname : "player " + std::to_string(line.player)) + " " +
                            line.text);
        }
        return 0;
    });
    return true;
}

Glue& TheGlue() {
    static Glue glue;  // kept for the whole run: the cave holds its mover
    return glue;
}

} // namespace

void Frame(Core::System& system, std::uint64_t main_base, const std::optional<std::string>& scene,
           const std::optional<coop::protocol::Vec3>& position, bool event_running, std::optional<bool> shut_in,
           coop::host::Session* session, std::uint64_t server_now_us) {
    // Native boss sync: during our event a copy boss runs its own changes (the intro's, the death's; boss-fix).
    CoopBossNative::TheTable().SetEvent(main_base != 0 && event_running);
    TheGlue().Frame(system, main_base, scene, position, event_running, shut_in, session, server_now_us);
}

bool Fighting() {
    const auto phase = TheGlue().sync.phase();
    return phase == Sync::Phase::intro || phase == Sync::Phase::fight || phase == Sync::Phase::outro;
}

bool AskSceneChange(Core::System& system, std::uint64_t main_base, std::string_view scene,
                    std::string_view start) {
    return main_base != 0 && Glue::SetScene(system, main_base, scene, start);
}

bool MoveLink(Core::System& system, std::uint64_t main_base, const Vec& to) {
    return main_base != 0 && TheGlue().on && TheGlue().AskMove(system, main_base, to);
}

bool FaceLink(Core::System& system, std::uint64_t main_base, float x, float z) {
    return main_base != 0 && TheGlue().on && TheGlue().AskFace(system, main_base, x, z);
}

void OnEvent(const coop::net::Event& event) {
    if (event.kind == coop::net::EventKind::message) {
        TheGlue().sync.Receive(event.message);
        TheGlue().scenes.Receive(event.message);
        // Row 159: the host's save load, its bosses alive. Taken off the record here, on the thread that adds to
        // it, so a death proven before it never outlives it; the hook's frame rewinds our game (Apply::HostAlive).
        // Only a boss that died this session is rewound: one a guest's own save beat before stays its own.
        if (const auto* alive = std::get_if<coop::protocol::BossAlive>(&event.message)) {
            CoopBossBeaten::Record& record = CoopBossBeaten::TheRecord();
            const auto here = static_cast<std::uint16_t>(alive->alive & record.SeenMask());
            record.Remove(here);
            CoopBossBeaten::TheHostAlive().Put(coop::protocol::BossAlive{alive->player_number, alive->known, here});
            LOG_INFO(Core, "coop: boss: the host's save holds bosses {:#05x} alive (of {:#05x} read; died this "
                           "session: {:#05x})",
                     alive->alive, alive->known, here);
        }
        const auto* m = std::get_if<coop::protocol::BossRoom>(&event.message);
        if (m != nullptr && m->state != coop::protocol::BossState::in_room) {
            LOG_INFO(Core, "coop: boss: player {} says {} for {}", m->player_number, static_cast<int>(m->state),
                     m->scene);
        }
        // G2: a proven death of a dungeon boss (another game took its death event, or the server's record of one
        // for a player who joins; never a player's BOSS_ROOM defeated, maybe a guess). A game whose own flags lack
        // it gets its beaten set at a scene outside the boss's (coop_boss_beaten.h, coop_hook.cpp).
        if (const auto room = CoopBossBeaten::ProvenDeath(event.message);
            room && CoopBossBeaten::TheRecord().Add(*room)) {
            LOG_INFO(Core, "coop: boss: the group beat the boss of room {}: kept for this session", *room);
        }
    }
}

void ForgetFights() {
    Glue& g = TheGlue();
    g.sync.Reset();
    g.scenes.Reset();
    CoopBossScenes::RefusedDeaths().store(0, std::memory_order_release);
    g.readiness.Reset();
    g.max_life_cap.reset();
    CoopBossBeaten::TheRecord().Clear();  // bosses beaten after that save are alive again (G2, as #52)
    LOG_INFO(Core, "coop: boss: the host loaded a save: the fights this session knew are forgotten");
}

void SetMaxLife(std::optional<std::int32_t> max_life) {
    TheGlue().max_life = max_life;
}

std::optional<std::int32_t> TakeMaxLifeWrite() {
    return std::exchange(TheGlue().max_life_write, std::nullopt);
}

std::optional<std::int32_t> MaxLifeCap() {
    return TheGlue().max_life_cap;
}

std::vector<std::string> TakeWantedEvents() {
    return std::exchange(TheGlue().wanted, {});
}

bool DeathMayRun(std::uint8_t boss) {
    return TheGlue().scenes.DeathMayRun(boss);
}

std::vector<std::string> TakeLines() {
    return std::exchange(TheGlue().lines, {});
}

} // namespace CoopBoss
