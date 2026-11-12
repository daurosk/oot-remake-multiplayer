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
  CHECK(none->max_half_open == 4u);

  auto set = parse_command_line(
      {"--port", "4000", "--game-build", "abc", "--run-for-ms", "50"}, error);
  REQUIRE(set.has_value());
  CHECK(set->port == 4000);
  CHECK(set->game_build_id == "abc");
  CHECK(set->run_for_ms == 50u);
  auto off = parse_command_line({"--max-half-open", "0"}, error);
  REQUIRE(off.has_value());
  CHECK(off->max_half_open == 0u);
  // The owner hold after a start (stray-hold): its default when not given, 0 turns it off.
  CHECK(!none->owner_hold_ms.has_value());
  auto hold = parse_command_line({"--owner-hold-ms", "0"}, error);
  REQUIRE(hold.has_value());
  CHECK(hold->owner_hold_ms == 0u);

  for (std::vector<std::string> bad : std::vector<std::vector<std::string>>{
           {"--port"}, {"--port", "70000"}, {"--port", "12x"}, {"--port", "-1"},
           {"--game-build"}, {"--run-for-ms", "soon"}, {"--owner-hold-ms"}, {"--owner-hold-ms", "x"},
           {"--max-half-open"}, {"--max-half-open", "-2"}, {"--frobnicate"}}) {
    error.clear();
    CHECK(!parse_command_line(bad, error).has_value());
    CHECK(!error.empty());
  }
}

TEST_CASE("command line: --bind and --allow") {
  std::string error;
  auto none = parse_command_line({}, error);
  REQUIRE(none.has_value());
  CHECK(!none->bind.has_value());
  CHECK(none->allowed.empty());

  auto set = parse_command_line({"--bind", "25.1.2.3", "--allow", "25.0.0.0/8,192.168.1.20"}, error);
  REQUIRE(set.has_value());
  CHECK(set->bind == 0x19010203u);
  REQUIRE(set->allowed.size() == 2);
  CHECK(set->allowed[0].first == 0x19000000u);
  CHECK(set->allowed[0].prefix == 8);
  CHECK(set->allowed[1].first == 0xC0A80114u);
  CHECK(set->allowed[1].prefix == 32);

  for (std::vector<std::string> bad : std::vector<std::vector<std::string>>{
           {"--bind"}, {"--bind", "25.0.0.0/8"}, {"--bind", "my-pc"}, {"--bind", "::1"},
           {"--allow"}, {"--allow", ""}, {"--allow", "25.1.2.3,"}, {"--allow", ",25.1.2.3"},
           {"--allow", "25.1.2.3,,26.1.2.3"}, {"--allow", "25.1.2.3;26.1.2.3"},
           {"--allow", "everyone"}}) {
    error.clear();
    CHECK(!parse_command_line(bad, error).has_value());
    CHECK(!error.empty());
  }
}

TEST_CASE("command line: --password and --password-env") {
  std::string error;
  auto none = parse_command_line({}, error);
  REQUIRE(none.has_value());
  CHECK(none->password.empty());
  CHECK(none->password_env.empty());

  auto set = parse_command_line({"--password", "hunter2"}, error);
  REQUIRE(set.has_value());
  CHECK(set->password == "hunter2");
  auto env = parse_command_line({"--password-env", "OOT_COOP_SERVER_PASSWORD"}, error);
  REQUIRE(env.has_value());
  CHECK(env->password_env == "OOT_COOP_SERVER_PASSWORD");

  for (std::vector<std::string> bad : std::vector<std::vector<std::string>>{
           {"--password"}, {"--password", ""}, {"--password", std::string(129, 'x')},
           {"--password", "tab\there"}, {"--password-env"}}) {
    error.clear();
    CHECK(!parse_command_line(bad, error).has_value());
    CHECK(!error.empty());
  }
  CHECK(password_problem(std::string(128, 'x')) == nullptr);
  CHECK(password_problem("") == nullptr);
}

TEST_CASE("command line: --pvp on or off, off by default") {
  std::string error;
  CHECK_FALSE(parse_command_line({}, error)->pvp);
  for (const char* on : {"on", "1", "yes"}) {
    auto line = parse_command_line({"--pvp", on}, error);
    REQUIRE(line.has_value());
    CHECK(line->pvp);
  }
  auto off = parse_command_line({"--pvp", "off", "--port", "4000"}, error);
  REQUIRE(off.has_value());
  CHECK_FALSE(off->pvp);
  CHECK(off->port == 4000);
  for (std::vector<std::string> bad : std::vector<std::vector<std::string>>{
           {"--pvp"}, {"--pvp", "maybe"}, {"--pvp", ""}}) {
    error.clear();
    CHECK(!parse_command_line(bad, error).has_value());
    CHECK(!error.empty());
  }
}
