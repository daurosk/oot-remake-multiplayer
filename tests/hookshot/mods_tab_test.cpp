// SPDX-License-Identifier: GPL-3.0-or-later
// The Mods tab (hookshot/src/coop_mods_tab.*) on a real Loader in a
// temporary mods folder: the list, a mod's page, switching, moving, settings
// of each kind, typing, the host's locked values and the notes.
#include <doctest/doctest.h>

#include <filesystem>
#include <optional>
#include <fstream>
#include <string>
#include <vector>

#include "mod_temp.h"
#include "coop_mods.h"
#include "coop_mods_tab.h"
#include "coop_rando.h"

namespace fs = std::filesystem;
using CoopModsTab::Input;
using CoopModsTab::Tab;
using CoopPauseMenu::RowKind;

namespace {

void Put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

struct Mods {
    fs::path root = ModTestDir("coop-mods-tab-test");
    std::unique_ptr<CoopMods::Loader> loader;
    Mods() {
        std::error_code ec;
        fs::remove_all(root, ec);
        // patch 0011's exe, this run, indexed both
        Put(root / "coop-mods.applied", "coop-mods-applied 1 1\non 1 a-tunic\non 1 b-hud\n");
        Put(root / "b-hud" / "romfs" / "UI" / "a.zs", "x");
        Put(root / "b-hud" / "mod.json", R"({"id": "hud", "name": "Big HUD", "version": "2", "author": "someone",
            "description": "Bigger hearts.", "settings": [
              {"key": "size", "name": "Size", "type": "number", "min": 1, "max": 3, "default": 2},
              {"key": "look", "type": "choice", "choices": ["plain", {"value": "gold", "name": "Gold"}]},
              {"key": "label", "name": "Label", "type": "text", "max_length": 8},
              {"key": "glow", "name": "Glow", "type": "toggle", "restart": true},
              {"key": "seed", "name": "Seed", "type": "text", "shared": true}]})");
        Put(root / "a-tunic" / "romfs" / "UI" / "a.zs", "y");
        loader = std::make_unique<CoopMods::Loader>(root, CoopMods::Loader::Hooks{nullptr, 1, nullptr});
        loader->Start();
    }
    ~Mods() {
        loader.reset();
        std::error_code ec;
        fs::remove_all(root, ec);
    }
    std::string Value(const std::string& key) const {
        for (const auto& s : loader->Settings("b-hud")) {
            if (s.setting.key == key) return s.value;
        }
        return "?";
    }
};

int RowNamed(const CoopPauseMenu::View& v, const std::string& name) {
    for (std::size_t i = 0; i < v.rows.size(); ++i) {
        if (v.rows[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace

TEST_CASE("mods tab: the list shows each mod with its switch, and Left / Right switch it") {
    Mods m;
    Tab tab({m.loader.get(), nullptr, nullptr});
    tab.Show();
    auto v = tab.view();
    REQUIRE(v.rows.size() == 2);
    CHECK(v.rows[0].name == "Big HUD 2");  // Hookshot's winner first: the name that sorts last
    CHECK(v.rows[0].kind == RowKind::kToggle);
    CHECK(v.rows[0].on);
    CHECK(v.selected == 0);
    CHECK(v.help == "Bigger hearts.");
    tab.Press(Input::kLeft);
    v = tab.view();
    CHECK_FALSE(v.rows[RowNamed(v, "Big HUD 2")].on);
    CHECK(v.gold == "Restart the game to use the changes.");
    CHECK(v.rows[0].kind == RowKind::kNote);  // "Restart needed: ..." on top
    CHECK(v.rows[0].name.rfind("Restart needed: ", 0) == 0);
    tab.Press(Input::kRight);
    CHECK(tab.view().rows[RowNamed(tab.view(), "Big HUD 2")].on);
    CHECK_FALSE(tab.Press(Input::kBack));  // the list: the menu closes
}

TEST_CASE("mods tab: a mod's page moves it and changes each kind of setting") {
    Mods m;
    Tab tab({m.loader.get(), nullptr, nullptr});
    tab.Show();
    tab.Press(Input::kDecide);  // Big HUD's page
    CHECK(tab.on_mod_page());
    auto v = tab.view();
    CHECK(v.rows[0].kind == RowKind::kHeading);
    CHECK(v.rows[0].name == "Big HUD");
    CHECK(RowNamed(v, "By someone, version 2") > 0);
    CHECK(v.rows[v.selected].name == "On");

    tab.Press(Input::kDown);  // Move up: first already
    tab.Press(Input::kDown);  // Move down
    tab.Press(Input::kDecide);
    CHECK(m.loader->Rows()[0].name == "a-tunic");
    tab.Press(Input::kUp);
    tab.Press(Input::kDecide);
    CHECK(m.loader->Rows()[0].name == "b-hud");

    // Size (number 1 to 3): Right, Right stops at 3.
    tab.Press(Input::kDown);
    tab.Press(Input::kDown);  // over the heading
    v = tab.view();
    REQUIRE(v.rows[v.selected].name == "Size");
    CHECK(v.rows[v.selected].left);
    tab.Press(Input::kRight);
    tab.Press(Input::kRight);
    CHECK(m.Value("size") == "3");
    CHECK_FALSE(tab.view().rows[tab.view().selected].right);
    // look (choice): its caption shown.
    tab.Press(Input::kDown);
    tab.Press(Input::kRight);
    v = tab.view();
    CHECK(v.rows[v.selected].value == "Gold");
    CHECK(m.Value("look") == "gold");
    // Label (text): typed, Erase, Enter.
    tab.Press(Input::kDown);
    tab.Press(Input::kDecide);
    CHECK(tab.typing());
    for (const char c : std::string("Hey!")) tab.Char(static_cast<unsigned char>(c));
    tab.Press(Input::kErase);
    CHECK(tab.view().rows[tab.view().selected].value == "Hey_");
    tab.Press(Input::kDecide);
    CHECK_FALSE(tab.typing());
    CHECK(m.Value("label") == "Hey");
    // Too long for its max_length (8): typing stops at the limit (nothing longer can be Done).
    tab.Press(Input::kDecide);
    for (const char c : std::string("123456789")) tab.Char(static_cast<unsigned char>(c));
    tab.Press(Input::kDecide);
    CHECK(m.Value("label") == "Hey12345");
    // Glow (toggle, restart): Confirm flips it, the help says so.
    tab.Press(Input::kDown);
    v = tab.view();
    CHECK(v.help.find("Counts after a restart.") != std::string::npos);
    tab.Press(Input::kDecide);
    CHECK(m.Value("glow") == "1");

    CHECK(tab.Press(Input::kBack));
    CHECK_FALSE(tab.on_mod_page());
    CHECK(tab.view().selected == 0 + 1);  // back on Big HUD's row (under the restart note)
}


TEST_CASE("mods tab: typing stops at the setting's max_length and a long value shows its end") {
    Mods m;
    Tab tab({m.loader.get(), nullptr, nullptr});
    tab.Show();
    tab.Press(Input::kDecide);
    for (int i = 0; i < 4; ++i) tab.Press(Input::kDown);
    tab.Press(Input::kDown);  // Size, look, Label
    auto v = tab.view();
    REQUIRE(v.rows[v.selected].name == "Label");
    tab.Press(Input::kDecide);
    for (const char c : std::string("123456789")) tab.Char(static_cast<unsigned char>(c));
    CHECK(tab.view().rows[tab.view().selected].value == "12345678_");   // max_length 8: the rest is not taken
    tab.Press(Input::kDecide);
    CHECK(m.Value("label") == "12345678");
    // The seed row (no max_length: 64): 100 typed show the END with "..." before it.
    tab.Press(Input::kDown);
    tab.Press(Input::kDown);
    v = tab.view();
    REQUIRE(v.rows[v.selected].name == "Seed");
    tab.Press(Input::kDecide);
    for (int i = 0; i < 100; ++i) tab.Char('a' + i % 26);
    v = tab.view();
    const std::string shown = v.rows[v.selected].value;
    CHECK(shown.rfind("...", 0) == 0);
    CHECK(shown.size() == 40 + 1);                       // 40 characters and the cursor
    CHECK(shown.back() == '_');
    CHECK(v.hint.find("64 bytes") != std::string::npos);
}

TEST_CASE("mods tab: Paste into a text row (the pad's way) and Ctrl+V while typing") {
    Mods m;
    Tab tab({m.loader.get(), nullptr, nullptr});
    tab.Show();
    tab.Press(Input::kDecide);
    for (int i = 0; i < 5; ++i) tab.Press(Input::kDown);
    auto v = tab.view();
    REQUIRE(v.rows[v.selected].name == "Label");
    CHECK(v.change == "Paste");
    // Right asks; the module hands the clipboard: the value is replaced, line breaks and spaces dropped.
    CHECK_FALSE(tab.TakePasteRequest());
    tab.Press(Input::kRight);
    REQUIRE(tab.TakePasteRequest());
    CHECK_FALSE(tab.TakePasteRequest());
    tab.Paste(std::string("  AB\r\nCD \n"), false);
    CHECK(m.Value("label") == "ABCD");
    CHECK(tab.view().gold.rfind("Pasted", 0) == 0);
    // Too long for max_length 8: nothing changes, and it says why.
    tab.Press(Input::kLeft);
    REQUIRE(tab.TakePasteRequest());
    tab.Paste(std::string("123456789"), false);
    CHECK(m.Value("label") == "ABCD");
    CHECK_FALSE(tab.view().gold.empty());
    // The clipboard could not be read: said, the value kept.
    tab.Press(Input::kRight);
    REQUIRE(tab.TakePasteRequest());
    tab.Paste(std::nullopt, false);
    CHECK(m.Value("label") == "ABCD");
    CHECK(tab.view().gold.find("clipboard") != std::string::npos);
    // A paste that comes after the player moved to another row is dropped.
    tab.Press(Input::kRight);
    REQUIRE(tab.TakePasteRequest());
    tab.Press(Input::kUp);
    tab.Paste(std::string("ZZ"), false);
    CHECK(m.Value("label") == "ABCD");
    tab.Press(Input::kDown);
    // Ctrl+V while typing joins the typed text (UTF-8 kept whole, cut on a character's edge at max_length).
    tab.Press(Input::kDecide);
    REQUIRE(tab.typing());
    tab.Paste(std::string("\xC3\xA9\xE4\xBD\xA0X"), true);   // e-acute (2), a CJK character (3), X (1): ABCD + 6 > 8
    CHECK(tab.view().rows[tab.view().selected].value == "ABCD\xC3\xA9_");   // room 4: the e-acute fits, the CJK character (3) does not
    CHECK_FALSE(tab.view().gold.empty());
}

TEST_CASE("mods tab: Paste is only for text rows (a number row still steps)") {
    Mods m;
    Tab tab({m.loader.get(), nullptr, nullptr});
    tab.Show();
    tab.Press(Input::kDecide);
    for (int i = 0; i < 3; ++i) tab.Press(Input::kDown);
    auto v = tab.view();
    REQUIRE(v.rows[v.selected].name == "Size");
    CHECK(v.change == "Switch");
    tab.Press(Input::kRight);
    CHECK_FALSE(tab.TakePasteRequest());   // a number row's sides still step it
    CHECK(m.Value("size") == "3");
    tab.Paste(std::string("x"), false);    // nothing asked, not a text row: nothing happens
    tab.Paste(std::string("x"), true);
    CHECK(m.Value("size") == "3");
}

TEST_CASE("mods tab: the host's value is locked, and co-op has its line") {
    Mods m;
    m.loader->SetHostValues(std::vector<CoopMods::SharedValue>{{"hud", "seed", "HOST"}});
    Tab tab({m.loader.get(), [] { return std::vector<std::string>{"Co-op: missing x (the host has it)"}; },
             [] { return std::string("seed AB is ready"); }});
    tab.Show();
    auto v = tab.view();
    CHECK(RowNamed(v, "Co-op: missing x (the host has it)") >= 0);
    CHECK(RowNamed(v, "Randomizer: seed AB is ready") < 0);  // on the randomizer's page, not the list
    tab.Click(static_cast<std::size_t>(RowNamed(v, "Big HUD 2")), 0);  // the name: its page
    v = tab.view();
    const int seed = RowNamed(v, "Seed");
    REQUIRE(seed > 0);
    CHECK(v.rows[seed].fixed);
    CHECK(v.rows[seed].value == "HOST");
    tab.Click(static_cast<std::size_t>(seed), 0);
    CHECK_FALSE(tab.typing());
    CHECK(tab.view().gold == "In this co-op game the host sets this.");
}

TEST_CASE("mods tab: the randomizer's row is its name and a Settings button that opens its page") {
    Mods m;
    Put(m.root / CoopRando::kFolder / "mod.json", CoopRando::ManifestText());
    m.loader->Refresh();
    Tab tab({m.loader.get(), nullptr, [] { return std::string("seed AB is ready"); }});
    tab.Show();
    auto v = tab.view();
    const int row = RowNamed(v, "OoT Remake Randomizer");  // no version, no Off / On
    REQUIRE(row >= 0);
    CHECK(v.rows[row].kind == RowKind::kAction);
    CHECK(v.rows[row].value == "Settings");
    CHECK_FALSE(v.rows[row].fixed);
    CHECK(RowNamed(v, "Randomizer: seed AB is ready") < 0);
    CHECK(v.rows[RowNamed(v, "Big HUD 2")].kind == RowKind::kToggle);  // the other mods keep their switch
    const auto rando_on = [&] {
        for (const auto& r : m.loader->Rows()) {
            if (r.id == CoopRando::kId) return r.on;
        }
        return false;
    };
    const bool was_on = rando_on();
    tab.Click(static_cast<std::size_t>(row), 1);  // the value's half: the button too, no switch
    CHECK(tab.on_mod_page());
    CHECK(rando_on() == was_on);
    v = tab.view();
    CHECK(RowNamed(v, "Randomizer: seed AB is ready") > 0);
    const int sw = RowNamed(v, "Randomizer");
    REQUIRE(sw > 0);
    CHECK(v.rows[sw].kind == RowKind::kToggle);
    CHECK(tab.Press(Input::kBack));
    CHECK_FALSE(tab.on_mod_page());
    tab.Select(static_cast<std::size_t>(row));
    CHECK(tab.Press(Input::kRight));  // Left / Right on the button change nothing
    CHECK_FALSE(tab.on_mod_page());
    CHECK(tab.Press(Input::kDecide));  // A: its page
    CHECK(tab.on_mod_page());
}

TEST_CASE("mods tab: an empty folder says where mods go") {
    const fs::path root = ModTestDir("coop-mods-tab-empty");
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    CoopMods::Loader loader(root, {nullptr, 1, nullptr});
    loader.Start();
    Tab tab({&loader, nullptr, nullptr});
    tab.Show();
    const auto v = tab.view();
    // The folder's path is long, so the sentence comes as the rows a NOTE line is.
    std::string said;
    for (const auto& row : v.rows) {
        CHECK(row.kind == RowKind::kNote);
        said += row.name;
    }
    REQUIRE_FALSE(v.rows.empty());
    CHECK(said.find("Put each mod in its own folder there.") != std::string::npos);
    CHECK(v.selected == -1);
    fs::remove_all(root, ec);
}

TEST_CASE("mods tab: a text longer than a row is handed over as several note rows") {
    using CoopModsTab::NoteLines;
    // A NOTE row is one line over the row's width (hos_coop_menu.h) and Hookshot ends
    // what does not fit with "…", so a longer text is cut here first.
    const std::string text = "打乱宝箱、迷宫道具和奖励（OOTRR）。联机时所有人玩房主的种子，拾取共享。换种子只需重启游戏，不用重新构建。";
    const std::vector<std::string> lines = NoteLines(text);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "打乱宝箱、迷宫道具和奖励（OOTRR）。联机时所有人玩房主的种子，拾取共享。");
    CHECK(lines[1] == "换种子只需重启游戏，不用重新构建。");

    // A newline is a break of its own; a short text is one row.
    CHECK(NoteLines("Short.") == std::vector<std::string>{"Short."});
    CHECK(NoteLines(std::string("a\nb")) == std::vector<std::string>{"a", "b"});

    // An English text breaks at a space rather than inside a word.
    const std::vector<std::string> words =
        NoteLines("Speeds the game up a little so that a long walk does not feel like one, and stops at once.");
    REQUIRE(words.size() >= 2);
    for (const std::string& line : words) {
        CHECK(line.front() != ' ');
        CHECK(line.back() != ' ');
        CHECK(line.find("  ") == std::string::npos);
    }
}

TEST_CASE("mods tab: a mod's long description shows as several rows of its page") {
    const fs::path root = ModTestDir("coop-mods-tab-long");
    std::error_code ec;
    fs::remove_all(root, ec);
    const std::string description =
        "打乱宝箱、迷宫道具和奖励（OOTRR）。联机时所有人玩房主的种子，拾取共享。换种子只需重启游戏，不用重新构建。";
    Put(root / "long" / "mod.json",
        R"json({"id": "long", "name": "Long", "description": "打乱宝箱、迷宫道具和奖励（OOTRR）。联机时所有人玩房主的种子，拾取共享。换种子只需重启游戏，不用重新构建。"})json");
    {
        CoopMods::Loader loader(root, {nullptr, 1, nullptr});
        loader.Start();
        Tab tab({&loader, nullptr, nullptr});
        tab.Show();
        tab.Press(Input::kDecide);
        REQUIRE(tab.on_mod_page());
        std::string joined;
        int notes = 0;
        for (const auto& row : tab.view().rows) {
            if (row.kind == RowKind::kNote) {
                ++notes;
                joined += row.name;
            }
        }
        CHECK(notes == 2);
        CHECK(joined == description);
    }
    fs::remove_all(root, ec);
}
