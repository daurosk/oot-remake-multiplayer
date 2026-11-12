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
#include "coop_objects.h"

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
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
constexpr std::uint64_t kSetMatrix = 0x4c6580;  // vtable slot 0x218 of every actor seen
constexpr int kCallerOrder = 50;  // after the puppets, spawn, fairy, objects, time
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
    bool connected = false;
    std::uint64_t debug_at = 0;
    Sync sync;
    Mover mover;

    Glue() {
        on = Env("LICHII_COOP_BOSS") != "0";
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

    void Frame(Core::System& system, std::uint64_t main, const std::optional<std::string>& scene,
               const std::optional<coop::protocol::Vec3>& position, bool event,
               coop::host::Session* session, std::uint64_t now_us) {
        if (!on) {
            return;
        }
        const auto steady_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
        if (tick_at != 0 && steady_us - tick_at < kTickEveryUs) {
            return;
        }
        tick_at = steady_us;
        if (main == 0 || now_us == 0 || session == nullptr) {
            if (connected) {
                sync.Reset();
                connected = false;
            }
            return;
        }
        connected = true;
        Local local;
        local.scene = scene;
        if (position) {
            local.position = Vec{(*position)[0], (*position)[1], (*position)[2]};
        }
        local.event = event;
        if (scene) {
            if (const Room* room = RoomOfScene(*scene)) {
                local.boss_alive = CoopObjects::LastBossAlive(Bodies(*room));
                local.boss_hurt = CoopObjects::LastBossHurt(Bodies(*room));
            }
        }
        local.self = session->with_client([](coop::net::Client& c) { return c.player_number(); });
        Actions act = sync.Tick(local, now_us);
        for (const auto& line : act.log) {
            LOG_INFO(Core, "coop: boss: {}", line);
        }
        if (debug && now_us - debug_at >= kDebugEveryUs) {
            debug_at = now_us;
            LOG_INFO(Core, "coop: boss: phase {}, scene {}, at {:.1f} {:.1f} {:.1f}, event {}",
                     PhaseName(sync.phase()), scene.value_or("?"), position ? (*position)[0] : 0.0f,
                     position ? (*position)[1] : 0.0f, position ? (*position)[2] : 0.0f, event);
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
    }
};

Glue& TheGlue() {
    static Glue glue;  // kept for the whole run: the cave holds its mover
    return glue;
}

} // namespace

void Frame(Core::System& system, std::uint64_t main_base, const std::optional<std::string>& scene,
           const std::optional<coop::protocol::Vec3>& position, bool event_running,
           coop::host::Session* session, std::uint64_t server_now_us) {
    TheGlue().Frame(system, main_base, scene, position, event_running, session, server_now_us);
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

void OnEvent(const coop::net::Event& event) {
    if (event.kind == coop::net::EventKind::message) {
        TheGlue().sync.Receive(event.message);
        const auto* m = std::get_if<coop::protocol::BossRoom>(&event.message);
        if (m != nullptr && m->state != coop::protocol::BossState::in_room) {
            LOG_INFO(Core, "coop: boss: player {} says {} for {}", m->player_number, static_cast<int>(m->state),
                     m->scene);
        }
    }
}

} // namespace CoopBoss
