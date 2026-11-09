// SPDX-License-Identifier: GPL-3.0-or-later
//
// The game's clock (build e0f3ccfaf50065ae, docs/time.md):
//   WorldMgr = [main + 0x4e7eab0] (the GOT slot 0x4c7e898 the AI node
//              ExecuteWmSetTime loads)
//   T        = [WorldMgr + 0x510], the time object
//   now      = the time record at T + 0x20, or T + 0x50 while bit 9 of
//              T + 8 is set (the getter 0x3452280 copies its +8 and +0x10)
//   set      = 0x34522d0(WorldMgr, seconds since midnight): ExecuteWmSetTime
//              calls it with hour * 3600 + minute * 60; it hands the time to
//              0xeee640(T, seconds, 1, -1)

#include "coop_time.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "common/logging.h"
#include "core/core.h"
#include "core/memory.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop_puppet.h"
#include "coop_svc.h"
#include "coop_time_sync.h"

namespace CoopTime {
namespace {

constexpr std::uint64_t kWorldMgrPtr = 0x4e7eab0;
constexpr std::uint64_t kTimeObject = 0x510;
constexpr std::uint64_t kTimeFlags = 0x8;
constexpr std::uint32_t kTimeOverride = 0x200;
constexpr std::uint64_t kTimeNow = 0x20;
constexpr std::uint64_t kTimeNowOverride = 0x50;
constexpr std::uint64_t kTimeAsked = 0x48;
constexpr std::uint64_t kSetTime = 0x34522d0;
constexpr int kCallerOrder = 40;  // after the puppets, spawn (10), fairy (20), objects (30)
constexpr unsigned kEveryFrames = 15;
constexpr std::uint64_t kDebugEveryUs = 2'000'000;

std::string Env(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

// The calc thread side: sets the clock when asked.
class Setter {
public:
    void Want(std::uint64_t main, std::uint32_t seconds) {
        std::lock_guard lock(mutex_);
        main_ = main;
        pending_ = seconds;
    }

    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call) {
        std::lock_guard lock(mutex_);
        if (after_call || !pending_ || main_ == 0) {
            return std::nullopt;
        }
        const std::uint32_t seconds = *pending_;
        pending_.reset();
        std::uint64_t world = 0;
        std::uint64_t time = 0;
        if (!guest.Read(main_ + kWorldMgrPtr, &world, 8) || world == 0 ||
            !guest.Read(world + kTimeObject, &time, 8) || time == 0) {
            return std::nullopt;  // no world yet: the next Tick asks again
        }
        CoopPuppet::Call c;
        c.function = main_ + kSetTime;
        c.x[0] = world;
        c.x[1] = seconds;
        LOG_INFO(Core, "coop: time: set to {:02}:{:02}:{:02}", seconds / 3600, seconds / 60 % 60,
                 seconds % 60);
        return c;
    }

private:
    std::mutex mutex_;
    std::uint64_t main_ = 0;
    std::optional<std::uint32_t> pending_;
};

template <typename T>
std::optional<T> Get(Core::Memory::Memory& memory, std::uint64_t address) {
    if (address < 0x1000 || !memory.IsValidVirtualAddressRange(address, sizeof(T))) {
        return std::nullopt;
    }
    T value{};
    memory.ReadBlockUnsafe(address, &value, sizeof(T));
    return value;
}

struct Glue {
    bool on = false;
    bool debug = false;
    unsigned frame = 0;
    std::uint64_t debug_at = 0;
    bool connected = false;
    // LICHII_COOP_TIME_TEST_SET=<seconds> (tests only): 5 s after joining,
    // this game sets its own clock there, as the Sun's Song would.
    std::optional<std::uint32_t> test_set;
    std::uint64_t joined_at = 0;
    Sync sync;
    Setter setter;

    Glue() {
        on = Env("LICHII_COOP_TIME") != "0";
        const auto d = Env("LICHII_COOP_DEBUG_TIME");
        debug = !d.empty() && d != "0";
        if (const auto t = Env("LICHII_COOP_TIME_TEST_SET"); !t.empty()) {
            const unsigned long v = std::strtoul(t.c_str(), nullptr, 10);
            if (v < coop::protocol::kSecondsPerDay) {
                test_set = static_cast<std::uint32_t>(v);
            }
        }
        if (on) {
            CoopSvc::AddCaller(kCallerOrder, [this](CoopPuppet::Guest& guest, bool after_call,
                                                    std::uint64_t, std::uint64_t) {
                return setter.Step(guest, after_call);
            });
        }
        LOG_INFO(Core, "coop: time {}", on ? "shared" : "off (LICHII_COOP_TIME=0)");
    }

    // Seconds since midnight, or nullopt.
    std::optional<std::uint32_t> Read(Core::System& system, std::uint64_t main,
                                      std::uint64_t now_us) {
        auto& memory = system.ApplicationMemory();
        const auto world = Get<std::uint64_t>(memory, main + kWorldMgrPtr);
        const auto time =
            world && *world != 0 ? Get<std::uint64_t>(memory, *world + kTimeObject) : std::nullopt;
        if (!time || *time == 0) {
            return std::nullopt;
        }
        const std::uint32_t flags = Get<std::uint32_t>(memory, *time + kTimeFlags).value_or(0);
        const std::uint64_t record =
            *time + ((flags & kTimeOverride) != 0 ? kTimeNowOverride : kTimeNow);
        const auto raw = Get<std::array<std::uint32_t, 6>>(memory, record);
        const auto asked = Get<std::array<std::uint32_t, 2>>(memory, *time + kTimeAsked);
        if (!raw) {
            return std::nullopt;
        }
        if (debug && now_us - debug_at >= kDebugEveryUs) {
            debug_at = now_us;
            LOG_INFO(Core,
                     "coop: time: raw flags {:#x} record {:08x} {:08x} {:08x} {:08x} {:08x} "
                     "{:08x}, asked {:08x} {:08x}",
                     flags, (*raw)[0], (*raw)[1], (*raw)[2], (*raw)[3], (*raw)[4], (*raw)[5],
                     asked ? (*asked)[0] : 0u, asked ? (*asked)[1] : 0u);
        }
        // The record's u32 at +0x10 (the getter copies +8 and +0x10).
        const std::uint32_t seconds = (*raw)[4];
        if (seconds >= coop::protocol::kSecondsPerDay) {
            return std::nullopt;
        }
        return seconds;
    }

    void Frame(Core::System& system, std::uint64_t main, coop::host::Session* session,
               std::uint64_t now_us) {
        if (!on || ++frame % kEveryFrames != 0) {
            return;
        }
        if (main == 0 || now_us == 0 || session == nullptr) {
            if (connected) {
                sync.Reset();
                connected = false;
            }
            if (main != 0 && debug) {
                (void)Read(system, main, now_us);
            }
            return;
        }
        const auto now = Read(system, main, now_us);
        if (!connected) {
            joined_at = now_us;
        }
        connected = true;
        if (test_set && now && now_us - joined_at >= 5'000'000) {
            LOG_INFO(Core, "coop: time: test set to {}", *test_set);
            setter.Want(main, *test_set);
            test_set.reset();
        }
        const bool host = session->with_client([](coop::net::Client& c) { return c.is_host(); });
        std::vector<coop::protocol::Message> send;
        if (const auto target = sync.Tick(now, host, now_us, send)) {
            setter.Want(main, *target);
        }
        if (send.empty()) {
            return;
        }
        session->with_client([&](coop::net::Client& c) {
            for (const auto& m : send) {
                const auto& t = std::get<coop::protocol::TimeOfDay>(m);
                const bool jump = (t.flags & coop::protocol::time_of_day::jump) != 0;
                const bool sent = c.send(m);
                if (jump || debug) {
                    LOG_INFO(Core, "coop: time: sent {:02}:{:02}{}{}", t.seconds / 3600,
                             t.seconds / 60 % 60, jump ? " (jump)" : "", sent ? "" : " (not sent)");
                }
            }
        });
    }
};

Glue& TheGlue() {
    static Glue glue;  // kept for the whole run: the cave holds its setter
    return glue;
}

} // namespace

void Frame(Core::System& system, std::uint64_t main_base, coop::host::Session* session,
           std::uint64_t server_now_us) {
    TheGlue().Frame(system, main_base, session, server_now_us);
}

void OnEvent(const coop::net::Event& event) {
    if (event.kind == coop::net::EventKind::message) {
        TheGlue().sync.Receive(event.message);
    }
}

} // namespace CoopTime
