// SPDX-License-Identifier: GPL-3.0-or-later
//
// The port end to end over real ENet on 127.0.0.1: game A is the shared-
// enemies port (objects.cpp with lichii/hook/coop_objects.cpp unchanged) on
// the stand-in game (fake_game.hpp), connected through coop/host's Session
// to the real server (server/enet_server.cpp); game B is a raw ENet client
// that says what a second game would. Single-threaded: the test pumps the
// server, A's network side, A's frame, A's game frame (the frame step hook)
// and B in turn. Takes a few seconds of real time (the server's clock).
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "coop/host/player_reader.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/protocol/codec.hpp"
#include "coop_hold.h"
#include "enet_server.hpp"
#include "fake_game.hpp"
#include "hold.hpp"
#include "objects.hpp"

namespace proto = coop::protocol;
namespace Objects = CoopNx2navi::Objects;
using FakeGame::kMain;

namespace {

constexpr const char* kScene = "HyruleField";
constexpr std::uint64_t kHash = 0xf9b880ab096a9cd6ull;
const proto::ObjectId kId{proto::ObjectKind::placed, 0, kHash};

struct Received {
    std::uint8_t channel;
    proto::Message message;
};

// Game B: the co-op client alone (it makes the join password's handshake and the traffic
// encryption every session has since protocol 10), with no game behind it: the test sends
// B's messages by hand and looks at what it is sent.
constexpr const char* kPassword = "loopback-pass";
proto::Hello HelloFrom(const std::string& nickname);
struct RawClient {
    std::unique_ptr<coop::net::Client> client;
    bool connected = false;
    std::vector<Received> inbox;

    void Connect(std::uint16_t port) {
        coop::net::ClientConfig config;
        config.host = "127.0.0.1";
        config.port = port;
        config.hello = HelloFrom("Mido");
        config.join_password = kPassword;
        client = std::make_unique<coop::net::Client>(config);
        REQUIRE(client->start());
    }
    void Send(const proto::Message& m) { REQUIRE(client->send(m)); }
    void Pump() {
        if (!client) {
            return;
        }
        client->service(0);
        while (auto e = client->next_event()) {
            if (e->kind == coop::net::EventKind::welcomed) {
                connected = true;
                inbox.push_back({proto::kChannelSession, proto::Message{e->welcome}});
            } else if (e->kind == coop::net::EventKind::message) {
                inbox.push_back({e->channel, e->message});
            }
        }
    }
    template <typename T>
    std::vector<T> All() const {
        std::vector<T> out;
        for (const Received& r : inbox) {
            if (const auto* m = std::get_if<T>(&r.message)) {
                out.push_back(*m);
            }
        }
        return out;
    }
};

struct EnetLife {
    EnetLife() { REQUIRE(enet_initialize() == 0); }
    ~EnetLife() { enet_deinitialize(); }
};

proto::Hello HelloFrom(const std::string& nickname) {
    proto::Hello h;
    h.build_id = "objects loopback test";
    h.game_build_id = "e0f3ccfaf50065ae";
    h.game_version = "1.1.1";
    h.nickname = nickname;
    return h;
}

std::vector<std::string> g_lines;
void Collect(const char* line) { g_lines.emplace_back(line); }

}  // namespace

TEST_CASE("objects loopback: the port shares an enemy with a second game over the real server") {
    EnetLife enet;
    coop::server::ServerOptions options;
    options.port = 0;
    options.session.ownership_check_us = 50'000;
    options.session.join_password = kPassword;
    options.session.owner_hold_us = 0;  // the first lister owns at once, as before the start hold
    options.session.handover_cooldown_us = 300'000;
    coop::server::EnetServer server(options);
    REQUIRE(server.start());

    // Game A: the stand-in game, the port, and coop/host's Session.
    FakeGame::Game game;
    const FakeGame::Actor link = game.Add("Player_Adult", ~0ull, {40, 0, 0}, {{16, 16}});
    const FakeGame::Actor enemy = game.Add("Enemy_ReDead", kHash, {5, 0, 5}, {{12, 12}}, "Wait");
    Objects::Options o;
    o.on = true;
    o.ai_hold = 1;
    REQUIRE(Objects::Init(game.Api(), o, Collect));
    coop::net::ClientConfig config;
    config.host = "127.0.0.1";
    config.port = server.port();
    config.hello = HelloFrom("Saria");
    config.join_password = kPassword;
    coop::net::Client client(config);
    REQUIRE(client.start());
    coop::host::SessionSettings settings;
    settings.on_event = [](const coop::net::Event& e, coop::net::Client&) { Objects::OnEvent(e); };
    coop::host::Session session(client, settings);
    std::uint64_t server_now = 0;

    RawClient b;
    std::array<float, 3> b_at{60, 0, 60};
    std::uint32_t b_sequence = 0;

    // One round of everything; B sends its sample (and `extra`) every round.
    const auto round = [&](const std::vector<proto::Message>& extra = {}) {
        server.service(1);
        session.service();
        coop::host::PlayerReading reading;
        reading.position = game.Position(link.address);
        reading.scene = kScene;
        reading.event_running = false;
        const auto report = session.frame(reading, nullptr);
        server_now = report.server_now_us;
        Objects::FrameInput in;
        in.main_base = kMain;
        in.session = report.connected ? &session : nullptr;
        in.scene = kScene;
        in.position = reading.position;
        in.server_now_us = report.server_now_us;
        Objects::Frame(in);
        game.Step();  // the game's frame: the frame step hook runs the Mover
        b.Pump();
        if (b.connected && server_now != 0) {
            proto::PlayerSample s;
            s.time.server_time_us = server_now;
            s.time.sequence = ++b_sequence;
            s.position = b_at;
            s.scene = kScene;
            b.Send(proto::Message{s});
            for (const auto& m : extra) {
                b.Send(m);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    const auto until = [&](int ms, const std::function<bool()>& done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline) {
            round();
            if (done()) {
                return true;
            }
        }
        return false;
    };

    // A joins and lists the enemy first: A owns it.
    const bool owned = until(4000, [&] { return server.session().object_owner(kId).has_value(); });
    if (!owned) {
        for (const auto& l : g_lines) {
            MESSAGE(l);
        }
    }
    REQUIRE(owned);
    CHECK(server.session().object_owner(kId)->first == 1);

    // B joins, far away, and lists it too.
    b.Connect(server.port());
    REQUIRE(until(2000, [&] { return !b.All<proto::Welcome>().empty(); }));
    proto::ObjectPresent present;
    present.scene = kScene;
    present.objects.push_back({kId, proto::object_presence::awake});
    b.Send(proto::Message{present});
    REQUIRE(until(2000, [&] { return !b.All<proto::ObjectOwner>().empty(); }));
    CHECK(b.All<proto::ObjectOwner>().back().owner == 1);

    // The owner's enemy moves: B gets samples with the new place and the life.
    game.SetPosition(enemy.address, {6, 0, 5});
    REQUIRE(until(2000, [&] {
        for (const auto& s : b.All<proto::ObjectSample>()) {
            if (s.id == kId && s.state.position && (*s.state.position)[0] > 5.9f) {
                return true;
            }
        }
        return false;
    }));
    const auto first = b.All<proto::ObjectSample>().back();
    REQUIRE(first.state.life.has_value());
    CHECK(first.state.life->life == 12);

    // B comes to it (A's Link stays 35 m off) and hits it for 2: the hit goes to
    // A's enemy, and the enemy goes to B at once (the hit rule).
    b_at = {6, 0, 6};
    for (int i = 0; i < 10; ++i) {
        round();
    }
    proto::Damage hit;
    hit.id = kId;
    hit.epoch = first.epoch;
    hit.amount = 2;
    hit.position = {6, 1, 5};
    b.Send(proto::Message{hit});
    REQUIRE(until(3000, [&] {
        const auto o = server.session().object_owner(kId);
        return o && o->first == 2;
    }));
    const std::uint16_t epoch = server.session().object_owner(kId)->second;
    // The hit is taken off A's enemy: through the game's own damage step first
    // (a hit record the Mover writes on the calc thread), then, as the stand-in
    // never takes it, as life after 400 ms.
    REQUIRE(until(2000, [&] { return game.Life(enemy) == 10; }));
    CHECK(game.Get<std::uint32_t>(enemy.life + 0x240 + 0xc) == 1);  // the replayed hit's present flag
    const auto states = b.All<proto::HandoverState>();
    REQUIRE_FALSE(states.empty());
    REQUIRE(states.back().state.life.has_value());
    CHECK(states.back().state.life->life <= 10);

    // B now owns it and sends where its enemy is and what it plays: A's copy is
    // put there by the game's setter and plays B's animation; its brain is held.
    const auto b_sample = [&](std::array<float, 3> at, std::int32_t life, const char* anim) {
        proto::ObjectSample s;
        s.id = kId;
        s.epoch = epoch;
        s.state.time.server_time_us = server_now;
        s.state.time.sequence = ++b_sequence;
        s.state.position = at;
        s.state.life = proto::ObjectLife{life, 12};
        s.state.animation = proto::Animation{anim, 3.0f, 1.0f};
        return proto::Message{s};
    };
    // 1.5 s: also past the second in which a fall of A's enemy still counts as
    // the game taking A's own earlier hurt (Sync::kHurtLateUs).
    const auto copy_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < copy_until) {
        round({b_sample({2, 0, 2}, 10, "Attack")});
    }
    const auto at = game.Position(enemy.address);
    CHECK(at[0] == doctest::Approx(2.0f).epsilon(0.02));
    CHECK(at[2] == doctest::Approx(2.0f).epsilon(0.02));
    bool set = false;
    for (const auto& c : game.CallsTo(0x4c6580)) {
        set = set || c.x[0] == enemy.address;
    }
    CHECK(set);
    CHECK(game.SlotName(enemy) == "Attack");
    CHECK(Objects::Hold::TheTable().Count() == 1);
    CHECK(Objects::Hold::TheTable().At(0) == enemy.bsa);

    // A's player hits the copy: a DAMAGE of 1 goes to B, the owner.
    game.SetLife(enemy, 9);
    const bool damaged = until(2000, [&] {
        for (const auto& d : b.All<proto::Damage>()) {
            if (d.id == kId && d.amount == 1) {
                return true;
            }
        }
        return false;
    });
    if (!damaged) {
        for (const auto& l : g_lines) {
            MESSAGE(l);
        }
    }
    REQUIRE(damaged);

    // B's enemy dies: A's copy is killed through the game, then deleted.
    proto::ObjectRemove dead;
    dead.id = kId;
    dead.epoch = epoch;
    dead.reason = proto::RemoveReason::dead;
    dead.position = {2, 0, 2};
    b.Send(proto::Message{dead});
    REQUIRE(until(2000, [&] { return game.Life(enemy) == 0; }));
    REQUIRE(until(5000, [&] { return !game.CallsTo(0xa6490).empty(); }));
    CHECK(game.CallsTo(0xa6490).back().x[0] == enemy.address);
    CHECK(Objects::StatusText().find("frame steps") != std::string::npos);
    MESSAGE(Objects::StatusText());

    Objects::FrameInput off;
    off.main_base = kMain;
    Objects::Frame(off);  // not connected: the sync forgets
    Objects::Shutdown();
    client.stop();
}
