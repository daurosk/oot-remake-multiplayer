// SPDX-License-Identifier: GPL-3.0-or-later
//
// The other players hear a player's Ocarina (issue 152 part 2, docs/puppet.md
// "Ocarina"). The sending side is here: while our own Link plays the Ocarina
// (its AS command DemoOcarina), a note button pressed sends OCARINA_NOTE with
// the note, and letting the last one go sends note 0. The other games play
// the note's SLink asset Ocarina_<note> on our puppet (coop_puppet_sound.h).
//
// The note comes from the button, not from game memory: where the game keeps
// the note was not found (reports/ocarina-probe.md), but its prompt while the
// Ocarina is out reads "Play a note: L R Y X A", the five notes from low to
// high as in Ocarina of Time 3D: L the low D, R F, Y A, X B, A the high D.
// No pitch bend is sent.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace coop::host {
class Session;
}
namespace Core {
class System;
}

namespace CoopOcarinaSound {

// The game's own note table (OcarinaMgr + 0x40, set up in its constructor at
// main + 0xe7c570), in that order; OCARINA_NOTE's note is the index plus one.
inline constexpr std::size_t kNoteCount = 19;
const char* NoteName(std::uint8_t note);  // 1..19: "D" for Ocarina_D; else ""
std::string AssetName(std::uint8_t note); // "Ocarina_D"; "" for no note

// The emulator's NpadButton bits (hid_core/hid_types.h), as CoopTestInput.
inline constexpr std::uint64_t kButtonA = 1u << 0;
inline constexpr std::uint64_t kButtonX = 1u << 2;
inline constexpr std::uint64_t kButtonY = 1u << 3;
inline constexpr std::uint64_t kButtonL = 1u << 6;
inline constexpr std::uint64_t kButtonR = 1u << 7;
inline constexpr std::uint64_t kNoteButtons = kButtonA | kButtonX | kButtonY | kButtonL | kButtonR;

// The note a button plays: L 4 (D), R 7 (F), Y 11 (A), X 13 (B), A 16
// (HighD); 0 for any other single button.
std::uint8_t NoteOf(std::uint64_t button);

// Whether our Link's AS command (PLAYER_SAMPLE's animation) is the Ocarina
// being played: DemoOcarina, not the song-learnt pose DemoOcarinaMelodyGet.
bool Playing(std::string_view command);

// Turns our Link's command and the pad into notes to send.
class NoteSender {
public:
    // Each frame. Returns the note to send now, if any: a new note when a
    // note button is pressed while playing (the newest press wins), 0 when
    // the note sounding is let go (or playing stops) with no other note held.
    std::optional<std::uint8_t> Update(bool playing, std::uint64_t buttons);
    std::uint8_t sounding() const { return sounding_; }

private:
    std::uint64_t held_ = 0;     // note buttons held last frame
    std::uint8_t sounding_ = 0;  // the note last sent; 0 none
    std::uint64_t button_ = 0;   // the button of that note
};

// The emulator side (present thread): reads player 1's pad and our Link's
// command, sends what NoteSender gives. `command`: our own reading's
// animation, nullopt if none. Logs each note sent (one line, "coop: ocarina
// note ... sent").
void Frame(Core::System& system, const std::optional<std::string>& command,
           coop::host::Session* session);

} // namespace CoopOcarinaSound
