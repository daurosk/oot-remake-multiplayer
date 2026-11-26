// The test and cheat gift of an item flag (lichii/hook/coop_cheat_give.h): what a play-test `give` writes. No
// emulator code.
#include <doctest/doctest.h>

#include <map>
#include <set>
#include <string>

#include "coop/host/game_items.hpp"
#include "coop_cheat_give.h"

namespace {

using coop::host::FlagKey;
using coop::protocol::FlagEntry;
using coop::protocol::FlagKind;

// A game's flags: only a flag the save has can be read, and a write needs the flag to be there with its kind.
class FakeFlags final : public coop::host::FlagStore {
public:
    std::map<std::uint32_t, FlagEntry> flags;
    std::set<std::uint32_t> refuses;  // writes that fail

    void has(const char* name, FlagKind kind, std::int32_t value) {
        const auto hash = coop::host::flag_hash(name);
        flags[hash] = FlagEntry(FlagKey(hash, 0), kind, static_cast<std::uint32_t>(value));
    }
    std::int32_t at(const char* name) const {
        return static_cast<std::int32_t>(flags.at(coop::host::flag_hash(name)).value);
    }
    std::optional<FlagEntry> read(FlagKey key) override {
        const auto it = flags.find(key.hash);
        if (it == flags.end()) return std::nullopt;
        return it->second;
    }
    bool write(const FlagEntry& entry) override {
        const auto it = flags.find(entry.key.hash);
        if (it == flags.end() || it->second.kind != entry.kind || refuses.count(entry.key.hash)) return false;
        it->second.value = entry.value;
        return true;
    }
};

constexpr const char* kBow = "PouchData.Weapon_Range_Bow.StockNum";
constexpr const char* kBowGet = "IsGet.Weapon_Range_Bow";
constexpr const char* kBag = "PouchData.Item_BombBag_Small.StockNum";
constexpr const char* kBombs = "PouchData.Item_Bomb.StockNum";

// Every flag the bow gift can touch, at 0.
void BowSave(FakeFlags& f) {
    f.has(kBow, FlagKind::int32, 0);
    f.has(kBowGet, FlagKind::boolean, 0);
    for (const auto& g : coop::host::item_grants(coop::host::flag_hash(kBow))) {
        FlagEntry e(FlagKey(g.hash, 0), g.kind, 0);
        f.flags[g.hash] = e;
    }
}

}  // namespace

TEST_CASE("cheat give: the words bombs, bow and hookshot stand for their item flags") {
    using namespace CoopCheatGive;
    CHECK(Names("bombs") == std::vector<std::string>{kBag});
    CHECK(Names("bow") == std::vector<std::string>{kBow});
    CHECK(Names("hookshot") == std::vector<std::string>{"PouchData.Item_Hookshot.StockNum"});
    CHECK(Names("PlayerStatus.MaxLife") == std::vector<std::string>{"PlayerStatus.MaxLife"});
    for (const std::string& name : Names("bombs")) CHECK(IsItemFlag(name));
    for (const std::string& name : Names("bow")) CHECK(IsItemFlag(name));
    for (const std::string& name : Names("hookshot")) CHECK(IsItemFlag(name));
}

TEST_CASE("cheat give: a bare item name stands for its pouch count, else its IsGet flag") {
    using namespace CoopCheatGive;
    // The pouch count wins (Give marks IsGet got too); the full IsGet form still works as it is.
    CHECK(Names("Item_Boomerang") == std::vector<std::string>{"PouchData.Item_Boomerang.StockNum"});
    CHECK(Names("Weapon_Sword_Kokiri") == std::vector<std::string>{"PouchData.Weapon_Sword_Kokiri.StockNum"});
    CHECK(Names("IsGet.Item_Boomerang") == std::vector<std::string>{"IsGet.Item_Boomerang"});
    for (const std::string& name : Names("Item_Boomerang")) CHECK(IsItemFlag(name));
    // A bare name the game has no flag for stays as typed, and Give says what form it wants.
    CHECK(Names("Item_NoSuchThing") == std::vector<std::string>{"Item_NoSuchThing"});
    FakeFlags f;
    const auto r = Give(f, "Item_NoSuchThing");
    CHECK_FALSE(r.ok);
    CHECK(r.text.find("not an item flag") != std::string::npos);
    CHECK(r.text.find("PouchData.Item_NoSuchThing.StockNum") != std::string::npos);
    // The gift through the bare name writes the pouch count and the IsGet flag.
    FakeFlags g;
    g.has("PouchData.Item_Boomerang.StockNum", FlagKind::int32, 0);
    g.has("IsGet.Item_Boomerang", FlagKind::boolean, 0);
    for (const std::string& name : Names("Item_Boomerang")) CHECK(Give(g, name).ok);
    CHECK(g.at("PouchData.Item_Boomerang.StockNum") == 1);
    CHECK(g.at("IsGet.Item_Boomerang") == 1);
}

TEST_CASE("cheat give: the ammo counts are item flags too (per player: the panel does not list them)") {
    CHECK(CoopCheatGive::IsItemFlag(kBombs));
    CHECK(CoopCheatGive::IsItemFlag("PouchData.Item_Arrow_Normal.StockNum"));
    CHECK_FALSE(CoopCheatGive::IsItemFlag("NotAnItem.StockNum"));
}

TEST_CASE("cheat give: an item not held is written with its IsGet flag and its ammo, and the text says what changed") {
    FakeFlags f;
    BowSave(f);
    const auto r = CoopCheatGive::Give(f, kBow);
    CHECK(r.ok);
    CHECK(r.changed);
    CHECK(f.at(kBow) == 1);
    CHECK(f.at(kBowGet) == 1);
    for (const auto& g : coop::host::item_grants(coop::host::flag_hash(kBow))) {
        CHECK_MESSAGE(static_cast<std::int32_t>(f.flags.at(g.hash).value) >= g.value, std::string(g.name));
    }
    CHECK(r.text.find(std::string(kBow) + " 0 -> 1") != std::string::npos);
    CHECK(r.text.find(std::string(kBowGet) + " 0 -> 1") != std::string::npos);
}

TEST_CASE("cheat give: an item already held changes nothing and says so (not an error)") {
    FakeFlags f;
    BowSave(f);
    REQUIRE(CoopCheatGive::Give(f, kBow).ok);
    const auto again = CoopCheatGive::Give(f, kBow);
    CHECK(again.ok);
    CHECK_FALSE(again.changed);
    CHECK(again.text.find("already 1") != std::string::npos);
}

TEST_CASE("cheat give: a grant is only a floor: ammo above it stays") {
    FakeFlags f;
    BowSave(f);
    const auto grants = coop::host::item_grants(coop::host::flag_hash(kBow));
    REQUIRE(!grants.empty());
    const auto& g = grants.front();
    if (g.kind == FlagKind::int32) {
        f.flags[g.hash].value = static_cast<std::uint32_t>(g.value + 7);
        CoopCheatGive::Give(f, kBow);
        CHECK(static_cast<std::int32_t>(f.flags[g.hash].value) == g.value + 7);
    }
}

TEST_CASE("cheat give: a count goes up by one and max life by one heart, each time") {
    FakeFlags f;
    f.has("PlayerStatus.MaxLife", FlagKind::int32, 12);
    CHECK(CoopCheatGive::Give(f, "PlayerStatus.MaxLife").changed);
    CHECK(f.at("PlayerStatus.MaxLife") == 16);
    CHECK(CoopCheatGive::Give(f, "PlayerStatus.MaxLife").changed);
    CHECK(f.at("PlayerStatus.MaxLife") == 20);
    // a count item (a heart piece, a key): the first one of the list
    for (const auto& item : coop::host::item_flags()) {
        if (item.rule != coop::host::ItemRule::count || item.kind != FlagKind::int32) continue;
        FakeFlags g;
        g.has(std::string(item.name).c_str(), FlagKind::int32, 3);
        CHECK(CoopCheatGive::Give(g, item.name).changed);
        CHECK(g.at(std::string(item.name).c_str()) == 4);
        break;
    }
}

TEST_CASE("cheat give: the reasons it fails are said, and nothing is claimed as given") {
    FakeFlags f;
    BowSave(f);
    auto r = CoopCheatGive::Give(f, "Nothing.Like.This");
    CHECK_FALSE(r.ok);
    CHECK(r.text.find("not an item flag") != std::string::npos);

    FakeFlags empty;  // no save loaded: no flag can be read
    r = CoopCheatGive::Give(empty, kBow);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.changed);
    CHECK(r.text.find("not readable") != std::string::npos);

    f.refuses.insert(coop::host::flag_hash(kBow));
    r = CoopCheatGive::Give(f, kBow);
    CHECK_FALSE(r.ok);
    CHECK_FALSE(r.changed);
    CHECK(r.text.find("write failed") != std::string::npos);
    CHECK(f.at(kBow) == 0);
}

TEST_CASE("cheat give: a failed IsGet write is a partial gift, the item is in and the text names the flag") {
    FakeFlags f;
    BowSave(f);
    f.refuses.insert(coop::host::flag_hash(kBowGet));
    const auto r = CoopCheatGive::Give(f, kBow);
    CHECK_FALSE(r.ok);
    CHECK(r.wrote);
    CHECK(r.changed);
    CHECK(f.at(kBow) == 1);
    CHECK(r.text.find("IsGet.Weapon_Range_Bow: write failed") != std::string::npos);
    CHECK_FALSE(CoopCheatGive::Give(f, "Nothing.Like.This").wrote);
}
