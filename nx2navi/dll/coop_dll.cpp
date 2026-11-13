// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op inside the NX2navi port, as a DLL the launcher loads into the game
// (nx2navi/README.md).
//
// The same shape as lichii/hook/coop_hook.cpp, without the emulator:
// - a frame reads the local player from the game's memory and hands it to
//   coop::host::Session::frame, which queues PLAYER_STATUS and PLAYER_SAMPLE.
//   In a game with the plugin interface that is done ON THE GAME'S THREAD, in
//   the game-thread callback of puppet.cpp (the frame step hook), through the
//   interface's read and write: no thread of ours touches the game's memory
//   (README, "Frame rate"). Our own thread then only keeps the session alive
//   while the game makes no frame. In a game without the interface our own
//   thread reads, 60 times a second, from outside (guest_memory.hpp);
// - the network thread runs Session::service: it sends what frame() queued,
//   runs ENet and feeds what comes in to presence (docs/host.md, "Session").
//
// A game with NX2navi's plugin interface (third_party/plugin_api.h) also gets
// the in-game panel, notices and chat box (overlay.hpp): the interface comes
// from nx2navi_plugin_init below, or from the game's export once the game is
// found. With `puppet = 1` the interface's frame callback also shows the
// other players as a second Link (puppet.hpp). A game without the interface
// gets exactly the two threads above. Markers are not here: see the README.
//
// Nothing is read until main is found at NX2navi's fixed base and its build
// id is the one the addresses in coop/host are for (guest_memory.hpp).
//
// The DLL is in the game at every start with the mod enabled (mod.json lists
// it) and idles until there is a session: one the launcher's Host or Join
// named (OOT_COOP_INI in the environment), or one the player starts from the
// title screen's "Multiplayer" entry (title_menu.hpp; interface version 3).
// Hosting there starts bin\oot-coop-server.exe from the game; joining takes
// the host's save first (save_sync.hpp) and starts the game again on the
// co-op profile it was written to.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <share.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "coop/host/game_flags.hpp"
#include "coop/host/game_items.hpp"
#include "coop/host/notify.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"
#include "coop/presence/presence.hpp"
#include "coop_game_build.h"
#include "ai_probe.hpp"
#include "guest_memory.hpp"
#include "objects/objects.hpp"
#include "progress.hpp"
#include "puppet_fx.hpp"
#include "upstream.hpp"
#ifdef COOP_UPSTREAM
#include "coop_puppet.h"
#include "coop_puppet_sound.h"
#include "coop_hud.h"
#endif
#include "objects/scene_gate.hpp"
#include "coop/host/scene.hpp"
#include "coop_pose.h"
#include "overlay.hpp"
#include "menu_model.hpp"
#include "mp_saves.hpp"
#include "puppet.hpp"
#include "save_sync.hpp"
#include "settings.hpp"
#include "teleport.hpp"
#include "steam_menu.hpp"
#include "steam_net.hpp"
#include "title_menu.hpp"
#include "third_party/plugin_api.h"

namespace {

constexpr const char* kBuildId = "nx2navi coop-dll 0.9";
constexpr auto kFrame = std::chrono::microseconds(16'667);  // 60 a second
constexpr auto kStatusEvery = std::chrono::seconds(10);

using Clock = std::chrono::steady_clock;

std::atomic<bool> g_stop{false};
std::atomic<int> g_state{0};  // coop_nx2navi_state
std::mutex g_wake_mutex;
std::condition_variable g_wake;
HANDLE g_thread = nullptr;

std::uint64_t SteadyUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
            .count());
}

// --- The log: coop-nx2navi.log in the mod folder, one line at a time ---

class Log {
public:
    // Once: the first caller (the DLL's thread or nx2navi_plugin_init, which
    // can come first) opens it, and later calls keep what is written.
    void Open(const std::filesystem::path& path) {
        std::lock_guard lock(mutex_);
        if (file_ == nullptr) {
            // A game the mod started again for the host's save goes on in the log
            // of the game that fetched it.
            wchar_t ready[4] = {};
            bool append = GetEnvironmentVariableW(L"OOT_COOP_SAVE_READY", ready, 4) == 1 && ready[0] == L'1';
            // The same for a game started again to host on a multiplayer save.
            append = append || (GetEnvironmentVariableW(L"OOT_COOP_MP_HOST", ready, 4) == 1 && ready[0] == L'1');
            file_ = _wfsopen(path.c_str(), append ? L"ab" : L"wb", _SH_DENYNO);
        }
    }
    bool open() const { return file_ != nullptr; }
#if defined(__GNUC__)
    __attribute__((format(gnu_printf, 2, 3)))
#endif
    void Line(const char* format, ...) {
        std::lock_guard lock(mutex_);
        if (file_ == nullptr) {
            return;
        }
        SYSTEMTIME now{};
        GetLocalTime(&now);
        std::fprintf(file_, "%02u:%02u:%02u.%03u coop: ", now.wHour, now.wMinute, now.wSecond,
                     now.wMilliseconds);
        va_list args;
        va_start(args, format);
        std::vfprintf(file_, format, args);
        va_end(args);
        std::fputs("\r\n", file_);
        std::fflush(file_);
    }

private:
    std::mutex mutex_;
    std::FILE* file_ = nullptr;
};

Log g_log;

void LogLine(const char* line) {
    g_log.Line("%s", line);
}

std::string Utf8(const std::wstring& wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(size, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::wstring Environment(const wchar_t* name) {
    std::wstring value(32768, L'\0');
    const DWORD size = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
    value.resize(size < value.size() ? size : 0);
    return value;
}

std::filesystem::path ModulePath(HMODULE module) {
    std::wstring path(32768, L'\0');
    const DWORD size = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    path.resize(size < path.size() ? size : 0);
    return path;
}

// The mod folder: where this DLL is, or one up when it is in bin\.
std::filesystem::path ModFolder(HMODULE module) {
    std::filesystem::path folder = ModulePath(module).parent_path();
    if (_wcsicmp(folder.filename().c_str(), L"bin") == 0) {
        folder = folder.parent_path();
    }
    return folder;
}

std::optional<std::string> ReadFile(const std::filesystem::path& path) {
    std::FILE* file = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (file == nullptr) {
        return std::nullopt;
    }
    std::string text;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0 && text.size() < (1u << 20)) {
        text.append(buffer, got);
    }
    std::fclose(file);
    return text;
}

// Waits until `when` or until the DLL is told to stop. True when stopping.
bool SleepUntil(Clock::time_point when) {
    std::unique_lock lock(g_wake_mutex);
    return g_wake.wait_until(lock, when, [] { return g_stop.load(); });
}

// coop/host's view of the game's memory on the game's thread: the access the
// game-thread callback has (the plugin interface's read and write).
class GuestView final : public coop::host::WritableGameMemory {
public:
    GuestView(CoopPuppet::Guest& guest, std::uint64_t main) : guest_(guest), main_(main) {
        const Nx2PluginApi* api = CoopNx2navi::Overlay::Api();
        if (api != nullptr && NX2_HAS(api, Nx2PluginApi, map_range) && api->map_range != nullptr) {
            map_ = api->map_range;
            for (Page& page : pages_) {
                page.number = ~std::uint64_t{0};
            }
        }
    }
    std::uint64_t main_base() const override { return main_; }
    // Shared progress reads some 40,000 small values a frame (2,265 flags, each
    // found by a search in the game's tables). The interface's `read` checks
    // the game's mapping at every call; `map_range` checks a page once and
    // hands back a pointer that is good until this callback returns (NX2navi
    // docs/plugins.md, "Memory"), so a page is asked for once a frame and
    // every read in it is a copy. The pointer is the game's own memory: a
    // write through the interface shows in it at once.
    bool read(std::uint64_t address, std::span<std::uint8_t> out) override {
        if (address < 0x10000) {
            return false;
        }
        const std::size_t size = out.size();
        if (map_ != nullptr && size != 0 && ((address ^ (address + size - 1)) >> 12) == 0) {
            const std::uint64_t number = address >> 12;
            Page& page = pages_[number & (kPages - 1)];
            if (page.number != number) {
                page.number = number;
                page.at = static_cast<const std::uint8_t*>(map_(number << 12, 4096, 0));
            }
            if (page.at != nullptr) {
                std::memcpy(out.data(), page.at + (address & 0xfff), size);
                return true;
            }
        }
        return guest_.Read(address, out.data(), size);
    }
    bool write(std::uint64_t address, std::span<const std::uint8_t> data) override {
        return address >= 0x10000 && guest_.Write(address, data.data(), data.size());
    }
    // On the word itself, mapped by the interface (version 2): the game's own
    // CAS is a host atomic on the same word.
    std::optional<std::uint32_t> compare_exchange_u32(std::uint64_t address, std::uint32_t expected,
                                                      std::uint32_t desired) override {
        const Nx2PluginApi* api = CoopNx2navi::Overlay::Api();
        if (api == nullptr || (address & 3) != 0 || !NX2_HAS(api, Nx2PluginApi, map_range) ||
            api->map_range == nullptr) {
            return std::nullopt;
        }
        void* at = api->map_range(address, 4, 1);
        if (at == nullptr) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(
            InterlockedCompareExchange(static_cast<volatile LONG*>(at), static_cast<LONG>(desired),
                                       static_cast<LONG>(expected)));
    }

private:
    static constexpr std::size_t kPages = 1024;
    struct Page {
        std::uint64_t number;
        const std::uint8_t* at;
    };
    CoopPuppet::Guest& guest_;
    std::uint64_t main_;
    void* (*map_)(std::uint64_t, std::size_t, int) = nullptr;
    Page pages_[kPages];
};

// Reads are kept by page for a frame (guest_memory.hpp). A write decides from
// what it reads first, so it starts from fresh pages.
class FreshWrites final : public coop::host::FlagStore {
public:
    // `memory`: the page cache to renew, or null (the game thread's view has none).
    FreshWrites(coop::host::FlagStore& inner, CoopNx2navi::Memory* memory)
        : inner_(inner), memory_(memory) {}
    std::optional<coop::protocol::FlagEntry> read(coop::host::FlagKey key) override {
        return inner_.read(key);
    }
    bool write(const coop::protocol::FlagEntry& entry) override {
        Fresh();
        return inner_.write(entry);
    }
    bool is_count(coop::host::FlagKey key) override { return inner_.is_count(key); }
    bool is_max(coop::host::FlagKey key) override { return inner_.is_max(key); }
    bool holds(coop::host::FlagKey key) override { return inner_.holds(key); }
    bool is_one_way(coop::host::FlagKey key) override { return inner_.is_one_way(key); }
    bool write_exact(const coop::protocol::FlagEntry& entry) override {
        Fresh();
        return inner_.write_exact(entry);
    }

private:
    void Fresh() {
        if (memory_ != nullptr) {
            memory_->Fresh();
        }
    }
    coop::host::FlagStore& inner_;
    CoopNx2navi::Memory* memory_;
};

// The title screen's entry and the exit callback, once, as soon as the game's
// interface is known: from the idle loop, or from a session's frames.
std::filesystem::path g_folder;
void EnsureTitleEntry();

#ifdef COOP_UPSTREAM
#include "coop_up.inc"
#else
class Coop {
public:
    // `welcome`: what the title menu's page says once the server let us in (a
    // sentence in the menu's language), when the session was started there.
    explicit Coop(CoopNx2navi::Settings settings, std::string welcome = {})
        : settings_(std::move(settings)),
          welcome_(std::move(welcome)),
          notifier_([this](std::uint8_t number) { return Name(number); },
                    &CoopNx2navi::Progress::ExtraNotice) {}

    bool Begin() {
        coop::net::ClientConfig config;
        config.host = settings_.coop.server;
        config.port = settings_.coop.port;
        config.hello.build_id = kBuildId;
        config.hello.game_build_id = std::string(CoopGameBuild::kPrimary.id);
        config.hello.game_version = std::string(CoopGameBuild::kPrimary.version);
        config.hello.nickname = settings_.coop.name;
        config.join_password = CoopNx2navi::Settings::JoinPassword(settings_.coop.password);
        client_ = std::make_unique<coop::net::Client>(std::move(config));
        if (!client_->start()) {
            g_log.Line("ENet did not start; co-op is off");
            client_.reset();
            return false;
        }
        coop::host::SessionSettings session;
        // Shared progress and world state, as the emulator hook wires it
        // (progress.hpp): items, songs, keys, chests, one-time pickups, dungeon
        // switches, doors, cleared rooms, story flags.
        progress_.Configure(session, settings_.share_progress, [](const std::string& line) {
            g_log.Line("%s", line.c_str());
        });
        session.clock = [] { return SteadyUs(); };
        session.on_event = [this](const coop::net::Event& event, coop::net::Client& client) {
            OnEvent(event);
            CoopNx2navi::Puppet::OnEvent(event);
            CoopNx2navi::Teleport::OnEvent(event, client);
            CoopNx2navi::Objects::OnEvent(event);
            progress_.OnEvent(event);
            CoopNx2navi::SaveSync::OnEvent(event, client);
        };
        {
            // Going to another player (teleport.hpp): the command, the join at
            // the host and the follow through exits. All of it runs in the
            // game-thread callback puppet.cpp registers, puppets or not.
            CoopNx2navi::Teleport::Auto::Options automatic;
            automatic.join_at_host = settings_.join_at_host;
            automatic.follow =
                settings_.follow == CoopNx2navi::Settings::Follow::host
                    ? CoopNx2navi::Teleport::FollowMode::host
                    : settings_.follow == CoopNx2navi::Settings::Follow::anyone
                          ? CoopNx2navi::Teleport::FollowMode::anyone
                          : CoopNx2navi::Teleport::FollowMode::off;
            CoopNx2navi::Teleport::Configure(automatic, LogLine);
            CoopNx2navi::Teleport::TestGoto(Utf8(Environment(L"OOT_COOP_TEST_GOTO")));
            CoopNx2navi::Teleport::TestScene(Utf8(Environment(L"OOT_COOP_TEST_SCENE")));
            debug_position_ = Environment(L"OOT_COOP_DEBUG_POS") == L"1";
            CoopNx2navi::Teleport::SetArriveEvent(Environment(L"OOT_COOP_TELEPORT_ARRIVE") == L"1");
        }
        const bool frame_hook = Environment(L"OOT_COOP_FRAME_HOOK") != L"0";
        if (const std::wstring reads = Environment(L"OOT_COOP_READS"); reads == L"thread") {
            reads_ = Reads::thread;
            g_log.Line("reads: on this DLL's own thread, from outside (OOT_COOP_READS=thread: a test)");
        } else if (reads == L"off") {
            reads_ = Reads::off;
            g_log.Line("reads: off (OOT_COOP_READS=off: a test; nothing is read about the player)");
        }
        CoopNx2navi::Puppet::SetFrameWork(GameThreadFrame, this);
        if (settings_.puppet) {
            // The frame callback is registered once the game is found and has
            // the interface (Frame); a game without it never shows a puppet.
            CoopNx2navi::Puppet::Options options;
            options.frame_hook = frame_hook;
            options.test_puppet = Environment(L"OOT_COOP_TEST_PUPPET") == L"1";
            options.verbose = Environment(L"OOT_COOP_DEBUG_PUPPET") == L"1";
            options.debug_anim = Environment(L"OOT_COOP_DEBUG_ANIM") == L"1";
            options.debug_pose = Environment(L"OOT_COOP_DEBUG_POSE") == L"1";
            // The bone-by-bone pose: on, as in the emulator build (README, "Puppets").
            options.pose_copy = pose_copy_ = Environment(L"OOT_COOP_PUPPET_POSE") != L"0";
            CoopNx2navi::Puppet::Configure(options, LogLine);
        } else {
            CoopNx2navi::Puppet::Options options;
            options.puppets = false;
            options.frame_hook = frame_hook;
            CoopNx2navi::Puppet::Configure(options, LogLine);
            g_log.Line("puppet: off (puppet = 0 in coop.ini)");
        }
        {
            // Shared enemies and boss rooms (objects/INTEGRATION.md): off unless objects = 1.
            CoopNx2navi::Objects::Options o;
            o.on = settings_.objects;
            o.boss = settings_.objects_boss;
            o.ai_hold = settings_.objects_ai_hold;
            o.world = settings_.objects_world;
            o.friendly = settings_.objects_friendly;
            o.time = settings_.time;
            o.drops = settings_.drops;
            o.rolls_apart_m = settings_.objects_rolls_apart;
            o.debug = Environment(L"OOT_COOP_DEBUG_OBJECTS") == L"1";
            o.near_log = Environment(L"OOT_COOP_DEBUG_NEAR") == L"1";
            objects_near_log_ = o.on && o.near_log;
            // The interface may not be known yet; Frame hands it over again.
            CoopNx2navi::Objects::Init(CoopNx2navi::Overlay::Api(), o, LogLine);
            if (!o.on) {
                g_log.Line("objects: off (objects = 0 in coop.ini)");
            }
        }
        session_ = std::make_unique<coop::host::Session>(*client_, std::move(session));
        progress_.Started(*session_);
        // The line the launcher's menu looks for ("coop: on, server").
        g_log.Line("on, server %s:%u as '%s', join password %s, shared progress %s",
                   settings_.coop.server.c_str(), static_cast<unsigned>(settings_.coop.port),
                   settings_.coop.name.c_str(), settings_.coop.password.empty() ? "none" : "set",
                   settings_.share_progress ? "on" : "off (share_progress = 0)");
        g_state = 1;
        CoopNx2navi::Overlay::SetMinimap(settings_.minimap,
                                         Environment(L"OOT_COOP_DEBUG_MINIMAP") == L"1");
        CoopNx2navi::Overlay::SetNameTags(settings_.name_tags,
                                          Environment(L"OOT_COOP_DEBUG_TAGS") == L"1");
        // For tests (OOT_COOP_TEST_LIFE="<seconds>:<quarter hearts>;..."): our
        // Link's life is written that many seconds after it first was in the
        // game, on the game's thread (to let a Link die for the way back after
        // a Game Over).
        {
            std::string spec = Utf8(Environment(L"OOT_COOP_TEST_LIFE"));
            while (!spec.empty()) {
                const auto end = spec.find(';');
                const std::string part = spec.substr(0, end);
                spec = end == std::string::npos ? std::string() : spec.substr(end + 1);
                double at = 0.0;
                int value = 0;
                if (std::sscanf(part.c_str(), "%lf:%d", &at, &value) == 2 && at >= 0.0 && value >= 0) {
                    test_life_.push_back({static_cast<std::uint64_t>(at * 1e6), value});
                }
            }
            if (!test_life_.empty()) {
                g_log.Line("test: %u writes of our Link's life (OOT_COOP_TEST_LIFE)",
                           static_cast<unsigned>(test_life_.size()));
            }
        }
        if (settings_.overlay) {
            // Registers now if nx2navi_plugin_init already gave the
            // interface, else as soon as it comes (FindGame asks the game).
            CoopNx2navi::Overlay::Want();
        } else {
            g_log.Line("overlay: off (overlay = 0 in coop.ini)");
        }
        return true;
    }

    void Run() {
        std::thread network([this] {
            try {
                while (!g_stop) {
                    session_->service();
                    if (pose_copy_) {
                        const auto now_ms = static_cast<std::uint64_t>(SteadyUs() / 1000);
                        session_->with_client(
                            [&](coop::net::Client& c) { CoopPose::Send(c, now_ms); });
                    }
                    session_->wait(2);
                }
            } catch (const std::exception& e) {
                g_log.Line("network thread stopped: %s", e.what());
            } catch (...) {
                g_log.Line("network thread stopped by an unknown error");
            }
        });
        try {
            auto next = Clock::now();
            while (!g_stop) {
                Tick();
                next += kFrame;
                const auto now = Clock::now();
                if (next < now) {
                    next = now;  // a stall: no burst of frames to catch up
                }
                if (SleepUntil(next)) {
                    break;
                }
            }
        } catch (const std::exception& e) {
            g_log.Line("frame thread stopped: %s", e.what());
        } catch (...) {
            g_log.Line("frame thread stopped by an unknown error");
        }
        g_stop = true;
        CoopNx2navi::Overlay::Shutdown();
        CoopNx2navi::Puppet::SetFrameWork(nullptr, nullptr);  // waits for a frame of ours on the game's thread
        CoopNx2navi::Puppet::Shutdown();
        CoopNx2navi::Objects::Shutdown();
        CoopNx2navi::Probe::Shutdown();
        session_->wake();
        network.join();
        // Says goodbye to the server. Only reached when the DLL is stopped by
        // hand (coop_nx2navi_stop): a game that exits just ends our threads,
        // and the server lets the player go after its timeout.
        session_->with_client([](coop::net::Client& client) { client.stop(); });
        g_state = 0;
        g_log.Line("stopped; %llu frames, %llu with the player, %llu messages sent",
                   static_cast<unsigned long long>(frames_),
                   static_cast<unsigned long long>(frames_with_player_),
                   static_cast<unsigned long long>(sent_));
    }

private:
    // --- Is the game there? ---

    // Looks for main at NX2navi's base: every second at first, then every
    // five. Once the right build is seen it stays trusted; another build is
    // believed after three looks in a row (a module still loading can show
    // half a note).
    void FindGame(Clock::time_point now) {
        if (trusted_ || refused_ || now < next_look_) {
            return;
        }
        ++looks_;
        next_look_ = now + (looks_ < 120 ? std::chrono::seconds(1) : std::chrono::seconds(5));
        // A game with the plugin interface says what it is: nothing of its
        // memory is looked at from this thread (the scan below asks Windows
        // about every page of main and copies it through the kernel).
        if (const Nx2PluginApi* api = CoopNx2navi::Overlay::Api()) {
            const Nx2GameInfo* info = api->game_info();
            const std::string_view told =
                info != nullptr && info->build_id != nullptr ? info->build_id : "";
            if (info != nullptr && info->main_base != 0 && told.size() >= 16) {
                const std::string_view want = CoopGameBuild::kPrimary.main_id;
                const std::size_t n = std::min(told.size(), want.size());
                bool same = true;
                for (std::size_t i = 0; i < n; ++i) {
                    same = same && std::toupper(static_cast<unsigned char>(told[i])) == want[i];
                }
                if (same) {
                    std::lock_guard lock(frame_mutex_);
                    main_base_ = info->main_base;
                    session_->reset();
                    trusted_ = true;
                    g_log.Line("game found: main at %#llx, build id %.*s, version %s (told by the "
                               "game's plugin interface); the player is read on the game's thread",
                               static_cast<unsigned long long>(info->main_base),
                               static_cast<int>(std::min<std::size_t>(told.size(), 40)), told.data(),
                               std::string(CoopGameBuild::kPrimary.version).c_str());
                } else {
                    refused_ = true;
                    g_log.Line("%s", CoopGameBuild::LogRefusal(std::string(told)).c_str());
                }
                return;
            }
        }
        const auto check = CoopNx2navi::CheckMain(memory_.main_base(),
                                                  std::string(CoopGameBuild::kPrimary.main_id));
        using State = CoopNx2navi::MainCheck::State;
        if (check.state == State::ok) {
            std::lock_guard lock(frame_mutex_);
            main_base_ = memory_.main_base();
            session_->reset();
            trusted_ = true;
            g_log.Line("game found: main at %#llx (%llu MiB mapped), build id %s, version %s; "
                       "reading the player",
                       static_cast<unsigned long long>(memory_.main_base()),
                       static_cast<unsigned long long>(check.size >> 20), check.build_id.c_str(),
                       std::string(CoopGameBuild::kPrimary.version).c_str());
            AskForInterface();
            return;
        }
        if (check.state == State::other_build) {
            if (++other_builds_ >= 3) {
                refused_ = true;
                g_log.Line("%s", CoopGameBuild::LogRefusal(check.build_id).c_str());
            }
            return;
        }
        other_builds_ = 0;
        if (check.state != last_state_ || looks_ == 30) {
            last_state_ = check.state;
            const char* what = check.state == State::not_mapped
                                   ? "nothing is mapped there (the game has not loaded yet, or "
                                     "this is not NX2navi)"
                                   : check.state == State::no_header
                                         ? "memory without a module header (still loading)"
                                         : "a module with no build id in it";
            g_log.Line("game not found yet: main is expected at %#llx, %s; nothing is read or "
                       "sent about the player until it is found",
                       static_cast<unsigned long long>(memory_.main_base()), what);
        }
    }

    // The game is mapped, so it has set itself up: asking its executable for
    // the plugin interface is now allowed (NX2navi docs/plugins.md: not from
    // DllMain, any time once the game runs). Not needed when the game already
    // called nx2navi_plugin_init.
    void AskForInterface() {
        if (CoopNx2navi::Overlay::HaveApi()) {
            return;
        }
        const int asked = CoopNx2navi::Overlay::AskGame();
        if (asked == 0) {
            g_log.Line("overlay: this game has no plugin interface (no %s export: an NX2navi "
                       "build from before it); co-op runs without the in-game panel",
                       NX2_PLUGIN_API_EXPORT);
        } else if (asked < 0) {
            g_log.Line("overlay: the game's plugin interface does not serve version %u; no "
                       "in-game panel",
                       NX2_PLUGIN_API_VERSION);
        }
    }

    // --- The overlay's side of a frame ---

    // Sends what was typed into the chat box (the client's send_chat checks
    // the protocol's rules: 1 to 200 characters, no control characters).
    void SendChat() {
        for (const std::string& text : CoopNx2navi::Overlay::TakeOutbox()) {
            // "/teleport <name>" is a command, done here and not sent.
            if (CoopNx2navi::Teleport::OnChat(text, *session_)) {
                continue;
            }
            const bool sent = session_->with_client(
                [&](coop::net::Client& client) { return client.send_chat(text); });
            if (sent) {
                // The server does not echo chat back: shown here.
                const std::string line = settings_.coop.name + ": " + text;
                g_log.Line("chat: %s", line.c_str());
                CoopNx2navi::Overlay::Notice(line, true);
            } else {
                CoopNx2navi::Overlay::Notice("(not sent: not connected, or the text was refused)");
            }
        }
    }

    void PublishOverlay(const std::optional<coop::host::PlayerReading>& reading,
                        const coop::host::FrameReport& report) {
        namespace Ui = CoopNx2navi::Ui;
        Ui::Snapshot s;
        s.link = static_cast<Ui::Link>(link_.load());
        s.server = settings_.coop.server + ":" + std::to_string(settings_.coop.port);
        s.self_name = settings_.coop.name;
        {
            std::lock_guard lock(names_mutex_);
            s.refused = refused_reason_;
        }
        s.local.game_found = trusted_;
        s.local.in_game = reading.has_value();
        if (reading) {
            s.local.scene = reading->scene;
            s.local.position = reading->position;
            s.local.event = reading->event_running.value_or(false);
            s.local.child = reading->age == coop::protocol::Age::child;
        }
        struct Room {
            std::uint8_t self = 0;
            std::vector<coop::net::Player> players;
        };
        Room room = session_->with_client([](coop::net::Client& client) {
            return Room{client.player_number(), client.players()};
        });
        s.self = room.self;
        if (!report.connected) {
            room.players.clear();
        }
        const std::uint64_t now = report.server_now_us;
        session_->with_presence([&](coop::presence::Presence& presence) {
            for (const auto& player : room.players) {
                if (player.self) {
                    continue;
                }
                Ui::Peer peer;
                peer.number = player.number;
                peer.name = player.nickname;
                if (const auto* p = presence.player(player.number)) {
                    peer.known = true;
                    peer.scene = p->scene();
                    if (!p->samples().empty()) {
                        const std::uint64_t last = p->samples().back().time.server_time_us;
                        peer.fresh = now >= last ? now - last <= Ui::kStaleUs : true;
                    }
                    if (const auto pose = presence.shown(player.number, now)) {
                        peer.position = pose->position;
                    }
                }
                s.peers.push_back(std::move(peer));
            }
        });
        CoopNx2navi::Overlay::Publish(std::move(s));
    }

    // The puppets' side of a frame: what each other player's Link should show
    // (the game's presenting thread acts on it in its frame callback).
    void PublishPuppets(const std::optional<coop::host::PlayerReading>& reading,
                        const coop::host::FrameReport& report) {
        if (!CoopNx2navi::Puppet::Register()) {
            if (!said_no_puppet_ && !CoopNx2navi::Overlay::HaveApi()) {
                said_no_puppet_ = true;
                g_log.Line("puppet: this game has no plugin interface; the other players are "
                           "not shown and nobody can be teleported");
            }
            return;
        }
        // Our own event hides the others; playing the Ocarina does not
        // (coop/host/scene.hpp, local_event_hides), as in the emulator hook.
        const bool event =
            reading && coop::host::local_event_hides(
                           reading->event_running.value_or(false),
                           reading->animation
                               ? std::optional<std::string_view>(reading->animation->name)
                               : std::nullopt);
        std::optional<std::array<float, 3>> position;
        if (reading) {
            position = reading->position;
        }
        CoopNx2navi::Puppet::Publish(reading ? main_base_ : 0, reading.has_value(),
                                     reading ? reading->scene : std::nullopt, position, event,
                                     report.connected ? session_.get() : nullptr,
                                     report.server_now_us);
    }

    // --- One frame ---

    // Our own thread, 60 times a second: looks for the game, serves the save
    // transfer, and makes the frame itself only when the game's thread does not
    // (a game without the plugin interface: from outside, guest_memory.hpp; a
    // game with it that makes no frame right now: with no reading, so the
    // session stays alive and the others see "not in game").
    void Tick() {
        const auto now = Clock::now();
        FindGame(now);
        EnsureTitleEntry();
        CoopNx2navi::SaveSync::Frame(*session_, connected_.load(), SaveDir());
        if (SteadyUs() - game_frame_us_.load() < kGameThreadQuietUs) {
            return;  // the game's thread makes the frames (GameThreadFrame)
        }
        std::lock_guard lock(frame_mutex_);
        // An interface that gives no callback on the game's thread (none free,
        // or a table too old for the game calls): after five seconds of that
        // the player is read from outside after all, as without the interface,
        // so co-op does not go blind. Said once.
        if (trusted_ && CoopNx2navi::Overlay::HaveApi() && !CoopNx2navi::Puppet::Active() &&
            reads_ == Reads::game) {
            if (no_callback_us_ == 0) {
                no_callback_us_ = SteadyUs();
            } else if (SteadyUs() - no_callback_us_ > 5'000'000) {
                reads_ = Reads::thread;
                g_log.Line("reads: the game's plugin interface gave no callback on the game's thread; the "
                           "player is read from this DLL's own thread, from outside (slow in a game "
                           "whose renderer tracks its pages)");
            }
        } else {
            no_callback_us_ = 0;
        }
        if (CoopNx2navi::Overlay::HaveApi() && reads_ != Reads::thread) {
            Frame(nullptr, nullptr);
        } else {
            memory_.Fresh();
            Frame(reads_ == Reads::off ? nullptr : &memory_, &memory_);
        }
    }

    // The game's thread, once a game frame, first thing in the game-thread
    // callback (puppet.cpp): the frame with the game's memory.
    static void GameThreadFrame(CoopPuppet::Guest& guest, std::uint64_t main_base, void* user) {
        auto* self = static_cast<Coop*>(user);
        if (main_base == 0 || self->reads_ != Reads::game) {
            return;
        }
        std::lock_guard lock(self->frame_mutex_);
        if (!self->trusted_) {
            return;
        }
        self->game_frame_us_ = SteadyUs();
        GuestView memory(guest, main_base);
        self->Frame(&memory, nullptr);
    }

    // One frame, under frame_mutex_. `memory`: the game's memory as this
    // thread may reach it, or null (nothing is read: no reading this frame);
    // `cache`: its page cache when it is our own thread's.
    void Frame(coop::host::WritableGameMemory* memory, CoopNx2navi::Memory* cache) {
        const auto now = Clock::now();
        std::optional<coop::host::PlayerReading> reading;
        std::optional<FreshWrites> store;
        coop::host::WritableGameMemory* game = nullptr;  // the game's memory this frame, when trusted
        if (trusted_ && memory != nullptr) {
            coop::host::PlayerReader reader(*memory, &drawn_);
            session_->set_scene_readable(reader.read_scene().has_value());
            reading = reader.read();
            WatchLife(*memory, reader, reading.has_value());
            game = memory;
        } else {
            session_->set_scene_readable(false);
        }
        // The stores the hook puts between the session and the game's flags
        // (items merged, quests ordered, room gimmicks held): progress.cpp.
        if (coop::host::FlagStore* flags = progress_.Before(game, *session_, reading)) {
            store.emplace(*flags, cache);
        }
        const auto report = session_->frame(reading, store ? &*store : nullptr);
        progress_.After(game, *session_, report, reading);
        connected_ = report.connected;
        queued_ += report.queued;
        sent_ += report.queued;
        ++frames_;
        if (reading) {
            ++frames_with_player_;
        }

        if (reading.has_value() != had_reading_ ||
            (reading && reading->scene != scene_ && reading->scene)) {
            had_reading_ = reading.has_value();
            if (reading) {
                scene_ = reading->scene;
                g_log.Line("player found in %s at (%.1f, %.1f, %.1f)",
                           scene_ ? scene_->c_str() : "an unknown scene",
                           static_cast<double>(reading->position[0]),
                           static_cast<double>(reading->position[1]),
                           static_cast<double>(reading->position[2]));
            } else if (trusted_) {
                g_log.Line("player not readable (a load, the title screen or a menu)");
            }
        }
        if (debug_position_ && reading && now >= next_position_) {
            // For tests (OOT_COOP_DEBUG_POS=1): where our Link is, once a second.
            next_position_ = now + std::chrono::seconds(1);
            // The facing: where the rotation takes the actor's +Z, on the ground.
            double fx = 0.0, fz = 0.0;
            if (reading->rotation) {
                const auto& q = *reading->rotation;
                fx = 2.0 * (static_cast<double>(q[0]) * q[2] + static_cast<double>(q[3]) * q[1]);
                fz = 1.0 - 2.0 * (static_cast<double>(q[0]) * q[0] + static_cast<double>(q[1]) * q[1]);
            }
            g_log.Line("pos: %s %.1f %.1f %.1f%s, life %d, facing %.2f %.2f",
                       reading->scene ? reading->scene->c_str() : "?",
                       static_cast<double>(reading->position[0]),
                       static_cast<double>(reading->position[1]),
                       static_cast<double>(reading->position[2]),
                       reading->event_running.value_or(false) ? " (event)" : "", life_, fx, fz);
        }
        if (report.holding && !said_holding_) {
            said_holding_ = true;
            g_log.Line("flags: the host's table is coming in parts; this port cannot hold the "
                       "game meanwhile, so wait a moment before you play on");
        }
        if (!report.received.empty()) {
            const auto now_ms = static_cast<std::uint64_t>(SteadyUs() / 1000);
            for (const std::string& line : notifier_.Take(report.received, self_.load(), now_ms)) {
                g_log.Line("notice: %s", line.c_str());
                CoopNx2navi::Overlay::Notice(line);  // "Saria got the Hookshot", "opened a chest"
            }
        }
        if (report.host_reset) {
            g_log.Line("flags: the host loaded a save; this game follows it");
            CoopNx2navi::Objects::OnHostReset();
        }
        if (report.connected && now >= next_status_) {
            next_status_ = now + kStatusEvery;
            Status(reading, report.server_now_us);
        }
        if (trusted_) {
            CoopNx2navi::Probe::Frame(CoopNx2navi::Overlay::Api(), main_base_, LogLine);  // research only
            PublishPuppets(reading, report);
            CoopNx2navi::Teleport::Frame(reading, *session_, report.connected, report.server_now_us,
                                         CoopNx2navi::Puppet::Active());
            // After PublishPuppets: the puppets' frame step hook is registered
            // first and runs first, as the director is the cave's first caller.
            CoopNx2navi::Objects::FrameInput objects;
            objects.api = CoopNx2navi::Overlay::Api();
            objects.main_base = main_base_;
            objects.session = report.connected ? session_.get() : nullptr;
            objects.scene = reading ? reading->scene : std::nullopt;
            if (reading) {
                objects.position = reading->position;
            }
            objects.event_running = reading && reading->event_running.value_or(false);
            objects.server_now_us = report.server_now_us;
            if (objects_near_log_ && reading && reading->scene) {
                // For the near log: the other players of our scene, as shown.
                const std::vector<coop::net::Player> players =
                    session_->with_client([](coop::net::Client& client) { return client.players(); });
                session_->with_presence([&](coop::presence::Presence& presence) {
                    for (const auto& player : players) {
                        const auto* p = player.self ? nullptr : presence.player(player.number);
                        if (p == nullptr || !p->scene() || *p->scene() != *reading->scene) {
                            continue;
                        }
                        if (const auto pose = presence.shown(player.number, report.server_now_us)) {
                            objects.peers.push_back({player.number,
                                                     {pose->position[0], pose->position[1], pose->position[2]}});
                        }
                    }
                });
            }
            // A frame in which the player merely could not be read (a hitch of
            // the game's thread, one failed reading) is not a scene change:
            // objects/scene_gate.hpp.
            const bool pass = objects_gate_.Pass(memory != nullptr, reading ? reading->scene : std::nullopt,
                                                 SteadyUs(), objects.scene);
            if (pass != objects_passed_ && settings_.objects) {
                g_log.Line(pass ? "objects: the player is read again; sharing goes on as it was"
                                : "objects: the player was not readable this frame (a hitch, not a load "
                                  "yet): what is shared is kept as it is");
            }
            objects_passed_ = pass;
            if (pass) {
                CoopNx2navi::Objects::Frame(objects);
            }
        }
        for (const std::string& line : CoopNx2navi::Teleport::TakeLines()) {
            CoopNx2navi::Overlay::Notice(line);
        }
        if (CoopNx2navi::Overlay::Active()) {
            SendChat();
            PublishOverlay(reading, report);
        }
    }

    // Our Link's life (quarter hearts), from its Life component as the shared
    // enemies read an actor's (lichii/hook/coop_objects.cpp: [[actor + 0x270] +
    // 0x2b8], the value at [life + 0x1c10] + 8, the most at [life + 0x1c18] + 8).
    // At none left the game shows Game Over and reloads: the follow rules then
    // bring us back to the player we follow (teleport_auto.hpp, Died).
    void WatchLife(coop::host::WritableGameMemory& memory, coop::host::PlayerReader& reader,
                   bool in_game) {
        if (!in_game) {
            return;
        }
        const auto actor = reader.find_player_actor();
        const auto table = actor ? coop::host::read_ptr(memory, *actor + 0x270) : std::nullopt;
        const auto life = table ? coop::host::read_ptr(memory, *table + 0x2b8) : std::nullopt;
        const auto value = life ? coop::host::read_ptr(memory, *life + 0x1c10) : std::nullopt;
        const auto most = life ? coop::host::read_ptr(memory, *life + 0x1c18) : std::nullopt;
        const auto now = value ? coop::host::read_u32(memory, *value + 8) : std::nullopt;
        const auto max = most ? coop::host::read_u32(memory, *most + 8) : std::nullopt;
        if (!now || !max || static_cast<std::int32_t>(*max) <= 0 ||
            static_cast<std::int32_t>(*max) > 4 * 40) {
            life_ = -1;
            return;
        }
        life_ = static_cast<std::int32_t>(*now);
        if (!test_life_.empty()) {
            const std::uint64_t at = SteadyUs();
            if (in_game_us_ == 0) {
                in_game_us_ = at;
            }
            if (at - in_game_us_ >= test_life_.front().after_us) {
                const std::uint32_t to = static_cast<std::uint32_t>(test_life_.front().value);
                std::uint8_t bytes[4];
                std::memcpy(bytes, &to, 4);
                const bool ok = memory.write(*value + 8, bytes);
                g_log.Line("test: our Link's life %d -> %u (%s)", life_, to, ok ? "written" : "NOT written");
                test_life_.erase(test_life_.begin());
            }
        }
        if (life_ <= 0 && !dead_) {
            dead_ = true;
            g_log.Line("our Link has no life left (Game Over); after the reload this game goes "
                       "back to the player it follows");
            CoopNx2navi::Teleport::Died();
        } else if (life_ > 0) {
            dead_ = false;
        }
    }

    // The game's committed save folder, once the game tells it (interface
    // version 2; "" before the game opened its storage).
    const std::filesystem::path& SaveDir() {
        if (save_dir_.empty()) {
            const Nx2PluginApi* api = CoopNx2navi::Overlay::Api();
            const Nx2GameInfo* info = api != nullptr ? api->game_info() : nullptr;
            if (info != nullptr && NX2_HAS(info, Nx2GameInfo, save_dir) && info->save_dir != nullptr &&
                *info->save_dir != 0) {
                save_dir_ = std::filesystem::path(
                    std::u8string(reinterpret_cast<const char8_t*>(info->save_dir)));
                g_log.Line("save: the game's save folder is %s", info->save_dir);
            }
        }
        return save_dir_;
    }

    // One line every ten seconds while connected: who is in, and where.
    void Status(const std::optional<coop::host::PlayerReading>& reading, std::uint64_t server_now) {
        struct Room {
            std::uint8_t self = 0;
            std::optional<std::uint64_t> round_trip_us;
            std::vector<coop::net::Player> players;
            bool host = false;
        };
        const Room room = session_->with_client([](coop::net::Client& client) {
            return Room{client.player_number(), client.round_trip_us(), client.players(),
                        client.is_host()};
        });
        std::string line = "status: player " + std::to_string(room.self) + " of " +
                           std::to_string(room.players.size());
        if (room.host) {
            line += " (host)";
        }
        if (room.round_trip_us) {
            line += ", ping " + std::to_string(*room.round_trip_us / 1000) + " ms";
        }
        line += reading ? ", you: " + (reading->scene ? *reading->scene : std::string("unknown scene"))
                        : std::string(trusted_ ? ", you: not in the game" : ", game not found");
        line += ", " + std::to_string(queued_) + " messages sent since the last status";
        queued_ = 0;
        for (const auto& player : room.players) {
            if (player.self) {
                continue;
            }
            struct Seen {
                bool known = false;
                std::optional<std::string> scene;
                std::optional<coop::presence::Pose> pose;
            };
            const Seen seen = session_->with_presence([&](coop::presence::Presence& presence) {
                Seen s;
                if (const auto* p = presence.player(player.number)) {
                    s.known = true;
                    s.scene = p->scene();
                    s.pose = presence.shown(player.number, server_now);
                }
                return s;
            });
            line += "; P" + std::to_string(player.number) + " " + player.nickname + ": ";
            if (!seen.known) {
                line += "joining";
            } else if (!seen.pose) {
                line += seen.scene ? *seen.scene + ", no position" : "no samples";
            } else {
                line += seen.scene ? *seen.scene : std::string("unknown scene");
                if (reading && reading->scene && seen.scene && *reading->scene == *seen.scene) {
                    const double dx = static_cast<double>(seen.pose->position[0] - reading->position[0]);
                    const double dy = static_cast<double>(seen.pose->position[1] - reading->position[1]);
                    const double dz = static_cast<double>(seen.pose->position[2] - reading->position[2]);
                    char buffer[48];
                    std::snprintf(buffer, sizeof(buffer), ", %.1f units away",
                                  std::sqrt(dx * dx + dy * dy + dz * dz));
                    line += buffer;
                }
            }
        }
        line += CoopNx2navi::Puppet::StatusText();
        line += CoopNx2navi::Objects::StatusText();
        line += progress_.StatusText();
        g_log.Line("%s", line.c_str());
    }

    // --- What the client saw. Network thread, under the session's lock. ---

    void OnEvent(const coop::net::Event& event) {
        using coop::net::EventKind;
        switch (event.kind) {
        case EventKind::welcomed:
        case EventKind::reconnected: {
            {
                std::lock_guard lock(names_mutex_);
                names_ = {};
                for (const auto& p : event.welcome.players) {
                    if (p.number < names_.size()) {
                        names_[p.number] = p.nickname;
                    }
                }
            }
            self_ = event.welcome.player_number;
            g_state = 2;
            link_ = static_cast<int>(CoopNx2navi::Ui::Link::connected);
            CoopNx2navi::Overlay::Notice(
                (event.kind == EventKind::reconnected ? "Connected again as P" : "Connected as P") +
                std::to_string(event.welcome.player_number));
            // The line the launcher's menu looks for ("coop: welcomed as player").
            g_log.Line("welcomed as player %u on %s:%u (%llu already in)",
                       static_cast<unsigned>(event.welcome.player_number),
                       settings_.coop.server.c_str(), static_cast<unsigned>(settings_.coop.port),
                       static_cast<unsigned long long>(event.welcome.players.size()));
            // The title menu's page, when it is what started this (it then closes).
            CoopNx2navi::TitleMenu::SetPhase(Hosting() ? CoopNx2navi::Menu::Phase::hosting
                                                       : CoopNx2navi::Menu::Phase::joined,
                                             welcome_, event.welcome.player_number);
            break;
        }
        case EventKind::refused: {
            g_state = 3;
            const std::string reason(coop::net::refuse_reason_text(event.refuse_reason));
            {
                std::lock_guard lock(names_mutex_);
                refused_reason_ = reason;
            }
            link_ = static_cast<int>(CoopNx2navi::Ui::Link::refused);
            CoopNx2navi::Overlay::Notice("Refused by the server: " + reason);
            // The line the launcher's menu looks for ("coop: server refused: ...").
            g_log.Line("server refused: %s", reason.c_str());
            g_log.Line("co-op stays off until the game is started again from the Multiplayer menu");
            CoopNx2navi::TitleMenu::SetPhase(
                CoopNx2navi::Menu::Phase::failed,
                std::string(CoopNx2navi::Menu::T(CoopNx2navi::TitleMenu::Language(),
                                                 CoopNx2navi::Menu::Text::refused)) +
                    ":\n" + reason);
            break;
        }
        case EventKind::connect_failed:
            if (++failed_ == 1 || failed_ % 20 == 0) {
                g_log.Line("no answer from %s:%u yet (attempt %u); still trying",
                           settings_.coop.server.c_str(),
                           static_cast<unsigned>(settings_.coop.port), failed_);
            }
            break;
        case EventKind::disconnected:
            g_state = 1;
            failed_ = 0;
            link_ = static_cast<int>(CoopNx2navi::Ui::Link::lost);
            CoopNx2navi::Overlay::Notice("Server lost; retrying");
            g_log.Line("connection lost; reconnecting (nothing from the server for %u ms, round "
                       "trip %u ms, loss %u%%)",
                       event.link.quiet_ms, event.link.round_trip_ms, event.link.loss_percent);
            break;
        case EventKind::player_joined: {
            std::lock_guard lock(names_mutex_);
            if (event.player < names_.size()) {
                names_[event.player] = event.nickname;
            }
            g_log.Line("player %u '%s' joined", static_cast<unsigned>(event.player),
                       event.nickname.c_str());
            CoopNx2navi::Overlay::Notice(event.nickname + " joined");
            break;
        }
        case EventKind::player_left:
            g_log.Line("player %u '%s' left", static_cast<unsigned>(event.player),
                       event.nickname.c_str());
            CoopNx2navi::Overlay::Notice(event.nickname + " left");
            break;
        case EventKind::chat: {
            const std::string line = Name(event.player) + ": " + event.text;
            g_log.Line("chat: %s", line.c_str());
            CoopNx2navi::Overlay::Notice(line, true);
            break;
        }
        case EventKind::host_changed:
            g_log.Line("player %u hosts the shared progress", static_cast<unsigned>(event.player));
            break;
        default:
            break;
        }
    }

    std::string Name(std::uint8_t number) {
        std::lock_guard lock(names_mutex_);
        if (number < names_.size() && !names_[number].empty()) {
            return names_[number];
        }
        return "P" + std::to_string(number);
    }

    // The server is on this PC and nobody's save is taken: we host.
    bool Hosting() const {
        const std::string& server = settings_.coop.server;
        return !settings_.host_save &&
               (server.rfind("127.", 0) == 0 || server == "localhost" || server == "::1");
    }

    CoopNx2navi::Settings settings_;
    std::string welcome_;
    std::unique_ptr<coop::net::Client> client_;
    std::unique_ptr<coop::host::Session> session_;

    // Which thread reads the game's memory. `game`: the game's own, in the
    // game-thread callback (a game with the plugin interface: always, unless a
    // test says otherwise); `thread`: our own, from outside (a game without
    // the interface; OOT_COOP_READS=thread forces it, to measure what it
    // costs); `off`: nobody (OOT_COOP_READS=off, to measure the rest).
    enum class Reads { game, thread, off };
    std::atomic<Reads> reads_{Reads::game};
    CoopNx2navi::Objects::SceneGate objects_gate_;  // under frame_mutex_
    bool objects_passed_ = true;
    bool objects_near_log_ = false;  // OOT_COOP_DEBUG_NEAR=1
    // Our own thread makes the frame when the game's thread made none for this long.
    static constexpr std::uint64_t kGameThreadQuietUs = 1'000'000;
    std::atomic<std::uint64_t> game_frame_us_{0};  // the last frame on the game's thread
    std::uint64_t no_callback_us_ = 0;             // since when the interface gives no game-thread callback
    std::atomic<bool> connected_{false};           // the last frame's report
    std::atomic<bool> trusted_{false};
    std::uint64_t main_base_ = CoopNx2navi::kMainBase;

    // A frame runs on one thread at a time (ours or the game's): everything
    // below down to the network thread's part is used under this.
    std::mutex frame_mutex_;
    CoopNx2navi::Memory memory_;
    coop::host::PlayerReader::DrawnState drawn_{};
    CoopNx2navi::Progress progress_;
    coop::host::Notifier notifier_;
    bool refused_ = false;
    unsigned looks_ = 0;
    unsigned other_builds_ = 0;
    CoopNx2navi::MainCheck::State last_state_ = CoopNx2navi::MainCheck::State::ok;
    Clock::time_point next_look_{};
    Clock::time_point next_status_{};
    bool had_reading_ = false;
    bool said_holding_ = false;
    bool said_no_puppet_ = false;
    bool pose_copy_ = false;  // set before the threads start
    bool debug_position_ = false;
    Clock::time_point next_position_{};
    std::int32_t life_ = -1;  // our Link's, quarter hearts; -1 not read
    struct TestLife {
        std::uint64_t after_us = 0;
        int value = 0;
    };
    std::vector<TestLife> test_life_;  // OOT_COOP_TEST_LIFE
    std::uint64_t in_game_us_ = 0;     // when our Link's life was first read
    bool dead_ = false;
    std::filesystem::path save_dir_;
    std::optional<std::string> scene_;
    std::size_t queued_ = 0;  // since the last status line
    std::uint64_t sent_ = 0;
    std::uint64_t frames_ = 0;
    std::uint64_t frames_with_player_ = 0;

    // Network thread, read by the frame thread.
    std::atomic<std::uint8_t> self_{0};
    std::mutex names_mutex_;
    std::array<std::string, 5> names_;
    std::string refused_reason_;  // under names_mutex_
    std::atomic<int> link_{static_cast<int>(CoopNx2navi::Ui::Link::connecting)};
    unsigned failed_ = 0;
};

#endif  // COOP_UPSTREAM

HMODULE ThisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModule), &module);
    return module;
}

void OpenLog(HMODULE module) {
    static std::once_flag once;
    std::call_once(once, [module] {
        const std::filesystem::path folder = ModFolder(module);
        g_log.Open(folder / L"coop-nx2navi.log");
        g_log.Line("%s in %s (process %s, id %lu)", kBuildId, Utf8(folder.wstring()).c_str(),
                   Utf8(ModulePath(nullptr).filename().wstring()).c_str(), GetCurrentProcessId());
        CoopNx2navi::Overlay::SetLog(LogLine);
        CoopNx2navi::SaveSync::SetLog(LogLine);
    });
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// This process's environment with `extra` set (an empty value takes the
// variable out), as the block CreateProcessW takes.
using EnvPairs = std::vector<std::pair<std::wstring, std::wstring>>;
std::vector<wchar_t> EnvironmentWith(const EnvPairs& extra) {
    std::vector<wchar_t> block;
    if (wchar_t* all = GetEnvironmentStringsW()) {
        for (const wchar_t* p = all; *p != 0; p += std::wcslen(p) + 1) {
            const std::wstring entry(p);
            const auto eq = entry.find(L'=', 1);  // "=C:=..." entries start with '='
            const std::wstring name = entry.substr(0, eq);
            bool replaced = false;
            for (const auto& [key, value] : extra) {
                replaced = replaced || _wcsicmp(key.c_str(), name.c_str()) == 0;
            }
            if (!replaced) {
                block.insert(block.end(), entry.begin(), entry.end());
                block.push_back(0);
            }
        }
        FreeEnvironmentStringsW(all);
    }
    for (const auto& [key, value] : extra) {
        if (value.empty()) {
            continue;
        }
        const std::wstring entry = key + L"=" + value;
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(0);
    }
    block.push_back(0);
    return block;
}

// --- The co-op server this game started (Host in the title menu) ---

std::mutex g_server_mutex;
HANDLE g_server_process = nullptr;
HANDLE g_server_job = nullptr;

// The running processes whose program is `exe`.
std::vector<DWORD> ProcessesOf(const std::filesystem::path& exe) {
    std::vector<DWORD> found;
    std::error_code ec;
    const std::filesystem::path want = std::filesystem::weakly_canonical(exe, ec);
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return found;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, exe.filename().c_str()) != 0) {
            continue;
        }
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (process == nullptr) {
            continue;
        }
        std::wstring path(32768, L'\0');
        DWORD size = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
            path.resize(size);
            if (std::filesystem::weakly_canonical(path, ec) == want) {
                found.push_back(entry.th32ProcessID);
            }
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return found;
}

enum class ServerStart { started, already_running, missing, failed };

// bin\oot-coop-server.exe --port <port>, with no window (its output goes to
// coop-server.log in the mod folder), the join password in its environment
// only (never on a command line), as the launcher add-on starts it. It ends
// with the game: at the exit callback, and through a job object when the game
// is killed. A server of this mod that already runs (the launcher's) is used
// as it is and left running.
// `this_pc_only` (a session over Steam, steam_net.hpp): it listens on 127.0.0.1
// alone, and a server that already runs is no use (its password is another).
ServerStart StartServer(const std::filesystem::path& folder, std::uint16_t port,
                        const std::string& password, bool this_pc_only = false) {
    std::lock_guard lock(g_server_mutex);
    const std::filesystem::path exe = folder / L"bin" / L"oot-coop-server.exe";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(exe, ec)) {
        g_log.Line("server: %s is missing", Utf8(exe.wstring()).c_str());
        return ServerStart::missing;
    }
    if (g_server_process != nullptr && WaitForSingleObject(g_server_process, 0) == WAIT_TIMEOUT) {
        g_log.Line("server: ours already runs; not starting a second one");
        return ServerStart::already_running;
    }
    if (const auto running = ProcessesOf(exe); !running.empty()) {
        g_log.Line("server: this mod's server already runs (process %lu, started elsewhere): using "
                   "it, and leaving it running when the game closes",
                   running.front());
        return ServerStart::already_running;
    }
    std::wstring command = L"\"" + exe.wstring() + L"\" --port " + std::to_wstring(port);
    if (this_pc_only) {
        command += L" --bind 127.0.0.1";
    }
    EnvPairs extra;
    // Always one: with none set, the open word every joiner without a password sends.
    command += L" --password-env OOT_COOP_SERVER_PASSWORD";
    extra.emplace_back(L"OOT_COOP_SERVER_PASSWORD", Wide(CoopNx2navi::Settings::JoinPassword(password)));
    std::vector<wchar_t> environment = EnvironmentWith(extra);
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const HANDLE log = CreateFileW((folder / L"coop-server.log").c_str(), GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (log != INVALID_HANDLE_VALUE) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdOutput = log;
        startup.hStdError = log;
    }
    PROCESS_INFORMATION info{};
    const std::wstring directory = folder.wstring();
    const BOOL ok = CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr,
                                   log != INVALID_HANDLE_VALUE ? TRUE : FALSE,
                                   CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                   environment.data(), directory.c_str(), &startup, &info);
    if (log != INVALID_HANDLE_VALUE) {
        CloseHandle(log);
    }
    if (!ok) {
        g_log.Line("server: could not be started (Windows error %lu)", GetLastError());
        return ServerStart::failed;
    }
    if (g_server_job == nullptr) {
        g_server_job = CreateJobObjectW(nullptr, nullptr);
        if (g_server_job != nullptr) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(g_server_job, JobObjectExtendedLimitInformation, &limits,
                                    sizeof(limits));
        }
    }
    if (g_server_job != nullptr) {
        AssignProcessToJobObject(g_server_job, info.hProcess);
    }
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    // A server that cannot listen (the port is taken) stops at once.
    if (WaitForSingleObject(info.hProcess, 900) != WAIT_TIMEOUT) {
        DWORD code = 0;
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hProcess);
        g_log.Line("server: it stopped at once (exit code %lu; see coop-server.log): UDP port %u "
                   "may be in use",
                   code, static_cast<unsigned>(port));
        return ServerStart::failed;
    }
    g_server_process = info.hProcess;
    g_log.Line("server: started on UDP port %u (process %lu, no window, join password %s; its "
               "output is in coop-server.log); it ends with the game",
               static_cast<unsigned>(port), info.dwProcessId, password.empty() ? "none" : "set");
    return ServerStart::started;
}

void StopServer() {
    std::lock_guard lock(g_server_mutex);
    if (g_server_process == nullptr) {
        return;
    }
    if (WaitForSingleObject(g_server_process, 0) == WAIT_TIMEOUT) {
        TerminateProcess(g_server_process, 0);
        WaitForSingleObject(g_server_process, 2000);
        g_log.Line("server: stopped with the game");
    }
    CloseHandle(g_server_process);
    g_server_process = nullptr;
}

// --- The game's end, and its restart into the co-op save ---

std::atomic<bool> g_restart{false};          // we start the game again ourselves, in the exit callback
std::atomic<bool> g_restart_by_game{false};  // the runtime does (interface version 4's restart)
EnvPairs g_restart_env;  // set before g_restart
std::atomic<int> g_exit_handle{0};

// Starts the game's program again, as its own Restart does (NX2navi
// runtime/platform/win32.c, plat_restart): the same command line, and
// NX2NAVI_RESTART_WAIT naming this process (id and creation time), so the new
// one waits until this one is gone before it opens its files.
void StartGameAgain() {
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    const unsigned long long since =
        static_cast<unsigned long long>(created.dwHighDateTime) << 32 | created.dwLowDateTime;
    EnvPairs extra = g_restart_env;
    extra.emplace_back(L"NX2NAVI_RESTART_WAIT",
                       std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(since));
    extra.emplace_back(L"NX2NAVI_REEXEC", L"");  // the new process counts its own retries
    std::vector<wchar_t> environment = EnvironmentWith(extra);
    std::wstring command = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                        environment.data(), nullptr, &startup, &info)) {
        g_log.Line("restart: the game could not be started again (Windows error %lu); start it "
                   "from the launcher's Multiplayer row, Join",
                   GetLastError());
        return;
    }
    g_log.Line("restart: the game starts again as process %lu, on the co-op profile", info.dwProcessId);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
}

// The game ends in an orderly way (its window was closed, it restarts, it
// exits): the session says goodbye, our server stops, and the game starts
// again when that is what closed it. On the thread that ends the process.
void OnGameExit(void*) {
    g_log.Line("the game ends%s", g_restart           ? " (to start again on the co-op profile)"
                                  : g_restart_by_game ? " (the game starts itself again on the co-op save)"
                                                      : "");
    {
        std::lock_guard lock(g_wake_mutex);
        g_stop = true;
    }
    g_wake.notify_all();
    if (g_thread != nullptr) {
        WaitForSingleObject(g_thread, 2000);  // the goodbye to the server
    }
    StopServer();
    if (g_restart) {
        StartGameAgain();
    }
}

void HookExit() {
    const Nx2PluginApi* api = CoopNx2navi::Overlay::Api();
    if (api == nullptr || g_exit_handle > 0 || !NX2_HAS(api, Nx2PluginApi, on_exit) ||
        api->on_exit == nullptr) {
        return;
    }
    g_exit_handle = api->on_exit(OnGameExit, nullptr);
}

// The game's window: the visible top-level window of this process.
HWND GameWindow() {
    struct Find {
        DWORD pid;
        HWND window;
    } find{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            auto* f = reinterpret_cast<Find*>(parameter);
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            if (pid == f->pid && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
                f->window = window;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&find));
    return find.window;
}

// Closes the game as its window's close button does; the exit callback then
// starts it again with `extra` in its environment. False when that cannot be
// done here (no exit callback in this game, no window found).
bool RestartGame(EnvPairs extra) {
    HookExit();
    const HWND window = GameWindow();
    const Nx2PluginApi* api = CoopNx2navi::Overlay::Api();
    // For tests: more environment for the restarted game ("NAME=value;NAME=value").
    std::wstring more = Environment(L"OOT_COOP_TEST_RESTART_ENV");
    while (!more.empty()) {
        const auto end = more.find(L';');
        const std::wstring pair = more.substr(0, end);
        more = end == std::wstring::npos ? std::wstring() : more.substr(end + 1);
        if (const auto eq = pair.find(L'='); eq != std::wstring::npos && eq > 0) {
            extra.emplace_back(pair.substr(0, eq), pair.substr(eq + 1));
        }
    }
    // Interface version 4's own restart: the runtime ends the game in an
    // orderly way (exit callbacks, saves flushed) and starts it again itself
    // with these variables. OOT_COOP_RESTART=own keeps the hand-made one below
    // (a test).
    if (api != nullptr && NX2_HAS(api, Nx2PluginApi, restart) && api->restart != nullptr &&
        Environment(L"OOT_COOP_RESTART") != L"own") {
        std::vector<std::string> texts;
        for (const auto& [name, value] : extra) {
            texts.push_back(Utf8(name) + "=" + Utf8(value));
        }
        std::vector<const char*> list;
        for (const std::string& text : texts) {
            list.push_back(text.c_str());
        }
        list.push_back(nullptr);
        g_restart_by_game = true;
        g_log.Line("restart: asking the game to start again on the co-op save (the plugin "
                   "interface's restart, %u variables)",
                   static_cast<unsigned>(texts.size()));
        if (api->restart(list.data()) == 1) {
            return true;
        }
        g_restart_by_game = false;
        g_log.Line("restart: the game refused its own restart; closing its window instead");
    }
    if (g_exit_handle <= 0 || window == nullptr) {
        g_log.Line("restart: not possible here (%s)",
                   g_exit_handle <= 0 ? "this game has no exit callback: plugin interface version 2"
                                      : "the game's window was not found");
        return false;
    }
    g_restart_env = std::move(extra);
    g_restart = true;
    g_log.Line("restart: closing the game to start it again on the co-op profile");
    PostMessageW(window, WM_CLOSE, 0, 0);
    return true;
}

// --- Starting co-op: from coop.ini (the launcher's Host and Join), or from
// the title screen's Multiplayer page ---

// The options of the session from the mod's settings file (the launcher
// add-on's toggles), for a session started inside the game.
CoopNx2navi::Settings SettingsFromFile(const std::filesystem::path& folder) {
    CoopNx2navi::Settings s;
    CoopNx2navi::Menu::FlatJson json;
    if (const auto text = ReadFile(folder / L"settings.json")) {
        json.Parse(*text);
    }
    s.share_progress = json.Bool("share_progress").value_or(true);
    s.overlay = json.Bool("overlay").value_or(true);
    s.puppet = json.Bool("puppet").value_or(true);
    s.join_at_host = json.Bool("join_at_host").value_or(true);
    s.objects = json.Bool("objects").value_or(true);
    const std::string follow = json.String("follow").value_or("host");
    s.follow = follow == "off"      ? CoopNx2navi::Settings::Follow::off
               : follow == "anyone" ? CoopNx2navi::Settings::Follow::anyone
                                    : CoopNx2navi::Settings::Follow::host;
    return s;
}

// coop.ini as the launcher's "Connection" row and a restarted game read it;
// never with the password (it travels in the environment).
bool WriteIni(const std::filesystem::path& path, const CoopNx2navi::Settings& settings) {
    std::FILE* file = _wfsopen(path.c_str(), L"wb", _SH_DENYWR);
    if (file == nullptr) {
        g_log.Line("coop.ini could not be written (%s)", Utf8(path.wstring()).c_str());
        return false;
    }
    const std::string text = CoopNx2navi::SettingsIni(settings, false);
    const bool ok = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    std::fclose(file);
    return ok;
}

std::optional<CoopNx2navi::Settings> ReadIni(const std::filesystem::path& ini) {
    const auto text = ReadFile(ini);
    if (!text) {
        g_log.Line("no coop.ini at %s", Utf8(ini.wstring()).c_str());
        return std::nullopt;
    }
    std::vector<std::string> errors;
    CoopNx2navi::Settings settings = CoopNx2navi::ParseSettings(*text, errors);
    for (const std::string& error : errors) {
        g_log.Line("%s", error.c_str());
    }
    if (!errors.empty()) {
        g_log.Line("coop.ini has a mistake, see above");
        return std::nullopt;
    }
    if (!settings.enabled()) {
        g_log.Line("no server set in coop.ini");
        return std::nullopt;
    }
    // The join password of a game the mod itself restarted.
    if (const std::wstring password = Environment(L"OOT_COOP_PASSWORD"); !password.empty()) {
        settings.coop.password = Utf8(password);
    }
    return settings;
}

// The interface, when the game did not hand it over at its start: asked of the
// executable once the game's main is mapped (never from DllMain).
bool HaveInterface() {
    if (CoopNx2navi::Overlay::HaveApi()) {
        return true;
    }
    const auto check =
        CoopNx2navi::CheckMain(CoopNx2navi::kMainBase, std::string(CoopGameBuild::kPrimary.main_id));
    if (check.state != CoopNx2navi::MainCheck::State::ok) {
        return false;
    }
    CoopNx2navi::Overlay::AskGame();
    return CoopNx2navi::Overlay::HaveApi();
}

std::filesystem::path PathOf(const char* utf8);

// Whether a puppet pack made for this port is in a mod folder the game reads:
// coop_puppet.json saying "pouch": false beside romfs\Pack\Actor\CoopPuppet_*.pack.zs
// (tools/puppet_pack; the launcher add-on makes it from the player's own game
// files, when the launcher starts with the mod enabled or at its Host or Join).
// Looked for in this DLL's own mod folder, then in every folder of the game's
// mods folder. Without one the game has no puppet actor to make.
bool PuppetPackMade(const std::filesystem::path& folder) {
    if (Environment(L"OOT_COOP_ASSUME_PACK") == L"1") {
        return true;  // for tests with a stand-in game
    }
    const auto made = [](const std::filesystem::path& mod) {
        std::error_code ec;
        const auto actors = mod / L"romfs" / L"Pack" / L"Actor";
        if (!std::filesystem::is_regular_file(actors / L"CoopPuppet_Child.pack.zs", ec) ||
            !std::filesystem::is_regular_file(actors / L"CoopPuppet_Adult.pack.zs", ec)) {
            return false;
        }
        CoopNx2navi::Menu::FlatJson json;
        const auto text = ReadFile(mod / L"coop_puppet.json");
        return text && json.Parse(*text) && json.Bool("pouch") == false;
    };
    if (made(folder)) {
        return true;
    }
    const Nx2PluginApi* api = CoopNx2navi::Overlay::Api();
    const Nx2GameInfo* info = api != nullptr ? api->game_info() : nullptr;
    if (info == nullptr || info->mods_dir == nullptr || *info->mods_dir == 0) {
        return false;
    }
    std::error_code ec;
    for (std::filesystem::directory_iterator it(PathOf(info->mods_dir), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->is_directory(ec) && made(it->path())) {
            return true;
        }
    }
    return false;
}

std::filesystem::path PathOf(const char* utf8) {
    return utf8 != nullptr ? std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8)))
                           : std::filesystem::path();
}

// The id of the Steam room this game joined from its Multiplayer page ("" over
// the local network): the game started again for the host's save enters it again.
std::string g_steam_room;

enum class HostSave { restarting, own_save, refused, no_answer };

// A joiner takes the host's save before anything is loaded (save_sync.hpp): it
// is fetched into <mod>\coop-save and the game starts again on that folder.
// `restarting`: the game is closing; `own_save`: no save could be had, the
// session goes on with the player's own; else the server could not be joined
// and `why` says so in the menu's language.
HostSave TakeHostSave(const std::filesystem::path& folder, const CoopNx2navi::Settings& settings,
                      std::string& why) {
    namespace Menu = CoopNx2navi::Menu;
    namespace TitleMenu = CoopNx2navi::TitleMenu;
    const Menu::Lang lang = TitleMenu::Language();
    // The game's own profile and save folder (interface version 2); the save
    // folder is known some seconds after the game's start.
    const Nx2PluginApi* api = nullptr;
    const Nx2GameInfo* info = nullptr;
    for (int tries = 0; tries < 600 && !g_stop; ++tries) {
        if (HaveInterface()) {
            api = CoopNx2navi::Overlay::Api();
            info = api->game_info();
            if (info == nullptr || !NX2_HAS(info, Nx2GameInfo, save_dir)) {
                info = nullptr;
                break;  // interface version 1: no folders
            }
            if (info->save_dir != nullptr && *info->save_dir != 0) {
                break;
            }
        }
        if (SleepUntil(Clock::now() + std::chrono::milliseconds(100))) {
            return HostSave::own_save;
        }
    }
    if (info == nullptr || info->save_dir == nullptr || *info->save_dir == 0) {
        g_log.Line("save: this game does not tell its save folder (plugin interface version 2 "
                   "gives it); the host's save is not taken");
        return HostSave::own_save;
    }
    const std::filesystem::path own_profile = PathOf(info->profile_dir);
    const std::filesystem::path own_save = PathOf(info->save_dir);
    // Where the restarted game finds it: a runtime with interface version 4
    // takes a save folder of its own (NX2NAVI_SAVE_DIR: save, save-tx and
    // save-backup there, the cache and everything else stay the player's
    // profile); an older one only a whole profile (NX2NAVI_PROFILE_DIR).
    const bool save_dir_only = NX2_HAS(api, Nx2PluginApi, world_to_screen);

    CoopNx2navi::SaveSync::FetchOptions options;
    options.server = settings.coop.server;
    options.port = settings.coop.port;
    options.name = settings.coop.name;
    options.password = settings.coop.password;
    options.build_id = kBuildId;
    options.game_build_id = std::string(CoopGameBuild::kPrimary.id);
    options.game_version = std::string(CoopGameBuild::kPrimary.version);
    const auto fetched = CoopNx2navi::SaveSync::Fetch(options, g_stop, [lang](bool) {
        TitleMenu::SetPhase(Menu::Phase::receiving_save, Menu::T(lang, Menu::Text::receiving_save));
    });
    using Result = CoopNx2navi::SaveSync::FetchResult;
    switch (fetched.result) {
    case Result::refused:
        why = std::string(Menu::T(lang, Menu::Text::refused)) + ":\n" + fetched.reason;
        // The lines the launcher's menu looks for.
        g_log.Line("server refused: %s", fetched.reason.c_str());
        return HostSave::refused;
    case Result::no_answer:
        why = Menu::T(lang, Menu::Text::no_answer);
        return HostSave::no_answer;
    case Result::fetched: break;
    case Result::new_game: break;  // the host is beginning a new game on a save with nothing in it yet
    default: return HostSave::own_save;
    }
    const bool fresh = fetched.result == Result::new_game;
    // Kept among the player's multiplayer saves (mp_saves.hpp), under the name
    // of the host's save and the host's own: "<save>@<host>" in coop-saves.
    const std::filesystem::path coop_profile =
        CoopNx2navi::MpSaves::Root(folder) /
        PathOf(CoopNx2navi::MpSaves::ReceivedFolder(fetched.save_name, fetched.host_name).c_str());
    std::string error;
    if (fresh ? !CoopNx2navi::SaveSync::WriteEmptyCoopProfile(own_profile, own_save, coop_profile, error)
              : !CoopNx2navi::SaveSync::WriteCoopProfile(own_profile, own_save, coop_profile,
                                                         *fetched.slot, error)) {
        g_log.Line("save: the host's save was not written: %s", error.c_str());
        why = Menu::T(lang, Menu::Text::save_failed);
        return HostSave::own_save;
    }
    g_log.Line("save: %s the co-op save folder %s (your own saves in "
               "%s are not touched); the game starts again with %s",
               fresh ? "a new game, as the host's, is to begin in" : "the host's save is slot_00 of",
               Utf8(coop_profile.wstring()).c_str(), Utf8(own_save.wstring()).c_str(),
               save_dir_only ? "NX2NAVI_SAVE_DIR" : "NX2NAVI_PROFILE_DIR");
    // The restarted game reads the session from this file, the password from
    // its environment, and knows the save is already in.
    const std::filesystem::path ini = folder / L"coop.ini";
    if (!WriteIni(ini, settings)) {
        return HostSave::own_save;
    }
    TitleMenu::SetPhase(Menu::Phase::restarting,
                        Menu::T(lang, fresh ? Menu::Text::restarting_new : Menu::Text::restarting));
    SleepUntil(Clock::now() + std::chrono::milliseconds(1500));  // the sentence is read
    EnvPairs extra{{save_dir_only ? L"NX2NAVI_SAVE_DIR" : L"NX2NAVI_PROFILE_DIR", coop_profile.wstring()},
                   {L"OOT_COOP_INI", ini.wstring()},
                   {L"OOT_COOP_SAVE_READY", L"1"},
                   {L"OOT_COOP_PASSWORD", Wide(settings.coop.password)}};
    if (!g_steam_room.empty()) {
        // Over Steam: the address and the password above are this process's own
        // (a local port, the room's secret); the next one asks the room again.
        extra.back().second.clear();
        extra.emplace_back(L"OOT_COOP_STEAM_JOIN", Wide(g_steam_room));
    } else if (!Environment(L"OOT_COOP_STEAM_JOIN").empty()) {
        extra.emplace_back(L"OOT_COOP_STEAM_JOIN", L"");  // not the one an earlier start left
    }
    // A joiner's next start never hosts, whatever an earlier start of this game did.
    for (const wchar_t* left : {L"OOT_COOP_STEAM_HOST", L"OOT_COOP_MP_HOST"}) {
        if (!Environment(left).empty()) {
            extra.emplace_back(left, L"");
        }
    }
    if (!RestartGame(std::move(extra))) {
        why = Menu::T(lang, Menu::Text::restart_failed);
        return HostSave::own_save;
    }
    return HostSave::restarting;
}

void EnsureTitleEntry() {
    static bool done = false;  // the DLL's own thread only
    if (done || !CoopNx2navi::Overlay::HaveApi()) {
        return;
    }
    done = true;
    HookExit();
    CoopNx2navi::TitleMenu::Register(CoopNx2navi::Overlay::Api(), g_folder, LogLine);
}

// --- Over Steam (steam_net.hpp): the same session, carried through Steam ---

// Waits for the game's interface (the game hands it over at its start, or
// answers once its main module is mapped). False when the DLL is to stop.
bool WaitInterface() {
    for (int tries = 0; tries < 600 && !HaveInterface(); ++tries) {
        if (SleepUntil(Clock::now() + std::chrono::milliseconds(100))) {
            return false;
        }
    }
    if (!HaveInterface()) {
        return false;
    }
    EnsureTitleEntry();  // the page's language, and steam_net's start
    return true;
}

bool SteamSleep(unsigned ms) {
    return SleepUntil(Clock::now() + std::chrono::milliseconds(ms));
}

void SteamFailed(CoopNx2navi::Menu::SteamFail fail, const char* what) {
    namespace Menu = CoopNx2navi::Menu;
    g_log.Line("steam: %s: not done (reason %d)", what, static_cast<int>(fail));
    CoopNx2navi::TitleMenu::SetPhase(
        Menu::Phase::failed, Menu::SteamPages::WhyFailed(CoopNx2navi::TitleMenu::Language(), fail));
}

// The host: the co-op server on 127.0.0.1 with a join password made for this
// room, a Steam room, and the members' datagrams carried between the two.
// `settings` then leads this game's own client to that server.
bool SteamHost(const std::filesystem::path& folder, CoopNx2navi::Settings& settings) {
    namespace Menu = CoopNx2navi::Menu;
    if (!WaitInterface()) {
        return false;
    }
    CoopNx2navi::TitleMenu::SetPhase(
        Menu::Phase::starting_server, Menu::T(CoopNx2navi::TitleMenu::Language(), Menu::Text::steam_making));
    const auto session = CoopNx2navi::SteamNet::HostSession(
        [&](std::uint16_t port, const std::string& secret) {
            return StartServer(folder, port, secret, true) == ServerStart::started;
        },
        SteamSleep);
    if (!session.ok) {
        StopServer();
        SteamFailed(session.fail, "host over Steam");
        return false;
    }
    settings.coop.server = "127.0.0.1";
    settings.coop.port = session.port;
    settings.coop.password = session.secret;
    settings.host_save = false;
    g_log.Line("steam: hosting over Steam: the room is open to your Steam friends, to invitations and "
               "to its ID (shown on the title screen's Multiplayer page, Host over Steam)");
    return true;
}

// A joiner: into the host's room, and `settings` then leads this game's client
// to the local port whose datagrams go to the host through Steam.
bool SteamJoin(const std::string& room, CoopNx2navi::Settings& settings) {
    namespace Menu = CoopNx2navi::Menu;
    if (!WaitInterface()) {
        return false;
    }
    CoopNx2navi::TitleMenu::SetPhase(
        Menu::Phase::connecting, Menu::T(CoopNx2navi::TitleMenu::Language(), Menu::Text::steam_entering));
    const auto session = CoopNx2navi::SteamNet::JoinSession(room, SteamSleep);
    if (!session.ok) {
        SteamFailed(session.fail, "join over Steam");
        return false;
    }
    settings.coop.server = "127.0.0.1";
    settings.coop.port = session.port;
    settings.coop.password = session.secret;
    g_steam_room = room;
    return true;
}

void Main(HMODULE module) {
    namespace Menu = CoopNx2navi::Menu;
    namespace TitleMenu = CoopNx2navi::TitleMenu;
    const std::filesystem::path folder = ModFolder(module);
    g_folder = folder;
    OpenLog(module);

    // Started for a session: the launcher's Host or Join (and this mod's own
    // restart into the co-op save) name the coop.ini to read. Without it the
    // DLL idles: the game was started with its normal Play.
    std::optional<CoopNx2navi::Settings> settings;
    std::string welcome;
    const std::filesystem::path ini = Environment(L"OOT_COOP_INI");
    const bool save_ready = Environment(L"OOT_COOP_SAVE_READY") == L"1";
    // This game was started again by its own Multiplayer page to host on a
    // multiplayer save (below): the server is ours to start.
    const bool host_on_save = Environment(L"OOT_COOP_MP_HOST") == L"1";
    // ... over Steam; or to join the Steam room whose host's save is in already.
    const bool steam_host = Environment(L"OOT_COOP_STEAM_HOST") == L"1";
    const std::string steam_join = Utf8(Environment(L"OOT_COOP_STEAM_JOIN"));
    if (save_ready) {
        g_log.Line("save: this game was started again for the host's save (save folder %s%s)",
                   Utf8(Environment(L"NX2NAVI_SAVE_DIR")).c_str(),
                   Utf8(Environment(L"NX2NAVI_PROFILE_DIR")).c_str());
    }
    if (!ini.empty()) {
        settings = ReadIni(ini);
        if (!settings) {
            g_log.Line("off for now (see above); the title screen's Multiplayer entry can still "
                       "host or join");
        }
    }
    if (settings && host_on_save) {
        g_log.Line("host: this game was started again to host on a multiplayer save (save folder %s)",
                   Utf8(Environment(L"NX2NAVI_SAVE_DIR")).c_str());
        const ServerStart started =
            steam_host ? (SteamHost(folder, *settings) ? ServerStart::started : ServerStart::failed)
                       : StartServer(folder, settings->coop.port, settings->coop.password);
        if (started == ServerStart::missing || started == ServerStart::failed) {
            g_log.Line("host: no server, so no session; the title screen's Multiplayer entry can try again");
            settings.reset();
        } else {
            if (HaveInterface()) {
                EnsureTitleEntry();  // the page's language, for the sentence below
            }
            welcome = Menu::T(TitleMenu::Language(), Menu::Text::hosting_done);
        }
    }
    if (settings && save_ready && !steam_join.empty() && !SteamJoin(steam_join, *settings)) {
        g_log.Line("join: the host's Steam room was not entered again; the title screen's Multiplayer "
                   "entry can try again");
        settings.reset();
    }
    bool said_idle = false;
    while (true) {
        if (settings && settings->host_save && !save_ready) {
            // A joiner: the host's save first.
            std::string why;
            switch (TakeHostSave(folder, *settings, why)) {
            case HostSave::restarting:
                // The game is closing; nothing more in this process.
                while (!SleepUntil(Clock::now() + std::chrono::seconds(1))) {
                }
                return;
            case HostSave::refused:
            case HostSave::no_answer:
                TitleMenu::SetPhase(Menu::Phase::failed, why);
                g_log.Line("join: not joined; the title screen's Multiplayer entry can try again");
                settings.reset();
                if (!g_steam_room.empty()) {
                    // Over Steam: out of the host's room, so that Join can enter one again.
                    CoopNx2navi::SteamNet::Leave();
                    g_steam_room.clear();
                }
                break;
            case HostSave::own_save:
                welcome = why.empty() ? Menu::T(TitleMenu::Language(), Menu::Text::no_host_save) : why;
                settings->host_save = false;
                break;
            }
            continue;
        }
        if (settings) {
            break;
        }
        if (!said_idle) {
            said_idle = true;
            g_log.Line("idle: no session yet (the game was not started by the launcher's Host or "
                       "Join). Nothing is read, sent or hooked until one is hosted or joined from "
                       "the title screen's Multiplayer entry");
        }
        if (SleepUntil(Clock::now() + std::chrono::milliseconds(TitleMenu::Registered() ? 50 : 250))) {
            return;
        }
        if (HaveInterface()) {
            EnsureTitleEntry();
        }
        const auto request = TitleMenu::TakeRequest();
        if (!request) {
            continue;
        }
        const Menu::Lang lang = TitleMenu::Language();
        CoopNx2navi::Settings chosen = SettingsFromFile(folder);
        chosen.coop.name = request->name;
        chosen.coop.port = request->port;
        chosen.coop.password = request->password;
        if (request->host && !request->save.empty()) {
            // Never on the player's own saves: the game starts again on the
            // chosen multiplayer save (the game opens its save folder once, at
            // its start), and that game starts the server and the session.
            const std::filesystem::path save_dir =
                CoopNx2navi::MpSaves::Root(folder) / PathOf(request->save.c_str());
            const Nx2PluginApi* api = CoopNx2navi::Overlay::Api();
            const Nx2GameInfo* info = api != nullptr ? api->game_info() : nullptr;
            const bool on_it = info != nullptr && NX2_HAS(info, Nx2GameInfo, save_dir) &&
                               CoopNx2navi::MpSaves::FolderOfSaveDir(PathOf(info->save_dir)) == request->save;
            if (!on_it) {
                chosen.coop.server = "127.0.0.1";
                chosen.host_save = false;
                const std::filesystem::path host_ini = folder / L"coop.ini";
                g_log.Line("host: the game starts again on the multiplayer save '%s' (%s); your own "
                           "saves are not used",
                           request->save.c_str(), Utf8(save_dir.wstring()).c_str());
                TitleMenu::SetPhase(Menu::Phase::restarting,
                                    Menu::T(lang, request->save_empty ? Menu::Text::host_restarting_new
                                                                      : Menu::Text::host_restarting));
                SleepUntil(Clock::now() + std::chrono::milliseconds(1500));  // the sentence is read
                EnvPairs next{{L"NX2NAVI_SAVE_DIR", save_dir.wstring()},
                              {L"OOT_COOP_INI", host_ini.wstring()},
                              {L"OOT_COOP_MP_HOST", L"1"},
                              {L"OOT_COOP_PASSWORD", Wide(request->password)}};
                // Over Steam the next start makes the room; over the local network it must
                // not, whatever an earlier start of this game left in the environment.
                if (request->steam || !Environment(L"OOT_COOP_STEAM_HOST").empty()) {
                    next.emplace_back(L"OOT_COOP_STEAM_HOST", request->steam ? L"1" : L"");
                }
                if (!Environment(L"OOT_COOP_STEAM_JOIN").empty()) {
                    next.emplace_back(L"OOT_COOP_STEAM_JOIN", L"");
                }
                if (WriteIni(host_ini, chosen) && RestartGame(std::move(next))) {
                    while (!SleepUntil(Clock::now() + std::chrono::seconds(1))) {
                    }
                    return;  // the game is closing; nothing more in this process
                }
                TitleMenu::SetPhase(Menu::Phase::failed, Menu::T(lang, Menu::Text::host_restart_failed));
                continue;
            }
        }
        if (request->steam) {
            // Over Steam: no address, no port and no password of the player's.
            chosen.coop.password.clear();
            if (request->host ? !SteamHost(folder, chosen) : !SteamJoin(request->room, chosen)) {
                continue;  // the page says why
            }
            chosen.host_save = !request->host;
            welcome = Menu::T(lang, request->host ? Menu::Text::hosting_done : Menu::Text::joined_done);
            if (request->host) {
                TitleMenu::SetPhase(Menu::Phase::connecting, Menu::T(lang, Menu::Text::connecting));
            }
        } else if (request->host) {
            const ServerStart started = StartServer(folder, request->port, request->password);
            if (started == ServerStart::missing || started == ServerStart::failed) {
                TitleMenu::SetPhase(Menu::Phase::failed,
                                    Menu::T(lang, started == ServerStart::missing
                                                      ? Menu::Text::server_missing
                                                      : Menu::Text::server_failed));
                continue;
            }
            chosen.coop.server = "127.0.0.1";
            chosen.host_save = false;
            welcome = Menu::T(lang, Menu::Text::hosting_done);
            TitleMenu::SetPhase(Menu::Phase::connecting, Menu::T(lang, Menu::Text::connecting));
        } else {
            const auto address = Menu::ParseJoinAddress(request->server);
            chosen.coop.server = address ? address->server : request->server;
            chosen.host_save = true;
            welcome = Menu::T(lang, Menu::Text::joined_done);
        }
        WriteIni(folder / L"coop.ini", chosen);
        settings = std::move(chosen);
    }

    // In a session from here on. The title entry stays (its page then says so).
    if (settings->puppet) {
        HaveInterface();  // the game's mods folder, when the game did not hand the interface over
        if (!PuppetPackMade(folder)) {
            settings->puppet = false;
            g_log.Line("puppet: off: no puppet pack in the mod folder yet, so the other players are "
                       "not shown as Link. The launcher makes it from your own game files when it "
                       "starts with the mod enabled (or at its Multiplayer row's Host or Join); "
                       "the game reads it at its next start");
        }
    }
    Coop coop(std::move(*settings), std::move(welcome));
    if (coop.Begin()) {
        coop.Run();
    }
}

DWORD WINAPI Thread(LPVOID parameter) {
    const auto module = static_cast<HMODULE>(parameter);
    try {
        Main(module);
    } catch (const std::exception& e) {
        g_log.Line("stopped by an error: %s", e.what());
    } catch (...) {
        g_log.Line("stopped by an unknown error");
    }
    g_state = 0;
    // The thread holds the DLL (DllMain), so it cannot be unloaded under it;
    // this lets go of it and ends the thread in one step.
    FreeLibraryAndExitThread(module, 0);
}

}  // namespace

extern "C" {

// 0 off or stopped, 1 connecting, 2 welcomed by the server, 3 refused.
__declspec(dllexport) int coop_nx2navi_state() {
    return g_state.load();
}

// Says goodbye to the server and ends the DLL's threads; waits for them up
// to `wait_ms`. After it returns 1 the DLL can be unloaded. The game never
// calls this (it knows nothing of the DLL); it is for tests and tools.
__declspec(dllexport) int coop_nx2navi_stop(unsigned wait_ms) {
    // The game's callbacks first, so none runs into a DLL about to unload.
    CoopNx2navi::Overlay::Shutdown();
#ifndef COOP_UPSTREAM
    CoopNx2navi::Puppet::Shutdown();
#endif
    CoopNx2navi::TitleMenu::Shutdown();
    CoopNx2navi::SteamNet::Shutdown();
    if (const int handle = g_exit_handle.exchange(0); handle > 0) {
        if (const Nx2PluginApi* api = CoopNx2navi::Overlay::Api()) {
            api->remove(handle);
        }
    }
    StopServer();
    {
        std::lock_guard lock(g_wake_mutex);
        g_stop = true;
    }
    g_wake.notify_all();
    if (g_thread == nullptr) {
        return 1;
    }
    return WaitForSingleObject(g_thread, wait_ms) == WAIT_OBJECT_0 ? 1 : 0;
}

// NX2navi's plugin interface (third_party/plugin_api.h): the game calls this
// once for every library NX2NAVI_MOD_DLLS names, on its first thread, before
// its own code runs. Only the table is kept here; the overlay's callbacks are
// registered once co-op runs with overlay = 1 (Coop::Begin). A second call (the
// library named twice) changes nothing.
__declspec(dllexport) void nx2navi_plugin_init(const Nx2PluginApi* api) {
    if (api == nullptr) {
        return;
    }
    OpenLog(ThisModule());
    CoopNx2navi::Overlay::Offer(api, "nx2navi_plugin_init");
}

}  // extern "C"

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Only a thread is started here: nothing heavy runs under the loader
        // lock. The thread gets its own hold on the DLL first.
        HMODULE held = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                               reinterpret_cast<LPCWSTR>(&DllMain), &held) &&
            held != nullptr) {
            g_thread = CreateThread(nullptr, 0, Thread, held, 0, nullptr);
            if (g_thread == nullptr) {
                FreeLibrary(held);
            }
        }
    }
    return TRUE;
}
