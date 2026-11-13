// SPDX-License-Identifier: GPL-3.0-or-later
// The shared-enemies port on a stand-in game (fake_game.hpp): what the
// emulator's object files get from the plugin interface, the frame step hook
// and its game calls, and the AI hold's hooks.
#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <thread>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_hold.h"
#include "coop_objects.h"
#include "coop_objects_sync.h"
#include "coop_svc.h"
#include "core/core.h"
#include "emu_port.hpp"
#include "brain.hpp"
#include "fake_game.hpp"
#include "hold.hpp"
#include "objects.hpp"
#include "scene_gate.hpp"

namespace Objects = CoopNx2navi::Objects;
using FakeGame::kMain;

namespace {

std::vector<std::string> g_lines;
void Collect(const char* line) { g_lines.emplace_back(line); }

bool Logged(std::string_view part) {
    for (const auto& l : g_lines) {
        if (l.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

Objects::Options On(int ai_hold = 0) {
    Objects::Options o;
    o.on = true;
    o.ai_hold = ai_hold;
    return o;
}

}  // namespace

TEST_CASE("objects port: off by default, and nothing happens") {
    FakeGame::Game game;
    CHECK_FALSE(Objects::Init(game.Api(), Objects::Options{}, Collect));
    CHECK_FALSE(Objects::Active());
    Objects::FrameInput in;
    in.main_base = kMain;
    Objects::Frame(in);
    CHECK(game.hooks.empty());
    CHECK(Objects::StatusText().empty());
    Objects::Shutdown();
}

TEST_CASE("objects port: townspeople and animals are each game's own unless objects_friendly asks") {
    // Their copies are not held (only enemy copies are), so shared they could
    // not be talked to, ridden or milked in the game that does not own them.
    FakeGame::Game game;
    g_lines.clear();
    REQUIRE(Objects::Init(game.Api(), On(3), Collect));
    CHECK(Logged("each game its own townspeople and animals"));
    for (const char* name : {"Npc_Malon_Adult", "Npc_Ingo", "Animal_Horse", "Animal_Cow", "Animal_Cucco",
                             "Animal_Fish_Small"}) {
        CAPTURE(name);
        CHECK_FALSE(CoopObjects::IsShared(name));
    }
    for (const char* name : {"Enemy_Stalchild", "Enemy_Stalchild_Big", "Enemy_DungeonBoss_Gohma", "Enemy_Poe"}) {
        CAPTURE(name);
        CHECK(CoopObjects::IsShared(name));
    }
    Objects::Shutdown();
    Objects::Options friendly = On(3);
    friendly.friendly = true;
    g_lines.clear();
    REQUIRE(Objects::Init(game.Api(), friendly, Collect));
    CHECK(Logged("townspeople and animals too"));
    CHECK(CoopObjects::IsShared("Npc_Malon_Adult"));
    CHECK(CoopObjects::IsShared("Animal_Horse"));
    CHECK_FALSE(CoopObjects::IsShared("Npc_Navi"));  // the emulator build's own exceptions stay
    Objects::Shutdown();
    CoopObjects::ShareFriendly(true);  // the hook files' default, for the other cases
}

TEST_CASE("objects port: an unreadable frame is not a scene change (run s1: enemies shared twice)") {
    using Objects::SceneGate;
    const std::optional<std::string> field = std::string("HyruleField_Adult");
    const std::optional<std::string> none;
    std::optional<std::string> out;
    SceneGate gate;
    // Before any scene: "no scene" goes through (the title screen), from either thread.
    CHECK(gate.Pass(false, none, 0, out));
    CHECK_FALSE(out.has_value());
    CHECK(gate.Pass(true, none, 1'000, out));
    CHECK_FALSE(out.has_value());
    CHECK(gate.Pass(true, field, 2'000, out));
    CHECK(out == field);
    // The game's thread made no frame for a second: our own thread's frame has
    // no reading. The objects are left alone, however long the game stands.
    for (std::uint64_t t = 1'002'000; t < 61'000'000; t += 16'000) {
        REQUIRE_FALSE(gate.Pass(false, none, t, out));
    }
    CHECK(gate.TakeHeld() > 3000);
    CHECK(gate.Pass(true, field, 61'000'000, out));  // it runs again: the same scene, nothing forgotten
    CHECK(out == field);
    // One failed reading on the game's own thread: left out.
    CHECK_FALSE(gate.Pass(true, none, 61'033'000, out));
    CHECK(gate.Pass(true, field, 61'066'000, out));
    // A real load: unreadable for longer than a blip. Passed on as "no scene",
    // once the blip time is over, and from then on every frame.
    CHECK_FALSE(gate.Pass(true, none, 61'100'000, out));
    CHECK_FALSE(gate.Pass(true, none, 61'066'000 + SceneGate::kBlipUs - 1, out));
    CHECK(gate.Pass(true, none, 61'066'000 + SceneGate::kBlipUs, out));
    CHECK_FALSE(out.has_value());
    CHECK(gate.Pass(true, none, 62'000'000, out));
    CHECK(gate.Pass(false, none, 63'000'000, out));  // no scene held any more: nothing to keep
    CHECK_FALSE(out.has_value());
    // The next scene.
    const std::optional<std::string> valley = std::string("GerudoValley_Adult");
    CHECK(gate.Pass(true, valley, 70'000'000, out));
    CHECK(out == valley);
}

TEST_CASE("objects port: the near log says who an enemy is with and who is nearer") {
    using Peer = Objects::FrameInput::Peer;
    const std::array<float, 3> enemy{-80.0f, 153.0f, 130.0f};
    const std::array<float, 3> self{-83.0f, 153.0f, 126.0f};  // 5 away
    std::string line = Objects::NearLine("Enemy_Stalchild", 0x1785de868, enemy, self, {Peer{2, {-80.0f, 153.0f, 142.0f}}});
    CHECK(line == "objects: near: ours Enemy_Stalchild 0x1785de868 at -80.0 153.0 130.0: 5.0 from us, "
                  "12.0 from P2 (we are the nearest)");
    line = Objects::NearLine("Enemy_Stalchild", 0x10, enemy, self, {Peer{2, {-80.0f, 153.0f, 133.0f}}, Peer{3, {0, 153.0f, 130.0f}}});
    CHECK(line.find("3.0 from P2, 80.0 from P3 (another player is nearer)") != std::string::npos);
    CHECK(Objects::NearLine("Enemy_Poe", 0x10, enemy, self, {}).ends_with("5.0 from us"));  // alone: no verdict
}

TEST_CASE("objects port: a version 1 game keeps it off and says why") {
    FakeGame::Game game;
    g_lines.clear();
    CHECK_FALSE(Objects::Init(game.ApiV1(), On(1), Collect));
    CHECK_FALSE(Objects::Active());
    CHECK(Logged("need the plugin interface version 2"));
    Objects::FrameInput in;
    in.main_base = kMain;
    Objects::Frame(in);
    CHECK(game.hooks.empty());
    CHECK(Objects::StatusText().find("off") != std::string::npos);
    Objects::Shutdown();
}

TEST_CASE("objects port: the interface may come with a later frame") {
    FakeGame::Game game;
    CHECK(Objects::Init(nullptr, On(), Collect));
    CHECK_FALSE(Objects::Active());
    CHECK(Objects::StatusText().find("waiting") != std::string::npos);
    Objects::FrameInput in;
    in.main_base = kMain;
    Objects::Frame(in);
    CHECK_FALSE(Objects::Active());
    in.api = game.Api();
    Objects::Frame(in);
    CHECK(Objects::Active());
    CHECK(game.Hooked(kMain + 0x3b4e300));
    Objects::Shutdown();
}

TEST_CASE("objects port: the frame step and the hold are hooked once main is known") {
    FakeGame::Game game;
    REQUIRE(Objects::Init(game.Api(), On(1), Collect));
    Objects::FrameInput in;
    Objects::Frame(in);  // main not trusted yet
    CHECK(game.hooks.empty());
    in.main_base = kMain;
    Objects::Frame(in);
    CHECK(game.Hooked(kMain + 0x3b4e300));
    CHECK(game.Hooked(kMain + Objects::Hold::kLoop));
    Objects::Frame(in);
    CHECK(game.hooks.size() == 2);  // not twice
    Objects::Shutdown();
    CHECK(game.hooks.empty());
}

TEST_CASE("objects port: the emulator's memory and System read the game through the interface") {
    FakeGame::Game game;
    REQUIRE(Objects::Init(game.Api(), On(), Collect));
    Objects::SetMain(kMain);
    const FakeGame::Actor a = game.Add("Enemy_ReDead", 0xf9b880ab096a9cd6ull, {1, 2, 3}, {{12, 12}});
    Core::System system;
    REQUIRE(system.ApplicationProcess() != nullptr);
    auto& memory = system.ApplicationMemory();
    CHECK(memory.IsValidVirtualAddressRange(a.address + 0x31c, 12));
    CHECK_FALSE(memory.IsValidVirtualAddressRange(0x1234000, 4));
    CHECK(memory.Read32(a.life_value + 8) == 12);
    memory.Write32(a.life_value + 8, 5);
    CHECK(game.Life(a) == 5);
    CHECK(memory.WriteExclusive32(a.life_value + 8, 4, 5));
    CHECK(game.Life(a) == 4);
    CHECK_FALSE(memory.WriteExclusive32(a.life_value + 8, 9, 5));
    Loader::AppLoader::Modules modules;
    REQUIRE(system.GetAppLoader().ReadNSOModules(modules) == Loader::ResultStatus::Success);
    CHECK(modules.begin()->first == kMain);
    Objects::Shutdown();
    CHECK(system.ApplicationProcess() == nullptr);
}

TEST_CASE("objects port: the unchanged actor walk reads the stand-in's list (a boss alive, then dead)") {
    FakeGame::Game game;
    REQUIRE(Objects::Init(game.Api(), On(), Collect));
    game.Add("Player_Adult", ~0ull, {0, 0, 0}, {{16, 16}});
    const FakeGame::Actor boss = game.Add("Enemy_DungeonBoss_Goma", 0x1234ull, {5, 0, 5}, {{24, 24}});
    // Not connected to a server: the walk runs, nothing is sent (no player number).
    coop::net::ClientConfig config;
    coop::net::Client client(config);
    coop::host::Session session(client);
    Objects::FrameInput in;
    in.main_base = kMain;
    in.session = &session;
    in.scene = "ydan_boss";
    in.server_now_us = 1'000'000;
    const std::array<std::string_view, 1> bodies{"Enemy_DungeonBoss_Goma"};
    // The walk runs every 33 ms on the steady clock (and an earlier test's
    // last walk may still be the fresh one).
    Objects::Frame(in);
    for (int i = 0; i < 20 && !CoopObjects::LastBossAlive(bodies).value_or(false); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Objects::Frame(in);
    }
    REQUIRE(CoopObjects::LastBossAlive(bodies).has_value());
    CHECK(*CoopObjects::LastBossAlive(bodies));
    game.SetLife(boss, 0);
    for (int i = 0; i < 20 && CoopObjects::LastBossAlive(bodies).value_or(true); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        Objects::Frame(in);
    }
    CHECK_FALSE(CoopObjects::LastBossAlive(bodies).value_or(true));
    in.session = nullptr;  // not connected: the sync forgets
    Objects::Frame(in);
    Objects::Shutdown();
}

TEST_CASE("objects port: the callers run in the frame step, with stack arguments, at most 256 calls") {
    FakeGame::Game game;
    REQUIRE(Objects::Init(game.Api(), On(), Collect));
    const std::size_t before = Objects::CallerCount();
    static bool on = true;  // the caller stays registered: it goes quiet after this test
    on = true;
    int asked = 0;
    std::uint64_t got = 0;
    // A caller as the hook's files add them: one call with a stack word, then
    // an endless stream to hit the frame's limit.
    CoopSvc::AddCaller(99, [&](CoopPuppet::Guest&, bool after_call, std::uint64_t result, std::uint64_t) {
        std::optional<CoopPuppet::Call> c;
        if (!on) {
            return c;
        }
        if (!after_call) {
            ++asked;
            CoopPuppet::Call call;
            call.function = kMain + 0x14c6c90;  // the fake's malloc
            call.x[0] = 0x40;
            call.stack = {0, 7, 0, 0};
            call.s0 = 1.5f;
            c = call;
        } else if (result != 0) {
            got = result;
            CoopPuppet::Call call;
            call.function = kMain + 0x123450;  // anything else: returns 0
            c = call;
        } else {
            CoopPuppet::Call call;
            call.function = kMain + 0x123450;
            c = call;
        }
        return c;
    });
    Objects::FrameInput in;
    in.main_base = kMain;
    Objects::Frame(in);
    game.Step();
    CHECK(asked == 1);
    CHECK(got != 0);
    const auto mallocs = game.CallsTo(0x14c6c90);
    REQUIRE(mallocs.size() == 1);
    CHECK(mallocs[0].x[0] == 0x40);
    CHECK(mallocs[0].s0 == doctest::Approx(1.5f));
    REQUIRE(mallocs[0].stack.size() == 4);
    CHECK(mallocs[0].stack[1] == 7);
    CHECK(game.calls.size() == 256);
    CHECK(Objects::StatusText().find("game calls") != std::string::npos);
    on = false;
    CHECK(Objects::CallerCount() == before + 1);
    Objects::Shutdown();
}

TEST_CASE("objects port: the Mover's hold table is caught, not written into main") {
    FakeGame::Game game;
    REQUIRE(Objects::Init(game.Api(), On(1), Collect));
    Objects::SetMain(kMain);
    const std::vector<std::uint64_t> components{0x2000001000ull, 0x2000002000ull};
    const auto bytes = CoopHold::Table(components, CoopHold::MasksFor(1));
    Core::System system;
    CHECK(system.ApplicationMemory().WriteBlockUnsafe(kMain + CoopHold::kTableOffset, bytes.data(),
                                                       bytes.size()));
    auto& table = Objects::Hold::TheTable();
    CHECK(table.Count() == 2);
    std::uint32_t bits = 0x2;
    CHECK(table.Alter(0x2000002000ull, bits));
    CHECK(bits == 0x3);
    bits = 0x2;
    CHECK_FALSE(table.Alter(0x2000003000ull, bits));
    CHECK(bits == 0x2);
    // Mode 2 clears the bit.
    const auto clear = CoopHold::Table(components, CoopHold::MasksFor(2));
    system.ApplicationMemory().WriteBlockUnsafe(kMain + CoopHold::kTableOffset, clear.data(), clear.size());
    bits = 0xf;
    CHECK(table.Alter(0x2000001000ull, bits));
    CHECK(bits == 0xe);
    Objects::Shutdown();
}

TEST_CASE("objects port: the hold finds the brain's calc-bits functions and changes their answer for held copies") {
    FakeGame::Game game;
    REQUIRE(Objects::Init(game.Api(), On(1), Collect));
    Objects::FrameInput in;
    in.main_base = kMain;
    Objects::Frame(in);
    REQUIRE(game.Hooked(kMain + Objects::Hold::kLoop));
    // The manager, its answering object and that object's vtable.
    const std::uint64_t manager = game.Alloc(0x1000), answerer = game.Alloc(0x10),
                        vtable = game.Alloc(0x100), context = game.Alloc(0x10);
    const std::uint64_t f40 = kMain + 0x1111000, f48 = kMain + 0x2222000;
    game.Put(context, manager);
    game.Put(manager + 0x950, answerer);
    game.Put(answerer, vtable);
    game.Put(vtable + 0x40, f40);
    game.Put(vtable + 0x48, f48);
    const std::uint64_t held = game.Alloc(0x100), free_one = game.Alloc(0x100);
    const auto bytes = CoopHold::Table({held}, CoopHold::MasksFor(1));
    Objects::Hold::TakeTable(kMain, kMain + CoopHold::kTableOffset, bytes.data(), bytes.size());

    int loop_ran = 0;
    Nx2HookCall loop{};
    loop.x[1] = context;
    game.CallHooked(kMain + Objects::Hold::kLoop, loop, [&](Nx2HookCall&) { ++loop_ran; });
    CHECK(loop_ran == 1);
    CHECK(game.Hooked(f40));
    CHECK(game.Hooked(f48));
    // A second loop does not hook them again.
    game.CallHooked(kMain + Objects::Hold::kLoop, loop, [&](Nx2HookCall&) { ++loop_ran; });
    CHECK(game.hooks.size() == 4);

    int answered = 0;
    const auto answer = [&](Nx2HookCall& c) {
        ++answered;
        c.x[0] = 0x12;  // bit 4 and bit 1 set, brain skip clear
    };
    // From the loop's call site, for a held copy: run, then bit 0 set.
    Nx2HookCall a{};
    a.x[1] = held;
    a.lr = kMain + Objects::Hold::kAnswerReturn;
    game.CallHooked(f40, a, answer);
    CHECK(answered == 1);
    CHECK(a.x[0] == 0x13);
    // The inlined copy of the loop in the frame function (not patched by the emulator).
    Nx2HookCall b{};
    b.x[1] = held;
    b.lr = kMain + Objects::Hold::kInlineReturn;
    game.CallHooked(f48, b, answer);
    CHECK(b.x[0] == 0x13);
    // A copy that is not held, or another caller of the function: untouched.
    Nx2HookCall c{};
    c.x[1] = free_one;
    c.lr = kMain + Objects::Hold::kAnswerReturn;
    game.CallHooked(f40, c, answer);
    CHECK(c.x[0] == 0x12);
    Nx2HookCall d{};
    d.x[1] = held;
    d.lr = kMain + 0x500000;
    game.CallHooked(f40, d, answer);
    CHECK(d.x[0] == 0x12);
    CHECK(answered == 4);
    const auto counts = Objects::Hold::Take();
    CHECK(counts.altered == 2);
    CHECK(counts.answer_hooks == 2);
    Objects::Shutdown();
    CHECK(game.hooks.empty());
}

TEST_CASE("objects port: hold mode 3 leaves a held copy's AI update out and nothing else") {
    FakeGame::Game game;
    REQUIRE(Objects::Init(game.Api(), On(3), Collect));
    Objects::FrameInput in;
    in.main_base = kMain;
    Objects::Frame(in);
    REQUIRE(game.Hooked(kMain + Objects::Brain::kActorAi));
    CHECK_FALSE(game.Hooked(kMain + Objects::Hold::kLoop));  // the brain skip bits are not touched
    CHECK(Logged("ai hold mode 3"));
    // Two actors with a component table and a BSA component each; one is a held copy.
    const auto actor = [&](std::uint64_t& bsa) {
        const std::uint64_t a = game.Alloc(0x800), table = game.Alloc(0x400);
        bsa = game.Alloc(0x100);
        game.Put(a + Objects::Brain::kActorComponents, table);
        game.Put(table + Objects::Brain::kComponentBsa, bsa);
        return a;
    };
    std::uint64_t held_bsa = 0, free_bsa = 0;
    const std::uint64_t held = actor(held_bsa), free_one = actor(free_bsa);
    const std::uint64_t bare = game.Alloc(0x800);  // an actor with no component table
    int ran = 0;
    const auto update = [&](Nx2HookCall&) { ++ran; };
    const auto call = [&](std::uint64_t a) {
        Nx2HookCall c{};
        c.x[0] = a;
        game.CallHooked(kMain + Objects::Brain::kActorAi, c, update);
    };
    // Nothing held: every update runs.
    call(held);
    call(free_one);
    CHECK(ran == 2);
    const auto bytes = CoopHold::Table({held_bsa}, CoopHold::Masks{});
    Objects::Hold::TakeTable(kMain, kMain + CoopHold::kTableOffset, bytes.data(), bytes.size());
    // Held for the first time: its first updates run (an enemy made as a copy
    // has to leave its first state), then it is left out.
    for (int i = 0; i < Objects::Brain::kWarmFrames; ++i) {
        call(held);
    }
    CHECK(ran == 2 + Objects::Brain::kWarmFrames);
    ran = 2;
    Objects::Brain::Take();
    call(held);      // left out
    CHECK(ran == 2);
    call(free_one);  // ours, or nobody's: runs
    call(bare);
    call(0);
    CHECK(ran == 5);
    // The copy became ours (the Mover's next table is empty): its AI runs again.
    const auto none = CoopHold::Table({}, CoopHold::Masks{});
    Objects::Hold::TakeTable(kMain, kMain + CoopHold::kTableOffset, none.data(), none.size());
    call(held);
    CHECK(ran == 6);
    auto counts = Objects::Brain::Take();
    CHECK(counts.calls == 5);
    CHECK(counts.skipped == 1);
    CHECK(counts.for_hits == 0);

    // A hit comes in for the held copy: its next kHitFrames updates run (the
    // game takes the hit in them: the life goes down, the enemy flinches), then
    // it is left out again.
    Objects::Hold::TakeTable(kMain, kMain + CoopHold::kTableOffset, bytes.data(), bytes.size());
    const std::uint64_t table = game.Get<std::uint64_t>(held + Objects::Brain::kActorComponents);
    const std::uint64_t life = game.Alloc(0x2000);
    game.Put(table + Objects::Brain::kComponentLife, life);
    game.Put<std::uint32_t>(life + Objects::Brain::kHitHeaderId, 0x175);   // a hit from long ago
    call(held);
    call(held);
    CHECK(ran == 6);  // nothing new: left out
    // Our own Link strikes the copy: the game writes a new id into the attack's header.
    game.Put<std::uint32_t>(life + Objects::Brain::kHitHeaderId, 0x177);
    for (int i = 0; i < Objects::Brain::kHitFrames + 5; ++i) {
        call(held);
    }
    CHECK(ran == 6 + Objects::Brain::kHitFrames);
    // The owner's game says the enemy was struck: the Mover writes a hit record
    // into the copy's Life component through the port (the same id each time).
    std::array<std::uint8_t, Objects::Brain::kHitRecordSize> record{};
    Core::System system;
    CHECK(system.ApplicationMemory().WriteBlockUnsafe(life + Objects::Brain::kHitRecord, record.data(),
                                                      record.size()));
    for (int i = 0; i < Objects::Brain::kHitFrames + 5; ++i) {
        call(held);
    }
    CHECK(ran == 6 + 2 * Objects::Brain::kHitFrames);
    // A write of another size, or a record for another actor's Life, changes nothing.
    std::array<std::uint8_t, 0x10> header{};
    system.ApplicationMemory().WriteBlockUnsafe(life + 0x224, header.data(), header.size());
    system.ApplicationMemory().WriteBlockUnsafe(game.Alloc(0x400) + Objects::Brain::kHitRecord, record.data(),
                                                record.size());
    call(held);
    CHECK(ran == 6 + 2 * Objects::Brain::kHitFrames);
    counts = Objects::Brain::Take();
    CHECK(counts.for_hits == 2u * Objects::Brain::kHitFrames);
    Objects::Shutdown();
    CHECK(game.hooks.empty());
}

TEST_CASE("objects port: with no answering function hookable the hold rewrites the bits after the loop") {
    FakeGame::Game game;
    REQUIRE(Objects::Init(game.Api(), On(1), Collect));
    Objects::FrameInput in;
    in.main_base = kMain;
    Objects::Frame(in);
    const std::uint64_t manager = game.Alloc(0x1000), answerer = game.Alloc(0x10),
                        vtable = game.Alloc(0x100), context = game.Alloc(0x10);
    game.Put(context, manager);
    game.Put(manager + 0x950, answerer);
    game.Put(answerer, vtable);  // both slots 0: nothing to hook
    const std::uint64_t held = game.Alloc(0x100);
    game.Put<std::uint16_t>(held + 0xc4, 0x0122);
    const auto bytes = CoopHold::Table({held}, CoopHold::MasksFor(1));
    Objects::Hold::TakeTable(kMain, kMain + CoopHold::kTableOffset, bytes.data(), bytes.size());
    int ran = 0;
    Nx2HookCall loop{};
    loop.x[1] = context;
    game.CallHooked(kMain + Objects::Hold::kLoop, loop, [&](Nx2HookCall&) { ++ran; });
    CHECK(ran == 1);
    CHECK(game.Get<std::uint16_t>(held + 0xc4) == 0x0123);
    Objects::Shutdown();
}
