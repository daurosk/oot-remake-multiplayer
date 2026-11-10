// Tests for the shared chests outside the dungeons (lichii/hook/coop_chests.cpp)
// and the keys the flag watcher writes for other players (docs/items.md, "Chests").
#include <doctest/doctest.h>

#include <map>
#include <set>
#include <string>
#include <string_view>

#include "coop_chests.h"
#include "coop_dungeon.h"
#include "coop/host/flag_watcher.hpp"
#include "coop/protocol/codec.hpp"

namespace host = coop::host;
namespace proto = coop::protocol;

namespace {

class Game : public host::FlagStore {
public:
    std::map<host::FlagKey, proto::FlagEntry> values;
    void set(host::FlagKey key, std::uint32_t v) { values[key] = {key, proto::FlagKind::boolean, v}; }
    std::optional<proto::FlagEntry> read(host::FlagKey key) override {
        const auto it = values.find(key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    bool write(const proto::FlagEntry& e) override {
        if (!values.contains(e.key)) return false;
        values[e.key] = e;
        return true;
    }
};

const proto::FlagSet* AsSet(const std::vector<host::Outgoing>& out, proto::Message& m) {
    if (out.size() != 1 || proto::decode(out[0].bytes, m) != proto::Error::none) return nullptr;
    return std::get_if<proto::FlagSet>(&m);
}

} // namespace

TEST_CASE("chests: the list is the chests outside the dungeons, once each, without the shop's") {
    std::set<std::uint64_t> keys;
    std::set<std::uint64_t> dungeon;
    for (const auto& k : CoopDungeon::Keys()) dungeon.insert(k.key);
    for (const auto& k : CoopChests::Keys()) {
        CHECK(keys.insert(k.key).second);
        CHECK_FALSE(dungeon.contains(k.key));
        CHECK(std::string_view(k.actor).find("TBox") != std::string_view::npos);
        CHECK(std::string_view(k.actor).find("Shop_TBox") == std::string_view::npos);
    }
    CHECK(keys.size() == 60);
    bool kokiri = false;
    for (const auto& k : CoopChests::Keys()) {
        kokiri |= std::string(k.area) == "KokiriForest";
    }
    CHECK(kokiri);
}

TEST_CASE("chests: Merge adds the keys, or nothing when off") {
    std::optional<host::SharedFlags> none;
    CHECK(CoopChests::Merge(none, false) == 0);
    CHECK_FALSE(none.has_value());
    CHECK(CoopChests::Merge(none, true) == CoopChests::Keys().size());
    REQUIRE(none.has_value());
    CHECK(none->contains(host::FlagKey::key64_flag(CoopChests::Keys()[0].key)));
    // With the dungeon list first, nothing is counted twice.
    std::optional<host::SharedFlags> both;
    CoopDungeon::Merge(both, true);
    const std::size_t dungeon = both->size();
    CHECK(CoopChests::Merge(both, true) == CoopChests::Keys().size());
    CHECK(both->size() == dungeon + CoopChests::Keys().size());
}

TEST_CASE("chests: a chest opened in A is written in B and reported for B's loaded copy, not A's") {
    std::optional<host::SharedFlags> list;
    CoopChests::Merge(list, true);
    const auto chest = host::FlagKey::key64_flag(CoopChests::Keys()[0].key);
    Game a;
    Game b;
    a.set(chest, 0);
    b.set(chest, 0);
    host::FlagWatcher wa(*list, host::FlagRole::player);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    for (host::FlagWatcher* w : {&wa, &wb}) {
        for (const auto& k : CoopChests::Keys()) w->share_pickup(host::FlagKey::key64_flag(k.key));
    }
    wa.frame(a);
    wb.frame(b);

    a.set(chest, 1);  // A opens it
    proto::Message m;
    const auto* set = AsSet(wa.frame(a), m);
    REQUIRE(set);
    REQUIRE(set->flags.size() == 1);

    // The host's answer goes to everyone, A included.
    const proto::FlagState state{1, set->flags};
    wa.receive(proto::Message{state});
    wb.receive(proto::Message{state});
    wa.frame(a);
    wb.frame(b);
    CHECK(b.values[chest].value == 1);
    // B wrote it for another player: its loaded chest goes. A opened it
    // itself: its chest (playing the opening) stays.
    CHECK(wb.take_written_for_others() == std::vector<host::FlagKey>{chest});
    CHECK(wb.take_written_for_others().empty());  // taken once
    CHECK(wa.take_written_for_others().empty());
}

TEST_CASE("chests: a late joiner's snapshot reports the chests it opens, a key the game cannot write is not") {
    std::optional<host::SharedFlags> list;
    CoopChests::Merge(list, true);
    const auto open = host::FlagKey::key64_flag(CoopChests::Keys()[0].key);
    const auto shut = host::FlagKey::key64_flag(CoopChests::Keys()[1].key);
    const auto unreadable = host::FlagKey::key64_flag(CoopChests::Keys()[2].key);
    const auto had = host::FlagKey::key64_flag(CoopChests::Keys()[3].key);
    Game b;
    b.set(open, 0);
    b.set(shut, 0);
    b.set(had, 1);  // opened in this save already
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wb.frame(b);
    proto::FlagSnapshot snap;
    snap.flags = {{open, proto::FlagKind::boolean, 1},
                  {shut, proto::FlagKind::boolean, 0},
                  {unreadable, proto::FlagKind::boolean, 1},
                  {had, proto::FlagKind::boolean, 1}};
    wb.receive(proto::Message{snap});
    wb.frame(b);
    CHECK(b.values[open].value == 1);
    CHECK(wb.take_written_for_others() == std::vector<host::FlagKey>{open});
}

TEST_CASE("chests: a Deku Tree chest opened in A is written in B and reported for B's loaded copy (issue 138)") {
    // The dungeons' chests come from coop_dungeon's list, not this one; the
    // hook shares them the same way (Merge, then share_pickup for each key),
    // so a loaded one in B is removed like an overworld chest.
    constexpr std::uint64_t kSlingshotChest = 0x0b66198d17d6584e;  // InsideTheDekuTree Obj_TBox_A_02
    bool listed = false;
    for (const auto& k : CoopDungeon::Keys()) listed = listed || k.key == kSlingshotChest;
    REQUIRE(listed);
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    CoopChests::Merge(list, true);
    const auto chest = host::FlagKey::key64_flag(kSlingshotChest);
    Game a;
    Game b;
    a.set(chest, 0);
    b.set(chest, 0);
    host::FlagWatcher wa(*list, host::FlagRole::player);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    for (host::FlagWatcher* w : {&wa, &wb}) {
        for (const auto& k : CoopDungeon::Keys()) w->share_pickup(host::FlagKey::key64_flag(k.key));
    }
    wa.frame(a);
    wb.frame(b);

    a.set(chest, 1);  // A opens the Slingshot chest
    proto::Message m;
    const auto* set = AsSet(wa.frame(a), m);
    REQUIRE(set);
    REQUIRE(set->flags.size() == 1);
    const proto::FlagState state{1, set->flags};
    wa.receive(proto::Message{state});
    wb.receive(proto::Message{state});
    wa.frame(a);
    wb.frame(b);
    CHECK(b.values[chest].value == 1);
    CHECK(wb.take_written_for_others() == std::vector<host::FlagKey>{chest});
    CHECK(wa.take_written_for_others().empty());
}
