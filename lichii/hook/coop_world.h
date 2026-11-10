// SPDX-License-Identifier: GPL-3.0-or-later
//
// World objects shared like enemies (docs/world.md): the breakables. Pots,
// crates, liftable and breakable rocks, grass, signs, cracked walls and the
// rest have the same Life and Death components as enemies (their packs'
// LifeRef, DeathRef and DestructionRef), so the enemy sharing in
// coop_objects_sync.cpp carries them: one owner, a break on a copy goes to
// the owner as DAMAGE, and the owner's life 0 reaches every copy, where the
// game's own death breaks it. No emulator code.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_puppet.h"

namespace CoopWorld {

// True for the breakable world objects (by actor name).
bool IsBreakable(std::string_view name);

// Research, not sync (docs/world.md, "Field grass"): the grass that is not an
// actor is drawn by game::foliage, and its shader reads a buffer named
// SsboGrassCut. Look() follows main+0x4e22540 -> +0xf0 -> +0x338 (the foliage
// scene, as the game's own 0x2c013c0 does) and returns a log line for every
// word that changed in the scene header and in what its SsboGrassCut field
// (scene+0x58, 0x30 bytes) points to, two pointers deep, with where our Link
// stood. Called under LICHII_COOP_DEBUG_OBJECTS only.
// Field grass cuts (docs/world.md, "Field grass"). Every cut the game makes,
// a sword swing or an AI's OneshotTryCutFoliage, is a 0x3c-byte slot it
// queues on the foliage scene (main+0x4e22540 -> +0xf0 -> +0x338): slots at
// scene+0x48, the queue at scene+0x20 (u32 capacity, entries of 16 bytes at
// +0x28: slot pointer, sequence; u64 write index at +0x38). Look() sends our
// new slots as GRASS_CUT. Every game in the same scene makes the others'
// cuts through the game's own queueing call, 0xb80800(module, request),
// which copies the request into a free slot: Step() does that from the svc
// cave. A cut we made comes back in our own queue; it is not sent again.
inline constexpr std::uint64_t kFoliageModule = 0x4e22540;  // main + this: Module*
inline constexpr std::uint64_t kCutFoliage = 0xb80800;      // main + this
inline constexpr std::size_t kCutSlotSize = 0x3c;

class GrassSync {
public:
    using ReadWord = std::function<std::optional<std::uint64_t>(std::uint64_t)>;
    using Slot = std::array<std::uint8_t, kCutSlotSize>;

    // The slot <-> message (the 0.3 at +0x10 is the game's, set on each cut).
    static coop::protocol::GrassCut ToMessage(const Slot& slot, const std::string& scene);
    static Slot ToSlot(const coop::protocol::GrassCut& cut);

    // Objects walk: our own new cuts since the last look, as messages. The
    // first look in a foliage scene only notes where the queue is.
    std::vector<coop::protocol::GrassCut> Look(const ReadWord& read, std::uint64_t main,
                                               const std::string& scene, std::uint64_t now_ms);

    // Network thread: another game's cut, kept to be made while our scene is
    // the same (the scene is set by Look).
    void Receive(const coop::protocol::GrassCut& cut);

    // Svc cave (coop_svc.h AddCaller): makes the received cuts, at most
    // kCutsPerFrame a frame. Mallocs its request buffer once.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call,
                                         std::uint64_t result, std::uint64_t now_ms);

    std::uint64_t Made() const;
    std::vector<std::string> TakeNotes();

private:
    static constexpr std::size_t kCutsPerFrame = 8;
    static constexpr std::size_t kMaxWaiting = 64;
    static constexpr std::uint64_t kEchoMs = 3000;

    std::optional<CoopPuppet::Call> Next(CoopPuppet::Guest& guest, std::uint64_t now_ms);

    mutable std::mutex mutex_;
    // Look's side
    std::uint64_t scene_address_ = 0;
    std::optional<std::uint64_t> last_index_;
    // shared
    std::string scene_;
    std::uint64_t main_ = 0;
    std::deque<Slot> waiting_;
    std::deque<std::pair<std::uint64_t, Slot>> made_;  // (time ms, slot) for the echo
    std::uint64_t made_count_ = 0;
    std::vector<std::string> notes_;
    // Step's side
    std::uint64_t block_ = 0;
    bool allocating_ = false;
    std::uint64_t retry_ms_ = 0;
    std::size_t this_frame_ = 0;
};

class FoliageProbe {
public:
    // Reads 8 bytes of the game's memory; nothing when the address is bad.
    using ReadWord = std::function<std::optional<std::uint64_t>(std::uint64_t)>;

    std::vector<std::string> Look(const ReadWord& read, std::uint64_t main,
                                  const std::optional<std::array<float, 3>>& link);

private:
    struct Block {
        std::uint64_t address = 0;
        std::vector<std::uint64_t> words;
        std::vector<int> changes;  // per word; a word that changes all the time is left out
        int lines = 0;
    };
    void Compare(const ReadWord& read, const std::string& key, std::uint64_t address,
                 std::size_t count, const std::string& where, std::vector<std::string>& out);

    std::uint64_t scene_ = 0;
    std::map<std::string, Block> blocks_;
    int lines_ = 0;
    int calls_ = 0;
};

} // namespace CoopWorld
