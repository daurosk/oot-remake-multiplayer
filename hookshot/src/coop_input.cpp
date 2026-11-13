// SPDX-License-Identifier: GPL-3.0-or-later
// This game's controller and keyboard (coop_input.h).
#include "coop_input.h"

#include <cctype>
#include <mutex>

namespace CoopInput {
namespace {

std::string Lower(std::string_view v) {
    std::size_t a = 0, b = v.size();
    while (a < b && std::isspace(static_cast<unsigned char>(v[a]))) {
        ++a;
    }
    while (b > a && std::isspace(static_cast<unsigned char>(v[b - 1]))) {
        --b;
    }
    std::string out(v.substr(a, b - a));
    for (auto& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::mutex g_mutex;
Exports g_exports;
Setting g_setting;

}  // namespace

std::optional<int> ParsePad(std::string_view value) {
    const std::string v = Lower(value);
    if (v.empty() || v == "auto") {
        return kAuto;
    }
    if (v == "none") {
        return kNone;
    }
    if (v.size() == 1 && v[0] >= '1' && v[0] <= '0' + kMaxPad) {
        return v[0] - '0';
    }
    return std::nullopt;
}

std::optional<Keyboard> ParseKeyboard(std::string_view value) {
    const std::string v = Lower(value);
    if (v.empty() || v == "focus") {
        return Keyboard::Focus;
    }
    if (v == "always") {
        return Keyboard::Always;
    }
    if (v == "off") {
        return Keyboard::Off;
    }
    return std::nullopt;
}

Setting FromValues(std::string_view pad, std::string_view keyboard, std::vector<std::string>* warnings) {
    Setting s;
    if (const auto p = ParsePad(pad)) {
        s.pad = *p;
    } else if (warnings) {
        warnings->push_back("LICHII_COOP_PAD = " + std::string(pad) + " is not auto, none or 1 to " +
                            std::to_string(kMaxPad) + ": every controller counts");
    }
    if (const auto k = ParseKeyboard(keyboard)) {
        s.keyboard = *k;
    } else if (warnings) {
        warnings->push_back("LICHII_COOP_KEYBOARD = " + std::string(keyboard) +
                            " is not focus, always or off: the keyboard counts with the window in front");
    }
    return s;
}

std::string PadValue(int pad) {
    if (pad == kNone) {
        return "none";
    }
    if (pad >= 1 && pad <= kMaxPad) {
        return std::to_string(pad);
    }
    return "auto";
}

std::string KeyboardValue(Keyboard keyboard) {
    switch (keyboard) {
    case Keyboard::Always:
        return "always";
    case Keyboard::Off:
        return "off";
    case Keyboard::Focus:
        break;
    }
    return "focus";
}

int NextPad(int pad, int connected) {
    if (connected > kMaxPad) {
        connected = kMaxPad;
    }
    if (pad == kNone) {
        return kAuto;
    }
    const int next = pad <= kAuto ? 1 : pad + 1;
    return next <= connected ? next : kNone;
}

Keyboard NextKeyboard(Keyboard keyboard) {
    switch (keyboard) {
    case Keyboard::Focus:
        return Keyboard::Always;
    case Keyboard::Always:
        return Keyboard::Off;
    case Keyboard::Off:
        break;
    }
    return Keyboard::Focus;
}

std::string ControllerLabel(int pad, std::string_view name) {
    if (pad == kNone) {
        return "Controller: none (keyboard + mouse)";
    }
    if (pad < 1 || pad > kMaxPad) {
        return "Controller: all";
    }
    const std::string n = std::to_string(pad);
    return name.empty() ? "Controller: " + n + " (not connected)" : "Controller: " + n + " - " + std::string(name);
}

std::string KeyboardLabel(Keyboard keyboard) {
    switch (keyboard) {
    case Keyboard::Always:
        return "Keyboard: also in background";
    case Keyboard::Off:
        return "Keyboard + mouse: off";
    case Keyboard::Focus:
        break;
    }
    return "Keyboard + mouse: window in front";
}

void Bind(const Exports& exports) {
    std::lock_guard lock(g_mutex);
    g_exports = exports.select && exports.pad_name ? exports : Exports{};
}

bool Available() {
    std::lock_guard lock(g_mutex);
    return g_exports.select != nullptr;
}

void Apply(const Setting& setting) {
    std::lock_guard lock(g_mutex);
    g_setting = setting;
    if (g_exports.select) {
        g_exports.select(setting.pad, static_cast<int>(setting.keyboard));
    }
}

Setting Current() {
    std::lock_guard lock(g_mutex);
    return g_setting;
}

std::string PadName(int n) {
    std::lock_guard lock(g_mutex);
    char buf[128] = {};
    if (!g_exports.pad_name || n < 1 || n > kMaxPad || !g_exports.pad_name(n, buf, sizeof buf)) {
        return {};
    }
    buf[sizeof buf - 1] = 0;
    return buf;
}

int Connected() {
    for (int n = kMaxPad; n >= 1; --n) {
        if (!PadName(n).empty()) {
            return n;
        }
    }
    return 0;
}

std::string ControllerRow() {
    const int pad = Current().pad;
    return ControllerLabel(pad, pad >= 1 ? PadName(pad) : std::string());
}

std::string KeyboardRow() {
    return KeyboardLabel(Current().keyboard);
}

Setting CycleController() {
    Setting s = Current();
    s.pad = NextPad(s.pad, Connected());
    Apply(s);
    return s;
}

Setting CycleKeyboard() {
    Setting s = Current();
    s.keyboard = NextKeyboard(s.keyboard);
    Apply(s);
    return s;
}

}  // namespace CoopInput
