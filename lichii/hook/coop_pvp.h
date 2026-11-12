// SPDX-License-Identifier: GPL-3.0-or-later
//
// Player against player (docs/play-online.md "PvP", docs/protocol.md "PvP"):
// the rules, with no emulator code in them (coop_pvp_game.cpp is the hook's
// side). Off unless the host turns it on; the server's PVP_MODE is the
// session's value, and a guest's own setting does nothing.
//
// - The attacker's game sees its own sword swing (the game's foliage cut,
//   which carries three points of the blade) or its own shot end (an arrow
//   or seed that hit, a bomb that went off) near another player's Link, as it
//   stands on the attacker's screen, and sends PVP_HIT to that player.
// - The victim's game decides (a laggy attacker cannot kill someone who got
//   away on their own screen): nothing in a cutscene, a menu, while loading
//   or paused, from an attacker who is in one, or from another scene; one hit
//   at most every kMercyUs, as Link blinks after a hit. It takes the life and
//   sends PVP_HURT, which every game shows as a chat notice.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopPvp {

using Vec3 = coop::protocol::Vec3;

// --- The session's value (PVP_MODE) ---

struct Mode {
    bool on = false;
    std::uint8_t set_by = 0;  // 0: the server's start (coop-options.ini); else the host
};

// --- /pvp in the chat ---

struct Command {
    enum class Kind : std::uint8_t { show, on, off, bad };
    Kind kind = Kind::show;
};

// Nothing for text that is chat. "/pvp" shows the value, "/pvp on" and
// "/pvp off" (also 1/0, yes/no, any case) ask the server to change it, which
// it does only for the host; anything else after "/pvp" is `bad`.
std::optional<Command> Parse(std::string_view text);

// --- The attacker's side ---

// Another player's Link as our game shows it (CoopPuppet::Collect).
struct Target {
    std::uint8_t number = 0;
    std::optional<std::string> scene;
    Vec3 position{};          // at the feet
    std::uint8_t status = 0;  // PLAYER_STATUS flags
};

inline constexpr float kSwordReachM = 1.3f;   // a blade point this near the body, level
inline constexpr float kBodyLowM = -0.4f;     // the body, from the feet up
inline constexpr float kBodyHighM = 2.2f;
inline constexpr float kMaxSwingFromSelfM = 4.0f;  // a blade this far from us is not ours
inline constexpr std::uint64_t kSameVictimUs = 400'000;  // one hit per player per swing

// Quarter hearts a hit takes before the victim's rules: half a heart for a
// sword, as most enemies hit.
std::int32_t AmountOf(coop::protocol::DamageCause cause);
// How near a shot's end must be to a body (level) to hit it; 0: never.
float ReachOf(coop::protocol::DamageCause cause);

// Whether a player in this state takes no part: paused, loading, in a
// cutscene, in a menu, or dead.
bool Guarded(std::uint8_t status_flags);

class Attacker {
public:
    // Our sword swing (sweep points NaN: not a sword, nothing). One PVP_HIT
    // for each other player in our scene it reached, at most one per player
    // in kSameVictimUs. `self` is our Link's place; `self_number` our number.
    std::vector<coop::protocol::PvpHit> Swing(const coop::protocol::GrassCut& cut, const Vec3& self,
                                              std::string_view scene, std::uint8_t self_number,
                                              std::span<const Target> targets, std::uint64_t now_us);
    // Our own shot ended at `at` (hit something, or went off): the players
    // it reached. `self` is where we stand (the hit comes from there).
    std::vector<coop::protocol::PvpHit> Shot(coop::protocol::DamageCause cause, const Vec3& at,
                                             const Vec3& self, std::string_view scene,
                                             std::uint8_t self_number, std::span<const Target> targets,
                                             std::uint64_t now_us);
    void Reset() { last_hit_us_ = {}; }

private:
    bool Ready(std::uint8_t number, std::uint64_t now_us);
    std::array<std::uint64_t, coop::protocol::kMaxPlayers + 1> last_hit_us_{};
};

// --- The victim's side ---

inline constexpr std::uint64_t kMercyUs = 800'000;  // after a PvP hurt, no other this long

// Our game this frame.
struct Local {
    std::uint8_t self = 0;            // our player number
    std::optional<std::string> scene;
    std::uint8_t status = 0;          // our PLAYER_STATUS flags
    bool event = false;               // an event (cutscene, talk) has control
    std::optional<std::int32_t> life; // our Link's life, quarter hearts; nothing: not read
};

// The attacker as presence knows it.
struct Hitter {
    std::optional<std::string> scene;
    std::uint8_t status = 0;
    bool known = false;  // in the room
};

struct Decision {
    coop::protocol::PvpHurt hurt;  // to send (player field 0)
    std::int32_t take = 0;         // life to take off our Link now
};

class Victim {
public:
    // A PVP_HIT that reached us. Nothing when it is no hit for us at all (PvP
    // off here, another victim, our own); a decision with take 0 and the
    // guarded flag when the rules keep it off.
    std::optional<Decision> Hit(const coop::protocol::PvpHit& hit, bool pvp_on, const Local& local,
                                const Hitter& attacker, std::uint64_t now_us);
    void Reset() { last_hurt_us_.reset(); }

private:
    std::optional<std::uint64_t> last_hurt_us_;
};

// --- What the chat box shows ---

// "PvP is on (the host turned it on)" and the like. `self` is our number.
std::string ModeLine(const Mode& mode, std::uint8_t self, std::string_view by_name);
// "Saria hit you (-1/2 heart)", "you defeated Mido", "Saria hit Mido (-1 heart)".
// Nothing for a guarded hurt.
std::optional<std::string> HurtLine(const coop::protocol::PvpHurt& hurt, std::uint8_t self,
                                    std::string_view victim_name, std::string_view attacker_name);
// "1/2 heart", "1 heart", "1 3/4 hearts".
std::string Hearts(std::int32_t quarters);

} // namespace CoopPvp
