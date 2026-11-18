// SPDX-License-Identifier: GPL-3.0-or-later
// The built-in randomizer (hookshot/src/coop_rando.*) with a fake OOTRR: the
// control mod, seeds and folders, what is built or switched, the command
// lines, and the host's seed in co-op.
#include <doctest/doctest.h>

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "mod_temp.h"
#include "coop_mod_json.h"
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

std::string Get(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
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
    std::set<std::string> bad_presets;  // presets this OOTRR cannot make (#16: OOTRR 0.5.0's full_shuffle)
    std::set<std::string> stopped_presets;  // presets whose run is stopped (a timeout, the game closing)
    bool probe = false;                 // Hooks::probe_presets (off: the tests count every run)
    bool spoiler = true;                // generate writes the --spoiler file (false: an OOTRR that writes none)
    std::vector<std::string> staged;  // the size tables the last build saw beside it (their text, sorted)
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
        h.probe_presets = probe;
        h.run = [this](const fs::path& program, const std::vector<std::string>& args, const fs::path& log) {
            const std::wstring line = CommandLine(program, args);
            const std::lock_guard lock(mutex);
            if (!fail_output.empty()) Put(log, fail_output);
            const int at = static_cast<int>(runs.size());
            runs.push_back(line);
            if (args.size() >= 2 && args[0] == "generate" && stopped_presets.count(args[1]) != 0) return kStopped;
            if (args.size() >= 2 && args[0] == "generate" && bad_presets.count(args[1]) != 0) {
                Put(log, "This build cannot apply these settings yet: bridge is dungeons\n");
                return 1;
            }
            if (at == fail_at) return 3;
            // generate <preset or string> <seed> <out.ootrr> [--spoiler <file>]: the seed file (JSON, its "settings"
            // the OOTRR1- string) and, when asked, the spoiler log, as OOTRR 0.6.1 and 0.6.2 write them.
            if (args.size() >= 4 && args[0] == "generate") {
                const std::string s = args[1].rfind("OOTRR", 0) == 0 ? args[1] : "OOTRR1-P" + args[1];
                Put(CoopMods::FromUtf8(args[3]), "{\n  \"format\": \"ootrr-seed\",\n  \"seed\": \"" + args[2] +
                                                     "\",\n  \"settings\": \"" + s + "\",\n  \"placements\": {}\n}\n");
                if (args.size() >= 6 && args[4] == "--spoiler" && spoiler) {
                    Put(CoopMods::FromUtf8(args[5]), "Randomized chests\n  KF Kokiri Sword Chest: " + args[2] + "\n");
                }
            }
            // build <seed> <game> <load dir> <folder>: write the folder into the load dir as OOTRR does, and note
            // the size tables OOTRR finds there (mods/<name>/romfs only).
            if (!args.empty() && args[0] == "build" && args.size() >= 5) {
                const fs::path load = CoopMods::FromUtf8(args[3]);
                const std::string folder = args[4];
                staged.clear();
                std::error_code ec;
                for (fs::directory_iterator d(load, ec), e; !ec && d != e; d.increment(ec)) {
                    const fs::path t = d->path() / "romfs" / "System" / "Resource" / "T.rsizetable.zs";
                    if (fs::is_regular_file(t)) staged.push_back(Get(t));
                }
                std::sort(staged.begin(), staged.end());
                Put(load / folder / "romfs" / "System" / "a.bin", "seeded");
                // The seed's size table: the game's with every staged table merged in (here: their texts).
                std::string merged = "game";
                for (const std::string& t : staged) merged += "+" + t;
                Put(load / folder / "romfs" / "System" / "Resource" / "T.rsizetable.zs", merged);
                const auto dash = folder.find('-');
                std::string word = folder.substr(dash + 1);
                if (const auto p = word.find('-'); p != std::string::npos) word = word.substr(0, p);
                Put(load / folder / kSeedFile, word + "\n");
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
    CHECK(shared == std::set<std::string>{"on", "seed", "preset", "settings"});
    CHECK(m->settings[2].initial == "default");
    CHECK(m->settings[2].choices.size() == std::size(kPresets));
    // Each choice is {"value", "name"}: OOTRR and the folder get the value, the
    // Mods tab shows the name (English here; the loader picks the language's
    // with TextIn, which the tab's own test covers).
    for (std::size_t i = 0; i < m->settings[2].choices.size(); ++i) {
        CHECK(m->settings[2].choices[i].first == kPresets[i]);
        CHECK_FALSE(m->settings[2].choices[i].second.empty());
    }
    // The kit's own mod carries its text in both Chinese tables, so a Chinese menu
    // shows it translated without a translation of the mod's file.
    CHECK(ManifestText().find("\"name_zh-Hans\"") != std::string::npos);
    CHECK(ManifestText().find("\"help_zh-Hant\"") != std::string::npos);
    CHECK(ManifestText().find("\"description_zh-Hans\"") != std::string::npos);

    // The loader's caps (coop_mod_library.cpp) are characters, and the Chinese
    // halves are three bytes a character: the longest of them (the author line)
    // is 71 bytes. Counted in bytes it was refused, and the whole manifest with
    // it; counted in characters it fits. The tests run in English, so TextIn
    // reads the plain fields above; this keeps the numbers of the others true.
    const auto chars = [](const std::string& text) {
        return static_cast<std::size_t>(
            std::count_if(text.begin(), text.end(), [](char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }));
    };
    std::size_t localised = 0;
    const auto fits = [&](const CoopModJson::Value& object, const char* key, std::size_t most) {
        for (const char* code : {"zh-Hans", "zh-Hant"}) {
            const std::string name = std::string(key) + '_' + code;
            if (const CoopModJson::Value* v = object.Get(name); v != nullptr && v->kind == CoopModJson::Value::Kind::String) {
                ++localised;
                INFO(name);
                CHECK(chars(v->string) <= most);
            }
        }
    };
    const auto json = CoopModJson::Parse(ManifestText());
    REQUIRE(json);
    fits(*json, "name", 64);
    fits(*json, "author", 64);
    fits(*json, "description", 512);
    for (const CoopModJson::Value& setting : json->Get("settings")->array) {
        fits(setting, "name", 64);
        fits(setting, "help", 256);
        const CoopModJson::Value* choices = setting.Get("choices");
        if (choices == nullptr) {
            continue;
        }
        for (const CoopModJson::Value& choice : choices->array) {
            if (choice.kind == CoopModJson::Value::Kind::Object) {
                fits(choice, "name", 64);
            }
        }
    }
    CHECK(localised >= 12);
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
    const std::vector<Built> built{{"randomizer-AB12", "AB12", "default", ""},
                                   {"randomizer-ZZ-easy", "ZZ", "easy", ""}};
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
    CHECK(GenerateArgs("easy", "AB", "s.ootrr", "C:/m/s.spoiler.txt") ==
          std::vector<std::string>{"generate", "easy", "AB", "s.ootrr", "--spoiler", "C:/m/s.spoiler.txt"});
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

TEST_CASE("rando: the seed is built beside every other mod's size table, the puppet pack's too") {
    // (lane1, 2026-11-14: the seed's table had none of the puppet pack's 189 entries, as OOTRR reads only
    // mods/<name>/romfs, and the game aborted while it loaded)
    World w;
    const fs::path table = fs::path("romfs") / "System" / "Resource" / "T.rsizetable.zs";
    Put(w.mods / "contents" / "0400486024DEC000" / "coop-puppet" / table, "puppet");
    Put(w.mods / "aa" / table, "aa");
    Put(w.mods / "_off" / table, "off");
    Put(w.mods / "randomizer-old" / table, "old seed");
    Put(w.mods / "randomizer-old" / kSeedFile, "old\n");
    Put(w.mods / "bb" / table, "bb");
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    REQUIRE(loader.SetOn("bb", false));
    Rando rando(loader, w.Hooks());
    rando.Start();
    w.Settings(loader);
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    REQUIRE(w.runs.size() == 2);
    CHECK(w.staged == std::vector<std::string>{"aa", "puppet"});
    const std::string folder = FolderFor(loader.Value(kId, "seed"), "default");
    CHECK(fs::is_regular_file(w.mods / folder / kSeedFile));
    CHECK(Get(w.mods / folder / "romfs" / "System" / "a.bin") == "seeded");
    CHECK_FALSE(fs::exists(w.mods / kFolder / "_seed-build"));
    CHECK(On(loader, folder));
}

TEST_CASE("rando: a failed build leaves no stage and no seed folder") {
    World w;
    Put(w.mods / "contents" / "0400486024DEC000" / "coop-puppet" / "romfs" / "System" / "Resource" / "T.rsizetable.zs", "pack");
    w.fail_at = 1;  // the build
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    w.Settings(loader);
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    REQUIRE(w.runs.size() == 2);
    CHECK_FALSE(fs::exists(w.mods / kFolder / "_seed-build"));
    CHECK_FALSE(fs::exists(w.mods / FolderFor(loader.Value(kId, "seed"), "default")));
}

TEST_CASE("rando: a failed build is said once and not tried again until the settings change") {
    World w;
    w.fail_at = 1;
    w.fail_output = "This build cannot apply these settings yet: bridge is dungeons\n";
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
    {
        const std::lock_guard lock(log_mutex);
        const bool path_logged = std::any_of(log.begin(), log.end(), [&](const std::string& l) {
            return l.find(CoopMods::ToUtf8(w.mods / kFolder / kLogFile)) != std::string::npos;
        });
        CHECK(path_logged);  // longshot_log.txt has the full path
    }
    const auto chat = rando.TakeLines();
    CHECK(std::find(chat.begin(), chat.end(), "OOTRR: This build cannot apply these settings yet: bridge is dungeons") != chat.end());
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
    CHECK(rando.TakeLines() == std::vector<std::string>{SwitchLine(true), ReadyLine(loader.Value(kId, "seed"))});
    CHECK(SwitchLine(true).find("until this chat says the seed is ready") != std::string::npos);
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
    CHECK(rando.TakeLines() == std::vector<std::string>{SwitchLine(true), ReadyLine("HOST8")});
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
        CHECK(rando.TakeLines() == std::vector<std::string>{SwitchLine(true), ReadyLine("HOST9")});
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
        // loud in this game's own chat, with what to do (#59)
        const auto chat = rando.TakeLines();
        CHECK(std::count_if(chat.begin(), chat.end(), [](const std::string& l) {
                  return l.rfind("Randomizer: NO SEED - you play the BASE GAME, not the host's seed HOST9", 0) == 0 &&
                         l.find("OOTRR") != std::string::npos && l.find("To fix: start the game again") != std::string::npos;
              }) == 1);
    }
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

namespace {
// A guest's told lines, with a switch for being connected.
struct Told {
    std::mutex mutex;
    std::vector<std::string> lines;
    bool connected = true;
    std::function<bool(const std::string&)> Hook() {
        return [this](const std::string& line) {
            const std::lock_guard lock(mutex);
            if (!connected) return false;
            lines.push_back(line);
            return true;
        };
    }
    std::vector<std::string> Get() {
        const std::lock_guard lock(mutex);
        return lines;
    }
};

bool HasPath(const std::string& line) {
    return line.find(":/") != std::string::npos || line.find(":\\") != std::string::npos ||
           line.find('/') != std::string::npos || line.find('\\') != std::string::npos;
}
}  // namespace

TEST_CASE("rando: the line a guest tells the others never holds a local path or setting") {
    World w;
    Told told;
    auto hooks = w.Hooks();
    hooks.tell_others = told.Hook();
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, hooks);
    rando.Start();
    // a setting naming a folder with the player's user name in it
    REQUIRE(loader.Set(kFolder, "ootrr", "C:/Users/SecretName/ootrr.exe"));
    REQUIRE(loader.Set(kFolder, "game_folder", CoopMods::ToUtf8(w.game)));
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "1"}, {kId, "seed", "HOSTP1"}});
    rando.Wait();
    CHECK(rando.Status().find("SecretName") != std::string::npos);  // said here, in the tab
    // a failed build, whose local line names OOTRR's log in the mods folder
    REQUIRE(loader.Set(kFolder, "ootrr", CoopMods::ToUtf8(w.ootrr)));
    w.fail_at = 1;
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "1"}, {kId, "seed", "HOSTP2"}});
    rando.Wait();
    // (the tab names the file plainly; its full path is in this game's chat, once)
    CHECK(rando.Status().find(std::string("see ") + kLogFile + " in the game's mods folder, " + kFolder) != std::string::npos);
    const auto chat = rando.TakeLines();
    CHECK(std::count_if(chat.begin(), chat.end(), [&](const std::string& l) {
              return l.find(ChatSafe(CoopMods::ToUtf8(w.mods / kFolder / kLogFile), 400)) != std::string::npos;
          }) == 1);
    const auto lines = told.Get();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "[randomizer] I play without the host's seed: OOTRR not found");
    CHECK(lines[1] == "[randomizer] I play without the host's seed: OOTRR could not build the seed (exit 3)");
    for (const std::string& l : lines) {
        CHECK_MESSAGE(!HasPath(l), l);
        CHECK(l.find("SecretName") == std::string::npos);
    }
}

TEST_CASE("rando: a line not sent yet is dropped when the host's seed changes or the host goes") {
    World w;  // no OOTRR set and none in the kit's place
    Told told;
    told.connected = false;
    auto hooks = w.Hooks();
    hooks.tell_others = told.Hook();
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, hooks);
    rando.Start();
    REQUIRE(loader.Set(kFolder, "game_folder", CoopMods::ToUtf8(w.game)));
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "1"}, {kId, "seed", "OLD1"}});
    rando.Wait();
    // OOTRR comes, the host picks another seed: built fine, so the old line is not true any more
    REQUIRE(loader.Set(kFolder, "ootrr", CoopMods::ToUtf8(w.ootrr)));
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "1"}, {kId, "seed", "NEW2"}});
    rando.Wait();
    CHECK(On(loader, "randomizer-NEW2"));
    {
        const std::lock_guard lock(told.mutex);
        told.connected = true;
    }
    rando.Recheck();
    rando.Wait();
    CHECK(told.Get().empty());
    // failed again while not connected, then the host goes: nothing is sent later
    {
        const std::lock_guard lock(told.mutex);
        told.connected = false;
    }
    REQUIRE(loader.Set(kFolder, "ootrr", CoopMods::ToUtf8(w.base / "gone.exe")));
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "1"}, {kId, "seed", "LATE3"}});
    rando.Wait();
    loader.SetHostValues(std::nullopt);
    rando.Recheck();
    rando.Wait();
    {
        const std::lock_guard lock(told.mutex);
        told.connected = true;
    }
    rando.Recheck();
    rando.Wait();
    CHECK(told.Get().empty());
}

TEST_CASE("rando: OOTRR's last lines for the log, and the tab's plain file name") {
    using CoopRando::LastLines;
    CHECK(LastLines("", 10).empty());
    CHECK(LastLines("a\r\n\r\nb\n  \nc", 10) == std::vector<std::string>{"a", "b", "c"});
    CHECK(LastLines("1\n2\n3\n4\n", 2) == std::vector<std::string>{"3", "4"});
    const std::string line = CoopRando::SeeLogLine("OOTRR could not make the seed (exit 1)");
    CHECK(line.find("last-build.txt") != std::string::npos);
    CHECK(line.find("oot-remake-randomizer") != std::string::npos);
    CHECK_FALSE(HasPath(line));
}

TEST_CASE("rando: OOTRR's own error lines for the chat (#16)") {
    using CoopRando::ErrorLines;
    const std::string out = "reading settings\nThis build cannot apply these settings yet: bridge is dungeons\nError: 2\n"
                            "x\ny\nz\nfailed\nfailed again\nerror 6\n";
    const auto e = ErrorLines(out, 5);
    REQUIRE(e.size() == 5);
    CHECK(e[0] == "This build cannot apply these settings yet: bridge is dungeons");
    CHECK(e[4] == "error 6");
    CHECK(ErrorLines("one\ntwo\n", 5) == std::vector<std::string>{"one", "two"});  // nothing error-like: the last lines
}

namespace {
std::vector<std::string> PresetChoices(const CoopMods::Loader& loader) {
    std::vector<std::string> out;
    for (const CoopMods::SettingRow& s : loader.Settings(kFolder)) {
        if (s.setting.key == "preset") {
            for (const auto& c : s.setting.choices) out.push_back(c.first);
        }
    }
    return out;
}
std::size_t Generates(const World& w, const char* preset) {
    std::size_t n = 0;
    const std::wstring want = L"generate " + std::wstring(preset, preset + std::strlen(preset)) + L" ";
    for (const auto& r : w.runs) n += r.find(want) != std::wstring::npos ? 1 : 0;
    return n;
}
}  // namespace

TEST_CASE("rando: only the presets this OOTRR can make are offered, a bad one goes back to default (#16)") {
    World w;
    w.probe = true;
    w.bad_presets = {"full_shuffle"};
    {
        CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        w.Settings(loader);
        REQUIRE(loader.Set(kFolder, "preset", "full_shuffle"));
        REQUIRE(loader.Set(kFolder, "seed", "PRE1"));
        REQUIRE(loader.Set(kFolder, "on", "1"));
        rando.Wait();
        for (const char* p : kPresets) CHECK(Generates(w, p) >= 1);  // each tried once
        CHECK(loader.Value(kId, "preset") == "default");
        const auto choices = PresetChoices(loader);
        CHECK(std::find(choices.begin(), choices.end(), "full_shuffle") == choices.end());
        CHECK(std::find(choices.begin(), choices.end(), "keysanity") != choices.end());
        CHECK(Get(w.mods / kFolder / kPresetsFile).find("full_shuffle bad") != std::string::npos);
        const auto chat = rando.TakeLines();
        CHECK(std::find(chat.begin(), chat.end(),
                        "Randomizer Settings full_shuffle are not in this OOTRR yet: back to default.") != chat.end());
        CHECK(On(loader, "randomizer-PRE1"));  // built with default
    }
    // The next run of the game, the same OOTRR: not tried again.
    const std::size_t before = w.runs.size();
    {
        CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        // Start's manifest rewrite (the names in the player's language) keeps the refused one out.
        const auto choices = PresetChoices(loader);
        CHECK(std::find(choices.begin(), choices.end(), "full_shuffle") == choices.end());
        CHECK(std::find(choices.begin(), choices.end(), "keysanity") != choices.end());
        CHECK(Get(w.mods / kFolder / CoopMods::kManifestFile).find("\"name_zh-Hans\"") != std::string::npos);
        REQUIRE(loader.Set(kFolder, "seed", "PRE2"));
        rando.Wait();
        CHECK(w.runs.size() == before + 2);  // generate and build only
    }
    // Another OOTRR (a kit update): tried again.
    Put(w.ootrr, "MZ new");
    const std::size_t before2 = w.runs.size();
    {
        CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        REQUIRE(loader.Set(kFolder, "seed", "PRE3"));
        rando.Wait();
        CHECK(w.runs.size() == before2 + std::size(kPresets) + 2);
    }
}

TEST_CASE("rando: the preset cache's text") {
    PresetCache c{"12-34", {{"default", true}, {"full_shuffle", false}}};
    const PresetCache back = ReadPresetCache(PresetCacheText(c));
    CHECK(back.key == "12-34");
    CHECK(back.ok == c.ok);
    CHECK(ReadPresetCache("").key.empty());
}

TEST_CASE("rando: chat lines mask the user's folder and are cut") {
    using CoopRando::ChatSafe;
    CHECK(ChatSafe(R"(see C:\Users\SecretName\Desktop\kit\last-build.txt)") == R"(see C:\Users\<user>\Desktop\kit\last-build.txt)");
    CHECK(ChatSafe("at C:/users/Secret Name/x") == "at C:/users/<user>/x");  // (a name with a space: all of it)
    CHECK(ChatSafe("/home/deck/kit/data/game") == "/home/<user>/kit/data/game");
    CHECK(ChatSafe("users and homes are words") == "users and homes are words");
    const std::string cut = ChatSafe(std::string(150, 'a'));
    CHECK(cut == std::string(100, 'a') + "...");
}

TEST_CASE("rando: a preset whose try was stopped is offered, not cached, and tried again at the next start (#16)") {
    World w;
    w.probe = true;
    w.stopped_presets = {"keysanity"};
    w.bad_presets = {"full_shuffle"};
    {
        CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        w.Settings(loader);
        REQUIRE(loader.Set(kFolder, "seed", "STOP1"));
        REQUIRE(loader.Set(kFolder, "on", "1"));
        rando.Wait();
        const auto choices = PresetChoices(loader);
        CHECK(std::find(choices.begin(), choices.end(), "keysanity") != choices.end());  // not known: offered
        CHECK(std::find(choices.begin(), choices.end(), "full_shuffle") == choices.end());
        const std::string cache = Get(w.mods / kFolder / kPresetsFile);
        CHECK(cache.find("keysanity") == std::string::npos);
        CHECK(cache.find("full_shuffle bad") != std::string::npos);
        CHECK(Generates(w, "keysanity") == 1);
        REQUIRE(loader.Set(kFolder, "seed", "STOP2"));  // a second build in this run: not tried again
        rando.Wait();
        CHECK(Generates(w, "keysanity") == 1);
    }
    // The next start: only keysanity is tried again; now it works and is cached.
    w.stopped_presets.clear();
    const std::size_t shuffles = Generates(w, "full_shuffle");
    {
        CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        REQUIRE(loader.Set(kFolder, "seed", "STOP3"));
        rando.Wait();
        CHECK(Generates(w, "keysanity") == 2);
        CHECK(Generates(w, "full_shuffle") == shuffles);  // known bad: not tried again
        CHECK(Get(w.mods / kFolder / kPresetsFile).find("keysanity ok") != std::string::npos);
    }
}

TEST_CASE("rando: only a real refusal counts as a bad preset") {
    CHECK(PresetRefused(1, "error: This build cannot apply these settings yet: bridge is dungeons"));
    CHECK_FALSE(PresetRefused(1, " \r\n"));      // said nothing
    CHECK_FALSE(PresetRefused(kStopped, "half"));  // a timeout or a stop
    CHECK_FALSE(PresetRefused(-1, ""));            // did not start
    CHECK_FALSE(PresetRefused(0, "fine"));
}

TEST_CASE("rando: OOTRR's output is read from its end, at most the cap") {
    World w;
    const fs::path f = w.base / "big.txt";
    Put(f, std::string(100, 'a') + "THE END");
    CHECK(ReadTail(f, 7) == "THE END");
    CHECK(ReadTail(f, 1000).size() == 107);
    CHECK(ReadTail(w.base / "none.txt", 10).empty());
}

TEST_CASE("rando: chat lines have no control characters") {
    CHECK(CoopRando::ChatSafe("a\tb\x1b[31mc\x7f") == "a b [31mc ");
}

namespace {
const fs::path kTable = fs::path("romfs") / "System" / "Resource" / "T.rsizetable.zs";

// This run's hookshot.exe started with `seed` (process `pid` indexed it).
void PlaysSeed(const World& w, const std::string& seed, int pid) {
    char line[400];
    hos_coop_mods_applied_head(line, sizeof line, pid);
    std::string text = line;
    hos_coop_mods_applied_row(line, sizeof line, seed.c_str(), 1);
    text += line;
    Put(w.mods / HOS_COOP_MODS_APPLIED_FILE, text);
}

std::size_t Count(const std::vector<std::string>& lines, const std::string& line) {
    return static_cast<std::size_t>(std::count(lines.begin(), lines.end(), line));
}
}  // namespace

TEST_CASE("rando: a built seed is made again when the other mods' size tables are not the ones it was built beside") {
    Plan p;
    const Want want{true, "S1", "default"};
    const std::vector<CoopMods::Row> rows{RowNamed("randomizer-S1", true)};
    const std::vector<Built> built{{"randomizer-S1", "S1", "default", "coop-tables 1\nx 1 0\n"}};
    p = Decide(want, built, rows, std::string("coop-tables 1\nx 1 0\n"));
    CHECK(p.what == Plan::Do::nothing);
    p = Decide(want, built, rows, std::string("coop-tables 1\nx 2 5\n"));
    CHECK(p.what == Plan::Do::refresh);
    CHECK(p.folder == "randomizer-S1");
    p = Decide(want, {{"randomizer-S1", "S1", "default", ""}}, rows, std::string("coop-tables 1\n"));
    CHECK(p.what == Plan::Do::refresh);  // built before the stamp: once more
    p = Decide(want, {{"randomizer-S1", "S1", "default", ""}}, rows);
    CHECK(p.what == Plan::Do::nothing);  // (not compared)
}

TEST_CASE("rando: the size tables' stamp changes with a table's bytes and leaves out seeds and switched-off mods") {
    World w;
    Put(w.mods / "contents" / "0400486024DEC000" / "coop-puppet" / kTable, "puppet");
    Put(w.mods / "randomizer-old" / kTable, "seed");
    Put(w.mods / "_off" / kTable, "off");
    const std::vector<CoopMods::Row> rows;
    const auto tables = OtherSizeTables(w.mods, rows);
    REQUIRE(tables.size() == 1);
    const std::string a = TablesStamp(w.mods, tables);
    CHECK(a.rfind("coop-tables 1\ncontents/0400486024DEC000/coop-puppet/romfs/System/Resource/T.rsizetable.zs 6 ", 0) ==
          0);
    CHECK(TablesStamp(w.mods, OtherSizeTables(w.mods, rows)) == a);
    Put(w.mods / "contents" / "0400486024DEC000" / "coop-puppet" / kTable, "puppeT");
    CHECK(TablesStamp(w.mods, OtherSizeTables(w.mods, rows)) != a);
    CHECK(TablesStamp(w.mods, {}) == "coop-tables 1\n");
}

TEST_CASE("rando: a puppet pack made after the seed: the seed's size table is made again, a restart is asked for") {
    // (player report 0.11.0: the other players invisible with the randomizer on, seen with it off: the game reads
    // only the seed's size table, built before the pack, so the puppets' files have no sizes)
    World w;
    {   // Run 1: the seed is built while there is no pack yet.
        CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        w.Settings(loader);
        REQUIRE(loader.Set(kFolder, "seed", "PACK1"));
        REQUIRE(loader.Set(kFolder, "on", "1"));
        rando.Wait();
        REQUIRE(w.runs.size() == 2);
        CHECK(Get(w.mods / "randomizer-PACK1" / kTable) == "game");
        CHECK(fs::is_regular_file(w.mods / "randomizer-PACK1" / kTablesFile));
    }
    // The kit makes the puppet pack; run 2 plays the seed.
    Put(w.mods / "contents" / "0400486024DEC000" / "coop-puppet" / kTable, "puppet");
    Put(w.mods / "randomizer-PACK1" / "romfs" / "System" / "a.bin", "the seed as played");
    PlaysSeed(w, "randomizer-PACK1", 2);
    {
        CoopMods::Loader loader(w.mods, {nullptr, 2, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        rando.Wait();
        REQUIRE(w.runs.size() == 4);  // generate + build again
        CHECK(Get(w.mods / "randomizer-PACK1" / kTable) == "game+puppet");
        CHECK(Get(w.mods / "randomizer-PACK1" / "romfs" / "System" / "a.bin") == "the seed as played");  // only the table
        CHECK(fs::is_regular_file(w.mods / "randomizer-PACK1" / kSeedFile));
        CHECK_FALSE(fs::exists(w.mods / kFolder / "_seed-build"));
        const auto lines = rando.TakeLines();
        CHECK(Count(lines, RefreshLine("PACK1")) == 1);
        CHECK(rando.HoldPack());  // this run read the old table: no puppet until the restart
        const auto why = loader.RestartNeeded().why;
        CHECK(std::find(why.begin(), why.end(), RefreshLine("PACK1")) != why.end());
        CHECK(On(loader, "randomizer-PACK1"));
    }
    {   // Run 3: built beside these tables: nothing to do.
        PlaysSeed(w, "randomizer-PACK1", 3);
        CoopMods::Loader loader(w.mods, {nullptr, 3, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        rando.Wait();
        CHECK(w.runs.size() == 4);
        CHECK(rando.TakeLines().empty());
        CHECK_FALSE(rando.HoldPack());
    }
}

TEST_CASE("rando: a seed from before the stamp whose table is already right is not a restart") {
    World w;
    Put(w.mods / "contents" / "0400486024DEC000" / "coop-puppet" / kTable, "puppet");
    {
        CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        w.Settings(loader);
        REQUIRE(loader.Set(kFolder, "seed", "OLD1"));
        REQUIRE(loader.Set(kFolder, "on", "1"));
        rando.Wait();
    }
    fs::remove(w.mods / "randomizer-OLD1" / kTablesFile);  // as a 0.11.0 seed
    PlaysSeed(w, "randomizer-OLD1", 2);
    CoopMods::Loader loader(w.mods, {nullptr, 2, nullptr});
    loader.Start();
    std::mutex log_mutex;
    std::vector<std::string> log;
    auto hooks = w.Hooks();
    hooks.log = [&](const std::string& l) {
        const std::lock_guard lock(log_mutex);
        log.push_back(l);
    };
    Rando rando(loader, hooks);
    rando.Start();
    rando.Wait();
    CHECK(w.runs.size() == 4);  // checked once
    CHECK(rando.TakeLines().empty());
    CHECK_FALSE(rando.HoldPack());  // held while it was checked, then let go
    const std::lock_guard lock(log_mutex);
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) {
        return l.find("the co-op puppets wait until it is checked") != std::string::npos;
    }));
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) {
        return l.find("size table was right: the co-op puppets are made now") != std::string::npos;
    }));
    CHECK(fs::is_regular_file(w.mods / "randomizer-OLD1" / kTablesFile));
}

TEST_CASE("rando: the restart line comes again when joining and every few minutes until the restart") {
    World w;
    auto clock = std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
    std::mutex clock_mutex;
    auto hooks = w.Hooks();
    hooks.now = [&] {
        const std::lock_guard lock(clock_mutex);
        return clock;
    };
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, hooks);
    rando.Start();
    w.Settings(loader);
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "1"}, {kId, "seed", "REP1"}});
    rando.Wait();
    CHECK(Count(rando.TakeLines(), ReadyLine("REP1")) == 1);  // once, though the build and the switch both ask for it
    CHECK(rando.TakeLines().empty());
    {
        const std::lock_guard lock(clock_mutex);
        clock += kRepeat;
    }
    CHECK(rando.TakeLines() == std::vector<std::string>{ReadyLine("REP1")});
    CHECK(rando.TakeLines().empty());
    rando.Recheck();  // the host's values again (a join)
    rando.Wait();
    CHECK(Count(rando.TakeLines(), ReadyLine("REP1")) == 1);
    // The host switches it off: this run never played it, so no restart is due any more.
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "0"}, {kId, "seed", "REP1"}});
    rando.Recheck();
    rando.Wait();
    (void)rando.TakeLines();
    {
        const std::lock_guard lock(clock_mutex);
        clock += kRepeat * 2;
    }
    CHECK(Count(rando.TakeLines(), ReadyLine("REP1")) == 0);
}

TEST_CASE("rando: a host's failed build is said in its own chat") {
    World w;
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    REQUIRE(loader.Set(kFolder, "ootrr", CoopMods::ToUtf8(w.base / "missing.exe")));
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    const auto lines = rando.TakeLines();
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string& l) {
        return l.rfind("Randomizer: no seed, this game plays without it: OOTRR not found", 0) == 0;
    }));
}

TEST_CASE("rando: a seed that cannot be made again for the puppets is kept, and the chat says why") {
    World w;
    {
        CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        w.Settings(loader);
        REQUIRE(loader.Set(kFolder, "seed", "KEEP1"));
        REQUIRE(loader.Set(kFolder, "on", "1"));
        rando.Wait();
        REQUIRE(w.runs.size() == 2);
    }
    Put(w.mods / "contents" / "0400486024DEC000" / "coop-puppet" / kTable, "puppet");
    PlaysSeed(w, "randomizer-KEEP1", 2);
    w.fail_at = 3;  // the build of the refresh
    Told told;
    auto hooks = w.Hooks();
    hooks.tell_others = told.Hook();
    CoopMods::Loader loader(w.mods, {nullptr, 2, nullptr});
    loader.Start();
    Rando rando(loader, hooks);
    rando.Start();
    rando.Wait();
    CHECK(w.runs.size() == 4);
    CHECK(Get(w.mods / "randomizer-KEEP1" / kTable) == "game");  // kept as it was
    CHECK(fs::is_regular_file(w.mods / "randomizer-KEEP1" / kSeedFile));
    const auto lines = rando.TakeLines();
    CHECK(std::any_of(lines.begin(), lines.end(), [](const std::string& l) {
        return l.rfind("Randomizer: the seed could not be made again for the co-op puppets", 0) == 0;
    }));
    CHECK(told.Get().empty());
    CHECK(rando.HoldPack());  // its table is still the old one
    rando.Recheck();  // not tried again in a loop
    rando.Wait();
    CHECK(w.runs.size() == 4);
}

namespace {
CoopMods::Row Indexed(const std::string& name) {
    CoopMods::Row r = RowNamed(name, true);
    r.on_now = true;
    return r;
}
}  // namespace

TEST_CASE("rando: a size table of a mod not made by us that wins is found (player report 48)") {
    World w;
    Put(w.mods / "ootrr-site" / kTable, "site");
    Put(w.mods / "randomizer-S1" / kTable, "seed");
    Put(w.mods / "aaa-textures" / kTable, "early");
    Put(w.mods / "no-table" / "romfs" / "a.bin", "x");
    Put(w.mods / kPackFolder / kTable, "puppet");
    // With the Mods list: what the game indexed, in its order (coop-mods.applied), the last one wins: the list's rows
    // from the last to the first, under them the mods it does not name, by name (the pack, written with '\').
    using Order = std::vector<std::string>;
    const std::string pack = "contents\\0400486024DEC000\\coop-puppet";
    const Order site_first{pack, "randomizer-S1", "ootrr-site"};
    CHECK(OutsideSizeTable(w.mods, {}, &site_first) == std::optional<std::string>("ootrr-site"));
    const Order seed_first{pack, "ootrr-site", "randomizer-S1"};
    CHECK_FALSE(OutsideSizeTable(w.mods, {}, &seed_first));
    const Order no_table_first{pack, "ootrr-site", "no-table"};
    CHECK(OutsideSizeTable(w.mods, {}, &no_table_first) == std::optional<std::string>("ootrr-site"));
    const Order site_not_read{pack, "randomizer-S1"};  // switched on after this run started: the game did not read it
    CHECK_FALSE(OutsideSizeTable(w.mods, {}, &site_not_read));
    const Order only_pack{pack};
    CHECK_FALSE(OutsideSizeTable(w.mods, {}, &only_pack));
    const Order nothing;
    CHECK_FALSE(OutsideSizeTable(w.mods, {}, &nothing));
    // A mod the list does not name (dropped in since it was written) sits at the pack's rank: by name, under
    // contents/0400486024DEC000/coop-puppet the pack's table wins, above it the mod's.
    const Order unlisted_under{"aaa-textures", pack};
    CHECK_FALSE(OutsideSizeTable(w.mods, {}, &unlisted_under));
    const Order unlisted_over{pack, "ootrr-site"};
    CHECK(OutsideSizeTable(w.mods, {}, &unlisted_over) == std::optional<std::string>("ootrr-site"));
    // Hookshot's own order (no list): by name, the last wins, the pack being contents/0400486024DEC000/coop-puppet.
    CHECK(OutsideSizeTable(w.mods, {Indexed("ootrr-site")}, nullptr) == std::optional<std::string>("ootrr-site"));
    CHECK_FALSE(OutsideSizeTable(w.mods, {Indexed("aaa-textures")}, nullptr));  // under the pack: the pack's wins
    CHECK_FALSE(OutsideSizeTable(w.mods, {Indexed("ootrr-site"), Indexed("randomizer-S1")}, nullptr));
    CoopMods::Row later = Indexed("ootrr-site");
    later.on_now = false;  // not indexed at the start
    CHECK_FALSE(OutsideSizeTable(w.mods, {later}, nullptr));
}

TEST_CASE("rando: the chat lines about an outside size table name the mod and fit a chat line") {
    CHECK(OutsideLine("ootrr-site").find("\"ootrr-site\"") != std::string::npos);
    CHECK(OutsideLine("ootrr-site").find("Mods > OoT Remake Randomizer > Settings") != std::string::npos);
    const std::string tell = OutsideTellLine(std::string(300, 'm'));
    CHECK(tell.size() <= kTellChars);
    CHECK(tell.find(std::string(48, 'm') + "...") != std::string::npos);
    CHECK(OutsideTellLine("s").rfind("[randomizer] ", 0) == 0);
}

TEST_CASE("rando: a run that reads an outside mod's size table holds the pack and tells everyone") {
    World w;
    Put(w.mods / "ootrr-site" / kTable, "site, without the puppets");
    PlaysSeed(w, "ootrr-site", 7);
    CoopMods::Loader loader(w.mods, {nullptr, 7, nullptr});
    loader.Start();
    std::mutex told_mutex;
    std::vector<std::string> told;
    bool connected = false;
    auto hooks = w.Hooks();
    hooks.tell_others = [&](const std::string& line) {
        const std::lock_guard lock(told_mutex);
        if (connected) told.push_back(line);
        return connected;
    };
    Rando rando(loader, hooks);
    rando.Start();
    rando.Wait();
    CHECK(rando.HoldPack());
    CHECK(Count(rando.TakeLines(), ChatSafe(OutsideLine("ootrr-site"), 300)) == 1);
    rando.Recheck();  // alone: nobody to tell yet, the line comes again
    rando.Wait();
    CHECK(Count(rando.TakeLines(), ChatSafe(OutsideLine("ootrr-site"), 300)) == 1);
    {
        const std::lock_guard lock(told_mutex);
        CHECK(told.empty());
        connected = true;
    }
    rando.Recheck();  // joined
    rando.Wait();
    {
        const std::lock_guard lock(told_mutex);
        CHECK(Count(told, OutsideTellLine("ootrr-site")) == 1);
    }
    CHECK(rando.HoldPack());  // the whole run
}

TEST_CASE("rando: FNV-1a 64 as the kit's merge writes it") {
    CHECK(Fnv64Hex("") == "cbf29ce484222325");
    CHECK(Fnv64Hex("a") == "af63dc4c8601ec8c");
    CHECK(Fnv64Hex("foobar") == "85944171f73967e8");
}

namespace {
// The kit's merge (#48 step 2) as it leaves a mod: its table rewritten, and the mark beside its romfs.
void MarkMerged(const fs::path& mods, const std::string& mod, const std::string& table_text) {
    Put(mods / mod / kTable, table_text);
    const std::string pack = Get(mods / kPackFolder / kTable);
    Put(mods / mod / kMergedFile, "coop-merged 1\r\ntable " + kTable.filename().string() + " " +
                                      Fnv64Hex(table_text) + "\r\npack " + Fnv64Hex(pack) + "\r\n");
}
}  // namespace

TEST_CASE("rando: a winning mod whose table the kit merged is not held, a stale mark is") {
    World w;
    Put(w.mods / kPackFolder / kTable, "puppet");
    MarkMerged(w.mods, "ootrr-site", "site + puppet");
    CHECK(MergedByKit(w.mods, "ootrr-site"));
    std::string merged;
    const std::vector<std::string> indexed{"contents/0400486024DEC000/coop-puppet", "ootrr-site"};
    CHECK_FALSE(OutsideSizeTable(w.mods, {}, &indexed, &merged));
    CHECK(merged == "ootrr-site");
    CHECK_FALSE(OutsideSizeTable(w.mods, {Indexed("ootrr-site")}, nullptr));
    // a new seed table dropped over it: stale
    Put(w.mods / "ootrr-site" / kTable, "another site seed");
    CHECK_FALSE(MergedByKit(w.mods, "ootrr-site"));
    CHECK(OutsideSizeTable(w.mods, {}, &indexed) == std::optional<std::string>("ootrr-site"));
    // the pack rebuilt since the merge: stale
    MarkMerged(w.mods, "ootrr-site", "site + puppet");
    Put(w.mods / kPackFolder / kTable, "puppet, rebuilt");
    CHECK_FALSE(MergedByKit(w.mods, "ootrr-site"));
    // a mark of another version, a path for a table name, or no pack: not trusted
    Put(w.mods / kPackFolder / kTable, "puppet");
    MarkMerged(w.mods, "ootrr-site", "site + puppet");
    CHECK(MergedByKit(w.mods, "ootrr-site"));
    const std::string good = Get(w.mods / "ootrr-site" / kMergedFile);
    std::string other = good;
    other.replace(0, std::string("coop-merged 1").size(), "coop-merged 2");
    Put(w.mods / "ootrr-site" / kMergedFile, other);
    CHECK_FALSE(MergedByKit(w.mods, "ootrr-site"));
    Put(w.mods / "ootrr-site" / kMergedFile,
        "coop-merged 1\ntable ../../coop-puppet/romfs/System/Resource/T.rsizetable.zs " + Fnv64Hex("puppet") +
            "\npack " + Fnv64Hex("puppet") + "\n");
    CHECK_FALSE(MergedByKit(w.mods, "ootrr-site"));
    Put(w.mods / "ootrr-site" / kMergedFile, good);
    fs::remove(w.mods / kPackFolder / kTable);
    CHECK_FALSE(MergedByKit(w.mods, "ootrr-site"));
    CHECK_FALSE(MergedByKit(w.mods, "no-such-mod"));
}

TEST_CASE("rando: the mark's rule says what let the table through, an unknown rule is not trusted") {
    World w;
    Put(w.mods / kPackFolder / kTable, "puppet");
    MarkMerged(w.mods, "ootrr-site", "site + puppet");  // (no rule line: an older kit's mark)
    std::string rule;
    CHECK(MergedByKit(w.mods, "ootrr-site", &rule));
    CHECK(rule == "merged");
    const std::string mark = Get(w.mods / "ootrr-site" / kMergedFile);
    // a skins mod in a title's folder whose maker merged the pack's entries in by hand (kit: rule contains)
    const std::string skins = "contents/0400486024DEC000/skins";
    MarkMerged(w.mods, skins, "skins + puppet");
    Put(w.mods / skins / kMergedFile, Get(w.mods / skins / kMergedFile) + "rule contains\r\n");
    rule.clear();
    CHECK(MergedByKit(w.mods, skins, &rule));
    CHECK(rule == "contains");
    std::string merged;
    rule.clear();
    const std::vector<std::string> skins_indexed{"contents/0400486024DEC000/coop-puppet", skins};
    CHECK_FALSE(OutsideSizeTable(w.mods, {}, &skins_indexed, &merged, &rule));  // indexed above the pack
    merged.clear();
    rule.clear();
    CHECK_FALSE(OutsideSizeTable(w.mods, {Indexed(skins)}, nullptr, &merged, &rule));  // by name above the pack
    CHECK(merged == skins);
    CHECK(rule == "contains");
    Put(w.mods / "ootrr-site" / kMergedFile, mark + "rule merged\n");
    CHECK(MergedByKit(w.mods, "ootrr-site", &rule));
    CHECK(rule == "merged");
    Put(w.mods / "ootrr-site" / kMergedFile, mark + "rule trust-me\n");
    CHECK_FALSE(MergedByKit(w.mods, "ootrr-site"));
    const std::vector<std::string> site_indexed{"contents/0400486024DEC000/coop-puppet", "ootrr-site"};
    CHECK(OutsideSizeTable(w.mods, {}, &site_indexed) == std::optional<std::string>("ootrr-site"));
    // rule contains, but the table changed since the kit looked: stale as any mark
    Put(w.mods / skins / kTable, "skins, without the puppet");
    CHECK_FALSE(MergedByKit(w.mods, skins));
}

TEST_CASE("rando: a run that reads a kit-merged outside table makes the puppets and says nothing") {
    World w;
    Put(w.mods / kPackFolder / kTable, "puppet");
    MarkMerged(w.mods, "ootrr-site", "site + puppet");
    PlaysSeed(w, "ootrr-site", 7);
    CoopMods::Loader loader(w.mods, {nullptr, 7, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    rando.Wait();
    CHECK_FALSE(rando.HoldPack());
    CHECK(Count(rando.TakeLines(), ChatSafe(OutsideLine("ootrr-site"), 300)) == 0);
}

TEST_CASE("rando: a run that reads our own seed's size table is not held for an outside mod") {
    World w;
    Put(w.mods / "ootrr-site" / kTable, "site");
    PlaysSeed(w, "contents/0400486024DEC000/coop-puppet", 8);  // only the pack indexed: the site mod came later
    Put(w.mods / "contents" / "0400486024DEC000" / "coop-puppet" / kTable, "puppet");
    CoopMods::Loader loader(w.mods, {nullptr, 8, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    rando.Wait();
    CHECK_FALSE(rando.HoldPack());
    for (const std::string& line : rando.TakeLines()) CHECK(line.find("own size table") == std::string::npos);
}

TEST_CASE("rando: a settings string from OOTRR's page is known, named short, and gets a short folder") {
    CHECK(IsSettingsString("OOTRR1-EYAQCTABAHIHE"));
    CHECK_FALSE(IsSettingsString("OOTRR"));
    CHECK_FALSE(IsSettingsString("default"));
    CHECK_FALSE(IsSettingsString("OOTRR1-AB CD"));
    CHECK_FALSE(IsSettingsString("OOTRR1-..\\x"));
    CHECK_FALSE(IsSettingsString("OOTRR1-" + std::string(kSettingsMax, 'A')));
    CHECK(SettingsString("  OOTRR1-EYAQCTABAHIHE\r\n") == "OOTRR1-EYAQCTABAHIHE");
    CHECK(SettingsString("keysanity").empty());
    CHECK(SettingsString("   ").empty());
    CHECK(SettingsName("keysanity") == "keysanity");
    CHECK(SettingsName("") == "default");
    CHECK(SettingsName("OOTRR1-EYAQCTABAHIHE") == "settings OOTRR1-EYAQCTABA...");
    const std::string folder = FolderFor("AB12", "OOTRR1-EYAQCTABAHIHE");
    CHECK(folder.rfind("randomizer-AB12-s", 0) == 0);
    CHECK(folder.size() == std::string("randomizer-AB12-s").size() + 8);
    CHECK(folder != FolderFor("AB12", "OOTRR1-EYAQCTABAHIHF"));
    CHECK(folder == FolderFor("AB12", "OOTRR1-EYAQCTABAHIHE"));
}

TEST_CASE("rando: the host's settings string is built in place of the preset, in the guest's game too") {
    World w;
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    w.Settings(loader);
    loader.SetHostValues(std::vector<CoopMods::SharedValue>{{kId, "on", "1"},
                                                            {kId, "seed", "SITE1"},
                                                            {kId, "preset", "default"},
                                                            {kId, "settings", "OOTRR1-EYAQCTABAHIHE"}});
    rando.Wait();
    REQUIRE(w.runs.size() == 2);
    CHECK(w.runs[0].find(L"generate") != std::wstring::npos);
    CHECK(w.runs[0].find(L"OOTRR1-EYAQCTABAHIHE") != std::wstring::npos);  // in place of the preset
    CHECK(w.runs[0].find(L"default") == std::wstring::npos);
    const std::string folder = FolderFor("SITE1", "OOTRR1-EYAQCTABAHIHE");
    CHECK(On(loader, folder));
    CHECK(Get(w.mods / folder / kPresetFile) == "OOTRR1-EYAQCTABAHIHE\n");
    // Built once: the same settings again build nothing.
    rando.Recheck();
    rando.Wait();
    CHECK(w.runs.size() == 2);
}

TEST_CASE("rando: a Settings string that is not one is said once and the preset is used") {
    World w;
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    w.Settings(loader);
    (void)rando.TakeLines();
    REQUIRE(loader.Set(kFolder, "settings", "not a string"));
    REQUIRE(loader.Set(kFolder, "seed", "BAD1"));
    REQUIRE(loader.Set(kFolder, "on", "1"));
    rando.Wait();
    std::size_t said = 0;
    for (const std::string& line : rando.TakeLines()) said += line.find("Settings string is not") != std::string::npos;
    CHECK(said == 1);
    CHECK(On(loader, FolderFor("BAD1", "default")));
}

TEST_CASE("rando: a control mod written before the Settings string gets it at the start") {
    World w;
    std::string old = ManifestText();
    const auto from = old.find("    {\"key\": \"settings\"");
    REQUIRE(from != std::string::npos);
    old.erase(from, old.find('\n', old.find("\"help\"", from)) + 1 - from);
    REQUIRE(old.find("\"key\": \"settings\"") == std::string::npos);
    Put(w.mods / kFolder / "mod.json", old);
    CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
    loader.Start();
    Rando rando(loader, w.Hooks());
    rando.Start();
    bool has = false;
    for (const CoopMods::SettingRow& s : loader.Settings(kFolder)) has = has || s.setting.key == "settings";
    CHECK(has);
}

TEST_CASE("rando: the settings string of a seed file") {
    // As OOTRR 0.6.1 writes it (generate default abc); 0.6.2 writes the same layout (its settings/seed.rs is unchanged).
    CHECK(SeedSettings("{\n  \"format\": \"ootrr-seed\",\n  \"version\": 1,\n  \"generator\": \"ootrr-core 0.6.1\",\n"
                       "  \"seed\": \"abc\",\n  \"settings\": \"OOTRR1-EYAQCTABAFQACAMOJY\",\n  \"placements\": {}\n}\n") ==
          "OOTRR1-EYAQCTABAFQACAMOJY");
    CHECK(SeedSettings("{\"settings\" :\r\n\t\"OOTRR1-XZLA\"}") == "OOTRR1-XZLA");
    CHECK(SeedSettings("{\"seed\": \"abc\"}").empty());
    CHECK(SeedSettings("{\"settings\": \"not a string\"}").empty());
    CHECK(SeedSettings("{\"settings\": \"OOTRR1-AB").empty());
    CHECK(SeedSettings("").empty());
}

TEST_CASE("rando: a seed built before spoiler.txt and settings.txt is made again once for them") {
    const Want want{true, "S1", "default"};
    const std::vector<CoopMods::Row> rows{RowNamed("randomizer-S1", true)};
    Built b{"randomizer-S1", "S1", "default", "coop-tables 1\n"};
    CHECK(Decide(want, {b}, rows, std::string("coop-tables 1\n")).what == Plan::Do::nothing);
    b.shared_files = false;
    const Plan p = Decide(want, {b}, rows, std::string("coop-tables 1\n"));
    CHECK(p.what == Plan::Do::refresh);
    CHECK_FALSE(p.for_tables);
    CHECK(p.folder == "randomizer-S1");
    // other size tables too: for those (the files come with it)
    CHECK(Decide(want, {b}, rows, std::string("coop-tables 1\nx 1 0\n")).for_tables);
}

TEST_CASE("rando: every built seed carries OOTRR's spoiler.txt and its settings.txt, kept when made again") {
    // (player report 0.11.3: no spoiler log and no OOTRR1- string to share; host and guests build the same seed)
    World w;
    const std::string folder = "randomizer-SP1";
    {
        CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        w.Settings(loader);
        REQUIRE(loader.Set(kFolder, "seed", "SP1"));
        REQUIRE(loader.Set(kFolder, "on", "1"));
        rando.Wait();
        REQUIRE(w.runs.size() == 2);
        CHECK(w.runs[0].find(L" --spoiler ") != std::wstring::npos);
        CHECK(Get(w.mods / folder / kSpoilerFile) == "Randomized chests\n  KF Kokiri Sword Chest: SP1\n");
        CHECK(Get(w.mods / folder / kSettingsFile) == "OOTRR1-Pdefault\n");
        CHECK(Get(w.mods / kFolder / (folder + ".spoiler.txt")) == Get(w.mods / folder / kSpoilerFile));
    }
    // A seed from before (0.11.3): made again once, in use, with no restart asked and no puppet line.
    fs::remove(w.mods / folder / kSpoilerFile);
    fs::remove(w.mods / folder / kSettingsFile);
    fs::remove(w.mods / kFolder / (folder + ".spoiler.txt"));
    PlaysSeed(w, folder, 2);
    {
        CoopMods::Loader loader(w.mods, {nullptr, 2, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        rando.Wait();
        CHECK(w.runs.size() == 4);
        CHECK(rando.TakeLines().empty());
        CHECK_FALSE(rando.HoldPack());
        CHECK(Get(w.mods / folder / kSpoilerFile) == "Randomized chests\n  KF Kokiri Sword Chest: SP1\n");
        CHECK(Get(w.mods / folder / kSettingsFile) == "OOTRR1-Pdefault\n");
        CHECK(fs::is_regular_file(w.mods / folder / kSeedFile));
    }
    // And not again.
    PlaysSeed(w, folder, 3);
    {
        CoopMods::Loader loader(w.mods, {nullptr, 3, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        rando.Wait();
        CHECK(w.runs.size() == 4);
    }
}

TEST_CASE("rando: a settings string seed's settings.txt is that string, and an OOTRR with no spoiler log does not loop") {
    World w;
    w.spoiler = false;
    const std::string s = "OOTRR1-EYAQCTABAHIHE";
    const std::string folder = FolderFor("SP2", s);
    {
        CoopMods::Loader loader(w.mods, {nullptr, 1, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        w.Settings(loader);
        REQUIRE(loader.Set(kFolder, "settings", s));
        REQUIRE(loader.Set(kFolder, "seed", "SP2"));
        REQUIRE(loader.Set(kFolder, "on", "1"));
        rando.Wait();
        REQUIRE(w.runs.size() == 2);
        CHECK(Get(w.mods / folder / kSettingsFile) == s + "\n");
        CHECK_FALSE(fs::exists(w.mods / folder / kSpoilerFile));
    }
    PlaysSeed(w, folder, 2);
    {
        CoopMods::Loader loader(w.mods, {nullptr, 2, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        rando.Wait();
        CHECK(w.runs.size() == 2);  // settings.txt is there: nothing made again
    }
}

namespace {
const fs::path kActors = fs::path("romfs") / "RSDB" / "ActorInfo.Product.111.rstbl.byml.zs";

void MarkRows(const fs::path& mods, const std::string& mod) {
    Put(mods / mod / kRowsFile, "coop-rows 1\r\ntable " + kActors.filename().string() + " " +
                                    Fnv64Hex(Get(mods / mod / kActors)) + " " +
                                    Fnv64Hex(Get(mods / kPackFolder / kActors)) + "\r\n");
}
}  // namespace

TEST_CASE("rando: a mod whose actor table wins without the pack's rows is found (player report 144/145)") {
    World w;
    Put(w.mods / kPackFolder / kActors, "game rows + CoopPuppet rows");
    Put(w.mods / "randomizer-S1" / kActors, "game rows, the seed's changes");
    Put(w.mods / "aaa-textures" / kActors, "early");
    CHECK(PackActorTables(w.mods) == std::vector<std::string>{kActors.filename().string()});
    using Order = std::vector<std::string>;
    const std::string pack = "contents\\0400486024DEC000\\coop-puppet";
    // Our seeds count too: OOTRR builds their actor tables from the game's, not the pack's.
    const Order seed_last{pack, "randomizer-S1"};
    const auto missing = OutsideActorRows(w.mods, {}, &seed_last);
    REQUIRE(missing);
    CHECK(missing->first == "randomizer-S1");
    CHECK(missing->second == kActors.filename().string());
    const Order pack_last{"randomizer-S1", pack};
    CHECK_FALSE(OutsideActorRows(w.mods, {}, &pack_last));
    const Order only_pack{pack};
    CHECK_FALSE(OutsideActorRows(w.mods, {}, &only_pack));
    // Hookshot's own order (no list): by name, the last wins; under the pack the pack's wins.
    CHECK(OutsideActorRows(w.mods, {Indexed("randomizer-S1")}, nullptr));
    CHECK_FALSE(OutsideActorRows(w.mods, {Indexed("aaa-textures")}, nullptr));
    // The kit's start merged the rows in: trusted while both files are still those bytes.
    MarkRows(w.mods, "randomizer-S1");
    CHECK(RowsMergedByKit(w.mods, "randomizer-S1", kActors.filename().string()));
    CHECK_FALSE(OutsideActorRows(w.mods, {}, &seed_last));
    Put(w.mods / kPackFolder / kActors, "the pack made again");
    CHECK_FALSE(RowsMergedByKit(w.mods, "randomizer-S1", kActors.filename().string()));
    CHECK(OutsideActorRows(w.mods, {}, &seed_last));
    MarkRows(w.mods, "randomizer-S1");
    Put(w.mods / "randomizer-S1" / kActors, "a new seed put in");
    CHECK(OutsideActorRows(w.mods, {}, &seed_last));
    // No pack table of that kind: nothing to carry.
    fs::remove(w.mods / kPackFolder / kActors);
    CHECK_FALSE(OutsideActorRows(w.mods, {}, &seed_last));
}

TEST_CASE("rando: a run whose actor table lacks the pack's rows holds the pack and says why in the chat") {
    World w;
    Put(w.mods / kPackFolder / kActors, "game rows + CoopPuppet rows");
    Put(w.mods / "randomizer-S1" / kActors, "game rows, the seed's changes");
    PlaysSeed(w, "randomizer-S1", 7);
    {
        CoopMods::Loader loader(w.mods, {nullptr, 7, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        rando.Wait();
        CHECK(rando.RowsMissing());
        CHECK(rando.HoldPack());  // (the others show as fairies, not as nothing)
        const std::string line = ChatSafe(RowsLine("randomizer-S1", kActors.filename().string()), 300);
        CHECK(line.find("\"randomizer-S1\" has its own ActorInfo table") != std::string::npos);
        CHECK(Count(rando.TakeLines(), line) == 1);
        CHECK(RowsLine(std::string(80, 'x'), "ActorInfo.Product.111.rstbl.byml.zs").size() <= 300);
    }
    MarkRows(w.mods, "randomizer-S1");
    {
        CoopMods::Loader loader(w.mods, {nullptr, 7, nullptr});
        loader.Start();
        Rando rando(loader, w.Hooks());
        rando.Start();
        rando.Wait();
        CHECK_FALSE(rando.RowsMissing());
    }
}

TEST_CASE("rando: a refreshed seed gets its own actor tables back and loses the kit mark") {
    World w;
    const fs::path seed = w.mods / "randomizer-S1";
    Put(seed / kActors, "merged by the kit");
    Put(seed / "_coop-rows" / kActors.filename(), "the seed own");
    Put(seed / "_coop-rows" / (kActors.filename().string() + ".txt"), "coop-rows 1");
    Put(seed / kRowsFile, "coop-rows 1");
    CHECK(PutBackKeptRows(seed) == 1);
    CHECK(Get(seed / kActors) == "the seed own");
    CHECK_FALSE(fs::exists(seed / kRowsFile));
    CHECK_FALSE(fs::exists(seed / "_coop-rows"));
    CHECK(PutBackKeptRows(seed) == 0);  // (nothing kept: nothing to do)
    CHECK(Get(seed / kActors) == "the seed own");
}
