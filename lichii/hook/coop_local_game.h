// SPDX-License-Identifier: GPL-3.0-or-later
// Local co-op's game side (docs/local-coop.md, step 2): CoopLocal::Game for
// a game whose Players read their pads through the wraps of patch 0010.
//
// Local player k (1 to 3) is the game's own Player_<Age>, made by the puppet
// Director as it makes another player's (coop_native_player.h: made, then
// left to the game's player code), with the number LocalNumber(k). Its pad is
// taken from player 1's merged pad and is Npad No.(k+1) (Hookshot's exports,
// patch 0010), and the Player's reads ask for that controller
// (coop_local_input.h). Views (step 3) and the session (step 4) come later.
//
// Everything outside (the pads, the Director's slots, guest memory) comes in
// through Deps, so the tests run it with no game.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "coop_local.h"
#include "coop_local_input.h"
#include "coop_puppet.h"

namespace CoopLocalGame {

// The Device slot of the keyboard and mouse: player 1's only (v1: they stay
// in player 1's merged pad).
inline constexpr int kKeyboardSlot = 100;
// Npad No.(k+1) for local player k.
inline constexpr int NpadFor(int player) { return player; }
// Local player k's number in the puppet Director: past every session number
// (1 to kMaxPlayers), so it never meets a remote player's, and the same
// offline and online (the session's number for it is only on the wire,
// coop_local_net.h). The Director's live mask has a bit for each (< 32).
constexpr std::uint8_t LocalNumber(int player) {
    return static_cast<std::uint8_t>(coop::protocol::kMaxPlayers + player);
}
static_assert(coop::protocol::kMaxPlayers + CoopLocalInput::kMostExtra < 32);
// How far beside the first Link an extra player is made, world units, by seat.
inline constexpr std::array<CoopPuppet::Vec3, CoopLocalInput::kMostExtra> kBeside{{
    {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}};

// An extra local player in play, for the session (coop_local_net.h).
struct SeatNow {
    int player = 0;              // 1 to 3
    std::uint64_t actor = 0;     // its Player, 0 while it is being made
    std::string name;            // "Player <n>" unless the menu named it
    std::optional<std::uint32_t> color;
    bool operator==(const SeatNow&) const = default;
};

struct Pad {
    int slot = 0;
    std::uint32_t serial = 0;
    std::string name;
    int npad = 0;   // taken as Npad No.(npad+1); 0: in player 1's pad
};

struct Deps {
    // The exe's pads (hos_coop_pad_info / hos_coop_pad_take). Unset: no local co-op.
    std::function<std::vector<Pad>()> pads;
    std::function<bool(int slot, std::uint32_t serial, int npad)> take;
    // Whether the Player's wraps 16, 17 and 19 are in this game build.
    std::function<bool()> wraps;
    // The Director's slots (CoopPuppet::TheDirector().Slots()), the calc thread.
    std::function<std::vector<CoopPuppet::Director::SlotView>()> slots;
    // main's guest address (CoopRuntime::MainBase), for the controller manager.
    std::function<std::uint64_t()> main_base;
    // Guest memory, any thread.
    std::function<bool(std::uint64_t address, void* out, std::size_t size)> read;
    std::function<void(const std::string&)> log;
};

// What the hook knows of the first Link this frame (calc thread).
struct Facts {
    bool game = false;                          // a game is running and its scene is known
    std::optional<CoopPuppet::Vec3> position;   // the first Link's
    bool adult = false;
};

class Game final : public CoopLocal::Game {
public:
    explicit Game(Deps deps) : deps_(std::move(deps)) {}

    bool Supported() const override;
    std::vector<CoopLocal::Device> Devices() const override;
    bool InPlay() const override;
    bool Add(int player, const CoopLocal::Seat& seat) override;
    Made Added(int player) override;
    bool Remove(int player) override;
    void Apply(const CoopLocal::Status& status) override;

    // The calc thread, each frame before the puppets are planned.
    void See(const Facts& facts);
    // The puppet Director's targets for the local players (calc thread, from CoopPuppet::Publish).
    void AddTargets(CoopPuppet::Input& input) const;
    // A wrapped function (hos_coop_wrap): 16 and 17 open and close a Player's
    // read, 19 is getController (x0 manager, w1 index): x1 is turned to
    // controller k while an extra player's Player reads. 18 is the Player's
    // input-off check: answered "off" (1, the game's check not run) for an
    // extra player's Player whose controller the game does not have, so it
    // never reads player 1's pad. Returns hos_coop_wrap's value.
    int Wrap(std::uint32_t id, int phase, std::uint64_t* x) const;

    const CoopLocalInput::Players& Players() const { return players_; }
    // The extra players asked for (calc thread).
    std::vector<SeatNow> Seats() const;
    void Note(const std::string& line) const { Say(line); }

private:
    struct Extra {
        bool wanted = false;
        std::string name;
        int device = -1;
        std::uint32_t serial = 0;   // the pad's opening when it was taken
        std::optional<CoopPuppet::Vec3> spot;   // where it is made: beside the first Link when asked
        std::optional<std::uint32_t> color;
    };
    static std::size_t Index(int player) { return static_cast<std::size_t>(player - 1); }
    std::optional<Pad> FindPad(int slot) const;
    bool TakePad(int player, int slot);
    void GivePad(int player);
    std::uint64_t ActorOf(int player) const;   // its Player once live, else 0
    std::uint64_t ReadingActor(std::uint64_t node) const;
    bool HasController(std::uint64_t manager, std::uint32_t index) const;
    void SayNoController(int player) const;
    void Say(const std::string& line) const {
        if (deps_.log) deps_.log("local co-op: " + line);
    }

    Deps deps_;
    Facts facts_;
    std::array<Extra, CoopLocalInput::kMostExtra> extra_{};
    CoopLocalInput::Players players_;
    mutable std::array<std::atomic<bool>, CoopLocalInput::kMostExtra> no_controller_said_{};
};

// The one game side, made at module start (Hookshot). Install also installs
// its CoopLocal::Core. Null until then: the free functions do nothing.
void Install(Deps deps);
Game* Installed();
// The calc thread: Facts, then the Core's frame.
void Frame(const Facts& facts);
// CoopPuppet::Publish: the local players' targets.
void AddTargets(CoopPuppet::Input& input);
// The extra players asked for (calc thread); none before Install.
std::vector<SeatNow> Seats();
// hos_coop_wrap; its return value.
int Wrap(std::uint32_t id, int phase, std::uint64_t* x);
// Tests (LICHII_COOP_LOCAL=<slot>): at the first frame in play, local co-op
// starts with the keyboard for player 1 and pad `slot` for player 2.
void AutoStart(int slot);

}  // namespace CoopLocalGame
