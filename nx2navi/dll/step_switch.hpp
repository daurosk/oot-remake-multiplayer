// SPDX-License-Identifier: GPL-3.0-or-later
// step_switch.hpp: floor switches pressed by any player are down in every game (no Windows code, unit-tested).
//
// A floor switch has no saved flag (docs/dungeon.md). Its AI's selector `SelectorSwitchStepBootup` (evaluate
// main+0x11a4920, x0 = the node, actor [[[node+8]+8]+0x1f0]) goes to its On child when the button's slider is at
// its bottom or when its ForceOn input (a bool byte at [node+0x58]) is set; While On it plays `Button_Pushed`,
// sets its logic output `OnState` and the room does the rest (a torch, a shutter, an event). The port hooks that
// call: what our own switch shows is sent (PLAYER_EXTRA kind 3, below), and a switch another player holds down
// gets ForceOn here for as long as that player's game says so (again every 2 s while pressed), so a hold-type
// switch stays down while anyone stands on theirs and comes up when nobody does; a stay-down one latches by its
// own AI as in one game.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace CoopNx2navi::StepSwitch {

inline constexpr std::uint8_t kExtraSwitch = 3;           // PLAYER_EXTRA kind (puppet_fx.hpp has 1 and 2)
inline constexpr std::uint64_t kAgainUs = 2'000'000;      // a pressed switch is said again this often
inline constexpr std::uint64_t kHoldUs = 5'000'000;       // another player's press lasts this long unless said again
inline constexpr std::uint64_t kForcedQuietUs = 30'000'000;  // a switch we forced is not ours to say this long

struct Note {
    std::string scene;
    std::uint64_t hash = 0;   // the switch's placement hash
    bool pressed = false;
};

// PLAYER_EXTRA data: u8 pressed, u64 hash (little endian), the scene's name.
std::vector<std::uint8_t> Encode(const Note& n);
std::optional<Note> Decode(const std::vector<std::uint8_t>& data);

class Switches {
public:
    // Our scene (the frame); a new one forgets the others' presses and ours.
    void SetScene(const std::string& scene);
    // The hook, before the call: should our switch with this hash get ForceOn now?
    bool Force(std::uint64_t hash, std::uint64_t now_us);
    // The hook, after the call: whether our switch is On now.
    void Seen(std::uint64_t hash, bool on, std::uint64_t now_us);
    // Another player's note.
    void Remote(std::uint8_t player, const Note& n, std::uint64_t now_us);
    // What to send now (changes, and pressed ones again every kAgainUs).
    std::vector<Note> TakeSends(std::uint64_t now_us);
    // Log lines since the last ask.
    std::vector<std::string> TakeLines();
    std::size_t Forced() const;  // switches held down for others right now (status line)

private:
    struct Ours {
        bool on = false;
        bool said = false;          // the state was sent once
        std::uint64_t sent_us = 0;
        bool changed = false;
    };
    mutable std::mutex mutex_;
    std::string scene_;
    std::map<std::uint64_t, Ours> ours_;
    std::map<std::uint64_t, std::map<std::uint8_t, std::uint64_t>> remote_;  // hash -> player -> until
    std::map<std::uint64_t, std::uint64_t> forced_us_;                       // hash -> last ForceOn
    std::set<std::uint64_t> forced_said_;
    std::vector<std::string> lines_;
};

} // namespace CoopNx2navi::StepSwitch
