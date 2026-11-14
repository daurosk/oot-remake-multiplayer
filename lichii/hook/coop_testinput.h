// Scripted controller input for automatic tests (LICHII_COOP_TEST_SCRIPT).
//
// LICHII_COOP_TEST_SCRIPT=<file> names a text file of input steps. The hook
// looks at the file about twice a second; each time it changes (and is not
// empty) it is read again and played from that moment into player 1's
// emulated pad (or the handheld pad when player 1 is not connected). The
// script's input is added to whatever the real keyboard or pad does. A test
// runner can so write one script to load the save, wait, then write the next.
//
// Time is counted in ticks of 1/60 s of real time, not in emulated frames:
// Ocarina of Time moves by elapsed time, so a step lasts as long whatever
// the frame rate. One step a line; `#` starts a comment:
//
//   wait 120               nothing held for 120 ticks (2 s)
//   hold A 30              hold A for 30 ticks
//   hold LSTICK_UP+B 90    left stick fully up and B together (run)
//   press A                hold A 6 ticks, then let go 6 ticks
//   press ZR+A 10          hold 10 ticks, then let go 10 ticks
//   stick 0.5 1.0 60       left stick at x 0.5, y 1.0 (each -1..1) for 60
//   stick 0 1 60 B         the same with buttons held too
//   rstick -1 0 30         right stick (the camera) the same way
//   goto -7.5 71.1         walk Link to world x -7.5, z 71.1 (within 1 m),
//   goto -7.5 71.1 1.5 600 ... within 1.5 m, giving up after 600 ticks
//   toward -7.5 71.1 8     push the stick toward that point for 8 ticks
//                          (Link turns to face it)
//   warp KakarikoVillage_Child Kakariko_WellSide
//   chat /teleport Saria   (the rest of the line, sent as the chat box sends it)
//                          test only: changes scene the way the game's own
//                          sequence does (Sequence_ReservedNextBanc), to the
//                          start position given (StartPos names in the
//                          scene's Banc) or the scene's own. The game takes
//                          it only during an event, such as the one after a
//                          save loads: put the warp before the presses that
//                          load the save (the hook asks again every 0.5 s
//                          for up to 90 s, until Link is in that scene)
//   place -37.2 109.8 13.5 test only: puts Link at world x, y (height), z
//                          in this scene (the boss pull's free-play move,
//                          CoopBoss::MoveLink; needs LICHII_COOP_BOSS on, the
//                          default), so a scenario reaches a room no walk
//                          can; logs "coop: test place ...: done" or "failed"
//   kill Enemy_Skulltula_Gold test only: the nearest actor of that name to
//                          Link takes a hit from Link through the game's own
//                          damage (as a real kill, so it drops what it
//                          drops); needs LICHII_COOP_OBJECTS on; logs
//                          "coop: test kill NAME ...: hit" or "none here"
//   give bombs             test only: gives an item through coop_cheats
//                          (needs LICHII_COOP_CHEATS=1): bombs, bow, hookshot,
//                          or any item flag name coop_cheats knows
//   mark walk-start        writes "coop: test mark walk-start" to the log
//
// goto and toward steer from Link's position, read each frame. The stick is
// relative to the camera, which turns as Link walks: the hook gives the
// stepper the camera's ground directions from the game's camera matrix each
// frame (without them it learns them from how Link moves). A push that gets
// nowhere for a second steps aside. A goto that gives up logs
// "coop: test goto timed out" and the script goes on.
//
// Buttons: A B X Y L R ZL ZR PLUS MINUS LCLICK RCLICK, the d-pad DUP DDOWN
// DLEFT DRIGHT, and full stick directions LSTICK_UP LSTICK_DOWN LSTICK_LEFT
// LSTICK_RIGHT RSTICK_UP RSTICK_DOWN RSTICK_LEFT RSTICK_RIGHT; join with `+`.
// When the last step ends the hook writes "coop: test script done".
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Core {
class System;
}

namespace CoopTestInput {

constexpr int kTicksPerSecond = 60;
constexpr std::int32_t kStickMax = 0x7fff;  // HID_JOYSTICK_MAX

// The emulator's NpadButton bits (hid_core/hid_types.h).
enum Button : std::uint64_t {
    kA = 1u << 0,
    kB = 1u << 1,
    kX = 1u << 2,
    kY = 1u << 3,
    kLClick = 1u << 4,
    kRClick = 1u << 5,
    kL = 1u << 6,
    kR = 1u << 7,
    kZL = 1u << 8,
    kZR = 1u << 9,
    kPlus = 1u << 10,
    kMinus = 1u << 11,
    kDLeft = 1u << 12,
    kDUp = 1u << 13,
    kDRight = 1u << 14,
    kDDown = 1u << 15,
};

struct Stick {
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool operator==(const Stick&) const = default;
};

enum class Kind {
    timed,   // wait, hold, press, stick, rstick
    mark,
    go,      // goto
    toward,
    warp,    // test only: scene change
    give,    // test only: an item
    wear,    // test only: a PlayerStatus.Equip* flag (what the puppets in other games wear)
    assign,  // test only: the item on ZR (PlayerStatus.AssignableItem.Child and .Adult)
    chat,    // test only: a chat line or command, as typed in the chat box (/teleport B)
    place,   // test only: Link put at x, y, z in this scene
    kill,    // test only: the nearest actor of that name hit by our Link for all its life
    setflag, // test only: an Int or Bool flag of this game set to a value (issue 130: shop-buy)
    setenum, // test only: an Enum flag (name or 0x hash) set to a value's name (issue 193: quests)
    sync,    // sync NAME N: wait here, buttons let go, until N games reached NAME
};

struct Step {
    Kind kind = Kind::timed;
    std::uint32_t ticks = 0;  // timed and toward; goto: the give-up time; sync: N
    std::uint64_t buttons = 0;
    std::optional<Stick> left;   // unset: the real stick
    std::optional<Stick> right;
    std::string mark;  // mark: its name; warp: the scene; give: the item; wear: the piece;
                       // assign: the item's name in the button enum (Bow, Hookshot, Hammer)
    std::string start; // warp: the start position name (empty: the scene's own); wear: the slot
    float x = 0.0f;  // goto and toward: the world point (x, z); place: (x, y, z)
    float y = 0.0f;
    float z = 0.0f;
    float radius = 1.0f;
    int line = 0;  // the script line it came from
};

struct Script {
    std::vector<Step> steps;
};

// Reads a script. On a bad line returns nothing and sets `error` to
// "line N: why".
std::optional<Script> Parse(std::string_view text, std::string& error);

// wear SLOT: the PlayerStatus flag name for weapon, shield, head, armor or
// boots (any case), e.g. "EquipHeadArmor"; nothing for another word.
std::optional<std::string> WearFlag(std::string_view slot);

// assign ITEM: true for a value of the item button enum
// (PlayerStatus.AssignableItem.Child / .Adult in GameDataList: Empty,
// DekuStick, Bomb, SlingShot, Hookshot, Longshot, Hammer, Bow, ...), as the
// game spells it.
bool AssignableItem(std::string_view name);

// What the pad gets now.
struct Output {
    std::uint64_t buttons = 0;
    std::optional<Stick> left;
    std::optional<Stick> right;
    bool operator==(const Output&) const = default;
};

// Plays a script step by step. No emulator code: the hook calls Advance once
// a frame with the time since the start and Link's position (x, z) when it
// can read it, and gives Current() to the pad.
class Runner {
public:
    explicit Runner(Script script);

    // sync NAME N: says we reached NAME and whether N games have (the hook
    // keeps one file per game in a folder the test runner shares). Without
    // one, a sync step passes at once.
    using Barrier = std::function<bool(const std::string& name, std::uint32_t games)>;
    void SetBarrier(Barrier barrier) { barrier_ = std::move(barrier); }

    // The camera on the ground: unit vectors (x, z) of where stick up and
    // stick right walk Link. From the game's camera matrix when the hook has
    // it; without it the stepper learns them from how Link moves.
    struct Camera {
        float forward_x = 0.0f;
        float forward_z = 0.0f;
        float right_x = 0.0f;
        float right_z = 0.0f;
    };

    // Moves on to the step for `now_s`. Appends to `log` the marks reached
    // and "goto timed out" lines, in order.
    void Advance(double now_s, std::optional<std::array<float, 2>> player,
                 std::vector<std::string>& log, std::optional<Camera> camera = std::nullopt);
    const Output& Current() const { return out_; }
    // The warp and give steps reached since the last call, for the hook.
    struct Request {
        Kind kind = Kind::warp;
        std::string name;
        std::string start;
        std::array<float, 3> at{};  // place
    };
    std::vector<Request> TakeRequests() { return std::exchange(requests_, {}); }
    bool Done() const { return index_ >= script_.steps.size(); }
    std::size_t StepCount() const { return script_.steps.size(); }
    // Steering now (goto or toward), for the log.
    bool Steering() const;
    std::optional<double> Heading() const { return heading_; }
    double Mirror() const { return mirror_; }

    // Link turns before he walks: movement is learnt from only after the
    // stick has held one direction this long, and a steering stick is
    // re-aimed at most this often, so such windows happen.
    static constexpr double kSettleS = 0.3;
    static constexpr double kAimEveryS = 0.5;
    // No 0.3 m of progress in kStuckS: push a quarter turn aside for kDetourS.
    static constexpr double kStuckS = 1.0;
    static constexpr double kDetourS = 0.7;

private:
    void Learn(double now_s, std::optional<std::array<float, 2>> player);
    void Steer(const Step& step, double now_s, std::optional<std::array<float, 2>> player);
    std::optional<Camera> camera_;  // this frame's, when given
    std::vector<Request> requests_;
    Barrier barrier_;
    void Hold(std::optional<double> stick_angle, double now_s);

    Script script_;
    std::size_t index_ = 0;
    std::size_t marked_index_ = static_cast<std::size_t>(-1);  // a hold whose mark was said
    double step_start_ = 0.0;  // the script starts at time 0
    Output out_;
    // The camera model: stick up moves Link along world angle `heading_`
    // (radians, atan2 of z and x); `mirror_` is -1 when stick right turns the
    // other way round. Learnt from Link's movement while the stick is held.
    std::optional<double> heading_;
    double mirror_ = 1.0;
    bool mirror_known_ = false;
    std::optional<double> stick_angle_;  // the left stick's direction now (none: let go)
    double stick_since_ = 0.0;           // when it took that direction
    std::optional<std::array<float, 2>> window_pos_;  // where a steady window began
    std::optional<std::array<float, 2>> prev_player_;  // last frame's position
    double aimed_at_ = -1.0;             // when Steer last chose a direction
    std::optional<std::array<float, 2>> progress_pos_;  // stuck check
    double progress_at_ = 0.0;
    double detour_until_ = -1.0;
    double detour_side_ = -1.0;
};

// Called once per emulated frame by the hook. Does nothing unless
// LICHII_COOP_TEST_SCRIPT is set. In the Hookshot module (COOP_HOOKSHOT) it
// reads LICHII_COOP_TEST_COMMANDS instead: the autotest driver
// (tools/autotest/hookshot.py) holds the pad through Hookshot's control bridge
// and writes only the test-only steps there; they are played without a pad
// and the file is emptied once read.
void OnFrame(Core::System& system);

} // namespace CoopTestInput
