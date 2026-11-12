// SPDX-License-Identifier: GPL-3.0-or-later
// The Hookshot module (hookshot/src) and our patch to Hookshot
// (hookshot/patches): what can be checked without the game.
#include <doctest/doctest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "coop_counters.h"
#include "coop_hold.h"
#include "coop_hookshot.h"
#include "hos_coop_world.h"
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

}  // namespace

TEST_CASE("hookshot: the API header in our patch is hookshot/src's") {
    CHECK(NewFile(Patch(), "host/hos_coop_api.h") == ReadFile(kRepo / "hookshot" / "src" / "hos_coop_api.h"));
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

TEST_CASE("hookshot: Link puppets follow the puppet guard in hookshot.exe") {
    // Unset: on with patch 0002's guard in the exe, off without it.
    CHECK(CoopHookshot::AllowLinkPuppets("", true));
    CHECK_FALSE(CoopHookshot::AllowLinkPuppets("", false));
    // LICHII_COOP_HOOKSHOT_LINK_PUPPETS forces it either way.
    CHECK(CoopHookshot::AllowLinkPuppets("1", false));
    CHECK_FALSE(CoopHookshot::AllowLinkPuppets("0", true));
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
    CHECK(module.find("CoopPuppetConfig::Configure()") != std::string::npos);
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
    // Hookshot's build gives the ImGui headers only to the files it lists: vk_coop.cpp is one (else it does not compile).
    CHECK(patch.find("+        imgui_users = (\"vk_imgui.cpp\", \"vk_menu.cpp\", \"vk_coop.cpp\")") != std::string::npos);
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
    CHECK(patch.find("+        if (hos_coop_char((unsigned)wp)) return 0;") != std::string::npos);
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
    // SDL's text events (both SDLs' layouts) and text input started on the window.
    CHECK(patch.find("+        case 0x303: {   /* (SDL_TextInputEvent: the text's pointer at 24") != std::string::npos);
    CHECK(patch.find("+        memcpy(e->text, raw + 12, sizeof e->text - 1);") != std::string::npos);
    CHECK(patch.find("+    hos_sdl_text_input(window);") != std::string::npos);
    // Keys by virtual-key code first, text as code points, the mouse as on Windows.
    CHECK(patch.find("+                    if (cvk && hos_coop_key(cvk, e.repeat)) {") != std::string::npos);
    CHECK(patch.find("+                    coop_text(e.text);") != std::string::npos);
    CHECK(patch.find("+                    else if (e.button == 1 && !mouse_captured && coop_mouse(e.x, e.y, 1)) {}") !=
          std::string::npos);
    // The key state for F6, F7 and the closing keys.
    CHECK(patch.find("+    api.key_down = bridge_key_down;") != std::string::npos);
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
}
