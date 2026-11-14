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
// The cycle (2 pi) when `key` in `floats` steps at `rate` as a wave phase, else nothing.
std::optional<float> WavePhaseCycle(const Floats& floats, std::uint64_t key, float rate);

// Finds the clock word of one driver from looks at it.
class ClockFinder {
public:
    static constexpr std::uint64_t kLookEveryUs = 250'000;
    static constexpr std::size_t kLooks = 4;        // three steps
    static constexpr float kMinRate = 0.05f;        // per second
    static constexpr float kMaxRate = 2000.0f;
    static constexpr float kRateSlack = 0.08f;      // the steps' rates agree this well (of the rate)
    static constexpr float kMaxValue = 1.0e6f;
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
    void Reset();

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

    struct Driver {
        std::uint64_t address = 0;
        std::uint64_t hash = 0;
        std::string name;
        coop::protocol::Vec3 position{};
    };
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
    };

    std::string scene_;
    std::map<std::uint64_t, std::map<std::uint8_t, Heard>> heard_;  // by hash, then player
    std::map<std::uint64_t, State> states_;                          // by hash
    std::size_t writes_ = 0;
};

}  // namespace CoopPlatforms
