// Tests for our downed Link's body: crawl, shield, unseen, heal (lichii/hook/coop_dbno_body.cpp).
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "coop_dbno_body.h"

using namespace CoopDbno::Body;
namespace bn = CoopBossNative;

namespace {

class Guest : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            o[i] = it == bytes.end() ? 0 : it->second;
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* b = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) {
            bytes[address + i] = b[i];
        }
        return true;
    }
    std::map<std::uint64_t, std::uint8_t> bytes;
};

constexpr std::uint64_t kBsa = 0x1000;
constexpr std::uint64_t kLife = 0x2000;
constexpr std::uint64_t kEmitter = 0x4000;
constexpr std::uint64_t kNode = 0x3000;
constexpr std::uint64_t kMain = 0x80000000;
constexpr std::uint64_t kBlock = 0x9000;

bn::Request Req(std::int32_t v, std::int32_t slot = 0, std::uint64_t node = kNode) {
    coop::protocol::BossAction a;
    a.verb = v;
    a.slot = slot;
    a.flags = 3;
    return bn::RequestFromAction(a, node);
}

Crawl::Wanted Want(bool lying, bool event = false) {
    Crawl::Wanted w;
    w.lying = lying;
    w.event = event;
    w.bsa = kBsa;
    w.life = kLife;
    w.emitter = kEmitter;
    return w;
}

// The game's key lists as its functions keep them: (component, key).
struct GameLists {
    std::set<std::pair<std::uint64_t, std::uint32_t>> no_reaction;  // Life+0x1c60 (main+0x8d75e0)
    std::set<std::pair<std::uint64_t, std::uint32_t>> other;        // Life+0x1c28 (main+0xb34800)
    std::set<std::pair<std::uint64_t, std::uint32_t>> emitter_off;  // emitter+0x20 (main+0x1278e0)
    bool Holds(std::uint64_t at, std::uint32_t key) const {
        const auto k = std::make_pair(at, key);
        return no_reaction.count(k) + other.count(k) + emitter_off.count(k) != 0;
    }
};
GameLists g_lists;

struct Made {
    std::uint64_t function = 0;  // main+
    std::uint64_t x0 = 0, x1 = 0, x2 = 0;
    std::int32_t verb = 0;  // an action request's verb
};

// One frame of the caller: action requests answered `result`, the malloc
// kBlock, the rest 0. Returns every call made.
std::vector<Made> RunFrame(Crawl& c, Guest& g, std::uint64_t now_ms, std::uint64_t result = 0) {
    std::vector<Made> made;
    auto call = c.Next(g, false, 0, now_ms, kMain);
    while (call) {
        Made m{call->function - kMain, call->x[0], call->x[1], call->x[2], 0};
        std::uint64_t r = 0;
        if (m.function == bn::addr::kMalloc) {
            CHECK(call->x[0] == addr::kBlockSize);
            r = kBlock;
        } else if (m.function == bn::addr::kRequestActionVerb) {
            CHECK_FALSE(call->copy_game);  // our own downed Link: what it makes is ours (#189 is copies only)
            CHECK(m.x0 == kBsa);
            CHECK(m.x1 == kBlock);
            g.Read(kBlock + bn::addr::kReqVerb, &m.verb, sizeof m.verb);
            r = result;
        } else {
            std::uint32_t key = 0;
            g.Read(kBlock + addr::kBlockKey, &key, sizeof key);
            CHECK(key == addr::kOurKey);
            // What the game's function does with it.
            if (m.function == addr::kInvincibleNoReaction || m.function == addr::kInvincibleList) {
                auto& list = m.function == addr::kInvincibleNoReaction ? g_lists.no_reaction : g_lists.other;
                if ((m.x1 & 1) != 0) {
                    list.insert({m.x0, key});  // Bool 1: added
                } else {
                    list.erase({m.x0, key});   // Bool 0: taken out
                }
            } else if (m.function == addr::kEmitterSwitch) {
                if ((m.x2 & 1) == 0) {
                    g_lists.emitter_off.insert({m.x0, key});  // IsEnable 0: our key turns it off
                } else {
                    g_lists.emitter_off.erase({m.x0, key});
                }
            }
        }
        made.push_back(m);
        call = c.Next(g, true, r, now_ms, kMain);
    }
    return made;
}

std::vector<std::int32_t> Verbs(const std::vector<Made>& made) {
    std::vector<std::int32_t> out;
    for (const Made& m : made) {
        if (m.function == bn::addr::kRequestActionVerb) {
            out.push_back(m.verb);
        }
    }
    return out;
}

int Count(const std::vector<Made>& made, std::uint64_t function) {
    int n = 0;
    for (const Made& m : made) {
        n += m.function == function ? 1 : 0;
    }
    return n;
}

// A Crawl that has learnt our Link's node and is crawling at 1000 ms.
void Crawling(Crawl& c, Guest& g) {
    c.Want(Want(false));
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    c.Want(Want(true));
    const auto v = Verbs(RunFrame(c, g, 1000));
    REQUIRE(v.size() == 1);
    REQUIRE(v[0] == verb::kCrawl);
}

}  // namespace

TEST_CASE("dbno body: a raise goes into the Life step as a negative sum") {
    CHECK(Raised(1, 0, 12) == -11);    // 1 -> 12: the step heals 11
    CHECK(Raised(1, 2, 12) == -11);    // a hit the same frame does not keep it lower
    CHECK(Raised(12, 0, 12) == 0);     // reached
    CHECK(Raised(16, 0, 12) == 0);     // above: nothing taken off
    CHECK(Raised(16, 2, 12) == 2);     // a hit that stays above the raise is the game's
    CHECK(Raised(5, 3, -1) == 3);      // no raise asked
}

TEST_CASE("dbno body: Raising waits for the step, then is written once") {
    Raising r;
    CHECK(r.Peek() == -1);
    r.Start(12, 1'000'000);
    CHECK(r.Peek() == 12);
    CHECK_FALSE(r.Frame(1, 1'100'000));
    const auto late = r.Frame(1, 1'000'000 + kHealWaitUs);
    REQUIRE(late);
    CHECK(*late == 12);
    CHECK(r.Peek() == -1);
    CHECK_FALSE(r.Frame(1, 2'000'000));  // once

    r.Start(12, 3'000'000);
    CHECK_FALSE(r.Frame(12, 3'100'000));  // the step took it
    CHECK(r.Peek() == -1);
    CHECK_FALSE(r.Frame(1, 4'000'000));
}

TEST_CASE("dbno body: the verbs the world forces pass") {
    for (const std::int32_t v : {verb::kFall, verb::kLand, verb::kSwim, verb::kWaterDive, verb::kSlide,
                                 verb::kSlippingDown, verb::kSlideLand, verb::kPreAbyss, verb::kPreEvent,
                                 verb::kPreDie}) {
        CHECK(Passes(v));
    }
    CHECK_FALSE(Passes(134));  // a small hit's reaction
    CHECK_FALSE(Passes(verb::kWalk));
    CHECK_FALSE(Passes(verb::kCrawl));
}

TEST_CASE("dbno body: only our crawling Link's own non-crawl actions are skipped") {
    Guest g;
    Crawl c;
    c.Want(Want(false));
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWalk), false));  // up: the game's
    Crawling(c, g);
    CHECK(c.Skip(kBsa, Req(134), false));                  // a hit's reaction (row 34)
    CHECK(c.Skip(kBsa, Req(verb::kWalk), false));
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kCrawl), false));   // the crawl itself
    CHECK_FALSE(c.Skip(kBsa, Req(134), true));             // our own call
    CHECK_FALSE(c.Skip(0x5000, Req(134), false));          // another actor (an enemy)
    CHECK_FALSE(c.Skip(0, Req(134), false));
    c.Want(Want(true, true));
    CHECK_FALSE(c.Skip(kBsa, Req(134), false));            // our event drives our Link
    const auto t = c.Totals();
    CHECK(t.skipped == 2);
    CHECK(t.skipped_verbs.size() == 2);
}

TEST_CASE("dbno body: a refused crawl skips nothing (a ladder, water, mid-air)") {
    Guest g;
    Crawl c;
    c.Want(Want(false));
    c.Skip(kBsa, Req(verb::kWait), false);
    c.Want(Want(true));
    CHECK(Verbs(RunFrame(c, g, 1000, 2)) == std::vector<std::int32_t>{verb::kCrawl});  // 2: refused
    CHECK_FALSE(c.Skip(kBsa, Req(71), false));  // climbing a ladder: the game's own
    CHECK_FALSE(c.Skip(kBsa, Req(134), false));
    CHECK(c.Totals().skipped == 0);
}

TEST_CASE("dbno body: a fall or water while crawling passes, and the crawl is asked again on the ground") {
    Guest g;
    Crawl c;
    Crawling(c, g);
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kFall), false));  // off a ledge
    CHECK(c.Totals().passed == 1);
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kLand), false));  // no longer crawling: the game's
    CHECK_FALSE(c.Skip(kBsa, Req(134), false));
    // Asked again (refused while in the air: spaced, at most kMaxAsks).
    int asks = 0;
    for (std::uint64_t ms = 1100; ms < 9000; ms += 100) {
        asks += static_cast<int>(Verbs(RunFrame(c, g, ms, 2)).size());
    }
    CHECK(asks == Crawl::kMaxAsks);
    // Back on the ground (the game's own Wait): asked at once, taken, skipping again.
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait), false));
    CHECK(Verbs(RunFrame(c, g, 9000)) == std::vector<std::int32_t>{verb::kCrawl});
    CHECK(c.Skip(kBsa, Req(134), false));

    // Swimming: passes the same way.
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kSwim), false));
    CHECK(c.Totals().passed == 2);
}

TEST_CASE("dbno body: nothing is asked before our Link's own node is known, nor when up") {
    Guest g;
    Crawl c;
    c.Want(Want(true));
    CHECK(Verbs(RunFrame(c, g, 1000)).empty());  // no node learnt yet
    c.Want(Want(false));
    c.Skip(kBsa, Req(verb::kWait, 2), false);   // learnt (up: not skipped)
    RunFrame(c, g, 1100);
    CHECK(RunFrame(c, g, 1200).empty());        // up, everything off again: nothing
}

TEST_CASE("dbno body: down -> Crawl once with the scheduler's node, up -> Wait once") {
    Guest g;
    Crawl c;
    c.Want(Want(false));
    c.Skip(kBsa, Req(77, 5, 0x7777), false);   // not Walk/Wait: not learnt from
    c.Skip(kBsa, Req(verb::kWalk, 2), false);  // the scheduler's main slot
    c.Want(Want(true));
    CHECK(Verbs(RunFrame(c, g, 1000)) == std::vector<std::int32_t>{verb::kCrawl});
    std::uint64_t node = 0;
    g.Read(kBlock + bn::addr::kReqNode, &node, sizeof node);
    CHECK(node == kNode);
    std::int32_t slot = -1;
    g.Read(kBlock + bn::addr::kReqSlot, &slot, sizeof slot);
    CHECK(slot == 2);
    CHECK(Verbs(RunFrame(c, g, 1100)).empty());  // taken: not asked again
    CHECK(Verbs(RunFrame(c, g, 5000)).empty());

    c.Want(Want(false));
    CHECK(Verbs(RunFrame(c, g, 6000)) == std::vector<std::int32_t>{verb::kWait});
    CHECK(Verbs(RunFrame(c, g, 6100)).empty());
    CHECK(c.Totals().asked == 2);
}

TEST_CASE("dbno body: an event of ours pauses the crawl, it is asked again after") {
    Guest g;
    Crawl c;
    Crawling(c, g);
    c.Want(Want(true, true));
    CHECK(Verbs(RunFrame(c, g, 2000)).empty());
    c.Want(Want(true));
    CHECK(Verbs(RunFrame(c, g, 3000)) == std::vector<std::int32_t>{verb::kCrawl});
}

TEST_CASE("dbno body: shield and unseen through the game's own functions, on while down, off when up") {
    Guest g;
    Crawl c;
    g_lists = {};
    c.Want(Want(true));
    const auto down = RunFrame(c, g, 1000);
    REQUIRE(Count(down, addr::kInvincibleNoReaction) == 1);
    REQUIRE(Count(down, addr::kEmitterSwitch) == 1);
    for (const Made& m : down) {
        if (m.function == addr::kInvincibleNoReaction) {
            CHECK(m.x0 == kLife);
            CHECK(m.x1 == 1);  // Bool: add our key to the no-reaction list
            CHECK(m.x2 == kBlock + addr::kBlockKey);
        }
        if (m.function == addr::kEmitterSwitch) {
            CHECK(m.x0 == kEmitter + addr::kEmitterKeys);
            CHECK(m.x1 == kBlock + addr::kBlockKey);
            CHECK(m.x2 == 0);  // IsEnable 0: off
        }
    }
    CHECK(g_lists.no_reaction.count({kLife, addr::kOurKey}) == 1);
    CHECK(g_lists.emitter_off.count({kEmitter + addr::kEmitterKeys, addr::kOurKey}) == 1);
    CHECK(g_lists.other.empty());
    CHECK(RunFrame(c, g, 1100).empty());  // once
    CHECK(c.Totals().shield_on == 1);
    CHECK(c.Totals().unseen_on == 1);

    c.Want(Want(false));
    const auto up = RunFrame(c, g, 2000);
    CHECK(Count(up, addr::kInvincibleNoReaction) == 1);
    CHECK(Count(up, addr::kInvincibleList) == 0);  // the other list is never touched
    REQUIRE(Count(up, addr::kEmitterSwitch) == 1);
    for (const Made& m : up) {
        if (m.function == addr::kInvincibleNoReaction) {
            CHECK(m.x0 == kLife);
            CHECK(m.x1 == 0);  // Bool 0: our key taken out
        }
        if (m.function == addr::kEmitterSwitch) {
            CHECK(m.x2 == 1);  // IsEnable: on again
        }
    }
    // On -> off: our key is gone from every list the game keeps.
    CHECK_FALSE(g_lists.Holds(kLife, addr::kOurKey));
    CHECK_FALSE(g_lists.Holds(kEmitter + addr::kEmitterKeys, addr::kOurKey));
    CHECK(g_lists.no_reaction.empty());
    CHECK(g_lists.other.empty());
    CHECK(g_lists.emitter_off.empty());
    CHECK(RunFrame(c, g, 2100).empty());
}

TEST_CASE("dbno body: a new Link (scene change) gets them again, the old component is left alone") {
    Guest g;
    Crawl c;
    c.Want(Want(true));
    RunFrame(c, g, 1000);
    Crawl::Wanted w = Want(true);
    w.life = 0x2200;
    w.emitter = 0x4400;
    c.Want(w);
    const auto again = RunFrame(c, g, 1100);
    CHECK(Count(again, addr::kInvincibleNoReaction) == 1);
    CHECK(Count(again, addr::kEmitterSwitch) == 1);
    // Up while on a Link that has gone (the components changed again): no call on a freed component.
    Crawl::Wanted gone = Want(false);
    gone.life = 0x2300;
    gone.emitter = 0x4500;
    c.Want(gone);
    const auto up = RunFrame(c, g, 1200);
    CHECK(Count(up, addr::kInvincibleNoReaction) == 0);
    CHECK(Count(up, addr::kEmitterSwitch) == 0);
}

TEST_CASE("dbno body: down and up several times leaves none of our keys behind") {
    Guest g;
    Crawl c;
    g_lists = {};
    std::uint64_t ms = 1000;
    for (int i = 0; i < 3; ++i) {
        c.Want(Want(true));
        RunFrame(c, g, ms += 100);
        CHECK(g_lists.no_reaction.size() == 1);
        CHECK(g_lists.emitter_off.size() == 1);
        c.Want(Want(false));
        RunFrame(c, g, ms += 100);
        CHECK(g_lists.no_reaction.empty());
        CHECK(g_lists.emitter_off.empty());
    }
    CHECK(g_lists.other.empty());
}

TEST_CASE("dbno body: never down, the game is never called") {
    Guest g;
    Crawl c;
    c.Want(Want(false));
    c.Skip(kBsa, Req(verb::kWait), false);
    for (std::uint64_t ms = 1000; ms < 3000; ms += 100) {
        CHECK(RunFrame(c, g, ms).empty());
    }
}

// Issue 150 with row 113: a downed teammate's puppet in our game is unseen too.
namespace {
constexpr std::uint64_t kPuppet = 0x60000;   // player 2's puppet actor
constexpr std::uint64_t kTable = 0x61000;    // its component table
constexpr std::uint64_t kPuppetEmitter = 0x62000;

void GivePuppet(Guest& g, std::uint64_t actor, std::uint64_t table, std::uint64_t emitter) {
    g.Write(actor + bn::addr::kActorComponents, &table, sizeof table);
    g.Write(table + addr::kComponentEmitter, &emitter, sizeof emitter);
}

Crawl::Wanted Teammates(std::vector<std::uint8_t> downed, const char* why = "downed") {
    Crawl::Wanted w = Want(false);
    for (const std::uint8_t n : downed) {
        w.unseen_puppets.push_back({n, why});
    }
    return w;
}
}  // namespace

TEST_CASE("dbno body: a downed teammate's puppet: our key in its emitter while down, out when up") {
    Guest g;
    Crawl c;
    g_lists = {};
    GivePuppet(g, kPuppet, kTable, kPuppetEmitter);
    c.SetPuppetActor([](std::uint8_t n) { return n == 2 ? kPuppet : 0; });
    c.Want(Teammates({2}));
    const auto down = RunFrame(c, g, 1000);
    REQUIRE(Count(down, addr::kEmitterSwitch) == 1);
    CHECK(Count(down, addr::kInvincibleNoReaction) == 0);  // our own Link is up: no shield, no own unseen
    for (const Made& m : down) {
        if (m.function == addr::kEmitterSwitch) {
            CHECK(m.x0 == kPuppetEmitter + addr::kEmitterKeys);
            CHECK(m.x2 == 0);  // IsEnable 0: off
        }
    }
    CHECK(g_lists.emitter_off.count({kPuppetEmitter + addr::kEmitterKeys, addr::kOurKey}) == 1);
    CHECK(RunFrame(c, g, 1100).empty());  // once
    CHECK(c.Totals().puppets_unseen_on == 1);
    CHECK(c.Totals().unseen_on == 0);

    c.Want(Teammates({}));
    const auto up = RunFrame(c, g, 2000);
    REQUIRE(Count(up, addr::kEmitterSwitch) == 1);
    for (const Made& m : up) {
        CHECK(m.x2 == 1);  // on again
    }
    CHECK(g_lists.emitter_off.empty());
    CHECK(RunFrame(c, g, 2100).empty());
}

TEST_CASE("dbno body: a remade puppet gets the key again, a gone one is never called") {
    Guest g;
    Crawl c;
    g_lists = {};
    std::uint64_t actor = kPuppet;
    GivePuppet(g, kPuppet, kTable, kPuppetEmitter);
    c.SetPuppetActor([&actor](std::uint8_t n) { return n == 3 ? actor : 0; });
    c.Want(Teammates({3}));
    RunFrame(c, g, 1000);
    // Remade (a scene change): a new actor with a new emitter.
    actor = 0x70000;
    GivePuppet(g, 0x70000, 0x71000, 0x72000);
    const auto again = RunFrame(c, g, 1100);
    REQUIRE(Count(again, addr::kEmitterSwitch) == 1);
    CHECK(again[0].x0 == 0x72000 + addr::kEmitterKeys);
    // Up while the puppet is gone (they left the scene): nothing is called on a freed component.
    actor = 0;
    c.Want(Teammates({}));
    CHECK(Count(RunFrame(c, g, 1200), addr::kEmitterSwitch) == 0);
    CHECK(RunFrame(c, g, 1300).empty());
}

TEST_CASE("dbno body: a puppet without perception (no emitter) or no puppet: the game is never asked") {
    Guest g;
    Crawl c;
    GivePuppet(g, kPuppet, kTable, 0);  // a pack without perception: no emitter component
    c.SetPuppetActor([](std::uint8_t n) { return n == 2 ? kPuppet : 0; });
    c.Want(Teammates({2, 4}));  // 4: no puppet in our scene
    for (std::uint64_t ms = 1000; ms < 1500; ms += 100) {
        CHECK(Count(RunFrame(c, g, ms), addr::kEmitterSwitch) == 0);
    }
    CHECK(c.Totals().puppets_unseen_on == 0);
}

TEST_CASE("dbno body: our own Link and a teammate both downed: both emitters off, both back on") {
    Guest g;
    Crawl c;
    g_lists = {};
    GivePuppet(g, kPuppet, kTable, kPuppetEmitter);
    c.SetPuppetActor([](std::uint8_t n) { return n == 2 ? kPuppet : 0; });
    Crawl::Wanted w = Want(true);
    w.unseen_puppets = {{2, "downed"}};
    c.Want(w);
    RunFrame(c, g, 1000);
    CHECK(g_lists.emitter_off.count({kEmitter + addr::kEmitterKeys, addr::kOurKey}) == 1);
    CHECK(g_lists.emitter_off.count({kPuppetEmitter + addr::kEmitterKeys, addr::kOurKey}) == 1);
    c.Want(Want(false));
    RunFrame(c, g, 2000);
    CHECK(g_lists.emitter_off.empty());
}

TEST_CASE("dbno body: why a teammate's puppet is unseen: downed, then every PLAYER_STATUS away bit") {
    namespace st = coop::protocol::status;
    CHECK(UnseenWhy(true, 0) == "downed");
    CHECK(UnseenWhy(true, st::paused) == "downed");
    CHECK(UnseenWhy(false, st::dead) == "dead");
    CHECK(UnseenWhy(false, st::loading) == "loading");
    CHECK(UnseenWhy(false, st::in_cutscene) == "in their event");
    CHECK(UnseenWhy(false, st::in_menu) == "in a menu");
    CHECK(UnseenWhy(false, st::paused) == "paused");
    CHECK(UnseenWhy(false, st::paused | st::in_menu) == "in a menu");
    CHECK_FALSE(UnseenWhy(false, 0).has_value());  // playing: seen
    for (int bit = 0; bit < 8; ++bit) {
        const auto b = static_cast<std::uint8_t>(1u << bit);
        CHECK_MESSAGE(UnseenWhy(false, b).has_value() == ((b & st::away) != 0), "bit ", bit);
    }
}

TEST_CASE("dbno body: a paused teammate's puppet is unseen until they play again, with the reason in the log") {
    Guest g;
    Crawl c;
    g_lists = {};
    GivePuppet(g, kPuppet, kTable, kPuppetEmitter);
    c.SetPuppetActor([](std::uint8_t n) { return n == 2 ? kPuppet : 0; });
    c.Want(Teammates({2}, "paused"));
    REQUIRE(Count(RunFrame(c, g, 1000), addr::kEmitterSwitch) == 1);
    CHECK(g_lists.emitter_off.count({kPuppetEmitter + addr::kEmitterKeys, addr::kOurKey}) == 1);
    const auto on = c.TakeNotes();
    CHECK(std::find(on.begin(), on.end(),
                    "the game's perception emitter off for player 2's puppet: they are paused") != on.end());
    // Paused -> in a menu: still unseen, no second call.
    c.Want(Teammates({2}, "in a menu"));
    CHECK(RunFrame(c, g, 1100).empty());
    // Playing again: on again.
    c.Want(Teammates({}));
    REQUIRE(Count(RunFrame(c, g, 1200), addr::kEmitterSwitch) == 1);
    CHECK(g_lists.emitter_off.empty());
    const auto off = c.TakeNotes();
    CHECK(std::find(off.begin(), off.end(),
                    "the game's perception emitter on again for player 2's puppet: they are back") != off.end());
}
