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
// LICHII_COOP_OBJECTS=1 shares enemies with the other games (Share and
// OnEvent below; the rules are in coop_objects_sync.h). Off by default until
// the two-game test passes.
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
#include <variant>
#include <vector>

#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "coop/host/game_flags.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_objects_sync.h"

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
// from docs/items.md, and the count each is kept at. Entries with is_get 0 are
// the ammo capacities (unnamed struct 0xf80f8077, docs/items.md): without one
// the item is in the menu but the game gives none (bombs: Link lifts empty hands).
struct CheatItem {
    const char* name;
    u32 stock;
    u32 is_get;
    s32 count;
};
constexpr std::array<CheatItem, 10> kCheatItems{{
    {"Weapon_Range_Bow", 0x938d56bf, 0x69ec28be, 1},
    {"Item_Quiver_Small", 0xa8c3e1fb, 0x0a550531, 1},
    {"Item_Arrow_Normal", 0x518ec74b, 0x572701ea, 30},
    {"capacity Arrow", 0x418db791, 0, 30},
    {"Item_BombBag_Small", 0xb0a1bf51, 0xa63f1e2d, 1},
    {"Item_Bomb", 0xdb1bcd4f, 0x2bb00768, 20},
    {"capacity Bomb", 0xe753508b, 0, 20},
    {"Weapon_Range_SlingShot", 0x80d34ce9, 0x0ec22ee1, 1},
    {"Item_DekuSeed", 0x5fadf7fc, 0x29888e1e, 30},
    {"capacity DekuSeed", 0xae8cc742, 0, 30},
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
        if (++frame_ % kEveryFrames != 0 || !Ready(system)) {
            return;
        }
        if (cheat_ && frame_ % kCheatEveryFrames == 0) {
            Cheat(system);
        }
        if (debug_) {
            Reader r(system.ApplicationMemory());
            Walk(r);
        }
    }

    // Present thread, after the session's frame. Not connected, or no scene:
    // forgets what it shared.
    void Share(Core::System& system, coop::host::Session* session,
               const std::optional<std::string>& scene, u64 server_now_us) {
        if (session == nullptr || !scene || scene->empty() || server_now_us == 0) {
            if (sharing_) {
                sync_.Reset();
                sharing_ = false;
            }
            return;
        }
        if (++share_frame_ % kEveryFrames != 0 || !Ready(system)) {
            return;
        }
        Reader r(system.ApplicationMemory());
        std::vector<Seen> seen;
        if (!Collect(r, seen)) {
            return;
        }
        const u8 self =
            session->with_client([](coop::net::Client& c) { return c.player_number(); });
        if (self == 0) {
            return;
        }
        sharing_ = true;
        std::vector<coop::protocol::Message> send;
        std::vector<LifeWrite> writes;
        sync_.Tick(*scene, self, server_now_us, seen, send, writes);
        for (const LifeWrite& w : writes) {
            WriteLife(system.ApplicationMemory(), r, w);
        }
        if (send.empty()) {
            return;
        }
        session->with_client([&](coop::net::Client& c) {
            for (const auto& m : send) {
                const bool sent = c.send(m);
                if (const auto* d = std::get_if<coop::protocol::Damage>(&m)) {
                    LOG_INFO(Core, "coop: objects: share damage {} to {:#x} epoch {}{}", d->amount,
                             d->id.value, d->epoch, sent ? "" : " (not sent)");
                } else if (const auto* p = std::get_if<coop::protocol::ObjectPresent>(&m);
                           p != nullptr && p->objects.size() != present_logged_) {
                    present_logged_ = p->objects.size();
                    LOG_INFO(Core, "coop: objects: share present {} enemies in {}{}",
                             p->objects.size(), p->scene, sent ? "" : " (not sent)");
                }
            }
        });
    }

    // Network thread.
    void Receive(const coop::protocol::Message& m) {
        if (const auto* o = std::get_if<coop::protocol::ObjectOwner>(&m)) {
            LOG_INFO(Core, "coop: objects: share owner {:#x} is player {} epoch {}", o->id.value,
                     o->owner, o->epoch);
        } else if (const auto* d = std::get_if<coop::protocol::Damage>(&m)) {
            LOG_INFO(Core, "coop: objects: share got damage {} for {:#x}", d->amount,
                     d->id.value);
        }
        sync_.Receive(m);
    }

    bool debug_ = false;
    bool cheat_ = false;
    bool share_ = false;

private:
    // The game is the right one; finds main once per process.
    bool Ready(Core::System& system) {
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
            names_.clear();
            sync_.Reset();
            LOG_INFO(Core, "coop: objects: main at {:#x}", main_);
        }
        return true;
    }

    // The shared actors of one whole walk, with life and place. False when
    // the list changed under us (try the next frame).
    bool Collect(Reader& r, std::vector<Seen>& out) {
        std::vector<u64> actors;
        if (!List(r, actors)) {
            return false;
        }
        std::unordered_map<u64, std::pair<std::string, u64>> names;
        for (const u64 a : actors) {
            auto it = names_.find(a);
            std::pair<std::string, u64> known;
            if (it != names_.end()) {
                known = std::move(it->second);
            } else {
                known.first = r.Name(a);
                const auto placement = r.Ptr(a + kActorPlacement);
                known.second = placement ? r.Get<u64>(*placement + kPlacementHash).value_or(0) : 0;
            }
            if (IsShared(known.first)) {
                Seen s;
                s.address = a;
                s.name = known.first;
                s.hash = known.second;
                for (std::size_t i = 0; i < 3; ++i) {
                    s.position[i] = r.Get<float>(a + kActorPosition + 4 * i).value_or(0.0f);
                }
                if (const auto value = LifeValue(r, a)) {
                    const auto life = r.Get<s32>(value->first + 8);
                    const auto max = r.Get<s32>(value->second + 8);
                    if (life && max) {
                        s.life = coop::protocol::ObjectLife{*life, *max};
                    }
                }
                out.push_back(std::move(s));
            }
            names.emplace(a, std::move(known));
        }
        names_ = std::move(names);
        return true;
    }

    // The life and max value entries ([life + 0x1c10], [life + 0x1c18]).
    std::optional<std::pair<u64, u64>> LifeValue(Reader& r, u64 a) {
        const auto table = r.Ptr(a + kActorComponents);
        const auto life = table ? r.Ptr(*table + kComponentLife) : std::nullopt;
        const auto value = life ? r.Ptr(*life + kLifeValue) : std::nullopt;
        const auto max = life ? r.Ptr(*life + kLifeMax) : std::nullopt;
        if (!value || !max) {
            return std::nullopt;
        }
        return std::pair{*value, *max};
    }

    // A direct write of life is enough for the game (docs/objects.md, "Damage").
    void WriteLife(Core::Memory::Memory& memory, Reader& r, const LifeWrite& w) {
        const auto value = LifeValue(r, w.address);
        if (!value || !memory.IsValidVirtualAddressRange(value->first + 8, 4)) {
            return;
        }
        const s32 old = r.Get<s32>(value->first + 8).value_or(-1);
        memory.Write32(value->first + 8, static_cast<u32>(w.life));
        LOG_INFO(Core, "coop: objects: share life {:#x}: {} -> {}", w.address, old, w.life);
    }

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
            if (item.is_get != 0 && flags.get_bool(item.is_get) == false) {
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

    // The actors on the live list. False when the list changed under us.
    bool List(Reader& r, std::vector<u64>& out) {
        const auto manager = r.Ptr(main_ + kActorManagerPtr);
        if (!manager) {
            return false;
        }
        const auto lists = r.Ptr(*manager + kManagerLists);
        if (!lists) {
            return false;
        }
        const u64 head = *lists + kListHead;
        const auto count = r.Get<u32>(head + kListCount);
        if (!count) {
            return false;
        }
        std::size_t walked = 0;
        auto node = r.Ptr(head);
        while (node && *node != head && walked < kMaxWalk) {
            ++walked;
            const auto owner = r.Get<u64>(*node + kNodeOwner);
            if (owner && *owner + kActorNode == *node) {
                out.push_back(*owner);
            }
            node = r.Ptr(*node);
        }
        const bool whole = node && walked == *count;
        if (first_ && whole) {
            LOG_INFO(Core, "coop: objects: list head {:#x}, {} entries, {} actors", head, walked,
                     out.size());
        }
        return whole;
    }

    void Walk(Reader& r) {
        std::vector<u64> list;
        if (!List(r, list)) {
            return;  // the list changed under us; try the next frame
        }
        const std::unordered_set<u64> seen(list.begin(), list.end());
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
    unsigned share_frame_ = 0;
    bool sharing_ = false;
    std::size_t present_logged_ = ~std::size_t{0};
    std::unordered_map<u64, std::pair<std::string, u64>> names_;  // name, hash by actor
    Sync sync_;
    Kernel::KProcess* process_ = nullptr;
    u64 main_ = 0;
    bool first_ = true;
    std::unordered_map<u64, Actor> actors_;
};

Objects& Instance() {
    static Objects objects;  // first called on the present thread
    static const bool once = [] {
        objects.debug_ = EnvFlag("LICHII_COOP_DEBUG_OBJECTS");
        objects.cheat_ = EnvFlag("LICHII_COOP_OBJECTS_CHEAT");
        objects.share_ = EnvFlag("LICHII_COOP_OBJECTS");
        return true;
    }();
    (void)once;
    return objects;
}

} // namespace

void OnFrame(Core::System& system) {
    Objects& objects = Instance();
    if (objects.debug_ || objects.cheat_) {
        objects.Frame(system);  // the present thread only
    }
}

void Share(Core::System& system, coop::host::Session* session,
           const std::optional<std::string>& scene, std::uint64_t server_now_us) {
    Objects& objects = Instance();
    if (objects.share_) {
        objects.Share(system, session, scene, server_now_us);
    }
}

void OnEvent(const coop::net::Event& event) {
    if (event.kind != coop::net::EventKind::message) {
        return;
    }
    Objects& objects = Instance();
    if (objects.share_) {
        objects.Receive(event.message);
    }
}

} // namespace CoopObjects
