// SPDX-License-Identifier: GPL-3.0-or-later
// Signature scan for the main offsets of a game build (issue 97,
// docs/game-builds.md "Signature scan"): searches main's code for short
// masked instruction patterns and decodes each offset from the one match.
//
// Header only and free of the emulator, so the unit tests run it. The
// patterns are generated from build 1.1.1 by tools/game_scan/make_patterns.py
// (coop_game_scan_patterns.inc); every bit a recompile or relink moves
// (branch targets, ADRP pages, the low 12 bits after an ADRP, the words the
// loader patches) is masked out.
//
// Rules: a pattern must match exactly once, else that offset is missing. The
// four cave offsets are not searched: they sit at fixed distances before
// rodata, as in 1.1.1, and the loader patch (CoopSvc::PatchMain) checks
// there is zero padding before it writes there.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "coop_game_build.h"

namespace CoopGameScan {

using CoopGameBuild::MainOffsets;

struct Word {
    std::uint32_t value;
    std::uint32_t mask;
};

enum class Kind : std::uint8_t {
    Site,  // the offset is the word at `at`
    Call,  // the `bl` at `at` calls the offset
    Page,  // the ADRP at `at` and the ADD/LDR/STR at `at2` reach the offset
    Got,   // as Page, but that is a GOT slot: its value plus the ADD at `at3`
};

struct Pattern {
    const char* name;
    std::uint64_t MainOffsets::*field;
    Kind kind;
    std::uint8_t at, at2, at3;
    const Word* words;
    std::size_t count;
};

namespace detail {
#define COOP_SCAN_PATTERN(field, kind, at, at2, at3, ...) \
    inline constexpr Word kWords_##field[] = {__VA_ARGS__};
#include "coop_game_scan_patterns.inc"
#undef COOP_SCAN_PATTERN
} // namespace detail

inline constexpr Pattern kPatterns[] = {
#define COOP_SCAN_PATTERN(field, kind, at, at2, at3, ...)                                     \
    {#field, &MainOffsets::field, Kind::kind, at, at2, at3, detail::kWords_##field, \
     std::size(detail::kWords_##field)},
#include "coop_game_scan_patterns.inc"
#undef COOP_SCAN_PATTERN
};

// Where the caves sit, counted back from the start of rodata (1.1.1:
// CoopSvc::kCaveOffset, CoopHold::kCaveOffset / kTableOffset, kCaveEnd).
inline constexpr std::uint64_t kSvcCaveBack = 0xd80;
inline constexpr std::uint64_t kHoldCaveBack = 0xd00;
inline constexpr std::uint64_t kHoldTableBack = 0xc00;

// rodata starts on the page that holds main's MOD0 header; text word 1 is
// its offset (the module header every NSO starts with).
inline std::uint64_t RodataStart(std::uint32_t mod0_offset) {
    return mod0_offset & ~std::uint64_t{0xfff};
}

inline std::int64_t Signed(std::uint64_t v, int bits) {
    const std::uint64_t sign = std::uint64_t{1} << (bits - 1);
    return static_cast<std::int64_t>((v ^ sign) - sign);
}

inline std::uint64_t BlTarget(std::uint64_t at, std::uint32_t w) {
    return at + static_cast<std::uint64_t>(Signed(w & 0x3ffffff, 26) * 4);
}

inline std::uint64_t AdrpPage(std::uint64_t at, std::uint32_t w) {
    const std::uint64_t imm = ((w >> 29) & 3) | (((w >> 5) & 0x7ffff) << 2);
    return (at & ~std::uint64_t{0xfff}) + static_cast<std::uint64_t>(Signed(imm, 21) * 4096);
}

// The low 12 bits an ADD imm or LDR/STR unsigned imm adds (scaled by size).
inline std::uint64_t Lo12(std::uint32_t w) {
    const std::uint64_t imm = (w >> 10) & 0xfff;
    if ((w & 0x7f000000) == 0x11000000) {
        return imm;
    }
    return imm << ((w >> 30) & 3);
}

struct Missing {
    const char* name;
    std::size_t hits;  // 0: not found; 2: more than one
};

struct Result {
    MainOffsets main{};
    std::vector<Missing> missing;
    bool Complete() const { return missing.empty(); }
};

// Matches of `p` in `text` (stops counting at 2); `first` gets the first.
inline std::size_t Find(const Pattern& p, const std::uint32_t* text, std::size_t words,
                        std::size_t& first) {
    if (p.count == 0 || words < p.count) {
        return 0;
    }
    std::size_t anchor = 0;
    int bits = -1;
    for (std::size_t i = 0; i < p.count; ++i) {
        const int b = std::popcount(p.words[i].mask);
        if (b > bits) {
            bits = b;
            anchor = i;
        }
    }
    const Word a = p.words[anchor];
    std::size_t hits = 0;
    const std::size_t last = words - p.count;
    for (std::size_t s = 0; s <= last; ++s) {
        if ((text[s + anchor] & a.mask) != a.value) {
            continue;
        }
        bool ok = true;
        for (std::size_t k = 0; k < p.count && ok; ++k) {
            ok = (text[s + k] & p.words[k].mask) == p.words[k].value;
        }
        if (ok) {
            if (hits++ == 0) {
                first = s;
            } else {
                return hits;
            }
        }
    }
    return hits;
}

// `text`: main's code as loaded, from offset 0 to rodata. `read_slot`
// gives the 8 bytes at a main offset, minus main's base (for GOT slots);
// nullopt if it cannot be read.
inline Result Scan(const std::uint32_t* text, std::size_t words, std::uint64_t rodata,
                   const std::function<std::optional<std::uint64_t>(std::uint64_t)>& read_slot) {
    Result r;
    for (const auto& p : kPatterns) {
        std::size_t s = 0;
        const std::size_t hits = Find(p, text, words, s);
        if (hits != 1) {
            r.missing.push_back({p.name, hits});
            continue;
        }
        const std::uint64_t i = s + p.at;
        std::uint64_t value = 0;
        switch (p.kind) {
        case Kind::Site:
            value = i * 4;
            break;
        case Kind::Call:
            value = BlTarget(i * 4, text[i]);
            break;
        case Kind::Page:
        case Kind::Got: {
            value = AdrpPage(i * 4, text[i]) + Lo12(text[s + p.at2]);
            if (p.kind == Kind::Got) {
                const auto slot = read_slot ? read_slot(value) : std::nullopt;
                if (!slot) {
                    r.missing.push_back({p.name, 0});
                    continue;
                }
                value = *slot + ((text[s + p.at3] >> 10) & 0xfff);
            }
            break;
        }
        }
        r.main.*p.field = value;
    }
    r.main.cave_end = rodata;
    r.main.svc_cave = rodata - kSvcCaveBack;
    r.main.hold_cave = rodata - kHoldCaveBack;
    r.main.hold_table = rodata - kHoldTableBack;
    return r;
}

// The fields that differ between two entries, "name (scan 0x.., table 0x..)".
inline std::vector<std::string> Differences(const MainOffsets& scan, const MainOffsets& table) {
    std::vector<std::string> out;
    const auto check = [&](const char* name, std::uint64_t MainOffsets::*f) {
        if (scan.*f != table.*f) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%s (scan %#llx, table %#llx)", name,
                          static_cast<unsigned long long>(scan.*f),
                          static_cast<unsigned long long>(table.*f));
            out.emplace_back(buf);
        }
    };
    for (const auto& p : kPatterns) {
        check(p.name, p.field);
    }
    check("cave_end", &MainOffsets::cave_end);
    check("svc_cave", &MainOffsets::svc_cave);
    check("hold_cave", &MainOffsets::hold_cave);
    check("hold_table", &MainOffsets::hold_table);
    return out;
}

// "a (none), b (2 or more)" for a log line.
inline std::string MissingText(const std::vector<Missing>& missing) {
    std::string out;
    for (const auto& m : missing) {
        if (!out.empty()) {
            out += ", ";
        }
        out += std::string(m.name) + (m.hits == 0 ? " (none)" : " (2 or more)");
    }
    return out;
}

} // namespace CoopGameScan
