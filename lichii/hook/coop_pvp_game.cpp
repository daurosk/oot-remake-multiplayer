// SPDX-License-Identifier: GPL-3.0-or-later
//
// Player against player, the emulator side (coop_pvp.h has the rules,
// docs/play-online.md "PvP"). The attacker's game reads its own sword
// swings (the grass sync's foliage cuts) and its own shots' ends (the spawn
// sync) against the other players' Links as presence shows them, and sends
// PVP_HIT; the victim's game decides, gives the hurt to its Link through the
// object walk (CoopObjects::HurtOurLink) and sends PVP_HURT. All of it waits
// on the server's PVP_MODE: off until the host's server says on.

#include "coop_pvp_game.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <utility>
#include <variant>

#include "common/logging.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_objects.h"
#include "coop_puppet.h"
#include "coop_pvp.h"

namespace CoopPvp {
namespace {

namespace proto = coop::protocol;

std::uint64_t SteadyUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

struct Glue {
    std::mutex mutex;
    Mode mode;                             // network thread writes, under mutex
    std::atomic<bool> on{false};           // mode.on, for any thread
    std::deque<proto::GrassCut> swings;    // present thread, ours
    struct Shot {
        proto::DamageCause cause;
        proto::Vec3 at;
    };
    std::deque<Shot> shots;
    std::deque<proto::PvpHit> hits;        // network thread -> present thread
    std::vector<std::string> lines;        // any thread -> chat box
    Attacker attacker;                     // present thread
    Victim victim;                         // present thread

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
    if (const auto* p = client.find_player(number); p != nullptr && !p->nickname.empty()) {
        return p->nickname;
    }
    return "P" + std::to_string(number);
}

} // namespace

bool On() {
    return TheGlue().on.load();
}

bool OnChat(std::string_view text, coop::host::Session& session) {
    const auto command = Parse(text);
    if (!command) {
        return false;
    }
    Glue& g = TheGlue();
    std::string line;
    session.with_client([&](coop::net::Client& client) {
        if (client.state() != coop::net::State::connected) {
            line = "pvp: not connected";
            return;
        }
        Mode now;
        {
            std::lock_guard lock(g.mutex);
            now = g.mode;
        }
        switch (command->kind) {
        case Command::Kind::show:
            line = ModeLine(now, client.player_number(), NameOf(client, now.set_by)) +
                   (client.is_host() ? ". /pvp on or /pvp off changes it" : "");
            return;
        case Command::Kind::bad:
            line = "pvp: /pvp on, /pvp off, or /pvp alone to see it";
            return;
        case Command::Kind::on:
        case Command::Kind::off: {
            if (!client.is_host()) {
                line = "pvp: only the host can change it";
                return;
            }
            const bool want = command->kind == Command::Kind::on;
            if (!client.send(proto::PvpMode{0, want})) {
                line = "pvp: could not be sent";
            }
            // The server's answer (PVP_MODE to everyone) shows the change.
            return;
        }
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
    if (event.kind == EventKind::welcomed || event.kind == EventKind::reconnected ||
        event.kind == EventKind::disconnected) {
        // Each connection starts with PvP off; the server says so when it is on.
        std::lock_guard lock(g.mutex);
        g.mode = Mode{};
        g.on.store(false);
        g.hits.clear();
        return;
    }
    if (event.kind != EventKind::message) {
        return;
    }
    if (const auto* m = std::get_if<proto::PvpMode>(&event.message)) {
        std::lock_guard lock(g.mutex);
        const bool changed = g.mode.on != m->on || m->set_by != 0;
        g.mode = Mode{m->on, m->set_by};
        g.on.store(m->on);
        if (!m->on) {
            g.hits.clear();
        }
        if (changed) {
            g.Line(ModeLine(g.mode, client.player_number(), NameOf(client, m->set_by)));
        }
        LOG_INFO(Core, "coop: pvp: {} (set by {})", m->on ? "on" : "off", m->set_by);
    } else if (const auto* h = std::get_if<proto::PvpHit>(&event.message)) {
        std::lock_guard lock(g.mutex);
        if (g.mode.on && g.hits.size() < 16) {
            g.hits.push_back(*h);
        }
    } else if (const auto* hurt = std::get_if<proto::PvpHurt>(&event.message)) {
        const auto line = HurtLine(*hurt, client.player_number(), NameOf(client, hurt->player_number),
                                   NameOf(client, hurt->attacker));
        LOG_INFO(Core, "coop: pvp: P{} hurt by P{}: {} (life {}, flags {})", hurt->player_number,
                 hurt->attacker, hurt->amount, hurt->life, hurt->flags);
        if (line) {
            std::lock_guard lock(g.mutex);
            g.Line(*line);
        }
    }
}

void OwnSwing(const proto::GrassCut& cut) {
    Glue& g = TheGlue();
    if (!g.on.load()) {
        return;
    }
    std::lock_guard lock(g.mutex);
    if (g.swings.size() < 32) {
        g.swings.push_back(cut);
    }
}

void OwnShot(proto::DamageCause cause, const proto::Vec3& at) {
    Glue& g = TheGlue();
    if (!g.on.load()) {
        return;
    }
    std::lock_guard lock(g.mutex);
    if (g.shots.size() < 32) {
        g.shots.push_back({cause, at});
    }
}

void Frame(const std::optional<std::string>& scene, const std::optional<coop::host::PlayerReading>& reading,
           coop::host::Session* session, std::uint64_t server_now_us) {
    Glue& g = TheGlue();
    std::deque<proto::GrassCut> swings;
    std::deque<Glue::Shot> shots;
    std::deque<proto::PvpHit> hits;
    {
        std::lock_guard lock(g.mutex);
        swings.swap(g.swings);
        shots.swap(g.shots);
        hits.swap(g.hits);
    }
    if (session == nullptr || !g.on.load()) {
        g.attacker.Reset();
        g.victim.Reset();
        return;
    }
    const std::uint64_t now_us = SteadyUs();

    // Who we are, and our own status as we last sent it.
    std::uint8_t self = 0;
    std::uint8_t status = 0;
    session->with_client([&](coop::net::Client& client) {
        self = client.player_number();
        if (const auto* me = client.find_player(self)) {
            status = me->status.value_or(0);
        }
    });

    std::vector<Target> targets;
    for (const auto& r : CoopPuppet::Collect(session, server_now_us)) {
        targets.push_back(Target{r.number, r.scene, r.position, r.status});
    }

    std::vector<proto::Message> send;
    // Ours hitting them.
    if (reading && scene && self != 0) {
        for (const auto& cut : swings) {
            for (auto& h : g.attacker.Swing(cut, reading->position, *scene, self, targets, now_us)) {
                LOG_INFO(Core, "coop: pvp: our sword reached P{} at {:.2f} {:.2f} {:.2f}", h.victim, h.at[0],
                         h.at[1], h.at[2]);
                send.emplace_back(h);
            }
        }
        for (const auto& s : shots) {
            for (auto& h : g.attacker.Shot(s.cause, s.at, reading->position, *scene, self, targets, now_us)) {
                LOG_INFO(Core, "coop: pvp: our shot (cause {}) reached P{}", static_cast<int>(s.cause),
                         h.victim);
                send.emplace_back(h);
            }
        }
    }

    // Theirs hitting us: our game decides.
    if (!hits.empty()) {
        Local local;
        local.self = self;
        local.scene = scene;
        local.status = status;
        local.event = reading && reading->event_running.value_or(false);
        local.life = CoopObjects::OurLinkLife();
        if (!reading) {
            local.status |= proto::status::loading;  // no Link to read: a load
        }
        for (const auto& h : hits) {
            Hitter who;
            for (const auto& t : targets) {
                if (t.number == h.player_number) {
                    who = Hitter{t.scene, t.status, true};
                }
            }
            const auto d = g.victim.Hit(h, true, local, who, now_us);
            if (!d) {
                continue;
            }
            if (d->take > 0) {
                CoopObjects::HurtOurLink(d->take, h.at, h.from);
                local.life = d->hurt.life;
            }
            LOG_INFO(Core, "coop: pvp: hit from P{} ({} quarter hearts): {}", h.player_number, h.amount,
                     d->take > 0 ? "taken" : "kept off (cutscene, menu, load, or just hit)");
            send.emplace_back(d->hurt);
        }
    }

    if (send.empty()) {
        return;
    }
    std::vector<std::string> lines;
    session->with_client([&](coop::net::Client& client) {
        for (const auto& m : send) {
            client.send(m);
            if (const auto* hurt = std::get_if<proto::PvpHurt>(&m)) {
                proto::PvpHurt mine = *hurt;
                mine.player_number = self;
                if (auto line = HurtLine(mine, self, NameOf(client, self), NameOf(client, hurt->attacker))) {
                    lines.push_back(std::move(*line));
                }
            }
        }
    });
    if (!lines.empty()) {
        std::lock_guard lock(g.mutex);
        for (auto& l : lines) {
            g.Line(std::move(l));
        }
    }
}

std::vector<std::string> TakeLines() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    std::vector<std::string> out;
    out.swap(g.lines);
    return out;
}

} // namespace CoopPvp
