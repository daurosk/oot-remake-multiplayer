// Tests for the puppet director and planner (lichii/hook/coop_puppet.cpp)
// and the cave's encoding (lichii/hook/coop_svc.h). No emulator code: guest
// memory is a set of fake pages.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "coop/host/player_reader.hpp"
#include "coop_fairy.h"
#include "coop_lift_player.h"
#include "coop_puppet.h"
#include "coop_puppet_own.h"
#include "coop_sites.h"
#include "coop_svc.h"

namespace {

using CoopPuppet::Call;
using CoopPuppet::Director;
using CoopPuppet::Phase;
using CoopPuppet::Target;
namespace addr = CoopPuppet::addr;
namespace block = CoopPuppet::block;

constexpr std::uint64_t kBase = 0x80000000;    // main
constexpr std::uint64_t kHeap = 0x200000000;   // what the fake malloc gives
constexpr std::uint64_t kManager = 0x300000000;
constexpr std::uint64_t kDefault = kBase + 0x3ee4f8c;
constexpr std::uint64_t kRequest = 0x400000000;
constexpr std::uint64_t kActor = 0x400002000;
constexpr std::uint64_t kVtable = kBase + 0x4000000;

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
            if (it == pages_.end()) {
                return false;
            }
            o[i] = it->second[(address + i) & 0xfff];
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = pages_.find((address + i) & ~0xfffULL);
            if (it == pages_.end()) {
                return false;
            }
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

// A game with the manager pointer, the walker's default and a create
// request that is not filled in until Spawn().
struct Game {
    FakeGuest guest;
    std::uint64_t create_result = 1;  // what the game's create returns
    bool queue_anyway = false;        // a request in the handle even so
    std::uint64_t unequip_result = 1; // what the game's armor take-off returns
    std::uint64_t equip_result = 0;   // what the game's equip call returns (1: worn)
    std::array<std::uint64_t, 8> slot_state{};  // what the game's state read (kEquipState) returns per slot
    std::vector<Call> state_checks;            // those reads, in order
    Game() {
        guest.Map(kBase + addr::kActorManagerPtr, 8);
        guest.Map(kBase + addr::kGotCreateDefault, 8);
        guest.Map(kHeap, 0x1000);
        guest.Map(kManager, 0x200);
        guest.Map(kRequest, 0x30);
        guest.Map(kActor, 0x400);
        guest.Set(kBase + addr::kActorManagerPtr, kManager);
        guest.Set(kBase + addr::kGotCreateDefault, kDefault);
    }
    // What the manager does: a request in the handle slot at once, loading
    // (state 2); later ready (3) with the actor, which is alive.
    void Request(std::uint64_t handle) {
        guest.Set(kRequest, std::uint32_t{2});
        guest.Set(handle, kRequest);
    }
    void Spawn(std::uint64_t handle, std::uint32_t state = 4) {
        Request(handle);
        guest.Set(kRequest, std::uint32_t{0x10000 | addr::kRequestReady});
        guest.Set(kRequest + addr::kRequestActor, kActor);
        guest.Set(kActor, kVtable);
        guest.Set(kActor + addr::kActorState, state);
    }
};

Target At(std::uint8_t number, float x, float y, float z, std::string actor = "Animal_Cow_LinkHouse") {
    Target t;
    t.number = number;
    t.actor = std::move(actor);
    t.position = {x, y, z};
    return t;
}

CoopPuppet::Input Wants(std::vector<Target> targets, std::string scene = "KokiriForest_Child") {
    CoopPuppet::Input in;
    in.main_base = kBase;
    in.scene = std::move(scene);
    in.targets = std::move(targets);
    return in;
}

// Runs the cave's loop for one frame as the cave would, with the game's
// functions faked. Returns the calls made.
std::vector<Call> Frame(Director& d, Game& g, std::uint64_t now_ms) {
    std::vector<Call> calls;
    auto call = d.Step(g.guest, false, 0, now_ms);
    while (call) {
        calls.push_back(*call);
        std::uint64_t result = 0;
        if (call->function == kBase + addr::kMalloc) {
            result = kHeap;
        } else if (call->function == kBase + addr::kCreate) {
            result = g.create_result;
            if (g.queue_anyway) {
                g.Request(call->x[3]);
            }
        } else if (call->function == kBase + addr::kRequestDelete) {
            result = 1;
        } else if (call->function == kBase + addr::kEquip) {
            result = g.equip_result;
        } else if (call->function == kBase + addr::kUnequipArmor) {
            result = g.unequip_result;
        } else if (call->function == kBase + addr::kTakeCreated) {
            REQUIRE(g.guest.Read(call->x[0] + addr::kRequestActor, &result, 8));
        } else if (call->function == kBase + addr::kEquipState) {
            // The game's read of a slot's state, from the fake's slot states;
            // kept apart from the calls a test counts.
            result = g.slot_state[call->x[1] & 7];
            g.state_checks.push_back(*call);
            calls.pop_back();
        }
        call = d.Step(g.guest, true, result, now_ms);
        REQUIRE(calls.size() < 16);
    }
    return calls;
}


}  // namespace

namespace {
void CheckPosition(FakeGuest& guest, std::uint64_t at, float x, float y, float z) {
    const auto p = guest.Get<std::array<float, 3>>(at);
    CHECK(p[0] == doctest::Approx(x));
    CHECK(p[1] == doctest::Approx(y));
    CHECK(p[2] == doctest::Approx(z));
}
}  // namespace

TEST_CASE("puppet cave: the call and the cave decode to the right places") {
    CHECK(CoopSvc::HookWord(0x6e6dd8) == 0x94d57d2au);
    for (const auto& h : CoopSvc::kHooks) {
        // The game's word is bl 0x3b4e300, ours lands on the cave.
        auto target = [&](std::uint32_t word) {
            REQUIRE((word & 0xfc000000u) == 0x94000000u);
            std::int64_t imm = word & 0x3ffffff;
            if (imm & 0x2000000) imm -= 0x4000000;
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(h.offset) + imm * 4);
        };
        CHECK(target(h.original) == 0x3b4e300);
        CHECK(target(CoopSvc::HookWord(h.offset)) == CoopSvc::kCaveOffset);
    }
    CHECK(CoopSvc::kHooks[1].offset == 0x6e70f8);

    const auto& cave = CoopSvc::kCave;
    CHECK(CoopSvc::kCaveOffset >= 0x3c46278);  // after Dynamic FPS
    CHECK(CoopSvc::kCaveOffset + cave.size() <= CoopSvc::kCaveEnd);
    std::uint32_t last = 0;
    std::memcpy(&last, cave.data() + cave.size() - 4, 4);
    REQUIRE((last & 0xfc000000u) == 0x14000000u);  // b
    std::int64_t b = last & 0x3ffffff;
    if (b & 0x2000000) b -= 0x4000000;
    CHECK(CoopSvc::kCaveOffset + cave.size() - 4 + b * 4 == 0x3b4e300);  // the game's call
    bool svc = false;
    for (std::size_t i = 0; i + 4 <= cave.size(); i += 4) {
        std::uint32_t w = 0;
        std::memcpy(&w, cave.data() + i, 4);
        svc = svc || w == (0xd4000001u | (CoopSvc::kPuppetSvc << 5));
    }
    CHECK(svc);
}

TEST_CASE("puppet cave: a game mod at a place co-op patches is found, others' bytes are not (#136)") {
    const auto put = [](std::vector<std::uint8_t>& v, std::uint64_t at, std::uint32_t word) {
        std::memcpy(v.data() + at, &word, 4);
    };
    // A main as shipped: the hooked calls hold the game's words, the caves are zero.
    std::vector<std::uint8_t> main(CoopSvc::kCaveEnd, 0);
    for (const auto& h : CoopSvc::kHooks) put(main, h.offset, h.original);
    put(main, CoopHold::kHookOffset, CoopHold::kHookOriginal);
    put(main, CoopCounters::kStartOffset, CoopCounters::kStartOriginal);
    CHECK_FALSE(CoopSites::ChangedSite(main).has_value());
    // The emulator's own built-ins and other bytes in the padding are not ours.
    main[0x3c46200] = 0xff;  // Dynamic FPS
    main[0x3c46e00] = 0xff;  // Fritchii v5.7 high refresh
    main[0x3c46b00] = 0xff;  // Fritchii v6 pickup counter stub
    main[0x3c46dc0] = 0xff;  // Fritchii v6 shadow steps
    main[0x3c46f10] = 0xff;  // Fritchii v6 texture pool routine
    CHECK_FALSE(CoopSites::ChangedSite(main).has_value());
    // Fritchii v6's high refresh fixes put stubs in main+0x3c463a0..0x3c46b00 (patch_manager.cpp
    // HR_FIX_STUBS), over our hold table and the pause counters' cave and ring: with them in, co-op
    // refuses as for a code mod. CoopHook::Start sets FRITCHII_NO_HR_FIXES=1 so they stay out.
    {
        auto hr = main;
        for (std::uint64_t i = 0x3c463a0; i < 0x3c46b00; ++i) hr[i] = 0xff;
        const auto hit = CoopSites::ChangedSite(hr);
        REQUIRE(hit.has_value());
        CHECK(*hit >= 0x3c463a0);
        CHECK(*hit < 0x3c46b00);
        CHECK(CoopHold::kTableOffset + CoopHold::kTableSize > 0x3c463a0);
        CHECK(CoopCounters::kCaveOffset < 0x3c46b00);
        CHECK(CoopCounters::kRingPointer + 8 <= 0x3c46b00);
    }
    // The Hyrule Cheat Menu v0.1 writes its stubs from main+0x3c46280: our cave.
    auto mod = main;
    put(mod, 0x3c46280, 0xd503201f);
    CHECK(CoopSites::ChangedSite(mod) == std::optional<std::uint64_t>(0x3c46280));
    // A changed hook word, the hold hook, a byte of the hold table.
    mod = main;
    put(mod, CoopSvc::kHooks[1].offset, 0x94000000);
    CHECK(CoopSites::ChangedSite(mod) == std::optional<std::uint64_t>(CoopSvc::kHooks[1].offset));
    mod = main;
    mod[CoopHold::kHookOffset] ^= 1;
    CHECK(CoopSites::ChangedSite(mod) == std::optional<std::uint64_t>(CoopHold::kHookOffset));
    mod = main;
    mod[CoopHold::kTableOffset + CoopHold::kTableSize - 1] = 1;
    CHECK(CoopSites::ChangedSite(mod) ==
          std::optional<std::uint64_t>(CoopHold::kTableOffset + CoopHold::kTableSize - 1));
    // The pause counters' start, its cave and the ring's address (platforms-b2).
    mod = main;
    put(mod, CoopCounters::kStartOffset, 0xd503201f);
    CHECK(CoopSites::ChangedSite(mod) == std::optional<std::uint64_t>(CoopCounters::kStartOffset));
    mod = main;
    mod[CoopCounters::kCaveOffset + 4] = 1;
    CHECK(CoopSites::ChangedSite(mod) == std::optional<std::uint64_t>(CoopCounters::kCaveOffset + 4));
    mod = main;
    mod[CoopCounters::kRingPointer + 7] = 1;
    CHECK(CoopSites::ChangedSite(mod) == std::optional<std::uint64_t>(CoopCounters::kRingPointer + 7));
    // With the NSO header in front, and a module too short to tell.
    std::vector<std::uint8_t> nso(0x100, 0);
    nso.insert(nso.end(), main.begin(), main.end());
    CHECK_FALSE(CoopSites::ChangedSite(nso, 0x100).has_value());
    CHECK_FALSE(CoopSites::ChangedSite(std::vector<std::uint8_t>(16, 0xff)).has_value());
}

TEST_CASE("puppet cave: co-op that already runs stops once when a code mod is found later (#136)") {
    // The emulator started directly: co-op connected before the game's main was patched.
    CHECK_FALSE(CoopSites::MustStopForMod(true, false, ""));             // main as shipped
    CHECK(CoopSites::MustStopForMod(true, false, "main+0x3c46280"));     // the patcher refused
    CHECK_FALSE(CoopSites::MustStopForMod(true, true, "main+0x3c46280"));   // stopped already
    CHECK_FALSE(CoopSites::MustStopForMod(false, false, "main+0x3c46280"));  // never started
}

TEST_CASE("puppet: a quaternion turns into the matrix the reader reads back") {
    const float s = std::sqrt(0.5f);
    for (const CoopPuppet::Quat q : {CoopPuppet::Quat{0, 0, 0, 1}, CoopPuppet::Quat{0, s, 0, s},
                                     CoopPuppet::Quat{0.1f, 0.7f, -0.2f, 0.67f}}) {
        const auto m = CoopPuppet::QuatToMatrix(q);
        coop::host::Mat3 rows{};
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                rows[r][c] = m[r * 3 + c];
            }
        }
        const auto back = coop::host::matrix_to_quaternion(rows);
        REQUIRE(back);
        const double n = std::sqrt(double(q[0]) * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        double dot = 0;
        for (int i = 0; i < 4; ++i) dot += (*back)[i] * q[i] / n;
        CHECK(std::fabs(dot) == doctest::Approx(1.0).epsilon(1e-4));
    }
    // Zero gives the identity.
    const auto id = CoopPuppet::QuatToMatrix({0, 0, 0, 0});
    CHECK(id[0] == 1.0f);
    CHECK(id[4] == 1.0f);
    CHECK(id[8] == 1.0f);
}

TEST_CASE("puppet: malloc, create, live, follow, delete") {
    Game g;
    std::vector<std::string> log;
    Director d([&](const std::string& line) { log.push_back(line); });

    // Nothing without a game.
    CHECK(Frame(d, g, 0).empty());

    d.SetInput(Wants({At(2, 10, 1, -5)}));
    auto calls = Frame(d, g, 100);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].function == kBase + addr::kMalloc);
    CHECK(calls[0].x[0] == block::kSize);
    const Call& create = calls[1];
    CHECK(create.function == kBase + addr::kCreate);
    CHECK(create.x[0] == kManager);
    CHECK(create.x[1] == kHeap + block::kName);
    CHECK(create.x[2] == kHeap + block::kCreateArg);
    const std::uint64_t handle = create.x[3];
    CHECK(handle >= kHeap + block::kHandles);
    CHECK(create.x[4] == 1);
    CHECK(create.x[5] == 0);
    CHECK(create.stack == std::array<std::uint64_t, 4>{});

    // The create argument and the name, as the walker fills them.
    CheckPosition(g.guest, kHeap + 0x00, 10, 1, -5);
    CHECK(g.guest.Get<float>(kHeap + 0x0c) == 1.0f);  // rotation row 0
    CHECK(g.guest.Get<float>(kHeap + 0x1c) == 1.0f);  // row 1, column 1
    CheckPosition(g.guest, kHeap + 0x30, 1, 1, 1);    // scale
    CHECK(g.guest.Get<std::uint64_t>(kHeap + 0x40) == kDefault);
    CHECK(g.guest.Get<std::uint32_t>(kHeap + 0x48) == 0u);
    CHECK(g.guest.Get<std::uint8_t>(kHeap + 0x98) == 1u);
    CHECK(g.guest.Get<std::uint64_t>(kHeap + block::kName) == kHeap + block::kNameText);
    CHECK(g.guest.Get<std::uint32_t>(kHeap + block::kName + 8) == block::kNameCapacity);
    std::array<char, 32> name{};
    g.guest.Read(kHeap + block::kNameText, name.data(), name.size());
    CHECK(std::string(name.data()) == "Animal_Cow_LinkHouse");
    REQUIRE(d.Slots().size() == 1);
    CHECK(d.Slots()[0].phase == Phase::creating);

    // Not there yet: nothing to do, also while the request loads.
    CHECK(Frame(d, g, 200).empty());
    g.Request(handle);
    CHECK(Frame(d, g, 250).empty());
    CHECK(d.Slots()[0].phase == Phase::creating);

    // The request is ready: the actor is taken, as the game does, and the
    // handle slot cleared; then it is live.
    g.Spawn(handle);
    calls = Frame(d, g, 300);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kTakeCreated);
    CHECK(calls[0].x[0] == kRequest);
    CHECK(calls[0].x[1] == 1);
    CHECK(calls[0].x[2] == 0);
    CHECK(g.guest.Get<std::uint64_t>(handle) == 0);
    CHECK(Frame(d, g, 310).empty());
    CHECK(d.Slots()[0].phase == Phase::live);
    CHECK(d.Slots()[0].actor == kActor);

    // It follows: the pose is written every frame.
    // (A step under CoopSnap::kSnapM: drawn as given, not eased; issue 194.)
    Target moved = At(2, 10.5f, 1.25f, -5.25f);
    moved.rotation = CoopPuppet::QuatToMatrix({0, std::sqrt(0.5f), 0, std::sqrt(0.5f)});
    d.SetInput(Wants({moved}));
    CHECK(Frame(d, g, 400).empty());
    CheckPosition(g.guest, kActor + addr::kActorPosition, 10.5f, 1.25f, -5.25f);
    CHECK(g.guest.Get<float>(kActor + addr::kActorRotation + 4 * 2) ==
          doctest::Approx(moved.rotation[2]));

    // A moment out of the plan (a stalled peer) deletes nothing, and coming
    // back resets the wait.
    d.SetInput(Wants({}));
    CHECK(Frame(d, g, 410).empty());
    CHECK(Frame(d, g, 410 + Director::kLeaveGraceMs - 1).empty());
    CHECK(d.Slots()[0].phase == Phase::live);
    d.SetInput(Wants({moved}));
    CHECK(Frame(d, g, 2500).empty());

    // The player leaves: after the grace, one delete request, with the actor.
    d.SetInput(Wants({}));
    CHECK(Frame(d, g, 2510).empty());
    calls = Frame(d, g, 2510 + Director::kLeaveGraceMs);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kRequestDelete);
    CHECK(calls[0].x[0] == kActor);
    CHECK(calls[0].x[1] == 0);
    CHECK(d.Slots()[0].phase == Phase::deleting);
    g.guest.Set(kActor + addr::kActorState, std::uint32_t{8});
    CHECK(Frame(d, g, 4600).empty());
    CHECK(d.Slots()[0].phase == Phase::idle);

    // Back again: the block is kept, only a create, in a new handle slot.
    d.SetInput(Wants({At(2, 0, 0, 0)}));
    calls = Frame(d, g, 4700);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kCreate);
    CHECK(calls[0].x[3] != handle);

    bool spawned = false;
    for (const auto& line : log) {
        spawned = spawned || line.find("spawned") != std::string::npos;
    }
    CHECK(spawned);
}

TEST_CASE("puppet: a scene change deletes what survived it, then creates anew") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0)}, "A"));
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() == 2);
    g.Spawn(calls[1].x[3]);
    Frame(d, g, 10);
    Frame(d, g, 15);
    REQUIRE(d.Slots()[0].phase == Phase::live);

    // Loading: no scene, nothing is touched.
    d.SetInput(Wants({}, ""));
    CHECK(Frame(d, g, 20).empty());
    CHECK(d.Slots()[0].phase == Phase::live);

    // The new scene, the actor still alive: delete it first.
    d.SetInput(Wants({At(1, 5, 0, 0)}, "B"));
    calls = Frame(d, g, 30);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kRequestDelete);
    g.guest.Set(kActor + addr::kActorState, std::uint32_t{9});
    calls = Frame(d, g, 40);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kCreate);
}

TEST_CASE("puppet: a player who changes age gets the other puppet, in the same scene") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Child")}));
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() == 2);
    g.Spawn(calls[1].x[3]);
    Frame(d, g, 10);
    Frame(d, g, 15);
    REQUIRE(d.Slots()[0].phase == Phase::live);

    // Now an adult: the child puppet is deleted at once, no leave grace.
    d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Adult")}));
    calls = Frame(d, g, 20);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kRequestDelete);
    CHECK(calls[0].x[0] == kActor);
    g.guest.Set(kActor + addr::kActorState, std::uint32_t{9});
    calls = Frame(d, g, 30);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kCreate);
    CHECK(d.Slots()[0].actor_name == "CoopPuppet_Adult");
}

TEST_CASE("puppet: a tunic puppet without a Player_ model falls back to the plain one") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Adult_Red")}));
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() == 2);
    g.Spawn(calls[1].x[3]);  // the fake actor has no skeleton or model
    Frame(d, g, 10);
    Frame(d, g, 15);
    REQUIRE(d.Slots()[0].phase == Phase::live);
    Frame(d, g, 1000);
    CHECK(d.FailedTunics().empty());  // checked only after kTunicModelCheckMs
    Frame(d, g, 15 + Director::kTunicModelCheckMs + 10);
    CHECK(d.FailedTunics() == std::vector<std::string>{"CoopPuppet_Adult_Red"});
}

TEST_CASE("puppet: the plain puppet's model is not checked") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Adult")}));
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() == 2);
    g.Spawn(calls[1].x[3]);
    for (std::uint64_t t = 10; t < 10000; t += 500) Frame(d, g, t);
    CHECK(d.FailedTunics().empty());
}

TEST_CASE("puppet: a tunic actor that is never made falls back after two timeouts") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Child_Blue")}));
    for (std::uint64_t t = 0; t < 2 * (Director::kCreateTimeoutMs + 5000) + 2000; t += 250) {
        Frame(d, g, t);
    }
    CHECK(d.FailedTunics() == std::vector<std::string>{"CoopPuppet_Child_Blue"});
}

TEST_CASE("puppet: a tunic actor the game keeps refusing falls back (an older pack, Hookshot proof 3)") {
    Game g;
    g.create_result = 0;  // the game refuses: no such actor in its ActorInfo
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Child_Blue")}));
    std::uint64_t t = 0;
    for (; t < 60000 && d.FailedTunics().empty(); t += 250) {
        Frame(d, g, t);
    }
    CHECK(d.FailedTunics() == std::vector<std::string>{"CoopPuppet_Child_Blue"});
    // About 3 x 1 s + 3 x 5 s: longer than a load's refusals, well before a minute.
    CHECK(t > 3 * Director::kRefusedRetryMs + 2 * Director::kRefusedSlowMs);
    CHECK(t < 30000);
}

TEST_CASE("puppet: refusals do not cost the plain puppet or a tunic one that is made in time") {
    {
        Game g;
        g.create_result = 0;
        Director d;
        d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Child")}));
        for (std::uint64_t t = 0; t < 60000; t += 250) Frame(d, g, t);
        CHECK(d.FailedTunics().empty());  // the plain one has nothing to fall back to: it keeps asking
    }
    {
        Game g;
        g.create_result = 0;  // refused while the scene loads ...
        Director d;
        d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Child_Blue")}));
        std::uint64_t t = 0;
        for (; t < 8000; t += 250) Frame(d, g, t);  // 3 fast refusals and one slow one
        g.create_result = 1;  // ... then accepted (the refusal count starts again)
        for (; t < 8000 + Director::kCreateTimeoutMs; t += 250) Frame(d, g, t);
        CHECK(d.FailedTunics().empty());
        CHECK(d.Slots()[0].actor_name == "CoopPuppet_Child_Blue");
    }
}

TEST_CASE("puppet: an engine-deleted actor is noticed and created again") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0)}));
    auto calls = Frame(d, g, 0);
    g.Spawn(calls[1].x[3]);
    Frame(d, g, 10);
    Frame(d, g, 15);
    REQUIRE(d.Slots()[0].phase == Phase::live);
    // Freed and reused for something else (another vtable): gone.
    g.guest.Set(kActor, kVtable + 0x100);
    CHECK(Frame(d, g, 20).size() == 1);  // gone, and at once a new create
    CHECK(d.Slots()[0].phase == Phase::creating);
}

TEST_CASE("puppet: a failed or dropped request is cleared and tried again") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0)}));
    auto calls = Frame(d, g, 0);
    const std::uint64_t handle = calls[1].x[3];
    g.Request(handle);
    g.guest.Set(kRequest, std::uint32_t{5});
    CHECK(Frame(d, g, 10).empty());
    CHECK(d.Slots()[0].phase == Phase::idle);
    CHECK(g.guest.Get<std::uint64_t>(handle) == 0);

    calls = Frame(d, g, 5100);
    REQUIRE(calls.size() == 1);
    const std::uint64_t again = calls[0].x[3];
    g.Request(again);
    g.guest.Set(kRequest, std::uint32_t{addr::kRequestDrop});
    calls = Frame(d, g, 5200);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kDropCreate);
    CHECK(calls[0].x[0] == kRequest);
    CHECK(d.Slots()[0].phase == Phase::idle);
    CHECK(g.guest.Get<std::uint64_t>(again) == 0);
}

TEST_CASE("puppet: a call the cave did not make is undone next frame") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0)}));
    auto first = d.Step(g.guest, false, 0, 0);
    REQUIRE(first);
    CHECK(d.Slots()[0].phase == Phase::allocating);
    // Next frame, with no result: the malloc is asked again.
    auto again = d.Step(g.guest, false, 0, 10);
    REQUIRE(again);
    CHECK(again->function == kBase + addr::kMalloc);
}

TEST_CASE("puppet: a create that never arrives is tried again later") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0)}));
    Frame(d, g, 0);
    CHECK(Frame(d, g, Director::kCreateTimeoutMs + 1).empty());
    CHECK(d.Slots()[0].phase == Phase::idle);
    CHECK(Frame(d, g, Director::kCreateTimeoutMs + 100).empty());  // waits 5 s
    const auto calls = Frame(d, g, Director::kCreateTimeoutMs + 6000);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kCreate);
}

TEST_CASE("puppet: no more than the frame's limit of calls") {
    Game g;
    Director d;
    std::vector<Target> many;
    for (std::uint8_t n = 0; n < 10; ++n) many.push_back(At(n, 0, 0, 0));
    d.SetInput(Wants(many));
    CHECK(Frame(d, g, 0).size() == Director::kMaxCallsPerFrame);
}

TEST_CASE("puppet planner: remote players in this scene, and the debug puppet") {
    CoopPuppet::PlannerSettings s;
    s.debug = true;
    CoopPuppet::Planner p(s);
    std::vector<CoopPuppet::Remote> remotes(3);
    remotes[0].number = 2;
    remotes[0].scene = "Kokiri";
    remotes[0].position = {1, 2, 3};
    remotes[0].rotation = CoopPuppet::Quat{0, 1, 0, 0};
    remotes[1].number = 3;
    remotes[1].scene = "Field";  // another scene
    remotes[2].number = 4;
    remotes[2].scene = "Kokiri";
    remotes[2].fade = 0.0f;  // faded out

    // No game: nothing.
    CHECK(p.Plan(0, std::string("Kokiri"), CoopPuppet::Vec3{0, 0, 0}, false, remotes, 0).main_base == 0);

    auto in = p.Plan(kBase, std::string("Kokiri"), CoopPuppet::Vec3{10, 0, 10}, false, remotes, 1000);
    REQUIRE(in.targets.size() == 1);  // the debug puppet waits for the delay
    CHECK(in.targets[0].number == 2);
    CHECK(in.targets[0].actor == "CoopPuppet_Child_Red");  // by age, player 2 red
    CHECK(in.targets[0].rotation[0] == doctest::Approx(-1.0f));  // half a turn about y

    in = p.Plan(kBase, std::string("Kokiri"), CoopPuppet::Vec3{10, 0, 10}, false, remotes, 4000);
    REQUIRE(in.targets.size() == 2);
    CHECK(in.targets[1].number == 0);
    CHECK(in.targets[1].position == CoopPuppet::Vec3{12, 0, 10});
    // It stays where it was put while you walk.
    in = p.Plan(kBase, std::string("Kokiri"), CoopPuppet::Vec3{20, 0, 10}, false, remotes, 5000);
    CHECK(in.targets[1].position == CoopPuppet::Vec3{12, 0, 10});

    // A new scene starts the delay again; an unknown scene plans nothing.
    in = p.Plan(kBase, std::string("Field"), CoopPuppet::Vec3{0, 0, 0}, false, remotes, 6000);
    REQUIRE(in.targets.size() == 1);
    CHECK(in.targets[0].number == 3);
    CHECK(p.Plan(kBase, std::nullopt, CoopPuppet::Vec3{0, 0, 0}, false, remotes, 7000).targets.empty());
}

namespace {
// The AS chain of kActor (actor +0x270, +0x40, +0x18, +0x18), with one slot
// whose frame is `frame`.
constexpr std::uint64_t kAs = 0x500000000;
constexpr std::uint64_t kController = kAs + 0x100;
constexpr std::uint64_t kSlot0 = kAs + 0x200;
void GiveAnimation(Game& g, float frame) {
    g.guest.Map(kAs, 0x1000);
    g.guest.Set(kActor + 0x270, kAs);
    g.guest.Set(kAs + 0x40, kAs + 0x40);
    g.guest.Set(kAs + 0x40 + 0x18, kAs + 0x80);
    g.guest.Set(kAs + 0x80 + 0x18, kController);
    g.guest.Set(kController + addr::kSlotCount, std::int32_t{1});
    g.guest.Set(kController + addr::kSlots, kAs + 0x300);
    g.guest.Set(kAs + 0x300, kSlot0);
    g.guest.Set(kSlot0 + addr::kSlotFrame, frame);
}

Target Playing(std::string name, float frame, std::uint32_t serial,
               CoopPuppet::Show show = CoopPuppet::Show::show) {
    Target t = At(1, 4, 5, 6);
    t.anim = CoopPuppet::Anim{std::move(name), frame, 1.5f};
    t.anim_serial = serial;
    t.show = show;
    return t;
}

// A live puppet P1 at frame 15.
void MakeLive(Director& d, Game& g) {
    d.SetInput(Wants({At(1, 4, 5, 6)}));
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() == 2);
    g.Spawn(calls[1].x[3]);
    Frame(d, g, 10);
    Frame(d, g, 15);
    REQUIRE(d.Slots()[0].phase == Phase::live);
}
}  // namespace

TEST_CASE("puppet animation: play the command, set its frame, correct only when far off") {
    Game g;
    std::vector<std::string> log;
    Director d([&](const std::string& line) { log.push_back(line); }, false, true);
    MakeLive(d, g);
    // No AS chain yet: nothing to play on.
    d.SetInput(Wants({Playing("Wait", 4.0f, 1)}));
    CHECK(Frame(d, g, 20).empty());

    GiveAnimation(g, 0.0f);
    auto calls = Frame(d, g, 30);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].function == kBase + addr::kPlayAs);
    const std::uint64_t request = calls[0].x[0];
    CHECK(request == kHeap + block::kPlayRequest);
    CHECK(g.guest.Get<std::uint64_t>(request + 0x00) == kActor);
    const auto name_at = g.guest.Get<std::uint64_t>(request + 0x08);
    CHECK(name_at == kHeap + block::kAnimText);
    std::array<char, block::kAnimCapacity> name{};
    REQUIRE(g.guest.Read(name_at, name.data(), name.size()));
    CHECK(std::string(name.data()) == "Wait");
    CHECK(g.guest.Get<std::int32_t>(request + 0x10) == 0);
    CHECK(g.guest.Get<float>(request + 0x14) == 1.5f);
    CHECK(g.guest.Get<float>(request + 0x18) == -1.0f);
    CHECK(g.guest.Get<float>(request + 0x1c) == -1.0f);
    CHECK(g.guest.Get<std::uint8_t>(request + 0x20) == 0u);
    CHECK(g.guest.Get<std::uint64_t>(request + 0x28) == 0u);
    CHECK(d.Slots()[0].anim_name == "Wait");
    // Then, in the same frame, the received frame set on slot 0.
    CHECK(calls[1].function == kBase + addr::kSetAsFrame);
    CHECK(calls[1].x[0] == kController);
    CHECK(calls[1].x[1] == 0u);
    CHECK(calls[1].s0 == 4.0f);
    CHECK(Frame(d, g, 40).empty());
    g.guest.Set(kSlot0 + addr::kSlotFrame, 6.0f);  // the game plays it on
    CHECK(Frame(d, g, 50).empty());

    // A new sample close to the puppet's frame: left alone. Far off: set.
    d.SetInput(Wants({Playing("Wait", 7.0f, 2)}));
    CHECK(Frame(d, g, 60).empty());
    d.SetInput(Wants({Playing("Wait", 20.0f, 3)}));
    calls = Frame(d, g, 70);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].s0 == 20.0f);
    CHECK(Frame(d, g, 80).empty());

    // A new command goes in the other name buffer.
    d.SetInput(Wants({Playing("Run", 1.0f, 4)}));
    calls = Frame(d, g, 90);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].function == kBase + addr::kPlayAs);
    CHECK(g.guest.Get<std::uint64_t>(request + 0x08) ==
          kHeap + block::kAnimText + block::kAnimCapacity);
    CHECK(d.Slots()[0].anim_name == "Run");

    // A name too long for the buffer is not played.
    d.SetInput(Wants({Playing(std::string(block::kAnimCapacity, 'x'), 1.0f, 5)}));
    CHECK(Frame(d, g, 100).empty());
    CHECK(d.Slots()[0].anim_name == "Run");
}

TEST_CASE("puppet animation: the backstop plays their command again when the puppet's own slot 0 left it (puppet-anim)") {
    Game g;
    std::vector<std::string> log;
    Director d([&](const std::string& line) { log.push_back(line); }, false, false);
    MakeLive(d, g);
    GiveAnimation(g, 0.0f);
    d.SetInput(Wants({Playing("Wait", 4.0f, 1)}));
    auto calls = Frame(d, g, 1000);
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0].function == kBase + addr::kPlayAs);
    const std::uint64_t played = g.guest.Get<std::uint64_t>(calls[0].x[0] + 0x08);
    // The game took it: slot 0 names the command we played.
    g.guest.Set(kSlot0 + addr::kSlotName, played);
    for (std::uint64_t ms = 2000; ms <= 6000; ms += 500) {
        CHECK(Frame(d, g, ms).empty());  // the same command: nothing to do
    }
    // The puppet's own animation moved on to another command (a one-shot ended, a play
    // dropped). One read finding it is not enough; the second, kBackstopMs on, plays theirs.
    const std::uint64_t other = kAs + 0x800;
    const char fall[] = "Fall";
    for (std::size_t i = 0; i < sizeof(fall); ++i) {
        g.guest.Set(other + i, static_cast<std::uint8_t>(fall[i]));
    }
    g.guest.Set(kSlot0 + addr::kSlotName, other);
    CHECK(Frame(d, g, 7000).empty());
    calls = Frame(d, g, 7000 + Director::kBackstopMs);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].function == kBase + addr::kPlayAs);
    CHECK(calls[1].function == kBase + addr::kSetAsFrame);
    CHECK(d.Slots()[0].anim_name == "Wait");
    REQUIRE_FALSE(log.empty());
    bool said = false;
    for (const auto& l : log) said = said || l.find("backstop: its own animation is 'Fall', not 'Wait'") != std::string::npos;
    CHECK(said);
}

TEST_CASE("puppet animation: the other slots synced from slot 0, pose request written") {
    Game g;
    Director d;
    MakeLive(d, g);
    GiveAnimation(g, 0.0f);
    g.guest.Set(kController + addr::kSlotCount, std::int32_t{3});
    d.SetInput(Wants({Playing("Wait", 4.0f, 1)}));
    auto calls = Frame(d, g, 20);
    REQUIRE(calls.size() == 4);
    CHECK(calls[0].function == kBase + addr::kPlayAs);
    CHECK(calls[1].function == kBase + addr::kSetAsFrame);
    for (std::size_t i = 2; i < 4; ++i) {
        CHECK(calls[i].function == kBase + addr::kSyncAsSlot);
        CHECK(calls[i].x[0] == kActor);
        CHECK(calls[i].x[1] == 0u);
        CHECK(calls[i].x[2] == i - 1);
    }
    CHECK(Frame(d, g, 30).empty());
    // The pending transform carries the pose too, so the actor's calc keeps it.
    CheckPosition(g.guest, kActor + addr::kActorPositionRequest, 4, 5, 6);
    CHECK(g.guest.Get<float>(kActor + addr::kActorRotationRequest) ==
          g.guest.Get<float>(kActor + addr::kActorRotation));

    // A new command syncs them again.
    d.SetInput(Wants({Playing("Run", 1.0f, 2)}));
    calls = Frame(d, g, 40);
    REQUIRE(calls.size() == 4);
    CHECK(calls[3].function == kBase + addr::kSyncAsSlot);
}

TEST_CASE("puppet gear: each known piece worn once, on its own name cell") {
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    Target t = At(1, 4, 5, 6);
    t.gear[0] = "Weapon_Sword_Kokiri";
    t.gear[1] = "Weapon_Shield_Deku";
    t.gear[2] = "";  // nothing: left as it is
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 20).empty());  // no EquipmentUser yet
    g.guest.Map(kParts, 0x1000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    auto calls = Frame(d, g, 30);
    REQUIRE(calls.size() == 2);
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(calls[i].function == kBase + addr::kEquip);
        CHECK(calls[i].x[0] == kEu);
        CHECK(calls[i].x[1] == CoopPuppet::kGearKinds[i]);
        CHECK(calls[i].x[2] == kHeap + block::kGearPtr + 8 * i);
        CHECK(calls[i].x[3] == 2u);
        CHECK(calls[i].x[4] == 0xFFFFFFFFu);
        CHECK(calls[i].x[5] == 1u);
        const auto text = g.guest.Get<std::uint64_t>(calls[i].x[2]);
        CHECK(text == kHeap + block::kGearText + block::kGearCapacity * i);
        std::array<char, block::kGearCapacity> name{};
        g.guest.Read(text, name.data(), name.size());
        CHECK(std::string(name.data()) == *t.gear[i]);
    }
    CHECK(Frame(d, g, 40).empty());  // worn: not again
    t.gear[1] = "Weapon_Shield_Hylian";
    d.SetInput(Wants({t}));
    calls = Frame(d, g, 50);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].x[1] == 6u);
}


TEST_CASE("puppet gear: a mask is worn in state 1, other pieces in state 2") {
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x1000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    Target t = At(1, 4, 5, 6);
    t.gear[2] = "Armor_Child_Mask_Keaton";  // head, kind 7
    t.gear[3] = "Armor_Adult_Goron";        // tunic, kind 8
    d.SetInput(Wants({t}));
    const auto calls = Frame(d, g, 30);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].x[1] == 7u);
    CHECK(calls[0].x[3] == 1u);
    CHECK(calls[1].x[1] == 8u);
    CHECK(calls[1].x[3] == 2u);
}
TEST_CASE("puppet gear: where null reads cannot give 0, the Hookshot is left off the puppet, and its slot is not ours (issue 232)") {
    Game g;
    Director d;
    d.SetNullReadsZero(false);
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x1000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    Target t = At(1, 4, 5, 6);
    t.gear[2] = "Armor_Child_Mask_Skull";
    d.SetInput(Wants({t}));
    auto calls = Frame(d, g, 20);
    REQUIRE(calls.size() == 1);  // the mask only
    CHECK(calls[0].x[1] == 7u);

    // The held slot is the Hookshot: no call is made for it, now or later.
    t.gear[CoopPuppet::kGearHeld] = "Weapon_Range_Hookshot";
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 100).empty());
    CHECK(Frame(d, g, 3000).empty());
    CHECK(Frame(d, g, 9000).empty());

    // Its going away is no take-off either: the mask stays, nothing is made again.
    t.gear[CoopPuppet::kGearHeld] = "";
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 10000).empty());
    CHECK(Frame(d, g, 20000).empty());

    // Any other piece of the same slot is worn as before: the slingshot, kind 5.
    t.gear[CoopPuppet::kGearHeld] = "Weapon_Range_SlingShot";
    d.SetInput(Wants({t}));
    calls = Frame(d, g, 30000);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kEquip);
    CHECK(calls[0].x[1] == 5u);

    // Slingshot -> Hookshot: the slingshot must not stay in slot 3, and no
    // call is known to take a held piece away, so the puppet is made again
    // without it (daurosk, PR 233: "the slingshot stays in slot 3").
    // (the frame that learns it only marks the remake: the delete is the next
    // frame's, as for a mask taken off)
    t.gear[CoopPuppet::kGearHeld] = "Weapon_Range_Hookshot";
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 40000).empty());
    calls = Frame(d, g, 41000);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kRequestDelete);
    // Made again (a delete, then a malloc and a create over the next frames),
    // and the Hookshot is never worn on it.
    bool equipped = false;
    for (std::uint64_t ms = 42000; ms <= 60000; ms += 1000) {
        for (const auto& c : Frame(d, g, ms)) {
            if (c.function == kBase + addr::kEquip) {
                equipped = true;
            }
        }
    }
    CHECK_FALSE(equipped);
}
TEST_CASE("puppet gear: where null reads give 0, the Hookshot and the Longshot are worn like any held item (issue 232)") {
    // The crash was the missing-Player read every held piece makes (guest
    // 0x7d8); with the null page armed (or on the emulator) it reads 0, so
    // the piece is worn, put away and drawn as the slingshot is.
    for (const char* name : {"Weapon_Range_Hookshot", "Weapon_Range_Longshot"}) {
        Game g;
        Director d;
        MakeLive(d, g);
        constexpr std::uint64_t kParts = 0x500000000;
        constexpr std::uint64_t kEu = kParts + 0x800;
        g.guest.Map(kParts, 0x1000);
        g.guest.Set(kActor + addr::kActorParts, kParts);
        g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
        Target t = At(1, 4, 5, 6);
        t.gear[CoopPuppet::kGearHeld] = name;
        d.SetInput(Wants({t}));
        auto calls = Frame(d, g, 20);
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].function == kBase + addr::kEquip);
        CHECK(calls[0].x[1] == 5u);
        CHECK(d.LeftOff(name) == false);

        // Switched to the slingshot: worn in its place, no remake.
        t.gear[CoopPuppet::kGearHeld] = "Weapon_Range_SlingShot";
        d.SetInput(Wants({t}));
        calls = Frame(d, g, 1000);
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].function == kBase + addr::kEquip);
        CHECK(calls[0].x[1] == 5u);
    }
    Director off;
    off.SetNullReadsZero(false);
    CHECK(off.LeftOff("Weapon_Range_Hookshot"));
    CHECK(off.LeftOff("Weapon_Range_Longshot"));
    CHECK_FALSE(off.LeftOff("Weapon_Range_SlingShot"));
}
TEST_CASE("puppet gear: a mask taken off is taken off with the game's call, a held item is only put away") {
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x1000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    Target t = At(1, 4, 5, 6);
    t.gear[CoopPuppet::kGearHeld] = "Weapon_Range_SlingShot";
    t.gear[2] = "Armor_Child_Mask_Skull";
    d.SetInput(Wants({t}));
    auto calls = Frame(d, g, 20);
    REQUIRE(calls.size() == 2);  // the mask and the slingshot

    // The slingshot's slot empties: put away as its player's, nothing made again.
    t.gear[CoopPuppet::kGearHeld] = "";
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 100).empty());
    CHECK(Frame(d, g, 3000).empty());

    // The mask comes off: a moment of "" (a load) is nothing...
    t.gear[2] = "";
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 3100).empty());
    t.gear[2] = "Armor_Child_Mask_Skull";
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 3200).empty());  // still worn: no call
    t.gear[2] = "";
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 3300).empty());
    CHECK(Frame(d, g, 3300 + Director::kTakeOffMs - 10).empty());
    // ...gone this long, the game's own call takes it off (head: index 0).
    calls = Frame(d, g, 3300 + Director::kTakeOffMs);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kUnequipArmor);
    CHECK(calls[0].x[0] == kEu);
    CHECK(calls[0].x[1] == 0u);
    CHECK(calls[0].x[2] == 0u);
    // Taken off: no remake, nothing more.
    CHECK(Frame(d, g, 3300 + Director::kTakeOffMs + 10).empty());
    CHECK(Frame(d, g, 3300 + Director::kTakeOffMs + 5000).empty());
    // Put on again later: worn with the equip call.
    t.gear[2] = "Armor_Child_Mask_Bunny";
    d.SetInput(Wants({t}));
    calls = Frame(d, g, 9000);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kEquip);
    CHECK(calls[0].x[1] == 7u);
}

TEST_CASE("puppet gear: a mask worn is shown with the game's own call, the hair put back when it comes off (#91)") {
    Game g;
    g.equip_result = 1;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x1000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    Target t = At(1, 4, 5, 6);
    t.gear[CoopPuppet::kGearHead] = "Armor_Child_Mask_Skull";
    d.SetInput(Wants({t}));
    auto calls = Frame(d, g, 20);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].function == kBase + addr::kEquip);
    // 0x33abfa0(owner) reads the EquipmentUser at owner + 0x470: a cell of the block
    CHECK(calls[1].function == kBase + addr::kShowHeadArmor);
    CHECK(calls[1].x[0] + addr::kShowHeadArmorEu == kHeap + block::kHeadOwnerEu);
    CHECK(g.guest.Get<std::uint64_t>(kHeap + block::kHeadOwnerEu) == kEu);
    CHECK(Frame(d, g, 100).empty());   // once

    // Another mask: worn and shown again.
    t.gear[CoopPuppet::kGearHead] = "Armor_Child_Mask_Bunny";
    d.SetInput(Wants({t}));
    calls = Frame(d, g, 200);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].function == kBase + addr::kEquip);
    CHECK(calls[1].function == kBase + addr::kShowHeadArmor);

    // Taken off: the game's take-off, then the hair as without a mask, once.
    t.gear[CoopPuppet::kGearHead] = "";
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 300).empty());
    calls = Frame(d, g, 300 + Director::kTakeOffMs);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].function == kBase + addr::kUnequipArmor);
    CHECK(calls[1].function == kBase + addr::kHeadArmorHairBack);
    CHECK(calls[1].x[0] == kEu);
    CHECK(Frame(d, g, 400 + Director::kTakeOffMs).empty());

    // A piece the equip call did not wear is not shown.
    g.equip_result = 0;
    t.gear[CoopPuppet::kGearHead] = "Armor_Child_Mask_Keaton";
    d.SetInput(Wants({t}));
    calls = Frame(d, g, 9000);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kEquip);
}

TEST_CASE("puppet gear: the head show call's addresses are the game's (#91)") {
    // 0x33abfa0: eu = [x0 + 0x470]; byte eu + 0x13c4 = (byte & ~3) | 2; 0x4b84b0(eu).
    CHECK(addr::kShowHeadArmor == 0x33abfa0u);
    CHECK(addr::kShowHeadArmorEu == 0x470u);
    CHECK(addr::kHeadArmorHairBack == 0x4b8240u);
    // the armor links' shown bytes follow the 8 hand slots' bytes
    CHECK(addr::kArmorShownBytes == addr::kEquipSlotBits + 8);
    CHECK(CoopPuppet::kGearKinds[CoopPuppet::kGearHead] == addr::kArmorKindFirst);
}

TEST_CASE("puppet gear: tunic, boots and gauntlets come off by their armor index") {
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x1000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    Target t = At(1, 4, 5, 6);
    t.gear[3] = "Armor_Adult_Goron";
    t.gear[4] = "Armor_Adult_HoverBoot";
    t.gear[CoopPuppet::kGearHand] = "Armor_Adult_Gauntlet_Silver";
    d.SetInput(Wants({t}));
    REQUIRE(Frame(d, g, 20).size() == 3);
    t.gear[3] = "";
    t.gear[4] = "";
    t.gear[CoopPuppet::kGearHand] = "";
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 100).empty());
    auto calls = Frame(d, g, 100 + Director::kTakeOffMs);
    REQUIRE(calls.size() == 3);
    CHECK(calls[0].function == kBase + addr::kUnequipArmor);
    CHECK(calls[0].x[1] == 1u);  // upper
    CHECK(calls[1].x[1] == 2u);  // lower
    CHECK(calls[2].x[1] == 3u);  // hand
    CHECK(Frame(d, g, 200 + Director::kTakeOffMs).empty());
}

TEST_CASE("puppet gear: the child's Goron's Bracelet is worn as Accessory2, as the game wears it") {
    using namespace CoopPuppet;
    CHECK(GearKind(kGearHand, "Armor_Child_Gauntlet_Goron") == 14u);
    CHECK(GearKind(kGearHand, "Armor_Adult_Gauntlet_Silver") == 10u);
    CHECK(GearKind(kGearHand, "Armor_Adult_Gauntlet_Gold") == 10u);
    CHECK(GearKind(kGearHand, "") == 10u);
    CHECK(GearKind(kGearHeld, "Armor_Child_Gauntlet_Goron") == 5u);  // only the hand piece
    CHECK(GearIsArmor(kGearHand, "Armor_Adult_Gauntlet_Silver"));
    CHECK_FALSE(GearIsArmor(kGearHand, "Armor_Child_Gauntlet_Goron"));
    CHECK_FALSE(GearIsArmor(kGearOcarina, "Item_Ocarina_Fairy"));
    CHECK(GearIsArmor(2, "Armor_Child_Mask_Keaton"));

    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x1000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    Target t = At(1, 4, 5, 6);
    t.gear[kGearHand] = "Armor_Child_Gauntlet_Goron";
    d.SetInput(Wants({t}));
    auto calls = Frame(d, g, 20);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kEquip);
    CHECK(calls[0].x[0] == kEu);
    CHECK(calls[0].x[1] == 14u);
    CHECK(calls[0].x[3] == 2u);  // worn as the belt pieces are
    CHECK(Frame(d, g, 30).empty());  // worn: not again
    // Taken off: no armor index for an accessory, so the puppet is made again.
    t.gear[kGearHand] = "";
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 100).empty());
    CHECK(Frame(d, g, 100 + Director::kTakeOffMs).empty());
    calls = Frame(d, g, 110 + Director::kTakeOffMs);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kRequestDelete);
}

TEST_CASE("puppet gear: each hand item is worn in the same hand slot as on its player") {
    using namespace CoopPuppet;
    // The game's two enums (rodata): the 8 hand slots (0x3ef2000) and the 18
    // slot kinds of the equip call (0x3ef9490). A piece sent from hand slot
    // n is worn with the kind of the same name, so it lands in slot n on the
    // puppet whichever item it is.
    const char* const slot_names[8] = {"Weapon", "SubWeapon", "ThirdWeapon", "Bow",
                                       "Shield", "Tool",      "SubTool",     "Helmet"};
    const char* const kind_names[18] = {
        "Weapon",     "SubWeapon",  "ThirdWeapon", "Sheath",     "Quiver",     "Bow",
        "Shield",     "ArmorHead",  "ArmorUpper",  "ArmorLower", "ArmorHand",  "Tool",
        "SubTool",    "Accessory1", "Accessory2",  "Accessory3", "Accessory4", "Extra"};
    for (std::size_t i = 0; i < kGearCount; ++i) {
        if (kGearSlots[i] >= 0) {
            CAPTURE(i);
            CHECK(std::string(kind_names[kGearKinds[i]]) == slot_names[kGearSlots[i]]);
        }
    }
    // Item by item: the piece that carries it and the kind it is worn with.
    // Only the rows marked (run) are slots seen on a player. The hookshot,
    // longshot, boomerang and hammer have not been seen in a slot: their
    // rows put them in slots of our choosing, and only show that any slot
    // routes the same way (GearOf and kGearKinds do not look at the name).
    struct Item {
        const char* name;
        std::optional<std::string> coop::protocol::Equipment::*field;
        std::size_t piece;
        std::uint32_t kind;
    };
    using E = coop::protocol::Equipment;
    const Item items[] = {
        {"Weapon_Bottle", &E::sub_weapon, kGearSubWeapon, 1},  // held up: slot 1 (run)
        {"Weapon_Bottle_Fairy", &E::sub_weapon, kGearSubWeapon, 1},
        {"Weapon_DekuStick", &E::sub_weapon, kGearSubWeapon, 1},  // slot 1 (run)
        {"Weapon_DekuStick", &E::weapon_slot, 0, 0},  // or in the sword hand
        {"Weapon_Range_SlingShot", &E::held, kGearHeld, 5},  // slot 3 (run)
        {"Weapon_Range_Bow", &E::held, kGearHeld, 5},  // a BowParam like the slingshot: not run
        {"Weapon_Range_Hookshot", &E::tool, kGearTool, 11},  // slot chosen here
        {"Weapon_Range_Longshot", &E::third_weapon, kGearThirdWeapon, 2},  // chosen here
        {"Weapon_Boomerang", &E::tool, kGearTool, 11},  // chosen here
        {"Weapon_Hammer", &E::sub_tool, kGearSubTool, 12},  // chosen here
        {"Armor_Child_Mask_Keaton", &E::head, 2, 7},  // a mask is head armor
    };
    for (const Item& it : items) {
        CAPTURE(it.name);
        E e;
        e.*it.field = it.name;
        const Gear g = GearOf(e);
        REQUIRE(g[it.piece]);
        CHECK(*g[it.piece] == it.name);
        CHECK(GearKind(it.piece, it.name) == it.kind);
        for (std::size_t i = 0; i < kGearCount; ++i) {
            if (i != it.piece) {
                CHECK_FALSE(g[i]);  // in no other piece
            }
        }
    }
    // Not covered here: Deku Nuts, bombs, bombchus and the Lens of Truth.
    // They are in no hand slot (thrown objects, which the objects sync
    // shows, and magic with no actor), so there is nothing to send and no
    // puppet test for them.
}

TEST_CASE("puppet gear: a refused take-off and a sword taken off make the puppet again") {
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x1000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    g.unequip_result = 0;
    Target t = At(1, 4, 5, 6);
    t.gear[2] = "Armor_Child_Mask_Skull";
    d.SetInput(Wants({t}));
    REQUIRE(Frame(d, g, 20).size() == 1);
    t.gear[2] = "";
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 100).empty());
    auto calls = Frame(d, g, 100 + Director::kTakeOffMs);
    // Asked and refused; the next frame deletes it, to make it again.
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kUnequipArmor);
    calls = Frame(d, g, 110 + Director::kTakeOffMs);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kRequestDelete);

    // A sword has no take-off call: made again without it.
    Game g2;
    Director d2;
    MakeLive(d2, g2);
    g2.guest.Map(kParts, 0x1000);
    g2.guest.Set(kActor + addr::kActorParts, kParts);
    g2.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    Target t2 = At(1, 4, 5, 6);
    t2.gear[0] = "Weapon_Sword_Kokiri";
    d2.SetInput(Wants({t2}));
    REQUIRE(Frame(d2, g2, 20).size() == 1);
    t2.gear[0] = "";
    d2.SetInput(Wants({t2}));
    CHECK(Frame(d2, g2, 100).empty());
    CHECK(Frame(d2, g2, 100 + Director::kTakeOffMs).empty());
    calls = Frame(d2, g2, 110 + Director::kTakeOffMs);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kRequestDelete);
}

TEST_CASE("puppet gear: a worn piece goes into the hand and back as its player's does") {
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x3000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    auto slot_id = [&](int s) {
        return kEu + addr::kEquipSlots + addr::kEquipSlotStride * s + addr::kEquipSlotId;
    };
    g.guest.Set(slot_id(4), std::uint32_t{0xFFFFFFFF});  // no shield actor yet
    Target t = At(1, 4, 5, 6);
    t.gear[0] = "Weapon_Sword_Kokiri";
    t.gear[1] = "Weapon_Shield_Deku";
    t.drawn = std::uint8_t{0};
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 20).size() == 2);  // worn, put away
    CHECK(Frame(d, g, 30).empty());      // as its player: nothing asked

    // The sword is drawn: asked into the hand until the game takes it.
    t.drawn = std::uint8_t{0x11};
    d.SetInput(Wants({t}));
    auto calls = Frame(d, g, 40);
    REQUIRE(calls.size() == 1);  // the shield has no actor: not asked
    CHECK(calls[0].function == kBase + addr::kEquipChangeState);
    CHECK(calls[0].x[0] == kEu);
    CHECK(calls[0].x[1] == 0u);
    CHECK(calls[0].x[2] == addr::kEquipStateInHand);
    CHECK(Frame(d, g, 50).empty());  // not taken: waits before asking again
    g.guest.Set(kEu + addr::kEquipSlotBits + 0, addr::kEquipBitInHand);
    CHECK(Frame(d, g, 400).size() == 1);
    CHECK(Frame(d, g, 410).empty());  // taken: not again

    // The shield's actor is there now.
    g.guest.Set(slot_id(4), std::uint32_t{7});
    g.guest.Set(kEu + addr::kEquipSlotBits + 4, addr::kEquipBitInHand);
    calls = Frame(d, g, 420);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].x[1] == 4u);
    CHECK(calls[0].x[2] == addr::kEquipStateInHand);

    // Put away again.
    t.drawn = std::uint8_t{0x10};
    d.SetInput(Wants({t}));
    g.guest.Set(kEu + addr::kEquipSlotBits + 0, addr::kEquipBitAway);
    calls = Frame(d, g, 430);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].x[1] == 0u);
    CHECK(calls[0].x[2] == addr::kEquipStateAway);
    CHECK(Frame(d, g, 440).empty());

    // A new shield is worn put away: asked into the hand again.
    t.gear[1] = "Weapon_Shield_Hylian";
    d.SetInput(Wants({t}));
    calls = Frame(d, g, 450);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].function == kBase + addr::kEquip);
    CHECK(calls[1].function == kBase + addr::kEquipChangeState);
    CHECK(calls[1].x[1] == 4u);

    // Drawn bits not known: nothing asked.
    t.drawn.reset();
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 460).empty());
}

TEST_CASE("puppet move: the game's setter once a frame when asked for") {
    Game g;
    Director d;
    d.SetMatrixCall(true);
    MakeLive(d, g);
    d.SetInput(Wants({At(1, 4, 5, 6)}));
    // kVtable is not the actor class the setter is known for: not called.
    CHECK(Frame(d, g, 20).empty());

    // An actor of that class: once a frame, after the transform is written.
    Game g2;
    Director d2;
    d2.SetMatrixCall(true);
    d2.SetInput(Wants({At(1, 4, 5, 6)}));
    auto calls = Frame(d2, g2, 0);
    REQUIRE(calls.size() == 2);
    g2.Spawn(calls[1].x[3]);
    g2.guest.Set(kActor, kBase + addr::kActorVtable);
    Frame(d2, g2, 10);
    Frame(d2, g2, 15);
    REQUIRE(d2.Slots()[0].phase == Phase::live);
    for (std::uint64_t now : {20, 30}) {
        calls = Frame(d2, g2, now);
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].function == kBase + addr::kSetMatrix);
        CHECK(calls[0].x[0] == kActor);
        CHECK(calls[0].x[1] == 0u);
    }
    CheckPosition(g2.guest, kActor + addr::kActorPosition, 4, 5, 6);
}


TEST_CASE("puppet: parking a hidden puppet takes its held pieces with it") {
    // A held piece is an actor of its own, and the game keeps stepping it. Parked with
    // the puppet left behind, the game follows a piece whose owner is 500 m below it;
    // seen 2026-11-15 as the other player changed to the Hookshot while the puppet came
    // and went: 0xc0000005 reading a null + 0x7d8 on a guest thread.
    Game g;
    Director d;
    MakeLive(d, g);

    // The puppet's equipment: one slot with a piece actor in it (after MakeLive: it
    // writes the actor's own state as the game does).
    const std::uint64_t kParts = 0x500000000;
    const std::uint64_t kEu = 0x500001000;
    const std::uint64_t kHolder = 0x500002000;
    const std::uint64_t kPiece = 0x500003000;
    g.guest.Map(kParts, 0x400);
    g.guest.Map(kEu, 0x1600);
    g.guest.Map(kHolder, 0x100);
    g.guest.Map(kPiece, 0x600);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    g.guest.Set(kEu + addr::kEquipSlots + addr::kEquipSlotStride * 3 + addr::kEquipSlotId,
                std::uint32_t{0x1234});
    g.guest.Set(kEu + addr::kEquipSlots + addr::kEquipSlotStride * 3 + addr::kEquipSlotHolder, kHolder);
    g.guest.Set(kHolder + addr::kHolderActor, kPiece);
    g.guest.Set(kPiece + addr::kActorState, std::uint32_t{addr::kStatePresent});
    const float at[3] = {4.0f, 5.0f, 6.0f};
    for (std::uint64_t i = 0; i < 3; ++i) {
        g.guest.Set(kPiece + addr::kActorPosition + 4 * i, at[i]);
    }

    // Parked: the piece moves down with it, not left where the puppet was.
    Target hidden = At(1, 40, 5, 6);
    hidden.show = CoopPuppet::Show::hide;
    // (the piece's own record: what the test wrote must still be there)
    CHECK(g.guest.Get<std::uint64_t>(kActor + addr::kActorParts) == kParts);
    CHECK(g.guest.Get<std::uint64_t>(kParts + addr::kPartsEquipmentUser) == kEu);
    CHECK(g.guest.Get<std::uint32_t>(kEu + addr::kEquipSlots + addr::kEquipSlotStride * 3 +
                                    addr::kEquipSlotId) == 0x1234u);
    d.SetInput(Wants({hidden}));
    Frame(d, g, 100);
    CheckPosition(g.guest, kActor + addr::kActorPosition, 40, 5 - Director::kHideDrop, 6);
    CheckPosition(g.guest, kPiece + addr::kActorPosition, 40, 5 - Director::kHideDrop, 6);

    // Shown again at their place: the puppet comes back up (the piece follows the
    // game's own bone bind from there, so it is not parked again).
    Target shown = At(1, 40, 5, 6);
    d.SetInput(Wants({shown}));
    Frame(d, g, 200);
    CheckPosition(g.guest, kActor + addr::kActorPosition, 40, 5, 6);
}

TEST_CASE("puppet scene filter: hold freezes its frame and follows its place, hide parks it") {
    Game g;
    Director d;
    MakeLive(d, g);
    GiveAnimation(g, 9.0f);
    d.SetInput(Wants({Playing("Wait", 9.0f, 1)}));
    Frame(d, g, 20);
    Frame(d, g, 30);
    CheckPosition(g.guest, kActor + addr::kActorPosition, 4, 5, 6);

    // Hold: its frame set back to where it was every frame, and (issue 194) it follows their
    // place, so it is not left behind for a snap at the end. A jump while held is a real
    // teleport (their samples move smoothly in a cutscene): drawn at once, not eased.
    // puppet-anim: their cutscene's command is tried first; the puppet's slot 0 does not name
    // it the next frame (it has no such animation), so it is held at its frame from then on.
    Target held = Playing("Walk", 2.0f, 2, CoopPuppet::Show::hold);
    held.position = {6, 5, 6};
    d.SetInput(Wants({held}));
    g.guest.Set(kSlot0 + addr::kSlotFrame, 11.0f);
    auto tried = Frame(d, g, 35);
    REQUIRE_FALSE(tried.empty());
    CHECK(tried[0].function == kBase + addr::kPlayAs);
    for (std::uint64_t now : {40, 50}) {
        auto calls = Frame(d, g, now);
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].function == kBase + addr::kSetAsFrame);
        CHECK(calls[0].s0 == 11.0f);
        g.guest.Set(kSlot0 + addr::kSlotFrame, 12.0f);
    }
    CheckPosition(g.guest, kActor + addr::kActorPosition, 6, 5, 6);
    CHECK(d.Slots()[0].anim_name.empty());  // played again once the hold ends
    // Their event walks them on: followed.
    held.position = {6.2f, 5, 6};
    d.SetInput(Wants({held}));
    Frame(d, g, 100);
    CheckPosition(g.guest, kActor + addr::kActorPosition, 6.2f, 5, 6);

    // Hide: parked below its spot, no animation calls, not deleted.
    Target hidden = Playing("Walk", 2.0f, 2, CoopPuppet::Show::hide);
    hidden.position = {40, 5, 6};
    d.SetInput(Wants({hidden}));
    CHECK(Frame(d, g, 400).empty());
    CheckPosition(g.guest, kActor + addr::kActorPosition, 40, 5 - Director::kHideDrop, 6);
    CHECK(d.Slots()[0].phase == Phase::live);

    // Shown again: moves, and plays what it missed.
    held.show = CoopPuppet::Show::show;
    held.position = {40, 5, 6};
    d.SetInput(Wants({held}));
    auto calls = Frame(d, g, 410);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].function == kBase + addr::kPlayAs);
    CheckPosition(g.guest, kActor + addr::kActorPosition, 40, 5, 6);  // where it was hidden: no ease
}

TEST_CASE("puppet animation: in their cutscene the puppet plays their command when it has it (item-get, puppet-anim)") {
    Game g;
    Director d;
    MakeLive(d, g);
    GiveAnimation(g, 9.0f);
    d.SetInput(Wants({Playing("Wait", 9.0f, 1)}));
    Frame(d, g, 20);
    Frame(d, g, 30);

    // Their chest opens: the item-get command arrives with the hold. It is played, and the
    // puppet's slot 0 names it (the game took it), so it is not frozen: no calls after that.
    Target held = Playing("DemoItemGet_t36", 0.0f, 2, CoopPuppet::Show::hold);
    d.SetInput(Wants({held}));
    auto calls = Frame(d, g, 40);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].function == kBase + addr::kPlayAs);
    CHECK(calls[1].function == kBase + addr::kSetAsFrame);
    g.guest.Set(kSlot0 + addr::kSlotName, g.guest.Get<std::uint64_t>(calls[0].x[0] + 0x08));
    for (std::uint64_t now : {50, 60, 70}) {
        CHECK(Frame(d, g, now).empty());
    }
    CHECK(d.Slots()[0].anim_name == "DemoItemGet_t36");
}

TEST_CASE("puppet: LiveMask has a live puppet shown or held, not one parked or gone (issue 145)") {
    Game g;
    Director d;
    CHECK(d.LiveMask() == 0u);
    MakeLive(d, g);
    d.SetInput(Wants({Playing("Wait", 9.0f, 1)}));
    Frame(d, g, 20);
    REQUIRE(d.Slots()[0].phase == Phase::live);
    const std::uint32_t bit = 1u << d.Slots()[0].number;
    CHECK(d.LiveMask() == bit);

    Target held = Playing("Walk", 2.0f, 2, CoopPuppet::Show::hold);
    d.SetInput(Wants({held}));
    Frame(d, g, 30);
    CHECK(d.LiveMask() == bit);  // held: still there to see

    Target hidden = Playing("Walk", 2.0f, 2, CoopPuppet::Show::hide);
    d.SetInput(Wants({hidden}));
    Frame(d, g, 40);
    CHECK(d.LiveMask() == 0u);  // parked under the floor

    held.show = CoopPuppet::Show::show;
    d.SetInput(Wants({held}));
    Frame(d, g, 50);
    CHECK(d.LiveMask() == bit);

    d.SetInput(Wants({}));  // the player left: kept for the grace, but not shown
    Frame(d, g, 60);
    CHECK(d.Slots()[0].phase == Phase::live);
    CHECK(d.LiveMask() == 0u);
}

TEST_CASE("puppet planner: scene and cutscene rules, actor by age, animation passed on") {
    CoopPuppet::Planner p;
    std::vector<CoopPuppet::Remote> remotes(3);
    remotes[0].number = 1;
    remotes[0].scene = "Kokiri";
    remotes[0].anim = CoopPuppet::Anim{"Wait", 3.0f, 1.0f};
    remotes[0].anim_serial = 7;
    remotes[1].number = 2;
    remotes[1].scene = "Kokiri";
    remotes[1].adult = true;
    remotes[1].status = coop::protocol::status::in_cutscene;
    remotes[2].number = 3;  // scene not known

    auto in = p.Plan(kBase, std::string("Kokiri"), std::nullopt, false, remotes, 0);
    REQUIRE(in.targets.size() == 2);
    CHECK(in.targets[0].show == CoopPuppet::Show::show);
    CHECK(in.targets[0].actor == "CoopPuppet_Child_Blue");  // player 1: blue
    REQUIRE(in.targets[0].anim);
    CHECK(in.targets[0].anim->name == "Wait");
    CHECK(in.targets[0].anim_serial == 7u);
    CHECK(in.targets[1].show == CoopPuppet::Show::hold);  // their cutscene
    CHECK(in.targets[1].actor == "CoopPuppet_Adult_Red");  // player 2: red

    // Our cutscene: every puppet here kept but hidden.
    in = p.Plan(kBase, std::string("Kokiri"), std::nullopt, true, remotes, 10);
    REQUIRE(in.targets.size() == 2);
    CHECK(in.targets[0].show == CoopPuppet::Show::hide);
    CHECK(in.targets[1].show == CoopPuppet::Show::hide);

    // An actor named in the settings wins over age.
    CoopPuppet::PlannerSettings s;
    s.actor = "Animal_Cow_LinkHouse";
    CoopPuppet::Planner cow(s);
    in = cow.Plan(kBase, std::string("Kokiri"), std::nullopt, false, remotes, 0);
    REQUIRE(in.targets.size() == 2);
    CHECK(in.targets[1].actor == "Animal_Cow_LinkHouse");
}

TEST_CASE("puppet planner: riding shows (issue 199), the Ocarina plays through their cutscene") {
    CoopPuppet::Planner p;
    std::vector<CoopPuppet::Remote> remotes(3);
    remotes[0].number = 1;
    remotes[0].scene = "HyruleField_Adult";
    remotes[0].adult = true;
    remotes[0].anim = CoopPuppet::Anim{"HorseMove", 0.0f, 1.0f};
    remotes[1].number = 2;
    remotes[1].scene = "HyruleField_Child";
    remotes[1].status = coop::protocol::status::in_cutscene;
    remotes[1].anim = CoopPuppet::Anim{"OcarinaReadyStart", 0.0f, 1.0f};
    remotes[2].number = 3;
    remotes[2].scene = "HyruleField_Child";
    remotes[2].status = coop::protocol::status::in_cutscene;
    remotes[2].anim = CoopPuppet::Anim{"DemoOcarinaMelodyGet", 0.0f, 1.0f};  // a song taught

    auto in = p.Plan(kBase, std::string("HyruleField_Child"), std::nullopt, false, remotes, 0);
    REQUIRE(in.targets.size() == 3);
    CHECK(in.targets[0].show == CoopPuppet::Show::show);  // on its horse copy, never parked
    CHECK(std::string(in.targets[0].why) == "riding");
    CHECK(in.targets[1].show == CoopPuppet::Show::show);
    CHECK(std::string(in.targets[1].why) == "ocarina");
    CHECK(in.targets[2].show == CoopPuppet::Show::hold);
    CHECK(std::string(in.targets[2].why) == "their cutscene");

    // Getting off: shown again.
    remotes[0].anim = CoopPuppet::Anim{"DismountHorse", 0.0f, 1.0f};
    in = p.Plan(kBase, std::string("HyruleField_Child"), std::nullopt, false, remotes, 10);
    CHECK(in.targets[0].show == CoopPuppet::Show::show);

    // Our cutscene hides them all, with why.
    in = p.Plan(kBase, std::string("HyruleField_Child"), std::nullopt, true, remotes, 20);
    REQUIRE(in.targets.size() == 3);
    for (const auto& t : in.targets) {
        CHECK(t.show == CoopPuppet::Show::hide);
        CHECK(std::string(t.why) == "our cutscene");
    }
}

TEST_CASE("puppet show changes are logged once each, with why") {
    std::vector<std::string> log;
    Game g;
    Director d([&](const std::string& line) { log.push_back(line); });
    MakeLive(d, g);
    GiveAnimation(g, 9.0f);
    d.SetInput(Wants({Playing("Wait", 9.0f, 1)}));
    Frame(d, g, 20);
    auto count = [&](const std::string& text) {
        return std::count_if(log.begin(), log.end(), [&](const std::string& l) {
            return l.find(text) != std::string::npos;
        });
    };
    CHECK(count("show -> ") == 0);
    Target parked = Playing("Wait", 9.0f, 1, CoopPuppet::Show::hide);
    parked.why = "riding";
    d.SetInput(Wants({parked}));
    Frame(d, g, 30);
    Frame(d, g, 40);
    CHECK(count("P1 show -> hide (riding)") == 1);
    d.SetInput(Wants({Playing("Wait", 9.0f, 1)}));
    Frame(d, g, 50);
    CHECK(count("P1 hide -> show") == 1);
}

namespace {
CoopPuppet::Input Drawing(std::vector<Target> targets, std::uint64_t drawn_since_ms,
                          std::string scene = "KokiriForest_Child") {
    auto in = Wants(std::move(targets), std::move(scene));
    in.camera = true;
    in.drawn_since_ms = drawn_since_ms;
    return in;
}
}  // namespace

TEST_CASE("puppet join: no create while the game loads, at once when it draws") {
    Game g;
    Director d;
    // The scene is readable at 0 but the loading screen has no camera.
    d.SetInput(Drawing({At(1, 0, 0, 0)}, 0));
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() == 1);  // the block only
    CHECK(calls[0].function == kBase + addr::kMalloc);
    CHECK(Frame(d, g, 3000).empty());
    CHECK(d.Slots()[0].phase == Phase::idle);
    // Drawn again from 3400: not yet...
    d.SetInput(Drawing({At(1, 0, 0, 0)}, 3400));
    CHECK(Frame(d, g, 3400 + Director::kDrawnMs - 1).empty());
    // ...then created, in the same scene, no retry wait.
    calls = Frame(d, g, 3400 + Director::kDrawnMs);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kCreate);
}

TEST_CASE("puppet join: drawing that never broke over a scene change waits the steady time") {
    Game g;
    Director d;
    d.SetInput(Drawing({}, 100, "A"));
    Frame(d, g, 1000);
    // New scene at 2000, the camera unbroken since 100 (begun before it).
    d.SetInput(Drawing({At(1, 0, 0, 0)}, 100, "B"));
    auto calls = Frame(d, g, 2000);
    REQUIRE(calls.size() == 1);  // the block only
    CHECK(Frame(d, g, 2000 + Director::kSteadyMs - 1).empty());
    calls = Frame(d, g, 2000 + Director::kSteadyMs);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kCreate);
}

TEST_CASE("puppet join: one already drawing creates at once (the other player joins)") {
    Game g;
    Director d;
    d.SetInput(Drawing({}, 100));
    Frame(d, g, 50);
    d.SetInput(Drawing({At(1, 0, 0, 0)}, 100));
    const auto calls = Frame(d, g, 60000);
    REQUIRE(calls.size() == 2);
    CHECK(calls[1].function == kBase + addr::kCreate);
}

TEST_CASE("puppet join: deleted by a load before the camera goes, made again after it") {
    Game g;
    Director d;
    d.SetInput(Drawing({At(1, 0, 0, 0)}, 100));
    CHECK(Frame(d, g, 50).size() == 1);  // the block; not drawn yet
    auto calls = Frame(d, g, 1000);
    REQUIRE(calls.size() == 1);
    g.Spawn(calls[0].x[3]);
    Frame(d, g, 1010);
    Frame(d, g, 1015);
    REQUIRE(d.Slots()[0].phase == Phase::live);
    // The engine deletes it at 5000; the camera is still fresh then.
    g.guest.Set(kActor + addr::kActorState, std::uint32_t{7});
    CHECK(Frame(d, g, 5000).empty());
    CHECK(d.Slots()[0].phase == Phase::idle);
    // The loading screen, then drawn from 9000.
    d.SetInput(Drawing({At(1, 0, 0, 0)}, 0));
    CHECK(Frame(d, g, 5100).empty());
    d.SetInput(Drawing({At(1, 0, 0, 0)}, 9000));
    CHECK(Frame(d, g, 9000 + Director::kDrawnMs - 1).empty());
    calls = Frame(d, g, 9000 + Director::kDrawnMs);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kCreate);
}

TEST_CASE("puppet join: a dropped create of the wrong age is followed by the right one at once") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Adult")}));
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() == 2);
    g.Request(calls[1].x[3]);
    g.guest.Set(kRequest, std::uint32_t{addr::kRequestDrop});
    calls = Frame(d, g, 100);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kDropCreate);
    REQUIRE(d.Slots()[0].phase == Phase::idle);
    // The same actor waits 5 s...
    CHECK(Frame(d, g, 200).empty());
    // ...the child, now that the age is read right, does not.
    d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Child")}));
    calls = Frame(d, g, 300);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kCreate);
    CHECK(d.Slots()[0].actor_name == "CoopPuppet_Child");
}

TEST_CASE("puppet join: the wanted actor must settle before a create (age read while loading)") {
    Game g;
    Director d;
    auto in = Wants({At(1, 0, 0, 0, "CoopPuppet_Adult")});
    in.settle_ms = 1000;
    d.SetInput(in);
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() == 1);  // the block only
    CHECK(calls[0].function == kBase + addr::kMalloc);
    CHECK(Frame(d, g, 500).empty());
    // The age is read right at 600: the wait starts again, no adult is made.
    in = Wants({At(1, 0, 0, 0, "CoopPuppet_Child")});
    in.settle_ms = 1000;
    d.SetInput(in);
    CHECK(Frame(d, g, 600).empty());
    CHECK(Frame(d, g, 1599).empty());
    calls = Frame(d, g, 1600);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kCreate);
    CHECK(d.Slots()[0].actor_name == "CoopPuppet_Child");
}

TEST_CASE("puppet gear: the held item is worn as a bow and follows drawn bit 3") {
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x3000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    const std::uint64_t id3 =
        kEu + addr::kEquipSlots + addr::kEquipSlotStride * 3 + addr::kEquipSlotId;
    g.guest.Set(id3, std::uint32_t{0xFFFFFFFF});
    Target t = At(1, 4, 5, 6);
    t.gear[CoopPuppet::kGearHeld] = "";  // nothing held: nothing asked
    t.drawn = std::uint8_t{0};
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 20).empty());

    t.gear[CoopPuppet::kGearHeld] = "Weapon_Range_SlingShot";
    d.SetInput(Wants({t}));
    auto calls = Frame(d, g, 30);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kEquip);
    CHECK(calls[0].x[0] == kEu);
    CHECK(calls[0].x[1] == 5u);  // the bow kind
    CHECK(calls[0].x[2] == kHeap + block::kGearPtr + 8 * CoopPuppet::kGearHeld);
    std::uint64_t text = 0;
    text = g.guest.Get<std::uint64_t>(calls[0].x[2]);
    CHECK(text == kHeap + block::kGearText + block::kGearCapacity * CoopPuppet::kGearHeld);
    CHECK(text + block::kGearCapacity <= kHeap + block::kGearPtr);  // names end before the cells
    CHECK(block::kGearPtr + 8 * CoopPuppet::kGearCount <= block::kSize);
    CHECK(Frame(d, g, 40).empty());  // worn put away, as its player has it

    // Its player aims: into the hand, once its actor is there.
    t.drawn = std::uint8_t{0x08};
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 50).empty());
    g.guest.Set(id3, std::uint32_t{9});
    g.guest.Set(kEu + addr::kEquipSlotBits + 3, addr::kEquipBitInHand);
    calls = Frame(d, g, 60);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kEquipChangeState);
    CHECK(calls[0].x[1] == 3u);
    CHECK(calls[0].x[2] == addr::kEquipStateInHand);
    CHECK(Frame(d, g, 70).empty());

    // Put away, also when the player's slot is empty by then.
    t.drawn = std::uint8_t{0};
    t.gear[CoopPuppet::kGearHeld] = "";
    d.SetInput(Wants({t}));
    g.guest.Set(kEu + addr::kEquipSlotBits + 3, addr::kEquipBitAway);
    calls = Frame(d, g, 80);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].x[1] == 3u);
    CHECK(calls[0].x[2] == addr::kEquipStateAway);
    CHECK(Frame(d, g, 90).empty());
}

TEST_CASE("puppet gear: slingshot out, then the sword: the slingshot goes away, the sword in hand") {
    // held-fix (owner, 0.11): the puppet's slingshot stayed out in the other
    // hand when its player used the sword, and the sword never showed in hand.
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x3000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    auto slot_id = [&](int s) {
        return kEu + addr::kEquipSlots + addr::kEquipSlotStride * s + addr::kEquipSlotId;
    };
    auto take = [&](int s, bool in_hand) {  // the game takes a change
        g.guest.Set(kEu + addr::kEquipSlotBits + s,
                    in_hand ? addr::kEquipBitInHand : addr::kEquipBitAway);
        g.slot_state[s] = in_hand ? addr::kEquipStateInHand : addr::kEquipStateAway;
    };
    for (int s = 0; s < 8; ++s) g.guest.Set(slot_id(s), std::uint32_t{0xFFFFFFFF});
    g.guest.Set(slot_id(0), std::uint32_t{3});  // the sword
    g.guest.Set(slot_id(3), std::uint32_t{4});  // the slingshot
    g.slot_state[0] = addr::kEquipStateAway;
    g.slot_state[3] = addr::kEquipStateAway;
    Target t = At(1, 4, 5, 6);
    t.gear[0] = "Weapon_Sword_Kokiri";
    t.gear[CoopPuppet::kGearHeld] = "Weapon_Range_SlingShot";
    t.drawn = std::uint8_t{0};
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 20).size() == 2);  // both worn, put away
    CHECK(Frame(d, g, 30).empty());

    // The slingshot out.
    t.drawn = std::uint8_t{0x08};
    d.SetInput(Wants({t}));
    auto calls = Frame(d, g, 40);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kEquipChangeState);
    CHECK(calls[0].x[1] == 3u);
    CHECK(calls[0].x[2] == addr::kEquipStateInHand);
    take(3, true);
    CHECK(Frame(d, g, 300).size() == 1);  // asked again, now seen taken
    CHECK(Frame(d, g, 310).empty());

    // The sword out (the reader sends the slingshot put away): both change.
    t.drawn = std::uint8_t{0x01};
    d.SetInput(Wants({t}));
    calls = Frame(d, g, 320);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].x[1] == 0u);
    CHECK(calls[0].x[2] == addr::kEquipStateInHand);
    take(0, true);
    calls = Frame(d, g, 600);
    REQUIRE(calls.size() == 2);  // the sword seen taken, then the slingshot
    CHECK(calls[1].x[1] == 3u);
    CHECK(calls[1].x[2] == addr::kEquipStateAway);
    take(3, false);
    CHECK(Frame(d, g, 900).size() == 1);
    CHECK(Frame(d, g, 910).empty());

    // The puppet's own animation puts its sword away: the game's state read
    // shows it, and the sword is asked into the hand again.
    g.slot_state[0] = addr::kEquipStateAway;
    g.guest.Set(kEu + addr::kEquipSlotBits + 0, std::uint8_t{0});
    g.state_checks.clear();
    calls.clear();
    std::uint64_t now = 1000;
    for (; now < 4000 && calls.empty(); now += 100) {
        calls = Frame(d, g, now);
    }
    REQUIRE(calls.size() == 1);
    CHECK(now <= 2600);  // within two check rounds
    CHECK(calls[0].function == kBase + addr::kEquipChangeState);
    CHECK(calls[0].x[1] == 0u);
    CHECK(calls[0].x[2] == addr::kEquipStateInHand);
    REQUIRE_FALSE(g.state_checks.empty());
    CHECK(g.state_checks.back().function == kBase + addr::kEquipState);
    CHECK(g.state_checks.back().x[0] == kEu);
    CHECK(g.state_checks.back().x[1] == 0u);
    take(0, true);

    // Both put away: the sword goes away (the game took the last ask late,
    // so a check finds it in hand), and nothing is asked again after.
    t.drawn = std::uint8_t{0};
    d.SetInput(Wants({t}));
    calls.clear();
    for (now = 3000; now < 6000 && calls.empty(); now += 100) {
        calls = Frame(d, g, now);
    }
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].x[1] == 0u);
    CHECK(calls[0].x[2] == addr::kEquipStateAway);
    take(0, false);
    CHECK(Frame(d, g, now + 300).size() == 1);  // asked again, now seen taken
    g.state_checks.clear();
    for (now += 400; now < 9000; now += 300) {
        CHECK(Frame(d, g, now).empty());  // checks only: the game has both as set
    }
    CHECK(g.state_checks.size() >= 4);
}

TEST_CASE("puppet gear: a piece its animation keeps undoing is set again, said once until it holds") {
    Game g;
    std::vector<std::string> log;
    Director d([&](const std::string& line) { log.push_back(line); });
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x3000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    for (int s = 0; s < 8; ++s) {
        g.guest.Set(kEu + addr::kEquipSlots + addr::kEquipSlotStride * s + addr::kEquipSlotId,
                    std::uint32_t{s == 0 ? 3u : 0xFFFFFFFFu});
    }
    g.guest.Set(kEu + addr::kEquipSlotBits + 0, addr::kEquipBitInHand);  // every ask taken
    g.slot_state[0] = addr::kEquipStateAway;  // but the game keeps putting it away
    Target t = At(1, 4, 5, 6);
    t.gear[0] = "Weapon_Sword_Kokiri";
    t.drawn = std::uint8_t{0x01};
    d.SetInput(Wants({t}));
    auto said = [&] {
        return std::count_if(log.begin(), log.end(), [](const std::string& l) {
            return l.find("not as set: set again") != std::string::npos;
        });
    };
    int asks = 0;
    for (std::uint64_t now = 20; now < 5000; now += 100) {
        for (const Call& c : Frame(d, g, now)) {
            asks += c.function == kBase + addr::kEquipChangeState ? 1 : 0;
        }
    }
    CHECK(asks >= 5);    // asked again each time a check finds it away
    CHECK(said() == 1);  // said once
    // It holds for a check, then is undone again: said again.
    g.slot_state[0] = addr::kEquipStateInHand;
    for (std::uint64_t now = 5000; now < 6500; now += 100) Frame(d, g, now);
    CHECK(said() == 1);
    g.slot_state[0] = addr::kEquipStateAway;
    for (std::uint64_t now = 6500; now < 8000; now += 100) Frame(d, g, now);
    CHECK(said() == 2);
}

TEST_CASE("puppet gear: a piece the game made anew is put into the hand again") {
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x3000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    const std::uint64_t sword_id = kEu + addr::kEquipSlots + addr::kEquipSlotId;
    const std::uint64_t record = kEu + addr::kEquipRecords + addr::kEquipRecordInHand;
    g.guest.Set(sword_id, std::uint32_t{3});
    Target t = At(1, 4, 5, 6);
    t.gear[0] = "Weapon_Sword_Kokiri";
    t.drawn = std::uint8_t{0x01};
    d.SetInput(Wants({t}));
    auto calls = Frame(d, g, 20);
    REQUIRE(calls.size() == 2);  // worn, then into the hand
    CHECK(calls[1].function == kBase + addr::kEquipChangeState);
    // The slot record's byte is not the state: it does not count as taken.
    g.guest.Set(record, std::uint8_t{1});
    CHECK(Frame(d, g, 300).size() == 1);  // not taken yet: asked again
    g.guest.Set(kEu + addr::kEquipSlotBits + 0, addr::kEquipBitInHand);
    CHECK(Frame(d, g, 600).size() == 1);
    CHECK(Frame(d, g, 900).empty());  // taken

    // The game makes the sword again (another id): the new one is put away.
    g.guest.Set(sword_id, std::uint32_t{9});
    calls = Frame(d, g, 910);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kEquipChangeState);
    CHECK(calls[0].x[1] == 0u);
    CHECK(calls[0].x[2] == addr::kEquipStateInHand);
}

TEST_CASE("puppet gear: the debug log lists every slot that holds a piece") {
    Game g;
    std::vector<std::string> log;
    Director d([&](const std::string& line) { log.push_back(line); }, false, true);
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    constexpr std::uint64_t kPiece = kParts + 0x2800;
    g.guest.Map(kParts, 0x3000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    for (std::uint32_t s = 0; s < 8; ++s) {
        g.guest.Set(kEu + addr::kEquipSlots + addr::kEquipSlotStride * s + addr::kEquipSlotId,
                    std::uint32_t{0xFFFFFFFF});
    }
    // A piece in slot 2 (not one of the drawable ones), 1 above the puppet.
    const std::uint64_t link = kEu + addr::kEquipSlots + addr::kEquipSlotStride * 2;
    g.guest.Set(link + addr::kEquipSlotId, std::uint32_t{0x21});
    g.guest.Set(link + addr::kEquipSlotHolder, kPiece);  // holder and piece in one place
    g.guest.Set(kPiece + addr::kHolderActor, kPiece + 0x100);
    g.guest.Set(kPiece + 0x100 + addr::kActorState, std::uint32_t{4});
    CoopPuppet::Vec3 own{};
    g.guest.Read(kActor + addr::kActorPosition, &own, sizeof(own));
    g.guest.Set(kPiece + 0x100 + addr::kActorPosition, CoopPuppet::Vec3{own[0], own[1] + 1.0f, own[2]});
    g.guest.Set(kEu + addr::kEquipRecords + addr::kEquipRecordStride * 2 + addr::kEquipRecordFlags,
                std::uint32_t{0x800});
    Frame(d, g, 5000);
    std::string slots;
    for (const auto& line : log) {
        if (line.find("slots (puppet") != std::string::npos) {
            slots = line;
        }
    }
    CHECK(slots.find("slot 2: id 0x21") != std::string::npos);
    CHECK(slots.find(" state 4 up ") != std::string::npos);
    CHECK(slots.find("flags 0x800") != std::string::npos);
    CHECK(slots.find("slot 0:") == std::string::npos);
}

TEST_CASE("puppet gear: nothing is worn while parked, a piece left behind is worn again") {
    Game g;
    std::vector<std::string> log;
    Director d([&](const std::string& line) { log.push_back(line); });
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    constexpr std::uint64_t kHolder = kParts + 0x2800;
    constexpr std::uint64_t kPiece = kParts + 0x2900;
    g.guest.Map(kParts, 0x3000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    for (std::uint32_t s = 0; s < 8; ++s) {
        g.guest.Set(kEu + addr::kEquipSlots + addr::kEquipSlotStride * s + addr::kEquipSlotId,
                    std::uint32_t{0xFFFFFFFF});
    }
    Target t = At(1, 4, 5, 6);
    t.gear[0] = "Weapon_Sword_Kokiri";
    t.drawn = std::uint8_t{0};

    // Parked (our event): no equip call.
    t.show = CoopPuppet::Show::hide;
    d.SetInput(Wants({t}));
    CHECK(Frame(d, g, 20).empty());

    // Shown: worn.
    t.show = CoopPuppet::Show::show;
    d.SetInput(Wants({t}));
    auto calls = Frame(d, g, 30);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kEquip);
    CHECK(Frame(d, g, 40).empty());

    // The game's sword is next to the puppet: nothing more.
    const std::uint64_t link = kEu + addr::kEquipSlots;
    g.guest.Set(link + addr::kEquipSlotId, std::uint32_t{0x30});
    g.guest.Set(link + addr::kEquipSlotHolder, kHolder);
    g.guest.Set(kHolder + addr::kHolderActor, kPiece);
    g.guest.Set(kPiece + addr::kActorState, addr::kStatePresent);
    CoopPuppet::Vec3 own{};
    g.guest.Read(kActor + addr::kActorPosition, &own, sizeof(own));
    g.guest.Set(kPiece + addr::kActorPosition, CoopPuppet::Vec3{own[0], own[1] + 0.9f, own[2]});
    CHECK(Frame(d, g, 50).empty());

    // Far for one reading only (a frame behind a jump): nothing.
    g.guest.Set(kPiece + addr::kActorPosition, CoopPuppet::Vec3{own[0], own[1] - 499.0f, own[2]});
    CHECK(Frame(d, g, 60).empty());
    g.guest.Set(kPiece + addr::kActorPosition, CoopPuppet::Vec3{own[0], own[1] + 0.9f, own[2]});
    CHECK(Frame(d, g, 200).empty());

    // It stays 500 below (made while parked): still far 100 ms later, worn again.
    g.guest.Set(kPiece + addr::kActorPosition, CoopPuppet::Vec3{own[0], own[1] - 499.0f, own[2]});
    CHECK(Frame(d, g, 300).empty());  // first far reading
    calls = Frame(d, g, 400);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kEquip);
    CHECK(Frame(d, g, 450).empty());
    CHECK(Frame(d, g, 1300).empty());  // at most once a second

    // Wearing it again does not bring it up: two more tries, then given up with one line.
    CHECK(Frame(d, g, 1400).empty());
    CHECK(Frame(d, g, 1500).size() == 1);  // try 2
    CHECK(Frame(d, g, 2600).empty());
    CHECK(Frame(d, g, 2700).size() == 1);  // try 3
    CHECK(Frame(d, g, 3800).empty());
    CHECK(Frame(d, g, 3900).empty());       // given up
    CHECK(Frame(d, g, 6000).empty());
    int tries = 0;
    int given_up = 0;
    for (const auto& line : log) {
        tries += line.find("worn again") != std::string::npos ? 1 : 0;
        given_up += line.find("given up") != std::string::npos ? 1 : 0;
    }
    CHECK(tries == 3);
    CHECK(given_up == 1);
}

TEST_CASE("puppet: a create the game refuses is asked again after 1 s") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0)}));
    g.create_result = 0;  // the old scene is still going
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() == 2);  // the block, the create
    CHECK(calls[1].function == kBase + addr::kCreate);
    CHECK(d.Slots()[0].phase == Phase::idle);
    CHECK(Frame(d, g, Director::kRefusedRetryMs - 1).empty());
    g.create_result = 1;
    calls = Frame(d, g, Director::kRefusedRetryMs);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kCreate);
    CHECK(d.Slots()[0].phase == Phase::creating);
}

TEST_CASE("puppet: refused creates slow down, one with a request is waited for") {
    Game g;
    std::vector<std::string> log;
    Director d([&](const std::string& line) { log.push_back(line); });
    d.SetInput(Wants({At(1, 0, 0, 0)}));
    g.create_result = 0;
    REQUIRE(Frame(d, g, 0).size() == 2);  // the block, refused 1
    REQUIRE(Frame(d, g, 1000).size() == 1);  // refused 2
    REQUIRE(Frame(d, g, 2000).size() == 1);  // refused 3: now every 5 s
    CHECK(Frame(d, g, 3000).empty());
    CHECK(Frame(d, g, 6999).empty());
    auto calls = Frame(d, g, 7000);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kCreate);
    int said = 0;
    for (const auto& line : log) {
        said += line.find("refused") != std::string::npos ? 1 : 0;
    }
    CHECK(said == 2);  // the first and the slow-down, not every try

    // Returned 0 but a request is in the handle: it is waited for, no new create.
    Game g2;
    Director d2;
    d2.SetInput(Wants({At(1, 0, 0, 0)}));
    g2.create_result = 0;
    g2.queue_anyway = true;
    calls = Frame(d2, g2, 0);
    REQUIRE(calls.size() == 2);
    CHECK(d2.Slots()[0].phase == Phase::creating);
    CHECK(Frame(d2, g2, 1500).empty());
}

TEST_CASE("puppet gear: every hand slot and the gauntlets have a piece, kind and name cell") {
    using namespace CoopPuppet;
    // The game's slot kinds: weapon 0, shield 6, head 7, armor 8, boots 9,
    // bow 5, gauntlets 10, sub weapon 1, third weapon 2, tool 11, sub tool 12,
    // the Ocarina's holder as Accessory4 (16) and the Ocarina as Accessory1 (13),
    // the player's own slots for them.
    CHECK(kGearKinds ==
          std::array<std::uint32_t, kGearCount>{0, 6, 7, 8, 9, 5, 10, 1, 2, 11, 12, 16, 13});
    CHECK(kGearKinds[kGearOcarina] == 13);
    CHECK(kGearKinds[kGearOcarinaHolder] == 16);
    // Each kind once, each hand slot at most once, none past the drawn bits.
    for (std::size_t i = 0; i < kGearCount; ++i) {
        for (std::size_t j = i + 1; j < kGearCount; ++j) {
            CHECK(kGearKinds[i] != kGearKinds[j]);
            CHECK((kGearSlots[i] < 0 || kGearSlots[i] != kGearSlots[j]));
        }
        CHECK(kGearSlots[i] < 8);
    }
    CHECK(kGearSlots[kGearSubWeapon] == 1);
    CHECK(kGearSlots[kGearThirdWeapon] == 2);
    CHECK(kGearSlots[kGearTool] == 5);
    CHECK(kGearSlots[kGearSubTool] == 6);
    CHECK(kGearSlots[kGearHand] == -1);
    CHECK(kGearSlots[kGearOcarinaHolder] == -1);  // accessories: no hand slot
    CHECK(kGearSlots[kGearOcarina] == -1);
    // Carried pieces are looked at for "left behind" only while in hand.
    CHECK(GearIsCarried(kGearHeld));
    CHECK(GearIsCarried(kGearTool));
    CHECK_FALSE(GearIsCarried(0));
    CHECK_FALSE(GearIsCarried(kGearHand));
    CHECK_FALSE(GearIsCarried(kGearOcarina));  // worn on the belt, like the sword
    // The block holds a name cell and a pointer for each.
    CHECK(block::kGearText + block::kGearCapacity * kGearCount <= block::kGearPtr);
    CHECK(block::kGearPtr + 8 * kGearCount <= block::kSize);
}

TEST_CASE("puppet gear: a PLAYER_EQUIP becomes the pieces, the sword hand's own actor first") {
    using namespace CoopPuppet;
    coop::protocol::Equipment e;
    e.weapon = "Weapon_Sword_Kokiri";
    e.shield = "Weapon_Shield_Deku";
    e.hand = "Armor_Adult_Gauntlet_Silver";
    e.held = "Weapon_Range_Bow";
    e.tool = "Weapon_Boomerang";
    e.sub_tool = "";
    Gear g = GearOf(e);
    CHECK(g[0] == "Weapon_Sword_Kokiri");  // slot 0 not known: the flag's sword
    CHECK(g[1] == "Weapon_Shield_Deku");
    CHECK(g[kGearHeld] == "Weapon_Range_Bow");
    CHECK(g[kGearHand] == "Armor_Adult_Gauntlet_Silver");
    CHECK(g[kGearTool] == "Weapon_Boomerang");
    CHECK(g[kGearSubTool] == "");
    CHECK_FALSE(g[kGearSubWeapon]);  // not sent: not known
    e.weapon_slot = "";  // nothing in the hand slot: still the flag's sword
    CHECK(GearOf(e)[0] == "Weapon_Sword_Kokiri");
    e.weapon_slot = "Weapon_Sword_DekuStick";  // a stick in the sword hand
    CHECK(GearOf(e)[0] == "Weapon_Sword_DekuStick");
    // Only a mask is worn on the head (row 197): a 0.11.8 sender's Lens of
    // Truth flag is taken off, a mask stays, nothing sent stays unknown.
    CHECK_FALSE(GearOf(e)[kGearHead]);
    e.head = "Armor_GlassTruth";
    CHECK(GearOf(e)[kGearHead] == "");
    e.head = "Armor_Child_GlassTruth";
    CHECK(GearOf(e)[kGearHead] == "");
    e.head = "Armor_Adult_Default_Head";
    CHECK(GearOf(e)[kGearHead] == "");
    e.head = "Armor_Mask_Base";  // the masks' base actor is no mask
    CHECK(GearOf(e)[kGearHead] == "");
    e.head = "Armor_Child_Mask_";
    CHECK(GearOf(e)[kGearHead] == "");
    e.head = "Armor_Child_Mask_Truth";
    CHECK(GearOf(e)[kGearHead] == "Armor_Child_Mask_Truth");
    e.head = "";
    CHECK(GearOf(e)[kGearHead] == "");
}

TEST_CASE("puppet gear: the Ocarina and its belt holder come from PLAYER_EQUIP's ocarina (issue 152)") {
    using namespace CoopPuppet;
    coop::protocol::Equipment e;
    CHECK_FALSE(GearOf(e)[kGearOcarina]);  // not sent: not known
    CHECK_FALSE(GearOf(e)[kGearOcarinaHolder]);
    e.ocarina = "";  // no Ocarina: nothing to wear
    CHECK_FALSE(GearOf(e)[kGearOcarina]);
    e.ocarina = "Item_Ocarina_Fairy";
    Gear g = GearOf(e);
    CHECK(g[kGearOcarina] == "Item_Ocarina_Fairy");
    CHECK(g[kGearOcarinaHolder] == "Item_Ocarina_Fairy_Holder");
    e.ocarina = "Item_Ocarina_Time";
    g = GearOf(e);
    CHECK(g[kGearOcarina] == "Item_Ocarina_Time");
    CHECK(g[kGearOcarinaHolder] == "Item_Ocarina_Time_Holder");
    e.ocarina = "Weapon_Sword_Kokiri";  // not an Ocarina: not worn as one
    CHECK_FALSE(GearOf(e)[kGearOcarina]);
}

TEST_CASE("puppet gear: the Ocarina is equipped as Accessory1 and its holder as Accessory4, as the player wears them") {
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x3000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    Target t = At(1, 4, 5, 6);
    t.gear[CoopPuppet::kGearOcarinaHolder] = "Item_Ocarina_Fairy_Holder";
    t.gear[CoopPuppet::kGearOcarina] = "Item_Ocarina_Fairy";
    d.SetInput(Wants({t}));
    const auto calls = Frame(d, g, 30);
    REQUIRE(calls.size() == 2);
    const std::size_t index[] = {CoopPuppet::kGearOcarinaHolder, CoopPuppet::kGearOcarina};
    for (std::size_t k = 0; k < 2; ++k) {
        CHECK(calls[k].function == kBase + addr::kEquip);
        CHECK(calls[k].x[0] == kEu);
        CHECK(calls[k].x[1] == (k == 0 ? 16u : 13u));  // holder Accessory4, Ocarina Accessory1
        CHECK(calls[k].x[2] == kHeap + block::kGearPtr + 8 * index[k]);
        const auto text = g.guest.Get<std::uint64_t>(calls[k].x[2]);
        std::array<char, block::kGearCapacity> name{};
        g.guest.Read(text, name.data(), name.size());
        CHECK(std::string(name.data()) == *t.gear[index[k]]);
    }
    CHECK(Frame(d, g, 40).empty());  // worn: not again
}

TEST_CASE("puppet gear: pieces in hand are put away while the puppet plays the Ocarina") {
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x3000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    g.guest.Set(kEu + addr::kEquipSlots + addr::kEquipSlotStride * 4 + addr::kEquipSlotId,
                std::uint32_t{7});
    auto holds = [&](std::uint64_t at) {
        std::vector<Call> out;
        for (const auto& c : Frame(d, g, at)) {
            if (c.function == kBase + addr::kEquipChangeState) {
                out.push_back(c);
            }
        }
        return out;
    };
    Target t = At(1, 4, 5, 6);
    t.gear[1] = "Weapon_Shield_Hylian";
    t.drawn = std::uint8_t{0x10};  // their shield in hand
    d.SetInput(Wants({t}));
    auto calls = holds(20);  // worn and, its actor there, asked into the hand
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].x[1] == 4u);
    CHECK(calls[0].x[2] == addr::kEquipStateInHand);
    g.guest.Set(kEu + addr::kEquipSlotBits + 4, addr::kEquipBitInHand);
    CHECK(holds(400).size() == 1);  // asked again after the wait, the bit seen: taken
    CHECK(holds(410).empty());

    // They play the Ocarina with the shield bit still set: put away.
    t.anim = CoopPuppet::Anim{"DemoOcarina", 0.0f, 1.0f};
    d.SetInput(Wants({t}));
    g.guest.Set(kEu + addr::kEquipSlotBits + 4, addr::kEquipBitAway);
    calls = holds(420);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].x[1] == 4u);
    CHECK(calls[0].x[2] == addr::kEquipStateAway);
    CHECK(holds(430).empty());

    // The song ends: back in the hand as their bits say.
    t.anim = CoopPuppet::Anim{"Wait", 0.0f, 1.0f};
    d.SetInput(Wants({t}));
    g.guest.Set(kEu + addr::kEquipSlotBits + 4, addr::kEquipBitInHand);
    calls = holds(440);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].x[2] == addr::kEquipStateInHand);
}

TEST_CASE("puppet gear: a tool is equipped with the tool kind and drawn on its own slot") {
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kEu = kParts + 0x800;
    g.guest.Map(kParts, 0x3000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    Target t = At(1, 4, 5, 6);
    t.gear[CoopPuppet::kGearTool] = "Weapon_Boomerang";
    t.drawn = std::uint8_t{0};
    d.SetInput(Wants({t}));
    auto calls = Frame(d, g, 30);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kEquip);
    CHECK(calls[0].x[1] == 11u);  // the tool kind
    CHECK(calls[0].x[2] == kHeap + block::kGearPtr + 8 * CoopPuppet::kGearTool);
    const std::uint64_t text = g.guest.Get<std::uint64_t>(calls[0].x[2]);
    CHECK(text == kHeap + block::kGearText + block::kGearCapacity * CoopPuppet::kGearTool);
    std::array<char, block::kGearCapacity> name{};
    g.guest.Read(text, name.data(), name.size());
    CHECK(std::string(name.data()) == "Weapon_Boomerang");
}

TEST_CASE("fairy join: the fairy waits for the drawing after a load, as her puppet (issue 93)") {
    // The fairies' director is fed by CoopFairy::Follower. Before issue 93 the
    // follower dropped the camera fields, so a fairy was asked for during the
    // loading screen: refused, or made and never drawn.
    Game g;
    Director d;
    CoopFairy::Follower follower;
    auto puppets = Drawing({At(1, 0, 0, 0)}, 0);
    puppets.settle_ms = 1000;
    d.SetInput(follower.Follow(puppets, 0));
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() == 1);  // the block only
    CHECK(calls[0].function == kBase + addr::kMalloc);
    CHECK(Frame(d, g, 3000).empty());
    CHECK(d.Slots()[0].phase == Phase::idle);
    // Drawn again from 3400: created once it has drawn kDrawnMs.
    puppets.drawn_since_ms = 3400;
    d.SetInput(follower.Follow(puppets, 3400));
    CHECK(Frame(d, g, 3400 + Director::kDrawnMs - 1).empty());
    calls = Frame(d, g, 3400 + Director::kDrawnMs);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kCreate);
}

TEST_CASE("puppet planner: the Ocarina played freely (DemoOcarina) shows through their event bit (#86)") {
    CoopPuppet::Planner p;
    std::vector<CoopPuppet::Remote> remotes(1);
    remotes[0].number = 1;
    remotes[0].scene = "Graveyard_Child";
    remotes[0].status = coop::protocol::status::in_cutscene;
    remotes[0].anim = CoopPuppet::Anim{"EvtWait", 0.0f, 1.0f};  // taking it out: any event
    auto in = p.Plan(kBase, std::string("Graveyard_Child"), std::nullopt, false, remotes, 0);
    REQUIRE(in.targets.size() == 1);
    CHECK(in.targets[0].show == CoopPuppet::Show::hold);
    remotes[0].anim = CoopPuppet::Anim{"DemoOcarina", 0.0f, 1.0f};  // playing
    in = p.Plan(kBase, std::string("Graveyard_Child"), std::nullopt, false, remotes, 10);
    REQUIRE(in.targets.size() == 1);
    CHECK(in.targets[0].show == CoopPuppet::Show::show);
    CHECK(std::string(in.targets[0].why) == "ocarina");
}

TEST_CASE("puppet: up to 15 puppets (16 players) all get made, within the frame's call budget") {
    Game g;
    Director d;
    std::vector<Target> targets;
    for (std::uint8_t n = 2; n <= coop::protocol::kMaxPlayers; ++n) {
        targets.push_back(At(n, n, 0, 0));
    }
    d.SetInput(Wants(targets));
    // Each puppet needs two calls (malloc, create): the budget spreads them over frames.
    const std::size_t frames = (2 * targets.size() + Director::kMaxCallsPerFrame - 1) /
                               Director::kMaxCallsPerFrame;
    for (std::size_t f = 0; f < frames; ++f) {
        CHECK(Frame(d, g, 100 + f).size() <= static_cast<std::size_t>(Director::kMaxCallsPerFrame));
    }
    const auto slots = d.Slots();
    REQUIRE(slots.size() == targets.size());
    for (const auto& s : slots) {
        INFO("P" << int(s.number));
        CHECK(s.phase == Phase::creating);
    }
    CHECK(coop::protocol::kMaxPlayers < 32);  // LiveMask: one bit per player number
}

// LICHII_COOP_NATIVE_PLAYER (coop_native_player.h): the game's own Player_<Age> is made as any
// puppet is, then nothing of ours touches it.
TEST_CASE("native player: Player_Child is created, then left alone and only watched") {
    Game g;
    std::vector<std::string> log;
    Director d([&](const std::string& line) { log.push_back(line); });
    d.SetInput(Wants({At(1, 3, 0, 0, "Player_Child")}));
    auto calls = Frame(d, g, 100);
    REQUIRE(calls.size() == 2);
    CHECK(calls[1].function == kBase + addr::kCreate);
    std::array<char, 32> name{};
    g.guest.Read(kHeap + block::kNameText, name.data(), name.size());
    CHECK(std::string(name.data()) == "Player_Child");
    g.Spawn(calls[1].x[3]);
    CHECK(Frame(d, g, 200).size() == 1);  // the take
    CHECK(Frame(d, g, 210).empty());
    REQUIRE(d.Slots()[0].phase == Phase::live);

    // The other player moves, wears things, plays an animation: no call, no write.
    Target moved = At(1, 8, 2, -4, "Player_Child");
    moved.anim = CoopPuppet::Anim{};
    moved.anim->name = "Run";
    moved.anim_serial = 1;
    moved.gear[0] = "Weapon_Sword_001";
    moved.drawn = std::uint8_t{1};
    moved.color = 0xff0000u;
    d.SetInput(Wants({moved}));
    for (std::uint64_t t = 300; t < 3000; t += 33) {
        CHECK(Frame(d, g, t).empty());
    }
    CheckPosition(g.guest, kActor + addr::kActorPosition, 0, 0, 0);
    CheckPosition(g.guest, kActor + addr::kActorPositionRequest, 0, 0, 0);
    CHECK(d.LiveMask() == 0u);  // nothing of the puppets' follows it (fairy, pose copy)
    const auto said = [&](const char* what) {
        return std::any_of(log.begin(), log.end(),
                           [&](const std::string& l) { return l.find(what) != std::string::npos; });
    };
    CHECK(said("native player: second 0x400002000 state 4"));
    CHECK(said("first look"));

    // Gone from the plan: deleted as any puppet.
    d.SetInput(Wants({}));
    bool deleted = false;
    for (std::uint64_t t = 3000; t < 3000 + 2 * Director::kLeaveGraceMs && !deleted; t += 100) {
        for (const Call& c : Frame(d, g, t)) {
            deleted = deleted || c.function == kBase + addr::kRequestDelete;
        }
    }
    CHECK(deleted);
}

TEST_CASE("native player: the planner asks for Player_<Age> only with the switch") {
    CoopPuppet::PlannerSettings s;
    CHECK(CoopPuppet::Planner(s).ActorFor(false, 0xff0000u) != "Player_Child");
    s.native_player = true;
    CHECK(CoopPuppet::Planner(s).ActorFor(false, 0xff0000u) == "Player_Child");
    CHECK(CoopPuppet::Planner(s).ActorFor(true) == "Player_Adult");
    s.actor = "CoopPuppet_Adult";  // LICHII_COOP_PUPPET_ACTOR still wins
    CHECK(CoopPuppet::Planner(s).ActorFor(false) == "CoopPuppet_Adult");
    CHECK(CoopNativePlayer::IsPlayerActor("Player_Adult"));
    CHECK_FALSE(CoopNativePlayer::IsPlayerActor("CoopPuppet_Child"));
    CHECK_FALSE(CoopNativePlayer::IsPlayerActor("Player_Child_Red"));
}

TEST_CASE("native player: the probe says when the second one moves, with the local Link or alone") {
    FakeGuest guest;
    const std::uint64_t second = 0x500000000, local = 0x500010000, parts = 0x500020000;
    guest.Map(second, 0x400);
    guest.Map(local, 0x400);
    guest.Map(parts, 0x400);
    guest.Set(second + 0x2c, std::uint32_t{4});
    guest.Set(second + 0x270, parts);
    guest.Set(parts + 0x358, std::uint64_t{0x77000});
    const CoopNativePlayer::ReadFn read = [&](std::uint64_t a, void* out, std::size_t n) {
        return guest.Read(a, out, n);
    };
    const auto put = [&](std::uint64_t actor, float x) {
        guest.Set(actor + 0x31c, std::array<float, 3>{x, 0, 0});
    };
    CoopNativePlayer::Probe probe;
    auto line = probe.Step(read, kBase, second, local, 0);
    REQUIRE(line);
    CHECK(line->find("player 0x77000") != std::string::npos);
    CHECK(line->find("preload lists -1/-1") != std::string::npos);  // not readable here
    CHECK_FALSE(probe.Step(read, kBase, second, local, 2000));      // nothing changed, not due
    line = probe.Step(read, kBase, second, local, 5000);
    REQUIRE(line);
    CHECK(line->find("no change") != std::string::npos);
    put(second, 1.0f);  // alone
    line = probe.Step(read, kBase, second, local, 6500);
    REQUIRE(line);
    CHECK(line->find("the second one moved") != std::string::npos);
    CHECK(line->find("WITH") == std::string::npos);
    put(second, 2.0f);  // both, the same way: it reads the local pad
    put(local, 1.0f);
    line = probe.Step(read, kBase, second, local, 8000);
    REQUIRE(line);
    CHECK(line->find("moved WITH the local Link") != std::string::npos);
    CHECK(line->find("2.00 m from where it was made") != std::string::npos);
    // The game's own lookup now gives the second one: the worst case, said loudly.
    line = probe.Step(read, kBase, second, second, 9500);
    REQUIRE(line);
    CHECK(line->find("THE GAME'S PLAYER IS NOW THE SECOND ONE") != std::string::npos);
}

TEST_CASE("puppet link sound: a sound call the cave did not make does not silence the puppet") {
    Game g;
    Director d;
    MakeLive(d, g);
    constexpr std::uint64_t kParts = 0x500000000;
    constexpr std::uint64_t kXLink = kParts + 0x400;
    constexpr std::uint64_t kUser = kParts + 0x800;
    constexpr std::uint64_t kResource = kParts + 0xa00;
    g.guest.Map(kParts, 0x1000);
    g.guest.Set(kActor + addr::kActorParts, kParts);
    g.guest.Set(kParts + CoopPuppetSound::addr::kPartsXLink, kXLink);
    g.guest.Set(kXLink + CoopPuppetSound::addr::kXLinkSoundUser, kUser);
    g.guest.Set(kUser + CoopLinkSound::addr::kUserResource, kResource);
    g.guest.Set(kResource + CoopLinkSound::addr::kResourceAccessor, kParts + 0xc00);
    Target t = At(1, 4, 5, 6);
    t.sounds.push_back({"Pl_Voice_Attack_00", 100});
    d.SetInput(Wants({t}));
    auto first = d.Step(g.guest, false, 0, 100);
    REQUIRE(first);
    CHECK(first->function == kBase + CoopLinkSound::addr::kSearchAsset);
    // The cave never made it (no result): the next frame undoes it, and the
    // next sound is still looked up (the first is lost).
    t.sounds = {{"Pl_Voice_Damage_00", 120}};
    d.SetInput(Wants({t}));
    auto again = d.Step(g.guest, false, 0, 120);
    REQUIRE(again);
    CHECK(again->function == kBase + CoopLinkSound::addr::kSearchAsset);
    CHECK(d.Slots()[0].phase == Phase::live);
}

namespace {
constexpr std::uint64_t kRequest2 = 0x500000000;
constexpr std::uint64_t kActor2 = 0x500002000;

// A second request and actor, for the create after a retry.
void SpawnSecond(Game& g, std::uint64_t handle, std::uint32_t state = 4) {
    g.guest.Map(kRequest2, 0x30);
    g.guest.Map(kActor2, 0x400);
    g.guest.Set(kRequest2, std::uint32_t{0x10000 | addr::kRequestReady});
    g.guest.Set(kRequest2 + addr::kRequestActor, kActor2);
    g.guest.Set(kActor2, kVtable);
    g.guest.Set(kActor2 + addr::kActorState, state);
    g.guest.Set(handle, kRequest2);
}

int Deletes(const std::vector<Call>& calls, std::uint64_t actor) {
    int n = 0;
    for (const auto& c : calls) {
        n += c.function == kBase + addr::kRequestDelete && c.x[0] == actor;
    }
    return n;
}

const Call* Find(const std::vector<Call>& calls, std::uint64_t function) {
    for (const auto& c : calls) {
        if (c.function == kBase + function) {
            return &c;
        }
    }
    return nullptr;
}
}  // namespace

TEST_CASE("puppet stray: an actor that spawns late after a retry is deleted, one puppet stays") {
    Game g;
    std::vector<std::string> log;
    Director d([&](const std::string& line) { log.push_back(line); });
    d.SetInput(Wants({At(2, 0, 0, 0)}));
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() == 2);
    // Taken, but still loading (state 3), as in the owner's playtest.
    g.Spawn(calls[1].x[3], 3);
    calls = Frame(d, g, 10);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kTakeCreated);

    // The create times out: its actor is asked to go with the game's delete.
    const std::uint64_t timeout = Director::kCreateTimeoutMs + 20;
    calls = Frame(d, g, timeout);
    CHECK(Deletes(calls, kActor) == 1);
    CHECK(d.Slots()[0].phase == Phase::idle);
    CHECK(d.Slots()[0].strays == 1);

    // The retry: a new actor, live.
    calls = Frame(d, g, timeout + 5100);
    const Call* create = Find(calls, addr::kCreate);
    REQUIRE(create != nullptr);
    CHECK(Deletes(calls, kActor) == 1);  // still there: asked again (once a second)
    SpawnSecond(g, create->x[3]);
    Frame(d, g, timeout + 5200);
    Frame(d, g, timeout + 5210);
    REQUIRE(d.Slots()[0].phase == Phase::live);
    CHECK(d.Slots()[0].actor == kActor2);

    // The old one finishes loading late: still deleted, never a second puppet.
    g.guest.Set(kActor + addr::kActorState, std::uint32_t{4});
    calls = Frame(d, g, timeout + 6200);
    CHECK(Deletes(calls, kActor) == 1);
    CHECK(Deletes(calls, kActor2) == 0);
    // The game deletes it: no more calls for it, the puppet stays.
    g.guest.Set(kActor + addr::kActorState, std::uint32_t{8});
    calls = Frame(d, g, timeout + 7300);
    CHECK(Deletes(calls, kActor) == 0);
    CHECK(d.Slots()[0].strays == 0);
    CHECK(d.Slots()[0].phase == Phase::live);
    CHECK(d.Slots()[0].actor == kActor2);
    CHECK(Deletes(Frame(d, g, timeout + 9000), kActor) == 0);

    bool said = false;
    for (const auto& line : log) {
        said = said || line.find("is a stray (its create timed out)") != std::string::npos;
    }
    CHECK(said);
}

TEST_CASE("puppet stray: a rejoin whose old puppet is not deleted in time keeps one puppet") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0)}));
    auto calls = Frame(d, g, 0);
    g.Spawn(calls[1].x[3]);
    Frame(d, g, 10);
    Frame(d, g, 15);
    REQUIRE(d.Slots()[0].phase == Phase::live);

    // The player leaves; the game leaves the actor standing past the 5 s wait.
    d.SetInput(Wants({}));
    Frame(d, g, 100);
    calls = Frame(d, g, 100 + Director::kLeaveGraceMs);
    CHECK(Deletes(calls, kActor) == 1);
    Frame(d, g, 8000);
    CHECK(d.Slots()[0].phase == Phase::idle);
    CHECK(d.Slots()[0].strays == 1);

    // They join again: the old one is asked to go first, then the new create.
    d.SetInput(Wants({At(1, 0, 0, 0)}));
    calls = Frame(d, g, 9000);
    REQUIRE(calls.size() == 2);
    CHECK(Deletes(calls, kActor) == 1);
    CHECK(calls[1].function == kBase + addr::kCreate);
    SpawnSecond(g, calls[1].x[3]);
    Frame(d, g, 9100);
    Frame(d, g, 9110);
    REQUIRE(d.Slots()[0].phase == Phase::live);
    CHECK(d.Slots()[0].actor == kActor2);
    g.guest.Set(kActor + addr::kActorState, std::uint32_t{9});
    Frame(d, g, 10100);
    CHECK(d.Slots()[0].strays == 0);
    CHECK(d.Slots()[0].actor == kActor2);
}

TEST_CASE("puppet stray: an address the engine reused for another actor is left alone") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0)}));
    auto calls = Frame(d, g, 0);
    g.Spawn(calls[1].x[3], 3);
    Frame(d, g, 10);
    CHECK(Deletes(Frame(d, g, Director::kCreateTimeoutMs + 20), kActor) == 1);
    REQUIRE(d.Slots()[0].strays == 1);
    g.guest.Set(kActor, kVtable + 0x100);  // freed and made into something else
    CHECK(Deletes(Frame(d, g, Director::kCreateTimeoutMs + 2000), kActor) == 0);
    CHECK(d.Slots()[0].strays == 0);
}

TEST_CASE("puppet stray: an actor of another name at the address is left alone") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0)}));
    auto calls = Frame(d, g, 0);
    g.Spawn(calls[1].x[3], 3);
    Frame(d, g, 10);
    Frame(d, g, Director::kCreateTimeoutMs + 20);
    REQUIRE(d.Slots()[0].strays == 1);
    // Same class, another actor (an NPC on the same vtable): its name differs.
    constexpr std::uint64_t kText = 0x400003000;
    g.guest.Map(kText, 0x40);
    const char other[] = "Npc_Kokiri";
    REQUIRE(g.guest.Write(kText, other, sizeof(other)));
    g.guest.Set(kActor + 0x260, kText);
    CHECK(Deletes(Frame(d, g, Director::kCreateTimeoutMs + 2000), kActor) == 0);
    CHECK(d.Slots()[0].strays == 0);
}

TEST_CASE("puppet stray: an actor freed or being deleted before the timeout gets no delete call") {
    for (const bool reused : {true, false}) {
        CAPTURE(reused);
        Game g;
        Director d;
        d.SetInput(Wants({At(1, 0, 0, 0)}));
        auto calls = Frame(d, g, 0);
        g.Spawn(calls[1].x[3], 3);
        Frame(d, g, 10);
        if (reused) {
            g.guest.Set(kActor, kVtable + 0x100);  // freed, the address made into another class
        } else {
            g.guest.Set(kActor + addr::kActorState, std::uint32_t{8});  // the game deletes it
        }
        calls = Frame(d, g, Director::kCreateTimeoutMs + 20);
        CHECK(Deletes(calls, kActor) == 0);
        CHECK(d.Slots()[0].strays == 0);
        CHECK(d.Slots()[0].phase == Phase::idle);
    }
}

TEST_CASE("puppet stray: a scene change lets strays go (the load deletes them)") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0)}, "A"));
    auto calls = Frame(d, g, 0);
    g.Spawn(calls[1].x[3], 3);
    Frame(d, g, 10);
    CHECK(Deletes(Frame(d, g, Director::kCreateTimeoutMs + 20), kActor) == 1);
    REQUIRE(d.Slots()[0].strays == 1);
    d.SetInput(Wants({}, ""));  // loading
    CHECK(Deletes(Frame(d, g, Director::kCreateTimeoutMs + 2000), kActor) == 0);
    CHECK(d.Slots()[0].strays == 0);
}

TEST_CASE("puppet stray: a stray the game keeps is let go after the most delete calls") {
    Game g;
    std::vector<std::string> log;
    Director d([&](const std::string& line) { log.push_back(line); });
    // A live puppet whose player leaves; the game never deletes it.
    d.SetInput(Wants({At(1, 0, 0, 0)}));
    auto calls = Frame(d, g, 0);
    g.Spawn(calls[1].x[3]);
    Frame(d, g, 10);
    Frame(d, g, 15);
    REQUIRE(d.Slots()[0].phase == Phase::live);
    d.SetInput(Wants({}));
    Frame(d, g, 100);
    Frame(d, g, 100 + Director::kLeaveGraceMs);
    int deletes = 0;
    for (std::uint64_t t = 8000; t < 8000 + 40 * Director::kStrayDeleteMs; t += 250) {
        deletes += Deletes(Frame(d, g, t), kActor);
    }
    CHECK(deletes == Director::kStrayMaxDeletes);
    CHECK(d.Slots()[0].strays == 0);
    bool said = false;
    for (const auto& line : log) {
        said = said || line.find("given up") != std::string::npos;
    }
    CHECK(said);
}

// sword-3: the local Link's sword and shield as the game's own query has them,
// for the sender (coop_puppet_own.h).
TEST_CASE("own drawn: a fresh answer of the game decides the slot, a stale one does not") {
    CoopPuppetOwn::Drawn d;
    // Nothing answered yet: the reader's bits stand.
    CHECK(d.Apply(0x09, 1000) == 0x09);
    d.Set(0, addr::kEquipStateInHand, 1000);  // the sword in hand
    d.Set(1, addr::kEquipStateAway, 1000);    // the shield on the back
    // The request byte was missed (sword bit clear) or stale (shield bit set): the game's answer wins;
    // the held item's bit (3) is left as read.
    CHECK(d.Apply(0x18, 1100) == 0x09);
    CHECK(d.Apply(0x00, 1499) == 0x01);
    // Older than kFreshMs: the reader's bits again.
    CHECK(d.Apply(0x10, 1000 + CoopPuppetOwn::kFreshMs) == 0x10);
    // Put away, and no piece (0): put away clears the bit, unknown leaves it.
    d.Set(0, addr::kEquipStateAway, 2000);
    CHECK(d.Apply(0x01, 2000) == 0x00);
    d.Set(0, 0, 2100);
    CHECK(d.Apply(0x01, 2100) == 0x01);
    d.Set(7, addr::kEquipStateInHand, 2100);  // no such index: ignored
    CHECK(d.Apply(0x00, 2100) == 0x00);
}

TEST_CASE("own drawn: the director asks the game's state of the local Link's sword and shield, one a frame") {
    constexpr std::uint64_t kScene = 0x500000000;
    constexpr std::uint64_t kPlayer = 0x500002000;
    constexpr std::uint64_t kParts = 0x500003000;
    constexpr std::uint64_t kEu = 0x500004000;
    Game g;
    // CoopPose::FindPlayer's chain: scene -> table -> array -> manager -> link -> holder -> actor.
    g.guest.Map(kBase + 0x4e97648, 8);
    g.guest.Map(kScene, 0x1000);
    g.guest.Map(kPlayer, 0x400);
    g.guest.Map(kParts, 0x400);
    g.guest.Map(kEu, 0x100);
    g.guest.Set(kBase + 0x4e97648, kScene);
    g.guest.Set(kScene + 0x1f0, kScene + 0x100);              // table
    g.guest.Set(kScene + 0x100 + 0x50, std::uint32_t{0x20});  // count
    g.guest.Set(kScene + 0x100 + 0x58, kScene + 0x200);       // array
    g.guest.Set(kScene + 0x200 + 0x78, kScene + 0x300);       // manager
    g.guest.Set(kScene + 0x300 + 0x30, std::uint64_t{1});     // must
    g.guest.Set(kScene + 0x300 + 0x40, kScene + 0x400);       // link
    g.guest.Set(kScene + 0x400 + 0x10, std::uint32_t{7});     // id
    g.guest.Set(kScene + 0x400 + 0x8, kScene + 0x500);        // holder
    g.guest.Set(kScene + 0x500 + 0x48, std::uint32_t{7});
    g.guest.Set(kScene + 0x500 + 0x40, kPlayer);
    g.guest.Set(kPlayer + 0x2c, std::uint32_t{4});
    g.guest.Set(kPlayer + addr::kActorParts, kParts);
    g.guest.Set(kParts + addr::kPartsEquipmentUser, kEu);
    g.slot_state[0] = addr::kEquipStateInHand;
    g.slot_state[4] = addr::kEquipStateAway;
    auto& own = CoopPuppetOwn::TheDrawn();
    own.Clear();

    Director d;
    d.SetInput(Wants({}));
    Frame(d, g, 1000);
    REQUIRE(g.state_checks.size() == 1);
    CHECK(g.state_checks[0].x[0] == kEu);
    CHECK(g.state_checks[0].x[1] == 0);
    Frame(d, g, 1016);
    REQUIRE(g.state_checks.size() == 2);
    CHECK(g.state_checks[1].x[1] == 4);
    // The sender's bits: sword in hand, shield away, whatever the request bytes showed.
    CHECK(own.Apply(0x10, CoopPuppetOwn::NowMs()) == 0x01);
    // One a frame: the next frame asks the sword again.
    Frame(d, g, 1033);
    REQUIRE(g.state_checks.size() == 3);
    CHECK(g.state_checks[2].x[1] == 0);
    own.Clear();
}

TEST_CASE("puppet: a player the PvP mask names gets the _Pvp copy, the others the plain one") {
    CoopPuppet::PlannerSettings s;
    CoopPuppet::Planner p(s);
    std::vector<CoopPuppet::Remote> remotes(2);
    remotes[0].number = 2;
    remotes[0].scene = "Kokiri";
    remotes[1].number = 3;
    remotes[1].scene = "Kokiri";
    // Off (the default): every copy plain.
    auto in = p.Plan(kBase, std::string("Kokiri"), CoopPuppet::Vec3{0, 0, 0}, false, remotes, 1000);
    REQUIRE(in.targets.size() == 2);
    const std::string two = in.targets[0].actor, three = in.targets[1].actor;
    CHECK(two == "CoopPuppet_Child_Red");
    CHECK(three.find("_Pvp") == std::string::npos);
    CoopPuppet::SetPvpCopies(1u << 3);
    in = p.Plan(kBase, std::string("Kokiri"), CoopPuppet::Vec3{0, 0, 0}, false, remotes, 2000);
    CoopPuppet::SetPvpCopies(0);
    REQUIRE(in.targets.size() == 2);
    CHECK(in.targets[0].actor == two);
    CHECK(in.targets[1].actor == three + "_Pvp");
}

TEST_CASE("puppet: a PvP mode change re-makes the copy, but not while our Link holds it or it holds something") {
    Game g;
    Director d;
    d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Child")}));
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() == 2);
    g.Spawn(calls[1].x[3]);
    Frame(d, g, 10);
    Frame(d, g, 15);
    REQUIRE(d.Slots()[0].phase == Phase::live);

    // Lifted by our Link (phase 3): no delete while it lasts.
    CoopPuppet::WatchGamePlaces(true);
    CoopPuppet::SetGameHolds(1);
    d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Child_Pvp")}));
    for (const auto& c : Frame(d, g, 20)) CHECK(c.function != kBase + addr::kRequestDelete);
    CHECK(d.Slots()[0].actor_name == "CoopPuppet_Child");
    CoopPuppet::SetGameHolds(0);
    CoopPuppet::WatchGamePlaces(false);

    // Its player holds something (its animation): kept too.
    Target held = At(1, 0, 0, 0, "CoopPuppet_Child_Pvp");
    held.anim = CoopPuppet::Anim{"Carry_Wait", 0.0f, 1.0f};
    d.SetInput(Wants({held}));
    for (const auto& c : Frame(d, g, 30)) CHECK(c.function != kBase + addr::kRequestDelete);

    // Its player is carried by another (lift-young: the carried pose on every copy): kept too.
    Target carried = At(1, 0, 0, 0, "CoopPuppet_Child_Pvp");
    carried.anim = CoopPuppet::Anim{std::string(CoopLiftPlayer::kCarriedPose), 0.0f, 1.0f};
    d.SetInput(Wants({carried}));
    for (const auto& c : Frame(d, g, 35)) CHECK(c.function != kBase + addr::kRequestDelete);
    CHECK(d.Slots()[0].actor_name == "CoopPuppet_Child");

    // Free again: the _Pvp copy at once.
    d.SetInput(Wants({At(1, 0, 0, 0, "CoopPuppet_Child_Pvp")}));
    calls = Frame(d, g, 40);
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].function == kBase + addr::kRequestDelete);
}

TEST_CASE("puppet: a playing pose sent at a still frame plays on, not set back each sample (carried pose)") {
    using CoopPuppet::Anim;
    using CoopPuppet::FrameFollowed;
    // The first sample of a command, or a new command: followed (set once).
    CHECK(FrameFollowed("", 0.0f, Anim{"Eaten", 0.0f, 1.0f}));
    CHECK(FrameFollowed("Wait", 0.0f, Anim{"Eaten", 0.0f, 1.0f}));
    // The same command at the same frame while playing (the lift's carried pose: frame 0, rate 1): not followed,
    // so the copy loops on instead of snapping back to frame 0 on every sample.
    CHECK_FALSE(FrameFollowed("Eaten", 0.0f, Anim{"Eaten", 0.0f, 1.0f}));
    // A real player's animation moves on between samples: followed as before.
    CHECK(FrameFollowed("Run", 10.0f, Anim{"Run", 11.5f, 1.0f}));
    // A held pose (rate 0) at a still frame: followed (the copy is kept on that frame).
    CHECK(FrameFollowed("Pose", 4.0f, Anim{"Pose", 4.0f, 0.0f}));
}

// Character Select's colour routine for a character's puppet (Director::Dress; review of #299 at 07de2fe2).
namespace {
constexpr std::uint64_t kDressAt = 0x2E1A000;   // main's offset, as the pack's manifest names it
constexpr std::uint64_t kAgainMs = 3000;      // Director::kDressAgainMs (private)
constexpr std::array<std::uint8_t, 16> kDressHead = {0xfd, 0x7b, 0xbf, 0xa9, 0xe0, 0x03, 0x00, 0x34,
                                                     0xab, 0x2c, 0x40, 0xb9, 0x6b, 0x1d, 0x00, 0x51};
std::size_t Dresses(const std::vector<Call>& calls, std::uint64_t* word = nullptr) {
    std::size_t n = 0;
    for (const auto& c : calls) {
        if (c.function == kBase + kDressAt) {
            ++n;
            if (word != nullptr) {
                *word = c.x[1];
            }
        }
    }
    return n;
}
Target Dressed(std::string actor, std::uint64_t custom) {
    Target t = At(1, 4, 5, 6, std::move(actor));
    t.custom = custom;
    return t;
}
// The calls of the frames that bring the puppet live (a Frame runs every Step until none is left, so the first
// colour routine call is among them, at the frame the puppet goes live).
std::vector<Call> LiveAs(Director& d, Game& g, const Target& t) {
    d.SetInput(Wants({t}));
    auto calls = Frame(d, g, 0);
    REQUIRE(calls.size() >= 2);
    g.Spawn(calls[1].x[3]);
    calls = Frame(d, g, 10);
    const auto more = Frame(d, g, 15);
    calls.insert(calls.end(), more.begin(), more.end());
    REQUIRE(d.Slots()[0].phase == Phase::live);
    return calls;
}
}  // namespace

TEST_CASE("puppet characters: the mod's colour routine is called with the options, once per change and again later") {
    Game g;
    g.guest.Map(kBase + kDressAt, 16);
    REQUIRE(g.guest.Write(kBase + kDressAt, kDressHead.data(), kDressHead.size()));
    std::vector<std::string> log;
    Director d([&](const std::string& line) { log.push_back(line); });
    d.SetCharacterApply(kDressAt, kDressHead);
    const std::uint64_t word = (std::uint64_t{9} << 32) | 0x123456u;
    auto calls = LiveAs(d, g, Dressed("CoopPuppet_Adult_C005", word));
    std::uint64_t given = 0;
    REQUIRE(Dresses(calls, &given) == 1);                                    // as soon as it is live
    CHECK(given == word);
    for (const auto& c : calls) {
        if (c.function == kBase + kDressAt) {
            CHECK(c.x[0] == kActor);   // the puppet's actor
        }
    }
    CHECK(Dresses(Frame(d, g, 20)) == 0);                                    // once
    CHECK(Dresses(Frame(d, g, 500)) == 0);
    CHECK(Dresses(Frame(d, g, 20 + kAgainMs + 1)) == 1);      // and again later (a late model)
    const std::uint64_t other = (std::uint64_t{9} << 32) | 0x000001u;
    d.SetInput(Wants({Dressed("CoopPuppet_Adult_C005", other)}));
    calls = Frame(d, g, 20 + kAgainMs + 50);
    REQUIRE(Dresses(calls, &given) == 1);                                    // a change: at once
    CHECK(given == other);
    // Options cleared: the model's own once (the routine with none), then left alone.
    const std::uint64_t none = std::uint64_t{9} << 32;
    d.SetInput(Wants({Dressed("CoopPuppet_Adult_C005", none)}));
    CHECK(Dresses(Frame(d, g, 20 + kAgainMs + 100)) == 1);
    CHECK(Dresses(Frame(d, g, 20 + 3 * kAgainMs)) == 0);
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) {
        return l.find("colour routine is at main+") != std::string::npos;
    }));
}

TEST_CASE("puppet characters: no routine call for another build of the mod, a non-character puppet or no options") {
    // Bytes that are not the routine's (another build of the mod, or no mod code in the game): never called.
    {
        Game g;
        g.guest.Map(kBase + kDressAt, 16);
        std::vector<std::string> log;
        Director d([&](const std::string& line) { log.push_back(line); });
        d.SetCharacterApply(kDressAt, kDressHead);
        LiveAs(d, g, Dressed("CoopPuppet_Adult_C005", (std::uint64_t{9} << 32) | 0x12u));
        CHECK(Dresses(Frame(d, g, 20)) == 0);
        CHECK(Dresses(Frame(d, g, 20 + 2 * kAgainMs)) == 0);
        CHECK(std::count_if(log.begin(), log.end(), [](const std::string& l) {
                  return l.find("colour routine is not at main+") != std::string::npos;
              }) == 1);   // said once
    }
    // A colour puppet, or a pack without the routine, or a character with nothing set: no call.
    const std::vector<std::tuple<std::string, std::uint64_t, std::uint64_t>> cases = {
        {"CoopPuppet_Adult_Red", kDressAt, 0x12u},
        {"CoopPuppet_Adult_C005", 0, (std::uint64_t{9} << 32) | 0x12u},
        {"CoopPuppet_Adult_C005", kDressAt, std::uint64_t{9} << 32}};
    for (const auto& [actor, offset, custom] : cases) {
        Game g;
        g.guest.Map(kBase + kDressAt, 16);
        REQUIRE(g.guest.Write(kBase + kDressAt, kDressHead.data(), kDressHead.size()));
        Director d;
        d.SetCharacterApply(offset, kDressHead);
        LiveAs(d, g, Dressed(actor, custom));
        CHECK(Dresses(Frame(d, g, 20)) == 0);
        CHECK(Dresses(Frame(d, g, 20 + 2 * kAgainMs)) == 0);
    }
}

TEST_CASE("puppet (row 177): only a plain puppet of a pack with perception is seen by enemies") {
    CHECK(CoopPuppet::SeenPuppetActor("CoopPuppet_Child", true));
    CHECK(CoopPuppet::SeenPuppetActor("CoopPuppet_Adult_Red", true));
    CHECK_FALSE(CoopPuppet::SeenPuppetActor("CoopPuppet_ChildQuiet", true));
    CHECK_FALSE(CoopPuppet::SeenPuppetActor("CoopPuppet_AdultQuiet_Red", true));
    CHECK_FALSE(CoopPuppet::SeenPuppetActor("CoopPuppet_Child", false));
    CHECK_FALSE(CoopPuppet::SeenPuppetActor("Player_Child", true));
}

// Review of charsel-colours-0118 (S1): a routine whose bytes match but that the runtime cannot call (not a game
// function the build knows) hands the puppets to co-op's own dressing after kDressNotMade calls not made.
namespace {
// One frame whose colour routine call is not made: the frame ends at it (as Hookshot's RunFrame on a failed call).
bool FrameEndingAtDress(Director& d, Game& g, std::uint64_t now_ms) {
    auto call = d.Step(g.guest, false, 0, now_ms);
    while (call) {
        if (call->function == kBase + kDressAt) {
            return true;
        }
        std::uint64_t result = 0;
        if (call->function == kBase + addr::kEquip) result = g.equip_result;
        if (call->function == kBase + addr::kTakeCreated) REQUIRE(g.guest.Read(call->x[0] + addr::kRequestActor, &result, 8));
        call = d.Step(g.guest, true, result, now_ms);
    }
    return false;
}
}  // namespace

TEST_CASE("puppet characters: a colour routine that cannot be called gives the puppet to co-op's own dressing") {
    Game g;
    g.guest.Map(kBase + kDressAt, 16);
    REQUIRE(g.guest.Write(kBase + kDressAt, kDressHead.data(), kDressHead.size()));
    std::vector<std::string> log;
    Director d([&](const std::string& line) { log.push_back(line); });
    d.SetCharacterApply(kDressAt, kDressHead);
    const std::uint64_t word = (std::uint64_t{9} << 32) | 0x12u;
    REQUIRE(Dresses(LiveAs(d, g, Dressed("CoopPuppet_Adult_C005", (std::uint64_t{9} << 32) | 0x1u))) == 1);
    d.SetInput(Wants({Dressed("CoopPuppet_Adult_C005", word)}));   // a change: its call is not made
    const std::uint64_t now = 100;
    REQUIRE(FrameEndingAtDress(d, g, now - 5));
    REQUIRE(d.DressTargets().size() == 1);
    CHECK(d.DressTargets()[0].mods_routine);    // its bytes match: the mod's routine's
    CHECK(d.DressTargets()[0].character);
    CHECK(d.DressTargets()[0].custom == 0x12u);
    REQUIRE(FrameEndingAtDress(d, g, now + 5));   // the first not made: asked again
    FrameEndingAtDress(d, g, now + 10);           // the second not made: ours from now on
    REQUIRE(d.DressTargets().size() == 1);
    CHECK_FALSE(d.DressTargets()[0].mods_routine);
    CHECK(std::count_if(log.begin(), log.end(), [](const std::string& l) {
              return l.find("could not be called") != std::string::npos;
          }) == 1);
    CHECK(Dresses(Frame(d, g, now + 20 + kAgainMs)) == 0);   // never asked of it again
}
