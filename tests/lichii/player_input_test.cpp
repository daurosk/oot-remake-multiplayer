// SPDX-License-Identifier: GPL-3.0-or-later
// The other player's Link reads its own input (lichii/hook/coop_player_input.h): the wraps of the
// game's pad reads, the frames between two games that run at different rates, and the bytes sent.
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "coop_player_input.h"

namespace {

using CoopPlayerInput::Frame;
using CoopPlayerInput::Queue;
using CoopPlayerInput::Wraps;
namespace addr = CoopPlayerInput::addr;

constexpr std::uint64_t kBase = 0x80020000;

struct Memory {
    std::map<std::uint64_t, std::uint8_t> bytes;
    CoopPlayerInput::Guest Guest() {
        return {[this](std::uint64_t a, void* out, std::size_t n) {
                    for (std::size_t i = 0; i < n; ++i) {
                        const auto it = bytes.find(a + i);
                        if (it == bytes.end()) return false;
                        static_cast<std::uint8_t*>(out)[i] = it->second;
                    }
                    return true;
                },
                [this](std::uint64_t a, const void* in, std::size_t n) {
                    for (std::size_t i = 0; i < n; ++i) bytes[a + i] = static_cast<const std::uint8_t*>(in)[i];
                    return true;
                }};
    }
    template <typename T>
    void Set(std::uint64_t a, const T& v) {
        Guest().write(a, &v, sizeof(T));
    }
    template <typename T>
    T Get(std::uint64_t a) {
        T v{};
        REQUIRE(Guest().read(a, &v, sizeof(T)));
        return v;
    }
    // An AI node whose actor is `actor`.
    void Node(std::uint64_t node, std::uint64_t actor) {
        Set(node + addr::kNodeOwner, node + 0x100);
        Set(node + 0x100 + addr::kOwnerHolder, node + 0x200);
        Set(node + 0x200 + addr::kHolderActor, actor);
    }
    void Zero(std::uint64_t a, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) bytes[a + i] = 0;
    }
};

constexpr std::uint64_t kLocalNode = 0x1000, kRemoteNode = 0x2000, kLocal = 0xa000, kRemote = 0xb000, kOut = 0x9000;

// One call of a wrapped read as Hookshot's wrapper makes it: phase 0, the game's function unless
// taken, phase 1. `game` is what the game's own function would do; it returns the x0 it leaves.
template <typename Game>
int Call(Wraps& w, Memory& m, std::uint32_t id, std::uint64_t node, std::uint64_t x30, Game game) {
    std::uint64_t x[32] = {};
    x[0] = kOut;
    x[19] = node;
    x[30] = x30;
    const auto guest = m.Guest();
    const int taken = w.Wrap(id, 0, x, guest);
    if (!taken) {
        x[0] = game();
    }
    CHECK(w.Wrap(id, 1, x, guest) == 0);
    return taken;
}

std::uint64_t On() {
    return 0;  // input is not off
}
std::uint64_t Off() {
    return 1;
}

}  // namespace

TEST_CASE("player input: the local Link's reads run as the game's and are kept, one frame a buttons read") {
    Memory m;
    Wraps w;
    w.SetMainBase(kBase);
    m.Node(kLocalNode, kLocal);
    m.Zero(kOut, 0x20);
    const auto sticks = [&] {
        m.Set(kOut + 0, std::array<float, 4>{0.5f, -1.0f, 0.25f, 0.0f});
        return std::uint64_t{0x1234};  // x0 is not the out block any more
    };
    const auto buttons = [&] {
        m.Set(kOut + addr::kOutHold, std::uint16_t{0x0005});
        m.Set(kOut + addr::kOutEdgeA, std::uint16_t{0x0002});
        m.Set(kOut + addr::kOutEdgeB, std::uint8_t{0x80});
        return std::uint64_t{0x1234};
    };
    const std::uint64_t from_sticks = kBase + 0x279e244, from_buttons = kBase + addr::kButtonsOffReturn;
    CHECK(Call(w, m, CoopPlayerInput::kWrapInputOff, kLocalNode, from_sticks, On) == 0);
    CHECK(Call(w, m, CoopPlayerInput::kWrapSticks, kLocalNode, 0, sticks) == 0);
    CHECK(Call(w, m, CoopPlayerInput::kWrapInputOff, kLocalNode, from_buttons, On) == 0);
    CHECK(Call(w, m, CoopPlayerInput::kWrapButtons, kLocalNode, 0, buttons) == 0);
    auto frames = w.TakeLocal();
    REQUIRE(frames.size() == 1);
    CHECK(frames[0].hold == 0x0005);
    CHECK(frames[0].edge_a == 0x0002);
    CHECK(frames[0].edge_b == 0x80);
    CHECK((frames[0].sticks == std::array<float, 4>{0.5f, -1.0f, 0.25f, 0.0f}));
    CHECK(w.TakeLocal().empty());

    // Input off (the pause menu): both nodes skip their read; one neutral frame, sticks at rest.
    CHECK(Call(w, m, CoopPlayerInput::kWrapInputOff, kLocalNode, from_sticks, Off) == 0);
    CHECK(Call(w, m, CoopPlayerInput::kWrapInputOff, kLocalNode, from_buttons, Off) == 0);
    frames = w.TakeLocal();
    REQUIRE(frames.size() == 1);
    CHECK(frames[0] == Frame{});

    // A node whose actor cannot be read: the game's own read, nothing kept.
    CHECK(Call(w, m, CoopPlayerInput::kWrapButtons, 0x7777, 0, buttons) == 0);
    CHECK(w.TakeLocal().empty());
    // Another wrap id is not ours.
    std::uint64_t x[32] = {};
    CHECK(w.Wrap(CoopPlayerInput::kWrapControllerGet, 0, x, m.Guest()) == 0);
    CHECK(w.Wrap(3, 0, x, m.Guest()) == 0);
}

TEST_CASE("player input: another player's Link gets that player's frames in place of the pad") {
    Memory m;
    Wraps w;
    w.SetMainBase(kBase);
    m.Node(kLocalNode, kLocal);
    m.Node(kRemoteNode, kRemote);
    w.SetRemote(2, kRemote);
    Frame a;
    a.hold = 0x0101;
    a.edge_a = 0x0100;
    a.edge_b = 0x01;
    a.sticks = {0.0f, 1.0f, 0.0f, 0.0f};
    w.Receive(2, {a});

    const auto never = []() -> std::uint64_t {
        FAIL("the game's read ran for another player's Link");
        return 0;
    };
    // The local game's menu does not stop it.
    std::uint64_t x[32] = {};
    x[0] = 0xdead;
    x[19] = kRemoteNode;
    CHECK(w.Wrap(CoopPlayerInput::kWrapInputOff, 0, x, m.Guest()) == 1);
    CHECK(x[0] == 0);

    m.Zero(kOut, 0x20);
    CHECK(Call(w, m, CoopPlayerInput::kWrapButtons, kRemoteNode, 0, never) == 1);
    CHECK(m.Get<std::uint16_t>(kOut + addr::kOutHold) == 0x0101);
    CHECK(m.Get<std::uint16_t>(kOut + addr::kOutEdgeA) == 0x0100);
    CHECK(m.Get<std::uint8_t>(kOut + addr::kOutEdgeB) == 0x01);
    CHECK(Call(w, m, CoopPlayerInput::kWrapSticks, kRemoteNode, 0, never) == 1);
    CHECK((m.Get<std::array<float, 4>>(kOut) == a.sticks));

    // Nothing new arrived (their game is slower): held stays held, nothing is pressed again.
    m.Zero(kOut, 0x20);
    CHECK(Call(w, m, CoopPlayerInput::kWrapButtons, kRemoteNode, 0, never) == 1);
    CHECK(m.Get<std::uint16_t>(kOut + addr::kOutHold) == 0x0101);
    CHECK(m.Get<std::uint16_t>(kOut + addr::kOutEdgeA) == 0);
    CHECK(m.Get<std::uint8_t>(kOut + addr::kOutEdgeB) == 0);
    CHECK(w.QueueOf(2).repeated() == 1);

    // Nothing of it is sent on as our own input.
    CHECK(w.TakeLocal().empty());

    // The actor is gone (a scene change) and made again: it starts with an empty queue.
    w.Receive(2, {a});
    w.SetRemote(2, 0);
    w.SetRemote(2, kRemote);
    CHECK(w.QueueOf(2).Waiting() == 0);
    // Numbers outside 1..16 are ignored.
    w.SetRemote(0, kLocal);
    w.SetRemote(17, kLocal);
    w.Receive(0, {a});
    w.Receive(200, {a});
    CHECK(Call(w, m, CoopPlayerInput::kWrapButtons, kLocalNode, 0, On) == 0);
}

TEST_CASE("player input: a faster sender's frames are merged, and no press is lost") {
    Queue q;
    for (int i = 0; i < 10; ++i) {
        Frame f;
        f.hold = static_cast<std::uint16_t>(i);
        f.edge_b = i == 2 ? 0x04 : 0;  // one press, early
        f.edge_a = i == 3 ? 0x10 : 0;
        q.Push(f);
    }
    CHECK(q.Waiting() == Queue::kMaxWaiting);
    CHECK(q.merged() == 6);
    const Frame first = q.Next();
    CHECK(first.hold == 6);       // the oldest kept frame's level
    CHECK(first.edge_b == 0x04);  // with every edge that was merged into it
    CHECK(first.edge_a == 0x10);
    CHECK(q.Next().edge_b == 0);
    CHECK(q.Current().hold == 7);
}

TEST_CASE("player input: frames survive the wire; runs of a level frame are one entry") {
    std::vector<Frame> frames;
    Frame run;
    run.hold = 0x2001;
    run.sticks = {0.25f, -0.75f, 1.0f, -1.0f};
    for (int i = 0; i < 40; ++i) frames.push_back(run);
    Frame press = run;
    press.edge_b = 0x02;
    frames.push_back(press);
    frames.push_back(press);  // an edge frame is never folded into a run
    frames.push_back(run);
    const auto bytes = CoopPlayerInput::EncodeBatch(frames);
    CHECK(bytes.size() == 1 + 4 * (1 + CoopPlayerInput::kFrameBytes));
    const auto back = CoopPlayerInput::DecodeBatch(bytes.data(), bytes.size());
    REQUIRE(back);
    CHECK(*back == frames);
    // An empty batch, a cut one, a run of 0.
    const auto none = CoopPlayerInput::EncodeBatch({});
    REQUIRE(CoopPlayerInput::DecodeBatch(none.data(), none.size()));
    CHECK(CoopPlayerInput::DecodeBatch(none.data(), none.size())->empty());
    CHECK_FALSE(CoopPlayerInput::DecodeBatch(bytes.data(), bytes.size() - 1));
    CHECK_FALSE(CoopPlayerInput::DecodeBatch(bytes.data(), 0));
    auto bad = bytes;
    bad[1] = 0;
    CHECK_FALSE(CoopPlayerInput::DecodeBatch(bad.data(), bad.size()));
    // A stick value that is not a number, or out of range, never reaches the game.
    Frame wild = run;
    wild.sticks = {std::nanf(""), 5.0f, -7.0f, 0.5f};
    std::uint8_t one[CoopPlayerInput::kFrameBytes];
    CoopPlayerInput::Encode(wild, one);
    CHECK((CoopPlayerInput::Decode(one).sticks == std::array<float, 4>{0.0f, 1.0f, -1.0f, 0.5f}));
}

TEST_CASE("player input: the move stick is turned from their camera to ours") {
    Frame f;
    f.sticks = {0.0f, 1.0f, 0.3f, 0.4f};  // forward on their screen
    CoopPlayerInput::TurnMoveStick(f, 1.57079632679f);  // their camera a quarter turn from ours
    CHECK(f.sticks[0] == doctest::Approx(-1.0f));
    CHECK(std::fabs(f.sticks[1]) < 0.001f);
    CHECK(f.sticks[2] == 0.3f);  // the other stick is theirs alone
    CHECK(f.sticks[3] == 0.4f);
}
