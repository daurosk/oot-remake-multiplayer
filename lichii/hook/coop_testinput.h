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
#include <optional>
#include <string>
#include <string_view>
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
};

struct Step {
    Kind kind = Kind::timed;
    std::uint32_t ticks = 0;  // timed and toward; goto: the give-up time
    std::uint64_t buttons = 0;
    std::optional<Stick> left;   // unset: the real stick
    std::optional<Stick> right;
    std::string mark;
    float x = 0.0f;  // goto and toward: the world point (x, z)
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
    void Hold(std::optional<double> stick_angle, double now_s);

    Script script_;
    std::size_t index_ = 0;
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
// LICHII_COOP_TEST_SCRIPT is set. Only in the emulator build.
void OnFrame(Core::System& system);

} // namespace CoopTestInput
