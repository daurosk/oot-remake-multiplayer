// SPDX-License-Identifier: GPL-3.0-or-later
// The mod loader (hookshot/src/coop_mod_*, coop_mods.*, docs/mods.md) without
// the game: the JSON reader, mod.json, the folder scan, the order and switches
// in coop-mods.list as patch 0011's host/mods.c reads them, the settings, the
// hashes, and the Mods tab's Loader in a temporary mods folder.
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "coop_mod_json.h"
#include "coop_mod_library.h"
#include "coop_mods.h"
#include "hos_coop_mods.h"

namespace fs = std::filesystem;
using namespace CoopMods;

namespace {

std::string Slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    std::string out;
    for (const char c : s.str()) {
        if (c != '\r') out += c;
    }
    return out;
}

void Put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

struct TempMods {
    fs::path root;
    explicit TempMods(const char* name) : root(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(root, ec);
        fs::create_directories(root);
    }
    ~TempMods() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    // A mod with one game file.
    void Romfs(const std::string& name, const std::string& file = "UI/a.zs", const std::string& bytes = "x") const {
        Put(root / FromUtf8(name) / "romfs" / FromUtf8(file), bytes);
    }
    void Json(const std::string& name, const std::string& text) const { Put(root / FromUtf8(name) / kManifestFile, text); }
};

// What host/mods.c (patch 0011) indexes with this list text: the same calls it makes, over the mods' names.
std::vector<std::string> ExeOrder(const std::string& list_text, std::vector<std::string> names) {
    auto list = std::make_unique<HosCoopModsList>();
    hos_coop_mods_parse(list_text.data(), list_text.size(), list.get());
    std::vector<std::pair<int, std::string>> ranked;
    for (const auto& n : names) {
        const int rank = hos_coop_mods_rank(list.get(), n.c_str());
        if (rank >= 0) ranked.emplace_back(rank, n);
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first < b.first : NameLess(a.second, b.second);
    });
    std::vector<std::string> out;
    for (auto& [r, n] : ranked) out.push_back(n);
    return out;
}

// The lines a patch adds to a new file, without their '+'.
std::string NewFile(const std::string& patch, const std::string& path) {
    const auto start = patch.find("+++ b/" + path + "\n");
    REQUIRE(start != std::string::npos);
    auto at = patch.find('\n', patch.find("@@", start)) + 1;
    std::string out;
    while (at < patch.size()) {
        const auto end = patch.find('\n', at);
        const std::string line = patch.substr(at, end - at);
        if (line.empty() || line[0] != '+') break;
        out += line.substr(1) + "\n";
        at = end + 1;
    }
    return out;
}

const fs::path kRepo = fs::path(COOP_TEST_SOURCE_DIR) / "..";

}  // namespace

TEST_CASE("mods: the JSON reader is strict and reads back what it writes") {
    using CoopModJson::Parse;
    std::string why;
    const auto v = Parse("\xef\xbb\xbf{\"a\": [1, -2.5e1, true, null, \"x\\u00e9\\ud83d\\ude00\\n\"], \"b\": {}}", &why);
    REQUIRE(v);
    REQUIRE(v->Get("a")->array.size() == 5);
    CHECK(v->Get("a")->array[1].number == -25);
    CHECK(v->Get("a")->array[4].string == "x\xc3\xa9\xf0\x9f\x98\x80\n");
    CHECK(Parse(CoopModJson::Write(*v))->Get("a")->array[4].string == v->Get("a")->array[4].string);
    for (const char* bad : {"", "{", "{\"a\":1,}", "[1,]", "01", "1.", "\"\\x\"", "\"\\ud800\"", "\"a\nb\"", "{} x",
                            "NaN", "1e999", "tru", "{a:1}"}) {
        INFO(bad);
        CHECK_FALSE(Parse(bad, &why));
        CHECK_FALSE(why.empty());
    }
    std::string deep(40, '[');
    deep += std::string(40, ']');
    CHECK_FALSE(Parse(deep));
    CHECK_FALSE(Parse(std::string(CoopModJson::kMaxText + 1, ' ')));
    // The last of a key twice counts.
    CHECK(Parse("{\"k\": 1, \"k\": 2}")->Number("k") == 2);
}

TEST_CASE("mods: mod.json is read strictly and says why it is refused") {
    std::string why;
    const auto m = ParseManifest(R"({
        "id": "ootrr", "name": "Randomizer", "version": "1.2.0", "author": "someone", "coop": "everyone", "api": 1,
        "code": {"windows": "bin/mod.dll", "linux": "bin/libmod.so"},
        "settings": [
          {"key": "logic", "type": "choice", "choices": ["glitchless", {"value": "none", "name": "No logic"}]},
          {"key": "hearts", "type": "number", "min": 1, "max": 20, "default": 3, "restart": true, "shared": true},
          {"key": "seed", "type": "text", "max_length": 16},
          {"key": "fast", "name": "Fast text"}
        ]})",
                                 "Folder", &why);
    REQUIRE_MESSAGE(m, why);
    CHECK(m->id == "ootrr");
    CHECK(m->api == 1);
    CHECK(m->share == Share::Everyone);
#ifdef _WIN32
    CHECK(m->code == "bin/mod.dll");
#else
    CHECK(m->code == "bin/libmod.so");
#endif
    REQUIRE(m->settings.size() == 4);
    CHECK(m->settings[0].initial == "glitchless");
    CHECK(m->settings[0].choices[1].second == "No logic");
    CHECK(m->settings[1].initial == "3");
    CHECK(m->settings[1].restart);
    CHECK(m->settings[3].type == SettingType::Toggle);
    CHECK(m->settings[3].initial == "0");
    CHECK(ValidValue(m->settings[1], "20"));
    CHECK_FALSE(ValidValue(m->settings[1], "21"));
    CHECK_FALSE(ValidValue(m->settings[1], "inf"));
    CHECK_FALSE(ValidValue(m->settings[0], "other"));
    CHECK_FALSE(ValidValue(m->settings[2], std::string(17, 'a')));

    // Without the optional members: the folder's name gives the id and the name.
    const auto plain = ParseManifest("{}", "My Mod!", &why);
    REQUIRE(plain);
    CHECK(plain->id == "my-mod-");
    CHECK(plain->name == "My Mod!");
    CHECK(plain->version == "0");

    for (const char* bad : {
             "[]", R"({"id": "Bad Id"})", R"({"id": "-x"})", R"({"name": ""})", R"({"coop": "some"})",
             R"({"api": 1.5})", R"({"code": "../x.dll"})", R"({"code": "C:/x.dll"})", R"({"code": "/x.dll"})",
             R"({"code": 3})", R"({"settings": [{"key": "A"}]})", R"({"settings": [{"key": "a"}, {"key": "a"}]})",
             R"({"settings": [{"key": "n", "type": "number", "min": 2, "max": 1}]})",
             R"({"settings": [{"key": "c", "type": "choice"}]})",
             R"({"settings": [{"key": "c", "type": "choice", "choices": ["a", "a"]}]})",
             R"({"settings": [{"key": "t", "default": 2}]})", R"({"settings": [{"key": "t", "type": "slider"}]})",
             R"({"name": "a\u0001"})"}) {
        INFO(bad);
        why.clear();
        CHECK_FALSE(ParseManifest(bad, "f", &why));
        CHECK(why.rfind("mod.json: ", 0) == 0);
    }
}

TEST_CASE("mods: ids, names and paths") {
    CHECK(IdFrom("Hyrule HD Textures") == "hyrule-hd-textures");
    CHECK(IdFrom("__x") == "x");
    CHECK(IdFrom("!!!") == "mod");
    CHECK(IdFrom(std::string(80, 'a')).size() == kMaxId);
    CHECK(ValidId(IdFrom("A.b_c-d")));
    CHECK(SafeRelative("bin/mod.dll"));
    CHECK(SafeRelative("bin\\mod.dll"));
    for (const char* bad : {"", "..", "a/../b", "a//b", "/a", "a/", "C:a", "./a"}) {
        INFO(bad);
        CHECK_FALSE(SafeRelative(bad));
    }
    CHECK(NameSame("Pack/One", "pack/one"));
    CHECK(NameLess("alpha", "Beta"));
    CHECK(hos_coop_mods_same("Pack\\One", "pack/one"));
}

TEST_CASE("mods: the folder is scanned as Hookshot's host/mods.c finds mods") {
    TempMods t("coop-mods-scan-test");
    t.Romfs("b-pack");
    t.Romfs("A-pack");
    t.Romfs("group/inner");                       // two levels down
    t.Romfs("-switched-off");                     // Hookshot's own switch
    t.Romfs("coop-puppet-pack");                  // co-op's own: internal
    t.Romfs(".hidden");
    t.Romfs("a/b/c/d/too-deep");                  // five levels: not a mod
    t.Json("code-only", R"({"id": "dup", "code": "x.dll"})");
    Put(t.root / "code-only" / "x.dll", "MZ");
    t.Json("other", R"({"id": "dup"})");
    t.Json("broken", "{");
    Put(t.root / "exe-only" / "exefs" / "main.ips", "x");
    const auto mods = Scan(t.root);
    std::vector<std::string> names;
    for (const auto& m : mods) names.push_back(m.name);
    CHECK(names == std::vector<std::string>{"-switched-off", "A-pack", "b-pack", "broken", "code-only",
                                            "coop-puppet-pack", "exe-only", "group/inner", "other"});
    const auto by = [&](const char* n) {
        return *std::find_if(mods.begin(), mods.end(), [&](const Mod& m) { return m.name == n; });
    };
    CHECK(by("-switched-off").folder_off);
    CHECK(by("coop-puppet-pack").internal);
    CHECK(by("exe-only").has_exefs);
    CHECK_FALSE(by("broken").problem.empty());
    CHECK(by("broken").manifest.id == "broken");   // its game files still count, under its folder's name
    CHECK(by("code-only").manifest.id == "dup");
    CHECK_FALSE(by("code-only").code.empty());
    CHECK(by("other").manifest.id == "dup-2");      // the later one of two with one id
    CHECK(by("other").problem.find("dup") != std::string::npos);
    CHECK(by("group/inner").manifest.id == "inner");
}

TEST_CASE("mods: a code file must exist inside the mod's folder and want no newer interface") {
    TempMods t("coop-mods-code-test");
    t.Json("missing", R"({"code": "nope.dll"})");
    t.Json("newer", R"({"code": "m.dll", "api": 99})");
    Put(t.root / "newer" / "m.dll", "MZ");
    t.Json("folder", R"({"code": "sub"})");
    fs::create_directories(t.root / "folder" / "sub");
    for (const auto& m : Scan(t.root)) {
        INFO(m.name);
        CHECK(m.code.empty());
        CHECK_FALSE(m.problem.empty());
    }
}

TEST_CASE("mods: coop-mods.list gives host/mods.c the tab's order and switches") {
    TempMods t("coop-mods-list-test");
    for (const char* n : {"a", "b", "c", "d"}) t.Romfs(n);
    const auto mods = Scan(t.root);
    State s;
    Normalize(s, mods);
    // A new list changes nothing: the last by name still wins (is indexed last).
    CHECK(s.order == std::vector<std::string>{"d", "c", "b", "a"});
    CHECK(IndexOrder(mods, s) == std::vector<std::string>{"a", "b", "c", "d"});
    CHECK(ExeOrder(ListText(s, mods), {"a", "b", "c", "d"}) == IndexOrder(mods, s));
    CHECK(ExeOrder("", {"b", "a"}) == std::vector<std::string>{"a", "b"});   // no list: Hookshot's own order

    s.order = {"a", "c"};   // a first: it wins; b and d not named yet
    s.off = {"c", "gone"};
    Normalize(s, mods);
    CHECK(s.order == std::vector<std::string>{"a", "c", "d", "b"});
    CHECK(s.off == std::vector<std::string>{"c"});
    const std::string text = ListText(s, mods);
    CHECK(text == "coop-mods 1\non a\noff c\non d\non b\n");
    CHECK(IndexOrder(mods, s) == std::vector<std::string>{"b", "d", "a"});
    CHECK(ExeOrder(text, {"a", "b", "c", "d"}) == IndexOrder(mods, s));
    // A mod dropped in later is not in the list: Hookshot indexes it first (it loses every clash), as Normalize adds it.
    CHECK(ExeOrder(text, {"a", "b", "c", "d", "e"}).front() == "e");

    State back;
    REQUIRE(ReadListText(text, back));
    CHECK(back.order == std::vector<std::string>{"a", "c", "d", "b"});
    CHECK(back.off == std::vector<std::string>{"c"});
    CHECK_FALSE(ReadListText("coop-mods 2\non a\n", back));
    CHECK_FALSE(ReadListText("", back));
}

TEST_CASE("mods: hos_coop_mods_parse skips what it does not know and keeps a name's first row") {
    auto list = std::make_unique<HosCoopModsList>();
    const std::string text = "coop-mods 1\r\non A\r\n\r\nmaybe b\noff a\noff \non\noff c";
    REQUIRE(hos_coop_mods_parse(text.data(), text.size(), list.get()));
    REQUIRE(list->count == 2);
    CHECK(std::string(list->rows[0].name) == "A");
    CHECK(list->rows[0].off == 0);
    CHECK(std::string(list->rows[1].name) == "c");   // the last line needs no '\n'
    CHECK(hos_coop_mods_rank(list.get(), "a") == 2);
    CHECK(hos_coop_mods_rank(list.get(), "C") == -1);
    CHECK(hos_coop_mods_rank(list.get(), "z") == 0);
    const std::string long_name = "coop-mods 1\non " + std::string(HOS_COOP_MODS_NAME, 'x') + "\n";
    REQUIRE(hos_coop_mods_parse(long_name.data(), long_name.size(), list.get()));
    CHECK(list->count == 0);
    std::string many = "coop-mods 1\n";
    for (int i = 0; i < HOS_COOP_MODS_MAX + 10; ++i) many += "on m" + std::to_string(i) + "\n";
    CHECK(many.size() < HOS_COOP_MODS_TEXT_MAX);
    REQUIRE(hos_coop_mods_parse(many.data(), many.size(), list.get()));
    CHECK(list->count == HOS_COOP_MODS_MAX);
}

TEST_CASE("mods: hos_coop_mods.h is the same file in the module and in patch 0011") {
    const std::string patch = Slurp(kRepo / "hookshot" / "patches" / "0011-coop-mods.patch");
    CHECK(NewFile(patch, "host/hos_coop_mods.h") == Slurp(kRepo / "hookshot" / "src" / "hos_coop_mods.h"));
}

TEST_CASE("mods: coop-mods.applied is read as host/mods.c writes it") {
    char line[600];
    std::string text;
    hos_coop_mods_applied_head(line, sizeof line, 4242);
    text += line;
    hos_coop_mods_applied_row(line, sizeof line, "group/inner", 12);
    text += line;
    hos_coop_mods_applied_row(line, sizeof line, "off one", -1);
    text += line;
    const Applied a = ParseApplied(text);
    CHECK(a.valid);
    CHECK(a.pid == 4242);
    REQUIRE(a.on.size() == 1);
    CHECK(a.on[0] == std::pair<std::string, int>{"group/inner", 12});
    CHECK(a.off == std::vector<std::string>{"off one"});
    CHECK_FALSE(ParseApplied("coop-mods-applied 2 1\n").valid);
    CHECK_FALSE(ParseApplied("coop-mods-applied 1 x\n").valid);
}

TEST_CASE("mods: settings are saved by mod id and an invalid saved value falls back to the default") {
    TempMods t("coop-mods-settings-test");
    t.Romfs("r");
    t.Json("r", R"({"id": "rando", "settings": [{"key": "hearts", "type": "number", "min": 1, "max": 9, "default": 3}]})");
    auto mods = Scan(t.root);
    State s;
    s.settings["rando"]["hearts"] = "7";
    s.off = {"r"};
    REQUIRE(Save(t.root, s, mods, nullptr));
    std::vector<std::string> problems;
    State back = Load(t.root, &problems);
    CHECK(problems.empty());
    CHECK(back.settings["rando"]["hearts"] == "7");
    CHECK(back.IsOff("R"));
    CHECK(ValueOf(mods[0], mods[0].manifest.settings[0], back) == "7");
    back.settings["rando"]["hearts"] = "99";
    CHECK(ValueOf(mods[0], mods[0].manifest.settings[0], back) == "3");
    Put(t.root / kSettingsFile, "not json");
    problems.clear();
    back = Load(t.root, &problems);
    CHECK(problems.size() == 1);
    CHECK(back.settings.empty());
}

TEST_CASE("mods: a mod's hash follows its files' names and bytes, and stops when asked") {
    TempMods t("coop-mods-hash-test");
    t.Romfs("one", "UI/a.zs", "abc");
    t.Romfs("two", "ui/A.zs", "abc");   // the same file to the game (paths have no case)
    t.Romfs("three", "UI/a.zs", "abd");
    t.Json("code", R"({"code": "m.dll"})");
    Put(t.root / "code" / "m.dll", "MZ1");
    const auto mods = Scan(t.root);
    const auto hash = [&](const char* name) {
        const auto m = std::find_if(mods.begin(), mods.end(), [&](const Mod& x) { return x.name == name; });
        return HashMod(*m).value();
    };
    const Hashes one = hash("one");
    CHECK(one.files == 1);
    CHECK(one.bytes == 3);
    CHECK(one.code == kNoHash);
    CHECK(one.game_files == hash("two").game_files);
    CHECK(one.game_files != hash("three").game_files);
    const Hashes code = hash("code");
    CHECK(code.game_files == kNoHash);
    CHECK(code.code != kNoHash);
    CHECK(Hex(one.game_files).size() == 32);
    const std::function<bool()> stop = [] { return true; };
    CHECK_FALSE(HashMod(mods[0], &stop));
}

TEST_CASE("mods: the Loader switches, moves and says when a restart is needed") {
    TempMods t("coop-mods-loader-test");
    t.Romfs("a");
    t.Romfs("b");
    t.Json("b", R"({"id": "bee", "name": "Bee", "settings": [
        {"key": "live", "type": "toggle"}, {"key": "boot", "type": "toggle", "restart": true}]})");
    t.Json("c", R"({"id": "cee", "code": "c.dll"})");
    Put(t.root / "c" / "c.dll", "MZ");
    constexpr unsigned long kPid = 777;
    {   // what a hookshot.exe with patch 0011 wrote at this run's start
        char line[400];
        std::string text;
        hos_coop_mods_applied_head(line, sizeof line, kPid);
        text += line;
        for (const char* n : {"a", "b"}) {
            hos_coop_mods_applied_row(line, sizeof line, n, 1);
            text += line;
        }
        Put(t.root / HOS_COOP_MODS_APPLIED_FILE, text);
    }
    std::vector<std::string> log;
    Loader loader(t.root, {[&](const std::string& l) { log.push_back(l); }, kPid});
    loader.Start();
    CHECK(loader.ExeReadsList());
    REQUIRE_FALSE(log.empty());
    CHECK(log[0].rfind("3 mods in ", 0) == 0);
    CHECK(fs::exists(t.root / HOS_COOP_MODS_LIST_FILE));

    // The plugin host loaded c's code.
    REQUIRE(loader.CodeToLoad().size() == 1);
    loader.NoteCode("c", "");
    CHECK_FALSE(loader.RestartNeeded().needed);

    auto rows = loader.Rows();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].name == "c");   // the last by name wins first
    CHECK(rows[0].on_now);
    CHECK(rows[1].title == "Bee");
    CHECK(rows[1].can_move);

    std::string problem;
    REQUIRE(loader.SetOn("a", false, &problem));
    auto restart = loader.RestartNeeded();
    CHECK(restart.needed);
    REQUIRE(restart.why.size() == 1);
    CHECK(restart.why[0].find("off or gone") != std::string::npos);
    REQUIRE(loader.SetOn("a", true, &problem));
    CHECK_FALSE(loader.RestartNeeded().needed);

    REQUIRE(loader.Move("a", -5, &problem));   // to the top: it now wins over b
    CHECK(loader.Rows()[0].name == "a");
    CHECK(loader.RestartNeeded().why == std::vector<std::string>{"The order of the mods changed: it counts from the next start."});
    REQUIRE(loader.Move("a", 5, &problem));
    CHECK_FALSE(loader.RestartNeeded().needed);

    // Settings: a live one is told at once, a restart one asks for a restart.
    std::vector<std::string> told;
    loader.OnSetting([&](const std::string& id, const std::string& key, const std::string& value) {
        told.push_back(id + "." + key + "=" + value);
    });
    REQUIRE(loader.Set("b", "live", "1", &problem));
    CHECK(told == std::vector<std::string>{"bee.live=1"});
    CHECK(loader.Value("bee", "live") == "1");
    CHECK_FALSE(loader.RestartNeeded().needed);
    REQUIRE(loader.Set("b", "boot", "1", &problem));
    CHECK(loader.RestartNeeded().why == std::vector<std::string>{"Bee: \"boot\" changes at the next start."});
    CHECK_FALSE(loader.Set("b", "live", "2", &problem));
    CHECK_FALSE(problem.empty());
    CHECK_FALSE(loader.Set("b", "nope", "1", &problem));
    CHECK_FALSE(loader.SetOn("nope", true, &problem));

    // Code: switched off, it stays loaded until a restart.
    REQUIRE(loader.SetOn("c", false, &problem));
    CHECK(loader.RestartNeeded().why.size() == 2);
    loader.NoteChanged("b", "A new seed: it is used from the next start.");
    CHECK(loader.RestartNeeded().why.back() == "A new seed: it is used from the next start.");

    // A second Loader (the next start) reads back what the first one saved.
    Loader next(t.root, {nullptr, kPid});
    next.Start();
    CHECK(next.Value("bee", "boot") == "1");
    CHECK(next.CodeToLoad().empty());
}

TEST_CASE("mods: without patch 0011 the tab cannot switch or move game files, but code and settings work") {
    TempMods t("coop-mods-old-exe-test");
    t.Romfs("a");
    t.Json("c", R"({"code": "c.dll", "settings": [{"key": "x"}]})");
    Put(t.root / "c" / "c.dll", "MZ");
    Loader loader(t.root, {nullptr, 1});   // no coop-mods.applied: an older hookshot.exe
    loader.Start();
    CHECK_FALSE(loader.ExeReadsList());
    std::string problem;
    CHECK_FALSE(loader.SetOn("a", false, &problem));
    CHECK(problem.find("build the game again") != std::string::npos);
    CHECK_FALSE(loader.Move("a", 1, &problem));
    CHECK(loader.SetOn("c", false, &problem));   // code only: the module itself loads it
    CHECK(loader.Set("c", "x", "1", &problem));
    const auto rows = loader.Rows();
    const auto a = std::find_if(rows.begin(), rows.end(), [](const Row& r) { return r.name == "a"; });
    CHECK_FALSE(a->can_switch);
    CHECK(a->on_now);
    CHECK_FALSE(a->note.empty());
}

TEST_CASE("mods: an applied file from another process is an earlier run's") {
    TempMods t("coop-mods-stale-test");
    t.Romfs("a");
    char line[100];
    hos_coop_mods_applied_head(line, sizeof line, 5);
    Put(t.root / HOS_COOP_MODS_APPLIED_FILE, line);
    Loader loader(t.root, {nullptr, 6});
    loader.Start();
    CHECK_FALSE(loader.ExeReadsList());
}
