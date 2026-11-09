// SPDX-License-Identifier: GPL-3.0-or-later
//
// LICHII_COOP_DEBUG_OBJECTS=1: logs the game's actors as they come and go,
// and enemy life, for docs/objects.md ("Live check").
//
// The actor list, read live (build e0f3ccfaf50065ae):
//   manager  = [main + 0x4e91888]
//   head     = [manager + 0x118] + 0x670, a circular doubly linked list;
//              the u32 at head + 0x10 is its length
//   node     = {next +0, prev +8, owner +0x10}; an actor's own node is at
//              actor + 0x538, so owner == node - 0x538 for actors (a few
//              other objects share the list and are skipped)
// Actor fields: name char* +0x260, state u32 +0x2c, position 3 x f32 +0x31c,
// placement hash u64 at [+0x3d8] + 0x88 (~0 for actors the game made),
// life component = [[+0x270] + 0x2b8], life s32 at [life + 0x1c10] + 8,
// max at [life + 0x1c18] + 8.
//
// LICHII_COOP_OBJECTS_CHEAT=1 (test only, for the stage 5 live checks): keeps
// the bow, the small quiver with 30 arrows, the small bomb bag and 20 bombs in
// the local player's pouch, topped up once a second through the game's own
// flag write queue (docs/items.md hashes). It touches nothing else.
//
// This runs on the present thread while the guest runs, so a walk can see the
// list mid-change. A walk whose length differs from the head's count is
// dropped, and an actor counts as gone only when two walks in a row miss it.

#include "coop_objects.h"
#include "coop_flags.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "coop/host/game_flags.hpp"

namespace CoopObjects {
namespace {

constexpr u64 kOcarinaProgramId = 0x0400486024DEC000ULL;
constexpr std::array<u8, 8> kGameBuildId{0xe0, 0xf3, 0xcc, 0xfa, 0xf5, 0x00, 0x65, 0xae};

constexpr u64 kActorManagerPtr = 0x4e91888;  // main offset
constexpr u64 kManagerLists = 0x118;
constexpr u64 kListHead = 0x670;
constexpr u64 kListCount = 0x10;
constexpr u64 kNodeOwner = 0x10;
constexpr u64 kActorNode = 0x538;
constexpr u64 kActorName = 0x260;
constexpr u64 kActorState = 0x2c;
constexpr u64 kActorPosition = 0x31c;
constexpr u64 kActorComponents = 0x270;
constexpr u64 kComponentLife = 0x2b8;
constexpr u64 kLifeValue = 0x1c10;
constexpr u64 kLifeMax = 0x1c18;
constexpr u64 kLifeCopy = 0x1ca8;  // life and max as last mirrored
constexpr u64 kActorPlacement = 0x3d8;
constexpr u64 kPlacementHash = 0x88;
constexpr std::size_t kMaxWalk = 8192;
constexpr unsigned kEveryFrames = 2;

// LICHII_COOP_OBJECTS_CHEAT: PouchData.<item>.StockNum and IsGet.<item> hashes
// from docs/items.md, and the count each is kept at.
struct CheatItem {
    const char* name;
    u32 stock;
    u32 is_get;
    s32 count;
};
constexpr std::array<CheatItem, 5> kCheatItems{{
    {"Weapon_Range_Bow", 0x938d56bf, 0x69ec28be, 1},
    {"Item_Quiver_Small", 0xa8c3e1fb, 0x0a550531, 1},
    {"Item_Arrow_Normal", 0x518ec74b, 0x572701ea, 30},
    {"Item_BombBag_Small", 0xb0a1bf51, 0xa63f1e2d, 1},
    {"Item_Bomb", 0xdb1bcd4f, 0x2bb00768, 20},
}};
constexpr unsigned kCheatEveryFrames = 60;

bool EnvFlag(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return false;
    }
    const bool on = value[0] != '\0' && std::string_view(value) != "0";
    std::free(value);
    return on;
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::string_view(value) != "0";
#endif
}

struct Actor {
    std::string name;
    u64 hash = 0;
    std::optional<std::array<s32, 4>> life;  // life, max, copy of life, copy of max
    unsigned missed = 0;
};

class Reader {
public:
    explicit Reader(Core::Memory::Memory& memory) : memory_(memory) {}

    template <typename T>
    std::optional<T> Get(u64 address) {
        if (address < 0x1000 || !memory_.IsValidVirtualAddressRange(address, sizeof(T))) {
            return std::nullopt;
        }
        T value{};
        memory_.ReadBlockUnsafe(address, &value, sizeof(T));
        return value;
    }

    std::optional<u64> Ptr(u64 address) {
        auto p = Get<u64>(address);
        if (!p || *p == 0) {
            return std::nullopt;
        }
        return p;
    }

    std::string Name(u64 actor) {
        const auto p = Ptr(actor + kActorName);
        if (!p) {
            return "?";
        }
        std::array<char, 64> buf{};
        if (!memory_.IsValidVirtualAddressRange(*p, buf.size())) {
            return "?";
        }
        memory_.ReadBlockUnsafe(*p, buf.data(), buf.size() - 1);
        return std::string(buf.data(), strnlen(buf.data(), buf.size()));
    }

private:
    Core::Memory::Memory& memory_;
};

class Objects {
public:
    void Frame(Core::System& system) {
        if (++frame_ % kEveryFrames != 0) {
            return;
        }
        auto* process = system.ApplicationProcess();
        if (process == nullptr || system.GetApplicationProcessProgramID() != kOcarinaProgramId) {
            return;
        }
        const auto& build = system.GetApplicationProcessBuildID();
        if (std::memcmp(build.data(), kGameBuildId.data(), kGameBuildId.size()) != 0) {
            return;
        }
        if (process != process_) {
            process_ = process;
            main_ = FindMain(system, process);
            actors_.clear();
            first_ = true;
            LOG_INFO(Core, "coop: objects: main at {:#x}", main_);
        }
        if (cheat_ && frame_ % kCheatEveryFrames == 0) {
            Cheat(system);
        }
        if (debug_) {
            Reader r(system.ApplicationMemory());
            Walk(r);
        }
    }

    bool debug_ = false;
    bool cheat_ = false;

private:
    // Tops up each cheat item below its count. A read that fails means the
    // flag manager is not up yet (title screen): try again next time.
    void Cheat(Core::System& system) {
        CoopHook::EmulatorMemory memory(system.ApplicationMemory(), main_);
        coop::host::GameFlags flags(memory);
        for (const CheatItem& item : kCheatItems) {
            const auto now = flags.get_int(item.stock);
            if (!now || *now >= item.count) {
                continue;
            }
            const bool set = coop::host::GameFlags::set_int(memory, item.stock, item.count);
            if (flags.get_bool(item.is_get) == false) {
                coop::host::GameFlags::set_bool(memory, item.is_get, true);
            }
            LOG_INFO(Core, "coop: objects: cheat {} {} -> {}{}", item.name, *now, item.count,
                     set ? "" : " (queue busy, again next second)");
        }
    }

    static u64 FindMain(Core::System& system, Kernel::KProcess* process) {
        Loader::AppLoader::Modules modules;
        if (system.GetAppLoader().ReadNSOModules(modules) != Loader::ResultStatus::Success ||
            modules.empty()) {
            modules = Core::FindModules(process);
        }
        for (const auto& [address, name] : modules) {
            if (name == "main" || name.starts_with("main.")) {
                return address;
            }
        }
        return GetInteger(Core::FindMainModuleEntrypoint(process));
    }

    void Walk(Reader& r) {
        const auto manager = r.Ptr(main_ + kActorManagerPtr);
        if (!manager) {
            return;
        }
        const auto lists = r.Ptr(*manager + kManagerLists);
        if (!lists) {
            return;
        }
        const u64 head = *lists + kListHead;
        const auto count = r.Get<u32>(head + kListCount);
        if (!count) {
            return;
        }
        std::unordered_set<u64> seen;
        std::size_t walked = 0;
        auto node = r.Ptr(head);
        while (node && *node != head && walked < kMaxWalk) {
            ++walked;
            const auto owner = r.Get<u64>(*node + kNodeOwner);
            if (owner && *owner + kActorNode == *node) {
                seen.insert(*owner);
            }
            node = r.Ptr(*node);
        }
        if (!node || walked != *count) {
            return;  // the list changed under us; try the next frame
        }
        if (first_) {
            LOG_INFO(Core, "coop: objects: list head {:#x}, {} entries, {} actors", head, walked,
                     seen.size());
        }
        for (const u64 a : seen) {
            auto [it, added] = actors_.try_emplace(a);
            Actor& actor = it->second;
            actor.missed = 0;
            if (added) {
                actor.name = r.Name(a);
                const auto placement = r.Ptr(a + kActorPlacement);
                actor.hash = placement ? r.Get<u64>(*placement + kPlacementHash).value_or(0) : 0;
                if (!first_) {
                    LogPose(r, "+", a, actor);
                }
            }
            if (actor.name.starts_with("Enemy_")) {
                Life(r, a, actor);
            }
        }
        for (auto it = actors_.begin(); it != actors_.end();) {
            if (seen.contains(it->first) || ++it->second.missed < 2) {
                ++it;
                continue;
            }
            LOG_INFO(Core, "coop: objects: - {} {:#x} hash {:#x}", it->second.name, it->first,
                     it->second.hash);
            it = actors_.erase(it);
        }
        first_ = false;
    }

    void LogPose(Reader& r, const char* what, u64 a, const Actor& actor) {
        const auto x = r.Get<float>(a + kActorPosition);
        const auto y = r.Get<float>(a + kActorPosition + 4);
        const auto z = r.Get<float>(a + kActorPosition + 8);
        const auto state = r.Get<u32>(a + kActorState);
        LOG_INFO(Core, "coop: objects: {} {} {:#x} state {} at {:.2f} {:.2f} {:.2f} hash {:#x}",
                 what, actor.name, a, state.value_or(0), x.value_or(0.0f), y.value_or(0.0f),
                 z.value_or(0.0f), actor.hash);
    }

    void Life(Reader& r, u64 a, Actor& actor) {
        const auto table = r.Ptr(a + kActorComponents);
        const auto life = table ? r.Ptr(*table + kComponentLife) : std::nullopt;
        if (!life) {
            return;
        }
        const auto value = r.Ptr(*life + kLifeValue);
        const auto max = r.Ptr(*life + kLifeMax);
        if (!value || !max) {
            return;
        }
        const std::array<s32, 4> now{
            r.Get<s32>(*value + 8).value_or(-1), r.Get<s32>(*max + 8).value_or(-1),
            r.Get<s32>(*life + kLifeCopy).value_or(-1), r.Get<s32>(*life + kLifeCopy + 4).value_or(-1)};
        if (actor.life && (*actor.life)[0] == now[0] && (*actor.life)[1] == now[1]) {
            return;  // the copy alone moves a frame or two later; not logged
        }
        LOG_INFO(Core, "coop: objects: life {} {:#x} hash {:#x}: {} -> {} of {} (copy {} of {})",
                 actor.name, a, actor.hash, actor.life ? (*actor.life)[0] : now[0], now[0], now[1],
                 now[2], now[3]);
        actor.life = now;
    }

    unsigned frame_ = 0;
    Kernel::KProcess* process_ = nullptr;
    u64 main_ = 0;
    bool first_ = true;
    std::unordered_map<u64, Actor> actors_;
};

} // namespace

void OnFrame(Core::System& system) {
    static const bool debug = EnvFlag("LICHII_COOP_DEBUG_OBJECTS");
    static const bool cheat = EnvFlag("LICHII_COOP_OBJECTS_CHEAT");
    if (!debug && !cheat) {
        return;
    }
    static Objects objects = [] {
        Objects o;
        o.debug_ = debug;
        o.cheat_ = cheat;
        return o;
    }();  // the present thread only
    objects.Frame(system);
}

} // namespace CoopObjects
