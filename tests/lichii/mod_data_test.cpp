// SPDX-License-Identifier: GPL-3.0-or-later
// The mods' data channel (lichii/hook/coop_mod_data.*, issue 250): this game's values and the others', what is sent
// when, and the changes the mods hear.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "coop_mod_data.h"

using namespace CoopModData;
namespace proto = coop::protocol;

namespace {
Bytes B(const std::string& s) { return Bytes(s.begin(), s.end()); }
}  // namespace

TEST_CASE("mod data store: own values within the limits, alone too") {
    Store s;
    CHECK(s.Set("my-mod", "score", B("12")) == SetResult::done);
    CHECK(s.Get(0, "my-mod", "score") == B("12"));
    CHECK(s.Set("My Mod", "score", B("1")) == SetResult::bad_id);
    CHECK(s.Set("my-mod", "Score", B("1")) == SetResult::bad_key);
    CHECK(s.Set("my-mod", "k", Bytes(proto::kMaxModDataBytes + 1, 1)) == SetResult::too_long);
    CHECK(s.Set("my-mod", "k", Bytes(proto::kMaxModDataBytes, 1)) == SetResult::done);
    for (std::size_t k = 2; k < proto::kMaxModDataKeysPerMod; ++k) {
        CHECK(s.Set("my-mod", "k" + std::to_string(k), B("x")) == SetResult::done);
    }
    CHECK(s.Set("my-mod", "one_more", B("x")) == SetResult::too_many_keys);
    CHECK(s.Set("my-mod", "score", B("13")) == SetResult::done);  // a kept key still changes
    CHECK(s.Set("my-mod", "score", {}) == SetResult::done);       // a clear makes room
    CHECK_FALSE(s.Get(0, "my-mod", "score"));
    CHECK(s.Set("my-mod", "one_more", B("x")) == SetResult::done);
    for (std::size_t m = 1; m < proto::kMaxModDataMods; ++m) {
        CHECK(s.Set("mod" + std::to_string(m), "k", B("x")) == SetResult::done);
    }
    CHECK(s.Set("mod99", "k", B("x")) == SetResult::too_many_mods);
    CHECK(s.Set("mod99", "k", {}) == SetResult::done);  // clearing what is not there is fine
    // Alone nothing is sent.
    CHECK_FALSE(s.Next(0));
}

TEST_CASE("mod data store: sends each change once, a clear first, one each quarter second, all again on a new connection") {
    Store s;
    CHECK(s.Set("a", "x", B("1")) == SetResult::done);
    s.Welcomed(2);
    std::uint64_t now = 10'000;
    auto next = s.Next(now);
    REQUIRE(next);
    CHECK(*next == proto::ModData{proto::kPlayerSelf, "a", "x", B("1")});
    s.MarkSent(*next, now, s.connection());
    CHECK_FALSE(s.Next(now + 100));  // nothing due
    s.Set("a", "x", B("2"));
    s.Set("a", "y", B("3"));
    CHECK_FALSE(s.Next(now + proto::kModDataSendGapMs - 1));  // the gap
    now += proto::kModDataSendGapMs;
    next = s.Next(now);
    REQUIRE(next);
    // The turn goes on after the last key sent (x): y first, then x again.
    CHECK(*next == proto::ModData{proto::kPlayerSelf, "a", "y", B("3")});
    // Not sent (the client refused it): still due.
    CHECK(s.Next(now) == next);
    s.MarkSent(*next, now, s.connection());
    now += proto::kModDataSendGapMs;
    next = s.Next(now);
    REQUIRE(next);
    CHECK(*next == proto::ModData{proto::kPlayerSelf, "a", "x", B("2")});
    s.MarkSent(*next, now, s.connection());
    // A clear goes before a key the server has not got yet.
    s.Set("a", "x", {});
    s.Set("a", "y", B("3"));  // the same value: the server has it, not sent again
    s.Set("a", "z", B("4"));
    now += proto::kModDataSendGapMs;
    next = s.Next(now);
    REQUIRE(next);
    CHECK(*next == proto::ModData{proto::kPlayerSelf, "a", "x", {}});
    s.MarkSent(*next, now, s.connection());
    now += proto::kModDataSendGapMs;
    next = s.Next(now);
    REQUIRE(next);
    CHECK(*next == proto::ModData{proto::kPlayerSelf, "a", "z", B("4")});
    s.MarkSent(*next, now, s.connection());
    now += proto::kModDataSendGapMs;
    CHECK_FALSE(s.Next(now));
    // A new connection: the server knows none of ours.
    s.Left();
    CHECK_FALSE(s.Next(now));
    CHECK(s.Get(0, "a", "y") == B("3"));  // kept for the next session
    s.Welcomed(1);
    next = s.Next(now);
    REQUIRE(next);
    CHECK(*next == proto::ModData{proto::kPlayerSelf, "a", "y", B("3")});
    s.MarkSent(*next, now, s.connection());
    now += proto::kModDataSendGapMs;
    next = s.Next(now);
    REQUIRE(next);
    CHECK(*next == proto::ModData{proto::kPlayerSelf, "a", "z", B("4")});
    s.MarkSent(*next, now, s.connection());
    now += proto::kModDataSendGapMs;
    CHECK_FALSE(s.Next(now));
}

TEST_CASE("mod data store: a key that changes all the time keeps no other back") {
    Store s;
    s.Welcomed(2);
    s.Set("z", "late", B("1"));
    std::uint64_t now = 10'000;
    bool late = false;
    for (int i = 0; i < 4; ++i) {
        s.Set("a", "score", B(std::to_string(i)));
        const auto next = s.Next(now);
        REQUIRE(next);
        late |= next->mod_id == "z";
        s.MarkSent(*next, now, s.connection());
        now += proto::kModDataSendGapMs;
    }
    CHECK(late);
}

TEST_CASE("mod data store: the others' values, their changes, and forgetting them") {
    Store s;
    s.Welcomed(1);
    s.Received(proto::ModData{2, "coop", "skin", B("malon")});
    s.Received(proto::ModData{2, "coop", "skin", B("malon")});  // the same: no change
    s.Received(proto::ModData{3, "hello-mod", "wave", B("1")});
    s.Received(proto::ModData{1, "hello-mod", "wave", B("9")});  // our own number: never ours from the wire
    s.Received(proto::ModData{3, "hello-mod", "gone", {}});      // a clear of nothing
    CHECK(s.Get(2, "coop", "skin") == B("malon"));
    CHECK_FALSE(s.Get(1, "hello-mod", "wave"));
    CHECK(s.TakeChanges() == std::vector<Change>{{2, "coop", "skin", B("malon")}, {3, "hello-mod", "wave", B("1")}});
    CHECK(s.TakeChanges().empty());
    s.Received(proto::ModData{3, "hello-mod", "wave", {}});
    CHECK_FALSE(s.Get(3, "hello-mod", "wave"));
    s.Received(proto::ModData{3, "hello-mod", "wave", B("2")});
    s.PlayerLeft(3);
    CHECK_FALSE(s.Get(3, "hello-mod", "wave"));
    CHECK(s.TakeChanges() == std::vector<Change>{{3, "hello-mod", "wave", {}},
                                                 {3, "hello-mod", "wave", B("2")},
                                                 {3, "hello-mod", "wave", {}}});
    // Leaving the session forgets the others (a clear each), so a mod never keeps a stale value.
    s.Left();
    CHECK_FALSE(s.Get(2, "coop", "skin"));
    CHECK(s.TakeChanges() == std::vector<Change>{{2, "coop", "skin", {}}});
    CHECK(s.number() == 0);
}

TEST_CASE("mod data store: changes nobody takes are capped, the oldest dropped") {
    Store s;
    s.Welcomed(1);
    for (std::size_t i = 0; i < kMaxChanges + 3; ++i) {
        s.Received(proto::ModData{2, "m", "k", B(std::to_string(i))});
    }
    const auto changes = s.TakeChanges();
    CHECK(changes.size() == kMaxChanges);
    CHECK(changes.back().value == B(std::to_string(kMaxChanges + 2)));
    CHECK(s.dropped() == 3);
}

TEST_CASE("mod data store: a send of a connection replaced before MarkSent is sent again on the new one") {
    Store s;
    s.Set("a", "x", B("1"));
    s.Welcomed(2);
    const auto next = s.Next(10'000);
    REQUIRE(next);
    const std::uint64_t old_connection = s.connection();
    s.Welcomed(2);  // a reconnect between the send and MarkSent
    s.MarkSent(*next, 10'000, old_connection);
    CHECK(s.Next(10'000) == next);  // still due on the new connection
    s.MarkSent(*next, 10'000, s.connection());
    CHECK_FALSE(s.Next(20'000));
}
