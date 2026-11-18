#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
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

  p.on_ask(2, 0);
  CHECK(p.due(t0) == std::vector<Send>{{2, proto::SaveReason::join}});
  CHECK(p.due(t0).empty());  // once

  // hostsave-fix: the host loaded a save, then a guest asks. The ask is answered at once with the save on disk
  // (it used to wait for the game's next save, and the guest got nothing); every guest gets the next save after it.
  p.on_host_load(later);
  p.on_ask(3, 0);
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
  p.on_ask(4, 0);
  CHECK(p.due(later + std::chrono::seconds(6)) == std::vector<Send>{{0, proto::SaveReason::host_load}});
  CHECK(p.idle());
  p.forget(4);

  // A player who left is not answered.
  CHECK(p.on_ask(4, 0));
  p.forget(4);
  CHECK(p.due(later + std::chrono::seconds(3)).empty());
}

TEST_CASE("save transfer: a host with no save tells the asker at once (hostsave-fix)") {
  using Send = save::Planner::Send;
  save::Planner p;
  CHECK(p.on_ask(2, 0));
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

TEST_CASE("save transfer: a guest's switch to the host's save goes only on the title, never during a load") {
  using save::PendingStep;
  using save::pending_step;
  CHECK(pending_step(false, true, false) == PendingStep::keep);  // nothing in yet
  CHECK(pending_step(true, true, false) == PendingStep::go);     // in, on the title's menu
  CHECK(pending_step(true, false, false) == PendingStep::keep);  // in, but the title is not up (booting, or a
                                                                 // file loading): never a re-mount under a load
  // Link is in the game (the player's own file or a new game, maybe loaded too early): kept, never switched under it;
  // switched once they are back on the title's menu with no file loaded
  CHECK(pending_step(true, true, true) == PendingStep::keep);
  CHECK(pending_step(false, false, true) == PendingStep::keep);
  CHECK(pending_step(true, false, true) == PendingStep::keep);
}

TEST_CASE("save transfer: a send the full outbox could not take is owed again") {
  using Send = save::Planner::Send;
  const auto t0 = fs::file_time_type::clock::now();
  save::Planner p;
  CHECK(p.on_ask(2, 0));
  const auto first = p.due(t0);
  REQUIRE(first == std::vector<Send>{{2, proto::SaveReason::join}});
  p.retry(first[0]);
  CHECK(p.due(t0) == first);
  // A player who left meanwhile is not sent to.
  p.retry(first[0]);
  p.forget(2);
  p.retry(first[0]);
  CHECK(p.due(t0).empty());
  // The send to everyone after a load comes back too.
  p.on_host_load(t0);
  const auto reload = p.due(t0 + std::chrono::seconds(1));
  REQUIRE(reload == std::vector<Send>{{0, proto::SaveReason::host_load}});
  p.retry(reload[0]);
  CHECK(p.due(t0 + std::chrono::seconds(1)) == reload);
}

TEST_CASE("save transfer: a host's save sent again replaces the co-op slot whole, nothing left beside it") {
  TempDir dir("coop_tests_save_resend");
  const fs::path own = dir.path / "0400486024DEC000";
  const fs::path coop = save::coop_dir_of(own);
  const auto first = Slot(20), second = Slot(30);
  CHECK(save::write_coop_save(coop, first, own) == save::WriteResult::written);
  CHECK(save::write_coop_save(coop, second, own) == save::WriteResult::written);
  CHECK(ReadFile(coop / "slot_00" / "progress.sav") == second.files[0].bytes);
  CHECK(ReadFile(coop / "slot_00" / "caption.sav") == second.files[1].bytes);
  CHECK_FALSE(fs::exists(coop / "slot_00.old"));
  CHECK_FALSE(fs::exists(coop / "slot_00.part"));
  // A stale .old from a cut-off write is never loadable and is replaced.
  WriteFile(coop / "slot_00.old" / "progress.sav", {1});
  CHECK(save::newest_slot(coop) == coop / "slot_00");
  CHECK(save::write_coop_save(coop, first, own) == save::WriteResult::written);
  CHECK(ReadFile(coop / "slot_00" / "progress.sav") == first.files[0].bytes);
  CHECK_FALSE(fs::exists(coop / "slot_00.old"));
}

TEST_CASE("save transfer: a guest that asks again and again gets the save once") {
  using Send = save::Planner::Send;
  const auto t0 = fs::file_time_type::clock::now();
  save::Planner p;
  CHECK(p.on_ask(2, 0));
  for (int i = 0; i < 100; ++i) CHECK_FALSE(p.on_ask(2, 1000 * static_cast<std::uint64_t>(i)));
  CHECK(p.due(t0) == std::vector<Send>{{2, proto::SaveReason::join}});
  for (int i = 0; i < 100; ++i) CHECK_FALSE(p.on_ask(2, 0));
  CHECK(p.idle());
  CHECK(p.due(t0).empty());
  // It left and a player with its number joined: that one may ask.
  p.forget(2);
  CHECK(p.on_ask(2, 0));
  CHECK(p.due(t0) == std::vector<Send>{{2, proto::SaveReason::join}});
}

TEST_CASE("save transfer: an ask again after 10 s is answered again (a lost answer, a rejoin before PLAYER_LEFT)") {
  // Revirie 0.10.1: the guest closed the game and joined again; the host still held its first ask as answered
  // ("once a player") and sent nothing, so the guest's title never got a Continue.
  using Send = save::Planner::Send;
  const auto t0 = fs::file_time_type::clock::now();
  const std::uint64_t s = 1'000'000;
  save::Planner p;
  CHECK(p.on_ask(2, 5 * s));
  CHECK(p.due(t0) == std::vector<Send>{{2, proto::SaveReason::join}});
  CHECK_FALSE(p.on_ask(2, 5 * s + save::Planner::kReaskUs - 1));
  CHECK(p.on_ask(2, 5 * s + save::Planner::kReaskUs));
  CHECK(p.due(t0) == std::vector<Send>{{2, proto::SaveReason::join}});
  // and the 10 s start over from that one
  CHECK_FALSE(p.on_ask(2, 5 * s + save::Planner::kReaskUs + 3 * s));
  // another player is never held up by it
  CHECK(p.on_ask(3, 5 * s + save::Planner::kReaskUs + 3 * s));
}

TEST_CASE("save transfer: a guest asks again when nothing at all came, or the transfer stopped part way") {
  const std::uint64_t s = 1'000'000;
  save::GuestSession g;
  CHECK_FALSE(save::reask_due(g, 100 * s));   // never asked: the first ask is not a re-ask
  g.asked = true;
  g.asked_us = 10 * s;
  g.asks = 1;
  CHECK_FALSE(save::reask_due(g, 10 * s + save::kGuestReaskUs - 1));
  CHECK(save::reask_due(g, 10 * s + save::kGuestReaskUs));
  // parts came after the ask, none kept yet: nothing to ask for
  save::GuestSession part = g;
  part.last_part_us = 11 * s;
  CHECK_FALSE(save::reask_due(part, 100 * s));
  // a transfer under way (a part came just now), then stopped part way: asked again once it is quiet
  save::GuestSession stalled = g;
  const auto parts = save::split(save::pack(Slot(5)), 3, proto::SaveReason::join, 2);
  REQUIRE(parts.size() > 2);
  REQUIRE(stalled.assembler.add(Relayed(parts[0], 1), 1) == save::Assembler::Result::partial);
  stalled.last_part_us = 12 * s;
  CHECK_FALSE(save::reask_due(stalled, 12 * s + save::kGuestReaskUs - 1));   // still coming
  CHECK(save::reask_due(stalled, 12 * s + save::kGuestReaskUs));
  // just asked again: not again before kGuestReaskUs (the old parts are still kept)
  stalled.asked_us = 12 * s + save::kGuestReaskUs;
  stalled.asks = 2;
  CHECK_FALSE(save::reask_due(stalled, stalled.asked_us + save::kGuestReaskUs - 1));
  CHECK(save::reask_due(stalled, stalled.asked_us + save::kGuestReaskUs));
  // the new transfer's parts come in: not while they come
  stalled.last_part_us = stalled.asked_us + 2 * s;
  CHECK_FALSE(save::reask_due(stalled, stalled.asked_us + save::kGuestReaskUs));
  // the save is in, written, or the host said it has none
  save::GuestSession in = g;
  in.unwritten = save::SaveSlot{};
  CHECK_FALSE(save::reask_due(in, 100 * s));
  save::GuestSession ready = g;
  ready.ready = true;
  CHECK_FALSE(save::reask_due(ready, 100 * s));
  save::GuestSession none = g;
  none.no_save = true;
  CHECK_FALSE(save::reask_due(none, 100 * s));
  // at most kGuestMaxAsks a session
  save::GuestSession many = g;
  many.asks = save::kGuestMaxAsks;
  CHECK_FALSE(save::reask_due(many, 100 * s));
}

TEST_CASE("save transfer: a guest gives up once after its last ask, and Loading ends") {
  using N = save::GuestNotices;
  const std::uint64_t s = 1'000'000;
  save::GuestSession g;
  g.asked = true;
  g.asked_us = 10 * s;
  g.asks = save::kGuestMaxAsks - 1;
  CHECK_FALSE(save::give_up_due(g, 100 * s));   // asks left: it asks again instead
  CHECK(save::reask_due(g, 100 * s));
  // the last ask: a transfer starts, then stops part way
  N n;
  n.on_in_game(false);
  g.asks = save::kGuestMaxAsks;
  const auto parts = save::split(save::pack(Slot(5)), 3, proto::SaveReason::join, 2);
  REQUIRE(parts.size() > 2);
  REQUIRE(g.assembler.add(Relayed(parts[0], 1), 1) == save::Assembler::Result::partial);
  g.last_part_us = 12 * s;
  CHECK(n.on_part(parts[0].transfer_id, parts.size()) == N::kLoading);
  CHECK_FALSE(save::give_up_due(g, 12 * s + save::kGuestReaskUs - 1));   // still coming
  CHECK_FALSE(save::reask_due(g, 12 * s + save::kGuestReaskUs));         // no asks left
  REQUIRE(save::give_up_due(g, 12 * s + save::kGuestReaskUs));
  CHECK(n.on_gave_up() == N::kGaveUp);
  g.gave_up = true;
  CHECK_FALSE(n.loading());
  CHECK_FALSE(save::give_up_due(g, 100 * s));   // said once
  // the player loads their own save later: not "too early"
  CHECK(n.on_in_game(true).empty());
  // nothing at all came after the last ask: gives up too; never once the save is in
  save::GuestSession quiet;
  quiet.asked = true;
  quiet.asked_us = 10 * s;
  quiet.asks = save::kGuestMaxAsks;
  CHECK(save::give_up_due(quiet, 10 * s + save::kGuestReaskUs));
  save::GuestSession ready = quiet;
  ready.ready = true;
  CHECK_FALSE(save::give_up_due(ready, 100 * s));
  CHECK(N::kGaveUp == "The host's save did not come: play on, or join again to get it.");
}

TEST_CASE("save transfer: after giving up, a new transfer from the host can give up again") {
  using N = save::GuestNotices;
  const std::uint64_t s = 1'000'000;
  save::GuestSession g;
  N n;
  n.on_in_game(false);
  g.asked = true;
  g.asked_us = 10 * s;
  g.asks = save::kGuestMaxAsks;
  const auto first = save::split(save::pack(Slot(5)), 3, proto::SaveReason::join, 2);
  REQUIRE(g.assembler.add(Relayed(first[0], 1), 1) == save::Assembler::Result::partial);
  save::note_part(g, 3, 12 * s);
  CHECK(n.on_part(3, first.size()) == N::kLoading);
  REQUIRE(save::give_up_due(g, 12 * s + save::kGuestReaskUs));
  CHECK(n.on_gave_up() == N::kGaveUp);
  g.gave_up = true;
  // more parts of the same transfer: still given up
  save::note_part(g, 3, 40 * s);
  CHECK(g.gave_up);
  CHECK_FALSE(save::give_up_due(g, 40 * s + save::kGuestReaskUs));
  // the host loads another save: a new transfer starts, Loading again, and it stalls too
  const auto second = save::split(save::pack(Slot(6)), 4, proto::SaveReason::host_load, 2);
  REQUIRE(g.assembler.add(Relayed(second[0], 1), 1) == save::Assembler::Result::partial);
  save::note_part(g, 4, 50 * s);
  CHECK_FALSE(g.gave_up);
  CHECK(n.on_part(4, second.size()) == N::kLoading);
  CHECK(n.loading());
  CHECK_FALSE(save::give_up_due(g, 50 * s + save::kGuestReaskUs - 1));
  REQUIRE(save::give_up_due(g, 50 * s + save::kGuestReaskUs));
  CHECK(n.on_gave_up() == N::kGaveUp);
  CHECK_FALSE(n.loading());
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
  // a guest that joins with a save loaded (the pause menu's Join) asks for the host's save too: before, it kept its
  // own saves and never asked, so nothing of the host's unshared state reached it (player reports 107, 120)
  CHECK(folder_choice(std::nullopt, true, true, false, true) == FolderChoice::coop_later);
  CHECK(folder_choice(std::nullopt, true, true, true, true) == FolderChoice::own);      // the host, a save loaded
  CHECK(folder_choice(std::nullopt, true, false, false, true) == FolderChoice::wait);
  // the emulator's fixed folder never changes under a game: the variable only
  CHECK(folder_choice(std::nullopt, false, true, false, true) == FolderChoice::own);
  CHECK(folder_choice(true, false, true, false, true) == FolderChoice::coop);
}

TEST_CASE("save transfer: a guest that joins with a file loaded asks and gets the save on the title (reports 107, 120)") {
  using N = save::GuestNotices;
  // The folder played from is not touched: the slot that came waits (pending_step) until the title's menu is up.
  const Bytes packed = save::pack(Slot(4));
  save::GuestSession s;
  N n{true};   // (Link is in the game when the session starts)
  CHECK(n.on_in_game(true).empty());            // the file played was loaded before the ask: never "too early"
  s.asked = true;
  const auto parts = save::split(packed, 31, proto::SaveReason::join, 2);
  REQUIRE(parts.size() > 1);
  CHECK(n.on_part(31, parts.size()) == N::kLoading);
  CHECK(n.on_in_game(true).empty());
  for (const auto& part : parts) {
    save::note_part(s, part.transfer_id, 1);
    if (s.assembler.add(Relayed(part, 1), 1) == save::Assembler::Result::complete) s.unwritten = save::unpack(s.assembler.take());
  }
  REQUIRE(s.unwritten);
  CHECK_FALSE(save::reask_due(s, 1 + save::kGuestReaskUs));   // the save is in: no more asks while it waits
  using save::PendingStep;
  CHECK(save::pending_step(true, false, true) == PendingStep::keep);   // in the game: nothing changes
  CHECK(save::pending_step(true, true, true) == PendingStep::keep);    // (the title's mark with Link still there)
  CHECK(save::pending_step(true, true, false) == PendingStep::go);     // back on the title: written and switched to
  CHECK(n.on_in_game(false).empty());
  CHECK(n.on_ready(false) == N::kSuccessTitle);
  CHECK(n.on_in_game(true).empty());            // loading the host's save after Success
  // The line the guest reads when it joins says where the save is taken.
  CHECK(N::kJoinedPlaying.find("go back to the title") != std::string_view::npos);
  CHECK(N::kJoinedPlaying.find("Success") != std::string_view::npos);
  CHECK(N::kTitleAsked.find("Success") != std::string_view::npos);  // the title's own ask (review of #278)
}

TEST_CASE("save transfer: a new session with a file still loaded chooses again without the title") {
  using save::rejoined_in_game;
  // The last session ended during play (decide_again keeps its choice: no switch under a game being played); the
  // player joins or hosts again from the pause menu. Before, the old choice stayed and the new session had none: a
  // guest did not ask, and Resync from host said "not a guest in a session".
  CHECK_FALSE(save::decide_again(true, true, true));
  CHECK(rejoined_in_game(true, true, true, true));
  CHECK_FALSE(rejoined_in_game(true, false, true, true));   // the same session (a reconnect is not an end)
  CHECK_FALSE(rejoined_in_game(true, true, false, true));   // not connected to a host yet
  CHECK_FALSE(rejoined_in_game(true, true, true, false));   // on the title: decide_again's reset decides
  CHECK_FALSE(rejoined_in_game(false, true, true, true));   // nothing chosen yet: the first choice decides
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

TEST_CASE("save transfer: the guest's lines come in order: loading, (too early), success") {
  using N = save::GuestNotices;
  // at the title: loading once per transfer, then success at the title
  N n;
  CHECK(n.on_in_game(false).empty());
  CHECK(n.on_part(7, 11) == N::kLoading);
  CHECK(n.on_part(7, 11).empty());   // (the same transfer's other parts)
  CHECK(n.loading());
  CHECK(n.on_in_game(false).empty());
  CHECK(n.on_ready(false) == N::kSuccessTitle);
  CHECK_FALSE(n.loading());
  CHECK(n.on_in_game(true).empty());   // (loading a save after Success: nothing to say)
  // the player loads a save while it loads: told once to load again after Success
  N early;
  early.on_in_game(false);
  CHECK(early.on_part(3, 11) == N::kLoading);
  CHECK(early.on_in_game(true) == N::kTooEarly);
  CHECK(early.on_in_game(true).empty());
  early.on_in_game(false);
  CHECK(early.on_in_game(true).empty());   // (once a transfer)
  CHECK(early.on_ready(true) == N::kSuccessPlaying);
  // during play (a join in the game, or the host loaded another save): no "too early" for being in the game,
  // success says the changes need a load
  N playing;
  playing.on_in_game(true);
  CHECK(playing.on_part(9, 11) == N::kLoading);
  CHECK(playing.on_in_game(true).empty());
  CHECK(playing.on_ready(true) == N::kSuccessPlaying);
  // the host loads another save later: a new transfer says loading again
  CHECK(playing.on_part(10, 11) == N::kLoading);
  CHECK(playing.on_ready(true) == N::kSuccessPlaying);
  // the owner's exact wording
  CHECK(N::kLoading == "Host save loading, don't load a save.");
  CHECK(N::kSuccessTitle == "Success, load any save.");
  CHECK(N::kSuccessPlaying == "Success, load any save for changes to occur.");
}

TEST_CASE("save transfer: no Loading for a no-save answer, a failed transfer ends Loading") {
  using N = save::GuestNotices;
  // the host has no save: its answer is one part; no Loading, and loading a save then is not "too early"
  N none;
  none.on_in_game(false);
  CHECK(none.on_part(4, 1).empty());
  CHECK_FALSE(none.loading());
  none.on_no_save();
  CHECK(none.on_in_game(true).empty());
  // a save that came damaged (or could not be read, written or switched to): Loading ends, no "too early" after
  N failed;
  failed.on_in_game(false);
  CHECK(failed.on_part(5, 11) == N::kLoading);
  failed.on_failed();
  CHECK_FALSE(failed.loading());
  CHECK(failed.on_in_game(true).empty());
  // the host's next send is a new transfer: Loading again
  failed.on_in_game(false);
  CHECK(failed.on_part(6, 11) == N::kLoading);
  CHECK(failed.loading());
}

TEST_CASE("save transfer: loaded too early, the save still comes and is switched to back on the title") {
  using N = save::GuestNotices;
  using save::PendingStep;
  using save::pending_step;
  N n;
  bool slot_in = false, in_game = false, title = true;
  n.on_in_game(in_game);
  CHECK(n.on_part(8, 11) == N::kLoading);
  // the player loads their own file while it comes: told to go back to the title after Success
  title = false;
  in_game = true;
  CHECK(n.on_in_game(in_game) == N::kTooEarly);
  // the rest comes in while they play: kept, never switched under the game
  slot_in = true;
  CHECK(pending_step(slot_in, title, in_game) == PendingStep::keep);
  // back on the title's menu with no file loaded: switched, then Success (at the title)
  in_game = false;
  title = true;
  CHECK(n.on_in_game(in_game).empty());
  CHECK(pending_step(slot_in, title, in_game) == PendingStep::go);
  CHECK(n.on_ready(in_game) == N::kSuccessTitle);
}

// Player report 120: the host answered joins with its newest slot on disk by write time, not the slot its game
// plays. A player who loads an older slot (or saved another one earlier) gave the guests the wrong save
// (roxas7833's host log: "sending slot_02 ... to player 2", then its game played slot_01).
TEST_CASE("save transfer: the slot sent is the one the host's game plays, not the newest on disk (report 120)") {
  TempDir dir("coop_tests_save_played");
  const fs::path game = dir.path / "5a3c";
  WriteFile(game / "slot_01" / "progress.sav", {1});
  WriteFile(game / "slot_02" / "progress.sav", {2});
  const auto t = fs::last_write_time(game / "slot_01" / "progress.sav");
  fs::last_write_time(game / "slot_02" / "progress.sav", t + std::chrono::hours(1));  // slot_02 saved last
  REQUIRE(save::newest_slot(game) == game / "slot_02");

  bool used = true;
  CHECK(save::slot_to_send(game, std::nullopt, &used) == game / "slot_02");  // not known: the newest, as before
  CHECK_FALSE(used);
  CHECK(save::slot_to_send(game, game / "slot_01", &used) == game / "slot_01");  // the played one
  CHECK(used);
  // Never a folder that is not one of this game's slots, or a slot with no progress.sav: the newest then.
  CHECK(save::slot_to_send(game, dir.path / "other" / "slot_01", &used) == game / "slot_02");
  CHECK_FALSE(used);
  CHECK(save::slot_to_send(game, game / "slot_04", &used) == game / "slot_02");
  CHECK_FALSE(used);
  CHECK(save::slot_to_send(game, game / "slot_01.part", &used) == game / "slot_02");
  CHECK_FALSE(used);
}

namespace {
// Patch 0025's export as the module sees it: a count of opens and the last one. Marks (the game thread's load
// window) and polls (once a frame) each read it at their own moment.
struct Opens {
  unsigned n = 0;
  fs::path dir;
  bool write = false;
  void open(const fs::path& d, bool w) {
    ++n;
    dir = d;
    write = w;
  }
  void mark(save::PlayedSlot& p, bool start) const { p.on_load_window(start, n, dir, write); }
  void poll(save::PlayedSlot& p, unsigned& seen) const {
    if (n != seen && !dir.empty()) {
      seen = n;
      p.on_open(dir, write, n);
    }
  }
};
}  // namespace

TEST_CASE("save transfer: the played slot comes only from definite signs (writes, reads in a load window)") {
  using S = save::PlayedSlot::Source;
  const fs::path a = "save/slot_00", b = "save/slot_01", c = "save/slot_02";
  Opens fs_;
  unsigned seen = 0;

  SUBCASE("a runtime with load windows (patch 0024): the title's reads never count, the load's read does") {
    save::PlayedSlot p;
    p.on_title();
    fs_.open(a, false);  // the title's list may read every slot
    fs_.poll(p, seen);
    fs_.open(b, false);
    fs_.open(c, false);
    fs_.poll(p, seen);
    CHECK_FALSE(p.slot());
    fs_.mark(p, true);   // Continue on slot_01
    fs_.open(b, false);
    fs_.poll(p, seen);
    CHECK_FALSE(p.slot());  // (only once the save is in)
    fs_.mark(p, false);
    CHECK(p.slot() == b);
    CHECK(p.source() == S::load_read);
    p.on_load_seen();       // the flag watcher's sight of the same load changes nothing
    CHECK(p.slot() == b);
    fs_.open(c, false);     // a read outside a load (the System menu's list): not the played slot
    fs_.poll(p, seen);
    CHECK(p.slot() == b);
  }
  SUBCASE("review: start, the read and the save in all before one poll: the read is not lost") {
    save::PlayedSlot p;
    p.on_title();
    fs_.open(c, false);
    fs_.poll(p, seen);
    fs_.mark(p, true);
    fs_.open(b, false);
    fs_.mark(p, false);   // (the marks are applied first at the poll, each with its own count)
    fs_.poll(p, seen);
    CHECK(p.slot() == b);
    CHECK(p.source() == S::load_read);
  }
  SUBCASE("review: a read just before the load's start, polled after it, is not taken as the load's") {
    save::PlayedSlot p;
    p.on_title();
    fs_.open(c, false);   // the title list's last read, not polled yet
    fs_.mark(p, true);    // the start, applied before the poll's open
    fs_.poll(p, seen);    // count unchanged since the start: before the window
    fs_.mark(p, false);   // a load with no read in it
    CHECK_FALSE(p.slot());
  }
  SUBCASE("the game's own start (a load start with no save in) does not make the title's reads a load") {
    save::PlayedSlot p;
    fs_.mark(p, true);   // BeforeInitializeCommon's load start
    p.on_title();
    fs_.open(c, false);
    fs_.poll(p, seen);
    p.on_load_seen();
    CHECK_FALSE(p.slot());
  }
  SUBCASE("a save into a slot is the slot played (a new game's first save, a save point, a door's autosave)") {
    save::PlayedSlot p;
    p.on_title();
    CHECK_FALSE(p.new_game(false, true));
    CHECK_FALSE(p.new_game(true, true));   // no windows on this runtime yet: never guessed
    fs_.mark(p, true);                     // (a runtime with windows)
    fs_.mark(p, false);                    // a load with no read in it (a temporary one): slot not known
    CHECK_FALSE(p.slot());
    p.on_title();
    CHECK(p.new_game(true, true));         // New Game: in the game with no load since the title
    CHECK_FALSE(p.new_game(true, false));  // (the title never seen this run: a load may have come before)
    fs_.open(c, true);
    fs_.poll(p, seen);
    CHECK(p.slot() == c);
    CHECK(p.source() == S::write);
    CHECK_FALSE(p.new_game(true, true));
  }
  SUBCASE("System > Load, Try Again: a new load's read replaces the slot, a temporary load keeps it") {
    save::PlayedSlot p;
    fs_.mark(p, true);
    fs_.open(a, false);
    fs_.mark(p, false);
    CHECK(p.slot() == a);
    fs_.mark(p, true);   // System > Load of slot_02
    fs_.open(c, false);
    fs_.mark(p, false);
    CHECK(p.slot() == c);
    fs_.mark(p, true);   // Game Over's Try Again from temporary storage: no slot read
    fs_.mark(p, false);
    CHECK(p.slot() == c);
  }
  SUBCASE("no load windows (no patch 0024): the last read when the load is seen") {
    save::PlayedSlot p;
    fs_.open(a, false);
    fs_.poll(p, seen);
    fs_.open(b, false);
    fs_.poll(p, seen);
    CHECK_FALSE(p.slot());
    p.on_load_seen();
    CHECK(p.slot() == b);
    CHECK(p.source() == S::last_read);
  }
  SUBCASE("the title's menu forgets the slot (a new pick follows)") {
    save::PlayedSlot p;
    fs_.open(a, true);
    fs_.poll(p, seen);
    CHECK(p.slot() == a);
    p.on_title();
    CHECK_FALSE(p.slot());
    p.on_load_seen();  // no read since the title: still not known (never the read from before it)
    CHECK_FALSE(p.slot());
  }
}

TEST_CASE("save transfer: a host load of a known slot goes to every guest at once, not after the next save") {
  using Send = save::Planner::Send;
  const auto t0 = fs::file_time_type::clock::now();
  save::Planner p;
  p.on_host_load(t0 + std::chrono::seconds(30), true);
  CHECK_FALSE(p.waiting());
  CHECK(p.due(t0) == std::vector<Send>{{0, proto::SaveReason::host_load}});  // the slot on disk is the one loaded
  CHECK(p.idle());
  // Not known: waits for the next save, as before.
  p.on_host_load(t0 + std::chrono::seconds(30), false);
  CHECK(p.waiting());
  CHECK(p.due(t0).empty());
}

TEST_CASE("save transfer: a guest back on the title after playing asks for the host's save again, only then") {
  const save::TitleResync go{.title_came = true, .connected_guest = true, .played_in_session = true, .on = true,
                             .can_switch = true, .plays_hosts_save = true, .ask_waiting = false};
  CHECK(save::title_resync_due(go));
  auto t = go;
  t.title_came = false;  // not a return to the title
  CHECK_FALSE(save::title_resync_due(t));
  t = go;
  t.connected_guest = false;  // the host, a game alone, or a session that just ended
  CHECK_FALSE(save::title_resync_due(t));
  t = go;
  t.played_in_session = false;  // nothing played since joining (asked on joining)
  CHECK_FALSE(save::title_resync_due(t));
  t = go;
  t.on = false;  // LICHII_COOP_TITLE_RESYNC=0
  CHECK_FALSE(save::title_resync_due(t));
  t = go;
  t.can_switch = false;  // the emulator's fixed folder: no line at every title
  CHECK_FALSE(save::title_resync_due(t));
  t = go;
  t.plays_hosts_save = false;  // LICHII_COOP_TEST_OWN_SAVE=1, or a failed folder switch: keeps its own saves
  CHECK_FALSE(save::title_resync_due(t));
  t = go;
  t.ask_waiting = true;  // joined with a file loaded (its ask waits for the title), or a save on its way
  CHECK_FALSE(save::title_resync_due(t));
}

TEST_CASE("save transfer: Resync from host goes only for a guest that can switch, and not twice at once") {
  using save::ResyncCheck;
  CHECK(save::resync_check(true, true, false) == ResyncCheck::go);
  CHECK(save::resync_check(false, true, false) == ResyncCheck::not_guest);  // the host, solo, not connected
  CHECK(save::resync_check(true, false, false) == ResyncCheck::no_switch);  // the emulator's fixed folder
  CHECK(save::resync_check(true, true, true) == ResyncCheck::coming);       // a transfer on its way
  // After a go, the guest's session starts again as on joining: it asks at once and the save waits for the title.
  save::GuestSession g;
  CHECK_FALSE(g.asked);
  CHECK(save::pending_step(true, false, true) == save::PendingStep::keep);  // playing: never switched under the game
  CHECK(save::pending_step(true, true, false) == save::PendingStep::go);     // the title's menu: switched
}

TEST_CASE("save transfer: the first multiplayer saves are a whole copy, never a half folder") {
  TempDir t("coop_copy_dir_whole_test");
  const fs::path solo = t.path / "account";
  const fs::path mp = t.path / "account-mp";
  fs::create_directories(solo / "slot_00");
  { std::ofstream(solo / "slot_00" / "progress.sav", std::ios::binary) << "save"; }
  { std::ofstream(solo / "option.sav", std::ios::binary) << "opt"; }
  // A half copy an earlier try left behind is thrown away, not used.
  fs::create_directories(t.path / "account-mp.part" / "junk");
  std::string why;
  REQUIRE(save::copy_dir_whole(solo, mp, &why));
  CHECK(fs::exists(mp / "slot_00" / "progress.sav"));
  CHECK(fs::exists(mp / "option.sav"));
  CHECK_FALSE(fs::exists(mp / "junk"));
  CHECK_FALSE(fs::exists(t.path / "account-mp.part"));
  CHECK(fs::exists(solo / "slot_00" / "progress.sav"));  // the solo saves are only read
  // Once there it is never made again (nothing touched).
  { std::ofstream(mp / "option.sav", std::ios::binary) << "played"; }
  CHECK_FALSE(save::copy_dir_whole(solo, mp, &why));
  std::ifstream kept(mp / "option.sav", std::ios::binary);
  CHECK(std::string(std::istreambuf_iterator<char>(kept), {}) == "played");
}

TEST_CASE("save transfer: a copy with nothing to copy from leaves no folder") {
  TempDir t("coop_copy_dir_whole_missing_test");
  std::string why;
  CHECK_FALSE(save::copy_dir_whole(t.path / "none", t.path / "none-mp", &why));
  CHECK_FALSE(why.empty());
  CHECK_FALSE(fs::exists(t.path / "none-mp"));
  CHECK_FALSE(fs::exists(t.path / "none-mp.part"));
}

TEST_CASE("save transfer: an ask while the host plays a file waits for the save its game is asked for (case 1)") {
  using Send = save::Planner::Send;
  constexpr std::uint64_t kLimit = save::Planner::kFreshSaveUs;
  static_assert(kLimit < save::kGuestReaskUs);   // the guest does not ask again while the host waits
  const auto t0 = fs::file_time_type::clock::now();
  const auto ask = t0 + std::chrono::seconds(60);   // the host's last door was a minute before the ask
  const auto saved = ask + std::chrono::seconds(1);

  SUBCASE("asked and saved in time: the guest gets the save written after the ask") {
    save::Planner p;
    CHECK(p.on_ask(2, 1'000, ask));
    CHECK(p.round_open());
    CHECK(p.take_save_request());
    CHECK_FALSE(p.take_save_request());   // once a round
    CHECK(p.due(t0, 2'000).empty());       // only the older save on disk: it waits
    CHECK(p.due(saved, 3'000) == std::vector<Send>{{2, proto::SaveReason::join, false, true}});
    CHECK_FALSE(p.round_open());
    CHECK(p.idle());
  }
  SUBCASE("asked and the game refused (a cutscene, a boss room, its cooldown): the last save after the limit") {
    save::Planner p;
    CHECK(p.on_ask(2, 1'000, ask));
    CHECK(p.take_save_request());
    CHECK(p.due(t0, 1'000 + kLimit - 1).empty());
    CHECK(p.due(t0, 1'000 + kLimit) == std::vector<Send>{{2, proto::SaveReason::join, false, false}});
    CHECK(p.idle());
    CHECK_FALSE(p.take_save_request());
  }
  SUBCASE("asked on the title or in a new game never saved: at once, as before (no round, no request)") {
    save::Planner p;
    CHECK(p.on_ask(2, 1'000));   // (the hook gives no fresh_after there)
    CHECK_FALSE(p.round_open());
    CHECK_FALSE(p.take_save_request());
    CHECK(p.due(t0, 1'000) == std::vector<Send>{{2, proto::SaveReason::join}});
    // a new game never saved: the hook passes no newest write, so "no save yet" (player report 120), round or not
    CHECK(p.on_ask(3, 2'000, ask));
    CHECK(p.due(std::nullopt, 2'000) == std::vector<Send>{{3, proto::SaveReason::join, true}});
    CHECK_FALSE(p.round_open());
  }
  SUBCASE("two guests asking close together: one request, two sends of the same save") {
    save::Planner p;
    CHECK(p.on_ask(2, 1'000, ask));
    CHECK(p.take_save_request());
    CHECK(p.on_ask(3, 500'000, ask + std::chrono::milliseconds(500)));   // joins the open round
    CHECK_FALSE(p.take_save_request());
    CHECK(p.due(saved, 600'000) == std::vector<Send>{{2, proto::SaveReason::join, false, true},
                                                     {3, proto::SaveReason::join, false, true}});
    // a later ask opens a new round of its own
    CHECK(p.on_ask(4, 20'000'000, saved + std::chrono::seconds(20)));
    CHECK(p.take_save_request());
  }
  SUBCASE("the host alone: no ask, no round, no request") {
    save::Planner p;
    CHECK(p.idle());
    CHECK_FALSE(p.take_save_request());
    CHECK(p.due(saved, 1'000).empty());
  }
  SUBCASE("the asker left during the round: the round ends with it") {
    save::Planner p;
    CHECK(p.on_ask(2, 1'000, ask));
    p.forget(2);
    CHECK_FALSE(p.round_open());
    CHECK(p.due(saved, 2'000).empty());
  }
  SUBCASE("a fresh send that could not go out stays fresh at its retry") {
    save::Planner p;
    CHECK(p.on_ask(2, 1'000, ask));
    const auto sends = p.due(saved, 2'000);
    REQUIRE(sends.size() == 1);
    p.retry(sends.front());
    CHECK(p.due(saved, 3'000) == std::vector<Send>{{2, proto::SaveReason::join, false, true}});
  }
  SUBCASE("the host loads a save during the round: the save after the load goes to everyone") {
    save::Planner p;
    CHECK(p.on_ask(2, 1'000, ask));
    p.on_host_load(ask);
    CHECK(p.due(saved, 2'000) == std::vector<Send>{{0, proto::SaveReason::host_load}});
    CHECK_FALSE(p.round_open());
    CHECK(p.idle());
  }
}

TEST_CASE("save transfer: a rejoin with the game on an earlier host's save goes back to its own folder on the title") {
  // (AskAtTitle while the co-op folder holds the OLDER host's save: never offered by the title)
  CHECK(save::old_coop_first(true, false, true, false, true));
  CHECK_FALSE(save::old_coop_first(true, false, false, true, true));   // a file played: nothing under it
  CHECK_FALSE(save::old_coop_first(true, true, true, false, true));    // the new save is in: written and switched
  CHECK_FALSE(save::old_coop_first(true, false, true, false, false));  // on its own folder already (a first join)
  CHECK_FALSE(save::old_coop_first(false, false, true, false, true));  // nothing asked (solo, the host)
  CHECK_FALSE(save::old_coop_first(true, false, false, false, true));  // a pick loading (suspended): after it
}

TEST_CASE("save transfer: a failed write or switch ends the ask, so Resync is not 'already on its way' for good") {
  constexpr std::uint64_t s = 1'000'000;
  save::GuestSession g;
  g.asked = true;
  CHECK(save::save_coming(g, 5 * s, 10 * s));   // asked, nothing yet: coming
  g.unwritten = Slot(5);
  CHECK(save::save_coming(g, 5 * s, 10 * s));   // waiting for the title
  save::note_write_failed(g);                   // the write or the switch failed
  CHECK_FALSE(g.unwritten.has_value());
  CHECK_FALSE(g.ready);
  CHECK_FALSE(save::save_coming(g, 6 * s, 10 * s));
  CHECK(save::resync_check(true, true, save::save_coming(g, 6 * s, 10 * s)) == save::ResyncCheck::go);
  // a fresh session (Resync's go) asks again
  g = save::GuestSession{};
  CHECK_FALSE(g.asked);
}

TEST_CASE("save transfer: every state that ends the ask also ends 'coming'") {
  constexpr std::uint64_t s = 1'000'000;
  save::GuestSession g;
  g.asked = true;
  g.no_save = true;   // the host has no save: nothing coming
  CHECK_FALSE(save::save_coming(g, 5 * s, 10 * s));
  g = save::GuestSession{};
  g.asked = true;
  g.gave_up = true;   // gave up after the asks
  CHECK_FALSE(save::save_coming(g, 5 * s, 10 * s));
  g = save::GuestSession{};
  g.asked = true;
  save::note_write_failed(g);   // unreadable slot / failed write / failed switch
  CHECK_FALSE(save::save_coming(g, 5 * s, 10 * s));
  save::note_part(g, 7, 6 * s);   // a new transfer clears gave_up again
  CHECK_FALSE(g.gave_up);
}
