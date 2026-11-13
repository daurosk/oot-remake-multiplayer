// The code mods' host (docs/mods.md "Writing a code mod"): loads each code
// mod's library at the start, in the Mods tab's order, hands it its
// CoopModApi table (coop_mod_api.h) and calls what it registered at the
// game's frame, overlay, key, setting and hook points. A mod that fails to
// load, refuses to start or crashes in a call is switched off with one line;
// the game and the other mods go on.
//
// Threads: Start and Stop once, on the module's start and stop. Frame, Overlay,
// Key and Hook from the game's threads at the same time: the callback lists
// are fixed when Start returns, so they are read without a lock.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "coop_mod_api.h"
#include "coop_mods.h"

namespace CoopModPlugins {

// How a code file is opened: the system's libraries in the game, fakes in the tests.
struct Library {
    // A handle, or nullptr with `problem` (plain English).
    std::function<void*(const std::filesystem::path& file, std::string* problem)> open;
    std::function<void*(void* handle, const char* name)> symbol;
};
Library SystemLibrary();

// What the mods reach of the game.
struct Game {
    std::function<void(const std::string&)> log;  // one line; the caller adds "coop: "
    std::function<std::uint64_t()> main_base;
    std::function<bool(std::uint64_t, void*, std::size_t)> read;
    std::function<bool(std::uint64_t, const void*, std::size_t)> write;
    std::function<bool(std::uint32_t)> has_hook;  // this run's exe has that hook (wrap) id
    std::string runtime = "hookshot";
};

class Host {
public:
    Host(CoopMods::Loader& loader, Library library, Game game);
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    // Loads and starts the code mods the loader names (Loader::CodeToLoad) and tells it what became of each
    // (Loader::NoteCode). Once.
    void Start();
    // coop_mod_stop of each mod still on, last loaded first; no callback runs after it.
    void Stop();

    void Frame();
    bool WantsOverlay() const;
    void Overlay(const CoopModDraw& draw);
    // True when a mod took the key.
    bool Key(std::uint32_t vk, bool down, bool repeat);
    // A hooked game function (coop_mod_api.h coop_mod_hook_fn); 1 when a mod did the work at phase 0.
    int Hook(std::uint32_t hook, int phase, std::uint64_t* x);
    // Whether any mod hooks that id (the module turns the wrap on only then).
    bool Hooks(std::uint32_t hook) const;

    // The ids of the mods running now, in load order.
    std::vector<std::string> Running() const;

    struct Slot;  // one loaded mod (coop_mod_plugins.cpp)

private:
    // Opens and starts one mod and tells the loader what became of it; false when its library crashed while it was
    // opened (no further library is opened then).
    bool Begin(Slot& s, const CoopMods::Mod& m);

    CoopMods::Loader& loader_;
    const Library library_;
    const Game game_;
    std::vector<std::unique_ptr<Slot>> slots_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};
};

// Runs fn(arg); false when it crashed (an access violation or an exception leaving it). Without structured
// exceptions (not MSVC) only C++ exceptions are caught.
bool Guarded(void (*fn)(void*), void* arg);

}  // namespace CoopModPlugins
