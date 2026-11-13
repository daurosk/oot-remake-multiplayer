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

#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>
#include <variant>

#include "common/logging.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_actor_events.h"
#include "coop_objects.h"
#include "coop_puppet.h"

namespace CoopDbno {
namespace {

std::uint64_t SteadyUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// What the pad hook does with the pad (set by the frame).
enum class PadMode : std::uint8_t { game, downed, reviver };

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
    std::vector<std::string> lines;   // any thread -> chat
    std::optional<View> view;         // frame -> the screen
    coop::host::Session* session = nullptr;  // the last frame's, for Change

    std::atomic<bool> tell_again{false};
    // The frame's own (no lock): only the frame touches them.
    Machine machine;
    Reviver reviver;

    // The game's threads.
    LifeGuard guard;
    std::atomic<std::uint64_t> life_component{0};
    std::atomic<std::int32_t> life_write{-1};  // raise to this (-1: none); the objects walk writes it
    std::atomic<std::int32_t> take_to{-1};     // lower to this (-1: none); the Life step takes it
    std::atomic<PadMode> pad_mode{PadMode::game};
    std::atomic<bool> button{false};  // A as the player holds it (before we take it)

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
            LOG_INFO(Core, "coop: dbno: P{} is {}", s->player_number,
                     s->state == State::up ? "up" : s->state == State::downed ? "down" : "out");
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
            t.scene = r.scene;
            t.position = r.position;
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
    const Output out = g.machine.Frame(in);
    g.guard.Set(out.guard);
    if (out.write_life) {
        // Less: through the game's own Life step (its death at 0). More (a
        // revive): written, as the step heals nothing.
        if (in.life && *out.write_life < *in.life) {
            g.take_to.store(*out.write_life);
        } else {
            g.life_write.store(*out.write_life);
        }
    }

    // Beside a downed teammate: hold A.
    const bool event = reading && reading->event_running.value_or(false);
    const bool able = connected && rules.on && out.state == State::up && reading.has_value() && !event;
    const Reviver::Out rev =
        g.reviver.Frame(now_us, able, g.button.load(), in.position, scene, fallen);
    g.pad_mode.store(out.state == State::downed ? PadMode::downed
                     : g.reviver.TakesButton()  ? PadMode::reviver
                                                : PadMode::game);

    std::vector<std::string> lines;
    std::optional<View> view;
    if (session != nullptr && (out.send || rev.send || !out.events.empty() || out.state == State::downed ||
                               rev.target != 0)) {
        session->with_client([&](coop::net::Client& client) {
            if (connected && out.send) {
                client.send(*out.send);
            }
            if (connected && rev.send) {
                client.send(*rev.send);
            }
            for (const Event e : out.events) {
                if (auto line = OwnLine(e, NameOf(client, out.by))) {
                    lines.push_back(std::move(*line));
                }
                LOG_INFO(Core, "coop: dbno: our Link: event {} (by P{})", static_cast<int>(e), out.by);
            }
            if (out.state == State::downed) {
                view = View{View::Kind::downed, NameOf(client, out.reviver), out.left_us, out.progress, false};
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
    std::lock_guard lock(g.mutex);
    for (auto& l : lines) {
        g.Line(std::move(l));
    }
    g.view = std::move(view);
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
    return Lowered(life, sum, g.take_to.exchange(-1));
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
    case PadMode::game:
        break;
    }
    return in;
}

}  // namespace CoopDbno
