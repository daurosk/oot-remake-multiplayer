// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <algorithm>
#include <ostream>
#include <optional>
#include <set>
#include <string>
#include <utility>
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

TEST_CASE("boss scenes: the settings a boss's own logic asks start its intro and death (Gohma on Longshot)") {
    // 0.11 run (deku-test-b): Gohma's logic asked InsideTheDekuTree_BossAppear, whose flow plays Dm_Deku_BossIn as
    // a sub-event the request wrap never sees; the intro was not shared.
    CHECK(CoopBossScenes::OfEvent("InsideTheDekuTree_BossAppear") ==
          std::optional(std::pair{kGohma, CoopBossScenes::Kind::intro}));
    CHECK(CoopBossScenes::OfEvent("InsideTheDekuTree_BossDie") ==
          std::optional(std::pair{kGohma, CoopBossScenes::Kind::outro}));
    CHECK(CoopBossScenes::OfEvent("InsideTheDekuTree_BossDie_DreamWorld") ==
          std::optional(std::pair{kGohma, CoopBossScenes::Kind::outro}));
    CHECK(CoopBossScenes::OfEvent("SageOfFire_DefeatBoss") ==
          std::optional(std::pair{std::uint8_t{4}, CoopBossScenes::Kind::outro}));
    CHECK(CoopBossScenes::OfEvent("InsideGanonCastle_Dungeon_BossDie_2") ==
          std::optional(std::pair{std::uint8_t{9}, CoopBossScenes::Kind::outro}));
    CHECK_FALSE(CoopBossScenes::OfEvent("InsideGanonCastle_Dungeon_Boss_GetMasterSword"));  // mid-fight
    CHECK_FALSE(CoopBossScenes::OfEvent(""));

    Director a, b;
    Actions out = a.Tick(At(kInRoom, 1, true), Took("InsideTheDekuTree_BossAppear"), 1'000'000);
    REQUIRE(out.send.size() == 1);
    CHECK(out.send[0].kind == proto::BossSceneKind::intro_start);
    REQUIRE_FALSE(out.log.empty());
    CHECK(out.log.back() == "Dm_Deku_BossIn: our game started it: the others in the area play it too (asked as "
                            "InsideTheDekuTree_BossAppear)");
    proto::BossScene start = out.send[0];
    start.player_number = 1;
    b.Receive(start);
    // The joiner plays the intro's own flow (the movie), not the setting that would run the fight's start again.
    CHECK(b.Tick(At(kBeforeDoor, 2), {}, 1'100'000).play == std::optional<std::string>("Dm_Deku_BossIn"));

    out = a.Tick(At(kInRoom, 1, true), Took("InsideTheDekuTree_BossDie"), 100'000'000);
    REQUIRE(out.send.size() == 1);
    CHECK(out.send[0].kind == proto::BossSceneKind::outro_start);
    proto::BossScene death = out.send[0];
    death.player_number = 1;
    b.Receive(death);
    CHECK(b.Tick(At(kInRoom, 2), {}, 100'100'000).play == std::optional<std::string>("Dm_Deku_BossOut"));
}

TEST_CASE("boss scenes: our intro or death seen with no name starts a fresh intro, never a leftover") {
    // 0.11 run 4 (boss-intro): Gohma's game asked only InsideTheDekuTree_BossArea_ShutIn; the older rules' own
    // reading of our event in the boss room names it instead.
    const auto searched = [](const char* name) { return std::vector<Request>{Request{name, true, true}}; };
    Director a;
    Actions out = a.Tick(At(kInRoom, 1, true), searched("Dm_Deku_BossIn"), 1'000'000);
    REQUIRE(out.send.size() == 1);
    CHECK(out.send[0].kind == proto::BossSceneKind::intro_start);
    CHECK(a.phase(kGohma) == Phase::intro);
    // Found again later in the fight (a name left in the session): nothing, no skip.
    a.Tick(At(kInRoom, 1, false), {}, 30'000'000);
    REQUIRE(a.phase(kGohma) == Phase::fight);
    out = a.Tick(At(kInRoom, 1, true), searched("Dm_Deku_BossIn"), 40'000'000);
    CHECK(out.send.empty());
    CHECK_FALSE(out.skip);
    // The death found by the search: shared once.
    const auto owner = [](Local l) {
        l.boss_ours = true;
        return l;
    };
    out = a.Tick(owner(At(kInRoom, 1, true)), searched("Dm_Deku_BossOut"), 100'000'000);
    REQUIRE(out.send.size() == 1);
    CHECK(out.send[0].kind == proto::BossSceneKind::outro_start);
    out = a.Tick(owner(At(kInRoom, 1, true)), searched("Dm_Deku_BossOut"), 100'500'000);
    CHECK(out.send.empty());
    // Beaten: a leftover intro starts nothing.
    a.Tick(At(kInRoom, 1, false), {}, 130'000'000);
    CHECK(a.Tick(At(kInRoom, 1, true), searched("Dm_Deku_BossIn"), 140'000'000).send.empty());
    // Another scene: nothing.
    Director c;
    CHECK(c.Tick(At(kInRoom, 3, true, "KokiriForest"), searched("Dm_Deku_BossIn"), 1'000'000).send.empty());
    CHECK(c.phase(kGohma) == Phase::none);
}

TEST_CASE("boss scenes: another's death is asked as the boss's own death setting in its dungeon (report 88)") {
    // The setting's flow entry moves our quest on (Get_StoneOfForest DefeatBoss, GoToWarpPoint): the room makes
    // the blue warp in our game too. The death flow alone moved nothing, so a guest's warp did nothing.
    proto::BossScene death = Scene(1, kGohma, proto::BossSceneKind::outro_start, kInRoom);
    const auto with_boss = [](Local l) {
        l.boss_ours = false;  // our walk has the boss (a copy)
        return l;
    };
    SUBCASE("asked as the setting, taken: played with the owner's, no fallback") {
        Director b;
        b.Receive(death);
        CHECK(b.Tick(with_boss(At(kInRoom, 2)), {}, 1'000'000).play == std::optional<std::string>("InsideTheDekuTree_BossDie"));
        Actions out = b.Tick(with_boss(At(kInRoom, 2, true)), Took("InsideTheDekuTree_BossDie"), 1'100'000);
        CHECK_FALSE(out.play);
        CHECK(out.send.empty());  // player 1 started it
        CHECK(b.phase(kGohma) == Phase::outro);
    }
    SUBCASE("our copy lives at the death: the setting waits for it to die (report 105, review boss-0113)") {
        // The real order: the owner's outro_start comes while our copy is held alive (life 1); the glue's
        // SetDying releases it only after this Tick, so the copy can read dead on a later tick at the earliest.
        const auto alive = [&with_boss](Local l, std::optional<bool> lives) {
            l = with_boss(l);
            l.boss_alive = lives;
            return l;
        };
        const auto said = [](const Actions& out, const char* text) {
            return std::any_of(out.log.begin(), out.log.end(),
                               [&](const std::string& l) { return l.find(text) != std::string::npos; });
        };
        {
            // Released, it dies in its own game a few frames later: the setting (quest step and warp here).
            Director b;
            b.Receive(death);
            Actions out = b.Tick(alive(At(kInRoom, 2), true), {}, 1'000'000);
            CHECK_FALSE(out.play);  // never the setting while it lives (Revirie's black screen)
            CHECK(said(out, "our copy still lives: waiting for it to die here"));
            CHECK(b.phase(kGohma) == Phase::outro);
            CHECK_FALSE(b.Tick(alive(At(kInRoom, 2), true), {}, 1'250'000).play);
            out = b.Tick(alive(At(kInRoom, 2), false), {}, 1'500'000);
            CHECK(out.play == std::optional<std::string>("InsideTheDekuTree_BossDie"));
            out = b.Tick(alive(At(kInRoom, 2, true), false), Took("InsideTheDekuTree_BossDie"), 1'600'000);
            CHECK_FALSE(out.play);
            // Its end: done, nothing asked again. While the death event runs nothing is refused by us (the
            // manager refuses the boss logic's asks itself); once it is over, a second death is refused.
            out = b.Tick(alive(At(kInRoom, 2, true), false), {}, 5'000'000);
            CHECK(out.refuse_deaths == 0);
            out = b.Tick(alive(At(kInRoom, 2), std::nullopt), {}, 6'000'000);
            CHECK_FALSE(out.play);
            CHECK(b.phase(kGohma) == Phase::done);
            CHECK(out.refuse_deaths == (1u << kGohma));
            CHECK_FALSE(b.Tick(alive(At(kInRoom, 2), false), {}, 7'000'000).play);
        }
        {
            // Still alive after kCopyWaitUs (a game built without patch 0007's LIFE_STOPS, a stop holding it): the
            // death flow alone, and that is its one death cutscene here (review-boss-0114 risk 3): nothing is asked
            // when the copy dies later, and our game's own late ask of the setting is refused at the request.
            Director b;
            b.Receive(death);
            b.Tick(alive(At(kInRoom, 2), true), {}, 1'000'000);
            Actions out = b.Tick(alive(At(kInRoom, 2), true), {}, 1'000'000 + Director::kCopyWaitUs);
            CHECK(out.play == std::optional<std::string>("Dm_Deku_BossOut"));
            CHECK(said(out, "nothing asked again"));
            CHECK(out.refuse_deaths == 0);  // (not played yet)
            out = b.Tick(alive(At(kInRoom, 2, true), true), Took("Dm_Deku_BossOut"), 2'600'000);
            CHECK_FALSE(out.play);
            CHECK(out.refuse_deaths == 0);  // (its event runs)
            CHECK_FALSE(b.Tick(alive(At(kInRoom, 2, true), false), {}, 8'000'000).play);  // the flow still runs
            out = b.Tick(alive(At(kInRoom, 2), false), {}, 9'000'000);  // the flow's end; the copy reads dead
            CHECK(b.phase(kGohma) == Phase::done);
            CHECK_FALSE(out.play);  // never the setting after the flow: no second cutscene
            CHECK(out.refuse_deaths == (1u << kGohma));
            for (std::uint64_t t = 9'250'000; t < 40'000'000; t += 250'000) {
                CHECK_FALSE(b.Tick(alive(At(kInRoom, 2), false), {}, t).play);
            }
            // The copy's own death logic asks the setting late: the request refuses it (RefuseAsk), and the
            // refusal asks no flow either.
            CoopBossScenes::RefusedDeaths().store(out.refuse_deaths);
            CHECK(CoopBossScenes::RefuseAsk("InsideTheDekuTree_BossDie"));
            CHECK(CoopBossScenes::RefuseAsk("Dm_Deku_BossOut"));
            CHECK_FALSE(CoopBossScenes::RefuseAsk("InsideTheDekuTree_BossAppear"));  // (an intro: never)
            CHECK_FALSE(CoopBossScenes::RefuseAsk("Dm_Dodongo_BossOut"));             // (another boss's)
            CoopBossScenes::RefusedDeaths().store(0);
            CHECK_FALSE(CoopBossScenes::RefuseAsk("InsideTheDekuTree_BossDie"));
            out = b.Tick(alive(At(kInRoom, 2), false), {Request{"InsideTheDekuTree_BossDie", false}}, 40'000'000);
            CHECK_FALSE(out.play);
            CHECK(said(out, "refused at the request"));
            // Its logic asks again (every frame): said once per fight (review-boss-0114 nit).
            out = b.Tick(alive(At(kInRoom, 2), false), {Request{"InsideTheDekuTree_BossDie", false}}, 40'100'000);
            CHECK_FALSE(out.play);
            CHECK_FALSE(said(out, "refused at the request"));
            // Out of its area: nothing refused (the bit is only for where its death played).
            CHECK(b.Tick(alive(At(kFar, 2), false), {}, 41'000'000).refuse_deaths == 0);
        }
        {
            // Our copy's own death logic asks the setting itself while we wait: nothing more asked.
            Director b;
            b.Receive(death);
            b.Tick(alive(At(kInRoom, 2), true), {}, 1'000'000);
            Actions out = b.Tick(alive(At(kInRoom, 2, true), false), Took("InsideTheDekuTree_BossDie"), 1'250'000);
            CHECK_FALSE(out.play);
            CHECK_FALSE(b.Tick(alive(At(kInRoom, 2, true), false), {}, 3'000'000).play);
        }
        {
            // Left the area while waiting: nothing asked.
            Director b;
            b.Receive(death);
            b.Tick(alive(At(kInRoom, 2), true), {}, 1'000'000);
            CHECK_FALSE(b.Tick(alive(At(kFar, 2), false), {}, 1'250'000).play);
            CHECK_FALSE(b.Tick(alive(At(kInRoom, 2), false), {}, 1'500'000).play);
        }
        // Our copy already dead (or gone from the walk) when the death comes: the setting, as before.
        Director d;
        d.Receive(death);
        CHECK(d.Tick(alive(At(kInRoom, 2), false), {}, 1'000'000).play ==
              std::optional<std::string>("InsideTheDekuTree_BossDie"));
        // The owner's own game (boss ours) never comes here with its own death; another's death while we own
        // the boss (a hand-over in flight) still asks the setting.
        Director e;
        e.Receive(death);
        Local owner = At(kInRoom, 2);
        owner.boss_ours = true;
        owner.boss_alive = true;
        CHECK(e.Tick(owner, {}, 1'000'000).play == std::optional<std::string>("InsideTheDekuTree_BossDie"));
    }
    SUBCASE("the setting refused: the death flow alone, once") {
        Director b;
        b.Receive(death);
        b.Tick(with_boss(At(kInRoom, 2)), {}, 1'000'000);
        Actions out = b.Tick(with_boss(At(kInRoom, 2)), {Request{"InsideTheDekuTree_BossDie", false}}, 1'100'000);
        CHECK(out.play == std::optional<std::string>("Dm_Deku_BossOut"));
        CHECK(out.refuse_deaths == 0);  // nothing played: nothing refused
        out = b.Tick(with_boss(At(kInRoom, 2)), {Request{"Dm_Deku_BossOut", false}}, 1'200'000);
        CHECK_FALSE(out.play);  // the flow refused too: nothing more
        CHECK(out.refuse_deaths == 0);
    }
    SUBCASE("the owner's own death: one cutscene, its later asks refused once its event is over") {
        Director b;
        Local owner = At(kInRoom, 1, true);
        owner.boss_ours = true;
        Actions out = b.Tick(owner, Took("InsideTheDekuTree_BossDie"), 1'000'000);
        CHECK(b.phase(kGohma) == Phase::outro);
        CHECK(out.refuse_deaths == 0);  // its event runs: the boss logic's repeated asks are the manager's to refuse
        out = b.Tick(owner, {Request{"InsideTheDekuTree_BossDie", false}}, 1'100'000);
        CHECK_FALSE(out.play);
        owner.event = false;
        out = b.Tick(owner, {}, 9'000'000);
        CHECK(out.refuse_deaths == (1u << kGohma));
    }
    SUBCASE("a refusal we did not ask (too late) asks nothing") {
        Director b;
        b.Receive(death);
        b.Tick(with_boss(At(kInRoom, 2)), {}, 1'000'000);
        CHECK_FALSE(b.Tick(with_boss(At(kInRoom, 2)), {Request{"InsideTheDekuTree_BossDie", false}},
                           1'000'000 + Director::kJoinUs + 1).play);
    }
    SUBCASE("King Dodongo and Barinade: their quests' own death settings") {
        const auto boss = [](std::string_view name) {
            for (std::size_t i = 0; i < CoopBossScenes::Bosses().size(); ++i) {
                if (CoopBossScenes::Bosses()[i].name == name) {
                    return static_cast<std::uint8_t>(i);
                }
            }
            return std::uint8_t{0xff};
        };
        for (const auto& [name, setting] : {std::pair{"King Dodongo", "Get_StoneOfFire_DefeatBoss"},
                                            std::pair{"Barinade", "Get_StoneOfWater_DestroyBoss"}}) {
            const std::uint8_t i = boss(name);
            REQUIRE(i < CoopBossScenes::Bosses().size());
            const auto* room = CoopBoss::RoomByIndex(static_cast<std::uint8_t>(CoopBossScenes::Bosses()[i].room));
            REQUIRE(room != nullptr);
            const Vec centre{room->centre_x, (room->y_low + room->y_high) / 2.0f, room->centre_z};
            Director b;
            b.Receive(Scene(1, i, proto::BossSceneKind::outro_start, centre, std::string(room->scene)));
            CHECK(b.Tick(with_boss(At(centre, 2, false, std::string(room->scene))), {}, 1'000'000).play ==
                  std::optional<std::string>(setting));
        }
    }
    SUBCASE("no boss in our walk (our save beat it already, or the copy is gone): the death flow") {
        // King Dodongo's and Barinade's settings would set a Complete quest back and signal an absent boss.
        Director b;
        b.Receive(death);
        CHECK(b.Tick(At(kInRoom, 2), {}, 1'000'000).play == std::optional<std::string>("Dm_Deku_BossOut"));
    }
    SUBCASE("the boss rush and Ganondorf: the death flow, as before") {
        Director b;
        const Vec starter{0.0f, 0.0f, 0.0f};
        b.Receive(Scene(1, kGohma, proto::BossSceneKind::outro_start, starter, "GohmaStage"));
        CHECK(b.Tick(with_boss(At(Vec{10.0f, 0.0f, 0.0f}, 2, false, "GohmaStage")), {}, 1'000'000).play ==
              std::optional<std::string>("Dm_Deku_BossOut"));
        Director g;
        g.Receive(Scene(1, kGanondorf, proto::BossSceneKind::outro_start, starter, "InsideGanonCastle"));
        CHECK(g.Tick(with_boss(At(Vec{10.0f, 0.0f, 0.0f}, 2, false, "InsideGanonCastle")), {}, 1'000'000).play ==
              std::optional<std::string>("Dm_InsideGanonCastle_BossOut"));
    }
}

TEST_CASE("boss scenes: a copy that dies in its own game starts no death, the owner's does (review boss-intro)") {
    // A's eye hit judged lethal on A's copy while the owner B refused it: A's walk reads its boss gone.
    const auto searched = [](const char* name) { return std::vector<Request>{Request{name, true, true}}; };
    Director a;
    a.Tick(At(kInRoom, 1, true), searched("Dm_Deku_BossIn"), 1'000'000);
    a.Tick(At(kInRoom, 1, false), {}, 30'000'000);
    REQUIRE(a.phase(kGohma) == Phase::fight);
    Local copy = At(kInRoom, 1, true);
    copy.boss_ours = false;
    Actions out = a.Tick(copy, searched("Dm_Deku_BossOut"), 60'000'000);
    CHECK(out.send.empty());
    CHECK_FALSE(out.play);
    CHECK(a.phase(kGohma) == Phase::fight);
    Local unknown = At(kInRoom, 1, true);  // no walk reading: not the owner's either
    CHECK(a.Tick(unknown, searched("Dm_Deku_BossOut"), 61'000'000).send.empty());
    // The owner's death comes: then A plays it.
    a.Receive(Scene(2, kGohma, proto::BossSceneKind::outro_start, kInRoom));
    out = a.Tick(At(kInRoom, 1, false), {}, 62'000'000);
    CHECK(out.play == std::optional<std::string>("Dm_Deku_BossOut"));
    // A death our game asked by name (a request) is still ours to share, owner or not (as before).
    Director c;
    c.Tick(At(kInRoom, 3, true), searched("Dm_Deku_BossIn"), 1'000'000);
    c.Tick(At(kInRoom, 3, false), {}, 30'000'000);
    CHECK(c.Tick(At(kInRoom, 3, true), Took("Dm_Deku_BossOut"), 60'000'000).send.size() == 1);
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
    SUBCASE("the players who did not take it get their hearts filled, once, the one who took it does not") {
        const proto::BossHeart given{1, kGohma, true, 16};
        a.Receive(given);
        c.Receive(given);
        CHECK_FALSE(a.Tick(with(1, 16), {}, 2'000'000).refill);
        CHECK(c.Tick(with(3, 12), {}, 2'000'000).refill);
        CHECK_FALSE(c.Tick(with(3, 16), {}, 2'100'000).refill);   // (once)
        c.Receive(given);                                         // (the answer again: no second refill)
        CHECK_FALSE(c.Tick(with(3, 16), {}, 2'200'000).refill);
    }
    SUBCASE("a maximum another game's pickup raised (the shared MaxLife flag) sends no claim") {
        // Four players, 2026-11-20: every game claimed when the flag raised its maximum, and the first claim to reach
        // the server named the wrong player.
        Local l = with(2, 16);
        l.max_life_from_others = true;
        CHECK(b.Tick(l, {}, 2'000'000).claims.empty());
        CHECK(b.Tick(with(2, 16), {}, 2'100'000).claims.empty());   // (once: not claimed later either)
        CHECK(a.Tick(with(1, 16), {}, 2'000'000).claims.size() == 1);   // the one who took it still claims
    }
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
    SUBCASE("in the beaten boss's area our own maximum is capped at one container, then at the answer") {
        // Before any answer: our maximum when the boss died + one container (the flag watcher holds it).
        CHECK(b.Tick(with(2, 12), {}, 1'500'000).max_life_cap == std::optional<std::int32_t>(16));
        CHECK(a.Tick(with(1, 16), {}, 2'000'000).max_life_cap == std::optional<std::int32_t>(16));
        b.Receive(proto::BossHeart{1, kGohma, true, 16});
        Actions ob = b.Tick(with(2, 12), {}, 2'100'000);
        CHECK(ob.set_max_life == std::optional<std::int32_t>(16));
        CHECK(ob.max_life_cap == std::optional<std::int32_t>(16));  // a second pickup can't pass it
        CHECK(ob.claims.empty());
        // Outside the area: no cap (no container there), the answer still raises.
        CHECK_FALSE(c.Tick(with(3, 12, kFar), {}, 2'100'000).max_life_cap);
        // A diverged save above the winner's: never capped below its own maximum.
        Director d;
        d.Receive(Scene(1, kGohma, proto::BossSceneKind::outro_start, kInRoom));
        d.Tick(with(4, 24), {}, 1'000'000);
        d.Tick(with(4, 24), {}, 1'100'000);
        d.Receive(proto::BossHeart{1, kGohma, true, 16});
        Actions od = d.Tick(with(4, 24), {}, 1'200'000);
        CHECK(od.max_life_cap == std::optional<std::int32_t>(24));
        CHECK_FALSE(od.set_max_life);
    }
    SUBCASE("a claim past what the message carries is not sent") {
        Director d;
        d.Receive(Scene(1, kGohma, proto::BossSceneKind::outro_start, kInRoom));
        d.Tick(with(4, 80), {}, 1'000'000);
        CHECK(d.Tick(with(4, 84), {}, 2'000'000).claims.empty());
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

TEST_CASE("boss scenes: the door's event ending as our game takes the intro does not end the intro") {
    Director a;
    a.Tick(At(kInRoom, 1, true), {}, 500'000);  // the boss door's own event runs
    // Our game takes the intro on the tick the door's event ended, before the intro runs.
    Actions out = a.Tick(At(kInRoom, 1, false), Took("Dm_Deku_BossIn"), 1'000'000);
    REQUIRE(out.send.size() == 1);
    CHECK(out.send[0].kind == proto::BossSceneKind::intro_start);
    CHECK(a.phase(kGohma) == Phase::intro);
    a.Tick(At(kInRoom, 1, true), {}, 1'100'000);  // the intro runs
    CHECK(a.phase(kGohma) == Phase::intro);
    out = a.Tick(At(kInRoom, 1, false), {}, 20'000'000);  // and ends: now it counts
    REQUIRE(out.send.size() == 1);
    CHECK(out.send[0].kind == proto::BossSceneKind::intro_end);
}

TEST_CASE("boss scenes: the cap's base is our maximum when the death was heard, wherever we were") {
    // A player outside the area hears the death at 12, then the winner's 16 comes by the shared flag,
    // then it walks in before the server's answer: one container on 12, not on 16.
    Director e;
    auto with = [](std::int32_t max, const Vec& p) {
        Local l = At(p, 5);
        l.max_life = max;
        return l;
    };
    e.Tick(with(12, kFar), {}, 500'000);
    e.Receive(Scene(1, kGohma, proto::BossSceneKind::outro_start, kInRoom));
    CHECK_FALSE(e.Tick(with(12, kFar), {}, 1'000'000).max_life_cap);  // outside: no cap
    CHECK(e.Tick(with(16, kFar), {}, 2'000'000).claims.empty());     // the flag's 16, outside
    Actions out = e.Tick(with(16, kInRoom), {}, 3'000'000);
    CHECK(out.max_life_cap == std::optional<std::int32_t>(16));
    CHECK(out.claims.empty());
}

TEST_CASE("boss scenes: the boss's own start of the fight is never skipped (player report #30)") {
    // The 0.11.0 run: our intro (seen without a name) ended, then Gohma's own logic asked
    // InsideTheDekuTree_BossAppear in our game; it was force-skipped as a late arrival's.
    Director b;
    std::vector<Request> seen{Request{"Dm_Deku_BossIn", true, true}};
    b.Tick(At(kInRoom, 1, true), seen, 1'000'000);
    b.Tick(At(kInRoom, 1, true), {}, 2'000'000);
    b.Tick(At(kInRoom, 1, false), {}, 10'000'000);  // ours is over: the fight is on
    REQUIRE(b.phase(kGohma) == Phase::fight);
    Actions out = b.Tick(At(kInRoom, 1, true), Took("InsideTheDekuTree_BossAppear"), 11'000'000);
    CHECK_FALSE(out.skip);
    CHECK_FALSE(b.Tick(At(kInRoom, 1, true), {}, 11'200'000).skip);
    // The flow itself, late: still skipped as before.
    Director late;
    late.Receive(Scene(2, kGohma, proto::BossSceneKind::intro_start, kInRoom));
    late.Tick(At(kFar, 1), {}, 1'000'000);
    late.Receive(Scene(2, kGohma, proto::BossSceneKind::intro_end, kInRoom));
    late.Tick(At(kFar, 1), {}, 20'000'000);
    CHECK(late.Tick(At(kInRoom, 1, true), Took("Dm_Deku_BossIn"), 30'000'000).skip);
}

TEST_CASE("boss scenes: a follower's own BossAppear plays out when the starter's intro ends") {
    Director b;
    b.Receive(Scene(2, kGohma, proto::BossSceneKind::intro_start, kInRoom));
    b.Tick(At(kInRoom, 1), {}, 1'000'000);
    b.Tick(At(kInRoom, 1, true), Took("InsideTheDekuTree_BossAppear"), 1'500'000);
    b.Receive(Scene(2, kGohma, proto::BossSceneKind::intro_end, kInRoom));
    CHECK_FALSE(b.Tick(At(kInRoom, 1, true), {}, 5'000'000).skip);
    CHECK(b.phase(kGohma) == Phase::fight);
}

TEST_CASE("boss scenes: DeathMayRun says while our own death of a boss may still run here (row 136)") {
    const proto::BossScene death = Scene(1, kGohma, proto::BossSceneKind::outro_start, kInRoom);
    const auto copy = [](Local l) {
        l.boss_ours = false;
        l.boss_alive = false;
        return l;
    };
    SUBCASE("nothing heard, nothing asked: nothing of ours runs it") {
        Director b;
        CHECK_FALSE(b.DeathMayRun(kGohma));
        b.Tick(At(kInRoom, 2), {}, 1'000'000);
        CHECK_FALSE(b.DeathMayRun(kGohma));
        CHECK_FALSE(b.DeathMayRun(200));
    }
    SUBCASE("an outro_start not yet taken, asked, playing, over") {
        Director b;
        b.Receive(death);
        CHECK(b.DeathMayRun(kGohma));  // the next Tick joins it
        CHECK_FALSE(b.DeathMayRun(1));  // another boss
        CHECK(b.Tick(copy(At(kInRoom, 2)), {}, 1'000'000).play);
        CHECK(b.DeathMayRun(kGohma));  // asked, not taken yet
        b.Tick(copy(At(kInRoom, 2, true)), Took("InsideTheDekuTree_BossDie"), 1'100'000);
        CHECK(b.DeathMayRun(kGohma));  // it plays
        b.Tick(copy(At(kInRoom, 2)), {}, 9'000'000);
        CHECK(b.phase(kGohma) == Phase::done);
        CHECK_FALSE(b.DeathMayRun(kGohma));  // over: its beaten set may be given here
    }
    SUBCASE("asked but never taken: nothing runs once kJoinUs is past") {
        Director b;
        b.Receive(death);
        CHECK(b.Tick(copy(At(kInRoom, 2)), {}, 1'000'000).play);
        b.Tick(copy(At(kInRoom, 2)), {}, 1'000'000 + Director::kJoinUs / 2);
        CHECK(b.DeathMayRun(kGohma));
        b.Tick(copy(At(kInRoom, 2)), {}, 1'000'000 + Director::kJoinUs + 1);
        CHECK_FALSE(b.DeathMayRun(kGohma));
    }
    SUBCASE("our copy still lives: the setting waits for it") {
        Director b;
        b.Receive(death);
        Local alive = At(kInRoom, 2);
        alive.boss_ours = false;
        alive.boss_alive = true;
        CHECK_FALSE(b.Tick(alive, {}, 1'000'000).play);
        CHECK(b.DeathMayRun(kGohma));
    }
    SUBCASE("came into the area after the death (walked in during it, fell in later): nothing asked, nothing runs") {
        Director b;
        b.Receive(death);
        CHECK_FALSE(b.Tick(At(kFar, 2), {}, 1'000'000).play);  // elsewhere in the dungeon at the death: a chat line
        CHECK(b.phase(kGohma) == Phase::outro);
        CHECK_FALSE(b.DeathMayRun(kGohma));
        CHECK_FALSE(b.Tick(At(kInRoom, 2), {}, 2'000'000).play);  // in the room during the others' death
        CHECK_FALSE(b.DeathMayRun(kGohma));
    }
    SUBCASE("any event of our game: it may be the death (seen with no name)") {
        Director b;
        b.Tick(At(kInRoom, 2, true), {}, 1'000'000);
        CHECK(b.DeathMayRun(kGohma));
        b.Tick(At(kInRoom, 2), {}, 2'000'000);
        CHECK_FALSE(b.DeathMayRun(kGohma));
    }
}
