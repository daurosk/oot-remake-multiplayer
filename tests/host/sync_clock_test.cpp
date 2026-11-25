#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <random>

#include "coop/host/sync_clock.hpp"

using namespace coop::host;

namespace {

constexpr std::uint64_t kMs = 1'000;
constexpr std::uint64_t kFrameUs = 33'334;  // the game's 30 frames a second

// One game: its own steady clock (an offset and a drift against true time), a link with a fixed one-way delay
// plus a random jitter each way, a PING a second.
struct Game {
  Game(std::uint64_t offset_us, double drift_ppm, std::uint64_t one_way_us, std::uint64_t jitter_us, std::uint32_t seed)
      : offset(offset_us), drift(drift_ppm), one_way(one_way_us), jitter(jitter_us), rng(seed) {}

  std::uint64_t local(std::uint64_t true_us) const {
    return offset + true_us + static_cast<std::uint64_t>(static_cast<double>(true_us) * drift / 1.0e6);
  }
  std::uint64_t wobble() { return jitter ? rng() % (jitter + 1) : 0; }
  // A PING sent at true time `t`; the server's clock is true time.
  void ping(std::uint64_t t) {
    const std::uint64_t up = one_way + wobble();
    const std::uint64_t down = one_way + wobble();
    clock.Add(local(t), t + up, local(t + up + down));
  }
  std::uint64_t server_at(std::uint64_t true_us) { return clock.At(local(true_us)); }

  std::uint64_t offset;
  double drift;
  std::uint64_t one_way;
  std::uint64_t jitter;
  std::mt19937 rng;
  SyncClock clock;
};

std::uint64_t apart(std::uint64_t a, std::uint64_t b) { return a > b ? a - b : b - a; }

}  // namespace

TEST_CASE("sync clock: server time never goes back and never jumps when the best sample changes") {
  Game g(5'000'000'000, 40.0, 50 * kMs, 20 * kMs, 7);
  std::uint64_t last = 0;
  std::uint64_t worst_step = 0;
  for (std::uint64_t t = 0; t < 120'000 * kMs; t += kFrameUs) {
    if (t % (1000 * kMs) < kFrameUs) g.ping(t);
    if (!g.clock.Ready()) continue;
    const std::uint64_t now = g.server_at(t);
    if (last != 0 && t > 3'000 * kMs) {
      CHECK(now >= last);
      worst_step = std::max(worst_step, apart(now - last, kFrameUs));
    }
    last = now;
  }
  // a frame's step is a frame, within the slew (2 % of a frame) and the drift: no jump of the 20 ms jitter
  CHECK(worst_step <= kFrameUs * 2 / 100 + 10);
  CHECK(g.clock.DriftPpm() != 0.0);
  CHECK(g.clock.DriftPpm() > -200.0);
  CHECK(g.clock.DriftPpm() < 200.0);
}

TEST_CASE("sync clock: two games' event times differ by less than one frame plus the jitter, 20 to 200 ms") {
  const std::uint64_t kJitter = 10 * kMs;
  for (const std::uint64_t round_trip_ms : {20u, 50u, 100u, 200u}) {
    for (std::uint32_t seed = 1; seed <= 20; ++seed) {
      CAPTURE(round_trip_ms);
      CAPTURE(seed);
      // A: fast clock, the given round trip. B: slow clock, another offset, a short round trip (the host's own PC).
      Game a(123'456'789, 50.0, round_trip_ms * kMs / 2, kJitter, seed);
      Game b(987, -50.0, 2 * kMs, kJitter, seed + 1000);
      std::mt19937 rng(seed);
      const std::uint64_t phase_a = rng() % kFrameUs;  // the two games' frames are not in step
      const std::uint64_t phase_b = rng() % kFrameUs;
      // 30 s of play, then an event stamped for server time `when`, and a running event that started at `start`.
      const std::uint64_t when = 30'000 * kMs + rng() % (1000 * kMs);
      const std::uint64_t start = 20'000 * kMs;
      std::uint64_t fired_a = 0, fired_b = 0;
      std::uint64_t worst_clock = 0;
      for (std::uint64_t t = 0; t < 40'000 * kMs; t += 1 * kMs) {
        if (t % (1000 * kMs) == 0) {
          a.ping(t + 137 * kMs);
          b.ping(t + 611 * kMs);
        }
        // each game's frames: every kFrameUs from its phase
        if (t >= phase_a && (t - phase_a) % kFrameUs < 1 * kMs && a.clock.Ready()) {
          const std::uint64_t now = a.server_at(t);
          if (!fired_a && due(when, now)) fired_a = t;
        }
        if (t >= phase_b && (t - phase_b) % kFrameUs < 1 * kMs && b.clock.Ready()) {
          const std::uint64_t now = b.server_at(t);
          if (!fired_b && due(when, now)) fired_b = t;
        }
        // at the same true moment (after the clocks settled), the time of the running event in both games
        if (t > 25'000 * kMs && t % (500 * kMs) == 0) {
          SyncClock ca = a.clock, cb = b.clock;  // read without moving the games' own clocks
          worst_clock = std::max(worst_clock, apart(elapsed_us(start, ca.At(a.local(t))), elapsed_us(start, cb.At(b.local(t)))));
        }
      }
      REQUIRE(fired_a != 0);
      REQUIRE(fired_b != 0);
      CHECK(worst_clock < kJitter);                       // the clocks: within the jitter
      CHECK(apart(fired_a, fired_b) < kFrameUs + kJitter);  // the flip: one frame plus the jitter
      CHECK(apart(fired_a, when) < kFrameUs + kJitter);     // and no game waits longer than that for it
    }
  }
}

TEST_CASE("sync clock: playout delay follows the transit and its jitter") {
  Playout p;
  CHECK_FALSE(p.Ready());
  CHECK(p.DelayUs() == 0);
  std::mt19937 rng(3);
  for (std::uint64_t t = 0; t < 400; ++t) {
    const std::uint64_t sent = 1'000'000 + t * 50 * kMs;
    p.Seen(sent, sent + 40 * kMs + rng() % (10 * kMs));
  }
  CHECK(p.DelayUs() > 45 * kMs);   // behind the mean
  CHECK(p.DelayUs() < 70 * kMs);   // not far behind the worst (50 ms)
  CHECK(p.JitterUs() < 6 * kMs);
  CHECK(p.PlayAt(5'000'000) == 5'000'000 + p.DelayUs());
  // a clock a little ahead gives a transit under 0: no delay under 0
  Playout q;
  for (int i = 0; i < 20; ++i) q.Seen(2'000'000, 1'999'000);
  CHECK(q.DelayUs() == 0);
  CHECK(lead_us(40 * kMs, kFrameUs) == 40 * kMs + kFrameUs);
  CHECK(lead_us(900 * kMs, kFrameUs) == kMaxLeadUs);
  CHECK(elapsed_us(10, 5) == 0);
  CHECK(elapsed_us(10, 25) == 15);
}
