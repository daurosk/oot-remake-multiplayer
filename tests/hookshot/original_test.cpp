// SPDX-License-Identifier: GPL-3.0-or-later
// Original game (coop_original.h): the switch, the mods list it writes and puts back, and its F1 tab.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "coop_original.h"
#include "coop_title_menu.h"
#include "hos_coop_menu.h"

namespace fs = std::filesystem;

namespace {

std::string Slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

void Put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

struct Folder {
    fs::path root = fs::temp_directory_path() / "coop-original-test";
    fs::path mods = root / "mods";
    Folder() {
        fs::remove_all(root);
        fs::create_directories(mods / "save-profiles");
        fs::create_directories(mods / "oot-remake-randomizer");
        fs::create_directories(mods / "_parked");
        fs::create_directories(mods / "contents" / "0400486024DEC000" / "coop-puppet" / "romfs");
        fs::create_directories(mods / "contents" / "0400486024DEC000" / "my-texture-mod" / "romfs");
        Put(mods / "save-profiles" / "mod.json", "{}");
    }
    ~Folder() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

}  // namespace

TEST_CASE("original game: the switch in coop-options.ini") {
    CHECK_FALSE(CoopOriginal::OnIn(""));
    CHECK_FALSE(CoopOriginal::OnIn("original_game = off\n"));
    CHECK(CoopOriginal::OnIn("original_game = on\n"));
    CHECK(CoopOriginal::OnIn("# a comment\noriginal_game=ON\n"));
    const std::string on = CoopOriginal::With("pvp = off\n", true);
    CHECK(CoopOriginal::OnIn(on));
    CHECK(on.find("pvp = off") != std::string::npos);
    CHECK_FALSE(CoopOriginal::OnIn(CoopOriginal::With(on, false)));
    // The settings page's row writes the same key (CoopTitleMenu::Options)
    CoopTitleMenu::Options o = CoopTitleMenu::Options::Read("", "");
    CHECK_FALSE(o.original_game);
    o.original_game = true;
    std::string coop, options;
    o.Write(coop, options);
    CHECK(CoopOriginal::OnIn(options));
    CHECK(CoopTitleMenu::Options::Read(coop, options).original_game);
}

TEST_CASE("original game: every mod in the folder is named, the pack and code mods too, never a parked one") {
    Folder f;
    const auto names = CoopOriginal::ModNames(f.mods);
    REQUIRE(names.size() == 4);
    CHECK(names[0] == "contents/0400486024DEC000/coop-puppet");
    CHECK(names[1] == "contents/0400486024DEC000/my-texture-mod");
    CHECK(names[2] == "oot-remake-randomizer");
    CHECK(names[3] == "save-profiles");
    const std::string list = CoopOriginal::OffList(names);
    CHECK(list.rfind("coop-mods 1\n", 0) == 0);
    CHECK(list.find("off contents/0400486024DEC000/coop-puppet\n") != std::string::npos);
    CHECK(list.find("off save-profiles\n") != std::string::npos);
    CHECK(list.find("on ") == std::string::npos);
}

TEST_CASE("original game: on keeps the player's list, off puts it back as it was") {
    Folder f;
    const std::string mine = "coop-mods 1\non oot-remake-randomizer\noff contents/0400486024DEC000/my-texture-mod\n";
    Put(f.mods / "coop-mods.list", mine);
    std::string why;
    REQUIRE(CoopOriginal::ApplyMods(f.mods, true, why));
    const std::string ours = Slurp(f.mods / "coop-mods.list");
    CHECK(ours.find("off contents/0400486024DEC000/coop-puppet") != std::string::npos);
    CHECK(Slurp(f.mods / CoopOriginal::kBackupFile) == mine);
    // A second on (the row pressed twice, a restart) never keeps our own list as the player's
    REQUIRE(CoopOriginal::ApplyMods(f.mods, true, why));
    CHECK(Slurp(f.mods / CoopOriginal::kBackupFile) == mine);
    REQUIRE(CoopOriginal::ApplyMods(f.mods, false, why));
    CHECK(Slurp(f.mods / "coop-mods.list") == mine);
    CHECK_FALSE(fs::exists(f.mods / CoopOriginal::kBackupFile));
    // Off when it never was on: nothing changes
    REQUIRE(CoopOriginal::ApplyMods(f.mods, false, why));
    CHECK(Slurp(f.mods / "coop-mods.list") == mine);
}

TEST_CASE("original game: with no list before, the way back leaves none") {
    Folder f;
    std::string why;
    REQUIRE(CoopOriginal::ApplyMods(f.mods, true, why));
    CHECK(fs::exists(f.mods / "coop-mods.list"));
    REQUIRE(CoopOriginal::ApplyMods(f.mods, false, why));
    CHECK_FALSE(fs::exists(f.mods / "coop-mods.list"));
    CHECK_FALSE(fs::exists(f.mods / CoopOriginal::kBackupFile));
}

TEST_CASE("original game: the F1 tab turns it back, and on again") {
    Folder f;
    const fs::path options = f.root / "coop-options.ini";
    Put(options, "pvp = off\noriginal_game = on\n");
    std::string why;
    REQUIRE(CoopOriginal::ApplyMods(f.mods, true, why));
    CoopOriginal::Tab tab;
    tab.Open(options, f.mods);
    REQUIRE(tab.Shown());
    CHECK(tab.on());
    HosCoopMenuPage page{};
    page.size = sizeof page;
    REQUIRE(tab.Page(&page));
    REQUIRE(page.rows_n >= 2);
    CHECK(page.rows[0].kind == HOS_COOP_MENU_TOGGLE);
    CHECK(page.rows[0].on == 1);
    CHECK(page.rows[1].kind == HOS_COOP_MENU_NOTE);
    // Left: the Off half
    CHECK(tab.Input(HOS_COOP_MENU_LEFT, 0, 0));
    CHECK_FALSE(tab.on());
    CHECK_FALSE(CoopOriginal::OnIn(Slurp(options)));
    CHECK(Slurp(options).find("pvp = off") != std::string::npos);
    CHECK_FALSE(fs::exists(f.mods / "coop-mods.list"));
    REQUIRE(tab.Page(&page));
    CHECK(page.rows[0].on == 0);
    CHECK(std::string(page.rows[1].name).find("Start the game again") != std::string::npos);
    // Left again: still off; a click on the On half: on again
    CHECK(tab.Input(HOS_COOP_MENU_LEFT, 0, 0));
    CHECK_FALSE(tab.on());
    CHECK(tab.Input(HOS_COOP_MENU_CLICK, 0, 1));
    CHECK(tab.on());
    CHECK(CoopOriginal::OnIn(Slurp(options)));
    CHECK(Slurp(f.mods / "coop-mods.list").find("off save-profiles") != std::string::npos);
    // Back: nothing to go back to (Longshot closes the menu)
    CHECK_FALSE(tab.Input(HOS_COOP_MENU_BACK, 0, 0));
}
