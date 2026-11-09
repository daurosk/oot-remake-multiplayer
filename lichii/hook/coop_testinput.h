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
//   mark walk-start        writes "coop: test mark walk-start" to the log
//
// Buttons: A B X Y L R ZL ZR PLUS MINUS LCLICK RCLICK, the d-pad DUP DDOWN
// DLEFT DRIGHT, and full stick directions LSTICK_UP LSTICK_DOWN LSTICK_LEFT
// LSTICK_RIGHT RSTICK_UP RSTICK_DOWN RSTICK_LEFT RSTICK_RIGHT; join with `+`.
// When the last step ends the hook writes "coop: test script done".
#pragma once

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
};

struct Step {
    std::uint32_t ticks = 0;  // 0 only for a mark
    std::uint64_t buttons = 0;
    std::optional<Stick> left;   // unset: the real stick
    std::optional<Stick> right;
    std::string mark;  // set: a mark, written to the log when reached
    int line = 0;      // the script line it came from
};

struct Script {
    std::vector<Step> steps;
    std::uint64_t total_ticks = 0;
};

// Reads a script. On a bad line returns nothing and sets `error` to
// "line N: why".
std::optional<Script> Parse(std::string_view text, std::string& error);

// The step (not a mark) that holds at `tick` from the start, or nullptr
// once the script is over.
const Step* At(const Script& script, std::uint64_t tick);

// The marks whose time is in (`from`, `to`], in order. `from` is -1 at the
// start so marks at tick 0 count.
std::vector<const Step*> MarksBetween(const Script& script, std::int64_t from, std::int64_t to);

// Called once per emulated frame by the hook. Does nothing unless
// LICHII_COOP_TEST_SCRIPT is set. Only in the emulator build.
void OnFrame(Core::System& system);

} // namespace CoopTestInput
