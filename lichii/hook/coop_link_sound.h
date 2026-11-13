// SPDX-License-Identifier: GPL-3.0-or-later
//
// The other Links' sounds (issues 131 and 152, docs/puppet.md "Link sounds"):
// every sound the game itself starts on our own Link is heard from our Link in
// the other games, from the game's own calls on both sides.
//
// Sending. Every named sound the game starts goes through xlink2's
// searchAndEmit, main + 0x440630 (user, &asset, &handle, 0); on Hookshot that
// function is wrapped (patch 0014, HOS_COOP_WRAP_SOUND_EMIT). Before it runs,
// the module hands its user and asset cell here (OnEmit): when the user is our
// Link's own SLink user ([[[link + 0x270] + 0x68] + 0x78], set each walk with
// SetOurUser), the asset's name ([[cell]], a char*) is kept. Voice, footsteps,
// sword, shield, items, water: whatever the game itself plays on our Link.
// Each frame the kept names go out as LINK_SOUND (500) through a Limiter.
//
// Playing. A LINK_SOUND for a player goes into that player's Heard queue if
// their Link is near ours (InHearing); the puppet director then starts the
// same asset on the puppet's own SLink user with the game's own calls:
// 0x43db00 (accessor, &asset, name) once per name (the asset is kept), then
// 0x440630 (user, &asset, &handle, 0). The puppet's user is placed at the
// puppet, so the game itself makes it positional and fades it with distance.
// A name the puppet already plays (its own animation made it) is not started
// again. A Budget caps the starts per second for the whole game (16 players).
#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopPuppet {
class Guest;
struct Call;
}
namespace coop::host {
class Session;
}

namespace CoopLinkSound {

namespace addr {
inline constexpr std::uint64_t kEmit = 0x440630;         // (user, &asset, &handle, 0)
inline constexpr std::uint64_t kSearchAsset = 0x43db00;  // (accessor, &asset, name) -> bool
inline constexpr std::uint64_t kUserResource = 0x58;
inline constexpr std::uint64_t kResourceAccessor = 0x18;
}  // namespace addr

// Room in the puppet's block (CoopPuppet::block). The Ocarina notes' asset and
// text cells (coop_puppet_sound.h) are shared: the two never run at once.
namespace block {
inline constexpr std::uint64_t kHandle = 0x6f8;  // 8 bytes, after the notes' text
}  // namespace block

// Limits (one place, so the tests and docs/puppet.md say the same).
inline constexpr std::size_t kNameMax = 55;            // = coop::protocol::kMaxLinkSoundChars
inline constexpr std::size_t kCaughtMax = 32;          // kept between two frames, then dropped
inline constexpr std::uint64_t kRepeatMs = 80;         // the same name again this soon: not sent
inline constexpr std::uint32_t kSendPerSecond = 20;    // per player sent
inline constexpr float kHearDistance = 30.0f;          // game units (about metres)
inline constexpr std::size_t kHeardMax = 6;            // waiting per player; the oldest goes
inline constexpr std::uint64_t kHeardStaleMs = 400;    // older than this when it could play: dropped
inline constexpr std::uint32_t kPlayPerSecond = 24;    // started per second, all puppets together

// --- Sending ------------------------------------------------------------------

// Our Link's sound user (0: none, nothing is caught). The calc thread, each walk.
void SetOurUser(std::uint64_t user);
std::uint64_t OurUser();

// Reads guest memory; false when it cannot.
using Read = std::function<bool(std::uint64_t address, void* out, std::size_t size)>;

// The emit wrap's phase 0 (any game thread; quick, no game call): x0 the user,
// x1 the asset cell. Keeps the name when the user is ours and the name is one
// the protocol takes. Returns whether it kept one.
bool OnEmit(std::uint64_t user, std::uint64_t asset_cell, const Read& read);
// The names kept since the last call, oldest first.
std::vector<std::string> TakeCaught();
// Debug (LICHII_COOP_DEBUG_SOUND=1): every emit is counted by user and name,
// ours or not, and the first sighting of each pair is logged by the frame.
void SetProbe(bool on);
std::vector<std::string> TakeProbeLines();

// Each frame (the hook's present step): logs the probe's lines and sends the
// names caught since the last frame through one Limiter. Emulator and Hookshot
// builds only (LICHII_COOP).
void Frame(coop::host::Session* session, std::uint64_t now_ms);

// The name at [[cell]], if it is a name the protocol takes; else "".
std::string NameAt(std::uint64_t asset_cell, const Read& read);

// The sender's rule: the same name again within kRepeatMs is dropped (the
// game starting one sound twice in a frame), and at most kSendPerSecond
// names go out in any second.
class Limiter {
public:
    bool Allow(std::string_view name, std::uint64_t now_ms);

private:
    std::map<std::string, std::uint64_t, std::less<>> last_ms_;
    std::deque<std::uint64_t> sent_ms_;
};

// --- Receiving ----------------------------------------------------------------

// Whether a sound from a Link at `from` is heard by ours at `at` (no position
// known: not heard).
bool InHearing(const std::optional<std::array<float, 3>>& at, const std::array<float, 3>& from);

struct Sound {
    std::string name;
    std::uint64_t at_ms = 0;  // received (steady ms)
};

// One player's sounds waiting to be played on their puppet.
class Heard {
public:
    void Push(std::string name, std::uint64_t now_ms);
    void PushFront(Sound sound);  // one taken out that must wait (its age kept)
    // The next one still fresh, dropping stale ones.
    std::optional<Sound> Pop(std::uint64_t now_ms);
    bool Empty() const { return queue_.empty(); }
    void Clear() { queue_.clear(); }
    std::size_t Size() const { return queue_.size(); }

private:
    std::deque<Sound> queue_;
};

// The whole game's starts: at most kPlayPerSecond in any second.
class Budget {
public:
    bool Take(std::uint64_t now_ms);

private:
    std::deque<std::uint64_t> taken_ms_;
};

// One puppet's player: the search/emit steps and the assets found so far.
struct Player {
    enum class Step : std::uint8_t { none, search, emit };
    std::uint64_t actor = 0;   // the puppet; a new one forgets the assets
    std::uint64_t user = 0;    // its sound user the assets belong to
    Step pending = Step::none;
    std::string name;          // the sound being started
    bool found = false;        // its asset was just found: it is emitted as the next call
    std::map<std::string, std::uint64_t, std::less<>> assets;  // 0: not in its user
    Heard heard;
    bool missing_said = false;
};

using Say = std::function<void(const std::string&)>;

// The next call for this puppet's sounds, if any (one at a time). `playing`
// is what the puppet's user plays now (CoopPuppetSound::Playing), read only
// when a sound is about to start. `debug` logs each start.
std::optional<CoopPuppet::Call> Next(Player& player, Budget& budget, CoopPuppet::Guest& guest,
                                     std::uint64_t main_base, std::uint64_t actor,
                                     std::uint64_t user, std::uint64_t block, std::uint64_t now_ms,
                                     const std::function<std::string()>& playing,
                                     const std::string& who, const Say& say, bool debug);
// The result (x0) of the call Next gave.
void Finish(Player& player, std::uint64_t result, CoopPuppet::Guest& guest, std::uint64_t block,
            const std::string& who, const Say& say, bool debug);
// Whether `name` is one of the names in a ", "-joined Playing list.
bool AlreadyPlaying(std::string_view playing, std::string_view name);

} // namespace CoopLinkSound
