// coop_mods_runtime.h: the module's one Loader and Host.
#include "coop_mods_runtime.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <monocypher.h>

#include "coop_mod_plugins.h"
#include "coop_mod_share.h"
#include "coop_rando.h"
#include "coop_runtime.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace CoopModsRuntime {

namespace {

std::unique_ptr<CoopMods::Loader> g_loader;
std::unique_ptr<CoopModPlugins::Host> g_host;
std::unique_ptr<CoopRando::Rando> g_rando;  // the built-in randomizer (coop_rando.h)
std::atomic<bool> g_pending{false};  // the code mods start at the next frame
std::atomic<bool> g_running{false};
std::atomic<std::uint64_t> g_hooks{0};
// Co-op's list of our mods (coop_mod_share.h): made by g_hasher, read by the present thread.
std::mutex g_mine_mutex;
std::optional<coop::protocol::ModList> g_mine;
std::thread g_hasher;
std::atomic<bool> g_stop_hashing{false};

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? v : "";
}

// As Hookshot's host/mods.c finds it.
std::filesystem::path ModsRoot() {
    const std::string env = Env("HOS_MODS");
    if (!env.empty()) {
        return CoopMods::FromUtf8(env);
    }
    return CoopRuntime::GameDir() / "mods";
}

unsigned long ThisProcess() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// Whether another game with that id still runs (its coop-mods.starting is then not a crash).
bool ProcessRuns(unsigned long pid) {
#ifdef _WIN32
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (p == nullptr) {
        return false;
    }
    const bool runs = WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
    CloseHandle(p);
    return runs;
#else
    (void)pid;
    return false;
#endif
}

// OOTRR is stopped when the game closes, or after this long (it takes about a minute).
constexpr std::chrono::minutes kOotrrLimit{10};
std::atomic<bool> g_stop_ootrr{false};

// The draw table a mod gets: the parts of Hookshot's it may use.
struct DrawUser {
    const HosCoopDraw* d;
};
void DrawRect(void* user, float x0, float y0, float x1, float y1, std::uint32_t rgba, float rounding) {
    const HosCoopDraw* d = static_cast<DrawUser*>(user)->d;
    if (d->rect) d->rect(d->user, x0, y0, x1, y1, rgba, rounding);
}
void DrawText(void* user, float x, float y, float px, std::uint32_t rgba, const char* utf8) {
    const HosCoopDraw* d = static_cast<DrawUser*>(user)->d;
    if (d->text && utf8 != nullptr) d->text(d->user, x, y, px, rgba, utf8);
}
float DrawTextWidth(void* user, float px, const char* utf8) {
    const HosCoopDraw* d = static_cast<DrawUser*>(user)->d;
    return d->text_width && utf8 != nullptr ? d->text_width(d->user, px, utf8) : 0.0f;
}

// The mods every player needs alike: on in this run (its game files indexed, or its code to be loaded), not "own".
std::vector<CoopMods::Mod> SharedMods() {
    std::vector<CoopMods::Mod> out;
    const std::vector<CoopMods::Mod> mods = g_loader->Mods();
    for (const CoopMods::Row& row : g_loader->Rows()) {
        if (!(row.on_now || (row.has_code && row.on))) {
            continue;
        }
        for (const CoopMods::Mod& m : mods) {
            if (m.name == row.name && !m.internal && m.manifest.share == CoopMods::Share::Everyone) {
                out.push_back(m);
            }
        }
    }
    return out;
}

// One hash for a mod's game files and its code.
coop::protocol::ModHash Combined(const CoopMods::Hashes& h) {
    coop::protocol::ModHash out{};
    if (h.code == CoopMods::kNoHash || h.game_files == CoopMods::kNoHash) {
        const CoopMods::Hash& one = h.code == CoopMods::kNoHash ? h.game_files : h.code;
        std::copy(one.begin(), one.end(), out.begin());
        return out;
    }
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, out.size());
    crypto_blake2b_update(&ctx, h.game_files.data(), h.game_files.size());
    crypto_blake2b_update(&ctx, h.code.data(), h.code.size());
    crypto_blake2b_final(&ctx, out.data());
    return out;
}

void Hash(std::vector<CoopMods::Mod> mods) {
    coop::protocol::ModList list;
    const std::function<bool()> stop = [] { return g_stop_hashing.load(); };
    for (const CoopMods::Mod& m : mods) {
        if (list.mods.size() == coop::protocol::kMaxModEntries) {
            CoopRuntime::Log("coop: mods: more than " + std::to_string(coop::protocol::kMaxModEntries) +
                             " shared mods: the rest are not compared with the other players'");
            break;
        }
        const auto h = CoopMods::HashMod(m, &stop);
        if (!h) {
            return;  // stopped
        }
        list.mods.push_back({m.manifest.id, m.manifest.version.substr(0, coop::protocol::kMaxModVersionChars),
                             Combined(*h)});
    }
    CoopRuntime::Log("coop: mods: " + std::to_string(list.mods.size()) + " shared mods ready for co-op");
    const std::lock_guard lock(g_mine_mutex);
    g_mine = std::move(list);
}

void StartSharing() {
    CoopModShare::Provider p;
    p.mine = []() -> std::optional<coop::protocol::ModList> {
        const std::lock_guard lock(g_mine_mutex);
        return g_mine;
    };
    p.shared = [] {
        std::vector<coop::protocol::ModSetting> out;
        for (CoopMods::SharedValue& v : g_loader->SharedValues()) {
            if (v.value.size() > coop::protocol::kMaxModSettingValueChars) {
                continue;  // too long to send: each player keeps its own (docs/mods.md)
            }
            out.push_back({std::move(v.id), std::move(v.key), std::move(v.value)});
        }
        return out;
    };
    p.host_values = [](std::optional<std::vector<coop::protocol::ModSetting>> values) {
        if (!values) {
            g_loader->SetHostValues(std::nullopt);
            if (g_rando) g_rando->Recheck();  // a host without the randomizer changes no value of ours
            return;
        }
        std::vector<CoopMods::SharedValue> in;
        for (auto& v : *values) {
            in.push_back({std::move(v.mod_id), std::move(v.key), std::move(v.value)});
        }
        g_loader->SetHostValues(std::move(in));
        if (g_rando) g_rando->Recheck();
    };
    p.notes = [] { return g_rando ? g_rando->TakeLines() : std::vector<std::string>{}; };
    CoopModShare::SetProvider(std::move(p));
    g_hasher = std::thread(Hash, SharedMods());
}

}  // namespace

void Start() {
    if (g_loader) {
        return;
    }
    g_loader = std::make_unique<CoopMods::Loader>(
        ModsRoot(), CoopMods::Loader::Hooks{[](const std::string& l) { CoopRuntime::Log("coop: mods: " + l); },
                                            ThisProcess(), ProcessRuns});
    g_loader->Start();
    CoopRando::Rando::Hooks rando;
    rando.log = [](const std::string& l) { CoopRuntime::Log("coop: mods: " + l); };
    rando.game_dir = CoopRuntime::GameDir();
    rando.run = [](const std::wstring& line, const std::filesystem::path& log) {
        return CoopRando::RunProgram(line, log, kOotrrLimit, &g_stop_ootrr);
    };
    rando.entropy = [] {
        return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) * 31 +
               ThisProcess();
    };
    g_rando = std::make_unique<CoopRando::Rando>(*g_loader, std::move(rando));
    g_rando->Start();  // before the list for co-op is made: it may switch seeds
    StartSharing();
    if (Env("HOS_COOP_MODS_CODE") == "0") {
        CoopRuntime::Log("coop: mods: code mods not loaded (HOS_COOP_MODS_CODE=0)");
        return;
    }
    if (g_loader->CodeToLoad().empty()) {
        return;
    }
    CoopModPlugins::Game game;
    game.log = [](const std::string& l) { CoopRuntime::Log("coop: " + l); };
    game.main_base = [] { return CoopRuntime::MainBase(); };
    game.read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
    game.write = [](std::uint64_t a, const void* in, std::size_t n) { return CoopRuntime::Write(a, in, n); };
    game.has_hook = [](std::uint32_t id) { return id < 64 && (g_hooks.load() >> id & 1) != 0; };
    game.runtime = "hookshot";
    g_host = std::make_unique<CoopModPlugins::Host>(*g_loader, CoopModPlugins::SystemLibrary(), std::move(game));
    g_pending = true;  // started at the first frame: the exe has told its hooks by then
}

void Stop() {
    g_stop_hashing = true;
    if (g_hasher.joinable()) {
        g_hasher.join();
    }
    g_pending = false;
    g_running = false;
    if (g_rando) {
        g_stop_ootrr = true;  // an OOTRR that runs now is stopped (the seed is made again at the next start)
        g_rando->Wait();
    }
    if (g_host) {
        g_host->Stop();  // the host and the loader stay: a game thread may still be in a call
    }
}

CoopMods::Loader* Loader() {
    return g_loader.get();
}

CoopRando::Rando* Rando() {
    return g_rando.get();
}

void Frame() {
    if (g_pending.exchange(false)) {
        g_host->Start();
        g_running = true;
    }
    if (g_running) {
        g_host->Frame();
    }
}

bool Key(std::uint32_t vk, bool repeat) {
    return g_running && g_host->Key(vk, true, repeat);
}

bool OverlayWanted() {
    return g_running && g_host->WantsOverlay();
}

void Overlay(const HosCoopDraw& d) {
    if (!g_running) {
        return;
    }
    DrawUser user{&d};
    CoopModDraw t{};
    t.width = d.width;
    t.height = d.height;
    t.user = &user;
    t.rect = DrawRect;
    t.text = DrawText;
    t.text_width = DrawTextWidth;
    g_host->Overlay(t);
}

void SetHooks(std::uint64_t bits) {
    g_hooks = bits;
}

int Hook(std::uint32_t id, int phase, std::uint64_t* x) {
    return g_running ? g_host->Hook(id, phase, x) : 0;
}

bool Hooked(std::uint32_t id) {
    return g_running && g_host->Hooks(id);
}

}  // namespace CoopModsRuntime
