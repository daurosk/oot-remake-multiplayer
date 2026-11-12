// SPDX-License-Identifier: GPL-3.0-or-later
//
// LICHII_COOP_DEBUG_OBJECTS=1: logs the game's actors as they come and go,
// and enemy life, for docs/objects.md ("Live check").
//
// The actor list, read live (build e0f3ccfaf50065ae):
//   manager  = [main + 0x4e91888]
//   head     = [manager + 0x118] + 0x670, a circular doubly linked list;
//              the u32 at head + 0x10 is its length
//   node     = {next +0, prev +8, owner +0x10}; an actor's own node is at
//              actor + 0x538, so owner == node - 0x538 for actors (a few
//              other objects share the list and are skipped)
// Actor fields: name char* +0x260, state u32 +0x2c, position 3 x f32 +0x31c,
// placement hash u64 at [+0x3d8] + 0x88 (~0 for actors the game made),
// life component = [[+0x270] + 0x2b8], life s32 at [life + 0x1c10] + 8,
// max at [life + 0x1c18] + 8.
//
// LICHII_COOP_OBJECTS_CHEAT=1 (test only, for the stage 5 live checks): keeps
// the bow, the small quiver with 30 arrows, the small bomb bag and 20 bombs in
// the local player's pouch, topped up once a second through the game's own
// flag write queue (docs/items.md hashes). It touches nothing else.
//
// LICHII_COOP_OBJECTS=1 shares enemies with the other games (Share and
// OnEvent below; the rules are in coop_objects_sync.h). Off by default until
// the two-game test passes. With it:
// - copies we do not own are placed every game frame where the owner's enemy
//   was, by the game's own setter on its calc thread (Mover; needs the svc
//   cave, so LICHII_COOP_PUPPET=1 or LICHII_COOP_SPAWN=1);
//   LICHII_COOP_OBJECTS_POSE=0 stops it;
// - a pickup another game took (a key64 flag set to 1) is deleted here;
// - a life a copy must lose is given to the game as damage (added to the s32
//   at Life + 0x228, which the Life step 0x49d8c0 sums into Life + 0x1cb0 and
//   takes off life), so the game reacts to it as to a hit;
//   that alone is not taken (LICHII_COOP_OBJECTS_HURT=1 still tries it), so
//   an enemy gets a whole hit record (kHitTemplate; on by default,
//   LICHII_COOP_OBJECTS_HIT_REPLAY=0 writes life directly instead);
// - a copy near our Link whose own AI plays another animation than its
//   owner's is asked for (issue 132; LICHII_COOP_OBJECTS_WAKE=0 stops it);
// LICHII_COOP_OBJECTS_TEST_DRIVE=1 (two-game tests only): on the game that
// LICHII_COOP_OBJECTS_TEST_TAKE=<hex key> (tests only): on player 2's game,
// 3 s after sharing starts, acts as if that 64-bit placement key arrived
// set to 1, so the actor with that placement hash is deleted here.
// owns it (player 1 only), one enemy (a ReDead if there is one) is held 3 m in front of our
// player, swaying 1.5 m side to side, and hurt by 1 every 4 s through the
// damage field. The other game's copy should do the same on its screen.
//
// This runs on the present thread while the guest runs, so a walk can see the
// list mid-change. A walk whose length differs from the head's count is
// dropped, and an actor counts as gone only when two walks in a row miss it.

#include "coop_objects.h"
#include "coop_every.h"
#include "coop_cheat_items.h"
#include "coop_flags.h"
#include "coop_hold.h"
#include "coop_puppet.h"
#include "coop_pvp_game.h"
#include "coop_svc.h"

#include <algorithm>
#include <utility>
#include <tuple>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "coop/host/game_flags.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_objects_sync.h"
#include "coop_ocarina.h"
#include "coop_spawn.h"
#include "coop_pickups.h"
#include "coop_projectiles.h"
#include "coop_guard.h"
#include <atomic>
#include <unordered_map>
#include "coop_world.h"
#include "coop_scrubs.h"
#include <set>

namespace CoopObjects {
void NoteBosses(const std::vector<Seen>& seen);  // issue 109, below
namespace {

constexpr u64 kOcarinaProgramId = 0x0400486024DEC000ULL;
constexpr std::array<u8, 8> kGameBuildId{0xe0, 0xf3, 0xcc, 0xfa, 0xf5, 0x00, 0x65, 0xae};

constexpr u64 kActorManagerPtr = 0x4e91888;  // main offset
constexpr u64 kManagerLists = 0x118;
constexpr u64 kListHead = 0x670;
constexpr u64 kListCount = 0x10;
constexpr u64 kNodeOwner = 0x10;
constexpr u64 kActorNode = 0x538;
constexpr u64 kActorName = 0x260;
constexpr u64 kActorState = 0x2c;
constexpr u64 kActorPosition = 0x31c;
constexpr u64 kActorRotation = 0x328;  // 9 x f32, rows of 3
constexpr u64 kLifeHurt = 0x228;       // s32 damage this frame (0x49dc90)
constexpr u32 kStateAlive = 4;
// What the game sets on an actor about 25 m or more from its Link (docs/
// objects.md, "States seen"). A copy asleep at its home is still moved to
// its owner's place when that is this near our Link (issue 101), so an
// enemy chasing another player to us comes along and the game wakes it.
constexpr u32 kStateAsleep = 6;
constexpr float kAsleepPlaceM = 30.0f;
float Apart(const coop::protocol::Vec3& a, const coop::protocol::Vec3& b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
// The game's own way to move an actor, as the AI node ExecuteActorForceSetMtx
// (run 0x2622e50) does it: write +0x31c..+0x34b (place, then the 3x3), then
// call vtable slot 0x218 (actor, 0). Every actor seen shares the vtable
// 0x4b25e18, whose slot 0x218 is 0x4c6580: it sets bit 23 of +0x4a8 and hands
// the new transform to the physics component (0x4c6630). Checked live: a
// plain write of +0x31c is undone by the game within a frame.
constexpr u64 kActorVtable = 0x4b25e18;
constexpr u64 kSetMatrix = 0x4c6580;
// Issues 150, 157, 101 (on unless LICHII_COOP_OBJECTS_RELEASE=0): the actor flags word the
// setter above marks (bit 23, docs/world.md). An enemy that becomes ours gets
// the bit cleared once, in case the game leaves it set after the Mover's
// last move and the body stays frozen or floats. Not proven.
constexpr u64 kActorFlags = 0x4a8;
constexpr u32 kWarpBit = u32{1} << 23;
constexpr u64 kRequestDelete = 0xa6490;  // (actor, 0), as docs/puppet.md
// Rolled enemies' copies in the spawn director: their keys have the top bit.
constexpr u64 kEnemyKey = u64{1} << 63;
constexpr unsigned kMoverCallerOrder = 30;  // after the puppets, spawn copies (10) and fairies (20)
constexpr unsigned kGrassCallerOrder = 40;  // the others' field grass cuts (coop_world.h)

CoopWorld::GrassSync& TheGrass() {
    static CoopWorld::GrassSync grass;
    return grass;
}

// Issue 149 (coop_scrubs.h): the Scrub Brothers' hits. The present thread
// looks and receives, the calc thread's cave gives the others' hits.
constexpr int kScrubsCallerOrder = 62;

std::string EnvText(const char* name);  // below EnvOn  // after the live events (60) and the teleport's (61)

struct ScrubsBox {
    std::mutex mutex;
    CoopScrubs::Puzzle puzzle{HitAnimations()};
    u64 main = 0;  // the game's main, for the cave

    static std::set<std::string> HitAnimations() {
        const std::string names = EnvText("LICHII_COOP_SCRUB_HIT_ANIMS");
        auto parsed = CoopScrubs::ParseAnimations(names);
        return parsed.empty() ? CoopScrubs::DefaultHitAnimations() : parsed;
    }
};

ScrubsBox& TheScrubs() {
    static ScrubsBox box;
    return box;
}

u64 SteadyNowMs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}
// Animations (docs/puppet.md "Animation"): the AS controller is
// [[[actor + 0x270] + 0x40] + 0x18] + 0x18; slot 0 is [controller + 0x20];
// its command name char* +0x10, rate f32 +0xd4, frame f32 +0xe8. The play
// request for 0x2022a0: +0 actor, +8 name, +0x10 slot, +0x14 rate, +0x18 and
// +0x1c -1.0, +0x20 skip-if-same, +0x28 node.
constexpr u64 kComponentsAs = 0x40;
constexpr u64 kAsInner = 0x18;
constexpr u64 kAsController = 0x18;
constexpr u64 kAsSlotCount = 0x18;
constexpr u64 kAsSlotArray = 0x20;
constexpr u64 kAsSlotEntry = 0x48;  // array entries; the first 8 bytes point to the slot (docs/puppet.md)
constexpr u64 kAsSlotName = 0x10;
constexpr u64 kAsSlotRate = 0xd4;
constexpr u64 kAsSlotFrame = 0xe8;
constexpr u64 kPlayAs = 0x2022a0;
constexpr u64 kSetAsFrame = 0x37b5d70;  // (controller, slot, s0 = frame), as the puppet
constexpr float kFrameSlack = 3.0f;     // frames off before a copy's frame is set
constexpr u64 kMalloc = 0x14c6c90;
// The mover's block from the game's malloc: the play request, then one
// fixed cell per animation name (a slot may keep the name's pointer).
constexpr u64 kBlockRequest = 0x0;
constexpr u64 kBlockNames = 0x40;
constexpr u64 kNameCell = 0x50;
constexpr u64 kNameCells = 400;
constexpr u64 kBlockSize = kBlockNames + kNameCell * kNameCells;
constexpr u64 kPlayLogMs = 1000;  // a copy's replays of one name: one log line a second
// LICHII_COOP_OBJECTS_HIT_REPLAY=1 (experiment): a life a copy must lose is
// given to the game as a whole hit, written into the copy's Life component
// on the calc thread before its step, shaped like the one a Kokiri Sword hit
// left on a ReDead (live dump, 2026-11-09): the attack header at +0x1f8, the
// amount at +0x224/+0x228, and the incoming hit at +0x240..+0x2d8 (attacker
// actor +0x240, hit id +0x248, present +0x24c, attack kind +0x250, the hit's
// place +0x27c, the attacker's place +0x2a0). The list links from +0x2d8 on
// are left alone. Our own player stands in as the attacker.
constexpr std::array<u32, 0x98 / 4> kHitTemplate{
    0x472d7288, 0x00000021, 0x000000b9, 0x00000001,  // +0x240 attacker, id, present
    0x00060001, 0x40040000, 0x00000014, 0x00000000,  // +0x250
    0x00220000, 0x00001000, 0x0000000a, 0x00000000,  // +0x260
    0x00000200, 0x00000000, 0x00000000, 0xc21085f6,  // +0x270, hit place from +0x27c
    0x42c63e88, 0x40501269, 0x3e76ed80, 0xb3c00000,  // +0x280
    0xbf787210, 0x40588db0, 0xc1129418, 0xc0cb90a0,  // +0x290
    0xc210600c, 0x42c45791, 0x409352f5, 0xbdcc4eb0,  // +0x2a0 attacker place
    0x3ecc555c, 0xc0d481b8, 0x000343ff, 0x00040000,  // +0x2b0
    0x00000004, 0x000000be, 0x00000002, 0x03000000,  // +0x2c0 (+0x2c4 id again)
    0x03010102, 0x00030000,                          // +0x2d0
};
constexpr u64 kHitRecord = 0x240;
constexpr u64 kHitLateMs = 200;  // under Sync::kHurtWaitUs
constexpr u64 kHitHeader = 0x1f8;
constexpr u64 kHitAmount = 0x224;
constexpr u64 kLastHitId = 0x1b18;
  // the copy's own AI took over: play again at most this often
constexpr u64 kDriftLogUs = 5'000'000;
constexpr u64 kActorComponents = 0x270;
// LICHII_COOP_OBJECTS_AI_HOLD=1 or 2 (off by default; docs/objects.md
// "Holding a copy's AI", coop_hold.h): the BSA component is table +0xb0
// (0xcad060), and bit 0 of its u16 at +0xc4 is what QueryBsaIsBrainSkip (run
// 0x29ce250) reads. Each game frame the Mover writes the held copies' BSA
// components into the table the hold cave reads, and counts in how many
// frames each one's bit read as asked.
constexpr u64 kComponentBsa = 0xb0;
constexpr u64 kBsaFlags = 0xc4;
constexpr u64 kComponentLife = 0x2b8;
constexpr u64 kLifeValue = 0x1c10;
constexpr u64 kLifeMax = 0x1c18;
constexpr u64 kLifeCopy = 0x1ca8;  // life and max as last mirrored
constexpr u64 kActorPlacement = 0x3d8;
constexpr u64 kPlacementHash = 0x88;
constexpr std::size_t kMaxWalk = 8192;

// LICHII_COOP_OBJECTS_CHEAT: what it sets is in coop_cheat_items.h.

// Keys to give back for locks opened twice (WorldSync::Out::key_refunds).
std::mutex& KeyRefundMutex() {
    static std::mutex m;
    return m;
}
std::vector<std::uint32_t>& KeyRefundQueue() {
    static std::vector<std::uint32_t> q;
    return q;
}
std::vector<std::uint32_t>& OwnDropQueue() {  // our own key spends, for WorldSync
    static std::vector<std::uint32_t> q;
    return q;
}
bool& ForgetKeptFlag() {  // a host save load: WorldSync::ForgetKept before the next walk
    static bool f = false;
    return f;
}
std::vector<std::string>& TestKillQueue() {  // the test script's kill steps (QueueTestKill)
    static std::vector<std::string> q;
    return q;
}

bool EnvFlag(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return false;
    }
    const bool on = value[0] != '\0' && std::string_view(value) != "0";
    std::free(value);
    return on;
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::string_view(value) != "0";
#endif
}

// The variable as a hex number (0 when unset or not one).
u64 EnvHex(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return 0;
    }
    const u64 n = std::strtoull(value, nullptr, 16);
    std::free(value);
    return n;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? 0 : std::strtoull(value, nullptr, 16);
#endif
}

// The variable's number (0 when unset or not a number).
int EnvNumber(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return 0;
    }
    const int n = std::atoi(value);
    std::free(value);
    return n;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? 0 : std::atoi(value);
#endif
}

// Unset or anything but "0": on.
bool EnvOn(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return true;
    }
    const bool on = std::string_view(value) != "0";
    std::free(value);
    return on;
#else
    const char* value = std::getenv(name);
    return value == nullptr || std::string_view(value) != "0";
#endif
}

// Unset: empty.
std::string EnvText(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string text(value);
    std::free(value);
    return text;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
#endif
}

struct Actor {
    std::string name;
    u64 hash = 0;
    std::optional<std::array<s32, 4>> life;  // life, max, copy of life, copy of max
    unsigned missed = 0;
};

class Reader {
public:
    explicit Reader(Core::Memory::Memory& memory) : memory_(memory) {}

    template <typename T>
    std::optional<T> Get(u64 address) {
        if (address < 0x1000 || !memory_.IsValidVirtualAddressRange(address, sizeof(T))) {
            return std::nullopt;
        }
        T value{};
        memory_.ReadBlockUnsafe(address, &value, sizeof(T));
        return value;
    }

    std::optional<u64> Ptr(u64 address) {
        auto p = Get<u64>(address);
        if (!p || *p == 0) {
            return std::nullopt;
        }
        return p;
    }

    std::string Name(u64 actor) {
        const auto p = Ptr(actor + kActorName);
        if (!p) {
            return "?";
        }
        std::array<char, 64> buf{};
        if (!memory_.IsValidVirtualAddressRange(*p, buf.size())) {
            return "?";
        }
        memory_.ReadBlockUnsafe(*p, buf.data(), buf.size() - 1);
        return std::string(buf.data(), strnlen(buf.data(), buf.size()));
    }

private:
    Core::Memory::Memory& memory_;
};

// The game's calc thread, at the svc cave (coop_svc.h AddCaller): moves the
// copies with the game's own setter and deletes the taken pickups. The
// present thread hands it the latest wish list; each game frame it works
// through that list, one game call per pose or delete.
class Mover {
public:
    struct Pose {
        u64 actor = 0;
        std::array<float, 12> transform{};  // place, then the 3x3 rows
        bool rotation = false;              // false: keep the actor's own
        bool asleep = false;                // state 6 may be moved too (issue 101)
    };

    struct Hit {
        u64 actor = 0;      // the copy
        u64 attacker = 0;   // our player
        s32 amount = 0;
        std::array<float, 3> at{};    // the copy's place
        std::array<float, 3> from{};  // the hitter's place (ours when not known)
        u64 asked_ms = 0;             // set by AddHit
        bool exact = false;           // `at` is where the blade was, not the actor's foot
    };

    // Present thread: hits to replay at the next game frame.
    void AddHit(const Hit& hit) {
        std::lock_guard lock(mutex_);
        if (hits_.size() < 64) {
            hits_.push_back(hit);
            hits_.back().asked_ms = SteadyMs();
        }
    }

    // Present thread: the enemy copies whose AI is held, and how.
    void Hold(std::vector<u64> actors, const CoopHold::Masks& masks) {
        std::lock_guard lock(mutex_);
        holds_ = std::move(actors);
        masks_ = masks;
        holding_ = true;
    }

    // Present thread: copies that just became ours, to clear the setter's
    // bit of at the next game frame (issues 150, 157; on unless
    // LICHII_COOP_OBJECTS_RELEASE=0).
    void Release(const std::vector<u64>& actors) {
        std::lock_guard lock(mutex_);
        for (const u64 a : actors) {
            if (releases_.size() < 64) {
                releases_.push_back(a);
            } else {
                ++release_dropped_;
            }
            // A pose wished before it became ours would set the bit again
            // in the same frame: it is no copy any more.
            std::erase_if(poses_, [a](const Pose& p) { return p.actor == a; });
            // Issue 157 (peahat autotest 2026-11-12): the Peahat's own AI
            // started 'TakeOffAnticipation' for our Link, the copy was put
            // back to 'GroundWait', and once it became ours it never tried
            // again: its AI waits for the end of a play we took away. Give
            // it that play back once.
            if (const auto w = wanted_.find(a); w != wanted_.end()) {
                if (SteadyMs() - w->second.second <= kWakeReplayMs && wakes_.size() < 64) {
                    wakes_.emplace_back(a, w->second.first);
                }
                wanted_.erase(w);
            }
        }
    }

    // Releases made since the last ask: bit found set and cleared, and not.
    // Also the ones dropped because the frame's list was full.
    std::tuple<std::size_t, std::size_t, std::size_t> TakeReleases() {
        std::lock_guard lock(mutex_);
        return {std::exchange(released_, 0), std::exchange(release_clear_, 0),
                std::exchange(release_dropped_, 0)};
    }

    void Want(u64 main, std::vector<Pose> poses, std::vector<u64> removes,
              std::vector<PlayWish> plays) {
        std::lock_guard lock(mutex_);
        main_ = main;
        poses_ = std::move(poses);
        removes_ = std::move(removes);
        plays_ = std::move(plays);
        want_ms_ = SteadyMs();
    }

    // Calls made since the last ask (for the 5 s log line).
    std::array<std::size_t, 4> TakeCounts() {
        std::lock_guard lock(mutex_);
        const std::array<std::size_t, 4> counts{moved_, deleted_, played_, framed_};
        moved_ = 0;
        deleted_ = 0;
        played_ = 0;
        framed_ = 0;
        return counts;
    }

    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call,
                                         std::uint64_t result, std::uint64_t now_ms) {
        std::lock_guard lock(mutex_);
        if (after_call && allocating_) {
            allocating_ = false;
            block_ = result;
            return std::nullopt;  // next frame
        }
        if (after_call && frame_after_) {
            // The play just made: start it at the owner's frame, not at 0.
            const auto [actor, index, frame] = *frame_after_;
            frame_after_.reset();
            if (const u64 controller = Controller(guest, actor); controller != 0) {
                return SetFrame(controller, frame, index);
            }
        }
        if (!after_call) {
            for (const u64 actor : std::exchange(releases_, {})) {
                u32 flags = 0;
                if (!Alive(guest, actor) || !guest.Read(actor + kActorFlags, &flags, 4)) {
                    continue;
                }
                if ((flags & kWarpBit) != 0) {
                    flags &= ~kWarpBit;
                    guest.Write(actor + kActorFlags, &flags, 4);
                    ++released_;
                } else {
                    ++release_clear_;
                }
            }
            if (block_ == 0 && main_ != 0 && !allocating_ && !plays_.empty() && !alloc_tried_) {
                allocating_ = true;
                alloc_tried_ = true;
                CoopPuppet::Call c;
                c.function = main_ + kMalloc;
                c.x[0] = kBlockSize;
                return c;
            }
            work_plays_ = plays_;
            next_play_ = 0;
            work_wakes_ = std::exchange(wakes_, {});
            next_wake_ = 0;
            for (const Hit& h : hits_) {
                // A hit that waited too long (no game frame ran) is dropped:
                // by then its life has been written directly.
                if (now_ms < h.asked_ms + kHitLateMs) {
                    Replay(guest, h);
                }
            }
            hits_.clear();
            if (holding_) {
                WriteHolds(guest);
            }
            work_ = poses_;
            if (work_.size() > 1) {
                std::rotate(work_.begin(), work_.begin() + (++turn_ % work_.size()), work_.end());
            }
            work_removes_.clear();
            for (const u64 a : removes_) {
                if (!asked_.contains(a)) {
                    work_removes_.push_back(a);
                }
            }
            if (removes_.empty()) {
                asked_.clear();
            }
            next_ = 0;
            next_remove_ = 0;
        }
        if (main_ == 0) {
            return std::nullopt;
        }
        while (next_wake_ < work_wakes_.size() && block_ != 0) {
            const auto [actor, name] = work_wakes_[next_wake_++];
            if (!Alive(guest, actor)) {
                continue;
            }
            const u64 controller = Controller(guest, actor);
            if (controller == 0 || SlotName(guest, controller) == name) {
                continue;  // its AI already plays it again
            }
            const u64 text = Cell(guest, name);
            if (text == 0) {
                continue;
            }
            std::array<std::uint8_t, 0x30> request{};
            const float unset = -1.0f;
            const float rate = 1.0f;
            const std::int32_t index = 0;
            std::memcpy(request.data() + 0x00, &actor, 8);
            std::memcpy(request.data() + 0x08, &text, 8);
            std::memcpy(request.data() + 0x10, &index, 4);
            std::memcpy(request.data() + 0x14, &rate, 4);
            std::memcpy(request.data() + 0x18, &unset, 4);
            std::memcpy(request.data() + 0x1c, &unset, 4);
            if (!guest.Write(block_ + kBlockRequest, request.data(), request.size())) {
                continue;
            }
            played_at_.erase(actor);
            ++woken_;
            LOG_INFO(Core, "coop: objects: {:#x} became ours: plays its own AI's '{}' again (issue 157)",
                     actor, name);
            CoopPuppet::Call c;
            c.function = main_ + kPlayAs;
            c.x[0] = block_ + kBlockRequest;
            return c;
        }
        while (next_play_ < work_plays_.size() && block_ != 0) {
            const PlayWish& p = work_plays_[next_play_++];
            if (!Alive(guest, p.address)) {
                continue;
            }
            const u64 controller = Controller(guest, p.address);
            if (controller == 0 || (p.slot != 0 && !HasSlot(guest, controller, p.slot))) {
                continue;
            }
            // Issue 150: a boss's other slots keep their own entries; only
            // slot 0 can be the copy's own AI taking over.
            const u64 key = p.slot == 0 ? p.address : p.address ^ (static_cast<u64>(p.slot) << 56);
            // Checked every game frame: the copy's own AI may have put its
            // own animation back since the last one.
            const float rate = p.rate;
            const u64 since = now_ms >= want_ms_ ? std::min<u64>(now_ms - want_ms_, 100) : 0;
            const float frame =
                FrameAfter(p.frame, rate, static_cast<std::int64_t>(since) * 1000);
            const std::string slot_name = SlotName(guest, controller, p.slot);
            if (slot_name == p.name) {
                auto& checked = checked_sample_[key];
                if (checked == p.sample) {
                    continue;
                }
                checked = p.sample;
                float now_frame = 0.0f;
                if (SlotFrame(guest, controller, now_frame, p.slot) &&
                    std::fabs(now_frame - frame) <= kFrameSlack) {
                    continue;
                }
                ++framed_;
                return SetFrame(controller, frame, p.slot);
            }
            // Issue 195: a chest shown open is played again only over its
            // closed idle, never over its own opening (our Link opens it).
            if (!p.only_over.empty() && !slot_name.empty() &&
                slot_name.find(p.only_over) == std::string::npos) {
                continue;
            }
            // Issue 157: the name we played last is gone from slot 0, so the
            // copy's own AI chose another one (a Peahat rising for our Link).
            // It is put back below at once, so the walk would rarely see it:
            // told to Sync as a wake (TakeTakeovers). A LOD variant of ours
            // is not the AI's.
            std::string took_over;
            if (const auto last = played_at_.find(p.address);
                p.slot == 0 && last != played_at_.end() && last->second.first == p.name &&
                !slot_name.empty() &&
                AnimationBase(slot_name) != AnimationBase(p.name)) {
                took_over = slot_name;
                if (took_over_.size() < 256) {
                    took_over_.emplace_back(p.address, slot_name);
                }
            }
            // Remembered even on the first play we make on this copy (the
            // peahat rerun: its rise was the first thing the Mover put back,
            // so the check above had nothing to compare with).
            if (p.slot == 0 && !slot_name.empty() &&
                AnimationBase(slot_name) != AnimationBase(p.name)) {
                if (wanted_.size() >= 256) {
                    std::erase_if(wanted_, [now_ms](const auto& e) {
                        return now_ms - e.second.second > kWakeReplayMs;
                    });
                }
                if (wanted_.size() < 256) {
                    wanted_[p.address] = {slot_name, now_ms};
                }
            }
            const u64 text = Cell(guest, p.name);
            if (text == 0) {
                continue;
            }
            std::array<std::uint8_t, 0x30> request{};
            const float unset = -1.0f;
            const std::int32_t index = p.slot;
            std::memcpy(request.data() + 0x00, &p.address, 8);
            std::memcpy(request.data() + 0x08, &text, 8);
            std::memcpy(request.data() + 0x10, &index, 4);
            std::memcpy(request.data() + 0x14, &rate, 4);
            std::memcpy(request.data() + 0x18, &unset, 4);
            std::memcpy(request.data() + 0x1c, &unset, 4);
            if (!guest.Write(block_ + kBlockRequest, request.data(), request.size())) {
                continue;
            }
            ++played_;
            auto& last = played_at_[key];
            if (last.first != p.name || now_ms - last.second >= kPlayLogMs) {
                LOG_INFO(Core, "coop: objects: copy {:#x} plays '{}' at frame {:.1f}{}{}{}{}",
                         p.address, p.name, frame,
                         p.slot == 0 ? std::string() : fmt::format(" on slot {}", p.slot),
                         took_over.empty() ? "" : " (its own AI: '", took_over,
                         took_over.empty() ? "" : "')");
                last = {p.name, now_ms};
            }
            checked_sample_[key] = p.sample;
            frame_after_ = std::make_tuple(p.address, p.slot, frame);
            CoopPuppet::Call c;
            c.function = main_ + kPlayAs;
            c.x[0] = block_ + kBlockRequest;
            return c;
        }
        while (next_remove_ < work_removes_.size()) {
            const u64 a = work_removes_[next_remove_++];
            if (!Alive(guest, a)) {
                // Not awake now (another room, asleep): asked again on a
                // later frame. Marked asked only once really asked, or a
                // dead copy asleep when first asked stays for good (issue
                // 133: enemies left standing, frozen, for a late entrant).
                continue;
            }
            asked_.insert(a);
            ++deleted_;
            CoopPuppet::Call c;
            c.function = main_ + kRequestDelete;
            c.x[0] = a;
            c.x[1] = 0;
            return c;
        }
        // Poses last: the cave makes at most CoopSvc::kCallsPerFrame calls a
        // frame for everyone, and a scene may hold more copies than are left.
        // The list starts one further on each frame, so no copy is left out
        // twice in a row (run3, 2026-11-09: with 16 calls a frame, 3 to 5 of
        // 19 to 21 copies were never moved and no animation was ever played).
        while (next_ < work_.size()) {
            const Pose& p = work_[next_++];
            if (!Alive(guest, p.actor, p.asleep)) {
                continue;
            }
            if (p.rotation) {
                guest.Write(p.actor + kActorPosition, p.transform.data(), 48);
            } else {
                guest.Write(p.actor + kActorPosition, p.transform.data(), 12);
            }
            ++moved_;
            CoopPuppet::Call c;
            c.function = main_ + kSetMatrix;
            c.x[0] = p.actor;
            c.x[1] = 0;
            return c;
        }
        return std::nullopt;
    }

    // Present thread: the copies whose own AI put another animation in slot 0
    // over the owner's since the last ask (issue 157).
    std::vector<std::pair<u64, std::string>> TakeTakeovers() {
        std::lock_guard lock(mutex_);
        return std::exchange(took_over_, {});
    }

    // Since the last ask: copy-frames held, and in how many of them the
    // copy's brain skip bit read as asked (the cave's work of the frame
    // before, seen after the game's frame).
    std::pair<std::size_t, std::size_t> TakeHolds() {
        std::lock_guard lock(mutex_);
        return {std::exchange(hold_set_, 0), std::exchange(hold_kept_, 0)};
    }

private:
    // Start of a game frame: the held copies' BSA components go into the
    // table the hold cave reads (an empty list empties it, so an enemy that
    // became ours runs its own AI again from this frame).
    void WriteHolds(CoopPuppet::Guest& guest) {
        if (main_ == 0) {
            return;
        }
        std::vector<u64> components;
        const u16 want = static_cast<u16>(masks_.or_mask & CoopHold::kBrainSkip);
        for (const u64 actor : holds_) {
            u64 table = 0;
            u64 bsa = 0;
            u16 flags = 0;
            if (!Alive(guest, actor) || !guest.Read(actor + kActorComponents, &table, 8) ||
                table == 0 || !guest.Read(table + kComponentBsa, &bsa, 8) || bsa == 0 ||
                !guest.Read(bsa + kBsaFlags, &flags, 2)) {
                continue;
            }
            if (held_before_.contains(bsa)) {
                ++hold_set_;
                if ((flags & CoopHold::kBrainSkip) == want) {
                    ++hold_kept_;
                }
            }
            components.push_back(bsa);
        }
        if (components.size() > CoopHold::kMaxHeld) {
            components.resize(CoopHold::kMaxHeld);
        }
        const auto bytes = CoopHold::Table(components, masks_);
        guest.Write(main_ + CoopHold::kTableOffset, bytes.data(), bytes.size());
        held_before_ = std::unordered_set<u64>(components.begin(), components.end());
    }

    static u64 SteadyMs() {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
    }

    CoopPuppet::Call SetFrame(u64 controller, float frame, std::int32_t index = 0) {
        CoopPuppet::Call c;
        c.function = main_ + kSetAsFrame;
        c.x[0] = controller;
        c.x[1] = static_cast<u64>(index);
        c.s0 = frame;
        return c;
    }

    // The actor's AS controller, or 0.
    u64 Controller(CoopPuppet::Guest& guest, u64 actor) {
        u64 table = 0;
        u64 as = 0;
        u64 inner = 0;
        u64 controller = 0;
        s32 count = 0;
        if (!guest.Read(actor + kActorComponents, &table, 8) || table == 0 ||
            !guest.Read(table + kComponentsAs, &as, 8) || as == 0 ||
            !guest.Read(as + kAsInner, &inner, 8) || inner == 0 ||
            !guest.Read(inner + kAsController, &controller, 8) || controller == 0 ||
            !guest.Read(controller + kAsSlotCount, &count, 4) || count < 1) {
            return 0;
        }
        return controller;
    }

    // Whether the controller has that many slots (issue 150: a boss's).
    bool HasSlot(CoopPuppet::Guest& guest, u64 controller, std::int32_t index) {
        s32 count = 0;
        return index >= 0 && guest.Read(controller + kAsSlotCount, &count, 4) && index < count &&
               count <= 64;
    }

    u64 Slot0(CoopPuppet::Guest& guest, u64 controller, std::int32_t index = 0) {
        u64 array = 0;
        u64 slot = 0;
        if (!guest.Read(controller + kAsSlotArray, &array, 8) || array == 0 ||
            !guest.Read(array + kAsSlotEntry * static_cast<u64>(index), &slot, 8)) {
            return 0;
        }
        return slot;
    }

    // A slot's command name, slot 0 unless told ("" when unreadable).
    std::string SlotName(CoopPuppet::Guest& guest, u64 controller, std::int32_t index = 0) {
        const u64 slot = Slot0(guest, controller, index);
        u64 at = 0;
        std::array<char, 64> text{};
        if (slot == 0 || !guest.Read(slot + kAsSlotName, &at, 8) || at == 0 ||
            !guest.Read(at, text.data(), text.size())) {
            return {};
        }
        return std::string(text.data(), strnlen(text.data(), text.size()));
    }

    bool SlotFrame(CoopPuppet::Guest& guest, u64 controller, float& frame, std::int32_t index = 0) {
        const u64 slot = Slot0(guest, controller, index);
        return slot != 0 && guest.Read(slot + kAsSlotFrame, &frame, 4) && std::isfinite(frame);
    }

    // Writes one hit into the copy's Life component (see kHitTemplate).
    void Replay(CoopPuppet::Guest& guest, const Hit& h) {
        if (main_ == 0 || !Alive(guest, h.actor)) {
            return;
        }
        u64 table = 0;
        u64 life = 0;
        if (!guest.Read(h.actor + kActorComponents, &table, 8) || table == 0 ||
            !guest.Read(table + kComponentLife, &life, 8) || life == 0) {
            return;
        }
        u32 last = 0;
        guest.Read(life + kLastHitId, &last, 4);
        const u32 id = (last == 0xffffffffu ? 0x1000u : last + 2u) & 0x7fffffffu;
        std::array<u32, kHitTemplate.size()> rec = kHitTemplate;
        std::memcpy(&rec[0], &h.attacker, 8);
        rec[2] = id;
        rec[0x2c4 / 4 - kHitRecord / 4] = id;
        const std::array<float, 3> at{h.at[0], h.at[1] + (h.exact ? 0.0f : 1.0f), h.at[2]};
        std::memcpy(&rec[0x27c / 4 - kHitRecord / 4], at.data(), 12);
        std::memcpy(&rec[0x2a0 / 4 - kHitRecord / 4], h.from.data(), 12);
        rec[3] = 0;  // present last, below
        const std::array<u32, 4> header{0x00060001, 0x40040000, id, 2};
        const std::array<s32, 2> amount{4, h.amount};
        guest.Write(life + kHitHeader, header.data(), sizeof(header));
        guest.Write(life + kHitAmount, amount.data(), sizeof(amount));
        guest.Write(life + kHitRecord, rec.data(), sizeof(rec));
        const u32 present = 1;
        guest.Write(life + kHitRecord + 0xc, &present, 4);
        ++replayed_;
        LOG_INFO(Core, "coop: objects: copy {:#x} replays a hit of {} (id {:#x})", h.actor,
                 h.amount, id);
    }

    // The fixed cell holding `name` in the block (written once), or 0 when
    // the block is full or the name too long.
    u64 Cell(CoopPuppet::Guest& guest, const std::string& name) {
        if (const auto it = cells_.find(name); it != cells_.end()) {
            return it->second;
        }
        if (name.size() >= kNameCell || cells_.size() >= kNameCells) {
            return 0;
        }
        const u64 at = block_ + kBlockNames + kNameCell * cells_.size();
        std::array<char, kNameCell> text{};
        std::memcpy(text.data(), name.data(), name.size());
        if (!guest.Write(at, text.data(), text.size())) {
            return 0;
        }
        cells_.emplace(name, at);
        return at;
    }

    // On the game thread, so the list cannot change under us: still on the
    // actor list (its node points back at it), awake, and an actor.
    // `asleep`: state 6 counts too (a copy moved to us from its far home).
    bool Alive(CoopPuppet::Guest& guest, u64 a, bool asleep = false) {
        u32 state = 0;
        u64 owner = 0;
        u64 vtable = 0;
        return a != 0 && guest.Read(a + kActorState, &state, 4) &&
               (state == kStateAlive || (asleep && state == kStateAsleep)) &&
               guest.Read(a + kActorNode + kNodeOwner, &owner, 8) && owner == a &&
               guest.Read(a, &vtable, 8) && vtable == main_ + kActorVtable;
    }

    std::mutex mutex_;
    u64 main_ = 0;
    std::vector<Pose> poses_;
    std::vector<u64> removes_;
    std::vector<PlayWish> plays_;
    std::vector<Hit> hits_;
    std::size_t replayed_ = 0;
    std::vector<PlayWish> work_plays_;
    std::size_t next_play_ = 0;
    std::size_t played_ = 0;
    std::size_t framed_ = 0;  // frames set on a copy already showing the name
    u64 want_ms_ = 0;
    std::optional<std::tuple<u64, std::int32_t, float>> frame_after_;  // actor, slot, frame: set after the play
    std::unordered_map<u64, std::uint64_t> checked_sample_;  // actor: sample time checked
    std::unordered_map<u64, std::pair<std::string, u64>> played_at_;  // actor: name, ms
    std::vector<std::pair<u64, std::string>> took_over_;  // copies whose own AI changed slot 0, and to what
    std::unordered_map<std::string, u64> cells_;
    u64 block_ = 0;
    bool allocating_ = false;
    bool alloc_tried_ = false;
    std::vector<Pose> work_;
    // Issue 157: the animation each copy's own AI last chose that we put
    // back (name, steady ms), and the ones to give back once it is ours.
    static constexpr u64 kWakeReplayMs = 8000;
    std::unordered_map<u64, std::pair<std::string, u64>> wanted_;
    std::vector<std::pair<u64, std::string>> wakes_;
    std::vector<std::pair<u64, std::string>> work_wakes_;
    std::size_t next_wake_ = 0;
    std::size_t woken_ = 0;
    std::vector<u64> work_removes_;
    std::unordered_set<u64> asked_;  // deletes asked once
    std::size_t next_ = 0;
    std::size_t turn_ = 0;  // where this frame's poses start
    std::size_t next_remove_ = 0;
    std::size_t moved_ = 0;
    std::size_t deleted_ = 0;
    std::vector<u64> holds_;
    CoopHold::Masks masks_;
    bool holding_ = false;
    std::vector<u64> releases_;
    std::size_t released_ = 0;       // bit 23 was set: cleared
    std::size_t release_clear_ = 0;  // bit 23 was already clear
    std::size_t release_dropped_ = 0;  // over 64 in one frame: not looked at
    std::unordered_set<u64> held_before_;  // BSA components in last frame's table
    std::size_t hold_set_ = 0;
    std::size_t hold_kept_ = 0;
};

Mover& TheMover() {
    static Mover mover;
    return mover;
}

// Whether our own Link is in an event (a talk, a cutscene), from the hook's
// reading each frame (SetLocalEvent; issue 144).
std::atomic<bool>& TheLocalEvent() {
    static std::atomic<bool> in_event{false};
    return in_event;
}

// The MaxLife flag as the session left it (SetMaxLife); -1: not known.
std::atomic<unsigned>& TheChestsOpenedTwice() {
    static std::atomic<unsigned> count{0};
    return count;
}

std::atomic<s32>& TheMaxLife() {
    static std::atomic<s32> quarters{-1};
    return quarters;
}

// Our Link's Life component as the walk last read it (OurLife): quarter
// hearts, and when (CoopEvery::NowUs; 0: never).
struct LifeSeen {
    std::atomic<s32> life{-1};
    std::atomic<s32> max{-1};
    std::atomic<u64> at_us{0};
};
LifeSeen& TheLife() {
    static LifeSeen seen;
    return seen;
}
// PvP (coop_pvp_game.cpp): our Link's life as the last walk read it, and
// when (steady ms; 0: never), for OurLinkLife; and the hurts the victim's
// rules decided (HurtOurLink), for the walk to give.
std::atomic<s32>& TheLinkLife() {
    static std::atomic<s32> quarters{-1};
    return quarters;
}
std::atomic<u64>& TheLinkLifeAt() {
    static std::atomic<u64> ms{0};
    return ms;
}
struct PvpAsk {
    s32 take = 0;
    std::array<float, 3> at{};
    std::array<float, 3> from{};
};
std::mutex& PvpMutex() {
    static std::mutex mutex;
    return mutex;
}
std::vector<PvpAsk>& PvpAsks() {
    static std::vector<PvpAsk> asks;
    return asks;
}
constexpr u64 kLinkLifeStaleMs = 1500;
constexpr u64 kPvpHitWaitMs = 400;  // the game's own hit, before the life is written

class Objects {
public:
    void Frame(Core::System& system) {
        const auto now_us = CoopEvery::NowUs();
        if (!scan_.Due(now_us) || !Ready(system)) {
            return;
        }
        if (cheat_ && cheat_every_.Due(now_us)) {
            Cheat(system);
        }
        if (debug_) {
            Reader r(system.ApplicationMemory());
            Walk(r);
        }
    }

    // Present thread, after the session's frame. Not connected, or no scene:
    // forgets what it shared.
    void Share(Core::System& system, coop::host::Session* session,
               const std::optional<std::string>& scene, u64 server_now_us) {
        if (session == nullptr || !scene || scene->empty() || server_now_us == 0) {
            if (sharing_) {
                sync_.Reset();
                world_.Reset();
                sharing_ = false;
            }
            return;
        }
        if (!Ready(system)) {
            return;
        }
        if (test_take_ != 0 && sharing_ && self_ == 2 && take_at_ != 0 &&
            server_now_us >= take_at_) {
            take_at_ = 0;
            coop::protocol::FlagState state;
            state.player_number = 1;
            state.flags.push_back(coop::protocol::FlagEntry(
                coop::protocol::FlagKey::key64_flag(test_take_), coop::protocol::FlagKind::boolean, 1));
            sync_.Receive(state);
            LOG_INFO(Core, "coop: objects: test take {:#x}", test_take_);
        }
        if (drive_ && sharing_) {
            Drive(system, server_now_us);
        }
        if (pose_ && sharing_) {
            Place(system, server_now_us);
        }
        if (!share_every_.Due(CoopEvery::NowUs())) {
            return;
        }
        Reader r(system.ApplicationMemory());
        memory_ = &system.ApplicationMemory();
        std::vector<Seen> seen;
        if (!Collect(r, seen)) {
            return;
        }
        NoteBosses(seen);
        const u8 self =
            session->with_client([](coop::net::Client& c) { return c.player_number(); });
        if (self == 0) {
            return;
        }
        if (!sharing_) {
            static const bool registered = [] {
                CoopSvc::AddCaller(kMoverCallerOrder,
                                   [](CoopPuppet::Guest& guest, bool after_call,
                                      std::uint64_t result, std::uint64_t now_ms) {
                                       return TheMover().Step(guest, after_call, result, now_ms);
                                   });
                CoopSvc::AddCaller(kGrassCallerOrder,
                                   [](CoopPuppet::Guest& guest, bool after_call,
                                      std::uint64_t result, std::uint64_t now_ms) {
                                       return TheGrass().Step(guest, after_call, result, now_ms);
                                   });
                // Issue 149: another player's hit on a Scrub Brother, given to our manager.
                CoopSvc::AddCaller(kScrubsCallerOrder,
                                   [](CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                      std::uint64_t) -> std::optional<CoopPuppet::Call> {
                                       ScrubsBox& box = TheScrubs();
                                       std::lock_guard lock(box.mutex);
                                       if (box.main == 0) {
                                           return std::nullopt;
                                       }
                                       auto call = box.puzzle.Step(guest, after_call, result, box.main,
                                                                   CoopEvery::NowUs());
                                       for (const auto& line : box.puzzle.TakeNotes()) {
                                           LOG_INFO(Core, "coop: {}", line);
                                       }
                                       return call;
                                   });
                return true;
            }();
            (void)registered;
            CoopSpawn::UseDirector();  // makes the rolled enemies another game decided
            sync_.SetHurt(hurt_);
            sync_.SetWake(wake_);  // issue 132: LICHII_COOP_OBJECTS_WAKE=0 turns it off
            // A projectile copy's blast here is not our hit (docs/objects.md, "Projectiles").
            sync_.SetBlastNear([](const coop::protocol::Vec3& at, std::uint64_t now) {
                return CoopProjectiles::TheBlasts().Near(at, now);
            });
            take_at_ = server_now_us + 3'000'000;
            LOG_INFO(Core, "coop: objects: share on, poses {}, hurt {}", pose_ ? "on" : "off",
                     hurt_ ? "through the game" : "written as life");
        }
        self_ = self;
        sharing_ = true;
        std::vector<coop::protocol::Message> send;
        std::vector<LifeWrite> writes;
        {
            std::map<std::uint64_t, std::uint64_t> live;
            for (const auto& [key, actor] : CoopSpawn::TheDirector().Live()) {
                if ((key & kEnemyKey) != 0) {
                    live[key & ~kEnemyKey] = actor;
                }
            }
            sync_.Bind(live);
        }
        sync_.Tick(*scene, self, server_now_us, seen, send, writes);
        if (const unsigned twice = sync_.ChestsOpenedTwice(); twice != chests_twice_seen_) {
            // Issue 195: a chest shown open that our Link opened again; the
            // flag watcher takes back its content's count (TakeChestsOpenedTwice).
            if (twice > chests_twice_seen_) {
                TheChestsOpenedTwice() += twice - chests_twice_seen_;
            }
            chests_twice_seen_ = twice;
        }
        {
            // Enemy shots (coop_enemy_shots.h): which game's enemy fired one.
            std::vector<CoopEnemyShots::Shooter> shooters;
            for (const Seen& s : seen) {
                if (s.name.starts_with("Enemy_")) {
                    const auto owner = sync_.OwnerOf(s.address);
                    shooters.push_back({s.name, s.position,
                                        !owner || *owner == 0 ? CoopEnemyShots::Owner::unknown
                                        : *owner == self      ? CoopEnemyShots::Owner::ours
                                                              : CoopEnemyShots::Owner::theirs});
                }
            }
            CoopEnemyShots::NoteShooters(std::move(shooters), server_now_us);
        }
        if (sync_.BlastDrops() != blast_drops_logged_) {
            blast_drops_logged_ = sync_.BlastDrops();
            LOG_INFO(Core, "coop: shots: life drop by a projectile copy's blast kept back ({} so far)",
                     blast_drops_logged_);
        }
        {
            std::vector<CoopSpawn::Copy> enemies;
            for (const EnemyCopy& e : sync_.Creates()) {
                CoopSpawn::Copy c;
                c.key = e.key | kEnemyKey;
                c.actor = e.name;
                c.position = e.position;
                c.act = e.removed ? CoopSpawn::Act::remove : CoopSpawn::Act::show;
                c.place = false;
                c.remakes = e.remakes;  // issue 124: a copy that sank by itself comes back
                enemies.push_back(std::move(c));
            }
            if (enemies.size() != enemies_logged_ || sync_.Refused() != refused_logged_ ||
                sync_.Decider() != decider_logged_) {
                enemies_logged_ = enemies.size();
                refused_logged_ = sync_.Refused();
                decider_logged_ = sync_.Decider();
                LOG_INFO(Core, "coop: objects: rolls decided by player {}, {} copies to make, {} own rolls deleted",
                         decider_logged_, enemies_logged_, refused_logged_);
            }
            CoopSpawn::TheDirector().SetEnemies(main_, std::move(enemies));
        }
        for (const auto& note : sync_.TakeNotes()) {
            LOG_INFO(Core, "coop: objects: share {}", note);
        }
        // Breaks and moves of placed world objects (docs/world.md).
        {
            CoopWorld::WorldSync::Out world;
            {
                std::lock_guard lock(KeyRefundMutex());
                for (const std::uint32_t key : OwnDropQueue()) {
                    world_.OwnCountDrop(key, server_now_us);
                }
                OwnDropQueue().clear();
                if (std::exchange(ForgetKeptFlag(), false)) {
                    world_.ForgetKept();
                }
            }
            world_.Tick(*scene, self, server_now_us, seen, player_, world);
            for (auto& m : world.send) {
                send.emplace_back(std::move(m));
            }
            writes.insert(writes.end(), world.lives.begin(), world.lives.end());
            TestKills(seen, writes);
            if (!world.key_refunds.empty()) {
                std::lock_guard lock(KeyRefundMutex());
                auto& refunds = KeyRefundQueue();
                refunds.insert(refunds.end(), world.key_refunds.begin(), world.key_refunds.end());
            }
            sign_words_ = std::move(world.words);  // written after the probe's look
            for (std::string& count : world.give_back) {
                CoopWorld::QueueGiveBack(std::move(count));  // a small key or token both took
            }
            for (const std::uint64_t key : world.taken_keys) {
                CoopWorld::QueueTakenKey(key);  // a token taken elsewhere: its Skulltula's key
            }
            block_fixes_ = std::move(world.blocks);  // issue 147, written below
            if (!world.hits.empty()) {
                // A sign another player's sword passed: the same hit on ours.
                u64 link = 0;
                for (const auto& [a, known] : names_) {
                    if (known.first == "Player_Child" || known.first == "Player_Adult") {
                        link = a;
                        break;
                    }
                }
                for (const auto& hit : world.hits) {
                    if (link == 0) {
                        break;
                    }
                    Mover::Hit h;
                    h.actor = hit.address;
                    h.attacker = link;
                    h.amount = 1;
                    h.at = hit.at;
                    h.from = hit.from;
                    h.exact = true;
                    TheMover().AddHit(h);
                }
            }
            for (const auto& note : world.notes) {
                if (world_lines_ < 2000) {
                    ++world_lines_;
                    LOG_INFO(Core, "coop: {}", note);
                }
            }
        }
        if (scrubs_on_) {  // issue 149: our hits on the Scrub Brothers go out
            CoopScrubs::Puzzle::Out out;
            {
                ScrubsBox& box = TheScrubs();
                std::lock_guard lock(box.mutex);
                box.main = main_;
                box.puzzle.SetEvent(TheLocalEvent().load(), CoopEvery::NowUs());  // issue 144
                box.puzzle.Look(*scene, scrubs_seen_, player_, CoopEvery::NowUs(), out);
            }
            for (auto& h : out.send) {
                send.emplace_back(std::move(h));
            }
            for (const auto& line : out.notes) {
                LOG_INFO(Core, "coop: {}", line);
            }
        }
        // Field grass (docs/world.md): our new cuts go out, the others' are
        // made at the svc cave.
        for (auto& cut : TheGrass().Look([&r](u64 address) { return r.Get<u64>(address); }, main_,
                                         *scene, SteadyNowMs())) {
            LOG_INFO(Core, "coop: grass: cut at {:.2f} {:.2f} {:.2f} radius {:.2f} flags {:#x} sent",
                     cut.position[0], cut.position[1], cut.position[2], cut.radius, cut.flags);
            {
                CoopWorld::WorldSync::Out swing;  // a sign the sword passed (docs/world.md)
                world_.Swing(cut, player_, server_now_us, swing);
                CoopPvp::OwnSwing(cut);  // PvP: the swing may reach another Link
                for (auto& m : swing.send) {
                    send.emplace_back(std::move(m));
                }
                for (const auto& note : swing.notes) {
                    LOG_INFO(Core, "coop: {}", note);
                }
            }
            send.emplace_back(std::move(cut));
        }
        for (const auto& note : TheGrass().TakeNotes()) {
            LOG_INFO(Core, "coop: {}", note);
        }
        // Signs (issue 90, docs/world.md "Signs"): the probe; our cut's words
        // go out (SIGN_CUT), another player's are written into our sign.
        {
            CoopWorld::WorldSync::Out signs;
            world_.ProbeSigns([&r](u64 address) { return r.Get<u64>(address); }, server_now_us,
                              signs);
            for (auto& m : signs.send) {
                send.emplace_back(std::move(m));
            }
            for (const auto& note : signs.notes) {
                LOG_INFO(Core, "coop: {}", note);
            }
            for (const auto& w : sign_words_) {
                std::string why;
                const auto at = world_.SignWordTarget(
                    [&r](u64 address) { return r.Get<u64>(address); }, w, &why);
                if (at) {
                    memory_->Write64(*at, w.value);
                } else {
                    LOG_INFO(Core, "coop: world: {:016x} sign word {} not written: {}", w.hash,
                             CoopWorld::SignProbe::Where(w.key), why);
                }
            }
            sign_words_.clear();
            // Issue 147: a block another game moved rests here now; its own
            // memory's old rest place gets the new one.
            for (const auto& fix : block_fixes_) {
                // world-objects-2: the block's push node (known fields,
                // found by its vtables and owner): on unless
                // LICHII_COOP_BLOCK_NODE=0.
                static const bool node_on = EnvOn("LICHII_COOP_BLOCK_NODE");
                if (node_on) {
                    const auto node = CoopWorld::FindBlockNode(
                        [&r](u64 address) { return r.Get<u64>(address); }, fix.address, main_);
                    if (!node) {
                        LOG_INFO(Core, "coop: world: {:016x} {} push node not found: its next push may "
                                       "start from the old place",
                                 fix.hash, fix.name);
                    } else {
                        const auto words = CoopWorld::NodeRestWords(
                            [&r](u64 address) { return r.Get<u64>(address); }, *node, fix.from, fix.to);
                        if (words.busy) {
                            LOG_INFO(Core, "coop: world: {:016x} {} push node {:x} busy (mid-push here): "
                                           "not written",
                                     fix.hash, fix.name, *node);
                            continue;  // RestWords below too: one block, one decision
                        }
                        for (const auto& w : words.writes) {
                            memory_->Write32(w.address, w.value);
                        }
                        std::string at;
                        for (const auto& w : words.where) {
                            at += " " + w;
                        }
                        LOG_INFO(Core, "coop: world: {:016x} {} push node {:x}: {} of its places moved to "
                                       "the new rest{}{}",
                                 fix.hash, fix.name, *node, words.where.size(), at.empty() ? "" : ":", at);
                    }
                }
                const auto scan = CoopWorld::RestWords(
                    [&r](u64 address) { return r.Get<u64>(address); }, fix.address, fix.from,
                    fix.to);
                std::string where;
                for (const auto& w : scan.where) {
                    where += " " + w;
                }
                if (scan.too_many) {
                    LOG_INFO(Core, "coop: world: {:016x} {} rest place {:.2f} {:.2f} {:.2f} found {} times, "
                                   "not written (too many):{}",
                             fix.hash, fix.name, fix.from[0], fix.from[1], fix.from[2],
                             scan.where.size(), where);
                    continue;
                }
                // The searched sizes are not known object sizes (review):
                // written only with LICHII_COOP_BLOCK_REST=1; otherwise only
                // where they would go is logged, for a run to name the word.
                static const bool write_rest = EnvFlag("LICHII_COOP_BLOCK_REST");
                if (write_rest) {
                    for (const auto& w : scan.writes) {
                        memory_->Write32(w.address, w.value);
                    }
                }
                LOG_INFO(Core, "coop: world: {:016x} {} old rest {:.2f} {:.2f} {:.2f} -> {:.2f} {:.2f} {:.2f}: "
                               "{} places {}{}{}",
                         fix.hash, fix.name, fix.from[0], fix.from[1], fix.from[2], fix.to[0],
                         fix.to[1], fix.to[2], scan.where.size(),
                         write_rest ? "rewritten" : "found, not written (LICHII_COOP_BLOCK_REST off)",
                         where.empty() ? "" : ":", where);
            }
            block_fixes_.clear();
        }
        // Probe (docs/dungeon.md "Live dungeon actions"): which words of a
        // switch, water plane or lift step when it changes. Reads only.
        if (gimmick_probe_on_) {
            for (const auto& line : gimmick_probe_.Look([&r](u64 address) { return r.Get<u64>(address); },
                                                        gimmicks_, player_, server_now_us)) {
                LOG_INFO(Core, "coop: {}", line);
            }
        }
        // Issue 152 (docs/puppet.md "Ocarina"): where our note is kept. Reads only.
        if (ocarina_probe_on_) {
            for (const auto& line : ocarina_probe_.Look([&r](u64 address) { return r.Get<u64>(address); },
                                                        our_link_, server_now_us)) {
                LOG_INFO(Core, "coop: {}", line);
            }
            // Issue 152: what moves the Ocarina from the belt to the hand.
            for (const auto& line : ocarina_hold_probe_.Step([&r](u64 address) { return r.Get<u64>(address); },
                                                             our_link_, ocarina_probe_.Active(), server_now_us)) {
                LOG_INFO(Core, "coop: {}", line);
            }
        }
        if (const u64 made = TheGrass().Made(); made != grass_made_logged_) {
            grass_made_logged_ = made;
            LOG_INFO(Core, "coop: grass: {} cuts of the others made here", made);
        }
        for (const Seen& s : seen) {
            if (!s.animation || !IsShared(s.name)) {
                continue;
            }
            auto& last = anim_logged_[s.address];
            if (last != s.animation->name && anim_lines_ < 400) {
                ++anim_lines_;
                LOG_INFO(Core, "coop: objects: anim {} {:#x} '{}'{}", s.name, s.address,
                         s.animation->name,
                         sync_.OwnerOf(sync_.IdOf(s.address).value_or(coop::protocol::ObjectId{})) ==
                                 self
                             ? " (ours)"
                             : "");
                last = s.animation->name;
            }
        }
        // Issue 144: a kill or hurt from another game waits while our Link is in
        // an event with that actor (near it), and is made when the event ends.
        {
            if (*scene != guard_scene_) {
                guard_scene_ = *scene;
                // Those actors are gone with the scene (enemies come back with it).
                if (const std::size_t dropped = held_writes_.Forget(); dropped != 0) {
                    LOG_INFO(Core, "coop: objects: the scene changed: {} waiting changes dropped", dropped);
                }
                guard_said_ = false;
            }
            where_.clear();
            for (const Seen& s : seen) {
                // Who is at the address: placement hash, name and the sync's object id
                // (actors the game makes share one "no hash"), so a reused address is
                // told apart (CoopGuard::Who).
                std::optional<std::uint64_t> object;
                if (const auto id = sync_.IdOf(s.address)) {
                    object = id->value ^ (std::uint64_t{id->maker} << 56) ^
                             (std::uint64_t(static_cast<unsigned>(id->kind)) << 48);
                }
                const u64 who = CoopGuard::Who(s.hash, s.name, object);
                where_.emplace(s.address, std::make_pair(s.position, who));
            }
            const CoopGuard::Local local = GuardLocal();
            auto passed = held_writes_.Pass(
                std::move(writes), local, [this](std::uint64_t a) { return Where(a); },
                [this](std::uint64_t a) { return HashAt(a); });
            writes = std::move(passed.apply);
            // The held ones land now: the Sync asked for them long ago, and their drop
            // is no hit of our Link's (else it went to the owner as DAMAGE again).
            for (std::size_t i = 0; i < passed.released; ++i) {
                sync_.Written(writes[i].address, writes[i].life, server_now_us);
            }
            if (held_writes_.Size() != 0 && !guard_said_) {
                guard_said_ = true;
                LOG_INFO(Core, "coop: objects: our Link is in an event: changes to {} actors near it wait until it ends",
                         held_writes_.Size());
            } else if (held_writes_.Size() == 0 && guard_said_ && !local.in_event) {
                guard_said_ = false;
                LOG_INFO(Core, "coop: objects: the event ended: {} waiting changes made now, {} dropped (another actor there now)",
                         passed.released, passed.dropped);
            }
        }
        for (const LifeWrite& w : writes) {
            WriteLife(system.ApplicationMemory(), r, w);
        }
        if (send.empty()) {
            return;
        }
        session->with_client([&](coop::net::Client& c) {
            for (const auto& m : send) {
                const bool sent = c.send(m);
                if (const auto* sp = std::get_if<coop::protocol::ObjectSpawn>(&m)) {
                    LOG_INFO(Core, "coop: objects: rolled {} shared as made {}{}", sp->actor,
                             sp->id.value, sent ? "" : " (not sent)");
                } else if (const auto* rm = std::get_if<coop::protocol::ObjectRemove>(&m)) {
                    LOG_INFO(Core, "coop: objects: share remove {:#x} reason {}{}", rm->id.value,
                             static_cast<int>(rm->reason), sent ? "" : " (not sent)");
                } else if (const auto* d = std::get_if<coop::protocol::Damage>(&m)) {
                    LOG_INFO(Core, "coop: objects: share damage {} to {:#x} epoch {}{}", d->amount,
                             d->id.value, d->epoch, sent ? "" : " (not sent)");
                } else if (const auto* p = std::get_if<coop::protocol::ObjectPresent>(&m);
                           p != nullptr && p->objects.size() != present_logged_) {
                    present_logged_ = p->objects.size();
                    LOG_INFO(Core, "coop: objects: share present {} enemies in {}{}",
                             p->objects.size(), p->scene, sent ? "" : " (not sent)");
                } else if (const auto* g = std::get_if<coop::protocol::ObjectGroup>(&m);
                           g != nullptr && g->entries != group_logged_) {
                    // Only the entries that are new or changed, at most 8 lines.
                    std::size_t logged = 0, changed = 0;
                    for (const auto& e : g->entries) {
                        if (std::find(group_logged_.begin(), group_logged_.end(), e) !=
                            group_logged_.end()) {
                            continue;
                        }
                        ++changed;
                        if (logged++ < 8) {
                            LOG_INFO(Core, "coop: objects: group {:#x} follows {:#x}{}",
                                     e.member.value, e.root.value,
                                     e.flags != 0 ? " (boss)" : "");
                        }
                    }
                    LOG_INFO(Core, "coop: objects: share group {} entries, {} changed{}",
                             g->entries.size(), changed, sent ? "" : " (not sent)");
                    group_logged_ = g->entries;
                } else if (const auto* f = std::get_if<coop::protocol::FishLure>(&m)) {
                    // Issue 151: one line when the fish at our lure change.
                    std::size_t in_reach = 0;
                    std::string at;
                    for (const auto& e : f->fish) {
                        if (e.distance_cm == coop::protocol::kFishLureNone) continue;
                        ++in_reach;
                        if (e.flags != 0) {
                            at += fmt::format(" {:#x} {} cm {}", e.id.value, e.distance_cm,
                                              (e.flags & coop::protocol::fish_lure::hooked)
                                                  ? "hooked"
                                                  : "drawn");
                        }
                    }
                    if (std::make_pair(in_reach, at) != fish_logged_) {
                        fish_logged_ = std::make_pair(in_reach, at);
                        LOG_INFO(Core, "coop: objects: share fish lure {} fish, {} near our lure{}{}",
                                 f->fish.size(), in_reach, at, sent ? "" : " (not sent)");
                    }
                }
            }
        });
    }

    // Network thread.
    void ForgetOwnerLog() {  // network thread
        owner_log_.Clear();
        sync_.ForgetOwners();  // the server's epochs may start at 0 again (issue 133)
    }

    void Take(std::uint64_t key) { sync_.Take(key); }  // any thread (Sync::Take locks)

    void Receive(const coop::protocol::Message& m) {
        if (const auto* o = std::get_if<coop::protocol::ObjectOwner>(&m)) {
            if (owner_log_.Changed(*o)) {  // owner, epoch or group changed
                LOG_INFO(Core, "coop: objects: share owner {:#x} is player {} epoch {} group {}",
                         o->id.value, o->owner, o->epoch, o->group);
            }
        } else if (const auto* d = std::get_if<coop::protocol::Damage>(&m)) {
            LOG_INFO(Core, "coop: objects: share got damage {} for {:#x}", d->amount,
                     d->id.value);
        } else if (const auto* g = std::get_if<coop::protocol::GrassCut>(&m)) {
            LOG_INFO(Core, "coop: grass: got cut from P{} in {} at {:.2f} {:.2f} {:.2f} radius {:.2f}",
                     g->player_number, g->scene, g->position[0], g->position[1], g->position[2],
                     g->radius);
            TheGrass().Receive(*g);
            return;
        } else if (const auto* sh = std::get_if<coop::protocol::WorldHit>(&m);
                   sh != nullptr && scrubs_on_ && CoopScrubs::FindBrother(sh->hash) != nullptr) {
            ScrubsBox& box = TheScrubs();  // issue 149: a Scrub Brother hit in another game
            std::lock_guard lock(box.mutex);
            box.puzzle.Receive(*sh, CoopEvery::NowUs());
            for (const auto& line : box.puzzle.TakeNotes()) {
                LOG_INFO(Core, "coop: {}", line);
            }
            return;
        } else if (std::holds_alternative<coop::protocol::WorldBreak>(m) ||
                   std::holds_alternative<coop::protocol::WorldPlace>(m) ||
                   std::holds_alternative<coop::protocol::WorldHit>(m) ||
                   std::holds_alternative<coop::protocol::SignCut>(m) ||
                   std::holds_alternative<coop::protocol::WorldMove>(m)) {
            if (const auto* b = std::get_if<coop::protocol::WorldBreak>(&m)) {
                LOG_INFO(Core, "coop: world: got break {:#x} from P{} in {}{}", b->hash,
                         b->player_number, b->scene,
                         (b->flags & coop::protocol::world_break::restored) != 0 ? " (back)" : "");
            } else if (const auto* p = std::get_if<coop::protocol::WorldPlace>(&m)) {
                LOG_INFO(Core, "coop: world: got place {:#x} from P{} at {:.2f} {:.2f} {:.2f}",
                         p->hash, p->player_number, p->position[0], p->position[1], p->position[2]);
            }
            world_.Receive(m);
            return;
        }        sync_.Receive(m);
    }

    bool debug_ = false;
    u64 grass_made_logged_ = 0;
    CoopWorld::WorldSync world_;
    CoopWorld::GimmickProbe gimmick_probe_;  // LICHII_COOP_GIMMICK_PROBE=1 turns it on
    std::vector<CoopWorld::GimmickProbe::Gimmick> gimmicks_;
    bool gimmick_probe_on_ = false;
    bool scrubs_on_ = true;  // LICHII_COOP_SCRUBS=0 turns it off (issue 149)
    std::vector<CoopScrubs::Seen> scrubs_seen_;
    bool ocarina_probe_on_ = false;           // issue 152: LICHII_COOP_OCARINA_PROBE=1
    CoopOcarina::OcarinaProbe ocarina_probe_;
    CoopOcarina::HoldProbe ocarina_hold_probe_;
    u64 our_link_ = 0;                        // our Link's actor, from the last walk
    std::vector<CoopWorld::WorldSync::Out::Word> sign_words_;  // SIGN_CUT words to write
    std::vector<CoopWorld::WorldSync::Out::BlockFix> block_fixes_;  // issue 147: blocks to fix
    std::optional<coop::protocol::Vec3> player_;  // our Link in the last walk
    // Issue 144: shared changes to actors near our Link wait while it is in an
    // event (coop_guard.h). Where each actor of the last walk was, kills and
    // hurts held back, and what was said about it.
    std::unordered_map<u64, std::pair<coop::protocol::Vec3, u64>> where_;  // place, who (hash and name)
    CoopGuard::Held<LifeWrite> held_writes_{
        [](LifeWrite& kept, LifeWrite&& newer) { CoopGuard::MergeLife(kept, std::move(newer)); }};
    std::string guard_scene_;
    bool guard_said_ = false;
    CoopGuard::Local GuardLocal() const { return {TheLocalEvent().load(), player_}; }
    std::optional<coop::protocol::Vec3> Where(u64 address) const {
        const auto it = where_.find(address);
        if (it == where_.end()) {
            return std::nullopt;
        }
        return it->second.first;
    }
    std::optional<u64> HashAt(u64 address) const {
        const auto it = where_.find(address);
        if (it == where_.end()) {
            return std::nullopt;
        }
        return it->second.second;
    }
    int world_lines_ = 0;
    CoopWorld::FoliageProbe foliage_;
    // Issue 88 (docs/items.md, "Heart containers").
    s32 link_life_logged_ = -1;
    s32 link_max_logged_ = -1;
    s32 flag_max_logged_ = -1;
    bool hearts_to_link_ = true;  // LICHII_COOP_HEARTS_TO_LINK, on unless =0 (issue 88)
    // PvP hurts being given to our Link (GivePvp).
    struct PvpGive {
        s32 take = 0;
        s32 expect = 0;  // the life after it
        u64 asked_ms = 0;
        bool replayed = false;
    };
    std::vector<PvpGive> pvp_;
    bool pvp_replay_ = true;  // LICHII_COOP_PVP_REPLAY=0: write the life only
    Core::Memory::Memory* memory_ = nullptr;  // set by Share for Collect
    bool cheat_ = false;
    bool share_ = false;

    // Present thread: the shared things this game owns (for coop_drops).
    auto OwnedNow() const { return sync_.Owned(); }
    bool pose_ = true;
    bool hurt_ = true;
    bool replay_ = false;
    bool wake_ = true;
    bool release_ = true;  // LICHII_COOP_OBJECTS_RELEASE=0 turns it off (issues 150, 157)
    bool wake_far_ = true;  // LICHII_COOP_OBJECTS_ASLEEP=0 turns it off (issue 101)
    std::size_t asleep_placed_ = 0;  // copy-frames placed in state 6 since the last log
    bool drive_ = false;
    int ai_hold_ = 0;  // 0 off, 1 set the brain skip bit on copies, 2 clear it
    u64 test_take_ = 0;
    u64 take_at_ = 0;
    int drive_mode_ = 0;

private:
    // The game is the right one; finds main once per process.
    bool Ready(Core::System& system) {
        auto* process = system.ApplicationProcess();
        if (process == nullptr || system.GetApplicationProcessProgramID() != kOcarinaProgramId) {
            return false;
        }
        const auto& build = system.GetApplicationProcessBuildID();
        if (std::memcmp(build.data(), kGameBuildId.data(), kGameBuildId.size()) != 0) {
            return false;
        }
        if (process != process_) {
            process_ = process;
            main_ = FindMain(system, process);
            actors_.clear();
            first_ = true;
            names_.clear();
            sync_.Reset();
            world_.Reset();
            {
                ScrubsBox& box = TheScrubs();  // a new game run: its block is gone
                std::lock_guard lock(box.mutex);
                box.puzzle = CoopScrubs::Puzzle(ScrubsBox::HitAnimations());
                box.main = 0;
            }
            LOG_INFO(Core, "coop: objects: main at {:#x}", main_);
        }
        return true;
    }

    // The shared actors of one whole walk, with life and place. False when
    // the list changed under us (try the next frame).
    bool Collect(Reader& r, std::vector<Seen>& out) {
        gimmicks_.clear();  // dungeon gimmicks for the probe
        scrubs_seen_.clear();  // issue 149
        std::vector<u64> actors;
        if (!List(r, actors)) {
            return false;
        }
        std::optional<coop::protocol::Vec3> player;
        std::unordered_map<u64, std::pair<std::string, u64>> names;
        for (const u64 a : actors) {
            auto it = names_.find(a);
            std::pair<std::string, u64> known;
            if (it != names_.end()) {
                known = std::move(it->second);
            } else {
                known.first = r.Name(a);
                const auto placement = r.Ptr(a + kActorPlacement);
                known.second = placement ? r.Get<u64>(*placement + kPlacementHash).value_or(0) : 0;
                CoopPickups::Note(known.first, known.second);  // one-time pickups (docs/items.md)
                if (IsShared(known.first) && vtables_logged_.insert(known.first).second) {
                    // The set-matrix node (0x2622e50) writes +0x31c..+0x34b and
                    // then calls vtable slot 0x218 (actor, 0): what does it do?
                    const u64 vt = r.Get<u64>(a).value_or(0);
                    const u64 slot = vt ? r.Get<u64>(vt + 0x218).value_or(0) : 0;
                    LOG_INFO(Core, "coop: objects: share vtable {} main+{:#x}, slot 0x218 main+{:#x}",
                             known.first, vt - main_, slot - main_);
                }
            }
            if (known.first.starts_with("Player_")) {  // our own Link, for coop_world
                if (known.first == "Player_Child" || known.first == "Player_Adult") {
                    our_link_ = a;  // the ocarina probe (issue 152)
                }
                coop::protocol::Vec3 at{};
                for (std::size_t i = 0; i < 3; ++i) {
                    at[i] = r.Get<float>(a + kActorPosition + 4 * i).value_or(0.0f);
                }
                player = at;
                // Issue 88: the hearts the game shows come from Link's own Life
                // component; logged beside the MaxLife flag (coop_flags debug)
                // so one run shows whether a shared MaxLife reaches it.
                if (const auto value = LifeValue(r, a)) {
                    const s32 life = r.Get<s32>(value->first + 8).value_or(-1);
                    const s32 max = r.Get<s32>(value->second + 8).value_or(-1);
                    const s32 flag = TheMaxLife().load();
                    if (life >= 0 && max > 0) {  // OurLife (the HUD, PvP)
                        TheLife().life.store(life);
                        TheLife().max.store(max);
                        TheLife().at_us.store(CoopEvery::NowUs());
                    }
                    if (max != link_max_logged_ || flag != flag_max_logged_ ||
                        (debug_ && life != link_life_logged_)) {
                        link_max_logged_ = max;
                        link_life_logged_ = life;
                        flag_max_logged_ = flag;
                        LOG_INFO(Core,
                                 "coop: objects: our Link's life {} of {}, MaxLife flag {} (quarter hearts)",
                                 life, max, flag);
                    }
                    if (known.first == "Player_Child" || known.first == "Player_Adult") {
                        GivePvp(a, value->first + 8, life);  // PvP (coop_pvp_game.cpp)
                    }
                    // On by default (=0 turns it off): a MaxLife raised by
                    // another player is put into the component the hearts are
                    // drawn from, so a heart container crosses (issue 88).
                    const auto raised = hearts_to_link_ ? RaisedMaxLife(max, flag) : std::nullopt;
                    if (raised && memory_ != nullptr &&
                        memory_->IsValidVirtualAddressRange(value->second + 8, 4)) {
                        memory_->Write32(value->second + 8, static_cast<u32>(*raised));
                        LOG_INFO(Core, "coop: objects: our Link's max life {} -> {} (from MaxLife)",
                                 max, *raised);
                    }
                }
            }
            if (scrubs_on_ && (CoopScrubs::FindBrother(known.second) != nullptr ||
                               CoopScrubs::IsManager(known.second))) {
                CoopScrubs::Seen sc;  // issue 149: the Scrub Brothers and their manager
                sc.address = a;
                sc.hash = known.second;
                sc.id = r.Get<u32>(a + CoopScrubs::addr::kTransceiverId).value_or(0);
                if (const auto anim = Animation(r, a)) {
                    sc.animation = anim->name;
                }
                for (std::size_t i = 0; i < 3; ++i) {
                    sc.position[i] = r.Get<float>(a + kActorPosition + 4 * i).value_or(0.0f);
                }
                scrubs_seen_.push_back(std::move(sc));
            }
            if (gimmick_probe_on_ && CoopWorld::IsGimmick(known.first)) {
                CoopWorld::GimmickProbe::Gimmick g;
                g.address = a;
                g.hash = known.second;
                g.name = known.first;
                for (std::size_t i = 0; i < 3; ++i) {
                    g.position[i] = r.Get<float>(a + kActorPosition + 4 * i).value_or(0.0f);
                }
                gimmicks_.push_back(std::move(g));
            }
            if (IsShared(known.first) || CoopWorld::IsWorldObject(known.first) ||
                CoopWorld::IsBlockAnchor(known.first) ||  // moved with its block (issue 147)
                (known.second != 0 && known.second != ~u64{0} && sync_.Taken(known.second))) {
                Seen s;
                s.address = a;
                s.name = known.first;
                s.hash = known.second;
                for (std::size_t i = 0; i < 3; ++i) {
                    s.position[i] = r.Get<float>(a + kActorPosition + 4 * i).value_or(0.0f);
                }
                if (const auto m = r.Get<std::array<float, 9>>(a + kActorRotation)) {
                    s.rotation = *m;
                }
                s.animation = Animation(r, a);
                if (IsBoss(s.name)) {
                    s.slots = OtherSlots(r, a);  // issue 150
                }
                if (const auto value = LifeValue(r, a)) {
                    const auto life = r.Get<s32>(value->first + 8);
                    const auto max = r.Get<s32>(value->second + 8);
                    if (life && max) {
                        s.life = coop::protocol::ObjectLife{*life, *max};
                    }
                }
                out.push_back(std::move(s));
            }
            names.emplace(a, std::move(known));
        }
        names_ = std::move(names);
        sync_.SetPlayer(player);
        sync_.SetLures(CoopProjectiles::TheLures().Get());  // issue 151: our cast lures
        player_ = player;
        if (debug_) {  // research: the field grass that is no actor (world.md)
            for (const std::string& line : foliage_.Look(
                     [&r](u64 address) { return r.Get<u64>(address); }, main_, player)) {
                LOG_INFO(Core, "coop: {}", line);
            }
        }
        return true;
    }

    // AS slot 0 of an actor: command name, frame, rate (as PlayerReader).
    std::optional<coop::protocol::Animation> Animation(Reader& r, u64 a) {
        return AnimationAt(r, a, 0, false);
    }

    // The AS controller of an actor and its slot count.
    std::optional<std::pair<u64, s32>> AsController(Reader& r, u64 a) {
        const auto table = r.Ptr(a + kActorComponents);
        const auto as = table ? r.Ptr(*table + kComponentsAs) : std::nullopt;
        const auto inner = as ? r.Ptr(*as + kAsInner) : std::nullopt;
        const auto controller = inner ? r.Ptr(*inner + kAsController) : std::nullopt;
        if (!controller) {
            return std::nullopt;
        }
        const s32 count = static_cast<s32>(r.Get<u32>(*controller + kAsSlotCount).value_or(0));
        if (count < 1 || count > 64) {
            return std::nullopt;
        }
        return std::make_pair(*controller, count);
    }

    // Issue 150: a boss's AS slots after slot 0 (Gohma's eye guard and
    // eyelids), up to kMaxObjectSlots. A slot that plays nothing is sent with
    // an empty name (the copy leaves its own slot alone then).
    std::vector<coop::protocol::SlotAnimation> OtherSlots(Reader& r, u64 a) {
        std::vector<coop::protocol::SlotAnimation> out;
        const auto c = AsController(r, a);
        if (!c) {
            return out;
        }
        const s32 last = std::min<s32>(c->second - 1, static_cast<s32>(coop::protocol::kMaxObjectSlots));
        for (s32 i = 1; i <= last && i <= coop::protocol::kMaxSlotIndex; ++i) {
            if (auto anim = AnimationAt(r, a, i, true)) {
                out.push_back({static_cast<std::uint8_t>(i), std::move(*anim)});
            }
        }
        return out;
    }

    // AS slot `index` of an actor. empty_ok: a slot that plays nothing gives
    // an empty name (slot 0 gives nothing then).
    std::optional<coop::protocol::Animation> AnimationAt(Reader& r, u64 a, s32 index,
                                                         bool empty_ok) {
        const auto c = AsController(r, a);
        if (!c || index >= c->second) {
            return std::nullopt;
        }
        const auto array = r.Ptr(c->first + kAsSlotArray);
        const auto slot = array ? r.Ptr(*array + kAsSlotEntry * static_cast<u64>(index)) : std::nullopt;
        const auto name_at = slot ? r.Ptr(*slot + kAsSlotName) : std::nullopt;
        if (!name_at) {
            return std::nullopt;
        }
        const auto chunk = r.Get<std::array<char, 64>>(*name_at);
        if (!chunk) {
            return std::nullopt;
        }
        const std::size_t n = strnlen(chunk->data(), chunk->size());
        if (n == 0 && empty_ok) {
            return coop::protocol::Animation{std::string(), 0.0f, 0.0f};
        }
        if (n == 0 || n >= chunk->size()) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if ((*chunk)[i] < 0x20 || (*chunk)[i] > 0x7e) {
                return std::nullopt;
            }
        }
        const float frame = r.Get<float>(*slot + kAsSlotFrame).value_or(0.0f);
        const float rate = r.Get<float>(*slot + kAsSlotRate).value_or(1.0f);
        if (!std::isfinite(frame) || !std::isfinite(rate) || std::fabs(frame) > 1e5f ||
            std::fabs(rate) > 100.0f) {
            return std::nullopt;
        }
        return coop::protocol::Animation{std::string(chunk->data(), n), frame, rate};
    }

    // The life and max value entries ([life + 0x1c10], [life + 0x1c18]).
    std::optional<std::pair<u64, u64>> LifeValue(Reader& r, u64 a) {
        const auto table = r.Ptr(a + kActorComponents);
        const auto life = table ? r.Ptr(*table + kComponentLife) : std::nullopt;
        const auto value = life ? r.Ptr(*life + kLifeValue) : std::nullopt;
        const auto max = life ? r.Ptr(*life + kLifeMax) : std::nullopt;
        if (!value || !max) {
            return std::nullopt;
        }
        return std::pair{*value, *max};
    }

    // PvP: our Link's life `life` (at `life_at`) was just read. Notes it for
    // OurLinkLife, and gives the hurts the victim's rules decided: first as
    // the game's own hit (the hit replay, as an enemy copy gets one: Link
    // flinches and is knocked away from the attacker), then, if the game has
    // not taken it within kPvpHitWaitMs, by writing the life directly (as
    // docs/objects.md "Damage" found enough for any actor).
    void GivePvp(u64 link, u64 life_at, s32 life) {
        const u64 now = SteadyNowMs();
        TheLinkLife().store(life);
        TheLinkLifeAt().store(now == 0 ? 1 : now);
        if (life < 0) {
            return;  // not read: nothing is given until it is
        }
        std::vector<PvpAsk> asks;
        {
            std::lock_guard lock(PvpMutex());
            asks.swap(PvpAsks());
        }
        for (const PvpAsk& ask : asks) {
            PvpGive h;
            h.take = ask.take;
            h.expect = std::max<s32>(0, life - ask.take);
            h.asked_ms = now;
            if (replay_ && pvp_replay_) {
                Mover::Hit hit;
                hit.actor = link;
                hit.attacker = link;  // the attacker's puppet is another game's; our Link stands in
                hit.amount = ask.take;
                hit.at = ask.at;
                hit.from = ask.from;
                hit.exact = true;
                TheMover().AddHit(hit);
                h.replayed = true;
            }
            LOG_INFO(Core, "coop: pvp: our Link hurt by {} at life {} ({})", ask.take, life,
                     h.replayed ? "the game's hit" : "written");
            pvp_.push_back(h);
        }
        for (auto it = pvp_.begin(); it != pvp_.end();) {
            if (life <= it->expect) {
                if (it->replayed) {
                    LOG_INFO(Core, "coop: pvp: the game took the hit (life {})", life);
                }
                it = pvp_.erase(it);
                continue;
            }
            if (!it->replayed || now >= it->asked_ms + kPvpHitWaitMs) {
                if (memory_ != nullptr && memory_->IsValidVirtualAddressRange(life_at, 4)) {
                    memory_->Write32(life_at, static_cast<u32>(it->expect));
                    LOG_INFO(Core, "coop: pvp: our Link's life {} -> {}{}", life, it->expect,
                             it->replayed ? " (the game's hit was not taken)" : "");
                    life = it->expect;
                }
                it = pvp_.erase(it);
                continue;
            }
            ++it;
        }
    }

    // A direct write of life is enough for the game (docs/objects.md, "Damage").
    // A hurt is added to the damage the Life step takes off this frame.
    // The test script's kill steps (coop_testinput, test only): the nearest
    // actor of that name to our Link with a life takes a hit from our Link of
    // its whole life, through the game's damage, so it dies as from a real hit
    // (and drops what it drops: a Gold Skulltula its token).
    void TestKills(const std::vector<Seen>& seen, std::vector<LifeWrite>& writes) {
        std::vector<std::string> names;
        {
            std::lock_guard lock(KeyRefundMutex());
            names.swap(TestKillQueue());
        }
        for (const std::string& name : names) {
            const Seen* best = nullptr;
            float best_m = 0.0f;
            for (const Seen& s : seen) {
                if (s.name != name || !s.life || s.life->life <= 0) {
                    continue;
                }
                float m = 0.0f;
                if (player_) {
                    const float dx = s.position[0] - (*player_)[0];
                    const float dy = s.position[1] - (*player_)[1];
                    const float dz = s.position[2] - (*player_)[2];
                    m = std::sqrt(dx * dx + dy * dy + dz * dz);
                }
                if (!best || m < best_m) {
                    best = &s;
                    best_m = m;
                }
            }
            if (!best) {
                LOG_INFO(Core, "coop: test kill {}: none here with a life", name);
                continue;
            }
            LifeWrite w;
            w.address = best->address;
            w.life = 0;
            w.hurt = std::max<std::int32_t>(best->life->life, 1);
            w.from = player_;
            writes.push_back(w);
            LOG_INFO(Core, "coop: test kill {} {:#x} at {:.2f} {:.2f} {:.2f} ({:.1f} m, life {}): hit",
                     name, best->address, best->position[0], best->position[1], best->position[2],
                     best_m, best->life->life);
        }
    }

    void WriteLife(Core::Memory::Memory& memory, Reader& r, const LifeWrite& w) {
        const auto value = LifeValue(r, w.address);
        if (!value || !memory.IsValidVirtualAddressRange(value->first + 8, 4)) {
            return;
        }
        const s32 old = r.Get<s32>(value->first + 8).value_or(-1);
        if (w.hurt > 0 && replay_) {
            u64 player = 0;
            for (const auto& [a, known] : names_) {
                if (known.first == "Player_Child" || known.first == "Player_Adult") {
                    player = a;
                    break;
                }
            }
            if (player != 0) {
                Mover::Hit h;
                h.actor = w.address;
                h.attacker = player;
                h.amount = w.hurt;
                h.at = r.Get<std::array<float, 3>>(w.address + kActorPosition).value_or(h.at);
                h.from = r.Get<std::array<float, 3>>(player + kActorPosition).value_or(h.at);
                if (w.from) {
                    // The player who hit, so the enemy is knocked away from them.
                    h.from = *w.from;
                }
                TheMover().AddHit(h);
                return;  // the game takes the life; Sync writes it after 400 ms if not
            }
        }
        if (w.hurt > 0) {
            const auto table = r.Ptr(w.address + kActorComponents);
            const auto life = table ? r.Ptr(*table + kComponentLife) : std::nullopt;
            if (life && memory.IsValidVirtualAddressRange(*life + kLifeHurt, 4)) {
                const s32 pending = r.Get<s32>(*life + kLifeHurt).value_or(0);
                memory.Write32(*life + kLifeHurt, static_cast<u32>(pending + w.hurt));
                LOG_INFO(Core, "coop: objects: share hurt {:#x}: {} at life {} (to {})",
                         w.address, w.hurt, old, w.life);
                return;
            }
        }
        memory.Write32(value->first + 8, static_cast<u32>(w.life));
        LOG_INFO(Core, "coop: objects: share life {:#x}: {} -> {}", w.address, old, w.life);
    }

    // LICHII_COOP_OBJECTS_TEST_DRIVE: see the top of this file.
    void Drive(Core::System& system, u64 now_us) {
        u64 player = 0;
        for (const auto& [a, known] : names_) {
            if (known.first == "Player_Child" || known.first == "Player_Adult") {
                player = a;
                break;
            }
        }
        const auto owned = sync_.Owned();
        drive_pose_.reset();
        if (self_ != 1 || player == 0 || owned.empty()) {  // player 1's game only
            return;
        }
        u64 enemy = owned.front().first;
        for (const auto& [a, name] : owned) {
            if (name == "Enemy_ReDead") {
                enemy = a;
                break;
            }
        }
        auto& memory = system.ApplicationMemory();
        Reader r(memory);
        const auto at = r.Get<std::array<float, 3>>(player + kActorPosition);
        const auto m = r.Get<std::array<float, 9>>(player + kActorRotation);
        if (!at || !m || r.Get<u32>(enemy + kActorState).value_or(0) != kStateAlive) {
            return;
        }
        // Columns of the player's rotation: column 0 is his side, column 2
        // the way he faces (two-game test 2026-11-09: row 2 put the enemy
        // between him and the camera).
        // Mode 2: still, 2 m ahead, no hurts (for real sword hits).
        const bool still = drive_mode_ == 2;
        const float t = static_cast<float>(now_us % 4'000'000) / 4'000'000.0f;
        const float sway = still ? 0.0f : 1.5f * std::sin(t * 6.2831853f);
        const float ahead = still ? 2.0f : 3.0f;
        Mover::Pose pose;
        pose.actor = enemy;
        pose.transform = {(*at)[0] + ahead * (*m)[2] + sway * (*m)[0], (*at)[1],
                          (*at)[2] + ahead * (*m)[8] + sway * (*m)[6]};
        drive_pose_ = pose;
        if (now_us - drive_log_at_ >= 2'000'000) {
            drive_log_at_ = now_us;
            LOG_INFO(Core,
                     "coop: objects: test drive player at {:.2f} {:.2f} {:.2f} rot {:.2f} {:.2f} {:.2f} | "
                     "{:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f}",
                     (*at)[0], (*at)[1], (*at)[2], (*m)[0], (*m)[1], (*m)[2], (*m)[3], (*m)[4],
                     (*m)[5], (*m)[6], (*m)[7], (*m)[8]);
        }
        if (enemy != drive_enemy_) {
            drive_enemy_ = enemy;
            drive_hurt_at_ = now_us;
            LOG_INFO(Core, "coop: objects: test drive {:#x} {} in front of {:#x}", enemy,
                     names_[enemy].first, player);
        }
        if (!still && now_us - drive_hurt_at_ >= 4'000'000) {
            drive_hurt_at_ = now_us;
            const auto value = LifeValue(r, enemy);
            const s32 life = value ? r.Get<s32>(value->first + 8).value_or(0) : 0;
            if (life > 1) {
                WriteLife(memory, r, {enemy, life - 1, hurt_ ? 1 : 0, {}});
            }
        }
    }

    // Every frame: each copy we do not own goes where the owner's enemy was.
    // How far the game moved it since our last write is logged every 5 s: if
    // its AI or physics fight the write, that shows here.
    void Place(Core::System& system, u64 now_us) {
        std::vector<PoseWrite> poses;
        sync_.Poses(now_us, poses);
        // A placed world object goes where coop_world says, or stays where
        // this game has it: the enemy sharing does not place those.
        std::erase_if(poses, [this](const PoseWrite& w) { return world_.Places(w.address); });
        world_.Poses(now_us, poses);        auto& memory = system.ApplicationMemory();
        Reader r(memory);
        std::unordered_map<u64, coop::protocol::Vec3> placed;
        std::vector<Mover::Pose> wish;
        if (drive_pose_) {
            wish.push_back(*drive_pose_);
        }
        // Copies whose own AI wanted another animation (issue 157).
        for (const auto& [a, name] : TheMover().TakeTakeovers()) {
            sync_.NoteTakeover(a, now_us, name);
        }
        for (const PoseWrite& w : poses) {
            const u32 state = r.Get<u32>(w.address + kActorState).value_or(0);
            bool asleep = false;
            if (state != kStateAlive) {
                // Issue 101: our copy sleeps at its home far from our Link
                // while the owner's enemy chases another player up to us.
                // Skipping it left it there; move it when the owner's place
                // is near our Link, and the game wakes it there.
                if (!wake_far_ || state != kStateAsleep || !player_ ||
                    Apart(*player_, w.position) >= kAsleepPlaceM) {
                    continue;
                }
                asleep = true;
                ++asleep_placed_;
            }
            if (const auto before = placed_.find(w.address); before != placed_.end()) {
                coop::protocol::Vec3 now{};
                float d2 = 0.0f;
                for (std::size_t i = 0; i < 3; ++i) {
                    now[i] = r.Get<float>(w.address + kActorPosition + 4 * i).value_or(0.0f);
                    const float d = now[i] - before->second[i];
                    d2 += d * d;
                }
                const float d = std::sqrt(d2);
                // A copy whose own AI pulls it at our Link (issue 101).
                sync_.NoteDrift(w.address, before->second, now, now_us);
                drift_sum_ += d;
                if (d > drift_max_) {
                    drift_max_ = d;
                    drift_worst_ = w.address;
                }
                ++drift_count_;
            }
            Mover::Pose p;
            p.actor = w.address;
            p.asleep = asleep;
            std::copy(w.position.begin(), w.position.end(), p.transform.begin());
            if (w.rotation) {
                std::copy(w.rotation->begin(), w.rotation->end(), p.transform.begin() + 3);
                p.rotation = true;
            }
            wish.push_back(p);
            placed.emplace(w.address, w.position);
        }
        placed_ = std::move(placed);
        std::vector<PlayWish> plays;
        sync_.Plays(now_us, plays);
        if (ai_hold_ != 0) {
            TheMover().Hold(sync_.Holds(now_us), CoopHold::MasksFor(ai_hold_));
        }
        if (const auto ours = sync_.TakeOursNow(); release_ && !ours.empty()) {
            TheMover().Release(ours);
        }
        std::vector<u64> removes = sync_.Removes();
        for (const u64 a : world_.Removes()) {
            removes.push_back(a);
        }
        // Issue 144: nothing near our Link is removed, moved or made to play while
        // it is in an event; they come again each frame and are made after it.
        {
            const CoopGuard::Local local = GuardLocal();
            const auto where = [this](std::uint64_t a) { return Where(a); };
            CoopGuard::Skip(removes, local, where, [](u64 a) { return a; });
            // Issue 194: a world object another game moves (coop_world: a falling ladder,
            // a block and its base anchor) is no event's partner: it follows at once, else
            // it jumped after.
            const auto world = [this](std::uint64_t a) { return world_.Follows(a); };
            CoopGuard::Skip(wish, local, where, [](const Mover::Pose& p) { return p.actor; }, world);
            std::vector<u64> wanted_plays;
            wanted_plays.reserve(plays.size());
            for (const PlayWish& p : plays) {
                wanted_plays.push_back(p.address);
            }
            CoopGuard::Skip(plays, local, where, [](const PlayWish& p) { return p.address; }, world);
            if (wanted_plays.size() != plays.size()) {
                // A chest's open idle held back: its show-open clock waits too.
                std::set<u64> sent;
                for (const PlayWish& p : plays) {
                    sent.insert(p.address);
                }
                for (const u64 a : wanted_plays) {
                    if (!sent.contains(a)) {
                        sync_.ChestPlayHeld(a, now_us);
                    }
                }
            }
        }
        TheMover().Want(main_, std::move(wish), std::move(removes), std::move(plays));
        if (now_us - drift_at_ >= kDriftLogUs) {
            const auto [moved, deleted, played, framed] = TheMover().TakeCounts();
            if (drift_count_ > 0 || moved > 0 || deleted > 0 || played > 0 || framed > 0 ||
                sync_.HurtsTaken() + sync_.HurtsMissed() > 0) {
                LOG_INFO(Core,
                         "coop: objects: share placed {} copies; off from where we put them "
                         "avg {:.3f} max {:.3f} over {}; game calls: {} moves, {} deletes, {} "
                         "plays, {} frame sets; hurts taken {} missed {}",
                         placed_.size(), drift_count_ ? drift_sum_ / drift_count_ : 0.0f,
                         drift_max_, drift_count_, moved, deleted, played, framed, sync_.HurtsTaken(),
                         sync_.HurtsMissed());
            }
            // Issue 101: which copy was furthest from where we put it, so
            // one run tells a copy the setter does not hold (a flying or
            // sleeping enemy) from the AI's own small pull.
            if (asleep_placed_ > 0) {
                LOG_INFO(Core,
                         "coop: objects: share {} copy-frames placed while asleep (owner's "
                         "place within {:.0f} m of our Link, issue 101)",
                         asleep_placed_, kAsleepPlaceM);
                asleep_placed_ = 0;
            }
            if (debug_ && drift_worst_ != 0 && drift_max_ >= kDriftWorstM) {
                LOG_INFO(Core, "coop: objects: share worst copy {:#x} '{}' off {:.2f} m", drift_worst_,
                         r.Name(drift_worst_), drift_max_);
            }
            if (release_) {
                const auto [cleared, clear, dropped] = TheMover().TakeReleases();
                if (cleared + clear + dropped > 0) {
                    LOG_INFO(Core,
                             "coop: objects: release: {} that became ours had bit 23 set "
                             "(cleared), {} had it clear, {} skipped (over 64 in a frame)",
                             cleared, clear, dropped);
                }
            }
            if (ai_hold_ != 0) {
                const auto [frames, kept] = TheMover().TakeHolds();
                LOG_INFO(Core,
                         "coop: objects: ai hold mode {}: {} copy-frames held, brain skip bit as "
                         "asked in {}; {} plays in {:.1f} s",
                         ai_hold_, frames, kept, played,
                         static_cast<double>(now_us - drift_at_) / 1e6);
            }
            drift_at_ = now_us;
            drift_sum_ = 0.0f;
            drift_max_ = 0.0f;
            drift_worst_ = 0;
            drift_count_ = 0;
        }
    }

    // The cheat items (coop_cheat_items.h), topped up once a second.
    void Cheat(Core::System& system) {
        CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_);
        for (const auto& note : CoopCheatItems::TopUp(memory, cheat_state_)) {
            LOG_INFO(Core, "coop: objects: {}", note);
        }
    }

    static u64 FindMain(Core::System& system, Kernel::KProcess* process) {
        Loader::AppLoader::Modules modules;
        if (system.GetAppLoader().ReadNSOModules(modules) != Loader::ResultStatus::Success ||
            modules.empty()) {
            modules = Core::FindModules(process);
        }
        for (const auto& [address, name] : modules) {
            if (name == "main" || name.starts_with("main.")) {
                return address;
            }
        }
        return GetInteger(Core::FindMainModuleEntrypoint(process));
    }

    // The actors on the live list. False when the list changed under us.
    bool List(Reader& r, std::vector<u64>& out) {
        const auto manager = r.Ptr(main_ + kActorManagerPtr);
        if (!manager) {
            return false;
        }
        const auto lists = r.Ptr(*manager + kManagerLists);
        if (!lists) {
            return false;
        }
        const u64 head = *lists + kListHead;
        const auto count = r.Get<u32>(head + kListCount);
        if (!count) {
            return false;
        }
        std::size_t walked = 0;
        auto node = r.Ptr(head);
        while (node && *node != head && walked < kMaxWalk) {
            ++walked;
            const auto owner = r.Get<u64>(*node + kNodeOwner);
            if (owner && *owner + kActorNode == *node) {
                out.push_back(*owner);
            }
            node = r.Ptr(*node);
        }
        const bool whole = node && walked == *count;
        if (first_ && whole) {
            LOG_INFO(Core, "coop: objects: list head {:#x}, {} entries, {} actors", head, walked,
                     out.size());
        }
        return whole;
    }

    void Walk(Reader& r) {
        std::vector<u64> list;
        if (!List(r, list)) {
            return;  // the list changed under us; try the next frame
        }
        const std::unordered_set<u64> seen(list.begin(), list.end());
        for (const u64 a : seen) {
            auto [it, added] = actors_.try_emplace(a);
            Actor& actor = it->second;
            actor.missed = 0;
            if (added) {
                actor.name = r.Name(a);
                const auto placement = r.Ptr(a + kActorPlacement);
                actor.hash = placement ? r.Get<u64>(*placement + kPlacementHash).value_or(0) : 0;
                if (!first_) {
                    LogPose(r, "+", a, actor);
                }
            }
            if (IsShared(actor.name)) {  // enemies and breakables
                Life(r, a, actor);
            }
        }
        for (auto it = actors_.begin(); it != actors_.end();) {
            if (seen.contains(it->first) || ++it->second.missed < 2) {
                ++it;
                continue;
            }
            LOG_INFO(Core, "coop: objects: - {} {:#x} hash {:#x}", it->second.name, it->first,
                     it->second.hash);
            it = actors_.erase(it);
        }
        first_ = false;
    }

    void LogPose(Reader& r, const char* what, u64 a, const Actor& actor) {
        const auto x = r.Get<float>(a + kActorPosition);
        const auto y = r.Get<float>(a + kActorPosition + 4);
        const auto z = r.Get<float>(a + kActorPosition + 8);
        const auto state = r.Get<u32>(a + kActorState);
        LOG_INFO(Core, "coop: objects: {} {} {:#x} state {} at {:.2f} {:.2f} {:.2f} hash {:#x}",
                 what, actor.name, a, state.value_or(0), x.value_or(0.0f), y.value_or(0.0f),
                 z.value_or(0.0f), actor.hash);
    }

    void Life(Reader& r, u64 a, Actor& actor) {
        const auto table = r.Ptr(a + kActorComponents);
        const auto life = table ? r.Ptr(*table + kComponentLife) : std::nullopt;
        if (!life) {
            return;
        }
        const auto value = r.Ptr(*life + kLifeValue);
        const auto max = r.Ptr(*life + kLifeMax);
        if (!value || !max) {
            return;
        }
        const std::array<s32, 4> now{
            r.Get<s32>(*value + 8).value_or(-1), r.Get<s32>(*max + 8).value_or(-1),
            r.Get<s32>(*life + kLifeCopy).value_or(-1), r.Get<s32>(*life + kLifeCopy + 4).value_or(-1)};
        if (actor.life && (*actor.life)[0] == now[0] && (*actor.life)[1] == now[1]) {
            return;  // the copy alone moves a frame or two later; not logged
        }
        LOG_INFO(Core, "coop: objects: life {} {:#x} hash {:#x}: {} -> {} of {} (copy {} of {})",
                 actor.name, a, actor.hash, actor.life ? (*actor.life)[0] : now[0], now[0], now[1],
                 now[2], now[3]);
        if (actor.life && now[0] < (*actor.life)[0]) {
            // A hit: the hit record the Life step reads (+0x1f0..+0x2f0) and
            // the copy it keeps of the last one (+0x1b00..+0x1bb0), in hex.
            for (const auto [from, to] : {std::pair<u64, u64>{0x1f0, 0x2f0}, {0x1b00, 0x1bb0}}) {
                std::string hex;
                for (u64 o = from; o < to; o += 4) {
                    hex += fmt::format("{:08x}{}", r.Get<u32>(*life + o).value_or(0),
                                       (o - from) % 32 == 28 ? " | " : " ");
                }
                LOG_INFO(Core, "coop: objects: hit record {} +{:#x}: {}", actor.name, from, hex);
            }
        }
        actor.life = now;
    }

    std::unordered_map<u64, coop::protocol::Vec3> placed_;  // what we wrote last frame
    u8 self_ = 0;
    std::unordered_set<std::string> vtables_logged_;
    std::unordered_map<u64, std::string> anim_logged_;
    unsigned anim_lines_ = 0;
    std::optional<Mover::Pose> drive_pose_;
    u64 drive_enemy_ = 0;
    u64 drive_log_at_ = 0;
    u64 drive_hurt_at_ = 0;
    float drift_sum_ = 0.0f;
    float drift_max_ = 0.0f;
    u64 drift_worst_ = 0;  // the copy at drift_max_
    static constexpr float kDriftWorstM = 1.0f;
    std::size_t drift_count_ = 0;
    u64 drift_at_ = 0;
    CoopEvery::Every scan_{CoopEvery::kTwoFramesUs};
    CoopEvery::Every cheat_every_{CoopEvery::kSecondUs};
    CoopCheatItems::State cheat_state_;
    CoopEvery::Every share_every_{CoopEvery::kTwoFramesUs};
    bool sharing_ = false;
    std::size_t blast_drops_logged_ = 0;
    unsigned chests_twice_seen_ = 0;  // sync_.ChestsOpenedTwice() passed on (issue 195)
    std::size_t present_logged_ = ~std::size_t{0};
    std::pair<std::size_t, std::string> fish_logged_;  // the last FISH_LURE logged
    std::vector<coop::protocol::ObjectGroupEntry> group_logged_;
    CoopObjects::OwnerLog owner_log_;  // network thread
    std::size_t enemies_logged_ = 0;
    std::size_t refused_logged_ = 0;
    std::uint8_t decider_logged_ = 0;
    std::unordered_map<u64, std::pair<std::string, u64>> names_;  // name, hash by actor
    Sync sync_;
    Kernel::KProcess* process_ = nullptr;
    u64 main_ = 0;
    bool first_ = true;
    std::unordered_map<u64, Actor> actors_;
};

Objects& Instance() {
    static Objects objects;  // first called on the present thread
    static const bool once = [] {
        objects.debug_ = EnvFlag("LICHII_COOP_DEBUG_OBJECTS");
        objects.cheat_ = EnvFlag("LICHII_COOP_OBJECTS_CHEAT");
        objects.share_ = EnvFlag("LICHII_COOP_OBJECTS");
        objects.pose_ = EnvOn("LICHII_COOP_OBJECTS_POSE");
        objects.gimmick_probe_on_ = EnvFlag("LICHII_COOP_GIMMICK_PROBE");  // opt-in: many reads
        objects.scrubs_on_ = EnvOn("LICHII_COOP_SCRUBS");  // issue 149, on unless =0
        objects.ocarina_probe_on_ = EnvFlag("LICHII_COOP_OCARINA_PROBE");  // issue 152, reads only
        // On by default (=0 turns it off): a life an enemy must lose is given
        // to the game as a whole hit, so it flinches, is knocked back and a
        // Stalchild loses its head on every screen. An amount alone at
        // Life + 0x228 is not taken (live test, 2026-11-09).
        objects.replay_ = EnvOn("LICHII_COOP_OBJECTS_HIT_REPLAY");
        objects.hurt_ = EnvFlag("LICHII_COOP_OBJECTS_HURT") || objects.replay_;
        objects.drive_ = EnvFlag("LICHII_COOP_OBJECTS_TEST_DRIVE");
        objects.ai_hold_ = EnvNumber("LICHII_COOP_OBJECTS_AI_HOLD");
        objects.test_take_ = EnvHex("LICHII_COOP_OBJECTS_TEST_TAKE");
        objects.drive_mode_ = objects.drive_ ? EnvNumber("LICHII_COOP_OBJECTS_TEST_DRIVE") : 0;
        objects.hearts_to_link_ = EnvOn("LICHII_COOP_HEARTS_TO_LINK");
        objects.wake_ = EnvOn("LICHII_COOP_OBJECTS_WAKE");
        objects.release_ = EnvOn("LICHII_COOP_OBJECTS_RELEASE");
        objects.pvp_replay_ = EnvOn("LICHII_COOP_PVP_REPLAY");
        objects.wake_far_ = EnvOn("LICHII_COOP_OBJECTS_ASLEEP");
        return true;
    }();
    (void)once;
    return objects;
}

} // namespace

void SetLocalEvent(bool in_event) {
    TheLocalEvent().store(in_event);
}

std::optional<LinkHearts> OurLife() {
    const u64 at = TheLife().at_us.load();
    if (at == 0 || CoopEvery::NowUs() - at > 2'000'000) {
        return std::nullopt;
    }
    return LinkHearts{TheLife().life.load(), TheLife().max.load()};
}

void SetMaxLife(std::optional<std::int32_t> quarters) {
    TheMaxLife().store(quarters.value_or(-1));
}

std::optional<std::int32_t> OurLinkLife() {
    const u64 at = TheLinkLifeAt().load();
    const s32 life = TheLinkLife().load();
    if (at == 0 || life < 0 || SteadyNowMs() > at + kLinkLifeStaleMs) {
        return std::nullopt;
    }
    return life;
}

void HurtOurLink(std::int32_t quarters, const std::array<float, 3>& at,
                 const std::array<float, 3>& from) {
    if (quarters <= 0) {
        return;
    }
    std::lock_guard lock(PvpMutex());
    if (PvpAsks().size() < 16) {
        PvpAsks().push_back({quarters, at, from});
    }
}

void OnFrame(Core::System& system) {
    Objects& objects = Instance();
    if (objects.debug_ || objects.cheat_) {
        objects.Frame(system);  // the present thread only
    }
}

void Share(Core::System& system, coop::host::Session* session,
           const std::optional<std::string>& scene, std::uint64_t server_now_us) {
    Objects& objects = Instance();
    if (objects.share_) {
        objects.Share(system, session, scene, server_now_us);
    }
}

std::vector<std::uint64_t> OwnedActors() {
    Objects& objects = Instance();
    std::vector<std::uint64_t> out;
    if (objects.share_) {
        for (const auto& [address, name] : objects.OwnedNow()) {
            out.push_back(address);
        }
    }
    return out;
}

void OwnCountDrops(const std::vector<std::uint32_t>& key_flags) {
    if (key_flags.empty()) {
        return;
    }
    std::lock_guard lock(KeyRefundMutex());
    auto& drops = OwnDropQueue();
    drops.insert(drops.end(), key_flags.begin(), key_flags.end());
}

void QueueTestKill(std::string name) {
    std::lock_guard lock(KeyRefundMutex());
    if (TestKillQueue().size() < 16) {
        TestKillQueue().push_back(std::move(name));
    }
}

void ForgetKeptBreaks() {
    std::lock_guard lock(KeyRefundMutex());
    ForgetKeptFlag() = true;
}

std::vector<std::uint32_t> TakeKeyRefunds() {
    std::lock_guard lock(KeyRefundMutex());
    std::vector<std::uint32_t> out;
    out.swap(KeyRefundQueue());
    return out;
}

unsigned TakeChestsOpenedTwice() {
    return TheChestsOpenedTwice().exchange(0);
}

void TakenForOthers(const std::vector<std::uint64_t>& keys) {
    if (keys.empty()) {
        return;
    }
    Objects& objects = Instance();
    for (const std::uint64_t key : keys) {
        objects.Take(key);
    }
}

void OnEvent(const coop::net::Event& event) {
    Objects& objects = Instance();
    if (event.kind == coop::net::EventKind::reconnected) {
        objects.ForgetOwnerLog();  // a restarted server may hand out the same owner and epoch again
    }
    if (event.kind != coop::net::EventKind::message) {
        return;
    }
    if (objects.share_) {
        objects.Receive(event.message);
    }
}

namespace {

// The bosses of the last walk, for LastBossAlive (issue 109).
std::mutex g_bosses_mutex;
std::vector<Seen> g_bosses;
std::uint64_t g_bosses_at = 0;
constexpr std::uint64_t kBossesFreshUs = 1'500'000;

} // namespace

void NoteBosses(const std::vector<Seen>& seen) {
    std::vector<Seen> bosses;
    for (const Seen& s : seen) {
        if (s.name.starts_with("Enemy_DungeonBoss_")) {
            bosses.push_back(s);
        }
    }
    std::lock_guard lock(g_bosses_mutex);
    g_bosses = std::move(bosses);
    g_bosses_at = CoopEvery::NowUs();
}

std::optional<bool> LastBossAlive(std::span<const std::string_view> bodies) {
    std::lock_guard lock(g_bosses_mutex);
    const std::uint64_t now = CoopEvery::NowUs();
    if (g_bosses_at == 0 || now < g_bosses_at || now - g_bosses_at > kBossesFreshUs) {
        return std::nullopt;
    }
    return BossAlive(g_bosses, bodies);
}

} // namespace CoopObjects
