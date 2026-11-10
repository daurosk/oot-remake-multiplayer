// Tests for shared drops (lichii/hook/coop_drops.cpp): which roll stays, the
// pickups and the late joiner, the remover's calls at the cave, and the spawn
// director letting go of a copy.
#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_drops.h"
#include "coop_spawn.h"

namespace proto = coop::protocol;
using CoopDrops::Create;
using CoopDrops::Remove;
using CoopDrops::Seen;
using CoopDrops::Sync;
using proto::Vec3;

namespace {

constexpr std::uint64_t kMs = 1000;
const std::string kScene = "Spot_GraveYard";
const std::string kGreen = "Obj_Rupee_Green";
const std::string kHeart = "Obj_Heart";

Seen Lying(std::uint64_t address, float x, std::string name = kGreen) {
    return Seen{address, std::move(name), {x, 0.0f, 0.0f}};
}

proto::ObjectId Id(std::uint8_t maker, std::uint64_t value) {
    return {proto::ObjectKind::made, maker, value};
}

proto::DropSpawn Told(std::uint8_t maker, std::uint64_t value, float x, std::uint64_t time_us,
                      std::string name = kGreen, std::uint8_t flags = 0) {
    proto::DropSpawn s;
    s.player_number = maker;
    s.id = Id(maker, value);
    s.scene = kScene;
    s.actor = std::move(name);
    s.position = {x, 0.0f, 0.0f};
    s.time_us = time_us;
    s.flags = flags;
    return s;
}

template <typename T>
std::vector<T> Of(const std::vector<proto::Message>& send) {
    std::vector<T> out;
    for (const auto& m : send) {
        if (const auto* v = std::get_if<T>(&m)) {
            out.push_back(*v);
        }
    }
    return out;
}

// One game: a sync and what its last walk gave.
struct Game {
    Sync sync;
    std::uint8_t self = 1;
    std::uint64_t now = 1'000'000;
    std::vector<Seen> seen;
    std::optional<Vec3> player = Vec3{100.0f, 0.0f, 0.0f};
    std::vector<Vec3> owned;
    std::map<std::uint64_t, std::uint64_t> made;
    std::vector<proto::Message> send;
    std::vector<Create> creates;
    std::vector<Remove> removes;

    explicit Game(std::uint8_t number) : self(number) {}

    void Tick(std::uint64_t ms = 33) {
        now += ms * kMs;
        send.clear();
        creates.clear();
        removes.clear();
        sync.Tick(kScene, self, now, seen, player, owned, made, send, creates, removes);
    }
    void Gone(std::uint64_t address) {
        std::erase_if(seen, [&](const Seen& s) { return s.address == address; });
    }
};

class FakeGuest : public CoopPuppet::Guest {
public:
    void Map(std::uint64_t address, std::size_t size) {
        for (std::uint64_t p = address & ~0xfffULL; p < address + size; p += 0x1000) {
            pages_[p];
        }
    }
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = pages_.find((address + i) & ~0xfffULL);
            if (it == pages_.end()) return false;
            o[i] = it->second[(address + i) & 0xfff];
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = pages_.find((address + i) & ~0xfffULL);
            if (it == pages_.end()) return false;
            it->second[(address + i) & 0xfff] = p[i];
        }
        return true;
    }
    template <typename T>
    void Set(std::uint64_t address, const T& v) {
        REQUIRE(Write(address, &v, sizeof(T)));
    }

private:
    std::map<std::uint64_t, std::array<std::uint8_t, 0x1000>> pages_;
};

} // namespace

TEST_CASE("drops: which actors are drops") {
    CHECK(CoopDrops::IsDrop("Obj_Rupee_Green"));
    CHECK(CoopDrops::IsDrop("Obj_Heart"));
    CHECK(CoopDrops::IsDrop("Obj_ArrowBundle_Small"));
    CHECK(CoopDrops::IsDrop("Obj_MagicJar_Large"));
    CHECK_FALSE(CoopDrops::IsDrop("Obj_Heart_Piece"));
    CHECK_FALSE(CoopDrops::IsDrop("Obj_RupeeToken"));
    CHECK_FALSE(CoopDrops::IsDrop("Obj_Fairy"));
    CHECK_FALSE(CoopDrops::IsDrop("Enemy_ReDead"));
    CHECK_FALSE(CoopDrops::IsDrop("Item_Bomb"));
}

TEST_CASE("drops: a new scene asks, our game's drop is told once") {
    Game a(1);
    a.seen = {Lying(0x1000, 50.0f)};  // lay there when we came
    a.Tick();
    REQUIRE(Of<proto::DropAsk>(a.send).size() == 1);
    CHECK(Of<proto::DropAsk>(a.send)[0].scene == kScene);
    CHECK(Of<proto::DropSpawn>(a.send).empty());

    a.seen.push_back(Lying(0x2000, 5.0f));
    a.Tick();
    const auto told = Of<proto::DropSpawn>(a.send);
    REQUIRE(told.size() == 1);
    CHECK(told[0].actor == kGreen);
    CHECK(told[0].scene == kScene);
    CHECK(told[0].id.maker == 1);
    CHECK(told[0].position[0] == doctest::Approx(5.0f));
    CHECK(told[0].time_us == a.now);
    CHECK(told[0].flags == 0);
    CHECK(a.sync.IdOf(0x2000) == told[0].id);
    CHECK(a.creates.empty());

    a.Tick();
    CHECK(a.send.empty());
    CHECK(a.sync.Live() == 1);
}

TEST_CASE("drops: a drop beside something we own is told as its owner's") {
    Game a(2);
    a.Tick();
    a.owned = {Vec3{6.0f, 0.0f, 0.0f}};
    a.Tick();
    a.owned.clear();  // the enemy died and left the list
    a.Tick(2000);
    a.seen = {Lying(0x2000, 5.0f)};
    a.Tick();
    const auto told = Of<proto::DropSpawn>(a.send);
    REQUIRE(told.size() == 1);
    CHECK(told[0].flags == proto::drop_spawn::owned);

    // Long after, or far from it, it is nobody's.
    a.Tick(6000);
    a.seen.push_back(Lying(0x3000, 6.0f, kHeart));
    a.Tick();
    REQUIRE(Of<proto::DropSpawn>(a.send).size() == 1);
    CHECK(Of<proto::DropSpawn>(a.send)[0].flags == 0);
}

TEST_CASE("drops: another game's drop is made here, then let go of") {
    Game b(2);
    b.Tick();
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick();
    CHECK(b.creates.empty());  // our own roll may still come
    b.Tick(CoopDrops::Sync::kAdoptWaitUs / kMs);
    REQUIRE(b.creates.size() == 1);
    CHECK(b.creates[0].name == kGreen);
    CHECK(b.creates[0].position[0] == doctest::Approx(5.0f));
    CHECK_FALSE(b.creates[0].release);
    const std::uint64_t key = b.creates[0].key;

    // The director made it; it is on our list too. Never told back.
    b.made[key] = 0x7000;
    b.seen = {Lying(0x7000, 5.0f)};
    b.Tick();
    REQUIRE(b.creates.size() == 1);
    CHECK(b.creates[0].release);
    CHECK(b.sync.IdOf(0x7000) == Id(1, 7));
    CHECK(Of<proto::DropSpawn>(b.send).empty());

    // The director let go: nothing more to ask of it.
    b.made.clear();
    b.Tick();
    CHECK(b.creates.empty());
    CHECK(b.removes.empty());
    CHECK(b.sync.Live() == 1);

    // Told again (another player came in): known, nothing happens.
    b.sync.Receive(Told(1, 7, 5.0f, b.now, kGreen, proto::drop_spawn::again));
    b.Tick(1000);
    CHECK(b.creates.empty());
}

TEST_CASE("drops: a copy seen on the list before the director reports it is not our roll") {
    Game b(2);
    b.Tick();
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick();
    b.Tick(600);
    REQUIRE(b.creates.size() == 1);
    const std::uint64_t key = b.creates[0].key;
    b.seen = {Lying(0x7000, 5.2f)};
    b.Tick();
    CHECK(Of<proto::DropSpawn>(b.send).empty());
    CHECK(b.sync.IdOf(0x7000) == Id(1, 7));
    REQUIRE(b.creates.size() == 1);
    CHECK_FALSE(b.creates[0].release);  // kept until the director says so
    b.made[key] = 0x7000;
    b.Tick();
    REQUIRE(b.creates.size() == 1);
    CHECK(b.creates[0].release);
    CHECK(b.sync.Adopted() == 0);
}

TEST_CASE("drops: both games rolled: the lower number's stays, the same kind is kept in place") {
    Game a(1), b(2);
    a.Tick();
    b.Tick();
    a.seen = {Lying(0x1000, 5.0f)};
    b.seen = {Lying(0x2000, 5.5f)};
    a.Tick();
    b.Tick();
    const auto from_a = Of<proto::DropSpawn>(a.send);
    const auto from_b = Of<proto::DropSpawn>(b.send);
    REQUIRE(from_a.size() == 1);
    REQUIRE(from_b.size() == 1);

    auto to_b = from_a[0];
    to_b.player_number = 1;
    auto to_a = from_b[0];
    to_a.player_number = 2;
    a.sync.Receive(to_a);
    b.sync.Receive(to_b);
    a.Tick();
    b.Tick();
    // A keeps its own and makes nothing for B's.
    CHECK(a.sync.IdOf(0x1000) == from_a[0].id);
    CHECK(a.removes.empty());
    a.Tick(1000);
    CHECK(a.creates.empty());
    CHECK(a.sync.Live() == 1);
    // B's own rupee is now A's drop: nothing deleted, nothing made.
    CHECK(b.sync.IdOf(0x2000) == from_a[0].id);
    CHECK(b.removes.empty());
    b.Tick(1000);
    CHECK(b.creates.empty());
    CHECK(b.sync.Live() == 1);
}

TEST_CASE("drops: both games rolled another kind: the loser's is deleted and the winner's made") {
    Game b(2);
    b.Tick();
    b.seen = {Lying(0x2000, 5.5f, kHeart)};
    b.Tick();
    REQUIRE(Of<proto::DropSpawn>(b.send).size() == 1);
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick();
    REQUIRE(b.removes.size() == 1);
    CHECK(b.removes[0] == Remove{0x2000, kHeart});
    REQUIRE(b.creates.size() == 1);  // at once: our own roll was already there
    CHECK(b.creates[0].name == kGreen);
    // Still on the list until the game deletes it: not a new roll.
    b.Tick();
    CHECK(Of<proto::DropSpawn>(b.send).empty());
    CHECK(b.removes.empty());
    CHECK(b.sync.Live() == 1);
}

TEST_CASE("drops: the owner's roll beats a lower player number") {
    // P1 rolled without owning the thing; P2, its owner, rolled too.
    Game a(1);
    a.Tick();
    a.seen = {Lying(0x1000, 5.0f)};
    a.Tick();
    const auto mine = Of<proto::DropSpawn>(a.send);
    REQUIRE(mine.size() == 1);
    a.sync.Receive(Told(2, 9, 5.5f, a.now, kGreen, proto::drop_spawn::owned));
    a.Tick();
    CHECK(a.sync.IdOf(0x1000) == Id(2, 9));
    CHECK(a.removes.empty());

    // And P2 itself keeps its own against P1's.
    Game b(2);
    b.Tick();
    b.owned = {Vec3{5.0f, 0.0f, 0.0f}};
    b.Tick();
    b.sync.Receive(Told(1, 3, 5.0f, b.now));
    b.Tick();
    b.seen = {Lying(0x2000, 5.5f)};
    b.Tick();
    const auto told = Of<proto::DropSpawn>(b.send);
    REQUIRE(told.size() == 1);
    CHECK(told[0].flags == proto::drop_spawn::owned);
    CHECK(b.sync.IdOf(0x2000) == told[0].id);
    b.Tick(1000);
    CHECK(b.creates.empty());
    CHECK(b.removes.empty());
}

TEST_CASE("drops: our roll that comes after the winner's copy was made is deleted") {
    Game b(2);
    b.Tick();
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick();
    b.Tick(600);
    REQUIRE(b.creates.size() == 1);
    b.made[b.creates[0].key] = 0x7000;
    b.seen = {Lying(0x7000, 5.0f)};
    b.Tick();
    b.made.clear();
    // Now our own game rolls for the same thing.
    b.seen.push_back(Lying(0x2000, 5.5f, kHeart));
    b.Tick();
    CHECK(Of<proto::DropSpawn>(b.send).empty());
    REQUIRE(b.removes.size() == 1);
    CHECK(b.removes[0] == Remove{0x2000, kHeart});
    CHECK(b.sync.Refused() == 1);
}

TEST_CASE("drops: our roll within the wait is kept as the winner's") {
    Game b(2);
    b.Tick();
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick(100);
    b.seen = {Lying(0x2000, 5.5f)};
    b.Tick(100);
    CHECK(Of<proto::DropSpawn>(b.send).empty());
    CHECK(b.sync.IdOf(0x2000) == Id(1, 7));
    CHECK(b.sync.Adopted() == 1);
    b.Tick(1000);
    CHECK(b.creates.empty());
    CHECK(b.removes.empty());
}

TEST_CASE("drops: rolls far apart in place or time are different things") {
    Game b(2);
    b.Tick();
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick();
    b.Tick(600);
    b.seen = {Lying(0x2000, 5.0f + CoopDrops::Sync::kMatchM + 1.0f)};
    b.Tick();
    CHECK(Of<proto::DropSpawn>(b.send).size() == 1);
    b.Tick(CoopDrops::Sync::kMatchUs / kMs);
    b.seen.push_back(Lying(0x3000, 5.5f, kHeart));
    // (the copy of P1's was never made here; its create is still asked)
    b.Tick();
    CHECK(Of<proto::DropSpawn>(b.send).size() == 1);
    CHECK(b.removes.empty());
}

TEST_CASE("drops: a third game ends with the same drop whatever order the rolls came in") {
    const auto low = Told(1, 7, 5.0f, 1'000'000);
    const auto high = Told(2, 9, 5.5f, 1'200'000, kHeart);
    for (int order = 0; order < 2; ++order) {
        Game c(3);
        c.Tick();
        c.sync.Receive(order == 0 ? low : high);
        c.Tick();
        c.Tick(700);
        // The first one's copy is made.
        REQUIRE(c.creates.size() == 1);
        const std::uint64_t key = c.creates[0].key;
        const std::string first = c.creates[0].name;
        c.made[key] = 0x7000;
        c.seen = {Lying(0x7000, 5.0f, first)};
        c.Tick();
        c.sync.Receive(order == 0 ? high : low);
        c.Tick();
        c.Tick();
        c.Tick(700);
        CHECK(c.sync.Live() == 1);
        if (order == 0) {
            // The lower number's copy stays; nothing is made for the other.
            CHECK(c.sync.IdOf(0x7000) == low.id);
            CHECK(c.creates.size() == 1);  // still held by the director: released
            CHECK(c.creates[0].release);
        } else {
            // The heart copy goes (no longer listed: the director deletes it),
            // the rupee is made.
            CHECK_FALSE(c.sync.IdOf(0x7000));
            REQUIRE(c.creates.size() == 1);
            CHECK(c.creates[0].name == kGreen);
            CHECK(c.creates[0].key != key);
            CHECK(c.removes.empty());
        }
    }
}

TEST_CASE("drops: picked up, blinked out, unloaded") {
    Game a(1);
    a.Tick();
    a.seen = {Lying(0x1000, 5.0f), Lying(0x2000, 30.0f), Lying(0x3000, 60.0f)};
    a.Tick();
    const auto told = Of<proto::DropSpawn>(a.send);
    REQUIRE(told.size() == 3);

    // Link walks onto the first: gone from the list.
    a.player = Vec3{5.5f, 0.0f, 0.0f};
    a.Gone(0x1000);
    a.Tick();
    CHECK(Of<proto::DropTake>(a.send).empty());  // one walk without it is not enough
    a.Tick();
    CHECK(Of<proto::DropTake>(a.send).empty());  // held a moment
    a.Tick(CoopDrops::Sync::kTakeHoldUs / kMs);
    auto takes = Of<proto::DropTake>(a.send);
    REQUIRE(takes.size() == 1);
    CHECK(takes[0].id == told[0].id);
    CHECK(takes[0].reason == proto::DropTakeReason::picked_up);
    CHECK(a.removes.empty());

    // The second goes with Link 10 m away: it blinked out.
    a.player = Vec3{20.0f, 0.0f, 0.0f};
    a.Gone(0x2000);
    a.Tick();
    a.Tick();
    a.Tick(CoopDrops::Sync::kTakeHoldUs / kMs);
    takes = Of<proto::DropTake>(a.send);
    REQUIRE(takes.size() == 1);
    CHECK(takes[0].id == told[1].id);
    CHECK(takes[0].reason == proto::DropTakeReason::gone);

    // The third goes with Link far away: unloaded here, nothing told.
    a.Gone(0x3000);
    a.Tick();
    a.Tick();
    CHECK(a.send.empty());
    CHECK(a.sync.Live() == 0);
}

TEST_CASE("drops: many gone at once, or no Link, is an unload, near 0 tells pickups only") {
    Game a(1);
    a.Tick();
    a.seen = {Lying(0x1000, 5.0f), Lying(0x2000, 6.0f), Lying(0x3000, 7.0f)};
    a.Tick();
    a.player = Vec3{5.0f, 0.0f, 0.0f};
    a.seen.clear();
    a.Tick();
    a.Tick();
    CHECK(Of<proto::DropTake>(a.send).empty());
    a.Tick(CoopDrops::Sync::kTakeHoldUs / kMs);
    CHECK(Of<proto::DropTake>(a.send).empty());

    Game b(1);
    b.Tick();
    b.seen = {Lying(0x1000, 5.0f)};
    b.Tick();
    b.player.reset();
    b.seen.clear();
    b.Tick();
    b.Tick();
    CHECK(Of<proto::DropTake>(b.send).empty());

    Game c(1);
    c.sync.SetNear(0.0f);
    c.Tick();
    c.seen = {Lying(0x1000, 5.0f), Lying(0x2000, 30.0f)};
    c.Tick();
    c.player = Vec3{12.0f, 0.0f, 0.0f};
    c.Gone(0x1000);
    c.Tick();
    c.Tick();
    CHECK(Of<proto::DropTake>(c.send).empty());
    c.player = Vec3{30.5f, 0.0f, 0.0f};
    c.Gone(0x2000);
    c.Tick();
    c.Tick();
    c.Tick(CoopDrops::Sync::kTakeHoldUs / kMs);
    CHECK(Of<proto::DropTake>(c.send).size() == 1);
}

TEST_CASE("drops: taken in another game: ours is deleted, and never told as taken by us") {
    Game b(2);
    b.Tick();
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick();
    b.Tick(600);
    REQUIRE(b.creates.size() == 1);
    b.made[b.creates[0].key] = 0x7000;
    b.seen = {Lying(0x7000, 5.0f)};
    b.Tick();
    b.made.clear();
    b.Tick();

    b.player = Vec3{5.0f, 0.0f, 0.0f};  // we stand on it too
    b.sync.Receive(proto::DropTake{1, Id(1, 7), proto::DropTakeReason::picked_up});
    b.Tick();
    REQUIRE(b.removes.size() == 1);
    CHECK(b.removes[0] == Remove{0x7000, kGreen});
    b.seen.clear();
    b.Tick();
    b.Tick();
    b.Tick();
    CHECK(b.send.empty());
    CHECK(b.sync.Live() == 0);

    // Told again late by a third game: stays gone.
    b.sync.Receive(Told(1, 7, 5.0f, b.now, kGreen, proto::drop_spawn::again));
    b.Tick(1000);
    CHECK(b.creates.empty());
}

TEST_CASE("drops: a take that comes before the drop keeps it from being made") {
    Game b(2);
    b.Tick();
    b.sync.Receive(proto::DropTake{1, Id(1, 7), proto::DropTakeReason::picked_up});
    b.Tick();
    b.sync.Receive(Told(3, 7, 5.0f, b.now, kGreen, proto::drop_spawn::again));  // another id: made
    b.sync.Receive(Told(1, 7, 9.0f, b.now, kGreen, proto::drop_spawn::again));
    b.Tick();
    REQUIRE(b.creates.size() == 1);
    CHECK(b.creates[0].position[0] == doctest::Approx(5.0f));
}

TEST_CASE("drops: taken while its copy is being made: the late copy is not our roll") {
    Game b(2);
    b.Tick();
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick();
    b.Tick(600);
    REQUIRE(b.creates.size() == 1);
    b.sync.Receive(proto::DropTake{1, Id(1, 7), proto::DropTakeReason::picked_up});
    b.Tick();
    CHECK(b.creates.empty());  // not listed: the director deletes it once made
    b.seen = {Lying(0x7000, 5.0f)};
    b.Tick();
    CHECK(Of<proto::DropSpawn>(b.send).empty());
    CHECK(b.removes.empty());
}

TEST_CASE("drops: a create that never comes is given up") {
    Game b(2);
    b.Tick();
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick();
    b.Tick(600);
    REQUIRE(b.creates.size() == 1);
    b.Tick(CoopDrops::Sync::kCreateWaitUs / kMs + 100);
    CHECK(b.creates.empty());
    CHECK(b.sync.Live() == 0);
}

TEST_CASE("drops: a player who comes in is told the drops still lying, where they are now") {
    Game a(1);
    a.Tick();
    a.seen = {Lying(0x1000, 5.0f), Lying(0x2000, 30.0f, kHeart)};
    a.Tick();
    REQUIRE(Of<proto::DropSpawn>(a.send).size() == 2);
    const std::uint64_t made_at = a.now;
    a.seen[0].position[0] = 6.0f;  // it rolled
    a.player = Vec3{30.0f, 0.0f, 0.0f};
    a.Gone(0x2000);  // we picked the heart up
    a.Tick();
    a.Tick();
    a.Tick(CoopDrops::Sync::kTakeHoldUs / kMs);
    CHECK(Of<proto::DropTake>(a.send).size() == 1);

    a.sync.Receive(proto::DropAsk{2, "Another_Scene"});
    a.Tick(1000);
    CHECK(a.send.empty());
    a.sync.Receive(proto::DropAsk{2, kScene});
    a.Tick();
    CHECK(a.send.empty());
    a.Tick(CoopDrops::Sync::kTellAfterUs / kMs);
    const auto again = Of<proto::DropSpawn>(a.send);
    REQUIRE(again.size() == 1);
    CHECK(again[0].actor == kGreen);
    CHECK(again[0].flags == proto::drop_spawn::again);
    CHECK(again[0].position[0] == doctest::Approx(6.0f));
    CHECK(again[0].time_us == made_at);

    // The newcomer makes it at once.
    Game b(2);
    b.Tick();
    auto got = again[0];
    got.player_number = 1;
    b.sync.Receive(got);
    b.Tick();
    REQUIRE(b.creates.size() == 1);
    CHECK(b.creates[0].position[0] == doctest::Approx(6.0f));
}

TEST_CASE("drops: drops of another scene are not ours, a new scene forgets") {
    Game b(2);
    b.Tick();
    auto other = Told(1, 7, 5.0f, b.now);
    other.scene = "Another_Scene";
    b.sync.Receive(other);
    b.sync.Receive(Told(1, 8, 5.0f, b.now, "Enemy_ReDead"));  // not a drop: never made
    b.Tick(1000);
    CHECK(b.creates.empty());
    CHECK(b.sync.Known() == 0);

    b.sync.Receive(Told(1, 9, 5.0f, b.now));
    b.Tick();
    CHECK(b.sync.Known() == 1);
    std::vector<proto::Message> send;
    std::vector<Create> creates;
    std::vector<Remove> removes;
    b.sync.Tick("Another_Scene", 2, b.now + 33'000, {}, b.player, {}, {}, send, creates, removes);
    CHECK(b.sync.Known() == 0);
    CHECK(Of<proto::DropAsk>(send).size() == 1);
}

TEST_CASE("drops: at most 20 messages a second leave") {
    Game a(1);
    a.Tick();
    for (std::uint64_t i = 0; i < 50; ++i) {
        a.seen.push_back(Lying(0x1000 + i * 0x100, 10.0f * float(i)));
    }
    a.Tick();
    std::size_t sent = a.send.size();
    CHECK(sent <= CoopDrops::Sync::kSendsPerSecond);
    for (int i = 0; i < 30; ++i) {  // one second
        a.Tick();
        sent += a.send.size();
    }
    CHECK(sent <= 2 * CoopDrops::Sync::kSendsPerSecond + 1);
    for (int i = 0; i < 60; ++i) {
        a.Tick();
        sent += a.send.size();
    }
    CHECK(sent == 50);
}

TEST_CASE("drops: the remover deletes a live actor of that name, once") {
    namespace addr = CoopPuppet::addr;
    constexpr std::uint64_t base = 0x8000000;
    constexpr std::uint64_t actor = 0x40000000, other = 0x41000000, text = 0x50000000;
    FakeGuest guest;
    guest.Map(actor, 0x600);
    guest.Map(other, 0x600);
    guest.Map(text, 0x100);
    REQUIRE(guest.Write(text, "Obj_Rupee_Green", 16));
    REQUIRE(guest.Write(text + 0x40, "Obj_Rupee_Green_Big", 20));
    for (const std::uint64_t a : {actor, other}) {
        guest.Set(a + addr::kActorState, std::uint32_t{4});
        guest.Set(a + 0x538 + 0x10, a);
    }
    guest.Set(actor + 0x260, text);
    guest.Set(other + 0x260, text + 0x40);  // the address holds something else now

    CoopDrops::Remover remover;
    CHECK_FALSE(remover.Step(guest, false, 0, 0));
    remover.Want(base, {{other, kGreen}, {actor, kGreen}, {0x99000000, kGreen}});
    auto c = remover.Step(guest, false, 0, 0);
    REQUIRE(c);
    CHECK(c->function == base + addr::kRequestDelete);
    CHECK(c->x[0] == actor);
    CHECK(c->x[1] == 0);
    CHECK_FALSE(remover.Step(guest, true, 0, 0));
    CHECK(remover.TakeCount() == 1);
    // Still wanted the next frame: not asked twice.
    CHECK_FALSE(remover.Step(guest, false, 0, 16));

    // Not alive yet is looked at again.
    guest.Set(other + 0x260, text);
    guest.Set(other + addr::kActorState, std::uint32_t{3});
    CHECK_FALSE(remover.Step(guest, false, 0, 33));
    guest.Set(other + addr::kActorState, std::uint32_t{4});
    c = remover.Step(guest, false, 0, 50);
    REQUIRE(c);
    CHECK(c->x[0] == other);
    CHECK(remover.TakeCount() == 1);
}

TEST_CASE("drops: the director lets go of a released copy without deleting it") {
    namespace addr = CoopPuppet::addr;
    constexpr std::uint64_t base = 0x8000000;
    constexpr std::uint64_t block = 0x30000000;
    constexpr std::uint64_t actor = 0x40000000;
    constexpr std::uint64_t request = 0x50000000;
    FakeGuest guest;
    guest.Map(base + addr::kActorManagerPtr, 8);
    guest.Map(base + addr::kGotCreateDefault, 8);
    guest.Set(base + addr::kActorManagerPtr, std::uint64_t{0x20000000});
    guest.Map(block, CoopSpawn::Director::kSlots * CoopSpawn::Director::kSlotSize);
    guest.Map(actor, 0x600);
    guest.Map(request, 0x10);
    guest.Set(actor, std::uint64_t{0xabc});
    guest.Set(actor + addr::kActorState, std::uint32_t{addr::kStatePresent});
    guest.Set(actor + addr::kActorPosition, 9.0f);

    CoopSpawn::Director d;
    CoopSpawn::Copy copy;
    copy.key = (std::uint64_t{1} << 62) | 5;
    copy.actor = kGreen;
    copy.position = {1.0f, 2.0f, 3.0f};
    copy.place = false;
    d.SetDrops(base, {copy});
    REQUIRE(d.Step(guest, false, 0, 0));  // malloc
    auto c = d.Step(guest, true, block, 0);
    REQUIRE(c);
    CHECK(c->function == base + addr::kCreate);
    const std::uint64_t handle = c->x[3];
    d.Step(guest, true, 0, 0);
    guest.Set(handle, request);
    guest.Set(request, std::uint32_t{addr::kRequestReady});
    REQUIRE(d.Step(guest, false, 0, 16));
    d.Step(guest, true, actor, 16);
    CHECK_FALSE(d.Step(guest, false, 0, 33));
    CHECK(d.Live().at(copy.key) == actor);
    float x = 0.0f;
    REQUIRE(guest.Read(actor + addr::kActorPosition, &x, 4));
    CHECK(x == doctest::Approx(9.0f));  // not placed: it lies where it fell

    copy.act = CoopSpawn::Act::release;
    d.SetDrops(base, {copy});
    CHECK_FALSE(d.Step(guest, false, 0, 50));  // no delete call
    CHECK(d.Live().empty());
    // No longer listed at all: still nothing to delete, and never made again.
    d.SetDrops(base, {});
    CHECK_FALSE(d.Step(guest, false, 0, 66));
    copy.act = CoopSpawn::Act::show;
    d.SetDrops(base, {copy});
    CHECK_FALSE(d.Step(guest, false, 0, 83));
}

TEST_CASE("drops: a take held while the scene changes is never told") {
    Game a(1);
    a.Tick();
    a.seen = {Lying(0x1000, 5.0f)};
    a.Tick();
    a.player = Vec3{5.0f, 0.0f, 0.0f};
    a.Gone(0x1000);  // the door: actors unload first
    a.Tick();
    a.Tick();
    CHECK(Of<proto::DropTake>(a.send).empty());
    // Then the scene name changes.
    a.send.clear();
    a.creates.clear();
    a.removes.clear();
    a.now += 100 * kMs;
    a.sync.Tick("Another_Scene", 1, a.now, {}, a.player, {}, {}, a.send, a.creates, a.removes);
    for (int i = 0; i < 30; ++i) {
        a.now += 33 * kMs;
        a.send.clear();
        a.sync.Tick("Another_Scene", 1, a.now, {}, a.player, {}, {}, a.send, a.creates, a.removes);
        CHECK(Of<proto::DropTake>(a.send).empty());
    }
}

TEST_CASE("drops: drops going one after another within the hold are an unload") {
    Game a(1);
    a.Tick();
    a.seen = {Lying(0x1000, 5.0f), Lying(0x2000, 7.0f), Lying(0x3000, 9.0f)};
    a.Tick();
    a.player = Vec3{7.0f, 0.0f, 0.0f};
    a.Gone(0x1000);
    a.Tick();
    a.Tick();
    a.Gone(0x2000);
    a.Tick();
    a.Tick();
    a.Gone(0x3000);
    a.Tick();
    a.Tick();
    for (int i = 0; i < 30; ++i) {
        a.Tick();
        CHECK(Of<proto::DropTake>(a.send).empty());
    }
    CHECK(a.sync.Live() == 0);
}

TEST_CASE("drops: a take in the walk after the director let go still deletes the copy") {
    Game b(2);
    b.Tick();
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick();
    b.Tick(600);
    REQUIRE(b.creates.size() == 1);
    const std::uint64_t key = b.creates[0].key;
    b.made[key] = 0x7000;
    b.seen = {Lying(0x7000, 5.0f)};
    b.Tick();
    REQUIRE(b.creates.size() == 1);
    CHECK(b.creates[0].release);
    // The director let go; in this same walk the take arrives.
    b.made.clear();
    b.sync.Receive(proto::DropTake{1, Id(1, 7), proto::DropTakeReason::picked_up});
    b.Tick();
    REQUIRE(b.removes.size() == 1);  // asked of the remover, not left to the director
    CHECK(b.removes[0] == Remove{0x7000, kGreen});
}

TEST_CASE("drops: a delete that never comes true is asked of the remover, never our roll") {
    Game b(2);
    b.Tick();
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick();
    b.Tick(600);
    REQUIRE(b.creates.size() == 1);
    const std::uint64_t key = b.creates[0].key;
    b.made[key] = 0x7000;
    b.seen = {Lying(0x7000, 5.0f)};
    b.Tick();
    // Taken while the director still holds it: left to the director.
    b.sync.Receive(proto::DropTake{1, Id(1, 7), proto::DropTakeReason::picked_up});
    b.Tick();
    CHECK(b.removes.empty());
    // But it stays (the director had let go after all).
    b.made.clear();
    bool asked = false;
    for (int i = 0; i < 120; ++i) {
        b.Tick();
        CHECK(Of<proto::DropSpawn>(b.send).empty());
        asked = asked || !b.removes.empty();
    }
    CHECK(asked);
}

TEST_CASE("drops: after a short loss of the session the drops lying here are kept, not doubled") {
    Game b(2);
    b.Tick();
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick();
    b.Tick(600);
    REQUIRE(b.creates.size() == 1);
    b.made[b.creates[0].key] = 0x7000;
    b.seen = {Lying(0x7000, 5.0f), Lying(0x2000, 20.0f, kHeart)};
    b.Tick();
    REQUIRE(Of<proto::DropSpawn>(b.send).size() == 1);  // our own heart
    const auto own = Of<proto::DropSpawn>(b.send)[0];
    b.made.clear();
    b.Tick();

    // The session drops and comes back: everything is forgotten.
    b.sync.Reset();
    b.Tick();
    REQUIRE(Of<proto::DropAsk>(b.send).size() == 1);
    // The others tell both again: ours (the heart) and P1's rupee.
    auto again_own = own;
    again_own.player_number = 1;
    again_own.flags = proto::drop_spawn::again;
    b.sync.Receive(again_own);
    b.sync.Receive(Told(1, 7, 5.0f, b.now, kGreen, proto::drop_spawn::again));
    b.Tick();
    CHECK(b.creates.empty());  // nothing made twice
    CHECK(b.sync.IdOf(0x7000) == Id(1, 7));
    CHECK(b.sync.IdOf(0x2000) == own.id);
    // And a pickup of the old rupee is still shared.
    b.player = Vec3{5.0f, 0.0f, 0.0f};
    b.Gone(0x7000);
    b.Tick();
    b.Tick();
    b.Tick(CoopDrops::Sync::kTakeHoldUs / kMs);
    REQUIRE(Of<proto::DropTake>(b.send).size() == 1);
    CHECK(Of<proto::DropTake>(b.send)[0].id == Id(1, 7));
}

TEST_CASE("drops: a server time that steps back does not end the waits") {
    Game b(2);
    b.now = 10'000'000;
    b.Tick();
    b.sync.Receive(Told(1, 7, 5.0f, b.now));
    b.Tick();
    b.Tick(600);
    REQUIRE(b.creates.size() == 1);
    // The clock offset is measured again: two seconds back.
    b.now -= 2'000'000;
    b.Tick();
    REQUIRE(b.creates.size() == 1);  // still asked, not given up
    CHECK(b.sync.Live() == 1);
}
