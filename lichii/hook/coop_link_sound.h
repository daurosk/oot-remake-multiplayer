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
//
// Effects (row 310, docs/puppet.md "Link effects"): the same path for our
// Link's ELink user ([[[link + 0x270] + 0x68] + 0x70], SetOurEffectUser): each
// key the game starts there goes out as LINK_EFFECT (501), and a sent key the
// user no longer plays (TrackOurEffects, every kTrackEveryMs) goes out again as
// an end. The receiver plays it on the puppet's own ELink user through a second
// Player with the effect cells (EffectCells), and kills it on the end.
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
inline constexpr std::uint64_t kHandle = 0x6f8;  // kHandleSlots handles of 8 bytes, after the notes' text
inline constexpr std::size_t kHandleSlots = 8;   // sounds that may still play per puppet
// The sound cells (= CoopPuppetSound::block::kAsset / kText, checked in the .cpp).
inline constexpr std::uint64_t kSoundAsset = 0x6b8;
inline constexpr std::uint64_t kSoundText = 0x6c0;
// The effects' own cells (row 310), after CoopPuppet::block::kDress's 0x100.
inline constexpr std::uint64_t kEffectAsset = 0x880;   // 8 bytes
inline constexpr std::uint64_t kEffectText = 0x888;    // kNameMax + 1 bytes
inline constexpr std::uint64_t kEffectHandle = 0x8c0;  // kHandleSlots handles of 8 bytes
inline constexpr std::uint64_t kEffectEnd = 0x900;
}  // namespace block

// Limits (one place, so the tests and docs/puppet.md say the same).
inline constexpr std::size_t kNameMax = 55;            // = coop::protocol::kMaxLinkSoundChars
inline constexpr std::size_t kCaughtMax = 32;          // kept between two frames, then dropped
inline constexpr std::uint64_t kRepeatMs = 80;         // the same name again this soon: not sent
inline constexpr std::uint32_t kSendPerSecond = 20;    // per player sent
inline constexpr float kHearDistance = 30.0f;          // game units (about metres)
inline constexpr std::size_t kHeardMax = 6;            // waiting per player; the oldest goes
inline constexpr std::uint64_t kHeardStaleMs = 400;    // older than this when it could play: dropped
inline constexpr std::uint64_t kLongestMs = 3000;      // a started sound is stopped after this (loops)
inline constexpr std::uint32_t kPlayPerSecond = 24;    // started per second, all puppets together
inline constexpr std::uint64_t kEffectLongestMs = 30000;  // an effect whose end never came is stopped after this
inline constexpr std::uint64_t kTrackEveryMs = 100;       // our effects user's playing list read this often
inline constexpr std::uint64_t kEndAfterMs = 300;         // a sent key is ended only once it was started this long ago
inline constexpr std::size_t kTrackedMax = 32;            // sent keys watched for their end
inline constexpr std::size_t kEndedMax = 8;               // ends waiting per player

// --- Sending ------------------------------------------------------------------

// The player's own sounds (tracker row 163): the game's system sounds (its
// SE_Sys_ bank, played flat, not at Link) that a few of Link's own assets
// start, which in the solo game only that player hears. The low-health beep
// HeartAlert (SE_Sys_HeartAlert, the one system sound in Player.bars; the
// game's own heart check, f_71004aaa80, starts it on our Link's sound user by
// name, and it ran only for the own Link: a puppet has no Player component),
// the hearts' and magic's HeartRecover / HeartReduce (UI_Static.bars) and the
// Lens of Truth's GlassTruth_* (ExpressionSound.bars SE_Sys_GlassTruth_*).
// Never sent, and never played on a puppet (a 0.11.7 or older sender still
// sends them).
bool OwnOnly(std::string_view name);

// Our Link's sound user (0: none, nothing is caught). The calc thread, each walk.
void SetOurUser(std::uint64_t user);
std::uint64_t OurUser();
// Our Link's effect (ELink) user, the same way (row 310).
void SetOurEffectUser(std::uint64_t user);
std::uint64_t OurEffectUser();

// Reads guest memory; false when it cannot.
using Read = std::function<bool(std::uint64_t address, void* out, std::size_t size)>;

// The emit wrap's phase 0 (any game thread; quick, no game call): x0 the user,
// x1 the asset cell. Keeps the name when the user is ours and the name is one
// the protocol takes. Returns whether it kept one.
bool OnEmit(std::uint64_t user, std::uint64_t asset_cell, const Read& read);
// The names kept since the last call, oldest first.
std::vector<std::string> TakeCaught();
// An effect key to send: a start, or with `end` the end of one sent before.
struct EffectOut {
    std::string name;
    bool end = false;
    bool operator==(const EffectOut&) const = default;
};
// The effect keys kept (and the ends found) since the last call, oldest first.
std::vector<EffectOut> TakeCaughtEffects();
// The calc thread, every kTrackEveryMs while our Link is known: `playing` is
// what our effect user plays now (CoopPuppetSound::Playing). A key caught and
// sent at least kEndAfterMs ago that is no longer in it gets an end.
void TrackOurEffects(std::string_view playing, std::uint64_t now_ms);
// The tracker itself (one per sender; TrackOurEffects uses the module's).
class EffectEnds {
public:
    void Started(std::string_view name, std::uint64_t now_ms);
    // The keys that ended (each once), given what the user plays now.
    std::vector<std::string> Check(std::string_view playing, std::uint64_t now_ms);
    std::size_t Size() const { return started_.size(); }

private:
    std::map<std::string, std::uint64_t, std::less<>> started_;
};
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
    bool effect = false;      // a LINK_EFFECT key (row 310), not a sound
    bool end = false;         // with effect: its end
};

// One player's sounds waiting to be played on their puppet.
class Heard {
public:
    void Push(std::string name, std::uint64_t now_ms);
    void PushFront(Sound sound);  // one taken out that must wait (its age kept)
    // The next one still fresh, dropping stale ones.
    std::optional<Sound> Pop(std::uint64_t now_ms);
    void Remove(std::string_view name);  // every waiting one of that name
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
    enum class Step : std::uint8_t { none, search, emit, kill };
    // Where in the puppet's block this player's calls keep their asset, the
    // name's text and the handles; how long a start may play before it is stopped.
    struct Cells {
        std::uint64_t asset = block::kSoundAsset;
        std::uint64_t text = block::kSoundText;
        std::uint64_t handle = block::kHandle;
        std::uint64_t longest_ms = kLongestMs;
        const char* what = "sound";
    };
    Cells cells;
    // Ends received (effects): the slot still playing that name is stopped first.
    std::deque<std::string> ended;
    // A sound started on the puppet: its handle cell is block::kHandle + 8 * slot.
    // The sender's loop end is not sent, so a looped asset (spin charge, swim)
    // is stopped by the game's own handle kill after kLongestMs, or when the
    // same name starts again; a one-shot has ended by then (the kill does nothing).
    struct Started {
        std::string name;  // empty: the slot is free
        std::uint64_t at_ms = 0;
    };
    std::uint64_t actor = 0;   // the puppet; a new one forgets the assets
    std::uint64_t user = 0;    // its sound user the assets belong to
    Step pending = Step::none;
    std::string name;          // the sound being started
    bool found = false;        // its asset was just found: it is emitted as the next call
    std::map<std::string, std::uint64_t, std::less<>> assets;  // 0: not in its user
    Heard heard;
    bool missing_said = false;
    std::array<Started, block::kHandleSlots> started{};
    std::size_t killing = 0;   // the slot of the kill call made
};

using Say = std::function<void(const std::string&)>;

// What each player's game plays on its Link now, from the LINK_EFFECTs received
// (row 310; the hand-over to puppet-pvp: its fallback area test reads it). Any
// thread. A key without an end is dropped after kEffectLongestMs.
struct Cast {
    std::string name;
    std::uint64_t at_ms = 0;  // its start was received (steady ms)
};
void NoteEffect(std::uint8_t player, std::string_view name, bool end, std::uint64_t now_ms);
std::vector<Cast> Casting(std::uint8_t player, std::uint64_t now_ms);

// The effect Player's cells (row 310).
Player::Cells EffectCells();
// A Player with them.
Player EffectPlayer();
// An end for `name` (effects): stops it if it still plays, and drops it from the queue. A name already waiting
// to end is not added twice; with kEndedMax waiting, an end for a key not started goes first (it stops nothing).
void End(Player& player, std::string name);
// Room for one more in a received queue of `max`: drops the OLDEST entry that is not an effect's end (an end
// lost leaves a held effect, the spin charge, on the puppet until kEffectLongestMs); only ends: the oldest.
void MakeRoom(std::vector<Sound>& queue, std::size_t max);

// The next call for this puppet's sounds, if any (one at a time). `playing`
// is what the puppet's user plays now (CoopPuppetSound::Playing), read only
// when a sound is about to start. `debug` logs each start.
std::optional<CoopPuppet::Call> Next(Player& player, Budget& budget, CoopPuppet::Guest& guest,
                                     std::uint64_t main_base, std::uint64_t actor,
                                     std::uint64_t user, std::uint64_t block, std::uint64_t now_ms,
                                     const std::function<std::string()>& playing,
                                     const std::string& who, const Say& say, bool debug);
// The call Next gave was not made (the cave's limit): the sound is lost, the next one goes.
void Dropped(Player& player);
// The puppet is made again (row 319 review): the next Next forgets the old user's assets even when the new actor
// and user reuse the old addresses.
void MadeAgain(Player& player);
// The result (x0) of the call Next gave.
void Finish(Player& player, std::uint64_t result, CoopPuppet::Guest& guest, std::uint64_t block,
            const std::string& who, const Say& say, bool debug);
// Whether `name` is one of the names in a ", "-joined Playing list.
bool AlreadyPlaying(std::string_view playing, std::string_view name);

} // namespace CoopLinkSound
