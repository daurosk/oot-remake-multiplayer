// SPDX-License-Identifier: GPL-3.0-or-later
//
// Row 98(2) result only (owner 07:40 UTC 2026-11-17): a room gimmick event another player played is never replayed in
// our game (a replay binds our Link: "movement stuck"); our room gets its RESULT only. Story scenes keep the replay
// (coop_shared_scenes.h).
//
// Where an event's result lives, read from the romfs room graphs by tools/puppet_pack/eye_switches.py --results (every
// asker, EventRequestActor or EventProxyRequestActor, of every event with a live place, coop_live_places.cpp):
// - class live: every room result of the asker reaches our room through a shared flag the room graph reads live, so
//   the other player's flags landing IS the result. Such an event's played flag is not held at all
//   (coop_dungeon.h HeldIn; it replaces the hand-made list of test 6: the Deku basement water and torches).
// - class asker: a result hangs on our asker's own outputs (EventBool, EventBoolEX1/EX2, EventFinish: what the event
//   flow's EventTriggerSetEventBool sets): an actor input, an unshared flag, or a flag the room reads only at its load
//   (UpdateOnce: the Deku ladder's RoomC_Cleared). Our asker's outputs are set with the game's own set-bool
//   (ExecuteLogicSetBool, coop_logic_set.h; the eye switch path's shape, coop_eye_switch.h), one output at a time in
//   table order, each until it reads back 1 for kApplyFrames frames. Then the event's held flags land (Hold below).
//   Asker not found, or an output that never stays 1 within kTryMs: the flags stay held until we leave the dungeon or
//   load (the safe floor of issue 129), one log line.
// - performer plays (row 165): an event flow's EventPlayAS on a room actor whose Logic_Delete reads the event's played
//   state (Dodongo's broken stair, the Forest Temple's box puzzle boxes, Ganon's Castle's lift rock). The room swaps
//   that actor for its end actor only when it loads with the event played, so the other player's played flag changes
//   nothing in our loaded room (run hs-dc-break: the up stair stayed): the event's own AS on its performer is the live
//   result. Such an asker is class asker: our performer (the one of its name at its placement) plays that AS through
//   the game's AS play request (0x2022a0, as the object sync's copies), once each, before our asker's outputs are set.
//   A performer not in our room (already its end state, or moved away) is skipped with a log line.
// - result guards: a shared flag that deletes a class-asker asker (Logic_Delete, read live). The other player's game
//   sets it during or at the end of its event; landing first, it would delete our asker before our result is set (the
//   Deku ladder never fell). It waits in its scene until our result is set this visit or our game played the event.
//
// Only in a session (the hold and the decider exist only there); nothing runs with co-op idle.
// No emulator code: tests/lichii/event_result_test.cpp.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop_eye_switch.h"
#include "coop_puppet.h"

namespace CoopEventResult {

// One asker of an event (generated). `outputs` is "" for class live, else the outputs to set joined by '+'.
struct Result {
    std::string_view event;  // without "HavePlayedEvent."
    std::string_view scene;
    std::array<float, 3> at;  // the asker's placement (x, height, z); 0,0,0 for an event a field asks (class live)
    std::string_view outputs;
};
std::span<const Result> Results();

// One performer's AS of an event (generated, row 165).
struct Play {
    std::string_view event;
    std::string_view scene;
    std::string_view actor;  // the performer's name
    std::array<float, 3> at; // its placement
    std::string_view as;     // the AS the event plays on it
};
std::span<const Play> Plays();
// The plays of `event` in `scene`, in table order.
std::vector<const Play*> PlaysOf(std::string_view event, std::string_view scene);
// A performer is the actor of its name within this of its placement.
inline constexpr float kPerformerAtM = 1.5f;
// The longest AS name a play cell holds (with its 0).
inline constexpr std::size_t kPlayNameCell = 0x40;
// Play requests per performer at most (the AS did not show in its slot 0 after the first).
inline constexpr int kMaxPlayCalls = 2;

struct Guard {
    std::string_view flag;
    std::string_view scene;
    std::string_view event;
};
std::span<const Guard> Guards();

// Every class-asker line of `event` in `scene` (outputs to set, or plays of the event there), nearest asker place to
// `link` first (table order without `link`).
std::vector<const Result*> AskerLines(std::string_view event, std::string_view scene,
                                      const std::optional<std::array<float, 3>>& link = std::nullopt);
// Whether every asker of `event` is class live (and it has at least one line): its played flag is not held.
bool IsLive(std::string_view event);
// Row 336: a class-asker event that is class asker ONLY for a shutter's lock (eye_switches.py lock_only; class live in
// 0.12.0). Its set has a bounded fallback: given up (not set, or not set within kLockFallbackMs of another player's
// played flag waiting here), its played flag and result guards land as a class-live event's did, never worse.
bool IsLockOnly(std::string_view event);
inline constexpr std::uint64_t kLockFallbackMs = 10000;
// The names in `outputs` ("EventBool+EventFinish"), in order.
std::vector<std::string_view> SplitOutputs(std::string_view outputs);

// Trigger guards (review-a38-5): the game's OWN asker of an event asks it when its EventRequestTrigger rises, and
// shared state can raise it: the other player's game plays the event and its state comes here (its shared flags, its
// keys, its kills), our asker asks the event too = a replay that binds our Link. Generated from every asker's trigger
// leaves (both classes; eye_switches.py --results, which also prints why every other leaf cannot rise from shared
// state). Each waits in its scene until our game has played the event (`HavePlayedEvent.<event>`: our asker is then
// disarmed; for class asker it lands once our result is set, for class live it lands live).
// - flag: a shared flag the trigger reads (not negated).
// - key: a trigger actor's shared placement key (a silver rupee counter's own key, an ice block's).
// - token: a key the counter `group` counts (its tokens). A split collection still works: only the LARGEST token
//   still missing here waits, the others land, so shared keys alone never complete our counter (no same-frame race:
//   the one that waits does not depend on which arrive). Our Link taking that token itself completes it: our own play.
// - kill: an actor whose death (Dead, IsDeadNotDelay, IsDeathEventPlay, a negated Logic_Alive) the trigger reads; the
//   object sync's kill or remove of the LARGEST live one of the event waits (KillKeepers), the others die.
// - follow: a world object coop_world follows (review of 70ba0763): the trigger actor itself (a hammer switch's press,
//   a hammer pillar's fall, a statue's or block's stop), a block, statue or rolling rock that can rest on a press
//   switch the trigger reads (one in the switch's room: each waits), or a block a trigger actor links (`group` = that
//   actor's placement hash: the Forest Temple box puzzle's four corners: only the LARGEST waits, the others land, so
//   another game's pushes alone never complete it; our Link pushing it is our own play). Another game's move, rest
//   place and fall of a keeper wait in coop_world (FollowKeepers) and land once our game has played the event.
struct Trigger {
    enum Kind : std::uint8_t { flag, key, token, kill, follow };
    Kind kind;
    std::string_view scene;
    std::string_view event;
    std::string_view name;  // kind flag: the flag's name
    std::uint64_t id;       // key, token: the placement key; kill, follow: the actor's placement hash
    std::uint64_t group;    // token: its counter's key; follow: the linking actor's hash (0: this one waits)
};
std::span<const Trigger> Triggers();

// What the flag hold does this frame (pure): `released` lands although held otherwise (the played flag, the result
// guards and the eye switch guards of every event whose result we set this visit); `waiting` is held (the result
// guards of `scene` whose event's result is not set here this visit and that our game has not played; the flag
// triggers of `scene` whose event our game has not played); `waiting_keys` are placement keys held (the key triggers
// and each counter's largest missing token, by `have_key`: our game's value of a key; none given: every token counts
// as missing).
struct Hold {
    std::vector<std::uint32_t> released;
    std::vector<std::uint32_t> waiting;
    std::vector<std::uint64_t> waiting_keys;
    bool Releases(std::uint32_t hash) const;
    bool Waits(std::uint32_t hash) const;
    bool WaitsKey(std::uint64_t key) const;
};
// `given_up` (row 336, Setter::GivenUp): lock-only events whose set was given up this visit: released like `applied`.
Hold HoldFor(std::string_view scene, const std::set<std::string>& applied,
             const std::function<bool(const std::string&)>& played,
             const std::function<bool(std::uint64_t)>& have_key = nullptr,
             const std::set<std::string>& given_up = {});

// The kill triggers' keepers in `scene` (pure): for each event our game has not played, the largest placement hash
// of its kill actors that `alive` says is alive in our walk and that our game does not own (`ours`). The object sync
// keeps a kill or remove of a keeper until it is no keeper any more (the event played here, or a scene change).
// Rows 160/161 (dc-doors-0118, Dodongo's Cavern's second Lizalfos room, 0.11.6): each enemy has one owner, whose game
// alone decides its death (a mid-boss copy is held at life 1 until its owner's dies). With the two Lizalfos owned by
// two games, each game kept the other owner's kill (the largest live one was always a copy there), so no game had both
// dead, nobody played the event and the room stayed shut for everyone. So: a kill of an enemy our game owns is never
// kept (it is the group's death of it: the remote player's last hit, landed through our game), and while one of the
// event's live kill actors is ours, no copy is the last one here, so nothing of that event is kept. The game that
// owns the event's last enemy plays the event; every other game keeps that one kill until the event's flags come.
std::set<std::uint64_t> KillKeepers(std::string_view scene, const std::function<bool(std::uint64_t)>& alive,
                                    const std::function<bool(const std::string&)>& played,
                                    const std::function<bool(std::uint64_t)>& ours = nullptr);

// Crossing last kills (review-dc-0118): two owners each kill their own last enemy of an event within one round trip,
// so each game has no owned enemy alive when the other's kill lands, and both keep it (nobody plays the event). The
// tiebreak (pure): another game's kill of `keeper` kept here for `kept_for_us` lands anyway once it was kept
// kCrossingUs, if one of the same event's kill actors that our game owned died here within kCrossingGapUs of that
// kill being kept (`ours_died`, the caller's check: a crossing, not one kill long after the other) and our player
// number is below the keeper's owner's. So exactly one of the crossing owners (the lowest) plays the event and the
// others get its flags; a game that owned none of them, or whose own enemy died long before, still only waits (in a
// kill after kill the last enemy's owner plays it). kCrossingUs is longer than a room event plays, so the playing
// game's flags have come by then in every case that is no crossing.
// The owner gone (`owner_here` false: its game no longer in our scene, or out of the session, by its samples): it
// cannot play the event any more, so after kCrossingUs the kill lands in the one game that decides for our scene
// (`we_decide`: Sync::DeciderOf, the lowest player number in it), whatever it owned; the others get its flags.
// The cost: in a crossing (or with the owner gone) the shutters open kCrossingUs late, in the deciding game first and
// in the others when its flags come. Before, they never opened until a reload.
// `keeper_owner` is Sync::OwnerOrDeathOwnerOf: the dead ObjectRemove that brings such a kill forgets the owner.
inline constexpr std::uint64_t kCrossingUs = 15'000'000;
inline constexpr std::uint64_t kCrossingGapUs = 2'000'000;
bool CrossingKillLands(std::string_view scene, std::uint64_t keeper, std::uint64_t kept_for_us, std::uint8_t self,
                       std::optional<std::uint8_t> keeper_owner, const std::function<bool(std::uint64_t)>& ours_died,
                       bool owner_here = true, bool we_decide = false);

// Row 160 (dungeon-sweep-0118-8, run 20261120-132403): a kept kill's owner whose Link is downed, spectating or out
// (DBNO), dead or in a load (a void-out's respawn, a Try Again: PLAYER_STATUS dead / loading) cannot play the event
// either, though its samples still say our scene. Before, such an owner counted as 'here', so the other games kept its
// kill and waited for an event nobody could play. `in_our_scene`: Sync::InOurScene; `link_up`: the player's DBNO state
// is up (always so with DBNO off); `status`: the player's PLAYER_STATUS flags. A cutscene (the room event itself) or a
// pause keeps the owner able: it plays the event once it is back.
bool AbleToPlay(bool in_our_scene, bool link_up, std::uint8_t status);
// The one game that decides for our scene among those that can play an event (pure): the lowest player number in
// `in_scene` (`self` included) that `able` says can play; 0 when none can (nobody lands a kept kill then: the first
// game whose Link is up again does).
std::uint8_t DeciderAmongAble(std::uint8_t self, std::span<const std::uint8_t> in_scene,
                              const std::function<bool(std::uint8_t)>& able);

// Row 160 (run 20261120-132403: our Link revived by a reload, which re-made the room): the kill actors of a room event
// to kill again once re-made in the same scene visit (pure). For each kill event in `scene` our game has not played,
// if every one of its kill actors is in `dead` (seen dead here in this visit) or `kept` (another game's kill kept
// here) and at least one is in `kept`, all of them: the group killed the whole room, so the re-made room is clear too
// and our game plays its event. An event with any kill actor never killed is left as the game made it (a native
// refight of the room), and so is one that only our own game killed (no kept kill): a solo game or a host alone in
// the room (co-op idle) that reloads before its event played fights the room again, as the game does.
std::set<std::uint64_t> KillsToRedo(std::string_view scene, const std::set<std::uint64_t>& dead,
                                    const std::set<std::uint64_t>& kept,
                                    const std::function<bool(const std::string&)>& played);
// The room event whose kill actor in `scene` is `hash` (pure); none: no kill line names it.
std::optional<std::string_view> KillEventOf(std::string_view scene, std::uint64_t hash);

// Row 160 (b) (run 20261120-132403): a kept kill's copy (another game killed it, the kill waits here for the event)
// played its owner's death and then stood at full life, and our Link could hit it again. While its kill is kept its
// Life holds our key in the game's own no-reaction invincibility (main+0x8d75e0, as our downed Link's: nothing is
// taken, no reaction), and the key comes out before the kill lands. Which calls to make (pure): `want` the Life
// components of the kept kills' actors now, `live` every Life component in our walk (a key is only taken out of a
// component that is still there; one that went took our key with it).
inline constexpr std::uint32_t kKeptShieldKey = 0xC0DB0D03u;  // ours (dbno ...01, PvP copies ...02)
inline constexpr unsigned kKeptShieldTries = 3;               // calls per component and way before it is given up
inline constexpr std::uint64_t kKeptShieldWaitUs = 1'000'000; // a landing kill waits this long for the key's removal
class KeptShield {
public:
    struct Step {
        std::uint64_t life = 0;
        bool on = false;  // add our key (true) or take it out
        bool operator==(const Step&) const = default;
    };
    std::optional<Step> Next(const std::set<std::uint64_t>& want, const std::set<std::uint64_t>& live);
    void Called(const Step& s, bool ok);
    bool On(std::uint64_t life) const { return on_.contains(life); }
    bool GivenUp(const Step& s) const;
    void Reset() {
        on_.clear();
        tries_.clear();
    }

private:
    std::set<std::uint64_t> on_;
    std::map<std::pair<std::uint64_t, bool>, unsigned> tries_;
};

// The follow triggers' keepers in `scene` (pure): for each event our game has not played, its follow actors of group
// 0 and the largest of each linked group. coop_world (WorldSync::SetTriggerKeepers) keeps another game's move, rest
// place and fall of a keeper until it is no keeper any more (the event played here, or a scene change).
std::set<std::uint64_t> FollowKeepers(std::string_view scene, const std::function<bool(const std::string&)>& played);

// Frames an output must read back 1 (after our set, or already) to count as set; and how long one event is tried.
inline constexpr int kApplyFrames = 3;
inline constexpr std::uint64_t kTryMs = 3000;

// The svc caller: one event's result at a time.
class Setter {
public:
    explicit Setter(CoopEyeSwitch::Finder find = CoopEyeSwitch::FindActors) : find_(std::move(find)) {}
    // The hook thread: set our asker's result of `event` in `scene`; `link` picks the nearest asker. False: no class
    // asker line (class live: its flags are the whole result; nothing to set).
    bool Want(const std::string& event, const std::string& scene,
              std::optional<std::array<float, 3>> link = std::nullopt);
    // The hook thread, once a frame: a new scene is a new visit (nothing set yet); a wanted event of another scene, or
    // one `played(event)` says our game has played, is dropped.
    void Visit(const std::string& scene, const std::function<bool(const std::string&)>& played = nullptr);
    // The events whose result was set this visit.
    std::set<std::string> Applied();
    // Row 336 (pure but for the clock): the lock-only events (IsLockOnly) of this visit whose set is given up: ended
    // without a set, or `held` (their played flag waits here: another player played them) for kLockFallbackMs from the
    // first frame seen held without a set. Each is noted once ("its flags land as a class-live event's").
    std::set<std::string> GivenUp(const std::vector<std::string>& held, std::uint64_t now_ms);
    // The calc thread (CoopSvc caller contract).
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t main, std::uint64_t now_ms);
    std::vector<std::string> TakeNotes();

private:
    struct Try {
        std::string event;
        std::string scene;
        std::optional<std::array<float, 3>> link;
        std::uint64_t until_ms = 0;  // 0: not begun
        std::uint64_t asker = 0;     // our asker, once found
        std::size_t output = 0;      // the output being set
        int ones = 0;                // frames it read 1 in a row
        int sets = 0;                // our set-bools of this output
        int zeros_after_set = 0;     // frames it read 0 after one of our sets (something rewrites it)
        std::size_t play = 0;        // the performer play being done (PlaysOf order), before the outputs
        int play_calls = 0;          // our play requests of it
        int play_ones = 0;           // frames its slot 0 showed the AS in a row
    };
    std::optional<CoopPuppet::Call> PlayStep(CoopPuppet::Guest& guest, Try& t, std::uint64_t main);
    std::optional<CoopPuppet::Call> Frame(CoopPuppet::Guest& guest, std::uint64_t main, std::uint64_t now_ms);
    void End(const std::string& why, bool applied);
    static bool EventRunning(CoopPuppet::Guest& guest, std::uint64_t main);

    CoopEyeSwitch::Finder find_;
    std::mutex mutex_;
    std::deque<Try> wanted_;  // front: the one being set
    std::vector<std::string> notes_;
    std::string scene_;
    std::set<std::string> applied_;  // this visit
    std::set<std::string> ended_;    // this visit: tried and not set (asked again on the next visit)
    std::map<std::string, std::uint64_t> held_since_;  // this visit: a lock-only event first seen held (row 336)
    std::set<std::string> given_up_;                   // this visit: lock-only events given up (row 336)
    std::uint64_t block_ = 0;
    enum class Next { none, malloc, execute, play };
    Next next_ = Next::none;
    bool failed_ = false;  // the malloc gave nothing: nothing is set (the flags stay held)
};

}  // namespace CoopEventResult
