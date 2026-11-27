// Tests for our downed Link's body: crawl, shield, unseen, heal (lichii/hook/coop_dbno_body.cpp).
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
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
constexpr std::uint64_t kActor = 0x50000;   // our Link (the adult's lying pose)
constexpr std::uint64_t kActor2 = 0x60000;  // another Link (an age change, a reload)

bn::Request Req(std::int32_t v, std::int32_t slot = 0, std::uint64_t node = kNode) {
    coop::protocol::BossAction a;
    a.verb = v;
    a.slot = slot;
    a.flags = 3;
    return bn::RequestFromAction(a, node);
}

Crawl::Wanted Want(bool lying, bool event = false, bool adult = false, std::uint64_t actor = 0) {
    Crawl::Wanted w;
    w.lying = lying;
    w.actor = actor;
    w.event = event;
    w.adult = adult;
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
    std::string as;         // an AS play's command
    float rate = -1.0f;     // ... and rate
};

// An actor's AS controller as the game keeps it (CoopPuppet::addr::kControllerPath; slot 0 only).
void SetAs(Guest& g, const std::string& name, float rate, std::uint64_t actor = kActor) {
    const std::uint64_t slot = actor + 0x5000, text = actor + 0x6000;
    std::array<char, 64> buf{};
    std::memcpy(buf.data(), name.data(), std::min(name.size(), buf.size() - 1));
    g.Write(text, buf.data(), buf.size());
    g.Write(slot + addr::kAsSlotRate, &rate, sizeof rate);
}
void MakeAs(Guest& g, const std::string& name, float rate, std::uint64_t actor = kActor) {
    namespace pa = CoopPuppet::addr;
    std::uint64_t at = actor;
    std::uint64_t next = actor + 0x100;
    for (const std::uint64_t step : pa::kControllerPath) {
        g.Write(at + step, &next, sizeof next);
        at = next;
        next += 0x100;
    }
    const std::int32_t count = 2;
    const std::uint64_t slots = actor + 0x4000, slot = actor + 0x5000, text = actor + 0x6000;
    const float frame = 0.0f;
    g.Write(at + pa::kSlotCount, &count, sizeof count);
    g.Write(at + pa::kSlots, &slots, sizeof slots);
    g.Write(slots, &slot, sizeof slot);
    g.Write(slot + pa::kSlotName, &text, sizeof text);
    g.Write(slot + pa::kSlotFrame, &frame, sizeof frame);
    SetAs(g, name, rate, actor);
}

// One frame of the caller: action requests answered `result`, the malloc
// kBlock, the rest 0. Returns every call made.
std::vector<Made> RunFrame(Crawl& c, Guest& g, std::uint64_t now_ms, std::uint64_t result = 0) {
    std::vector<Made> made;
    auto call = c.Next(g, false, 0, now_ms, kMain);
    while (call) {
        Made m{call->function - kMain, call->x[0], call->x[1], call->x[2], 0, std::string(), -1.0f};
        std::uint64_t r = 0;
        if (m.function == bn::addr::kMalloc) {
            CHECK(call->x[0] == addr::kBlockSize);
            r = kBlock;
        } else if (m.function == CoopPuppet::addr::kPlayAs) {
            // The game's play call: the command goes on the actor's slot 0 at the rate.
            CHECK(m.x0 == kBlock + addr::kBlockPlay);
            std::uint64_t actor = 0, text = 0;
            float frame_from = 0.0f, blend = 0.0f;
            g.Read(m.x0, &actor, sizeof actor);
            g.Read(m.x0 + 8, &text, sizeof text);
            g.Read(m.x0 + 0x14, &m.rate, sizeof m.rate);
            g.Read(m.x0 + 0x18, &frame_from, sizeof frame_from);
            g.Read(m.x0 + 0x1c, &blend, sizeof blend);
            CHECK(frame_from == -1.0f);
            CHECK(blend == -1.0f);
            CHECK(text == kBlock + addr::kBlockText);
            std::array<char, addr::kTextSize> name{};
            g.Read(text, name.data(), name.size());
            REQUIRE(name.back() == 0);
            m.as = name.data();
            CHECK(actor != 0);
            SetAs(g, m.as, m.rate, actor);
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

std::vector<Made> Plays(const std::vector<Made>& made) {
    std::vector<Made> out;
    for (const Made& m : made) {
        if (m.function == CoopPuppet::addr::kPlayAs) {
            out.push_back(m);
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

// A Crawl that has learnt our Link's node and is crawling at 1000 ms. The
// frame our Link goes down, the game's own Wait already ran on the slot (it
// is busy): nothing is asked; the next frame's Wait is skipped and ours asked.
void Crawling(Crawl& c, Guest& g) {
    c.Want(Want(false));
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    c.Want(Want(true));
    REQUIRE(Verbs(RunFrame(c, g, 984)).empty());
    REQUIRE(c.Skip(kBsa, Req(verb::kWait, 2), false));
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
    CHECK(Verbs(RunFrame(c, g, 984, 2)).empty());  // (the slot was the game's this frame)
    CHECK(Verbs(RunFrame(c, g, 1000, 2)) == std::vector<std::int32_t>{verb::kCrawl});  // 2: refused
    CHECK_FALSE(c.Skip(kBsa, Req(71), false));  // climbing a ladder: the game's own
    CHECK_FALSE(c.Skip(kBsa, Req(134), false));
    CHECK(c.Totals().skipped == 0);
    CHECK(c.Totals().refused == 1);
    CHECK(c.Totals().busy == 0);
}

TEST_CASE("dbno body (row 190): the game's own requests on our slot are skipped while the crawl is asked, so it gets the slot") {
    // The game takes one request per slot a frame (main+0xc08f0 answers 3 to
    // a second one): the 15:38 run's 6482 'refused (3)' lines, Geno's 162.
    Guest g;
    Crawl d;
    d.Want(Want(false));
    CHECK_FALSE(d.Skip(kBsa, Req(verb::kWait), false));  // up: the game's, and the slot is its for this frame
    d.Want(Want(true));
    // Down in that frame: the slot is known busy, so nothing is asked (an ask would get 3).
    CHECK(Verbs(RunFrame(d, g, 1000)).empty());
    CHECK(d.Totals().held_back == 1);
    CHECK(d.Totals().asked == 0);
    // The next frame: the scheduler's own Wait, before our caller: skipped. Another slot's request has its own
    // record: it runs.
    CHECK(d.Skip(kBsa, Req(verb::kWait), false));
    CHECK(d.Skip(kBsa, Req(134), false));
    CHECK_FALSE(d.Skip(kBsa, Req(0, 1), false));
    CHECK(Verbs(RunFrame(d, g, 1016, 0)) == std::vector<std::int32_t>{verb::kCrawl});
    CHECK(Verbs(RunFrame(d, g, 1032, 0)).empty());  // taken
    const auto t = d.Totals();
    CHECK(t.busy == 0);
    CHECK(t.refused == 0);
    CHECK(t.cleared == 2);
    CHECK(t.asked == 1);
    bool refused_line = false;
    bool taken_line = false;
    for (const auto& n : d.TakeNotes()) {
        refused_line = refused_line || n.find("refused") != std::string::npos;
        taken_line = taken_line || n.find("Crawl (234) asked and taken") != std::string::npos;
    }
    CHECK_FALSE(refused_line);
    CHECK(taken_line);
    CHECK(d.Skip(kBsa, Req(134), false));     // crawling: skipped as before
    CHECK(d.Skip(kBsa, Req(0, 1), false));    // ... on every slot (a reaction may not stand our Link up)
}

TEST_CASE("dbno body (row 190): a 3 all the same is asked again the next frame, and is no refusal") {
    Guest g;
    Crawl d;
    d.Want(Want(false));
    d.Skip(kBsa, Req(verb::kWait), false);
    d.Want(Want(true));
    RunFrame(d, g, 1000);
    CHECK(d.Skip(kBsa, Req(verb::kWait), false));
    CHECK(Verbs(RunFrame(d, g, 1016, Crawl::kSlotBusy)) == std::vector<std::int32_t>{verb::kCrawl});
    CHECK(d.Skip(kBsa, Req(verb::kWait), false));  // still a round: still skipped
    CHECK(Verbs(RunFrame(d, g, 1032, 0)) == std::vector<std::int32_t>{verb::kCrawl});  // not kAskEveryMs later
    CHECK(Verbs(RunFrame(d, g, 1048, 0)).empty());
    const auto t = d.Totals();
    CHECK(t.busy == 1);
    CHECK(t.refused == 0);
    for (const auto& n : d.TakeNotes()) {
        CHECK(n.find("refused") == std::string::npos);
    }
}

TEST_CASE("dbno body (row 190): a world's verb let through on our slot holds our ask back that frame") {
    // The game asks Land and then Wait in one frame: Land runs (the world's) and takes the slot, the Wait is
    // skipped, and an ask of ours would get 3 (the adult run of 11-16: six in a row, the whole round).
    Guest g;
    Crawl c;
    Crawling(c, g);
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kFall, 2), false));  // off a ledge
    RunFrame(c, g, 1100);
    const auto asked = c.Totals().asked;
    for (std::uint64_t ms = 1116; ms < 1500; ms += 16) {
        CHECK_FALSE(c.Skip(kBsa, Req(verb::kLand, 2), false));
        c.Skip(kBsa, Req(verb::kWait, 2), false);
        CHECK(Verbs(RunFrame(c, g, ms, Crawl::kSlotBusy)).empty());
    }
    CHECK(c.Totals().asked == asked);
    CHECK(c.Totals().busy == 0);
    // Landed: only the Wait, skipped; the crawl is asked and taken.
    CHECK(c.Skip(kBsa, Req(verb::kWait, 2), false));
    CHECK(Verbs(RunFrame(c, g, 1500)) == std::vector<std::int32_t>{verb::kCrawl});
    CHECK(c.Skip(kBsa, Req(134, 2), false));
}

TEST_CASE("dbno body (row 190): a crawl the game does not take on the spot is not asked for ever") {
    // Geno's boss run: 162 and 155 'refused (3)' lines, one every 500 ms for the whole downing: each ground
    // Walk / Wait of the game's reset the count. Now: rounds of at most kMaxAsks, kRetryMs apart, kSpotRounds of
    // them after the first; then nothing until our Link really moves.
    for (const std::uint64_t answer : {std::uint64_t{2}, Crawl::kSlotBusy}) {
        Guest g;
        Crawl c;
        c.Want(Want(false));
        c.Skip(kBsa, Req(verb::kWait), false);
        c.Want(Want(true));
        int asks = 0;
        int skipped = 0;
        for (std::uint64_t ms = 1000; ms < 121000; ms += 16) {  // two minutes of Walk / Wait every frame
            skipped += c.Skip(kBsa, Req((ms / 16) % 2 == 0 ? verb::kWait : verb::kWalk), false) ? 1 : 0;
            asks += static_cast<int>(Verbs(RunFrame(c, g, ms, answer)).size());
        }
        const int rounds = 1 + Crawl::kSpotRounds;
        if (answer == Crawl::kSlotBusy) {
            CHECK(asks == rounds * Crawl::kMaxAsks);  // each round runs out of asks
            CHECK(c.Totals().refused == 0);
        } else {
            CHECK(asks == rounds);  // a real refusal ends its round at once
            CHECK(c.Totals().refused == static_cast<std::uint64_t>(rounds));
        }
        CHECK(skipped <= rounds * (Crawl::kMaxAsks + 1));  // the game's own run again once a round is over
        CHECK(c.Totals().rounds == static_cast<std::uint64_t>(rounds));
        std::size_t lines = 0;
        for (std::uint64_t ms = 121000; ms < 122000; ms += 16) {
            lines += c.TakeNotes().size();
            c.Skip(kBsa, Req(verb::kWait), false);
            RunFrame(c, g, ms, answer);
        }
        CHECK(lines <= 32);  // (what was noted before; nothing new)

        // A real move of our Link (a ladder: the game's own, it runs), then the ground: a new round at once.
        CHECK_FALSE(c.Skip(kBsa, Req(71), false));
        CHECK(Verbs(RunFrame(c, g, 122000, answer)).empty());
        CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait), false));  // let through (no round yet): the slot is busy
        CHECK(Verbs(RunFrame(c, g, 122016, answer)).empty());
        CHECK(c.Skip(kBsa, Req(verb::kWait), false));
        CHECK(Verbs(RunFrame(c, g, 122032, 0)) == std::vector<std::int32_t>{verb::kCrawl});
        CHECK(c.Skip(kBsa, Req(134), false));  // taken: crawling
    }
}

TEST_CASE("dbno body: the verbs by age (Crawl down for both, up: Wait, or LDamageStandUp from the adult's pose)") {
    CHECK(DownVerb(false) == verb::kCrawl);
    CHECK(DownVerb(true) == verb::kCrawl);
    CHECK(UpVerb(false) == verb::kWait);
    CHECK(UpVerb(true) == verb::kLDamageStandUp);
    CHECK(std::string(VerbName(verb::kLDamageStandUp)) == "LDamageStandUp");
    CHECK(PoseWanted(true, true, false, true, false));
    CHECK_FALSE(PoseWanted(false, true, false, true, false));  // a child: the crawl's own clips
    CHECK_FALSE(PoseWanted(true, false, false, true, false));  // up
    CHECK_FALSE(PoseWanted(true, true, true, true, false));    // an event drives him
    CHECK_FALSE(PoseWanted(true, true, false, false, false));  // the crawl not taken (yet, or a fall)
    CHECK_FALSE(PoseWanted(true, true, false, true, true));    // on a horse
    CHECK(Riding("HorseWait"));
    CHECK(Riding("HorseMove"));
    CHECK(Riding("AimHorseLowerF"));
    CHECK(Riding("RideonRunHorse"));
    CHECK(Riding("RideOnDirectHorse"));
    CHECK(Riding("DismountHorse"));
    CHECK_FALSE(Riding("Wait"));
    CHECK_FALSE(Riding("Crouch"));
    CHECK_FALSE(Riding(std::string(kLyingPose)));
}

TEST_CASE("dbno body: slot 0 is read as the puppets read it") {
    Guest g;
    CHECK_FALSE(ReadSlot0(g, kActor));  // no AS controller
    CHECK_FALSE(ReadSlot0(g, 0));
    MakeAs(g, "Crouch", 1.0f);
    const auto s = ReadSlot0(g, kActor);
    REQUIRE(s);
    CHECK(s->name == "Crouch");
    CHECK(s->rate == 1.0f);
    SetAs(g, "DamageLStand", 0.0f);
    CHECK(ReadSlot0(g, kActor)->name == "DamageLStand");
    CHECK(ReadSlot0(g, kActor)->rate == 0.0f);
}

TEST_CASE("dbno body: a downed adult in Crawl lies in the game's DamageLStand held at frame 0, and gets up with it") {
    Guest g;
    MakeAs(g, "Wait", 1.0f);
    Crawl c;
    c.Want(Want(false, false, true, kActor));
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    c.Want(Want(true, false, true, kActor));
    auto f = RunFrame(c, g, 984);
    CHECK(Verbs(f).empty());
    CHECK(Plays(f).empty());  // not in our Crawl yet: nothing played
    CHECK(c.Skip(kBsa, Req(verb::kWait, 2), false));
    f = RunFrame(c, g, 1000);
    CHECK(Verbs(f) == std::vector<std::int32_t>{verb::kCrawl});
    CHECK(Plays(f).empty());
    SetAs(g, "Crouch", 1.0f);  // the crawl's own command (no clips on an adult: he stood upright)
    f = RunFrame(c, g, 1100);
    REQUIRE(Plays(f).size() == 1);
    CHECK(Plays(f)[0].as == "DamageLStand");
    CHECK(Plays(f)[0].rate == 0.0f);  // held: frame 0 is him lying
    CHECK(ReadSlot0(g, kActor)->name == "DamageLStand");
    CHECK(RunFrame(c, g, 1200).empty());  // held: nothing more
    CHECK(RunFrame(c, g, 1300).empty());
    // The game plays its own on slot 0 (the crawl's move): played again, held.
    SetAs(g, "Crouch", 1.0f);
    f = RunFrame(c, g, 1400);
    REQUIRE(Plays(f).size() == 1);
    CHECK(Plays(f)[0].rate == 0.0f);
    CHECK(c.Totals().poses == 2);
    CHECK(c.Skip(kBsa, Req(verb::kLDamageStandUp, 2), false));  // the game's own stand-up while down: skipped

    // Revived (held A): the game's own Walk / Wait on our slot is skipped from
    // the up frame on, the pose plays on at rate 1 (the game's stand-up) and
    // the game's LDamageStandUp is asked.
    c.Want(Want(false, false, true, kActor));
    CHECK(c.Skip(kBsa, Req(verb::kWait, 2), false));
    CHECK_FALSE(c.Skip(kBsa, Req(134, 1), false));  // another slot's: the game's
    f = RunFrame(c, g, 2000);
    REQUIRE(Plays(f).size() == 1);
    CHECK(Plays(f)[0].rate == 1.0f);
    CHECK(Verbs(f) == std::vector<std::int32_t>{verb::kLDamageStandUp});
    CHECK(c.Totals().releases == 1);
    // Taken: the game's own run again, nothing more is asked or played.
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait, 2), false));
    CHECK(RunFrame(c, g, 2100).empty());
    SetAs(g, "Wait", 1.0f);
    CHECK(RunFrame(c, g, 2200).empty());
    CHECK(RunFrame(c, g, 2300).empty());
}

TEST_CASE("dbno body: getting up, a stand-up the game keeps held is played on again (bounded), a busy slot asked again") {
    Guest g;
    MakeAs(g, "Wait", 1.0f);
    Crawl c;
    c.Want(Want(false, false, true, kActor));
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    c.Want(Want(true, false, true, kActor));
    RunFrame(c, g, 984);
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    REQUIRE(Verbs(RunFrame(c, g, 1000)) == std::vector<std::int32_t>{verb::kCrawl});
    REQUIRE(Plays(RunFrame(c, g, 1100)).size() == 1);
    c.Want(Want(false, false, true, kActor));
    auto f = RunFrame(c, g, 2000, CoopDbno::Body::Crawl::kSlotBusy);  // 3: asked again next frame
    CHECK(Verbs(f) == std::vector<std::int32_t>{verb::kLDamageStandUp});
    CHECK(c.Skip(kBsa, Req(verb::kWait, 2), false));  // still left to ours
    SetAs(g, "DamageLStand", 0.0f);  // the game's own ask of the same command kept it held
    f = RunFrame(c, g, 2100);
    CHECK(Plays(f).size() == 1);
    CHECK(Verbs(f) == std::vector<std::int32_t>{verb::kLDamageStandUp});
    int plays = 0;
    for (std::uint64_t ms = 2200; ms < 4000; ms += 100) {
        SetAs(g, "DamageLStand", 0.0f);
        plays += static_cast<int>(Plays(RunFrame(c, g, ms)).size());
    }
    CHECK(plays + 2 <= kMaxReleases);  // never played for ever
    CHECK(c.Totals().releases <= static_cast<std::uint64_t>(kMaxReleases));
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait, 2), false));  // the round is over: the game's own runs
}

TEST_CASE("dbno body: a refused stand-up leaves the game's own Wait to stand the adult up") {
    Guest g;
    MakeAs(g, "Wait", 1.0f);
    Crawl c;
    c.Want(Want(false, false, true, kActor));
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    c.Want(Want(true, false, true, kActor));
    RunFrame(c, g, 984);
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    RunFrame(c, g, 1000);
    RunFrame(c, g, 1100);
    c.Want(Want(false, false, true, kActor));
    CHECK(Verbs(RunFrame(c, g, 2000, 2)) == std::vector<std::int32_t>{verb::kLDamageStandUp});
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait, 2), false));
    CHECK(Verbs(RunFrame(c, g, 2100)).empty());
}

TEST_CASE("dbno body: refusals while down are noted three times, then counted only") {
    Guest g;
    Crawl c;
    c.Want(Want(false));
    c.Skip(kBsa, Req(verb::kWait), false);
    c.Want(Want(true));
    c.TakeNotes();
    // A boss fight: events come and go while our Link lies there, each a fresh round of asks, all refused.
    std::uint64_t ms = 1000;
    for (int round = 0; round < 20; ++round) {
        c.Want(Want(true, true));
        RunFrame(c, g, ms += 100, 2);
        c.Want(Want(true));
        RunFrame(c, g, ms += 100, 2);
    }
    int noted = 0;
    for (const auto& n : c.TakeNotes()) {
        noted += n.find("refused") != std::string::npos ? 1 : 0;
    }
    CHECK(noted == 3);
    CHECK(c.Totals().refused >= 20);
    // Up and down again: noted again.
    c.Want(Want(false));
    RunFrame(c, g, ms += 100);
    c.Want(Want(true));
    RunFrame(c, g, ms += 5000, 2);
    int again = 0;
    for (const auto& n : c.TakeNotes()) {
        again += n.find("refused") != std::string::npos ? 1 : 0;
    }
    CHECK(again == 1);
}

TEST_CASE("dbno body (row 190 + PR #308): slot-busy waits are noted three times a downing too, then counted only") {
    Guest g;
    Crawl c;
    c.Want(Want(false));
    c.Skip(kBsa, Req(verb::kWait), false);
    c.Want(Want(true));
    c.TakeNotes();
    // A boss fight: each event starts a new round; every ask answered 3 (the slot took a request that frame).
    std::uint64_t ms = 1000;
    for (int round = 0; round < 20; ++round) {
        c.Want(Want(true, true));
        RunFrame(c, g, ms += 100, 3);
        c.Want(Want(true));
        RunFrame(c, g, ms += 100, 3);
    }
    int noted = 0;
    for (const auto& n : c.TakeNotes()) {
        noted += n.find("waits") != std::string::npos ? 1 : 0;
    }
    CHECK(noted == 3);
    CHECK(c.Totals().busy >= 20);
    CHECK(c.Totals().refused == 0);  // (3 is not a refusal)
}

TEST_CASE("dbno body: a child whose game crawl runs is played no pose (the crawl has its own clips)") {
    Guest g;
    MakeAs(g, "Wait", 1.0f);
    Crawl c;
    c.Want(Want(false, false, false, kActor));
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    c.Want(Want(true, false, false, kActor));
    RunFrame(c, g, 984);
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    CHECK(Verbs(RunFrame(c, g, 1000)) == std::vector<std::int32_t>{verb::kCrawl});
    for (std::uint64_t ms = 1100; ms < 3000; ms += 100) {
        SetAs(g, "Crouch", 1.0f);
        CHECK(Plays(RunFrame(c, g, ms)).empty());
    }
    c.Want(Want(false, false, false, kActor));
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait, 2), false));  // no stand-up round for a child
    const auto f = RunFrame(c, g, 4000);
    CHECK(Plays(f).empty());
    CHECK(Verbs(f).empty());  // (the game's own Wait took the slot this frame: it stands him up)
    CHECK(c.Totals().poses == 0);
}

TEST_CASE("dbno body: an age change or a new Link while down: the pose goes with the Link it was put on") {
    Guest g;
    MakeAs(g, "Wait", 1.0f);
    Crawl c;
    c.Want(Want(false, false, true, kActor));
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    c.Want(Want(true, false, true, kActor));
    RunFrame(c, g, 984);
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    RunFrame(c, g, 1000);
    REQUIRE(Plays(RunFrame(c, g, 1100)).size() == 1);
    // The age change is an event (the Master Sword's pedestal, the Song of
    // Time): nothing played meanwhile.
    c.Want(Want(true, true, true, kActor));
    SetAs(g, "EvtWait", 1.0f);
    CHECK(Plays(RunFrame(c, g, 1200)).empty());
    // A child after it (a new Link actor): the crawl asked again (kAskEveryMs after the last), no pose.
    MakeAs(g, "Wait", 1.0f, kActor2);
    c.Want(Want(true, false, false, kActor2));
    CHECK(c.Skip(kBsa, Req(verb::kWait, 2), false));
    auto f = RunFrame(c, g, 1600);
    CHECK(Verbs(f) == std::vector<std::int32_t>{verb::kCrawl});
    CHECK(Plays(f).empty());
    CHECK(Plays(RunFrame(c, g, 1700)).empty());
    // Back to an adult (a new Link again): laid down on that one.
    MakeAs(g, "Crouch", 1.0f, kActor);
    c.Want(Want(true, false, true, kActor));
    f = RunFrame(c, g, 1800);
    REQUIRE(Plays(f).size() == 1);
    CHECK(Plays(f)[0].rate == 0.0f);
    // A reload (Game Over, a scene's re-entry) gives a new Link that is up:
    // no stand-up, nothing played on it.
    MakeAs(g, "Wait", 1.0f, kActor2);
    c.Want(Want(false, false, true, kActor2));
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait, 2), false));
    f = RunFrame(c, g, 1900);
    CHECK(Plays(f).empty());
    for (const auto v : Verbs(f)) {
        CHECK(v != verb::kLDamageStandUp);
    }
}

TEST_CASE("dbno body: a downed adult on a horse is asked no crawl until he is off it") {
    Guest g;
    MakeAs(g, "Wait", 1.0f);
    Crawl c;
    c.Want(Want(false, false, true, kActor));
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    SetAs(g, "HorseMove", 1.0f);
    c.Want(Want(true, false, true, kActor));
    int asks = 0;
    for (std::uint64_t ms = 1000; ms < 5000; ms += 100) {
        const auto f = RunFrame(c, g, ms);
        asks += static_cast<int>(Verbs(f).size());
        CHECK(Plays(f).empty());
        CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait, 2), false));  // the game's ride goes on
    }
    CHECK(asks == 0);
    CHECK(c.Totals().riding > 0);
    SetAs(g, "Wait", 1.0f);  // off the horse
    RunFrame(c, g, 5000);    // (the game's Wait of the last frame took the slot)
    CHECK(c.Skip(kBsa, Req(verb::kWait, 2), false));
    CHECK(Verbs(RunFrame(c, g, 5100)) == std::vector<std::int32_t>{verb::kCrawl});
    SetAs(g, "Crouch", 1.0f);
    CHECK(Plays(RunFrame(c, g, 5200)).size() == 1);
}

TEST_CASE("dbno body: a fall while the adult lies: the game's own plays, laid down again on the ground") {
    Guest g;
    MakeAs(g, "Wait", 1.0f);
    Crawl c;
    c.Want(Want(false, false, true, kActor));
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    c.Want(Want(true, false, true, kActor));
    RunFrame(c, g, 984);
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    RunFrame(c, g, 1000);
    REQUIRE(Plays(RunFrame(c, g, 1100)).size() == 1);
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kFall, 2), false));
    SetAs(g, "Fall", 1.0f);
    for (std::uint64_t ms = 1200; ms < 2000; ms += 100) {
        CHECK(Plays(RunFrame(c, g, ms, 2)).empty());  // the game's fall plays
    }
    SetAs(g, "Land", 1.0f);
    CHECK(c.Skip(kBsa, Req(verb::kWait, 2), false));
    CHECK(Verbs(RunFrame(c, g, 2000)) == std::vector<std::int32_t>{verb::kCrawl});
    CHECK(Plays(RunFrame(c, g, 2100)).size() == 1);  // lying again
}

TEST_CASE("dbno body: solo or up, an adult with no downed state is never read or played") {
    Guest g;
    MakeAs(g, "Wait", 1.0f);
    Crawl c;
    c.Want(Want(false, false, true, kActor));
    for (std::uint64_t ms = 0; ms < 3000; ms += 100) {
        CHECK(RunFrame(c, g, ms).empty());
        CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait, 2), false));
        CHECK_FALSE(c.Skip(kBsa, Req(verb::kLDamageStandUp, 2), false));
    }
    CHECK(c.Totals().poses == 0);
    CHECK(c.Totals().releases == 0);
}

TEST_CASE("dbno body: a fall or water while crawling passes, and the crawl is asked again on the ground") {
    Guest g;
    Crawl c;
    Crawling(c, g);
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kFall), false));  // off a ledge
    CHECK(c.Totals().passed == 1);
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kLand), false));  // no longer crawling: the game's
    CHECK_FALSE(c.Skip(kBsa, Req(134), false));
    // Not asked while the world moves our Link.
    int asks = 0;
    for (std::uint64_t ms = 1100; ms < 9000; ms += 100) {
        asks += static_cast<int>(Verbs(RunFrame(c, g, ms, 2)).size());
    }
    CHECK(asks == 0);
    // Back on the ground (the game's own Wait, skipped: it would take the
    // slot): asked at once, taken, skipping again.
    CHECK(c.Skip(kBsa, Req(verb::kWait), false));
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
    CHECK(Verbs(RunFrame(c, g, 984)).empty());  // (the Walk ran on the slot this frame)
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

TEST_CASE("dbno body: up in a frame the game's own Wait ran: that stands our Link up, ours is not asked on top") {
    Guest g;
    Crawl c;
    Crawling(c, g);
    c.Want(Want(false));
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait, 2), false));  // up: no longer skipped, it takes the slot
    const auto asked = c.Totals().asked;
    CHECK(Verbs(RunFrame(c, g, 6000, Crawl::kSlotBusy)).empty());
    CHECK(Verbs(RunFrame(c, g, 6016, Crawl::kSlotBusy)).empty());
    CHECK(c.Totals().asked == asked);
    bool said = false;
    for (const auto& n : c.TakeNotes()) {
        CHECK(n.find("refused") == std::string::npos);
        said = said || n.find("the game's own request stands it up") != std::string::npos;
    }
    CHECK(said);
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

TEST_CASE("dbno body (row 177): a teammate's puppet is unseen while they are in the unseen list") {
    Crawl c;
    CHECK_FALSE(c.PuppetUnseen(2));
    Crawl::Wanted w;
    w.unseen_puppets = {{2, "downed"}};
    c.Want(w);
    CHECK(c.PuppetUnseen(2));
    CHECK_FALSE(c.PuppetUnseen(3));
    c.Want(Crawl::Wanted{});
    CHECK_FALSE(c.PuppetUnseen(2));
}

TEST_CASE("dbno body: issue 337, the game's own CrawlAnchor in front of a downed child, facing him") {
    const CoopPuppet::Mat3 identity{1, 0, 0, 0, 1, 0, 0, 0, 1};  // his front: +Z
    const std::array<float, 3> at{10.0f, 2.0f, -5.0f};
    const auto spot = AnchorFor(true, false, false, at, identity);
    REQUIRE(spot);
    CHECK(spot->position[0] == doctest::Approx(10.0f));
    CHECK(spot->position[1] == doctest::Approx(2.0f));
    CHECK(spot->position[2] == doctest::Approx(-5.0f + kAnchorAhead));
    // The anchor's +Z (its rotation's third column) points back at him.
    CHECK(spot->rotation[2] == doctest::Approx(0.0f));
    CHECK(spot->rotation[8] == doctest::Approx(-1.0f));
    CHECK(spot->rotation[4] == doctest::Approx(1.0f));

    // Turned 90 degrees (front +X), tilted rows still give a flat front.
    const CoopPuppet::Mat3 east{0, 0, 1, 0, 1, 0, -1, 0, 0};
    const auto e = AnchorFor(true, false, false, at, east);
    REQUIRE(e);
    CHECK(e->position[0] == doctest::Approx(10.0f + kAnchorAhead));
    CHECK(e->position[2] == doctest::Approx(-5.0f));
    CHECK(e->rotation[2] == doctest::Approx(-1.0f));  // +Z column = (-1, 0, 0)
    CHECK(e->rotation[8] == doctest::Approx(0.0f));
    // Still a rotation: rows unit and orthogonal.
    CHECK(e->rotation[0] * e->rotation[0] + e->rotation[2] * e->rotation[2] == doctest::Approx(1.0f));

    // None: up, in an event, an adult (no crawl for him), no flat front, a bad place.
    CHECK_FALSE(AnchorFor(false, false, false, at, identity));
    CHECK_FALSE(AnchorFor(true, true, false, at, identity));
    CHECK_FALSE(AnchorFor(true, false, true, at, identity));
    CHECK_FALSE(AnchorFor(true, false, false, at, CoopPuppet::Mat3{1, 0, 0, 0, 0, 1, 0, -1, 0}));
    CHECK_FALSE(AnchorFor(true, false, false, {NAN, 0, 0}, identity));
}

TEST_CASE("dbno body: issue 337, each down gets its own anchor key, kept while he stays down") {
    SetAnchor(std::nullopt);
    const CoopPuppet::Mat3 identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    SetAnchor(AnchorFor(true, false, false, {0, 0, 0}, identity));
    const auto first = Anchor();
    REQUIRE(first);
    SetAnchor(AnchorFor(true, false, false, {1, 0, 0}, identity));  // he crawls on: same anchor, moved
    REQUIRE(Anchor());
    CHECK(Anchor()->down == first->down);
    CHECK(Anchor()->position[0] == doctest::Approx(1.0f));
    SetAnchor(std::nullopt);  // up
    CHECK_FALSE(Anchor());
    SetAnchor(AnchorFor(true, false, false, {0, 0, 0}, identity));  // down again: a new one
    REQUIRE(Anchor());
    CHECK(Anchor()->down == first->down + 1);
    SetAnchor(std::nullopt);
}

TEST_CASE("dbno body: issue 337, the game's own crawl commands") {
    // The three commands of AS/Player_Child.root.asb that play the Tunnel_* clips (the 0.11.13 FAIL run's
    // slot 0 read 'CrouchLockOnMove' while the crawl ran).
    CHECK(CrawlCommand("Crouch"));
    CHECK(CrawlCommand("CrouchLockOnMove"));
    CHECK(CrawlCommand("CrouchLockOnMoveEnd"));
    // Clip names and guesses are not commands.
    CHECK_FALSE(CrawlCommand("Tunnel_Move_F"));
    CHECK_FALSE(CrawlCommand("Crawl"));
    CHECK_FALSE(CrawlCommand("CrouchGuard"));
    CHECK_FALSE(CrawlCommand("CrouchMove"));
    CHECK_FALSE(CrawlCommand("Wait"));
    CHECK_FALSE(CrawlCommand("Walk"));
    CHECK_FALSE(CrawlCommand("DamageLStand"));
    CHECK_FALSE(CrawlCommand(""));
}

TEST_CASE("dbno body: issue 337, a child's Crawl waits until this down's anchor is a live actor") {
    // Run crawl-337-2-r2: Crawl (234) was asked and taken before the async CrawlAnchor existed; slot 0 stayed
    // DamageS. The copy director's on_made (coop_spawn) marks this down's anchor live; the ask waits for it.
    const CoopPuppet::Mat3 identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    SetAnchor(std::nullopt);
    CHECK_FALSE(AnchorReady());  // no anchor wanted
    SetAnchor(AnchorFor(true, false, false, {0, 0, 0}, identity));
    REQUIRE(Anchor());
    const std::uint32_t down = Anchor()->down;
    CHECK_FALSE(AnchorReady());  // wanted, not made yet: the Crawl waits
    AnchorLive(down - 1);        // an older down's copy: not this one
    CHECK_FALSE(AnchorReady());
    AnchorLive(down);
    CHECK(AnchorReady());
    SetAnchor(AnchorFor(true, false, false, {1, 0, 0}, identity));  // moved with him: still live
    CHECK(AnchorReady());
    SetAnchor(std::nullopt);  // up
    CHECK_FALSE(AnchorReady());
    SetAnchor(AnchorFor(true, false, false, {0, 0, 0}, identity));  // a new down: waits for its own anchor
    CHECK_FALSE(AnchorReady());
    AnchorLive(Anchor()->down);
    CHECK(AnchorReady());
    SetAnchor(std::nullopt);
}

namespace {
// Our Link's work object as the game keeps it ([actor+0x270]+0x358), with its do action.
// [actor+0x270] is also the first link of MakeAs's animation chain, so both write the same table (actor+0x100):
// either order keeps the other's part.
void SetDoAction(Guest& g, std::uint32_t action, std::uint64_t actor = kActor) {
    const std::uint64_t parts = actor + 0x100, work = actor + 0xa000;
    g.Write(actor + addr::kActorComponents, &parts, sizeof parts);
    g.Write(parts + addr::kComponentWork, &work, sizeof work);
    g.Write(work + addr::kWorkDoAction, &action, sizeof action);
}
}  // namespace

TEST_CASE("dbno body: issue 337, the do action is read from our Link's work object") {
    Guest g;
    CHECK_FALSE(ReadDoAction(g, 0));
    CHECK_FALSE(ReadDoAction(g, kActor));  // no components, no work object
    SetDoAction(g, 0);
    CHECK(ReadDoAction(g, kActor) == std::optional<std::uint32_t>(0));
    SetDoAction(g, addr::kDoActionCrawl);
    CHECK(ReadDoAction(g, kActor) == std::optional<std::uint32_t>(14));
}

TEST_CASE("dbno body: issue 337, a child's Crawl is asked only when the game itself offers it, and its own verbs run until then") {
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    Guest g;
    MakeAs(g, "DamageS", 1.0f);
    SetDoAction(g, 0);
    Crawl c;
    c.Want(Want(false, false, false, kActor));
    c.Skip(kBsa, Req(verb::kWait), false);  // the scheduler's slot
    SetAnchor(std::nullopt);
    SetAnchor(AnchorFor(true, false, false, {0, 0, 0}, identity));
    const std::uint32_t down = Anchor()->down;
    c.Want(Want(true, false, false, kActor));

    // The anchor is not a live actor yet: nothing asked, the game's damage verb and Wait run.
    CHECK_FALSE(c.Skip(kBsa, Req(134), false));
    CHECK(Verbs(RunFrame(c, g, 1000)).empty());
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait), false));
    CHECK(Verbs(RunFrame(c, g, 1016)).empty());

    // Live, but the game does not offer Crawl (the attention has not taken it): still nothing, still not skipped.
    AnchorLive(down);
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait), false));
    CHECK(Verbs(RunFrame(c, g, 1032)).empty());
    SetDoAction(g, 1);
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait), false));
    CHECK(Verbs(RunFrame(c, g, 1048)).empty());
    CHECK(c.Totals().asked == 0);

    // The game offers Crawl. Its Wait ran on the slot this frame: ours next frame, with the game's skipped.
    SetDoAction(g, addr::kDoActionCrawl);
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait), false));
    CHECK(Verbs(RunFrame(c, g, 1064)).empty());
    CHECK(c.Skip(kBsa, Req(verb::kWait), false));
    CHECK(Verbs(RunFrame(c, g, 1080)) == std::vector<std::int32_t>{verb::kCrawl});
    CHECK(c.Skip(kBsa, Req(verb::kWait), false));  // taken: our crawl holds the slot

    // The game's crawl runs: nothing more is asked, however long.
    SetAs(g, "CrouchLockOnMove", 1.0f);
    for (int i = 0; i < 3 * Crawl::kCrawlStartReads; ++i) {
        CHECK(Verbs(RunFrame(c, g, 1100 + 16 * i)).empty());
    }
    CHECK(c.GameCrawl());
    CHECK(c.Skip(kBsa, Req(verb::kWait), false));

    // It ends by itself (room to stand up: slot 0 leaves the crawl's commands). Part 2: the slot stays ours,
    // the game's lying wait is played on slot 0, and no Crawl is asked again in this down.
    CHECK_FALSE(c.LaidDown());
    SetAs(g, "Wait", 1.0f);
    SetDoAction(g, 0);
    auto f = RunFrame(c, g, 4000);
    REQUIRE(Plays(f).size() == 1);
    CHECK(Plays(f)[0].as == "Crouch");
    CHECK(Plays(f)[0].rate == 1.0f);
    CHECK(Verbs(f).empty());
    CHECK(c.LaidDown());
    CHECK(c.Totals().crawl_ended == 1);
    CHECK(c.Totals().lie_downs == 1);
    CHECK(ReadSlot0(g, kActor)->name == "Crouch");
    std::uint64_t t = 4000;
    SetDoAction(g, addr::kDoActionCrawl);  // (an offer changes nothing now)
    for (int i = 0; i < 3 * Crawl::kCrawlStartReads; ++i) {
        // 0.12.1 downed-move: the game's ground move runs (he slides lying, the pad's slow stick); v0.12.0
        // skipped it too and he lay still. Its other verbs stay skipped.
        CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait), false));
        CHECK(c.Skip(kBsa, Req(134), false));
        CHECK(RunFrame(c, g, t += 16).empty());        // held: nothing played, nothing asked
    }
    CHECK_FALSE(c.GameCrawl());  // the lying wait is ours: the pad's slow stick
    // The game plays another command on slot 0: the lying wait is played again.
    SetAs(g, "DamageS", 1.0f);
    f = RunFrame(c, g, t += 16);
    REQUIRE(Plays(f).size() == 1);
    CHECK(Plays(f)[0].as == "Crouch");
    CHECK(c.Totals().child_poses == 2);
    // A fall is the world's: let through, nothing played in the air; back on the ground Crawl is asked
    // again (no offer waited for) and he lies again.
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kFall), false));
    SetAs(g, "Fall", 1.0f);
    CHECK(Plays(RunFrame(c, g, t += 16)).empty());
    SetDoAction(g, 0);
    int asked = 0;
    for (int i = 0; i < 60; ++i) {
        c.Skip(kBsa, Req(verb::kWait), false);
        f = RunFrame(c, g, t += 100);
        asked += static_cast<int>(Verbs(f).size());
    }
    CHECK(asked == 1);
    CHECK(ReadSlot0(g, kActor)->name == "Crouch");
    CHECK(c.Totals().lie_downs == 1);  // (one down)

    // Up: Wait is the game's own; the next down starts afresh.
    SetDoAction(g, addr::kDoActionCrawl);
    c.Want(Want(false, false, false, kActor));
    SetAnchor(std::nullopt);
    RunFrame(c, g, t += 16);
    CHECK_FALSE(c.LaidDown());
    SetAnchor(AnchorFor(true, false, false, {0, 0, 0}, identity));
    AnchorLive(Anchor()->down);
    c.Want(Want(true, false, false, kActor));
    c.Skip(kBsa, Req(verb::kWait), false);
    RunFrame(c, g, t += 1000);
    c.Skip(kBsa, Req(verb::kWait), false);
    CHECK(Verbs(RunFrame(c, g, t += 16)) == std::vector<std::int32_t>{verb::kCrawl});
    CHECK_FALSE(c.LaidDown());
    SetAnchor(std::nullopt);
}

namespace {
// A downed child with this down's CrawlAnchor (live or not), the game's do action as given.
void ChildDown(Crawl& c, Guest& g, bool live, std::uint32_t action) {
    const std::array<float, 9> identity{1, 0, 0, 0, 1, 0, 0, 0, 1};
    MakeAs(g, "DamageS", 1.0f);
    SetDoAction(g, action);
    c.Want(Want(false, false, false, kActor));
    c.Skip(kBsa, Req(verb::kWait), false);  // the scheduler's slot
    SetAnchor(std::nullopt);
    SetAnchor(AnchorFor(true, false, false, {0, 0, 0}, identity));
    if (live) {
        AnchorLive(Anchor()->down);
    }
    c.Want(Want(true, false, false, kActor));
}
void SetFrame(Guest& g, float frame) {
    g.Write(kActor + 0x5000 + CoopPuppet::addr::kSlotFrame, &frame, sizeof frame);
}
}  // namespace

TEST_CASE("dbno body: issue 337 part 2, the crawl's own get-up plays to its end, then the child is laid down") {
    Guest g;
    Crawl c;
    ChildDown(c, g, true, addr::kDoActionCrawl);
    std::uint64_t t = 1000;
    c.Skip(kBsa, Req(verb::kWait), false);
    RunFrame(c, g, t += 16);
    c.Skip(kBsa, Req(verb::kWait), false);
    CHECK(Verbs(RunFrame(c, g, t += 16)) == std::vector<std::int32_t>{verb::kCrawl});
    SetAs(g, "CrouchLockOnMove", 1.0f);  // lies down
    CHECK(RunFrame(c, g, t += 16).empty());
    CHECK(c.GameCrawl());
    // Room to stand up: the game gets him up. While that clip runs nothing is played.
    SetAs(g, "CrouchLockOnMoveEnd", 1.0f);
    for (int i = 0; i < 20; ++i) {
        SetFrame(g, static_cast<float>(i));
        CHECK(RunFrame(c, g, t += 16).empty());
    }
    CHECK(c.GameCrawl());
    CHECK_FALSE(c.LaidDown());
    // The clip is over (its frame stands still): the game's lying wait is played, the slot stays ours.
    std::vector<Made> plays;
    for (int i = 0; i < Crawl::kEndStallReads + 1 && plays.empty(); ++i) {
        plays = Plays(RunFrame(c, g, t += 16));
    }
    REQUIRE(plays.size() == 1);
    CHECK(plays[0].as == "Crouch");
    CHECK(plays[0].rate == 1.0f);
    CHECK(c.LaidDown());
    CHECK(c.Totals().crawl_ended == 1);
    // 0.12.1 downed-move: the laid-down child's Wait is the game's ground move now (v0.12.0 skipped it: he lay still).
    const auto moves = c.Totals().ground_moves;
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait), false));
    CHECK(c.Totals().ground_moves == moves + 1);
    CHECK(RunFrame(c, g, t += 16).empty());
    CHECK_FALSE(c.GameCrawl());
    // In an event nothing is played (the event drives him).
    c.Want(Want(true, true, false, kActor));
    SetAs(g, "Demo", 1.0f);
    CHECK(Plays(RunFrame(c, g, t += 16)).empty());
    SetAnchor(std::nullopt);
}

TEST_CASE("dbno body: issue 337 part 2, a taken Crawl that never crawls kMaxFalseStarts times: the child is laid down") {
    Guest g;
    Crawl c;
    ChildDown(c, g, true, addr::kDoActionCrawl);
    std::uint64_t t = 1000;
    int asked = 0;
    for (int i = 0; i < 40 * Crawl::kCrawlStartReads; ++i) {
        c.Skip(kBsa, Req(verb::kWait), false);
        asked += static_cast<int>(Verbs(RunFrame(c, g, t += 16)).size());
    }
    CHECK(asked == Crawl::kMaxFalseStarts + 1);  // (the last one holds the slot for the lying wait)
    CHECK(c.LaidDown());
    CHECK(c.Totals().crawl_ended == 0);
    CHECK(c.Totals().lie_downs == 1);
    CHECK(ReadSlot0(g, kActor)->name == "Crouch");
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWalk), false));  // 0.12.1 downed-move: he moves lying
    CHECK(c.Skip(kBsa, Req(134), false));
    SetAnchor(std::nullopt);
}

TEST_CASE("dbno body (0.12.1 downed-move): the game's crawl asks for room to stand up: no room for our downed child only") {
    // v0.12.0: in open ground the crawl's state 4 found room to stand at once (main+0x301ac50), got him up, and he
    // was laid down still. NoRoom answers that check (patch 0046's wrap) so the game's crawl lasts the down.
    Guest g;
    Crawl c;
    ChildDown(c, g, true, addr::kDoActionCrawl);
    CHECK_FALSE(c.NoRoom(kActor));  // no Crawl taken yet: the game's own check
    std::uint64_t t = 1000;
    c.Skip(kBsa, Req(verb::kWait), false);
    RunFrame(c, g, t += 16);
    c.Skip(kBsa, Req(verb::kWait), false);
    REQUIRE(Verbs(RunFrame(c, g, t += 16)) == std::vector<std::int32_t>{verb::kCrawl});
    SetAs(g, "CrouchLockOnMove", 1.0f);
    CHECK(RunFrame(c, g, t += 16).empty());
    REQUIRE(c.GameCrawl());
    c.TakeNotes();
    CHECK(c.NoRoom(kActor));
    CHECK(c.NoRoom(kActor));
    CHECK_FALSE(c.NoRoom(kActor2));  // another Link (a puppet, a reload's new actor)
    CHECK_FALSE(c.NoRoom(0));
    CHECK(c.Totals().kept_crawling == 2);
    int lines = 0;
    for (const auto& n : c.TakeNotes()) {
        lines += n.find("answered no room") != std::string::npos ? 1 : 0;
    }
    CHECK(lines == 1);  // once a down
    // The crawl goes on: never laid down, still the game's crawl, the full stick.
    for (int i = 0; i < 3 * Crawl::kCrawlStartReads; ++i) {
        CHECK(c.Skip(kBsa, Req(verb::kWait), false));  // the game's own ground move would end the crawl
        CHECK(RunFrame(c, g, t += 16).empty());
    }
    CHECK(c.GameCrawl());
    CHECK_FALSE(c.LaidDown());
    // In an event the event drives him: the game's own check.
    c.Want(Want(true, true, false, kActor));
    CHECK_FALSE(c.NoRoom(kActor));
    c.Want(Want(true, false, false, kActor));
    // Up again (revived, bled out, rules off): the game's own check, so its crawl gets him up.
    c.Want(Want(false, false, false, kActor));
    CHECK_FALSE(c.NoRoom(kActor));
    CHECK(c.Totals().kept_crawling == 2);
    SetAnchor(std::nullopt);
}

TEST_CASE("dbno body (0.12.1 downed-move): a downed adult held lying moves with the game's ground move, and is never kept crawling") {
    Guest g;
    MakeAs(g, "Wait", 1.0f);
    Crawl c;
    c.Want(Want(false, false, true, kActor));
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    c.Want(Want(true, false, true, kActor));
    RunFrame(c, g, 984);
    CHECK(c.Skip(kBsa, Req(verb::kWait, 2), false));  // a crawl round: cleared so our Crawl gets the slot
    REQUIRE(Verbs(RunFrame(c, g, 1000)) == std::vector<std::int32_t>{verb::kCrawl});
    SetAs(g, "Crouch", 1.0f);
    REQUIRE(Plays(RunFrame(c, g, 1100)).size() == 1);  // DamageLStand held
    c.TakeNotes();
    // v0.12.0 skipped these too: nothing read the stick and he lay still.
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWalk, 2), false));
    CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait, 2), false));
    CHECK(c.Totals().ground_moves == 2);
    int lines = 0;
    for (const auto& n : c.TakeNotes()) {
        lines += n.find("he moves lying") != std::string::npos ? 1 : 0;
    }
    CHECK(lines == 1);
    // Everything else stays skipped: a hit's reaction, the game's own stand-up.
    CHECK(c.Skip(kBsa, Req(134, 2), false));
    CHECK(c.Skip(kBsa, Req(verb::kLDamageStandUp, 2), false));
    CHECK(c.Skip(kBsa, Req(134, 1), false));  // another slot's too, as before (the crawl skips every verb of our BSA)
    // The walk's own command on slot 0: the pose is played again, held.
    SetAs(g, "Walk", 1.0f);
    const auto f = RunFrame(c, g, 1200);
    REQUIRE(Plays(f).size() == 1);
    CHECK(Plays(f)[0].as == "DamageLStand");
    CHECK(Plays(f)[0].rate == 0.0f);
    CHECK(Verbs(f).empty());  // no Crawl asked again
    CHECK_FALSE(c.NoRoom(kActor));  // the adult has no game crawl to keep
    CHECK_FALSE(c.GameCrawl());     // the pad's slow stick
}

TEST_CASE("dbno body: issue 337 part 2, no offer (or no live anchor) in kOfferWaitReads frames: the child is laid down") {
    for (const bool live : {false, true}) {
        Guest g;
        Crawl c;
        ChildDown(c, g, live, 0);
        std::uint64_t t = 1000;
        for (int i = 0; i < Crawl::kOfferWaitReads - 1; ++i) {
            CHECK_FALSE(c.Skip(kBsa, Req(verb::kWait), false));  // the game's own verbs run while waiting
            const auto f = RunFrame(c, g, t += 16);
            CHECK(Verbs(f).empty());
            CHECK(Plays(f).empty());
        }
        CHECK_FALSE(c.LaidDown());
        int asked = 0;
        for (int i = 0; i < 30; ++i) {
            c.Skip(kBsa, Req(verb::kWait), false);
            asked += static_cast<int>(Verbs(RunFrame(c, g, t += 16)).size());
        }
        CHECK(asked == 1);
        CHECK(c.LaidDown());
        CHECK(ReadSlot0(g, kActor)->name == "Crouch");
        CHECK(c.Totals().child_poses == 1);
        SetAnchor(std::nullopt);
    }
}

TEST_CASE("dbno body: issue 337 part 2, an adult is never played the child's lying wait") {
    Guest g;
    MakeAs(g, "Wait", 1.0f);
    Crawl c;
    c.Want(Want(false, false, true, kActor));
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    c.Want(Want(true, false, true, kActor));
    RunFrame(c, g, 984);
    c.Skip(kBsa, Req(verb::kWait, 2), false);
    CHECK(Verbs(RunFrame(c, g, 1000)) == std::vector<std::int32_t>{verb::kCrawl});
    std::uint64_t t = 1000;
    for (const char* name : {"CrouchLockOnMove", "CrouchLockOnMoveEnd", "Wait", "Crouch"}) {
        SetAs(g, name, 1.0f);
        for (int i = 0; i < 5; ++i) {
            for (const Made& m : Plays(RunFrame(c, g, t += 16))) {
                CHECK(m.as == "DamageLStand");
            }
        }
        CHECK(ReadSlot0(g, kActor)->name == "DamageLStand");
    }
    CHECK_FALSE(c.LaidDown());
    CHECK(c.Totals().child_poses == 0);
}
