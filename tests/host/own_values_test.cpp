// Row 249: a guest's own values (rupees, ammo, bottles, life, magic: ItemRule::per_player) outlive every join,
// return to the title, Resync and host change; the host's own saves and solo saves are never touched.
#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "coop/host/flags.hpp"
#include "coop/host/save_transfer.hpp"

namespace save = coop::host::save;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

constexpr std::uint32_t kRupee = 0xa77921d7;    // PlayerStatus.CurrentRupee (int32, per player)
constexpr std::uint32_t kBomb = 0xdb1bcd4f;     // PouchData.Item_Bomb.StockNum (int32, per player)
constexpr std::uint32_t kLife = 0x31ab5580;     // PlayerStatus.Life (int32, per player)
constexpr std::uint32_t kBottleA = 0x7d48df76;  // ...BottleA.BottleItem (uint32, per player)
constexpr std::uint32_t kWallet = 0x1d74617e;   // PouchData.Item_Wallet_Small.StockNum (shared, max)
constexpr std::uint32_t kMP = 0x76b18200;       // PlayerStatus.MP (int32, per player)
constexpr std::uint32_t kCapRupee = 0xe0010a50; // 0xf80f8077.Rupee: the wallet's capacity
constexpr std::uint32_t kCapBomb = 0xe753508b;  // 0xf80f8077.Bomb: the bomb bag's
const std::uint32_t kMaxLife = coop::host::flag_hash("PlayerStatus.MaxLife");
const std::uint32_t kMPMax = coop::host::flag_hash("PlayerStatus.MPMax");
const std::uint32_t kBottleAValid = coop::host::flag_hash("PlayerStatus.AssignableItem.BottleInfo.BottleA.IsValid");

struct Values {
  std::int32_t rupee = 0, bomb = 0, life = 12, max_life = 12, wallet = 1, mp = 0, mp_max = 240, rupee_cap = 999,
               bomb_cap = 40;
  std::uint32_t bottle = 0, bottle_valid = 1;
};

// A saved game in tools/autotest/save_flags.py's layout: Bool (type 0), Int (type 2), UInt (type 20), padding.
Bytes FakeSave(const Values& v) {
  Bytes d(0x20, 0);
  const auto put = [&](std::size_t at, std::uint32_t x) {
    if (d.size() < at + 4) d.resize(at + 4, 0);
    for (std::size_t i = 0; i < 4; ++i) d[at + i] = static_cast<std::uint8_t>(x >> (8 * i));
  };
  put(0, 0x01020304);
  std::size_t at = 0x20;
  const auto rec = [&](std::uint32_t a, std::uint32_t b) { put(at, a), put(at + 4, b), at += 8; };
  for (std::uint32_t type = 0; type < 0x20; ++type) {
    rec(0, type);
    if (type == 0) {
      rec(kRupee, 1);  // a Bool with a per-player value's hash: never taken for it
      rec(kBottleAValid, v.bottle_valid);
    }
    if (type == 2) {
      rec(kRupee, static_cast<std::uint32_t>(v.rupee));
      rec(kBomb, static_cast<std::uint32_t>(v.bomb));
      rec(kLife, static_cast<std::uint32_t>(v.life));
      rec(kMaxLife, static_cast<std::uint32_t>(v.max_life));
      rec(kWallet, static_cast<std::uint32_t>(v.wallet));
      rec(kMP, static_cast<std::uint32_t>(v.mp));
      rec(kMPMax, static_cast<std::uint32_t>(v.mp_max));
      rec(kCapRupee, static_cast<std::uint32_t>(v.rupee_cap));
      rec(kCapBomb, static_cast<std::uint32_t>(v.bomb_cap));
    }
    if (type == 20) rec(kBottleA, v.bottle);
  }
  rec(0, 0x20);
  rec(0x12345678, 0);
  put(8, static_cast<std::uint32_t>(at));
  d.resize(d.size() + 64, 0xab);  // a pool after the table
  return d;
}

std::uint32_t At(const Bytes& d, std::uint32_t hash, std::optional<std::uint32_t> type = std::nullopt) {
  const auto at = save::save_value_at(d, hash, type);
  REQUIRE(at);
  std::uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = v << 8 | d[at->offset + static_cast<std::size_t>(i)];
  return v;
}

save::SaveSlot Slot(const Values& v) { return {{{"progress.sav", FakeSave(v)}, {"caption.sav", Bytes(40, 7)}}}; }

struct TempDir {
  fs::path path;
  explicit TempDir(const char* name) : path(fs::temp_directory_path() / name) {
    fs::remove_all(path);
    fs::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

Bytes ReadFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The guest's game saving into a slot of the folder it plays from (its own autosave), the newest by a second.
void GameSaves(const fs::path& dir, const char* slot, const Values& v, int seconds_later) {
  fs::create_directories(dir / slot);
  const fs::path p = dir / slot / "progress.sav";
  {
    const Bytes b = FakeSave(v);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  }
  fs::last_write_time(p, fs::file_time_type::clock::now() + std::chrono::seconds(seconds_later));
}

std::uint32_t RupeesIn(const fs::path& slot_dir) { return At(ReadFile(slot_dir / "progress.sav"), kRupee, 2); }

}  // namespace

TEST_CASE("own values: inline values are found by hash and type, a damaged table is refused") {
  const Bytes d = FakeSave({.rupee = 321, .bottle = 9});
  CHECK(At(d, kRupee) == 321);           // the Int, not the Bool with the same hash before it
  CHECK(At(d, kRupee, 2) == 321);
  CHECK(At(d, kBottleA) == 9);
  CHECK(save::save_value_at(d, kBottleA)->type == save::kSaveTypeUInt);
  CHECK_FALSE(save::save_value_at(d, kBottleA, save::kSaveTypeInt));
  CHECK_FALSE(save::save_value_at(d, 0x99999999));
  CHECK_FALSE(save::save_value_at(d, 0));
  Bytes bad = d;
  bad[0] = 0;
  CHECK_FALSE(save::save_value_at(bad, kRupee));
  CHECK_FALSE(save::save_value_at(Bytes(10, 0), kRupee));
  Bytes order = d;
  order[0x24] = 5;  // the first type is not 0
  CHECK_FALSE(save::save_value_at(order, kRupee));
}

TEST_CASE("own values: only the per-player values are read and written, in place") {
  const Bytes guest = FakeSave({.rupee = 250, .bomb = 17, .life = 10, .max_life = 16, .wallet = 1, .bottle = 4});
  const auto own = save::read_own_values(guest);
  REQUIRE(own.size() == 5);  // life, MP, bottle A, rupees, bombs; never the wallet (shared, max), MaxLife or a capacity
  Bytes host = FakeSave({.rupee = 5, .bomb = 0, .life = 16, .max_life = 16, .wallet = 2, .bottle = 0});
  const Bytes host_before = host;
  CHECK(save::put_own_values(host, own) == 5);
  CHECK(host.size() == host_before.size());
  CHECK(At(host, kRupee, 2) == 250);
  CHECK(At(host, kBomb) == 17);
  CHECK(At(host, kLife) == 10);
  CHECK(At(host, kBottleA) == 4);
  CHECK(At(host, kWallet) == 2);  // the shared wallet stays the host's
  CHECK(At(host, kMaxLife) == 16);
  // a value that is not a per-player one is never written, whatever a kept file says
  const std::vector<save::OwnValue> sneaky{{kWallet, 0}, {kMaxLife, 1}};
  CHECK(save::put_own_values(host, sneaky) == 0);
  CHECK(At(host, kWallet) == 2);
}

TEST_CASE("own values: life is kept only when alive, at most the host's save's hearts") {
  Bytes host = FakeSave({.life = 12, .max_life = 12});
  const std::vector<save::OwnValue> dead{{kLife, 0}};
  CHECK(save::put_own_values(host, dead) == 0);
  CHECK(At(host, kLife) == 12);
  const std::vector<save::OwnValue> more{{kLife, 20}};
  CHECK(save::put_own_values(host, more) == 1);
  CHECK(At(host, kLife) == 12);  // (the host's save's hearts)
  const std::vector<save::OwnValue> some{{kLife, 7}};
  CHECK(save::put_own_values(host, some) == 1);
  CHECK(At(host, kLife) == 7);
}

TEST_CASE("own values: never more than the host's save holds room for, a bottle only where it has one") {
  // The guest's 500 rupees, 30 bombs and 480 magic into a save with a 200 wallet, a 20 bomb bag and one meter.
  const std::vector<save::OwnValue> own{{kRupee, 500}, {kBomb, 30}, {kMP, 480}, {kBottleA, 4}};
  Bytes host = FakeSave({.rupee = 5, .bomb = 2, .mp = 100, .mp_max = 240, .rupee_cap = 200, .bomb_cap = 20});
  CHECK(save::put_own_values(host, own) == 4);
  CHECK(At(host, kRupee, 2) == 200);
  CHECK(At(host, kBomb) == 20);
  CHECK(At(host, kMP) == 240);
  CHECK(At(host, kBottleA) == 4);
  // No bomb bag in that save: no bombs. A negative count: 0. Within the room: as it was.
  Bytes no_bag = FakeSave({.bomb = 0, .bomb_cap = 0});
  const std::vector<save::OwnValue> some{{kBomb, 12}, {kRupee, static_cast<std::uint32_t>(-3)}, {kMP, 120}};
  CHECK(save::put_own_values(no_bag, some) == 3);
  CHECK(At(no_bag, kBomb) == 0);
  CHECK(At(no_bag, kRupee, 2) == 0);
  CHECK(At(no_bag, kMP) == 120);
  // A bottle that save does not have keeps its own (empty) content.
  Bytes no_bottle = FakeSave({.bottle = 0, .bottle_valid = 0});
  const std::vector<save::OwnValue> fairy{{kBottleA, 6}};
  CHECK(save::put_own_values(no_bottle, fairy) == 0);
  CHECK(At(no_bottle, kBottleA) == 0);
  CHECK(At(no_bottle, kBottleAValid, save::kSaveTypeBool) == 0);
}

TEST_CASE("own values: the kept file reads back, anything else is refused") {
  const std::vector<save::OwnValue> own{{kRupee, 250}, {kBottleA, 0xffffffffu}};
  const std::string text = save::own_values_text(own);
  CHECK(text == "coop-own-values 1\n0xa77921d7 250\n0x7d48df76 4294967295\n");
  CHECK(save::parse_own_values(text) == own);
  std::string crlf = "coop-own-values 1\r\n0xa77921d7 250\r\n";
  CHECK(save::parse_own_values(crlf) == std::vector<save::OwnValue>{{kRupee, 250}});
  CHECK(save::parse_own_values("coop-own-values 1\n") == std::vector<save::OwnValue>{});
  CHECK_FALSE(save::parse_own_values(""));
  CHECK_FALSE(save::parse_own_values("coop-own-values 2\n0xa77921d7 1\n"));
  CHECK_FALSE(save::parse_own_values("coop-own-values 1\na77921d7 1\n"));
  CHECK_FALSE(save::parse_own_values("coop-own-values 1\n0xa77921d7\n"));
  CHECK_FALSE(save::parse_own_values("coop-own-values 1\n0xa77921d7 1x\n"));
  CHECK_FALSE(save::parse_own_values("coop-own-values 1\n0x 1\n"));
}

TEST_CASE("own values: a guest keeps its rupees over rejoins, Resync and another host (row 249)") {
  TempDir base("coop-own-values-test");
  const fs::path own = base.path / "5a3c", coop = base.path / "5a3c-coop";
  // The guest's own solo save, which co-op never touches.
  GameSaves(own, "slot_00", {.rupee = 99}, -100);
  const Bytes solo_before = ReadFile(own / "slot_00" / "progress.sav");

  // First join ever: nothing kept, the host's values are where the guest starts.
  int kept = 0;
  REQUIRE(save::write_coop_save(coop, Slot({.rupee = 5, .bomb = 3}), own, &kept) == save::WriteResult::written);
  CHECK(kept == -1);
  CHECK(RupeesIn(coop / "slot_00") == 5);

  // The guest plays: its game saves 250 rupees and 17 bombs into slot_03.
  GameSaves(coop, "slot_03", {.rupee = 250, .bomb = 17, .bottle = 4}, 5);

  // Back on the title / a rejoin: the slots go (the guest's values are kept first), then the host's save comes.
  CHECK(save::clear_coop_slots(coop) == 2);
  REQUIRE(fs::exists(coop / std::string(save::kOwnValuesFile)));
  REQUIRE(save::write_coop_save(coop, Slot({.rupee = 5, .bomb = 3}), own, &kept) == save::WriteResult::written);
  CHECK(kept == 5);
  {
    const Bytes p = ReadFile(coop / "slot_00" / "progress.sav");
    CHECK(At(p, kRupee, 2) == 250);
    CHECK(At(p, kBomb) == 17);
    CHECK(At(p, kBottleA) == 4);
  }

  // The host's save again while the guest plays from the folder (the host loaded a save): no clear first; the
  // guest's newest slot is read then.
  GameSaves(coop, "slot_04", {.rupee = 300, .bomb = 1}, 10);
  REQUIRE(save::write_coop_save(coop, Slot({.rupee = 5}), own, &kept) == save::WriteResult::written);
  CHECK(RupeesIn(coop / "slot_04") == 300);

  // Two clears in a row (a join, then Resync before the host's save came): the kept values are not lost.
  CHECK(save::clear_coop_slots(coop) > 0);
  CHECK(save::clear_coop_slots(coop) == 0);
  // Another host, with its own rupees: still the guest's.
  REQUIRE(save::write_coop_save(coop, Slot({.rupee = 777, .bomb = 40}), own, &kept) == save::WriteResult::written);
  CHECK(RupeesIn(coop / "slot_00") == 300);
  CHECK(At(ReadFile(coop / "slot_00" / "progress.sav"), kBomb) == 1);

  // The guest's own saves were never written, and never got the file.
  CHECK(ReadFile(own / "slot_00" / "progress.sav") == solo_before);
  CHECK_FALSE(fs::exists(own / std::string(save::kOwnValuesFile)));
  CHECK_FALSE(save::keep_own_values(own));  // never the player's own folder
}

TEST_CASE("own values: a co-op folder left by 0.11.8 to 0.11.10 (slots, no kept file) keeps the guest's values") {
  TempDir base("coop-own-values-old-test");
  const fs::path own = base.path / "5a3c", coop = base.path / "5a3c-coop";
  fs::create_directories(own);
  GameSaves(coop, "slot_02", {.rupee = 120}, 0);
  int kept = 0;
  REQUIRE(save::write_coop_save(coop, Slot({.rupee = 5}), own, &kept) == save::WriteResult::written);
  CHECK(kept >= 1);
  CHECK(RupeesIn(coop / "slot_00") == 120);
  CHECK(RupeesIn(coop / "slot_02") == 120);
}

TEST_CASE("own values: slots that are not saves this build can read leave what was kept alone") {
  TempDir base("coop-own-values-noise-test");
  const fs::path own = base.path / "5a3c", coop = base.path / "5a3c-coop";
  fs::create_directories(own);
  GameSaves(coop, "slot_00", {.rupee = 42}, 0);
  CHECK(save::keep_own_values(coop));
  {
    std::ofstream out(coop / "slot_00" / "progress.sav", std::ios::binary | std::ios::trunc);
    out << "not a save";
  }
  fs::last_write_time(coop / "slot_00" / "progress.sav", fs::file_time_type::clock::now() + std::chrono::seconds(9));
  CHECK_FALSE(save::keep_own_values(coop));
  const auto kept = save::kept_own_values(coop);
  REQUIRE(kept);
  CHECK(*kept == std::vector<save::OwnValue>{{kLife, 12}, {kMP, 0}, {kBottleA, 0}, {kRupee, 42}, {kBomb, 0}});  // (item_flags order)
}
