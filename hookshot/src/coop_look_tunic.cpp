// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look_tunic.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <string_view>
#include <utility>

namespace CoopLookTunic {
namespace {

// tools/puppet_pack/look_live.py
constexpr char kMagic[8] = {'C', 'O', 'O', 'P', 'L', 'O', 'O', 'K'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kNameBytes = 64;
constexpr std::size_t kMaxTextures = 64;


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

// Memory is compared in pieces of this size.
constexpr std::size_t kPiece = std::size_t{4} << 20;

bool Same(const Textures& memory, std::uint64_t address, const std::vector<std::uint8_t>& texels) {
    std::vector<std::uint8_t> piece(std::min(kPiece, texels.size()));
    for (std::size_t at = 0; at < texels.size(); at += piece.size()) {
        const std::size_t n = std::min(piece.size(), texels.size() - at);
        if (!memory.Read(address + at, piece.data(), n) || std::memcmp(piece.data(), texels.data() + at, n) != 0) {
            return false;
        }
    }
    return true;
}

bool InOwnFile(const Textures& memory, std::uint64_t address, const Texture& texture) {
    if (address < texture.offset) {
        return false;
    }
    std::vector<std::uint8_t> head(static_cast<std::size_t>(texture.offset));
    if (!memory.Read(address - texture.offset, head.data(), head.size()) ||
        std::memcmp(head.data(), "BNTX", 4) != 0 || std::memcmp(head.data() + head.size() - 16, "BRTD", 4) != 0) {
        return false;
    }
    const char* name = texture.name.c_str();
    return std::search(head.begin(), head.end(), name, name + texture.name.size() + 1) != head.end();
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

std::vector<std::uint8_t> Pack(const Texels& texels) {
    std::vector<std::uint8_t> out(kMagic, kMagic + sizeof kMagic);
    auto put = [&](std::uint64_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    };
    put(kVersion, 4);
    put(texels.size(), 4);
    for (const Texture& t : texels) {
        std::string name = t.name.substr(0, kNameBytes - 1);
        name.resize(kNameBytes, '\0');
        out.insert(out.end(), name.begin(), name.end());
        put(t.offset, 8);
        put(t.texels.size(), 8);
        out.insert(out.end(), t.texels.begin(), t.texels.end());
    }
    return out;
}

bool SameLayout(const Texels& a, const Texels& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const Texture& x, const Texture& y) {
        return x.name == y.name && x.offset == y.offset && x.texels.size() == y.texels.size();
    });
}

Search Find(const Textures& textures, const std::vector<std::shared_ptr<const Texels>>& colours) {
    Search out;
    std::uint64_t smallest = ~std::uint64_t{0}, largest = 0;
    for (const auto& colour : colours) {
        for (std::size_t i = 0; colour && i < colour->size(); ++i) {
            smallest = std::min<std::uint64_t>(smallest, (*colour)[i].texels.size());
            largest = std::max<std::uint64_t>(largest, (*colour)[i].texels.size());
        }
    }
    if (largest == 0) {
        return out;
    }
    // A storage holds at least its texels, and is rounded up a little at most.
    for (const Live& live : textures.List(smallest, largest + kStorageSlack, nullptr)) {
        std::uint8_t head[kHead];
        if (!textures.Read(live.address, head, kHead)) {
            continue;
        }
        bool taken = false;
        for (const auto& colour : colours) {
            for (std::size_t i = 0; !taken && colour && i < colour->size(); ++i) {
                const Texture& texture = (*colour)[i];
                if (texture.texels.size() > live.size || std::memcmp(head, texture.texels.data(), kHead) != 0 ||
                    !Same(textures, live.address, texture.texels)) {
                    continue;
                }
                taken = true;
                if (InOwnFile(textures, live.address, texture)) {
                    out.places.push_back({i, live.address, live.serial, colour, colour, false});
                } else {
                    ++out.copies;
                }
            }
            if (taken) {
                break;
            }
        }
    }
    return out;
}

bool Draw(Textures& textures, Place& place, const std::shared_ptr<const Texels>& colour) {
    const Texture& held = (*place.held)[place.texture];
    const Texture& to = (*colour)[place.texture];
    if (to.texels.size() != held.texels.size()) {
        return false;
    }
    if (to.texels == held.texels) {   // the game's memory holds it: no override
        if (place.overridden && !textures.Override(place.address, place.serial, nullptr, held.texels.size())) {
            return false;
        }
        place.overridden = false;
    } else {
        if (!textures.Override(place.address, place.serial, to.texels.data(), to.texels.size())) {
            return false;
        }
        place.overridden = true;
    }
    place.drawn = colour;
    return true;
}

void Wardrobe::Wear(std::string colour) {
    std::lock_guard lock(mutex_);
    if (colour != host_.base) {
        busy_ = true;
    }
    wanted_ = std::move(colour);
}

void Wardrobe::Kick() {
    std::lock_guard lock(mutex_);
    kicked_ = true;
}

bool Wardrobe::Busy() const {
    std::lock_guard lock(mutex_);
    return busy_;
}

void Wardrobe::Clear() {
    for (Place& p : places_) {
        if (p.overridden) {
            host_.textures->Override(p.address, p.serial, nullptr, (*p.held)[p.texture].texels.size());
            p.overridden = false;
            p.drawn = p.held;
        }
    }
    places_.clear();
    looked_ = false;
    target_name_.clear();
    asked_.clear();
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
    (void)now_ms;
    std::string want;
    {
        std::lock_guard lock(mutex_);
        if (!busy_) {
            return;
        }
        want = wanted_;
        if (std::exchange(kicked_, false)) {
            look_now_ = true;   // the equipment may have changed
        }
    }
    if (want != asked_) {
        asked_ = want;
        look_now_ = true;
        said_nothing_found_ = false;
    }
    if (target_name_ != want && refused_ != want) {
        if (!green_) {
            green_ = Load(host_.base);
        }
        auto target = !green_ ? nullptr : want == host_.base ? green_ : Load(want);
        if (target && !SameLayout(*green_, *target)) {
            host_.log("coop: look: coop_look/" + want + ".bin does not fit the pack's " + host_.base + ".bin: your own Link keeps "
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

    // The game's textures, again when it made or finalized one (a scene
    // change, an age change, a new shield) or the pause screen closed.
    std::uint64_t generation = 0;
    host_.textures->List(1, 0, &generation);   // (no storage is that size: the generation only)
    if (look_now_ || !looked_ || generation != generation_) {
        std::vector<std::shared_ptr<const Texels>> colours{green_};
        if (target_ != green_) {
            colours.push_back(target_);   // (a pack that put this colour into the game's own files)
        }
        Search found = Find(*host_.textures, colours);
        for (Place& p : found.places) {   // a texture kept since: drawn as it was
            const auto old = std::find_if(places_.begin(), places_.end(), [&](const Place& o) {
                return o.address == p.address && o.serial == p.serial && o.texture == p.texture;
            });
            if (old != places_.end() && old->held == p.held) {
                p.drawn = old->drawn, p.overridden = old->overridden;
            }
        }
        places_ = std::move(found.places);   // (a texture finalized took its override with it)
        look_now_ = false, looked_ = true;
        generation_ = generation;
        if (places_.empty() && !said_nothing_found_) {
            said_nothing_found_ = true;
            host_.log("coop: look: no " + host_.what + " texture of your Link among the game's yet (" +
                      std::to_string(found.copies) + " of puppets, left alone)");
        }
    }
    std::size_t drawn = 0;
    for (Place& p : places_) {
        if (p.drawn != target_ && Draw(*host_.textures, p, target_)) {
            ++drawn;
        }
    }
    if (drawn != 0) {
        said_nothing_found_ = false;
        host_.log("coop: look: your Link's " + host_.what + " is " + target_name_ + " now (" + std::to_string(drawn) +
                  " texture(s) drawn so by Longshot's texture overrides; the game's memory is left as it is)");
    }
    if (want == host_.base && target_ == green_ &&
        std::none_of(places_.begin(), places_.end(), [](const Place& p) { return p.overridden; })) {
        // Everything is the game's own again: nothing to keep.
        std::lock_guard lock(mutex_);
        if (wanted_ == host_.base) {
            busy_ = false;
            places_.clear();
            looked_ = false;
        }
    }
}

}  // namespace CoopLookTunic
