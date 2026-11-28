// Row 307: our Link's AS variables read from guest memory as the game's own lookups find them
// (coop/host/as_vars.hpp; the game's code: reports/puppet-run-307-5.md).
#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "coop/host/as_vars.hpp"
#include "fake_memory.hpp"

using namespace coop::host;
using coop::host::test::FakeMemory;
namespace proto = coop::protocol;

namespace {

constexpr std::uint64_t kAs = 0x90000000;
constexpr std::uint64_t kVt = 0x90010000;
constexpr std::uint64_t kRes = 0x90020000;
constexpr std::uint64_t kResVt = 0x90030000;
constexpr std::uint64_t kFile = 0x90040000;
constexpr std::uint64_t kTable = 0x90050000;
constexpr std::uint64_t kAsb = 0x90060000;
constexpr std::uint64_t kStrings = 0x90070000;
constexpr std::uint64_t kFloats = 0x90080000;
constexpr std::uint64_t kBools = 0x90090000;

// An AS whose f32 names are, in this order, XZSpeed, LinearVelXZ, LeftStickLength, Other (handles 0..3) and whose
// bool names are IsDash, IsLeftStickInput (handles 0, 1): the handles differ from PLAYER_SAMPLE's order.
void make_as(FakeMemory& m, std::uint64_t as = kAs) {
  const std::uint64_t b = FakeMemory::kBase;
  m.put_u64(as, kVt);
  m.put_u64(kVt + as_addr::kVtF32Address, b + as_addr::kF32Address);
  m.put_u64(kVt + as_addr::kVtBoolAddress, b + as_addr::kBoolAddress);
  m.put_u64(kVt + as_addr::kVtF32Get, b + as_addr::kF32Get);
  m.put_u64(kVt + as_addr::kVtBoolGet, b + as_addr::kBoolGet);
  m.put_u64(kVt + as_addr::kVtF32Handle, b + as_addr::kF32Handle);
  m.put_u64(kVt + as_addr::kVtBoolHandle, b + as_addr::kBoolHandle);
  for (const std::uint64_t off : {0x88ull, 0x90ull, 0xb8ull, 0xc0ull, 0xf8ull}) m.put_u64(as + off, 0);
  m.put_u64(as + 0xf0, kRes);
  m.put_u64(kRes, kResVt);
  m.put_u64(kResVt + as_addr::kResourceVtF32Table, b + as_addr::kF32HandleTable);
  m.put_u64(kResVt + as_addr::kResourceVtBoolTable, b + as_addr::kBoolHandleTable);
  m.put_u64(kRes + 0x10, kFile);
  m.put_u64(kFile + 0x8, kAsb);
  m.put_u64(kFile + 0x20, kTable);
  m.put_u32(kAsb + 0x3c, static_cast<std::uint32_t>(kStrings - kAsb));
  m.put_u16(kTable + 0x18, 4);  // f32 count
  m.put_u16(kTable + 0x1a, 1);  // f32 names from table entry 1 (entry 0 is someone else's)
  m.put_u16(kTable + 0x20, 2);  // bool count
  m.put_u16(kTable + 0x22, 5);  // bool names from entry 5
  const char* names[] = {"Before", "XZSpeed", "LinearVelXZ", "LeftStickLength", "Other", "IsDash", "IsLeftStickInput"};
  std::uint32_t at = 0;
  for (std::uint32_t i = 0; i < 7; ++i) {
    const std::string s = names[i];
    m.put(kStrings + at, s.c_str(), s.size() + 1);
    m.put_u32(kTable + 0x38 + 4 * i, at | 0x5000000u);  // the top byte is not part of the offset
    at += static_cast<std::uint32_t>(s.size() + 1);
  }
  m.put_u64(as + 0x28, kFloats);
  m.put_u32(kFloats, 4);
  m.put_f32(kFloats + 4, 6.5f);    // XZSpeed
  m.put_f32(kFloats + 8, 6.25f);   // LinearVelXZ
  m.put_f32(kFloats + 12, 1.0f);   // LeftStickLength
  m.put_f32(kFloats + 16, 99.0f);  // Other
  m.put_u64(as + 0x30, kBools);
  m.put_u32(kBools, 2);
  m.put_u8(kBools + 4, 1);  // IsDash
  m.put_u8(kBools + 5, 0);  // IsLeftStickInput
}

}  // namespace

TEST_CASE("as vars: handles by name, values from the AS's own arrays (row 307)") {
  FakeMemory m;
  make_as(m);
  CHECK(as_var_handle(m, kAs, "XZSpeed", false) == 0);
  CHECK(as_var_handle(m, kAs, "LeftStickLength", false) == 2);
  CHECK(as_var_handle(m, kAs, "IsLeftStickInput", true) == 1);
  CHECK(as_var_handle(m, kAs, "Before", false) == -1);  // before the f32 names' first entry
  CHECK(as_var_handle(m, kAs, "XZSpee", false) == -1);  // a prefix is not the name
  AsVarsMemo memo;
  const auto v = read_as_vars(m, kAs, memo);
  REQUIRE(v);
  CHECK(v->left_stick_length == 1.0f);
  CHECK(v->linear_vel_xz == 6.25f);
  CHECK(v->xz_speed == 6.5f);
  CHECK(v->bits == proto::kAsVarDash);
  CHECK(memo.as == kAs);
  // The handles are kept: a second frame reads only the values.
  m.put_u8(kBools + 5, 1);
  m.reads = 0;
  const auto again = read_as_vars(m, kAs, memo);
  REQUIRE(again);
  CHECK(again->bits == (proto::kAsVarDash | proto::kAsVarLeftStickInput));
  CHECK(m.reads < 60);
}

TEST_CASE("as vars: a bound value, a linked AS and an override read as the game's lookup reads them") {
  FakeMemory m;
  make_as(m);
  // [AS + 0xb8]: per-handle pointers; XZSpeed (handle 0) points elsewhere.
  constexpr std::uint64_t kBound = 0x900a0000;
  m.put_u64(kAs + 0xb8, kBound);
  m.put_u32(kBound, 4);
  m.put_u64(kBound + 8, 0x900b0000);
  m.put_f32(0x900b0000, 3.0f);
  for (std::uint64_t i = 1; i < 4; ++i) m.put_u64(kBound + 8 + 8 * i, 0);
  CHECK(as_f32(m, kAs, 0) == 3.0f);
  CHECK(as_f32(m, kAs, 1) == 6.25f);  // a null pointer: the AS's own value
  // [AS + 0x90]: bool handle 0 (IsDash) is another AS's bool 1.
  constexpr std::uint64_t kOther = 0x900c0000;
  make_as(m, kOther);
  constexpr std::uint64_t kLink = 0x900d0000;
  m.put_u64(kAs + 0x90, kLink);
  m.put_u32(kLink, 2);
  m.put_u8(kLink + 4, 2);  // linked AS 2: [AS + 0xd0 + 16]
  m.put_u8(kLink + 5, 1);
  m.put_u8(kLink + 6, 0xff);  // handle 1: not linked
  m.put_u8(kLink + 7, 0);
  m.put_u64(kAs + 0xd0 + 16, kOther);
  CHECK(as_bool(m, kAs, 0) == false);  // kOther's bool 1 (IsLeftStickInput, 0); its arrays are kFloats/kBools too
  m.put_u8(kBools + 5, 1);
  CHECK(as_bool(m, kAs, 0) == true);
  // A class whose value lookup is not the default one: nothing, never a guess.
  m.put_u64(kVt + as_addr::kVtF32Address, 0x1234);
  CHECK_FALSE(as_f32(m, kAs, 1));
  AsVarsMemo memo;
  CHECK_FALSE(read_as_vars(m, kAs, memo));
}

TEST_CASE("as vars: a name the AS does not have sends 0, no AS gives nothing") {
  FakeMemory m;
  make_as(m);
  m.put(kStrings, "Zzzzzz", 7);  // "Before" becomes "Zzzzzz"
  m.put_u32(kTable + 0x38 + 4 * 3, 0);  // LeftStickLength's entry now names "Zzzzzz": not found
  AsVarsMemo memo;
  const auto v = read_as_vars(m, kAs, memo);
  REQUIRE(v);
  CHECK(memo.f32[0] == -1);
  CHECK(v->left_stick_length == 0.0f);
  CHECK(v->xz_speed == 6.5f);
  CHECK_FALSE(read_as_vars(m, 0, memo));
}
