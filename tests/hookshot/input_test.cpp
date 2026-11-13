// CoopInput (hookshot/src/coop_input.h): this game's controller and keyboard
// for two games on one PC. The exe's exports (Hookshot patch 0015) are faked.

#include <doctest/doctest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "coop_input.h"

using CoopInput::Keyboard;
using CoopInput::kAuto;
using CoopInput::kNone;

TEST_CASE("CoopInput: the switches' values") {
    CHECK(CoopInput::ParsePad("") == kAuto);
    CHECK(CoopInput::ParsePad(" Auto ") == kAuto);
    CHECK(CoopInput::ParsePad("NONE") == kNone);
    CHECK(CoopInput::ParsePad("1") == 1);
    CHECK(CoopInput::ParsePad(" 8") == 8);
    CHECK_FALSE(CoopInput::ParsePad("0").has_value());
    CHECK_FALSE(CoopInput::ParsePad("9").has_value());
    CHECK_FALSE(CoopInput::ParsePad("12").has_value());
    CHECK_FALSE(CoopInput::ParsePad("-1").has_value());
    CHECK_FALSE(CoopInput::ParsePad("pad1").has_value());
    CHECK(CoopInput::ParseKeyboard("") == Keyboard::Focus);
    CHECK(CoopInput::ParseKeyboard("focus") == Keyboard::Focus);
    CHECK(CoopInput::ParseKeyboard("Always") == Keyboard::Always);
    CHECK(CoopInput::ParseKeyboard("off ") == Keyboard::Off);
    CHECK_FALSE(CoopInput::ParseKeyboard("on").has_value());
    // Round trip: what the row saves reads back the same.
    for (int p : {kAuto, kNone, 1, 2, 8}) {
        CHECK(CoopInput::ParsePad(CoopInput::PadValue(p)) == p);
    }
    for (Keyboard k : {Keyboard::Focus, Keyboard::Always, Keyboard::Off}) {
        CHECK(CoopInput::ParseKeyboard(CoopInput::KeyboardValue(k)) == k);
    }
    CHECK(CoopInput::PadValue(42) == "auto");
}

TEST_CASE("CoopInput: a bad value is the default, with a warning") {
    std::vector<std::string> w;
    auto s = CoopInput::FromValues("2", "off", &w);
    CHECK(s == CoopInput::Setting{2, Keyboard::Off});
    CHECK(w.empty());
    s = CoopInput::FromValues("pad two", "sometimes", &w);
    CHECK(s == CoopInput::Setting{});
    REQUIRE(w.size() == 2);
    CHECK(w[0].find("LICHII_COOP_PAD") != std::string::npos);
    CHECK(w[1].find("LICHII_COOP_KEYBOARD") != std::string::npos);
    CHECK(CoopInput::FromValues("", "", nullptr) == CoopInput::Setting{});
}

TEST_CASE("CoopInput: the row's order") {
    // auto -> 1 -> 2 -> none -> auto with two pads
    CHECK(CoopInput::NextPad(kAuto, 2) == 1);
    CHECK(CoopInput::NextPad(1, 2) == 2);
    CHECK(CoopInput::NextPad(2, 2) == kNone);
    CHECK(CoopInput::NextPad(kNone, 2) == kAuto);
    // no pads: auto -> none -> auto
    CHECK(CoopInput::NextPad(kAuto, 0) == kNone);
    // a pad that was unplugged goes on to none
    CHECK(CoopInput::NextPad(3, 1) == kNone);
    CHECK(CoopInput::NextPad(8, 20) == kNone);
    CHECK(CoopInput::NextKeyboard(Keyboard::Focus) == Keyboard::Always);
    CHECK(CoopInput::NextKeyboard(Keyboard::Always) == Keyboard::Off);
    CHECK(CoopInput::NextKeyboard(Keyboard::Off) == Keyboard::Focus);
}

TEST_CASE("CoopInput: the labels") {
    CHECK(CoopInput::ControllerLabel(kAuto, "") == "Controller: all");
    CHECK(CoopInput::ControllerLabel(kNone, "x") == "Controller: none (keyboard + mouse)");
    CHECK(CoopInput::ControllerLabel(2, "Xbox Wireless Controller") == "Controller: 2 - Xbox Wireless Controller");
    CHECK(CoopInput::ControllerLabel(2, "") == "Controller: 2 (not connected)");
    CHECK(CoopInput::KeyboardLabel(Keyboard::Focus) == "Keyboard + mouse: window in front");
    CHECK(CoopInput::KeyboardLabel(Keyboard::Off) == "Keyboard + mouse: off");
}

namespace {
int g_pad = 99, g_keyboard = 99, g_selects = 0;
std::vector<std::string> g_names;
void FakeSelect(int pad, int keyboard) {
    g_pad = pad, g_keyboard = keyboard, ++g_selects;
}
int FakeName(int n, char* name, std::size_t cap) {
    const bool ok = n >= 1 && n <= static_cast<int>(g_names.size()) && !g_names[n - 1].empty();
    std::snprintf(name, cap, "%s", ok ? g_names[n - 1].c_str() : "");
    return ok ? 1 : 0;
}
}  // namespace

TEST_CASE("CoopInput: without patch 0015 the setting is only remembered") {
    CoopInput::Bind({});
    CHECK_FALSE(CoopInput::Available());
    CoopInput::Apply({1, Keyboard::Off});
    CHECK(CoopInput::Current() == CoopInput::Setting{1, Keyboard::Off});
    CHECK(CoopInput::PadName(1).empty());
    CHECK(CoopInput::Connected() == 0);
    CHECK(CoopInput::ControllerRow() == "Controller: 1 (not connected)");
    CoopInput::Bind({FakeSelect, nullptr});  // half a pair is none
    CHECK_FALSE(CoopInput::Available());
    CoopInput::Apply({});
}

TEST_CASE("CoopInput: the exe's exports pick and name the pads") {
    g_names = {"Xbox Wireless Controller", "DualSense Wireless Controller"};
    g_selects = 0;
    CoopInput::Bind({FakeSelect, FakeName});
    REQUIRE(CoopInput::Available());
    CoopInput::Apply({});
    CHECK(g_pad == kAuto);
    CHECK(g_keyboard == 0);
    CHECK(CoopInput::Connected() == 2);
    CHECK(CoopInput::PadName(2) == "DualSense Wireless Controller");
    CHECK(CoopInput::PadName(3).empty());
    CHECK(CoopInput::PadName(0).empty());
    CHECK(CoopInput::ControllerRow() == "Controller: all");

    // The row: each press applies at once.
    auto s = CoopInput::CycleController();
    CHECK(s.pad == 1);
    CHECK(g_pad == 1);
    CHECK(CoopInput::ControllerRow() == "Controller: 1 - Xbox Wireless Controller");
    CoopInput::CycleController();
    CHECK(g_pad == 2);
    CoopInput::CycleController();
    CHECK(g_pad == kNone);
    CHECK(CoopInput::ControllerRow() == "Controller: none (keyboard + mouse)");
    CoopInput::CycleController();
    CHECK(g_pad == kAuto);

    s = CoopInput::CycleKeyboard();
    CHECK(s.keyboard == Keyboard::Always);
    CHECK(g_keyboard == 1);
    CoopInput::CycleKeyboard();
    CHECK(g_keyboard == 2);
    CHECK(CoopInput::KeyboardRow() == "Keyboard + mouse: off");
    CHECK(g_selects == 7);  // the first Apply, four controller presses, two keyboard ones

    // The chosen pad unplugged: the row says so, and the next press goes on to none.
    CoopInput::Apply({2, Keyboard::Focus});
    g_names.resize(1);
    CHECK(CoopInput::ControllerRow() == "Controller: 2 (not connected)");
    CHECK(CoopInput::CycleController().pad == kNone);

    CoopInput::Bind({});
    CoopInput::Apply({});
}
