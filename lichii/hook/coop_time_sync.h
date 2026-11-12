// SPDX-License-Identifier: GPL-3.0-or-later
//
// Time of day, the same in every game (docs/time.md). No emulator code:
// coop_time.cpp reads the game's clock and sets it; this decides what to send
// and when to set. Tested in tests/lichii/time_test.cpp.
//
// - The host sends TIME_OF_DAY about once a second.
// - Another game moves its clock to the host's once when it joins, when it
//   is more than kDriftSeconds off (either way round midnight). After that
//   only when it is more than kResyncSeconds off, its own clock has run for
//   kRunningForUs, the host's clock runs too (its last two TIME_OF_DAY moved
//   forward), and kSetEveryUs has passed since the last set (issue 159:
//   every set runs the game's time change, which makes the area's grass and
//   objects again and stops the clock for some seconds; with a two-minute
//   limit that stop put the guest past the limit again, so it set its clock,
//   and remade its world, every two or three seconds).
// - A game whose clock jumped by itself (the Sun's Song, a night's sleep, an
//   event) sends TIME_OF_DAY with the jump bit at once; every other game, the
//   host's too, takes it. A jump we caused by setting the clock is not sent.
// - Between the two (issue 192): a guest whose running clock is more than
//   kNudgeSeconds and at most kResyncSeconds off the host's running clock is
//   nudged: the clock's seconds word is written directly (no time change, so
//   no world remake), at most kNudgeStepSeconds at a time and once per
//   kNudgeEveryUs. If kNudgeTries nudges in a row do not bring the clocks
//   within kNudgeSeconds, the game keeps its own clock elsewhere and nudges
//   stop for the session (the hourly set still works).
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopTime {

// Seconds from a to b the short way round midnight (-43200 to 43199).
std::int32_t Ahead(std::uint32_t a, std::uint32_t b);

class Sync {
public:
    static constexpr std::uint64_t kSendEveryUs = 1'000'000;
    static constexpr std::uint32_t kDriftSeconds = 120;  // two in-game minutes: on joining, a jump
    static constexpr std::uint32_t kResyncSeconds = 3'600;  // one in-game hour: while playing
    static constexpr std::uint32_t kJumpSeconds = 1'800;  // half an hour in one read
    static constexpr std::uint64_t kSetEveryUs = 60'000'000;
    static constexpr std::uint64_t kRunningForUs = 2'000'000;  // our clock runs: no change, not indoors
    static constexpr std::uint64_t kQuietAfterSetUs = 3'000'000;  // our own set is no jump
    static constexpr std::uint32_t kNudgeSeconds = 60;  // half a real second: closer is in sync
    static constexpr std::uint32_t kNudgeStepSeconds = 600;  // ten in-game minutes per nudge at most
    static constexpr std::uint64_t kNudgeEveryUs = 1'000'000;
    static constexpr int kNudgeTries = 8;

    // Network thread: TIME_OF_DAY from the others. Applied on the next Tick.
    void Receive(const coop::protocol::Message& message);

    // Present thread, a few times a second. `now` is our clock (nullopt when
    // it cannot be read: no game, a load), `host` whether we host. Fills
    // `send`; returns the time to set our clock to, if any.
    std::optional<std::uint32_t> Tick(std::optional<std::uint32_t> now, bool host,
                                      std::uint64_t now_us,
                                      std::vector<coop::protocol::Message>& send);

    // Present thread, after Tick: the value to write into our clock's
    // seconds word (a nudge, issue 192), if any. Taken once.
    std::optional<std::uint32_t> TakeNudge();
    // A nudge could not be written (the clock is in a time change): forget it.
    void NudgeNotWritten();
    // Nudges stopped for this session (they did not hold).
    bool NudgesOff() const { return nudges_off_; }

    void Reset();

private:
    std::optional<std::uint32_t> last_;  // our clock at the last tick
    std::uint64_t last_at_ = 0;
    std::uint64_t sent_at_ = 0;
    bool sent_once_ = false;
    std::uint64_t set_at_ = 0;
    bool set_once_ = false;
    std::optional<std::uint64_t> running_since_;  // our clock moved forward on every tick since
    std::optional<std::uint32_t> host_last_;  // the host's last plain TIME_OF_DAY
    bool host_running_ = false;
    bool joined_ = false;  // the host's first plain TIME_OF_DAY was taken (or kept)  // its last two moved forward (not indoors, no time change)

    std::optional<std::uint32_t> nudge_;  // to write, taken by TakeNudge
    std::uint64_t nudge_at_ = 0;
    int nudge_tries_ = 0;  // nudges in a row without getting within kNudgeSeconds
    bool nudges_off_ = false;

    std::mutex inbox_mutex_;
    std::vector<coop::protocol::TimeOfDay> inbox_;
};

} // namespace CoopTime
