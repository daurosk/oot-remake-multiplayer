// The Mods tab of Hookshot's menu, right after Multiplayer (docs/mods.md "The
// Mods tab"): every mod as a row with its switch, its own page (on/off, up and
// down, its settings), what needs a restart, how this game's mods differ from
// the co-op host's, and the randomizer's status. The tab's logic (Tab) works
// on a Loader and two lines of text, so the tests drive it in a temporary
// mods folder; the module's part (Page, Input, Key, Char) hands it to
// Hookshot through hos_coop_menu.h as tab 1.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "coop_mods.h"
#include "coop_pause_menu.h"
#include "hos_coop_menu.h"

namespace CoopModsTab {

enum class Input { kUp, kDown, kLeft, kRight, kDecide, kBack, kErase };

class Tab {
public:
    struct Sources {
        CoopMods::Loader* loader = nullptr;
        // Co-op's word on the mods (CoopModShare::Now): lines shown on the list. Empty: none.
        std::function<std::vector<std::string>()> coop;
        // The randomizer's last line (CoopRando::Rando::Status). Empty: none.
        std::function<std::string()> rando;
    };
    explicit Tab(Sources sources);

    // The tab came to show: the folder is read again (a mod dropped in shows). The page stays.
    void Show();
    void Hide();  // typing ends, its text dropped
    bool Press(Input input);  // false: nothing took it (Back on the list: the menu closes)
    void Select(std::size_t row);
    void Click(std::size_t row, int side);  // side -1 / 1: the value's halves, 0: the name
    bool Char(std::uint32_t cp);            // into the text being typed
    bool typing() const { return typing_; }
    bool on_mod_page() const { return !mod_.empty(); }
    CoopPauseMenu::View view() const;

private:
    enum class Act { kNone, kNote, kMod, kOn, kUp, kDown, kSetting };
    struct Line {
        CoopPauseMenu::Row row;
        Act act = Act::kNone;
        std::string name;  // kMod: the mod; kSetting: the setting's key
        std::string help;
    };
    std::vector<Line> Lines() const;
    static bool Selectable(const Line& l);
    void Clamp();
    void Move(int step);
    void Change(const Line& l, int step);  // Left / Right
    void Decide(const Line& l);
    void Say(std::string line);

    Sources src_;
    std::string mod_;  // the mod whose page shows; empty: the list
    std::size_t selected_ = 0;
    std::size_t list_selected_ = 0;  // where the list was, for Back
    bool typing_ = false;
    std::string typed_;
    std::string note_;  // the answer to the last press ("In this co-op game the host sets this.")
};

// The module's part: tab 1 of hos_coop_menu.h. Before CoopModsRuntime::Start (or with no loader): no tab.
bool Available();
const char* TabName();
bool Page(HosCoopMenuPage* out);
bool MenuInput(int event, int arg, int arg2);
bool Key(std::uint32_t vk, bool repeat);  // while typing: the tab's
bool Char(std::uint32_t cp);
bool KeysTaken();

}  // namespace CoopModsTab
