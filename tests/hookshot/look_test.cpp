// SPDX-License-Identifier: GPL-3.0-or-later
// Your Link's look changed in the pause menu (hookshot/src/coop_look.h) and
// its tunic rewritten in memory (coop_look_tunic.h).
#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "coop_colors.h"
#include "coop_look.h"
#include "coop_look_paint.h"
#include "coop_look_parts.h"
#include "coop_look_tunic.h"
#include "coop_title_menu.h"

using CoopLook::Look;
using CoopLook::Part;

namespace {

Look Tunic(std::optional<std::uint32_t> c) {
    Look l;
    l[Part::tunic] = c;
    return l;
}

// One block of memory at `base`; anything else is not there.
class FakeMemory final : public CoopLookTunic::Memory {
public:
    FakeMemory(std::uint64_t base, std::size_t size) : base_(base), bytes_(size) {}
    bool Read(std::uint64_t a, void* out, std::size_t n) const override {
        if (!In(a, n)) {
            return false;
        }
        std::memcpy(out, bytes_.data() + (a - base_), n);
        return true;
    }
    bool Write(std::uint64_t a, const void* in, std::size_t n) override {
        if (!In(a, n)) {
            return false;
        }
        std::memcpy(bytes_.data() + (a - base_), in, n);
        ++writes;
        return true;
    }
    std::uint8_t* at(std::uint64_t a) { return bytes_.data() + (a - base_); }
    int writes = 0;

private:
    bool In(std::uint64_t a, std::size_t n) const { return a >= base_ && a + n <= base_ + bytes_.size(); }
    std::uint64_t base_;
    std::vector<std::uint8_t> bytes_;
};

constexpr std::uint64_t kOffset = 0x100;   // the texels' place in the texture's file
constexpr std::size_t kTexels = 256;
constexpr std::uint64_t kBase = 0x10000;   // where the fake heap starts

std::vector<std::uint8_t> TexelsOf(std::uint8_t seed) {
    std::vector<std::uint8_t> t(kTexels);
    for (std::size_t i = 0; i < t.size(); ++i) {
        t[i] = static_cast<std::uint8_t>(seed + i * 7);
    }
    return t;
}

// A texture file's image at `address`: a header naming `name`, then the texels.
void PutFile(FakeMemory& m, std::uint64_t address, const std::string& name, const std::vector<std::uint8_t>& texels) {
    std::uint8_t* p = m.at(address);
    std::memcpy(p, "BNTX", 4);
    std::memcpy(p + 0x40, name.c_str(), name.size() + 1);
    std::memcpy(p + kOffset - 16, "BRTD", 4);
    std::memcpy(p + kOffset, texels.data(), texels.size());
}

using Named = std::vector<std::pair<std::string, std::vector<std::uint8_t>>>;

// A coop_look file, as tools/puppet_pack/look_live.py packs it.
std::vector<std::uint8_t> LookFile(const Named& textures) {
    std::vector<std::uint8_t> out = {'C', 'O', 'O', 'P', 'L', 'O', 'O', 'K'};
    const auto put = [&](std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    put(1, 4);
    put(textures.size(), 4);
    for (const auto& [name, texels] : textures) {
        std::string n = name;
        n.resize(64, '\0');
        out.insert(out.end(), n.begin(), n.end());
        put(kOffset, 8);
        put(texels.size(), 8);
        out.insert(out.end(), texels.begin(), texels.end());
    }
    return out;
}

std::shared_ptr<const CoopLookTunic::Texels> Colour(const Named& textures) {
    auto t = CoopLookTunic::Parse(LookFile(textures));
    REQUIRE(t);
    return std::make_shared<const CoopLookTunic::Texels>(std::move(*t));
}

bool Holding(FakeMemory& m, std::uint64_t at, std::uint8_t seed) {
    return std::memcmp(m.at(at), TexelsOf(seed).data(), kTexels) == 0;
}

}  // namespace

TEST_CASE("look: a chosen look waits for the pause screen to close") {
    CoopLook::Choice c(Tunic(0x3070FF));
    CHECK(c.Shown() == Tunic(0x3070FF));
    CHECK_FALSE(c.Unpause());
    c.SetPending(Tunic(0xFF3030));
    CHECK(c.Shown() == Tunic(0xFF3030));
    CHECK(c.Worn() == Tunic(0x3070FF));   // nothing changes before the unpause
    const auto worn = c.Unpause();
    REQUIRE(worn);
    CHECK(*worn == Tunic(0xFF3030));
    CHECK(c.Worn() == Tunic(0xFF3030));
    CHECK_FALSE(c.Unpause());   // once
    // Choosing the worn look again takes the waiting one away.
    c.SetPending(Tunic(std::nullopt));
    c.SetPending(Tunic(0xFF3030));
    CHECK_FALSE(c.Unpause());
}

TEST_CASE("look: colours as the settings files and the puppet pack name them") {
    CHECK(CoopLook::ColourText(std::nullopt).empty());
    CHECK(CoopLook::ColourText(CoopColors::Parse("blue")) == "blue");
    CHECK(CoopLook::ColourText(0x123456u) == "#123456");
    CHECK(CoopColors::Parse(CoopLook::ColourText(0x123456u)) == 0x123456u);
    CHECK(CoopLook::TunicName(std::nullopt) == "green");
    CHECK(CoopLook::TunicName(CoopColors::Parse("green")) == "green");
    CHECK(CoopLook::TunicName(CoopColors::Parse("blue")) == "blue");
    CHECK(CoopLook::TunicName(0xF03020u) == "red");   // the nearest pack colour
}

TEST_CASE("look: the colour is saved in coop-options.ini for a kit, else in coop.ini") {
    std::string coop = "server = 1.2.3.4\n";
    std::string options = "# color = blue\nLICHII_COOP_TUNICS=1\n";
    CHECK(CoopLook::WriteSettings(Tunic(CoopColors::Parse("red")), coop, &options));
    CHECK(coop == "server = 1.2.3.4\n");
    CHECK(options == "color = red\nLICHII_COOP_TUNICS=1\n");
    CHECK_FALSE(CoopLook::WriteSettings(Tunic(CoopColors::Parse("red")), coop, &options));   // unchanged
    CHECK(CoopLook::WriteSettings(Tunic(std::nullopt), coop, &options));
    CHECK(options.rfind("color", 0) != 0);   // commented out: the player number's colour
    CHECK_FALSE(CoopTitleMenu::IniGet(options, "color").has_value());
    CHECK(CoopLook::WriteSettings(Tunic(0x123456u), coop, nullptr));
    CHECK(coop == "server = 1.2.3.4\ncolor = #123456\n");
}

TEST_CASE("look: every part has its number, a key and a row, and the numbers never move") {
    std::set<std::string> keys;
    for (std::size_t i = 0; i < CoopLook::kParts; ++i) {
        const auto part = CoopLook::PartOf(static_cast<std::uint8_t>(i));
        REQUIRE(part);
        const CoopLook::PartInfo& info = CoopLook::InfoOf(*part);
        CHECK(info.part == *part);
        CHECK(keys.insert(info.key).second);
        CHECK(std::string(info.item).size() > 0);
        CHECK(std::string(info.name).size() > 0);
    }
    CHECK_FALSE(CoopLook::PartOf(static_cast<std::uint8_t>(CoopLook::kParts)));
    // The numbers are the settings' and the network's: they never change.
    CHECK(static_cast<int>(Part::tunic) == 0);
    CHECK(static_cast<int>(Part::hylian_frame) == 3);
    CHECK(static_cast<int>(Part::master_blade) == 11);
    CHECK(static_cast<int>(Part::gauntlets) == 16);
}

TEST_CASE("look: every part is saved under its own key, and read back") {
    Look look;
    look[Part::tunic] = CoopColors::Parse("red");
    look[Part::hylian_frame] = 0xE0C030u;
    look[Part::master_hilt] = CoopColors::Parse("blue");
    std::string coop = "server = 1.2.3.4\n";
    CHECK(CoopLook::WriteSettings(look, coop, nullptr));
    CHECK(coop == "server = 1.2.3.4\ncolor = red\nlook_hylian_frame = #E0C030\nlook_master_hilt = blue\n");
    std::vector<std::string> errors;
    const Look back = CoopLook::ReadParts(coop, &errors);
    CHECK(errors.empty());
    CHECK_FALSE(back[Part::tunic]);   // the tunic comes with the co-op settings, not here
    CHECK(back[Part::hylian_frame] == 0xE0C030u);
    CHECK(back[Part::master_hilt] == CoopColors::Parse("blue"));
    CHECK_FALSE(back[Part::boots]);
    // A part set back to the game's own is commented out; one never set stays out of the file.
    look[Part::hylian_frame].reset();
    CHECK(CoopLook::WriteSettings(look, coop, nullptr));
    CHECK_FALSE(CoopTitleMenu::IniGet(coop, "look_hylian_frame").has_value());
    CHECK(coop.find("look_boots") == std::string::npos);
    CHECK(CoopLook::ReadParts("look_boots = sparkly\n", &errors) == Look{});
    CHECK(errors.size() == 1);
}

TEST_CASE("look: the wire form lists the set parts by number") {
    Look look;
    look[Part::zora_tunic] = 0x102030u;
    look[Part::boots] = 0xFFFFFFu;
    const auto wire = CoopLook::Wire(look);
    REQUIRE(wire.size() == 2);
    CHECK(wire[0] == std::pair<std::uint8_t, std::uint32_t>{2, 0x102030u});
    CHECK(wire[1] == std::pair<std::uint8_t, std::uint32_t>{15, 0xFFFFFFu});
    CHECK(CoopLook::FromWire(wire) == look);
    // A part a newer build has is passed over.
    CHECK(CoopLook::FromWire({{2, 0x102030u}, {15, 0xFFFFFFu}, {40, 1u}}) == look);
    CHECK(CoopLook::Wire(Look{}).empty());
}

TEST_CASE("look: the module applies a chosen look on the frame after the unpause, once") {
    std::vector<Look> applied;
    CoopLook::Init(Tunic(std::nullopt), [&](const Look& l) { applied.push_back(l); });
    CoopLook::Frame();
    CHECK(applied.empty());   // the start's look is not applied again
    CoopLook::SetPending(Tunic(0x3070FF));
    CoopLook::Frame();
    CHECK(applied.empty());   // still paused
    CHECK(CoopLook::Shown() == Tunic(0x3070FF));
    CHECK(CoopLook::Worn() == Tunic(std::nullopt));
    CoopLook::ApplyOnUnpause();
    CoopLook::Frame();
    REQUIRE(applied.size() == 1);
    CHECK(applied[0] == Tunic(0x3070FF));
    CHECK(CoopLook::Worn() == Tunic(0x3070FF));
    CoopLook::Frame();
    CoopLook::ApplyOnUnpause();   // a pause with no change
    CoopLook::Frame();
    CHECK(applied.size() == 1);
    CoopLook::Init({}, nullptr);
}

TEST_CASE("look tunic: look files are read whole or not at all") {
    const auto file = LookFile({{"Tile_A", TexelsOf(1)}, {"Vest", TexelsOf(2)}});
    const auto t = CoopLookTunic::Parse(file);
    REQUIRE(t);
    REQUIRE(t->size() == 2);
    CHECK((*t)[1].name == "Vest");
    CHECK((*t)[1].offset == kOffset);
    CHECK((*t)[1].texels == TexelsOf(2));
    std::string why;
    CHECK_FALSE(CoopLookTunic::Parse({file.begin(), file.end() - 1}, &why));
    CHECK(why == "the look file is cut short");
    auto other = file;
    other[8] = 2;   // another version
    CHECK_FALSE(CoopLookTunic::Parse(other));
    CHECK(CoopLookTunic::SameLayout(*t, *Colour({{"Tile_A", TexelsOf(9)}, {"Vest", TexelsOf(8)}})));
    CHECK_FALSE(CoopLookTunic::SameLayout(*t, *Colour({{"Tile_A", TexelsOf(9)}})));
}

TEST_CASE("look tunic: the mapped end of the heap") {
    FakeMemory m(0x1000, 0x5000);
    CHECK(CoopLookTunic::MappedEnd(m, 0x1000, 0x100000, 0x1000) == 0x6000);
    CHECK(CoopLookTunic::MappedEnd(m, 0x1000, 0x3000, 0x1000) == 0x3000);       // the limit
    CHECK(CoopLookTunic::MappedEnd(m, 0x80000, 0x100000, 0x1000) == 0x80000);   // nothing there
}

TEST_CASE("look tunic: your Link's textures are found and rewritten, a puppet's copy is left alone") {
    FakeMemory m(kBase, 0x10000);
    const auto green = Colour({{"Tile_A", TexelsOf(1)}, {"Vest", TexelsOf(2)}});
    const auto red = Colour({{"Tile_A", TexelsOf(9)}, {"Vest", TexelsOf(8)}});
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    PutFile(m, kBase + 0x3000, "Vest", TexelsOf(2));
    PutFile(m, kBase + 0x5000, "Tile_A_Red", TexelsOf(1));   // a puppet's: another name

    const auto found = CoopLookTunic::Find(m, kBase, kBase + 0x10000, {green, red});
    REQUIRE(found.places.size() == 2);
    CHECK(found.copies == 1);
    auto place = found.places[0];
    CHECK(place.address == kBase + 0x1000 + kOffset);
    CHECK((place.colour == green));
    CHECK(CoopLookTunic::Holds(m, place));
    REQUIRE(CoopLookTunic::Put(m, place, red));
    CHECK((place.colour == red));
    CHECK(Holding(m, place.address, 9));
    CHECK(Holding(m, kBase + 0x5000 + kOffset, 1));   // the puppet's copy
    // The game loaded it again (green): a Put that expects red writes nothing.
    std::memcpy(m.at(place.address), TexelsOf(1).data(), kTexels);
    CHECK_FALSE(CoopLookTunic::Holds(m, place));
    const int writes = m.writes;
    CHECK_FALSE(CoopLookTunic::Put(m, place, green));
    CHECK(m.writes == writes);
}

TEST_CASE("look tunic: the wardrobe keeps your Link in the colour worn") {
    FakeMemory m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    std::vector<std::string> log;
    CoopLookTunic::Wardrobe::Host host;
    host.memory = &m;
    host.range = [] { return std::pair<std::uint64_t, std::uint64_t>{kBase, kBase + 0x10000}; };
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        if (it == pack.end()) {
            return std::nullopt;
        }
        return it->second;
    };
    host.log = [&](const std::string& l) { log.push_back(l); };
    CoopLookTunic::Wardrobe w(host);
    const std::uint64_t at = kBase + 0x1000 + kOffset;

    w.Wear("green");
    CHECK_FALSE(w.Busy());   // the game's own: nothing to do
    w.Wear("red");
    CHECK(w.Busy());
    w.Step(0);
    CHECK(Holding(m, at, 9));
    // The game loads the texture again (a scene change): red again.
    std::memcpy(m.at(at), TexelsOf(1).data(), kTexels);
    w.Step(1000);
    CHECK(Holding(m, at, 9));
    // A colour the pack does not have: the one worn stays, said once.
    w.Wear("teal");
    w.Step(2000);
    w.Step(3000);
    CHECK(Holding(m, at, 9));
    CHECK(std::count_if(log.begin(), log.end(), [](const std::string& l) { return l.find("teal") != std::string::npos; }) == 1);
    // Back to green: written, then nothing more to do.
    w.Wear("green");
    w.Step(4000);
    CHECK(Holding(m, at, 1));
    CHECK_FALSE(w.Busy());
}

// ---------------------------------------------------------------------------
// The gear (coop_look_parts.h)

namespace {

// BC1 texels of kTexels bytes: blocks of a blue end pair and of a grey one in turn.
std::vector<std::uint8_t> Shield() {
    std::vector<std::uint8_t> t;
    for (std::size_t b = 0; b < kTexels / 8; ++b) {
        const std::uint16_t c0 = b % 2 == 0 ? 0x3a54 : 0xb5b6;   // (60,72,160) blue / (180,180,180) grey
        const std::uint16_t c1 = b % 2 == 0 ? 0x1909 : 0x5aeb;
        const std::uint8_t block[8] = {static_cast<std::uint8_t>(c0), static_cast<std::uint8_t>(c0 >> 8),
                                       static_cast<std::uint8_t>(c1), static_cast<std::uint8_t>(c1 >> 8),
                                       0x1b, 0x1b, 0x1b, 0x1b};
        t.insert(t.end(), block, block + 8);
    }
    return t;
}

const char* const kHylian = "Mt_Weapon_Shield_Hylian_Alb";

}  // namespace

TEST_CASE("look parts: a look file packed is read back the same") {
    const auto texels = Colour({{"A", TexelsOf(1)}, {"B_Alb", TexelsOf(2)}});
    const auto back = CoopLookTunic::Parse(CoopLookTunic::Pack(*texels));
    REQUIRE(back);
    CHECK(back->size() == 2);
    CHECK(CoopLookTunic::SameLayout(*back, *texels));
    CHECK((*back)[1].texels == TexelsOf(2));
    CHECK(CoopLookTunic::Pack(*texels) == LookFile({{"A", TexelsOf(1)}, {"B_Alb", TexelsOf(2)}}));
}

TEST_CASE("look parts: every gear part has a texture, every texture once") {
    std::set<std::string> names;
    std::set<Part> parts;
    for (const auto& g : CoopLookParts::Gear()) {
        CHECK(names.insert(g.name).second);
        CHECK_FALSE(g.rules.empty());
        for (const auto& r : g.rules) {
            CHECK(r.part != Part::tunic);   // the tunic has the pack's colours (coop_look_tunic.h)
            CHECK(r.tones != 0);
            parts.insert(r.part);
        }
    }
    CHECK(parts.size() == CoopLook::kParts - 1);
}

TEST_CASE("look parts: the gear painted in a look, a painting's name says its colours") {
    Look look;
    CHECK_FALSE(CoopLookParts::AnyGear(look));
    CHECK(CoopLookParts::Name(look) == "gear");
    look[Part::tunic] = 0xFF0000u;
    CHECK(CoopLookParts::Name(look) == "gear");   // the tunic is not gear
    look[Part::hylian_face] = 0x20A040u;
    CHECK(CoopLookParts::AnyGear(look));
    const std::string name = CoopLookParts::Name(look);
    CHECK(name.find("20A040") != std::string::npos);
    look[Part::hylian_face] = 0x20A041u;
    CHECK(CoopLookParts::Name(look) != name);
    look[Part::hylian_face] = 0x20A040u;

    const auto gear = Colour({{kHylian, Shield()}, {"Something_Else_Alb", Shield()}});
    const auto painted = CoopLookParts::Painted(*gear, look);
    REQUIRE(painted.size() == 2);
    CHECK(painted[1].texels == Shield());   // not a gear texture: as it is
    const auto& p = painted[0].texels;
    CHECK(p != Shield());
    // the blue blocks are green now, the grey ones as they were
    CHECK(std::memcmp(p.data() + 8, Shield().data() + 8, 8) == 0);
    const auto ends = CoopLookPaint::Ends(CoopLookPaint::Format::bc1, std::span<const std::uint8_t>(p.data(), 8));
    CHECK(CoopLookPaint::ToneOf(ends[0]) == CoopLookPaint::kGreen);
    // nothing set for this texture's parts: as it is
    Look boots;
    boots[Part::boots] = 0x000080u;
    CHECK(CoopLookParts::Painted(*gear, boots)[0].texels == Shield());
}

TEST_CASE("look parts: the gear wardrobe paints your shield, and finds one loaded later at the unpause") {
    FakeMemory m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, kHylian, Shield());
    Look look;
    look[Part::hylian_frame] = 0xE0C030u;
    const auto base = LookFile({{kHylian, Shield()}});
    CoopLookTunic::Wardrobe::Host host;
    host.memory = &m;
    host.range = [] { return std::pair<std::uint64_t, std::uint64_t>{kBase, kBase + 0x10000}; };
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        if (c == CoopLookParts::kBase) {
            return base;
        }
        return CoopLookTunic::Pack(CoopLookParts::Painted(*CoopLookTunic::Parse(base), look));
    };
    std::vector<std::string> log;
    host.log = [&](const std::string& l) { log.push_back(l); };
    host.base = CoopLookParts::kBase;
    host.what = "gear";
    CoopLookTunic::Wardrobe w(host);
    w.Wear(CoopLookParts::Name(look));
    w.Step(0);
    const auto want = CoopLookParts::Painted(*CoopLookTunic::Parse(base), look)[0].texels;
    CHECK(std::memcmp(m.at(kBase + 0x1000 + kOffset), want.data(), want.size()) == 0);
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) { return l.find("your Link's gear is") != std::string::npos; }));
    // A second shield's texture loads (the game's own texels): not seen until the next search...
    PutFile(m, kBase + 0x4000, kHylian, Shield());
    w.Step(1000);
    CHECK(std::memcmp(m.at(kBase + 0x4000 + kOffset), Shield().data(), kTexels) == 0);
    // ...which the unpause brings forward.
    w.Kick();
    w.Step(2000);
    CHECK(std::memcmp(m.at(kBase + 0x4000 + kOffset), want.data(), want.size()) == 0);
    // Back to the game's own colours.
    w.Wear(CoopLookParts::kBase);
    w.Step(3000);
    CHECK(std::memcmp(m.at(kBase + 0x1000 + kOffset), Shield().data(), kTexels) == 0);
    CHECK_FALSE(w.Busy());
}

TEST_CASE("look: an unpause with the look unchanged is told too") {
    int applied = 0, unpaused = 0;
    CoopLook::Init(Look{}, [&](const Look&) { ++applied; }, [&] { ++unpaused; });
    CoopLook::ApplyOnUnpause();
    CoopLook::Frame();
    CHECK(applied == 0);
    CHECK(unpaused == 1);
    Look l;
    l[Part::boots] = 1u;
    CoopLook::SetPending(l);
    CoopLook::ApplyOnUnpause();
    CoopLook::Frame();
    CHECK(applied == 1);
    CHECK(unpaused == 1);
    CoopLook::Frame();   // no unpause: nothing
    CHECK(unpaused == 1);
    CoopLook::Init(Look{}, nullptr);
}
