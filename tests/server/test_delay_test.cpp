// TEST ONLY delay (server/test_delay.hpp): when each held packet may go on.

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "options.hpp"
#include "test_delay.hpp"

using namespace coop::server;

TEST_CASE("test delay: half the round trip each way without jitter") {
  DelayClock clock(TestDelay{250, 0, 1});
  CHECK(clock.due(0, 1'000'000) == 1'125'000);
  CHECK(clock.due(1, 1'000'000) == 1'125'000);
  CHECK(clock.due(0, 1'010'000) == 1'135'000);
  CHECK(clock.last(0) == 1'135'000);
  CHECK(clock.last(7) == 0);
}

TEST_CASE("test delay: jitter stays in its range and never reorders a lane") {
  DelayClock clock(TestDelay{250, 60, 3});
  std::uint64_t now = 5'000'000;
  std::uint64_t last_a = 0;
  std::uint64_t last_b = 0;
  bool varied = false;
  std::uint64_t first = 0;
  for (int i = 0; i < 2000; ++i) {
    now += 1000 + static_cast<std::uint64_t>(i % 7) * 500;  // 1 to 4 ms apart
    const std::uint64_t a = clock.due(10, now);
    const std::uint64_t b = clock.due(11, now);
    CHECK(a >= last_a);
    CHECK(b >= last_b);
    CHECK(a >= now + 125'000 - 60'000);
    // Later than its own draw only by waiting for the one before it.
    CHECK(a <= (std::max)(now + 125'000 + 60'000, last_a));
    if (i == 0) first = a - now;
    if (a - now != first) varied = true;
    last_a = a;
    last_b = b;
  }
  CHECK(varied);
}

TEST_CASE("test delay: jitter larger than the delay never goes before now") {
  DelayClock clock(TestDelay{0, 40, 9});
  for (std::uint64_t now = 1'000; now < 200'000; now += 1'000) {
    CHECK(clock.due(0, now) >= now);
  }
  clock.forget(0);
  CHECK(clock.last(0) == 0);
}

TEST_CASE("test delay: the queue gives items back in time order, ties in push order") {
  DelayQueue<std::string> queue;
  queue.push(300, "c");
  queue.push(100, "a");
  queue.push(300, "d");
  queue.push(200, "b");
  CHECK(queue.take_due(50).empty());
  CHECK(queue.take_due(200) == std::vector<std::string>{"a", "b"});
  CHECK(queue.size() == 2);
  queue.push(250, "x");
  queue.drop_if([](const std::string& s) { return s == "x"; });
  CHECK(queue.take_due(1000) == std::vector<std::string>{"c", "d"});
  CHECK(queue.size() == 0);
}

TEST_CASE("command line: --test-delay-ms and --test-jitter-ms") {
  std::string error;
  auto none = parse_command_line({}, error);
  REQUIRE(none.has_value());
  CHECK(!none->test_delay.on());

  auto set = parse_command_line({"--test-delay-ms", "250", "--test-jitter-ms", "60"}, error);
  REQUIRE(set.has_value());
  CHECK(set->test_delay.round_trip_ms == 250u);
  CHECK(set->test_delay.jitter_ms == 60u);
  CHECK(set->test_delay.on());

  CHECK(!parse_command_line({"--test-delay-ms"}, error).has_value());
  CHECK(!parse_command_line({"--test-delay-ms", "x"}, error).has_value());
  CHECK(!parse_command_line({"--test-jitter-ms", "10001"}, error).has_value());
  CHECK(!parse_command_line({"--relay", "--test-delay-ms", "100"}, error).has_value());
  CHECK(error.find("not --relay") != std::string::npos);
  CHECK(!parse_command_line({"--join", "1.2.3.4:27510", "--test-jitter-ms", "5"}, error).has_value());
}
