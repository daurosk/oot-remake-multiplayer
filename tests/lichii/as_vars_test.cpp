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
