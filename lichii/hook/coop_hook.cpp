// Co-op presence inside the emulator (plan stage 1 step 1).
//
// Threads:
// - the present thread calls OnFrame: it reads the local player from guest
//   memory (coop_host), queues PLAYER_STATUS and PLAYER_SAMPLE, and works out
//   where the other players are shown (coop_presence);
// - the network thread owns the coop::net::Client: it sends what OnFrame
//   queued, runs ENet and feeds what comes in to presence;
// - the Qt co-op window reads OverlayLines() a few times a second.
// Lock order: net_mutex, then presence_mutex, then view_mutex. OnFrame never
// holds two at once.

#include "coop_hook.h"

#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"

#include "coop/host/game_memory.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/host/sampler.hpp"
#include "coop/net/client.hpp"
#include "coop/overlay/overlay.hpp"
#include "coop/presence/presence.hpp"
#include "coop/protocol/codec.hpp"
#include "coop/version.hpp"
#include "coop_settings.h"

namespace CoopHook {
namespace {

// The game and build the addresses in coop/host are for.
constexpr u64 kOcarinaProgramId = 0x0400486024DEC000ULL;
constexpr const char* kGameBuildId = "e0f3ccfaf50065ae";
constexpr const char* kGameVersion = "1.1.1";
constexpr const char* kBuildId = "lichii fritchii-v4.1 coop";

// A frame older than this means the game is not running (stopped, paused).
constexpr auto kFrameStale = std::chrono::seconds(1);

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

// Guest memory through the emulator. Unmapped reads fail quietly.
class EmulatorMemory : public coop::host::GameMemory {
public:
    EmulatorMemory(Core::Memory::Memory& memory, u64 base) : memory_(memory), base_(base) {}
    std::uint64_t main_base() const override {
        return base_;
    }
    bool read(std::uint64_t address, std::span<std::uint8_t> out) override {
        if (out.empty()) {
            return true;
        }
        if (address + out.size() < address ||
            !memory_.IsValidVirtualAddressRange(address, out.size())) {
            return false;
        }
        // The unsafe read skips GPU flushing: the player state is CPU data,
        // and the present thread must not wait on the GPU.
        return memory_.ReadBlockUnsafe(address, out.data(), out.size());
    }

private:
    Core::Memory::Memory& memory_;
    u64 base_;
};

// What OnFrame found, for the window.
struct FrameView {
    Clock::time_point at{};
    bool game = false;               // Ocarina of the right build
    std::string game_note;           // why not, when not
    std::optional<coop::protocol::Vec3> position;
    std::optional<std::string> scene;
    std::vector<coop::overlay::Shown> others;
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
    explicit Coop(Settings settings) : settings_(std::move(settings)) {}

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
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    // `quiet`: no logging, for the end of the program, when the log may be gone.
    void End(bool quiet) {
        {
            std::lock_guard lock(net_mutex_);
            stop_ = true;
            quiet_ = quiet;
        }
        wake_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    void Frame(Core::System& system) {
        FrameView view;
        view.at = Clock::now();
        const auto reading = Read(system, view);

        std::optional<std::uint64_t> now;
        bool reset = false;
        {
            std::lock_guard lock(net_mutex_);
            if (client_ && client_->state() == coop::net::State::connected) {
                now = client_->server_now_us();
                reset = std::exchange(reset_sampler_, false);
            }
        }

        if (now) {
            if (reset) {
                sampler_.reset();
            }
            std::vector<coop::protocol::Message> out;
            for (auto& m : sampler_.frame(reading, *now)) {
                coop::protocol::Message message;
                if (coop::protocol::decode(m.bytes, message) == coop::protocol::Error::none) {
                    out.push_back(std::move(message));
                }
            }
            if (!out.empty()) {
                {
                    std::lock_guard lock(net_mutex_);
                    for (auto& m : out) {
                        outbox_.push_back(std::move(m));
                    }
                }
                wake_.notify_one();
            }
            std::lock_guard lock(presence_mutex_);
            view.others = coop::overlay::collect(presence_, *now);
        }

        std::lock_guard lock(view_mutex_);
        frame_ = std::move(view);
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
            base_ = FindMain(process);
            sampler_.reset();
            LOG_INFO(Core, "coop: game build {}, main at {:#x}", build, base_);
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

    static u64 FindMain(Kernel::KProcess* process) {
        const auto modules = Core::FindModules(process);
        for (const auto& [address, name] : modules) {
            if (name == "main" || name.starts_with("main.")) {
                return address;
            }
        }
        return GetInteger(Core::FindMainModuleEntrypoint(process));
    }

    void Run() {
        std::unique_lock lock(net_mutex_);
        auto last_state = coop::net::State::idle;
        while (!stop_) {
            for (const auto& m : outbox_) {
                client_->send(m);
            }
            outbox_.clear();
            client_->service(0);
            while (auto event = client_->next_event()) {
                Handle(*event);
            }
            if (client_->state() != last_state) {
                last_state = client_->state();
                LOG_INFO(Core, "coop: {}", coop::net::to_string(last_state));
            }
            {
                std::lock_guard view(view_mutex_);
                room_.state = client_->state();
                room_.self = client_->player_number();
                room_.round_trip_us = client_->round_trip_us();
                room_.players = client_->players();
                room_.note = note_;
            }
            wake_.wait_for(lock, std::chrono::milliseconds(2),
                           [this] { return stop_ || !outbox_.empty(); });
        }
        client_->stop();
        client_->service(0);
        if (!quiet_) {
            LOG_INFO(Core, "coop: stopped");
        }
    }

    void Handle(const coop::net::Event& event) {
        using coop::net::EventKind;
        switch (event.kind) {
        case EventKind::welcomed:
        case EventKind::reconnected: {
            LOG_INFO(Core, "coop: welcomed as player {} on {}:{}", event.welcome.player_number,
                     settings_.server, settings_.port);
            note_.clear();
            reset_sampler_ = true;
            std::lock_guard lock(presence_mutex_);
            presence_.on_welcome(event.welcome);
            break;
        }
        case EventKind::refused:
            note_ = "refused: " +
                    std::string(coop::net::refuse_reason_text(event.refuse_reason));
            LOG_WARNING(Core, "coop: server {}", note_);
            break;
        case EventKind::disconnected: {
            LOG_WARNING(Core, "coop: connection lost; reconnecting");
            std::lock_guard lock(presence_mutex_);
            presence_.clear();
            break;
        }
        case EventKind::message: {
            std::lock_guard lock(presence_mutex_);
            presence_.apply(event.message);
            break;
        }
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

    // Present thread only.
    coop::host::Sampler sampler_;
    Kernel::KProcess* process_ = nullptr;
    u64 base_ = 0;
    bool build_warned_ = false;

    // Guarded by net_mutex_.
    std::mutex net_mutex_;
    std::condition_variable wake_;
    std::unique_ptr<coop::net::Client> client_;
    std::vector<coop::protocol::Message> outbox_;
    bool stop_ = false;
    bool quiet_ = false;
    bool reset_sampler_ = false;
    std::string note_;  // network thread only
    std::thread thread_;

    // Guarded by presence_mutex_.
    std::mutex presence_mutex_;
    coop::presence::Presence presence_;

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

} // namespace CoopHook
