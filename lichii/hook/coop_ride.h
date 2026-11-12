// SPDX-License-Identifier: GPL-3.0-or-later
//
// Issue 199: a player riding Epona stays seen by the others, on a horse.
//
// Before, a remote rider's puppet was parked out of sight (cutscene-hide,
// coop/host/scene.hpp), because the other game has no horse for it to sit
// on. Now the puppet shows in its riding animation (Visibility::ride) and a
// copy of the horse is drawn under it:
//
// - The rider's game (Rider): while its own Link's command names a horse
//   (coop::host::activity_of), it finds the nearest Animal_Epona_Adult on the
//   actor list and sends PLAYER_RIDE about 30 times a second: the horse's
//   place and turn in the rider's own frame (so the copy turns and moves
//   with the puppet, whatever the network delay), and the horse's AS slot 0.
//   When the rider gets off it sends riding off a few times.
// - The other games (Rides): for each remote whose puppet is live and who
//   rides (fresh PLAYER_RIDE, Visibility::ride), one copy of the horse at the
//   puppet's place, made by CoopSpawn's director as the stand-in
//   CoopRide_Epona (the horse's model and animations, built by
//   tools/puppet_pack; never the game's own horse, which has AI). Getting
//   off, leaving, a stale ride or the puppet going: the copy is deleted.
// - No stand-in in the pack (an old pack) or no horse found: the puppet still
//   shows in its riding pose where the rider is (never invisible).
//
// Rider and Rides need no emulator code (tests/lichii/ride_test.cpp); the
// glue at the end of coop_ride.cpp is built into the emulator only.

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"
#include "coop_puppet.h"
#include "coop_spawn.h"

namespace coop::host {
class Session;
}
namespace Core {
class System;
}

namespace CoopRide {

using Vec3 = CoopPuppet::Vec3;
using Mat3 = CoopPuppet::Mat3;
using Quat = CoopPuppet::Quat;

// The horse a player rides and its copy's pack (tools/puppet_pack STAND_INS).
inline constexpr std::string_view kHorse = "Animal_Epona_Adult";
inline constexpr std::string_view kHorseStandIn = "CoopRide_Epona";
bool IsHorse(std::string_view actor_name);

// The turn and place of `what` in the frame of `frame` (rows, as the actor
// holds its matrix), and back. Exact inverses for a proper rotation `frame`.
Mat3 Relative(const Mat3& frame, const Mat3& what);
Mat3 Absolute(const Mat3& frame, const Mat3& relative);
Vec3 ToFrame(const Mat3& frame, const Vec3& world_offset);
Vec3 FromFrame(const Mat3& frame, const Vec3& local_offset);

// A horse on our actor list.
struct HorseSeen {
    std::uint64_t address = 0;
    Vec3 position{};
    Mat3 rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::optional<coop::protocol::Animation> animation;
};

// Our side: what to send.
class Rider {
public:
    static constexpr std::uint64_t kEveryUs = 33'000;  // 30 a second
    static constexpr float kNear = 4.0f;               // the horse under us, metres
    static constexpr int kOffSends = 3;                // riding off, sent this many times

    // Each frame. `riding`: our command names a horse. `rotation` unset:
    // identity. Returns the message to send now, if any.
    std::optional<coop::protocol::PlayerRide> Tick(bool riding, const std::optional<Vec3>& position,
                                                   const std::optional<Quat>& rotation,
                                                   const std::vector<HorseSeen>& horses,
                                                   std::uint64_t now_us);
    // Whether a walk for horses is needed this frame (riding, or off to send).
    bool Wants(bool riding, std::uint64_t now_us) const;
    bool Riding() const { return riding_; }
    // The horse we last sent (0 none), for the log.
    std::uint64_t Horse() const { return horse_; }
    void Reset();

private:
    bool riding_ = false;
    int off_left_ = 0;
    std::uint64_t last_us_ = 0;   // last sent
    std::uint64_t tried_us_ = 0;  // last Tick (Wants: a walk at most kEveryUs apart)
    std::uint64_t horse_ = 0;
};

// What the others' games draw.
class Rides {
public:
    // A ride not heard of for this long is over (riding off can be lost: the
    // samples channel is unreliable).
    static constexpr std::uint64_t kStaleUs = 600'000;
    static constexpr std::uint64_t kKeyBit = 1ull << 61;  // CoopSpawn::Director::SetRides

    // Network thread.
    void Receive(const coop::protocol::PlayerRide& ride, std::uint64_t now_us);
    void Forget(std::uint8_t number);  // the player left

    // Present thread. `remotes`: CoopPuppet::Collect, as the puppets plan.
    // `live_puppets`: CoopPuppet::LivePuppets (bit n: puppet n is live and
    // not parked). One copy per remote who rides here and whose puppet is
    // live, at the puppet's place.
    std::vector<CoopSpawn::Copy> Plan(const std::optional<std::string>& scene, bool local_event,
                                      const std::vector<CoopPuppet::Remote>& remotes,
                                      std::uint32_t live_puppets, std::uint64_t now_us);

    // For the log: a line for each rider that started or stopped showing on a
    // horse since the last call ("P2 rides Animal_Epona_Adult", "P2 off the horse").
    std::vector<std::string> Changes();

private:
    struct Ride {
        coop::protocol::PlayerRide message;
        std::uint64_t at_us = 0;
        std::uint32_t mount = 0;  // goes up with each new ride: a new copy key
    };
    std::mutex mutex_;
    std::map<std::uint8_t, Ride> rides_;  // guarded
    std::map<std::uint8_t, bool> shown_;  // present thread
    std::vector<std::string> changes_;    // present thread
};

// The hook (emulator only): CoopHook's frame, after CoopPuppet::Publish.
// `own_animation`: our Link's AS command (PlayerReading::animation).
void Frame(Core::System& system, std::uint64_t main_base, coop::host::Session* session,
           const std::optional<std::string>& scene, bool local_event,
           const std::optional<Vec3>& own_position, const std::optional<Quat>& own_rotation,
           const std::optional<coop::protocol::Animation>& own_animation,
           std::uint64_t server_now_us);
void OnEvent(const coop::net::Event& event);

} // namespace CoopRide
