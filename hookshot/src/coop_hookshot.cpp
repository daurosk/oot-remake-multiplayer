// SPDX-License-Identifier: GPL-3.0-or-later
//
// Co-op inside Hookshot (docs/hookshot.md): hookshot_coop.dll, loaded by our
// patch's host/coop_bridge.c. It runs lichii/hook's co-op unchanged: the
// emulator's API slice the hook uses is answered by hookshot/compat from the
// bridge's table (CoopRuntime below). What differs from the emulator is only
// how the hook is driven:
// - hos_coop_frame at the every-frame site runs CoopHook::OnFrame, then the
//   callers' game calls (CoopCallers::RunFrame) directly, where the emulator's
//   cave asks them one at a time through `svc #0xf4` (coop_svc.cpp);
// - the AI hold reads the same table the emulator's hold cave reads
//   (coop_hold.h), at the same place in main;
// - settings: coop.ini beside longshot.exe, then the LICHII_COOP_* variables,
//   as in the emulator (the kit's launcher sets them from coop-options.ini).
#include "coop_hookshot.h"
#include "coop_game_clock.h"

#include <algorithm>
#include <cctype>
#include <array>
#include <cstddef>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstdlib>
#include <bit>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop_callers.h"
#include "coop_camera.h"
#include "coop_cheats.h"
#include "coop_cheats_panel.h"
#include "coop_debug_panels.h"
#include "coop_playtest.h"
#include "coop_talk_text.h"
#include "coop_char_colours.h"
#include "coop_gear_params.h"
#include "coop_chatbox.h"
#include "coop_draw.h"
#include "coop_map_icons.h"
#include "coop_dbno_draw.h"
#include "coop/host/map_drop_log.hpp"
#include "coop/host/save_transfer.hpp"
#include "coop_char_ages.h"
#include "coop_flag_native.h"
#include "coop_flags.h"
#include "coop_game_build.h"
#include "coop_i18n.h"
#include "coop_native_ui.h"
#include "coop_party_hud.h"
#include "coop_race_mode.h"
#include "coop_save.h"
#include "coop_hold.h"
#include "coop_counters.h"
#include "coop_world.h"
#include "coop_difficulty.h"
#include "coop_actor_events.h"
#include "coop_boss_native.h"
#include "coop_boss_orders.h"
#include "coop_boss_shooter_table.h"
#include "coop_boss_scenes.h"
#include "coop_shooter_probe.h"
#include "coop_hallway.h"
#include "coop_dbno_body.h"
#include "coop_dbno_game.h"
#include "coop_enemy_shots.h"
#include "coop_mod_api.h"
#include "coop_menu_tabs_rule.h"
#include "coop_mods_runtime.h"
#include "coop_part_fallback.h"
#include "coop_pad_order.h"
#include "coop_rando.h"
#include "coop_hook.h"
#include "coop_native_tags.h"
#include "coop_lobby_game.h"
#include "coop_lobby_tab.h"
#include "coop_voice.h"
#include "coop_fresh.h"
#include "coop_hud.h"
#include "coop_hud_dim.h"
#include "coop_hud_hearts.h"
#include "coop_link_sound.h"
#include "coop_map_page.h"
#include "coop_map_refresh.h"
#include "coop_testinput.h"
#include "coop_maps.h"
#include "coop_ngplus.h"
#include "coop_live_events.h"
#include "coop_objects_sync.h"
#include "coop_off_frame.h"
#include "coop_hit_replay.h"
#include "coop_input.h"
#include "coop_look.h"
#include "coop_look_share.h"
#include "coop_look_view.h"
#include "coop_lift_player_game.h"
#include "coop_objects.h"
#include "coop_local_game.h"
#include "coop_local_layer.h"
#include "coop_local_probe.h"
#include "coop_player_input.h"
#include "coop_png.h"
#include "coop_window_brand.h"
#include "coop_pose.h"
#include "coop_puppet.h"
#include "coop_puppet_config.h"
#include "coop_pack_notice.h"
#include "coop_frame_clock.h"
#include "coop_original.h"
#include "coop/protocol/messages.hpp"
#include "coop_runtime.h"
#include "coop_pause_files.h"
#include "coop_pickups.h"
#include "coop_settings.h"
#include "coop_svc.h"
#include "coop_time.h"
#include "coop_pause.h"
#include "coop_pause_tab.h"
#include "coop_pause_world.h"
#include "coop_longshot_settings.h"
#include "coop_title.h"
#include "coop_title_menu.h"
#include "core/core.h"
#include "coop_host_menu.h"
#include "coop_null_page.h"
#include "hos_coop_api.h"
#include "hos_coop_actors.h"
#include "hos_coop_nodes.h"
#include "hos_coop_bosses.h"
#include "hos_coop_boss_orders.h"
#include "hos_coop_boss_shooter.h"
#include "hos_coop_crawl.h"
#include "hos_coop_pose_play.h"
#include "hos_coop_pads.h"
#include "hos_coop_save_load.h"
#include "hos_coop_sound.h"
#include "hos_coop_pane_body.h"
#include "hos_coop_pane_text.h"
#include "hos_coop_host.h"
#include "hos_coop_world.h"
#include "hos_coop_view.h"
#include "hos_coop_turn.h"
#include "coop_export_guard.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>

#include "coop_clipboard_posix.h"
#endif

namespace {

const HosCoopApi* g_api = nullptr;
std::string g_build;
std::atomic<bool> g_on{false};  // (read on the game's threads by the world hooks)
// Original game (coop_original.h): the module off, Hookshot's F1 menu with the switch's tab alone.
std::atomic<bool> g_original_on{false};
std::mutex g_original_mutex;
CoopOriginal::Tab g_original;
std::atomic<std::uint32_t> g_world_version{0};  // Hookshot's HOS_COOP_WORLD_VERSION, 0 until it says hello
std::atomic<std::uint64_t> g_world_wraps{0};    // the wraps that Hookshot has, a bit for each id
std::atomic<bool> g_present_seen{false};  // our patch's present site has run
std::atomic<bool> g_flag_setters{false};  // our flag writes go through the game's setters (coop_flag_native.h)
bool g_hold = false;
Core::System g_system;

std::string Env(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr ? std::string(v) : std::string();
}

bool EnvOn(const char* name) {
    const auto v = Env(name);
    return !v.empty() && v != "0";
}

std::uint64_t SteadyMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

class BridgeGuest final : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        return CoopRuntime::Read(address, out, size);
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        return CoopRuntime::Write(address, in, size);
    }
};

// The game calls the hook's callers ask for this frame, made here.
// The actor a requestCreateActorAsync call names: x1 is {char*, capacity}.
std::string CreateName(const CoopPuppet::Call& c) {
    std::uint64_t text = 0;
    char name[0x41] = {};
    if (!CoopRuntime::Read(c.x[1], &text, sizeof text) || text == 0 ||
        !CoopRuntime::Read(text, name, sizeof name - 1)) {
        return {};
    }
    return std::string(name);
}

// Our patch 0002 (nx2/coop_puppet_guard.py) is in this longshot.exe: its log text is in the exe's read-only data.
// (Linux: always true, see the end.)
bool PuppetGuardInExe() {
#ifdef _WIN32
    static const char kMark[] = "coop: puppet guard: actor";
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    if (base == nullptr) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if ((sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 || (sec->Characteristics & IMAGE_SCN_MEM_READ) == 0) {
            continue;
        }
        const std::string_view data(reinterpret_cast<const char*>(base + sec->VirtualAddress),
                                    std::min<std::size_t>(sec->Misc.VirtualSize, sec->SizeOfRawData));
        if (data.find(std::string_view(kMark, sizeof(kMark) - 1)) != std::string_view::npos) {
            return true;
        }
    }
    return false;
#else
    return true;  // (Linux: Hookshot maps the whole guest window readable, so a puppet's null reads give 0 there
                  //  as on the emulator, with or without patch 0002; untested on Linux)
#endif
}

// Before the game makes our first puppet: its null reads give 0 (issue 150, coop_null_page.h). Once.
// Where that failed after all, the puppets' director learns it before the puppet exists (GearIsUnsafe,
// issue 232), and the create is refused unless HOS_COOP_NULL_READS=0 asked for the faults
// (coop_hookshot.h RefuseUnarmed: every held piece and the puppet itself read through null). True: make it.
bool ArmNullPage(const std::string& actor) {
    if (actor.rfind("CoopPuppet_", 0) != 0) {
        return true;
    }
    const std::string env = Env("HOS_COOP_NULL_READS");
    const bool armed = CoopNullPage::Arm(env);
    if (!armed) {
        CoopPuppet::TheDirector().SetNullReadsZero(false);
    }
    if (!CoopHookshot::RefuseUnarmed(actor, armed, env)) {
        return true;
    }
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) {
        LOG_WARNING(Core, "coop: {} not made: null-pointer reads cannot give 0 in this longshot.exe (see the null "
                          "page line above); other players show as their fairy",
                    actor);
    }
    return false;
}

// Whether a perceived puppet is safe here (coop_null_page.h Armable); if not, quiet puppets everywhere.
bool PerceivedSafe() {
    return CoopNullPage::Armable(Env("HOS_COOP_NULL_READS"), CoopNullPage::Query());
}

// coop_hookshot.h AllowLinkPuppets / RefuseCreate: without patch 0002 a Link-model puppet crashes Hookshot's game.
bool Refused(const CoopPuppet::Call& c) {
    static const bool allow = [] {
        const bool guard = PuppetGuardInExe();
        const bool on = CoopHookshot::AllowLinkPuppets(Env("LICHII_COOP_HOOKSHOT_LINK_PUPPETS"), guard);
        LOG_INFO(Core, "coop: Hookshot: other players as Link {} (puppet guard {} this longshot.exe{})",
                 on ? "on" : "off", guard ? "in" : "NOT in",
                 guard ? "" : ": build it again with Build-Hookshot-Coop for patch 0002");
        return on;
    }();
    if (c.function != CoopRuntime::MainBase() + CoopPuppet::addr::kCreate) {
        return false;
    }
    const std::string actor = CreateName(c);
    // A randomizer seed whose size table may lack the puppet pack's entries (coop_rando.h HoldPack): a pack actor
    // made now crashes the game, so none until the restart (the chat says RESTART); the others show as fairies.
    if (actor.rfind("Coop", 0) == 0) {
        if (const CoopRando::Rando* r = CoopModsRuntime::Rando(); r != nullptr && r->HoldPack()) {
            static bool held_said = false;
            static bool rows_said = false;
            if (r->RowsMissing()) {
                // (player report 144/145: the game would refuse it for ever, the body never shown)
                if (!rows_said) {
                    rows_said = true;
                    LOG_WARNING(Core, "coop: {} not made: the game's actor table comes from a mod without the "
                                      "puppet pack's rows, so the game would refuse it (see the randomizer line at the "
                                      "start, coop-rows.txt); other players show as their fairy",
                                actor);
                }
            } else if (!held_said) {
                held_said = true;
                LOG_WARNING(Core, "coop: {} not made: the randomizer seed's size table is being checked or was made "
                                  "again (restart the game); other players show as their fairy until then",
                            actor);
            }
            return true;
        }
    }
    if (!CoopHookshot::RefuseCreate(actor, allow)) {
        return !ArmNullPage(actor);
    }
    static bool said = false;
    if (!said) {
        said = true;
        LOG_WARNING(Core,
                    "coop: Hookshot: {} not made (this longshot.exe has no puppet guard, or "
                    "LICHII_COOP_HOOKSHOT_LINK_PUPPETS=0); other players show as their fairy",
                    actor);
    }
    return true;
}

// True while this thread runs one of the module's own game calls: a create
// or a delete the game asks for inside it is ours (a copy, a remove), not
// the game's own doing, unless the call runs a copy's own game code
// (t_copy_game; CoopActorEvents::Ours).
thread_local bool t_own_call = false;
thread_local bool t_copy_game = false;
struct OwnCall {
    explicit OwnCall(bool copy_game) {
        t_own_call = true;
        t_copy_game = copy_game;
    }
    ~OwnCall() {
        t_own_call = false;
        t_copy_game = false;
    }
    OwnCall(const OwnCall&) = delete;
    OwnCall& operator=(const OwnCall&) = delete;
};

// The flag writes that wait for the game's own setters (coop_flag_native.h), made here on the calc thread. A setter
// that cannot be called (no function there: another build of the lift) ends this: the writes go into the game's
// queues again, the ones that wait first.
void RunFlagSetters(void* ctx) {
    if (!g_flag_setters.load(std::memory_order_relaxed)) {
        return;
    }
    CoopHook::EmulatorMemory memory(g_system.ApplicationMemory(), CoopRuntime::MainBase());
    const bool ok = CoopFlagNative::RunSetters(
        memory, [ctx](std::uint64_t function, std::uint64_t x0, std::uint64_t x1, std::uint64_t x2,
                      std::optional<std::uint32_t> s0) {
            HosCoopCall k{};
            k.function = function;
            k.x[0] = x0;
            k.x[1] = x1;
            k.x[2] = x2;
            if (s0) std::memcpy(&k.s0, &*s0, sizeof k.s0);  // a Float setter's value
            std::uint64_t result = 0;
            return g_api->call(ctx, &k, &result) != 0;
        },
        [ctx](const void* data, std::size_t n) -> std::uint64_t {
            // (patch 0004's scratch: an older Hookshot has none, and String64 writes then go into the queues)
            const bool has = g_api->size >= offsetof(HosCoopApi, scratch) + sizeof(g_api->scratch) &&
                             g_api->scratch != nullptr;
            return has ? g_api->scratch(ctx, data, n) : 0;
        });
    if (ok) {
        return;
    }
    g_flag_setters.store(false);
    coop::host::GameFlags::use_calls(nullptr);
    std::size_t moved = 0, lost = 0;
    CoopFlagNative::Calls().run(coop::host::GameFlagCalls::kMost, [&](const coop::host::GameFlagWrite& w) {
        using coop::host::GameFlags;
        using coop::host::GameFlagType;
        const auto hash = static_cast<std::uint32_t>(w.id);
        bool done = false;
        switch (w.type) {
        case GameFlagType::boolean:
            done = GameFlags::set_bool(memory, hash, w.value != 0);
            break;
        case GameFlagType::int32:
            done = GameFlags::set_int(memory, hash, static_cast<std::int32_t>(w.value));
            break;
        case GameFlagType::float32:
            done = GameFlags::set_float(memory, hash, std::bit_cast<float>(w.value));
            break;
        case GameFlagType::enum32:
            done = GameFlags::set_enum(memory, hash, w.value);
            break;
        case GameFlagType::uint32:
            done = GameFlags::set_uint(memory, hash, w.value);
            break;
        case GameFlagType::key64:
            done = GameFlags::set_key64(memory, w.id, w.value != 0);
            break;
        case GameFlagType::string64:
            if (const auto text = CoopFlagNative::Calls().text(w)) done = GameFlags::set_string64(memory, hash, *text);
            break;
        }
        ++(done ? moved : lost);
        return true;
    });
    LOG_WARNING(Core, "coop: flags: a game setter could not be called: writes go into the game's queues from now on "
                      "({} waiting writes moved there, {} lost)", moved, lost);
}

// Every 10 s while anything moved: what the applier's wrap and the setter calls did.
void LogFlagNative() {
    static std::int64_t said_ms = 0;
    static std::uint64_t said_applies = 0, said_setters = 0;
    const std::int64_t now = SteadyMs();
    if (now - said_ms < 10'000) {
        return;
    }
    const CoopFlagNative::Counters& c = CoopFlagNative::Counts();
    const std::uint64_t applies = c.applies.load(), setters = c.setters.load();
    if (applies == said_applies && setters == said_setters) {
        return;
    }
    said_ms = now;
    said_applies = applies;
    said_setters = setters;
    LOG_INFO(Core, "coop: flags native: the game's applier ran {} times, {} writes read from its queues ({} dropped), "
                   "{} setter calls made for co-op ({} waiting, {} frames waited for room)",
             applies, c.writes.load(), CoopFlagNative::Events().dropped(), setters, CoopFlagNative::Calls().size(),
             c.no_room.load());
}

// Native boss sync (coop_boss_native.h): on when this hookshot.exe has
// both wraps (the hook table's rows 4 and 5) and LICHII_COOP_BOSS_NATIVE is not
// 0; the action verbs also need LICHII_COOP_BOSS_ACTIONS not 0.
bool WrapIn(std::uint32_t id) {  // (as CoopHookshot::HasWrap)
    return id < 64 && (g_world_wraps.load() >> id & 1) != 0;
}
void SetBossNative() {
    const bool brain = WrapIn(HOS_COOP_WRAP_BRAIN_VERB) && Env("LICHII_COOP_BOSS_NATIVE") != "0";
    const bool actions = brain && WrapIn(HOS_COOP_WRAP_ACTION_VERB) && Env("LICHII_COOP_BOSS_ACTIONS") != "0";
    CoopBossNative::Table& t = CoopBossNative::TheTable();
    const bool turns = CoopBossNative::TurnsSwitch(Env("LICHII_COOP_BOSS_TURNS"));
    if (t.TurnsOn() != turns) {
        t.SetTurns(turns);
        LOG_INFO(Core, "coop: bosses: turns {} (LICHII_COOP_BOSS_TURNS)", turns ? "on" : "off");
    }
    if (t.BrainOn() == brain && t.ActionsOn() == actions) {
        return;
    }
    t.SetOn(brain, actions);
    LOG_INFO(Core, "coop: bosses: native sync {} (brain verbs {}, action verbs {}){}", brain ? "on" : "off",
             WrapIn(HOS_COOP_WRAP_BRAIN_VERB) ? "wrapped" : "not wrapped in this hookshot.exe",
             WrapIn(HOS_COOP_WRAP_ACTION_VERB) ? (actions ? "wrapped" : "wrapped, off") : "not wrapped",
             brain ? "" : ": bosses as before (owner's animations, gohma-sync)");
}

// Row 222 (coop_boss_orders.h): one battle manager for every game, when this hookshot.exe wraps the sisters' message
// handler (patch 0036, row 25) and LICHII_COOP_BOSS_ORDERS is not 0. Asked with SetBossNative.
void SetBossOrders() {
    const bool on = WrapIn(HOS_COOP_WRAP_BOSS_ORDER) && Env("LICHII_COOP_BOSS_ORDERS") != "0";
    CoopBossOrders::Table& t = CoopBossOrders::TheTable();
    if (t.On() == on) {
        return;
    }
    t.SetOn(on);
    LOG_INFO(Core, "coop: bosses: battle manager orders {} ({})", on ? "shared" : "each game's own",
             WrapIn(HOS_COOP_WRAP_BOSS_ORDER) ? (on ? "wrapped" : "wrapped, off by LICHII_COOP_BOSS_ORDERS")
                                              : "not wrapped in this hookshot.exe");
}

// Rows 222, 286 (coop_boss_shooter_table.h): the owner's game decides a boss's shots, when this hookshot.exe wraps
// the Shooter's three verbs (patch 0045) and LICHII_COOP_BOSS_SHOOTER is not 0. Asked with SetBossNative.
void SetBossShooter() {
    const bool wrapped = WrapIn(HOS_COOP_WRAP_SHOOTER_PREPARE) && WrapIn(HOS_COOP_WRAP_SHOOTER_SHOOT) &&
                         WrapIn(HOS_COOP_WRAP_SHOOTER_SLEEP);
    const bool on = wrapped && Env("LICHII_COOP_BOSS_SHOOTER") != "0";
    CoopBossShooter::Table& t = CoopBossShooter::TheTable();
    if (t.On() == on) {
        return;
    }
    t.SetOn(on);
    LOG_INFO(Core, "coop: bosses: boss shots {} ({})", on ? "the owner's" : "each game's own",
             wrapped ? (on ? "wrapped" : "wrapped, off by LICHII_COOP_BOSS_SHOOTER")
                     : "not wrapped in this hookshot.exe");
}

// Row 286's probe (coop_shooter_probe.h): log only, with LICHII_COOP_SHOOTER_PROBE=1 in a hookshot.exe with patch
// 0045's wraps. Asked with SetBossNative.
void SetShooterProbe() {
    if (Env("LICHII_COOP_SHOOTER_PROBE") != "1") {
        return;
    }
    const bool prepare = WrapIn(HOS_COOP_WRAP_SHOOTER_PREPARE);
    const bool shoot = WrapIn(HOS_COOP_WRAP_SHOOTER_SHOOT);
    const bool sleep = WrapIn(HOS_COOP_WRAP_SHOOTER_SLEEP);
    CoopShooterProbe::Probe& probe = CoopShooterProbe::TheProbe();
    const bool on = prepare || shoot || sleep;
    static bool said = false;
    if (probe.On() == on && std::exchange(said, true)) {
        return;
    }
    said = true;
    probe.SetOn(on);
    LOG_INFO(Core, "coop: shooterprobe: {} (prepare {}, shoot {}, sleep {})", on ? "on" : "off",
             prepare ? "wrapped" : "not wrapped", shoot ? "wrapped" : "not wrapped", sleep ? "wrapped" : "not wrapped");
}
void ShooterProbe() {
    CoopShooterProbe::Probe& probe = CoopShooterProbe::TheProbe();
    if (!probe.On()) {
        return;
    }
    probe.Look([](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); },
               CoopRuntime::MainBase(), SteadyMs());
    for (const std::string& line : probe.TakeLines()) {
        LOG_INFO(Core, "coop: shooterprobe: {}", line);
    }
}

// Every 10 s while orders move: the counts (the probe run's lines).
void LogBossOrders() {
    static std::uint64_t said_ms = 0;
    static CoopBossOrders::Counts said{};
    const std::uint64_t now = SteadyMs();
    if (now - said_ms < 10'000) {
        return;
    }
    const CoopBossOrders::Counts c = CoopBossOrders::TheTable().GetCounts();
    if (c.owner_kept == said.owner_kept && c.copy_skipped == said.copy_skipped && c.delivered == said.delivered &&
        c.copy_own_ran == said.copy_own_ran && c.dropped == said.dropped && c.restarts == said.restarts &&
        c.replayed == said.replayed) {
        return;
    }
    said_ms = now;
    said = c;
    LOG_INFO(Core, "coop: bosses: battle orders: ours sent {}; copies: {} owner's put in, {} own skipped, {} own run "
                   "before the owner's came, {} dropped, {} started again; {} to unlisted actors; made ours: {} of our "
                   "manager's skipped orders put in",
             c.owner_kept, c.delivered, c.copy_skipped, c.copy_own_ran, c.dropped, c.restarts, c.unknown, c.replayed);
}

// Every 10 s while boss shots move: the counts (the probe and fix runs' lines).
void LogBossShooter() {
    static std::uint64_t said_ms = 0;
    static CoopBossShooter::Counts said{};
    const std::uint64_t now = SteadyMs();
    if (now - said_ms < 10'000) {
        return;
    }
    const CoopBossShooter::Counts c = CoopBossShooter::TheTable().GetCounts();
    if (c.owner_kept == said.owner_kept && c.owner_refused == said.owner_refused &&
        c.copy_skipped == said.copy_skipped && c.copy_own_ran == said.copy_own_ran && c.made == said.made &&
        c.refused == said.refused && c.no_template == said.no_template && c.not_prepared == said.not_prepared &&
        c.dropped == said.dropped && c.restarts == said.restarts && c.put_back == said.put_back) {
        return;
    }
    said_ms = now;
    said = c;
    LOG_INFO(Core, "coop: bosses: boss shots: ours sent {} ({} refused by the game); copies: {} own skipped, {} own "
                   "run before the owner's came, {} owner's made ({} refused), {} no template, {} not prepared, {} "
                   "dropped, {} started again, {} put back",
             c.owner_kept, c.owner_refused, c.copy_skipped, c.copy_own_ran, c.made, c.refused, c.no_template,
             c.not_prepared, c.dropped, c.restarts, c.put_back);
}

// Row 222: how many plugs of the action-verb wrap read as a name (the owner's variations), and how many did not.
std::atomic<std::uint64_t> g_plug_read{0};
std::atomic<std::uint64_t> g_plug_unread{0};

// Every 10 s while a boss is synced natively: the counts (first proof lines).
void LogBossNative() {
    static std::uint64_t said_ms = 0;
    static CoopBossNative::Counts said{};
    const std::uint64_t now = SteadyMs();
    if (now - said_ms < 10'000) {
        return;
    }
    const CoopBossNative::Counts c = CoopBossNative::TheTable().Totals();
    if (c.owner_brain == said.owner_brain && c.owner_action == said.owner_action && c.copy_skipped == said.copy_skipped &&
        c.brain_applied == said.brain_applied && c.action_applied == said.action_applied &&
        c.brain_came == said.brain_came && c.brain_already == said.brain_already &&
        c.copy_own_run == said.copy_own_run && c.copy_own_late == said.copy_own_late &&
        c.copy_life_held == said.copy_life_held && c.copy_killed == said.copy_killed &&
        c.copy_finished == said.copy_finished && c.copy_shot_hits == said.copy_shot_hits &&
        c.copy_hits_to_owner == said.copy_hits_to_owner && c.copy_own_stuns == said.copy_own_stuns &&
        c.plain_wakes == said.plain_wakes && c.plain_deaths == said.plain_deaths &&
        c.action_slot_busy == said.action_slot_busy && c.action_busy_again == said.action_busy_again &&
        c.body_deaths == said.body_deaths &&
        c.variant_named == said.variant_named && c.variant_plain == said.variant_plain &&
        c.variant_node_moved == said.variant_node_moved && c.variant_node_conflict == said.variant_node_conflict &&
        c.copy_own_move == said.copy_own_move && c.owner_move_left == said.owner_move_left &&
        c.owner_variants == said.owner_variants && c.own_starts_held == said.own_starts_held &&
        c.own_start_reasks == said.own_start_reasks && c.made_ours_own_other == said.made_ours_own_other) {
        return;
    }
    said_ms = now;
    said = c;
    LOG_INFO(Core, "coop: bosses: ours sent {} brain + {} action verbs ({} by plug); copies: {} own skipped, {} brain + "
                   "{} action verbs put in ({} waited for a node; {} answered a busy slot, {} of them asked again next frame), {} own run in our event or after our hit ({} late, {} hits by our shots, {} own stuns; "
                   "{} hits that took life left to the owner), "
                   "{} own minions not made; {} own Life steps held at 1, {} let die after the owner's, {} ended in their death; "
                   "plain enemies: {} own other phases told, {} own wakes run, {} deaths begun; {} boss bodies dead before the outro; "
                   "owner's brain verbs given to copies {} ({} the copy was in already); variations put in: {} named, {} "
                   "plain, {} not known, {} past the name pools; {} through another node (the move's chose another "
                   "variation), {} with no such node left (row 222); {} own asks of the owner's move run ({} renamed to its "
                   "variation), {} owner's repeats left out; owner's variations kept {} (plug names read {}, not read {}); "
                   "own starts held {} (asked again {}, {} reached the cap, {} met the owner's move), {} made ours "
                   "with its own AI in another move",
             c.owner_brain, c.owner_action, c.plug_actions, c.copy_skipped, c.brain_applied, c.action_applied,
             c.action_waiting, c.action_slot_busy, c.action_busy_again, c.copy_own_run, c.copy_own_late, c.copy_shot_hits, c.copy_own_stuns, c.copy_hits_to_owner, c.minions_stopped, c.copy_life_held,
             c.copy_killed, c.copy_finished, c.plain_wants, c.plain_wakes, c.plain_deaths, c.body_deaths, c.brain_came,
             c.brain_already, c.variant_named, c.variant_plain, c.variant_unknown, c.variant_pool_full,
             c.variant_node_moved, c.variant_node_conflict, c.copy_own_move, c.own_move_renamed, c.owner_move_left, c.owner_variants, g_plug_read.load(),
             g_plug_unread.load(), c.own_starts_held, c.own_start_reasks, c.own_start_holds_capped,
             c.own_start_holds_met, c.made_ours_own_other);
}

// ---- Name tags on the game's own balloons (coop_native_tags.h, B3 of reports/ui-audit-01112.md), at the frame hook.
// LICHII_COOP_NATIVE_TAGS=0: the overlay's drawn name text instead (no arrows, no dots either way). The game gave no
// memory for the names: the drawn text too, said once in the log.
class TagsGame final : public CoopNativeTags::Game {
public:
    explicit TagsGame(void* ctx) : ctx_(ctx) {}
    std::uint64_t MainBase() const override { return CoopRuntime::MainBase(); }
    bool Read(std::uint64_t a, void* out, std::size_t n) override { return CoopRuntime::Read(a, out, n); }
    bool Write(std::uint64_t a, const void* in, std::size_t n) override { return CoopRuntime::Write(a, in, n); }
    std::optional<std::uint64_t> Call(std::uint64_t f, const std::array<std::uint64_t, 5>& x) override {
        HosCoopCall k{};
        k.function = f;
        for (std::size_t i = 0; i < x.size(); ++i) k.x[i] = x[i];
        std::uint64_t x0 = 0;
        if (g_api == nullptr || !g_api->call(ctx_, &k, &x0)) return std::nullopt;
        return x0;
    }

private:
    void* ctx_;
};
std::atomic<bool> g_tags_drawn{false};   // true: the overlay draws the name text (native off or failed)
CoopNativeTags::Balloons g_balloons;     // the game's thread only
// The others on the maps as the game's own map icons (coop_map_icons.h, item 8 / #255). LICHII_COOP_MAP_NATIVE=0: the
// overlay's map names instead. g_map_native: the copies were found (the pack has them): the overlay draws none.
CoopMapIcons::Icons g_map_icons;         // the game's thread only
std::atomic<bool> g_map_native{false};

// B8's probe (LICHII_COOP_MAP_PROBE=1, off by default): every 5 s, the MapIcon layouts the HUD's map screen holds
// and the balloon screen's shown parts, into the log (how the game makes its own map icons; whether our balloons stay).
void MapProbe(void* ctx) {
    static const bool on = Env("LICHII_COOP_MAP_PROBE") == "1";
    static std::uint64_t next = 0;
    if (!on || SteadyMs() < next) return;
    next = SteadyMs() + 5000;
    TagsGame game(ctx);
    const std::uint64_t map = CoopNativeTags::ScreenByVtable(game, CoopNativeTags::kMapMenuVtable);
    // (the screen itself holds only its own layout: the icon parts' layouts hang under their panes and the game's
    // run-time copies one level down, so the probe walks them as Step does: Icons::Probe)
    if (map == 0) LOG_INFO(Core, "coop: map icons probe: no map screen");
    for (const std::string& line : map != 0 ? g_map_icons.Probe(game) : std::vector<std::string>{}) {
        LOG_INFO(Core, "coop: map icons probe: {}", line);
    }
    LOG_INFO(Core, "coop: map icons probe: copies up {} shown {} copy0 {:#x} ({})", g_map_icons.Up(), g_map_icons.shown(),
             g_map_icons.copy(0), g_map_icons.why());
    const std::uint64_t balloons = CoopNativeTags::ScreenByVtable(game, CoopNativeTags::addr::kBalloonVtable);
    std::string parts;
    for (std::size_t i = 0; i < CoopNativeTags::addr::kParts && balloons != 0; ++i) {
        std::uint64_t part = 0, root = 0;
        float xy[2] = {}, alpha = 0;
        CoopRuntime::Read(balloons + CoopNativeTags::addr::kPartTable + i * CoopNativeTags::addr::kPartStride, &part, 8);
        if (const auto r = part != 0 ? game.Call(CoopRuntime::MainBase() + CoopNativeTags::addr::kPartRootPane,
                                                 {part + 0x40, 0, 0, 0, 0})
                                     : std::nullopt) {
            root = *r;
        }
        if (root != 0) {
            CoopRuntime::Read(root + CoopNativeTags::addr::kPaneXY, xy, sizeof xy);
            CoopRuntime::Read(root + 0x78, &alpha, sizeof alpha);
        }
        parts += fmt::format(" {}:{:.0f},{:.0f} a{:.2f}", i, xy[0], xy[1], alpha);
    }
    LOG_INFO(Core, "coop: map icons probe: balloon screen {:#x} parts{} (ours shown: {})", balloons, parts, g_balloons.shown());
}

void MapIcons(void* ctx) {
    static const bool on = Env("LICHII_COOP_MAP_NATIVE") != "0";
    // (set, 0 included: 0 = not turned, the escape from the inferred kPaneTurn without a rebuild)
    static const std::string turn_env = Env("LICHII_COOP_MAP_TURN_AT");
    static const std::uint64_t turn_at = std::strtoull(turn_env.c_str(), nullptr, 16);
    static std::string said;
    if (!on) return;
    // A load: every pane the copies knew may be freed; forgotten before anything else (also while nothing is shown,
    // so a load between two co-op map views is seen: the party-hud-crash-c126 rule, as CoopMapPage's).
    if (CoopNativeUi::SceneLoading()) {
        g_map_icons.Forget();
        return;
    }
    // No turn place known (kPaneTurn 0 and no trial, or TURN_AT=0): the game's copies would show the others' arrows
    // unturned, so they stay down and the overlay keeps drawing the turned arrows (G2a; no loss against 0.11.12).
    static const std::uint64_t place = turn_env.empty() ? CoopMapIcons::kPaneTurn : turn_at;
    const auto frame = CoopMapIcons::Taken();
    const bool stepping = place != 0 && !(frame.marks.empty() && g_map_icons.shown() == 0);
    // The turn place's probe (play-test bridge only, test_map_turn_probe.py): while the copies are not stepped (no
    // place known, solo, nobody on the map) the game's own arrow's words still go into map_native, every 30 frames,
    // read only, every pane looked up fresh (a load drops nothing it keeps).
    static std::uint32_t probe_tick = 0;
    if (!stepping && CoopPlaytest::On() && ++probe_tick % 30 == 0) {
        TagsGame game(ctx);
        CoopMapIcons::PublishState(CoopMapIcons::Json(g_map_icons.ProbeArrow(game)));
    }
    if (!stepping) return;   // no turn place (the overlay's turned arrows stay), or solo / nobody on the map
    TagsGame game(ctx);
    g_map_icons.TurnAt(place);
    g_map_icons.Step(game, frame);
    if (CoopPlaytest::On()) CoopMapIcons::PublishState(CoopMapIcons::Json(g_map_icons.Snapshot(game)));   // (test-only)
    if (g_map_icons.Up()) g_map_native.store(true);
    if (g_map_icons.why() != said) {
        said = g_map_icons.why();
        LOG_INFO(Core, "coop: map icons: {}", said.empty() ? fmt::format("on the game's map icons ({} shown)", g_map_icons.shown()) : said);
    }
}

// The steps that ran a second or more after the one before (coop_hookshot.h SlowStep), for the bridge's state.
constexpr std::uint64_t kSlowStepMs = 1000;
std::mutex g_slow_mutex;
CoopHookshot::SlowStep g_party_slow, g_tags_slow;
CoopHookshot::TagsNow g_tags_now;   // (g_slow_mutex)

void NoteSlow(CoopHookshot::SlowStep& slot, std::uint64_t& last_ms, std::uint64_t now, int shown) {
    const std::uint64_t gap = last_ms != 0 && now > last_ms ? now - last_ms : 0;
    last_ms = now;
    if (gap < kSlowStepMs) return;
    std::lock_guard lock(g_slow_mutex);
    ++slot.count;
    slot.gap_ms = gap;
    slot.shown = shown;
}

void NativeTags(void* ctx) {
    static const bool on = Env("LICHII_COOP_NATIVE_TAGS") != "0";
    static bool said = false, said_fail = false;
    MapProbe(ctx);
    MapIcons(ctx);
    if (!on) {
        g_tags_drawn.store(true);
        return;
    }
    TagsGame game(ctx);
    std::vector<CoopNativeTags::Spot> spots;
    const auto markers = CoopHook::CurrentMarkers();
    if (markers && !markers->items.empty()) {
        std::uint8_t self = 0;
        const auto rows = CoopHook::PlayerList();
        for (const auto& row : rows) {
            if (row.self) self = row.number;
        }
        for (const auto& m : markers->items) {
            if (m.edge || m.self || m.tag.empty() || !m.tag_head) continue;
            CoopNativeTags::Spot s{m.tag, m.tag_x, m.tag_y, m.alpha, 0xFFFFFF};
            for (const auto& row : rows) {
                if (row.number != 0 && row.name == m.tag) s.rgb = CoopDraw::Lifted(CoopPuppet::NameColor(row.number, self));
            }
            spots.push_back(std::move(s));
        }
    }
    static std::uint64_t last_tags_ms = 0;
    if (spots.empty() && g_balloons.shown() == 0) {   // solo, or nobody near: the balloons are not touched
        NoteSlow(g_tags_slow, last_tags_ms, SteadyMs(), 0);
        std::lock_guard lock(g_slow_mutex);
        g_tags_now = {};
        return;
    }
    const bool up = g_balloons.Step(game, spots, markers ? markers->width : 0.0f, markers ? markers->height : 0.0f);
    {
        const auto seen = g_balloons.seen();
        std::lock_guard lock(g_slow_mutex);
        g_tags_now = {static_cast<int>(seen.shown), static_cast<int>(seen.open), seen.screen_root};
    }
    NoteSlow(g_tags_slow, last_tags_ms, SteadyMs(), up ? static_cast<int>(g_balloons.shown()) : 0);
    if (g_balloons.no_memory()) {
        g_tags_drawn.store(true);
        if (!said_fail) {
            said_fail = true;
            LOG_WARNING(Core, "coop: native tags: the game gave no memory for the names; drawn name tags instead");
        }
        return;
    }
    if (up && g_balloons.shown() > 0 && !said) {
        said = true;
        LOG_INFO(Core, "coop: native tags: name tags on the game's balloons ({} shown, screen {:#x})", g_balloons.shown(),
                 g_balloons.screen());
    }
}

void RunCalls(void* ctx) {
    BridgeGuest guest;
    CoopCallers::RunFrame(guest, SteadyMs(), CoopSvc::kCallsPerFrame, [ctx](const CoopPuppet::Call& c) {
        if (Refused(c)) {
            return std::optional<std::uint64_t>(0);  // as the game refusing it: asked again later
        }
        HosCoopCall k{};
        k.function = c.function;
        std::memcpy(k.x, c.x.data(), sizeof k.x);
        std::memcpy(k.stack, c.stack.data(), sizeof k.stack);
        k.s0 = c.s0;
        std::uint64_t x0 = 0;
        const OwnCall own(c.copy_game);  // (what the game asks inside it is our own asking: World())
        if (!g_api->call(ctx, &k, &x0)) {
            LOG_WARNING(Core, "coop: no game function at {:#x}", c.function);
            return std::optional<std::uint64_t>();
        }
        return std::optional<std::uint64_t>(x0);
    });
}

// Case 1 of a join's state (player reports 107, 129, 132): a guest asked for our save while our file is played, and
// the file on disk is only as new as our last door. Our game is asked for its own autosave with the call its
// EventTriggerRequestAutoSave node makes at the end of an event (the node's body, main+0x31f2f50): the AutoSaveMgr is
// the game system's component 0xc (main+0xba7880(system, 0xc), 0 when there is none), then
// main+0xba7730(mgr, 5, 0). That call runs the game's own gate first (main+0xba72d0) and, only when the gate lets it,
// marks the request: bit 0 of the byte at *(mgr+0x130)+5; the manager saves at its own next step, into the slot it
// always uses. The gate refuses (nothing marked) while mgr+0x158 has bit 2 or 3, while the game's autosave-off Bool
// is set, in its cooldown after a save (the float mgr+0x15c above 0) or in a player state it does not save in.
// rejoin-host-273-3: a refusal is often only for a moment (the cooldown right after a load or a scene change), so the
// ask is made again every kAutoSaveRetryUs until the mark is there or its window ends (the guests' round:
// save::Planner::kFreshSaveUs; the test commands' autosave: kTestAutoSaveUs). Each answer is logged, with the gate's
// own inputs on a refusal. We write nothing in the game; the send waits for a save written after the ask (the
// Planner's round), and after its window our last save on disk goes as before.
constexpr std::uint64_t kGameSystemGot = 0x4c7e178;    // main+: GOT -> the global that holds the game system
constexpr std::uint64_t kSystemComponent = 0xba7880;   // main+: (system, index) -> the component, or 0
constexpr std::uint64_t kAutoSaveMgrIndex = 0xc;       // the AutoSaveMgr's index (the node's own)
constexpr std::uint64_t kRequestAutoSave = 0xba7730;   // main+: (mgr, kind, 0): the gate, then the request mark
constexpr std::uint64_t kAutoSaveKindEvent = 5;        // EventTriggerRequestAutoSave's kind
constexpr std::uint64_t kAutoSaveMgrState = 0x130;     // mgr: -> the state the request marks (0 before it is made)
constexpr std::uint64_t kAutoSaveMark = 5;             // state+: the request byte; bit 0 is the mark (main+0xba77dc)
constexpr std::uint64_t kAutoSaveMgrOff = 0x158;       // mgr: u32; bit 2 or 3 and the gate refuses at once
constexpr std::uint64_t kAutoSaveCooldown = 0x15c;     // mgr: float; above 0 and the gate refuses (after a save)
constexpr std::uint64_t kAutoSaveRetryUs = 250'000;
constexpr std::uint64_t kTestAutoSaveUs = 30'000'000;

struct AutoSaveAsk {   // the calc thread's ask still open (RequestHostAutoSave only)
    CoopSave::SaveRequest kind = CoopSave::SaveRequest::none;
    std::uint64_t until_us = 0;
    std::uint64_t next_us = 0;
    unsigned refusals = 0;
};
AutoSaveAsk g_autosave_ask;

std::uint64_t AutoSaveNowUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// The gate's inputs we can read (main+0xba72d0): why it refused, for the log.
std::string AutoSaveRefusal(std::uint64_t mgr) {
    std::uint32_t off = 0;
    float cooldown = 0.0f;
    g_api->read(mgr + kAutoSaveMgrOff, &off, sizeof off);
    g_api->read(mgr + kAutoSaveCooldown, &cooldown, sizeof cooldown);
    if ((off & 0xc) != 0) return fmt::format("the manager's own off bits, +0x158 = {:#x}", off);
    if (cooldown > 0.0f) return fmt::format("its cooldown after a save, +0x15c = {}", cooldown);
    return fmt::format("the game's autosave-off setting or a player state it does not save in, +0x158 = {:#x}, "
                       "no cooldown",
                       off);
}

void RequestHostAutoSave(void* ctx) {
    const auto request = CoopSave::TakeSaveRequest();
    if (request == CoopSave::SaveRequest::none && g_autosave_ask.kind == CoopSave::SaveRequest::none) return;
    const std::uint64_t now = AutoSaveNowUs();
    if (request != CoopSave::SaveRequest::none) {   // (a new ask: its own window; a guests' ask wins over a test's)
        if (g_autosave_ask.kind != CoopSave::SaveRequest::guests || request == CoopSave::SaveRequest::guests) {
            g_autosave_ask.kind = request;
        }
        g_autosave_ask.until_us =
            now + (g_autosave_ask.kind == CoopSave::SaveRequest::test ? kTestAutoSaveUs
                                                                       : coop::host::save::Planner::kFreshSaveUs);
        g_autosave_ask.next_us = now;
        g_autosave_ask.refusals = 0;
    }
    if (now < g_autosave_ask.next_us) return;
    // (the test commands' `autosave`, rejoin-host-273-2: the same call; no guest waits for it)
    const bool test = g_autosave_ask.kind == CoopSave::SaveRequest::test;
    const bool first = g_autosave_ask.refusals == 0;
    const char* const without = test ? "the test's autosave is not asked" : "the guests get our last save on disk";
    const auto end = [&](const char* why) {
        if (why != nullptr) LOG_WARNING(Core, "coop: save: {}: {}", why, without);
        g_autosave_ask = AutoSaveAsk{};
    };
    if (now >= g_autosave_ask.until_us) {
        return end("our game's own gate refused the autosave the whole window");
    }
    g_autosave_ask.next_us = now + kAutoSaveRetryUs;
    const std::uint64_t base = CoopRuntime::MainBase();
    const auto read64 = [](std::uint64_t at) {
        std::uint64_t v = 0;
        return g_api->read(at, &v, sizeof v) != 0 ? v : 0;
    };
    const auto marked = [](std::uint64_t state) {
        std::uint8_t v = 0;
        return g_api->read(state + kAutoSaveMark, &v, sizeof v) != 0 && (v & 1) != 0;
    };
    const std::uint64_t got = read64(base + kGameSystemGot);
    const std::uint64_t system = got != 0 ? read64(got) : 0;
    if (system == 0) return end("our game has no game system yet");
    HosCoopCall k{};
    k.function = base + kSystemComponent;
    k.x[0] = system;
    k.x[1] = kAutoSaveMgrIndex;
    std::uint64_t mgr = 0;
    const std::uint64_t state = g_api->call(ctx, &k, &mgr) && mgr != 0 ? read64(mgr + kAutoSaveMgrState) : 0;
    if (state == 0) return end("our game's autosave manager is not there");
    const bool before = marked(state);
    k = HosCoopCall{};
    k.function = base + kRequestAutoSave;
    k.x[0] = mgr;
    k.x[1] = kAutoSaveKindEvent;
    k.x[2] = 0;
    std::uint64_t x0 = 0;
    if (!g_api->call(ctx, &k, &x0)) {
        LOG_WARNING(Core, "coop: no game function at {:#x}", k.function);
        return end("the autosave call is not there");
    }
    if (first) {
        if (test) {
            LOG_INFO(Core, "coop: save: asked our game for its own autosave (an event's end request), for the test "
                           "commands' autosave: the game's own gate may refuse it");
        } else {
            LOG_INFO(Core, "coop: save: asked our game for its own autosave (an event's end request): the guests' "
                           "send waits for that save, at most {} s",
                     coop::host::save::Planner::kFreshSaveUs / 1'000'000);
        }
    }
    if (marked(state)) {
        LOG_INFO(Core, "coop: save: our game's gate took the autosave ask{} after {} refusals: it saves at its own "
                       "next step",
                 before ? " (an earlier ask was still marked)" : "", g_autosave_ask.refusals);
        return end(nullptr);
    }
    if (g_autosave_ask.refusals++ == 0) {
        LOG_INFO(Core, "coop: save: our game's gate refused the autosave ask ({}): asked again every {} ms for {} s",
                 AutoSaveRefusal(mgr), kAutoSaveRetryUs / 1000, (g_autosave_ask.until_us - now) / 1'000'000);
    }
}

}  // namespace

namespace CoopRuntime {
bool Running() {
    return g_api != nullptr;
}
std::uint64_t MainBase() {
    return g_api != nullptr ? g_api->main_base : 0;
}
const std::string& MainBuildHex() {
    return g_build;
}
std::filesystem::path GameDir() {
    if (g_api == nullptr || g_api->game_dir == nullptr) {
        return std::filesystem::path(".");
    }
    // UTF-8 from the bridge: a folder name outside the ANSI code page stays whole.
    const std::string dir = g_api->game_dir;
    return std::filesystem::path(std::u8string(dir.begin(), dir.end()));
}
bool Valid(std::uint64_t address, std::size_t size) {
    // The bridge checks a whole range before it copies: one byte at each end
    // says whether the range is mapped (Hookshot's guest memory has no holes
    // inside a mapped range the hook reads).
    std::uint8_t b = 0;
    return g_api != nullptr && size != 0 && address + size > address && g_api->read(address, &b, 1) != 0 &&
           g_api->read(address + size - 1, &b, 1) != 0;
}
bool Read(std::uint64_t address, void* out, std::size_t size) {
    return g_api != nullptr && g_api->read(address, out, size) != 0;
}
bool Write(std::uint64_t address, const void* in, std::size_t size) {
    return g_api != nullptr && g_api->write(address, in, size) != 0;
}
bool Cas32(std::uint64_t address, std::uint32_t expected, std::uint32_t desired, std::uint32_t& seen) {
    return g_api != nullptr && g_api->cas32(address, expected, desired, &seen) != 0;
}
void Log(const std::string& line) {
    if (g_api != nullptr && g_api->log != nullptr) {
        g_api->log(line.c_str());
    }
}
bool PadButtons(std::uint64_t& out) {
    if (!CoopHookshot::HasPadButtons(g_api)) {
        return false;
    }
    out = g_api->pad_buttons();
    return true;
}
}  // namespace CoopRuntime

// The emulator-only parts of the hook, as Hookshot has them.
namespace CoopSvc {
std::string Refusal() {
    return {};  // nothing to refuse: Hookshot applies no exefs patches (its host/mods.c)
}
}  // namespace CoopSvc

namespace CoopHookshot {

// Host and Join from the game's menu (hos_coop_host.h, coop_host_menu.h).
CoopHostMenu::Menu& HostMenu() {
    static CoopHostMenu::Menu menu([] {
        CoopHostMenu::Hooks h;
        h.connect = [](const CoopHostMenu::Target& t) {
            const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
            std::vector<std::string> errors;
            CoopHook::Settings s = CoopHook::LoadSettings(ini, errors);  // the colour, as the player set it
            s.server = t.server;
            s.port = t.port;
            s.name = t.name;
            s.password = t.password;
            return CoopHook::Begin(std::move(s));
        };
        h.disconnect = [] {
            CoopHook::End();
            CoopLobby::Forget();  // the Lobby tab shows nobody once co-op is left (lobby-state-b)
        };
        h.client_id = [] { return CoopTitle::ClientId(); };
        h.ngplus_level = [] { return CoopNgPlus::Level(); };  // (Navi) New Game+ for our server
        h.password_changed = [](const std::string& password) { CoopHook::SetJoinPassword(password); };
        h.log = [](const std::string& line) { LOG_INFO(Core, "{}", line); };
        h.default_name = [] {
            const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
            std::vector<std::string> errors;
            return CoopHook::LoadSettings(ini, errors).name;
        };
        return h;
    }());
    return menu;
}

namespace {

// F6 and F7 (CoopDraw::Keys), only while one of this process's windows has
// the keyboard. On Linux through the table's key_down (the SDL window's
// keys, none while it does not have the keyboard).
bool KeyDown(int vk) {
#ifdef _WIN32
    DWORD pid = 0;
    const HWND front = GetForegroundWindow();
    if (front == nullptr || (GetWindowThreadProcessId(front, &pid), pid) != GetCurrentProcessId()) {
        return false;
    }
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    return HasKeyDown(g_api) && g_api->key_down(static_cast<std::uint32_t>(vk)) != 0;
#endif
}

std::mutex g_keys_mutex;
CoopDraw::Keys g_keys;

// The chat box (coop_chatbox.h): the window's thread types, the game's input
// thread asks whether it has the keyboard, the render thread draws it.
std::mutex g_box_mutex;
CoopChatBox::Box g_box;
unsigned g_box_lead = 0;  // a double-byte code page's lead byte, waiting for its second
bool g_box_key_set = false;  // the open key was taken from LICHII_COOP_CHAT_KEY
// The tab's new open key (SetChatKey), taken by the next key press under g_box_mutex. Not a lock: the tab calls in
// holding CoopPause's own mutex, and TypeChar takes that one while it holds g_box_mutex (one lock order only).
constexpr std::uint64_t kNoChatKey = ~std::uint64_t{0};
std::atomic<std::uint64_t> g_box_key_next{kNoChatKey};

// (under g_box_mutex) "" = T, the box's own default.
void UseChatKey(const std::string& v) {
    if (v.empty()) {
        return;
    }
    if (const auto k = CoopChatBox::ParseOpenKey(v)) {
        g_box.SetOpenKey(*k);
        LOG_INFO(Core, "coop: chat box key: {}", *k == 0 ? std::string("off") : v);
    } else {
        LOG_WARNING(Core, "coop: LICHII_COOP_CHAT_KEY={} is not a letter, a digit, F1 to F12 or off; T opens the chat box", v);
    }
}
// The item cheats panel (coop_cheats_panel.h), under the same mutex.
CoopCheatsPanel::Panel g_cheats;

// The game look (coop_draw.h Look): the HUD pack's font and heart pictures,
// loaded through hos_coop_draw2's font and image calls on the render thread
// the first time it draws. Off for good when there is no pack, a file will
// not load, or Hookshot is older (it calls hos_coop_draw only).
enum : int { kLookNotYet = 0, kLookOn = 1, kLookOff = 2 };
std::atomic<int> g_look_state{kLookNotYet};
CoopDraw::Look g_look;  // the render thread's

std::string Utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

void LoadLook(const HosCoopDraw& d) {
    const auto dir = CoopHud::Folder();  // it logs why not
    if (!dir) {
        g_look_state = kLookOff;
        return;
    }
    CoopDraw::Look l;
    l.regular = d.font(d.user, Utf8(*dir / "font-regular.otf").c_str());
    if (std::ifstream f(*dir / "font-regular.otf", std::ios::binary); f) {   // its metrics: the title prompt's words centred on its button
        l.regular_metrics = CoopDraw::MetricsOf({std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()});
    }
    l.medium = d.font(d.user, Utf8(*dir / "font-medium.otf").c_str());
    if (l.medium == 0) {
        l.medium = l.regular;
    }
    std::string missing = l.regular == 0 ? "font-regular.otf" : "";
    for (int q = 0; q < 5; ++q) {
        const std::string name = "heart-" + std::to_string(q) + ".png";
        const auto img = CoopPng::Load(*dir / name);
        l.hearts[q] = img ? d.image(d.user, img->width, img->height, img->rgba.data()) : 0;
        if (l.hearts[q] == 0 && missing.empty()) {
            missing = name;
        }
    }
    if (!l.On()) {
        LOG_WARNING(Core, "coop: hud: {} in {} could not be loaded; plain look", missing, dir->string());
        g_look_state = kLookOff;
        return;
    }
    // The game's map arrow, which Hookshot turns to each player's facing (optional: a pack made before it has none,
    // a Hookshot without draw_image_turned cannot turn it; the maps then show names only, said once here).
    std::string arrow_why;
    if (d.draw_image_turned == nullptr) {
        arrow_why = "this Hookshot cannot turn pictures (no hos_coop_draw3: build it again with our patch)";
    } else {
        const auto rim = CoopPng::Load(*dir / "map-arrow.png");
        const auto fill = CoopPng::Load(*dir / "map-arrow-fill.png");
        if (!rim || !fill) {
            arrow_why = std::string(!rim ? "map-arrow.png" : "map-arrow-fill.png") + " missing in the pack";
        } else {
            l.map_arrow = d.image(d.user, rim->width, rim->height, rim->rgba.data());
            l.map_arrow_fill = d.image(d.user, fill->width, fill->height, fill->rgba.data());
            if (!l.MapArrows()) {   // (Hookshot keeps at most 64 pictures: coop_draw.h Look::kPictures)
                arrow_why = "map-arrow pictures not taken by Hookshot";
                l.map_arrow = l.map_arrow_fill = 0;
            }
        }
    }
    if (l.MapArrows()) {
        LOG_INFO(Core, "coop: hud: game look on Hookshot: the game font, heart pictures and map arrows loaded");
    } else {
        LOG_INFO(Core, "coop: hud: game look on Hookshot: the game font and heart pictures loaded; no map arrows ({}): "
                       "names only on the maps", arrow_why);
    }
    g_look = l;
    g_look_state = kLookOn;
}

// The game look may be used: not known to be off, and the pack is there.
bool LookPossible() {
    return g_look_state.load() != kLookOff && CoopHud::Folder().has_value();
}

std::vector<CoopCheatsPanel::Row> CheatRows() {
    static const std::vector<std::string> items = CoopCheats::Items();
    return CoopCheatsPanel::Rows(g_cheats.filter(), items);
}

// The flag Enter or a click chose, given (under g_box_mutex).
void GiveChosen(const std::vector<CoopCheatsPanel::Row>& rows) {
    if (const auto i = g_cheats.TakeGive(); i && *i < rows.size()) {
        CoopCheats::Give(rows[*i].flag);
        LOG_INFO(Core, "coop: cheats: give {}", rows[*i].flag);
    }
}

// The picture's size at the last draw: where the mouse is on it.
std::atomic<float> g_drawn_w{0.0f};
std::atomic<float> g_drawn_h{0.0f};

bool AsyncDown(int vk) {
#ifdef _WIN32
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    return HasKeyDown(g_api) && g_api->key_down(static_cast<std::uint32_t>(vk)) != 0;
#endif
}

// A WM_CHAR byte of the ANSI window as Unicode; 0 while a lead byte waits.
// On Linux the window hands code points already (SDL's typed text).
std::uint32_t FromCodePage(unsigned byte) {
#ifdef _WIN32
    char b[2];
    int n = 0;
    if (g_box_lead != 0) {
        b[n++] = static_cast<char>(g_box_lead);
        g_box_lead = 0;
    } else if (byte >= 0x80 && IsDBCSLeadByte(static_cast<BYTE>(byte))) {
        g_box_lead = byte;
        return 0;
    }
    b[n++] = static_cast<char>(byte);
    wchar_t w[2] = {};
    if (MultiByteToWideChar(CP_ACP, 0, b, n, w, 2) != 1) {
        return 0xFFFD;
    }
    return w[0];
#else
    return byte <= 0x10FFFF && (byte < 0xD800 || byte > 0xDFFF) ? byte : 0xFFFD;
#endif
}

// The players on the map (SetMapOptions): on unless the settings say off
// (0, no, off, false: as coop-options.ini is read, CoopTitleMenu::Options).
bool EnvOff(const char* name) {
    std::string v = Env(name);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return v == "0" || v == "no" || v == "off" || v == "false";
}
std::atomic<bool> g_map_players{!EnvOff("LICHII_COOP_MAP")};
std::atomic<bool> g_map_names{!EnvOff("LICHII_COOP_MAP_NAMES")};

// The players on the maps (CoopDraw::MapShown), or null: the setting is
// off, our place is not known or has no map (coop_maps.txt), nobody else is
// there, or our own cutscene runs (the game hides its HUD). The minimap needs
// the game's camera (we play), no game menu in front (patch 0018) and the
// game's own minimap panes placed (CoopMapPage::Minimap: where the game draws
// no minimap, nothing); the Map page's picture is CoopMapPage's. With
// LICHII_COOP_MAP_SELF=1 (tests) our own Link is marked too, as player 0, to
// hold against the game's own arrow. The present's thread only: the view, the
// players and their names keep their room between presents.
const CoopDraw::MapShown* MapScene(bool camera, std::uint8_t self) {
    static CoopHook::MapView view;
    static CoopDraw::MapShown m;
    static std::optional<int> page_open_floor;   // the game's floor when the Map page came up
    static const bool mark_self = Env("LICHII_COOP_MAP_SELF") == "1";
    if (!g_map_players.load() || !CoopHook::CurrentMapView(view) || view.event || (view.others.empty() && !mark_self)) {
        page_open_floor.reset();
        return nullptr;
    }
    const auto* capture = CoopMaps::Maps().find(view.scene);
    if (capture == nullptr) {
        page_open_floor.reset();
        return nullptr;
    }
    const auto menu = CoopMapPage::MenuState();
    // (tabs-overhaul-01110 (c): not over our - tabs, where the - menu still counts the Map as its page)
    const auto page = CoopPauseTab::OwnMinusPageShown() ? std::nullopt : CoopMapPage::Shown(capture->dungeon);
    m.minimap.reset();
    // (row 199: also while the pause menu's tab strip is up, any page: patch 0018's menu_open is a screen that had the
    // mouse, which a mod's page such as Character Select's never has, so the marks drew over it. The Map page: m.page)
    if (camera && !(menu && menu->menu_open) && !CoopPauseTab::GameMenuUp()) {
        m.minimap = CoopMapPage::Minimap(capture->dungeon);
    }
    m.minimap_floor = coop::overlay::map::floor_of(*capture, view.self[1]);
    m.page.reset();
    m.page_floor = coop::overlay::map::floor_of(*capture, view.self[1]);
    if (page) {
        m.page = page->box;
        // The page opens on our floor; each floor up (the game's count) is one less in floor_of's.
        if (page->floors > 0) {
            if (!page_open_floor) page_open_floor = page->floor;
            m.page_floor += *page_open_floor - page->floor;
        }
    } else {
        page_open_floor.reset();
    }
    if (!m.minimap && !m.page) {
        return nullptr;
    }
    m.scene.assign(view.scene);
    m.self = view.self;
    m.capture = capture;
    const bool names = g_map_names.load();
    m.players.resize(view.others.size() + (mark_self ? 1 : 0));
    m.names.resize(names ? m.players.size() : 0);
    for (std::size_t i = 0; i < view.others.size(); ++i) {
        const auto& o = view.others[i];
        auto& p = m.players[i];
        p.number = o.number;
        p.position = o.position;
        p.rotation = o.rotation;
        if (o.scene) {
            if (!p.scene) p.scene.emplace();
            p.scene->assign(*o.scene);
        } else {
            p.scene.reset();
        }
        p.fade = o.fade;
        const std::uint32_t c = CoopDraw::Lifted(CoopPuppet::NameColor(o.number, self));
        p.rgb = (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF);  // (IM_COL32 to 0xRRGGBB)
        if (names) {
            if (o.name.empty()) {
                m.names[i] = "P" + std::to_string(o.number);
            } else {
                m.names[i].assign(o.name);
            }
        }
    }
    if (mark_self) {  // (white, no facing: a dot where we place our own Link)
        auto& p = m.players.back();
        p.number = 0;
        p.position = view.self;
        p.rotation.reset();
        p.scene.emplace(view.scene);
        p.fade = 1.0f;
        p.rgb = 0xFFFFFF;
        if (names) m.names.back() = "self";
        static auto next_log = std::chrono::steady_clock::time_point{};
        const auto now = std::chrono::steady_clock::now();
        if (m.minimap && now >= next_log) {  // (where our code puts our own Link, in the layout's units)
            next_log = now + std::chrono::seconds(5);
            static std::vector<coop::overlay::map::Icon> one;
            coop::overlay::map::minimap_icons(*m.minimap, *capture, m.minimap_floor, {p}, one);
            LOG_INFO(Core, "coop: map: self in {} at {:.1f} {:.1f} {:.1f}: on the minimap {} (alpha {:.2f})", view.scene,
                     p.position[0], p.position[1], p.position[2],
                     one.empty() ? std::string("nowhere") : fmt::format("{:.0f} {:.0f}{}", one[0].x, one[0].y,
                                                                         one[0].clamped ? " (held at the edge)" : ""),
                     m.minimap->alpha);
        }
    }
    return &m;
}

// ---- Chat as the game's own UI (coop_native_ui.h, docs/native-ui.md), on the game's thread at the frame hook: the
// chat lines, notices, the typed line and the held line (down but not out) all on the game's banner (SimpleInfo_00,
// its frame and font). Nothing of it is drawn. LICHII_COOP_NATIVE_UI=0, or a game that gives no memory for
// the texts: the drawn lines and box of before (text only, no box behind them).
class NativeGame final : public CoopNativeUi::Game {
public:
    explicit NativeGame(void* ctx) : ctx_(ctx) {}
    bool Read(std::uint64_t a, void* out, std::size_t n) override { return CoopRuntime::Read(a, out, n); }
    bool Write(std::uint64_t a, const void* in, std::size_t n) override { return CoopRuntime::Write(a, in, n); }
    std::optional<std::uint64_t> Call(std::uint64_t f, const std::array<std::uint64_t, 5>& x) override {
        HosCoopCall k{};
        k.function = f;
        for (std::size_t i = 0; i < x.size(); ++i) k.x[i] = x[i];
        std::uint64_t x0 = 0;
        if (g_api == nullptr || g_api->call(ctx_, &k, &x0) == 0) return std::nullopt;
        return x0;
    }
    std::uint64_t Main() const override { return CoopRuntime::MainBase(); }

private:
    void* ctx_;
};

struct Native {
    enum class Block { kNone, kFailed, kHave } block = Block::kNone;
    CoopNativeUi::TextRing ring;
    CoopNativeUi::ChatFeed feed;
    CoopNativeUi::Banner banner;
    bool typing = false;          // the typed line is on the game's banner
    std::string typed;            // what it shows
    std::uint64_t typed_ms = 0;
    std::uint64_t failed_ms = 0;  // the banner was not free: tried again a second later
    bool said_wait = false;
};
Native g_native;                             // the game's thread only
std::atomic<bool> g_native_chat{false};      // the game's windows carry the chat: nothing of it drawn (DrawScene)

bool NativeOn() {
    static const bool on = Env("LICHII_COOP_NATIVE_UI") != "0";
    return on;
}

// The other players' hearts on the game's own HUD (coop_party_hud.h). DrawScene (the present's thread) makes the
// blocks; NativeStep (the game's thread) sets the game's heart parts from them. While the game's HUD shows them,
// DrawScene draws no blocks of its own, so nothing shows twice.
class PartyGame final : public CoopPartyHud::Game {
public:
    PartyGame(NativeGame& game, void* ctx) : game_(game), ctx_(ctx) {}
    bool Read(std::uint64_t a, void* out, std::size_t n) override { return game_.Read(a, out, n); }
    bool Write(std::uint64_t a, const void* in, std::size_t n) override { return game_.Write(a, in, n); }
    std::optional<std::uint64_t> Call(std::uint64_t f, const std::array<std::uint64_t, 5>& x) override {
        return game_.Call(f, x);
    }
    std::uint64_t Main() const override { return game_.Main(); }
    std::optional<std::uint64_t> CallFloat(std::uint64_t f, std::uint64_t x0, float s0) override {
        HosCoopCall k{};
        k.function = f;
        k.x[0] = x0;
        k.s0 = s0;
        std::uint64_t r = 0;
        if (g_api == nullptr || g_api->call(ctx_, &k, &r) == 0) return std::nullopt;
        return r;
    }

private:
    NativeGame& game_;
    void* ctx_;
};

struct PartyShared {
    std::mutex mutex;
    std::vector<CoopPartyHud::Block> blocks;   // DrawScene's last
    std::optional<double> game_hud_bottom;
    std::string own;                           // our own line ("You" + voice words), "" none
    bool game_menu_up = false;
    std::uint64_t ms = 0;                      // when DrawScene left them (SteadyMs)
    std::uint64_t present = 0;                 // and at which present (CoopFresh::Presents)
};
PartyShared g_party;
std::atomic<bool> g_party_native{false};     // the game's HUD showed the blocks last frame (Hud::Step true)

bool PartyTest() {
    static const bool on = EnvOn("COOP_TEST_PARTY_HUD");   // scenario hs-party-hud: four made-up players
    return on;
}

// party-hud-crash-c126 probe (LICHII_COOP_PARTY_HUD_PROBE=1, off by default): which game field says a listed screen
// really runs. Reads only, no game call: each frame, for every screen of the UI manager's list with one of the vtables
// co-op calls into (the status screen with the hearts, the banner, the balloons, the HUD's map screen) its state byte
// (+0x1a6), its request (+0x1a5), its layout (+0x28) and the layout's vtable, and whether patch 0018 saw its update in
// the last 100 ms (Menu::Runs); with the scene-load flag and what the gated helpers hand out (ScreenByVtable +
// ScreenLayout: what Hud::Step would Find on). One line each time any of it changes (at most 800). While it is on, the
// party HUD makes no game call at all (PartyStep returns after it), so a run goes boot -> title -> load -> play ->
// title -> load without the crash and the log shows the fields at each step.
bool PartyProbe(NativeGame& game) {
    static const bool on = Env("LICHII_COOP_PARTY_HUD_PROBE") == "1";
    if (!on) return false;
    static int lines = 0;
    static std::string was;
    static std::uint64_t frame = 0, gated_was = 0;
    ++frame;
    if (lines >= 800) return true;
    const std::uint64_t main = CoopRuntime::MainBase();
    const auto read64 = [&](std::uint64_t a) {
        std::uint64_t v = 0;
        return game.Read(a, &v, sizeof v) ? v : 0ull;
    };
    const auto off = [&](std::uint64_t v) {
        char b[24];
        if (v >= main && v - main < 0x8000000) {
            std::snprintf(b, sizeof b, "m+%llx", static_cast<unsigned long long>(v - main));
        } else {
            std::snprintf(b, sizeof b, "%llx", static_cast<unsigned long long>(v));
        }
        return std::string(b);
    };
    const auto menu = CoopMapPage::MenuState();
    struct Kind { const char* name; std::uint64_t vt; };
    static constexpr Kind kKinds[] = {{"status", CoopPartyHud::addr::kStatusVtable},
                                      {"banner", CoopNativeUi::addr::kInfoVtable},
                                      {"balloons", CoopNativeTags::addr::kBalloonVtable},
                                      {"mapmenu", CoopNativeTags::kMapMenuVtable}};
    std::string now = std::string("loading=") + (CoopNativeUi::SceneLoading() ? "1" : "0");
    if (menu) {
        now += std::string(" 0018: known=") + (menu->screens_known ? "1" : "0") + " running=" +
               std::to_string(menu->screen_count) + " hud=" + off(menu->hud_screen) + " hud_runs=" +
               (menu->HudRuns() ? "1" : "0") + " menu_open=" + (menu->menu_open ? "1" : "0");
    } else {
        now += " 0018: none";
    }
    const std::uint64_t got = read64(main + CoopNativeUi::addr::kUiManagerGot);
    const std::uint64_t manager = got != 0 ? read64(got) : 0;
    std::uint32_t count = 0;
    const std::uint64_t screens = manager != 0 ? read64(manager + CoopNativeUi::addr::kUiScreens) : 0;
    if (manager == 0 || !game.Read(manager + CoopNativeUi::addr::kUiCount, &count, sizeof count) || screens == 0) {
        now += " ui: no list";
        count = 0;
    } else {
        now += " ui: " + std::to_string(count);
    }
    for (std::uint32_t i = 0; i < count && i < 512; ++i) {
        const std::uint64_t s = read64(screens + i * 8ull);
        const std::uint64_t vt = s != 0 ? read64(s) : 0;
        for (const Kind& k : kKinds) {
            if (vt == 0 || vt != main + k.vt) continue;
            std::uint8_t state = 0xff;
            std::int8_t request = -128;
            game.Read(s + CoopNativeUi::addr::kScreenState, &state, 1);
            game.Read(s + CoopNativeUi::addr::kScreenRequest, &request, 1);
            const std::uint64_t layout = read64(s + CoopNativeUi::addr::kScreenLayout);
            const std::uint64_t layout_vt = layout != 0 ? read64(layout) : 0;
            now += std::string(" | ") + k.name + "#" + std::to_string(i) + " " + off(s) + " state=" +
                   std::to_string(state) + " req=" + std::to_string(request) + " layout=" + off(layout) + " lvt=" +
                   off(layout_vt) + " runs=" + (menu && menu->screens_known ? (menu->Runs(s) ? "1" : "0") : "?");
        }
    }
    const std::uint64_t gated_screen = CoopNativeUi::ScreenByVtable(game, CoopPartyHud::addr::kStatusVtable);
    const std::uint64_t gated = CoopNativeUi::ScreenLayout(game, gated_screen);
    now += " | gated status=" + off(gated_screen) + " layout=" + off(gated);
    const bool would_find = gated != 0 && gated != gated_was;
    gated_was = gated;
    if (now == was && !would_find) return true;
    was = now;
    ++lines;
    LOG_INFO(Core, "coop: party hud probe: f{} {}{}", frame, now, would_find ? " | WOULD FIND (Hud::Step: a new layout)" : "");
    return true;
}

void PartyStep(NativeGame& game, void* ctx, std::uint64_t now) {
    if (PartyProbe(game)) return;   // (the probe: logs only, the HUD makes no game call)
    static CoopPartyHud::Hud hud;
    std::vector<CoopPartyHud::Block> blocks;
    std::optional<double> bottom;
    std::string own;
    bool menu_up = false;
    {
        std::lock_guard lock(g_party.mutex);
        // Older than a second AND presents went on without DrawScene = none (coop_fresh.h). By the wall clock alone a
        // game at a frame a second or slower (the bridge stepping frames, a long hitch) found every frame's blocks
        // stale and hid the other players (testkit-0120 kit-in-play shots, 1.5 s a frame).
        if (!CoopFresh::Stale(now > g_party.ms ? now - g_party.ms : 0, 1000, g_party.present, CoopFresh::Presents())) {
            blocks = g_party.blocks, own = g_party.own;
        }
        bottom = g_party.game_hud_bottom;
        menu_up = g_party.game_menu_up;
    }
    if (PartyTest() && !menu_up) blocks = CoopPartyHud::TestBlocks(now), own = CoopPartyHud::TestOwn();
    // Double defence is one for all players (a shared flag): ours is theirs. Race mode shares no flag and no message
    // carries theirs, so it is off on their hearts then.
    if (!blocks.empty() && !CoopRaceMode::InForce()) {
        CoopHook::EmulatorMemory memory(g_system.ApplicationMemory(), CoopRuntime::MainBase());
        const bool defence = coop::host::GameFlags(memory).get_bool(CoopPartyHud::kDefenceUpHash).value_or(false);
        for (auto& b : blocks) b.double_defence = defence;
    }
    PartyGame party(game, ctx);
    static const bool no_pack = EnvOn("COOP_TEST_PARTY_HUD_NO_PACK");   // scenario hs-party-hud-probe: as without a pack
    hud.IgnorePack(no_pack);
    std::vector<std::string> log;
    const bool on = hud.Step(party, blocks, bottom, &log, now, own);
    static std::uint64_t last_step_ms = 0;
    NoteSlow(g_party_slow, last_step_ms, now, on ? static_cast<int>(blocks.size()) : 0);
    for (const auto& l : log) LOG_INFO(Core, "{}", l);
    if (g_party_native.exchange(on) != on) {
        LOG_INFO(Core, "coop: party hud: {}", on ? "on the game's HUD" : "hidden (the game's HUD has none of ours now; nothing drawn instead)");
    }
}

using Colours = std::map<std::string, std::uint32_t, std::less<>>;

// The speakers' colours by the names chat lines call them (the HUD's and tags' colours: CoopPuppet::NameColor).
Colours SpeakerColours() {
    Colours out;
    const auto rows = CoopHook::PlayerList();
    std::uint8_t self = 0;
    for (const auto& row : rows) {
        if (row.self) self = row.number;
    }
    for (const auto& row : rows) {
        if (row.number == 0) continue;
        const std::uint32_t c = CoopPuppet::NameColor(row.number, self) & 0xFFFFFF;
        if (!row.name.empty()) out[row.name] = c;
        out["P" + std::to_string(row.number)] = c;
        if (row.self) out["you"] = c;
    }
    return out;
}

// The chat box open: the typed line on the game's banner ("you: hello_"), changing as it is typed. Lines that come
// meanwhile wait (the banner is the box's) and show once it closes. True while the box is open.
bool NativeTyping(CoopNativeUi::Game& game, std::uint64_t now, const Colours& colours) {
    std::optional<std::pair<std::string, bool>> box;
    {
        std::lock_guard lock(g_box_mutex);
        if (g_box.open()) box.emplace(g_box.text(), g_box.failed());
    }
    if (!box) {
        if (g_native.typing) {
            g_native.banner.End(game, g_native.ring);
            g_native.typing = false;
        }
        g_native.typed.clear();
        return false;
    }
    auto line = CoopNativeUi::TypingLine(CoopI18n::Tr("you"), box->first, box->second, CoopI18n::Tr("(not sent)"));
    if (const auto c = colours.find("you"); c != colours.end() && !box->second) line.colour = c->second;
    // (a failed send keeps TypingLine's kFailedColour: the line plainly did not go)
    // Again on a change, and before its time runs out (kTypingSeconds); while the banner shows the game's own (or the
    // held line) the typed line waits for it, the keys still type. Nothing is drawn.
    const bool due = !g_native.typing || !g_native.banner.showing() || line.text != g_native.typed ||
                     now > g_native.typed_ms + 30000;
    if (due && now >= g_native.failed_ms + 250 &&
        g_native.banner.Show(game, g_native.ring, line, now)) {
        static bool said = false;
        if (!std::exchange(said, true)) LOG_INFO(Core, "coop: native ui: typing on the game's banner");
        g_native.typing = true;
        g_native.typed = line.text;
        g_native.typed_ms = now;
    } else if (due && !g_native.banner.showing()) {
        g_native.typing = false;
        g_native.failed_ms = now;   // (the game's own banner, or the held line: tried again shortly)
    }
    return true;
}

void NativeStep(void* ctx) {
    if (!NativeOn()) return;
    NativeGame game(ctx);
    const std::uint64_t now = SteadyMs();
    PartyStep(game, ctx, now);   // (its own memory: on even when the chat's texts got none)
    if (g_native.block == Native::Block::kFailed) return;
    if (g_native.block == Native::Block::kNone) {
        const auto block =
            game.Call(game.At(CoopNativeUi::addr::kMalloc), {CoopNativeUi::TextRing::kBlockBytes + 16, 0, 0, 0, 0});
        if (!block || *block == 0) {
            g_native.block = Native::Block::kFailed;
            LOG_WARNING(Core, "coop: native ui: the game gave no memory for its texts; chat drawn as before");
            return;
        }
        g_native.ring.SetBlock((*block + 15) & ~std::uint64_t{15});
        g_native.block = Native::Block::kHave;
        // The game's own font (the HUD pack's font-medium.otf) measures every banner line (CoopNativeUi::Width);
        // without the pack the widths are the safe guesses.
        if (const auto dir = CoopHud::Folder()) {
            std::ifstream f(*dir / "font-medium.otf", std::ios::binary);
            const std::vector<std::uint8_t> font{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
            if (CoopNativeUi::SetFontWidths(font)) {
                LOG_INFO(Core, "coop: native ui: banner lines measured with the game's font ({} bytes)", font.size());
            } else {
                LOG_WARNING(Core, "coop: native ui: font-medium.otf not read; banner lines measured by safe guesses");
            }
        }
        g_native_chat = true;
        LOG_INFO(Core, "coop: native ui: chat on the game's banner (SimpleInfo_00; texts at {:#x})", *block);
    }
    const auto lines = CoopHook::ChatLines();
    const Colours colours = SpeakerColours();
    std::vector<CoopNativeUi::ChatIn> in;
    {  // row 280 (coop_part_fallback.h): a mod part with files left out (the game's own used) says so on the banner,
       // once each time play starts from the title, solo too. Its arrival time is fixed, so the feed takes it once;
       // the feed's first Update (primed) has been, so it is not swallowed as a line from before.
        static CoopPartFallback::OncePerLoad once;
        static std::uint64_t came = 0;
        static bool fed = false;
        static std::optional<std::vector<std::string>> part;
        if (!part && CoopModsRuntime::Loader() != nullptr) part = CoopModsRuntime::PartFallbackLines();
        if (fed && part && !part->empty()) {
            if (once.Step(CoopSave::TitleUp(), CoopObjects::OurLinkActor().has_value())) came = now;
            if (came != 0 && now - came < CoopNativeUi::ChatFeed::kStaleMs) {
                for (const std::string& t : *part) in.push_back({t, true, now - came, 0});
            }
        }
        fed = true;
    }
    for (const auto& l : lines) {
        // (a notice is the module's own words: in the player's language; chat is as typed)
        CoopNativeUi::ChatIn c{l.notice ? CoopI18n::Tr(l.text) : l.text, l.notice, l.age_ms, 0};
        if (const auto at = l.text.find(": "); !l.notice && at != std::string::npos) {
            if (const auto it = colours.find(std::string_view(l.text).substr(0, at)); it != colours.end()) {
                c.colour = it->second;
            }
        }
        in.push_back(std::move(c));
    }
    g_native.feed.Update(in, now);
    // The held line (down but not out) keeps the banner; a page of ours whose time is over closes. Pages wait
    // meanwhile.
    struct LogBanner {   // typing-blink-0121: the banner's changes (CoopNativeUi::TakeBannerLines), however we leave
        ~LogBanner() {
            for (const std::string& l : CoopNativeUi::TakeBannerLines()) LOG_INFO(Core, "{}", l);
        }
    } log_banner;
    if (g_native.banner.Step(game, g_native.ring, CoopNativeUi::HeldBanner(), now)) return;
    if (NativeTyping(game, now, colours)) return;
    if (g_native.feed.Busy(now) || now < g_native.failed_ms + 1000) return;
    std::optional<CoopNativeUi::Shown> line = g_native.feed.Next(now);
    if (!line) return;
    if (g_native.banner.Show(game, g_native.ring, *line, now)) {
        static bool said = false;
        if (!std::exchange(said, true)) {
            LOG_INFO(Core, "coop: native ui: first {} shown on the game's banner", line->notice ? "notice" : "chat line");
        }
        return;
    }
    // No banner now (the title, a load) or one of the game's own on it: tried again in a second, until the line is
    // stale (ChatFeed::kStaleMs). Nothing drawn meanwhile.
    g_native.feed.PutBack(*line);
    g_native.failed_ms = now;
    if (!std::exchange(g_native.said_wait, true)) {
        LOG_INFO(Core, "coop: native ui: the game's banner is not free now; the line waits");
    }
}

}  // namespace

SlowStep PartySlow() {
    std::lock_guard lock(g_slow_mutex);
    return g_party_slow;
}

SlowStep TagsSlow() {
    std::lock_guard lock(g_slow_mutex);
    return g_tags_slow;
}

TagsNow TagsSeen() {
    std::lock_guard lock(g_slow_mutex);
    return g_tags_now;
}

void SetMapOptions(bool players, bool names) {
    g_map_players.store(players);
    g_map_names.store(names);
}

CoopDraw::Scene DrawScene() {
    CoopDraw::Scene s;
    s.title = CoopTitle::View();
    {
        std::lock_guard lock(g_box_mutex);
        if (g_box.open() && !g_native_chat.load()) {  // (the game's message window shows it: NativeTyping)
            s.box = CoopDraw::ChatBoxView{g_box.text(), g_box.failed()};
        }
        if (CoopDebugPanels::kOn && g_cheats.open()) {   // (release kits: no F8 panel, coop_debug_panels.h)
            CoopDraw::CheatsView v;
            v.filter = g_cheats.filter();
            for (const auto& r : CheatRows()) {
                v.rows.push_back(r.label);
            }
            v.selected = g_cheats.selected();
            v.result = CoopCheats::LastResult();
            s.cheats = std::move(v);
        }
    }
    if (!g_native_chat.load()) {  // (the game's message window and banner show them: NativeStep)
        s.chat = CoopDraw::ShownChat(CoopHook::ChatLines(), s.box.has_value());
    }
    s.markers = CoopHook::CurrentMarkers();
    if (s.markers && !g_tags_drawn.load()) {   // the game's balloons carry the name tags (NativeTags): none drawn
        std::erase_if(s.markers->items, [](const CoopHook::Marker& m) { return !m.self; });
    }
    s.dbno = CoopDbno::Shown();  // down but not out: one line (coop_dbno_draw.h)
    // With the game's own UI up (coop_native_ui.h: the banner is found and works), the line is held on the game's
    // banner (SimpleInfo_00) and nothing is drawn; let go when down ends. Without it (LICHII_COOP_NATIVE_UI=0, a game
    // that gave no memory): the drawn line, text only.
    // The banner's line always fits it with the clock / share / "A: next player" kept (BannerLine); spectating holds it
    // only a few seconds after each change of whom we watch, so notices are not kept waiting for minutes (BannerHold).
    if (g_native_chat.load()) {
        static std::mutex hold_mutex;
        static CoopDbnoDraw::BannerHold hold;
        std::lock_guard hold_lock(hold_mutex);   // (DrawScene runs on the present's thread and from draw_wanted)
        const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           std::chrono::steady_clock::now().time_since_epoch())
                                                           .count());
        if (s.dbno && hold.Want(*s.dbno, now_ms)) {
            CoopNativeUi::HoldBanner(CoopDbnoDraw::BannerLine(*s.dbno, CoopNativeUi::kBannerWidth));
        } else if (CoopNativeUi::HeldBanner()) {
            CoopNativeUi::EndBanner();
        }
        if (!s.dbno) {
            hold.Reset();
        }
        s.dbno.reset();   // (nothing drawn: the game's banner has it, or had it and let notices go on)
    } else if (CoopNativeUi::HeldBanner()) {
        CoopNativeUi::EndBanner();   // (the native UI went: the drawn line takes over)
    }
    bool list = false, panel = false, list_press = false, always = false;
    {
        std::lock_guard lock(g_keys_mutex);
        static bool panel_env = [] {
            g_keys.panel = CoopDebugPanels::kOn && EnvOn("LICHII_COOP_DEBUG_PANEL");
            return true;
        }();
        (void)panel_env;
        g_keys.Update(KeyDown(0x75 /* F6 */), KeyDown(0x76 /* F7 */));
        static const bool always_env = EnvOn("LICHII_COOP_SHOW_PLAYERS");  // tests: no F6 to hold
        always = always_env;
        list = g_keys.list || always;
        list_press = g_keys.list_press && !always;
        panel = CoopDebugPanels::kOn && g_keys.panel;   // (release kits: no F7 panel, coop_debug_panels.h)
    }
    auto rows = CoopHook::PlayerList();
    // The HUD blocks, players' colours and place names are the game look's
    // (as the co-op window: LICHII_COOP_HUD=0 or no pack, the plain look).
    const bool game = LookPossible();
    if (game) {
        std::uint8_t self = 0;
        for (const auto& row : rows) {
            if (row.self) {
                self = row.number;
            }
        }
        for (std::uint8_t n = 1; n <= 4; ++n) {  // a tag with no name
            s.colours["P" + std::to_string(n)] = CoopDraw::Lifted(CoopPuppet::NameColor(n, self));
        }
        if (self != 0) {
            s.colours["you"] = CoopDraw::Lifted(CoopPuppet::NameColor(self, self));
        }
        for (const auto& row : rows) {
            if (row.number != 0 && !row.name.empty()) {
                s.colours[row.name] = CoopDraw::Lifted(CoopPuppet::NameColor(row.number, self));
            }
        }
    }
    // The voice states after the names, as words (docs/voice.md, "Who is talking"): only while voice chat is on here.
    const bool voice_on = game && CoopVoice::Available() && CoopVoice::Get().mode != coop::voice::Mode::off;
    const CoopLobby::Seen seen = voice_on ? CoopLobby::Now() : CoopLobby::Seen{};
    const auto voice_muted = [&](std::uint8_t number) {
        for (const auto& p : seen.players) {
            if (p.number == number) {
                return ((p.host_mutes | (p.self ? 0 : p.own_mutes)) & coop::protocol::lobby_mute::voice) != 0;
            }
        }
        return false;
    };
    // Each block's dim look held steady (coop_hud_dim.h): no flashing while a slow game's samples come a second apart.
    static std::mutex dim_mutex;
    static CoopHudDim::Hold dim_hold;
    std::lock_guard dim_lock(dim_mutex);
    const auto dim_now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    for (std::uint8_t n = 1; n <= CoopHudDim::Hold::kSlots; ++n) {
        if (std::none_of(rows.begin(), rows.end(), [n](const CoopHook::PlayerRow& r) { return r.number == n && !r.self; })) {
            dim_hold.Forget(n);
        }
    }
    // (the game's pause menu up: the other players' blocks wait; drawn over the pause menu's left rows they hid our
    // Multiplayer page's, a test run 2026-11-20)
    CoopPauseTab::CensusFromMenuState();   // (the - menu research: only with COOP_TEST_SCREEN_PROBE=1)
    const auto menu_state = CoopMapPage::MenuState();
    const bool game_menu_up = (menu_state && menu_state->menu_open) || CoopPauseTab::GameMenuUp();   // (row 199)
    // The blocks for the game's own heart parts (PartyStep): per player the line is the name, voice words and place.
    // Never drawn by us (owner: nothing hand-drawn); with an older pack that has none they are hidden (PartyStep logs why).
    std::vector<CoopPartyHud::Block> party;
    std::string own;
    for (const auto& row : game ? rows : std::vector<CoopHook::PlayerRow>{}) {
        if (row.self) {   // our own line over the blocks: "You" and our voice state
            const auto icon = CoopVoice::IconFor(voice_on, CoopVoice::Get().muted || voice_muted(row.number),
                                                 CoopVoice::Talking(), true);
            if (icon != CoopVoice::Icon::none) own = CoopI18n::Tr("You") + CoopDraw::VoiceWords(icon);
            continue;
        }
        if (game_menu_up) continue;
        CoopPartyHud::Block b;
        const auto voice = CoopVoice::IconFor(voice_on, voice_muted(row.number), CoopVoice::Speaking(row.number), false);
        b.text = (row.name.empty() ? "Player " + std::to_string(row.number) : row.name) + CoopDraw::VoiceWords(voice);
        if (row.scene && !row.scene->empty()) {
            // The place names are the game's own (places.json): the table has them
            // for the languages it carries, and a name it does not know stays as it is.
            const std::string place = CoopI18n::Place(CoopHud::PlaceName(*row.scene));
            if (!place.empty()) b.text += "  " + place;
        }
        if (const auto life = CoopHud::PlayerLife(row.number); life && life->max > 0) {
            b.hearts = CoopHudHearts::Quarters(life->life, life->max);
        }
        b.dim = dim_hold.Dim(row.number, row.state == "connected", dim_now);
        party.push_back(std::move(b));
    }
    {   // row 141: the blocks go below the game's own HUD as it is laid out now
        const std::optional<double> bottom =
            !party.empty() || PartyTest() ? CoopMapPage::GameHudBottom() : std::optional<double>{};
        std::lock_guard lock(g_party.mutex);
        g_party.blocks = std::move(party);
        g_party.own = std::move(own);
        g_party.game_hud_bottom = bottom;
        g_party.game_menu_up = game_menu_up;
        g_party.ms = SteadyMs();
        g_party.present = CoopFresh::Presents();
    }
    // F6 (ui-audit-01112 B5): the game's own - menu, Multiplayer > Players (coop_players_open.h), when we are in a
    // scene (never on the title or a file screen, where Minus is not the - menu); the drawn list only when the pack has
    // no - tabs of ours (OpenPlayers false) or for tests (LICHII_COOP_SHOW_PLAYERS).
    static std::atomic<bool> native_list{true};
    {   // Free play for F6's Minus, each frame: our Link's frame is fresh (none in a load or on the title) and no event
        // (cutscene, talk) has control (CoopHook::CurrentMapView); the pause tab also reads the event manager itself.
        thread_local CoopHook::MapView view;
        const bool title = CoopTitle::Up();
        const bool fresh = !title && CoopHook::CurrentMapView(view);
        const bool free_play = fresh && !view.event;
        CoopPauseTab::SetFreePlay(free_play);
        // Each change, for tests (tools/autotest/hs_steps.py `state`, f1-event-c31): what F1 and the - menu go by.
        const char* why = free_play ? "yes" : title ? "no (title)" : fresh ? "no (event)" : "no (load, no Link)";
        thread_local const char* told = nullptr;
        if (told != why) {
            told = why;
            LOG_INFO(Core, "coop: free play: {}", why);
        }
    }
    if (list_press) {
        const bool in_scene = !CoopTitle::Up() && std::any_of(rows.begin(), rows.end(), [](const CoopHook::PlayerRow& r) {
            return r.self && r.scene && !r.scene->empty();
        });
        native_list = in_scene && CoopPauseTab::OpenPlayers();
    }
    if (list && (always || !native_list.load())) {
        if (game) {
            for (auto& row : rows) {
                if (row.scene && !row.scene->empty()) {
                    row.scene = CoopI18n::Place(CoopHud::PlaceName(*row.scene));
                }
            }
        }
        s.players = rows;
    }
    if (panel) {
        s.panel = CoopHook::OverlayLines();
    }
    {
        std::uint8_t self = 0;
        for (const auto& row : rows) {
            self = row.self ? row.number : self;
        }
        s.map = MapScene(s.markers.has_value(), self);
        CoopMapIcons::Publish(s.map ? CoopDraw::MapMarks(*s.map) : CoopMapIcons::Frame{});
        if (g_map_native.load()) s.map = nullptr;   // (the game's own icons show them: nothing drawn)
    }
    return s;
}

bool TypeKey(std::uint32_t vk, bool repeat) {
    // The pause menu's Multiplayer page's field, while it is typed into: every key is its own, the ones it does not
    // use too (a letter comes again as its character, TypeChar), so none opens the chat box or the cheats panel.
    if (CoopPauseTab::Key(vk, repeat) || CoopPauseTab::KeysTaken()) {
        return true;
    }
    if (CoopPause::Key(vk, repeat)) {   // the Multiplayer tab's field, while it is typed into
        return true;
    }
    if (!g_on) {
        return false;
    }
    std::lock_guard lock(g_box_mutex);
    // The cheats panel (F8, LICHII_COOP_CHEATS=1) when it is open or the chat box is not.
    // Release kits have no panel (coop_debug_panels.h): F8 opens nothing and takes no keys.
    if (CoopDebugPanels::kOn && (g_cheats.open() || (!g_box.open() && CoopCheats::On()))) {
        const auto rows = CheatRows();
        if (g_cheats.Key(vk, repeat, rows.size())) {
            GiveChosen(rows);
            return true;
        }
    }
    if (!g_box_key_set) {  // LICHII_COOP_CHAT_KEY: the key that opens the box (T unless set), until the tab sets one
        g_box_key_set = true;
        UseChatKey(Env("LICHII_COOP_CHAT_KEY"));
    }
    if (const std::uint64_t next = g_box_key_next.exchange(kNoChatKey); next != kNoChatKey) {
        g_box.SetOpenKey(static_cast<std::uint32_t>(next));   // (after the variable: the tab's choice wins)
    }
    const bool taken = g_box.Key(vk, repeat);
    if (auto text = g_box.TakeSend()) {
        const bool ok = CoopHook::SendChat(*text);
        g_box.Sent(ok);
        if (!ok) {
            LOG_INFO(Core, "coop: chat not sent (not connected, or refused); the box stays open");
        }
    }
    return taken;
}

bool TypeChar(std::uint32_t byte) {
    if (CoopPauseTab::KeysTaken()) {   // the pause menu's Multiplayer page's field (ASCII only: a room name, a password)
        CoopPauseTab::Char(byte < 0x80 ? byte : 0);
        return true;
    }
    if (!g_on) {
        return false;
    }
    std::lock_guard lock(g_box_mutex);
    if (!CoopPause::KeysTaken() && !g_box.open() && !g_cheats.open()) {
        return false;
    }
    const std::uint32_t cp = FromCodePage(byte);
    if (cp == 0) {
        return true;
    }
    if (CoopPause::Char(cp)) {
        return true;
    }
    if (g_cheats.open()) {
        return g_cheats.Char(cp, CoopChatBox::Utf8(cp));
    }
    return g_box.Char(cp);
}

bool Mouse(float fx, float fy, int event) {
    if (!g_on || event < HOS_COOP_MOUSE_MOVE || event > HOS_COOP_MOUSE_WHEEL_DOWN) {
        return false;
    }
    std::lock_guard lock(g_box_mutex);
    if (!g_cheats.open()) {
        return false;
    }
    const auto rows = CheatRows();
    const float w = g_drawn_w.load(), h = g_drawn_h.load();
    std::optional<std::size_t> row;
    if (w > 0 && h > 0) {
        CoopDraw::CheatsView v;  // its layout only: how many rows, whether a result line shows
        v.rows.resize(rows.size());
        v.result = CoopCheats::LastResult();
        row = CoopDraw::CheatsRowAt(w, h, v, fx * w, fy * h);
    }
    static constexpr CoopCheatsPanel::Mouse kWhat[] = {CoopCheatsPanel::Mouse::kMove, CoopCheatsPanel::Mouse::kLeft,
                                                      CoopCheatsPanel::Mouse::kWheelUp,
                                                      CoopCheatsPanel::Mouse::kWheelDown};
    const bool taken = g_cheats.Pointer(kWhat[event], row, rows.size());
    GiveChosen(rows);
    return taken;
}

void SetChatKey(const std::string& key) {
    const std::string k = key.empty() ? std::string("T") : key;
    if (const auto vk = CoopChatBox::ParseOpenKey(k)) {
        g_box_key_next.store(*vk);   // (no g_box_mutex here: see g_box_key_next)
        LOG_INFO(Core, "coop: chat box key: {} (from the Multiplayer tab)", *vk == 0 ? std::string("off") : k);
    } else {
        LOG_WARNING(Core, "coop: chat key {} is not a letter, a digit, F1 to F12 or off; the key stays", k);
    }
}

// (a menu's typing ended by Enter or Esc: kept until that key is let go, as the chat box does. The pause page's
// Enter went on to the game as its own key and shut the pause menu, a test run 2026-11-20)
static std::atomic<bool> g_menu_keys_release{false};

bool ChatOpen() {
    std::lock_guard lock(g_box_mutex);
    return g_box.open() || g_cheats.open();
}

bool KeysTaken() {
    if (CoopPauseTab::KeysTaken() || CoopPause::KeysTaken()) {
        g_menu_keys_release = true;
        return true;
    }
    if (g_menu_keys_release.load()) {
        if (AsyncDown(0x0D) || AsyncDown(0x1B)) return true;
        g_menu_keys_release = false;
    }
    if (!g_on) {
        return false;
    }
    std::lock_guard lock(g_box_mutex);
    const bool closing = AsyncDown(0x0D) || AsyncDown(0x1B);
    const bool box = g_box.KeysTaken(closing);
    const bool cheats = g_cheats.KeysTaken(closing || AsyncDown(CoopCheatsPanel::kKeyToggle));
    return box || cheats;
}

// This game's controller and keyboard (coop_input.h): longshot.exe's patch 0015 exports the choice; the kit's
// LICHII_COOP_PAD and LICHII_COOP_KEYBOARD set it. Applied before co-op may turn itself off, so two games on one PC
// keep their own pads either way (the choice stays in the exe). An exe without patch 0015: every pad counts.
void UseInputSelect() {
    CoopInput::Exports e;
#ifdef _WIN32
    const HMODULE exe = GetModuleHandleW(nullptr);
    e.select = reinterpret_cast<decltype(e.select)>(reinterpret_cast<void*>(GetProcAddress(exe, "hos_coop_input_select")));
    e.pad_name =
        reinterpret_cast<decltype(e.pad_name)>(reinterpret_cast<void*>(GetProcAddress(exe, "hos_coop_input_pad_name")));
#else
    e.select = reinterpret_cast<decltype(e.select)>(dlsym(RTLD_DEFAULT, "hos_coop_input_select"));
    e.pad_name = reinterpret_cast<decltype(e.pad_name)>(dlsym(RTLD_DEFAULT, "hos_coop_input_pad_name"));
#endif
    CoopInput::Bind(e);
    std::vector<std::string> warnings;
    const auto s = CoopInput::FromValues(Env("LICHII_COOP_PAD"), Env("LICHII_COOP_KEYBOARD"), &warnings);
    for (const auto& w : warnings) {
        LOG_WARNING(Core, "coop: {}", w);
    }
    CoopInput::Apply(s);
    if (!CoopInput::Available()) {
        if (s != CoopInput::Setting{}) {
            LOG_WARNING(Core, "coop: this Hookshot cannot choose a controller (patch 0015): every pad counts");
        }
        return;
    }
    LOG_INFO(Core, "coop: controller {}, keyboard {}", CoopInput::PadValue(s.pad), CoopInput::KeyboardValue(s.keyboard));
}

// Why nothing may choose the title's buttons for the player now (nullptr: free): co-op is hosting, joining, in a
// session or the host's save is on its way. Skip title screen's hold (hos_coop_title_hold) and the code mods' choice
// (mod API v2) ask it.
const char* TitleHoldWhy() {
    CoopTitleMenu::SkipTitleInput in;
    HosCoopStatus st{};
    st.size = sizeof st;
    HostMenu().Status(&st);
    in.host_state = st.state;
    in.session_live = CoopSave::SessionLive();
    in.save_coming = CoopSave::HostSaveComing();
    in.save_wait = CoopTitle::SaveWaitWaiting();
    return CoopTitleMenu::SkipTitleHold(in);
}

bool Start(const HosCoopApi* api) {
    if (api == nullptr || api->version != HOS_COOP_API_VERSION || api->size < kFirstApiSize) {
        return false;
    }
    g_api = api;
    g_build = api->main_build_id != nullptr ? api->main_build_id : "";
    if (!BuildSupported(g_build)) {
        LOG_WARNING(Core, "coop: {}", CoopGameBuild::LogRefusal(g_build));
        g_api = nullptr;
        return false;
    }
    LOG_INFO(Core, "coop: Hookshot module, game build {}, main at {:#x}{}", g_build.substr(0, 16), api->main_base,
             HasPadButtons(api) ? "" : "; this Hookshot gives no pad word (own Ocarina notes not sent: build it again)");
    // Started without the kit's launcher: its coop-options.ini switches still count.
    CoopTitle::ApplyKitOptions();
    // Row 219 (#84): the one-time placements the kit's start lists beside the mods (merge_tables.py one_time_keys),
    // read by the hook when a session starts. The mods folder as Hookshot's host/mods.c finds it.
    if (Env("LICHII_COOP_ONE_TIME_KEYS").empty()) {
        const std::string mods = Env("HOS_MODS");
        const std::filesystem::path root =
            mods.empty() ? CoopRuntime::GameDir() / "mods" : std::filesystem::path(std::u8string(mods.begin(), mods.end()));
        CoopFiles::SetEnv("LICHII_COOP_ONE_TIME_KEYS", CoopFiles::Utf8Path(root / CoopPickups::kOneTimeFile));
    }
    // Original game (coop_original.h): the module stays off, all of it, and Longshot's F1 menu gets one tab with the
    // switch alone, so the player can turn it back.
    // The mods list in step with the switch first (CoopOriginal::Sync: a list an older kit wrote, the switch set or
    // taken out by hand in coop-options.ini; report 266), for the next start: Longshot read the list before the module.
    const auto options = CoopTitle::KitOptionsFile();
    const bool original = options && CoopOriginal::OnIn(CoopTitle::ReadKitOptions());
    if (options) {
        bool changed = false;
        std::string why;
        if (!CoopOriginal::Sync(CoopRuntime::GameDir() / "mods", original, changed, why)) {
            LOG_WARNING(Core, "coop: original game: the mods list is not in step with the switch: {}", why);
        } else if (changed) {
            LOG_INFO(Core, "coop: original game {}: the mods list put in step with it (from the next start)",
                     original ? "on" : "off");
        }
    }
    if (original) {
        g_original.Open(*options, CoopRuntime::GameDir() / "mods");
        g_original_on = true;
        LOG_INFO(Core, "coop: original game ({}): the module stays off; F1's \"Original game\" tab turns it back",
                 CoopFiles::Utf8Path(*options));
        g_api = nullptr;
        return false;
    }
    UseInputSelect();
    // (the map's players and names as coop-options.ini says, now that ApplyKitOptions put them in the process's
    // variables: read at the module's load they missed the kit's options without the kit's launcher, 2026-11-20)
    g_map_players.store(!EnvOff("LICHII_COOP_MAP"));   // (the same for the map's players and names)
    g_map_names.store(!EnvOff("LICHII_COOP_MAP_NAMES"));
    CoopFiles::CheckName();  // never the PC's user or PC name as the co-op name (coop_name_privacy.h), before coop.ini is read
    const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
    std::vector<std::string> errors;
    // The Multiplayer tab (patch 0004's Hookshot and later, or HOS_COOP_MENU=1): the module stays loaded with co-op idle
    // until the player hosts or joins from the tab (coop_pause.h, hos_coop_host.h).
    const bool menu = EnvOn("HOS_COOP_MENU") || TitleMenuPossible(api, Env("LICHII_COOP_TITLE_MENU"));
    const bool server = CoopHook::LoadSettings(ini, errors).enabled();
    for (const auto& e : errors) {
        LOG_WARNING(Core, "coop: {}", e);
    }
    if (!menu && !server) {
        CoopHook::Start();  // logs why co-op is off
        g_api = nullptr;
        return false;
    }
    if (!server) {
        LOG_INFO(Core, "coop: no server in coop.ini: co-op idle until Host or Join on the menu's Multiplayer tab");
    }
    // The same switches that make the emulator put its cave in.
    CoopLook::AgreeStartTunic();  // your start tunic from the pack's files, before the puppets read it (coop_look.h)
    if (EnvOn("LICHII_COOP_PUPPET") || EnvOn("LICHII_COOP_DEBUG_PUPPET")) {
        CoopPuppetConfig::Configure(PerceivedSafe());  // as the emulator's cave (coop_puppet_config.h)
    }
    CoopPackNotice::Enable(CoopPuppetConfig::HasPack());  // no pack: the chat says so once (coop_pack_notice.h)
    // The kit's start file sets OOT_COOP_KIT_START (play.ps1, start.sh, the autotest). Without it (and not Nucleus
    // Co-op, which starts each game its own way) the chat says to use Start Squadala (player report 225).
    const bool direct = !EnvOn("OOT_COOP_KIT_START") && !CoopLook::NucleusWrote(CoopFiles::ReadText(ini));
    CoopPackNotice::EnableDirect(direct);
    if (direct) {
        LOG_WARNING(Core, "coop: the game was not started with Start Squadala (the kit's start file): the title and the chat say so");
    }
    g_hold = EnvOn("LICHII_COOP_OBJECTS_AI_HOLD");
    CoopLinkSound::SetProbe(EnvOn("LICHII_COOP_DEBUG_SOUND"));  // every user's sounds named once in the log
    // Flags through the game's own functions (coop_flag_native.h): its writes as events from the applier's wrap, ours
    // through its setters on the calc thread. LICHII_COOP_FLAG_EVENTS=0 / LICHII_COOP_FLAG_SETTERS=0: as on the
    // emulator (the table diff, writes into the game's queues).
    CoopFlagNative::Counts().events_on.store(Env("LICHII_COOP_FLAG_EVENTS") != "0");
    g_flag_setters.store(Env("LICHII_COOP_FLAG_SETTERS") != "0");
    coop::host::GameFlags::use_calls(g_flag_setters.load() ? &CoopFlagNative::Calls() : nullptr);
    LOG_INFO(Core, "coop: flags: the game's writes {}, our writes {}",
             CoopFlagNative::Counts().events_on.load() ? "as events from its applier (patch 0005's wrap)"
                                                       : "by reading the tables (LICHII_COOP_FLAG_EVENTS=0)",
             g_flag_setters.load() ? "through the game's setters on its calc thread"
                                   : "into the game's queues (LICHII_COOP_FLAG_SETTERS=0)");
    // Row 63: the host's Gold Skulltula choice (coop-options.ini gold_skulltulas) before co-op starts, so a Host.bat
    // start, the first player in, puts it in force at its welcome (lane run of 2026-11-23: read later, it was lost).
    CoopFiles::ReadOptions();
    CoopHook::Start();
    g_on = true;
    CoopModsRuntime::Start();  // the mods folder and the code mods (docs/mods.md)
    // (mod API v2: the menu screens' calls) A code mod's button choice waits while co-op is busy at the title, unless
    // the player pressed A on the title's first button just before: then it is theirs (Save Profiles' file select
    // takes that A and presses Continue for the file picked; refused, the player was left at the title).
    CoopModsRuntime::SetGame(api, [] {
        const char* why = TitleHoldWhy();
        if (why != nullptr && CoopTitle::PlayerChoseLately()) {
            static std::atomic<const char*> said{nullptr};
            if (said.exchange(why) != why) {
                LOG_INFO(Core, "coop: mods: a mod's button choice goes through while {}: the player pressed A on the "
                               "title's first button just before",
                         why);
            }
            return static_cast<const char*>(nullptr);
        }
        return why;
    });
    CoopTitle::Init(api);
    CoopPauseTab::Init(api);   // the pause menu's Multiplayer tab (patch 0030)
    CoopPauseWorld::Init(api);   // row 299's PauseMgr probe (test only: the bridge asks)
    CoopLongshotSettings::Init(api);   // Longshot's own settings for the Settings tab (patch 0032; none from an older Hookshot)
    CoopPause::Init(server);
    CoopTestInput::SetMenuOpener(CoopPause::TestMenu);   // (the autotest's menu step; only a commands file asks it)
    CoopPlaytest::Start(g_system, CoopRuntime::GameDir());   // the play-test bridge: off unless LICHII_COOP_PLAYTEST_PORT
    CoopLook::StartModule();  // the look chosen in the pause menu (coop_look.h)
    return true;
}

void LocalProbe();   // (below)
void LocalLayer(void* ctx);   // (below)
void DressCharacters(void* ctx);   // (below)

// A test's view of the menus (LICHII_COOP_TEST_STATE=1 only): coop-test-state.txt in the game's folder, 5 times a
// second: the pause page, the title, co-op's menu. No lock of ours held (each part takes its own).
std::atomic<uint64_t> g_test_pad_came{0}, g_test_pad_got{0};   // (hos_coop_pad's, for TestState)
// (each stage's last pad with buttons: our page's, the mods' hold, the title's, down but not out's; TestState)
std::atomic<uint64_t> g_test_st_page{0}, g_test_st_hold{0}, g_test_st_title{0}, g_test_st_dbno{0};
void TestState() {
    static const bool on = [] {
        const char* v = std::getenv("LICHII_COOP_TEST_STATE");
        return v != nullptr && v[0] == '1';
    }();
    if (!on) return;
    static std::uint64_t next = 0;
    const std::uint64_t now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    if (now < next) return;
    next = now + 200;
    const std::string line = fmt::format("{} {} mp_page={} keys_taken={} host_started={} pad_came={:#x} pad_got={:#x} st_page={:#x} st_hold={:#x} "
                                         "st_title={:#x} st_dbno={:#x} t={}\n",
                                         CoopPauseTab::TestState(), CoopTitle::TestState(),
                                         static_cast<int>(CoopPause::NativePage()), CoopPause::KeysTaken() ? 1 : 0,
                                         CoopPause::HostStarted() ? 1 : 0, g_test_pad_came.load(), g_test_pad_got.load(),
                                         g_test_st_page.load(), g_test_st_hold.load(), g_test_st_title.load(),
                                         g_test_st_dbno.load(), now);
    const std::filesystem::path p = CoopRuntime::GameDir() / "coop-test-state.txt";
    if (FILE* f = std::fopen(p.string().c_str(), "wb")) {
        std::fwrite(line.data(), 1, line.size(), f);
        std::fclose(f);
    }
}

void Frame(void* ctx, int site) {
    if (g_original_on) {
        return;   // (original game: not even the window's title)
    }
    if (site == HOS_COOP_SITE_FRAME) {
        // The play-test bridge's hold (docs/playtest.md): first, before any co-op work and with no lock of ours held,
        // so a held frame has not run at all and nothing else waits on it. Off: one atomic load.
        CoopPlaytest::AtFrame();
        CoopGameClock::NoteFrame();   // the game's clock: one frame ran (coop_game_clock.h), solo too
    }
    CoopWindowBrand::Tick(CoopRuntime::GameDir());  // the window's title and icon, co-op on or idle (solo too)
    if (site == HOS_COOP_SITE_FRAME) {  // row 271: this game's memory once a minute, solo too (coop_host_menu.h)
        // Row 292: read on a thread of its own, never in the frame. fps-spike-0121: counters only; the heap walk froze
        // the whole game even off the frame, so it runs only for a memory hunt (OOT_COOP_MEMORY_HEAPS=1).
        static std::uint64_t next_memory_ms = 0;
        static CoopOffFrame::Slot memory_read;
        if (CoopHostMenu::MemoryLineDue(SteadyMs(), next_memory_ms)) {
            memory_read.Start([] {
                std::string text = CoopHostMenu::ProcessMemoryText();
                if (CoopHostMenu::MemoryHeapsOn()) {
                    text += "; " + CoopHostMenu::ProcessHeapsText();
                }
                LOG_INFO(Core, "coop: memory: {}", text);
            });
        }
    }
    if (!g_on) {
        return;
    }
    if (site == HOS_COOP_SITE_FRAME) {
        CoopFrameClock::Note();  // jitter-b: the game frame the next present reads (coop_frame_clock.h)
        CoopModsRuntime::Frame();
        CoopVoiceGameMic::Poll();   // (row 229) Longshot's Insert-menu microphone, for the voice's Default
        CoopPause::Tick();   // the Multiplayer tab's follow-ups, also while the menu is closed
        TestState();         // (LICHII_COOP_TEST_STATE=1 only)
        CoopLook::Frame();   // a look chosen in the pause menu, once it closed
        LocalProbe();
        if (ctx != nullptr) LocalLayer(ctx);
        if (ctx != nullptr) DressCharacters(ctx);
    }
    if (site == HOS_COOP_SITE_PRESENT && !g_present_seen.exchange(true)) {
        LOG_INFO(Core, "coop: Hookshot calls co-op at each present (loads and pause included)");
    }
    if (site != HOS_COOP_SITE_PRESENT && g_world_version.load() == 0) {
        // A Hookshot built without patch 0005 never says hello (one with it does at the first hooked function the game
        // runs): say so once, some thousand frame hooks in.
        static std::atomic<std::uint32_t> frames{0};
        if (frames.fetch_add(1) == 3600) {
            LOG_WARNING(Core, "coop: world hooks: hookshot.exe has not said hello yet: built without patch 0005, or no "
                              "hooked game function ran so far (until it does: no pause counters, no flag events)");
        }
    }
    const FrameWork w = WorkAt(site, g_present_seen.load());
    if (w.on_frame) {
        CoopHook::OnFrame(g_system);
    }
    if (w.calls && ctx != nullptr) {
        {  // party-hud-crash-c126: which listed screens are live (CoopNativeUi::ScreenLive), before any screen work this
           // frame: the game's own scene load (Sequence_IsLoadingBanc, read as CoopStuckLoad does: coop_hook.cpp
           // StuckLoadFrame; on through the boot, the title and every load) and patch 0018's running screens and HUD.
            static const std::uint32_t loading_hash = coop::host::flag_hash("Sequence_IsLoadingBanc");
            CoopHook::EmulatorMemory memory(g_system.ApplicationMemory(), CoopRuntime::MainBase());
            const bool loading = coop::host::GameFlags(memory).get_bool(loading_hash).value_or(false);
            const bool was_up = CoopNativeUi::GameUiUp();
            if (CoopNativeUi::SceneLoading() != loading) {
                LOG_INFO(Core, "coop: native ui: scene load {} (Sequence_IsLoadingBanc): {}", loading ? "began" : "ended",
                         loading ? "no screen's layout is called into until it ends"
                                 : "screens handed out once the game's HUD screen has run");
            }
            CoopNativeUi::SetSceneLoading(loading);
            // party-hud-crash-c126-6: the title's menu up is no play scene either (a title reached from play by the pause
            // menu's Title Screen has no scene load: the HUD and panel screens co-op knew are freed under it).
            static bool title_said = false;
            const bool title = CoopTitle::Up();
            if (title != title_said) {
                title_said = title;
                LOG_INFO(Core, "coop: native ui: the title's menu is {}", title
                             ? "up: no play scene, no screen is called into until play is back"
                             : "gone: screens handed out once the game's HUD screen has run");
            }
            CoopNativeUi::SetTitleUp(title);
            const auto menu = CoopMapPage::MenuState();
            std::optional<std::vector<std::uint64_t>> running;
            std::optional<bool> hud_runs;
            if (menu && menu->screens_known) {   // (row 141's struct: the running screens and the HUD's ran time)
                running.emplace(menu->screens.begin(), menu->screens.begin() + menu->screen_count);
                hud_runs = menu->HudRuns();
            }
            CoopNativeUi::SetRunningScreens(std::move(running), hud_runs);
            if (CoopNativeUi::GameUiUp() != was_up) {
                LOG_INFO(Core, "coop: native ui: the game's play UI {}", !was_up
                             ? "is up (patch 0018's HUD screen ran since the load): its screens are live"
                             : "is down (a scene load or the title): no screen is live");
            }
            static bool said_unknown = false;
            if (!hud_runs && !std::exchange(said_unknown, true)) {
                LOG_WARNING(Core, "coop: native ui: this Longshot's patch 0018 does not say which screens run: no screen is "
                                  "called into (the native chat, the party HUD and the balloons wait)");
            }
        }
        RunFlagSetters(ctx);
        LogFlagNative();
        {  // rows 63/76: the game's own HUD refresh after co-op wrote a dungeon item (coop_map_refresh.h). Patch 0018
           // runs it at the HUD screen's own next update: never on a stored screen pointer.
            const auto now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                               std::chrono::steady_clock::now().time_since_epoch())
                                                               .count());
            if (CoopMapRefresh::TheRefresher().Due(now_ms)) CoopMapPage::AskHudRefresh();
        }
        RunCalls(ctx);
        RequestHostAutoSave(ctx);   // (only when a guest's ask opened a round: CoopSave::TakeSaveRequest)
        NativeStep(ctx);            // chat in the game's own message window and banner (coop_native_ui.h)
        NativeTags(ctx);            // the name tags on the game's balloons (coop_native_tags.h)
        LogBossNative();
        LogBossOrders();
        LogBossShooter();
        ShooterProbe();
        CoopMapPage::Frame(ctx, g_api);   // the Map page's panes, found once while the game plays
        if (const auto menu = CoopMapPage::MenuState()) {  // minimap-b: the drop log's menu state (patch 0018)
            coop::host::map_drop_log().set_menu(menu->menu_open, menu->map_page);
        }
    }
}

// (CREATE_FROM's caller and create argument until that thread's CREATE_REQUEST)
thread_local std::uint64_t t_create_from = 0;
thread_local std::uint64_t t_create_arg = 0;
// (CREATE_REQUEST's verdict until that thread's CREATE_STOP)
thread_local bool t_create_stop = false;
// (SHOOT_FROM's shooter until that thread's next CREATE_REQUEST; used only for
// the create the shoot code itself makes, by its return address)
thread_local std::uint64_t t_shooter = 0;
constexpr std::uint64_t kShootCreateReturn = 0xc17300;  // (after the bl at main+0xc172fc)
// (LIFE_STEP's verdict for its component until that step's LIFE_STOPS: past its own life stops)
thread_local std::uint64_t t_life_pass = 0;
// (LIFE_STOPS passed for this component: that step's death keeps no revive, LIFE_REVIVE)
thread_local std::uint64_t t_life_dead = 0;

// The world hooks (patch 0005): the game's C hands its world events here instead of into caves and guest rings.
int World(std::uint32_t event, std::uint64_t a, std::uint64_t b) {
    if (!g_on) {
        return 0;
    }
    switch (event) {
    case HOS_COOP_WORLD_HELLO: {
        // (the first patch 0005 sent a = 0: level 1, with the flag applier's wrap)
        const WorldLevel level = HelloLevel(a, b);
        g_world_wraps.store(level.wraps);
        SetBossNative();
        SetBossOrders();
        SetBossShooter();
        SetShooterProbe();
        if (g_world_version.exchange(level.version) != level.version) {
            LOG_INFO(Core, "coop: down but not out: the crawl's room check {}",
                     WrapIn(HOS_COOP_WRAP_CRAWL_ROOM) ? "wrapped (a downed child keeps crawling)"
                                                      : "not wrapped in this hookshot.exe (patch 0046)");
            LOG_INFO(Core, "coop: down but not out: the game's play request {}",
                     WrapIn(HOS_COOP_WRAP_POSE_PLAY) ? "wrapped (a downed Link's lying pose is held)"
                                                     : "not wrapped in this hookshot.exe (patch 0049)");
            LOG_INFO(Core, "coop: world hooks: hookshot.exe has level {} (this module {}), wraps {:#x}{}", level.version,
                     HOS_COOP_WORLD_VERSION, level.wraps,
                     level.version < HOS_COOP_WORLD_VERSION ? ": an older Hookshot build, some hooks are missing" : "");
            if ((level.wraps >> HOS_COOP_WRAP_SOUND_EMIT & 1) == 0) {
                LOG_INFO(Core, "coop: this Hookshot has no sound wrap (patch 0014): our Link's sounds are not sent");
            }
            if (CoopPlaytest::On() && (level.wraps >> HOS_COOP_WRAP_PANE_TEXT & 1) == 0) {
                LOG_INFO(Core, "coop: playtest: this Hookshot has no pane text wrap (patch 0041): `text` stays empty");
            }
            if (CoopPlaytest::On() && (level.wraps >> HOS_COOP_WRAP_PANE_BODY & 1) == 0) {
                LOG_INFO(Core, "coop: playtest: this Hookshot has no pane body wrap (patch 0042): a talk's box text "
                               "is not in `text`");
            }
        }
        {
            // Patch 0024: the game's own save loads (docs/flags.md, "A load window"). All three wraps, or the old signs.
            const bool loads = (level.wraps >> HOS_COOP_WRAP_SAVE_LOADED & 1) != 0 &&
                               (level.wraps >> HOS_COOP_WRAP_SAVE_LOAD_START & 1) != 0 &&
                               (level.wraps >> HOS_COOP_WRAP_TEMP_LOAD_START & 1) != 0 &&
                               (level.wraps >> HOS_COOP_WRAP_GAME_OVER_SAVE & 1) != 0;
            if (CoopSave::NativeSaveLoads() != loads || !loads) {
                LOG_INFO(Core, "{}", loads ? "coop: save loads: the game's own load nodes tell every save load (patch 0024)"
                                           : "coop: save loads: this Hookshot has no save-load wraps (patch 0024): a save "
                                             "load is guessed from the flags and the title");
            }
            CoopSave::SetNativeSaveLoads(loads);
        }
        return 1;
    }
    case HOS_COOP_WORLD_COUNTER_START: {  // (coop_counters.h: the Objects walk reads the ring)
        CoopCounters::TheHostRing().Push(a);
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            LOG_INFO(Core, "coop: world hooks: first pause counter start from hookshot.exe (node {:#x})", a);
        }
        return 1;
    }
    case HOS_COOP_WORLD_NODE_START: {  // (patch 0029, hos_coop_nodes.h: coop_world.h's NodeStarts; the Objects walk
                                       // looks a pushed block's node up there, FindBlockNode)
        CoopWorld::TheNodeStarts().Push(a, b);
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            LOG_INFO(Core, "coop: world hooks: first AI node start from hookshot.exe (node {:#x}, actor {:#x})", a, b);
        }
        return 1;
    }
    // The actor events (patch 0007, hos_coop_actors.h; coop_actor_events.h:
    // the Objects step drains them on the frame hook). Only reads here.
    case HOS_COOP_WORLD_SHOOT_FROM: {  // (a: its first 8 bytes are the shooter's actor)
        std::uint64_t actor = 0;
        t_shooter = a != 0 && CoopRuntime::Read(a, &actor, sizeof actor) ? actor : 0;
        return 1;
    }
    case HOS_COOP_WORLD_CREATE_FROM: {  // (just before CREATE_REQUEST, on this thread)
        t_create_from = a;
        t_create_arg = b;
        return 1;
    }
    case HOS_COOP_WORLD_CREATE_REQUEST: {
        CoopActorEvents::Event e;
        e.kind = CoopActorEvents::Kind::Create;
        e.flags = CoopActorEvents::Ours(t_own_call, t_copy_game) ? CoopActorEvents::kOwn : 0;
        e.a = a;
        e.b = b;
        e.from = std::exchange(t_create_from, 0);
        const std::uint64_t arg = std::exchange(t_create_arg, 0);
        std::uint64_t text = 0;  // (x1: its first 8 bytes point at the name; on the caller's stack: read now)
        // The ring keeps 31 bytes of the name; the stop rules below need it whole
        // (boss-larva-0116: Enemy_DungeonBoss_Dodongo_SpawnBabyDodongo is 42, so a
        // copy King Dodongo's own spawner was never stopped). The rest is read
        // apart, so a name near the end of its memory still gives its first 31.
        char name[64] = {};
        if (a != 0 && CoopRuntime::Read(a, &text, sizeof text) && text != 0 &&
            CoopRuntime::Read(text, name, 31)) {
            if (std::memchr(name, 0, 31) == nullptr && !CoopRuntime::Read(text + 31, name + 31, sizeof name - 32)) {
                name[31] = 0;
            }
            e.SetName(name);
        }
        float place[3] = {};
        if (arg != 0 && CoopRuntime::Read(arg, place, sizeof place)) {
            for (int i = 0; i < 3; ++i) {
                std::memcpy(&e.place[i], &place[i], 4);
            }
        }
        // A copy enemy's own shot is not made at all (coop_enemy_shots.h):
        // the owner's game fires, and its shot comes as a copy, which is
        // one of our own creates and never stopped.
        // The shooter is the one the game's shoot code named, when this is
        // that code's own create; any other create is judged by its place.
        const std::uint64_t shooter = std::exchange(t_shooter, 0);
        const bool shot = e.from != 0 && e.from == CoopRuntime::MainBase() + kShootCreateReturn;
        t_create_stop = !CoopActorEvents::Ours(t_own_call, t_copy_game) && name[0] != 0 &&
                        (CoopEnemyShots::StopAtCreate(name, {place[0], place[1], place[2]}, shot ? shooter : 0) ||
                         // Native boss sync: a copy boss's own minion (the owner's comes as a copy).
                         CoopBossNative::TheTable().CopyMakes(CoopObjects::MinionMaker(name)));
        if (t_create_stop) {
            e.flags |= CoopActorEvents::kStopped;
        }
        CoopActorEvents::TheRing().Push(e);
        return 1;
    }
    case HOS_COOP_WORLD_CREATE_STOP: {  // (a: our C's uint32, in hookshot.exe's memory)
        auto* stop = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(a));
        if (stop != nullptr && std::exchange(t_create_stop, false)) {
            *stop = 1;
        }
        return 1;
    }
    case HOS_COOP_WORLD_DELETE_REQUEST: {
        CoopActorEvents::Event e;
        e.kind = CoopActorEvents::Kind::Delete;
        e.flags = CoopActorEvents::Ours(t_own_call, t_copy_game) ? CoopActorEvents::kOwn : 0;
        e.a = a;
        e.b = b;
        CoopActorEvents::TheRing().Push(e);
        return 1;
    }
    case HOS_COOP_WORLD_LIFE_STEP: {  // (b: our C's int32 sum, in hookshot.exe's memory)
        auto* sum = reinterpret_cast<std::int32_t*>(static_cast<std::uintptr_t>(b));
        if (sum == nullptr) {
            return 0;
        }
        if (!CoopActorEvents::LifeStepSeen().exchange(true, std::memory_order_relaxed)) {
            LOG_INFO(Core, "coop: actors: the game's Life step calls the module (patch 0007): hits go in through it");
        }
        // Row 230: our own Link's hits, read here, where its step has them: the step sets our Link's hit id and
        // clears it inside the frame, so the walk never saw one (scream-diag-230, 2026-11-23). A scream (no life)
        // reaches this step too (sum 0). The walk hands them to the Sync (CoopObjects::NoteOurLinkStepHit).
        if (a != 0 && a == CoopDbno::OurLifeComponent()) {
            static std::atomic<std::uint32_t> last_id{0xffffffffu};
            std::uint32_t id = 0xffffffffu;
            CoopHitReplay::Record rec{};
            if (CoopRuntime::Read(a + CoopHitReplay::kLastHitId, &id, sizeof id) && id != 0xffffffffu &&
                last_id.exchange(id, std::memory_order_relaxed) != id &&
                CoopRuntime::Read(a + CoopHitReplay::kHitRecord, rec.data(), sizeof rec)) {
                CoopObjects::NoteOurLinkStepHit(id, rec, *sum);
            }
        }
        // Difficulty (coop_difficulty.h): what the game takes of the frame's own damage. What another
        // game's hit gives (the life table) was scaled where that hit was dealt.
        const std::int32_t own = CoopDifficulty::TheKinds().Step(
            a, *sum, CoopDifficulty::Now(), CoopDifficulty::SteadyMs(), [a] {
                std::uint64_t attacker = 0;
                return CoopRuntime::Read(a + CoopDifficulty::kHitAttacker, &attacker, sizeof attacker) ? attacker
                                                                                                         : 0;
            });
        t_life_pass = 0;
        t_life_dead = 0;
        bool judged = false;
        // enemy-late-kill: a fall given in place of our replayed hit is dropped once the component's last hit id
        // reached the replay (its damage step took it, this frame or before); read only while something waits.
        CoopActorEvents::LifeTable& table = CoopActorEvents::TheLifeTable();
        std::uint32_t last_id = CoopActorEvents::kNoHitId;
        bool id_read = true;
        if (table.Pending(a) != 0 &&
            !CoopRuntime::Read(a + CoopActorEvents::kLifeLastHitId, &last_id, sizeof last_id)) {
            id_read = false;
            last_id = CoopActorEvents::kNoHitId;  // (not reached: the fall is taken, the safe side)
        }
        std::uint32_t withdraw = CoopActorEvents::kNoHitId;
        *sum = CoopActorEvents::Step(CoopActorEvents::TheRing(), table, a, own, &judged, last_id, &withdraw);
        if (withdraw != CoopActorEvents::kNoHitId) {
            // The fall was taken in the replay's place: a replay still waiting is withdrawn (its present word
            // cleared), so its damage step cannot deal the same fall again later. Said (LICHII_COOP_HIT_PROBE).
            std::uint32_t record_id = 0;
            std::uint32_t present = 0;
            std::uint32_t flags = 0;
            CoopRuntime::Read(a + CoopActorEvents::kLifeRecordId, &record_id, sizeof record_id);
            CoopRuntime::Read(a + CoopActorEvents::kLifeRecordPresent, &present, sizeof present);
            CoopRuntime::Read(a + CoopBossNative::addr::kLifeFlags, &flags, sizeof flags);
            CoopActorEvents::Event e;
            e.kind = CoopActorEvents::Kind::Refused;
            e.a = a;
            e.from = flags;
            if (CoopActorEvents::WithdrawReplay(withdraw, record_id, present)) {
                const std::uint32_t none = 0;
                CoopRuntime::Write(a + CoopActorEvents::kLifeRecordPresent, &none, sizeof none);
                e.flags |= CoopActorEvents::kWithdrawn;
            }
            if (!id_read) {
                e.flags |= CoopActorEvents::kIdUnread;
            }
            CoopActorEvents::TheRing().Push(e);
        }
        const auto life_now = [a]() -> std::optional<std::int32_t> {
            std::uint64_t value = 0;
            std::int32_t life = 0;
            if (CoopRuntime::Read(a + CoopDbno::kLifeValueEntry, &value, sizeof value) && value != 0 &&
                CoopRuntime::Read(value + 8, &life, sizeof life)) {
                return life;
            }
            return std::nullopt;
        };
        // A death kept past its revive ends when the component lives again (a heal, a new actor at that address).
        if (CoopActorEvents::KeptDeaths& kept = CoopActorEvents::TheKeptDeaths(); kept.Kept(a)) {
            if (const auto life = life_now(); life && *life > 0) {
                kept.Alive(a);
            }
        }
        // One boss death for everyone (coop_boss_native.h HeldLifeStep): a copy boss's own Life step
        // keeps it at life 1 until its owner's has died; then this same step lets it die.
        bool finishing = false;
        if (CoopBossNative::Table& bosses = CoopBossNative::TheTable(); *sum > 0 && bosses.Watches(a)) {
            if (const auto life = life_now()) {
                *sum = bosses.HeldLifeStep(a, *life, *sum);
                finishing = bosses.Finishes(a);
            }
        }
        // Down but not out (coop_dbno_game.h): our Link's last life is held back, after every other
        // change to the sum (difficulty, PvP), so the guard sees what would really be taken.
        if (a != 0 && a == CoopDbno::OurLifeComponent()) {
            std::uint64_t value = 0;
            std::int32_t life = 0;
            if (CoopRuntime::Read(a + CoopDbno::kLifeValueEntry, &value, sizeof value) && value != 0 &&
                CoopRuntime::Read(value + 8, &life, sizeof life)) {
                *sum = CoopDbno::LifeStep(a, life, *sum);
            }
        }
        // The owner's death goes past the copy's own life stops (CoopActorEvents::PassStops; LIFE_STOPS below).
        if (*sum > 0 && (judged || finishing)) {
            if (const auto life = life_now(); life && CoopActorEvents::PassStops(*sum, *life, judged, finishing)) {
                t_life_pass = a;
            }
        }
        return 1;
    }
    case HOS_COOP_WORLD_LIFE_STOPS: {  // (b: our C's uint32, in hookshot.exe's memory)
        auto* pass = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(b));
        if (!CoopActorEvents::LifeStopsSeen().exchange(true, std::memory_order_relaxed)) {
            LOG_INFO(Core, "coop: actors: the game's Life step asks the module before its life stops (patch 0007): "
                           "an owner's death goes past a copy's own");
        }
        if (pass != nullptr && a != 0 && std::exchange(t_life_pass, 0) == a) {
            *pass = 1;
            t_life_dead = a;  // (its death, if the step reaches 0, keeps no revive either)
            CoopActorEvents::StopsPassed().fetch_add(1, std::memory_order_relaxed);
        }
        return 1;
    }
    case HOS_COOP_WORLD_LIFE_REVIVE: {  // (b: our C's uint32, in hookshot.exe's memory)
        // The same step at life 0, before the revive check (main+0x49d6b0): the actor's own callback gives life back
        // (Gohma: 1 outside her chance time), so a death past the stops still ended at 1 (big-run-0115: every test
        // kill and the owner's death on a copy 21 -> 1). A death whose stops we passed keeps no revive.
        auto* dead = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(b));
        if (!CoopActorEvents::LifeReviveSeen().exchange(true, std::memory_order_relaxed)) {
            LOG_INFO(Core, "coop: actors: the game's Life step asks the module before its revive (patch 0007): an "
                           "owner's death and a test kill stay deaths");
        }
        // The check runs on every step that ends at 0, so a death kept once stays kept (KeptDeaths, boss-die-0116).
        const bool passed = a != 0 && std::exchange(t_life_dead, 0) == a;
        if (dead != nullptr && CoopActorEvents::SkipRevive(CoopActorEvents::TheKeptDeaths(), a, passed)) {
            *dead = 1;
            (passed ? CoopActorEvents::RevivesSkipped() : CoopActorEvents::RevivesKeptSkipped())
                .fetch_add(1, std::memory_order_relaxed);
        }
        return 1;
    }
    default:
        return 0;  // (a newer Hookshot's event this module does not know)
    }
}

WorldLevel WorldHooks() {
    return WorldLevel{g_world_version.load(), g_world_wraps.load()};
}

bool HasWrap(std::uint32_t id) {
    return id < 64 && (g_world_wraps.load() >> id & 1) != 0;
}

bool KeyItemTallyNow(coop::host::KeyItemTally& out) {
    static std::mutex lock;
    static coop::host::KeyItemTally cached;
    static bool cached_ok = false;
    static std::chrono::steady_clock::time_point at{};
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> guard(lock);
    if (at == std::chrono::steady_clock::time_point{} || now - at > std::chrono::milliseconds(500)) {
        at = now;
        cached_ok = false;
        if (CoopRuntime::MainBase() != 0) {
            CoopHook::EmulatorMemory memory(g_system.ApplicationMemory(), CoopRuntime::MainBase());
            coop::host::GameFlags flags(memory);
            bool any_flag_table = false;   // (the manager is not up: get_bool says nothing for every flag)
            cached = coop::host::key_item_tally([&](std::uint32_t hash) {
                const auto v = flags.get_bool(hash);
                any_flag_table = any_flag_table || v.has_value();
                return v.value_or(false);
            });
            cached_ok = any_flag_table;
        }
    }
    if (cached_ok) out = cached;
    return cached_ok;
}

// The wrapped game functions (patch 0005, nx2/coop_world_wraps.py). The game's function always runs (0 at phase 0).
int Wrap(std::uint32_t id, int phase, std::uint64_t* x, void* /*ctx*/) {
    if (!g_on || x == nullptr) {
        return 0;
    }
    if (id == CoopPlayerInput::kWrapInputOff) {
        // Native puppets phase 3 (LICHII_COOP_LIFT_PLAYER=1): another player's Link holds ours, so our Link's
        // own input check answers "off" (both callers skip their pad read), as for an extra player without a
        // controller. Only our Link's check: the reading actor ([[[node+8]+8]+0x1f0], node in x19) is ours.
        // Row 299 the same way: a game menu in front with the world running under it (the host's "World runs in
        // menus", CoopPauseWorld::WorldRuns): the pad works the menu, never our Link (phase 0's run: the stick
        // walked Link under the open menu).
        thread_local bool lift_held_off = false;
        if (phase == 1 && std::exchange(lift_held_off, false)) {
            x[0] = 1;
            return 0;
        }
        if (phase == 0 && ((CoopLiftPlayerGame::On() && CoopLiftPlayerGame::OurInputOff()) ||
                           CoopPauseWorld::WorldRuns())) {
            namespace a = CoopPlayerInput::addr;
            std::uint64_t owner = 0, holder = 0, actor = 0;
            const auto link = CoopObjects::OurLinkActor();
            if (link && CoopRuntime::Read(x[19] + a::kNodeOwner, &owner, 8) && owner != 0 &&
                CoopRuntime::Read(owner + a::kOwnerHolder, &holder, 8) && holder != 0 &&
                CoopRuntime::Read(holder + a::kHolderActor, &actor, 8) && actor == *link) {
                lift_held_off = true;
                return 1;  // (the game's check is not run; phase 1 answers 1)
            }
        }
    }
    if (id == CoopPlayerInput::kWrapButtons || id == CoopPlayerInput::kWrapSticks ||
        id == CoopPlayerInput::kWrapInputOff || id == CoopPlayerInput::kWrapControllerGet) {
        // Local co-op: an extra player's Player reads its own controller. (native-puppets-b2's remote input, once
        // wired, is a second handler of these ids: both must run, and either may take phase 0.)
        return CoopLocalGame::Wrap(id, phase, x);
    }
    if (id == HOS_COOP_WRAP_GAME_OVER_SAVE) {
        // Game Over saves its last state (Try Again or Quit Game; patch 0024). Reads nothing.
        if (phase == 0) {
            CoopSave::NoteGameOverSave();
        }
        return 0;
    }
    if (id == HOS_COOP_WRAP_SAVE_LOAD_START || id == HOS_COOP_WRAP_TEMP_LOAD_START || id == HOS_COOP_WRAP_SAVE_LOADED) {
        // A save load starts, or the save is in (patch 0024): the game's own sequence nodes. Reads nothing.
        if (phase == 0) {
            CoopSave::NoteGameSaveLoad(id != HOS_COOP_WRAP_SAVE_LOADED);
        }
        if (phase == 0 && id == HOS_COOP_WRAP_SAVE_LOADED) {
            // The save is in, the player not made yet (the area load after it makes it): a Character Select pick per
            // age is written in place now (coop_char_ages.h). Writes the pick's live value only.
            CoopHook::EmulatorMemory memory(g_system.ApplicationMemory(), CoopRuntime::MainBase());
            CoopCharAges::AtSaveLoaded(memory);
        }
        return 0;
    }
    if (id == HOS_COOP_WRAP_PANE_TEXT) {
        // A text the game sets in a text pane (patch 0041): copied for the play-test bridge's `text` only
        // (coop_talk_text.h). Nothing when the bridge is off. Reads only.
        if (phase == 0 && CoopPlaytest::On()) {
            CoopTalkText::TheLog().Note(x[0], x[1], CoopPlaytest::Frame(), [](std::uint64_t a, void* out, std::size_t n) {
                return CoopRuntime::Read(a, out, n);
            });
        }
        return 0;
    }
    if (id == HOS_COOP_WRAP_PANE_BODY || id == HOS_COOP_WRAP_PANE_ALIAS) {
        // The texts of the two setters that skip 0041's call (patch 0042): a letter-animated pane's (x1 the text, w2
        // its length) and each pane with a matching alias (x1 a reference, as 0041's). The same log. Reads only.
        if (phase == 0 && CoopPlaytest::On()) {
            const auto read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
            if (id == HOS_COOP_WRAP_PANE_BODY) {
                CoopTalkText::TheLog().NoteText(x[0], x[1], x[2], CoopPlaytest::Frame(), read, id);
            } else {
                CoopTalkText::TheLog().Note(x[0], x[1], CoopPlaytest::Frame(), read, id);
            }
        }
        return 0;
    }
    if (id == HOS_COOP_WRAP_SOUND_EMIT) {
        // A sound the game starts (patch 0014): kept when it is on our own Link (coop_link_sound.h). Reads only.
        if (phase == 0) {
            CoopLinkSound::OnEmit(x[0], x[1], [](std::uint64_t a, void* out, std::size_t n) {
                return CoopRuntime::Read(a, out, n);
            });
        }
        return 0;
    }
    if (id == HOS_COOP_WRAP_EVENT_REQUEST) {
        // The game asks for an event: its name at the request (phase 0: x1 is the request), whether the manager took
        // it after the game's function (phase 1: x0). Both on the same game thread, one after the other.
        thread_local char name[sizeof(CoopLiveEvents::GameRequest::name)];
        thread_local bool named = false;
        if (phase == 0) {
            std::uint64_t text = 0;
            named = x[1] != 0 && CoopRuntime::Read(x[1] + CoopLiveEvents::addr::kArgName, &text, sizeof text) &&
                    text != 0 && CoopRuntime::Read(text, name, sizeof name - 1);
            name[sizeof name - 1] = 0;
            // One death cutscene per game (coop_boss_scenes.h refuse_deaths): a boss's death asked again after it
            // played here is not run; x0 0 is the manager's own "not taken" (phase 1 still reports it).
            if (named && CoopBossScenes::RefuseAsk(name)) {
                x[0] = 0;
                return 1;
            }
            // The Forest Temple's twisting hallway never turns under another player standing in it
            // (coop_hallway.h): the eye's turn is not run, and the lines say who is in the way.
            if (named && CoopHallway::RefuseAsk(name)) {
                x[0] = 0;
                return 1;
            }
        } else if (named) {
            named = false;
            CoopLiveEvents::TheGameRequests().Push(name, x[0] != 0);
        }
        return 0;
    }
    if (id == HOS_COOP_WRAP_BRAIN_VERB && phase == 0) {
        // Native boss sync: a boss's brain verb change (x0 BSA, w1 verb, w2 force). A copy's own is skipped
        // once the owner's verbs have come (1: the game's function does not run; x0 0 as a defined result, though
        // no caller reads one).
        if (!CoopBossNative::TheTable().OnBrainVerb(x[0], static_cast<std::int32_t>(x[1]), (x[2] & 1) != 0,
                                                    t_own_call)) {
            return 0;
        }
        x[0] = 0;
        return 1;
    }
    if (id == HOS_COOP_WRAP_BOSS_ORDER && phase == 0) {
        // Row 222: a battle manager's order reaches a sister (x0 its receive node, x1 the message: +4 type, +0x10
        // the payload). A copy's own manager's orders are skipped once the owner's have come (1: the handler does not
        // run; x0 1, what it always returns).
        CoopBossOrders::Table& t = CoopBossOrders::TheTable();
        std::uint32_t type = 0;
        if (x[1] == 0 || !CoopRuntime::Read(x[1] + CoopBossOrders::addr::kMessageType, &type, sizeof type) ||
            !CoopBossOrders::IsOrder(type) || !t.On()) {
            return 0;
        }
        std::uint64_t payload_at = 0;
        CoopBossOrders::Payload payload{};
        if (!CoopRuntime::Read(x[1] + CoopBossOrders::addr::kMessagePayload, &payload_at, sizeof payload_at) ||
            payload_at == 0 || !CoopRuntime::Read(payload_at, payload.data(), payload.size())) {
            return 0;
        }
        namespace oa = CoopBossOrders::addr;
        std::uint64_t context = 0, owner = 0, actor = 0;
        if (!CoopRuntime::Read(x[0] + oa::kNodeContext, &context, sizeof context) || context == 0 ||
            !CoopRuntime::Read(context + oa::kContextOwner, &owner, sizeof owner) || owner == 0 ||
            !CoopRuntime::Read(owner + oa::kOwnerActor, &actor, sizeof actor)) {
            actor = 0;
        }
        if (!t.OnReceive(x[0], actor, type, payload, t_own_call)) {
            return 0;
        }
        x[0] = 1;
        return 1;
    }
    if (id == HOS_COOP_WRAP_CRAWL_ROOM && phase == 0) {
        // 0.12.1 downed-move (patch 0046, hos_coop_crawl.h): the game's crawl asks whether the player (x0) has room
        // to stand up. For our downed child the answer is "no room" (the game's false), so its crawl goes on with
        // its own clips, stick movement and camera; everything else runs the game's own check.
        if (CoopDbno::Body::TheCrawl().NoRoom(x[0])) {
            x[0] = 0;
            return 1;
        }
        return 0;
    }
    if (id == HOS_COOP_WRAP_POSE_PLAY && phase == 0) {
        // 0.12.1 downed-move root fix (patch 0049, hos_coop_pose_play.h): the game plays an AS command (x0 the
        // request). On our downed Link held in a lying pose, slot 0, the game's own command is answered "played"
        // (its skip-if-same answer, 1) and not played, so the game's ground verb moves him in the pose. Fast: one
        // read and a compare for every other actor.
        const std::uint64_t held = CoopDbno::Body::TheCrawl().HeldActor();
        std::uint64_t actor = 0;
        if (held == 0 || !CoopRuntime::Read(x[0], &actor, sizeof actor) || actor != held) {
            return 0;
        }
        std::int32_t slot = -1;
        std::uint64_t text = 0;
        char name[32] = {};  // (31 read: the pose names and a horse's prefixes are shorter; a longer one is no pose)
        if (!CoopRuntime::Read(x[0] + 0x10, &slot, sizeof slot) || slot != 0 ||
            !CoopRuntime::Read(x[0] + 8, &text, sizeof text) || text == 0 ||
            !CoopRuntime::Read(text, name, sizeof name - 1)) {
            return 0;
        }
        if (CoopDbno::Body::TheCrawl().SkipPlay(actor, slot, std::string_view(name))) {
            x[0] = 1;
            return 1;
        }
        return 0;
    }
    if (id == HOS_COOP_WRAP_SHOOTER_PREPARE || id == HOS_COOP_WRAP_SHOOTER_SHOOT ||
        id == HOS_COOP_WRAP_SHOOTER_SLEEP) {
        // Row 286's probe: read and logged, nothing changed, both phases on the same game thread. A call inside a
        // call (a woken bullet's own) gets its own place: phase 1 ends the newest of its function.
        // Rows 222, 286 (coop_boss_shooter_table.h): the table answers a copy's own AI call and keeps what the
        // owner's game took; `seen` pairs the same way. A skipped call gets no phase 1: its entries end here.
        thread_local std::vector<std::pair<std::uint32_t, CoopShooterProbe::Call>> calls;
        thread_local std::vector<std::pair<std::uint32_t, CoopBossShooter::Seen>> seen;
        CoopShooterProbe::Probe& probe = CoopShooterProbe::TheProbe();
        CoopBossShooter::Table& table = CoopBossShooter::TheTable();
        const CoopShooterProbe::Reader read = [](std::uint64_t a, void* out, std::size_t n) {
            return CoopRuntime::Read(a, out, n);
        };
        if (phase == 0) {
            if (probe.On()) {
                if (calls.size() >= 8) {  // (a call that never ended)
                    calls.clear();
                }
                const CoopShooterProbe::Fn fn = id == HOS_COOP_WRAP_SHOOTER_PREPARE ? CoopShooterProbe::Fn::prepare
                                                : id == HOS_COOP_WRAP_SHOOTER_SHOOT ? CoopShooterProbe::Fn::shoot
                                                                                    : CoopShooterProbe::Fn::sleep;
                calls.emplace_back(id, CoopShooterProbe::Begin(read, fn, x[0], x[1], x[2], x[30], t_own_call));
            }
            if (table.On()) {
                if (seen.size() >= 8) {  // (a call that never ended)
                    seen.clear();
                }
                const CoopBossShooter::Kind kind = id == HOS_COOP_WRAP_SHOOTER_PREPARE ? CoopBossShooter::Kind::prepare
                                                   : id == HOS_COOP_WRAP_SHOOTER_SHOOT ? CoopBossShooter::Kind::shoot
                                                                                       : CoopBossShooter::Kind::sleep;
                CoopBossShooter::Seen s{};
                const CoopBossShooter::Answer a = table.OnCall(read, kind, x[0], x[1], x[2], t_own_call, &s);
                if (a.skip) {
                    if (probe.On() && !calls.empty() && calls.back().first == id) {
                        const CoopShooterProbe::Call call = std::move(calls.back().second);
                        calls.pop_back();
                        probe.End(read, call, a.x0, CoopRuntime::MainBase(), SteadyMs());
                    }
                    x[0] = a.x0;
                    return 1;
                }
                seen.emplace_back(id, s);
            }
            return 0;
        }
        if (!calls.empty() && calls.back().first == id) {
            const CoopShooterProbe::Call call = std::move(calls.back().second);
            calls.pop_back();
            probe.End(read, call, x[0], CoopRuntime::MainBase(), SteadyMs());
        }
        if (!seen.empty() && seen.back().first == id) {
            const CoopBossShooter::Seen s = seen.back().second;
            seen.pop_back();
            table.OnDone(s, x[0], SteadyMs());
        }
        return 0;
    }
    if (id == HOS_COOP_WRAP_ACTION_VERB && phase == 0) {
        // Native boss sync: a boss's action verb request (x0 BSA, x1 the request in guest memory).
        CoopBossNative::Table& t = CoopBossNative::TheTable();
        CoopDbno::Body::Crawl& crawl = CoopDbno::Body::TheCrawl();
        if (x[1] == 0 || (!t.ActionsOn() && !crawl.IsOurs(x[0]))) {
            return 0;
        }
        CoopBossNative::Request r{};
        if (!CoopRuntime::Read(x[1], r.data(), r.size())) {
            return 0;
        }
        // Down but not out (coop_dbno_body.h): while our Link is down it stays in the game's Crawl; the
        // game's other actions for it (a hit's reaction, walk, items) are skipped, answered as taken.
        if (crawl.Skip(x[0], r, t_own_call)) {
            x[0] = CoopBossNative::addr::kActionTaken;
            return 1;
        }
        if (!t.ActionsOn()) {
            return 0;
        }
        // Row 222 part 2: the plug's name, the move's variation ('Ice', a feint, ...), kept by an owner to send.
        std::optional<std::string> plug_name;
        if (r[CoopBossNative::addr::kReqHasPlug] == 1) {
            std::uint64_t plug = 0;
            std::memcpy(&plug, r.data() + CoopBossNative::addr::kReqPlug, sizeof plug);
            plug_name = CoopBossNative::VariantName(
                [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); }, plug);
            if (plug_name) {
                g_plug_read.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_plug_unread.fetch_add(1, std::memory_order_relaxed);
            }
        }
        std::optional<CoopBossNative::Request> renamed;
        std::uint64_t answer = CoopBossNative::addr::kActionTaken;
        std::int32_t brain_now = 0;  // the brain verb its AI asks in (its own asks are kept per verb)
        const bool brain_read =
            CoopRuntime::Read(x[0] + CoopBossNative::addr::kBsaBrainVerb, &brain_now, sizeof brain_now);
        if (!t.OnActionVerb(x[0], r, (x[2] & 1) != 0, t_own_call, std::move(plug_name), &renamed, &answer,
                            brain_read ? std::optional(brain_now) : std::nullopt)) {
            if (renamed) {
                // Row 222: a copy's own ask of the owner's move, run with the owner's variation: its plug and
                // has-plug set before the game reads the request (0x38..0x40).
                CoopRuntime::Write(x[1] + CoopBossNative::addr::kReqPlug,
                                   renamed->data() + CoopBossNative::addr::kReqPlug,
                                   CoopBossNative::addr::kReqHasPlug + 1 - CoopBossNative::addr::kReqPlug);
            }
            return 0;
        }
        // Skipped: the result its callers pass up is what the game gives for a request it took (0, its main path
        // main+0xc10f4; 2 and 3 are its refusals), so the asking node goes on as after its own request. A copy's own
        // start held (Twinrova hand-over) answers 3, the slot busy: its node asks again.
        x[0] = answer;
        return 1;
    }
    if (id == HOS_COOP_WRAP_TIME_SET && phase == 0) {
        // The game's own time change, with its exact time: the time sync's jump (coop_time_sync.h, GameSet).
        CoopTime::OnGameSet(static_cast<std::uint32_t>(x[1]), static_cast<std::uint32_t>(x[2]),
                            static_cast<std::int32_t>(x[3]));
        return 0;
    }
    if (id == HOS_COOP_WRAP_FLAG_APPLY && phase == 1) {
        // The game applied its queued flag writes (a save load's too): the pick held through a load is put back when
        // the game wrote another over it (coop_char_ages.h LoadHold; nothing when none is held).
        CoopHook::EmulatorMemory memory(g_system.ApplicationMemory(), CoopRuntime::MainBase());
        CoopCharAges::AtFlagsApplied(memory);
        return 0;
    }
    if (id == HOS_COOP_WRAP_FLAG_APPLY && phase == 0) {
        // The writes the game is about to apply, read from its queues (x0 = the manager): the flag watcher's events.
        CoopHook::EmulatorMemory memory(g_system.ApplicationMemory(), CoopRuntime::MainBase());
        CoopFlagNative::OnApply(memory, x[0]);
        // First proof: how often the game applies its queued flag writes (one line at the first, then every 10 s).
        static std::atomic<std::uint64_t> runs{0};
        static std::atomic<std::int64_t> said_ms{0};
        const std::uint64_t n = runs.fetch_add(1, std::memory_order_relaxed) + 1;
        const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();
        std::int64_t last = said_ms.load(std::memory_order_relaxed);
        if ((n == 1 || now - last >= 10'000) && said_ms.compare_exchange_strong(last, now)) {
            LOG_INFO(Core, "coop: world wraps: the game's flag applier ran {} times so far (x0 {:#x})", n, x[0]);
        }
    }
    return 0;
}

// The AI hold (native-plan row 6): a held component's calc bits become
// (bits & and) | or, from the set the Objects step published in our own
// memory (CoopHold::TheHostTable; no guest table at main+0x3c46400).
std::uint32_t Hold(std::uint64_t component, std::uint32_t bits) {
    if (!g_hold || component == 0) {
        return bits;
    }
    return CoopHold::TheHostTable().Apply(component, bits);
}

void Stop() {
    CoopPlaytest::Stop();   // (a held game let go first)
    CoopModsRuntime::Stop();
    HostMenu().Leave();  // our own server goes after our own client
    CoopHold::TheHostTable().Clear();  // (no copy stays held once co-op is off)
    if (g_on) {
        g_on = false;
        CoopLook::StopModule();
        CoopHook::Shutdown();
    }
}

void NoGameLook() {
    g_look_state = kLookOff;
}

void NoteDrawn(const HosCoopDraw& d) {
    g_drawn_w = d.width;
    g_drawn_h = d.height;
}

void DrawWithLook(const HosCoopDraw& d) {
    NoteDrawn(d);
    if (g_look_state.load() == kLookNotYet) {
        if (d.font && d.use_font && d.image && d.draw_image) {
            LoadLook(d);
        } else {
            g_look_state = kLookOff;
        }
    }
    CoopDraw::Draw(d, DrawScene(), g_look_state.load() == kLookOn ? g_look : CoopDraw::Look{});
}

// Local co-op's research probe (coop_local_probe.h, LICHII_COOP_LOCAL_PROBE=1): the renderer's layers and the
// controller manager, logged every 10 s, five times. Reads only.
void LocalProbe() {
    static const bool wanted = [] {
        const char* v = std::getenv("LICHII_COOP_LOCAL_PROBE");
        return v != nullptr && *v != 0 && std::strcmp(v, "0") != 0;
    }();
    static int said = 0;
    static std::chrono::steady_clock::time_point next{};
    const auto now = std::chrono::steady_clock::now();
    if (!wanted || said >= 5 || now < next) return;
    next = now + std::chrono::seconds(10);
    said++;
    const CoopLocalProbe::ReadFn read = [](std::uint64_t a, void* out, std::size_t n) {
        return CoopRuntime::Read(a, out, n);
    };
    for (const auto lines : {&CoopLocalProbe::Layers, &CoopLocalProbe::Controllers})
        for (const std::string& line : (*lines)(read, CoopRuntime::MainBase()))
            LOG_INFO(Core, "coop: local probe {}: {}", said, line);
}

// Split screen's second world layer, research (coop_local_layer.h): LICHII_COOP_LOCAL_LAYER=1 only, with an exe whose
// call() and scratch() are there (patch 0004). The calc thread's every-frame site.
void LocalLayer(void* ctx) {
    static const bool wanted = [] {
        const char* v = std::getenv("LICHII_COOP_LOCAL_LAYER");
        return v != nullptr && std::string_view(v) == "1";
    }();
    if (!wanted || g_api == nullptr || g_api->size < offsetof(HosCoopApi, scratch) + sizeof(g_api->scratch) ||
        g_api->scratch == nullptr || g_api->call == nullptr)
        return;
    static CoopLocalLayer::Second second{[] {
        CoopLocalLayer::Game g;
        g.main_base = [] { return CoopRuntime::MainBase(); };
        g.read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
        g.write = [](std::uint64_t a, const void* in, std::size_t n) { return CoopRuntime::Write(a, in, n); };
        g.log = [](const std::string& line) { LOG_INFO(Core, "coop: {}", line); };
        return g;
    }()};
    static void* frame_ctx = nullptr;   // (this call's: scratch() and call() take it)
    frame_ctx = ctx;
    static const bool set = [] {
        second.Set([](const std::vector<std::uint8_t>& b) { return g_api->scratch(frame_ctx, b.data(), b.size()); },
                   [](std::uint64_t f, const std::array<std::uint64_t, 8>& x) -> std::optional<std::uint64_t> {
                       HosCoopCall k{};
                       k.function = f;
                       for (std::size_t i = 0; i < 8; i++) k.x[i] = x[i];
                       std::uint64_t x0 = 0;
                       if (!g_api->call(frame_ctx, &k, &x0)) return std::nullopt;
                       return x0;
                   });
        return true;
    }();
    (void)set;
    std::optional<std::array<float, 3>> first;
    if (const auto p = CoopLocalGame::FirstLink()) first = std::array<float, 3>{(*p)[0], (*p)[1], (*p)[2]};
    second.Step(first);
}

// Row 277: the gear parts that are a material's parameter (coop_gear_params.h: the Master Sword's hilt), on your Link's
// equipped pieces (the look shown: Customize's choice at once) and each live puppet's (its player's look). Solo too:
// your Link alone. A cell of guest memory only once a colour was ever chosen (`cell` makes it).
void DressGearParams(const CoopCharColours::Game& g, const std::vector<CoopPuppet::Director::DressView>& views,
                     const CoopLook::Look& shown, const std::function<std::uint64_t()>& cell) {
    static CoopGearParams::Dresser dresser;
    static bool ever = false;   // a part's colour was chosen or received: from then on the dresser runs (puts back)
    std::vector<CoopGearParams::Target> targets;
    CoopGearParams::Target own;
    bool any = false;
    for (std::size_t i = 0; i < CoopGearParams::kPartCount; i++) {
        if (const auto part = CoopLook::PartOf(CoopGearParams::kParts[i].part)) own.colours[i] = shown[*part];
        any = any || own.colours[i].has_value();
    }
    for (const auto& v : views) {
        CoopGearParams::Target t;
        t.actor = v.actor;
        if (const auto parts = CoopLookShare::Of(v.number)) {
            for (const auto& p : *parts) {
                for (std::size_t i = 0; i < CoopGearParams::kPartCount; i++) {
                    if (p.part == CoopGearParams::kParts[i].part) t.colours[i] = p.colour;
                }
            }
        }
        for (const auto& c : t.colours) any = any || c.has_value();
        targets.push_back(t);
    }
    ever = ever || any;
    if (!ever) return;   // (nothing ever chosen: no reads, no calls)
    BridgeGuest guest;
    own.actor = CoopPose::FindPlayer(guest, CoopRuntime::MainBase());
    if (own.actor != 0) targets.push_back(own);
    if (targets.empty()) return;
    const std::uint64_t at = cell();
    if (at == 0) return;
    dresser.Frame(g, at, targets, SteadyMs(), [](const std::string& line) { LOG_INFO(Core, "coop: {}", line); });
}

// Character Select's Customize options on the puppets, co-op's own (coop_char_colours.h): each frame, the game's own
// model calls for each live puppet that is due. Solo nothing is live, so nothing is done.
void DressCharacters(void* ctx) {
    if (g_api == nullptr || g_api->call == nullptr) return;
    const auto views = CoopPuppet::DressTargets();
    static void* frame_ctx = nullptr;
    frame_ctx = ctx;
    static const CoopCharColours::Game game = [] {
        CoopCharColours::Game g;
        g.read = [](std::uint64_t a, void* out, std::size_t n) { return CoopRuntime::Read(a, out, n); };
        g.write = [](std::uint64_t a, const void* in, std::size_t n) { return CoopRuntime::Write(a, in, n); };
        g.call = [](std::uint64_t f, const std::array<std::uint64_t, 8>& x) -> std::optional<std::uint64_t> {
            HosCoopCall k{};
            k.function = f;
            for (std::size_t i = 0; i < 8; i++) k.x[i] = x[i];
            std::uint64_t x0 = 0;
            if (!g_api->call(frame_ctx, &k, &x0)) return std::nullopt;
            return x0;
        };
        return g;
    }();
    CoopCharColours::Game g = game;
    g.main_base = CoopRuntime::MainBase();
    std::vector<CoopCharColours::Target> targets;
    targets.reserve(views.size() + 1);
    for (const auto& v : views) {
        // (a puppet is made again as another actor on its player's age or character switch: CoopPuppet_Adult_C007)
        targets.push_back(CoopCharColours::PuppetTarget(v.actor, v.cell, v.mods_routine, v.actor_name,
                                                        {v.custom, v.eyes, v.hair}));
    }
    // (0.11.9: your own Link's eye colour, the look shown: the one chosen in Customize at once, a preview, else worn;
    // its cell of guest memory made once with the game's malloc, as a puppet's block)
    // (and, while the menu is up, the pause menu's model actor that is your Link: Character Select's Customize picture
    // is a model of the menu's own, so the eyes chosen showed on your Link in the field but not in the picture,
    // lane-0119 13:03. Your Link there: an actor of the field player's own actor name, while you are Link; never the
    // picker's other characters (review). Each its own cell: 0 your Link's, 1..4 the menu's slots)
    static std::array<std::uint64_t, 6> cells{};   // (5: the gear parameters' dresser, below)
    static std::array<bool, 6> cells_asked{};
    const auto cell = [&](std::size_t i) {
        if (cells[i] == 0 && !cells_asked[i]) {
            cells_asked[i] = true;
            HosCoopCall k{};
            k.function = CoopRuntime::MainBase() + CoopPuppet::addr::kMalloc;
            k.x[0] = CoopCharColours::kCellBytes;
            std::uint64_t x0 = 0;
            if (g_api->call(ctx, &k, &x0) && x0 != 0) {
                const std::array<std::uint8_t, CoopCharColours::kCellBytes> zero{};
                CoopRuntime::Write(x0, zero.data(), zero.size());
                cells[i] = x0;
            }
        }
        return cells[i];
    };
    const CoopLook::Look shown = CoopLook::Shown();
    // (npc-customize: Link's eyes and hair on your Link, the character parts on a character: CoopCharColours::ForModel.
    // A character is always a target, so its model is looked up for its Customize page's rows and its equipment kept
    // hidden; Link only with a colour to put on, as before.)
    const std::uint16_t character = CoopCharColours::ShownCharacter();
    const auto own = CoopCharColours::ForModel(character == 0, [&shown](std::uint8_t number) {
        const auto part = CoopLook::PartOf(number);
        return part ? shown[*part] : std::nullopt;
    });
    // (and Link once he has worn a colour: back to none, the wardrobe puts the game's own back. Without him as a
    // target it never saw the change, and Game's own colour left the last colour on him)
    static bool own_worn = false;
    // (look-link-cs: and his equipment Hidden, ForModel's custom on Link)
    own_worn = own_worn || own.eyes || own.hair || own.custom != 0;
    if (own_worn || character != 0) {
        // (the model key: the actor name and the character shown, ModelKey, so another pick or age is looked up anew;
        // `picture`: dressed only on the Customize screen, its rows are the page's)
        const auto dress = [&](std::uint64_t actor, std::size_t i, std::uint64_t model) {
            if (actor == 0 || cell(i) == 0) return;
            targets.push_back(CoopCharColours::OwnTarget(actor, cells[i], model, own, character, i == 0,
                                                         i != 0 && character != 0));
        };
        const auto name_of = [](std::uint64_t actor) {
            std::uint64_t at = 0;
            char text[64] = {};
            if (actor == 0 || !CoopRuntime::Read(actor + 0x260, &at, sizeof at) || at == 0) return std::string();
            if (!CoopRuntime::Read(at, text, sizeof text - 1)) return std::string();
            return std::string(text);
        };
        BridgeGuest guest;
        const std::uint64_t player = CoopPose::FindPlayer(guest, CoopRuntime::MainBase());
        const std::string link = player != 0 ? name_of(player) : std::string();
        dress(player, 0, CoopCharColours::ModelKey(link, character));
        if (CoopPauseTab::GameMenuUp() && player != 0) {
            static std::string said;
            std::string seen;
            const auto menu = CoopPauseTab::MenuActors();
            for (std::size_t s = 0; s < menu.size(); ++s) {
                if (menu[s].actor == 0) continue;
                const std::string name = name_of(menu[s].actor);
                seen += fmt::format("{}{}: {}", seen.empty() ? "" : ", ", s, name.empty() ? "?" : name);
                // (a character's picture only on Character Select's Customize screen, where the model shown is your
                // pick: the picker's own may show another character under the same name)
                if (CoopCharColours::MenuModelOfPlayer(name, link) &&
                    (character == 0 || CoopPauseTab::CharacterCustomizeOpen()))
                    dress(menu[s].actor, 1 + s, CoopCharColours::ModelKey(name, character) ^ menu[s].id);
            }
            if (seen != said) {   // (what the menu holds, once a change: which of them is your Link)
                said = seen;
                LOG_INFO(Core, "coop: look: the menu's models ({}); your Link's actor {}", seen.empty() ? "none" : seen, link);
            }
        }
    }
    DressGearParams(g, views, shown, [&] { return cell(5); });
    if (targets.empty()) return;
    CoopCharColours::TheWardrobe().Frame(g, targets, SteadyMs(),
                                         [](const std::string& line) { LOG_INFO(Core, "coop: {}", line); });
}

// Local co-op (coop_local_game.h): the exe's pads (patch 0016, hos_coop_pads.h) and the Player's wraps 16, 17 and 19.
// An exe without them: CoopLocal says NotSupported. LICHII_COOP_LOCAL=<pad slot> starts a second player on that pad
// at the first frame in play, without the menu (tests).
void UseLocalPads() {
#ifdef _WIN32
    const HMODULE exe = GetModuleHandleW(nullptr);
    const auto find = [exe](const char* name) { return reinterpret_cast<void*>(GetProcAddress(exe, name)); };
#else
    const auto find = [](const char* name) { return dlsym(RTLD_DEFAULT, name); };
#endif
    const auto info = reinterpret_cast<hos_coop_pad_info_fn>(find(HOS_COOP_PAD_INFO));
    const auto take = reinterpret_cast<hos_coop_pad_take_fn>(find(HOS_COOP_PAD_TAKE));
    CoopLocalGame::Deps deps;
    if (info && take) {
        deps.pads = [info] {
            std::vector<CoopLocalGame::Pad> pads;
            for (int slot = 1; slot <= 64; slot++) {   // (by place in SDL's list, from 1: patch 0016)
                char name[64] = {};
                std::uint32_t serial = 0;
                int npad = 0;
                const int there = info(slot, &serial, name, sizeof name, &npad);
                if (there < 0) break;
                if (there > 0) pads.push_back({slot, serial, name, npad});
            }
            return pads;
        };
        deps.take = [take](int slot, std::uint32_t serial, int npad) { return take(slot, serial, npad) != 0; };
    }
    deps.wraps = [] {
        return HasWrap(CoopPlayerInput::kWrapButtons) && HasWrap(CoopPlayerInput::kWrapSticks) &&
               HasWrap(CoopPlayerInput::kWrapInputOff) && HasWrap(CoopPlayerInput::kWrapControllerGet);
    };
    deps.main_base = [] { return CoopRuntime::MainBase(); };
    deps.slots = [] { return CoopPuppet::TheDirector().Slots(); };
    deps.read = [](std::uint64_t address, void* out, std::size_t size) { return CoopRuntime::Read(address, out, size); };
    deps.write = [](std::uint64_t address, const void* in, std::size_t size) {
        return CoopRuntime::Write(address, in, size);
    };
    deps.log = [](const std::string& line) { LOG_INFO(Core, "coop: {}", line); };
    LOG_INFO(Core, "coop: local co-op: {}", info && take ? "this Hookshot gives the module its pads (patch 0016)"
                                                          : "this Hookshot has no pads for local co-op (patch 0016)");
    CoopLocalGame::Install(std::move(deps));
    if (const char* slot = std::getenv("LICHII_COOP_LOCAL"); slot && *slot) {
        CoopLocalGame::AutoStart(std::atoi(slot));
    }
    if (const char* views = std::getenv("LICHII_COOP_LOCAL_VIEWS"); views && *views) {   // (tests: the split alone)
        CoopLocalGame::ForceViews(std::atoi(views));
        LOG_INFO(Core, "coop: local co-op: LICHII_COOP_LOCAL_VIEWS={}: the views split with no second player", views);
    }
}

}  // namespace CoopHookshot

extern "C" {
// The host's save for a guest (issue 167): longshot.exe's patch 0008 exports the account save's folder and a
// re-mount of it; with them a guest switches to the co-op folder when it joins (CoopSave, save::folder_choice).
// An exe without patch 0008: CoopSave as on a fixed folder (LICHII_COOP_HOST_SAVE, else the player's own).
void UseSaveSwitch() {
#ifdef _WIN32
    const HMODULE exe = GetModuleHandleW(nullptr);
    using RootFn = int (*)(char*, size_t);
    using RemountFn = int (*)(const char*);
    const auto root = reinterpret_cast<RootFn>(reinterpret_cast<void*>(GetProcAddress(exe, "hos_coop_fs_save_root")));
    const auto remount = reinterpret_cast<RemountFn>(reinterpret_cast<void*>(GetProcAddress(exe, "hos_coop_fs_save_remount")));
#else
    using RootFn = int (*)(char*, size_t);
    using RemountFn = int (*)(const char*);
    const auto root = reinterpret_cast<RootFn>(dlsym(RTLD_DEFAULT, "hos_coop_fs_save_root"));
    const auto remount = reinterpret_cast<RemountFn>(dlsym(RTLD_DEFAULT, "hos_coop_fs_save_remount"));
#endif
    if (!root || !remount) {
        LOG_INFO(Core, "coop: save: this Hookshot has no save-folder switch (patch 0008): the host's save goes only to "
                       "a game started with LICHII_COOP_HOST_SAVE=1");
        return;
    }
    CoopSave::SetSwitch(
        [root] {
            char buf[1024] = {};   // (save-crash-0118: read up to its end inside the buffer, never past it)
            return root(buf, sizeof buf) ? std::string(CoopBoundary::EndedText(buf, sizeof buf)) : std::string();
        },
        [remount](const std::string& dir) { return remount(dir.c_str()) != 0; });
}

// Player report 120: patch 0025's opens of a slot's progress.sav, so the host sends the slot its game plays (not the
// newest file on disk). An exe without it: the newest slot, as before.
void UseSlotOpens() {
    using OpenedFn = unsigned (*)(char*, size_t, int*);
#ifdef _WIN32
    const auto opened = reinterpret_cast<OpenedFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "hos_coop_fs_save_slot_opened")));
#else
    const auto opened = reinterpret_cast<OpenedFn>(dlsym(RTLD_DEFAULT, "hos_coop_fs_save_slot_opened"));
#endif
    if (!opened) {
        LOG_INFO(Core, "coop: save: this Hookshot does not say which save slot the game opens (patch 0025): the host "
                       "sends its newest slot");
        return;
    }
    CoopSave::SetSlotOpens(opened);   // (also read at each load-window mark, on the game's thread)
}

COOP_HOOKSHOT_EXPORT int hos_coop_start(const HosCoopApi* api) {
    static CoopBoundary::Site site{"hos_coop_start"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        if (!CoopHookshot::Start(api)) return 0;
        UseSaveSwitch();
        UseSlotOpens();
        CoopSave::SetSaveRequests(true);   // (RequestHostAutoSave, on the calc thread's call site)
        CoopHookshot::UseLocalPads();
        return 1;
    });
}
COOP_HOOKSHOT_EXPORT void hos_coop_frame(void* ctx, int site) {
    static CoopBoundary::Site guard_site{"hos_coop_frame"};
    CoopExportGuard::Guard(guard_site, [&] {
        CoopHookshot::Frame(ctx, site);
    });
}
COOP_HOOKSHOT_EXPORT uint32_t hos_coop_hold(uint64_t component, uint32_t bits) {
    static CoopBoundary::Site site{"hos_coop_hold"};
    return CoopExportGuard::Guard(site, static_cast<uint32_t>(bits), [&]() -> uint32_t {
        return CoopHookshot::Hold(component, bits);
    });
}
// The world hooks (hos_coop_world.h): Hookshot's host/coop_world.c, on the game's threads.
COOP_HOOKSHOT_EXPORT int hos_coop_world(uint32_t event, uint64_t a, uint64_t b) {
    static CoopBoundary::Site site{"hos_coop_world"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        return CoopHookshot::World(event, a, b);
    });
}
COOP_HOOKSHOT_EXPORT int hos_coop_wrap(uint32_t id, int phase, uint64_t* x, void* ctx) {
    static CoopBoundary::Site site{"hos_coop_wrap"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        return CoopHookshot::Wrap(id, phase, x, ctx);
    });
}
COOP_HOOKSHOT_EXPORT void hos_coop_stop(void) {
    static CoopBoundary::Site site{"hos_coop_stop"};
    CoopExportGuard::Guard(site, [&] {
        CoopHookshot::Stop();
    });
}
// Host and Join from the game's menu (hos_coop_host.h).
COOP_HOOKSHOT_EXPORT int hos_coop_host_start(const HosCoopHostOptions* options) {
    static CoopBoundary::Site site{"hos_coop_host_start"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        return CoopHookshot::HostMenu().HostStart(options);
    });
}
COOP_HOOKSHOT_EXPORT int hos_coop_join(const HosCoopJoinOptions* options) {
    static CoopBoundary::Site site{"hos_coop_join"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        return CoopHookshot::HostMenu().Join(options);
    });
}
COOP_HOOKSHOT_EXPORT int hos_coop_host_answer(const char* answer_code) {
    static CoopBoundary::Site site{"hos_coop_host_answer"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        return CoopHookshot::HostMenu().Answer(answer_code);
    });
}
COOP_HOOKSHOT_EXPORT void hos_coop_leave(void) {
    static CoopBoundary::Site site{"hos_coop_leave"};
    CoopExportGuard::Guard(site, [&] {
        CoopHookshot::HostMenu().Leave();
    });
}
COOP_HOOKSHOT_EXPORT void hos_coop_status(HosCoopStatus* out) {
    static CoopBoundary::Site site{"hos_coop_status"};
    CoopExportGuard::Guard(site, [&] {
        CoopHookshot::HostMenu().Status(out);
    });
}
// Typing (hos_coop_api.h): the chat box. (Linux: Longshot calls these on the window's thread, SDL's: a Ctrl+V there
// reads SDL's clipboard, which needs no wl-paste or xclip: CoopClipboard::OnSdlThread)
// Where co-op's menu is now (coop_menu_tabs_rule.h, f1-event-c31): our - tabs in the pack, the - menu can open now, one
// of co-op's tabs shows in Longshot's menu. Any thread: atomics and reads only, no co-op lock (hos_coop_menu_tabs runs
// under Longshot's menu lock, row 279).
static CoopMenuTabsRule::Now MenuNow() {
    static const bool has_pack = CoopPuppetConfig::HasPack();   // (a folder scan: once; the pack is set at the start)
    CoopMenuTabsRule::Now now;
    now.minus_tabs = has_pack && !CoopPauseTab::NoMinusTabs();
    now.minus_now = now.minus_tabs && CoopPauseTab::MinusMenuNow();
    now.tab_shown = CoopPause::TabShown();
    return now;
}
// A key press, the window's (hos_coop_key, on the window's thread) or the play-test bridge's `key` (CoopHookshot::
// BridgeKey, on the bridge's thread): the same steps and lines either way. 1: co-op took it. Only hos_coop_key marks
// its thread as SDL's (Linux: the window's event loop, where SDL's clipboard may be read); the bridge's thread is not
// SDL's, so a paste there takes the tools' way (review-c123-13 17:56, port-g7 follow-up).
static int KeyPress(uint32_t vk, bool repeat) {
    if (CoopHookshot::TypeKey(vk, repeat) || CoopModsRuntime::Key(vk, repeat)) return 1;
    // F1 in free play: co-op's home, the - menu's Multiplayer tab (menu-polish-01113, owner O5). Where the - menu
    // cannot open now (an event, a load, the title, a file screen: f1-event-c31) Longshot's menu, on co-op's
    // Multiplayer tab (its tabs are there then, hos_coop_menu_tabs), as in 0.11.12: Leave from a stuck event. F1
    // with one of co-op's tabs showing, or an old pack without our - tabs: Longshot's own F1. Not in original game.
    if (vk == 0x70 /* F1 */ && !repeat && g_on) {
        const CoopMenuTabsRule::Now now = MenuNow();
        CoopMenuTabsRule::F1 route = CoopMenuTabsRule::OnF1(now);
        if (route == CoopMenuTabsRule::F1::kMinusMenu) {
            if (CoopPauseTab::OpenMultiplayer()) {
                LOG_INFO(Core, "coop: F1: the - menu's Multiplayer tab (free play)");
                CoopPause::WantClose();
                return 1;
            }
            route = now.tab_shown ? CoopMenuTabsRule::F1::kLongshot : CoopMenuTabsRule::F1::kLongshotOnCoop;   // (refused now)
        }
        if (route == CoopMenuTabsRule::F1::kLongshotOnCoop) {
            LOG_INFO(Core, "coop: F1: Longshot's menu on co-op's Multiplayer tab (our - menu cannot open now)");
            CoopPause::WantMultiplayerTab();
            return 1;
        }
        LOG_INFO(Core, "coop: F1: Longshot's own ({})", now.minus_tabs ? "co-op's tab shows: it closes" : "no - tabs of ours");
    }
    return 0;
}
COOP_HOOKSHOT_EXPORT int hos_coop_key(uint32_t vk, int repeat) {
    static CoopBoundary::Site site{"hos_coop_key"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
    #ifndef _WIN32
        const CoopClipboard::OnSdlThread sdl_thread;
    #endif
        return KeyPress(vk, repeat != 0);
    });
}
COOP_HOOKSHOT_EXPORT int hos_coop_char(uint32_t ch) {
    static CoopBoundary::Site site{"hos_coop_char"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        return CoopHookshot::TypeChar(ch) ? 1 : 0;
    });
}
COOP_HOOKSHOT_EXPORT int hos_coop_keys_taken(void) {
    static CoopBoundary::Site site{"hos_coop_keys_taken"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        return CoopHookshot::KeysTaken() ? 1 : 0;
    });
}
// The Discord status's second line (Longshot patch 0026, host/discord_rpc.c, every 5 s on its own thread):
// "Co-op: N players" in a session, "Solo" with co-op idle. 1 = written (cut to cap, always ended); 0 = none
// (nothing thrown crosses into Longshot's C: a failed copy of the session is no line, Longshot's own stays).
COOP_HOOKSHOT_EXPORT int hos_coop_discord_state(char* out, size_t cap) noexcept {
    if (out == nullptr || cap == 0 || g_original_on) {
        return 0;   // (original game: Longshot's own line)
    }
    out[0] = '\0';
    try {
        const CoopLobby::Seen seen = CoopLobby::Now();
        const std::string line = CoopLobbyTab::DiscordState(seen.connected, seen.players.size());
        const size_t n = line.size() < cap - 1 ? line.size() : cap - 1;
        std::memcpy(out, line.data(), n);
        out[n] = '\0';
        return 1;
    } catch (...) {
        return 0;
    }
}
// Longshot patch 0028 (player report 133), host/oot_autoload.c: its Skip title screen asks before it chooses one of the
// title's buttons itself (the game's own Button::Decide, no pressed buttons), from its game thread at a menu screen's
// update. 1 = not now: co-op is hosting, joining or in a session, or the host's save is on its way (the choice is the
// player's then; CoopTitleMenu::SkipTitleHold). 0 with co-op off: solo play skips the title as Longshot does. Nothing
// thrown crosses into Longshot's C; a failure holds.
COOP_HOOKSHOT_EXPORT int hos_coop_title_hold(void) noexcept {
    if (!g_on) {
        return 0;
    }
    try {
        const char* why = CoopHookshot::TitleHoldWhy();
        static std::atomic<const char*> said{nullptr};
        if (why != nullptr && said.exchange(why) != why) {
            LOG_INFO(Core, "coop: title: Skip title screen waits: {}", why);
        }
        return why != nullptr ? 1 : 0;
    } catch (...) {
        return 1;
    }
}
// The mouse (hos_coop_api.h): the cheats panel's clicks.
COOP_HOOKSHOT_EXPORT int hos_coop_mouse(float fx, float fy, int event) {
    static CoopBoundary::Site site{"hos_coop_mouse"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        return CoopHookshot::Mouse(fx, fy, event) ? 1 : 0;
    });
}
// The game's menu mouse held off while co-op presses the menu's buttons for the player (coop_auto_press.h; Longshot
// patch 0030's oot_menu.c asks each menu frame): 1 = held, the mouse neither points nor clicks.
COOP_HOOKSHOT_EXPORT int hos_coop_menu_mouse_held(void) noexcept {
    static CoopBoundary::Site site{"hos_coop_menu_mouse_held"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        return g_on && CoopTitle::MenuMouseHeld() ? 1 : 0;
    });
}
// Drawing over the picture (hos_coop_draw.h), on Hookshot's render thread.
// The title menu (hos_coop_api.h, patch 0004): the game's title screen and the pad first.
COOP_HOOKSHOT_EXPORT void hos_coop_title(void* ctx, uint64_t screen, int event) {
    static CoopBoundary::Site site{"hos_coop_title"};
    CoopExportGuard::Guard(site, [&] {
        if (g_on) {
            CoopTitle::OnTitle(ctx, screen, event);
        }
    });
}
// The pause menu's Multiplayer tab (coop_pause_tab.h, patch 0030): every menu screen's update, on the game's threads,
// and the page the mouse's tab strip counts (4 while our page shows, else -1).
// The code mods' menu screens (coop_mod_api.h version 2) from the same call: read only when a mod asked for them.
COOP_HOOKSHOT_EXPORT void hos_coop_game_screen(void* ctx, uint64_t screen) noexcept {
    static CoopBoundary::Site site{"hos_coop_game_screen"};
    CoopExportGuard::Guard(site, [&] {
        if (g_on) {
            CoopPauseTab::OnScreen(ctx, screen);
            {
                // (row 299: the host's "World runs in menus" lets the menu's holds go; a test's `menu hold release N`)
                const auto menu_state = CoopMapPage::MenuState();
                CoopPauseWorld::OnScreen(ctx, (menu_state && menu_state->menu_open) || CoopPauseTab::GameMenuUp());
            }
            try {
                // (not while our Multiplayer page is in front: the screen behind it is not the player's, and a mod's
                // choose_focused would press the game's button under our page)
                // (nor while co-op's rows are on the title's buttons: a mod's Continue is co-op's row there)
                if (CoopPauseTab::MousePage() != 4 && !CoopTitle::RowsOurs()) {
                    CoopModsRuntime::Screen(ctx, screen);
                }
            } catch (...) {
                // (the mods' screens skip this update: the game's own goes on)
            }
        }
    });
}
COOP_HOOKSHOT_EXPORT int hos_coop_game_pause_page(void) noexcept {
    static CoopBoundary::Site site{"hos_coop_game_pause_page"};
    return CoopExportGuard::Guard(site, static_cast<int>(-2), [&]() -> int {
        return g_on ? CoopPauseTab::MousePage() : -2;   // (-2: no page of ours: the mouse keeps the fifth tab out)
    });
}
// The Multiplayer tab of Hookshot's menu (hos_coop_menu.h, patch 0012), from Hookshot's menu tick and present thread.
// (original game, coop_original.h: one tab, the switch alone, from Hookshot's threads: g_original_mutex)
COOP_HOOKSHOT_EXPORT int hos_coop_menu_tabs(void) {
    static CoopBoundary::Site site{"hos_coop_menu_tabs"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        // (co-op's home is the game's - menu; co-op's tabs in Longshot's F1 menu whenever it cannot open now (an event,
        // a load, the title, a file screen: f1-event-c31), while one of them shows, and with no pack or a pack without
        // our - tabs: coop_menu_tabs_rule.h. Original game: its one tab, the way back)
        if (!g_on) return g_original_on ? 1 : 0;
        const int tabs = CoopMenuTabsRule::LongshotTabs(MenuNow(), CoopPause::Tabs());
        static std::atomic<int> told{-1};
        if (told.exchange(tabs) != tabs) {
            LOG_INFO(Core, "coop: Longshot's menu: {} of co-op's tabs ({})", tabs,
                     tabs > 0 ? "our - menu cannot open now, or one shows" : "co-op's home is the - menu");
        }
        return tabs;
    });
}
COOP_HOOKSHOT_EXPORT const char* hos_coop_menu_tab_name(int tab) {
    static CoopBoundary::Site site{"hos_coop_menu_tab_name"};
    return CoopExportGuard::Guard(site, static_cast<const char*>(nullptr), [&]() -> const char* {
        if (!g_on) return g_original_on && tab == 0 ? g_original.Name() : nullptr;
        return CoopPause::TabName(tab);
    });
}
COOP_HOOKSHOT_EXPORT int hos_coop_menu_page(int tab, HosCoopMenuPage* out) {
    static CoopBoundary::Site site{"hos_coop_menu_page"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        if (!g_on) {
            if (!g_original_on || tab != 0) return 0;
            std::lock_guard lock(g_original_mutex);
            return g_original.Page(out) ? 1 : 0;
        }
        return CoopPause::Page(tab, out) ? 1 : 0;
    });
}
COOP_HOOKSHOT_EXPORT int hos_coop_menu_input(int tab, int event, int arg, int arg2) {
    static CoopBoundary::Site site{"hos_coop_menu_input"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        if (!g_on) {
            if (!g_original_on || tab != 0) return 0;
            std::lock_guard lock(g_original_mutex);
            return g_original.Input(event, arg, arg2) ? 1 : 0;
        }
        return CoopPause::Input(tab, event, arg, arg2) ? 1 : 0;
    });
}
COOP_HOOKSHOT_EXPORT int hos_coop_menu_wanted(void) {
    static CoopBoundary::Site site{"hos_coop_menu_wanted"};
    return CoopExportGuard::Guard(site, static_cast<int>(HOS_COOP_MENU_WANT_NOTHING), [&]() -> int {
        return g_on ? CoopPause::Wanted() : HOS_COOP_MENU_WANT_NOTHING;
    });
}
// The picture beside the tab's page (hos_coop_menu_picture.h, patch 0019): the Customize page's Link.
COOP_HOOKSHOT_EXPORT int hos_coop_menu_picture(int tab, HosCoopMenuPicture* out) {
    static CoopBoundary::Site site{"hos_coop_menu_picture"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        return g_on && CoopPause::Picture(tab, out) ? 1 : 0;
    });
}
// The player camera's angles (hos_coop_turn.h, patch 0040): turned with the Customize page's orbit, so the game's
// own collision places the eye.
COOP_HOOKSHOT_EXPORT int hos_coop_turn(float* yaw, float* pitch, int state) {
    static CoopBoundary::Site site{"hos_coop_turn"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        // (also with co-op off: a camera still turned is put back once)
        return CoopLookView::Turn(yaw, pitch, state, g_on) ? 1 : 0;
    });
}
// The gameplay camera (hos_coop_view.h, patch 0019): around your own Link while the Customize page shows him.
COOP_HOOKSHOT_EXPORT int hos_coop_view(double eye[3], double target[3]) {
    static CoopBoundary::Site site{"hos_coop_view"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        if (!g_on || eye == nullptr || target == nullptr) {
            return 0;
        }
        if (CoopLookView::Camera(eye, target)) {
            return 1;
        }
        // Down but not out, spectating (coop_dbno_game.h): the game's own camera, as it follows our Link, moved
        // onto the player we watch, so it follows them the same way (the right stick still turns it).
        if (const auto off = CoopDbno::SpectateOffset()) {
            for (int i = 0; i < 3; ++i) {
                eye[i] += (*off)[i];
                target[i] += (*off)[i];
            }
            return 1;
        }
        return 0;
    });
}
static_assert(CoopPadOrder::kHoldAll == COOP_MOD_HOLD_ALL);
COOP_HOOKSHOT_EXPORT uint64_t hos_coop_pad(uint64_t buttons, int32_t stick[4]) noexcept {
    if (!g_on || stick == nullptr) {
        return buttons;
    }
    // The order (coop_pad_order.h): our pause page first (it takes the pad while it shows), then the code mods see what
    // it passed on and their holds come off (coop_mod_api.h hold_input), then the title's watch and down but not out on
    // what the game gets. Nothing thrown leaves this C export: the game then gets the pad as it came.
    const uint64_t came = buttons;
    int32_t was[4] = {stick[0], stick[1], stick[2], stick[3]};
    try {
        // A play-test `move` (coop_playtest.h): its stick, from the world yaw and this frame's camera, is the pad's.
        CoopPlaytest::PadMove(stick);
        const uint64_t got = CoopPadOrder::Run(
            buttons, stick,
            [](uint64_t b, int32_t* s) {
                const uint64_t o = CoopPauseTab::OnPad(b, s);
                if (o != 0) CoopHookshot::g_test_st_page = o;
                return o;
            },
            [](uint64_t b) {
                CoopTitle::NotePlayerPad(b);   // (the player's own A on the title, before a mod's hold takes it)
                CoopModsRuntime::NotePad(b);
                // Break free (row 312, coop_lift_player.h Victim::Struggle): lifted by another player, mashing the face
                // buttons or pushing the stick about frees our Link. (Its input check is off then, so the game reads none.)
                namespace pt = CoopPauseTab;
                CoopLiftPlayerGame::NotePad(b & (pt::kPadA | pt::kPadB | pt::kPadX | pt::kPadY | pt::kPadStickUp |
                                                 pt::kPadStickDown | pt::kPadStickLeft | pt::kPadStickRight));
            },
            [] {
                const uint64_t h = CoopTitle::RowsOurs() || CoopTitle::FocusOffFirst() ? std::uint64_t{0}
                                                                                       : CoopModsRuntime::HeldButtons();
                if (h != 0) CoopHookshot::g_test_st_hold = h;
                return h;
            },
            [](uint64_t b, int32_t* s) {
                const uint64_t o = CoopTitle::OnPad(b, s);
                if (b != 0) CoopHookshot::g_test_st_title = o | (b << 32);   // (in high, out low)
                return o;
            },
            [](uint64_t b, int32_t* s) {
                // Down but not out (coop_dbno_game.h): a downed Link crawls and uses nothing; beside one, A revives.
                CoopDbno::Pad pad{b, {s[0], s[1], s[2], s[3]}};
                pad = CoopDbno::OnPad(pad);
                if (b != 0) CoopHookshot::g_test_st_dbno = pad.buttons | (b << 32);   // (in high, out low)
                for (int i = 0; i < 4; ++i) {
                    s[i] = pad.stick[i];
                }
                return pad.buttons;
            });
        // (a test's view, LICHII_COOP_TEST_STATE: the last pad that came with buttons, and what the game got of it)
        if (came != 0) CoopHookshot::g_test_pad_came = came, CoopHookshot::g_test_pad_got = got;
        return got;
    } catch (...) {
        for (int i = 0; i < 4; ++i) {
            stick[i] = was[i];
        }
        return came;
    }
}
COOP_HOOKSHOT_EXPORT int hos_coop_draw_wanted(void) {
    static CoopBoundary::Site site{"hos_coop_draw_wanted"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        return g_on && (CoopDraw::Wanted(CoopHookshot::DrawScene()) || CoopModsRuntime::OverlayWanted())
                   ? 1
                   : 0;
    });
}
COOP_HOOKSHOT_EXPORT void hos_coop_draw(const HosCoopDraw* d) {
    static CoopBoundary::Site site{"hos_coop_draw"};
    CoopExportGuard::Guard(site, [&] {
        if (g_on && d != nullptr) {
            static std::atomic<bool> said{false};
            if (!said.exchange(true)) {
                LOG_INFO(Core, "coop: drawing over Hookshot's picture: chat lines, name tags ({}x{}); plain look: "
                         "this Hookshot has no hos_coop_draw2 (build it again with our patch for the game look)",
                         static_cast<int>(d->width), static_cast<int>(d->height));
            }
            CoopHookshot::NoGameLook();
            CoopHookshot::NoteDrawn(*d);
            // An older Hookshot's table ends at text_width: the rest stays empty.
            HosCoopDraw t{};
            t.width = d->width;
            t.height = d->height;
            t.user = d->user;
            t.rect = d->rect;
            t.circle = d->circle;
            t.triangle = d->triangle;
            t.text = d->text;
            t.text_width = d->text_width;
            CoopDraw::Draw(t, CoopHookshot::DrawScene());
            CoopModsRuntime::Overlay(t);
        }
    });
}
// The game's camera for the name tags (hos_coop_draw.h), on the thread that records Hookshot's draws: published as
// the emulator's renderer does (lichii/hook/coop_camera_track.h), the largest scene target of a frame. The game's
// clip y runs up, as on the emulator ("clip y up"); LICHII_COOP_CAMERA_FLIP_Y turns the markers over if not.
COOP_HOOKSHOT_EXPORT int hos_coop_camera_wanted(uint64_t frame, float width, float height) {
    static CoopBoundary::Site site{"hos_coop_camera_wanted"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int {
        return g_on && CoopCamera::Wanted() && CoopCamera::Better(frame, width, height) ? 1 : 0;
    });
}
COOP_HOOKSHOT_EXPORT void hos_coop_camera(uint64_t frame, const float* block, float width, float height) {
    static CoopBoundary::Site site{"hos_coop_camera"};
    CoopExportGuard::Guard(site, [&] {
        if (!g_on || !CoopHookshot::CameraBlockOk(block, width, height)) {
            return;
        }
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            LOG_INFO(Core, "coop: Hookshot hands co-op the game's camera ({}x{} scene): name tags on",
                     static_cast<int>(width), static_cast<int>(height));
        }
        CoopCamera::Publish(frame, block + CoopHookshot::kCameraMatrix, width, height, false);
    });
}
// hos_coop_draw3 and hos_coop_draw2 (hos_coop_draw.h): the scene in the game look.
static void DrawGameLook(const HosCoopDraw& d) {
    static std::atomic<bool> said{false};
    if (!said.exchange(true)) {
        LOG_INFO(Core, "coop: drawing over Hookshot's picture: chat lines, name tags ({}x{}){}", static_cast<int>(d.width),
                 static_cast<int>(d.height), d.draw_image_turned ? "" : "; no turned pictures (hos_coop_draw2)");
    }
    CoopHookshot::DrawWithLook(d);
    CoopModsRuntime::Overlay(d);
}
COOP_HOOKSHOT_EXPORT void hos_coop_draw3(const HosCoopDraw* d) {
    static CoopBoundary::Site site{"hos_coop_draw3"};
    CoopExportGuard::Guard(site, [&] {
        if (g_on && d != nullptr) {
            DrawGameLook(*d);
        }
    });
}
// A Hookshot from before draw_image_turned calls this one: its table ends at draw_image (the rest left empty here;
// the maps then show names only).
COOP_HOOKSHOT_EXPORT void hos_coop_draw2(const HosCoopDraw* d) {
    static CoopBoundary::Site site{"hos_coop_draw2"};
    CoopExportGuard::Guard(site, [&] {
        if (g_on && d != nullptr) {
            HosCoopDraw t{};
            t.width = d->width;
            t.height = d->height;
            t.user = d->user;
            t.rect = d->rect;
            t.circle = d->circle;
            t.triangle = d->triangle;
            t.text = d->text;
            t.text_width = d->text_width;
            t.font = d->font;
            t.use_font = d->use_font;
            t.image = d->image;
            t.draw_image = d->draw_image;
            DrawGameLook(t);
        }
    });
}
}

namespace CoopHookshot {
// (port-g7: f1-event C3-1 as a bridge test; B #137: any host key) The play-test bridge's `key`: the steps of the
// window's own entry (KeyPress, as hos_coop_key runs them for Longshot's WM_KEYDOWN: ls32-src/host/display.c, a key
// co-op takes never reaches Longshot), so what it does and logs is a real key press's (F1's route, F6's Players, the
// chat key). Not hos_coop_key itself: that marks its thread as SDL's on Linux, and this is the bridge's thread.
bool BridgeKey(std::uint32_t vk) {
    static CoopBoundary::Site site{"bridge_key"};
    return CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int { return KeyPress(vk, false); }) != 0;
}
std::size_t BridgeChars(std::string_view text) {
    static CoopBoundary::Site site{"bridge_chars"};
    std::size_t taken = 0;
    for (const char ch : text) {
        const auto byte = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
        taken += CoopExportGuard::Guard(site, static_cast<int>(0), [&]() -> int { return TypeChar(byte) ? 1 : 0; });
    }
    return taken;
}
}  // namespace CoopHookshot
