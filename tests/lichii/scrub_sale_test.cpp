// Tests for the Business Scrubs' one-time sales shared between games (lichii/hook/coop_scrub_sale.h).
#include <doctest/doctest.h>

#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "coop/host/flags.hpp"
#include "coop_dungeon.h"
#include "coop_scrub_sale.h"
#include "coop_story.h"

namespace {

namespace proto = coop::protocol;
using coop::host::FlagKey;

constexpr std::uint32_t kNutBag = 0x5ef24799;     // Lostwood_Enemy_Scrub_Business_DekuNutsPouch_01
constexpr std::uint32_t kStickBag = 0x8c98cf5a;   // LostWood_Enemy_Scrub_Business_DekuStickPouch_01
constexpr std::uint32_t kFieldHeart = 0xe41e1597; // HyruleField_Enemy_Scrub_Business_HeartPiece_01
constexpr std::uint32_t kRandoRear = 0x1ef2f533;  // HavePlayedEvent.Enemy_Scrub_Business_DekuStick_01
constexpr std::uint32_t kOther = 0x4b640f7e;      // DodongoCavern_Room02_EventFinish

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

CoopScrubSale::Here At(std::string_view scene, float x, float y, float z) {
    CoopScrubSale::Here h;
    h.known = true;
    h.scene = scene;
    h.x = x;
    h.y = y;
    h.z = z;
    return h;
}

} // namespace

TEST_CASE("scrub sale: each flag's name hashes to its listed hash, and no other list shares it") {
    CHECK(CoopScrubSale::kSaleFlags.size() == 6);
    for (const auto& f : CoopScrubSale::kSaleFlags) {
        CHECK(proto::flag_hash(std::string(f.name)) == f.hash);
        CHECK(CoopScrubSale::IsSaleFlag(f.hash));
        CHECK(CoopScrubSale::Find(f.hash) == &f);
        CHECK(CoopStory::Find(f.hash) == nullptr);  // one rule per flag: the pickup rule, not the story's
        for (const auto& d : CoopDungeon::Flags()) {
            CHECK(d.hash != f.hash);
        }
    }
    CHECK_FALSE(CoopScrubSale::IsSaleFlag(kOther));
    CHECK(CoopScrubSale::Find(kOther) == nullptr);
}

TEST_CASE("scrub sale: every flag has a scrub that sells it, and every scrub a listed flag") {
    for (const auto& f : CoopScrubSale::kSaleFlags) {
        bool sold = false;
        for (const auto& s : CoopScrubSale::kSellers) {
            sold = sold || s.flag == f.hash;
        }
        CHECK_MESSAGE(sold, f.name);
    }
    for (const auto& s : CoopScrubSale::kSellers) {
        CHECK(CoopScrubSale::IsSaleFlag(s.flag));
        CHECK(CoopScrubSale::Near(s.flag, s.scene, s.x, s.y, s.z));  // standing on it
    }
}

TEST_CASE("scrub sale: another player's buy waits only while we may be talking to that scrub") {
    // Not a sale flag: never held here, wherever we are.
    CoopScrubSale::Here talking = At("LostWood_Child", 7.3f, 82.9f, -85.6f);
    talking.in_event = true;
    CHECK_FALSE(CoopScrubSale::Waits(kOther, talking));

    // In an event (our talk, the scrub's menu): every sale flag waits.
    CoopScrubSale::Here event;
    event.in_event = true;
    CHECK(CoopScrubSale::Waits(kNutBag, event));
    CHECK(CoopScrubSale::Waits(kFieldHeart, event));

    // Next to the grotto's nut scrub (child or adult), not in an event: it waits.
    CHECK(CoopScrubSale::Waits(kNutBag, At("LostWood_Child", 8.0f, 82.9f, -84.0f)));
    CHECK(CoopScrubSale::Waits(kNutBag, At("LostWood_Adult", 8.0f, 82.9f, -84.0f)));
    // The randomizer's rear scrub stands 4 m away in the same grotto.
    CHECK(CoopScrubSale::Waits(kRandoRear, At("LostWood_Child", 8.0f, 82.9f, -84.0f)));
    // The stick bag scrub stands in two places: both count.
    CHECK(CoopScrubSale::Waits(kStickBag, At("KokiriForest_Child", -127.0f, 114.6f, -16.0f)));
    CHECK(CoopScrubSale::Waits(kStickBag, At("LostWood_Adult", -45.0f, 95.2f, 72.0f)));

    // Elsewhere in the same scene, in another scene at the same spot, or no reading: it lands.
    CHECK_FALSE(CoopScrubSale::Waits(kNutBag, At("LostWood_Child", 20.0f, 99.9f, -51.2f)));
    CHECK_FALSE(CoopScrubSale::Waits(kNutBag, At("HyruleField_Child", 7.3f, 82.9f, -85.6f)));
    CHECK_FALSE(CoopScrubSale::Waits(kFieldHeart, At("LostWood_Child", 7.3f, 82.9f, -85.6f)));
    CHECK_FALSE(CoopScrubSale::Waits(kNutBag, CoopScrubSale::Here{}));
    // The scrub's reach (Alert 10 m) is held, with its 1 m margin; past kNearM it lands.
    CHECK(CoopScrubSale::kNearM == doctest::Approx(11.0f));
    CHECK(CoopScrubSale::Waits(kFieldHeart, At("HyruleField_Child", 25.0f + 9.5f, 129.9f, 22.1f)));
    CHECK(CoopScrubSale::Waits(kFieldHeart, At("HyruleField_Child", 25.0f + 10.5f, 129.9f, 22.1f)));
    CHECK(CoopScrubSale::Waits(kFieldHeart, At("HyruleField_Child", 25.0f + 10.9f, 129.9f, 22.1f)));
    CHECK_FALSE(CoopScrubSale::Waits(kFieldHeart, At("HyruleField_Child", 25.0f + 11.1f, 129.9f, 22.1f)));
}

TEST_CASE("scrub sale: a scrub bound into our event holds its flag at any distance (Busy, from the walk)") {
    constexpr std::uint64_t kDmcSeller = 0x22f23970504a8bcfULL;   // DeathMountainCrater_Child_Static
    constexpr std::uint64_t kJabuSeller = 0x83c9e5108c604316ULL;  // OOTRR's Jabu-Jabu seller
    constexpr std::uint32_t kDmcHeart = 0x6b8fc751;
    constexpr std::uint32_t kJabu = 0x25359a9e;
    CHECK(CoopScrubSale::FindSeller(kDmcSeller)->flag == kDmcHeart);
    CHECK(CoopScrubSale::FindSeller(0x1234) == nullptr);
    CoopScrubSale::Busy b;
    CHECK(b.Mask(1'000'000) == 0);
    b.See(kDmcSeller, 30.0f, 1'000'000, false);         // far, not in an event: nothing
    b.See(kDmcSeller, 30.0f, 1'000'000, std::nullopt);  // far, performer not read: nothing
    b.See(0x1234, 1.0f, 1'000'000, true);                // not a seller
    CHECK(b.Mask(1'000'000) == 0);
    b.See(kDmcSeller, 30.0f, 1'000'000, true);  // a deflected nut started its attacked event from afar
    CoopScrubSale::Here far = At("LostWood_Child", 0, 0, 0);  // not even near: only Busy says so
    far.busy = b.Mask(1'200'000);
    CHECK(CoopScrubSale::Waits(kDmcHeart, far));
    CHECK_FALSE(CoopScrubSale::Waits(kJabu, far));  // only that flag
    CHECK_FALSE(CoopScrubSale::Waits(kNutBag, far));
    b.See(kJabuSeller, std::numeric_limits<float>::infinity(), 1'000'000, true);  // no player read: the event counts
    CHECK((b.Mask(1'000'000) & (1u << CoopScrubSale::IndexOf(kJabu))) != 0);
    b.See(kJabuSeller, 10.5f, 2'000'000, false);  // within the scrub's reach
    CHECK((b.Mask(2'000'000) & (1u << CoopScrubSale::IndexOf(kJabu))) != 0);
    CHECK(b.Mask(3'500'000) == 0);  // no walk saw either since: released
    b.See(kDmcSeller, 5.0f, 4'000'000, std::nullopt);
    b.Reset();
    CHECK(b.Mask(4'000'000) == 0);
}

TEST_CASE("scrub sale: event_running alone also holds every sale flag") {
    CoopScrubSale::Here h;  // no reading, no walk
    h.in_event = true;
    for (const auto& f : CoopScrubSale::kSaleFlags) {
        CHECK(CoopScrubSale::Waits(f.hash, h));
    }
    CHECK_FALSE(CoopScrubSale::Waits(kOther, h));
}

TEST_CASE("scrub sale: in the HoldFlagStore the buy lands once our Link walks away") {
    Store game;
    game.values[kNutBag] = 0;
    game.values[kOther] = 0;
    const auto near = At("LostWood_Child", 7.0f, 82.9f, -86.0f);
    {
        coop::host::HoldFlagStore hold(game, [near](FlagKey k) { return CoopScrubSale::Waits(k.hash, near); });
        CHECK_FALSE(hold.write(Set(kNutBag)));  // our scrub stays while we may be in its menu
        CHECK(hold.write(Set(kOther)));
        CHECK(hold.held() == 1);
    }
    CHECK(game.values[kNutBag] == 0);
    const auto away = At("LostWood_Child", 30.0f, 99.0f, -40.0f);
    {
        coop::host::HoldFlagStore hold(game, [away](FlagKey k) { return CoopScrubSale::Waits(k.hash, away); });
        CHECK(hold.write(Set(kNutBag)));  // the watcher's retry: sold in both games
    }
    CHECK(game.values[kNutBag] == 1);
}

TEST_CASE("scrub sale: the flags join the shared list once") {
    std::optional<coop::host::SharedFlags> shared;
    CHECK(CoopScrubSale::Merge(shared) == 6);
    REQUIRE(shared.has_value());
    CHECK(shared->contains(FlagKey(kNutBag, 0)));
    CHECK(shared->contains(FlagKey(kRandoRear, 0)));
    CHECK(CoopScrubSale::Merge(shared) == 0);
    CHECK(shared->size() == 6);
}
