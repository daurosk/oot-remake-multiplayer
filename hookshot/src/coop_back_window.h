// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Row 83 / #245: with two or more of our games on one PC (the launcher's local co-op, 2 to 4 games, or Nucleus Co-op),
// the one whose window is not in front was choppy. Nothing of ours or Longshot's slows it (reports/row-83-unfocus.md:
// EcoQoS and the timer limit are off, focus is read for input only); Windows favours the foreground process for the
// CPU (its quantum boost) and the GPU (WDDM's foreground boost). So a game whose window is behind while ANOTHER of our
// games runs on the PC asks Windows for one step more than normal; in front, alone, minimized or hidden it is plain
// normal again, so a solo game behind a browser never takes anything from the window in front.
//
// Logic only (tests/hookshot/back_window_test.cpp); the Windows reads and calls are in coop_back_window_os.cpp.

#include <cstring>

namespace CoopBackWindow {

// What is raised: LICHII_COOP_BACK_BOOST = off | gpu | cpu | both (anything else: the default).
enum class Mode { Off, Gpu, Cpu, Both };

constexpr Mode kDefault = Mode::Both;

inline Mode ModeFrom(const char* env) {
    if (env == nullptr || *env == 0) return kDefault;
    if (std::strcmp(env, "off") == 0 || std::strcmp(env, "0") == 0) return Mode::Off;
    if (std::strcmp(env, "gpu") == 0) return Mode::Gpu;
    if (std::strcmp(env, "cpu") == 0) return Mode::Cpu;
    if (std::strcmp(env, "both") == 0 || std::strcmp(env, "1") == 0) return Mode::Both;
    return kDefault;
}

struct State {
    bool other_game = false;   // another process of our game's exe runs on this PC
    bool has_window = false;   // this game's window was found (visible)
    bool in_front = false;     // it is the foreground window
    bool iconic = false;       // minimized
};

struct Want {
    bool gpu_up = false;   // GPU scheduling class above normal (else normal)
    bool cpu_up = false;   // process class above normal (else normal)
    bool operator==(const Want& o) const { return gpu_up == o.gpu_up && cpu_up == o.cpu_up; }
    bool operator!=(const Want& o) const { return !(*this == o); }
};

inline Want Decide(Mode m, const State& s) {
    const bool behind = s.other_game && s.has_window && !s.in_front && !s.iconic;
    Want w;
    if (!behind || m == Mode::Off) return w;
    w.gpu_up = m == Mode::Gpu || m == Mode::Both;
    w.cpu_up = m == Mode::Cpu || m == Mode::Both;
    return w;
}

// Called from the frame (coop_hookshot.cpp): once a second it reads the state and applies a changed Want on a thread
// of its own (CoopOffFrame), never in the frame. A no-op off Windows.
void Tick();

}  // namespace CoopBackWindow
