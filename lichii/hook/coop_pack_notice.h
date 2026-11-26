// SPDX-License-Identifier: GPL-3.0-or-later
//
// No puppet pack (player report, 0.11.0): the other players then show as their
// names only, and the only hint was a line in longshot_log.txt. This says it in
// the co-op chat once per game run, when another player is first shown, with
// what to do. Only the Longshot module turns it on (Enable): the emulator's
// kit says it in its start window. Header only, standard C++ only.
#pragma once

#include <atomic>
#include <string>
#include <vector>

namespace CoopPackNotice {

// The kit's start file per system: the Windows kit's Start Squadala.bat, the Linux kit's start-squadala.sh.
#ifdef _WIN32
inline constexpr const char* kText =
    "No puppet pack: other players show as names only. Close the game and run Start Squadala.bat again.";
#else
inline constexpr const char* kText =
    "No puppet pack: other players show as names only. Close the game and run start-squadala.sh again.";
#endif

// The once-per-run decision, apart for the tests.
class Once {
public:
    // `pack`: a puppet pack is loaded. `others`: another player is shown now.
    // True the first time there is no pack and someone to see; never again.
    bool Due(bool pack, bool others) {
        if (said_ || pack || !others) {
            return false;
        }
        said_ = true;
        return true;
    }

private:
    bool said_ = false;
};

namespace detail {
inline std::atomic<int>& State() {  // 0 off, 1 on with a pack, 2 on without one
    static std::atomic<int> s{0};
    return s;
}
inline Once& TheOnce() {
    static Once o;
    return o;
}
}  // namespace detail

// The module's start, once: `pack` is whether the puppet pack is there (Longshot
// indexes its mod files once at start, so it does not change while it runs).
inline void Enable(bool pack) { detail::State().store(pack ? 1 : 2); }

// The game's own start file (longshot.exe in the kit's game folder, or a Longshot the player built) started by hand
// instead of the kit's start file (player report 225, 0.11.8): the launcher is what makes the puppet pack, the
// size tables and the Discord status, and keeps the game and the kit the same version. Said once per run, in the
// chat, at once (not only when someone is shown). The module turns it on when the launcher's marker is missing.
#ifdef _WIN32
inline constexpr const char* kDirectText =
    "This game was not started with Start Squadala.bat. Close it and start the kit with Start Squadala.bat "
    "(you do not need your own Longshot build: the kit builds its own game).";
#else
inline constexpr const char* kDirectText =
    "This game was not started with start-squadala.sh. Close it and start the kit with start-squadala.sh "
    "(you do not need your own Longshot build: the kit builds its own game).";
#endif

namespace detail {
inline std::atomic<bool>& DirectOn() {
    static std::atomic<bool> s{false};
    return s;
}
inline std::atomic<bool>& DirectSaid() {
    static std::atomic<bool> s{false};
    return s;
}
}  // namespace detail

inline void EnableDirect(bool on) { detail::DirectOn().store(on); }

// Solo, with no session and so no chat box: the title's own hint line under its buttons says it (coop_title.cpp).
inline bool DirectOn() { return detail::DirectOn().load(std::memory_order_relaxed); }
inline constexpr const char* kDirectHint =
    "Not started with Start Squadala: close the game and start the kit with Start Squadala.";

// Present thread, once per frame: the line to add to the chat, or nothing.
inline std::vector<std::string> TakeLines(bool others) {
    if (detail::DirectOn().load(std::memory_order_relaxed) && !detail::DirectSaid().exchange(true)) {
        return {kDirectText};
    }
    const int s = detail::State().load(std::memory_order_relaxed);
    if (s == 0 || !detail::TheOnce().Due(s == 1, others)) {
        return {};
    }
    return {kText};
}

}  // namespace CoopPackNotice
