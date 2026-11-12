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
    CHECK(CoopQuests::Quests().size() == 11);
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
