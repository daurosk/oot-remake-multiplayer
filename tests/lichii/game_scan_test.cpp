// Tests for the signature scan (lichii/hook/coop_game_scan.h, issue 97).
//
// The repo holds no game code, so the main test builds a main from the
// patterns alone: every pattern is written into an empty text, its masked
// bits filled so that it points where the 1.1.1 table says, and the scan
// must give back the whole table. With COOP_TEST_MAIN set to your own plain
// 1.1.1 main NSO, the last test scans the real one too.
#include <doctest/doctest.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include "coop_game_build.h"
#include "coop_game_scan.h"
#include "coop_hold.h"
#include "coop_svc.h"

using namespace CoopGameScan;

namespace {

const MainOffsets& Table() { return CoopGameBuild::kPrimary.main; }

constexpr std::uint64_t kRodata = 0x3c47000;  // 1.1.1, NSO header
constexpr std::uint32_t kMod0 = 0x3c4705c;    // 1.1.1 text word 1

void SetBits(std::uint32_t& w, std::uint32_t field, std::uint32_t mask, int shift) {
    w = (w & ~(mask << shift)) | ((field & mask) << shift);
}

void EncodeAdrp(std::uint32_t& w, std::uint64_t at, std::uint64_t target) {
    const std::int64_t delta = static_cast<std::int64_t>(target >> 12) - static_cast<std::int64_t>(at >> 12);
    const auto d = static_cast<std::uint32_t>(delta);
    SetBits(w, d & 3, 3, 29);
    SetBits(w, d >> 2, 0x7ffff, 5);
}

void EncodeLo12(std::uint32_t& w, std::uint64_t target) {
    std::uint32_t lo = target & 0xfff;
    if ((w & 0x7f000000) != 0x11000000) {
        lo >>= (w >> 30) & 3;  // LDR/STR scale
    }
    SetBits(w, lo, 0xfff, 10);
}

// A main of zeros with every pattern planted to give the 1.1.1 table.
struct Planted {
    std::vector<std::uint32_t> text = std::vector<std::uint32_t>(kRodata / 4, 0);
    std::map<std::uint64_t, std::uint64_t> slots;  // GOT slot -> value minus base
    std::size_t cursor = 0x400;                    // free words for non-Site patterns

    std::size_t Write(const Pattern& p, std::size_t s) {
        for (std::size_t k = 0; k < p.count; ++k) {
            text[s + k] = p.words[k].value;
        }
        return s;
    }

    void Plant(const Pattern& p) {
        const std::uint64_t target = Table().*p.field;
        if (p.kind == Kind::Site) {
            Write(p, target / 4 - p.at);
            return;
        }
        const std::size_t s = Write(p, cursor);
        cursor += p.count + 16;
        auto& w = text[s + p.at];
        const std::uint64_t at = (s + p.at) * 4;
        if (p.kind == Kind::Call) {
            SetBits(w, static_cast<std::uint32_t>((target - at) / 4), 0x3ffffff, 0);
            return;
        }
        std::uint64_t page_target = target;
        if (p.kind == Kind::Got) {
            page_target = 0x4f00000 + slots.size() * 8;
            std::uint32_t& add = text[s + p.at3];
            if (((add >> 10) & 0xfff) == 0) {
                SetBits(add, 0x10, 0xfff, 10);
            }
            slots[page_target] = target - ((add >> 10) & 0xfff);
        }
        EncodeAdrp(w, at, page_target);
        EncodeLo12(text[s + p.at2], page_target);
    }

    Result Run() const {
        return Scan(text.data(), text.size(), RodataStart(kMod0),
                    [this](std::uint64_t slot) -> std::optional<std::uint64_t> {
                        const auto it = slots.find(slot);
                        if (it == slots.end()) return std::nullopt;
                        return it->second;
                    });
    }
};

} // namespace

TEST_CASE("game scan: every main offset is a pattern or placed by the caves rule") {
    constexpr std::size_t kFields = sizeof(MainOffsets) / sizeof(std::uint64_t);
    std::set<std::string> names;
    for (const auto& p : kPatterns) {
        CHECK(names.insert(p.name).second);
        CHECK(p.count >= 8);   // short patterns may hit a wrong spot in another build
        CHECK(p.count <= 24);  // and long ones are game code, not a signature
        CHECK(p.at < p.count);
        CHECK(p.at2 < p.count);
        CHECK(p.at3 < p.count);
    }
    CHECK(std::size(kPatterns) + 4 == kFields);
    for (const char* derived : {"svc_cave", "cave_end", "hold_cave", "hold_table"}) {
        CHECK(names.count(derived) == 0);
    }
}

TEST_CASE("game scan: the hooked words are masked out of every pattern") {
    // The loader patches them before the game runs; the scan sees the patch.
    for (const std::uint64_t site :
         {Table().svc_hook_rare, Table().svc_hook_frame, Table().hold_hook}) {
        for (const auto& p : kPatterns) {
            if (p.kind == Kind::Site && Table().*p.field / 4 - p.at <= site / 4 &&
                site / 4 < Table().*p.field / 4 - p.at + p.count) {
                CHECK(p.words[site / 4 - (Table().*p.field / 4 - p.at)].mask == 0);
            }
        }
    }
}

TEST_CASE("game scan: rodata and the caves come out as in the 1.1.1 table") {
    CHECK(RodataStart(kMod0) == Table().cave_end);
    CHECK(Table().cave_end - kSvcCaveBack == CoopSvc::kCaveOffset);
    CHECK(Table().cave_end - kHoldCaveBack == CoopHold::kCaveOffset);
    CHECK(Table().cave_end - kHoldTableBack == CoopHold::kTableOffset);
}

TEST_CASE("game scan: a main built from the patterns scans to the 1.1.1 table") {
    Planted m;
    for (const auto& p : kPatterns) {
        m.Plant(p);
    }
    const Result r = m.Run();
    INFO("missing: " << MissingText(r.missing));
    CHECK(r.Complete());
    const auto diff = Differences(r.main, Table());
    for (const auto& d : diff) {
        INFO(d);
        CHECK(false);
    }
    CHECK(diff.empty());
}

TEST_CASE("game scan: a missing or doubled pattern is named, the rest still found") {
    Planted m;
    for (const auto& p : kPatterns) {
        if (std::strcmp(p.name, "set_time") != 0) {
            m.Plant(p);
        }
    }
    // malloc twice: the scan cannot tell which one is right.
    for (const auto& p : kPatterns) {
        if (std::strcmp(p.name, "malloc") == 0) {
            m.Write(p, 0x200000 / 4);
        }
    }
    const Result r = m.Run();
    CHECK_FALSE(r.Complete());
    REQUIRE(r.missing.size() == 2);
    const std::string text = MissingText(r.missing);
    CHECK(text.find("set_time (none)") != std::string::npos);
    CHECK(text.find("malloc (2 or more)") != std::string::npos);
    CHECK(r.main.create_actor == Table().create_actor);
}

TEST_CASE("game scan: an empty main finds nothing") {
    std::vector<std::uint32_t> text(0x10000, 0);
    const Result r = Scan(text.data(), text.size(), 0x40000, nullptr);
    CHECK(r.missing.size() == std::size(kPatterns));
}

TEST_CASE("game scan: decoders") {
    // Both svc hooks are `bl frame_step` in the unpatched game.
    for (const auto& hook : CoopSvc::kHooks) {
        CHECK(BlTarget(hook.offset, hook.original) == Table().frame_step);
    }
    CHECK(BlTarget(0x1000, 0x97ffffff) == 0xffc);         // bl .-4
    CHECK(AdrpPage(0x5000, 0x90000008) == 0x5000);        // adrp x8, .
    CHECK(AdrpPage(0x5abc, 0xb0000008) == 0x6000);        // adrp x8, .+1 page
    CHECK(Lo12(0x91004108) == 0x10);                      // add x8, x8, #0x10
    CHECK(Lo12(0xf9441d08) == 0x838);                     // ldr x8, [x8, #0x838]
    CHECK(Lo12(0xb9400108 | (3u << 10)) == 12);           // ldr w8, [x8, #12]
}

TEST_CASE("game scan: your own 1.1.1 main (COOP_TEST_MAIN, plain NSO)") {
    const char* path = std::getenv("COOP_TEST_MAIN");
    if (path == nullptr || *path == 0) {
        MESSAGE("COOP_TEST_MAIN not set: real main not scanned");
        return;
    }
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f);
    std::vector<char> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    REQUIRE(d.size() > 0x100);
    REQUIRE(std::memcmp(d.data(), "NSO0", 4) == 0);
    std::uint32_t flags = 0;
    std::memcpy(&flags, d.data() + 0x0c, 4);
    REQUIRE((flags & 7) == 0);  // plain, not compressed
    // Lay out the three segments; RELR keeps each slot's value in place.
    std::vector<std::uint8_t> mem(0x6000000, 0);
    std::uint32_t text_size = 0;
    for (int i = 0; i < 3; ++i) {
        std::uint32_t off = 0, at = 0, size = 0;
        std::memcpy(&off, d.data() + 0x10 + i * 0x10, 4);
        std::memcpy(&at, d.data() + 0x14 + i * 0x10, 4);
        std::memcpy(&size, d.data() + 0x18 + i * 0x10, 4);
        REQUIRE(off + size <= d.size());
        REQUIRE(at + size <= mem.size());
        std::memcpy(mem.data() + at, d.data() + off, size);
        if (i == 0) text_size = size;
    }
    std::uint32_t mod0 = 0;
    std::memcpy(&mod0, mem.data() + 4, 4);
    const std::uint64_t rodata = RodataStart(mod0);
    REQUIRE(rodata >= text_size);
    std::vector<std::uint32_t> text(rodata / 4);
    std::memcpy(text.data(), mem.data(), rodata);
    const Result r = Scan(text.data(), text.size(), rodata,
                          [&](std::uint64_t slot) -> std::optional<std::uint64_t> {
                              if (slot + 8 > mem.size()) return std::nullopt;
                              std::uint64_t v = 0;
                              std::memcpy(&v, mem.data() + slot, 8);
                              return v;
                          });
    INFO("missing: " << MissingText(r.missing));
    CHECK(r.Complete());
    for (const auto& diff : Differences(r.main, Table())) {
        INFO(diff);
        CHECK(false);
    }
}
