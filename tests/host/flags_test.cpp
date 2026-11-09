#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include "coop/host/flags.hpp"

using namespace coop::host;

TEST_CASE("flag hash: Murmur3 x86 32-bit reference vectors, seed 0") {
  CHECK(flag_hash("") == 0x00000000u);
  CHECK(flag_hash("test") == 0xba6bd213u);
  CHECK(flag_hash("abc") == 0xb3dd93fau);
  CHECK(flag_hash("hello") == 0x248bfa47u);
  CHECK(flag_hash("Hello, world!") == 0xc0363e43u);
  CHECK(flag_hash("The quick brown fox jumps over the lazy dog") == 0x2e4ff723u);
}

TEST_CASE("flag hash: names the game was seen to use") {
  // From the stage 2 map (integration/docs/player-state-map.md): constants in
  // the game's code and flags in its table.
  CHECK(flag_hash("EventSkip_00") == 0xb9283e8au);
  CHECK(flag_hash("HavePlayedEvent.Opening_Introduction") == 0x15b67d0eu);
  CHECK(flag_hash("HavePlayedEvent.Dm_Kokiri_0030") == 0xb041ae87u);
  CHECK(flag_hash("HavePlayedEvent.Opening_LoadingKokiriForest") == 0xa95bd438u);
}

TEST_CASE("shared flags: names, hashes, indexes and ranges") {
  std::string error;
  const auto flags = SharedFlags::parse(
      "# comment\n"
      "\n"
      "Some.Flag\r\n"
      "0xBEEF   # hash\n"
      "  Some.Array 2\n"
      "Some.Array 0-3\n"
      "Some.Flag\n",
      &error);
  REQUIRE_MESSAGE(flags, error);
  const std::uint32_t array = flag_hash("Some.Array");
  CHECK(flags->size() == 6);  // the flag, the hash, elements 0 to 3 once each
  CHECK(flags->contains({flag_hash("Some.Flag"), 0}));
  CHECK(flags->contains({0xbeefu, 0}));
  for (std::uint16_t i = 0; i < 4; ++i) CHECK(flags->contains({array, i}));
  CHECK_FALSE(flags->contains({array, 4}));
  CHECK_FALSE(flags->contains({flag_hash("Some.Flag"), 1}));
}

TEST_CASE("shared flags: bad lines are refused with their line number") {
  const char* bad[] = {
      "a b c\n",            // too many tokens
      "0x123456789\n",      // a hash over 8 digits
      "a 65536\n",          // index too big
      "a 5-2\n",            // a range going down
      "a x\n",              // not a number
      "a 1-\n",             // half a range
      "na\xc3\xafve\n",     // not ASCII
  };
  for (const char* text : bad) {
    std::string error;
    CHECK_FALSE(SharedFlags::parse(std::string("ok\n") + text, &error));
    CHECK(error.rfind("line 2:", 0) == 0);
  }
  // 0x with no digits, or with other letters, is a name.
  const auto names = SharedFlags::parse("0x\n0xZZ\n");
  REQUIRE(names);
  CHECK(names->contains({flag_hash("0x"), 0}));
  CHECK(names->contains({flag_hash("0xZZ"), 0}));
}

TEST_CASE("shared flags: the example file loads") {
  const auto path = std::filesystem::path(__FILE__).parent_path() / "shared_flags.example.txt";
  std::string error;
  const auto flags = SharedFlags::load(path, &error);
  REQUIRE_MESSAGE(flags, error);
  CHECK(flags->size() == 7);
  CHECK(flags->contains({flag_hash("Example.ChestOpened"), 0}));
  CHECK(flags->contains({0xbeefu, 0}));
  CHECK(flags->contains({flag_hash("Example.DoorUnlocked"), 2}));
  CHECK(flags->contains({flag_hash("Example.KeysUsed"), 3}));

  CHECK_FALSE(SharedFlags::load(path.parent_path() / "no_such_file.txt", &error));
}
