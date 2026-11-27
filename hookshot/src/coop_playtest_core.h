// SPDX-License-Identifier: GPL-3.0-or-later
// The play-test bridge's game-free part (docs/playtest.md, "Bridge protocol"): the frame hold, the requests'
// checks and the stick's numbers. coop_playtest.cpp puts it in the game (the socket, Longshot's control bridge,
// the module's test commands); the unit tests (tests/hookshot/playtest_test.cpp) drive it with plain threads.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop_mod_json.h"
#include "coop_rooms.h"
#include "coop_strike.h"
#include "coop_world_snapshot.h"

namespace CoopPlaytest {

inline constexpr int kProtocol = 1;
inline constexpr std::size_t kMaxLine = 64 * 1024;
inline constexpr std::uint32_t kMaxFrames = 3600;   // step, input
inline constexpr double kDefaultIdleS = 600.0;      // a hold with no command this long ends by itself

// LICHII_COOP_PLAYTEST_PORT: 1024 to 65535; anything else (unset, empty, 0, words) is off.
std::optional<std::uint16_t> PortFrom(std::string_view value);
// LICHII_COOP_PLAYTEST_IDLE: seconds (0 never); unset or bad: the default.
double IdleFrom(std::string_view value);
// A dump's tag: 1 to 64 of letters, digits, '-' and '_' (it names a file in the dump folder: no path).
bool ValidTag(std::string_view tag);
// The bridge `key`'s name as the Windows virtual key the window gives hos_coop_key (WM_KEYDOWN's wParam, which Longshot
// passes as it is: ls32-src/host/display.c; Linux maps SDL's keys to the same codes: host/posix/display_sdl.c): F1 to F12
// = 0x70 to 0x7B, A to Z and 0 to 9 = their upper-case ASCII codes. Any other name: none.
std::optional<std::uint32_t> KeyVk(std::string_view name);

// The game's frame hold. The game's calc thread calls AtFrame at the top of every frame hook; the bridge's thread
// calls the rest. While held, AtFrame waits (the frame's update has not run yet) and runs the jobs the bridge
// hands it there, on the game's thread.
class Gate {
public:
    using Clock = std::chrono::steady_clock;
    explicit Gate(double idle_s = kDefaultIdleS) : idle_s_(idle_s) {}

    // (calc thread) One frame hook: counts the frame, and holds while asked.
    void AtFrame();

    std::uint64_t Frame() const;
    bool Held() const;     // the calc thread waits in AtFrame now
    bool Holding() const;  // a hold is asked (held now, or at the next frame)

    // Hold at the next frame; true once held (false: no frame came in time: a load, the game closed).
    bool Pause(std::chrono::milliseconds timeout);
    // Run in real time again (also: the bridge's client left; the idle time ran out).
    void Resume();
    // Let exactly `frames` frames run, then hold. Holds first when running. `from`: the frame it started at.
    bool Step(std::uint32_t frames, std::chrono::milliseconds timeout, std::uint64_t* from = nullptr);
    // Run `job` on the calc thread inside the hold (holds first). False: not held in time (the job is dropped).
    bool RunHeld(std::function<void()> job, std::chrono::milliseconds timeout);
    // A command came: the idle time starts again.
    void Touch();
    // How the last hold ended by itself, once (for the log): "idle" or empty.
    std::string TakeEndNote();
    // Called with true when a hold starts and false when it ends, on the calc thread under the gate's lock: it must
    // not block or call the gate (the module passes it to co-op's network thread: CoopHook::SetHeld). Set before
    // the first frame.
    void OnHeld(std::function<void(bool)> on_held) { on_held_ = std::move(on_held); }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::uint64_t frame_ = 0;
    std::uint32_t budget_ = 0;  // frames still to run in a step
    bool want_hold_ = false;
    bool held_ = false;
    std::deque<std::function<void()>*> jobs_;
    Clock::time_point touched_ = Clock::now();
    double idle_s_;
    std::string end_note_;
    std::function<void(bool)> on_held_;
};

// The stick: angle in degrees (0 up, 90 right, clockwise), strength 0 to 1, as Longshot's axes (-1 to 1, y up).
struct Axes {
    double x = 0;
    double y = 0;
};
Axes StickAxes(double angle_deg, double strength);

// The left stick that walks Link toward world yaw `yaw_deg` (degrees, 0 toward +z, 90 toward +x: the state's `pos` and
// `facing` convention) at `strength` (0 to 1), for the game's camera `clip_from_world` (row-major, clip = m * p, as
// CoopCamera hands it). Stick up is the camera's forward on the ground (the way clip w grows: depth into the picture)
// and stick right is the picture's right (the way clip x grows), both read from the matrix, so no handedness, yaw
// offset or mirror is assumed. nullopt when the matrix gives no ground direction (a camera looking straight down, a
// broken matrix).
std::optional<Axes> MoveAxes(const std::array<float, 16>& clip_from_world, double yaw_deg, double strength);
// The camera's yaw on the ground from the same matrix (degrees in [-180, 180), the state's convention), for replies.
std::optional<double> ViewYaw(const std::array<float, 16>& clip_from_world);

// A `move` in progress: the bridge thread starts and stops it, the game's input thread asks it at each pad read for
// the stick to put in (hos_coop_pad), from the camera of the last drawn frame. Each read turns the world yaw into the
// stick again, so a camera that turns while Link walks is followed on the next read.
class MoveHold {
   public:
    struct Counts {
        std::uint32_t reads = 0;      // pad reads while the move was on
        std::uint32_t no_camera = 0;  // of those, reads with no usable camera: the stick was centred
        std::optional<Axes> last;     // the last stick put in
        std::optional<double> view;   // the camera yaw that stick came from
    };
    void Start(double yaw_deg, double strength);
    Counts Stop();  // ends the move and gives what it did
    bool On() const;
    // The input thread: false when no move is on (the stick is left as it came). On: the stick's left axes
    // (stick[0], stick[1], -32767 to 32767, y up) from `camera`, or centred when there is none.
    bool Apply(const std::optional<std::array<float, 16>>& camera, std::int32_t stick[4]);

   private:
    mutable std::mutex mu_;
    std::atomic<bool> on_{false};  // read without the lock first: the pad hook runs at every pad read
    double yaw_ = 0, strength_ = 0;
    Counts counts_;
};

// A checked request.
struct Request {
    std::string cmd;
    CoopModJson::Value id;  // (null when none)
    std::uint32_t frames = 1;
    std::vector<std::string> buttons;
    std::optional<Axes> stick, rstick;
    double yaw = 0, strength = 1;  // move: world yaw in degrees, stick strength 0 to 1
    bool exclusive = true;
    std::string scene, entrance, name, item, tag, value;
    std::string lines;  // act: one test-script line (kill, hit, hurt, place, ...); PlayNow checks its kind
    std::uint64_t since = 0;  // text: only the texts after this seq (0: all kept)
    std::string layout;       // text: only the layouts whose name has this in it ("": every layout)
    std::string key;          // key: the host key's name (F1, T, ...) and its virtual key (KeyVk)
    std::uint32_t vk = 0;
    std::string chars;        // key: typed characters in place of a key (hos_coop_char's steps, one per byte)
    double timeout_s = 60;
};
// The request on one line, or `error` (bad_request's message).
std::optional<Request> ParseRequest(std::string_view line, std::string& error);

// Longshot's sequence request for an input (one step: the buttons and both sticks, `frames` frames, from the next
// controller read), not waited on: the bridge steps the frames itself.
std::string ControlInput(const Request& r, int control_id);

// The reply's first fields: id, ok, frame, paused; `rest` (a JSON object's members, may be empty) after them.
std::string Reply(const CoopModJson::Value& id, std::uint64_t frame, bool paused, const std::string& rest);
// An error answer always names a reason: an empty `message` (a line the game refused without a word, or Longshot's
// error with no message) becomes a sentence that says so and names the code.
std::string ErrorReply(const CoopModJson::Value& id, std::uint64_t frame, bool paused, std::string_view code,
                       std::string_view message);
// The reason for a failed Longshot teleport: its code and message, never just the code (an empty message says that
// Longshot gave none, and where else to look).
std::string TeleportFailure(std::string_view code, std::string_view message);
// A JSON string literal.
std::string Quote(std::string_view text);
// A value as JSON on one line, no spaces (CoopModJson::Write is for files: it indents objects and ends with a
// newline, which would split a reply in two).
std::string Compact(const CoopModJson::Value& value);

// `flag_read`: one flag's value, read only (docs/playtest.md). What the game held, as the module read it.
struct FlagValue {
    std::string kind = "unknown";        // int, bool, enum, uint, key64, text, unknown (no such flag)
    std::optional<std::int64_t> number;  // int, bool and key64 (0 or 1), uint
    std::optional<std::uint32_t> hash;   // enum: the flag_hash of the value's name
    std::string text;                    // text
    std::optional<std::int32_t> live;    // int: the table value without a write still waiting in the queue
};
// The reply's members for a read: "name", "kind", "value" (a number; for an enum the value's hash as "0x" and 8 hex
// digits, the form `flag` takes back; text as a string; null when unknown) and, for an int, "pending" (true while a
// write waits in the game's queue: `value` is that write, the table still holds the old one).
std::string FlagReadMembers(std::string_view name, const FlagValue& v);
// An enum value's hash as `flag_read` gives it ("0x" and 8 lowercase hex digits).
std::string HashText(std::uint32_t hash);
// "0x" and 1 to 8 hex digits, not 0: the hash. Anything else: nothing.
std::optional<std::uint32_t> HashValue(std::string_view text);
// Whether a `flag` value (ParseRequest's text) is a whole number: then it is written with setflag; a "0x" hash goes to
// an Enum (EnumValues); any other text is an enum value's name (setenum).
bool IsWholeNumber(std::string_view value);
// The Enum values `flag_read` has read, by flag name as asked. `flag` writes a "0x" value hash only when it is one of
// these for that same flag: a value the game itself held in this run, so no raw number reaches an Enum by mistake.
class EnumValues {
   public:
    void Remember(std::string_view name, std::uint32_t hash);
    bool Known(std::string_view name, std::uint32_t hash) const;

   private:
    mutable std::mutex mu_;
    std::vector<std::pair<std::string, std::uint32_t>> seen_;
};
// Why a `flag` write of a "0x" value is refused ("" when it may go): not a hash, or not one `flag_read` read from this
// flag.
std::string EnumHashRefusal(const EnumValues& known, std::string_view name, std::string_view value);

// The commands that change the game and reach the wire: act, warp, flag and give. state, dump, shot, input, step, pause,
// resume and flag_read are not gated (flag_read writes nothing; it exists only where the bridge does: a play-test build
// with LICHII_COOP_PLAYTEST_PORT set).
bool GatedCommand(std::string_view cmd);
// The gated commands run only when the test runner asked for them (LICHII_COOP_PLAYTEST_VERBS, set by tools/autotest
// next to the bridge port and by nothing a player starts: not play.ps1, start.sh or coop-options.ini) and cheats are on
// (LICHII_COOP_CHEATS). OOT_COOP_KIT_START is no sign of a test: the kit's start files set it for every player's game,
// and so is a loopback server (a real host's game connects to its own server at 127.0.0.1). Empty when allowed, else
// the reason (the error reply's text).
std::string VerbRefusal(bool runner, bool cheats);
// The line the bridge logs at start: "verbs on|off (test runner: yes|no, cheats: on|off)".
std::string VerbGateLine(bool runner, bool cheats);
// Whether an environment value counts as on (set, not empty, not "0").
bool EnvIsOn(std::string_view value);

// Longshot's Link snapshot (the control bridge's `status`, `game`) is taken only in a running Link update while a status
// is asked; a held game keeps the old one. `status` is the whole reply: its `frame` is Longshot's present count now,
// `game.frame` the present count the snapshot was taken at. Old: the snapshot is more than kSnapshotSlack presents behind
// (a guess: a running game takes it within a present or two; not tuned in a game yet). No `game.frame` (no sample was
// ever taken: only the typed event, loading and age) is not old.
inline constexpr double kSnapshotSlack = 4;
bool SnapshotIsOld(const CoopModJson::Value* status);
// Whether a reply has no Link sample at all: `status` is there, and its `game` is null, or has no `frame` (Longshot
// takes a sample only when a status is asked in a running Link update, so a game held since its start has none yet).
// Not the same as old: a sample that never came cannot be told from "behind" by its frame. A reply that is itself
// absent is not missing a sample (there is no reply: `control` says so).
bool SnapshotMissing(const CoopModJson::Value* status);
// The reason the Link members are null, "" when they are not (a fresh sample is in the reply): no reply, no sample
// yet, or an old sample. Shown as `not_ready` in the state, so a tester never reads a silent null.
std::string NotReadyReason(const CoopModJson::Value* status);
// The state's Link members from that reply, each with a leading comma: pos, facing, camera, life, max_life, rupees,
// age, event, loading, prompt, snapshot_present, snapshot_stale and not_ready (null, or the reason the members are null:
// NotReadyReason). An old snapshot shows null in every Link member (a
// walk that trusts a stale pos says "arrived" 11 m away) and `"snapshot_stale":true`; `snapshot_present` keeps its number.
// No reply (`status` null) or no `game`: all null, `snapshot_stale` false (`control` says why).
std::string LinkMembers(const CoopModJson::Value* status);
// The state's `room` member (leading comma): the dungeon room our Link stands in, guessed from the rooms table
// (coop_rooms.h: the puppet pack's coop_rooms.txt, the same guess the Fairy and the joiner use) at the Link sample's
// `pos`. null when the sample is old or missing, the scene has no rooms (not a dungeon, or no pack) or no placement of
// it is near (CoopRooms::kFar).
std::string RoomMember(const CoopModJson::Value* status, std::string_view scene, const CoopRooms::Table& rooms);
// The state's `view` member (leading comma): the game's camera of the last drawn frame, so a tester can aim the way a
// player does (turn the view until the target sits under the reticle). {"yaw": degrees (the `facing` convention),
// "pitch": degrees up (negative: looking down), "clip": the 16 numbers of clip_from_world, row-major, clip = m * [x y
// z 1]: a point's place on the screen is x/w, y/w, "y_down": true when +y/w is lower on the screen (Vulkan)}. null with
// no camera (a title, a load) or one with no forward.
std::string ViewMember(const std::optional<std::array<float, 16>>& clip_from_world, bool y_down = false);
// The state's `body` member (leading comma): "child" or "adult" from the name of our own Link actor in the world list
// (Player_Child, Player_Adult; the others are CoopPuppet_). It follows the actor the game made, so it is right at once
// after an area change. null without a live list (`live` false) or without our Link in it (a load).
std::string BodyMember(const std::optional<CoopWorldSnapshot::Snapshot>& snapshot, bool live);
// Why a `give` is refused now, "" when it may go. Refused: a load runs (`game.loading` true), or the Link sample is old
// or missing (NotReadyReason: no Link update ran, as in a load, a pause or a title, so `loading` cannot be trusted: a
// load sends no Link update, so the sample taken before it still says `loading` false). The write would be made, and a
// load would put the save's values back over it. Give it the status LinkStatus() read (it ran one frame if the sample
// was old). No reply at all (no control bridge) is not a refusal: nothing can be told, and the write is safe there.
std::string GiveRefusal(const CoopModJson::Value* status);

// The read-back of a `give` or a `flag` write (give-life-fix, 2026-11-26): a value the game keeps elsewhere (our Link's
// Life component, of which PlayerStatus.Life/MaxLife are only the mirror) is put back by the game's next frame, and the
// write looked done while it was lost. Each flag the call wrote is read before it, right after it, and after
// kStickFrames frames. One "did not stick" when the write changed the value and those frames put the old one back
// (a value the game moved on to a third value is its own use, not a lost write); "" when every write kept.
inline constexpr int kStickFrames = 2;
struct StickRead {
    std::string name;
    std::optional<std::int32_t> before, written, after;
};
std::string StickProblem(const std::vector<StickRead>& reads, int frames = kStickFrames);

// `warp`: two ways to change scene. While an event runs, the module's own request (`Sequence_ReservedNextBanc`, the test
// script's `warp`; the game takes it only then, docs/scenes.md "Free-play scene change"). In free play, Longshot's control
// bridge `teleport` (the Cheats page's destination list; the game takes it at Link's next update; the way hookshot.py's
// boot-script warps go), which knows only its own list of maps and has no start position (`entrance`). A third way
// (`map_event`, step 2 of kit-setup): in free play with an `entrance`, or a map Longshot's list lacks (a boss stage), the
// module asks the game's own map change event (CoopTeleport::MapChange, as an entrance does it).
enum class WarpRoute { refuse, event, teleport, map_event };
struct WarpPlan {
    WarpRoute route = WarpRoute::refuse;
    std::string code;  // for `refuse`: the bridge's error code (not_in_play, not_in_event)
    std::string why;   // for `refuse`: the text
    std::string set_age;  // "child" or "adult": set Sequence_IsChildhood and let frames run before the warp; else empty
};
// Which way, from `status` (a reply with a fresh Link snapshot: an old or missing one cannot say, so it refuses), whether
// the caller named an `entrance`, and the `scene` asked: an event runs -> event; none and no entrance -> teleport; none
// and an entrance -> refuse (the teleport has no start position, the event-only request does). Longshot's teleport picks
// the map for Link's current age, so in free play a `scene` ending in the other age (`_Child`/`_Adult`, any case)
// goes by the map change event with `set_age` filled (the caller sets the age flag and runs frames first); the event
// request sets the age itself.
WarpPlan PlanWarp(const CoopModJson::Value* status, bool entrance_given, std::string_view scene);
// What the teleport route does with Longshot's ok reply (`sent`, `map`: the map it sent, `accepted`: the game's event
// manager's answer): `accepted` false fails now (`refused`) instead of waiting out the timeout; else wait for `map`
// (Longshot's spelling, for Link's age), or for the scene asked when the reply has none.
struct TeleportWait {
    bool failed = false;
    std::string code, why;
    std::string wait_for;
};
TeleportWait ReadTeleport(const CoopModJson::Value& reply, std::string_view scene);
// Whether Link is in `scene` now: `status`'s map is the scene (any case; a `_Child` or `_Adult` ending names the age too,
// the map may not carry it: not seen in a game yet, so either spelling counts) and its fresh snapshot says no event and
// no load.
bool WarpArrived(const CoopModJson::Value* status, std::string_view scene);

// The `world` member of `state` (docs/playtest.md): the actors near our Link from the object walk's last snapshot.
// "" (no member) when the snapshot switch is off, so the old reply stays byte for byte the same. `now_frame` is the
// bridge's frame count, `now_ms` the steady clock in ms. Returns `,"world":{...}`: frame (the snapshot's), age_ms,
// stale (older than 1.5 s and 30 frames behind the game: then `actors` is empty; a held game does not age it), behind (the game is past the snapshot's frame), cost_us, count,
// total (actors in the snapshot), other_count (of them, kind "other": grass, pots), count (listed), actors
// [{id,addr,name,kind,pos,dist,life,max_life,owner,alive}] nearest first. `id` is the actor's address in decimal and
// `addr` the same in hex, as the LICHII_COOP_DEBUG_OBJECTS log prints it. `full` false (state): at most
// kStateLimit actors of a known kind, the "other" ones only counted; true (dump): all the snapshot's actors.
// An empty `actors` has a `why`: "no co-op session: ..." (no snapshot), "stale: no walk for N ms ..." or "empty: the
// walk ran and saw no actors". No snapshot yet: `frame` null.
std::string WorldMember(bool enabled, const std::optional<CoopWorldSnapshot::Snapshot>& snapshot,
                        std::uint64_t now_frame, std::uint64_t now_ms, bool full = false);

// The strike acts (docs/playtest.md): `kill ID NAME` and `hit ID NAME N` name an actor by the id `get_world` lists. The
// id is resolved in the objects walk, which runs only while connected and sharing, so with no usable world list (no
// snapshot, or one older than the stale limit) an id is refused before anything is queued. Empty when the lines have
// no id strike or a list is there; else the reply's text. A line that is not a strike is not looked at (the parser
// checks the rest).
std::string StrikeIdRefusal(std::string_view lines, bool world_list);
// Whether the snapshot is a usable world list: there is one and it is not stale (CoopWorldSnapshot::IsStale: by the
// wall clock and the game's frames together, so a held game keeps its list). `now_frame` is the bridge's frame count.
bool WorldListLive(const std::optional<CoopWorldSnapshot::Snapshot>& snapshot, std::uint64_t now_ms,
                   std::uint64_t now_frame);
// Whether the lines hold a `kill` or `hit` step (with an id or not): the strike queue gives each a number.
bool HasStrikeLine(std::string_view lines);
// The `strike` member of `state`: `,"strike":{seq,result,name,id}` for the newest strike the walk ran, or "" when none
// ran yet (so the reply stays as it was for a game that never struck).
std::string StrikeMember(const std::optional<CoopStrike::Result>& last);

}  // namespace CoopPlaytest
