// SPDX-License-Identifier: GPL-3.0-or-later
// The built-in randomizer (hookshot/src/coop_rando.*) with a fake OOTRR: the
// control mod, seeds and folders, what is built or switched, the command
// lines, and the host's seed in co-op.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "mod_temp.h"
#include "coop_mod_library.h"
#include "coop_mods.h"
#include "coop_rando.h"
#include "hos_coop_mods.h"

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
    fs::path base = ModTestDir("coop-rando-test");
    fs::path mods = base / "mods";
    fs::path game = base / "romfs";
    fs::path ootrr = base / "ootrr.exe";
    std::mutex mutex;
    std::vector<std::wstring> runs;
    int fail_at = -1;  // the run (0, 1, ...) that fails
    std::string fail_output;  // what OOTRR writes into its log (each run)
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
        h.run = [this](const fs::path& program, const std::vector<std::string>& args, const fs::path& log) {
            const std::wstring line = CommandLine(program, args);
            const std::lock_guard lock(mutex);
            if (!fail_output.empty()) Put(log, fail_output);
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
    CHECK(rando.Status().find("OOTRR not found at") != std::string::npos);
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

TEST_CASE("rando: off by default - a fresh mods folder builds nothing and switches no seed on") {
    World w;
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    w.Settings(loader);  // OOTRR and the game are there: still nothing without the switch
    rando.Wait();
    CHECK(loader.Value(kId, "on") == "0");
    CHECK(loader.Value(kId, "seed").empty());  // no seed is made either
    CHECK(w.runs.empty());
    CHECK(rando.TakeLines().empty());
    for (const auto& r : loader.Rows()) CHECK_FALSE((r.name.rfind(kSeedPrefix, 0) == 0 && r.on));
}

TEST_CASE("rando: off by default - a seed left from an older kit is switched off") {
    World w;
    Put(w.mods / "randomizer-OLD1" / kSeedFile, "OLD1\n");
    Put(w.mods / "randomizer-OLD1" / "romfs" / "System" / "a.bin", "seeded");
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    REQUIRE(On(loader, "randomizer-OLD1"));  // a folder dropped in is on, as every mod
    Rando rando(loader, w.Hooks());
    rando.Start();
    rando.Wait();
    CHECK_FALSE(On(loader, "randomizer-OLD1"));
    CHECK(w.runs.empty());
}

TEST_CASE("rando: switching it on or off says to restart, in the chat and in the tab") {
    World w;
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    w.Settings(loader);
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    CHECK(rando.TakeLines() == std::vector<std::string>{SwitchLine(true)});
    CHECK(SwitchLine(true) == "Randomizer on: restart the game and start a new save to play it");
    const auto why = loader.RestartNeeded().why;
    CHECK(std::find(why.begin(), why.end(), SwitchLine(true)) != why.end());
    REQUIRE(loader.Set(kFolder, "preset", "easy"));  // another setting: said once only
    rando.Wait();
    CHECK(rando.TakeLines().empty());
    REQUIRE(loader.Set(kFolder, "on", "0"));
    rando.Wait();
    CHECK(rando.TakeLines().empty());  // back as this run started (off): nothing to restart for
    // and the tab's line goes.
    const auto after = loader.RestartNeeded().why;
    CHECK(std::find(after.begin(), after.end(), SwitchLine(false)) == after.end());
    CHECK(std::find(after.begin(), after.end(), SwitchLine(true)) == after.end());
}

TEST_CASE("rando: a host's switch reaching a guest says to restart") {
    World w;
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    w.Settings(loader);
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "1"}, {kId, "seed", "HOST8"}});
    rando.Wait();
    CHECK(rando.TakeLines() == std::vector<std::string>{SwitchLine(true)});
    CHECK(On(loader, "randomizer-HOST8"));
}

TEST_CASE("rando: a guest of a host without the randomizer plays without it") {
    World w;
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    w.Settings(loader);
    REQUIRE(loader.Set(kFolder, "seed", "MINE1"));
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    REQUIRE(On(loader, "randomizer-MINE1"));
    (void)rando.TakeLines();
    // The host sends no randomizer values (its mod is off, or an older game): no setting call comes, the glue asks.
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{});
    rando.Recheck();
    rando.Wait();
    CHECK_FALSE(On(loader, "randomizer-MINE1"));
    CHECK(rando.TakeLines().empty());  // this run never played the seed (built in it): nothing to restart for
    CHECK(loader.Value(kId, "on") == "1");  // our own switch is kept for a game alone
    loader.SetHostValues(std::nullopt);  // left: alone again, our seed again
    rando.Recheck();
    rando.Wait();
    CHECK(On(loader, "randomizer-MINE1"));
}

TEST_CASE("rando: a guest restarted on the host's seed is not told to restart again") {
    World w;
    {   // Run 1: joins a host with the randomizer on; the seed is built and the guest is told to restart.
        CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        w.Settings(loader);
        loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "1"}, {kId, "seed", "HOST9"}});
        rando.Wait();
        CHECK(rando.TakeLines() == std::vector<std::string>{SwitchLine(true)});
        REQUIRE(On(loader, "randomizer-HOST9"));
    }
    // Run 2: the game started with that seed (Hookshot indexed it).
    {
        char line[400];
        hos_coop_mods_applied_head(line, sizeof line, 2);
        std::string text = line;
        hos_coop_mods_applied_row(line, sizeof line, "randomizer-HOST9", 1);
        text += line;
        Put(w.mods / HOS_COOP_MODS_APPLIED_FILE, text);
    }
    CoopMods::Loader loader(w.mods, {nullptr, 2, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();  // before the join: the host's values were kept as the guest's own
    rando.Wait();
    CHECK(loader.Value(kId, "on") == "1");
    CHECK(rando.TakeLines().empty());
    CHECK(On(loader, "randomizer-HOST9"));
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "1"}, {kId, "seed", "HOST9"}});
    rando.Recheck();
    rando.Wait();
    CHECK(rando.TakeLines().empty());
    CHECK(On(loader, "randomizer-HOST9"));
    CHECK(w.runs.size() == 2);  // built once, in run 1
    const auto why = loader.RestartNeeded().why;
    CHECK(std::find(why.begin(), why.end(), SwitchLine(true)) == why.end());
    // Switched off in this run, which plays the seed: that one says to restart.
    loader.SetHostValues(std::nullopt);
    REQUIRE(loader.Set(kFolder, "on", "0"));
    rando.Wait();
    CHECK(rando.TakeLines() == std::vector<std::string>{SwitchLine(false)});
}

TEST_CASE("rando: the kit's own OOTRR and game files are found with no setting") {
    if (std::getenv("OOT_ROMFS") != nullptr) return;  // (a named game folder comes first)
    World w;
    Put(w.base / "tools" / "ootrr" / OotrrName(), "MZ");  // where the kit's build puts it (hookshot\OOTRR.txt)
    CHECK(FindOotrr("", w.base, w.mods) == w.base / "tools" / "ootrr" / OotrrName());
    CHECK(FindGameFolder("", w.base) == w.game);  // <game>\romfs, unpacked by the kit's build
    CHECK(FindGameFolder("", w.base / "bin") == w.game);  // the program in <game>/bin
    CHECK_FALSE(FindGameFolder("", w.base / "a" / "b"));
    CHECK_FALSE(FindGameFolder(CoopMods::ToUtf8(w.base / "elsewhere"), w.base));  // a wrong setting is not passed over
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    REQUIRE(loader.Set(kFolder, "seed", "KIT1"));
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    REQUIRE(w.runs.size() == 2);
    CHECK(w.runs[1].find(L"romfs") != std::wstring::npos);  // the kit's game files, passed to ootrr build
    CHECK(On(loader, "randomizer-KIT1"));
}

TEST_CASE("rando: without the kit's OOTRR or game files it says what to do") {
    if (std::getenv("OOT_ROMFS") != nullptr) return;
    World w;
    fs::remove_all(w.game);
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    CHECK(rando.Status().find("start the game from the kit again") != std::string::npos);
    Put(w.base / "tools" / "ootrr" / OotrrName(), "MZ");
    REQUIRE(loader.Set(kFolder, "seed", "NOGAME"));
    rando.Wait();
    CHECK(rando.Status().find("build the kit's game again") != std::string::npos);
    CHECK(w.runs.empty());
}

TEST_CASE("rando: game files of another version than 1.1.1 say what to do") {
    CHECK(BuildFailureHint("error: the game folder has no Pack/Actor/X.pack.zs").find("not version 1.1.1") !=
          std::string::npos);
    CHECK(BuildFailureHint("something else").empty());
    World w;
    w.fail_at = 1;
    w.fail_output = "error: the game folder has no System/Resource/ResourceSizeTable.Product.112.rsizetable.zs\n";
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    w.Settings(loader);
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    CHECK(rando.Status().find("not version 1.1.1") != std::string::npos);
}

TEST_CASE("rando: the line a guest sends fits a chat line") {
    CHECK(TellLine("x").rfind("[randomizer] ", 0) == 0);
    const std::string long_line = TellLine(std::string(150, 'a') + "\xC3\xA9\xC3\xA9\xC3\xA9" + std::string(100, 'b'));
    CHECK(long_line.size() <= kTellChars);
    CHECK((static_cast<unsigned char>(long_line.back()) & 0xC0) != 0xC0);  // no half character at the end
}

TEST_CASE("rando: a guest that cannot build the host's seed tells the others, once") {
    World w;  // no OOTRR set and none in the kit's place
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    std::mutex told_mutex;
    std::vector<std::string> told;
    bool connected = false;
    auto hooks = w.Hooks();
    hooks.tell_others = [&](const std::string& line) {
        const std::lock_guard lock(told_mutex);
        if (!connected) return false;
        told.push_back(line);
        return true;
    };
    Rando rando(loader, hooks);
    rando.Start();
    REQUIRE(loader.Set(kFolder, "game_folder", CoopMods::ToUtf8(w.game)));
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "1"}, {kId, "seed", "HOST9"}});
    rando.Wait();
    CHECK(w.runs.empty());
    {
        const std::lock_guard lock(told_mutex);
        CHECK(told.empty());  // not connected: kept
        connected = true;
    }
    rando.Recheck();
    rando.Wait();
    rando.Recheck();
    rando.Wait();
    const std::lock_guard lock(told_mutex);
    REQUIRE(told.size() == 1);
    CHECK(told[0].rfind("[randomizer] I play without the host's seed: OOTRR", 0) == 0);
    CHECK(told[0].size() <= kTellChars);
}

TEST_CASE("rando: a host or a player alone whose build fails tells nobody") {
    World w;
    int told = 0;
    auto hooks = w.Hooks();
    hooks.tell_others = [&](const std::string&) {
        ++told;
        return true;
    };
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, hooks);
    rando.Start();
    REQUIRE(loader.Set(kFolder, "ootrr", CoopMods::ToUtf8(w.base / "missing.exe")));
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    rando.Recheck();
    rando.Wait();
    CHECK(told == 0);
}
