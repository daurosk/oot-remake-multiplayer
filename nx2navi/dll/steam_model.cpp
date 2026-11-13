// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam_model.hpp"

#include <algorithm>
#include <cstring>

#include "overlay_model.hpp"

namespace CoopNx2navi::SteamModel {
namespace {

constexpr std::size_t kPartHead = 5;  // kind, u16 number, index, count
constexpr std::size_t kPartBytes = kSteamDatagram - kPartHead;
constexpr std::string_view kRoomSymbols = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";

}  // namespace

std::vector<std::uint8_t> Hello() {
    return {static_cast<std::uint8_t>(Kind::hello), kVersion};
}

std::vector<std::uint8_t> Secret(std::string_view secret) {
    std::vector<std::uint8_t> out{static_cast<std::uint8_t>(Kind::secret), kVersion};
    out.insert(out.end(), secret.begin(), secret.end());
    return out;
}

std::optional<Parsed> Parse(std::span<const std::uint8_t> d) {
    if (d.size() < 2 || d.size() > kSteamDatagram) {
        return std::nullopt;
    }
    Parsed p;
    switch (d[0]) {
    case static_cast<std::uint8_t>(Kind::hello):
        if (d.size() != 2) {
            return std::nullopt;
        }
        p.kind = Kind::hello;
        p.version = d[1];
        return p;
    case static_cast<std::uint8_t>(Kind::secret): {
        p.kind = Kind::secret;
        p.version = d[1];
        p.secret.assign(reinterpret_cast<const char*>(d.data()) + 2, d.size() - 2);
        if (!ValidSecret(p.secret)) {
            return std::nullopt;
        }
        return p;
    }
    case static_cast<std::uint8_t>(Kind::data):
        p.kind = Kind::data;
        p.bytes = d.subspan(1);
        return p;
    case static_cast<std::uint8_t>(Kind::part):
        if (d.size() <= kPartHead) {
            return std::nullopt;
        }
        p.kind = Kind::part;
        p.number = static_cast<std::uint16_t>(d[1] | d[2] << 8);
        p.index = d[3];
        p.count = d[4];
        if (p.count < 2 || p.count > kMaxParts || p.index >= p.count) {
            return std::nullopt;
        }
        p.bytes = d.subspan(kPartHead);
        return p;
    default: return std::nullopt;
    }
}

std::vector<std::vector<std::uint8_t>> Wrap(std::span<const std::uint8_t> enet, std::uint16_t number) {
    std::vector<std::vector<std::uint8_t>> out;
    if (enet.empty() || enet.size() > kPartBytes * kMaxParts) {
        return out;
    }
    if (enet.size() + 1 <= kSteamDatagram) {
        std::vector<std::uint8_t> one;
        one.reserve(enet.size() + 1);
        one.push_back(static_cast<std::uint8_t>(Kind::data));
        one.insert(one.end(), enet.begin(), enet.end());
        out.push_back(std::move(one));
        return out;
    }
    const std::size_t count = (enet.size() + kPartBytes - 1) / kPartBytes;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t from = i * kPartBytes;
        const std::size_t size = std::min(kPartBytes, enet.size() - from);
        std::vector<std::uint8_t> part{static_cast<std::uint8_t>(Kind::part),
                                       static_cast<std::uint8_t>(number & 0xff),
                                       static_cast<std::uint8_t>(number >> 8),
                                       static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(count)};
        part.insert(part.end(), enet.begin() + static_cast<std::ptrdiff_t>(from),
                    enet.begin() + static_cast<std::ptrdiff_t>(from + size));
        out.push_back(std::move(part));
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> Joiner::Add(const Parsed& part) {
    if (part.kind != Kind::part || part.count < 2 || part.count > kMaxParts || part.index >= part.count ||
        part.bytes.empty() || part.bytes.size() > kPartBytes) {
        return std::nullopt;
    }
    if (have_ == 0 || part.number != number_ || part.count != count_) {
        number_ = part.number;
        count_ = part.count;
        have_ = 0;
    }
    parts_[part.index].assign(part.bytes.begin(), part.bytes.end());
    have_ = static_cast<std::uint8_t>(have_ | 1u << part.index);
    if (have_ != (1u << count_) - 1) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> whole;
    for (std::size_t i = 0; i < count_; ++i) {
        whole.insert(whole.end(), parts_[i].begin(), parts_[i].end());
    }
    have_ = 0;
    return whole;
}

std::string SecretFrom(std::span<const std::uint8_t, 16> random) {
    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(kSecretChars);
    for (const std::uint8_t byte : random) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 15]);
    }
    return out;
}

bool ValidSecret(std::string_view secret) {
    return secret.size() == kSecretChars &&
           std::all_of(secret.begin(), secret.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string PlayerName(std::string_view steam_name) {
    std::string out;
    for (std::size_t i = 0; i < steam_name.size();) {
        const auto c = static_cast<unsigned char>(steam_name[i]);
        const std::size_t size = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (size == 0 || i + size > steam_name.size()) {
            ++i;  // not UTF-8: left out
            continue;
        }
        bool whole = true;
        for (std::size_t k = 1; k < size; ++k) {
            whole = whole && (static_cast<unsigned char>(steam_name[i + k]) & 0xc0) == 0x80;
        }
        // Control characters, and the C1 ones (U+0080 to U+009F: C2 80 to C2 9F).
        const bool control = c < 0x20 || c == 0x7f ||
                             (c == 0xc2 && size == 2 && static_cast<unsigned char>(steam_name[i + 1]) < 0xa0);
        if (whole && !control && !(out.empty() && c == ' ')) {
            if (Ui::Utf8Count(out) >= 16) {
                break;
            }
            out.append(steam_name.substr(i, size));
        }
        i += whole ? size : 1;
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string RoomId(std::string_view typed) {
    std::string out;
    for (char c : typed) {
        if (c == ' ' || c == '-' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        if (kRoomSymbols.find(c) == std::string_view::npos || out.size() >= 8) {
            return {};
        }
        out.push_back(c);
    }
    if (out.size() != 8) {
        return {};
    }
    out.insert(4, "-");
    return out;
}

}  // namespace CoopNx2navi::SteamModel
