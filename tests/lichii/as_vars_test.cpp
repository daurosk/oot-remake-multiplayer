// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop_as_vars.h"

namespace {

// A fake guest memory of 4 KiB pages: a read is false outside a page that was written.
struct Memory {
    std::map<std::uint64_t, std::array<std::uint8_t, 0x1000>> pages;

    void Write(std::uint64_t at, const void* in, std::size_t size) {
        const auto* b = static_cast<const std::uint8_t*>(in);
        for (std::size_t i = 0; i < size; ++i) {
            auto& page = pages[(at + i) & ~0xfffull];
            page[(at + i) & 0xfff] = b[i];
        }
    }
    void Word(std::uint64_t at, std::uint64_t v) { Write(at, &v, sizeof(v)); }
    void Float(std::uint64_t at, float v) { Write(at, &v, sizeof(v)); }
    CoopAsVars::Probe::Read Reader() const {
        return [this](std::uint64_t at, void* out, std::size_t size) {
            auto* b = static_cast<std::uint8_t*>(out);
            for (std::size_t i = 0; i < size; ++i) {
                const auto it = pages.find((at + i) & ~0xfffull);
                if (it == pages.end()) return false;
                b[i] = it->second[(at + i) & 0xfff];
            }
            return true;
        };
    }
};

constexpr std::uint64_t kOurs = 0x100000000ull;      // our Link's AS controller
constexpr std::uint64_t kOurVars = 0x100010000ull;   // a block it points to (+0x58), the variables at +0x20
constexpr std::uint64_t kPuppet = 0x200000000ull;    // a puppet's controller, the same shape
constexpr std::uint64_t kPuppetVars = 0x200010000ull;

}  // namespace

TEST_CASE("as-vars probe: the word that follows our speed is named by its path, and read on a puppet") {
    Memory m;
    m.Word(kOurs + 0x58, kOurVars);
    m.Float(kOurVars + 0x0, 0.0f);
    m.Word(kPuppet + 0x58, kPuppetVars);
    m.Float(kPuppetVars + 0x0, 0.0f);
    m.Float(kPuppetVars + 0x24, 0.0f);  // nothing writes it on the puppet
    CoopAsVars::Probe probe;
    std::vector<std::string> lines;
    float x = 0.0f;
    std::uint64_t now = 1000;
    for (int i = 0; i < 120; ++i) {
        // stand, walk, run, in turn: 0, 1.5, 5 m/s
        const float speed = (i / 10) % 3 == 0 ? 0.0f : (i / 10) % 3 == 1 ? 1.5f : 5.0f;
        x += speed * 0.1f;
        m.Float(kOurVars + 0x24, speed / 5.0f);    // a stick length 0 to 1
        m.Float(kOurVars + 0x28, (i % 7) * 1.0f);  // noise
        const auto got = probe.Look(m.Reader(), kOurs, {x, 0.0f, 0.0f}, now, {{"P2 CoopPuppet_Child_C003", kPuppet}});
        lines.insert(lines.end(), got.begin(), got.end());
        now += 100;
    }
    REQUIRE(!probe.Best().empty());
    const CoopAsVars::Spot best = probe.Best().front();
    CHECK(CoopAsVars::SpotText(best) == "ctl+0x58>:+0x24");
    bool puppet_line = false;
    for (const auto& line : lines) {
        if (line.rfind("P2 CoopPuppet_Child_C003:", 0) == 0) {
            puppet_line = true;
            CHECK(line.find("ctl+0x58>:+0x24 0.000") != std::string::npos);
        }
    }
    CHECK(puppet_line);
}

TEST_CASE("as-vars probe: no controller, no lines, standing still, no report") {
    Memory m;
    m.Word(kOurs + 0x58, kOurVars);
    CoopAsVars::Probe probe;
    CHECK(probe.Look(m.Reader(), 0, {0, 0, 0}, 1000, {}).empty());
    std::uint64_t now = 1000;
    std::size_t reports = 0;
    for (int i = 0; i < 100; ++i) {
        for (const auto& line : probe.Look(m.Reader(), kOurs, {0, 0, 0}, now, {})) {
            reports += line.find("reads, our speed") != std::string::npos;
        }
        now += 100;
    }
    CHECK(reports == 0);
}

TEST_CASE("as-vars writer: a character's AS gets each name's handle once, then the changed values (row 307)") {
    Memory m;
    constexpr std::uint64_t actor = 0x10000, comps = 0x11000, comp = 0x12000, inner = 0x13000, ctl = 0x14000;
    constexpr std::uint64_t as = 0x15000, vt = 0x16000, block = 0x17000;
    m.Word(actor + 0x270, comps);
    m.Word(comps + 0x40, comp);
    m.Word(comp + 0x18, inner);
    m.Word(inner + 0x18, ctl);
    m.Word(ctl + 0x40, as);
    m.Word(as, vt);
    m.Word(vt + 0x80, 0xf32500);
    m.Word(vt + 0x88, 0xb00500);
    m.Word(vt + 0x2f8, 0xf32400);
    m.Word(vt + 0x300, 0xb00400);
    m.Word(block, 0);
    const auto read = m.Reader();
    const CoopAsVars::Writer::Write write = [&m](std::uint64_t at, const void* in, std::size_t size) {
        m.Write(at, in, size);
        return true;
    };
    CoopAsVars::Writer w;
    CHECK(CoopAsVars::Writer::AsOf(read, actor) == as);
    CHECK_FALSE(w.Next(read, write, actor, block, std::nullopt, 1));  // an older sender: nothing
    const coop::protocol::AsVars v{1.0f, 6.25f, 6.5f, coop::protocol::kAsVarDash};
    // Lookups: f32 names through vt+0x2f8, bools through vt+0x300, x1 = the cell holding the name's address.
    const std::array<std::uint32_t, 5> handles{7, 9, 0xffffffffu, 3, 4};  // XZSpeed is not in this AS
    std::uint64_t frame = 10;
    for (std::size_t i = 0; i < 5; ++i) {
        if (i % 2 == 0) ++frame;  // two calls a frame
        const auto a = w.Next(read, write, actor, block, v, frame);
        REQUIRE(a);
        CHECK(a->function == (i < 3 ? 0xf32400u : 0xb00400u));
        CHECK(a->x0 == as);
        CHECK(a->x1 == block + CoopAsVars::block::kNamePtr);
        std::uint64_t text_at = 0;
        REQUIRE(read(a->x1, &text_at, 8));
        char name[0x20] = {};
        REQUIRE(read(text_at, name, sizeof name));
        CHECK(std::string(name) == CoopAsVars::kNames[i]);
        CHECK_FALSE(w.Finish(handles[i]).empty());
        if (i % 2 == 1) CHECK_FALSE(w.Next(read, write, actor, block, v, frame));  // the frame's 2 are used
    }
    CHECK(w.Handle(2) == -1);
    // Values: four (XZSpeed skipped), f32 by vt+0x80 with s0, bools by vt+0x88 with x2.
    std::vector<CoopAsVars::Ask> asks;
    for (int k = 0; k < 4; ++k) {
        const auto a = w.Next(read, write, actor, block, v, ++frame);
        REQUIRE(a);
        asks.push_back(*a);
        w.Finish(0);
    }
    CHECK_FALSE(w.Next(read, write, actor, block, v, ++frame));  // all written, none changed
    CHECK(asks[0].function == 0xf32500u);
    CHECK(asks[0].x1 == 7);
    CHECK(asks[0].s0 == 1.0f);
    CHECK(asks[1].x1 == 9);
    CHECK(asks[1].s0 == 6.25f);
    CHECK(asks[2].function == 0xb00500u);
    CHECK(asks[2].x1 == 3);
    CHECK(asks[2].x2 == 0);  // not IsLeftStickInput
    CHECK(asks[3].x1 == 4);
    CHECK(asks[3].x2 == 1);  // IsDash
    // A changed value is written again, alone.
    auto v2 = v;
    v2.linear_vel_xz = 2.0f;
    const auto again = w.Next(read, write, actor, block, v2, ++frame);
    REQUIRE(again);
    CHECK(again->x1 == 9);
    CHECK(again->s0 == 2.0f);
    w.Finish(0);
    CHECK_FALSE(w.Next(read, write, actor, block, v2, ++frame));
    // A new AS (the actor swapped): every name is looked up again.
    m.Word(ctl + 0x40, as + 0x800);
    m.Word(as + 0x800, vt);
    const auto look = w.Next(read, write, actor, block, v2, ++frame);
    REQUIRE(look);
    CHECK(look->function == 0xf32400u);
    CHECK(look->x0 == as + 0x800);
}
