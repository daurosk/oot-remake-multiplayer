// SPDX-License-Identifier: GPL-3.0-or-later
// Character Select's page flags put back after a swap cut short (lichii/hook/coop_charsel_tidy.h, player report 202).
#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <string>

#include "../../lichii/hook/coop_charsel_tidy.h"

using namespace CoopCharselTidy;

namespace {
struct Game {
    std::map<std::string, std::int32_t> flags;   // the mod's flags (none: no mod)
    bool refuse = false;
    int writes = 0;
    Tidy::Read read() {
        return [this](const Flag& f) -> std::optional<std::int32_t> {
            const auto it = flags.find(std::string(f.name));
            if (it == flags.end()) return std::nullopt;
            return it->second;
        };
    }
    Tidy::Write write() {
        return [this](const Flag& f, std::int32_t v) {
            if (refuse) return false;
            ++writes;
            flags[std::string(f.name)] = v;
            return true;
        };
    }
};
Game WithMod() {
    Game g;
    for (const Flag& f : kFlags) g.flags[std::string(f.name)] = 0;
    g.flags["CharacterMenu_Slide"] = 0;      // (his, never written here)
    g.flags["CharacterMenu_SlideOut"] = 0;
    return g;
}
// A swap running on his page: the page shown, the keep-camera and slide flags set.
void Swapping(Game& g) {
    g.flags["CharacterMenu_IsPageOpen"] = 1;
    g.flags["CharacterMenu_KeepUICamera"] = 1;
    g.flags["CharacterMenu_Slide"] = 200;
}
std::vector<std::string> Run(Tidy& t, Game& g, std::optional<bool> menu_up, int frames) {
    std::vector<std::string> all;
    for (int i = 0; i < frames; ++i) {
        for (auto& l : t.Frame(menu_up, g.read(), g.write())) all.push_back(l);
    }
    return all;
}
}  // namespace

TEST_CASE("charsel tidy: Slide and SlideOut are never written, only KeepUICamera and a stuck IsPageOpen") {
    CHECK(kFlags.size() == 2);
    for (const Flag& f : kFlags) {
        CHECK(f.name != "CharacterMenu_Slide");
        CHECK(f.name != "CharacterMenu_SlideOut");
    }
}

TEST_CASE("charsel tidy: a swap running on his page is never touched") {
    Game g = WithMod();
    Swapping(g);
    Tidy t;
    CHECK(Run(t, g, true, 600).empty());
    CHECK(g.writes == 0);
}

TEST_CASE("charsel tidy: his page left mid-swap with the menu still up: cleared on that frame, before any close (row 202)") {
    // R/ZR to Key Items, or another page of the - menu: his exit cleared IsPageOpen, the swap's flags stayed.
    Game g = WithMod();
    Swapping(g);
    Tidy t;
    Run(t, g, true, 10);
    g.flags["CharacterMenu_IsPageOpen"] = 0;
    const auto lines = Run(t, g, true, 1);   // the menu still up: the very next frame
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("CharacterMenu_KeepUICamera was left at 1 after his page closed") != std::string::npos);
    CHECK(g.flags["CharacterMenu_KeepUICamera"] == 0);
    // Slide stays as his swap left it: the next delete with it not 0 deletes the character parked in slot 4 and his
    // page view patch then puts Slide and SlideOut back (his step 5). Zeroed here, slot 4 would keep that character.
    CHECK(g.flags["CharacterMenu_Slide"] == 200);
    CHECK(Run(t, g, true, 600).empty());
    CHECK(Run(t, g, false, 600).empty());
    CHECK(g.writes == 1);
}

TEST_CASE("charsel tidy: B or - closing the menu mid-swap: cleared on the first frame his page reads closed") {
    // MinusMenuToMain clears IsPageOpen in the step that closes the screens; no area load can come before it.
    Game g = WithMod();
    Swapping(g);
    Tidy t;
    Run(t, g, true, 10);
    g.flags["CharacterMenu_IsPageOpen"] = 0;
    CHECK(Run(t, g, false, 1).size() == 1);   // the menu state already says closed: still at once, no wait
    CHECK(g.flags["CharacterMenu_KeepUICamera"] == 0);
    CHECK(g.flags["CharacterMenu_Slide"] == 200);
}

TEST_CASE("charsel tidy: IsPageOpen itself left at 1 is cleared with the rest once no menu has been up a while") {
    Game g = WithMod();
    Swapping(g);
    Tidy t;
    CHECK(Run(t, g, false, kQuietFrames - 1).empty());
    const auto lines = Run(t, g, false, 1);
    CHECK(lines.size() == 2);
    CHECK(g.flags["CharacterMenu_IsPageOpen"] == 0);
    CHECK(g.flags["CharacterMenu_KeepUICamera"] == 0);
    CHECK(g.flags["CharacterMenu_Slide"] == 200);   // his to put right, here too
    // A short look away from the menu (fewer than kQuietFrames) or an unknown menu state never counts.
    Game h = WithMod();
    Swapping(h);
    Tidy t2;
    for (int n = 0; n < 20; ++n) {
        Run(t2, h, false, kQuietFrames - 1);
        Run(t2, h, true, 1);
    }
    CHECK(Run(t2, h, std::nullopt, 600).empty());
    CHECK(h.writes == 0);
}

TEST_CASE("charsel tidy: no mod, a refused write, and a later cut swap") {
    Game none;   // no Character Select: no such flags
    Tidy t0;
    CHECK(Run(t0, none, false, 600).empty());
    CHECK(none.writes == 0);
    Game g = WithMod();
    g.flags["CharacterMenu_KeepUICamera"] = 1;   // IsPageOpen 0
    g.refuse = true;
    Tidy t;
    CHECK(Run(t, g, true, 5).empty());
    g.refuse = false;
    CHECK(Run(t, g, true, 1).size() == 1);
    Swapping(g);
    Run(t, g, true, 10);
    g.flags["CharacterMenu_IsPageOpen"] = 0;
    CHECK(Run(t, g, true, 1).size() == 1);
    CHECK(t.cleared() == 2);
}
