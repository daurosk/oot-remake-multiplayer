// Tests for the shared Threads of Time quests (lichii/hook/coop_quests.cpp, issue 193).
#include <doctest/doctest.h>

#include <map>
#include <set>
#include <string>

#include "coop_quests.h"
#include "coop_story.h"
#include "coop/host/flag_watcher.hpp"
#include "coop/protocol/codec.hpp"

namespace host = coop::host;
namespace proto = coop::protocol;

namespace {

constexpr std::uint32_t kGotoCastle = 0xdb89c1a9u;  // Goto_HyruleCastle (the owl's quest)
constexpr std::uint32_t kFairy = 0x682d1169u;       // Meet_GreatFairy
constexpr std::uint32_t kEpona = 0x92431c55u;       // Get_Epona (its end only)
constexpr std::uint32_t kForest = 0x7d776b61u;      // Get_StoneOfForest (the Deku Tree)
constexpr std::uint32_t kFire = 0x1002b466u;        // Get_StoneOfFire (the Goron Ruby, issue 60)

host::FlagKey Key(std::uint32_t hash) {
    return host::FlagKey(hash, std::uint16_t{0});
}

// A game's saved Enums (quest hash -> step name's hash) and, below them, a
// plain store for every other flag.
struct Game {
    std::map<std::uint32_t, std::uint32_t> enums;
    std::uint64_t enum_writes = 0;

    class Inner : public host::FlagStore {
    public:
        std::map<host::FlagKey, proto::FlagEntry> values;
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
    } inner;

    CoopQuests::Store store{
        inner,
        [this](std::uint32_t hash) -> std::optional<std::uint32_t> {
            const auto it = enums.find(hash);
            if (it == enums.end()) return std::nullopt;
            return it->second;
        },
        [this](std::uint32_t hash, std::uint32_t value) {
            if (!enums.contains(hash)) return false;
            enums[hash] = value;
            ++enum_writes;
            return true;
        }};

    void step(std::uint32_t quest, const char* name) {
        enums[quest] = host::flag_hash(name);
    }
    bool at(std::uint32_t quest, const char* name) const {
        return enums.at(quest) == host::flag_hash(name);
    }
};

const CoopQuests::Quest& Q(std::uint32_t hash) {
    const auto* q = CoopQuests::Find(hash);
    REQUIRE(q != nullptr);
    return *q;
}

std::int32_t RankOf(std::uint32_t quest, const char* step) {
    const auto r = CoopQuests::Rank(Q(quest), host::flag_hash(step));
    REQUIRE(r.has_value());
    return *r;
}

} // namespace

TEST_CASE("quests: the table is sorted by hash, once, each from NotReady to Complete") {
    std::uint32_t last = 0;
    bool first = true;
    for (const auto& q : CoopQuests::Quests()) {
        if (!first) CHECK_MESSAGE(last < q.hash, q.name);
        last = q.hash;
        first = false;
        CHECK(CoopQuests::Find(q.hash) == &q);
        REQUIRE(q.steps.size() >= 3);
        CHECK(std::string(q.steps.front()) == "NotReady");
        CHECK(std::string(q.steps.back()) == "Complete");
        std::set<std::uint32_t> seen;
        for (const char* s : q.steps) CHECK_MESSAGE(seen.insert(host::flag_hash(s)).second, q.name);
    }
    CHECK(CoopQuests::Quests().size() == 13);
    CHECK(CoopQuests::Find(0x12345678u) == nullptr);
}

TEST_CASE("quests: Rank and ValueAt are each other's inverse") {
    const auto& q = Q(kGotoCastle);
    for (std::int32_t i = 0; i < static_cast<std::int32_t>(q.steps.size()); ++i) {
        const auto v = CoopQuests::ValueAt(q, i);
        REQUIRE(v.has_value());
        CHECK(CoopQuests::Rank(q, *v) == i);
    }
    CHECK_FALSE(CoopQuests::ValueAt(q, -1).has_value());
    CHECK_FALSE(CoopQuests::ValueAt(q, static_cast<std::int32_t>(q.steps.size())).has_value());
    CHECK_FALSE(CoopQuests::Rank(q, host::flag_hash("NoSuchStep")).has_value());
}

TEST_CASE("quests: the menu unlock is a shared story flag, the first-open prompt is not") {
    CHECK(CoopStory::Find(host::flag_hash("UI_IsFinishChallengeMenuOpenEvent")) != nullptr);
    CHECK(host::flag_hash("UI_IsFinishChallengeMenuOpenEvent") == 0x1450c74fu);
    CHECK(CoopStory::Find(host::flag_hash("UI_IsOpenChallengeMenuOnce")) == nullptr);
}

TEST_CASE("quests: Merge adds every quest, or none when off") {
    std::optional<host::SharedFlags> list;
    CHECK(CoopQuests::Merge(list, false) == 0);
    CHECK_FALSE(list.has_value());
    CHECK(CoopQuests::Merge(list, true) == CoopQuests::Quests().size());
    REQUIRE(list.has_value());
    CHECK(list->size() == CoopQuests::Quests().size());
    CHECK(CoopQuests::Merge(list, true) == 0);  // repeats dropped
}

TEST_CASE("quests: a quest reads as its step's place, a value not in the list is not read") {
    Game g;
    g.step(kGotoCastle, "Step02");
    const auto e = g.store.read(Key(kGotoCastle));
    REQUIRE(e.has_value());
    CHECK(e->kind == proto::FlagKind::int32);
    CHECK(static_cast<std::int32_t>(e->value) == RankOf(kGotoCastle, "Step02"));
    CHECK(g.store.is_max(Key(kGotoCastle)));
    g.enums[kGotoCastle] = 0xdeadbeefu;
    CHECK_FALSE(g.store.read(Key(kGotoCastle)).has_value());
    CHECK_FALSE(g.store.read(Key(kFairy)).has_value());  // no such Enum in this game
}

TEST_CASE("quests: a write raises, never lowers, write_exact sets, bad writes are refused") {
    Game g;
    g.step(kGotoCastle, "Step03");
    const auto key = Key(kGotoCastle);
    const auto entry = [&](const char* step) {
        return proto::FlagEntry{key, proto::FlagKind::int32,
                                static_cast<std::uint32_t>(RankOf(kGotoCastle, step))};
    };
    CHECK(g.store.write(entry("Step01")));  // lower: ours kept
    CHECK(g.at(kGotoCastle, "Step03"));
    CHECK(g.enum_writes == 0);
    CHECK(g.store.write(entry("Step04")));
    CHECK(g.at(kGotoCastle, "Step04"));
    const auto raised = g.store.take_raised();
    REQUIRE(raised.size() == 1);
    CHECK(std::string(raised[0].from) == "Step03");
    CHECK(std::string(raised[0].to) == "Step04");
    CHECK(g.store.take_raised().empty());
    CHECK(g.store.write_exact(entry("Ready")));  // the host's save load
    CHECK(g.at(kGotoCastle, "Ready"));

    CHECK_FALSE(g.store.write({key, proto::FlagKind::boolean, 1}));
    CHECK_FALSE(g.store.write({key, proto::FlagKind::int32, 99}));
    CHECK_FALSE(g.store.write({key, proto::FlagKind::int32, static_cast<std::uint32_t>(-1)}));
    CHECK(g.store.refused() == 3);
    CHECK(g.at(kGotoCastle, "Ready"));
}

TEST_CASE("quests: a value not in the list, or unread, is not written over (review must-fix 2)") {
    Game g;
    const auto key = Key(kFairy);
    g.enums[kFairy] = 0xdeadbeefu;
    CHECK(g.store.write({key, proto::FlagKind::int32, 0}));  // left alone
    CHECK(g.store.write({key, proto::FlagKind::int32, 3}));
    CHECK(g.enums[kFairy] == 0xdeadbeefu);
    CHECK(g.enum_writes == 0);
    // No such Enum readable: not written blind, the watcher tries again.
    CHECK_FALSE(g.store.write({Key(kGotoCastle), proto::FlagKind::int32, 3}));
    CHECK(g.enum_writes == 0);
    // The host's save load still sets an off-list value.
    CHECK(g.store.write_exact({key, proto::FlagKind::int32, 2}));
    CHECK(g.at(kFairy, "GoToTopOfMountain"));
}

TEST_CASE("quests: Goto_HyruleCastle moves a player on only from Step01 (still in the opening: left alone)") {
    Game g;
    const auto key = Key(kGotoCastle);
    const auto entry = [&](const char* step) {
        return proto::FlagEntry{key, proto::FlagKind::int32,
                                static_cast<std::uint32_t>(RankOf(kGotoCastle, step))};
    };
    CHECK(Q(kGotoCastle).floor == RankOf(kGotoCastle, "Step01"));
    for (const char* below : {"NotReady", "Ready"}) {
        g.step(kGotoCastle, below);
        CHECK(g.store.write(entry("Complete")));
        CHECK(g.at(kGotoCastle, below));
    }
    CHECK(g.enum_writes == 0);
    g.step(kGotoCastle, "Step01");
    CHECK(g.store.write(entry("Complete")));
    CHECK(g.at(kGotoCastle, "Complete"));
    // The host's save load is not held by the floor.
    g.step(kGotoCastle, "NotReady");
    CHECK(g.store.write_exact(entry("Step03")));
    CHECK(g.at(kGotoCastle, "Step03"));
    // Other quests have no floor.
    g.step(kFairy, "NotReady");
    CHECK(g.store.write({Key(kFairy), proto::FlagKind::int32, 3}));
    CHECK(g.at(kFairy, "GoToFountain"));
}

TEST_CASE("quests: other flags pass to the store below") {
    Game g;
    const auto other = host::FlagKey(host::flag_hash("SomeBool"), std::uint16_t{0});
    g.inner.values[other] = {other, proto::FlagKind::boolean, 0};
    CHECK(g.store.write({other, proto::FlagKind::boolean, 1}));
    CHECK(g.store.read(other)->value == 1);
    CHECK_FALSE(g.store.is_max(other));
}

TEST_CASE("quests: A's progress reaches B, B's progress reaches A (issue 193 scenario)") {
    std::optional<host::SharedFlags> list;
    CoopQuests::Merge(list, true);
    Game a;
    Game b;
    for (Game* g : {&a, &b}) {
        for (const auto& q : CoopQuests::Quests()) g->step(q.hash, "NotReady");
        g->step(kGotoCastle, "Step01");
    }
    host::FlagWatcher wa(*list, host::FlagRole::player);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wa.frame(a.store);
    wb.frame(b.store);

    const auto relay = [](host::FlagWatcher& from, Game& from_game, host::FlagWatcher& to, Game& to_game) {
        const auto out = from.frame(from_game.store);
        REQUIRE(out.size() == 1);
        proto::Message m;
        REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
        const auto* set = std::get_if<proto::FlagSet>(&m);
        REQUIRE(set);
        to.receive(proto::Message{proto::FlagState{1, set->flags}});
        to.frame(to_game.store);
    };

    // Both are out of the Deku Tree (Goto Step01, its floor). A meets the owl.
    a.step(kGotoCastle, "Step02");
    relay(wa, a, wb, b);
    CHECK(b.at(kGotoCastle, "Step02"));
    CHECK(wb.frame(b.store).empty());  // no echo back

    // B goes on with another quest; A gets it.
    b.step(kFairy, "GoToFountain");
    relay(wb, b, wa, a);
    CHECK(a.at(kFairy, "GoToFountain"));
    CHECK(a.at(kGotoCastle, "Step02"));
}

TEST_CASE("quests: below Goto's floor a player waits, then catches up once its game sends Step01") {
    std::optional<host::SharedFlags> list;
    CoopQuests::Merge(list, true);
    Game h;  // the host's game: past the owl
    Game b;  // a player still in the opening
    for (Game* g : {&h, &b}) {
        for (const auto& q : CoopQuests::Quests()) g->step(q.hash, "NotReady");
    }
    h.step(kGotoCastle, "Step03");
    b.step(kGotoCastle, "Ready");
    host::FlagWatcher wh(*list, host::FlagRole::host);
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wh.frame(h.store);
    wb.frame(b.store);

    const auto decode_one = [](const std::vector<host::Outgoing>& out) {
        REQUIRE(out.size() == 1);
        proto::Message m;
        REQUIRE(proto::decode(out[0].bytes, m) == proto::Error::none);
        return m;
    };

    // The host's step reaches B below the floor: B's quest stays where it is.
    wb.receive(proto::Message{proto::FlagState{0, {{Key(kGotoCastle), proto::FlagKind::int32,
                                                   static_cast<std::uint32_t>(RankOf(kGotoCastle, "Step03"))}}}});
    wb.frame(b.store);
    CHECK(b.at(kGotoCastle, "Ready"));

    // B leaves the Deku Tree (Step01): its game sends it, the host keeps its
    // higher step and answers it, and B takes it now.
    b.step(kGotoCastle, "Step01");
    const auto sent = decode_one(wb.frame(b.store));
    const auto* set = std::get_if<proto::FlagSet>(&sent);
    REQUIRE(set);
    CHECK(wh.receive(proto::Message{proto::FlagSet{2, set->flags}}));
    const auto answer = decode_one(wh.frame(h.store));
    const auto* state = std::get_if<proto::FlagState>(&answer);
    REQUIRE(state);
    CHECK(h.at(kGotoCastle, "Step03"));  // never lowered on the host
    wb.receive(proto::Message{proto::FlagState{0, state->flags}});
    wb.frame(b.store);
    CHECK(b.at(kGotoCastle, "Step03"));
}

TEST_CASE("quests: a step held in a dungeon is not lost when our game takes a lower step of its own (review should-fix 4)") {
    std::optional<host::SharedFlags> list;
    CoopQuests::Merge(list, true);
    Game b;
    for (const auto& q : CoopQuests::Quests()) b.step(q.hash, "NotReady");
    b.step(kForest, "GoInside");
    bool in_dungeon = true;
    host::HoldFlagStore hold(b.store, [&](host::FlagKey key) {
        return in_dungeon && CoopQuests::Find(key.hash) != nullptr;
    });
    host::FlagWatcher wb(*list, host::FlagRole::player);
    wb.frame(hold);

    // A beat the boss: DefeatBoss comes in while B is inside, so it waits.
    wb.receive(proto::Message{proto::FlagState{0, {{Key(kForest), proto::FlagKind::int32,
                                                   static_cast<std::uint32_t>(RankOf(kForest, "DefeatBoss"))}}}});
    wb.frame(hold);
    CHECK(b.at(kForest, "GoInside"));
    // B's own game moves on, still below A's step.
    b.step(kForest, "MeetBoss");
    wb.frame(hold);
    wb.frame(hold);
    CHECK(b.at(kForest, "MeetBoss"));
    // B leaves the dungeon: the held step is written.
    in_dungeon = false;
    wb.frame(hold);
    CHECK(b.at(kForest, "DefeatBoss"));
}

TEST_CASE("quests: a guest who joins later gets the group's steps") {
    std::optional<host::SharedFlags> list;
    CoopQuests::Merge(list, true);
    Game guest;
    for (const auto& q : CoopQuests::Quests()) guest.step(q.hash, "NotReady");
    guest.step(kGotoCastle, "Step01");
    host::FlagWatcher wg(*list, host::FlagRole::player);
    wg.frame(guest.store);
    proto::FlagSnapshot snap;
    snap.flags = {{Key(kGotoCastle), proto::FlagKind::int32,
                   static_cast<std::uint32_t>(RankOf(kGotoCastle, "Step03"))}};
    wg.receive(proto::Message{snap});
    wg.frame(guest.store);
    CHECK(guest.at(kGotoCastle, "Step03"));
    CHECK(wg.frame(guest.store).empty());
}

TEST_CASE("quests: Get_Epona shares its end only") {
    const auto key = Key(kEpona);
    const auto entry = [&](const char* step) {
        return proto::FlagEntry{key, proto::FlagKind::int32,
                                static_cast<std::uint32_t>(RankOf(kEpona, step))};
    };
    REQUIRE(Q(kEpona).complete_only);

    // A game on its way reads as NotReady; one that has won her reads as Complete.
    Game g;
    g.step(kEpona, "AfterTalkMalon");
    auto e = g.store.read(key);
    REQUIRE(e.has_value());
    CHECK(e->value == 0u);
    CHECK(g.store.is_max(key));

    // Another player's steps never move ours, neither does the host's save load.
    CHECK(g.store.write(entry("ReadyForGambleRace")));
    CHECK(g.store.write(entry("AfterWonGambleRace")));
    CHECK(g.store.write_exact(entry("NotReady")));
    CHECK(g.at(kEpona, "AfterTalkMalon"));
    CHECK(g.enum_writes == 0);

    // Another player won her: ours is Complete too, from any step before the race.
    CHECK(g.store.write(entry("Complete")));
    CHECK(g.at(kEpona, "Complete"));
    const auto raised = g.store.take_raised();
    REQUIRE(raised.size() == 1);
    CHECK(std::string(raised[0].quest) == "Get_Epona");
    CHECK(std::string(raised[0].to) == "Complete");
    e = g.store.read(key);
    REQUIRE(e.has_value());
    CHECK(static_cast<std::int32_t>(e->value) == RankOf(kEpona, "Complete"));
    CHECK(g.store.write_exact(entry("NotReady")));  // a host without her: ours stays
    CHECK(g.at(kEpona, "Complete"));

    Game fresh;  // a child who never went to the ranch
    fresh.step(kEpona, "NotReady");
    CHECK(fresh.store.write_exact(entry("Complete")));
    CHECK(fresh.at(kEpona, "Complete"));

    // A player in the race, or riding out after it, finishes it themselves (no race flag read: held).
    for (const char* step : {"GambleRacePlaying", "AfterWonGambleRace"}) {
        Game racing;
        racing.step(kEpona, step);
        CHECK_FALSE(racing.store.write(entry("Complete")));  // held, tried again
        CHECK(racing.store.busy(key));
        CHECK(racing.at(kEpona, step));
        CHECK(racing.enum_writes == 0);
    }

    // Lost the race (back to ReadyForGambleRace): the held end is written then.
    Game lost;
    lost.step(kEpona, "GambleRacePlaying");
    CHECK_FALSE(lost.store.write(entry("Complete")));
    lost.step(kEpona, "ReadyForGambleRace");
    CHECK_FALSE(lost.store.busy(key));
    CHECK(lost.store.write(entry("Complete")));
    CHECK(lost.at(kEpona, "Complete"));

    // Unread (GameData not up) or a value not in the list: not written.
    Game none;
    CHECK_FALSE(none.store.write_exact(entry("Complete")));
    Game odd;
    odd.enums[kEpona] = 0xdeadbeefu;
    CHECK(odd.store.write_exact(entry("Complete")));
    CHECK(odd.enums[kEpona] == 0xdeadbeefu);
}

TEST_CASE("quests: Get_StoneOfFire is shared, in the game's order, from Ready (issue 60)") {
    CHECK(host::flag_hash("Step_Get_StoneOfFire") == kFire);
    const auto& q = Q(kFire);
    CHECK(q.floor == RankOf(kFire, "Ready"));
    // The RSDB row's steps in order (Darunia sends Link to Saria, then the cavern).
    CHECK(RankOf(kFire, "KakarikoVillageNpc") == 2);
    CHECK(RankOf(kFire, "GoronCityDarniaTalk") < RankOf(kFire, "KokiriForestFire"));
    CHECK(RankOf(kFire, "KokiriForestFire") < RankOf(kFire, "KokiriForestIn"));
    CHECK(RankOf(kFire, "SariaMelodyPlay") < RankOf(kFire, "DodongoCavernOpne"));
    CHECK(RankOf(kFire, "EscapeDungeon") == RankOf(kFire, "Complete") - 1);
    const proto::FlagEntry done{Key(kFire), proto::FlagKind::int32,
                                static_cast<std::uint32_t>(RankOf(kFire, "Complete"))};
    // B was in Kakariko while A got the Goron Ruby: B's quest is Complete too,
    // so B's Get_StoneOfFire_IsCompleted_Exp (the Door of Time's check) holds.
    Game b;
    b.step(kFire, "KakarikoVillageLetter");
    CHECK(b.store.write(done));
    CHECK(b.at(kFire, "Complete"));
    // From Ready on (the manager's own step), not from a new save's NotReady.
    Game c;
    c.step(kFire, "NotReady");
    CHECK(c.store.write(done));
    CHECK(c.at(kFire, "NotReady"));
    c.step(kFire, "Ready");
    CHECK(c.store.write(done));
    CHECK(c.at(kFire, "Complete"));
}

// Issue 94: every shared quest's GameData step also goes to the game's own
// ChallengeMgr (the Threads of Time tab and the game's logic read it), not only
// after a reload.
namespace {

std::map<std::uint32_t, std::uint32_t> AllAt(const char* step_of_each) {
    std::map<std::uint32_t, std::uint32_t> at;
    for (const auto& q : CoopQuests::Quests()) at[q.hash] = host::flag_hash(step_of_each);
    return at;
}

CoopQuest::ReadEnum Reader(const std::map<std::uint32_t, std::uint32_t>& at) {
    return [&at](std::uint32_t h) -> std::optional<std::uint32_t> {
        const auto it = at.find(h);
        if (it == at.end()) return std::nullopt;
        return it->second;
    };
}

const CoopQuest::StepWant* WantOf(const std::vector<CoopQuest::StepWant>& wants, const char* name) {
    for (const auto& w : wants) {
        if (w.name == name) return &w;
    }
    return nullptr;
}

} // namespace

TEST_CASE("quests live: each challenge is flag_hash of the quest's name (the manager's own hashes)") {
    // The hashes the manager entries carry (read from the game, coop_quest.cpp).
    CHECK(host::flag_hash("Get_StoneOfFire") == 0xe4c584e6u);
    CHECK(host::flag_hash("DoorOfTime") == 0xe8dafe6cu);
    CHECK(host::flag_hash("Get_StoneOfForest") == 0x628de7a6u);
    CHECK(host::flag_hash("Get_StoneOfWater") == 0x0aa38424u);
    CHECK(host::flag_hash("SageOfFire") == 0x533999f4u);
    CHECK(host::flag_hash("SageOfForest") == 0x5a2be4b5u);
    CHECK(host::flag_hash("Get_Epona") == 0xe873ce22u);
}

TEST_CASE("quests live: a step past Ready goes to the manager, with the quest's steps in order") {
    auto at = AllAt("Complete");
    const auto wants = CoopQuests::LiveWants(Reader(at));
    const auto* forest = WantOf(wants, "Get_StoneOfForest");
    REQUIRE(forest != nullptr);
    CHECK(forest->challenge == 0x628de7a6u);
    CHECK(forest->step == host::flag_hash("Complete"));
    REQUIRE(forest->steps.size() == 8);
    CHECK(forest->steps.front() == host::flag_hash("NotReady"));
    CHECK(forest->steps.back() == host::flag_hash("Complete"));
    CHECK(WantOf(wants, "SageOfFire") != nullptr);
    CHECK(WantOf(wants, "Goto_HyruleCastle") != nullptr);
    // Get_StoneOfFire is CoopQuest's own (live since issue 171), and so is Get_Epona (row 150: from
    // AfterGetEponaSong, its end included): not twice.
    CHECK(WantOf(wants, "Get_StoneOfFire") == nullptr);
    CHECK(WantOf(wants, "Get_Epona") == nullptr);
    for (const auto& w : wants) {
        CHECK(w.challenge == host::flag_hash(w.name.c_str()));
    }
}

TEST_CASE("quests live: Ready, NotReady, an unknown or unread step and a step under the floor are not sent") {
    auto at = AllAt("Ready");
    CHECK(CoopQuests::LiveWants(Reader(at)).empty());
    at = AllAt("NotReady");
    CHECK(CoopQuests::LiveWants(Reader(at)).empty());
    at = AllAt("NoSuchStep");
    CHECK(CoopQuests::LiveWants(Reader(at)).empty());
    std::map<std::uint32_t, std::uint32_t> none;
    CHECK(CoopQuests::LiveWants(Reader(none)).empty());
    // Goto_HyruleCastle's floor is Step01 (index 2): Step01 itself is sent.
    at.clear();
    at[kGotoCastle] = host::flag_hash("Step01");
    CHECK(WantOf(CoopQuests::LiveWants(Reader(at)), "Goto_HyruleCastle") != nullptr);
}

TEST_CASE("quests live: Get_Epona reaches the manager once, through CoopQuest (row 150)") {
    // Its end (shared here) and the song's step (CoopQuest's rule) both go through CoopQuest::LiveWant, from
    // AfterGetEponaSong on; CoopQuests::LiveWants leaves the quest to it, so nothing is asked twice.
    const CoopQuest::Quest* epona = nullptr;
    for (const auto& q : CoopQuest::Quests()) {
        if (q.hash == kEpona) epona = &q;
    }
    REQUIRE(epona != nullptr);
    std::map<std::uint32_t, std::uint32_t> at;
    for (const char* step : {"ReadyForGetEponaSong", "AfterGetEponaSong", "Complete"}) {
        at[kEpona] = host::flag_hash(step);
        CHECK(WantOf(CoopQuests::LiveWants(Reader(at)), "Get_Epona") == nullptr);
    }
    at[kEpona] = host::flag_hash("ReadyForGetEponaSong");
    CHECK_FALSE(CoopQuest::LiveWant(*epona, Reader(at)).has_value());
    at[kEpona] = host::flag_hash("Complete");
    const auto w = CoopQuest::LiveWant(*epona, Reader(at));
    REQUIRE(w.has_value());
    CHECK(w->challenge == host::flag_hash("Get_Epona"));
    CHECK(w->step == host::flag_hash("Complete"));
}

TEST_CASE("quests live: a mid step goes as it is (Meet_GreatFairy GoToFountain)") {
    std::map<std::uint32_t, std::uint32_t> at;
    at[kFairy] = host::flag_hash("GoToFountain");
    const auto wants = CoopQuests::LiveWants(Reader(at));
    REQUIRE(wants.size() == 1);
    CHECK(wants[0].name == "Meet_GreatFairy");
    CHECK(wants[0].step == host::flag_hash("GoToFountain"));
}

namespace {

constexpr std::uint32_t kRaceRunning = 0xa253721cu;  // LonLonRanch_PlayingGambleRace

// Game, with the game's race flag (LonLonRanch_PlayingGambleRace) read through the store, as the hook does.
struct RanchGame : Game {
    std::map<std::uint32_t, bool> bools;
    CoopQuests::Store ranch{
        inner,
        [this](std::uint32_t hash) -> std::optional<std::uint32_t> {
            const auto it = enums.find(hash);
            if (it == enums.end()) return std::nullopt;
            return it->second;
        },
        [this](std::uint32_t hash, std::uint32_t value) {
            if (!enums.contains(hash)) return false;
            enums[hash] = value;
            ++enum_writes;
            return true;
        },
        [this](std::uint32_t hash) -> std::optional<bool> {
            const auto it = bools.find(hash);
            if (it == bools.end()) return std::nullopt;
            return it->second;
        }};
};

} // namespace

TEST_CASE("quests: row 150: Get_Epona's end waits only while a race against Ingo runs in our game") {
    CHECK(host::flag_hash("LonLonRanch_PlayingGambleRace") == kRaceRunning);
    const auto key = Key(kEpona);
    const proto::FlagEntry complete{key, proto::FlagKind::int32,
                                    static_cast<std::uint32_t>(RankOf(kEpona, "Complete"))};
    for (const char* step : {"GambleRacePlaying", "AfterWonGambleRace"}) {
        // The race runs (the game's own flow set the flag at its start): held, tried again.
        RanchGame racing;
        racing.step(kEpona, step);
        racing.bools[kRaceRunning] = true;
        CHECK(racing.ranch.busy(key));
        CHECK_FALSE(racing.ranch.write(complete));
        CHECK(racing.at(kEpona, step));
        // The flag unread (GameData not up yet): held as well.
        RanchGame unread;
        unread.step(kEpona, step);
        CHECK(unread.ranch.busy(key));
        CHECK_FALSE(unread.ranch.write(complete));
        // The player report's save: Dm_LonLon_0040 (another player's win) played here left the quest at
        // AfterWonGambleRace with no race and no horse. No race runs: the group's end is taken.
        RanchGame stuck;
        stuck.step(kEpona, step);
        stuck.bools[kRaceRunning] = false;
        CHECK_FALSE(stuck.ranch.busy(key));
        CHECK(stuck.ranch.write(complete));
        CHECK(stuck.at(kEpona, "Complete"));
        // The host's save load (Resync from host) gives the same.
        RanchGame resynced;
        resynced.step(kEpona, step);
        resynced.bools[kRaceRunning] = false;
        CHECK(resynced.ranch.write_exact(complete));
        CHECK(resynced.at(kEpona, "Complete"));
    }
    // Earlier steps never depended on the race flag.
    RanchGame before;
    before.step(kEpona, "AfterTalkMalon");
    before.bools[kRaceRunning] = true;
    CHECK_FALSE(before.ranch.busy(key));
    CHECK(before.ranch.write(complete));
    CHECK(before.at(kEpona, "Complete"));
}
