// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared enemies, the hook side (docs/objects.md, "Design" and "Telling one
// actor from another"). No emulator code: coop_objects.cpp walks the game's
// actor list and hands each walk to Sync::Tick, which says what to send and
// which lives to write. Tested in tests/lichii/objects_test.cpp.
//
// - OBJECT_PRESENT for the enemies loaded in our scene, on every change and
//   every 2 s.
// - The owner sends OBJECT_SAMPLE (time, position, life) at most 20 a second
//   while it changes, once a second while it is still.
// - A copy we do not own whose life drops sends DAMAGE to the owner, summed
//   over 100 ms; the owner writes life minus the damage into its enemy.
// - A copy we do not own takes the owner's life from its samples (a lower
//   life at once; a higher one only when no hit of ours is in flight).
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopObjects {

// One actor of one walk.
struct Seen {
    std::uint64_t address = 0;  // the actor, in guest memory
    std::string name;           // the game's actor name
    std::uint64_t hash = 0;     // placement hash; 0 or ~0 when the game made it
    std::optional<coop::protocol::ObjectLife> life;
    coop::protocol::Vec3 position{};
};

// A life to write into an actor (its +0x1c10 life value).
struct LifeWrite {
    std::uint64_t address = 0;
    std::int32_t life = 0;
};

// Shared: enemies, and the breakable world objects (coop_world.h).
bool IsShared(std::string_view name);

// The id of an enemy the scene made without a placement hash: the scene, the
// actor name and its place in the order such enemies of that name appeared
// in this scene visit (first seen first; the same walk ordered by x, then z).
// Both games load a scene's own enemies the same way, so they match; enemies
// made near one player (a Stalchild under one game's Link) do not, and stay
// each game's own. The top bit is set, the rest is FNV-1a 64.
std::uint64_t SceneSpawnId(std::string_view scene, std::string_view name, unsigned order);

class Sync {
public:
    static constexpr std::uint64_t kPresentEveryUs = 2'000'000;
    static constexpr std::uint64_t kSampleEveryUs = 50'000;     // 20 a second
    static constexpr std::uint64_t kStillEveryUs = 1'000'000;
    static constexpr std::uint64_t kDamageSumUs = 100'000;
    static constexpr std::uint64_t kHitHoldUs = 500'000;        // no healing back after our hit
    static constexpr std::uint64_t kFightUs = 12'000'000;

    // Network thread: OBJECT_OWNER, OBJECT_SAMPLE, HANDOVER_ASK,
    // HANDOVER_STATE, DAMAGE. Others are ignored. Applied on the next Tick.
    void Receive(const coop::protocol::Message& message);

    // Present thread, once per walk. `self` is our player number (1 to 4),
    // `now_us` the server time. A new scene forgets every object.
    void Tick(const std::string& scene, std::uint8_t self, std::uint64_t now_us,
              const std::vector<Seen>& seen, std::vector<coop::protocol::Message>& send,
              std::vector<LifeWrite>& writes);

    // Present thread: not connected or the game went away. Forgets everything.
    void Reset();

    // For tests and logs.
    std::size_t Count() const { return objects_.size(); }
    std::optional<coop::protocol::ObjectId> IdOf(std::uint64_t address) const;
    std::uint8_t OwnerOf(const coop::protocol::ObjectId& id) const;

private:
    struct Owner {
        std::uint8_t player = 0;
        std::uint16_t epoch = 0;
    };
    struct Object {
        coop::protocol::ObjectId id;
        std::uint64_t address = 0;
        std::optional<std::int32_t> life;  // as last read or written
        std::int32_t max = 0;
        coop::protocol::Vec3 position{};
        std::uint64_t fight_until = 0;
        // Owner side.
        std::uint64_t sent_at = 0;
        bool sent_once = false;
        std::optional<coop::protocol::ObjectLife> sent_life;
        coop::protocol::Vec3 sent_position{};
        std::uint32_t sequence = 0;
        // Copy side.
        std::int32_t hit_sum = 0;
        std::uint64_t hit_since = 0;
        std::uint64_t hit_at = 0;  // last DAMAGE sent
    };

    void Flush(std::uint8_t self, std::uint64_t now_us, std::vector<Object>& gone,
               std::vector<coop::protocol::Message>& send);
    void Apply(std::uint8_t self, std::uint64_t now_us, std::vector<coop::protocol::Message>& send,
               std::vector<LifeWrite>& writes);
    coop::protocol::ObjectState StateOf(Object& o, std::uint64_t now_us);

    std::string scene_;
    std::map<std::uint64_t, Object> objects_;  // by actor address
    std::map<std::string, unsigned> order_;    // next order per name, this scene visit
    std::map<coop::protocol::ObjectId, Owner> owners_;
    std::vector<coop::protocol::ObjectPresence> present_sent_;
    std::uint64_t present_at_ = 0;
    bool present_once_ = false;

    std::mutex inbox_mutex_;
    std::vector<coop::protocol::Message> inbox_;
};

} // namespace CoopObjects
