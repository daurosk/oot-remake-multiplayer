// Tests for the Deku Scrub destroy flags held during our Link's event (lichii/hook/coop_scrub_talk.h, issue 144).
#include <doctest/doctest.h>

#include <cmath>
#include <map>

#include "coop/host/flags.hpp"
#include "coop_dungeon.h"
#include "coop_scrub_talk.h"
#include "coop_scrubs.h"

namespace {

namespace proto = coop::protocol;
using coop::host::FlagKey;

constexpr std::uint32_t kDestroy001 = 0x1c594551;
constexpr std::uint32_t kRoomBCleared = 0x64edd2f3;
constexpr std::uint32_t kOther = 0x4b640f7e;  // DodongoCavern_Room02_EventFinish

class Store : public coop::host::FlagStore {
public:
    std::map<std::uint32_t, std::uint32_t> values;
    std::optional<proto::FlagEntry> read(FlagKey key) override {
        const auto it = values.find(key.hash);
        if (it == values.end()) return std::nullopt;
        return proto::FlagEntry{key, proto::FlagKind::boolean, it->second};
    }
    bool write(const proto::FlagEntry& e) override {
        values[e.key.hash] = e.value;
        return true;
    }
};

proto::FlagEntry Set(std::uint32_t hash) {
    return {FlagKey(hash, 0), proto::FlagKind::boolean, 1};
}

} // namespace

TEST_CASE("scrub talk: the held flags are the shared Deku Tree scrub flags, by name") {
    for (const auto& f : CoopScrubTalk::kTalkFlags) {
        CHECK(proto::flag_hash(f.name) == f.hash);
        bool shared = false;
        for (const auto& d : CoopDungeon::Flags()) {
            if (d.hash == f.hash) {
                shared = true;
                CHECK(std::string_view(d.name) == f.name);
            }
        }
        CHECK(shared);
        CHECK(CoopScrubTalk::IsTalkFlag(f.hash));
    }
    CHECK_FALSE(CoopScrubTalk::IsTalkFlag(kOther));
}

TEST_CASE("scrub talk: the hold runs through our event and a tail after it") {
    CoopScrubTalk::Hold h;
    CHECK_FALSE(h.Frame(false, 1'000'000));  // never in an event: no hold
    CHECK_FALSE(h.Holding());
    CHECK(h.Frame(true, 2'000'000));  // the talk starts
    CHECK(h.Holds(kDestroy001));
    CHECK(h.Holds(kRoomBCleared));
    CHECK_FALSE(h.Holds(kOther));
    CHECK_FALSE(h.Frame(true, 9'000'000));  // a long talk
    CHECK_FALSE(h.Frame(false, 9'500'000));  // ended: the tail
    CHECK(h.Holding());
    CHECK_FALSE(h.Frame(true, 10'000'000));  // the run away, after one frame without the flag
    CHECK_FALSE(h.Frame(false, 11'000'000));
    CHECK(h.Frame(false, 12'000'000));  // 2 s after the last event frame: released
    CHECK_FALSE(h.Holding());
    CHECK_FALSE(h.Holds(kDestroy001));
}

TEST_CASE("scrub talk: another player's destroy flag waits in the HoldFlagStore until our talk ends") {
    Store game;
    game.values[kDestroy001] = 0;
    game.values[kOther] = 0;
    CoopScrubTalk::Hold h;
    h.Frame(true, 1'000'000);  // our Link is in the scrub's talk
    {
        const bool talk = h.Holding();
        coop::host::HoldFlagStore hold(game, [talk](FlagKey k) { return talk && CoopScrubTalk::IsTalkFlag(k.hash); });
        CHECK_FALSE(hold.write(Set(kDestroy001)));  // the scrub stays: the room graph does not delete it
        CHECK(hold.holds(FlagKey(kDestroy001, 0)));
        CHECK(hold.write(Set(kOther)));             // other flags go at once
        CHECK(hold.held() == 1);
    }
    CHECK(game.values[kDestroy001] == 0);
    CHECK(game.values[kOther] == 1);
    h.Frame(false, 1'500'000);
    h.Frame(false, 3'100'000);  // the tail is over
    {
        const bool talk = h.Holding();
        coop::host::HoldFlagStore hold(game, [talk](FlagKey k) { return talk && CoopScrubTalk::IsTalkFlag(k.hash); });
        CHECK(hold.write(Set(kDestroy001)));  // the watcher's retry lands: both games agree
    }
    CHECK(game.values[kDestroy001] == 1);
}

TEST_CASE("scrub talk: the event hold applies only in the Deku Tree, the scrubs' scene (row 275)") {
    CoopScrubTalk::Hold h;
    h.Frame(true, 1'000'000);  // an event of our Link's
    REQUIRE(h.Holds(kDestroy001));
    CHECK(CoopScrubTalk::HoldsIn(h, std::string_view("InsideTheDekuTree"), kDestroy001));
    CHECK(CoopScrubTalk::HoldsIn(h, std::string_view("InsideTheDekuTree"), kRoomBCleared));
    CHECK_FALSE(CoopScrubTalk::HoldsIn(h, std::string_view("InsideTheDekuTree"), kOther));
    // Geno's kit: both games in Twinrova's room, its intro running: the host's save values go through at once.
    CHECK_FALSE(CoopScrubTalk::HoldsIn(h, std::string_view("TwinrovaStage"), kDestroy001));
    CHECK_FALSE(CoopScrubTalk::HoldsIn(h, std::nullopt, kRoomBCleared));  // no scene known: none of them loaded
    CoopScrubTalk::Hold idle;
    CHECK_FALSE(CoopScrubTalk::HoldsIn(idle, std::string_view("InsideTheDekuTree"), kDestroy001));
}

// Issue 144, second guard: our copy of the scrub near our Link (CoopScrubTalk::Near).

TEST_CASE("scrub talk near: every held flag has its scrubs, and the brothers are the Deku Tree's") {
    for (const auto& f : CoopScrubTalk::kTalkFlags) {
        const auto g = CoopScrubTalk::GroupOf(f.hash);
        REQUIRE(g.has_value());
        int scrubs = 0;
        for (const auto& s : CoopScrubTalk::kNearScrubs) {
            scrubs += s.group == *g ? 1 : 0;
        }
        CHECK(scrubs >= 1);
    }
    CHECK_FALSE(CoopScrubTalk::GroupOf(kOther).has_value());
    // The three brothers here are coop_scrubs.cpp's Deku Tree brothers (one manager).
    std::uint64_t manager = 0;
    int brothers = 0;
    for (const auto& s : CoopScrubTalk::kNearScrubs) {
        const auto* b = CoopScrubs::FindBrother(s.hash);
        if (s.group == CoopScrubTalk::Group::brothers) {
            REQUIRE(b != nullptr);
            if (manager == 0) manager = b->manager;
            CHECK(b->manager == manager);
            ++brothers;
        } else {
            CHECK(b == nullptr);  // the lone scrubs are no brothers
        }
    }
    CHECK(brothers == 3);
    CHECK(CoopScrubTalk::FindNearScrub(0x1234) == nullptr);
}

TEST_CASE("scrub talk near: a scrub within talking distance holds its own group only, and lets go 1.5 s after the last walk") {
    CoopScrubTalk::Near n;
    const std::uint64_t room_b = 0xaa6f22763e99e851ULL;
    const std::uint64_t one = 0x9b04ae25f50c7fcaULL;
    CHECK(n.Mask(1'000'000) == 0);
    n.See(room_b, 8.1f, 1'000'000);  // too far: nothing
    CHECK(n.Mask(1'000'000) == 0);
    n.See(room_b, 10.5f, 1'000'000);  // aiming from the room's side: nothing
    CHECK(n.Mask(1'000'000) == 0);
    n.See(room_b, std::nanf(""), 1'000'000);  // a bad read: nothing
    CHECK(n.Mask(1'000'000) == 0);
    n.See(0x1234, 1.0f, 1'000'000);  // not a Deku Tree scrub
    CHECK(n.Mask(1'000'000) == 0);
    n.See(room_b, 2.0f, 1'000'000);  // our Link walks up to it (its talk)
    const std::uint8_t m = n.Mask(1'200'000);
    CHECK(CoopScrubTalk::NearHolds(m, kDestroy001));
    CHECK(CoopScrubTalk::NearHolds(m, kRoomBCleared));
    CHECK_FALSE(CoopScrubTalk::NearHolds(m, 0x1d493033));  // room F's _002
    CHECK_FALSE(CoopScrubTalk::NearHolds(m, 0x60c22ed7));  // the brothers' _003
    CHECK_FALSE(CoopScrubTalk::NearHolds(m, kOther));
    CHECK(n.Mask(2'499'999) != 0);
    CHECK(n.Mask(2'500'000) == 0);  // no walk saw it near since: released
    n.See(one, 5.29f, 3'000'000);  // where B talked to One in run 20261116-002801
    CHECK(CoopScrubTalk::NearHolds(n.Mask(3'000'000), 0xd2f46f66));  // RoomK_Cleared
    n.Reset();
    CHECK(n.Mask(3'000'000) == 0);
}

TEST_CASE("scrub talk near: a scrub bound into our event holds its group at any distance") {
    CoopScrubTalk::Near n;
    const std::uint64_t room_b = 0xaa6f22763e99e851ULL;
    n.See(room_b, 15.0f, 1'000'000, false);         // far and not in an event
    n.See(room_b, 15.0f, 1'000'000, std::nullopt);  // far, performer not read
    CHECK(n.Mask(1'000'000) == 0);
    n.See(room_b, 15.0f, 1'000'000, true);  // a deflected nut started Talk_Deku_01_Attacked from afar
    CHECK(CoopScrubTalk::NearHolds(n.Mask(1'000'000), kDestroy001));
    CHECK_FALSE(CoopScrubTalk::NearHolds(n.Mask(1'000'000), 0x1d493033));  // room F's
    n.See(0x1234, 1.0f, 1'000'000, true);  // not one of these scrubs: nothing more
    CHECK(n.Mask(2'600'000) == 0);
}

TEST_CASE("scrub talk near: another player's flag waits while the scrub is near, even with no event flag") {
    Store game;
    game.values[kDestroy001] = 0;
    CoopScrubTalk::Hold talk;
    talk.Frame(false, 1'000'000);  // the game's event flag never comes on (unseen in a game)
    CoopScrubTalk::Near n;
    n.See(0xaa6f22763e99e851ULL, 1.5f, 1'000'000);
    auto pred = [&talk](std::uint8_t near) {
        return [talk, near](FlagKey k) { return talk.Holds(k.hash) || CoopScrubTalk::NearHolds(near, k.hash); };
    };
    {
        coop::host::HoldFlagStore hold(game, pred(n.Mask(1'100'000)));
        CHECK_FALSE(hold.write(Set(kDestroy001)));  // the scrub stays for our talk
    }
    CHECK(game.values[kDestroy001] == 0);
    {
        coop::host::HoldFlagStore hold(game, pred(n.Mask(3'000'000)));  // we walked away
        CHECK(hold.write(Set(kDestroy001)));
    }
    CHECK(game.values[kDestroy001] == 1);
}
