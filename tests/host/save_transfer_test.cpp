#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "coop/host/save_transfer.hpp"
#include "coop/protocol/codec.hpp"

namespace save = coop::host::save;
namespace proto = coop::protocol;
namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

Bytes Noise(std::size_t n, std::uint32_t seed) {
  std::mt19937 rng(seed);
  Bytes b(n);
  for (auto& x : b) x = static_cast<std::uint8_t>(rng());
  return b;
}

save::SaveSlot Slot(std::uint32_t seed) {
  return {{{"progress.sav", Noise(133616, seed)}, {"caption.sav", Noise(38573, seed + 1)}}};
}

// A fresh folder under the temp folder, removed at the end of the test.
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

void WriteFile(const fs::path& p, const Bytes& b) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

Bytes ReadFile(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// What the server does to a part on its way: the host's number in.
proto::SavePart Relayed(proto::SavePart p, std::uint8_t host) {
  p.player_number = host;
  return p;
}

}  // namespace

TEST_CASE("save transfer: CRC-32 is zlib's") {
  const std::string s = "123456789";
  CHECK(save::crc32(Bytes(s.begin(), s.end())) == 0xCBF43926u);
  CHECK(save::crc32(Bytes{}) == 0u);
}

TEST_CASE("save transfer: a slot packs and unpacks, anything else is refused") {
  const auto slot = Slot(1);
  const Bytes packed = save::pack(slot);
  REQUIRE_FALSE(packed.empty());
  auto back = save::unpack(packed);
  REQUIRE(back.has_value());
  CHECK(*back == slot);

  CHECK(save::pack({{{"option.sav", {1}}}}).empty());                       // not a slot file
  CHECK(save::pack({{{"../progress.sav", {1}}}}).empty());                  // no paths
  CHECK(save::pack({{{"caption.sav", {1}}}}).empty());                      // no progress.sav
  CHECK(save::pack({{{"progress.sav", {1}}, {"progress.sav", {2}}}}).empty());  // twice
  CHECK(save::pack({{{"progress.sav", Bytes(proto::kMaxSaveBytes, 0)}}}).empty());  // too large

  Bytes bad = packed;
  bad[0] = 'X';
  CHECK_FALSE(save::unpack(bad));
  bad = packed;
  bad.push_back(0);
  CHECK_FALSE(save::unpack(bad));
  bad = packed;
  bad.pop_back();
  CHECK_FALSE(save::unpack(bad));
}

TEST_CASE("save transfer: a slot goes in parts, each of which encodes, and comes back whole") {
  const Bytes packed = save::pack(Slot(2));
  auto parts = save::split(packed, 9, proto::SaveReason::join, 2);
  REQUIRE(parts.size() == proto::save_part_count(static_cast<std::uint32_t>(packed.size())));
  CHECK(parts.size() == 11);  // about 170 KB in 16 KB parts
  for (const auto& p : parts) {
    Bytes out;
    CHECK(proto::encode(proto::Message{p}, out) == proto::Error::none);
    CHECK(out.size() <= 0xFFFF + proto::kHeaderSize);
  }

  // Out of order and twice: still one complete slot, with the right CRC.
  save::Assembler a;
  std::size_t completes = 0;
  for (std::size_t i = parts.size(); i-- > 0;) {
    const auto r = a.add(Relayed(parts[i], 1), 1);
    CHECK(r != save::Assembler::Result::refused);
    if (i == parts.size() - 1) CHECK(a.add(Relayed(parts[i], 1), 1) == save::Assembler::Result::partial);
    if (r == save::Assembler::Result::complete) ++completes;
  }
  CHECK(completes == 1);
  CHECK(a.reason() == proto::SaveReason::join);
  CHECK(a.take() == packed);
  CHECK(a.take().empty());  // once
}

TEST_CASE("save transfer: parts not from the host, mixed headers and a bad CRC are refused") {
  const Bytes packed = save::pack(Slot(3));
  auto parts = save::split(packed, 4, proto::SaveReason::host_load, 0);
  save::Assembler a;
  CHECK(a.add(Relayed(parts[0], 2), 1) == save::Assembler::Result::refused);  // not the host
  CHECK(a.add(Relayed(parts[0], 1), 0) == save::Assembler::Result::refused);  // no host known
  CHECK(a.add(Relayed(parts[0], 1), 1) == save::Assembler::Result::partial);
  auto mixed = Relayed(parts[1], 1);
  mixed.crc32 ^= 1;
  CHECK(a.add(mixed, 1) == save::Assembler::Result::refused);

  // A newer transfer from the host starts again (the host loaded another save).
  const Bytes newer = save::pack(Slot(4));
  auto parts2 = save::split(newer, 5, proto::SaveReason::host_load, 0);
  save::Assembler::Result last{};
  for (const auto& p : parts2) last = a.add(Relayed(p, 1), 1);
  CHECK(last == save::Assembler::Result::complete);
  CHECK(a.transfer_id() == 5);
  CHECK(a.take() == newer);

  // Bytes changed on the way: every part in, but the CRC differs.
  save::Assembler b;
  auto broken = save::split(packed, 6, proto::SaveReason::join, 3);
  broken[2].data[10] ^= 0xFF;
  for (const auto& p : broken) last = b.add(Relayed(p, 1), 1);
  CHECK(last == save::Assembler::Result::bad_crc);
  CHECK(b.take().empty());
}

TEST_CASE("save transfer: the host answers asks at once with its newest save, also right after its own load") {
  using Send = save::Planner::Send;
  const auto t0 = fs::file_time_type::clock::now();
  const auto later = t0 + std::chrono::seconds(30);
  save::Planner p;
  CHECK(p.due(t0).empty());  // nobody asked

  p.on_ask(2);
  CHECK(p.due(t0) == std::vector<Send>{{2, proto::SaveReason::join}});
  CHECK(p.due(t0).empty());  // once

  // hostsave-fix: the host loaded a save, then a guest asks. The ask is answered at once with the save on disk
  // (it used to wait for the game's next save, and the guest got nothing); every guest gets the next save after it.
  p.on_host_load(later);
  p.on_ask(3);
  CHECK(p.waiting());
  CHECK(p.due(t0) == std::vector<Send>{{3, proto::SaveReason::join}});  // at once, from before the load
  CHECK(p.waiting());
  CHECK(p.due(later).empty());  // nothing new on disk
  CHECK(p.due(later + std::chrono::seconds(1)) == std::vector<Send>{{0, proto::SaveReason::host_load}});
  CHECK_FALSE(p.waiting());
  CHECK(p.idle());
  CHECK(p.due(later + std::chrono::seconds(2)).empty());

  // An ask on the same look as the reload: the reload goes to everyone, once.
  p.on_host_load(later + std::chrono::seconds(5));
  p.on_ask(4);
  CHECK(p.due(later + std::chrono::seconds(6)) == std::vector<Send>{{0, proto::SaveReason::host_load}});
  CHECK(p.idle());
  p.forget(4);

  // A player who left is not answered.
  CHECK(p.on_ask(4));
  p.forget(4);
  CHECK(p.due(later + std::chrono::seconds(3)).empty());
}

TEST_CASE("save transfer: a host with no save tells the asker at once (hostsave-fix)") {
  using Send = save::Planner::Send;
  save::Planner p;
  CHECK(p.on_ask(2));
  CHECK(p.due(std::nullopt) == std::vector<Send>{{2, proto::SaveReason::join, true}});
  CHECK(p.idle());
  CHECK(p.due(std::nullopt).empty());  // once

  // "No save" goes as an ordinary transfer and comes back as no slot, but as "no save".
  const auto none = save::pack_no_save();
  CHECK(save::is_no_save(none));
  CHECK_FALSE(save::unpack(none));
  CHECK_FALSE(save::is_no_save(save::pack(Slot(3))));
  CHECK_FALSE(save::is_no_save(Bytes{}));
  const auto parts = save::split(none, 77, proto::SaveReason::join, 2);
  REQUIRE(parts.size() == 1);
  Bytes wire;
  CHECK(proto::encode(proto::Message{parts[0]}, wire) == proto::Error::none);
  save::Assembler a;
  CHECK(a.add(Relayed(parts[0], 1), 1) == save::Assembler::Result::complete);
  CHECK(save::is_no_save(a.take()));
}

TEST_CASE("save transfer: a half-received save is never written, a slot is written whole (hostsave-fix)") {
  TempDir dir("coop_tests_save_partial");
  const fs::path own = dir.path / "0400486024DEC000";
  const fs::path coop = save::coop_dir_of(own);
  WriteFile(own / "slot_02" / "progress.sav", {7});
  const auto packed = save::pack(Slot(9));
  const auto parts = save::split(packed, 5, proto::SaveReason::join, 2);
  REQUIRE(parts.size() > 2);
  save::Assembler a;
  for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
    CHECK(a.add(Relayed(parts[i], 1), 1) == save::Assembler::Result::partial);
  }
  CHECK(a.take().empty());          // nothing to write until the last part
  CHECK_FALSE(fs::exists(coop));    // and nothing was
  CHECK(a.add(Relayed(parts.back(), 1), 1) == save::Assembler::Result::complete);
  const auto slot = save::unpack(a.take());
  REQUIRE(slot);

  // A stale .part folder (a write cut off earlier) is not loadable, and is replaced.
  WriteFile(coop / "slot_00.part" / "progress.sav", {1});
  CHECK_FALSE(save::newest_slot(coop).has_value());
  CHECK(save::write_coop_save(coop, *slot, own) == save::WriteResult::written);
  CHECK(ReadFile(coop / "slot_00" / "progress.sav") == slot->files[0].bytes);
  CHECK(ReadFile(coop / "slot_02" / "progress.sav") == slot->files[0].bytes);  // the own folder's slot name too
  CHECK_FALSE(fs::exists(coop / "slot_00.part"));
  CHECK_FALSE(fs::exists(coop / "slot_02.part"));
  CHECK(ReadFile(own / "slot_02" / "progress.sav") == Bytes{7});  // own save untouched
}

TEST_CASE("save transfer: a guest that asks again and again gets the save once") {
  using Send = save::Planner::Send;
  const auto t0 = fs::file_time_type::clock::now();
  save::Planner p;
  CHECK(p.on_ask(2));
  for (int i = 0; i < 100; ++i) CHECK_FALSE(p.on_ask(2));
  CHECK(p.due(t0) == std::vector<Send>{{2, proto::SaveReason::join}});
  for (int i = 0; i < 100; ++i) CHECK_FALSE(p.on_ask(2));
  CHECK(p.idle());
  CHECK(p.due(t0).empty());
  // It left and a player with its number joined: that one may ask.
  p.forget(2);
  CHECK(p.on_ask(2));
  CHECK(p.due(t0) == std::vector<Send>{{2, proto::SaveReason::join}});
}

TEST_CASE("save transfer: the newest slot is found and read") {
  TempDir dir("coop_tests_save_newest");
  const fs::path game = dir.path / "0400486024DEC000";
  WriteFile(game / "slot_00" / "progress.sav", {1});
  WriteFile(game / "slot_03" / "progress.sav", {3, 3});
  WriteFile(game / "slot_03" / "caption.sav", {4});
  WriteFile(game / "slot_05" / "caption.sav", {5});  // no progress: not a save
  WriteFile(game / "option.sav", {9});
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(game / "slot_00" / "progress.sav", now - std::chrono::hours(1));
  fs::last_write_time(game / "slot_03" / "progress.sav", now);

  const auto newest = save::newest_slot(game);
  REQUIRE(newest.has_value());
  CHECK(newest->filename() == "slot_03");
  CHECK(save::newest_write(game) == now);
  const auto slot = save::read_slot(*newest);
  REQUIRE(slot.has_value());
  CHECK(*slot == save::SaveSlot{{{"progress.sav", {3, 3}}, {"caption.sav", {4}}}});
  CHECK_FALSE(save::read_slot(game / "slot_05"));
  CHECK_FALSE(save::newest_slot(dir.path / "missing"));
}

TEST_CASE("save transfer: the host's slot goes only into the co-op folder, never over the player's own saves") {
  TempDir dir("coop_tests_save_write");
  const fs::path own = dir.path / "0400486024DEC000";
  const fs::path coop = save::coop_dir_of(own);
  CHECK(coop.filename() == "0400486024DEC000-coop");
  CHECK(save::own_dir_of(coop) == own);
  CHECK(save::coop_dir_of(coop) == coop);
  CHECK(save::is_coop_dir(coop));
  CHECK_FALSE(save::is_coop_dir(own));
  CHECK_FALSE(save::is_coop_dir(dir.path / "-coop"));

  const Bytes own_progress{7, 7, 7};
  WriteFile(own / "slot_00" / "progress.sav", own_progress);
  WriteFile(own / "slot_01" / "progress.sav", own_progress);
  WriteFile(own / "option.sav", {1, 2});
  WriteFile(own / "account.sav", {3});
  const save::SaveSlot host{{{"progress.sav", {9, 9}}, {"caption.sav", {8}}}};

  // Refused: the player's own folder, or the two swapped.
  CHECK(save::write_coop_save(own, host, own) == save::WriteResult::not_coop);
  CHECK(save::write_coop_save(own, host, coop) == save::WriteResult::not_coop);
  CHECK(ReadFile(own / "slot_00" / "progress.sav") == own_progress);
  CHECK_FALSE(fs::exists(own / "slot_00" / "caption.sav"));

  // Written: slot_00 of the co-op folder, the player's settings copied in.
  WriteFile(coop / "slot_04" / "progress.sav", {1});  // the guest's own co-op autosave
  WriteFile(coop / "option.sav", {5});                // the co-op folder's settings stay
  CHECK(save::write_coop_save(coop, host, own) == save::WriteResult::written);
  CHECK(ReadFile(coop / "slot_00" / "progress.sav") == Bytes{9, 9});
  CHECK(ReadFile(coop / "slot_00" / "caption.sav") == Bytes{8});
  CHECK(ReadFile(coop / "slot_04" / "progress.sav") == Bytes{9, 9});  // every co-op slot is the host's
  CHECK(ReadFile(coop / "slot_01" / "progress.sav") == Bytes{9, 9});  // and every slot name of the own folder
  CHECK(ReadFile(coop / "option.sav") == Bytes{5});
  CHECK(ReadFile(coop / "account.sav") == Bytes{3});
  CHECK_FALSE(fs::exists(coop / "slot_00" / "progress.sav.tmp"));

  // The player's own saves are as they were.
  CHECK(ReadFile(own / "slot_00" / "progress.sav") == own_progress);
  CHECK(ReadFile(own / "slot_01" / "progress.sav") == own_progress);
  CHECK(ReadFile(own / "option.sav") == Bytes{1, 2});
  CHECK(ReadFile(own / "account.sav") == Bytes{3});
  CHECK(fs::exists(own / "account.sav"));  // copied, not moved
}

TEST_CASE("save transfer: which save folder a game plays from (issue 167)") {
  using coop::host::save::folder_choice;
  using coop::host::save::FolderChoice;
  // a fixed folder (the emulator): LICHII_COOP_HOST_SAVE (play.ps1: 1 for a join by address or invite code, 0 for
  // a host) decides
  CHECK(folder_choice(true, false, false, false, false) == FolderChoice::coop);
  CHECK(folder_choice(false, false, true, false, false) == FolderChoice::own);
  // switchable (Hookshot): the session's role, never the variable (the title menu may Host or Join after any start)
  CHECK(folder_choice(true, true, true, true, false) == FolderChoice::own);    // started as a join, then hosted
  CHECK(folder_choice(false, true, true, false, false) == FolderChoice::coop); // started as a host, then joined
  CHECK(folder_choice(true, true, false, false, false) == FolderChoice::wait);
  // a fixed folder (the emulator) without it: the player's own saves, whatever coop.ini says
  CHECK(folder_choice(std::nullopt, false, true, false, false) == FolderChoice::own);
  // switchable (Hookshot): the session decides, once connected to a host
  CHECK(folder_choice(std::nullopt, true, false, false, false) == FolderChoice::wait);
  CHECK(folder_choice(std::nullopt, true, true, false, false) == FolderChoice::coop);   // a title-menu Join or an invite
  CHECK(folder_choice(std::nullopt, true, true, true, false) == FolderChoice::own);     // the host, even with an old
                                                                                         // friend's address in coop.ini
  CHECK(folder_choice(std::nullopt, true, true, false, true) == FolderChoice::own);     // joined with a save loaded
}

TEST_CASE("save transfer: a co-op folder is emptied of slots before the host's save comes (issue 167)") {
  namespace fs = std::filesystem;
  const fs::path base = fs::temp_directory_path() / "coop-clear-slots-test";
  std::error_code ec;
  fs::remove_all(base, ec);
  const fs::path coop = base / "0123-coop", own = base / "0123";
  for (const char* d : {"slot_00", "slot_03", "other"}) fs::create_directories(coop / d);
  std::ofstream(coop / "slot_00" / "progress.sav") << "an older host's save";
  std::ofstream(coop / "option.sav") << "settings";
  fs::create_directories(own / "slot_00");
  CHECK(coop::host::save::clear_coop_slots(coop) == 2);
  CHECK_FALSE(fs::exists(coop / "slot_00"));
  CHECK_FALSE(fs::exists(coop / "slot_03"));
  CHECK(fs::exists(coop / "other"));       // not a slot: left
  CHECK(fs::exists(coop / "option.sav"));  // the player's settings stay
  CHECK(coop::host::save::clear_coop_slots(own) == -1);   // never the player's own folder
  CHECK(fs::exists(own / "slot_00"));
  CHECK(coop::host::save::clear_coop_slots(base / "none-coop") == 0);
  fs::remove_all(base, ec);
}

TEST_CASE("save transfer: the folder is chosen again for each session, never with a save loaded") {
  using coop::host::save::decide_again;
  CHECK(decide_again(true, true, false));     // Join, back to the title, then Host: chosen again
  CHECK_FALSE(decide_again(true, false, false));  // the same session (or a reconnect): kept
  CHECK_FALSE(decide_again(true, true, true));    // Link in the game: no switch
  CHECK_FALSE(decide_again(false, true, false));  // nothing chosen yet
}

TEST_CASE("save transfer: two Join sessions in one run each ask once and get the save") {
  const Bytes packed = save::pack(Slot(2));
  save::GuestSession s;
  for (int session = 0; session < 2; ++session) {
    if (session == 1) {
      CHECK(coop::host::save::decide_again(true, true, false));
      s = save::GuestSession{};   // (CoopSave: what decide_again resets)
    }
    CHECK_FALSE(s.asked);   // this session's ask has not gone out: it will
    CHECK_FALSE(s.ready);
    s.asked = true;         // the ask goes out once
    const auto parts = save::split(packed, 20 + static_cast<std::uint32_t>(session), proto::SaveReason::join, 1);
    std::size_t completes = 0;
    for (const auto& p : parts) {
      if (s.assembler.add(Relayed(p, 1), 1) == save::Assembler::Result::complete) ++completes;
    }
    CHECK(completes == 1);
    CHECK(s.assembler.take() == packed);
    s.ready = true;
  }
}
