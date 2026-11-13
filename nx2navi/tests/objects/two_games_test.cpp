// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two games through the server's rules, without a network: each game is the
// hook's own Sync (lichii/hook/coop_objects_sync.cpp, as the port runs it)
// over a small actor table that stands for the walk (what coop_objects.cpp
// reads) and the Mover (what it writes: lives and poses). The server is
// server/session.cpp. One clock for all, stepped 33 ms at a time.
//
// It checks the user's three wishes against what the rules do:
// - same enemies, positions, life and deaths: one owner, copies placed where
//   the owner's enemy is, a hit on a copy reaching the owner, a kill removing
//   the copies;
// - the nearest player's game owns the enemy (so it attacks that player):
//   when, after how long, and the cases where it does not;
// - a boss and its parts move together, and boss rooms start together.
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop/protocol/join_password.hpp"
#include "coop/protocol/traffic_crypto.hpp"
#include "coop_boss.h"
#include "coop_objects_sync.h"
#include "session.hpp"

namespace proto = coop::protocol;
using coop::server::ConnectionId;
using CoopObjects::Seen;

namespace {

constexpr std::uint64_t kStepUs = 33'333;
constexpr std::uint64_t kS = 1'000'000;
constexpr const char* kScene = "HyruleField";

struct Actor {
    std::uint64_t address = 0;
    std::string name;
    std::uint64_t hash = 0;
    proto::Vec3 position{};
    std::int32_t life = 0;
    std::int32_t max = 0;
    bool listed = true;  // on the game's actor list
    std::optional<proto::Animation> animation;   // AS slot 0, as the walk reads it
    std::vector<proto::SlotAnimation> slots;      // the other slots
};

struct Game {
    std::uint8_t number = 0;
    ConnectionId conn = 0;
    CoopObjects::Sync sync;
    std::map<std::uint64_t, Actor> actors;  // by address
    proto::Vec3 link{};
    std::string scene = kScene;
    std::vector<std::uint64_t> deleted;  // the Mover's delete requests carried out
    std::uint32_t sequence = 0;

    Actor& Add(std::uint64_t address, const std::string& name, std::uint64_t hash, proto::Vec3 at,
               std::int32_t life) {
        Actor a;
        a.address = address;
        a.name = name;
        a.hash = hash;
        a.position = at;
        a.life = life;
        a.max = life;
        return actors[address] = a;
    }
    Actor& At(std::uint64_t address) { return actors.at(address); }
};

float Distance(const proto::Vec3& a, const proto::Vec3& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

class World {
public:
    // No owner hold after the start (owner_hold_us, upstream's 2 s for a restarted server): these
    // cases look at who owns what from the first list on.
    static coop::server::SessionConfig NoHold(coop::server::SessionConfig config) {
        config.owner_hold_us = 0;
        config.join_password = kPassword;  // every session has one: there is no plain way in
        return config;
    }
    static constexpr const char* kPassword = "test-pass";
    static const proto::KeyPair& Keys() {
        static const proto::KeyPair keys = proto::KeyPair::from_secret({7, 7, 7});
        return keys;
    }
    explicit World(coop::server::SessionConfig config = {}) : server_(NoHold(std::move(config))) {
        Join(a, 1, "Saria");
        Join(b, 2, "Mido");
    }

    Game a, b;
    std::uint64_t now = 10 * kS;
    std::vector<proto::Message> to_a, to_b;  // everything the server sent, for checks

    // One frame of both games and the server.
    void Step() {
        now += kStepUs;
        Frame(a);
        Frame(b);
        server_.tick(now);
        Deliver();
    }
    void Run(std::uint64_t us) {
        const std::uint64_t until = now + us;
        while (now < until) {
            Step();
        }
    }
    // Runs until `done` or `us` passed; the time it took, or nothing.
    template <typename F>
    std::optional<std::uint64_t> RunUntil(std::uint64_t us, F done) {
        const std::uint64_t from = now;
        while (now - from < us) {
            Step();
            if (done()) {
                return now - from;
            }
        }
        return std::nullopt;
    }

    std::uint8_t Owner(std::uint64_t hash) const {
        const auto o = server_.object_owner(proto::ObjectId{proto::ObjectKind::placed, 0, hash});
        return o ? o->first : 0;
    }
    std::uint32_t Group(std::uint64_t hash) const {
        return server_.object_group(proto::ObjectId{proto::ObjectKind::placed, 0, hash}).value_or(0);
    }
    const coop::server::Session& Server() const { return server_; }

    template <typename T>
    std::size_t Count(const std::vector<proto::Message>& box) const {
        return static_cast<std::size_t>(std::count_if(box.begin(), box.end(), [](const proto::Message& m) {
            return std::holds_alternative<T>(m);
        }));
    }

private:
    void Join(Game& g, std::uint8_t number, const std::string& name) {
        g.number = number;
        g.conn = number;
        REQUIRE(server_.connected(g.conn, now));
        proto::Hello h;
        h.build_id = "test 0.0";
        h.game_build_id = "e0f3ccfaf50065ae";
        h.game_version = "1.1.1";
        h.nickname = name;
        h.key_share = Keys().share();
        Send(g, proto::Message{h});
        Deliver();
    }

    void Send(Game& g, const proto::Message& m) {
        std::vector<std::uint8_t> bytes;
        REQUIRE(proto::encode(m, bytes) == proto::Error::none);
        server_.received(g.conn, proto::channel_of(m), bytes, now);
    }

    void Frame(Game& g) {
        // The player's own sample (coop/host's Sampler does this in the game).
        proto::PlayerSample sample;
        sample.time.server_time_us = now;
        sample.time.sequence = ++g.sequence;
        sample.position = g.link;
        sample.scene = g.scene;
        Send(g, proto::Message{sample});
        // The walk.
        std::vector<Seen> seen;
        for (const auto& [address, actor] : g.actors) {
            if (!actor.listed) {
                continue;
            }
            Seen s;
            s.address = address;
            s.name = actor.name;
            s.hash = actor.hash;
            s.position = actor.position;
            s.life = proto::ObjectLife{actor.life, actor.max};
            s.animation = actor.animation;
            s.slots = actor.slots;
            seen.push_back(s);
        }
        g.sync.SetPlayer(g.link);
        std::vector<proto::Message> send;
        std::vector<CoopObjects::LifeWrite> writes;
        g.sync.Tick(g.scene, g.number, now, seen, send, writes);
        // The Mover: lives, poses and deletes.
        for (const auto& w : writes) {
            if (auto it = g.actors.find(w.address); it != g.actors.end()) {
                it->second.life = std::max(0, w.life);
            }
        }
        std::vector<CoopObjects::PoseWrite> poses;
        g.sync.Poses(now, poses);
        for (const auto& p : poses) {
            if (auto it = g.actors.find(p.address); it != g.actors.end() && it->second.listed) {
                it->second.position = p.position;
            }
        }
        for (const std::uint64_t address : g.sync.Removes()) {
            if (auto it = g.actors.find(address); it != g.actors.end() && it->second.listed) {
                it->second.listed = false;
                g.deleted.push_back(address);
            }
        }
        for (const auto& m : send) {
            Send(g, m);
        }
    }

    void Deliver() {
        for (auto& action : server_.take_actions()) {
            const auto* s = std::get_if<coop::server::SendAction>(&action);
            if (s == nullptr) {
                continue;
            }
            proto::Message m;
            REQUIRE(proto::decode(s->bytes, m) == proto::Error::none);
            Game& g = s->to == a.conn ? a : b;
            if (const auto* challenge = std::get_if<proto::JoinChallenge>(&m)) {
                // The join password's proof, bound to both key shares, as a game sends it.
                Send(g, proto::Message{proto::JoinProof{proto::join_proof(
                            kPassword, challenge->nonce, Keys().share(),
                            challenge->key_share.value_or(proto::KeyShare{}))}});
                Deliver();
                return;
            }
            (s->to == a.conn ? to_a : to_b).push_back(m);
            g.sync.Receive(m);
        }
    }

    coop::server::Session server_;
};

constexpr std::uint64_t kReDead = 0xf9b880ab096a9cd6ull;
constexpr std::uint64_t kAddrA = 0x2000001000ull;  // the same enemy in each game's memory
constexpr std::uint64_t kAddrB = 0x3000005000ull;

}  // namespace

TEST_CASE("two games: one owner, the copy stands where the owner's enemy is") {
    World w;
    w.a.link = {0, 0, 0};
    w.b.link = {3, 0, 0};
    w.a.Add(kAddrA, "Enemy_ReDead", kReDead, {5, 0, 5}, 12);
    w.b.Add(kAddrB, "Enemy_ReDead", kReDead, {5, 0, 5}, 12);
    w.Run(kS);
    CHECK(w.Owner(kReDead) == 1);  // A listed it first
    // The owner's enemy walks off; the copy follows, 100 ms behind.
    for (int i = 0; i < 60; ++i) {
        w.a.At(kAddrA).position = {5.0f + 0.1f * static_cast<float>(i), 0, 5};
        w.Step();
    }
    // Right after the owner stops, the copy runs on along the last velocity for
    // up to 150 ms (kExtrapolateUs): 3 m/s x 0.15 s = 0.45 m past it, until the
    // owner's next sample, which for a still enemy comes within 1 s.
    w.Run(300'000);
    CHECK(Distance(w.b.At(kAddrB).position, w.a.At(kAddrA).position) < 0.5f);
    w.Run(kS);
    CHECK(Distance(w.b.At(kAddrB).position, w.a.At(kAddrA).position) < 0.05f);
    CHECK(w.b.At(kAddrB).position[0] == doctest::Approx(10.9f).epsilon(0.01));
}

TEST_CASE("two games: a hit on a copy is taken off the owner's enemy, and shows on both") {
    World w;
    w.a.link = {0, 0, 0};
    w.b.link = {3, 0, 0};
    w.a.Add(kAddrA, "Enemy_ReDead", kReDead, {5, 0, 5}, 12);
    w.b.Add(kAddrB, "Enemy_ReDead", kReDead, {5, 0, 5}, 12);
    w.Run(kS);
    REQUIRE(w.Owner(kReDead) == 1);
    w.b.At(kAddrB).life = 10;  // B's sword on its copy
    w.Run(600'000);
    CHECK(w.a.At(kAddrA).life == 10);
    CHECK(w.b.At(kAddrB).life == 10);
    CHECK(w.Owner(kReDead) == 1);  // two players in one fight: no flip
    CHECK(w.Server().object_in_fight(proto::ObjectId{proto::ObjectKind::placed, 0, kReDead}, w.now));
}

TEST_CASE("two games: a kill in the owner's game kills the copy, which then goes") {
    World w;
    w.a.link = {0, 0, 0};
    w.b.link = {3, 0, 0};
    w.a.Add(kAddrA, "Enemy_ReDead", kReDead, {5, 0, 5}, 12);
    w.b.Add(kAddrB, "Enemy_ReDead", kReDead, {5, 0, 5}, 12);
    w.Run(kS);
    REQUIRE(w.Owner(kReDead) == 1);
    w.a.At(kAddrA).life = 0;  // A's last hit
    w.Run(300'000);
    w.a.At(kAddrA).listed = false;  // the game's own death takes it off the list
    w.Run(500'000);
    CHECK(w.b.At(kAddrB).life == 0);  // the copy is killed through the game
    // Still standing 3 s later: deleted.
    const auto gone = w.RunUntil(5 * kS, [&] { return !w.b.At(kAddrB).listed; });
    CHECK(gone.has_value());
    CHECK(w.b.deleted == std::vector<std::uint64_t>{kAddrB});
}

TEST_CASE("two games: a hit on the owner's own enemy from the other game is not counted twice") {
    World w;
    w.a.link = {0, 0, 0};
    w.b.link = {3, 0, 0};
    w.a.Add(kAddrA, "Enemy_ReDead", kReDead, {5, 0, 5}, 12);
    w.b.Add(kAddrB, "Enemy_ReDead", kReDead, {5, 0, 5}, 12);
    w.Run(kS);
    w.a.At(kAddrA).life = 11;  // A hits its own enemy
    w.Run(600'000);
    CHECK(w.b.At(kAddrB).life == 11);
    CHECK(w.a.At(kAddrA).life == 11);
    CHECK(w.Count<proto::Damage>(w.to_a) == 0);
}

TEST_CASE("two games: the nearer player's game takes an idle enemy, after about 3 s") {
    World w;
    w.a.link = {0, 0, 0};
    w.b.link = {48, 0, 0};
    w.a.Add(kAddrA, "Enemy_ReDead", kReDead, {2, 0, 0}, 12);
    w.b.Add(kAddrB, "Enemy_ReDead", kReDead, {2, 0, 0}, 12);
    w.Run(kS);
    REQUIRE(w.Owner(kReDead) == 1);
    w.Run(6 * kS);  // past the 5 s cooldown of the first owner
    // A walks away (45 m), B comes to it (2 m).
    w.a.link = {47, 0, 0};
    w.b.link = {4, 0, 0};
    const auto took = w.RunUntil(10 * kS, [&] { return w.Owner(kReDead) == 2; });
    REQUIRE(took.has_value());
    CHECK(*took >= 3 * kS);           // handover_hold_us
    CHECK(*took <= 3 * kS + 600'000);  // + the 250 ms check + the owner's answer
    MESSAGE("hand-over to the nearer player took " << *took / 1000 << " ms");
    // A's enemy is now the copy: it goes where B's enemy is.
    w.b.At(kAddrB).position = {3, 0, 1};
    w.Run(1'500'000);  // past the overshoot of a one-frame jump (see the first case)
    CHECK(Distance(w.a.At(kAddrA).position, proto::Vec3{3, 0, 1}) < 0.05f);
    CHECK(w.a.At(kAddrA).life == 12);  // the state came with it
}

TEST_CASE("two games: a kill by the player who does not own the enemy (runs s2, v1, v2)") {
    // Both players stand by the enemy, so it stays the first player's (two
    // players in one fight are never far apart: no hand-over on a hit).
    World w;
    w.a.link = {0, 0, 0};
    w.b.link = {2, 0, 0};
    w.a.Add(kAddrA, "Enemy_Stalchild_Big", kReDead, {1, 0, 2}, 9);
    w.b.Add(kAddrB, "Enemy_Stalchild_Big", kReDead, {1, 0, 2}, 9);
    w.Run(7 * kS);
    REQUIRE(w.Owner(kReDead) == 1);
    // B strikes its copy four times: each fall reaches the owner's enemy.
    for (std::int32_t life : {7, 5, 3, 1}) {
        w.b.At(kAddrB).life = life;
        w.Run(600'000);
        CHECK(w.a.At(kAddrA).life == life);
        CHECK(w.b.At(kAddrB).life == life);  // and is not healed back from the owner's samples
    }
    CHECK(w.Owner(kReDead) == 1);
    CHECK(w.Count<proto::Damage>(w.to_a) == 4);
    // The fifth strike kills the copy in B's game; the owner's enemy dies of
    // the DAMAGE, its game takes it off the list, and says it is dead.
    w.b.At(kAddrB).life = 0;
    w.Run(600'000);
    CHECK(w.a.At(kAddrA).life == 0);
    w.a.At(kAddrA).listed = false;  // the owner's game ends its dead enemy
    w.Run(kS);
    const bool said_dead = std::any_of(w.to_b.begin(), w.to_b.end(), [](const proto::Message& m) {
        const auto* r = std::get_if<proto::ObjectRemove>(&m);
        return r != nullptr && r->reason == proto::RemoveReason::dead;
    });
    CHECK(said_dead);
    // B's copy, should its own game not have ended it by then, is deleted
    // after the grace time; it is never healed or left standing.
    w.Run(4 * kS);
    CHECK(w.b.At(kAddrB).life == 0);
    CHECK(w.b.deleted == std::vector<std::uint64_t>{kAddrB});
    CHECK(w.a.deleted.empty());
}

// coop-dll 0.8: every AS slot of an enemy (CoopObjects::EnemySlots). A Stalchild walks on slots 1 and
// 2 with nothing in slot 0; 0.7 wished slot 0 alone and nothing at all while it was empty.
TEST_CASE("two games: a copy is wished every slot of its owner's enemy, an empty slot 0 too") {
    const auto wishes = [](World& w) {
        std::vector<CoopObjects::PlayWish> all, out;
        w.b.sync.Plays(w.now, all);
        for (const auto& p : all) {
            if (p.address == kAddrB) {
                out.push_back(p);
            }
        }
        std::sort(out.begin(), out.end(), [](const auto& x, const auto& y) { return x.slot < y.slot; });
        return out;
    };
    const auto walking = [](Actor& a) {
        a.animation = proto::Animation{"", 0.0f, 1.0f};
        a.slots = {{1, proto::Animation{"Walk", 0.0f, 1.0f}}, {2, proto::Animation{"WalkUpper", 0.0f, 1.0f}}};
    };
    SUBCASE("on (the NX2navi port)") {
        CoopObjects::EnemySlots(true);
        World w;
        w.a.link = {0, 0, 0};
        w.b.link = {30, 0, 0};
        walking(w.a.Add(kAddrA, "Enemy_Stalchild", kReDead, {0, 0, 5}, 2));
        w.b.Add(kAddrB, "Enemy_Stalchild", kReDead, {0, 0, 5}, 2);
        w.Run(2 * kS);
        REQUIRE(w.Owner(kReDead) == 1);
        auto got = wishes(w);
        REQUIRE(got.size() == 3);
        CHECK(got[0].slot == 0);
        CHECK(got[0].name.empty());  // nothing in slot 0: said, so the next one-shot is seen as new
        CHECK(got[1].name == "Walk");
        CHECK(got[2].name == "WalkUpper");
        // The owner's enemy attacks: slot 0 gets the one-shot, the others play nothing.
        w.a.At(kAddrA).animation = proto::Animation{"NoWeaponAttack", 0.0f, 1.0f};
        w.a.At(kAddrA).slots = {{1, proto::Animation{"", 0.0f, 1.0f}}, {2, proto::Animation{"", 0.0f, 1.0f}}};
        w.Run(kS / 2);
        got = wishes(w);
        REQUIRE(got.size() == 3);
        CHECK(got[0].name == "NoWeaponAttack");
        CHECK(got[1].name.empty());
        // And walks again: the one-shot's name stays in the owner's slot 0 in the game; the walk
        // reads what is there, so the copy is wished the same.
        w.a.At(kAddrA).slots = {{1, proto::Animation{"Walk", 0.0f, 1.0f}}, {2, proto::Animation{"WalkUpper", 0.0f, 1.0f}}};
        w.Run(kS / 2);
        got = wishes(w);
        REQUIRE(got.size() == 3);
        CHECK(got[1].name == "Walk");
    }
    SUBCASE("off (the shared code's default): slot 0 alone, and nothing while it is empty") {
        CoopObjects::EnemySlots(false);
        World w;
        w.a.link = {0, 0, 0};
        w.b.link = {30, 0, 0};
        walking(w.a.Add(kAddrA, "Enemy_Stalchild", kReDead, {0, 0, 5}, 2));
        w.b.Add(kAddrB, "Enemy_Stalchild", kReDead, {0, 0, 5}, 2);
        w.Run(2 * kS);
        CHECK(wishes(w).empty());
    }
    CoopObjects::EnemySlots(false);
}

// The mod's server rule (`--owner nearest`, SessionConfig::nearest_owner): an enemy's AI goes for
// its own game's Link only, so the nearest player's game owns it, in a fight too.
TEST_CASE("two games, nearest owner: the nearest player's game gets the enemy within 2 s, in a fight too") {
    coop::server::SessionConfig config;
    config.nearest_owner = true;
    World w(config);
    w.a.link = {0, 0, 0};
    w.b.link = {0, 0, 50};
    w.a.Add(kAddrA, "Enemy_ReDead", kReDead, {0, 0, 5}, 12);
    w.b.Add(kAddrB, "Enemy_ReDead", kReDead, {0, 0, 5}, 12);
    w.Run(4 * kS);
    REQUIRE(w.Owner(kReDead) == 1);
    w.a.At(kAddrA).life = 11;  // A hits it: a fight, which the classic rule keeps for 12 s and more
    w.Run(kS);
    w.a.link = {0, 0, 30};  // A steps back 25 m, B comes within 1 m
    w.b.link = {0, 0, 6};
    const auto took = w.RunUntil(5 * kS, [&] { return w.Owner(kReDead) == 2; });
    REQUIRE(took.has_value());
    MESSAGE("in a fight, the nearest player's game had it " << *took / 1000 << " ms after becoming the nearest");
    CHECK(*took <= 2 * kS);
    // And back, as fast, when A is the nearest again.
    w.b.link = {0, 0, 40};
    w.a.link = {0, 0, 4};
    const auto back = w.RunUntil(5 * kS, [&] { return w.Owner(kReDead) == 1; });
    REQUIRE(back.has_value());
    CHECK(*back <= 2500'000);  // the 1.5 s between two changes
}

TEST_CASE("two games, nearest owner: two players side by side do not pass an enemy to and fro") {
    coop::server::SessionConfig config;
    config.nearest_owner = true;
    World w(config);
    w.a.link = {0, 0, 0};
    w.b.link = {2, 0, 0};
    w.a.Add(kAddrA, "Enemy_ReDead", kReDead, {0, 0, 5}, 12);
    w.b.Add(kAddrB, "Enemy_ReDead", kReDead, {0, 0, 5}, 12);
    w.Run(4 * kS);
    REQUIRE(w.Owner(kReDead) == 1);
    const std::size_t before = w.Count<proto::ObjectOwner>(w.to_a);
    // B is nearer by less than the margin (2.5 m), then A, a few times: it stays A's.
    for (int i = 0; i < 6; ++i) {
        w.b.link = {i % 2 == 0 ? 0.5f : 2.0f, 0, i % 2 == 0 ? 1.5f : 0.0f};  // 3.5 m or 5.4 m from it; A 5.0 m
        w.Run(2 * kS);
        CHECK(w.Owner(kReDead) == 1);
    }
    // No change of owner was told in all that time (the 10 s repeats aside).
    CHECK(w.Count<proto::ObjectOwner>(w.to_a) - before <= 2);
    // Clearly nearer (3 m against 5 m is not; 1 m against 5 m is): B's.
    w.b.link = {0, 0, 4};
    const auto took = w.RunUntil(4 * kS, [&] { return w.Owner(kReDead) == 2; });
    CHECK(took.has_value());
}

TEST_CASE("two games: where the nearest player's game does NOT get the enemy") {
    SUBCASE("only 9 m nearer (the margin is 10 m): stays") {
        World w;
        w.a.link = {0, 0, 0};
        w.b.link = {0, 0, 0};
        w.a.Add(kAddrA, "Enemy_ReDead", kReDead, {0, 0, 25}, 12);
        w.b.Add(kAddrB, "Enemy_ReDead", kReDead, {0, 0, 25}, 12);
        w.Run(7 * kS);
        REQUIRE(w.Owner(kReDead) == 1);
        w.b.link = {0, 0, 9};  // 16 m from it, A 25 m
        w.Run(10 * kS);
        CHECK(w.Owner(kReDead) == 1);
    }
    SUBCASE("between the two, 11 m nearer but neither near (B 24 m, A 35 m): stays") {
        World w;
        w.a.link = {0, 0, 0};
        w.b.link = {0, 0, 0};
        w.a.Add(kAddrA, "Enemy_ReDead", kReDead, {0, 0, 35}, 12);
        w.b.Add(kAddrB, "Enemy_ReDead", kReDead, {0, 0, 35}, 12);
        w.Run(7 * kS);
        REQUIRE(w.Owner(kReDead) == 1);
        w.b.link = {0, 0, 11};
        w.Run(10 * kS);
        CHECK(w.Owner(kReDead) == 1);
    }
    SUBCASE("in a fight (its life changed lately) nobody takes it by distance") {
        World w;
        w.a.link = {0, 0, 0};
        w.b.link = {0, 0, 50};
        w.a.Add(kAddrA, "Enemy_ReDead", kReDead, {0, 0, 5}, 12);
        w.b.Add(kAddrB, "Enemy_ReDead", kReDead, {0, 0, 5}, 12);
        w.Run(7 * kS);
        REQUIRE(w.Owner(kReDead) == 1);
        w.a.At(kAddrA).life = 11;  // A hits it: a fight for 12 s
        w.Run(kS);
        w.a.link = {0, 0, 60};  // A runs off, B comes
        w.b.link = {0, 0, 6};
        w.Run(8 * kS);
        CHECK(w.Owner(kReDead) == 1);
        // The lock is 12 s on the server, but the owner's game also flags the
        // enemy in_fight for 12 s after its life changed (Sync::kFightUs), and
        // each OBJECT_PRESENT with that flag (every 2 s) starts the server's
        // 12 s again: the enemy moves about 22 to 24 s after the last hit,
        // plus the 3 s distance hold.
        const auto took = w.RunUntil(30 * kS, [&] { return w.Owner(kReDead) == 2; });
        REQUIRE(took.has_value());
        MESSAGE("after a hit, the distance rule moved it " << (*took + 9 * kS) / 1000
                << " ms after the hit");
        CHECK(*took + 9 * kS > 20 * kS);
    }
}

// The shared code's own rule since the upstream update (Sync::RollerFor: the lowest player number
// within kRollNearM, 40 m, of the roll keeps it), which replaced this port's OwnRollsWhenApart.
TEST_CASE("two games: a player away from the host rolls its own Stalchildren (run n1: none in 60 s)") {
    constexpr std::uint64_t kNoHash = ~0ull;
    const auto rolled_by = [](const std::vector<proto::Message>& box, std::uint8_t maker) {
        return std::count_if(box.begin(), box.end(), [maker](const proto::Message& m) {
            const auto* s = std::get_if<proto::ObjectSpawn>(&m);
            return s != nullptr && s->id.kind == proto::ObjectKind::made && s->id.maker == maker;
        });
    };
    SUBCASE("100 m from the host it is kept and shared, the host gets it to copy") {
        World w;
        w.a.link = {0, 0, 0};
        w.b.link = {100, 0, 0};
        w.Run(2 * kS);
        w.b.Add(kAddrB, "Enemy_Stalchild", kNoHash, {102, 0, 0}, 2);
        w.Run(2 * kS);
        CHECK(w.b.deleted.empty());
        CHECK(rolled_by(w.to_a, 2) == 1);
        // The host's own rolls are shared as ever.
        w.a.Add(kAddrA, "Enemy_Stalchild", kNoHash, {2, 0, 0}, 2);
        w.Run(2 * kS);
        CHECK(w.a.deleted.empty());
        CHECK(rolled_by(w.to_b, 1) == 1);
        // The second player's enemy takes the second player's hits as its
        // owner's: nothing goes to the host as DAMAGE.
        w.b.At(kAddrB).life = 0;
        w.Run(kS);
        CHECK(w.Count<proto::Damage>(w.to_a) == 0);
    }
    SUBCASE("beside the host (10 m) only the host's game rolls, so two players get one set") {
        World w;
        w.a.link = {0, 0, 0};
        w.b.link = {10, 0, 0};
        w.Run(2 * kS);
        w.b.Add(kAddrB, "Enemy_Stalchild", kNoHash, {12, 0, 0}, 2);
        w.Run(2 * kS);
        CHECK(w.b.deleted == std::vector<std::uint64_t>{kAddrB});
        CHECK(rolled_by(w.to_a, 2) == 0);
    }
}

TEST_CASE("two games: a hit from the nearer player takes the enemy at once") {
    World w;
    w.a.link = {0, 0, 0};
    w.b.link = {0, 0, 0};
    w.a.Add(kAddrA, "Enemy_ReDead", kReDead, {0, 0, 20}, 12);
    w.b.Add(kAddrB, "Enemy_ReDead", kReDead, {0, 0, 20}, 12);
    w.Run(7 * kS);
    REQUIRE(w.Owner(kReDead) == 1);
    w.b.link = {0, 0, 18};  // 2 m from it, A 20 m: 18 m nearer
    w.Run(100'000);
    w.b.At(kAddrB).life = 10;
    const auto took = w.RunUntil(2 * kS, [&] { return w.Owner(kReDead) == 2; });
    REQUIRE(took.has_value());
    CHECK(*took < 500'000);
    w.Run(kS);
    CHECK(w.a.At(kAddrA).life == 10);  // the hit itself was not lost
    CHECK(w.b.At(kAddrB).life == 10);
}

TEST_CASE("two games: a boss and its parts have one owner and change owner together") {
    constexpr std::uint64_t kBoss = 0xb0550001ull, kArm = 0xb0550002ull;
    World w;
    w.a.link = {0, 0, 0};
    w.b.link = {0, 0, 30};
    w.a.Add(0x2000001000ull, "Enemy_DungeonBoss_Barinade", kBoss, {0, 0, 10}, 24);
    w.a.Add(0x2000002000ull, "Enemy_DungeonBoss_Barinade_Arm", kArm, {0, 0, 12}, 4);
    w.b.Add(0x3000001000ull, "Enemy_DungeonBoss_Barinade", kBoss, {0, 0, 10}, 24);
    w.b.Add(0x3000002000ull, "Enemy_DungeonBoss_Barinade_Arm", kArm, {0, 0, 12}, 4);
    w.Run(2 * kS);
    CHECK(w.Owner(kBoss) == 1);
    CHECK(w.Owner(kArm) == 1);
    CHECK(w.Group(kBoss) != 0);
    CHECK(w.Group(kArm) == w.Group(kBoss));
    w.Run(9 * kS);  // past the cooldowns
    // B hits the arm from close by. Since upstream's gohma-sync a boss hit that takes life stays
    // with the owner by default (one owner for the fight; a stun hands it over:
    // tests/server/session_test.cpp), so the boss and its arm are still A's.
    w.b.link = {0, 0, 13};
    w.Run(200'000);
    w.b.At(0x3000002000ull).life = 3;
    w.Run(3 * kS);
    CHECK(w.Owner(kBoss) == 1);
    CHECK(w.Owner(kArm) == 1);
    CHECK(w.Group(kArm) == w.Group(kBoss));
}

// Boss rooms starting together (the pull in, the shared intro skip) are the shared code's own
// rules: tests/lichii/boss_test.cpp, which this program builds too (build_tests.py).
