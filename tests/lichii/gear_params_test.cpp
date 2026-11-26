// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../lichii/hook/coop_gear_params.h"

using namespace CoopGearParams;
namespace CC = CoopCharColours;

namespace {

// A guest with one actor wearing pieces (its EquipmentUser's handles), each piece a model of one unit whose virtual
// functions are fakes the calls land in (the layout CoopCharColours' tests use for the player's model).
struct FakeGame {
    std::map<std::uint64_t, std::uint8_t> mem;
    static constexpr std::uint64_t kActor = 0x10000, kComps = 0x11000, kEquipment = 0x12000, kCell = 0x20000;
    static constexpr std::uint64_t kFnCount = 0xF1, kFnByName = 0xF2, kFnParam = 0xF3, kFnSet = 0xF4;
    struct Mat {
        std::string name;
        std::vector<std::string> params;
    };
    // The Master Sword's three materials (Model/Weapon_Sword_Master), the grip's second.
    std::vector<Mat> mats = {
        {"Mt_Weapon_Sword_Master_Metal", {"ns_param_color1", "ns_param_color0"}},
        {"Mt_Weapon_Sword_Master", {"ns_param_scalar0", "ns_param_color1", "ns_param_color0"}},
        {"Mt_Weapon_Sword_Master_Coat", {"ns_param_color0"}},
    };
    struct Set {
        std::uint64_t unit;
        std::int64_t mat;
        std::uint64_t param;
        std::array<float, 4> f;
    };
    std::vector<Set> sets;
    std::size_t calls = 0;
    bool fail = false;

    void W64(std::uint64_t a, std::uint64_t v) {
        for (int i = 0; i < 8; i++) mem[a + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
    void W(std::uint64_t a, const void* p, std::size_t n) {
        for (std::size_t i = 0; i < n; i++) mem[a + i] = static_cast<const std::uint8_t*>(p)[i];
    }
    void W32(std::uint64_t a, std::uint32_t v) { W(a, &v, 4); }
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
    static std::uint64_t Piece(std::size_t k) { return 0x100000 + 0x10000 * k; }
    // Every piece's materials are those of `mats`; the units differ by piece (and by `generation`: made again).
    static std::uint64_t Unit(std::size_t k, std::uint64_t generation) { return 0x800000 + 0x10000 * k + generation; }

    FakeGame() { W64(kActor + CC::kActorComponents, kComps), W64(kComps + CC::kActorEquipment, kEquipment); }

    // Handle k holds a piece of this name (state: the piece's +0x2c).
    void Wear(std::size_t k, const std::string& name, std::uint32_t state = 3, std::uint64_t generation = 0) {
        const std::uint64_t handle = kEquipment + CC::kGearFirst + CC::kGearStride * k;
        const std::uint64_t link = 0x50000 + 0x100 * k, p = Piece(k), base = p + 0x1000;
        const std::uint32_t id = 0x100 + static_cast<std::uint32_t>(k);
        W32(handle + 0x10, id), W64(handle + 0x8, link), W32(link + 0x48, id), W64(link + 0x40, p);
        W32(p + 0x2c, state);
        W64(p + 0x260, base);
        W(base, name.c_str(), name.size() + 1);
        // the model component (component table +8), its model's one unit
        W64(p + CC::kActorComponents, base + 0x100), W64(base + 0x100 + 8, base + 0x200);
        W64(base + 0x200 + 0x18, base + 0x300);
        const std::int32_t one = 1;
        W(base + 0x300 + 0x20, &one, 4), W64(base + 0x300 + 0x28, base + 0x400), W64(base + 0x400, base + 0x500);
        const std::uint64_t unit = Unit(k, generation), vt = unit + 0x8000;
        W64(base + 0x500, unit), W64(unit, vt);
        W64(vt + CC::kUnitMaterialCount, kFnCount), W64(vt + CC::kUnitMaterialByName, kFnByName);
        W64(vt + CC::kUnitParamByName, kFnParam), W64(vt + CC::kUnitSetFloats, kFnSet);
        W64(unit + 0x108, unit + 0x1000);
        for (std::size_t i = 0; i < mats.size(); i++) {
            const std::uint64_t obj = unit + 0x1000 + 0x80 * i, rec = unit + 0x2000 + 0x1000 * i;
            W64(obj, rec);
            W64(rec + 0x10, rec + 0x700), W64(rec + 0x700, rec + 0x800), W64(rec + 0x800 + 0x20, rec + 0x900);
            W64(rec + 0x58, rec + 0xA00);
            for (std::size_t q = 0; q < mats[i].params.size(); q++) {
                const auto off = static_cast<std::uint16_t>(16 * q);
                W(rec + 0x900 + 0x18 * q + 0x10, &off, 2);
                // the file's own: the grip's colour (0.022, 0.318, 0.402) and 1
                const std::array<float, 4> f{0.022f, 0.318f, 0.402f, 1.0f};
                W(rec + 0xA00 + off, f.data(), 16);
            }
        }
    }
    void Remove(std::size_t k) { W32(kEquipment + CC::kGearFirst + CC::kGearStride * k + 0x10, 0xFFFFFFFFu); }

    CC::Game AsGame() {
        CC::Game g;
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
            if (fail) return std::nullopt;
            if (f == kFnCount) return mats.size();
            if (f == kFnByName) {
                const std::string name = Str(R64(x[1]));
                for (std::size_t i = 0; i < mats.size(); i++) {
                    if (mats[i].name == name) return i;
                }
                return 0xFFFF;
            }
            if (f == kFnParam) {
                const std::string name = Str(R64(x[2]));
                const auto& ps = mats[x[1]].params;
                for (std::size_t q = 0; q < ps.size(); q++) {
                    if (ps[q] == name) return q;
                }
                return 0xFFFF;
            }
            if (f == kFnSet) {
                Set s{x[0], static_cast<std::int64_t>(x[1]), x[2], {}};
                std::array<std::uint8_t, 16> b{};
                for (std::size_t i = 0; i < b.size(); i++) b[i] = mem[x[3] + i];
                std::memcpy(s.f.data(), b.data(), b.size());
                sets.push_back(s);
                return 0;
            }
            return std::nullopt;
        };
        return g;
    }
};

Target Wearer(std::optional<std::uint32_t> hilt) {
    Target t;
    t.actor = FakeGame::kActor;
    t.colours[0] = hilt;
    return t;
}

}  // namespace

TEST_CASE("gear params (row 277): the Master Sword's hilt is its grip material's ns_param_color0") {
    REQUIRE(kPartCount == 1);
    CHECK(kParts[0].part == 12);   // CoopLook::Part::master_hilt
    CHECK(std::string(kParts[0].material) == "Mt_Weapon_Sword_Master");
    CHECK(std::string(kParts[0].param) == "ns_param_color0");
}

TEST_CASE("gear params: the game's sword and its co-op copies wear the material, Dark Link's and other items not") {
    const auto& p = kParts[0];
    CHECK(WearsPart("Weapon_Sword_Master", p));
    CHECK(WearsPart("Weapon_Sword_Master_Look3", p));
    CHECK(WearsPart("Weapon_Sword_Master_Blade12", p));
    CHECK_FALSE(WearsPart("Weapon_Sword_Master_Dark", p));
    CHECK_FALSE(WearsPart("Weapon_Sword_Kokiri", p));
    CHECK_FALSE(WearsPart("Weapon_Sword_Maste", p));
    CHECK_FALSE(WearsPart("", p));
}

TEST_CASE("gear params: a colour as the parameter's linear floats") {
    const auto white = Linear(0xFFFFFF), black = Linear(0x000000), grey = Linear(0x808080);
    for (int k = 0; k < 3; k++) {
        CHECK(white[static_cast<std::size_t>(k)] == doctest::Approx(1.0f));
        CHECK(black[static_cast<std::size_t>(k)] == doctest::Approx(0.0f));
        CHECK(grey[static_cast<std::size_t>(k)] == doctest::Approx(0.2158f).epsilon(0.001));
    }
    // (the game's own grip, linear 0.022 0.318 0.402, is about sRGB #2999AA)
    const auto grip = Linear(0x2999AA);
    CHECK(grip[0] == doctest::Approx(0.022f).epsilon(0.05));
    CHECK(grip[1] == doctest::Approx(0.318f).epsilon(0.01));
    CHECK(grip[2] == doctest::Approx(0.402f).epsilon(0.01));
    const auto red = Linear(0xFF0000);
    CHECK(red[0] == doctest::Approx(1.0f));
    CHECK(red[1] == doctest::Approx(0.0f));
}

TEST_CASE("gear params: the hilt's colour goes on the equipped sword's grip material, the alpha stays the file's") {
    FakeGame fg;
    fg.Wear(2, "Weapon_Sword_Master");
    fg.Wear(4, "Weapon_Shield_Hylian");
    const CC::Game g = fg.AsGame();
    Dresser d;
    std::vector<std::string> said;
    d.Frame(g, FakeGame::kCell, {Wearer(0xFF0000)}, 1000, [&](const std::string& s) { said.push_back(s); });
    REQUIRE(fg.sets.size() == 1);
    CHECK(fg.sets[0].unit == FakeGame::Unit(2, 0));
    CHECK(fg.sets[0].mat == 1);     // Mt_Weapon_Sword_Master (not _Metal, not _Coat)
    CHECK(fg.sets[0].param == 2);   // its ns_param_color0
    CHECK(fg.sets[0].f[0] == doctest::Approx(1.0f));
    CHECK(fg.sets[0].f[1] == doctest::Approx(0.0f));
    CHECK(fg.sets[0].f[2] == doctest::Approx(0.0f));
    CHECK(fg.sets[0].f[3] == doctest::Approx(1.0f));
    REQUIRE(said.size() == 1);
    CHECK(said[0].find("Weapon_Sword_Master wears Mt_Weapon_Sword_Master.ns_param_color0") != std::string::npos);
}

TEST_CASE("gear params: nothing chosen is no call, the same colour again only after kAgainMs") {
    FakeGame fg;
    fg.Wear(2, "Weapon_Sword_Master_Blade3");
    const CC::Game g = fg.AsGame();
    Dresser d;
    CHECK(d.Frame(g, FakeGame::kCell, {Wearer(std::nullopt)}, 1000, nullptr) == 0);
    CHECK(fg.calls == 0);
    d.Frame(g, FakeGame::kCell, {Wearer(0x00FF00)}, 1000, nullptr);
    REQUIRE(fg.sets.size() == 1);
    const std::size_t after_first = fg.calls;
    d.Frame(g, FakeGame::kCell, {Wearer(0x00FF00)}, 1000 + Dresser::kAgainMs - 1, nullptr);
    CHECK(fg.calls == after_first);   // looked up once: no call at all
    d.Frame(g, FakeGame::kCell, {Wearer(0x00FF00)}, 1000 + Dresser::kAgainMs, nullptr);
    CHECK(fg.sets.size() == 2);
    CHECK(fg.calls == after_first + 1);   // the set alone
    d.Frame(g, FakeGame::kCell, {Wearer(0x0000FF)}, 1000 + Dresser::kAgainMs + 1, nullptr);
    REQUIRE(fg.sets.size() == 3);
    CHECK(fg.sets[2].f[2] == doctest::Approx(1.0f));
}

TEST_CASE("gear params: a colour taken away puts the file's own back once") {
    FakeGame fg;
    fg.Wear(0, "Weapon_Sword_Master");
    const CC::Game g = fg.AsGame();
    Dresser d;
    d.Frame(g, FakeGame::kCell, {Wearer(0xFFFFFF)}, 1000, nullptr);
    d.Frame(g, FakeGame::kCell, {Wearer(std::nullopt)}, 1001, nullptr);
    REQUIRE(fg.sets.size() == 2);
    CHECK(fg.sets[1].f[0] == doctest::Approx(0.022f));
    CHECK(fg.sets[1].f[1] == doctest::Approx(0.318f));
    CHECK(fg.sets[1].f[2] == doctest::Approx(0.402f));
    CHECK(fg.sets[1].f[3] == doctest::Approx(1.0f));
    const std::size_t calls = fg.calls;
    d.Frame(g, FakeGame::kCell, {Wearer(std::nullopt)}, 5000, nullptr);
    CHECK(fg.calls == calls);
}

TEST_CASE("gear params: a sword made again (another unit) is looked up and coloured at once") {
    FakeGame fg;
    fg.Wear(2, "Weapon_Sword_Master");
    const CC::Game g = fg.AsGame();
    Dresser d;
    d.Frame(g, FakeGame::kCell, {Wearer(0xFF0000)}, 1000, nullptr);
    fg.Wear(2, "Weapon_Sword_Master", 3, 0x100);   // (the same handle, the piece's model made again)
    d.Frame(g, FakeGame::kCell, {Wearer(0xFF0000)}, 1001, nullptr);
    REQUIRE(fg.sets.size() == 2);
    CHECK(fg.sets[1].unit == FakeGame::Unit(2, 0x100));
}

TEST_CASE("gear params: other items, Dark Link's sword and a piece being made or deleted are left alone") {
    FakeGame fg;
    fg.Wear(1, "Weapon_Sword_Master_Dark");
    fg.Wear(3, "Weapon_Sword_Kokiri");
    fg.Wear(5, "Weapon_Sword_Master", 9);   // (state 7..11: being made or deleted)
    const CC::Game g = fg.AsGame();
    Dresser d;
    d.Frame(g, FakeGame::kCell, {Wearer(0xFF0000)}, 1000, nullptr);
    CHECK(fg.calls == 0);
    CHECK(fg.sets.empty());
    fg.Wear(5, "Weapon_Sword_Master", 3);   // made: now worn
    d.Frame(g, FakeGame::kCell, {Wearer(0xFF0000)}, 1001, nullptr);
    CHECK(fg.sets.size() == 1);
    fg.Remove(5);   // unequipped: nothing more
    const std::size_t calls = fg.calls;
    d.Frame(g, FakeGame::kCell, {Wearer(0xFF0000)}, 5000, nullptr);
    CHECK(fg.calls == calls);
}

TEST_CASE("gear params: a model without the material is said once and never called again") {
    FakeGame fg;
    fg.mats = {{"Mt_Weapon_Sword_Master_Metal", {"ns_param_color0"}}};
    fg.Wear(2, "Weapon_Sword_Master");
    const CC::Game g = fg.AsGame();
    Dresser d;
    std::vector<std::string> said;
    const auto say = [&](const std::string& s) { said.push_back(s); };
    d.Frame(g, FakeGame::kCell, {Wearer(0xFF0000)}, 1000, say);
    const std::size_t calls = fg.calls;
    d.Frame(g, FakeGame::kCell, {Wearer(0xFF0000)}, 5000, say);
    CHECK(fg.calls == calls);
    CHECK(fg.sets.empty());
    REQUIRE(said.size() == 1);
    CHECK(said[0].find("has no Mt_Weapon_Sword_Master.ns_param_color0") != std::string::npos);
}

TEST_CASE("gear params: a failed call waits kAgainMs, said once, no cell is nothing") {
    FakeGame fg;
    fg.Wear(2, "Weapon_Sword_Master");
    const CC::Game g = fg.AsGame();
    Dresser d;
    CHECK(d.Frame(g, 0, {Wearer(0xFF0000)}, 1000, nullptr) == 0);
    CHECK(fg.calls == 0);
    std::vector<std::string> said;
    const auto say = [&](const std::string& s) { said.push_back(s); };
    fg.fail = true;
    d.Frame(g, FakeGame::kCell, {Wearer(0xFF0000)}, 1000, say);
    CHECK(fg.calls == 1);
    d.Frame(g, FakeGame::kCell, {Wearer(0xFF0000)}, 1000 + Dresser::kAgainMs - 1, say);
    CHECK(fg.calls == 1);
    fg.fail = false;
    d.Frame(g, FakeGame::kCell, {Wearer(0xFF0000)}, 1000 + Dresser::kAgainMs, say);
    CHECK(fg.sets.size() == 1);
    CHECK(said.size() == 2);   // the failure once, then the material found
}

TEST_CASE("gear params: each wearer's own colour, your Link and a puppet in one frame") {
    FakeGame fg;
    fg.Wear(2, "Weapon_Sword_Master");
    // a second wearer: a puppet with its own equipment and its player's sword copy
    constexpr std::uint64_t kPuppet = 0x60000, kPComps = 0x61000, kPEquip = 0x62000;
    fg.W64(kPuppet + CC::kActorComponents, kPComps), fg.W64(kPComps + CC::kActorEquipment, kPEquip);
    fg.Wear(7, "Weapon_Sword_Master_Look2");   // (handle 7 of the fake's actor, moved below to the puppet's)
    const std::uint64_t from = FakeGame::kEquipment + CC::kGearFirst + CC::kGearStride * 7;
    const std::uint64_t to = kPEquip + CC::kGearFirst + CC::kGearStride * 7;
    for (std::uint64_t i = 0; i < CC::kGearStride; i++) fg.mem[to + i] = fg.mem[from + i];
    fg.Remove(7);
    const CC::Game g = fg.AsGame();
    Dresser d;
    Target puppet;
    puppet.actor = kPuppet;
    puppet.colours[0] = 0x0000FF;
    d.Frame(g, FakeGame::kCell, {Wearer(0xFF0000), puppet}, 1000, nullptr);
    REQUIRE(fg.sets.size() == 2);
    CHECK(fg.sets[0].unit == FakeGame::Unit(2, 0));
    CHECK(fg.sets[0].f[0] == doctest::Approx(1.0f));
    CHECK(fg.sets[1].unit == FakeGame::Unit(7, 0));
    CHECK(fg.sets[1].f[2] == doctest::Approx(1.0f));
    CHECK(fg.sets[1].f[0] == doctest::Approx(0.0f));
}
