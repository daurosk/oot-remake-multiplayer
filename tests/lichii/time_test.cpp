// Tests for the shared time of day (lichii/hook/coop_time_sync.cpp).
#include <doctest/doctest.h>

#include <cstdlib>
#include <variant>
#include <vector>

#include "coop_time_sync.h"

namespace proto = coop::protocol;
using CoopTime::Sync;

namespace {

constexpr std::uint64_t kMs = 1000;

std::vector<proto::TimeOfDay> Sent(const std::vector<proto::Message>& send) {
    std::vector<proto::TimeOfDay> out;
    for (const auto& m : send) {
        out.push_back(std::get<proto::TimeOfDay>(m));
    }
    return out;
}

proto::TimeOfDay From(std::uint8_t player, std::uint32_t seconds, bool jump = false) {
    return {player, seconds, jump ? proto::time_of_day::jump : std::uint8_t{0}};
}

} // namespace

TEST_CASE("time: Ahead goes the short way round midnight") {
    CHECK(CoopTime::Ahead(100, 160) == 60);
    CHECK(CoopTime::Ahead(160, 100) == -60);
    CHECK(CoopTime::Ahead(86'390, 10) == 20);
    CHECK(CoopTime::Ahead(10, 86'390) == -20);
}

TEST_CASE("time: the host says its clock once a second, others do not") {
    Sync host;
    std::vector<proto::Message> send;
    CHECK_FALSE(host.Tick(36'000, true, 0, send));
    auto sent = Sent(send);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].seconds == 36'000);
    CHECK(sent[0].flags == 0);
    send.clear();
    host.Tick(36'010, true, 500 * kMs, send);
    CHECK(send.empty());
    host.Tick(36'020, true, 1000 * kMs, send);
    CHECK(Sent(send).size() == 1);

    Sync guest;
    send.clear();
    guest.Tick(36'000, false, 0, send);
    guest.Tick(36'020, false, 1000 * kMs, send);
    CHECK(send.empty());
}

TEST_CASE("time: a guest takes the host's clock on joining when two minutes off") {
    Sync guest;
    std::vector<proto::Message> send;
    Sync near;
    near.Tick(36'000, false, 0, send);
    near.Receive(From(1, 36'100));  // under two minutes: kept
    CHECK_FALSE(near.Tick(36'005, false, 100 * kMs, send));
    near.Receive(From(1, 80'000));  // after the first word: no join set
    CHECK_FALSE(near.Tick(36'010, false, 200 * kMs, send));
    guest.Tick(36'000, false, 0, send);
    guest.Receive(From(1, 80'000));  // far: set
    const auto set = guest.Tick(36'010, false, 200 * kMs, send);
    REQUIRE(set);
    CHECK(*set == 80'000);
    // Our own set is no jump of ours.
    send.clear();
    guest.Tick(80'001, false, 300 * kMs, send);
    CHECK(send.empty());
    // Round midnight counts the short way.
    Sync late;
    late.Tick(86'380, false, 0, send);
    late.Receive(From(1, 30));
    CHECK_FALSE(late.Tick(86'381, false, 100 * kMs, send));
}

TEST_CASE("time: while playing, a guest sets its clock only when an hour off, running, once a minute") {
    Sync guest;
    std::vector<proto::Message> send;
    guest.Tick(36'000, false, 0, send);
    guest.Receive(From(1, 40'000));
    REQUIRE(guest.Tick(36'000, false, 250 * kMs, send) == 40'000u);
    // Our clock runs 100 a second from the set.
    std::uint32_t ours = 40'000;
    std::uint64_t t = 250 * kMs;
    const auto run = [&](std::uint64_t until_us, std::uint32_t host_ahead, bool clock_runs) {
        std::optional<std::uint32_t> got;
        while (t < until_us) {
            t += 250 * kMs;
            if (clock_runs) ours += 25;
            guest.Receive(From(1, (ours + host_ahead) % proto::kSecondsPerDay));
            if (const auto set = guest.Tick(ours, false, t, send)) {
                got = set;
                ours = *set;
            }
        }
        return got;
    };
    // 20 in-game minutes off (a time change stopped our clock): kept.
    CHECK_FALSE(run(90'000 * kMs, 1'200, true));
    // Two hours off while our clock stands (indoors): kept.
    CHECK_FALSE(run(100'000 * kMs, 7'200, false));
    // Two hours off and our clock runs again: set once it ran 2 s.
    const auto set = run(103'000 * kMs, 7'200, true);
    REQUIRE(set);
    // Still off right after (the host moved on): not again within a minute.
    CHECK_FALSE(run(150'000 * kMs, 7'200, true));
    CHECK(run(170'000 * kMs, 7'200, true));
}

TEST_CASE("time: a set that stops the clock does not set it again and again (issue 159)") {
    // Live (stalchild run, 2026-11-09): each set runs the game's time change,
    // which stops the guest's clock for about 8 s while the host's runs on
    // (about 100 in-game seconds a real second). With the old two-minute
    // limit the guest set its clock every 2-3 s, and every set made the
    // area's grass and objects again.
    Sync guest;
    std::vector<proto::Message> send;
    std::uint32_t host = 36'000;
    std::uint32_t ours = 30'000;
    std::uint64_t frozen_until = 0;
    int sets = 0;
    for (std::uint64_t t = 0; t <= 600'000 * kMs; t += 250 * kMs) {
        host = (host + 25) % proto::kSecondsPerDay;
        if (t >= frozen_until) ours = (ours + 25) % proto::kSecondsPerDay;
        guest.Receive(From(1, host));
        if (const auto set = guest.Tick(ours, false, t, send)) {
            ++sets;
            ours = *set;
            frozen_until = t + 8'000 * kMs;
        }
    }
    CHECK(sets == 1);  // joining only, in ten minutes
}

TEST_CASE("time: host clock stands, guest runs: no set") {
    // The host is indoors or in a dungeon (its clock stands) while the guest
    // runs outdoors: the drift grows past an hour, but the guest keeps its own.
    Sync guest;
    std::vector<proto::Message> send;
    guest.Tick(36'000, false, 0, send);
    guest.Receive(From(1, 36'000));
    CHECK_FALSE(guest.Tick(36'000, false, 250 * kMs, send));
    std::uint32_t ours = 36'000;
    int sets = 0;
    for (std::uint64_t t = 500 * kMs; t <= 600'000 * kMs; t += 250 * kMs) {
        ours = (ours + 25) % proto::kSecondsPerDay;
        guest.Receive(From(1, 36'000));  // the host's clock stands
        if (guest.Tick(ours, false, t, send)) ++sets;
    }
    CHECK(sets == 0);
}

TEST_CASE("time: a clock that jumps by itself is sent as a jump, and everyone takes a jump") {
    Sync a;
    std::vector<proto::Message> send;
    a.Tick(36'000, false, 0, send);
    a.Tick(36'010, false, 250 * kMs, send);
    CHECK(send.empty());
    a.Tick(64'800, false, 500 * kMs, send);  // the Sun's Song: to 18:00
    auto sent = Sent(send);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].seconds == 64'800);
    CHECK((sent[0].flags & proto::time_of_day::jump) != 0);

    // The host takes a jump too.
    Sync host;
    send.clear();
    host.Tick(36'000, true, 0, send);
    host.Receive(From(2, 64'800, true));
    const auto set = host.Tick(36'005, true, 300 * kMs, send);
    REQUIRE(set);
    CHECK(*set == 64'800);
    // A jump to where we already are changes nothing.
    host.Receive(From(2, 64'850, true));
    CHECK_FALSE(host.Tick(64'801, true, 3500 * kMs, send));
}

TEST_CASE("time: no clock (a load) sends nothing, and the next read is no jump") {
    Sync a;
    std::vector<proto::Message> send;
    a.Tick(36'000, false, 0, send);
    CHECK_FALSE(a.Tick(std::nullopt, true, 100 * kMs, send));
    CHECK(send.empty());
    a.Tick(70'000, false, 200 * kMs, send);
    CHECK(send.empty());
}

// Issue 192: drift under an hour is nudged (the seconds word written), not set.
namespace {

// Guest and host both running at 30 in-game seconds per quarter second; the
// guest `behind` seconds behind. Ticks every 250 ms, the host's word every
// second. Returns the guest's sync after `seconds` real seconds; `nudges`
// counts the nudges taken; `hold` = whether the game keeps our write.
struct Run {
    Sync guest;
    std::uint32_t host_clock = 40'000;
    std::uint32_t clock = 0;
    std::uint64_t at = 0;
    int nudges = 0;
    int sets = 0;
    std::vector<proto::Message> send;

    explicit Run(int behind) : clock(static_cast<std::uint32_t>(40'000 - behind)) {
        guest.Tick(clock, false, at, send);
        guest.Receive(From(1, clock));  // the join word: ours, so no join set
        guest.Tick(clock, false, at, send);
    }
    void For(int seconds, bool hold = true, std::uint32_t guest_step = 30) {
        for (int i = 0; i < seconds * 4; ++i) {
            at += 250 * kMs;
            host_clock += 30;
            clock += guest_step;
            if (i % 4 == 3) guest.Receive(From(1, host_clock));
            if (guest.Tick(clock, false, at, send)) ++sets;
            if (const auto n = guest.TakeNudge()) {
                ++nudges;
                if (hold) clock = *n;
            }
        }
    }
};

} // namespace

TEST_CASE("time: a guest a few minutes behind is nudged, never set (issue 192)") {
    Run r(5 * 60);
    r.For(1);  // the guest's own clock must run 2 s first
    CHECK(r.nudges == 0);
    r.For(5);
    CHECK(r.sets == 0);
    CHECK(r.nudges == 1);  // 300 s in one step
    CHECK(std::abs(CoopTime::Ahead(r.clock, r.host_clock)) <= 60);
    r.For(30);
    CHECK(r.nudges == 1);  // in sync: nothing more
    CHECK(r.sets == 0);
}

TEST_CASE("time: forty minutes off closes in ten-minute nudges, ahead too") {
    Run behind(40 * 60);
    behind.For(12);
    CHECK(behind.sets == 0);
    CHECK(behind.nudges == 4);
    CHECK(std::abs(CoopTime::Ahead(behind.clock, behind.host_clock)) <= 60);

    Run ahead(-25 * 60);
    ahead.For(20);
    CHECK(ahead.sets == 0);
    CHECK(ahead.nudges == 3);
    CHECK(std::abs(CoopTime::Ahead(ahead.clock, ahead.host_clock)) <= 60);
}

TEST_CASE("time: a slow guest is kept close by a nudge now and then") {
    Run r(0);
    r.For(120, true, 27);  // 10% slow: 3 s behind per tick
    CHECK(r.sets == 0);
    CHECK(r.nudges >= 10);
    CHECK(std::abs(CoopTime::Ahead(r.clock, r.host_clock)) <= 72);
    CHECK_FALSE(r.guest.NudgesOff());
}

TEST_CASE("time: nudges that do not hold stop, and the hourly set is left") {
    Run r(20 * 60);
    r.For(60, false);
    CHECK(r.guest.NudgesOff());
    CHECK(r.nudges == Sync::kNudgeTries);
    CHECK(r.sets == 0);  // still under the hour
    // Reset (a new session) turns them on again.
    r.guest.Reset();
    CHECK_FALSE(r.guest.NudgesOff());
}

TEST_CASE("time: no nudge during an event, it runs once the event ends") {
    Run r(10 * 60);
    r.guest.SetEvent(true);  // a cutscene or a talk
    r.For(10);
    CHECK(r.nudges == 0);
    CHECK(r.sets == 0);
    r.guest.SetEvent(false);
    r.For(3);
    CHECK(r.nudges == 1);
    CHECK(std::abs(CoopTime::Ahead(r.clock, r.host_clock)) <= 60);
}

TEST_CASE("time: no nudge across midnight, it runs once both clocks are past it") {
    Sync guest;
    std::vector<proto::Message> send;
    std::uint64_t at = 0;
    std::uint32_t clock = 86'400 - 300;      // 23:55
    std::uint32_t host_clock = 300;          // 00:05, 10 min ahead
    guest.Tick(clock, false, at, send);
    guest.Receive(From(1, clock));
    guest.Tick(clock, false, at, send);
    int nudges = 0;
    int crossing = 0;
    for (int i = 0; i < 60 * 4; ++i) {
        at += 250 * kMs;
        host_clock = (host_clock + 30) % 86'400;
        clock = (clock + 30) % 86'400;
        if (i % 4 == 3) guest.Receive(From(1, host_clock));
        guest.Tick(clock, false, at, send);
        if (const auto n = guest.TakeNudge()) {
            ++nudges;
            if ((clock > 43'200) != (*n > 43'200)) ++crossing;
            clock = *n;
        }
    }
    CHECK(crossing == 0);
    CHECK(nudges >= 1);
    CHECK(std::abs(CoopTime::Ahead(clock, host_clock)) <= 60);
}

TEST_CASE("time: no nudge while either clock stands, nor for the host") {
    Run r(10 * 60);
    r.For(3, true, 0);  // our clock stands (indoors)
    CHECK(r.nudges == 0);

    Sync host;
    std::vector<proto::Message> send;
    host.Tick(40'000, true, 0, send);
    host.Receive(From(2, 41'000));
    host.Tick(40'030, true, 3000 * kMs, send);
    CHECK_FALSE(host.TakeNudge());
}

TEST_CASE("time: a guest's save load is not sent as a jump, it takes the host's clock again (issue 159)") {
    Sync guest;
    std::vector<proto::Message> send;
    // Joined and in sync with the host at 10:00.
    guest.Receive(From(1, 36'000));
    CHECK_FALSE(guest.Tick(36'000, false, 0, send));
    guest.Tick(36'025, false, 250 * kMs, send);
    // The guest loads its save (11:59): the hook reports the load, the
    // clock reads the save's time just after.
    guest.SaveLoaded(10'000 * kMs);
    guest.Tick(43'166, false, 10'100 * kMs, send);
    guest.Tick(43'190, false, 10'350 * kMs, send);
    CHECK(send.empty());  // the host is not moved to the guest's save
    // A host word in the first second is not taken yet (the load still settles).
    guest.Receive(From(1, 37'000));
    CHECK_FALSE(guest.Tick(43'215, false, 10'600 * kMs, send));
    // The next one is: the guest goes back to the group's clock, once.
    guest.Receive(From(1, 37'100));
    const auto set = guest.Tick(43'240, false, 11'100 * kMs, send);
    REQUIRE(set);
    CHECK(*set == 37'100);
    // The set's time change stands the clock for seconds: no second set.
    std::uint32_t host = 37'100;
    int sets = 0;
    for (std::uint64_t t = 11'350; t < 20'000; t += 250) {
        if (t % 1000 == 100) {
            host += 100;
            guest.Receive(From(1, host));
        }
        if (guest.Tick(37'100, false, t * kMs, send)) ++sets;
    }
    CHECK(sets == 0);
    CHECK(send.empty());
}

TEST_CASE("time: the host's save load is sent as a jump (the group follows the host's save)") {
    Sync host;
    std::vector<proto::Message> send;
    host.Tick(36'000, true, 0, send);
    send.clear();
    host.SaveLoaded(5'000 * kMs);
    host.Tick(43'166, true, 5'100 * kMs, send);
    bool jump = false;
    for (const auto& t : Sent(send)) {
        if ((t.flags & proto::time_of_day::jump) != 0 && t.seconds == 43'166) jump = true;
    }
    CHECK(jump);
}

TEST_CASE("time: guests loading saves again and again never move the host's clock") {
    Sync host;
    Sync guest;
    std::vector<proto::Message> to_guest;
    std::vector<proto::Message> to_host;
    std::uint32_t host_clock = 36'000;
    std::uint32_t guest_clock = 36'000;
    int host_sets = 0;
    int guest_sets = 0;
    for (std::uint64_t t = 0; t < 300'000; t += 250) {  // 5 min, ticks every 250 ms
        host_clock = (host_clock + 25) % proto::kSecondsPerDay;
        guest_clock = (guest_clock + 25) % proto::kSecondsPerDay;
        if (t % 30'000 == 10'000) {  // the guest loads a save at 06:00 every 30 s
            guest.SaveLoaded(t * kMs);
            guest_clock = 21'600;
        }
        to_guest.clear();
        to_host.clear();
        if (const auto s = host.Tick(host_clock, true, t * kMs, to_guest)) {
            host_clock = *s;
            ++host_sets;
        }
        if (const auto s = guest.Tick(guest_clock, false, t * kMs, to_host)) {
            guest_clock = *s;
            ++guest_sets;
        }
        for (const auto& m : to_guest) guest.Receive(m);
        for (const auto& m : to_host) host.Receive(m);
    }
    CHECK(host_sets == 0);
    CHECK(guest_sets == 10);  // once after each of its loads
}

TEST_CASE("time: a guest's reload without a save load seen (Link gone) sends no jump either") {
    Sync guest;
    std::vector<proto::Message> send;
    guest.Receive(From(1, 55'000));
    guest.Tick(55'000, false, 0, send);
    guest.Tick(55'025, false, 250 * kMs, send);
    // A death's reload: Link cannot be read for a few frames, then the clock
    // reads the reload's time (four hours back, as in dauro's hand test at 245.8 s).
    for (std::uint64_t t = 500; t <= 1000; t += 16) guest.Loading(t * kMs);
    guest.Tick(40'817, false, 1'600 * kMs, send);
    CHECK(send.empty());
    // Link stays gone for 8 s (no Tick change); then the host's word is taken once.
    for (std::uint64_t t = 1000; t <= 9000; t += 16) guest.Loading(t * kMs);
    guest.Receive(From(1, 56'000));
    CHECK_FALSE(guest.Tick(40'817, false, 9'500 * kMs, send));  // under 1 s after Link came back
    guest.Receive(From(1, 56'100));
    const auto set = guest.Tick(40'817, false, 10'250 * kMs, send);
    REQUIRE(set);
    CHECK(*set == 56'100);
    CHECK(send.empty());

    // A blink of Link with no jump changes nothing, and a real jump later is sent.
    Sync b;
    send.clear();
    b.Tick(36'000, false, 0, send);
    b.Loading(100 * kMs);
    b.Tick(36'025, false, 250 * kMs, send);
    b.Tick(64'800, false, 6'000 * kMs, send);  // the Sun's Song, 6 s later
    REQUIRE(send.size() == 1);
    CHECK((std::get<proto::TimeOfDay>(send[0]).flags & proto::time_of_day::jump) != 0);
}
