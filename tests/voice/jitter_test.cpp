#include <doctest/doctest.h>

#include <cstdint>
#include <vector>

#include "coop/voice/jitter.hpp"

using namespace coop::voice;
using Kind = JitterBuffer::Kind;

namespace {

std::vector<std::uint8_t> payload(std::uint16_t sequence) {
  return {static_cast<std::uint8_t>(sequence & 0xff), static_cast<std::uint8_t>(sequence >> 8)};
}

void push(JitterBuffer& j, std::uint16_t sequence, std::uint64_t now) {
  const auto p = payload(sequence);
  j.push(sequence, false, p, now);
}

void push_end(JitterBuffer& j, std::uint16_t sequence, std::uint64_t now) {
  j.push(sequence, true, {}, now);
}

std::uint16_t played(const JitterBuffer::Out& o) {
  REQUIRE(o.payload.size() == 2);
  return static_cast<std::uint16_t>(o.payload[0] | (o.payload[1] << 8));
}

}  // namespace

TEST_CASE("voice jitter: a spurt waits for its target, then plays in order") {
  JitterBuffer j;
  CHECK(j.pop(0).kind == Kind::quiet);
  push(j, 10, 0);
  CHECK(j.pop(0).kind == Kind::quiet);  // one waiting, target 2
  push(j, 11, kFrameUs);
  auto a = j.pop(kFrameUs);
  REQUIRE(a.kind == Kind::frame);
  CHECK(played(a) == 10);
  CHECK(j.playing());
  push(j, 13, 2 * kFrameUs);  // out of order
  push(j, 12, 2 * kFrameUs);
  CHECK(played(j.pop(2 * kFrameUs)) == 11);
  CHECK(played(j.pop(3 * kFrameUs)) == 12);
  CHECK(played(j.pop(4 * kFrameUs)) == 13);
  CHECK(j.stats().spurts == 1);
}

TEST_CASE("voice jitter: one frame alone plays once it has waited the target") {
  JitterBuffer j;
  push(j, 1, 0);
  CHECK(j.pop(kFrameUs).kind == Kind::quiet);
  CHECK(j.pop(2 * kFrameUs).kind == Kind::frame);
}

TEST_CASE("voice jitter: a missing frame uses the next one's FEC, then concealment") {
  JitterBuffer j;
  push(j, 1, 0);
  push(j, 2, 0);
  push(j, 4, 0);
  push(j, 5, 0);
  CHECK(played(j.pop(0)) == 1);
  CHECK(played(j.pop(0)) == 2);
  auto fec = j.pop(0);  // 3 missing, 4 here
  CHECK(fec.kind == Kind::fec);
  CHECK(played(fec) == 4);
  CHECK(played(j.pop(0)) == 4);
  CHECK(played(j.pop(0)) == 5);
  CHECK(j.pop(0).kind == Kind::lost);  // 6 missing, nothing after
  CHECK(j.stats().concealed == 2);
}

TEST_CASE("voice jitter: a late frame is dropped") {
  JitterBuffer j;
  push(j, 1, 0);
  push(j, 2, 0);
  CHECK(played(j.pop(0)) == 1);
  CHECK(played(j.pop(0)) == 2);
  push(j, 1, 0);
  CHECK(j.stats().late == 1);
  push(j, 3, 0);
  push(j, 3, 0);
  CHECK(j.stats().repeated == 1);
}

TEST_CASE("voice jitter: the end frame closes the spurt; the next one waits again") {
  JitterBuffer j;
  push(j, 1, 0);
  push(j, 2, 0);
  push_end(j, 3, 0);
  CHECK(played(j.pop(0)) == 1);
  CHECK(played(j.pop(0)) == 2);
  CHECK(j.pop(0).kind == Kind::quiet);
  CHECK_FALSE(j.playing());
  CHECK(j.pop(0).kind == Kind::quiet);  // no concealment after the end
  push(j, 4, 100 * kFrameUs);
  CHECK(j.pop(100 * kFrameUs).kind == Kind::quiet);  // waits for its target again
  push(j, 5, 101 * kFrameUs);
  CHECK(played(j.pop(101 * kFrameUs)) == 4);
  CHECK(j.stats().spurts == 2);
}

TEST_CASE("voice jitter: too many losses in a row end the spurt") {
  JitterConfig c;
  c.max_lost = 3;
  JitterBuffer j(c);
  push(j, 1, 0);
  push(j, 2, 0);
  j.pop(0);
  j.pop(0);
  CHECK(j.pop(0).kind == Kind::lost);
  CHECK(j.pop(0).kind == Kind::lost);
  CHECK(j.pop(0).kind == Kind::quiet);  // the third ends it
  CHECK_FALSE(j.playing());
}

TEST_CASE("voice jitter: sequence numbers wrap") {
  JitterBuffer j;
  push(j, 65534, 0);
  push(j, 65535, 0);
  push(j, 0, 0);
  push(j, 1, 0);
  CHECK(played(j.pop(0)) == 65534);
  CHECK(played(j.pop(0)) == 65535);
  CHECK(played(j.pop(0)) == 0);
  CHECK(played(j.pop(0)) == 1);
}

TEST_CASE("voice jitter: the target grows with the arrival jitter") {
  JitterBuffer steady;
  for (std::uint16_t i = 0; i < 100; ++i) push(steady, i, i * kFrameUs);
  CHECK(steady.jitter_us() < 1000.0);
  JitterBuffer bumpy;
  for (std::uint16_t i = 0; i < 100; ++i) {
    // Every other frame 30 ms late.
    push(bumpy, i, i * kFrameUs + (i % 2 == 1 ? 30'000u : 0u));
  }
  CHECK(bumpy.jitter_us() > 15'000.0);
  // A new spurt takes the larger target.
  JitterBuffer j;
  for (std::uint16_t i = 0; i < 100; ++i) {
    push(j, i, i * kFrameUs + (i % 2 == 1 ? 30'000u : 0u));
    j.pop(i * kFrameUs);
  }
  push_end(j, 100, 100 * kFrameUs);
  for (int k = 0; k < 20; ++k) j.pop(100 * kFrameUs);
  push(j, 101, 200 * kFrameUs);
  CHECK(j.target() > 2);
  CHECK(j.target() <= JitterConfig{}.max_target);
}

TEST_CASE("voice jitter: a stall drops the oldest and a rejoin starts over") {
  JitterBuffer j;
  for (std::uint16_t i = 0; i < 40; ++i) push(j, i, 0);
  CHECK(j.queued() == static_cast<std::size_t>(JitterConfig{}.max_queued));
  CHECK(j.stats().overflow == 15);
  CHECK(played(j.pop(0)) >= 15);
  // Quiet, then the speaker's game counts from 0 again.
  j.reset();
  push(j, 5000, 0);
  push(j, 5001, 0);
  j.pop(0);
  j.pop(0);
  push_end(j, 5002, 0);
  j.pop(0);
  push(j, 0, 0);
  push(j, 1, 0);
  CHECK(played(j.pop(0)) == 0);
}
