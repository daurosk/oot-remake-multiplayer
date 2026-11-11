// SPDX-License-Identifier: GPL-3.0-or-later
//
// Scene-change probe (coop_scene_probe.h). Logs one line per change:
//   coop: scene probe: frame N scene=X event=0/1 switch=0/1 other=0/1
//   loading=0/1 control=<name or hash> banc="..." start="..." starter="..."
//   boot="..."
// "-" is a flag that could not be read.

#include "coop_scene_probe.h"

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "common/logging.h"
#include "core/core.h"
#include "core/memory.h"
#include "coop/host/flags.hpp"
#include "coop/host/game_flags.hpp"
#include "coop_flags.h"

namespace CoopSceneProbe {
namespace {

bool OnByEnv() {
    const char* v = std::getenv("LICHII_COOP_SCENE_PROBE");
    return v != nullptr && std::string_view(v) == "1";
}

// Main's selector reads these two Bools (0x3303ca0): 0x3fc35576 picks
// BancChange; 0x1ebc8dcd, read just before, sends it elsewhere.
constexpr std::uint32_t kSwitch = 0x3fc35576;
constexpr std::uint32_t kOther = 0x1ebc8dcd;

std::string Bool(const std::optional<bool>& b) {
    return b ? (*b ? "1" : "0") : "-";
}

std::string Str(const std::optional<std::string>& s) {
    return s ? "\"" + *s + "\"" : "-";
}

std::string Control(const std::optional<std::uint32_t>& v) {
    if (!v) {
        return "-";
    }
    static constexpr std::array<std::string_view, 6> kNames{
        "Idle", "ResetGimmick", "BloodyMoon", "ChangeBancOnEvent", "MemoryShortageBloodyMoon",
        "ResetAIScheduleNpc"};
    for (const auto name : kNames) {
        if (coop::host::flag_hash(name) == *v) {
            return std::string(name);
        }
    }
    return fmt::format("{:#010x}", *v);
}

struct State {
    bool on = OnByEnv();
    bool said = false;
    std::uint64_t frame = 0;
    std::string last;
};

State& TheState() {
    static State s;
    return s;
}

} // namespace

void Frame(Core::System& system, std::uint64_t main_base, const std::optional<coop::host::PlayerReading>& reading) {
    State& s = TheState();
    if (!s.on) {
        return;
    }
    if (!s.said) {
        s.said = true;
        LOG_INFO(Core, "coop: scene probe on (LICHII_COOP_SCENE_PROBE=1)");
    }
    ++s.frame;
    if (main_base == 0) {
        return;
    }
    using coop::host::flag_hash;
    CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_base);
    coop::host::GameFlags flags(memory);
    const std::string scene = reading && reading->scene ? *reading->scene : "-";
    const std::string event = reading ? Bool(reading->event_running) : "-";
    const std::string line =
        "scene=" + scene + " event=" + event + " switch=" + Bool(flags.get_bool(kSwitch)) +
        " other=" + Bool(flags.get_bool(kOther)) +
        " loading=" + Bool(flags.get_bool(flag_hash("Sequence_IsLoadingBanc"))) +
        " control=" + Control(flags.get_enum(flag_hash("Sequence_ControlInEvent"))) +
        " banc=" + Str(flags.get_string64(flag_hash("Sequence_ReservedNextBanc"))) +
        " start=" + Str(flags.get_string64(flag_hash("Sequence_ReservedNextStartPosName"))) +
        " starter=" + Str(flags.get_string64(flag_hash("Sequence_ReservedEventStarterName"))) +
        " boot=" + Str(flags.get_string64(flag_hash("Sequence_ReservedBootEventName")));
    if (line != s.last) {
        s.last = line;
        LOG_INFO(Core, "coop: scene probe: frame {} {}", s.frame, line);
    }
}

} // namespace CoopSceneProbe
