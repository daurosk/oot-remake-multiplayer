// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../lichii/hook/coop_char_colours.h"

using namespace CoopCharColours;

namespace {

// A guest: bytes by address, and a model of one unit whose virtual functions are fakes the calls land in.
struct FakeGame {
    std::map<std::uint64_t, std::uint8_t> mem;
    static constexpr std::uint64_t kMain = 0x80020000, kKind = 0x9000;
    static constexpr std::uint64_t kActor = 0x10000, kComps = 0x11000, kComponent = 0x12000, kModel = 0x13000;
    static constexpr std::uint64_t kList = 0x14000, kUnitPtr = 0x14100, kUnit = 0x15000, kUnitVt = 0x16000;
    static constexpr std::uint64_t kCompVt = 0x17000, kObjs = 0x18000, kCell = 0x20000;
    static constexpr std::uint64_t kFnCount = 0xF1, kFnByName = 0xF2, kFnParam = 0xF3, kFnSet = 0xF4;
    struct Mat {
        std::string name;
        std::uint8_t samplers, textures;
        std::vector<std::string> params;
    };
    std::vector<Mat> mats;
    struct Set {
        std::int64_t mat;
        std::uint64_t param;
        std::array<float, 4> f;
    };
    std::vector<Set> sets;
    struct Tex {
        std::uint64_t object, view, slot;
    };
    std::vector<Tex> textures;
    std::size_t calls = 0;

    void W64(std::uint64_t a, std::uint64_t v) {
        for (int i = 0; i < 8; i++) mem[a + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    void W(std::uint64_t a, const void* p, std::size_t n) {
        for (std::size_t i = 0; i < n; i++) mem[a + i] = static_cast<const std::uint8_t*>(p)[i];
    }
    std::uint64_t R64(std::uint64_t a) const {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; i++) {
            auto it = mem.find(a + i);
            v |= static_cast<std::uint64_t>(it == mem.end() ? 0 : it->second) << (8 * i);
        }
        return v;
    }
    std::string Str(std::uint64_t a) const {
        std::string s;
        for (;; a++) {
            auto it = mem.find(a);
            if (it == mem.end() || it->second == 0) return s;
            s += static_cast<char>(it->second);
        }
    }
    static std::uint64_t Obj(std::size_t i) { return kObjs + 0x80 * i; }
    static std::uint64_t Rec(std::size_t i) { return 0x30000 + 0x1000 * i; }
    static std::uint64_t View(std::size_t i, std::size_t k) { return 0xA0000000 + 0x100 * i + k; }
    static std::uint64_t Slot(std::size_t i, std::size_t k) { return 0xB0000000 + 0x100 * i + k; }

    explicit FakeGame(std::vector<Mat> m) : mats(std::move(m)) {
        W64(kMain + kModelTypeGot, kKind);
        W64(kActor + kActorComponents, kComps);
        W64(kComps + 8, kComponent);
        W64(kComponent, kCompVt);
        W64(kCompVt + 0xf8, kKind);
        W64(kComponent + 0x18, kModel);
        const std::int32_t one = 1;
        W(kModel + 0x20, &one, 4);
        W64(kModel + 0x28, kList);
        W64(kList, kUnitPtr);
        W64(kUnitPtr, kUnit);
        W64(kUnit, kUnitVt);
        W64(kUnitVt + kUnitMaterialCount, kFnCount);
        W64(kUnitVt + kUnitMaterialByName, kFnByName);
        W64(kUnitVt + kUnitParamByName, kFnParam);
        W64(kUnitVt + kUnitSetFloats, kFnSet);
        W64(kUnit + 0x108, kObjs);
        for (std::size_t i = 0; i < mats.size(); i++) {
            const std::uint64_t rec = Rec(i);
            W64(Obj(i), rec);
            mem[rec + kMatSamplers] = mats[i].samplers;
            mem[rec + kMatTextures] = mats[i].textures;
            W64(rec + kMatViews, rec + 0x200);
            W64(rec + kMatSlots, rec + 0x400);
            for (std::size_t k = 0; k < mats[i].textures; k++) {
                W64(rec + 0x200 + 8 * k, View(i, k));
                W64(rec + 0x400 + 8 * k, Slot(i, k));
            }
            W64(Obj(i) + kObjViews, rec + 0x600);
            W64(rec + 0x600, View(i, 0));   // the live list: the material's own first
            // the parameters' info and the file's values: parameter p at offset 16 p, (p+1)/10 ... and alpha 0.75
            W64(rec + 0x10, rec + 0x700);
            W64(rec + 0x700, rec + 0x800);
            W64(rec + 0x800 + 0x20, rec + 0x900);
            W64(rec + 0x58, rec + 0xA00);
            for (std::size_t p = 0; p < 3; p++) {
                const auto off = static_cast<std::uint16_t>(16 * p);
                W(rec + 0x900 + 0x18 * p + 0x10, &off, 2);
                const std::array<float, 4> f{0.1f * (p + 1), 0.2f, 0.3f, 0.75f};
                W(rec + 0xA00 + off, f.data(), 16);
            }
        }
    }

    Game AsGame() {
        Game g;
        g.main_base = kMain;
        g.read = [this](std::uint64_t a, void* out, std::size_t n) {
            for (std::size_t i = 0; i < n; i++) {
                auto it = mem.find(a + i);
                static_cast<std::uint8_t*>(out)[i] = it == mem.end() ? 0 : it->second;
            }
            return true;
        };
        g.write = [this](std::uint64_t a, const void* in, std::size_t n) {
            W(a, in, n);
            return true;
        };
        g.call = [this](std::uint64_t f, const std::array<std::uint64_t, 8>& x) -> std::optional<std::uint64_t> {
            calls++;
            if (f == kFnCount) return mats.size();
            if (f == kFnByName) {
                const std::string name = Str(R64(x[1]));
                for (std::size_t i = 0; i < mats.size(); i++)
                    if (mats[i].name == name) return i;
                return 0xFFFFFFFFu;
            }
            if (f == kFnParam) {
                const std::string name = Str(R64(x[2]));
                const auto& ps = mats[x[1]].params;
                for (std::size_t p = 0; p < ps.size(); p++)
                    if (ps[p] == name) return p;   // (index p into the info: this fake keeps the params in order)
                return 0xFFFFu;
            }
            if (f == kFnSet) {
                std::array<float, 4> v{};
                for (int i = 0; i < 4; i++) {
                    std::uint32_t b = 0;
                    for (int k = 0; k < 4; k++) b |= static_cast<std::uint32_t>(mem[x[3] + 4 * i + k]) << (8 * k);
                    std::memcpy(&v[i], &b, 4);
                }
                sets.push_back({static_cast<std::int64_t>(x[1]), x[2], v});
                return 0;
            }
            if (f == kMain + kSetTexture) {
                textures.push_back({x[0], R64(x[2]), R64(x[2] + 8)});
                W64(R64(x[0] + kObjViews), R64(x[2]));   // the engine's own routine sets the live list
                return 0;
            }
            return std::nullopt;
        };
        return g;
    }
};

std::vector<FakeGame::Mat> Character() {
    return {
        {"Mt_Hairs", 2, 11, {"ns_param_color0", "ns_param_color1"}},   // hair: a palette of 9
        {"Mt_Eye_Inner", 1, 1, {"x", "y", "ns_param_color2"}},
        {"Mt_Cloth", 1, 8, {}},                                          // a Clothing Color 1 palette (7)
        {"Mt_Vest", 1, 9, {}},                                           // Clothing Color 2 (8)
        {"Mt_Skin", 1, 1, {}},
    };
}

std::uint32_t Custom(int hair, int eyes, int gear, int c1, int c2 = 0, int c3 = 0) {
    return static_cast<std::uint32_t>(hair | eyes << 4 | gear << 8 | c1 << 12 | c2 << 16 | c3 << 20);
}

State StateFor() {
    State s;
    s.actor = FakeGame::kActor;
    s.cell = FakeGame::kCell;
    return s;
}

}  // namespace

TEST_CASE("char colours: the six options are nibbles in the mod's order, out of range ones are its own") {
    const Options o = Options::Of(Custom(3, 7, 1, 2, 5, 7));
    CHECK(o.hair == 3);
    CHECK(o.eyes == 7);
    CHECK(o.gear == 1);
    CHECK(o.clothes == std::array<std::uint8_t, 3>{2, 5, 7});
    CHECK(Options::Of(Custom(10, 8, 0, 8)).Colours() == false);   // hair > 9, eyes > 7, cloth > 7: the model's own
    CHECK(Options::Of(Custom(0, 0, 1, 0)).Colours() == false);    // the equipment alone is no colour
}

TEST_CASE("char colours: palette places and clothing rows by the palette's length") {
    CHECK(PalettePlace(2, 11, 1) == 2);   // the first entry behind the two samplers' own
    CHECK(PalettePlace(2, 11, 9) == 10);
    CHECK(PalettePlace(2, 9, 9) == 0);    // a colour the palette lacks: the material's own
    CHECK(PalettePlace(2, 2, 3) == 0);    // no palette
    CHECK(PalettePlace(1, 8, 0) == 0);
    CHECK(ClothRow(1, 8) == 0);
    CHECK(ClothRow(1, 9) == 1);
    CHECK(ClothRow(1, 10) == -1);   // 9: the hair's
    CHECK(ClothRow(1, 11) == 2);
    CHECK(ClothRow(1, 7) == -1);
    CHECK(ClothRow(1, 12) == -1);
}

TEST_CASE("char colours: the colours are the mod's, the iris shift is held to a byte") {
    CHECK(HairColour(0, 0) == std::nullopt);
    CHECK((*HairColour(1, 0))[0] == doctest::Approx(1.0));
    CHECK((*HairColour(1, 1))[0] == doctest::Approx(202 / 255.0));
    CHECK((*BrowColour(9))[2] == doctest::Approx(166 / 255.0));
    CHECK((*EyeColour(1, 0))[0] == doctest::Approx(128 / 255.0));
    CHECK((*EyeColour(5, 100))[0] == doctest::Approx(1.0));   // 242 + 100 held at 255
    CHECK((*EyeColour(3, -100))[0] == doctest::Approx(0.0));
    CHECK(EyeColour(8, 0) == std::nullopt);
}

TEST_CASE("char colours (npc-customize): Link's eyes and hair on a Link model, the character parts on a character's") {
    using CoopCharColours::ForModel;
    using CoopCharColours::ModelColours;
    std::map<std::uint8_t, std::uint32_t> look = {{18, 0xFF123456u}, {19, 0x00ABCDEFu}};
    const auto part = [&look](std::uint8_t n) -> std::optional<std::uint32_t> {
        const auto it = look.find(n);
        return it == look.end() ? std::nullopt : std::optional<std::uint32_t>(it->second);
    };
    CHECK(ForModel(true, part) == ModelColours{0, 0x123456u, 0xABCDEFu});
    // A character with none of its own parts: Link's never (review 0.11.9), a 0.11.8 peer's options as they came.
    CHECK(ForModel(false, part) == ModelColours{});
    CHECK(ForModel(false, part, 0xFF000012u) == ModelColours{0x12u, std::nullopt, std::nullopt});
    // Its own parts: exact hair and eyes, the hair's and clothes' nearest palette entries and Hidden as the options.
    look[20] = 0x0A0808u;   // (near black: hair entry 3)
    look[21] = 0x20C040u;
    look[22] = 0xFF1010u;   // (red: clothing entry 2)
    look[24] = 0xFFFFFFu;   // (white: entry 7)
    look[25] = 1u;
    const ModelColours c = ForModel(false, part, 0x12u);
    CHECK(c.hair == 0x0A0808u);
    CHECK(c.eyes == 0x20C040u);
    CHECK(c.custom == (3u | (1u << 8) | (2u << 12) | (7u << 20)));
    CHECK(ForModel(true, part) == ModelColours{0, 0x123456u, 0xABCDEFu});   // (Link keeps his own)
}

TEST_CASE("char colours (npc-customize): the mod's nearest hair and clothing palette entries") {
    using namespace CoopCharColours;
    for (std::uint8_t i = 0; i < kCloths; ++i) CHECK(NearestCloth(kClothRgb[i]) == i + 1);
    CHECK(NearestCloth(0x102080u) == 3);   // (a dark blue: Blue, by hue; plain RGB distance said Purple)
    CHECK(NearestCloth(0x000040u) == 3);   // (a navy)
    CHECK(NearestCloth(0x40FF40u) == 1);   // (a light green)
    CHECK(NearestCloth(0xFF80FFu) == 4);   // (a pink: Purple)
    CHECK(NearestCloth(0xC0C0C0u) == 7);   // (a light grey: White)
    CHECK(NearestCloth(0x404040u) == 6);   // (a dark grey: Black)
    CHECK(NearestCloth(0x100808u) == 6);   // (near black)
    CHECK(NearestHair(0xFFD050u) == 1);    // (a light blond: Blonde)
    CHECK(NearestHair(0x000000u) == 3);    // Black
    CHECK(NearestHair(0xF0F0F8u) == 9);    // White
}

TEST_CASE("char colours (npc-customize): Equipment Hidden clears the shown bit of the mod's handles, and shows again") {
    using namespace CoopCharColours;
    std::map<std::uint64_t, std::uint8_t> mem;
    const auto put = [&](std::uint64_t a, const void* in, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) mem[a + i] = static_cast<const std::uint8_t*>(in)[i];
        return true;
    };
    const auto put64 = [&](std::uint64_t a, std::uint64_t v) { put(a, &v, 8); };
    const auto put32 = [&](std::uint64_t a, std::uint32_t v) { put(a, &v, 4); };
    Game g;
    g.read = [&](std::uint64_t a, void* out, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            const auto it = mem.find(a + i);
            if (it == mem.end()) return false;
            static_cast<std::uint8_t*>(out)[i] = it->second;
        }
        return true;
    };
    g.write = put;
    const std::uint64_t actor = 0x1000, comps = 0x2000, equip = 0x3000;
    put64(actor + kActorComponents, comps);
    put64(comps + kActorEquipment, equip);
    // handle 0 (the sword: in the mask), 1 (the item in hand: not), 4 (the shield, being deleted: state 9)
    const auto piece = [&](std::uint32_t k, std::uint64_t link, std::uint64_t at, std::uint32_t state) {
        const std::uint64_t h = equip + kGearFirst + kGearStride * k;
        put32(h + 0x10, 40 + k);
        put64(h + 0x8, link);
        put32(link + 0x48, 40 + k);
        put64(link + 0x40, at);
        put32(at + 0x2c, state);
        put32(at + kActorFlags, 0x10003u);
    };
    piece(0, 0x4000, 0x5000, 1);
    piece(1, 0x4100, 0x5100, 1);
    piece(4, 0x4200, 0x5200, 9);
    const auto flags = [&](std::uint64_t at) {
        std::uint32_t v = 0;
        g.read(at + kActorFlags, &v, 4);
        return v;
    };
    CHECK(SetGearShown(g, actor, false) == 1);
    CHECK(flags(0x5000) == 0x3u);       // hidden, its other bits kept
    CHECK(flags(0x5100) == 0x10003u);   // the item in hand stays
    CHECK(flags(0x5200) == 0x10003u);   // a piece being deleted is left alone
    CHECK(SetGearShown(g, actor, false) == 0);
    CHECK(SetGearShown(g, actor, true) == 1);
    CHECK(flags(0x5000) == 0x10003u);
    CHECK(SetGearShown(g, 0x9999, false) == 0);   // (no equipment: nothing)
}

TEST_CASE("char colours: the pause menu's model of your Link is the game's Pouch twin of the field player (lane: eyes)") {
    using CoopCharColours::MenuModelOfPlayer;
    CHECK(MenuModelOfPlayer("PouchPlayer_Child", "Player_Child"));
    CHECK(MenuModelOfPlayer("PouchPlayer_Adult", "Player_Adult"));
    CHECK_FALSE(MenuModelOfPlayer("PouchPlayer_Adult", "Player_Child"));   // (the other age: not this Link)
    CHECK_FALSE(MenuModelOfPlayer("Player_Child", "Player_Child"));        // (the field actor's own name: not the menu's)
    CHECK_FALSE(MenuModelOfPlayer("Tapestry_ChallengeMenu", "Player_Child"));
    CHECK_FALSE(MenuModelOfPlayer("Pouch", ""));                            // (no field player: nothing)
    CHECK_FALSE(MenuModelOfPlayer("", ""));
}

TEST_CASE("char colours: a puppet gets its player's hair, eyes and clothes through the model's own calls") {
    FakeGame f(Character());
    State s = StateFor();
    const Result r = Dress(f.AsGame(), s, Custom(2, 3, 0, 4, 1), false);
    CHECK_FALSE(r.failed);
    CHECK(r.units == 1);
    // hair: colour 0 and colour 1 of Mt_Hairs (material 0), the file's alpha kept
    REQUIRE(f.sets.size() == 3);
    CHECK(f.sets[0].mat == 0);
    CHECK(f.sets[0].param == 0);
    CHECK(f.sets[0].f[0] == doctest::Approx(102 / 255.0));
    CHECK(f.sets[0].f[3] == doctest::Approx(0.75));
    CHECK(f.sets[1].param == 1);
    CHECK(f.sets[1].f[1] == doctest::Approx(36 / 255.0));
    // eyes: ns_param_color2 of Mt_Eye_Inner (material 1, its third parameter)
    CHECK(f.sets[2].mat == 1);
    CHECK(f.sets[2].param == 2);
    CHECK(f.sets[2].f[0] == doctest::Approx(59 / 255.0));
    // textures: the hair's entry 2 (place 2 + 2 - 1 = 3), Mt_Cloth's colour 4 (place 4), Mt_Vest's colour 1 (place 1)
    REQUIRE(f.textures.size() == 3);
    CHECK(f.textures[0].object == FakeGame::Obj(0));
    CHECK(f.textures[0].view == FakeGame::View(0, 3));
    CHECK(f.textures[0].slot == FakeGame::Slot(0, 3));
    CHECK(f.textures[1].view == FakeGame::View(2, 4));
    CHECK(f.textures[2].view == FakeGame::View(3, 1));
    // Again: the names are known (no more look-ups), the textures there already are not set again.
    const std::size_t before = f.calls;
    f.textures.clear();
    const Result again = Dress(f.AsGame(), s, Custom(2, 3, 0, 4, 1), false);
    CHECK(f.textures.empty());
    CHECK(again.calls == f.calls - before);
    CHECK(again.calls == 3);   // three parameter sets (the material count is from the look-up)
}

TEST_CASE("char colours: an option at 0 is left alone, or put back to the file's own with put_back") {
    FakeGame f(Character());
    State s = StateFor();
    Dress(f.AsGame(), s, Custom(2, 0, 0, 4), false);
    f.sets.clear();
    f.textures.clear();
    Dress(f.AsGame(), s, 0, false);   // nothing set, nothing put back: no call at all
    CHECK(f.sets.empty());
    CHECK(f.textures.empty());
    Dress(f.AsGame(), s, 0, true);
    REQUIRE(f.sets.size() == 3);
    CHECK(f.sets[0].f[0] == doctest::Approx(0.1));   // the file's own colour 0
    CHECK(f.sets[2].f[0] == doctest::Approx(0.3));   // the iris's own
    REQUIRE(f.textures.size() == 2);                 // the hair and Mt_Cloth back to their own first texture
    CHECK(f.textures[0].view == FakeGame::View(0, 0));
    CHECK(f.textures[1].view == FakeGame::View(2, 0));
}

TEST_CASE("char colours: a hair stand-in without a palette keeps its colours, a model not there is not dressed") {
    auto mats = Character();
    mats[0].textures = 2;   // Mt_Hairs without the mod's palette: a stand-in
    FakeGame f(mats);
    State s = StateFor();
    Dress(f.AsGame(), s, Custom(2, 0, 0, 0), false);
    CHECK(f.sets.empty());
    FakeGame other(Character());
    other.W64(FakeGame::kCompVt + 0xf8, 0x1234);   // not the player's model kind
    State t = StateFor();
    const Result r = Dress(other.AsGame(), t, Custom(2, 3, 0, 4), false);
    CHECK(r.units == 0);
    CHECK(r.calls == 0);
}

TEST_CASE("char colours: the wardrobe dresses on a change, again after kAgainMs, every frame while the viewer's paint") {
    FakeGame f(Character());
    Wardrobe w;
    const Game g = f.AsGame();
    std::vector<std::string> said;
    const auto say = [&](const std::string& l) { said.push_back(l); };
    std::vector<Target> t{{.actor = FakeGame::kActor, .cell = FakeGame::kCell, .custom = Custom(2, 0, 0, 0), .model = 1}};
    CHECK(w.Frame(g, t, 1000, say) > 0);
    CHECK(said.size() == 1);
    CHECK(w.Frame(g, t, 1100, say) == 0);
    CHECK(w.Frame(g, t, 1000 + Wardrobe::kAgainMs, say) > 0);
    t[0].custom = 0;   // back to the model's own: put back once
    f.sets.clear();
    CHECK(w.Frame(g, t, 5000, say) > 0);
    CHECK(f.sets[0].f[0] == doctest::Approx(0.1));
    CHECK(w.Frame(g, t, 9000, say) == 0);
    // The viewer's own options are colours: the mod paints them on every model, so every frame, Link puppets too.
    w.SetViewer(Custom(5, 0, 0, 0));
    CHECK(w.Frame(g, t, 9001, say) > 0);
    CHECK(w.Frame(g, t, 9002, say) > 0);
    // The mod's own verified routine dresses it: left alone.
    t[0].mods_routine = true;
    CHECK(w.Frame(g, t, 9003, say) == 0);
}

TEST_CASE("char colours: a failed call stops the dressing and is tried again later") {
    FakeGame f(Character());
    Game g = f.AsGame();
    g.call = [](std::uint64_t, const std::array<std::uint64_t, 8>&) { return std::optional<std::uint64_t>(); };
    State s = StateFor();
    const Result r = Dress(g, s, Custom(2, 0, 0, 0), false);
    CHECK(r.failed);
    CHECK(r.calls == 1);
    CHECK_FALSE(s.units[0].looked_up);
}

TEST_CASE("char colours: a failed call waits kAgainMs and is said once (review M2)") {
    FakeGame f(Character());
    Wardrobe w;
    Game g = f.AsGame();
    bool fail = true;
    const auto real = g.call;
    g.call = [&](std::uint64_t fn, const std::array<std::uint64_t, 8>& x) -> std::optional<std::uint64_t> {
        if (fail) return std::nullopt;
        return real(fn, x);
    };
    std::vector<std::string> said;
    const auto say = [&](const std::string& l) { said.push_back(l); };
    std::vector<Target> t{{.actor = FakeGame::kActor, .cell = FakeGame::kCell, .custom = Custom(2, 0, 0, 0), .model = 1}};
    w.SetViewer(Custom(5, 0, 0, 0));   // the viewer paints: due every frame, still held back after a failure
    CHECK(w.Frame(g, t, 1000, say) == 1);
    CHECK(w.Frame(g, t, 1016, say) == 0);
    CHECK(w.Frame(g, t, 1000 + Wardrobe::kAgainMs - 1, say) == 0);
    CHECK(w.Frame(g, t, 1000 + Wardrobe::kAgainMs, say) == 1);   // tried again, failed again
    CHECK(said.size() == 1);                                     // said once
    fail = false;
    CHECK(w.Frame(g, t, 1000 + 2 * Wardrobe::kAgainMs, say) > 1);
    CHECK_FALSE(f.sets.empty());
}

TEST_CASE("char colours: a puppet made again as another actor looks its materials up anew (review M3)") {
    FakeGame f(Character());
    Wardrobe w;
    const Game g = f.AsGame();
    std::vector<Target> t{{.actor = FakeGame::kActor, .cell = FakeGame::kCell, .custom = Custom(2, 0, 0, 0), .model = 1}};
    w.Frame(g, t, 1000, {});
    // The same address and block, another character with fewer materials: Mt_Hairs is now number 3 of 4.
    f.mats = {{"Mt_Skin", 1, 1, {}}, {"Mt_Eye_Inner", 1, 1, {"x", "y", "ns_param_color2"}}, {"Mt_A", 1, 1, {}},
              {"Mt_Hairs", 2, 11, {"ns_param_color0", "ns_param_color1"}}};
    FakeGame fresh(f.mats);
    f.mem = fresh.mem;
    f.sets.clear();
    t[0].model = 2;
    w.Frame(g, t, 1100, {});
    REQUIRE_FALSE(f.sets.empty());
    CHECK(f.sets[0].mat == 3);
}

TEST_CASE("char colours: a material number the unit does not have is never used") {
    FakeGame f(Character());
    Game g = f.AsGame();
    const auto real = g.call;
    g.call = [&](std::uint64_t fn, const std::array<std::uint64_t, 8>& x) -> std::optional<std::uint64_t> {
        if (fn == FakeGame::kFnCount) return 1;   // only material 0: the others' numbers are past the list
        return real(fn, x);
    };
    State s = StateFor();
    Dress(g, s, Custom(2, 3, 0, 4), false);
    for (const auto& set : f.sets) CHECK(set.mat == 0);
    for (const auto& tex : f.textures) CHECK(tex.object == FakeGame::Obj(0));
}

TEST_CASE("char colours: the call budget stops a dressing, the next frame goes on, the first target turns (review S2)") {
    FakeGame f(Character());
    State s = StateFor();
    const Result cut = Dress(f.AsGame(), s, Custom(2, 3, 0, 4, 1), false, 0, 3);
    CHECK(cut.cut);
    CHECK(cut.calls == 3);
    CHECK_FALSE(s.units[0].looked_up);
    const Result rest = Dress(f.AsGame(), s, Custom(2, 3, 0, 4, 1), false);
    CHECK_FALSE(rest.cut);
    CHECK(s.units[0].looked_up);
    // (the three palettes of the full dressing above: the hair, Clothing Color 1 and 2; with Clothing Color 2 at 0
    // Mt_Vest keeps its own texture and only two are set)
    CHECK(f.textures.size() == 3);
}

TEST_CASE("char colours (0.11.9): co-op's exact eye colour as the iris parameter's three moves") {
    using CoopCharColours::EyeParam;
    // The common iris's own colour: no move.
    const auto same = EyeParam(0x2E5BA8);   // (hue 0.608, about its saturation and brightness)
    CHECK(same[0] == doctest::Approx(0.5f).epsilon(0.02));
    // Green (hue 1/3): 0.5 + (0.333 - 0.608) / 2.
    const auto green = EyeParam(0x00FF00);
    CHECK(green[0] == doctest::Approx(0.5f + (1.0f / 3.0f - CoopCharColours::kIrisHue) / 2.0f));
    CHECK(green[1] == doctest::Approx(0.5f + (1.0f - CoopCharColours::kIrisSaturation) / 2.0f));
    CHECK(green[2] == doctest::Approx(0.5f + (1.0f - CoopCharColours::kIrisBrightness) / 2.0f));
    // A grey keeps the iris's hue, less saturated; black and white stay within 0..1.
    const auto grey = EyeParam(0x808080);
    CHECK(grey[0] == 0.5f);
    CHECK(grey[1] < 0.5f);
    for (std::uint32_t c : {0x000000u, 0xFFFFFFu, 0xFF0000u, 0xFF00FFu}) {
        for (float v : EyeParam(c)) {
            CHECK(v >= 0.0f);
            CHECK(v <= 1.0f);
        }
    }
    // As options: an exact eye colour is a colour to put on (Colours), over the mod's eye option.
    CoopCharColours::Options o;
    CHECK_FALSE(o.Colours());
    o.eyes_exact = EyeParam(0x00FF00);
    CHECK(o.Colours());
}

TEST_CASE("char colours (0.11.9): co-op's hair colour as the hair parameters (linear), colour 1 lighter, brows darker") {
    using CoopCharColours::HairParam;
    const auto h = HairParam(0xFF8000);
    CHECK(h[0][0] == doctest::Approx(1.0f));
    CHECK(h[0][1] == doctest::Approx(0.2158f).epsilon(0.01));   // (0x80 linear)
    CHECK(h[0][2] == doctest::Approx(0.0f));
    CHECK(h[1][0] == doctest::Approx(1.0f));                    // (lighter, not past 1)
    CHECK(h[1][1] == doctest::Approx(0.2158f * CoopCharColours::kHairLighter).epsilon(0.01));
    CHECK(h[2][1] == doctest::Approx(0.2158f * CoopCharColours::kBrowScale).epsilon(0.01));
    CoopCharColours::Options o;
    o.hair_exact = h;
    CHECK(o.Colours());
}

TEST_CASE("char colours (row 207): whether Character Select is in this game, as the hook last read it") {
    CHECK_FALSE(CoopCharColours::ModPresent());
    CoopCharColours::SetModPresent(true);
    CHECK(CoopCharColours::ModPresent());
    CoopCharColours::SetModPresent(false);
    CHECK_FALSE(CoopCharColours::ModPresent());
}

TEST_CASE("char colours (npc-customize-2): your own pick, read solo or in a session, is the character shown") {
    using CoopCharColours::NoteOwnPick;
    constexpr std::uint16_t kMax = 0x0FFF;   // (CoopLookShare::kCharacterMax)
    // Every pick Character Select can store (its 117 characters, and the rest of the numbers a save may hold).
    for (std::int32_t n = 1; n <= kMax; ++n) {
        CHECK(NoteOwnPick(n, kMax) == n);
        CHECK(CoopCharColours::ShownCharacter() == n);
        CHECK(CoopCharColours::ModPresent());
    }
    // Link, or a number no character has: Link, and the mod is there (its Int is).
    for (const std::int32_t n : {0, -1, int{kMax} + 1, 0x7FFFFFFF}) {
        CHECK(NoteOwnPick(n, kMax) == 0);
        CHECK(CoopCharColours::ShownCharacter() == 0);
        CHECK(CoopCharColours::ModPresent());
    }
    // No such Int: no Character Select in this game.
    NoteOwnPick(2, kMax);
    CHECK(NoteOwnPick(std::nullopt, kMax) == 0);
    CHECK(CoopCharColours::ShownCharacter() == 0);
    CHECK_FALSE(CoopCharColours::ModPresent());
}

TEST_CASE("char colours (npc-customize-2): a model's rows: what it has, only Equipment for none, unknown until all is read") {
    constexpr std::uint8_t kSaria = kRowHair | kRowEyes | kRowCloth1 | 8 | kRowGear | kRowsKnown;
    constexpr std::uint8_t kGearOnly = kRowGear | kRowsKnown;
    FakeGame saria(Character());
    State s = StateFor();
    CHECK(Rows(s) == 0);   // (not looked up: the page shows every row)
    Learn(saria.AsGame(), s);
    CHECK(Rows(s) == kSaria);
    // A model with nothing colourable (Cow: no hair, eye or clothing palette material): Equipment only, known.
    FakeGame cow({{"Mt_Body", 1, 1, {}}, {"Mt_Horn", 1, 1, {}}});
    State c = StateFor();
    Learn(cow.AsGame(), c);
    CHECK(Rows(c) == kGearOnly);
    // A model the mod's check refuses (not the player model's kind): none of its options go on it, Equipment only.
    FakeGame other(Character());
    other.W64(FakeGame::kCompVt + 0xf8, 0x1234);
    State o = StateFor();
    Learn(other.AsGame(), o);
    CHECK(Rows(o) == kGearOnly);
    // Part of a model read (the call budget ran out): not known yet.
    State part = StateFor();
    part.units.resize(2);
    part.units[0].unit = 0x100, part.units[0].looked_up = true;
    part.units[1].unit = 0x200;
    CHECK(Rows(part) == 0);
}

TEST_CASE("char colours (npc-customize-2): the page's rows are the Customize picture's, else your model's, looked up again") {
    constexpr std::uint16_t kCow = 113;
    FakeGame f(Character());
    // The picture: the menu's model of your pick, another actor, a model of another kind (the Cow's picture here).
    constexpr std::uint64_t kPicture = 0x40000, kPicComps = 0x41000, kPicComponent = 0x42000, kPicVt = 0x43000;
    f.W64(kPicture + kActorComponents, kPicComps);
    f.W64(kPicComps + 8, kPicComponent);
    f.W64(kPicComponent, kPicVt);
    f.W64(kPicVt + 0xf8, 0x1234);
    Wardrobe w;
    const Game g = f.AsGame();
    Target field{.actor = FakeGame::kActor, .cell = FakeGame::kCell, .model = 1};
    field.own = true, field.character = kCow;
    // The field model is still the earlier pick's (Saria's materials) while the menu shows the Cow: its rows are not
    // the Cow's page.
    w.Frame(g, {field}, 1000, {});
    CHECK(w.OwnRows(kCow) == (kRowHair | kRowEyes | kRowCloth1 | 8 | kRowGear | kRowsKnown));
    Target picture{.actor = kPicture, .cell = FakeGame::kCell + 0x1000, .model = 2};
    picture.character = kCow, picture.picture = true;
    w.Frame(g, {field, picture}, 1001, {});
    CHECK(w.OwnRows(kCow) == (kRowGear | kRowsKnown));
    // The menu closes and the field model becomes the Cow's (other units, nothing colourable): looked up again.
    f.mats = {{"Mt_Body", 1, 1, {}}};
    constexpr std::uint64_t kCowUnit = 0x15800;
    f.W64(FakeGame::kUnitPtr, kCowUnit);
    f.W64(kCowUnit, FakeGame::kUnitVt);
    f.W64(kCowUnit + 0x108, FakeGame::kObjs);
    w.Frame(g, {field}, 1002, {});
    CHECK(w.OwnRows(kCow) == (kRowGear | kRowsKnown));
    // Another pick's number: none of these rows.
    CHECK(w.OwnRows(2) == 0);
}

TEST_CASE("char colours: the glue's targets carry the model key in `model`, the colours in `eyes` and `hair`") {
    // (Geno's review of dress-colours, kept at the npc-customize merge: a positional Target{actor, cell, custom,
    // routine, 0, key} put the key into `eyes` and left `model` 0, so no age, character or puppet switch was ever
    // looked up anew; ForModel's colours go in by name)
    const ModelColours c{.custom = 7, .eyes = 0x00FF00u, .hair = 0xFF0000u};
    const Target p = PuppetTarget(0x100, 0x200, true, "CoopPuppet_Adult_C007", c);
    CHECK(p.actor == 0x100u);
    CHECK(p.cell == 0x200u);
    CHECK(p.custom == 7u);
    CHECK(p.mods_routine);
    CHECK(p.iris_shift == 0);
    CHECK(p.eyes == std::optional<std::uint32_t>(0x00FF00u));
    CHECK(p.hair == std::optional<std::uint32_t>(0xFF0000u));
    CHECK(p.model == ModelKey("CoopPuppet_Adult_C007", 0));
    CHECK(p.model != 0);
    CHECK_FALSE(p.own);
    CHECK_FALSE(p.picture);
    CHECK(PuppetTarget(0x100, 0x200, true, "CoopPuppet_Child_C007", c).model != p.model);   // (age)
    CHECK(PuppetTarget(0x100, 0x200, true, "CoopPuppet_Adult_C012", c).model != p.model);   // (character)
    const Target o = OwnTarget(0x300, 0x400, ModelKey("Player_Adult", 9), {.custom = 5, .eyes = 0x123456u}, 9, true,
                               false);
    CHECK(o.actor == 0x300u);
    CHECK(o.cell == 0x400u);
    CHECK(o.custom == 5u);
    CHECK_FALSE(o.mods_routine);
    CHECK(o.eyes == std::optional<std::uint32_t>(0x123456u));
    CHECK_FALSE(o.hair);
    CHECK(o.model == ModelKey("Player_Adult", 9));
    CHECK(o.own);
    CHECK(o.character == 9);
    CHECK_FALSE(o.picture);
    CHECK(ModelKey("Player_Adult", 9) != ModelKey("Player_Adult", 2));   // (another pick: another model)
    CHECK(ModelKey("Player_Adult", 9) != ModelKey("Player_Child", 9));   // (another age)
}

TEST_CASE("char colours: a model made again as another actor at the same address is looked up and dressed at once") {
    FakeGame f(Character());
    Wardrobe w;
    const Game g = f.AsGame();
    const ModelColours c{.eyes = 0x00FF00u};
    CHECK(w.Frame(g, {PuppetTarget(FakeGame::kActor, FakeGame::kCell, false, "CoopPuppet_Child_C007", c)}, 1000, {}) > 0);
    const std::size_t looked = f.calls;
    f.sets.clear();
    CHECK(w.Frame(g, {PuppetTarget(FakeGame::kActor, FakeGame::kCell, false, "CoopPuppet_Child_C007", c)}, 1100, {}) ==
          0);   // (the same model: not before kAgainMs)
    // Its player switched age: another actor at the same address, before kAgainMs.
    CHECK(w.Frame(g, {PuppetTarget(FakeGame::kActor, FakeGame::kCell, false, "CoopPuppet_Adult_C007", c)}, 1200, {}) > 0);
    CHECK(f.calls - looked > 1);   // (its materials looked up again, then the iris set)
    CHECK(f.sets.size() == 1);
}

TEST_CASE("char colours: your own model after a pick switch at the same address starts anew (ModelKey of the pick)") {
    // (the glue's own target: OwnTarget with ModelKey(actor name, character shown); with the key left 0 the kept
    // slot of the earlier pick stayed and its material numbers were used on the new model)
    FakeGame f(Character());
    Wardrobe w;
    const Game g = f.AsGame();
    const ModelColours c{.eyes = 0x00FF00u};
    const auto own = [&](std::uint16_t character) {
        return OwnTarget(FakeGame::kActor, FakeGame::kCell, ModelKey("Player_Child", character), c, character, true,
                         false);
    };
    CHECK(w.Frame(g, {own(2)}, 1000, {}) > 0);
    const std::size_t looked = f.calls;
    f.sets.clear();
    CHECK(w.Frame(g, {own(2)}, 1100, {}) == 0);   // (the same pick: not before kAgainMs)
    CHECK(w.Frame(g, {own(8)}, 1200, {}) > 0);    // (another pick, same actor address: looked up and dressed at once)
    CHECK(f.calls - looked > 1);
    CHECK(f.sets.size() == 1);
}
