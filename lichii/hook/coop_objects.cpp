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
#include "coop_caller_orders.h"
#include "coop_hit_replay.h"
#include "coop_every.h"
#include "coop_event_result.h"
#include "coop_cheat_items.h"
#include "coop_flags.h"
#include "coop_hold.h"
#include "coop_actor_events.h"
#include "coop_boss_native.h"
#include "coop_difficulty.h"
#include "coop_puppet.h"
#include "coop_pvp.h"
#include "coop_pvp_game.h"
#include "coop_pvp_native.h"
#include "coop_pvp_native_game.h"
#include "coop_dbno_body.h"
#include "coop_dbno_game.h"
#include "coop_svc.h"
#include "coop_testinput.h"

#include <algorithm>
#include <limits>
#include <map>
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
#include "coop/host/game_items.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_objects_sync.h"
#include "coop_follow.h"
#include "coop_frame_clock.h"
#include "coop_link_sound.h"
#include "coop_ocarina.h"
#include "coop_puppet_sound.h"
#include "coop_spawn.h"
#include "coop_pickups.h"
#include "coop_projectiles.h"
#include "coop_guard.h"
#include <atomic>
#include <unordered_map>
#include "coop_world.h"
#include "coop_lift_ride.h"
#include "coop_room_objects.h"
#include "coop_switch_share.h"
#include "coop_logic_probe.h"
#include "coop_sensor_claim.h"
#include "coop_platforms.h"
#include "coop_counters.h"
#include "coop_scrub_talk.h"
#include "coop_scrub_sale.h"
#include "coop_scrubs.h"
#include "coop_event_member.h"
#include "coop_event_owner.h"
#include <set>

namespace CoopObjects {
void NoteBosses(const std::vector<Seen>& seen, const std::vector<std::pair<u64, std::string>>& owned,
                const std::string& scene);  // issue 109, below
void NoteBossDeleted(std::uint64_t address);  // below: the game's delete request (row 114)
void ForgetBosses();  // below: a new session, the title or a new game run
// Boss bodies whose intro this game has not played (SetUnreadyBosses, below).
std::mutex g_unready_mutex;
std::vector<std::string> g_unready;
std::vector<std::string> g_dying;  // SetDyingBosses (same mutex)
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
// Difficulty's extra enemies (coop_difficulty.h Extras) on the director's enemy list: their own keys there.
constexpr u64 kExtraKey = kEnemyKey | u64{1} << 62;
// Difficulty's night horde (coop_difficulty.h Horde): its own keys on the same list.
constexpr u64 kHordeKey = kEnemyKey | u64{1} << 61;
constexpr int kMoverCallerOrder = CoopCallerOrders::kMover;  // after the puppets, spawn copies (10) and fairies (20)
constexpr int kGrassCallerOrder = CoopCallerOrders::kGrass;  // the others' field grass cuts (coop_world.h)

CoopWorld::GrassSync& TheGrass() {
    static CoopWorld::GrassSync grass;
    return grass;
}

// Issue 149 (coop_scrubs.h): the Scrub Brothers' hits. The present thread
// looks and receives, the calc thread's cave gives the others' hits.
constexpr int kScrubsCallerOrder = CoopCallerOrders::kScrubs;
constexpr int kLiftRideCallerOrder = CoopCallerOrders::kLiftRide;  // jabu-lifts (#65): after the boss table (45), before the boss rooms (50)
constexpr int kSwitchShareCallerOrder = CoopCallerOrders::kSwitchShare;  // room-objects: another player's timed switch held on ours
constexpr int kPvpShieldCallerOrder = CoopCallerOrders::kPvpShield;  // phase 4: the _Pvp copies' Life invincible
constexpr int kDbnoBodyCallerOrder = CoopCallerOrders::kDbnoBody;  // down but not out: our downed Link's crawl, shield, unseen (coop_dbno_body.h)

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
// deku-visuals-b: the enter arguments of a push block's node start (0x10
// bytes, zeroed: the start reads only the float at +8).
constexpr u64 kBlockNodeArgs = 0x30;
constexpr u64 kBlockNames = 0x40;
constexpr u64 kNameCell = 0x50;
constexpr u64 kNameCells = 400;
constexpr u64 kBlockSize = kBlockNames + kNameCell * kNameCells;

// The pause counters' ring (coop_counters.h): made once with the game's
// malloc on the calc thread, its address put where the cave reads it.
constexpr int kCountersCallerOrder = CoopCallerOrders::kCounters;
// deku-switches (LICHII_COOP_LOGIC_PROBE): a room's event asker (coop_live_events.h kAskerNames).
bool IsRoomAsker(std::string_view name) { return name == "EventRequestActor" || name == "EventProxyRequestActor"; }
// deku-ladder (LICHII_COOP_LOGIC_PROBE): switch kinds that are not gimmicks (CoopWorld::IsGimmick) but start room events.
bool IsProbeSwitch(std::string_view name) {
    return name.find("HammerSwitch_") != std::string_view::npos || name.find("DiveSwitch_") != std::string_view::npos;
}
struct CounterRingBox {
    std::mutex mutex;
    u64 main = 0;    // set by Share while sharing; 0: nothing to do
    u64 ring = 0;
    bool asked = false;
    bool allocating = false;
};
CounterRingBox& TheCounterRing() {
    static CounterRingBox box;
    return box;
}

std::optional<CoopPuppet::Call> CounterRingStep(CoopPuppet::Guest& guest, bool after_call,
                                                std::uint64_t result) {
    CounterRingBox& box = TheCounterRing();
    std::lock_guard lock(box.mutex);
    if (after_call && box.allocating) {
        box.allocating = false;
        if (result != 0 && box.main != 0) {
            const std::array<u8, CoopCounters::kRingBytes> zeros{};
            guest.Write(result, zeros.data(), zeros.size());
            guest.Write(box.main + CoopCounters::kRingPointer, &result, 8);
            box.ring = result;
            LOG_INFO(Core, "coop: platforms: pause counter ring at {:#x}", result);
        }
        return std::nullopt;
    }
    if (after_call || box.main == 0 || box.ring != 0 || box.asked) {
        return std::nullopt;
    }
#ifdef COOP_HOOKSHOT
    // Hookshot: no guest ring; the game's C hands each start to the module
    // (patch 0005's world hook, hos_coop_world -> CoopCounters::TheHostRing).
    box.asked = true;
    LOG_INFO(Core, "coop: platforms: pause counters from hookshot.exe's world hook (patch 0005), no guest ring");
    return std::nullopt;
#else
    u32 word = 0;  // only when the loader put the cave in
    guest.Read(box.main + CoopCounters::kStartOffset, &word, 4);
    if (word != CoopCounters::HookWord()) {
        box.asked = true;
        LOG_INFO(Core, "coop: platforms: no counter cave (main+{:#x} holds {:#010x}): pause counters off",
                 CoopCounters::kStartOffset, word);
        return std::nullopt;
    }
    box.asked = true;
    box.allocating = true;
    CoopPuppet::Call c;
    c.function = box.main + kMalloc;
    c.x[0] = CoopCounters::kRingBytes;
    return c;
#endif
}
constexpr u64 kPlayLogMs = 1000;  // a copy's replays of one name: one log line a second
// LICHII_COOP_OBJECTS_HIT_REPLAY=1 (experiment): a life a copy must lose is
// given to the game as a whole hit, written into the copy's Life component
// on the calc thread before its step, shaped like the one a Kokiri Sword hit
// left on a ReDead (live dump, 2026-11-09): the attack header at +0x1f8, the
// amount at +0x224/+0x228, and the incoming hit at +0x240..+0x2d8 (attacker
// actor +0x240, hit id +0x248, present +0x24c, attack kind +0x250, the hit's
// place +0x27c, the attacker's place +0x2a0). The list links from +0x2d8 on
// are left alone. Our own player stands in as the attacker.
// (kHitTemplate: coop_hit_replay.h)
constexpr u64 kHitRecord = 0x240;
constexpr u64 kHitHeader = 0x1f8;
constexpr u64 kHitAmount = 0x224;
constexpr u64 kLastHitId = 0x1b18;
  // the copy's own AI took over: play again at most this often
constexpr u64 kDriftLogUs = 5'000'000;
constexpr u64 kActorComponents = 0x270;

namespace {
// Boss turns debug (LICHII_COOP_DEBUG_BOSS): each boss's attack starting or ending in our walk, with its five action
// slots' verbs and their attribute bits; at most kMaxAttackNotes lines (plain enemies: their first 20 edges).
void NoteAttackEdge(u64 actor, const std::string& name, bool attacking, const std::array<s32, 5>& verbs, u64 bits) {
    static std::mutex m;
    static std::map<u64, bool> last;
    static std::size_t notes = 0, plain_notes = 0;
    constexpr std::size_t kMaxAttackNotes = 400;
    std::lock_guard lock(m);
    auto [it, fresh] = last.try_emplace(actor, attacking);
    if (!fresh && it->second == attacking) {
        return;
    }
    it->second = attacking;
    const bool boss = CoopObjects::IsBossOrPart(name);
    if (notes >= kMaxAttackNotes || (!boss && plain_notes >= 20)) {
        return;
    }
    ++notes;
    if (!boss) {
        ++plain_notes;
    }
    LOG_INFO(Core, "coop: bosses: turns: {} {:#x} {} (action verbs {} {} {} {} {}, attributes {:#x})", name, actor,
             fresh ? (attacking ? "first read: attacking" : "first read: between moves")
                   : (attacking ? "attack starts" : "attack ends"),
             verbs[0], verbs[1], verbs[2], verbs[3], verbs[4], bits);
}
}  // namespace
// LICHII_COOP_OBJECTS_AI_HOLD=1 or 2 (off by default; docs/objects.md
// "Holding a copy's AI", coop_hold.h): the BSA component is table +0xb0
// (0xcad060), and bit 0 of its u16 at +0xc4 is what QueryBsaIsBrainSkip (run
// 0x29ce250) reads. Each game frame the Mover writes the held copies' BSA
// components into the table the hold cave reads, and counts in how many
// frames each one's bit read as asked.
constexpr u64 kComponentBsa = 0xb0;
constexpr u64 kBsaFlags = 0xc4;
constexpr u64 kComponentLife = 0x2b8;
static_assert(sizeof(CoopHitReplay::Record) == sizeof(coop::protocol::HitRecord));
static_assert(kComponentLife == CoopHitReplay::kComponentLife && kActorComponents == CoopHitReplay::kActorComponents &&
              kHitRecord == CoopHitReplay::kHitRecord && kHitHeader == CoopHitReplay::kHitHeader &&
              kHitAmount == CoopHitReplay::kHitAmount && kLastHitId == CoopHitReplay::kLastHitId);
constexpr u64 kLifeValue = 0x1c10;
constexpr u64 kLifeMax = 0x1c18;
constexpr u64 kLifeCopy = 0x1ca8;  // life and max as last mirrored
constexpr u64 kActorPlacement = 0x3d8;
constexpr u64 kPlacementHash = 0x88;
constexpr std::size_t kMaxWalk = 8192;
// What an actor's cached name was read with (row 172): its name pointer and its placement pointer. A new
// actor at a reused address differs in at least one, and its name is read again.
struct ActorStamp {
    u64 name = 0;
    u64 head = 0;  // the name's first 8 characters (a name kept inside the actor has the same pointer)
    u64 placement = 0;
    bool operator==(const ActorStamp&) const = default;
};

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
std::vector<std::pair<std::uint32_t, bool>>& KeyLendQueue() {  // lends (false) and take-backs (true)
    static std::vector<std::pair<std::uint32_t, bool>> q;
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
struct TestStrike {
    std::string name;
    std::int32_t amount = 0;  // 0: a kill (its whole life); -1: a stun (TestStun)
};
struct TestPlatformPlaces {  // TestPlatformPlace: the last walk's moving platforms nearest our Link, by name
    std::mutex mutex;
    std::map<std::string, std::array<float, 3>> at;
};
TestPlatformPlaces& ThePlatformPlaces() {
    static TestPlatformPlaces p;
    return p;
}
std::vector<TestStrike>& TestKillQueue() {  // the test script's kill, hit and stun steps (QueueTestKill)
    static std::vector<TestStrike> q;
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

// Unset or anything but "0": on (a switch that is on by default). An opt-in switch (off unless set) reads EnvFlag
// above. g4-enemies: this was named EnvOn, the name the other files (coop_drops, coop_svc, coop_fairy, the
// Hookshot module) give the opt-in reading, so LICHII_COOP_ENEMY_BRAIN and LICHII_COOP_DEBUG_NPC, both opt-in, read
// as on for everyone.
bool EnvOnUnlessZero(const char* name) {
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

// Native boss sync: main's base for the boss caller (set by the walk).
std::atomic<std::uint64_t>& TheBossMain() {
    static std::atomic<std::uint64_t> main{0};
    return main;
}

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
        // jitter-b (coop_follow.h): a townsperson's velocity, metres a second: followed
        // each game frame from when it was wished. Unset: written where it is.
        std::optional<std::array<float, 3>> velocity;
    };

    using Hit = CoopHitReplay::Hit;

    // Test only (`aim X Y Z`, row 173): our next arrow is put on its way into `target` the first frame it flies.
    void ArmAim(const std::array<float, 3>& target, u64 until_ms) {
        std::lock_guard lock(mutex_);
        aim_ = Aim{target, until_ms};
    }
    // The walk: a NormalArrow first seen (ours, nocked: another player's is a CoopShot_Arrow stand-in), taken
    // while an aim waits for one.
    void AimArrow(u64 actor, const std::array<float, 3>& at) {
        std::lock_guard lock(mutex_);
        if (aim_ && aim_->arrow == 0) {
            aim_->arrow = actor;
            aim_->last = at;
            LOG_INFO(Core, "coop: test aim: our arrow {:#x} nocked at {:.2f} {:.2f} {:.2f}", actor, at[0], at[1], at[2]);
        }
    }

    // Any thread: the hit id we last wrote into this Life component (the walk never takes our own replay for
    // a hit of our game's).
    u32 ReplayedId(u64 life) {
        return replayer_.ReplayedId(life);
    }
    // A new scene: its Life components may sit where the old ones were (ReplaySeen would wait on an old id).
    void ForgetReplayed() {
        replayer_.Forget();  // (the ids' own lock, never the Mover's: boss-intro d58e663e)
    }

    // Present thread: hits to replay at the next game frame.
    void AddHit(const Hit& hit) {
        replayer_.Add(hit, SteadyMs());
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
    // `no_replay`: bosses on the brain path (CoopBossNative::Table::RunsOwnMoveWhenOurs, row 178): their own AI
    // goes on through the Table, no animation is played over their move.
    void Release(const std::vector<u64>& actors, const std::vector<u64>& no_replay = {}) {
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
                const bool replay = std::find(no_replay.begin(), no_replay.end(), a) == no_replay.end();
                if (!replay) {
                    LOG_INFO(Core, "coop: objects: {:#x} became ours: a boss on the brain path, its own AI goes "
                                   "on (no '{}' replay, row 178)", a, w->second.first);
                }
                if (replay && SteadyMs() - w->second.second <= kWakeReplayMs && wakes_.size() < 64) {
                    wakes_.emplace_back(a, w->second.first);
                }
                wanted_.erase(w);
            }
        }
    }

    // Present thread (report 109): a copy's own wake play that a put-back
    // took away is played again once at the next game frame. The owner's
    // play wished for it before is dropped, and the wake is no hand-over's
    // to replay later (wanted_).
    void GiveBack(u64 actor, std::string name) {
        std::lock_guard lock(mutex_);
        LOG_INFO(Core, "coop: objects: copy {:#x}: its own wake '{}' is played again (report 109)", actor, name);
        if (wakes_.size() < 64) {
            wakes_.emplace_back(actor, std::move(name));
        }
        std::erase_if(plays_, [actor](const PlayWish& p) { return p.address == actor && p.slot == 0; });
        wanted_.erase(actor);
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
        // jitter-b: the time the wishes are for (Share was given that game frame's time).
        want_ms_ = CoopFrameClock::PresentAgeUs() != 0 ? CoopFrameClock::PresentAtMs() : SteadyMs();
    }

    // deku-visuals-b (issue 147): run the game's own push-node start on a
    // block another game moved (CoopWorld::BlockNode::kStart_fn), on the
    // next game frame, after that frame's pose writes. `node` is checked by
    // its vtable again then.
    void StartBlockNode(u64 node, u64 actor, const coop::protocol::Vec3& from,
                        const coop::protocol::Vec3& to, bool untouched) {
        std::lock_guard lock(mutex_);
        if (node_starts_.size() < 16) {
            node_starts_.push_back({node, actor, from, to, untouched});
        }
    }

    // Node starts made and skipped since the last ask (for the 5 s log line).
    std::pair<std::size_t, std::size_t> TakeNodeStarts() {
        std::lock_guard lock(mutex_);
        return {std::exchange(node_started_, 0), std::exchange(node_skipped_, 0)};
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

    // Issue 239: where the Follower last drew each followed copy (what was written, not the place wished),
    // so the walk's own-AI pull (NoteDrift, issue 101) is measured from what the game was given.
    std::unordered_map<u64, coop::protocol::Vec3> Drawn() {
        std::lock_guard lock(mutex_);
        return drawn_;
    }

    // Issue 239 probe, since the last ask: followed copies' steps against their step a written frame before
    // (avg, max metres, count). Even movement: ~0; a turn, a start or a stop: a few cm.
    std::tuple<float, float, std::size_t> TakeUneven() {
        std::lock_guard lock(mutex_);
        const auto out = std::make_tuple(uneven_count_ ? uneven_sum_ / uneven_count_ : 0.0f, uneven_max_,
                                         uneven_count_);
        uneven_sum_ = 0.0f;
        uneven_max_ = 0.0f;
        uneven_count_ = 0;
        return out;
    }

    // Townspeople, since the last ask: plays put under the copy's own AI's
    // name, names given back, frames set.
    std::array<std::size_t, 3> TakeNpc() {
        std::lock_guard lock(mutex_);
        return {std::exchange(aliased_, 0), std::exchange(unaliased_, 0),
                std::exchange(npc_framed_, 0)};
    }

    // The owner's animation a copy plays while its slot names its AI's one
    // (empty: not aliased, the slot's name is what plays).
    std::string AliasedPlay(std::uint64_t actor) {
        std::lock_guard lock(mutex_);
        const auto* entry = aliases_.Find(actor);
        return entry ? entry->played : std::string();
    }

    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call,
                                         std::uint64_t result, std::uint64_t now_ms) {
        std::lock_guard lock(mutex_);
        if (after_call && allocating_) {
            allocating_ = false;
            block_ = result;
            return std::nullopt;  // next frame
        }
        if (after_call && alias_after_) {
            // The play just made, on a townsperson: its slot names what its
            // own AI asked for again, so that AI's request of every frame
            // finds it there and leaves our animation playing.
            const auto [actor, name_at, name, played] = *std::exchange(alias_after_, std::nullopt);
            const u64 controller = Controller(guest, actor);
            const u64 slot = controller != 0 ? Slot0(guest, controller) : 0;
            if (slot != 0 && guest.Write(slot + kAsSlotName, &name_at, 8)) {
                aliases_.Played(actor, played, name_at, name);
                ++aliased_;
            }
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
            alias_after_.reset();  // its play's frame ended before we were asked again
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
            if (block_ == 0 && main_ != 0 && !allocating_ && (!plays_.empty() || !node_starts_.empty()) &&
                !alloc_tried_) {
                allocating_ = true;
                alloc_tried_ = true;
                CoopPuppet::Call c;
                c.function = main_ + kMalloc;
                c.x[0] = kBlockSize;
                return c;
            }
            work_plays_ = plays_;
            next_play_ = 0;
            if (block_ != 0) {
                work_node_starts_ = std::exchange(node_starts_, {});
            }
            next_node_start_ = 0;
            Unalias(guest);
            work_wakes_ = std::exchange(wakes_, {});
            next_wake_ = 0;
            // The queued hits (coop_hit_replay.h: its own locks, never this mutex_ again).
            if (main_ != 0) {
                for (const auto& w : replayer_.Run(guest, now_ms, [&](u64 a) { return Alive(guest, a); })) {
                    // Difficulty: the player who dealt it had it scaled; the Life step takes it as it is.
                    CoopDifficulty::TheKinds().Pass(w.life, w.amount, CoopDifficulty::SteadyMs());
                    ++replayed_;
                    LOG_INFO(Core, "coop: objects: copy {:#x} replays a hit of {} (id {:#x}{}{})", w.actor, w.amount,
                             w.id, w.exact ? ", where it landed in the hitter's game" : "",
                             w.real ? ", the hit the other game took" : ", the stand-in sword hit");
                }
            }
            if (holding_) {
                WriteHolds(guest);
            }
            work_ = poses_;
            std::erase_if(follow_, [this](const auto& f) {  // jitter-b: copies no longer followed
                return std::none_of(work_.begin(), work_.end(),
                                    [&f](const Pose& p) { return p.actor == f.first && p.velocity; });
            });
            std::erase_if(drawn_, [this](const auto& d) { return !follow_.contains(d.first); });
            std::erase_if(steps_, [this](const auto& d) { return !follow_.contains(d.first); });
            if (resync_.Size() > 256) {  // jitter-b: copies that left
                std::unordered_set<u64> playing;
                for (const PlayWish& w : plays_) {
                    playing.insert(w.slot == 0 ? w.address : w.address ^ (static_cast<u64>(w.slot) << 56));
                }
                resync_.Keep(playing);
            }
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
        if (!after_call && aim_) {
            if (auto c = AimStep(guest)) {
                return c;
            }
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
            const std::int64_t since_us = static_cast<std::int64_t>(since) * 1000;
            // A townsperson's (coop_npc_anim.h): the playing frame, inside its loop.
            const float frame =
                p.play_frame ? CoopNpcAnim::Wrapped(
                            FrameAfter(p.frame, p.run < 0.0f ? rate : p.run, since_us), p.loop)
                      : FrameAfter(p.frame, rate, since_us);
            const std::string slot_name = SlotName(guest, controller, p.slot);
            const u64 slot_name_at = p.alias ? SlotNameAt(guest, controller) : 0;
            if (p.alias ? aliases_.Shows(p.address, p.name, slot_name_at, slot_name)
                        : slot_name == p.name) {
                auto& checked = checked_sample_[key];
                if (checked == p.sample) {
                    continue;
                }
                checked = p.sample;
                float now_frame = 0.0f;
                if (p.play_frame) {
                    const float gap = PlayFrame(guest, controller, now_frame)
                                          ? CoopNpcAnim::FrameGap(now_frame, frame, p.loop)
                                          : std::numeric_limits<float>::infinity();
                    // jitter-b: at most once a second, and not again while sets do not take.
                    const auto verdict = resync_.Check(key, p.name, gap, now_ms);
                    if (verdict == CoopNpcAnim::Resync::Verdict::give_up) {
                        LOG_INFO(Core, "coop: npc-anim: copy {:#x} '{}': its frame sets do not take "
                                       "({:.1f} frames off): left to its own playback",
                                 p.address, p.name, gap);
                    }
                    if (verdict != CoopNpcAnim::Resync::Verdict::set) {
                        continue;
                    }
                    ++npc_framed_;
                } else if (SlotFrame(guest, controller, now_frame, p.slot) &&
                           std::fabs(now_frame - frame) <= kFrameSlack) {
                    continue;
                }
                ++framed_;
                return SetFrame(controller, frame, p.slot);
            }
            // Issue 195: a chest shown open is played again only over its
            // closed idle, never over its own opening (our Link opens it).
            if (!slot_name.empty() && !PlaysOver(slot_name, p.only_over)) {
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
            if (p.alias && slot_name_at != 0 && !slot_name.empty()) {
                alias_after_ = std::make_tuple(p.address, slot_name_at, slot_name, p.name);
            }
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
            Pose followed = p;
            if (p.velocity) {
                const CoopFollow::Vec3 at{p.transform[0], p.transform[1], p.transform[2]};
                const auto drawn = follow_[p.actor].Step(at, *p.velocity, want_ms_, cadence_.At(now_ms));  // jitter-2: even
                std::copy(drawn.begin(), drawn.end(), followed.transform.begin());
                // Issue 239 probe: how uneven the drawn steps are (this step against the last one, metres).
                if (const auto was = drawn_.find(p.actor); was != drawn_.end()) {
                    coop::protocol::Vec3 step{};
                    for (std::size_t i = 0; i < 3; ++i) {
                        step[i] = drawn[i] - was->second[i];
                    }
                    if (const auto last = steps_.find(p.actor); last != steps_.end()) {
                        float u2 = 0.0f;
                        for (std::size_t i = 0; i < 3; ++i) {
                            const float u = step[i] - last->second[i];
                            u2 += u * u;
                        }
                        const float u = std::sqrt(u2);
                        if (std::isfinite(u)) {
                            uneven_sum_ += u;
                            uneven_max_ = std::max(uneven_max_, u);
                            ++uneven_count_;
                        }
                    }
                    steps_[p.actor] = step;
                }
                drawn_[p.actor] = {drawn[0], drawn[1], drawn[2]};
            }
            if (p.rotation) {
                guest.Write(p.actor + kActorPosition, followed.transform.data(), 48);
            } else {
                guest.Write(p.actor + kActorPosition, followed.transform.data(), 12);
            }
            ++moved_;
            CoopPuppet::Call c;
            c.function = main_ + kSetMatrix;
            c.x[0] = p.actor;
            c.x[1] = 0;
            return c;
        }
        // deku-visuals-b (issue 147): a block another game moved runs its
        // own push node's start again, after this frame's pose writes (its
        // base anchor is at the new rest by then). The start sets the push's
        // start and target from the block's place, as when the node is
        // entered, so this player's next push starts where the block is.
        while (next_node_start_ < work_node_starts_.size()) {
            const NodeStart& n = work_node_starts_[next_node_start_++];
            const u64 node = n.node;
            u64 table = 0;
            if (block_ == 0 || !Alive(guest, n.actor) || !guest.Read(node, &table, 8) ||
                !CoopWorld::BlockNode::IsNodeVtable(table, main_)) {
                ++node_skipped_;
                continue;
            }
            // Still idle at the old rest, read again now (review): our
            // player may have grabbed the block since the walk read it.
            const auto stale = CoopWorld::NodeRestWords(
                [&guest](u64 address) -> std::optional<u64> {
                    u64 word = 0;
                    return guest.Read(address, &word, 8) ? std::optional<u64>(word) : std::nullopt;
                },
                node, n.from, n.to, n.untouched);
            if (stale.busy || stale.writes.empty()) {
                ++node_skipped_;
                continue;
            }
            const std::array<std::uint8_t, 0x10> args{};
            if (!guest.Write(block_ + kBlockNodeArgs, args.data(), args.size())) {
                continue;
            }
            ++node_started_;
            CoopPuppet::Call c;
            c.function = main_ + CoopWorld::BlockNode::kStartFn;
            c.x[0] = node;
            c.x[1] = block_ + kBlockNodeArgs;
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
#ifdef COOP_HOOKSHOT
        // Hookshot: the hold hook asks the module (CoopHold::TheHostTable),
        // no table in the game's memory.
        CoopHold::TheHostTable().Publish(components, masks_);
#else
        const auto bytes = CoopHold::Table(components, masks_);
        guest.Write(main_ + CoopHold::kTableOffset, bytes.data(), bytes.size());
#endif
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

    // The pointer slot 0 keeps to its command name, or 0.
    u64 SlotNameAt(CoopPuppet::Guest& guest, u64 controller) {
        const u64 slot = Slot0(guest, controller);
        u64 at = 0;
        return slot != 0 && guest.Read(slot + kAsSlotName, &at, 8) ? at : 0;
    }

    // Slot 0's playing frame (coop_npc_anim.h).
    bool PlayFrame(CoopPuppet::Guest& guest, u64 controller, float& frame) {
        const u64 slot = Slot0(guest, controller);
        return slot != 0 && guest.Read(slot + CoopNpcAnim::kSlotPlayFrame, &frame, 4) &&
               std::isfinite(frame);
    }

    // Start of a game frame: the townspeople that are no longer wished to
    // play an owner's animation under their own AI's name (ours now, the
    // owner silent, our Link in an event beside them) get the name of what
    // really plays back, so their own AI's next request plays its own.
    void Unalias(CoopPuppet::Guest& guest) {
        if (aliases_.Size() == 0) {
            return;
        }
        std::unordered_set<u64> wished;
        for (const PlayWish& p : work_plays_) {
            if (p.alias) {
                wished.insert(p.address);
            }
        }
        for (const auto& [actor, entry] : aliases_.DropAllBut(wished)) {
            const u64 controller = Alive(guest, actor) ? Controller(guest, actor) : 0;
            if (controller == 0 || SlotNameAt(guest, controller) != entry.name_at ||
                SlotName(guest, controller) != entry.name) {
                continue;  // gone, or its AI chose another animation since
            }
            const u64 slot = Slot0(guest, controller);
            if (const u64 text = Cell(guest, entry.played); slot != 0 && text != 0) {
                guest.Write(slot + kAsSlotName, &text, 8);
                ++unaliased_;
            }
        }
    }

    bool SlotFrame(CoopPuppet::Guest& guest, u64 controller, float& frame, std::int32_t index = 0) {
        const u64 slot = Slot0(guest, controller, index);
        return slot != 0 && guest.Read(slot + kAsSlotFrame, &frame, 4) && std::isfinite(frame);
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
    // `aim` (row 173), each frame under mutex_: the nocked arrow is watched until it moves more than
    // CoopTestInput::kAimLetGoM in a frame (let go: ~113 m/s), then put once on its line into the target, pointed
    // at it, so its own flight takes it in; the place's set-matrix call is returned.
    std::optional<CoopPuppet::Call> AimStep(CoopPuppet::Guest& guest) {
        Aim& a = *aim_;
        const auto target = a.target;
        if (SteadyNowMs() > a.until_ms) {
            LOG_INFO(Core, "coop: test aim {:.2f} {:.2f} {:.2f}: not put ({})", target[0], target[1], target[2],
                     a.arrow ? "the arrow was never let go" : "no arrow of ours nocked");
            aim_.reset();
            return std::nullopt;
        }
        if (a.arrow == 0) {
            return std::nullopt;
        }
        std::array<float, 3> at{};
        if (!Alive(guest, a.arrow) || !guest.Read(a.arrow + kActorPosition, at.data(), sizeof(at))) {
            LOG_INFO(Core, "coop: test aim {:.2f} {:.2f} {:.2f}: not put (arrow {:#x} gone first)", target[0],
                     target[1], target[2], a.arrow);
            aim_.reset();
            return std::nullopt;
        }
        const float dx = at[0] - a.last[0];
        const float dy = at[1] - a.last[1];
        const float dz = at[2] - a.last[2];
        const float moved = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (!(moved > CoopTestInput::kAimLetGoM)) {
            a.last = at;
            return std::nullopt;
        }
        const u64 arrow = a.arrow;
        aim_.reset();
        const auto put = CoopTestInput::AimPlace(at, target, kAimShortM);
        if (!put || !guest.Write(arrow + kActorPosition, put->data(), sizeof(*put))) {
            LOG_INFO(Core, "coop: test aim {:.2f} {:.2f} {:.2f}: not put (arrow {:#x} at {:.2f} {:.2f} {:.2f})",
                     target[0], target[1], target[2], arrow, at[0], at[1], at[2]);
            return std::nullopt;
        }
        LOG_INFO(Core,
                 "coop: test aim {:.2f} {:.2f} {:.2f}: arrow {:#x} let go ({:.2f} m in a frame, at {:.2f} {:.2f} "
                 "{:.2f}), put at {:.2f} {:.2f} {:.2f}",
                 target[0], target[1], target[2], arrow, moved, at[0], at[1], at[2], (*put)[0], (*put)[1], (*put)[2]);
        CoopPuppet::Call c;
        c.function = main_ + kSetMatrix;
        c.x[0] = arrow;
        c.x[1] = 0;
        return c;
    }

    bool Alive(CoopPuppet::Guest& guest, u64 a, bool asleep = false) {
        u32 state = 0;
        u64 owner = 0;
        u64 vtable = 0;
        return a != 0 && guest.Read(a + kActorState, &state, 4) &&
               (state == kStateAlive || (asleep && state == kStateAsleep)) &&
               guest.Read(a + kActorNode + kNodeOwner, &owner, 8) && owner == a &&
               guest.Read(a, &vtable, 8) && vtable == main_ + kActorVtable;
    }

    struct Aim {
        std::array<float, 3> target{};
        u64 until_ms = 0;
        u64 arrow = 0;               // our nocked arrow, from the walk (0: none yet)
        std::array<float, 3> last{};  // its place the frame before
    };
    static constexpr float kAimShortM = 0.6f;  // the arrow is put this far short of the target
    std::optional<Aim> aim_;                    // test only (`aim`)
    std::mutex mutex_;
    CoopHitReplay::Replayer replayer_;  // the hit replay: its own locks (Step runs it under mutex_)
    u64 main_ = 0;
    std::vector<Pose> poses_;
    std::unordered_map<u64, CoopFollow::Follower> follow_;  // jitter-b: by actor
    std::unordered_map<u64, coop::protocol::Vec3> drawn_;    // issue 239: the Follower's last place, by actor
    std::unordered_map<u64, coop::protocol::Vec3> steps_;    // issue 239: its last step
    float uneven_sum_ = 0.0f;
    float uneven_max_ = 0.0f;
    std::size_t uneven_count_ = 0;
    CoopFollow::Cadence cadence_;  // jitter-2: the game frames' time made even
    CoopNpcAnim::Resync resync_;  // jitter-b: townspeople's frame sets
    std::vector<u64> removes_;
    std::vector<PlayWish> plays_;
    std::size_t replayed_ = 0;
    std::vector<PlayWish> work_plays_;
    std::size_t next_play_ = 0;
    std::size_t played_ = 0;
    std::size_t framed_ = 0;  // frames set on a copy already showing the name
    u64 want_ms_ = 0;
    std::optional<std::tuple<u64, std::int32_t, float>> frame_after_;  // actor, slot, frame: set after the play
    // Walking townspeople (coop_npc_anim.h): actor, the AI's name pointer
    // and name, and what we played: aliased after the play.
    std::optional<std::tuple<u64, u64, std::string, std::string>> alias_after_;
    CoopNpcAnim::Aliases aliases_;
    std::size_t aliased_ = 0;
    std::size_t unaliased_ = 0;
    std::size_t npc_framed_ = 0;
    std::unordered_map<u64, std::uint64_t> checked_sample_;  // actor: sample time checked
    std::unordered_map<u64, std::pair<std::string, u64>> played_at_;  // actor: name, ms
    std::vector<std::pair<u64, std::string>> took_over_;  // copies whose own AI changed slot 0, and to what
    std::unordered_map<std::string, u64> cells_;
    u64 block_ = 0;
    bool allocating_ = false;
    bool alloc_tried_ = false;
    struct NodeStart {  // deku-visuals-b: a push node to start again
        u64 node = 0;
        u64 actor = 0;  // its block
        coop::protocol::Vec3 from{};  // the old rest both its places must still hold
        coop::protocol::Vec3 to{};
        bool untouched = false;  // row 166: our player never moved it in this follow (CoopWorld::NodeRestWords)
    };
    std::vector<NodeStart> node_starts_;
    std::vector<NodeStart> work_node_starts_;
    std::size_t next_node_start_ = 0;
    std::size_t node_started_ = 0;
    std::size_t node_skipped_ = 0;
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

// Whether an event runs in our game, from the hook's reading each frame
// (SetLocalEvent; issue 144).
std::atomic<bool>& TheLocalEvent() {
    static std::atomic<bool> in_event{false};
    return in_event;
}

// G1: whether our own Link is in that event (not another player's replayed here,
// not the free Ocarina; coop/host/event_owner.hpp).
std::atomic<bool>& TheOwnEvent() {
    static std::atomic<bool> ours{false};
    return ours;
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

// SetMaxLifeLowerTo; -1: no hold.
std::atomic<s32>& TheMaxLifeLowerTo() {
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
// Rows 93/103 (OurLinkActor): our Link's actor as the last walk saw it, and when (steady ms; 0: never).
std::atomic<u64>& TheLinkActor() {
    static std::atomic<u64> actor{0};
    return actor;
}
std::atomic<u64>& TheLinkActorAt() {
    static std::atomic<u64> ms{0};
    return ms;
}
// Native puppets phase 4 (docs/native-puppets.md 5.1; review M1 of glue part 2): each live _Pvp copy's Life is
// made invincible once, through the game's own call (CoopPvpNative::kInvincibleReacting with our key), so no hit
// in any game (our own, an enemy's, a third game's replay) ever takes the copy's hearts. The calc thread, at the
// svc cave, after the down-crawl's caller; one call a frame.
class PvpShieldCaller {
public:
    std::optional<CoopPuppet::Call> Next(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main_base) {
        if (after_call) {
            const Pending was = std::exchange(pending_, Pending::none);
            if (was == Pending::malloc) {
                block_ = result;
            } else if (was == Pending::shield) {
                shield_.Called(asked_, true);
                LOG_INFO(Core, "coop: pvp native: P{}'s _Pvp copy {:#x}: the game's invincibility on (Life {:#x})",
                         asked_number_, asked_.first, asked_.second);
                return std::nullopt;  // one a frame
            }
        }
        if (main_base == 0 || pending_ != Pending::none || !CoopPvpNativeGame::On()) {
            return std::nullopt;
        }
        std::vector<CoopPvpNative::CopyShield::Copy> live;
        std::vector<std::uint8_t> numbers;
        for (std::uint8_t n = 1; n <= coop::protocol::kMaxPlayers; ++n) {
            const u64 actor = CoopPuppet::TheDirector().LivePvpActor(n);
            u64 table = 0;
            u64 life = 0;
            if (actor == 0 || !guest.Read(actor + kActorComponents, &table, 8) || table == 0 ||
                !guest.Read(table + kComponentLife, &life, 8) || life == 0) {
                continue;
            }
            live.emplace_back(actor, life);
            numbers.push_back(n);
        }
        const auto next = shield_.Next(live);
        if (!next) {
            return std::nullopt;
        }
        CoopPuppet::Call c;
        if (block_ == 0) {
            if (after_call && result == 0) {
                return std::nullopt;  // the game had no memory: next frame
            }
            c.function = main_base + CoopBossNative::addr::kMalloc;
            c.x[0] = 16;
            pending_ = Pending::malloc;
            return c;
        }
        const std::uint32_t key = CoopPvpNative::kCopyShieldKey;
        if (!guest.Write(block_, &key, sizeof key)) {
            shield_.Called(*next, false);
            if (shield_.GivenUp(*next)) {
                LOG_INFO(Core, "coop: pvp native: _Pvp copy {:#x}: could not make it invincible: given up", next->first);
            }
            return std::nullopt;
        }
        asked_ = *next;
        asked_number_ = numbers[static_cast<std::size_t>(std::find(live.begin(), live.end(), *next) - live.begin())];
        c.function = main_base + CoopPvpNative::kInvincibleReacting;
        c.x[0] = next->second;  // the Life component
        c.x[1] = 1;             // add our key
        c.x[2] = block_;        // -> the key
        pending_ = Pending::shield;
        return c;
    }

private:
    enum class Pending : std::uint8_t { none, malloc, shield };
    Pending pending_ = Pending::none;
    u64 block_ = 0;
    CoopPvpNative::CopyShield shield_;
    CoopPvpNative::CopyShield::Copy asked_{};
    std::uint8_t asked_number_ = 0;
};
PvpShieldCaller& ThePvpShield() {
    static PvpShieldCaller caller;
    return caller;
}

struct PvpAsk {
    s32 take = 0;
    std::array<float, 3> at{};
    std::array<float, 3> from{};
    // Native puppets phase 4, path B (HurtOurLinkNative): the strike's kind goes into the game's hit, and the
    // life is never written when the game does not take it (docs/native-puppets.md 5.5).
    bool native = false;
    std::optional<CoopHitReplay::Record> record;
};
std::mutex& PvpMutex() {
    static std::mutex mutex;
    return mutex;
}
// Native puppets phase 3 (PlaceOurLink): where another player's Link holds ours, and when it was asked.
struct OurLinkPut {
    std::array<float, 3> at{};
    u64 asked_ms = 0;
};
std::mutex& OurLinkPutMutex() {
    static std::mutex m;
    return m;
}
OurLinkPut& TheOurLinkPut() {
    static OurLinkPut put;
    return put;
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
                if (scene && !scene->empty() && Ready(system)) {
                    // Row 127: the session ended with our game still in its scene (we left, the server dropped:
                    // the host's game crashed). Our boss copies are no copies any more (Publish({}) below); one
                    // held at life 1 for its owner takes the owner's last life first, through its own Life step,
                    // so the next hit does not kill it (Sync::KeepBossLives; no new network data).
                    std::vector<LifeWrite> keep;
                    sync_.KeepBossLives(self_, keep);
                    if (!keep.empty()) {
                        Reader r(system.ApplicationMemory());
                        for (const LifeWrite& w : keep) {
                            WriteLife(system.ApplicationMemory(), r, w);
                        }
                    }
                    for (const std::string& note : sync_.TakeNotes()) {
                        LOG_INFO(Core, "coop: objects: {}", note);
                    }
                }
                sync_.Reset();
                world_.Reset();
                ForgetGold();  // issue 89: the next save's keys are read afresh
                block_retry_.clear();
                ForgetBosses();  // (not connected, or no scene: the title)
                // No owner or copy any more: our game runs every boss of its own again (a copy held at life 1
                // for its owner could not die after a leave or a drop).
                if (CoopBossNative::Table& bosses = CoopBossNative::TheTable(); bosses.BrainOn()) {
                    bosses.Publish({});
                }
                sharing_ = false;
            }
            return;
        }
        if (!Ready(system)) {
            return;
        }
#ifdef COOP_HOOKSHOT
        if (*scene != life_scene_) {
            // Another scene's actors: their Life components' slots are free
            // again (a slot is a component's for good otherwise, and 256 of
            // them fill up over a long session). Nothing given there is owed.
            life_scene_ = *scene;
            CoopActorEvents::TheLifeTable().Clear();
            CoopActorEvents::TheKeptDeaths().Clear();  // (deaths kept past their revive: the scene before's)
            CoopDifficulty::TheKinds().Clear();  // difficulty: the kinds of the scene before
            life_given_.clear();
            step_hits_.clear();
            boss_hit_ids_.clear();  // (actors of the scene before; an address reused here starts fresh)
            hit_lives_.clear();
            pvp_life_ = -1;  // (our Link of the scene before)
            pvp_watch_.Reset();
            TheMover().ForgetReplayed();  // its Life components may sit where the old ones were
        }
        ActorEvents(server_now_us);
#endif
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
        // Issue 200, row 97: our event's actors become ours (Sync::SetEvent).
        // G1: another player's event here: its bound actors act for it, none is claimed.
        sync_.SetEvent(TheLocalEvent().load(), server_now_us, TheOwnEvent().load());
        NoteBosses(seen, sync_.Owned(), *scene);
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
                CoopSvc::AddCaller(kCountersCallerOrder,
                                   [](CoopPuppet::Guest& guest, bool after_call,
                                      std::uint64_t result, std::uint64_t) {
                                       return CounterRingStep(guest, after_call, result);
                                   });
                // Native boss sync: the owner's boss verbs put into our copies.
                CoopSvc::AddCaller(CoopBossNative::Table::kCallerOrder,
                                   [](CoopPuppet::Guest& guest, bool after_call,
                                      std::uint64_t result, std::uint64_t) {
                                       return CoopBossNative::TheTable().Step(guest, after_call, result,
                                                                              TheBossMain().load());
                                   });
                CoopSvc::AddCaller(kGrassCallerOrder,
                                   [](CoopPuppet::Guest& guest, bool after_call,
                                      std::uint64_t result, std::uint64_t now_ms) {
                                       return TheGrass().Step(guest, after_call, result, now_ms);
                                   });
                // jabu-lifts (#65): another player on our lift: its RideOnPlayer through the game's set-bool.
                CoopSvc::AddCaller(kLiftRideCallerOrder,
                                   [](CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                      std::uint64_t now_ms) {
                                       return CoopLiftRide::TheRider().Step(guest, after_call, result, now_ms);
                                   });
                // room-objects: another player's timed switch is on: ours held on through the game's set-bool.
                CoopSvc::AddCaller(kSwitchShareCallerOrder,
                                   [](CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                      std::uint64_t now_ms) {
                                       return CoopSwitchShare::TheSetter().Step(guest, after_call, result, now_ms);
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
#ifdef COOP_HOOKSHOT
        StepHits();
#endif
        share_now_us_ = server_now_us;
        {
            std::lock_guard lock(g_unready_mutex);
            sync_.SetUnready(g_unready);
            sync_.SetDying(g_dying);
        }
        sync_.Tick(*scene, self, server_now_us, seen, send, writes);
        // A Gold Skulltula whose key this save already holds is not made again
        // (issue 89, 2026-11-16). The game reads a placement's key once, when
        // the scene's actors are made, and never looks again, so a key that
        // came in from another game after the scene loaded does not take the
        // actor away (docs/items.md, "the loaded Skulltula stays"): the
        // Skulltula stayed on the wall, and in a game that never saw the other
        // player it came back after every reload. The keys are fed to the same
        // "taken elsewhere" path a pickup takes, so an actor the game made for
        // an already-true key is deleted as the next walk sees it, whatever
        // wrote the key. Once a scene, after the scene's first Tick (that Tick
        // resets the sync, which would drop takes queued before it).
        if (main_ == 0) {
            // (no game: nothing to read)
        } else if (*scene != keys_scene_) {
            keys_scene_ = *scene;
            CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_);
            coop::host::GameFlags flags(memory);
            std::size_t took = 0;
            gold_taken_.clear();
            gold_baseline_scene_ = *scene;
            gold_baseline_tokens_ = flags.get_int(coop::host::kGoldTokenCount).value_or(0);
            for (const std::uint64_t key : coop::host::gold_skulltula_keys()) {
                // A placement is its key and key + 2 (docs/items.md).
                if (flags.get_key64(key).value_or(false) ||
                    flags.get_key64(key + 2).value_or(false)) {
                    sync_.Take(key);
                    sync_.Take(key + 2);
                    world_.TakenKey(key);
                    gold_taken_.insert(key);
                    ++took;
                }
            }
            if (took != 0) {
                LOG_INFO(Core, "coop: objects: {} Gold Skulltulas this save has taken are left out of {}",
                         took, *scene);
            }
        } else if (gold_baseline_scene_ == *scene && gold_sweep_.Due(CoopEvery::NowUs())) {
            // The token count rose since the last look here: this game took a
            // token (a token made and taken between two walks never crossed our
            // walk, so no break was sent for it — issue 89, 2026-11-17). Only
            // then are the 100 keys read: one binary search each.
            //
            // Twice a second, not every two frames: one Int read is cheap but a
            // walk runs 500 times a second, and this must cost nothing on a slow
            // frame (it was every two frames, and a frame-rate complaint
            // followed). A token taken is noticed within half a second.
            CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_);
            coop::host::GameFlags flags(memory);
            const std::int32_t now_tokens = flags.get_int(coop::host::kGoldTokenCount).value_or(0);
            if (now_tokens > gold_baseline_tokens_) {
                std::vector<std::uint64_t> fresh;
                for (const std::uint64_t key : coop::host::gold_skulltula_keys()) {
                    if (gold_taken_.contains(key)) {
                        continue;
                    }
                    if (flags.get_key64(key).value_or(false) ||
                        flags.get_key64(key + 2).value_or(false)) {
                        fresh.push_back(key);
                    }
                }
                for (const std::uint64_t key : fresh) {
                    gold_taken_.insert(key);
                    world_.TakenKey(key);
                    // skulltula-0118: taken here, so no copy of it stays or is made
                    // here either, and our own one leaving is its death for all.
                    sync_.Take(key);
                    char hex[24];
                    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(key));
                    LOG_INFO(Core, "coop: objects: Gold Skulltula {} is taken in this save: its token goes",
                             hex);
                }
            }
            // The last count, not the highest (review of #257): after a
            // give-back (40 -> 41 -> 40) a next real take to 41 is a rise
            // again; keeping the highest would miss it.
            gold_baseline_tokens_ = now_tokens;
        }
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
                                                              : CoopEnemyShots::Owner::theirs,
                                        s.address});
                }
            }
            CoopEnemyShots::NoteShooters(std::move(shooters), server_now_us);
        }
        if (CoopBossNative::Table& bosses = CoopBossNative::TheTable(); bosses.BrainOn()) {
            // Native boss sync (coop_boss_native.h): which bosses are
            // ours (our game runs them and sends their verbs) and which are
            // copies (they take the owner's verbs, put in by its caller).
            std::vector<CoopBossNative::Boss> list;
            // Row 149: a boss part (King Dodongo's roll centre, Bongo Bongo's hands, ...) is run by its root's
            // owner from its first frame, not by the server's owner of the part until the group link has moved
            // it (a roll centre is made anew for each roll: its first owner was whichever game listed it first).
            std::vector<std::string_view> names;
            std::vector<coop::protocol::Vec3> places;
            for (const Seen& s : seen) {
                names.push_back(s.name);
                places.push_back(s.position);
            }
            for (const Seen& s : seen) {
                if (s.bsa_component == 0) {
                    continue;
                }
                std::optional<u8> owner = sync_.OwnerOf(s.address);
                if (const auto root = BossPartRoot(s.name, s.position, names, places)) {
                    if (const auto root_owner = sync_.OwnerOf(seen[*root].address); root_owner && *root_owner != 0) {
                        owner = root_owner;
                    }
                }
                if (owner && *owner != 0) {
                    const bool owner_dead = sync_.OwnerSaysDead(s.address);
                    const bool plain = !IsBossOrPart(s.name);  // (seen with a BSA only under LICHII_COOP_ENEMY_BRAIN)
                    const std::optional<std::int32_t> life =
                        s.life ? std::optional<std::int32_t>(s.life->life) : std::nullopt;
                    CoopBossNative::Boss b{s.address, s.bsa_component,
                                           *owner == self ? CoopBossNative::Role::owner : CoopBossNative::Role::copy,
                                           s.name, *owner, s.life_component, owner_dead, plain,
                                           plain && CoopBossNative::PlainDeath(life, owner_dead),
                                           s.animation ? s.animation->name : std::string{}, life,
                                           s.position, s.attacking};
                    list.push_back(std::move(b));
                }
            }
            TheBossMain().store(main_);
            bosses.Publish(list);
            for (const auto& [address, state] : sync_.BossVerbsIn()) {
                bosses.Receive(address, state);
            }
            for (const std::string& note : bosses.TakeNotes()) {
                LOG_INFO(Core, "coop: bosses: {}", note);
            }
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
            PlanExtras(*scene, seen, enemies);
            PlanHorde(*scene, self, server_now_us, seen, enemies);
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
            // dungeon-sync-b (#75): a lock another player opened is kept here and our player opens it
            // with a lent key, the game's own way: the door set unlocks only on the lock's
            // ReceiveDoAttention pulse, so a deleted lock left the door shut. The lent key stays in our
            // game (coop::host::LentKeys), so the group's count is right. LICHII_COOP_LOCK_DELETE=1:
            // deleted, as in 0.11.0.
            static const bool delete_locks = [] {
                const char* v = std::getenv("LICHII_COOP_LOCK_DELETE");
                return v != nullptr && std::string_view(v) == "1";
            }();
            world_.SetDeleteLocks(delete_locks);
            // Row 98(2) trigger guards (Trigger::follow): another game's push of a block onto a press switch, its press
            // of a hammer switch or the box puzzle's last corner would raise our own asker's trigger here (a replay
            // that binds our Link): those world objects follow only once our game has played the event.
            world_.SetTriggerKeepers(TriggerFollowKeepers(system, *scene));
            world_.Tick(*scene, self, server_now_us, seen, player_, world);
            for (auto& m : world.send) {
                send.emplace_back(std::move(m));
            }
            writes.insert(writes.end(), world.lives.begin(), world.lives.end());
            TestKills(seen, writes, self);
            if (!world.key_lends.empty() || !world.lend_returns.empty()) {
                std::lock_guard lock(KeyRefundMutex());
                auto& lends = KeyLendQueue();
                for (const std::uint32_t key : world.key_lends) {
                    lends.emplace_back(key, false);
                }
                for (const std::uint32_t key : world.lend_returns) {
                    lends.emplace_back(key, true);
                }
            }
            if (!world.key_refunds.empty()) {
                std::lock_guard lock(KeyRefundMutex());
                auto& refunds = KeyRefundQueue();
                refunds.insert(refunds.end(), world.key_refunds.begin(), world.key_refunds.end());
            }
            sign_words_ = std::move(world.words);  // written after the probe's look
            for (std::string& count : world.give_back) {
                CoopWorld::QueueGiveBack(std::move(count));  // a small key or token both took
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
                box.puzzle.SetEvent(TheOwnEvent().load(), CoopEvery::NowUs());  // issue 144; G1: our Link's event
                box.puzzle.Look(*scene, scrubs_seen_, player_, CoopEvery::NowUs(), out);
            }
            for (auto& h : out.send) {
                send.emplace_back(std::move(h));
            }
            for (const auto& line : out.notes) {
                LOG_INFO(Core, "coop: {}", line);
            }
            // Issue 243: the solve flags at the hit, through the game's own flag store (the flag sync then
            // shares them as when the talk set them; the room graph deletes the brothers, as in solo).
            if (!out.solve.empty()) {
                CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_);
                for (const std::uint32_t hash : out.solve) {
                    const bool was = coop::host::GameFlags(memory).get_bool(hash).value_or(false);
                    const bool set = was || coop::host::GameFlags::set_bool(memory, hash, true);
                    LOG_INFO(Core, "coop: scrubs: solve flag {:#010x} {}", hash,
                             was ? "already set" : set ? "set" : "NOT set (the store refused)");
                }
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
            // deku-visuals-b: fixes whose node was not found or was busy come again.
            for (auto& again : std::exchange(block_retry_, {})) {
                block_fixes_.push_back(std::move(again));
            }
            for (auto& fix : block_fixes_) {
                if (fix.snap) {
                    continue;  // deku-visuals-b: logged by WorldSync only
                }
                // world-objects-2, deku-visuals-b: the block's push node
                // (named by the game's own start, patch 0029's node-start
                // event; checked by its vtable and owner; read only) runs its
                // own start again (the Mover's call), so its next push starts
                // where the block rests now. On unless LICHII_COOP_BLOCK_NODE=0.
                static const bool node_on = EnvOnUnlessZero("LICHII_COOP_BLOCK_NODE");
                const bool first = fix.tries == 0;
                if (!node_on) {
                    continue;
                }
                const auto node = CoopWorld::FindBlockNode(
                    [&r](u64 address) { return r.Get<u64>(address); }, fix.address, main_,
                    CoopWorld::TheNodeStarts());
                // Stale: both its start and target still hold the old rest
                // (read only). Mid-push here (one of them), or not started
                // yet: tried again on the next walks (a lookup in our own
                // table of started nodes, no heap search: report 37).
                const auto stale = node ? CoopWorld::NodeRestWords(
                                              [&r](u64 address) { return r.Get<u64>(address); },
                                              *node, fix.from, fix.to, fix.untouched)
                                        : CoopWorld::RestScan{};
                if (node && !stale.busy) {
                    if (!stale.writes.empty()) {
                        TheMover().StartBlockNode(*node, fix.address, fix.from, fix.to, fix.untouched);
                        LOG_INFO(Core, "coop: world: {:016x} {} push node {:x}: its own start "
                                       "(main+{:#x}) asked to run again, from where it rests now",
                                 fix.hash, fix.name, *node, CoopWorld::BlockNode::kStartFn);
                    } else {
                        LOG_INFO(Core, "coop: world: {:016x} {} push node {:x} already starts from "
                                       "where it rests: left alone",
                                 fix.hash, fix.name, *node);
                    }
                } else if (++fix.tries < kBlockNodeTries) {
                    if (first) {
                        LOG_INFO(Core, "coop: world: {:016x} {} push node {}: tried again for {} walks",
                                 fix.hash, fix.name, node ? "busy (mid-push here)" : "not started yet",
                                 kBlockNodeTries);
                    }
                    block_retry_.push_back(fix);
                } else {
                    LOG_INFO(Core, "coop: world: {:016x} {} push node {}: its next push may start "
                                   "from the old place",
                             fix.hash, fix.name,
                             node ? "still busy"
                                  : (CoopWorld::TheNodeStarts().Count() == 0
                                         ? "not found (no node start from the game: patch 0029 missing)"
                                         : "not found among the started nodes"));
                }
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
        // jabu-lifts (#65): our lifts another player stands on get RideOnPlayer, as our Link's contact gives it.
        if (lift_ride_on_) {
            auto& rider = CoopLiftRide::TheRider();
            rider.Want(CoopLiftRide::Ridden(ride_lifts_, sync_.OthersHere(server_now_us)), main_, SteadyNowMs());
            for (const auto& line : rider.TakeNotes()) {
                LOG_INFO(Core, "coop: {}", line);
            }
        }
        // jabu-lifts (#65, #176): the lifts' logic plugs (RideOnCharacter, ...) and the Deku ladder and pillar's, as the game's set-bool sees them.
        if (logic_probe_on_) {
            std::vector<std::pair<float, const CoopWorld::GimmickProbe::Gimmick*>> near;
            for (const auto& g : gimmicks_) {
                if (!player_ || (g.name.find("ElevatorLift") == std::string::npos && g.name.find("FallingLadder_") == std::string::npos &&
                                 g.name.find("Stopper_") == std::string::npos &&
                                 g.name.find("MoveFloor") == std::string::npos &&
                                 g.name.find("EyeSwitch_") == std::string::npos &&
                                 // deku-ladder: the other switch kinds whose plug starts a room event (step, hit, sun)
                                 g.name.find("StepSwitch_") == std::string::npos &&
                                 g.name.find("SwitchHit_") == std::string::npos &&
                                 g.name.find("SolarSwitch_") == std::string::npos && !IsProbeSwitch(g.name) &&
                                 !IsRoomAsker(g.name))) {
                    continue;
                }
                float d2 = 0.0f;
                for (std::size_t i = 0; i < 3; ++i) {
                    const float d = g.position[i] - (*player_)[i];
                    d2 += d * d;
                }
                if (d2 <= 20.0f * 20.0f) near.emplace_back(d2, &g);
            }
            std::sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            std::vector<std::pair<u64, std::string>> lifts;
            for (const auto& [d2, g] : near) lifts.emplace_back(g->address, g->name);
            for (const auto& line : logic_probe_.Look([&r](u64 address) { return r.Get<u64>(address); }, lifts)) {
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
                block_retry_.clear();  // deku-visuals-b: its blocks are gone with it
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
            const CoopGuard::Local local = GuardLocal(server_now_us);
            auto passed = held_writes_.Pass(
                std::move(writes), local, [this](std::uint64_t a) { return Where(a); },
                [this](std::uint64_t a) { return HashAt(a); });
            writes = std::move(passed.apply);
            // The held ones land now: the Sync asked for them long ago, and their drop
            // is no hit of our Link's (else it went to the owner as DAMAGE again).
            for (std::size_t i = 0; i < passed.released; ++i) {
                sync_.Written(writes[i].address, writes[i].life, server_now_us);
            }
            KeepTriggerKills(system, *scene, seen, writes, server_now_us);
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
#ifdef COOP_HOOKSHOT
        StaleGiven(system.ApplicationMemory(), r, server_now_us);
#endif
        for (const LifeWrite& w : writes) {
            WriteLife(system.ApplicationMemory(), r, w);
        }
        if (switch_share_mode_ != 0) {
            SwitchShare(system, r, *scene, server_now_us, send);
        }
        if (platforms_mode_ != 0) {
            // Row 164: the players whose game stands still now (ourselves too): their lifts stop while the server
            // clock runs, so they lead no lift (CoopCounters::CounterSync::Tick's `away`).
            counter_away_.clear();
            session->with_client([&](coop::net::Client& c) {
                for (const auto& p : c.players()) {
                    if ((p.status.value_or(0) & kCounterAway) != 0) {
                        counter_away_.insert(p.number);
                    }
                }
                return 0;
            });
            Platforms(r, *scene, self, server_now_us, send);
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
        if (const auto* left = std::get_if<coop::protocol::PlayerLeft>(&m)) {
            CoopSwitchShare::TheSharer().Forget(left->player_number);  // room-objects: their switch holds end now
        }
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
        } else if (const auto* pc = std::get_if<coop::protocol::PlatformClock>(&m)) {
            std::lock_guard lock(platforms_mutex_);  // taken on the present thread (Platforms)
            if (platforms_in_.size() < 64) {
                platforms_in_.push_back(*pc);
            }
            return;
        } else if (const auto* ss = std::get_if<coop::protocol::SwitchState>(&m)) {
            if (switch_share_mode_ != 0) {
                CoopSwitchShare::TheSharer().Receive(*ss, SteadyNowMs());  // room-objects
            }
            return;
        } else if (const auto* pk = std::get_if<coop::protocol::PlatformCounter>(&m)) {
            std::lock_guard lock(platforms_mutex_);
            if (counters_in_.size() < 64) {
                counters_in_.push_back(*pk);
            }
            return;
        }        sync_.Receive(m);
    }

    bool debug_ = false;
    bool aggro_ = false;   // LICHII_COOP_DEBUG_AGGRO (Aggro)
    u64 aggro_at_ = 0;
    u64 grass_made_logged_ = 0;
    CoopWorld::WorldSync world_;
    CoopWorld::GimmickProbe gimmick_probe_;  // LICHII_COOP_GIMMICK_PROBE=1 turns it on
    // Moving platforms (coop_platforms.h, issues 196 and 194).
    // LICHII_COOP_PLATFORMS: on unless 0; "probe" finds and sends, never writes.
    int platforms_mode_ = 1;  // 0 off, 1 on, 2 probe
    // room-objects (coop_switch_share.h): the timed switches' presses. LICHII_COOP_SWITCH_SHARE: on unless 0;
    // "probe" sends and logs, never sets.
    int switch_share_mode_ = 1;  // 0 off, 1 on, 2 probe
    struct ShareSwitch {
        u64 address = 0;
        std::string name;
        std::array<float, 3> position{};
    };
    std::vector<ShareSwitch> share_switches_;  // this walk's switches that have a SwitchPlugs line by name
    CoopPlatforms::PlatformSync platforms_;
    std::vector<CoopPlatforms::PlatformSync::Driver> platform_drivers_;
    // #196: each wave/rotate GroundControl actor's node (coop_platforms.h, GroundNode), by actor address. Checked on
    // every read (RegionOf, two words), looked for again when it fails; a search runs kReadsPerStep words a walk
    // (GroundNode::Search) until done; not found: tried again kGroundNodeRetryUs later, at most kGroundNodeTries times
    // while the actor is here.
    struct GroundNodeLook {
        std::optional<CoopPlatforms::GroundNode::Hit> hit;
        std::optional<CoopPlatforms::GroundNode::Search> search;  // the search going on, if any
        int tries = 0;
        u64 next_try_us = 0;
    };
    static constexpr u64 kGroundNodeRetryUs = 5'000'000;
    static constexpr int kGroundNodeTries = 6;
    std::map<u64, GroundNodeLook> ground_nodes_;
    std::mutex platforms_mutex_;
    std::vector<coop::protocol::PlatformClock> platforms_in_;  // under platforms_mutex_
    std::vector<coop::protocol::PlatformCounter> counters_in_;  // under platforms_mutex_
    CoopCounters::CounterSync counters_;
    // Row 164: PLAYER_STATUS bits of a game whose world stands still (a pause or quick-switch menu, a load, a
    // cutscene), and the players with one of them now (filled each share, before Platforms).
    static constexpr u8 kCounterAway = coop::protocol::status::paused | coop::protocol::status::in_menu |
                                       coop::protocol::status::loading | coop::protocol::status::in_cutscene;
    std::set<u8> counter_away_;
    std::set<u64> counter_nodes_;  // the platform nodes the ring named, while their actor is here
    // Each counter node's placement in the room table, found once a scene (Counters).
    std::unordered_map<u64, const CoopRoomObjects::Placement*> counter_places_;
    CoopCounters::OtherNodes counter_others_;  // nodes of other actors (not Obj_), not read every look
    std::string counter_scene_;
    u32 ring_count_seen_ = 0;      // the ring's count at the last look
    u64 ring_starts_ = 0;          // counter starts since the last line
    u64 ring_overruns_ = 0;        // looks that found more starts than the ring holds
    u64 counter_said_us_ = 0;
    int counter_lines_ = 0;
    std::size_t platform_lines_ = 0;
    std::vector<CoopWorld::GimmickProbe::Gimmick> gimmicks_;
    bool gimmick_probe_on_ = false;
    CoopLogicProbe::Watch logic_probe_;  // LICHII_COOP_LOGIC_PROBE=1 (jabu-lifts)
    bool logic_probe_on_ = false;
    std::vector<CoopLiftRide::Lift> ride_lifts_;  // jabu-lifts (#65): this walk's lifts
    bool lift_ride_on_ = true;                    // LICHII_COOP_LIFT_RIDE=0 turns it off
    bool scrubs_on_ = true;  // LICHII_COOP_SCRUBS=0 turns it off (issue 149)
    std::vector<CoopScrubs::Seen> scrubs_seen_;
    bool ocarina_probe_on_ = false;           // issue 152: LICHII_COOP_OCARINA_PROBE=1
    CoopOcarina::OcarinaProbe ocarina_probe_;
    CoopOcarina::HoldProbe ocarina_hold_probe_;
    u64 our_link_ = 0;                        // our Link's actor, from the last walk
    std::vector<CoopWorld::WorldSync::Out::Word> sign_words_;  // SIGN_CUT words to write
    std::vector<CoopWorld::WorldSync::Out::BlockFix> block_fixes_;  // issue 147: blocks to fix
    std::vector<CoopWorld::WorldSync::Out::BlockFix> block_retry_;  // node not found or busy: next walk
    static constexpr int kBlockNodeTries = 20;
    std::optional<coop::protocol::Vec3> player_;  // our Link in the last walk
    // Issue 144: shared changes to actors near our Link wait while it is in an
    // event (coop_guard.h). Where each actor of the last walk was, kills and
    // hurts held back, and what was said about it.
    std::unordered_map<u64, std::pair<coop::protocol::Vec3, u64>> where_;  // place, who (hash and name)
    CoopGuard::Held<LifeWrite> held_writes_{
        [](LifeWrite& kept, LifeWrite&& newer) { CoopGuard::MergeLife(kept, std::move(newer)); }};
    std::string guard_scene_;
    bool guard_said_ = false;
    // Row 98(2) trigger guards (coop_event_result.h, review-a38-5): a room event's asker asks it when the enemies its
    // trigger reads die, so another game's kill of the last one would make our asker ask that player's event here (a
    // replay that binds our Link). The event's keeper (its largest live kill actor, CoopEventResult::KillKeepers) is
    // neither killed nor removed by another game until our game has played the event; its kill is kept (one per
    // actor, the newest) and lands then, on the same actor. Our Link can still kill it: our own play. Rows 160/161:
    // an enemy our game owns is never a keeper (its death is decided here, a remote player's last hit too), and while
    // one of the event's enemies we own lives, no copy is the last one here: the owner of the last one plays it.
    std::map<u64, std::pair<LifeWrite, u64>> trigger_kept_;  // address -> the kill, the placement hash there
    std::map<u64, u64> trigger_kept_since_;  // address -> when its kill was first kept (crossing last kills)
    std::set<u64> owned_seen_;               // placement hashes our game owned in this scene (crossing last kills)
    std::map<u64, u64> owned_died_at_;       // of those, when our walk first saw it dead or gone
    std::set<u64> keeper_addresses_;                          // this walk's keepers (Place skips their removes)
    std::string keeper_scene_;
    // The follow keepers (CoopEventResult::FollowKeepers) of `scene` by our game's played flags; none before the game's
    // main is known.
    std::set<std::uint64_t> TriggerFollowKeepers(Core::System& system, const std::string& scene) const {
        if (main_ == 0) {
            return {};
        }
        CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_);
        coop::host::GameFlags flags(memory);
        return CoopEventResult::FollowKeepers(scene, [&flags](const std::string& e) {
            return flags.get_bool(coop::host::flag_hash("HavePlayedEvent." + e)).value_or(false);
        });
    }
    void KeepTriggerKills(Core::System& system, const std::string& scene, const std::vector<Seen>& seen,
                          std::vector<LifeWrite>& writes, u64 now_us) {
        if (scene != keeper_scene_) {
            keeper_scene_ = scene;
            trigger_kept_.clear();  // those actors are gone with the scene
            trigger_kept_since_.clear();
            owned_seen_.clear();
            owned_died_at_.clear();
        }
        std::unordered_map<u64, u64> hash_at;  // address -> placement hash
        std::set<u64> alive;
        for (const Seen& s : seen) {
            hash_at.emplace(s.address, s.hash);
            if (!s.life || s.life->life > 0) {
                alive.insert(s.hash);
            }
        }
        // Rows 160/161: the enemies our game owns (their death is decided here, a remote hit's too).
        std::set<u64> owned;
        for (const auto& [address, name] : sync_.Owned()) {
            if (const auto at = hash_at.find(address); at != hash_at.end()) {
                owned.insert(at->second);
                owned_seen_.insert(at->second);
            }
        }
        std::set<std::uint64_t> keepers;
        if (main_ != 0) {
            CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_);
            coop::host::GameFlags flags(memory);
            keepers = CoopEventResult::KillKeepers(
                scene, [&alive](std::uint64_t h) { return alive.contains(h); },
                [&flags](const std::string& e) {
                    return flags.get_bool(coop::host::flag_hash("HavePlayedEvent." + e)).value_or(false);
                },
                [&owned](std::uint64_t h) { return owned.contains(h); });
        }
        keeper_addresses_.clear();
        for (const auto& [a, h] : hash_at) {
            if (keepers.contains(h)) {
                keeper_addresses_.insert(a);
            }
        }
        // Crossing last kills (review-dc-0118): two owners each killed their own last enemy of the event within one
        // round trip, so each game keeps the other's kill. After kCrossingUs the lowest-numbered of those owners lets
        // it land (CoopEventResult::CrossingKillLands): that one game plays the event, the others get its flags.
        for (const u64 h : owned_seen_) {
            if (!alive.contains(h)) owned_died_at_.try_emplace(h, now_us);
        }
        for (const auto& [address, kept] : trigger_kept_) {
            const auto since = trigger_kept_since_.find(address);
            if (since == trigger_kept_since_.end()) continue;
            const u64 kept_at = since->second;
            const auto ours_died = [&](std::uint64_t h) {
                const auto d = owned_died_at_.find(h);
                return d != owned_died_at_.end() &&
                       (d->second > kept_at ? d->second - kept_at : kept_at - d->second) <= CoopEventResult::kCrossingGapUs;
            };
            if (!keeper_addresses_.contains(address)) {
                continue;
            }
            const std::optional<u8> owner = sync_.OwnerOrDeathOwnerOf(address);
            const bool owner_here = owner && sync_.InOurScene(*owner, now_us);
            const bool we_decide = sync_.Self() != 0 && sync_.SceneDecider(sync_.Self(), now_us) == sync_.Self();
            if (!CoopEventResult::CrossingKillLands(scene, kept.second, now_us - kept_at, sync_.Self(), owner,
                                                    ours_died, owner_here, we_decide)) {
                continue;
            }
            LOG_INFO(Core, "coop: dungeon: trigger: the kill of {:#x} kept {} ms lands: {} (player {}), our game plays "
                           "the event", kept.second, (now_us - kept_at) / 1000,
                     owner_here ? "crossing last kills, ours is the lowest owner's number"
                                : "its owner is gone from our scene, we decide here",
                     owner.value_or(0));
            keeper_addresses_.erase(address);
        }
        // A kept kill whose actor is no keeper any more lands now, on the same actor (else it is dropped).
        for (auto it = trigger_kept_.begin(); it != trigger_kept_.end();) {
            if (keeper_addresses_.contains(it->first)) {
                ++it;
                continue;
            }
            const auto at = hash_at.find(it->first);
            if (at != hash_at.end() && at->second == it->second.second) {
                LOG_INFO(Core, "coop: dungeon: trigger: the kill of {:#x} kept for its event lands now", at->second);
                sync_.Written(it->first, it->second.first.life, now_us);
                writes.push_back(std::move(it->second.first));
            } else {
                // Row 160 (hiddenblitz 20261119-201953, room 1): a kept kill was dropped with no line. Harmless when
                // the event was played elsewhere (its clear flag comes), but a stuck room must show it in the log.
                LOG_INFO(Core, "coop: dungeon: trigger: the kill of {:#x} kept for its event is dropped: {}",
                         it->second.second,
                         at == hash_at.end() ? "its actor is gone from our walk" : "another actor is at its address");
            }
            trigger_kept_since_.erase(it->first);
            it = trigger_kept_.erase(it);
        }
        std::erase_if(writes, [&](LifeWrite& w) {
            if (w.life > 0 || w.heal || w.test_kill || !keeper_addresses_.contains(w.address)) {
                return false;
            }
            const u64 hash = hash_at.at(w.address);
            if (!trigger_kept_.contains(w.address)) {
                LOG_INFO(Core, "coop: dungeon: trigger: another game's kill of {:#x} waits: the last enemy of a room "
                               "event our game has not played (its asker would ask the event here)", hash);
            }
            trigger_kept_since_.try_emplace(w.address, now_us);
            trigger_kept_.insert_or_assign(w.address, std::make_pair(std::move(w), hash));
            return true;
        });
    }
    // Row 97: the event's partners wait wherever they are (Sync::EventPartner).
    CoopGuard::Local GuardLocal(u64 now_us) const {
        // G1: near-by changes wait in our Link's event; the event's own actors in any event here.
        const bool any = TheLocalEvent().load();
        const bool ours = any && TheOwnEvent().load();
        return {ours, player_,
                [this, now_us](std::uint64_t a) { return sync_.EventPartner(a, now_us); }, any && !ours};
    }
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
    // PvP hurts being given to our Link (GivePvp, CoopPvp::FollowLife).
    std::vector<CoopPvp::PendingGive> pvp_;
    s32 pvp_life_ = -1;  // our Link's life at the last GivePvp (after its write); -1: none (a new scene)
    CoopPvp::LinkLifeWatch pvp_watch_;  // pairs a fall with its hit id across one read (GivePvp)
    CoopPvpNative::CopyHitWatch pvp_copy_watch_;  // phase 4: the _Pvp copies' last hit ids (ReadPvpCopies)
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
            stamps_.clear();
            no_area_.clear();
            TheMover().ForgetReplayed();
#ifdef COOP_HOOKSHOT
            CoopActorEvents::TheKeptDeaths().Clear();
#endif
            sync_.Reset();
            world_.Reset();
            ForgetGold();  // issue 89: a new game process, its own keys
            block_retry_.clear();
            ForgetBosses();
            boss_hit_ids_.clear();
            hit_lives_.clear();
            pvp_life_ = -1;
            pvp_watch_.Reset();
            CoopProjectiles::TheOwnShots().Reset();
            extras_.Reset();  // difficulty: that run's extra enemies are gone with it
            horde_.Reset();
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
    // room-objects (coop_switch_share.h): our timed switches' presses out, the others' held on ours.
    // g3-objects: a line with guard flags (a plug that also raises our event asker) is armed only once all of them
    // are on in our game (its asker can no longer fire: the hold gives the result, no replay). A load guard is read
    // here too; the Sharer keeps its value at the room's load (Seen::load_on).
    void SwitchShare(Core::System& system, Reader& r, const std::string& scene, u64 server_now_us,
                     std::vector<coop::protocol::Message>& send) {
        std::vector<CoopSwitchShare::Seen> seen;
        std::optional<CoopHook::EmulatorMemory> memory;
        for (const ShareSwitch& s : share_switches_) {
            for (const CoopRoomObjects::SwitchPlug* line : CoopRoomObjects::SwitchPlugsOf(scene, s.name, s.position)) {
                CoopSwitchShare::Seen v;
                v.line = line;
                v.address = s.address;
                for (const CoopRoomObjects::GuardFlag& guard : CoopRoomObjects::GuardFlags(*line)) {
                    if (!memory) memory.emplace(system.ApplicationMemory(), main_);
                    if (!coop::host::GameFlags(*memory).get_bool(coop::host::flag_hash(guard.flag)).value_or(false)) {
                        (guard.at_load ? v.load_on : v.armed) = false;
                    }
                }
                const std::uint32_t key = CoopSwitchShare::PlugKey(line->plug) ^ 7u;
                if (const auto plugs = CoopLogicProbe::Read([&r](u64 a) { return r.Get<u64>(a); }, s.address)) {
                    for (const CoopLogicProbe::Plug& p : *plugs) {
                        if (p.key == key && p.type == CoopLogicProbe::kBool && p.value) {
                            v.on = *p.value != 0;
                            break;
                        }
                    }
                }
                seen.push_back(v);
            }
        }
        std::vector<coop::protocol::SwitchState> out;
        std::vector<std::string> notes;
        const auto holds = CoopSwitchShare::TheSharer().Tick(scene, seen, !sync_.OthersHere(server_now_us).empty(),
                                                             SteadyNowMs(), out, notes);
        for (auto& m : out) {
            send.emplace_back(std::move(m));
        }
        if (switch_share_mode_ == 1) {
            auto& setter = CoopSwitchShare::TheSetter();
            setter.Want(holds, main_, SteadyNowMs());
            for (const auto& line : setter.TakeNotes()) {
                LOG_INFO(Core, "coop: {}", line);
            }
        }
        for (const auto& line : notes) {
            LOG_INFO(Core, "coop: {}{}", line, switch_share_mode_ == 2 ? " (probe: not set)" : "");
        }
    }

    // Moving platforms (coop_platforms.h): our drivers' clocks out, the
    // owners' clocks into ours.
    void Platforms(Reader& r, const std::string& scene, u8 self, u64 server_now_us,
                   std::vector<coop::protocol::Message>& send) {
        {
            std::vector<coop::protocol::PlatformClock> in;
            {
                std::lock_guard lock(platforms_mutex_);
                in.swap(platforms_in_);
            }
            for (const auto& m : in) {
                platforms_.Receive(m);
            }
        }
        // Looping platforms' pause counters first (coop_counters.h): the
        // clock-word path below leaves their actors alone.
        Counters(r, scene, self, server_now_us, send);
        if (const auto& looping = counters_.CounterOwners(); !looping.empty()) {
            std::erase_if(platform_drivers_, [&](const auto& d) { return looping.contains(d.hash); });
        }
        std::erase_if(ground_nodes_, [this](const auto& e) {
            return std::none_of(platform_drivers_.begin(), platform_drivers_.end(),
                                [&e](const auto& d) { return d.address == e.first; });
        });
        const auto read = [&r, this](const CoopPlatforms::PlatformSync::Driver& d) {
            auto words = CoopWorld::SignProbe::Read([&r](u64 a) { return r.Get<u64>(a); }, d.address);
            if (!words.empty()) {
                if (const u64 slot = AsSlot0(r, d.address); slot != 0) {
                    if (const auto frame = r.Get<u32>(slot + kAsSlotFrame)) {
                        words[CoopPlatforms::Key(CoopPlatforms::kRegionAsFrame, 0)] = *frame;
                    }
                }
                if (const auto* hit = GroundNodeOf(r, d)) {
                    words.merge(CoopPlatforms::GroundNode::Words([&r](u64 a) { return r.Get<u64>(a); }, *hit));
                }
            }
            return words;
        };
        // Issue 194: the game's own ride state of each platform near our Link, its AI's bool plug
        // RideOnPlayer (CoopLogicProbe::Read; the plug's key is Murmur3 ^ 7): no clock write while he rides it.
        if (player_) {
            const std::vector<coop::protocol::Vec3> others = sync_.OthersHere(server_now_us);
            for (auto& d : platform_drivers_) {
                d.ride_plug.reset();  // read fresh each tick
                d.other_on = std::any_of(others.begin(), others.end(), [&d](const coop::protocol::Vec3& o) {
                    return CoopPlatforms::PlatformSync::OnFootprint(d.position, o);
                });
                const float dx = d.position[0] - (*player_)[0];
                const float dy = d.position[1] - (*player_)[1];
                const float dz = d.position[2] - (*player_)[2];
                const float near_m = CoopPlatforms::PlatformSync::kNearM;
                if (dx * dx + dy * dy + dz * dz > near_m * near_m) {
                    continue;
                }
                const auto plugs = CoopLogicProbe::Read([&r](u64 a) { return r.Get<u64>(a); }, d.address);
                if (!plugs) {
                    continue;
                }
                for (const CoopLogicProbe::Plug& p : *plugs) {
                    if (p.key == (CoopLiftRide::kRideOnPlayer ^ 7u) && p.type == CoopLogicProbe::kBool && p.value) {
                        d.ride_plug = *p.value != 0;
                        break;
                    }
                }
            }
        }
        if (player_) {  // the test's "place PLATFORM UP" (TestPlatformPlace)
            std::map<std::string, std::pair<float, std::array<float, 3>>> nearest;
            for (const auto& d : platform_drivers_) {
                const float dx = d.position[0] - (*player_)[0];
                const float dz = d.position[2] - (*player_)[2];
                const float m = dx * dx + dz * dz;
                const auto it = nearest.find(d.name);
                if (it == nearest.end() || m < it->second.first) {
                    nearest[d.name] = {m, {d.position[0], d.position[1], d.position[2]}};
                }
            }
            TestPlatformPlaces& places = ThePlatformPlaces();
            std::lock_guard lock(places.mutex);
            places.at.clear();
            for (const auto& [name, near] : nearest) {
                places.at[name] = near.second;
            }
        }
        CoopPlatforms::PlatformSync::Out out;
        platforms_.Tick(scene, self, CoopEvery::NowUs(), server_now_us, platform_drivers_, player_, read,
                        platforms_mode_ == 2, out);
        for (auto& m : out.send) {
            send.emplace_back(std::move(m));
        }
        for (const auto& w : out.writes) {
            const u64 at = NodeWordAt(r, w.address, w.key).value_or(PlatformWordAt(r, w.address, w.key));
            if (at == 0 || memory_ == nullptr || !memory_->IsValidVirtualAddressRange(at, 4)) {
                continue;
            }
            u32 bits = 0;
            std::memcpy(&bits, &w.value, 4);
            memory_->Write32(at, bits);
            if (platform_lines_ < 200) {
                ++platform_lines_;
                LOG_INFO(Core, "coop: platforms: {:016x} {} {:.3f} -> {:.3f}", w.hash,
                         CoopPlatforms::Where(w.key), w.was, w.value);
            }
        }
        for (const auto& line : out.notes) {
            LOG_INFO(Core, "coop: platforms: {}", line);
        }
    }

    // An enemy or a boss: its life is never written (the native rule), only changed by its game's own steps.
    bool NativeOnly(u64 actor) const {
        return NativeOnlyName(NameOf(actor));
    }

    std::string NameOf(u64 actor) const {
        const auto known = names_.find(actor);
        return known != names_.end() ? known->second.first : std::string("?");
    }
    std::size_t lives_lost_ = 0;  // enemy life changes no game step took (never written: lost)

    // The pause counters (coop_counters.h): the nodes the cave kept, read.
#ifdef COOP_HOOKSHOT
    // The game's own creates, deletes and hits (patch 0007), drained each
    // presented frame. First step: counted and said, per kind, against
    // what the walk sees, so one run shows the events are the game's own
    // (docs/hookshot.md "Actor events"); the walk still decides.
    void ActorEvents(u64 server_now_us) {
        std::size_t lost = 0;
        const auto events = CoopActorEvents::TheRing().Drain(&lost);
        actor_events_lost_ += lost;
        sync_.SetDeleteEvents(true);  // churn-b: off the walk with no delete request is away, not gone
        for (const auto& e : events) {
            switch (e.kind) {
            case CoopActorEvents::Kind::Create:
                ++actor_creates_;
                if ((e.flags & CoopActorEvents::kStopped) != 0) {
                    ++actor_stops_;
                    if (actor_stop_lines_ < 10) {
                        ++actor_stop_lines_;
                        LOG_INFO(Core, "coop: actors: '{}' not made: a copy enemy's own shot, refused at the game's create "
                                       "request (the owner's comes as a copy)", e.Name());
                    }
                }
                if (actor_event_lines_ < 40 && (e.flags & CoopActorEvents::kOwn) == 0) {
                    ++actor_event_lines_;
                    float place[3] = {};
                    for (int i = 0; i < 3; ++i) {
                        const auto bits = static_cast<u32>(e.place[i]);
                        std::memcpy(&place[i], &bits, 4);
                    }
                    LOG_INFO(Core, "coop: actors: create '{}' from main+{:#x} at ({:.1f}, {:.1f}, {:.1f})", e.Name(),
                             e.from >= main_ && main_ != 0 ? e.from - main_ : e.from, place[0], place[1], place[2]);
                }
                break;
            case CoopActorEvents::Kind::Delete: {
                ++actor_deletes_;
                // (gone at the first walk without it; the game's own, not ours: an enemy of ours ended, Left)
                sync_.GameDeleted(e.a, server_now_us, (e.flags & CoopActorEvents::kOwn) != 0);
                NoteBossDeleted(e.a);  // (a boss body off the walk: removed, not away)
                const auto known = names_.find(e.a);
                if (actor_event_lines_ < 40 && known != names_.end()) {
                    ++actor_event_lines_;
                    LOG_INFO(Core, "coop: actors: {} {:#x} '{}' (reason {})",
                             (e.flags & CoopActorEvents::kOwn) != 0 ? "co-op deletes" : "the game deletes", e.a,
                             known->second.first, e.b);
                }
                break;
            }
            case CoopActorEvents::Kind::Damage:
                ++actor_hits_;
                actor_hit_sum_ += CoopActorEvents::Amount(e.b);
                if (step_hits_.size() < kStepHitsKept) {
                    step_hits_.emplace_back(e.a, CoopActorEvents::Amount(e.b));  // (for the Sync: StepHits)
                }
                break;
            default:
                break;
            }
        }
        if (actor_events_said_us_ == 0) {
            actor_events_said_us_ = server_now_us;
        } else if (server_now_us >= actor_events_said_us_ + 10'000'000 &&
                   (actor_creates_ | actor_deletes_ | actor_hits_ | actor_events_lost_) != 0) {
            actor_events_said_us_ = server_now_us;
            const std::uint64_t passed = CoopActorEvents::StopsPassed().load(std::memory_order_relaxed);
            const std::uint64_t kept = CoopActorEvents::RevivesSkipped().load(std::memory_order_relaxed);
            const std::uint64_t again = CoopActorEvents::RevivesKeptSkipped().load(std::memory_order_relaxed);
            LOG_INFO(Core, "coop: actors: in 10 s the game asked {} creates ({} copies' shots refused), {} deletes, "
                           "{} hits (life {}), {} lost; owners' deaths past a copy's own life stops {}, past its "
                           "revive {} (later steps kept dead {})",
                     actor_creates_, actor_stops_, actor_deletes_, actor_hits_, actor_hit_sum_, actor_events_lost_,
                     passed, kept, again);
            actor_creates_ = actor_deletes_ = actor_hits_ = actor_events_lost_ = actor_stops_ = 0;
            actor_hit_sum_ = 0;
        }
    }
    // The game's hits since the last walk, by Life component, handed to the
    // Sync as this game's hits on its copies (Sync::GameHit) instead of the
    // life read falling. Not with LICHII_COOP_OBJECTS_HIT_REPLAY: there the
    // other players' hits are dealt by the game too and would come back.
    void StepHits() {
        const bool on = CoopActorEvents::LifeStepSeen().load(std::memory_order_relaxed) && !replay_;
        if (on != sync_.LifeEvents()) {
            sync_.SetLifeEvents(on);
            LOG_INFO(Core, "coop: actors: hits on copies are {}", on ? "taken from the game's Life step (patch 0007)"
                                                                     : "read from the life falling");
        }
        for (const auto& [component, amount] : step_hits_) {
            const auto actor = life_components_.find(component);
            if (on && actor != life_components_.end()) {
                sync_.GameHit(actor->second, amount);
            }
        }
        step_hits_.clear();
        if (const std::size_t loose = sync_.LooseDrops(); loose != loose_said_) {
            LOG_INFO(Core, "coop: actors: {} life fell on copies with no Life step for it (counted as hits)",
                     loose - loose_said_);
            loose_said_ = loose;
        }
    }
    // A life change given to the Life step that no step took in kGivenWaitUs
    // (the actor asleep or paused): taken back and written, as before.
    void StaleGiven(Core::Memory::Memory& memory, Reader& r, u64 now_us) {
        auto& steps = CoopActorEvents::TheLifeTable();
        std::erase_if(life_given_, [&](const LifeGiven& g) {
            if (now_us < g.at_us + kGivenWaitUs) {
                return false;
            }
            const auto table = r.Ptr(g.address + kActorComponents);
            const auto life = table ? r.Ptr(*table + kComponentLife) : std::nullopt;
            if (!life || *life != g.component) {
                steps.Take(g.component);  // (another actor there now, or none)
                return true;
            }
            const s32 left = steps.Take(g.component);
            if (left != 0 && NativeOnly(g.address)) {
                ++lives_lost_;
                LOG_INFO(Core, "coop: objects: {:#x} '{}': its Life step did not take the change: lost, its life "
                               "stays its game's ({} lost so far)",
                         g.address, NameOf(g.address), lives_lost_);
            } else if (left != 0) {
                const auto value = LifeValue(r, g.address);
                if (value && memory.IsValidVirtualAddressRange(value->first + 8, 4) &&
                    r.Get<s32>(value->first + 8).value_or(g.life) > g.life) {
                    memory.Write32(value->first + 8, static_cast<u32>(g.life));
                    LOG_INFO(Core, "coop: objects: {:#x} '{}': its Life step did not take the change: life written {}",
                             g.address, NameOf(g.address), g.life);
                }
            }
            return true;
        });
    }
    struct LifeGiven {
        u64 component = 0;
        u64 address = 0;
        s32 life = 0;
        u64 at_us = 0;
    };
    // Has the enemy's own damage step seen the hit we replayed last on this Life component? (Its last hit id,
    // Life + 0x1b18, reached our id: hit ids only grow.) True when we never replayed one.
    bool ReplaySeen(Reader& r, u64 life) const {
        const u32 ours = TheMover().ReplayedId(life);
        if (ours == 0xffffffffu) {
            return true;
        }
        const u32 last = r.Get<u32>(life + kLastHitId).value_or(0xffffffffu);
        return last != 0xffffffffu && static_cast<s32>(last - ours) >= 0;
    }
    std::string life_scene_;
    bool life_table_full_said_ = false;
    static constexpr std::size_t kStepHitsKept = 4096;
    static constexpr u64 kGivenWaitUs = 250'000;
    std::vector<std::pair<u64, s32>> step_hits_;
    std::vector<LifeGiven> life_given_;
    std::size_t loose_said_ = 0;
    std::size_t actor_creates_ = 0;
    std::size_t actor_stops_ = 0;
    std::size_t actor_stop_lines_ = 0;
    std::size_t actor_deletes_ = 0;
    std::size_t actor_hits_ = 0;
    std::size_t actor_events_lost_ = 0;
    std::int64_t actor_hit_sum_ = 0;
    std::size_t actor_event_lines_ = 0;
    u64 actor_events_said_us_ = 0;
#endif

    void Counters(Reader& r, const std::string& scene, u8 self, u64 server_now_us,
                  std::vector<coop::protocol::Message>& send) {
        u64 ring = 0;
        {
            CounterRingBox& box = TheCounterRing();
            std::lock_guard lock(box.mutex);
            if (box.main != main_) {
                // A new game boot in this process: a new ring for its heap (review).
                box.ring = 0;
                box.asked = false;
                box.allocating = false;
                counter_nodes_.clear();
                counter_places_.clear();  // PR 279 review M2: a placement is kept only while its node is
                counter_others_.Clear();
                ring_count_seen_ = 0;
#ifdef COOP_HOOKSHOT
                CoopCounters::TheHostRing().Clear();
                ring_count_seen_ = CoopCounters::TheHostRing().Count();
                CoopActorEvents::TheLifeTable().Clear();  // (another heap's components)
                CoopDifficulty::TheKinds().Clear();
                CoopActorEvents::TheRing().Forget();
                CoopWorld::TheNodeStarts().Clear();  // (patch 0029: another heap's nodes)
#endif
                CoopDbno::SetOurLifeComponent(0);  // down but not out: until our Link is seen again
            }
            box.main = main_;
            ring = box.ring;
        }
        {
            std::vector<coop::protocol::PlatformCounter> in;
            {
                std::lock_guard lock(platforms_mutex_);
                in.swap(counters_in_);
            }
            for (const auto& m : in) {
                counters_.Receive(m);
            }
        }
        if (scene != counter_scene_) {
            counter_scene_ = scene;  // a new scene: its actors are new
            counter_places_.clear();
            counter_nodes_.clear();
            counter_others_.Clear();
        }
        counter_others_.Keep([this](std::uint64_t actor) { return names_.contains(actor); });
        const auto actor_of = [&](u64 node, u64* graph_out) -> u64 {
            const u64 vtable = r.Get<u64>(node).value_or(0);
            const u64 graph = vtable == main_ + CoopCounters::kNodeVtable
                                  ? r.Get<u64>(node + CoopCounters::kNodeGraph).value_or(0)
                                  : 0;
            const u64 mid = graph ? r.Get<u64>(graph + CoopCounters::kGraphOwner).value_or(0) : 0;
            if (graph_out != nullptr) {
                *graph_out = graph;
            }
            return mid ? r.Get<u64>(mid + CoopCounters::kOwnerActor).value_or(0) : 0;
        };
        std::size_t named_new = 0;
#ifdef COOP_HOOKSHOT
        const bool have_ring = true;  // (the world hook's, in our memory)
        (void)ring;
#else
        const bool have_ring = ring != 0;
#endif
        if (have_ring) {
            std::array<u64, CoopCounters::kRingNodes> entries{};
#ifdef COOP_HOOKSHOT
            const u32 count = CoopCounters::TheHostRing().Read(entries);
#else
            for (std::size_t i = 0; i < entries.size(); ++i) {
                entries[i] = r.Get<u64>(ring + 8 + 8 * i).value_or(0);
            }
            const u32 count = r.Get<u32>(ring).value_or(0);
#endif
            // Starts since the last look: more than the ring holds and some were lost.
            const u32 since = count - ring_count_seen_;
            ring_count_seen_ = count;
            ring_starts_ += since;
            if (since > CoopCounters::kRingNodes) {
                ++ring_overruns_;
            }
            for (const u64 node : CoopCounters::RingNodes(entries)) {
                if (counter_nodes_.size() < 512 && !counter_nodes_.contains(node) &&
                    !(counter_others_.Contains(node) && counter_others_.Skip(node, actor_of(node, nullptr)))) {
                    counter_nodes_.insert(node);
                    ++named_new;
                }
            }
        }
        std::vector<CoopCounters::Seen> seen;
        for (auto it = counter_nodes_.begin(); it != counter_nodes_.end();) {
            const u64 node = *it;
            u64 graph = 0;
            const u64 actor = actor_of(node, &graph);
            const auto known = actor ? names_.find(actor) : names_.end();
            if (known == names_.end()) {
                counter_places_.erase(node);  // review M2: a reused address must not keep another lift's placement
                it = counter_nodes_.erase(it);  // gone, reused, or its actor left the walk
                continue;
            }
            if (!CoopCounters::IsPlatformOwner(known->second.first)) {
                // An enemy's, an NPC's, Link's: not read again while it is
                // that actor's (CoopCounters::OtherNodes).
                counter_others_.Add(node, actor);
                counter_places_.erase(node);
                it = counter_nodes_.erase(it);
                continue;
            }
            ++it;
            CoopCounters::Seen s;
            s.node = node;
            s.owner_name = known->second.first;
            s.owner_hash = known->second.second;
            s.remaining = r.Get<float>(node + CoopCounters::kNodeRemaining).value_or(0.0f);
            s.duration = r.Get<float>(node + CoopCounters::kNodeDuration).value_or(0.0f);
            s.key = CoopCounters::KeyOf(s.owner_hash, s.duration, node, graph);
            // Which wait it is at, and whether its own logic moves it (report 138's review): from its placement in
            // the room table, the same in every game. The placement is found once per node (kept for the scene),
            // by its horizontal place: a lift moves up and down, and Dodongo's three are 15 m apart, so its height
            // never makes it look like another one. No placement known: no wait (the period rule only).
            std::array<float, 3> at{};
            for (std::size_t i = 0; i < 3; ++i) {
                at[i] = r.Get<float>(actor + kActorPosition + 4 * i).value_or(0.0f);
            }
            auto [placed, fresh_node] = counter_places_.try_emplace(node, nullptr);
            if (fresh_node) {
                float best = 0.0f;
                for (const CoopRoomObjects::Placement& p : CoopRoomObjects::Placements()) {
                    if (p.scene != scene || p.name != s.owner_name) {
                        continue;
                    }
                    const float dx = at[0] - p.place[0];
                    const float dz = at[2] - p.place[2];
                    if (placed->second == nullptr || dx * dx + dz * dz < best) {
                        placed->second = &p;
                        best = dx * dx + dz * dz;
                    }
                }
            }
            if (const CoopRoomObjects::Placement* p = placed->second) {
                float d2 = 0.0f;
                for (std::size_t i = 0; i < 3; ++i) {
                    d2 += (at[i] - p->place[i]) * (at[i] - p->place[i]);
                }
                s.home_m = std::sqrt(d2);
                s.wait = d2 <= CoopCounters::kHomeM * CoopCounters::kHomeM ? 0 : 1;
                // Row 164: only a player's own input (a rider, a player sensor) keeps it out; a shared switch's
                // input (Dodongo's shortcut lift, IsShortCutMove) runs the same in every game: lined up once it loops.
                s.driven = CoopRoomObjects::PlayerDriven(p->why);
                s.switched = !p->why.empty() && !s.driven;
            }
            seen.push_back(std::move(s));
        }
        // What the ring caught, every 10 s while a platform counter is near
        // (the 2026-11-12 run had no counter line at all; this says why).
        if (have_ring && counter_lines_ < 60 &&
            (counter_said_us_ == 0 || server_now_us >= counter_said_us_ + 10'000'000)) {
            counter_said_us_ = server_now_us;
            ++counter_lines_;
            std::string platforms;
            for (std::size_t i = 0; i < seen.size() && i < 6; ++i) {
                platforms += fmt::format(" {} {:.1f}/{:.1f}", seen[i].owner_name, seen[i].remaining,
                                         seen[i].duration);
            }
            LOG_INFO(Core,
                     "coop: platforms: pause counters: {} starts in 10 s ({} looks overran the ring), "
                     "{} new nodes now, {} platform nodes, {} other actors' nodes, {} looping:{}",
                     ring_starts_, ring_overruns_, named_new, seen.size(), counter_others_.Size(),
                     counters_.LoopingOwners().size(), platforms);
            ring_starts_ = 0;
            ring_overruns_ = 0;
        }
        CoopCounters::CounterSync::Out out;
        counters_.Tick(scene, self, server_now_us, seen, platforms_mode_ == 2, out, counter_away_);
        for (auto& m : out.send) {
            send.emplace_back(std::move(m));
        }
        for (const auto& w : out.writes) {
            if (platforms_mode_ == 2 || memory_ == nullptr ||
                !memory_->IsValidVirtualAddressRange(w.node + CoopCounters::kNodeRemaining, 4)) {
                continue;  // LICHII_COOP_PLATFORMS=probe: logged, not written
            }
            u32 bits = 0;
            std::memcpy(&bits, &w.remaining, 4);
            memory_->Write32(w.node + CoopCounters::kNodeRemaining, bits);
        }
        for (const auto& line : out.notes) {
            LOG_INFO(Core, "coop: platforms: {}", line);
        }
    }

    // The actor's AS slot 0, or 0 (the chain Animation reads).
    static u64 AsSlot0(Reader& r, u64 actor) {
        const u64 table = r.Get<u64>(actor + kActorComponents).value_or(0);
        const u64 as = table ? r.Get<u64>(table + kComponentsAs).value_or(0) : 0;
        const u64 inner = as ? r.Get<u64>(as + kAsInner).value_or(0) : 0;
        const u64 controller = inner ? r.Get<u64>(inner + kAsController).value_or(0) : 0;
        const u64 array = controller ? r.Get<u64>(controller + kAsSlotArray).value_or(0) : 0;
        return array ? r.Get<u64>(array).value_or(0) : 0;
    }

    // #196: the wave/rotate node of a GroundControl driver (coop_platforms.h, GroundNode), or nullptr: only for
    // GroundControlActor_WaveLoop / _Rotate (and the game's GrouondControl spelling). Logs where it was found once.
    const CoopPlatforms::GroundNode::Hit* GroundNodeOf(Reader& r, const CoopPlatforms::PlatformSync::Driver& d) {
        const std::string_view name = d.name;
        if (!(name.starts_with("GroundControlActor_") || name.starts_with("GrouondControlActor_")) ||
            (name.find("WaveLoop") == std::string_view::npos && name.find("Rotate") == std::string_view::npos)) {
            return nullptr;
        }
        const auto read = [&r](u64 a) { return r.Get<u64>(a); };
        GroundNodeLook& look = ground_nodes_[d.address];
        if (look.hit && CoopPlatforms::GroundNode::RegionOf(read, look.hit->node, d.address, main_) == look.hit->region) {
            return &*look.hit;
        }
        look.hit.reset();
        if (!look.search) {
            const u64 now = CoopEvery::NowUs();
            if (look.tries >= kGroundNodeTries || now < look.next_try_us) {
                return nullptr;
            }
            ++look.tries;
            look.next_try_us = now + kGroundNodeRetryUs;
            look.search.emplace(d.address, main_);
        }
        // B #29 N1: a part of the search each walk, never the whole of it in one frame.
        look.hit = look.search->Step(read, CoopPlatforms::GroundNode::kReadsPerStep);
        if (look.hit) {
            look.tries = 0;
            LOG_INFO(Core, "coop: platforms: {:016x} {}: its {} node at {:#x} ({}, {} objects / {} words looked at)",
                     d.hash, d.name, look.hit->region == CoopPlatforms::kRegionWaveNode ? "wave" : "rotate",
                     look.hit->node, look.hit->path, look.search->Looked(), look.search->Reads());
            look.search.reset();
            return &*look.hit;
        }
        if (look.search->Done()) {
            if (look.tries == kGroundNodeTries) {
                LOG_INFO(Core,
                         "coop: platforms: {:016x} {}: no GroundControl node found ({} tries, {} objects the last)",
                         d.hash, d.name, look.tries, look.search->Looked());
            }
            look.search.reset();
        }
        return nullptr;
    }

    // #196: where a node-region key is (the driver's GroundControl node + offset), 0 when that node is not there
    // now; nothing for any other key.
    std::optional<u64> NodeWordAt(Reader& r, u64 actor, std::uint64_t key) const {
        const std::uint64_t region = key >> 32;
        const std::uint64_t offset = key & 0xffffffffu;
        if (region != CoopPlatforms::kRegionWaveNode && region != CoopPlatforms::kRegionRotateNode) {
            return std::nullopt;
        }
        const auto it = ground_nodes_.find(actor);
        if (it == ground_nodes_.end() || !it->second.hit || offset < CoopPlatforms::GroundNode::kFrom ||
            offset >= CoopPlatforms::GroundNode::kTo ||
            CoopPlatforms::GroundNode::RegionOf([&r](u64 a) { return r.Get<u64>(a); }, it->second.hit->node, actor,
                                                main_) != region) {
            return u64{0};
        }
        return it->second.hit->node + offset;
    }

    // Where a PLATFORM_CLOCK key is in this actor, or 0.
    static u64 PlatformWordAt(Reader& r, u64 actor, std::uint64_t key) {
        const std::uint64_t region = key >> 32;
        const std::uint64_t offset = key & 0xffffffffu;
        if (region == CoopPlatforms::kRegionAsFrame) {
            const u64 slot = AsSlot0(r, actor);
            return slot ? slot + kAsSlotFrame : 0;
        }
        if (region == CoopPlatforms::kRegionActor) {
            return offset < coop::protocol::kPlatformActorBytes ? actor + offset : 0;
        }
        if (region < CoopPlatforms::kRegionComponent ||
            region - CoopPlatforms::kRegionComponent >= coop::protocol::kPlatformComponents ||
            offset >= coop::protocol::kPlatformComponentBytes) {
            return 0;
        }
        const u64 table = r.Get<u64>(actor + kActorComponents).value_or(0);
        const u64 c = table ? r.Get<u64>(table + 8 * (region - CoopPlatforms::kRegionComponent)).value_or(0) : 0;
        return c ? c + offset : 0;
    }

    bool Collect(Reader& r, std::vector<Seen>& out) {
        gimmicks_.clear();  // dungeon gimmicks for the probe
        ride_lifts_.clear();  // jabu-lifts (#65)
        share_switches_.clear();  // room-objects
        platform_drivers_.clear();
        scrubs_seen_.clear();  // issue 149
        std::vector<u64> actors;
        if (!List(r, actors)) {
            return false;
        }
        // Row 97 / G1: our own Link's EventMember, read with the game's own test
        // (CoopEventMember::ReadMember): an actor is in our event when a member slot of its own holds
        // the same event as one of our Link's (InSameEvent). Our Link's actor is the last walk's.
        // Also logged: our Link's on each change, with the running event and the event ring; every
        // other actor's while an event runs here or our Link's member says anything.
        const auto read_word = [&r](std::uint64_t at) { return r.Get<u64>(at); };
        const CoopEventMember::MemberRead link_member = CoopEventMember::ReadMember(read_word, our_link_);
        const bool link_in_event = link_member.Member();
        if (!link_in_event) {
            member_said_.clear();
        }
        const bool members_wanted = TheLocalEvent().load() || link_member.SomeReason() || link_member.state != 0;
        {
            const auto bits = CoopEventMember::PerformerBits(read_word, our_link_);
            std::string text = CoopEventMember::MemberText(link_member) + " performer bits " +
                               (bits ? CoopEventMember::Hex(*bits) : std::string("?"));
            if (our_link_ != 0 && text != link_member_said_) {
                const auto ev = CoopEventMember::ReadRunning(read_word, main_);
                LOG_INFO(Core, "coop: event: member: our Link {:#x}: {}{}; running event {:#x}, ring {}; {}", our_link_,
                         text, link_member.Member() ? " (an event member, the game's test)" : "", ev.event,
                         CoopEventMember::EventRingText(read_word, main_),
                         CoopEventMember::MemberDump(read_word, link_member));
                link_member_said_ = std::move(text);
            }
            if (!members_wanted) {
                members_said_.clear();
            }
        }
        const auto member_log = [&](u64 actor, const std::string& name) {
            if (!members_wanted || actor == our_link_) {
                return;
            }
            const CoopEventMember::MemberRead m = CoopEventMember::ReadMember(read_word, actor);
            const auto said = members_said_.find(actor);
            if (!m.SomeReason() && m.state == 0 && said == members_said_.end()) {
                return;  // in no event and asking for none since this one began: nothing to say
            }
            std::string text = CoopEventMember::MemberText(m);
            if (said != members_said_.end() ? said->second == text : members_said_.size() >= kMembersSaidMax) {
                return;
            }
            LOG_INFO(Core, "coop: event: member: {} {:#x}: {}{}; {}", name, actor, text,
                     m.Member() ? " (an event member, the game's test)" : "", CoopEventMember::MemberDump(read_word, m));
            members_said_[actor] = std::move(text);
        };
        const auto member_of = [&](u64 actor, const std::string& name) {
            const std::optional<bool> in = CoopEventMember::InSameEvent(read_word, actor, link_member);
            // Once per actor name and event, for the next run.
            if (link_in_event && member_said_.size() < kMemberSaidMax && member_said_.insert(name).second) {
                LOG_INFO(Core, "coop: event: {} {:#x}: {}", name, actor,
                         !in   ? "no event member component"
                         : *in ? "an event member in our Link's event"
                               : "not an event member in our Link's event");
            }
            return in;
        };
        std::optional<coop::protocol::Vec3> player;
        std::vector<coop::protocol::Vec3> foes;  // down but not out: living enemies, where they stand
        // Issue 144 and the scrubs' one-time sales: the scrubs whose flags may delete them from under
        // an event of ours (coop_scrub_talk.h, coop_scrub_sale.h).
        struct TalkScrub {
            u64 hash = 0;
            coop::protocol::Vec3 at{};
            std::optional<bool> in_event;  // an event member in our Link's event (InSameEvent)
        };
        std::vector<TalkScrub> talk_scrubs;
        std::unordered_map<u64, std::pair<std::string, u64>> names;
        std::unordered_map<u64, ActorStamp> stamps;
        std::unordered_map<u64, u64> life_components;
        {
            // Difficulty: the extra enemies the director made so far (coop_difficulty.h Extras).
            std::map<u64, u64> live;
            for (const auto& [key, actor] : CoopSpawn::TheDirector().Live()) {
                if ((key & kExtraKey) == kExtraKey) {
                    live[key & ~kExtraKey] = actor;
                }
            }
            extras_.Keep(actors);  // (a whole walk: List) one whose actor is gone frees its address
            extras_.Bind(live);
            std::map<u64, u64> horde;
            for (const auto& [key, actor] : CoopSpawn::TheDirector().Live()) {
                if ((key & (kExtraKey | kHordeKey)) == kHordeKey) {
                    horde[key & ~kHordeKey] = actor;
                }
            }
            horde_.Keep(actors);
            horde_.Bind(horde);
        }
        const u64 now_ms = SteadyNowMs();
        for (const u64 a : actors) {
            // Row 172 (run 053934): a scene change can put a new actor at an address an old one held without a
            // walk between (our adult Link where the last scene's TerrainScenePermanent was): the cached name
            // would stay the old one, and WriteLife found no Player (another player's kill not replayed). The
            // cache holds only while the actor's name (pointer and first characters) and placement are the ones it
            // was read with (the vtable is one generic class for every actor, main+0x4b25e18: no help).
            const u64 name_at = r.Ptr(a + kActorName).value_or(0);
            const ActorStamp stamp{name_at, name_at ? r.Get<u64>(name_at).value_or(0) : 0,
                                   r.Ptr(a + kActorPlacement).value_or(0)};
            stamps.emplace(a, stamp);
            auto it = names_.find(a);
            const auto was = stamps_.find(a);
            std::pair<std::string, u64> known;
            if (it != names_.end() && was != stamps_.end() && was->second == stamp) {
                known = std::move(it->second);
            } else {
                known.first = r.Name(a);
                const auto placement = r.Ptr(a + kActorPlacement);
                known.second = placement ? r.Get<u64>(*placement + kPlacementHash).value_or(0) : 0;
                CoopPickups::Note(known.first, known.second);  // one-time pickups (docs/items.md)
                if (known.first == "NormalArrow") {  // test only (`aim`): a new arrow of ours
                    if (const auto at = r.Get<std::array<float, 3>>(a + kActorPosition)) {
                        TheMover().AimArrow(a, *at);
                    }
                }
                if (IsShared(known.first) && vtables_logged_.insert(known.first).second) {
                    // The set-matrix node (0x2622e50) writes +0x31c..+0x34b and
                    // then calls vtable slot 0x218 (actor, 0): what does it do?
                    const u64 vt = r.Get<u64>(a).value_or(0);
                    const u64 slot = vt ? r.Get<u64>(vt + 0x218).value_or(0) : 0;
                    LOG_INFO(Core, "coop: objects: share vtable {} main+{:#x}, slot 0x218 main+{:#x}",
                             known.first, vt - main_, slot - main_);
                }
            }
            if (const auto extra = extras_.HashOf(a, known.first)) {
                known.second = *extra;  // difficulty: an extra enemy is shared by its own placement id
            }
            member_log(a, known.first);  // G1: the game's own event member test, for the log
            // Difficulty: a made actor that may be an extra the director has not bound yet waits for it.
            const bool extra_unbound = (known.second == 0 || known.second == ~u64{0}) &&
                                       extras_.Holds(known.first, now_ms);
            if (CoopDifficulty::IsEnemy(known.first, false)) {
                // Down but not out (CoopDbno::NoteEnemiesNear): where the living enemies stand.
                if (const auto value = LifeValue(r, a); value && r.Get<s32>(value->first + 8).value_or(0) > 0) {
                    coop::protocol::Vec3 at{};
                    for (std::size_t i = 0; i < 3; ++i) {
                        at[i] = r.Get<float>(a + kActorPosition + 4 * i).value_or(0.0f);
                    }
                    foes.push_back(at);
                }
            }
            if (CoopDifficulty::IsEnemy(known.first, CoopEnemyShots::IsEnemyShot(known.first))) {
                // Difficulty: an attacker (a boss, one with its copies' weight folded in, or plain).
                // A no-spot enemy folds by its own missing extras (FoldCount), keyed by its placement.
                const bool no_spot = extras_.NoSpot(known.second);
                CoopDifficulty::TheKinds().Mark(a, CoopDifficulty::AttackerKind(known.first, known.second, no_spot),
                                                no_spot ? known.second : 0);
            } else {
                CoopDifficulty::TheKinds().Forget(a);  // difficulty: a reused address (our bomb where a Keese was)
            }
            if (known.first.starts_with("Player_")) {  // our own Link, for coop_world
                if (known.first == "Player_Child" || known.first == "Player_Adult") {
                    our_link_ = a;  // the ocarina probe (issue 152)
                    TheLinkActor().store(a);  // the ocarina's equip (rows 93/103, OurLinkActor)
                    {
                        const u64 now = SteadyNowMs();
                        TheLinkActorAt().store(now == 0 ? 1 : now);
                    }
                    // Difficulty: the Life component whose hits from enemies are scaled.
                    if (const auto table = r.Ptr(a + kActorComponents)) {
                        if (const auto component = r.Ptr(*table + kComponentLife)) {
                            CoopDifficulty::TheKinds().Mark(*component, CoopDifficulty::Kind::our_link_life);
                        }
                    }
                    // Our Link's own sound user, whose sounds go to the others (coop_link_sound.h).
                    const u64 parts = r.Get<u64>(a + CoopPuppetSound::addr::kActorParts).value_or(0);
                    const u64 xlink = parts ? r.Get<u64>(parts + CoopPuppetSound::addr::kPartsXLink).value_or(0) : 0;
                    CoopLinkSound::SetOurUser(
                        xlink ? r.Get<u64>(xlink + CoopPuppetSound::addr::kXLinkSoundUser).value_or(0) : 0);
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
                        GivePvp(r, a, value->first + 8, life);  // PvP (coop_pvp_game.cpp)
                        ReadPvpCopies(r, a);  // PvP on real bodies (coop_pvp_native_game.cpp)
                    }
                    if (known.first == "Player_Child" || known.first == "Player_Adult") {
                        GiveDbno(r, a, value->first + 8, life);  // down but not out (coop_dbno_game.h)
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
                    // Rows 136/137: the host's older save lowered MaxLife; row 181: a second pickup of a boss's one
                    // container was put back. The component follows it (never in the frame it was raised).
                    const s32 lower = TheMaxLifeLowerTo().load();
                    const auto lowered =
                        raised ? std::nullopt
                               : LoweredMaxLife(max, life, lower >= 0 ? std::optional<std::int32_t>(lower) : std::nullopt);
                    if (lowered && memory_ != nullptr && memory_->IsValidVirtualAddressRange(value->second + 8, 4) &&
                        memory_->IsValidVirtualAddressRange(value->first + 8, 4)) {
                        if (lowered->life) {
                            memory_->Write32(value->first + 8, static_cast<u32>(*lowered->life));
                        }
                        memory_->Write32(value->second + 8, static_cast<u32>(lowered->max));
                        LOG_INFO(Core, "coop: objects: our Link's max life {} -> {} (MaxLife is held lower: the host "
                                       "loaded a save with fewer hearts, or a boss's one container was taken twice){}",
                                 max, lowered->max,
                                 lowered->life ? fmt::format(", life {} -> {}", life, *lowered->life) : "");
                    }
                }
            }
            if (CoopScrubTalk::FindNearScrub(known.second) != nullptr ||
                CoopScrubSale::FindSeller(known.second) != nullptr) {
                TalkScrub t;  // how near our Link (after the walk), and whether an event of ours binds it
                t.hash = known.second;
                for (std::size_t i = 0; i < 3; ++i) {
                    t.at[i] = r.Get<float>(a + kActorPosition + 4 * i).value_or(0.0f);
                }
                // Read whatever event_running says: it has not been seen during a scrub's event.
                t.in_event = member_of(a, known.first);  // bound into our running event (row 97)
                talk_scrubs.push_back(t);
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
            if (platforms_mode_ != 0 && CoopPlatforms::IsDriver(known.first)) {
                CoopPlatforms::PlatformSync::Driver d;  // moving platforms (coop_platforms.h)
                d.address = a;
                d.hash = known.second;
                d.name = known.first;
                for (std::size_t i = 0; i < 3; ++i) {
                    d.position[i] = r.Get<float>(a + kActorPosition + 4 * i).value_or(0.0f);
                }
                platform_drivers_.push_back(std::move(d));
            }
            if (switch_share_mode_ != 0 && CoopRoomObjects::SharedSwitchName(known.first)) {
                ShareSwitch s;  // room-objects: a timed switch some room object reads
                s.address = a;
                s.name = known.first;
                for (std::size_t i = 0; i < 3; ++i) {
                    s.position[i] = r.Get<float>(a + kActorPosition + 4 * i).value_or(0.0f);
                }
                share_switches_.push_back(std::move(s));
            }
            if (lift_ride_on_ && CoopLiftRide::IsRideLift(known.first)) {
                CoopLiftRide::Lift l;
                l.address = a;
                l.name = known.first;
                for (std::size_t i = 0; i < 3; ++i) {
                    l.position[i] = r.Get<float>(a + kActorPosition + 4 * i).value_or(0.0f);
                }
                ride_lifts_.push_back(std::move(l));
            }
            if (((gimmick_probe_on_ || logic_probe_on_) && CoopWorld::IsGimmick(known.first)) ||
                (logic_probe_on_ && (IsRoomAsker(known.first) || IsProbeSwitch(known.first)))) {  // the asker's trigger
                CoopWorld::GimmickProbe::Gimmick g;
                g.address = a;
                g.hash = known.second;
                g.name = known.first;
                for (std::size_t i = 0; i < 3; ++i) {
                    g.position[i] = r.Get<float>(a + kActorPosition + 4 * i).value_or(0.0f);
                }
                gimmicks_.push_back(std::move(g));
            }
            if (!extra_unbound &&
                (IsShared(known.first) || CoopWorld::IsWorldObject(known.first) ||
                 CoopWorld::IsBlockAnchor(known.first) ||  // moved with its block (issue 147)
                 (known.second != 0 && known.second != ~u64{0} && sync_.Taken(known.second)))) {
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
                // Townspeople and enemies send the playing frame (PlaysByFrame).
                s.animation = PlaysByFrame(s.name)
                                  ? AnimationAt(r, a, 0, false, CoopNpcAnim::kSlotPlayFrame)
                                  : Animation(r, a);
                // Its life, read between two reads of its hit id (NewHits).
                const auto life_value = LifeValue(r, a);
                std::optional<s32> life_read;
                std::optional<s32> max_read;
                const std::optional<u64> own_hit_life = NewHits(r, a, s, [&] {
                    if (life_value) {
                        life_read = r.Get<s32>(life_value->first + 8);
                        max_read = r.Get<s32>(life_value->second + 8);
                    }
                });
                // Row 138 (2)(7): whether our own hit took life (its life the walk before, and now).
                bool own_hit_took_life = false;
                {
                    const auto last = hit_lives_.find(a);
                    own_hit_took_life = own_hit_life.has_value() &&
                                        CoopBossNative::OwnHitTookLife(
                                            last != hit_lives_.end() ? std::optional<s32>(last->second) : std::nullopt,
                                            life_read);
                    if (life_read) {
                        hit_lives_[a] = *life_read;
                    } else if (last != hit_lives_.end()) {
                        hit_lives_.erase(last);
                    }
                }
                // One model for every enemy (fable-g4-enemies), off by default until its game run:
                // LICHII_COOP_ENEMY_BRAIN=1 puts every plain Enemy_ on the bosses' native path (the owner's brain
                // and action verbs through the game's own two functions, the copy's own brain's changes skipped),
                // so a copy's own AI no longer moves it against the owner's place. A plain one's Life is not held
                // and its death, own hit and own wake run its own brain (CoopBossNative, Boss::plain). Its
                // animation slots and its samples' hit place stay as a plain enemy's.
                static const bool enemy_brain = [] {
                    const bool on = EnemyBrainSwitch(EnvText("LICHII_COOP_ENEMY_BRAIN"));  // opt-in
                    LOG_INFO(Core, "coop: objects: enemies' brain path {} (LICHII_COOP_ENEMY_BRAIN)",
                             on ? "on: plain enemies take the owner's verbs" : "off: plain copies are posed");
                    return on;
                }();
                if (enemy_brain && OnBrainPath(s.name)) {
                    if (const auto table = r.Ptr(a + kActorComponents)) {
                        s.bsa_component = r.Ptr(*table + CoopBossNative::addr::kComponentBsa).value_or(0);
                    }
                    if (s.bsa_component != 0) {
                        s.bsa = CoopBossNative::TheTable().OwnerState(a);
                        if (own_hit_life) {
                            (void)NewOwnHitAt(r, a, *own_hit_life, s.position, own_hit_took_life);
                        }  // our own hit: its own reaction runs (NoteOwnHit) unless it took life (NoteOwnDamage)
                    }
                }
                if (IsBossOrPart(s.name) || OnBrainPath(s.name)) {
                    // Boss turns (0.11.8, server/boss_turns.hpp): its attack as its own game says (the attributes of
                    // its current action verbs, Attack), sent in our samples so the server hands it on between attacks.
                    if (const auto table = r.Ptr(a + kActorComponents)) {
                        if (const auto bsa = r.Ptr(*table + CoopBossNative::addr::kComponentBsa); bsa && *bsa != 0) {
                            namespace ba = CoopBossNative::addr;
                            std::uint64_t bits = 0;
                            std::array<s32, ba::kBsaActionSlots> verbs{};
                            bool read = true;
                            for (std::size_t slot = 0; slot < ba::kBsaActionSlots && read; ++slot) {
                                const auto verb = r.Get<s32>(ba::ActionVerbAt(*bsa, slot));
                                const auto word = verb ? r.Get<u64>(ba::ActionAttributesAt(*bsa, *verb)) : std::nullopt;
                                read = verb && word;
                                if (read) {
                                    verbs[slot] = *verb;
                                    bits |= *word;
                                }
                            }
                            if (read) {
                                s.attacking = ba::IsAttack(bits);
                                NoteAttackEdge(a, s.name, *s.attacking, verbs, bits);
                            }
                        }
                    }
                }
                if (IsBossOrPart(s.name)) {
                    s.hit_ours.reset();  // issue 133: whose hit is for plain enemies (bosses: NewOwnHitAt, their sync)
                    s.slots = OtherSlots(r, a);  // issue 150
                    // Native boss sync (coop_boss_native.h): its BSA
                    // component, and our game's verbs for it when it is ours.
                    if (const auto table = r.Ptr(a + kActorComponents)) {
                        s.bsa_component = r.Ptr(*table + CoopBossNative::addr::kComponentBsa).value_or(0);
                    }
                    s.bsa = CoopBossNative::TheTable().OwnerState(a);
                    if (own_hit_life) {
                        s.hit_at = NewOwnHitAt(r, a, *own_hit_life, s.position, own_hit_took_life);
                    } else if (CoopBossNative::TheTable().OwnShotNear(s.position)) {
                        // Issue 150: our shot's weak-point hit that took no life (no hit id moved):
                        // a copy runs its own reaction, the stun (the boss's own AI; NoteOwnShotEnd).
                        CoopBossNative::TheTable().NoteOwnHit(a, true);
                    }
                }
                if (const auto value = life_value) {
                    const auto life = life_read;
                    const auto max = max_read;
                    if (life && max) {
                        s.life = coop::protocol::ObjectLife{*life, *max};
                        const auto table = r.Ptr(a + kActorComponents);
                        if (const auto component = table ? r.Ptr(*table + kComponentLife) : std::nullopt) {
                            life_components.emplace(*component, a);  // (whose Life step is whose: StepHits)
                            s.life_component = *component;
                            if (CoopDifficulty::IsEnemy(s.name, false)) {  // difficulty: its hits are scaled
                                const bool no_spot = extras_.NoSpot(s.hash);
                                CoopDifficulty::TheKinds().Mark(*component,
                                                                CoopDifficulty::LifeKind(s.name, s.hash, no_spot),
                                                                no_spot ? s.hash : 0);
                            } else {
                                CoopDifficulty::TheKinds().Forget(*component);  // (a reused address)
                            }
                        }
                    }
                }
                // Row 97: whether our running event binds it (coop_event_member.h). Read only
                // while our Link is in an event; the Sync claims such an actor at any distance.
                if (TheLocalEvent().load()) {
                    s.event_member = member_of(a, s.name);
                }
                // Row 138(8) (coop_sensor_claim.h): whether our Link is in its area by its own sensor input
                // (Jabu-Jabu's mouth). Read only; a placement whose live logic has none of the inputs is
                // not read again.
                if (CoopSensorClaim::Kind(s.name)) {
                    const auto none = no_area_.find(a);
                    if (none == no_area_.end() || none->second != s.hash) {
                        if (const auto plugs = CoopLogicProbe::Read([&r](u64 x) { return r.Get<u64>(x); }, a)) {
                            s.sensor_in_area = CoopSensorClaim::InArea(*plugs);
                            if (!s.sensor_in_area) {
                                if (no_area_.size() >= kNoAreaMax) no_area_.clear();
                                no_area_.insert_or_assign(a, s.hash);
                            }
                        }
                    }
                }
                out.push_back(std::move(s));
            }
            names.emplace(a, std::move(known));
        }
        names_ = std::move(names);
        stamps_ = std::move(stamps);
        life_components_ = std::move(life_components);
        CoopDbno::NoteEnemiesNear(player && CoopDbno::AnyNear(*player, foes));
        sync_.SetPlayer(player);
        sync_.SetLures(CoopProjectiles::TheLures().Get());  // issue 151: our cast lures
        player_ = player;
        {  // issue 144 / scrub sales: a scrub near our Link or bound into our event holds its flags
            const std::uint64_t now_us = CoopEvery::NowUs();
            for (const TalkScrub& t : talk_scrubs) {
                float distance = std::numeric_limits<float>::infinity();  // no player: only the event counts
                if (player) {
                    float d2 = 0.0f;
                    for (std::size_t i = 0; i < 3; ++i) {
                        const float d = t.at[i] - (*player)[i];
                        d2 += d * d;
                    }
                    distance = std::sqrt(d2);
                }
                CoopScrubTalk::TheNear().See(t.hash, distance, now_us, t.in_event);
                CoopScrubSale::TheBusy().See(t.hash, distance, now_us, t.in_event);
            }
        }
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
                                                         bool empty_ok,
                                                         u64 frame_at = kAsSlotFrame) {
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
        const float frame = r.Get<float>(*slot + frame_at).value_or(0.0f);
        const float rate = r.Get<float>(*slot + kAsSlotRate).value_or(1.0f);
        if (!std::isfinite(frame) || !std::isfinite(rate) || std::fabs(frame) > 1e5f ||
            std::fabs(rate) > 100.0f) {
            return std::nullopt;
        }
        return coop::protocol::Animation{std::string(chunk->data(), n), frame, rate};
    }

    // The life and max value entries ([life + 0x1c10], [life + 0x1c18]).
    // Where a boss's incoming hit our game took since the last walk landed: its Life component's last hit id
    // moved (+0x1b18), not to one we replayed, and the record's hit place (+0x240 + 0x3c, kHitTemplate's
    // "the hit's place") is finite and within kHitNearBoss of the boss. Only that one named field leaves this
    // game (DAMAGE's hit_at); the owner's game builds the rest of the hit itself (boss-intro).
    static constexpr float kHitNearBoss = 12.0f;
    static constexpr u64 kHitPlace = 0x27c;
    // Issue 133 (whose hit): our game's own hit's attacker, the actor at the start of its Life component's hit record
    // (+0x240): ours when our Link or one of our own projectiles made it (Seen::hit_ours).
    static constexpr u64 kHitAttacker = kHitRecord;
    std::unordered_map<u64, u32> boss_hit_ids_;  // actor: the last hit id seen (every actor with a Life component)
    std::unordered_map<u64, s32> hit_lives_;     // actor: its life the walk before (OwnHitTookLife), same actors
    // Whose hits reached this actor since the last walk (Seen::own_hit, Seen::replay_hit): its Life component's last
    // hit id moved (hit ids only grow); to one we did not replay is our game's own hit. Returns its Life component
    // when our game's own hit came (for a boss's hit place).
    //
    // `read_life` reads the actor's life. It runs between two reads of the hit id: the game's Life step sets both on
    // the calc thread while this walk reads them one after the other, so a step landing between the reads would
    // show its id and its fall in different walks (review of 367e23f3: a replay's fall booked as harm on our Link
    // and its PvP hit dealt again; on a copy, our own hit's fall booked as the replay landing). When the id moved
    // between the reads, the life is read again after it, so the id and the life read are of the same step.
    template <typename ReadLife>
    std::optional<u64> NewHits(Reader& r, u64 a, Seen& s, ReadLife&& read_life) {
        const auto table = r.Ptr(a + kActorComponents);
        const auto life = table ? r.Ptr(*table + kComponentLife) : std::nullopt;
        const auto first = life ? r.Get<u32>(*life + kLastHitId) : std::nullopt;
        read_life();
        const auto id = life ? r.Get<u32>(*life + kLastHitId) : std::nullopt;
        if (first && id && *first != *id) {
            read_life();  // (a step landed between the reads: its life, after its id)
        }
        if (!id) {
            return std::nullopt;
        }
        const auto [it, fresh] = boss_hit_ids_.try_emplace(a, *id);
        s.hit_id_read = !fresh;
        if (fresh || it->second == *id) {
            return std::nullopt;
        }
        const u32 before = std::exchange(it->second, *id);
        const u32 ours = TheMover().ReplayedId(*life);
        // (our replay inside the move: before < ours <= now, in the ids' own wrap-around order)
        s.replay_hit = ours != 0xffffffffu && static_cast<s32>(ours - before) > 0 && static_cast<s32>(*id - ours) >= 0;
        // Protocol 14 (every hit shows on every screen): the hit its damage step took, as it took it (our own on
        // a copy goes with our DAMAGE; any on our own enemy, replays too, goes in its samples for the copies).
        s.hit_record = ReadHitRecord(r, *life);
        if (*id == ours) {
            return std::nullopt;  // our replay of the owner's hit, not one of our game's
        }
        s.own_hit = true;
        const u64 attacker = r.Get<u64>(*life + kHitAttacker).value_or(0);
        s.hit_ours = CoopProjectiles::HitIsOurs(attacker, our_link_, CoopProjectiles::TheOwnShots(), CoopEvery::NowUs());
        return *life;
    }
    // The incoming hit record its Life component holds (+0x240, 38 words), with the attacker pointer and the
    // present flag cleared: those are this game's own, the receiver puts its own in (CoopHitReplay::Write).
    static std::optional<coop::protocol::HitRecord> ReadHitRecord(Reader& r, u64 life) {
        auto rec = r.Get<coop::protocol::HitRecord>(life + kHitRecord);
        if (rec) {
            (*rec)[0] = (*rec)[1] = 0;
            (*rec)[3] = 0;
        }
        return rec;
    }
    std::optional<coop::protocol::Vec3> NewOwnHitAt(Reader& r, u64 a, u64 life, const coop::protocol::Vec3& boss,
                                                    bool took_life) {
        // Our game's own hit. One that took no life: a copy boss runs its own reaction (the stun; boss-fix). One
        // that took life is the owner's to judge (its DAMAGE), whose reaction comes as its verbs (row 138 (2)(7)).
        if (took_life) {
            CoopBossNative::TheTable().NoteOwnDamage(a);
        } else {
            CoopBossNative::TheTable().NoteOwnHit(a);
        }
        const auto at = r.Get<std::array<float, 3>>(life + kHitPlace);
        if (!at) {
            return std::nullopt;
        }
        return CoopObjects::CheckedHitPlace(*at, boss, kHitNearBoss);
    }

    // Row 178: the actor's own "takes no hit now" state (CoopBossNative::HitGuard): its Life component's
    // invincibility counts and flags, and its no-hit map (each rigid body: in no-hit mode or not).
    CoopBossNative::HitGuard ReadHitGuard(Reader& r, u64 a) {
        namespace ba = CoopBossNative::addr;
        CoopBossNative::HitGuard g;
        const auto table = r.Ptr(a + kActorComponents);
        const auto life = table ? r.Ptr(*table + ba::kComponentLife) : std::nullopt;
        if (!life) {
            return g;
        }
        const auto names = r.Get<s32>(*life + ba::kLifeInvincibleNames);
        const auto other = r.Get<s32>(*life + ba::kLifeInvincibleOther);
        const auto flags = r.Get<u32>(*life + ba::kLifeFlags);
        if (!names || !other || !flags) {
            return g;
        }
        g.read = true;
        g.invincible_names = *names;
        g.invincible_other = *other;
        g.life_flags = *flags;
        const auto map = r.Ptr(*table + ba::kComponentNoHit);
        const auto keys = map ? r.Ptr(*map + ba::kNoHitKeys) : std::nullopt;
        const auto values = map ? r.Ptr(*map + ba::kNoHitValues) : std::nullopt;
        const s32 slots = map ? r.Get<s32>(*map + ba::kNoHitSlots).value_or(0) : 0;
        if (!keys || !values || slots <= 0 || slots > ba::kNoHitMaxSlots) {
            return g;
        }
        for (s32 i = 0; i < slots; ++i) {
            const auto key = r.Get<u64>(*keys + 8 * static_cast<u64>(i));
            const auto value = r.Get<u32>(*values + 4 * static_cast<u64>(i));
            if (!key || !value) {
                g.bodies = 0;  // (a part unread: nothing known of the bodies)
                g.no_hit_bodies = 0;
                return g;
            }
            if (*key != 0) {
                ++g.bodies;
                g.no_hit_bodies += *value != 0 ? 1 : 0;
            }
        }
        return g;
    }

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

    // Native puppets phase 4 (docs/native-puppets.md 5.4): each live _Pvp copy's Life component, read once a
    // walk. A last hit id that moved to one that is not our own third-game replay is our game's own hit on that
    // player: its record goes to the glue, which sends the strike when it is ours (our Link, or a shot we made).
    void ReadPvpCopies(Reader& r, u64 link) {
        if (!CoopPvpNativeGame::On() || CoopPuppet::PvpCopies() == 0) {
            pvp_copy_watch_.Reset();
            return;
        }
        static const bool shield_caller = [] {
            CoopSvc::AddCaller(kPvpShieldCallerOrder, [](CoopPuppet::Guest& guest, bool after_call,
                                                         std::uint64_t result, std::uint64_t /*now_ms*/) {
                return ThePvpShield().Next(guest, after_call, result, TheBossMain().load());
            });
            return true;
        }();
        (void)shield_caller;
        for (std::uint8_t n = 1; n <= coop::protocol::kMaxPlayers; ++n) {
            const u64 actor = CoopPuppet::TheDirector().LivePvpActor(n);
            if (actor == 0) {
                pvp_copy_watch_.Forget(n);
                continue;
            }
            const auto table = r.Ptr(actor + kActorComponents);
            const auto life = table ? r.Ptr(*table + kComponentLife) : std::nullopt;
            const auto id = life ? r.Get<u32>(*life + kLastHitId) : std::nullopt;
            if (!id || !pvp_copy_watch_.Moved(n, actor, *id, TheMover().ReplayedId(*life))) {
                continue;
            }
            const auto rec = r.Get<coop::protocol::HitRecord>(*life + kHitRecord);
            if (!rec) {
                continue;
            }
            CoopPvpNative::CopyHit hit;
            hit.victim = n;
            const u64 attacker = r.Get<u64>(*life + kHitAttacker).value_or(0);
            hit.from.our_link = attacker != 0 && attacker == link;
            hit.own_shot = !hit.from.our_link && CoopProjectiles::HitIsOurs(attacker, link,
                                                                            CoopProjectiles::TheOwnShots(),
                                                                            CoopEvery::NowUs());
            for (std::size_t i = 0; i < hit.kind.size(); ++i) {
                hit.kind[i] = (*rec)[CoopHitReplay::kRecKind + i];
            }
            std::memcpy(hit.at.data(), &(*rec)[CoopHitReplay::kRecPlace], 12);
            std::memcpy(hit.attacker_at.data(), &(*rec)[CoopHitReplay::kRecFrom], 12);
            hit.amount = r.Get<s32>(*life + kLifeHurt).value_or(0);
            // U2/U7 probe (docs 5.8): the whole record, to find the AttackParam row (its AttackType hash) or the
            // kind words' meaning per attack. Our own hits only (an enemy's is not logged).
            if (hit.from.our_link || hit.own_shot) {
                std::string words;
                for (const u32 w : *rec) {
                    words += fmt::format("{:08x} ", w);
                }
                LOG_INFO(Core, "coop: pvp native: U2 record of our hit on P{}'s copy (id {:#x}, +0x228 {}): {}", n, *id,
                         hit.amount, words);
            }
            CoopPvpNativeGame::NoteCopyHit(hit);
        }
    }

    // Down but not out (coop_dbno_game.h): our Link's Life component for the
    // Life step's guard, and the life the rules ask for (a revive, a downed
    // Link's quarter heart, out), written as PvP's fallback does.
    void GiveDbno(Reader& r, u64 link, u64 life_at, s32 life) {
        const auto table = r.Ptr(link + kActorComponents);
        const auto component = table ? r.Ptr(*table + kComponentLife) : std::nullopt;
        CoopDbno::SetOurLifeComponent(component.value_or(0));
        // Our downed Link's crawl, shield and unseen (coop_dbno_body.h): its BSA and PerceptionEmitter, and the
        // caller that asks the game.
        const auto bsa = table ? r.Ptr(*table + CoopDbno::Body::addr::kComponentBsa) : std::nullopt;
        const auto emitter = table ? r.Ptr(*table + CoopDbno::Body::addr::kComponentEmitter) : std::nullopt;
        CoopDbno::SetOurComponents(bsa.value_or(0), emitter.value_or(0));
        static const bool body_caller = [] {
            // A downed teammate's puppet (issue 150): its actor, from the puppet director's slots (the same
            // calc-thread caller chain, so its slots are not changing meanwhile).
            CoopDbno::Body::TheCrawl().SetPuppetActor([](std::uint8_t number) -> std::uint64_t {
                for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
                    if (slot.number == number && slot.phase == CoopPuppet::Phase::live) {
                        return slot.actor;
                    }
                }
                return 0;
            });
            CoopSvc::AddCaller(kDbnoBodyCallerOrder, [](CoopPuppet::Guest& guest, bool after_call,
                                                        std::uint64_t result, std::uint64_t now_ms) {
                return CoopDbno::Body::TheCrawl().Next(guest, after_call, result, now_ms, TheBossMain().load());
            });
            return true;
        }();
        (void)body_caller;
        const auto want = CoopDbno::TakeLifeWrite();
        if (!want || life < 0 || *want == life || memory_ == nullptr ||
            !memory_->IsValidVirtualAddressRange(life_at, 4)) {
            return;
        }
        memory_->Write32(life_at, static_cast<u32>(*want));
        TheLinkLife().store(*want);
        LOG_INFO(Core, "coop: dbno: our Link's life {} -> {}", life, *want);
    }

    // PvP: our Link's life `life` (at `life_at`) was just read. Notes it for
    // OurLinkLife, and gives the hurts the victim's rules decided: first as
    // the game's own hit (the hit replay, as an enemy copy gets one: Link
    // flinches and is knocked away from the attacker), then, if the game has
    // not taken it within kPvpHitWaitMs, by writing the life directly (as
    // docs/objects.md "Damage" found enough for any actor).
    void GivePvp(Reader& r, u64 link, u64 life_at, s32 life) {
        const u64 now = SteadyNowMs();
        TheLinkLife().store(life);
        TheLinkLifeAt().store(now == 0 ? 1 : now);
        if (life < 0) {
            return;  // not read: nothing is given until it is
        }
        // A monster's hit, a fall, fire or a heart since the last read is not the PvP hit: the pending hurts'
        // expected lives move with it; a fall and its hit id read a walk apart are paired (CoopPvp::LinkLifeWatch).
        Seen hits;
        NewHits(r, link, hits, [&] {
            if (const auto now_life = r.Get<s32>(life_at)) {
                life = *now_life;  // (of the same step as the id: NewHits)
            }
        });
        TheLinkLife().store(life);
        const bool hold = pvp_watch_.Follow(pvp_, pvp_life_, life, hits.own_hit, hits.replay_hit, hits.hit_id_read);
        pvp_life_ = life;
        // A hold read takes no new asks (they wait in PvpAsks for the next read): the held fall is booked next read
        // against the hurts waiting now only, and `life` here already has it in it (review-a38-4).
        std::vector<PvpAsk> asks;
        if (!hold) {
            std::lock_guard lock(PvpMutex());
            asks.swap(PvpAsks());
        }
        for (const PvpAsk& ask : asks) {
            if (ask.native && !(replay_ && pvp_replay_)) {
                LOG_INFO(Core, "coop: pvp native: hit of {} not given: the hit replay is off (never written)",
                         ask.take);
                continue;
            }
            if (ask.native && ask.take == 0) {
                // A no-life kind (a nut's, the boomerang's stun): the game's hit, nothing taken, no life to wait for.
                Mover::Hit hit;
                hit.actor = link;
                hit.attacker = link;
                hit.at = ask.at;
                hit.from = ask.from;
                hit.exact = true;
                hit.record = ask.record;
                TheMover().AddHit(hit);
                LOG_INFO(Core, "coop: pvp native: our Link takes a no-life hit at life {}", life);
                continue;
            }
            CoopPvp::PendingGive h;
            h.take = ask.take;
            h.write = !ask.native;
            h.expect = CoopPvp::NextExpect(pvp_, life, ask.take);
            h.asked_ms = now;
            if (replay_ && pvp_replay_) {
                Mover::Hit hit;
                hit.actor = link;
                hit.attacker = link;  // the attacker's puppet is another game's; our Link stands in
                hit.amount = ask.take;
                hit.at = ask.at;
                hit.from = ask.from;
                hit.exact = true;
                hit.record = ask.record;  // phase 4: the strike's kind (CoopHitReplay::WithHitKind)
                TheMover().AddHit(hit);
                h.replayed = true;
            }
            LOG_INFO(Core, "coop: pvp: our Link hurt by {} at life {} ({})", ask.take, life,
                     h.replayed ? "the game's hit" : "written");
            pvp_.push_back(h);
        }
        for (auto it = pvp_.begin(); !hold && it != pvp_.end();) {
            if (life <= it->expect) {
                if (it->replayed) {
                    LOG_INFO(Core, "coop: pvp: the game took the hit (life {})", life);
                }
                it = pvp_.erase(it);
                continue;
            }
            if ((!it->replayed || now >= it->asked_ms + kPvpHitWaitMs) && !it->write) {
                // Phase 4, path B: the game's own hit only (5.5); one the game did not take stays not taken.
                LOG_INFO(Core, "coop: pvp native: the game did not take the hit of {} (life {}); nothing written",
                         it->take, life);
                it = pvp_.erase(it);
                continue;
            }
            if (!it->replayed || now >= it->asked_ms + kPvpHitWaitMs) {
                // Down but not out (player report 34): the write takes what the Life step would. The
                // game's own hit may have been taken already and held back by the guard (life 1: downed),
                // and a downed Link takes nothing; writing `expect` past it killed a downed Link.
                const s32 to = CoopDbno::GuardedWrite(CoopDbno::TheGuard(), life, it->expect);
                if (to >= life) {
                    LOG_INFO(Core, "coop: pvp: our Link's life stays {}: down but not out holds the hit back",
                             life);
                } else if (memory_ != nullptr && memory_->IsValidVirtualAddressRange(life_at, 4)) {
                    memory_->Write32(life_at, static_cast<u32>(to));
                    LOG_INFO(Core, "coop: pvp: our Link's life {} -> {}{}", life, to,
                             it->replayed ? " (the game's hit was not taken)" : "");
                    life = to;
                    pvp_life_ = to;
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
    // Difficulty (coop_difficulty.h Extras): the extra enemies of this scene
    // visit, planned from the walk's placed enemies at the count in force, go
    // on the director's enemy list as the game's own actors, made once each.
    // The enemies the sync treats in its own way get no extras (by name, so the placement data can say too).
    static bool SpecialForExtras(std::string_view name) {
        return IsRolled(name) || IsHatchingEgg(name) || IsHatchling(name) || IsAnnounced(name) || IsOwnPuzzle(name) ||
               IsFish(name);
    }
    void PlanExtras(const std::string& scene, const std::vector<Seen>& seen, std::vector<CoopSpawn::Copy>& enemies) {
        std::vector<CoopDifficulty::Extras::Original> originals;
        bool boss = false;
        for (const Seen& s : seen) {
            boss = boss || IsBossOrPart(s.name);
            const bool special = SpecialForExtras(s.name) ||
                                 extras_.HashOf(s.address, s.name).has_value();  // an extra gets none of its own
            if (CoopDifficulty::GetsExtras(s.name, s.hash, s.life && s.life->max > 0, special)) {
                originals.push_back({s.name, s.hash});
            }
        }
        const std::uint16_t count = CoopDifficulty::Now().count_pct;
        const std::size_t no_spot = extras_.NoSpots();
        if (const std::size_t added = extras_.Plan(scene, originals, count, boss, SteadyNowMs())) {
            LOG_INFO(Core, "coop: difficulty: {} extra enemies to make in {} (enemies {}%, {} in this visit)", added,
                     scene, count, extras_.All().size());
        }
        if (extras_.NoSpots() > no_spot) {
            LOG_INFO(Core, "coop: difficulty: {} more placed enemies in {} with no ground for copies: weight instead",
                     extras_.NoSpots() - no_spot, scene);
        }
        for (const auto& e : extras_.All()) {
            CoopSpawn::Copy c;
            c.key = kExtraKey | e.key;
            c.actor = e.name;
            c.position = {e.position[0], e.position[1], e.position[2]};
            // Once made and bound, the director lets it go (its slot is free for
            // the shots and drops); the actor stays, the sync's like any enemy.
            c.act = e.address != 0 ? CoopSpawn::Act::release : CoopSpawn::Act::show;
            c.place = false;  // its own AI moves it; the sync places it where it is a copy
            c.remakes = 0;    // one that went is not made again
            c.real = true;    // the game's own enemy, never a stand-in
            enemies.push_back(std::move(c));
        }
    }

    // Difficulty (coop_difficulty.h Horde): more night Stalchild beside the
    // rolls this game keeps and shares, the more players stand near each.
    void PlanHorde(const std::string& scene, std::uint8_t self, u64 server_now_us, const std::vector<Seen>& seen,
                   std::vector<CoopSpawn::Copy>& enemies) {
        std::vector<CoopDifficulty::Horde::Roll> rolls;
        // Ours rise where the game's rolls rose (CoopDifficulty::FindSpot), apart from every enemy here and our Link.
        std::vector<CoopDifficulty::Point> apart;
        if (player_) {
            apart.push_back({(*player_)[0], (*player_)[1], (*player_)[2]});
        }
        std::size_t alive = 0;
        for (const Seen& s : seen) {
            if (CoopDifficulty::IsEnemy(s.name, false)) {
                apart.push_back({s.position[0], s.position[1], s.position[2]});
            }
            if (!IsRolled(s.name) || s.name == "Enemy_Poe" || (s.hash != 0 && s.hash != ~u64{0})) {
                continue;  // (the horde is Stalchild only)
            }
            ++alive;
            const auto id = sync_.IdOf(s.address);
            if (!id || id->kind != coop::protocol::ObjectKind::made || id->maker != self || horde_.Ours(s.address)) {
                continue;  // another game's roll (a copy here), or one we made: no horde of its own
            }
            const bool dead = s.life && s.life->life <= 0;
            rolls.push_back({s.address, s.name, {s.position[0], s.position[1], s.position[2]},
                             sync_.PlayersNear(s.position, server_now_us), dead});
        }
        const std::uint16_t count = CoopDifficulty::Now().count_pct;
        if (const std::size_t added = horde_.Plan(scene, rolls, alive, apart, count, SteadyNowMs())) {
            LOG_INFO(Core, "coop: difficulty: night horde: {} more Stalchild in {} ({} alive, enemies {}%)", added,
                     scene, alive, count);
        }
        for (const auto& m : horde_.All()) {
            CoopSpawn::Copy c;
            c.key = kHordeKey | m.key;
            c.actor = m.name;
            c.position = {m.position[0], m.position[1], m.position[2]};
            if (m.gone) {
                continue;  // (one never made: off the director's list, so it is not made)
            }
            c.act = m.address != 0 ? CoopSpawn::Act::release : CoopSpawn::Act::show;
            c.place = false;  // its own AI; the roll sync shares it as one of our rolls
            c.remakes = 0;
            c.real = true;
            enemies.push_back(std::move(c));
        }
    }

    void TestKills(const std::vector<Seen>& seen, std::vector<LifeWrite>& writes, std::uint8_t self) {
        std::vector<TestStrike> strikes;
        {
            std::lock_guard lock(KeyRefundMutex());
            strikes.swap(TestKillQueue());
        }
        for (const TestStrike& strike : strikes) {
            const std::string& name = strike.name;
            if (strike.amount < 0) {
                LOG_INFO(Core, "coop: test stun {}: {}", name,
                         sync_.TestStun(name, player_, self) ? "said" : "not a copy (none here, or ours)");
                continue;
            }
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
                LOG_INFO(Core, "coop: test {} {}: none here with a life", strike.amount > 0 ? "hit" : "kill", name);
                continue;
            }
            if (strike.amount > 0) {
                // A partial hit: through the game's own Life step, never past its stops (no test_kill).
                LifeWrite w;
                w.address = best->address;
                w.hurt = std::min(strike.amount, best->life->life);
                w.life = best->life->life - w.hurt;
                w.from = player_;
                w.direct = true;
                writes.push_back(w);
                // (given straight to the Life step, no hit id moves: its fall is marked as our game's own hit)
                sync_.TestOwnHit(best->address, share_now_us_);
                LOG_INFO(Core, "coop: test hit {} {} {:#x} ({:.1f} m, life {}): hit", name, w.hurt, best->address,
                         best_m, best->life->life);
                continue;
            }
            LifeWrite w;
            w.address = best->address;
            w.life = 0;
            w.hurt = std::max<std::int32_t>(best->life->life, 1);
            w.from = player_;
            w.direct = true;
            w.test_kill = true;
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
        // An enemy (a boss too, or an actor we cannot name) is never written (the owner's native rule): its life
        // changes only through its game's own damage step (the replayed hit) or its own Life step (patch 0007);
        // else the change is lost. RouteLife decides (objects_test); this reads the game for it and acts.
        u64 player = 0;
        for (const auto& [a, known] : names_) {
            if (known.first == "Player_Child" || known.first == "Player_Adult") {
                player = a;
                break;
            }
        }
        LifeAsk ask;
        ask.native = w.direct || NativeOnly(w.address);
        ask.direct = w.direct;
        ask.replay_on = replay_;
        ask.has_player = player != 0;
        ask.hurt = w.hurt;
        ask.life = w.life;
        ask.old = old;
        ask.heal = w.heal;
        // Row 178: another player's hit on our boss while it is in a move its own game takes no hit in.
        // Its own game says whether it takes a hit now (HitGuard): for every boss, no move list.
        CoopBossNative::HitGuard guard;
        if (w.hurt > 0 && !w.direct && w.hitter != 0) {
            guard = ReadHitGuard(r, w.address);
            ask.out_of_reach = CoopBossNative::TheTable().OutOfReach(w.address, guard);
            if (const auto move = CoopBossNative::TheTable().OurBossMove(w.address)) {
                LOG_INFO(Core, "coop: objects: P{}'s hit on our boss {:#x} '{}' in '{}': its game {} (row 178: "
                               "invincible {}/{}, flags {:#x}, no-hit bodies {}/{}{})",
                         w.hitter, w.address, NameOf(w.address), *move,
                         guard.TakesNoHit() ? "takes no hit now" : "takes it",
                         guard.invincible_names, guard.invincible_other, guard.life_flags, guard.no_hit_bodies,
                         guard.bodies, guard.read ? "" : ", unread");
            }
        }
        std::optional<u64> life;
#ifdef COOP_HOOKSHOT
        // Hookshot with patch 0007: the hit (or the life the owner says this
        // copy has lost) goes in through the game's own Life step on its own
        // thread (CoopActorEvents::LifeTable), so the game clamps it, reacts
        // and runs its own death. A change still waiting from an earlier give
        // (the actor's step did not run: asleep, paused) is taken back: an
        // enemy's goes in again with the new one (RouteLife), anything else's
        // life is written directly, so nothing is dealt twice.
        // The damage field is left alone then: the step reads it into the
        // sum it tells us, and it would come back as a hit of this game's.
        auto& steps = CoopActorEvents::TheLifeTable();
        ask.step_seen = CoopActorEvents::LifeStepSeen().load(std::memory_order_relaxed);
        if (ask.step_seen) {
            const auto table = r.Ptr(w.address + kActorComponents);
            life = table ? r.Ptr(*table + kComponentLife) : std::nullopt;
            ask.has_component = life.has_value();
            if (life) {
                ask.replay_seen = ReplaySeen(r, *life);
            }
        }
        const bool steppable = ask.step_seen && life && old >= 0 && (w.hurt > 0 || w.life < old || (w.heal && w.life > old)) &&
                               !(w.hurt > 0 && replay_ && !w.direct && player != 0 && !ask.out_of_reach);
        if (steppable) {
            ask.stale = steps.Take(*life);
        }
#endif
        const LifeRouted routed = RouteLife(ask);
        if (static std::atomic<int> said_why{0};
            w.hurt > 0 && routed.route != LifeRoute::replay && said_why.fetch_add(1) < 40) {
            // Row 172 (run 053413): another player's hit not replayed loses the game's kill consequences.
            LOG_INFO(Core, "coop: objects: share life {:#x} '{}': P{}'s hit not replayed (route {}): replay {}, "
                           "player {:#x} (our Link {:#x}, {} names), direct {}, out of reach {}",
                     w.address, NameOf(w.address), w.hitter, static_cast<int>(routed.route), replay_, player,
                     our_link_, names_.size(), w.direct, ask.out_of_reach);
        }
        switch (routed.route) {
        case LifeRoute::replay: {
            Mover::Hit h;
            h.actor = w.address;
            // Row 177: the hitter's puppet, so the game's battle AI turns on the player who hit (else our Link).
            // A killing blow keeps our Link (the game's kill consequences may want the Player as the killer), and so
            // does every boss (row 178: its attacks need the game's attack permission, the Player's only).
            h.attacker = CoopHitReplay::ReplayAttacker(player, w.hitter, CoopPuppet::TheDirector().LiveSeenActor(w.hitter),
                                                       CoopDbno::Body::TheCrawl().PuppetUnseen(w.hitter),
                                                       CoopHitReplay::LethalHit(old, w.hurt, w.life),
                                                       CoopBossNative::TheTable().KeepsLinkAsAttacker(w.address));
            if (static std::atomic<int> said_kill{0};
                h.attacker == player && w.hitter != 0 && CoopHitReplay::LethalHit(old, w.hurt, w.life) &&
                said_kill.fetch_add(1) < 40) {
                LOG_INFO(Core, "coop: objects: share life {:#x} '{}': P{}'s killing blow replayed with our Link as the "
                               "attacker (row 177 review: the game's kill consequences see the Player)",
                         w.address, NameOf(w.address), w.hitter);
            }
            if (static std::atomic<int> said_boss{0};
                h.attacker == player && w.hitter != 0 && CoopBossNative::TheTable().KeepsLinkAsAttacker(w.address) &&
                said_boss.fetch_add(1) < 40) {
                LOG_INFO(Core, "coop: objects: share life {:#x} '{}': P{}'s hit on a boss replayed with our Link as "
                               "the attacker (row 178: a boss attacks only a target with the game's attack permission)",
                         w.address, NameOf(w.address), w.hitter);
            }
            if (static std::atomic<int> said{0}; h.attacker != player && said.fetch_add(1) < 40) {
                LOG_INFO(Core, "coop: objects: share life {:#x} '{}': P{}'s hit replayed with their puppet {:#x} as "
                               "the attacker (row 177: its AI turns on them)",
                         w.address, NameOf(w.address), w.hitter, h.attacker);
            }
            h.amount = w.hurt;
            h.at = r.Get<std::array<float, 3>>(w.address + kActorPosition).value_or(h.at);
            if (w.hit_at) {
                // Where it landed in the hitter's game (a boss: Gohma's headshot check against her eye bone,
                // which our game's own damage step makes with its own boss): the hit lands there.
                h.at = {(*w.hit_at)[0], (*w.hit_at)[1], (*w.hit_at)[2]};
                h.exact = true;
            }
            h.record = w.record;  // protocol 14: the hit that happened there (kHitTemplate when not sent)
            h.from = r.Get<std::array<float, 3>>(player + kActorPosition).value_or(h.at);
            if (w.from) {
                // The player who hit, so the enemy is knocked away from them.
                h.from = *w.from;
            }
            TheMover().AddHit(h);
            return;  // the game takes the life; after 400 ms the rest goes to its Life step (Sync)
        }
        case LifeRoute::wait_replay:
#ifdef COOP_HOOKSHOT
            if (ask.stale != 0) {
                steps.Give(*life, ask.stale);  // (left as it was)
            }
#endif
            // Our replayed hit still waits for this enemy's own damage step (asleep, paused): it takes that
            // hit itself when it runs, so the same fall is not given a second time.
            LOG_INFO(Core, "coop: objects: share life {:#x} '{}': its damage step has not seen our replayed hit "
                           "yet: nothing given (life {}, owner's {})",
                     w.address, NameOf(w.address), old, w.life);
            return;
        case LifeRoute::step:
#ifdef COOP_HOOKSHOT
            // A life (not a hurt, not a raise) is the owner's for its copy: judged there (PassStops). A test kill
            // is a kill (past the stops where it is not held: our own enemy).
            if (ask.out_of_reach) {
                LOG_INFO(Core, "coop: objects: share life {:#x} '{}': P{}'s hit came while its game takes no hit "
                               "(row 178): life only, through its Life step",
                         w.address, NameOf(w.address), w.hitter);
            }
            if (steps.Give(*life, routed.amount, (w.hurt <= 0 && !w.heal) || w.test_kill)) {
                LOG_INFO(Core, "coop: objects: share {} {:#x}: {} through the game's Life step at life {} (to {})",
                         w.hurt > 0 ? "hurt" : "life", w.address, routed.amount, old, w.life);
                if (w.hurt <= 0) {
                    // (a hurt's fall the Sync already expects, and writes itself when not taken)
                    sync_.Stepped(w.address, routed.amount, share_now_us_);
                    life_given_.push_back({*life, w.address, w.life, share_now_us_});
                }
                return;
            }
            if (!life_table_full_said_) {
                life_table_full_said_ = true;
                LOG_WARNING(Core, "coop: objects: no room to give a hit to the game's Life step ({:#x} '{}'): "
                                  "until the next scene an enemy's is lost, anything else's life written",
                            w.address, NameOf(w.address));
            }
#endif
            break;
        case LifeRoute::lost:
        case LifeRoute::write:
#ifdef COOP_HOOKSHOT
            if (ask.stale != 0 && !ask.native) {
                // (its step did not run: asleep or paused, or one of the
                // step's two paths that skip the sum; the name says which actors)
                LOG_INFO(Core, "coop: objects: {:#x} '{}': {} given earlier was never taken by its Life step",
                         w.address, NameOf(w.address), ask.stale);
            }
#endif
            break;
        }
        if (w.heal) {
            LOG_INFO(Core, "coop: objects: share raise {:#x} '{}': not taken by its game's Life step: its life stays {} "
                           "(old owner's {})",
                     w.address, NameOf(w.address), old, w.life);
            return;  // never a write of ours
        }
        if (w.direct) {
            LOG_INFO(Core, "coop: objects: a direct hit (test kill or boss hit) {:#x} '{}': the game's Life step "
                           "did not take it (or there is none: patch 0007): nothing written",
                     w.address, NameOf(w.address));
            return;  // a test kill or a boss hit goes only through the game's own step, never a write of ours
        }
        if (ask.native) {
            // (no Life step here, or it could not take it: the hit is lost; never a write of ours)
            ++lives_lost_;
            LOG_INFO(Core, "coop: objects: share {} {:#x} '{}': not taken by its game's own steps: its life stays "
                           "{} (owner's {}; {} lost so far)",
                     w.hurt > 0 ? "hurt" : "life", w.address, NameOf(w.address), old, w.life, lives_lost_);
            return;
        }
        if (w.hurt > 0 && !ask.step_seen) {
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

    // Row 97 (event-copy): our Link's event in the log. When it begins: its
    // kind and the name-like strings it points at (CoopEventMember::ReadRunning);
    // each change of the actors it is with (address, name, owner, why, distance);
    // every kEventStillUs while it runs, how long and which shared copies near our
    // Link still get their owner's motion; and how long it ran when it ends.
    static constexpr u64 kEventStillUs = 30'000'000;
    void EventLog(Reader& r, u64 now_us) {
        const bool running = TheLocalEvent().load();
        if (running && event_began_us_ == 0) {
            event_began_us_ = now_us;
            event_still_us_ = now_us;
            const auto ev = CoopEventMember::ReadRunning([&r](std::uint64_t at) { return r.Get<u64>(at); }, main_);
            std::string names;
            for (const std::string& n : ev.names) {
                names += (names.empty() ? "" : ", ") + n;
            }
            LOG_INFO(Core, "coop: event: {} event began: {:#x} kind {} ({} in the session); names seen: {}",
                     TheOwnEvent().load() ? "our Link's" : "an event without our Link (another player's or the Ocarina)",
                     ev.event, ev.kind ? std::to_string(*ev.kind) : std::string("?"), ev.count,
                     names.empty() ? std::string("none") : names);
        } else if (!running && event_began_us_ != 0) {
            LOG_INFO(Core, "coop: event: our Link's event ended after {:.1f} s",
                     static_cast<double>(now_us - event_began_us_) / 1e6);
            event_began_us_ = 0;
            event_partners_.clear();
        }
        if (!running) {
            return;
        }
        static constexpr const char* kWhy[] = {"none", "bound by the event", "its own AI acts", "townsperson"};
        std::set<u64> now_partners;
        const auto list = sync_.EventPartnerList(now_us);
        for (const auto& p : list) {
            now_partners.insert(p.address);
        }
        if (now_partners != event_partners_) {
            std::string text;
            for (const auto& p : list) {
                text += fmt::format("{}{} {:#x} (owner P{}, {}, {:.1f} m)", text.empty() ? "" : "; ", p.name,
                                    p.address, p.owner, kWhy[static_cast<int>(p.why)], p.metres);
            }
            LOG_INFO(Core, "coop: event: the actors our event is with: {}", text.empty() ? std::string("none") : text);
            event_partners_ = std::move(now_partners);
        }
        if (now_us - event_still_us_ >= kEventStillUs) {
            event_still_us_ = now_us;
            LOG_INFO(Core, "coop: event: our Link is still in its event after {:.0f} s ({} actors claimed)",
                     static_cast<double>(now_us - event_began_us_) / 1e6, list.size());
        }
    }

    // Issue 200 (npc-anim). The townspeople our event claimed (Sync::EventPartner)
    // keep this game's cutscene motion while the server hands them to us: the
    // owner's routine is not put on them. Logs the claim (our side) and, on the
    // other games, each NPC copy that moves to another owner; with
    // LICHII_COOP_DEBUG_NPC=1 every NPC copy's place and animation once a second.
    // Walking townspeople
    // (coop_npc_anim.h): the 5 s line of plays and frame sets.
    // LICHII_COOP_DEBUG_AGGRO=1 (issue 150 proof, research only): once a second, every live Enemy_ within
    // kAggroM of our Link: the Link it faces (ours or a puppet: a perceived puppet is who the game's own
    // perception may pick), by its forward axis (rotation column 2) on the ground, and each Link's distance
    // and angle off that axis. Read only; nothing is written.
    static constexpr float kAggroM = 30.0f;
    void Aggro(Reader& r, u64 now_us) {
        if (!aggro_ || !player_ || now_us - aggro_at_ < 1'000'000) {
            return;
        }
        aggro_at_ = now_us;
        struct Seen {
            std::string who;
            std::array<float, 3> at;
        };
        std::vector<Seen> links{{"Link", *player_}};
        for (const auto& [a, known] : names_) {
            if (known.first.starts_with("CoopPuppet_") && r.Get<u32>(a + kActorState).value_or(0) == kStateAlive) {
                if (const auto at = r.Get<std::array<float, 3>>(a + kActorPosition)) {
                    links.push_back({fmt::format("puppet {:#x}", a), *at});
                }
            }
        }
        for (const auto& [a, known] : names_) {
            if (!known.first.starts_with("Enemy_") || r.Get<u32>(a + kActorState).value_or(0) != kStateAlive) {
                continue;
            }
            const auto at = r.Get<std::array<float, 3>>(a + kActorPosition);
            const auto m = r.Get<std::array<float, 9>>(a + kActorRotation);
            if (!at || !m || Apart(*player_, *at) > kAggroM) {
                continue;
            }
            const float yaw = std::atan2((*m)[2], (*m)[8]);
            std::string each;
            std::size_t best = 0;
            float best_off = 1e9f;
            for (std::size_t i = 0; i < links.size(); ++i) {
                const float dx = links[i].at[0] - (*at)[0];
                const float dz = links[i].at[2] - (*at)[2];
                float off = std::fabs(std::remainder(std::atan2(dx, dz) - yaw, 6.2831853f)) * 57.29578f;
                if (off < best_off) {
                    best_off = off;
                    best = i;
                }
                each += fmt::format("; {} {:.1f} m off {:.0f} deg", links[i].who, std::sqrt(dx * dx + dz * dz), off);
            }
            // owner P0: not shared (yet), or not on our list.
            LOG_INFO(Core, "coop: aggro: {} {:#x} (owner P{}) faces {} (off {:.0f} deg){}", known.first, a,
                     sync_.OwnerAt(a), links[best].who, best_off, each);
        }
    }

    void NpcAnim(Reader& r, u64 now_us, std::vector<u64>& removes,
                 std::vector<Mover::Pose>& wish, std::vector<PlayWish>& plays) {
        // Row 97: every actor the event is with (Sync::EventPartner), not only
        // townspeople: its own game runs it for the event.
        std::erase_if(removes, [&](u64 a) { return sync_.EventPartner(a, now_us); });
        std::erase_if(wish, [&](const Mover::Pose& p) { return sync_.EventPartner(p.actor, now_us); });
        std::erase_if(plays, [&](const PlayWish& p) { return sync_.EventPartner(p.address, now_us); });
        EventLog(r, now_us);
        const std::size_t claimed = sync_.EventPartners(now_us);
        if ((claimed == 0) != (npc_claimed_ == 0)) {
            if (claimed != 0) {
                LOG_INFO(Core, "coop: npc-anim: our Link is in an event: {} actors it is with claimed", claimed);
            } else {
                LOG_INFO(Core, "coop: npc-anim: the event's actors are free again");
            }
        }
        // The hand-over itself (the server may give them 0.5 s later, or
        // keep one in another game's fight lock): log each change of how
        // many of the claimed ones are ours.
        const std::size_t ours = claimed == 0 ? 0 : sync_.EventPartnersOurs(now_us);
        if (ours != npc_ours_ && claimed != 0) {  // the end has its own line
            LOG_INFO(Core, "coop: npc-anim: {} of the {} claimed actors are ours now", ours, claimed);
        }
        npc_ours_ = ours;
        npc_claimed_ = claimed;
        // Row 138(8): the area claims (coop_sensor_claim.h), on each change of how many.
        const std::size_t area = sync_.AreaClaims(now_us);
        if (area != area_claimed_) {
            LOG_INFO(Core, "coop: area claim: our Link is in the area of {} shared actors (their own sensor input)",
                     area);
            area_claimed_ = area;
        }
        static const bool debug_npc = EnvFlag("LICHII_COOP_DEBUG_NPC");  // opt-in debug lines
        const bool every = debug_npc && now_us - npc_log_at_ >= 1'000'000;
        if (every) {
            npc_log_at_ = now_us;
        }
        // Walking townspeople (coop_npc_anim.h). Every 5 s: how often an
        // owner's animation had to be played on a copy (once for each change,
        // not once a frame), and how often a copy's frame was put right.
        if (now_us - npc_calls_at_ >= 5'000'000) {
            npc_calls_at_ = now_us;
            const auto [aliased, unaliased, framed] = TheMover().TakeNpc();
            if (aliased + unaliased + framed > 0) {
                LOG_INFO(Core,
                         "coop: npc-anim: in 5 s {} owner animations played under the copy's "
                         "own AI's name, {} names given back, {} frames set",
                         aliased, unaliased, framed);
            }
        }
        if (every) {
            // What each copy really shows, and ours (the frame we send).
            for (const PlayWish& p : plays) {
                const auto name = names_.find(p.address);
                if (!p.play_frame || name == names_.end() || !name->second.first.starts_with("Npc_")) {
                    continue;
                }
                const auto shown = AnimationAt(r, p.address, 0, false, CoopNpcAnim::kSlotPlayFrame);
                // Under an alias the slot names the AI's animation, not the
                // one playing: say which plays, so a working alias and the
                // AI winning read differently in the log.
                const std::string aliased = TheMover().AliasedPlay(p.address);
                LOG_INFO(Core,
                         "coop: npc-anim: copy {} shows '{}'{} frame {:.1f}; its owner '{}' frame "
                         "{:.1f} rate {:.2f} running {:.2f} loop {:.1f}",
                         name->second.first, shown ? shown->name : std::string(),
                         aliased.empty() ? std::string() : " (alias of '" + aliased + "')",
                         shown ? shown->frame : 0.0f, p.name, p.frame, p.rate, p.run, p.loop);
            }
            for (const auto& [a, name] : sync_.Owned()) {
                if (!name.starts_with("Npc_")) {
                    continue;
                }
                if (const auto ours = AnimationAt(r, a, 0, false, CoopNpcAnim::kSlotPlayFrame)) {
                    LOG_INFO(Core, "coop: npc-anim: ours {} plays '{}' frame {:.1f} rate {:.2f}",
                             name, ours->name, ours->frame, ours->rate);
                }
            }
        }
        for (const Mover::Pose& p : wish) {
            const auto name = names_.find(p.actor);
            if (name == names_.end() || !name->second.first.starts_with("Npc_")) {
                continue;
            }
            const u8 owner = sync_.OwnerAt(p.actor);
            auto& last = npc_owner_[p.actor];
            if (!every && last == owner) {
                continue;
            }
            const auto play = std::find_if(plays.begin(), plays.end(),
                                           [&p](const PlayWish& w) { return w.address == p.actor; });
            LOG_INFO(Core, "coop: npc-anim: {} copied from P{} at {:.2f} {:.2f} {:.2f} plays '{}' frame {:.1f}",
                     name->second.first, owner, p.transform[0], p.transform[1], p.transform[2],
                     play != plays.end() ? play->name : std::string(), play != plays.end() ? play->frame : 0.0f);
            last = owner;
        }
        if (npc_owner_.size() > 512) {
            npc_owner_.clear();
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
        // The enemies' brain path: a plain copy whose own brain asked for another phase than its owner's wants
        // to act on our Link (issue 101; no play to give back: its brain is let run instead).
        for (const u64 a : CoopBossNative::TheTable().TakeOwnWants()) {
            sync_.NoteOwnBrainWant(a, now_us);
        }
        // Issue 239: a followed copy was written where the Follower drew it, not where it was wished.
        const auto drawn = TheMover().Drawn();
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
            const auto drew = drawn.find(w.address);
            const auto put = placed_.find(w.address);
            if (drew != drawn.end() || put != placed_.end()) {
                const coop::protocol::Vec3& before = drew != drawn.end() ? drew->second : put->second;
                coop::protocol::Vec3 now{};
                float d2 = 0.0f;
                for (std::size_t i = 0; i < 3; ++i) {
                    now[i] = r.Get<float>(w.address + kActorPosition + 4 * i).value_or(0.0f);
                    const float d = now[i] - before[i];
                    d2 += d * d;
                }
                const float d = std::sqrt(d2);
                // A copy whose own AI pulls it at our Link (issue 101), from what it was last written at.
                sync_.NoteDrift(w.address, before, now, now_us);
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
            // jitter-b: a townsperson is followed in even steps each game frame (coop_follow.h). Issue 239 /
            // rows 37, 54: an enemy copy too (plain, mid-boss, boss). Its place was worked out at the present
            // and written on a later game frame, a different time apart each frame (the cause jitter-b fixed
            // for townspeople): uneven steps, the more so the faster it moves. Same setter as before.
            if (const auto name = names_.find(w.address);
                w.velocity && name != names_.end() && FollowedEachFrame(name->second.first)) {
                p.velocity = std::array<float, 3>{(*w.velocity)[0], (*w.velocity)[1], (*w.velocity)[2]};
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
            std::vector<u64> no_replay;
            for (const u64 a : ours) {
                if (CoopBossNative::TheTable().RunsOwnMoveWhenOurs(a)) {
                    no_replay.push_back(a);
                }
            }
            TheMover().Release(ours, no_replay);
        }
        // Report 109: a copy's own wake. On the enemies' brain path its own brain runs it (NoteOwnWake); the
        // pose-era give-back of the wake's animation is for the other copies only (one maker, rows 109/111).
        for (auto& [a, name] : sync_.TakeOwnWakes()) {
            if (!CoopBossNative::TheTable().WakesOwnBrain(a)) {
                TheMover().GiveBack(a, std::move(name));
            }
        }
        for (const u64 a : sync_.TakeOwnWakeStarts()) {
            CoopBossNative::TheTable().NoteOwnWake(a);  // (a plain copy on the brain path; else nothing)
        }
        for (const u64 a : sync_.TakeOwnStuns()) {
            CoopBossNative::TheTable().NoteOwnStun(a);  // row 169: a copy's own stun is its own time
        }
        std::vector<u64> removes = sync_.Removes();
        for (const u64 a : horde_.Removes()) {
            removes.push_back(a);  // difficulty: night horde beside a roll the spawner took away
        }
        for (const u64 a : world_.Removes()) {
            removes.push_back(a);
        }
        // Issue 144: nothing near our Link is removed, moved or made to play while
        // it is in an event; they come again each frame and are made after it.
        {
            const CoopGuard::Local local = GuardLocal(now_us);
            const auto where = [this](std::uint64_t a) { return Where(a); };
            CoopGuard::Skip(removes, local, where, [](u64 a) { return a; });
            // Row 98(2): an event's last enemy is not removed by another game before our game played it.
            std::erase_if(removes, [this](u64 a) { return keeper_addresses_.contains(a); });
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
        // Row 130 (two players entering Gohma's room together; only one could beat her): a boss copy that runs
        // its own changes now (CoopBossNative OwnTime: our event, our hit's reaction, a mid-boss's own start, its
        // death) gets none of its owner's place, animation or frame meanwhile. Else its own game's event waited
        // for a boss the owner's motion put back every frame (her appear: C01 at frame 1 over Pre_Battle_*), and
        // our Link stayed in it until the owner's game killed her. Its owner's remove still comes.
        {
            const auto own = CoopBossNative::TheTable().CopiesRunningOwn();
            const auto runs_own = [&own](u64 a) { return std::find(own.begin(), own.end(), a) != own.end(); };
            std::erase_if(wish, [&](const Mover::Pose& p) { return runs_own(p.actor); });
            std::erase_if(plays, [&](const PlayWish& p) { return runs_own(p.address); });
            if (own.empty() == boss_own_said_) {
                boss_own_said_ = !own.empty();
                LOG_INFO(Core, "coop: objects: {}", boss_own_said_
                             ? fmt::format("{} boss copy(ies) run their own (our event, our hit, their start or "
                                           "death): their owner's place and animation wait", own.size())
                             : std::string("boss copies take their owner's place and animation again"));
            }
        }
        NpcAnim(r, now_us, removes, wish, plays);
        Aggro(r, now_us);
        // Native puppets phase 3: another player's Link holds ours: our Link is placed first, as a copy is.
        {
            OurLinkPut put;
            {
                std::lock_guard lock(OurLinkPutMutex());
                put = TheOurLinkPut();
            }
            const u64 now_ms = SteadyNowMs();
            if (put.asked_ms != 0 && now_ms - put.asked_ms <= 200) {
                if (const auto link = OurLinkActor()) {
                    Mover::Pose p;
                    p.actor = *link;
                    p.transform[0] = put.at[0];
                    p.transform[1] = put.at[1];
                    p.transform[2] = put.at[2];
                    wish.insert(wish.begin(), p);
                }
            }
        }
        TheMover().Want(main_, std::move(wish), std::move(removes), std::move(plays));
        if (now_us - drift_at_ >= kDriftLogUs) {
            const auto [moved, deleted, played, framed] = TheMover().TakeCounts();
            if (const auto [uneven_avg, uneven_max, uneven_n] = TheMover().TakeUneven(); uneven_n > 0) {
                LOG_INFO(Core, "coop: objects: followed copies' steps uneven avg {:.4f} max {:.3f} m over {} "
                               "(issue 239: a step against the one before)",
                         uneven_avg, uneven_max, uneven_n);
            }
            if (const auto [started, skipped] = TheMover().TakeNodeStarts(); started + skipped > 0) {
                LOG_INFO(Core, "coop: world: push node starts run {}, skipped {} (block gone, another "
                               "node, or busy by then)",
                         started, skipped);
            }
            if (drift_count_ > 0 || moved > 0 || deleted > 0 || played > 0 || framed > 0 ||
                sync_.HurtsTaken() + sync_.HurtsMissed() > 0) {
                LOG_INFO(Core,
                         "coop: objects: share placed {} copies; off from where we put them "
                         "avg {:.3f} max {:.3f} over {}; game calls: {} moves, {} deletes, {} "
                         "plays, {} frame sets; hurts taken {} missed {} (boss hits to its Life step {}, dying copies "
                         "finished {}); own hits kept while a replay waited {}",
                         placed_.size(), drift_count_ ? drift_sum_ / drift_count_ : 0.0f,
                         drift_max_, drift_count_, moved, deleted, played, framed, sync_.HurtsTaken(),
                         sync_.HurtsMissed(), sync_.BossHitsStepped(), sync_.DyingFinished(),
                         sync_.OwnHitsKept());
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
    // Issue 200 (npc-anim): the log of our event's claim and of NPC copies.
    std::size_t npc_claimed_ = 0;
    u64 event_began_us_ = 0;    // row 97: EventLog
    u64 event_still_us_ = 0;
    std::set<u64> event_partners_;
    std::set<std::string> member_said_;  // row 97: names whose event member read was logged this event
    static constexpr std::size_t kMemberSaidMax = 32;
    // G1: the game's own member test in the log: our Link's last line, and each actor's last line
    // while an event runs (by address: cleared when no event runs and our Link's member is idle).
    std::string link_member_said_;
    std::map<u64, std::string> members_said_;
    static constexpr std::size_t kMembersSaidMax = 160;
    std::size_t npc_ours_ = 0;     // how many of them the server gave us (log)
    u64 npc_log_at_ = 0;
    u64 npc_calls_at_ = 0;  // the 5 s line of the walking townspeople
    std::unordered_map<u64, u8> npc_owner_;  // the owner each NPC copy was last logged with
    u64 drive_enemy_ = 0;
    u64 drive_log_at_ = 0;
    u64 drive_hurt_at_ = 0;
    float drift_sum_ = 0.0f;
    float drift_max_ = 0.0f;
    u64 drift_worst_ = 0;  // the copy at drift_max_
    static constexpr float kDriftWorstM = 1.0f;
    std::size_t drift_count_ = 0;
    u64 drift_at_ = 0;
    bool boss_own_said_ = false;  // row 130: the last 'boss copies run their own' line logged
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
    std::unordered_map<u64, ActorStamp> stamps_;  // what each cached name was read with (row 172)
    // Row 138(8): townspeople and animals (by actor, its placement hash) whose live logic has no area input.
    std::unordered_map<u64, u64> no_area_;
    static constexpr std::size_t kNoAreaMax = 4096;
    std::size_t area_claimed_ = 0;  // row 138(8): the log's last count of area claims
    std::unordered_map<u64, u64> life_components_;  // the actor by its Life component (shared actors with a life)
    // Difficulty: this scene visit's extra enemies (where from the game's placement data).
    CoopDifficulty::Extras extras_{CoopDifficulty::AreaOf, SpecialForExtras};
    CoopDifficulty::Horde horde_;    // difficulty: the night horde we made beside our own rolls
    u64 share_now_us_ = 0;  // this Share's server time (WriteLife)
    // The scene the save's Gold Skulltula keys were fed for (issue 89, Share's
    // key scan): a scene's actors are made once, so one scan a scene is enough.
    std::string keys_scene_;
    // The Gold Skulltula placements known to be 1 here, and the scene that set the
    // baseline (issue 89, 2026-11-17): a key that is 1 now and was 0 at the
    // baseline is one this game took, and its Skulltula and token go at once
    // (a token made and taken between two walks was never sent as a break).
    std::set<u64> gold_taken_;
    std::string gold_baseline_scene_;
    std::int32_t gold_baseline_tokens_ = 0;
    CoopEvery::Every gold_sweep_{CoopEvery::kHalfSecondUs};  // the token count, twice a second
    // A new session, the title, or a new game run: what the last save's keys said
    // is forgotten, so the next save's own keys are read (review of #257: a
    // player who reconnects and loads another save must not have that save's
    // untaken Gold Skulltulas deleted). Same thread as Share.
    void ForgetGold() {
        keys_scene_.clear();
        gold_taken_.clear();
        gold_baseline_scene_.clear();
        gold_baseline_tokens_ = 0;
        gold_sweep_.Reset();
    }
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
        objects.aggro_ = EnvFlag("LICHII_COOP_DEBUG_AGGRO");
        objects.cheat_ = EnvFlag("LICHII_COOP_OBJECTS_CHEAT");
        objects.share_ = EnvFlag("LICHII_COOP_OBJECTS");
        objects.pose_ = EnvOnUnlessZero("LICHII_COOP_OBJECTS_POSE");
        objects.gimmick_probe_on_ = EnvFlag("LICHII_COOP_GIMMICK_PROBE");  // opt-in: many reads
        objects.logic_probe_on_ = EnvFlag("LICHII_COOP_LOGIC_PROBE");      // jabu-lifts: reads only
        objects.lift_ride_on_ = EnvOnUnlessZero("LICHII_COOP_LIFT_RIDE");            // jabu-lifts (#65), on unless =0
        {
            const std::string platforms = EnvText("LICHII_COOP_PLATFORMS");
            objects.platforms_mode_ = platforms == "0" ? 0 : platforms == "probe" ? 2 : 1;
            const std::string share = EnvText("LICHII_COOP_SWITCH_SHARE");  // room-objects
            objects.switch_share_mode_ = share == "0" ? 0 : share == "probe" ? 2 : 1;
        }
        objects.scrubs_on_ = EnvOnUnlessZero("LICHII_COOP_SCRUBS");  // issue 149, on unless =0
        objects.ocarina_probe_on_ = EnvFlag("LICHII_COOP_OCARINA_PROBE");  // issue 152, reads only
        // On by default (=0 turns it off): a life an enemy must lose is given
        // to the game as a whole hit, so it flinches, is knocked back and a
        // Stalchild loses its head on every screen. An amount alone at
        // Life + 0x228 is not taken (live test, 2026-11-09).
        objects.replay_ = EnvOnUnlessZero("LICHII_COOP_OBJECTS_HIT_REPLAY");
        objects.hurt_ = EnvFlag("LICHII_COOP_OBJECTS_HURT") || objects.replay_;
        objects.drive_ = EnvFlag("LICHII_COOP_OBJECTS_TEST_DRIVE");
        objects.ai_hold_ = EnvNumber("LICHII_COOP_OBJECTS_AI_HOLD");
        objects.test_take_ = EnvHex("LICHII_COOP_OBJECTS_TEST_TAKE");
        objects.drive_mode_ = objects.drive_ ? EnvNumber("LICHII_COOP_OBJECTS_TEST_DRIVE") : 0;
        objects.hearts_to_link_ = EnvOnUnlessZero("LICHII_COOP_HEARTS_TO_LINK");
        objects.wake_ = EnvOnUnlessZero("LICHII_COOP_OBJECTS_WAKE");
        objects.release_ = EnvOnUnlessZero("LICHII_COOP_OBJECTS_RELEASE");
        objects.pvp_replay_ = EnvOnUnlessZero("LICHII_COOP_PVP_REPLAY");
        objects.wake_far_ = EnvOnUnlessZero("LICHII_COOP_OBJECTS_ASLEEP");
        return true;
    }();
    (void)once;
    return objects;
}

} // namespace

void SetLocalEvent(bool in_event, bool ours) {
    TheOwnEvent().store(in_event && ours);
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

void SetMaxLifeLowerTo(std::optional<std::int32_t> quarters) {
    TheMaxLifeLowerTo().store(quarters.value_or(-1));
}

std::optional<std::int32_t> OurLinkLife() {
    const u64 at = TheLinkLifeAt().load();
    const s32 life = TheLinkLife().load();
    if (at == 0 || life < 0 || SteadyNowMs() > at + kLinkLifeStaleMs) {
        return std::nullopt;
    }
    return life;
}

std::optional<std::uint64_t> OurLinkActor() {
    const u64 at = TheLinkActorAt().load();
    const u64 actor = TheLinkActor().load();
    if (at == 0 || actor == 0 || SteadyNowMs() > at + kLinkLifeStaleMs) {
        return std::nullopt;
    }
    return actor;
}

void PlaceOurLink(const std::array<float, 3>& at) {
    const u64 now = SteadyNowMs();
    std::lock_guard lock(OurLinkPutMutex());
    TheOurLinkPut() = OurLinkPut{at, now == 0 ? 1 : now};
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

static_assert(kPvpKindWords == coop::protocol::kPvpKindWords &&
              CoopHitReplay::kRecKind + kPvpKindWords == CoopHitReplay::kRecKindEnd);

namespace {
CoopHitReplay::Record KindRecord(const std::array<std::uint32_t, kPvpKindWords>& kind) {
    CoopHitReplay::Record rec = CoopHitReplay::kHitTemplate;
    for (std::size_t i = 0; i < kind.size(); ++i) {
        rec[CoopHitReplay::kRecKind + i] = kind[i];
    }
    return rec;
}
} // namespace

void HurtOurLinkNative(std::int32_t quarters, const std::array<float, 3>& at, const std::array<float, 3>& from,
                       const std::array<std::uint32_t, kPvpKindWords>& kind) {
    if (quarters < 0) {
        return;
    }
    std::lock_guard lock(PvpMutex());
    if (PvpAsks().size() < 16) {
        PvpAsk ask{quarters, at, from};
        ask.native = true;
        ask.record = KindRecord(kind);
        PvpAsks().push_back(ask);
    }
}

void ReplayOnCopy(std::uint64_t actor, std::int32_t amount, const std::array<float, 3>& at,
                  const std::array<std::uint32_t, kPvpKindWords>& kind) {
    const auto link = OurLinkActor();
    if (actor == 0 || !link) {
        return;
    }
    Mover::Hit hit;
    hit.actor = actor;
    hit.attacker = *link;  // the attacker's Link is another game's; ours stands in (as protocol 14)
    (void)amount;
    hit.amount = 0;  // the effect only: a copy never takes life in any game (review M1; its Life is invincible too)
    hit.at = at;
    hit.from = at;
    hit.exact = true;
    hit.record = KindRecord(kind);
    TheMover().AddHit(hit);
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

std::optional<std::array<float, 3>> TestPlatformPlace(const std::string& name) {
    TestPlatformPlaces& places = ThePlatformPlaces();
    std::lock_guard lock(places.mutex);
    const auto it = places.at.find(name);
    if (it == places.at.end()) {
        return std::nullopt;
    }
    return it->second;
}

void QueueTestAim(float x, float y, float z) {
    TheMover().ArmAim({x, y, z}, SteadyNowMs() + 10000);
    LOG_INFO(Core, "coop: test aim {:.2f} {:.2f} {:.2f}: waits for our next arrow (10 s)", x, y, z);
}

void QueueTestKill(std::string name) {
    std::lock_guard lock(KeyRefundMutex());
    if (TestKillQueue().size() < 16) {
        TestKillQueue().push_back({std::move(name), 0});
    }
}

void QueueTestHit(std::string name, std::int32_t amount) {
    std::lock_guard lock(KeyRefundMutex());
    if (TestKillQueue().size() < 16 && amount > 0) {
        TestKillQueue().push_back({std::move(name), amount});
    }
}

void QueueTestStun(std::string name) {
    std::lock_guard lock(KeyRefundMutex());
    if (TestKillQueue().size() < 16) {
        TestKillQueue().push_back({std::move(name), -1});
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

std::vector<std::pair<std::uint32_t, bool>> TakeKeyLends() {
    std::lock_guard lock(KeyRefundMutex());
    std::vector<std::pair<std::uint32_t, bool>> out;
    out.swap(KeyLendQueue());
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
std::vector<std::uint64_t> g_bosses_ours;  // the bosses of g_bosses our game owns (boss-intro)
std::map<std::string, BossOwnership, std::less<>> g_boss_last;  // the last ownership reading per boss (BossOurs)
BossLives g_boss_lives;  // the last reading per boss body this scene, and its delete request (BossAliveKept)
std::string g_boss_scene;  // the scene g_boss_lives is of
std::uint64_t g_bosses_at = 0;
constexpr std::uint64_t kBossesFreshUs = 1'500'000;

} // namespace

void NoteBosses(const std::vector<Seen>& seen, const std::vector<std::pair<u64, std::string>>& owned,
                const std::string& scene) {
    std::vector<Seen> bosses;
    std::vector<std::uint64_t> ours;
    for (const Seen& s : seen) {
        if (s.name.starts_with("Enemy_DungeonBoss_")) {
            bosses.push_back(s);
            for (const auto& [address, name] : owned) {
                if (address == s.address) {
                    ours.push_back(address);
                }
            }
        }
    }
    std::lock_guard lock(g_bosses_mutex);
    g_bosses = std::move(bosses);
    g_bosses_ours = std::move(ours);
    g_bosses_at = CoopEvery::NowUs();
    if (scene != g_boss_scene) {
        g_boss_lives.clear();  // another scene's bodies (left with the scene, not deleted one by one)
        g_boss_scene = scene;
    }
    for (const Seen& s : g_bosses) {
        // Every boss body's death as our walk reads it (the LANE1 checks look for this line in each game).
        const auto was = g_boss_lives.find(s.name);
        if (s.life && s.life->life <= 0 && (was == g_boss_lives.end() || was->second.life > 0)) {
            LOG_INFO(Core, "coop: bosses: {} {:#x} read at life 0 here ({}): its death", s.name, s.address,
                     was == g_boss_lives.end() ? std::string("first reading")
                                               : "from " + std::to_string(was->second.life));
        }
    }
    NoteBossLives(g_bosses, g_boss_lives);
}

void NoteBossDeleted(std::uint64_t address) {
    std::lock_guard lock(g_bosses_mutex);
    NoteBossRemoved(address, g_boss_lives);
}

std::optional<bool> LastBossAlive(std::span<const std::string_view> bodies) {
    std::lock_guard lock(g_bosses_mutex);
    const std::uint64_t now = CoopEvery::NowUs();
    if (g_bosses_at == 0 || now < g_bosses_at || now - g_bosses_at > kBossesFreshUs) {
        return std::nullopt;
    }
    return BossAliveKept(g_bosses, bodies, g_boss_lives);
}

bool LastBossReadDead(std::span<const std::string_view> bodies) {
    std::lock_guard lock(g_bosses_mutex);
    const std::uint64_t now = CoopEvery::NowUs();
    if (g_bosses_at == 0 || now < g_bosses_at || now - g_bosses_at > kBossesFreshUs) {
        return false;
    }
    return BossReadDead(g_bosses, bodies, g_boss_lives);
}

void ForgetBosses() {
    std::lock_guard lock(g_bosses_mutex);
    g_bosses.clear();
    g_bosses_ours.clear();
    g_boss_last.clear();  // another session's (or run's) owner says nothing here
    g_boss_lives.clear();
    g_boss_scene.clear();
    g_bosses_at = 0;
}

std::optional<bool> LastBossOurs(std::span<const std::string_view> bodies) {
    std::lock_guard lock(g_bosses_mutex);
    const std::uint64_t now = CoopEvery::NowUs();
    if (g_bosses_at == 0 || now < g_bosses_at || now - g_bosses_at > kBossesFreshUs) {
        return std::nullopt;
    }
    return BossOurs(g_bosses, g_bosses_ours, bodies, g_bosses_at, g_boss_last);
}

void SetUnreadyBosses(std::vector<std::string> bodies) {
    std::lock_guard lock(g_unready_mutex);
    g_unready = std::move(bodies);
}

void SetDyingBosses(std::vector<std::string> bodies) {
    std::lock_guard lock(g_unready_mutex);
    g_dying = std::move(bodies);
}

std::optional<bool> LastBossHurt(std::span<const std::string_view> bodies) {
    std::lock_guard lock(g_bosses_mutex);
    const std::uint64_t now = CoopEvery::NowUs();
    if (g_bosses_at == 0 || now < g_bosses_at || now - g_bosses_at > kBossesFreshUs) {
        return std::nullopt;
    }
    return BossHurt(g_bosses, bodies);
}

std::optional<bool> LastBossPreBattle(std::span<const std::string_view> bodies) {
    std::lock_guard lock(g_bosses_mutex);
    const std::uint64_t now = CoopEvery::NowUs();
    if (g_bosses_at == 0 || now < g_bosses_at || now - g_bosses_at > kBossesFreshUs) {
        return std::nullopt;
    }
    return BossPreBattle(g_bosses, bodies);
}

} // namespace CoopObjects
