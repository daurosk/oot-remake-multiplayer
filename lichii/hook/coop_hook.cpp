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

#include "coop/host/game_memory.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/overlay/overlay.hpp"
#include "coop/presence/presence.hpp"
#include "coop/version.hpp"
#include "coop_camera.h"
#include "coop_flags.h"
#include "coop_objects.h"
#include "coop_settings.h"

namespace CoopHook {
namespace {

// The game and build the addresses in coop/host are for.
constexpr u64 kOcarinaProgramId = 0x0400486024DEC000ULL;
constexpr const char* kGameBuildId = "e0f3ccfaf50065ae";
constexpr const char* kGameVersion = "1.1.1";
constexpr const char* kBuildId = "lichii fritchii-v5 coop";

// A frame older than this means the game is not running (stopped, paused).
constexpr auto kFrameStale = std::chrono::seconds(1);
// A camera older than this is from a scene no longer drawn (a menu, a load).
constexpr auto kCameraStale = std::chrono::milliseconds(250);
// With LICHII_COOP_DEBUG_CAMERA, how often the local player's projection is logged.
constexpr std::uint64_t kDebugLogFrames = 120;

// The overlay's defaults are for centimetres; Ocarina's world unit is about a
// metre (the first run: a child Link walks about 2.5 units a second and the
// camera follows him about 4 units behind). So markers are 24 px across at
// 10 units, and nothing nearer than half a unit is drawn.
coop::overlay::Settings MarkerSettings() {
    coop::overlay::Settings s;
    s.reference_distance = 10.0f;
    s.near_distance = 0.5f;
    return s;
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
    std::vector<coop::overlay::Shown> others;
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
        config.hello.game_build_id = kGameBuildId;
        config.hello.game_version = kGameVersion;
        config.hello.nickname = settings_.name;
        client_ = std::make_unique<coop::net::Client>(std::move(config));
        if (!client_->start()) {
            LOG_ERROR(Core, "coop: ENet did not start; co-op is off");
            client_.reset();
            return false;
        }
        coop::host::SessionSettings session;
        std::string flag_error;
        session.shared = LoadShared(flag_error);
        if (!flag_error.empty()) {
            LOG_ERROR(Core, "coop: {}; flags are not shared", flag_error);
        }
        sharing_ = session.shared.has_value();
        // Called by service() on the network thread, with the client.
        session.on_event = [this](const coop::net::Event& event, coop::net::Client&) {
            Log(event);
        };
        session_ = std::make_unique<coop::host::Session>(*client_, std::move(session));
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
        std::optional<coop::host::GameFlagStore> store;
        if (view.game) {
            memory.emplace(system.ApplicationMemory(), base_);
            flag_debug_.Frame(*memory);
            if (sharing_) {
                store.emplace(*memory);
            }
        }
        const auto report = session_->frame(reading, store ? &*store : nullptr);
        if (report.connected) {
            view.others = session_->with_presence([&](coop::presence::Presence& p) {
                return coop::overlay::collect(p, report.server_now_us);
            });
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

private:
    // Projects the players with the camera the renderer found (coop_camera.h).
    // Without one, view.markers stays empty and only the panel shows.
    void Place(FrameView& view) {
        CoopCamera::SetWanted(view.game);
        if (!view.game) {
            return;
        }
        ++frames_;
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
        const bool log = debug_camera_ && frames_ % kDebugLogFrames == 0;
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
        if (debug_camera_ && view.position) {
            players.push_back({0, "you", *view.position, view.scene, 1.0f});
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
            return std::nullopt;
        }
        if (system.GetApplicationProcessProgramID() != kOcarinaProgramId) {
            view.game_note = "not Ocarina of Time";
            return std::nullopt;
        }
        const auto build = Hex(system.GetApplicationProcessBuildID(), 8);
        if (build != kGameBuildId) {
            view.game_note = "game build " + build + ", co-op needs " + kGameBuildId;
            if (!build_warned_) {
                build_warned_ = true;
                LOG_WARNING(Core, "coop: game build {} is not {}; nothing is read or sent", build,
                            kGameBuildId);
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
        coop::host::PlayerReader reader(memory);
        auto reading = reader.read();
        if (reading) {
            view.position = reading->position;
            view.scene = reading->scene;
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
            RoomView room = session_->with_client([&](coop::net::Client& c) {
                return RoomView{c.state(), c.player_number(), c.round_trip_us(), c.players(),
                                note_};
            });
            if (room.state != last_state) {
                last_state = room.state;
                LOG_INFO(Core, "coop: {}", coop::net::to_string(last_state));
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
            LOG_WARNING(Core, "coop: connection lost; reconnecting");
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
    std::uint64_t frames_ = 0;
    Kernel::KProcess* process_ = nullptr;
    u64 base_ = 0;
    bool build_warned_ = false;
    FlagDebug flag_debug_;  // LICHII_COOP_DEBUG_FLAGS and the other flag settings
    bool sharing_ = false;  // a flag list is shared; set by Begin

    // Set by Begin before the network thread starts; the session borrows the
    // client and locks it, so neither is touched directly after that.
    std::unique_ptr<coop::net::Client> client_;
    std::unique_ptr<coop::host::Session> session_;
    std::atomic<bool> stop_ = false;
    std::atomic<bool> quiet_ = false;
    std::string note_;  // network thread only
    std::thread thread_;

    // Guarded by view_mutex_.
    std::mutex view_mutex_;
    RoomView room_;
    FrameView frame_;
};

std::mutex g_mutex;
bool g_started = false;
bool g_shut_down = false;
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
    LOG_INFO(Core, "coop: on, server {}:{}, name '{}'", settings.server, settings.port,
             settings.name);
    auto coop = std::make_unique<Coop>(std::move(settings));
    if (coop->Begin()) {
        g_coop = std::move(coop);
    }
}

void OnFrame(Core::System& system) {
    CoopObjects::OnFrame(system);
    Start();
    // g_coop only changes in Start (before this) and Shutdown (after
    // emulation has stopped), so it stays valid for this frame.
    if (auto* coop = Get()) {
        coop->Frame(system);
    }
}

void Shutdown() {
    Stop(false);
}

std::vector<OverlayLine> OverlayLines() {
    if (auto* coop = Get()) {
        return coop->Lines();
    }
    return {};
}

std::optional<Markers> CurrentMarkers() {
    if (auto* coop = Get()) {
        return coop->CurrentMarkers();
    }
    return std::nullopt;
}

} // namespace CoopHook
