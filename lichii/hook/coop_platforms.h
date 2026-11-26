// Moving platforms in step for everyone (docs/objects.md, "Moving platforms";
// issues 196, 194). The game's moving floors, lifts and turning floors are
// moved by delta time: each is driven by one clock word (an elapsed time, a
// phase or an angle) that steps by the frame's time and decides where the
// platform is. Share that one word, with the server time it was read at,
// and every game shows the platform at the same place.
//
// Which word it is is found in the running game, not hard-coded: the
// platforms' drivers (GroundControlActor_WaveLoop / _Rotate / _ResidentMoveTo
// / _SimpleMoveTo and the platform actors themselves) are copied a few times
// a second, and a float that steps steadily with time (the same rate in
// three looks in a row) is the clock. Its rate is measured too, so seconds,
// frames and angles all work.
//
// One game in a scene owns each platform: the one that has sent it the
// longest without a kOwnerQuietUs break (a game that comes later follows;
// two that began together: the lower player number). The owner sends its word every kSendEveryUs,
// and at once when it jumped (a switch started it, it looped). The others
// put value + rate x (server now - sent server time) into their own word,
// wrapped by the cycle when one was seen, and only when theirs is more than
// kDriftS seconds of the clock off: no write every frame.
//
// No emulator code. Tested in tests/lichii/platforms_test.cpp.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <utility>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopPlatforms {

// A platform's driver or a platform actor whose clock may be shared.
bool IsDriver(std::string_view name);

// Where a clock word is, as PLATFORM_CLOCK keys it (coop::protocol::PlatformTick):
// region 0 the actor (+0 .. +0x600), region 2 + n the component in table
// entry n (+0 .. +0x200), kRegionAsFrame the AS slot 0 frame.
inline constexpr std::uint64_t kRegionActor = 0;
inline constexpr std::uint64_t kRegionComponent = 2;
inline constexpr std::uint64_t kRegionAsFrame = coop::protocol::kPlatformAsFrameRegion;
inline constexpr std::uint64_t Key(std::uint64_t region, std::uint64_t offset) {
    return (region << 32) | offset;
}
std::string Where(std::uint64_t key);

// The float words of one look at a driver, by key (offsets in steps of 4).
using Floats = std::map<std::uint64_t, float>;

// Splits 8-byte words (as SignProbe::Read keys them: region << 32 | offset)
// into the two floats each holds. Words that look like heap pointers, and
// the actor's own place and turn (+0x31c .. +0x34b, moved by the game from
// the clock), are left out.
Floats FloatsOf(const std::map<std::uint64_t, std::uint64_t>& words);

// The wave movers' phase (GroundControlActor_WaveLoop, deku-switches-b, issue 196; read from main, build
// e0f3ccfaf50065ae): the controller's setup (main+0x2fff730) reads WaveCycle_Linear, WaveRadius_Linear,
// WaveCycle_Angular and WaveRadius_Angular from the blackboard into a wave block (main+0x11d07b0: cycle at +8,
// radius at +20, angular cycle at +32, radius at +44, each three floats) and puts the phase angles after them
// (+56 linear x, y, z, then angular), each from the place it starts at, wrapped into [0, 2 pi). Each frame a phase
// goes on by 2 pi / its cycle a second, and the floor stands at its start + radius x sin(phase). Both games start
// from the same place, so the phase is the one value that differs (when each loaded the room). A word that steps at
// 2 pi / the float kWavePhaseFromCycle bytes before it is that phase: the finder takes it before any other, with
// its loop 2 pi.
inline constexpr std::uint64_t kWavePhaseFromCycle = 48;  // phase (+56) - cycle (+8)
inline constexpr float kTwoPi = 6.28318531f;
// The cycle (2 pi) when `key` in `floats` steps at `rate` as a wave phase, else nothing. A key in a GroundControl
// node region (below) is one by the node's layout alone (NodePhaseCycle).
std::optional<float> WavePhaseCycle(const Floats& floats, std::uint64_t key, float rate);

// #196 (room-actor-replay-0116-2, read from the recompiled main of build e0f3ccfaf50065ae): the wave block is not in
// the actor or its components. It is in the mover's GroundControl node, an object of its own: [node] = main +
// kWaveVtable, [node + 8] = the actor (the node's per-frame step main+0x2fff930 reads [[node + 8] + 0x270] + 0x118,
// the actor's logic), the block at node + 0xc0 (its destructor main+0x3000e10 stores the block's own vtable there).
// main+0x2fff730 sets the block up (main+0x11d07b0); each frame main+0x2fff930 calls the block's step main+0x386c160,
// which adds frame time x 2 pi / cycle to each phase and wraps it into [0, 2 pi): linear x, y, z at node + 0xf8,
// 0xfc, 0x100 (cycle 48 bytes before), angular x, y, z at node + 0x104, 0x108, 0x10c (cycle 36 bytes before). The
// floor the controller moves (Obj_Deku_MoveFloor_A_01) is put at its start + radius x sin(phase) through
// main+0x3c42460: every word of the floor is worked out from this phase, which is why none of them held a write.
// GroundControlActor_Rotate's node is the same kind (vtable kRotateVtable, setup main+0x2ffea40 -> main+0x11d0d80,
// step main+0x2ffed30 -> main+0x386bb00): its angle at node + 0xd4 goes on by its speed (node + 0xd0, eased to the
// set speed at node + 0xc8) a second, wrapped into [0, 2 pi). Region kRegionWaveNode / kRegionRotateNode keys a word
// by its offset in the node (kNodeFrom .. kNodeTo).
inline constexpr std::uint64_t kRegionWaveNode = coop::protocol::kPlatformWaveNodeRegion;
inline constexpr std::uint64_t kRegionRotateNode = coop::protocol::kPlatformRotateNodeRegion;
namespace GroundNode {
inline constexpr std::uint64_t kWaveVtable = 0x4b6ad78;
inline constexpr std::uint64_t kRotateVtable = 0x4b6acd8;
inline constexpr std::uint64_t kActor = 0x8;
inline constexpr std::uint64_t kFrom = coop::protocol::kPlatformNodeFrom;  // 0xc0, the block
inline constexpr std::uint64_t kTo = coop::protocol::kPlatformNodeTo;      // 0x140
inline constexpr std::uint64_t kWavePhaseFirst = 0xf8;
inline constexpr std::uint64_t kWavePhaseLast = 0x10c;
inline constexpr std::uint64_t kWaveAngularFirst = 0x104;
inline constexpr std::uint64_t kRotateAngle = 0xd4;
inline constexpr std::uint64_t kRotateSpeed = 0xd0;
// The search from the actor (no hook names this node): every pointer word of the actor, its component table, its
// components and what they point to, breadth first, kMaxDepth objects deep, at most kMaxObjects objects of
// kObjectWords words. An object whose first word is one of the two vtables and whose [+8] is the actor is the node.
inline constexpr std::size_t kMaxDepth = 4;
inline constexpr std::size_t kMaxObjects = 4096;
inline constexpr std::size_t kObjectWords = 0x200 / 8;
inline constexpr std::size_t kActorWords = 0x600 / 8;
struct Hit {
    std::uint64_t node = 0;
    std::uint64_t region = 0;  // kRegionWaveNode or kRegionRotateNode
    std::string path;          // how it was reached, for the log ("actor+0x270 > +0x118 > ...")
};
using ReadWord = std::function<std::optional<std::uint64_t>(std::uint64_t)>;
// The node's region when `node` is a live GroundControl node of `actor`, else 0.
std::uint64_t RegionOf(const ReadWord& read, std::uint64_t node, std::uint64_t actor, std::uint64_t main);
// The search spread over frames (B #29 N1): a whole search reads up to kMaxObjects x kObjectWords words (about
// 262k), too many for one frame of the present thread. Step() goes on where the last call stopped and reads at most
// `budget` words (+1: a node check reads two), so the walk calls it with kReadsPerStep each time until it is Done().
// Reads only; the words read are the same as one whole search's. The walk reads a driver once a look (kLookEveryUs,
// 250 ms) and already reads a few thousand words of it then (SignProbe); 8192 more is of that order, and a whole
// search takes at most 32 looks (8 s) instead of one 262k-word frame.
inline constexpr std::size_t kReadsPerStep = 8192;
class Search {
public:
    Search(std::uint64_t actor, std::uint64_t main);
    // The node once found (then on every later call too), else nothing yet.
    std::optional<Hit> Step(const ReadWord& read, std::size_t budget);
    bool Done() const { return done_; }       // found, or nothing left to look at
    std::size_t Looked() const { return looked_; }  // objects looked at so far
    std::size_t Reads() const { return reads_; }    // words read so far

private:
    struct Object {
        std::uint64_t address = 0;
        std::size_t depth = 0;
        std::size_t parent = 0;  // index in objects_
        std::uint64_t via = 0;   // the word's offset in the parent
    };
    std::string PathOf(std::size_t i) const;
    std::uint64_t actor_ = 0;
    std::uint64_t main_ = 0;
    std::vector<Object> objects_;
    std::set<std::uint64_t> seen_;
    std::size_t i_ = 0;        // the object being read
    std::size_t j_ = 0;        // its next word
    bool checked_ = false;     // objects_[i_] was checked for the node
    bool done_ = false;
    std::size_t looked_ = 0;
    std::size_t reads_ = 0;
    std::optional<Hit> hit_;
};
// Finds `actor`'s node in one go (tests, tools); `looked` gets the number of objects read. Reads only.
std::optional<Hit> Find(const ReadWord& read, std::uint64_t actor, std::uint64_t main, std::size_t* looked = nullptr);
// The node's words for SignProbe-style keys: Key(region, offset) for offset kFrom .. kTo, 8 bytes each.
std::map<std::uint64_t, std::uint64_t> Words(const ReadWord& read, const Hit& hit);
}  // namespace GroundNode
// The loop (2 pi) of a node word that is a phase or angle by the node's layout, else nothing: a wave phase whose
// cycle word is over 0, the rotate angle while its speed is not 0.
std::optional<float> NodePhaseCycle(const Floats& floats, std::uint64_t key);

// Finds the clock word of one driver from looks at it.
class ClockFinder {
public:
    static constexpr std::uint64_t kLookEveryUs = 250'000;
    static constexpr std::size_t kLooks = 4;        // three steps
    static constexpr float kMinRate = 0.05f;        // per second
    static constexpr float kMaxRate = 2000.0f;
    static constexpr float kRateSlack = 0.08f;      // the steps' rates agree this well (of the rate)
    static constexpr float kMaxValue = 1.0e6f;
    static constexpr float kWaveResetSlack = 0.1f;  // a wave phase that steps back to under this was set to its start
    static constexpr int kKeepAfter = 3;            // agreeing checks before a word is the clock
    static constexpr int kDropAfter = 6;            // failed checks in a row before it is let go
    static constexpr int kMaxStalled = 40;          // looks standing still (a pause: 10 s) before it is let go

    struct Clock {
        std::uint64_t key = 0;
        float rate = 0.0f;    // per second
        float value = 0.0f;   // at the last look
        float cycle = 0.0f;   // where it loops back (0: not seen)
        std::uint64_t at_us = 0;  // our time of the last look
    };

    // One look at the driver; `now_us` is our own clock (steady), not the server's.
    void Add(std::uint64_t now_us, const Floats& floats);
    // The clock, once one word kept its rate for kKeepAfter looks; nothing
    // while it stands still (the game paused or hitched).
    std::optional<Clock> Found() const;
    // The clock jumped in the last look (looped or a switch reset it).
    bool Jumped() const { return jumped_; }
    // #196 (room-actor-replay-0116 run, review-room-actor-replay-r1): words dropped for good as no wave phase, with why,
    // since the last call (for the log). A wave mover's phase is wrapped into [0, 2 pi) by the game's own setup
    // (kWavePhaseFromCycle) and only goes on: a word that matched the cycle test but stood outside [0, 2 pi) (the Deku
    // Tree water room's floor c+0x60+0x1d8 at -26.4, worked out again every frame) or went back other than by looping
    // (its WaveLoop actor+0x3a8: 0.461 -> -0.032, a wave's offset, not its phase) is no clock: never chosen again.
    struct Dropped {
        std::uint64_t key = 0;
        float value = 0.0f;
        const char* why = "";
    };
    std::vector<Dropped> TakeDropped() { return std::exchange(dropped_, {}); }
    bool Banned(std::uint64_t key) const { return banned_.count(key) != 0; }
    // A looped clock's value is one the game can hold: inside [0, cycle) (a little slack for float steps).
    static bool InLoop(float value, float cycle);
    // The kept clock's word while it stands still (the game paused or
    // hitched; Found() is empty then), else nothing.
    std::optional<std::uint64_t> StalledKey() const;
    void Reset();
    // room-objects: only a word known to be a clock may be chosen: a wave mover's phase (WavePhaseCycle) or the AS
    // slot 0 frame. Any other stepping float was a guess, and runs showed what it picked: the Deku Tree lifts' frame
    // counter (written back by the game: "do not hold"), the water-room controller's place, a Jabu-Jabu floor's
    // word at -0.066 a second. A guessed word is still watched (logs), never sent or written.
    static bool Trusted(std::uint64_t key, bool wave);

private:
    struct Candidate {
        int good = 0;
        int bad = 0;
        int stalled = 0;     // looks in a row it stood still (the game paused)
        float rate = 0.0f;
        float top = 0.0f;    // the highest value seen
        float cycle = 0.0f;
        bool wave = false;   // a wave mover's phase (WavePhaseCycle): its loop is known
    };
    struct Look {
        std::uint64_t at_us = 0;
        Floats floats;
    };
    std::deque<Look> looks_;
    std::map<std::uint64_t, Candidate> candidates_;
    std::optional<std::uint64_t> chosen_;
    bool jumped_ = false;
    std::set<std::uint64_t> banned_;   // #196: never a clock (see Dropped)
    std::vector<Dropped> dropped_;
};

// The value a clock sent at `sent_us` has at `now_us` (both server time):
// value + rate x elapsed, looped by `cycle` when it is over 0. A packet from
// the future (clocks a little apart) counts as elapsed 0.
float Advance(float value, float rate, float cycle, std::uint64_t sent_us, std::uint64_t now_us);

// How far apart two clock values are in seconds of the clock, the short way
// round when there is a cycle.
float DriftSeconds(float ours, float theirs, float rate, float cycle);

// Share and follow the clocks of the drivers near our Link.
class PlatformSync {
public:
    static constexpr std::uint64_t kSendEveryUs = 1'000'000;
    static constexpr std::uint64_t kOwnerQuietUs = 3'000'000;  // a lower player number this recent owns it
    static constexpr std::uint64_t kStaleUs = 5'000'000;       // a tick older than this is not used
    static constexpr std::uint64_t kWriteEveryUs = 500'000;    // per platform
    static constexpr float kDriftS = 0.1f;                      // seconds of the clock
    static constexpr float kNearM = 40.0f;
    static constexpr std::size_t kMaxWatched = 8;               // the nearest drivers
    static constexpr int kMaxNoHold = 4;  // writes that did not hold (off again at once) before we stop
    // A pause menu stops our game's clocks but not the shared one (#35): an owner whose clock stood still
    // and goes on from where it stopped (within kResumeS of the clock) puts it where its last sent tick has
    // reached by now, once, before it sends again; a follower whose word was moving and stands still writes
    // nothing (and counts no write as not held) until it moves again.
    static constexpr float kResumeS = 1.0f;

    struct Driver {
        std::uint64_t address = 0;
        std::uint64_t hash = 0;
        std::string name;
        coop::protocol::Vec3 position{};
        // Issue 194: the game's own ride state, the platform AI's bool plug RideOnPlayer (set every frame from a
        // physics contact with a player; coop_lift_ride.h). Unset: the actor has no such plug.
        std::optional<bool> ride_plug;
        // Another player stands on its footprint (their samples): what else a plug of 1 can mean.
        bool other_on = false;
    };
    // Issue 194: our Link rides this platform, so a write of its clock would move him with it (the
    // platform jumped, and bounced back each time the game took a write back). The plug RideOnPlayer
    // is the game's word that a player is on it, but it is also 1 while another player's puppet rides
    // our copy (CoopLiftRide sets it): a plug of 1 is ours unless another player is on the footprint
    // and our Link is not (a platform wider than the footprint: our Link on its far edge is still
    // ours). With no plug the footprint alone. A GroundControl controller is not where its floor is:
    // only its plug.
    static constexpr float kRideRadiusM = 1.8f;  // as CoopLiftRide's footprint
    static constexpr float kRideBelowM = 0.5f;
    static constexpr float kRideAboveM = 2.5f;
    // The plug is 0 while our Link is in the air (a jump on the platform): still riding this long after
    // the last look that said so, so no write moves the platform from under his landing.
    static constexpr std::uint64_t kRideGraceUs = 1'000'000;
    static bool OnFootprint(const coop::protocol::Vec3& platform, const coop::protocol::Vec3& who);
    // room-objects (coop_room_objects.h): why this driver is moved by its own logic (a switch, sensor or event input
    // in the room graph, or a rider plug: a live RideOnPlayer, which CoopLiftRide sets for the other players), so
    // its clock is neither sent nor written; nothing when it runs free.
    static std::optional<std::string> OwnLogic(const std::string& scene, const Driver& d);
    static bool OurLinkRides(const Driver& d, const coop::protocol::Vec3& player);
    // The 8-byte words of a driver (SignProbe::Read), plus its AS slot 0
    // frame under Key(kRegionAsFrame, 0) when it has one. Empty: not there.
    using ReadDriver = std::function<std::map<std::uint64_t, std::uint64_t>(const Driver&)>;

    struct Write {
        std::uint64_t address = 0;  // the driver
        std::uint64_t hash = 0;
        std::uint64_t key = 0;
        float value = 0.0f;
        float was = 0.0f;
    };
    struct Out {
        std::vector<coop::protocol::Message> send;
        std::vector<Write> writes;
        std::vector<std::string> notes;
    };

    // Network thread (the glue locks): another game's PLATFORM_CLOCK.
    void Receive(const coop::protocol::PlatformClock& message);

    // Present thread, a few times a second. `steady_us` is our own clock,
    // `server_us` the server's (coop::net clock offset). `probe_only`: find
    // and send, never write (LICHII_COOP_PLATFORMS=probe).
    void Tick(const std::string& scene, std::uint8_t self, std::uint64_t steady_us,
              std::uint64_t server_us, const std::vector<Driver>& drivers,
              const std::optional<coop::protocol::Vec3>& player, const ReadDriver& read,
              bool probe_only, Out& out);

    void Reset();

    // For tests and logs.
    std::uint8_t OwnerOf(std::uint64_t hash, std::uint8_t self, std::uint64_t server_us) const;
    std::size_t Writes() const { return writes_; }

private:
    struct Heard {
        std::uint8_t player = 0;
        std::uint64_t got_us = 0;   // server time we took it
        std::uint64_t sent_us = 0;  // the sender's server time
        std::uint64_t since_us = 0; // when its run of sends began
        coop::protocol::PlatformTick tick;
    };
    struct State {
        ClockFinder finder;
        std::uint64_t last_look_us = 0;
        std::uint64_t last_send_us = 0;
        std::uint64_t last_write_us = 0;
        std::uint64_t since_us = 0;  // our run of sends began (server time); 0: not sending
        bool logged_clock = false;
        bool logged_follow = false;
        int no_hold = 0;
        bool gave_up = false;
        std::optional<float> written;  // what we wrote last, to see whether it held
        // #35, the owner: the last tick we sent (server time), and the value our clock stood still at.
        std::optional<coop::protocol::PlatformTick> sent;
        std::uint64_t sent_us = 0;
        std::optional<float> paused_at;
        // #35, a follower: our word at the last look (key, value) and whether it was seen moving.
        std::uint64_t mine_key = 0;
        std::optional<float> mine_was;
        bool mine_moved = false;
        // Issue 194: a write was held back while our Link rode it (logged once a ride).
        bool held_for_ride = false;
        std::uint64_t ride_until_us = 0;  // steady time: riding until then (kRideGraceUs after the last look)
    };

    std::string scene_;
    std::map<std::uint64_t, std::map<std::uint8_t, Heard>> heard_;  // by hash, then player
    std::map<std::uint64_t, State> states_;                          // by hash
    std::size_t writes_ = 0;
    std::set<std::uint64_t> told_own_logic_;  // room-objects: logged once a scene
};

}  // namespace CoopPlatforms
