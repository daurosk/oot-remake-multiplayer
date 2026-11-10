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

TEST_CASE("shared flags: 64-bit keys in hex or decimal") {
  std::string error;
  const auto flags = SharedFlags::parse(
      "key64:0x2fa4c1d09b7e6a35  # a chest\n"
      "key64:3433081917669272117\n"  // the same key in decimal
      "key64:0X1\n"
      "key64:18446744073709551615\n"
      "0x2fa4c1d0\n",
      &error);
  REQUIRE_MESSAGE(flags, error);
  CHECK(flags->size() == 4);
  CHECK(flags->contains(FlagKey::key64_flag(0x2fa4c1d09b7e6a35ull)));
  CHECK(flags->contains(FlagKey::key64_flag(1)));
  CHECK(flags->contains(FlagKey::key64_flag(0xffffffffffffffffull)));
  CHECK(flags->contains({0x2fa4c1d0u, 0}));
  CHECK_FALSE(flags->contains(FlagKey::key64_flag(0x2fa4c1d0u)));
  CHECK_FALSE(flags->contains(FlagKey::key64_flag(0x2fa4c1d09b7e6a35ull, 1)));
  // Named flags sort before 64-bit keys.
  CHECK(flags->keys().front().kind == FlagKeyKind::named);
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
      "key64:\n",           // no key
      "key64:0x\n",         // no hex digits
      "key64:0x10000000000000000\n",  // 17 hex digits
      "key64:18446744073709551616\n",  // past u64
      "key64:12ab\n",       // not decimal
      "key64:-1\n",         // not a number
      "key64:5 2\n",        // a 64-bit key with an index
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
  const auto path = std::filesystem::path(COOP_TEST_SOURCE_DIR) / "host" / "shared_flags.example.txt";
  std::string error;
  const auto flags = SharedFlags::load(path, &error);
  REQUIRE_MESSAGE(flags, error);
  CHECK(flags->size() == 8);
  CHECK(flags->contains(FlagKey::key64_flag(0x2fa4c1d09b7e6a35ull)));
  CHECK(flags->contains({flag_hash("Example.ChestOpened"), 0}));
  CHECK(flags->contains({0xbeefu, 0}));
  CHECK(flags->contains({flag_hash("Example.DoorUnlocked"), 2}));
  CHECK(flags->contains({flag_hash("Example.KeysUsed"), 3}));

  CHECK_FALSE(SharedFlags::load(path.parent_path() / "no_such_file.txt", &error));
}
