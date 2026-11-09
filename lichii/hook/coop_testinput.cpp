// Scripted controller input for automatic tests; see coop_testinput.h.
// The parser has no emulator code (the tests build it alone); the part that
// drives the emulated pad is only built with LICHII_COOP.
#include "coop_testinput.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace CoopTestInput {

namespace {

constexpr std::uint32_t kPressTicks = 6;
constexpr std::uint32_t kMaxStepTicks = 60 * 60 * 10;  // 10 minutes

std::vector<std::string_view> Words(std::string_view line) {
    std::vector<std::string_view> words;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        const std::size_t start = i;
        while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        if (i > start) {
            words.push_back(line.substr(start, i - start));
        }
    }
    return words;
}

std::string Upper(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

bool Ticks(std::string_view word, std::uint32_t& out) {
    if (word.empty() || word.size() > 9) {
        return false;
    }
    std::uint32_t value = 0;
    for (const char c : word) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (value == 0 || value > kMaxStepTicks) {
        return false;
    }
    out = value;
    return true;
}

bool Axis(std::string_view word, std::int32_t& out) {
    const std::string s(word);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size() || !std::isfinite(v) || v < -1.0 || v > 1.0) {
        return false;
    }
    out = static_cast<std::int32_t>(std::lround(v * kStickMax));
    return true;
}

// Adds `word` ("A+LSTICK_UP") to the step. False on an unknown name.
bool Keys(std::string_view word, Step& step, std::string& bad) {
    struct Name {
        const char* name;
        std::uint64_t button;
    };
    static constexpr Name kNames[] = {
        {"A", kA},          {"B", kB},           {"X", kX},           {"Y", kY},
        {"L", kL},          {"R", kR},           {"ZL", kZL},         {"ZR", kZR},
        {"PLUS", kPlus},    {"MINUS", kMinus},   {"LCLICK", kLClick}, {"RCLICK", kRClick},
        {"DUP", kDUp},      {"DDOWN", kDDown},   {"DLEFT", kDLeft},   {"DRIGHT", kDRight},
    };
    std::size_t start = 0;
    while (start <= word.size()) {
        std::size_t end = word.find('+', start);
        if (end == std::string_view::npos) {
            end = word.size();
        }
        const std::string key = Upper(word.substr(start, end - start));
        start = end + 1;
        bool found = false;
        for (const Name& n : kNames) {
            if (key == n.name) {
                step.buttons |= n.button;
                found = true;
                break;
            }
        }
        if (!found && (key.rfind("LSTICK_", 0) == 0 || key.rfind("RSTICK_", 0) == 0)) {
            auto& stick = key[0] == 'L' ? step.left : step.right;
            if (!stick) {
                stick = Stick{};
            }
            const std::string dir = key.substr(7);
            found = true;
            if (dir == "UP") {
                stick->y = kStickMax;
            } else if (dir == "DOWN") {
                stick->y = -kStickMax;
            } else if (dir == "LEFT") {
                stick->x = -kStickMax;
            } else if (dir == "RIGHT") {
                stick->x = kStickMax;
            } else {
                found = false;
            }
        }
        if (!found) {
            bad = key;
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<Script> Parse(std::string_view text, std::string& error) {
    Script script;
    int number = 0;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        std::size_t end = text.find('\n', pos);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(pos, end - pos);
        pos = end + 1;
        ++number;
        if (const auto hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        const auto w = Words(line);
        if (w.empty()) {
            continue;
        }
        const std::string cmd = Upper(w[0]);
        const auto fail = [&](const std::string& why) {
            error = "line " + std::to_string(number) + ": " + why;
            return std::nullopt;
        };
        Step step;
        step.line = number;
        std::string bad;
        if (cmd == "WAIT") {
            if (w.size() != 2 || !Ticks(w[1], step.ticks)) {
                return fail("want: wait TICKS");
            }
        } else if (cmd == "HOLD") {
            if (w.size() != 3 || !Ticks(w[2], step.ticks)) {
                return fail("want: hold KEYS TICKS");
            }
            if (!Keys(w[1], step, bad)) {
                return fail("unknown key " + bad);
            }
        } else if (cmd == "PRESS") {
            std::uint32_t ticks = kPressTicks;
            if (w.size() < 2 || w.size() > 3 || (w.size() == 3 && !Ticks(w[2], ticks))) {
                return fail("want: press KEYS [TICKS]");
            }
            if (!Keys(w[1], step, bad)) {
                return fail("unknown key " + bad);
            }
            step.ticks = ticks;
            script.steps.push_back(step);
            Step release;
            release.line = number;
            release.ticks = ticks;
            step = release;
        } else if (cmd == "STICK" || cmd == "RSTICK") {
            Stick s;
            if (w.size() < 4 || w.size() > 5 || !Axis(w[1], s.x) || !Axis(w[2], s.y) ||
                !Ticks(w[3], step.ticks)) {
                return fail("want: " + Upper(w[0]) + " X Y TICKS [KEYS], X and Y from -1 to 1");
            }
            if (w.size() == 5 && !Keys(w[4], step, bad)) {
                return fail("unknown key " + bad);
            }
            (cmd == "STICK" ? step.left : step.right) = s;
        } else if (cmd == "MARK") {
            if (w.size() != 2) {
                return fail("want: mark NAME (one word)");
            }
            step.mark = std::string(w[1]);
        } else {
            return fail("unknown command " + std::string(w[0]));
        }
        script.steps.push_back(std::move(step));
    }
    for (const Step& s : script.steps) {
        script.total_ticks += s.ticks;
    }
    if (script.steps.empty()) {
        error = "no steps";
        return std::nullopt;
    }
    return script;
}

const Step* At(const Script& script, std::uint64_t tick) {
    std::uint64_t start = 0;
    for (const Step& s : script.steps) {
        if (s.ticks == 0) {
            continue;
        }
        if (tick < start + s.ticks) {
            return &s;
        }
        start += s.ticks;
    }
    return nullptr;
}

std::vector<const Step*> MarksBetween(const Script& script, std::int64_t from, std::int64_t to) {
    std::vector<const Step*> marks;
    std::int64_t start = 0;
    for (const Step& s : script.steps) {
        if (s.ticks == 0 && start > from && start <= to) {
            marks.push_back(&s);
        }
        start += s.ticks;
    }
    return marks;
}

} // namespace CoopTestInput

#ifdef LICHII_COOP

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

#include "common/logging.h"
#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"

namespace CoopTestInput {

namespace {

using Clock = std::chrono::steady_clock;

struct Running {
    std::shared_ptr<const Script> script;
    Clock::time_point start;
    std::int64_t last_tick = -1;
    Core::HID::EmulatedController* controller = nullptr;
};

struct State {
    bool checked_env = false;
    std::filesystem::path path;  // empty: off
    std::filesystem::file_time_type seen_time{};
    std::uintmax_t seen_size = 0;
    bool seen = false;
    unsigned frame = 0;
    std::optional<Running> running;
} g_state;  // only touched on the present thread

std::filesystem::path EnvPath() {
#ifdef _MSC_VER
    wchar_t* value = nullptr;
    std::size_t size = 0;
    if (_wdupenv_s(&value, &size, L"LICHII_COOP_TEST_SCRIPT") != 0 || value == nullptr) {
        return {};
    }
    std::filesystem::path p(value);
    std::free(value);
    return p;
#else
    const char* value = std::getenv("LICHII_COOP_TEST_SCRIPT");
    return value ? std::filesystem::path(value) : std::filesystem::path{};
#endif
}

std::int64_t TickNow(const Running& r) {
    const auto elapsed = std::chrono::duration<double>(Clock::now() - r.start).count();
    return static_cast<std::int64_t>(elapsed * kTicksPerSecond);
}

Core::HID::EmulatedController* PickController(Core::System& system) {
    auto& hid = system.HIDCore();
    auto* player1 = hid.GetEmulatedController(Core::HID::NpadIdType::Player1);
    if (player1 != nullptr && player1->IsConnected()) {
        return player1;
    }
    auto* handheld = hid.GetEmulatedController(Core::HID::NpadIdType::Handheld);
    if (handheld != nullptr && handheld->IsConnected()) {
        return handheld;
    }
    return nullptr;
}

void StopRunning() {
    if (g_state.running && g_state.running->controller) {
        g_state.running->controller->SetInputInjector({});
    }
    g_state.running.reset();
}

void Begin(Core::System& system, Script script) {
    StopRunning();
    auto* controller = PickController(system);
    if (controller == nullptr) {
        LOG_ERROR(Core, "coop: test script: no player 1 or handheld controller connected");
        return;
    }
    Running r;
    r.script = std::make_shared<const Script>(std::move(script));
    r.start = Clock::now();
    r.controller = controller;
    controller->SetInputInjector([script = r.script, start = r.start]()
                                     -> std::optional<Core::HID::InjectedInput> {
        const auto elapsed = std::chrono::duration<double>(Clock::now() - start).count();
        const Step* step =
            At(*script, static_cast<std::uint64_t>(elapsed * kTicksPerSecond));
        if (step == nullptr) {
            return std::nullopt;
        }
        Core::HID::InjectedInput in;
        in.buttons = static_cast<Core::HID::NpadButton>(step->buttons);
        if (step->left) {
            in.left = Core::HID::AnalogStickState{step->left->x, step->left->y};
        }
        if (step->right) {
            in.right = Core::HID::AnalogStickState{step->right->x, step->right->y};
        }
        return in;
    });
    LOG_INFO(Core, "coop: test script started: {} steps, {:.1f} s", r.script->steps.size(),
             static_cast<double>(r.script->total_ticks) / kTicksPerSecond);
    g_state.running = std::move(r);
}

void CheckFile(Core::System& system) {
    std::error_code ec;
    const auto time = std::filesystem::last_write_time(g_state.path, ec);
    if (ec) {
        return;
    }
    const auto size = std::filesystem::file_size(g_state.path, ec);
    if (ec || (g_state.seen && time == g_state.seen_time && size == g_state.seen_size)) {
        return;
    }
    g_state.seen = true;
    g_state.seen_time = time;
    g_state.seen_size = size;
    if (size == 0) {
        return;
    }
    std::ifstream in(g_state.path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    std::string error;
    auto script = Parse(text.str(), error);
    if (!script) {
        LOG_ERROR(Core, "coop: test script {}: {}", g_state.path.string(), error);
        return;
    }
    Begin(system, std::move(*script));
}

} // namespace

void OnFrame(Core::System& system) {
    if (!g_state.checked_env) {
        g_state.checked_env = true;
        g_state.path = EnvPath();
        if (!g_state.path.empty()) {
            LOG_INFO(Core, "coop: test script file {}", g_state.path.string());
        }
    }
    if (g_state.path.empty()) {
        return;
    }
    if (g_state.frame++ % 15 == 0) {
        CheckFile(system);
    }
    if (!g_state.running) {
        return;
    }
    Running& r = *g_state.running;
    const std::int64_t now = TickNow(r);
    for (const Step* m : MarksBetween(*r.script, r.last_tick, now)) {
        LOG_INFO(Core, "coop: test mark {}", m->mark);
    }
    r.last_tick = now;
    if (now >= static_cast<std::int64_t>(r.script->total_ticks)) {
        LOG_INFO(Core, "coop: test script done");
        StopRunning();
    }
}

} // namespace CoopTestInput

#endif // LICHII_COOP
