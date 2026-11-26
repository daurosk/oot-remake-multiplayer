// SPDX-License-Identifier: GPL-3.0-or-later
// coop_null_page.h. The Windows part follows Hookshot patch 0002's host/coop_null_page.c (ours, same project).
#include "coop_null_page.h"

#include <atomic>
#include <mutex>

#include "common/logging.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace CoopNullPage {

#ifdef _WIN32
namespace {

void* const kBase = reinterpret_cast<void*>(static_cast<std::uintptr_t>(kGuestBase));
constexpr long kSaid = 32;  // the first reads named in the log, then a count a minute
std::atomic<long> g_hits{0};
std::atomic<long> g_minute{0};

LONG CALLBACK OnGuard(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;
    if (rec->ExceptionCode != STATUS_GUARD_PAGE_VIOLATION || rec->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto at = GuestOffset(static_cast<std::uint64_t>(rec->ExceptionInformation[1]));
    if (!at) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_minute.fetch_add(1);
    if (g_hits.fetch_add(1) < kSaid) {
        LOG_INFO(Core, "coop: null read: guest {:#x} from longshot.exe+{:#x} ({}; reads 0, as on the emulator)", *at,
                 reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress) -
                     reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)),
                 rec->ExceptionInformation[0] != 0 ? "a write: it faults next" : "a read");
    }
    return EXCEPTION_CONTINUE_EXECUTION;  // (the guard bit is off for that page now: the read runs again)
}

DWORD WINAPI Rearm(LPVOID) {
    for (;;) {
        Sleep(60000);
        if (const long n = g_minute.exchange(0); n != 0) {
            LOG_INFO(Core, "coop: null reads in the last minute: {} page(s) first touched ({} since the first puppet)",
                     n, g_hits.load());
        }
        DWORD was = 0;
        VirtualProtect(kBase, kSize, PAGE_READONLY | PAGE_GUARD, &was);
    }
}

}  // namespace

Mb Query() {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(kBase, &mbi, sizeof mbi) == 0 || mbi.BaseAddress != kBase) {
        return Mb::Other;
    }
    return mbi.State == MEM_COMMIT ? Mb::Committed : mbi.State == MEM_RESERVE ? Mb::Reserved : Mb::Other;
}

bool Arm(const std::string& null_reads_env) {
    static std::mutex m;
    static std::optional<bool> done;
    std::lock_guard lock(m);
    if (done) {
        return *done;
    }
    const Mb mb = Query();
    done = Armable(null_reads_env, mb);
    if (null_reads_env == "0") {
        LOG_INFO(Core, "coop: null-pointer reads fault (HOS_COOP_NULL_READS=0)");
    } else if (mb == Mb::Committed) {
        LOG_INFO(Core, "coop: the guest's first MB is mapped already: null-pointer reads give 0");
    } else if (mb == Mb::Other) {
        LOG_WARNING(Core, "coop: the guest's first MB is not where it should be: null-pointer reads still fault");
    } else {
        AddVectoredExceptionHandler(1, OnGuard);
        if (VirtualAlloc(kBase, kSize, MEM_COMMIT, PAGE_READONLY | PAGE_GUARD) == nullptr) {
            LOG_WARNING(Core, "coop: the guest's first MB could not be mapped (error {}): null-pointer reads still fault",
                        GetLastError());
            done = false;
        } else {
            CloseHandle(CreateThread(nullptr, 0, Rearm, nullptr, 0, nullptr));
            LOG_INFO(Core, "coop: the guest's first MB reads as zeros from now on (null-pointer reads give 0, as on "
                           "the emulator; counted in this log; HOS_COOP_NULL_READS=0: off)");
        }
    }
    return *done;
}
#else
Mb Query() {
    return Mb::Committed;  // (Linux: Hookshot maps the guest window readable whole)
}

bool Arm(const std::string&) {
    return true;
}
#endif

}  // namespace CoopNullPage
