// Co-op presence inside the emulator (plan stage 1 step 1).
//
// Threads:
// - the present thread calls OnFrame: it reads the local player from guest
//   memory and hands it to coop::host::Session::frame, which queues
//   PLAYER_STATUS and PLAYER_SAMPLE; then it works out where the other players
//   are shown from the session's presence;
// - the network thread runs Session::service: it sends what frame() queued,
//   runs ENet and feeds what comes in to presence (docs/host.md, "Session");
// - the Qt co-op window reads OverlayLines() a few times a second.
// Lock order: the session's network lock, then its presence lock, then
// view_mutex. OnFrame never holds two at once.

#include "coop_hook.h"
#include "coop_caller_orders.h"
#include "coop_version.h" // LICHII_COOP_VERSION, written on each build (cmake/coop_version.cmake)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/loader/loader.h"
#include "core/memory.h"

#include "coop/host/game_items.hpp"
#include "coop/host/game_memory.hpp"
#include "coop/host/notify.hpp"
#include "coop/host/lent_keys.hpp"
#include "coop/host/map_drop_log.hpp"
#include "coop/host/one_way_keeper.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/host/scene.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/overlay/overlay.hpp"
#include "coop/presence/presence.hpp"
#include "coop/version.hpp"
#include "coop_boss.h"
#include "coop_boss_beaten.h"
#include "coop_frame_clock.h"
#include "coop_boss_scenes.h"
#include "coop_teleport_game.h"
#include "coop_pvp_game.h"
#include "coop_lift_player_game.h"
#include "coop_pvp_native_game.h"
#include "coop_lobby_game.h"
#include "coop_dbno_game.h"
#include "coop_mod_share.h"
#include "coop_pack_notice.h"
#include "coop_name_privacy.h"
#include "coop_scene_probe.h"
#include "coop_camera.h"
#include "coop_colors.h"
#include "coop_chat.h"
#include "coop_hud.h"  // the HUD's hearts (PLAYER_LIFE), docs/overlay.md
#include "coop_cheats.h"
#include "coop_chests.h"
#include "coop_flags.h"
#ifdef COOP_HOOKSHOT
#include "coop_flag_native.h"
#include "coop_map_page.h"  // patch 0018's menu state: PLAYER_STATUS in_menu/paused (Read)
#endif
#include "coop_save.h"
#include "coop_game_build.h"
#include "coop_game_scan.h"
#include "coop_pickups.h"
#include "coop_fairy.h"
#include "coop_difficulty.h"
#include "coop_ngplus.h"
#include "coop_drops.h"
#include "coop_every.h"
#include "coop_event_owner.h"  // G1: whose the running event is
#include "coop_event_result.h"  // row 98(2): another player's gimmick event gives our room its result, no replay
#include "coop_dungeon.h"
#include "coop_quest.h"
#include "coop_story.h"
#include "coop_quests.h"
#include "coop_eye_switch.h"
#include "coop_live_events.h"
#include "coop_local_game.h"
#include "coop_local_net.h"
#include "coop_shared_scenes.h"
#include "coop_solo_view.h"
#include "coop_session_end.h"
#include "coop_boss_native.h"
#include "coop_objects.h"
#include "coop_link_sound.h"
#include "coop_look_share.h"
#include "coop_ocarina_sound.h"
#include "coop_pose.h"
#include "coop_puppet.h"
#include "coop_puppet_own.h"
#include "coop_ride.h"
#include "coop_voice.h"
#include "coop_settings.h"
#include "coop_spawn.h"
#include "coop_scrub_sale.h"
#include "coop_shop_sale.h"
#include "coop_scrub_talk.h"
#include "coop_sites.h"
#include "coop_svc.h"
#include "coop_map_refresh.h"
#include "coop_time.h"
#include "coop_testinput.h"
#include "coop_boundary.h"
#include "coop_world.h"

namespace CoopHook {
bool StartMapRefresh();  // rows 63/76: the game's HUD refresh after co-op writes a dungeon item (below)
namespace {

// The game and build the addresses in coop/host are for.
constexpr u64 kOcarinaProgramId = 0x0400486024DEC000ULL;
// The builds it knows: coop_game_build.h (docs/game-builds.md).
constexpr std::string_view kGameBuildId = CoopGameBuild::kPrimary.id;
constexpr std::string_view kGameVersion = CoopGameBuild::kPrimary.version;
// The client this game runs the module in, for HELLO's build_id (docs/crossplay.md): the server writes it beside the
// player's name in its log, so a problem between two clients can be told apart. Nothing is decided by it. The Fritchii
// emulator (stage 1): "coop fritchii-v5 <version>"; the Hookshot/Longshot module (COOP_HOOKSHOT, whose
// LICHII_COOP_VERSION starts with "hookshot "): "coop hookshot <version>".
#ifdef COOP_HOOKSHOT
constexpr const char* kBuildId = "coop " LICHII_COOP_VERSION;
#else
constexpr const char* kBuildId = "coop fritchii-v5 " LICHII_COOP_VERSION;
#endif

// A frame older than this means the game is not running (stopped, paused).
constexpr auto kFrameStale = std::chrono::seconds(1);
// A camera older than this is from a scene no longer drawn (a menu, a load).
constexpr auto kCameraStale = std::chrono::milliseconds(250);
// With LICHII_COOP_DEBUG_CAMERA, how often the local player's projection is logged.
constexpr std::uint64_t kDebugLogUs = 2 * CoopEvery::kSecondUs;

// The overlay's defaults are for centimetres; Ocarina's world unit is about a
// metre (the first run: a child Link walks about 2.5 units a second and the
// camera follows him about 4 units behind). So markers are 24 px across at
// 10 units, and nothing nearer than half a unit is drawn. Name tags are
// solid up to 10 units away, fade out by 40 and are not drawn beyond; the
// edge arrow for an off-screen player stays (docs/lichii.md, Name tags).
coop::overlay::Settings MarkerSettings() {
    coop::overlay::Settings s;
    s.reference_distance = 10.0f;
    s.near_distance = 0.5f;
    s.tag_gap = 4.0f;
    s.fade_start = 10.0f;
    s.fade_end = 40.0f;
    return s;
}

// The top of Link's head above his feet, in world units, by age: the name
// tag floats just over it (measured on screen, docs/lichii.md, Name tags).
constexpr float kHeadChild = 1.35f;
constexpr float kHeadAdult = 1.9f;

float HeadHeight(std::optional<coop::protocol::Age> age) {
    return age == coop::protocol::Age::adult ? kHeadAdult : kHeadChild;
}

// An environment variable that is set and not "0".
bool EnvFlag(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return false;
    }
    const bool on = value[0] != '\0' && std::string_view(value) != "0";
    std::free(value);
    return on;
#else
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' && std::string_view(value) != "0";
#endif
}

// An environment variable's text, or "" when it is not set.
std::string EnvString(const char* name) {
#ifdef _MSC_VER
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

const char* WhyText(coop::overlay::Why why) {
    using coop::overlay::Why;
    switch (why) {
    case Why::invalid:
        return "invalid";
    case Why::faded:
        return "faded";
    case Why::other_scene:
        return "other scene";
    case Why::behind:
        return "behind the camera";
    case Why::too_near:
        return "too near";
    case Why::too_far:
        return "too far";
    case Why::off_screen:
        return "off screen";
    }
    return "?";
}

using Clock = std::chrono::steady_clock;

// The calc thread's clock (coop_svc.cpp), so pose times line up.
std::uint64_t SteadyMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
            .count());
}

std::string Hex(const std::array<u8, 0x20>& id, std::size_t bytes) {
    std::string out;
    char buf[3];
    for (std::size_t i = 0; i < bytes; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", id[i]);
        out += buf;
    }
    return out;
}

// What OnFrame found, for the window.
// minimap-b (defined below, near LocalFrameOff).
void WatchMapDrops(EmulatorMemory& memory, bool in_play, const std::optional<std::string>& scene,
                   const std::optional<std::array<float, 3>>& link);

struct FrameView {
    Clock::time_point at{};
    bool game = false;               // Ocarina of the right build
    std::string game_note;           // why not, when not
    std::optional<coop::protocol::Vec3> position;
    std::optional<std::string> scene;
    std::optional<coop::protocol::Age> age;
    std::vector<coop::overlay::Shown> others;
    std::vector<std::uint8_t> hidden;  // others with no marker (scene.hpp: hide)
    std::vector<std::uint8_t> unheard; // others whose voice is not heard (scene.hpp: voice_heard; no event counts)
    std::optional<Markers> markers;  // none: no camera
    std::string camera_note;         // for the panel, when the game runs
    bool event = false;              // our own event hides the others (and the game its HUD)
};

// What the network thread knows, for the window.
struct RoomView {
    coop::net::State state = coop::net::State::idle;
    std::uint8_t self = 0;
    std::optional<std::uint64_t> round_trip_us;
    std::vector<coop::net::Player> players;
    std::string note;  // the last refusal or error
    bool encrypted = false;  // docs/protocol.md "Encrypted traffic"
};

// Issue 84: a one-way story flag back on the wrong side in a game whose own
// quest proves the right value (Mido blocks again past him; the Deku Tree's
// mouth shut with the quest Complete): a shared value from a save behind the
// group put it there. Set back through the game's Bool setter
// (CoopStory::Mends says which and when). Held 3 s first, so a host save load
// whose quest and scene flags land a few frames after the flag is not fought.
// In a session (Coop::CatchUpQuest) and with none (LocalFrameOff), so a save
// stuck that way is mended when it is played alone too. `clocks`: the
// caller's own hold clocks.
//
// Row 129 (GitHub #173): then what a shared story event sets in the game that
// plays it, in a game where its played flag came from another game and its
// flow never ran (CoopStory::Effects: the owl's HyruleField_ArriveEntrance,
// without which the game's clock never starts). Only while no event runs here
// (`idle`: known to run none), so an event this game plays itself sets its own
// flags first, and after the same 3 s hold.
struct MendClocks {
    std::array<std::uint64_t, CoopStory::kMends> mends{};
    // Row 140 (flag-oneway-c): each mend at most once per load, like the effects: one the game sets back after we
    // mended it is left alone (said once), not fought every kHoldUs.
    std::array<bool, CoopStory::kMends> mended{};
    std::array<bool, CoopStory::kMends> set_back_said{};
    std::array<std::uint64_t, CoopStory::kStepFloors> floors{};  // row 145
    std::uint64_t effects = 0;
    CoopStory::EffectsGiven given;  // once per flag per load (review of owl-time)
    void NewLoad() {  // the title or a load: nothing to read yet
        mends = {};
        mended = {};
        set_back_said = {};
        floors = {};
        effects = 0;
        given.NewLoad();
    }
};
// Row 140: a flag key for the log: a named flag's hash (and element, and name when known), or a 64-bit key.
std::string FlagKeyText(const coop::host::FlagKey& key, const coop::host::ItemFlag* item) {
    if (key.kind != coop::protocol::FlagKeyKind::named) return fmt::format("key64 {:#018x}", key.key);
    std::string text = fmt::format("{:#010x}", key.hash);
    if (key.index != 0) text += fmt::format("[{}]", key.index);
    if (item != nullptr) text += fmt::format(" {}", item->name);
    return text;
}

void MendStory(EmulatorMemory& memory, MendClocks& clocks, bool idle) {
    static constexpr std::uint64_t kHoldUs = 3'000'000;
    if (!CoopStory::On()) return;
    auto& since_us = clocks.mends;
    namespace quest = CoopStory::quest;
    coop::host::GameFlags flags(memory);
    const auto step = flags.get_enum(quest::kMeetDekuTree);
    const auto met = flags.get_bool(quest::kMetDekuTree);
    const std::uint64_t now_us = CoopEvery::NowUs();
    for (std::size_t i = 0; i < CoopStory::kMends; ++i) {
        const auto& mend = CoopStory::Mends()[i];
        auto& since = since_us[i];
        if (!mend.due(step, flags.get_bool(mend.flag), met)) {
            since = 0;
        } else if (clocks.mended[i]) {
            if (!std::exchange(clocks.set_back_said[i], true)) {
                LOG_INFO(Core, "coop: story: {}: set back after we mended it: left alone until the next load (row 140)",
                         mend.what);
            }
        } else if (since == 0) {
            since = now_us;
        } else if (now_us - since >= kHoldUs && coop::host::GameFlags::set_bool(memory, mend.flag, mend.value)) {
            since = 0;
            clocks.mended[i] = true;
            LOG_INFO(Core, "coop: story: {} (Meet_DekuTree {:#x}): set to {} (issue 84)", mend.what,
                     step.value_or(0), mend.value ? 1 : 0);
        }
    }
    // Row 145: a quest step the game's own flows set back below what a played scene proves (King Zora back in
    // front of the fountain after the diving game or Ruto's Letter got or read). Only outside events, so the
    // event that moves the quest on sets its own step first; the game's Enum setter, after the same hold.
    // One quest's rows are listed highest first: only the first one due raises (the Enum write may land a frame
    // later, so a lower row must not write over it).
    std::array<std::uint32_t, CoopStory::kStepFloors> raising{};
    std::size_t raising_n = 0;
    for (std::size_t i = 0; i < CoopStory::kStepFloors; ++i) {
        const auto& floor = CoopStory::StepFloors()[i];
        auto& since = clocks.floors[i];
        const auto at = flags.get_enum(floor.quest);
        auto raise = idle ? CoopStory::StepFloorDue(floor, at, flags.get_bool(floor.proof)) : std::nullopt;
        if (raise) {
            const auto taken = raising.begin() + static_cast<std::ptrdiff_t>(raising_n);
            if (std::find(raising.begin(), taken, floor.quest) != taken) {
                raise.reset();
            } else {
                raising[raising_n++] = floor.quest;
            }
        }
        if (!raise) {
            since = 0;
        } else if (since == 0) {
            since = now_us;
        } else if (now_us - since >= kHoldUs && coop::host::GameFlags::set_enum(memory, floor.quest, *raise)) {
            since = 0;
            LOG_INFO(Core, "coop: story: quest {:#010x} {:#010x} -> {:#010x}: {} (row 145)", floor.quest,
                     at.value_or(0), *raise, floor.what);
        }
    }
    std::array<const CoopStory::Effect*, CoopStory::kEffectsMax> due{};
    const std::size_t n = idle ? CoopStory::EffectsDue([&](std::uint32_t h) { return flags.get_bool(h); }, due) : 0;
    if (n == 0) {
        clocks.effects = 0;
    } else if (clocks.effects == 0) {
        clocks.effects = now_us;
    } else if (now_us - clocks.effects >= kHoldUs) {
        clocks.effects = 0;
        for (std::size_t i = 0; i < n; ++i) {
            switch (clocks.given.Take(*due[i])) {
            case CoopStory::EffectsGiven::Verdict::give:
                if (coop::host::GameFlags::set_bool(memory, due[i]->flag, due[i]->value)) {
                    clocks.given.Given(*due[i]);  // only once written: a failed write is tried again
                    LOG_INFO(Core, "coop: story: {} set to {}: {} was played here by another game (row 129)",
                             due[i]->name, due[i]->value ? 1 : 0, due[i]->setting);
                }
                break;
            case CoopStory::EffectsGiven::Verdict::set_back:
                LOG_INFO(Core, "coop: story: {} was set back after we gave it ({}): left alone until the next load",
                         due[i]->name, due[i]->setting);
                break;
            case CoopStory::EffectsGiven::Verdict::quiet:
                break;
            }
        }
    }
}

// G1 (coop/host/event_owner.hpp): whose the running event is, logged on each change (with what
// decided it). Once a frame, from the session's frame or the frame without one (only one runs).
coop::host::EventWhose ClassifyEvent(bool running, bool link_bound, const std::optional<std::string_view>& anim) {
    static std::uint64_t cursor = 0;
    static int said = -1;
    static int said_by = -1;
    for (const auto& r : CoopLiveEvents::TheGameRequests().Since(cursor)) {
        if (r.taken) CoopEventOwner::GameAsked(r.name);
    }
    coop::host::EventOwnerTracker::By by{};
    const auto whose = CoopEventOwner::Classify(running, link_bound, anim, &by);
    const int now = running ? static_cast<int>(whose) : -1;
    if (now != said || (now >= 0 && static_cast<int>(by) != said_by)) {
        if (now >= 0) {
            LOG_INFO(Core, "coop: event: the running event is {} ({}){}", coop::host::whose_name(whose),
                     coop::host::by_name(by),
                     whose == coop::host::EventWhose::ours
                         ? ": our Link is in it, the others are hidden, holds apply"
                         : ": our Link is not held by it, no hide, no hold, no claim; its actors here act for it");
        }
        said = now;
        said_by = static_cast<int>(by);
    }
    return whose;
}

void ApplyEventOwner(coop::host::PlayerReading& reading) {
    // Review of e3542a8a: a scene change drops a request still waiting for its run. The session's reading has the
    // scene already; the frame without a session (co-op idle) reads nothing more for it.
    if (reading.scene) CoopEventOwner::Scene(*reading.scene);
    reading.event_whose = ClassifyEvent(
        reading.event_running.value_or(false), reading.link_bound,
        coop::host::name_of(reading.animation));
}

class Coop {
public:
    // main's address in a game process (split screen with co-op off: LocalFrameOff below).
    static u64 MainOf(Core::System& system, Kernel::KProcess* process) { return FindMain(system, process); }
    explicit Coop(Settings settings)
        : settings_(std::move(settings)), debug_camera_(EnvFlag("LICHII_COOP_DEBUG_CAMERA")),
          flip_y_(EnvFlag("LICHII_COOP_CAMERA_FLIP_Y")) {
        load_mend_.set_clock([] { return CoopEvery::NowUs(); });  // issue 106: 1/60 s, not presents
        if (debug_camera_ || flip_y_) {
            LOG_INFO(Core, "coop: camera debug {}, flip y {}", debug_camera_ ? "on" : "off",
                     flip_y_ ? "on" : "off");
        }
    }

    bool Begin() {
        coop::net::ClientConfig config;
        config.host = settings_.server;
        config.port = settings_.port;
        config.hello.build_id = coop::protocol::clamp_build_id(kBuildId);   // (a long version text: cut, never unsendable)
        config.hello.game_build_id = std::string(kGameBuildId);
        config.hello.game_version = std::string(kGameVersion);
        config.hello.nickname = settings_.name;
        config.player_key = settings_.player_key;
        config.join_password = settings_.password;
        client_ = std::make_unique<coop::net::Client>(std::move(config));
        if (!client_->start()) {
            LOG_ERROR(Core, "coop: ENet did not start; co-op is off");
            client_.reset();
            return false;
        }
        coop::host::SessionSettings session;
        session.game_frame_age_us = [] { return CoopFrameClock::PresentAgeUs(); };  // jitter-b
        // The player's colour goes out in PLAYER_EQUIP (docs/puppet.md, "Colour").
        session.sampler.color = settings_.color;
        CoopPuppet::SetOwnColor(settings_.color);
        LOG_INFO(Core, "coop: colour {}", settings_.color ? CoopColors::Text(*settings_.color)
                                                          : std::string("none chosen (slot colour)"));
        std::string flag_error;
        session.shared = LoadShared(flag_error);
        if (flag_error.empty()) CoopDungeon::AddShared(session.shared);  // docs/dungeon.md
        if (flag_error.empty()) CoopStory::AddShared(session.shared);  // docs/flags.md, "Story and cutscenes"
        if (flag_error.empty()) CoopQuests::AddShared(session.shared);  // docs/flags.md, "Threads of Time"
        if (flag_error.empty()) CoopChests::AddShared(session.shared);  // docs/items.md, "Chests"
        if (flag_error.empty() && scrub_sale_on_) {  // docs/items.md, "One-time sales"
            const std::size_t added = CoopScrubSale::Merge(session.shared);
            LOG_INFO(Core, "coop: scrubs: sharing {} one-time sale flags (off with LICHII_COOP_SCRUB_SALE=0)", added);
        }
        // docs/items.md "One-time sales": a randomizer seed with shops sells each one-time shelf once for the group.
        shop_sale_ = shop_sale_on_ && CoopShopSale::SeedHasShops();
        if (flag_error.empty() && shop_sale_on_) {  // the randomizer's Great Fairy gifts: given once for the group
            const std::size_t added = CoopShopSale::MergeGifts(session.shared);
            LOG_INFO(Core, "coop: fairies: sharing {} randomizer Great Fairy gift flags", added);
        }
        if (flag_error.empty() && shop_sale_) {
            const std::size_t added = CoopShopSale::Merge(session.shared);
            LOG_INFO(Core, "coop: shops: the seed has shops: sharing {} shop sold counters by the highest value "
                           "(off with LICHII_COOP_SHOP_SALE=0)", added);
        }
        if (!flag_error.empty()) {
            LOG_ERROR(Core, "coop: {}; flags are not shared", flag_error);
        }
        sharing_ = session.shared.has_value();
        // One-time pickups (docs/items.md): keys the object walk notes are
        // shared as they are seen, and another game's taken key is taken here
        // too. LICHII_COOP_PICKUPS=0 turns both off.
        pickups_ = sharing_ && !(std::getenv("LICHII_COOP_PICKUPS") != nullptr &&
                                 std::string_view(std::getenv("LICHII_COOP_PICKUPS")) == "0");
        session.learn_pickups = pickups_;
        // Issue 106: the flag watcher's frames are 1/60 s of real time, not
        // presents (high refresh presents up to 165 a second).
        session.clock = [] { return CoopEvery::NowUs(); };
        pickups_given_ = 0;
        // Called by service() on the network thread, with the client.
        session.on_event = [this](const coop::net::Event& event, coop::net::Client& client) {
            OnSessionEvent(event, client, false);
        };
        CoopPose::SetLog([](const std::string& line) { LOG_INFO(Core, "coop: {}", line); },
                         EnvFlag("LICHII_COOP_DEBUG_POSE"), EnvFlag("LICHII_COOP_POSE_AS"));
        session_ = std::make_unique<coop::host::Session>(*client_, std::move(session));
        if (pickups_ && CoopDungeon::On()) {  // docs/dungeon.md: chests, silver rupees, cleared rooms stay done
            for (const auto& k : CoopDungeon::Keys()) {
                session_->share_pickup(coop::protocol::FlagKey::key64_flag(k.key));
            }
        }
        // On unless LICHII_COOP_LIVE_EVENTS=0 (dungeon-switches): another
        // player's gimmick event plays here too when our Link comes near it.
        live_events_ = EnvString("LICHII_COOP_LIVE_EVENTS") != "0";
        if (live_events_) {
            // LICHII_COOP_LIVE_EVENT_NAMES=a,b,...: only these events (default
            // and "all": every held gimmick event with a known place).
            const std::string names = EnvString("LICHII_COOP_LIVE_EVENT_NAMES");
            if (!names.empty()) {
                if (auto parsed = CoopLiveEvents::ParseNames(names)) {
                    live_decider_ = CoopLiveEvents::Decider(std::move(*parsed));
                } else {
                    live_events_ = false;
                    LOG_INFO(Core, "coop: dungeon: LICHII_COOP_LIVE_EVENT_NAMES names no event: live events off");
                }
            }  // issue 129: start another player's gimmick event here (coop_live_events.h)
        }
        map_refresh_ = StartMapRefresh();  // rows 63/76, #146: the HUD's own copy of the map (coop_map_refresh.h)
        if (CoopStory::On()) {  // issue 171: shared quest steps into the game's ChallengeMgr
            CoopSvc::AddCaller(CoopCallerOrders::kStory, [this](CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                          std::uint64_t) {
                return challenges_.Step(guest, after_call, result, base_);
            });
        }
        // Rows 93/103: the game equips a shared ocarina from its flags (OneShotPlayerEquipDynamicEquipmentFromGameData's
        // call), once after the mend wrote them, outside events (coop::host::OcarinaEquip).
        CoopSvc::AddCaller(CoopCallerOrders::kOcarinaEquip, [this](CoopPuppet::Guest& guest, bool after_call, std::uint64_t,
                                      std::uint64_t) -> std::optional<CoopPuppet::Call> {
            namespace oc = coop::host::ocarina;
            const auto slot = ocarina_equip_.step(after_call);
            if (!slot) return std::nullopt;
            const auto actor = CoopObjects::OurLinkActor();
            std::uint64_t holder = 0, equipment = 0;
            if (!actor || base_ == 0 || !guest.Read(*actor + oc::kActorEquipment, &holder, sizeof holder) ||
                holder == 0 || !guest.Read(holder + oc::kHolderEquipment, &equipment, sizeof equipment) ||
                equipment == 0) {
                ocarina_equip_.not_now();
                return std::nullopt;
            }
            CoopPuppet::Call c;
            c.function = base_ + oc::kEquipFromGameData;
            c.x[0] = equipment;
            c.x[1] = *slot;
            return c;
        });
        // Test only (`dress`, tools/autotest): the game's own equip from the PlayerStatus.Equip* flags for our Link,
        // as the pause menu's equip and a save load make it, so a `wear` shows on our own Link too. Nothing is asked
        // unless a scenario's step queued a kind (CoopTestInput::DressAsk); a player's game never does.
        CoopSvc::AddCaller(CoopCallerOrders::kTestDress, [this](CoopPuppet::Guest& guest, bool, std::uint64_t,
                                      std::uint64_t) -> std::optional<CoopPuppet::Call> {
            namespace oc = coop::host::ocarina;
            const auto actor = CoopObjects::OurLinkActor();
            std::uint64_t holder = 0, equipment = 0;
            if (!actor || base_ == 0 || !guest.Read(*actor + oc::kActorEquipment, &holder, sizeof holder) ||
                holder == 0 || !guest.Read(holder + oc::kHolderEquipment, &equipment, sizeof equipment) ||
                equipment == 0) {
                return std::nullopt;  // (the asks wait for a Link)
            }
            const auto kind = CoopTestInput::DressNext();
            if (!kind) return std::nullopt;
            CoopPuppet::Call c;
            c.function = base_ + oc::kEquipFromGameData;
            c.x[0] = equipment;
            c.x[1] = *kind;
            return c;
        });
        if (live_events_) {
            {
                // G1: the events this requester asks for (a held live event, a shared scene joined)
                // and the eye switches set for another game's event, for the log only. Whose a running
                // event is comes from our Link's event membership (coop/host/event_owner.hpp).
                std::lock_guard lock(live_mutex_);
                const auto mark = [](const std::string& name) {
                    LOG_INFO(Core, "coop: event: our game was asked for another player's event {}", name);
                };
                live_requester_.OnTaken(mark);
                eye_switch_.OnSet(mark);
            }
            CoopSvc::AddCaller(CoopCallerOrders::kLiveEvents, [this](CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                          std::uint64_t) {
                std::lock_guard lock(live_mutex_);
                auto call = live_requester_.Step(guest, after_call, result, base_);
                for (const auto& line : live_requester_.TakeNotes()) {
                    LOG_INFO(Core, "coop: dungeon: {}", line);
                }
                return call;
            });
            // Row 98(2): our asker's result outputs of a held gimmick event, before the switches (59).
            CoopSvc::AddCaller(CoopCallerOrders::kEventResult, [this](CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                          std::uint64_t now_ms) {
                auto call = event_result_.Step(guest, after_call, result, base_, now_ms);
                for (const auto& line : event_result_.TakeNotes()) {
                    LOG_INFO(Core, "coop: dungeon: {}", line);
                }
                return call;
            });
            LOG_INFO(Core, "coop: dungeon: live events on (off with LICHII_COOP_LIVE_EVENTS=0)");
            eye_switch_on_ = CoopEyeSwitch::OnFromEnv(EnvString("LICHII_COOP_EYE_SWITCH"));
            if (eye_switch_on_) {  // deku-switches: before the requester (60)
                CoopSvc::AddCaller(CoopCallerOrders::kEyeSwitch, [this](CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                              std::uint64_t now_ms) {
                    auto call = eye_switch_.Step(guest, after_call, result, base_, now_ms);
                    for (const auto& line : eye_switch_.TakeNotes()) {
                        LOG_INFO(Core, "coop: dungeon: {}", line);
                    }
                    return call;
                });
            }
            LOG_INFO(Core, "coop: dungeon: eye switches by FirstOn {} (LICHII_COOP_EYE_SWITCH=1)",
                     eye_switch_on_ ? "on" : "off");
        }
        // Shared story cutscenes: they start by the live-event requester.
        shared_scenes_ = live_events_ && CoopSharedScenes::OnFromEnv(EnvString("LICHII_COOP_SHARED_CUTSCENES"));
        LOG_INFO(Core, "coop: cutscene: shared story cutscenes {}",
                 shared_scenes_ ? "on (off with LICHII_COOP_SHARED_CUTSCENES=0)" : "off");
        if (CoopStory::On()) {  // issue 84: story flags the game moves one way never go back here
            for (const auto& f : CoopStory::OneWays()) {
                const coop::protocol::FlagKey key(f.hash, std::uint16_t{0});
                if (f.way == CoopStory::Way::down) {
                    session_->share_down_only(key);
                } else {
                    session_->share_up_only(key);
                }
            }
        }
        if (CoopDungeon::On()) {  // room switch flags the game resets itself: one way, no load write-back
            for (const auto& f : CoopDungeon::LiveFlags()) {
                session_->share_one_way(coop::protocol::FlagKey(f.hash, std::uint16_t{0}));
            }
        }
        if (pickups_ && CoopChests::On()) {  // one way: an opened chest never closes again
            for (const auto& k : CoopChests::Keys()) {
                session_->share_pickup(coop::protocol::FlagKey::key64_flag(k.key));
            }
            // What a real opening sets (issue 87): the chest's own Bool.
            for (const auto& o : CoopChests::OpenedFlags()) {
                session_->share_pickup_named(coop::protocol::FlagKey(o.flag, std::uint16_t{0}));
            }
        }
        if (sharing_ && scrub_sale_on_) {  // a Business Scrub's one-time sale: bought once for everyone
            for (const auto& f : CoopScrubSale::kSaleFlags) {
                session_->share_pickup_named(coop::protocol::FlagKey(f.hash, std::uint16_t{0}));
            }
        }
        if (sharing_ && shop_sale_on_) {  // a Great Fairy's shuffled gift (coop_shop_sale.h): given once for everyone
            for (const auto& f : CoopShopSale::kFairyGifts) {
                session_->share_pickup_named(coop::protocol::FlagKey(f.hash, std::uint16_t{0}));
            }
        }
        if (pickups_) {  // issue 89: every Gold Skulltula's key, and the token count guarded by them
            std::vector<coop::protocol::FlagKey> skulltulas;
            for (const std::uint64_t key : coop::host::gold_skulltula_keys()) {
                skulltulas.push_back(coop::protocol::FlagKey::key64_flag(key));
            }
            session_->guard_count(coop::protocol::FlagKey(coop::host::kGoldTokenCount, std::uint16_t{0}),
                                  skulltulas);
        }
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    // Each event from the server, to every part of co-op. `left`: the player left from the menu while connected
    // (Run's end): the client sends no event then, so the parts get a `disconnected` here and forget the
    // session as on a drop (the difficulty, PvP, down but not out, ...), and playing on alone is the game as
    // it is. Not logged or put in the chat as a lost connection. The network thread, with the client.
    void OnSessionEvent(const coop::net::Event& event, coop::net::Client& client, bool left) {
        if (!left) {
            Log(event);
        }
        CoopPuppet::OnEvent(event);
        CoopPose::OnEvent(event, SteadyMs());
        if (!left) {
            Chat(event, client);
        }
        CoopObjects::OnEvent(event);
        CoopSpawn::OnEvent(event);
        CoopDrops::OnEvent(event);
        CoopTime::OnEvent(event);
        CoopBoss::OnEvent(event);  // docs/dungeons-boss.md
        CoopTeleport::OnEvent(event, client);  // /teleport (docs/setup.md "Chat commands")
        CoopPvp::OnEvent(event, client);  // PvP (docs/play-online.md "PvP")
        CoopLiftPlayerGame::OnEvent(event, client);  // native puppets phase 3 (LICHII_COOP_LIFT_PLAYER=1)
        CoopPvpNativeGame::OnEvent(event, client);  // native puppets phase 4 (LICHII_COOP_PVP_NATIVE=1)
        CoopDbno::OnEvent(event, client);  // down but not out (docs/play-online.md)
        CoopDifficulty::OnEvent(event, client);  // docs/play-online.md "Difficulty"
        CoopModShare::OnEvent(event, client);  // mods (docs/mods.md "Mods in co-op")
        CoopSave::OnEvent(event, client);  // the host's save for the guests (issue 167)
        CoopHud::OnEvent(event);  // the others' hearts for the HUD (PLAYER_LIFE)
        CoopLookShare::OnEvent(event, client);  // looks of several parts (PLAYER_LOOK, coop_look_share.h)
        if (event.kind == coop::net::EventKind::message) {  // deku-dungeon-2
            if (const auto* e = std::get_if<coop::protocol::DungeonEvent>(&event.message)) {
                std::lock_guard lock(dungeon_events_mutex_);
                // A story cutscene's start (coop_shared_scenes.h) has its own queue.
                auto& in = CoopSharedScenes::IsStory(e->name) ? story_events_in_ : dungeon_events_in_;
                if (in.size() < 16) {
                    in.push_back(*e);
                }
            }
        }
        CoopRide::OnEvent(event);  // their horses (PLAYER_RIDE, issue 199)
        CoopLobby::OnEvent(event, client);  // the Lobby's mutes and the host's actions
        // Their voices (VOICE_FRAME, docs/voice.md), but one we muted ourselves (the Lobby).
        if (const auto* v = event.kind == coop::net::EventKind::message
                                ? std::get_if<coop::protocol::VoiceFrame>(&event.message)
                                : nullptr;
            v == nullptr || !CoopLobby::VoiceHidden(v->player_number)) {
            CoopVoice::OnEvent(event);
        }
        CoopLocalNet::OnEvent(event);  // split screen's seats in the session (coop_local_net.h)
    }

    // `quiet`: no logging, for the end of the program, when the log may be gone.
    void End(bool quiet) {
        CoopCamera::SetWanted(false);
        quiet_ = quiet;
        stop_ = true;
        if (session_) {
            session_->wake();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Frame(Core::System& system) {
        CoopFrameClock::BeginPresent();  // jitter-b: the game frame this present reads
        FrameView view;
        view.at = Clock::now();
        const auto reading = Read(system, view);

        // The game's flags, when it is the right game and a list is shared.
        std::optional<EmulatorMemory> memory;
        // Item flags are merged, not overwritten (ItemFlagStore, docs/items.md).
        std::optional<coop::host::GameFlagStore> game_store;
        std::optional<coop::host::ItemFlagStore> store;
        if (view.game) {
            memory.emplace(system.ApplicationMemory(), base_);
            flag_debug_.Frame(*memory);
            // Down but not out (row 162): a bottled fairy is the game's own revive at no life.
            if (reading) {
                coop::host::GameFlags bottles(*memory);
                CoopDbno::NoteBottledFairy(coop::host::bottled_fairy(
                    [&](std::uint32_t h) { return bottles.get_bool(h); },
                    [&](std::uint32_t h) { return bottles.get_enum(h); }));
            }
            if (sharing_ || cheats_) {
                game_store.emplace(*memory);
                if (!queue_sizes_logged_) {  // once: half of each is ours (issue 175)
                    if (const auto q = coop::host::GameFlags(*memory).queue_sizes()) {
                        queue_sizes_logged_ = true;
                        const std::uint32_t least =
                            std::min({q->boolean, q->int32, q->uint32, q->enumeration, q->key64});
                        LOG_INFO(Core,
                                 "coop: flags: the game's write queues hold Bool {}, Int {}, UInt {}, Enum {}, "
                                 "64-bit keys {} slots; ours is half of each{}",
                                 q->boolean, q->int32, q->uint32, q->enumeration, q->key64,
                                 least < 2 ? " (a queue under 2 slots gets no write of ours)" : "");
                    }
                }
            }
            if (cheats_) {
                CoopCheats::Frame(*game_store);
            }
            if (sharing_ && load_mend_.ready() && CoopStory::On() &&
                quest_catch_up_.Due(CoopEvery::NowUs())) {
                CatchUpQuests(*memory, reading && reading->event_running.value_or(false));  // issues 173, 171
            }
            if (sharing_) {
                // A small key or token we and another player took at once: ours
                // goes back (docs/world.md, small keys and tokens). No key is
                // written for another game's token: its own placement key is
                // the game's to write, and it arrives through the flag list.
                for (const std::string& count : CoopWorld::TakeGiveBacks()) {
                    LOG_INFO(Core, "coop: {}", CoopWorld::GiveBack(*game_store, count));
                }
            }
            if (sharing_) {
                // Issue 138: grants wait for a loaded save, and the capacities
                // of held items are mended for a while after each load.
                const bool mend = load_mend_.frame(reading.has_value());
                if (!reading) mend_since_us_.NewLoad();  // the story effects: once per flag per load (owl-time)
                CoopSave::NoteInGame(reading.has_value());  // issue 167: no save-folder switch once a save is loaded
                store.emplace(*game_store, &load_mend_);
                if (mend) {
                    if (const auto raised = store->mend(); raised != 0) {
                        LOG_INFO(Core, "coop: items: {} capacities mended after a load", raised);
                    }
                }
                // Issue 125: a shared bracelet or gauntlets give the lift
                // grade too, or every pulled bomb is deleted at once.
                if (load_mend_.ready()) {
                    MendLiftGrade(*memory);
                    MendDiveDepth(*memory);  // a shared scale dives as deep
                    // Issue 190: a shared ocarina is equipped too, or D-pad
                    // Left takes nothing out.
                    MendOcarina(*memory, reading && reading->event_running.value_or(false));
                    // issue 58: Zelda's Letter spent, Ruto's Letter for everyone;
                    // row 117: Ruto's Letter spent once shown in any game
                    MendLetters(*memory, reading && reading->event_running.has_value() &&
                                             !*reading->event_running);
                    if (CoopStory::On()) {
                        CatchUpQuest(*memory, reading && reading->event_running.has_value() &&
                                                  !*reading->event_running);  // issue 130: the Kokiri Shop opens
                    }
                } else {
                    ocarina_equip_.link(false, {});  // a load or scene change: the next stay asks again (row 103)
                }
            }
        }
        if (pickups_) {
            const auto keys = CoopPickups::Since(pickups_given_);
            for (const std::uint64_t key : keys) {
                session_->share_pickup(coop::protocol::FlagKey::key64_flag(key));
                LOG_INFO(Core, "coop: flags: one-time pickup {:#x} is shared from now on", key);
            }
            pickups_given_ += keys.size();
        }
        // Issue 129 (docs/dungeon.md "Held until the room loads"): a room
        // gimmick's played event, or the Forest Temple corridor, waits while
        // our Link is in that dungeon; written when we are out or loading.
        std::optional<coop::host::HoldFlagStore> hold;
        // Issue 193: the Threads of Time quests, ordered (coop_quests.h).
        std::optional<CoopQuests::Store> quests;
        // The randomizer's one-time shelves (coop_shop_sale.h): their sold counters by the highest value.
        std::optional<CoopShopSale::Store> shop_view;
        // dungeon-sync-b (#75): small keys lent at a lock another player opened
        // stay in our game; the watcher sees the counts without them.
        std::optional<coop::host::LentKeyStore> lent_view;
        if (store) {
            for (const auto& [count, back] : CoopObjects::TakeKeyLends()) {
                if (back) {
                    lent_keys_.give_back(coop::protocol::FlagKey{count, 0});
                } else {
                    lent_keys_.lend(coop::protocol::FlagKey{count, 0});
                }
            }
            for (const std::string& line : lent_keys_.frame(*store)) {
                LOG_INFO(Core, "coop: {}", line);
            }
            lent_view.emplace(*store, lent_keys_);
            quests.emplace(
                *lent_view,
                [&memory](std::uint32_t hash) { return coop::host::GameFlags(*memory).get_enum(hash); },
                [&memory](std::uint32_t hash, std::uint32_t value) {
                    return coop::host::GameFlags::set_enum(*memory, hash, value);
                },
                // row 150: Get_Epona's end waits only while a race against Ingo runs here
                [&memory](std::uint32_t hash) { return coop::host::GameFlags(*memory).get_bool(hash); });
            // Released only after a real load (0.5 s without Link), not on one
            // frame the reader missed; the scene as last read. Timed, not
            // counted in presents: high refresh (Ctrl+U) presents up to 2.75
            // times as many frames (issue 106).
            if (reading) {
                no_link_since_.reset();
            } else if (!no_link_since_) {
                no_link_since_ = view.at;
            }
            if (view.scene) {
                hold_scene_ = *view.scene;
            }
            const bool link = !no_link_since_ || view.at - *no_link_since_ < kHoldRelease;
            const std::optional<std::string> scene =
                hold_scene_.empty() ? std::nullopt : std::optional<std::string>(hold_scene_);
            // Issue 144 (coop_scrub_talk.h): a scrub's destroy flag waits while our
            // Link is in an event (its talk), or the room graph deletes the scrub mid-talk.
            // G1: only an event our Link is in (not another player's replayed here, not the Ocarina).
            if (scrub_talk_on_ && !scrub_near_only_ && scrub_talk_.Frame(OurLinkInEvent(reading),
                                  CoopEvery::NowUs())) {
                LOG_INFO(Core, "coop: dungeon: {}", scrub_talk_.Holding()
                             ? "our Link is in an event: Deku Scrub destroy flags from others wait"
                             : "the event ended: Deku Scrub destroy flags from others go through");
            }
            // Issue 144, second guard: also while our copy of that scrub is near our Link
            // (CoopScrubTalk::Near, from the objects walk), whatever the event flag says.
            const std::uint8_t near = scrub_talk_on_ ? CoopScrubTalk::TheNear().Mask(CoopEvery::NowUs()) : 0;
            if (near != scrub_near_) {
                for (std::size_t g = 0; g < CoopScrubTalk::kGroups; ++g) {
                    const unsigned bit = 1u << g;
                    if ((near & bit) != (scrub_near_ & bit)) {
                        LOG_INFO(Core, "coop: dungeon: scrub near ({}): {}", CoopScrubTalk::GroupName(static_cast<CoopScrubTalk::Group>(g)),
                                 (near & bit) ? "within talking distance of our Link or in our event, its destroy flags from others wait"
                                              : "no longer near our Link nor in our event, its destroy flags from others go through");
                    }
                }
                scrub_near_ = near;
            }
            const CoopScrubTalk::Hold talk = scrub_talk_;
            // A scrub's one-time sale bought by another player waits while that scrub may be in an
            // event of ours (coop_scrub_sale.h): its prefab graph deletes the scrub from the flag.
            // event_running is read here directly: scrub_talk_ is not updated with
            // LICHII_COOP_SCRUB_TALK_HOLD=0 or =near.
            CoopScrubSale::Here sale_here;
            sale_here.in_event = talk.Holding() || OurLinkInEvent(reading);
            sale_here.busy = CoopScrubSale::TheBusy().Mask(CoopEvery::NowUs());
            if (reading && reading->scene) {
                sale_here.known = true;
                sale_here.x = reading->position[0];
                sale_here.y = reading->position[1];
                sale_here.z = reading->position[2];
            }
            const std::string sale_scene = reading && reading->scene ? *reading->scene : std::string();
            const bool sale_on = scrub_sale_on_;
            const bool shop_on = shop_sale_;
            const bool gift_on = shop_sale_on_;
            // Row 98(2): this frame's result hold (coop_event_result.h): a new scene is a new visit; the flags of an
            // event whose result we set here land; a result guard (ours, and the eye switch path's guards, which used
            // to wait for its replay) of a class-asker event not set here and not played waits.
            CoopEventResult::Hold result_hold;
            if (live_events_ && !hold_scene_.empty()) {
                const std::function<bool(const std::string&)> played = [&memory](const std::string& n) {
                    return coop::host::GameFlags(*memory)
                        .get_bool(coop::host::flag_hash("HavePlayedEvent." + n))
                        .value_or(false);
                };
                // Our game's value of a placement key (a silver rupee token: the counter's largest missing one waits).
                const std::function<bool(std::uint64_t)> have_key = [&memory](std::uint64_t k) {
                    return coop::host::GameFlags(*memory).get_key64(k).value_or(false);
                };
                event_result_.Visit(hold_scene_, played);
                result_hold = CoopEventResult::HoldFor(hold_scene_, event_result_.Applied(), played, have_key);
                const std::pair<std::size_t, std::size_t> waits{result_hold.waiting_keys.size(),
                                                                result_hold.waiting.size()};
                if (waits != trigger_wait_logged_) {
                    trigger_wait_logged_ = waits;
                    LOG_INFO(Core, "coop: dungeon: trigger: in {} {} shared keys and {} flags wait (an asker's trigger "
                                   "or result guard; each until our game played its event)",
                             hold_scene_, waits.first, waits.second);
                }
            }
            shop_view.emplace(*quests);
            hold.emplace(*shop_view, [link, scene, talk, near, sale_here, sale_scene, sale_on, shop_on, gift_on,
                                   result_hold](coop::protocol::FlagKey key) {
                if (key.kind == coop::protocol::FlagKeyKind::key64) {
                    // Row 98(2) trigger guards: a shared key that would raise our asker's trigger (a silver rupee
                    // counter's key, its last token, an ice block) waits until our game has played the event.
                    return link && scene && result_hold.WaitsKey(key.key);
                }
                if (key.kind != coop::protocol::FlagKeyKind::named || key.index != 0) {
                    return false;
                }
                if (talk.Holds(key.hash) || CoopScrubTalk::NearHolds(near, key.hash)) {
                    return true;
                }
                if (shop_on && CoopShopSale::Waits(key.hash, sale_here.in_event)) {
                    return true;  // a shelf another player sold out waits while our shop talk runs
                }
                if (gift_on && CoopShopSale::GiftWaits(key.hash, sale_here.in_event)) {
                    return true;  // a Great Fairy's gift another player got waits while our event runs
                }
                if (sale_on) {
                    CoopScrubSale::Here here = sale_here;
                    here.scene = sale_scene;
                    if (CoopScrubSale::Waits(key.hash, here)) {
                        return true;
                    }
                }
                if (!link || !scene) {
                    return false;
                }
                if (result_hold.Releases(key.hash)) {
                    return false;  // row 98(2): our result of its event is set here
                }
                if (result_hold.Waits(key.hash)) {
                    return true;  // row 98(2): it would delete our asker before our result is set
                }
                if (CoopQuests::Find(key.hash) != nullptr) {
                    return CoopDungeon::IsDungeonScene(*scene);  // a quest step waits outside dungeons
                }
                const auto in = CoopDungeon::HeldIn(key.hash);
                return in && (in->empty() ? CoopDungeon::IsDungeonScene(*scene) : *scene == *in);
            });
        }
        if (memory) {  // docs/flags.md, "Saves": no scene for a while is the title screen
            session_->set_scene_readable(coop::host::PlayerReader(*memory).read_scene().has_value());
        }
#ifdef COOP_HOOKSHOT
        // The title's menu up: a guest's load through it is a save load (player report #96, docs/flags.md "Saves").
        session_->set_title_up(CoopSave::TitleUp());
        // Patch 0024: every save the game loads, from its own load nodes (docs/flags.md, "A load window").
        session_->set_native_save_loads(CoopSave::NativeSaveLoads());
        if (const auto loads = CoopSave::TakeGameSaveLoads(); loads.starts != 0 || loads.loads != 0) {
            if (loads.starts != 0) {
                session_->note_save_load_start();
            }
            if (loads.loads != 0) {
                session_->note_save_loaded(loads.retry);
            }
            LOG_INFO(Core, "coop: save: the game {}",
                     loads.loads == 0 ? "starts loading a save (its own load node)"
                     : loads.retry    ? "loaded a save: Game Over's Try Again (its own load nodes): the group's "
                                        "progress stays"
                                      : "loaded a save (its own load nodes)");
        }
        // The game's own flag writes since the last frame (the applier's wrap, coop_flag_native.h): the watcher
        // reads those flags, not the whole list.
        CoopFlagNative::Feed(*session_);
        LogFlagEvents();
#endif
        LogFlagTraffic();
        if (auto color = TakeColor()) {  // the pause menu's look (hookshot/src/coop_look.h)
            session_->set_color(*color);
        }
        const auto report = session_->frame(reading, hold ? &*hold : nullptr);
        CoopMapRefresh::TheRefresher().SetInPlay(reading.has_value());  // rows 63/76: no refresh off play (session)
        if (memory) {  // minimap-b: the current dungeon's map or compass going 1 -> 0, logged with its context
            WatchMapDrops(*memory, reading.has_value(),
                          reading ? reading->scene : coop::host::PlayerReader(*memory).read_scene(),
                          reading ? std::optional(reading->position) : std::nullopt);
        }
        if (memory) {  // split screen: each extra local player is a player of the session (coop_local_net.h)
            CoopLocalNet::Online(
                *memory, reading, report.connected, report.server_now_us,
                [this](const coop::protocol::Message& m) {
                    return session_->with_client([&m](coop::net::Client& client) { return client.send(m); });
                },
                [](const std::string& line) { LOG_INFO(Core, "coop: {}", line); });
        }
        if (quests) {
            for (const auto& r : quests->take_raised()) {
                LOG_INFO(Core, "coop: quests: {} {} -> {} (shared Threads of Time; issue 193)", r.quest,
                         r.from, r.to);
            }
        }
        // Issue 106, LICHII_COOP_DEBUG_SAMPLES=1: samples sent a second (at most
        // about 60, with high refresh too) and presents a second.
        if (debug_samples_) {
            ++presents_;
            const std::uint64_t now_us = CoopEvery::NowUs();
            if (samples_log_.Due(now_us)) {
                const std::uint32_t seq = session_->sampler().next_sequence();
                if (samples_log_at_ != 0 && now_us > samples_log_at_) {
                    const double seconds = static_cast<double>(now_us - samples_log_at_) / 1e6;
                    LOG_INFO(Core, "coop: samples: {:.1f} sent/s, {:.1f} presents/s",
                             static_cast<double>(seq - samples_seq_) / seconds,
                             static_cast<double>(presents_) / seconds);
                }
                samples_seq_ = seq;
                samples_log_at_ = now_us;
                presents_ = 0;
            }
        }
        {   // deku-sync-fix: what our game asked before the title or this co-op session is not ours now
            const bool scene_loaded = view.scene.has_value();
            const bool title = start_source_.NoScene(scene_loaded);
            if (story_source_.NoScene(scene_loaded) || title) {
                LOG_INFO(Core, "coop: live events: the title: the game's earlier event requests are forgotten");
            }
            if (sources_reset_.exchange(false)) {
                start_source_.Reset();
                story_source_.Reset();
                LOG_INFO(Core, "coop: live events: a new co-op session: the game's earlier event requests are forgotten");
            }
        }
        if (report.save_load) {
            LOG_INFO(Core, "coop: flags: save load seen ({} flags back, {} frames without a scene; {})",
                     report.save_load_flags, report.save_load_frames,
                     report.save_load_native ? "the game's own load nodes" : "the old signs");
            CoopTime::SaveLoaded(report.server_now_us);  // issue 159
            lent_keys_.forget();  // dungeon-sync-b: the save has its own key counts
            start_source_.Reset();  // deku-sync-fix: what the game asked before the load is not ours now
            story_source_.Reset();
            coop::host::map_drop_log().note("save load seen (" + std::to_string(report.save_load_flags) + " flags back)");
        }
        // deku-dungeon-2 (DUNGEON_EVENT, issues 176 and 196): a gimmick event
        // our game starts goes out at once, and another player's start is
        // offered like a held flag, so the event runs in both games together
        // instead of here only after their cutscene ended.
        if (hold && live_events_) {
            std::vector<coop::protocol::DungeonEvent> got;
            {
                std::lock_guard lock(dungeon_events_mutex_);
                got.swap(dungeon_events_in_);
            }
            for (const auto& e : got) {
                if (e.scene == hold_scene_ &&
                    CoopDungeon::HeldIn(coop::host::flag_hash("HavePlayedEvent." + e.name))) {
                    LOG_INFO(Core, "coop: dungeon: player {} started the event {} in this scene", e.player_number,
                             e.name);
                    live_decider_.Started(e.name, e.scene);
                }
            }
            const bool running_now = reading && reading->event_running.value_or(false);
            if (memory && !hold_scene_.empty()) {
                // Only the held gimmick events (their result comes no other way);
                // the ones whose room flags already carry the result (the
                // basement water, the torch room) would only add a cutscene.
                std::vector<std::string_view> names;
                for (const std::string_view e : CoopLiveEvents::EventsIn(hold_scene_)) {
                    if (CoopDungeon::HeldIn(coop::host::flag_hash("HavePlayedEvent." + std::string(e)))) {
                        names.push_back(e);
                    }
                }
                // Our own start: on Hookshot the game's own request names it (the world hook's wrap); the search
                // of the event session is the emulator's way, and Hookshot's net.
                auto& requests = CoopLiveEvents::TheGameRequests();
                const bool by_request = requests.Live();
                start_source_.Scene(hold_scene_);
                const auto found = start_source_.Frame(
                    by_request ? CoopLiveEvents::RequestedOf(requests.Since(start_cursor_), names) : std::nullopt,
                    by_request, running_now,
                    [&] { return CoopLiveEvents::FindRunningEvent(&ReadGuest, &*memory, base_, names); });
                LogScanOnly(start_source_, start_scan_only_, "dungeon");
                if (found) {
                    live_decider_.Ours(*found, hold_scene_);
                    coop::protocol::DungeonEvent e;
                    e.scene = hold_scene_;
                    e.name = *found;
                    session_->with_client([&](coop::net::Client& c) {
                        const bool sent = c.send(coop::protocol::Message{e});
                        LOG_INFO(Core, "coop: dungeon: our game started the event {}: {}", *found,
                                 sent ? "sent to the others" : "not sent");
                        return 0;
                    });
                }
            }
        }
        if (!(hold && live_events_)) {
            std::lock_guard lock(dungeon_events_mutex_);
            dungeon_events_in_.clear();  // nothing takes them now: none applied late
        }
        if (shared_scenes_) {
            SharedScenes(view, reading, memory, report.server_now_us);
        }
        // Boss cutscenes (coop_boss_scenes.h): another player's boss intro or
        // death, asked of our game by name, as a story cutscene's.
        if (live_events_) {
            for (auto& name : CoopBoss::TakeWantedEvents()) {
                std::lock_guard lock(live_mutex_);
                live_requester_.Want(std::move(name), view.scene.value_or(std::string()), CoopEvery::NowUs(),
                                     CoopBossScenes::Director::kJoinUs);
            }
        }
        // review-p11-interactions: a gimmick or story event still waiting in
        // the requester is dropped when we left its scene, when it waited too
        // long (story cutscenes: kJoinUs) or when our game has it played.
        // cutscene-bleed (#251): a start decided in a dungeon room waits (not asked) while our Link is clearly in
        // another room (it waited behind the door we walked through) and is asked once our Link is back in it.
        // #197: none is asked while our teleport is under way (Pause); they are kept for after the trip.
        if (live_events_) {
            const bool underway = CoopTeleport::Underway();  // its own event_mutex, taken before live_mutex_
            std::lock_guard lock(live_mutex_);
            live_requester_.Pause(underway);
        }
        if (live_events_ && memory) {
            std::lock_guard lock(live_mutex_);
            live_requester_.Expire(view.scene.value_or(std::string()), CoopEvery::NowUs(),
                                   [&](const std::string& n) {
                                       return coop::host::GameFlags(*memory)
                                           .get_bool(coop::host::flag_hash("HavePlayedEvent." + n))
                                           .value_or(false);
                                   },
                                   [&](int room) { return OurLinkAwayFrom(view, room); });
        }
        if (live_events_ && eye_switch_on_) {  // deku-switches: a switch that found nothing goes the old way
            // deku-ladder: also dropped once our game has it played (our own asker ran it): the eye is never set again.
            std::function<bool(const std::string&)> played;
            if (memory) {
                played = [&](const std::string& n) {
                    return coop::host::GameFlags(*memory)
                        .get_bool(coop::host::flag_hash("HavePlayedEvent." + n))
                        .value_or(false);
                };
            }
            eye_switch_.Expire(view.scene.value_or(std::string()), played);
            for (auto& [event, scene] : eye_switch_.TakeFallbacks()) {
                std::lock_guard lock(live_mutex_);
                live_requester_.WantFromRoom(std::move(event), std::move(scene), OurDungeonRoom(view));
            }
        }
        // Issue 129: a held gimmick event is started here too when our Link is
        // near the place that asks for it, so its result happens live
        // (coop_live_events.h; off with LICHII_COOP_LIVE_EVENTS=0).
        if (hold && live_events_) {
            std::vector<std::string> held_events;
            for (const auto& k : hold->held_keys()) {
                if (const auto e = CoopDungeon::EventOf(k.hash); !e.empty()) {
                    held_events.emplace_back(e);
                }
            }
            const bool running = reading && reading->event_running.value_or(false);
            std::optional<std::array<float, 3>> link;
            if (view.position) {
                link = std::array<float, 3>{(*view.position)[0], (*view.position)[1], (*view.position)[2]};
            }
            // Never for a place in another room than ours: not loaded here.
            const CoopLiveEvents::RoomOf room_of = [&](const std::array<float, 3>& at) -> std::optional<int> {
                const auto guess = DungeonRooms().Nearest(hold_scene_, at);
                return guess ? std::optional<int>(guess->room) : std::nullopt;
            };
            if (const auto name = live_decider_.Next(held_events, hold_scene_, running, link, &room_of)) {
                // Row 98(2) (owner 07:40 UTC 2026-11-17): RESULT only, never a replay here (a replay binds our Link:
                // "movement stuck"). The old replay ways (the eye switch's plug, the requester's WantFromRoom) are
                // not asked for a held gimmick event any more; story scenes keep theirs (SharedScenes).
                if (event_result_.Want(*name, hold_scene_, link)) {
                    LOG_INFO(Core, "coop: dungeon: result: {}: another player played it: setting our asker's result "
                             "here (no replay)", *name);
                } else {
                    LOG_INFO(Core, "coop: dungeon: result: {}: another player played it: its result comes by the "
                             "shared flags (no asker output to set)", *name);
                }
            }
        }
        // The scrub flags of issue 144 and the scrubs' one-time sales have their own log lines.
        const bool gimmick_held =
            hold && std::any_of(hold->held_keys().begin(), hold->held_keys().end(), [](const coop::protocol::FlagKey& k) {
                return !CoopScrubTalk::IsTalkFlag(k.hash) && !CoopScrubSale::IsSaleFlag(k.hash);
            });
        if (hold) {
            std::uint32_t sale = 0;
            for (const auto& k : hold->held_keys()) {
                if (CoopScrubSale::IsSaleFlag(k.hash)) sale = k.hash;
            }
            if (sale != sale_held_logged_) {
                if (sale != 0) {
                    LOG_INFO(Core, "coop: scrubs: {} was bought by another player: waits while we are near it or in an event",
                             CoopScrubSale::Find(sale)->what);
                } else {
                    LOG_INFO(Core, "coop: scrubs: a one-time sale from another player goes through");
                }
                sale_held_logged_ = sale;
            }
        }
        if (hold && gimmick_held != holding_logged_) {
            holding_logged_ = gimmick_held;
            LOG_INFO(Core, "coop: dungeon: {}", holding_logged_
                         ? "room gimmick flags from another player held until we leave this dungeon or load"
                         : "no room gimmick flags held");
        }
        if (map_refresh_) {  // another player's map, compass, boss key or floors (also the join snapshot, a reset)
            for (const auto& r : report.received) {
                if (coop::host::is_hud_dungeon_item(r.key)) {
                    CoopMapRefresh::TheRefresher().Want(SteadyMs());
                    break;
                }
            }
        }
        // Chat notices (docs/setup.md): what another player's change gave us.
        if (notify_ && !report.received.empty()) {
            std::uint8_t self = 0;
            std::vector<std::pair<std::uint8_t, std::string>> names;
            session_->with_client([&](coop::net::Client& c) {
                self = c.player_number();
                for (const auto& p : c.players()) {
                    names.emplace_back(p.number, p.nickname);
                }
                return 0;
            });
            notifier_.SetNames(std::move(names));
            for (auto& line : notifier_.Take(report.received, self, SteadyMs())) {
                LOG_INFO(Core, "coop: notice: {}", line);
                chat_.Add(std::move(line), true);
            }
        }
        // Rows 106/107: the two states in which our game sends none of its own flag changes, made visible.
        if (report.hold_timeout) {
            const coop::host::FlagStats fs = session_->flag_stats();
            LOG_WARNING(Core, "coop: flags: the host's progress (join snapshot) did not finish arriving in {} s: "
                              "dropped, our changes are sent again ({} timeouts, {} bad snapshots this session)",
                        coop::host::FlagWatcher::kHoldMaxFrames / 60, fs.hold_timeouts, fs.bad_snapshots);
            if (!hold_timeout_told_) {
                hold_timeout_told_ = true;
                std::string line = "The host's progress did not finish arriving. If keys or dungeon items look "
                                   "wrong, use Resync from host in the Lobby tab.";
                LOG_INFO(Core, "coop: notice: {}", line);
                chat_.Add(std::move(line), true);
            }
        }
        if (report.window_for == 0) {
            long_window_logged_ = 0;
        } else if (report.window_for >= long_window_logged_ + kLongWindowFrames) {
            long_window_logged_ = report.window_for;
#ifdef COOP_HOOKSHOT
            const bool title = CoopSave::TitleUp();
#else
            const bool title = false;
#endif
            LOG_WARNING(Core, "coop: flags: a load window open for {} s (our Link read: {}, title menu up: {}): "
                              "our own flag changes are not sent while it lasts",
                        report.window_for / 60, reading.has_value() ? "yes" : "no", title ? "yes" : "no");
        }
        if (report.host_reset) {
            // docs/flags.md, "Saves": the host loaded a save. Session-wide state
            // kept outside the flags is dropped here (each owner adds its clear:
            // locked doors' kept breaks, the kills-late dead list, ...).
            LOG_INFO(Core, "coop: flags: the host loaded a save: the group follows it");
            CoopObjects::ForgetKeptBreaks();  // locked doors' kept breaks (docs/dungeon.md)
            CoopBoss::ForgetFights();         // bosses beaten after that save fight again (player report #52)
            boss_beaten_.HostReset(CoopQuests::On());  // row 138 (4): a boss alive in that save, its own Bools back
            lent_keys_.forget();              // the host's save has its own key counts
        }
        if (report.hosting && (report.host_reset || report.save_load) && !report.save_load_retry) {
            // Row 159: our own save load (not Game Over's Try Again): which bosses it holds alive goes to the
            // guests once our game reads the quests again (BOSS_ALIVE, after the FLAG_RESET). Also with no flag
            // changed: a boss whose quest is each game's own changes no shared flag.
            boss_alive_due_ = true;
            // Row 159 (dungeon-sweep-0118-2): our own save decides, from the moment it is in. The FLAG_RESET (and
            // ForgetFights' clear of the record) comes only when the load window ends, after the new scene is read:
            // in between Apply gave our game the group's beaten set, so the host's save took a boss the group beat
            // after it (Gohma's Bools written into the host, then its quest put the boss back: the warp and Gohma).
            // Not Game Over's Try Again: the group's values come back then, the record with them.
            if (CoopBossBeaten::OwnLoadClears(report.hosting, report.save_load, report.save_load_retry)) {
                CoopBossBeaten::TheRecord().Clear();
                LOG_INFO(Core, "coop: boss: our own save load (host): bosses the group beat this session are not "
                               "given to it (the save decides)");
            }
        }
        // Issue 167: the host's save for the guests (coop_save.h): a guest asks
        // once, the host sends its newest save a part a frame, and again to
        // everyone after its own save load.
        session_->with_client([&](coop::net::Client& c) {
            CoopSave::OnFrame(c, report.host_reset);
            return 0;
        });
        if (auto line = CoopSave::TakeNotice(); !line.empty()) {
            LOG_INFO(Core, "coop: notice: {}", line);
            chat_.Add(std::move(line), true);
        }
        if (auto line = CoopVoice::TakeNotice(); !line.empty()) {   // voice chat turned on: how it works
            LOG_INFO(Core, "coop: notice: {}", line);
            chat_.Add(std::move(line), true);
        }
        // Chests opened and pickups taken in another game: what is loaded of
        // them goes here too (docs/items.md, "Chests").
        // Issue 146: a map or compass the game took back, put back.
        // Each key logged the first time; the count says how often in all.
        // Row 140: every put-back (not only a key's first, which is all take_one_way_restores lists) asks the HUD
        // refresh on this frame, so the HUD screen's copy takes the merged value before the game writes it back.
        if (session_->take_one_way_hud_refresh() && map_refresh_) CoopMapRefresh::TheRefresher().WantNow(SteadyMs());
        for (const auto& k : session_->take_one_way_restores()) {
            if (k.kept) {  // dungeon-sync-b (#63/#76)
                LOG_INFO(Core,
                         "coop: flags: {:#010x} (dungeon floors) {:#x} -> {:#x} with its map just got here: the "
                         "game's own map get, kept (not ORed back)",
                         k.key.hash, k.before, k.now);
                continue;
            }
            LOG_INFO(Core,
                     "coop: flags: {:#010x} (dungeon map, compass or floors) went back here: {:#x} -> {:#x}, put "
                     "back {:#x} ({} put back this session)",
                     k.key.hash, k.before, k.now, k.put, session_->one_way_restores());
        }
        for (const auto& k : session_->take_one_way_left()) {  // row 140 (flag-oneway-c)
            const auto* item = coop::host::find_item(k.hash);
            LOG_INFO(Core,
                     "coop: flags: {} ({:#010x}): the game lowered it again right after our put-back ({} times): its "
                     "own value until the next load, not put back or sent (row 140)",
                     item != nullptr ? item->name : std::string_view("?"), k.hash,
                     coop::host::FlagWatcher::kMaxQuickUndos);
        }
        for (const auto& f : session_->take_keeps_failing()) {  // row 140: the key of a write that keeps failing
            const auto* item = f.key.kind == coop::protocol::FlagKeyKind::named ? coop::host::find_item(f.key.hash)
                                                                                : nullptr;
            LOG_WARNING(Core, "coop: flags: write of {} keeps failing: {} tries ({}) (row 140)",
                        FlagKeyText(f.key, item), f.count, f.site);
        }
        for (const auto& g : session_->take_counts_guarded()) {  // issue 89 (docs/items.md)
            LOG_INFO(Core,
                     "coop: flags: count {:#010x} +{} here was a pickup another player had taken: "
                     "put back, not sent",
                     g.count.hash, g.amount);
        }
        {  // issue 195: a chest another player had opened, opened again here
            const bool in_event = OurLinkInEvent(reading);  // G1: our Link's get scene, not another player's event here
            for (unsigned n = CoopObjects::TakeChestsOpenedTwice(); n > 0; --n) {
                if (session_->expect_doubled_gain()) {
                    LOG_INFO(Core, "coop: flags: a chest another player had opened was opened here too: "
                                   "this game's count gain in its get scene is taken back");
                    doubled_since_ms_ = SteadyMs();
                    doubled_event_seen_ = in_event;
                }
            }
            if (doubled_since_ms_ != 0) {
                // Only its own get scene: over when our Link's event ends, or
                // when none started (the opening was not ours after all).
                doubled_event_seen_ = doubled_event_seen_ || in_event;
                if (!in_event && (doubled_event_seen_ || SteadyMs() - doubled_since_ms_ > kDoubledNoEventMs)) {
                    session_->end_doubled_gain();
                    doubled_since_ms_ = 0;
                }
            }
        }
        if (const auto taken = session_->take_written_for_others(); !taken.empty()) {
            std::vector<std::uint64_t> keys;
            for (const auto& k : taken) {
                if (k.kind == coop::protocol::FlagKeyKind::key64) {
                    keys.push_back(k.key);
                    // Issue 89 (2026-11-17): a Gold Skulltula's placement key just
                    // went 1 here for another player. The world sync takes that
                    // Skulltula and the token it made away with it: the token is
                    // made and taken in one run, so it never went through our
                    // walk, and no break was ever sent for it (ROGX kept it and
                    // the count went 41 -> 42).
                    // Either of its two keys (skulltula-0118: key + 2 alone named none).
                    if (const std::uint64_t placement = CoopWorld::GoldPlacementOfKey(k.key); placement != 0) {
                        CoopWorld::QueueTakenPlacement(placement);
                    }
                } else if (const auto chest = CoopChests::KeyOfOpened(k.hash)) {
                    keys.push_back(*chest);  // a chest's opened flag: that chest (issue 87)
                }
            }
            CoopObjects::TakenForOthers(keys);
        }
        // Issue 148: a boss door left shut in this save by an older build (a "shut in" flag that was
        // shared then) is opened once per scene load while Link is in no boss room.
        if (game_store) {
            std::optional<CoopBoss::Vec> at;
            if (view.position) {
                at = CoopBoss::Vec{(*view.position)[0], (*view.position)[1], (*view.position)[2]};
            }
            const auto key = [](std::string_view name) {
                return coop::protocol::FlagKey{coop::host::flag_hash(std::string(name)), 0};
            };
            for (const std::string_view name : stale_boss_.Check(view.scene, at, [&](std::string_view n) {
                     const auto v = game_store->read(key(n));
                     return v && v->value != 0;
                 })) {
                const bool ok = game_store->write(
                    coop::protocol::FlagEntry{key(name), coop::protocol::FlagKind::boolean, 0});
                LOG_INFO(Core, "coop: boss: {} was left set in this save (an older co-op build shared it): {}",
                         name, ok ? "cleared" : "could not clear");
            }
            // G2 (#241): a boss the group beat while our game did not see it die gets its beaten set, one way, at the
            // next load of a scene other than the boss's, never mid-scene (coop_boss_beaten.h). Solo or co-op idle: the
            // record is empty.
            // In the boss's own dungeon scene: live, once our Link is out of the boss's area (rows 136/137), or in it
            // once no death of ours can run there (row 136: came in after the death, joined, a refused ask).
            // Row 159: the sets' quests are Enums, which the flag store neither reads nor writes: the game's Enum
            // getter and setter for those (CoopBossBeaten::GameIo).
            const CoopBossBeaten::GameIo boss_io{
                [&](std::uint32_t hash) {
                    const auto v = game_store->read(coop::protocol::FlagKey{hash, 0});
                    return v && v->kind == coop::protocol::FlagKind::boolean ? std::optional<std::uint32_t>(v->value)
                                                                             : std::nullopt;
                },
                [&](std::uint32_t hash) { return coop::host::GameFlags(*memory).get_enum(hash); },
                [&](std::uint32_t hash, bool value) {
                    return game_store->write(coop::protocol::FlagEntry{coop::protocol::FlagKey{hash, 0},
                                                                       coop::protocol::FlagKind::boolean, value ? 1u : 0u});
                },
                [&](std::uint32_t hash, std::uint32_t value) {
                    return coop::host::GameFlags::set_enum(*memory, hash, value);
                }};
            const auto read_flag = [&](std::uint32_t hash) { return CoopBossBeaten::Read(boss_io, hash); };
            // Row 159: the host's BOSS_ALIVE (guests), and ours to send after our own save load (host).
            if (const auto alive = CoopBossBeaten::TheHostAlive().Take()) {
                boss_beaten_.HostAlive(alive->known, alive->alive, CoopQuests::On());
            }
            if (boss_alive_due_ && view.scene && at) {
                const CoopBossBeaten::AliveBits bits = CoopBossBeaten::AliveInSave(read_flag);
                if (bits.known != 0) {
                    boss_alive_due_ = false;
                    const bool sent = session_->with_client([&](coop::net::Client& c) {
                        return c.send(coop::protocol::Message{coop::protocol::BossAlive{0, bits.known, bits.alive}}) ? 1
                                                                                                                  : 0;
                    }) == 1;
                    LOG_INFO(Core, "coop: boss: our save holds bosses {:#05x} alive (of {:#05x} read): told the "
                                   "guests{}",
                             bits.alive, bits.known, sent ? "" : " (not sent)");
                }
            }
            const CoopBossBeaten::Record& record = CoopBossBeaten::TheRecord();
            const std::vector<std::uint8_t> beaten = record.Rooms();
            const auto near_boss = [&](std::uint8_t room) {
                const auto bosses = CoopBossScenes::Bosses();
                if (!at || !view.scene || room >= bosses.size()) {
                    return true;  // not known: wait
                }
                return CoopBossScenes::InArea(bosses[room], *view.scene, std::nullopt, *view.scene, *at);
            };
            for (const CoopBossBeaten::Write& w : boss_beaten_.Check(
                     view.scene, at.has_value(), beaten, read_flag, near_boss, [&](std::uint8_t room) {
                         // an event runs (or is unread): it may be the death; else what the Director knows
                         return !reading || reading->event_running.value_or(true) || CoopBoss::DeathMayRun(room);
                     })) {
                const bool ok = CoopBossBeaten::WriteTo(boss_io, w);
                const CoopBossBeaten::BeatenSet* set = CoopBossBeaten::ForRoom(w.room);
                if (w.revert) {
                    LOG_INFO(Core, "coop: boss: {} is alive in the host's save: {} {} {}", set->boss, w.name,
                             w.is_enum ? std::string("-> the step before its death") : "= " + std::to_string(w.value),
                             ok ? "(written)" : "(could not write)");
                    continue;
                }
                LOG_INFO(Core, "coop: boss: {} was beaten by the group, not in this game: {} {} {}", set->boss, w.name,
                         w.is_enum ? std::string("-> ") + set->step_name : std::string("= ") + std::to_string(w.value),
                         ok ? "(written)" : "(could not write)");
            }
            for (const std::string& line : boss_beaten_.TakeNotes()) {
                LOG_INFO(Core, "coop: boss: {}", line);
            }
        }
        {  // our own key spends, for the locks' give-back (docs/dungeon.md, "Locked doors")
            std::vector<std::uint32_t> drops;
            for (const auto& k : session_->take_own_count_drops()) {
                drops.push_back(k.hash);
            }
            for (const auto& k : lent_keys_.take_spent()) {  // a lent key used on its lock
                drops.push_back(k.hash);
            }
            CoopObjects::OwnCountDrops(drops);
        }
        if (game_store) {  // a lock opened twice: our second key back (docs/dungeon.md)
            for (const std::uint32_t key : CoopObjects::TakeKeyRefunds()) {
                const auto now = game_store->read(coop::protocol::FlagKey{key, 0});
                if (now && now->kind == coop::protocol::FlagKind::int32) {
                    auto back = *now;
                    back.value = static_cast<std::uint32_t>(static_cast<std::int32_t>(now->value) + 1);
                    const bool ok = game_store->write(back);
                    LOG_INFO(Core, "coop: doors: small key {:#x} given back ({} -> {}){}", key,
                             static_cast<std::int32_t>(now->value), static_cast<std::int32_t>(back.value),
                             ok ? "" : ": the write failed");
                }
            }
        }
        if (game_store) {  // issue 88: what the session left in MaxLife (docs/items.md)
            const auto max_life = game_store->read(coop::protocol::FlagKey{0xfbe01da1u, 0});
            CoopObjects::SetMaxLife(max_life && max_life->kind == coop::protocol::FlagKind::int32
                                        ? std::optional<std::int32_t>(static_cast<std::int32_t>(max_life->value))
                                        : std::nullopt);
            // Boss cutscenes (coop_boss_scenes.h): the one shared heart container sees our maximum and may set it.
            const bool known = max_life && max_life->kind == coop::protocol::FlagKind::int32;
            CoopBoss::SetMaxLife(known ? std::optional<std::int32_t>(static_cast<std::int32_t>(max_life->value))
                                       : std::nullopt);
            // A second pickup of that container is put back by the flag watcher, never sent.
            if (session_) {
                session_->cap_max(coop::protocol::FlagKey{0xfbe01da1u, 0}, CoopBoss::MaxLifeCap());
            }
            // Rows 136/137: the host's older save lowered MaxLife; row 181: a capped second container pickup was put
            // back. Either way our Link's own max follows (coop_objects.h), so his Life component stops writing the
            // higher max back.
            CoopObjects::SetMaxLifeLowerTo(session_ ? session_->held_lower(coop::protocol::FlagKey{0xfbe01da1u, 0})
                                                    : std::nullopt);
            if (const auto to = CoopBoss::TakeMaxLifeWrite(); to && known) {
                auto set = *max_life;
                set.value = static_cast<std::uint32_t>(*to);
                const bool ok = game_store->write(set);
                LOG_INFO(Core, "coop: boss: our maximum life {} -> {} (the boss's one heart container){}",
                         static_cast<std::int32_t>(max_life->value), *to, ok ? "" : ": the write failed");
            }
        }
        // Issue 144: changes from the others to actors near our Link wait while
        // our Link is in an event (a talk, a cutscene).
        // G1: any event in our game lets the actors it binds here act for it; only our Link's own
        // holds near-by changes and claims actors (coop/host/event_owner.hpp).
        CoopObjects::SetLocalEvent(reading && reading->event_running.value_or(false), OurLinkInEvent(reading));
        {
            // Row 97: which sign says our Link is in an event (PlayerReading::event_running):
            // the event manager's word, or only our Link's own performer (a Deku Scrub's talk
            // or AttackedEvent leaves the word 0). Logged on each change.
            static int said = -1;  // -1 not yet, 0 none, 1 the word, 2 our Link's performer only
            const int now = !(reading && reading->event_running.value_or(false)) ? 0
                            : reading->event_by_link                                ? 2
                                                                                    : 1;
            if (now != said) {
                if (now == 2 || said == 2) {
                    LOG_INFO(Core, "coop: event: our Link {} (by its own event performer; the manager's word is 0)",
                             now == 2 ? "is in an event" : "left the event its performer was bound into");
                }
                said = now;
            }
        }
        // Our own event hides the others (puppet, Navi, marker); playing the
        // Ocarina does not, nor another player's event replayed here (G1,
        // coop/host/event_owner.hpp, local_event_hides).
        const bool event =
            reading && coop::host::local_event_hides(
                           reading->event_running.value_or(false), reading->event_whose,
                           coop::host::name_of(reading->animation));
        view.event = event;  // (the map icons, CurrentMapView)
        if (report.connected) {
            view.others = session_->with_presence([&](coop::presence::Presence& p) {
                auto shown = coop::overlay::collect(p, report.server_now_us);
                for (auto& s : shown) {
                    const auto* player = p.player(s.number);
                    s.head = HeadHeight(player ? player->age() : std::nullopt);
                }
                // The same rule as the puppet: no marker for a player in
                // another scene or while our own cutscene runs.
                for (const auto& s : shown) {
                    const auto* player = p.player(s.number);
                    const std::optional<std::string_view> theirs =
                        coop::host::view_of(s.scene);
                    const std::optional<std::string_view> ours =
                        coop::host::view_of(view.scene);
                    if (coop::host::visibility(ours, event, theirs, player ? player->flags() : 0) ==
                        coop::host::Visibility::hide) {
                        view.hidden.push_back(s.number);
                    }
                    // Their voice: the place alone (row 155: our talk hides the puppet, not the voice).
                    if (!coop::host::voice_heard(ours, theirs)) {
                        view.unheard.push_back(s.number);
                    }
                }
                return shown;
            });
        }
        // Local co-op (coop_local_game.h): the first Link this frame, then its Core's step, before the puppets are planned.
        CoopLocalGame::Frame({view.game && view.scene.has_value(), view.position,
                              view.age == coop::protocol::Age::adult});
        CoopPuppet::Publish(view.game ? base_ : 0, view.scene, view.position, event,
                            report.connected ? session_.get() : nullptr, report.server_now_us);
        // Proximity voice (docs/voice.md): our microphone out, theirs at their Links.
        CoopVoice::SetHostMuted(CoopLobby::OwnVoiceMuted());
        CoopVoice::Frame(view.game ? view.position : std::nullopt, view.others, view.unheard,
                         report.connected ? session_.get() : nullptr);
        // Our Ocarina's notes for the other games (issue 152, coop_ocarina_sound.h).
        CoopOcarinaSound::Frame(system,
                                reading && reading->animation
                                    ? std::optional<std::string>(reading->animation->name)
                                    : std::nullopt,
                                report.connected ? session_.get() : nullptr);
        // The character picked in the Character Select mod, when that mod is installed (coop_look_share.h): its
        // saved Int, read twice a second while a game runs. No such flag (no mod): Link.
        static std::uint32_t character_tick = 0;
        if (view.game && character_tick++ % 30 == 0) {
            static const std::uint32_t picked_hash = coop::host::flag_hash("CharacterMenu_SelectedCharacter");
            EmulatorMemory flag_memory(system.ApplicationMemory(), base_);
            const auto picked = coop::host::GameFlags(flag_memory).get_int(picked_hash);
            // ... and that mod's Customize options for it (kCustomFlags: an Int each, 0 to 15; 0 is the model's own).
            static const auto custom_hashes = [] {
                std::array<std::uint32_t, CoopLookShare::kCustomFlags.size()> out{};
                for (std::size_t i = 0; i < out.size(); ++i) {
                    out[i] = coop::host::flag_hash(CoopLookShare::kCustomFlags[i]);
                }
                return out;
            }();
            std::uint32_t custom = 0;
            for (std::size_t i = 0; i < custom_hashes.size(); ++i) {
                const auto v = coop::host::GameFlags(flag_memory).get_int(custom_hashes[i]);
                if (v && *v > 0 && *v <= 15) {
                    custom |= static_cast<std::uint32_t>(*v) << (4 * i);
                }
            }
            // A guest on the host's save sends its own pick and options, not the host's (CoopLookShare::OwnPick).
            using CoopLookShare::OwnPick;
            static OwnPick own_pick;
            const auto kept = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop-character.txt";
            static const bool seeded = [&] {
                std::ifstream in(kept);
                std::uint64_t value = 0;
                if (in >> value && OwnPick::CharacterIn(value) <= CoopLookShare::kCharacterMax) {
                    own_pick.Seed(value);
                }
                return true;
            }();
            (void)seeded;
            const std::uint16_t character =
                picked && *picked > 0 && *picked <= CoopLookShare::kCharacterMax ? static_cast<std::uint16_t>(*picked)
                                                                                 : std::uint16_t{0};
            const std::uint64_t sent = own_pick.Update(OwnPick::Of(character, custom),
                                                       CoopSave::UseCoopFolder() || CoopSave::HostSaveReady(),
                                                       CoopSave::SaveLoadStarts());
            CoopLookShare::SetCharacter(OwnPick::CharacterIn(sent));
            CoopLookShare::SetCustom(OwnPick::CustomIn(sent));
            if (const auto own = own_pick.TakeOwnChange()) {
                std::ofstream(kept, std::ios::trunc) << *own << '\n';   // the next start (a guest that joins at once)
            }
        }
        // Our look of several parts, chosen in the Multiplayer tab (coop_look_share.h).
        CoopLookShare::Frame(report.connected ? session_.get() : nullptr);
        // The sounds the game starts on our own Link, for the other games (coop_link_sound.h).
        CoopLinkSound::Frame(report.connected ? session_.get() : nullptr,
                             static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                            std::chrono::steady_clock::now().time_since_epoch())
                                                            .count()));
        CoopFairy::Publish(view.game ? base_ : 0, view.scene, view.position, event,
                           report.connected ? session_.get() : nullptr, report.server_now_us);
        // Issue 199: our horse sent while we ride; a copy of theirs under each
        // riding puppet (coop_ride.h).
        CoopRide::Frame(system, view.game ? base_ : 0, report.connected ? session_.get() : nullptr,
                        view.scene, event, view.position,
                        reading ? reading->rotation : std::nullopt,
                        reading ? reading->animation : std::nullopt, report.server_now_us);
        // jitter-b: stamped with the game frame the objects were read from (coop_frame_clock.h).
        CoopObjects::Share(system, report.connected ? session_.get() : nullptr, view.scene,
                           coop::host::frame_aligned_us(report.server_now_us, CoopFrameClock::PresentAgeUs()));
        CoopHud::Frame(report.connected ? session_.get() : nullptr);  // our hearts (PLAYER_LIFE)
        CoopSpawn::Frame(system, report.connected ? session_.get() : nullptr, view.scene, report.server_now_us);
        CoopDrops::Frame(system, report.connected ? session_.get() : nullptr, view.scene, report.server_now_us);
        if (!reading) CoopTime::Loading(report.server_now_us);  // issue 159: a reload, a death
        CoopTime::Frame(system, view.game ? base_ : 0, report.connected ? session_.get() : nullptr,
                        report.server_now_us,
                        reading && reading->event_running.value_or(false));  // issue 192: no nudge in a cutscene
        // Boss rooms together (docs/dungeons-boss.md). Issue 148: the room's
        // "shut in" flag says when we are past its door.
        std::optional<bool> boss_shut_in;
        if (memory && view.scene) {
            if (const CoopBoss::Room* room = CoopBoss::RoomOfScene(*view.scene)) {
                if (const std::string_view flag = CoopBoss::ShutInFlag(*room); !flag.empty()) {
                    if (flag != boss_shut_in_name_) {  // hashed once, not every frame
                        boss_shut_in_name_ = flag;
                        boss_shut_in_hash_ = coop::host::flag_hash(std::string(flag));
                    }
                    coop::host::GameFlagStore flags(*memory);
                    const auto v = flags.read(coop::protocol::FlagKey{boss_shut_in_hash_, 0});
                    if (v) boss_shut_in = v->value != 0;
                }
            }
        }
        CoopBoss::Frame(system, view.game ? base_ : 0, view.scene, view.position,
                        reading && reading->event_running.value_or(false), boss_shut_in,
                        report.connected ? session_.get() : nullptr, report.server_now_us);
        // /teleport <player> (docs/setup.md "Chat commands"), after the boss
        // rooms: it reads whether our boss fight runs.
        CoopTeleport::Frame(system, view.game ? base_ : 0, reading, report.connected ? session_.get() : nullptr);
        for (auto& line : CoopBoss::TakeLines()) {
            chat_.Add(std::move(line), true);
        }
        // LICHII_COOP_SCENE_PROBE=1 only (docs/scenes.md "Free-play scene change").
        CoopSceneProbe::Frame(system, view.game ? base_ : 0, reading);
        for (auto& line : CoopTeleport::TakeLines()) {
            chat_.Add(std::move(line), true);
        }
        // PvP (docs/play-online.md "PvP"), after the objects and spawn syncs:
        // their swings and shots of this frame, and our Link's life.
        CoopPvp::Frame(view.scene, reading, report.connected ? session_.get() : nullptr,
                       report.server_now_us);
        for (auto& line : CoopPvp::TakeLines()) {
            chat_.Add(std::move(line), true);
        }
        // Native puppets phase 3, lifting another player (docs/native-puppets.md): off unless
        // LICHII_COOP_LIFT_PLAYER=1 (then nothing runs here).
        CoopLiftPlayerGame::Frame(view.scene, reading, report.connected ? session_.get() : nullptr,
                                  report.server_now_us);
        // Native puppets phase 4, PvP on real bodies (docs/native-puppets.md section 5): off unless
        // LICHII_COOP_PVP_NATIVE=1 (then nothing runs here).
        CoopPvpNativeGame::Frame(view.scene, reading, report.connected ? session_.get() : nullptr,
                                 report.server_now_us);
        // Down but not out (docs/play-online.md), after the walk read our
        // Link's life and after PvP's hurts.
        CoopDbno::Frame(view.scene, reading, report.connected ? session_.get() : nullptr,
                        report.server_now_us);
        for (auto& line : CoopDbno::TakeLines()) {
            chat_.Add(std::move(line), true);
        }
        // The Lobby (docs/play-online.md "Lobby"): the tab's host actions go out.
        CoopLobby::Frame(report.connected ? session_.get() : nullptr);
        for (auto& line : CoopLobby::TakeLines()) {
            chat_.Add(std::move(line), true);
        }
        // Difficulty (docs/play-online.md "Difficulty"): the host's choice goes out.
        CoopDifficulty::Frame(report.connected ? session_.get() : nullptr);
        for (auto& line : CoopDifficulty::TakeLines()) {
            chat_.Add(std::move(line), true);
        }
        // (Navi) New Game+ (coop_ngplus.h): Ganon's death unlocks it; with a level on, the kept hearts.
        if (memory) {
            CoopNgPlus::Frame(*memory, reading.has_value(), !report.connected || report.hosting,  // host or alone
                              CoopSave::TitleUp());
        }
        for (auto& line : CoopNgPlus::TakeLines()) {
            chat_.Add(std::move(line), true);
        }
        // Mods (docs/mods.md "Mods in co-op"): our list, the host's shared settings, a warning when they differ.
        CoopModShare::Frame(report.connected ? session_.get() : nullptr);
        for (auto& line : CoopModShare::TakeLines()) {
            chat_.Add(std::move(line), true);
        }
        // No puppet pack (Longshot module only): said once, when another player is first shown.
        for (auto& line : CoopPackNotice::TakeLines(!view.others.empty())) {
            chat_.Add(std::move(line), true);
        }
        // The co-op name was this PC's user or PC name and is now a random one (coop_name_privacy.h): said once.
        for (auto& line : CoopNamePrivacy::TakeLines()) {
            LOG_INFO(Core, "coop: notice: {}", line);
            chat_.Add(std::move(line), true);
        }
        Place(view);

        std::lock_guard lock(view_mutex_);
        frame_ = std::move(view);
    }

    std::optional<Markers> CurrentMarkers() {
        std::lock_guard lock(view_mutex_);
        if (Clock::now() - frame_.at >= kFrameStale) {
            return std::nullopt;
        }
        return frame_.markers;
    }

    // CurrentMapView (coop_hook.h): from the last frame, as the markers.
    bool CurrentMapView(MapView& v) {
        std::lock_guard lock(view_mutex_);
        if (Clock::now() - frame_.at >= kFrameStale || !frame_.game || !frame_.scene || !frame_.position) {
            return false;
        }
        v.scene.assign(*frame_.scene);
        v.self = *frame_.position;
        v.adult = frame_.age == coop::protocol::Age::adult;
        v.event = frame_.event;
        v.others.resize(frame_.others.size());
        for (std::size_t i = 0; i < frame_.others.size(); ++i) {
            const auto& s = frame_.others[i];
            auto& o = v.others[i];
            o.number = s.number;
            o.name.assign(s.name);
            o.position = s.position;
            o.rotation = s.rotation;
            if (s.scene) {
                if (!o.scene) o.scene.emplace();
                o.scene->assign(*s.scene);
            } else {
                o.scene.reset();
            }
            o.fade = s.fade;
        }
        return true;
    }

    std::vector<OverlayLine> Lines() {
        RoomView room;
        FrameView frame;
        {
            std::lock_guard lock(view_mutex_);
            room = room_;
            frame = frame_;
        }
        return Compose(room, frame);
    }

    // SetColor (coop_hook.h). Any thread; the next Frame sends it.
    void SetColor(std::optional<std::uint32_t> color) {
        std::lock_guard lock(color_mutex_);
        color_change_ = color;
    }

    // The host's new join password, for this client's next connection (coop_hook.h). Any thread.
    void SetJoinPassword(const std::string& password) {
        session_->with_client([&](coop::net::Client& c) { c.set_join_password(password); });
    }

    // Chat and the player list (coop_hook.h). Any thread.
    bool SendChat(const std::string& text) {
        if (CoopTeleport::OnChat(text, *session_)) {
            return true;  // a command: done here, its answer shows as a notice
        }
        if (CoopPvp::OnChat(text, *session_)) {
            return true;  // /pvp: the same
        }
        if (CoopDbno::OnChat(text, *session_)) {
            return true;  // /dbno: the same
        }
        if (CoopLobby::OnChat(text, *session_)) {
            return true;  // /kick, /ban, /mute, /unmute, /allow, /deny (the Lobby): the same
        }
        if (CoopLobby::OwnChatMuted()) {
            chat_.Add("the host muted your chat: nobody sees it", true);
            return true;
        }
        const bool sent =
            session_->with_client([&](coop::net::Client& c) { return c.send_chat(text); });
        if (sent) {
            chat_.Add("you: " + text, false);
        }
        return sent;
    }

    std::vector<ChatLine> ChatLines() const {
        std::vector<ChatLine> out;
        const auto now = CoopChat::Clock::now();
        for (auto& l : chat_.Lines()) {
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - l.at);
            out.push_back({std::move(l.text), l.notice, static_cast<std::uint64_t>(age.count())});
        }
        return out;
    }

    std::vector<PlayerRow> PlayerList() {
        struct Net {
            coop::net::State state;
            std::optional<std::uint64_t> round_trip_us;
            std::uint64_t server_now_us;
            std::vector<coop::net::Player> players;
            bool name_retry;
        };
        // One lock at a time: the network lock, then the presence lock.
        const Net net = session_->with_client([](coop::net::Client& c) {
            const bool on = c.state() == coop::net::State::connected;
            return Net{c.state(), c.round_trip_us(), on ? c.server_now_us() : 0, c.players(),
                       c.retrying_name_taken()};
        });
        std::optional<std::string> own_scene;
        {
            std::lock_guard lock(view_mutex_);
            if (Clock::now() - frame_.at < kFrameStale) {
                own_scene = frame_.scene;
            }
        }
        std::vector<PlayerRow> rows;
        const bool on = net.state == coop::net::State::connected;
        session_->with_presence([&](coop::presence::Presence& p) {
            for (const auto& player : net.players) {
                PlayerRow row{player.number, player.nickname, player.self};
                if (player.self) {
                    row.state = on ? "connected" : "?";
                    if (net.round_trip_us) {
                        row.ms = static_cast<std::uint32_t>(*net.round_trip_us / 1000);
                    }
                    row.scene = own_scene;
                } else if (const auto* other = p.player(player.number)) {
                    row.scene = other->scene();
                    row.state = "no samples";
                    if (!other->samples().empty() && net.server_now_us) {
                        const auto t = other->samples().back().time.server_time_us;
                        const auto d = net.server_now_us > t ? net.server_now_us - t : 0;
                        row.ms = static_cast<std::uint32_t>(std::min<std::uint64_t>(d / 1000, 99999));
                        row.state = d < 1'000'000 ? "connected" : "quiet";
                    }
                } else {
                    row.state = "joining";
                }
                rows.push_back(std::move(row));
            }
        });
        std::stable_sort(rows.begin(), rows.end(),
                         [](const PlayerRow& a, const PlayerRow& b) { return a.self && !b.self; });
        if (!on) {
            // The client drops the room on a lost connection; still show ourselves.
            PlayerRow self{0, settings_.name, true};
            self.state = net.name_retry ? "name in use, asking again"
                         : welcomed_once_ && net.state != coop::net::State::refused
                             ? "reconnecting"
                             : std::string(coop::net::to_string(net.state));
            self.scene = own_scene;
            rows.assign(1, std::move(self));
        }
        return rows;
    }

private:
    std::optional<std::optional<std::uint32_t>> TakeColor() {
        std::lock_guard lock(color_mutex_);
        return std::exchange(color_change_, std::nullopt);
    }

    // Projects the players with the camera the renderer found (coop_camera.h).
    // Without one, view.markers stays empty and only the panel shows.
    void Place(FrameView& view) {
        CoopCamera::SetWanted(view.game);
        if (!view.game) {
            return;
        }
        const auto camera = CoopCamera::Latest();
        const bool fresh = camera && view.at - camera->at < kCameraStale;
        if (fresh != camera_seen_) {
            camera_seen_ = fresh;
            if (fresh) {
                LOG_INFO(Core, "coop: camera found ({}x{}, clip y {}), markers on",
                         camera->width, camera->height, camera->y_down ? "down" : "up");
            } else {
                LOG_INFO(Core, "coop: camera lost, markers off");
            }
        }
        const bool log = debug_camera_ && debug_log_.Due(CoopEvery::NowUs());
        if (!fresh) {
            view.camera_note = "markers: no camera";
            if (log) {
                LOG_INFO(Core, "coop: camera debug: no camera");
            }
            return;
        }

        // The game's matrix is row-major and maps world to clip; the overlay
        // takes column-major view and projection, so the view is identity.
        // Clip y is turned to point up the picture, as the overlay expects.
        coop::overlay::Camera c;
        c.view = coop::overlay::identity();
        const bool flip = camera->y_down != flip_y_;
        for (int row = 0; row < 4; ++row) {
            for (int col = 0; col < 4; ++col) {
                const float v = camera->clip_from_world[row * 4 + col];
                c.projection[col * 4 + row] = (row == 1 && flip) ? -v : v;
            }
        }
        c.viewport_width = camera->width;
        c.viewport_height = camera->height;

        auto players = view.others;
        std::erase_if(players, [&](const coop::overlay::Shown& s) {
            return std::find(view.hidden.begin(), view.hidden.end(), s.number) != view.hidden.end();
        });
        if (debug_camera_ && view.position) {
            players.push_back({0, "you", *view.position, view.scene, 1.0f, HeadHeight(view.age)});
        }
        const auto result = overlay_.build(c, view.scene.value_or(""), players);
        if (result.error != coop::overlay::Error::none) {
            view.camera_note = "markers: bad camera matrix";
            if (log) {
                LOG_INFO(Core, "coop: camera debug: matrix not finite");
            }
            return;
        }
        char buf[96];
        std::snprintf(buf, sizeof(buf), "markers: on (camera %.0fx%.0f%s)", camera->width,
                      camera->height, debug_camera_ ? ", debug" : "");
        view.camera_note = buf;

        Markers markers;
        markers.width = camera->width;
        markers.height = camera->height;
        for (const auto& item : result.items) {
            Marker m;
            m.x = item.marker.x;
            m.y = item.marker.y;
            m.size = item.marker.size;
            m.alpha = item.alpha;
            m.edge = item.edge;
            m.arrow_angle = item.arrow_angle;
            m.self = item.number == 0;
            m.tag = item.tag.text;
            m.tag_x = item.tag.x;
            m.tag_y = item.tag.y;
            m.tag_below = item.tag.below;
            m.tag_head = item.tag.head;
            markers.items.push_back(std::move(m));
        }
        view.markers = std::move(markers);

        if (log) {
            LogSelf(view, *camera, c, result);
            for (const auto& other : view.others) {
                std::string where = "not drawn";
                for (const auto& item : result.items) {
                    if (item.number == other.number) {
                        where = fmt::format("at {:.0f},{:.0f}{}", item.marker.x, item.marker.y,
                                            item.edge ? " (edge arrow)" : "");
                    }
                }
                for (const auto& skipped : result.skipped) {
                    if (skipped.number == other.number) {
                        where = std::string("skipped: ") + WhyText(skipped.why);
                    }
                }
                LOG_INFO(Core, "coop: camera debug: P{} at ({:.1f}, {:.1f}, {:.1f}) {}",
                         other.number, other.position[0], other.position[1], other.position[2],
                         where);
            }
        }
    }

    // LICHII_COOP_DEBUG_CAMERA: where the local player lands on screen. With
    // the right matrix, the marker labelled "you" sits on Link.
    static void LogSelf(const FrameView& view, const CoopCamera::Frame& camera,
                        const coop::overlay::Camera& c, const coop::overlay::Result& result) {
        if (!view.position) {
            LOG_INFO(Core, "coop: camera debug: own position not readable");
            return;
        }
        const auto& p = *view.position;
        float clip[4];
        for (int row = 0; row < 4; ++row) {
            clip[row] = c.projection[0 * 4 + row] * p[0] + c.projection[1 * 4 + row] * p[1] +
                        c.projection[2 * 4 + row] * p[2] + c.projection[3 * 4 + row];
        }
        std::string where = "not drawn";
        for (const auto& item : result.items) {
            if (item.number == 0) {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "at %.0f,%.0f of %.0fx%.0f%s", item.marker.x,
                              item.marker.y, camera.width, camera.height,
                              item.edge ? " (edge arrow)" : "");
                where = buf;
            }
        }
        for (const auto& s : result.skipped) {
            if (s.number == 0) {
                where = std::string("skipped: ") + WhyText(s.why);
            }
        }
        LOG_INFO(Core,
                 "coop: camera debug: you at ({:.1f}, {:.1f}, {:.1f}) clip ({:.3f}, {:.3f}, "
                 "{:.3f}, {:.3f}) {}",
                 p[0], p[1], p[2], clip[0], clip[1], clip[2], clip[3], where);
    }

    // G1: an event our own Link is in (coop/host/event_owner.hpp our_link_in_event).
    static bool OurLinkInEvent(const std::optional<coop::host::PlayerReading>& reading) {
        return reading && coop::host::our_link_in_event(reading->event_running.value_or(false), reading->event_whose);
    }

    std::optional<coop::host::PlayerReading> Read(Core::System& system, FrameView& view) {
        auto* process = system.ApplicationProcess();
        if (process == nullptr) {
            view.game_note = "no game";
            build_warned_ = nullptr;  // the next game start warns again
            scanned_ = nullptr;       // and is scanned again
            return std::nullopt;
        }
        if (system.GetApplicationProcessProgramID() != kOcarinaProgramId) {
            view.game_note = "not Ocarina of Time";
            return std::nullopt;
        }
        const auto build = Hex(system.GetApplicationProcessBuildID(), 8);
        // Issue 97: the signature scan, once per game start (coop_game_scan.h).
        if (scanned_ != process) {
            scanned_ = process;
            scan_runs_ = ScanBuild(system, process, build);
        }
        if (CoopGameBuild::Find(build) == nullptr && !scan_runs_) {
            view.game_note = CoopGameBuild::PanelRefusal(build);
            if (build_warned_ != process) {
                build_warned_ = process;  // once per game start
                LOG_WARNING(Core, "coop: {}", CoopGameBuild::LogRefusal(build));
            }
            return std::nullopt;
        }
        if (process != process_) {
            process_ = process;
            base_ = FindMain(system, process);
            LOG_INFO(Core, "coop: game build {}, main at {:#x}", build, base_);
            if (debug_camera_) {
                for (const auto& [address, name] : Modules(system, process)) {
                    LOG_INFO(Core, "coop: debug: module {} at {:#x}", name, address);
                }
            }
            // A new game process: its first status goes again.
            session_->reset();
            flag_debug_.Reset();
        }
        view.game = true;
        EmulatorMemory memory(system.ApplicationMemory(), base_);
        coop::host::PlayerReader reader(memory, &drawn_state_);
        auto reading = reader.read();
        // sword-3: the sword and shield as the game's own query has them
        // (coop_puppet_own.h), not as the passing request byte was seen.
        if (reading && reading->equipment && reading->equipment->drawn) {
            reading->equipment->drawn =
                CoopPuppetOwn::TheDrawn().Apply(*reading->equipment->drawn, CoopPuppetOwn::NowMs());
        }
        // Native puppets phase 3: lifted, our Link's copies play the carried pose (off: unchanged).
        CoopLiftPlayerGame::CarriedPose(reading);
        if (reading) {
            view.position = reading->position;
            view.scene = reading->scene;
            view.age = reading->age;
            ApplyEventOwner(*reading);
            // PLAYER_STATUS's in_menu/paused (coop/host/sampler.hpp status_bits; docs/puppet.md "Away"; dead is the
            // reader's own, PlayerReader::read_life): the game's menu screens (Longshot's patch 0018: one of them in
            // front, f_802f1380's screens).
#ifdef COOP_HOOKSHOT
            if (const auto menu = CoopMapPage::MenuState()) {
                reading->in_menu = menu->menu_open;
            }
#endif
        }
        return reading;
    }


    // The loader's own list (the NSOs it loaded, by file name), else the
    // modules found in memory by their path names. Fritchii v5 maps Ocarina's
    // code so that the memory search finds nothing, and the emulator's
    // FindMainModuleEntrypoint then gives the code region start (0x8000000),
    // not main; its own Ocarina code (svc_nx2_oot_present.cpp) does the same.
    static Loader::AppLoader::Modules Modules(Core::System& system, Kernel::KProcess* process) {
        Loader::AppLoader::Modules modules;
        if (system.GetAppLoader().ReadNSOModules(modules) != Loader::ResultStatus::Success ||
            modules.empty()) {
            modules = Core::FindModules(process);
        }
        return modules;
    }

    // Issue 97: scans main for the offsets (coop_game_scan.h) and logs what it
    // found. True when co-op may run on a build the table does not know:
    // every offset found once and equal to the 1.1.1 ones, since the readers
    // still use those (docs/game-builds.md). A known build runs on its table
    // whatever the scan says; the log then shows whether the scan agrees.
    static std::string Join(const std::vector<std::string>& parts) {
        std::string out;
        for (const auto& part : parts) {
            out += (out.empty() ? "" : ", ") + part;
        }
        return out;
    }

    static bool ScanBuild(Core::System& system, Kernel::KProcess* process,
                          const std::string& build) {
        const auto* known = CoopGameBuild::Find(build);
        // A known build runs on its table; the scan there only checks itself.
        // LICHII_COOP_SCAN=0 leaves that check out (unknown builds still scan).
        if (const char* env = std::getenv("LICHII_COOP_SCAN");
            known != nullptr && env != nullptr && std::string_view(env) == "0") {
            return false;
        }
        const u64 base = FindMain(system, process);
        auto& memory = system.ApplicationMemory();
        u32 mod0 = 0;
        if (base == 0 || !memory.ReadBlockUnsafe(base + 4, &mod0, sizeof(mod0))) {
            LOG_WARNING(Core, "coop: build {}: scan skipped, main not readable", build);
            return false;
        }
        const u64 rodata = CoopGameScan::RodataStart(mod0);
        // Range first: a bad MOD0 word must not size a 4 GB buffer.
        if (rodata == 0 || rodata > 0x10000000) {
            LOG_WARNING(Core, "coop: build {}: scan skipped, rodata at {:#x} is not sensible",
                        build, rodata);
            return false;
        }
        std::vector<u32> text(rodata / 4);
        if (!memory.ReadBlockUnsafe(base, text.data(), text.size() * 4)) {
            LOG_WARNING(Core, "coop: build {}: scan skipped, main's code ({:#x} bytes) not readable",
                        build, rodata);
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        const auto result = CoopGameScan::Scan(
            text.data(), text.size(), rodata, [&](u64 slot) -> std::optional<u64> {
                u64 value = 0;
                if (!memory.ReadBlockUnsafe(base + slot, &value, sizeof(value)) || value < base) {
                    return std::nullopt;
                }
                return value - base;
            });
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start).count();
        if (!result.Complete()) {
            LOG_WARNING(Core, "coop: build {}: scan did not find {} of {} offsets: {} ({} ms)", build,
                        result.missing.size(), std::size(CoopGameScan::kPatterns),
                        CoopGameScan::MissingText(result.missing), ms);
            return false;
        }
        const auto diff = CoopGameScan::Differences(result.main, CoopGameBuild::kPrimary.main);
        if (known != nullptr) {
            const auto own = CoopGameScan::Differences(result.main, known->main);
            if (own.empty()) {
                LOG_INFO(Core, "coop: build {}: offsets by scan match the table ({} patterns, {} ms)",
                         build, std::size(CoopGameScan::kPatterns), ms);
            } else {
                LOG_WARNING(Core, "coop: build {}: scan differs from the table, the table is used: {}",
                            build, Join(own));
            }
            return false;
        }
        if (diff.empty()) {
            // Partial: the loader patch (puppets, svc hooks) and the object,
            // drop and spawn readers still want build 1.1.1 by id, and HELLO
            // names 1.1.1 (docs/game-builds.md).
            LOG_INFO(Core,
                     "coop: build {}: offsets by scan ({} ms); partial co-op: position, scene and "
                     "flags only, no puppets, objects, drops or spawns on this build",
                     build, ms);
            return true;
        }
        // Found, but elsewhere than in 1.1.1: log them so a table entry can be
        // made (docs/game-builds.md, steps for one new build).
        LOG_WARNING(Core, "coop: build {}: offsets by scan differ from 1.1.1, co-op stays off until "
                          "the readers use them: {}",
                    build, Join(diff));
        return false;
    }

    static u64 FindMain(Core::System& system, Kernel::KProcess* process) {
        for (const auto& [address, name] : Modules(system, process)) {
            if (name == "main" || name.starts_with("main.")) {
                return address;
            }
        }
        return GetInteger(Core::FindMainModuleEntrypoint(process));
    }

    void Run() {
        auto last_state = coop::net::State::idle;
        while (!stop_) {
            session_->service();
            session_->with_client([](coop::net::Client& c) { CoopPose::Send(c, SteadyMs()); });
            RoomView room = session_->with_client([&](coop::net::Client& c) {
                return RoomView{c.state(), c.player_number(), c.round_trip_us(), c.players(),
                                note_, c.encrypted()};
            });
            if (room.state != last_state) {
                last_state = room.state;
                LOG_INFO(Core, "coop: {}", coop::net::to_string(last_state));
                if (last_state == coop::net::State::connected) {
                    LOG_INFO(Core, "coop: traffic {}", room.encrypted
                                                           ? "encrypted"
                                                           : "NOT encrypted (should never happen: the client joins only encrypted servers)");
                }
            }
            // Issue 108: each REFUSED name taken asked again, so a log shows
            // the retries (every name_taken_retry_ms, at least 100 ms apart)
            // and when the window ends.
            // Counted from the start of each window (the client's count is a total).
            const auto [name_retries, name_retrying] = session_->with_client([](coop::net::Client& c) {
                return std::pair{c.name_taken_retries(), c.retrying_name_taken()};
            });
            if (name_retries != name_retries_logged_) {
                name_retries_logged_ = name_retries;
                LOG_INFO(Core, "coop: name {} is in use on the server; asking again (retry {})",
                         settings_.name, name_retries - name_retries_base_);
            }
            if (!name_retrying) name_retries_base_ = name_retries;
            // Issue 143: the server PINGs every 250 ms. Seconds without a
            // packet while connected means its packets do not reach us.
            if (room.state == coop::net::State::connected) {
                const auto link = session_->with_client([](coop::net::Client& c) { return c.link(); });
                if (!quiet_warned_ && link.quiet_ms >= kQuietWarnMs) {
                    quiet_warned_ = true;
                    LOG_WARNING(Core,
                                "coop: no packets from the server for {} ms (round trip {} ms, "
                                "loss {}%, datagram {} bytes): a firewall or VPN may be "
                                "dropping them (docs/setup.md, Troubleshooting)",
                                link.quiet_ms, link.round_trip_ms, link.loss_percent, link.mtu);
                } else if (quiet_warned_ && link.quiet_ms < kQuietWarnMs / 3) {
                    quiet_warned_ = false;
                    LOG_INFO(Core, "coop: packets from the server again");
                }
            } else {
                quiet_warned_ = false;
            }
            {
                std::lock_guard view(view_mutex_);
                room_ = std::move(room);
            }
            session_->wait(2);
        }
        session_->with_client([&](coop::net::Client& c) {
            const bool was_connected = c.state() == coop::net::State::connected;
            c.stop();
            c.service(0);
            if (was_connected) {  // solo after a session: nothing of it stays (OnSessionEvent)
                coop::net::Event left;
                left.kind = coop::net::EventKind::disconnected;
                OnSessionEvent(left, c, true);
            }
        });
        if (!quiet_) {
            LOG_INFO(Core, "coop: stopped");
        }
    }

    // What the session handled, for the log and the panel. Network thread,
    // under the session's network lock: it must not call with_client.
    void Log(const coop::net::Event& event) {
        using coop::net::EventKind;
        switch (event.kind) {
        case EventKind::welcomed:
        case EventKind::reconnected:
            LOG_INFO(Core, "coop: welcomed as player {} on {}:{}", event.welcome.player_number,
                     settings_.server, settings_.port);
            note_.clear();
            break;
        case EventKind::refused:
            note_ = "refused: " +
                    std::string(coop::net::refuse_text(event));
            LOG_WARNING(Core, "coop: server {}", note_);
            break;
        case EventKind::disconnected:
            // Issue 143: what the link looked like, so a report tells a
            // firewall or VPN (nothing heard for seconds) from a slow line.
            LOG_WARNING(Core,
                        "coop: connection lost; reconnecting (nothing from the server for {} ms, "
                        "round trip {} ms, loss {}%, datagram {} bytes, largest message sent {} "
                        "bytes, ENet waiting {} bytes, lost fragment bytes taken off its count {})",
                        event.link.quiet_ms, event.link.round_trip_ms, event.link.loss_percent,
                        event.link.mtu, event.link.largest_sent, event.link.waiting_bytes,
                        event.link.mended_bytes);
            break;
        case EventKind::player_joined:
            LOG_INFO(Core, "coop: player {} '{}' joined", event.player, event.nickname);
            break;
        case EventKind::player_left:
            LOG_INFO(Core, "coop: player {} '{}' left", event.player, event.nickname);
            break;
        default:
            break;
        }
    }

    // Chat and room notices for the window's chat lines. Same thread and lock
    // as Log.
    void Chat(const coop::net::Event& event, const coop::net::Client& client) {
        using coop::net::EventKind;
        const auto name = [&](std::uint8_t number, const std::string& known) {
            if (!known.empty()) {
                return known;
            }
            for (const auto& p : client.players()) {
                if (p.number == number && !p.nickname.empty()) {
                    return p.nickname;
                }
            }
            return "P" + std::to_string(number);
        };
        switch (event.kind) {
        case EventKind::welcomed:
            welcomed_once_ = true;
            sources_reset_ = true;  // deku-sync-fix: read on the frame
            chat_.Add("joined as player " + std::to_string(event.welcome.player_number), true);
            break;
        case EventKind::reconnected:
            sources_reset_ = true;
            chat_.Add("reconnected as player " + std::to_string(event.welcome.player_number), true);
            break;
        case EventKind::disconnected:
            chat_.Add("connection lost, reconnecting", true);
            break;
        case EventKind::refused:
            chat_.Add("server " + note_, true);
            break;
        case EventKind::chat:
            if (!CoopLobby::ChatHidden(event.player)) {  // the Lobby: muted by us
                chat_.Add(name(event.player, {}) + ": " + event.text, false);
            }
            break;
        case EventKind::player_joined:
            chat_.Add(name(event.player, event.nickname) + " joined", true);
            break;
        case EventKind::player_left:
            chat_.Add(name(event.player, event.nickname) + " left", true);
            break;
        default:
            break;
        }
    }

    static std::vector<OverlayLine> Compose(const RoomView& room, const FrameView& frame) {
        std::vector<OverlayLine> lines;
        char buf[160];
        const bool running = frame.at != Clock::time_point{} && Clock::now() - frame.at < kFrameStale;

        if (room.state == coop::net::State::connected) {
            std::string head = "Co-op: player " + std::to_string(room.self);
            if (room.round_trip_us) {
                std::snprintf(buf, sizeof(buf), ", ping %llu ms",
                              static_cast<unsigned long long>(*room.round_trip_us / 1000));
                head += buf;
            }
            if (CoopPvp::On()) {
                head += ", PvP on";
            }
            // Every session is encrypted (the client refuses a plain server).
            head += room.encrypted ? ", encrypted" : ", NOT encrypted";
            lines.push_back({head});
        } else {
            std::string head = "Co-op: " + std::string(coop::net::to_string(room.state));
            if (!room.note.empty()) {
                head += " (" + room.note + ")";
            }
            lines.push_back({head});
        }
        if (!running) {
            lines.push_back({"game not running", true});
        } else if (!frame.game) {
            lines.push_back({frame.game_note, true});
        } else if (!frame.position) {
            lines.push_back({"own position not readable", true});
        } else if (frame.scene) {
            lines.push_back({"you: " + *frame.scene, true});
        }
        if (running && frame.game && !frame.camera_note.empty()) {
            lines.push_back({frame.camera_note, true});
        }

        std::map<std::uint8_t, const coop::overlay::Shown*> shown;
        if (running) {
            for (const auto& s : frame.others) {
                shown[s.number] = &s;
            }
        }
        for (const auto& p : room.players) {
            if (p.self) {
                continue;
            }
            std::string text = "P" + std::to_string(p.number) + " " + p.nickname;
            const auto it = shown.find(p.number);
            if (it == shown.end() || it->second->fade <= 0.0f) {
                lines.push_back({text + ": no position", true});
                continue;
            }
            const auto& s = *it->second;
            text += ": " + s.scene.value_or("?");
            const bool same = s.scene && frame.scene && *s.scene == *frame.scene;
            if (same && frame.position) {
                const auto& a = s.position;
                const auto& b = *frame.position;
                const double d = std::sqrt(double(a[0] - b[0]) * (a[0] - b[0]) +
                                           double(a[1] - b[1]) * (a[1] - b[1]) +
                                           double(a[2] - b[2]) * (a[2] - b[2]));
                std::snprintf(buf, sizeof(buf), ", %.0f units away", d);
                text += buf;
            }
            lines.push_back({text, !same});
        }
        return lines;
    }

    const Settings settings_;

    const bool debug_camera_;  // LICHII_COOP_DEBUG_CAMERA: mark yourself, log where
    const bool flip_y_;        // LICHII_COOP_CAMERA_FLIP_Y: if markers are upside down

    // Present thread only.
    coop::overlay::Overlay overlay_{MarkerSettings()};
    bool camera_seen_ = false;
    CoopEvery::Every debug_log_{kDebugLogUs};
    Kernel::KProcess* process_ = nullptr;
    u64 base_ = 0;
    Kernel::KProcess* scanned_ = nullptr;  // issue 97: the process last scanned
    bool scan_runs_ = false;               // its scan lets an unknown build run
    Kernel::KProcess* build_warned_ = nullptr;  // the process last warned about
    FlagDebug flag_debug_;  // LICHII_COOP_DEBUG_FLAGS and the other flag settings
    bool queue_sizes_logged_ = false;  // the game's flag queue sizes, logged once
    // Kept across frames: a drawn sword shows in the game's memory for a moment only.
    coop::host::PlayerReader::DrawnState drawn_state_{};
    bool sharing_ = false;  // a flag list is shared; set by Begin
    CoopBoss::StaleBossFlags stale_boss_;  // issue 148: shut boss doors from older builds
    bool boss_alive_due_ = false;          // row 159: BOSS_ALIVE to send after our own save load (host)
    CoopBossBeaten::Apply boss_beaten_;    // G2: a boss the group beat, given to a game that did not see it die
    std::string_view boss_shut_in_name_;  // issue 148: CoopBoss::ShutInFlag, and its hash
    std::uint32_t boss_shut_in_hash_ = 0;
    bool pickups_ = false;  // one-time pickups are shared (coop_pickups.h)
    std::uint64_t doubled_since_ms_ = 0;  // issue 195: a reopened chest's take-back is armed
    bool doubled_event_seen_ = false;     // its get scene (our Link's event) began
    static constexpr std::uint64_t kDoubledNoEventMs = 1000;
    std::size_t pickups_given_ = 0;  // CoopPickups keys handed to the session
    bool holding_logged_ = false;  // issue 129: HoldFlagStore held something last frame
    std::uint32_t sale_held_logged_ = 0;  // a scrub sale flag the hold kept last frame (log)
#ifdef COOP_HOOKSHOT
    // Every 10 s while the flag watcher goes by the game's events (coop_flag_native.h): what it read, and what its
    // audit found that no event had named (0 when the applier's wrap sees every write).
    void LogFlagEvents() {
        if (!session_ || !CoopFlagNative::EventsLive() || !flag_events_log_.Due(CoopEvery::NowUs())) {
            return;
        }
        const coop::host::FlagStats s = session_->flag_stats();
        if (s.event_reads == flag_event_reads_ && s.event_misses == flag_event_misses_ &&
            s.full_reads == flag_full_reads_) {
            return;
        }
        flag_event_reads_ = s.event_reads;
        flag_event_misses_ = s.event_misses;
        flag_full_reads_ = s.full_reads;
        LOG_INFO(Core, "coop: flags: by the game's events: {} flag reads for its writes, {} changes only the audit found, "
                       "{} whole-list reads (loads)", s.event_reads, s.event_misses, s.full_reads);
    }
    CoopEvery::Every flag_events_log_{10 * CoopEvery::kSecondUs};
    std::uint64_t flag_event_reads_ = 0, flag_event_misses_ = 0, flag_full_reads_ = 0;
#endif
    // forest-temple-b (player reports #106-#108): every 10 s while it changed, the shared flags that went out, came
    // in and were written, so a player's log says whether two games' flags reach each other at all.
    void LogFlagTraffic() {
        if (!session_ || !flag_traffic_log_.Due(CoopEvery::NowUs())) {
            return;
        }
        const coop::host::FlagStats s = session_->flag_stats();
        // holding_frames and window_frames too: a game stuck in either state still prints (rows 106/107).
        const std::uint64_t sum = s.changes_sent + s.answers_sent + s.entries_in + s.applied + s.write_failures +
                                  s.holding_frames + s.window_frames;
        if (sum == flag_traffic_sum_) {
            return;
        }
        flag_traffic_sum_ = sum;
        LOG_INFO(Core, "coop: flags: traffic: {} changes sent, {} answers sent, {} entries came, {} written here "
                       "({} count deltas, {} load reverts, {} one-way put back, {} from a host reset), {} kept over "
                       "our own, {} not shared, {} write failures ({} tries of held flags, not failures), "
                       "{} messages not for us, {} bad snapshots; "
                       "{} s holding a join snapshot ({} dropped unfinished), {} s in load windows, {} changes "
                       "waiting for the host's answer; {} one-way left to the game; host table asked for while loading "
                       "{} (sent whole at the save load {}); our own save kept before the group's values came {}",
                 s.changes_sent, s.answers_sent, s.entries_in, s.applied, s.deltas_added, s.load_reverts,
                 s.one_way_restores, s.reset_applied, s.kept_local, s.not_shared, s.write_failures, s.held_retries,
                 s.ignored_messages,
                 s.bad_snapshots, s.holding_frames / 60, s.hold_timeouts, s.window_frames / 60, s.pending_now,
                 s.one_way_left,  // row 140
                 s.table_waits, s.table_resets, s.own_save_kept);  // New Event spam (world-fails-0115)
        // events-next-a-11: which flag the write failures are (a log showed ~60 a second after an event, no key).
        if (s.top_write_failure_count > 0) {
            const coop::protocol::FlagKey& k = s.top_write_failure;
            if (k.kind == coop::protocol::FlagKeyKind::key64) {
                LOG_INFO(Core, "coop: flags: top write failure: key64 0x{:016x} table {} ({} of {}, in {})", k.key,
                         k.table, s.top_write_failure_count, s.write_failures, s.top_write_failure_site);
            } else {
                LOG_INFO(Core, "coop: flags: top write failure: named 0x{:08x}[{}] ({} of {}, in {})", k.hash,
                         k.index, s.top_write_failure_count, s.write_failures, s.top_write_failure_site);
            }
        }
    }
    CoopEvery::Every flag_traffic_log_{10 * CoopEvery::kSecondUs};
    std::uint64_t flag_traffic_sum_ = 0;
    bool hold_timeout_told_ = false;               // rows 106/107: the chat notice once a run
    std::uint64_t long_window_logged_ = 0;         // the open window's length (1/60 s) last logged
    static constexpr std::uint64_t kLongWindowFrames = 60 * 60;  // a load window open a minute is logged
    CoopScrubTalk::Hold scrub_talk_;  // issue 144: scrub destroy flags wait for our event
    std::uint8_t scrub_near_ = 0;     // issue 144: the scrub groups near our Link (for the log)
    coop::host::LentKeys lent_keys_;  // dungeon-sync-b (#75): keys lent at another player's lock
    bool scrub_talk_on_ = EnvString("LICHII_COOP_SCRUB_TALK_HOLD") != "0";  // =0: off (the old softlock)
    // =near: only the near guard (CoopScrubTalk::Near), no event hold: a test proves it alone.
    bool scrub_near_only_ = EnvString("LICHII_COOP_SCRUB_TALK_HOLD") == "near";
    bool scrub_sale_on_ = EnvString("LICHII_COOP_SCRUB_SALE") != "0";  // =0: one-time sales each game's own
    bool shop_sale_on_ = EnvString("LICHII_COOP_SHOP_SALE") != "0";  // =0: the randomizer's one-time shelves each game's own
    bool shop_sale_ = false;  // this session shares the shop sold counters (a seed with shops)
    bool live_events_ = false;     // LICHII_COOP_LIVE_EVENTS=1 (issue 129)
    // deku-switches (#176 #129): an eye-switch room event starts by our eye's FirstOn (coop_eye_switch.h).
    // Off unless LICHII_COOP_EYE_SWITCH=1 until the probe run (hs-eye-probe).
    bool eye_switch_on_ = false;
    CoopEyeSwitch::Switcher eye_switch_;
    // Row 98(2) (owner 07:40 UTC 2026-11-17): a held gimmick event another player played sets our asker's result
    // outputs (coop_event_result.h) instead of replaying it here; only with live events on (in a session).
    CoopEventResult::Setter event_result_;
    // The trigger guards' wait last logged (coop_event_result.h): placement keys, flags (hook thread).
    std::pair<std::size_t, std::size_t> trigger_wait_logged_{0, 0};
    // Chat notices, on unless LICHII_COOP_NOTIFY=0 (docs/setup.md).
    bool notify_ = EnvString("LICHII_COOP_NOTIFY") != "0";
    bool map_refresh_ = false;  // LICHII_COOP_MAP_REFRESH=1 (coop_map_refresh.h)
    struct Names {
        std::vector<std::pair<std::uint8_t, std::string>> list;
        std::string operator()(std::uint8_t n) const {
            for (const auto& [number, nickname] : list) {
                if (number == n && !nickname.empty()) return nickname;
            }
            return "P" + std::to_string(n);
        }
    };
    class Notices {
    public:
        Notices()
            : names_(std::make_shared<Names>()),
              notifier_([names = names_](std::uint8_t n) { return (*names)(n); },
                        [](coop::protocol::FlagKey key, const coop::protocol::FlagEntry&)
                            -> std::optional<std::string> {
                            if (key.kind == coop::protocol::FlagKeyKind::named &&
                                CoopChests::KeyOfOpened(key.hash)) {
                                return std::string("opened a chest");
                            }
                            return std::nullopt;
                        }) {}
        void SetNames(std::vector<std::pair<std::uint8_t, std::string>> list) { names_->list = std::move(list); }
        std::vector<std::string> Take(const std::vector<coop::host::FlagWatcher::Received>& r, std::uint8_t self,
                                      std::uint64_t now_ms) {
            return notifier_.Take(r, self, now_ms);
        }

    private:
        std::shared_ptr<Names> names_;
        coop::host::Notifier notifier_;
    };
    Notices notifier_;
    static constexpr std::chrono::milliseconds kHoldRelease{500};
    std::optional<Clock::time_point> no_link_since_;  // the first frame without our Link (the hold's debounce)
    std::string hold_scene_;       // the last scene read, for the hold
    CoopLiveEvents::Decider live_decider_;
    std::mutex live_mutex_;  // the requester runs on the calc thread (CoopSvc caller)
    CoopLiveEvents::Requester live_requester_;
    CoopLiveEvents::StartSource start_source_;  // our own gimmick event starts
    std::uint64_t start_cursor_ = 0, start_scan_only_ = 0;
    // An event start the search found that the game's request had not named, with the wrap there: logged, as the
    // sign that some event does not start through the wrapped request.
    static void LogScanOnly(const CoopLiveEvents::StartSource& source, std::uint64_t& said, const char* what,
                            std::uint64_t* refused_said = nullptr) {
        if (source.ScanOnly() != said) {
            said = source.ScanOnly();
            LOG_INFO(Core, "coop: {}: an event start found by the search that no request of our game named in this "
                               "scene: a leftover, not sent ({} so far; deku-sync-fix)",
                     what, said);
        }
        if (refused_said != nullptr && source.Refused() != *refused_said) {
            *refused_said = source.Refused();
            LOG_INFO(Core, "coop: {}: a name the search found while the game ran another event it had asked for: "
                           "not sent ({} so far; intro-ocarina)", what, *refused_said);
        }
    }
    // Shared story cutscenes (coop_shared_scenes.h).
    bool shared_scenes_ = false;  // LICHII_COOP_SHARED_CUTSCENES, needs live events
    CoopSharedScenes::Joiner shared_joiner_;
    CoopLiveEvents::StartSource story_source_;  // our own story cutscene starts
    std::uint64_t story_cursor_ = 0, story_scan_only_ = 0, story_refused_ = 0;
    std::vector<coop::protocol::DungeonEvent> story_events_in_;  // under dungeon_events_mutex_  // deku-dungeon-2: our own gimmick event starts
    std::mutex dungeon_events_mutex_;         // DUNGEON_EVENT from the network thread
    std::vector<coop::protocol::DungeonEvent> dungeon_events_in_;
    static bool ReadGuest(void* context, std::uint64_t address, void* out, std::size_t size) {
        return static_cast<EmulatorMemory*>(context)->read(
            address, std::span<std::uint8_t>(static_cast<std::uint8_t*>(out), size));
    }
    const bool debug_samples_ = EnvFlag("LICHII_COOP_DEBUG_SAMPLES");  // issue 106
    CoopEvery::Every samples_log_{5 * CoopEvery::kSecondUs};
    std::uint64_t samples_log_at_ = 0;
    std::uint32_t samples_seq_ = 0;
    std::uint64_t presents_ = 0;
    coop::host::LoadMend load_mend_;  // issue 138: when ItemFlagStore grants and mends

    // The dungeon rooms' table (coop_rooms.txt of the puppet pack), read once
    // at its first use; empty without a pack (then no room is known).
    static const CoopRooms::Table& DungeonRooms() {
        static const CoopRooms::Table rooms = CoopFairy::RoomTable();
        return rooms;
    }

    // cutscene-bleed (#251): our Link's dungeon room this frame (the same guess the decider and the joiner use), if
    // in a dungeon and known.
    static std::optional<int> OurDungeonRoom(const FrameView& view) {
        if (!view.scene || !view.position || !CoopDungeon::IsDungeonScene(*view.scene)) {
            return std::nullopt;
        }
        const std::array<float, 3> at{(*view.position)[0], (*view.position)[1], (*view.position)[2]};
        if (const auto guess = DungeonRooms().Nearest(*view.scene, at)) {
            return guess->room;
        }
        return std::nullopt;
    }

    // cutscene-bleed (#251, review-0116-c item 2): our Link is clearly in another dungeon room than `room`: the
    // nearest room's placement is at least CoopRooms::kMargin nearer than `room`'s nearest one (the Tracker's slack:
    // the raw nearest flickers in a doorway and through walls). Not known (no scene, position or table, or far from
    // every placement): not away, the start is asked as before.
    static bool OurLinkAwayFrom(const FrameView& view, int room) {
        if (!view.scene || !view.position || !CoopDungeon::IsDungeonScene(*view.scene)) {
            return false;
        }
        const std::array<float, 3> at{(*view.position)[0], (*view.position)[1], (*view.position)[2]};
        return CoopRooms::ClearlyIn(DungeonRooms(), *view.scene, at, room) == CoopRooms::Where::other_room;
    }

    // Shared story cutscenes (coop_shared_scenes.h): a story cutscene
    // our game starts goes to the others in our scene (DUNGEON_EVENT, as a
    // gimmick event's start); one another player started near us plays here
    // too, through the live-event requester. Once a frame.
    void SharedScenes(const FrameView& view, const std::optional<coop::host::PlayerReading>& reading,
                      std::optional<EmulatorMemory>& memory, std::uint64_t server_now_us) {
        namespace proto = coop::protocol;
        std::vector<proto::DungeonEvent> got;
        {
            std::lock_guard lock(dungeon_events_mutex_);
            got.swap(story_events_in_);
        }
        const std::uint64_t now_us = CoopEvery::NowUs();
        if (!got.empty()) {
            const auto remotes = CoopPuppet::Collect(session_.get(), server_now_us);
            for (const auto& e : got) {
                std::optional<CoopSharedScenes::Vec3> starter;
                for (const auto& r : remotes) {
                    if (r.number == e.player_number && r.scene == e.scene) {
                        starter = r.position;
                    }
                }
                std::optional<int> room;
                if (starter) {
                    if (const auto guess = DungeonRooms().Nearest(e.scene, *starter)) {
                        room = guess->room;
                    }
                }
                shared_joiner_.Started(e.player_number, e.name, e.scene, starter, now_us, room);
            }
        }
        const bool running = reading && reading->event_running.value_or(false);
        std::uint8_t status = 0;
        session_->with_client([&](coop::net::Client& c) {
            if (const auto* me = c.find_player(c.player_number())) {
                status = me->status.value_or(0);
            }
            return 0;
        });
        CoopSharedScenes::Here here;
        here.scene = view.scene.value_or(std::string());
        here.link = view.position;
        here.can_start = reading && !running &&
                         (status & (proto::status::paused | proto::status::in_menu | proto::status::loading)) == 0;
        here.now_us = now_us;
        // Row 176: who is in our scene now, so a held start whose starter is still here outlasts kHoldUs.
        // Read from presence directly (CoopPuppet::Collect would take the puppets' sounds).
        if (shared_joiner_.Waiting() != 0 && !here.scene.empty()) {
            session_->with_presence([&](coop::presence::Presence& p) {
                for (const std::uint8_t n : p.players()) {
                    const auto* player = p.player(n);
                    if (player != nullptr && player->scene() == here.scene) {
                        here.in_scene.push_back(n);
                    }
                }
                return 0;
            });
        }
        // In a dungeon only a starter in our own room is joined (the other
        // rooms are not loaded here: a black picture until the cutscene ends).
        if (shared_joiner_.Waiting() != 0 && here.link && CoopDungeon::IsDungeonScene(here.scene)) {
            here.dungeon = true;
            if (const auto guess = DungeonRooms().Nearest(here.scene, *here.link)) {
                here.room = guess->room;
            }
        }
        // Our own start: found in the event session on a few early frames.
        if (memory && !here.scene.empty()) {
            static const std::vector<std::string_view> names = [] {
                const auto all = CoopSharedScenes::Names();
                return std::vector<std::string_view>(all.begin(), all.end());
            }();
            auto& requests = CoopLiveEvents::TheGameRequests();
            const bool by_request = requests.Live();
            // Only the story path refuses the search's finds while another asked-for event runs (intro-ocarina);
            // the dungeon gimmicks above keep their net as it was (a gimmick may start inside a talk's flow).
            story_source_.Scene(here.scene);
            const auto asked =
                by_request ? requests.Since(story_cursor_) : std::vector<CoopLiveEvents::GameRequest>{};
            // Player report 110: a story cutscene asked by its event setting (the Deku Tree's meeting:
            // KokiriForest_Meet_DekuTree_MeetDekuTree_01, flow Meet_DekuTree) is a story start by that setting,
            // not another event; it is sent and joined by the setting (coop_shared_scenes.h).
            static const std::vector<std::string_view> request_names = CoopSharedScenes::RequestNames();
            const auto found = story_source_.Frame(
                by_request ? CoopLiveEvents::RequestedOf(asked, request_names) : std::nullopt,
                CoopLiveEvents::RequestedOtherOf(asked, request_names), by_request, running,
                [&] { return CoopLiveEvents::FindRunningEvent(&ReadGuest, &*memory, base_, names); });
            LogScanOnly(story_source_, story_scan_only_, "cutscene", &story_refused_);
            const auto story_flag = [&](std::uint32_t h) { return coop::host::GameFlags(*memory).get_bool(h); };
            if (found && !CoopSharedScenes::Askable(*found)) {
                // A story quest's flow (Get_StoneOfWater) the search found while one of its settings runs that
                // this game's request did not name (no game can be asked for it), or a story cutscene that gives
                // or takes what is the player's own (a trade, rupees): not sent (report 110 review).
                LOG_INFO(Core, "coop: cutscene: our game started {}: not sent ({})", *found,
                         CoopSharedScenes::NotShared(*found));
            } else if (found && !CoopSharedScenes::GateHolds(*found, story_flag)) {
                // Our game cannot be in that scene now (intro-ocarina): a name left in the session.
                LOG_INFO(Core, "coop: cutscene: the search found the story cutscene {}, which our story is not at "
                               "yet: not sent", *found);
            } else if (found && shared_joiner_.Ours(*found, here.scene)) {
                proto::DungeonEvent e;
                e.scene = here.scene;
                e.name = *found;
                session_->with_client([&](coop::net::Client& c) {
                    const bool sent = c.send(proto::Message{e});
                    LOG_INFO(Core, "coop: cutscene: our game started the story cutscene {}: {}", *found,
                             sent ? "sent to the others in this scene" : "not sent");
                    return 0;
                });
            }
        }
        const auto name = shared_joiner_.Next(
            here,
            [&](const std::string& n) {
                // A story name or setting: its own played flag (every event setting has one).
                return memory && coop::host::GameFlags(*memory)
                                     .get_bool(coop::host::flag_hash("HavePlayedEvent." + n))
                                     .value_or(false);
            },
            [&](std::uint32_t h) {
                return memory ? coop::host::GameFlags(*memory).get_bool(h) : std::optional<bool>{};
            });
        for (const auto& line : shared_joiner_.TakeNotes()) {
            LOG_INFO(Core, "coop: cutscene: {}", line);
        }
        if (name) {
            std::lock_guard lock(live_mutex_);
            // Asked as the starter's game asked it: by the story name or setting it sent (report 110).
            // #251: with the room it was joined in (not asked while our Link is in another room).
            live_requester_.Want(*name, here.scene, now_us, CoopSharedScenes::kJoinUs,
                                 here.dungeon ? here.room : std::nullopt);
        }
    }

    // Issue 173 (coop_quest.h): raises our own quests (Goto_HyruleCastle,
    // DoorOfTime, Get_StoneOfFire) to what the shared story scenes say, so a
    // scene another player played does not leave our quest behind. Only ever
    // raised. Issue 171: the game's logic reads its ChallengeMgr, not the
    // GameData Enum co-op writes, so Get_StoneOfFire's GameData step (the
    // Kakariko gate's) and DoorOfTime's stone steps (issue 60: Zelda's escape
    // in the field; CoopQuest::LiveWant) go to the manager too, through the
    // game's setStep (challenges_, a caller on the calc thread); never while
    // our Link is in an event.
    CoopEvery::Every quest_catch_up_{CoopEvery::kSecondUs};
    CoopQuest::Challenges challenges_;
    void CatchUpQuests(EmulatorMemory& memory, bool in_event) {
        coop::host::GameFlags flags(memory);
        const CoopQuest::ReadBool flag = [&](std::uint32_t h) { return flags.get_bool(h); };
        const CoopQuest::ReadEnum value = [&](std::uint32_t h) { return flags.get_enum(h); };
        for (const auto& quest : CoopQuest::Quests()) {
            const auto was = flags.get_enum(quest.hash);
            const auto raise = CoopQuest::CatchUp(quest, flag, value);
            if (raise && was && coop::host::GameFlags::set_enum(memory, quest.hash, *raise)) {
                LOG_INFO(Core, "coop: story: quest {} {:#x} -> {:#x} (shared story; issue 173)",
                         quest.name, *was, *raise);
            }
        }
        for (const auto& line : challenges_.TakeNotes()) {
            LOG_INFO(Core, "coop: story: {}", line);
        }
        if (in_event) {
            return;
        }
        std::vector<CoopQuest::StepWant> wants;
        for (const auto& quest : CoopQuest::Quests()) {
            if (auto w = CoopQuest::LiveWant(quest, value)) wants.push_back(std::move(*w));
        }
        if (CoopQuests::LiveOn()) {  // issue 94: every shared quest, not only after a reload
            for (auto& w : CoopQuests::LiveWants(value)) wants.push_back(std::move(w));
        }
        if (auto w = FollowTradeItem(memory)) wants.push_back(std::move(*w));
        challenges_.Want(std::move(wants));
    }

    // Row 117 (docs/items.md, "Every item the game takes away"): the adult
    // trade's items are shared by mirror, but each trade NPC tests our own
    // SubEvent_GetSwordBigGoron step. The step follows the item held: our
    // GameData step (what the NPCs' _IsAfter_ Exps read) is raised to the
    // step at which the game's own flow hands that item over, and the
    // manager gets it through the game's setStep (Challenges, as the shared
    // quests), so its own step start runs (a spoiling item's timer too). Only
    // forward, never from NotReady, never in an event (our own trade's flow
    // sets its step itself). Returns the manager's want for this quest.
    std::optional<CoopQuest::StepWant> FollowTradeItem(EmulatorMemory& memory) {
        namespace trade = coop::host::trade;
        coop::host::GameFlags flags(memory);
        const auto was = flags.get_enum(trade::kQuest);
        const auto raise = coop::host::trade_step_raise(was, [&](std::uint32_t h) { return flags.get_int(h); });
        if (raise && coop::host::GameFlags::set_enum(memory, trade::kQuest, *raise)) {
            const char* held = "?";
            for (const auto& h : trade::kHolds) {
                if (h.step == *raise) held = h.name;
            }
            LOG_INFO(Core,
                     "coop: items: trade quest SubEvent_GetSwordBigGoron {:#x} -> {:#x}: {} is held here "
                     "(row 117)",
                     *was, *raise, held);
        }
        const auto now = flags.get_enum(trade::kQuest);
        const auto rank = now ? coop::host::trade_step_rank(*now) : std::nullopt;
        if (!rank || *rank < 2) {
            return std::nullopt;  // unread, NotReady or Ready: the manager is the game's own
        }
        CoopQuest::StepWant w{trade::kChallenge, *now, {}, "SubEvent_GetSwordBigGoron"};
        w.steps.assign(trade::kSteps.begin(), trade::kSteps.end());
        return w;
    }

    // Issue 125 (docs/items.md, "Strength"): raises PlayerStatus.LiftGrade to
    // what the held bracelet or gauntlets give. Only ever raised; an unknown
    // value or a flag that cannot be read is left alone.
    void MendLiftGrade(EmulatorMemory& memory) {
        namespace lift = coop::host::lift;
        coop::host::GameFlags flags(memory);
        const auto grade = flags.get_enum(lift::kGrade);
        if (!grade) {
            return;
        }
        const auto stock = [&](std::uint32_t hash) { return flags.get_int(hash).value_or(0); };
        const auto raise = coop::host::lift_grade_raise(stock(lift::kGoron), stock(lift::kSilver),
                                                        stock(lift::kGold), *grade);
        if (raise && coop::host::GameFlags::set_enum(memory, lift::kGrade, *raise)) {
            LOG_INFO(Core, "coop: items: lift grade {:#x} -> {:#x} for the held strength item",
                     *grade, *raise);
        }
    }

    // Scales (docs/mods.md, randomizer items): raises PlayerStatus.DiveDepth (a Float, through the game's Float setter
    // 0xba6f80) to what the held Silver or Golden Scale gives. Only ever raised; a flag that cannot be read is left alone.
    void MendDiveDepth(EmulatorMemory& memory) {
        namespace dive = coop::host::dive;
        coop::host::GameFlags flags(memory);
        const auto depth = flags.get_float(dive::kDepth);
        if (!depth) {
            return;
        }
        const auto raise = coop::host::dive_depth_raise(flags.get_int(dive::kSilver).value_or(0),
                                                        flags.get_int(dive::kGold).value_or(0), *depth);
        if (raise && coop::host::GameFlags::set_float(memory, dive::kDepth, *raise)) {
            LOG_INFO(Core, "coop: items: dive depth {} -> {} for the held scale", *depth, *raise);
        }
    }

    // Issue 190 (docs/items.md, "The ocarina"): equips the best held ocarina
    // when PlayerStatus.EquipOcarina is "" (an ocarina from another player)
    // or the Fairy Ocarina while the Ocarina of Time is held. The String64 goes
    // through the game's own String64 push (GameFlags::set_string64), so the
    // saved copy changes too: written straight into the live text, it went
    // back to "" at every scene load, before Link took it out (issue 57,
    // a player's log: the mend wrote it again after each load). A flag that
    // cannot be read is left alone.
    void MendOcarina(EmulatorMemory& memory, bool in_event) {
        namespace oc = coop::host::ocarina;
        coop::host::GameFlags flags(memory);
        for (const auto& line : ocarina_equip_.take_notes()) {
            LOG_INFO(Core, "coop: items: {}", line);
        }
        bool raised = false;
        // The Bools a real get sets beside the count: without them D-pad Left
        // took nothing out of a shared ocarina (ocarina-given, issue 190).
        const auto fairy = flags.get_int(oc::kFairy).value_or(0);
        const auto time = flags.get_int(oc::kTime).value_or(0);
        for (const std::uint32_t hash :
             coop::host::ocarina_bools_mend(fairy, time, [&](std::uint32_t h) { return flags.get_bool(h); })) {
            if (coop::host::GameFlags::set_bool(memory, hash, true)) {
                LOG_INFO(Core, "coop: items: ocarina bool {:#010x} 0 -> 1 for the held ocarina", hash);
                raised = true;
            }
        }
        const auto current = flags.get_string64(oc::kEquip);
        if (!current) {
            return;
        }
        // Rows 93/103: the flags alone never let it out; the game is asked to equip it from them once the live
        // name reads what was written (docs/items.md, "The game's own equip").
        if (raised && !current->empty()) {
            ocarina_equip_.want(*current);
        }
        // Row 103: and once per stay of our Link while one is held, also when its flags needed no write here (a save
        // an earlier mend wrote, the join snapshot, the host's file).
        ocarina_equip_.link(true, coop::host::ocarina_held(fairy, time, *current));
        std::uint8_t status = 0;
        if (session_) {
            session_->with_client([&](coop::net::Client& c) {
                if (const auto* me = c.find_player(c.player_number())) status = me->status.value_or(0);
                return 0;
            });
        }
        const bool busy =
            in_event || (status & (coop::protocol::status::paused | coop::protocol::status::in_menu |
                                coop::protocol::status::loading)) != 0;
        ocarina_equip_.arm(*current, CoopObjects::OurLinkActor().has_value(), busy);
        const auto want = coop::host::ocarina_equip_mend(flags.get_int(oc::kFairy).value_or(0),
                                                         flags.get_int(oc::kTime).value_or(0), *current);
        if (!want) {
            return;
        }
        if (coop::host::GameFlags::set_string64(memory, oc::kEquip, *want)) {
            ocarina_equip_.want(*want);
            LOG_INFO(Core, "coop: items: ocarina '{}' -> '{}' for the held ocarina", *current, *want);
        } else if (!ocarina_mend_failed_logged_) {
            ocarina_mend_failed_logged_ = true;
            LOG_WARNING(Core, "coop: items: could not equip the held ocarina '{}'", *want);
        }
    }
    bool ocarina_mend_failed_logged_ = false;
    coop::host::OcarinaEquip ocarina_equip_;  // rows 93/103: the game's own equip of a shared ocarina

    // Issue 58 (docs/items.md, "The letters"). Zelda's Letter: once the
    // Kakariko guard's Bool is set (by any player; shared story), the guard
    // never asks for the letter again, so ours is taken out as the guard
    // takes the shower's. Ruto's Letter: a bottle's contents, per player, so
    // another player got the bottle empty: until someone shows it, while our
    // King Zora (our own ChallengeMgr) has not moved, the letter goes into our
    // first empty bottle, as the game's own get puts it there. Both through
    // the game's setters; anything unread is left alone.
    // Row 117: once any player has shown Ruto's Letter (King Zora's scene
    // Dm_Zora_0010, shared story), it is never given again, and one still in
    // a bottle here is emptied, as that scene empties the shower's, once our
    // own King Zora has moved too (the quest catch-up; before that it is the
    // only way past him). Only while our Link is known to be in no event, so
    // our own King Zora scene takes ours itself.
    void MendLetters(EmulatorMemory& memory, bool no_event) {
        namespace letters = coop::host::letters;
        coop::host::GameFlags flags(memory);
        const auto zelda = flags.get_int(letters::kZelda);
        if (zelda && coop::host::zelda_letter_spent(*zelda, flags.get_bool(letters::kGateOpen)) &&
            coop::host::GameFlags::set_int(memory, letters::kZelda, 0)) {
            LOG_INFO(Core, "coop: items: Zelda's Letter {} -> 0: the Kakariko gate is open (issue 58)", *zelda);
        }
        const auto live = CoopQuest::LiveStep(
            [&memory](std::uint64_t a, void* out, std::size_t n) { return ReadGuest(&memory, a, out, n); }, base_,
            letters::kStoneOfWater);
        const auto plan = coop::host::ruto_letter_plan(
            flags.get_bool(letters::kRutoGot), flags.get_bool(letters::kKingRead),
            live ? coop::host::king_zora_moved(*live) : std::nullopt,
            [&](std::uint32_t h) { return flags.get_bool(h); }, [&](std::uint32_t h) { return flags.get_enum(h); });
        // Each change of why it waits is logged once (the 0.11.2 big run's
        // letter never came and said nothing about which test held it).
        if (plan.why != ruto_why_ && plan.why != coop::host::RutoWait::give) {
            ruto_why_ = plan.why;
            const std::string step =
                live ? fmt::format("{:#010x}", *live)
                     : CoopQuest::LiveDescribe([&memory](std::uint64_t a, void* out,
                                                         std::size_t n) { return ReadGuest(&memory, a, out, n); },
                                               base_, letters::kStoneOfWater);
            LOG_INFO(Core, "coop: items: Ruto's Letter waits: {} (Get_StoneOfWater: {}) (issue 58)",
                     coop::host::ruto_wait_text(plan.why), step);
        }
        if (plan.spent && no_event && coop::host::GameFlags::set_enum(memory, *plan.spent, letters::kEmpty)) {
            LOG_INFO(Core,
                     "coop: items: Ruto's Letter out of bottle {:#010x}: it was shown to King Zora "
                     "(HavePlayedEvent.Dm_Zora_0010) (row 117)",
                     *plan.spent);
        }
        if (plan.bottle && coop::host::GameFlags::set_enum(memory, *plan.bottle, letters::kLetter)) {
            ruto_why_ = plan.why;
            LOG_INFO(Core, "coop: items: Ruto's Letter into bottle {:#010x}: King Zora has not moved here (issue 58)",
                     *plan.bottle);
        }
    }
    std::optional<coop::host::RutoWait> ruto_why_;  // issue 58, the last reason logged

    // Issue 130 (docs/flags.md, "Story and cutscenes"): raises our own
    // Meet_DekuTree quest to what the shared story says (Mido let someone by:
    // Step03; the Deku Tree scene played: Complete), so the Kokiri Shop sells
    // here too. Only ever raised; anything unread is left alone.
    void CatchUpQuest(EmulatorMemory& memory, bool idle) {
        namespace quest = CoopStory::quest;
        coop::host::GameFlags flags(memory);
        const auto step = flags.get_enum(quest::kMeetDekuTree);
        const auto raise = CoopStory::QuestCatchUp(step, flags.get_bool(quest::kMidoBlocker),
                                                   flags.get_bool(quest::kMetDekuTree));
        if (raise && coop::host::GameFlags::set_enum(memory, quest::kMeetDekuTree, *raise)) {
            LOG_INFO(Core, "coop: story: quest Meet_DekuTree {:#x} -> {:#x} (shared story; issue 130)",
                     *step, *raise);
        }
        MendStory(memory, mend_since_us_, idle);  // issue 84, row 129
    }
    MendClocks mend_since_us_{};  // CatchUpQuest's (game thread)

    // Set by Begin before the network thread starts; the session borrows the
    // client and locks it, so neither is touched directly after that.
    std::unique_ptr<coop::net::Client> client_;
    std::unique_ptr<coop::host::Session> session_;
    // Debug item cheats (coop_cheats.h, LICHII_COOP_CHEATS).
    const bool cheats_ = CoopCheats::On();
    std::atomic<bool> stop_ = false;
    std::atomic<bool> quiet_ = false;
    std::string note_;  // network thread only
    CoopChat::Log chat_;  // locks itself
    std::atomic<bool> welcomed_once_ = false;
    std::atomic<bool> sources_reset_ = false;  // deku-sync-fix: welcomed or reconnected (network thread)
    // Network thread (Run): the "no packets from the server" warning is out (issue 143).
    bool quiet_warned_ = false;
    static constexpr std::uint32_t kQuietWarnMs = 3000;
    std::uint32_t name_retries_logged_ = 0;  // network thread (Run), issue 108
    std::uint32_t name_retries_base_ = 0;    // the total when this window began
    std::thread thread_;

    // Guarded by view_mutex_.
    std::mutex view_mutex_;
    RoomView room_;
    FrameView frame_;
    std::mutex color_mutex_;
    std::optional<std::optional<std::uint32_t>> color_change_;  // guarded: SetColor's, for the next Frame
};

std::mutex g_mutex;
bool g_started = false;
bool g_shut_down = false;
std::atomic<bool> g_mod_refused{false};  // a game mod patches where co-op does (Start)
std::unique_ptr<Coop> g_coop;  // set by Start or Begin, cleared by End or Shutdown
std::vector<std::unique_ptr<Coop>> g_ended;  // End's: kept until Shutdown (see End)
// SetColor's colour: a session started after it wears it, not the settings' (guarded).
std::optional<std::optional<std::uint32_t>> g_chosen_color;

// Joins the thread if the program ends without Shutdown.
void Stop(bool quiet) {
    std::unique_ptr<Coop> coop;
    std::vector<std::unique_ptr<Coop>> ended;
    {
        std::lock_guard lock(g_mutex);
        g_shut_down = true;
        coop = std::move(g_coop);
        ended.swap(g_ended);
    }
    if (coop) {
        coop->End(quiet);
    }
}

struct AtExit {
    ~AtExit() {
        Stop(true);
    }
} g_at_exit;

Coop* Get() {
    std::lock_guard lock(g_mutex);
    return g_coop.get();
}

// The session End stopped last (kept until Shutdown), or none.
Coop* LastEnded() {
    std::lock_guard lock(g_mutex);
    return g_ended.empty() ? nullptr : g_ended.back().get();
}

} // namespace

// The Lobby (docs/protocol.md "Lobby"): the player's own key, from
// player-key.txt beside coop.ini (made there once), so the host's mutes and
// bans follow the player and not the name.
static void WithPlayerKey(Settings& settings) {
    if (settings.player_key) {
        return;
    }
    std::string note;
    settings.player_key =
        LoadPlayerKey(Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "player-key.txt", note);
    if (!note.empty()) {
        LOG_INFO(Core, "coop: {}", note);
    }
}

void Start() {
    std::lock_guard lock(g_mutex);
    if (g_started || g_shut_down) {
        return;
    }
    g_started = true;
    const auto v = coop::enet_linked_version();
    LOG_INFO(Core, "coop: loaded (ENet {}.{}.{})", (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
    LOG_INFO(Core, "coop: build {}, protocol {}", LICHII_COOP_VERSION, coop::protocol::kProtocolVersion);

    // A game mod patches the game where co-op does (CoopSvc::Refusal, issue
    // 136): co-op stays off for this run, and the overlay says why.
    if (const std::string changed = CoopSvc::Refusal(); !changed.empty()) {
        g_mod_refused = true;
        LOG_WARNING(Core, "coop: off: a code mod is installed ({} is not the game's own). Co-op "
                          "does not work with game mods: remove them or turn them off in the "
                          "game's Add-Ons.",
                    changed);
        return;
    }
    const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
    std::vector<std::string> errors;
    auto settings = LoadSettings(ini, errors);
    // Each problem drops only its own line (CoopHook::ApplyIni): co-op goes on with the rest.
    for (const auto& e : errors) {
        LOG_WARNING(Core, "coop: {}", e);
    }
    if (!settings.enabled()) {
        LOG_INFO(Core, "coop: off (no server in {} or LICHII_COOP_SERVER)", ini.string());
        return;
    }
    LOG_INFO(Core, "coop: on, server {}:{}, name '{}', join password {}", settings.server,
             settings.port, settings.name, settings.password.empty() ? "none" : "set");
    if (g_chosen_color) {
        settings.color = *g_chosen_color;
    }
    WithPlayerKey(settings);
    auto coop = std::make_unique<Coop>(std::move(settings));
    if (coop->Begin()) {
        g_coop = std::move(coop);
    }
}

// Rows 63/76, #146 (dungeon-sync-b, dungeon-maps-a). The Map page and the minimap draw from the HUD screen's own copy
// of the dungeon's map, compass, boss key and floors, which the game reads in only at the HUD's init and its refresh
// (main+0x33e4200; the game's get scene ends in one). A value co-op writes in play (another player's item, the join
// snapshot, a reset, a put-back, the solo keeper) skips both, so the minimap stayed blank until the Map page or a
// room load (run hs-map-opener 20261116-013911). This asks the game's own refresh once after such a write, in a
// session and alone (Hookshot's frame asks it: coop_map_refresh.h). LICHII_COOP_MAP_REFRESH=0 turns it off. True when
// on.
bool StartMapRefresh() {
    static const bool on = [] {
        if (EnvString("LICHII_COOP_MAP_REFRESH") == "0") {
            LOG_INFO(Core, "coop: map refresh off (LICHII_COOP_MAP_REFRESH=0)");
            return false;
        }
        LOG_INFO(Core, "coop: map refresh on: the game's HUD refresh after a dungeon map, compass, boss key or "
                       "floors written by co-op (run at the HUD screen's own update, Longshot patch 0018)");
        return true;
    }();
    return on;
}

namespace {

// minimap-b. Set when a session ran: the solo keeper forgets its baseline (the session's writes are not the game's).
std::atomic<bool> g_keeper_reset{false};

// Row 112 (#74): our own Link with no session, for CurrentMapView (the Customize page's live picture). LocalFrameOff
// publishes it, a session's start clears it (coop_solo_view.h).
CoopSoloView::Store g_solo_view;

// A guard, not a fix (the cause is not found yet): with no session, a map or compass the game gave in this save is
// written back if the game takes it back while Link is in that dungeon (coop/host/one_way_keeper.hpp). In a session
// the flag watcher does it (issue 146). Through the game's own Bool setter (GameFlagStore). LICHII_COOP_KEEP_MAPS=0
// turns it off.
void KeepMapsAlone(EmulatorMemory& memory, bool in_play, const std::optional<std::string>& scene) {
    static const bool on = EnvString("LICHII_COOP_KEEP_MAPS") != "0";
    static coop::host::OneWayKeeper keeper = [] {
        coop::host::OneWayKeeper k;
        k.set_clock([] { return CoopEvery::NowUs(); });
        return k;
    }();
    if (!on) return;
    if (g_keeper_reset.exchange(false)) keeper.reset();
#ifdef COOP_HOOKSHOT
    // The title menu: the next save is another file (forest-temple-a: the no-scene sign alone may not come there).
    static bool title_was_up = false;
    const bool title_up = CoopSave::TitleUp();
    if (title_up && !title_was_up) keeper.reset();
    title_was_up = title_up;
    // Patch 0024: the game loaded a save (any way): another file's maps are not this one's.
    if (CoopSave::TakeKeeperSaveLoad()) keeper.reset();
#endif
    coop::host::GameFlagStore flags(memory);
    const coop::host::MapDropBecause because("the solo keeper (minimap-b)");
    const auto restored = keeper.frame(flags, in_play, coop::host::view_of(scene));
    if (keeper.take_save_load()) coop::host::map_drop_log().note("save load (no scene for a while: the title)");
    if (!restored.empty() && StartMapRefresh()) CoopMapRefresh::TheRefresher().WantNow(SteadyMs());  // row 140
    for (const auto key : restored) {
        const auto* item = coop::host::find_item(key.hash);
        LOG_INFO(Core, "coop: items: {} went back to 0 with no session: written back (minimap-b guard; restores {})",
                 item != nullptr ? item->name : std::string_view("?"), keeper.stats().restores);
    }
    for (const auto key : keeper.take_left()) {  // row 140 (flag-oneway-c)
        const auto* item = coop::host::find_item(key.hash);
        LOG_INFO(Core, "coop: items: {} went back to 0 right after each of {} write-backs with no session: left to the game "
                       "until it gives it again (row 140)",
                 item != nullptr ? item->name : std::string_view("?"), coop::host::OneWayKeeper::kMaxQuickUndos);
    }
}

// minimap-b diagnostics: one line whenever any dungeon's map or compass goes from 1 to 0, with what happened
// in the seconds before (coop/host/map_drop_log.hpp). In a session and alone.
void WatchMapDrops(EmulatorMemory& memory, bool in_play, const std::optional<std::string>& scene,
                   const std::optional<std::array<float, 3>>& link) {
    coop::host::GameFlagStore flags(memory);
    for (const auto& line : coop::host::map_drop_log().frame(
             flags, coop::host::view_of(scene), in_play, link)) {
        LOG_WARNING(Core, "{}", line);
    }
}

}  // namespace

// Split screen needs no session (docs/local-coop.md): with co-op off, the first Link is read here for local co-op's
// frame step, as Coop::Read does while it is on. Only Ocarina of Time on a build the table knows. What is read and
// when the Director is published: CoopLocalGame::OffFrame (tested).
// `writes`: false with a code mod (issue 136): only our Link for the Customize picture is read, nothing written.
void LocalFrameOff(Core::System& system, bool writes) {
    static Kernel::KProcess* seen = nullptr;
    static u64 base = 0;
    static CoopLocalGame::OffFrame step;
    auto* process = system.ApplicationProcess();
    if (process == nullptr || system.GetApplicationProcessProgramID() != kOcarinaProgramId ||
        CoopGameBuild::Find(Hex(system.GetApplicationProcessBuildID(), 8)) == nullptr) {
        if (writes) {
            CoopLocalGame::Frame({});
        }
        g_solo_view.Publish({}, CoopSoloView::Clock::now());
        return;
    }
    if (process != seen) {
        seen = process;
        base = Coop::MainOf(system, process);
    }
    EmulatorMemory memory(system.ApplicationMemory(), base);
    coop::host::PlayerReader reader(memory);
    std::optional<coop::host::PlayerReading> full;
    CoopLocalGame::Facts facts;
    if (writes && step.FullRead(CoopLocalGame::HasSeats())) {
        full = reader.read();
        facts = {full.has_value(), full ? std::optional(full->position) : std::nullopt,
                 full && full->age == coop::protocol::Age::adult};
    } else {
        // Solo: only what InPlay and Facts need.
        const auto position = reader.read_position();
        facts = {position.has_value(), position, position && reader.read_age() == coop::protocol::Age::adult};
    }
    if (writes) {
        CoopLocalGame::Frame(facts);
        // (Navi) New Game+ alone (coop_ngplus.h): no chat box here, the log and the Settings page say it.
        CoopNgPlus::Frame(memory, facts.position.has_value(), true, CoopSave::TitleUp());
        for (const auto& line : CoopNgPlus::TakeLines()) {
            LOG_INFO(Core, "coop: {}", line);
        }
    }
    // G1: whose the running event is, every frame (the tracker sees each run start and end). The
    // animation and our Link's performer are read only while an event runs.
    bool solo_running = false;
    std::optional<bool> solo_running_known;  // nothing: not read
    std::optional<std::string> solo_anim_name;
    coop::host::EventWhose solo_whose = coop::host::EventWhose::ours;
    if (full) {
        ApplyEventOwner(*full);
        solo_running_known = full->event_running;
        solo_running = solo_running_known.value_or(false);
        if (full->animation) solo_anim_name = full->animation->name;
        solo_whose = full->event_whose;
    } else {
        solo_running_known = reader.read_event_running();
        solo_running = solo_running_known.value_or(false);
        bool bound = false;
        if (solo_running) {
            if (const auto animation = reader.read_animation()) solo_anim_name = animation->name;
            bound = reader.read_link_bound().value_or(false);
        }
        solo_whose = ClassifyEvent(solo_running, bound,
                                   coop::host::view_of(solo_anim_name));
    }
    const std::optional<std::string_view> solo_anim =
        coop::host::view_of(solo_anim_name);
    {
        const auto scene = full ? full->scene : reader.read_scene();
        {   // Row 112: our Link for the Customize picture with no session, as a session's frame gives it.
            CoopSoloView::Reading solo;
            if (facts.position && scene) {
                solo.position = std::array<float, 3>{(*facts.position)[0], (*facts.position)[1], (*facts.position)[2]};
                solo.scene = scene;
                solo.adult = facts.adult;
                // Our own event (a cutscene, not the Ocarina, not another player's): G1, classified above.
                solo.event = coop::host::local_event_hides(solo_running, solo_whose, solo_anim);
            }
            g_solo_view.Publish(solo, CoopSoloView::Clock::now());
        }
        if (!writes) {
            return;  // a code mod: only the reading above
        }
        WatchMapDrops(memory, facts.position.has_value(), scene, facts.position);
        KeepMapsAlone(memory, facts.position.has_value(), scene);
        CoopMapRefresh::TheRefresher().SetInPlay(facts.position.has_value());  // rows 63/76: no refresh off play (alone)
        static MendClocks mend_since_us{};
        if (facts.position.has_value()) {
            MendStory(memory, mend_since_us, solo_running_known.has_value() && !*solo_running_known);  // issue 84,
            // row 129: a stuck save, played alone
        } else {
            mend_since_us.NewLoad();  // the title or a load: nothing to read yet
        }
    }
    if (!step.Publish(CoopLocalGame::HasSeats())) return;
    if (!full) full = reader.read();   // seats came in this frame's step
    if (full) full->event_whose = solo_whose;  // G1: classified once this frame, above
    // The extra local players' Players are made (and deleted) by the puppet Director, as online (no remote players).
    const bool event =
        full && coop::host::local_event_hides(full->event_running.value_or(false), full->event_whose,
                                              coop::host::name_of(full->animation));
    CoopPuppet::Publish(full ? base : 0, full ? full->scene : std::nullopt,
                        full ? std::optional(full->position) : std::nullopt, event, nullptr, 0);
}

namespace {
void OnFrameBody(Core::System& system);
}

// save-crash-0118: the frame's co-op work behind the guarded boundary: a throw (a std::bad_alloc at the PC's commit
// limit, runs 081813 + 082906) ends this frame's co-op work, logged once, and never the game (solo too).
void OnFrame(Core::System& system) {
    static CoopBoundary::Site site{"CoopHook::OnFrame"};
    CoopBoundary::Run(site, [&] { OnFrameBody(system); },
                      [](const char* where, const char* what, std::uint64_t n) {
                          LOG_ERROR(Core, "coop: {} threw ({}, {} so far): this frame's co-op work skipped", where,
                                    what, n);
                      });
}

namespace {
void OnFrameBody(Core::System& system) {
    CoopObjects::OnFrame(system);
    {   // player report 120: the slot our game plays (Hookshot's patch 0025; solo too). Its own guard: a slot folder
        // that throws (a path the PC's code page cannot hold, no memory) costs only the slot, not the frame's co-op.
        static CoopBoundary::Site site{"CoopSave::PollSlotOpens"};
        CoopBoundary::Run(site, [] { CoopSave::PollSlotOpens(); },
                          [](const char* where, const char* what, std::uint64_t n) {
                              LOG_ERROR(Core, "coop: {} threw ({}, {} so far): the played save slot not read this "
                                              "frame", where, what, n);
                          });
    }
    CoopTestInput::OnFrame(system);
    Start();
    // g_coop only changes in Start (before this), End (which keeps the old
    // one alive until Shutdown) and Shutdown (after
    // emulation has stopped), so it stays valid for this frame.
    auto* coop = Get();
    {   // minimap-b: a session started or ended (the drop log's line; the solo keeper's baseline is dropped)
        static CoopSessionEnd::LastFrame<Coop> last;
        if (const auto change = last.Note(coop); change.started || change.ended) {
            coop::host::map_drop_log().note(change.started ? "co-op session started" : "co-op session ended");
            g_keeper_reset.store(true);
            g_solo_view.Clear();  // row 112: the session's frame from now, and a fresh reading after it
        }
        // Left from the menu (End): every part forgets the session on a frame that is not connected (the objects'
        // owners and copies, bosses held for their owner, spawns, drops, the time, the HUD, ...), as on a drop,
        // but no frame came after End. The ended session gets that one frame once End has stopped and kept it.
        if (Coop* ended = last.Take(LastEnded(), g_mod_refused)) {
            ended->Frame(system);
        }
    }
    // The game's main may be patched (or refused for a code mod) after co-op
    // started: then co-op disconnects and stays off (issue 136). The
    // Coop object stays until Shutdown: the overlay may hold it.
    if (CoopSites::MustStopForMod(coop != nullptr, g_mod_refused, coop ? CoopSvc::Refusal() : std::string{})) {
        g_mod_refused = true;
        LOG_WARNING(Core, "coop: off: a code mod is installed ({} is not the game's own). Co-op "
                          "does not work with code or cheat mods: remove them or turn them off "
                          "in the game's Add-Ons. Disconnecting.",
                    CoopSvc::Refusal());
        coop->End(false);
        // Our own table only (no game write): a copy boss held at life 1 for its owner can die again here.
        if (CoopBossNative::Table& bosses = CoopBossNative::TheTable(); bosses.BrainOn()) {
            bosses.Publish({});
        }
    }
    switch (CoopSessionEnd::PathOf(coop != nullptr, g_mod_refused)) {
    case CoopSessionEnd::Path::session:
        coop->Frame(system);
        break;
    case CoopSessionEnd::Path::solo:
        LocalFrameOff(system, true);
        break;
    case CoopSessionEnd::Path::read_only: {
        // Issue 136, with or without a session before: what is not multiplayer and only reads the game goes on
        // (the Customize picture; the menus and settings never needed a session), nothing is written to it.
        static bool said = false;
        if (!std::exchange(said, true)) {
            LOG_INFO(Core, "coop: a code mod is installed: solo features that only read the game go on (the "
                           "Customize picture, menus, settings); nothing is written to the game (story and map "
                           "mends, split screen, a left session's clean-up) while the mod is installed");
        }
        LocalFrameOff(system, false);
        break;
    }
    }
}
}  // namespace

void Shutdown() {
    Stop(false);
}

bool Begin(Settings settings) {
    std::lock_guard lock(g_mutex);
    if (g_shut_down || g_coop || g_mod_refused || settings.server.empty()) {
        return false;
    }
    g_started = true;  // Start() then reads no coop.ini
    LOG_INFO(Core, "coop: on (from the menu), server {}:{}, name '{}', join password {}", settings.server,
             settings.port, settings.name, settings.password.empty() ? "none" : "set");
    if (g_chosen_color) {
        settings.color = *g_chosen_color;
    }
    WithPlayerKey(settings);
    auto coop = std::make_unique<Coop>(std::move(settings));
    if (!coop->Begin()) {
        return false;
    }
    g_coop = std::move(coop);
    return true;
}

void End() {
    std::unique_ptr<Coop> coop;
    {
        std::lock_guard lock(g_mutex);
        coop = std::move(g_coop);
    }
    if (!coop) {
        return;
    }
    coop->End(false);
    LOG_INFO(Core, "coop: off (left from the menu)");
    std::lock_guard lock(g_mutex);
    g_ended.push_back(std::move(coop));
}

void SetJoinPassword(const std::string& password) {
    std::lock_guard lock(g_mutex);
    if (g_coop) {
        g_coop->SetJoinPassword(password);
        LOG_INFO(Core, "coop: this game's own reconnect uses the new join password");   // (never the password)
    }
}

void SetColor(std::optional<std::uint32_t> color) {
    CoopPuppet::SetOwnColor(color);  // the test puppet and your name's colour
    std::lock_guard lock(g_mutex);
    g_chosen_color = color;
    if (g_coop) {
        g_coop->SetColor(color);
    }
    LOG_INFO(Core, "coop: colour changed in the game: {}",
             color ? CoopColors::Text(*color) : std::string("none chosen (slot colour)"));
}

std::vector<OverlayLine> OverlayLines() {
    if (g_mod_refused) {
        return {{"Co-op is off: a code mod is installed"}};
    }
    if (auto* coop = Get()) {
        return coop->Lines();
    }
    return {};
}

std::optional<Markers> CurrentMarkers() {
    if (g_mod_refused) {
        return std::nullopt;
    }
    if (auto* coop = Get()) {
        return coop->CurrentMarkers();
    }
    return std::nullopt;
}

bool CurrentMapView(MapView& out) {
    // A session's frame, but never one stopped for a code mod (issue 136: its Coop stays until Shutdown).
    if (!g_mod_refused) {
        if (auto* coop = Get()) {
            return coop->CurrentMapView(out);
        }
    }
    // Row 112 (#74): co-op idle is not "no Link": our own Link as LocalFrameOff read it, no other players. Also with
    // a code mod: the Customize picture only reads the game.
    CoopSoloView::View solo;
    if (!g_solo_view.Get(solo, CoopSoloView::Clock::now())) {
        return false;
    }
    out.scene.assign(solo.scene);
    out.self = solo.self;
    out.adult = solo.adult;
    out.event = solo.event;
    out.others.clear();
    return true;
}

bool SendChat(const std::string& text) {
    if (g_mod_refused) {
        return false;
    }
    auto* coop = Get();
    return coop && coop->SendChat(text);
}

std::vector<ChatLine> ChatLines() {
    if (g_mod_refused) {
        return {};
    }
    if (auto* coop = Get()) {
        return coop->ChatLines();
    }
    return {};
}

std::vector<PlayerRow> PlayerList() {
    if (g_mod_refused) {
        return {};
    }
    if (auto* coop = Get()) {
        return coop->PlayerList();
    }
    return {};
}

} // namespace CoopHook
