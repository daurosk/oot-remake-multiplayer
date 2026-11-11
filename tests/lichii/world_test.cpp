// Tests for the shared world objects (lichii/hook/coop_world.cpp) and the
// break that is read just before an object leaves the list
// (coop_objects_sync.cpp, Flush). No emulator code.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
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

// ------------------------------------------------------------ WorldSync

namespace {

using CoopWorld::WorldSync;

Seen Thing(std::uint64_t address, std::uint64_t hash, std::optional<std::int32_t> life,
           std::string name, proto::Vec3 at = {1.0f, 2.0f, 3.0f}) {
    Seen s;
    s.address = address;
    s.name = std::move(name);
    s.hash = hash;
    if (life) {
        s.life = proto::ObjectLife{*life, 1};
    }
    s.position = at;
    return s;
}

const proto::Vec3 kLink{1.0f, 2.0f, 4.0f};  // one metre from Thing's default place

WorldSync::Out Walk(WorldSync& w, std::uint64_t now, const std::vector<Seen>& seen,
                    std::optional<proto::Vec3> player = kLink, std::uint8_t self = 2,
                    const std::string& scene = kScene) {
    WorldSync::Out out;
    w.Tick(scene, self, now, seen, player, out);
    return out;
}

template <typename T>
std::vector<T> Sent(const WorldSync::Out& out) {
    std::vector<T> got;
    for (const auto& m : out.send) {
        if (const auto* t = std::get_if<T>(&m)) {
            got.push_back(*t);
        }
    }
    return got;
}

proto::WorldBreak BreakOf(std::uint64_t hash, std::uint8_t flags = 0,
                          const std::string& scene = kScene) {
    proto::WorldBreak b;
    b.player_number = 1;
    b.scene = scene;
    b.hash = hash;
    b.flags = flags;
    return b;
}

proto::WorldMove MoveOf(std::uint64_t hash, std::uint64_t time, float x, std::uint8_t player = 1) {
    proto::WorldMove v;
    v.player_number = player;
    v.scene = kScene;
    v.hash = hash;
    v.time_us = time;
    v.position = {x, 2.0f, 3.0f};
    return v;
}

proto::WorldPlace PlaceOf(std::uint64_t hash, float x) {
    proto::WorldPlace p;
    p.player_number = 1;
    p.scene = kScene;
    p.hash = hash;
    p.position = {x, 2.0f, 3.0f};
    return p;
}

const char* const kBox = "Obj_ForestTemple_MovableBox_A_01";
const char* const kTuft = "Land_kokiri_Plant_A_02_DropFixed_03";

} // namespace

// ------------------------------------------------- locked doors (docs/dungeon.md)

namespace {

const char* const kLock = "Obj_DoorChainLock_A_01";
const std::string kTemple = "WaterTemple";
const proto::Vec3 kFar{1.0f, 2.0f, 10.0f};  // seven metres from Thing's default place

std::vector<Seen> LockAt(std::uint64_t address = 0x100) { return {Thing(address, 0x11, std::nullopt, kLock)}; }

WorldSync::Out TempleWalk(WorldSync& w, std::uint64_t ms, std::vector<Seen> seen, proto::Vec3 link = kLink,
                          std::uint8_t self = 2) {
    return Walk(w, ms * kMs, seen, link, self, kTemple);
}

} // namespace

TEST_CASE("doors: a small-key lock is a kept breakable, a shop's night lock is not") {
    CHECK(CoopWorld::IsLock("Obj_DoorChainLock_A_01"));
    CHECK(CoopWorld::IsLock("Obj_DoorChainLock_A_04"));
    CHECK(CoopWorld::IsLock("Obj_SpiritTemple_DoorChainLock_A_01"));
    CHECK(CoopWorld::IsBreakable("Obj_DoorChainLock_A_02"));
    CHECK_FALSE(CoopWorld::IsLock("Obj_HrlCity_Shop_DoorChainLock_A_01"));
    CHECK_FALSE(CoopWorld::IsLock("Dm_Obj_DoorChainLock_A_01"));
    CHECK(CoopWorld::SmallKeyFlag("WaterTemple") == 0x877bdceeu);  // DungeonInfo.WaterTemple.SmallKeyNum
    CHECK(CoopWorld::SmallKeyFlag("InsideTheDekuTree") == 0x0914ba07u);
    CHECK(CoopWorld::SmallKeyFlag("KakarikoVillage") == 0u);
}

TEST_CASE("doors: our lock opened next to Link goes out as a kept break") {
    WorldSync w;
    TempleWalk(w, 1000, LockAt());
    TempleWalk(w, 5000, LockAt());
    TempleWalk(w, 5100, {});
    TempleWalk(w, 5200, {});
    const auto out = TempleWalk(w, 6000, {});
    const auto sent = Sent<proto::WorldBreak>(out);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].hash == 0x11);
    CHECK(sent[0].flags == proto::world_break::kept);
    CHECK(out.key_refunds.empty());
}

TEST_CASE("doors: another player's lock goes here when our Link is away, and stays gone on a reload") {
    WorldSync w;
    TempleWalk(w, 1000, LockAt(), kFar);
    TempleWalk(w, 5000, LockAt(), kFar);
    w.Receive(BreakOf(0x11, proto::world_break::kept, kTemple));
    TempleWalk(w, 5100, LockAt(), kFar);
    CHECK(w.Removes().empty());
    TempleWalk(w, 5600, LockAt(), kFar);
    CHECK(w.Removes() == std::vector<std::uint64_t>{0x100});
    // Deleted: it leaves the walk, no key back.
    TempleWalk(w, 5700, {}, kFar);
    auto out = TempleWalk(w, 5800, {}, kFar);
    CHECK(out.key_refunds.empty());
    // Its room loads again later: the lock is back in this game, never "restored".
    out = TempleWalk(w, 30000, LockAt(0x300), kFar);
    CHECK(Sent<proto::WorldBreak>(out).empty());
    CHECK(w.Removes() == std::vector<std::uint64_t>{0x300});
}

TEST_CASE("doors: a lock at our Link's feet is not taken, and opening it then gives our key back") {
    WorldSync w;
    TempleWalk(w, 1000, LockAt());
    TempleWalk(w, 5000, LockAt());
    w.Receive(BreakOf(0x11, proto::world_break::kept, kTemple));
    for (std::uint64_t t = 5100; t < 9000; t += 100) {
        TempleWalk(w, t, LockAt());
        CHECK(w.Removes().empty());
    }
    // Our player opens it: our game spends a key, then takes the lock.
    w.OwnCountDrop(0x877bdceeu, 9000 * kMs);
    std::vector<std::uint32_t> back;
    for (std::uint64_t t = 9100; t < 12000; t += 100) {
        auto out = TempleWalk(w, t, {});
        CHECK(Sent<proto::WorldBreak>(out).empty());  // the scene has this break already
        if (t < 9900) {
            CHECK(out.key_refunds.empty());  // decided after the gone wait (0.7 s)
        }
        back.insert(back.end(), out.key_refunds.begin(), out.key_refunds.end());
    }
    CHECK(back == std::vector<std::uint32_t>{0x877bdceeu});  // once
}

TEST_CASE("doors: a held lock that goes with no key of ours spent, or with the area, gives nothing back") {
    {
        // Gone next to our Link, but our count never dropped (we warped, died, or
        // only another's spend came in).
        WorldSync w;
        TempleWalk(w, 1000, LockAt());
        TempleWalk(w, 5000, LockAt());
        w.Receive(BreakOf(0x11, proto::world_break::kept, kTemple));
        TempleWalk(w, 5100, LockAt());
        for (std::uint64_t t = 5200; t < 8000; t += 100) {
            CHECK(TempleWalk(w, t, {}).key_refunds.empty());
        }
    }
    {
        // The room unloads as we leave by a door beside it: four far things go too.
        WorldSync w;
        const char* pot = "Obj_Pot_A_01";
        std::vector<Seen> room = LockAt();
        for (std::uint64_t i = 0; i < 4; ++i) {
            room.push_back(Thing(0x200 + i, 0x21 + i, 1, pot, {1.0f, 2.0f, 30.0f + i}));
        }
        TempleWalk(w, 1000, room);
        TempleWalk(w, 5000, room);
        w.Receive(BreakOf(0x11, proto::world_break::kept, kTemple));
        TempleWalk(w, 5100, room);
        w.OwnCountDrop(0x877bdceeu, 5100 * kMs);  // a key we spent on another door
        for (std::uint64_t t = 5200; t < 8000; t += 100) {
            CHECK(TempleWalk(w, t, {}).key_refunds.empty());
        }
    }
}

TEST_CASE("doors: our own lock's key cannot also pay for a held lock beside it") {
    // B holds lock Y (another player opened it) and opens its own lock X next to it.
    WorldSync w;
    const auto x = Thing(0x100, 0x11, std::nullopt, kLock);
    const auto y = Thing(0x200, 0x12, std::nullopt, kLock, {1.0f, 2.0f, 4.5f});
    TempleWalk(w, 1000, {x, y});
    TempleWalk(w, 5000, {x, y});
    w.Receive(BreakOf(0x12, proto::world_break::kept, kTemple));
    TempleWalk(w, 5100, {x, y});
    w.OwnCountDrop(0x877bdceeu, 6000 * kMs);  // the key spent on X
    std::size_t sent = 0;
    std::vector<std::uint32_t> back;
    for (std::uint64_t t = 6100; t < 8000; t += 100) {
        auto out = TempleWalk(w, t, {y});
        sent += Sent<proto::WorldBreak>(out).size();
        back.insert(back.end(), out.key_refunds.begin(), out.key_refunds.end());
    }
    CHECK(sent == 1);  // X went out, kept
    // Y now leaves next to our Link (B leaves by a door): no key of ours is left for it.
    for (std::uint64_t t = 8000; t < 11000; t += 100) {
        auto out = TempleWalk(w, t, {});
        back.insert(back.end(), out.key_refunds.begin(), out.key_refunds.end());
    }
    CHECK(back.empty());
}

TEST_CASE("doors: a host save load forgets the kept locks, which come back with their rooms") {
    WorldSync w;
    TempleWalk(w, 1000, LockAt(), kFar);
    TempleWalk(w, 5000, LockAt(), kFar);
    w.Receive(BreakOf(0x11, proto::world_break::kept, kTemple));
    TempleWalk(w, 5100, LockAt(), kFar);
    TempleWalk(w, 5600, LockAt(), kFar);
    CHECK(w.Removes() == std::vector<std::uint64_t>{0x100});
    w.ForgetKept();
    CHECK_FALSE(w.Broken(0x11));
    CHECK(w.Removes().empty());
    TempleWalk(w, 5700, {}, kFar);
    TempleWalk(w, 5800, {}, kFar);
    TempleWalk(w, 30000, LockAt(0x300), kFar);
    CHECK(w.Removes().empty());
}

TEST_CASE("doors: a lock at our feet that we walk away from is deleted, no key back") {
    WorldSync w;
    TempleWalk(w, 1000, LockAt());
    TempleWalk(w, 5000, LockAt());
    w.Receive(BreakOf(0x11, proto::world_break::kept, kTemple));
    TempleWalk(w, 5100, LockAt());
    TempleWalk(w, 6000, LockAt(), kFar);
    TempleWalk(w, 6500, LockAt(), kFar);
    CHECK(w.Removes() == std::vector<std::uint64_t>{0x100});
    TempleWalk(w, 6600, {}, kFar);
    CHECK(TempleWalk(w, 6700, {}, kFar).key_refunds.empty());
}

TEST_CASE("doors: both opened the lock at once: the higher player number gives its key back") {
    for (const std::uint8_t self : {std::uint8_t{1}, std::uint8_t{3}}) {
        WorldSync w;
        TempleWalk(w, 1000, LockAt(), kLink, self);
        TempleWalk(w, 5000, LockAt(), kLink, self);
        TempleWalk(w, 5100, {}, kLink, self);
        TempleWalk(w, 5200, {}, kLink, self);
        REQUIRE(Sent<proto::WorldBreak>(TempleWalk(w, 6000, {}, kLink, self)).size() == 1);
        w.OwnCountDrop(0x877bdceeu, 5000 * kMs);  // our player spent the key on it
        auto other = BreakOf(0x11, proto::world_break::kept, kTemple);
        other.player_number = 2;
        w.Receive(other);
        const auto out = TempleWalk(w, 6100, {}, kLink, self);
        if (self == 3) {
            CHECK(out.key_refunds == std::vector<std::uint32_t>{0x877bdceeu});
        } else {
            CHECK(out.key_refunds.empty());
        }
        w.Receive(other);  // told twice: still one key
        CHECK(TempleWalk(w, 6200, {}, kLink, self).key_refunds.empty());
    }
}

TEST_CASE("world: movables, grass actors and the new breakables by name") {
    for (const char* name : {"Obj_MovableBox_A_01", "Obj_ForestTemple_MovableBox_A_03",
                             "Obj_LonLonRanch_MovableBox_A_01", "Obj_SliderBox_A_01",
                             "Obj_AmosStatue_A_01", "Obj_Kokiri_RollingRock_A_01", "Obj_RollingRock_A_05",
                             "Obj_Firetemple_RollingRock_A_01"}) {
        CAPTURE(name);
        CHECK(CoopWorld::IsMovable(name));
        CHECK_FALSE(CoopWorld::IsBreakable(name));
    }
    for (const char* name : {"Obj_MovableBox_BasePosAnchor", "Obj_Pot_A_01", "Obj_Deku_MoveFloor_A_01",
                             "Enemy_MovableBox_A_01", "Land_Kokiri_RollingRockArea_A_01",
                             "FldObj_AirWall_RollingRock", "RollingRockSpawnActor_DeathMountainTrail"}) {
        CAPTURE(name);
        CHECK_FALSE(CoopWorld::IsMovable(name));
    }
    CHECK(CoopWorld::IsRoller("Obj_Kokiri_RollingRock_A_01"));
    CHECK_FALSE(CoopWorld::IsRoller("Obj_MovableBox_A_01"));
    CHECK_FALSE(CoopWorld::IsPushBlock("Obj_Kokiri_RollingRock_A_01"));
    CHECK(CoopWorld::IsBreakable("Obj_Zora_GuidePost_A_01"));
    CHECK(CoopWorld::IsBreakable("Obj_RedIce_A_01"));
    CHECK(CoopWorld::IsGrassActor(kTuft));
    CHECK(CoopWorld::IsGrassActor("Obj_Plant_Grass_A_01"));
    CHECK_FALSE(CoopWorld::IsGrassActor("Obj_Pot_A_01"));
}

TEST_CASE("world: a pot broken here is sent once and left to the game") {
    WorldSync w;
    CHECK(Walk(w, 1000 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01")}).send.empty());
    const auto out = Walk(w, 1100 * kMs, {Thing(0x100, 0x11, 0, "Obj_Pot_A_01")});
    const auto sent = Sent<proto::WorldBreak>(out);
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].hash == 0x11);
    CHECK(sent[0].scene == kScene);
    CHECK(sent[0].flags == 0);
    CHECK(sent[0].player_number == 0);
    CHECK(w.Broken(0x11));
    // Its pieces lie there for ten seconds: nothing more is sent, nothing deleted.
    for (std::uint64_t t = 1200; t < 12000; t += 100) {
        const auto later = Walk(w, t * kMs, {Thing(0x100, 0x11, 0, "Obj_Pot_A_01")});
        CHECK(later.send.empty());
        CHECK(later.lives.empty());
    }
    CHECK(w.Removes().empty());
    // No placement hash: not ours to share.
    WorldSync other;
    Walk(other, 1000 * kMs, {Thing(0x100, 0, 1, "Obj_Pot_A_01")});
    CHECK(Walk(other, 1100 * kMs, {Thing(0x100, 0, 0, "Obj_Pot_A_01")}).send.empty());
    CHECK(other.Count() == 0);
}

TEST_CASE("world: a crate whose life can no longer be read was broken") {
    WorldSync w;
    Walk(w, 1000 * kMs, {Thing(0x100, 0x11, 5, "Obj_BrokenBox_A_01")});
    const auto out = Walk(w, 1100 * kMs, {Thing(0x100, 0x11, std::nullopt, "Obj_BrokenBox_A_01")});
    CHECK(Sent<proto::WorldBreak>(out).size() == 1);
}

TEST_CASE("world: another player's break takes our copy's life, then the copy if it stays") {
    WorldSync w;
    Walk(w, 1000 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01"), Thing(0x200, 0x22, 5, "Obj_BrokenBox_A_01")});
    Walk(w, 5000 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01"), Thing(0x200, 0x22, 5, "Obj_BrokenBox_A_01")});
    w.Receive(BreakOf(0x11));
    w.Receive(BreakOf(0x22));
    auto out = Walk(w, 5100 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01"), Thing(0x200, 0x22, 5, "Obj_BrokenBox_A_01")});
    REQUIRE(out.lives.size() == 2);
    CHECK(out.lives[0].address == 0x100);
    CHECK(out.lives[0].life == 0);
    CHECK(out.lives[0].hurt == 0);
    CHECK(out.send.empty());  // not our break: nothing goes back
    // The pot takes it (life 0, pieces); the crate's life came back.
    for (std::uint64_t t = 5200; t < 11000; t += 100) {
        out = Walk(w, t * kMs, {Thing(0x100, 0x11, 0, "Obj_Pot_A_01"), Thing(0x200, 0x22, 5, "Obj_BrokenBox_A_01")});
        CHECK(out.send.empty());
        CHECK(out.lives.empty());
        CHECK(w.Removes().empty());
    }
    Walk(w, 11100 * kMs, {Thing(0x100, 0x11, 0, "Obj_Pot_A_01"), Thing(0x200, 0x22, 5, "Obj_BrokenBox_A_01")});
    CHECK(w.Removes() == std::vector<std::uint64_t>{0x200});
}

TEST_CASE("world: a grass tuft cut by another player is deleted when life 0 leaves it standing") {
    WorldSync w;
    Walk(w, 1000 * kMs, {Thing(0x100, 0x11, 1, kTuft)});
    Walk(w, 5000 * kMs, {Thing(0x100, 0x11, 1, kTuft)});
    w.Receive(BreakOf(0x11));
    CHECK(Walk(w, 5100 * kMs, {Thing(0x100, 0x11, 1, kTuft)}).lives.size() == 1);
    Walk(w, 5300 * kMs, {Thing(0x100, 0x11, 0, kTuft)});
    CHECK(w.Removes().empty());
    Walk(w, 5500 * kMs, {Thing(0x100, 0x11, 0, kTuft)});
    CHECK(w.Removes() == std::vector<std::uint64_t>{0x100});
}

TEST_CASE("world: life 0 read just before the other player's break is their break") {
    // The enemy sharing wrote life 0 into our tuft first; WORLD_BREAK came a walk later.
    WorldSync w;
    Walk(w, 1000 * kMs, {Thing(0x100, 0x11, 1, kTuft)});
    Walk(w, 5000 * kMs, {Thing(0x100, 0x11, 1, kTuft)});
    CHECK(Sent<proto::WorldBreak>(Walk(w, 5100 * kMs, {Thing(0x100, 0x11, 0, kTuft)})).size() == 1);
    w.Receive(BreakOf(0x11));
    Walk(w, 5200 * kMs, {Thing(0x100, 0x11, 0, kTuft)});
    Walk(w, 5700 * kMs, {Thing(0x100, 0x11, 0, kTuft)});
    CHECK(w.Removes() == std::vector<std::uint64_t>{0x100});
}

TEST_CASE("world: a copy first seen after the break is deleted quietly") {
    WorldSync w;
    Walk(w, 1000 * kMs, {});
    w.Receive(BreakOf(0x11));
    Walk(w, 2000 * kMs, {});
    // The area loads a minute later, the pot whole in this game.
    const auto out = Walk(w, 60000 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01")});
    CHECK(out.lives.empty());
    CHECK(out.send.empty());
    CHECK(w.Removes() == std::vector<std::uint64_t>{0x100});
    // The delete takes it; it does not count as put back when the area loads again.
    Walk(w, 60100 * kMs, {});
    Walk(w, 60200 * kMs, {});
    const auto again = Walk(w, 70000 * kMs, {Thing(0x300, 0x11, 1, "Obj_Pot_A_01")});
    CHECK(Sent<proto::WorldBreak>(again).size() == 1);
    CHECK(Sent<proto::WorldBreak>(again)[0].flags == proto::world_break::restored);
}

TEST_CASE("world: the server's replay for the scene we are entering is kept") {
    WorldSync w;
    Walk(w, 1000 * kMs, {}, kLink, 2, "Market");
    w.Receive(BreakOf(0x11));              // for kScene, while our walk still says Market
    w.Receive(BreakOf(0x33, 0, "Graveyard"));
    Walk(w, 1100 * kMs, {}, kLink, 2, "Market");
    CHECK_FALSE(w.Broken(0x11));
    Walk(w, 3000 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01")});
    CHECK(w.Broken(0x11));
    CHECK_FALSE(w.Broken(0x33));
    CHECK(w.Removes() == std::vector<std::uint64_t>{0x100});
    // A new scene forgets the old one's breaks.
    Walk(w, 9000 * kMs, {}, kLink, 2, "Market");
    CHECK_FALSE(w.Broken(0x11));
}

TEST_CASE("world: a rock thrown and gone next to Link was broken, an unloaded one was not") {
    const char* rock = "Obj_LiftRock_A_01";
    WorldSync w;
    Walk(w, 1000 * kMs, {Thing(0x100, 0x11, 1, rock), Thing(0x200, 0x22, 1, rock, {1.0f, 2.0f, 30.0f})});
    // Carried five metres, then gone (two walks), six metres from Link.
    Walk(w, 1100 * kMs, {Thing(0x100, 0x11, 1, rock, {1.0f, 2.0f, 10.0f}), Thing(0x200, 0x22, 1, rock, {1.0f, 2.0f, 30.0f})});
    std::vector<Seen> rest{Thing(0x200, 0x22, 1, rock, {1.0f, 2.0f, 30.0f})};
    CHECK(Walk(w, 1200 * kMs, rest).send.empty());
    CHECK(Walk(w, 1300 * kMs, rest).send.empty());
    CHECK(Walk(w, 1900 * kMs, rest).send.empty());  // held
    const auto out = Walk(w, 2100 * kMs, rest);
    REQUIRE(Sent<proto::WorldBreak>(out).size() == 1);
    CHECK(Sent<proto::WorldBreak>(out)[0].hash == 0x11);
    // The far rock leaves: unloaded, nothing sent.
    for (std::uint64_t t = 2200; t < 4000; t += 100) {
        CHECK(Sent<proto::WorldBreak>(Walk(w, t * kMs, {})).empty());
    }
    CHECK_FALSE(w.Broken(0x22));
}

TEST_CASE("world: pots that leave with their whole area were not broken") {
    WorldSync w;
    std::vector<Seen> room;
    for (std::uint64_t i = 0; i < 6; ++i) {
        // Two by the door next to Link, four across the room.
        room.push_back(Thing(0x100 * (i + 1), 0x10 + i, 1, "Obj_Pot_A_01",
                             {1.0f, 2.0f, i < 2 ? 3.0f : 20.0f + static_cast<float>(i)}));
    }
    Walk(w, 1000 * kMs, room);
    for (std::uint64_t t = 1100; t < 4000; t += 100) {
        CHECK(Walk(w, t * kMs, {}).send.empty());
    }
    // And with no Link in the walk (the scene is going) nothing is a break either.
    WorldSync v;
    Walk(v, 1000 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01")});
    for (std::uint64_t t = 1100; t < 4000; t += 100) {
        CHECK(Walk(v, t * kMs, {}, std::nullopt).send.empty());
    }
}

TEST_CASE("world: a pot the game puts back after we saw it break is whole for everyone again") {
    WorldSync w;
    Walk(w, 1000 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_02_Respawn")});
    CHECK(Sent<proto::WorldBreak>(Walk(w, 1100 * kMs, {Thing(0x100, 0x11, 0, "Obj_Pot_A_02_Respawn")})).size() == 1);
    Walk(w, 1200 * kMs, {});
    Walk(w, 1300 * kMs, {});
    const auto out = Walk(w, 9000 * kMs, {Thing(0x400, 0x11, 1, "Obj_Pot_A_02_Respawn")});
    REQUIRE(Sent<proto::WorldBreak>(out).size() == 1);
    CHECK(Sent<proto::WorldBreak>(out)[0].flags == proto::world_break::restored);
    CHECK_FALSE(w.Broken(0x11));
    CHECK(w.Removes().empty());
    // The other game hears it and lets its own one be.
    WorldSync b;
    Walk(b, 1000 * kMs, {});
    b.Receive(BreakOf(0x11));
    Walk(b, 1100 * kMs, {});
    b.Receive(BreakOf(0x11, proto::world_break::restored));
    Walk(b, 9100 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_02_Respawn")});
    CHECK(b.Removes().empty());
}

TEST_CASE("world: a block pushed next to our Link is sent while it moves, then where it rests") {
    WorldSync w;
    CHECK(Walk(w, 1000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox)}).send.empty());
    CHECK(w.Places(0x100));
    auto out = Walk(w, 1100 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {1.2f, 2.0f, 3.0f})});
    REQUIRE(Sent<proto::WorldMove>(out).size() == 1);
    CHECK(Sent<proto::WorldMove>(out)[0].hash == 0x50);
    CHECK(Sent<proto::WorldMove>(out)[0].time_us == 1100 * kMs);
    CHECK(Sent<proto::WorldMove>(out)[0].position[0] == doctest::Approx(1.2f));
    CHECK(Sent<proto::WorldMove>(out)[0].flags == proto::world_move::carried);  // within 1.5 m
    CHECK(w.MoverOf(0x50) == 2);
    out = Walk(w, 1200 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {1.4f, 2.0f, 3.0f})});
    CHECK(Sent<proto::WorldMove>(out).size() == 1);
    for (std::uint64_t t = 1300; t < 1600; t += 100) {  // still: nothing new to say
        CHECK(Walk(w, t * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {1.4f, 2.0f, 3.0f})}).send.empty());
    }
    out = Walk(w, 1600 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {1.4f, 2.0f, 3.0f})});
    REQUIRE(Sent<proto::WorldPlace>(out).size() == 1);
    CHECK(Sent<proto::WorldPlace>(out)[0].position[0] == doctest::Approx(1.4f));
    CHECK(w.MoverOf(0x50) == 0);
    CHECK(Walk(w, 1700 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {1.4f, 2.0f, 3.0f})}).send.empty());
    // Every message is one the codec takes.
    for (const auto& m : out.send) {
        CHECK(proto::encode(m).has_value());
    }
}

TEST_CASE("world: an object that moves far from our Link is not ours to send") {
    WorldSync w;
    const proto::Vec3 far_link{100.0f, 2.0f, 3.0f};
    Walk(w, 1000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox)}, far_link);
    for (std::uint64_t t = 1100; t < 2000; t += 100) {
        const float x = 1.0f + static_cast<float>(t - 1000) / 1000.0f;
        CHECK(Walk(w, t * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {x, 2.0f, 3.0f})}, far_link).send.empty());
    }
    // A pot that only settles a few centimetres is not being moved.
    WorldSync v;
    Walk(v, 1000 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01")});
    CHECK(Walk(v, 1100 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01", {1.0f, 1.9f, 3.0f})}).send.empty());
}

TEST_CASE("world: another player's moves place our copy, a little behind, then at its rest place") {
    WorldSync w;
    Walk(w, 1000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox)});
    w.Receive(MoveOf(0x50, 2000 * kMs, 1.0f));
    w.Receive(MoveOf(0x50, 2100 * kMs, 2.0f));
    CHECK(Walk(w, 2100 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox)}).send.empty());
    CHECK(w.MoverOf(0x50) == 1);
    std::vector<CoopObjects::PoseWrite> poses;
    w.Poses(2150 * kMs, poses);  // shows 2050: half way
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].address == 0x100);
    CHECK(poses[0].position[0] == doctest::Approx(1.5f));
    REQUIRE(poses[0].rotation);
    CHECK((*poses[0].rotation)[0] == doctest::Approx(1.0f));
    // Our copy goes where it is put; that is not us moving it.
    CHECK(Walk(w, 2200 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {1.5f, 2.0f, 3.0f})}).send.empty());
    w.Receive(PlaceOf(0x50, 2.5f));
    CHECK(Walk(w, 2300 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {2.0f, 2.0f, 3.0f})}).send.empty());
    poses.clear();
    w.Poses(2400 * kMs, poses);
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].position[0] == doctest::Approx(2.5f));
    // Held for half a second, then let be; settling after that is not a move of ours.
    CHECK(Walk(w, 2900 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {2.5f, 2.0f, 3.0f})}).send.empty());
    poses.clear();
    w.Poses(2950 * kMs, poses);
    CHECK(poses.empty());
    CHECK(w.MoverOf(0x50) == 0);
    CHECK(Walk(w, 3000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {2.5f, 1.8f, 3.0f})}).send.empty());
    CHECK(Walk(w, 4300 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {2.5f, 1.8f, 3.0f})}).send.empty());
    // Later we push it ourselves.
    const auto out = Walk(w, 6000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {3.0f, 1.8f, 3.0f})}, proto::Vec3{3.0f, 1.8f, 4.0f});
    CHECK(Sent<proto::WorldMove>(out).size() == 1);
}

TEST_CASE("world: two players moving one object: the lower number keeps it") {
    // We are player 2 and pushing; player 1's moves come in: ours stop, theirs are placed.
    WorldSync w;
    Walk(w, 1000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox)});
    CHECK(Sent<proto::WorldMove>(Walk(w, 1100 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {1.2f, 2.0f, 3.0f})})).size() == 1);
    w.Receive(MoveOf(0x50, 1150 * kMs, 0.8f, 1));
    CHECK(Walk(w, 1200 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {1.4f, 2.0f, 3.0f})}).send.empty());
    CHECK(w.MoverOf(0x50) == 1);
    std::vector<CoopObjects::PoseWrite> poses;
    w.Poses(1300 * kMs, poses);
    CHECK(poses.size() == 1);
    // We are player 2 and pushing; player 3's moves come in: ours go on, theirs are not placed.
    WorldSync v;
    Walk(v, 1000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox)});
    Walk(v, 1100 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {1.2f, 2.0f, 3.0f})});
    v.Receive(MoveOf(0x50, 1150 * kMs, 0.8f, 3));
    CHECK(Sent<proto::WorldMove>(Walk(v, 1200 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {1.4f, 2.0f, 3.0f})})).size() == 1);
    CHECK(v.MoverOf(0x50) == 2);
    poses.clear();
    v.Poses(1300 * kMs, poses);
    CHECK(poses.empty());
    // Between two other players the lower number's moves are the ones placed.
    WorldSync u;
    Walk(u, 1000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox)}, kLink, 4);
    u.Receive(MoveOf(0x50, 1100 * kMs, 5.0f, 3));
    u.Receive(MoveOf(0x50, 1110 * kMs, 9.0f, 1));
    u.Receive(MoveOf(0x50, 1120 * kMs, 5.1f, 3));
    Walk(u, 1200 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox)}, kLink, 4);
    CHECK(u.MoverOf(0x50) == 1);
    poses.clear();
    u.Poses(1400 * kMs, poses);
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].position[0] == doctest::Approx(9.0f));
}

TEST_CASE("world: a rolling boulder rolls for everyone as the nearest game rolls it (issue 84)") {
    const char* rock = "Obj_Kokiri_RollingRock_A_01";
    auto at = [rock](std::uint64_t t) {
        // It rolls 2 m a second along x past our Link, never resting.
        return Thing(0x100, 0x60, std::nullopt, rock, {1.0f + static_cast<float>(t - 1000) / 500.0f, 2.0f, 3.0f});
    };
    WorldSync a;  // player 1, beside it
    Walk(a, 1000 * kMs, {at(1000)}, kLink, 1);
    std::size_t moves = 0;
    for (std::uint64_t t = 1100; t <= 3000; t += 100) {
        const auto out = Walk(a, t * kMs, {at(t)}, kLink, 1);
        for (const auto& m : Sent<proto::WorldMove>(out)) {
            CHECK(m.flags == 0);  // rolling past Link is not carrying it
            ++moves;
        }
        CHECK(Sent<proto::WorldPlace>(out).empty());  // it never rests
    }
    CHECK(moves >= 15);
    CHECK(a.MoverOf(0x60) == 1);
    // Player 2 rolls its own copy too, beside it, but player 1's rolls come in: it follows them.
    WorldSync b;
    Walk(b, 1000 * kMs, {at(1000)}, kLink, 2);
    b.Receive(MoveOf(0x60, 1050 * kMs, 1.1f, 1));
    CHECK(Sent<proto::WorldMove>(Walk(b, 1100 * kMs, {at(1100)}, kLink, 2)).empty());
    for (std::uint64_t t = 1200; t <= 2000; t += 100) {
        b.Receive(MoveOf(0x60, (t - 50) * kMs, 1.0f + static_cast<float>(t - 1050) / 500.0f, 1));
        CHECK(Walk(b, t * kMs, {at(t + 300)}, kLink, 2).send.empty());  // its own roll drifted ahead
    }
    CHECK(b.MoverOf(0x60) == 1);
    std::vector<CoopObjects::PoseWrite> poses;
    b.Poses(2000 * kMs, poses);
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].position[0] < 1.0f + 1000.0f / 500.0f);  // player 1's place, not its own
    // Player 1 leaves: its rolls stop; after the quiet time player 2 rolls it for the others.
    bool took = false;
    for (std::uint64_t t = 2100; t <= 5000 && !took; t += 100) {
        took = !Sent<proto::WorldMove>(Walk(b, t * kMs, {at(t)}, kLink, 2)).empty();
    }
    CHECK(took);
}

TEST_CASE("world: a block first seen after it was moved goes to its rest place") {
    WorldSync w;
    Walk(w, 1000 * kMs, {});
    w.Receive(PlaceOf(0x50, 7.0f));  // the server's replay as we entered
    Walk(w, 1100 * kMs, {});
    Walk(w, 30000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox)});
    std::vector<CoopObjects::PoseWrite> poses;
    w.Poses(30050 * kMs, poses);
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].position[0] == doctest::Approx(7.0f));
    CHECK(Walk(w, 30200 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {7.0f, 2.0f, 3.0f})}).send.empty());
    Walk(w, 30600 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {7.0f, 2.0f, 3.0f})});
    poses.clear();
    w.Poses(30650 * kMs, poses);
    CHECK(poses.empty());
}

TEST_CASE("world: a carried pot is moved for the others and breaks where it lands") {
    WorldSync a;  // the carrier
    Walk(a, 1000 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01")});
    auto out = Walk(a, 1100 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01", {1.0f, 3.2f, 3.6f})});
    REQUIRE(Sent<proto::WorldMove>(out).size() == 1);
    out = Walk(a, 1200 * kMs, {Thing(0x100, 0x11, 0, "Obj_Pot_A_01", {1.0f, 2.0f, 8.0f})});
    CHECK(Sent<proto::WorldBreak>(out).size() == 1);
    CHECK(Sent<proto::WorldMove>(out).empty());  // broken: no more moves
    CHECK(a.MoverOf(0x11) == 0);
    WorldSync b;  // the watcher
    Walk(b, 1000 * kMs, {Thing(0x900, 0x11, 1, "Obj_Pot_A_01")});
    Walk(b, 5000 * kMs, {Thing(0x900, 0x11, 1, "Obj_Pot_A_01")});
    b.Receive(MoveOf(0x11, 5000 * kMs, 1.0f));
    Walk(b, 5100 * kMs, {Thing(0x900, 0x11, 1, "Obj_Pot_A_01")});
    std::vector<CoopObjects::PoseWrite> poses;
    b.Poses(5150 * kMs, poses);
    CHECK(poses.size() == 1);
    b.Receive(BreakOf(0x11));
    CHECK(Walk(b, 5200 * kMs, {Thing(0x900, 0x11, 1, "Obj_Pot_A_01")}).lives.size() == 1);
    poses.clear();
    b.Poses(5250 * kMs, poses);
    CHECK(poses.empty());  // it is breaking: not placed any more
}

TEST_CASE("world: Reset and a reused address start clean") {
    WorldSync w;
    Walk(w, 1000 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01")});
    w.Receive(BreakOf(0x11));
    w.Reset();
    CHECK(w.Count() == 0);
    Walk(w, 2000 * kMs, {Thing(0x100, 0x11, 1, "Obj_Pot_A_01")});
    CHECK_FALSE(w.Broken(0x11));
    // The same address with another placement is another object: no break from the life change.
    CHECK(Walk(w, 2100 * kMs, {Thing(0x100, 0x77, 0, "Obj_Pot_A_01")}).send.empty());
    CHECK(w.Count() == 1);
}

TEST_CASE("world: our sword passing a sign sends a hit, another player's hit is given to our sign") {
    const char* sign = "Obj_GuidePost_A_02";
    CHECK(CoopWorld::IsSign(sign));
    CHECK(CoopWorld::IsSign("Obj_Kokiri_Signboard_A_01"));
    CHECK_FALSE(CoopWorld::IsSign("Obj_Pot_A_01"));
    WorldSync w;
    Walk(w, 1000 * kMs, {Thing(0x100, 0x70, 100, sign), Thing(0x200, 0x71, 100, sign, {30.0f, 2.0f, 3.0f})});
    const float nan = std::numeric_limits<float>::quiet_NaN();
    proto::GrassCut swing;
    swing.scene = kScene;
    swing.position = {1.0f, 3.0f, 4.5f};  // 1.5 m off: just in reach
    swing.radius = 0.5f;
    swing.sweep = {proto::Vec3{1.2f, 3.1f, 3.4f}, proto::Vec3{0.4f, 3.0f, 3.9f}, proto::Vec3{nan, nan, nan}};
    WorldSync::Out out;
    w.Swing(swing, kLink, 1100 * kMs, out);
    REQUIRE(Sent<proto::WorldHit>(out).size() == 1);
    const proto::WorldHit hit = Sent<proto::WorldHit>(out)[0];
    CHECK(hit.hash == 0x70);
    CHECK(hit.at == proto::Vec3{1.2f, 3.1f, 3.4f});  // the nearest point of the swing
    CHECK(hit.from == kLink);
    CHECK(proto::encode(proto::Message{hit}).has_value());
    // The next slots of the same swing do not send it again; a later swing does.
    out = {};
    w.Swing(swing, kLink, 1150 * kMs, out);
    CHECK(out.send.empty());
    w.Swing(swing, kLink, 1500 * kMs, out);
    CHECK(Sent<proto::WorldHit>(out).size() == 1);
    // A cut that is no sword swing (an AI's sphere), or one for another scene, is nothing.
    out = {};
    proto::GrassCut sphere = swing;
    sphere.sweep = {proto::Vec3{nan, nan, nan}, proto::Vec3{nan, nan, nan}, proto::Vec3{nan, nan, nan}};
    w.Swing(sphere, kLink, 3000 * kMs, out);
    swing.scene = "Market";
    w.Swing(swing, kLink, 3000 * kMs, out);
    CHECK(out.send.empty());

    // The other game: the hit waits for the sign, then goes to it once.
    WorldSync b;
    Walk(b, 1000 * kMs, {});
    proto::WorldHit got = hit;
    got.player_number = 1;
    b.Receive(got);
    CHECK(Walk(b, 1100 * kMs, {}).hits.empty());
    CHECK(Walk(b, 2000 * kMs, {Thing(0x900, 0x70, 100, sign)}).hits.empty());  // just loaded
    const auto given = Walk(b, 2400 * kMs, {Thing(0x900, 0x70, 100, sign)});
    REQUIRE(given.hits.size() == 1);
    CHECK(given.hits[0].address == 0x900);
    CHECK(given.hits[0].at == hit.at);
    CHECK(given.hits[0].from == kLink);
    CHECK(given.send.empty());
    CHECK(Walk(b, 2500 * kMs, {Thing(0x900, 0x70, 100, sign)}).hits.empty());
}
namespace {

// A sign in fake memory for the probe: the actor, its component table at
// +0x270 and two components (table +0x10 and +0x2b8).
struct FakeSign {
    static constexpr std::uint64_t kActor = 0x10000000;
    static constexpr std::uint64_t kTable = 0x20000000;
    static constexpr std::uint64_t kLife = 0x30000000;
    static constexpr std::uint64_t kCut = 0x40000000;
    static constexpr std::uint64_t kPoint = 0x50000000;  // table +0x268: the cut point and flag
    std::map<std::uint64_t, std::uint64_t> words;
    int reads = 0;

    FakeSign() {
        words[kActor + 0x270] = kTable;
        words[kTable + 0x10] = kCut;
        words[kTable + 0x2b8] = kLife;
        words[kTable + 0x2c0] = kLife;  // the same component twice: read once
        words[kTable + 0x268] = kPoint;
    }
    std::optional<std::uint64_t> operator()(std::uint64_t a) {
        ++reads;
        const auto in = [a](std::uint64_t base, std::uint64_t size) { return a >= base && a < base + size; };
        if (!in(kActor, 0x600) && !in(kTable, 0x300) && !in(kLife, 0x200) && !in(kCut, 0x200) &&
            !in(kPoint, 0x200)) {
            return std::nullopt;
        }
        const auto it = words.find(a);
        return it == words.end() ? 0 : it->second;
    }
};

} // namespace

TEST_CASE("sign probe: names where a word is") {
    using CoopWorld::SignProbe;
    CHECK(SignProbe::Where(0x4a8) == "actor+0x4a8");
    CHECK(SignProbe::Where((1ull << 32) | 0x2b8) == "table+0x2b8");
    CHECK(SignProbe::Where(((2ull + 0x57) << 32) | 0x40) == "c+0x2b8+0x40");
    CHECK(SignProbe::Where(0) == "actor+0x0");
    CHECK(SignProbe::Where(2ull << 32) == "c+0x0+0x0");
}

TEST_CASE("sign probe: logs what a cut changed, without the counters") {
    using CoopWorld::SignProbe;
    FakeSign mem;
    SignProbe p;
    auto read = [&mem](std::uint64_t a) { return mem(a); };
    const SignProbe::Sign sign{FakeSign::kActor, 0x70, "Obj_GuidePost_A_02", {1.0f, 2.0f, 3.0f}};
    const std::optional<proto::Vec3> link = proto::Vec3{2.0f, 2.0f, 3.0f};
    CHECK(p.Look(read, {sign}, link, 1000 * kMs).empty());  // first copy
    mem.words[FakeSign::kLife + 0x8] = 1;                    // a counter
    CHECK(p.Look(read, {sign}, link, 1500 * kMs).empty());  // too soon for a copy
    CHECK(p.Look(read, {sign}, link, 2000 * kMs).empty());  // second copy: the counter is noisy
    // The cut: a word of a component, a bit of the actor, a new table entry.
    mem.words[FakeSign::kLife + 0x8] = 2;
    mem.words[FakeSign::kCut + 0x40] = 1;
    mem.words[FakeSign::kActor + 0x4a8] = 0x800000;
    p.Hit(sign, true, 2600 * kMs);  // the swing is read later than the cut
    p.Hit(sign, false, 2700 * kMs);  // a second hit does not start again
    CHECK(p.Look(read, {sign}, link, 2800 * kMs).empty());
    auto lines = p.Look(read, {sign}, link, 3100 * kMs);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("sign probe: 0000000000000070 Obj_GuidePost_A_02 our cut +0.5 s: 2 words changed:") == 0);
    CHECK(lines[0].find("c+0x10+0x40:0>1") != std::string::npos);
    CHECK(lines[0].find("actor+0x4a8:0>800000") != std::string::npos);
    CHECK(lines[0].find("c+0x2b8+0x8") == std::string::npos);  // the counter
    CHECK(p.Look(read, {sign}, link, 4000 * kMs).empty());
    lines = p.Look(read, {sign}, link, 4600 * kMs);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("our cut +2.0 s: 2 words changed") != std::string::npos);
    // Done: copies start again, with nothing to report.
    CHECK(p.Look(read, {sign}, link, 5000 * kMs).empty());
    CHECK(p.Look(read, {sign}, link, 7000 * kMs).empty());
}

TEST_CASE("sign probe: a given hit, a hit with no copy before it, a far sign") {
    using CoopWorld::SignProbe;
    FakeSign mem;
    SignProbe p;
    auto read = [&mem](std::uint64_t a) { return mem(a); };
    const SignProbe::Sign sign{FakeSign::kActor, 0x70, "Obj_GuidePost_A_02", {1.0f, 2.0f, 3.0f}};
    // Far from our Link: not read.
    CHECK(p.Look(read, {sign}, proto::Vec3{40.0f, 2.0f, 3.0f}, 1000 * kMs).empty());
    CHECK(mem.reads == 0);
    // Hit with no copy from before it.
    p.Hit(sign, false, 1100 * kMs);
    auto lines = p.Look(read, {sign}, proto::Vec3{40.0f, 2.0f, 3.0f}, 1700 * kMs);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("given hit +0.6 s: no copy from before the hit") != std::string::npos);
    // A given hit lands after the call: a copy taken just now is from before it.
    SignProbe q;
    const std::optional<proto::Vec3> near = proto::Vec3{6.0f, 2.0f, 3.0f};
    CHECK(q.Look(read, {sign}, near, 1000 * kMs).empty());
    q.Hit(sign, false, 1000 * kMs);
    mem.words[FakeSign::kCut + 0x48] = 5;
    lines = q.Look(read, {sign}, near, 1500 * kMs);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("given hit +0.5 s: 1 words changed: c+0x10+0x48:0>5") != std::string::npos);
    // A sign that left the walk is forgotten; with a hit still to report, that is logged.
    lines = q.Look(read, {}, near, 1600 * kMs);  // its +2 s line was still to come
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("given hit: gone after the hit (+0.6 s)") != std::string::npos);
    CHECK(q.Look(read, {sign}, near, 1700 * kMs).empty());
    q.Hit(sign, true, 2000 * kMs);
    lines = q.Look(read, {}, near, 2300 * kMs);
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == "sign probe: 0000000000000070 Obj_GuidePost_A_02 our cut: gone after the hit (+0.3 s)");
    CHECK(q.Look(read, {}, near, 2400 * kMs).empty());
    CHECK(q.Look(read, {sign}, near, 3600 * kMs).empty());
}

TEST_CASE("sign cut: our cut's words go out as SIGN_CUT and are written into the other game's sign") {
    using CoopWorld::SignProbe;
    const char* name = "Obj_GuidePost_A_02";
    FakeSign mem;
    auto read = [&mem](std::uint64_t a) { return mem(a); };
    WorldSync w;
    WorldSync::Out out;
    Walk(w, 1000 * kMs, {Thing(FakeSign::kActor, 0x70, 100, name)});
    w.ProbeSigns(read, 1000 * kMs, out);
    CHECK(out.send.empty());
    // The cut: the point (into an empty word) and the flag, words off the list, the hit record.
    mem.words[FakeSign::kPoint + 0x98] = 0x42ccd1dec0f55bf7;
    mem.words[FakeSign::kPoint + 0x1e8] = 0x800c000080f3;
    mem.words[FakeSign::kPoint + 0x1f0] = 5;                  // not on the list
    mem.words[FakeSign::kCut + 0x18] = 0x800c000080f3;         // entry 0x10: not on the list
    mem.words[FakeSign::kLife + 0x1f8] = 0x4004000000060001;  // the hit record
    const float nan = std::numeric_limits<float>::quiet_NaN();
    proto::GrassCut swing;
    swing.scene = kScene;
    swing.position = {1.2f, 3.0f, 3.4f};
    swing.radius = 0.5f;
    swing.sweep = {proto::Vec3{1.2f, 3.1f, 3.4f}, proto::Vec3{nan, nan, nan}, proto::Vec3{nan, nan, nan}};
    w.Swing(swing, kLink, 1400 * kMs, out);
    REQUIRE(Sent<proto::WorldHit>(out).size() == 1);
    proto::WorldHit hit = Sent<proto::WorldHit>(out)[0];
    out = {};
    Walk(w, 1900 * kMs, {Thing(FakeSign::kActor, 0x70, 100, name)});
    w.ProbeSigns(read, 1900 * kMs, out);
    REQUIRE(Sent<proto::SignCut>(out).size() == 1);
    proto::SignCut cut = Sent<proto::SignCut>(out)[0];
    CHECK(cut.scene == kScene);
    CHECK(cut.hash == 0x70);
    const std::uint64_t entry = 2 + 0x268 / 8;
    REQUIRE(cut.words.size() == 2);
    CHECK(cut.words[0] == proto::SignWord{(entry << 32) | 0x98, 0x42ccd1dec0f55bf7});
    CHECK(cut.words[1] == proto::SignWord{(entry << 32) | 0x1e8, 0x800c000080f3});
    CHECK(proto::encode(proto::Message{cut}).has_value());
    // Only once: the +2 s lines send nothing.
    out = {};
    w.ProbeSigns(read, 3500 * kMs, out);
    CHECK(out.send.empty());

    // The other game: the hit first, then the words once its probe has a copy.
    FakeSign other;
    auto read_b = [&other](std::uint64_t a) { return other(a); };
    WorldSync b;
    WorldSync::Out probe;
    Walk(b, 1000 * kMs, {Thing(FakeSign::kActor, 0x70, 100, name)});
    b.ProbeSigns(read_b, 1000 * kMs, probe);  // B stood near the sign: the probe has its copy
    hit.player_number = 1;
    cut.player_number = 1;
    // A word off the list, as a buggy or hostile sender might send it: dropped.
    cut.words.push_back({(4ull << 32) | 0x18, 1});
    b.Receive(hit);
    b.Receive(cut);
    auto step = Walk(b, 1500 * kMs, {Thing(FakeSign::kActor, 0x70, 100, name)});
    CHECK(step.hits.size() == 1);
    CHECK(step.words.empty());
    CHECK(Walk(b, 1600 * kMs, {Thing(FakeSign::kActor, 0x70, 100, name)}).words.empty());  // 0.2 s after the hit
    step = Walk(b, 1700 * kMs, {Thing(FakeSign::kActor, 0x70, 100, name)});
    REQUIRE(step.words.size() == 2);
    CHECK(step.words[0].part.table == FakeSign::kTable);
    CHECK(step.words[0].part.component == FakeSign::kPoint);
    CHECK(std::any_of(step.notes.begin(), step.notes.end(), [](const std::string& n) {
        return n.find("cut by another player: 2 of 3 words to write") != std::string::npos;
    }));
    // The write goes where the probe saw the component, after the checks.
    CHECK(b.SignWordTarget(read_b, step.words[0]) == FakeSign::kPoint + 0x98);
    CHECK(b.SignWordTarget(read_b, step.words[1]) == FakeSign::kPoint + 0x1e8);
    auto moved = step.words[0];
    other.words[FakeSign::kTable + 0x268] = FakeSign::kCut;  // the table changed since the copy
    CHECK_FALSE(b.SignWordTarget(read_b, moved).has_value());
    other.words[FakeSign::kTable + 0x268] = FakeSign::kPoint;
    moved.part.component = 0x7;  // not a pointer
    CHECK_FALSE(b.SignWordTarget(read_b, moved).has_value());
    moved = step.words[0];
    moved.hash = 0x71;  // the actor is another sign now
    CHECK_FALSE(b.SignWordTarget(read_b, moved).has_value());
    moved = step.words[0];
    moved.key = (moved.key & ~0xffffffffull) | 0x1f0;  // off the list
    CHECK_FALSE(b.SignWordTarget(read_b, moved).has_value());
    other.words[FakeSign::kPoint + 0x98] = 0x42cd0ee8c0eedabe;  // a transform there now
    CHECK_FALSE(b.SignWordTarget(read_b, step.words[0]).has_value());
    CHECK(Walk(b, 1900 * kMs, {Thing(FakeSign::kActor, 0x70, 100, name)}).words.empty());  // once

    // A game entering later gets the words (no hit) once the sign is loaded and copied.
    FakeSign third;
    WorldSync c;
    Walk(c, 1000 * kMs, {});
    c.Receive(cut);
    CHECK(Walk(c, 1100 * kMs, {Thing(FakeSign::kActor, 0x70, 100, name)}).words.empty());  // just loaded
    c.ProbeSigns([&third](std::uint64_t a) { return third(a); }, 1100 * kMs, probe);
    CHECK(Walk(c, 1400 * kMs, {Thing(FakeSign::kActor, 0x70, 100, name)}).words.size() == 2);
}

TEST_CASE("sign cut: the message checks its words") {
    proto::SignCut m;
    m.player_number = 1;
    m.scene = kScene;
    m.hash = 0x70;
    CHECK_FALSE(proto::encode(proto::Message{m}).has_value());  // no words
    m.words = {{(4ull << 32) | 0x18, 1}};
    const auto bytes = proto::encode(proto::Message{m});
    REQUIRE(bytes.has_value());
    proto::Message back;
    REQUIRE(proto::decode(*bytes, back) == proto::Error::none);
    CHECK(std::get<proto::SignCut>(back) == m);
    CHECK(static_cast<int>(proto::type_of(back)) == 80);
    for (const std::uint64_t key : std::vector<std::uint64_t>{0x18, (1ull << 32) | 0x18, (4ull << 32) | 0x1c,
                                    (4ull << 32) | 0x200, (0x62ull << 32)}) {
        m.words = {{key, 1}};
        CHECK_FALSE(proto::encode(proto::Message{m}).has_value());
    }
    m.words.assign(proto::kMaxSignCutWords + 1, {(4ull << 32) | 0x18, 1});
    CHECK_FALSE(proto::encode(proto::Message{m}).has_value());
}

TEST_CASE("sign cut: what another player's word may be written over") {
    using CoopWorld::SignWordWritable;
    CHECK(SignWordWritable(0x8004000080f3, 0x800c000080f3));      // a flag
    CHECK(SignWordWritable(0x7fc00000, 0x3e13b645));              // NaN to a float
    CHECK(SignWordWritable(0, 0x42ccd1dec0f55bf7));               // a point into an empty word
    CHECK_FALSE(SignWordWritable(0x42cd0ee8c0eedabe, 0x42cd0edac0ee8696));  // over a transform
    CHECK_FALSE(SignWordWritable(0x42cd0ee8c0eedabe, 0));
    CHECK_FALSE(SignWordWritable(0, 0x214ba36b18));               // a heap pointer
    CHECK_FALSE(SignWordWritable(0x214ba36b18, 1));               // over one
    CHECK(CoopWorld::SignCutKeyAllowed(((2ull + 0x268 / 8) << 32) | 0x1e8));
    CHECK(CoopWorld::SignCutKeyAllowed(((2ull + 0x130 / 8) << 32) | 0xb0));
    CHECK_FALSE(CoopWorld::SignCutKeyAllowed(((2ull + 0x268 / 8) << 32) | 0x1f0));
    CHECK_FALSE(CoopWorld::SignCutKeyAllowed(((2ull + 0x2b8 / 8) << 32) | 0x1f8));
    CHECK_FALSE(CoopWorld::SignCutKeyAllowed(0x268));
    // The world sync drops a pointer and another scene's cut.
    FakeSign mem;
    WorldSync b;
    WorldSync::Out probe;
    Walk(b, 1000 * kMs, {Thing(FakeSign::kActor, 0x70, 100, "Obj_GuidePost_A_02")});
    b.ProbeSigns([&mem](std::uint64_t a) { return mem(a); }, 1000 * kMs, probe);
    const std::uint64_t entry = 2 + 0x268 / 8;
    proto::SignCut cut;
    cut.player_number = 1;
    cut.scene = kScene;
    cut.hash = 0x70;
    cut.words = {{(entry << 32) | 0x98, 0x214ba36b18}, {(entry << 32) | 0x1e8, 1}};
    b.Receive(cut);
    const auto step = Walk(b, 1500 * kMs, {Thing(FakeSign::kActor, 0x70, 100, "Obj_GuidePost_A_02")});
    REQUIRE(step.words.size() == 1);
    CHECK(step.words[0].value == 1);
    cut.scene = "Market";
    b.Receive(cut);
    CHECK(Walk(b, 2000 * kMs, {Thing(FakeSign::kActor, 0x70, 100, "Obj_GuidePost_A_02")}).words.empty());
}

TEST_CASE("gimmick probe: names of switches, water planes, shutters, torches and lifts") {
    for (const char* n : {"Obj_StepSwitch_A_01", "Obj_InsideJabu_Jabu_StepSwitch_A_01", "Obj_EyeSwitch_A_02",
                          "Obj_SwitchHit_A_01", "Obj_WaterTemple_WaterPlane_A_03", "Obj_Well_WaterPlane_A_01",
                          "Obj_WaterTemple_SwitchShutter_A_01", "Obj_ForestTemple_SwitchCandleStick_A_01",
                          "Obj_CandleStick_A_01", "Obj_InsideTheDekuTree_ElevatorLift_A_01",
                          "Obj_FireTemple_MoveFloor_A_01", "Obj_SolarSwitch_A_01", "Obj_MovableBox_A_01",
                          "Obj_MovableBox_BasePosAnchor", "Obj_InsideTheDekuTree_FallingLadder_A_01",
                          "Obj_InsideTheDekuTree_Stopper_A_01", "Obj_InsideTheDekuTree_FallingFloor_A_02"}) {
        CAPTURE(n);
        CHECK(CoopWorld::IsGimmick(n));
    }
    CHECK(CoopWorld::IsGimmick("Enemy_Scrub_Deku_Brothers"));    // dungeon-switches: the 2-3-1 probe
    CHECK(CoopWorld::IsGimmick("Scrub_Deku_Brothers_Manager"));
    CHECK_FALSE(CoopWorld::IsGimmick("Enemy_Scrub_Deku"));
    for (const char* n : {"Obj_Pot_A_01", "Enemy_Stalchild", "Land_Kokiri_WaterPlane_A_01", "Obj_TBox_A_01"}) {
        CAPTURE(n);
        CHECK_FALSE(CoopWorld::IsGimmick(n));
    }
}

TEST_CASE("gimmick probe: a word that steps once is logged, one that keeps moving is muted") {
    std::map<std::uint64_t, std::uint64_t> mem;
    constexpr std::uint64_t kAt = 0x10000;
    for (std::uint64_t i = 0; i < 0x600; i += 8) mem[kAt + i] = 0;  // the actor, no component table
    const CoopWorld::SignProbe::ReadWord read = [&](std::uint64_t a) -> std::optional<std::uint64_t> {
        const auto it = mem.find(a);
        if (it == mem.end()) return std::nullopt;
        return it->second;
    };
    CoopWorld::GimmickProbe probe;
    std::vector<CoopWorld::GimmickProbe::Gimmick> g(1);
    g[0].address = kAt;
    g[0].hash = 0x1234;
    g[0].name = "Obj_StepSwitch_A_01";
    g[0].position = {2.0f, 0.0f, 0.0f};
    const std::optional<coop::protocol::Vec3> link = coop::protocol::Vec3{0.0f, 0.0f, 0.0f};
    std::uint64_t t = 1'000'000;
    const auto look = [&] {
        t += CoopWorld::GimmickProbe::kEveryUs;
        return probe.Look(read, g, link, t);
    };
    auto lines = look();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("watching") != std::string::npos);
    CHECK(look().empty());
    CHECK(look().empty());  // two equal copies now
    mem[kAt + 0x100] = 1;  // pressed
    lines = look();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("Obj_StepSwitch_A_01") != std::string::npos);
    CHECK(lines[0].find("actor+0x100:0>1") != std::string::npos);
    CHECK(look().empty());  // stays pressed: no step
    CHECK(look().empty());
    mem[kAt + 0x100] = 0;  // released
    lines = look();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("actor+0x100:1>0") != std::string::npos);

    // A timer: steps every time it is stable for two copies... it never is,
    // so it is never logged; one that steps every third look is muted after 6.
    int logged = 0;
    for (int i = 0; i < 40; ++i) {
        if (i % 3 == 0) mem[kAt + 0x180] += 1;
        for (const auto& l : look()) logged += l.find("actor+0x180") != std::string::npos ? 1 : 0;
    }
    CHECK(logged == CoopWorld::GimmickProbe::kNoisySteps);

    // Out of reach: forgotten, and nothing read.
    g[0].position = {40.0f, 0.0f, 0.0f};
    mem[kAt + 0x100] = 7;
    CHECK(look().empty());
}

namespace {
const char* const kPillar = "Obj_FireTemple_HammerSlipPillar_A_01";
Seen Fallen(std::uint64_t address, std::uint64_t hash, std::optional<std::int32_t> life) {
    // Fallen over: a metre and a half lower and further on (a bridge now).
    return Thing(address, hash, life, kPillar, proto::Vec3{3.5f, 0.5f, 3.0f});
}
} // namespace

TEST_CASE("world: hammer pillars fall for everyone and are never moved or deleted (breakables)") {
    CHECK(CoopWorld::IsFaller(kPillar));
    CHECK(CoopWorld::IsFaller("Obj_FireTemple_HammerSlipPillar_A_02"));
    CHECK_FALSE(CoopWorld::IsFaller("Obj_Pot_A_01"));
    CHECK_FALSE(CoopWorld::IsBreakable(kPillar));  // the object sharing leaves it alone

    // Ours: the hammer takes its life, it falls; the break is sent once, the fall is no move.
    WorldSync a;
    Walk(a, 1000 * kMs, {Thing(0x100, 0x11, 1, kPillar)});
    const auto hit = Walk(a, 1100 * kMs, {Thing(0x100, 0x11, 0, kPillar)});
    REQUIRE(Sent<proto::WorldBreak>(hit).size() == 1);
    for (std::uint64_t t = 1200; t < 4000; t += 100) {
        const auto out = Walk(a, t * kMs, {Fallen(0x100, 0x11, 0)});
        CHECK(Sent<proto::WorldMove>(out).empty());
        CHECK(Sent<proto::WorldPlace>(out).empty());
        CHECK(Sent<proto::WorldBreak>(out).empty());
    }
    CHECK(a.Removes().empty());

    // Another player's: life 0 is written, again each second while it stands, never deleted.
    WorldSync b;
    Walk(b, 1000 * kMs, {Thing(0x100, 0x11, 1, kPillar)});
    Walk(b, 5000 * kMs, {Thing(0x100, 0x11, 1, kPillar)});
    b.Receive(BreakOf(0x11));
    auto out = Walk(b, 5100 * kMs, {Thing(0x100, 0x11, 1, kPillar)});
    REQUIRE(out.lives.size() == 1);
    CHECK(out.lives[0].address == 0x100);
    CHECK(out.lives[0].life == 0);
    CHECK(out.send.empty());
    CHECK(Walk(b, 5600 * kMs, {Thing(0x100, 0x11, 1, kPillar)}).lives.empty());
    CHECK(Walk(b, 6200 * kMs, {Thing(0x100, 0x11, 1, kPillar)}).lives.size() == 1);  // again
    for (std::uint64_t t = 6300; t < 20000; t += 100) {
        out = Walk(b, t * kMs, {Fallen(0x100, 0x11, 0)});  // it fell
        CHECK(out.lives.empty());
        CHECK(Sent<proto::WorldMove>(out).empty());
    }
    CHECK(b.Removes().empty());

    // A game that loads the room after the break: no delete; a pillar that loaded
    // standing is felled, one that loaded fallen (life 0) is left as it is.
    WorldSync c;
    c.Receive(BreakOf(0x11));
    c.Receive(BreakOf(0x22));
    out = Walk(c, 1000 * kMs, {Thing(0x100, 0x11, 1, kPillar), Fallen(0x200, 0x22, 0)});
    REQUIRE(out.lives.size() == 1);
    CHECK(out.lives[0].address == 0x100);
    for (std::uint64_t t = 1100; t < 10000; t += 100) {
        Walk(c, t * kMs, {Fallen(0x100, 0x11, 0), Fallen(0x200, 0x22, 0)});
    }
    CHECK(c.Removes().empty());
}

// ------------------------------------------------- breakables-2

namespace {
const char* const kKey = "Obj_Dungeon_SmallKey";
const std::string kWell = "BottomOfTheWell";

WorldSync::Out WellWalk(WorldSync& w, std::uint64_t now, const std::vector<Seen>& seen) {
    return Walk(w, now, seen, kLink, 2, kWell);
}
proto::WorldBreak KeyBreak(std::uint64_t hash) { return BreakOf(hash, 0, kWell); }

bool Removed(const WorldSync& w, std::uint64_t address) {
    const auto r = w.Removes();
    return std::find(r.begin(), r.end(), address) != r.end();
}

// A store of Int flags by name hash.
class CountStore final : public coop::host::FlagStore {
public:
    std::map<std::uint32_t, std::int32_t> values;
    std::optional<proto::FlagEntry> read(coop::host::FlagKey key) override {
        const auto it = values.find(key.hash);
        if (it == values.end()) {
            return std::nullopt;
        }
        return proto::FlagEntry(key, proto::FlagKind::int32, static_cast<std::uint32_t>(it->second));
    }
    bool write(const proto::FlagEntry& e) override {
        values[e.key.hash] = static_cast<std::int32_t>(e.value);
        return true;
    }
};
} // namespace

TEST_CASE("world: lying small keys, shifters and hammer switches by name (breakables-2)") {
    CHECK(CoopWorld::IsLyingKey(kKey));
    CHECK(CoopWorld::IsLyingKey("Obj_Dungeon_SmallKey_GerudoFortress_04"));
    CHECK_FALSE(CoopWorld::IsLyingKey("Obj_MiniGame_SmallKey"));
    CHECK_FALSE(CoopWorld::IsBreakable(kKey));  // not the enemy-style sharing
    for (const char* name : {"Obj_FireTemple_HammerDropFloor_A_01", "Obj_FireTemple_HammerDropPillar_A_01",
                             "Obj_FireTemple_HammerStair_A_01", "Obj_InsideTheDekuTree_FallingLadder_A_01",
                             "Obj_InsideTheDekuTree_FallingFloor_A_03", "Obj_ShadowTemple_FallingFloor_A_03"}) {
        CAPTURE(name);
        CHECK(CoopWorld::IsShifter(name));
        CHECK(CoopWorld::IsWorldObject(name));
    }
    // The Deku Tree's keyed floors (the slingshot room's bridge) are shifters
    // too: the others see the fall, then the key takes them (dungeon-switches).
    // The Shadow Temple's keyed one goes by its key (kept as ground).
    CHECK(CoopWorld::IsShifter("Obj_InsideTheDekuTree_FallingFloor_A_01"));
    CHECK(CoopWorld::IsShifter("Obj_InsideTheDekuTree_FallingFloor_A_02"));
    CHECK_FALSE(CoopWorld::IsShifter("Obj_ShadowTemple_FallingFloor_A_01"));
    for (const char* name : {"Obj_InsideTheDekuTree_FallingFloor_A_01", "Obj_InsideTheDekuTree_FallingFloor_A_02",
                             "Obj_InsideTheDekuTree_InteractiveSpiderWebParts_A_01",
                             "Obj_InsideTheDekuTree_InteractiveSpiderWebParts_A_02"}) {
        CAPTURE(name);
        CHECK(CoopWorld::IsFallTaken(name));
    }
    for (const char* name : {"Obj_ShadowTemple_FallingFloor_A_01", "Obj_InsideTheDekuTree_SpiderWeb_A_01",
                             "Obj_TBox_A_02", "Obj_InsideTheDekuTree_FallingLadder_A_01"}) {
        CAPTURE(name);
        CHECK_FALSE(CoopWorld::IsFallTaken(name));
    }
    CHECK(CoopWorld::IsFaller("Obj_HammerSwitch_A_01"));
    // The walk hands all of them to WorldSync (the pillars were missing there).
    for (const char* name : {kKey, kPillar, "Obj_HammerSwitch_A_01", "Obj_Pot_A_01", kBox}) {
        CAPTURE(name);
        CHECK(CoopWorld::IsWorldObject(name));
    }
    CHECK_FALSE(CoopWorld::IsWorldObject("Obj_TBox_A_01"));
    CHECK_FALSE(CoopWorld::IsWorldObject("Obj_LakeHrl_Ice_A_01"));  // scenery: no life, no destruction

    CHECK(CoopWorld::KeyCountOf("BottomOfTheWell") == "DungeonInfo.BottomOfTheWell.SmallKeyNum");
    CHECK(CoopWorld::KeyCountOf("ShadowTemple") == "DungeonInfo.ShadowTemple.SmallKeyNum");
    CHECK(CoopWorld::KeyCountOf("GerudoFortress_Adult") == "DungeonInfo.GerudoFortress.SmallKeyNum");
    CHECK_FALSE(CoopWorld::KeyCountOf("HyruleField_Child"));
}

TEST_CASE("world: a small key taken here goes out as a break, one taken elsewhere goes here at once") {
    // Taken here: gone next to our Link, sent after the hold, nothing given back.
    WorldSync a;
    WellWalk(a, 1000 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey)});
    std::vector<proto::WorldBreak> sent;
    WorldSync::Out out;
    for (std::uint64_t t = 1100; t <= 2500; t += 100) {
        out = WellWalk(a, t * kMs, {});
        for (const auto& b : Sent<proto::WorldBreak>(out)) {
            sent.push_back(b);
        }
        CHECK(out.give_back.empty());
    }
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].hash == 0x77);
    CHECK(sent[0].scene == kWell);

    // Taken elsewhere while ours lies there: deleted on the next walk, no wait.
    WorldSync b;
    WellWalk(b, 1000 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey)});
    b.Receive(KeyBreak(0x77));
    out = WellWalk(b, 1100 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey)});
    CHECK(Removed(b, 0x100));
    CHECK(out.give_back.empty());
    CHECK(out.lives.empty());
    // Our delete makes it leave next to our Link: that is not our take.
    for (std::uint64_t t = 1200; t <= 3000; t += 100) {
        out = WellWalk(b, t * kMs, {});
        CHECK(Sent<proto::WorldBreak>(out).empty());
        CHECK(out.give_back.empty());
    }

    // A game that loads the room after the take: its key goes too.
    WorldSync c;
    c.Receive(KeyBreak(0x77));
    WellWalk(c, 5000 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey)});
    CHECK(Removed(c, 0x100));
}

TEST_CASE("world: both players take the same small key at once: one key goes back, once") {
    // (1) Ours went out first, theirs came after: the server keeps the first
    // break only and relayed theirs, so theirs was first. One key back.
    WorldSync a;
    WellWalk(a, 1000 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey)});
    bool sent = false;
    for (std::uint64_t t = 1100; t <= 2500; t += 100) {
        sent = sent || !Sent<proto::WorldBreak>(WellWalk(a, t * kMs, {})).empty();
    }
    REQUIRE(sent);
    a.Receive(KeyBreak(0x77));
    auto out = WellWalk(a, 2600 * kMs, {});
    REQUIRE(out.give_back.size() == 1);
    CHECK(out.give_back[0] == "DungeonInfo.BottomOfTheWell.SmallKeyNum");
    a.Receive(KeyBreak(0x77));  // a repeat changes nothing
    CHECK(WellWalk(a, 2700 * kMs, {}).give_back.empty());

    // (2) Theirs comes while ours waits out the hold: ours is never sent, one key back.
    WorldSync b;
    WellWalk(b, 1000 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey)});
    WellWalk(b, 1100 * kMs, {});
    WellWalk(b, 1200 * kMs, {});  // gone two walks: held as a take
    b.Receive(KeyBreak(0x77));
    out = WellWalk(b, 1300 * kMs, {});
    REQUIRE(out.give_back.size() == 1);
    for (std::uint64_t t = 1400; t <= 3000; t += 100) {
        out = WellWalk(b, t * kMs, {});
        CHECK(Sent<proto::WorldBreak>(out).empty());
        CHECK(out.give_back.empty());
    }

    // (3) Theirs comes when our key has just left the walk (one walk missed).
    WorldSync c;
    WellWalk(c, 1000 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey)});
    WellWalk(c, 1100 * kMs, {});
    c.Receive(KeyBreak(0x77));
    out = WellWalk(c, 1200 * kMs, {});
    CHECK(out.give_back.size() == 1);

    // (4) A key far from our Link that leaves the walk was not ours to take.
    WorldSync d;
    const proto::Vec3 far{1.0f, 2.0f, 40.0f};
    WellWalk(d, 1000 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey, far)});
    WellWalk(d, 1100 * kMs, {});
    d.Receive(KeyBreak(0x77));
    CHECK(WellWalk(d, 1200 * kMs, {}).give_back.empty());
}

TEST_CASE("world: a key given back lowers the dungeon's count by one, never below 0") {
    const std::uint32_t hash = proto::flag_hash("DungeonInfo.BottomOfTheWell.SmallKeyNum");
    CountStore game;
    game.values[hash] = 2;
    const std::string count = *CoopWorld::KeyCountOf(kWell);
    const std::string line = CoopWorld::GiveBack(game, count);
    CHECK(game.values[hash] == 1);
    CHECK(line.find("2 -> 1") != std::string::npos);
    game.values[hash] = 0;
    CoopWorld::GiveBack(game, count);
    CHECK(game.values[hash] == 0);
    CHECK(CoopWorld::GiveBack(game, "").find("no count") != std::string::npos);

    // The queue between the threads.
    CoopWorld::QueueGiveBack("ShadowTemple");
    CoopWorld::QueueGiveBack("BottomOfTheWell");
    CHECK(CoopWorld::TakeGiveBacks() == std::vector<std::string>{"ShadowTemple", "BottomOfTheWell"});
    CHECK(CoopWorld::TakeGiveBacks().empty());
}

namespace {
const char* const kLadder = "Obj_InsideTheDekuTree_FallingLadder_A_01";
// Turned `angle` radians about x.
Seen Tipped(std::uint64_t address, std::uint64_t hash, const char* name, float angle,
            proto::Vec3 at = {1.0f, 2.0f, 3.0f}) {
    Seen s = Thing(address, hash, std::nullopt, name, at);
    const float c = std::cos(angle), n = std::sin(angle);
    s.rotation = CoopObjects::Mat3{1, 0, 0, 0, c, -n, 0, n, c};
    return s;
}
} // namespace

TEST_CASE("world: a shifter that only turns is sent where it rests, and held there in the others") {
    // Ours: the ladder tips over on its foot (no move), 10 m off (a slingshot shot).
    WorldSync a;
    const proto::Vec3 link{1.0f, 2.0f, 13.0f};
    Walk(a, 1000 * kMs, {Tipped(0x100, 0x31, kLadder, 0.0f)}, link);
    std::vector<proto::WorldPlace> places;
    for (int i = 1; i <= 15; ++i) {
        const float angle = std::min(1.5f, 0.3f * static_cast<float>(i));
        const auto out = Walk(a, static_cast<std::uint64_t>(1000 + 100 * i) * kMs,
                              {Tipped(0x100, 0x31, kLadder, angle)}, link);
        for (const auto& p : Sent<proto::WorldPlace>(out)) {
            places.push_back(p);
        }
        for (const auto& v : Sent<proto::WorldMove>(out)) {
            CHECK(v.flags == 0);  // never "carried"
        }
    }
    REQUIRE(places.size() == 1);
    CHECK(places[0].hash == 0x31);
    // Its place's turn is the tipped one (about x by 1.5).
    CHECK(std::abs(places[0].rotation[0]) == doctest::Approx(std::sin(0.75f)).epsilon(0.01));

    // A box that only turns is no move (shifters only).
    WorldSync box;
    Walk(box, 1000 * kMs, {Tipped(0x100, 0x50, kBox, 0.0f)});
    CHECK(Walk(box, 1100 * kMs, {Tipped(0x100, 0x50, kBox, 0.5f)}).send.empty());

    // Theirs: put where it rests and held there long after (the game would put it back).
    WorldSync b;
    Walk(b, 1000 * kMs, {Tipped(0x100, 0x31, kLadder, 0.0f)});
    b.Receive(places[0]);
    Walk(b, 2000 * kMs, {Tipped(0x100, 0x31, kLadder, 0.0f)});
    for (std::uint64_t t = 2100; t <= 20000; t += 1000) {
        CHECK(Walk(b, t * kMs, {Tipped(0x100, 0x31, kLadder, 0.0f)}).send.empty());
        std::vector<CoopObjects::PoseWrite> poses;
        b.Poses(t * kMs, poses);
        REQUIRE(poses.size() == 1);
        CHECK(poses[0].address == 0x100);
        REQUIRE(poses[0].rotation);
        CHECK((*poses[0].rotation)[4] == doctest::Approx(std::cos(1.5f)).epsilon(0.01));
    }

    // A game that loads the room later gets the server's place: held as well.
    WorldSync c;
    c.Receive(places[0]);
    Walk(c, 1000 * kMs, {Tipped(0x100, 0x31, kLadder, 0.0f)});
    Walk(c, 9000 * kMs, {Tipped(0x100, 0x31, kLadder, 0.0f)});
    std::vector<CoopObjects::PoseWrite> poses;
    c.Poses(9000 * kMs, poses);
    CHECK(poses.size() == 1);
}

TEST_CASE("world: a shifter hit from across the room is sent, a box that far is not ours (issue 176)") {
    // Issue 176: the slingshot room's eye is shot from the chest ledge, 17 m from
    // the ladder; the ladder only fell for the shooter.
    const proto::Vec3 ledge{1.0f, 2.0f, 20.0f};  // 17 m from {1, 2, 3}
    WorldSync a;
    Walk(a, 1000 * kMs, {Tipped(0x100, 0x31, kLadder, 0.0f)}, ledge);
    bool sent = false;
    for (int i = 1; i <= 15; ++i) {
        const float angle = std::min(1.5f, 0.3f * static_cast<float>(i));
        const auto out = Walk(a, static_cast<std::uint64_t>(1000 + 100 * i) * kMs,
                              {Tipped(0x100, 0x31, kLadder, angle)}, ledge);
        sent = sent || !out.send.empty();
    }
    CHECK(sent);
    // The bridge's keyed floor sinks 17 m off: sent too.
    const char* bridge = "Obj_InsideTheDekuTree_FallingFloor_A_01";
    WorldSync f;
    Walk(f, 1000 * kMs, {Thing(0x200, 0x51, std::nullopt, bridge)}, ledge);
    const auto fell = Walk(f, 1100 * kMs, {Thing(0x200, 0x51, std::nullopt, bridge, {1.0f, 1.0f, 3.0f})}, ledge);
    CHECK_FALSE(Sent<proto::WorldMove>(fell).empty());
    // Beyond 40 m: not ours.
    WorldSync far;
    const proto::Vec3 away{1.0f, 2.0f, 50.0f};
    Walk(far, 1000 * kMs, {Tipped(0x100, 0x31, kLadder, 0.0f)}, away);
    CHECK(Walk(far, 1100 * kMs, {Tipped(0x100, 0x31, kLadder, 0.6f)}, away).send.empty());
    // A box moved 17 m from our Link is not ours to send (another game moves it).
    WorldSync box;
    Walk(box, 1000 * kMs, {Thing(0x300, 0x61, std::nullopt, kBox)}, ledge);
    CHECK(Walk(box, 1100 * kMs, {Thing(0x300, 0x61, std::nullopt, kBox, {1.5f, 2.0f, 3.0f})}, ledge).send.empty());
}

TEST_CASE("world: a hammer drop floor that sinks is sent and held, a hammer switch is pressed by life 0") {
    const char* floor = "Obj_FireTemple_HammerDropFloor_A_01";
    WorldSync a;
    Walk(a, 1000 * kMs, {Thing(0x100, 0x41, std::nullopt, floor)});
    std::vector<proto::WorldPlace> places;
    for (int i = 1; i <= 12; ++i) {
        const float y = std::max(0.0f, 2.0f - 0.5f * static_cast<float>(i));
        const auto out = Walk(a, static_cast<std::uint64_t>(1000 + 100 * i) * kMs,
                              {Thing(0x100, 0x41, std::nullopt, floor, {1.0f, y, 3.0f})});
        for (const auto& p : Sent<proto::WorldPlace>(out)) {
            places.push_back(p);
        }
    }
    REQUIRE(places.size() == 1);
    CHECK(places[0].position[1] == doctest::Approx(0.0f));

    // The switch: another player's press writes life 0, it is never deleted.
    const char* sw = "Obj_HammerSwitch_A_01";
    WorldSync b;
    Walk(b, 1000 * kMs, {Thing(0x200, 0x42, 1, sw)});
    Walk(b, 5000 * kMs, {Thing(0x200, 0x42, 1, sw)});
    b.Receive(BreakOf(0x42));
    const auto out = Walk(b, 5100 * kMs, {Thing(0x200, 0x42, 1, sw)});
    REQUIRE(out.lives.size() == 1);
    CHECK(out.lives[0].life == 0);
    for (std::uint64_t t = 5200; t < 9000; t += 100) {
        Walk(b, t * kMs, {Thing(0x200, 0x42, 0, sw)});
    }
    CHECK(b.Removes().empty());
}

// ------------------------------------------------- Gold Skulltula tokens (#89)

namespace {
const char* const kGold = "Enemy_Skulltula_Gold";
const char* const kToken = "Obj_Token_Gold";
const std::string kGrave = "Spot_GraveYard";
constexpr std::uint64_t kGoldHash = 0xf2e27d6a27026acfULL;
const proto::Vec3 kGoldAt{1.0f, 3.0f, 3.0f};  // on a wall one metre up

WorldSync::Out GraveWalk(WorldSync& w, std::uint64_t now, const std::vector<Seen>& seen,
                         const std::string& scene = kGrave) {
    return Walk(w, now, seen, kLink, 2, scene);
}
Seen Token(std::uint64_t address, proto::Vec3 at = {1.0f, 2.5f, 3.0f}) {
    return Thing(address, 0, std::nullopt, kToken, at);  // made by the game: no placement
}
} // namespace

TEST_CASE("world: a Gold Skulltula's token is known by its Skulltula, taken here it goes out once") {
    CHECK(CoopWorld::IsGoldToken(kToken));
    CHECK(CoopWorld::IsWorldObject(kToken));
    CHECK_FALSE(CoopWorld::IsGoldToken(kGold));
    CHECK(CoopWorld::TokenId(kGoldHash) != kGoldHash);
    CHECK(CoopWorld::TokenId(kGoldHash) != 0);

    WorldSync a;
    GraveWalk(a, 1000 * kMs, {Thing(0x100, kGoldHash, 1, kGold, kGoldAt)});
    // It dies; the game makes the token where it was.
    GraveWalk(a, 1100 * kMs, {Token(0x200)});
    std::vector<proto::WorldBreak> sent;
    for (std::uint64_t t = 1200; t <= 2700; t += 100) {
        const auto out = GraveWalk(a, t * kMs, {});
        for (const auto& b : Sent<proto::WorldBreak>(out)) {
            sent.push_back(b);
        }
        CHECK(out.give_back.empty());
        CHECK(out.taken_keys.empty());  // our own game set its key itself
    }
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].hash == CoopWorld::TokenId(kGoldHash));
}

TEST_CASE("world: a token taken elsewhere goes at once here, its Skulltula's key is set, and a reload does not bring it back") {
    WorldSync b;
    GraveWalk(b, 1000 * kMs, {Thing(0x100, kGoldHash, 1, kGold, kGoldAt)});
    GraveWalk(b, 1100 * kMs, {Token(0x200)});  // the kill was shared: our copy left a token too
    proto::WorldBreak taken = BreakOf(CoopWorld::TokenId(kGoldHash), 0, kGrave);
    taken.position = {1.0f, 2.5f, 3.0f};
    b.Receive(taken);
    auto out = GraveWalk(b, 1200 * kMs, {Token(0x200)});
    CHECK(Removed(b, 0x200));
    REQUIRE(out.taken_keys.size() == 1);
    CHECK(out.taken_keys[0] == kGoldHash);  // the persistent "taken" state
    CHECK(out.give_back.empty());
    for (std::uint64_t t = 1300; t <= 3000; t += 100) {
        out = GraveWalk(b, t * kMs, {});
        CHECK(Sent<proto::WorldBreak>(out).empty());  // our delete is not our take
        CHECK(out.give_back.empty());
    }

    // B leaves and comes back: no Skulltula now, the game makes the token again
    // at the same spot; the server's replay of the scene's breaks deletes it.
    GraveWalk(b, 4000 * kMs, {}, "KakarikoVillage_Child");
    b.Receive(taken);  // the server replays the scene's breaks on entering
    GraveWalk(b, 5000 * kMs, {Token(0x300, {1.2f, 2.5f, 3.0f})});
    CHECK(Removed(b, 0x300));

    // A token that turns up far from any known Skulltula or token spot is left alone.
    WorldSync c;
    GraveWalk(c, 1000 * kMs, {Token(0x400, {40.0f, 2.0f, 3.0f})});
    for (std::uint64_t t = 1100; t <= 2500; t += 100) {
        CHECK(Sent<proto::WorldBreak>(GraveWalk(c, t * kMs, {})).empty());
    }
}

TEST_CASE("world: both players take the same token at once: one token goes back") {
    WorldSync a;
    GraveWalk(a, 1000 * kMs, {Thing(0x100, kGoldHash, 1, kGold, kGoldAt)});
    GraveWalk(a, 1100 * kMs, {Token(0x200)});
    GraveWalk(a, 1200 * kMs, {});
    GraveWalk(a, 1300 * kMs, {});  // taken: held
    a.Receive(BreakOf(CoopWorld::TokenId(kGoldHash), 0, kGrave));
    const auto out = GraveWalk(a, 1400 * kMs, {});
    REQUIRE(out.give_back.size() == 1);
    CHECK(out.give_back[0] == CoopWorld::kTokenCount);
    for (std::uint64_t t = 1500; t <= 3000; t += 100) {
        CHECK(Sent<proto::WorldBreak>(GraveWalk(a, t * kMs, {})).empty());
    }
}

TEST_CASE("world: a token taken elsewhere sets its Skulltula's key, once") {
    const std::uint32_t tokens = proto::flag_hash(CoopWorld::kTokenCount);
    CountStore game;
    game.values[tokens] = 5;
    CHECK(CoopWorld::GiveBack(game, CoopWorld::kTokenCount).find("5 -> 4") != std::string::npos);
    CHECK(game.values[tokens] == 4);
    // SetTakenKey writes a 64-bit key: the store sees it under its key.
    struct KeyStore final : public coop::host::FlagStore {
        std::map<std::uint64_t, std::uint32_t> keys;
        std::optional<proto::FlagEntry> read(coop::host::FlagKey k) override {
            const auto it = keys.find(k.key);
            if (it == keys.end()) return proto::FlagEntry(k, proto::FlagKind::boolean, 0);
            return proto::FlagEntry(k, proto::FlagKind::boolean, it->second);
        }
        bool write(const proto::FlagEntry& e) override {
            keys[e.key.key] = e.value;
            return true;
        }
    } store;
    CHECK(CoopWorld::SetTakenKey(store, kGoldHash).find("set to 1") != std::string::npos);
    CHECK(store.keys[kGoldHash] == 1);
    CHECK(CoopWorld::SetTakenKey(store, kGoldHash).find("1 already") != std::string::npos);
    CoopWorld::QueueTakenKey(kGoldHash);
    CHECK(CoopWorld::TakeTakenKeys() == std::vector<std::uint64_t>{kGoldHash});
    CHECK(CoopWorld::TakeTakenKeys().empty());
}

TEST_CASE("world: a pillar that does not fall on life 0 is written 10 times, then one line, then left (breakables)") {
    WorldSync b;
    Walk(b, 1000 * kMs, {Thing(0x100, 0x11, 1, kPillar)});
    b.Receive(BreakOf(0x11));
    int writes = 0;
    int gave_up = 0;
    for (std::uint64_t t = 5000; t < 40000; t += 100) {
        const auto out = Walk(b, t * kMs, {Thing(0x100, 0x11, 1, kPillar)});  // never falls
        writes += static_cast<int>(out.lives.size());
        for (const auto& line : out.notes) {
            if (line.find("did not fall on life 0") != std::string::npos) ++gave_up;
        }
        CHECK(out.send.empty());
    }
    CHECK(writes == WorldSync::kFallerTries);
    CHECK(gave_up == 1);
    CHECK(b.Removes().empty());
}

TEST_CASE("world: a small key still on the walk but lifted next to our Link when theirs comes: one back") {
    // Review of breakables-2: B is picking the key up (the game lifts it to
    // Link's hands) when A's break arrives; B's count rises too.
    WorldSync b;
    WellWalk(b, 1000 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey)});
    WellWalk(b, 1100 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey, {1.0f, 2.6f, 3.0f})});  // lifted 0.6 m
    b.Receive(KeyBreak(0x77));
    auto out = WellWalk(b, 1200 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey, {1.0f, 2.7f, 3.0f})});
    REQUIRE(out.give_back.size() == 1);
    CHECK(out.give_back[0] == "DungeonInfo.BottomOfTheWell.SmallKeyNum");
    for (std::uint64_t t = 1300; t <= 3000; t += 100) {
        out = WellWalk(b, t * kMs, {});
        CHECK(out.give_back.empty());
        CHECK(Sent<proto::WorldBreak>(out).empty());
    }

    // Lying where it was, next to our Link: not being taken, nothing back.
    WorldSync c;
    WellWalk(c, 1000 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey)});
    c.Receive(KeyBreak(0x77));
    out = WellWalk(c, 1100 * kMs, {Thing(0x100, 0x77, std::nullopt, kKey, {1.0f, 2.1f, 3.0f})});
    CHECK(out.give_back.empty());
    CHECK(Removed(c, 0x100));
}

TEST_CASE("world: a falling floor with no AI settles at another game's place, it is not held for good") {
    const char* floor = "Obj_InsideTheDekuTree_FallingFloor_A_03";
    CHECK(CoopWorld::IsShifter(floor));
    CHECK_FALSE(CoopWorld::HoldsPlace(floor));
    CHECK_FALSE(CoopWorld::HoldsPlace("Obj_ShadowTemple_FallingFloor_A_03"));
    CHECK(CoopWorld::HoldsPlace(kLadder));
    CHECK(CoopWorld::HoldsPlace("Obj_FireTemple_HammerDropPillar_A_01"));

    WorldSync b;
    Walk(b, 1000 * kMs, {Thing(0x100, 0x61, std::nullopt, floor)});
    b.Receive(PlaceOf(0x61, 1.0f));
    Walk(b, 2000 * kMs, {Thing(0x100, 0x61, std::nullopt, floor)});
    std::vector<CoopObjects::PoseWrite> poses;
    b.Poses(2100 * kMs, poses);
    CHECK(poses.size() == 1);  // put there
    Walk(b, 3000 * kMs, {Thing(0x100, 0x61, std::nullopt, floor)});
    poses.clear();
    b.Poses(3000 * kMs, poses);
    CHECK(poses.empty());  // then left to the game
}

// Issue 147: a block another game pushed keeps its old rest place in its own
// memory; the follower gives the glue a fix, and the base anchor is moved.
TEST_CASE("world: a block another player pushed asks for its old rest place to be rewritten (issue 147)") {
    CHECK(CoopWorld::IsBlockAnchor("Obj_MovableBox_BasePosAnchor"));
    CHECK_FALSE(CoopWorld::IsMovable("Obj_MovableBox_BasePosAnchor"));
    CHECK_FALSE(CoopWorld::IsBlockAnchor(kBox));
    const auto anchor = [](proto::Vec3 at) {
        return Thing(0x900, 0, std::nullopt, "Obj_MovableBox_BasePosAnchor", at);
    };
    WorldSync w;
    Walk(w, 1000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox), anchor({1.0f, 2.0f, 3.0f})});
    w.Receive(MoveOf(0x50, 2000 * kMs, 1.0f));
    w.Receive(MoveOf(0x50, 2100 * kMs, 2.0f));
    auto out = Walk(w, 2100 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox), anchor({1.0f, 2.0f, 3.0f})});
    CHECK(out.blocks.empty());  // still moving
    w.Receive(PlaceOf(0x50, 2.5f));
    Walk(w, 2300 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {2.0f, 2.0f, 3.0f}), anchor({1.0f, 2.0f, 3.0f})});
    CHECK(Walk(w, 2700 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {2.5f, 2.0f, 3.0f}),
                               anchor({1.0f, 2.0f, 3.0f})}).blocks.empty());  // held at its rest
    out = Walk(w, 2900 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {2.5f, 2.0f, 3.0f}), anchor({1.0f, 2.0f, 3.0f})});
    REQUIRE(out.blocks.size() == 1);
    const auto& fix = out.blocks[0];
    CHECK(fix.address == 0x100);
    CHECK(fix.hash == 0x50);
    CHECK(fix.from[0] == doctest::Approx(1.0f));
    CHECK(fix.to[0] == doctest::Approx(2.5f));
    CHECK(fix.anchor == 0x900);
    // The anchor is put at the new rest place for a while, then let be.
    std::vector<CoopObjects::PoseWrite> poses;
    w.Poses(3000 * kMs, poses);
    REQUIRE(poses.size() == 1);
    CHECK(poses[0].address == 0x900);
    CHECK(poses[0].position[0] == doctest::Approx(2.5f));
    CHECK_FALSE(poses[0].rotation);
    Walk(w, 4000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {2.5f, 2.0f, 3.0f}), anchor({2.5f, 2.0f, 3.0f})});
    poses.clear();
    w.Poses(4000 * kMs, poses);
    CHECK(poses.empty());
    // Once only; and our own push next makes none.
    CHECK(Walk(w, 4100 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {2.5f, 2.0f, 3.0f})}).blocks.empty());
    out = Walk(w, 6000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {3.0f, 2.0f, 3.0f})}, proto::Vec3{3.0f, 2.0f, 4.0f});
    CHECK(Sent<proto::WorldMove>(out).size() == 1);
    CHECK(out.blocks.empty());
    out = Walk(w, 6600 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {3.0f, 2.0f, 3.0f})}, proto::Vec3{3.0f, 2.0f, 4.0f});
    CHECK(Sent<proto::WorldPlace>(out).size() == 1);
    CHECK(out.blocks.empty());
}

TEST_CASE("world: a block first seen after it was moved is fixed from where it was made (issue 147)") {
    WorldSync w;
    Walk(w, 1000 * kMs, {});
    w.Receive(PlaceOf(0x50, 7.0f));  // the server's replay as we entered
    Walk(w, 30000 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox)});
    Walk(w, 30200 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {7.0f, 2.0f, 3.0f})});
    const auto out = Walk(w, 30600 * kMs, {Thing(0x100, 0x50, std::nullopt, kBox, {7.0f, 2.0f, 3.0f})});
    REQUIRE(out.blocks.size() == 1);
    CHECK(out.blocks[0].from[0] == doctest::Approx(1.0f));
    CHECK(out.blocks[0].to[0] == doctest::Approx(7.0f));
    CHECK(out.blocks[0].anchor == 0);  // none seen
}

TEST_CASE("world: a pot carried by another player asks for no block fix") {
    WorldSync w;
    Walk(w, 1000 * kMs, {Thing(0x100, 0x51, 1, "Obj_Pot_A_01")});
    w.Receive(MoveOf(0x51, 2000 * kMs, 1.0f));
    w.Receive(PlaceOf(0x51, 4.0f));
    Walk(w, 2100 * kMs, {Thing(0x100, 0x51, 1, "Obj_Pot_A_01", {3.0f, 2.0f, 3.0f})});
    const auto out = Walk(w, 3000 * kMs, {Thing(0x100, 0x51, 1, "Obj_Pot_A_01", {4.0f, 2.0f, 3.0f})});
    CHECK(out.blocks.empty());
}

namespace {

void PutFloats(FakeGame& g, std::uint64_t at, std::initializer_list<float> v, std::size_t stride = 4) {
    std::size_t i = 0;
    for (const float f : v) {
        g.Put(at + stride * i++, &f, 4);
    }
}

float FloatAt(const FakeGame& g, std::uint64_t at) {
    std::uint8_t b[4];
    for (int i = 0; i < 4; ++i) {
        b[i] = g.bytes.at(at + i);
    }
    float f = 0.0f;
    std::memcpy(&f, b, 4);
    return f;
}

// A block's memory: the actor, its component table, one component and the
// physics root, all zero.
constexpr std::uint64_t kBlockActor = 0x2100100000, kBlockTable = 0x2100200000,
                        kBlockComp = 0x2100300000, kBlockPhysics = 0x2100400000,
                        kBlockRoot = 0x2100500000;
void BlockMemory(FakeGame& g) {
    for (std::uint64_t a = 0; a < 0x600; a += 8) g.Word(kBlockActor + a, 0);
    for (std::uint64_t a = 0; a < 0x300; a += 8) g.Word(kBlockTable + a, 0);
    for (std::uint64_t a = 0; a < 0x200; a += 8) g.Word(kBlockComp + a, 0);
    for (std::uint64_t a = 0; a < 0x200; a += 8) g.Word(kBlockPhysics + a, 0);
    for (std::uint64_t a = 0; a < 0x400; a += 8) g.Word(kBlockRoot + a, 0);
    g.Word(kBlockActor + 0x270, kBlockTable);
    g.Word(kBlockTable + 0x10, kBlockComp);
    g.Word(kBlockTable + 0x50, kBlockPhysics);
    g.Word(kBlockPhysics + 0x20, kBlockRoot);
}

} // namespace

TEST_CASE("world: RestWords puts the new rest place where the block's memory keeps the old (issue 147)") {
    FakeGame g;
    BlockMemory(g);
    PutFloats(g, kBlockActor + 0x31c, {10.0f, 20.0f, 30.0f});  // its own place: the setter's
    PutFloats(g, kBlockActor + 0x100, {10.0f, 20.0f, 30.0f});  // a remembered place
    PutFloats(g, kBlockComp + 0x40, {10.01f, 20.0f, 29.99f}, 16);  // a 3x4 matrix's translation
    PutFloats(g, kBlockRoot + 0x1d0, {10.0f, 20.0f, 30.0f});   // the physics root
    PutFloats(g, kBlockActor + 0x200, {10.0f, 20.0f, 31.0f});  // not the place (z off)
    const proto::Vec3 from{10.0f, 20.0f, 30.0f}, to{12.0f, 20.0f, 30.0f};
    const auto scan = CoopWorld::RestWords(g.Reader(), kBlockActor, from, to);
    CHECK_FALSE(scan.too_many);
    REQUIRE(scan.where.size() == 3);
    CHECK(scan.where[0] == "actor+0x100");
    CHECK(scan.where[1] == "c+0x10+0x40 (matrix)");
    CHECK(scan.where[2] == "physics root+0x1d0");
    REQUIRE(scan.writes.size() == 9);
    for (const auto& w : scan.writes) {
        g.Put(w.address, &w.value, 4);
    }
    CHECK(FloatAt(g, kBlockActor + 0x100) == 12.0f);
    CHECK(FloatAt(g, kBlockActor + 0x104) == 20.0f);
    CHECK(FloatAt(g, kBlockComp + 0x40) == 12.0f);
    CHECK(FloatAt(g, kBlockComp + 0x50) == 20.0f);
    CHECK(FloatAt(g, kBlockComp + 0x60) == 30.0f);
    CHECK(FloatAt(g, kBlockRoot + 0x1d0) == 12.0f);
    CHECK(FloatAt(g, kBlockActor + 0x31c) == 10.0f);  // left alone
    CHECK(FloatAt(g, kBlockActor + 0x200) == 10.0f);  // not a match

    // No real move, a place near the origin (zeros would match), or too many: nothing.
    CHECK(CoopWorld::RestWords(g.Reader(), kBlockActor, from, {10.01f, 20.0f, 30.0f}).writes.empty());
    FakeGame z;
    BlockMemory(z);
    CHECK(CoopWorld::RestWords(z.Reader(), kBlockActor, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}).writes.empty());
    FakeGame many;
    BlockMemory(many);
    for (std::uint64_t i = 0; i <= CoopWorld::kMaxRestTriplets; ++i) {
        PutFloats(many, kBlockActor + 0x20 * i, {10.0f, 20.0f, 30.0f});
    }
    const auto lots = CoopWorld::RestWords(many.Reader(), kBlockActor, from, to);
    CHECK(lots.too_many);
    CHECK(lots.writes.empty());
}

TEST_CASE("world: only push blocks get a rest fix, not an Armos statue (issue 147 review)") {
    CHECK(CoopWorld::IsPushBlock(kBox));
    CHECK(CoopWorld::IsPushBlock("Obj_MovableBox_A_01"));
    CHECK(CoopWorld::IsPushBlock("Obj_SliderBox_A_01"));
    CHECK_FALSE(CoopWorld::IsPushBlock("Obj_AmosStatue_A_01"));
    CHECK_FALSE(CoopWorld::IsPushBlock("Obj_MovableBox_BasePosAnchor"));
    for (const char* name : {"Obj_AmosStatue_A_01", "Obj_SliderBox_A_01"}) {
        CAPTURE(name);
        WorldSync w;
        Walk(w, 1000 * kMs, {Thing(0x100, 0x50, std::nullopt, name)});
        w.Receive(PlaceOf(0x50, 4.0f));
        Walk(w, 1100 * kMs, {Thing(0x100, 0x50, std::nullopt, name, {4.0f, 2.0f, 3.0f})});
        const auto out = Walk(w, 1700 * kMs, {Thing(0x100, 0x50, std::nullopt, name, {4.0f, 2.0f, 3.0f})});
        CHECK(out.blocks.size() == (std::string_view(name).starts_with("Obj_SliderBox_") ? 1u : 0u));
    }
}

TEST_CASE("world: RestWords skips exact zeros and finds a place ending on a region's last float") {
    FakeGame g;
    BlockMemory(g);
    // The old place has a 0.0 axis: zeros everywhere would match, so no match is taken.
    PutFloats(g, kBlockActor + 0x100, {10.0f, 0.0f, 30.0f});
    CHECK(CoopWorld::RestWords(g.Reader(), kBlockActor, {10.0f, 0.0f, 30.0f}, {12.0f, 0.0f, 30.0f})
              .where.empty());
    // A place whose last float is the component's last (0x1fc of 0x200) is found.
    PutFloats(g, kBlockComp + 0x1f4, {10.0f, 20.0f, 30.0f});
    const auto scan =
        CoopWorld::RestWords(g.Reader(), kBlockActor, {10.0f, 20.0f, 30.0f}, {12.0f, 20.0f, 30.0f});
    REQUIRE(scan.where.size() == 1);
    CHECK(scan.where[0] == "c+0x10+0x1f4");
}
