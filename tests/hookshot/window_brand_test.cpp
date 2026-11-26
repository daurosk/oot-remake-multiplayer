// SPDX-License-Identifier: GPL-3.0-or-later
// The game window's name and picture (hookshot/src/coop_window_brand.h): the parts that need no window.
#include <doctest/doctest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include "coop_window_brand.h"
#include "../test_temp.h"

TEST_CASE("window brand: the title is Squadala!") {
    CHECK(std::string(CoopWindowBrand::kTitle) == "Squadala!");
    CHECK_FALSE(CoopWindowBrand::TitleNeedsSetting("Squadala!", "Squadala!"));
    CHECK(CoopWindowBrand::TitleNeedsSetting("Longshot", "Squadala!"));
    CHECK(CoopWindowBrand::TitleNeedsSetting("The Legend of Zelda: Ocarina of Time (Longshot)", "Squadala!"));
    CHECK(CoopWindowBrand::TitleNeedsSetting("", "Squadala!"));
    CHECK(CoopWindowBrand::TitleNeedsSetting("Squadala!", "Squadala! v0.11.6"));   // the release added once read
    CHECK_FALSE(CoopWindowBrand::TitleNeedsSetting("Squadala! v0.11.6", "Squadala! v0.11.6"));
}

TEST_CASE("window brand: the title shows the kit's release (0.11.6)") {
    using CoopWindowBrand::ReleaseOf;
    using CoopWindowBrand::TitleFor;
    // make_kit_hookshot.ps1's first line
    CHECK(ReleaseOf("kit: Squadala-v0.11.6-windows (release v0.11.6)") == "v0.11.6");
    CHECK(ReleaseOf("kit: Squadala-v0.11.6-rc1 (release v0.11.6-rc.1)") == "v0.11.6-rc.1");
    CHECK(ReleaseOf("kit: auto-A") == "");                       // a kit made without -Release
    CHECK(ReleaseOf("kit: x (release )") == "");
    CHECK(ReleaseOf("kit: x (release ../../evil)") == "");       // never a path or anything odd on the title
    CHECK(ReleaseOf("kit: x (release v1 <b>)") == "");
    CHECK(ReleaseOf("kit: x (release " + std::string(40, '1') + ")") == "");
    CHECK(TitleFor("v0.11.6") == "Squadala! v0.11.6");
    CHECK(TitleFor("0.11.6") == "Squadala! v0.11.6");
    CHECK(TitleFor("") == "Squadala!");
}

TEST_CASE("window brand: the kit's VERSION.txt is found above the game's folder") {
    namespace fs = std::filesystem;
    const fs::path root = ModTestDir("coop-window-brand-test");
    fs::remove_all(root);
    const fs::path game = root / "kit" / "hookshot" / "game";
    fs::create_directories(game);
    CHECK(CoopWindowBrand::KitRelease(game) == "");                // a developer's build: no kit around it
    {
        std::ofstream(root / "kit" / "VERSION.txt", std::ios::binary)
            << "kit: Squadala-v0.11.6-windows (release v0.11.6)\r\nkind: hookshot\r\n";
    }
    CHECK(CoopWindowBrand::KitRelease(game) == "v0.11.6");
    {
        std::ofstream(game / "VERSION.txt", std::ios::binary) << "not a kit's file\n";   // skipped, the kit's found
    }
    CHECK(CoopWindowBrand::KitRelease(game) == "v0.11.6");
    // Linux: <kit>/data/game/ (start.sh), the kit's VERSION.txt two up
    const fs::path linux_game = root / "lin" / "data" / "game";
    fs::create_directories(linux_game);
    {
        std::ofstream(root / "lin" / "VERSION.txt", std::ios::binary) << "kit: Squadala-v0.11.6-linux (release v0.11.6)\n";
    }
    CHECK(CoopWindowBrand::KitRelease(linux_game) == "v0.11.6");
    fs::remove_all(root);
}

TEST_CASE("window brand: only the game's window class is renamed") {
    CHECK(CoopWindowBrand::IsGameWindowClass("Longshot"));
    CHECK(CoopWindowBrand::IsGameWindowClass("Hookshot"));
    CHECK_FALSE(CoopWindowBrand::IsGameWindowClass("Qt5QWindowIcon"));
    CHECK_FALSE(CoopWindowBrand::IsGameWindowClass(""));
}

TEST_CASE("window brand: icon pixels are bottom-up premultiplied BGRA") {
    // 1 wide, 2 high: top pixel red opaque, bottom pixel half-transparent green
    const std::uint8_t rgba[8] = {255, 0, 0, 255, 0, 200, 0, 128};
    const auto out = CoopWindowBrand::RgbaToIconBgra(rgba, 1, 2);
    REQUIRE(out.size() == 8);
    CHECK(out[0] == 0);  // bottom row first: green * 128 / 255 = 100
    CHECK(out[1] == 100);
    CHECK(out[2] == 0);
    CHECK(out[3] == 128);
    CHECK(out[4] == 0);  // then the red pixel: B G R A
    CHECK(out[5] == 0);
    CHECK(out[6] == 255);
    CHECK(out[7] == 255);
    CHECK(CoopWindowBrand::RgbaToIconBgra(nullptr, 1, 1).empty());
    CHECK(CoopWindowBrand::RgbaToIconBgra(rgba, 0, 2).empty());
}
