// SPDX-License-Identifier: GPL-3.0-or-later
// The Mods tab (hookshot/src/coop_mods_tab.*) on a real Loader in a
// temporary mods folder: the list, a mod's page, switching, moving, settings
// of each kind, typing, the host's locked values and the notes.
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mod_temp.h"
#include "coop_mods.h"
#include "coop_mods_tab.h"

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
    // Too long for its max_length: refused with the reason, the old value kept.
    tab.Press(Input::kDecide);
    for (const char c : std::string("123456789")) tab.Char(static_cast<unsigned char>(c));
    tab.Press(Input::kDecide);
    CHECK(m.Value("label") == "Hey");
    CHECK_FALSE(tab.view().gold.empty());
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

TEST_CASE("mods tab: the host's value is locked, and co-op and the randomizer have their lines") {
    Mods m;
    m.loader->SetHostValues(std::vector<CoopMods::SharedValue>{{"hud", "seed", "HOST"}});
    Tab tab({m.loader.get(), [] { return std::vector<std::string>{"Co-op: missing x (the host has it)"}; },
             [] { return std::string("seed AB is ready"); }});
    tab.Show();
    auto v = tab.view();
    CHECK(RowNamed(v, "Co-op: missing x (the host has it)") >= 0);
    CHECK(RowNamed(v, "Randomizer: seed AB is ready") >= 0);
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
    REQUIRE(v.rows.size() == 1);
    CHECK(v.rows[0].name.find("Put each mod in its own folder there.") != std::string::npos);
    CHECK(v.selected == -1);
    fs::remove_all(root, ec);
}
