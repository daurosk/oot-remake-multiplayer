// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_window_brand.h"

#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

#include "coop_png.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace CoopWindowBrand {

bool IsGameWindowClass(const std::string& cls) {
    return cls == "Longshot" || cls == "Hookshot";
}

bool TitleNeedsSetting(const std::string& current, const std::string& want) {
    return current != want;
}

std::string ReleaseOf(const std::string& version_first_line) {
    static constexpr const char* kOpen = "(release ";
    const std::size_t at = version_first_line.rfind(kOpen);
    if (at == std::string::npos) return {};
    const std::size_t from = at + std::strlen(kOpen);
    const std::size_t end = version_first_line.find(')', from);
    if (end == std::string::npos || end == from || end - from > 32) return {};
    std::string tag = version_first_line.substr(from, end - from);
    for (const char c : tag) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '+' && c != '_') return {};
    }
    return tag;
}

std::string TitleFor(const std::string& release) {
    if (release.empty()) return kTitle;
    const bool digit = std::isdigit(static_cast<unsigned char>(release.front())) != 0;
    return std::string(kTitle) + (digit ? " v" : " ") + release;
}

std::string KitRelease(const std::filesystem::path& game_dir) {
    std::filesystem::path d = game_dir;
    for (int i = 0; i < 5 && !d.empty(); ++i) {
        std::ifstream in(d / "VERSION.txt", std::ios::binary);
        std::string first;
        if (in && std::getline(in, first)) {
            if (!first.empty() && first.back() == '\r') first.pop_back();
            if (first.rfind("kit: ", 0) == 0) return ReleaseOf(first);
        }
        if (d.parent_path() == d) break;
        d = d.parent_path();
    }
    return {};
}

const std::string& TitleOf(const std::filesystem::path& game_dir) {
    static std::once_flag once;
    static std::string title;
    std::call_once(once, [&] { title = TitleFor(KitRelease(game_dir)); });
    return title;
}

std::vector<std::uint8_t> RgbaToIconBgra(const std::uint8_t* rgba, int width, int height) {
    std::vector<std::uint8_t> out;
    if (rgba == nullptr || width <= 0 || height <= 0) {
        return out;
    }
    out.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = rgba + static_cast<std::size_t>(height - 1 - y) * static_cast<std::size_t>(width) * 4;
        std::uint8_t* dst = out.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4;
        for (int x = 0; x < width; ++x) {
            const unsigned a = row[x * 4 + 3];
            dst[x * 4 + 0] = static_cast<std::uint8_t>((row[x * 4 + 2] * a + 127) / 255);
            dst[x * 4 + 1] = static_cast<std::uint8_t>((row[x * 4 + 1] * a + 127) / 255);
            dst[x * 4 + 2] = static_cast<std::uint8_t>((row[x * 4 + 0] * a + 127) / 255);
            dst[x * 4 + 3] = static_cast<std::uint8_t>(a);
        }
    }
    return out;
}

#ifdef _WIN32
namespace {

HICON MakeIcon(const CoopPng::Image& img) {
    const std::vector<std::uint8_t> bgra = RgbaToIconBgra(img.rgba.data(), img.width, img.height);
    if (bgra.empty()) {
        return nullptr;
    }
    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof bi;
    bi.bV5Width = img.width;
    bi.bV5Height = img.height;  // bottom-up, the order RgbaToIconBgra gives
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (color == nullptr || bits == nullptr) {
        if (color != nullptr) DeleteObject(color);
        return nullptr;
    }
    std::memcpy(bits, bgra.data(), bgra.size());
    HBITMAP mask = CreateBitmap(img.width, img.height, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmColor = color;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

struct Found {
    HWND hwnd = nullptr;
};

BOOL CALLBACK Each(HWND h, LPARAM l) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) {
        return TRUE;
    }
    char cls[64] = {};
    GetClassNameA(h, cls, sizeof cls);
    if (IsGameWindowClass(cls)) {
        reinterpret_cast<Found*>(l)->hwnd = h;
        return FALSE;
    }
    return TRUE;
}

HICON g_icon = nullptr;
bool g_icon_tried = false;
HWND g_icon_window = nullptr;

}  // namespace

void Tick(const std::filesystem::path& game_dir) {
    using Clock = std::chrono::steady_clock;
    static Clock::time_point next{};
    const Clock::time_point now = Clock::now();
    if (now < next) {
        return;
    }
    next = now + std::chrono::seconds(1);
    Found f;
    EnumWindows(Each, reinterpret_cast<LPARAM>(&f));
    if (f.hwnd == nullptr) {
        return;
    }
    wchar_t cur[256] = {};
    InternalGetWindowText(f.hwnd, cur, 256);  // no WM_GETTEXT sent to the display thread
    std::string narrow;
    for (const wchar_t* p = cur; *p != 0; ++p) narrow.push_back(*p < 128 ? static_cast<char>(*p) : '?');
    const std::string& want = TitleOf(game_dir);   // ASCII only (ReleaseOf)
    if (TitleNeedsSetting(narrow, want)) {
        const std::wstring wide(want.begin(), want.end());
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(f.hwnd, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(wide.c_str()), SMTO_ABORTIFHUNG, 200, &ignored);
    }
    if (!g_icon_tried) {
        g_icon_tried = true;
        const std::filesystem::path png = game_dir / kIconFile;
        if (const auto img = CoopPng::Load(png); img && img->width == img->height) {
            g_icon = MakeIcon(*img);
        }
    }
    if (g_icon != nullptr && g_icon_window != f.hwnd) {
        g_icon_window = f.hwnd;
        DWORD_PTR ignored = 0;
        SendMessageTimeoutW(f.hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_icon), SMTO_ABORTIFHUNG, 200, &ignored);
        SendMessageTimeoutW(f.hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_icon), SMTO_ABORTIFHUNG, 200, &ignored);
    }
}
#else
void Tick(const std::filesystem::path&) {}
#endif

}  // namespace CoopWindowBrand
