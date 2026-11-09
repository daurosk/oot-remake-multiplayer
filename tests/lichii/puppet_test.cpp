// Tests for the puppet director and planner (lichii/hook/coop_puppet.cpp)
// and the cave's encoding (lichii/hook/coop_svc.h). No emulator code: guest
// memory is a set of fake pages.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop/host/player_reader.hpp"
#include "coop_puppet.h"
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
            result = 1;
        } else if (call->function == kBase + addr::kRequestDelete) {
            result = 1;
        } else if (call->function == kBase + addr::kTakeCreated) {
            REQUIRE(g.guest.Read(call->x[0] + addr::kRequestActor, &result, 8));
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
    Target moved = At(2, 12, 1.5f, -6);
    moved.rotation = CoopPuppet::QuatToMatrix({0, std::sqrt(0.5f), 0, std::sqrt(0.5f)});
    d.SetInput(Wants({moved}));
    CHECK(Frame(d, g, 400).empty());
    CheckPosition(g.guest, kActor + addr::kActorPosition, 12, 1.5f, -6);
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
    CHECK(in.targets[0].actor == "CoopPuppet_Child");  // by age
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

TEST_CASE("puppet animation: the other slots synced from slot 0; pose request written") {
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

TEST_CASE("puppet scene filter: hold freezes it, hide parks it under the floor") {
    Game g;
    Director d;
    MakeLive(d, g);
    GiveAnimation(g, 9.0f);
    d.SetInput(Wants({Playing("Wait", 9.0f, 1)}));
    Frame(d, g, 20);
    Frame(d, g, 30);
    CheckPosition(g.guest, kActor + addr::kActorPosition, 4, 5, 6);

    // Hold: not moved, and its frame set back to where it was every frame.
    Target held = Playing("Walk", 2.0f, 2, CoopPuppet::Show::hold);
    held.position = {40, 5, 6};
    d.SetInput(Wants({held}));
    g.guest.Set(kSlot0 + addr::kSlotFrame, 11.0f);
    for (std::uint64_t now : {40, 50}) {
        auto calls = Frame(d, g, now);
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].function == kBase + addr::kSetAsFrame);
        CHECK(calls[0].s0 == 11.0f);
        g.guest.Set(kSlot0 + addr::kSlotFrame, 12.0f);
    }
    CheckPosition(g.guest, kActor + addr::kActorPosition, 4, 5, 6);
    CHECK(d.Slots()[0].anim_name == "Wait");

    // Hide: parked below its spot, no animation calls, not deleted.
    Target hidden = Playing("Walk", 2.0f, 2, CoopPuppet::Show::hide);
    hidden.position = {40, 5, 6};
    d.SetInput(Wants({hidden}));
    CHECK(Frame(d, g, 60).empty());
    CheckPosition(g.guest, kActor + addr::kActorPosition, 40, 5 - Director::kHideDrop, 6);
    CHECK(d.Slots()[0].phase == Phase::live);

    // Shown again: moves, and plays what it missed.
    held.show = CoopPuppet::Show::show;
    d.SetInput(Wants({held}));
    auto calls = Frame(d, g, 70);
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].function == kBase + addr::kPlayAs);
    CheckPosition(g.guest, kActor + addr::kActorPosition, 40, 5, 6);
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
    CHECK(in.targets[0].actor == "CoopPuppet_Child");
    REQUIRE(in.targets[0].anim);
    CHECK(in.targets[0].anim->name == "Wait");
    CHECK(in.targets[0].anim_serial == 7u);
    CHECK(in.targets[1].show == CoopPuppet::Show::hold);  // their cutscene
    CHECK(in.targets[1].actor == "CoopPuppet_Adult");

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
