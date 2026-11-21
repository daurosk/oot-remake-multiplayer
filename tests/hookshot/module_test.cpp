// SPDX-License-Identifier: GPL-3.0-or-later
// The Hookshot module (hookshot/src) and our patch to Hookshot
// (hookshot/patches): what can be checked without the game.
#include <doctest/doctest.h>

#include <array>
#include <cctype>
#include <cstddef>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "coop_boss_native.h"
#include "coop_counters.h"
#include "coop_world.h"
#include "coop_hold.h"
#include "coop_hookshot.h"
#include "coop_null_page.h"
#include "hos_coop_save_load.h"
#include "hos_coop_crash.h"
#include "hos_coop_sound.h"
#include "hos_coop_world.h"
#include "hos_coop_actors.h"
#include "hos_coop_nodes.h"
#include "hos_coop_bosses.h"
#include "coop_svc.h"

namespace {

std::string ReadFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    std::string text = s.str();
    std::string out;  // without carriage returns
    for (const char c : text) {
        if (c != '\r') out += c;
    }
    return out;
}

const std::filesystem::path kRepo = std::filesystem::path(COOP_TEST_SOURCE_DIR) / "..";

std::string Patch() {
    return ReadFile(kRepo / "hookshot" / "patches" / "0001-coop-bridge.patch");
}

// The lines a patch adds to a new file, without their '+'.
std::string NewFile(const std::string& patch, const std::string& path) {
    const auto start = patch.find("+++ b/" + path + "\n");
    REQUIRE(start != std::string::npos);
    auto at = patch.find("@@", start);
    at = patch.find('\n', at) + 1;
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

// `text` with a later patch's hunks for `path` applied (unified diff: context
// ' ', removed '-', added '+' lines; each hunk at its "@@ -line" in the text
// as the earlier hunks left it). Empty when a hunk does not match.
std::string Applied(const std::string& text, const std::string& patch, const std::string& path) {
    std::vector<std::string> lines;
    for (std::size_t at = 0; at < text.size();) {
        const auto end = text.find('\n', at);
        lines.push_back(text.substr(at, end - at));
        at = end == std::string::npos ? text.size() : end + 1;
    }
    auto at = patch.find("+++ b/" + path + "\n");
    REQUIRE(at != std::string::npos);
    at = patch.find('\n', at) + 1;
    long shift = 0;
    while (at < patch.size() && patch.compare(at, 3, "@@ ") == 0) {
        const long old_start = std::stol(patch.substr(at + 4));
        at = patch.find('\n', at) + 1;
        std::size_t pos = static_cast<std::size_t>(old_start - 1 + shift);
        while (at < patch.size()) {
            const auto end = patch.find('\n', at);
            const std::string line = patch.substr(at, end - at);
            if (line.empty() || (line[0] != ' ' && line[0] != '-' && line[0] != '+')) break;
            if (line[0] == '+') {
                lines.insert(lines.begin() + static_cast<long>(pos++), line.substr(1));
                ++shift;
            } else {
                if (pos >= lines.size() || lines[pos] != line.substr(1)) return {};
                if (line[0] == '-') {
                    lines.erase(lines.begin() + static_cast<long>(pos));
                    --shift;
                } else {
                    ++pos;
                }
            }
            at = end + 1;
        }
    }
    std::string out;
    for (const auto& l : lines) out += l + "\n";
    return out;
}

}  // namespace

TEST_CASE("hookshot: the API header in our patches is hookshot/src's") {
    // 0001 makes it, 0004 (the title menu) and 0032 (the Settings tab) add to it.
    const std::string first = NewFile(Patch(), "host/hos_coop_api.h");
    const std::string title = ReadFile(kRepo / "hookshot" / "patches" / "0004-coop-title-menu.patch");
    const std::string settings = ReadFile(kRepo / "hookshot" / "patches" / "0032-coop-settings.patch");
    CHECK(Applied(Applied(first, title, "host/hos_coop_api.h"), settings, "host/hos_coop_api.h") ==
          ReadFile(kRepo / "hookshot" / "src" / "hos_coop_api.h"));
}

TEST_CASE("hookshot: patch 0032 hands the module the Insert menu's settings, the control bridge's own two") {
    const std::string p = ReadFile(kRepo / "hookshot" / "patches" / "0032-coop-settings.patch");
    CHECK(p.find("+    api.setting = bridge_setting, api.setting_set = bridge_setting_set;") != std::string::npos);
    CHECK(p.find("+    if (!out || !rx_control_setting(index, &s)) return 0;") != std::string::npos);
    // (the control bridge's own answer: 1 set, 0 no such row, -1 fixed, -2 unreachable; never a -1 or -2 as set)
    CHECK(p.find("+    const int r = rx_control_setting_set(key, value, &s);\n+    if (r && out) setting_copy(out, &s);\n"
                 "+    return r;") != std::string::npos);
    CHECK(p.find("+_Static_assert(sizeof(HosCoopSetting) == sizeof(HosControlSetting)") != std::string::npos);
    // (the bridge, the API header and the microphones' list: no lift hook)
    CHECK(p.find("diff --git a/nx2/") == std::string::npos);
    CHECK(p.find("diff --git a/host/oot_menu.c") == std::string::npos);   // (five tabs: the mouse's own table)
    // The microphones listed as the menu lists them when it opens, at an enumeration's start, when their count moved.
    CHECK(p.find("+    if (index == 0 && can_mic() && std::chrono::steady_clock::now() - mics_asked > std::chrono::seconds(2)) {") !=
          std::string::npos);
    CHECK(p.find("+        if (std::min(host->mic_count(), kMaxMics) != mic_listed) refresh_mics();") != std::string::npos);
    // A key the bridge handed out cut to 31 bytes is the row's it starts.
    CHECK(p.find("+    while (i < kRows && !(bridge_row(i) && bridge_key_is(ini_key(rows[i]), key))) i++;") != std::string::npos);
    // The module's table reads them only when the exe's table is that long (an older Hookshot goes without).
    static_assert(offsetof(HosCoopApi, setting) > offsetof(HosCoopApi, scratch));
    static_assert(offsetof(HosCoopApi, setting_set) > offsetof(HosCoopApi, setting));
    static_assert(sizeof(HosCoopSetting) == 32 + 48 + 64 + 16 + 6 * sizeof(int));
}

TEST_CASE("hookshot: patch 0004 hooks the title screen, hands the pad over and sets x8 for calls") {
    const std::string p = ReadFile(kRepo / "hookshot" / "patches" / "0004-coop-title-menu.patch");
    CHECK(p.find("+    if (connected) buttons = hos_coop_pad_filter(buttons, stick);") != std::string::npos);
    CHECK(p.find("+    coop_title_hooks.install(lifter, img, build_id)") != std::string::npos);
    CHECK(p.find("+SITES = ((0x3406940, (0xf9401660, 0xb0004961), 1, 19),") != std::string::npos);
    CHECK(p.find("+    c->x[8] = (saved.x[31] - 0xb0) & ~0xfULL;") != std::string::npos);
    CHECK(p.find("+    coop_pad = (hos_coop_pad_fn)(void *)GetProcAddress(m, \"hos_coop_pad\");") != std::string::npos);
    CHECK(p.find("+    coop_title = (hos_coop_title_fn)(void *)GetProcAddress(m, \"hos_coop_title\");") != std::string::npos);
}

TEST_CASE("hookshot: the title menu needs patch 0004's table and can be turned off") {
    HosCoopApi api{};
    api.size = sizeof api;
    CHECK_FALSE(CoopHookshot::TitleMenuPossible(&api, ""));   // no scratch call
    api.scratch = [](void*, const void*, size_t) -> uint64_t { return 0; };
    CHECK(CoopHookshot::TitleMenuPossible(&api, ""));
    CHECK(CoopHookshot::TitleMenuPossible(&api, "1"));
    CHECK_FALSE(CoopHookshot::TitleMenuPossible(&api, "0"));
    api.size = offsetof(HosCoopApi, scratch);   // an older Hookshot's table
    CHECK_FALSE(CoopHookshot::TitleMenuPossible(&api, ""));
    CHECK_FALSE(CoopHookshot::TitleMenuPossible(nullptr, ""));
}

TEST_CASE("hookshot: the voice header in patch 0009 is hookshot/src's") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0009-coop-voice.patch");
    CHECK(NewFile(patch, "host/hos_coop_voice.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_voice.h"));
    // The mixer's hunk and the module's exports it looks for.
    CHECK(patch.find("hos_coop_voice_mix(ch, vn, FRAME);") != std::string::npos);
    CHECK(patch.find("\"hos_coop_voice_attach\"") != std::string::npos);
    CHECK(patch.find("\"hos_coop_voice_render\"") != std::string::npos);
}

TEST_CASE("hookshot: the patch's hook sites are the emulator cave's") {
    const std::string hooks = NewFile(Patch(), "nx2/coop_hooks.py");
    // The frame hook goes after the `mov x0, x20` before each of the cave's calls.
    for (const auto& h : CoopSvc::kHooks) {
        char site[64];
        std::snprintf(site, sizeof site, "(0x%llx, 0x%08x, ", static_cast<unsigned long long>(h.offset - 4), h.original);
        CHECK_MESSAGE(hooks.find(site) != std::string::npos, site);
    }
    char hold[48];
    std::snprintf(hold, sizeof hold, "HOLD_CALL = 0x%llx", static_cast<unsigned long long>(CoopHold::kHookOffset));
    CHECK(hooks.find(hold) != std::string::npos);
    char word[32];
    std::snprintf(word, sizeof word, "HOLD_WORDS = (0x%08x,", CoopHold::kHookOriginal);
    CHECK(hooks.find(word) != std::string::npos);
    CHECK(hooks.find("MAIN = 0x80020000") != std::string::npos);
}

TEST_CASE("hookshot: the module knows the game build by main's id as Hookshot gives it") {
    CHECK(CoopHookshot::BuildSupported("e0f3ccfaf50065ae4ea01eb5aeb13b1a"));
    CHECK(CoopHookshot::BuildSupported("E0F3CCFAF50065AE"));
    CHECK_FALSE(CoopHookshot::BuildSupported("d00e4f399e411d0613aa8d26b1c02fef"));
    CHECK_FALSE(CoopHookshot::BuildSupported("e0f3cc"));
    CHECK_FALSE(CoopHookshot::BuildSupported(""));
}

TEST_CASE("hookshot: Link-model puppets are not made (they crash Hookshot's game), the rest are") {
    // Plain and coloured, both ages: the crash is the same for all of them.
    CHECK(CoopHookshot::RefuseCreate("CoopPuppet_Child", false));
    CHECK(CoopHookshot::RefuseCreate("CoopPuppet_Adult", false));
    CHECK(CoopHookshot::RefuseCreate("CoopPuppet_Child_Red", false));
    CHECK(CoopHookshot::RefuseCreate("CoopPuppet_Adult_Cyan", false));
    // The fairy, the shot and ride stand-ins and a chosen other actor still are.
    CHECK_FALSE(CoopHookshot::RefuseCreate("Dm_Npc_Navi", false));
    CHECK_FALSE(CoopHookshot::RefuseCreate("CoopShot_Arrow", false));
    CHECK_FALSE(CoopHookshot::RefuseCreate("CoopRide_Epona", false));
    CHECK_FALSE(CoopHookshot::RefuseCreate("Animal_Cow_LinkHouse", false));
    CHECK_FALSE(CoopHookshot::RefuseCreate("", false));
    // LICHII_COOP_HOOKSHOT_LINK_PUPPETS=1: made again (to check a Hookshot fix).
    CHECK_FALSE(CoopHookshot::RefuseCreate("CoopPuppet_Child_Red", true));
}

TEST_CASE("hookshot: Link puppets follow the puppet guard in longshot.exe") {
    // Unset: on with patch 0002's guard in the exe, off without it.
    CHECK(CoopHookshot::AllowLinkPuppets("", true));
    CHECK_FALSE(CoopHookshot::AllowLinkPuppets("", false));
    // LICHII_COOP_HOOKSHOT_LINK_PUPPETS forces it either way.
    CHECK(CoopHookshot::AllowLinkPuppets("1", false));
    CHECK_FALSE(CoopHookshot::AllowLinkPuppets("0", true));
}

TEST_CASE("hookshot: no Link puppet where the null page could not be armed (player report 131)") {
    // Armed (or Linux): made. Not armed: refused, every held piece and the puppet's own update read through null.
    CHECK_FALSE(CoopHookshot::RefuseUnarmed("CoopPuppet_Child_Green", true, ""));
    CHECK(CoopHookshot::RefuseUnarmed("CoopPuppet_Child_Green", false, ""));
    CHECK(CoopHookshot::RefuseUnarmed("CoopPuppet_ChildQuiet_Red", false, "1"));
    // HOS_COOP_NULL_READS=0 asks for the faults (a test): still made.
    CHECK_FALSE(CoopHookshot::RefuseUnarmed("CoopPuppet_Adult", false, "0"));
    // Not a Link puppet (the fairy, gear): never refused here.
    CHECK_FALSE(CoopHookshot::RefuseUnarmed("Dm_Npc_Navi", false, ""));
    CHECK_FALSE(CoopHookshot::RefuseUnarmed("Weapon_Range_SlingShot", false, ""));
}

TEST_CASE("hookshot: the module's null page covers the guest's first MB only (issue 150)") {
    using CoopNullPage::GuestOffset;
    CHECK(GuestOffset(0x1000000007d8ULL) == std::optional<std::uint64_t>(0x7d8));  // the perception crash's read
    CHECK(GuestOffset(0x100000000000ULL) == std::optional<std::uint64_t>(0));
    CHECK(GuestOffset(0x1000000fffffULL) == std::optional<std::uint64_t>(0xfffff));
    CHECK_FALSE(GuestOffset(0x100000100000ULL));  // past the MB: a real fault
    CHECK_FALSE(GuestOffset(0x0fffffffffffULL));
    CHECK_FALSE(GuestOffset(0x7d8));
}

TEST_CASE("hookshot: perceived puppets only where null reads can give 0 (issue 150)") {
    using CoopNullPage::Armable;
    using CoopNullPage::Mb;
    CHECK(Armable("", Mb::Reserved));   // the module commits it
    CHECK(Armable("1", Mb::Committed));  // the exe's guard (or Linux) did already
    CHECK_FALSE(Armable("0", Mb::Reserved));  // HOS_COOP_NULL_READS=0
    CHECK_FALSE(Armable("", Mb::Other));
}

TEST_CASE("hookshot: the null page is armed before the first puppet create reaches the game (issue 150)") {
    const std::string src = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    const auto arm = src.find("return !ArmNullPage(actor);");
    const auto call = src.find("g_api->call(ctx, &k, &x0)");
    REQUIRE(arm != std::string::npos);
    REQUIRE(call != std::string::npos);
    CHECK(arm < call);  // inside Refused(), which runs before the call
    CHECK(src.find("CoopPuppetConfig::Configure(PerceivedSafe())") != std::string::npos);
}

TEST_CASE("hookshot: the module refuses puppet creates before they reach the game") {
    const std::string src = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    const auto refused = src.find("if (Refused(c))");
    const auto call = src.find("g_api->call(ctx, &k, &x0)");
    REQUIRE(refused != std::string::npos);
    REQUIRE(call != std::string::npos);
    CHECK(refused < call);
    CHECK(src.find("LICHII_COOP_HOOKSHOT_LINK_PUPPETS") != std::string::npos);
}

TEST_CASE("hookshot: the module builds every hook file the emulator builds, but the cave") {
    const std::string cmake = ReadFile(kRepo / "hookshot" / "CMakeLists.txt");
    CHECK(cmake.find("lichii/hook/core.cmake") != std::string::npos);
    CHECK(cmake.find("NOT name STREQUAL \"coop_svc.cpp\"") != std::string::npos);
    // The test commands are built in (the autotest driver's place, give, ...).
    CHECK(cmake.find("NOT name STREQUAL \"coop_testinput.cpp\"") == std::string::npos);
    // The stand-ins for what the cave did.
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("std::string Refusal()") != std::string::npos);
    CHECK(module.find("CoopCallers::RunFrame") != std::string::npos);
    CHECK(module.find("namespace CoopTestInput") == std::string::npos);  // the real one is built
}

TEST_CASE("hookshot: the puppets' settings are the emulator's, not a copy") {
    // A shortened copy in the module dropped LICHII_COOP_TUNICS and
    // LICHII_COOP_SELF_TUNIC on Hookshot (issue 202): both call one function.
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    const std::string svc = ReadFile(kRepo / "lichii" / "hook" / "coop_svc.cpp");
    CHECK(module.find("CoopPuppetConfig::Configure(PerceivedSafe())") != std::string::npos);
    CHECK(svc.find("CoopPuppetConfig::Configure()") != std::string::npos);
    CHECK(module.find("PlannerSettings") == std::string::npos);
    CHECK(svc.find("PlannerSettings") == std::string::npos);
    const std::string cmake = ReadFile(kRepo / "lichii" / "hook" / "core.cmake");
    CHECK(cmake.find("coop_puppet_config.cpp") != std::string::npos);
}

TEST_CASE("hookshot: test commands come from the driver's file, the pad stays the emulator's") {
    const std::string input = ReadFile(kRepo / "lichii" / "hook" / "coop_testinput.cpp");
    CHECK(input.find("\"LICHII_COOP_TEST_COMMANDS\"") != std::string::npos);
    CHECK(input.find("\"LICHII_COOP_TEST_SCRIPT\"") != std::string::npos);
    // The hid headers only for the emulator.
    const auto hid = input.find("#include \"hid_core/");
    REQUIRE(hid != std::string::npos);
    const auto guard = input.rfind("#ifndef COOP_HOOKSHOT", hid);
    CHECK(guard != std::string::npos);
    CHECK(hid - guard < 30);
    // The driver writes the same variable.
    const std::string driver = ReadFile(kRepo / "tools" / "autotest" / "hookshot.py");
    CHECK(driver.find("LICHII_COOP_TEST_COMMANDS") != std::string::npos);
}

namespace {
std::uint64_t TestPad() {
    return 0xC5;  // A, X, Y, L, R
}
}  // namespace

TEST_CASE("hookshot: own Ocarina notes read the pad word Hookshot gave the game (issue 152)") {
    // The table: an older bridge (no pad_buttons) still starts co-op, without notes.
    HosCoopApi api{};
    api.version = HOS_COOP_API_VERSION;
    api.size = static_cast<std::uint32_t>(CoopHookshot::kFirstApiSize);
    CHECK_FALSE(CoopHookshot::HasPadButtons(&api));
    api.size = sizeof api;
    CHECK_FALSE(CoopHookshot::HasPadButtons(&api));  // no function
    api.pad_buttons = TestPad;
    CHECK(CoopHookshot::HasPadButtons(&api));
    CHECK_FALSE(CoopHookshot::HasPadButtons(nullptr));
    // key_down (Linux's keys) is after it: an older table has none.
    CHECK_FALSE(CoopHookshot::HasKeyDown(&api));
    api.key_down = [](std::uint32_t vk) { return vk == 0x75 ? 1 : 0; };
    CHECK(CoopHookshot::HasKeyDown(&api));
    api.size = static_cast<std::uint32_t>(offsetof(HosCoopApi, key_down));
    CHECK_FALSE(CoopHookshot::HasKeyDown(&api));
    CHECK(CoopHookshot::HasPadButtons(&api));
    // The patch keeps the word nn::hid gives the game and hands it over.
    const std::string patch = Patch();
    CHECK(patch.find("+volatile uint64_t hos_coop_pad;") != std::string::npos);
    CHECK(patch.find("+    if (connected) __atomic_store_n(&hos_coop_pad, word, __ATOMIC_RELAXED);") != std::string::npos);
    CHECK(patch.find("+        G64(e + 8) = word;") != std::string::npos);
    CHECK(patch.find("+    api.pad_buttons = bridge_pad_buttons;") != std::string::npos);
    // The hook asks for it on Hookshot instead of sending nothing.
    const std::string sound = ReadFile(kRepo / "lichii" / "hook" / "coop_ocarina_sound.cpp");
    CHECK(sound.find("CoopRuntime::PadButtons(buttons)") != std::string::npos);
    CHECK(sound.find("so our own notes are not sent") == std::string::npos);
}

TEST_CASE("hookshot: co-op's per-frame work runs at each present, the game calls at the frame site") {
    using CoopHookshot::WorkAt;
    // A Hookshot from an older patch: everything at the every-frame site.
    CHECK(WorkAt(HOS_COOP_SITE_FRAME, false).on_frame);
    CHECK(WorkAt(HOS_COOP_SITE_FRAME, false).calls);
    // Presents seen: OnFrame there only (as the emulator's composite), calls stay at the frame site.
    CHECK(WorkAt(HOS_COOP_SITE_PRESENT, true).on_frame);
    CHECK_FALSE(WorkAt(HOS_COOP_SITE_PRESENT, true).calls);
    CHECK_FALSE(WorkAt(HOS_COOP_SITE_FRAME, true).on_frame);
    CHECK(WorkAt(HOS_COOP_SITE_FRAME, true).calls);
    // The rare site does nothing, as before.
    CHECK_FALSE(WorkAt(HOS_COOP_SITE_RARE, false).on_frame);
    CHECK_FALSE(WorkAt(HOS_COOP_SITE_RARE, true).calls);
    // The patch calls the present site from the game's present, only once the module is loaded.
    const std::string patch = Patch();
    CHECK(patch.find("+    hos_coop_present();") != std::string::npos);
    CHECK(patch.find("+    if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2) coop_frame(NULL, HOS_COOP_SITE_PRESENT);") !=
          std::string::npos);
}

TEST_CASE("hookshot: co-op draws over the picture through Hookshot's ImGui (chat lines, name tags)") {
    // The draw table in our patch is hookshot/src's.
    const std::string patch = Patch();
    CHECK(NewFile(patch, "gpu/vk/hos_coop_draw.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_draw.h"));
    // The renderer finds the module's two exports and draws in the menu's ImGui frame.
    const std::string vk = NewFile(patch, "gpu/vk/vk_coop.cpp");
    CHECK(vk.find("\"hos_coop_draw_wanted\"") != std::string::npos);
    CHECK(vk.find("\"hos_coop_draw\"") != std::string::npos);
    CHECK(patch.find("+    if (coop) coop::draw(w, h);") != std::string::npos);
    // Hookshot's build gives the ImGui headers only to the files it lists: vk_coop.cpp is one (else it does not compile),
    // after Longshot's own (rc.6 lists its crash window, vk_crash_window.cpp, too).
    CHECK(patch.find("+        imgui_users = (\"vk_imgui.cpp\", \"vk_menu.cpp\", \"vk_crash_window.cpp\", \"vk_coop.cpp\")") !=
          std::string::npos);
    // The module has them.
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("COOP_HOOKSHOT_EXPORT int hos_coop_draw_wanted(void)") != std::string::npos);
    CHECK(module.find("COOP_HOOKSHOT_EXPORT void hos_coop_draw(const HosCoopDraw* d)") != std::string::npos);
    // The game look: the renderer prefers hos_coop_draw2 and hands it the font and picture calls; the module has it.
    CHECK(vk.find("\"hos_coop_draw2\"") != std::string::npos);
    CHECK(vk.find("font_load, use_font, image, draw_image}") != std::string::npos);
    CHECK(vk.find("AddFontFromFileTTF") != std::string::npos);
    CHECK(vk.find("ImGui::RegisterUserTexture(t)") != std::string::npos);
    CHECK(module.find("COOP_HOOKSHOT_EXPORT void hos_coop_draw2(const HosCoopDraw* d)") != std::string::npos);
}

TEST_CASE("hookshot: the chat box gets Hookshot's keys first, and the game none while it types") {
    const std::string patch = Patch();
    CHECK(patch.find("+        if (hos_coop_key((unsigned)wp, (lp & (1 << 30)) != 0)) {") != std::string::npos);
    // (Longshot rc.4 has its own WM_CHAR for the Insert menu's text fields: the module is asked first inside it)
    CHECK(patch.find("+        if (!trail && hos_coop_char((unsigned)wp)) return 0;") != std::string::npos);
    CHECK(patch.find("+static int key(int vk) { return !rx_option(1, 0) && !hos_coop_keys_taken() &&") != std::string::npos);
    CHECK(patch.find("GetProcAddress(m, \"hos_coop_keys_taken\")") != std::string::npos);
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("COOP_HOOKSHOT_EXPORT int hos_coop_key(uint32_t vk, int repeat)") != std::string::npos);
    CHECK(module.find("COOP_HOOKSHOT_EXPORT int hos_coop_char(uint32_t ch)") != std::string::npos);
    CHECK(module.find("COOP_HOOKSHOT_EXPORT int hos_coop_keys_taken(void)") != std::string::npos);
}

TEST_CASE("hookshot: the mouse reaches the cheats panel first, a click it takes never turns the camera") {
    const std::string patch = Patch();
    CHECK(patch.find("+    coop_mouse = (hos_coop_mouse_fn)(void *)GetProcAddress(m, \"hos_coop_mouse\");") != std::string::npos);
    CHECK(patch.find("+        if (coop_mouse(w, (POINT){(short)LOWORD(lp), (short)HIWORD(lp)}, 1)) return 0;") != std::string::npos);
    CHECK(patch.find("+        if (coop_mouse(w, at, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 2 : 3)) return 0;") != std::string::npos);
    CHECK(patch.find("+            if (mouse_captured && hos_coop_keys_taken()) mouse_capture(w, 0);") != std::string::npos);
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("COOP_HOOKSHOT_EXPORT int hos_coop_mouse(float fx, float fy, int event)") != std::string::npos);
}

TEST_CASE("hookshot: on Linux the window's keys, typed text and mouse reach co-op too (display_sdl.c)") {
    const std::string patch = Patch();
    // SDL's text events are Longshot's own since rc.4 (hos_sdl.c); text input stays on while co-op has the keys.
    CHECK(patch.find("+            const int text_wanted = hos_text_focus() || hos_coop_keys_taken();") != std::string::npos);
    CHECK(patch.find("+            if (text_wanted != text_on) hos_sdl_text_input(window, text_on = text_wanted);") !=
          std::string::npos);
    CHECK(patch.find("diff --git a/host/hos_sdl.c") == std::string::npos);
    // Keys by virtual-key code first, text as code points, the mouse as on Windows.
    CHECK(patch.find("+                    if (cvk && hos_coop_key(cvk, e.repeat)) {") != std::string::npos);
    CHECK(patch.find("+                    if (!hos_text_focus()) coop_text(e.text);") != std::string::npos);
    CHECK(patch.find("+                    else if (e.button == 1 && !mouse_captured && coop_mouse(e.x, e.y, 1)) {}") !=
          std::string::npos);
    // The key state for F6, F7 and the closing keys.
    CHECK(patch.find("+    api.key_down = bridge_key_down;") != std::string::npos);
}

TEST_CASE("hookshot: the game's camera reaches the module for the name tags (no camera, no tags)") {
    // The block as the game fills it: the target's size at 0x120, the projection's w row (.., -1, 0) at 0xa0..0xac.
    std::array<float, 0x130 / 4> block{};
    block[0x120 / 4] = 1440.0f;
    block[0x124 / 4] = 810.0f;
    block[0xa8 / 4] = -1.0f;
    block[CoopHookshot::kCameraMatrix] = 1.5f;  // the world-to-clip matrix starts at 0x30
    CHECK(CoopHookshot::kCameraMatrix == 12);
    CHECK(CoopHookshot::CameraBlockOk(block.data(), 1440.0f, 810.0f));
    CHECK_FALSE(CoopHookshot::CameraBlockOk(block.data(), 1920.0f, 1080.0f));  // another target's block
    CHECK_FALSE(CoopHookshot::CameraBlockOk(nullptr, 1440.0f, 810.0f));
    block[0xa8 / 4] = 0.0f;  // not a perspective projection (a shadow or HUD pass)
    CHECK_FALSE(CoopHookshot::CameraBlockOk(block.data(), 1440.0f, 810.0f));
    // The patch: vk_draw.cpp hands each scene draw to vk_coop.cpp, which finds the module's two exports.
    const std::string patch = Patch();
    CHECK(patch.find("+        coop::note_scene_draw(at.color[0], at.width, at.height, s.ubo[0][0].addr, s.ubo[0][0].size);") !=
          std::string::npos);
    const std::string vk = NewFile(patch, "gpu/vk/vk_coop.cpp");
    CHECK(vk.find("\"hos_coop_camera_wanted\"") != std::string::npos);
    CHECK(vk.find("\"hos_coop_camera\"") != std::string::npos);
    CHECK(vk.find("color0->format != VK_FORMAT_B10G11R11_UFLOAT_PACK32") != std::string::npos);
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("COOP_HOOKSHOT_EXPORT int hos_coop_camera_wanted(uint64_t frame, float width, float height)") !=
          std::string::npos);
    CHECK(module.find("COOP_HOOKSHOT_EXPORT void hos_coop_camera(uint64_t frame, const float* block, float width, float height)") !=
          std::string::npos);
}

TEST_CASE("hookshot: the world hooks (patch 0005) hand the pause counter starts to the module") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0005-coop-world-hooks.patch");
    CHECK(NewFile(patch, "host/hos_coop_world.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_world.h"));
    // The lift hook calls the world hook with the counter start's number, at the start function the cave patches.
    char call[64];
    std::snprintf(call, sizeof call, "hos_coop_world_event(%uu, c->x[0], 0)", HOS_COOP_WORLD_COUNTER_START);
    CHECK(patch.find(call) != std::string::npos);
    const std::string lift = ReadFile(kRepo / "hookshot" / "patches" / "0003-coop-platforms.patch");
    char word[48];
    std::snprintf(word, sizeof word, "((0x%llx, 0x%08x),", static_cast<unsigned long long>(CoopCounters::kStartOffset),
                  CoopCounters::kStartOriginal);
    CHECK(lift.find(word) != std::string::npos);
    // The guest ring is gone on Hookshot; the bridge keeps the export only once the module says it is on.
    CHECK(patch.find("deleted file mode 100644") != std::string::npos);
    CHECK(patch.find("--- a/host/coop_platform_ring.c") != std::string::npos);
    CHECK(patch.find("w(HOS_COOP_WORLD_HELLO, HOS_COOP_WORLD_VERSION, hos_coop_wrap_mask) != 1") != std::string::npos);
    // The hello's level: the first patch sent 0, 0 and had the flag applier's wrap; later ones their version and wraps.
    CHECK(CoopHookshot::HelloLevel(0, 0).version == 1);
    CHECK(CoopHookshot::HelloLevel(0, 0).wraps == 1ull << HOS_COOP_WRAP_FLAG_APPLY);
    CHECK(CoopHookshot::HelloLevel(HOS_COOP_WORLD_VERSION, 0x6).version == HOS_COOP_WORLD_VERSION);
    CHECK(CoopHookshot::HelloLevel(HOS_COOP_WORLD_VERSION, 0x6).wraps == 0x6);
    // Only a function the lift made is wrapped, a wrapped one stays hot, and the build's wraps go out as bits.
    CHECK(patch.find("the lift made no function there") != std::string::npos);
    CHECK(patch.find("+            if hot and \"f_%08x\" % e in hot:") != std::string::npos);
    CHECK(patch.find("const uint64_t hos_coop_wrap_mask = %#xull;") != std::string::npos);
    // The wraps: every call reaches the wrapper, the emit workers get the table, and the first wrap's id is the header's.
    CHECK(patch.find("return \"hos_coop_w_%08x\" % t") != std::string::npos);
    CHECK(patch.find("\"wraps\")   # (oot-remake-multiplayer's patch 0005)") != std::string::npos);
    char wrap[48];
    std::snprintf(wrap, sizeof wrap, "(%u, 0xb99980, (0xd503237f,", HOS_COOP_WRAP_FLAG_APPLY);
    CHECK(NewFile(patch, "nx2/coop_world_wraps.py").find(wrap) != std::string::npos);
    CHECK(patch.find("+    coop_world_wraps.install(lifter, img, build_id)") != std::string::npos);
    // Its install is not at the end of coop_hooks.py's install(), where each later patch adds its own line (0004, 0006,
    // 0007): the patches then apply in any order.
    CHECK(patch.find("+    coop_world_wraps.install(lifter, img, build_id)\n     import coop_puppet_guard") !=
          std::string::npos);
    // The event request's wrap, although Hookshot edits that function's head: only a call looked for by name bars a wrap.
    std::snprintf(wrap, sizeof wrap, "(%u, 0xd7a820, (0xb940b808,", HOS_COOP_WRAP_EVENT_REQUEST);
    CHECK(NewFile(patch, "nx2/coop_world_wraps.py").find(wrap) != std::string::npos);
    CHECK(patch.find("(\"f_%08x(c)\", \"TAILCALL(f_%08x)\")") != std::string::npos);
    // The time set's wrap: the function every time change of the game goes through.
    std::snprintf(wrap, sizeof wrap, "(%u, 0xeee640, (0xd503237f,", HOS_COOP_WRAP_TIME_SET);
    CHECK(NewFile(patch, "nx2/coop_world_wraps.py").find(wrap) != std::string::npos);
}

TEST_CASE("hookshot: the actor events (patch 0007) use the world hook with the module's event numbers") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0007-coop-actor-events.patch");
    const std::string lift = NewFile(patch, "nx2/coop_actor_lift.py");
    REQUIRE(!lift.empty());
    char call[160];
    std::snprintf(call, sizeof call, "hos_coop_world_event(%uu, c->x[30], c->x[2]); hos_coop_world_event(%uu, c->x[1], c->x[3])",
                  HOS_COOP_WORLD_CREATE_FROM, HOS_COOP_WORLD_CREATE_REQUEST);
    CHECK(lift.find(call) != std::string::npos);
    // A refused create leaves through the game's own "no creates now" exit, whose words are checked too.
    std::snprintf(call, sizeof call,
                  "hos_coop_world_event(%uu, (uint64_t)(uintptr_t)&coop_stop, 0); if (coop_stop) goto L_80392c88;",
                  HOS_COOP_WORLD_CREATE_STOP);
    CHECK(lift.find(call) != std::string::npos);
    CHECK(lift.find("(0x372a9c, 0x37080f68)") != std::string::npos);  // tbnz w8, #1, main+0x372c88
    CHECK(lift.find("(0x372c88, 0xb4000137)") != std::string::npos);
    CHECK(HOS_COOP_WORLD_CREATE_STOP <= HOS_COOP_WORLD_ACTOR_LAST);
    // The shoot code names the shooter before its create (its x1 holds the actor first).
    std::snprintf(call, sizeof call, "hos_coop_world_event(%uu, c->x[1], c->x[0])", HOS_COOP_WORLD_SHOOT_FROM);
    CHECK(lift.find(call) != std::string::npos);
    CHECK(lift.find("(0xc16d24, 0xaa0103f6)") != std::string::npos);
    CHECK(lift.find("(0xc172fc, 0x97dd6ddd)") != std::string::npos);  // its create call: the module's kShootCreateReturn - 4
    CHECK(HOS_COOP_WORLD_SHOOT_FROM <= HOS_COOP_WORLD_ACTOR_LAST);
    std::snprintf(call, sizeof call, "hos_coop_world_event(%uu, c->x[19], c->x[20])", HOS_COOP_WORLD_DELETE_REQUEST);
    CHECK(lift.find(call) != std::string::npos);
    std::snprintf(call, sizeof call,
                  "hos_coop_world_event(%uu, c->x[19], (uint64_t)(uintptr_t)&coop_sum); c->x[9] = (uint32_t)coop_sum;",
                  HOS_COOP_WORLD_LIFE_STEP);
    CHECK(lift.find(call) != std::string::npos);
    // The three sites, each after its checked word; the install is called from coop_hooks.py.
    CHECK(lift.find("(0x372a94, 0xf94037b7)") != std::string::npos);
    CHECK(lift.find("(\"delete request\", 0xa65d0,") != std::string::npos);  // (where the request is accepted)
    CHECK(lift.find("(0xa65d0, 0xf94002c0)") != std::string::npos);
    CHECK(lift.find("(0x49dcb0, 0x0b090149)") != std::string::npos);
    // The life stops: the same step, before its stop test; the module's 1 clears w10 (no stops this frame).
    std::snprintf(call, sizeof call,
                  "hos_coop_world_event(%uu, c->x[19], (uint64_t)(uintptr_t)&coop_pass); if (coop_pass) c->x[10] = 0;",
                  HOS_COOP_WORLD_LIFE_STOPS);
    CHECK(lift.find(call) != std::string::npos);
    CHECK(lift.find("(\"life stops\", 0x49dcbc,") != std::string::npos);
    CHECK(lift.find("(0x49dcb4, 0xb940726a)") != std::string::npos);  // ldr w10, [x19, #0x70]: the stops' count
    CHECK(lift.find("(0x49dcc0, 0x35006eaa)") != std::string::npos);  // cbnz w10, main+0x49ea94
    CHECK(HOS_COOP_WORLD_LIFE_STOPS <= HOS_COOP_WORLD_ACTOR_LAST);
    // The revive: the same step at life 0, before its revive check; the module's 1 skips it (x0 = 0, on where the
    // step goes when the check answers 0: main+0x49dd18, a label of the same function).
    std::snprintf(call, sizeof call, "hos_coop_world_event(%uu, c->x[19], (uint64_t)(uintptr_t)&coop_dead); ",
                  HOS_COOP_WORLD_LIFE_REVIVE);
    CHECK(lift.find(call) != std::string::npos);
    CHECK(lift.find("if (coop_dead) { c->x[0] = 0; goto L_804bdd18; }") != std::string::npos);
    CHECK(lift.find("(\"life revive\", 0x49ed0c,") != std::string::npos);
    CHECK(lift.find("(0x49ed10, 0x97fffa68)") != std::string::npos);  // bl main+0x49d6b0: the revive check
    CHECK(lift.find("(0x49ed18, 0x54ff800b)") != std::string::npos);  // b.lt main+0x49dd18: the goto's target
    CHECK(HOS_COOP_WORLD_LIFE_REVIVE <= HOS_COOP_WORLD_ACTOR_LAST);
    CHECK(HOS_COOP_WORLD_LIFE_REVIVE != HOS_COOP_WORLD_LIFE_STOPS);
    CHECK(patch.find("+    coop_actor_lift.install(lifter, img, build_id)") != std::string::npos);
    // New numbers follow the world hook's own (0 hello, 1 counter start).
    CHECK(HOS_COOP_WORLD_CREATE_REQUEST > HOS_COOP_WORLD_COUNTER_START);
}

TEST_CASE("hookshot: the boss wraps take the next ids of the one wrap space (native boss sync)") {
    // (patch 0005's wraps are 1 to 3; the hook table's rows 4 and 5 are the bosses')
    CHECK(HOS_COOP_WRAP_BRAIN_VERB == HOS_COOP_WRAP_EVENT_REQUEST + 1);
    CHECK(HOS_COOP_WRAP_ACTION_VERB == HOS_COOP_WRAP_BRAIN_VERB + 1);
    CHECK(HOS_COOP_WRAP_ACTION_VERB < 64);  // (one bit each in HELLO's b)
}

TEST_CASE("hookshot: patch 0013 adds the two boss rows to the wrap table, with the module's ids and addresses") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0013-coop-boss-wraps.patch");
    // Hookshot's copy of the ids is the module's file.
    CHECK(NewFile(patch, "host/hos_coop_bosses.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_bosses.h"));
    // The rows: the id, the function the module calls for a copy, and its first words (checked by the lift).
    char row[96];
    std::snprintf(row, sizeof row, "+    (%u, 0x%llx, (0xd503237f, 0xa9bd7bfd, 0xf9000bf5, 0xa9024ff4)),",
                  HOS_COOP_WRAP_BRAIN_VERB, static_cast<unsigned long long>(CoopBossNative::addr::kSetBrainVerb));
    CHECK(patch.find(row) != std::string::npos);
    std::snprintf(row, sizeof row, "+    (%u, 0x%llx, (0xd503237f, 0xd10743ff, 0xa9187bfd, 0xf900cbfc)),",
                  HOS_COOP_WRAP_ACTION_VERB, static_cast<unsigned long long>(CoopBossNative::addr::kRequestActionVerb));
    CHECK(patch.find(row) != std::string::npos);
    // It changes the table only (no lift code, no host code): the wrap machinery stays patch 0005's.
    CHECK(patch.find("diff --git a/nx2/coop_world_wraps.py b/nx2/coop_world_wraps.py") != std::string::npos);
    std::size_t files = 0;
    for (std::size_t at = patch.find("\ndiff --git "); at != std::string::npos;
         at = patch.find("\ndiff --git ", at + 1)) {
        ++files;
    }
    CHECK(files == 2);
    // The table's file is patch 0005's: 0013 applies after it.
    CHECK(ReadFile(kRepo / "hookshot" / "patches" / "0005-coop-world-hooks.patch")
              .find("+++ b/nx2/coop_world_wraps.py") != std::string::npos);
}

TEST_CASE("hookshot: the Multiplayer tab leads Hookshot's menu (patch 0012, hos_coop_menu.h)") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0012-coop-menu-tabs.patch");
    REQUIRE_FALSE(patch.empty());
    // The table in our patch is hookshot/src's.
    CHECK(NewFile(patch, "gpu/vk/hos_coop_menu.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_menu.h"));
    // Hookshot looks for the module's five exports (four, or no tabs) and leads its strip with the tabs.
    for (const char* name : {"\"hos_coop_menu_tabs\"", "\"hos_coop_menu_tab_name\"", "\"hos_coop_menu_page\"",
                             "\"hos_coop_menu_input\"", "\"hos_coop_menu_wanted\""}) {
        CHECK(patch.find(name) != std::string::npos);
    }
    CHECK(patch.find("+        draw_coop_page(c, hh);") != std::string::npos);
    CHECK(patch.find("+        coop_tick(press, tab_left, tab_right, mouse ? &ptr : nullptr, mx, my, moved, changed);") !=
          std::string::npos);
    // Only the Vulkan back end's files: no game rebuild (no lift), the same Hookshot build as the other patches.
    for (std::size_t at = patch.find("diff --git a/"); at != std::string::npos; at = patch.find("diff --git a/", at + 1)) {
        CHECK(patch.compare(at, 20, "diff --git a/gpu/vk/") == 0);
    }
    // The module has the exports.
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    for (const char* name : {"COOP_HOOKSHOT_EXPORT int hos_coop_menu_tabs(void)",
                             "COOP_HOOKSHOT_EXPORT const char* hos_coop_menu_tab_name(int tab)",
                             "COOP_HOOKSHOT_EXPORT int hos_coop_menu_page(int tab, HosCoopMenuPage* out)",
                             "COOP_HOOKSHOT_EXPORT int hos_coop_menu_input(int tab, int event, int arg, int arg2)",
                             "COOP_HOOKSHOT_EXPORT int hos_coop_menu_wanted(void)"}) {
        CHECK(module.find(name) != std::string::npos);
    }
}

TEST_CASE("hookshot: a module tab may draw a picture beside its page (patch 0019, hos_coop_menu_picture.h)") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0019-coop-menu-picture.patch");
    REQUIRE_FALSE(patch.empty());
    CHECK(NewFile(patch, "gpu/vk/hos_coop_menu_picture.h") ==
          ReadFile(kRepo / "hookshot" / "src" / "hos_coop_menu_picture.h"));
    // Hookshot looks for the optional export and draws the shapes in the help column.
    CHECK(patch.find("look(\"hos_coop_menu_picture\")") != std::string::npos);
    CHECK(patch.find("+        if (coop::menu_picture(coop_tab, &pic, where, ww / wh)) {") != std::string::npos);
    // A live picture: the dim leaves its square out.
    CHECK(patch.find("+    if (!(tune_mode && page == 0)) dim_game(c);") != std::string::npos);
    // The camera (hos_coop_view.h): the bridge looks for the optional export, and the gameplay view asks it before the
    // Debug tab's free camera.
    CHECK(NewFile(patch, "host/hos_coop_view.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_view.h"));
    CHECK(patch.find("GetProcAddress(m, \"hos_coop_view\")") != std::string::npos);
    CHECK(patch.find("+    if (oot_coop_view(c, view)) return;") != std::string::npos);
    // The Vulkan back end's files and the host's (the bridge, the view the game's own routine already calls): no lift,
    // no game code, no new hook in the game.
    for (std::size_t at = patch.find("diff --git a/"); at != std::string::npos; at = patch.find("diff --git a/", at + 1)) {
        bool allowed = false;
        for (const std::string prefix : {"diff --git a/gpu/vk/", "diff --git a/host/coop_bridge.c ",
                                         "diff --git a/host/hos_main.c ", "diff --git a/host/hos_coop_view.h "}) {
            allowed = allowed || patch.compare(at, prefix.size(), prefix) == 0;
        }
        CHECK(allowed);
    }
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("COOP_HOOKSHOT_EXPORT int hos_coop_menu_picture(int tab, HosCoopMenuPicture* out)") !=
          std::string::npos);
    CHECK(module.find("COOP_HOOKSHOT_EXPORT int hos_coop_view(double eye[3], double target[3])") != std::string::npos);
}

namespace {

// s without spaces, tabs and line ends.
std::string Squashed(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            out += c;
        }
    }
    return out;
}

// A patched file's code as the patch leaves it (its hunks' '+' and ' ' lines, without the diff's first column),
// with every space, tab and line end taken out: a search matches the code, not the diff's layout, so a refreshed
// patch (other context, a statement that moved from '+' to context) still matches while the code is the same.
std::string PatchedCode(const std::string& patch, const std::string& path) {
    std::string code;
    auto at = patch.find("+++ b/" + path + "\n");
    if (at == std::string::npos) {
        return code;
    }
    at = patch.find('\n', at) + 1;
    while (at < patch.size()) {
        const auto end = patch.find('\n', at);
        const std::string line = patch.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (line.rfind("diff --git ", 0) == 0 || line == "-- ") {
            break;   // (the next file, or the patch's end)
        }
        if (!line.empty() && (line[0] == '+' || line[0] == ' ')) {
            code += line.substr(1);
        }
        if (end == std::string::npos) {
            break;
        }
        at = end + 1;
    }
    return Squashed(code);
}

}  // namespace

TEST_CASE("hookshot: the Discord status header in patch 0026 is hookshot/src's, and the module gives its line") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0026-coop-discord-status.patch");
    REQUIRE_FALSE(patch.empty());
    CHECK(NewFile(patch, "host/hos_discord_activity.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_discord_activity.h"));
    const std::string rpc = PatchedCode(patch, "host/discord_rpc.c");
    REQUIRE_FALSE(rpc.empty());
    const auto has = [&rpc](const char* code) { return rpc.find(Squashed(code)) != std::string::npos; };
    CHECK(has("GetProcAddress(m, \"hos_coop_discord_state\")"));
    CHECK(has("dlsym(m, \"hos_coop_discord_state\")"));
    // Owner's rule: a co-op build's Discord status is always Squadala!'s. Longshot v3.0.0 (as rc.8) writes its own from a
    // template (host/discord_activity.c); 0026 sends the header's frame in every state instead (title, play, pause,
    // idle), with no art (the Squadala! application's icon), and always on the Squadala! application.
    CHECK(has("if (1) { /* (patch 0026: always Squadala!'s frame, no art (the application's icon); Longshot's template below is"
              " never used) */ HosDiscordWords w = words; snprintf(w.image, sizeof w.image, \"none\");"));
    CHECK(has("if (!hos_discord_activity(activity, sizeof activity, COOP_DISCORD_NAME, \"\", \"none\", &w, module_line, started) ||"));
    CHECK(has("hos_discord_set_activity(json, cap, nonce, process_id(), activity); } return; }"));
    CHECK(has("snprintf(out, cap, \"%s\", COOP_DISCORD_APP_ID); /* (patch 0026: always the Squadala! application) */ }"));
    CHECK(has("discord_settings_line(&settings, line); read_words_line(line);"));
    CHECK(has("if (!module_state(module_line, sizeof module_line)) *module_line = 0;"));
    {   // the application's id: the kit's (tools/release/kit/scripts/discord_status.py APP_ID, its one place)
        const std::string kit = ReadFile(kRepo / "tools" / "release" / "kit" / "scripts" / "discord_status.py");
        const auto at = kit.find("\nAPP_ID = \"");
        REQUIRE(at != std::string::npos);
        const std::string id = kit.substr(at + 11, kit.find('"', at + 11) - (at + 11));
        CHECK(has(("#define COOP_DISCORD_APP_ID \"" + id + "\"").c_str()));
    }
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("COOP_HOOKSHOT_EXPORT int hos_coop_discord_state(char* out, size_t cap) noexcept") != std::string::npos);
}

// Player report 133: the title loaded the newest save by itself (no New Game), the pad unplugged. Longshot's Skip title
// screen (host/oot_autoload.c, off by default; longshot_options.ini keeps it across kit updates) tapped A every 2 s from
// 20 s after the start. Owner (2026-11-17): keep the row, no pressed buttons: patch 0028 makes its choices the game's own
// way (Button::Decide on the focused button, at a menu screen's update), stops on any press of the player's and while
// the co-op module holds it.
TEST_CASE("hookshot: Skip title screen chooses the game's own way, presses nothing (patch 0028)") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0028-coop-skip-title-native.patch");
    REQUIRE_FALSE(patch.empty());
    CHECK_FALSE(std::filesystem::exists(kRepo / "hookshot" / "patches" / "0028-coop-title-no-autopress.patch"));
    const std::string autoload = PatchedCode(patch, "host/oot_autoload.c");
    REQUIRE_FALSE(autoload.empty());
    const auto has = [&autoload](const char* code) { return autoload.find(Squashed(code)) != std::string::npos; };
    // The taps are gone, the call and its declaration both.
    CHECK(patch.find("\n-    hos_hid_tap(\"0:A/200\");\n") != std::string::npos);
    CHECK(patch.find("\n-extern void hos_hid_tap(const char *presses);") != std::string::npos);
    CHECK(autoload.find(Squashed("hos_hid_tap(")) == std::string::npos);
    // The choice: the game's Button::Decide(button, 0) on the one focused button, steady first, its answer read.
    CHECK(has("static const uint64_t kButtonDecide = 0x80020000ULL + 0x3771db0ULL;"));
    CHECK(has("const A64Fn decide = a64_lookup(kButtonDecide);"));
    CHECK(has("c->x[0] = button, c->x[1] = 0, c->x[30] = 0; decide(c); const int chosen = (int)(c->x[0] & 1); *c = saved;"));
    CHECK(has("if (now - steady_ms < kSteadyMs) return;"));
    CHECK(has("if (state && (state & 0xfe) != 4) { if (found) return 0; found = button; }"));
    // Its stops: any press of the player's, the module's hold, the row off, Link running, the time and the count.
    CHECK(has("if (__atomic_load_n(&hos_coop_pad, __ATOMIC_RELAXED) != 0) {"));
    CHECK(has("if (hos_coop_title_hold_now()) {"));
    CHECK(has("if (!rx_option(HOS_OPT_SKIP_TITLE, 0)) { stop(\"the row was turned off while waiting\");"));
    CHECK(has("if (hos_oot_player_seen()) { stop(\"loaded, the player is running\");"));
    // Only the known screens, in order (hos_skip_title.h, the same file as hookshot/src's: skip_title_test.cpp); after
    // three choices nothing more is chosen.
    CHECK(NewFile(patch, "host/hos_skip_title.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_skip_title.h"));
    CHECK(has("#include \"hos_skip_title.h\""));
    CHECK(has("if (choices >= HOS_SKIP_STEPS) return;"));
    CHECK(has("const int next = hos_skip_title_next(choices, vtable, name, &why); if (next == HOS_SKIP_WAIT) return; "
              "if (next == HOS_SKIP_STOP) {"));
    CHECK(has("const uint64_t vtable = peek64(screen); char name[64]; button_name(button, name, sizeof name);"));
    // (Armed only by the row, off by default: Longshot's own start test, unchanged, outside the patch's hunks.)
    // The screen hook hands each menu screen to it, before the screen's own update. One tick for both platforms:
    // patch 0018 builds it on Linux too (Linux's stub is gone), so 0028 adds no Linux-only tick.
    const std::string menu = PatchedCode(patch, "host/oot_menu.c");
    CHECK(menu.find(Squashed("void hos_oot_menu_tick(Ctx *c, uint64_t screen) { if (!screen) return; "
                             "hos_oot_autoload_screen(c, screen);")) != std::string::npos);
    CHECK(patch.find("+void hos_oot_menu_tick(") == std::string::npos);
    // The bridge asks the module's optional export; no module or co-op off: 0 (solo skips the title).
    const std::string bridge = PatchedCode(patch, "host/coop_bridge.c");
    CHECK(bridge.find(Squashed("coop_title_hold = (int (*)(void))(void *)GetProcAddress(m, \"hos_coop_title_hold\");")) !=
          std::string::npos);
    CHECK(bridge.find(Squashed("return __atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2 && coop_title_hold ? "
                               "coop_title_hold() : 0;")) != std::string::npos);
    // The row stays (no menu change) and the module gives the hold.
    CHECK(patch.find("gpu/vk/vk_menu.cpp") == std::string::npos);
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("COOP_HOOKSHOT_EXPORT int hos_coop_title_hold(void) noexcept {") != std::string::npos);
    // No co-op patch adds a press of its own (hos_hid_tap is the host's press; the module's pad filter only takes
    // bits away: coop_title.cpp OnPad, coop_dbno.cpp).
    for (const auto& e : std::filesystem::directory_iterator(kRepo / "hookshot" / "patches")) {
        const std::string text = ReadFile(e.path());
        std::istringstream lines(text);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line[0] == '+' && line.rfind("+++", 0) != 0) {
                INFO(e.path().filename().string() << ": " << line);
                CHECK(line.find("hos_hid_tap(") == std::string::npos);
            }
        }
    }
}

TEST_CASE("hookshot: a module may have a texture drawn with other texels (patch 0020, hos_coop_texture.h)") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0020-coop-texture-override.patch");
    REQUIRE_FALSE(patch.empty());
    CHECK(NewFile(patch, "host/hos_coop_texture.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_texture.h"));
    // The exe's side is the file the unit tests build (texture_override_test.cpp).
    CHECK(NewFile(patch, "host/coop_texture.c") == ReadFile(kRepo / "hookshot" / "src" / "coop_texture.c"));
    // The exe's exports, the textures it is told of, and both renderers reading through the overlay.
    CHECK(patch.find("+COOP_TEXTURE_EXPORT int hos_coop_textures(") != std::string::npos);
    CHECK(patch.find("+COOP_TEXTURE_EXPORT int hos_coop_texture_override(uint64_t address, uint64_t serial,") !=
          std::string::npos);
    CHECK(patch.find("+        hos_coop_texture_made(o, o->gpu, o->L.size,") != std::string::npos);
    CHECK(patch.find("+    hos_coop_texture_gone(o);") != std::string::npos);
    CHECK(patch.find("+        x.overlay = (overlay_fn)look(\"hos_coop_texture_overlay\");") != std::string::npos);
    // Longshot rc.5 has no WebGPU renderer (gpu/render keeps headers only): no patch reads through gpu/render.
    CHECK(patch.find("gpu/render/") == std::string::npos);
    CHECK_FALSE(std::filesystem::exists(kRepo / "hookshot" / "patches" / "0021-coop-texture-override-webgpu.patch"));
    // Nothing of the game is written: no lift, no guest write (only the pages marked for the renderers).
    for (std::size_t at = patch.find("diff --git a/"); at != std::string::npos; at = patch.find("diff --git a/", at + 1)) {
        CHECK((patch.compare(at, 18, "diff --git a/host/") == 0 || patch.compare(at, 17, "diff --git a/gpu/") == 0));
    }
    CHECK(patch.find("nx2/") == std::string::npos);
    // The module finds them by name and never writes the game's memory for a look.
    const std::string live = ReadFile(kRepo / "hookshot" / "src" / "coop_look_live.cpp");
    CHECK(live.find("look(\"hos_coop_texture_override\")") != std::string::npos);
    CHECK(live.find("CoopRuntime::Write") == std::string::npos);
}

TEST_CASE("hookshot: the sound wrap (patch 0014) is one row in 0005's table, its header hookshot/src's") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0014-coop-sound-wrap.patch");
    CHECK(NewFile(patch, "host/hos_coop_sound.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_sound.h"));
    char row[128];
    std::snprintf(row, sizeof row, "+    (%u, 0x440630, (0xd503237f, 0xa9bb7bfd, 0xa90167fa, 0xa9025ff8)),",
                  HOS_COOP_WRAP_SOUND_EMIT);
    CHECK(patch.find(row) != std::string::npos);
    // At the table's head, so 0013's rows after row 3 and this one apply in either order.
    CHECK(patch.find(" WRAPS = (\n+    # HOS_COOP_WRAP_SOUND_EMIT") != std::string::npos);
    // The id is free in the one wrap space (0005: 1-3, 0013: 4-5, 0010: 16-23).
    CHECK(HOS_COOP_WRAP_SOUND_EMIT > HOS_COOP_WRAP_EVENT_REQUEST);
    CHECK(HOS_COOP_WRAP_SOUND_EMIT < 64);
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("if (id == HOS_COOP_WRAP_SOUND_EMIT) {") != std::string::npos);
}

TEST_CASE("hookshot: the AI node starts (patch 0029) are one generic world event, their header hookshot/src's") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0029-coop-node-starts.patch");
    CHECK(NewFile(patch, "host/hos_coop_nodes.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_nodes.h"));
    const std::string lift = NewFile(patch, "nx2/coop_node_lift.py");
    REQUIRE(!lift.empty());
    // One event naming the node (x0) and its actor (x9, just read from [[[node + 8] + 8] + 0x1f0]).
    char call[96];
    std::snprintf(call, sizeof call, "hos_coop_world_event(%uu, c->x[0], c->x[9])", HOS_COOP_WORLD_NODE_START);
    CHECK(lift.find(call) != std::string::npos);
    CHECK(HOS_COOP_WORLD_NODE_START >= HOS_COOP_WORLD_NEXT_FIRST);
    CHECK(HOS_COOP_WORLD_NODE_START <= HOS_COOP_WORLD_NEXT_LAST);
    // The push node's start (the module's BlockNode::kStartFn), after the actor's load; words checked.
    CHECK(lift.find("(\"push node\", 0x27517c0,") != std::string::npos);
    CHECK(lift.find("(0x2751790, 0xd503237f)") != std::string::npos);
    CHECK(lift.find("(0x27517c0, 0xf940f929)") != std::string::npos);  // ldr x9, [x9, #0x1f0]
    CHECK(CoopWorld::BlockNode::kStartFn == 0x2751790);
    CHECK(patch.find("+    coop_node_lift.install(lifter, img, build_id)") != std::string::npos);
    // Generic: no block-only event or file in the hook layer, and patch 0005's files are left to it.
    CHECK(patch.find("BLOCK") == std::string::npos);
    CHECK(patch.find("diff --git a/host/hos_coop_world.h") == std::string::npos);
    CHECK(patch.find("diff --git a/host/coop_world.c") == std::string::npos);
}

// Issue 264, piece 4 (the pause menu's Multiplayer tab): patch 0030 hands the module every menu screen's update and
// asks it which page the mouse's tab strip counts; with five tabs the mouse's zones follow co-op's layout.
TEST_CASE("hookshot: the game's menu screens reach the module, the mouse knows a fifth tab (patch 0030)") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0030-coop-pause-tab.patch");
    REQUIRE_FALSE(patch.empty());
    const std::string menu = PatchedCode(patch, "host/oot_menu.c");
    REQUIRE_FALSE(menu.empty());
    const auto in_menu = [&menu](const char* code) { return menu.find(Squashed(code)) != std::string::npos; };
    // In the one tick of both platforms, after 0028's call, before the screen's own update.
    const auto tick = menu.find(Squashed("void hos_oot_menu_tick(Ctx *c, uint64_t screen) { if (!screen) return; "
                                         "hos_oot_autoload_screen(c, screen);"));
    const auto ours = menu.find(Squashed("hos_coop_game_screen_now(c, screen);"));
    CHECK(tick != std::string::npos);
    CHECK((ours != std::string::npos && ours > tick && ours - tick < 300));
    // The mouse: L and R past a fifth tab's number, five zones when the strip counts five, the module's page.
    CHECK(in_menu("enum { TAB_L = 8, TAB_R = 9 };"));
    CHECK(in_menu("return strip && peek(strip + 0x328, &n, sizeof n) && n == 5 ? 5 : 4;"));
    // (the fifth zone is a tab to step to only while the module has its page: -1 or 4; -2 without one)
    CHECK(in_menu("return x < -462 ? 0 : x < 6 ? 1 : x < 474 ? 2 : x < 916 ? 3 : hos_coop_game_pause_page_now() >= -1 "
                  "? 4 : -1;"));
    CHECK(in_menu("const int coop = hos_coop_game_pause_page_now(); return coop >= 0 ? coop : page_now;"));
    CHECK(in_menu("const int cur = pause ? pause_page() : page;"));
    // The bridge asks the module's optional exports; without them, or with co-op off, nothing changes.
    const std::string bridge = PatchedCode(patch, "host/coop_bridge.c");
    CHECK(bridge.find(Squashed("coop_game_screen = (void (*)(void *, uint64_t))(void *)GetProcAddress(m, "
                               "\"hos_coop_game_screen\");")) != std::string::npos);
    CHECK(bridge.find(Squashed("if (__atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2 && coop_game_screen) "
                               "coop_game_screen(c, screen);")) != std::string::npos);
    CHECK(bridge.find(Squashed("return __atomic_load_n(&state, __ATOMIC_ACQUIRE) == 2 && coop_pause_page ? "
                               "coop_pause_page() : -2;")) != std::string::npos);
    // Longshot's own call: no lift hook, no wrapped function, the API header as it was.
    CHECK(patch.find("diff --git a/nx2/") == std::string::npos);
    CHECK(patch.find("diff --git a/host/hos_coop_api.h") == std::string::npos);
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("COOP_HOOKSHOT_EXPORT void hos_coop_game_screen(void* ctx, uint64_t screen) noexcept {") !=
          std::string::npos);
    CHECK(module.find("COOP_HOOKSHOT_EXPORT int hos_coop_game_pause_page(void) noexcept {") != std::string::npos);
    CHECK(module.find("return g_on ? CoopPauseTab::MousePage() : -2;") != std::string::npos);   // (off: no page)
}

TEST_CASE("hookshot: the save-load wraps (patch 0024) are four rows in 0005's table, their header hookshot/src's") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0024-coop-save-load-wrap.patch");
    CHECK(NewFile(patch, "host/hos_coop_save_load.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_save_load.h"));
    const struct {
        unsigned id;
        const char* rest;
    } rows[] = {
        {HOS_COOP_WRAP_SAVE_LOADED, "0x32d8690, (0xd503237f, 0xa9be7bfd, 0xf9000bf3, 0x910003fd)),"},
        {HOS_COOP_WRAP_SAVE_LOAD_START, "0x32c1db0, (0xd503237f, 0xa9bd7bfd, 0xf9000bf5, 0xa9024ff4)),"},
        {HOS_COOP_WRAP_TEMP_LOAD_START, "0x32c2080, (0xd503237f, 0xa9bf7bfd, 0x910003fd, 0xb000dea8)),"},
        {HOS_COOP_WRAP_GAME_OVER_SAVE, "0x32bbce0, (0xd503237f, 0xa9be7bfd, 0xf9000bf3, 0x910003fd)),"},
    };
    for (const auto& r : rows) {
        char row[128];
        std::snprintf(row, sizeof row, "+    (%u, %s", r.id, r.rest);
        CHECK_MESSAGE(patch.find(row) != std::string::npos, row);
        // Free in the one wrap space (0005: 1-3, 0013: 4-5, 0010: 16-23, 0014: 24).
        CHECK(r.id > HOS_COOP_WRAP_ACTION_VERB);
        CHECK(r.id < 16);
    }
    // After 0013's last row, at the table's end.
    CHECK(patch.find("     (5, 0xc08f0,") != std::string::npos);
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("CoopSave::NoteGameSaveLoad(id != HOS_COOP_WRAP_SAVE_LOADED)") != std::string::npos);
    CHECK(module.find("CoopSave::NoteGameOverSave()") != std::string::npos);
    CHECK(module.find("(level.wraps >> HOS_COOP_WRAP_GAME_OVER_SAVE & 1) != 0") != std::string::npos);
    CHECK(module.find("CoopSave::SetNativeSaveLoads(loads)") != std::string::npos);
}

TEST_CASE("hookshot: on Linux the exe exports every function the module finds in it by name") {
    // The module finds these with dlsym(RTLD_DEFAULT, ...), which sees only an exe's dynamic symbols. Hookshot's Linux
    // link exports hos_posix_* alone, so patch 0001 names each of ours there; without it the module found none of them
    // on Linux (no host save for a guest, no pad choice, no local pads), while Windows finds them as dllexport.
    std::vector<std::string> names;
    const auto collect = [&names](const std::string& text, const std::string& before) {
        for (auto at = text.find(before); at != std::string::npos; at = text.find(before, at + 1)) {
            const auto start = at + before.size();
            names.push_back(text.substr(start, text.find('"', start) - start));
        }
    };
    collect(ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp"), "dlsym(RTLD_DEFAULT, \"");
    const std::string pads = ReadFile(kRepo / "hookshot" / "src" / "hos_coop_pads.h");
    for (const char* d : {"#define HOS_COOP_PAD_INFO \"", "#define HOS_COOP_PAD_READ \"", "#define HOS_COOP_PAD_TAKE \""}) {
        collect(pads, d);
    }
    // Patch 0020's texture exports: the module's live look finds two, the exe's own renderers the other two.
    collect(ReadFile(kRepo / "hookshot" / "src" / "coop_look_live.cpp"), "look(\"");
    collect(ReadFile(kRepo / "hookshot" / "patches" / "0020-coop-texture-override.patch"), "look(\"");
    // Patch 0018's exports (the menu state, the HUD refresh): every function it marks dllexport, which Linux builds too
    const std::string map_state = ReadFile(kRepo / "hookshot" / "patches" / "0018-coop-map-state.patch");
    const std::string exported = "\n+__declspec(dllexport) int ";
    std::size_t map_exports = 0;
    for (auto at = map_state.find(exported); at != std::string::npos; at = map_state.find(exported, at + 1)) {
        const auto start = at + exported.size();
        names.push_back(map_state.substr(start, map_state.find('(', start) - start));
        ++map_exports;
    }
    CHECK(map_exports == 2);
    CHECK(names.size() >= 13);
    const std::string patch = Patch();
    const auto start = patch.find("+++ b/nx2/build.py\n");
    REQUIRE(start != std::string::npos);
    const std::string build = patch.substr(start, patch.find("\ndiff --git", start) - start);
    // (the exe's link: Longshot's run_link since rc.5, subprocess.run before; the line's indent follows the call)
    const auto link = build.find("r = run_link([");
    REQUIRE(link != std::string::npos);
    REQUIRE(build.find("[\"-Wl,--gc-sections\", \"-Wl,--export-dynamic-symbol=hos_posix_*\"] + coop", link) !=
            std::string::npos);
    for (const auto& name : names) {
        CAPTURE(name);
        CHECK(build.find("\"" + name + "\"") != std::string::npos);
    }
}

TEST_CASE("hookshot: typing in a co-op field keeps the keyboard from the menu (patch 0022), Insert and F1 stay the menu's") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0022-coop-menu-typing-keys.patch");
    REQUIRE_FALSE(patch.empty());
    // the menu asks the module (by name, with its other exports) and drops its own key presses while it types:
    // Linux reads the keys' state, so Q and E changed the tab there (player report, Linux and Steam Deck)
    CHECK(patch.find("+    keys_taken_fn.store((int (*)(void))look(\"hos_coop_keys_taken\"), std::memory_order_release);") !=
          std::string::npos);
    CHECK(patch.find("+    const bool coop_typing = coop::keys_taken();") != std::string::npos);
    CHECK(patch.find("+            if (keys[i] != VK_INSERT && keys[i] != VK_F1) press[i] = false;") != std::string::npos);
    CHECK(patch.find("+            const bool down = (held[i] && !coop_typing) ||") != std::string::npos);   // no arrow repeat
    CHECK(patch.find("+    if (coop_typing) tune_cancelled = true;") != std::string::npos);   // Shift: no compact menu
    // the module says so while a field of its tab is typed into
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    CHECK(module.find("COOP_HOOKSHOT_EXPORT int hos_coop_keys_taken(void) {") != std::string::npos);
    // menu files only: no lift, no host change
    for (std::size_t at = patch.find("diff --git a/"); at != std::string::npos; at = patch.find("diff --git a/", at + 1)) {
        CHECK(patch.compare(at, 20, "diff --git a/gpu/vk/") == 0);
    }
}

TEST_CASE("hookshot: a guest that joins with a file loaded asks for the host's save and touches no folder") {
    // Player reports 107 and 120: one path with Resync from host (AskAtTitle), and nothing of the folder changes
    // under the game being played (no re-mount, no slots taken out): the title's PendingFrame does both.
    const std::string save = ReadFile(kRepo / "lichii" / "hook" / "coop_save.cpp");
    const auto body = [&](const char* head) {
        const auto at = save.find(head);
        REQUIRE(at != std::string::npos);
        const auto end = save.find("\n}\n", at);
        REQUIRE(end != std::string::npos);
        return save.substr(at, end - at);
    };
    const std::string in_game = body("void DecideInGame(coop::net::Client& client) {");
    CHECK(in_game.find("AskAtTitle();") != std::string::npos);
    CHECK(in_game.find("Remount") == std::string::npos);
    CHECK(in_game.find("clear_coop_slots") == std::string::npos);
    CHECK(in_game.find("TestOwnSave()") != std::string::npos);   // (the autotests' games keep their scenario's save)
    CHECK(in_game.find("save::GuestNotices::kJoinedPlaying") != std::string::npos);
    const std::string ask = body("void AskAtTitle() {");
    CHECK(ask.find("Remount") == std::string::npos);
    CHECK(ask.find("g_pending = save::coop_dir_of(own);") != std::string::npos);
    CHECK(ask.find("save::GuestNotices{g_in_game}") != std::string::npos);
    CHECK(body("void Resync(bool connected_guest) {").find("AskAtTitle();") != std::string::npos);
    // Both choices reach it: the first one of a run, and a new session after one that ended during play.
    const std::string frame = body("void SwitchFrame(coop::net::Client& client) {");
    CHECK(frame.find("choice == save::FolderChoice::coop_later") != std::string::npos);
    CHECK(frame.find("save::rejoined_in_game(g_decided, g_ended, connected, g_in_game)") != std::string::npos);
    // On the title before the new save is in, a game still on an earlier host's co-op folder goes back to its own
    // first, through the one switch path (never the older save offered).
    const std::string pending = body("void PendingFrame() {");
    const auto first = pending.find("save::old_coop_first(");
    REQUIRE(first != std::string::npos);
    CHECK(pending.find("RemountLocked(own)", first) < pending.find("save::clear_coop_slots(g_pending)", first));
    CHECK(pending.find("g_remount(") == std::string::npos);
}

TEST_CASE("hookshot: a guest's ask makes the host's game save first, with the call its own event node makes") {
    // Case 1 of the join's state (player reports 107, 129, 132): the host's file on disk is as old as its last door.
    const std::string module = ReadFile(kRepo / "hookshot" / "src" / "coop_hookshot.cpp");
    const auto body = [&](const char* head) {
        const auto at = module.find(head);
        REQUIRE(at != std::string::npos);
        const auto end = module.find("\n}\n", at);
        REQUIRE(end != std::string::npos);
        return module.substr(at, end - at);
    };
    // main offsets read from the binary: EventTriggerRequestAutoSave's body (main+0x31f2f50) calls
    // main+0xba7880(system, 0xc) and then main+0xba7730(mgr, 5, 0); the system from the GOT at main+0x4c7e178.
    CHECK(module.find("constexpr std::uint64_t kGameSystemGot = 0x4c7e178;") != std::string::npos);
    CHECK(module.find("constexpr std::uint64_t kSystemComponent = 0xba7880;") != std::string::npos);
    CHECK(module.find("constexpr std::uint64_t kAutoSaveMgrIndex = 0xc;") != std::string::npos);
    CHECK(module.find("constexpr std::uint64_t kRequestAutoSave = 0xba7730;") != std::string::npos);
    CHECK(module.find("constexpr std::uint64_t kAutoSaveKindEvent = 5;") != std::string::npos);
    const std::string ask = body("void RequestHostAutoSave(void* ctx) {");
    CHECK(ask.find("if (!CoopSave::TakeSaveRequest()) return;") != std::string::npos);   // only a guest's ask
    CHECK(ask.find("base + kSystemComponent") != std::string::npos);
    CHECK(ask.find("base + kRequestAutoSave") != std::string::npos);
    CHECK(ask.find("g_api->write") == std::string::npos);   // the game's own call; nothing written by us
    CHECK(ask.find("Write(") == std::string::npos);
    // made on the calc thread's call site, after the module's own calls; the save module told at start
    const auto calls = module.find("        RunCalls(ctx);\n        RequestHostAutoSave(ctx);");
    CHECK(calls != std::string::npos);
    CHECK(module.find("CoopSave::SetSaveRequests(true);") != std::string::npos);
    // The host side asks only while it plays a file (not on the title, not in a new game never saved)
    const std::string save = ReadFile(kRepo / "lichii" / "hook" / "coop_save.cpp");
    CHECK(save.find("const bool fresh = g_save_requests && g_in_game && "
                    "!g_played.new_game(g_in_game, g_title_seen);") != std::string::npos);
    CHECK(save.find("return g_in_game && g_planner.take_save_request();") != std::string::npos);
    // (the calc thread's every-frame look takes no lock: an atomic set when an ask opens a round)
    const auto take = save.find("bool TakeSaveRequest() {");
    REQUIRE(take != std::string::npos);
    CHECK(save.find("if (!g_save_request_due.exchange(false, std::memory_order_acq_rel)) return false;", take) <
          save.find("std::lock_guard lock(g_mutex);", take));
    CHECK(save.find("if (fresh && !open) g_save_request_due.store(true, std::memory_order_release);") !=
          std::string::npos);
    CHECK(save.find(": send.fresh ? save::newest_slot(g_dir)") != std::string::npos);
}

TEST_CASE("hookshot: the title's and the Map page's main addresses are offsets, made guest ones only by MainBase()") {
    // The save list's addresses were once written as 0x80000000 + the offset (0x20000 short of main's base). Now
    // every main address in coop_title.cpp is a main offset (docs/hookshot.md names the save list's) used through
    // Main() = CoopRuntime::MainBase() + offset, and no guest address is written in the file.
    const std::string title = ReadFile(kRepo / "hookshot" / "src" / "coop_title.cpp");
    const std::string docs = ReadFile(kRepo / "docs" / "hookshot.md");
    CHECK(title.find("return CoopRuntime::MainBase() + offset;") != std::string::npos);
    const struct {
        const char* name;
        std::uint64_t main_offset;
        bool in_docs;
    } want[] = {{"kCaptionMgrGot", 0x4c81660, true}, {"kCaptionBusy", 0x33ad380, true},
                {"kCaptionReset", 0xbafd40, true},    {"kCaptionReload", 0xf6da40, true},
                {"kTitleVtable", 0x4bf06e8, false},   {"kMsgRefInit", 0xa66f70, false},
                {"kMsgRefSet", 0xa6afe0, false},      {"kPaneText", 0xa67d30, false},
                {"kAnimFind", 0x43d870, true},        {"kAnimLastFrame", 0x2d1340, true},
                {"kAnimStopAt", 0x2d2d90, true},      {"kTitleSoundType", 0x3407000, true}};
    for (const auto& w : want) {
        INFO(w.name);
        char doc_text[32];
        std::snprintf(doc_text, sizeof doc_text, "main+0x%llx", static_cast<unsigned long long>(w.main_offset));
        char code_text[96];
        std::snprintf(code_text, sizeof code_text, "constexpr std::uint64_t %s = 0x%llx;", w.name,
                      static_cast<unsigned long long>(w.main_offset));
        char use_text[64];
        std::snprintf(use_text, sizeof use_text, "Main(%s)", w.name);
        if (w.in_docs) CHECK(docs.find(doc_text) != std::string::npos);
        CHECK(title.find(code_text) != std::string::npos);
        CHECK(title.find(use_text) != std::string::npos);
    }
    // no guest address (0x8xxxxxxx) written as a constant in the file
    std::size_t at = 0;
    while ((at = title.find("constexpr std::uint64_t ", at)) != std::string::npos) {
        const auto line_end = title.find('\n', at);
        const std::string line = title.substr(at, line_end - at);
        INFO(line);
        const auto value = line.find("= 0x");
        at = line_end;
        if (value == std::string::npos) continue;   // (button bits: 1ull << n)
        std::size_t digits = 0;
        while (value + 4 + digits < line.size() &&
               std::isxdigit(static_cast<unsigned char>(line[value + 4 + digits]))) {
            ++digits;
        }
        CHECK(digits < 8);   // a main offset (main is under 0x5000000); a guest address has 8 digits
    }
    const std::string page_h = ReadFile(kRepo / "hookshot" / "src" / "coop_map_page.h");
    const std::string page = ReadFile(kRepo / "hookshot" / "src" / "coop_map_page.cpp");
    CHECK(page_h.find("inline constexpr std::uint64_t kFindPane = 0x76e2b0;") != std::string::npos);
    CHECK(page.find("k.function = CoopRuntime::MainBase() + kFindPane;") != std::string::npos);
}

TEST_CASE("hookshot: patch 0018's screens' tracker and menu state build on Linux too, the mouse stays Windows'") {
    // Linux's oot_menu.c was a stub: no tracker, so hos_coop_menu_state said nothing there (no in_menu/paused for
    // Linux players, issue 150). The file's one '#if defined(_WIN32)' is gone; each mouse block has its own guard.
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0018-coop-map-state.patch");
    REQUIRE_FALSE(patch.empty());
    CHECK(patch.find("\n-#if defined(_WIN32)\n+/* (oot-remake-multiplayer's patch 0018) The screens' tracker") != std::string::npos);
    CHECK(patch.find("\n-void hos_oot_menu_tick(Ctx *c, uint64_t screen) { (void)c, (void)screen; }\n") != std::string::npos);
    // the guards open and close in pairs (the file's last '#else' stays Linux's focus hook, which Longshot's
    // tests/test_hold_toggle.py compiles alone)
    std::size_t opened = 0, closed = 0;
    for (auto at = patch.find("\n+#if defined(_WIN32)   /* (patch 0018: the mouse's"); at != std::string::npos;
         at = patch.find("\n+#if defined(_WIN32)   /* (patch 0018: the mouse's", at + 1)) {
        ++opened;
    }
    for (auto at = patch.find("\n+#endif   /* (patch 0018) */"); at != std::string::npos;
         at = patch.find("\n+#endif   /* (patch 0018) */", at + 1)) {
        ++closed;
    }
    CHECK(opened == 6);
    CHECK(closed == opened);
    // Linux's focus hook hands the tracker the pause menu's screen (its Map page is not the pause menu's frame)
    CHECK(patch.find("+    hos_oot_menu_pause_focus(screen);") != std::string::npos);
    CHECK(patch.find("+void hos_oot_menu_pause_focus(uint64_t screen) { InterlockedExchange64(&pause_screen, (LONG64)screen); }") !=
          std::string::npos);
    // no hunk on Longshot's tests/: the Linux kit patches the AppImage's source, which has none (CI linux-longshot,
    // 'apply_patch: tests/test_hold_toggle.py: not found'); the test below holds every patch to that
    CHECK(patch.find("diff --git a/tests/") == std::string::npos);
}

TEST_CASE("hookshot: every patch touches only what Longshot's Linux AppImage ships, so the Linux kit applies it") {
    // tools/release/kit/linux-longshot/scripts/build.sh applies every hookshot/patches/*.patch to a copy of the
    // AppImage's usr/share/longshot, which holds only what Longshot's scripts/linux/appimage.sh copies (v3.0.0):
    // no tests/, docs/, .forgejo/, most of third_party/. A hunk on anything else applies on Windows' full source and
    // stops every Linux build. Keep this list to appimage.sh's when the pin moves: the list is for kShippedFor, and
    // the test fails on any other tag in hookshot/LINUX.txt until the list is re-checked and kShippedFor moved.
    const std::string kShippedFor = "v3.0.0";
    const std::string linux_pin = ReadFile(kRepo / "hookshot" / "LINUX.txt");
    const auto tag_at = linux_pin.find("\ntag=");
    REQUIRE(tag_at != std::string::npos);
    const auto tag_start = tag_at + 5;
    const std::string tag = linux_pin.substr(tag_start, linux_pin.find('\n', tag_start) - tag_start);
    {
        INFO("hookshot/LINUX.txt moved to " << tag << ": re-check this list against that tag's scripts/linux/appimage.sh, "
             "then set kShippedFor to it");
        REQUIRE(tag == kShippedFor);
    }
    const char* const shipped[] = {
        "nx2/", "host/", "scripts/", "assets/", "builder/linux/", "gpu/vk/", "gpu/compat/", "gpu/third_party/",
        "third_party/opus/", "third_party/kenney_input/", "third_party/kenney_input_keyboard/", "third_party/imgui/",
        "third_party/zstd/", "third_party/fonts/", "third_party/fidelityfx/", "third_party/a64-kit/",
    };
    const auto ships = [&](const std::string& path) {
        for (const char* dir : shipped) {
            if (path.rfind(dir, 0) == 0) return true;
        }
        // gpu/render: only its headers, top level (cp -a gpu/render/*.h); third_party/zbic: its listed files
        const std::string render = "gpu/render/";
        if (path.rfind(render, 0) == 0 && path.find('/', render.size()) == std::string::npos &&
            path.size() > 2 && path.compare(path.size() - 2, 2, ".h") == 0) {
            return true;
        }
        for (const char* file : {"third_party/zbic/zstd.c", "third_party/zbic/zstd.h", "third_party/zbic/zstd_errors.h",
                                 "LICENSE", "README.md", "tools.lock"}) {
            if (path == file) return true;
        }
        return false;
    };
    std::size_t patches = 0, files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(kRepo / "hookshot" / "patches")) {
        if (entry.path().extension() != ".patch") continue;
        ++patches;
        const std::string patch = ReadFile(entry.path());
        const std::string head = "diff --git a/";
        for (auto at = patch.find(head); at != std::string::npos; at = patch.find(head, at + 1)) {
            if (at != 0 && patch[at - 1] != '\n') continue;
            const auto start = at + head.size();
            const std::string path = patch.substr(start, patch.find(' ', start) - start);
            ++files;
            INFO(entry.path().filename().string() << ": " << path);
            CHECK(ships(path));
        }
    }
    CHECK(patches >= 24);
    CHECK(files >= patches);
}

TEST_CASE("hookshot: our patches call game functions as main + offset, never a bare 0x8 address (patch 0018's HUD refresh)") {
    // Patch 0018 called the HUD refresh at 0x833e4200: main's offset 0x33e4200 put on 0x80000000 (rtld's base), not on
    // main's 0x80020000. That is the middle of f_833e4190, so a64_lookup found nothing, guest_call gave 0 and the
    // refresh (rows 63/76) never ran while hud_refreshes still counted. No line a patch adds calls guest_call or
    // a64_lookup with a bare 0x8... literal: a function is named by a constant written as main + offset.
    const std::string bridge = Patch();
    REQUIRE(bridge.find("+    api.main_base = 0x80020000ULL;") != std::string::npos);   // (the main the offsets are on)
    std::size_t patches = 0;
    for (const auto& entry : std::filesystem::directory_iterator(kRepo / "hookshot" / "patches")) {
        if (entry.path().extension() != ".patch") continue;
        ++patches;
        std::istringstream lines(ReadFile(entry.path()));
        for (std::string line; std::getline(lines, line);) {
            if (line.rfind("+", 0) != 0 || line.rfind("+++", 0) == 0) continue;
            for (const char* call : {"guest_call(", "a64_lookup("}) {
                const auto at = line.find(call);
                if (at == std::string::npos) continue;
                INFO(entry.path().filename().string() << ": " << line);
                CHECK(line.find("0x8", at) == std::string::npos);
            }
        }
    }
    CHECK(patches > 20);
    // The HUD refresh: main + 0x33e4200 (f_83404200), looked up first; counted only when it ran, else one log line.
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0018-coop-map-state.patch");
    CHECK(patch.find("+#define HUD_REFRESH_FN (0x80020000ULL + 0x33e4200ULL)\n") != std::string::npos);
    CHECK(patch.find("833e4200ULL") == std::string::npos);
    CHECK(patch.find("+        if (a64_lookup(HUD_REFRESH_FN)) {\n"
                     "+            guest_call(c, HUD_REFRESH_FN, screen, 0, NULL, NULL);\n"
                     "+            InterlockedIncrement(&hud_refreshes);\n"
                     "+        } else if (!InterlockedExchange(&hud_refresh_gone, 1)) {\n") != std::string::npos);
    // patch 0028's Button::Decide is written the same way
    const std::string skip = ReadFile(kRepo / "hookshot" / "patches" / "0028-coop-skip-title-native.patch");
    CHECK(skip.find("+static const uint64_t kButtonDecide = 0x80020000ULL + 0x3771db0ULL;") != std::string::npos);
}

TEST_CASE("hookshot: co-op's log lines stay out of Longshot's crash report (patch 0027), its header hookshot/src's") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0027-coop-crash-report.patch");
    CHECK(NewFile(patch, "host/hos_coop_crash.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_crash.h"));
    // Both readers of the run log skip such a line: read_log (the saved and copied report) and crash_lines (what Send posts).
    CHECK(patch.find("+        if (hos_coop_crash_private(line)) continue;   /* (patch 0027) */\n         char *clean = redacted(") !=
          std::string::npos);
    CHECK(patch.find("+        if (hos_coop_crash_private(line)) continue;   /* (patch 0027) */\n         if (!found && hos_crash_starts(") !=
          std::string::npos);
    // As hos_log writes them (host/hos_thread.c: "[hos <thread>] [<level>] "), the module's level word after.
    CHECK(hos_coop_crash_private("[hos main] coop: on, server 192.0.2.7:27015, name 'Saria', join password none\n"));
    CHECK(hos_coop_crash_private("[hos net] coop: player 2 'Mido' joined"));
    CHECK(hos_coop_crash_private("[hos main] [warn] warning: coop: voice: microphone Headset is not connected"));
    CHECK(hos_coop_crash_private("[hos main] error: coop: server refused"));
    CHECK(hos_coop_crash_private("[hos audio] coop voice: on (the voices in the game's final mix)"));
    CHECK(hos_coop_crash_private("coop: module loaded"));
    // Longshot's own lines stay (the crash itself, the renderer, the maps).
    CHECK_FALSE(hos_coop_crash_private("[hos main] CRASH: exception 0xc0000005 (read 0x8) at longshot.exe+0x1a2b3c"));
    CHECK_FALSE(hos_coop_crash_private("[hos main] vk: status: 3.1 of 15.9 GB"));
    CHECK_FALSE(hos_coop_crash_private("[hos main] hid: controller \"Pad\" is pad 1"));
    CHECK_FALSE(hos_coop_crash_private("[hos main] map: coop:"));
    CHECK_FALSE(hos_coop_crash_private("[hos main"));
    CHECK_FALSE(hos_coop_crash_private(""));
}

TEST_CASE("hookshot: a co-op build sends nothing to Longshot and is never a Longshot release (patch 0027)") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0027-coop-crash-report.patch");
    // No address is read, so neither Report automatically nor longshot_crash.ini's silent send is offered.
    CHECK(patch.find("+    s->reports[0] = s->issue[0] = 0;\n }") != std::string::npos);
    CHECK(patch.find("-    char *stamp = join(path, sizeof path, s->dir, \"longshot_version.txt\")") != std::string::npos);
    // The release flag is always 0.
    CHECK(patch.find("-    in.release = hos_crash_release(in.bare_version, source, stamp_source);\n+    in.release = 0;") !=
          std::string::npos);
    // No performance summary is measured or written.
    CHECK(patch.find("bool perf_report_init() {\n     folder = game_dir_path();\n+") != std::string::npos);
    CHECK(patch.find("+    if (true) return false;") != std::string::npos);
    // Every address empty, so the build's stamp has none and a sender has nowhere to send.
    for (const char* key : {"crash_reports", "crash_issue", "perf_reports"}) {
        CHECK(patch.find(std::string("\n+") + key + " =\n") != std::string::npos);
        CHECK(patch.find(std::string("\n-") + key + " = https://") != std::string::npos);
    }
    // Report manually opens co-op's page, not Longshot's issue form.
    CHECK(patch.find("+#define HOS_CRASH_ISSUES HOS_COOP_CRASH_REPORT_PAGE") != std::string::npos);
    CHECK(patch.find("+    snprintf(out->url, sizeof out->url, \"%s\", HOS_COOP_CRASH_REPORT_PAGE);") != std::string::npos);
    CHECK(std::string(HOS_COOP_CRASH_REPORT_PAGE).rfind("https://github.com/daurosk/oot-remake-multiplayer/", 0) == 0);
    CHECK(patch.find("+                           \"your browser (this is a co-op build: not Longshot's to fix). %s\",") !=
          std::string::npos);
}

TEST_CASE("hookshot: no patch logs a microphone, pad, player or server name outside co-op's own lines") {
    // A line our patches add to Longshot's log reaches its crash report unless it is co-op's ("coop..."; patch 0027).
    // So a name a player chose or a device carries goes only into co-op's own lines (the device's own connect line is
    // Longshot's). 0009: the co-op voice's source logs a fixed word; 0015: the pad's number without its name again.
    static const char* const kNames[] = {"voice_name", "named ? name", "hos_sdl_pad_name", "nickname", "server", "src->want",
                                         "sel_name", "dev_names"};
    for (const auto& entry : std::filesystem::directory_iterator(kRepo / "hookshot" / "patches")) {
        if (entry.path().extension() != ".patch") continue;
        // (the whole call, from hos_log( to its ");", over every added line its arguments wrap to)
        std::istringstream lines(ReadFile(entry.path()));
        std::string call;
        for (std::string line; std::getline(lines, line);) {
            if (line.rfind("+", 0) != 0) {
                call.clear();
                continue;
            }
            if (call.empty()) {
                const auto at = line.find("hos_log(");
                if (at == std::string::npos) continue;
                call = line.substr(at);
            } else {
                call += " " + line.substr(1);
            }
            if (call.find(");") == std::string::npos) continue;
            const bool coop = call.rfind("hos_log(\"coop", 0) == 0;
            for (const char* name : kNames) {
                INFO(entry.path().filename().string() << ": " << call);
                CHECK((coop || call.find(name) == std::string::npos));
            }
            call.clear();
        }
    }
    const std::string voice = ReadFile(kRepo / "hookshot" / "patches" / "0009-coop-voice.patch");
    CHECK(voice.find("+    return src->own ? \"the co-op voice's microphone\" : src->mode == 2 ? src->want : \"the default microphone\";") !=
          std::string::npos);
    CHECK(voice.find("+    hos_log(\"coop voice: microphone: %s\"") != std::string::npos);
    // rc.6's fifth source (the microphone test's slot) is free for the co-op voice too.
    CHECK(voice.find("+    while (named && i < MAX_STREAMS + 1 && sources[i].used) i++;") != std::string::npos);
    const std::string pads = ReadFile(kRepo / "hookshot" / "patches" / "0015-coop-pad-select.patch");
    CHECK(pads.find("+                    hos_log(\"hid: that controller is pad %d\", n);") != std::string::npos);
    // A game with no keyboard or mouse leaves the camera to its pad, behind other windows too (Nucleus Co-op's Ctrl+H
    // puts every game behind): the mouse camera's last angles are never put back over the right stick.
    CHECK(pads.find("+        if (hos_input_keyboard_mode() == 2) hos_mouse_native_stick(1);") != std::string::npos);
    CHECK(pads.find("+        hos_mouse_native_stick(stick[2] != 0 || stick[3] != 0);") == std::string::npos);
}

TEST_CASE("hookshot: the Insert menu files nothing with Longshot in a co-op build (patch 0027)") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0027-coop-crash-report.patch");
    // Bug report: co-op's page, not Longshot's issue chooser or its game-problem form; co-op's lines out of the copied logs.
    CHECK(patch.find("-constexpr const char* kIssues = \"https://git.magicbean.dev/") != std::string::npos);
    CHECK(patch.find("-constexpr const char* kForm = \"https://git.magicbean.dev/") != std::string::npos);
    CHECK(patch.find("+constexpr const char* kIssues = HOS_COOP_CRASH_REPORT_PAGE;") != std::string::npos);
    CHECK(patch.find("+    link = kIssues, link_short = false;") != std::string::npos);
    CHECK(patch.find("+        if (hos_coop_crash_private(line.c_str())) {") != std::string::npos);
    CHECK(patch.find("+    return \"Saved in the game folder as \" + folder + \". To send it, open co-op's Reporting a problem page") !=
          std::string::npos);
    // Save my pipeline list (Longshot issue 14): never listed on the Debug tab, and its press does nothing.
    CHECK(patch.find("&& !is_share_row(r))") != std::string::npos);
    CHECK(patch.find("-        else if (press[7] && is_share_row(r)) share_pipeline_list(), press[7] = false, changed = true;\n"
                     "+        else if (press[7] && is_share_row(r)) press[7] = false;") != std::string::npos);
    // No patch adds a Longshot issue, form or telemetry address anywhere.
    for (const auto& entry : std::filesystem::directory_iterator(kRepo / "hookshot" / "patches")) {
        if (entry.path().extension() != ".patch") continue;
        std::istringstream lines(ReadFile(entry.path()));
        for (std::string line; std::getline(lines, line);) {
            if (line.rfind("+", 0) != 0 || line.rfind("+++", 0) == 0) continue;
            INFO(entry.path().filename().string() << ": " << line);
            CHECK(line.find("git.magicbean.dev") == std::string::npos);
            CHECK(line.find("data-services.magicbean.dev") == std::string::npos);
        }
    }
}

TEST_CASE("hookshot: every switch of the save folder marks the game's save list for a re-read (player report 124)") {
    // One path: the only call of the exe's remount is in CoopSave's RemountLocked, which marks the list stale; the
    // title's step has the game re-read it (its own SeqOneshotRequestLoadAndSerializeCaption body).
    const std::string save = ReadFile(kRepo / "lichii" / "hook" / "coop_save.cpp");
    const auto first = save.find("g_remount(");
    REQUIRE(first != std::string::npos);
    CHECK(save.find("g_remount(", first + 1) == std::string::npos);
    const auto helper = save.find("bool RemountLocked(const fs::path& dir) {");
    REQUIRE(helper != std::string::npos);
    CHECK(first > helper);
    CHECK(save.find("g_list_stale = true;", helper) != std::string::npos);
    // (Navi's Saves row, ChooseSaveSet) the solo/multiplayer switch goes through it too: its new list shows at once.
    const auto apply = save.find("std::string ApplySaveSetLocked() {");
    REQUIRE(apply != std::string::npos);
    CHECK(save.find("if (!RemountLocked(want)) {", apply) != std::string::npos);
    const std::string title = ReadFile(kRepo / "hookshot" / "src" / "coop_title.cpp");
    CHECK(title.find("constexpr std::uint64_t kCaptionReload = 0xf6da40;") != std::string::npos);
    CHECK(title.find("constexpr std::uint64_t kCaptionBusy = 0x33ad380;") != std::string::npos);
    CHECK(title.find("constexpr std::uint64_t kCaptionReset = 0xbafd40;") != std::string::npos);
    // The game's order: its idle test, its reset, then its read (a read starts only from state 0).
    const auto busy = title.find("GameCall(ctx, Main(kCaptionBusy), mgr)");
    const auto reset = title.find("GameCall(ctx, Main(kCaptionReset), 0)");
    const auto read = title.find("GameCall(ctx, Main(kCaptionReload), mgr)");
    REQUIRE(busy != std::string::npos);
    REQUIRE(reset != std::string::npos);
    REQUIRE(read != std::string::npos);
    CHECK(busy < reset);
    CHECK(reset < read);
    CHECK(title.find("constexpr std::uint64_t kCaptionMgrGot = 0x4c81660;") != std::string::npos);
    CHECK(title.find("in.stale = CoopSave::TakeSaveListStale();") != std::string::npos);
    const auto step = title.find("if (event == HOS_COOP_TITLE_STEP) {");
    REQUIRE(step != std::string::npos);
    CHECK(title.find("RefreshSaveList(ctx);", step) != std::string::npos);
    // The open title is arranged again only once the game has read the list again (the done case, so only after a
    // switch of the save folder: solo with co-op idle never switches), and a failure changes nothing.
    const auto done = title.find("case R::Action::done:");
    const auto gave_up = title.find("case R::Action::gave_up:");
    const auto arrange = title.find("ArrangeTitle(ctx, mgr)");
    REQUIRE(done != std::string::npos);
    REQUIRE(gave_up != std::string::npos);
    REQUIRE(arrange != std::string::npos);
    CHECK(done < arrange);
    CHECK(arrange < gave_up);
    CHECK(title.find("ArrangeTitle(ctx, mgr)", arrange + 1) == std::string::npos);   // called nowhere else
    // The game's own request path's layout, checked against the title's before any call; the frame is stopped only
    // if BtnNum reaches it, as the game's own request does.
    const auto guard = title.find("Read64(screen + kTitleAnimLayout); layout == 0 || used != layout");
    const auto find = title.find("k.function = Main(kAnimFind);");
    const auto last = title.find("GameCall(ctx, Main(kAnimLastFrame), player)");
    const auto stop = title.find("k.function = Main(kAnimStopAt);");
    REQUIRE(guard != std::string::npos);
    REQUIRE(find != std::string::npos);
    REQUIRE(last != std::string::npos);
    REQUIRE(stop != std::string::npos);
    CHECK(guard < find);
    CHECK(find < last);
    CHECK(last < stop);
    CHECK(title.find("constexpr std::uint64_t kTitleAnimLayout = 0xe0;") != std::string::npos);
}

// Owner photo 2026-11-19 (patch 0031): the module tab's column is opaque (the game's own pause page showed through
// under a short page) and an action row with a value and `right` set draws the page arrow after the value.
TEST_CASE("hookshot: the module tab's column is opaque and a valued page row has its arrow (patch 0031)") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0031-coop-menu-column.patch");
    REQUIRE_FALSE(patch.empty());
    const std::string menu = PatchedCode(patch, "gpu/vk/vk_menu.cpp");
    REQUIRE_FALSE(menu.empty());
    const auto in_menu = [&menu](const char* code) { return menu.find(Squashed(code)) != std::string::npos; };
    CHECK(in_menu("c.d->AddRectFilled(ImVec2(cx0, hh), ImVec2(cx1, c.h), ui::rgba(34, 26, 17));"));
    CHECK(in_menu("if (r.right) ui::triangle(c, ImVec2(vb.x - 15 * s, (va.y + vb.y) * 0.5f), 10 * s, 1,"));
}

// Row 156 (three crashes, 0xc0000005 in SDL_GetAudioStreamAvailable on the thread reading the co-op voice's microphone):
// Longshot's rec_ready (host/hos_sdl.c) called SDL_InitSubSystem(SDL_INIT_AUDIO) for every microphone list and open and
// never quit it. SDL3 counts those calls in 8 bits: the 256th wrapped the count to 0 and the next call restarted SDL's
// audio, destroying every SDL_AudioStream (the open microphones' too), which nn_audio_in.c then read freed. Patch 0033
// starts SDL's audio once, under a lock of its own; a failed start (SDL leaves it uncounted) is tried again.
TEST_CASE("hookshot: SDL's audio is started once for the microphones, not at every list and open (patch 0033)") {
    const std::string patch = ReadFile(kRepo / "hookshot" / "patches" / "0033-coop-sdl-audio-once.patch");
    REQUIRE_FALSE(patch.empty());
    const std::string sdl = PatchedCode(patch, "host/hos_sdl.c");
    REQUIRE_FALSE(sdl.empty());
    const auto has = [&sdl](const char* code) { return sdl.find(Squashed(code)) != std::string::npos; };
    // The old line, which counted one more start at every call, is gone.
    CHECK(patch.find("\n-    return rec.ok && hos_sdl_init(HOS_SDL_AUDIO);\n") != std::string::npos);
    CHECK_FALSE(has("return rec.ok && hos_sdl_init(HOS_SDL_AUDIO);"));
    // Once, remembered, and under the lock (the lists come from the menu's and the module's threads).
    CHECK(has("int audio_on;"));
    CHECK(has("static SRWLOCK rec_lock = SRWLOCK_INIT;"));
    const auto lock = sdl.find(Squashed("AcquireSRWLockExclusive(&rec_lock); if (!rec.tried) {"));
    const auto start = sdl.find(Squashed("if (rec.ok && !rec.audio_on) rec.audio_on = hos_sdl_init(HOS_SDL_AUDIO);"));
    const auto unlock = sdl.find(Squashed("const int ready = rec.ok && rec.audio_on; ReleaseSRWLockExclusive(&rec_lock); return ready;"));
    REQUIRE(lock != std::string::npos);
    REQUIRE(start != std::string::npos);
    REQUIRE(unlock != std::string::npos);
    CHECK(lock < start);
    CHECK(start < unlock);
    // No other patch starts SDL's audio from the host's code: one counted start for the microphones.
    for (const auto& entry : std::filesystem::directory_iterator(kRepo / "hookshot" / "patches")) {
        const std::string text = ReadFile(entry.path());
        for (auto at = text.find("\n+"); at != std::string::npos; at = text.find("\n+", at + 1)) {
            const std::string line = text.substr(at + 1, text.find('\n', at + 1) - at - 1);
            if (line.find("hos_sdl_init(HOS_SDL_AUDIO)") != std::string::npos) {
                CHECK(entry.path().filename().string() == "0033-coop-sdl-audio-once.patch");
                CHECK(line.find("!rec.audio_on") != std::string::npos);
            }
        }
    }
}
