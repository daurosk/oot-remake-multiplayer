// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "coop_ocarina.h"

namespace {

// A fake game memory: our Link, the AS chain to the controller, two slots.
struct Memory {
    std::map<std::uint64_t, std::uint64_t> words;

    void Put(std::uint64_t at, std::uint64_t value) { words[at] = value; }
    void PutName(std::uint64_t at, const std::string& name) {
        std::string s = name;
        s.resize((s.size() / 8 + 1) * 8, '\0');
        for (std::size_t i = 0; i < s.size(); i += 8) {
            std::uint64_t w = 0;
            std::memcpy(&w, s.data() + i, 8);
            words[at + i] = w;
        }
    }
    CoopOcarina::OcarinaProbe::ReadWord Reader() const {
        return [this](std::uint64_t at) -> std::optional<std::uint64_t> {
            const auto it = words.find(at);
            return it == words.end() ? std::optional<std::uint64_t>(0) : std::optional<std::uint64_t>(it->second);
        };
    }
};

constexpr std::uint64_t kActor = 0x1000;
constexpr std::uint64_t kController = 0x5000;
constexpr std::uint64_t kSlot0 = 0x7000;
constexpr std::uint64_t kSlot1 = 0x8000;

Memory Link(const std::string& slot0, const std::string& slot1) {
    Memory m;
    m.Put(kActor + 0x270, 0x2000);
    m.Put(0x2000 + 0x40, 0x3000);
    m.Put(0x3000 + 0x18, 0x4000);
    m.Put(0x4000 + 0x18, kController);
    m.Put(kController + 0x18, 2);
    m.Put(kController + 0x20, 0x6000);
    m.Put(0x6000, kSlot0);
    m.Put(0x6000 + 0x48, kSlot1);
    m.Put(kSlot0 + 0x10, 0x9000);
    m.Put(kSlot1 + 0x10, 0x9100);
    m.PutName(0x9000, slot0);
    m.PutName(0x9100, slot1);
    return m;
}

bool Has(const std::vector<std::string>& lines, const std::string& part) {
    for (const auto& l : lines) {
        if (l.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("ocarina: note names are the game's SLink assets") {
    CHECK(CoopOcarina::NoteIndex("Ocarina_A") != 0);
    CHECK(CoopOcarina::NoteIndex("HighF") != 0);
    CHECK(CoopOcarina::NoteIndex("Ocarina_A") != CoopOcarina::NoteIndex("Ocarina_Asharp"));
    CHECK(CoopOcarina::NoteIndex("Ocarina_ZeldaLullaby") == 0);  // a song, not a note
    CHECK(CoopOcarina::NoteNames()[CoopOcarina::NoteIndex("Ocarina_Dsharp")] == "Dsharp");
}

TEST_CASE("ocarina: the probe says nothing until our Link plays the ocarina") {
    CoopOcarina::OcarinaProbe probe;
    Memory m = Link("Wait", "UpperWait");
    CHECK(probe.Look(m.Reader(), kActor, 100'000).empty());
    CHECK_FALSE(probe.Active());
    m.Put(kSlot1 + 0x30, 1);  // steps while not playing: not watched
    CHECK(probe.Look(m.Reader(), kActor, 200'000).empty());
}

TEST_CASE("ocarina: the probe logs the slots and the small words that step with each note") {
    CoopOcarina::OcarinaProbe probe;
    Memory m = Link("Ocarina", "Ocarina_Child_B_lp");
    auto lines = probe.Look(m.Reader(), kActor, 100'000);
    CHECK(probe.Active());
    CHECK(Has(lines, "plays the ocarina, 2 AS slots"));
    CHECK(Has(lines, "0=Ocarina 1=Ocarina_Child_B_lp"));
    // A note: a small word steps, the finger slot changes; a pointer-sized change is ignored.
    m.Put(kSlot1 + 0x30, 13);
    m.Put(kController + 0x100, 0x123456789);
    m.PutName(0x9100, "Ocarina_Child_A_lp");
    lines = probe.Look(m.Reader(), kActor, 200'000);
    CHECK(Has(lines, "1=Ocarina_Child_A_lp"));
    CHECK(Has(lines, "slot1+0x30:0>13"));
    CHECK_FALSE(Has(lines, "ctl+0x100"));
    // Too soon: the next look waits kEveryUs.
    m.Put(kSlot1 + 0x30, 14);
    CHECK(probe.Look(m.Reader(), kActor, 210'000).empty());
    CHECK(Has(probe.Look(m.Reader(), kActor, 300'000), "slot1+0x30:13>14"));
    // Put away.
    m.PutName(0x9000, "Wait");
    m.PutName(0x9100, "UpperWait");
    CHECK(Has(probe.Look(m.Reader(), kActor, 400'000), "ocarina put away"));
    CHECK_FALSE(probe.Active());
}

TEST_CASE("ocarina: a word that steps all the time is muted") {
    CoopOcarina::OcarinaProbe probe;
    Memory m = Link("Ocarina", "Ocarina_Child_B_lp");
    probe.Look(m.Reader(), kActor, 100'000);
    int seen = 0;
    for (int i = 1; i <= 60; ++i) {
        m.Put(kController + 0x40, static_cast<std::uint64_t>(i % 2));
        seen += Has(probe.Look(m.Reader(), kActor, 100'000 + 100'000ull * i), "ctl+0x40") ? 1 : 0;
    }
    CHECK(seen == CoopOcarina::OcarinaProbe::kNoisySteps);
}

namespace {

// Our Link with an EquipmentUser whose armor link 5 holds the Ocarina.
constexpr std::uint64_t kLink = 0x10000;
constexpr std::uint64_t kEu = 0x20000;
constexpr std::uint64_t kOcarina = 0x30000;
constexpr std::uint64_t kOcarinaParts = 0x38000;

Memory Belt() {
    using P = CoopOcarina::HoldProbe;
    Memory m;
    m.Put(kLink + P::kParts, 0x11000);
    m.Put(0x11000 + P::kPartsEquipment, kEu);
    for (std::uint32_t i = 0; i < P::kArmorLinkCount; ++i) {
        m.Put(kEu + P::kArmorLinks + P::kLinkStride * i + P::kLinkId, 0xffffffffu);
    }
    const std::uint64_t link5 = kEu + P::kArmorLinks + P::kLinkStride * 5;
    m.Put(link5 + P::kLinkId, 0x2f2);
    m.Put(link5 + P::kLinkHolder, 0x12000);
    m.Put(0x12000 + P::kHolderActor, kOcarina);
    m.Put(kOcarina + P::kActorName, 0x13000);
    m.PutName(0x13000, "Item_Ocarina_Fairy");
    m.Put(kOcarina + P::kParts, kOcarinaParts);
    m.Put(kOcarinaParts + 0x10, 0x39000);  // a component block (same heap)
    m.Put(kOcarinaParts + 0x18, 0x5);      // not a pointer
    return m;
}

} // namespace

TEST_CASE("ocarina hold probe: finds the Ocarina on the belt and its blocks") {
    CoopOcarina::HoldProbe probe;
    Memory m = Belt();
    const auto regions = probe.Regions(m.Reader(), kLink);
    REQUIRE(regions.size() == 4);
    CHECK(regions[0].base == kEu);
    CHECK(regions[1].base == kOcarina);
    CHECK(regions[2].base == kOcarinaParts);
    CHECK(regions[3].base == 0x39000);
    CHECK(regions[3].name == "ocaparts+0x10");
    m.PutName(0x13000, "Item_Ocarina_Fairy_Holder");  // only a holder: nothing
    CHECK(probe.Regions(m.Reader(), kLink).empty());
}

TEST_CASE("ocarina hold probe: logs the words that change while playing and come back") {
    CoopOcarina::HoldProbe probe;
    Memory m = Belt();
    CHECK(probe.Step(m.Reader(), kLink, false, 1'000'000).empty());
    auto lines = probe.Step(m.Reader(), kLink, true, 2'000'000);
    CHECK(Has(lines, "ocarina hold probe: playing; 4 regions"));
    m.Put(kEu + 0x500, 7);           // the bind: changes, comes back
    m.Put(0x39000 + 0x40, 0xabc);    // a counter: changes, stays
    lines = probe.Step(m.Reader(), kLink, true, 3'600'000);
    CHECK(Has(lines, "during"));
    CHECK(probe.Step(m.Reader(), kLink, false, 4'000'000).empty());
    m.Put(kEu + 0x500, 0);
    CHECK(probe.Step(m.Reader(), kLink, false, 4'500'000).empty());  // settling
    lines = probe.Step(m.Reader(), kLink, false, 5'600'000);
    CHECK(Has(lines, "eu 0x20000+0x500: 0x0 > 0x7"));
    CHECK_FALSE(Has(lines, "0x39000+0x40"));
    CHECK(Has(lines, "1 words changed"));
    CHECK(probe.Step(m.Reader(), kLink, true, 9'000'000).empty());  // once per session
}
