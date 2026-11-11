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
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "coop/host/flags.hpp"
#include "coop/protocol/messages.hpp"
#include "coop_objects_sync.h"
#include "coop_puppet.h"

namespace CoopWorld {

// True for the breakable world objects (by actor name).
bool IsBreakable(std::string_view name);

// True for the objects a player moves without breaking them: push and pull
// blocks of every dungeon (the MovableBox family, the Armos statue that is
// one, the slider box), and the placed rolling boulders (IsRoller).
bool IsMovable(std::string_view name);

// The placed boulders that roll on their own (issue 84: the Kokiri Sword
// crawlspace's Obj_Kokiri_RollingRock_A_01; the Fire Temple, Gerudo Training
// Ground, Spirit Temple, Water Temple and Ganon's Castle ones; 22 placements
// in all). Every game rolls its own, so they drift apart; shared as a
// movable: the game nearest to it that moves it first (the lower player
// number if two) sends its rolls, the others follow. Never "carried". Not
// the spawned Death Mountain Trail rocks (no placement hash) nor the
// RollingRockArea scenery.
bool IsRoller(std::string_view name);

// A door's lock (Obj_DoorChainLock_*, Obj_SpiritTemple_DoorChainLock_*; a door
// set places one on each side): it goes when opened and the opener's game
// saves that, so its break is kept (docs/dungeon.md, "Locked doors"). Not the
// Market shops' night locks; the boss doors' (ChainLockLocator_BossDoor) are no
// Obj_ actor. A lock that needs no small key gives none back: a key goes back
// only against our own SmallKeyNum drop.
bool IsLock(std::string_view name);

// The flag of `scene`'s small key count, DungeonInfo.<scene>.SmallKeyNum, or 0
// for a scene with no keys.
std::uint32_t SmallKeyFlag(std::string_view scene);

// Signs a sword cuts (their life never drops: WORLD_HIT shares the swing).
bool IsSign(std::string_view name);

// The anchor actor a push block keeps at its base (Obj_MovableBox_BasePosAnchor:
// no model, a dummy physics body, never placed in a map, so made by the
// block). Not a movable itself; moved with its block (issue 147).
bool IsBlockAnchor(std::string_view name);

// The push blocks whose own rest place a followed push fixes (issue 147):
// the MovableBox family and the slider box, not the Armos statue.
bool IsPushBlock(std::string_view name);

// Grass that is an actor: a tuft that is cut is deleted at once in the other
// games (writing life 0 did not remove it, run of 2026-11-09).
bool IsGrassActor(std::string_view name);

// Things a hit makes fall and that then stay, as ground or a bridge: the Fire
// Temple's and the Gerudo Training Ground's hammer pillars (the same actors,
// Obj_FireTemple_HammerSlipPillar_*, life
// Obj_HammerSlipPillar, destruction FallingRock). Shared by WorldSync like a
// breakable, but never moved and never deleted: another game's break writes
// life 0 so the game drops it here too (breakables, sync-coverage P1). Not
// IsBreakable: the enemy-style object sharing leaves them alone.
bool IsFaller(std::string_view name);

// A small key lying in a dungeon or the Gerudo Fortress (Obj_Dungeon_SmallKey*,
// not the minigame's). The game keeps no 64-bit key for these, so taking
// one is shared as a WORLD_BREAK: the others delete theirs at once, and a
// game that loads the room later gets the server's break (issue 102).
bool IsLyingKey(std::string_view name);

// The Gold Skulltula's token (Obj_Token_Gold, issue 89): made by the game
// where its Skulltula dies, with no placement. Its taking is shared like a
// lying key's, under TokenId(the Skulltula's placement hash): the token
// that appears within kTokenNearM of a Gold Skulltula we saw, or where a
// token of this scene was before (a room loaded again: its Skulltula is
// gone, the game makes the token again).
bool IsGoldToken(std::string_view name);
std::uint64_t TokenId(std::uint64_t skulltula_hash);
inline constexpr const char* kTokenCount = "PouchData.Item_Token_Gold.StockNum";

// A path piece that moves or tilts when hit and stays so, with no life to
// write: the Fire Temple's hammer drop floors, drop pillars and stair, the
// Deku Tree's falling ladder and its unkeyed falling floor, the Shadow
// Temple's unkeyed falling floor. Its new pose goes to the others as
// WORLD_PLACE (a move by a turn too, and from any game within reach), and
// the others hold it there, also a game that loads the room later (the
// server keeps the place).
bool IsShifter(std::string_view name);
// A shifter held for good at a place another game gave (its AI would put it
// back); the rest settle as blocks do.
bool HoldsPlace(std::string_view name);

// Everything WorldSync tracks: breakables, movables, fallers, lying keys and
// shifters. The objects walk hands these to it (coop_objects.cpp).
bool IsWorldObject(std::string_view name);

// The `DungeonInfo.<dungeon>.SmallKeyNum` count of the dungeon a scene is
// in ("ShadowTemple", "GerudoFortress_Adult"), or nothing.
std::optional<std::string> KeyCountOf(std::string_view scene);

// Give-backs (WorldSync::Out::give_back, count names) and taken keys
// (Out::taken_keys) from the objects' thread to the frame that writes the
// game's flags (coop_hook.cpp). Thread safe.
void QueueGiveBack(std::string count);
std::vector<std::string> TakeGiveBacks();
void QueueTakenKey(std::uint64_t key);
std::vector<std::uint64_t> TakeTakenKeys();

// One back in the count item `count` (KeyCountOf's, kTokenCount), never
// below 0 (the frame that writes flags). The flag watcher sends the -1 like
// any change, so the group's count rose once. Returns the log line.
std::string GiveBack(coop::host::FlagStore& game, std::string_view count);
// The 64-bit key `key` set to 1 in the game (a token's Skulltula). Returns
// the log line.
std::string SetTakenKey(coop::host::FlagStore& game, std::uint64_t key);

// Research for signs (issue 90; docs/world.md, "Signs: the probe"). A sign's
// cut is not in its life and its pack keeps no flag for it, and whether the
// replayed hit (WORLD_HIT) cuts the others' sign has not been seen. The probe
// answers both in one run: for each sign near our Link it keeps a copy of
// the actor (+0..+0x600), its component table (+0..+0x300) and the first
// 0x200 bytes of each component, taken again every second while nothing hit
// it. Words that change between two such copies are counters and are left
// out. When the sign is hit (our own swing passed it, or another player's
// hit was given to it), it logs every other word that changed, 0.5 s and
// 2 s later. The game that cut it and the game that got the hit should log
// the same words if the replay cuts; those words are the cut state.
class SignProbe {
public:
    using ReadWord = std::function<std::optional<std::uint64_t>(std::uint64_t)>;

    static constexpr std::size_t kActorWords = 0x600 / 8;
    static constexpr std::size_t kTableWords = 0x300 / 8;
    static constexpr std::size_t kComponentWords = 0x200 / 8;
    static constexpr std::uint64_t kActorTable = 0x270;    // as coop_objects.cpp
    static constexpr std::uint64_t kBaseEveryUs = 1'000'000;
    static constexpr std::uint64_t kBaseAgeUs = 250'000;   // our swing is read this late at most
    static constexpr std::uint64_t kFirstReportUs = 500'000;
    static constexpr std::uint64_t kLastReportUs = 2'000'000;
    static constexpr float kNearM = 12.0f;
    static constexpr std::size_t kMaxSigns = 3;            // read per look, the nearest
    static constexpr std::size_t kMaxShown = 24;           // changes per line
    static constexpr int kMaxLines = 200;                  // per session

    struct Sign {
        std::uint64_t address = 0;
        std::uint64_t hash = 0;
        std::string name;
        coop::protocol::Vec3 position{};
    };

    // A hit on this sign: `ours` when our swing passed it, else another
    // player's hit given to it.
    void Hit(const Sign& sign, bool ours, std::uint64_t now_us);

    // Our own cut of a sign (the first lines after the hit): the words it
    // changed, for SIGN_CUT.
    struct Cut {
        std::uint64_t address = 0;
        std::uint64_t hash = 0;
        std::vector<coop::protocol::SignWord> words;
    };

    // Once per objects walk with the signs the walk saw. Returns log lines;
    // our cuts go to `cuts` when given.
    std::vector<std::string> Look(const ReadWord& read, const std::vector<Sign>& signs,
                                  const std::optional<coop::protocol::Vec3>& player,
                                  std::uint64_t now_us, std::vector<Cut>* cuts = nullptr);

    void Reset();

    // Where a word was read, for logs: actor+0x.., table+0x.., c+0x2b8+0x...
    static std::string Where(std::uint64_t key);

    // The sign's component table and the component in table entry `entry`,
    // as the probe's latest copy of this sign has them (it read the whole
    // 0x200 bytes of that component): nothing when it has no such copy.
    struct Part {
        std::uint64_t table = 0;
        std::uint64_t component = 0;
    };
    std::optional<Part> PartOf(std::uint64_t address, std::uint64_t hash, std::uint64_t entry) const;
    bool Copied(std::uint64_t address, std::uint64_t hash) const;  // the probe has a copy

    using Snapshot = std::map<std::uint64_t, std::uint64_t>;  // key (Where) -> word
    // The actor's words, its component table and the first 0x200 bytes of
    // each component (keys as Where). Empty when the actor is not there.
    static Snapshot Read(const ReadWord& read, std::uint64_t address);

private:
    struct Base {
        std::uint64_t time_us = 0;
        Snapshot words;
    };
    struct Event {
        bool ours = false;
        std::uint64_t time_us = 0;
        int reported = 0;  // 0, 1 (first), 2 (last)
        std::optional<Base> base;
    };
    struct State {
        std::uint64_t hash = 0;
        std::string name;
        std::deque<Base> bases;  // newest last, at most 3
        std::set<std::uint64_t> noisy;
        std::optional<Event> event;
        std::uint64_t quiet_from_us = 0;  // a base taken from here on may add noise
    };

    const Snapshot* CopyOf(std::uint64_t address, std::uint64_t hash) const;
    // What of a cut can be written into another game's sign: component
    // words (not the Life hit record, not counters seen before the hit),
    // no heap pointers, no transforms of the pieces.
    static std::vector<coop::protocol::SignWord> CutWords(const Snapshot& before, const Snapshot& now,
                                                          const std::set<std::uint64_t>& noisy);
    std::string Report(const State& s, const Event& e, const Snapshot& now, std::uint64_t after_us);

    std::map<std::uint64_t, State> signs_;  // by actor address
    int lines_ = 0;
};

// Another player's cut word may go into our sign over `current` (what our
// sign has there now): never a heap pointer either way, never over a
// transform (two real floats), and two real floats only into an empty word.
bool SignWordWritable(std::uint64_t current, std::uint64_t value);

// The words a sign's cut changes (docs/world.md, "Signs: the cut words",
// from the run of 2026-11-10), as SIGN_CUT keys: only these are sent and
// written, into components the probe has read 0x200 bytes of.
bool SignCutKeyAllowed(std::uint64_t key);

// A dungeon gimmick: a switch, a water plane, a shutter, a torch, a lift or
// a moving floor (docs/dungeon.md "Live dungeon actions"); for the
// probe also the push blocks and their base anchor (issue 147), the falling
// ladder, its pillar and the falling floors (issue 129).
bool IsGimmick(std::string_view name);

// Probe for dungeon gimmicks (docs/dungeon.md "Live dungeon actions").
// A held floor switch, a timed switch or a water plane keeps its state in
// the actor, not in a flag, so it can only be shared live once the words
// that hold it are known. This finds them in a run, the way the sign probe
// found a sign's cut: every kEveryUs it copies the nearest gimmicks (as
// SignProbe::Read) and logs each word that stepped: equal in the two copies
// before, different now. A word that steps often (a timer, a counter, a
// moving part's transform) is muted after kNoisySteps steps. It only reads.
class GimmickProbe {
public:
    static constexpr std::uint64_t kEveryUs = 500'000;
    static constexpr float kNearM = 15.0f;
    static constexpr std::size_t kMaxGimmicks = 4;   // the nearest, per look
    static constexpr std::size_t kMaxShown = 16;     // steps per line
    static constexpr int kNoisySteps = 6;            // steps of one word before it is muted
    static constexpr int kMaxLines = 400;            // per session

    struct Gimmick {
        std::uint64_t address = 0;
        std::uint64_t hash = 0;
        std::string name;
        coop::protocol::Vec3 position{};
    };

    // Once per objects walk. Returns log lines.
    std::vector<std::string> Look(const SignProbe::ReadWord& read, const std::vector<Gimmick>& gimmicks,
                                  const std::optional<coop::protocol::Vec3>& player, std::uint64_t now_us);
    void Reset();

private:
    struct State {
        std::uint64_t hash = 0;
        std::string name;
        std::deque<SignProbe::Snapshot> copies;  // newest last, at most 2 kept before the new one
        std::map<std::uint64_t, int> steps;      // per word
    };
    std::map<std::uint64_t, State> states_;  // by actor address
    std::uint64_t last_us_ = 0;
    int lines_ = 0;
};
// Issue 147: a block another game pushed is warped to its new rest place
// here, but the block's own memory still holds where it rested before (its
// base or target), so our player's next push snaps it back there. RestWords
// looks through the block's memory for that old place, as three floats in a
// row or as the translation of a 3x4 matrix (a float every 16 bytes), and
// gives the writes that put the new place there instead:
// - the actor (+0..+0x600, not its own place +0x31c..+0x34b, which the
//   setter already wrote),
// - the first 0x200 bytes of each component its table (+0x270) points to,
// - the physics root ([[table + 0x50] + 0x20], first 0x400 bytes; docs/world.md).
// Only for a real move (5 cm or more) from a place that is not near the
// origin (zeros would match), each axis within 2 cm, never a match that is
// exactly 0.0 on an axis, and at most kMaxRestTriplets places: more is taken
// as a wrong match and nothing is written. The sizes searched are not known
// object sizes, so the glue writes them only with LICHII_COOP_BLOCK_REST=1
// (the integrator's test); by default it logs where they would go.
struct RestWrite {
    std::uint64_t address = 0;
    std::uint32_t value = 0;  // f32 bits
};
struct RestScan {
    std::vector<RestWrite> writes;
    std::vector<std::string> where;  // per place found: actor+0x.., c+0x50+0x.., physics root+0x.. (matrix)
    bool too_many = false;
};
inline constexpr std::size_t kMaxRestTriplets = 12;
inline constexpr float kRestMatchM = 0.02f;
RestScan RestWords(const std::function<std::optional<std::uint64_t>(std::uint64_t)>& read,
                   std::uint64_t actor, const coop::protocol::Vec3& from,
                   const coop::protocol::Vec3& to);

// Breaks and moves of placed world objects, shared by placement hash
// (docs/world.md, "Broken for everyone" and "Moved for everyone"). Beside the
// enemy sharing, which still carries a breakable's life: this adds what that
// cannot do.
// - WORLD_BREAK: a break seen here (life read at 0, or the object gone next
//   to our Link) is sent once; the server keeps it for the scene. A game
//   that gets one writes life 0 into its copy and deletes the copy if it
//   still stands after a wait; a copy first seen after the break (a player
//   who came later, an area loaded later) is deleted quietly.
// - WORLD_MOVE / WORLD_PLACE: the game whose own Link moves an object (a
//   block pushed, a pot or rock carried or thrown) sends where it is; the
//   others put their copy there. One mover at a time: while another game's
//   moves come in, ours are not sent, and when two start at once the lower
//   player number keeps it.
// No emulator code. Tested in tests/lichii/world_test.cpp.
class WorldSync {
public:
    static constexpr std::uint64_t kStaleUs = 1'500'000;      // a break this soon after we first saw the object is older than our sight of it
    static constexpr std::uint64_t kDeleteGrassUs = 400'000;  // life 0 written, still there: delete
    static constexpr std::uint64_t kDeleteUs = 6'000'000;     // (a crate takes 4.6 s to go)
    static constexpr std::uint64_t kFallerRetryUs = 1'000'000;  // a faller's life 0, again
    static constexpr float kLiftedM = 0.3f;      // a key this far from where it lay is being picked up
    static constexpr float kTokenNearM = 3.0f;    // a new token this near a Gold Skulltula is its
    static constexpr float kTokenSpotM = 1.5f;    // a token this near a known token spot is that one
    static constexpr float kShiftTurnRad = 0.05f;  // a shifter turned this far from its rest: moved
    static constexpr int kFallerTries = 10;  // then one "did not fall" line, and no more writes
    static constexpr std::uint64_t kGoneHoldUs = 700'000;     // a gone-near-Link break waits this long
    static constexpr std::uint64_t kRespawnUs = 3'000'000;    // back this long after it went: put back by the game
    static constexpr std::uint64_t kEchoUs = 2'000'000;       // our break this soon before theirs was theirs
    static constexpr int kMissedWalks = 2;
    static constexpr float kLockHoldM = 2.5f;  // another's lock this near our Link: not deleted
    static constexpr std::uint64_t kOwnDropUs = 8'000'000;  // a key spent this recently can go back
    static constexpr float kBreakNearM = 8.0f;   // as CoopObjects::Sync
    static constexpr float kBreakReachM = 3.0f;
    static constexpr int kUnloadCount = 4;       // this many gone together, one of them far: unloaded
    static constexpr float kMoverReachM = 15.0f; // only an object this near our Link is ours to move
    static constexpr float kBlockStartM = 0.05f; // moved this far from where it rested: being moved
    static constexpr float kLooseStartM = 0.15f; // the same for pots and rocks (they settle a little)
    static constexpr std::uint64_t kMoveEveryUs = 50'000;
    static constexpr std::uint64_t kRestUs = 400'000;        // still this long: at rest
    static constexpr std::uint64_t kRemoteHoldUs = 600'000;  // no move this long: the mover stopped
    static constexpr std::uint64_t kSettleUs = 500'000;      // a rest place is held this long
    static constexpr std::uint64_t kQuietUs = 1'500'000;     // after following: no claim while it settles
    static constexpr std::uint64_t kBehindUs = 100'000;      // copies show the mover this late
    static constexpr std::size_t kKeepSamples = 16;
    static constexpr float kSignReachM = 1.5f;  // a swing this near a sign (level) is sent
    static constexpr float kSignHighM = 2.5f;   // and no more than this above or below its foot
    static constexpr std::uint64_t kHitEveryUs = 300'000;  // per sign
    static constexpr std::uint64_t kHitWaitUs = 300'000;   // a sign just loaded is hit this late
    static constexpr std::uint64_t kCutAfterHitUs = 200'000;  // cut words this long after its hit
    static constexpr float kAnchorNearM = 0.5f;          // a block's base anchor is this near its old rest
    static constexpr std::uint64_t kAnchorHoldUs = 1'000'000;  // the anchor is put at the new rest this long

    struct Out {
        std::vector<coop::protocol::Message> send;
        // Small keys to give back to our game: the flag of the dungeon's
        // SmallKeyNum, once per lock this game opened after another already
        // had (docs/dungeon.md, "Locked doors").
        std::vector<std::uint32_t> key_refunds;
        std::vector<CoopObjects::LifeWrite> lives;
        // Another player's sword hits to give our copies (signs), through
        // the game's own damage call.
        struct Hit {
            std::uint64_t address = 0;
            coop::protocol::Vec3 at{};
            coop::protocol::Vec3 from{};
        };
        std::vector<Hit> hits;
        // Words of another player's cut to write into our sign (SIGN_CUT):
        // component table entry (key >> 32) - 2, byte offset key & 0xffffffff.
        struct Word {
            std::uint64_t address = 0;  // the actor
            std::uint64_t hash = 0;
            std::uint64_t key = 0;
            std::uint64_t value = 0;
            SignProbe::Part part;       // as the probe saw them
        };
        std::vector<Word> words;
        // Counts where we and another player took the same lying key or
        // token at once and theirs reached the server first: one goes back
        // each (KeyCountOf's, kTokenCount), so the shared count rose once.
        std::vector<std::string> give_back;
        // 64-bit keys to set to 1 here: the Gold Skulltula of a token
        // another player took, so a room loaded again has no token.
        std::vector<std::uint64_t> taken_keys;
        // Issue 147: a block another game moved has come to rest here. Its
        // memory still holds `from` (RestWords puts `to` there); `anchor`,
        // when not 0, is its base anchor actor found at `from`, which Poses
        // now puts at `to`.
        struct BlockFix {
            std::uint64_t address = 0;
            std::uint64_t hash = 0;
            std::string name;
            coop::protocol::Vec3 from{};
            coop::protocol::Vec3 to{};
            std::uint64_t anchor = 0;
        };
        std::vector<BlockFix> blocks;
        std::vector<std::string> notes;  // log lines
    };

    // Network thread: WORLD_BREAK, WORLD_MOVE, WORLD_PLACE. Others ignored.
    void Receive(const coop::protocol::Message& message);

    // Present thread, before Tick: our own game lowered this count (the
    // SmallKeyNum flag's hash; FlagWatcher::take_own_count_drops).
    void OwnCountDrop(std::uint32_t key_flag, std::uint64_t now_us);
    // Present thread: the host loaded a save (host-save's FLAG_RESET): kept
    // breaks (locks) are forgotten, so our locks come back with their rooms.
    void ForgetKept();

    // Present thread, once per walk, with every breakable and movable the
    // walk saw. `player` is our own Link, when the walk saw him.
    void Tick(const std::string& scene, std::uint8_t self, std::uint64_t now_us,
              const std::vector<CoopObjects::Seen>& seen,
              const std::optional<coop::protocol::Vec3>& player, Out& out);

    // Present thread, after Tick: one sword swing of our own Link, as the
    // game queued it for the grass (GrassSync::Look; sweep points NaN when
    // it was no sword). A sign it passed gets WORLD_HIT.
    void Swing(const coop::protocol::GrassCut& cut,
               const std::optional<coop::protocol::Vec3>& player, std::uint64_t now_us, Out& out);

    // Present thread, every frame: where the copies another game moves go.
    void Poses(std::uint64_t now_us, std::vector<CoopObjects::PoseWrite>& out) const;
    // This actor's place is ours to decide: the enemy sharing must not place it.
    bool Places(std::uint64_t address) const { return objects_.contains(address); }
    // The copies to delete (broken elsewhere).
    std::vector<std::uint64_t> Removes() const;

    // Present thread, after Swing: the sign probe (SignProbe) on the signs of
    // the last Tick. Our own cut's words go out as SIGN_CUT.
    void ProbeSigns(const SignProbe::ReadWord& read, std::uint64_t now_us, Out& out);

    void Reset();

    // Where one of another player's cut words goes in our sign, read again
    // just before the write: nothing unless the actor is still that sign,
    // its table and component are the ones the probe saw (pointers), the
    // key is on the list and the word may be written (SignWordWritable).
    std::optional<std::uint64_t> SignWordTarget(const SignProbe::ReadWord& read,
                                                const Out::Word& w) const;

    // For tests and logs.
    std::size_t Count() const { return objects_.size(); }
    bool Broken(std::uint64_t hash) const { return broken_.contains(hash); }
    std::uint8_t MoverOf(std::uint64_t hash) const;  // 0: nobody

private:
    struct Break {
        std::uint64_t got_us = 0;   // when we learned of it
        bool local = false;         // broken in this game
        bool kept = false;          // a lock: lasts, never put back
        bool refunded = false;      // our second key for it went back
        bool paid = false;          // opened here: one of our key drops paid for it
        bool seen_go = false;       // our own copy went (or never came) after it
        std::uint64_t gone_us = 0;  // when our copy went
        bool key = false;           // a lying key or token we took
        std::string count;          // its count, to give one back (may be empty)
    };
    struct Sample {
        std::uint64_t time_us = 0;
        coop::protocol::Vec3 position{};
        coop::protocol::Quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
    };
    struct Remote {  // another game's moves of one object
        std::uint8_t player = 0;
        std::uint64_t got_us = 0;  // our time of the latest move
        std::deque<Sample> samples;
        std::optional<Sample> place;  // its rest place (time_us: when we got it)
        std::uint32_t place_serial = 0;  // counts the rest places
    };
    struct Object {
        std::uint64_t address = 0;
        std::uint64_t hash = 0;
        std::string name;
        bool breakable = false;
        bool grass = false;
        bool sign = false;
        bool faller = false;  // IsFaller: falls and stays, never moved or deleted
        bool key = false;     // IsLyingKey or IsGoldToken: its taking is a break
        std::string count;    // the count it adds to (KeyCountOf, kTokenCount)
        bool shifter = false; // IsShifter
        bool hold = false;    // HoldsPlace: a given place is held for good
        std::optional<CoopObjects::Mat3> rest_rotation;  // with `rest`
        std::uint64_t hit_sent_us = 0;
        std::uint64_t hit_given_us = 0;
        std::optional<std::int32_t> life;
        coop::protocol::Vec3 position{};
        std::optional<CoopObjects::Mat3> rotation;
        coop::protocol::Vec3 first_position{};
        std::uint64_t first_us = 0;
        int missed = 0;
        bool sent_break = false;
        bool kept = false;  // a small-key door's lock (IsLock)
        // A break from elsewhere being made here.
        std::uint64_t life_written_us = 0;
        int faller_tries = 0;  // life 0 writes to a faller (kFallerTries)
        bool remove = false;
        // Moves.
        coop::protocol::Vec3 rest{};       // where it last rested for everyone
        bool ours = false;                 // we are sending its moves
        std::uint64_t moved_us = 0;        // last time it moved while ours
        std::uint64_t sent_us = 0;
        coop::protocol::Vec3 sent_position{};
        bool following = false;            // another game's moves are placed
        bool stirred = false;              // moved since the walk before
        bool roller = false;               // IsRoller: rolls on its own
        std::uint32_t place_seen = 0;      // the rest place we last took (Remote::place_serial)
        std::optional<coop::protocol::Vec3> follow_from;  // where it rested before another game moved it
        std::uint64_t settle_us = 0;       // when we took it
        std::uint64_t quiet_until = 0;
    };
    struct Gone {  // left the walk near our Link: a break unless it was an unload
        Object object;
        std::uint64_t at_us = 0;
        bool refund = false;  // another game's lock we held: our second key, if we spent one
    };

    void Drain(std::uint64_t now_us);
    // A lying small key we took that has not gone out as our break yet.
    std::optional<std::string> TookKey(std::uint64_t hash) const;  // its count
    // A token's id (TokenId) from where it is, or 0: not known.
    std::uint64_t IdentifyToken(const coop::protocol::Vec3& at);
    // Another player's break of a token we know: its spot is kept, and its
    // Skulltula's key is set here (Out::taken_keys).
    void Token(const coop::protocol::WorldBreak& b);
    void LocalBreak(Object& o, std::uint64_t now_us, const char* how, Out& out);
    void MakeBreak(Object& o, const Break& b, std::uint64_t now_us, Out& out);
    // One small key back to our game for a lock opened twice (marks `b`),
    // only against a drop of our own key count in the last kOwnDropUs (that
    // drop is then used up).
    void Refund(Break& b, std::uint64_t hash, std::uint64_t now_us, const char* why);
    // Takes one of our own key drops for this scene out (it paid for a lock).
    bool TakeOwnDrop(std::uint64_t now_us);
    void Move(Object& o, std::uint8_t self, std::uint64_t now_us,
              const std::optional<coop::protocol::Vec3>& player, Out& out);
    std::optional<Sample> PoseAt(const Remote& r, std::uint64_t now_us) const;

    std::string scene_;
    std::uint64_t scene_since_ = 0;
    std::map<std::uint64_t, Object> objects_;  // by actor address
    std::map<std::uint64_t, Break> broken_;    // by placement hash, this scene
    std::vector<std::uint32_t> refunds_;       // keys to give back, for the next Out
    std::vector<std::string> refund_notes_;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> own_drops_;  // (key flag, when)
    bool no_key_logged_ = false;
    std::map<std::uint64_t, Remote> remote_;   // by placement hash, this scene
    std::vector<Gone> gone_;
    std::vector<std::string> give_back_;  // from Drain, for the next Tick's Out
    std::vector<std::uint64_t> taken_keys_;  // the same
    // Gold Skulltulas of this scene by placement hash: where we saw them last.
    std::map<std::uint64_t, coop::protocol::Vec3> skulltulas_;
    // Tokens by scene, then id: where they were. Kept across scene changes,
    // so a token the game makes again on a reload is known.
    std::map<std::string, std::map<std::uint64_t, coop::protocol::Vec3>> token_spots_;
    struct Anchor {
        std::uint64_t address = 0;
        coop::protocol::Vec3 position{};
    };
    std::vector<Anchor> anchors_;  // block base anchors in the last walk
    struct AnchorHold {
        coop::protocol::Vec3 to{};
        std::uint64_t until_us = 0;
    };
    std::map<std::uint64_t, AnchorHold> anchor_holds_;  // by actor address
    // Hits for signs, until our copy is there to take them (by hash).
    std::map<std::uint64_t, std::vector<std::pair<coop::protocol::Vec3, coop::protocol::Vec3>>> hits_;
    // Another player's cut words for signs (SIGN_CUT), until our copy is there (by hash).
    std::map<std::uint64_t, std::vector<coop::protocol::SignWord>> cuts_;
    std::vector<std::pair<std::uint64_t, bool>> went_;  // (time, far) of everything that left lately
    std::vector<coop::protocol::Message> early_;  // for a scene we are not in yet
    std::uint8_t self_ = 0;
    std::optional<coop::protocol::Vec3> player_;  // our Link at the last Tick
    SignProbe probe_;

    mutable std::mutex inbox_mutex_;
    std::vector<coop::protocol::Message> inbox_;
};
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
    // the same (the scene is set by Look). The server keeps each scene's cuts
    // and sends them again to a game that enters it (docs/world.md).
    void Receive(const coop::protocol::GrassCut& cut);

    // Svc cave (coop_svc.h AddCaller): makes the received cuts, at most
    // kCutsPerFrame a frame. Mallocs its request buffer once.
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call,
                                         std::uint64_t result, std::uint64_t now_ms);

    std::uint64_t Made() const;
    std::vector<std::string> TakeNotes();

private:
    static constexpr std::size_t kCutsPerFrame = 8;
    static constexpr std::size_t kMaxWaiting = coop::protocol::kMaxGrassCuts;
    static constexpr std::uint64_t kEchoMs = 3000;
    static constexpr std::uint64_t kSettleMs = 2000;

    std::optional<CoopPuppet::Call> Next(CoopPuppet::Guest& guest, std::uint64_t now_ms);
    void SetReady(bool ready);

    struct Waiting {
        std::string scene;
        Slot slot;
    };

    mutable std::mutex mutex_;
    // Look's side
    std::uint64_t scene_address_ = 0;
    std::optional<std::uint64_t> last_index_;
    std::string settle_scene_;
    std::uint64_t settle_ms_ = 0;
    // shared
    std::string scene_;
    std::uint64_t main_ = 0;
    std::deque<Waiting> waiting_;
    bool ready_ = false;  // the walk sees our scene's foliage queue
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
