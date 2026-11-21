// SPDX-License-Identifier: GPL-3.0-or-later
// Your Link's look changed in the pause menu (hookshot/src/coop_look.h) and
// its tunic rewritten in memory (coop_look_tunic.h).
#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <fstream>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "coop/protocol/codec.hpp"
#include "coop/protocol/messages.hpp"
#include "coop_colors.h"
#include "coop_look.h"
#include "coop_look_paint.h"
#include "coop_look_parts.h"
#include "coop_look_tunic.h"
#include "coop_settings.h"
#include "coop_title_menu.h"

using CoopLook::Look;
using CoopLook::Part;

namespace {

Look Tunic(std::optional<std::uint32_t> c) {
    Look l;
    l[Part::tunic] = c;
    return l;
}

// One block of the game's memory at `base` (anything else is not there), the
// textures the game made in it and Longshot's overrides of them, as
// host/coop_texture.c keeps them (patch 0020).
class FakeTextures final : public CoopLookTunic::Textures {
public:
    FakeTextures(std::uint64_t base, std::size_t size) : base_(base), bytes_(size) {}
    std::vector<CoopLookTunic::Live> List(std::uint64_t lo, std::uint64_t hi, std::uint64_t* generation) const override {
        std::vector<CoopLookTunic::Live> out;
        for (const auto& t : made_) {
            if (t.size >= lo && t.size <= hi) {
                out.push_back(t);
            }
        }
        if (generation != nullptr) {
            *generation = generation_;
        }
        return out;
    }
    bool Read(std::uint64_t a, void* out, std::size_t n) const override {
        if (!In(a, n)) {
            return false;
        }
        ++reads[a];
        std::memcpy(out, bytes_.data() + (a - base_), n);
        return true;
    }
    bool Override(std::uint64_t a, std::uint64_t serial, const void* in, std::size_t n) override {
        if (in == nullptr) {
            Drop(a, n);
            return true;
        }
        const bool inside = std::any_of(made_.begin(), made_.end(), [&](const auto& t) {
            return t.serial == serial && t.address <= a && a + n <= t.address + t.size;
        });
        if (!inside) {
            return false;
        }
        Drop(a, n);
        const auto* p = static_cast<const std::uint8_t*>(in);
        overrides_[a] = std::vector<std::uint8_t>(p, p + n);
        ++overridden;
        return true;
    }
    // The game makes a texture whose storage is [a, a + n) (its file already in memory).
    void Make(std::uint64_t a, std::uint64_t n) {
        Finalize(a);
        made_.push_back({a, n, ++serials_});
        ++generation_;
    }
    void Finalize(std::uint64_t a) {
        for (auto it = made_.begin(); it != made_.end(); ++it) {
            if (it->address == a) {
                Drop(it->address, it->size);
                made_.erase(it);
                ++generation_;
                return;
            }
        }
    }
    // What the renderer draws at a: the game's memory with the overrides over it.
    std::vector<std::uint8_t> Drawn(std::uint64_t a, std::size_t n) const {
        std::vector<std::uint8_t> out(bytes_.begin() + static_cast<std::ptrdiff_t>(a - base_),
                                      bytes_.begin() + static_cast<std::ptrdiff_t>(a - base_ + n));
        for (const auto& [at, o] : overrides_) {
            for (std::size_t i = 0; i < o.size(); ++i) {
                if (at + i >= a && at + i < a + n) {
                    out[at + i - a] = o[i];
                }
            }
        }
        return out;
    }
    std::uint8_t* at(std::uint64_t a) { return bytes_.data() + (a - base_); }
    std::size_t overrides() const { return overrides_.size(); }
    int overridden = 0;
    mutable std::map<std::uint64_t, int> reads;   // Read calls by address

private:
    void Drop(std::uint64_t a, std::uint64_t n) {
        for (auto it = overrides_.begin(); it != overrides_.end();) {
            it = it->first < a + n && a < it->first + it->second.size() ? overrides_.erase(it) : std::next(it);
        }
    }
    bool In(std::uint64_t a, std::size_t n) const { return a >= base_ && a + n <= base_ + bytes_.size(); }
    std::uint64_t base_;
    std::vector<std::uint8_t> bytes_;
    std::vector<CoopLookTunic::Live> made_;
    std::map<std::uint64_t, std::vector<std::uint8_t>> overrides_;
    std::uint64_t generation_ = 0, serials_ = 0;
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

// A puppet copy's texels as the pack writes them (look_live.py mark).
std::vector<std::uint8_t> Marked(std::vector<std::uint8_t> t, std::size_t copy) {
    CoopLookParts::Mark(t, copy);
    return t;
}

// A texture file's image at `address`: a header naming `name`, then the
// texels; and the game makes the texture over them.
void PutFile(FakeTextures& m, std::uint64_t address, const std::string& name, const std::vector<std::uint8_t>& texels) {
    std::uint8_t* p = m.at(address);
    std::memcpy(p, "BNTX", 4);
    std::memcpy(p + 0x40, name.c_str(), name.size() + 1);
    std::memcpy(p + kOffset - 16, "BRTD", 4);
    std::memcpy(p + kOffset, texels.data(), texels.size());
    m.Make(address + kOffset, texels.size());
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

// The renderer draws the texture at `at` in that seed's texels.
bool Drawn(const FakeTextures& m, std::uint64_t at, std::uint8_t seed) {
    return m.Drawn(at, kTexels) == TexelsOf(seed);
}
// The game's memory holds that seed's texels at `at`.
bool Holding(FakeTextures& m, std::uint64_t at, std::uint8_t seed) {
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
    CHECK(static_cast<int>(Part::cap_trim) == 17);
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

TEST_CASE("look tunic: a texture is known by its exact texels, with no file in front, a marked copy left alone") {
    FakeTextures m(kBase, 0x10000);
    const auto green = Colour({{"Tile_A", TexelsOf(1)}});
    std::memcpy(m.at(kBase + 0x3000), TexelsOf(1).data(), kTexels);
    m.Make(kBase + 0x3000, kTexels);   // your own: the game copied the texels into its texture memory
    std::memcpy(m.at(kBase + 0x4000), Marked(TexelsOf(1), 2).data(), kTexels);
    m.Make(kBase + 0x4000, kTexels);   // a puppet's copy 2: the same texels, marked
    m.Make(kBase + 0x20000, kTexels);  // a storage outside the readable memory

    const auto found = CoopLookTunic::Find(m, {green});
    REQUIRE(found.places.size() == 1);
    CHECK(found.places[0].address == kBase + 0x3000);
    CHECK(found.listed == 3);
    CHECK(found.unread == 1);
    CHECK(found.heads == 1);
}

TEST_CASE("look tunic: your Link's textures are found and drawn over, the game's memory and a puppet's copy left alone") {
    FakeTextures m(kBase, 0x10000);
    const auto green = Colour({{"Tile_A", TexelsOf(1)}, {"Vest", TexelsOf(2)}});
    const auto red = Colour({{"Tile_A", TexelsOf(9)}, {"Vest", TexelsOf(8)}});
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    PutFile(m, kBase + 0x3000, "Vest", TexelsOf(2));
    PutFile(m, kBase + 0x5000, "Tile_A_Red", Marked(TexelsOf(1), 1));   // a puppet's copy: marked
    m.at(kBase + 0x7000)[0] = 1;
    m.Make(kBase + 0x7000, kTexels);   // another texture of that size, other texels

    const auto found = CoopLookTunic::Find(m, {green, red});
    REQUIRE(found.places.size() == 2);
    CHECK(found.heads == 1);
    auto place = found.places[0];
    CHECK(place.address == kBase + 0x1000 + kOffset);
    CHECK((place.held == green));
    CHECK((place.drawn == green));
    REQUIRE(CoopLookTunic::Draw(m, place, red));
    CHECK((place.drawn == red));
    CHECK(place.overridden);
    CHECK(Drawn(m, place.address, 9));
    CHECK(Holding(m, place.address, 1));   // the game's memory: as it was
    CHECK(m.Drawn(kBase + 0x5000 + kOffset, kTexels) == Marked(TexelsOf(1), 1));   // the puppet's copy: as it was
    // Back to the game's own: the override taken away.
    REQUIRE(CoopLookTunic::Draw(m, place, green));
    CHECK_FALSE(place.overridden);
    CHECK(m.overrides() == 0);
    CHECK(Drawn(m, place.address, 1));
    // An override outside a texture the game has is refused.
    m.Finalize(place.address);
    CHECK_FALSE(CoopLookTunic::Draw(m, place, red));
    CHECK(m.overrides() == 0);
    // Another texture made over the same storage after the place was found (a late Draw): refused too.
    m.Make(place.address, kTexels);
    CHECK_FALSE(CoopLookTunic::Draw(m, place, red));
    CHECK(m.overrides() == 0);
}

TEST_CASE("look tunic (0.11.9 lane crash, a178e8bd): a Host with a part missing never throws out of the worker") {
    // The puppets' tunics' Host was copied from one already moved from: no file, no log. The worker's first Step
    // called the empty log (std::bad_function_call out of its thread: the game aborted, 0xc0000409, ~25 s in).
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    CoopLookTunic::Wardrobe::Host moved_from;   // (what a moved-from Host holds: no file, no log)
    moved_from.textures = &m;
    moved_from.base = "ptunics";
    CoopLookTunic::Wardrobe w(moved_from);
    w.Wear("ptunics|red=FF3030");   // (the lane's last line: "the puppets' tunics are ptunics|red=FF3030 now")
    w.Kick();
    CHECK_NOTHROW(w.Step(0));
    CHECK_NOTHROW(w.Step(1000));
    CHECK(Drawn(m, kBase + 0x1000 + kOffset, 1));   // (no file: nothing drawn, the game's own stays)
    CoopLookTunic::Wardrobe none(CoopLookTunic::Wardrobe::Host{});   // (no textures at all)
    none.Wear("red");
    CHECK_NOTHROW(none.Step(0));
}

TEST_CASE("look tunic: the wardrobe keeps your Link in the colour worn") {
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    std::vector<std::string> log;
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
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
    CHECK(Drawn(m, at, 9));
    CHECK(Holding(m, at, 1));
    // Nothing changed: nothing set again.
    const int set = m.overridden;
    w.Step(1000);
    CHECK(m.overridden == set);
    // The game makes the texture again (a scene change; its override went with the old one): red again.
    m.Make(at, kTexels);
    CHECK(Drawn(m, at, 1));
    w.Step(2000);
    CHECK(Drawn(m, at, 9));
    // A colour the pack does not have: the one worn stays, said once.
    w.Wear("teal");
    w.Step(3000);
    w.Step(4000);
    CHECK(Drawn(m, at, 9));
    CHECK(std::count_if(log.begin(), log.end(), [](const std::string& l) { return l.find("teal") != std::string::npos; }) == 1);
    // Back to green: the override taken away, then nothing more to do.
    w.Wear("green");
    w.Step(5000);
    CHECK(Drawn(m, at, 1));
    CHECK(m.overrides() == 0);
    CHECK_FALSE(w.Busy());
    // The module stops while red is worn: its override goes.
    w.Wear("red");
    w.Step(6000);
    CHECK(m.overrides() == 1);
    w.Clear();
    CHECK(m.overrides() == 0);
    CHECK(Drawn(m, at, 1));
    // A later start wears red again.
    w.Step(7000);
    CHECK(Drawn(m, at, 9));
}

TEST_CASE("look tunic: a texture the game makes again is drawn at the next step, the others not read again") {
    // 0.11.0 players: the game made Link's textures again (scene changes, cutscenes, its streaming)
    // and each time the game's colours showed until the next round, a second later: flicker.
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::uint64_t other = kBase + 0x3000 + kOffset;   // a texture of the size that is not Link's
    m.Make(other, kTexels);
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        return it == pack.end() ? std::nullopt : std::optional(it->second);
    };
    host.log = [](const std::string&) {};
    CoopLookTunic::Wardrobe w(host);
    const std::uint64_t at = kBase + 0x1000 + kOffset;
    w.Wear("red");
    w.Step(0);
    CHECK(Drawn(m, at, 9));
    CHECK(m.reads[other] > 0);
    // (the other texture, no colour in it, is read again while it is young: kLateTexelsMs)
    m.Make(at, kTexels);
    w.Step(1100);
    CHECK(Drawn(m, at, 9));
    const int other_reads = m.reads[other];
    // Made again 20 ms later: red at the next step; the other texture (no colour) is not read again.
    m.Make(at, kTexels);
    CHECK(Drawn(m, at, 1));
    w.Step(1120);
    CHECK(Drawn(m, at, 9));
    CHECK(m.reads[other] == other_reads);
    // A new texture of Link's made since: found by the quick step too.
    const std::uint64_t second = kBase + 0x5000 + kOffset;
    std::memcpy(m.at(second), TexelsOf(1).data(), kTexels);
    m.Make(second, kTexels);
    w.Step(1140);
    CHECK(Drawn(m, second, 9));
    CHECK(m.reads[other] == other_reads);
    // Its texels came after it was made (a quick step missed it): read again at the next step, nothing
    // made since (player report #78: before, only the full search a second on found it).
    const std::uint64_t late = kBase + 0x7000 + kOffset;
    m.Make(late, kTexels);
    w.Step(1160);
    CHECK_FALSE(Drawn(m, late, 9));
    std::memcpy(m.at(late), TexelsOf(1).data(), kTexels);
    w.Step(1180);
    CHECK(Drawn(m, late, 9));
    CHECK(m.reads[other] == other_reads);
    // The same with something made in between (the generation moves).
    const std::uint64_t later = kBase + 0x9000 + kOffset;
    m.Make(later, kTexels);
    w.Step(1200);
    std::memcpy(m.at(later), TexelsOf(1).data(), kTexels);
    m.Make(other, kTexels);
    w.Step(1220);
    CHECK(Drawn(m, later, 9));
    CHECK(m.reads[other] > other_reads);
}

TEST_CASE("look tunic: texels that come in late are drawn within a step or two, each texture drawn once") {
    // Player report #78 (0.11.1): your own customised sword showed the game's colours for up to a second
    // after each cutscene or scene, then snapped back. The game makes the texture first, its texels after.
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    std::vector<std::string> said;
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        return it == pack.end() ? std::nullopt : std::optional(it->second);
    };
    host.log = [&](const std::string& s) { said.push_back(s); };
    CoopLookTunic::Wardrobe w(host);
    const auto repaints = [&] {
        return std::count_if(said.begin(), said.end(), [](const std::string& s) { return s.find(" now (") != std::string::npos; });
    };
    w.Wear("red");
    w.Step(0);
    REQUIRE(Drawn(m, kBase + 0x1000 + kOffset, 9));
    REQUIRE(repaints() == 1);
    // Made again with nothing in it; its first bytes come in, then the rest, steps apart.
    const std::uint64_t at = kBase + 0x4000 + kOffset;
    m.Make(at, kTexels);
    w.Step(20);
    std::memcpy(m.at(at), TexelsOf(1).data(), kTexels / 2);
    w.Step(40);
    CHECK_FALSE(Drawn(m, at, 9));
    std::memcpy(m.at(at), TexelsOf(1).data(), kTexels);
    w.Step(60);
    CHECK(Drawn(m, at, 9));
    CHECK(repaints() == 2);
    // Drawn once: later steps (made or not made anything) draw nothing again.
    const int overridden = m.overridden;
    for (std::uint64_t t = 80; t <= 2000; t += 20) {
        if (t % 400 == 0) {
            m.Make(kBase + 0xa000 + kOffset, kTexels);   // (something else made now and then)
        }
        w.Step(t);
    }
    CHECK(m.overridden == overridden);
    CHECK(repaints() == 2);
    CHECK(Drawn(m, at, 9));
}

TEST_CASE("look tunic: a scene's many new textures are read again at most kLateChecks a step, for kLateTexelsMs") {
    FakeTextures m(kBase, 0x40000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        return it == pack.end() ? std::nullopt : std::optional(it->second);
    };
    host.log = [](const std::string&) {};
    CoopLookTunic::Wardrobe w(host);
    w.Wear("red");
    w.Step(0);
    // 200 textures of the size with no colour in them (the scene's), and one of Link's late.
    std::vector<std::uint64_t> made;
    for (std::uint64_t i = 0; i < 200; ++i) {
        made.push_back(kBase + 0x2000 + i * 0x120);
        m.Make(made.back(), kTexels);
    }
    const std::uint64_t late = made[150];
    w.Step(20);   // (the generation moved: every new one read once)
    const auto reads = [&] {
        int n = 0;
        for (const std::uint64_t a : made) {
            n += m.reads[a];
        }
        return n;
    };
    CHECK(reads() == 200);
    // The textures one step reads (a texture whose first bytes are a colour's is read twice: head, then whole).
    const auto step = [&](std::uint64_t at_ms) {
        const std::map<std::uint64_t, int> was = m.reads;
        w.Step(at_ms);
        std::size_t n = 0;
        for (const std::uint64_t a : made) {
            n += m.reads[a] != (was.count(a) ? was.at(a) : 0) ? 1 : 0;
        }
        return n;
    };
    std::memcpy(m.at(late), TexelsOf(1).data(), kTexels);
    std::size_t n = step(40);
    CHECK(n <= CoopLookTunic::kLateChecks);
    CHECK(n > 0);
    // Every one is read again in turn: Link's is drawn within a few steps.
    std::uint64_t t = 40;
    while (!Drawn(m, late, 9) && t < 200) {
        t += 20;
        CHECK(step(t) <= CoopLookTunic::kLateChecks);
    }
    CHECK(Drawn(m, late, 9));
    CHECK(t <= 20 + 20 * (200 / CoopLookTunic::kLateChecks + 1));
    // Once they are older than kLateTexelsMs, a step with nothing made reads none of them.
    w.Step(20 + CoopLookTunic::kLateTexelsMs + 20);
    CHECK(step(20 + CoopLookTunic::kLateTexelsMs + 40) == 0);
}

TEST_CASE("look tunic: while the game makes textures every step, the young ones are still read at most kLateChecks a step") {
    // review-sword-blink: a scene load makes textures over many steps; each such step reads the new
    // ones and at most kLateChecks of the young misses, not all of them again.
    FakeTextures m(kBase, 0x40000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        return it == pack.end() ? std::nullopt : std::optional(it->second);
    };
    host.log = [](const std::string&) {};
    CoopLookTunic::Wardrobe w(host);
    w.Wear("red");
    w.Step(0);
    std::vector<std::uint64_t> made;
    for (std::uint64_t i = 0; i < 200; ++i) {
        made.push_back(kBase + 0x2000 + i * 0x120);
        m.Make(made.back(), kTexels);
    }
    const std::uint64_t late = made[150];
    w.Step(20);
    std::memcpy(m.at(late), TexelsOf(1).data(), kTexels);
    // One new texture made before each step (none of Link's): the 200 are read <= kLateChecks a step.
    std::uint64_t t = 20;
    std::uint64_t fresh = kBase + 0x20000;
    while (!Drawn(m, late, 9) && t < 200) {
        t += 20;
        m.Make(fresh, kTexels);
        fresh += 0x120;
        const std::map<std::uint64_t, int> was = m.reads;
        w.Step(t);
        std::size_t n = 0;
        for (const std::uint64_t a : made) {
            n += m.reads[a] != (was.count(a) ? was.at(a) : 0) ? 1 : 0;
        }
        CHECK(n <= CoopLookTunic::kLateChecks);
        CHECK(m.reads[fresh - 0x120] > 0);   // (the new one is read at once)
    }
    CHECK(Drawn(m, late, 9));
    CHECK(t <= 20 + 20 * (200 / CoopLookTunic::kLateChecks + 1));
}

TEST_CASE("look tunic: an override goes when the game's memory there holds something else, its texture not finalized") {
    // 0.11.0 players (tracker #32): effects' textures gone. The game gave the memory of Link's
    // texture to another texture without finalizing it; the override stayed, drawn over the new one.
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, "Tile_A", TexelsOf(1));
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"red", LookFile({{"Tile_A", TexelsOf(9)}})},
    };
    std::vector<std::string> said;
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        return it == pack.end() ? std::nullopt : std::optional(it->second);
    };
    host.log = [&](const std::string& s) { said.push_back(s); };
    CoopLookTunic::Wardrobe w(host);
    const std::uint64_t at = kBase + 0x1000 + kOffset;
    w.Wear("red");
    w.Step(0);
    REQUIRE(Drawn(m, at, 9));
    // Another texture's texels over that memory, made a little before it (no finalize of Link's).
    const std::vector<std::uint8_t> effect = TexelsOf(50);
    std::memcpy(m.at(at), effect.data(), kTexels);
    m.Make(at - 0x40, kTexels + 0x40);
    w.Step(20);
    CHECK(m.overrides() == 0);
    CHECK(m.Drawn(at, kTexels) == effect);
    CHECK(w.places().empty());
    CHECK(std::any_of(said.begin(), said.end(), [](const std::string& s) {
        return s.find("taken away") != std::string::npos;
    }));
    // A texture of Link's kept since keeps its override (nothing taken away).
    std::memcpy(m.at(at), TexelsOf(1).data(), kTexels);
    m.Make(at, kTexels);
    w.Step(40);
    CHECK(Drawn(m, at, 9));
    const std::size_t lines = said.size();
    m.Make(kBase + 0x9000, kTexels);   // (something else made: the generation moves)
    w.Step(60);
    CHECK(Drawn(m, at, 9));
    CHECK(std::none_of(said.begin() + static_cast<std::ptrdiff_t>(lines), said.end(), [](const std::string& s) {
        return s.find("taken away") != std::string::npos;
    }));
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
            CHECK(r.part != Part::cap_trim);   // the Cap trim is painted with the tunic (TunicPainted)
            CHECK(r.part != Part::eyes);       // the eyes are the model's parameter (CoopCharColours), no texture
            CHECK(r.part != Part::hair);       // the hair too
            CHECK(r.tones != 0);
            parts.insert(r.part);
        }
    }
    CHECK(parts.size() == CoopLook::kParts - 4);
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

namespace {
// metal.bin's bytes (tools/puppet_pack/look_metal.py pack).
struct MetalEntry {
    std::string name;
    std::uint32_t part;
    std::vector<std::uint32_t> offsets;
};
std::vector<std::uint8_t> MetalFile(const std::vector<MetalEntry>& entries) {
    std::vector<std::uint8_t> out = {'C', 'O', 'O', 'P', 'M', 'T', 'L', '1'};
    const auto put = [&](std::uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        }
    };
    put(static_cast<std::uint32_t>(entries.size()));
    for (const MetalEntry& e : entries) {
        std::string n = e.name;
        n.resize(64, '\0');
        out.insert(out.end(), n.begin(), n.end());
        put(e.part);
        put(static_cast<std::uint32_t>(e.offsets.size()));
        for (const std::uint32_t o : e.offsets) {
            put(o);
        }
    }
    return out;
}
const char* const kHylianMetal = "Mt_Weapon_Shield_Hylian_Mtl";
// Eight BC4 blocks (a look file's texture holds 64 bytes or more), every one metal (ends 200 and 120).
std::vector<std::uint8_t> MetalTexels() {
    std::vector<std::uint8_t> t;
    for (int b = 0; b < 8; ++b) {
        const std::uint8_t block[8] = {200, 120, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24};
        t.insert(t.end(), block, block + 8);
    }
    return t;
}
}  // namespace

TEST_CASE("look parts: metal.bin is read whole or not at all") {
    const auto file = MetalFile({{kHylianMetal, 3, {0, 16}}, {kHylianMetal, 4, {8}}});
    const auto metal = CoopLookParts::ParseMetal(file);
    REQUIRE(metal);
    REQUIRE(metal->size() == 2);
    CHECK((*metal)[0].texture == kHylianMetal);
    CHECK((*metal)[0].part == Part::hylian_frame);
    CHECK((*metal)[0].offsets == std::vector<std::uint32_t>{0, 16});
    CHECK((*metal)[1].part == Part::hylian_face);
    std::string why;
    auto cut = file;
    cut.pop_back();
    CHECK_FALSE(CoopLookParts::ParseMetal(cut, &why));
    CHECK(why == "the metal file is cut short");
    auto magic = file;
    magic[7] = '2';
    CHECK_FALSE(CoopLookParts::ParseMetal(magic, &why));
    CHECK(why == "not a metal file");
    CHECK_FALSE(CoopLookParts::ParseMetal(MetalFile({{kHylianMetal, CoopLook::kParts, {0}}}), &why));
    CHECK(why == "a metal entry names no part");
    CHECK(CoopLookParts::ParseMetal(MetalFile({}))->empty());
}

TEST_CASE("look parts: a painted metal part loses its metal, the other blocks and an unpainted part keep it (#202)") {
    const CoopLookParts::Metal metal = *CoopLookParts::ParseMetal(
        MetalFile({{kHylianMetal, 3, {0, 16, 64}}, {kHylianMetal, 4, {8}}, {"Other_Mtl", 3, {24}}}));
    Look look;
    std::vector<std::uint8_t> t = MetalTexels();
    // nothing painted: the game's own metal
    CHECK(CoopLookParts::PaintMetal(metal, kHylianMetal, t, look) == 0);
    CHECK(t == MetalTexels());
    // the frame painted: its blocks 0 and 2 are 0 (offset 64 is past the texels: left alone)
    look[Part::hylian_frame] = 0xE0C030u;
    CHECK(CoopLookParts::PaintMetal(metal, kHylianMetal, t, look) == 2);
    CHECK(std::all_of(t.begin(), t.begin() + 8, [](std::uint8_t b) { return b == 0; }));
    CHECK(std::equal(t.begin() + 8, t.begin() + 16, MetalTexels().begin() + 8));   // the face's: kept
    CHECK(std::all_of(t.begin() + 16, t.begin() + 24, [](std::uint8_t b) { return b == 0; }));
    CHECK(std::equal(t.begin() + 24, t.end(), MetalTexels().begin() + 24));   // Other_Mtl's entry
    // a misaligned offset is never written
    const CoopLookParts::Metal odd = *CoopLookParts::ParseMetal(MetalFile({{kHylianMetal, 3, {4}}}));
    std::vector<std::uint8_t> u = MetalTexels();
    CHECK(CoopLookParts::PaintMetal(odd, kHylianMetal, u, look) == 0);
    CHECK(u == MetalTexels());

    // Painted uses the table it is given (UseMetal); without one, metal stays.
    const auto gear = Colour({{kHylian, Shield()}, {kHylianMetal, MetalTexels()}});
    CHECK(CoopLookParts::Painted(*gear, look)[1].texels == MetalTexels());
    CoopLookParts::UseMetal(std::make_shared<const CoopLookParts::Metal>(metal));
    const auto painted = CoopLookParts::Painted(*gear, look);
    CHECK(painted[1].texels == t);
    CHECK(painted[0].texels != Shield());   // (the frame's colour as before)
    Look plain;
    CHECK(CoopLookParts::Painted(*gear, plain)[1].texels == MetalTexels());
    // a puppet's copy: renamed, its metal painted down, then marked
    const auto copy = CoopLookParts::CopyTexels(*gear, 2, look);
    REQUIRE(copy.size() == 2);
    CHECK(copy[1].name == "C2_Weapon_Shield_Hylian_Mtl");
    auto marked = t;
    CoopLookParts::Mark(marked, 2);
    CHECK(copy[1].texels == marked);
    CoopLookParts::UseMetal(nullptr);
    CHECK(CoopLookParts::Painted(*gear, look)[1].texels == MetalTexels());
}

TEST_CASE("look parts: the adult's Goron and Zora tunics are painted in all their cloth") {
    // The adult's tunic models name more cloth than the vest and hat (the dump: Armor_Adult_Goron's inner,
    // Armor_Adult_Zora's parts and feather): each is in the table under its tunic.
    const auto has = [](const char* name, Part part) {
        for (const CoopLookParts::GearTexture& g : CoopLookParts::Gear()) {
            if (std::string_view(g.name) == name) {
                return std::any_of(g.rules.begin(), g.rules.end(), [&](const auto& r) { return r.part == part; });
            }
        }
        return false;
    };
    CHECK(has("Mt_Inner_Armor_Adult_Goron_Alb", Part::goron_tunic));
    CHECK(has("Mt_Parts_Armor_Adult_Zora_Alb", Part::zora_tunic));
    CHECK(has("Mt_Feather_Armor_Adult_Zora_Alb", Part::zora_tunic));
    // The Zora feather (BC1, blue ends as Shield's face): painted in the Zora tunic's colour, not the Goron's.
    const auto gear = Colour({{"Mt_Feather_Armor_Adult_Zora_Alb", Shield()}});
    Look goron;
    goron[Part::goron_tunic] = 0x20A040u;
    CHECK(CoopLookParts::Painted(*gear, goron)[0].texels == Shield());
    Look zora;
    zora[Part::zora_tunic] = 0x20A040u;
    const auto p = CoopLookParts::Painted(*gear, zora)[0].texels;
    const auto ends = CoopLookPaint::Ends(CoopLookPaint::Format::bc1, std::span<const std::uint8_t>(p.data(), 8));
    CHECK(CoopLookPaint::ToneOf(ends[0]) == CoopLookPaint::kGreen);
}

TEST_CASE("look parts: the gear wardrobe paints your shield, and one the game makes later") {
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, kHylian, Shield());
    Look look;
    look[Part::hylian_frame] = 0xE0C030u;
    const auto base = LookFile({{kHylian, Shield()}});
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
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
    CHECK(m.Drawn(kBase + 0x1000 + kOffset, want.size()) == want);
    CHECK(std::memcmp(m.at(kBase + 0x1000 + kOffset), Shield().data(), kTexels) == 0);   // the game's memory: its own
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) { return l.find("your Link's gear is") != std::string::npos; }));
    // A second shield's texture is made (the game's own texels): painted at the next round.
    PutFile(m, kBase + 0x4000, kHylian, Shield());
    w.Step(1000);
    CHECK(m.Drawn(kBase + 0x4000 + kOffset, want.size()) == want);
    // Back to the game's own colours.
    w.Wear(CoopLookParts::kBase);
    w.Step(2000);
    CHECK(m.Drawn(kBase + 0x1000 + kOffset, kTexels) == Shield());
    CHECK(m.overrides() == 0);
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

TEST_CASE("look tunic: the colour chosen before is worn at start, once the game loads your Link's tunic") {
    // A player's report (0.10.1, before look-live): "Chosen color doesn't apply for self, but does for others".
    // StartModule wears the settings' colour (coop.ini / coop-options.ini through LICHII_COOP_COLOR) before the
    // game has loaded Link: the texture comes later (the title, then the save's scene), and is drawn blue then.
    FakeTextures m(kBase, 0x10000);
    const std::map<std::string, std::vector<std::uint8_t>> pack = {
        {"green", LookFile({{"Tile_A", TexelsOf(1)}})},
        {"blue", LookFile({{"Tile_A", TexelsOf(7)}})},
    };
    std::vector<std::string> log;
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = pack.find(c);
        if (it == pack.end()) {
            return std::nullopt;
        }
        return it->second;
    };
    host.log = [&](const std::string& l) { log.push_back(l); };
    CoopLookTunic::Wardrobe w(host);
    w.Wear(CoopLook::TunicName(0x2050E0));   // a hex colour near blue: the pack's nearest tunic
    CHECK(w.Busy());
    w.Step(0);   // nothing of Link loaded yet
    const std::uint64_t at = kBase + 0x2000 + kOffset;
    PutFile(m, kBase + 0x2000, "Tile_A", TexelsOf(1));   // the game loads the tunic (green, as the game has it)
    w.Step(1000);   // the next round
    CHECK(Drawn(m, at, 7));     // blue, with no pause-menu change and no restart
    CHECK(Holding(m, at, 1));   // (through the texture override: the game's memory keeps its own)
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) { return l.find("is blue now") != std::string::npos; }));
}

TEST_CASE("look parts: a puppet copy's textures are named per copy and painted in its look") {
    CHECK(CoopLookParts::SlotName(kHylian, 2) == "C2_Weapon_Shield_Hylian_Alb");
    CHECK(CoopLookParts::SlotName("Mt_Weapon_Sword_Biggoron_Alb.00", 1) == "C1_Weapon_Sword_Biggoron_Alb.00");
    CHECK(CoopLookParts::SlotName("Angin_Tile_A_Alb", 1).empty());
    CHECK(CoopLookParts::SlotName(kHylian, 0).empty());
    for (const auto& g : CoopLookParts::Gear()) {
        CHECK(CoopLookParts::SlotName(g.name, 8).size() == std::strlen(g.name));   // as long (look_items.py)
    }
    Look red;
    red[Part::hylian_face] = 0xFF0000u;
    const auto gear = Colour({{kHylian, Shield()}, {"Something_Else_Alb", Shield()}});
    const auto plain = CoopLookParts::CopyTexels(*gear, 3, std::nullopt);
    REQUIRE(plain.size() == 1);   // not a gear texture: no copy of it
    CHECK(plain[0].name == "C3_Weapon_Shield_Hylian_Alb");
    CHECK(plain[0].texels == Marked(Shield(), 3));   // marked as the pack's copy file (look_live.py mark)
    CHECK(plain[0].offset == kOffset);
    const auto painted = CoopLookParts::CopyTexels(*gear, 3, red);
    REQUIRE(painted.size() == 1);
    CHECK(painted[0].texels == Marked(CoopLookParts::Painted(*gear, red)[0].texels, 3));
    CHECK(CoopLookTunic::SameLayout(plain, painted));

    CoopLookParts::CopyLooks looks(4);
    CHECK(CoopLookParts::CopiesName(looks) == CoopLookParts::kCopiesBase);
    looks[2] = Look{};   // a look with no gear part: as the game's own
    CHECK(CoopLookParts::CopiesName(looks) == CoopLookParts::kCopiesBase);
    looks[2] = red;
    const std::string name = CoopLookParts::CopiesName(looks);
    CHECK(name != CoopLookParts::kCopiesBase);
    looks[1] = red;
    CHECK(CoopLookParts::CopiesName(looks) != name);   // which copy is painted counts
}

TEST_CASE("look parts: the puppets' wardrobe draws a copy the game makes, and leaves your own shield alone") {
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, kHylian, Shield());   // your own
    const auto gear = *CoopLookTunic::Parse(LookFile({{kHylian, Shield()}}));
    Look red;
    red[Part::hylian_face] = 0xFF0000u;
    CoopLookParts::CopyLooks looks(2);
    looks[1] = red;
    const auto all = [&](const CoopLookParts::CopyLooks& l) {
        CoopLookTunic::Texels out;
        for (std::size_t k = 1; k <= l.size(); ++k) {
            const auto t = CoopLookParts::CopyTexels(gear, k, l[k - 1]);
            out.insert(out.end(), t.begin(), t.end());
        }
        return CoopLookTunic::Pack(out);
    };
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        return all(c == CoopLookParts::kCopiesBase ? CoopLookParts::CopyLooks(2) : looks);
    };
    host.log = [](const std::string&) {};
    host.base = CoopLookParts::kCopiesBase;
    host.what = "puppets' gear";
    CoopLookTunic::Wardrobe w(host);
    w.Wear(CoopLookParts::CopiesName(looks));
    w.Step(0);   // no puppet wears a copy yet: nothing drawn
    CHECK(m.overrides() == 0);
    // The game makes copy 2's shield for a puppet (its file, the game's own texels, under the copy's name)...
    PutFile(m, kBase + 0x3000, "C2_Weapon_Shield_Hylian_Alb", Marked(Shield(), 2));
    PutFile(m, kBase + 0x5000, "C1_Weapon_Shield_Hylian_Alb", Marked(Shield(), 1));
    w.Step(1000);
    // ...and it is drawn in that player's look, the game's memory as it was.
    const auto want = Marked(CoopLookParts::Painted(gear, red)[0].texels, 2);
    CHECK(m.Drawn(kBase + 0x3000 + kOffset, want.size()) == want);
    CHECK(std::memcmp(m.at(kBase + 0x3000 + kOffset), Marked(Shield(), 2).data(), kTexels) == 0);
    CHECK(m.Drawn(kBase + 0x5000 + kOffset, kTexels) == Marked(Shield(), 1));   // copy 1: the game's own colours
    CHECK(m.Drawn(kBase + 0x1000 + kOffset, kTexels) == Shield());   // your own: not the puppets'
    w.Clear();
    CHECK(m.overrides() == 0);
}

TEST_CASE("look parts: a copy's texture name never names another folder") {
    CHECK(CoopLookParts::SlotName("Mt_../../x", 1).empty());
    CHECK(CoopLookParts::SlotName("Mt_a/b", 1).empty());
    CHECK(CoopLookParts::SlotName("Mt_a\\b", 1).empty());
    CHECK(CoopLookParts::SlotName("Mt_C:x", 1).empty());
    CHECK(CoopLookParts::SlotName("Mt_Weapon_Sword_Biggoron_Alb.01", 2) == "C2_Weapon_Sword_Biggoron_Alb.01");
}

TEST_CASE("look parts: the boots in Link's own model get a copy per tunic colour, painted for its wearer") {
    const std::string kChild = "Mt_Leather_Player_Child_Alb", kAdult = "Mt_Boots_Player_Adult_Alb";
    for (const std::string& b : CoopLookParts::BodyBoots()) {   // gear textures too: your own Link's are painted
        CHECK(std::any_of(CoopLookParts::Gear().begin(), CoopLookParts::Gear().end(),
                          [&](const auto& g) { return b == g.name; }));
        CHECK(CoopLookParts::BootsName(b, 'M').size() == b.size());   // as long (look_items.py boots_name)
    }
    CHECK(CoopLookParts::BootsName(kChild, 'M') == "PM_Leather_Player_Child_Alb");
    CHECK(CoopLookParts::BootsName(kHylian, 'M').empty());
    CHECK(CoopLookParts::LetterOf("pink") == 'M');
    CHECK(CoopLookParts::LetterOf("green") == 'G');
    CHECK_FALSE(CoopLookParts::LetterOf("Pink").has_value());   // CoopLook::TunicName's lower case

    // your own Link first, then the others in order; a colour already worn stays with its first wearer
    CoopLookParts::Wearer own{'M', 0xFFFF00u};
    const auto w = CoopLookParts::Assign(own, {{'M', 0x0000FFu}, {'R', 0x00FF00u}, {'R', 0xFF0000u}, {'B', {}},
                                               {std::nullopt, 0x123456u}});
    CHECK(w == CoopLookParts::Wearers{{'M', 0xFFFF00u}, {'R', 0x00FF00u}});
    CHECK(CoopLookParts::Assign({'B', {}}, {{'B', 0xFF0000u}}).empty());   // your own Link keeps the game's
    CHECK(CoopLookParts::BootsPaintingName({}) == CoopLookParts::kBootsBase);
    CHECK(CoopLookParts::BootsPaintingName(w) == "boots.MFFFF00.R00FF00");

    const auto gear = Colour({{kHylian, Shield()}, {kChild, Shield()}, {kAdult, TexelsOf(3)}});
    const auto plain = CoopLookParts::BootsTexels(*gear, {});
    REQUIRE(plain.size() == 20);   // ten colours, two boots each, no shield
    CHECK(plain[0].name == "PR_Leather_Player_Child_Alb");
    CHECK(plain[0].texels == Marked(Shield(), 'R'));   // marked as the pack's copy (look_items.py boots_mark)
    CHECK(plain[1].name == "PR_Boots_Player_Adult_Alb");
    const auto painted = CoopLookParts::BootsTexels(*gear, w);
    CHECK(CoopLookTunic::SameLayout(plain, painted));
    Look yellow;
    yellow[Part::boots] = 0xFFFF00u;
    const auto want = CoopLookParts::Painted(*gear, yellow);
    for (const auto& t : painted) {
        if (t.name == "PM_Leather_Player_Child_Alb") {
            CHECK(t.texels == Marked(want[1].texels, 'M'));
        } else if (t.name == "PB_Leather_Player_Child_Alb") {
            CHECK(t.texels == Marked(Shield(), 'B'));   // no wearer with boots: the game's colour
        }
    }
}

TEST_CASE("look parts: the boots wardrobe draws a colour's copy, and leaves your own boots alone") {
    const std::string kChild = "Mt_Leather_Player_Child_Alb";
    FakeTextures m(kBase, 0x10000);
    PutFile(m, kBase + 0x1000, kChild, Shield());   // your own Link's (the game's model)
    PutFile(m, kBase + 0x3000, "PM_Leather_Player_Child_Alb", Marked(Shield(), 'M'));   // a pink puppet's
    const auto gear = *CoopLookTunic::Parse(LookFile({{kChild, Shield()}}));
    const CoopLookParts::Wearers wearers{{'M', 0x2020FFu}};
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        return CoopLookTunic::Pack(
            CoopLookParts::BootsTexels(gear, c == CoopLookParts::kBootsBase ? CoopLookParts::Wearers{} : wearers));
    };
    host.log = [](const std::string&) {};
    host.base = CoopLookParts::kBootsBase;
    host.what = "puppets' boots";
    CoopLookTunic::Wardrobe w(host);
    w.Wear(CoopLookParts::BootsPaintingName(wearers));
    w.Step(0);
    Look blue;
    blue[Part::boots] = 0x2020FFu;
    const auto want = Marked(CoopLookParts::Painted(gear, blue)[0].texels, 'M');
    CHECK(m.Drawn(kBase + 0x3000 + kOffset, want.size()) == want);
    CHECK(m.Drawn(kBase + 0x1000 + kOffset, kTexels) == Shield());   // your own: not the puppets'
    w.Clear();
    CHECK(m.overrides() == 0);
}

TEST_CASE("look: a look chosen with the menu open is shown on your own Link at once, applied on unpause") {
    std::vector<Look> applied, previewed;
    CoopLook::Init(Look{}, [&](const Look& l) { applied.push_back(l); }, {},
                   [&](const Look& l) { previewed.push_back(l); });
    Look red;
    red[Part::hylian_frame] = 0xFF0000u;
    CoopLook::SetPending(red);
    CoopLook::Frame();   // the menu still open: shown, not applied
    REQUIRE(previewed.size() == 1);
    CHECK(previewed[0] == red);
    CHECK(applied.empty());
    CoopLook::Frame();   // nothing new: shown once
    CHECK(previewed.size() == 1);
    Look blue = red;
    blue[Part::master_blade] = 0x0000FFu;
    CoopLook::SetPending(red);
    CoopLook::SetPending(blue);   // two choices in one frame: the last is shown
    CoopLook::Frame();
    REQUIRE(previewed.size() == 2);
    CHECK(previewed[1] == blue);
    CoopLook::SetPending(red);
    CoopLook::ApplyOnUnpause();   // chosen and unpaused in one frame: applied, not shown first
    CoopLook::Frame();
    CHECK(previewed.size() == 2);
    REQUIRE(applied.size() == 1);
    CHECK(applied[0] == red);
    CHECK(CoopLook::Worn() == red);
    CoopLook::Init(Look{}, nullptr);
}

TEST_CASE("look: your own Link wears your slot colour when you chose none") {
    // None chosen ("by player number"): the slot colour the others see you in, once the number is known.
    CHECK(CoopLook::OwnTunic(std::nullopt, 0) == std::nullopt);   // before the welcome, or co-op off: the game's own
    CHECK(CoopLook::OwnTunic(std::nullopt, 1) == coop::protocol::slot_color(1));
    CHECK(CoopLook::OwnTunic(std::nullopt, 2) == coop::protocol::slot_color(2));   // a rejoin with another number
    CHECK(CoopLook::TunicName(CoopLook::OwnTunic(std::nullopt, 1)) == "blue");
    CHECK(CoopLook::TunicName(CoopLook::OwnTunic(std::nullopt, 2)) == "red");
    // A chosen colour wins over the number.
    CHECK(CoopLook::OwnTunic(0x123456u, 2) == 0x123456u);
    CHECK(CoopLook::OwnTunic(0x123456u, 0) == 0x123456u);
}

TEST_CASE("look tunic: a storage shorter than the pack's texels is not taken (a copy's mark is the image's last byte)") {
    // The pack holds each image at its own size, as the game's storage; a storage that is shorter would
    // leave the mark uncompared, so a puppet's copy could be taken for your own.
    FakeTextures m(kBase, 0x10000);
    const auto green = Colour({{"Tile_A", TexelsOf(1)}});
    PutFile(m, kBase + 0x1000, "Tile_A", Marked(TexelsOf(1), 3));   // copy 3, marked in its last byte
    m.Make(kBase + 0x1000 + kOffset, kTexels - 1);                   // seen as one byte shorter: the mark cut off
    CHECK(CoopLookTunic::Find(m, {green}).places.empty());
}

TEST_CASE("look tunic: the puppets' file of every copy's gear is read (more than 64 textures)") {
    Named many;
    for (int i = 0; i < 136; ++i) {   // 8 copies of 17 gear textures
        many.push_back({"C" + std::to_string(i) + "_Gear", TexelsOf(static_cast<std::uint8_t>(i))});
    }
    std::string why;
    const auto t = CoopLookTunic::Parse(LookFile(many), &why);
    REQUIRE(t);
    CHECK(t->size() == 136);
    const auto again = CoopLookTunic::Parse(CoopLookTunic::Pack(*t));
    REQUIRE(again);
    CHECK(again->size() == 136);
    CHECK((*again)[135].texels == TexelsOf(135));
}

// ini-reject: the module's own coop.ini (the launcher's lines, Customize's
// colour and parts, Join's last invite and host, the title's client id),
// written as the module writes it, then read back as the next start reads
// it: co-op stays on, nothing is refused.
TEST_CASE("look: coop.ini as Customize and Join write it keeps co-op on") {
    std::string coop = "server = 127.0.0.1\r\nport = 27510\r\nname = AlexA\r\n";
    Look look;
    look[Part::tunic] = CoopColors::Parse("black");
    look[Part::hylian_frame] = 0xE0C030u;
    look[Part::hylian_emblem] = CoopColors::Parse("red");
    CHECK(CoopLook::WriteSettings(look, coop, nullptr));
    coop = CoopTitleMenu::IniSet(coop, "last_invite", "ABCDE-FGHJK", true);
    coop = CoopTitleMenu::IniSet(coop, "last_host", "26.1.2.3:27510", true);
    coop = CoopTitleMenu::IniSet(coop, "client_id", "0123456789abcdef0123456789abcdef", true);
    // An unset part is commented out, as Customize leaves it.
    look[Part::hylian_emblem].reset();
    CHECK(CoopLook::WriteSettings(look, coop, nullptr));

    const auto ini = std::filesystem::temp_directory_path() / "coop_tests_module.ini";
    {
        std::ofstream out(ini, std::ios::binary | std::ios::trunc);
        out << coop;
    }
    std::vector<std::string> errors;
    const CoopHook::Settings s = CoopHook::LoadSettings(ini, errors);
    std::filesystem::remove(ini);
    for (const auto& e : errors) {
        MESSAGE(e);
    }
    const auto env_set = [](const char* name) {
#ifdef _MSC_VER
        char* v = nullptr;
        std::size_t n = 0;
        const bool set = _dupenv_s(&v, &n, name) == 0 && v != nullptr;
        std::free(v);
        return set;
#else
        return std::getenv(name) != nullptr;
#endif
    };
    if (!env_set("LICHII_COOP_SERVER") && !env_set("LICHII_COOP_NAME") && !env_set("LICHII_COOP_COLOR") &&
        !env_set("LICHII_COOP_PORT") && !env_set("LICHII_COOP_PASSWORD")) {
        CHECK(errors.empty());
        CHECK(s.enabled());
        CHECK(s.server == "127.0.0.1");
        CHECK(s.name == "AlexA");
        CHECK(s.color == CoopColors::Parse("black"));
    }
    const Look back = CoopLook::ReadParts(coop);
    CHECK(back[Part::hylian_frame] == 0xE0C030u);
    CHECK_FALSE(back[Part::hylian_emblem]);
}

TEST_CASE("look: a Nucleus game's coop.ini is known by the handler's first line (row 194)") {
    const std::string marker = "# Written by the Nucleus Co-op handler (tools/nucleus/OoTCoop.js) at each start.";
    CHECK(CoopLook::NucleusWrote(marker + "\r\nserver = 127.0.0.1\r\ncolor = red\r\n"));
    CHECK(CoopLook::NucleusWrote("\xef\xbb\xbf" + marker + "\n"));
    CHECK(CoopLook::NucleusWrote(marker));
    CHECK_FALSE(CoopLook::NucleusWrote(""));
    CHECK_FALSE(CoopLook::NucleusWrote("# Written by"));
    CHECK_FALSE(CoopLook::NucleusWrote("name = Saria\r\ncolor = red\r\n"));   // (a kit's or a player's own)
    CHECK_FALSE(CoopLook::NucleusWrote("name = Saria\r\n" + marker + "\r\n"));   // (only the first line counts)
}

TEST_CASE("look: your own Link starts in the colour of the pack's tunic files, not the switch's") {
    const std::map<std::string, std::vector<std::uint8_t>> self{{"black", {1, 2}}, {"pink", {3, 4}}, {"white", {5}}};
    const CoopLook::FileFn file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = self.find(c);
        return it == self.end() ? std::nullopt : std::optional(it->second);
    };
    // No tile in the pack: the game's own green, whatever the switch says.
    CHECK(CoopLook::PackStartTunic(std::nullopt, file, "") == "green");
    CHECK(CoopLook::PackStartTunic(std::nullopt, file, "pink") == "green");
    // The owner's lane1 run: black files left in the pack, pink chosen (sent), no switch.
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{1, 2}, file, "") == "black");
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{1, 2}, file, "pink") == "black");
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{3, 4}, file, "pink") == "pink");
    // A tile that is no colour's file (an older pack): the switch, else green.
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{9}, file, "white") == "white");
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{9}, file, "") == "green");
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{9}, nullptr, "") == "green");
}

TEST_CASE("look: green's own tunic files are known too (npc-colour)") {
    // Your own Link wears its own copies in green as well: the game's tunic
    // textures are on Mido's and Saria's models (every Kokiri).
    const std::map<std::string, std::vector<std::uint8_t>> self{{"red", {1}}, {"green", {7}}};
    const CoopLook::FileFn file = [&](const std::string& c) -> std::optional<std::vector<std::uint8_t>> {
        const auto it = self.find(c);
        return it == self.end() ? std::nullopt : std::optional(it->second);
    };
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{7}, file, "red") == "green");
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{7}, file, "") == "green");
    CHECK(CoopLook::PackStartTunic(std::vector<std::uint8_t>{1}, file, "green") == "red");
}

namespace {
// A green.bin of synthetic texels (nothing of the game): every tunic texture
// in its format, eight blocks of greens each (ASTC: one-colour blocks).
CoopLookTunic::Texels GreenTunic() {
    using CoopLookPaint::Format;
    const auto rgb565 = [](int r, int g, int b) {
        return static_cast<std::uint16_t>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
    };
    CoopLookTunic::Texels out;
    for (const auto& t : CoopLookParts::TunicTextures()) {
        std::vector<std::uint8_t> texels;
        for (int k = 0; k < 8; ++k) {
            if (t.format == Format::astc) {
                std::uint8_t block[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   // void extent, LDR
                const int c[4] = {60 + 3 * k, 130 + 2 * k, 50, 255};
                for (int i = 0; i < 4; ++i) {
                    block[8 + 2 * i] = static_cast<std::uint8_t>(c[i]);
                    block[9 + 2 * i] = static_cast<std::uint8_t>(c[i]);
                }
                texels.insert(texels.end(), block, block + 16);
                continue;
            }
            if (t.format == Format::bc3) {
                const std::uint8_t alpha[8] = {0xFF, 0xFF};
                texels.insert(texels.end(), alpha, alpha + 8);
            }
            const std::uint16_t c0 = rgb565(70 + 4 * k, 150, 50), c1 = rgb565(30, 90, 20);
            const std::uint8_t colour[8] = {static_cast<std::uint8_t>(c0), static_cast<std::uint8_t>(c0 >> 8),
                                            static_cast<std::uint8_t>(c1), static_cast<std::uint8_t>(c1 >> 8),
                                            0x1B, 0xE4, 0x1B, 0xE4};
            texels.insert(texels.end(), colour, colour + 8);
        }
        out.push_back({t.name, kOffset, std::move(texels)});
    }
    return out;
}
}  // namespace

TEST_CASE("look parts: your own tunic is painted in the exact colour, every texture of both ages") {
    // Names: the game's own green when none, else the exact colour; read back.
    CHECK(CoopLookParts::TunicPainting(std::nullopt) == "green");
    CHECK(CoopLookParts::TunicPainting(0x30D040u) == "green");   // (the preset green: the game's own)
    CHECK(CoopLookParts::TunicPainting(0x38D040u) == "tunic-38D040");
    CHECK(CoopLookParts::TunicColour("tunic-30D040") == 0x30D040u);
    CHECK(CoopLookParts::TunicColour(CoopLookParts::TunicPainting(0x0A0B0Cu)) == 0x0A0B0Cu);
    CHECK_FALSE(CoopLookParts::TunicColour("green").has_value());
    CHECK_FALSE(CoopLookParts::TunicColour("red").has_value());
    CHECK_FALSE(CoopLookParts::TunicColour("tunic-30D04").has_value());
    CHECK_FALSE(CoopLookParts::TunicColour("tunic-30D04G").has_value());
    // Two near colours: the same preset for the puppets, two paintings of your own Link.
    CHECK(CoopLook::TunicName(0x30D040u) == CoopLook::TunicName(0x38D040u));
    CHECK(CoopLookParts::TunicPainting(0x30D040u) != CoopLookParts::TunicPainting(0x38D040u));

    const auto green = GreenTunic();
    REQUIRE(green.size() == 7);
    CHECK(green[0].name == CoopLookParts::kTunicTile);
    const auto a = CoopLookParts::TunicPainted(green, 0x30D040u);
    const auto b = CoopLookParts::TunicPainted(green, 0x38D040u);
    REQUIRE(a.size() == green.size());
    CHECK(CoopLookTunic::SameLayout(a, green));
    for (std::size_t i = 0; i < green.size(); ++i) {
        CAPTURE(green[i].name);
        CHECK(a[i].texels.size() == green[i].texels.size());   // what Draw needs: the size held
        CHECK(a[i].offset == green[i].offset);
        CHECK(a[i].texels != green[i].texels);                 // every texture painted
        CHECK(a[i].texels != b[i].texels);                     // and each colour its own
    }
    // The tile's average becomes the colour (tunic.py); a colour far off is far off.
    using CoopLookPaint::AverageLuma;
    using CoopLookPaint::Format;
    CHECK(AverageLuma(Format::bc3, a[0].texels) == doctest::Approx(CoopLookPaint::Luma({0x30, 0xD0, 0x40})).epsilon(0.05));
    const auto red = CoopLookParts::TunicPainted(green, 0xD02020u);
    const auto ends = CoopLookPaint::Ends(Format::bc3, red[0].texels);
    CHECK(CoopLookPaint::ToneOf(ends[0]) == CoopLookPaint::kRed);
    // A texture that is not the tunic's: as it is.
    auto more = green;
    more.push_back({"Something_Else_Alb", kOffset, green[2].texels});
    CHECK(CoopLookParts::TunicPainted(more, 0xD02020u).back().texels == green[2].texels);
}

TEST_CASE("look tunic: your own Link wears the exact colour picked, not the pack's nearest tunic") {
    // The colour editor's Red, Green and Blue rows on the Kokiri tunic: a step of 8 is drawn, and the game's
    // own green comes back with no colour. The wardrobe paints green.bin's texels (the module's TunicFile).
    FakeTextures m(kBase, 0x10000);
    const auto green = GreenTunic();
    std::vector<std::string> log, asked;
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& name) -> std::optional<std::vector<std::uint8_t>> {
        asked.push_back(name);
        if (name == "green") {
            return CoopLookTunic::Pack(green);
        }
        if (const auto c = CoopLookParts::TunicColour(name)) {
            return CoopLookTunic::Pack(CoopLookParts::TunicPainted(green, *c));
        }
        return std::nullopt;   // (no preset file is asked for)
    };
    host.log = [&](const std::string& l) { log.push_back(l); };
    CoopLookTunic::Wardrobe w(host);
    // The game has your own Link's tile and the adult's lower tunic (green: the files the kit put in).
    const std::uint64_t tile = kBase + 0x2000, vest = kBase + 0x4000;
    PutFile(m, tile, green[0].name, green[0].texels);
    PutFile(m, vest, green[2].name, green[2].texels);
    const auto drawn = [&](std::uint64_t at, const std::vector<std::uint8_t>& t) {
        return m.Drawn(at + kOffset, t.size()) == t;
    };
    w.Wear(CoopLookParts::TunicPainting(0x3070FFu));
    w.Step(0);
    const auto a = CoopLookParts::TunicPainted(green, 0x3070FFu);
    CHECK(drawn(tile, a[0].texels));
    CHECK(drawn(vest, a[2].texels));
    w.Wear(CoopLookParts::TunicPainting(0x3870FFu));   // one step of 8 on Red
    w.Step(20);
    const auto b = CoopLookParts::TunicPainted(green, 0x3870FFu);
    CHECK(drawn(tile, b[0].texels));
    CHECK(drawn(vest, b[2].texels));
    CHECK_FALSE(drawn(tile, a[0].texels));
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) { return l.find("is tunic-3870FF now") != std::string::npos; }));
    w.Wear(CoopLookParts::TunicPainting(std::nullopt));   // the game's own
    w.Step(40);
    CHECK(drawn(tile, green[0].texels));
    CHECK(m.overrides() == 0);
    CHECK(std::none_of(asked.begin(), asked.end(), [](const std::string& n) { return n == "red" || n == "blue"; }));
}

// ---- the Cap trim (CoopLook::Part::cap_trim) ----

TEST_CASE("look: the Cap trim is a part of its own: its number, key, row, settings and wire form") {
    const CoopLook::PartInfo& info = CoopLook::InfoOf(Part::cap_trim);
    CHECK(info.part == Part::cap_trim);
    CHECK(std::string(info.key) == "cap_trim");
    CHECK(std::string(info.item) == "Tunic");
    CHECK(std::string(info.name) == "Cap trim");
    CHECK(CoopLook::PartOf(17) == Part::cap_trim);
    // On the menu: the Tunic heading's, after the tunics.
    const auto& parts = CoopLook::Parts();
    CHECK(parts[3].part == Part::cap_trim);
    CHECK(parts[2].part == Part::zora_tunic);
    // Saved under look_cap_trim and read back; unset again: commented out.
    Look look;
    look[Part::cap_trim] = 0xC02020u;
    std::string coop = "server = 1.2.3.4\n";
    CHECK(CoopLook::WriteSettings(look, coop, nullptr));
    CHECK(coop.find("look_cap_trim = #C02020\n") != std::string::npos);
    CHECK(CoopLook::ReadParts(coop)[Part::cap_trim] == 0xC02020u);
    std::string options = "color = red\n";
    CHECK(CoopLook::WriteSettings(look, coop, &options));   // a kit: coop-options.ini
    CHECK(CoopLook::ReadParts(options)[Part::cap_trim] == 0xC02020u);
    look[Part::cap_trim].reset();
    CHECK(CoopLook::WriteSettings(look, coop, nullptr));
    CHECK_FALSE(CoopTitleMenu::IniGet(coop, "look_cap_trim").has_value());
    CHECK_FALSE(CoopLook::ReadParts(coop)[Part::cap_trim]);
    // On the wire as part 17, read back; a build that knows 17 parts passes it over (as FromWire does past kParts).
    look[Part::cap_trim] = 0x3070FFu;
    look[Part::boots] = 0x112233u;
    const auto wire = CoopLook::Wire(look);
    REQUIRE(wire.size() == 2);
    CHECK(wire[1] == std::pair<std::uint8_t, std::uint32_t>{17, 0x3070FFu});
    CHECK(CoopLook::FromWire(wire) == look);
    coop::protocol::PlayerLook m{coop::protocol::kPlayerSelf, {{15, 0x112233u}, {17, 0x3070FFu}}};
    std::vector<std::uint8_t> bytes;
    REQUIRE(coop::protocol::encode(coop::protocol::Message{m}, bytes) == coop::protocol::Error::none);
    coop::protocol::Message back;
    REQUIRE(coop::protocol::decode(bytes, back) == coop::protocol::Error::none);
    CHECK(std::get<coop::protocol::PlayerLook>(back) == m);
    // Not gear: the gear's painting and the puppets' copies are not made again for it.
    Look cap;
    cap[Part::cap_trim] = 0x3070FFu;
    CHECK_FALSE(CoopLookParts::AnyGear(cap));
    CHECK(CoopLookParts::Name(cap) == "gear");
    CHECK(CoopLookParts::Name(look) == CoopLookParts::Name([&] {
              Look l = look;
              l[Part::cap_trim].reset();
              return l;
          }()));
}

TEST_CASE("look parts: a tunic painting's name says the tunic's colour and the Cap trim's") {
    using CoopLookParts::TunicPaint;
    using CoopLookParts::TunicPaintOf;
    using CoopLookParts::TunicPainting;
    // No cap colour: the names as before (the cap follows the tunic).
    CHECK(TunicPainting(std::nullopt, std::nullopt) == "green");
    CHECK(TunicPainting(0x38D040u, std::nullopt) == "tunic-38D040");
    // With one: both, the game's own green said as "green".
    CHECK(TunicPainting(0x38D040u, 0xC02020u) == "tunic-38D040-cap-C02020");
    CHECK(TunicPainting(std::nullopt, 0xC02020u) == "tunic-green-cap-C02020");
    CHECK(TunicPainting(0x30D040u, 0xC02020u) == "tunic-green-cap-C02020");   // (the preset green: the game's own)
    // Read back.
    CHECK(TunicPaintOf("tunic-38D040-cap-C02020") == TunicPaint{0x38D040u, 0xC02020u});
    CHECK(TunicPaintOf("tunic-green-cap-C02020") == TunicPaint{std::nullopt, 0xC02020u});
    CHECK(TunicPaintOf("tunic-38D040") == TunicPaint{0x38D040u, std::nullopt});
    CHECK(TunicPaintOf(TunicPainting(0x0A0B0Cu, 0x010203u)) == TunicPaint{0x0A0B0Cu, 0x010203u});
    CHECK_FALSE(TunicPaintOf("green"));
    CHECK_FALSE(TunicPaintOf("red"));
    CHECK_FALSE(TunicPaintOf("tunic-green"));
    CHECK_FALSE(TunicPaintOf("tunic-38D040-cap-"));
    CHECK_FALSE(TunicPaintOf("tunic-38D040-cap-C0202G"));
    CHECK_FALSE(TunicPaintOf("tunic-38d040-cap-C02020"));
    // The old reader takes the tunic-only names only.
    CHECK_FALSE(CoopLookParts::TunicColour("tunic-38D040-cap-C02020").has_value());
    // Each cap colour its own painting.
    CHECK(TunicPainting(0x38D040u, 0xC02020u) != TunicPainting(0x38D040u, 0xC02028u));
}

namespace {
// cap.bin's bytes (tools/puppet_pack/look_cap.py pack).
struct CapEntry {
    std::string name;
    std::uint32_t block;
    std::vector<std::uint32_t> offsets;
};
std::vector<std::uint8_t> CapFile(const std::vector<CapEntry>& entries) {
    std::vector<std::uint8_t> out = {'C', 'O', 'O', 'P', 'C', 'A', 'P', '1'};
    const auto put = [&](std::uint32_t v) {
        for (int k = 0; k < 4; ++k) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        }
    };
    put(static_cast<std::uint32_t>(entries.size()));
    for (const CapEntry& e : entries) {
        std::string n = e.name;
        n.resize(64, '\0');
        out.insert(out.end(), n.begin(), n.end());
        put(e.block);
        put(static_cast<std::uint32_t>(e.offsets.size()));
        for (const std::uint32_t o : e.offsets) {
            put(o);
        }
    }
    return out;
}
const char* const kChildParts = "Mt_Cloth_Parts_Player_Child_Alb";   // ASTC: 16-byte blocks
const char* const kAdultAcs = "Mt_Angin_Acs_Player_Adult_Alb";      // BC1: 8-byte blocks

// The blocks (of `step` bytes) where two texels differ.
std::vector<std::size_t> Changed(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
                                 std::size_t step) {
    std::vector<std::size_t> out;
    for (std::size_t at = 0; at + step <= std::min(a.size(), b.size()); at += step) {
        if (std::memcmp(a.data() + at, b.data() + at, step) != 0) {
            out.push_back(at);
        }
    }
    return out;
}

const CoopLookTunic::Texture& TextureOf(const CoopLookTunic::Texels& t, const std::string& name) {
    for (const auto& x : t) {
        if (x.name == name) {
            return x;
        }
    }
    return t.front();
}
}  // namespace

TEST_CASE("look parts: cap.bin is read whole or not at all") {
    const auto file = CapFile({{kChildParts, 16, {0, 48}}, {kAdultAcs, 8, {8}}});
    const auto cap = CoopLookParts::ParseCap(file);
    REQUIRE(cap);
    REQUIRE(cap->size() == 2);
    CHECK((*cap)[0].texture == kChildParts);
    CHECK((*cap)[0].block == 16);
    CHECK((*cap)[0].offsets == std::vector<std::uint32_t>{0, 48});
    CHECK((*cap)[1].offsets == std::vector<std::uint32_t>{8});
    std::string why;
    auto cut = file;
    cut.pop_back();
    CHECK_FALSE(CoopLookParts::ParseCap(cut, &why));
    CHECK(why == "the cap file is cut short");
    auto magic = file;
    magic[4] = 'M';
    CHECK_FALSE(CoopLookParts::ParseCap(magic, &why));
    CHECK(why == "not a cap file");
    CHECK_FALSE(CoopLookParts::ParseCap(CapFile({{kChildParts, 4, {0}}}), &why));
    CHECK(why == "a cap entry has no block size");
    CHECK(CoopLookParts::ParseCap(CapFile({}))->empty());
}

TEST_CASE("look parts: the Cap trim paints only the cap's blocks, over the tunic's colour") {
    using CoopLookParts::TunicPaint;
    const auto green = GreenTunic();
    // The child's headband: blocks 1 and 3; the adult's hood trim: block 0. An offset off a block, one past the
    // texels and an entry of another block size are passed over.
    const auto cap = *CoopLookParts::ParseCap(CapFile(
        {{kChildParts, 16, {16, 48, 20, 4096}}, {kAdultAcs, 8, {0}}, {"Mt_Vest_Player_Adult_Alb", 8, {0}}}));
    const auto tunic = CoopLookParts::TunicPainted(green, TunicPaint{0x3070FFu, std::nullopt}, &cap);
    CHECK(tunic.size() == green.size());
    // No cap colour: the cap follows the tunic (the painting of the tunic alone, the same bytes).
    const auto alone = CoopLookParts::TunicPainted(green, 0x3070FFu);
    for (std::size_t i = 0; i < green.size(); ++i) {
        CHECK(tunic[i].texels == alone[i].texels);
    }
    // A cap colour: the cap's blocks differ from the tunic's painting, every other byte is the same.
    const auto both = CoopLookParts::TunicPainted(green, TunicPaint{0x3070FFu, 0xC02020u}, &cap);
    REQUIRE(both.size() == green.size());
    CHECK(CoopLookTunic::SameLayout(both, green));
    for (std::size_t i = 0; i < green.size(); ++i) {
        CAPTURE(green[i].name);
        CHECK(both[i].texels.size() == green[i].texels.size());
        if (green[i].name == kChildParts) {
            CHECK(Changed(both[i].texels, alone[i].texels, 16) == std::vector<std::size_t>{16, 48});
        } else if (green[i].name == kAdultAcs) {
            CHECK(Changed(both[i].texels, alone[i].texels, 8) == std::vector<std::size_t>{0});
        } else {
            CHECK(both[i].texels == alone[i].texels);   // (the adult's vest: its entry's block size is wrong)
        }
    }
    // The cap's blocks are red now, from green's own texels; the tunic's blue around them.
    using CoopLookPaint::Format;
    const auto& acs = TextureOf(both, kAdultAcs).texels;
    const auto cap_ends = CoopLookPaint::Ends(Format::bc1, std::span<const std::uint8_t>(acs.data(), 8));
    CHECK(CoopLookPaint::ToneOf(cap_ends[0]) == CoopLookPaint::kRed);
    const auto rest = CoopLookPaint::Ends(Format::bc1, std::span<const std::uint8_t>(acs.data() + 8, 8));
    CHECK(CoopLookPaint::ToneOf(rest[0]) == CoopLookPaint::kBlue);
    // On the game's own green: only the cap's blocks change.
    const auto own = CoopLookParts::TunicPainted(green, TunicPaint{std::nullopt, 0xC02020u}, &cap);
    for (std::size_t i = 0; i < green.size(); ++i) {
        CAPTURE(green[i].name);
        if (green[i].name == kChildParts) {
            CHECK(Changed(own[i].texels, green[i].texels, 16) == std::vector<std::size_t>{16, 48});
        } else if (green[i].name == kAdultAcs) {
            CHECK(Changed(own[i].texels, green[i].texels, 8) == std::vector<std::size_t>{0});
        } else {
            CHECK(own[i].texels == green[i].texels);
        }
    }
    // The cap painted from green's texels: the same blocks whatever the tunic's colour.
    const auto other = CoopLookParts::TunicPainted(green, TunicPaint{0xD0D020u, 0xC02020u}, &cap);
    const auto& a = TextureOf(both, kChildParts).texels;
    const auto& b = TextureOf(other, kChildParts).texels;
    CHECK(std::equal(a.begin() + 16, a.begin() + 32, b.begin() + 16));
    CHECK(std::equal(a.begin() + 48, a.begin() + 64, b.begin() + 48));
    // No cap.bin (an older pack): a cap colour changes nothing.
    const auto none = CoopLookParts::TunicPainted(green, TunicPaint{0x3070FFu, 0xC02020u}, nullptr);
    for (std::size_t i = 0; i < green.size(); ++i) {
        CHECK(none[i].texels == alone[i].texels);
    }
    // PaintCap alone: the blocks it changed, the rest of the bytes left.
    std::vector<std::uint8_t> t = TextureOf(green, kChildParts).texels;
    CHECK(CoopLookParts::PaintCap(cap, kChildParts, Format::astc, TextureOf(green, kChildParts).texels, t, 0xC02020u,
                                  0.2) == 2);
    CHECK(Changed(t, TextureOf(green, kChildParts).texels, 16) == std::vector<std::size_t>{16, 48});
    CHECK(CoopLookParts::PaintCap(cap, "Something_Else_Alb", Format::astc, TextureOf(green, kChildParts).texels, t,
                                  0xC02020u, 0.2) == 0);
}

TEST_CASE("look tunic: the wardrobe draws the tunic and the Cap trim as one painting") {
    using CoopLookParts::TunicPaint;
    FakeTextures m(kBase, 0x10000);
    // (GreenTunic's ASTC textures hold the same texels; the wardrobe finds a texture by its texels, so the
    // child's parts get one block of their own)
    auto green = GreenTunic();
    for (auto& t : green) {
        if (t.name == kChildParts) {
            t.texels[7 * 16 + 8] ^= 0x10;
        }
    }
    const auto cap = *CoopLookParts::ParseCap(CapFile({{kChildParts, 16, {16}}}));
    CoopLookTunic::Wardrobe::Host host;
    host.textures = &m;
    host.file = [&](const std::string& name) -> std::optional<std::vector<std::uint8_t>> {   // (the module's TunicFile)
        if (name == "green") {
            return CoopLookTunic::Pack(green);
        }
        if (const auto paint = CoopLookParts::TunicPaintOf(name)) {
            return CoopLookTunic::Pack(CoopLookParts::TunicPainted(green, *paint, &cap));
        }
        return std::nullopt;
    };
    host.log = [](const std::string&) {};
    CoopLookTunic::Wardrobe w(host);
    const auto parts = TextureOf(green, kChildParts);   // (a copy: gcc -Werror=dangling-reference)
    const std::uint64_t at = kBase + 0x2000;
    PutFile(m, at, parts.name, parts.texels);
    const auto drawn = [&](const std::vector<std::uint8_t>& t) { return m.Drawn(at + kOffset, t.size()) == t; };
    w.Wear(CoopLookParts::TunicPainting(0x3070FFu, 0xC02020u));
    w.Step(0);
    const auto both = CoopLookParts::TunicPainted(green, TunicPaint{0x3070FFu, 0xC02020u}, &cap);
    CHECK(drawn(TextureOf(both, kChildParts).texels));
    // The cap colour taken away: the cap follows the tunic again.
    w.Wear(CoopLookParts::TunicPainting(0x3070FFu, std::nullopt));
    w.Step(20);
    CHECK(drawn(TextureOf(CoopLookParts::TunicPainted(green, 0x3070FFu), kChildParts).texels));
    // The game's own green with a cap colour: only the cap's block drawn over.
    w.Wear(CoopLookParts::TunicPainting(std::nullopt, 0xC02020u));
    w.Step(40);
    const auto own = CoopLookParts::TunicPainted(green, TunicPaint{std::nullopt, 0xC02020u}, &cap);
    CHECK(drawn(TextureOf(own, kChildParts).texels));
    CHECK(Changed(TextureOf(own, kChildParts).texels, parts.texels, 16) == std::vector<std::size_t>{16});
}

TEST_CASE("look (row 198 guard): a texture of your Link found more than once is named") {
    using CoopLookTunic::Place;
    std::vector<Place> places(4);
    places[0].texture = 0, places[1].texture = 2, places[2].texture = 2, places[3].texture = 5;
    const auto twice = CoopLookTunic::Repeated(places);
    REQUIRE(twice.size() == 1);
    CHECK(twice[0] == std::pair<std::size_t, std::size_t>{2, 2});
    places.pop_back();
    places[2].texture = 3;
    CHECK(CoopLookTunic::Repeated(places).empty());
}

TEST_CASE("look (row 198): where the places lie, and two wardrobes over one texture") {
    using CoopLookTunic::Place;
    CoopLookTunic::Texels names(2);
    names[0].name = "Mt_Weapon_Shield_Hylian_Alb";
    names[1].name = "Mt_Weapon_Shield_Hylian_Mtl";
    std::vector<Place> own(2);
    own[0] = {0, 0x1000, 7, 0x100, nullptr, nullptr, false};
    own[1] = {1, 0x3000, 8, 0x10, nullptr, nullptr, false};
    CHECK(CoopLookTunic::PlacesLine(own, &names) ==
          "Mt_Weapon_Shield_Hylian_Alb@0x1000/7+256 Mt_Weapon_Shield_Hylian_Mtl@0x3000/8+16");
    CHECK(CoopLookTunic::PlacesLine(own, nullptr) == "#0@0x1000/7+256 #1@0x3000/8+16");
    std::vector<Place> theirs(2);
    theirs[0] = {4, 0x1100, 9, 0x100, nullptr, nullptr, false};   // starts after own[0]'s end: apart
    theirs[1] = {5, 0x2ff8, 10, 0x10, nullptr, nullptr, false};   // its last 8 bytes on own[1]'s first
    const auto both = CoopLookTunic::Overlaps(own, theirs);
    REQUIRE(both.size() == 1);
    CHECK(both[0] == std::pair<std::size_t, std::size_t>{1, 1});
    theirs[1].address = 0x3010;   // right after own[1]: apart
    CHECK(CoopLookTunic::Overlaps(own, theirs).empty());
}

TEST_CASE("look (0.11.9): the puppets' tunic copies, each repainted in the exact colour of the player given it") {
    const auto green = GreenTunic();
    // Two copies as the pack wrote them (here: green's texels shifted, so they differ from the painting).
    auto red = green, blue = green;
    for (auto& t : red) t.texels.back() ^= 0xA5;
    for (auto& t : blue) t.texels.front() ^= 0x11;
    blue.back().texels.push_back(0);   // (a texture of another size: kept as written)
    const std::vector<std::pair<std::string, CoopLookTunic::Texels>> copies = {{"red", red}, {"blue", blue}};
    CHECK(CoopLookParts::PuppetTunicsName({}) == "ptunics");
    using Paint = CoopLookParts::TunicPaint;
    CHECK(CoopLookParts::PuppetTunicsName({{"red", Paint{0xFF6820u, std::nullopt}}, {"blue", Paint{0x123456u, std::nullopt}}}) ==
          "ptunics|blue=123456|red=FF6820");
    CHECK(CoopLookParts::PuppetTunicsName({{"red", Paint{0xFF6820u, 0x2040FFu}}}) == "ptunics|red=FF6820+cap2040FF");   // (the trim too)
    // None painted: the base, every copy as written.
    auto base = CoopLookParts::PuppetTunicsTexels(copies, green, {});
    REQUIRE(base.size() == red.size() + blue.size());
    CHECK(CoopLookTunic::SameLayout(base, base));
    for (std::size_t i = 0; i < red.size(); ++i) CHECK(base[i].texels == red[i].texels);
    // Blue given to a player in #123456: its textures from green in that colour, red's as written.
    const auto painted = CoopLookParts::PuppetTunicsTexels(copies, green, {{"blue", Paint{0x123456u, std::nullopt}}});
    REQUIRE(painted.size() == base.size());
    CHECK(CoopLookTunic::SameLayout(painted, base));   // (the wardrobe draws a painting over the base, texture by texture)
    const auto want = CoopLookParts::TunicPainted(green, 0x123456u);
    for (std::size_t i = 0; i < red.size(); ++i) CHECK(painted[i].texels == red[i].texels);
    for (std::size_t i = 0; i + 1 < blue.size(); ++i) {
        CHECK(painted[red.size() + i].texels == want[i].texels);
        CHECK(painted[red.size() + i].name == blue[i].name);
    }
    CHECK(painted.back().texels == blue.back().texels);   // (another size: as written)

    // The dark tile (review round 2): in the copies it is the main tile darker (BC3, the main tile's size); green's is
    // another size (ASTC): painted from green's main tile in the colour scaled by kDarkScale.
    REQUIRE(green.size() >= 2);
    auto green2 = green;   // (its dark tile another size than the tile, as the game's ASTC one is)
    const std::vector<std::uint8_t> block(green2[1].texels.begin(), green2[1].texels.begin() + 16);
    green2[1].texels.insert(green2[1].texels.end(), block.begin(), block.end());
    auto red2 = green2;
    red2[1].name = "Angin_Tile_R_Dark_Alb";
    red2[1].texels = red2[0].texels;   // (as long as the main tile)
    REQUIRE(red2[1].texels.size() != green2[1].texels.size());
    const auto dark = CoopLookParts::PuppetTunicsTexels({{"red", red2}}, green2, {{"red", Paint{0xC86432u, std::nullopt}}});
    const auto darker = CoopLookParts::TunicPainted(green2, 0x804020u);   // (0xC8, 0x64, 0x32 times 0.64, rounded)
    CHECK(dark[1].texels == darker[0].texels);
    CHECK(dark[0].texels == CoopLookParts::TunicPainted(green2, 0xC86432u)[0].texels);
}
