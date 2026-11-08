#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <vector>

#include "options.hpp"

using namespace coop::server;

TEST_CASE("command line: defaults, flags and mistakes") {
  std::string error;
  auto none = parse_command_line({}, error);
  REQUIRE(none.has_value());
  CHECK(none->port == 27510);
  CHECK(none->game_build_id.empty());
  CHECK(!none->run_for_ms.has_value());

  auto set = parse_command_line(
      {"--port", "4000", "--game-build", "abc", "--run-for-ms", "50"}, error);
  REQUIRE(set.has_value());
  CHECK(set->port == 4000);
  CHECK(set->game_build_id == "abc");
  CHECK(set->run_for_ms == 50u);

  for (std::vector<std::string> bad : std::vector<std::vector<std::string>>{
           {"--port"}, {"--port", "70000"}, {"--port", "12x"}, {"--port", "-1"},
           {"--game-build"}, {"--run-for-ms", "soon"}, {"--frobnicate"}}) {
    error.clear();
    CHECK(!parse_command_line(bad, error).has_value());
    CHECK(!error.empty());
  }
}
