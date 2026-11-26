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

// --- Hurts being given to our Link (coop_objects.cpp GivePvp) ---

// One PvP hurt our game gives its Link: first as the game's own hit (the hit replay), then, if the game has not
// taken it in time, by writing the life. It is taken once Link's life is at `expect` or below.
struct PendingGive {
    std::int32_t take = 0;
    std::int32_t expect = 0;  // the life after it
    std::uint64_t asked_ms = 0;
    bool replayed = false;
    bool write = true;  // false (phase 4 path B): never the life write, only the game's own hit
};

// Our Link's life between two reads, against the PvP hurts waiting on it. `expect` is a life, so every change that
// is not the PvP hit itself moves it too, else that change is booked as the PvP hit (g2-bosses, the copy-hit rule
// the other way): a fall that is not our replay landing lowers every expect by that fall (a monster's hit: Link's
// last hit id moved to an id we did not replay; harm with no hit record, a fall, fire, drowning: no id moved), and
// a rise (a heart, a fairy, a revive) raises it, so the written fallback never takes a heal back.
//
// The walk reads the life and the hit id one after the other on the present thread while the game's Life step
// sets both on the calc thread, so a step can land between the two reads: its id is then seen one read before or
// after its fall (review of 367e23f3: a replay's fall read a walk after its id was booked as harm, and the
// fallback dealt the PvP hit again). So the two are paired across one read, both ways:
// - our replayed id reached with no fall: its fall may come next read (`reached`), and is the landing then;
// - a fall with no id moved (id read): its id may come next read, so it is held one read (`Follow` returns true:
//   nothing is booked or written that read); next read it is the landing if our replayed id came, else harm.
// Id not read (`id_read` false): a fall is the landing, as before.
// When our replayed id comes with a fall held last read, the held fall and this read's are booked as one change
// (our replay's part first, the rest harm), never held again. A held fall is applied only to the gives waiting when
// it was held: the caller takes no new PvP asks on a hold read (review-a38-4: a give pushed then, its expect taken
// from a life that already had the held fall in it, was lowered by that fall again next read).
class LinkLifeWatch {
public:
    // Returns true when this read must not book or write any wait (a fall held for its id).
    bool Follow(std::vector<PendingGive>& gives, std::int32_t before, std::int32_t now, bool own_hit,
                bool replay_hit, bool id_read);
    void Reset() {
        held_ = 0;
        reached_ = 0;
    }

private:
    std::int32_t held_ = 0;     // a fall with no id moved, last read
    std::int32_t reached_ = 0;  // our replayed id reached with no fall, last read: its takes
};

// The life a new PvP hurt `take` expects our Link at: after every hurt still waiting (each one's `expect` already
// counts the ones before it), so one hurt's landing or written fallback is never booked as another's (two asks of
// equal take: the first landing booked both, and the second, refused by Link's invincible frames, was lost; with
// the replay off the first write booked the second unwritten).
std::int32_t NextExpect(const std::vector<PendingGive>& gives, std::int32_t life, std::int32_t take);

} // namespace CoopPvp
