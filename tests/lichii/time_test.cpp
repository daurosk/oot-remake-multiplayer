// Tests for the shared time of day (lichii/hook/coop_time_sync.cpp).
#include <doctest/doctest.h>

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
    guest.Tick(36'000, false, 0, send);
    guest.Receive(From(1, 36'100));  // under two minutes: kept
    CHECK_FALSE(guest.Tick(36'005, false, 100 * kMs, send));
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
