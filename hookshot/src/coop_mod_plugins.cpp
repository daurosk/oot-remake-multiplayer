// coop_mod_plugins.h: each mod gets its own table; the table's calls find the
// mod from `self`; every call into a mod goes through Guarded.
#include "coop_mod_plugins.h"

#include <cstring>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace CoopModPlugins {

namespace {

template <typename Fn>
struct Callback {
    Fn fn;
    void* user;
};

}  // namespace

struct Host::Slot {
    // The table a mod gets, with the way back to its slot. Standard layout, so `self` is the Table.
    struct Table {
        CoopModApi api;
        Slot* slot;
    };

    Table table{};
    CoopMods::Loader* loader = nullptr;
    const Game* game = nullptr;
    std::string name;  // the mod's folder name (the Loader's key)
    std::string id;
    std::string folder;
    void* handle = nullptr;
    coop_mod_stop_fn stop = nullptr;
    std::atomic<bool> on{false};
    bool starting = false;  // registration is allowed (only inside coop_mod_start)
    std::vector<Callback<coop_mod_frame_fn>> frame;
    std::vector<Callback<coop_mod_overlay_fn>> overlay;
    std::vector<Callback<coop_mod_key_fn>> key;
    std::vector<Callback<coop_mod_setting_fn>> setting;
    std::vector<std::pair<std::uint32_t, Callback<coop_mod_hook_fn>>> hook;

    static Slot* Of(const CoopModApi* self) {
        return self == nullptr ? nullptr : reinterpret_cast<const Table*>(self)->slot;
    }
    void Say(const std::string& line) const {
        if (game->log) {
            game->log("mod " + id + ": " + line);
        }
    }
    // A call into the mod crashed: off for this run, and the tab says so.
    void Crashed(const char* where) {
        if (on.exchange(false)) {
            const std::string problem = std::string("crashed in its ") + where + ": switched off until the next start";
            Say(problem);
            loader->NoteCode(name, problem);
        }
    }
};

namespace {

using Slot = Host::Slot;
static_assert(std::is_standard_layout_v<Slot::Table>);

// ---- the calls into a mod, each through Guarded ----

struct FrameCall {
    coop_mod_frame_fn fn;
    void* user;
};
void RunFrame(void* p) {
    auto* c = static_cast<FrameCall*>(p);
    c->fn(c->user);
}

struct OverlayCall {
    coop_mod_overlay_fn fn;
    void* user;
    const CoopModDraw* draw;
};
void RunOverlay(void* p) {
    auto* c = static_cast<OverlayCall*>(p);
    c->fn(c->user, c->draw);
}

struct KeyCall {
    coop_mod_key_fn fn;
    void* user;
    std::uint32_t vk;
    int down, repeat, taken;
};
void RunKey(void* p) {
    auto* c = static_cast<KeyCall*>(p);
    c->taken = c->fn(c->user, c->vk, c->down, c->repeat);
}

struct SettingCall {
    coop_mod_setting_fn fn;
    void* user;
    const char* key;
    const char* value;
};
void RunSetting(void* p) {
    auto* c = static_cast<SettingCall*>(p);
    c->fn(c->user, c->key, c->value);
}

struct HookCall {
    coop_mod_hook_fn fn;
    void* user;
    std::uint32_t hook;
    int phase;
    std::uint64_t* x;
    int result;
};
void RunHook(void* p) {
    auto* c = static_cast<HookCall*>(p);
    c->result = c->fn(c->user, c->hook, c->phase, c->x);
}

struct StartCall {
    coop_mod_start_fn fn;
    const CoopModApi* api;
    int result;
};
void RunStart(void* p) {
    auto* c = static_cast<StartCall*>(p);
    c->result = c->fn(c->api);
}

struct OpenCall {  // the library's own start (DllMain, static constructors) runs in it
    const Library* library;
    const std::filesystem::path* file;
    std::string* problem;
    void* handle;
};
void RunOpen(void* p) {
    auto* c = static_cast<OpenCall*>(p);
    c->handle = c->library->open(*c->file, c->problem);
}

struct StopCall {
    coop_mod_stop_fn fn;
};
void RunStop(void* p) {
    static_cast<StopCall*>(p)->fn();
}

// ---- the table's calls ----

void ApiLog(const CoopModApi* self, const char* line) {
    if (Slot* s = Slot::Of(self); s != nullptr && line != nullptr) {
        s->Say(line);
    }
}

const Game* g_game_for_memory = nullptr;  // read/write/main_base/has_hook take no `self`: the running host's game

std::uint64_t ApiMainBase() {
    const Game* g = g_game_for_memory;
    return g != nullptr && g->main_base ? g->main_base() : 0;
}
int ApiRead(std::uint64_t address, void* out, std::size_t size) {
    const Game* g = g_game_for_memory;
    return g != nullptr && g->read && out != nullptr && g->read(address, out, size) ? 1 : 0;
}
int ApiWrite(std::uint64_t address, const void* in, std::size_t size) {
    const Game* g = g_game_for_memory;
    return g != nullptr && g->write && in != nullptr && g->write(address, in, size) ? 1 : 0;
}
int ApiHasHook(std::uint32_t hook) {
    const Game* g = g_game_for_memory;
    return g != nullptr && g->has_hook && g->has_hook(hook) ? 1 : 0;
}

int ApiSetting(const CoopModApi* self, const char* key, char* out, std::size_t cap) {
    Slot* s = Slot::Of(self);
    if (s == nullptr || key == nullptr) {
        return -1;
    }
    for (const CoopMods::SettingRow& row : s->loader->Settings(s->name)) {
        if (row.setting.key == key) {
            if (out != nullptr && cap > 0) {
                const std::size_t n = row.value.size() < cap - 1 ? row.value.size() : cap - 1;
                std::memcpy(out, row.value.data(), n);
                out[n] = 0;
            }
            return static_cast<int>(row.value.size());
        }
    }
    return -1;
}

// Registration: only inside the mod's own coop_mod_start.
Slot* Registering(const CoopModApi* self, const void* fn, const char* what) {
    Slot* s = Slot::Of(self);
    if (s == nullptr || fn == nullptr) {
        return nullptr;
    }
    if (!s->starting) {
        s->Say(std::string("asked for a ") + what + " callback outside coop_mod_start: refused");
        return nullptr;
    }
    return s;
}

int ApiOnFrame(const CoopModApi* self, coop_mod_frame_fn fn, void* user) {
    Slot* s = Registering(self, reinterpret_cast<const void*>(fn), "frame");
    return s != nullptr ? (s->frame.push_back({fn, user}), 1) : 0;
}
int ApiOnOverlay(const CoopModApi* self, coop_mod_overlay_fn fn, void* user) {
    Slot* s = Registering(self, reinterpret_cast<const void*>(fn), "overlay");
    return s != nullptr ? (s->overlay.push_back({fn, user}), 1) : 0;
}
int ApiOnKey(const CoopModApi* self, coop_mod_key_fn fn, void* user) {
    Slot* s = Registering(self, reinterpret_cast<const void*>(fn), "key");
    return s != nullptr ? (s->key.push_back({fn, user}), 1) : 0;
}
int ApiOnSetting(const CoopModApi* self, coop_mod_setting_fn fn, void* user) {
    Slot* s = Registering(self, reinterpret_cast<const void*>(fn), "setting");
    return s != nullptr ? (s->setting.push_back({fn, user}), 1) : 0;
}
int ApiHook(const CoopModApi* self, std::uint32_t hook, coop_mod_hook_fn fn, void* user) {
    Slot* s = Registering(self, reinterpret_cast<const void*>(fn), "hook");
    if (s == nullptr) {
        return 0;
    }
    if (!ApiHasHook(hook)) {
        s->Say("asked for hook " + std::to_string(hook) + ", which this game build does not have");
        return 0;
    }
    s->hook.push_back({hook, {fn, user}});
    return 1;
}

void ApiRestartNeeded(const CoopModApi* self, const char* reason) {
    if (Slot* s = Slot::Of(self); s != nullptr) {
        s->loader->NoteChanged(s->name, reason != nullptr && *reason ? reason : s->id + " changed its files");
    }
}

}  // namespace

// ---- the system's libraries ----

Library SystemLibrary() {
    Library lib;
#ifdef _WIN32
    lib.open = [](const std::filesystem::path& file, std::string* problem) -> void* {
        // The mod's folder is searched for the libraries it needs itself.
        HMODULE h = LoadLibraryExW(file.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (h == nullptr && problem != nullptr) {
            *problem = "Windows could not load it (error " + std::to_string(GetLastError()) +
                       "; 126 or 193: a library it needs is missing, or it is not a 64-bit Windows library)";
        }
        return h;
    };
    lib.symbol = [](void* handle, const char* name) -> void* {
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
    };
#else
    lib.open = [](const std::filesystem::path& file, std::string* problem) -> void* {
        void* h = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (h == nullptr && problem != nullptr) {
            const char* e = dlerror();
            *problem = std::string("could not load it: ") + (e != nullptr ? e : "unknown error");
        }
        return h;
    };
    lib.symbol = [](void* handle, const char* name) -> void* { return dlsym(handle, name); };
#endif
    return lib;
}

// ---- the host ----

Host::Host(CoopMods::Loader& loader, Library library, Game game)
    : loader_(loader), library_(std::move(library)), game_(std::move(game)) {}

Host::~Host() {
    Stop();
    if (g_game_for_memory == &game_) {
        g_game_for_memory = nullptr;
    }
}

void Host::Start() {
    if (started_.exchange(true)) {
        return;
    }
    g_game_for_memory = &game_;
    loader_.OnSetting([this](const std::string& id, const std::string& key, const std::string& value) {
        if (stopped_) {
            return;
        }
        for (auto& s : slots_) {
            if (s->id != id || !s->on) {
                continue;
            }
            for (const auto& cb : s->setting) {
                SettingCall c{cb.fn, cb.user, key.c_str(), value.c_str()};
                if (!Guarded(RunSetting, &c)) {
                    s->Crashed("setting callback");
                    break;
                }
            }
        }
    });
    for (const CoopMods::Mod& m : loader_.CodeToLoad()) {
        auto s = std::make_unique<Slot>();
        s->loader = &loader_;
        s->game = &game_;
        s->name = m.name;
        s->id = m.manifest.id;
        s->folder = CoopMods::ToUtf8(m.folder);
        // From before its library opens to the end of its start: a crash the guard cannot catch (a stack overflow,
        // a fast fail, its own thread) or a hang leaves coop-mods.starting behind, and the next start skips it.
        loader_.CodeStarting(m.name);
        Begin(*s, m);
        loader_.CodeStarted();
        slots_.push_back(std::move(s));  // a library that opened stays: its code may have started a thread
    }
}

void Host::Begin(Slot& s, const CoopMods::Mod& m) {
    std::string problem;
    if (library_.open) {
        OpenCall open{&library_, &m.code, &problem, nullptr};
        if (!Guarded(RunOpen, &open)) {
            problem = "crashed while its code was loaded";
        }
        s.handle = open.handle;
    }
    const auto start = s.handle == nullptr
                           ? nullptr
                           : reinterpret_cast<coop_mod_start_fn>(library_.symbol(s.handle, COOP_MOD_START_EXPORT));
    if (s.handle == nullptr || start == nullptr) {
        if (s.handle == nullptr && problem.empty()) {
            problem = "its code could not be loaded";
        } else if (s.handle != nullptr) {
            problem = "its code has no " COOP_MOD_START_EXPORT;
        }
        s.Say(problem);
        loader_.NoteCode(m.name, problem);
        return;
    }
    s.stop = reinterpret_cast<coop_mod_stop_fn>(library_.symbol(s.handle, COOP_MOD_STOP_EXPORT));
    CoopModApi& api = s.table.api;
    s.table.slot = &s;
    api.size = sizeof(CoopModApi);
    api.version = COOP_MOD_API_VERSION;
    api.mod_id = s.id.c_str();
    api.mod_folder = s.folder.c_str();
    api.runtime = game_.runtime.c_str();
    api.log = ApiLog;
    api.main_base = ApiMainBase;
    api.read = ApiRead;
    api.write = ApiWrite;
    api.setting = ApiSetting;
    api.on_frame = ApiOnFrame;
    api.on_overlay = ApiOnOverlay;
    api.on_key = ApiOnKey;
    api.on_setting = ApiOnSetting;
    api.hook = ApiHook;
    api.has_hook = ApiHasHook;
    api.restart_needed = ApiRestartNeeded;

    s.starting = true;
    s.on = true;
    StartCall c{start, &api, 0};
    const bool ran = Guarded(RunStart, &c);
    s.starting = false;
    if (!ran || c.result != 1) {
        problem = ran ? "its " COOP_MOD_START_EXPORT " said no (its own line above says why)"
                      : "crashed in its " COOP_MOD_START_EXPORT;
        s.on = false;
        s.frame.clear();
        s.overlay.clear();
        s.key.clear();
        s.setting.clear();
        s.hook.clear();
        s.stop = nullptr;
        s.Say(problem);
        loader_.NoteCode(m.name, problem);
        return;
    }
    s.Say("started (" +
          std::to_string(s.frame.size() + s.overlay.size() + s.key.size() + s.setting.size() + s.hook.size()) +
          " callbacks)");
    loader_.NoteCode(m.name, "");
}

void Host::Stop() {
    if (!started_ || stopped_.exchange(true)) {
        return;
    }
    for (auto it = slots_.rbegin(); it != slots_.rend(); ++it) {
        Slot& s = **it;
        if (s.on.exchange(false) && s.stop != nullptr) {
            StopCall c{s.stop};
            if (!Guarded(RunStop, &c)) {
                s.Say("crashed in its " COOP_MOD_STOP_EXPORT);
            }
        }
    }
}

void Host::Frame() {
    if (!started_ || stopped_) {
        return;
    }
    for (auto& s : slots_) {
        for (const auto& cb : s->frame) {
            if (!s->on) {
                break;
            }
            FrameCall c{cb.fn, cb.user};
            if (!Guarded(RunFrame, &c)) {
                s->Crashed("frame callback");
            }
        }
    }
}

bool Host::WantsOverlay() const {
    if (!started_ || stopped_) {
        return false;
    }
    for (const auto& s : slots_) {
        if (s->on && !s->overlay.empty()) {
            return true;
        }
    }
    return false;
}

void Host::Overlay(const CoopModDraw& draw) {
    if (!started_ || stopped_) {
        return;
    }
    for (auto& s : slots_) {
        for (const auto& cb : s->overlay) {
            if (!s->on) {
                break;
            }
            OverlayCall c{cb.fn, cb.user, &draw};
            if (!Guarded(RunOverlay, &c)) {
                s->Crashed("overlay callback");
            }
        }
    }
}

bool Host::Key(std::uint32_t vk, bool down, bool repeat) {
    if (!started_ || stopped_) {
        return false;
    }
    for (auto& s : slots_) {
        for (const auto& cb : s->key) {
            if (!s->on) {
                break;
            }
            KeyCall c{cb.fn, cb.user, vk, down ? 1 : 0, repeat ? 1 : 0, 0};
            if (!Guarded(RunKey, &c)) {
                s->Crashed("key callback");
            } else if (c.taken == 1) {
                return true;
            }
        }
    }
    return false;
}

int Host::Hook(std::uint32_t hook, int phase, std::uint64_t* x) {
    if (!started_ || stopped_ || x == nullptr) {
        return 0;
    }
    int done = 0;
    for (auto& s : slots_) {
        for (const auto& [id, cb] : s->hook) {
            if (!s->on) {
                break;
            }
            if (id != hook) {
                continue;
            }
            HookCall c{cb.fn, cb.user, hook, phase, x, 0};
            if (!Guarded(RunHook, &c)) {
                s->Crashed("hook");
            } else if (phase == 0 && c.result == 1) {
                done = 1;  // the later mods still see phase 0: they may watch the call
            }
        }
    }
    return done;
}

bool Host::Hooks(std::uint32_t hook) const {
    for (const auto& s : slots_) {
        if (!s->on) {
            continue;
        }
        for (const auto& [id, cb] : s->hook) {
            if (id == hook) {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::string> Host::Running() const {
    std::vector<std::string> out;
    for (const auto& s : slots_) {
        if (s->on) {
            out.push_back(s->id);
        }
    }
    return out;
}

}  // namespace CoopModPlugins
