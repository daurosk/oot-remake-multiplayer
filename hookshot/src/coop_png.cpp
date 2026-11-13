// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_png.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <utility>

namespace CoopPng {
namespace {

// ---- inflate (RFC 1951), canonical Huffman codes decoded a bit at a time ----

struct Bits {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;  // bytes
    unsigned buf = 0;
    int count = 0;
    bool bad = false;

    unsigned Get(int n) {
        unsigned v = buf;
        while (count < n) {
            if (pos >= size) {
                bad = true;
                return 0;
            }
            v |= static_cast<unsigned>(data[pos++]) << count;
            count += 8;
        }
        buf = n < 32 ? v >> n : 0;
        count -= n;
        return v & ((1u << n) - 1);
    }
};

constexpr int kMaxBits = 15;

struct Huffman {
    std::array<std::uint16_t, kMaxBits + 1> count{};
    std::array<std::uint16_t, 288> symbol{};
};

// Codes from the code lengths; false when they over-subscribe.
bool Build(Huffman& h, const std::uint8_t* lengths, int n) {
    h.count.fill(0);
    for (int s = 0; s < n; ++s) {
        h.count[lengths[s]]++;
    }
    if (h.count[0] == n) {
        return true;  // no codes: fine until one is read
    }
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) {
            return false;
        }
    }
    std::array<std::uint16_t, kMaxBits + 1> offs{};
    for (int len = 1; len < kMaxBits; ++len) {
        offs[len + 1] = static_cast<std::uint16_t>(offs[len] + h.count[len]);
    }
    for (int s = 0; s < n; ++s) {
        if (lengths[s] != 0) {
            h.symbol[offs[lengths[s]]++] = static_cast<std::uint16_t>(s);
        }
    }
    return true;
}

int Decode(Bits& b, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        code |= static_cast<int>(b.Get(1));
        if (b.bad) {
            return -1;
        }
        const int c = h.count[len];
        if (code - c < first) {
            return h.symbol[index + (code - first)];
        }
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

constexpr std::uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,    25,
                                         33,   49,   65,   97,   129,  193,   257,   385,   513,   769,
                                         1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                         6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool Codes(Bits& b, std::vector<std::uint8_t>& out, std::size_t limit, const Huffman& lit,
           const Huffman& dist) {
    for (;;) {
        int s = Decode(b, lit);
        if (s < 0) {
            return false;
        }
        if (s < 256) {
            if (out.size() >= limit) {
                return false;
            }
            out.push_back(static_cast<std::uint8_t>(s));
            continue;
        }
        if (s == 256) {
            return true;
        }
        s -= 257;
        if (s >= 29) {
            return false;
        }
        const std::size_t len = kLenBase[s] + b.Get(kLenExtra[s]);
        const int d = Decode(b, dist);
        if (d < 0 || d >= 30) {
            return false;
        }
        const std::size_t back = kDistBase[d] + b.Get(kDistExtra[d]);
        if (b.bad || back > out.size() || out.size() + len > limit) {
            return false;
        }
        const std::size_t from = out.size() - back;
        for (std::size_t i = 0; i < len; ++i) {
            out.push_back(out[from + i]);
        }
    }
}

bool Fixed(Bits& b, std::vector<std::uint8_t>& out, std::size_t limit) {
    static const auto tables = [] {
        std::pair<Huffman, Huffman> t;
        std::uint8_t l[288];
        int s = 0;
        for (; s < 144; ++s) l[s] = 8;
        for (; s < 256; ++s) l[s] = 9;
        for (; s < 280; ++s) l[s] = 7;
        for (; s < 288; ++s) l[s] = 8;
        Build(t.first, l, 288);
        for (s = 0; s < 30; ++s) l[s] = 5;
        Build(t.second, l, 30);
        return t;
    }();
    return Codes(b, out, limit, tables.first, tables.second);
}

bool Dynamic(Bits& b, std::vector<std::uint8_t>& out, std::size_t limit) {
    static constexpr std::uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int nlen = static_cast<int>(b.Get(5)) + 257;
    const int ndist = static_cast<int>(b.Get(5)) + 1;
    const int ncode = static_cast<int>(b.Get(4)) + 4;
    if (b.bad || nlen > 286 || ndist > 30) {
        return false;
    }
    std::uint8_t lengths[320] = {};
    for (int i = 0; i < ncode; ++i) {
        lengths[kOrder[i]] = static_cast<std::uint8_t>(b.Get(3));
    }
    Huffman lencode;
    if (b.bad || !Build(lencode, lengths, 19)) {
        return false;
    }
    std::uint8_t codes[320] = {};
    int at = 0;
    while (at < nlen + ndist) {
        const int s = Decode(b, lencode);
        if (s < 0) {
            return false;
        }
        if (s < 16) {
            codes[at++] = static_cast<std::uint8_t>(s);
            continue;
        }
        std::uint8_t len = 0;
        int repeat = 0;
        if (s == 16) {
            if (at == 0) {
                return false;
            }
            len = codes[at - 1];
            repeat = 3 + static_cast<int>(b.Get(2));
        } else if (s == 17) {
            repeat = 3 + static_cast<int>(b.Get(3));
        } else {
            repeat = 11 + static_cast<int>(b.Get(7));
        }
        if (b.bad || at + repeat > nlen + ndist) {
            return false;
        }
        while (repeat-- > 0) {
            codes[at++] = len;
        }
    }
    if (codes[256] == 0) {
        return false;  // no end-of-block code
    }
    Huffman lit, dist;
    if (!Build(lit, codes, nlen) || !Build(dist, codes + nlen, ndist)) {
        return false;
    }
    return Codes(b, out, limit, lit, dist);
}

// ---- PNG ----

std::uint32_t Be32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | p[3];
}

int Paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    return pb <= pc ? b : c;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> Inflate(const std::uint8_t* data, std::size_t size,
                                                 std::size_t limit) {
    if (size < 6 || (data[0] & 0x0F) != 8 || ((data[0] << 8) | data[1]) % 31 != 0 || (data[1] & 0x20) != 0) {
        return std::nullopt;  // not zlib's deflate, or a preset dictionary
    }
    Bits b{data + 2, size - 2};
    std::vector<std::uint8_t> out;
    for (bool last = false; !last;) {
        last = b.Get(1) != 0;
        const unsigned type = b.Get(2);
        if (b.bad) {
            return std::nullopt;
        }
        bool ok = false;
        if (type == 0) {
            b.buf = 0;
            b.count = 0;  // to the byte
            if (b.pos + 4 > b.size) {
                return std::nullopt;
            }
            const unsigned len = b.data[b.pos] | b.data[b.pos + 1] << 8;
            const unsigned nlen = b.data[b.pos + 2] | b.data[b.pos + 3] << 8;
            b.pos += 4;
            if ((len ^ 0xFFFFu) != nlen || b.pos + len > b.size || out.size() + len > limit) {
                return std::nullopt;
            }
            out.insert(out.end(), b.data + b.pos, b.data + b.pos + len);
            b.pos += len;
            ok = true;
        } else if (type == 1) {
            ok = Fixed(b, out, limit);
        } else if (type == 2) {
            ok = Dynamic(b, out, limit);
        }
        if (!ok) {
            return std::nullopt;
        }
    }
    // Adler-32 of what came out (the bits left in the last byte are padding).
    if (b.pos + 4 > b.size) {
        return std::nullopt;
    }
    std::uint32_t s1 = 1, s2 = 0;
    for (const std::uint8_t c : out) {
        s1 = (s1 + c) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    if (Be32(b.data + b.pos) != (s2 << 16 | s1)) {
        return std::nullopt;
    }
    return out;
}

std::optional<Image> Decode(const std::uint8_t* data, std::size_t size) {
    static constexpr std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (data == nullptr || size < 8 || std::memcmp(data, kSignature, 8) != 0) {
        return std::nullopt;
    }
    std::uint32_t width = 0, height = 0;
    int type = -1;
    std::vector<std::uint8_t> idat;
    std::vector<std::uint8_t> palette;  // RGBA
    for (std::size_t at = 8; at + 12 <= size;) {
        const std::uint32_t len = Be32(data + at);
        if (len > size - at - 12) {
            return std::nullopt;
        }
        const std::uint8_t* tag = data + at + 4;
        const std::uint8_t* body = data + at + 8;
        if (std::memcmp(tag, "IHDR", 4) == 0) {
            if (len != 13 || body[8] != 8 || body[10] != 0 || body[11] != 0 || body[12] != 0) {
                return std::nullopt;  // 8 bits a channel, deflate, filter set 0, not interlaced
            }
            width = Be32(body);
            height = Be32(body + 4);
            type = body[9];
        } else if (std::memcmp(tag, "PLTE", 4) == 0) {
            palette.clear();
            for (std::uint32_t i = 0; i + 3 <= len; i += 3) {
                palette.insert(palette.end(), {body[i], body[i + 1], body[i + 2], 255});
            }
        } else if (std::memcmp(tag, "tRNS", 4) == 0 && type == 3) {
            for (std::uint32_t i = 0; i < len && i * 4 + 3 < palette.size(); ++i) {
                palette[i * 4 + 3] = body[i];
            }
        } else if (std::memcmp(tag, "IDAT", 4) == 0) {
            idat.insert(idat.end(), body, body + len);
        } else if (std::memcmp(tag, "IEND", 4) == 0) {
            break;
        }
        at += 12 + static_cast<std::size_t>(len);
    }
    int channels = 0;
    switch (type) {
        case 0: channels = 1; break;
        case 2: channels = 3; break;
        case 3: channels = 1; break;
        case 4: channels = 2; break;
        case 6: channels = 4; break;
        default: return std::nullopt;
    }
    if (width == 0 || height == 0 || width > 4096 || height > 4096 || (type == 3 && palette.empty())) {
        return std::nullopt;
    }
    const std::size_t stride = static_cast<std::size_t>(width) * channels;
    const std::size_t want = (stride + 1) * height;
    const auto raw = Inflate(idat.data(), idat.size(), want);
    if (!raw || raw->size() != want) {
        return std::nullopt;
    }
    // Undo the filters, row by row.
    std::vector<std::uint8_t> px(stride * height);
    const std::size_t bpp = static_cast<std::size_t>(channels);
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::uint8_t filter = (*raw)[y * (stride + 1)];
        const std::uint8_t* in = raw->data() + y * (stride + 1) + 1;
        std::uint8_t* row = px.data() + y * stride;
        const std::uint8_t* up = y > 0 ? row - stride : nullptr;
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? row[i - bpp] : 0;
            const int b = up ? up[i] : 0;
            const int c = up && i >= bpp ? up[i - bpp] : 0;
            int v = in[i];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: v += Paeth(a, b, c); break;
                default: return std::nullopt;
            }
            row[i] = static_cast<std::uint8_t>(v);
        }
    }
    Image img;
    img.width = static_cast<int>(width);
    img.height = static_cast<int>(height);
    img.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t p = 0; p < static_cast<std::size_t>(width) * height; ++p) {
        const std::uint8_t* s = px.data() + p * bpp;
        std::uint8_t* d = img.rgba.data() + p * 4;
        switch (type) {
            case 0: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
            case 2: d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; break;
            case 3:
                if (static_cast<std::size_t>(s[0]) * 4 + 3 >= palette.size()) {
                    return std::nullopt;
                }
                std::memcpy(d, palette.data() + static_cast<std::size_t>(s[0]) * 4, 4);
                break;
            case 4: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
            default: std::memcpy(d, s, 4); break;
        }
    }
    return img;
}

std::optional<Image> Load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::nullopt;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return Decode(bytes.data(), bytes.size());
}

}  // namespace CoopPng
