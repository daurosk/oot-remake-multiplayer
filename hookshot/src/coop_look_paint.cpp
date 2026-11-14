// SPDX-License-Identifier: GPL-3.0-or-later
// coop_look_paint.h. The ASTC part follows tools/puppet_pack/texture.py (the
// decoder's pieces) and tunic_extra.py (the rewrite) step by step, so both
// give the same bytes; tests/hookshot/look_paint_test.cpp checks blocks made
// by the Python tool.
#include "coop_look_paint.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <tuple>

namespace CoopLookPaint {
namespace {

// ---- colour maths (sRGB 0..255 <-> linear 0..1), as tunic.py ----

double Lin(int c) {
    const double x = c / 255.0;
    return x <= 0.04045 ? x / 12.92 : std::pow((x + 0.055) / 1.055, 2.4);
}

// Python's round(): half to even (the default rounding mode).
int Round(double x) { return static_cast<int>(std::nearbyint(x)); }

std::uint8_t Srgb(double x) {
    x = std::clamp(x, 0.0, 1.0);
    const double c = x <= 0.0031308 ? x * 12.92 : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(Round(c * 255));
}

int Cl(double x) { return std::clamp(Round(x), 0, 255); }

double Hue(const Rgb& c) {
    const int r = c[0], g = c[1], b = c[2];
    const int hi = std::max({r, g, b}), lo = std::min({r, g, b});
    const double d = hi - lo;
    if (hi == r) {
        return std::fmod(std::fmod(60 * (g - b) / d, 360.0) + 360.0, 360.0);
    }
    if (hi == g) {
        return 60 * (b - r) / d + 120;
    }
    return 60 * (r - g) / d + 240;
}

Rgb Unpack565(std::uint16_t v) {
    const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return {static_cast<std::uint8_t>((r << 3) | (r >> 2)), static_cast<std::uint8_t>((g << 2) | (g >> 4)),
            static_cast<std::uint8_t>((b << 3) | (b >> 2))};
}

std::uint16_t Pack565(const Rgb& c) {
    return static_cast<std::uint16_t>(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 |
                                      ((c[2] * 31 + 127) / 255));
}

Rgb RgbOf(std::uint32_t colour) {
    return {static_cast<std::uint8_t>(colour >> 16), static_cast<std::uint8_t>(colour >> 8),
            static_cast<std::uint8_t>(colour)};
}

// Which rule paints an end colour: the first whose tones match; -1 none.
struct Painter {
    const std::vector<Rule>& rules;
    std::vector<double> base;    // each rule's average linear luma in the texture
    std::vector<Rgb> target;

    int Match(const Rgb& c) const {
        const std::uint32_t tone = ToneOf(c);
        for (std::size_t i = 0; i < rules.size(); ++i) {
            if ((rules[i].tones & tone) != 0) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
    bool Usable(int rule) const { return rule >= 0 && base[static_cast<std::size_t>(rule)] > 0; }
    int Paints(const Rgb& c) const {
        const int r = Match(c);
        return Usable(r) ? r : -1;
    }
    Rgb Paint(const Rgb& c, int rule) const {
        return Shade(c, target[static_cast<std::size_t>(rule)], base[static_cast<std::size_t>(rule)]);
    }
};

// ---- a 128-bit block ----

struct V128 {
    std::uint64_t lo = 0, hi = 0;
    bool operator==(const V128&) const = default;
};

V128 Load(const std::uint8_t* p) {
    V128 v;
    std::memcpy(&v.lo, p, 8);
    std::memcpy(&v.hi, p + 8, 8);
    return v;   // (little-endian hosts only: Windows and Linux on x86-64 / arm64)
}

void Store(const V128& v, std::uint8_t* p) {
    std::memcpy(p, &v.lo, 8);
    std::memcpy(p + 8, &v.hi, 8);
}

V128 Shr(const V128& v, int s) {
    if (s <= 0) {
        return v;
    }
    if (s >= 128) {
        return {};
    }
    if (s >= 64) {
        return {v.hi >> (s - 64), 0};
    }
    return {(v.lo >> s) | (v.hi << (64 - s)), v.hi >> s};
}

V128 Shl(const V128& v, int s) {
    if (s <= 0) {
        return v;
    }
    if (s >= 128) {
        return {};
    }
    if (s >= 64) {
        return {0, v.lo << (s - 64)};
    }
    return {v.lo << s, (v.hi << s) | (v.lo >> (64 - s))};
}

V128 Mask(int n) {   // n low bits set
    if (n <= 0) {
        return {};
    }
    if (n >= 128) {
        return {~0ULL, ~0ULL};
    }
    if (n >= 64) {
        return {~0ULL, n == 64 ? 0 : (~0ULL >> (128 - n))};
    }
    return {(1ULL << n) - 1, 0};
}

V128 And(const V128& a, const V128& b) { return {a.lo & b.lo, a.hi & b.hi}; }
V128 Or(const V128& a, const V128& b) { return {a.lo | b.lo, a.hi | b.hi}; }
V128 Not(const V128& a) { return {~a.lo, ~a.hi}; }

int Bits(const V128& v, int lo, int n) {
    return static_cast<int>(Shr(v, lo).lo & ((n >= 64) ? ~0ULL : ((1ULL << n) - 1)));
}

V128 WithBits(const V128& v, int at, int n, std::uint64_t x) {
    const V128 field = Shl(Mask(n), at);
    return Or(And(v, Not(field)), And(Shl(V128{x, 0}, at), field));
}

int B(int v, int lo, int n) { return (v >> lo) & ((1 << n) - 1); }

// ---- integer sequence encoding (texture.py) ----

struct Ise {
    int t, q, b;
};

std::optional<Ise> IseOf(int range) {
    switch (range) {
    case 2: return Ise{0, 0, 1};
    case 3: return Ise{1, 0, 0};
    case 4: return Ise{0, 0, 2};
    case 5: return Ise{0, 1, 0};
    case 6: return Ise{1, 0, 1};
    case 8: return Ise{0, 0, 3};
    case 10: return Ise{0, 1, 1};
    case 12: return Ise{1, 0, 2};
    case 16: return Ise{0, 0, 4};
    case 20: return Ise{0, 1, 2};
    case 24: return Ise{1, 0, 3};
    case 32: return Ise{0, 0, 5};
    case 40: return Ise{0, 1, 3};
    case 48: return Ise{1, 0, 4};
    case 64: return Ise{0, 0, 6};
    case 80: return Ise{0, 1, 4};
    case 96: return Ise{1, 0, 5};
    case 128: return Ise{0, 0, 7};
    case 160: return Ise{0, 1, 5};
    case 192: return Ise{1, 0, 6};
    case 256: return Ise{0, 0, 8};
    default: return std::nullopt;
    }
}

constexpr int kWeightRanges[] = {2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32};
constexpr int kColourRanges[] = {2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 24, 32, 40, 48, 64, 80, 96, 128, 160, 192, 256};

int IseBits(int count, int range) {
    const Ise e = *IseOf(range);
    if (e.t) {
        return (8 * count + 4) / 5 + count * e.b;
    }
    if (e.q) {
        return (7 * count + 2) / 3 + count * e.b;
    }
    return count * e.b;
}

std::array<int, 5> Trits(int T) {
    int C, t4, t3;
    if (B(T, 2, 3) == 7) {
        C = (B(T, 5, 3) << 2) | B(T, 0, 2);
        t4 = t3 = 2;
    } else {
        C = B(T, 0, 5);
        if (B(T, 5, 2) == 3) {
            t4 = 2;
            t3 = B(T, 7, 1);
        } else {
            t4 = B(T, 7, 1);
            t3 = B(T, 5, 2);
        }
    }
    int t2, t1, t0;
    if (B(C, 0, 2) == 3) {
        t2 = 2;
        t1 = B(C, 4, 1);
        t0 = (B(C, 3, 1) << 1) | (B(C, 2, 1) & ~B(C, 3, 1) & 1);
    } else if (B(C, 2, 2) == 3) {
        t2 = 2;
        t1 = 2;
        t0 = B(C, 0, 2);
    } else {
        t2 = B(C, 4, 1);
        t1 = B(C, 2, 2);
        t0 = (B(C, 1, 1) << 1) | (B(C, 0, 1) & ~B(C, 1, 1) & 1);
    }
    return {t0, t1, t2, t3, t4};
}

std::array<int, 3> Quints(int Q) {
    if (B(Q, 1, 2) == 3 && B(Q, 5, 2) == 0) {
        const int q2 = (B(Q, 0, 1) << 2) | ((B(Q, 4, 1) & ~B(Q, 0, 1) & 1) << 1) | (B(Q, 3, 1) & ~B(Q, 0, 1) & 1);
        return {4, 4, q2};
    }
    int q2, C;
    if (B(Q, 1, 2) == 3) {
        q2 = 4;
        C = (B(Q, 3, 2) << 3) | ((~B(Q, 5, 2) & 3) << 1) | B(Q, 0, 1);
    } else {
        q2 = B(Q, 5, 2);
        C = B(Q, 0, 5);
    }
    int q1, q0;
    if (B(C, 0, 3) == 5) {
        q1 = 4;
        q0 = B(C, 3, 2);
    } else {
        q1 = B(C, 3, 2);
        q0 = B(C, 0, 3);
    }
    return {q0, q1, q2};
}

struct Digit {
    int d = -1;   // the trit or quint; -1 none (a plain bits range)
    int m = 0;    // the low bits
    bool operator==(const Digit&) const = default;
};

constexpr std::pair<int, int> kTritSpans[] = {{2, 0}, {2, 2}, {1, 4}, {2, 5}, {1, 7}};
constexpr std::pair<int, int> kQuintSpans[] = {{3, 0}, {2, 3}, {2, 5}};

// `count` values from bit `start`; bits past the sequence's end read as zero.
std::vector<Digit> IseRead(const V128& v, int start, int count, int range) {
    const Ise e = *IseOf(range);
    const int length = IseBits(count, range);
    const V128 vm = And(Shr(v, start), Mask(length));
    std::vector<Digit> out;
    int pos = 0;
    if (e.t || e.q) {
        const auto* spans = e.t ? kTritSpans : kQuintSpans;
        const int n_spans = e.t ? 5 : 3;
        while (static_cast<int>(out.size()) < count) {
            int m[5] = {};
            int code = 0;
            for (int i = 0; i < n_spans; ++i) {
                m[i] = Bits(vm, pos, e.b);
                pos += e.b;
                code |= Bits(vm, pos, spans[i].first) << spans[i].second;
                pos += spans[i].first;
            }
            if (e.t) {
                const auto d = Trits(code);
                for (int i = 0; i < 5; ++i) out.push_back({d[i], m[i]});
            } else {
                const auto d = Quints(code);
                for (int i = 0; i < 3; ++i) out.push_back({d[i], m[i]});
            }
        }
        out.resize(static_cast<std::size_t>(count));
        return out;
    }
    for (int i = 0; i < count; ++i) {
        out.push_back({-1, Bits(vm, pos, e.b)});
        pos += e.b;
    }
    return out;
}

int Replicate(int value, int bits, int to) {
    if (bits == 0) {
        return 0;
    }
    int out = 0, have = 0;
    while (have < to) {
        out = (out << bits) | value;
        have += bits;
    }
    return out >> (have - to);
}

int UnqColour(int d, int m, int range) {
    const Ise e = *IseOf(range);
    const int b = e.b;
    if (!e.t && !e.q) {
        return Replicate(m, b, 8);
    }
    const int A = (m & 1) ? 0x1FF : 0;
    const int bb = (m >> 1) & 1, c = (m >> 2) & 1, dd = (m >> 3) & 1, ee = (m >> 4) & 1, f = (m >> 5) & 1;
    int C = 0, Bv = 0;
    if (e.t) {
        static constexpr int kC[] = {0, 204, 93, 44, 22, 11, 5};
        C = kC[b];
        switch (b) {
        case 2: Bv = (bb << 8) | (bb << 4) | (bb << 2) | (bb << 1); break;
        case 3: Bv = (c << 8) | (bb << 7) | (c << 3) | (bb << 2) | (c << 1) | bb; break;
        case 4: Bv = (dd << 8) | (c << 7) | (bb << 6) | (dd << 2) | (c << 1) | bb; break;
        case 5: Bv = (ee << 8) | (dd << 7) | (c << 6) | (bb << 5) | (ee << 1) | dd; break;
        case 6: Bv = (f << 8) | (ee << 7) | (dd << 6) | (c << 5) | (bb << 4) | f; break;
        default: Bv = 0; break;
        }
    } else {
        static constexpr int kC[] = {0, 113, 54, 26, 13, 6};
        C = kC[b];
        switch (b) {
        case 2: Bv = (bb << 8) | (bb << 3) | (bb << 2); break;
        case 3: Bv = (c << 8) | (bb << 7) | (c << 2) | (bb << 1) | c; break;
        case 4: Bv = (dd << 8) | (c << 7) | (bb << 6) | (dd << 1) | c; break;
        case 5: Bv = (ee << 8) | (dd << 7) | (c << 6) | (bb << 5) | ee; break;
        default: Bv = 0; break;
        }
    }
    const int T = (d * C + Bv) ^ A;
    return (A & 0x80) | (T >> 2);
}

std::vector<int> DecodeVals(const std::vector<Digit>& dm, int range) {
    std::vector<int> out;
    out.reserve(dm.size());
    for (const Digit& x : dm) {
        out.push_back(UnqColour(x.d < 0 ? 0 : x.d, x.m, range));
    }
    return out;
}

using Rgba = std::array<int, 4>;
using EndPair = std::pair<Rgba, Rgba>;

std::pair<int, int> Bts(int a, int b) {   // bit_transfer_signed: (signed a, b)
    b = (b >> 1) | (a & 0x80);
    a = (a >> 1) & 0x3F;
    if (a & 0x20) {
        a -= 0x40;
    }
    return {a, b};
}

int Clamp255(int v) { return std::clamp(v, 0, 255); }

Rgba Blue(int r, int g, int b, int a) { return {(r + b) >> 1, (g + b) >> 1, b, a}; }

std::optional<EndPair> Endpoints(int mode, const std::vector<int>& v) {
    switch (mode) {
    case 0: return EndPair{{v[0], v[0], v[0], 255}, {v[1], v[1], v[1], 255}};
    case 1: {
        const int l0 = (v[0] >> 2) | (v[1] & 0xC0);
        const int l1 = std::min(l0 + (v[1] & 0x3F), 255);
        return EndPair{{l0, l0, l0, 255}, {l1, l1, l1, 255}};
    }
    case 4: return EndPair{{v[0], v[0], v[0], v[2]}, {v[1], v[1], v[1], v[3]}};
    case 5: {
        const auto [d1, b0] = Bts(v[1], v[0]);
        const auto [d3, b2] = Bts(v[3], v[2]);
        const int l1 = Clamp255(b0 + d1), a1 = Clamp255(b2 + d3);
        return EndPair{{b0, b0, b0, b2}, {l1, l1, l1, a1}};
    }
    case 6:
    case 10: {
        const int a0 = mode == 10 ? v[4] : 255, a1 = mode == 10 ? v[5] : 255;
        return EndPair{{(v[0] * v[3]) >> 8, (v[1] * v[3]) >> 8, (v[2] * v[3]) >> 8, a0}, {v[0], v[1], v[2], a1}};
    }
    case 8:
    case 12: {
        const int a0 = mode == 12 ? v[6] : 255, a1 = mode == 12 ? v[7] : 255;
        if (v[1] + v[3] + v[5] >= v[0] + v[2] + v[4]) {
            return EndPair{{v[0], v[2], v[4], a0}, {v[1], v[3], v[5], a1}};
        }
        return EndPair{Blue(v[1], v[3], v[5], a1), Blue(v[0], v[2], v[4], a0)};
    }
    case 9:
    case 13: {
        const auto [d1, b0] = Bts(v[1], v[0]);
        const auto [d3, b2] = Bts(v[3], v[2]);
        const auto [d5, b4] = Bts(v[5], v[4]);
        int d7 = 0, b6 = 255;
        if (mode == 13) {
            std::tie(d7, b6) = Bts(v[7], v[6]);
        }
        const Rgba sum{Clamp255(b0 + d1), Clamp255(b2 + d3), Clamp255(b4 + d5), Clamp255(b6 + d7)};
        if (d1 + d3 + d5 >= 0) {
            return EndPair{{b0, b2, b4, b6}, sum};
        }
        return EndPair{Blue(sum[0], sum[1], sum[2], sum[3]), Blue(b0, b2, b4, b6)};
    }
    default: return std::nullopt;   // HDR modes
    }
}

struct BlockMode {
    int w, h;
    bool dual;
    int range;
};

std::optional<BlockMode> ModeOf(int mode) {
    int R = (mode >> 4) & 1;
    int H = (mode >> 9) & 1;
    int D = (mode >> 10) & 1;
    const int A = (mode >> 5) & 3;
    int w = 0, h = 0;
    if (mode & 3) {
        R |= (mode & 3) << 1;
        const int Bb = (mode >> 7) & 3;
        const int kind = (mode >> 2) & 3;
        if (kind == 0) {
            w = Bb + 4, h = A + 2;
        } else if (kind == 1) {
            w = Bb + 8, h = A + 2;
        } else if (kind == 2) {
            w = A + 2, h = Bb + 8;
        } else if (mode & 0x100) {
            w = (Bb & 1) + 2, h = A + 2;
        } else {
            w = A + 2, h = (Bb & 1) + 6;
        }
    } else {
        R |= ((mode >> 2) & 3) << 1;
        if (((mode >> 2) & 3) == 0) {
            return std::nullopt;
        }
        const int Bb = (mode >> 9) & 3;
        const int kind = (mode >> 7) & 3;
        if (kind == 0) {
            w = 12, h = A + 2;
        } else if (kind == 1) {
            w = A + 2, h = 12;
        } else if (kind == 2) {
            w = A + 6, h = Bb + 6;
            D = H = 0;
        } else if (A == 0) {
            w = 6, h = 10;
        } else if (A == 1) {
            w = 10, h = 6;
        } else {
            return std::nullopt;
        }
    }
    const int q = R - 2 + 6 * H;
    if (q < 0 || q >= static_cast<int>(std::size(kWeightRanges))) {
        return std::nullopt;
    }
    return BlockMode{w, h, D != 0, kWeightRanges[q]};
}

struct Layout {
    int cem_at = -1;   // the one mode field of every partition; -1: none (a mode per partition)
    std::vector<int> cems;
    int cstart = 0, nvals = 0, range = 0;
};

std::optional<Layout> LayoutOf(const V128& v) {
    const auto bm = ModeOf(Bits(v, 0, 11));
    if (!bm) {
        return std::nullopt;
    }
    const int parts = Bits(v, 11, 2) + 1;
    const int wcount = bm->w * bm->h * (bm->dual ? 2 : 1);
    if (wcount > 64 || (bm->dual && parts == 4)) {
        return std::nullopt;
    }
    int below = 128 - IseBits(wcount, bm->range);
    Layout lay;
    if (parts == 1) {
        lay.cems = {Bits(v, 13, 4)};
        lay.cstart = 17;
        lay.cem_at = 13;
    } else {
        lay.cstart = 29;
        int enc = Bits(v, 23, 6);
        if ((enc & 3) == 0) {
            lay.cems.assign(static_cast<std::size_t>(parts), (enc >> 2) & 0xF);
            lay.cem_at = 25;
        } else {
            const int extra = 3 * parts - 4;
            below -= extra;
            if (below < lay.cstart) {
                return std::nullopt;
            }
            enc |= Bits(v, below, extra) << 6;
            const int base = (enc & 3) - 1;
            for (int i = 0; i < parts; ++i) {
                lay.cems.push_back(((base + ((enc >> (2 + i)) & 1)) << 2) | ((enc >> (2 + parts + 2 * i)) & 3));
            }
        }
    }
    if (bm->dual) {
        below -= 2;
    }
    for (const int c : lay.cems) {
        lay.nvals += 2 * ((c >> 2) + 1);
    }
    if (lay.nvals > 18) {
        return std::nullopt;
    }
    for (auto it = std::rbegin(kColourRanges); it != std::rend(kColourRanges); ++it) {
        if (IseBits(lay.nvals, *it) <= below - lay.cstart) {
            lay.range = *it;
            const Ise e = *IseOf(lay.range);
            if ((e.t || e.q) && e.b == 0) {
                return std::nullopt;   // (no encoder makes colours in ranges 3 and 5)
            }
            return lay;
        }
    }
    return std::nullopt;
}

bool VoidExtent(const V128& v) { return Bits(v, 0, 9) == 0x1FC; }

Rgb Rgb3(const Rgba& c) {
    return {static_cast<std::uint8_t>(c[0]), static_cast<std::uint8_t>(c[1]), static_cast<std::uint8_t>(c[2])};
}

std::vector<Rgba> AstcEndsOf(const V128& v) {
    if (VoidExtent(v)) {
        return {{Bits(v, 64, 16) >> 8, Bits(v, 80, 16) >> 8, Bits(v, 96, 16) >> 8, Bits(v, 112, 16) >> 8}};
    }
    const auto lay = LayoutOf(v);
    if (!lay) {
        return {};
    }
    const auto vals = DecodeVals(IseRead(v, lay->cstart, lay->nvals, lay->range), lay->range);
    std::vector<Rgba> out;
    std::size_t k = 0;
    for (const int c : lay->cems) {
        const std::size_t n = static_cast<std::size_t>(2 * ((c >> 2) + 1));
        if (const auto e = Endpoints(c, std::vector<int>(vals.begin() + k, vals.begin() + k + n))) {
            out.push_back(e->first);
            out.push_back(e->second);
        }
        k += n;
    }
    return out;
}

// The colour range's nearest (digit, low bits) for each value 0..255 (tunic_extra._quant).
const std::array<Digit, 256>& Quant(int range) {
    static std::array<std::optional<std::array<Digit, 256>>, 257> cache;   // (one per range; built on first use)
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    auto& q = cache[static_cast<std::size_t>(range)];
    if (!q) {
        const Ise e = *IseOf(range);
        std::vector<std::pair<int, Digit>> cands;
        const int digits = e.t ? 3 : e.q ? 5 : 1;
        for (int d = 0; d < digits; ++d) {
            for (int m = 0; m < (1 << e.b); ++m) {
                const Digit x{(e.t || e.q) ? d : -1, m};
                cands.push_back({UnqColour(d, m, range), x});
            }
        }
        std::array<Digit, 256> table{};
        for (int v = 0; v < 256; ++v) {
            int best = -1;
            for (const auto& [value, x] : cands) {
                if (best < 0 || std::abs(value - v) < best) {
                    best = std::abs(value - v);
                    table[static_cast<std::size_t>(v)] = x;
                }
            }
        }
        q = table;
    }
    return *q;
}

// The trit (8 bits) or quint (7 bits) code whose first n digits are these and
// whose bits in `outside` are zero; -1 none.
int GroupCode(bool trit, const std::array<int, 5>& digits, int n, int outside) {
    const int width = trit ? 8 : 7;
    for (int x = 0; x < (1 << width); ++x) {
        if (x & outside) {
            continue;
        }
        bool same = true;
        if (trit) {
            const auto d = Trits(x);
            for (int i = 0; i < n && same; ++i) same = d[i] == digits[i];
        } else {
            const auto d = Quints(x);
            for (int i = 0; i < n && same; ++i) same = d[i] == digits[i];
        }
        if (same) {
            return x;
        }
    }
    return -1;
}

std::optional<std::pair<V128, int>> IseEncode(const std::vector<Digit>& vals, int range) {
    const Ise e = *IseOf(range);
    const int count = static_cast<int>(vals.size());
    const int length = IseBits(count, range);
    V128 out;
    int pos = 0;
    if (!e.t && !e.q) {
        for (const Digit& x : vals) {
            out = Or(out, Shl(V128{static_cast<std::uint64_t>(x.m), 0}, pos));
            pos += e.b;
        }
        return std::pair{And(out, Mask(length)), length};
    }
    const auto* spans = e.t ? kTritSpans : kQuintSpans;
    const int per = e.t ? 5 : 3;
    for (int g = 0; g < count; g += per) {
        const int n = std::min(per, count - g);
        std::array<int, 5> digits{};
        std::array<int, 5> ms{};
        for (int i = 0; i < n; ++i) {
            digits[static_cast<std::size_t>(i)] = std::max(0, vals[static_cast<std::size_t>(g + i)].d);
            ms[static_cast<std::size_t>(i)] = vals[static_cast<std::size_t>(g + i)].m;
        }
        // where each code bit lands, to keep the ones past the end zero
        int p = pos, outside = 0;
        for (int i = 0; i < per; ++i) {
            p += e.b;
            for (int k = 0; k < spans[i].first; ++k) {
                if (p + k >= length) {
                    outside |= 1 << (spans[i].second + k);
                }
            }
            p += spans[i].first;
        }
        const int code = GroupCode(e.t != 0, digits, n, outside);
        if (code < 0) {
            return std::nullopt;
        }
        for (int i = 0; i < per; ++i) {
            out = Or(out, Shl(V128{static_cast<std::uint64_t>(ms[static_cast<std::size_t>(i)]), 0}, pos));
            pos += e.b;
            const int w = spans[i].first, sh = spans[i].second;
            out = Or(out, Shl(V128{static_cast<std::uint64_t>((code >> sh) & ((1 << w) - 1)), 0}, pos));
            pos += w;
        }
    }
    return std::pair{And(out, Mask(length)), length};
}

std::vector<std::vector<int>> Direct(const Rgb& n0, const Rgb& n1, int a0, int a1, bool alpha) {
    auto plain = [&](const std::array<int, 3>& x0, const std::array<int, 3>& x1) {
        std::vector<int> v{x0[0], x1[0], x0[1], x1[1], x0[2], x1[2]};
        if (alpha) {
            v.push_back(a0);
            v.push_back(a1);
        }
        return v;
    };
    auto ints = [](const Rgb& c) { return std::array<int, 3>{c[0], c[1], c[2]}; };
    auto scaled = [](const Rgb& c, int total) {
        int s = c[0] + c[1] + c[2];
        if (s == 0) {
            s = 1;
        }
        return std::array<int, 3>{Cl(static_cast<double>(c[0]) * total / s), Cl(static_cast<double>(c[1]) * total / s),
                                  Cl(static_cast<double>(c[2]) * total / s)};
    };
    std::vector<int> contr{Cl(2 * n1[0] - n1[2]), Cl(2 * n0[0] - n0[2]), Cl(2 * n1[1] - n1[2]),
                           Cl(2 * n0[1] - n0[2]), n1[2], n0[2]};
    if (alpha) {
        contr.push_back(a1);
        contr.push_back(a0);
    }
    const std::array<int, 3> avg{Cl((n0[0] + n1[0]) / 2.0), Cl((n0[1] + n1[1]) / 2.0), Cl((n0[2] + n1[2]) / 2.0)};
    return {plain(ints(n0), ints(n1)), contr, plain(avg, avg), plain(scaled(n0, n1[0] + n1[1] + n1[2]), ints(n1)),
            plain(ints(n0), scaled(n1, n0[0] + n0[1] + n0[2]))};
}

bool AnyPainted(const Painter& p, const std::vector<int>& cems, const std::vector<int>& vals) {
    std::size_t k = 0;
    for (const int c : cems) {
        const std::size_t n = static_cast<std::size_t>(2 * ((c >> 2) + 1));
        if (c == 8 || c == 9 || c == 12 || c == 13) {
            const auto e = Endpoints(c, std::vector<int>(vals.begin() + k, vals.begin() + k + n));
            if (e && (p.Paints(Rgb3(e->first)) >= 0 || p.Paints(Rgb3(e->second)) >= 0)) {
                return true;
            }
        }
        k += n;
    }
    return false;
}

// The block with its painted end colours, or nothing (unchanged).
std::optional<V128> AstcBlock(V128 v, const Painter& p) {
    if (VoidExtent(v)) {
        const Rgb c{static_cast<std::uint8_t>(Bits(v, 64, 16) >> 8), static_cast<std::uint8_t>(Bits(v, 80, 16) >> 8),
                    static_cast<std::uint8_t>(Bits(v, 96, 16) >> 8)};
        const int rule = p.Paints(c);
        if (rule < 0) {
            return std::nullopt;
        }
        const Rgb n = p.Paint(c, rule);
        for (int k = 0; k < 3; ++k) {
            v = WithBits(v, 64 + 16 * k, 16, static_cast<std::uint64_t>((n[static_cast<std::size_t>(k)] << 8) | n[static_cast<std::size_t>(k)]));
        }
        return v;
    }
    const auto lay = LayoutOf(v);
    if (!lay) {
        return std::nullopt;
    }
    const auto dm = IseRead(v, lay->cstart, lay->nvals, lay->range);
    const auto vals = DecodeVals(dm, lay->range);
    const auto& quant = Quant(lay->range);
    auto new_dm = dm;
    auto new_cems = lay->cems;
    bool changed = false;
    const bool same_mode = std::set<int>(lay->cems.begin(), lay->cems.end()).size() == 1;
    // one mode field for all partitions: if one base+offset partition becomes direct, all do
    const bool convert_all = lay->cem_at >= 0 && (lay->cems[0] == 9 || lay->cems[0] == 13) && same_mode &&
                             AnyPainted(p, lay->cems, vals);
    std::size_t k = 0;
    for (std::size_t i = 0; i < lay->cems.size(); ++i) {
        const int c = lay->cems[i];
        const std::size_t n = static_cast<std::size_t>(2 * ((c >> 2) + 1));
        const std::vector<int> part(vals.begin() + k, vals.begin() + k + n);
        if (c == 6 || c == 10) {
            const Rgb base{static_cast<std::uint8_t>(part[0]), static_cast<std::uint8_t>(part[1]),
                           static_cast<std::uint8_t>(part[2])};
            if (const int rule = p.Paints(base); rule >= 0) {
                const Rgb rgb = p.Paint(base, rule);
                for (std::size_t j = 0; j < 3; ++j) {
                    new_dm[k + j] = quant[rgb[j]];
                }
                changed = true;
            }
        } else if (c == 8 || c == 9 || c == 12 || c == 13) {
            const auto e = Endpoints(c, part);
            const Rgb e0 = Rgb3(e->first), e1 = Rgb3(e->second);
            const int r0 = p.Paints(e0), r1 = p.Paints(e1);
            if (r0 >= 0 || r1 >= 0 || convert_all) {
                const Rgb n0 = r0 >= 0 ? p.Paint(e0, r0) : e0;
                const Rgb n1 = r1 >= 0 ? p.Paint(e1, r1) : e1;
                const bool alpha = c == 12 || c == 13;
                std::optional<std::pair<int, std::vector<Digit>>> best;
                for (const auto& cand : Direct(n0, n1, e->first[3], e->second[3], alpha)) {
                    std::vector<Digit> qdm;
                    for (const int x : cand) {
                        qdm.push_back(quant[static_cast<std::size_t>(x)]);
                    }
                    const auto got = *Endpoints(alpha ? 12 : 8, DecodeVals(qdm, lay->range));
                    int err = 0;
                    for (std::size_t j = 0; j < 3; ++j) {
                        err += std::abs(got.first[j] - n0[j]) + std::abs(got.second[j] - n1[j]);
                    }
                    err += std::abs(got.first[3] - e->first[3]) + std::abs(got.second[3] - e->second[3]);
                    if (!best || err < best->first) {
                        best = std::pair{err, qdm};
                    }
                }
                std::copy(best->second.begin(), best->second.end(), new_dm.begin() + static_cast<std::ptrdiff_t>(k));
                new_cems[i] = alpha ? 12 : 8;
                changed = true;
            }
        }
        k += n;
    }
    if (!changed) {
        return std::nullopt;
    }
    if (new_cems != lay->cems) {
        if (lay->cem_at < 0 || std::set<int>(new_cems.begin(), new_cems.end()).size() != 1 || !same_mode) {
            return std::nullopt;   // mixed modes: the field cannot say it
        }
        v = WithBits(v, lay->cem_at, 4, static_cast<std::uint64_t>(new_cems[0]));
    }
    const auto enc = IseEncode(new_dm, lay->range);
    if (!enc) {
        return std::nullopt;
    }
    const auto [bits, length] = *enc;
    const V128 field = Shl(Mask(length), lay->cstart);
    v = Or(And(v, Not(field)), Shl(bits, lay->cstart));
    if (IseRead(v, lay->cstart, lay->nvals, lay->range) != new_dm) {
        return std::nullopt;
    }
    return v;
}

// BC1 picks 4-colour mode when c0 > c1: keep the block's mode after the recolour.
void Bc1Order(bool four, std::uint16_t& n0, std::uint16_t& n1, std::uint32_t& idx) {
    if (four == (n0 > n1)) {
        return;
    }
    if (n0 == n1) {
        if (four) {
            idx = 0;   // cannot be 4-colour any more: every texel the one colour
        }
        return;
    }
    std::uint32_t x = 0;
    for (int k = 0; k < 16; ++k) {
        std::uint32_t i = (idx >> (2 * k)) & 3;
        if (four || i < 2) {
            i ^= 1;
        }
        x |= i << (2 * k);
    }
    std::swap(n0, n1);
    idx = x;
}

std::size_t ColourAt(Format f) { return f == Format::bc3 ? 8 : 0; }   // the colour block in a BC3 block

}  // namespace

std::uint32_t ToneOf(const Rgb& c) {
    const int hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]});
    if (hi < 25) {
        return kBlack;
    }
    if (hi - lo < std::max(12.0, 0.15 * hi)) {
        return kGrey;
    }
    const double h = Hue(c);
    if (h < 15 || h >= 345) return kRed;
    if (h < 45) return hi >= 150 ? kGold : kBrown;
    if (h < 70) return hi >= 150 ? kGold : kYellow;
    if (h < 160) return kGreen;
    if (h < 200) return kCyan;
    if (h < 260) return kBlue;
    if (h < 300) return kPurple;
    return kPink;
}

double Luma(const Rgb& c) { return 0.2126 * Lin(c[0]) + 0.7152 * Lin(c[1]) + 0.0722 * Lin(c[2]); }

Rgb Shade(const Rgb& c, const Rgb& target, double base) {
    const double k = Luma(c) / base;
    return {Srgb(Lin(target[0]) * k), Srgb(Lin(target[1]) * k), Srgb(Lin(target[2]) * k)};
}

std::optional<Format> FormatOf(std::uint32_t bntx_format) {
    const std::uint32_t kind = bntx_format >> 8;
    if (kind == 0x1a) return Format::bc1;
    if (kind == 0x1c) return Format::bc3;
    if (kind >= 0x2d && kind <= 0x3a) return Format::astc;
    return std::nullopt;
}

std::size_t BlockBytes(Format f) { return f == Format::bc1 ? 8 : 16; }

std::vector<std::array<std::uint8_t, 4>> AstcEnds(const std::array<std::uint8_t, 16>& block) {
    std::vector<std::array<std::uint8_t, 4>> out;
    for (const Rgba& e : AstcEndsOf(Load(block.data()))) {
        out.push_back({static_cast<std::uint8_t>(e[0]), static_cast<std::uint8_t>(e[1]), static_cast<std::uint8_t>(e[2]),
                       static_cast<std::uint8_t>(e[3])});
    }
    return out;
}

std::vector<Rgb> Ends(Format f, std::span<const std::uint8_t> texels) {
    std::vector<Rgb> out;
    const std::size_t step = BlockBytes(f);
    for (std::size_t at = 0; at + step <= texels.size(); at += step) {
        const std::uint8_t* b = texels.data() + at;
        if (f == Format::astc) {
            for (const Rgba& e : AstcEndsOf(Load(b))) {
                out.push_back(Rgb3(e));
            }
            continue;
        }
        std::uint16_t c0, c1;
        std::uint32_t idx;
        std::memcpy(&c0, b + ColourAt(f), 2);
        std::memcpy(&c1, b + ColourAt(f) + 2, 2);
        std::memcpy(&idx, b + ColourAt(f) + 4, 4);
        if (c0 || c1 || idx) {
            out.push_back(Unpack565(c0));
            out.push_back(Unpack565(c1));
        }
    }
    return out;
}

std::size_t Recolour(Format f, std::span<const std::uint8_t> texels, std::span<std::uint8_t> out,
                     const std::vector<Rule>& rules) {
    Painter p{rules, std::vector<double>(rules.size(), 0.0), {}};
    for (const Rule& r : rules) {
        p.target.push_back(RgbOf(r.colour));
    }
    {
        std::vector<std::size_t> n(rules.size(), 0);
        for (const Rgb& e : Ends(f, texels)) {
            if (const int r = p.Match(e); r >= 0) {
                p.base[static_cast<std::size_t>(r)] += Luma(e);
                ++n[static_cast<std::size_t>(r)];
            }
        }
        for (std::size_t i = 0; i < rules.size(); ++i) {
            p.base[i] = n[i] != 0 ? p.base[i] / static_cast<double>(n[i]) : 0.0;
        }
    }
    const std::size_t size = std::min(texels.size(), out.size());
    if (out.data() != texels.data()) {
        std::memmove(out.data(), texels.data(), size);
    }
    const std::size_t step = BlockBytes(f);
    std::size_t changed = 0;
    for (std::size_t at = 0; at + step <= size; at += step) {
        std::uint8_t* o = out.data() + at;
        if (f == Format::astc) {
            if (const auto nv = AstcBlock(Load(texels.data() + at), p)) {
                Store(*nv, o);
                ++changed;
            }
            continue;
        }
        std::uint16_t c0, c1;
        std::uint32_t idx;
        std::memcpy(&c0, texels.data() + at + ColourAt(f), 2);
        std::memcpy(&c1, texels.data() + at + ColourAt(f) + 2, 2);
        std::memcpy(&idx, texels.data() + at + ColourAt(f) + 4, 4);
        if (!(c0 || c1 || idx)) {
            continue;
        }
        const Rgb e0 = Unpack565(c0), e1 = Unpack565(c1);
        const int r0 = p.Paints(e0), r1 = p.Paints(e1);
        if (r0 < 0 && r1 < 0) {
            continue;
        }
        std::uint16_t n0 = r0 >= 0 ? Pack565(p.Paint(e0, r0)) : c0;
        std::uint16_t n1 = r1 >= 0 ? Pack565(p.Paint(e1, r1)) : c1;
        if (f == Format::bc1) {
            Bc1Order(c0 > c1, n0, n1, idx);
        }
        std::memcpy(o + ColourAt(f), &n0, 2);
        std::memcpy(o + ColourAt(f) + 2, &n1, 2);
        std::memcpy(o + ColourAt(f) + 4, &idx, 4);
        ++changed;
    }
    return changed;
}

}  // namespace CoopLookPaint
