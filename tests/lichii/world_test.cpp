// Tests for the shared world objects (lichii/hook/coop_world.cpp) and the
// break that is read just before an object leaves the list
// (coop_objects_sync.cpp, Flush). No emulator code.
#include <doctest/doctest.h>

#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop_objects_sync.h"
#include "coop_world.h"

namespace proto = coop::protocol;
using CoopObjects::LifeWrite;
using CoopObjects::Seen;
using CoopObjects::Sync;

namespace {

constexpr std::uint64_t kMs = 1000;
const std::string kScene = "KakarikoVillage_Child";

Seen Pot(std::uint64_t address, std::uint64_t hash, std::int32_t life,
         std::string name = "Obj_Pot_A_01") {
    Seen s;
    s.address = address;
    s.name = std::move(name);
    s.hash = hash;
    s.life = proto::ObjectLife{life, 1};
    s.position = {1.0f, 2.0f, 3.0f};
    return s;
}

struct Step {
    std::vector<proto::Message> send;
    std::vector<LifeWrite> writes;
};

Step Tick(Sync& sync, std::uint64_t now, const std::vector<Seen>& seen, std::uint8_t self = 1) {
    Step step;
    sync.Tick(kScene, self, now, seen, step.send, step.writes);
    return step;
}

template <typename T>
std::vector<T> All(const Step& step) {
    std::vector<T> out;
    for (const auto& m : step.send) {
        if (const auto* t = std::get_if<T>(&m)) {
            out.push_back(*t);
        }
    }
    return out;
}

} // namespace

TEST_CASE("world: the breakables are shared, other world objects are not") {
    for (const char* name : {"Obj_Pot_A_01", "Obj_Pot_A_02_Respawn", "Obj_BrokenBox_A_02",
                             "Obj_LiftRock_A_03", "Obj_BrokenRock_A_01", "Obj_Plant_Grass_A_01",
                             "Obj_SignBoard_A_01", "Obj_Kokiri_Signboard_A_01",
                             "Obj_DodongoCave_CrackWall_A_01", "Obj_ShadowTemple_SkullPot_A_01",
                             "Obj_Kokiri_Beehive_A_01", "Obj_GuidePost_A_02",
                             "Obj_InsideGanonCastle_SpiderWeb_A_01", "Obj_Goron_BrokenRock_A_01",
                             // grass actors with no Obj_Plant_Grass_ name
                             "Land_kokiri_Plant_A_02_DropFixed_03", "Land_Kokiri_Plant_G_01",
                             "Land_Kokiri_Zenmai_A_02", "Obj_Plant_DestructibleGrass_Base"}) {
        CAPTURE(name);
        CHECK(CoopWorld::IsBreakable(name));
        CHECK(CoopObjects::IsShared(name));
    }
    for (const char* name : {"Obj_TBox_A_04", "Obj_Kakariko_Door_A_01", "Obj_StepSwitch_A_01",
                             "Obj_Kakariko_CookingPot_A_01", "Obj_GoronCity_LargePot_A_01",
                             "Land_Graveyard_Plants_A_10", "Land_kokiri_Plant_A_01", "Item_Bomb", "Player_Child",
                             "Obj_Pot"}) {
        CAPTURE(name);
        CHECK_FALSE(CoopWorld::IsBreakable(name));
    }
    CHECK(CoopObjects::IsShared("Enemy_Stalchild"));
}

TEST_CASE("world: a pot keeps its placement id") {
    Sync sync;
    Tick(sync, 0, {Pot(0x100, 0x8796851d6bbe37c, 1)});
    const auto id = sync.IdOf(0x100);
    REQUIRE(id);
    CHECK(id->kind == proto::ObjectKind::placed);
    CHECK(id->value == 0x8796851d6bbe37c);
}

TEST_CASE("world: the owner's pot broken, then gone two walks later, still sends life 0") {
    Sync sync;
    Tick(sync, 0, {Pot(0x100, 0x11, 1)});
    const auto id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 0, 0});
    auto first = All<proto::ObjectSample>(Tick(sync, 10 * kMs, {Pot(0x100, 0x11, 1)}));
    REQUIRE(first.size() == 1);
    // Broken: life 0 read, then removed before a sample went (within 50 ms).
    CHECK(All<proto::ObjectSample>(Tick(sync, 20 * kMs, {Pot(0x100, 0x11, 0)})).empty());
    CHECK(All<proto::ObjectSample>(Tick(sync, 25 * kMs, {})).empty());  // missed once
    const auto last = All<proto::ObjectSample>(Tick(sync, 30 * kMs, {}));
    REQUIRE(last.size() == 1);
    CHECK(last[0].id == id);
    REQUIRE(last[0].state.life);
    CHECK(last[0].state.life->life == 0);
    CHECK(sync.Count() == 0);
}

TEST_CASE("world: a pot that leaves with life left (unloaded) sends nothing") {
    Sync sync;
    Tick(sync, 0, {Pot(0x100, 0x11, 1)});
    sync.Receive(proto::ObjectOwner{*sync.IdOf(0x100), 1, 0, 0});
    Tick(sync, 10 * kMs, {Pot(0x100, 0x11, 1)});
    Tick(sync, 15 * kMs, {});
    const auto step = Tick(sync, 20 * kMs, {});
    CHECK(All<proto::ObjectSample>(step).empty());
    CHECK(All<proto::Damage>(step).empty());
}

TEST_CASE("world: a copy broken and gone before 100 ms sends its DAMAGE at once") {
    Sync sync;
    Tick(sync, 0, {Pot(0x100, 0x11, 1)}, 2);
    const auto id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 3, 0});
    Tick(sync, 10 * kMs, {Pot(0x100, 0x11, 1)}, 2);
    CHECK(All<proto::Damage>(Tick(sync, 20 * kMs, {Pot(0x100, 0x11, 0)}, 2)).empty());
    CHECK(All<proto::Damage>(Tick(sync, 25 * kMs, {}, 2)).empty());  // missed once
    const auto damage = All<proto::Damage>(Tick(sync, 30 * kMs, {}, 2));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].id == id);
    CHECK(damage[0].epoch == 3);
    CHECK(damage[0].amount == 1);
}

TEST_CASE("world: the owner's life 0 breaks the copy") {
    Sync sync;
    Tick(sync, 0, {Pot(0x100, 0x11, 1)}, 2);
    const auto id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 0, 0});
    Tick(sync, 10 * kMs, {Pot(0x100, 0x11, 1)}, 2);
    proto::ObjectSample sample;
    sample.id = id;
    sample.epoch = 0;
    sample.state.time.server_time_us = 20 * kMs;
    sample.state.time.sequence = 1;
    sample.state.life = proto::ObjectLife{0, 1};
    sync.Receive(sample);
    const auto step = Tick(sync, 30 * kMs, {Pot(0x100, 0x11, 1)}, 2);
    REQUIRE(step.writes.size() == 1);
    CHECK(step.writes[0].address == 0x100);
    CHECK(step.writes[0].life == 0);
}

namespace {

Seen At(Seen s, float x, float z) {
    s.position = {x, 2.0f, z};
    return s;
}

} // namespace

TEST_CASE("world: a rock lifted, thrown and gone near our Link is broken, with no life 0 read") {
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 2.0f, 0.0f});
    Tick(sync, 0, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 1.0f, 1.0f)});
    const auto id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 0, 0});
    Tick(sync, 10 * kMs, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 1.0f, 1.0f)});
    // Lifted (it moves with Link), thrown 3 m ahead, then removed.
    Tick(sync, 20 * kMs, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 0.2f, 0.3f)});
    Tick(sync, 30 * kMs, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 3.0f, 0.5f)});
    Tick(sync, 35 * kMs, {});  // missed once
    const auto last = All<proto::ObjectSample>(Tick(sync, 40 * kMs, {}));
    REQUIRE(last.size() == 1);
    REQUIRE(last[0].state.life);
    CHECK(last[0].state.life->life == 0);
}

TEST_CASE("world: a copy thrown and gone near our Link sends its whole life as DAMAGE") {
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 2.0f, 0.0f});
    Tick(sync, 0, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 1.0f, 1.0f)}, 2);
    const auto id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 4, 0});
    Tick(sync, 10 * kMs, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), 3.0f, 0.5f)}, 2);
    Tick(sync, 15 * kMs, {}, 2);  // missed once
    const auto damage = All<proto::Damage>(Tick(sync, 20 * kMs, {}, 2));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].id == id);
    CHECK(damage[0].epoch == 4);
    CHECK(damage[0].amount == 1);
}

TEST_CASE("world: a breakable gone unmoved, far away, or with no Link seen is not broken") {
    for (const int which : {0, 1, 2}) {
        CAPTURE(which);
        Sync sync;
        if (which != 2) {
            sync.SetPlayer(proto::Vec3{which == 1 ? 50.0f : 0.0f, 2.0f, 0.0f});
        }
        Tick(sync, 0, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), which == 0 ? 4.0f : 1.0f, 1.0f)});
        sync.Receive(proto::ObjectOwner{*sync.IdOf(0x100), 1, 0, 0});
        // 0: under 0.5 m, unmoved, and 4 m from Link (beyond sword reach).
        const float moved = which == 0 ? 4.2f : 8.0f;
        Tick(sync, 10 * kMs, {At(Pot(0x100, 0x11, 1, "Obj_LiftRock_A_03"), moved, 1.0f)});
        Tick(sync, 15 * kMs, {});
        const auto step = Tick(sync, 20 * kMs, {});
        CHECK(All<proto::ObjectSample>(step).empty());
        CHECK(All<proto::Damage>(step).empty());
    }
}

TEST_CASE("world: a crate whose life cannot be read any more has died") {
    Sync sync;
    Seen crate = Pot(0x100, 0x11, 1, "Obj_BrokenBox_A_01");
    Tick(sync, 0, {crate});
    const auto id = *sync.IdOf(0x100);
    sync.Receive(proto::ObjectOwner{id, 1, 0, 0});
    Tick(sync, 10 * kMs, {crate});
    crate.life.reset();  // still in the list, its life object gone
    const auto samples = All<proto::ObjectSample>(Tick(sync, 100 * kMs, {crate}));
    REQUIRE(samples.size() == 1);
    REQUIRE(samples[0].state.life);
    CHECK(samples[0].state.life->life == 0);

    // On a copy the same is our hit, sent to the owner.
    Sync copy;
    Seen c2 = Pot(0x200, 0x11, 3, "Obj_BrokenBox_A_01");
    Tick(copy, 0, {c2}, 2);
    copy.Receive(proto::ObjectOwner{*copy.IdOf(0x200), 1, 0, 0});
    Tick(copy, 10 * kMs, {c2}, 2);
    c2.life.reset();
    Tick(copy, 20 * kMs, {c2}, 2);
    const auto damage = All<proto::Damage>(Tick(copy, 200 * kMs, {c2}, 2));
    REQUIRE(damage.size() == 1);
    CHECK(damage[0].amount == 3);
}

TEST_CASE("world: a crate gone unmoved within sword reach of our Link was broken") {
    Sync sync;
    sync.SetPlayer(proto::Vec3{0.0f, 2.0f, 0.0f});
    const Seen crate = At(Pot(0x100, 0x11, 1, "Obj_BrokenBox_A_01"), 1.3f, 0.2f);
    Tick(sync, 0, {crate});
    sync.Receive(proto::ObjectOwner{*sync.IdOf(0x100), 1, 0, 0});
    Tick(sync, 10 * kMs, {crate});
    Tick(sync, 15 * kMs, {});
    const auto last = All<proto::ObjectSample>(Tick(sync, 20 * kMs, {}));
    REQUIRE(last.size() == 1);
    REQUIRE(last[0].state.life);
    CHECK(last[0].state.life->life == 0);
}

TEST_CASE("world: the foliage probe logs the cut buffer once, then only what changed") {
    const std::uint64_t main = 0x8000000, module = 0x2100000000, holder = 0x2100001000,
                        scene = 0x2100002000, buffer = 0x2100003000;
    std::map<std::uint64_t, std::uint64_t> memory;
    const auto fill = [&](std::uint64_t at, std::size_t words) {
        for (std::size_t i = 0; i < words; ++i) {
            memory.try_emplace(at + 8 * i, 0);
        }
    };
    fill(module, 64);
    fill(holder, 128);
    fill(scene, 128);
    fill(buffer, 64);
    memory[main + 0x4e22540] = module;
    memory[module + 0xf0] = holder;
    memory[holder + 0x338] = scene;
    memory[scene + 0x58] = buffer;
    const CoopWorld::FoliageProbe::ReadWord read =
        [&](std::uint64_t at) -> std::optional<std::uint64_t> {
        const auto it = memory.find(at);
        return it == memory.end() ? std::nullopt : std::optional<std::uint64_t>(it->second);
    };
    CoopWorld::FoliageProbe probe;
    const std::optional<std::array<float, 3>> link{{1.0f, 2.0f, 3.0f}};
    std::vector<std::string> lines;
    const auto look = [&] {
        lines.clear();
        for (int i = 0; i < 4; ++i) {  // it looks on every fourth call
            for (auto& line : probe.Look(read, main, link)) {
                lines.push_back(std::move(line));
            }
        }
    };
    look();
    REQUIRE(lines.size() == 3);  // the path, the scene header, the buffer
    CHECK(lines[0].find("scene 0x2100002000") != std::string::npos);
    CHECK(lines[2].find("cut+0x58 at 0x2100003000") != std::string::npos);
    look();
    CHECK(lines.empty());
    memory[buffer + 0x10] = 0x3f800000;
    look();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "foliage: cut+0x58 changed +0x10:0>3f800000 | Link 1.00 2.00 3.00");
    // A word that changes on every look is a counter: six lines, one "noisy", then quiet.
    for (int i = 1; i <= 9; ++i) {
        memory[buffer + 0x18] = i;
        look();
        if (i <= 6) {
            REQUIRE(lines.size() == 1);
            CHECK(lines[0].find("+0x18:") != std::string::npos);
        } else if (i == 7) {
            REQUIRE(lines.size() == 1);
            CHECK(lines[0].find("+0x18:noisy") != std::string::npos);
        } else {
            CHECK(lines.empty());
        }
    }
    memory[buffer + 0x20] = 5;  // a quiet word next to it still shows
    look();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("+0x20:0>5") != std::string::npos);
    memory[module + 0x30] = 1;  // the module says off: no scene, as the game reads it
    look();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("scene 0 (off)") != std::string::npos);
    memory[module + 0x30] = 0;
    look();
    REQUIRE(lines.size() == 3);  // back: the path and both blocks again
    memory[main + 0x4e22540] = 0;  // scene gone
    look();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("scene 0") != std::string::npos);
}

namespace {

// A game's memory as bytes, for the grass cut queue and the cave calls.
struct FakeGame : CoopPuppet::Guest {
    std::map<std::uint64_t, std::uint8_t> bytes;
    void Put(std::uint64_t at, const void* in, std::size_t n) {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < n; ++i) {
            bytes[at + i] = p[i];
        }
    }
    void Word(std::uint64_t at, std::uint64_t v) { Put(at, &v, 8); }
    bool Read(std::uint64_t at, void* out, std::size_t n) override {
        auto* p = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = bytes.find(at + i);
            if (it == bytes.end()) {
                return false;
            }
            p[i] = it->second;
        }
        return true;
    }
    bool Write(std::uint64_t at, const void* in, std::size_t n) override {
        Put(at, in, n);
        return true;
    }
    CoopWorld::GrassSync::ReadWord Reader() {
        return [this](std::uint64_t at) -> std::optional<std::uint64_t> {
            std::uint64_t v = 0;
            return Read(at, &v, 8) ? std::optional<std::uint64_t>(v) : std::nullopt;
        };
    }
};

constexpr std::uint64_t kMain = 0x8000000, kModule = 0x2100000000, kHolder = 0x2100001000,
                        kFolScene = 0x2100002000, kEntries = 0x2100003000, kSlots = 0x2100004000;
constexpr std::uint64_t kCap = 16;

// The foliage scene with an empty queue of 16 (as seen live, 2026-11-09).
void Setup(FakeGame& g) {
    g.Word(kMain + CoopWorld::kFoliageModule, kModule);
    g.Word(kModule + 0x30, 0xb8);  // bit 0 clear: on
    g.Word(kModule + 0xf0, kHolder);
    g.Word(kHolder + 0x338, kFolScene);
    g.Word(kFolScene + 0x20, kCap);
    g.Word(kFolScene + 0x28, kEntries);
    g.Word(kFolScene + 0x38, 0);
    for (std::uint64_t i = 0; i < kCap; ++i) {
        g.Word(kEntries + 16 * i, 0);
        g.Word(kEntries + 16 * i + 8, i);
    }
    const std::vector<std::uint8_t> zero(0x3c * kCap + 8, 0);
    g.Put(kSlots, zero.data(), zero.size());
}

// The game queues a cut, as 0xb808f0 does: slot pointer, then sequence i + 1.
void GameCuts(FakeGame& g, std::uint64_t slot_index, const CoopWorld::GrassSync::Slot& slot) {
    std::uint64_t i = 0;
    g.Read(kFolScene + 0x38, &i, 8);
    const std::uint64_t at = kSlots + 0x3c * slot_index;  // 4-aligned, as the game's
    g.Put(at, slot.data(), slot.size());
    g.Word(kEntries + 16 * (i % kCap), at);
    g.Word(kEntries + 16 * (i % kCap) + 8, i + 1);
    g.Word(kFolScene + 0x38, i + 1);
}

proto::GrassCut Sword() {
    proto::GrassCut c;
    c.scene = "HyruleField_Child";
    c.position = {189.57f, 157.52f, 24.04f};
    c.radius = 0.525f;
    c.flags = 0x0c;
    c.sweep = {proto::Vec3{189.0f, 157.9f, 24.5f}, proto::Vec3{190.1f, 157.6f, 23.8f},
               proto::Vec3{190.5f, 157.7f, 23.1f}};
    return c;
}

} // namespace

TEST_CASE("grass: a cut slot and its message carry the same cut") {
    const auto slot = CoopWorld::GrassSync::ToSlot(Sword());
    float f = 0.0f;
    std::memcpy(&f, slot.data() + 0x10, 4);
    CHECK(f == doctest::Approx(0.3f));  // the game's own value
    CHECK(slot[0x14] == 0x0c);
    CHECK(CoopWorld::GrassSync::ToMessage(slot, "HyruleField_Child") == Sword());
}

TEST_CASE("grass: our own new cuts are sent once, from the second look on") {
    FakeGame g;
    Setup(g);
    CoopWorld::GrassSync grass;
    const auto slot = CoopWorld::GrassSync::ToSlot(Sword());
    GameCuts(g, 0, slot);  // before we looked: not ours to send
    CHECK(grass.Look(g.Reader(), kMain, "HyruleField_Child", 1000).empty());
    proto::GrassCut second = Sword();
    second.position[0] = 184.0f;
    GameCuts(g, 1, CoopWorld::GrassSync::ToSlot(second));
    // taken out by the game already: sequence i + capacity
    g.Word(kEntries + 16 * 1 + 8, 1 + kCap);
    proto::GrassCut third = Sword();
    third.position[0] = 178.0f;
    GameCuts(g, 2, CoopWorld::GrassSync::ToSlot(third));
    const auto sent = grass.Look(g.Reader(), kMain, "HyruleField_Child", 1100);
    REQUIRE(sent.size() == 2);
    CHECK(sent[0] == second);
    CHECK(sent[1] == third);
    CHECK(grass.Look(g.Reader(), kMain, "HyruleField_Child", 1200).empty());
    // an entry whose sequence is neither i + 1 nor i + capacity is skipped
    GameCuts(g, 3, slot);
    g.Word(kEntries + 16 * 3 + 8, 99);
    CHECK(grass.Look(g.Reader(), kMain, "HyruleField_Child", 1300).empty());
    // the module says off: nothing, and the queue is learnt again after
    g.Word(kModule + 0x30, 0xb9);
    CHECK(grass.Look(g.Reader(), kMain, "HyruleField_Child", 1400).empty());
}

TEST_CASE("grass: another game's cut is made through the game's call, and not sent back") {
    FakeGame g;
    Setup(g);
    CoopWorld::GrassSync grass;
    CHECK(grass.Look(g.Reader(), kMain, "HyruleField_Child", 1000).empty());
    CHECK(grass.Look(g.Reader(), kMain, "HyruleField_Child", 3000).empty());  // settled
    proto::GrassCut theirs = Sword();
    theirs.player_number = 2;
    proto::GrassCut elsewhere = theirs;
    elsewhere.scene = "KakarikoVillage_Child";
    grass.Receive(elsewhere);  // not our scene: dropped
    grass.Receive(theirs);
    auto call = grass.Step(g, false, 0, 3010);
    REQUIRE(call);
    CHECK(call->function == kMain + 0x14c6c90);  // malloc, once
    CHECK(call->x[0] == 0x40);
    const std::uint64_t block = 0x2100009000;
    call = grass.Step(g, true, block, 3010);
    REQUIRE(call);
    CHECK(call->function == kMain + CoopWorld::kCutFoliage);
    CHECK(call->x[0] == kModule);
    CHECK(call->x[1] == block);
    CoopWorld::GrassSync::Slot written{};
    REQUIRE(g.Read(block, written.data(), written.size()));
    CHECK(written == CoopWorld::GrassSync::ToSlot(Sword()));
    CHECK_FALSE(grass.Step(g, true, 1, 3010));  // nothing more to make
    CHECK(grass.Made() == 1);
    // the game queues the cut we asked for: it is not sent back
    GameCuts(g, 0, written);
    CHECK(grass.Look(g.Reader(), kMain, "HyruleField_Child", 3100).empty());
    // our own cut at the same place later is sent
    GameCuts(g, 1, written);
    CHECK(grass.Look(g.Reader(), kMain, "HyruleField_Child", 3200).size() == 1);
}

TEST_CASE("grass: at most eight cuts a frame, the rest the next frame") {
    FakeGame g;
    Setup(g);
    CoopWorld::GrassSync grass;
    (void)grass.Look(g.Reader(), kMain, "HyruleField_Child", 1000);
    (void)grass.Look(g.Reader(), kMain, "HyruleField_Child", 3000);
    for (int i = 0; i < 10; ++i) {
        proto::GrassCut c = Sword();
        c.position[2] += static_cast<float>(i);
        grass.Receive(c);
    }
    int calls = 0;
    auto call = grass.Step(g, false, 0, 1000);  // malloc
    for (call = grass.Step(g, true, 0x2100009000, 1000); call; call = grass.Step(g, true, 1, 1000)) {
        ++calls;
    }
    CHECK(calls == 8);
    calls = 0;
    for (call = grass.Step(g, false, 0, 1016); call; call = grass.Step(g, true, 1, 1016)) {
        ++calls;
    }
    CHECK(calls == 2);
    CHECK(grass.Made() == 10);
}

TEST_CASE("grass: cuts that come before the scene settles are made once it has") {
    FakeGame g;
    Setup(g);
    CoopWorld::GrassSync grass;
    // The server's replay can come before our walk saw the new scene.
    proto::GrassCut theirs = Sword();
    theirs.player_number = 2;
    proto::GrassCut old = theirs;
    old.scene = "KakarikoVillage_Child";
    grass.Receive(theirs);
    grass.Receive(old);
    CHECK_FALSE(grass.Step(g, false, 0, 900));  // no scene yet: nothing made
    (void)grass.Look(g.Reader(), kMain, "HyruleField_Child", 1000);
    CHECK_FALSE(grass.Step(g, false, 0, 1500));  // not settled yet
    (void)grass.Look(g.Reader(), kMain, "HyruleField_Child", 3000);
    auto call = grass.Step(g, false, 0, 3000);  // malloc
    REQUIRE(call);
    call = grass.Step(g, true, 0x2100009000, 3000);
    REQUIRE(call);
    CHECK(call->function == kMain + CoopWorld::kCutFoliage);
    CHECK_FALSE(grass.Step(g, true, 1, 3000));  // Kakariko's was dropped
    CHECK(grass.Made() == 1);
    // the foliage scene changes under the same name: wait again
    g.Word(kHolder + 0x338, 0);
    (void)grass.Look(g.Reader(), kMain, "HyruleField_Child", 3100);
    g.Word(kHolder + 0x338, kFolScene);
    (void)grass.Look(g.Reader(), kMain, "HyruleField_Child", 3200);
    grass.Receive(theirs);
    CHECK_FALSE(grass.Step(g, false, 0, 3300));
    (void)grass.Look(g.Reader(), kMain, "HyruleField_Child", 5300);
    CHECK(grass.Step(g, false, 0, 5300));
}
