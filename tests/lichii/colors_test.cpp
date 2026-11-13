// Player colours: parsing, the tint written into a puppet's model, and the
// planner giving each remote its colour (lichii/hook/coop_colors.*).

#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <ostream>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_colors.h"
#include "coop_puppet.h"

namespace {

namespace addr = CoopColors::addr;

class Memory : public CoopPuppet::Guest {
public:
    void Map(std::uint64_t address, std::size_t size) {
        for (std::uint64_t i = 0; i < size; ++i) bytes_[address + i];
    }
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = bytes_.find(address + i);
            if (it == bytes_.end()) return false;
            o[i] = it->second;
        }
        return true;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const auto* p = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = bytes_.find(address + i);
            if (it == bytes_.end()) return false;
            it->second = p[i];
        }
        ++writes;
        return true;
    }
    template <typename T>
    void Put(std::uint64_t address, const T& value) {
        Map(address, sizeof(T));
        Write(address, &value, sizeof(T));
    }
    template <typename T>
    T Get(std::uint64_t address) {
        T value{};
        Read(address, &value, sizeof(T));
        return value;
    }
    int writes = 0;

private:
    std::map<std::uint64_t, std::uint8_t> bytes_;
};

constexpr std::uint64_t kActor = 0x80001000;
constexpr std::uint64_t kParts = 0x80002000;
constexpr std::uint64_t kModel = 0x80003000;

// An actor whose model is there, with no colour and one other dirty bit.
Memory ActorWithModel() {
    Memory m;
    m.Put<std::uint64_t>(kActor + addr::kActorParts, kParts);
    m.Put<std::uint64_t>(kParts + addr::kPartsModel, kModel);
    m.Put<CoopColors::Rgba>(kModel + addr::kModelColor, {0, 0, 0, 0});
    m.Put<std::uint32_t>(kModel + addr::kModelDirty, 0x100);
    m.writes = 0;
    return m;
}

}  // namespace

TEST_CASE("colours: names and #RRGGBB") {
    CHECK(CoopColors::Parse("blue") == 0x3070FFu);
    CHECK(CoopColors::Parse("  Red ") == 0xFF3030u);
    CHECK(CoopColors::Parse("GREY") == CoopColors::Parse("gray"));
    CHECK(CoopColors::Parse("#12ab7F") == 0x12AB7Fu);
    CHECK(CoopColors::Parse("12AB7F") == 0x12AB7Fu);
    CHECK_FALSE(CoopColors::Parse(""));
    CHECK_FALSE(CoopColors::Parse("#12AB7"));
    CHECK_FALSE(CoopColors::Parse("#12AB7FF"));
    CHECK_FALSE(CoopColors::Parse("#12AG7F"));
    CHECK_FALSE(CoopColors::Parse("blurple"));
    CHECK(CoopColors::Text(0x0A0B0C) == "#0A0B0C");
    // The forms players type in an ini (10.1: a hex colour did nothing for one).
    CHECK(CoopColors::Parse("\"#FF8020\"") == 0xFF8020u);
    CHECK(CoopColors::Parse("'ff8020'") == 0xFF8020u);
    CHECK(CoopColors::Parse("0xFF8020") == 0xFF8020u);
    CHECK(CoopColors::Parse("#F82") == 0xFF8822u);
    CHECK(CoopColors::Parse("#FF8020FF") == 0xFF8020u);
    CHECK(CoopColors::Parse("FF802080") == 0xFF8020u);
    CHECK(CoopColors::Parse("rgb(255, 128, 32)") == 0xFF8020u);
    CHECK(CoopColors::Parse("rgba(255,128,32,0.5)") == 0xFF8020u);
    CHECK_FALSE(CoopColors::Parse("rgb(256, 0, 0)"));
    CHECK_FALSE(CoopColors::Parse("\"#FF8020"));
    CHECK(CoopColors::TunicOf(*CoopColors::Parse("#FF8020")) == CoopColors::TunicOf(*CoopColors::Parse("orange")));
    // Every name the error message lists is known.
    std::string names = CoopColors::Names();
    for (std::size_t at = 0; at < names.size();) {
        const std::size_t comma = names.find(',', at);
        const std::string name = names.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        CHECK_MESSAGE(CoopColors::Parse(name), name);
        at = comma == std::string::npos ? names.size() : comma + 2;
    }
}

TEST_CASE("colours: the tint and its strength") {
    const auto t = CoopColors::TintOf(0xFF8000, 0.5f);
    CHECK(t[0] == doctest::Approx(1.0f));
    CHECK(t[1] == doctest::Approx(128.0f / 255.0f));
    CHECK(t[2] == doctest::Approx(0.0f));
    CHECK(t[3] == doctest::Approx(0.5f));
    CHECK(CoopColors::TintOf(0xFF8000, 3.0f)[3] == doctest::Approx(1.0f));
    // Strength 0 is the game's own "no flash", (0, 0, 0, 0).
    CHECK(CoopColors::TintOf(0xFF8000, 0.0f) == CoopColors::Rgba{0, 0, 0, 0});

    CHECK(CoopColors::StrengthFrom(nullptr) == doctest::Approx(CoopColors::kDefaultStrength));
    CHECK(CoopColors::StrengthFrom("") == doctest::Approx(CoopColors::kDefaultStrength));
    CHECK(CoopColors::StrengthFrom("0.8") == doctest::Approx(0.8f));
    CHECK(CoopColors::StrengthFrom("0") == 0.0f);
    CHECK(CoopColors::StrengthFrom("off") == 0.0f);
    CHECK(CoopColors::StrengthFrom("2") == doctest::Approx(CoopColors::kDefaultStrength));
    CHECK(CoopColors::StrengthFrom("x") == doctest::Approx(CoopColors::kDefaultStrength));
}

TEST_CASE("colours: written into the model as the hit flash does") {
    Memory m = ActorWithModel();
    const auto tint = CoopColors::TintOf(0x3070FF, 0.45f);
    CHECK(CoopColors::ModelOf(m, kActor) == kModel);
    CHECK(CoopColors::Apply(m, kActor, tint) == CoopColors::Applied::written);
    CHECK(m.Get<CoopColors::Rgba>(kModel + addr::kModelColor) == tint);
    CHECK(m.Get<std::uint32_t>(kModel + addr::kModelDirty) == (0x100u | addr::kDirtyColor));

    // The same again writes nothing.
    m.writes = 0;
    CHECK(CoopColors::Apply(m, kActor, tint) == CoopColors::Applied::same);
    CHECK(m.writes == 0);

    // The game put its own colour back (a hit flash ended): written again.
    m.Put<CoopColors::Rgba>(kModel + addr::kModelColor, {0, 0, 0, 0});
    m.Put<std::uint32_t>(kModel + addr::kModelDirty, 0);
    CHECK(CoopColors::Apply(m, kActor, tint) == CoopColors::Applied::written);
    CHECK(m.Get<std::uint32_t>(kModel + addr::kModelDirty) == addr::kDirtyColor);
}

TEST_CASE("colours: no model, nothing written") {
    Memory m;
    CHECK(CoopColors::Apply(m, 0, {1, 0, 0, 1}) == CoopColors::Applied::no_model);
    CHECK(CoopColors::Apply(m, kActor, {1, 0, 0, 1}) == CoopColors::Applied::no_model);
    m.Put<std::uint64_t>(kActor + addr::kActorParts, 0);  // parts not made yet
    CHECK(CoopColors::Apply(m, kActor, {1, 0, 0, 1}) == CoopColors::Applied::no_model);
    m.Put<std::uint64_t>(kActor + addr::kActorParts, kParts);
    m.Put<std::uint64_t>(kParts + addr::kPartsModel, 0);  // no model
    CHECK(CoopColors::Apply(m, kActor, {1, 0, 0, 1}) == CoopColors::Applied::no_model);
    CHECK(m.writes == 3);  // the three Puts only
}

TEST_CASE("colours: each remote gets its chosen colour, or its slot's") {
    CoopPuppet::Planner p;
    std::vector<CoopPuppet::Remote> remotes(2);
    remotes[0].number = 2;
    remotes[0].scene = "Kokiri";
    remotes[1].number = 3;
    remotes[1].scene = "Kokiri";
    remotes[1].color = 0x123456u;
    const auto in = p.Plan(0x8000000, std::string("Kokiri"), CoopPuppet::Vec3{0, 0, 0}, false,
                           remotes, 1000);
    REQUIRE(in.targets.size() == 2);
    CHECK(in.targets[0].color == coop::protocol::slot_color(2));
    CHECK(in.targets[1].color == 0x123456u);
}

TEST_CASE("colours: the tunic of a colour, and the puppet actor for it") {
    CHECK(CoopColors::TunicOf(0xFF3030) == "Red");
    CHECK(CoopColors::TunicOf(0x3070FF) == "Blue");
    CHECK(CoopColors::TunicOf(0x30D040).empty());  // green: the game's own tunic
    CHECK(CoopColors::TunicOf(0xE00000) == "Red");  // nearest
    CHECK(CoopColors::TunicOf(0xA0A0A0) == "White");
    CHECK(CoopColors::TunicOf(0x000000) == "Black");
    CHECK(CoopColors::Tunics().size() == 9);  // tools/puppet_pack/tunic.py COLORS
    for (const auto name : {"red", "orange", "yellow", "cyan", "blue", "purple", "pink", "white", "black"}) {
        const auto rgb = CoopColors::Parse(name);
        REQUIRE(rgb);
        CHECK_FALSE(CoopColors::TunicOf(*rgb).empty());
    }

    CoopPuppet::Planner p;
    CHECK(p.ActorFor(true) == "CoopPuppet_Adult");
    CHECK(p.ActorFor(true, 0xFF3030u) == "CoopPuppet_Adult_Red");
    CHECK(p.ActorFor(false, 0x3070FFu) == "CoopPuppet_Child_Blue");
    CHECK(p.ActorFor(false, 0x30D040u) == "CoopPuppet_Child");
    p.SetFailedTunics({"CoopPuppet_Adult_Red"});  // that one did not work: plain, the others stay
    CHECK(p.ActorFor(true, 0xFF3030u) == "CoopPuppet_Adult");
    CHECK(p.ActorFor(false, 0xFF3030u) == "CoopPuppet_Child_Red");
    CHECK(p.ActorFor(true, 0x3070FFu) == "CoopPuppet_Adult_Blue");
    p.SetFailedTunics({});
    CHECK(p.ActorFor(true, 0xFF3030u) == "CoopPuppet_Adult_Red");
    p.SetGreenPuppet(true);  // your own Link wears your colour: green players get their own puppet
    CHECK(p.ActorFor(false, 0x30D040u) == "CoopPuppet_Child_Green");
    CHECK(p.ActorFor(true, 0xFF3030u) == "CoopPuppet_Adult_Red");
    p.SetFailedTunics({"CoopPuppet_Child_Green"});
    CHECK(p.ActorFor(false, 0x30D040u) == "CoopPuppet_Child");
    p.SetFailedTunics({});
    CHECK(CoopPuppet::Director::IsTunicActor("CoopPuppet_Child_Green"));
    CHECK(CoopPuppet::PackHasGreenPuppets(
        R"({"actors": [{"actor": "CoopPuppet_Child_Green"}, {"actor": "CoopPuppet_Adult_Green"}]})"));
    CHECK_FALSE(CoopPuppet::PackHasGreenPuppets(R"({"actors": [{"actor": "CoopPuppet_Child_Red"}]})"));
    CHECK_FALSE(CoopPuppet::PackHasGreenPuppets(R"({"actors": [{"actor": "CoopPuppet_Child_Green"}]})"));
    CHECK_FALSE(CoopPuppet::PackHasGreenPuppets(""));
    p.SetGreenPuppet(false);
    p.SetTunics(false);  // LICHII_COOP_TUNICS=0
    CHECK(p.ActorFor(true, 0xFF3030u) == "CoopPuppet_Adult");
    CHECK(CoopPuppet::Director::IsTunicActor("CoopPuppet_Adult_Red"));
    CHECK_FALSE(CoopPuppet::Director::IsTunicActor("CoopPuppet_Adult"));
    CHECK_FALSE(CoopPuppet::Director::IsTunicActor("Npc_Some_Thing"));
}

TEST_CASE("colours: the Goron and Zora tunics a puppet wears follow its player's colour") {
    const std::string goron = "Armor_Adult_Goron";
    const std::string zora = "Armor_Adult_Zora";
    // The names as tools/puppet_pack writes them (coop_puppet.json "armors"), each once.
    const auto pack = CoopPuppet::PackArmorTunics(
        R"({"armors": [{"actor": "CoopArmor_Goron_Red", "pack": "Pack/Actor/CoopArmor_Goron_Red.pack.zs",
            "members": ["Actor/CoopArmor_Goron_Red.engine__actor__ActorParam.bgyml"]},
            {"actor": "CoopArmor_Goron_Blue"}, {"actor": "CoopArmor_Goron_Green"},
            {"actor": "CoopArmor_Zora_Red"}, {"actor": "CoopArmor_Zora_Green"}],
            "actors": [{"actor": "CoopPuppet_Adult_Red"}]})");
    CHECK(pack == std::vector<std::string>{"CoopArmor_Goron_Red", "CoopArmor_Goron_Blue", "CoopArmor_Goron_Green",
                                           "CoopArmor_Zora_Red", "CoopArmor_Zora_Green"});
    CHECK(CoopPuppet::PackArmorTunics(R"({"actors": [{"actor": "CoopPuppet_Adult_Red"}]})").empty());
    CHECK(CoopPuppet::PackArmorTunics("").empty());
    CHECK(CoopPuppet::PackArmorTunics(R"("CoopArmor_Goron_Re)").empty());  // cut short: nothing

    CoopPuppet::Planner p;
    // A pack without them (made before): the game's own tunics, as before.
    CHECK(p.ArmorFor(goron, 0xFF3030u) == goron);
    p.SetArmorTunics(pack);
    CHECK(p.ArmorFor(goron, 0xFF3030u) == "CoopArmor_Goron_Red");
    CHECK(p.ArmorFor(zora, 0xFF3030u) == "CoopArmor_Zora_Red");
    CHECK(p.ArmorFor(goron, 0x3070FFu) == "CoopArmor_Goron_Blue");
    CHECK(p.ArmorFor(goron, 0xE01818u) == "CoopArmor_Goron_Red");  // a hex colour: its nearest preset
    CHECK(p.ArmorFor(zora, 0x3070FFu) == zora);  // the pack has no blue Zora tunic: the game's
    // Green (no colour): the game's own Goron red and Zora blue, untouched.
    CHECK(p.ArmorFor(goron, 0x30D040u) == goron);
    CHECK(p.ArmorFor(zora, 0x30D040u) == zora);
    CHECK(p.ArmorFor(goron, std::nullopt) == goron);
    // Anything else a player wears is never changed.
    CHECK(p.ArmorFor("Armor_Adult_Normal", 0xFF3030u) == "Armor_Adult_Normal");
    CHECK(p.ArmorFor("Armor_Adult_HoverBoot", 0xFF3030u) == "Armor_Adult_HoverBoot");
    CHECK(p.ArmorFor("", 0xFF3030u) == "");
    // One that was not made in the game: the game's own for it, the others stay.
    p.SetFailedTunics({"CoopArmor_Goron_Red"});
    CHECK(p.ArmorFor(goron, 0xFF3030u) == goron);
    CHECK(p.ArmorFor(zora, 0xFF3030u) == "CoopArmor_Zora_Red");
    p.SetFailedTunics({});
    // Your own Link wears another colour: the game's tunic models are that colour's, so a green
    // player gets the pack's green copy (the game's red and blue under another name).
    p.SetGreenPuppet(true);
    CHECK(p.ArmorFor(goron, 0x30D040u) == "CoopArmor_Goron_Green");
    CHECK(p.ArmorFor(zora, 0x30D040u) == "CoopArmor_Zora_Green");
    CHECK(p.ArmorFor(goron, 0xFF3030u) == "CoopArmor_Goron_Red");
    p.SetGreenPuppet(false);
    p.SetTunics(false);  // LICHII_COOP_TUNICS=0
    CHECK(p.ArmorFor(goron, 0xFF3030u) == goron);
    CHECK(CoopPuppet::Director::IsTunicArmor("CoopArmor_Goron_Red"));
    CHECK_FALSE(CoopPuppet::Director::IsTunicArmor(goron));
    CHECK_FALSE(CoopPuppet::Director::IsTunicArmor("CoopPuppet_Adult_Red"));
    // Every name fits the gear text the equip call is given.
    for (const auto tunic : CoopColors::Tunics()) {
        CHECK(std::string("CoopArmor_Goron_").size() + tunic.size() < CoopPuppet::block::kGearCapacity);
    }
}
