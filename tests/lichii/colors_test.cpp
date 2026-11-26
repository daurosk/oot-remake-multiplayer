// Player colours: parsing, the tint written into a puppet's model, and the
// planner giving each remote its colour (lichii/hook/coop_colors.*).

#include <algorithm>
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
    // Player report 47: never the plain puppet (your own Link's model) while
    // the green one works: a failed colour falls back to green.
    p.SetFailedTunics({"CoopPuppet_Adult_Red"});
    CHECK(p.ActorFor(true, 0xFF3030u) == "CoopPuppet_Adult_Green");
    p.SetFailedTunics({});
    p.SetTunics(false);  // tunics off: everyone green, on the green puppet
    CHECK(p.ActorFor(true, 0xFF3030u) == "CoopPuppet_Adult_Green");
    CHECK(p.ActorFor(false, std::nullopt) == "CoopPuppet_Child_Green");
    p.SetTunics(true);
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

TEST_CASE("puppets: in a scene with stealth guards each puppet is its quiet copy (issue 150)") {
    CHECK(CoopPuppet::QuietActor("CoopPuppet_Child") == "CoopPuppet_ChildQuiet");
    CHECK(CoopPuppet::QuietActor("CoopPuppet_Adult_Red") == "CoopPuppet_AdultQuiet_Red");
    CHECK(CoopPuppet::QuietActor("CoopPuppet_Childish") == "CoopPuppet_Childish");  // no puppet of ours
    CHECK(CoopPuppet::QuietActor("Player_Child") == "Player_Child");
    // Still a tunic actor (the model check) or a plain one, as its loud puppet.
    CHECK(CoopPuppet::Director::IsTunicActor("CoopPuppet_AdultQuiet_Red"));
    CHECK_FALSE(CoopPuppet::Director::IsTunicActor("CoopPuppet_ChildQuiet"));

    CHECK(CoopPuppet::PackQuietScenes(R"({"perception": true, "quiet_scenes": ["GerudoFortress_Adult", "HyruleCastle"]})") ==
          std::vector<std::string>{"GerudoFortress_Adult", "HyruleCastle"});
    CHECK(CoopPuppet::PackQuietScenes(R"({"quiet_scenes": []})").empty());
    CHECK(CoopPuppet::PackQuietScenes(R"({"perception": false})").empty());  // a pack before them
    CHECK(CoopPuppet::PackPerception(R"({"perception": true, "quiet_scenes": []})"));
    CHECK(CoopPuppet::PackPerception(R"({"perception" :  true})"));
    CHECK_FALSE(CoopPuppet::PackPerception(R"({"perception": false, "quiet_scenes": []})"));
    CHECK_FALSE(CoopPuppet::PackPerception(R"({"tool": "tools/puppet_pack"})"));  // a pack before it
    // build.py's shape: every actor's own "perception" (null or its emitter) comes before the pack's.
    CHECK(CoopPuppet::PackPerception(
        R"({"actors": [{"actor": "CoopPuppet_Child", "perception": {"param": "x", "size": 4}}, )"
        R"({"actor": "CoopPuppet_ChildQuiet", "perception": null}], "perception": true, "body": true})"));
    CHECK_FALSE(CoopPuppet::PackPerception(
        R"({"actors": [{"actor": "CoopPuppet_Child", "perception": null}], "perception": false, "body": false})"));
    // A quiet pack still counts its loud tunic puppets only (PackHasTunicPuppets).
    CHECK_FALSE(CoopPuppet::PackHasTunicPuppets(R"({"actors": [{"actor": "CoopPuppet_ChildQuiet"}]})"));

    CoopPuppet::PlannerSettings s;
    s.quiet_scenes = {"HyruleCastle"};
    CoopPuppet::Planner p(s);
    std::vector<CoopPuppet::Remote> remotes(1);
    remotes[0].number = 2;
    remotes[0].color = 0xFF3030u;
    const auto plan = [&](const char* scene) {
        remotes[0].scene = scene;
        const auto in = p.Plan(0x8000000, std::string(scene), CoopPuppet::Vec3{0, 0, 0}, false, remotes, 1000);
        REQUIRE(in.targets.size() == 1);
        return in.targets[0].actor;
    };
    CHECK(plan("Kokiri") == "CoopPuppet_Child_Red");  // enemies see it
    CHECK(plan("HyruleCastle") == "CoopPuppet_ChildQuiet_Red");  // no guard does
    p.SetFailedTunics({"CoopPuppet_ChildQuiet_Red"});
    CHECK(plan("HyruleCastle") == "CoopPuppet_ChildQuiet");
    CHECK(plan("Kokiri") == "CoopPuppet_Child_Red");
    p.SetTunics(false);
    CHECK(plan("HyruleCastle") == "CoopPuppet_ChildQuiet");
    CHECK(plan("Kokiri") == "CoopPuppet_Child");
}

TEST_CASE("puppets: quiet everywhere when the runtime cannot make a perceived puppet safe (issue 150)") {
    CoopPuppet::PlannerSettings s;
    s.quiet_scenes = {"HyruleCastle"};
    s.quiet_everywhere = true;
    CoopPuppet::Planner p(s);
    std::vector<CoopPuppet::Remote> remotes(1);
    remotes[0].number = 2;
    remotes[0].color = 0xFF3030u;
    const auto plan = [&](const char* scene) {
        remotes[0].scene = scene;
        const auto in = p.Plan(0x8000000, std::string(scene), CoopPuppet::Vec3{0, 0, 0}, false, remotes, 1000);
        REQUIRE(in.targets.size() == 1);
        return in.targets[0].actor;
    };
    CHECK(plan("Kokiri") == "CoopPuppet_ChildQuiet_Red");  // no Physics: patch 0002's guard arms the null page too
    CHECK(plan("HyruleCastle") == "CoopPuppet_ChildQuiet_Red");
    p.SetTunics(false);
    CHECK(plan("Kokiri") == "CoopPuppet_ChildQuiet");
}

TEST_CASE("colors (0.11.9): each other player gets a tunic copy of their own, nearest free, never the plain one") {
    using CoopColors::AssignTunicCopies;
    // Two players in near the same orange: the first by number gets Orange, the next the nearest still free.
    auto a = AssignTunicCopies({{5, 0xFF7020}, {2, 0xFF6820}});
    REQUIRE(a.size() == 2);
    CHECK(a[2].suffix == "Orange");
    CHECK(a[2].exact);
    CHECK(a[5].suffix != "Orange");
    CHECK_FALSE(a[5].suffix.empty());          // never the plain puppet (the game's own tunic texture)
    CHECK(a[5].exact);
    // A green player also gets a coloured copy (repainted green), not the plain one.
    a = AssignTunicCopies({{3, 0x30D040}});
    CHECK_FALSE(a[3].suffix.empty());
    CHECK(a[3].exact);
    // Nine copies: the tenth player and later share the nearest, not exact.
    std::vector<std::pair<std::uint8_t, std::uint32_t>> many;
    for (std::uint8_t n = 1; n <= 11; ++n) many.push_back({n, 0x123456});
    a = AssignTunicCopies(many);
    int exact = 0;
    std::vector<std::string> suffixes;
    for (const auto& [n, copy] : a) {
        if (copy.exact) {
            ++exact;
            suffixes.push_back(copy.suffix);
        }
    }
    CHECK(exact == 9);
    std::sort(suffixes.begin(), suffixes.end());
    CHECK(std::adjacent_find(suffixes.begin(), suffixes.end()) == suffixes.end());   // all different
    CHECK_FALSE(a[10].exact);
    CHECK(a[10].suffix == std::string(CoopColors::TunicOf(0x123456)));
    // Sticky (review round 2): P3 joins in a colour nearer P2's copy: P2 keeps it, P3 gets a free one; P2 changing
    // colour keeps its copy (repainted); P2 leaving frees it for the next.
    const auto first = AssignTunicCopies({{2, 0xFF7020}});
    REQUIRE(first.at(2).suffix == "Orange");
    auto next = AssignTunicCopies({{2, 0xFF7020}, {1, 0xFF8020}}, first);   // (P1 sorts before P2, and is nearer Orange)
    CHECK(next.at(2).suffix == "Orange");
    CHECK(next.at(1).suffix != "Orange");
    CHECK(next.at(1).exact);
    auto changed = AssignTunicCopies({{2, 0x2040FF}, {1, 0xFF8020}}, next);
    CHECK(changed.at(2).suffix == "Orange");                 // (its copy kept, repainted blue)
    CHECK(changed.at(1).suffix == next.at(1).suffix);
    auto left = AssignTunicCopies({{1, 0xFF8020}, {4, 0xFF8020}}, changed);
    CHECK(left.at(1).suffix == next.at(1).suffix);
    CHECK(left.at(4).suffix == "Orange");                    // (P2 left: its copy free again)
    CHECK(CoopColors::TunicColour("Red") == 0xFF3030u);
    CHECK_FALSE(CoopColors::TunicColour(""));
    CHECK_FALSE(CoopColors::TunicColour("Mauve"));
}

TEST_CASE("colors (0.11.9): a puppet wears the tunic copy given to its player, else the nearest") {
    CoopPuppet::PlannerSettings settings;
    settings.tunics = true;
    CoopPuppet::Planner p(settings);
    CHECK(p.ActorFor(false, 0xFF6820u) == "CoopPuppet_Child_Orange");
    CHECK(p.ActorFor(false, 0xFF6820u, 0, std::string("Purple")) == "CoopPuppet_Child_Purple");   // (its own copy)
    CHECK(p.ActorFor(true, std::nullopt, 0, std::string("Cyan")) == "CoopPuppet_Adult_Cyan");
    CHECK(p.ActorFor(false, 0xFF6820u, 0, std::string("")) == "CoopPuppet_Child");   // (the plain one: shared, green)
    CoopPuppet::SetTunicCopies({{3, "Pink"}});
    CHECK(CoopPuppet::TunicCopyOf(3) == std::optional<std::string>("Pink"));
    CHECK_FALSE(CoopPuppet::TunicCopyOf(4));
    CoopPuppet::SetTunicCopies({});
}
