// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_world.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>

#include "coop/protocol/codec.hpp"

namespace CoopWorld {

namespace {

// Every Obj_ pack of build e0f3ccfaf50065ae that has LifeRef and
// DestructionRef (read from the romfs, 2026-11-09), by name part. Left out:
// the chest Obj_TBox_A_04 (opening a chest is a flag, flags.md), shop
// targets and moving obstacles (minigames), icicles and hammer pillars
// (they fall or tilt rather than break), Obj_Well_CandleStick_A_03.
constexpr std::array<std::string_view, 6> kPrefixes{
    "Obj_Pot_",          // pots, the respawning and dream ones too
    "Obj_BrokenBox_",    // crates
    "Obj_LiftRock_",     // rocks Link lifts and throws
    "Obj_BrokenRock_",   // rocks a bomb or hammer breaks
    "Obj_Plant_Grass_",  // grass
    "Obj_GuidePost_",    // signposts
};
constexpr std::array<std::string_view, 10> kParts{
    "SignBoard_",  "Signboard_",      // cuttable signs, every region's
    "CrackWall_",  "CrackFloor_",     // bombable walls and floors
    "BrokenRock_",                    // Goron and Death Mountain rocks
    "BrokenIcePillar_", "SkullPot_", "ArcheryPot_", "Beehive_", "SpiderWeb_",
};

// The grass that is an actor without an Obj_Plant_Grass_ name: every pack
// with the life of Obj_Plant_DestructibleGrass_Base (a pot's: 1, then gone).
// It has no DestructionRef, so the list above missed it; the 13 tufts by
// the Kokiri side of Hyrule Field are the third. The rest of the field's
// grass is foliage, no actor at all, and is not shared (world.md).
constexpr std::array<std::string_view, 4> kGrass{
    "Obj_Plant_DestructibleGrass_Base",
    "Land_Kokiri_Plant_G_01",
    "Land_kokiri_Plant_A_02_DropFixed_03",
    "Land_Kokiri_Zenmai_A_02",
};

} // namespace

bool IsBreakable(std::string_view name) {
    for (const auto grass : kGrass) {
        if (name == grass) {
            return true;
        }
    }
    if (!name.starts_with("Obj_")) {
        return false;
    }
    for (const auto prefix : kPrefixes) {
        if (name.starts_with(prefix)) {
            return true;
        }
    }
    for (const auto part : kParts) {
        if (name.find(part) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

namespace {

constexpr int kBlockLines = 60;                      // per block and scene
constexpr int kNoisy = 6;  // a word that changed this often is a counter: no more lines for it
constexpr int kAllLines = 1500;                      // per session

bool LooksLikePointer(std::uint64_t v) {
    return (v >> 32) != 0 && v < (1ull << 40) && v % 4 == 0;
}

} // namespace

void FoliageProbe::Compare(const ReadWord& read, const std::string& key, std::uint64_t address,
                           std::size_t count, const std::string& where,
                           std::vector<std::string>& out) {
    std::vector<std::uint64_t> now(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto w = read(address + 8 * i);
        if (!w) {
            return;  // not readable: not a buffer
        }
        now[i] = *w;
    }
    Block& b = blocks_[key];
    if (b.lines >= kBlockLines || lines_ >= kAllLines) {
        b.address = address;
        b.words = std::move(now);
        b.changes.assign(count, 0);
        return;
    }
    char text[64];
    std::string line;
    if (b.address != address || b.words.size() != count) {
        std::snprintf(text, sizeof text, " at %#llx:", static_cast<unsigned long long>(address));
        line = "foliage: " + key + text;
        b.changes.assign(count, 0);
        for (const std::uint64_t w : now) {
            std::snprintf(text, sizeof text, " %llx", static_cast<unsigned long long>(w));
            line += text;
        }
    } else {
        for (std::size_t i = 0; i < count; ++i) {
            if (now[i] == b.words[i] || ++b.changes[i] > kNoisy + 1) {
                continue;
            }
            if (b.changes[i] == kNoisy + 1) {
                std::snprintf(text, sizeof text, " +%#zx:noisy", 8 * i);
            } else {
                std::snprintf(text, sizeof text, " +%#zx:%llx>%llx", 8 * i,
                              static_cast<unsigned long long>(b.words[i]),
                              static_cast<unsigned long long>(now[i]));
            }
            line += text;
        }
        if (line.empty()) {
            return;
        }
        line = "foliage: " + key + " changed" + line;
    }
    b.address = address;
    b.words = std::move(now);
    ++b.lines;
    ++lines_;
    out.push_back(line + where);
}

std::vector<std::string> FoliageProbe::Look(const ReadWord& read, std::uint64_t main,
                                            const std::optional<std::array<float, 3>>& link) {
    std::vector<std::string> out;
    if (++calls_ % 4 != 0 || main == 0) {
        return out;
    }
    const auto module = read(main + kFoliageModule);
    const std::uint64_t flags = module && *module ? read(*module + 0x30).value_or(0) & 0xff : 0;
    // As the game's own 0x2c013c0: with bit 0 of the module's byte +0x30 set there is no scene.
    const bool off = (flags & 1) != 0;
    const auto holder = module && *module ? read(*module + 0xf0) : std::nullopt;
    const auto scene = !off && holder && *holder ? read(*holder + 0x338) : std::nullopt;
    const std::uint64_t s = scene.value_or(0);
    char text[160];
    if (s != scene_) {
        scene_ = s;
        blocks_.clear();
        std::snprintf(text, sizeof text,
                      "foliage: module %#llx flags %#llx holder %#llx scene %#llx%s",
                      static_cast<unsigned long long>(module.value_or(0)),
                      static_cast<unsigned long long>(flags),
                      static_cast<unsigned long long>(holder.value_or(0)),
                      static_cast<unsigned long long>(s), off ? " (off)" : "");
        out.emplace_back(text);
    }
    if (s == 0) {
        return out;
    }
    std::string where;
    if (link) {
        std::snprintf(text, sizeof text, " | Link %.2f %.2f %.2f", (*link)[0], (*link)[1],
                      (*link)[2]);
        where = text;
    }
    // The scene's first 0x400 bytes: SsboGrassCut is +0x58, SsboGather +0x88. A cut that
    // lands in a field this walk does not follow still shows here.
    Compare(read, "scene", s, 128, where, out);
    for (std::uint64_t field = 0x58; field < 0x88; field += 8) {
        const auto p = read(s + field);
        if (!p || !LooksLikePointer(*p)) {
            continue;
        }
        std::snprintf(text, sizeof text, "cut+%#llx", static_cast<unsigned long long>(field));
        const std::string key = text;
        Compare(read, key, *p, 64, where, out);
        for (std::uint64_t inner = 0; inner < 0x40; inner += 8) {
            const auto q = read(*p + inner);
            if (!q || !LooksLikePointer(*q) || *q == *p) {
                continue;
            }
            std::snprintf(text, sizeof text, "->+%#llx", static_cast<unsigned long long>(inner));
            Compare(read, key + text, *q, 64, where, out);
        }
    }
    return out;
}

namespace {

constexpr std::uint64_t kMalloc = 0x14c6c90;  // main + this (coop_puppet.h)
constexpr float kRequestDefault = 0.3f;       // what the game puts at +0x10

float FloatAt(const GrassSync::Slot& s, std::size_t offset) {
    float f = 0.0f;
    std::memcpy(&f, s.data() + offset, 4);
    return f;
}

void PutFloat(GrassSync::Slot& s, std::size_t offset, float f) {
    std::memcpy(s.data() + offset, &f, 4);
}

// The same cut: position and radius, bit for bit.
bool SameCut(const GrassSync::Slot& a, const GrassSync::Slot& b) {
    return std::memcmp(a.data(), b.data(), 16) == 0;
}

std::optional<std::uint64_t> SceneOf(const GrassSync::ReadWord& read, std::uint64_t main) {
    const auto module = read(main + kFoliageModule);
    if (!module || *module == 0) {
        return std::nullopt;
    }
    if ((read(*module + 0x30).value_or(1) & 1) != 0) {
        return std::nullopt;  // off, as the game's 0x2c013c0 reads it
    }
    const auto holder = read(*module + 0xf0);
    if (!holder || *holder == 0) {
        return std::nullopt;
    }
    const auto scene = read(*holder + 0x338);
    if (!scene || *scene == 0) {
        return std::nullopt;
    }
    return scene;
}

} // namespace

coop::protocol::GrassCut GrassSync::ToMessage(const Slot& slot, const std::string& scene) {
    coop::protocol::GrassCut c;
    c.scene = scene;
    for (std::size_t i = 0; i < 3; ++i) {
        c.position[i] = FloatAt(slot, 4 * i);
        for (std::size_t j = 0; j < 3; ++j) {
            c.sweep[i][j] = FloatAt(slot, 0x18 + 0xc * i + 4 * j);
        }
    }
    c.radius = FloatAt(slot, 0xc);
    c.flags = slot[0x14];
    return c;
}

GrassSync::Slot GrassSync::ToSlot(const coop::protocol::GrassCut& cut) {
    Slot s{};
    for (std::size_t i = 0; i < 3; ++i) {
        PutFloat(s, 4 * i, cut.position[i]);
        for (std::size_t j = 0; j < 3; ++j) {
            PutFloat(s, 0x18 + 0xc * i + 4 * j, cut.sweep[i][j]);
        }
    }
    PutFloat(s, 0xc, cut.radius);
    PutFloat(s, 0x10, kRequestDefault);
    s[0x14] = cut.flags;
    return s;
}

std::vector<coop::protocol::GrassCut> GrassSync::Look(const ReadWord& read, std::uint64_t main,
                                                      const std::string& scene,
                                                      std::uint64_t now_ms) {
    std::vector<coop::protocol::GrassCut> out;
    const auto s = main != 0 ? SceneOf(read, main) : std::nullopt;
    {
        std::lock_guard lock(mutex_);
        main_ = main;
        if (scene != scene_) {
            scene_ = scene;
            waiting_.clear();  // cuts of the scene we left
        }
        while (!made_.empty() && now_ms - made_.front().first > kEchoMs) {
            made_.pop_front();
        }
    }
    if (!s) {
        scene_address_ = 0;
        last_index_.reset();
        return out;
    }
    if (*s != scene_address_) {
        scene_address_ = *s;
        last_index_.reset();
    }
    const auto capacity = read(*s + 0x20);
    const auto entries = read(*s + 0x28);
    const auto written = read(*s + 0x38);
    const std::uint64_t cap = capacity.value_or(0) & 0xffffffff;
    if (!entries || !written || cap == 0 || cap > 4096 || (cap & (cap - 1)) != 0) {
        return out;
    }
    if (!last_index_ || *written < *last_index_) {
        last_index_ = *written;  // the queue is new to us: only cuts from now on
        return out;
    }
    std::uint64_t from = *last_index_;
    if (*written - from > cap) {
        from = *written - cap;
    }
    last_index_ = *written;
    std::optional<Slot> previous;
    for (std::uint64_t i = from; i < *written; ++i) {
        const std::uint64_t entry = *entries + 16 * (i & (cap - 1));
        const auto pointer = read(entry);
        const auto sequence = read(entry + 8);
        // i + 1 while queued, i + capacity once taken out (and not reused).
        if (!pointer || *pointer == 0 || !sequence ||
            (*sequence != i + 1 && *sequence != i + cap)) {
            continue;
        }
        Slot slot{};
        bool ok = true;
        for (std::size_t k = 0; k < kCutSlotSize && ok; k += 8) {
            const auto w = read(*pointer + k);
            ok = w.has_value();
            if (ok) {
                std::memcpy(slot.data() + k, &*w, std::min<std::size_t>(8, kCutSlotSize - k));
            }
        }
        if (!ok || (previous && SameCut(*previous, slot))) {
            continue;
        }
        previous = slot;
        coop::protocol::GrassCut cut = ToMessage(slot, scene);
        bool echo = false;
        {
            std::lock_guard lock(mutex_);
            for (auto it = made_.begin(); it != made_.end(); ++it) {
                if (SameCut(it->second, slot)) {
                    made_.erase(it);
                    echo = true;
                    break;
                }
            }
        }
        const coop::protocol::Message check = cut;
        std::vector<std::uint8_t> bytes;
        if (echo || scene.empty() ||
            coop::protocol::encode(check, bytes) != coop::protocol::Error::none) {
            continue;  // made here for another game, or not a cut we can send
        }
        out.push_back(std::move(cut));
    }
    return out;
}

void GrassSync::Receive(const coop::protocol::GrassCut& cut) {
    std::lock_guard lock(mutex_);
    if (cut.scene != scene_ || waiting_.size() >= kMaxWaiting) {
        return;
    }
    waiting_.push_back(ToSlot(cut));
}

std::optional<CoopPuppet::Call> GrassSync::Next(CoopPuppet::Guest& guest, std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    if (waiting_.empty() || main_ == 0 || this_frame_ >= kCutsPerFrame) {
        return std::nullopt;
    }
    if (block_ == 0) {
        if (allocating_ || now_ms < retry_ms_) {
            return std::nullopt;
        }
        allocating_ = true;
        CoopPuppet::Call c;
        c.function = main_ + kMalloc;
        c.x[0] = 0x40;
        return c;
    }
    std::uint64_t module = 0;
    if (!guest.Read(main_ + kFoliageModule, &module, sizeof module) || module == 0) {
        waiting_.clear();
        return std::nullopt;
    }
    const Slot slot = waiting_.front();
    waiting_.pop_front();
    if (!guest.Write(block_, slot.data(), slot.size())) {
        return std::nullopt;
    }
    made_.emplace_back(now_ms, slot);
    ++made_count_;
    ++this_frame_;
    CoopPuppet::Call c;
    c.function = main_ + kCutFoliage;
    c.x[0] = module;
    c.x[1] = block_;
    return c;
}

std::optional<CoopPuppet::Call> GrassSync::Step(CoopPuppet::Guest& guest, bool after_call,
                                                std::uint64_t result, std::uint64_t now_ms) {
    {
        std::lock_guard lock(mutex_);
        if (!after_call) {
            this_frame_ = 0;
        } else if (allocating_) {
            allocating_ = false;
            if (result == 0) {
                retry_ms_ = now_ms + 5000;
                notes_.emplace_back("grass: malloc gave nothing; again in 5 s");
            } else {
                block_ = result;
                char text[64];
                std::snprintf(text, sizeof text, "grass: request at %#llx",
                              static_cast<unsigned long long>(result));
                notes_.emplace_back(text);
            }
        }
    }
    return Next(guest, now_ms);
}

std::uint64_t GrassSync::Made() const {
    std::lock_guard lock(mutex_);
    return made_count_;
}

std::vector<std::string> GrassSync::TakeNotes() {
    std::lock_guard lock(mutex_);
    return std::exchange(notes_, {});
}

} // namespace CoopWorld
