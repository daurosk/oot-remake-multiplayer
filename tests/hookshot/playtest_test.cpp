// SPDX-License-Identifier: GPL-3.0-or-later
// The play-test bridge's game-free part (hookshot/src/coop_playtest_core.h): the frame hold driven by a fake frame
// source (a thread calling AtFrame as the game's calc thread does), and every command's checks.
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include "coop_playtest_core.h"

using namespace CoopPlaytest;
using namespace std::chrono_literals;

namespace {

// The game's calc thread: a frame hook about every millisecond until stopped.
struct FakeGame {
    Gate& gate;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> updates{0};  // frames whose update ran (past the hook)
    std::thread thread;
    explicit FakeGame(Gate& g) : gate(g) {
        thread = std::thread([this] {
            while (!stop.load()) {
                gate.AtFrame();
                updates.fetch_add(1);
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    ~FakeGame() {
        stop.store(true);
        gate.Resume();
        thread.join();
    }
};

std::optional<Request> Parse(std::string_view line) {
    std::string error;
    return ParseRequest(line, error);
}

std::string ParseError(std::string_view line) {
    std::string error;
    CHECK_FALSE(ParseRequest(line, error));
    return error;
}

}  // namespace

TEST_CASE("playtest: the port and idle switches: off unless a real port is given") {
    CHECK_FALSE(PortFrom(""));
    CHECK_FALSE(PortFrom("0"));
    CHECK_FALSE(PortFrom("80"));
    CHECK_FALSE(PortFrom("65536"));
    CHECK_FALSE(PortFrom("5001x"));
    CHECK_FALSE(PortFrom("-5001"));
    CHECK(PortFrom("5001") == 5001);
    CHECK(PortFrom("5012") == 5012);
    CHECK(IdleFrom("") == kDefaultIdleS);
    CHECK(IdleFrom("abc") == kDefaultIdleS);
    CHECK(IdleFrom("-1") == kDefaultIdleS);
    CHECK(IdleFrom("0") == 0);
    CHECK(IdleFrom("30") == 30);
}

TEST_CASE("playtest: pause holds the game still, resume lets it run") {
    Gate gate(0);
    FakeGame game(gate);
    REQUIRE(gate.Pause(2s));
    CHECK(gate.Held());
    const auto frame = gate.Frame();
    const auto updates = game.updates.load();
    std::this_thread::sleep_for(100ms);
    CHECK(gate.Frame() == frame);          // no frame hook passed
    CHECK(game.updates.load() == updates);  // no update ran
    gate.Resume();
    std::this_thread::sleep_for(50ms);
    CHECK(gate.Frame() > frame);
    CHECK_FALSE(gate.Held());
}

TEST_CASE("playtest: the hold is told when it starts and when it ends (co-op keeps sending the still player)") {
    Gate gate(0);
    std::atomic<int> starts{0}, ends{0};
    std::atomic<bool> held{false};
    gate.OnHeld([&](bool h) {
        held.store(h);
        (h ? starts : ends).fetch_add(1);
    });
    FakeGame game(gate);
    REQUIRE(gate.Pause(2s));
    CHECK(held.load());
    CHECK(starts.load() == 1);
    CHECK(ends.load() == 0);
    gate.Resume();
    std::this_thread::sleep_for(50ms);
    CHECK_FALSE(held.load());
    CHECK(ends.load() == 1);
}

TEST_CASE("playtest: the hold is told it ended on a step, a stop and the idle time") {
    std::atomic<int> starts{0}, ends{0};
    auto watch = [&](Gate& gate) {
        gate.OnHeld([&](bool h) { (h ? starts : ends).fetch_add(1); });
    };
    {
        Gate gate(0);
        watch(gate);
        FakeGame game(gate);
        REQUIRE(gate.Pause(2s));
        REQUIRE(gate.Step(3, 2s));  // the hold ends for 3 frames and starts again
        CHECK(ends.load() == 1);
        CHECK(starts.load() == 2);
        // The module's Stop and a client leaving end the hold with Resume (coop_playtest.cpp Stop).
        gate.Resume();
        std::this_thread::sleep_for(50ms);
        CHECK(ends.load() == 2);
    }
    starts = 0;
    ends = 0;
    {
        Gate gate(0.05);  // the idle time runs out
        watch(gate);
        FakeGame game(gate);
        REQUIRE(gate.Pause(2s));
        std::this_thread::sleep_for(300ms);
        CHECK_FALSE(gate.Held());
        CHECK(starts.load() == ends.load());
    }
}

TEST_CASE("playtest: step N runs exactly N frames and holds again, from a hold or from running") {
    Gate gate(0);
    FakeGame game(gate);
    for (const std::uint32_t n : {1u, 2u, 7u, 60u}) {
        std::uint64_t from = 0;
        REQUIRE(gate.Step(n, 2s, &from));
        CHECK(gate.Held());
        CHECK(gate.Frame() - from == n);
        // and nothing slips after the step: still there a while later
        const auto at = gate.Frame();
        const auto updates = game.updates.load();
        std::this_thread::sleep_for(30ms);
        CHECK(gate.Frame() == at);
        CHECK(game.updates.load() == updates);
    }
    gate.Resume();
    std::this_thread::sleep_for(20ms);
    std::uint64_t from = 0;
    REQUIRE(gate.Step(5, 2s, &from));  // (from running: held first, then 5)
    CHECK(gate.Frame() - from == 5);
}

TEST_CASE("playtest: a job runs on the frame thread inside the hold, no frame passing") {
    Gate gate(0);
    FakeGame game(gate);
    REQUIRE(gate.Pause(2s));
    const auto frame = gate.Frame();
    std::thread::id ran_on;
    REQUIRE(gate.RunHeld([&] { ran_on = std::this_thread::get_id(); }, 2s));
    CHECK(ran_on == game.thread.get_id());
    CHECK(gate.Frame() == frame);
    CHECK(gate.Held());
}

TEST_CASE("playtest: the hold ends when the client leaves (Resume) and when the idle time runs out") {
    {
        Gate gate(0);
        FakeGame game(gate);
        REQUIRE(gate.Pause(2s));
        gate.Resume();  // (what the bridge does when its client closes or crashes)
        std::this_thread::sleep_for(30ms);
        CHECK_FALSE(gate.Holding());
        CHECK_FALSE(gate.Held());
    }
    {
        Gate gate(0.1);
        FakeGame game(gate);
        REQUIRE(gate.Pause(2s));
        std::this_thread::sleep_for(400ms);
        CHECK_FALSE(gate.Held());
        CHECK(gate.TakeEndNote() == "idle");
        CHECK(gate.TakeEndNote().empty());
    }
}

TEST_CASE("playtest: no frame source (a load) answers in time instead of hanging") {
    Gate gate(0);
    CHECK_FALSE(gate.Pause(50ms));
    CHECK_FALSE(gate.Step(3, 50ms));
    CHECK_FALSE(gate.RunHeld([] {}, 50ms));
}

TEST_CASE("playtest: the stick's angle and strength as Longshot's axes") {
    auto near = [](Axes a, double x, double y) { return std::fabs(a.x - x) < 1e-6 && std::fabs(a.y - y) < 1e-6; };
    CHECK(near(StickAxes(0, 1), 0, 1));     // up
    CHECK(near(StickAxes(90, 1), 1, 0));    // right
    CHECK(near(StickAxes(180, 0.5), 0, -0.5));
    CHECK(near(StickAxes(-90, 1), -1, 0));
    CHECK(near(StickAxes(45, 2), std::sqrt(0.5), std::sqrt(0.5)));  // strength capped at 1
    CHECK(near(StickAxes(NAN, 1), 0, 0));
}

namespace {
constexpr double kPi = 3.14159265358979323846;

// A camera at yaw `cam` (degrees, 0 toward +z, 90 toward +x) pitched down `pitch` degrees, as the game's clip_from_world
// rows: x = sx * right + jitter * forward, w = forward (depth into the picture). `right_handed`: the picture's right is
// forward x up (+z forward gives -x right), as GL and Vulkan views are; false: the mirror (+z forward gives +x right).
std::array<float, 16> Camera(double cam, bool right_handed, double pitch = 0, double jitter = 0) {
    const double c = cam * kPi / 180, p = pitch * kPi / 180;
    const double fx = std::sin(c) * std::cos(p), fy = -std::sin(p), fz = std::cos(c) * std::cos(p);
    const double rx = right_handed ? -std::cos(c) : std::cos(c), rz = right_handed ? std::sin(c) : -std::sin(c);
    std::array<float, 16> m{};
    m[0] = static_cast<float>(1.3 * rx + jitter * fx), m[1] = static_cast<float>(jitter * fy);
    m[2] = static_cast<float>(1.3 * rz + jitter * fz), m[3] = 5.0f;
    m[5] = 1.7f;  // (y: not used for the ground)
    m[12] = static_cast<float>(fx), m[13] = static_cast<float>(fy), m[14] = static_cast<float>(fz), m[15] = 2.0f;
    return m;
}

// The world yaw Link walks for a stick, as the game moves him: stick up = the camera's forward on the ground, stick
// right = the picture's right.
double WalkedYaw(Axes a, double cam, bool right_handed) {
    const double c = cam * kPi / 180;
    const double fx = std::sin(c), fz = std::cos(c);
    const double rx = right_handed ? -std::cos(c) : std::cos(c), rz = right_handed ? std::sin(c) : -std::sin(c);
    return std::atan2(a.y * fx + a.x * rx, a.y * fz + a.x * rz) * 180 / kPi;
}

double AngleGap(double a, double b) { return std::fabs(std::remainder(a - b, 360.0)); }
}  // namespace

TEST_CASE("playtest: move turns a world yaw into the stick that walks that way, for any camera") {
    for (const bool rh : {true, false}) {
        for (const double cam : {0.0, 45.0, 90.0, 135.0, 180.0, -135.0, -90.0, -30.0}) {
            for (const double want : {0.0, 90.0, 180.0, -90.0, 30.0, 150.0, -150.0, -60.0}) {
                for (const double pitch : {0.0, 35.0, 80.0}) {
                    CAPTURE(rh);
                    CAPTURE(cam);
                    CAPTURE(want);
                    CAPTURE(pitch);
                    const auto a = MoveAxes(Camera(cam, rh, pitch, 0.02), want, 1.0);
                    REQUIRE(a);
                    CHECK(AngleGap(WalkedYaw(*a, cam, rh), want) < 0.01);
                    CHECK(std::hypot(a->x, a->y) == doctest::Approx(1.0).epsilon(1e-6));
                }
            }
        }
    }
}

TEST_CASE("playtest: move's stick in the four quadrants with the camera toward +z") {
    auto near = [](std::optional<Axes> a, double x, double y) {
        return a && std::fabs(a->x - x) < 1e-6 && std::fabs(a->y - y) < 1e-6;
    };
    const auto rh = Camera(0, true), lh = Camera(0, false);
    CHECK(near(MoveAxes(rh, 0, 1), 0, 1));     // away from the camera: up
    CHECK(near(MoveAxes(rh, 180, 1), 0, -1));  // toward it: down
    CHECK(near(MoveAxes(rh, 90, 1), -1, 0));   // +x is the picture's left in a right-handed view
    CHECK(near(MoveAxes(rh, -90, 1), 1, 0));
    CHECK(near(MoveAxes(lh, 90, 1), 1, 0));    // and its right in the mirror
    CHECK(near(MoveAxes(lh, -90, 0.5), -0.5, 0));
    // The camera turned behind Link (toward -z): the same world yaw is the other way on the stick.
    CHECK(near(MoveAxes(Camera(180, true), 0, 1), 0, -1));
    CHECK(near(MoveAxes(Camera(90, true), 90, 1), 0, 1));
    CHECK(near(MoveAxes(Camera(90, true), 0, 1), 1, 0));
    CHECK(MoveAxes(rh, 0, 2)->y == doctest::Approx(1.0));  // strength capped at 1
    CHECK(ViewYaw(Camera(90, true)).value() == doctest::Approx(90.0));
    CHECK(ViewYaw(Camera(-135, false)).value() == doctest::Approx(-135.0));
}

TEST_CASE("playtest: the state's view has the camera's yaw, pitch and matrix") {
    const std::string v = ViewMember(Camera(90, true, 30), true);
    CHECK(v.rfind(",\"view\":{\"yaw\":90,\"pitch\":-30,\"clip\":[", 0) == 0);
    CHECK(v.find(",1.7,") != std::string::npos);
    CHECK(v.size() - v.rfind("],\"y_down\":true}") == std::string("],\"y_down\":true}").size());
    CHECK(ViewMember(Camera(-45, false)).find("\"yaw\":-45,\"pitch\":0,") != std::string::npos);
    CHECK(ViewMember(std::nullopt) == ",\"view\":null");
    CHECK(ViewMember(std::array<float, 16>{}) == ",\"view\":null");   // no forward
    auto bad = Camera(0, true);
    bad[5] = NAN;
    CHECK(ViewMember(bad) == ",\"view\":null");
}

TEST_CASE("playtest: move has no stick for a camera with no ground direction") {
    CHECK_FALSE(MoveAxes(Camera(0, true, 90), 0, 1));  // straight down
    std::array<float, 16> zero{};
    CHECK_FALSE(MoveAxes(zero, 0, 1));
    auto bad = Camera(0, true);
    bad[0] = NAN;
    CHECK_FALSE(MoveAxes(bad, 0, 1));
    CHECK_FALSE(MoveAxes(Camera(0, true), NAN, 1));
    CHECK_FALSE(ViewYaw(zero));
}

TEST_CASE("playtest: the move hold puts its stick in only while on, and counts what it did") {
    MoveHold hold;
    std::int32_t stick[4] = {100, -200, 300, 400};
    CHECK_FALSE(hold.Apply(Camera(0, true), stick));
    CHECK(stick[0] == 100);
    CHECK(stick[1] == -200);

    hold.Start(-90, 1.0);
    CHECK(hold.On());
    CHECK(hold.Apply(Camera(0, true), stick));
    CHECK(stick[0] == 32767);  // -x: the picture's right
    CHECK(stick[1] == 0);
    CHECK(stick[2] == 300);  // the right stick is left alone
    CHECK(stick[3] == 400);
    CHECK(hold.Apply(std::nullopt, stick));  // no camera: centred, not a stale direction
    CHECK(stick[0] == 0);
    CHECK(stick[1] == 0);
    CHECK(hold.Apply(Camera(180, true), stick));  // the camera turned: the next read follows it
    CHECK(stick[0] == -32767);
    const auto did = hold.Stop();
    CHECK_FALSE(hold.On());
    CHECK(did.reads == 3);
    CHECK(did.no_camera == 1);
    REQUIRE(did.last);
    CHECK(did.last->x == doctest::Approx(-1.0));
    REQUIRE(did.view);
    CHECK(AngleGap(*did.view, 180) < 1e-6);
    stick[0] = 5;
    CHECK_FALSE(hold.Apply(Camera(0, true), stick));
    CHECK(stick[0] == 5);
    hold.Start(0, 1.0);
    CHECK(hold.Stop().reads == 0);  // a new move starts its counts again
}

TEST_CASE("playtest: the pad thread reads the move hold while moves start and stop") {
    // The pad hook skips the lock when no move is on; a read that races a Start or a Stop still gives
    // either the untouched stick or the whole move, and every read it took is counted.
    MoveHold hold;
    std::atomic<bool> done{false};
    std::atomic<std::uint32_t> taken{0}, bad{0};
    std::thread pad([&] {
        while (!done.load()) {
            std::int32_t stick[4] = {7, 7, 0, 0};
            if (hold.Apply(Camera(0, true), stick)) {
                ++taken;
                if (stick[0] != 32767 || stick[1] != 0) ++bad;
            } else if (stick[0] != 7 || stick[1] != 7) {
                ++bad;
            }
        }
    });
    std::uint32_t counted = 0;
    for (int i = 0; i < 2000; ++i) {
        hold.Start(-90, 1.0);
        counted += hold.Stop().reads;
    }
    done = true;
    pad.join();
    CHECK(bad.load() == 0);
    CHECK(counted == taken.load());
}

TEST_CASE("playtest: move parses its world yaw, strength and frames") {
    auto r = Parse(R"({"cmd":"move","yaw":-135.5,"frames":8})");
    REQUIRE(r);
    CHECK(r->yaw == doctest::Approx(-135.5));
    CHECK(r->strength == doctest::Approx(1.0));
    CHECK(r->frames == 8);
    CHECK(r->exclusive);
    r = Parse(R"({"cmd":"move","yaw":10,"strength":0.4,"exclusive":false})");
    REQUIRE(r);
    CHECK(r->strength == doctest::Approx(0.4));
    CHECK_FALSE(r->exclusive);
    CHECK(ParseError(R"({"cmd":"move"})").find("yaw") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"move","yaw":"north"})").find("yaw") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"move","yaw":0,"strength":1.5})").find("strength") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"move","yaw":0,"frames":0})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"move","yaw":0,"exclusive":1})").find("exclusive") != std::string::npos);
    // The pad sequence a move sends holds both sticks centred: the pad hook puts the move's stick in.
    Request pad = *r;
    const std::string c = ControlInput(pad, 4);
    CHECK(c.find("\"lx\":0.000000,\"ly\":0.000000") != std::string::npos);
}

TEST_CASE("playtest: every command parses, ids come back as given") {
    for (const char* line : {R"({"cmd":"hello"})", R"({"cmd":"state","id":"s1"})", R"({"cmd":"pause"})",
                             R"({"cmd":"resume"})"}) {
        CHECK(Parse(line));
    }
    auto r = Parse(R"({"id":7,"cmd":"step","frames":60})");
    REQUIRE(r);
    CHECK(r->frames == 60);
    CHECK(Reply(r->id, 10, true, "").rfind("{\"id\":7,\"ok\":true,\"frame\":10,\"paused\":true", 0) == 0);
    CHECK(Parse(R"({"cmd":"step"})")->frames == 1);

    r = Parse(R"({"cmd":"input","buttons":["A","ZR"],"stick":[90,1.0],"frames":20})");
    REQUIRE(r);
    CHECK(r->buttons.size() == 2);
    CHECK(r->stick);
    CHECK_FALSE(r->rstick);
    CHECK(r->exclusive);
    const std::string c = ControlInput(*r, 3);
    CHECK(c.find("\"cmd\":\"sequence\"") != std::string::npos);
    CHECK(c.find("\"frames\":20") != std::string::npos);
    CHECK(c.find("\"buttons\":[\"A\",\"ZR\"]") != std::string::npos);
    CHECK(c.find("\"lx\":1.000000,\"ly\":0.000000") != std::string::npos);
    CHECK(c.find("\"wait\":false") != std::string::npos);
    CHECK(c.find("\"exclusive\":true") != std::string::npos);
    // (nothing given: still a step, both sticks centred, so Longshot does not refuse it as empty)
    CHECK(ControlInput(*Parse(R"({"cmd":"input","exclusive":false})"), 4).find("\"lx\":0.000000") != std::string::npos);

    r = Parse(R"({"cmd":"warp","scene":"KokiriForest_Child","entrance":"Kokiri_Start","timeout":30})");
    REQUIRE(r);
    CHECK(r->scene == "KokiriForest_Child");
    CHECK(r->entrance == "Kokiri_Start");
    CHECK(r->timeout_s == 30);

    r = Parse(R"({"cmd":"flag","name":"ForestTemple_Room18_Picture_Shot","value":1})");
    REQUIRE(r);
    CHECK(r->value == "1");
    CHECK(Parse(R"({"cmd":"flag","name":"0x1A2B","value":false})")->value == "0");
    CHECK(Parse(R"({"cmd":"flag","name":"Quest.Step","value":"Done"})")->value == "Done");
    CHECK(Parse(R"({"cmd":"flag","name":"X","value":-3})")->value == "-3");

    CHECK(Parse(R"({"cmd":"give","item":"bombs"})")->item == "bombs");
    CHECK(Parse(R"({"cmd":"dump","tag":"after_chest-2"})")->tag == "after_chest-2");
    CHECK(Parse(R"({"cmd":"shot"})")->tag.empty());
    CHECK(Parse(R"({"cmd":"shot","name":"door_open"})")->tag == "door_open");
    // key (port-g7, B #137): a host key's name as the window's virtual key (WM_KEYDOWN's wParam)
    CHECK(Parse(R"({"cmd":"key","key":"F1"})")->key == "F1");
    CHECK(Parse(R"({"cmd":"key","key":"F1"})")->vk == 0x70u);
    CHECK(Parse(R"({"cmd":"key","key":"F6"})")->vk == 0x75u);
    CHECK(Parse(R"({"cmd":"key","key":"F12"})")->vk == 0x7Bu);
    CHECK(Parse(R"({"cmd":"key","key":"T"})")->vk == static_cast<std::uint32_t>('T'));
    CHECK(Parse(R"({"cmd":"key","key":"7"})")->vk == static_cast<std::uint32_t>('7'));
    // typing-blink-0121: the chat box's own keys, and typed characters
    CHECK(Parse(R"({"cmd":"key","key":"Esc"})")->vk == 0x1Bu);
    CHECK(Parse(R"({"cmd":"key","key":"Enter"})")->vk == 0x0Du);
    CHECK(Parse(R"({"cmd":"key","key":"Backspace"})")->vk == 0x08u);
    CHECK(Parse(R"({"cmd":"key","text":"hello there"})")->chars == "hello there");
    CHECK(Parse(R"({"cmd":"key","text":"hello there"})")->vk == 0u);
}

TEST_CASE("playtest: bad requests are refused with a reason, never acted on") {
    CHECK(ParseError("not json").find("JSON") != std::string::npos);
    CHECK(ParseError("[1,2]").find("object") != std::string::npos);
    CHECK(ParseError(R"({"id":1})").find("cmd") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"format_disk"})").find("cmd") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"step","frames":0})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"step","frames":3601})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"step","frames":1.5})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"step","frames":"10"})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","buttons":["A","HOME"]})").find("buttons") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","buttons":"A"})").find("buttons") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","stick":[90]})").find("stick") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","stick":[90,1.5]})").find("stick") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","rstick":["up",1]})").find("rstick") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","exclusive":1})").find("exclusive") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"warp"})").find("scene") != std::string::npos);
    // (a test command line is built from these: no spaces, ';' or '#' that would make a second step)
    CHECK(ParseError(R"({"cmd":"warp","scene":"A B"})").find("scene") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"warp","scene":"A;give bombs"})").find("scene") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"warp","scene":"A","entrance":"x\ngive"})").find("entrance") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"warp","scene":"A","timeout":0})").find("timeout") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag","name":"X"})").find("value") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag","name":"X","value":1.5})").find("value") != std::string::npos);
    // (past int32: refused here, never handed to the game's thread; review-01111-b)
    CHECK(ParseError(R"({"cmd":"flag","name":"X","value":2147483648})").find("value") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag","name":"X","value":2150000000})").find("value") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag","name":"X","value":-2147483649})").find("value") != std::string::npos);
    CHECK(Parse(R"({"cmd":"flag","name":"X","value":2147483647})")->value == "2147483647");
    CHECK(Parse(R"({"cmd":"flag","name":"X","value":-2147483648})")->value == "-2147483648");
    CHECK(ParseError(R"({"cmd":"flag","name":"X Y","value":1})").find("name") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"give"})").find("item") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"give","item":"bombs#x"})").find("item") != std::string::npos);
    // dump and shot name files only inside the bridge's own folder (review-01111-b 1)
    CHECK(ParseError(R"({"cmd":"dump"})").find("tag") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"dump","tag":"../x"})").find("tag") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"dump","tag":"C:\\x"})").find("tag") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"shot","path":"/tmp/x.png"})").find("path") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"shot","name":"../../x"})").find("name") != std::string::npos);
    // key: F1 to F12, A to Z, 0 to 9 only (the pad is `input`, text is `act chat`)
    CHECK(ParseError(R"({"cmd":"key"})").find("key") != std::string::npos);
    for (const char* bad : {"f1", "F0", "F01", "F13", "TAB", "t", "", "F1 "}) {
        CHECK(ParseError(std::string(R"({"cmd":"key","key":")") + bad + "\"}").find("F1 to F12") != std::string::npos);
    }
    CHECK(ParseError(R"({"cmd":"key","text":""})").find("printable") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"key","text":"tab\there"})").find("printable") != std::string::npos);
    CHECK(ParseError(std::string(R"({"cmd":"key","text":")") + std::string(65, 'a') + "\"}").find("64") !=
          std::string::npos);
    CHECK(ParseError(R"({"cmd":"key","key":"T","text":"x"})").find("not both") != std::string::npos);
    CHECK_FALSE(ValidTag(std::string(65, 'a')));
    CHECK(ParseError(std::string(kMaxLine + 1, ' ')).find("64 KB") != std::string::npos);
}

TEST_CASE("playtest: every reply is one line: nested values written compact, strings escaped") {
    // (the JSON text has the escape \n inside a string: raw strings keep the backslash)
    const auto v = CoopModJson::Parse(R"({"a":[1, 2.5, {"b":"x\ny"}], "c":{"d":true,"e":null}})");
    REQUIRE(v);
    const std::string c = Compact(*v);
    CHECK(c == R"({"a":[1,2.5,{"b":"x\ny"}],"c":{"d":true,"e":null}})");
    CHECK(c.find('\n') == std::string::npos);
    CHECK(Quote("two\nlines") == R"("two\nlines")");
    const std::string r = Reply(*v, 3, false, "\"state\":" + c);
    CHECK(r.find('\n') == std::string::npos);
    CHECK(CoopModJson::Parse(r));
    CHECK(ErrorReply(*v, 3, true, "failed", "a\nb").find('\n') == std::string::npos);
    CHECK(Reply(CoopModJson::Value::Of(7.0), 10, true, "") == R"({"id":7,"ok":true,"frame":10,"paused":true})");
}

TEST_CASE("playtest: an error reply carries the frame, the hold and a reason") {
    const std::string e = ErrorReply(CoopModJson::Value{}, 42, false, "busy", "another client \"x\"");
    CHECK(e.find("\"id\":null") != std::string::npos);
    CHECK(e.find("\"ok\":false") != std::string::npos);
    CHECK(e.find("\"frame\":42") != std::string::npos);
    CHECK(e.find("\"paused\":false") != std::string::npos);
    CHECK(e.find("\"error\":\"busy\"") != std::string::npos);
    CHECK(CoopModJson::Parse(e));  // valid JSON, the quote escaped
}

TEST_CASE("playtest: a failed answer always names a reason, even when the game or Longshot gave none") {
    const auto none = CoopModJson::Value{};
    const std::string empty = ErrorReply(none, 1, false, "failed", "");
    CHECK(empty.find("\"error\":\"failed\"") != std::string::npos);
    CHECK(empty.find("\"message\":\"\"") == std::string::npos);        // not an empty message
    CHECK(empty.find("no reason was given") != std::string::npos);
    CHECK(CoopModJson::Parse(empty));
    const std::string given = ErrorReply(none, 1, false, "failed", "line 1: want: wait TICKS");
    CHECK(given.find("want: wait TICKS") != std::string::npos);
    CHECK(given.find("no reason was given") == std::string::npos);       // a real reason is kept as it is
    // Longshot's teleport: the code and the message; with no message, a sentence that says so.
    CHECK(TeleportFailure("failed", "not in play") == "Longshot's teleport: failed: not in play");
    const std::string bare = TeleportFailure("failed", "");
    CHECK(bare.find("failed") != std::string::npos);
    CHECK(bare.find("no message") != std::string::npos);
    CHECK(TeleportFailure("", "").find("failed") != std::string::npos);   // no code either
}

TEST_CASE("playtest: the world member is absent when the snapshot switch is off") {
    CHECK(WorldMember(false, std::nullopt, 10, 1000).empty());
    CoopWorldSnapshot::Snapshot s;
    s.frame = 10;
    s.taken_ms = 1000;
    CHECK(WorldMember(false, s, 10, 1000).empty());  // off: nothing, so the old `state` text is unchanged
}

TEST_CASE("playtest: the world member with no snapshot yet is an empty list with a null frame") {
    const std::string m = WorldMember(true, std::nullopt, 10, 1000);
    const auto v = CoopModJson::Parse("{" + m.substr(1) + "}");
    REQUIRE(v);
    const auto* w = v->Get("world");
    REQUIRE(w);
    CHECK(w->Get("frame")->kind == CoopModJson::Value::Kind::Null);
    CHECK(w->Get("actors")->array.empty());
    // solo: the reason says so, so a tester does not read "nothing here"
    CHECK(Compact(*w->Get("why")).find("no co-op session") != std::string::npos);
}

TEST_CASE("playtest: the world member lists actors with owner, alive and both frames") {
    CoopWorldSnapshot::Snapshot s;
    s.frame = 8;
    s.taken_ms = 1000;
    CoopWorldSnapshot::WorldActor foe;
    foe.id = 4660;
    foe.name = "Enemy_Test";
    foe.kind = CoopWorldSnapshot::Kind::enemy;
    foe.pos = {1.5f, 2.0f, -3.25f};
    foe.dist = 4.0f;
    foe.life = 2;
    foe.max_life = 4;
    foe.owner = 0;
    foe.alive = true;
    CoopWorldSnapshot::WorldActor grass;
    grass.name = "Obj_Plant_Grass_A";
    s.actors = {foe, grass};
    const std::string m = WorldMember(true, s, 10, 1200);
    CHECK(m.find('\n') == std::string::npos);
    const auto v = CoopModJson::Parse("{" + m.substr(1) + "}");
    REQUIRE(v);
    const auto* w = v->Get("world");
    REQUIRE(w);
    CHECK(w->Get("frame")->number == 8);
    CHECK(w->Get("age_ms")->number == 200);
    CHECK(Compact(*w->Get("stale")) == "false");
    CHECK(Compact(*w->Get("behind")) == "true");  // the game is at 10, the snapshot at 8
    CHECK(w->Get("count")->number == 1);  // `state`: the known kinds only, grass is counted
    CHECK(w->Get("total")->number == 2);
    CHECK(w->Get("other_count")->number == 1);
    CHECK(w->Get("why") == nullptr);  // not empty: no reason
    CHECK(Compact(*w->Get("solo")) == "false");  // the sharing walk's list
    s.solo = true;
    const auto solo = CoopModJson::Parse("{" + WorldMember(true, s, 10, 1200).substr(1) + "}");
    REQUIRE(solo);
    CHECK(Compact(*solo->Get("world")->Get("solo")) == "true");  // the solo walk's: a strike by id is refused
    s.solo = false;
    const auto& a0 = w->Get("actors")->array[0];
    CHECK(a0.Get("id")->number == 4660);
    CHECK(Compact(*a0.Get("addr")) == "\"0x1234\"");  // the id in hex, as the debug log prints addresses
    CHECK(Compact(*a0.Get("owner")) == "\"server\"");
    CHECK(Compact(*a0.Get("kind")) == "\"enemy\"");
    CHECK(Compact(*a0.Get("alive")) == "true");
    CHECK(a0.Get("pos")->array.size() == 3);
    CHECK(a0.Get("pos")->array[2].number == doctest::Approx(-3.25).epsilon(0.05));
    const std::string whole = WorldMember(true, s, 10, 1200, true);  // `dump`: the grass too
    const auto fv = CoopModJson::Parse("{" + whole.substr(1) + "}");
    REQUIRE(fv);
    const auto* fw = fv->Get("world");
    REQUIRE(fw);
    CHECK(fw->Get("count")->number == 2);
    const auto& a1 = fw->Get("actors")->array[1];
    CHECK(Compact(*a1.Get("owner")) == "null");  // unknown owner: null
    CHECK(Compact(*a1.Get("alive")) == "null");  // no life read: unknown, not dead
    CHECK(Compact(*a1.Get("life")) == "null");
}

TEST_CASE("playtest: a world snapshot older than 1.5 s is an empty list marked stale") {
    CoopWorldSnapshot::Snapshot s;
    s.frame = 8;
    s.taken_ms = 1000;
    s.actors.resize(3);
    const std::string m = WorldMember(true, s, 8 + CoopWorldSnapshot::kStaleFrames, 1000 + CoopWorldSnapshot::kStaleMs + 1);
    const auto v = CoopModJson::Parse("{" + m.substr(1) + "}");
    REQUIRE(v);
    const auto* w = v->Get("world");
    REQUIRE(w);
    CHECK(Compact(*w->Get("stale")) == "true");
    CHECK(w->Get("count")->number == 0);
    CHECK(w->Get("actors")->array.empty());
    CHECK(w->Get("age_ms")->number == 1501);
    CHECK(Compact(*w->Get("why")).find("stale: no walk for 1501 ms") != std::string::npos);
}

TEST_CASE("playtest: an empty snapshot that is fresh says the walk ran and saw nothing") {
    CoopWorldSnapshot::Snapshot s;
    s.frame = 8;
    s.taken_ms = 1000;
    const std::string m = WorldMember(true, s, 8, 1100);
    const auto v = CoopModJson::Parse("{" + m.substr(1) + "}");
    REQUIRE(v);
    const auto* w = v->Get("world");
    REQUIRE(w);
    CHECK(Compact(*w->Get("stale")) == "false");
    CHECK(w->Get("count")->number == 0);
    CHECK(Compact(*w->Get("why")).find("empty:") != std::string::npos);
}

TEST_CASE("playtest: a room with only other kinds gives state an empty list with a why, and the dump lists them") {
    CoopWorldSnapshot::Snapshot s;
    s.frame = 8;
    s.taken_ms = 1000;
    for (int i = 0; i < 3; ++i) {
        CoopWorldSnapshot::WorldActor a;
        a.id = 1000 + static_cast<std::uint64_t>(i);
        a.name = "Obj_Plant_Grass_A";
        s.actors.push_back(a);
    }
    const auto parse = [&](bool full) {
        const std::string m = WorldMember(true, s, 8, 1000, full);
        return CoopModJson::Parse("{" + m.substr(1) + "}");
    };
    const auto brief = parse(false);
    REQUIRE(brief);
    const auto* w = brief->Get("world");
    CHECK(w->Get("count")->number == 0);
    CHECK(w->Get("other_count")->number == 3);
    CHECK(Compact(*w->Get("why")).find("only other kinds near: dump lists them") != std::string::npos);
    const auto whole = parse(true);
    REQUIRE(whole);
    CHECK(whole->Get("world")->Get("count")->number == 3);
    CHECK(whole->Get("world")->Get("why") == nullptr);
}

TEST_CASE("playtest: state lists at most 40 known-kind actors and counts the rest, the dump lists them all") {
    CoopWorldSnapshot::Snapshot s;
    s.frame = 8;
    s.taken_ms = 1000;
    for (int i = 0; i < 60; ++i) {  // 60 enemies then 100 grass (the snapshot is sorted known kinds first)
        CoopWorldSnapshot::WorldActor a;
        a.id = 100 + static_cast<std::uint64_t>(i);
        a.name = "Enemy_Test";
        a.kind = CoopWorldSnapshot::Kind::enemy;
        s.actors.push_back(a);
    }
    for (int i = 0; i < 100; ++i) {
        CoopWorldSnapshot::WorldActor a;
        a.id = 1000 + static_cast<std::uint64_t>(i);
        a.name = "Obj_Plant_Grass_A";
        s.actors.push_back(a);
    }
    const auto parse = [&](bool full) {
        const std::string m = WorldMember(true, s, 8, 1000, full);
        return CoopModJson::Parse("{" + m.substr(1) + "}");
    };
    const auto brief = parse(false);
    REQUIRE(brief);
    CHECK(brief->Get("world")->Get("count")->number == 40);
    CHECK(brief->Get("world")->Get("actors")->array.size() == 40);
    CHECK(brief->Get("world")->Get("total")->number == 160);
    CHECK(brief->Get("world")->Get("other_count")->number == 100);
    const auto whole = parse(true);
    REQUIRE(whole);
    CHECK(whole->Get("world")->Get("count")->number == 160);
    CHECK(whole->Get("world")->Get("actors")->array.size() == 160);
}

TEST_CASE("playtest: a held state waits for a newer snapshot only when one is coming") {
    CoopWorldSnapshot::Snapshot s;
    s.frame = 5;
    s.taken_ms = 1000;
    CHECK_FALSE(CoopWorldSnapshot::WorthWaiting(std::nullopt, 10, 1000));  // solo: no walk, no wait
    CHECK(CoopWorldSnapshot::WorthWaiting(s, 10, 1100));                   // fresh and behind: wait
    CHECK_FALSE(CoopWorldSnapshot::WorthWaiting(s, 5, 1100));              // already at the frame
    CHECK(CoopWorldSnapshot::WorthWaiting(s, 10, 1000 + CoopWorldSnapshot::kStaleMs + 1));  // (held: not stale)
    CHECK_FALSE(CoopWorldSnapshot::WorthWaiting(s, 5 + CoopWorldSnapshot::kStaleFrames,
                                                1000 + CoopWorldSnapshot::kStaleMs + 1));  // stale: no wait
}

TEST_CASE("playtest: act takes one plain test line") {
    const auto r = Parse(R"({"cmd":"act","lines":"kill Enemy_Test"})");
    REQUIRE(r);
    CHECK(r->cmd == "act");
    CHECK(r->lines == "kill Enemy_Test");
    CHECK(ParseError(R"({"cmd":"act"})").find("lines") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"act","lines":""})").find("lines") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"act","lines":"kill A\ngive bombs"})").find("lines") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"act","lines":"kill A;give bombs"})").find("lines") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"act","lines":"kill A # x"})").find("lines") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"nope"})").find("act") != std::string::npos);  // the command list names act
}

TEST_CASE("playtest: act, flag, warp and give run only when the test runner asked and cheats are on") {
    // (the kit's start files set OOT_COOP_KIT_START for every player, and a real host's game connects to its own
    // server at 127.0.0.1: neither makes a test. Only the runner's own variable does.)
    CHECK(VerbRefusal(true, true).empty());
    const std::string no_runner = VerbRefusal(false, true);  // a player's game with cheats on: refused
    CHECK(no_runner.find("LICHII_COOP_PLAYTEST_VERBS") != std::string::npos);
    CHECK(no_runner.find("OOT_COOP_KIT_START") == std::string::npos);
    CHECK(VerbRefusal(false, false).find("LICHII_COOP_PLAYTEST_VERBS") != std::string::npos);
    const std::string no_cheats = VerbRefusal(true, false);  // the runner, cheats off: refused
    CHECK(no_cheats.find("LICHII_COOP_CHEATS") != std::string::npos);
}

TEST_CASE("playtest: exactly act, warp, flag and give are gated") {
    for (const char* cmd : {"act", "warp", "flag", "give"}) CHECK(GatedCommand(cmd));
    for (const char* cmd : {"state", "dump", "shot", "input", "step", "pause", "resume", "hello", "key", "nope", ""}) {
        CHECK_FALSE(GatedCommand(cmd));
    }
}

TEST_CASE("playtest: flag_read takes a name only and is not gated (it writes nothing)") {
    auto r = Parse(R"({"cmd":"flag_read","name":"Gohma_DefeatBoss","id":4})");
    REQUIRE(r);
    CHECK(r->cmd == "flag_read");
    CHECK(r->name == "Gohma_DefeatBoss");
    CHECK(Parse(R"({"cmd":"flag_read","name":"key64:0x12AB"})")->name == "key64:0x12AB");
    CHECK(Parse(R"({"cmd":"flag_read","name":"0x1A2B"})")->name == "0x1A2B");
    CHECK(ParseError(R"({"cmd":"flag_read"})").find("name") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag_read","name":"A;setflag B 1"})").find("name") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag_read","name":"X","value":1})").find("only reads") != std::string::npos);
    CHECK_FALSE(GatedCommand("flag_read"));
    CHECK(ParseError(R"({"cmd":"nope"})").find("flag_read") != std::string::npos);
}

TEST_CASE("playtest: flag_read replies with the kind and the value, an enum as the hash flag takes back") {
    FlagValue v;
    v.kind = "bool";
    v.number = 1;
    CHECK(FlagReadMembers("Gohma_DefeatBoss", v) == R"("name":"Gohma_DefeatBoss","kind":"bool","value":1)");
    v = {};
    v.kind = "int";
    v.number = 2;
    v.live = 2;
    CHECK(FlagReadMembers("K.SmallKeyNum", v) == R"("name":"K.SmallKeyNum","kind":"int","value":2,"pending":false)");
    v.live = 0;  // a write of 2 still waits in the game's queue
    CHECK(FlagReadMembers("K.SmallKeyNum", v).find(R"("pending":true)") != std::string::npos);
    v = {};
    v.kind = "enum";
    v.hash = 0xABCu;
    CHECK(FlagReadMembers("Q.Step", v) == R"("name":"Q.Step","kind":"enum","value":"0x00000abc")");
    v = {};
    v.kind = "text";
    v.text = "Weapon_Sword_Kokiri";
    CHECK(FlagReadMembers("PlayerStatus.EquipWeapon", v).find(R"("value":"Weapon_Sword_Kokiri")") != std::string::npos);
    v = {};
    v.kind = "int";
    v.number = -5;
    CHECK(FlagReadMembers("X", v).find(R"("value":-5)") != std::string::npos);
    CHECK(FlagReadMembers("Nope", FlagValue{}) == R"("name":"Nope","kind":"unknown","value":null)");
    // a whole reply is one line
    const std::string reply = Reply(CoopModJson::Value::Of(4.0), 10, true, FlagReadMembers("Nope", FlagValue{}));
    CHECK(reply.find('\n') == std::string::npos);
    CHECK(reply.find(R"("ok":true)") != std::string::npos);
}

TEST_CASE("playtest: flag writes an enum hash only when flag_read read it from that same flag") {
    CHECK(HashText(0xABCu) == "0x00000abc");
    CHECK(HashValue("0x00000abc") == 0xABCu);
    CHECK(HashValue("0XFFFFFFFF") == 0xFFFFFFFFu);
    CHECK_FALSE(HashValue("0x0"));
    CHECK_FALSE(HashValue("0x"));
    CHECK_FALSE(HashValue("0x123456789"));
    CHECK_FALSE(HashValue("0xG1"));
    CHECK_FALSE(HashValue("12"));
    CHECK(IsWholeNumber("0"));
    CHECK(IsWholeNumber("-3"));
    CHECK_FALSE(IsWholeNumber("0x1F"));  // (a hash is no number: it was once taken as setflag)
    CHECK_FALSE(IsWholeNumber("-"));
    CHECK_FALSE(IsWholeNumber("Done"));
    CHECK_FALSE(IsWholeNumber(""));

    EnumValues known;
    // nothing read yet: refused, with the reason
    CHECK(EnumHashRefusal(known, "Q.Step", "0x00000abc").find("flag_read") != std::string::npos);
    known.Remember("Q.Step", 0xABCu);
    CHECK(EnumHashRefusal(known, "Q.Step", "0x00000abc").empty());
    CHECK(EnumHashRefusal(known, "Q.Step", "0xABC").empty());  // the same hash, written shorter
    CHECK_FALSE(EnumHashRefusal(known, "Q.Step", "0x00000abd").empty());   // another value
    CHECK_FALSE(EnumHashRefusal(known, "Other.Step", "0x00000abc").empty());  // another flag
    CHECK(EnumHashRefusal(known, "Q.Step", "0x0").find("not 0") != std::string::npos);
    known.Remember("Q.Step", 0xABCu);  // (a repeat is kept once)
    CHECK(known.Known("Q.Step", 0xABCu));
}

TEST_CASE("playtest: an actor id in a strike is refused when there is no world list (solo), a name alone is not") {
    CHECK(StrikeIdRefusal("kill 4096 Enemy_Test", true).empty());
    CHECK(StrikeIdRefusal("kill Enemy_Test", false).empty());
    CHECK(StrikeIdRefusal("hit Enemy_Test 3", false).empty());
    CHECK(StrikeIdRefusal("chat 123 hello", false).empty());
    CHECK(StrikeIdRefusal("", false).empty());
    CHECK(StrikeIdRefusal("kill 4096 Enemy_Test", false).find("no world list") == 0);
    CHECK(StrikeIdRefusal("hit 4096 Enemy_Test 3", false).find("no world list") == 0);
    CHECK(StrikeIdRefusal("KILL\t4096 Enemy_Test", false).find("no world list") == 0);
    CHECK(StrikeIdRefusal("hurt 1\nkill Enemy_Test\nhit 12 Enemy_Test 2", false).find("no world list") == 0);
}

TEST_CASE("playtest: a world list is live when there is a snapshot that is not stale") {
    CHECK_FALSE(WorldListLive(std::nullopt, 5000, 10));
    CoopWorldSnapshot::Snapshot s;
    s.taken_ms = 1000;
    s.frame = 10;
    CHECK(WorldListLive(s, 1000, 10));
    CHECK(WorldListLive(s, 1000 + CoopWorldSnapshot::kStaleMs, 10));
    CHECK(WorldListLive(s, 1001 + CoopWorldSnapshot::kStaleMs, 10 + CoopWorldSnapshot::kStaleFrames - 1));
    CHECK_FALSE(WorldListLive(s, 1001 + CoopWorldSnapshot::kStaleMs, 10 + CoopWorldSnapshot::kStaleFrames));
    CHECK(WorldListLive(s, 1000 + CoopWorldSnapshot::kStaleMs, 10 + CoopWorldSnapshot::kStaleFrames));  // (frames only)
}

TEST_CASE("playtest: a game held for seconds at the frame of its snapshot still has a world list") {
    CoopWorldSnapshot::Snapshot s;
    s.taken_ms = 1000;
    s.frame = 700;
    s.actors.resize(1);
    CHECK(WorldListLive(s, 1000 + 5000, 700));  // held 5 s: no present, no new snapshot, and not stale
    CHECK(WorldListLive(s, 1000 + 60000, 703));
    const auto text = WorldMember(true, s, 700, 1000 + 5000, true);
    CHECK(text.find("\"stale\":false") != std::string::npos);
    CHECK(text.find("\"count\":1") != std::string::npos);
    CHECK(StrikeIdRefusal("kill 4096 Enemy_Test", WorldListLive(s, 1000 + 5000, 700)).empty());
    // the game then runs on with no walk (a load): stale once it is 30 frames past
    CHECK_FALSE(WorldListLive(s, 1000 + 5000, 700 + CoopWorldSnapshot::kStaleFrames));
}

TEST_CASE("playtest: a kill or hit line is a strike line") {
    CHECK(HasStrikeLine("kill Enemy_Test"));
    CHECK(HasStrikeLine("  HIT 12 Enemy_Test 2"));
    CHECK(HasStrikeLine("hurt 1\nkill Enemy_Test"));
    CHECK_FALSE(HasStrikeLine("hurt 4"));
    CHECK_FALSE(HasStrikeLine("chat kill"));
    CHECK_FALSE(HasStrikeLine(""));
}

TEST_CASE("playtest: state.strike carries the newest strike result, and is absent before the first") {
    CHECK(StrikeMember(std::nullopt).empty());
    CHECK(StrikeMember(CoopStrike::Result{3, "gone", "Enemy_Test", 4096}) ==
          ",\"strike\":{\"seq\":3,\"result\":\"gone\",\"name\":\"Enemy_Test\",\"id\":4096}");
    CHECK(StrikeMember(CoopStrike::Result{4, "none with a life", "Enemy_Test", 0}) ==
          ",\"strike\":{\"seq\":4,\"result\":\"none with a life\",\"name\":\"Enemy_Test\",\"id\":0}");
}

TEST_CASE("playtest: the gate line at start says what is on and why") {
    CHECK(VerbGateLine(true, true) == "verbs on (test runner: yes, cheats: on)");
    CHECK(VerbGateLine(true, false) == "verbs off (test runner: yes, cheats: off)");
    CHECK(VerbGateLine(false, true) == "verbs off (test runner: no, cheats: on)");
    CHECK(VerbGateLine(false, false) == "verbs off (test runner: no, cheats: off)");
}

TEST_CASE("playtest: an environment value is on when set, not empty and not 0") {
    CHECK(EnvIsOn("1"));
    CHECK(EnvIsOn("yes"));
    CHECK_FALSE(EnvIsOn(""));
    CHECK_FALSE(EnvIsOn("0"));
}

// ---- F1: Longshot's Link snapshot is old while the game is held ----

namespace {
// A status reply as Longshot's control bridge gives it: `frame` is its present count now, `game.frame` the snapshot's.
CoopModJson::Value Status(const std::string& game, int frame = 1000, const std::string& map = "\"KokiriForest\"") {
    std::string error;
    auto v = CoopModJson::Parse("{\"id\":1,\"ok\":true,\"frame\":" + std::to_string(frame) + ",\"map\":" + map +
                                    ",\"game\":" + game + "}",
                                &error);
    REQUIRE_MESSAGE(v, error);
    return *v;
}
// A game object as the control bridge samples it, taken at present `frame`.
std::string Snap(int frame, bool event = false, bool loading = false) {
    return "{\"frame\":" + std::to_string(frame) +
           ",\"pos\":[1.5,100.0,-2.5],\"facing\":90,\"camera\":45,\"life\":12,\"max_life\":16,\"rupees\":7,"
           "\"age\":\"child\",\"event\":" + (event ? "true" : "false") +
           ",\"loading\":" + (loading ? "true" : "false") + ",\"prompt\":\"Talk\"}";
}
}  // namespace

TEST_CASE("playtest: a snapshot is old only when it is behind the reply's present by more than the slack") {
    CHECK_FALSE(SnapshotIsOld(nullptr));
    const auto same = Status(Snap(1000));
    CHECK_FALSE(SnapshotIsOld(&same));
    const auto close = Status(Snap(1000 - static_cast<int>(kSnapshotSlack)));
    CHECK_FALSE(SnapshotIsOld(&close));
    const auto edge = Status(Snap(1000 - static_cast<int>(kSnapshotSlack) - 1));
    CHECK(SnapshotIsOld(&edge));
    const auto old = Status(Snap(11890), 12984);   // the S8 numbers: held, 1094 presents behind
    CHECK(SnapshotIsOld(&old));
    // no game, a null game, or a game with only the typed event/loading/age (no sample ever taken): not old
    const auto none = Status("null");
    CHECK_FALSE(SnapshotIsOld(&none));
    const auto typed = Status("{\"event\":false,\"loading\":false,\"age\":\"child\"}");
    CHECK_FALSE(SnapshotIsOld(&typed));
}

TEST_CASE("playtest: LinkMembers gives the snapshot's values when it is fresh") {
    const auto fresh = Status(Snap(998));
    const std::string m = LinkMembers(&fresh);
    CHECK(m.find(",\"pos\":[1.5,100,-2.5]") != std::string::npos);
    CHECK(m.find(",\"facing\":90") != std::string::npos);
    CHECK(m.find(",\"life\":12") != std::string::npos);
    CHECK(m.find(",\"rupees\":7") != std::string::npos);
    CHECK(m.find(",\"prompt\":\"Talk\"") != std::string::npos);
    CHECK(m.find(",\"snapshot_present\":998") != std::string::npos);
    CHECK(m.find(",\"snapshot_stale\":false") != std::string::npos);
}

TEST_CASE("playtest: LinkMembers shows null for every Link member of an old snapshot, never the old position") {
    const auto old = Status(Snap(11890), 12984);
    const std::string m = LinkMembers(&old);
    for (const char* k : {"pos", "facing", "camera", "life", "max_life", "rupees", "age", "event", "loading", "prompt"}) {
        CHECK_MESSAGE(m.find(std::string(",\"") + k + "\":null") != std::string::npos, k);
    }
    CHECK(m.find("1.5") == std::string::npos);                       // no old number leaks
    CHECK(m.find(",\"snapshot_present\":11890") != std::string::npos);  // how old it is stays visible
    CHECK(m.find(",\"snapshot_stale\":true") != std::string::npos);
}

TEST_CASE("playtest: RoomMember gives the room of the nearest placement, null when it cannot know") {
    const CoopRooms::Table rooms(CoopRooms::Parse("Dungeon 3 1.5 100 -4\nDungeon 7 40 100 40\n"));
    const auto fresh = Status(Snap(998), 1000, "\"Dungeon\"");
    CHECK(RoomMember(&fresh, "Dungeon", rooms) == ",\"room\":3");
    CHECK(RoomMember(&fresh, "KokiriForest", rooms) == ",\"room\":null");  // a scene with no rooms
    CHECK(RoomMember(&fresh, "", rooms) == ",\"room\":null");
    CHECK(RoomMember(&fresh, "Dungeon", CoopRooms::Table{}) == ",\"room\":null");  // no pack
    const CoopRooms::Table far(CoopRooms::Parse("Dungeon 3 500 100 500\n"));
    CHECK(RoomMember(&fresh, "Dungeon", far) == ",\"room\":null");  // no placement near
    const auto old = Status(Snap(11890), 12984, "\"Dungeon\"");
    CHECK(RoomMember(&old, "Dungeon", rooms) == ",\"room\":null");  // never the room of an old place
    const auto none = Status("null", 1000, "\"Dungeon\"");
    CHECK(RoomMember(&none, "Dungeon", rooms) == ",\"room\":null");
    CHECK(RoomMember(nullptr, "Dungeon", rooms) == ",\"room\":null");
}

TEST_CASE("playtest: BodyMember reads the age from our Link actor's name, null without a live list") {
    CoopWorldSnapshot::Snapshot snap;
    CoopWorldSnapshot::WorldActor puppet;
    puppet.name = "CoopPuppet_Adult";
    snap.actors.push_back(puppet);
    CHECK(BodyMember(snap, true) == ",\"body\":null");  // a puppet is not our Link
    CoopWorldSnapshot::WorldActor link;
    link.name = "Player_Child";
    snap.actors.push_back(link);
    CHECK(BodyMember(snap, true) == ",\"body\":\"child\"");
    snap.actors.back().name = "Player_Adult";
    CHECK(BodyMember(snap, true) == ",\"body\":\"adult\"");
    CHECK(BodyMember(snap, false) == ",\"body\":null");  // a stale list (a load) says nothing
    CHECK(BodyMember(std::nullopt, true) == ",\"body\":null");
}

TEST_CASE("playtest: LinkMembers with no reply or no game: all null, not stale") {
    const std::string none = LinkMembers(nullptr);
    CHECK(none.find(",\"pos\":null") != std::string::npos);
    CHECK(none.find(",\"snapshot_present\":null") != std::string::npos);
    CHECK(none.find(",\"snapshot_stale\":false") != std::string::npos);
    const auto nogame = Status("null");
    CHECK(LinkMembers(&nogame).find(",\"pos\":null") != std::string::npos);
    // only the typed three: event, loading and age come through; the rest is null; not stale
    const auto typed = Status("{\"event\":true,\"loading\":false,\"age\":\"adult\"}");
    const std::string t = LinkMembers(&typed);
    CHECK(t.find(",\"event\":true") != std::string::npos);
    CHECK(t.find(",\"age\":\"adult\"") != std::string::npos);
    CHECK(t.find(",\"pos\":null") != std::string::npos);
    CHECK(t.find(",\"snapshot_stale\":false") != std::string::npos);
}

TEST_CASE("playtest: a reply with no sample at all is missing, not old, and no reply is neither") {
    CHECK_FALSE(SnapshotMissing(nullptr));
    const auto fresh = Status(Snap(998));
    CHECK_FALSE(SnapshotMissing(&fresh));
    const auto nogame = Status("null");                 // a game held since its start: no sample yet
    CHECK(SnapshotMissing(&nogame));
    const auto typed = Status("{\"event\":false,\"loading\":false,\"age\":\"child\"}");
    CHECK(SnapshotMissing(&typed));                      // only the typed members: still no sample
    const auto old = Status(Snap(11890), 12984);
    CHECK_FALSE(SnapshotMissing(&old));                  // old is a different case
}

TEST_CASE("playtest: not_ready says why the Link members are null, and is null when they are not") {
    const auto fresh = Status(Snap(998));
    CHECK(NotReadyReason(&fresh).empty());
    CHECK(LinkMembers(&fresh).find(",\"not_ready\":null") != std::string::npos);

    const auto nogame = Status("null");
    const std::string none = LinkMembers(&nogame);
    CHECK(none.find(",\"pos\":null") != std::string::npos);
    CHECK(none.find(",\"not_ready\":\"no Link sample has been taken yet") != std::string::npos);

    const auto old = Status(Snap(11890), 12984);
    CHECK(LinkMembers(&old).find(",\"not_ready\":\"the Link sample is older than the frame") != std::string::npos);

    // no reply at all: the reason points to `control`
    CHECK(LinkMembers(nullptr).find(",\"not_ready\":\"no answer from Longshot") != std::string::npos);
}

TEST_CASE("playtest: a give is refused while a load runs, and goes otherwise") {
    const auto loading = Status(Snap(998, false, true));
    CHECK(GiveRefusal(&loading).find("loading") != std::string::npos);
    const auto fresh = Status(Snap(998));
    CHECK(GiveRefusal(&fresh).empty());
    CHECK(GiveRefusal(nullptr).empty());              // no reply: not a reason to refuse
}

TEST_CASE("playtest: a give is refused on an old sample with loading false, which is what a real load leaves") {
    // No Link update runs during a load, so the sample from before it stays, with loading false and an old frame.
    const auto old = Status(Snap(11890, false, false), 12984);
    CHECK(GiveRefusal(&old).find("no fresh Link state") != std::string::npos);
    CHECK(GiveRefusal(&old).find("not_ready") != std::string::npos);
    const auto paused = Status(Snap(998, false, false), 1000 + static_cast<int>(kSnapshotSlack) + 1);
    CHECK_FALSE(GiveRefusal(&paused).empty());        // just past the slack
    const auto edge = Status(Snap(1000 - static_cast<int>(kSnapshotSlack), false, false));
    CHECK(GiveRefusal(&edge).empty());                // within the slack: a running game
    const auto nogame = Status("null");
    CHECK_FALSE(GiveRefusal(&nogame).empty());        // no sample at all (a title, or held since its start)
    const auto typed = Status("{\"event\":false,\"loading\":false,\"age\":\"child\"}");
    CHECK_FALSE(GiveRefusal(&typed).empty());         // only the typed members: still no sample
}

// ---- F2: warp by event, or by Longshot's teleport in free play ----

TEST_CASE("playtest: warp goes by the event request during an event and by the teleport in free play") {
    const auto event = Status(Snap(999, true));
    CHECK(PlanWarp(&event, false, "KokiriForest").route == WarpRoute::event);
    CHECK(PlanWarp(&event, true, "KokiriForest").route == WarpRoute::event);     // the event request takes a start position
    const auto play = Status(Snap(999));
    CHECK(PlanWarp(&play, false, "KokiriForest").route == WarpRoute::teleport);
}

TEST_CASE("playtest: warp with an entrance in free play goes by the game's map change event") {
    const auto play = Status(Snap(999));
    CHECK(PlanWarp(&play, true, "KokiriForest").route == WarpRoute::map_event);
    CHECK(PlanWarp(&play, true, "GohmaStage").route == WarpRoute::map_event);
    // The map change event does not change Link's age: the other age's map sets the age first (set_age).
    const auto adult = PlanWarp(&play, true, "KokiriForest_Adult");
    CHECK(adult.route == WarpRoute::map_event);
    CHECK(adult.set_age == "adult");
    // During an event the module's own request still takes the start.
    const auto event = Status(Snap(999, true));
    CHECK(PlanWarp(&event, true, "GohmaStage").route == WarpRoute::event);
}

TEST_CASE("playtest: warp says it cannot tell when the snapshot is old or missing") {
    const auto old = Status(Snap(100, true));
    for (const bool entrance : {false, true}) {
        CHECK(PlanWarp(&old, entrance, "KokiriForest").route == WarpRoute::refuse);
        CHECK(PlanWarp(&old, entrance, "KokiriForest").code == "not_in_play");
        CHECK(PlanWarp(&old, entrance, "KokiriForest").why.find("not readable") != std::string::npos);
        CHECK(PlanWarp(nullptr, entrance, "KokiriForest").why.find("not readable") != std::string::npos);
    }
    const auto nogame = Status("null");
    CHECK(PlanWarp(&nogame, false, "KokiriForest").route == WarpRoute::refuse);
}

TEST_CASE("playtest: a missing sample is treated like an old one by warp, the typed members alone do not count") {
    // No Link sample yet (a game held since its start, a title): only the typed event, loading and age are there.
    for (const char* typed : {"{\"event\":true,\"loading\":false,\"age\":\"child\"}",
                              "{\"event\":false,\"loading\":false,\"age\":\"child\"}"}) {
        const auto boot = Status(typed);
        for (const bool entrance : {false, true}) {
            const auto plan = PlanWarp(&boot, entrance, "KokiriForest");
            CHECK(plan.route == WarpRoute::refuse);
            CHECK(plan.code == "not_in_play");
        }
        CHECK_FALSE(WarpArrived(&boot, "KokiriForest"));   // typed event false, loading false and the right map: still no
    }
}

TEST_CASE("playtest: a warp to the other age's map sets the age first and goes by the map change event") {
    // Longshot's teleport picks the map by Link's current age, and a free-play warp with the suffix alone left Link
    // a child (lane runs): the bridge sets Sequence_IsChildhood and lets frames run, then asks the map change event.
    const auto child = Status(Snap(999));                                  // Snap: age child
    const auto same = PlanWarp(&child, false, "KokiriForest_Child");
    CHECK(same.route == WarpRoute::teleport);
    CHECK(same.set_age.empty());
    CHECK(PlanWarp(&child, false, "kokiriforest_child").route == WarpRoute::teleport);   // any case
    CHECK(PlanWarp(&child, false, "KokiriForest").set_age.empty());                     // no suffix: as it was
    for (const char* scene : {"KokiriForest_Adult", "kokiriforest_ADULT"}) {
        const auto plan = PlanWarp(&child, false, scene);
        CHECK(plan.route == WarpRoute::map_event);
        CHECK(plan.set_age == "adult");
    }
    const auto with_start = PlanWarp(&child, true, "KokiriForest_Adult");
    CHECK(with_start.route == WarpRoute::map_event);
    CHECK(with_start.set_age == "adult");
    const auto event = Status(Snap(999, true));
    CHECK(PlanWarp(&event, false, "KokiriForest_Adult").route == WarpRoute::event);   // the event request sets the age itself
    CHECK(PlanWarp(&event, false, "KokiriForest_Adult").set_age.empty());
}

TEST_CASE("playtest: the teleport's reply says what to wait for and fails at once when the game did not accept it") {
    auto parse = [](const std::string& text) {
        std::string error;
        auto v = CoopModJson::Parse(text, &error);
        REQUIRE_MESSAGE(v, error);
        return *v;
    };
    // Longshot sent the map for Link's age, in its own spelling: wait for that, not for the name asked
    const auto sent = parse("{\"ok\":true,\"sent\":true,\"map\":\"KokiriForest_Child\",\"accepted\":true}");
    const auto w = ReadTeleport(sent, "kokiriforest_adult");
    CHECK_FALSE(w.failed);
    CHECK(w.wait_for == "KokiriForest_Child");
    // no map in the reply: the name asked
    const auto nomap = parse("{\"ok\":true,\"sent\":true,\"map\":null,\"accepted\":true}");
    CHECK(ReadTeleport(nomap, "Market_Child").wait_for == "Market_Child");
    // the game's event manager did not accept it: fail now with the map in the text, do not wait out the timeout
    const auto no = parse("{\"ok\":true,\"sent\":true,\"map\":\"Market_Child\",\"accepted\":false}");
    const auto f = ReadTeleport(no, "Market_Child");
    CHECK(f.failed);
    CHECK(f.code == "refused");
    CHECK(f.why.find("Market_Child") != std::string::npos);
    CHECK(f.why.find("accept") != std::string::npos);
    // no `accepted` member (an older Longshot): not a failure
    const auto old = parse("{\"ok\":true,\"sent\":true,\"map\":\"Market_Child\"}");
    CHECK_FALSE(ReadTeleport(old, "Market_Child").failed);
}

TEST_CASE("playtest: warp arrived compares scene names in any case") {
    const auto in = Status(Snap(999), 1000, "\"KokiriForest_Child\"");
    CHECK(WarpArrived(&in, "kokiriforest_child"));
    CHECK(WarpArrived(&in, "KOKIRIFOREST_CHILD"));
    CHECK_FALSE(WarpArrived(&in, "KOKIRIFOREST"));       // (unchanged rule: only the scene's ending is dropped)
    const auto bare = Status(Snap(999), 1000, "\"KokiriForest\"");
    CHECK(WarpArrived(&bare, "kokiriforest_ADULT"));    // (the age ending is dropped, as before)
    CHECK_FALSE(WarpArrived(&bare, "Market"));
}

TEST_CASE("playtest: warp arrived needs the scene, no event, no load and a fresh snapshot") {
    const auto in = Status(Snap(999), 1000, "\"KokiriForest\"");
    CHECK(WarpArrived(&in, "KokiriForest"));
    CHECK(WarpArrived(&in, "KokiriForest_Child"));     // the map may not carry the age
    CHECK_FALSE(WarpArrived(&in, "Market_Child"));
    CHECK_FALSE(WarpArrived(&in, "Kokiri"));
    const auto exact = Status(Snap(999), 1000, "\"KokiriForest_Child\"");
    CHECK(WarpArrived(&exact, "KokiriForest_Child"));
    const auto loading = Status(Snap(999, false, true), 1000, "\"KokiriForest\"");
    CHECK_FALSE(WarpArrived(&loading, "KokiriForest"));
    const auto event = Status(Snap(999, true), 1000, "\"KokiriForest\"");
    CHECK_FALSE(WarpArrived(&event, "KokiriForest"));
    const auto old = Status(Snap(100), 5000, "\"KokiriForest\"");
    CHECK_FALSE(WarpArrived(&old, "KokiriForest"));        // an old snapshot's event/loading say nothing now
    CHECK_FALSE(WarpArrived(nullptr, "KokiriForest"));
}

TEST_CASE("playtest: a give or flag write the game's frames put back is said at once (did not stick)") {
    // volvagia-rocks-234-2 (2026-11-26): give MaxLife read 16 -> 20, and the next frame put 16 back.
    const auto lost = StickProblem({{"PlayerStatus.MaxLife", 16, 20, 16}});
    CHECK(lost.find("did not stick: PlayerStatus.MaxLife written 20, the game put back 16 within 2 frames") == 0);
    CHECK(StickProblem({{"PlayerStatus.MaxLife", 16, 20, 20}}).empty());      // kept
    CHECK(StickProblem({{"PlayerStatus.Life", 16, 16, 16}}).empty());         // no change: nothing to keep
    CHECK(StickProblem({{"PouchData.Item_Bomb.StockNum", 0, 20, 19}}).empty());   // the game used one: its own use
    CHECK(StickProblem({{"X", std::nullopt, 1, 0}, {"Y", 0, std::nullopt, 0}}).empty());   // unreadable: not judged
    const auto two = StickProblem({{"A", 0, 1, 0}, {"B", 2, 3, 3}, {"C", 5, 6, 5}});
    CHECK(two.find("A written 1") != std::string::npos);
    CHECK(two.find("B written") == std::string::npos);
    CHECK(two.find("; C written 6") != std::string::npos);
}
