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
// - Bled out, or nobody left standing: out. The game's own death runs then,
//   as without co-op (a fairy in a bottle still saves Link there).
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
    std::uint8_t revive_tenths = 30;  // how long a revive is held
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
    off,         // the game's own rules (mode off, nobody to revive us, or out)
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

// Another player as presence and their DBNO_STATE show them.
struct Teammate {
    std::uint8_t number = 0;
    State state = State::up;
    bool in_game = false;             // has a Link now (not on the title, not loading)
    std::optional<std::string> scene;
    std::optional<Vec3> position;     // at the feet, as our game shows them
};

// Whether `them` at `at` is near enough to `self` to revive, `slack` times
// the reach.
bool InReach(const Vec3& self, const Vec3& at, float slack = 1.0f);

struct Input {
    std::uint64_t now_us = 0;
    Rules rules;
    bool connected = false;
    bool guard_live = false;              // LifeGuard::Live
    bool lethal = false;                  // LifeGuard::TakeLethal
    std::optional<std::int32_t> life;     // our Link's life, quarter hearts; nothing: no Link to read
    std::optional<std::int32_t> max_life;
    std::optional<std::string> scene;
    Vec3 position{};
    std::span<const Teammate> team;
};

enum class Event : std::uint8_t {
    went_down,
    revived,     // by `Output::by`
    bled_out,
    wiped,       // nobody was left standing
    died,        // the game took the life anyway (a pit, a script)
    got_up,      // the host turned the mode off
    back,        // alive again after out (a fairy, a continue)
};

struct Output {
    State state = State::up;
    Guard guard = Guard::off;
    std::optional<std::int32_t> write_life;      // set our Link's life to this now
    std::optional<proto::DbnoStatus> send;       // to everyone (player field 0)
    std::vector<Event> events;
    std::uint8_t by = 0;                         // revived: who did it
    // For the screen, while downed.
    std::uint64_t left_us = 0;
    std::uint8_t reviver = 0;
    float progress = 0.0f;                       // 0 to 1
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

// How far a downed Link's stick reaches: a slow walk under the crawl.
inline constexpr std::int32_t kCrawlStickPercent = 30;

// What the game gets from a downed player: the pause buttons, the camera,
// and the left stick held to a slow walk. No items, no sword, no roll.
Pad DownedPad(const Pad& in);
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
