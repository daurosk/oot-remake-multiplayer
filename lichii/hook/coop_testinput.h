// Scripted controller input for automatic tests (LICHII_COOP_TEST_SCRIPT).
//
// LICHII_COOP_TEST_SCRIPT=<file> names a text file of input steps. The hook
// looks at the file about twice a second; each time it changes (and is not
// empty) it is read again and played from that moment into player 1's
// emulated pad (or the handheld pad when player 1 is not connected). The
// script's input is added to whatever the real keyboard or pad does. A test
// runner can so write one script to load the save, wait, then write the next.
//
// Time is counted in ticks of 1/60 s of real time, not in emulated frames:
// Ocarina of Time moves by elapsed time, so a step lasts as long whatever
// the frame rate. One step a line; `#` starts a comment:
//
//   wait 120               nothing held for 120 ticks (2 s)
//   hold A 30              hold A for 30 ticks
//   hold LSTICK_UP+B 90    left stick fully up and B together (run)
//   press A                hold A 6 ticks, then let go 6 ticks
//   press ZR+A 10          hold 10 ticks, then let go 10 ticks
//   stick 0.5 1.0 60       left stick at x 0.5, y 1.0 (each -1..1) for 60
//   stick 0 1 60 B         the same with buttons held too
//   rstick -1 0 30         right stick (the camera) the same way
//   goto -7.5 71.1         walk Link to world x -7.5, z 71.1 (within 1 m),
//   goto -7.5 71.1 1.5 600 ... within 1.5 m, giving up after 600 ticks
//   toward -7.5 71.1 8     push the stick toward that point for 8 ticks
//                          (Link turns to face it)
//   warp KakarikoVillage_Child Kakariko_WellSide
//                          (the scene must be a real scene name, such as Market_Child, not Market: an unknown
//                          name is refused with a "coop: test script ... unknown scene" line and nothing is written)
//   chat /teleport Saria   (the rest of the line, sent as the chat box sends it)
//                          test only: changes scene the way the game's own
//                          sequence does (Sequence_ReservedNextBanc), to the
//                          start position given (StartPos names in the
//                          scene's Banc) or the scene's own. The game takes
//                          it only during an event, such as the one after a
//                          save loads: put the warp before the presses that
//                          load the save (the hook asks again every 0.5 s
//                          for up to 90 s, until Link is in that scene)
//   place -37.2 109.8 13.5 test only: puts Link at world x, y (height), z
//                          in this scene (the boss pull's free-play move,
//                          CoopBoss::MoveLink; needs LICHII_COOP_BOSS on, the
//                          default), so a scenario reaches a room no walk
//                          can; logs "coop: test place ...: done" or "failed"
//   face 66.96 0.0         test only (row 175): turns Link on the spot to
//                          face world x, z (its rotation written, as the
//                          place's move; CoopBoss::FaceLink), for an aim the
//                          camera-learnt `toward` cannot square; logs
//                          "coop: test face ...: turned to yaw N deg"
//   aim -56.5 131.5 -10.1  test only (row 173): as face, and our next arrow
//                          (NormalArrow nocked within 10 s) is put, on the
//                          first frame it flies, 0.6 m short of world x, y
//                          (height), z on its line there and pointed at it,
//                          so its own flight takes it in (the bow's pitch
//                          comes from the camera, which no step can square);
//                          logs "coop: test aim ...: arrow ... put" or why not
//   kill Enemy_Skulltula_Gold test only: the nearest actor of that name to
//                          Link takes a hit from Link through the game's own
//                          damage (as a real kill, so it drops what it
//                          drops); needs LICHII_COOP_OBJECTS on; logs
//                          "coop: test kill NAME ...: hit" or "none here"
//   hit Enemy_DungeonBoss_Gohma 2  test only: as kill, but a hit of N (1 to
//                          80) of its life through the game's own Life step
//                          (never past its stops); a copy's fall goes to the
//                          owner as DAMAGE as any hit of ours; logs
//                          "coop: test hit NAME N ...: hit" or "none here"
//   stun Enemy_DungeonBoss_Gohma  test only: our COPY of the nearest boss of
//                          that name tells its owner it is stunned here (a
//                          DAMAGE of 0, issue 150's hand-over), as our game's
//                          own stun would; its stun animation is not played;
//                          logs "coop: test stun NAME: said" or "not a copy"
//   hurt 4                 test only: our own Link takes a hit of that many
//                          quarter hearts (1 to 80) through the game's own hit
//                          on him (CoopObjects::HurtOurLink, PvP's path: he
//                          flinches; a lethal one runs the game's death);
//                          logs "coop: test hurt N: asked" (or "no Link")
//   give bombs             test only: gives an item through coop_cheats
//                          (needs LICHII_COOP_CHEATS=1): bombs, bow, hookshot,
//                          or any item flag name coop_cheats knows
//   mark walk-start        writes "coop: test mark walk-start" to the log
//
// goto and toward steer from Link's position, read each frame. The stick is
// relative to the camera, which turns as Link walks: the hook gives the
// stepper the camera's ground directions from the game's camera matrix each
// frame (without them it learns them from how Link moves). A push that gets
// nowhere for a second steps aside. A goto that gives up logs
// "coop: test goto timed out" and the script goes on.
//
// Buttons: A B X Y L R ZL ZR PLUS MINUS LCLICK RCLICK, the d-pad DUP DDOWN
// DLEFT DRIGHT, and full stick directions LSTICK_UP LSTICK_DOWN LSTICK_LEFT
// LSTICK_RIGHT RSTICK_UP RSTICK_DOWN RSTICK_LEFT RSTICK_RIGHT; join with `+`.
// When the last step ends the hook writes "coop: test script done".
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Core {
class System;
}

namespace CoopTestInput {

constexpr int kTicksPerSecond = 60;
constexpr std::int32_t kStickMax = 0x7fff;  // HID_JOYSTICK_MAX

// The emulator's NpadButton bits (hid_core/hid_types.h).
enum Button : std::uint64_t {
    kA = 1u << 0,
    kB = 1u << 1,
    kX = 1u << 2,
    kY = 1u << 3,
    kLClick = 1u << 4,
    kRClick = 1u << 5,
    kL = 1u << 6,
    kR = 1u << 7,
    kZL = 1u << 8,
    kZR = 1u << 9,
    kPlus = 1u << 10,
    kMinus = 1u << 11,
    kDLeft = 1u << 12,
    kDUp = 1u << 13,
    kDRight = 1u << 14,
    kDDown = 1u << 15,
};

struct Stick {
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool operator==(const Stick&) const = default;
};

enum class Kind {
    timed,   // wait, hold, press, stick, rstick
    mark,
    go,      // goto
    toward,
    warp,    // test only: scene change
    give,    // test only: an item
    wear,    // test only: a PlayerStatus.Equip* flag (what the puppets in other games wear)
    assign,  // test only: the item on ZR (PlayerStatus.AssignableItem.Child and .Adult)
    dress,   // test only: the game's own equip call for a slot, from the PlayerStatus.Equip* flags (re-dresses OUR Link)
    chat,    // test only: a chat line or command, as typed in the chat box (/teleport B)
    place,   // test only: Link put at x, y, z in this scene
    face,    // test only: Link turned on the spot to face the world point x, z
    aim,     // test only: as face, then our next arrow put on its way into the world point x, y, z
    kill,    // test only: the nearest actor of that name hit by our Link for all its life
    hurt,    // test only: our own Link hit for N quarter hearts (the game's own hit)
    hit,     // test only: the nearest actor of that name hit by our Link for N of its life (its Life step)
    verb,    // test only (row 287): the boss of that name that is OURS runs an action verb (the game's own request)
    stun,    // test only: our copy of the nearest boss of that name says its owner a stun (DAMAGE 0)
    drop,    // test only (row 178): the nearest boss of that name that is OURS moved N m straight down
    setflag, // test only: an Int or Bool flag of this game set to a value (issue 130: shop-buy)
    setenum, // test only: an Enum flag (name or 0x hash) set to a value's name (issue 193: quests)
    sync,    // sync NAME N: wait here, buttons let go, until N games reached NAME
    menu,    // test only: menu customize|close, the Multiplayer tab's Customize page opened or the menu closed (issue 74)
    dump,    // test only (play-test kit): this game's shared flags written to <commands file>.dump-TAG.json
    event,   // test only (boss-route-01112): an event setting asked of our game by name (the live-event requester)
    autosave,  // test only (rejoin-host-273-2): our game asked for its own autosave (CoopSave::AskOwnAutoSave)
};

struct Step {
    Kind kind = Kind::timed;
    std::uint32_t ticks = 0;  // timed and toward; goto: the give-up time; sync: N
    std::uint64_t buttons = 0;
    std::optional<Stick> left;   // unset: the real stick
    std::optional<Stick> right;
    std::string mark;  // mark: its name; warp: the scene; give: the item; wear: the piece;
                       // assign: the item's name in the button enum (Bow, Hookshot, Hammer)
    std::string start; // warp: the start position name (empty: the scene's own); wear: the slot
    std::uint64_t id = 0;  // kill and hit: the actor's address ("kill ID NAME"); 0: the nearest of that name
    float x = 0.0f;  // goto and toward: the world point (x, z); place: (x, y, z)
    float y = 0.0f;
    float z = 0.0f;
    float radius = 1.0f;
    int line = 0;  // the script line it came from
};

struct Script {
    std::vector<Step> steps;
    // The first `song` line (the runners write songs out as presses; Parse only checks its shape), 0 for none:
    // a script loaded to be played with one is refused (SongNotWrittenOut).
    int song_line = 0;
};

// The error for a script about to be played that still has a `song` line, or "" when it has none.
std::string SongNotWrittenOut(const Script& script);

// Why `warp scene [start]` must not be asked of the game, or an empty string when it may. The scene must be one the
// game has (CoopTeleport::IsKnownScene; the error names the likely ones: "Market" is not a scene, Market_Child and
// Market_Adult are). A start, when given, must be a plain name of at most 63 characters (letters, digits, '_'): the
// game's start names are not all known to the code, so one that is only unknown is left to the game. Nothing is
// written into the game for a scene or start that gets a reason here.
std::string WarpProblem(std::string_view scene, std::string_view start);

// The "gave up" line of a warp that never happened. `scene` is the scene the game is in now: nothing when no save is
// loaded (Link is nowhere: the title, a load), which the line says in words instead of "scene ?".
std::string WarpGiveUpLine(std::string_view target, double seconds, const std::optional<std::string>& scene);

// Reads a script. On a bad line returns nothing and sets `error` to
// "line N: why".
std::optional<Script> Parse(std::string_view text, std::string& error);

// The kinds the play-test bridge's `act` command may run (hookshot/src/coop_playtest.cpp): kill, hit, hurt, place, face,
// aim, wear, dress, assign, chat, menu, setflag, setenum, verb (row 287: a boss move the harness cannot bring about,
// queued like kill). The rest of what a script can do has its own bridge command or makes no sense while the game is
// held (wait, hold, press, goto, toward, sync, mark, dump, warp, give, stun, drop).
const std::vector<Kind>& ActKinds();
// The word a script line starts with for this kind ("kill", "goto", ...; a timed step: "timed").
const char* KindWord(Kind kind);
// Empty when every step of `script` is one of `allowed`; else "line N: WORD is not allowed here (allowed: ...)". The check is
// on the parsed kinds, so case, spacing and a hidden second line cannot get past it.
std::string NotAllowed(const Script& script, const std::vector<Kind>& allowed);

// `aim` (row 173): the last bit of a player's aim. The shot is aimed by turning the view the way a player does; at
// the let-go the arrow or seed is only moved sideways onto the line through the target along its own flight (its
// way `flight` and speed stay the game's), and only when that flight already points within `max_deg` of the target
// and passes within `max_side_m` of it (a fixed distance, so the slack does not grow with range).
// `off_deg`: how far it pointed off. `put`: the new place, none when it was too far off (no cheat for a bad aim) or
// flying away. Nothing at all when there is no flight, the shot is at the target, or a value is not finite.
struct AimNudged {
    float off_deg = 0.0f;
    float side_m = 0.0f;   // how far the flight's line passes from the target
    std::optional<std::array<float, 3>> put;
};
inline constexpr float kAimNudgeDeg = 4.0f;   // the reticle's slack, not a new aim
inline constexpr float kAimNudgeM = 0.5f;     // and never more than this sideways (4 degrees is 2 m at 30 m)
std::optional<AimNudged> AimNudge(const std::array<float, 3>& at, const std::array<float, 3>& flight,
                                  const std::array<float, 3>& target, float max_deg, float max_side_m = kAimNudgeM);
// `aim`: a nocked arrow is let go when it moves more than this between two frames (its flight is ~113 m/s, ~1.9 m
// a frame at 60; a nocked one moves with Link's hands, well under it).
inline constexpr float kAimLetGoM = 1.0f;
// `aim`: the projectiles of ours it steers, by actor name: the bow's arrow and the slingshot's seed
// (coop_projectiles.cpp names both; another player's shot is a CoopShot_ stand-in, never one of these).
inline bool Aimable(std::string_view name) {
    return name == "NormalArrow" || name == "Shootable_DekuSeed";
}

// `aim`: the aim that waits for our next shot. The walk offers it every listed arrow or seed of ours, not only one it
// sees for the first time: the slingshot makes its next seed right after a shot, so that seed is already listed when
// the aim is armed (lane run). The first one offered within kAimHandM of our Link is taken (a shot still flying or
// lying from before is farther); AimStep then watches it until it is let go. With Link's place unknown, the first.
inline constexpr float kAimHandM = 2.0f;
struct AimWait {
    std::array<float, 3> target{};
    std::uint64_t until_ms = 0;
    std::uint64_t arrow = 0;       // our nocked arrow or seed (0: none yet)
    std::array<float, 3> last{};   // its place the frame before

    bool Offer(std::string_view name, std::uint64_t actor, const std::array<float, 3>& at,
               const std::array<float, 3>* link = nullptr) {
        if (arrow != 0 || actor == 0 || !Aimable(name)) {
            return false;
        }
        if (link) {
            const float dx = at[0] - (*link)[0];
            const float dy = at[1] - (*link)[1];
            const float dz = at[2] - (*link)[2];
            if (!(dx * dx + dy * dy + dz * dz <= kAimHandM * kAimHandM)) {
                return false;
            }
        }
        arrow = actor;
        last = at;
        return true;
    }
};

// wear SLOT: the PlayerStatus flag name for weapon, shield, head, armor or
// boots (any case), e.g. "EquipHeadArmor"; nothing for another word.
std::optional<std::string> WearFlag(std::string_view slot);

// dress SLOT: the equip kind the game's own GameData equip call (main + 0x2df6c80, the call a save load and the
// pause menu's equip make) takes for weapon, shield, head, armor, boots or hand (any case); nothing for another
// word. The parser expands "all" to the six.
std::optional<std::uint32_t> DressKind(std::string_view slot);

// The kinds a `dress` step asked for that the game has not been asked yet. The svc caller (coop_hook.cpp,
// CoopCallerOrders::kTestDress) takes one per call, on the game's calc thread. Thread-safe.
void DressAsk(std::uint32_t kind);
std::optional<std::uint32_t> DressNext();

// assign ITEM: true for a value of the item button enum
// (PlayerStatus.AssignableItem.Child / .Adult in GameDataList: Empty,
// DekuStick, Bomb, SlingShot, Hookshot, Longshot, Hammer, Bow, ...), as the
// game spells it.
bool AssignableItem(std::string_view name);

// What the pad gets now.
struct Output {
    std::uint64_t buttons = 0;
    std::optional<Stick> left;
    std::optional<Stick> right;
    bool operator==(const Output&) const = default;
};

// Plays a script step by step. No emulator code: the hook calls Advance once
// a frame with the time since the start and Link's position (x, z) when it
// can read it, and gives Current() to the pad.
class Runner {
public:
    explicit Runner(Script script);

    // sync NAME N: says we reached NAME and whether N games have (the hook
    // keeps one file per game in a folder the test runner shares). Without
    // one, a sync step passes at once.
    using Barrier = std::function<bool(const std::string& name, std::uint32_t games)>;
    void SetBarrier(Barrier barrier) { barrier_ = std::move(barrier); }

    // The camera on the ground: unit vectors (x, z) of where stick up and
    // stick right walk Link. From the game's camera matrix when the hook has
    // it; without it the stepper learns them from how Link moves.
    struct Camera {
        float forward_x = 0.0f;
        float forward_z = 0.0f;
        float right_x = 0.0f;
        float right_z = 0.0f;
    };

    // Moves on to the step for `now_s`. Appends to `log` the marks reached
    // and "goto timed out" lines, in order.
    void Advance(double now_s, std::optional<std::array<float, 2>> player,
                 std::vector<std::string>& log, std::optional<Camera> camera = std::nullopt);
    const Output& Current() const { return out_; }
    // The warp and give steps reached since the last call, for the hook.
    struct Request {
        Kind kind = Kind::warp;
        std::string name;
        std::string start;
        std::array<float, 3> at{};  // place
        std::uint64_t id = 0;       // kill and hit: the actor's address; 0: by name
    };
    std::vector<Request> TakeRequests() { return std::exchange(requests_, {}); }
    bool Done() const { return index_ >= script_.steps.size(); }
    std::size_t StepCount() const { return script_.steps.size(); }
    // Steering now (goto or toward), for the log.
    bool Steering() const;
    std::optional<double> Heading() const { return heading_; }
    double Mirror() const { return mirror_; }

    // Link turns before he walks: movement is learnt from only after the
    // stick has held one direction this long, and a steering stick is
    // re-aimed at most this often, so such windows happen.
    static constexpr double kSettleS = 0.3;
    static constexpr double kAimEveryS = 0.5;
    // No 0.3 m of progress in kStuckS: push a quarter turn aside for kDetourS.
    static constexpr double kStuckS = 1.0;
    static constexpr double kDetourS = 0.7;

private:
    void Learn(double now_s, std::optional<std::array<float, 2>> player);
    void Steer(const Step& step, double now_s, std::optional<std::array<float, 2>> player);
    std::optional<Camera> camera_;  // this frame's, when given
    std::vector<Request> requests_;
    Barrier barrier_;
    void Hold(std::optional<double> stick_angle, double now_s);

    Script script_;
    std::size_t index_ = 0;
    std::size_t marked_index_ = static_cast<std::size_t>(-1);  // a hold whose mark was said
    double step_start_ = 0.0;  // the script starts at time 0
    Output out_;
    // The camera model: stick up moves Link along world angle `heading_`
    // (radians, atan2 of z and x); `mirror_` is -1 when stick right turns the
    // other way round. Learnt from Link's movement while the stick is held.
    std::optional<double> heading_;
    double mirror_ = 1.0;
    bool mirror_known_ = false;
    std::optional<double> stick_angle_;  // the left stick's direction now (none: let go)
    double stick_since_ = 0.0;           // when it took that direction
    std::optional<std::array<float, 2>> window_pos_;  // where a steady window began
    std::optional<std::array<float, 2>> prev_player_;  // last frame's position
    double aimed_at_ = -1.0;             // when Steer last chose a direction
    std::optional<std::array<float, 2>> progress_pos_;  // stuck check
    double progress_at_ = 0.0;
    double detour_until_ = -1.0;
    double detour_side_ = -1.0;
};

// Called once per emulated frame by the hook. Does nothing unless
// LICHII_COOP_TEST_SCRIPT is set. In the Hookshot module (COOP_HOOKSHOT) it
// reads LICHII_COOP_TEST_COMMANDS instead: the autotest driver
// (tools/autotest/hookshot.py) holds the pad through Hookshot's control bridge
// and writes only the test-only steps there; they are played without a pad
// and the file is emptied once read.
void OnFrame(Core::System& system);

// The play-test bridge (hookshot/src/coop_playtest.h): test-only lines done at once on the calling thread (the
// game's calc thread while the bridge holds it), as a commands file's would be: setflag, setenum, give, wear,
// assign, dress, place, face, aim, kill, hit, stun, drop, hurt, chat, menu, dump, mark. A `warp` (alone) begins a
// script that asks again as frames run (OnFrame) until Link is in that scene. Timed steps (hold, goto, ...) are
// refused: the bridge's own input moves Link. False with `error` for a bad or refused line.
// `allowed` (the bridge's `act` gives ActKinds()): a script with a step of another kind is refused whole, before any
// step runs. Null: every kind PlayNow can do (the bridge's warp, flag and give lines).
bool PlayNow(Core::System& system, std::string_view lines, std::string& error,
             const std::vector<Kind>* allowed = nullptr);
// What the last `give` did, one text for all its flags ("NAME was -> now; ..."; "no change: ... already N"; "NOT
// given: why"). Any thread. Empty before the first give.
std::string LastGive();

// The value of every flag a dump writes (the shared dungeon and story lists; Bool as 0 or 1), in list order.
std::vector<std::pair<std::string, std::int32_t>> SharedFlags(Core::System& system);
// One game flag by name: an Int, or a Bool as 0 or 1; a String64. Nothing when the game has no such flag.
std::optional<std::int32_t> FlagInt(Core::System& system, std::string_view name);
std::optional<std::string> FlagText(Core::System& system, std::string_view name);
// One flag read for the play-test bridge's `flag_read`: read only, writes nothing. `name` is a GameData flag name, a
// 0x hash, or key64:0xKEY. The kinds are tried in this order: Int, Bool, Enum, UInt, String64 (key64 only for the
// key64: form). `kind` is "int", "bool", "enum", "uint", "key64", "text", or "unknown" when the game has no such flag
// (or its flag table is not up). An Int's `live` is its table value without a write of ours still waiting in the
// queue (GameFlags::get_int_live): `number` and `live` differ while a write waits.
struct FlagReading {
    std::string kind = "unknown";
    std::optional<std::int64_t> number;  // int, bool (0 or 1), uint, key64 (0 or 1)
    std::optional<std::uint32_t> hash;   // enum: the flag_hash of the value's name
    std::string text;                    // text
    std::optional<std::int32_t> live;    // int only
};
FlagReading ReadFlag(Core::System& system, std::string_view name);
// Write an Enum flag (a name or a 0x hash) to a value given by its hash, through the game's own queue. Only the
// bridge's `flag` calls it, and only with a value hash that `flag_read` read from that same flag in this game run
// (CoopPlaytest::EnumValues), so no raw number is ever written to an Enum. Call it inside the held frame. False when
// the flag is unknown or the write was not queued.
bool SetEnumHash(Core::System& system, std::string_view name, std::uint32_t value);

// The menu step's doer: the Hookshot module sets it (coop_pause.cpp); "customize" opens
// the Multiplayer tab's Customize page, "close" closes the menu, "host" hosts, "paste" joins with the
// clipboard's code. True when asked.
// Unset (the emulator): the step logs that it is not here.
using MenuOpener = std::function<bool(const std::string& page)>;
void SetMenuOpener(MenuOpener opener);

} // namespace CoopTestInput
