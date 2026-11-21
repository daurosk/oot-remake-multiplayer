// Tests for the test-input script parser and stepper
// (lichii/hook/coop_testinput.cpp). No emulator code.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "coop_testinput.h"

using namespace CoopTestInput;

namespace {

Script Must(const char* text) {
    std::string error;
    auto s = Parse(text, error);
    REQUIRE_MESSAGE(s, error);
    return *s;
}

// The output at `t` seconds when only time matters.
Output At(Runner& r, double t, std::vector<std::string>* log = nullptr) {
    std::vector<std::string> ignored;
    r.Advance(t, std::nullopt, log ? *log : ignored);
    return r.Current();
}

double Tick(int n) {
    return static_cast<double>(n) / kTicksPerSecond;
}

} // namespace

TEST_CASE("testinput: wait, hold, press and marks in time") {
    Runner r(Must("# load the save\n"
                  "wait 120\n"
                  "mark go\n"
                  "hold LSTICK_UP+B 90   # run\n"
                  "press a\n"
                  "press ZR+A 10\n"));
    std::vector<std::string> log;
    CHECK(At(r, 0, &log).buttons == 0);
    CHECK(log.empty());
    const Output run = At(r, Tick(120) + 0.001, &log);
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "mark go");
    CHECK(run.buttons == kB);
    REQUIRE(run.left);
    CHECK(*run.left == Stick{0, kStickMax});
    CHECK(At(r, Tick(209)).buttons == kB);
    CHECK(At(r, Tick(211)).buttons == kA);
    CHECK(At(r, Tick(217)).buttons == 0);
    CHECK(At(r, Tick(223)).buttons == (kZR | kA));
    CHECK(At(r, Tick(233)).buttons == 0);
    CHECK_FALSE(r.Done());
    CHECK(At(r, Tick(243)) == Output{});
    CHECK(r.Done());
}

TEST_CASE("testinput: a late frame skips the steps it missed, in order") {
    Runner r(Must("wait 10\nmark a\nhold A 10\nmark b\nhold B 10\n"));
    std::vector<std::string> log;
    CHECK(At(r, Tick(25), &log).buttons == kB);
    REQUIRE(log.size() == 2);
    CHECK(log[0] == "mark a");
    CHECK(log[1] == "mark b");
}

TEST_CASE("testinput: sticks") {
    Runner r(Must("stick 0.5 -1 60 B\nrstick -1 0 30\nhold LSTICK_UP+LSTICK_LEFT 5\n"));
    const Output a = At(r, 0);
    REQUIRE(a.left);
    CHECK(a.left->x == (kStickMax + 1) / 2);
    CHECK(a.left->y == -kStickMax);
    CHECK(a.buttons == kB);
    CHECK_FALSE(a.right);
    const Output b = At(r, Tick(61));
    REQUIRE(b.right);
    CHECK(b.right->x == -kStickMax);
    CHECK_FALSE(b.left);
    const Output c = At(r, Tick(91));
    REQUIRE(c.left);
    CHECK(*c.left == Stick{-kStickMax, kStickMax});
}

TEST_CASE("testinput: bad lines say where") {
    std::string error;
    CHECK_FALSE(Parse("wait 10\nhold Q 5\n", error));
    CHECK(error == "line 2: unknown key Q");
    CHECK_FALSE(Parse("wait\n", error));
    CHECK(error.rfind("line 1:", 0) == 0);
    CHECK_FALSE(Parse("wait 0\n", error));
    CHECK_FALSE(Parse("wait -5\n", error));
    CHECK_FALSE(Parse("stick 2 0 10\n", error));
    CHECK_FALSE(Parse("stick 0 0 10 A Q\n", error));
    CHECK_FALSE(Parse("jump 10\n", error));
    CHECK(error == "line 1: unknown command jump");
    CHECK_FALSE(Parse("hold A+ 5\n", error));
    CHECK_FALSE(Parse("goto 1\n", error));
    CHECK_FALSE(Parse("goto 1 2 0.1\n", error));
    CHECK_FALSE(Parse("goto x 2\n", error));
    CHECK_FALSE(Parse("toward 1 2\n", error));
    CHECK_FALSE(Parse("  # only a comment\n\n", error));
    CHECK(error == "no steps");
}

TEST_CASE("testinput: windows line ends") {
    Runner r(Must("wait 5\r\npress B\r\n"));
    CHECK(At(r, Tick(6)).buttons == kB);
    CHECK(At(r, Tick(18)) == Output{});
    CHECK(r.Done());
}

namespace {

// A Link that walks where the stick says, through a camera turned by
// `camera` radians and, with `mirrored`, flipped left for right. Like the
// game's, he turns at most 8 radians a second and walks only once he faces
// within 1 radian of the stick.
struct FakeLink {
    double camera = 0.0;
    bool mirrored = false;
    std::array<float, 2> pos{0.0f, 0.0f};
    double facing = 0.0;

    void Step(const Output& out, double dt) {
        if (!out.left || (out.left->x == 0 && out.left->y == 0)) {
            return;
        }
        const double sx = static_cast<double>(out.left->x) / kStickMax;
        const double sy = static_cast<double>(out.left->y) / kStickMax;
        const double mag = std::min(1.0, std::hypot(sx, sy));
        double a = std::atan2(sy, sx) - 3.14159265358979 / 2;  // 0 = stick up
        if (mirrored) {
            a = -a;
        }
        a += camera;  // the world angle the stick asks for
        double turn = std::remainder(a - facing, 2 * 3.14159265358979);
        const double most = 8.0 * dt;
        turn = std::clamp(turn, -most, most);
        facing += turn;
        if (std::fabs(std::remainder(a - facing, 2 * 3.14159265358979)) > 1.0) {
            return;
        }
        const float nx = pos[0] + static_cast<float>(std::cos(facing) * 5.0 * mag * dt);
        const float nz = pos[1] + static_cast<float>(std::sin(facing) * 5.0 * mag * dt);
        if (wall && nx >= 2.0f && nx <= 2.5f && std::fabs(nz) <= 1.5f) {
            return;  // a short wall across the way: x 2 to 2.5, z -1.5 to 1.5
        }
        pos = {nx, nz};
    }
    bool wall = false;
};

// Plays `script` at 30 frames a second for up to `seconds`.
bool Play(Runner& r, FakeLink& link, double seconds, std::vector<std::string>& log) {
    const double dt = 1.0 / 30;
    for (double t = 0; t < seconds; t += dt) {
        r.Advance(t, link.pos, log);
        if (r.Done()) {
            return true;
        }
        link.Step(r.Current(), dt);
    }
    return false;
}

} // namespace

TEST_CASE("testinput: goto reaches the point whatever the camera") {
    for (const double camera : {0.0, 1.0, 2.5, -2.0}) {
        for (const bool mirrored : {false, true}) {
            CAPTURE(camera);
            CAPTURE(mirrored);
            Runner r(Must("goto 8 -6\nmark there\ngoto -3 4 0.5\n"));
            FakeLink link{camera, mirrored};
            std::vector<std::string> log;
            REQUIRE(Play(r, link, 20.0, log));
            REQUIRE(log.size() == 1);
            CHECK(log[0] == "mark there");
            CHECK(std::hypot(link.pos[0] + 3.0f, link.pos[1] - 4.0f) <= 0.5f);
        }
    }
}

TEST_CASE("testinput: a goto that cannot get there gives up and goes on") {
    Runner r(Must("goto 100 0 1 60\nmark after\n"));
    FakeLink link;
    std::vector<std::string> log;
    for (double t = 0; t < 1.5; t += 1.0 / 30) {
        r.Advance(t, link.pos, log);  // Link never moves
    }
    REQUIRE(log.size() == 2);
    CHECK(log[0] == "goto timed out at line 1");
    CHECK(log[1] == "mark after");
    CHECK(r.Done());
}

TEST_CASE("testinput: toward turns Link to the point for its ticks, with buttons") {
    Runner r(Must("goto 0 5\ntoward 5 5 6 Y\n"));
    FakeLink link{0.7, false};
    std::vector<std::string> log;
    const double dt = 1.0 / 30;
    double t = 0;
    for (; t < 10 && !r.Done(); t += dt) {
        r.Advance(t, link.pos, log);
        if (r.Current().buttons == kY) {
            break;
        }
        link.Step(r.Current(), dt);
    }
    REQUIRE(r.Current().buttons == kY);
    // The stick now walks Link along +x in the world, toward the point.
    const auto before = link.pos;
    for (int i = 0; i < 15; ++i) {  // he turns first, then walks
        link.Step(r.Current(), dt);
    }
    const float dx = link.pos[0] - before[0];
    const float dz = link.pos[1] - before[1];
    CHECK(dx > 0.3f);
    CHECK(std::fabs(dz) < 0.3f * dx);
}

TEST_CASE("testinput: goto gets round a short wall in the way") {
    for (const double camera : {0.0, 1.2}) {
        CAPTURE(camera);
        Runner r(Must("goto 5 0 0.6 1200\nmark there\n"));
        FakeLink link{camera, false};
        link.wall = true;
        std::vector<std::string> log;
        REQUIRE(Play(r, link, 25.0, log));
        REQUIRE(log.size() == 1);
        CHECK(log[0] == "mark there");
    }
}

TEST_CASE("testinput: goto with the game's camera, which turns behind Link as he walks") {
    for (const bool mirrored : {false, true}) {
        CAPTURE(mirrored);
        Runner r(Must("goto 8 -6 0.6\nmark there\ngoto -3 4 0.6\n"));
        FakeLink link{0.4, mirrored};
        std::vector<std::string> log;
        const double dt = 1.0 / 30;
        bool done = false;
        for (double t = 0; t < 20.0 && !done; t += dt) {
            const double c = link.camera;
            const double side = mirrored ? 1.0 : -1.0;  // where stick right walks
            Runner::Camera cam{static_cast<float>(std::cos(c)), static_cast<float>(std::sin(c)),
                               static_cast<float>(std::cos(c + side * 1.5707963)),
                               static_cast<float>(std::sin(c + side * 1.5707963))};
            r.Advance(t, link.pos, log, cam);
            done = r.Done();
            link.Step(r.Current(), dt);
            // The camera swings round behind Link at 1.5 radians a second.
            const double behind = std::remainder(link.facing - link.camera, 2 * 3.14159265358979);
            link.camera += std::clamp(behind, -1.5 * dt, 1.5 * dt);
        }
        REQUIRE(done);
        REQUIRE(log.size() == 1);
        CHECK(std::hypot(link.pos[0] + 3.0f, link.pos[1] - 4.0f) <= 0.6f);
    }
}

TEST_CASE("testinput: warp and give are handed to the hook once, in order") {
    std::string error;
    CHECK_FALSE(Parse("warp\n", error));
    CHECK_FALSE(Parse("warp A B C\n", error));
    CHECK_FALSE(Parse("give\n", error));
    Runner r(Must("warp KakarikoVillage_Child Kakariko_WellSide\ngive bombs\nwait 10\n"
                  "warp WaterTemple\nmark after\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].kind == Kind::warp);
    CHECK(requests[0].name == "KakarikoVillage_Child");
    CHECK(requests[0].start == "Kakariko_WellSide");
    CHECK(requests[1].kind == Kind::give);
    CHECK(requests[1].name == "bombs");
    CHECK(r.TakeRequests().empty());
    r.Advance(Tick(11), std::nullopt, log);
    requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].name == "WaterTemple");
    CHECK(requests[0].start.empty());
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "mark after");
}

TEST_CASE("testinput: wear names a gear slot and a piece") {
    std::string error;
    CHECK_FALSE(Parse("wear head\n", error));
    CHECK_FALSE(Parse("wear hat Armor_Child_Mask_Keaton\n", error));
    CHECK(WearFlag("head") == std::optional<std::string>("EquipHeadArmor"));
    CHECK(WearFlag("Boots") == std::optional<std::string>("EquipBoot"));
    CHECK(WearFlag("hand") == std::optional<std::string>("EquipHandArmor"));  // the gauntlets
    CHECK_FALSE(WearFlag("gloves"));
    Runner r(Must("wear head Armor_Child_Mask_Keaton\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].kind == Kind::wear);
    CHECK(requests[0].start == "head");
    CHECK(requests[0].name == "Armor_Child_Mask_Keaton");
}

TEST_CASE("testinput: dress asks the game's equip for a slot, or all of them") {
    std::string error;
    CHECK_FALSE(Parse("dress\n", error));
    CHECK_FALSE(Parse("dress hat\n", error));
    CHECK_FALSE(Parse("dress head extra\n", error));
    CHECK(DressKind("weapon") == std::optional<std::uint32_t>(0));
    CHECK(DressKind("Head") == std::optional<std::uint32_t>(7));
    CHECK(DressKind("armor") == std::optional<std::uint32_t>(8));
    CHECK(DressKind("boots") == std::optional<std::uint32_t>(9));
    CHECK(DressKind("hand") == std::optional<std::uint32_t>(10));
    CHECK(DressKind("shield") == std::optional<std::uint32_t>(6));
    CHECK_FALSE(DressKind("all"));
    Runner r(Must("dress HEAD\ndress all\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].kind == Kind::dress);
    CHECK(requests[0].start == "head");
    CHECK(requests[1].start == "all");
    CHECK_FALSE(DressNext());
    DressAsk(7);
    DressAsk(8);
    CHECK(DressNext() == std::optional<std::uint32_t>(7));
    CHECK(DressNext() == std::optional<std::uint32_t>(8));
    CHECK_FALSE(DressNext());
    // `dress head show` (row 197's mask check): the equip, then the game's head show call; only for the head.
    CHECK_FALSE(Parse("dress shield show
", error));
    CHECK_FALSE(Parse("dress head show now
", error));
    Runner shown(Must("dress head show
dress head
"));
    shown.Advance(0.0, std::nullopt, log);
    const auto asks = shown.TakeRequests();
    REQUIRE(asks.size() == 2);
    CHECK(asks[0].kind == Kind::dress);
    CHECK(asks[0].start == "head");
    CHECK(asks[0].name == "show");
    CHECK(asks[1].name.empty());
    // The show call's ask is no slot kind of the equip call (row 197 test: `dress head` then shows it).
    for (const char* slot : {"weapon", "shield", "head", "armor", "boots", "hand"}) {
        CHECK(DressKind(slot) != std::optional<std::uint32_t>(kDressShowHead));
    }
}

TEST_CASE("testinput: a hold's mark is said halfway, with the keys still down") {
    std::string error;
    CHECK_FALSE(Parse("hold ZR 60 aim extra\n", error));
    Runner r(Must("hold ZR 60 aim\nmark after\n"));
    std::vector<std::string> log;
    CHECK(At(r, Tick(10), &log).buttons == kZR);
    CHECK(log.empty());
    CHECK(At(r, Tick(31), &log).buttons == kZR);  // past halfway: the mark, ZR still held
    CHECK(log == std::vector<std::string>{"mark aim"});
    CHECK(At(r, Tick(40), &log).buttons == kZR);
    CHECK(log.size() == 1);  // once
    At(r, Tick(61), &log);
    CHECK(log == std::vector<std::string>{"mark aim", "mark after"});
    // A frame late past the whole hold still says it, before the next mark.
    Runner late(Must("hold ZR 60 aim\nmark after\n"));
    std::vector<std::string> log2;
    At(late, Tick(100), &log2);
    CHECK(log2 == std::vector<std::string>{"mark aim", "mark after"});
}

TEST_CASE("testinput: assign puts an item on ZR by the button enum's name (held-gear-2)") {
    std::string error;
    CHECK_FALSE(Parse("assign\n", error));
    CHECK_FALSE(Parse("assign bow\n", error));  // the game's spelling only
    CHECK_FALSE(Parse("assign Weapon_Range_Bow\n", error));
    CHECK(AssignableItem("Bow"));
    CHECK(AssignableItem("Hookshot"));
    CHECK(AssignableItem("Hammer"));
    CHECK(AssignableItem("SlingShot"));
    CHECK_FALSE(AssignableItem("Slingshot"));
    Runner r(Must("assign Hookshot\nmark after\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].kind == Kind::assign);
    CHECK(requests[0].name == "Hookshot");
    CHECK(log == std::vector<std::string>{"mark after"});
}

TEST_CASE("testinput: wear SLOT none takes a piece off") {
    Runner r(Must("wear head none\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].start == "head");
    CHECK(requests[0].name == "none");  // the emulator side writes "" for it
}

// Every input script in tools/autotest/scenarios parses (the steps files are the runner's,
// not the emulator's), so a bad line shows here and not first in a game run.
namespace {
// hookshot.py read_script: each `include FILE` line is replaced by FILE's text (FILE beside it, at most 4 deep)
// before the script reaches the module, so the module's parser never sees one.
std::string ReadScenario(const std::filesystem::path& path, int depth = 0) {
    std::ifstream in(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = std::min(text.find('\n', at), text.size());
        const std::string line = text.substr(at, end - at);
        std::istringstream words(line.substr(0, line.find('#')));
        std::string first, file;
        words >> first >> file;
        if (depth < 4 && (first == "include" || first == "INCLUDE") && !file.empty()) {
            out += ReadScenario(path.parent_path() / file, depth + 1);
        } else {
            out += line;
        }
        out += '\n';
        at = end + 1;
    }
    return out;
}
}  // namespace

TEST_CASE("testinput: every autotest scenario script parses") {
    const std::filesystem::path dir =
        std::filesystem::path(COOP_TEST_SOURCE_DIR) / ".." / "tools" / "autotest" / "scenarios";
    REQUIRE(std::filesystem::is_directory(dir));
    int checked = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".txt" || name.find(".steps.") != std::string::npos ||
            name.find(".inc.") != std::string::npos) {
            continue;  // (a shared .inc.txt part is parsed inside the scenarios that include it)
        }
        const std::string text = ReadScenario(entry.path());
        std::string error;
        CHECK_MESSAGE(Parse(text, error).has_value(), name << ": " << error);
        ++checked;
    }
    CHECK(checked > 40);
}

TEST_CASE("testinput: chat sends the rest of the line, spaces kept") {
    std::string error;
    CHECK_FALSE(Parse("chat\n", error));
    Runner r(Must("chat /teleport  Saria the  Kokiri   # a comment\nchat hello\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].kind == Kind::chat);
    CHECK(requests[0].name == "/teleport  Saria the  Kokiri");
    CHECK(requests[1].name == "hello");
}

TEST_CASE("testinput: place PLATFORM UP hands the hook the platform's name and the height over it (#196)") {
    std::string error;
    CHECK_FALSE(Parse("place Obj_Deku_MoveFloor_A_01\n", error));
    CHECK_FALSE(Parse("place Obj_Deku_MoveFloor_A_01 up\n", error));
    Runner r(Must("place Obj_Deku_MoveFloor_A_01 0.3\nmark on\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].kind == Kind::place);
    CHECK(requests[0].name == "Obj_Deku_MoveFloor_A_01");  // the name's case kept
    CHECK(requests[0].at[1] == doctest::Approx(0.3f));
    CHECK(r.Done());
}

TEST_CASE("testinput: place hands the hook a world point, then the script goes on (world-objects)") {
    std::string error;
    CHECK_FALSE(Parse("place 1 2\n", error));
    CHECK_FALSE(Parse("place 1 2 x\n", error));
    CHECK_FALSE(Parse("place 1 2 3 4\n", error));
    Runner r(Must("place -37.2 109.8 13.5\nmark there\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].kind == Kind::place);
    CHECK(requests[0].at[0] == doctest::Approx(-37.2f));
    CHECK(requests[0].at[1] == doctest::Approx(109.8f));
    CHECK(requests[0].at[2] == doctest::Approx(13.5f));
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "mark there");
    CHECK(r.Done());
}

TEST_CASE("testinput: hurt hands the hook a hit on our own Link in quarter hearts, then the script goes on") {
    std::string error;
    CHECK_FALSE(Parse("hurt\n", error));
    CHECK_FALSE(Parse("hurt 0\n", error));
    CHECK_FALSE(Parse("hurt 81\n", error));
    CHECK_FALSE(Parse("hurt 2x\n", error));
    CHECK_FALSE(Parse("hurt 4 5\n", error));
    Runner r(Must("hurt 4\nmark hurt\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].kind == Kind::hurt);
    CHECK(requests[0].name == "4");
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "mark hurt");
    CHECK(r.Done());
}

TEST_CASE("testinput: kill hands the hook an actor name as written, then the script goes on (items-sync)") {
    std::string error;
    CHECK_FALSE(Parse("kill\n", error));
    CHECK_FALSE(Parse("kill Enemy_Skulltula_Gold now\n", error));
    Runner r(Must("kill Enemy_Skulltula_Gold\nmark killed\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].kind == Kind::kill);
    CHECK(requests[0].name == "Enemy_Skulltula_Gold");
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "mark killed");
    CHECK(r.Done());
}

TEST_CASE("testinput: hit and stun hand the hook an actor name (and an amount), then the script goes on") {
    std::string error;
    CHECK_FALSE(Parse("hit Enemy_DungeonBoss_Gohma\n", error));
    CHECK_FALSE(Parse("hit Enemy_DungeonBoss_Gohma 0\n", error));
    CHECK_FALSE(Parse("hit Enemy_DungeonBoss_Gohma 81\n", error));
    CHECK_FALSE(Parse("hit Enemy_DungeonBoss_Gohma 2x\n", error));
    CHECK_FALSE(Parse("stun\n", error));
    CHECK_FALSE(Parse("stun Enemy_DungeonBoss_Gohma now\n", error));
    Runner r(Must("hit Enemy_DungeonBoss_Gohma 2\nstun Enemy_DungeonBoss_Gohma\nmark struck\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].kind == Kind::hit);
    CHECK(requests[0].name == "Enemy_DungeonBoss_Gohma");
    CHECK(requests[0].start == "2");
    CHECK(requests[1].kind == Kind::stun);
    CHECK(requests[1].name == "Enemy_DungeonBoss_Gohma");
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "mark struck");
    CHECK(r.Done());
}

TEST_CASE("testinput: setflag hands the hook a flag name and a whole number (issue 130)") {
    std::string error;
    CHECK_FALSE(Parse("setflag\n", error));
    CHECK_FALSE(Parse("setflag PlayerStatus.CurrentRupee\n", error));
    CHECK_FALSE(Parse("setflag PlayerStatus.CurrentRupee lots\n", error));
    CHECK_FALSE(Parse("setflag PlayerStatus.CurrentRupee 9 9\n", error));
    Runner r(Must("setflag PouchData.Weapon_Shield_Deku.StockNum 0\nsetflag PlayerStatus.CurrentRupee 99\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].kind == Kind::setflag);
    CHECK(requests[0].name == "PouchData.Weapon_Shield_Deku.StockNum");
    CHECK(requests[0].start == "0");
    CHECK(requests[1].name == "PlayerStatus.CurrentRupee");
    CHECK(requests[1].start == "99");
}

TEST_CASE("testinput: setenum hands the hook an Enum flag (name or 0x hash) and a value's name (issue 193)") {
    std::string error;
    CHECK_FALSE(Parse("setenum\n", error));
    CHECK_FALSE(Parse("setenum 0xdb89c1a9\n", error));
    CHECK_FALSE(Parse("setenum 0xdb89c1a9 Step03 more\n", error));
    Runner r(Must("setenum 0xdb89c1a9 Step03\nsetenum PlayerStatus.AssignableItem.Child Bow\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].kind == Kind::setenum);
    CHECK(requests[0].name == "0xdb89c1a9");
    CHECK(requests[0].start == "Step03");
    CHECK(requests[1].kind == Kind::setenum);
    CHECK(requests[1].name == "PlayerStatus.AssignableItem.Child");
    CHECK(requests[1].start == "Bow");
}

TEST_CASE("testinput: drop hands the hook a boss name and its metres (row 178, test only)") {
    std::string error;
    CHECK_FALSE(Parse("drop\n", error));
    CHECK_FALSE(Parse("drop Enemy_DungeonBoss_PhantomGanon\n", error));
    CHECK_FALSE(Parse("drop Enemy_DungeonBoss_PhantomGanon 0\n", error));
    CHECK_FALSE(Parse("drop Enemy_DungeonBoss_PhantomGanon 501\n", error));
    CHECK_FALSE(Parse("drop Enemy_DungeonBoss_PhantomGanon 6x\n", error));
    Runner r(Must("drop Enemy_DungeonBoss_PhantomGanon 60\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].kind == Kind::drop);
    CHECK(requests[0].name == "Enemy_DungeonBoss_PhantomGanon");
    CHECK(requests[0].start == "60");
}

TEST_CASE("testinput: nofairy empties every bottle (five setenum requests), as hookshot.py does") {
    // A bottled fairy makes a lethal hit the game's own death and revive, never down-but-out (3 VOID runs).
    std::string error;
    CHECK_FALSE(Parse("nofairy now\n", error));
    Runner r(Must("nofairy\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 5);
    const char* items[] = {"0x7d48df76", "0x1edd164a", "0xb80c70a8", "0xe4c1b1fc", "0x365e1953"};
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(requests[i].kind == Kind::setenum);
        CHECK(requests[i].name == items[i]);
        CHECK(requests[i].start == "Empty");
    }
}

TEST_CASE("testinput: menu hands the hook the Customize page or the close (issue 74)") {
    std::string error;
    CHECK_FALSE(Parse("menu\n", error));
    CHECK_FALSE(Parse("menu map\n", error));
    CHECK_FALSE(Parse("menu customize now\n", error));
    Runner r(Must("menu Customize\nmenu close\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].kind == Kind::menu);
    CHECK(requests[0].name == "customize");
    CHECK(requests[1].kind == Kind::menu);
    CHECK(requests[1].name == "close");
}

TEST_CASE("test input: sync waits with every button let go until the games meet, then goes on together") {
    std::string error;
    auto script = Parse("press A 6\nsync title 2\npress A 6\n", error);
    REQUIRE(script.has_value());
    // press is a press and a release step; the sync comes after both.
    const auto it = std::find_if((*script).steps.begin(), (*script).steps.end(),
                                 [](const Step& s) { return s.kind == Kind::sync; });
    REQUIRE(it != (*script).steps.end());
    CHECK(it->ticks == 2u);
    CHECK(it->mark == "title");
    Runner r(std::move(*script));
    bool met = false;
    int asked = 0;
    r.SetBarrier([&](const std::string& name, std::uint32_t games) {
        ++asked;
        CHECK(name == "title");
        CHECK(games == 2u);
        return met;
    });
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    r.Advance(1.0, std::nullopt, log);  // the first press is over: waiting at sync
    CHECK(r.Current().buttons == 0u);
    CHECK(asked > 0);
    r.Advance(30.0, std::nullopt, log);  // still waiting, however long
    CHECK(r.Current().buttons == 0u);
    met = true;
    log.clear();
    r.Advance(30.5, std::nullopt, log);
    CHECK(std::find(log.begin(), log.end(), "sync title passed") != log.end());
    CHECK(r.Current().buttons != 0u);  // the press after it, starting now
    std::string bad;
    CHECK_FALSE(Parse("sync title 9\n", bad).has_value());
    CHECK_FALSE(Parse("sync title\n", bad).has_value());
}

TEST_CASE("testinput: face X Z hands the hook the world point Link turns to (row 175)") {
    std::string error;
    CHECK_FALSE(Parse("face\n", error));
    CHECK_FALSE(Parse("face 66.96\n", error));
    CHECK_FALSE(Parse("face 66.96 0.0 1\n", error));
    CHECK_FALSE(Parse("face east 0.0\n", error));
    Runner r(Must("face 66.96 -0.5\nmark faced\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].kind == Kind::face);
    CHECK(requests[0].at[0] == doctest::Approx(66.96f));
    CHECK(requests[0].at[2] == doctest::Approx(-0.5f));
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "mark faced");
    CHECK(r.Done());
}

TEST_CASE("testinput: aim X Y Z hands the hook the world point our next arrow is put on its way into (row 173)") {
    std::string error;
    CHECK_FALSE(Parse("aim\n", error));
    CHECK_FALSE(Parse("aim -56.5 131.5\n", error));
    CHECK_FALSE(Parse("aim -56.5 131.5 -10.1 1\n", error));
    CHECK_FALSE(Parse("aim eye 131.5 -10.1\n", error));
    Runner r(Must("aim -56.5 131.5 -10.1\nmark aimed\n"));
    std::vector<std::string> log;
    r.Advance(0.0, std::nullopt, log);
    const auto requests = r.TakeRequests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].kind == Kind::aim);
    CHECK(requests[0].at[0] == doctest::Approx(-56.5f));
    CHECK(requests[0].at[1] == doctest::Approx(131.5f));
    CHECK(requests[0].at[2] == doctest::Approx(-10.1f));
    REQUIRE(log.size() == 1);
    CHECK(log[0] == "mark aimed");
}

TEST_CASE("testinput: AimPlace puts the arrow short of the target on its line, facing it (row 173)") {
    // Row 173's relief: the arrow let go 2 m in front of Link (z -4.0 at bow height), the eye 6.1 m on and 5.3 m up.
    const std::array<float, 3> from{-56.5f, 126.2f, -4.0f};
    const std::array<float, 3> eye{-56.5f, 131.5f, -10.1f};
    const auto t = AimPlace(from, eye, 0.6f);
    REQUIRE(t.has_value());
    const float d[3] = {eye[0] - from[0], eye[1] - from[1], eye[2] - from[2]};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; ++i) {
        const float f = d[i] / len;
        CHECK((*t)[i] == doctest::Approx(eye[i] - f * 0.6f).epsilon(1e-4));  // 0.6 m short, on the line
        CHECK((*t)[3 + i * 3 + 2] == doctest::Approx(f).epsilon(1e-4));      // column 2: the way to the eye
    }
    // The 3x3 is a rotation: columns of length 1, each square to the others; its up (column 1) is up-ish.
    for (int c = 0; c < 3; ++c) {
        float n = 0.0f;
        for (int r = 0; r < 3; ++r) n += (*t)[3 + r * 3 + c] * (*t)[3 + r * 3 + c];
        CHECK(n == doctest::Approx(1.0f).epsilon(1e-4));
        for (int o = c + 1; o < 3; ++o) {
            float dot = 0.0f;
            for (int r = 0; r < 3; ++r) dot += (*t)[3 + r * 3 + c] * (*t)[3 + r * 3 + o];
            CHECK(dot == doctest::Approx(0.0f).epsilon(1e-4));
        }
    }
    CHECK((*t)[3 + 1 * 3 + 1] > 0.5f);
    // A flat aim is face's turn: facing -z is the yaw of 180 degrees (face writes {c, 0, s, 0, 1, 0, -s, 0, c}).
    const auto flat = AimPlace({0.0f, 1.0f, 0.0f}, {0.0f, 1.0f, -8.0f}, 0.6f);
    REQUIRE(flat.has_value());
    const float want[12] = {0.0f, 1.0f, -7.4f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f};
    for (int i = 0; i < 12; ++i) CHECK((*flat)[i] == doctest::Approx(want[i]).epsilon(1e-4));
    // Nearer than the short way: put where it is. Straight up: still a rotation. The same point or NaN: nothing.
    const auto near = AimPlace({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -0.3f}, 0.6f);
    REQUIRE(near.has_value());
    CHECK((*near)[2] == doctest::Approx(0.0f));
    const auto up = AimPlace({0.0f, 0.0f, 0.0f}, {0.0f, 5.0f, 0.0f}, 0.6f);
    REQUIRE(up.has_value());
    CHECK((*up)[1] == doctest::Approx(4.4f));
    CHECK((*up)[3 + 1 * 3 + 2] == doctest::Approx(1.0f));
    CHECK_FALSE(AimPlace(eye, eye, 0.6f).has_value());
    CHECK_FALSE(AimPlace({std::nanf(""), 0.0f, 0.0f}, eye, 0.6f).has_value());
}
