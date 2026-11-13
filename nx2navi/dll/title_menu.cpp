// SPDX-License-Identifier: GPL-3.0-or-later
#include "title_menu.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wincrypt.h>

#include <share.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <random>
#include <vector>

#include "mp_saves.hpp"
#include "overlay_model.hpp"
#include "steam_menu.hpp"
#include "steam_model.hpp"
#include "steam_net.hpp"
#include "third_party/plugin_api.h"

namespace CoopNx2navi::TitleMenu {
namespace {

std::atomic<void (*)(const char*)> g_log{nullptr};

void Log(const char* format, ...) {
    auto* log = g_log.load();
    if (log == nullptr) {
        return;
    }
    char line[768];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    log(line);
}

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

std::string Utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string Environment(const wchar_t* name) {
    std::wstring value(4096, L'\0');
    const DWORD size = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    value.resize(size < value.size() ? size : 0);
    return Utf8(value);
}

std::string ReadText(const std::filesystem::path& path) {
    std::FILE* file = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (file == nullptr) {
        return {};
    }
    std::string text;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < (1u << 20)) {
        text.append(buffer, got);
    }
    std::fclose(file);
    return text;
}

// --- State ---

std::mutex g_m;  // everything below but the handles' use of the interface
const Nx2PluginApi* g_api = nullptr;
std::filesystem::path g_folder;
Menu::State g_state;
Menu::FlatJson g_settings;
bool g_registered = false;
bool g_open = false;
std::atomic<bool> g_page_shown{false};  // g_open, for PageOpen
bool g_dirty = false;         // the page shows something older than the state
bool g_label_stale = false;   // the title entry's text is in the other language
int g_title = 0;
int g_handles[5] = {0, 0, 0, 0, 0};  // frame, key, text, pad, the caption hook
// The caption under the list. The game sets it when the cursor comes to a row
// (its settings screen's caption function, main+0x34031d0 in game 1.1.1:
// (screen, row type)), and the runtime gives our row's text to that call; an
// update of the open page's rows does not make the game set it again, so a new
// text for the row under the cursor (what is being typed, a refusal's reason,
// "copied") would only show once the cursor moved. A hook of that function
// tells us the screen and the row it last set, and after each update the same
// call is made again. For game 1.1.1 only (the address is that build's); asked
// of the runtime as its own job (NOTES.md).
constexpr std::uint64_t kCaptionFunction = 0x34031d0;
std::atomic<std::uint64_t> g_caption_screen{0};
std::atomic<std::uint64_t> g_caption_type{0};

// Interface version 5: update_menu makes the game set the caption of the row
// under the cursor again by itself, so the hook and the call below are not
// made there. OOT_COOP_MENU_CAPTION=own keeps them, for a comparison.
bool RuntimeSetsCaption() {
    return g_api != nullptr && g_api->version >= 5 && Environment(L"OOT_COOP_MENU_CAPTION") != "own";
}

void OnCaption(Nx2HookCall* call, void*) {
    g_caption_screen.store(call->x[0], std::memory_order_relaxed);
    g_caption_type.store(call->x[1], std::memory_order_relaxed);
}

// The game's thread, after an update the game took: the caption of the row
// under the cursor, set again.
void RefreshCaption() {
    const std::uint64_t screen = g_caption_screen.load(std::memory_order_relaxed);
    const Nx2GameInfo* info = g_api->game_info();
    if (screen == 0 || info == nullptr || info->main_base == 0 || info->game_version != 111 ||
        RuntimeSetsCaption()) {
        return;
    }
    const std::uint64_t args[2] = {screen, g_caption_type.load(std::memory_order_relaxed)};
    g_api->guest_call(info->main_base + kCaptionFunction, args, 2);
}
std::optional<Request> g_request;
int g_captured = 0;           // our capture_input(1) not yet undone
std::uint64_t g_edit_at_ms = 0;
std::atomic<std::uint64_t> g_key_at_ms{0};
std::uint64_t g_close_at_ms = 0;  // close the page then (0: no)
// For tests (OOT_COOP_TEST_SCRIPT_PAD=1): the presses of NX2navi's input script count
// as a gamepad's. The runtime marks them as keys (no gamepad holds the button).
bool g_script_pad = false;
bool g_debug = false;  // OOT_COOP_DEBUG_MENU=1: the cursor's row in the log
// The game can take a text row's typing itself (interface version 4's text_max):
// used while the player's presses come from a keyboard. OOT_COOP_MENU_TYPING=
// pad or game says which typing every press gets, for tests.
bool g_game_typing = false;
int g_typing_forced = 0;  // 0 by the device, 1 always ours (pad), 2 always the game's
// How the page goes from one of its three pages to another. false: an update
// of the open page's rows (at once; the caption under the list keeps the old
// row's text until the cursor moves). true: the page is opened anew over the
// open one (interface version 4: the cursor starts on the new page's first row
// with its own caption; the title menu shows for about a second in between).
// OOT_COOP_MENU_PAGES=update or replace says which.
bool g_replace_pages = false;
std::uint64_t g_replacing_until_ms = 0;  // a page is being replaced: updates wait

// --- Over Steam (steam_net.hpp, steam_menu.hpp) ---

// What can be said of Steam without signing in, looked up when the page opens.
Menu::SteamAvail g_steam_precheck = Menu::SteamAvail::old_game;
std::uint64_t g_steam_synced_ms = 0;

// Under g_m: what steam_net.hpp knows, into the pages. True when a page shows
// something else for it.
bool SyncSteam() {
    const SteamNet::View v = SteamNet::Get();
    std::vector<std::string> members;
    for (const SteamNet::Member& m : v.members) {
        members.push_back(m.name);
    }
    std::vector<Menu::SteamInvite> invites;
    for (const SteamNet::Invitation& i : v.invites) {
        invites.push_back({i.from, i.room});
    }
    const bool hosting = v.owner && v.link == SteamNet::Link::ready;
    static bool said = false;
    if (hosting != said) {
        // Counts only: a room's id, a Steam id and a friend's name are never logged.
        said = hosting;
        if (hosting) {
            Log("menu: Host over Steam shows the room's ID (%u characters), %u player%s and %u friend%s to invite",
                static_cast<unsigned>(v.room.size()), static_cast<unsigned>(v.members.size()),
                v.members.size() == 1 ? "" : "s", static_cast<unsigned>(v.friends), v.friends == 1 ? "" : "s");
        }
    }
    return Menu::SteamPages::Update(g_state, v.started ? v.avail : g_steam_precheck, v.name,
                                    hosting ? v.room : std::string(), hosting ? std::move(members)
                                                                              : std::vector<std::string>(),
                                    hosting ? SteamNet::Friends() : std::vector<Menu::SteamFriend>(),
                                    std::move(invites));
}

std::string ClipboardText() {
    std::string out;
    if (!OpenClipboard(nullptr)) {
        return out;
    }
    if (const HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(data))) {
            out = Utf8(std::wstring(text, wcsnlen(text, 64)));
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return out;
}

// --- Settings ---

std::filesystem::path GameSaveDir();

void SaveSettings() {
    // Under g_m. The launcher add-on's file and keys; the passwords typed in
    // the game are kept protected, never as clear text.
    g_settings.SetString("name", g_state.name);
    g_settings.SetString("host_address", g_state.address);
    g_settings.SetNumber("port", std::strtol(g_state.port.c_str(), nullptr, 10));
    // The keys of the language row this page had until coop-dll 0.5: gone (the
    // language is the game's; "language" overrides it, written by hand).
    g_settings.Erase("menu_language");
    g_settings.Erase("menu_language_game");
    // The name typed on a Steam page; without it the Steam name is used.
    if (g_state.steam.own_name.empty()) {
        g_settings.Erase("steam_name");
    } else {
        g_settings.SetString("steam_name", g_state.steam.own_name);
    }
    const auto keep = [](const char* plain, const char* kept, const std::string& password) {
        // Only when it differs from the add-on's clear one: that one stays the add-on's.
        if (g_settings.String(plain).value_or("") == password) {
            g_settings.Erase(kept);
            return;
        }
        g_settings.SetString(plain, "");
        const std::string stored = password.empty() ? std::string() : Protect(password);
        if (stored.empty()) {
            g_settings.Erase(kept);
        } else {
            g_settings.SetString(kept, stored);
        }
    };
    // The multiplayer save hosted on last (its folder's name).
    if (g_state.save.empty()) {
        g_settings.Erase("mp_save");
    } else {
        g_settings.SetString("mp_save", g_state.save);
    }
    keep("join_password", "join_password_protected", g_state.join_password);
    keep("server_password", "server_password_protected", g_state.host_password);
    const std::filesystem::path path = g_folder / L"settings.json";
    std::filesystem::path tmp = path;
    tmp += L".tmp";
    std::FILE* file = _wfsopen(tmp.c_str(), L"wb", _SH_DENYWR);
    if (file == nullptr) {
        Log("menu: the settings could not be written (%s)", Utf8(tmp.wstring()).c_str());
        return;
    }
    const std::string text = g_settings.Dump();
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    std::fclose(file);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        Log("menu: the settings could not be written (%s)", Utf8(path.wstring()).c_str());
    }
}

std::string DefaultName() {
    wchar_t user[257] = {};
    DWORD size = 257;
    if (GetUserNameW(user, &size)) {
        std::string name = Utf8(user);
        while (Ui::Utf8Count(name) > Menu::kMaxName) {
            Ui::Utf8PopBack(name);
        }
        if (Menu::ValidName(name)) {
            return name;
        }
    }
    return "Link";
}

// This PC's IPv4 addresses with what Windows says about each one's network
// card (GetAdaptersAddresses: is it up, has it a default gateway, is it a
// virtual machine's switch), in Windows' own order, and last the address a UDP
// socket would use to reach the outside (nothing is sent: the socket is only
// asked), in case the list could not be had.
std::vector<Menu::LocalAddress> LocalAddresses() {
    std::vector<Menu::LocalAddress> found;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 4 && result == ERROR_BUFFER_OVERFLOW; ++tries) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET,
                                      GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                          GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                      nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
                                      &size);
    }
    if (result == NO_ERROR) {
        for (const IP_ADAPTER_ADDRESSES* card = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
             card != nullptr; card = card->Next) {
            if (card->OperStatus != IfOperStatusUp || card->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }
            const std::string name = Utf8(card->FriendlyName != nullptr ? card->FriendlyName : L"");
            const std::string description = Utf8(card->Description != nullptr ? card->Description : L"");
            for (const IP_ADAPTER_UNICAST_ADDRESS* a = card->FirstUnicastAddress; a != nullptr; a = a->Next) {
                if (a->Address.lpSockaddr == nullptr || a->Address.lpSockaddr->sa_family != AF_INET) {
                    continue;
                }
                char text[64] = {};
                const auto* in = reinterpret_cast<const sockaddr_in*>(a->Address.lpSockaddr);
                if (inet_ntop(AF_INET, &in->sin_addr, text, sizeof(text)) == nullptr) {
                    continue;
                }
                Menu::LocalAddress local;
                local.address = text;
                local.gateway = card->FirstGatewayAddress != nullptr;
                local.virtual_card = Menu::VirtualCard(name, description);
                local.card = name + " (" + description + ")";
                found.push_back(std::move(local));
            }
        }
    }
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return found;
    }
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s != INVALID_SOCKET) {
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(9);
        inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);  // a documentation address; UDP connect sends nothing
        sockaddr_in self{};
        int size = sizeof(self);
        if (connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0 &&
            getsockname(s, reinterpret_cast<sockaddr*>(&self), &size) == 0) {
            char text[64] = {};
            if (inet_ntop(AF_INET, &self.sin_addr, text, sizeof(text)) != nullptr &&
                std::none_of(found.begin(), found.end(),
                             [&](const Menu::LocalAddress& a) { return a.address == text; })) {
                // Not in the list (or no list): the route outside goes through it.
                found.push_back({text, true, false, "the route outside"});
            }
        }
        closesocket(s);
    }
    WSACleanup();
    return found;
}

std::filesystem::path PathOf(const char* utf8) {
    return utf8 != nullptr && *utf8 != 0 ? std::filesystem::path(Wide(utf8)) : std::filesystem::path();
}

// The folder of the game's save now, "" while the game has not opened it.
std::filesystem::path GameSaveDir() {
    const Nx2GameInfo* info = g_api->game_info();
    if (info == nullptr || !NX2_HAS(info, Nx2GameInfo, save_dir)) {
        return {};
    }
    return PathOf(info->save_dir);
}

// The account folder's name, "" when not known yet.
std::string Account() {
    const std::filesystem::path now = GameSaveDir();
    if (!now.empty()) {
        return Utf8(now.filename().wstring());
    }
#if NX2_PLUGIN_API_VERSION >= 5
    const Nx2GameInfo* info = g_api->game_info();
    if (info != nullptr && NX2_HAS(info, Nx2GameInfo, account) && info->account != nullptr) {
        return info->account;
    }
#endif
    return {};
}

// The player's own (solo) saves: the profile's save folder, also while the
// game runs on a multiplayer save.
std::filesystem::path OwnSaveDir() {
    const std::filesystem::path now = GameSaveDir();
    if (!now.empty() && MpSaves::FolderOfSaveDir(now).empty()) {
        return now;
    }
    const Nx2GameInfo* info = g_api->game_info();
    const std::string account = Account();
    if (info == nullptr || !NX2_HAS(info, Nx2GameInfo, profile_dir) || account.empty()) {
        return {};
    }
    const std::filesystem::path profile = PathOf(info->profile_dir);
    return profile.empty() ? profile : profile / L"save" / Wide(account);
}

std::vector<Menu::SaveInfo> SavesOnDisk() {
    std::vector<Menu::SaveInfo> out;
    for (MpSaves::Entry& e : MpSaves::List(MpSaves::Root(g_folder))) {
        Menu::SaveInfo info;
        info.folder = std::move(e.folder);
        info.name = std::move(e.name);
        info.host = std::move(e.host);
        info.received = e.received;
        info.empty = e.empty;
        info.played = MpSaves::DateText(e.played, g_state.lang == Menu::Lang::fr);
        info.place = std::move(e.place);
        out.push_back(std::move(info));
    }
    return out;
}

// Under g_m: the multiplayer saves as they are on disk, the save the game runs
// on, and whether the player has a save of their own to copy.
void LoadSaves() {
    if (!g_state.saves_on) {
        return;
    }
    g_state.running_save = MpSaves::FolderOfSaveDir(GameSaveDir());
    std::error_code ec;
    g_state.solo_save = false;
    const std::filesystem::path own = OwnSaveDir();
    for (std::filesystem::directory_iterator it(own, ec), end; !own.empty() && !ec && it != end;
         it.increment(ec)) {
        std::error_code fec;
        if (std::filesystem::is_regular_file(it->path() / L"progress.sav", fec)) {
            g_state.solo_save = true;
            break;
        }
    }
    Menu::SetSaves(g_state, SavesOnDisk());
}

void SaveSettings();

// Under g_m: what the page asked to be done with a save's files (a new one, a
// copy of the player's own, another name, taken out of the list).
void RunPending() {
    const Menu::PendingOp op = Menu::TakePending(g_state);
    if (op.op == Menu::SaveOp::none) {
        return;
    }
    const std::filesystem::path root = MpSaves::Root(g_folder);
    const std::filesystem::path own = OwnSaveDir();
    const std::filesystem::path in_use = GameSaveDir();
    std::string error;
    std::string folder = op.folder;
    bool ok = false;
    const char* what = "";
    switch (op.op) {
    case Menu::SaveOp::create:
        what = "made (a game to begin)";
        ok = MpSaves::Create(root, op.name, Account(), own, error);
        folder = op.name;
        break;
    case Menu::SaveOp::copy_solo:
        what = "made from the newest save of your own (which is left as it is)";
        ok = MpSaves::CopySolo(root, op.name, own, error);
        folder = op.name;
        break;
    case Menu::SaveOp::rename:
        what = "renamed";
        ok = MpSaves::Rename(root, op.folder, op.name, in_use, error);
        folder = ok ? op.name : op.folder;
        break;
    case Menu::SaveOp::remove:
        what = "taken out of the list (moved to coop-saves\\_deleted)";
        ok = MpSaves::Remove(root, op.folder, in_use, error);
        break;
    default: break;
    }
    if (ok) {
        Log("menu: multiplayer save '%s' %s, in %s", folder.c_str(), what, Utf8(root.wstring()).c_str());
    } else {
        Log("menu: multiplayer save '%s': not done: %s", (op.folder.empty() ? op.name : op.folder).c_str(),
            error.c_str());
    }
    Menu::OpDone(g_state, op, ok, folder, SavesOnDisk());
    SaveSettings();  // the save chosen for hosting may be another now
    g_dirty = true;
}

void LoadSettings() {
    // Under g_m, once.
    g_settings.Parse(ReadText(g_folder / L"settings.json"));
    g_state.name = g_settings.String("name").value_or("");
    if (!Menu::ValidName(g_state.name)) {
        g_state.name = DefaultName();
    }
    g_state.address = g_settings.String("host_address").value_or("");
    if (!g_state.address.empty() && !Menu::ParseJoinAddress(g_state.address)) {
        g_state.address.clear();
    }
    const std::string port = std::to_string(g_settings.Number("port").value_or(Menu::kDefaultPort));
    g_state.port = Menu::ValidPort(port) ? port : std::to_string(Menu::kDefaultPort);
    const auto password = [](const char* plain, const char* kept) {
        std::string value = g_settings.String(plain).value_or("");
        if (value.empty()) {
            if (const auto stored = g_settings.String(kept)) {
                value = Unprotect(*stored);
            }
        }
        return Menu::ValidPassword(value) ? value : std::string();
    };
    g_state.join_password = password("join_password", "join_password_protected");
    g_state.host_password = password("server_password", "server_password_protected");
    // The Steam rows as the game shows a setting it rules out (dimmed, the
    // cursor passes over them): only where update_menu can enable a row again
    // (interface version 4), since the next page uses the same rows for its own
    // fields. OOT_COOP_MENU_DIM=0 or 1 says otherwise, for tests.
    g_script_pad = Environment(L"OOT_COOP_TEST_SCRIPT_PAD") == "1";
    g_debug = Environment(L"OOT_COOP_DEBUG_MENU") == "1";
    const std::string dim = Environment(L"OOT_COOP_MENU_DIM");
    g_state.dim_steam = dim == "1" || (dim != "0" && NX2_HAS(g_api, Nx2PluginApi, world_to_screen));
    const std::string pages = Environment(L"OOT_COOP_MENU_PAGES");
    g_replace_pages = pages == "replace" && NX2_HAS(g_api, Nx2PluginApi, world_to_screen);
    const std::string typing = Environment(L"OOT_COOP_MENU_TYPING");
    g_typing_forced = typing == "pad" ? 1 : typing == "game" ? 2 : 0;
    g_game_typing = NX2_HAS(g_api, Nx2PluginApi, world_to_screen);
    // A keyboard until a gamepad's press is seen (OnPad): the mouse and the
    // keyboard make no gamepad press.
    g_state.game_typing = g_game_typing && g_typing_forced != 1;
    // Multiplayer saves: where the game can be started again on a save folder
    // (interface version 4's restart with NX2NAVI_SAVE_DIR). A host then plays
    // on one of them, never on the player's own. OOT_COOP_MP_SAVES=0 turns
    // them off, for a comparison.
    g_state.saves_on = NX2_HAS(g_api, Nx2PluginApi, restart) && g_api->restart != nullptr &&
                       Environment(L"OOT_COOP_MP_SAVES") != "0";
    g_state.save = g_settings.String("mp_save").value_or("");
    // Over Steam: the name typed for it, and whether Steam can be used at all.
    g_state.steam.own_name = g_settings.String("steam_name").value_or("");
    if (!Menu::ValidName(g_state.steam.own_name)) {
        g_state.steam.own_name.clear();
    }
    SteamNet::Init(g_api, g_log.load());
    g_steam_precheck = SteamNet::Precheck();
    SyncSteam();

    // The game's language, unless the player chose one on the page.
    const Nx2GameInfo* info = g_api->game_info();
    std::string ini;
    if (info != nullptr && info->game_dir != nullptr && *info->game_dir != 0) {
        ini = ReadText(std::filesystem::path(Wide(info->game_dir)) / L"nx2navi.ini");
    }
    wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    // The game's language, with no row for it. "language": "fr" or "en" in the
    // mod's settings.json overrides it (written by hand; "auto" or nothing: the
    // game's); OOT_COOP_LANG=fr|en does for a test.
    const std::string forced = Environment(L"OOT_COOP_LANG");
    const std::string chosen = g_settings.String("language").value_or("");
    Menu::Lang game = Menu::DetectLang(Environment(L"NX2NAVI_LANG"), ini,
                                       Environment(L"NX2NAVI_LOCALE"), Utf8(locale));
#if NX2_PLUGIN_API_VERSION >= 5
    // Interface version 5 says the language the game runs in ("fr", "fr-CA",
    // "en-US", ...): that, not our own reading of the settings.
    if (info != nullptr && NX2_HAS(info, Nx2GameInfo, language) && info->language != nullptr &&
        *info->language != 0) {
        game = Menu::LangFromCode(info->language);
    }
#endif
    const char* game_code = game == Menu::Lang::fr ? "fr" : "en";
    const bool by_file = forced.empty() && (chosen == "fr" || chosen == "en");
    g_state.lang = !forced.empty() ? Menu::LangFromCode(forced) : by_file ? Menu::LangFromCode(chosen) : game;
    Log("menu: language %s (the game's: %s%s%s)", g_state.lang == Menu::Lang::fr ? "French" : "English",
        game_code, !forced.empty() ? ", OOT_COOP_LANG set" : "",
        by_file ? ", \"language\" set in settings.json" : "");
}

bool CopyToClipboard(const std::string& text) {
    const std::wstring wide = Wide(text);
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    bool ok = false;
    EmptyClipboard();
    const HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
    if (memory != nullptr) {
        if (void* at = GlobalLock(memory)) {
            std::memcpy(at, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(memory);
            ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!ok) {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
    return ok;
}

// --- The page ---

// The state's rows as the interface takes them. The strings live in `keep`.
struct Built {
    std::string title;
    std::vector<Menu::Row> keep;
    std::vector<Nx2MenuRow> rows;
    Nx2Menu menu{};
};

void OnEvent(const Nx2MenuEvent* event, void*);

void Build(Built& b) {
    // Under g_m.
    b.title = Menu::Title(g_state);
    b.keep = Menu::Rows(g_state);
    b.rows.resize(b.keep.size());
    for (std::size_t i = 0; i < b.keep.size(); ++i) {
        const Menu::Row& in = b.keep[i];
        Nx2MenuRow& out = b.rows[i];
        out = Nx2MenuRow{};
        out.size = sizeof(Nx2MenuRow);
        // A text row is a button to the game; with text_max the game takes its
        // typing itself (interface version 4; an older game has the field as
        // "reserved" and sends the row's choice to us, as with 0).
        out.kind = in.kind == Menu::RowKind::separator ? NX2_ROW_SEPARATOR
                   : in.kind == Menu::RowKind::text    ? NX2_ROW_TEXT
                                                       : NX2_ROW_ACTION;
        out.label = in.label.c_str();
        out.description = in.description.c_str();
        out.value = in.value.c_str();
        out.disabled = in.disabled ? 1 : 0;
        out.text_max = g_game_typing ? in.text_max : 0;
    }
    b.menu = Nx2Menu{};
    b.menu.size = sizeof(Nx2Menu);
    b.menu.row_count = static_cast<std::uint32_t>(b.rows.size());
    b.menu.title = b.title.c_str();
    b.menu.rows = b.rows.data();
    b.menu.on_event = OnEvent;
    // Interface version 4: where the cursor goes (an older game ignores it and
    // leaves the cursor on the row it was on).
    b.menu.focus_row = g_state.focus;
    g_state.focus = 0;
}

// The game's thread: the open page takes the state's texts.
void Refresh() {
    Built b;
    {
        std::lock_guard lock(g_m);
        if (!g_open) {
            return;
        }
        g_dirty = false;
        Build(b);
    }
    if (g_api->update_menu(&b.menu) != 1) {
        std::lock_guard lock(g_m);
        if (NowMs() < g_replacing_until_ms) {
            g_dirty = true;  // the new page is not up yet: again at a later frame
        } else {
            Log("menu: the game did not take the page's new texts");
        }
        return;
    }
    RefreshCaption();
}

// The game's thread, inside the page's own event: another page of ours over
// the open one. False when the game did not take it (the caller updates the
// open page's rows instead).
bool Replace() {
    Built b;
    {
        std::lock_guard lock(g_m);
        if (!g_open) {
            return false;
        }
        Build(b);
    }
    if (g_api->open_menu(&b.menu) != 1) {
        Log("menu: the game did not open the next page over this one; its rows are updated instead");
        return false;
    }
    std::lock_guard lock(g_m);
    g_dirty = false;
    g_caption_screen = 0;  // the new page's screen is another object
    g_replacing_until_ms = NowMs() + 4000;
    Log("menu: the page is replaced (%s)", b.title.c_str());
    return true;
}

void Capture(bool on) {
    // Under g_m.
    if (on) {
        ++g_captured;
        g_api->capture_input(1);
    } else if (g_captured > 0) {
        --g_captured;
        g_api->capture_input(0);
    }
}

// Typing ends: the text is taken or dropped, the game gets its input back.
void EndEdit(bool take) {
    // Under g_m.
    if (g_state.editing == Menu::Field::none) {
        return;
    }
    if (take) {
        const Menu::Field field = g_state.editing;
        const bool taken = Menu::CommitEdit(g_state);
        if (taken) {
            SaveSettings();
        }
        RunPending();  // a save's name was typed
        if (g_debug && field != Menu::Field::host_password && field != Menu::Field::join_password) {
            Log("menu: typed text %s (name '%s', address '%s', port %s)", taken ? "taken" : "refused",
                g_state.name.c_str(), g_state.address.c_str(), g_state.port.c_str());
        }
    } else {
        Menu::CancelEdit(g_state);
    }
    Capture(false);
    g_dirty = true;
}

void OnFrame(const Nx2Frame*, void*) {
    bool refresh = false;
    bool close = false;
    {
        std::lock_guard lock(g_m);
        if (g_open && NowMs() - g_steam_synced_ms >= 250) {
            // The room, its players, the friends and the invitations as they are now.
            g_steam_synced_ms = NowMs();
            if (SyncSteam() && g_state.editing == Menu::Field::none) {
                g_dirty = true;
            }
            // An invitation accepted in Steam's own window: joined as if chosen here.
            if (const std::string asked = SteamNet::TakeAsked();
                !asked.empty() && !g_request && g_state.editing == Menu::Field::none &&
                (g_state.phase == Menu::Phase::idle || g_state.phase == Menu::Phase::failed)) {
                Request request;
                request.steam = true;
                request.name = Menu::SteamPages::PlayerName(g_state);
                request.room = asked;
                g_state.steam.room = asked;
                g_state.page = Menu::Page::steam_join;
                g_state.phase = Menu::Phase::connecting;
                g_state.message = Menu::T(g_state.lang, Menu::Text::steam_entering);
                Log("menu: an invitation accepted in Steam: joining its room");
                g_request = std::move(request);
                g_dirty = true;
            }
        }
        refresh = g_open && g_dirty;
        if (g_open && g_close_at_ms != 0 && NowMs() >= g_close_at_ms) {
            g_close_at_ms = 0;
            close = true;
        }
    }
    if (refresh) {
        Refresh();
    }
    if (close) {
        g_api->close_menu();
    }
}

int OnKey(const Nx2KeyEvent* event, void*) {
    if (event == nullptr) {
        return 0;
    }
    std::lock_guard lock(g_m);
    if (g_state.editing == Menu::Field::none) {
        return 0;
    }
    g_key_at_ms = NowMs();
    if (event->down) {
        if (event->key == NX2_KEY_ENTER && !event->repeat) {
            EndEdit(true);
        } else if (event->key == NX2_KEY_ESCAPE && !event->repeat) {
            EndEdit(false);
        } else if (event->key == NX2_KEY_BACKSPACE) {
            Menu::EditBackspace(g_state);
            g_dirty = true;
        }
    }
    return 1;  // nothing typed opens the runtime's own menu
}

void OnText(std::uint32_t codepoint, void*) {
    std::lock_guard lock(g_m);
    if (g_state.editing != Menu::Field::none) {
        g_key_at_ms = NowMs();
        Menu::EditText(g_state, codepoint);
        g_dirty = true;
    }
}

// The game's buttons while a text is typed: a keyboard key bound to a button
// comes here too, so nothing is taken right after a key event.
void OnPad(const Nx2PadEvent* event, void*) {
    if (event == nullptr || !event->down) {
        return;
    }
    std::lock_guard lock(g_m);
    const std::uint64_t now = NowMs();
    // Interface version 4 says when the press is a key bound to the button and
    // not a gamepad's (`keyboard`, 0 in an older game): typed keys are text here.
    // Which of the two the player uses decides who takes the typing of the
    // name, address and port rows: the game itself for a keyboard (text_max),
    // we for a gamepad (a gamepad alone cannot type into the game's).
    const bool gamepad = event->keyboard == 0 || g_script_pad;
    const bool game_typing = g_game_typing && (g_typing_forced == 2 || (g_typing_forced == 0 && !gamepad));
    if (g_state.game_typing != game_typing) {
        g_state.game_typing = game_typing;
        if (g_state.editing == Menu::Field::none && g_state.page != Menu::Page::main) {
            g_dirty = true;  // the rows change hands at the next frame
        }
    }
    if (g_state.editing == Menu::Field::none || (event->keyboard != 0 && !g_script_pad) ||
        now - g_edit_at_ms < 250 || now - g_key_at_ms.load() < 300) {
        return;
    }
    switch (event->button) {
    case NX2_PAD_UP: Menu::EditPad(g_state, Menu::PadKey::up); break;
    case NX2_PAD_DOWN: Menu::EditPad(g_state, Menu::PadKey::down); break;
    case NX2_PAD_LEFT: Menu::EditPad(g_state, Menu::PadKey::left); break;
    case NX2_PAD_RIGHT: Menu::EditPad(g_state, Menu::PadKey::right); break;
    case NX2_PAD_A: EndEdit(true); return;
    case NX2_PAD_B: EndEdit(false); return;
    default: return;
    }
    g_dirty = true;
}

void AddPageCallbacks() {
    // Under g_m, on the game's thread.
    if (g_handles[0] > 0) {
        return;
    }
    g_handles[0] = g_api->on_frame(OnFrame, nullptr);
    g_handles[1] = g_api->on_key(OnKey, nullptr);
    g_handles[2] = g_api->on_text(OnText, nullptr);
    g_handles[3] = g_api->on_pad(OnPad, nullptr);
    g_caption_screen = 0;
    const Nx2GameInfo* info = g_api->game_info();
    if (info != nullptr && info->main_base != 0 && info->game_version == 111 && !RuntimeSetsCaption() &&
        NX2_HAS(g_api, Nx2PluginApi, hook_function) && g_api->hook_function != nullptr) {
        g_handles[4] = g_api->hook_function(info->main_base + kCaptionFunction, OnCaption, nullptr);
    }
}

// Under g_m: the handles to take out once the lock is let go (a callback of
// ours may be waiting for it on another thread while the game waits for that
// callback).
std::vector<int> TakePageCallbacks() {
    std::vector<int> out;
    for (int& handle : g_handles) {
        if (handle > 0) {
            out.push_back(handle);
        }
        handle = 0;
    }
    return out;
}

void OnSelect(void*);

void AddTitleEntry() {
    // Under g_m.
    Nx2TitleButton button{};
    button.size = sizeof(button);
    button.label = Menu::T(g_state.lang, Menu::Text::title_entry);
    button.on_select = OnSelect;
    g_title = g_api->add_title_button(&button);
    g_label_stale = false;
}

// The game's thread, inside the game's handling of a row.
void OnEvent(const Nx2MenuEvent* event, void*) {
    if (event == nullptr) {
        return;
    }
    bool refresh = false;
    bool replace = false;
    bool close = false;
    bool relabel = false;
    std::string copy;
    std::string copy_room;      // over Steam: the hosted room's id, never logged
    bool paste_room = false;
    bool steam_start = false;   // a Steam page opened: sign in
    bool steam_idle = false;    // the page closed: sign out unless a session needs Steam
    std::uint64_t invite_id = 0;
    std::vector<int> gone;
    {
        std::lock_guard lock(g_m);
        switch (event->kind) {
        case NX2_MENU_ACTIVATE: {
            const Menu::Page before = g_state.page;
            const Menu::Action action = Menu::Activate(g_state, event->row);
            switch (action) {
            case Menu::Action::begin_edit:
                g_edit_at_ms = NowMs();
                Capture(true);
                break;
            case Menu::Action::start_host:
            case Menu::Action::start_join: {
                Request request;
                request.host = action == Menu::Action::start_host;
                request.name = g_state.name;
                request.port = static_cast<std::uint16_t>(std::strtol(g_state.port.c_str(), nullptr, 10));
                if (request.host) {
                    request.password = g_state.host_password;
                    g_state.phase = Menu::Phase::starting_server;
                    if (g_state.saves_on) {
                        // Never on the player's own saves: the chosen multiplayer one.
                        request.save = g_state.save;
                        const Menu::SaveInfo* chosen = Menu::FindSave(g_state, g_state.save);
                        request.save_empty = chosen != nullptr && chosen->empty;
                    }
                } else {
                    request.server = g_state.address;
                    request.password = g_state.join_password;
                    g_state.phase = Menu::Phase::connecting;
                    g_state.message = Menu::T(g_state.lang, Menu::Text::connecting);
                }
                SaveSettings();
                Log("menu: %s chosen (name '%s', port %s%s%s%s)", request.host ? "Start" : "Join",
                    g_state.name.c_str(), g_state.port.c_str(),
                    request.save.empty() ? "" : ", multiplayer save '", request.save.c_str(),
                    request.save.empty() ? "" : "'");
                g_request = std::move(request);
                break;
            }
            case Menu::Action::steam_open: steam_start = true; break;
            case Menu::Action::start_steam_host:
            case Menu::Action::start_steam_join: {
                Request request;
                request.steam = true;
                request.host = action == Menu::Action::start_steam_host;
                request.name = Menu::SteamPages::PlayerName(g_state);
                if (request.host) {
                    g_state.phase = Menu::Phase::starting_server;
                    if (g_state.saves_on) {
                        request.save = g_state.save;
                        const Menu::SaveInfo* chosen = Menu::FindSave(g_state, g_state.save);
                        request.save_empty = chosen != nullptr && chosen->empty;
                    }
                } else {
                    request.room = g_state.steam.join_room;
                    g_state.phase = Menu::Phase::connecting;
                    g_state.message = Menu::T(g_state.lang, Menu::Text::steam_entering);
                }
                SaveSettings();
                Log("menu: %s over Steam chosen (name '%s'%s%s%s)", request.host ? "Start" : "Join",
                    request.name.c_str(), request.save.empty() ? "" : ", multiplayer save '",
                    request.save.c_str(), request.save.empty() ? "" : "'");
                g_request = std::move(request);
                break;
            }
            case Menu::Action::copy_room: copy_room = g_state.steam.hosted_room; break;
            case Menu::Action::paste_room: paste_room = true; break;
            case Menu::Action::invite_friend: invite_id = g_state.steam.invite_id; break;
            case Menu::Action::copy_address:
                if (!g_state.addresses.empty()) {
                    copy = g_state.addresses[g_state.address_shown % g_state.addresses.size()].address;
                    if (g_state.port != std::to_string(Menu::kDefaultPort)) {
                        copy += ":" + g_state.port;
                    }
                }
                break;
            case Menu::Action::close: close = true; break;
            case Menu::Action::save_chosen:
                SaveSettings();
                Log("menu: multiplayer save '%s' chosen for hosting", g_state.save.c_str());
                break;
            default: break;
            }
            RunPending();
            if (g_state.page == Menu::Page::host && before != Menu::Page::host) {
                const auto local = LocalAddresses();
                g_state.addresses = Menu::JoinAddresses(local);
                g_state.address_shown = 0;
                for (const Menu::LocalAddress& a : local) {
                    Log("menu: this PC has %s on %s%s%s", a.address.c_str(), a.card.c_str(),
                        a.gateway ? ", with a default gateway" : ", no gateway",
                        a.virtual_card ? ", a virtual card" : "");
                }
                std::string order;
                for (const Menu::GiveAddress& g : g_state.addresses) {
                    order += (order.empty() ? "" : ", ") + g.address;
                }
                Log("menu: addresses to give, best first: %s", order.c_str());
            }
            refresh = action != Menu::Action::none && action != Menu::Action::close;
            replace = refresh && g_replace_pages && g_state.page != before;
            break;
        }
        case NX2_MENU_TEXT: {
            // The game's own typing of a text row ended (interface version 4).
            const char* text = NX2_HAS(event, Nx2MenuEvent, text) && event->text != nullptr ? event->text : "";
            const bool taken = Menu::TakeText(g_state, event->row, text);
            if (taken) {
                SaveSettings();
            }
            RunPending();  // a save's name was typed
            if (g_debug) {
                Log("menu: text typed into the game's row %u %s (name '%s', address '%s', port %s)",
                    event->row, taken ? "taken" : "refused", g_state.name.c_str(),
                    g_state.address.c_str(), g_state.port.c_str());
            }
            refresh = true;  // the text as it was taken (or the old one), and the caption
            break;
        }
        case NX2_MENU_FOCUS:
            if (g_debug) {
                Log("menu: cursor on row %u", event->row);
            }
            // A Delete that waited for its second choice is let go.
            refresh = Menu::Focused(g_state, event->row);
            break;
        case NX2_MENU_CLOSED:
            while (g_captured > 0) {
                Capture(false);
            }
            Menu::Closed(g_state);
            g_open = false;
            g_page_shown = false;
            g_close_at_ms = 0;
            gone = TakePageCallbacks();
            steam_idle = !g_request.has_value();
            if (g_label_stale && g_title > 0) {
                // The title entry in the language just chosen.
                gone.push_back(g_title);
                g_title = 0;
                relabel = true;
            }
            Log("menu: page closed");
            break;
        default: break;
        }
    }
    for (const int handle : gone) {
        g_api->remove(handle);
    }
    if (relabel) {
        std::lock_guard lock(g_m);
        AddTitleEntry();
    }
    if (!copy.empty()) {
        const bool ok = CopyToClipboard(copy);
        Log("menu: address %s %s", copy.c_str(), ok ? "copied to the clipboard" : "shown (the clipboard refused it)");
    }
    if (steam_start) {
        SteamNet::Start();
        std::lock_guard lock(g_m);
        SyncSteam();
    }
    if (!copy_room.empty()) {
        const bool ok = CopyToClipboard(copy_room);
        Log("menu: the room ID was %s", ok ? "copied to the clipboard" : "not taken by the clipboard");
        std::lock_guard lock(g_m);
        Menu::SteamPages::Copied(g_state, ok);
    }
    if (paste_room) {
        const std::string text = ClipboardText();
        std::lock_guard lock(g_m);
        Menu::SteamPages::Pasted(g_state, text);
        Log("menu: paste: the clipboard %s", g_state.steam.room.empty() ? "holds no room ID" : "gave a room ID");
    }
    if (invite_id != 0) {
        const bool ok = SteamNet::Invite(invite_id);
        std::lock_guard lock(g_m);
        Menu::SteamPages::Invited(g_state, ok);
    }
    if (steam_idle) {
        SteamNet::StopIfIdle();
    }
    if (replace && Replace()) {
        refresh = false;
    }
    if (refresh) {
        Refresh();
    }
    if (close) {
        g_api->close_menu();
    }
}

// The game's thread: the title entry was chosen.
void OnSelect(void*) {
    Built b;
    {
        std::lock_guard lock(g_m);
        if (g_open) {
            return;
        }
        Menu::Closed(g_state);
        LoadSaves();
        g_steam_precheck = SteamNet::Precheck();
        SyncSteam();
        Build(b);
    }
    const int opened = g_api->open_menu(&b.menu);
    std::lock_guard lock(g_m);
    if (opened == 1) {
        g_open = true;
        g_page_shown = true;
        g_dirty = false;
        g_close_at_ms = 0;
        AddPageCallbacks();
        Log("menu: the Multiplayer page opens (%s)", b.title.c_str());
    } else {
        Log("menu: the game refused the page (the game's log says why)");
    }
}

}  // namespace

bool Register(const Nx2PluginApi* api, const std::filesystem::path& mod_folder,
              void (*log)(const char* line)) {
    std::lock_guard lock(g_m);
    if (g_registered) {
        return true;
    }
    g_log = log;
    if (api == nullptr || !NX2_HAS(api, Nx2PluginApi, close_menu) || api->open_menu == nullptr ||
        api->add_title_button == nullptr || api->on_pad == nullptr) {
        Log("menu: no Multiplayer entry on the title screen: it needs NX2navi's plugin interface "
            "version 3 (this game serves %u); host or join from the launcher's Multiplayer row",
            api != nullptr ? api->version : 0u);
        return false;
    }
    g_api = api;
    g_folder = mod_folder;
    LoadSettings();
    AddTitleEntry();
    if (g_title <= 0) {
        Log("menu: the game refused the title entry (a game version its interface has no title "
            "menu for); host or join from the launcher's Multiplayer row");
        return false;
    }
    g_registered = true;
    Log("menu: '%s' is on the title screen", Menu::T(g_state.lang, Menu::Text::title_entry));
    return true;
}

bool Registered() {
    std::lock_guard lock(g_m);
    return g_registered;
}

std::optional<Request> TakeRequest() {
    std::lock_guard lock(g_m);
    std::optional<Request> out;
    out.swap(g_request);
    return out;
}

void SetPhase(Menu::Phase phase, std::string message, std::uint8_t player) {
    std::lock_guard lock(g_m);
    g_state.phase = phase;
    g_state.message = std::move(message);
    g_state.player = player;
    g_dirty = true;
    if (g_open && (phase == Menu::Phase::hosting || phase == Menu::Phase::joined)) {
        g_close_at_ms = NowMs() + 1500;  // the word is read, then the title screen is back
    }
}

void Close() {
    const Nx2PluginApi* api = nullptr;
    {
        std::lock_guard lock(g_m);
        if (!g_open) {
            return;
        }
        api = g_api;
    }
    api->close_menu();
}

bool PageOpen() {
    return g_page_shown.load(std::memory_order_relaxed);
}

Menu::Lang Language() {
    std::lock_guard lock(g_m);
    return g_state.lang;
}

void Shutdown() {
    std::vector<int> gone;
    {
        std::lock_guard lock(g_m);
        if (!g_registered) {
            return;
        }
        while (g_captured > 0) {
            Capture(false);
        }
        gone = TakePageCallbacks();
        if (g_title > 0) {
            gone.push_back(g_title);
            g_title = 0;
        }
        g_registered = false;
        g_open = false;
        g_page_shown = false;
    }
    for (const int handle : gone) {
        g_api->remove(handle);
    }
}

std::string Protect(const std::string& secret) {
    DATA_BLOB in{static_cast<DWORD>(secret.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"OoT Co-op join password", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    DWORD size = 0;
    std::string text;
    if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                             nullptr, &size)) {
        text.resize(size);
        if (CryptBinaryToStringA(out.pbData, out.cbData, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                                 text.data(), &size)) {
            text.resize(size);
        } else {
            text.clear();
        }
    }
    LocalFree(out.pbData);
    return text;
}

std::string Unprotect(const std::string& stored) {
    DWORD size = 0;
    if (stored.empty() || !CryptStringToBinaryA(stored.c_str(), static_cast<DWORD>(stored.size()),
                                                CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr)) {
        return {};
    }
    std::vector<BYTE> bytes(size);
    if (!CryptStringToBinaryA(stored.c_str(), static_cast<DWORD>(stored.size()), CRYPT_STRING_BASE64,
                              bytes.data(), &size, nullptr, nullptr)) {
        return {};
    }
    DATA_BLOB in{size, bytes.data()};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    std::string secret(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return secret;
}

}  // namespace CoopNx2navi::TitleMenu
