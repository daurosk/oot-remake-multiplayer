// SPDX-License-Identifier: GPL-3.0-or-later
//
// Local co-op (split screen): the switchboard between the menu's Local co-op
// page and the game side (docs/local-coop.md). No game or emulator code here.
//
// One world runs; each local player after the first is another Player of the
// game with its own input device and its own view. This file holds who sits
// where and the steps of turning it on and off:
//
//   Off -> Starting (the game side makes each extra player, one frame hook at
//   a time) -> On -> Stopping (it removes them) -> Off.
//
// - The menu (any thread) calls the free functions at the end: Devices,
//   Start, Stop, Get, SetSplit, SetScale, SetDevice. They only record what is
//   wanted; nothing of the game is touched from the menu's thread.
// - The game side implements Game and calls Core::Frame on the game's calc
//   thread at the frame hook. Every Game call but Devices and Supported is
//   made from there.
// - Start answers at once whether the request is acceptable (Why). A failure
//   later (a player the game could not make) ends in Off with Why::Failed,
//   which Get reports until the next Start.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coop_local_layout.h"

namespace CoopLocal {

inline constexpr int kMostPlayers = CoopLocalLayout::kMostViews;
inline constexpr std::size_t kMostNameBytes = 16;   // as the session's player names
// A player the game has not made after this many frame hooks is given up
// (20 s at 30 hooks a second: a pack loads in well under that).
inline constexpr int kStartFrames = 600;

using Split = CoopLocalLayout::Split;

// An input device a local player can take: a gamepad, or the keyboard and
// mouse (one device).
struct Device {
    int slot = -1;          // the game side's number for it, stable while it stays connected
    std::string name;       // as the menu shows it
    bool keyboard = false;
    bool operator==(const Device&) const = default;
};

// One local player. Seat 0 is the first player (the Link the game already has).
struct Seat {
    int device = -1;                       // a Device::slot
    std::string name;                      // empty: "Player <n>"
    std::optional<std::uint32_t> color;    // 0xRRGGBB; unset: the session's colour for the player
    bool operator==(const Seat&) const = default;
};

struct Config {
    std::vector<Seat> seats;     // 2 to kMostPlayers
    Split split = Split::Side;   // for two players
    int scale = 100;             // each view's render size, percent (CoopLocalLayout::ClampScale)
};

enum class State : std::uint8_t { Off, Starting, On, Stopping };

enum class Why : std::uint8_t {
    Ok,
    NotSupported,     // this game build has no local co-op (no game side, or its hooks are missing)
    NotInGame,        // no save is loaded: start it from the pause menu in play
    Busy,             // it is on already, or still starting or stopping
    TooFewPlayers,
    TooManyPlayers,
    NoDevice,         // a seat names no device, or one that is not connected
    DeviceTwice,      // two seats share a device
    BadName,          // a name longer than 16 bytes
    NoSuchPlayer,
    Failed,           // the game could not make a player; everything was undone
};

// One plain line for the menu.
constexpr std::string_view Text(Why why) {
    switch (why) {
    case Why::Ok: return "OK";
    case Why::NotSupported: return "This build of the game has no local co-op";
    case Why::NotInGame: return "Load a save first, then start local co-op from the pause menu";
    case Why::Busy: return "Local co-op is already on or changing";
    case Why::TooFewPlayers: return "Local co-op needs at least two players";
    case Why::TooManyPlayers: return "Local co-op takes at most four players";
    case Why::NoDevice: return "Every player needs a connected controller (or the keyboard)";
    case Why::DeviceTwice: return "Two players cannot share one controller";
    case Why::BadName: return "A player name is too long (16 characters at most)";
    case Why::NoSuchPlayer: return "There is no such local player";
    case Why::Failed: return "The game could not add the player; local co-op was turned off";
    }
    return "";
}

struct Status {
    State state = State::Off;
    Why why = Why::Ok;          // why the last Start ended in Off, if it failed
    int players = 0;            // seats while not Off, else 0
    Split split = Split::Side;
    int scale = 100;
    std::array<Seat, kMostPlayers> seat{};        // the first `players` are used
    std::array<bool, kMostPlayers> lost{};        // the seat's device is not connected now
};

// The game side. Devices and Supported: any thread. Everything else: the
// game's calc thread, from Core::Frame.
class Game {
public:
    enum class Made : std::uint8_t { Pending, Yes, No };

    virtual ~Game() = default;
    virtual bool Supported() const = 0;
    virtual std::vector<Device> Devices() const = 0;
    virtual bool InPlay() const = 0;   // a scene is loaded and the first Link is there
    // Asks for local player `player` (1 and up) to be made; false: it cannot be.
    virtual bool Add(int player, const Seat& seat) = 0;
    // Whether the player asked for is there yet. Asked every frame while Starting.
    virtual Made Added(int player) = 0;
    // Takes the player out again (also one that was only asked for). True once it is gone.
    virtual bool Remove(int player) = 0;
    // The seats, split or scale changed, or local co-op went on or off (players 0):
    // devices and views follow `status`.
    virtual void Apply(const Status& status) = 0;
};

class Core {
public:
    explicit Core(Game& game) : game_(game) {}

    std::vector<Device> Devices() const { return game_.Supported() ? game_.Devices() : std::vector<Device>{}; }

    Why Start(const Config& config) {
        if (!game_.Supported()) return Why::NotSupported;
        const std::vector<Device> devices = game_.Devices();
        std::lock_guard lock(mutex_);
        if (status_.state != State::Off || want_ != Want::None) return Why::Busy;
        if (config.seats.size() < 2) return Why::TooFewPlayers;
        if (config.seats.size() > static_cast<std::size_t>(kMostPlayers)) return Why::TooManyPlayers;
        for (std::size_t i = 0; i < config.seats.size(); i++) {
            const Seat& seat = config.seats[i];
            if (seat.name.size() > kMostNameBytes) return Why::BadName;
            if (!Connected(devices, seat.device)) return Why::NoDevice;
            for (std::size_t j = 0; j < i; j++)
                if (config.seats[j].device == seat.device) return Why::DeviceTwice;
        }
        wanted_ = config;
        wanted_.scale = CoopLocalLayout::ClampScale(config.scale);
        for (std::size_t i = 0; i < wanted_.seats.size(); i++)
            if (wanted_.seats[i].name.empty()) wanted_.seats[i].name = "Player " + std::to_string(i + 1);
        want_ = Want::Start;
        status_.why = Why::Ok;
        return Why::Ok;
    }

    // Turns it off (also while it is starting). Nothing when it is off.
    void Stop() {
        std::lock_guard lock(mutex_);
        if (want_ == Want::Start) {
            want_ = Want::None;   // not begun yet
            return;
        }
        if (status_.state == State::Starting || status_.state == State::On) want_ = Want::Stop;
    }

    Status Get() const {
        std::lock_guard lock(mutex_);
        return status_;
    }

    // The split and the scale can change while it is starting or on; Start sets both.
    Why SetSplit(Split split) {
        std::lock_guard lock(mutex_);
        if (status_.split != split) status_.split = split, dirty_ = true;
        return Why::Ok;
    }

    Why SetScale(int percent) {
        std::lock_guard lock(mutex_);
        const int scale = CoopLocalLayout::ClampScale(percent);
        if (status_.scale != scale) status_.scale = scale, dirty_ = true;
        return Why::Ok;
    }

    // Gives local player `player` (0 and up) another device while it is on.
    Why SetDevice(int player, int device) {
        if (!game_.Supported()) return Why::NotSupported;
        const std::vector<Device> devices = game_.Devices();
        std::lock_guard lock(mutex_);
        if (status_.state != State::On || player < 0 || player >= status_.players) return Why::NoSuchPlayer;
        if (!Connected(devices, device)) return Why::NoDevice;
        for (int i = 0; i < status_.players; i++)
            if (i != player && status_.seat[Index(i)].device == device) return Why::DeviceTwice;
        if (status_.seat[Index(player)].device != device) status_.seat[Index(player)].device = device, dirty_ = true;
        return Why::Ok;
    }

    // The game's calc thread, once a frame hook.
    void Frame() {
        const std::vector<Device> devices = game_.Supported() ? game_.Devices() : std::vector<Device>{};
        std::lock_guard lock(mutex_);
        switch (status_.state) {
        case State::Off:
            if (want_ != Want::Start) break;
            want_ = Want::None;
            if (!game_.InPlay()) {
                status_.why = Why::NotInGame;
                break;
            }
            status_.players = static_cast<int>(wanted_.seats.size());
            status_.split = wanted_.split;
            status_.scale = wanted_.scale;
            for (int i = 0; i < status_.players; i++) status_.seat[Index(i)] = wanted_.seats[Index(i)];
            status_.state = State::Starting;
            asked_ = 0;
            made_ = 1;   // (player 0 is the game's own Link)
            frames_ = 0;
            [[fallthrough]];
        case State::Starting:
            if (want_ == Want::Stop) {
                BeginStop(Why::Ok);
                break;
            }
            StepStart();
            break;
        case State::On:
            if (want_ == Want::Stop) {
                BeginStop(Why::Ok);
                break;
            }
            MarkLost(devices);
            if (std::exchange(dirty_, false)) game_.Apply(status_);
            break;
        case State::Stopping:
            break;
        }
        if (status_.state == State::Stopping) StepStop();
    }

private:
    enum class Want : std::uint8_t { None, Start, Stop };

    static std::size_t Index(int player) { return static_cast<std::size_t>(player); }

    static bool Connected(const std::vector<Device>& devices, int slot) {
        if (slot < 0) return false;
        for (const Device& d : devices)
            if (d.slot == slot) return true;
        return false;
    }

    // One player at a time: ask, wait until it is there, then the next.
    void StepStart() {
        while (made_ < status_.players) {
            if (asked_ < made_) {
                if (!game_.Add(made_, status_.seat[Index(made_)])) return BeginStop(Why::Failed);
                asked_ = made_;
                frames_ = 0;
            }
            const Game::Made made = game_.Added(made_);
            if (made == Game::Made::No || (made == Game::Made::Pending && ++frames_ > kStartFrames))
                return BeginStop(Why::Failed);
            if (made == Game::Made::Pending) return;
            made_++;
        }
        status_.state = State::On;
        dirty_ = false;
        game_.Apply(status_);
    }

    void BeginStop(Why why) {
        want_ = Want::None;
        status_.why = why;
        status_.state = State::Stopping;
    }

    // Removes the players asked for, the last first; then one Apply with no players.
    void StepStop() {
        while (asked_ >= 1) {
            if (!game_.Remove(asked_)) return;
            asked_--;
        }
        const Why why = status_.why;
        status_ = Status{};
        status_.why = why;
        made_ = 0;
        dirty_ = false;
        game_.Apply(status_);
    }

    void MarkLost(const std::vector<Device>& devices) {
        for (int i = 0; i < status_.players; i++) {
            const bool lost = !Connected(devices, status_.seat[Index(i)].device);
            if (status_.lost[Index(i)] != lost) status_.lost[Index(i)] = lost, dirty_ = true;
        }
    }

    Game& game_;
    mutable std::mutex mutex_;
    Status status_;
    Config wanted_;
    Want want_ = Want::None;
    int asked_ = 0;    // the highest player Add was called for (0: none)
    int made_ = 0;     // players that are there, the first Link included
    int frames_ = 0;   // frame hooks waited for the player asked for
    bool dirty_ = false;
};

// The one Core of this game, set by the game side at start (null: this build
// has no local co-op). The menu's functions below go through it. Any thread.
inline std::atomic<Core*>& Installed() {
    static std::atomic<Core*> core{nullptr};
    return core;
}
inline void Install(Core* core) { Installed().store(core); }

inline std::vector<Device> Devices() {
    Core* core = Installed().load();
    return core ? core->Devices() : std::vector<Device>{};
}
inline Why Start(const Config& config) {
    Core* core = Installed().load();
    return core ? core->Start(config) : Why::NotSupported;
}
inline void Stop() {
    if (Core* core = Installed().load()) core->Stop();
}
inline Status Get() {
    Core* core = Installed().load();
    return core ? core->Get() : Status{};
}
inline Why SetSplit(Split split) {
    Core* core = Installed().load();
    return core ? core->SetSplit(split) : Why::NotSupported;
}
inline Why SetScale(int percent) {
    Core* core = Installed().load();
    return core ? core->SetScale(percent) : Why::NotSupported;
}
inline Why SetDevice(int player, int device) {
    Core* core = Installed().load();
    return core ? core->SetDevice(player, device) : Why::NotSupported;
}

} // namespace CoopLocal
