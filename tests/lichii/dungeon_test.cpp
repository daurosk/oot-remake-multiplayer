// Tests for the shared dungeon flags (lichii/hook/coop_dungeon.cpp).
#include <doctest/doctest.h>

#include <map>
#include <set>
#include <string>

#include "coop_dungeon.h"
#include "coop/host/flag_watcher.hpp"
#include "coop/protocol/codec.hpp"

namespace host = coop::host;

TEST_CASE("dungeon: every flag's hash is the hash of its name, once") {
    std::set<std::uint32_t> seen;
    std::size_t ints = 0;
    for (const auto& f : CoopDungeon::Flags()) {
        CHECK_MESSAGE(host::flag_hash(f.name) == f.hash, f.name);
        CHECK(seen.insert(f.hash).second);
        ints += f.is_int ? 1 : 0;
    }
    CHECK(CoopDungeon::Flags().size() > 700);
    CHECK(ints > 10);
}

TEST_CASE("dungeon: the Water Temple's water level and switches are in the list") {
    bool level = false;
    bool shutter = false;
    for (const auto& f : CoopDungeon::Flags()) {
        level |= std::string(f.name) == "WaterTemple_Room00_WaterLevel" && f.is_int;
        shutter |= std::string(f.name) == "WaterTemple_Room00_Switch_NearTheShutter";
        // One player's own state is not shared.
        CHECK(std::string(f.name).find("NaviHelp") == std::string::npos);
        CHECK(std::string(f.name).find("IsPlayerIn") == std::string::npos);
    }
    CHECK(level);
    CHECK(shutter);
}

TEST_CASE("dungeon: a room gimmick's played event is shared, story scenes and bosses are not") {
    std::set<std::string> names;
    std::size_t played = 0;
    for (const auto& f : CoopDungeon::Flags()) {
        names.insert(f.name);
        const std::string name = f.name;
        if (name.rfind("HavePlayedEvent.", 0) == 0) {
            ++played;
            CHECK_FALSE(f.is_int);
            for (const char* left_out : {"Boss", "Navi", "Talk", "Meet", "Ruto", "SageOf",
                                         "HavePlayedEvent.Common_", "HavePlayedEvent.Dm_",
                                         "HavePlayedEvent.Demo_"}) {
                CHECK_MESSAGE(name.find(left_out) == std::string::npos, name);
            }
        }
    }
    // Issue 129: the Deku Tree slingshot room's ladder, and its room cleared.
    CHECK(names.count("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder") == 1);
    CHECK(names.count("InsideTheDekuTree_RoomC_Cleared") == 1);
    // The other Deku Tree rooms' gimmicks, and some of every kind elsewhere.
    CHECK(names.count("HavePlayedEvent.InsideTheDekuTree_Gimmick_RoomD_PlatformUp") == 1);
    CHECK(names.count("HavePlayedEvent.InsideTheDekuTree_Gimmick_RoomG_WaterLevelDrop") == 1);
    CHECK(names.count("HavePlayedEvent.DodongoCavern_Gimmick_EyeSwitch") == 1);
    CHECK(names.count("HavePlayedEvent.ForestTemple_Room22_ShutterOpen") == 1);
    CHECK(names.count("HavePlayedEvent.ShadowTemple_Room06_SilverToken_Unlock") == 1);
    // One-way rides and camera shots that are no room's state stay out.
    for (const char* out :
         {"HavePlayedEvent.ShadowTemple_Gimmick_Departure", "HavePlayedEvent.ForestTemple_Room11_ElevatorStart",
          "HavePlayedEvent.ForestTemple_Room11_RoomSpin", "HavePlayedEvent.ForestTemple_Room22_RoomSpin",
          "HavePlayedEvent.FocusOnGimmick_Basic", "HavePlayedEvent.FocusOnGimmick_NoSE",
          "HavePlayedEvent.TrapTreasureBoxOpen", "HavePlayedEvent.TreasureBoxAppear_Room04",
          "HavePlayedEvent.InsideGanonCastle_LookBarrierFromDoor_Fire",
          "HavePlayedEvent.GanonCastle_Dungeon_SeeTowerBarrier",
          "HavePlayedEvent.GanonCastle_Dungeon_SeeTowerBarrier_FromDoor"}) {
        CHECK_MESSAGE(names.count(out) == 0, out);
    }
    CHECK(played > 180);
}

TEST_CASE("dungeon: Merge adds to the list, makes one when there is none, or does nothing when off") {
    std::optional<host::SharedFlags> none;
    CHECK(CoopDungeon::Merge(none, false) == 0);
    CHECK_FALSE(none.has_value());

    const std::size_t all =
        CoopDungeon::Flags().size() + CoopDungeon::LiveFlags().size() + CoopDungeon::Keys().size();
    const std::size_t n = CoopDungeon::Merge(none, true);
    REQUIRE(none.has_value());
    CHECK(n == all);
    CHECK(none->contains(host::FlagKey::key64_flag(CoopDungeon::Keys()[0].key)));
    CHECK(none->contains(host::FlagKey(host::flag_hash("WaterTemple_Room00_WaterLevel"), 0)));

    // An existing list keeps its own flags; a flag already there counts once.
    const auto mine = host::FlagKey(0x12345678, 0);
    const auto dup = host::FlagKey(CoopDungeon::Flags()[0].hash, 0);
    std::optional<host::SharedFlags> some = host::SharedFlags({mine, dup});
    CHECK(CoopDungeon::Merge(some, true) == all - 1);
    CHECK(some->contains(mine));
    CHECK(some->size() == all + 1);
}

namespace {

bool HasKey(std::uint64_t key, const char* dungeon, const char* actor) {
    for (const auto& k : CoopDungeon::Keys()) {
        if (k.key == key) {
            return std::string(k.dungeon) == dungeon && std::string(k.actor) == actor;
        }
    }
    return false;
}

} // namespace

TEST_CASE("dungeon: the placement keys are one each, and only things that stay done") {
    std::set<std::uint64_t> seen;
    std::map<std::string, std::size_t> per_dungeon;
    std::size_t chests = 0;
    for (const auto& k : CoopDungeon::Keys()) {
        CHECK(k.key != 0);
        CHECK(k.key != ~std::uint64_t{0});
        CHECK_MESSAGE(seen.insert(k.key).second, k.actor);
        const std::string actor = k.actor;
        // Pots, enemies and grass come back on a scene load: not in the list.
        CHECK_MESSAGE(!actor.starts_with("Obj_Pot_"), actor);
        CHECK_MESSAGE(actor.find("Grass") == std::string::npos, actor);
        CHECK_MESSAGE((!actor.starts_with("Enemy_") || actor == "Enemy_Skulltula_Gold" ||
                       actor == "Enemy_Anubis" || actor.starts_with("Enemy_MiddleBoss")),
                      actor);
        chests += actor.starts_with("Obj_TBox") ? 1 : 0;
        ++per_dungeon[k.dungeon];
    }
    CHECK(CoopDungeon::Keys().size() == 500);
    CHECK(chests == 140);
    CHECK(per_dungeon.size() == 13);
    CHECK(per_dungeon["InsideTheDekuTree"] == 37);
    CHECK(per_dungeon["DodongoCavern"] == 36);
}

TEST_CASE("dungeon: the Deku Tree's and Dodongo's Cavern's chests are in the list") {
    CHECK(HasKey(0x9dc359c40cd5dfe7ull, "InsideTheDekuTree", "Obj_TBox_A_01"));
    CHECK(HasKey(0x0b66198d17d6584eull, "InsideTheDekuTree", "Obj_TBox_A_02"));
    CHECK(HasKey(0x77750b6072f44277ull, "DodongoCavern", "Obj_TBox_A_01"));
    CHECK(HasKey(0x686c7d83d07add16ull, "DodongoCavern", "Obj_TBox_A_06"));
}

namespace {

class Game : public host::FlagStore {
public:
    std::map<host::FlagKey, coop::protocol::FlagEntry> values;
    void set(host::FlagKey key, std::uint32_t v) {
        values[key] = {key, coop::protocol::FlagKind::boolean, v};
    }
    std::optional<coop::protocol::FlagEntry> read(host::FlagKey key) override {
        const auto it = values.find(key);
        if (it == values.end()) return std::nullopt;
        return it->second;
    }
    bool write(const coop::protocol::FlagEntry& e) override {
        if (!values.contains(e.key)) return false;
        values[e.key] = e;
        return true;
    }
};

} // namespace

TEST_CASE("dungeon: a chest opened in one game opens in the other and never closes again") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    REQUIRE(list.has_value());
    const auto chest = host::FlagKey::key64_flag(0x9dc359c40cd5dfe7ull);  // Deku Tree
    const auto level = host::FlagKey(host::flag_hash("WaterTemple_Room00_WaterLevel"), 0);

    Game a;
    Game b;
    for (Game* g : {&a, &b}) {
        g->set(chest, 0);
        g->set(level, 2);
    }
    host::FlagWatcher wa(*list, host::FlagRole::player);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    for (host::FlagWatcher* w : {&wa, &wb}) {  // as the hook does with pickups on
        for (const auto& k : CoopDungeon::Keys()) w->share_pickup(host::FlagKey::key64_flag(k.key));
    }
    wa.frame(a);
    wb.frame(b);

    a.set(chest, 1);
    const auto out = wa.frame(a);
    REQUIRE(out.size() == 1);
    proto::Message m;
    REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
    const auto* set = std::get_if<proto::FlagSet>(&m);
    REQUIRE(set);
    REQUIRE(set->flags.size() == 1);
    CHECK(set->flags[0].key == chest);
    CHECK(set->flags[0].value == 1);

    // The host's answer reaches B: B's chest is open.
    wb.receive(proto::Message{proto::FlagState{1, set->flags}});
    wb.frame(b);
    CHECK(b.values[chest].value == 1);

    // B leaves the dungeon and the game reads 0 for a moment: not sent, and
    // a 0 coming in is not written over it.
    b.set(chest, 0);
    CHECK(wb.frame(b).empty());
    b.set(chest, 1);
    wb.frame(b);
    wb.receive(proto::Message{proto::FlagState{2, {{chest, proto::FlagKind::boolean, 0}}}});
    wb.frame(b);
    CHECK(b.values[chest].value == 1);
}

TEST_CASE("dungeon: the slingshot room's ladder event played in A is set in B (issue 129)") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    REQUIRE(list.has_value());
    const auto ladder =
        host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder"), 0);
    const auto cleared = host::FlagKey(host::flag_hash("InsideTheDekuTree_RoomC_Cleared"), 0);

    Game a;
    Game b;
    for (Game* g : {&a, &b}) {
        g->set(ladder, 0);
        g->set(cleared, 0);
    }
    host::FlagWatcher wa(*list, host::FlagRole::player);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wa.frame(a);
    wb.frame(b);

    // A shoots the eye switch: its game plays the event and clears the room.
    a.set(ladder, 1);
    a.set(cleared, 1);
    const auto out = wa.frame(a);
    REQUIRE(out.size() == 1);
    proto::Message m;
    REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
    const auto* set = std::get_if<proto::FlagSet>(&m);
    REQUIRE(set);
    CHECK(set->flags.size() == 2);

    wb.receive(proto::Message{proto::FlagState{1, set->flags}});
    wb.frame(b);
    CHECK(b.values[ladder].value == 1);
    CHECK(b.values[cleared].value == 1);
}

TEST_CASE("dungeon: the room switch flags that are not saved are shared live too") {
    std::set<std::uint32_t> saved;
    for (const auto& f : CoopDungeon::Flags()) saved.insert(f.hash);
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    REQUIRE(list);
    bool switch_on = false;
    for (const auto& f : CoopDungeon::LiveFlags()) {
        CHECK_MESSAGE(host::flag_hash(f.name) == f.hash, f.name);
        CHECK(saved.count(f.hash) == 0);
        CHECK(list->contains(host::FlagKey(f.hash, std::uint16_t{0})));
        // Never one player's own state.
        CHECK(std::string(f.name).find("OnThe") == std::string::npos);
        CHECK(std::string(f.name).find("Navi") == std::string::npos);
        switch_on |= std::string(f.name) == "ForestTemple_Room17_Switch_On_02";
    }
    CHECK(switch_on);
}

namespace {

// B's hold, as the hook makes it: held while B's Link is in that dungeon.
host::HoldFlagStore::Hold HoldIn(const bool& link, const std::string& scene) {
    return [&link, &scene](host::FlagKey key) {
        if (!link) return false;
        const auto in = CoopDungeon::HeldIn(key.hash);
        return in && (in->empty() ? CoopDungeon::IsDungeonScene(scene) : scene == *in);
    };
}

} // namespace

TEST_CASE("dungeon: the ladder event waits while B is in the Deku Tree, B's own eye still works (issue 129)") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto ladder =
        host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder"), 0);
    const auto cleared = host::FlagKey(host::flag_hash("InsideTheDekuTree_RoomC_Cleared"), 0);
    REQUIRE(CoopDungeon::HeldIn(ladder.hash) == std::optional<std::string_view>("InsideTheDekuTree"));
    CHECK_FALSE(CoopDungeon::HeldIn(cleared.hash).has_value());  // the room's own flag is live

    Game b;
    b.set(ladder, 0);
    b.set(cleared, 0);
    bool link = true;
    std::string scene = "InsideTheDekuTree";
    host::HoldFlagStore store(b, HoldIn(link, scene));
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wb.frame(store);

    // A shot the eye: the room clear is written, the ladder event is held.
    const std::vector<proto::FlagEntry> from_a{{ladder, proto::FlagKind::boolean, 1},
                                              {cleared, proto::FlagKind::boolean, 1}};
    wb.receive(proto::Message{proto::FlagState{1, from_a}});
    wb.frame(store);
    CHECK(b.values[cleared].value == 1);
    CHECK(b.values[ladder].value == 0);  // B's eye switch still plays the event
    wb.frame(store);
    CHECK(b.values[ladder].value == 0);

    // B leaves the Deku Tree (or loads): the event is written, so the room
    // loads with the ladder down.
    scene = "KokiriForest";
    wb.frame(store);
    CHECK(b.values[ladder].value == 1);
}

TEST_CASE("dungeon: a held event B plays itself is not written again, and is sent as B's") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto ladder =
        host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder"), 0);
    Game b;
    b.set(ladder, 0);
    bool link = true;
    std::string scene = "InsideTheDekuTree";
    host::HoldFlagStore store(b, HoldIn(link, scene));
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wb.frame(store);
    wb.receive(proto::Message{proto::FlagState{1, {{ladder, proto::FlagKind::boolean, 1}}}});
    wb.frame(store);
    CHECK(b.values[ladder].value == 0);
    b.set(ladder, 1);  // B shoots its own eye: B's game plays the event
    const auto out = wb.frame(store);
    CHECK(out.size() == 1);  // B's change goes out as usual
    scene = "KokiriForest";
    wb.frame(store);
    CHECK(b.values[ladder].value == 1);
    CHECK(store.held() == 1);  // tried once; dropped once B played it
}

TEST_CASE("dungeon: loading (no Link) writes what was held, the corridor waits in the Forest Temple") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto twist = host::FlagKey(host::flag_hash("ForestTemple_TwistCorridor_IsReverse"), 0);
    REQUIRE(CoopDungeon::HeldIn(twist.hash) == std::optional<std::string_view>("ForestTemple"));
    Game b;
    b.set(twist, 0);
    bool link = true;
    std::string scene = "ForestTemple";
    host::HoldFlagStore store(b, HoldIn(link, scene));
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wb.frame(store);
    wb.receive(proto::Message{proto::FlagState{1, {{twist, proto::FlagKind::boolean, 1}}}});
    wb.frame(store);
    CHECK(b.values[twist].value == 0);  // never turns under B
    link = false;                       // a load: the room is built again
    wb.frame(store);
    CHECK(b.values[twist].value == 1);
}

TEST_CASE("dungeon: the host answers a held write with the asked value") {
    namespace proto = coop::protocol;
    std::optional<host::SharedFlags> list;
    CoopDungeon::Merge(list, true);
    const auto ladder =
        host::FlagKey(host::flag_hash("HavePlayedEvent.InsideTheDekuTree_Gimmick_FallingLadder"), 0);
    Game h;
    h.set(ladder, 0);
    bool link = true;
    std::string scene = "InsideTheDekuTree";
    host::HoldFlagStore store(h, HoldIn(link, scene));
    host::FlagWatcher wh(*list, host::FlagRole::host);
    wh.frame(store);
    wh.receive(proto::Message{proto::FlagSet{2, {{ladder, proto::FlagKind::boolean, 1}}}});
    const auto out = wh.frame(store);
    bool answered = false;
    for (const auto& o : out) {
        proto::Message m;
        REQUIRE(proto::decode(o.bytes, m) == proto::Error::none);
        if (const auto* st = std::get_if<proto::FlagState>(&m)) {
            for (const auto& e : st->flags) answered |= e.key == ladder && e.value == 1;
        }
    }
    CHECK(answered);
    CHECK(h.values[ladder].value == 0);
    scene = "KokiriForest";
    wh.frame(store);
    CHECK(h.values[ladder].value == 1);
}

TEST_CASE("dungeon: which flags are held: gimmick events and the corridor, not switches or keys") {
    std::size_t held = 0;
    for (const auto& f : CoopDungeon::Flags()) {
        const auto in = CoopDungeon::HeldIn(f.hash);
        const std::string name = f.name;
        if (name.starts_with("HavePlayedEvent.")) {
            CHECK_MESSAGE(in.has_value(), name);
            if (in && !in->empty()) CHECK(CoopDungeon::IsDungeonScene(*in));
        }
        if (!name.starts_with("HavePlayedEvent.") &&
            (name.find("SmallKey") != std::string::npos || name.find("WaterLevel") != std::string::npos)) {
            CHECK_FALSE(in.has_value());
        }
        held += in ? 1 : 0;
    }
    CHECK(held > 190);
    CHECK(CoopDungeon::HeldIn(host::flag_hash("HavePlayedEvent.SpritTemple_Room01_Gimmick")) == std::nullopt);
    CHECK(CoopDungeon::IsDungeonScene("InsideJabuJabuBelly"));
    CHECK_FALSE(CoopDungeon::IsDungeonScene("KokiriForest"));
}
