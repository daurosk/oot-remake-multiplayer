// SPDX-License-Identifier: GPL-3.0-or-later
// The code mods' host (hookshot/src/coop_mod_plugins.*, coop_mod_api.h) with
// fake libraries: load order, refusals, the table's calls, registration only
// at the start, keys taken, hooks, settings, crashes switched off.
#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "coop_mod_plugins.h"
#include "coop_mods.h"

namespace fs = std::filesystem;
using namespace CoopModPlugins;

namespace {

void Put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

// What the fake mods saw and did.
struct Seen {
    std::vector<std::string> calls;
    const CoopModApi* api[4] = {};
    int take_key = 0;
    int skip_hook = 0;
    std::uint64_t memory[4] = {1, 2, 3, 4};
    std::string marker;  // coop-mods.starting as alpha's start saw it
};
Seen g;

int StartA(const CoopModApi* api) {
    g.api[0] = api;
    g.calls.push_back(std::string("start a ") + api->mod_id);
    const auto marker = CoopMods::LoadStarting(fs::path(api->mod_folder).parent_path());
    g.marker = marker ? marker->name : "none";
    api->log(api, "hello");
    api->on_frame(api, [](void*) { g.calls.push_back("frame a"); }, nullptr);
    api->on_key(api, [](void*, std::uint32_t vk, int down, int) {
        g.calls.push_back("key a " + std::to_string(vk) + (down ? " down" : " up"));
        return g.take_key;
    }, nullptr);
    api->on_setting(api, [](void*, const char* k, const char* v) {
        g.calls.push_back(std::string("setting a ") + k + "=" + v);
    }, nullptr);
    api->hook(api, 3, [](void*, std::uint32_t, int phase, std::uint64_t* x) {
        g.calls.push_back("hook a " + std::to_string(phase) + " " + std::to_string(x[0]));
        x[0] += 1;
        return g.skip_hook;
    }, nullptr);
    return 1;
}
void StopA() { g.calls.push_back("stop a"); }

int StartB(const CoopModApi* api) {
    g.api[1] = api;
    g.calls.push_back("start b");
    api->on_frame(api, [](void*) { g.calls.push_back("frame b"); }, nullptr);
    api->on_key(api, [](void*, std::uint32_t, int, int) {
        g.calls.push_back("key b");
        return 0;
    }, nullptr);
    api->on_overlay(api, [](void*, const CoopModDraw* d) {
        d->text(d->user, 1, 2, 16, 0xffffffffu, "b");
    }, nullptr);
    CHECK(api->hook(api, 9, [](void*, std::uint32_t, int, std::uint64_t*) { return 0; }, nullptr) == 0);  // not built in
    return 1;
}
void StopB() { g.calls.push_back("stop b"); }

int StartNo(const CoopModApi* api) {
    g.api[2] = api;
    api->on_frame(api, [](void*) { g.calls.push_back("frame no"); }, nullptr);
    api->log(api, "no sound card");
    return 0;
}

int StartCrash(const CoopModApi* api) {
    g.api[3] = api;
    api->on_frame(api, [](void*) {
        g.calls.push_back("frame crash");
        throw std::runtime_error("boom");
    }, nullptr);
    return 1;
}

// The fake libraries: a file's stem names the exports it has.
struct Fake {
    std::map<std::string, void*> exports;
};
std::map<std::string, Fake>& Fakes() {
    static std::map<std::string, Fake> f{
        {"a", {{{COOP_MOD_START_EXPORT, reinterpret_cast<void*>(&StartA)}, {COOP_MOD_STOP_EXPORT, reinterpret_cast<void*>(&StopA)}}}},
        {"b", {{{COOP_MOD_START_EXPORT, reinterpret_cast<void*>(&StartB)}, {COOP_MOD_STOP_EXPORT, reinterpret_cast<void*>(&StopB)}}}},
        {"no", {{{COOP_MOD_START_EXPORT, reinterpret_cast<void*>(&StartNo)}}}},
        {"crash", {{{COOP_MOD_START_EXPORT, reinterpret_cast<void*>(&StartCrash)}}}},
        {"empty", {}},
    };
    return f;
}
Library FakeLibrary() {
    Library lib;
    lib.open = [](const fs::path& file, std::string* problem) -> void* {
        if (file.stem() == "boom") {
            throw std::runtime_error("its DllMain crashed");
        }
        auto it = Fakes().find(file.stem().string());
        if (it == Fakes().end()) {
            *problem = "not a library";
            return nullptr;
        }
        return &it->second;
    };
    lib.symbol = [](void* handle, const char* name) -> void* {
        auto& e = static_cast<Fake*>(handle)->exports;
        auto it = e.find(name);
        return it == e.end() ? nullptr : it->second;
    };
    return lib;
}

struct Folder {
    fs::path root = fs::temp_directory_path() / "coop-mod-plugins-test";
    Folder() {
        std::error_code ec;
        fs::remove_all(root, ec);
        // The tab lists a new mod as Hookshot ranks it: the name that sorts last first (it wins a file). mod.json ids are
        // what the mods see.
        Code("9-a", "alpha", "a.dll", R"(, "settings": [{"key": "speed", "type": "number", "min": 1, "max": 9, "default": 2}])");
        Code("8-no", "nope", "no.dll");
        Code("7-b", "bee", "b.dll");
        Code("6-empty", "empty", "empty.dll");
        Code("5-junk", "junk", "junk.dll");
        Code("4-crash", "crash", "crash.dll");
    }
    ~Folder() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    void Code(const std::string& folder, const std::string& id, const std::string& file, const std::string& more = "") {
        Put(root / folder / "mod.json", R"({"id": ")" + id + R"(", "code": ")" + file + "\"" + more + "}");
        Put(root / folder / file, "MZ");
    }
};

struct Run {
    Folder folder;
    std::vector<std::string> log;
    std::function<bool(unsigned long)> running;  // which other games run (the loader asks about a leftover marker)
    CoopMods::Loader loader{folder.root,
                            {[this](const std::string& l) { log.push_back("mods: " + l); }, 1,
                             [this](unsigned long pid) { return running && running(pid); }}};
    Game game;
    std::unique_ptr<Host> host;
    explicit Run(const std::function<void(Run&)>& before = {}) {
        g = Seen{};
        if (before) {
            before(*this);
        }
        game.log = [this](const std::string& l) { log.push_back(l); };
        game.main_base = [] { return std::uint64_t{0x1000}; };
        game.read = [](std::uint64_t a, void* out, std::size_t n) {
            if (a + n > sizeof g.memory) return false;
            std::memcpy(out, reinterpret_cast<char*>(g.memory) + a, n);
            return true;
        };
        game.write = [](std::uint64_t a, const void* in, std::size_t n) {
            if (a + n > sizeof g.memory) return false;
            std::memcpy(reinterpret_cast<char*>(g.memory) + a, in, n);
            return true;
        };
        game.has_hook = [](std::uint32_t id) { return id == 3; };
        loader.Start();
        host = std::make_unique<Host>(loader, FakeLibrary(), game);
        host->Start();
    }
    bool Logged(const std::string& part) const {
        for (const auto& l : log) {
            if (l.find(part) != std::string::npos) return true;
        }
        return false;
    }
    std::string Note(const std::string& name) const {
        for (const auto& r : loader.Rows()) {
            if (r.name == name) return r.note;
        }
        return "?";
    }
};

}  // namespace

TEST_CASE("mod plugins: loaded in the tab's order, and a mod that cannot start is skipped with a line") {
    Run r;
    CHECK(r.host->Running() == std::vector<std::string>{"alpha", "bee", "crash"});
    CHECK(g.calls[0] == "start a alpha");
    CHECK(g.calls[1] == "start b");
    CHECK(r.Logged("mod alpha: hello"));
    CHECK(r.Logged("mod nope: no sound card"));
    CHECK(r.Logged("mod nope: its coop_mod_start said no"));
    CHECK(r.Logged("mod empty: its code has no coop_mod_start"));
    CHECK(r.Logged("mod junk: not a library"));
    CHECK(r.Logged("mod bee: asked for hook 9, which this game build does not have"));
    CHECK(std::string(g.api[0]->runtime) == "hookshot");
    CHECK(g.api[0]->version == COOP_MOD_API_VERSION);
    CHECK(g.api[0]->size == sizeof(CoopModApi));
    CHECK(fs::path(g.api[1]->mod_folder).filename() == "7-b");

    g.calls.clear();
    r.host->Frame();  // the refused mod's frame callback was dropped
    CHECK(g.calls == std::vector<std::string>{"frame a", "frame b", "frame crash"});
}

TEST_CASE("mod plugins: the table reads and writes memory, reads settings and hears changes") {
    Run r;
    const CoopModApi* a = g.api[0];
    CHECK(a->main_base() == 0x1000);
    std::uint64_t v = 0;
    CHECK(a->read(8, &v, 8) == 1);
    CHECK(v == 2);
    v = 42;
    CHECK(a->write(16, &v, 8) == 1);
    CHECK(g.memory[2] == 42);
    CHECK(a->read(30, &v, 8) == 0);

    char out[8];
    CHECK(a->setting(a, "speed", out, sizeof out) == 1);
    CHECK(std::string(out) == "2");
    CHECK(a->setting(a, "nothing", out, sizeof out) == -1);
    CHECK(a->setting(a, "speed", nullptr, 0) == 1);

    g.calls.clear();
    CHECK(r.loader.Set("9-a", "speed", "5"));
    CHECK(g.calls == std::vector<std::string>{"setting a speed=5"});
    CHECK(a->setting(a, "speed", out, sizeof out) == 1);
    CHECK(std::string(out) == "5");
}

TEST_CASE("mod plugins: callbacks only inside coop_mod_start") {
    Run r;
    const CoopModApi* a = g.api[0];
    CHECK(a->on_frame(a, [](void*) {}, nullptr) == 0);
    CHECK(r.Logged("mod alpha: asked for a frame callback outside coop_mod_start: refused"));
}

TEST_CASE("mod plugins: a key a mod takes goes no further, and hooks change registers and can skip the function") {
    Run r;
    g.calls.clear();
    CHECK_FALSE(r.host->Key(65, true, false));
    CHECK(g.calls == std::vector<std::string>{"key a 65 down", "key b"});
    g.take_key = 1;
    g.calls.clear();
    CHECK(r.host->Key(66, false, false));
    CHECK(g.calls == std::vector<std::string>{"key a 66 up"});

    CHECK(r.host->Hooks(3));
    CHECK_FALSE(r.host->Hooks(9));
    std::uint64_t x[32] = {7};
    g.calls.clear();
    CHECK(r.host->Hook(3, 0, x) == 0);
    CHECK(x[0] == 8);
    g.skip_hook = 1;
    CHECK(r.host->Hook(3, 0, x) == 1);
    CHECK(r.host->Hook(3, 1, x) == 0);  // phase 1 never skips
    CHECK(r.host->Hook(4, 0, x) == 0);
    CHECK(g.calls == std::vector<std::string>{"hook a 0 7", "hook a 0 8", "hook a 1 9"});
}

TEST_CASE("mod plugins: the overlay draws through the table it is given") {
    Run r;
    CHECK(r.host->WantsOverlay());
    std::vector<std::string> drawn;
    CoopModDraw d{};
    d.width = 1280;
    d.height = 720;
    d.user = &drawn;
    d.text = [](void* user, float, float, float, std::uint32_t, const char* t) {
        static_cast<std::vector<std::string>*>(user)->push_back(t);
    };
    r.host->Overlay(d);
    CHECK(drawn == std::vector<std::string>{"b"});
}

TEST_CASE("mod plugins: a mod that crashes is switched off for this run and the tab says so") {
    Run r;
    g.calls.clear();
    r.host->Frame();
    r.host->Frame();
    CHECK(g.calls == std::vector<std::string>{"frame a", "frame b", "frame crash", "frame a", "frame b"});
    CHECK(r.host->Running() == std::vector<std::string>{"alpha", "bee"});
    CHECK(r.Logged("mod crash: crashed in its frame callback: switched off until the next start"));
    CHECK(r.Note("4-crash").find("crashed in its frame callback") != std::string::npos);
}

TEST_CASE("mod plugins: stop runs last loaded first, and nothing is called after it") {
    Run r;
    g.calls.clear();
    r.host->Stop();
    CHECK(g.calls == std::vector<std::string>{"stop b", "stop a"});
    g.calls.clear();
    r.host->Frame();
    CHECK_FALSE(r.host->Key(65, true, false));
    CHECK(g.calls.empty());
    CHECK(r.host->Running().empty());
}

TEST_CASE("mod plugins: a mod can say a restart is needed") {
    Run r;
    g.api[1]->restart_needed(g.api[1], "a new seed");
    const auto restart = r.loader.RestartNeeded();
    CHECK(restart.needed);
    bool found = false;
    for (const auto& w : restart.why) found = found || w.find("a new seed") != std::string::npos;
    CHECK(found);
}

TEST_CASE("mod plugins: coop-mods.starting names the mod while it starts and is gone after") {
    Run r;
    CHECK(g.marker == "9-a");
    CHECK_FALSE(fs::exists(r.folder.root / CoopMods::kStartingFile));
}

TEST_CASE("mod plugins: a mod that crashed or hung the last start is skipped and switched off") {
    Run r([](Run& run) {
        CoopMods::WriteStarting(run.folder.root, {7, "9-a"});  // a game (id 7) that died while alpha started
    });
    CHECK(r.host->Running() == std::vector<std::string>{"bee", "crash"});
    CHECK(r.Logged("9-a: Its code crashed or hung the game at the last start"));
    CHECK(r.Logged("HOS_COOP_MODS_CODE=0"));
    CHECK(r.Note("9-a").find("switched off: switch it on to try again") != std::string::npos);
    CHECK_FALSE(fs::exists(r.folder.root / CoopMods::kStartingFile));
    bool on = true;
    for (const auto& row : r.loader.Rows()) {
        if (row.name == "9-a") on = row.on;
    }
    CHECK_FALSE(on);
    // Switched on again in the tab: its code loads at the next start.
    CHECK(r.loader.SetOn("9-a", true));
    bool said = false;
    for (const auto& w : r.loader.RestartNeeded().why) said = said || w.find("code is loaded at the next start") != std::string::npos;
    CHECK(said);
}

TEST_CASE("mod plugins: the marker of a game that still runs is not a crash") {
    Run r([](Run& run) {
        CoopMods::WriteStarting(run.folder.root, {7, "9-a"});
        run.running = [](unsigned long pid) { return pid == 7; };
    });
    CHECK(r.host->Running() == std::vector<std::string>{"alpha", "bee", "crash"});
    CHECK_FALSE(r.Logged("crashed or hung"));
}

TEST_CASE("mod plugins: a library that crashes while it is loaded is skipped, the others go on") {
    Run r([](Run& run) { run.folder.Code("3-boom", "boom", "boom.dll"); });
    CHECK(r.host->Running() == std::vector<std::string>{"alpha", "bee", "crash"});
    CHECK(r.Logged("mod boom: crashed while its code was loaded"));
    CHECK(r.Note("3-boom") == "crashed while its code was loaded");
}

TEST_CASE("mod plugins: coop-mods.starting text") {
    CHECK(CoopMods::StartingText({12, "my mod"}) == "coop-mods-starting 1 12\nmy mod\n");
    const auto s = CoopMods::ParseStarting("coop-mods-starting 1 12\r\nmy mod\r\n");
    REQUIRE(s);
    CHECK(s->pid == 12);
    CHECK(s->name == "my mod");
    CHECK_FALSE(CoopMods::ParseStarting("coop-mods-starting 2 12\nx\n"));
    CHECK_FALSE(CoopMods::ParseStarting("coop-mods-starting 1 \nx\n"));
    CHECK_FALSE(CoopMods::ParseStarting("coop-mods-starting 1 12\n\n"));
    CHECK_FALSE(CoopMods::ParseStarting("coop-mods-starting 1 12\na\nb\n"));
}

TEST_CASE("mod plugins: the sample mod (hookshot/samples/hello-mod) through the system's loader") {
    const fs::path root = fs::temp_directory_path() / "coop-mod-plugins-sample";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "hello-mod");
    fs::copy_file(COOP_SAMPLE_MOD, root / "hello-mod" / "hello_mod.dll");
    fs::copy_file(COOP_SAMPLE_MOD_JSON, root / "hello-mod" / "mod.json");
    std::vector<std::string> log;
    {
        CoopMods::Loader loader(root, {[&](const std::string& l) { log.push_back("mods: " + l); }, 1, nullptr});
        loader.Start();
        Game game;
        game.log = [&](const std::string& l) { log.push_back(l); };
        Host host(loader, SystemLibrary(), game);
        host.Start();
        REQUIRE(host.Running() == std::vector<std::string>{"hello-mod"});
        host.Frame();
        host.Frame();
        CHECK(host.WantsOverlay());
        CHECK(host.Key(0x76, true, false));  // F7: taken
        CHECK_FALSE(host.Key(0x41, true, false));
        CHECK(loader.Set("hello-mod", "message", "hi there"));
        host.Stop();
        const auto has = [&](const std::string& part) {
            for (const auto& l : log) {
                if (l.find(part) != std::string::npos) return true;
            }
            return false;
        };
        CHECK(has("mod hello-mod: hello from hookshot"));
        CHECK(has("mod hello-mod: shown"));
        CHECK(has("mod hello-mod: message changed"));
        CHECK(has("mod hello-mod: goodbye"));
        CHECK_FALSE(fs::exists(root / CoopMods::kStartingFile));
    }
    // The library stays loaded until the process ends (a mod's thread may still run), so the folder is left behind.
}
