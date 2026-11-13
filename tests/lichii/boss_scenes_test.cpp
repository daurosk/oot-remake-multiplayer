// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "coop_boss_scenes.h"

namespace proto = coop::protocol;
using CoopBossScenes::Actions;
using CoopBossScenes::Director;
using CoopBossScenes::Local;
using CoopBossScenes::Request;
using CoopBossScenes::Vec;
using Phase = CoopBossScenes::Director::Phase;

namespace {

constexpr std::uint8_t kGohma = 0;
constexpr std::uint8_t kGanondorf = 8;
const std::string kDeku = "InsideTheDekuTree";
// Gohma's room: centre (7.5, 67.2), radius 37, heights 43 to 63.
const Vec kInRoom{7.5f, 50.0f, 67.2f};
const Vec kBeforeDoor{7.5f + 44.0f, 50.0f, 67.2f};  // outside the circle, inside the antechamber
const Vec kFar{7.5f + 80.0f, 50.0f, 67.2f};         // elsewhere in the dungeon

Local At(const Vec& p, std::uint8_t self, bool event = false, std::string scene = kDeku) {
    Local l;
    l.scene = std::move(scene);
    l.position = p;
    l.self = self;
    l.event = event;
    return l;
}

proto::BossScene Scene(std::uint8_t from, std::uint8_t boss, proto::BossSceneKind kind, const Vec& at,
                       std::string scene = kDeku) {
    return proto::BossScene{from, std::move(scene), boss, kind, {at.x, at.y, at.z}};
}

std::vector<Request> Took(const char* name) {
    return {Request{name, true}};
}

} // namespace

TEST_CASE("boss scenes: ten bosses, every intro and death event known, no mini-boss") {
    CHECK(CoopBossScenes::Bosses().size() == 10);
    std::set<std::string_view> names;
    for (const auto& b : CoopBossScenes::Bosses()) {
        names.insert(b.intro);
        names.insert(b.outro);
        if (b.room >= 0) {
            const auto* room = CoopBoss::RoomByIndex(static_cast<std::uint8_t>(b.room));
            REQUIRE(room != nullptr);
            CHECK(room->intro == b.intro);
            CHECK(room->outro == b.outro);
        }
    }
    CHECK(names.size() == 20);
    CHECK(CoopBossScenes::OfEvent("Dm_FinalBattle_BossOut") == std::pair{std::uint8_t{9}, CoopBossScenes::Kind::outro});
    CHECK_FALSE(CoopBossScenes::OfEvent("Dm_Deku_BossIn_Effect"));  // a sub-flow, not the event
    CHECK_FALSE(CoopBossScenes::OfEvent("Dm_FinalBattle_0005"));
}

TEST_CASE("boss scenes: the area is the boss room and the room before its door") {
    const auto& gohma = CoopBossScenes::Bosses()[kGohma];
    CHECK(CoopBossScenes::InArea(gohma, kDeku, std::nullopt, kDeku, kInRoom));
    CHECK(CoopBossScenes::InRoom(gohma, kDeku, std::nullopt, kDeku, kInRoom));
    CHECK(CoopBossScenes::InArea(gohma, kDeku, std::nullopt, kDeku, kBeforeDoor));
    CHECK_FALSE(CoopBossScenes::InRoom(gohma, kDeku, std::nullopt, kDeku, kBeforeDoor));
    CHECK_FALSE(CoopBossScenes::InArea(gohma, kDeku, std::nullopt, kDeku, kFar));
    CHECK_FALSE(CoopBossScenes::InArea(gohma, kDeku, std::nullopt, kDeku, Vec{7.5f, 20.0f, 67.2f}));  // a floor below
    CHECK_FALSE(CoopBossScenes::InArea(gohma, kDeku, std::nullopt, "KokiriForest", kInRoom));
    // No room in the table (Ganondorf): the starter's scene, near the starter.
    const auto& ganondorf = CoopBossScenes::Bosses()[kGanondorf];
    const Vec starter{0.0f, 100.0f, 0.0f};
    CHECK(CoopBossScenes::InArea(ganondorf, "InsideGanonCastle", starter, "InsideGanonCastle", Vec{20.0f, 100.0f, 0.0f}));
    CHECK_FALSE(
        CoopBossScenes::InArea(ganondorf, "InsideGanonCastle", starter, "InsideGanonCastle", Vec{40.0f, 100.0f, 0.0f}));
    CHECK_FALSE(CoopBossScenes::InArea(ganondorf, "InsideGanonCastle", std::nullopt, "InsideGanonCastle", starter));
}

TEST_CASE("boss scenes: the first game's intro plays in every game of the area, not far away") {
    Director a, b, c;
    // A's game takes the intro: it is the starter.
    Actions out = a.Tick(At(kInRoom, 1, true), Took("Dm_Deku_BossIn"), 1'000'000);
    REQUIRE(out.send.size() == 1);
    CHECK(out.send[0].kind == proto::BossSceneKind::intro_start);
    CHECK(a.phase(kGohma) == Phase::intro);
    CHECK(a.starter(kGohma) == 1);
    proto::BossScene start = out.send[0];
    start.player_number = 1;  // as the server relays it

    // B stands before the door: plays it too and is moved into the room.
    b.Receive(start);
    out = b.Tick(At(kBeforeDoor, 2), {}, 1'100'000);
    CHECK(out.play == std::optional<std::string>("Dm_Deku_BossIn"));
    REQUIRE(out.move_to);
    CHECK(CoopBossScenes::InRoom(CoopBossScenes::Bosses()[kGohma], kDeku, std::nullopt, kDeku, *out.move_to));
    CHECK(out.chat.empty());
    CHECK(b.starter(kGohma) == 1);

    // C is elsewhere in the dungeon: no cutscene, no move, one chat line.
    c.Receive(start);
    out = c.Tick(At(kFar, 3), {}, 1'100'000);
    CHECK_FALSE(out.play);
    CHECK_FALSE(out.move_to);
    REQUIRE(out.chat.size() == 1);
    CHECK(out.chat[0].player == 1);
    CHECK(out.chat[0].text == "entered the boss room of Queen Gohma");
    c.Receive(start);
    CHECK(c.Tick(At(kFar, 3), {}, 1'200'000).chat.empty());  // once
}

TEST_CASE("boss scenes: every intro ends when the starter's ends, one skipping alone waits") {
    Director a, b;
    proto::BossScene start = a.Tick(At(kInRoom, 1, true), Took("Dm_Deku_BossIn"), 1'000'000).send.at(0);
    start.player_number = 1;
    b.Receive(start);
    CHECK(b.Tick(At(kInRoom, 2), {}, 1'100'000).play);
    b.Tick(At(kInRoom, 2, true), Took("Dm_Deku_BossIn"), 1'200'000);  // B's game took it (our ask)

    SUBCASE("the starter ends first: B's is skipped") {
        Actions out = a.Tick(At(kInRoom, 1, false), {}, 20'000'000);
        REQUIRE(out.send.size() == 1);
        proto::BossScene end = out.send[0];
        CHECK(end.kind == proto::BossSceneKind::intro_end);
        end.player_number = 1;
        b.Receive(end);
        out = b.Tick(At(kInRoom, 2, true), {}, 20'100'000);
        CHECK(out.skip);
        CHECK(b.phase(kGohma) == Phase::fight);
        CHECK_FALSE(b.Tick(At(kInRoom, 2, true), {}, 20'200'000).skip);  // every 0.5 s
        CHECK(b.Tick(At(kInRoom, 2, true), {}, 20'700'000).skip);
        CHECK_FALSE(b.Tick(At(kInRoom, 2, false), {}, 20'800'000).skip);  // gone
    }
    SUBCASE("B skips alone: nothing is sent, B waits for the starter's") {
        Actions out = b.Tick(At(kInRoom, 2, false), {}, 5'000'000);
        CHECK(out.send.empty());
        CHECK(b.phase(kGohma) == Phase::intro);
        CHECK_FALSE(a.Tick(At(kInRoom, 1, true), {}, 5'100'000).skip);  // A's intro goes on
    }
}

TEST_CASE("boss scenes: two games starting together agree on the lower number") {
    Director a, b;
    proto::BossScene sa = a.Tick(At(kInRoom, 1, true), Took("Dm_Deku_BossIn"), 1'000'000).send.at(0);
    proto::BossScene sb = b.Tick(At(kInRoom, 2, true), Took("Dm_Deku_BossIn"), 1'000'000).send.at(0);
    sa.player_number = 1;
    sb.player_number = 2;
    a.Receive(sb);
    b.Receive(sa);
    a.Tick(At(kInRoom, 1, true), {}, 1'100'000);
    b.Tick(At(kInRoom, 2, true), {}, 1'100'000);
    CHECK(a.starter(kGohma) == 1);
    CHECK(b.starter(kGohma) == 1);
    // B's own end sends nothing: only the starter's ends it.
    CHECK(b.Tick(At(kInRoom, 2, false), {}, 3'000'000).send.empty());
    CHECK(a.Tick(At(kInRoom, 1, false), {}, 3'000'000).send.size() == 1);
}

TEST_CASE("boss scenes: a late arrival goes straight into the fight") {
    Director b;
    proto::BossScene start = Scene(1, kGohma, proto::BossSceneKind::intro_start, kInRoom);
    proto::BossScene end = Scene(1, kGohma, proto::BossSceneKind::intro_end, kInRoom);
    b.Receive(start);
    b.Tick(At(kFar, 2), {}, 1'000'000);  // far: not played
    b.Receive(end);
    b.Tick(At(kFar, 2), {}, 20'000'000);
    CHECK(b.phase(kGohma) == Phase::fight);

    SUBCASE("walks in: its own intro is skipped at once, nothing sent") {
        Actions out = b.Tick(At(kInRoom, 2, true), Took("Dm_Deku_BossIn"), 30'000'000);
        CHECK(out.skip);
        CHECK(out.send.empty());
    }
    SUBCASE("stands before the door: moved in, a few tries") {
        Actions out = b.Tick(At(kBeforeDoor, 2), {}, 30'000'000);
        CHECK(out.move_to);
        CHECK_FALSE(b.Tick(At(kBeforeDoor, 2), {}, 31'000'000).move_to);
        CHECK(b.Tick(At(kBeforeDoor, 2), {}, 33'100'000).move_to);
        CHECK(b.Tick(At(kBeforeDoor, 2), {}, 36'200'000).move_to);
        CHECK_FALSE(b.Tick(At(kBeforeDoor, 2), {}, 39'300'000).move_to);  // 3 tries
    }
    SUBCASE("a game that never heard the start: told the fight is on, and skips") {
        Director late;
        proto::BossScene own = late.Tick(At(kInRoom, 3, true), Took("Dm_Deku_BossIn"), 30'000'000).send.at(0);
        own.player_number = 3;
        b.Receive(own);
        Actions out = b.Tick(At(kInRoom, 2), {}, 30'100'000);
        REQUIRE(out.send.size() == 1);
        proto::BossScene told = out.send[0];
        CHECK(told.kind == proto::BossSceneKind::intro_end);
        told.player_number = 2;
        late.Receive(told);
        CHECK(late.Tick(At(kInRoom, 3, true), {}, 30'200'000).skip);
        CHECK(late.phase(kGohma) == Phase::fight);
    }
}

TEST_CASE("boss scenes: the starter leaving during the intro blocks nobody") {
    Director b;
    b.Receive(Scene(1, kGohma, proto::BossSceneKind::intro_start, kInRoom));
    b.Tick(At(kInRoom, 2), {}, 1'000'000);
    b.Tick(At(kInRoom, 2, true), Took("Dm_Deku_BossIn"), 1'100'000);
    SUBCASE("left") {
        b.Receive(proto::PlayerLeft{1, proto::LeaveReason::timed_out});
        Actions out = b.Tick(At(kInRoom, 2, true), {}, 2'000'000);
        CHECK(b.phase(kGohma) == Phase::fight);
        CHECK_FALSE(out.skip);  // ours plays to its own end
        CHECK(b.Tick(At(kInRoom, 2, false), {}, 9'000'000).send.empty());
    }
    SUBCASE("no end ever heard") {
        b.Tick(At(kInRoom, 2, false), {}, 10'000'000);
        CHECK(b.phase(kGohma) == Phase::intro);
        b.Tick(At(kInRoom, 2, false), {}, 1'000'000 + Director::kIntroMaxUs);
        CHECK(b.phase(kGohma) == Phase::fight);
    }
    SUBCASE("reset on a disconnect") {
        b.Reset();
        CHECK(b.phase(kGohma) == Phase::none);
    }
}

TEST_CASE("boss scenes: the death plays in the area, outside only a chat line") {
    Director a, b, c;
    proto::BossScene death = a.Tick(At(kInRoom, 1, true), Took("Dm_Deku_BossOut"), 100'000'000).send.at(0);
    CHECK(death.kind == proto::BossSceneKind::outro_start);
    death.player_number = 1;
    b.Receive(death);
    Actions out = b.Tick(At(kInRoom, 2), {}, 100'100'000);
    CHECK(out.play == std::optional<std::string>("Dm_Deku_BossOut"));
    c.Receive(death);
    out = c.Tick(At(kFar, 3), {}, 100'100'000);
    CHECK_FALSE(out.play);
    REQUIRE(out.chat.size() == 1);
    CHECK(out.chat[0].text == "beat Queen Gohma");
    // B's game takes it (our ask), then it ends: done, B sends nothing (A started it).
    b.Tick(At(kInRoom, 2, true), Took("Dm_Deku_BossOut"), 100'200'000);
    CHECK(b.Tick(At(kInRoom, 2, false), {}, 120'000'000).send.empty());
    CHECK(b.phase(kGohma) == Phase::done);
    out = a.Tick(At(kInRoom, 1, false), {}, 120'000'000);
    REQUIRE(out.send.size() == 1);
    CHECK(out.send[0].kind == proto::BossSceneKind::outro_end);
    // A game whose own boss dies the same moment plays its own; it starts nothing new.
    Director d;
    d.Receive(death);
    d.Tick(At(kInRoom, 4), {}, 100'100'000);
    CHECK(d.Tick(At(kInRoom, 4, true), Took("Dm_Deku_BossOut"), 100'150'000).send.empty());
}

TEST_CASE("boss scenes: one heart container for all, never two") {
    Director a, b, c;
    auto beaten = [](Director& d, std::uint8_t self) {
        d.Receive(Scene(self == 1 ? 2 : 1, kGohma, proto::BossSceneKind::outro_start, kInRoom));
        Local l = At(kInRoom, self);
        l.max_life = 12;
        d.Tick(l, {}, 1'000'000);
    };
    beaten(a, 1);
    beaten(b, 2);
    beaten(c, 3);
    auto with = [](std::uint8_t self, std::int32_t max, const Vec& p = kInRoom) {
        Local l = At(p, self);
        l.max_life = max;
        return l;
    };
    SUBCASE("A and B grab it together: the server's one answer, one +1 each, C too") {
        Actions oa = a.Tick(with(1, 16), {}, 2'000'000);
        Actions ob = b.Tick(with(2, 16), {}, 2'000'000);
        REQUIRE(oa.claims.size() == 1);
        REQUIRE(ob.claims.size() == 1);
        CHECK(oa.claims[0] == proto::BossHeart{0, kGohma, false, 16});
        const proto::BossHeart given{1, kGohma, true, 16};  // the server took A's
        for (Director* d : {&a, &b, &c}) {
            d->Receive(given);
        }
        b.Receive(given);  // B's own claim answered too
        CHECK_FALSE(a.Tick(with(1, 16), {}, 2'100'000).set_max_life);
        CHECK_FALSE(b.Tick(with(2, 16), {}, 2'100'000).set_max_life);
        Actions oc = c.Tick(with(3, 12, kFar), {}, 2'100'000);  // outside the area: still gets it
        CHECK(oc.set_max_life == std::optional<std::int32_t>(16));
        CHECK(oc.claims.empty());
        // C's raise is not a pickup of its own.
        CHECK(c.Tick(with(3, 16), {}, 2'200'000).claims.empty());
    }
    SUBCASE("B picks it up after the answer came: put back, no second heart") {
        a.Tick(with(1, 16), {}, 2'000'000);
        b.Receive(proto::BossHeart{1, kGohma, true, 16});
        CHECK(b.Tick(with(2, 12), {}, 2'100'000).set_max_life == std::optional<std::int32_t>(16));
        Actions ob = b.Tick(with(2, 20), {}, 2'200'000);  // its game's own pickup on top
        CHECK(ob.set_max_life == std::optional<std::int32_t>(16));
        CHECK(ob.claims.empty());
    }
    SUBCASE("a rise before the boss is beaten is no claim") {
        Director d;
        d.Tick(with(4, 12), {}, 1'000'000);
        CHECK(d.Tick(with(4, 16), {}, 2'000'000).claims.empty());
    }
}

TEST_CASE("boss scenes: during the intro every joiner is put at its spot, the starter never") {
    const auto* room = CoopBoss::RoomByIndex(0);
    REQUIRE(room != nullptr);
    const Vec corridor{-12.0f, 48.5f, 66.5f};  // the way to Gohma's door: inside the room's circle
    CHECK(CoopBossScenes::InRoom(CoopBossScenes::Bosses()[kGohma], kDeku, std::nullopt, kDeku, corridor));
    Director a, b, c;
    proto::BossScene start = a.Tick(At(kInRoom, 1, true), Took("Dm_Deku_BossIn"), 1'000'000).send.at(0);
    start.player_number = 1;
    CHECK_FALSE(a.Tick(At(kInRoom, 1, true), {}, 1'100'000).move_to);  // the starter stays
    b.Receive(start);
    Actions out = b.Tick(At(corridor, 2), {}, 1'100'000);
    REQUIRE(out.move_to);
    const Vec spot = CoopBoss::ArrivalFor(*room, 2);
    CHECK(out.move_to->x == spot.x);
    CHECK(out.move_to->z == spot.z);
    // Already at its spot: left alone.
    c.Receive(start);
    CHECK_FALSE(c.Tick(At(CoopBoss::ArrivalFor(*room, 3), 3), {}, 1'100'000).move_to);
}

TEST_CASE("boss scenes: in a boss rush stage the area is near the starter, and nobody is moved") {
    const auto& gohma = CoopBossScenes::Bosses()[kGohma];
    const Vec starter{0.0f, 0.0f, 0.0f};
    CHECK(CoopBossScenes::InArea(gohma, "GohmaStage", starter, "GohmaStage", Vec{10.0f, 0.0f, 0.0f}));
    CHECK_FALSE(CoopBossScenes::InArea(gohma, "GohmaStage", starter, "GohmaStage", Vec{40.0f, 0.0f, 0.0f}));
    CHECK_FALSE(CoopBossScenes::InArea(gohma, "GohmaStage", starter, "KokiriForest", Vec{10.0f, 0.0f, 0.0f}));  // the starter's scene only
    Director b;
    b.Receive(Scene(1, kGohma, proto::BossSceneKind::intro_start, starter, "GohmaStage"));
    Actions out = b.Tick(At(Vec{10.0f, 0.0f, 0.0f}, 2, false, "GohmaStage"), {}, 1'000'000);
    CHECK(out.play == std::optional<std::string>("Dm_Deku_BossIn"));
    CHECK_FALSE(out.move_to);
}
