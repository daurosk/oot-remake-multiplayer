// Tests for the randomizer's one-time shop shelves shared between games (lichii/hook/coop_shop_sale.h).
#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <string>

#include "coop/host/flags.hpp"
#include "coop/host/game_items.hpp"
#include "coop_dungeon.h"
#include "coop_scrub_sale.h"
#include "coop_shop_sale.h"
#include "coop_story.h"

namespace {

namespace proto = coop::protocol;
using coop::host::FlagKey;

// OOTRR 0.7.3's own check (shelf/counters.rs): the Goron Shop's child slot 3 counter.
constexpr std::uint32_t kGoronChild3 = 0xe783dc08;
constexpr std::uint32_t kOther = 0x4b640f7e;  // DodongoCavern_Room02_EventFinish

class Ints : public coop::host::FlagStore {
public:
    std::map<std::uint32_t, std::int32_t> values;
    int writes = 0, exact = 0;
    std::optional<proto::FlagEntry> read(FlagKey key) override {
        const auto it = values.find(key.hash);
        if (it == values.end()) return std::nullopt;
        return proto::FlagEntry{key, proto::FlagKind::int32, static_cast<std::uint32_t>(it->second)};
    }
    bool write(const proto::FlagEntry& e) override {
        ++writes;
        values[e.key.hash] = static_cast<std::int32_t>(e.value);
        return true;
    }
    bool write_exact(const proto::FlagEntry& e) override {
        ++exact;
        values[e.key.hash] = static_cast<std::int32_t>(e.value);
        return true;
    }
};

proto::FlagEntry Count(std::uint32_t hash, std::int32_t v) {
    return {FlagKey(hash, 0), proto::FlagKind::int32, static_cast<std::uint32_t>(v)};
}

} // namespace

TEST_CASE("shop sale: the 96 sold counters, by OOTRR's names, and no other rule shares them") {
    CHECK(CoopShopSale::Counters().size() == 96);
    CHECK(proto::flag_hash(CoopShopSale::CounterName("Npc_Goron_011_Shop_Child", 3)) == kGoronChild3);
    CHECK(CoopShopSale::IsCounter(kGoronChild3));
    CHECK(CoopShopSale::IsCounter(proto::flag_hash("Shop_SoldNum.Npc_Kokiri_001_Shop_Child_ShortStock.7")));
    CHECK_FALSE(CoopShopSale::IsCounter(proto::flag_hash("Shop_SoldNum.Npc_Kokiri_001_Shop_Child.8")));
    CHECK_FALSE(CoopShopSale::IsCounter(kOther));
    const auto items = coop::host::item_shared_flags();
    for (const std::uint32_t h : CoopShopSale::Counters()) {
        CHECK_FALSE(items.contains(FlagKey(h, std::uint16_t{0})));  // one rule per flag
        CHECK_FALSE(CoopScrubSale::IsSaleFlag(h));
        CHECK(CoopStory::Find(h) == nullptr);
        for (const auto& d : CoopDungeon::Flags()) CHECK(d.hash != h);
    }
}

TEST_CASE("shop sale: merged into the shared list once") {
    std::optional<coop::host::SharedFlags> shared;
    CHECK(CoopShopSale::Merge(shared) == 96);
    CHECK(shared->contains(FlagKey(kGoronChild3, std::uint16_t{0})));
    CHECK(CoopShopSale::Merge(shared) == 0);
}

TEST_CASE("shop sale: a counter is kept at the highest value, all else passes") {
    Ints game;
    CoopShopSale::Store store(game);
    const FlagKey key(kGoronChild3, std::uint16_t{0});
    CHECK(store.is_max(key));
    CHECK_FALSE(store.is_max(FlagKey(kOther, std::uint16_t{0})));
    CHECK_FALSE(store.is_count(key));

    // Another player sold the slot out: our game takes it.
    game.values[kGoronChild3] = 0;
    CHECK(store.write(Count(kGoronChild3, 1)));
    CHECK(game.values[kGoronChild3] == 1);
    CHECK(game.writes == 1);
    // A lower or equal value (a daily reset in their game) is not written over ours.
    CHECK(store.write(Count(kGoronChild3, 0)));
    CHECK(store.write(Count(kGoronChild3, 1)));
    CHECK(game.values[kGoronChild3] == 1);
    CHECK(game.writes == 1);
    // The host's save load decides (exact).
    CHECK(store.write_exact(Count(kGoronChild3, 0)));
    CHECK(game.values[kGoronChild3] == 0);
    CHECK(game.exact == 1);
    // Not a counter: written as it comes.
    game.values[kOther] = 5;
    CHECK(store.write(Count(kOther, 2)));
    CHECK(game.values[kOther] == 2);
}

TEST_CASE("shop sale: another player's counter waits only while our Link is in an event") {
    CHECK(CoopShopSale::Waits(kGoronChild3, true));
    CHECK_FALSE(CoopShopSale::Waits(kGoronChild3, false));
    CHECK_FALSE(CoopShopSale::Waits(kOther, true));
}

TEST_CASE("shop sale: off until the randomizer says its seed has shops") {
    CHECK_FALSE(CoopShopSale::SeedHasShops());
    CoopShopSale::SetSeedHasShops(true);
    CHECK(CoopShopSale::SeedHasShops());
    CoopShopSale::SetSeedHasShops(false);
    CHECK_FALSE(CoopShopSale::SeedHasShops());
}

TEST_CASE("fairy gifts: each flag's name hashes to its listed hash, and no other list shares it") {
    CHECK(CoopShopSale::kFairyGifts.size() == 6);
    const auto items = coop::host::item_shared_flags();
    for (const auto& f : CoopShopSale::kFairyGifts) {
        CHECK(proto::flag_hash(std::string(f.name)) == f.hash);
        CHECK(CoopShopSale::FindGift(f.hash) == &f);
        CHECK_FALSE(CoopShopSale::IsCounter(f.hash));
        CHECK_FALSE(CoopScrubSale::IsSaleFlag(f.hash));
        CHECK_FALSE(items.contains(FlagKey(f.hash, std::uint16_t{0})));
        CHECK(CoopStory::Find(f.hash) == nullptr);  // one rule per flag: the pickup rule
        for (const auto& d : CoopDungeon::Flags()) CHECK(d.hash != f.hash);
    }
    CHECK_FALSE(CoopShopSale::IsGift(kOther));
    std::optional<coop::host::SharedFlags> shared;
    CHECK(CoopShopSale::MergeGifts(shared) == 6);
    CHECK(CoopShopSale::MergeGifts(shared) == 0);
}

TEST_CASE("fairy gifts: another player's gift waits only while our Link is in an event") {
    CHECK(CoopShopSale::GiftWaits(0xbc9cec9b, true));
    CHECK_FALSE(CoopShopSale::GiftWaits(0xbc9cec9b, false));
    CHECK_FALSE(CoopShopSale::GiftWaits(kOther, true));
}
