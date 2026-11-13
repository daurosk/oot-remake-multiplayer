// SPDX-License-Identifier: GPL-3.0-or-later
// The built-in randomizer (hookshot/src/coop_rando.*) with a fake OOTRR: the
// control mod, seeds and folders, what is built or switched, the command
// lines, and the host's seed in co-op.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "coop_mod_library.h"
#include "coop_mods.h"
#include "coop_rando.h"

namespace fs = std::filesystem;
using namespace CoopRando;

namespace {

void Put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

CoopMods::Row RowNamed(const std::string& name, bool on) {
    CoopMods::Row r;
    r.name = name;
    r.on = on;
    return r;
}

// A mods folder and a game folder, and an OOTRR that writes what the real one writes for a Hookshot build.
struct World {
    fs::path base = fs::temp_directory_path() / "coop-rando-test";
    fs::path mods = base / "mods";
    fs::path game = base / "romfs";
    fs::path ootrr = base / "ootrr.exe";
    std::mutex mutex;
    std::vector<std::wstring> runs;
    int fail_at = -1;  // the run (0, 1, ...) that fails
    World() {
        std::error_code ec;
        fs::remove_all(base, ec);
        fs::create_directories(mods);
        fs::create_directories(game / "Pack");
        fs::create_directories(game / "System");
        Put(ootrr, "MZ");
        // This run's hookshot.exe has patch 0011 (it reads the Mods tab's list): process 1, nothing indexed.
        Put(mods / "coop-mods.applied", "coop-mods-applied 1 1\n");
    }
    ~World() {
        std::error_code ec;
        fs::remove_all(base, ec);
    }
    Rando::Hooks Hooks() {
        Rando::Hooks h;
        h.game_dir = base;
        h.entropy = [] { return std::uint64_t{123456789}; };
        h.run = [this](const std::wstring& line, const fs::path&) {
            const std::lock_guard lock(mutex);
            const int at = static_cast<int>(runs.size());
            runs.push_back(line);
            if (at == fail_at) return 3;
            // "build" names the folder last but two (before --host hookshot): write it as OOTRR does.
            const std::string text = CoopMods::ToUtf8(fs::path(line));
            if (text.find(" build ") != std::string::npos) {
                const auto end = text.rfind(" --host");
                const auto start = text.rfind(' ', end - 1) + 1;
                const std::string folder = text.substr(start, end - start);
                Put(mods / folder / "romfs" / "System" / "a.bin", "seeded");
                const auto dash = folder.find('-');
                std::string word = folder.substr(dash + 1);
                if (const auto p = word.find('-'); p != std::string::npos) word = word.substr(0, p);
                Put(mods / folder / kSeedFile, word + "\n");
            }
            return 0;
        };
        return h;
    }
    void Settings(CoopMods::Loader& loader) {
        REQUIRE(loader.Set(kFolder, "ootrr", CoopMods::ToUtf8(ootrr)));
        REQUIRE(loader.Set(kFolder, "game_folder", CoopMods::ToUtf8(game)));
    }
};

bool On(const CoopMods::Loader& loader, const std::string& name) {
    for (const auto& r : loader.Rows()) {
        if (r.name == name) return r.on;
    }
    return false;
}

}  // namespace

TEST_CASE("rando: the control mod's mod.json is valid, with the seed and preset shared") {
    std::string why;
    const auto m = CoopMods::ParseManifest(ManifestText(), kFolder, &why);
    REQUIRE_MESSAGE(m, why);
    CHECK(m->id == kId);
    CHECK(m->share == CoopMods::Share::Everyone);
    std::set<std::string> shared;
    for (const auto& s : m->settings) {
        if (s.shared) shared.insert(s.key);
    }
    CHECK(shared == std::set<std::string>{"on", "seed", "preset"});
    CHECK(m->settings[2].initial == "default");
    CHECK(m->settings[2].choices.size() == std::size(kPresets));
}

TEST_CASE("rando: seed words and folders as OOTRR names them") {
    CHECK(SeedWord("Hello World!") == "HelloWorld");
    CHECK(SeedWord("***") == "seed");
    CHECK(SeedWord(std::string(80, 'a')).size() == 63);
    CHECK(FolderFor("AB12", "default") == "randomizer-AB12");
    CHECK(FolderFor("AB12", "") == "randomizer-AB12");
    CHECK(FolderFor("AB12", "keysanity") == "randomizer-AB12-keysanity");
    const std::string a = NewSeed(1), b = NewSeed(2);
    CHECK(a.size() == 8);
    CHECK(a != b);
    CHECK(SeedWord(a) == a);
}

TEST_CASE("rando: what to do for the settings, the seeds built and the switches") {
    const std::vector<Built> built{{"randomizer-AB12", "AB12", "default"}, {"randomizer-ZZ-easy", "ZZ", "easy"}};
    {
        const Plan p = Decide({true, "NEW1", "default"}, built, {});
        CHECK(p.what == Plan::Do::build);
        CHECK(p.folder == "randomizer-NEW1");
    }
    {   // built, off now, another seed on: switch
        const Plan p = Decide({true, "AB12", ""}, built,
                              {RowNamed("randomizer-AB12", false), RowNamed("randomizer-ZZ-easy", true)});
        CHECK(p.what == Plan::Do::use);
        CHECK(p.on == std::vector<std::string>{"randomizer-AB12"});
        CHECK(p.off == std::vector<std::string>{"randomizer-ZZ-easy"});
    }
    {   // as it should be
        const Plan p = Decide({true, "AB12", "default"}, built, {RowNamed("randomizer-AB12", true)});
        CHECK(p.what == Plan::Do::nothing);
    }
    {   // same seed, other preset: its own folder
        const Plan p = Decide({true, "AB12", "easy"}, built, {});
        CHECK(p.what == Plan::Do::build);
        CHECK(p.folder == "randomizer-AB12-easy");
    }
    {   // off: every seed off; other mods untouched
        const Plan p = Decide({false, "AB12", ""}, built,
                              {RowNamed("randomizer-AB12", true), RowNamed("hd-hud", true)});
        CHECK(p.what == Plan::Do::use);
        CHECK(p.off == std::vector<std::string>{"randomizer-AB12"});
    }
    CHECK(Decide({true, "", ""}, built, {}).what == Plan::Do::nothing);
}

TEST_CASE("rando: OOTRR's command lines") {
    CHECK(GenerateArgs("", "a b", "s.ootrr") == std::vector<std::string>{"generate", "default", "ab", "s.ootrr"});
    CHECK(BuildArgs("s.ootrr", "C:/game", "C:/mods", "randomizer-ab") ==
          std::vector<std::string>{"build", "s.ootrr", "C:/game", "C:/mods", "randomizer-ab", "--host", "hookshot"});
    // "C:\My Tools\ootrr.exe" build "C:\a b\\" "x\"y"   (a trailing backslash doubled before the quote)
    const std::wstring want = L"\"C:\\My Tools\\ootrr.exe\" build \"C:\\a b\\\\\" \"x\\\"y\"";
    CHECK(CommandLine("C:/My Tools/ootrr.exe", {"build", "C:/a b/", "x\"y"}) == want);
}

TEST_CASE("rando: switched on, a seed is made, built once and switched on first, and a restart is asked for") {
    World w;
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    CHECK(fs::exists(w.mods / kFolder / CoopMods::kManifestFile));
    w.Settings(loader);
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    const std::string seed = loader.Value(kId, "seed");
    CHECK(seed == NewSeed(123456789));
    REQUIRE(w.runs.size() == 2);
    CHECK(w.runs[0].find(L" generate default ") != std::wstring::npos);
    CHECK(w.runs[1].find(L" --host hookshot") != std::wstring::npos);
    const std::string folder = FolderFor(seed, "default");
    CHECK(On(loader, folder));
    CHECK(loader.Rows().front().name == folder);
    CHECK(loader.RestartNeeded().needed);
    CHECK_FALSE(rando.Building());

    // The same settings again: nothing is built twice.
    REQUIRE(loader.Set(kFolder, "preset", "easy"));
    REQUIRE(loader.Set(kFolder, "preset", "default"));
    rando.Wait();
    CHECK(w.runs.size() == 4);  // easy was built once; default came back built
    CHECK(On(loader, folder));
    CHECK_FALSE(On(loader, FolderFor(seed, "easy")));

    // Off: its seeds are off.
    REQUIRE(loader.Set(kFolder, "on", "0"));
    rando.Wait();
    CHECK_FALSE(On(loader, folder));
}

TEST_CASE("rando: a failed build is said once and not tried again until the settings change") {
    World w;
    w.fail_at = 1;
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    std::vector<std::string> log;
    auto hooks = w.Hooks();
    std::mutex log_mutex;
    hooks.log = [&](const std::string& l) {
        const std::lock_guard lock(log_mutex);
        log.push_back(l);
    };
    Rando rando(loader, hooks);
    rando.Start();
    w.Settings(loader);
    REQUIRE(loader.Set(kFolder, "seed", "FAIL1"));
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    CHECK(w.runs.size() == 2);
    CHECK(rando.Status().find("OOTRR could not build the seed (exit 3)") != std::string::npos);
    CHECK_FALSE(fs::exists(w.mods / "randomizer-FAIL1"));
    REQUIRE(loader.Set(kFolder, "game_folder", CoopMods::ToUtf8(w.game) + "/"));  // another setting, same seed
    rando.Wait();
    CHECK(w.runs.size() == 2);
    REQUIRE(loader.Set(kFolder, "seed", "GOOD2"));
    rando.Wait();
    CHECK(w.runs.size() == 4);
    CHECK(On(loader, "randomizer-GOOD2"));
}

TEST_CASE("rando: without OOTRR or the game folder it says what to set") {
    World w;
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    REQUIRE(loader.Set(kFolder, "ootrr", CoopMods::ToUtf8(w.base / "missing.exe")));
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    CHECK(rando.Status().find("OOTRR (ootrr.exe) not found") != std::string::npos);
    CHECK(w.runs.empty());
}

TEST_CASE("rando: a guest builds the host's seed, and its own seed is not made") {
    World w;
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    w.Settings(loader);
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "1"}, {kId, "seed", ""}});
    rando.Wait();
    CHECK(w.runs.empty());  // the host's game makes the seed
    CHECK(loader.Value(kId, "seed").empty());
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{
        {kId, "on", "1"}, {kId, "seed", "HOST7"}, {kId, "preset", "keysanity"}});
    rando.Wait();
    CHECK(w.runs.size() == 2);
    CHECK(On(loader, "randomizer-HOST7-keysanity"));
}
