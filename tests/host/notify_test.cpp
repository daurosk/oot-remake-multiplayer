#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "coop/host/flag_watcher.hpp"
#include "coop/host/flags.hpp"
#include "coop/host/notify.hpp"
#include "coop/protocol/codec.hpp"

using coop::host::FlagWatcher;
using coop::host::Notifier;
namespace proto = coop::protocol;

namespace {

using Kind = FlagWatcher::ReceivedKind;

FlagWatcher::Received Bool(const char* name, std::uint8_t from, Kind kind = Kind::live) {
  const proto::FlagKey key(proto::flag_hash(name), std::uint16_t{0});
  return {key, proto::FlagEntry{key, proto::FlagKind::boolean, 0}, proto::FlagEntry{key, proto::FlagKind::boolean, 1},
          from, kind};
}

FlagWatcher::Received Int(const char* name, std::int32_t before, std::int32_t after, std::uint8_t from,
                          Kind kind = Kind::live) {
  const proto::FlagKey key(proto::flag_hash(name), std::uint16_t{0});
  return {key, proto::FlagEntry{key, proto::FlagKind::int32, static_cast<std::uint32_t>(before)},
          proto::FlagEntry{key, proto::FlagKind::int32, static_cast<std::uint32_t>(after)}, from, kind};
}

Notifier Make() {
  return Notifier([](std::uint8_t n) { return n == 2 ? std::string("Tobi") : std::string(); });
}

}  // namespace

TEST_CASE("notify: names a player knows") {
  CHECK(coop::host::ItemName("Weapon_Range_SlingShot") == "Fairy Slingshot");
  CHECK(coop::host::ItemName("Item_PocketEgg") == "Pocket Egg");
  CHECK(coop::host::ItemName("Armor_Child_Mask_Keaton") == "Mask Keaton");
  CHECK(coop::host::SongName("ZeldaLullaby") == "Zelda's Lullaby");
  CHECK(coop::host::DungeonName("InsideTheDekuTree") == "Deku Tree");
  CHECK(coop::host::DungeonName("ForestTemple") == "Forest Temple");
}

TEST_CASE("notify: one line for what another player's change gave us") {
  Notifier n = Make();
  auto lines = n.Take({Bool("IsGet.Weapon_Range_SlingShot", 2)}, 1, 1000);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "Tobi got the Fairy Slingshot - you got one too!");
  lines = n.Take({Int("DungeonInfo.InsideTheDekuTree.SmallKeyNum", 1, 2, 2)}, 1, 1100);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "Tobi found a Small Key (Deku Tree) - the group has 2");
  lines = n.Take({Int("PlayerStatus.MaxLife", 12, 16, 3)}, 1, 1200);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "Player 3 got a Heart Container - your max health went up");  // no name known
  lines = n.Take({Bool("DungeonInfo.InsideTheDekuTree.IsGetMap", 2), Bool("IsGet.Item_Melody_ZeldaLullaby", 2),
                  Int("PouchData.Item_Token_Gold.StockNum", 11, 12, 2)},
                 1, 1300);
  REQUIRE(lines.size() == 3);
  CHECK(lines[0] == "Tobi got the map (Deku Tree)");
  CHECK(lines[1] == "Tobi learned Zelda's Lullaby");
  CHECK(lines[2] == "Tobi got a Gold Skulltula token (12)");
}

TEST_CASE("notify: what is not told") {
  Notifier n = Make();
  CHECK(n.Take({Bool("IsGet.Weapon_Range_SlingShot", 1)}, 1, 1000).empty());       // our own
  CHECK(n.Take({Int("DungeonInfo.InsideTheDekuTree.SmallKeyNum", 2, 1, 2)}, 1, 1000).empty());  // spent
  CHECK(n.Take({Bool("SomeRoomSwitch_NotAnItem", 2)}, 1, 1000).empty());           // not an item
  CHECK(n.Take({Int("PouchData.Item_Bomb.StockNum", 5, 10, 2)}, 1, 1000).empty()); // ammo
}

TEST_CASE("notify: a join and a host's save load are one line each") {
  Notifier n = Make();
  std::vector<FlagWatcher::Received> join;
  for (const char* item : {"IsGet.Weapon_Range_SlingShot", "IsGet.Item_Bomb", "IsGet.Item_DekuNut"}) {
    join.push_back(Bool(item, 0, Kind::join));
  }
  join.push_back(Bool("IsGet.Item_Melody_ZeldaLullaby", 0, Kind::join));
  auto lines = n.Take(join, 1, 1000);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "Synced with the group: 3 items, 1 songs");
  lines = n.Take({Bool("IsGet.Item_Bomb", 2, Kind::reset), Int("PlayerStatus.MaxLife", 12, 16, 2, Kind::reset)}, 1,
                 2000);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "Tobi loaded a save - the group follows it (1 items, 1 heart containers changed)");
}

TEST_CASE("notify: one player's items in one frame are one line, a burst is held back") {
  Notifier n = Make();
  auto lines = n.Take({Bool("IsGet.Item_Bomb", 2), Bool("IsGet.Item_DekuNut", 2)}, 1, 1000);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "Tobi got the Bombs, the Deku Nuts - you got them too!");
  // Four lines in the window at most; the rest is one line once there is room.
  const char* songs[] = {"IsGet.Item_Melody_SunSong", "IsGet.Item_Melody_TimeSong", "IsGet.Item_Melody_StormSong",
                         "IsGet.Item_Melody_EponaSong", "IsGet.Item_Melody_SariaSong"};
  std::size_t said = 1;
  for (std::uint64_t i = 0; i < 5; ++i) {
    said += n.Take({Bool(songs[i], static_cast<std::uint8_t>(2 + i % 2))}, 1, 1100 + i).size();
  }
  CHECK(said == Notifier::kMaxLines);
  CHECK(n.Take({}, 1, 2000).empty());  // still full
  lines = n.Take({}, 1, 1000 + Notifier::kWindowMs + 200);
  REQUIRE(lines.size() == 1);
  CHECK(lines[0] == "...and 2 more from the group");
}

namespace {
class OneFlagStore : public coop::host::FlagStore {
 public:
  proto::FlagEntry value;
  std::optional<proto::FlagEntry> read(coop::host::FlagKey) override { return value; }
  bool write(const proto::FlagEntry& e) override {
    value = e;
    return true;
  }
};
}  // namespace

TEST_CASE("notify: the watcher says who made what it writes") {
  const proto::FlagKey sling(proto::flag_hash("IsGet.Weapon_Range_SlingShot"), std::uint16_t{0});
  OneFlagStore store;
  store.value = proto::FlagEntry{sling, proto::FlagKind::boolean, 0};
  FlagWatcher w(coop::host::SharedFlags({sling}), coop::host::FlagRole::player);
  w.frame(store);  // the baseline
  // The server names the asker (player 3); the host's answer (player 1) follows.
  w.receive(proto::Message{proto::FlagOrigin{3, {proto::FlagEntry{sling, proto::FlagKind::boolean, 1}}}});
  w.receive(proto::Message{proto::FlagState{1, {proto::FlagEntry{sling, proto::FlagKind::boolean, 1}}}});
  w.frame(store);
  auto got = w.take_received();
  REQUIRE(got.size() == 1);
  CHECK(got[0].from == 3);
  CHECK(got[0].kind == FlagWatcher::ReceivedKind::live);
  CHECK(got[0].before.value == 0);
  CHECK(got[0].after.value == 1);
  CHECK(w.take_received().empty());
  // Without an origin: the host that sent it.
  w.receive(proto::Message{proto::FlagState{1, {proto::FlagEntry{sling, proto::FlagKind::boolean, 0}}}});
  w.frame(store);
  got = w.take_received();
  REQUIRE(got.size() == 1);
  CHECK(got[0].from == 1);
}

TEST_CASE("notify: FLAG_ORIGIN is the server's, with a player and flags") {
  const proto::FlagKey key(proto::flag_hash("IsGet.Item_Bomb"), std::uint16_t{0});
  proto::FlagOrigin m{2, {proto::FlagEntry{key, proto::FlagKind::boolean, 1}}};
  const auto bytes = proto::encode(proto::Message{m});
  REQUIRE(bytes.has_value());
  proto::Message back;
  REQUIRE(proto::decode(*bytes, back) == proto::Error::none);
  CHECK(std::get<proto::FlagOrigin>(back) == m);
  CHECK(static_cast<int>(proto::type_of(back)) == 150);
  CHECK(proto::channel_of(back) == proto::kChannelProgress);
  CHECK(proto::check_received(back, proto::Sender::client, proto::kChannelProgress) != proto::Error::none);
  m.player_number = 0;  // a player is named
  CHECK_FALSE(proto::encode(proto::Message{m}).has_value());
}
