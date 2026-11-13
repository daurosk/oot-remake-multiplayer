// SPDX-License-Identifier: GPL-3.0-or-later
// Puppets through NX2navi's plugin interface (puppet.hpp).
#include "puppet.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

#include "coop_camera.h"
#include "coop_pose.h"
#include "coop_puppet.h"
#include "overlay.hpp"
#include "teleport.hpp"
#include "third_party/plugin_api.h"

namespace CoopNx2navi::Puppet {
namespace {

// The game's own thin caller of the create function, main+0x2ef7600
// (obj, name, create argument, source): it copies `source` into obj + 0x10
// unless they are the same address, copies three fields of `source` to where
// they already are when source is obj + 0x10, and then calls
// requestCreateActorAsync(manager, name, argument, obj + 8, 0, 0, 0, 0) with
// the three stack arguments 0. guest_call cannot pass stack arguments (the
// callee sees whatever the presenting function has at its stack pointer, and
// the create function writes through the second one when it is not 0), so
// the director's create goes through this function, with obj = handle - 8 and
// source = handle + 8: the same call, but its fifth argument is 0 where the
// emulator's cave passes 1 (README, "Puppets").
constexpr std::uint64_t kCreateCaller = 0x2ef7600;

void (*g_log)(const char* line) = nullptr;
std::atomic<bool> g_configured{false};
std::atomic<bool> g_puppets{false};  // the director is asked (puppet = 1)
std::atomic<bool> g_broken{false};  // a guest call of the director was refused: no more of them
std::atomic<bool> g_teleport_broken{false};  // the same for teleport.hpp's calls
std::atomic<bool> g_hooked{false};  // the callback is a hook of the game's frame step
std::atomic<bool> g_fast{false};    // the interface's own read and write are fast (version 2)
// The game's frame step and where moduleCalc_ returns to after calling it: the
// emulator hook's own point (lichii/hook/coop_svc.h; README, "Puppets").
constexpr std::uint64_t kFrameStep = 0x3b4e300;
constexpr std::uint64_t kFrameStepReturn = 0x6e70fc;      // every frame
constexpr std::uint64_t kFrameStepReturnRare = 0x6e6ddc;  // the rare one
std::mutex g_reg;
bool g_frame_hook = true;           // Configure: try the frame step hook first
int g_handle = 0;                   // under g_reg
std::atomic<bool> g_active{false};
std::atomic<const Nx2PluginApi*> g_api{nullptr};

std::atomic<std::uint64_t> g_frames{0};       // frame callbacks
std::atomic<std::uint64_t> g_calls{0};        // guest calls made
std::atomic<std::uint64_t> g_frame_us{0};     // time spent in the frame callbacks
std::atomic<std::uint64_t> g_worst_us{0};     // the longest one
// Where that time goes: the interface's reads and writes and the game calls.
std::atomic<std::uint64_t> g_reads{0}, g_read_ns{0}, g_writes{0}, g_write_ns{0}, g_call_ns{0};
std::atomic<std::uint64_t> g_slow{0};  // reads and writes that went through the interface

std::uint64_t NowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
std::atomic<std::uint64_t> g_drawn_frame{0};  // Publish: frames with the player readable

// The session's frame work (SetFrameWork), called under this mutex.
std::mutex g_work_mutex;
FrameWork g_work = nullptr;
void* g_work_user = nullptr;

void Log(const char* format, ...)
#if defined(__GNUC__)
    __attribute__((format(gnu_printf, 1, 2)))
#endif
    ;
void Log(const char* format, ...) {
    if (g_log == nullptr) {
        return;
    }
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    g_log(line);
}

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// Guest memory for the director, on the game's thread inside a frame callback.
//
// The interface's read and write are the safe way, but each takes a lock of
// the game's memory manager that its renderer also holds: in the game one
// read took about 0.9 ms (measured 2026-11-12: 1450 reads in 1370 ms), and a
// live puppet needs some 25 reads and 4 writes a frame. Windows' own calls
// on the process's memory are slow there as well (ReadProcessMemory 0.12 ms,
// WriteProcessMemory 1.1 ms, VirtualQuery about 1 ms: the game changes page
// protections all the time), so they are kept to one VirtualQuery per run of
// pages a second (kRegionNs; the same runs, the actors' heap, are asked for
// every frame).
// Memory that query says is plainly there is copied directly: a guest address
// is an address in this process (README, "How the game's memory is reached"),
// this is the game's own thread between two frames, and a page the game
// protects after the query faults into the game's own handler exactly as it
// does under the game's code and the interface's own copy. Writes go direct
// only into committed read-write pages. Everything else (a page the game has
// not touched yet, one it holds read-only or without access for its own
// bookkeeping) goes through the interface as before.
//
// All of that is for a game with interface version 1. From version 2 on the
// interface's read and write are fast themselves (they keep the game's own
// regions with the mapping generation they were named at, so a range the game
// unmapped is not believed for a second, as a Region here is) and every read
// and write goes through them: no VirtualQuery, no cache of ours.
struct Region {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
    bool readable = false;
    bool writable = false;
    std::uint64_t asked_ns = 0;
};
constexpr std::uint64_t kRegionNs = 1'000'000'000;
std::atomic<std::uint64_t> g_queries{0};  // VirtualQuery calls
// The game's presenting thread only (every ApiGuest lives in its frame callback).
std::array<Region, 64> regions_{};

class ApiGuest final : public CoopPuppet::Guest {
public:
    explicit ApiGuest(const Nx2PluginApi* api) : api_(api) {}
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        if (size == 0) {
            return true;
        }
        const std::uint64_t begin = NowNs();
        bool ok = false;
        if (g_fast.load(std::memory_order_relaxed)) {
            // Version 2: the game's own copy, which knows its mapping (above).
            ok = api_->read(address, out, size) == 1;
        } else if (Plain(address, size, false)) {
            std::memcpy(out, reinterpret_cast<const void*>(address), size);
            ok = true;
        } else {
            g_slow.fetch_add(1, std::memory_order_relaxed);
            ok = api_->read(address, out, size) == 1;
        }
        g_read_ns.fetch_add(NowNs() - begin, std::memory_order_relaxed);
        g_reads.fetch_add(1, std::memory_order_relaxed);
        return ok;
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        if (size == 0) {
            return true;
        }
        const std::uint64_t begin = NowNs();
        bool ok = false;
        if (g_fast.load(std::memory_order_relaxed)) {
            ok = api_->write(address, in, size) == 1;
        } else if (Plain(address, size, true)) {
            std::memcpy(reinterpret_cast<void*>(address), in, size);
            ok = true;
        } else {
            g_slow.fetch_add(1, std::memory_order_relaxed);
            ok = api_->write(address, in, size) == 1;
        }
        g_write_ns.fetch_add(NowNs() - begin, std::memory_order_relaxed);
        g_writes.fetch_add(1, std::memory_order_relaxed);
        return ok;
    }

private:
    // Whether [address, address + size) lies in one run of committed pages
    // that can be read (or, for `write`, are read-write). A run found readable
    // is believed for kRegionNs; anything else is asked again each time.
    bool Plain(std::uint64_t address, std::size_t size, bool write) {
        if (address + size < address) {
            return false;
        }
        const std::uint64_t now = NowNs();
        for (const Region& r : regions_) {
            if (address >= r.begin && address + size <= r.end && now - r.asked_ns < kRegionNs &&
                (write ? r.writable : r.readable)) {
                return true;
            }
        }
        MEMORY_BASIC_INFORMATION info{};
        g_queries.fetch_add(1, std::memory_order_relaxed);
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) != sizeof(info)) {
            return false;
        }
        Region r;
        r.begin = reinterpret_cast<std::uint64_t>(info.BaseAddress);
        r.end = r.begin + info.RegionSize;
        const DWORD protect = info.Protect & 0xff;
        const bool plain = info.State == MEM_COMMIT && (info.Protect & PAGE_GUARD) == 0;
        r.writable = plain && protect == PAGE_READWRITE;
        r.readable = r.writable || (plain && protect == PAGE_READONLY);
        r.asked_ns = now;
        if (r.readable) {
            // In place of an older answer for the same run, else of the oldest.
            Region* slot = &regions_[0];
            for (Region& old : regions_) {
                if (old.begin == r.begin) {
                    slot = &old;
                    break;
                }
                if (old.asked_ns < slot->asked_ns) {
                    slot = &old;
                }
            }
            *slot = r;
        }
        return address + size <= r.end && (write ? r.writable : r.readable);
    }

    const Nx2PluginApi* api_;
};

// One call of the director, made as the emulator's cave makes it: x0 to x7
// and s0 in, x0 out. False when the game refused the address.
bool Make(const Nx2PluginApi* api, std::uint64_t main_base, const CoopPuppet::Call& call,
          std::uint64_t& result) {
    Nx2GuestRegs regs{};
    regs.size = sizeof(regs);
    std::uint64_t function = call.function;
    for (std::size_t i = 0; i < call.x.size(); ++i) {
        regs.x[i] = call.x[i];
    }
    if (function == main_base + CoopPuppet::addr::kCreate) {
        function = main_base + kCreateCaller;
        const std::uint64_t handle = call.x[3];
        regs.x[0] = handle - 8;
        regs.x[3] = handle + 8;
        regs.x[4] = regs.x[5] = regs.x[6] = regs.x[7] = 0;
    } else {
        for (const std::uint64_t word : call.stack) {
            if (word != 0) {
                return false;  // a stack argument: not possible here
            }
        }
    }
    std::uint32_t s0 = 0;
    std::memcpy(&s0, &call.s0, sizeof(s0));
    regs.v[0][0] = s0;
    const std::uint64_t begin = NowNs();
    const bool ran = api->guest_call_regs(function, &regs) == 1;
    g_call_ns.fetch_add(NowNs() - begin, std::memory_order_relaxed);
    if (!ran) {
        return false;
    }
    result = regs.x[0];
    return true;
}

// One frame's work on the game's thread: the director's step and its calls
// (as the emulator's cave makes them), then teleport.hpp's.
void Work() {
    const Nx2PluginApi* api = g_api.load();
    if (api == nullptr) {
        return;
    }
    g_frames.fetch_add(1, std::memory_order_relaxed);
    // How long the game's thread is kept, for the status line.
    struct Timer {
        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        ~Timer() {
            const auto us = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - begin)
                    .count());
            g_frame_us.fetch_add(us, std::memory_order_relaxed);
            if (us > g_worst_us.load(std::memory_order_relaxed)) {
                g_worst_us.store(us, std::memory_order_relaxed);
            }
        }
    } timer;
    const Nx2GameInfo* info = api->game_info();
    const std::uint64_t main_base = info != nullptr ? info->main_base : 0;
    ApiGuest guest(api);
    {
        std::lock_guard lock(g_work_mutex);
        if (g_work != nullptr) {
            g_work(guest, main_base, g_work_user);
        }
    }
    const std::uint64_t now = NowMs();
    if (g_puppets.load(std::memory_order_relaxed) && !g_broken.load(std::memory_order_relaxed)) {
        CoopPuppet::Director& director = CoopPuppet::TheDirector();
        auto call = director.Step(guest, false, 0, now);
        // The director stops itself at kMaxCallsPerFrame; this bound is a guard.
        for (int made = 0; call && made < 64; ++made) {
            std::uint64_t result = 0;
            if (!Make(api, main_base, *call, result)) {
                g_broken = true;
                Log("puppet: the game refused the call of main+%#llx (not a function of this "
                    "build, or it needs stack arguments); puppets are off for this run",
                    static_cast<unsigned long long>(call->function - main_base));
                break;
            }
            g_calls.fetch_add(1, std::memory_order_relaxed);
            call = director.Step(guest, true, result, now);
        }
    }
    if (!g_teleport_broken.load(std::memory_order_relaxed)) {
        auto call = Teleport::Step(guest, false, 0, main_base);
        for (int made = 0; call && made < 16; ++made) {
            std::uint64_t result = 0;
            if (!Make(api, main_base, *call, result)) {
                g_teleport_broken = true;
                Log("teleport: the game refused the call of main+%#llx; no teleport in this run",
                    static_cast<unsigned long long>(call->function - main_base));
                break;
            }
            g_calls.fetch_add(1, std::memory_order_relaxed);
            call = Teleport::Step(guest, true, result, main_base);
        }
    }
}

// Interface version 1: the frame callback, on the game's presenting thread
// between two frames.
void OnFrame(const Nx2Frame*, void*) {
    Work();
}

// Interface version 2: a hook of the game's frame step, on the thread that
// calls it from moduleCalc_, before it runs. That is where the emulator build
// does this work: the frame's actors have run and its jobs were waited for.
// The function then runs as the game called it.
void OnFrameStep(Nx2HookCall* call, void*) {
    const Nx2PluginApi* api = g_api.load();
    const Nx2GameInfo* info = api != nullptr ? api->game_info() : nullptr;
    if (info == nullptr) {
        return;
    }
    const std::uint64_t from = call->lr - info->main_base;
    if (from == kFrameStepReturn || from == kFrameStepReturnRare) {
        Work();
    }
}

}  // namespace

void Configure(const Options& options, void (*log)(const char* line)) {
    g_log = log;
    g_frame_hook = options.frame_hook;
    g_puppets = options.puppets;
    if (!options.puppets) {
        g_configured = true;  // the game-thread callback alone (teleport.hpp)
        return;
    }
    CoopPuppet::PlannerSettings settings;
    settings.debug = options.test_puppet;
    settings.debug_anim = options.debug_anim;
    CoopPuppet::Configure(std::move(settings), options.verbose, [](const std::string& line) {
        if (g_log != nullptr) {
            // The director's lines start with "coop: "; the DLL's log adds that itself.
            g_log(line.rfind("coop: ", 0) == 0 ? line.c_str() + 6 : line.c_str());
        }
    });
    // The pose copy writes each bone after the game's animation ran and before
    // the frame is drawn. The frame callback comes after the draw, so the next
    // frame's animation would write over it: the AS commands animate the
    // puppet instead (README, "Puppets").
    CoopPuppet::TheDirector().SetPoseCopy(options.pose_copy);
    CoopPose::SetLog(
        [](const std::string& line) {
            if (g_log != nullptr) {
                g_log(line.c_str());
            }
        },
        options.debug_pose, false);
    g_configured = true;
    Log("puppet: on%s%s (the other players as a second Link; needs the puppet pack in the "
        "mod's romfs folder)",
        options.test_puppet ? ", test puppet beside you" : "",
        options.pose_copy ? ", pose copy" : "");
}

bool Configured() { return g_configured; }

bool Register() {
    if (!g_configured) {
        return false;
    }
    std::lock_guard lock(g_reg);
    if (g_handle > 0) {
        return true;
    }
    const Nx2PluginApi* api = Overlay::Api();
    if (api == nullptr || !NX2_HAS(api, Nx2PluginApi, guest_call_regs)) {
        return false;
    }
    g_api = api;
    // Version 2's read and write need no page cache of ours (ApiGuest).
    g_fast = NX2_HAS(api, Nx2PluginApi, map_range) && api->map_range != nullptr;
    const Nx2GameInfo* info = api->game_info();
    if (g_frame_hook && info != nullptr && info->main_base != 0 &&
        NX2_HAS(api, Nx2PluginApi, hook_function) && api->hook_function != nullptr) {
        g_handle = api->hook_function(info->main_base + kFrameStep, OnFrameStep, nullptr);
        if (g_handle > 0) {
            g_hooked = true;
            g_active = true;
            Log("game thread: hooked the game's frame step main+%#llx (interface version %u): "
                "game calls and writes run where the emulator build makes them",
                static_cast<unsigned long long>(kFrameStep), api->version);
            return true;
        }
        Log("game thread: the frame step hook was refused; using the frame callback");
    }
    g_handle = api->on_frame(OnFrame, nullptr);
    if (g_handle <= 0) {
        g_handle = 0;
        return false;
    }
    g_active = true;
    Log("game thread: frame callback registered (game calls run on the game's presenting "
        "thread, between two frames)%s",
        g_frame_hook ? "" : "; the frame step hook is off (OOT_COOP_FRAME_HOOK=0)");
    return true;
}

bool Active() { return g_active; }

void SetFrameWork(FrameWork work, void* user) {
    std::lock_guard lock(g_work_mutex);
    g_work = work;
    g_work_user = user;
}

std::uint64_t Frames() { return g_frames.load(std::memory_order_relaxed); }

void Publish(std::uint64_t main_base, bool in_game, const std::optional<std::string>& scene,
             const std::optional<std::array<float, 3>>& position, bool local_event,
             coop::host::Session* session, std::uint64_t server_now_us) {
    if (!g_configured || !g_active || !g_puppets) {
        return;
    }
    // The director creates a puppet only while the game draws (a puppet made
    // during a load is never shown). The emulator build knows that from its
    // renderer's camera; here "the player is readable" stands for it.
    if (in_game) {
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        CoopCamera::Publish(g_drawn_frame.fetch_add(1) + 1, identity, 1.0f, 1.0f, false);
    }
    CoopPuppet::Publish(main_base, scene, position, local_event, session, server_now_us);
}

void OnEvent(const coop::net::Event& event) {
    if (!g_configured || !g_puppets) {
        return;
    }
    CoopPuppet::OnEvent(event);
    CoopPose::OnEvent(event, NowMs());
}

std::string StatusText() {
    if (!g_configured) {
        return {};
    }
    char buffer[320];
    const std::uint64_t frames = g_frames.load();
    std::snprintf(buffer, sizeof(buffer),
                  "; game thread: %llu frames (%.3f ms each, the longest %.3f ms), %llu game "
                  "calls (%.1f ms), %llu reads (%.1f ms), %llu writes (%.1f ms), %llu page queries, "
                  "%llu through the interface",
                  static_cast<unsigned long long>(frames),
                  frames != 0 ? static_cast<double>(g_frame_us.load()) / 1000.0 /
                                    static_cast<double>(frames)
                              : 0.0,
                  static_cast<double>(g_worst_us.load()) / 1000.0,
                  static_cast<unsigned long long>(g_calls.load()),
                  static_cast<double>(g_call_ns.load()) / 1e6,
                  static_cast<unsigned long long>(g_reads.load()),
                  static_cast<double>(g_read_ns.load()) / 1e6,
                  static_cast<unsigned long long>(g_writes.load()),
                  static_cast<double>(g_write_ns.load()) / 1e6,
                  static_cast<unsigned long long>(g_queries.load()),
                  static_cast<unsigned long long>(g_slow.load()));
    std::string text = buffer;
    text += g_hooked ? " (frame step hook)" : " (frame callback)";
    if (!g_active) {
        text += " (no frame callback)";
    }
    if (g_broken) {
        text += " (puppets off: a call was refused)";
    }
    return text;
}

void Shutdown() {
    int handle = 0;
    {
        std::lock_guard lock(g_reg);
        handle = std::exchange(g_handle, 0);
        g_active = false;
    }
    // Outside the lock: the callback being taken out may be in the session's
    // frame, which asks Register() for it.
    const Nx2PluginApi* api = g_api.load();
    if (handle > 0 && api != nullptr) {
        api->remove(handle);
    }
}

}  // namespace CoopNx2navi::Puppet
