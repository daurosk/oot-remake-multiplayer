// Tests for things a player makes (lichii/hook/coop_spawn.cpp): the sync's
// messages and copies, and the director's calls at the cave.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_spawn.h"

namespace proto = coop::protocol;
using CoopSpawn::Act;
using CoopSpawn::Copy;
using CoopSpawn::Seen;

namespace {

constexpr std::uint64_t kMs = 1000;
const std::string kScene = "Spot_GraveYard";

Seen Made(std::uint64_t address, float x, std::string name = "Item_Bomb") {
    Seen s;
    s.address = address;
    s.name = std::move(name);
    s.position = {x, 1.0f, 2.0f};
    return s;
}

// Encoded and decoded, as the server would pass it on.
proto::Message Wire(proto::Message m, std::uint8_t from) {
    std::visit([&](auto& v) {
        if constexpr (requires { v.player_number; }) {
            v.player_number = from;
        }
    }, m);
    const auto bytes = proto::encode(m);
    REQUIRE(bytes);
    proto::Message out;
    REQUIRE(proto::decode(*bytes, out) == proto::Error::none);
    return out;
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
    T Get(std::uint64_t address) {
        T v{};
        REQUIRE(Read(address, &v, sizeof(T)));
        return v;
    }
    template <typename T>
    void Set(std::uint64_t address, const T& v) {
        REQUIRE(Write(address, &v, sizeof(T)));
    }

private:
    std::map<std::uint64_t, std::array<std::uint8_t, 0x1000>> pages_;
};

} // namespace

TEST_CASE("spawn: our bomb is spawned, sampled and removed as exploded") {
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<Copy> copies;
    const std::map<std::uint64_t, std::uint64_t> none;
    std::uint64_t now = 1'000'000;

    // On the list when we came: not ours to share.
    sync.Tick(kScene, 1, now, {Made(0x5000, 0.0f)}, none, send, copies);
    CHECK(send.empty());

    now += 33 * kMs;
    sync.Tick(kScene, 1, now, {Made(0x5000, 0.0f), Made(0x6000, 3.0f)}, none, send, copies);
    auto spawns = Of<proto::ObjectSpawn>(send);
    REQUIRE(spawns.size() == 1);
    CHECK(spawns[0].actor == "Item_Bomb");
    CHECK(spawns[0].id.kind == proto::ObjectKind::made);
    CHECK(spawns[0].id.maker == 1);
    CHECK(spawns[0].flags == proto::object_spawn::projectile);
    CHECK((*spawns[0].state.position)[0] == doctest::Approx(3.0f));
    CHECK(proto::encode(send[0]));
    const proto::ObjectId id = spawns[0].id;

    send.clear();
    now += 40 * kMs;
    sync.Tick(kScene, 1, now, {Made(0x5000, 0.0f), Made(0x6000, 4.0f)}, none, send, copies);
    auto samples = Of<proto::ObjectSample>(send);
    REQUIRE(samples.size() == 1);
    CHECK(samples[0].id == id);
    CHECK((*samples[0].state.position)[0] == doctest::Approx(4.0f));
    CHECK((*samples[0].state.velocity)[0] == doctest::Approx(25.0f));

    // The fuse burns (a bomb lasts a few seconds) without moving.
    now += 3'000 * kMs;
    sync.Tick(kScene, 1, now, {Made(0x5000, 0.0f), Made(0x6000, 4.0f)}, none, send, copies);

    // Gone for two walks: removed, exploded, where it was last.
    send.clear();
    sync.Tick(kScene, 1, now + 33 * kMs, {Made(0x5000, 0.0f)}, none, send, copies);
    CHECK(Of<proto::ObjectRemove>(send).empty());
    sync.Tick(kScene, 1, now + 66 * kMs, {Made(0x5000, 0.0f)}, none, send, copies);
    auto removes = Of<proto::ObjectRemove>(send);
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].id == id);
    CHECK(removes[0].reason == proto::RemoveReason::exploded);
    CHECK(removes[0].position[0] == doctest::Approx(4.0f));
    CHECK(sync.Mine() == 0);
}

TEST_CASE("spawn: a bomb gone within a second did not explode") {
    // Taking out a bomb the first time makes one that lasts about 0.1 s while
    // the game shows its help box: the copy must not blow up.
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<Copy> copies;
    const std::map<std::uint64_t, std::uint64_t> none;
    std::uint64_t now = 1'000'000;
    sync.Tick(kScene, 1, now, {}, none, send, copies);
    now += 33 * kMs;
    sync.Tick(kScene, 1, now, {Made(0x6000, 3.0f)}, none, send, copies);
    REQUIRE(Of<proto::ObjectSpawn>(send).size() == 1);
    sync.Tick(kScene, 1, now + 33 * kMs, {}, none, send, copies);
    sync.Tick(kScene, 1, now + 66 * kMs, {}, none, send, copies);
    const auto removes = Of<proto::ObjectRemove>(send);
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].reason == proto::RemoveReason::gone);
}

TEST_CASE("spawn: a seed is removed as gone, and other actors are not shared") {
    CHECK(CoopSpawn::IsShared("Shootable_DekuSeed"));
    CHECK_FALSE(CoopSpawn::IsShared("Enemy_Stalchild"));
    CHECK_FALSE(CoopSpawn::Explodes("Item_DekuNut"));
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<Copy> copies;
    const std::map<std::uint64_t, std::uint64_t> none;
    sync.Tick(kScene, 2, 1'000'000, {}, none, send, copies);
    sync.Tick(kScene, 2, 1'033'000, {Made(0x7000, 0.0f, "Shootable_DekuSeed")}, none, send, copies);
    sync.Tick(kScene, 2, 1'066'000, {}, none, send, copies);
    sync.Tick(kScene, 2, 1'099'000, {}, none, send, copies);
    const auto removes = Of<proto::ObjectRemove>(send);
    REQUIRE(removes.size() == 1);
    CHECK(removes[0].reason == proto::RemoveReason::gone);
    CHECK(removes[0].id.maker == 2);
}

TEST_CASE("spawn: another player's bomb becomes a copy that is never sent back") {
    CoopSpawn::Sync sync;
    std::vector<proto::Message> send;
    std::vector<Copy> copies;
    std::map<std::uint64_t, std::uint64_t> live;
    std::uint64_t now = 5'000'000;
    sync.Tick(kScene, 2, now, {}, live, send, copies);

    proto::ObjectSpawn spawn;
    spawn.id = {proto::ObjectKind::made, 1, 77};
    spawn.actor = "Item_Bomb";
    spawn.state.time = {4'990'000, 0};
    spawn.state.position = proto::Vec3{10.0f, 1.0f, 2.0f};
    spawn.state.velocity = proto::Vec3{0.0f, 0.0f, 0.0f};
    sync.Receive(Wire(spawn, 1));
    now += 33 * kMs;
    sync.Tick(kScene, 2, now, {}, live, send, copies);
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].actor == "Item_Bomb");
    CHECK(copies[0].act == Act::show);
    CHECK(copies[0].position[0] == doctest::Approx(10.0f));
    const std::uint64_t key = copies[0].key;

    // The copy shows up on our list before the director says so: claimed.
    copies.clear();
    now += 33 * kMs;
    sync.Tick(kScene, 2, now, {Made(0x9000, 10.2f)}, live, send, copies);
    CHECK(send.empty());

    // The owner's sample moves it, with its velocity carried on from the
    // sample's time on the server clock (99 ms at 10 m/s).
    proto::ObjectSample sample;
    sample.id = spawn.id;
    sample.state.time = {5'100'000, 1};
    sample.state.position = proto::Vec3{12.0f, 0.0f, 0.0f};
    sample.state.velocity = proto::Vec3{10.0f, 0.0f, 0.0f};
    sync.Receive(Wire(sample, 1));
    live[key] = 0x9000;
    copies.clear();
    now += 33 * kMs;
    sync.Tick(kScene, 2, now, {Made(0x9000, 12.0f)}, live, send, copies);
    CHECK(send.empty());
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].position[0] == doctest::Approx(12.0f));
    copies.clear();
    now += 100 * kMs;
    sync.Tick(kScene, 2, now, {Made(0x9000, 12.0f)}, live, send, copies);
    CHECK(copies[0].position[0] == doctest::Approx(12.99f));

    // The owner's bomb exploded: so does the copy, at the owner's place.
    proto::ObjectRemove remove;
    remove.id = spawn.id;
    remove.reason = proto::RemoveReason::exploded;
    remove.position = {14.0f, 0.0f, 0.0f};
    sync.Receive(Wire(remove, 1));
    copies.clear();
    now += 33 * kMs;
    sync.Tick(kScene, 2, now, {Made(0x9000, 12.0f)}, live, send, copies);
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].act == Act::explode);
    CHECK(copies[0].position[0] == doctest::Approx(14.0f));
    CHECK(send.empty());

    // A new scene forgets it.
    copies.clear();
    sync.Tick("Spot_Other", 2, now + 33 * kMs, {}, {}, send, copies);
    CHECK(copies.empty());
}

TEST_CASE("spawn: the director creates a copy, moves it and explodes it") {
    namespace addr = CoopPuppet::addr;
    constexpr std::uint64_t base = 0x8000000;
    constexpr std::uint64_t manager = 0x20000000;
    constexpr std::uint64_t block = 0x30000000;
    constexpr std::uint64_t actor = 0x40000000;
    constexpr std::uint64_t request = 0x50000000;
    constexpr std::uint64_t table = 0x60000000, life = 0x61000000, value = 0x62000000;
    FakeGuest guest;
    guest.Map(base + addr::kActorManagerPtr, 8);
    guest.Map(base + addr::kGotCreateDefault, 8);
    guest.Set(base + addr::kActorManagerPtr, manager);
    guest.Set(base + addr::kGotCreateDefault, std::uint64_t{0x1234});
    guest.Map(block, CoopSpawn::Director::kSlots * CoopSpawn::Director::kSlotSize);
    guest.Map(actor, 0x600);
    guest.Map(request, 0x10);
    guest.Map(table, 0x1000);
    guest.Map(life, 0x2000);
    guest.Map(value, 0x10);
    guest.Set(actor, std::uint64_t{0xabc});  // vtable
    guest.Set(actor + addr::kActorState, std::uint32_t{addr::kStatePresent});
    guest.Set(actor + 0x270, table);
    guest.Set(table + 0x2b8, life);
    guest.Set(life + 0x1c10, value);
    guest.Set(value + 8, std::int32_t{1});

    std::vector<std::string> said;
    CoopSpawn::Director d([&](const std::string& s) { said.push_back(s); });
    Copy copy;
    copy.key = 1;
    copy.actor = "Item_Bomb";
    copy.position = {1.0f, 2.0f, 3.0f};
    d.SetInput(base, {copy});

    auto c = d.Step(guest, false, 0, 0);
    REQUIRE(c);
    CHECK(c->function == base + addr::kMalloc);
    c = d.Step(guest, true, block, 0);
    REQUIRE(c);
    CHECK(c->function == base + addr::kCreate);
    CHECK(c->x[0] == manager);
    CHECK(guest.Get<float>(c->x[2]) == doctest::Approx(1.0f));
    char name[10]{};
    std::uint64_t text = guest.Get<std::uint64_t>(c->x[1]);
    REQUIRE(guest.Read(text, name, 9));
    CHECK(std::string(name) == "Item_Bomb");
    const std::uint64_t handle = c->x[3];
    CHECK_FALSE(d.Step(guest, true, 0, 0));

    // The request is ready the next frame: taken.
    guest.Set(handle, request);
    guest.Set(request, std::uint32_t{addr::kRequestReady});
    c = d.Step(guest, false, 0, 16);
    REQUIRE(c);
    CHECK(c->function == base + addr::kTakeCreated);
    CHECK(c->x[0] == request);
    CHECK_FALSE(d.Step(guest, true, actor, 16));
    CHECK(guest.Get<std::uint64_t>(handle) == 0);

    // Live: moved every frame, and the director says where it is.
    copy.position = {5.0f, 6.0f, 7.0f};
    d.SetInput(base, {copy});
    CHECK_FALSE(d.Step(guest, false, 0, 33));
    CHECK(guest.Get<float>(actor + addr::kActorPosition) == doctest::Approx(5.0f));
    CHECK(d.Live().at(1) == actor);

    // Exploded by the owner: life 0, never created again.
    copy.act = Act::explode;
    d.SetInput(base, {copy});
    CHECK_FALSE(d.Step(guest, false, 0, 50));
    CHECK(guest.Get<std::int32_t>(value + 8) == 0);

    // Its game removed it: no copy is made again for the same key.
    guest.Set(actor + addr::kActorState, std::uint32_t{8});
    CHECK_FALSE(d.Step(guest, false, 0, 66));
    CHECK(d.Live().empty());
}

TEST_CASE("spawn: a seed copy the owner removed is deleted") {
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

    CoopSpawn::Director d;
    Copy copy;
    copy.key = 3;
    copy.actor = "Shootable_DekuSeed";
    d.SetInput(base, {copy});
    REQUIRE(d.Step(guest, false, 0, 0));
    auto c = d.Step(guest, true, block, 0);
    REQUIRE(c);
    const std::uint64_t handle = c->x[3];
    d.Step(guest, true, 0, 0);
    guest.Set(handle, request);
    guest.Set(request, std::uint32_t{addr::kRequestReady});
    REQUIRE(d.Step(guest, false, 0, 16));
    d.Step(guest, true, actor, 16);

    copy.act = Act::remove;
    d.SetInput(base, {copy});
    c = d.Step(guest, false, 0, 33);
    REQUIRE(c);
    CHECK(c->function == base + addr::kRequestDelete);
    CHECK(c->x[0] == actor);
    CHECK_FALSE(d.Step(guest, true, 0, 33));
    CHECK(d.Live().empty());
}

TEST_CASE("spawn: a rolled enemy copy that went by itself is made again, at most remakes times") {
    // Issue 124: a Stalchild copy sank into the ground by its own AI 2 s after
    // it was made, while the decider's one still fought, and was never made again.
    namespace addr = CoopPuppet::addr;
    constexpr std::uint64_t base = 0x8000000;
    constexpr std::uint64_t block = 0x30000000;
    constexpr std::uint64_t request = 0x50000000;
    FakeGuest guest;
    guest.Map(base + addr::kActorManagerPtr, 8);
    guest.Map(base + addr::kGotCreateDefault, 8);
    guest.Set(base + addr::kActorManagerPtr, std::uint64_t{0x20000000});
    guest.Map(block, CoopSpawn::Director::kSlots * CoopSpawn::Director::kSlotSize);
    guest.Map(request, 0x10);

    std::vector<std::string> said;
    CoopSpawn::Director d([&](const std::string& s) { said.push_back(s); });
    Copy copy;
    copy.key = (1ULL << 63) | 1;
    copy.actor = "Enemy_Stalchild";
    copy.place = false;
    copy.remakes = 2;
    d.SetEnemies(base, {copy});

    // Creates the copy as `actor` at now_ms; true when it was made.
    const auto make = [&](std::uint64_t actor, std::uint64_t now_ms) {
        guest.Map(actor, 0x600);
        guest.Set(actor, std::uint64_t{0xabc});
        guest.Set(actor + addr::kActorState, std::uint32_t{addr::kStatePresent});
        auto c = d.Step(guest, false, 0, now_ms);
        if (c && c->function == base + addr::kMalloc) {
            c = d.Step(guest, true, block, now_ms);
        }
        if (!c || c->function != base + addr::kCreate) {
            return false;
        }
        const std::uint64_t handle = c->x[3];
        CHECK_FALSE(d.Step(guest, true, 0, now_ms));
        guest.Set(handle, request);
        guest.Set(request, std::uint32_t{addr::kRequestReady});
        c = d.Step(guest, false, 0, now_ms + 16);
        REQUIRE(c);
        CHECK(c->function == base + addr::kTakeCreated);
        CHECK_FALSE(d.Step(guest, true, actor, now_ms + 16));
        CHECK_FALSE(d.Step(guest, false, 0, now_ms + 33));  // Live() is set at a new frame
        return d.Live().count(copy.key) == 1 && d.Live().at(copy.key) == actor;
    };

    REQUIRE(make(0x40000000, 0));
    // Its own AI deletes it: gone, and not made again at once.
    guest.Set(std::uint64_t{0x40000000} + addr::kActorState, std::uint32_t{8});
    CHECK_FALSE(d.Step(guest, false, 0, 2000));
    CHECK(d.Live().empty());
    CHECK_FALSE(d.Step(guest, false, 0, 2500));
    // A second later it is made again.
    REQUIRE(make(0x41000000, 3000));
    CHECK(std::any_of(said.begin(), said.end(), [](const std::string& s) {
        return s.find("went by itself; made again (1 of 2)") != std::string::npos;
    }));

    guest.Set(std::uint64_t{0x41000000} + addr::kActorState, std::uint32_t{8});
    CHECK_FALSE(d.Step(guest, false, 0, 4000));
    REQUIRE(make(0x42000000, 5000));

    // The third time it stays gone.
    guest.Set(std::uint64_t{0x42000000} + addr::kActorState, std::uint32_t{8});
    CHECK_FALSE(d.Step(guest, false, 0, 6000));
    CHECK_FALSE(make(0x43000000, 8000));
    CHECK(d.Live().empty());
    CHECK(std::any_of(said.begin(), said.end(), [](const std::string& s) {
        return s.find("not made again") != std::string::npos;
    }));
}

TEST_CASE("spawn: a copy without remakes that went by itself is not made again") {
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

    CoopSpawn::Director d;
    Copy copy;
    copy.key = (1ULL << 63) | 2;
    copy.actor = "Enemy_Stalchild";
    copy.place = false;
    d.SetEnemies(base, {copy});
    REQUIRE(d.Step(guest, false, 0, 0));
    auto c = d.Step(guest, true, block, 0);
    REQUIRE(c);
    const std::uint64_t handle = c->x[3];
    d.Step(guest, true, 0, 0);
    guest.Set(handle, request);
    guest.Set(request, std::uint32_t{addr::kRequestReady});
    REQUIRE(d.Step(guest, false, 0, 16));
    d.Step(guest, true, actor, 16);
    CHECK_FALSE(d.Step(guest, false, 0, 33));
    REQUIRE(d.Live().count(copy.key) == 1);

    guest.Set(actor + addr::kActorState, std::uint32_t{8});
    CHECK_FALSE(d.Step(guest, false, 0, 100));
    CHECK_FALSE(d.Step(guest, false, 0, 5000));
    CHECK(d.Live().empty());
}

TEST_CASE("spawn: a copy waiting to be made again is not made once the input says remakes 0") {
    // Review of issue 124: a copy killed here is gone for the director at once,
    // while Sync sends remakes 0 only after two missed walks.
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

    std::vector<std::string> said;
    CoopSpawn::Director d([&](const std::string& s) { said.push_back(s); });
    Copy copy;
    copy.key = (1ULL << 63) | 3;
    copy.actor = "Enemy_Stalchild";
    copy.place = false;
    copy.remakes = 3;
    d.SetEnemies(base, {copy});
    REQUIRE(d.Step(guest, false, 0, 0));
    auto c = d.Step(guest, true, block, 0);
    REQUIRE(c);
    const std::uint64_t handle = c->x[3];
    d.Step(guest, true, 0, 0);
    guest.Set(handle, request);
    guest.Set(request, std::uint32_t{addr::kRequestReady});
    REQUIRE(d.Step(guest, false, 0, 16));
    d.Step(guest, true, actor, 16);
    CHECK_FALSE(d.Step(guest, false, 0, 33));
    REQUIRE(d.Live().count(copy.key) == 1);

    // Killed here: gone while the input still says 3, so a remake is planned.
    guest.Set(actor + addr::kActorState, std::uint32_t{8});
    CHECK_FALSE(d.Step(guest, false, 0, 100));
    CHECK(std::any_of(said.begin(), said.end(), [](const std::string& s) {
        return s.find("made again (1 of 3)") != std::string::npos;
    }));
    // Two walks later Sync knows it died: remakes 0. Nothing is made, then or later.
    copy.remakes = 0;
    d.SetEnemies(base, {copy});
    CHECK_FALSE(d.Step(guest, false, 0, 200));
    CHECK_FALSE(d.Step(guest, false, 0, 1200));
    CHECK_FALSE(d.Step(guest, false, 0, 5000));
    CHECK(d.Live().empty());
}

namespace {

// A guest with a director's block and one actor ready to be taken.
struct StandInRig {
    static constexpr std::uint64_t base = 0x8000000;
    static constexpr std::uint64_t block = 0x30000000;
    static constexpr std::uint64_t actor = 0x40000000;
    static constexpr std::uint64_t request = 0x50000000;
    FakeGuest guest;
    StandInRig() {
        namespace addr = CoopPuppet::addr;
        guest.Map(base + addr::kActorManagerPtr, 8);
        guest.Map(base + addr::kGotCreateDefault, 8);
        guest.Set(base + addr::kActorManagerPtr, std::uint64_t{0x20000000});
        guest.Map(block, CoopSpawn::Director::kSlots * CoopSpawn::Director::kSlotSize);
        guest.Map(actor, 0x600);
        guest.Map(request, 0x10);
        guest.Set(actor, std::uint64_t{0xabc});
        guest.Set(actor + addr::kActorState, std::uint32_t{addr::kStatePresent});
    }
    // The name a create call asks for.
    std::string Name(const CoopPuppet::Call& c) {
        char name[32]{};
        REQUIRE(guest.Read(guest.Get<std::uint64_t>(c.x[1]), name, 31));
        return name;
    }
};

} // namespace

// Issue 85: a seed, arrow or hookshot copy is made from our stand-in pack (the
// game's own shootable made without a shooter is never drawn), placed as the
// puppet is (place and the pending transform, no setter call).
TEST_CASE("spawn: a seed copy is the stand-in the pack has, placed as the puppet is") {
    namespace addr = CoopPuppet::addr;
    using R = StandInRig;
    CHECK(std::string(CoopSpawn::StandIn("Shootable_DekuSeed")) == "CoopShot_Seed");
    CHECK(std::string(CoopSpawn::StandIn("NormalArrow")) == "CoopShot_Arrow");
    CHECK(std::string(CoopSpawn::StandIn("Shootable_Hook")) == "CoopShot_Hook");
    CHECK(std::string(CoopSpawn::StandIn("Shootable_HookLong")) == "CoopShot_HookLong");
    CHECK(CoopSpawn::StandIn("Item_Bomb").empty());
    CHECK(CoopSpawn::StandIn("Item_DekuNut").empty());
    CHECK(CoopSpawn::StandIn("Weapon_Boomerang").empty());

    StandInRig rig;
    CoopSpawn::Director d;
    d.SetStandIns({"CoopShot_Seed"});
    Copy copy;
    copy.key = 7;
    copy.actor = "Shootable_DekuSeed";
    copy.position = {1.0f, 2.0f, 3.0f};
    copy.rotation = std::array<float, 9>{0, 0, 1, 0, 1, 0, -1, 0, 0};
    copy.setter = true;
    d.SetInput(R::base, {copy});
    REQUIRE(d.Step(rig.guest, false, 0, 0));  // malloc
    auto c = d.Step(rig.guest, true, R::block, 0);
    REQUIRE(c);
    REQUIRE(c->function == R::base + addr::kCreate);
    CHECK(rig.Name(*c) == "CoopShot_Seed");
    const std::uint64_t handle = c->x[3];
    d.Step(rig.guest, true, 0, 0);
    rig.guest.Set(handle, R::request);
    rig.guest.Set(R::request, std::uint32_t{addr::kRequestReady});
    REQUIRE(d.Step(rig.guest, false, 0, 16));  // take
    d.Step(rig.guest, true, R::actor, 16);

    // Live: the place and the pending transform are written, the setter is not called.
    copy.position = {5.0f, 6.0f, 7.0f};
    d.SetInput(R::base, {copy});
    CHECK_FALSE(d.Step(rig.guest, false, 0, 33));
    CHECK(rig.guest.Get<float>(R::actor + addr::kActorPosition) == doctest::Approx(5.0f));
    CHECK(rig.guest.Get<float>(R::actor + addr::kActorPositionRequest + 8) == doctest::Approx(7.0f));
    CHECK(rig.guest.Get<float>(R::actor + addr::kActorRotationRequest + 8) == doctest::Approx(1.0f));
    CHECK(d.Live().at(7) == R::actor);
}

// A name the pack does not list is never asked for (an old pack, or no pack):
// straight to the game's own actor.
TEST_CASE("spawn: without the stand-in in the pack, the copy is the game's own seed") {
    using R = StandInRig;
    namespace addr = CoopPuppet::addr;
    CHECK(CoopSpawn::StandInsIn("{\"actors\": [], \"sound\": true}").empty());
    CHECK(CoopSpawn::StandInsIn("{\"shots\": [{\"actor\": \"CoopShot_Seed\"}, {\"actor\": \"CoopShot_Hook\"}]}") ==
          std::set<std::string>{"CoopShot_Hook", "CoopShot_Seed"});
    CHECK(CoopSpawn::StandInsIn("CoopShot_SeedX").empty());

    for (const bool listed_other : {false, true}) {
        StandInRig rig;
        CoopSpawn::Director d;
        if (listed_other) {
            d.SetStandIns({"CoopShot_Arrow"});
        }
        Copy copy;
        copy.key = 1;
        copy.actor = "Shootable_DekuSeed";
        d.SetInput(R::base, {copy});
        REQUIRE(d.Step(rig.guest, false, 0, 0));
        auto c = d.Step(rig.guest, true, R::block, 0);
        REQUIRE(c);
        REQUIRE(c->function == R::base + addr::kCreate);
        CHECK(rig.Name(*c) == "Shootable_DekuSeed");
    }
}

// A stand-in that never becomes an actor (failed, timed out, dropped, or the
// take gave none) is made again; only kStandInTries failures in a row turn it
// off for the session.
TEST_CASE("spawn: a failed stand-in is tried again, then the game's own actor is used") {
    using R = StandInRig;
    namespace addr = CoopPuppet::addr;
    StandInRig rig;
    constexpr std::uint64_t failed = 0x51000000;
    rig.guest.Map(failed, 0x10);
    rig.guest.Set(failed, std::uint32_t{5});
    std::vector<std::string> said;
    CoopSpawn::Director d([&](const std::string& s) { said.push_back(s); });
    d.SetStandIns({"CoopShot_Seed"});
    Copy copy;
    copy.key = 1;
    copy.actor = "Shootable_DekuSeed";
    d.SetInput(R::base, {copy});
    REQUIRE(d.Step(rig.guest, false, 0, 0));  // malloc
    auto c = d.Step(rig.guest, true, R::block, 0);
    std::uint64_t now = 0;
    std::vector<std::string> asked;
    // Each create of the stand-in fails a different way: state 5, a drop, no actor.
    for (int round = 0; round < 4 && c && c->function == R::base + addr::kCreate; ++round) {
        asked.push_back(rig.Name(*c));
        const std::uint64_t handle = c->x[3];
        d.Step(rig.guest, true, 0, now);
        now += 16;
        if (round == 0) {
            rig.guest.Set(handle, failed);
            c = d.Step(rig.guest, false, 0, now);  // failure seen, made again at once
        } else if (round == 1) {
            rig.guest.Set(handle, R::request);
            rig.guest.Set(R::request, std::uint32_t{addr::kRequestDrop});
            c = d.Step(rig.guest, false, 0, now);  // the drop call
            REQUIRE(c);
            REQUIRE(c->function == R::base + addr::kDropCreate);
            d.Step(rig.guest, true, 0, now);
            now += 16;
            c = d.Step(rig.guest, false, 0, now);
        } else {
            rig.guest.Set(handle, R::request);
            rig.guest.Set(R::request, std::uint32_t{addr::kRequestReady});
            c = d.Step(rig.guest, false, 0, now);  // the take
            REQUIRE(c);
            REQUIRE(c->function == R::base + addr::kTakeCreated);
            d.Step(rig.guest, true, 0, now);  // it gave no actor
            now += 16;
            c = d.Step(rig.guest, false, 0, now);
        }
        now += 16;
    }
    REQUIRE(asked.size() == 4);
    CHECK(asked[0] == "CoopShot_Seed");
    CHECK(asked[1] == "CoopShot_Seed");
    CHECK(asked[2] == "CoopShot_Seed");
    CHECK(asked[3] == "Shootable_DekuSeed");
    CHECK(std::any_of(said.begin(), said.end(), [](const std::string& s) {
        return s.find("could not be made 3 times in a row") != std::string::npos;
    }));
}
