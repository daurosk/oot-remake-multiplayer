// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look_tunic.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string_view>

namespace CoopLookTunic {
namespace {

// tools/puppet_pack/look_live.py
constexpr char kMagic[8] = {'C', 'O', 'O', 'P', 'L', 'O', 'O', 'K'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kNameBytes = 64;
constexpr std::size_t kMaxTextures = 64;

// Memory is searched in pieces of this size (and compared in them).
constexpr std::size_t kPiece = std::size_t{4} << 20;
// A piece that cannot be read whole is read in these.
constexpr std::size_t kSmallPiece = std::size_t{64} << 10;

template <typename T>
T Little(const std::uint8_t* p) {
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        v |= static_cast<T>(p[i]) << (8 * i);
    }
    return v;
}

void Fail(std::string* why, std::string text) {
    if (why != nullptr) {
        *why = std::move(text);
    }
}

// The texels at `address` are these, whole.
bool Same(const Memory& memory, std::uint64_t address, const std::vector<std::uint8_t>& texels) {
    std::vector<std::uint8_t> piece(std::min(kPiece, texels.size()));
    for (std::size_t at = 0; at < texels.size(); at += piece.size()) {
        const std::size_t n = std::min(piece.size(), texels.size() - at);
        if (!memory.Read(address + at, piece.data(), n) || std::memcmp(piece.data(), texels.data() + at, n) != 0) {
            return false;
        }
    }
    return true;
}

// The texels at `address` are in the texture's own file: the file's header
// `offset` bytes before them, naming the texture, and the data block's own
// header right in front.
bool InOwnFile(const Memory& memory, std::uint64_t address, const Texture& texture) {
    if (address < texture.offset) {
        return false;
    }
    std::vector<std::uint8_t> head(static_cast<std::size_t>(texture.offset));
    if (!memory.Read(address - texture.offset, head.data(), head.size()) ||
        std::memcmp(head.data(), "BNTX", 4) != 0 || std::memcmp(head.data() + head.size() - 16, "BRTD", 4) != 0) {
        return false;
    }
    // The name as the file's string table has it: NUL-ended.
    const char* name = texture.name.c_str();
    return std::search(head.begin(), head.end(), name, name + texture.name.size() + 1) != head.end();
}

struct Key {
    std::uint64_t first = 0;   // the texels' first eight bytes
    std::size_t texture = 0;
    std::shared_ptr<const Texels> colour;
};

std::string Megabytes(std::uint64_t bytes) {
    return std::to_string(bytes >> 20) + " MB";
}

}  // namespace

std::optional<Texels> Parse(const std::vector<std::uint8_t>& file, std::string* why) {
    if (file.size() < 16 || std::memcmp(file.data(), kMagic, sizeof kMagic) != 0) {
        Fail(why, "not a look file");
        return std::nullopt;
    }
    if (const auto version = Little<std::uint32_t>(file.data() + 8); version != kVersion) {
        Fail(why, "look file version " + std::to_string(version) + ", this build reads " + std::to_string(kVersion));
        return std::nullopt;
    }
    const auto count = Little<std::uint32_t>(file.data() + 12);
    if (count == 0 || count > kMaxTextures) {
        Fail(why, "a look file of " + std::to_string(count) + " textures");
        return std::nullopt;
    }
    Texels out;
    std::size_t at = 16;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (file.size() - at < kNameBytes + 16) {
            Fail(why, "the look file is cut short");
            return std::nullopt;
        }
        Texture t;
        const char* name = reinterpret_cast<const char*>(file.data() + at);
        t.name.assign(name, std::find(name, name + kNameBytes, '\0'));
        t.offset = Little<std::uint64_t>(file.data() + at + kNameBytes);
        const auto size = Little<std::uint64_t>(file.data() + at + kNameBytes + 8);
        at += kNameBytes + 16;
        if (size > file.size() - at) {
            Fail(why, "the look file is cut short");
            return std::nullopt;
        }
        if (t.name.empty() || t.offset < 16 || t.offset > kMaxOffset || size < kHead) {
            Fail(why, "texture " + std::to_string(i) + " of the look file cannot be placed");
            return std::nullopt;
        }
        t.texels.assign(file.begin() + static_cast<std::ptrdiff_t>(at),
                        file.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += static_cast<std::size_t>(size);
        out.push_back(std::move(t));
    }
    return out;
}

bool SameLayout(const Texels& a, const Texels& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const Texture& x, const Texture& y) {
        return x.name == y.name && x.offset == y.offset && x.texels.size() == y.texels.size();
    });
}

Search Find(const Memory& memory, std::uint64_t begin, std::uint64_t end,
            const std::vector<std::shared_ptr<const Texels>>& colours) {
    Search out;
    std::vector<Key> keys;
    std::vector<bool> starts(std::size_t{1} << 16);   // the first two bytes of any key
    for (const auto& colour : colours) {
        for (std::size_t i = 0; colour && i < colour->size(); ++i) {
            const auto& texels = (*colour)[i].texels;
            keys.push_back({Little<std::uint64_t>(texels.data()), i, colour});
            starts[Little<std::uint16_t>(texels.data())] = true;
        }
    }
    begin = (begin + kAlign - 1) / kAlign * kAlign;
    if (keys.empty() || begin >= end) {
        return out;
    }
    const auto look_at = [&](const std::uint8_t* bytes, std::uint64_t address) {
        if (!starts[Little<std::uint16_t>(bytes)]) {
            return;
        }
        const auto first = Little<std::uint64_t>(bytes);
        for (const Key& key : keys) {
            const Texture& texture = (*key.colour)[key.texture];
            if (key.first != first || std::memcmp(bytes, texture.texels.data(), kHead) != 0 ||
                !Same(memory, address, texture.texels)) {
                continue;
            }
            if (InOwnFile(memory, address, texture)) {
                out.places.push_back({key.texture, address, key.colour});
            } else {
                ++out.copies;
            }
            return;
        }
    };
    // Each piece is read with the next one's first bytes, so a head across
    // two pieces is seen once, by the piece it starts in.
    std::vector<std::uint8_t> piece(kPiece + kHead);
    const auto search = [&](std::uint64_t at, std::size_t starts_in, std::size_t step) {
        for (std::uint64_t p = at; p < at + starts_in; p += step) {
            const std::size_t own = static_cast<std::size_t>(std::min<std::uint64_t>(step, at + starts_in - p));
            const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(own + kHead, end - p));
            if (!memory.Read(p, piece.data(), n)) {
                if (step > kSmallPiece) {
                    return false;
                }
                continue;   // not there: passed over
            }
            for (std::size_t o = 0; o < own && o + kHead <= n; o += kAlign) {
                look_at(piece.data() + o, p + o);
            }
        }
        return true;
    };
    for (std::uint64_t at = begin; at < end; at += kPiece) {
        const std::size_t own = static_cast<std::size_t>(std::min<std::uint64_t>(kPiece, end - at));
        if (!search(at, own, kPiece)) {
            search(at, own, kSmallPiece);
        }
    }
    return out;
}

bool Holds(const Memory& memory, const Place& place) {
    const Texture& texture = (*place.colour)[place.texture];
    return Same(memory, place.address, texture.texels) && InOwnFile(memory, place.address, texture);
}

bool Put(Memory& memory, Place& place, const std::shared_ptr<const Texels>& colour) {
    const Texture& from = (*place.colour)[place.texture];
    const Texture& to = (*colour)[place.texture];
    if (from.texels.size() != to.texels.size() || !InOwnFile(memory, place.address, from) ||
        !Same(memory, place.address, from.texels) ||
        !memory.Write(place.address, to.texels.data(), to.texels.size())) {
        return false;
    }
    place.colour = colour;
    return true;
}

void Wardrobe::Wear(std::string colour) {
    std::lock_guard lock(mutex_);
    if (colour != "green") {
        busy_ = true;
    }
    wanted_ = std::move(colour);
}

bool Wardrobe::Busy() const {
    std::lock_guard lock(mutex_);
    return busy_;
}

std::shared_ptr<const Texels> Wardrobe::Load(const std::string& colour) {
    const auto file = host_.file(colour);
    if (!file) {
        host_.log("coop: look: the puppet pack has no coop_look/" + colour +
                  ".bin: your own Link keeps the colour it has (make the puppet pack again; the others see your "
                  "new colour all the same)");
        return nullptr;
    }
    std::string why;
    auto texels = Parse(*file, &why);
    if (!texels) {
        host_.log("coop: look: coop_look/" + colour + ".bin of the puppet pack is not read (" + why +
                  "): your own Link keeps the colour it has");
        return nullptr;
    }
    return std::make_shared<const Texels>(std::move(*texels));
}

void Wardrobe::Step(std::uint64_t now_ms) {
    std::string want;
    {
        std::lock_guard lock(mutex_);
        if (!busy_) {
            return;
        }
        want = wanted_;
    }
    if (want != asked_) {
        asked_ = want;
        search_now_ = true;
        said_nothing_found_ = false;
    }
    if (target_name_ != want && refused_ != want) {
        if (!green_) {
            green_ = Load("green");
        }
        auto target = !green_ ? nullptr : want == "green" ? green_ : Load(want);
        if (target && !SameLayout(*green_, *target)) {
            host_.log("coop: look: coop_look/" + want + ".bin does not fit the pack's green.bin: your own Link keeps "
                      "the colour it has (make the puppet pack again)");
            target = nullptr;
        }
        if (target) {
            target_ = std::move(target);
            target_name_ = want;
        } else {
            refused_ = want;   // said once; the colour worn before stays
        }
    }
    if (!target_) {
        return;
    }

    // Places the game took back: the texture unloaded, or loaded again (green).
    const std::size_t before = places_.size();
    std::erase_if(places_, [&](const Place& p) { return !Holds(*host_.memory, p); });
    if (places_.size() != before) {
        search_now_ = true;
    }
    std::string searched;
    if (search_now_ || now_ms >= next_search_ms_) {
        // What a tunic texture can hold: the game's own colour (just loaded),
        // the colour wanted, and the colours written before.
        std::vector<std::shared_ptr<const Texels>> colours{green_};
        const auto add = [&](const std::shared_ptr<const Texels>& c) {
            if (std::find(colours.begin(), colours.end(), c) == colours.end()) {
                colours.push_back(c);
            }
        };
        add(target_);
        for (const Place& p : places_) {
            add(p.colour);
        }
        const auto [begin, end] = host_.range();
        const auto t0 = std::chrono::steady_clock::now();
        Search found = Find(*host_.memory, begin, end, colours);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
        places_ = std::move(found.places);
        search_now_ = false;
        next_search_ms_ = now_ms + (places_.empty() ? kSearchSoonMs : kSearchAgainMs);
        searched = "; searched " + Megabytes(end > begin ? end - begin : 0) + " in " + std::to_string(ms.count()) +
                   " ms: " + std::to_string(places_.size()) + " of your Link's, " + std::to_string(found.copies) +
                   " of puppets (left alone)";
        if (places_.empty() && !said_nothing_found_) {
            said_nothing_found_ = true;
            host_.log("coop: look: no tunic texture of your Link in the game's memory yet" + searched);
        }
    }
    std::size_t written = 0;
    for (Place& p : places_) {
        if (p.colour == target_) {
            continue;
        }
        if ((*p.colour)[p.texture].texels == (*target_)[p.texture].texels) {
            p.colour = target_;   // this texture has no green to change: the same bytes
        } else if (Put(*host_.memory, p, target_)) {
            ++written;
        }
    }
    if (written != 0) {
        said_nothing_found_ = false;
        host_.log("coop: look: your Link's tunic is " + target_name_ + " now (" + std::to_string(written) +
                  " texture(s) written)" + searched);
    }
    if (want == "green" && target_ == green_ &&
        std::all_of(places_.begin(), places_.end(), [&](const Place& p) { return p.colour == green_; })) {
        // Everything is the game's own again: nothing to keep.
        std::lock_guard lock(mutex_);
        if (wanted_ == "green") {
            busy_ = false;
            places_.clear();
        }
    }
}

}  // namespace CoopLookTunic
