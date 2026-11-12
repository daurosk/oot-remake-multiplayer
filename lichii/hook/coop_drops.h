// SPDX-License-Identifier: GPL-3.0-or-later
//
// Shared drops (docs/objects.md, "Drops"): the rupees, hearts and ammo that
// an enemy, a pot, grass or a rock leaves are the same in every game, and one
// player picking one up takes it away in all of them. On unless
// LICHII_COOP_DROPS=0; needs the svc cave (puppets or spawn on).
//
// Every game's own game still rolls its own drop when the thing dies or
// breaks there. Nothing in the game is patched; the list of actors is watched:
//
// - A drop actor that turns up is announced with DROP_SPAWN (its own id).
// - Two announcements from different games, near in place and time, are the
//   rolls for the same thing. One stays: the one whose maker owns what
//   dropped it, then the one from the lower player number. Every game works
//   that out from the announcements alone, so all agree whatever order they
//   came in.
// - A game holding a losing roll deletes that actor, or keeps it as the
//   winner's when it is the same kind of drop. A game with no actor for the
//   winner creates one through the spawn director.
// - A drop that leaves our list while our Link stands on it was picked up, one
//   that leaves with Link near blinked out: DROP_TAKE, and every game deletes
//   its own. Further away it was only unloaded here.
// - A game that enters a scene sends DROP_ASK; the others tell the drops they
//   still have again.
//
// Sync and Remover need no emulator code and are tested in
// tests/lichii/drops_test.cpp.
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_puppet.h"

namespace Core {
class System;
}
namespace coop::host {
class Session;
}
namespace coop::net {
struct Event;
}

namespace CoopDrops {

using coop::protocol::Vec3;

// The actors that are drops: rupees, recovery hearts, magic jars, arrows,
// bombs, nuts, seeds and sticks to pick up. Ones the game made (no placement
// hash) are rolled per game and matched; placed ones (a bush's or a grass
// tuft's fixed loot, #102) lie in every game with the same hash and only
// their pickup is shared.
bool IsDrop(std::string_view name);

// A placed drop whose pickup this file shares (bush and grass loot). Not a
// placed silver rupee: the dungeon key path removes that one (docs/items.md).
bool SharesPlacedPickup(std::string_view name);

// One drop actor of one walk.
struct Seen {
    std::uint64_t address = 0;
    std::string name;
    Vec3 position{};
    // The placement hash of a placed drop (bush and grass loot: every game
    // has its own, same hash); 0 for one the game made.
    std::uint64_t hash = 0;
};

// A copy for the spawn director to make. `release`: it is made and known;
// the director lets go of its slot and leaves the actor alone.
struct Create {
    std::uint64_t key = 0;  // unique for this run
    std::string name;
    Vec3 position{};
    bool release = false;
};

// An actor of ours to delete.
struct Remove {
    std::uint64_t address = 0;
    std::string name;
    bool operator==(const Remove&) const = default;
};

class Sync {
public:
    // Two rolls this near in place and time are for the same thing.
    static constexpr float kMatchM = 3.0f;
    static constexpr std::uint64_t kMatchUs = 3'000'000;
    // Something we own this near, seen this recently, dropped it.
    static constexpr float kSourceM = 3.0f;
    static constexpr std::uint64_t kSourceUs = 5'000'000;
    // Gone from the list with our Link this near: picked up; this near: it
    // blinked out; further: unloaded here only.
    static constexpr float kTakeM = 2.5f;
    static constexpr float kNearM = 15.0f;
    static constexpr unsigned kGoneWalks = 2;
    // This many of our drops gone in one walk: the area was unloaded.
    static constexpr std::size_t kUnloadCount = 3;
    // A winner we have no actor for is created after this wait, so that our
    // own game's roll for the same thing can be kept as it instead.
    static constexpr std::uint64_t kAdoptWaitUs = 500'000;
    static constexpr std::uint64_t kCreateWaitUs = 5'000'000;  // then given up
    static constexpr std::uint64_t kDeleteWaitUs = 3'000'000;  // a delete that never came true
    static constexpr std::uint64_t kKeepUs = 60'000'000;       // a gone drop is remembered
    static constexpr std::uint64_t kTellAfterUs = 500'000;     // after a DROP_ASK
    // A pickup or blink-out is told this late, so that leaving the scene (or
    // more drops going: an unload) in the meantime can still call it off.
    static constexpr std::uint64_t kTakeHoldUs = 500'000;
    static constexpr unsigned kSendsPerSecond = 20;
    static constexpr std::size_t kMaxDrops = 256;

    // Network thread: DROP_SPAWN, DROP_TAKE, DROP_ASK. Applied on the next Tick.
    void Receive(const coop::protocol::Message& message);

    // Present thread, once per walk. `player` is our Link's place when the
    // walk saw it, `owned` the places of the shared things we own, `made` the
    // copies the director holds (key to actor).
    void Tick(const std::string& scene, std::uint8_t self, std::uint64_t now_us,
              const std::vector<Seen>& seen, const std::optional<Vec3>& player,
              const std::vector<Vec3>& owned, const std::map<std::uint64_t, std::uint64_t>& made,
              std::vector<coop::protocol::Message>& send, std::vector<Create>& creates,
              std::vector<Remove>& removes);

    void Reset();

    // How near our Link must be for a drop that left the list to count as
    // blinked out (kNearM; 0: never, only pickups are told).
    void SetNear(float metres) { near_m_ = metres; }

    // For tests and logs.
    std::size_t Live() const;       // drops this game shows or is making
    std::size_t Known() const { return drops_.size(); }
    std::size_t Refused() const { return refused_; }  // own rolls deleted
    std::size_t Adopted() const { return adopted_; }  // own rolls kept as another's
    std::optional<coop::protocol::ObjectId> IdOf(std::uint64_t address) const;
    std::vector<std::string> TakeNotes() { return std::exchange(notes_, {}); }

private:
    enum class State : std::uint8_t {
        live,   // lies in every game
        lost,   // another roll for the same thing stayed
        taken,  // picked up or blinked out
        away,   // not in our game (unloaded, or the create failed)
    };
    struct Drop {
        coop::protocol::ObjectId id;
        std::string name;
        Vec3 origin{};    // where it was announced: what rolls are matched by
        Vec3 position{};  // where it is now
        std::uint64_t time_us = 0;  // its maker's first sight
        bool owned = false;         // its maker owns what dropped it
        State state = State::live;
        std::uint64_t address = 0;  // our actor of it
        std::uint64_t got_us = 0;   // when we learned of it
        std::uint64_t wait_us = 0;  // before a create
        std::uint64_t key = 0;      // the director's, once a create was asked
        std::uint64_t asked_us = 0; // when
        bool made = false;          // the director reported it
        unsigned missed = 0;
        std::uint64_t done_us = 0;  // when it stopped being live
    };

    void Apply(std::uint8_t self, std::uint64_t now_us, std::vector<Remove>& removes);
    void Learn(const coop::protocol::DropSpawn& spawn, std::uint64_t now_us,
               std::vector<Remove>& removes);
    static bool Matches(const Drop& a, const Drop& b);
    static bool Beats(const Drop& a, const Drop& b);
    bool Beaten(const Drop& d) const;
    // `d` lost to `winner` (or was taken, with no winner): its actor goes to
    // the winner when that fits, else it is deleted.
    void Lose(Drop& d, Drop* winner, State state, std::uint64_t now_us,
              std::vector<Remove>& removes);
    void Delete(std::uint64_t address, const std::string& name, std::uint64_t now_us,
                std::vector<Remove>& removes);
    void Bind(Drop& d, std::uint64_t address);
    void Queue(coop::protocol::Message message);
    void Note(std::string text);
    // Placed drops (Seen::hash): a pickup here is told as a DROP_TAKE with a
    // placed id; one taken anywhere is deleted here whenever it shows up
    // while we stay in the scene.
    void TickPlaced(const std::vector<Seen>& seen, const std::optional<Vec3>& player,
                    std::uint64_t now_us, std::vector<Remove>& removes);
    struct PlacedDrop {
        std::uint64_t address = 0;
        std::string name;
        Vec3 position{};
        unsigned missed = 0;
    };
    std::map<std::uint64_t, PlacedDrop> placed_;               // hash: our actor of it
    std::set<std::uint64_t> placed_taken_;                     // hashes taken in this scene
    std::map<std::uint64_t, std::uint64_t> placed_deleting_;  // actor: when its delete was asked

    // A create that was called off before the director reported its actor.
    struct Ghost {
        std::string name;
        Vec3 position{};
        std::uint64_t since_us = 0;
    };

    std::string scene_;
    std::uint8_t self_ = 0;
    float near_m_ = kNearM;
    std::vector<Ghost> ghosts_;
    // Takes not told yet (kTakeHoldUs), with when the drop left the list.
    struct HeldTake {
        coop::protocol::DropTake take;
        std::uint64_t since_us = 0;
    };
    std::vector<HeldTake> held_;
    std::uint64_t last_now_ = 0;  // `now` never goes back inside Tick
    bool first_walk_ = true;
    std::set<std::uint64_t> old_;  // on the list at the first walk of a scene
    std::map<coop::protocol::ObjectId, Drop> drops_;
    std::map<std::uint64_t, coop::protocol::ObjectId> bound_;  // actor to drop
    std::map<std::uint64_t, std::pair<std::string, std::uint64_t>> deleting_;  // actor: name, since
    std::map<coop::protocol::ObjectId, std::uint64_t> gone_unknown_;  // taken before we knew it
    std::vector<std::pair<Vec3, std::uint64_t>> sources_;  // owned things: place, last seen
    std::uint64_t counter_ = 0;
    std::uint64_t next_key_ = 1;
    std::uint64_t tell_at_ = 0;  // answer a DROP_ASK then
    std::deque<coop::protocol::Message> outbox_;
    double tokens_ = 0.0;
    std::uint64_t tokens_at_ = 0;
    std::size_t refused_ = 0;
    std::size_t adopted_ = 0;
    std::vector<std::string> notes_;

    mutable std::mutex inbox_mutex_;
    std::vector<coop::protocol::Message> inbox_;
};

// The calc thread's side: deletes actors with the game's own delete request
// (0xa6490), each once, after checking that the address still holds a live
// actor of that name. Same cave contract as CoopPuppet::Director::Step.
class Remover {
public:
    static constexpr int kMaxCallsPerFrame = 8;

    // Present thread.
    void Want(std::uint64_t main_base, std::vector<Remove> removes);
    std::size_t TakeCount();  // deletes asked since the last call

    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call,
                                         std::uint64_t result, std::uint64_t now_ms);

private:
    bool Alive(CoopPuppet::Guest& guest, const Remove& r) const;

    std::mutex mutex_;
    std::uint64_t base_ = 0;         // guarded
    std::vector<Remove> removes_;    // guarded
    std::size_t count_ = 0;          // guarded
    std::set<std::uint64_t> asked_;  // calc thread
    std::vector<Remove> work_;
    std::size_t next_ = 0;
    int calls_ = 0;
    std::uint64_t work_base_ = 0;
};

// The hook: present thread, once per frame after the session's frame.
void Frame(Core::System& system, coop::host::Session* session,
           const std::optional<std::string>& scene, std::uint64_t server_now_us);
// Network thread, from the session's on_event.
void OnEvent(const coop::net::Event& event);

} // namespace CoopDrops
