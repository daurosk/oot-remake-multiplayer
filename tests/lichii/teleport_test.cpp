// Tests for /teleport <player> (lichii/hook/coop_teleport.cpp).
#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <vector>

#include "coop_teleport.h"

namespace proto = coop::protocol;
using CoopTeleport::Command;
using CoopTeleport::Grounded;
using CoopTeleport::Local;
using CoopTeleport::Player;
using CoopTeleport::Trip;
using CoopTeleport::Vec;

namespace {

constexpr std::uint64_t kS = 1'000'000;

const std::vector<Player> kPlayers{{1, "Link"}, {2, "Saria"}, {3, "Sam"}, {4, ""}};

Local At(const char* scene, float x, float y, float z) {
    Local l;
    l.scene = scene;
    l.position = Vec{x, y, z};
    l.age = proto::Age::child;
    return l;
}

proto::JoinTarget Target(const char* scene, float x, float y, float z) {
    proto::JoinTarget t;
    t.scene = scene;
    t.position = {x, y, z};
    t.rotation = {0, 0, 0, 1};
    return t;
}

bool Has(const std::vector<std::string>& lines, const std::string& part) {
    for (const auto& l : lines) {
        if (l.find(part) != std::string::npos) return true;
    }
    return false;
}

} // namespace

TEST_CASE("teleport: the command is parsed, chat is not") {
    CHECK_FALSE(CoopTeleport::Parse("hello"));
    CHECK_FALSE(CoopTeleport::Parse("/teleportx saria"));
    CHECK_FALSE(CoopTeleport::Parse("/help"));
    const auto list = CoopTeleport::Parse("/teleport");
    REQUIRE(list);
    CHECK(list->kind == Command::Kind::list);
    const auto spaces = CoopTeleport::Parse("  /TELEPORT   ");
    REQUIRE(spaces);
    CHECK(spaces->kind == Command::Kind::list);
    const auto go = CoopTeleport::Parse("/Teleport  Saria the Kokiri ");
    REQUIRE(go);
    CHECK(go->kind == Command::Kind::go);
    CHECK(go->name == "Saria the Kokiri");
    const auto tp = CoopTeleport::Parse("/tp 2");
    REQUIRE(tp);
    CHECK(tp->name == "2");
}

TEST_CASE("teleport: the player is found by name, start of a name or number") {
    CHECK(CoopTeleport::FindPlayer("saria", kPlayers, 1).number == 2);
    CHECK(CoopTeleport::FindPlayer("SAM", kPlayers, 1).number == 3);  // whole name before a start
    CHECK(CoopTeleport::FindPlayer("sar", kPlayers, 1).number == 2);
    const auto both = CoopTeleport::FindPlayer("sa", kPlayers, 1);
    CHECK(both.number == 0);
    CHECK(both.error.find("Saria, Sam") != std::string::npos);
    CHECK(CoopTeleport::FindPlayer("4", kPlayers, 1).number == 4);
    CHECK(CoopTeleport::FindPlayer("p4", kPlayers, 1).name == "P4");
    CHECK(CoopTeleport::FindPlayer("link", kPlayers, 1).error == "teleport: that is you");
    CHECK(CoopTeleport::FindPlayer("1", kPlayers, 1).number == 0);
    const auto none = CoopTeleport::FindPlayer("mido", kPlayers, 1);
    CHECK(none.number == 0);
    CHECK(none.error.find("nobody called \"mido\"") != std::string::npos);
    // A start of our own name is not a match for us.
    const std::vector<Player> two{{1, "Sam"}, {2, "Saria"}};
    CHECK(CoopTeleport::FindPlayer("sa", two, 1).number == 2);
}

TEST_CASE("teleport: /teleport alone lists the others") {
    CHECK(CoopTeleport::ListLine(kPlayers, 1) == "teleport to: Saria (2), Sam (3), P4 (4). Type /teleport <name>");
    const std::vector<Player> alone{{1, "Link"}};
    CHECK(CoopTeleport::ListLine(alone, 1) == "teleport: nobody else is here");
}

TEST_CASE("teleport: the age of a scene comes from its name") {
    CHECK(CoopTeleport::AgeOfScene("KakarikoVillage_Child") == proto::Age::child);
    CHECK(CoopTeleport::AgeOfScene("KakarikoVillage_Adult") == proto::Age::adult);
    CHECK_FALSE(CoopTeleport::AgeOfScene("InsideTheDekuTree"));
    CHECK_FALSE(CoopTeleport::AgeOfScene("_Child"));
}

TEST_CASE("teleport: the grounded place is where Link last stood on its feet") {
    Grounded g;
    std::uint64_t t = 0;
    for (int i = 0; i < 5; ++i, t += kS / 10) {
        g.Add(At("Kokiri", 1.0f + i, 0.0f, 0.0f), t);  // walking on flat ground
    }
    REQUIRE(g.Last());
    CHECK(g.Last()->x == doctest::Approx(5.0f));
    CHECK(g.Scene() == "Kokiri");
    // A jump: the height changes fast; the place stays where Link stood.
    g.Add(At("Kokiri", 6.0f, 1.0f, 0.0f), t);
    t += kS / 10;
    g.Add(At("Kokiri", 7.0f, 1.5f, 0.0f), t);
    t += kS / 10;
    CHECK(g.Last()->x == doctest::Approx(5.0f));
    // Landed again: grounded only after kSteadyUs.
    g.Add(At("Kokiri", 8.0f, 0.5f, 0.0f), t);
    t += kS / 10;
    g.Add(At("Kokiri", 8.0f, 0.5f, 0.0f), t);
    CHECK(g.Last()->x == doctest::Approx(5.0f));
    t += kS / 2;
    g.Add(At("Kokiri", 8.0f, 0.5f, 0.0f), t);
    CHECK(g.Last()->x == doctest::Approx(8.0f));
    // An event: nothing new is taken.
    Local talk = At("Kokiri", 20.0f, 0.5f, 0.0f);
    talk.event = true;
    g.Add(talk, t + kS);
    g.Add(talk, t + 2 * kS);
    CHECK(g.Last()->x == doctest::Approx(8.0f));
}

TEST_CASE("teleport: the answer is refused when busy, in a boss fight or not grounded") {
    Grounded g;
    Local here = At("Kokiri", 3.0f, 0.0f, 4.0f);
    here.rotation = proto::Quat{0, 1, 0, 0};
    // Not stood still yet.
    auto a = CoopTeleport::AnswerFor(here, g);
    CHECK_FALSE(a.accept);
    CHECK(a.reason == proto::JoinRefuseReason::busy);
    g.Add(here, 0);
    g.Add(here, kS);
    a = CoopTeleport::AnswerFor(here, g);
    REQUIRE(a.accept);
    CHECK(a.target.scene == "Kokiri");
    CHECK(a.target.position == proto::Vec3{3.0f, 0.0f, 4.0f});
    CHECK(a.target.rotation == proto::Quat{0, 1, 0, 0});
    // Loading (no scene), an event, a fight.
    Local loading;
    CHECK(CoopTeleport::AnswerFor(loading, g).reason == proto::JoinRefuseReason::busy);
    Local talk = here;
    talk.event = true;
    CHECK(CoopTeleport::AnswerFor(talk, g).reason == proto::JoinRefuseReason::busy);
    Local fight = here;
    fight.fighting = true;
    CHECK(CoopTeleport::AnswerFor(fight, g).reason == proto::JoinRefuseReason::cannot_warp);
    // Another scene than where it stood: just loaded, not grounded there yet.
    CHECK_FALSE(CoopTeleport::AnswerFor(At("Hyrule", 0, 0, 0), g).accept);
}

TEST_CASE("teleport: our own game refuses before anything is sent") {
    Trip trip;
    Local loading;
    auto a = trip.Start(2, "Saria", loading, 0);
    CHECK_FALSE(a.ask);
    CHECK(Has(a.lines, "wait until your game is loaded"));
    Local talk = At("Kokiri", 0, 0, 0);
    talk.event = true;
    a = trip.Start(2, "Saria", talk, 0);
    CHECK_FALSE(a.ask);
    CHECK(Has(a.lines, "not during a cutscene"));
    Local fight = At("InsideTheDekuTree", 0, 0, 0);
    fight.fighting = true;
    a = trip.Start(2, "Saria", fight, 0);
    CHECK_FALSE(a.ask);
    CHECK(Has(a.lines, "boss fight"));
    CHECK(trip.phase() == Trip::Phase::idle);
}

TEST_CASE("teleport: the same scene moves Link at once") {
    Trip trip;
    const Local here = At("Kokiri", 0, 0, 0);
    auto a = trip.Start(2, "Saria", here, 0);
    REQUIRE(a.ask);
    CHECK(*a.ask == 2);
    CHECK(trip.phase() == Trip::Phase::asking);
    // An answer from another player is dropped.
    a = trip.Accepted(3, Target("Kokiri", 9, 9, 9), here, kS);
    CHECK_FALSE(a.move);
    a = trip.Accepted(2, Target("Kokiri", 5, 1, 6), here, kS);
    REQUIRE(a.move);
    CHECK(a.move->x == 5.0f);
    CHECK(a.move->z == 6.0f);
    CHECK_FALSE(a.scene);
    CHECK(Has(a.lines, "next to Saria"));
    CHECK(trip.phase() == Trip::Phase::idle);
    // A late second answer does nothing.
    CHECK_FALSE(trip.Accepted(2, Target("Kokiri", 5, 1, 6), here, kS).move);
}

TEST_CASE("teleport: another scene changes scene, then asks again and moves") {
    Trip trip;
    const Local here = At("Kokiri", 0, 0, 0);
    trip.Start(2, "Saria", here, 0);
    auto a = trip.Accepted(2, Target("KakarikoVillage_Child", 10, 2, 20), here, kS);
    REQUIRE(a.scene);
    CHECK(*a.scene == "KakarikoVillage_Child");
    CHECK_FALSE(a.move);
    CHECK(Has(a.lines, "going to Saria in KakarikoVillage_Child"));
    CHECK_FALSE(Has(a.lines, "talk to someone"));  // free play takes it: no event needed
    CHECK(trip.phase() == Trip::Phase::travelling);
    // Asked again every 0.5 s until the game takes it.
    CHECK_FALSE(trip.Tick(here, kS + kS / 4).scene);
    CHECK(trip.Tick(here, kS + kS / 2).scene);
    // The load: no scene (taken: the switch is dropped once), then the new
    // scene with its arrival event.
    Local loading;
    a = trip.Tick(loading, 2 * kS);
    CHECK_FALSE(a.scene);
    CHECK(a.release);
    CHECK_FALSE(a.cancel_scene);
    CHECK_FALSE(trip.Tick(loading, 2 * kS + kS / 2).release);
    Local arrived = At("KakarikoVillage_Child", 0, 0, 0);
    arrived.event = true;
    CHECK(trip.Tick(arrived, 3 * kS).cancel_scene);  // the request is cleared on arrival
    CHECK(trip.phase() == Trip::Phase::settling);
    arrived.event = false;
    CHECK_FALSE(trip.Tick(arrived, 4 * kS).ask);  // the settle starts
    CHECK_FALSE(trip.Tick(arrived, 5 * kS).ask);
    a = trip.Tick(arrived, 4 * kS + Trip::kSettleUs);
    REQUIRE(a.ask);
    CHECK(*a.ask == 2);
    CHECK(trip.phase() == Trip::Phase::asking_again);
    // Where Saria stands now.
    a = trip.Accepted(2, Target("KakarikoVillage_Child", 12, 2, 21), arrived, 6 * kS);
    REQUIRE(a.move);
    CHECK(a.move->x == 12.0f);
    CHECK(trip.phase() == Trip::Phase::idle);
}

TEST_CASE("teleport: after the scene change, a busy or silent player still gives the first place") {
    for (int silent = 0; silent < 2; ++silent) {
        Trip trip;
        const Local here = At("Kokiri", 0, 0, 0);
        trip.Start(2, "Saria", here, 0);
        trip.Accepted(2, Target("Hyrule", 10, 2, 20), here, kS);
        const Local arrived = At("Hyrule", 0, 0, 0);
        trip.Tick(arrived, 2 * kS);
        trip.Tick(arrived, 3 * kS);
        REQUIRE(trip.Tick(arrived, 3 * kS + Trip::kSettleUs).ask);
        const auto a = silent ? trip.TimedOut(2) : trip.Refused(2, proto::JoinRefuseReason::busy);
        REQUIRE(a.move);
        CHECK(a.move->x == 10.0f);
        CHECK(trip.phase() == Trip::Phase::idle);
    }
    // They went on to another scene meanwhile: where they were.
    Trip trip;
    const Local here = At("Kokiri", 0, 0, 0);
    trip.Start(2, "Saria", here, 0);
    trip.Accepted(2, Target("Hyrule", 10, 2, 20), here, kS);
    const Local arrived = At("Hyrule", 0, 0, 0);
    trip.Tick(arrived, 2 * kS);
    trip.Tick(arrived, 3 * kS);
    trip.Tick(arrived, 3 * kS + Trip::kSettleUs);
    const auto a = trip.Accepted(2, Target("LonLonRanch", 1, 1, 1), arrived, 6 * kS);
    REQUIRE(a.move);
    CHECK(a.move->x == 10.0f);
    CHECK(Has(a.lines, "has gone on to LonLonRanch"));
}

TEST_CASE("teleport: a scene for the other age is refused") {
    Trip trip;
    Local child = At("Kokiri", 0, 0, 0);
    trip.Start(2, "Saria", child, 0);
    const auto a = trip.Accepted(2, Target("KakarikoVillage_Adult", 1, 1, 1), child, kS);
    CHECK_FALSE(a.scene);
    CHECK_FALSE(a.move);
    CHECK(Has(a.lines, "a place for adult Link; yours is child"));
    // A scene of both ages is fine, and a dungeon warns about items.
    trip.Start(2, "Saria", child, 0);
    const auto d = trip.Accepted(2, Target("InsideTheDekuTree", 1, 1, 1), child, kS);
    CHECK(d.scene);
    CHECK(Has(d.lines, "you may need its items"));
}

TEST_CASE("teleport: refusals and time-outs from the other game") {
    const Local here = At("Kokiri", 0, 0, 0);
    Trip trip;
    trip.Start(2, "Saria", here, 0);
    CHECK(trip.Refused(3, proto::JoinRefuseReason::busy).lines.empty());  // not ours
    auto a = trip.Refused(2, proto::JoinRefuseReason::cannot_warp);
    CHECK(Has(a.lines, "Saria is in a boss fight"));
    CHECK_FALSE(a.move);
    trip.Start(2, "Saria", here, 0);
    CHECK(Has(trip.Refused(2, proto::JoinRefuseReason::not_there).lines, "not in the game"));
    trip.Start(2, "Saria", here, 0);
    CHECK(Has(trip.Refused(2, proto::JoinRefuseReason::busy).lines, "Saria is busy"));
    trip.Start(2, "Saria", here, 0);
    CHECK(Has(trip.Refused(2, static_cast<proto::JoinRefuseReason>(9)).lines, "reason 9"));
    trip.Start(2, "Saria", here, 0);
    CHECK(Has(trip.TimedOut(2).lines, "no answer from Saria"));
    CHECK(trip.phase() == Trip::Phase::idle);
}

TEST_CASE("teleport: a scene change not taken is given up and cleared") {
    const Local here = At("Kokiri", 0, 0, 0);
    Trip trip;
    trip.Start(2, "Saria", here, 0);
    trip.Accepted(2, Target("Hyrule", 1, 1, 1), here, kS);
    CHECK_FALSE(trip.Tick(here, kS + Trip::kGiveUpUs).cancel_scene);
    const auto a = trip.Tick(here, 2 * kS + Trip::kGiveUpUs);
    CHECK(a.cancel_scene);
    CHECK(Has(a.lines, "did not change scene in time"));
    CHECK(trip.phase() == Trip::Phase::idle);
    // The player leaving, or a new /teleport, also clears it.
    trip.Start(2, "Saria", here, 0);
    trip.Accepted(2, Target("Hyrule", 1, 1, 1), here, kS);
    CHECK(trip.Left(3).lines.empty());
    const auto left = trip.Left(2);
    CHECK(left.cancel_scene);
    CHECK(Has(left.lines, "Saria left"));
    trip.Start(2, "Saria", here, 0);
    trip.Accepted(2, Target("Hyrule", 1, 1, 1), here, kS);
    const auto again = trip.Start(3, "Sam", here, 2 * kS);
    CHECK(again.cancel_scene);
    CHECK(again.ask);
    CHECK(Has(trip.Left(0).lines, "connection was lost"));
}

TEST_CASE("teleport: leaving the scene while settling stops") {
    const Local here = At("Kokiri", 0, 0, 0);
    Trip trip;
    trip.Start(2, "Saria", here, 0);
    trip.Accepted(2, Target("Hyrule", 1, 1, 1), here, kS);
    trip.Tick(At("Hyrule", 0, 0, 0), 2 * kS);
    CHECK(trip.phase() == Trip::Phase::settling);
    const auto a = trip.Tick(At("LonLonRanch", 0, 0, 0), 3 * kS);
    CHECK(Has(a.lines, "you left Hyrule"));
    CHECK(trip.phase() == Trip::Phase::idle);
}

TEST_CASE("teleport: a taken scene change is not asked again while it loads") {
    Trip trip;
    Local here = At("Kokiri", 0, 0, 0);
    here.reserved_banc = "";
    trip.Start(2, "Saria", here, 0);
    REQUIRE(trip.Accepted(2, Target("Hyrule", 1, 1, 1), here, kS).scene);
    // Our request is in the game's flag: not taken yet, asked again.
    here.reserved_banc = "Hyrule";
    auto a = trip.Tick(here, kS + kS / 2);
    CHECK(a.scene);
    CHECK_FALSE(a.release);
    // The game started loading (Sequence_IsLoadingBanc), the old scene still
    // read: taken, the switch dropped, no new ask.
    here.loading_banc = true;
    a = trip.Tick(here, 2 * kS);
    CHECK(a.release);
    CHECK_FALSE(a.scene);
    CHECK_FALSE(trip.Tick(here, 9 * kS).scene);  // still loading: never asked again
    // Arrived: the request and the switch are cleared, even while the load
    // flag is still up; the settle waits for it.
    Local arrived = At("Hyrule", 0, 0, 0);
    arrived.loading_banc = true;
    a = trip.Tick(arrived, 10 * kS);
    CHECK(a.cancel_scene);
    CHECK(trip.phase() == Trip::Phase::settling);
    CHECK_FALSE(trip.Tick(arrived, 10 * kS + 2 * Trip::kSettleUs).ask);
    arrived.loading_banc = false;
    CHECK_FALSE(trip.Tick(arrived, 13 * kS).ask);
    CHECK(trip.Tick(arrived, 13 * kS + Trip::kSettleUs).ask);
}

TEST_CASE("teleport: the game emptying the request counts as taken; a load elsewhere asks again") {
    Trip trip;
    Local here = At("Kokiri", 0, 0, 0);
    trip.Start(2, "Saria", here, 0);
    REQUIRE(trip.Accepted(2, Target("Hyrule", 1, 1, 1), here, kS).scene);
    here.reserved_banc = "";  // ChangeBancOnMain took it and emptied it
    auto a = trip.Tick(here, kS + kS / 10);
    CHECK(a.release);
    CHECK_FALSE(trip.Tick(here, kS + kS / 2).scene);
    CHECK_FALSE(trip.Tick(here, kS + Trip::kRearmUs - 1).scene);
    // Still not there after kRearmUs with no load running: asked again.
    a = trip.Tick(here, kS + kS / 10 + Trip::kRearmUs);
    CHECK(a.scene);
    CHECK(*a.scene == "Hyrule");
    CHECK(trip.phase() == Trip::Phase::travelling);
}
