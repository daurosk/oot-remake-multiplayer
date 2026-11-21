// The Mods tab of Hookshot's menu, right after Multiplayer (docs/mods.md "The
// Mods tab"): every mod as a row with its switch (the randomizer's row: its
// name and a Settings button), its own page (on/off, up and down, its
// settings; the randomizer's status on its page), what needs a restart, and
// how this game's mods differ from the co-op host's. The tab's logic (Tab) works
// on a Loader and two lines of text, so the tests drive it in a temporary
// mods folder; the module's part (Page, Input, Key, Char) hands it to
// Hookshot through hos_coop_menu.h as tab 2 (after Multiplayer and the Lobby).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "coop_mods.h"
#include "coop_pause_menu.h"
#include "hos_coop_menu.h"

namespace CoopModsTab {

// A paragraph as the rows a NOTE line is: Hookshot draws a row as one line over
// the row's width and ends what does not fit with "…", so a longer text is cut
// here instead — after a full stop or a comma when one is near the end, at a
// space otherwise, and between characters for the scripts that have no spaces.
// A '\n' in the text starts a new line; empty lines are dropped.
std::vector<std::string> NoteLines(const std::string& text);

enum class Input { kUp, kDown, kLeft, kRight, kDecide, kBack, kErase };

class Tab {
public:
    struct Sources {
        CoopMods::Loader* loader = nullptr;
        // Co-op's word on the mods (CoopModShare::Now): lines shown on the list. Empty: none.
        std::function<std::vector<std::string>()> coop;
        // The randomizer's last line (CoopRando::Rando::Status), on its page. Empty: none.
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
    // Paste. A text row not being typed into asks for the clipboard on Left / Right / a click on its sides
    // (TakePasteRequest: the module reads the clipboard, which may come a moment later, and hands it to Paste
    // with into_typing false: the row takes it whole as its value). While typing, Ctrl+V / Shift+Insert (the
    // module) hand it with into_typing true: it joins the typed text. nullopt: the clipboard could not be read.
    // Text that comes after the player left the field is dropped. The setting's max_length holds in both.
    bool TakePasteRequest();
    void Paste(const std::optional<std::string>& clip, bool into_typing);
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
    static bool IsText(const Line& l);
    std::size_t MaxLength(const std::string& key) const;  // the text setting's max_length (and the 512 cap)
    void AskPaste(const Line& l);

    Sources src_;
    std::string mod_;  // the mod whose page shows; empty: the list
    std::size_t selected_ = 0;
    std::size_t list_selected_ = 0;  // where the list was, for Back
    bool typing_ = false;
    std::string typed_;
    bool paste_wanted_ = false;  // a Paste on a text row waits for the module to read the clipboard
    std::string paste_for_;      // mod + "\n" + key of the row that asked
    std::string note_;  // the answer to the last press ("In this co-op game the host sets this.")
};

// The module's part: tab 2 of hos_coop_menu.h. Before CoopModsRuntime::Start (or with no loader): no tab.
bool Available();
const char* TabName();
bool Page(HosCoopMenuPage* out);
// The tab's page as the pause menu's - page draws it (0.11.9: the Mods tab right of Map): its rows, and whether a
// mod's own page shows (Back goes to the list) rather than the list. False before the tab is made.
bool View(CoopPauseMenu::View* out, bool* on_mod_page);
bool MenuInput(int event, int arg, int arg2);
bool Key(std::uint32_t vk, bool repeat);  // while typing: the tab's
bool Char(std::uint32_t cp);
bool KeysTaken();
// The clipboard text for the Mods tab (coop_pause.cpp reads it): into the field being typed (Ctrl+V /
// Shift+Insert) or onto the selected text row (the pad's Paste). TakePasteRequest: a text row asked (once).
void Paste(const std::optional<std::string>& clip, bool into_typing);
bool TakePasteRequest();

}  // namespace CoopModsTab
