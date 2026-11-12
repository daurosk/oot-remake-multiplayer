// Tests for the Deku Scrub destroy flags held during our Link's event (lichii/hook/coop_scrub_talk.h, issue 144).
#include <doctest/doctest.h>

#include <map>

#include "coop/host/flags.hpp"
#include "coop_dungeon.h"
#include "coop_scrub_talk.h"

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
