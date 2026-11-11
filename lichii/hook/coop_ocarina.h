// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ocarina notes for the other players (issue 152, docs/puppet.md "Ocarina").
// The notes are not AS events: the game's ocarina code picks the note from
// the player's input and plays the SLink asset Ocarina_<note> itself, and
// Link's AS chooses the fingers from AS variables (OcarinaLastSingleNote,
// OcarinaLastMelodyType, OcarinaFrameCount). Where that note is kept, and the
// call that plays a named SLink asset, were not found by reading `main`. This
// probe finds the note in a run: while one of our Link's AS slots plays an
// ocarina node, it logs each slot's command when it changes and every small
// word (0 to 63) of the AS controller and its slots that stepped. It only
// reads, and is off unless LICHII_COOP_OCARINA_PROBE=1.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopOcarina {

// The notes of the game, in the order of its SLink assets Ocarina_<name>.
// Index 0 is "no note".
const std::vector<std::string>& NoteNames();
// The index of a note name (Ocarina_A -> "A"), or 0.
std::uint8_t NoteIndex(std::string_view name);

class OcarinaProbe {
public:
    using ReadWord = std::function<std::optional<std::uint64_t>(std::uint64_t)>;

    // The AS chain (docs/puppet.md, "Where the current animation is").
    static constexpr std::uint64_t kAsComponent = 0x270;
    static constexpr std::uint64_t kAsStep1 = 0x40;
    static constexpr std::uint64_t kAsStep2 = 0x18;
    static constexpr std::uint64_t kAsStep3 = 0x18;
    static constexpr std::uint64_t kSlotCount = 0x18;
    static constexpr std::uint64_t kSlotArray = 0x20;
    static constexpr std::uint64_t kSlotStride = 0x48;
    static constexpr std::uint64_t kSlotCommand = 0x10;
    static constexpr std::size_t kMaxSlots = 8;
    static constexpr std::size_t kControllerBytes = 0x400;
    static constexpr std::size_t kSlotBytes = 0x200;
    static constexpr std::uint32_t kSmall = 64;      // a note or melody index is below this
    static constexpr int kNoisySteps = 40;           // a word that steps this often is muted
    static constexpr std::size_t kMaxShown = 24;     // steps per line
    static constexpr int kMaxLines = 600;            // per session
    static constexpr std::uint64_t kEveryUs = 33'000;

    // Once per walk with our own Link's actor. Returns log lines.
    std::vector<std::string> Look(const ReadWord& read, std::uint64_t actor, std::uint64_t now_us);
    void Reset();

    // For tests.
    bool Active() const { return active_; }

private:
    std::optional<std::string> ReadName(const ReadWord& read, std::uint64_t address) const;

    std::map<std::uint64_t, std::uint32_t> words_;  // key: (region << 32) | offset
    std::map<std::uint64_t, int> steps_;
    std::vector<std::string> commands_;
    bool active_ = false;
    std::uint64_t last_us_ = 0;
    int lines_ = 0;
};

} // namespace CoopOcarina
