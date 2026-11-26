// SPDX-License-Identifier: GPL-3.0-or-later
// Row 83 / #245: a game whose window is behind another of our games asks Windows for one step more (coop_back_window.h).
#include "coop_back_window.h"

#include "common/logging.h"

#ifdef _WIN32
#include <chrono>
#include <cstdlib>
#include <string>

#include <windows.h>
#include <tlhelp32.h>

#include "coop_off_frame.h"
#include "coop_window_brand.h"
#endif

namespace CoopBackWindow {

#ifdef _WIN32
namespace {

// d3dkmthk.h D3DKMT_SCHEDULINGPRIORITYCLASS: _NORMAL 2, _ABOVE_NORMAL 3 (gdi32.dll exports, looked up at run time).
constexpr int kGpuNormal = 2;
constexpr int kGpuAboveNormal = 3;
using SetGpuClass = LONG(WINAPI*)(HANDLE, int);

struct FoundWindow {
    HWND hwnd = nullptr;
};

BOOL CALLBACK EachWindow(HWND h, LPARAM l) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    char cls[64] = {};
    GetClassNameA(h, cls, sizeof cls);
    if (!CoopWindowBrand::IsGameWindowClass(cls)) return TRUE;
    reinterpret_cast<FoundWindow*>(l)->hwnd = h;
    return FALSE;
}

// This process's exe name (the launcher's games 2-N and Nucleus's copies keep it: local-coop.ps1 Start-KitLocalCoop
// starts <instance>\<Split-Path -Leaf $Exe>).
const std::wstring& OwnExe() {
    static const std::wstring name = [] {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring p(path, n);
        const auto slash = p.find_last_of(L"\\/");
        return slash == std::wstring::npos ? p : p.substr(slash + 1);
    }();
    return name;
}

bool OtherGameRuns() {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{};
    e.dwSize = sizeof e;
    bool found = false;
    const DWORD self = GetCurrentProcessId();
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e)) {
        found = e.th32ProcessID != self && _wcsicmp(e.szExeFile, OwnExe().c_str()) == 0;
    }
    CloseHandle(snap);
    return found;
}

Want g_have;           // what is applied (both normal at the start: nothing of ours set yet)
bool g_said_mode = false;

void Pass(Mode mode) {
    State s;
    s.other_game = OtherGameRuns();
    FoundWindow f;
    EnumWindows(EachWindow, reinterpret_cast<LPARAM>(&f));
    s.has_window = f.hwnd != nullptr;
    s.in_front = s.has_window && GetForegroundWindow() == f.hwnd;
    s.iconic = s.has_window && IsIconic(f.hwnd);
    const Want want = Decide(mode, s);
    if (want == g_have) return;
    long gpu_status = 0;
    if (want.gpu_up != g_have.gpu_up) {
        static const auto set = reinterpret_cast<SetGpuClass>(
            reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass")));
        gpu_status = set ? set(GetCurrentProcess(), want.gpu_up ? kGpuAboveNormal : kGpuNormal) : -1;
    }
    BOOL cpu_ok = TRUE;
    if (want.cpu_up != g_have.cpu_up) {
        cpu_ok = SetPriorityClass(GetCurrentProcess(), want.cpu_up ? ABOVE_NORMAL_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    }
    LOG_INFO(Core,
             "coop: back window: {} (another game on this PC {}, window {}, in front {}, minimized {}): GPU class {} "
             "(status {:#x}), process class {}{}",
             want.gpu_up || want.cpu_up ? "behind another game of ours: one step up" : "back to normal", s.other_game,
             s.has_window, s.in_front, s.iconic, want.gpu_up ? "above normal" : "normal",
             static_cast<unsigned long>(gpu_status), want.cpu_up ? "above normal" : "normal", cpu_ok ? "" : " (refused)");
    g_have = want;
}

}  // namespace

void Tick() {
    static const Mode mode = ModeFrom(std::getenv("LICHII_COOP_BACK_BOOST"));
    if (!g_said_mode) {
        g_said_mode = true;
        LOG_INFO(Core, "coop: back window: mode {} (LICHII_COOP_BACK_BOOST = off | gpu | cpu | both)",
                 mode == Mode::Off ? "off" : mode == Mode::Gpu ? "gpu" : mode == Mode::Cpu ? "cpu" : "both");
    }
    if (mode == Mode::Off) return;
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next{};
    static CoopOffFrame::Slot pass;
    const Clock::time_point now = Clock::now();
    if (now < next) return;
    next = now + std::chrono::seconds(1);
    pass.Start([] { Pass(mode); });
}
#else
void Tick() {}
#endif

}  // namespace CoopBackWindow
