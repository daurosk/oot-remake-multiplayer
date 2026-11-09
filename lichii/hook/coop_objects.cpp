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
//   off by default (LICHII_COOP_OBJECTS_HURT=1 tries it): the game needs a
//   whole hit record there, so life is written directly.
// LICHII_COOP_OBJECTS_TEST_DRIVE=1 (two-game tests only): on the game that
// LICHII_COOP_OBJECTS_TEST_TAKE=<hex key> (tests only): on player 2's game,
// 10 s after sharing starts, acts as if that 64-bit placement key arrived
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
#include "coop_puppet.h"
#include "coop_svc.h"

#include <algorithm>
#include <array>
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
constexpr unsigned kMoverCallerOrder = 20;  // after the puppets and spawn copies (10)
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
constexpr u64 kMalloc = 0x14c6c90;
// The mover's block from the game's malloc: the play request, then one
// fixed cell per animation name (a slot may keep the name's pointer).
constexpr u64 kBlockRequest = 0x0;
constexpr u64 kBlockNames = 0x40;
constexpr u64 kNameCell = 0x50;
constexpr u64 kNameCells = 400;
constexpr u64 kBlockSize = kBlockNames + kNameCell * kNameCells;
constexpr u64 kReplayMs = 250;  // the copy's own AI took over: play again at most this often
constexpr u64 kDriftLogUs = 5'000'000;
constexpr u64 kActorComponents = 0x270;
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

    void Want(u64 main, std::vector<Pose> poses, std::vector<u64> removes,
              std::vector<PlayWish> plays) {
        std::lock_guard lock(mutex_);
        main_ = main;
        poses_ = std::move(poses);
        removes_ = std::move(removes);
        plays_ = std::move(plays);
    }

    // Calls made since the last ask (for the 5 s log line).
    std::array<std::size_t, 3> TakeCounts() {
        std::lock_guard lock(mutex_);
        const std::array<std::size_t, 3> counts{moved_, deleted_, played_};
        moved_ = 0;
        deleted_ = 0;
        played_ = 0;
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
            work_ = poses_;
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
        while (next_play_ < work_plays_.size() && block_ != 0) {
            const PlayWish& p = work_plays_[next_play_++];
            if (!Alive(guest, p.address)) {
                continue;
            }
            auto& last = played_at_[p.address];
            if (last.first == p.name && now_ms - last.second < kReplayMs) {
                continue;
            }
            const u64 text = Cell(guest, p.name);
            if (text == 0) {
                continue;
            }
            std::array<std::uint8_t, 0x30> request{};
            const float rate = p.rate;
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
            last = {p.name, now_ms};
            ++played_;
            LOG_INFO(Core, "coop: objects: copy {:#x} plays '{}'", p.address, p.name);
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
        return std::nullopt;
    }

private:
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
    std::vector<PlayWish> work_plays_;
    std::size_t next_play_ = 0;
    std::size_t played_ = 0;
    std::unordered_map<u64, std::pair<std::string, u64>> played_at_;  // actor: name, ms
    std::unordered_map<std::string, u64> cells_;
    u64 block_ = 0;
    bool allocating_ = false;
    bool alloc_tried_ = false;
    std::vector<Pose> work_;
    std::vector<u64> work_removes_;
    std::unordered_set<u64> asked_;  // deletes asked once
    std::size_t next_ = 0;
    std::size_t next_remove_ = 0;
    std::size_t moved_ = 0;
    std::size_t deleted_ = 0;
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
                return true;
            }();
            (void)registered;
            sync_.SetHurt(hurt_);
            take_at_ = server_now_us + 10'000'000;
            LOG_INFO(Core, "coop: objects: share on, poses {}, hurt {}", pose_ ? "on" : "off",
                     hurt_ ? "through the game" : "written as life");
        }
        self_ = self;
        sharing_ = true;
        std::vector<coop::protocol::Message> send;
        std::vector<LifeWrite> writes;
        sync_.Tick(*scene, self, server_now_us, seen, send, writes);
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
                if (const auto* d = std::get_if<coop::protocol::Damage>(&m)) {
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
        }
        sync_.Receive(m);
    }

    bool debug_ = false;
    bool cheat_ = false;
    bool share_ = false;
    bool pose_ = true;
    bool hurt_ = true;
    bool drive_ = false;
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
            if (IsShared(known.first) ||
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
                WriteLife(memory, r, {enemy, life - 1, hurt_ ? 1 : 0});
            }
        }
    }

    // Every frame: each copy we do not own goes where the owner's enemy was.
    // How far the game moved it since our last write is logged every 5 s: if
    // its AI or physics fight the write, that shows here.
    void Place(Core::System& system, u64 now_us) {
        std::vector<PoseWrite> poses;
        sync_.Poses(now_us, poses);
        auto& memory = system.ApplicationMemory();
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
        TheMover().Want(main_, std::move(wish), sync_.Removes(), std::move(plays));
        if (now_us - drift_at_ >= kDriftLogUs) {
            const auto [moved, deleted, played] = TheMover().TakeCounts();
            if (drift_count_ > 0 || moved > 0 || deleted > 0 || played > 0 ||
                sync_.HurtsTaken() + sync_.HurtsMissed() > 0) {
                LOG_INFO(Core,
                         "coop: objects: share placed {} copies; off from where we put them "
                         "avg {:.3f} max {:.3f} over {}; game calls: {} moves, {} deletes, {} "
                         "plays; hurts taken {} missed {}",
                         placed_.size(), drift_count_ ? drift_sum_ / drift_count_ : 0.0f,
                         drift_max_, drift_count_, moved, deleted, played, sync_.HurtsTaken(),
                         sync_.HurtsMissed());
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
            if (actor.name.starts_with("Enemy_")) {
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
        // Off by default: the live test (2026-11-09) showed the game does not
        // take an amount alone from Life + 0x228 (it needs a whole hit record).
        objects.hurt_ = EnvFlag("LICHII_COOP_OBJECTS_HURT");
        objects.drive_ = EnvFlag("LICHII_COOP_OBJECTS_TEST_DRIVE");
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
