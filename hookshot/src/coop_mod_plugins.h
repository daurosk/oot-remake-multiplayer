// The code mods' host (docs/mods.md "Writing a code mod"): loads each code
// mod's library at the start, in the Mods tab's order, hands it its
// CoopModApi table (coop_mod_api.h) and calls what it registered at the
// game's frame, overlay, key, setting, hook and menu screen points. A mod that fails to
// load, refuses to start or crashes in a call is switched off with one line;
// the game and the other mods go on.
//
// Threads: Start and Stop once, on the module's start and stop. Frame, Overlay,
// Key, Hook and Screen from the game's threads at the same time: the callback
// lists are fixed when Start returns, so they are read without a lock. Screen
// runs one update at a time (an update that comes while another thread is in
// one is skipped for the mods).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
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
    // (API v2) What a mod may do inside its on_screen call, on that thread, for the screen being updated (ctx and
    // screen as Screen was given them). Each true when the game took it.
    std::function<bool(void* ctx, std::uint64_t screen)> choose_focused;
    std::function<bool()> reload_save_list;
    std::function<bool(void* ctx, std::uint64_t screen, const std::string& name)> play_sound;
    std::function<std::uint64_t()> now_ms;  // a steady clock (the input hold's end)
};

// (API v2) One menu screen's update, as the runtime read it.
struct ScreenUpdate {
    void* ctx = nullptr;          // the game thread's context (the runtime's own: never shown to a mod)
    std::uint64_t screen = 0;     // the screen object (likewise)
    std::uint32_t kind = COOP_MOD_SCREEN_OTHER;
    std::string focused;          // the focused button's name, "" none
    bool save_list_ready = false;
    std::uint64_t buttons = 0;    // player 1's buttons before any hold
    bool focused_ready = false;   // the focused button takes a choice now
};

// A hold ends when the menu screen it was set on has not updated for this long.
inline constexpr std::uint64_t kHoldLapseMs = 500;

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

    // (API v2) Whether any mod running now asked for menu screens (the runtime reads a screen only then).
    bool WantsScreens() const;
    // (API v2) Whether a mod running now is a save screen's file select (its mod.json says "file_select": true) and
    // asked for the menu screens. Another menu-screen mod is not one.
    bool FileSelectRuns() const;
    // (API v2) A menu screen updates: each mod's on_screen, with the calls only it may make. Skipped when another
    // thread is in one.
    void Screen(const ScreenUpdate& update);
    // (API v2) The buttons the mods hold from the game now (COOP_MOD_HOLD_ALL: the sticks too), 0 none. A hold whose mod
    // is off, or whose screen (the one it was set on) has not updated for kHoldLapseMs, ends here (with a line).
    std::uint64_t HeldButtons();

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
    std::mutex screen_mutex_;                    // one screen update at a time
};

// Runs fn(arg); false when it crashed (an access violation or an exception leaving it). Without structured
// exceptions (not MSVC) only C++ exceptions are caught.
bool Guarded(void (*fn)(void*), void* arg);

}  // namespace CoopModPlugins
