// SPDX-License-Identifier: GPL-3.0-or-later
//
// Time of day, the same in every game (docs/time.md). No emulator code:
// coop_time.cpp reads the game's clock and sets it; this decides what to send
// and when to set. Tested in tests/lichii/time_test.cpp.
//
// - The host sends TIME_OF_DAY about once a second.
// - Another game moves its clock to the host's when it is more than
//   kDriftSeconds off (either way round midnight), at most once every
//   kSetEveryUs.
// - A game whose clock jumped by itself (the Sun's Song, a night's sleep, an
//   event) sends TIME_OF_DAY with the jump bit at once; every other game, the
//   host's too, takes it. A jump we caused by setting the clock is not sent.
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
    static constexpr std::uint32_t kDriftSeconds = 120;  // two in-game minutes
    static constexpr std::uint32_t kJumpSeconds = 1'800;  // half an hour in one read
    static constexpr std::uint64_t kSetEveryUs = 2'000'000;
    static constexpr std::uint64_t kQuietAfterSetUs = 3'000'000;  // our own set is no jump

    // Network thread: TIME_OF_DAY from the others. Applied on the next Tick.
    void Receive(const coop::protocol::Message& message);

    // Present thread, a few times a second. `now` is our clock (nullopt when
    // it cannot be read: no game, a load), `host` whether we host. Fills
    // `send`; returns the time to set our clock to, if any.
    std::optional<std::uint32_t> Tick(std::optional<std::uint32_t> now, bool host,
                                      std::uint64_t now_us,
                                      std::vector<coop::protocol::Message>& send);

    void Reset();

private:
    std::optional<std::uint32_t> last_;  // our clock at the last tick
    std::uint64_t last_at_ = 0;
    std::uint64_t sent_at_ = 0;
    bool sent_once_ = false;
    std::uint64_t set_at_ = 0;
    bool set_once_ = false;

    std::mutex inbox_mutex_;
    std::vector<coop::protocol::TimeOfDay> inbox_;
};

} // namespace CoopTime
