// Tests for the sync digest lines (lichii/hook/coop_digest.h).
#include <doctest/doctest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "coop_digest.h"

namespace proto = coop::protocol;
using CoopDigest::Entry;

namespace {

constexpr std::uint64_t kS = 1'000'000;

Entry Obj(std::uint64_t hash, int life, int owner = 1) {
    return Entry{proto::ObjectId{proto::ObjectKind::placed, 0, hash},
                 {{"name", "Enemy_Stalchild"}, {"owner", std::to_string(owner)}, {"life", std::to_string(life)}}};
}

std::size_t Count(const std::vector<std::string>& lines, const std::string& part) {
    return static_cast<std::size_t>(
        std::count_if(lines.begin(), lines.end(), [&](const std::string& l) { return l.find(part) != std::string::npos; }));
}

} // namespace

TEST_CASE("digest: ids, clean values, the hash leaves local fields out") {
    CHECK(CoopDigest::IdText({proto::ObjectKind::placed, 0, 0xabc}) == "pabc");
    CHECK(CoopDigest::IdText({proto::ObjectKind::made, 2, 0x1f}) == "m2.1f");
    CHECK(CoopDigest::Clean("a b=c") == "a_b_c");
    CHECK(CoopDigest::Clean("") == "-");
    Entry a = Obj(0x11, 4);
    Entry b = a;
    b.fields.emplace_back("local.owner_life", "3");
    CHECK(CoopDigest::Hash(a) == CoopDigest::Hash(b));
    CHECK(CoopDigest::Hash(a) != CoopDigest::Hash(Obj(0x11, 3)));
}

TEST_CASE("digest: lines every 2 s of server time: changed objects, gone ones and the room") {
    CoopDigest::Log log;
    auto lines = log.Take(10 * kS, "Dungeon001", 1, {Obj(0x11, 4), Obj(0x22, 2)});
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].starts_with("coop: digest: t=10000 scene=Dungeon001 self=1 obj=p11 name=Enemy_Stalchild owner=1 life=4 h="));
    CHECK(lines[2].find(" room objects=2 h=") != std::string::npos);
    // Between ticks: nothing.
    CHECK(log.Take(11 * kS, "Dungeon001", 1, {Obj(0x11, 0)}).empty());
    // Next tick: only the changed one, the gone one and the room.
    lines = log.Take(12 * kS, "Dungeon001", 1, {Obj(0x11, 3)});
    CHECK(lines.size() == 3);
    CHECK(Count(lines, "obj=p11 ") == 1);
    CHECK(Count(lines, "obj=p22 gone") == 1);
    CHECK(Count(lines, "life=3") == 1);
    // Unchanged: the room line only, until kRefreshUs after its last line.
    lines = log.Take(14 * kS, "Dungeon001", 1, {Obj(0x11, 3)});
    CHECK(lines.size() == 1);
    CHECK(Count(lines, " room ") == 1);
    lines = log.Take(22 * kS, "Dungeon001", 1, {Obj(0x11, 3)});
    CHECK(Count(lines, "obj=p11 ") == 1);
}

TEST_CASE("digest: a local field's change is written but does not move the hash") {
    CoopDigest::Log log;
    Entry e = Obj(0x11, 4, 2);
    e.fields.emplace_back("local.owner_life", "4");
    (void)log.Take(2 * kS, "S", 1, {e});
    e.fields.back().second = "3";
    const auto lines = log.Take(4 * kS, "S", 1, {e});
    REQUIRE(Count(lines, "obj=p11 ") == 1);
    CHECK(Count(lines, "local.owner_life=3") == 1);
}

TEST_CASE("digest: the room hash is the same for the same objects in any order, and a new scene starts afresh") {
    CoopDigest::Log a;
    CoopDigest::Log b;
    const auto la = a.Take(2 * kS, "S", 1, {Obj(0x11, 4), Obj(0x22, 2)});
    const auto lb = b.Take(2 * kS, "S", 2, {Obj(0x22, 2), Obj(0x11, 4)});
    const auto room = [](const std::vector<std::string>& l) { return l.back().substr(l.back().find(" room ")); };
    CHECK(room(la) == room(lb));
    // A new scene: no gone lines for the old one's objects, the next scene's are all written.
    const auto next = a.Take(4 * kS, "T", 1, {Obj(0x11, 4)});
    CHECK(Count(next, " gone") == 0);
    CHECK(Count(next, "scene=T self=1 obj=p11 ") == 1);
}

TEST_CASE("digest: the beat keeps its rate and starts over after a stall, Reset starts afresh") {
    CoopDigest::Log log;
    int ticks = 0;
    int due = 0;
    for (std::uint64_t t = 1 * kS; t < 21 * kS; t += 33'333) {
        const bool d = log.Due(t);
        const bool wrote = Count(log.Take(t, "S", 1, {}), " room ") > 0;
        CHECK(d == wrote);  // Due says exactly when Take writes: the caller builds entries only then
        due += d ? 1 : 0;
        ticks += wrote ? 1 : 0;
    }
    CHECK(ticks == 10);
    CHECK(due == 10);
    CHECK_FALSE(log.Due(0));  // no server time yet
    CHECK(Count(log.Take(100 * kS, "S", 1, {}), " room ") == 1);  // a pause: once, then the new beat
    CHECK(log.Take(101 * kS, "S", 1, {}).empty());
    (void)log.Take(102 * kS, "S", 1, {Obj(0x11, 4)});
    log.Reset();
    CHECK(Count(log.Take(102 * kS + 1, "S", 1, {Obj(0x11, 4)}), "obj=p11 ") == 1);
    CHECK(log.Take(0, "S", 1, {}).empty());  // no server time yet
}
