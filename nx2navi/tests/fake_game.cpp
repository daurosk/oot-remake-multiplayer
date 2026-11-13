// SPDX-License-Identifier: GPL-3.0-or-later
// A stand-in for the NX2navi game process, to test coop_nx2navi.dll without
// the game (nx2navi/tests/test_dll.py runs it).
//
// It loads the DLL the way the launcher's injected thread does (LoadLibraryW)
// and, unless told not to, first maps a made-up "main" where NX2navi maps the
// real one: a module header, a build id note, and the few objects coop/host
// follows to the player (the layout of tests/host/fake_memory.hpp, in real
// memory), with a player walking in a circle. Nothing in it comes from the
// game: the numbers are the offsets of coop/host/src/addresses.hpp and the
// rest is invented.
//
//   fake_game <dll> [--seconds N] [--no-game] [--other-build] [--name-scene S]
//
// Exit code 0: the DLL loaded, ran, stopped when asked and unloaded.
//
// Built with COOP_FAKE_PLUGIN_HOST (coop_nx2navi_fake_host.exe) it is also a
// game with NX2navi's plugin interface (dll/third_party/plugin_api.h): the
// executable exports nx2navi_plugin_api, calls the DLL's nx2navi_plugin_init
// after loading it (unless --no-init: then the DLL has to ask the export), and
// drives the callbacks on threads of its own as the game would: overlay
// callbacks on a "present" thread 60 times a second, recording every draw
// call; key and text callbacks on a "window" thread from a script:
//   --key T NAME     at T seconds, NAME (enter, esc, backspace, f9, or a code)
//                    goes down and up
//   --type T TEXT    at T seconds, TEXT typed, a character at a time, while the
//                    input is captured (as the game sends typed text)
//   --snapshot T     the picture drawn at T seconds goes into --draw-out
//   --draw-out PATH  the snapshots, the last picture and every distinct text
//                    drawn
//   --twice          the DLL loaded twice and its nx2navi_plugin_init called
//                    twice
//   --save-root DIR  the game's saves are in DIR\save\<account> (as NX2NAVI_SAVE_DIR),
//                    not in the profile's (give it after --profile)
//   --v3             the interface is version 3 (the whole table): a title menu
//                    entry, the game-style menu page, the game's buttons, the
//                    exit callback, the game's folders. Function hooks are
//                    refused (this stand-in has no game function to hook).
//   --v5             version 5: --v4, and the camera reset (said when asked) and the
//                    game's language (French) in the game's information.
//   --v4             version 4: also world_to_screen (a made-up camera: a world
//                    point (x, y, z) shows at (960 + 10 x, 540 - 10 y), and is
//                    behind the camera when z is below -100), rows enabled again
//                    by an update, and the cursor's row
//   --game-dir D, --profile D   the folders Nx2GameInfo names (version 3); the
//                    save folder is D\save\0123456789abcdef0123456789abcdef
//   --title T        at T seconds the player chooses the title menu entry
//   --menu T ROW     at T seconds the player chooses row ROW of the open page
//   --menu-change T ROW VALUE   a toggle or select row of the page changes
//   --menu-text T ROW TEXT      the game's own typing of a text row ended with TEXT (version 4)
//   --pad-key T BUTTON          a button pressed by a key bound to it (Nx2PadEvent.keyboard)
//   --pad T NAME     a button of the game's controls (a, b, up, down, left,
//                    right) goes down and up
//   --menu-back T    the player leaves the page (B)
//   --exit           at the end the game "closes its window": the exit
//                    callbacks run before the DLL is stopped
// Every page the DLL opens or updates is printed ("fake_host: menu open: ...").

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "../../coop/host/src/addresses.hpp"
#include "coop_game_build.h"
#include "guest_memory.hpp"

#ifdef COOP_FAKE_PLUGIN_HOST
#include <atomic>
#include <cstdarg>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "third_party/plugin_api.h"
#endif

namespace addr = coop::host::addr;

#ifdef COOP_FAKE_PLUGIN_HOST
// --- The plugin interface, as a game would serve it ---
namespace host {

enum Kind { kFrame, kOverlay, kKey, kText, kPad, kExit, kTitle, kKinds };
struct Slot {
    int handle = 0;
    void* fn = nullptr;
    void* user = nullptr;
};
std::mutex g_table;                 // the slots
std::recursive_mutex g_calls[kKinds];  // held while a kind is called; remove waits for it
std::vector<Slot> g_slots[kKinds];
int g_next = 1;
std::atomic<int> g_capture{0};
std::atomic<int> g_capture_calls{0};
std::atomic<std::uint64_t> g_frames{0};

int Add(Kind kind, void* fn, void* user) {
    if (fn == nullptr) {
        return 0;
    }
    std::lock_guard lock(g_table);
    g_slots[kind].push_back({g_next, fn, user});
    return g_next++;
}
int Count(Kind kind) {
    std::lock_guard lock(g_table);
    return static_cast<int>(g_slots[kind].size());
}
std::vector<Slot> Take(Kind kind) {
    std::lock_guard lock(g_table);
    return g_slots[kind];
}

std::string g_game_dir = ".";
std::string g_language;  // --v5: Nx2GameInfo.language
std::string g_profile_dir;
std::string g_save_dir;
const Nx2GameInfo* GameInfo() {
    static Nx2GameInfo info = {sizeof(Nx2GameInfo), 111, "fake", g_game_dir.c_str(), ".", "",
                               CoopNx2navi::kMainBase, 0x5000000, g_profile_dir.c_str(),
                               g_save_dir.c_str()};
    // Version 5 (--v5): the language the game runs in; null in an older game.
    info.language = g_language.empty() ? nullptr : g_language.c_str();
    return &info;
}
void LogLine(const char* format, ...) {
    char line[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::printf("fake_host: game log: %s\n", line);
    std::fflush(stdout);
}
int Read(std::uint64_t a, void* buf, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a), buf, n, &got) &&
                   got == n
               ? 1
               : 0;
}
// Only into committed read-write memory, as the game refuses its code and constants.
int Write(std::uint64_t a, const void* buf, size_t n) {
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(a), &info, sizeof(info)) != sizeof(info) ||
        info.State != MEM_COMMIT || info.Protect != PAGE_READWRITE ||
        a + n > reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize) {
        return 0;
    }
    std::memcpy(reinterpret_cast<void*>(a), buf, n);
    return 1;
}
int OnFrame(Nx2FrameFn fn, void* user) { return Add(kFrame, reinterpret_cast<void*>(fn), user); }
int OnOverlay(Nx2OverlayFn fn, void* user) { return Add(kOverlay, reinterpret_cast<void*>(fn), user); }
int OnKey(Nx2KeyFn fn, void* user) { return Add(kKey, reinterpret_cast<void*>(fn), user); }
int OnText(Nx2TextFn fn, void* user) { return Add(kText, reinterpret_cast<void*>(fn), user); }
void Remove(int handle) {
    for (int kind = 0; kind < kKinds; ++kind) {
        std::lock_guard calls(g_calls[kind]);
        std::lock_guard lock(g_table);
        auto& v = g_slots[kind];
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Slot& s) { return s.handle == handle; }),
                v.end());
    }
}
std::uint64_t GuestCall(std::uint64_t, const std::uint64_t*, int) { return 0; }
// No game function is here to call: the call is refused, as the game refuses an
// address that is no function of its own, and the first few are said.
int GuestCallRegs(std::uint64_t address, Nx2GuestRegs* regs) {
    // The one function this stand-in has: an actor's set-matrix (teleport.hpp
    // calls it after writing Link's position). Said with the position it finds.
    if (address == CoopNx2navi::kMainBase + 0x4c6580 && regs != nullptr) {
        float at[3] = {};
        Read(regs->x[0] + 0x31c, at, sizeof(at));
        std::printf("fake_host: guest call of main+0x4c6580 (set matrix) with Link at %.1f %.1f %.1f\n",
                    static_cast<double>(at[0]), static_cast<double>(at[1]), static_cast<double>(at[2]));
        std::fflush(stdout);
        regs->x[0] = 0;
        return 1;
    }
    static std::atomic<int> said{0};
    if (said.fetch_add(1) < 4) {
        std::printf("fake_host: guest call of main+%#llx refused\n",
                    static_cast<unsigned long long>(address - CoopNx2navi::kMainBase));
        std::fflush(stdout);
    }
    return 0;
}
void Capture(int on) {
    ++g_capture_calls;
    if (on) {
        ++g_capture;
    } else if (g_capture > 0) {
        --g_capture;
    }
}

// Interface version 1: the table ends before version 2's members (function
// hooks, map_range, ...), as an older game's does, so the DLL takes its
// version 1 ways here (the frame callback, its own page cache).
const Nx2PluginApi g_api = {
    static_cast<std::uint32_t>(offsetof(Nx2PluginApi, hook_function)), 1, GameInfo, LogLine, Read,
    Write, OnFrame, OnOverlay, OnKey, OnText, Remove, GuestCall, GuestCallRegs, Capture,
};

// --- Interface version 3: the title menu entry and the game-style page ---

int HookFunction(std::uint64_t address, Nx2HookFn, void*) {
    std::printf("fake_host: hook of main+%#llx refused (no game function here)\n",
                static_cast<unsigned long long>(address - CoopNx2navi::kMainBase));
    std::fflush(stdout);
    return 0;
}
std::uint64_t CallOriginal(Nx2HookCall*) { return 0; }
void* MapRange(std::uint64_t, size_t, int) { return nullptr; }
int OnExit(Nx2ExitFn fn, void* user) { return Add(kExit, reinterpret_cast<void*>(fn), user); }
int OnPad(Nx2PadFn fn, void* user) { return Add(kPad, reinterpret_cast<void*>(fn), user); }

struct TitleButton {
    int handle = 0;
    std::string label;
    Nx2TitleFn on_select = nullptr;
    void* user = nullptr;
};
std::mutex g_menu;  // the title buttons and the page
std::vector<TitleButton> g_titles;
thread_local bool t_selecting = false;
struct PageRow {
    std::uint32_t kind = 0;
    std::uint32_t choices = 0;
};
bool g_page_open = false;
std::vector<PageRow> g_page_rows;
Nx2MenuFn g_page_event = nullptr;
void* g_page_user = nullptr;
std::atomic<bool> g_page_close{false};

int AddTitleButton(const Nx2TitleButton* button) {
    if (button == nullptr || button->label == nullptr || button->on_select == nullptr) {
        return 0;
    }
    const int handle = Add(kTitle, reinterpret_cast<void*>(button->on_select), button->user);
    std::lock_guard lock(g_menu);
    g_titles.push_back({handle, button->label, button->on_select, button->user});
    std::printf("fake_host: title menu entry '%s'\n", button->label);
    std::fflush(stdout);
    return handle;
}

const char* KindName(std::uint32_t kind) {
    switch (kind) {
    case NX2_ROW_ACTION: return "action";
    case NX2_ROW_TOGGLE: return "toggle";
    case NX2_ROW_SELECT: return "select";
    case NX2_ROW_TEXT: return "text";
    default: return "separator";
    }
}

// The page as one line: the title, then each row "kind 'label' = 'value'".
void PrintPage(const char* what, const Nx2Menu* menu) {
    std::string line = std::string("fake_host: menu ") + what + ": " + (menu->title ? menu->title : "");
    for (std::uint32_t i = 0; i < menu->row_count; ++i) {
        const auto* row = reinterpret_cast<const Nx2MenuRow*>(
            reinterpret_cast<const char*>(menu->rows) + static_cast<std::size_t>(i) * menu->rows->size);
        line += " || " + std::to_string(i) + " " + KindName(row->kind);
        if (row->kind == NX2_ROW_SEPARATOR) {
            continue;
        }
        line += std::string(" '") + (row->label ? row->label : "") + "'";
        if (row->kind == NX2_ROW_SELECT) {
            line += std::string(" = '") +
                    (row->choices && row->current >= 0 &&
                             static_cast<std::uint32_t>(row->current) < row->choice_count
                         ? row->choices[row->current]
                         : "?") +
                    "' of " + std::to_string(row->choice_count);
        } else {
            line += std::string(" = '") + (row->value ? row->value : "") + "'";
            if (row->kind == NX2_ROW_TEXT && row->text_max != 0) {
                line += " max " + std::to_string(row->text_max);
            }
        }
        if (row->disabled) {
            line += " (disabled)";
        }
        // A caption's line break as the two characters \n: one page stays one line here.
        std::string caption = row->description ? row->description : "";
        for (std::size_t at = 0; (at = caption.find('\n', at)) != std::string::npos; at += 2) {
            caption.replace(at, 1, "\\n");
        }
        line += " [" + caption + "]";
    }
    if (menu->size >= offsetof(Nx2Menu, focus_row) + sizeof(menu->focus_row) && menu->focus_row != 0) {
        line += " || cursor to row " + std::to_string(menu->focus_row - 1);
    }
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

std::vector<PageRow> RowsOf(const Nx2Menu* menu) {
    std::vector<PageRow> rows;
    for (std::uint32_t i = 0; i < menu->row_count; ++i) {
        const auto* row = reinterpret_cast<const Nx2MenuRow*>(
            reinterpret_cast<const char*>(menu->rows) + static_cast<std::size_t>(i) * menu->rows->size);
        // A text row is a button for the game, as in the runtime.
        rows.push_back({row->kind == NX2_ROW_TEXT ? static_cast<std::uint32_t>(NX2_ROW_ACTION) : row->kind,
                        row->kind == NX2_ROW_SELECT ? row->choice_count : 0});
    }
    return rows;
}

bool SameRows(const std::vector<PageRow>& a, const std::vector<PageRow>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i].kind != b[i].kind || a[i].choices != b[i].choices) {
            return false;
        }
    }
    return true;
}

// As the runtime: only inside a title menu button's on_select, one page at a
// time, 1 to 48 rows with at least one button.
int OpenMenu(const Nx2Menu* menu) {
    std::lock_guard lock(g_menu);
    if (!t_selecting || g_page_open || menu == nullptr || menu->rows == nullptr ||
        menu->row_count == 0 || menu->row_count > 48) {
        std::printf("fake_host: menu refused\n");
        std::fflush(stdout);
        return 0;
    }
    g_page_rows = RowsOf(menu);
    g_page_event = menu->on_event;
    g_page_user = menu->user;
    g_page_open = true;
    g_page_close = false;
    PrintPage("open", menu);
    return 1;
}

int UpdateMenu(const Nx2Menu* menu) {
    std::lock_guard lock(g_menu);
    if (!g_page_open || menu == nullptr || !SameRows(g_page_rows, RowsOf(menu))) {
        std::printf("fake_host: menu update refused (not the same rows)\n");
        std::fflush(stdout);
        return 0;
    }
    PrintPage("update", menu);
    return 1;
}

void CloseMenu() { g_page_close = true; }

void MenuEvent(std::uint32_t kind, std::uint32_t row, std::int32_t value, const char* text = nullptr) {
    Nx2MenuFn fn = nullptr;
    void* user = nullptr;
    {
        std::lock_guard lock(g_menu);
        if (!g_page_open) {
            std::printf("fake_host: no page is open for that\n");
            std::fflush(stdout);
            return;
        }
        if (kind == NX2_MENU_CLOSED) {
            g_page_open = false;
            std::printf("fake_host: menu closed\n");
            std::fflush(stdout);
        }
        fn = g_page_event;
        user = g_page_user;
    }
    if (fn != nullptr) {
        Nx2MenuEvent e{};
        e.size = sizeof(Nx2MenuEvent);
        e.kind = kind;
        e.row = row;
        e.value = value;
        e.text = text;
        fn(&e, user);
    }
}

void ChooseTitle() {
    TitleButton chosen;
    {
        std::lock_guard lock(g_menu);
        std::lock_guard table(g_table);
        for (const TitleButton& b : g_titles) {
            for (const Slot& slot : g_slots[kTitle]) {
                if (slot.handle == b.handle) {
                    chosen = b;  // the last one still registered
                }
            }
        }
    }
    if (chosen.on_select == nullptr) {
        std::printf("fake_host: no title menu entry to choose\n");
        std::fflush(stdout);
        return;
    }
    std::printf("fake_host: the player chooses '%s'\n", chosen.label.c_str());
    std::fflush(stdout);
    t_selecting = true;
    chosen.on_select(chosen.user);
    t_selecting = false;
}

void Pad(std::uint32_t button, bool keyboard = false) {
    for (const int down : {1, 0}) {
        Nx2PadEvent e{};
        e.size = sizeof(e);
        e.button = button;
        e.down = static_cast<std::uint8_t>(down);
        e.keyboard = keyboard ? 1 : 0;
        std::lock_guard calls(g_calls[kPad]);
        for (const Slot& s : Take(kPad)) {
            reinterpret_cast<Nx2PadFn>(s.fn)(&e, s.user);
        }
    }
}

void RunExit() {
    std::printf("fake_host: the game ends: %d exit callbacks\n", Count(kExit));
    std::fflush(stdout);
    for (const Slot& s : Take(kExit)) {
        reinterpret_cast<Nx2ExitFn>(s.fn)(s.user);
    }
}

// The table as a version 3 game serves it: it ends before version 4's members.
const Nx2PluginApi g_api3 = {
    static_cast<std::uint32_t>(offsetof(Nx2PluginApi, camera)), 3, GameInfo, LogLine, Read, Write,
    OnFrame, OnOverlay, OnKey, OnText, Remove, GuestCall, GuestCallRegs, Capture, HookFunction,
    CallOriginal, MapRange, OnExit, AddTitleButton, OnPad, OpenMenu, UpdateMenu, CloseMenu,
};
// Version 4: a camera of this stand-in's own making.
int WorldToScreen(const float world[3], float out_xy[2]) {
    if (world[2] < -100.0f) {
        return 0;
    }
    out_xy[0] = 960.0f + 10.0f * world[0];
    out_xy[1] = 540.0f - 10.0f * world[1];
    return 1;
}
int Camera(Nx2Camera*) { return 0; }
// Version 4's restart: said, with the variables, and the stand-in then ends as
// a game that closes in an orderly way does (its main loop looks at g_restart).
std::atomic<bool> g_restart{false};
int Restart(const char* const* env) {
    std::string line = "fake_host: restart asked:";
    for (; env != nullptr && *env != nullptr; ++env) {
        const std::string text = *env;
        // The join password travels here: its value is not printed.
        line += " [" + (text.rfind("OOT_COOP_PASSWORD=", 0) == 0
                            ? std::string("OOT_COOP_PASSWORD=") + (text.size() > 18 ? "(set)" : "")
                            : text) + "]";
    }
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    g_restart = true;
    return 1;
}
const Nx2PluginApi g_api4 = {
    sizeof(Nx2PluginApi), 4, GameInfo, LogLine, Read, Write, OnFrame, OnOverlay, OnKey, OnText, Remove,
    GuestCall, GuestCallRegs, Capture, HookFunction, CallOriginal, MapRange, OnExit, AddTitleButton,
    OnPad, OpenMenu, UpdateMenu, CloseMenu, Camera, WorldToScreen, Restart,
};
// Version 5: the camera reset, said when asked; no set_save_dir in this stand-in.
int CameraReset() {
    std::printf("fake_host: camera reset asked\n");
    std::fflush(stdout);
    return 1;
}
const Nx2PluginApi g_api5 = {
    sizeof(Nx2PluginApi), 5, GameInfo, LogLine, Read, Write, OnFrame, OnOverlay, OnKey, OnText, Remove,
    GuestCall, GuestCallRegs, Capture, HookFunction, CallOriginal, MapRange, OnExit, AddTitleButton,
    OnPad, OpenMenu, UpdateMenu, CloseMenu, Camera, WorldToScreen, Restart, CameraReset, nullptr,
};
bool g_v3 = false;
bool g_v4 = false;
bool g_v5 = false;
const Nx2PluginApi* Served() { return g_v5 ? &g_api5 : g_v4 ? &g_api4 : g_v3 ? &g_api3 : &g_api; }

// --- Drawing, recorded ---

thread_local std::vector<std::string>* t_frame = nullptr;
std::mutex g_drawn;  // the three below
std::vector<std::string> g_last;
std::vector<std::string> g_texts;  // every distinct text, first seen first
std::set<std::string> g_seen;
std::string g_snapshots;

std::size_t CodePoints(const char* s) {
    std::size_t n = 0;
    for (; *s; ++s) {
        n += (static_cast<unsigned char>(*s) & 0xc0) != 0x80;
    }
    return n;
}
void DText(float x, float y, float size, std::uint32_t rgba, const char* s) {
    char head[96];
    std::snprintf(head, sizeof(head), "text %.0f %.0f %.0f a=%u | ", x, y, size, rgba >> 24);
    t_frame->push_back(head + std::string(s));
    std::lock_guard lock(g_drawn);
    if (g_seen.size() < 5000 && g_seen.insert(s).second) {
        g_texts.push_back(s);
    }
}
void DSize(float size, const char* s, float out[2]) {
    out[0] = 0.5f * size * static_cast<float>(CodePoints(s));  // a monospaced stand-in
    out[1] = size;
}
void DRect(float x0, float y0, float x1, float y1, std::uint32_t rgba, float) {
    char line[128];
    std::snprintf(line, sizeof(line), "rect %.0f %.0f %.0f %.0f %08x", x0, y0, x1, y1, rgba);
    t_frame->push_back(line);
}
void DRectFilled(float x0, float y0, float x1, float y1, std::uint32_t rgba, float) {
    char line[128];
    std::snprintf(line, sizeof(line), "rect_filled %.0f %.0f %.0f %.0f %08x", x0, y0, x1, y1, rgba);
    t_frame->push_back(line);
}
void DLine(float, float, float, float, std::uint32_t, float) { t_frame->push_back("line"); }

void Present() {
    std::vector<std::string> frame;
    t_frame = &frame;
    Nx2Draw d{};
    d.size = sizeof(d);
    d.width = 1920;
    d.height = 1080;
    d.scale = 1.0f;
    d.text_height = 24.0f;
    d.frame = g_frames.load();
    d.picture_x = 0;
    d.picture_y = 0;
    d.picture_width = 1920;
    d.picture_height = 1080;
    d.text = DText;
    d.text_size = DSize;
    d.rect = DRect;
    d.rect_filled = DRectFilled;
    d.line = DLine;
    {
        std::lock_guard calls(g_calls[kOverlay]);
        for (const Slot& s : Take(kOverlay)) {
            reinterpret_cast<Nx2OverlayFn>(s.fn)(&d, s.user);
        }
    }
    t_frame = nullptr;
    std::lock_guard lock(g_drawn);
    g_last = std::move(frame);
}

void Frame() {
    Nx2Frame f{sizeof(Nx2Frame), 0, g_frames++};
    {
        std::lock_guard calls(g_calls[kFrame]);
        for (const Slot& s : Take(kFrame)) {
            reinterpret_cast<Nx2FrameFn>(s.fn)(&f, s.user);
        }
    }
    if (g_page_close.exchange(false)) {
        MenuEvent(NX2_MENU_CLOSED, 0, 0);  // close_menu: the page goes as with "back"
    }
}

void Key(std::uint32_t key, bool down) {
    Nx2KeyEvent e{};
    e.size = sizeof(e);
    e.key = key;
    e.down = down ? 1 : 0;
    std::lock_guard calls(g_calls[kKey]);
    for (const Slot& s : Take(kKey)) {
        reinterpret_cast<Nx2KeyFn>(s.fn)(&e, s.user);
    }
}

void Text(std::uint32_t codepoint) {
    if (g_capture <= 0) {
        return;  // the game sends typed text only while the input is captured
    }
    std::lock_guard calls(g_calls[kText]);
    for (const Slot& s : Take(kText)) {
        reinterpret_cast<Nx2TextFn>(s.fn)(codepoint, s.user);
    }
}

struct Scripted {
    double at = 0;
    enum { key, text, snapshot, title, menu, menu_change, menu_back, pad, pad_key, menu_text } what = key;
    std::uint32_t key_code = 0;
    std::u32string text_value;
    std::string utf8;  // menu_text
    std::uint32_t row = 0;
    std::int32_t value = 0;
};

std::uint32_t PadCode(const std::wstring& name) {
    if (name == L"a") return NX2_PAD_A;
    if (name == L"b") return NX2_PAD_B;
    if (name == L"up") return NX2_PAD_UP;
    if (name == L"down") return NX2_PAD_DOWN;
    if (name == L"left") return NX2_PAD_LEFT;
    return NX2_PAD_RIGHT;
}

std::uint32_t KeyCode(const std::wstring& name) {
    if (name == L"enter") return NX2_KEY_ENTER;
    if (name == L"esc") return NX2_KEY_ESCAPE;
    if (name == L"backspace") return NX2_KEY_BACKSPACE;
    if (name == L"tab") return NX2_KEY_TAB;
    if (name == L"f9") return NX2_KEY_F1 + 8;
    return static_cast<std::uint32_t>(std::wcstoul(name.c_str(), nullptr, 0));
}

std::u32string CodePointsOf(const std::wstring& wide) {
    std::u32string out;
    for (std::size_t i = 0; i < wide.size(); ++i) {
        std::uint32_t c = wide[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < wide.size()) {
            c = 0x10000 + ((c - 0xd800) << 10) + (static_cast<std::uint32_t>(wide[++i]) - 0xdc00);
        }
        out.push_back(c);
    }
    return out;
}

}  // namespace host

extern "C" __declspec(dllexport) const Nx2PluginApi* nx2navi_plugin_api(std::uint32_t version) {
    if (host::g_v3 || host::g_v4) {
        return version >= 1 && version <= host::Served()->version ? host::Served() : nullptr;
    }
    return version == 1 ? &host::g_api : nullptr;  // a game from before version 2
}
#endif

namespace {

void Put(std::uint64_t address, const void* data, std::size_t size) {
    std::memcpy(reinterpret_cast<void*>(address), data, size);
}
void PutU8(std::uint64_t a, std::uint8_t v) { Put(a, &v, 1); }
void PutU16(std::uint64_t a, std::uint16_t v) { Put(a, &v, 2); }
void PutU32(std::uint64_t a, std::uint32_t v) { Put(a, &v, 4); }
void PutU64(std::uint64_t a, std::uint64_t v) { Put(a, &v, 8); }
void PutF32(std::uint64_t a, float v) { PutU32(a, std::bit_cast<std::uint32_t>(v)); }
void PutVec3(std::uint64_t a, float x, float y, float z) {
    PutF32(a, x);
    PutF32(a + 4, y);
    PutF32(a + 8, z);
}

// The objects on the fake heap, 64 KiB apart.
struct Layout {
    std::uint64_t game_ui, scene, table, array, manager, link1, holder1, actor1, link2, holder2,
        actor2, banc, path, events;
};

void PutActor(std::uint64_t link, std::uint64_t holder, std::uint64_t actor, std::uint32_t id,
              std::uint32_t state) {
    PutU64(link + addr::kLinkHolder, holder);
    PutU32(link + addr::kLinkId, id);
    PutU64(holder + addr::kHolderActor, actor);
    PutU32(holder + addr::kHolderId, id);
    PutU32(actor + addr::kActorState, state);
    PutVec3(actor + addr::kActorPosition, 0, 0, 0);
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            PutF32(actor + addr::kActorRotation + 4 * static_cast<std::uint64_t>(3 * i + j),
                   i == j ? 1.0f : 0.0f);
        }
    }
}

bool MakeGame(bool other_build, const std::string& scene, Layout& l) {
    const std::uint64_t base = CoopNx2navi::kMainBase;
    // Room for every pointer slot coop/host reads in main.
    const std::size_t main_size = 0x5000000;
    if (VirtualAlloc(reinterpret_cast<void*>(base), main_size, MEM_RESERVE | MEM_COMMIT,
                     PAGE_READWRITE) != reinterpret_cast<void*>(base)) {
        std::fprintf(stderr, "fake_game: cannot map main at %#llx (error %lu)\n",
                     static_cast<unsigned long long>(base), GetLastError());
        return false;
    }
    const std::size_t slot = 0x10000;
    auto* heap = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, 16 * slot, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (heap == nullptr) {
        return false;
    }
    std::uint64_t next = reinterpret_cast<std::uint64_t>(heap);
    for (std::uint64_t* p : {&l.game_ui, &l.scene, &l.table, &l.array, &l.manager, &l.link1,
                             &l.holder1, &l.actor1, &l.link2, &l.holder2, &l.actor2, &l.banc,
                             &l.path, &l.events}) {
        *p = next;
        next += slot;
    }

    // The module header and the build id note.
    PutU32(base + 4, 0x100);
    Put(base + 0x100, "MOD0", 4);
    const std::uint8_t note[16] = {4, 0, 0, 0, 20, 0, 0, 0, 3, 0, 0, 0, 'G', 'N', 'U', 0};
    Put(base + 0x2000, note, sizeof(note));
    const std::string id(CoopGameBuild::kPrimary.main_id);
    for (std::size_t i = 0; i < 20; ++i) {
        auto byte = static_cast<std::uint8_t>(std::stoi(id.substr(2 * i, 2), nullptr, 16));
        if (other_build && i == 0) {
            byte ^= 0xff;
        }
        PutU8(base + 0x2010 + i, byte);
    }

    // The player, as tests/host/fake_memory.hpp make_game lays it out.
    PutU64(base + addr::kGameUiModulePtr, l.game_ui);
    PutU16(l.game_ui + addr::kGameUiPoseFlags, addr::kGameUiPoseValidBit);
    PutU8(l.game_ui + addr::kGameUiIsChild, 1);
    PutU64(base + addr::kSceneModulePtr, l.scene);
    PutU64(l.scene + addr::kSceneTable, l.table);
    PutU32(l.table + addr::kTableCount, 0x20);
    PutU64(l.table + addr::kTableArray, l.array);
    PutU64(l.array + addr::kPlayerManagerSlot, l.manager);
    PutU64(l.manager + addr::kManagerMustBeSet, 0x1234);
    PutU64(l.manager + addr::kManagerLinkFirst, l.link1);
    PutU64(l.manager + addr::kManagerLinkSecond, l.link2);
    PutActor(l.link1, l.holder1, l.actor1, 7, addr::kActorStatePresent);
    PutActor(l.link2, l.holder2, l.actor2, addr::kEmptyId, 0);
    PutU64(base + addr::kBancModulePtr, l.banc);
    PutU64(l.banc + addr::kBancMustBeSet, 0x5678);
    PutU64(l.banc + addr::kBancPathPtr, l.path);
    PutU64(l.banc + addr::kBancPathCapacity, 64);
    const std::string path = "Work/Banc/Field/" + scene + ".banc";
    Put(l.path, path.c_str(), path.size() + 1);
    PutU64(base + addr::kEventManagerPtr, l.events);
    PutU64(l.events + addr::kEventRunning, 0);

    // Like the port's loader: code and constants are sealed read-only.
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(base), 0x100000, PAGE_READONLY, &old);
    return true;
}

void Walk(const Layout& l, double seconds, float radius) {
    const float x = radius * static_cast<float>(std::cos(seconds));
    const float z = radius * static_cast<float>(std::sin(seconds));
    PutVec3(l.game_ui + addr::kGameUiPosition, x, 2.0f, z);
    PutVec3(l.actor1 + addr::kActorPosition, x, 2.0f, z);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: fake_game <dll> [--seconds N] [--no-game] [--other-build] "
                             "[--scene NAME] [--radius R]\n");
        return 2;
    }
    double seconds = 5;
    bool game = true;
    bool other_build = false;
    std::string scene = "spot00";
    float radius = 3.0f;
#ifdef COOP_FAKE_PLUGIN_HOST
    bool init = true;
    bool twice = false;
    bool exit_callbacks = false;
    std::wstring draw_out;
    std::vector<host::Scripted> script;
#endif
    for (int i = 2; i < argc; ++i) {
        const std::wstring arg = argv[i];
#ifdef COOP_FAKE_PLUGIN_HOST
        if (arg == L"--no-init") {
            init = false;
            continue;
        }
        if (arg == L"--twice") {
            twice = true;  // loaded twice and offered the interface twice
            continue;
        }
        if (arg == L"--v3") {
            host::g_v3 = true;
            continue;
        }
        if (arg == L"--v4") {
            host::g_v4 = true;
            continue;
        }
        if (arg == L"--v5") {  // a French game with interface version 5
            host::g_v4 = true;
            host::g_v5 = true;
            host::g_language = "fr-CA";
            continue;
        }
        if (arg == L"--exit") {
            exit_callbacks = true;
            continue;
        }
        if ((arg == L"--game-dir" || arg == L"--profile" || arg == L"--save-root") && i + 1 < argc) {
            const std::wstring wide = argv[++i];
            std::string utf8(static_cast<std::size_t>(WideCharToMultiByte(
                                 CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr)),
                             '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), static_cast<int>(utf8.size()),
                                nullptr, nullptr);
            utf8.resize(std::strlen(utf8.c_str()));
            if (arg == L"--game-dir") {
                host::g_game_dir = utf8;
            } else if (arg == L"--save-root") {
                // As NX2NAVI_SAVE_DIR: the saves are in another folder than the profile's
                // (after --profile).
                host::g_save_dir = utf8 + "/save/0123456789abcdef0123456789abcdef";
            } else {
                host::g_profile_dir = utf8;
                host::g_save_dir = utf8 + "/save/0123456789abcdef0123456789abcdef";
            }
            continue;
        }
        if ((arg == L"--title" || arg == L"--menu-back") && i + 1 < argc) {
            host::Scripted s;
            s.at = std::wcstod(argv[++i], nullptr);
            s.what = arg == L"--title" ? host::Scripted::title : host::Scripted::menu_back;
            script.push_back(s);
            continue;
        }
        if (arg == L"--menu" && i + 2 < argc) {
            host::Scripted s;
            s.at = std::wcstod(argv[++i], nullptr);
            s.what = host::Scripted::menu;
            s.row = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
            script.push_back(s);
            continue;
        }
        if (arg == L"--menu-text" && i + 3 < argc) {
            host::Scripted s;
            s.at = std::wcstod(argv[++i], nullptr);
            s.what = host::Scripted::menu_text;
            s.row = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
            const std::wstring wide = argv[++i];
            const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                                 nullptr, 0, nullptr, nullptr);
            s.utf8.assign(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), s.utf8.data(), size,
                                nullptr, nullptr);
            script.push_back(s);
            continue;
        }
        if (arg == L"--menu-change" && i + 3 < argc) {
            host::Scripted s;
            s.at = std::wcstod(argv[++i], nullptr);
            s.what = host::Scripted::menu_change;
            s.row = static_cast<std::uint32_t>(std::wcstoul(argv[++i], nullptr, 10));
            s.value = static_cast<std::int32_t>(std::wcstol(argv[++i], nullptr, 10));
            script.push_back(s);
            continue;
        }
        if ((arg == L"--pad" || arg == L"--pad-key") && i + 2 < argc) {
            host::Scripted s;
            s.at = std::wcstod(argv[++i], nullptr);
            s.what = arg == L"--pad" ? host::Scripted::pad : host::Scripted::pad_key;
            s.key_code = host::PadCode(argv[++i]);
            script.push_back(s);
            continue;
        }
        if (arg == L"--draw-out" && i + 1 < argc) {
            draw_out = argv[++i];
            continue;
        }
        if ((arg == L"--key" || arg == L"--type") && i + 2 < argc) {
            host::Scripted s;
            s.at = std::wcstod(argv[++i], nullptr);
            const std::wstring value = argv[++i];
            if (arg == L"--key") {
                s.what = host::Scripted::key;
                s.key_code = host::KeyCode(value);
            } else {
                s.what = host::Scripted::text;
                s.text_value = host::CodePointsOf(value);
            }
            script.push_back(s);
            continue;
        }
        if (arg == L"--snapshot" && i + 1 < argc) {
            host::Scripted s;
            s.at = std::wcstod(argv[++i], nullptr);
            s.what = host::Scripted::snapshot;
            script.push_back(s);
            continue;
        }
#endif
        if (arg == L"--seconds" && i + 1 < argc) {
            seconds = std::wcstod(argv[++i], nullptr);
        } else if (arg == L"--no-game") {
            game = false;
        } else if (arg == L"--other-build") {
            other_build = true;
        } else if (arg == L"--scene" && i + 1 < argc) {
            const std::wstring wide = argv[++i];
            scene.assign(wide.begin(), wide.end());
        } else if (arg == L"--radius" && i + 1 < argc) {
            radius = static_cast<float>(std::wcstod(argv[++i], nullptr));
        } else {
            std::fprintf(stderr, "fake_game: unknown argument\n");
            return 2;
        }
    }

    Layout layout{};
    if (game && !MakeGame(other_build, scene, layout)) {
        return 3;
    }
    const HMODULE dll = LoadLibraryW(argv[1]);
    if (dll == nullptr) {
        std::fprintf(stderr, "fake_game: LoadLibrary failed (error %lu)\n", GetLastError());
        return 4;
    }
    using StateFn = int (*)();
    using StopFn = int (*)(unsigned);
    const auto state = reinterpret_cast<StateFn>(
        reinterpret_cast<void*>(GetProcAddress(dll, "coop_nx2navi_state")));
    const auto stop = reinterpret_cast<StopFn>(
        reinterpret_cast<void*>(GetProcAddress(dll, "coop_nx2navi_stop")));
    if (state == nullptr || stop == nullptr) {
        std::fprintf(stderr, "fake_game: the DLL does not export its two functions\n");
        return 5;
    }

    const ULONGLONG start = GetTickCount64();
#ifdef COOP_FAKE_PLUGIN_HOST
    if (init) {
        using InitFn = void (*)(const Nx2PluginApi*);
        const auto plugin_init = reinterpret_cast<InitFn>(
            reinterpret_cast<void*>(GetProcAddress(dll, NX2_PLUGIN_INIT_EXPORT)));
        if (plugin_init == nullptr) {
            std::fprintf(stderr, "fake_host: the DLL does not export %s\n", NX2_PLUGIN_INIT_EXPORT);
            return 8;
        }
        plugin_init(host::Served());
        if (twice) {
            // As when the launcher injected the DLL and NX2NAVI_MOD_DLLS names it
            // too (or names it twice): the same module, a second init.
            if (LoadLibraryW(argv[1]) != dll) {
                std::fprintf(stderr, "fake_host: a second LoadLibrary gave another module\n");
                return 9;
            }
            plugin_init(host::Served());
        }
    }
    std::stable_sort(script.begin(), script.end(),
                     [](const host::Scripted& a, const host::Scripted& b) { return a.at < b.at; });
    std::atomic<bool> driving{true};
    const auto passed_s = [start] { return static_cast<double>(GetTickCount64() - start) / 1000.0; };
    // The game's thread and the presenting thread, one here: a frame, then its picture.
    std::thread present([&] {
        while (driving) {
            host::Frame();
            host::Present();
            Sleep(16);
        }
    });
    // The window's thread: the scripted keys and text.
    std::thread window([&] {
        std::size_t next = 0;
        while (driving && next < script.size()) {
            const host::Scripted& s = script[next];
            if (passed_s() < s.at) {
                Sleep(5);
                continue;
            }
            ++next;
            if (s.what == host::Scripted::key) {
                host::Key(s.key_code, true);
                host::Key(s.key_code, false);
            } else if (s.what == host::Scripted::text) {
                for (const char32_t c : s.text_value) {
                    host::Text(static_cast<std::uint32_t>(c));
                    Sleep(2);
                }
            } else if (s.what == host::Scripted::title) {
                host::ChooseTitle();
            } else if (s.what == host::Scripted::menu) {
                host::MenuEvent(NX2_MENU_ACTIVATE, s.row, 0);
            } else if (s.what == host::Scripted::menu_change) {
                host::MenuEvent(NX2_MENU_CHANGE, s.row, s.value);
            } else if (s.what == host::Scripted::menu_text) {
                host::MenuEvent(NX2_MENU_TEXT, s.row, static_cast<std::int32_t>(s.utf8.size()), s.utf8.c_str());
            } else if (s.what == host::Scripted::pad_key) {
                host::Pad(s.key_code, true);
            } else if (s.what == host::Scripted::menu_back) {
                host::MenuEvent(NX2_MENU_CLOSED, 0, 0);
            } else if (s.what == host::Scripted::pad) {
                host::Pad(s.key_code);
            } else {
                std::lock_guard lock(host::g_drawn);
                char head[64];
                std::snprintf(head, sizeof(head), "== picture at %.1f s ==\n", s.at);
                host::g_snapshots += head;
                for (const std::string& line : host::g_last) {
                    host::g_snapshots += line + "\n";
                }
            }
        }
    });
#endif
    int best = 0;
    while (true) {
        const double passed = static_cast<double>(GetTickCount64() - start) / 1000.0;
        if (passed >= seconds) {
            break;
        }
#ifdef COOP_FAKE_PLUGIN_HOST
        if (host::g_restart) {
            exit_callbacks = true;  // a restart is an orderly end
            break;
        }
#endif
        if (game) {
            Walk(layout, passed, radius);
        }
        best = std::max(best, state());
        Sleep(16);
    }
    std::printf("fake_game: best state %d, last state %d\n", best, state());
#ifdef COOP_FAKE_PLUGIN_HOST
    std::printf("fake_host: callbacks frame %d overlay %d key %d text %d; input captured %d "
                "(%d capture calls); %llu frames\n",
                host::Count(host::kFrame), host::Count(host::kOverlay), host::Count(host::kKey),
                host::Count(host::kText), host::g_capture.load(), host::g_capture_calls.load(),
                static_cast<unsigned long long>(host::g_frames.load()));
    std::string last_picture;
    {
        std::lock_guard lock(host::g_drawn);
        for (const std::string& line : host::g_last) {
            last_picture += line + "\n";
        }
    }
#endif
#ifdef COOP_FAKE_PLUGIN_HOST
    if (exit_callbacks) {
        host::RunExit();
    }
#endif
    if (stop(10000) != 1) {
        std::fprintf(stderr, "fake_game: the DLL's thread did not end in 10 s\n");
        return 6;
    }
#ifdef COOP_FAKE_PLUGIN_HOST
    // The DLL took its callbacks out while they were still being called.
    const int left = host::Count(host::kFrame) + host::Count(host::kOverlay) +
                     host::Count(host::kKey) + host::Count(host::kText) + host::Count(host::kPad) +
                     host::Count(host::kTitle) + host::Count(host::kExit);
    std::printf("fake_host: callbacks left after stop %d, input captured %d\n", left,
                host::g_capture.load());
    driving = false;
    present.join();
    window.join();
    if (!draw_out.empty()) {
        std::FILE* out = _wfopen(draw_out.c_str(), L"wb");
        if (out != nullptr) {
            std::lock_guard lock(host::g_drawn);
            std::fputs(host::g_snapshots.c_str(), out);
            std::fputs("== last picture ==\n", out);
            std::fputs(last_picture.c_str(), out);
            std::fputs("== texts ==\n", out);
            for (const std::string& text : host::g_texts) {
                std::fputs((text + "\n").c_str(), out);
            }
            std::fclose(out);
        }
    }
    std::fflush(stdout);
#endif
#ifdef COOP_FAKE_PLUGIN_HOST
    if (twice) {
        FreeLibrary(dll);
    }
#endif
    FreeLibrary(dll);
    Sleep(100);
    if (GetModuleHandleW(argv[1]) != nullptr) {
        std::fprintf(stderr, "fake_game: the DLL is still loaded after FreeLibrary\n");
        return 7;
    }
    std::printf("fake_game: unloaded cleanly\n");
    return 0;
}
