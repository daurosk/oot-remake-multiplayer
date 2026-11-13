// SPDX-License-Identifier: GPL-3.0-or-later
//
// A stand-in for the game behind NX2navi's plugin interface (version 2), for
// the shared-enemies port's tests: guest memory as a set of regions, the
// game's live actor list laid out as lichii/hook/coop_objects.cpp reads it,
// and an Nx2PluginApi whose read, write, map_range, hook_function,
// call_original and guest_call_regs work on that memory. guest_call_regs
// carries out the few game functions the hook's Mover calls (the setter, the
// animation play, the frame set, the delete request, malloc) and records
// every call.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "plugin_api_v2.h"

namespace FakeGame {

inline constexpr std::uint64_t kMain = 0x7000000000ull;

struct CallRecord {
    std::uint64_t function = 0;  // absolute
    std::array<std::uint64_t, 8> x{};
    float s0 = 0.0f;
    std::vector<std::uint64_t> stack;
};

struct Hook {
    std::uint64_t address = 0;
    Nx2HookFn fn = nullptr;
    void* user = nullptr;
    int handle = 0;
};

// One actor of the fake list, with the components the hook reads.
struct Actor {
    std::uint64_t address = 0;
    std::uint64_t table = 0;   // component table (+0x270)
    std::uint64_t life = 0;    // Life component (table +0x2b8), 0 when none
    std::uint64_t life_value = 0, life_max = 0;  // the value objects (+8 is the s32)
    std::uint64_t slot = 0;    // AS slot 0
    std::uint64_t bsa = 0;     // BSA component (table +0xb0)
};

class Game {
public:
    Game();
    ~Game();

    // The interface the DLL gets; valid while this Game lives (one at a time).
    const Nx2PluginApi* Api() const { return &api_; }
    // A version 1 table (no hooks, no map_range).
    const Nx2PluginApi* ApiV1() const { return &api_v1_; }

    // Memory.
    std::uint64_t Alloc(std::size_t size);  // zeroed, 16-aligned, in its own region
    bool Read(std::uint64_t address, void* out, std::size_t size) const;
    bool Write(std::uint64_t address, const void* in, std::size_t size);
    void* Map(std::uint64_t address, std::size_t size);
    template <typename T>
    T Get(std::uint64_t address) const {
        T v{};
        Read(address, &v, sizeof(v));
        return v;
    }
    template <typename T>
    void Put(std::uint64_t address, const T& v) {
        Write(address, &v, sizeof(v));
    }
    std::uint64_t String(const std::string& text);  // a NUL-terminated copy in memory

    // The actor list.
    Actor Add(const std::string& name, std::uint64_t hash, std::array<float, 3> at,
              std::optional<std::pair<std::int32_t, std::int32_t>> life = std::nullopt,
              const std::string& animation = {});
    void Unlink(std::uint64_t actor);
    std::array<float, 3> Position(std::uint64_t actor) const {
        return Get<std::array<float, 3>>(actor + 0x31c);
    }
    void SetPosition(std::uint64_t actor, std::array<float, 3> at) { Put(actor + 0x31c, at); }
    std::int32_t Life(const Actor& a) const { return Get<std::int32_t>(a.life_value + 8); }
    void SetLife(const Actor& a, std::int32_t life) { Put(a.life_value + 8, life); }
    std::string SlotName(const Actor& a) const;

    // The game's frame step: runs the hooks on main+0x3b4e300 (the callers).
    void Step();
    // Calls the hooks registered on `address` with this call, as the game
    // would; `original` is what the function itself does (call_original or
    // no hook skipping it).
    void CallHooked(std::uint64_t address, Nx2HookCall& call,
                    const std::function<void(Nx2HookCall&)>& original);
    bool Hooked(std::uint64_t address) const;
    std::vector<Hook> hooks;
    std::vector<CallRecord> calls;
    std::vector<std::string> log;
    std::mutex mutex;  // calls, log (the tests read them between steps)
    std::vector<CallRecord> CallsTo(std::uint64_t offset);

private:
    struct Region {
        std::vector<std::uint8_t> bytes;
    };
    std::map<std::uint64_t, Region> regions_;  // by base
    std::uint64_t next_ = 0x2000000000ull;
    std::uint64_t head_ = 0;  // the list head node
    Nx2PluginApi api_{};
    Nx2PluginApi api_v1_{};
    Nx2GameInfo info_{};
    int next_handle_ = 1;
    const std::function<void(Nx2HookCall&)>* original_ = nullptr;  // inside CallHooked

    const Region* Find(std::uint64_t address, std::size_t size, std::uint64_t& offset) const;
    std::uint64_t Run(const Nx2GuestRegs& regs, std::uint64_t function);
    void RunFrom(std::uint64_t address, Nx2HookCall& call, std::size_t index);

    friend struct Callbacks;
};

}  // namespace FakeGame
