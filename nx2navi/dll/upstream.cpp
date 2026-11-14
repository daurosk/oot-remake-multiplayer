// SPDX-License-Identifier: GPL-3.0-or-later
// upstream.hpp: lichii/hook's co-op, unchanged, driven from NX2navi's plugin
// interface. Modelled on hookshot/src/coop_hookshot.cpp.
#include "upstream.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

#include "common/logging.h"
#include "coop_callers.h"
#include "coop_counters.h"
#include "coop_game_build.h"
#include "coop_hold.h"
#include "coop_hook.h"
#include "coop_objects_sync.h"
#include "coop_puppet.h"
#include "coop_pose.h"
#include "coop_world.h"
#include "coop_puppet_config.h"
#include "coop_puppet_sound.h"
#include "coop_runtime.h"
#include "coop_save.h"
#include "coop_svc.h"
#include "coop_teleport_game.h"
#include "core/core.h"
#include "objects/anim_probe.hpp"
#include "objects/brain.hpp"
#include "objects/hold.hpp"
#include "objects/world_probe.hpp"
#include "overlay.hpp"
#include "puppet_fx.hpp"
#include "save_sync.hpp"
#include "teleport.hpp"
#include "third_party/plugin_api.h"

namespace CoopNx2navi::Up {
namespace {

constexpr std::uint64_t kFrameStep = 0x3b4e300;           // coop_svc.h kHooks: the function both sites call
constexpr std::uint64_t kFrameStepReturn = 0x6e70fc;      // the every-frame call's return address
constexpr std::uint64_t kFrameStepReturnRare = 0x6e6ddc;  // the rare one's
constexpr int kTeleportOrder = 55;                        // this port's teleport caller, after the hook's movers

std::atomic<const Nx2PluginApi*> g_api{nullptr};
std::atomic<std::uint64_t> g_main{0};
void (*g_log)(const char*) = nullptr;
Hooks g_hooks;
std::filesystem::path g_dir;  // what the hook takes for the game's folder (CoopRuntime::GameDir)
std::string g_build;
bool g_configured = false;
bool g_hold3 = false;
std::atomic<bool> g_on{false};
std::atomic<bool> g_link_alive{true};
std::atomic<bool> g_broken{false};
int g_frame_handle = 0, g_step_handle = 0, g_counter_handle = 0;
std::atomic<std::uint64_t> g_ring{0};  // the platform counters' ring (coop_counters.h), kept here
Core::System g_system;
std::atomic<std::uint64_t> g_frames{0}, g_steps{0}, g_calls{0}, g_frame_us{0}, g_step_us{0}, g_worst_us{0};
std::atomic<std::uint64_t> g_worst_frame_us{0};  // the longest OnFrame since the last status line
std::atomic<std::uint64_t> g_ours_us{0};         // of this OnFrame, the time in the port's own frame (hooks.frame)

// Inside a frame or hook callback a page of the game's memory is asked for once (map_range: a pointer that is
// good until the callback returns, NX2navi docs/plugins.md "Memory") and every read in it is then a copy: the
// hook reads some 40,000 small values a frame (its flag watcher and its walk of the actors), and the
// interface's read checks the game's mapping at each call. Writes go through the interface and show in the
// mapped page at once (it is the game's own memory).
struct PageCache {
    static constexpr std::size_t kPages = 2048;
    struct Page {
        std::uint64_t number = ~std::uint64_t{0};
        std::uint64_t pass = 0;
        const std::uint8_t* at = nullptr;
    };
    bool active = false;
    std::uint64_t pass = 0;
    Page pages[kPages];
};
thread_local PageCache t_pages;
// OFF unless OOT_COOP_PAGE_CACHE=1 since run wd1 (2026-11-13): the host's game ended with an access violation in
// this cache's copy, seven seconds into the Forest Temple's load, inside the present-time frame. A pointer
// map_range gave is only good while the game's mapping stands, and other threads of the game free and map memory
// while the picture is presented; the interface's own read asks for the mapping's generation at every call and
// this cache does not (a plugin cannot). Every read is the interface's own again.
bool PageCacheOn() {
    static const bool on = [] {
        const char* e = std::getenv("OOT_COOP_PAGE_CACHE");
        return e != nullptr && e[0] == '1';
    }();
    return on;
}
struct InCallback {
    InCallback() {
        t_pages.active = PageCacheOn();
        ++t_pages.pass;
    }
    ~InCallback() { t_pages.active = false; }
};

std::uint64_t SteadyMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
std::uint64_t SteadyUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void SetEnv(const char* name, const std::string& value) { _putenv_s(name, value.c_str()); }
bool EnvSet(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != 0;
}
// A switch of the settings, unless the environment already says (a test).
void Default(const char* name, const std::string& value) {
    if (!EnvSet(name)) {
        SetEnv(name, value);
    }
}

class ApiGuest final : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        return CoopRuntime::Read(address, out, size);
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        return CoopRuntime::Write(address, in, size);
    }
};

class ApiPort final : public Objects::GuestPort {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        return CoopRuntime::Read(address, out, size);
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        return CoopRuntime::Write(address, in, size);
    }
    std::optional<std::uint32_t> CompareExchange32(std::uint64_t address, std::uint32_t expected,
                                                   std::uint32_t desired) override {
        std::uint32_t seen = 0;
        return CoopRuntime::Cas32(address, expected, desired, seen) ? std::optional<std::uint32_t>(seen)
                                                                    : std::nullopt;
    }
};

// The puppet pack's manifest where the hook looks for it: <load folder>/<title id>/<mod>/coop_puppet.json.
void ShadowManifest(const std::filesystem::path& mod_folder) {
    std::error_code ec;
    std::filesystem::path found;
    const auto has = [&](const std::filesystem::path& mod) {
        return std::filesystem::is_regular_file(mod / "coop_puppet.json", ec);
    };
    if (has(mod_folder)) {
        found = mod_folder;
    } else {
        // Another mod folder of the game holds the pack (a second game on one PC with its own mod folder).
        const Nx2PluginApi* api = Overlay::Api();
        const Nx2GameInfo* info = api != nullptr ? api->game_info() : nullptr;
        if (info != nullptr && info->mods_dir != nullptr && *info->mods_dir != 0) {
            const std::string dir = info->mods_dir;
            for (std::filesystem::directory_iterator it(std::filesystem::path(std::u8string(dir.begin(), dir.end())),
                                                        ec),
                 end;
                 !ec && it != end; it.increment(ec)) {
                if (it->is_directory(ec) && has(it->path())) {
                    found = it->path();
                    break;
                }
            }
        }
    }
    const auto to = g_dir / "mods" / "contents" / "0400486024DEC000" / "coop-puppet";
    std::filesystem::create_directories(to, ec);
    if (found.empty()) {
        std::filesystem::remove(to / "coop_puppet.json", ec);
        return;
    }
    std::filesystem::copy_file(found / "coop_puppet.json", to / "coop_puppet.json",
                               std::filesystem::copy_options::overwrite_existing, ec);
}

// One call as the emulator's cave makes it: x0 to x7, s0 and the four stack words.
std::optional<std::uint64_t> Make(const Nx2PluginApi* api, const CoopPuppet::Call& call) {
    Nx2GuestRegs regs{};
    regs.size = sizeof(regs);
    for (std::size_t i = 0; i < call.x.size(); ++i) {
        regs.x[i] = call.x[i];
    }
    std::uint32_t s0 = 0;
    std::memcpy(&s0, &call.s0, sizeof(s0));
    regs.v[0][0] = s0;
    regs.stack = call.stack.data();
    regs.nstack = call.stack.size();
    if (api->guest_call_regs(call.function, &regs) != 1) {
        g_broken = true;
        LOG_WARNING(Core, "coop: the game refused the call of main+{:#x}; no more game calls in this run",
                    call.function - g_main.load());
        return std::nullopt;
    }
    return regs.x[0];
}

void FlashStep(CoopPuppet::Guest& guest, std::uint64_t now_ms);

// The game's calc thread, before the frame step runs, after the frame's actors: the emulator's cave.
void OnStep(Nx2HookCall* call, void*) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    const std::uint64_t main = g_main.load(std::memory_order_relaxed);
    if (api == nullptr || !g_on || g_broken || main == 0 ||
        (call->lr != main + kFrameStepReturn && call->lr != main + kFrameStepReturnRare)) {
        return;
    }
    const std::uint64_t begin = SteadyUs();
    ApiGuest guest;
    InCallback in;
    const std::size_t made = CoopCallers::RunFrame(guest, SteadyMs(), CoopSvc::kCallsPerFrame,
                                                   [api](const CoopPuppet::Call& c) { return Make(api, c); });
    FlashStep(guest, SteadyMs());
    if (static std::uint64_t next_player_ms = 0; g_hold3 && SteadyMs() >= next_player_ms) {
        next_player_ms = SteadyMs() + 500;  // our Link's actor, for the enemies' target (objects/brain.hpp)
        Objects::Brain::NoteOurPlayer(main, Objects::Brain::FindPlayer(api, main));
    }
    static ApiPort port;
    if (Objects::AnimProbe::On()) {
        Objects::AnimProbe::Step(port, main, SteadyMs(), [](const std::string& line) { CoopRuntime::Log(line); });
    }
    if (Objects::WorldProbe::On()) {
        Objects::WorldProbe::Step(port, main, SteadyMs(), [](const std::string& line) { CoopRuntime::Log(line); });
    }
    const std::uint64_t us = SteadyUs() - begin;
    g_steps.fetch_add(1, std::memory_order_relaxed);
    g_calls.fetch_add(made, std::memory_order_relaxed);
    g_step_us.fetch_add(us, std::memory_order_relaxed);
    if (us > g_worst_us.load(std::memory_order_relaxed)) {
        g_worst_us.store(us, std::memory_order_relaxed);
    }
}

// Once per presented frame (loads and pauses included), on the game's presenting thread: Hookshot's present site.
void OnFrame(const Nx2Frame*, void*) {
    if (!g_on) {
        return;
    }
    const std::uint64_t begin = SteadyUs();
    InCallback in;
    g_ours_us.store(0, std::memory_order_relaxed);
    CoopHook::OnFrame(g_system);
    g_frames.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t us = SteadyUs() - begin;
    if (us > 250'000 && g_log != nullptr) {
        // A frame of the hook that held the picture back (the dead player's lag): how much was ours.
        char line[160];
        std::snprintf(line, sizeof(line), "slow hook frame: %.0f ms, of it %.0f ms in this port's own frame",
                      static_cast<double>(us) / 1000.0,
                      static_cast<double>(g_ours_us.load(std::memory_order_relaxed)) / 1000.0);
        g_log(line);
    }
    g_frame_us.fetch_add(us, std::memory_order_relaxed);
    if (us > g_worst_frame_us.load(std::memory_order_relaxed)) {
        g_worst_frame_us.store(us, std::memory_order_relaxed);
    }
}

// --- A puppet's own voice ---
//
// The puppet takes the other player's pose bone for bone, so its own animation is not played and the
// sounds that hang on the animation's events (and the hurt voice, which the player's own code asks for)
// are not made: seen in run kh1, where the struck Link's own sound user played Damage_S_01 / _03 and its
// puppet's user played nothing at all. What is asked for here is emitted as the hook's Ocarina notes
// are (lichii/hook/coop_puppet_sound.h): the named asset is looked up in the puppet's own sound user
// (main+0x43db00) and emitted on that user (main+0x440630), the game's own "search and emit". The user
// belongs to the puppet actor, so the game's sound system places the sound at the puppet as it places
// any actor's. Two game calls per sound, on the calc thread at the frame step; the scratch memory (the
// asset, the handle and the name) is one small block of the game's heap, asked for once.
void WriteVariation(CoopPuppet::Guest& guest, std::uint64_t user,
                    const std::vector<std::pair<std::uint8_t, std::uint32_t>>& props);  // below, "Gear sounds"

struct VoiceBox {
    std::mutex mutex;
    struct Want {
        std::uint8_t player = 0;
        std::string asset;
        std::uint64_t at_ms = 0;
        std::vector<std::pair<std::uint8_t, std::uint32_t>> props;  // gear sounds: the user's variation values
    };
    std::vector<Want> wanted;  // any thread
    // the calc thread
    enum class Step { idle, allocating, searching, emitting } step = Step::idle;
    std::uint64_t block = 0;
    std::uint64_t user = 0;
    std::uint8_t player = 0;
    std::string asset;
    std::uint64_t said_ms = 0;
};
VoiceBox g_voice;
std::atomic<std::uint64_t> g_sounds_stale{0}, g_sounds_played{0}, g_sounds_missing{0};
bool g_voice_debug = false;
constexpr int kVoiceOrder = 58;  // after the hook's own callers and this port's teleport
constexpr std::uint64_t kVoiceAsset = 0x00, kVoiceHandle = 0x08, kVoiceText = 0x40, kVoiceBlock = 0x100;

std::optional<CoopPuppet::Call> VoiceStep(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                          std::uint64_t now_ms) {
    VoiceBox& v = g_voice;
    const std::uint64_t main = g_main.load(std::memory_order_relaxed);
    if (after_call) {
        const VoiceBox::Step done = v.step;
        v.step = VoiceBox::Step::idle;
        if (done == VoiceBox::Step::allocating) {
            v.block = result;
            return std::nullopt;  // the sound itself at the next frame step
        }
        if (done == VoiceBox::Step::searching) {
            if ((result & 1) == 0) {
                g_sounds_missing.fetch_add(1, std::memory_order_relaxed);
                if (g_voice_debug) {
                    LOG_INFO(Core, "coop: voice: P{}'s puppet has no sound '{}' in its sound user '{}'", v.player,
                             v.asset, CoopPuppetSound::UserName(guest, v.user));
                }
                return std::nullopt;
            }
            CoopPuppet::Call c;
            c.function = main + CoopPuppetSound::addr::kEmit;
            c.x[0] = v.user;
            c.x[1] = v.block + kVoiceAsset;
            c.x[2] = v.block + kVoiceHandle;
            c.x[3] = 0;
            v.step = VoiceBox::Step::emitting;
            return c;
        }
        if (done == VoiceBox::Step::emitting) {
            g_sounds_played.fetch_add(1, std::memory_order_relaxed);
            if (g_voice_debug) {
                // What the puppet's sound user plays right after: the game's own list of that user's events.
                LOG_INFO(Core,
                         "coop: voice: P{}'s puppet emits '{}' on its own sound user '{}'; that user now plays: {}",
                         v.player, v.asset, CoopPuppetSound::UserName(guest, v.user),
                         CoopPuppetSound::Playing(guest, v.user));
            }
        }
        return std::nullopt;
    }
    std::pair<std::uint8_t, std::string> next;
    std::vector<std::pair<std::uint8_t, std::uint32_t>> next_props;
    {
        std::lock_guard lock(v.mutex);
        // A sound that waited too long (a load, a puppet being made) is not played late.
        while (!v.wanted.empty() && now_ms - v.wanted.front().at_ms > PuppetFx::kSoundStaleMs) {
            v.wanted.erase(v.wanted.begin());
            g_sounds_stale.fetch_add(1, std::memory_order_relaxed);
        }
        if (v.wanted.empty()) {
            return std::nullopt;
        }
        if (v.block != 0) {
            next = {v.wanted.front().player, v.wanted.front().asset};
            next_props = std::move(v.wanted.front().props);
            v.wanted.erase(v.wanted.begin());
        }
    }
    if (v.block == 0) {
        CoopPuppet::Call c;
        c.function = main + CoopPuppet::addr::kMalloc;
        c.x[0] = kVoiceBlock;
        v.step = VoiceBox::Step::allocating;
        return c;
    }
    std::uint64_t actor = 0;
    for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
        if (slot.number == next.first && slot.phase == CoopPuppet::Phase::live) {
            actor = slot.actor;
        }
    }
    const std::uint64_t user = actor != 0 ? CoopPuppetSound::SoundUser(guest, actor) : 0;
    std::uint64_t resource = 0, accessor = 0;
    if (user == 0 || !guest.Read(user + CoopPuppetSound::addr::kUserResource, &resource, 8) || resource == 0 ||
        !guest.Read(resource + CoopPuppetSound::addr::kResourceAccessor, &accessor, 8) || accessor == 0) {
        if (now_ms - v.said_ms > 5000) {
            v.said_ms = now_ms;
            LOG_INFO(Core, "coop: voice: P{} has no live puppet with a sound user here; '{}' is not played",
                     next.first, next.second);
        }
        return std::nullopt;
    }
    std::array<char, 0x40> text{};
    std::memcpy(text.data(), next.second.data(), std::min<std::size_t>(next.second.size(), text.size() - 1));
    const std::uint64_t zero = 0;
    const std::array<std::uint8_t, 8> handle{0xff, 0, 0xff, 0xff, 0, 0, 0, 0};  // as the game's own node starts one
    if (!guest.Write(v.block + kVoiceText, text.data(), text.size()) ||
        !guest.Write(v.block + kVoiceAsset, &zero, 8) || !guest.Write(v.block + kVoiceHandle, handle.data(), 8)) {
        return std::nullopt;
    }
    WriteVariation(guest, user, next_props);  // the boots, the floor, the foot of the real Link's step
    v.user = user;
    v.player = next.first;
    v.asset = next.second;
    CoopPuppet::Call c;
    c.function = main + CoopPuppetSound::addr::kSearchAsset;
    c.x[0] = accessor;
    c.x[1] = v.block + kVoiceAsset;
    c.x[2] = v.block + kVoiceText;
    v.step = VoiceBox::Step::searching;
    return c;
}

// --- Our own Link's sounds, for the other games' puppets ---
//
// Every sound an actor's sound user starts goes through xlink2's emit (main+0x440630: x0 the user,
// x1 -> the asset, whose first word points at its name). A hook on it keeps the names our own Link's
// user starts (footsteps, sword swings, voices, the hurt voice): they are sent as PLAYER_EXTRA and the
// other games emit them on our puppet's own user. The puppet's user is another object, so what a
// puppet plays here is never taken for our own.
struct OwnSounds {
    std::mutex mutex;
    std::vector<std::string> names;
    std::vector<std::vector<std::pair<std::uint8_t, std::uint32_t>>> props;  // with each name (may be empty)
};

// --- Gear sounds: the sound user's properties that choose a footstep's variation ---
// Read from the game's sound resource (SLink2/slink2.Product.111.bslnk.zs) and the lifted code by a research
// sub-agent: 'FootStep' of user Player_Adult switches on the property 足装備 (boots: Hover, Heavy, normal), each
// leaf picks its wave by サウンドマテリアル (the floor), and 足 (which foot) and the speeds choose further;
// 'EquipmentNoise_FootStep' switches on the weapon and shield properties. A user's values are a u32 array at
// [user+0x98], one slot per definition ([[user+0x58]+0x48][i], count u16 at [[user+0x58]+0x44]); the game marks a
// changed one in the bit masks at user+0x88 / +0x90. So our Link's values go with each such sound and are written
// into the puppet's user (same user, same definitions) just before the puppet emits it.
const char* const kVariationProps[] = {"足装備", "足", "サウンドマテリアル", "移動速度(アニメ)", "速度", "足速度",
                                       "勾配", "接地面の濡れ率", "水マテリアル", "武器種別", "武器装備状態",
                                       "盾種別", "盾装備状態"};
struct PropIndex {
    std::mutex mutex;
    std::uint64_t user = 0;  // whose definitions these are
    std::vector<std::uint8_t> indices;
    bool said = false;
};
PropIndex g_prop_index;
bool g_gear_sounds = true;  // OOT_COOP_GEAR_SOUNDS=0: names only, as before

std::uint32_t PropCount(const Nx2PluginApi* api, std::uint64_t user, std::uint64_t* defs) {
    std::uint64_t res = 0;
    std::uint16_t count = 0;
    if (api->read(user + 0x58, &res, 8) != 1 || res < 0x10000 || api->read(res + 0x44, &count, 2) != 1 ||
        api->read(res + 0x48, defs, 8) != 1 || *defs < 0x10000) {
        return 0;
    }
    return count;
}

// The indices of kVariationProps in this user's definitions (found once per user by name).
std::vector<std::uint8_t> VariationIndices(const Nx2PluginApi* api, std::uint64_t user) {
    std::lock_guard lock(g_prop_index.mutex);
    if (g_prop_index.user == user) {
        return g_prop_index.indices;
    }
    g_prop_index.user = user;
    g_prop_index.indices.clear();
    std::uint64_t defs = 0;
    const std::uint32_t count = PropCount(api, user, &defs);
    std::string seen;
    for (std::uint32_t i = 0; i < count && i < 255; ++i) {
        std::uint64_t def = 0;
        if (api->read(defs + 8 * i, &def, 8) != 1 || def < 0x10000) {
            continue;
        }
        // The name: a char* at def+8 (as in the XLink component's own lists), else at def+0.
        for (const std::uint64_t at : {std::uint64_t{8}, std::uint64_t{0}}) {
            std::uint64_t text = 0;
            char name[48] = {};
            if (api->read(def + at, &text, 8) != 1 || text < 0x10000 || api->read(text, name, sizeof(name) - 1) != 1) {
                continue;
            }
            bool found = false;
            for (const char* want : kVariationProps) {
                if (std::string_view(name) == want) {
                    g_prop_index.indices.push_back(static_cast<std::uint8_t>(i));
                    found = true;
                }
            }
            if (found) {
                seen += std::string(seen.empty() ? "" : ", ") + name + " " + std::to_string(i);
                break;
            }
        }
    }
    if (!g_prop_index.said && g_log != nullptr) {
        g_prop_index.said = true;
        g_log(("gear sounds: our Link's sound user has " + std::to_string(count) + " properties; the variation ones: " +
               (seen.empty() ? std::string("none found by name (names only are sent)") : seen))
                  .c_str());
    }
    return g_prop_index.indices;
}

std::vector<std::pair<std::uint8_t, std::uint32_t>> ReadVariation(const Nx2PluginApi* api, std::uint64_t user) {
    std::vector<std::pair<std::uint8_t, std::uint32_t>> out;
    std::uint64_t values = 0;
    if (!g_gear_sounds || api->read(user + 0x98, &values, 8) != 1 || values < 0x10000) {
        return out;
    }
    for (const std::uint8_t i : VariationIndices(api, user)) {
        std::uint32_t v = 0;
        if (api->read(values + 4 * i, &v, 4) == 1) {
            out.emplace_back(i, v);
        }
    }
    return out;
}

// The puppet's user: the same values, marked changed as the game's own setter does (main+0x3bd01a0).
void WriteVariation(CoopPuppet::Guest& guest, std::uint64_t user,
                    const std::vector<std::pair<std::uint8_t, std::uint32_t>>& props) {
    std::uint64_t values = 0, res = 0;
    std::uint16_t count = 0;
    if (props.empty() || !guest.Read(user + 0x98, &values, 8) || values < 0x10000 || !guest.Read(user + 0x58, &res, 8) ||
        res < 0x10000 || !guest.Read(res + 0x44, &count, 2)) {
        return;
    }
    for (const auto& [i, v] : props) {
        if (i >= count) {
            continue;
        }
        guest.Write(values + 4 * i, &v, 4);
        const std::uint64_t mask_at = user + 0x88 + 8 * (i >> 6);
        std::uint64_t mask = 0;
        if (guest.Read(mask_at, &mask, 8)) {
            mask |= std::uint64_t{1} << (i & 63);
            guest.Write(mask_at, &mask, 8);
        }
    }
}
OwnSounds g_own;
std::atomic<std::uint64_t> g_own_user{0};
std::atomic<std::uint64_t> g_own_heard{0};
int g_emit_handle = 0;
int g_own_debug = 0;

void OnEmit(Nx2HookCall* call, void*) {
    const std::uint64_t user = call->x[0];
    if (user == 0 || (user != g_own_user.load(std::memory_order_relaxed) && g_own_debug < 2)) {
        return;
    }
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    std::uint64_t asset = 0, text = 0;
    char name[PuppetFx::kMaxSoundName + 1] = {};
    if (api == nullptr || call->x[1] < 0x10000 || api->read(call->x[1], &asset, 8) != 1 || asset < 0x10000 ||
        api->read(asset, &text, 8) != 1 || text < 0x10000 || api->read(text, name, sizeof(name) - 1) != 1) {
        return;
    }
    if (!PuppetFx::SoundName(name)) {
        return;
    }
    if (user != g_own_user.load(std::memory_order_relaxed)) {
        if (g_log != nullptr) {  // OOT_COOP_DEBUG_OWN_SOUNDS=2: every user's
            g_log((std::string("sound: another user ") + std::to_string(user) + " starts '" + name + "'").c_str());
        }
        return;
    }
    g_own_heard.fetch_add(1, std::memory_order_relaxed);
    if (g_own_debug != 0 && g_log != nullptr) {
        g_log((std::string("sound: our Link starts '") + name + "'").c_str());
    }
    auto props = PuppetFx::HasVariations(name) ? ReadVariation(api, user)
                                               : std::vector<std::pair<std::uint8_t, std::uint32_t>>{};
    if (g_own_debug != 0 && g_log != nullptr && !props.empty()) {
        std::string line = std::string("sound: '") + name + "' with";
        for (const auto& [i, v] : props) {
            line += " " + std::to_string(i) + "=" + std::to_string(v);
        }
        g_log(line.c_str());
    }
    std::lock_guard lock(g_own.mutex);
    if (g_own.names.size() < 32) {
        g_own.names.emplace_back(name);
        g_own.props.push_back(std::move(props));
    }
}

// --- Research (OOT_COOP_DEBUG_SIGN=1): each call of a sign's cut routine main+0x266f960 (node, mode), with the hit
// its Life component holds at that moment and what the routine answers (docs/world.md, "Signs: the hit itself").
thread_local int t_in_sign_cut = 0;
void OnSignTrace(Nx2HookCall* call, void*) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (t_in_sign_cut == 0 || api == nullptr || g_log == nullptr) {
        return;
    }
    const std::uint64_t x0 = call->x[0], x1 = call->x[1], x2 = call->x[2], lr = call->lr, x8 = call->x[8];
    if (call->addr - g_main.load() == 0x2670a60) {  // its fourteen inputs, as floats
        std::array<std::uint64_t, 14> at{};
        api->read(x0, at.data(), sizeof(at));
        std::string line = "sign trace: inputs";
        for (std::size_t i = 0; i < at.size(); ++i) {
            std::array<float, 4> f{};
            std::array<std::uint32_t, 4> u{};
            api->read(at[i], f.data(), sizeof(f));
            std::memcpy(u.data(), f.data(), sizeof(u));
            char one[120];
            std::snprintf(one, sizeof one, " [%zu] %.4g %.4g %.4g %.4g (%x)", i, f[0], f[1], f[2], f[3], u[0]);
            line += one;
        }
        g_log(line.c_str());
    }
    api->call_original(call);
    if (call->addr - g_main.load() == 0x6ab6f0 && lr - g_main.load() == 0x2671784) {
        // The contacts of the board's "Tgt" body, as the cut routine walks them.
        const std::uint64_t it = x8;
        std::array<std::uint64_t, 9> w{};
        api->read(it, w.data(), sizeof(w));
        std::array<std::uint64_t, 0x28> c{};
        api->read(x0, c.data(), sizeof(c));
        char one[200];
        std::string line = "sign contacts: iterator";
        for (const std::uint64_t v : w) {
            std::snprintf(one, sizeof one, " %llx", static_cast<unsigned long long>(v));
            line += one;
        }
        std::snprintf(one, sizeof one, "; container +0x8 %llx +0x28 %llx +0xa0 %llx; entries:",
                      static_cast<unsigned long long>(c[1]), static_cast<unsigned long long>(c[5]),
                      static_cast<unsigned long long>(c[0x14]));
        line += one;
        const std::uint64_t first = (w[8] & 0xff) != 0 ? 0 : w[0], end = w[2];
        for (std::uint64_t e = first; e != 0 && e < end && e < first + 0x80; e += 0x10) {
            std::uint64_t p = 0, flag = 0, a = 0, b = 0, va = 0, vb = 0, vp = 0;
            std::uint16_t bits = 0;
            api->read(e, &p, 8), api->read(e + 8, &flag, 8);
            api->read(p, &vp, 8), api->read(p + 0x5c, &bits, 2), api->read(p + 0x60, &a, 8), api->read(p + 0x68, &b, 8);
            api->read(a, &va, 8), api->read(b, &vb, 8);
            std::snprintf(one, sizeof one, " {%llx vt+0x%llx flag %llx bits %x a %llx vt+0x%llx b %llx vt+0x%llx}",
                          static_cast<unsigned long long>(p), static_cast<unsigned long long>(vp - g_main.load()),
                          static_cast<unsigned long long>(flag & 0xff), bits, static_cast<unsigned long long>(a),
                          static_cast<unsigned long long>(va - g_main.load()), static_cast<unsigned long long>(b),
                          static_cast<unsigned long long>(vb - g_main.load()));
            line += one;
        }
        g_log(line.c_str());
    }
    char text[200];
    std::snprintf(text, sizeof text, "sign trace: main+0x%llx(%llx, %llx, %llx) from +0x%llx -> %llx",
                  static_cast<unsigned long long>(call->addr - g_main.load()), static_cast<unsigned long long>(x0),
                  static_cast<unsigned long long>(x1), static_cast<unsigned long long>(x2),
                  static_cast<unsigned long long>(lr - g_main.load()), static_cast<unsigned long long>(call->x[0]));
    g_log(text);
}
void OnSignCut(Nx2HookCall* call, void*) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (api == nullptr || g_log == nullptr) {
        return;
    }
    const std::uint64_t node = call->x[0], mode = call->x[1];
    std::uint64_t a = 0, actor = 0, table = 0, life = 0;
    std::string text;
    if (api->read(node + 8, &a, 8) == 1 && api->read(a + 8, &a, 8) == 1 && api->read(a + 0x1f0, &actor, 8) == 1 &&
        api->read(actor + 0x270, &table, 8) == 1 && api->read(table + 0x2b8, &life, 8) == 1 && life > 0x10000) {
        std::array<std::uint64_t, 0x100 / 8> words{};
        api->read(life + 0x1f8, words.data(), sizeof(words));
        char one[24];
        for (const std::uint64_t w : words) {
            std::snprintf(one, sizeof one, " %llx", static_cast<unsigned long long>(w));
            text += one;
        }
        std::uint64_t cfg = 0, inputs = 0;
        api->read(life + 0x12b0, &cfg, 8);
        api->read(node + 0x58, &inputs, 8);
        std::snprintf(one, sizeof one, " | %llx", static_cast<unsigned long long>(cfg));
        text += one;
        std::snprintf(one, sizeof one, " | %llx", static_cast<unsigned long long>(inputs));
        text += one;
    }
    ++t_in_sign_cut;
    api->call_original(call);
    --t_in_sign_cut;
    char head[160];
    std::snprintf(head, sizeof head, "sign cut routine: actor %llx mode %llu answers %llu; Life +0x1f8:",
                  static_cast<unsigned long long>(actor), static_cast<unsigned long long>(mode),
                  static_cast<unsigned long long>(call->x[0] & 1));
    g_log((head + text).c_str());
}

// --- A sign cut by another player: the blade's contact (docs/world.md, "Signs: the blade's contact") ---
// The hook gives our sign the cutter's own sword hit, and the sign's AI then runs the game's cut routine
// main+0x266f960. Its helper main+0x2670a60 cuts a piece of the board only when the board's "Tgt" rigid body has a
// contact with another body at that moment (the list at [body + 0x158]: count +0x8, entries +0x28 of
// {contact, flags}, the other body at contact + 0x60): the blade, which is not there in this game. For the length
// of that one call the list gets one contact whose other body is the board's own, and is emptied again after it.
std::atomic<std::uint64_t> g_sign_block{0};  // 0x100 bytes of the game's heap for the contact, made once
std::atomic<std::uint64_t> g_sign_cuts{0};
bool g_sign_gate_debug = false;
void OnSignGate(Nx2HookCall* call, void*) {
    const std::uint64_t given = CoopWorld::GivenSignHitActor(500);
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (given == 0 || api == nullptr) {
        return;
    }
    const std::uint64_t main = g_main.load();
    auto word = [api](std::uint64_t at) {
        std::uint64_t v = 0;
        return at >= 0x10000 && api->read(at, &v, 8) == 1 ? v : 0;
    };
    const std::uint64_t actor = word(word(word(call->x[0] + 8) + 8) + 0x1f0), mode = call->x[1];
    if (actor != given) {
        return;
    }
    const std::uint64_t root = word(word(word(actor + 0x270) + 0x50) + 0x20);
    std::uint64_t block = g_sign_block.load();
    if (block == 0) {
        const std::uint64_t size[1] = {0x100};
        block = api->guest_call(main + CoopPuppet::addr::kMalloc, size, 1);
        if (block < 0x10000) {
            return;
        }
        g_sign_block.store(block);
    }
    const std::uint64_t name = main + 0x3ca5b05;  // "Tgt"
    const std::uint64_t find[2] = {root, name};
    std::uint64_t body = root != 0 ? api->guest_call(main + 0x6b1e30, find, 2) : 0;
    if (body >= 0x10000) {
        const std::uint64_t again[2] = {body, name};
        body = api->guest_call(main + 0x1132250, again, 2);
    }
    const std::uint64_t list = body >= 0x10000 ? word(body + 0x158) : 0;
    std::uint32_t count = 1;
    const std::uint64_t entries = list != 0 ? word(list + 0x28) : 0;
    if (entries == 0 || api->read(list + 8, &count, 4) != 1 || count != 0) {
        return;  // not found, or real contacts: the game's own answer stands
    }
    // The other body: the board's own rigid body, an object of the class the game's contacts hold.
    std::uint64_t other = 0;
    for (std::uint64_t at = 0; at < 0x200 && other == 0; at += 8) {
        const std::uint64_t p = word(body + at);
        if (p >= 0x10000 && word(p) == main + 0x4c37d20) {
            other = p;
        }
    }
    if (other == 0) {
        if (g_sign_gate_debug && g_log != nullptr) {
            g_log("sign: no rigid body found for the stand-in contact; the cut is left to the game");
        }
        return;
    }
    std::array<std::uint64_t, 0x70 / 8> contact{};
    contact[0x60 / 8] = other;
    contact[0x68 / 8] = other;
    std::array<std::uint64_t, 2> entry{block, 0}, before{};
    api->read(entries, before.data(), sizeof(before));
    api->write(block, contact.data(), sizeof(contact));
    api->write(entries, entry.data(), sizeof(entry));
    count = 1;
    api->write(list + 8, &count, 4);
    api->call_original(call);
    count = 0;
    api->write(list + 8, &count, 4);
    api->write(entries, before.data(), sizeof(before));
    g_sign_cuts.fetch_add(1);
    if (g_log != nullptr) {
        char text[160];
        std::snprintf(text, sizeof text, "sign: the cutter's hit ran our sign's cut routine with a stand-in contact "
                      "(mode %llu, answer %llu)", static_cast<unsigned long long>(mode),
                      static_cast<unsigned long long>(call->x[0] & 1));
        g_log(text);
    }
}

#include "item_get_step.inc"

// --- Floor switches pressed by any player are down in every game (step_switch.hpp) ---
// The step switch's selector `SelectorSwitchStepBootup` (evaluate main+0x11a4920, x0 = the node): [node+0x58]
// points at its ForceOn input byte, the node's current child is the byte at +0x1e and its On child's index the
// byte at +0x68. Found by a research sub-agent from the lifted code and the switch's AI file (not by a run).
StepSwitch::Switches g_switches;
std::atomic<bool> g_switches_on{false};
std::atomic<std::uint64_t> g_switch_calls{0};
void OnStepSelector(Nx2HookCall* call, void*) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (api == nullptr) {
        return;
    }
    auto word = [api](std::uint64_t at) {
        std::uint64_t v = 0;
        return at >= 0x10000 && api->read(at, &v, 8) == 1 ? v : 0;
    };
    const std::uint64_t node = call->x[0];
    const std::uint64_t actor = word(word(word(node + 8) + 8) + 0x1f0);
    const std::uint64_t placement = actor != 0 ? word(actor + 0x3d8) : 0;
    const std::uint64_t hash = placement != 0 ? word(placement + 0x88) : 0;
    if (hash == 0 || hash == ~std::uint64_t{0}) {
        return;
    }
    g_switch_calls.fetch_add(1, std::memory_order_relaxed);
    const std::uint64_t now = SteadyUs();
    if (g_switches.Force(hash, now)) {
        const std::uint64_t port = word(node + 0x58);
        const std::uint8_t one = 1;
        if (port >= 0x10000) {
            api->write(port, &one, 1);
        }
    }
    api->call_original(call);
    std::uint8_t current = 0xff, on = 0xfe;
    if (api->read(node + 0x1e, &current, 1) == 1 && api->read(node + 0x68, &on, 1) == 1) {
        g_switches.Seen(hash, current == on, now);
    }
}

// --- A struck player's puppet turns red as the game's own Link does (puppet_fx.hpp) ---
std::array<std::atomic<std::uint64_t>, 5> g_hurt_ms{};  // when each player was last struck (steady ms), 0 none
std::array<std::uint8_t, 5> g_flash_state{};            // the calc thread: 1 while a ratio above 0 is written
std::array<std::uint8_t, 5> g_blink_state{};            // 1 while the puppet may be hidden
bool g_blink = false;
std::atomic<std::uint64_t> g_flashes{0};

void FlashStep(CoopPuppet::Guest& guest, std::uint64_t now_ms) {
    for (const auto& slot : CoopPuppet::TheDirector().Slots()) {
        if (slot.number == 0 || slot.number >= g_hurt_ms.size()) {
            continue;
        }
        const std::uint64_t hurt = g_hurt_ms[slot.number].load(std::memory_order_relaxed);
        const bool live = slot.phase == CoopPuppet::Phase::live && slot.actor != 0;
        if (!live || (hurt == 0 && g_flash_state[slot.number] == 0 && g_blink_state[slot.number] == 0)) {
            if (!live) {
                g_flash_state[slot.number] = g_blink_state[slot.number] = 0;
            }
            continue;
        }
        std::uint64_t parts = 0, model = 0;
        std::uint32_t dirty = 0;
        if (!guest.Read(slot.actor + CoopPuppetSound::addr::kActorParts, &parts, 8) || parts == 0 ||
            !guest.Read(parts + PuppetFx::addr::kPartsModelController, &model, 8) || model == 0 ||
            !guest.Read(model + PuppetFx::addr::kDirty, &dirty, 4)) {
            continue;
        }
        const std::uint64_t never = ~std::uint64_t{0};
        const std::uint64_t ago = hurt != 0 && now_ms >= hurt ? now_ms - hurt : never;
        std::uint32_t set = 0;
        if (ago < PuppetFx::kFlashMs || g_flash_state[slot.number] != 0) {
            const float ratio = ago < PuppetFx::kFlashMs ? PuppetFx::DamageRatio(ago) : 0.0f;
            if (g_flash_state[slot.number] == 0) {
                g_flashes.fetch_add(1, std::memory_order_relaxed);
            }
            g_flash_state[slot.number] = ago < PuppetFx::kFlashMs ? 1 : 0;
            guest.Write(model + PuppetFx::addr::kDamageRatio, &ratio, 4);
            set |= PuppetFx::addr::kDirtyDamage;
        }
        if (g_blink && (ago < PuppetFx::kBlinkMs || g_blink_state[slot.number] != 0)) {
            const std::uint8_t shown = ago < PuppetFx::kBlinkMs ? (PuppetFx::BlinkShown(ago) ? 1 : 0) : 1;
            g_blink_state[slot.number] = ago < PuppetFx::kBlinkMs ? 1 : 0;
            guest.Write(model + PuppetFx::addr::kVisible, &shown, 1);
            set |= PuppetFx::addr::kDirtyVisible;
        }
        if (set != 0) {
            dirty |= set;
            guest.Write(model + PuppetFx::addr::kDirty, &dirty, 4);
        }
        if (ago >= PuppetFx::kBlinkMs && ago != never) {
            g_hurt_ms[slot.number].store(0, std::memory_order_relaxed);
        }
    }
}

// coop_counter.s: a started AI pause counter (x0 at main+0x5e6760) goes into the ring.
void OnCounter(Nx2HookCall* call, void*) {
    const std::uint64_t ring = g_ring.load(std::memory_order_relaxed);
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    std::uint32_t count = 0;
    if (ring == 0 || call->x[0] == 0 || api == nullptr || api->read(ring, &count, 4) != 1) {
        return;
    }
    ++count;
    const std::uint64_t node = call->x[0];
    api->write(ring + 8 + 8ull * (count % 256), &node, 8);
    api->write(ring, &count, 4);
}

}  // namespace

void Configure(const Settings& s, const std::filesystem::path& mod_folder, void (*log)(const char* line),
               const Hooks& hooks) {
    g_log = log;
    g_hooks = hooks;
    std::error_code ec;
    g_dir = mod_folder / "coop-up";  // holds no coop.ini: the settings come through the environment
    std::filesystem::create_directories(g_dir, ec);
    ShadowManifest(mod_folder);
    // coop_settings.cpp: the environment replaces the file's keys.
    SetEnv("LICHII_COOP_SERVER", s.coop.server);
    SetEnv("LICHII_COOP_PORT", std::to_string(s.coop.port));
    SetEnv("LICHII_COOP_NAME", s.coop.name);
    SetEnv("LICHII_COOP_PASSWORD", Settings::JoinPassword(s.coop.password));
    // The kit's switches (tools/release/kit/coop-options.ini), from this port's keys.
    Default("LICHII_COOP_PUPPET", s.puppet ? "1" : "0");
    Default("LICHII_COOP_OBJECTS", s.objects ? "1" : "0");
    Default("LICHII_COOP_SPAWN", s.objects ? "1" : "0");
    Default("LICHII_COOP_BOSS", s.objects && s.objects_boss ? "1" : "0");
    Default("LICHII_COOP_TIME", s.time ? "1" : "0");
    Default("LICHII_COOP_DROPS", s.objects && s.drops ? "1" : "0");
    if (!s.share_progress) {
        for (const char* name : {"LICHII_COOP_ITEMS", "LICHII_COOP_DUNGEON", "LICHII_COOP_STORY",
                                 "LICHII_COOP_QUESTS", "LICHII_COOP_CHESTS", "LICHII_COOP_PICKUPS"}) {
            Default(name, "0");
        }
    }
    // Plain puppets only (the user's choice; the pack made for this port has no tunic puppets).
    Default("LICHII_COOP_TUNICS", "0");
    // The other players' Navi needs its own actor in the pack; off until that is seen in this game.
    Default("LICHII_COOP_FAIRY", "0");
    g_hold3 = s.objects && s.objects_ai_hold != 0;
    if (g_hold3) {
        Default("LICHII_COOP_OBJECTS_AI_HOLD", std::to_string(s.objects_ai_hold));
    }
    // This port's own rules in the shared enemies' code (lichii/hook/coop_objects_sync.h).
    CoopObjects::ShareFriendly(s.objects_friendly);
    {
        const char* slots = std::getenv("OOT_COOP_ENEMY_SLOTS");
        CoopObjects::EnemySlots(slots == nullptr || std::string_view(slots) != "0");
    }
    g_configured = true;
}

bool Attached() { return g_on; }

bool Attach(const Nx2PluginApi* api, std::uint64_t main_base) {
    if (!g_configured || g_on) {
        return g_on;
    }
    if (api == nullptr || api->version < 2 || !NX2_HAS(api, Nx2PluginApi, hook_function) ||
        !NX2_HAS(api, Nx2PluginApi, map_range)) {
        if (g_log != nullptr) {
            g_log("off: co-op needs NX2navi's plugin interface version 2 or later (function hooks and fast "
                  "memory); this game has an older one");
        }
        return false;
    }
    const Nx2GameInfo* info = api->game_info();
    g_build = info != nullptr && info->build_id != nullptr ? info->build_id : "";
    for (char& c : g_build) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    g_main = main_base;
    g_api = api;
    if (std::getenv("LICHII_COOP_PUPPET") != nullptr && std::string_view(std::getenv("LICHII_COOP_PUPPET")) != "0") {
        CoopPuppetConfig::Configure();  // as the emulator's cave and Hookshot's module do
    }
    // This port's teleport (join at the host, follow through exits) is one more caller of the frame step.
    CoopCallers::Add(kTeleportOrder, [](CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                        std::uint64_t) {
        return Teleport::Step(guest, after_call, result, g_main.load());
    });
    CoopCallers::Add(kVoiceOrder, VoiceStep);
    CoopCallers::Add(kItemOrder, ItemStep);
    {
        const char* e = std::getenv("OOT_COOP_DEBUG_OWN_SOUNDS");
        g_own_debug = e != nullptr ? std::atoi(e) : 0;
        e = std::getenv("OOT_COOP_DEBUG_PUPPET_SOUNDS");
        g_voice_debug = e != nullptr && std::string_view(e) == "1";
        e = std::getenv("OOT_COOP_PUPPET_BLINK");
        g_blink = e != nullptr && std::string_view(e) == "1";
        e = std::getenv("OOT_COOP_GEAR_SOUNDS");
        g_gear_sounds = e == nullptr || std::string_view(e) != "0";
        e = std::getenv("OOT_COOP_PUPPET_SOUNDS");
        if (e == nullptr || std::string_view(e) != "0") {
            g_emit_handle = api->hook_function(main_base + PuppetFx::addr::kEmit, OnEmit, nullptr);
        }
    }
    if (const char* e = std::getenv("OOT_COOP_SIGN_CUT"); e == nullptr || std::string_view(e) != "0") {
        g_sign_gate_debug = std::getenv("OOT_COOP_DEBUG_SIGN") != nullptr;
        api->hook_function(main_base + 0x266f960, OnSignGate, nullptr);
    }
    if (const char* e = std::getenv("OOT_COOP_DEBUG_SIGN"); e != nullptr && std::string_view(e) == "1") {
        api->hook_function(main_base + 0x266f960, OnSignCut, nullptr);
    } else if (e != nullptr && std::string_view(e) == "2") {
        api->hook_function(main_base + 0x266f960, OnSignCut, nullptr);
        for (const std::uint64_t at : {0x266f8e0ull, 0x2670a60ull, 0xf0b190ull, 0xd73710ull, 0x1173c30ull, 0x6b1e30ull,
                                       0x1132250ull, 0x6ab6f0ull, 0x122750ull, 0x2e2ec00ull, 0x3b10100ull}) {
            api->hook_function(main_base + at, OnSignTrace, nullptr);
        }
    }
    if (const char* e = std::getenv("OOT_COOP_SWITCHES"); e == nullptr || std::string_view(e) != "0") {
        g_switches_on = api->hook_function(main_base + 0x11a4920, OnStepSelector, nullptr) > 0;
    }
    g_on = true;
    g_step_handle = api->hook_function(main_base + kFrameStep, OnStep, nullptr);
    g_frame_handle = api->on_frame(OnFrame, nullptr);
    g_counter_handle = api->hook_function(main_base + CoopCounters::kStartOffset, OnCounter, nullptr);
    const bool brain = g_hold3 && Objects::Brain::Install(api, main_base);
    LOG_INFO(Core,
             "coop: NX2navi module (lichii/hook unchanged, as on Hookshot): main at {:#x}, build {}; frame "
             "callback {}, frame step hook {}, platform counter hook {}, AI hold {}",
             main_base, g_build.substr(0, 16), g_frame_handle > 0 ? "on" : "REFUSED",
             g_step_handle > 0 ? "on" : "REFUSED", g_counter_handle > 0 ? "on" : "refused",
             g_hold3 ? (brain ? "on (a held copy's AI update main+0xcb9c0 left out)" : "REFUSED") : "off");
    CoopHook::Start();
    return true;
}

bool SendChat(const std::string& text) { return g_on && CoopHook::SendChat(text); }

StepSwitch::Switches* Switches() { return g_switches_on.load() ? &g_switches : nullptr; }
void AskItemScene(const std::string& actor, const std::array<float, 3>& at) {
    std::lock_guard lock(g_item.mutex);
    g_item.scene = std::make_pair(actor, at);
}
void AskFanfare(int type) {
    std::lock_guard lock(g_item.mutex);
    g_item.fanfare = type;
}
int TakeItemDone() { return g_item_done.exchange(0); }
void NoteEvent(bool event) { g_item_event = event; }
std::uint64_t SwitchCalls() { return g_switch_calls.load(std::memory_order_relaxed); }

void PuppetSound(std::uint8_t player, const std::string& asset,
                 std::vector<std::pair<std::uint8_t, std::uint32_t>> props) {
    if (!g_on || !PuppetFx::SoundName(asset)) {
        return;
    }
    std::lock_guard lock(g_voice.mutex);
    if (g_voice.wanted.size() < 16) {
        g_voice.wanted.push_back({player, asset, SteadyMs(), std::move(props)});
    }
}

void PuppetHurt(std::uint8_t player) {
    if (g_on && player < g_hurt_ms.size()) {
        g_hurt_ms[player].store(SteadyMs(), std::memory_order_relaxed);
    }
}

void SetOwnSoundUser(std::uint64_t user) { g_own_user.store(user, std::memory_order_relaxed); }

std::vector<PuppetFx::SoundProps> TakeOwnSounds() {
    std::lock_guard lock(g_own.mutex);
    std::vector<PuppetFx::SoundProps> out;
    for (std::size_t i = 0; i < g_own.names.size(); ++i) {
        out.push_back({std::move(g_own.names[i]), i < g_own.props.size() ? std::move(g_own.props[i])
                                                                          : decltype(g_own.props)::value_type{}});
    }
    g_own.names.clear();
    g_own.props.clear();
    return out;
}

std::string StatusText() {
    if (!g_on) {
        return "; hook: not attached to the game yet";
    }
    const std::uint64_t frames = g_frames.load(), steps = g_steps.load();
    char text[300];
    std::snprintf(text, sizeof(text),
                  "; hook: %llu frames (%.3f ms each, the longest since the last line %.3f ms), %llu frame steps "
                  "(%.3f ms each, the longest since the last line %.3f ms), %llu game calls%s",
                  static_cast<unsigned long long>(frames),
                  frames != 0 ? static_cast<double>(g_frame_us.load()) / 1000.0 / static_cast<double>(frames) : 0.0,
                  static_cast<double>(g_worst_frame_us.exchange(0)) / 1000.0,
                  static_cast<unsigned long long>(steps),
                  steps != 0 ? static_cast<double>(g_step_us.load()) / 1000.0 / static_cast<double>(steps) : 0.0,
                  static_cast<double>(g_worst_us.exchange(0)) / 1000.0, static_cast<unsigned long long>(g_calls.load()),
                  g_broken ? " (a call was refused: none since)" : "");
    std::string line = text;
    std::snprintf(text, sizeof(text),
                  ", sounds: %llu of our Link's heard%s, %llu played on puppets, %llu not in their set, %llu too "
                  "late; %llu red flashes",
                  static_cast<unsigned long long>(g_own_heard.load()), g_emit_handle > 0 ? "" : " (hook off)",
                  static_cast<unsigned long long>(g_sounds_played.load()),
                  static_cast<unsigned long long>(g_sounds_missing.load()),
                  static_cast<unsigned long long>(g_sounds_stale.load()),
                  static_cast<unsigned long long>(g_flashes.load()));
    line += text;
    if (g_hold3) {
        const Objects::Brain::Counts c = Objects::Brain::Take();
        std::snprintf(text, sizeof(text),
                      ", hold: %zu held, %llu AI updates, %llu left out, %llu run for a hit, %llu with the far rule "
                      "widened%s, burrows: %llu refused (copies), %llu turned to us, %llu let, %llu held for a living player, "
                      "%llu aimed at our Link",
                      Objects::Hold::TheTable().Count(), static_cast<unsigned long long>(c.calls),
                      static_cast<unsigned long long>(c.skipped), static_cast<unsigned long long>(c.for_hits),
                      static_cast<unsigned long long>(c.far_runs), c.far_found ? "" : " (its file not found)",
                      static_cast<unsigned long long>(c.burrows_refused),
                      static_cast<unsigned long long>(c.burrows_redirected),
                      static_cast<unsigned long long>(c.burrows_let),
                      static_cast<unsigned long long>(c.burrows_held),
                      static_cast<unsigned long long>(c.targets_set));
        line += text;
        if (!c.verbs.empty()) {
            line += " (verbs: " + c.verbs + ", far looks " + std::to_string(c.far_tries) + ")";
        }
    }
    return line;
}

void NoteOurLife(int life) { g_link_alive.store(life > 0, std::memory_order_relaxed); }
bool OurLinkAlive() { return g_link_alive.load(std::memory_order_relaxed); }

void Stop() {
    if (!g_on.exchange(false)) {
        return;
    }
    const Nx2PluginApi* api = g_api.load();
    for (int* handle : {&g_frame_handle, &g_step_handle, &g_counter_handle, &g_emit_handle}) {
        if (api != nullptr && *handle > 0) {
            api->remove(*handle);
        }
        *handle = 0;
    }
    if (g_hold3) {
        Objects::Brain::Remove();
    }
    CoopHook::Shutdown();
}

}  // namespace CoopNx2navi::Up

// --- hookshot/compat's runtime, answered from NX2navi's plugin interface ---

namespace CoopRuntime {
using namespace CoopNx2navi;
using CoopNx2navi::Up::g_api;
using CoopNx2navi::Up::g_main;

bool Running() { return g_api.load(std::memory_order_acquire) != nullptr && g_main.load() != 0; }
std::uint64_t MainBase() { return g_main.load(std::memory_order_relaxed); }
const std::string& MainBuildHex() { return CoopNx2navi::Up::g_build; }
std::filesystem::path GameDir() { return CoopNx2navi::Up::g_dir; }
bool Read(std::uint64_t address, void* out, std::size_t size) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (size == 0) {
        return true;
    }
    if (api == nullptr || address < 0x10000) {
        return false;
    }
    const std::uint64_t main = g_main.load(std::memory_order_relaxed);
    if (address == main + CoopCounters::kRingPointer && size == 8) {  // kept here, not in main's code
        const std::uint64_t ring = CoopNx2navi::Up::g_ring.load();
        std::memcpy(out, &ring, 8);
        return true;
    }
    auto& cache = CoopNx2navi::Up::t_pages;
    if (cache.active && ((address ^ (address + size - 1)) >> 12) == 0) {
        const std::uint64_t number = address >> 12;
        auto& page = cache.pages[number & (CoopNx2navi::Up::PageCache::kPages - 1)];
        if (page.number != number || page.pass != cache.pass) {
            page.number = number;
            page.pass = cache.pass;
            page.at = static_cast<const std::uint8_t*>(api->map_range(number << 12, 4096, 0));
        }
        if (page.at != nullptr) {
            std::memcpy(out, page.at + (address & 0xfff), size);
            return true;
        }
    }
    return api->read(address, out, size) == 1;
}
// Whether a range can be read. Asked before nearly every read of the hook's (EmulatorMemory::read), and by
// its scans with many words that only might be pointers (CoopLiveEvents::FindRunningEvent): a read of memory
// that is not the guest's own heap, stacks or modules takes the runtime's slow checks, about a millisecond
// each, which made a frame of the hook take 1 to 3 s on the Game Over and at every load (the dead player's
// lag, runs ds11 to ds13). The interface's map_range answers from the runtime's own map of guest regions
// without that (its pointer is not kept: only whether there is one). OOT_COOP_VALID_SLOW=1: the old way.
bool Valid(std::uint64_t address, std::size_t size) {
    if (size == 0 || address + size <= address || address < 0x10000) {
        return false;
    }
    static const bool slow = [] {
        const char* e = std::getenv("OOT_COOP_VALID_SLOW");
        return e != nullptr && *e == '1';
    }();
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (!slow && api != nullptr && NX2_HAS(api, Nx2PluginApi, map_range) && api->map_range != nullptr) {
        const std::uint64_t main = g_main.load(std::memory_order_relaxed);
        if (address == main + CoopCounters::kRingPointer) {
            return true;  // kept here, not in main
        }
        return api->map_range(address, size, 0) != nullptr;
    }
    std::uint8_t b = 0;
    return Read(address, &b, 1) && Read(address + size - 1, &b, 1);
}
bool Write(std::uint64_t address, const void* in, std::size_t size) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (size == 0) {
        return true;
    }
    if (api == nullptr || address < 0x10000) {
        return false;
    }
    const std::uint64_t main = g_main.load(std::memory_order_relaxed);
    if (Objects::Hold::TakeTable(main, address, in, size)) {
        return true;  // the AI hold's table (coop_hold.h): brain.cpp reads it here
    }
    if (address == main + CoopCounters::kRingPointer && size == 8) {
        std::uint64_t ring = 0;
        std::memcpy(&ring, in, 8);
        CoopNx2navi::Up::g_ring = ring;
        return true;
    }
    Objects::Brain::NoteWrite(address, size);  // a hit record: that copy's AI update runs for it
    return api->write(address, in, size) == 1;
}
bool Cas32(std::uint64_t address, std::uint32_t expected, std::uint32_t desired, std::uint32_t& seen) {
    const Nx2PluginApi* api = g_api.load(std::memory_order_acquire);
    if (api == nullptr || (address & 3) != 0) {
        return false;
    }
    // The game's own compare-and-swap is a host atomic on the same word (nx2navi/README.md).
    void* at = api->map_range(address, 4, 1);
    if (at == nullptr) {
        return false;
    }
    seen = expected;
    __atomic_compare_exchange_n(static_cast<std::uint32_t*>(at), &seen, desired, false, __ATOMIC_SEQ_CST,
                                __ATOMIC_SEQ_CST);
    return true;
}
void Log(const std::string& line) {
    // The hook's lines start with "coop: "; the mod's log adds that itself.
    const std::string_view text = line.rfind("coop: ", 0) == 0 ? std::string_view(line).substr(6) : line;
    // The hook's share pass says where the actor list is at every frame (its "first pass" flag is only
    // cleared by its debug walk): 30 lines a second, each a write to the log file. Said when the list's
    // address changes, and else once a minute.
    static constexpr std::string_view kListHead = "objects: list head ";
    if (text.substr(0, kListHead.size()) == kListHead) {
        static std::mutex m;
        static std::string last_head;
        static std::uint64_t last_ms = 0;
        std::lock_guard lock(m);
        const std::string head(text.substr(0, text.find(',')));
        const std::uint64_t now = CoopNx2navi::Up::SteadyMs();
        if (head == last_head && now - last_ms < 60000) {
            return;
        }
        last_head = head;
        last_ms = now;
    }
    if (CoopNx2navi::Up::g_log != nullptr) {
        CoopNx2navi::Up::g_log(std::string(text).c_str());
    }
    if (text.rfind("notice: ", 0) == 0) {  // "Saria got the Hookshot", "Mido opened a chest"
        Overlay::Notice(std::string(text.substr(8)));
    }
}
bool PadButtons(std::uint64_t&) {
    return false;  // NX2navi hands a plugin button events, not the word: own Ocarina notes are not sent yet
}
}  // namespace CoopRuntime

// --- The hook's modules that are answered here instead of built ---

namespace CoopSvc {
std::string Refusal() {
    return {};  // no cave here, so no game mod can sit where it would be
}
}  // namespace CoopSvc

// lichii/hook/coop_teleport_game.h: this port's own game side (dll/teleport.cpp).
namespace CoopTeleport {
bool OnChat(std::string_view text, coop::host::Session& session) {
    return CoopNx2navi::Teleport::OnChat(text, session);
}
void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    CoopNx2navi::Teleport::OnEvent(event, client);
}
void Frame(Core::System&, std::uint64_t, const std::optional<coop::host::PlayerReading>& reading,
           coop::host::Session* session) {
    const auto& hooks = CoopNx2navi::Up::g_hooks;
    if (reading) {
        const float at[3] = {reading->position[0], reading->position[1], reading->position[2]};
        CoopNx2navi::Objects::Brain::NoteOurLink(at, CoopNx2navi::Up::OurLinkAlive());
    } else {
        CoopNx2navi::Objects::Brain::NoteOurLink(nullptr, CoopNx2navi::Up::OurLinkAlive());
    }
    if (hooks.frame != nullptr) {
        const std::uint64_t ours_begin = CoopNx2navi::Up::SteadyUs();
        hooks.frame(reading, session, hooks.user);
        CoopNx2navi::Up::g_ours_us.fetch_add(CoopNx2navi::Up::SteadyUs() - ours_begin, std::memory_order_relaxed);
    }
}
std::vector<std::string> TakeLines() {
    return {};  // the port shows its own teleport lines
}
}  // namespace CoopTeleport

// lichii/hook/coop_save.h: this port's own save transfer (dll/save_sync.cpp, multiplayer saves).
namespace CoopSave {
bool UseCoopFolder() { return false; }
std::string SaveDirectory(const std::string& path, bool) { return path; }
void NoteSaveDir(const std::string&) {}
void NoteInGame(bool) {}  // upstream's save-folder switch (Hookshot patch 0008); this port has its own
void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    CoopNx2navi::SaveSync::OnEvent(event, client);
    const auto& hooks = CoopNx2navi::Up::g_hooks;
    if (hooks.event != nullptr) {
        hooks.event(event, client, hooks.user);
    }
}
void OnFrame(coop::net::Client&, bool) {}
std::string TakeNotice() { return {}; }
}  // namespace CoopSave
