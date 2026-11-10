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
#include "coop_flags.h"
#include "coop_hold.h"
#include "coop_puppet.h"
#include "coop_svc.h"

#include <algorithm>
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
#include "coop_spawn.h"
#include "coop_world.h"

namespace CoopObjects {
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
// The game's own way to move an actor, as the AI node ExecuteActorForceSetMtx
// (run 0x2622e50) does it: write +0x31c..+0x34b (place, then the 3x3), then
// call vtable slot 0x218 (actor, 0). Every actor seen shares the vtable
// 0x4b25e18, whose slot 0x218 is 0x4c6580: it sets bit 23 of +0x4a8 and hands
// the new transform to the physics component (0x4c6630). Checked live: a
// plain write of +0x31c is undone by the game within a frame.
constexpr u64 kActorVtable = 0x4b25e18;
constexpr u64 kSetMatrix = 0x4c6580;
constexpr u64 kRequestDelete = 0xa6490;  // (actor, 0), as docs/puppet.md
// Rolled enemies' copies in the spawn director: their keys have the top bit.
constexpr u64 kEnemyKey = u64{1} << 63;
constexpr unsigned kMoverCallerOrder = 30;  // after the puppets, spawn copies (10) and fairies (20)
constexpr unsigned kGrassCallerOrder = 40;  // the others' field grass cuts (coop_world.h)

CoopWorld::GrassSync& TheGrass() {
    static CoopWorld::GrassSync grass;
    return grass;
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
constexpr unsigned kEveryFrames = 2;

// LICHII_COOP_OBJECTS_CHEAT: PouchData.<item>.StockNum and IsGet.<item> hashes
// from docs/items.md, and the count each is kept at. Entries with is_get 0 are
// the ammo capacities (unnamed struct 0xf80f8077, docs/items.md): without one
// the item is in the menu but the game gives none (bombs: Link lifts empty hands).
struct CheatItem {
    const char* name;
    u32 stock;
    u32 is_get;
    s32 count;
};
constexpr std::array<CheatItem, 10> kCheatItems{{
    {"Weapon_Range_Bow", 0x938d56bf, 0x69ec28be, 1},
    {"Item_Quiver_Small", 0xa8c3e1fb, 0x0a550531, 1},
    {"Item_Arrow_Normal", 0x518ec74b, 0x572701ea, 30},
    {"capacity Arrow", 0x418db791, 0, 30},
    {"Item_BombBag_Small", 0xb0a1bf51, 0xa63f1e2d, 1},
    {"Item_Bomb", 0xdb1bcd4f, 0x2bb00768, 20},
    {"capacity Bomb", 0xe753508b, 0, 20},
    {"Weapon_Range_SlingShot", 0x80d34ce9, 0x0ec22ee1, 1},
    {"Item_DekuSeed", 0x5fadf7fc, 0x29888e1e, 30},
    {"capacity DekuSeed", 0xae8cc742, 0, 30},
}};
constexpr unsigned kCheatEveryFrames = 60;

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
            const auto [actor, frame] = *frame_after_;
            frame_after_.reset();
            if (const u64 controller = Controller(guest, actor); controller != 0) {
                return SetFrame(controller, frame);
            }
        }
        if (!after_call) {
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
        while (next_play_ < work_plays_.size() && block_ != 0) {
            const PlayWish& p = work_plays_[next_play_++];
            if (!Alive(guest, p.address)) {
                continue;
            }
            const u64 controller = Controller(guest, p.address);
            if (controller == 0) {
                continue;
            }
            // Checked every game frame: the copy's own AI may have put its
            // own animation back since the last one.
            const float rate = p.rate;
            const u64 since = now_ms >= want_ms_ ? std::min<u64>(now_ms - want_ms_, 100) : 0;
            const float frame =
                FrameAfter(p.frame, rate, static_cast<std::int64_t>(since) * 1000);
            if (SlotName(guest, controller) == p.name) {
                auto& checked = checked_sample_[p.address];
                if (checked == p.sample) {
                    continue;
                }
                checked = p.sample;
                float now_frame = 0.0f;
                if (SlotFrame(guest, controller, now_frame) &&
                    std::fabs(now_frame - frame) <= kFrameSlack) {
                    continue;
                }
                ++framed_;
                return SetFrame(controller, frame);
            }
            const u64 text = Cell(guest, p.name);
            if (text == 0) {
                continue;
            }
            std::array<std::uint8_t, 0x30> request{};
            const float unset = -1.0f;
            const std::int32_t index = 0;
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
            auto& last = played_at_[p.address];
            if (last.first != p.name || now_ms - last.second >= kPlayLogMs) {
                LOG_INFO(Core, "coop: objects: copy {:#x} plays '{}' at frame {:.1f}", p.address,
                         p.name, frame);
                last = {p.name, now_ms};
            }
            checked_sample_[p.address] = p.sample;
            frame_after_ = std::make_pair(p.address, frame);
            CoopPuppet::Call c;
            c.function = main_ + kPlayAs;
            c.x[0] = block_ + kBlockRequest;
            return c;
        }
        while (next_remove_ < work_removes_.size()) {
            const u64 a = work_removes_[next_remove_++];
            asked_.insert(a);
            if (!Alive(guest, a)) {
                continue;
            }
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
            if (!Alive(guest, p.actor)) {
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

    CoopPuppet::Call SetFrame(u64 controller, float frame) {
        CoopPuppet::Call c;
        c.function = main_ + kSetAsFrame;
        c.x[0] = controller;
        c.x[1] = 0;  // slot 0
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

    u64 Slot0(CoopPuppet::Guest& guest, u64 controller) {
        u64 array = 0;
        u64 slot = 0;
        if (!guest.Read(controller + kAsSlotArray, &array, 8) || array == 0 ||
            !guest.Read(array, &slot, 8)) {
            return 0;
        }
        return slot;
    }

    // Slot 0's command name ("" when unreadable).
    std::string SlotName(CoopPuppet::Guest& guest, u64 controller) {
        const u64 slot = Slot0(guest, controller);
        u64 at = 0;
        std::array<char, 64> text{};
        if (slot == 0 || !guest.Read(slot + kAsSlotName, &at, 8) || at == 0 ||
            !guest.Read(at, text.data(), text.size())) {
            return {};
        }
        return std::string(text.data(), strnlen(text.data(), text.size()));
    }

    bool SlotFrame(CoopPuppet::Guest& guest, u64 controller, float& frame) {
        const u64 slot = Slot0(guest, controller);
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
    bool Alive(CoopPuppet::Guest& guest, u64 a) {
        u32 state = 0;
        u64 owner = 0;
        u64 vtable = 0;
        return a != 0 && guest.Read(a + kActorState, &state, 4) && state == kStateAlive &&
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
    std::optional<std::pair<u64, float>> frame_after_;  // actor, frame: set after the play
    std::unordered_map<u64, std::uint64_t> checked_sample_;  // actor: sample time checked
    std::unordered_map<u64, std::pair<std::string, u64>> played_at_;  // actor: name, ms
    std::unordered_map<std::string, u64> cells_;
    u64 block_ = 0;
    bool allocating_ = false;
    bool alloc_tried_ = false;
    std::vector<Pose> work_;
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
    std::unordered_set<u64> held_before_;  // BSA components in last frame's table
    std::size_t hold_set_ = 0;
    std::size_t hold_kept_ = 0;
};

Mover& TheMover() {
    static Mover mover;
    return mover;
}

class Objects {
public:
    void Frame(Core::System& system) {
        if (++frame_ % kEveryFrames != 0 || !Ready(system)) {
            return;
        }
        if (cheat_ && frame_ % kCheatEveryFrames == 0) {
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
        if (++share_frame_ % kEveryFrames != 0) {
            return;
        }
        Reader r(system.ApplicationMemory());
        std::vector<Seen> seen;
        if (!Collect(r, seen)) {
            return;
        }
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
                return true;
            }();
            (void)registered;
            CoopSpawn::UseDirector();  // makes the rolled enemies another game decided
            sync_.SetHurt(hurt_);
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
        {
            std::vector<CoopSpawn::Copy> enemies;
            for (const EnemyCopy& e : sync_.Creates()) {
                CoopSpawn::Copy c;
                c.key = e.key | kEnemyKey;
                c.actor = e.name;
                c.position = e.position;
                c.act = e.removed ? CoopSpawn::Act::remove : CoopSpawn::Act::show;
                c.place = false;
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
            world_.Tick(*scene, self, server_now_us, seen, player_, world);
            for (auto& m : world.send) {
                send.emplace_back(std::move(m));
            }
            writes.insert(writes.end(), world.lives.begin(), world.lives.end());
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
        }        // Field grass (docs/world.md): our new cuts go out, the others' are
        // made at the svc cave.
        for (auto& cut : TheGrass().Look([&r](u64 address) { return r.Get<u64>(address); }, main_,
                                         *scene, SteadyNowMs())) {
            LOG_INFO(Core, "coop: grass: cut at {:.2f} {:.2f} {:.2f} radius {:.2f} flags {:#x} sent",
                     cut.position[0], cut.position[1], cut.position[2], cut.radius, cut.flags);
            {
                CoopWorld::WorldSync::Out swing;  // a sign the sword passed (docs/world.md)
                world_.Swing(cut, player_, server_now_us, swing);
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
                    LOG_INFO(Core, "coop: objects: rolled made {} gone{}", rm->id.value,
                             sent ? "" : " (not sent)");
                } else if (const auto* d = std::get_if<coop::protocol::Damage>(&m)) {
                    LOG_INFO(Core, "coop: objects: share damage {} to {:#x} epoch {}{}", d->amount,
                             d->id.value, d->epoch, sent ? "" : " (not sent)");
                } else if (const auto* p = std::get_if<coop::protocol::ObjectPresent>(&m);
                           p != nullptr && p->objects.size() != present_logged_) {
                    present_logged_ = p->objects.size();
                    LOG_INFO(Core, "coop: objects: share present {} enemies in {}{}",
                             p->objects.size(), p->scene, sent ? "" : " (not sent)");
                }
            }
        });
    }

    // Network thread.
    void Receive(const coop::protocol::Message& m) {
        if (const auto* o = std::get_if<coop::protocol::ObjectOwner>(&m)) {
            LOG_INFO(Core, "coop: objects: share owner {:#x} is player {} epoch {}", o->id.value,
                     o->owner, o->epoch);
        } else if (const auto* d = std::get_if<coop::protocol::Damage>(&m)) {
            LOG_INFO(Core, "coop: objects: share got damage {} for {:#x}", d->amount,
                     d->id.value);
        } else if (const auto* g = std::get_if<coop::protocol::GrassCut>(&m)) {
            LOG_INFO(Core, "coop: grass: got cut from P{} in {} at {:.2f} {:.2f} {:.2f} radius {:.2f}",
                     g->player_number, g->scene, g->position[0], g->position[1], g->position[2],
                     g->radius);
            TheGrass().Receive(*g);
            return;
        } else if (std::holds_alternative<coop::protocol::WorldBreak>(m) ||
                   std::holds_alternative<coop::protocol::WorldPlace>(m) ||
                   std::holds_alternative<coop::protocol::WorldHit>(m) ||
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
    std::optional<coop::protocol::Vec3> player_;  // our Link in the last walk
    int world_lines_ = 0;
    CoopWorld::FoliageProbe foliage_;
    bool cheat_ = false;
    bool share_ = false;

    // Present thread: the shared things this game owns (for coop_drops).
    auto OwnedNow() const { return sync_.Owned(); }
    bool pose_ = true;
    bool hurt_ = true;
    bool replay_ = false;
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
            LOG_INFO(Core, "coop: objects: main at {:#x}", main_);
        }
        return true;
    }

    // The shared actors of one whole walk, with life and place. False when
    // the list changed under us (try the next frame).
    bool Collect(Reader& r, std::vector<Seen>& out) {
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
                coop::protocol::Vec3 at{};
                for (std::size_t i = 0; i < 3; ++i) {
                    at[i] = r.Get<float>(a + kActorPosition + 4 * i).value_or(0.0f);
                }
                player = at;
            }
            if (IsShared(known.first) || CoopWorld::IsMovable(known.first) ||
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
        const auto table = r.Ptr(a + kActorComponents);
        const auto as = table ? r.Ptr(*table + kComponentsAs) : std::nullopt;
        const auto inner = as ? r.Ptr(*as + kAsInner) : std::nullopt;
        const auto controller = inner ? r.Ptr(*inner + kAsController) : std::nullopt;
        if (!controller || static_cast<s32>(r.Get<u32>(*controller + kAsSlotCount).value_or(0)) < 1) {
            return std::nullopt;
        }
        const auto array = r.Ptr(*controller + kAsSlotArray);
        const auto slot = array ? r.Ptr(*array) : std::nullopt;
        const auto name_at = slot ? r.Ptr(*slot + kAsSlotName) : std::nullopt;
        if (!name_at) {
            return std::nullopt;
        }
        const auto chunk = r.Get<std::array<char, 64>>(*name_at);
        if (!chunk) {
            return std::nullopt;
        }
        const std::size_t n = strnlen(chunk->data(), chunk->size());
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

    // A direct write of life is enough for the game (docs/objects.md, "Damage").
    // A hurt is added to the damage the Life step takes off this frame.
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
        for (const PoseWrite& w : poses) {
            if (r.Get<u32>(w.address + kActorState).value_or(0) != kStateAlive) {
                continue;
            }
            if (const auto before = placed_.find(w.address); before != placed_.end()) {
                float d2 = 0.0f;
                for (std::size_t i = 0; i < 3; ++i) {
                    const float d =
                        r.Get<float>(w.address + kActorPosition + 4 * i).value_or(0.0f) -
                        before->second[i];
                    d2 += d * d;
                }
                const float d = std::sqrt(d2);
                drift_sum_ += d;
                drift_max_ = std::max(drift_max_, d);
                ++drift_count_;
            }
            Mover::Pose p;
            p.actor = w.address;
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
        std::vector<u64> removes = sync_.Removes();
        for (const u64 a : world_.Removes()) {
            removes.push_back(a);
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
            drift_count_ = 0;
        }
    }

    // Tops up each cheat item below its count. A read that fails means the
    // flag manager is not up yet (title screen): try again next time.
    void Cheat(Core::System& system) {
        CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_);
        coop::host::GameFlags flags(memory);
        for (const CheatItem& item : kCheatItems) {
            const auto now = flags.get_int(item.stock);
            if (!now || *now >= item.count) {
                continue;
            }
            const bool set = coop::host::GameFlags::set_int(memory, item.stock, item.count);
            if (item.is_get != 0 && flags.get_bool(item.is_get) == false) {
                coop::host::GameFlags::set_bool(memory, item.is_get, true);
            }
            LOG_INFO(Core, "coop: objects: cheat {} {} -> {}{}", item.name, *now, item.count,
                     set ? "" : " (queue busy, again next second)");
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
    std::size_t drift_count_ = 0;
    u64 drift_at_ = 0;
    unsigned frame_ = 0;
    unsigned share_frame_ = 0;
    bool sharing_ = false;
    std::size_t present_logged_ = ~std::size_t{0};
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
        return true;
    }();
    (void)once;
    return objects;
}

} // namespace

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

void OnEvent(const coop::net::Event& event) {
    if (event.kind != coop::net::EventKind::message) {
        return;
    }
    Objects& objects = Instance();
    if (objects.share_) {
        objects.Receive(event.message);
    }
}

} // namespace CoopObjects
