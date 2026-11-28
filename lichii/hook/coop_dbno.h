// SPDX-License-Identifier: GPL-3.0-or-later
//
// Down but not out (docs/play-online.md "Down but not out", docs/protocol.md
// "Down but not out"): the rules, with no emulator or Hookshot code in them
// (coop_dbno_game.cpp is the hook's side). Co-op only, on unless the host
// turns it off; the server's DBNO_MODE is the session's value.
//
// - A hit that would take our Link's last life leaves a quarter heart and
//   downs him instead (LifeGuard, asked by the game's own Life step): he
//   crawls slowly, uses nothing, takes no more damage, and bleeds out.
// - A teammate standing beside him holds A (Reviver sends DBNO_REVIVE); the
//   downed player's own game counts the hold (Machine) and tells everyone
//   (DBNO_STATE), so both see the same ring. A full hold: up again with the
//   rules' hearts.
// - Bled out: spectating. The camera watches a player still up (A: the next
//   one); once no enemy is near the players still up (beaten, or left behind),
//   back up beside the one watched, with the game's continue hearts.
// - Nobody left standing (everyone down, spectating or out): out, in every
//   game at once. The game's own death and Game Over run, as without co-op (a
//   fairy in a bottle still saves Link there); Try Again follows the host's
//   save (patch 0024, docs/flags.md).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopDbno {

namespace proto = coop::protocol;
using Vec3 = proto::Vec3;
using State = proto::DbnoState;

// --- The session's rules (DBNO_MODE) ---

// A game starts every connection with the mode off, until its server says
// otherwise: with a server that knows nothing of it, death is the game's own.
struct Rules {
    bool on = false;
    std::uint8_t set_by = 0;          // 0: the server's start; else the host
    std::uint8_t bleed_s = 45;        // bleed-out time
    std::uint8_t revive_tenths = 50;  // how long a revive is held (5 s, row 174)
    std::uint8_t revive_life = 12;    // quarter hearts a revived player gets

    std::uint64_t BleedUs() const { return std::uint64_t{bleed_s} * 1'000'000; }
    std::uint64_t ReviveUs() const { return std::uint64_t{revive_tenths} * 100'000; }
    bool operator==(const Rules&) const = default;
};

Rules RulesOf(const proto::DbnoMode& mode);
// What the host asks the server for (`set_by` 0), each value brought within
// the protocol's limits, so the Host settings page may hand over anything.
proto::DbnoMode Ask(bool on, int bleed_s, int revive_tenths, int revive_life);

// The Host settings page's side (coop_dbno_game.cpp; any thread): the rules
// as the session has them now, and a change the host asks for. Change answers
// false when we are not connected or do not host; the server's DBNO_MODE
// that follows is what Current then shows.
Rules Current();
bool Change(const proto::DbnoMode& want);

// --- /dbno in the chat ---

struct Command {
    enum class Kind : std::uint8_t { show, change, bad };
    Kind kind = Kind::show;
    // change: what was typed; the rest stays as the session has it.
    std::optional<bool> on;
    std::optional<int> bleed_s;
    std::optional<int> revive_tenths;
    std::optional<int> revive_life;
};

// Nothing for text that is chat. "/dbno" shows the rules; "/dbno on", "/dbno
// off", "/dbno time 60" (seconds to bleed out), "/dbno hold 3" (seconds a
// revive is held, halves allowed: 2.5) and "/dbno hearts 3" (halves allowed)
// ask the server to change them, which it does only for the host.
std::optional<Command> Parse(std::string_view text);
// The rules with a command's change applied, as the host's ask.
proto::DbnoMode Applied(const Rules& now, const Command& change);

// --- The life guard ---

// Life a downed Link keeps: a quarter heart, so the game never sees him dead.
inline constexpr std::int32_t kDownedLife = 1;

enum class Guard : std::uint8_t {
    off,         // the game's own rules (mode off, nobody to revive us, a bottled fairy, or out)
    last_stand,  // up: a hit that would take the last life leaves kDownedLife
    shielded,    // downed, or just revived: nothing is taken
};

struct Taken {
    std::int32_t sum = 0;  // what the Life step takes off this frame
    bool lethal = false;   // last_stand only: the hit was held back from killing
};

// What a Life step that was about to take `sum` off `life` takes instead. A
// negative sum is healing, which the guard never holds back from a Link who
// is up.
constexpr Taken Guarded(Guard guard, std::int32_t life, std::int32_t sum) {
    switch (guard) {
    case Guard::off:
        break;
    case Guard::last_stand:
        if (sum > 0 && life > 0 && sum >= life) {
            return Taken{life - kDownedLife, true};
        }
        break;
    case Guard::shielded:
        return Taken{0, false};
    }
    return Taken{sum, false};
}

// A Life step that takes `sum` off `life` when the rules want the life
// lowered to `to` (-1: they do not): it takes at least down to `to`, so the
// game's own step does it (its death at 0). Never less than the game's own.
constexpr std::int32_t Lowered(std::int32_t life, std::int32_t sum, std::int32_t to) {
    return to >= 0 && life - sum > to ? life - to : sum;
}

// How long a lowering waits for the game's Life step before it is written
// directly (the step skips some frames: no life value), as CoopActorEvents'
// fallback does.
inline constexpr std::uint64_t kTakeWaitUs = 250'000;

// A lowering of our Link's life for the Life step (Lowered's `to`). The frame
// asks (Ask, each frame it wants it); the Life step takes it (Take, any game
// thread). An ask the step has not taken within kTakeWaitUs comes back from
// Ask as the life to write directly instead.
class Lowering {
public:
    std::optional<std::int32_t> Ask(std::int32_t to, std::uint64_t now_us) noexcept {
        if (to_.exchange(to, std::memory_order_acq_rel) < 0 || since_us_ == 0) {
            since_us_ = now_us == 0 ? 1 : now_us;  // a new ask: the step had taken the last
            return std::nullopt;
        }
        if (now_us < since_us_ + kTakeWaitUs) {
            return std::nullopt;
        }
        to_.store(-1, std::memory_order_release);
        since_us_ = 0;
        return to;
    }
    // The frame's ask is over (nothing to lower now).
    void Drop() noexcept {
        to_.store(-1, std::memory_order_release);
        since_us_ = 0;
    }
    std::int32_t Take() noexcept { return to_.exchange(-1, std::memory_order_acq_rel); }

private:
    std::atomic<std::int32_t> to_{-1};
    std::uint64_t since_us_ = 0;  // the frame's own
};

// The guard as the game's threads see it. The frame sets it (Set); the Life
// step of our own Link asks it (Step, any game thread) and its lethal hits
// are counted for the next frame (TakeLethal).
class LifeGuard {
public:
    void Set(Guard guard) noexcept { guard_.store(guard, std::memory_order_release); }
    Guard Get() const noexcept { return guard_.load(std::memory_order_acquire); }
    // Our Link's Life step: the sum to take. A lethal hit turns the guard to
    // shielded at once, so a second hit in the same frame takes nothing.
    std::int32_t Step(std::int32_t life, std::int32_t sum) noexcept;
    // Whether a lethal hit was held back since the last call.
    bool TakeLethal() noexcept { return lethal_.exchange(false, std::memory_order_acq_rel); }
    // That a Life step asks the guard at all in this build (the native Life
    // step hook): without it nothing can hold a lethal hit back, and the
    // rules stay out of the way.
    void SetLive(bool live) noexcept { live_.store(live, std::memory_order_release); }
    bool Live() const noexcept { return live_.load(std::memory_order_acquire); }

private:
    std::atomic<Guard> guard_{Guard::off};
    std::atomic<bool> lethal_{false};
    std::atomic<bool> live_{false};
};

// A life our own code writes to our Link past the game's Life step (PvP's
// write when the game's own hit did not come in time): what the guard lets
// it lower `life` to when `to` is asked, as the Life step would (a lethal
// one leaves kDownedLife and downs him; downed or just revived, nothing).
// Player report 34: PvP wrote 0 over a Link the game's hit had just downed.
inline std::int32_t GuardedWrite(LifeGuard& guard, std::int32_t life, std::int32_t to) {
    return to >= life ? to : life - guard.Step(life, life - to);
}

// --- The downed player's side ---

inline constexpr float kReviveReachM = 2.5f;  // level distance, feet to feet
inline constexpr float kReviveHighM = 2.0f;   // height apart at most
// The downed game sees the reviver through its puppet, a little late: it
// takes a hold from this much further than the reviver's own game asks.
inline constexpr float kReviveSlack = 1.3f;
inline constexpr std::uint64_t kHoldSendUs = 200'000;   // a held revive is sent this often
inline constexpr std::uint64_t kHoldLiveUs = 700'000;   // and counts this long after its last message
inline constexpr std::uint64_t kWipeGraceUs = 3'000'000;  // nobody up this long: out
inline constexpr std::uint64_t kMercyUs = 2'000'000;    // shielded this long after a revive
inline constexpr std::uint64_t kMaxStepUs = 250'000;    // a frame counts as at most this (a hitch, a load)
inline constexpr std::uint64_t kOutAgainUs = 500'000;   // out and still alive: the life is written again
// Downed with no Link this long (not a load: back on the title screen): up
// again without a word, as the save that is loaded next decides the life.
inline constexpr std::uint64_t kGoneUs = 20'000'000;
inline constexpr std::uint64_t kStateSendUs = 500'000;  // DBNO_STATE while downed, with nobody reviving
inline constexpr std::uint64_t kStateSendRevivingUs = 100'000;  // and while the ring fills
// Spectating: back up once no enemy has been near the players watched for
// kQuietUs, and never before kSpectateMinUs (the camera shows where they are).
inline constexpr std::uint64_t kQuietUs = 3'000'000;
inline constexpr std::uint64_t kSpectateMinUs = 3'000'000;
// Life a spectating player comes back with: 3 hearts, as the game's own
// continue after a Game Over (capped at the player's hearts).
inline constexpr std::int32_t kRespawnLife = 12;
// Our own Link at no life while up: the game's death may still be saved (a
// fairy in a bottle reads 0 while it revives him). Out only once the Link is
// gone (the Game Over screen, a reload) or the life stays 0 this long: a
// fallback only (the Game Over screen is the game's own sign); the bottle
// fairy's revive is to be measured in the research run (must be shorter).
inline constexpr std::uint64_t kDyingUs = 8'000'000;
// The respawn's move (the /teleport trip, then the entrance) that has not
// answered in this long counts as failed.
inline constexpr std::uint64_t kRespawnWaitUs = 20'000'000;
// Rows 318 + 334: a teammate who stops being nearby (Teammate::here) still
// counts this long, and while their scene loads (a door, a loading zone), so
// a door's load does not flap between nearby and not.
inline constexpr std::uint64_t kNearbyGraceUs = 1'000'000;
// Row 330: a downed player who holds A this long gives up (row 317 c): the
// co-op death at once (as a bleed-out: watch the one nearby, back beside them
// once no enemy is near), never the game's Game Over and its Try Again at the
// last save while a teammate stands. A fresh press: A held as he went down
// (an attack) does not count until it is let go.
inline constexpr std::uint64_t kGiveUpUs = 2'000'000;

// How the respawn's move went (coop_teleport_game.h Beside / ToEntrance).
enum class Moved : std::uint8_t { none, done, failed };

// What a player's last DBNO_THREAT said (unknown: none heard lately, which
// never holds a spectating player back: a build without it, a lost message).
enum class Threat : std::uint8_t { unknown, clear, enemy };

// Another player as presence and their DBNO_STATE show them.
struct Teammate {
    std::uint8_t number = 0;
    State state = State::up;
    bool in_game = false;             // has a Link now (not on the title, not loading)
    bool loading = false;             // a scene load (a door, a loading zone, a teleport, a Try Again)
    std::uint8_t status = 0;          // their PLAYER_STATUS flags (proto::status)
    std::optional<std::string> scene;
    std::optional<Vec3> position;     // at the feet, as our game shows them
    Threat threat = Threat::unknown;  // their DBNO_THREAT
    // The game can show them from where we are (the spectating camera's
    // rule, coop_dbno_game.cpp ShownHere): in the game, our scene and, in a
    // dungeon, a room CoopTeleport::CheckMove says ok. Rows 318 + 334: only
    // a nearby teammate (up, here, or here within kNearbyGraceUs) keeps a
    // downed or spectating player waiting.
    bool here = false;
};

// Whether `them` at `at` is near enough to `self` to revive, `slack` times
// the reach.
bool InReach(const Vec3& self, const Vec3& at, float slack = 1.0f);

struct Input {
    std::uint64_t now_us = 0;
    Rules rules;
    bool connected = false;               // and this connection has heard its DBNO_MODE
    bool guard_live = false;              // LifeGuard::Live
    bool lethal = false;                  // LifeGuard::TakeLethal
    std::optional<std::int32_t> life;     // our Link's life, quarter hearts; nothing: no Link to read
    std::optional<std::int32_t> max_life;
    std::optional<std::string> scene;
    Vec3 position{};
    std::span<const Teammate> team;
    bool next = false;                    // spectating: the switch button went down this frame
    Moved moved = Moved::none;            // spectating: how the respawn's move asked earlier went
    bool enemy_near_us = false;           // an enemy of our game within kThreatReachM of our own Link
    // A fairy in one of our bottles (tracker row 162): the game's own death at no
    // life lets it revive us, as in the solo game, so a lethal hit is not held
    // back (the guard is off while we hold one) and we are not downed.
    bool bottled_fairy = false;
    // Row 330: downed, A held this frame (our own pad, before DownedPad takes it).
    bool give_up = false;
};

enum class Event : std::uint8_t {
    went_down,
    revived,     // by `Output::by`
    bled_out,
    wiped,       // nobody was left standing
    died,        // the game took the life anyway (a pit, a script)
    got_up,      // the host turned the mode off
    back,        // alive again after out (a fairy, a continue)
    respawned,   // spectating over: up again beside `Output::watching` (0: at the entrance)
    // Rows 318 + 334: a lethal hit or a bleed-out with no teammate nearby: no
    // downed wait, no spectating; straight to the entrance, then respawned.
    alone,
    // Row 330: we held A to give up while downed: spectating at once, as a
    // bleed-out (Output::by: whom we watch); with nobody up, the wipe.
    gave_up,
};

struct Output {
    State state = State::up;
    Guard guard = Guard::off;
    std::optional<std::int32_t> write_life;      // set our Link's life to this now
    std::optional<proto::DbnoStatus> send;       // to everyone (player field 0)
    std::vector<Event> events;
    std::uint8_t by = 0;                         // revived: who did it; bled out, respawned: whom we watch
    // For the screen, while downed.
    std::uint64_t left_us = 0;
    std::uint8_t reviver = 0;
    float progress = 0.0f;                       // 0 to 1
    // Spectating: whom the camera watches (0 nobody). Bled out and respawned
    // name that player in `by`.
    std::uint8_t watching = 0;
    // Spectating, ready to come back: move our Link beside this player first
    // (the /teleport trip); or, that failed, to the entrance (the game's own
    // scene start). The respawn follows once Input::moved says done.
    std::uint8_t move_beside = 0;
    bool move_entrance = false;
    // Row 330, downed: a teammate is up, so holding A gives up; how far the
    // hold is (0 to 1).
    bool can_give_up = false;
    float give_up = 0.0f;
};

class Machine {
public:
    Output Frame(const Input& in);
    // A DBNO_REVIVE that reached us.
    void Hold(std::uint8_t from, bool holding, std::uint64_t now_us);
    // Someone joined, or we are connected again: our state is told once more
    // (only a state other than up is news to them).
    void TellAgain() { tell_again_ = true; }
    State Now() const { return state_; }

private:
    std::uint8_t PickReviver(const Input& in) const;
    std::uint8_t PickWatch(const Input& in) const;
    // Row 330: any teammate up and in the game to be moved beside (the
    // nearest in our scene, else the lowest number); 0 none.
    std::uint8_t PickBeside(const Input& in) const;
    // Rows 318 + 334: who is nearby now (Teammate::here, kNearbyGraceUs).
    void Notice(const Input& in);
    bool Nearby(const Teammate& t, std::uint64_t now_us) const;
    bool AnyNearby(const Input& in) const;
    // Nobody nearby: our Link to the entrance at once, then up (row 318).
    void ToEntranceAlone(Output& out, std::uint64_t now_us);
    bool Quiet(const Input& in) const;
    proto::DbnoStatus Status() const;
    void Enter(State state, std::uint64_t now_us);

    State state_ = State::up;
    std::uint64_t last_us_ = 0;
    std::uint64_t left_us_ = 0;       // downed: bleed-out time left
    std::uint64_t held_us_ = 0;       // downed: how long the revive has been held
    std::uint8_t reviver_ = 0;
    std::uint64_t alone_us_ = 0;      // downed: how long nobody has been up
    std::uint64_t gone_since_us_ = 0; // since when there has been no Link to read; 0: there is one
    std::uint64_t mercy_until_us_ = 0;
    std::uint64_t out_written_us_ = 0;
    bool out_seen_dead_ = false;
    std::uint8_t watch_ = 0;          // spectating: whom we watch
    std::uint64_t spectate_since_us_ = 0;
    std::uint64_t quiet_us_ = 0;      // spectating: how long no enemy has been near them
    // Spectating: the respawn's move under way (beside, then entrance) and since when.
    enum class Move : std::uint8_t { none, beside, entrance } move_ = Move::none;
    std::uint8_t move_to_ = 0;
    std::uint64_t move_since_us_ = 0;
    std::uint64_t dying_since_us_ = 0;  // up at no life: since when; 0 not
    std::uint64_t alone_retry_us_ = 0;  // spectating alone: the entrance not before this
    std::uint64_t give_up_us_ = 0;      // downed: how long A has been held to give up
    bool give_up_armed_ = false;        // downed: A was let go since we went down
    // Row 330: out by the game's own death while a teammate stood (not the
    // wipe): its Game Over's Try Again loads the last save, maybe far away;
    // once back, our Link is moved beside a teammate.
    bool out_alone_ = false;
    std::array<std::uint64_t, proto::kMaxPlayers + 1> near_us_{};  // last time each was nearby; 0 never
    std::array<std::uint64_t, proto::kMaxPlayers + 1> hold_us_{};  // last hold per player; 0 none
    std::optional<proto::DbnoStatus> sent_;
    std::uint64_t sent_us_ = 0;
    bool tell_again_ = false;
};

// --- The other players' states (DBNO_STATE), for the screen and the reviver ---

class Roster {
public:
    void Set(const proto::DbnoStatus& status, std::uint64_t now_us);
    void Left(std::uint8_t number);
    void Clear();
    State StateOf(std::uint8_t number) const;
    struct Entry {
        proto::DbnoStatus status;
        std::uint64_t at_us = 0;
    };
    // A downed player's entry; nothing for anyone else.
    std::optional<Entry> Downed(std::uint8_t number) const;
    // Bleed-out time left now: what they said, less the time since (it stands
    // still while someone revives them).
    static std::uint64_t LeftUs(const Entry& entry, std::uint64_t now_us);

private:
    std::array<std::optional<Entry>, proto::kMaxPlayers + 1> entries_{};
};

// --- Enemies near our Link (DBNO_THREAT), told while we are up ---

inline constexpr float kThreatReachM = 15.0f;            // an enemy this near (level) counts
inline constexpr std::uint64_t kThreatSendUs = 2'000'000;  // told again this often
// A DBNO_THREAT older than this is unknown (it is sent every kThreatSendUs).
inline constexpr std::uint64_t kThreatStaleUs = 3 * kThreatSendUs;

// Whether any of `foes` stands within kThreatReachM of `self` (level
// distance; at most kReviveHighM * 4 apart in height, so an enemy on another
// floor of a dungeon does not count).
bool AnyNear(const Vec3& self, std::span<const Vec3> foes);

class ThreatTeller {
public:
    // Each frame. `able`: connected, the mode on, we are up and have a Link;
    // else nothing is sent (and the next able frame tells at once).
    std::optional<proto::DbnoThreat> Frame(std::uint64_t now_us, bool able, bool enemy_near);

private:
    std::optional<bool> sent_;
    std::uint64_t sent_us_ = 0;
};

// --- The reviver's side ---

struct Fallen {
    std::uint8_t number = 0;
    std::optional<std::string> scene;
    Vec3 position{};
};

class Reviver {
public:
    struct Out {
        std::optional<proto::DbnoRevive> send;  // player field 0
        std::uint8_t target = 0;                // the downed player in reach; 0 none
        bool holding = false;
    };
    // Each frame. `able`: we are up, have a Link, no event has control and
    // the mode is on. `button`: the revive button is held.
    Out Frame(std::uint64_t now_us, bool able, bool button, const Vec3& self,
              const std::optional<std::string>& scene, std::span<const Fallen> fallen);
    // Whether the revive button is ours and not the game's now (a downed
    // player is in reach): Link must not roll away from them.
    bool TakesButton() const { return target_ != 0; }
    void Reset();

private:
    std::uint8_t target_ = 0;
    bool holding_ = false;
    std::uint64_t sent_us_ = 0;
};

// --- The pad ---

// Switch pad bits (nn::hid::NpadButton) the rules name.
namespace pad {
inline constexpr std::uint64_t a = 1ull << 0;
inline constexpr std::uint64_t plus = 1ull << 10;
inline constexpr std::uint64_t minus = 1ull << 11;
inline constexpr std::uint64_t stick_l_dirs = 0xFull << 16;  // the left stick as four buttons
inline constexpr std::uint64_t stick_r_dirs = 0xFull << 20;
}  // namespace pad

struct Pad {
    std::uint64_t buttons = 0;
    std::array<std::int32_t, 4> stick{};  // left x, y, right x, y
    bool operator==(const Pad&) const = default;
};

// How far a downed Link's stick reaches while the game is not crawling him
// (an adult in the lying pose, a child before the game's crawl runs): a slow
// walk. Issue 337: in the game's own crawl the stick is whole: the crawl's
// speed is the game's (PlayerCrawlMoveRate).
inline constexpr std::int32_t kCrawlStickPercent = 30;

// What the game gets from a downed player: the pause buttons, the camera,
// and the left stick (held to a slow walk unless `game_crawl`). No items, no
// sword, no roll.
Pad DownedPad(const Pad& in, bool game_crawl = false);
// What it gets from a spectating player: the pause buttons and the camera
// stick; Link does not move (A, the switch button, is ours).
Pad SpectatorPad(const Pad& in);
// What it gets from a player beside a downed one: everything but A.
Pad ReviverPad(const Pad& in);

// --- What the screen shows ---

// "0:45", "1:02".
std::string Clock(std::uint64_t left_us);
// "3 hearts", "1/2 heart" for the rules line.
std::string RulesLine(const Rules& rules, std::uint8_t self, std::string_view by_name);
// A chat notice for our own event; nothing for one that needs none.
std::optional<std::string> OwnLine(Event event, std::string_view by_name);
// A chat notice for another player's change of state: "Saria is down",
// "Mido revived Saria", "Saria is out". Nothing when it is no news.
std::optional<std::string> TheirLine(const std::optional<proto::DbnoStatus>& before,
                                     const proto::DbnoStatus& now, std::string_view name,
                                     std::string_view reviver_name);

}  // namespace CoopDbno
