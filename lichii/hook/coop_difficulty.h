// SPDX-License-Identifier: GPL-3.0-or-later
//
// Difficulty: enemies with more life, harder hits and more of them, by the
// number of players or as the host sets it (docs/play-online.md "Difficulty";
// docs/protocol.md "Difficulty"). The server decides the three values for
// everyone (coop/protocol/difficulty.hpp has the scaler and the presets);
// this is the game's side: the rules that turn the values into life, damage
// and extra enemies (no emulator code: built straight into the tests), and
// the session's value with the host's settings for the pause menu
// (coop_difficulty_game.cpp).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/difficulty.hpp"

namespace coop::host {
class Session;
}
namespace coop::net {
class Client;
struct Event;
}

namespace CoopDifficulty {

using Values = coop::protocol::DifficultyValues;
using Preset = coop::protocol::DifficultyPreset;
using Choice = coop::protocol::DifficultyChoice;

// ---------------------------------------------------------------------------
// The host's settings (pause menu: Multiplayer > Host settings > Difficulty)
// ---------------------------------------------------------------------------

// What the menu shows. `choice` is the session's (the server's), `values`
// what is in force for everyone now.
struct State {
    bool connected = false;  // in a session, and the server has said its difficulty
    bool can_edit = false;   // we host: Set() changes it for everyone
    Choice choice;
    Values values;
    std::uint8_t set_by = 0;  // 0: nobody chose yet (automatic), else the host who did
};

// Any thread.
State Get();

// Any thread: the host's new choice, for everyone. Saved for this player's
// next session as host, and sent on the next frame; the menu shows it once
// the server has answered (Get). False when we do not host a session now.
bool Set(const Choice& choice);

// Any thread: the values in force now (the game as it is while not in a
// session or before the server said them).
Values Now();

// The menu's rows. A preset or a moved slider turns the automatic scaler
// off: what the host sets stays, whoever joins or leaves.
enum class Slider : std::uint8_t { hp, damage, count };
std::string_view PresetName(Preset preset);  // "Easy", "Normal", "Hard", "Custom"
Choice WithAuto(const Choice& now, bool on);
Choice WithPreset(const Choice& now, Preset preset);
// `steps` slider steps up (down when negative) from `in_force`, the values
// shown now, kept within the slider's ends; the preset becomes custom.
Choice WithSlider(const Choice& now, const Values& in_force, Slider slider, int steps);

// "Automatic (3 players): life 180%, damage 140%, enemies 150%",
// "Hard: life 200%, ...". `players` is only shown with the automatic scaler.
std::string Describe(const Choice& choice, const Values& values, std::size_t players);

// The host's choice as kept between sessions (coop-difficulty.ini beside
// coop.ini, written by Set): `key = value` lines. Nothing when the text is not one.
std::string FormatChoice(const Choice& choice);
std::optional<Choice> ParseChoice(std::string_view text);

// ---------------------------------------------------------------------------
// The rules the game's side applies
// ---------------------------------------------------------------------------

// Enemy life. The most life an enemy has at `hp_pct` when the game gave it
// `base_max`: rounded to the nearest, never under 1.
std::int32_t ScaledMax(std::int32_t base_max, std::uint16_t hp_pct);
// An enemy's life after its most went from `old_max` to `new_max`: the same
// share of it, rounded up when the most grows and down when it shrinks (so
// up and back down gives the life it had), never under 1 for a living
// enemy: a change never kills one. 0 stays 0.
std::int32_t ScaledLife(std::int32_t life, std::int32_t old_max, std::int32_t new_max);

// Enemy damage. What one hit of `taken` (quarter hearts) costs at
// `damage_pct`. Parts of a quarter are carried to the next hit, so many
// small hits add up to the same as one big one.
class DamageScaler {
public:
    std::int32_t Scale(std::int32_t taken, std::uint16_t damage_pct);
    void Reset() { carry_ = 0; }

private:
    std::int32_t carry_ = 0;  // hundredths of a quarter heart not yet taken
};

// Enemy count. How many more of the enemy placed with `key` there are at
// `count_pct`: every enemy gets count_pct / 100 - 1, and count_pct % 100 in
// a hundred get one more. Which ones is decided by the key alone, so every
// game picks the same enemies every time.
unsigned ExtraCount(std::uint64_t key, std::uint16_t count_pct);

// ---------------------------------------------------------------------------
// The session (coop_difficulty_game.cpp)
// ---------------------------------------------------------------------------

// Whether a game that hosts sends its kept choice: once per connection, when
// it was a choice of its own (not the automatic scaler) and nobody has
// chosen for this session yet. A host who takes over a session keeps what
// the host before set.
bool OffersKept(bool hosts, std::uint8_t set_by, const std::optional<Choice>& kept, bool offered);

// Network thread, from the session's on_event (the network lock is held):
// DIFFICULTY; a new connection starts with the game as it is.
void OnEvent(const coop::net::Event& event, coop::net::Client& client);

// Present thread, once per frame: sends the host's choice (Set, or the kept
// one when we start to host a session nobody has chosen for). `session`
// null when not connected.
void Frame(coop::host::Session* session);

// The chat notices since the last call (for the chat box).
std::vector<std::string> TakeLines();

} // namespace CoopDifficulty
