// The Customize page's live picture (docs/puppet.md "The Link customizer"):
// your own Link, the real one in the game, seen through a clear window in
// the menu (hos_coop_menu_picture.h `live`, patch 0019) while the game's
// camera turns slowly around him (hos_coop_view.h, patch 0019). Longshot's
// menu does not stop the game, so the world keeps drawing under it; the
// camera is the game's own view, only its eye and target are ours.
//
// The menu's thread asks for the picture each drawing (Ask); the game's
// thread frames the camera while it is asked for (Camera). Asked for no more
// (the page left, the menu closed): the game's camera again within kHoldMs.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>

namespace CoopLookView {

using Vec3 = std::array<double, 3>;

// The picture's square in the window: fractions of it (y down), and the
// window's width over its height.
struct Window {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float aspect = 16.0f / 9.0f;
    bool Valid() const;
};

struct Shot {
    Vec3 eye{};
    Vec3 target{};
};

// The game's vertical field of view, in radians, when its camera is not
// handed over (FovFromClip has none): measured on a screenshot.
inline constexpr double kFovY = 0.785398;   // 45 degrees
// The field of view FovFromClip believes: outside it, kFovY.
inline constexpr double kFovMin = 0.17;   // 10 degrees
inline constexpr double kFovMax = 2.1;    // 120 degrees
// One turn around Link, in seconds.
inline constexpr double kTurnSeconds = 16.0;
// The camera looks down on him so that the line from the eye to his feet falls
// this much: a step or ledge edge up to 0.5 m high, 0.9 m or more in front of
// him, stays below that line, so his feet show over it (beta-test-1: at 8
// degrees up, a stone step's edge in Kakariko hid him). The game's collision
// is not used here (docs/puppet.md "The Link customizer"): walls and low
// ceilings between the eye and him can still show.
inline constexpr double kFeetDrop = 0.5236;       // radians (30 degrees)
inline constexpr double kElevationMin = 0.14;     // radians (8 degrees): the eye never lower
inline constexpr double kElevationMax = 0.7854;   // radians (45 degrees): never steeper
// Link's height fills this much of the window's height.
inline constexpr double kFill = 0.85;
// Asked for no later than this ago: still the picture (the menu draws at the
// display's rate, the game frames its camera at its own).
inline constexpr std::uint64_t kHoldMs = 250;

// The game's vertical field of view from its world-to-clip matrix (row-major,
// clip = m * p: the camera block Hookshot hands the name tags, CoopCamera).
// Its y row is the view's up scaled by 1/tan(fov/2), its w row the view's
// depth axis at scale 1, so the fov is 2 atan(|w| / |y|) (the xyz parts).
// None for a matrix that is not such a perspective.
std::optional<double> FovFromClip(const std::array<float, 16>& clip_from_world);

// The shot that shows Link (his feet, his height) in the middle of the
// window, from `yaw` (the direction from him to the eye, as atan2(x, z): the
// game's views' convention), filling kFill of the window's height, looking
// down on him so that his feet show over a step (kFeetDrop).
Shot Aim(const std::array<float, 3>& feet, float height, double yaw, const Window& window, double fov_y = kFovY);

// How steeply the line from a shot's eye down to his feet falls, in radians
// (Aim makes it kFeetDrop, within kElevationMin and kElevationMax).
double FeetDrop(const Shot& shot, const std::array<float, 3>& feet);

// The yaw that looks at Link's front, from the game's own camera (behind
// him): its direction turned round.
double FrontYaw(const Vec3& game_eye, const Vec3& game_target);

// Where a point lands in the window (fractions, y down) for a shot, as the
// game's view (rebuilt from eye and target, hos_main.c oot_coop_view) and a
// projection of `fov_y` show it. For tests and for checking Aim.
std::array<double, 2> Project(const Shot& shot, const Vec3& point, double aspect, double fov_y = kFovY);

// Link as the camera needs him: his feet and his height.
struct Subject {
    std::array<float, 3> feet{};
    float height = 0;
};

// One turn of the camera: started when first asked for, its yaw from the
// game's camera then; ended when not asked for kHoldMs.
class Orbit {
public:
    void Ask(const Window& window, std::uint64_t now_ms);
    bool Asked(std::uint64_t now_ms) const;
    // The game's thread: the shot for now, or none (the game's camera).
    // `fov_y`: the game's (FovFromClip), else kFovY.
    std::optional<Shot> Camera(const Vec3& game_eye, const Vec3& game_target, const std::optional<Subject>& link,
                               std::uint64_t now_ms, double fov_y = kFovY);
    const Window& window() const { return window_; }

private:
    Window window_{};
    std::optional<std::uint64_t> asked_ms_;
    std::optional<std::uint64_t> start_ms_;   // the turn's start; none: no turn
    double start_yaw_ = 0;
};

// The game's own camera turned with the orbit (patch 0040, hos_coop_turn.h), so the game's own collision places
// the eye: a wall or a low ceiling between Link and the eye brings the eye nearer, as in play (look-camera: next to
// a cliff, one side of the turn put the eye inside the rock). The game's camera hook asks for its yaw and pitch
// (Steer::Command) before the camera's update; the view hook then gets the eye the game placed and the module keeps
// it (Framed). Without patch 0040, or when the camera does not follow, the picture is Aim's as before.

// The game camera's angles, in degrees (hos_coop_turn.h: yaw falls as the view turns right, pitch rises as it
// tilts down).
struct Angles {
    float yaw = 0;
    float pitch = 0;
};

// Where an eye is seen from a target, in degrees: its yaw as atan2(x, z) (Aim's convention) and how high it is.
struct Direction {
    double yaw = 0;
    double elevation = 0;
};
// None when the two are (nearly) the same point.
std::optional<Direction> DirectionOf(const Vec3& target, const Vec3& eye);
// An angle in degrees wrapped into -180..180.
double Wrap(double degrees);

// The shot from `eye` that shows `point` at the window's middle (the eye stays where it is; only the aim turns).
Shot ReAim(const Vec3& eye, const Vec3& point, const Window& window, double fov_y = kFovY);

// Below this the game's eye is too near its target to frame Link from (metres).
inline constexpr double kMinEye = 0.2;
// The game's eye counts as moved in by its collision when it is nearer its target than this share of the
// furthest it was seen (its free distance).
inline constexpr double kPulledIn = 0.9;
// The picture from the game's own eye (the camera turned with the orbit), only where the game's collision moved
// that eye in (nearer than kPulledIn of `free_distance`, the furthest the eye was seen from its target): the eye on
// the line from the game's target to the game's eye, no further than Aim's eye is from that target, aimed so that
// Link's middle stays at the window's middle. None (Aim's picture) in open ground: a game camera that sits nearer
// than Aim's by itself would show Link bigger than the window. None also when the eye is under kMinEye from its
// target, or the view would have to tilt past straight down.
std::optional<Shot> Framed(const Vec3& game_eye, const Vec3& game_target, const Shot& aim, const Subject& link,
                           const Window& window, double free_distance, double fov_y = kFovY);

// Turns the game's camera with the orbit. The camera's angles are not world angles: their sign and offset are
// learned. First a probe (Aim's picture meanwhile): a few frames turning the camera a little from where it was,
// to see which way its eye goes and by how much. Then each frame the camera is given the angles that put its eye
// in the orbit's direction, from the offsets seen so far. A camera that does not follow (no view seen, or the eye
// turning the wrong amount, or off by too much for too long) is put back and left alone until the page is left.
// The angles it had are put back when the page is left. Not thread-safe: the caller's lock.
class Steer {
public:
    enum class Phase : std::uint8_t { idle, probe, drive, lost };
    // Probe: this many frames, each turning the camera this much more.
    static constexpr int kProbeSteps = 5;
    static constexpr double kProbeYaw = 3.0;     // degrees a frame
    static constexpr double kProbePitch = 2.0;   // degrees a frame
    // The eye's turn over the camera's must be within this (either sign), or the camera does not follow.
    static constexpr double kRatioMin = 0.5;
    static constexpr double kRatioMax = 2.0;
    // Frames the probe waits for a view before it gives up.
    static constexpr int kProbeWait = 30;
    // How much of an offset seen is taken each frame.
    static constexpr double kGain = 0.5;
    // Off by more than these (degrees) for kLostFrames frames in a row: the camera does not follow.
    static constexpr double kLostYaw = 20.0;
    static constexpr double kLostElevation = 25.0;
    static constexpr int kLostFrames = 45;
    // While driven, the game's eye counts as blocked (a wall or a ceiling at the lens: rock, a dark frame, his
    // tunic filling it; camera-check-2, the Bottom of the Well) when it is nearer its target than kBlockedMin
    // metres, or than kBlockedShare of the free distance. It is clear again only kClearMargin times further (so
    // an eye at the edge does not flip the picture back and forth).
    static constexpr double kBlockedMin = 1.2;
    static constexpr double kBlockedShare = 0.6;
    static constexpr double kClearMargin = 1.1;
    // Blocked with no clear view yet: the camera is raised this many degrees a frame, to look down over the wall,
    // up to kLiftTo degrees above level (the game's collision still places the eye); lowered again when the eye is
    // far enough but the view would have to tilt past straight down to put Link in the window's middle (a window
    // off the screen's middle runs out of tilt sooner). Once a clear view has been had it comes back down,
    // kLiftStep a frame, to the orbit's own elevation (blocked again on the way: the clear view is held).
    static constexpr double kLiftStep = 1.5;
    static constexpr double kLiftTo = 60.0;
    // The free distance is kept over openings of the page while the game's target stays within this (metres) of
    // where it was learned; further away (another scene, Link walked on), it is learned anew.
    static constexpr double kSamePlace = 1.5;
    // The log while the page is asked and the camera is not turned (the lane run of 2026-11-24 saw no steer line at
    // all): the camera hook's state, said once per change after it held this many calls.
    static constexpr int kGateCalls = 30;
    // The view hook's frames with the page asked and no call of the camera hook: said once at this many.
    static constexpr int kNoTurnFrames = 120;

    // The camera hook (the game's thread), before the camera's update: the angles to give it, or none. `state`:
    // the camera's (3 follows Link, 4 the player's; others: another camera has the view). `asked`: the picture is
    // still asked for.
    std::optional<Angles> Command(Angles now, int state, bool asked);
    // The view hook, each frame the page is asked: counts the frames with no Command since the last one.
    void Viewed();
    // The camera's update is wanted (patch 0043, the wake, hos_coop_turn.h): the page is asked, the orbit wants a
    // direction, and the camera was not given up on. Changes nothing.
    bool WantsCamera(bool asked) const { return asked && want_.has_value() && phase_ != Phase::lost; }
    // The view hook (the game's thread): the eye and target the game's camera placed this frame.
    void Observe(const Vec3& game_eye, const Vec3& game_target);
    // The view hook: where the orbit wants the eye seen from the game's target, or none (no picture now), and how
    // far Aim's eye is from that target (for the log).
    void Want(const std::optional<Direction>& want, double aim_distance = 0.0);
    // The view hook, after Observe: the picture for this frame. Not driven (the probe, a camera that does not
    // follow): `aim`, the orbit's own. Driven: from the game's eye (Framed), or `aim` in open ground (the eye not
    // moved in and the camera not raised), or, when the eye is blocked, the last clear view held (its eye kept
    // where it was beside Link, aimed at his middle). Blocked with no clear view yet: none (the page shows the flat
    // picture, Hidden) and the camera is raised. Never `aim` while driven otherwise: it has no collision, and in a
    // narrow place it is inside the rock.
    std::optional<Shot> Picture(const Vec3& game_eye, const Vec3& game_target, const Shot& aim, const Subject& link,
                                const Window& window, double fov_y = kFovY);
    // Driven, blocked, and no clear view to hold: the page shows the flat picture (and still asks for the live
    // one, so the camera keeps turning to find a clear view).
    bool Hidden() const { return phase_ == Phase::drive && blocked_ && !held_; }
    // The furthest the game's eye was seen from its target at this place (0: none seen).
    double FreeDistance() const { return free_; }
    Phase phase() const { return phase_; }
    // The game's camera turns with the orbit: its eye is the picture's.
    bool Driving() const { return phase_ == Phase::drive; }
    // A line for the log when the steering changed (once each).
    std::optional<std::string> TakeNote();

private:
    // What the camera hook was handed while the page is asked and the camera is not turned (for the log).
    struct Gate {
        int state = 0;
        bool want = false;
        Phase phase = Phase::idle;
        bool operator==(const Gate&) const = default;
    };
    std::optional<Angles> Step(Angles now, int state, bool asked);
    void Watch(int state, bool asked);
    std::optional<Angles> Restore();
    void Lose(const std::string& why);
    std::optional<Angles> Drive();
    void Say(std::string line);
    // The elevation the camera is given now: the orbit's, raised by lift_ (no higher than kLiftTo).
    double Elevation() const;

    Phase phase_ = Phase::idle;
    std::optional<Angles> original_;   // the camera's angles before the first turn: put back at the end
    std::optional<Direction> want_;
    std::optional<Direction> seen_;    // the last view, since the last command
    Angles last_{};                    // the last command
    int step_ = 0, waited_ = 0, off_ = 0;
    Angles probe_cmd_[kProbeSteps]{};
    Direction probe_seen_[kProbeSteps]{};
    double sign_yaw_ = 1, sign_pitch_ = 1;   // the eye's turn over the camera's
    double offset_yaw_ = 0, offset_pitch_ = 0;   // eye direction = sign * camera angle + offset
    double free_ = 0, aim_distance_ = 0;         // metres
    std::optional<Vec3> free_at_;                // the game's target where free_ was learned
    bool blocked_ = false;
    std::optional<Vec3> held_;                   // the last clear view's eye, from Link's middle
    double lift_ = 0;                            // degrees the camera is raised over the orbit's elevation
    std::deque<std::string> notes_;
    std::optional<Gate> gate_now_, gate_said_;
    int gate_calls_ = 0;
    int since_turn_ = 0;   // the view hook's frames since the last Command
};

// What the page shows, for the log (0.11.1: adult Link got the flat picture
// where a child got the live one; the log now says which and why).
enum class Showing : std::uint8_t {
    live_child,     // the live picture, a child Link
    live_adult,     // the live picture, an adult Link
    flat_no_link,   // the flat one: the hook has no Link (no game, scene or place) for longer than Keep::kGapMs
    live_event,     // the live picture while our own event (a cutscene) runs: the camera still turns around him
    flat_blocked,   // the flat one while the camera turns: its eye is blocked and it has had no clear view yet
};
const char* Describe(Showing showing);

// Your own Link as the page last saw him, kept through a short gap (#74: the
// picture flipped to the flat one for a moment after each scene entry, when
// the hook had no Link for a frame or two). Not thread-safe: the caller's lock.
struct Seen {
    Subject link;
    Showing showing = Showing::live_child;
};
class Keep {
public:
    static constexpr std::uint64_t kGapMs = 1500;
    // This moment's Link (none: the hook has none): him, or the last one seen
    // no longer than kGapMs ago, or none.
    std::optional<Seen> Take(const std::optional<Seen>& now, std::uint64_t now_ms);

private:
    std::optional<Seen> last_;
    std::uint64_t seen_ms_ = 0;
};

// One line to the log each time the page's picture changes (and again at
// each opening of the page), and once when a live picture's camera is never
// framed (the game's view routine did not ask for it). Not thread-safe: the
// caller's lock.
class Watch {
public:
    // How long a live picture may go without a framed camera before it is said.
    static constexpr std::uint64_t kUnframedMs = 2000;
    // The menu's thread, each drawing of the page: a line to log, or none.
    std::optional<std::string> Drawn(Showing showing, std::uint64_t now_ms);
    // The game's thread: the camera was framed around Link.
    void Framed(std::uint64_t now_ms);

private:
    std::optional<Showing> last_;
    std::uint64_t drawn_ms_ = 0;
    std::uint64_t live_since_ms_ = 0;
    std::optional<std::uint64_t> framed_ms_;
    bool told_unframed_ = false;
};

// The module's one orbit, with your own Link from the co-op hook
// (CoopHook::CurrentMapView). Any thread.
//
// Your own Link can be shown now (the game runs, his place is known, no
// cutscene): the page asks for the live picture, else draws the flat one.
// `clear` (when given): false while the camera's eye is blocked with no clear
// view yet (Steer::Hidden); the page then still asks (the camera keeps
// turning) but draws the flat picture.
bool LinkShown(bool* clear = nullptr);
// The menu's thread, each drawing of the live picture.
void Ask(const Window& window);
// hos_coop_view (the game's thread): 1 = eye and target replaced.
bool Camera(double eye[3], double target[3]);
// hos_coop_turn (the game's thread, patch 0040): 1 = yaw and pitch replaced. `coop_on` false (co-op switched off):
// a camera still turned is put back once, then left alone. Yaw and pitch null with state -1 (the host's wake,
// hos_coop_turn.h): true while the Customize picture wants the camera's update (Steer::WantsCamera); nothing changes.
bool Turn(float* yaw, float* pitch, int state, bool coop_on);

}  // namespace CoopLookView
