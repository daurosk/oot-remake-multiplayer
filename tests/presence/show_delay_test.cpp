// The delay another player is drawn with follows its samples' ages
// (coop/presence/show_delay.hpp, player report #37).

#include <doctest/doctest.h>

#include <cstdint>
#include <random>

#include "coop/presence/presence.hpp"
#include "coop/presence/show_delay.hpp"

namespace {

using coop::presence::ShowDelay;
namespace proto = coop::protocol;

constexpr std::uint64_t kFloor = 100'000;
constexpr std::uint64_t kStart = 10'000'000;

// 20 samples a second for `seconds`, each `age_us` (+ 0 to `jitter_us`) old
// when it comes; step() every 16 ms. Returns the last delay; `backwards` is
// set when the shown time (now - delay) ever went back.
std::uint64_t feed(ShowDelay& d, std::uint64_t& now, double seconds, std::uint64_t age_us,
                   std::uint64_t jitter_us, bool* backwards = nullptr, std::uint32_t seed = 1) {
  std::mt19937 rng(seed);
  std::uniform_int_distribution<std::uint64_t> jitter(0, jitter_us);
  const std::uint64_t end = now + static_cast<std::uint64_t>(seconds * 1e6);
  std::uint64_t next_sample = now;
  std::uint64_t last_shown = 0;
  std::uint64_t delay = 0;
  for (; now < end; now += 16'000) {
    while (next_sample <= now) {
      const std::uint64_t age = age_us + jitter(rng);
      d.seen(now - age, now);
      next_sample += 50'000;
    }
    delay = d.step(now, kFloor);
    const std::uint64_t shown = now - delay;
    if (backwards != nullptr && shown < last_shown) *backwards = true;
    last_shown = shown;
  }
  return delay;
}

}  // namespace

TEST_CASE("show delay: a LAN keeps the old 100 ms") {
  ShowDelay d;
  std::uint64_t now = kStart;
  CHECK(d.step(now, kFloor) == kFloor);
  CHECK(feed(d, now, 5.0, 5'000, 10'000) == kFloor);
}

TEST_CASE("show delay: 250 ms old samples are drawn behind their age plus a send interval") {
  ShowDelay d;
  std::uint64_t now = kStart;
  bool backwards = false;
  const std::uint64_t delay = feed(d, now, 5.0, 200'000, 80'000, &backwards);
  // The 90th percentile age is about 272 ms; plus the 60 ms margin.
  CHECK(delay >= 300'000);
  CHECK(delay <= 350'000);
  CHECK_FALSE(backwards);
}

TEST_CASE("show delay: it grows at half speed and shrinks slowly, never past its aim, and never goes back") {
  ShowDelay d;
  std::uint64_t now = kStart;
  bool backwards = false;
  feed(d, now, 0.5, 300'000, 0, &backwards);  // the aim jumps to 360 ms at the 8th sample (0.35 s)
  const std::uint64_t early = d.current_us();
  CHECK(early > kFloor);
  CHECK(early < 360'000);  // still on its way
  CHECK(feed(d, now, 2.0, 300'000, 0, &backwards) == 360'000);
  // The line gets better: down by at most 5 % of the time passed.
  const std::uint64_t before = d.current_us();
  feed(d, now, 2.0, 20'000, 0, &backwards);  // the old ages leave the window after ~1.5 s
  CHECK(d.current_us() >= before - 101'000);
  CHECK(d.current_us() < before);
  CHECK(feed(d, now, 10.0, 20'000, 0, &backwards) == kFloor);
  CHECK_FALSE(backwards);
}

TEST_CASE("show delay: a stalled sample does not raise it past kMaxUs, and times of 0 are left out") {
  ShowDelay d;
  std::uint64_t now = kStart;
  for (int i = 0; i < 40; ++i) d.seen(now - 5'000'000, now);
  CHECK(d.target(kFloor) == ShowDelay::kMaxUs);
  ShowDelay e;
  for (int i = 0; i < 40; ++i) e.seen(0, now);
  CHECK(e.target(kFloor) == kFloor);
}

TEST_CASE("show delay: Presence draws each player with its own delay") {
  coop::presence::Presence p;
  proto::Welcome w;
  w.player_number = 1;
  p.on_welcome(w);
  proto::PlayerJoined far_join;
  far_join.player_number = 2;
  far_join.nickname = "Zelda";
  REQUIRE(p.on_joined(far_join));
  proto::PlayerJoined near_join;
  near_join.player_number = 3;
  near_join.nickname = "Saria";
  REQUIRE(p.on_joined(near_join));

  std::uint64_t now = kStart;
  std::uint32_t seq = 0;
  for (int i = 0; i < 100; ++i, now += 50'000) {
    for (std::uint8_t n : {std::uint8_t{2}, std::uint8_t{3}}) {
      proto::PlayerSample s;
      s.player_number = n;
      const std::uint64_t age = n == 2 ? 250'000 : 10'000;
      s.time = {now - age, ++seq};
      s.position = proto::Vec3{static_cast<float>(i), 0.0f, 0.0f};
      p.on_sample(s);
      p.seen(n, s.time.server_time_us, now);
    }
    p.shown(2, now);
    p.shown(3, now);
  }
  CHECK(p.show_delay_us(2) == 310'000);
  CHECK(p.show_delay_us(3) == kFloor);
  CHECK(p.show_delay_us(9) == kFloor);  // not in the session
  // Player 2 is drawn between two samples now, not held past the newest.
  const auto pose = p.shown(2, now);
  REQUIRE(pose);
  CHECK(pose->how == coop::presence::How::interpolated);
  CHECK(pose->at_us == now - 310'000);
}
