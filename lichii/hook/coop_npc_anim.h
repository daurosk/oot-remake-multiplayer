// SPDX-License-Identifier: GPL-3.0-or-later
//
// Townspeople that walk and run on every player's screen (docs/objects.md,
// "Walking townspeople"). No emulator or game types here: the frame
// arithmetic and the Mover's book of aliases, unit tested in
// tests/lichii/npc_anim_test.cpp.
//
// What was wrong (kak-gate run, 2026-11-12, B's log): a copy's own AI asks
// for its own animation every game frame, the Mover put the owner's back
// every game frame (over 100 plays in 5 s for one copy), and started it at
// "the owner's frame" read from AS slot +0xe8, which is not the playing
// frame (the game's own getter 0x37b5e70 reads +0x10c). So a copy slid along
// the owner's path frozen in one pose.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace CoopNpcAnim {

// The playing frame of an AS slot (f32): what 0x37b5e70 (controller, slot)
// returns and the game compares with the frame count for "has it ended".
inline constexpr std::uint64_t kSlotPlayFrame = 0x10c;

// Frames a copy may be off before its frame is set.
inline constexpr float kFrameSlack = 3.0f;

// A drop of the owner's frame by more than this within one animation is the
// loop starting again (or the animation played again).
inline constexpr float kLoopDrop = 2.0f;

// The owner's animation speed in frames per game frame, measured between
// its last two samples of the same animation: a walk whose speed the game
// scales, or an animation standing on its last frame, is forecast as it
// really runs. `nominal` (the slot's rate) when the samples do not tell: too
// close, too far apart, or the frame went back (the loop began again).
float SeenRate(float before_frame, std::uint64_t before_us, float last_frame,
               std::uint64_t last_us, float nominal);

// The loop of the animation an owner plays, learned from its samples.
class Loop {
public:
    void See(std::string_view name, float frame);
    // Frames in one loop; 0 while no loop was seen end.
    float Length() const { return length_; }

private:
    std::string name_;
    float last_ = 0.0f;
    float top_ = 0.0f;
    float length_ = 0.0f;
};

// `frame` inside a loop of `length` frames (unchanged when length is 0).
float Wrapped(float frame, float length);

// How many frames `now` is from `want`, the short way round the loop.
float FrameGap(float now, float want, float length);

// The Mover's book for the copies that play the owner's animation while
// their slot names the one their own AI asked for. The game's play request
// skips a name the slot already shows, so the AI's request of every frame
// does nothing and the owner's animation plays on.
class Aliases {
public:
    struct Entry {
        std::string played;        // the owner's animation, playing
        std::uint64_t name_at = 0; // what the slot's name pointer is set to
        std::string name;          // the AI's own name, as the slot shows it
    };

    // Whether the copy plays `wanted`: its slot names it, or names the alias
    // written over our play of it. Anything else (the AI changed its mind,
    // the owner plays another one) is to be played again.
    bool Shows(std::uint64_t actor, std::string_view wanted, std::uint64_t slot_name_at,
               std::string_view slot_name);

    // Our play of `wanted` was made and the slot's name set back to the AI's.
    void Played(std::uint64_t actor, std::string_view wanted, std::uint64_t alias_at,
                std::string_view alias);

    // The copy is ours now, or gone: its entry, taken out (to tell the slot
    // the truth again, so its AI plays its own animation).
    std::optional<Entry> Drop(std::uint64_t actor);

    // The copies no longer among `wished` (the owner fell silent, the actor
    // left): their entries, taken out.
    std::vector<std::pair<std::uint64_t, Entry>> DropAllBut(
        const std::unordered_set<std::uint64_t>& wished);

    std::size_t Size() const { return entries_.size(); }

private:
    std::unordered_map<std::uint64_t, Entry> entries_;
};

} // namespace CoopNpcAnim
