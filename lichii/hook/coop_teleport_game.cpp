// SPDX-License-Identifier: GPL-3.0-or-later
//
// /teleport <player>, the emulator side (coop_teleport.h, docs/setup.md
// "Chat commands"). The asking and answering go through JOIN_PARTNER
// (coop/net Client: request_join, accept_join, refuse_join); our Link is
// moved and the scene changed the ways the boss rooms do it (CoopBoss::
// MoveLink, CoopBoss::AskSceneChange; docs/dungeons-boss.md).
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
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_boss.h"

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
    Grounded grounded;

    Glue() {
        LOG_INFO(Core, "coop: teleport {}", on ? "on (/teleport <player> in chat)" : "off (LICHII_COOP_TELEPORT=0)");
    }

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

    for (auto& act : all) {
        if (act.cancel_scene) {
            const bool ok = CoopBoss::AskSceneChange(system, main_base, "", "");
            LOG_INFO(Core, "coop: teleport: scene change request cleared: {}", ok ? "done" : "NOT done");
        }
        if (act.scene) {
            const bool ok = CoopBoss::AskSceneChange(system, main_base, *act.scene, "");
            if (!ok) {
                LOG_WARNING(Core, "coop: teleport: the scene change to {} could not be asked", *act.scene);
            }
        }
        if (act.move) {
            const bool ok = CoopBoss::MoveLink(system, main_base, CoopBoss::Vec{act.move->x, act.move->y, act.move->z});
            LOG_INFO(Core, "coop: teleport: move to {:.1f} {:.1f} {:.1f}: {}", act.move->x, act.move->y,
                     act.move->z, ok ? "asked" : "NOT asked (no Link, or LICHII_COOP_BOSS=0)");
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
