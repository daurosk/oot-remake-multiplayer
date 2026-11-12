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

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <string_view>
#include <thread>

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
#include "coop/host/player_reader.hpp"
#include "coop/host/scene.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/overlay/overlay.hpp"
#include "coop/presence/presence.hpp"
#include "coop/version.hpp"
#include "coop_boss.h"
#include "coop_teleport_game.h"
#include "coop_pvp_game.h"
#include "coop_scene_probe.h"
#include "coop_camera.h"
#include "coop_chat.h"
#include "coop_hud.h"  // the HUD's hearts (PLAYER_LIFE), docs/overlay.md
#include "coop_cheats.h"
#include "coop_chests.h"
#include "coop_flags.h"
#include "coop_save.h"
#include "coop_game_build.h"
#include "coop_pickups.h"
#include "coop_fairy.h"
#include "coop_drops.h"
#include "coop_every.h"
#include "coop_dungeon.h"
#include "coop_story.h"
#include "coop_quests.h"
#include "coop_live_events.h"
#include "coop_objects.h"
#include "coop_pose.h"
#include "coop_puppet.h"
#include "coop_settings.h"
#include "coop_spawn.h"
#include "coop_sites.h"
#include "coop_svc.h"
#include "coop_time.h"
#include "coop_testinput.h"
#include "coop_world.h"

namespace CoopHook {
namespace {

// The game and build the addresses in coop/host are for.
constexpr u64 kOcarinaProgramId = 0x0400486024DEC000ULL;
// The builds it knows: coop_game_build.h (docs/game-builds.md).
constexpr std::string_view kGameBuildId = CoopGameBuild::kPrimary.id;
constexpr std::string_view kGameVersion = CoopGameBuild::kPrimary.version;
constexpr const char* kBuildId = "lichii fritchii-v5 coop";

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
struct FrameView {
    Clock::time_point at{};
    bool game = false;               // Ocarina of the right build
    std::string game_note;           // why not, when not
    std::optional<coop::protocol::Vec3> position;
    std::optional<std::string> scene;
    std::optional<coop::protocol::Age> age;
    std::vector<coop::overlay::Shown> others;
    std::vector<std::uint8_t> hidden;  // others with no marker (scene.hpp: hide)
    std::optional<Markers> markers;  // none: no camera
    std::string camera_note;         // for the panel, when the game runs
};

// What the network thread knows, for the window.
struct RoomView {
    coop::net::State state = coop::net::State::idle;
    std::uint8_t self = 0;
    std::optional<std::uint64_t> round_trip_us;
    std::vector<coop::net::Player> players;
    std::string note;  // the last refusal or error
};

class Coop {
public:
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
        config.hello.build_id = kBuildId;
        config.hello.game_build_id = std::string(kGameBuildId);
        config.hello.game_version = std::string(kGameVersion);
        config.hello.nickname = settings_.name;
        config.join_password = settings_.password;
        client_ = std::make_unique<coop::net::Client>(std::move(config));
        if (!client_->start()) {
            LOG_ERROR(Core, "coop: ENet did not start; co-op is off");
            client_.reset();
            return false;
        }
        coop::host::SessionSettings session;
        std::string flag_error;
        session.shared = LoadShared(flag_error);
        if (flag_error.empty()) CoopDungeon::AddShared(session.shared);  // docs/dungeon.md
        if (flag_error.empty()) CoopStory::AddShared(session.shared);  // docs/flags.md, "Story and cutscenes"
        if (flag_error.empty()) CoopQuests::AddShared(session.shared);  // docs/flags.md, "Threads of Time"
        if (flag_error.empty()) CoopChests::AddShared(session.shared);  // docs/items.md, "Chests"
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
            Log(event);
            CoopPuppet::OnEvent(event);
            CoopPose::OnEvent(event, SteadyMs());
            Chat(event, client);
            CoopObjects::OnEvent(event);
            CoopSpawn::OnEvent(event);
            CoopDrops::OnEvent(event);
            CoopTime::OnEvent(event);
            CoopBoss::OnEvent(event);  // docs/dungeons-boss.md
            CoopTeleport::OnEvent(event, client);  // /teleport (docs/setup.md "Chat commands")
            CoopPvp::OnEvent(event, client);  // PvP (docs/play-online.md "PvP")
            CoopSave::OnEvent(event, client);  // the host's save for the guests (issue 167)
            CoopHud::OnEvent(event);  // the others' hearts for the HUD (PLAYER_LIFE)
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
        if (live_events_) {
            CoopSvc::AddCaller(60, [this](CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                          std::uint64_t) {
                std::lock_guard lock(live_mutex_);
                auto call = live_requester_.Step(guest, after_call, result, base_);
                for (const auto& line : live_requester_.TakeNotes()) {
                    LOG_INFO(Core, "coop: dungeon: {}", line);
                }
                return call;
            });
            LOG_INFO(Core, "coop: dungeon: live events on (off with LICHII_COOP_LIVE_EVENTS=0)");
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
            if (sharing_ || cheats_) {
                game_store.emplace(*memory);
            }
            if (cheats_) {
                CoopCheats::Frame(*game_store);
            }
            if (sharing_) {
                // A small key or token we and another player took at once: ours
                // goes back. A token taken elsewhere: its Skulltula's key here
                // (docs/world.md, small keys and tokens).
                for (const std::string& count : CoopWorld::TakeGiveBacks()) {
                    LOG_INFO(Core, "coop: {}", CoopWorld::GiveBack(*game_store, count));
                }
                for (const std::uint64_t key : CoopWorld::TakeTakenKeys()) {
                    LOG_INFO(Core, "coop: {}", CoopWorld::SetTakenKey(*game_store, key));
                }
            }
            if (sharing_) {
                // Issue 138: grants wait for a loaded save, and the capacities
                // of held items are mended for a while after each load.
                const bool mend = load_mend_.frame(reading.has_value());
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
                    // Issue 190: a shared ocarina is equipped too, or D-pad
                    // Left takes nothing out.
                    MendOcarina(*memory);
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
        if (store) {
            quests.emplace(
                *store,
                [&memory](std::uint32_t hash) { return coop::host::GameFlags(*memory).get_enum(hash); },
                [&memory](std::uint32_t hash, std::uint32_t value) {
                    return coop::host::GameFlags::set_enum(*memory, hash, value);
                });
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
            hold.emplace(*quests, [link, scene](coop::protocol::FlagKey key) {
                if (!link || !scene || key.kind != coop::protocol::FlagKeyKind::named || key.index != 0) {
                    return false;
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
        const auto report = session_->frame(reading, hold ? &*hold : nullptr);
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
        if (report.save_load_flags != 0) {
            LOG_INFO(Core, "coop: flags: save load seen ({} flags back, {} frames without a scene)",
                     report.save_load_flags, report.save_load_frames);
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
            if (const auto name = live_decider_.Next(held_events, hold_scene_, running, link)) {
                LOG_INFO(Core, "coop: dungeon: starting the event {} here (another player played it)", *name);
                std::lock_guard lock(live_mutex_);
                live_requester_.Want(*name);
            }
        }
        if (hold && (hold->held() != 0) != holding_logged_) {
            holding_logged_ = hold->held() != 0;
            LOG_INFO(Core, "coop: dungeon: {}", holding_logged_
                         ? "room gimmick flags from another player held until we leave this dungeon or load"
                         : "no room gimmick flags held");
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
        if (report.host_reset) {
            // docs/flags.md, "Saves": the host loaded a save. Session-wide state
            // kept outside the flags is dropped here (each owner adds its clear:
            // locked doors' kept breaks, the kills-late dead list, ...).
            LOG_INFO(Core, "coop: flags: the host loaded a save: the group follows it");
            CoopObjects::ForgetKeptBreaks();  // locked doors' kept breaks (docs/dungeon.md)
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
        // Chests opened and pickups taken in another game: what is loaded of
        // them goes here too (docs/items.md, "Chests").
        // Issue 146: a map or compass the game took back, put back.
        // Each key logged the first time; the count says how often in all.
        for (const auto& k : session_->take_one_way_restores()) {
            LOG_INFO(Core,
                     "coop: flags: {:#010x} (dungeon map or compass) went back to 0 here: put back to 1 "
                     "({} put back this session)",
                     k.hash, session_->one_way_restores());
        }
        for (const auto& g : session_->take_counts_guarded()) {  // issue 89 (docs/items.md)
            LOG_INFO(Core,
                     "coop: flags: count {:#010x} +{} here was a pickup another player had taken: "
                     "put back, not sent",
                     g.count.hash, g.amount);
        }
        if (const auto taken = session_->take_written_for_others(); !taken.empty()) {
            std::vector<std::uint64_t> keys;
            for (const auto& k : taken) {
                if (k.kind == coop::protocol::FlagKeyKind::key64) {
                    keys.push_back(k.key);
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
        }
        {  // our own key spends, for the locks' give-back (docs/dungeon.md, "Locked doors")
            std::vector<std::uint32_t> drops;
            for (const auto& k : session_->take_own_count_drops()) {
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
        }
        // Issue 144: changes from the others to actors near our Link wait while
        // our Link is in an event (a talk, a cutscene).
        CoopObjects::SetLocalEvent(reading && reading->event_running.value_or(false));
        // Our own event hides the others (puppet, Navi, marker); playing the
        // Ocarina does not (coop/host/scene.hpp, local_event_hides).
        const bool event =
            reading && coop::host::local_event_hides(
                           reading->event_running.value_or(false),
                           reading->animation ? std::optional<std::string_view>(reading->animation->name)
                                              : std::nullopt);
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
                        s.scene ? std::optional<std::string_view>(*s.scene) : std::nullopt;
                    const std::optional<std::string_view> ours =
                        view.scene ? std::optional<std::string_view>(*view.scene) : std::nullopt;
                    if (coop::host::visibility(ours, event, theirs, player ? player->flags() : 0) ==
                        coop::host::Visibility::hide) {
                        view.hidden.push_back(s.number);
                    }
                }
                return shown;
            });
        }
        CoopPuppet::Publish(view.game ? base_ : 0, view.scene, view.position, event,
                            report.connected ? session_.get() : nullptr, report.server_now_us);
        CoopFairy::Publish(view.game ? base_ : 0, view.scene, view.position, event,
                           report.connected ? session_.get() : nullptr, report.server_now_us);
        CoopObjects::Share(system, report.connected ? session_.get() : nullptr, view.scene,
                           report.server_now_us);
        CoopHud::Frame(report.connected ? session_.get() : nullptr);  // our hearts (PLAYER_LIFE)
        CoopSpawn::Frame(system, report.connected ? session_.get() : nullptr, view.scene, report.server_now_us);
        CoopDrops::Frame(system, report.connected ? session_.get() : nullptr, view.scene, report.server_now_us);
        CoopTime::Frame(system, view.game ? base_ : 0, report.connected ? session_.get() : nullptr,
                        report.server_now_us);
        // Boss rooms together (docs/dungeons-boss.md).
        CoopBoss::Frame(system, view.game ? base_ : 0, view.scene, view.position,
                        reading && reading->event_running.value_or(false),
                        report.connected ? session_.get() : nullptr, report.server_now_us);
        // /teleport <player> (docs/setup.md "Chat commands"), after the boss
        // rooms: it reads whether our boss fight runs.
        CoopTeleport::Frame(system, view.game ? base_ : 0, reading, report.connected ? session_.get() : nullptr);
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

    // Chat and the player list (coop_hook.h). Any thread.
    bool SendChat(const std::string& text) {
        if (CoopTeleport::OnChat(text, *session_)) {
            return true;  // a command: done here, its answer shows as a notice
        }
        if (CoopPvp::OnChat(text, *session_)) {
            return true;  // /pvp: the same
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

    std::optional<coop::host::PlayerReading> Read(Core::System& system, FrameView& view) {
        auto* process = system.ApplicationProcess();
        if (process == nullptr) {
            view.game_note = "no game";
            build_warned_ = nullptr;  // the next game start warns again
            return std::nullopt;
        }
        if (system.GetApplicationProcessProgramID() != kOcarinaProgramId) {
            view.game_note = "not Ocarina of Time";
            return std::nullopt;
        }
        const auto build = Hex(system.GetApplicationProcessBuildID(), 8);
        if (CoopGameBuild::Find(build) == nullptr) {
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
        if (reading) {
            view.position = reading->position;
            view.scene = reading->scene;
            view.age = reading->age;
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
                                note_};
            });
            if (room.state != last_state) {
                last_state = room.state;
                LOG_INFO(Core, "coop: {}", coop::net::to_string(last_state));
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
        session_->with_client([](coop::net::Client& c) {
            c.stop();
            c.service(0);
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
                    std::string(coop::net::refuse_reason_text(event.refuse_reason));
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
            chat_.Add("joined as player " + std::to_string(event.welcome.player_number), true);
            break;
        case EventKind::reconnected:
            chat_.Add("reconnected as player " + std::to_string(event.welcome.player_number), true);
            break;
        case EventKind::disconnected:
            chat_.Add("connection lost, reconnecting", true);
            break;
        case EventKind::refused:
            chat_.Add("server " + note_, true);
            break;
        case EventKind::chat:
            chat_.Add(name(event.player, {}) + ": " + event.text, false);
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
    Kernel::KProcess* build_warned_ = nullptr;  // the process last warned about
    FlagDebug flag_debug_;  // LICHII_COOP_DEBUG_FLAGS and the other flag settings
    // Kept across frames: a drawn sword shows in the game's memory for a moment only.
    coop::host::PlayerReader::DrawnState drawn_state_{};
    bool sharing_ = false;  // a flag list is shared; set by Begin
    CoopBoss::StaleBossFlags stale_boss_;  // issue 148: shut boss doors from older builds
    bool pickups_ = false;  // one-time pickups are shared (coop_pickups.h)
    std::size_t pickups_given_ = 0;  // CoopPickups keys handed to the session
    bool holding_logged_ = false;  // issue 129: HoldFlagStore held something last frame
    bool live_events_ = false;     // LICHII_COOP_LIVE_EVENTS=1 (issue 129)
    // Chat notices, on unless LICHII_COOP_NOTIFY=0 (docs/setup.md).
    bool notify_ = EnvString("LICHII_COOP_NOTIFY") != "0";
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
    const bool debug_samples_ = EnvFlag("LICHII_COOP_DEBUG_SAMPLES");  // issue 106
    CoopEvery::Every samples_log_{5 * CoopEvery::kSecondUs};
    std::uint64_t samples_log_at_ = 0;
    std::uint32_t samples_seq_ = 0;
    std::uint64_t presents_ = 0;
    coop::host::LoadMend load_mend_;  // issue 138: when ItemFlagStore grants and mends

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

    // Issue 190 (docs/items.md, "The ocarina"): equips the best held ocarina
    // when PlayerStatus.EquipOcarina is "" (an ocarina from another player)
    // or the Fairy Ocarina while the Ocarina of Time is held. The String64 is
    // written straight into its buffer (the game has no queue for it), as the
    // teleport does. A flag that cannot be read is left alone.
    void MendOcarina(EmulatorMemory& memory) {
        namespace oc = coop::host::ocarina;
        coop::host::GameFlags flags(memory);
        const auto current = flags.get_string64(oc::kEquip);
        if (!current) {
            return;
        }
        const auto want = coop::host::ocarina_equip_mend(flags.get_int(oc::kFairy).value_or(0),
                                                         flags.get_int(oc::kTime).value_or(0), *current);
        if (!want) {
            return;
        }
        if (coop::host::GameFlags::set_string64_now(memory, oc::kEquip, *want)) {
            LOG_INFO(Core, "coop: items: ocarina '{}' -> '{}' for the held ocarina", *current, *want);
        } else if (!ocarina_mend_failed_logged_) {
            ocarina_mend_failed_logged_ = true;
            LOG_WARNING(Core, "coop: items: could not equip the held ocarina '{}'", *want);
        }
    }
    bool ocarina_mend_failed_logged_ = false;

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
};

std::mutex g_mutex;
bool g_started = false;
bool g_shut_down = false;
std::atomic<bool> g_mod_refused{false};  // a game mod patches where co-op does (Start)
std::unique_ptr<Coop> g_coop;  // set once by Start, cleared by Shutdown

// Joins the thread if the program ends without Shutdown.
void Stop(bool quiet) {
    std::unique_ptr<Coop> coop;
    {
        std::lock_guard lock(g_mutex);
        g_shut_down = true;
        coop = std::move(g_coop);
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

} // namespace

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
    for (const auto& e : errors) {
        LOG_ERROR(Core, "coop: {}", e);
    }
    if (!errors.empty()) {
        LOG_ERROR(Core, "coop: off (fix the settings above)");
        return;
    }
    if (!settings.enabled()) {
        LOG_INFO(Core, "coop: off (no server in {} or LICHII_COOP_SERVER)", ini.string());
        return;
    }
    LOG_INFO(Core, "coop: on, server {}:{}, name '{}', join password {}", settings.server,
             settings.port, settings.name, settings.password.empty() ? "none" : "set");
    auto coop = std::make_unique<Coop>(std::move(settings));
    if (coop->Begin()) {
        g_coop = std::move(coop);
    }
}

void OnFrame(Core::System& system) {
    CoopObjects::OnFrame(system);
    CoopTestInput::OnFrame(system);
    Start();
    // g_coop only changes in Start (before this) and Shutdown (after
    // emulation has stopped), so it stays valid for this frame.
    auto* coop = Get();
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
    }
    if (coop && !g_mod_refused) {
        coop->Frame(system);
    }
}

void Shutdown() {
    Stop(false);
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
