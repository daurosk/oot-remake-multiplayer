#include <doctest/doctest.h>

#include <cstdint>
#include <limits>

#include "coop/net/clock.hpp"

using namespace coop::net;

TEST_CASE("clock sample: server ahead of the client") {
  // PING at 1000, server stamps 50500, PONG back at 2000.
  const auto s = clock_sample(1000, 50500, 2000);
  REQUIRE(s.has_value());
  CHECK(s->round_trip_us == 1000);
  CHECK(s->offset_us == 50500 - 1500);
}

TEST_CASE("clock sample: server behind the client") {
  const auto s = clock_sample(10000, 4000, 10400);
  REQUIRE(s.has_value());
  CHECK(s->round_trip_us == 400);
  CHECK(s->offset_us == 4000 - 10200);
}

TEST_CASE("clock sample: same clocks, odd round trip rounds down") {
  const auto s = clock_sample(100, 150, 201);
  REQUIRE(s.has_value());
  CHECK(s->round_trip_us == 101);
  CHECK(s->offset_us == 0);
}

TEST_CASE("clock sample: refused when the answer is before the question") {
  CHECK_FALSE(clock_sample(2000, 1500, 1999).has_value());
}

TEST_CASE("clock sample: refused when the offset does not fit") {
  constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
  CHECK_FALSE(clock_sample(0, kMax, 0).has_value());
  CHECK_FALSE(clock_sample(kMax, 0, kMax).has_value());
  // Large times that are close together are fine.
  const auto s = clock_sample(kMax - 10, kMax - 2, kMax);
  REQUIRE(s.has_value());
  CHECK(s->round_trip_us == 10);
  CHECK(s->offset_us == 3);
}

TEST_CASE("estimator: latest round trip, offset from the shortest one") {
  ClockEstimator e;
  CHECK_FALSE(e.has_estimate());
  CHECK_FALSE(e.round_trip_us().has_value());
  CHECK_FALSE(e.offset_us().has_value());

  e.add({3000, 900});
  e.add({1000, 1000});  // the best
  e.add({5000, 2500});
  CHECK(e.round_trip_us() == 5000u);
  CHECK(e.offset_us() == 1000);

  e.reset();
  CHECK_FALSE(e.has_estimate());
}

TEST_CASE("estimator: old samples leave the window") {
  ClockEstimator e;
  e.add({100, -7});  // best, but pushed out by the next eight
  for (int i = 0; i < static_cast<int>(ClockEstimator::kWindow); ++i) {
    e.add({static_cast<std::uint64_t>(1000 + i), i});
  }
  CHECK(e.offset_us() == 0);  // the 1000 us sample
  CHECK(e.round_trip_us() == 1000u + ClockEstimator::kWindow - 1);
}

TEST_CASE("apply offset: both ways and saturating") {
  constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
  CHECK(apply_offset(1000, 250) == 1250);
  CHECK(apply_offset(1000, -250) == 750);
  CHECK(apply_offset(100, -250) == 0);
  CHECK(apply_offset(kMax - 1, 5) == kMax);
  CHECK(apply_offset(5, std::numeric_limits<std::int64_t>::min()) == 0);
}
