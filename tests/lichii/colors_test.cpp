// Player colours: parsing, the tint written into a puppet's model, and the
// planner giving each remote its colour (lichii/hook/coop_colors.*).

#include <doctest/doctest.h>

#include <cstring>
#include <map>
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
    CHECK(CoopColors::Parse("blue") == 0x2050FFu);
    CHECK(CoopColors::Parse("  Red ") == 0xE02020u);
    // The first four names are the player number colours.
    CHECK(CoopColors::Parse("green") == coop::protocol::slot_color(1));
    CHECK(CoopColors::Parse("red") == coop::protocol::slot_color(2));
    CHECK(CoopColors::Parse("blue") == coop::protocol::slot_color(3));
    CHECK(CoopColors::Parse("purple") == coop::protocol::slot_color(4));
    CHECK(CoopColors::Parse("#000001") == 0x000001u);  // any colour
    CHECK(CoopColors::Parse("GREY") == CoopColors::Parse("gray"));
    CHECK(CoopColors::Parse("#12ab7F") == 0x12AB7Fu);
    CHECK(CoopColors::Parse("12AB7F") == 0x12AB7Fu);
    CHECK_FALSE(CoopColors::Parse(""));
    CHECK_FALSE(CoopColors::Parse("#12AB7"));
    CHECK_FALSE(CoopColors::Parse("#12AB7FF"));
    CHECK_FALSE(CoopColors::Parse("#12AG7F"));
    CHECK_FALSE(CoopColors::Parse("blurple"));
    CHECK(CoopColors::Text(0x0A0B0C) == "#0A0B0C");
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

TEST_CASE("colours: ForPlayer gives the choice, else the number's colour") {
    CoopColors::ForgetAll();
    CoopColors::SetOwn(std::nullopt);
    CHECK(CoopColors::ForPlayer(1) == 0x20C040u);
    CHECK(CoopColors::ForPlayer(4) == 0x9030E0u);
    CoopColors::SetChosen(2, 0x123456u);
    CHECK(CoopColors::ForPlayer(2) == 0x123456u);
    CHECK(CoopColors::Chosen(2) == 0x123456u);
    // Our own number: our own choice, not a remote one.
    CoopColors::SetOwn(0xABCDEFu);
    CHECK(CoopColors::ForPlayer(2, 2) == 0xABCDEFu);
    CHECK(CoopColors::ForPlayer(3, 3) == 0xABCDEFu);
    CHECK(CoopColors::ForPlayer(3, 2) == 0x2050FFu);
    // Two players may share a colour.
    CoopColors::SetChosen(3, 0x123456u);
    CHECK(CoopColors::ForPlayer(3) == CoopColors::ForPlayer(2));
    CoopColors::ForgetAll();
    CoopColors::SetOwn(std::nullopt);
    CHECK(CoopColors::ForPlayer(2) == 0xE02020u);
}

namespace {
constexpr std::uint64_t kEu = 0x80004000;
constexpr std::uint64_t kHolder = 0x80005000;
constexpr std::uint64_t kTunic = 0x80006000;
constexpr std::uint64_t kTunicParts = 0x80007000;
constexpr std::uint64_t kTunicModel = 0x80008000;

// ActorWithModel, wearing a tunic piece with its own model (an adult).
void WearTunic(Memory& m) {
    m.Put<std::uint64_t>(kParts + addr::kPartsEquipmentUser, kEu);
    const std::uint64_t link = kEu + addr::kArmorLinks + addr::kLinkStride * addr::kTunicLink;
    m.Put<std::uint32_t>(link + addr::kLinkId, 7);
    m.Put<std::uint64_t>(link + addr::kLinkHolder, kHolder);
    m.Put<std::uint64_t>(kHolder + addr::kHolderActor, kTunic);
    m.Put<std::uint64_t>(kTunic + addr::kActorParts, kTunicParts);
    m.Put<std::uint64_t>(kTunicParts + addr::kPartsModel, kTunicModel);
    m.Put<CoopColors::Rgba>(kTunicModel + addr::kModelColor, {0, 0, 0, 0});
    m.Put<std::uint32_t>(kTunicModel + addr::kModelDirty, 0);
}
}  // namespace

TEST_CASE("colours: the whole puppet, a child's body or an adult's body and tunic") {
    Memory m = ActorWithModel();
    m.Put<std::uint64_t>(kParts + addr::kPartsEquipmentUser, kEu);
    const std::uint64_t link = kEu + addr::kArmorLinks + addr::kLinkStride * addr::kTunicLink;
    m.Put<std::uint32_t>(link + addr::kLinkId, 0xFFFFFFFFu);  // no tunic piece (a child)
    m.Put<std::uint64_t>(link + addr::kLinkHolder, 0);
    CHECK(CoopColors::TunicOf(m, kActor) == 0);

    CoopColors::Painted painted;
    const auto tint = CoopColors::TintOf(0xE02020, 0.6f);
    // A child: the body.
    std::string line = CoopColors::Paint(m, kActor, 0xE02020, 0.6f, painted);
    CHECK(line.find("on body") != std::string::npos);
    CHECK(line.find("0 worn pieces") != std::string::npos);
    CHECK(m.Get<CoopColors::Rgba>(kModel + addr::kModelColor) == tint);
    CHECK(CoopColors::Paint(m, kActor, 0xE02020, 0.6f, painted).empty());  // said once

    // The tunic appears: it takes the colour too, and the body keeps it.
    WearTunic(m);
    CHECK(CoopColors::TunicOf(m, kActor) == kTunic);
    CHECK(CoopColors::PieceOf(m, kActor, 0) == 0);
    line = CoopColors::Paint(m, kActor, 0xE02020, 0.6f, painted);
    CHECK(line.find("1 worn piece,") != std::string::npos);
    CHECK(m.Get<CoopColors::Rgba>(kTunicModel + addr::kModelColor) == tint);
    CHECK(m.Get<std::uint32_t>(kTunicModel + addr::kModelDirty) == addr::kDirtyColor);
    CHECK(m.Get<CoopColors::Rgba>(kModel + addr::kModelColor) == tint);
    CHECK(CoopColors::Paint(m, kActor, 0xE02020, 0.6f, painted).empty());

    // A new colour goes on both; a hit flash the game wrote is painted over.
    m.Put<CoopColors::Rgba>(kModel + addr::kModelColor, {1, 0, 0, 1});
    line = CoopColors::Paint(m, kActor, 0x2050FF, 0.6f, painted);
    CHECK(line.find("#2050FF") != std::string::npos);
    CHECK(m.Get<CoopColors::Rgba>(kTunicModel + addr::kModelColor) == CoopColors::TintOf(0x2050FF, 0.6f));
    CHECK(m.Get<CoopColors::Rgba>(kModel + addr::kModelColor) == CoopColors::TintOf(0x2050FF, 0.6f));
}

TEST_CASE("colours: a piece whose model is not made yet: the body for now") {
    Memory m = ActorWithModel();
    WearTunic(m);
    m.Put<std::uint64_t>(kTunicParts + addr::kPartsModel, 0);
    CoopColors::Painted painted;
    const std::string line = CoopColors::Paint(m, kActor, 0x20C040, 0.6f, painted);
    CHECK(line.find("on body") != std::string::npos);
    CHECK(line.find("0 worn pieces") != std::string::npos);
    CHECK(m.Get<CoopColors::Rgba>(kModel + addr::kModelColor) == CoopColors::TintOf(0x20C040, 0.6f));
    // The model comes: a new line, now with the piece.
    m.Put<std::uint64_t>(kTunicParts + addr::kPartsModel, kTunicModel);
    CHECK(CoopColors::Paint(m, kActor, 0x20C040, 0.6f, painted).find("1 worn piece,") !=
          std::string::npos);
}

namespace {
namespace cloth = CoopColors::cloth;
constexpr std::uint64_t kBase = 0x10000000;
constexpr std::uint64_t kHolder2 = 0x80009000;
constexpr std::uint64_t kHolderVt = 0x8000a000;
constexpr std::uint64_t kSet = 0x8000b000;
constexpr std::uint64_t kUnits = 0x8000c000;
constexpr std::uint64_t kEntry = 0x8000d000;
constexpr std::uint64_t kUnit = 0x8000e000;
constexpr std::uint64_t kUnitVt = 0x8000f000;
constexpr std::uint64_t kScratch = 0x80010000;

// An actor with one model unit, as the game lays it out.
Memory ActorWithUnit() {
    Memory m;
    m.Put<std::uint64_t>(kActor + addr::kActorParts, kParts);
    m.Put<std::uint64_t>(kParts + cloth::kPartsHolder, kHolder2);
    m.Put<std::uint64_t>(kHolder2, kHolderVt);
    m.Put<std::uint64_t>(kHolderVt + cloth::kHolderGetSet, kBase + cloth::kHolderGetSetDefault);
    m.Put<std::uint64_t>(kHolder2 + cloth::kHolderSet, kSet);
    m.Put<std::int32_t>(kSet + cloth::kSetCount, 1);
    m.Put<std::uint64_t>(kSet + cloth::kSetUnits, kUnits);
    m.Put<std::uint64_t>(kUnits, kEntry);
    m.Put<std::uint64_t>(kEntry, kUnit);
    m.Put<std::uint64_t>(kUnit, kUnitVt);
    m.Put<std::uint64_t>(kUnitVt + cloth::kFindMaterial, 0x111);
    m.Put<std::uint64_t>(kUnitVt + cloth::kFindParam, 0x222);
    m.Put<std::uint64_t>(kUnitVt + cloth::kSetColor, 0x333);
    m.Map(kScratch, 0x60);
    return m;
}

std::string TextAt(Memory& m, std::uint64_t ptr_cell) {
    const auto ptr = m.Get<std::uint64_t>(ptr_cell);
    std::string s;
    for (char c = m.Get<char>(ptr); c != 0; c = m.Get<char>(ptr + s.size())) s += c;
    return s;
}
}  // namespace

TEST_CASE("colours: cloth colour, the shade and linear values") {
    const auto c = CoopColors::ClothColor(0xFF0000, 0.0f);
    CHECK(c[0] == doctest::Approx(1.0f));
    CHECK(c[1] == doctest::Approx(0.0f));
    CHECK(c[3] == doctest::Approx(1.0f));
    const auto light = CoopColors::ClothColor(0xFF0000, 0.5f);
    CHECK(light[0] == doctest::Approx(1.0f));
    CHECK(light[1] > 0.2f);  // half way to white in sRGB, then linear
    CHECK(light[1] < 0.5f);
    const auto white = CoopColors::ClothColor(0x2050FF, 1.0f);
    CHECK(white[0] == doctest::Approx(1.0f));
    // Divided by the texture's own colour: a mid-grey base gives more than 4 times the value.
    const auto based = CoopColors::ClothColor(0xFF0000, 0.0f, 0x808080);
    CHECK(based[0] > 4.0f);
    CHECK(based[1] == doctest::Approx(0.0f));
    CHECK(based[3] == doctest::Approx(1.0f));
}

TEST_CASE("colours: cloth, the tunic strong and the undershirt light, through the game's calls") {
    Memory m = ActorWithUnit();
    CoopColors::ClothSettings s;
    s.strong = {"Mt_Angin"};
    s.light = {"Mt_Inner"};
    CoopColors::Cloth cloth;
    std::string line;

    // Material lookups: Mt_Angin is material 4, Mt_Inner is not there.
    auto c = cloth.Next(m, kBase, kActor, 0xE02020, s, kScratch, line);
    REQUIRE(c);
    CHECK(c->function == 0x111);
    CHECK(c->x[0] == kUnit);
    CHECK(TextAt(m, c->x[1]) == "Mt_Angin");
    CHECK_FALSE(cloth.Next(m, kBase, kActor, 0xE02020, s, kScratch, line));  // waiting
    CHECK(cloth.Done(4, s).empty());
    c = cloth.Next(m, kBase, kActor, 0xE02020, s, kScratch, line);
    REQUIRE(c);
    CHECK(TextAt(m, c->x[1]) == "Mt_Inner");
    cloth.Done(0xFFFFFFFFu, s);

    // The parameter of the one found.
    c = cloth.Next(m, kBase, kActor, 0xE02020, s, kScratch, line);
    REQUIRE(c);
    CHECK(c->function == 0x222);
    CHECK(c->x[1] == 4);
    CHECK(TextAt(m, c->x[2]) == "ns_param_color0");
    cloth.Done(2, s);

    // The colour.
    c = cloth.Next(m, kBase, kActor, 0xE02020, s, kScratch, line);
    REQUIRE(c);
    CHECK(c->function == 0x333);
    CHECK(c->x[1] == 4);
    CHECK(c->x[2] == 2);
    CHECK(m.Get<CoopColors::Rgba>(c->x[3]) == CoopColors::ClothColor(0xE02020, 0.0f));
    cloth.Done(0, s);
    CHECK_FALSE(cloth.Next(m, kBase, kActor, 0xE02020, s, kScratch, line));
    CHECK(line.find("Mt_Angin u0 m4 ns_param_color0=2 (strong)") != std::string::npos);
    CHECK(line.find("painted 1 strong, 0 light") != std::string::npos);

    // Same colour: nothing more. A new colour: painted again, no lookups.
    CHECK_FALSE(cloth.Next(m, kBase, kActor, 0xE02020, s, kScratch, line));
    c = cloth.Next(m, kBase, kActor, 0x2050FF, s, kScratch, line);
    REQUIRE(c);
    CHECK(c->function == 0x333);
    CHECK(m.Get<CoopColors::Rgba>(c->x[3]) == CoopColors::ClothColor(0x2050FF, 0.0f));
}

TEST_CASE("colours: cloth waits for the model and checks the holder's class") {
    Memory m = ActorWithUnit();
    CoopColors::ClothSettings s;
    CoopColors::Cloth cloth;
    std::string line;
    m.Put<std::uint64_t>(kHolderVt + cloth::kHolderGetSet, 0x999);  // another class
    CHECK_FALSE(cloth.Next(m, kBase, kActor, 0xE02020, s, kScratch, line));
    m.Put<std::uint64_t>(kHolderVt + cloth::kHolderGetSet, kBase + cloth::kHolderGetSetDefault);
    m.Put<std::int32_t>(kSet + cloth::kSetCount, 0);  // no unit yet
    CHECK_FALSE(cloth.Next(m, kBase, kActor, 0xE02020, s, kScratch, line));
    m.Put<std::int32_t>(kSet + cloth::kSetCount, 1);
    CHECK(cloth.Next(m, kBase, kActor, 0xE02020, s, kScratch, line));
}
