// SPDX-License-Identifier: GPL-3.0-or-later
//
// Co-op inside Hookshot (docs/hookshot.md): hookshot_coop.dll, loaded by our
// patch's host/coop_bridge.c. Presence and puppets: our Link is read and sent,
// the others are shown as puppet Links that copy their pose. The game logic is
// lichii/hook's (coop_puppet, coop_pose, coop_callers) and coop/'s; only the
// way into the game differs from the emulator: memory and calls through the
// bridge's table (hos_coop_api.h), on the game's calc thread at the frame hook.
//
// Settings as in the emulator (coop_settings.h): coop.ini beside the game,
// then LICHII_COOP_SERVER, _PORT, _NAME, _PASSWORD. Puppets on unless
// LICHII_COOP_PUPPET=0; LICHII_COOP_TEST_PUPPET=1 adds one beside you.
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "coop/host/game_flags.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/host/scene.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/protocol/messages.hpp"
#include "coop_callers.h"
#include "coop_game_build.h"
#include "coop_hookshot.h"
#include "coop_pose.h"
#include "coop_puppet.h"
#include "coop_settings.h"
#include "hos_coop_api.h"

namespace CoopHookshot {
namespace {

const HosCoopApi* g_api = nullptr;

void Log(const std::string& line) {
    if (g_api != nullptr && g_api->log != nullptr) {
        g_api->log(line.c_str());
    }
}

void Logf(const char* format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    Log(buf);
}

std::uint64_t SteadyMs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

bool EnvOff(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && std::string(v) == "0";
}

bool EnvOn(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && *v != 0 && std::string(v) != "0";
}

class Coop {
public:
    explicit Coop(CoopHook::Settings settings) : settings_(std::move(settings)) {}

    bool Begin() {
        coop::net::ClientConfig config;
        config.host = settings_.server;
        config.port = settings_.port;
        config.hello.build_id = kHelloBuild;
        config.hello.game_build_id = std::string(CoopGameBuild::kPrimary.id);
        config.hello.game_version = std::string(CoopGameBuild::kPrimary.version);
        config.hello.nickname = settings_.name;
        config.join_password = settings_.password;
        client_ = std::make_unique<coop::net::Client>(std::move(config));
        if (!client_->start()) {
            Log("coop: ENet did not start; co-op is off");
            client_.reset();
            return false;
        }
        coop::host::SessionSettings session;  // no shared flags yet (presence and puppets only)
        session.on_event = [](const coop::net::Event& event, coop::net::Client&) {
            CoopPuppet::OnEvent(event);
            CoopPose::OnEvent(event, SteadyMs());
        };
        CoopPose::SetLog([](const std::string& line) { Log("coop: " + line); }, EnvOn("LICHII_COOP_DEBUG_POSE"),
                         EnvOn("LICHII_COOP_POSE_AS"));
        session_ = std::make_unique<coop::host::Session>(*client_, std::move(session));
        thread_ = std::thread([this] { Run(); });
        return true;
    }

    void End() {
        stop_ = true;
        if (session_) {
            session_->wake();
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // The game's calc thread, once a frame (HOS_COOP_SITE_FRAME).
    void Frame(void* ctx) {
        BridgeMemory memory;
        coop::host::PlayerReader reader(memory, &drawn_state_);
        const auto reading = reader.read();
        session_->set_scene_readable(reader.read_scene().has_value());
        const auto report = session_->frame(reading, nullptr);
        std::optional<std::string> scene;
        std::optional<CoopPuppet::Vec3> position;
        bool event = false;
        if (reading) {
            scene = reading->scene;
            position = reading->position;
            event = coop::host::local_event_hides(
                reading->event_running.value_or(false),
                reading->animation ? std::optional<std::string_view>(reading->animation->name) : std::nullopt);
        }
        if (scene != last_scene_) {
            last_scene_ = scene;
            Logf("coop: scene %s", scene ? scene->c_str() : "(none)");
        }
        if (report.connected != connected_) {
            connected_ = report.connected;
            Log(connected_ ? "coop: in the room" : "coop: not in a room");
        }
        if (puppets_) {
            CoopPuppet::Publish(g_api->main_base, scene, position, event,
                                report.connected ? session_.get() : nullptr, report.server_now_us);
            RunCalls(ctx);
        }
    }

    void RunCalls(void* ctx) {
        BridgeGuest guest;
        const auto made = CoopCallers::RunFrame(guest, SteadyMs(), kCallsPerFrame, [ctx](const CoopPuppet::Call& c) {
            HosCoopCall k{};
            k.function = c.function;
            std::memcpy(k.x, c.x.data(), sizeof k.x);
            std::memcpy(k.stack, c.stack.data(), sizeof k.stack);
            k.s0 = c.s0;
            std::uint64_t x0 = 0;
            if (!g_api->call(ctx, &k, &x0)) {
                Logf("coop: no game function at %#llx", static_cast<unsigned long long>(c.function));
                return std::optional<std::uint64_t>();
            }
            return std::optional<std::uint64_t>(x0);
        });
        if (made != 0 && !calls_logged_) {
            calls_logged_ = true;
            Logf("coop: first game calls made (%zu this frame)", made);
        }
    }

    bool puppets_ = true;

private:
    static constexpr const char* kHelloBuild = "hookshot coop";
    static constexpr std::size_t kCallsPerFrame = 256;  // as the emulator's cave (coop_svc.h)

    // Guest memory through the bridge, for coop/host's readers.
    class BridgeMemory final : public coop::host::WritableGameMemory {
    public:
        std::uint64_t main_base() const override {
            return g_api->main_base;
        }
        bool read(std::uint64_t address, std::span<std::uint8_t> out) override {
            return g_api->read(address, out.data(), out.size()) != 0;
        }
        bool write(std::uint64_t address, std::span<const std::uint8_t> data) override {
            return g_api->write(address, data.data(), data.size()) != 0;
        }
        std::optional<std::uint32_t> compare_exchange_u32(std::uint64_t address, std::uint32_t expected,
                                                          std::uint32_t desired) override {
            std::uint32_t seen = 0;
            if (!g_api->cas32(address, expected, desired, &seen)) {
                return std::nullopt;
            }
            return seen;
        }
    };

    // The same, for the puppet director and the other callers.
    class BridgeGuest final : public CoopPuppet::Guest {
    public:
        bool Read(std::uint64_t address, void* out, std::size_t size) override {
            return g_api->read(address, out, size) != 0;
        }
        bool Write(std::uint64_t address, const void* in, std::size_t size) override {
            return g_api->write(address, in, size) != 0;
        }
    };

    void Run() {
        auto last = coop::net::State::idle;
        while (!stop_) {
            session_->service();
            session_->with_client([](coop::net::Client& c) { CoopPose::Send(c, SteadyMs()); });
            const auto state = session_->with_client([](coop::net::Client& c) { return c.state(); });
            if (state != last) {
                last = state;
                Log("coop: " + std::string(coop::net::to_string(state)));
            }
            session_->wait(2);
        }
        session_->with_client([](coop::net::Client& c) {
            c.stop();
            c.service(0);
        });
    }

    const CoopHook::Settings settings_;
    std::unique_ptr<coop::net::Client> client_;
    std::unique_ptr<coop::host::Session> session_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    coop::host::PlayerReader::DrawnState drawn_state_{};
    std::optional<std::string> last_scene_;
    bool connected_ = false;
    bool calls_logged_ = false;
};

std::mutex g_mutex;
std::unique_ptr<Coop> g_coop;

}  // namespace

bool Start(const HosCoopApi* api) {
    std::lock_guard lock(g_mutex);
    if (api == nullptr || api->version != HOS_COOP_API_VERSION || api->size < sizeof(HosCoopApi)) {
        return false;
    }
    g_api = api;
    const std::string build = api->main_build_id != nullptr ? api->main_build_id : "";
    if (!BuildSupported(build)) {
        Logf("coop: %s", CoopGameBuild::LogRefusal(build.substr(0, 16)).c_str());
        return false;
    }
    Logf("coop: loaded, protocol %u, game build %.16s, main at %#llx", static_cast<unsigned>(coop::protocol::kProtocolVersion),
         build.c_str(), static_cast<unsigned long long>(api->main_base));
    const std::filesystem::path ini = std::filesystem::path(api->game_dir != nullptr ? api->game_dir : ".") / "coop.ini";
    std::vector<std::string> errors;
    auto settings = CoopHook::LoadSettings(ini, errors);
    for (const auto& e : errors) {
        Log("coop: " + e);
    }
    if (!errors.empty()) {
        Log("coop: off (fix the settings above)");
        return false;
    }
    if (!settings.enabled()) {
        Log("coop: off (no server in " + ini.string() + " or LICHII_COOP_SERVER)");
        return false;
    }
    Logf("coop: on, server %s:%u, name '%s', join password %s", settings.server.c_str(),
         static_cast<unsigned>(settings.port), settings.name.c_str(), settings.password.empty() ? "none" : "set");
    CoopPuppet::PlannerSettings planner;
    planner.debug = EnvOn("LICHII_COOP_TEST_PUPPET");
    planner.debug_anim = EnvOn("LICHII_COOP_DEBUG_ANIM");
    if (const char* actor = std::getenv("LICHII_COOP_PUPPET_ACTOR"); actor != nullptr && *actor != 0) {
        planner.actor = actor;
    }
    CoopPuppet::Configure(std::move(planner), EnvOn("LICHII_COOP_DEBUG_PUPPET"), [](const std::string& line) { Log(line); });
    auto coop = std::make_unique<Coop>(std::move(settings));
    coop->puppets_ = !EnvOff("LICHII_COOP_PUPPET");
    Logf("coop: puppets %s", coop->puppets_ ? "on" : "off (LICHII_COOP_PUPPET=0)");
    if (!coop->Begin()) {
        return false;
    }
    g_coop = std::move(coop);
    return true;
}

void Frame(void* ctx, int site) {
    // The calc thread only; g_coop changes only in Start (before any frame)
    // and Stop (at exit).
    if (site == HOS_COOP_SITE_FRAME && g_coop) {
        g_coop->Frame(ctx);
    }
}

std::uint32_t Hold(std::uint64_t, std::uint32_t bits) {
    return bits;  // the AI hold comes with shared enemies (coop_objects), not in this build yet
}

void Stop() {
    std::lock_guard lock(g_mutex);
    if (g_coop) {
        g_coop->End();
    }
}

bool BuildSupported(const std::string& build_id_hex) {
    if (build_id_hex.size() < 16) {
        return false;
    }
    return CoopGameBuild::Find(build_id_hex.substr(0, 16)) != nullptr;
}

}  // namespace CoopHookshot

extern "C" {
COOP_HOOKSHOT_EXPORT int hos_coop_start(const HosCoopApi* api) {
    return CoopHookshot::Start(api) ? 1 : 0;
}
COOP_HOOKSHOT_EXPORT void hos_coop_frame(void* ctx, int site) {
    CoopHookshot::Frame(ctx, site);
}
COOP_HOOKSHOT_EXPORT uint32_t hos_coop_hold(uint64_t component, uint32_t bits) {
    return CoopHookshot::Hold(component, bits);
}
COOP_HOOKSHOT_EXPORT void hos_coop_stop(void) {
    CoopHookshot::Stop();
}
}
