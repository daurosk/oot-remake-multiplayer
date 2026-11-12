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

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_puppet.h"

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

// The other players' ocarina notes, played here (issue 152): the same game
// calls the ocarina sound object's own update makes (main 0x3294df0), from
// the svc cave (coop_svc.h AddCaller). A note starts with 0xa44760 (a request
// that names the SLink asset Ocarina_<note>; it gives the sound, whose serial
// is at +0x10) and stops with 0x3a48de0 (the handle {sound, serial}, s0 < 0:
// at once). One voice per other player; one memory block from the game's
// malloc for the request and the handles, kept for the whole run. The cave's
// thread only, Want from any thread under the caller's lock.
class Voices {
public:
    static constexpr std::uint64_t kPlaySound = 0xa44760;
    static constexpr std::uint64_t kStopSound = 0x3a48de0;
    static constexpr std::uint64_t kMalloc = 0x14c6c90;  // CoopPuppet::addr::kMalloc
    // The names Ocarina_LowB ... Ocarina_HighF in main's rodata, as the
    // constructor main 0xe7c570 puts them at the object's +0x40.
    static constexpr std::array<std::uint64_t, coop::protocol::kOcarinaNotes> kNoteNames{
        0x3d99960, 0x3e03988, 0x3cf3aff, 0x3d74752, 0x3d40809, 0x3ce75ac, 0x3d003ad,
        0x3e1dd9b, 0x3e03995, 0x3d267ba, 0x3ccf52f, 0x3ca9c2d, 0x3d40818, 0x3e1ddaa,
        0x3d5a824, 0x3ca9c3c, 0x3ddbaa4, 0x3d0d49d, 0x3dc1d86};
    static constexpr std::size_t kVoices = 4;
    static constexpr std::size_t kRequestSize = 0x28;
    static constexpr std::uint64_t kRequest = 0x00;
    static constexpr std::uint64_t kHandles = 0x40;
    static constexpr std::uint64_t kHandleSize = 0x10;
    static constexpr std::size_t kBlockSize = kHandles + kHandleSize * kVoices;

    // What voice `voice` (a player number - 1) should sound: a note, or
    // nothing. A new count is a new note, even the same one.
    void Want(std::size_t voice, std::optional<coop::protocol::OcarinaNote> note);
    void SetVolume(float volume) { volume_ = volume; }
    // As a CoopSvc caller.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main_base);
    std::vector<std::string> TakeNotes();

    // For tests.
    std::uint64_t Block() const { return block_; }
    unsigned Played() const { return played_; }

private:
    enum class Pending { none, malloc, play, stop };
    struct Voice {
        std::optional<coop::protocol::OcarinaNote> want;
        std::optional<coop::protocol::OcarinaNote> playing;
        bool handle = false;  // a sound to stop is in this voice's handle
    };
    void Finish(CoopPuppet::Guest& guest, std::uint64_t result);
    static std::string Hex(std::uint64_t v);

    std::array<Voice, kVoices> voices_{};
    std::uint64_t block_ = 0;
    bool allocating_ = false;
    bool no_memory_ = false;
    Pending pending_ = Pending::none;
    std::size_t pending_voice_ = 0;
    float volume_ = 0.8f;  // the game's own quieter level (its other request)
    unsigned played_ = 0;
    std::vector<std::string> notes_;
};

} // namespace CoopOcarina
