// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_shot_reflect.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

namespace CoopShotReflect {

namespace {

// Every shared shot (coop_enemy_shots.cpp's table) whose ShootableParam chain sends it back (romfs, build
// e0f3ccfaf50065ae; the others say neither: Dodongo fire, shock waves, thunder, Twinrova's beams and balls,
// Poe and Flare Dancer fire, Skull Kid's). Twinrova's beams go back by the Mirror Shield's own
// _MirrorShieldReflect actor, Link's own (never an enemy shot). The Ganondorf charge ball's own AI asks its
// sensor itself (not its Shootable): open, see docs/objects.md.
struct Entry {
    std::string_view name;
    Way way;
};
constexpr std::array<Entry, 5> kReflected{{
    {"DungeonBossGanondrof_Bullet_LightBall", Way::melee},     // Ganondorf's (and Phantom Ganon's) light ball
    {"DungeonBossPhantomGanon_Bullet_LightBall", Way::melee},  // its parent: the one above
    {"Octorok_Bullet", Way::guard},                            // parent ScrubDeku_DekuNut
    {"ScrubDeku_DekuNut", Way::guard},
    {"ScrubForestStage_DekuNut", Way::guard},
}};

template <typename T>
bool Get(CoopPuppet::Guest& guest, std::uint64_t address, T& out) {
    return address != 0 && guest.Read(address, &out, sizeof(T));
}

struct List {
    std::uint64_t entries = 0;
    std::uint32_t count = 0;
};

std::optional<List> ReadList(CoopPuppet::Guest& guest, std::uint64_t shot, std::uint64_t at) {
    std::uint64_t table = 0, sensor = 0, list = 0;
    if (!Get(guest, shot + addr::kActorComponents, table) || table == 0 ||
        !Get(guest, table + addr::kComponentSensor, sensor) || sensor == 0 || !Get(guest, sensor + at, list) ||
        list == 0) {
        return std::nullopt;
    }
    std::uint32_t capacity = 0, count = 0;
    List l;
    if (!Get(guest, list + 0x8, capacity) || !Get(guest, list + 0x18, count) ||
        !Get(guest, list + 0x10, l.entries) || l.entries == 0) {
        return std::nullopt;
    }
    l.count = std::min({capacity, count, kMaxHits});
    return l;
}

std::string Hex(std::uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%#llx", static_cast<unsigned long long>(v));
    return b;
}

}  // namespace

Way ReflectedBy(std::string_view shot) {
    for (const Entry& e : kReflected) {
        if (shot == e.name) {
            return e.way;
        }
    }
    return Way::none;
}

const char* NameOf(Way way) {
    switch (way) {
    case Way::melee: return "melee";
    case Way::guard: return "guard";
    default: return "none";
    }
}

std::uint64_t HandleActor(CoopPuppet::Guest& guest, std::uint64_t handle) {
    std::uint64_t holder = 0, actor = 0;
    if (!Get(guest, handle + addr::kHandleHolder, holder) || holder == 0 ||
        !Get(guest, holder + addr::kHolderActor, actor)) {
        return 0;
    }
    return actor;
}

bool OurHit(CoopPuppet::Guest& guest, std::uint64_t shot, std::uint64_t link, Way way) {
    if (shot == 0 || link == 0 || way == Way::none) {
        return false;
    }
    if (way == Way::melee) {
        const auto list = ReadList(guest, shot, addr::kSensorTgList);
        if (!list) {
            return false;
        }
        for (std::uint32_t i = 0; i < list->count; ++i) {
            const std::uint64_t e = list->entries + i * addr::kTgEntrySize;
            std::uint8_t kind = 0, flags = 0;
            if (!Get(guest, e + addr::kTgKind, kind) || kind > 0x12 || ((1u << kind) & addr::kReflectKinds) == 0 ||
                !Get(guest, e + addr::kTgHandleFlags, flags)) {
                continue;
            }
            const std::uint64_t handle = e + ((flags & 1) != 0 ? addr::kTgAttackerB : addr::kTgAttackerA);
            if (HandleActor(guest, handle) == link) {
                return true;
            }
        }
        return false;
    }
    const auto list = ReadList(guest, shot, addr::kSensorAtList);
    if (!list) {
        return false;
    }
    for (std::uint32_t i = 0; i < list->count; ++i) {
        const std::uint64_t e = list->entries + i * addr::kAtEntrySize;
        std::uint8_t flags = 0;
        if (Get(guest, e + addr::kAtFlags, flags) && (flags & 1) != 0 &&
            HandleActor(guest, e + addr::kAtDefender) == link) {
            return true;
        }
    }
    return false;
}

std::string Describe(CoopPuppet::Guest& guest, std::uint64_t shot) {
    std::string out;
    if (const auto tg = ReadList(guest, shot, addr::kSensorTgList)) {
        for (std::uint32_t i = 0; i < std::min(tg->count, 4u); ++i) {
            const std::uint64_t e = tg->entries + i * addr::kTgEntrySize;
            std::uint8_t kind = 0, flags = 0;
            Get(guest, e + addr::kTgKind, kind);
            Get(guest, e + addr::kTgHandleFlags, flags);
            out += " taken kind " + std::to_string(kind) + " by " + Hex(HandleActor(guest, e + addr::kTgAttackerA)) +
                   "/" + Hex(HandleActor(guest, e + addr::kTgAttackerB)) + (flags & 1 ? " (b)" : " (a)");
        }
    }
    if (const auto at = ReadList(guest, shot, addr::kSensorAtList)) {
        for (std::uint32_t i = 0; i < std::min(at->count, 4u); ++i) {
            const std::uint64_t e = at->entries + i * addr::kAtEntrySize;
            std::uint8_t flags = 0;
            Get(guest, e + addr::kAtFlags, flags);
            out += " dealt flags " + Hex(flags) + " to " + Hex(HandleActor(guest, e + addr::kAtDefender));
        }
    }
    return out;
}

std::optional<Record> FindRecord(CoopPuppet::Guest& guest, std::uint64_t maker, std::uint64_t shot) {
    std::uint64_t table = 0, shooter = 0, map = 0;
    std::uint32_t slots = 0;
    if (maker == 0 || shot == 0 || !Get(guest, maker + addr::kActorComponents, table) || table == 0 ||
        !Get(guest, table + addr::kComponentShooter, shooter) || shooter == 0 ||
        !Get(guest, shooter + addr::kShooterMap, map) || map == 0 ||
        !Get(guest, shooter + addr::kShooterSlots, slots) || slots == 0 || slots > kMaxSlots) {
        return std::nullopt;
    }
    for (std::uint32_t s = 0; s < slots; ++s) {
        const std::uint64_t entry = map + s * addr::kMapEntrySize;
        std::uint32_t key = 0, count = 0;
        std::uint64_t records = 0;
        if (!Get(guest, entry, key) || key == 0 || !Get(guest, entry + addr::kEntryCount, count) ||
            !Get(guest, entry + addr::kEntryRecords, records) || records == 0) {
            continue;
        }
        for (std::uint32_t i = 0; i < std::min(count, kMaxRecords); ++i) {
            if (HandleActor(guest, records + i * addr::kRecordSize + addr::kRecordShot) == shot) {
                return Record{shooter, key, static_cast<std::int32_t>(i)};
            }
        }
    }
    return std::nullopt;
}

CoopPuppet::Call ReflectCall(std::uint64_t main_base, const Record& record) {
    CoopPuppet::Call c;
    c.function = main_base + addr::kRequestChangeStateReflected;
    c.x[0] = record.shooter;
    c.x[1] = record.key;
    c.x[2] = static_cast<std::uint64_t>(static_cast<std::uint32_t>(record.index));
    return c;
}

void Queue::Ask(std::uint64_t shot, std::string name, std::vector<std::uint64_t> makers, std::uint64_t main_base) {
    std::lock_guard lock(mutex_);
    if (asks_.size() < 32) {
        asks_.push_back(Ask_{shot, std::move(name), std::move(makers), main_base});
    }
}

std::optional<CoopPuppet::Call> Queue::Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result) {
    if (after_call && calling_) {
        std::lock_guard lock(mutex_);
        notes_.push_back(calling_->name + " " + Hex(calling_->shot) + ": the game's RequestChangeStateReflected " +
                         ((result & 0xff) != 0 ? "took it" : "refused it"));
        calling_.reset();
    }
    if (after_call) {
        return std::nullopt;  // one a frame
    }
    std::optional<Ask_> ask;
    {
        std::lock_guard lock(mutex_);
        if (asks_.empty()) {
            return std::nullopt;
        }
        ask = std::move(asks_.front());
        asks_.erase(asks_.begin());
    }
    for (const std::uint64_t maker : ask->makers) {
        if (const auto record = FindRecord(guest, maker, ask->shot)) {
            std::lock_guard lock(mutex_);
            ++asked_;
            notes_.push_back(ask->name + " " + Hex(ask->shot) + ": sent back, its maker " + Hex(maker) +
                             "'s shooter record key " + Hex(record->key) + " index " +
                             std::to_string(record->index));
            calling_ = std::move(ask);
            return ReflectCall(calling_->main_base, *record);
        }
    }
    std::lock_guard lock(mutex_);
    notes_.push_back(ask->name + " " + Hex(ask->shot) + ": sent back, but no shooter record of it in " +
                     std::to_string(ask->makers.size()) + " of our enemies (gone, or not a shooter's shot)");
    return std::nullopt;
}

std::vector<std::string> Queue::TakeNotes() {
    std::lock_guard lock(mutex_);
    return std::exchange(notes_, {});
}

void Queue::Reset() {
    std::lock_guard lock(mutex_);
    asks_.clear();
}

std::size_t Queue::Asked() const {
    std::lock_guard lock(mutex_);
    return asked_;
}

Queue& TheQueue() {
    static Queue queue;
    return queue;
}

}  // namespace CoopShotReflect
