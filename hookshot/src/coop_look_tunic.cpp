// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_look_tunic.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

namespace CoopLookTunic {
namespace {

// tools/puppet_pack/look_live.py
constexpr char kMagic[8] = {'C', 'O', 'O', 'P', 'L', 'O', 'O', 'K'};
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kNameBytes = 64;
// The puppets' file holds every copy's gear (8 copies of 17 textures today).
constexpr std::size_t kMaxTextures = 4096;


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

bool Same(const Textures& memory, std::uint64_t address, const std::vector<std::uint8_t>& texels, std::size_t size) {
    std::vector<std::uint8_t> piece(std::min(kPiece, size));
    for (std::size_t at = 0; at < size; at += piece.size()) {
        const std::size_t n = std::min(piece.size(), size - at);
        if (!memory.Read(address + at, piece.data(), n) || std::memcmp(piece.data(), texels.data() + at, n) != 0) {
            return false;
        }
    }
    return true;
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
        return x.name == y.name;
    });
}

std::pair<std::uint64_t, std::uint64_t> Sizes(const std::vector<std::shared_ptr<const Texels>>& colours) {
    std::uint64_t smallest = ~std::uint64_t{0}, largest = 0;
    for (const auto& colour : colours) {
        for (std::size_t i = 0; colour && i < colour->size(); ++i) {
            smallest = std::min<std::uint64_t>(smallest, (*colour)[i].texels.size());
            largest = std::max<std::uint64_t>(largest, (*colour)[i].texels.size());
        }
    }
    if (largest == 0) {
        return {0, 0};
    }
    // A storage is the image's own size (the pack's texels), or its layout
    // rounded up a little.
    return {smallest, largest + kStorageSlack};
}

std::string PlacesLine(const std::vector<Place>& places, const Texels* names) {
    std::string line;
    char buf[64];
    for (const Place& p : places) {
        if (!line.empty()) line += ' ';
        if (names != nullptr && p.texture < names->size()) {
            line += (*names)[p.texture].name;
        } else {
            line += '#' + std::to_string(p.texture);
        }
        std::snprintf(buf, sizeof buf, "@0x%llx/%llu+%llu", static_cast<unsigned long long>(p.address),
                      static_cast<unsigned long long>(p.serial), static_cast<unsigned long long>(p.size));
        line += buf;
    }
    return line;
}

std::vector<std::pair<std::size_t, std::size_t>> Overlaps(const std::vector<Place>& a, const std::vector<Place>& b) {
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (std::size_t i = 0; i < a.size(); ++i) {
        for (std::size_t j = 0; j < b.size(); ++j) {
            if (a[i].address < b[j].address + b[j].size && b[j].address < a[i].address + a[i].size) {
                out.emplace_back(i, j);
            }
        }
    }
    return out;
}

std::vector<std::pair<std::size_t, std::size_t>> Repeated(const std::vector<Place>& places) {
    std::map<std::size_t, std::size_t> n;
    for (const Place& p : places) ++n[p.texture];
    std::vector<std::pair<std::size_t, std::size_t>> out;
    for (const auto& [texture, count] : n) {
        if (count > 1) out.emplace_back(texture, count);
    }
    return out;
}

Search Find(const Textures& textures, const std::vector<std::shared_ptr<const Texels>>& colours,
            const std::function<bool(const Live&)>& skip) {
    const auto [lo, hi] = Sizes(colours);
    if (hi == 0) {
        return {};
    }
    std::vector<Live> listed = textures.List(lo, hi, nullptr);
    const std::size_t count = listed.size();
    if (skip) {
        std::erase_if(listed, skip);
    }
    Search out = Find(textures, listed, colours);
    out.listed = count;
    return out;
}

Search Find(const Textures& textures, const std::vector<Live>& listed,
            const std::vector<std::shared_ptr<const Texels>>& colours) {
    Search out;
    out.listed = listed.size();
    for (const Live& live : listed) {
        std::uint8_t head[kHead];
        if (!textures.Read(live.address, head, kHead)) {
            ++out.unread;
            continue;
        }
        // The game copies each texture's texels into its texture memory (no
        // file, no name in front: lane1, 2026-11-14), so a texture is known by
        // its texels alone. The puppets' copies of the same texels are marked
        // (tools/puppet_pack/look_live.py mark): only your own hold a colour's
        // texels exactly. The first colour that holds them is the one held.
        bool taken = false, head_only = false;
        for (const auto& colour : colours) {
            for (std::size_t i = 0; !taken && colour && i < colour->size(); ++i) {
                const Texture& texture = (*colour)[i];
                const std::size_t size =
                    static_cast<std::size_t>(std::min<std::uint64_t>(texture.texels.size(), live.size));
                // (the whole image, its last byte too: a copy's mark is there)
                if (texture.texels.size() > live.size || std::memcmp(head, texture.texels.data(), kHead) != 0) {
                    continue;
                }
                if (!Same(textures, live.address, texture.texels, size)) {
                    head_only = true;
                    continue;
                }
                taken = true;
                out.places.push_back({i, live.address, live.serial, size, colour, colour, false});
            }
            if (taken) {
                break;
            }
        }
        if (head_only && !taken) {
            ++out.heads;   // (a miss too: its texels may still be coming in)
        }
        if (!taken) {
            out.misses.push_back(live);
        }
    }
    return out;
}

bool Draw(Textures& textures, Place& place, const std::shared_ptr<const Texels>& colour) {
    const Texture& held = (*place.held)[place.texture];
    const Texture& to = (*colour)[place.texture];
    const std::size_t n = static_cast<std::size_t>(place.size);
    if (to.texels.size() != held.texels.size() || n > to.texels.size()) {
        return false;
    }
    if (std::equal(to.texels.begin(), to.texels.begin() + static_cast<std::ptrdiff_t>(n), held.texels.begin())) {
        // the game's memory holds it: no override
        if (place.overridden && !textures.Override(place.address, place.serial, nullptr, n)) {
            return false;
        }
        place.overridden = false;
    } else {
        if (!textures.Override(place.address, place.serial, to.texels.data(), n)) {
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
            host_.textures->Override(p.address, p.serial, nullptr, static_cast<std::size_t>(p.size));
            p.overridden = false;
            p.drawn = p.held;
        }
    }
    places_.clear();
    misses_.clear();
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
    if (host_.textures == nullptr) return;   // (no game textures: nothing to look at)
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
            if (green_ && !host_.start.empty() && host_.start != host_.base) {
                start_ = Load(host_.start);
                if (start_ && !SameLayout(*green_, *start_)) {
                    start_ = nullptr;
                }
            }
        }
        auto target = !green_ ? nullptr : want == host_.base ? green_ : start_ && want == host_.start ? start_ : Load(want);
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
        // Only the textures made since the last search, unless asked to look
        // again or a full search is due (a texture whose texels came later).
        const bool full = look_now_ || !looked_ || now_ms - full_ms_ >= kFullSearchMs;
        if (full) {
            full_ms_ = now_ms;
        }
        // What the game's memory may hold: the start's colour first (the kit put
        // its files in), then the game's own, then the one asked for.
        std::vector<std::shared_ptr<const Texels>> colours;
        for (const auto& c : {start_, green_, target_}) {
            if (c && std::find(colours.begin(), colours.end(), c) == colours.end()) {
                colours.push_back(c);
            }
        }
        // The misses: those the game still has and this search skips (a quick
        // one skips every known miss, young ones too: LateLook below reads
        // those, kLateChecks a step), and those read now (each keeps when it
        // was first read). A texture finalized is not listed: its miss goes.
        std::map<std::pair<std::uint64_t, std::uint64_t>, Miss> misses;
        Search found = Find(*host_.textures, colours, [&](const Live& l) {
            const auto m = full ? misses_.end() : misses_.find({l.address, l.serial});
            if (m == misses_.end()) {
                return false;
            }
            misses.insert(*m);
            return true;
        });
        for (const Live& l : found.misses) {
            const auto old = misses_.find({l.address, l.serial});
            misses[{l.address, l.serial}] = {l, old != misses_.end() ? old->second.since : now_ms};
        }
        misses_ = std::move(misses);
        std::vector<bool> carried(places_.size(), false);
        for (Place& p : found.places) {   // a texture kept since: drawn as it was
            const auto old = std::find_if(places_.begin(), places_.end(), [&](const Place& o) {
                return o.address == p.address && o.serial == p.serial && o.texture == p.texture;
            });
            if (old != places_.end() && old->held == p.held) {
                p.drawn = old->drawn, p.overridden = old->overridden;
                carried[static_cast<std::size_t>(old - places_.begin())] = true;
            }
        }
        // An override no place carries on: the game's memory there no longer
        // holds your Link's texels, while the game has not finalized that
        // texture (it gave the memory to something else, made another texture
        // over it). Longshot keeps an override until the finalize, drawn over
        // whatever the game keeps there next (players, 0.11.0: effects'
        // textures, stick flames and nut flashes gone): taken away here. (A
        // texture finalized took its override with it: then this is a no-op.)
        std::size_t taken = 0;
        for (std::size_t i = 0; i < places_.size(); ++i) {
            const Place& o = places_[i];
            if (o.overridden && !carried[i]) {
                host_.textures->Override(o.address, o.serial, nullptr, static_cast<std::size_t>(o.size));
                ++taken;
            }
        }
        if (taken != 0 && said_taken_ < 20) {
            ++said_taken_;
            host_.log("coop: look: " + std::to_string(taken) + " override(s) of your Link's " + host_.what +
                      " taken away: the game's memory there holds something else now");
        }
        places_ = std::move(found.places);
        if (const auto twice = Repeated(places_); !twice.empty()) {
            std::string line = "coop: look: row 198 guard: your Link's " + host_.what + " has textures found more than once "
                               "among the game's (a copy with the game's own texels: your colour would show on it too):";
            for (const auto& [texture, n] : twice) line += " #" + std::to_string(texture) + " x" + std::to_string(n);
            if (line != said_repeated_) {
                said_repeated_ = line;
                host_.log(line);
            }
        }
        look_now_ = false, looked_ = true;
        generation_ = generation;
        if (!full) {
            LateLook(now_ms, colours);   // (a full search read them all just now)
        }
        if (places_.empty() && full) {
            std::string line = "coop: look: no " + host_.what + " texture of your Link among the game's yet (" +
                               std::to_string(found.listed) + " of the game's of those sizes, " +
                               std::to_string(found.unread) + " not readable, " + std::to_string(found.heads) +
                               " with only the first bytes the same)";
            // (said again when it changes, at most 20 times: the game makes its textures over a while)
            if ((!said_nothing_found_ || line != said_) && said_count_ < 20) {
                ++said_count_;
                host_.log(line);
            }
            said_nothing_found_ = true;
            said_ = std::move(line);
        }
    } else {
        // Nothing made or finalized: the young misses only (texels that came in late).
        std::vector<std::shared_ptr<const Texels>> colours;
        for (const auto& c : {start_, green_, target_}) {
            if (c && std::find(colours.begin(), colours.end(), c) == colours.end()) {
                colours.push_back(c);
            }
        }
        LateLook(now_ms, colours);
    }
    std::size_t drawn = 0;
    for (Place& p : places_) {
        if (p.drawn != target_ && Draw(*host_.textures, p, target_)) {
            ++drawn;
        }
    }
    if (drawn != 0 && said_places_count_ < kPlacesSaid) {
        // (row 198: where your colours are drawn, so a log shows whose texture each override lies on)
        std::string line = PlacesLine(places_, target_.get());
        if (line != said_places_) {
            ++said_places_count_;
            host_.log("coop: look: " + host_.what + " places: " + line);
            said_places_ = std::move(line);
        }
    }
    if (drawn != 0) {
        said_nothing_found_ = false;
        host_.log("coop: look: your Link's " + host_.what + " is " + target_name_ + " now (" + std::to_string(drawn) +
                  " texture(s) drawn so by Longshot's texture overrides; the game's memory is left as it is)");
    }
    if (want == (start_ ? host_.start : host_.base) && target_ == (start_ ? start_ : green_) &&
        std::none_of(places_.begin(), places_.end(), [](const Place& p) { return p.overridden; })) {
        // Everything is the game's own again: nothing to keep.
        std::lock_guard lock(mutex_);
        if (wanted_ == (start_ ? host_.start : host_.base)) {
            busy_ = false;
            places_.clear();
            looked_ = false;
        }
    }
}

void Wardrobe::LateLook(std::uint64_t now_ms, const std::vector<std::shared_ptr<const Texels>>& colours) {
    // The young misses, from late_next_ on (in turn: at most kLateChecks a Step).
    std::vector<Live> look_at;
    // (one first read in this very Step is not read again in it)
    const auto young = [&](const auto& e) { return e.second.since != now_ms && now_ms - e.second.since < kLateTexelsMs; };
    auto it = misses_.lower_bound(late_next_);
    for (std::size_t n = 0; n < misses_.size() && look_at.size() < kLateChecks; ++n, ++it) {
        if (it == misses_.end()) {
            it = misses_.begin();
        }
        if (young(*it)) {
            look_at.push_back(it->second.live);
        }
    }
    if (look_at.empty()) {
        return;
    }
    late_next_ = it == misses_.end() ? std::pair<std::uint64_t, std::uint64_t>{} : it->first;
    Search found = Find(*host_.textures, look_at, colours);
    for (Place& p : found.places) {
        misses_.erase({p.address, p.serial});
        places_.push_back(std::move(p));
    }
}

}  // namespace CoopLookTunic
