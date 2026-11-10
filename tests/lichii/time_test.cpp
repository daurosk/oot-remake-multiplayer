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

TEST_CASE("time: a guest moves to the host's clock when two minutes off") {
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
    // Not again within two seconds, then again.
    guest.Receive(From(1, 10'000));
    CHECK_FALSE(guest.Tick(80'002, false, 400 * kMs, send));
    guest.Receive(From(1, 10'000));
    CHECK(guest.Tick(80'003, false, 2300 * kMs, send) == 10'000u);
    // Round midnight counts the short way.
    Sync late;
    late.Tick(86'380, false, 0, send);
    late.Receive(From(1, 30));
    CHECK_FALSE(late.Tick(86'381, false, 100 * kMs, send));
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
