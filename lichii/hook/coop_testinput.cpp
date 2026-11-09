// Scripted controller input for automatic tests; see coop_testinput.h.
// The parser and the stepper have no emulator code (the tests build them
// alone); the part that drives the emulated pad is only built with LICHII_COOP.
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

bool Number(std::string_view word, float& out) {
    const std::string s(word);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (s.empty() || end != s.c_str() + s.size() || !std::isfinite(v) || std::fabs(v) > 1e6) {
        return false;
    }
    out = static_cast<float>(v);
    return true;
}

constexpr double kPi = 3.14159265358979323846;

double Wrap(double a) {
    while (a > kPi) {
        a -= 2 * kPi;
    }
    while (a < -kPi) {
        a += 2 * kPi;
    }
    return a;
}

constexpr std::uint32_t kGotoTicks = 600;  // 10 s
constexpr float kMovedM = 0.15f;           // movement that teaches the camera model
constexpr double kMinMagnitude = 0.45;     // slower near the point, never a crawl

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
        } else if (cmd == "GOTO") {
            step.kind = Kind::go;
            step.ticks = kGotoTicks;
            if (w.size() < 3 || w.size() > 5 || !Number(w[1], step.x) || !Number(w[2], step.z) ||
                (w.size() >= 4 && (!Number(w[3], step.radius) || step.radius < 0.2f)) ||
                (w.size() == 5 && !Ticks(w[4], step.ticks))) {
                return fail("want: goto X Z [RADIUS] [TICKS], radius at least 0.2");
            }
        } else if (cmd == "TOWARD") {
            step.kind = Kind::toward;
            if (w.size() < 4 || w.size() > 5 || !Number(w[1], step.x) || !Number(w[2], step.z) ||
                !Ticks(w[3], step.ticks)) {
                return fail("want: toward X Z TICKS [KEYS]");
            }
            if (w.size() == 5 && !Keys(w[4], step, bad)) {
                return fail("unknown key " + bad);
            }
        } else if (cmd == "MARK") {
            if (w.size() != 2) {
                return fail("want: mark NAME (one word)");
            }
            step.kind = Kind::mark;
            step.mark = std::string(w[1]);
        } else {
            return fail("unknown command " + std::string(w[0]));
        }
        script.steps.push_back(std::move(step));
    }
    if (script.steps.empty()) {
        error = "no steps";
        return std::nullopt;
    }
    return script;
}

Runner::Runner(Script script) : script_(std::move(script)) {}

void Runner::Advance(double now_s, std::optional<std::array<float, 2>> player,
                     std::vector<std::string>& log) {
    // Learn the camera from how Link moved under the stick held since last_pos_.
    if (player) {
        if (!last_pos_ || !last_stick_angle_) {
            last_pos_ = player;
        } else {
            const float dx = (*player)[0] - (*last_pos_)[0];
            const float dz = (*player)[1] - (*last_pos_)[1];
            if (std::hypot(dx, dz) >= kMovedM) {
                const double seen = std::atan2(dz, dx);
                const double stick = *last_stick_angle_ - kPi / 2;
                if (heading_) {
                    const double expected = *heading_ + mirror_ * stick;
                    if (std::fabs(Wrap(seen - expected)) > 2.0) {
                        if (++wrong_way_ >= 3) {
                            mirror_ = -mirror_;
                            wrong_way_ = 0;
                        }
                    } else {
                        wrong_way_ = 0;
                    }
                }
                heading_ = Wrap(seen - mirror_ * stick);
                last_pos_ = player;
            }
        }
    }

    while (index_ < script_.steps.size()) {
        const Step& s = script_.steps[index_];
        const double length = static_cast<double>(s.ticks) / kTicksPerSecond;
        if (s.kind == Kind::mark) {
            log.push_back("mark " + s.mark);
            ++index_;  // the next step starts when this one did
            continue;
        }
        if (s.kind == Kind::go) {
            const bool there =
                player && std::hypot(s.x - (*player)[0], s.z - (*player)[1]) <= s.radius;
            if (there || now_s - step_start_ >= length) {
                if (!there) {
                    log.push_back("goto timed out at line " + std::to_string(s.line));
                }
                ++index_;
                step_start_ = now_s;
                continue;
            }
            Steer(s, player);
            return;
        }
        if (now_s - step_start_ >= length) {
            ++index_;
            step_start_ += length;  // timed steps keep exact time between them
            continue;
        }
        if (s.kind == Kind::toward) {
            Steer(s, player);
            return;
        }
        out_ = Output{s.buttons, s.left, s.right};
        last_stick_angle_.reset();
        if (s.left && (s.left->x != 0 || s.left->y != 0)) {
            last_stick_angle_ = std::atan2(static_cast<double>(s.left->y), s.left->x);
        }
        return;
    }
    out_ = Output{};
    last_stick_angle_.reset();
}

void Runner::Steer(const Step& step, std::optional<std::array<float, 2>> player) {
    out_ = Output{step.buttons, Stick{0, 0}, std::nullopt};
    if (!player) {
        last_stick_angle_.reset();
        return;
    }
    double angle = kPi / 2;  // no camera model yet: stick up, and learn from it
    double magnitude = 1.0;
    if (heading_) {
        const double want = std::atan2(step.z - (*player)[1], step.x - (*player)[0]);
        angle = kPi / 2 + mirror_ * Wrap(want - *heading_);
        if (step.kind == Kind::go) {
            const double dist = std::hypot(step.x - (*player)[0], step.z - (*player)[1]);
            magnitude = std::clamp(dist / 2.0, kMinMagnitude, 1.0);
        }
    }
    out_.left = Stick{static_cast<std::int32_t>(std::lround(std::cos(angle) * magnitude * kStickMax)),
                      static_cast<std::int32_t>(std::lround(std::sin(angle) * magnitude * kStickMax))};
    last_stick_angle_ = angle;
}

} // namespace CoopTestInput

#ifdef LICHII_COOP

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

#include "common/logging.h"
#include "coop/host/player_reader.hpp"
#include "coop_flags.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_types.h"

namespace CoopTestInput {

namespace {

using Clock = std::chrono::steady_clock;

// What the injector gives the pad: written on the present thread, read on
// the input thread.
struct Shared {
    std::mutex mutex;
    Output out;
};

struct Running {
    std::unique_ptr<Runner> runner;
    std::shared_ptr<Shared> shared;
    Clock::time_point start;
    Core::HID::EmulatedController* controller = nullptr;
};

struct State {
    bool checked_env = false;
    std::filesystem::path path;  // empty: off
    std::filesystem::file_time_type seen_time{};
    std::uintmax_t seen_size = 0;
    bool seen = false;
    unsigned frame = 0;
    Kernel::KProcess* process = nullptr;
    u64 main = 0;
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

// Link's position (x, z), for goto and toward.
std::optional<std::array<float, 2>> PlayerXZ(Core::System& system) {
    auto* process = system.ApplicationProcess();
    if (process == nullptr) {
        return std::nullopt;
    }
    if (process != g_state.process) {
        g_state.process = process;
        Loader::AppLoader::Modules modules;
        if (system.GetAppLoader().ReadNSOModules(modules) != Loader::ResultStatus::Success ||
            modules.empty()) {
            modules = Core::FindModules(process);
        }
        g_state.main = 0;
        for (const auto& [address, name] : modules) {
            if (name == "main" || name.starts_with("main.")) {
                g_state.main = address;
            }
        }
        if (g_state.main == 0) {
            g_state.main = GetInteger(Core::FindMainModuleEntrypoint(process));
        }
    }
    CoopHook::EmulatorMemory memory(system.ApplicationMemory(), g_state.main);
    coop::host::PlayerReader reader(memory);
    const auto p = reader.read_position();
    if (!p) {
        return std::nullopt;
    }
    return std::array<float, 2>{(*p)[0], (*p)[2]};
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
    r.runner = std::make_unique<Runner>(std::move(script));
    r.shared = std::make_shared<Shared>();
    r.start = Clock::now();
    r.controller = controller;
    controller->SetInputInjector(
        [shared = r.shared]() -> std::optional<Core::HID::InjectedInput> {
            Output out;
            {
                std::lock_guard lock(shared->mutex);
                out = shared->out;
            }
            Core::HID::InjectedInput in;
            in.buttons = static_cast<Core::HID::NpadButton>(out.buttons);
            if (out.left) {
                in.left = Core::HID::AnalogStickState{out.left->x, out.left->y};
            }
            if (out.right) {
                in.right = Core::HID::AnalogStickState{out.right->x, out.right->y};
            }
            return in;
        });
    LOG_INFO(Core, "coop: test script started: {} steps", r.runner->StepCount());
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
    const double now = std::chrono::duration<double>(Clock::now() - r.start).count();
    std::vector<std::string> log;
    r.runner->Advance(now, PlayerXZ(system), log);
    for (const auto& line : log) {
        LOG_INFO(Core, "coop: test {}", line);
    }
    {
        std::lock_guard lock(r.shared->mutex);
        r.shared->out = r.runner->Current();
    }
    if (r.runner->Done()) {
        LOG_INFO(Core, "coop: test script done");
        StopRunning();
    }
}

} // namespace CoopTestInput

#endif // LICHII_COOP
