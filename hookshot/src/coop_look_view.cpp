// The Customize page's live picture: the game's camera around your own Link
// (coop_look_view.h).
#include "coop_look_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

namespace CoopLookView {

namespace {

constexpr double kTau = 6.283185307179586;

// The view's rows as the game's view is rebuilt from an eye and a target
// (hos_main.c oot_coop_view, the free camera's): right, up, back.
struct Axes {
    Vec3 right, up, back;
};

Axes AxesOf(double yaw, double pitch) {
    const double sy = std::sin(yaw), cy = std::cos(yaw), sp = std::sin(pitch), cp = std::cos(pitch);
    return {{cy, 0, -sy}, {-sp * sy, cp, -sp * cy}, {cp * sy, sp, cp * cy}};
}

double Dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

}  // namespace

bool Window::Valid() const {
    return std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1) && std::isfinite(aspect) &&
           x0 >= 0 && y0 >= 0 && x1 <= 1 && y1 <= 1 && x1 > x0 && y1 > y0 && aspect > 0;
}

namespace {

// The shot from `yaw` at `elevation` and distance `d`: Link's middle at the window's middle.
Shot AimAt(const std::array<float, 3>& feet, double h, double yaw, double elevation, double d, const Window& window,
           double fov_y) {
    const Axes a = AxesOf(yaw, elevation);
    const double t = std::tan(fov_y * 0.5);
    // The window's middle in the view's terms (-1 to 1, y up).
    const double nx = static_cast<double>(window.x0) + window.x1 - 1.0;
    const double ny = 1.0 - (static_cast<double>(window.y0) + window.y1);
    const Vec3 middle{feet[0], feet[1] + h * 0.5, feet[2]};
    Shot s;
    for (int i = 0; i < 3; ++i) {
        // Aimed beside him, so that he stands at the window's middle; the eye d behind that aim.
        s.target[i] = middle[i] - a.right[i] * nx * d * t * window.aspect - a.up[i] * ny * d * t;
        s.eye[i] = s.target[i] + a.back[i] * d;
    }
    return s;
}

// How much of the window's height he fills, head to feet, in a shot.
double Filled(const Shot& s, const std::array<float, 3>& feet, double h, const Window& window, double fov_y) {
    const auto top = Project(s, {feet[0], feet[1] + h, feet[2]}, window.aspect, fov_y);
    const auto bottom = Project(s, {feet[0], feet[1], feet[2]}, window.aspect, fov_y);
    return (bottom[1] - top[1]) / (static_cast<double>(window.y1) - window.y0);
}

}  // namespace

double FeetDrop(const Shot& shot, const std::array<float, 3>& feet) {
    return std::atan2(shot.eye[1] - feet[1], std::hypot(shot.eye[0] - feet[0], shot.eye[2] - feet[2]));
}

Shot Aim(const std::array<float, 3>& feet, float height, double yaw, const Window& window, double fov_y) {
    const double h = height > 0 ? height : 1.0;
    // Start at the distance that fits his height into kFill of the window seen straight on, and at the elevation
    // whose line to his feet would fall kFeetDrop. Then a few steps: the window's place and the near-far of the
    // view move both a little, and each step corrects the elevation by the drop missed and the distance by the
    // fill missed.
    double elevation = kElevationMin;
    double d = h / (kFill * (window.y1 - window.y0) * 2.0 * std::tan(fov_y * 0.5));
    Shot s = AimAt(feet, h, yaw, elevation, d, window, fov_y);
    for (int step = 0; step < 12; ++step) {
        const double next = std::clamp(elevation + kFeetDrop - FeetDrop(s, feet), kElevationMin, kElevationMax);
        const double filled = Filled(s, feet, h, window, fov_y);
        const double next_d = filled > 0 ? d * filled / kFill : d;
        if (std::fabs(next - elevation) < 1e-7 && std::fabs(next_d - d) < 1e-7 * d) {
            break;
        }
        elevation = next;
        d = next_d;
        s = AimAt(feet, h, yaw, elevation, d, window, fov_y);
    }
    return s;
}

double FrontYaw(const Vec3& game_eye, const Vec3& game_target) {
    return std::atan2(game_eye[0] - game_target[0], game_eye[2] - game_target[2]) + kTau / 2;
}

std::array<double, 2> Project(const Shot& shot, const Vec3& point, double aspect, double fov_y) {
    const Vec3 b{shot.eye[0] - shot.target[0], shot.eye[1] - shot.target[1], shot.eye[2] - shot.target[2]};
    const Axes a = AxesOf(std::atan2(b[0], b[2]), std::atan2(b[1], std::hypot(b[0], b[2])));
    const Vec3 p{point[0] - shot.eye[0], point[1] - shot.eye[1], point[2] - shot.eye[2]};
    const double t = std::tan(fov_y * 0.5), depth = -Dot(a.back, p);
    const double nx = Dot(a.right, p) / (depth * t * aspect), ny = Dot(a.up, p) / (depth * t);
    return {(nx + 1) * 0.5, (1 - ny) * 0.5};
}

void Orbit::Ask(const Window& window, std::uint64_t now_ms) {
    if (!window.Valid()) {
        return;
    }
    window_ = window;
    asked_ms_ = now_ms;
}

bool Orbit::Asked(std::uint64_t now_ms) const {
    return asked_ms_ && now_ms >= *asked_ms_ && now_ms - *asked_ms_ <= kHoldMs;
}

std::optional<double> FovFromClip(const std::array<float, 16>& m) {
    const auto length = [&m](int row) {
        return std::sqrt(static_cast<double>(m[4 * row]) * m[4 * row] + static_cast<double>(m[4 * row + 1]) * m[4 * row + 1] +
                         static_cast<double>(m[4 * row + 2]) * m[4 * row + 2]);
    };
    const double up = length(1), depth = length(3);
    if (!std::isfinite(up) || !std::isfinite(depth) || up <= 1e-9 || depth <= 1e-9) {
        return std::nullopt;
    }
    const double fov = 2.0 * std::atan(depth / up);
    if (!(fov >= kFovMin && fov <= kFovMax)) {
        return std::nullopt;
    }
    return fov;
}

std::optional<Shot> Orbit::Camera(const Vec3& game_eye, const Vec3& game_target, const std::optional<Subject>& link,
                                  std::uint64_t now_ms, double fov_y) {
    if (!Asked(now_ms) || !link) {
        start_ms_.reset();   // (the next turn starts in front of him again)
        return std::nullopt;
    }
    if (!start_ms_) {
        start_ms_ = now_ms;
        start_yaw_ = FrontYaw(game_eye, game_target);
    }
    const double turn = static_cast<double>(now_ms - *start_ms_) / 1000.0 / kTurnSeconds;
    return Aim(link->feet, link->height, start_yaw_ + kTau * (turn - std::floor(turn)), window_, fov_y);
}

namespace {
constexpr double kDeg = 57.29577951308232;
}  // namespace

double Wrap(double degrees) {
    double w = std::fmod(degrees, 360.0);
    if (w > 180.0) {
        w -= 360.0;
    } else if (w < -180.0) {
        w += 360.0;
    }
    return w;
}

std::optional<Direction> DirectionOf(const Vec3& target, const Vec3& eye) {
    const double x = eye[0] - target[0], y = eye[1] - target[1], z = eye[2] - target[2];
    const double flat = std::hypot(x, z);
    if (!(std::hypot(flat, y) > 1e-6)) {
        return std::nullopt;
    }
    return Direction{flat > 1e-9 ? std::atan2(x, z) * kDeg : 0.0, std::atan2(y, flat) * kDeg};
}

Shot ReAim(const Vec3& eye, const Vec3& point, const Window& window, double fov_y) {
    // The point is seen along v in the view's own terms (right, up, back), with v's depth -1: where the window's
    // middle is. Find the yaw and pitch whose axes turn v onto the line from the eye to the point (AxesOf: world =
    // vx right + vy up + vz back). Height: vy cos p + vz sin p = u_y. Across: atan2(x, z) = yaw + atan2(vx, k), with
    // k = vz cos p - vy sin p, the root with k < 0 (the point in front of the eye).
    const double t = std::tan(fov_y * 0.5);
    const double nx = static_cast<double>(window.x0) + window.x1 - 1.0;
    const double ny = 1.0 - (static_cast<double>(window.y0) + window.y1);
    double vx = nx * t * window.aspect, vy = ny * t, vz = -1.0;
    const double vl = std::sqrt(vx * vx + vy * vy + vz * vz);
    vx /= vl, vy /= vl, vz /= vl;
    Vec3 u{point[0] - eye[0], point[1] - eye[1], point[2] - eye[2]};
    const double ul = std::sqrt(Dot(u, u));
    Shot s;
    s.eye = eye;
    if (!(ul > 1e-9)) {
        s.target = point;
        return s;
    }
    for (auto& c : u) {
        c /= ul;
    }
    const double r = std::hypot(vy, vz), phi = std::atan2(vz, vy);
    const double turn = std::acos(std::clamp(u[1] / r, -1.0, 1.0));
    double pitch = phi + turn;
    double k = vz * std::cos(pitch) - vy * std::sin(pitch);
    if (k > 0) {   // (the other root: the point in front of the eye, not behind it)
        pitch = phi - turn;
        k = vz * std::cos(pitch) - vy * std::sin(pitch);
    }
    const double yaw = std::atan2(u[0], u[2]) - std::atan2(vx, k);
    const Axes a = AxesOf(yaw, pitch);
    for (int i = 0; i < 3; ++i) {
        s.target[i] = eye[i] - a.back[i] * ul;
    }
    return s;
}

std::optional<Shot> Framed(const Vec3& game_eye, const Vec3& game_target, const Shot& aim, const Subject& link,
                           const Window& window, double free_distance, double fov_y) {
    const Vec3 d{game_eye[0] - game_target[0], game_eye[1] - game_target[1], game_eye[2] - game_target[2]};
    const double dist = std::sqrt(Dot(d, d));
    if (!(dist >= kMinEye) || !(dist < free_distance * kPulledIn)) {
        return std::nullopt;
    }
    const Vec3 a{aim.eye[0] - game_target[0], aim.eye[1] - game_target[1], aim.eye[2] - game_target[2]};
    const double use = std::min(dist, std::sqrt(Dot(a, a)));
    if (!(use >= kMinEye)) {
        return std::nullopt;
    }
    Vec3 eye{};
    for (int i = 0; i < 3; ++i) {
        eye[i] = game_target[i] + d[i] / dist * use;
    }
    const double h = link.height > 0 ? link.height : 1.0;
    const Vec3 middle{link.feet[0], link.feet[1] + h * 0.5, link.feet[2]};
    const Shot shot = ReAim(eye, middle, window, fov_y);
    // A camera above him that would have to tilt past straight down to put him in the window: Aim's picture instead.
    const auto at = Project(shot, middle, window.aspect, fov_y);
    if (!(std::fabs(at[0] - (static_cast<double>(window.x0) + window.x1) * 0.5) < 1e-3 &&
          std::fabs(at[1] - (static_cast<double>(window.y0) + window.y1) * 0.5) < 1e-3)) {
        return std::nullopt;
    }
    return shot;
}

std::optional<Angles> Steer::Restore() {
    const auto back = original_;
    original_.reset();
    return back;
}

void Steer::Lose(const std::string& why) {
    phase_ = Phase::lost;
    Say("coop: look: the game camera does not turn with the Customize picture (" + why +
            "): the picture keeps its own framing, and the camera is put back");
}

std::optional<Angles> Steer::Drive() {
    if (seen_) {
        // The offsets seen with the last command, taken in part (a view one frame late only shifts them a little).
        offset_yaw_ = Wrap(offset_yaw_ + kGain * Wrap(seen_->yaw - sign_yaw_ * last_.yaw - offset_yaw_));
        offset_pitch_ += kGain * (seen_->elevation - sign_pitch_ * last_.pitch - offset_pitch_);
        const double off_yaw = std::fabs(Wrap(want_->yaw - seen_->yaw));
        const double off_elevation = std::fabs(Elevation() - seen_->elevation);
        off_ = off_yaw > kLostYaw || off_elevation > kLostElevation ? off_ + 1 : 0;
        seen_.reset();
        if (off_ >= kLostFrames) {
            char why[96];
            std::snprintf(why, sizeof why, "off by %.0f degrees across and %.0f up for %d frames", off_yaw,
                          off_elevation, kLostFrames);
            Lose(why);
            return Restore();
        }
    }
    last_.yaw = static_cast<float>(Wrap(sign_yaw_ * Wrap(want_->yaw - offset_yaw_)));
    last_.pitch = static_cast<float>(sign_pitch_ * (Elevation() - offset_pitch_));
    return last_;
}

std::optional<Angles> Steer::Command(Angles now, int state, bool asked) {
    since_turn_ = 0;
    auto angles = Step(now, state, asked);
    Watch(state, asked);
    return angles;
}

void Steer::Watch(int state, bool asked) {
    if (!asked || phase_ == Phase::probe || phase_ == Phase::drive) {
        gate_now_.reset();
        gate_said_.reset();   // (said again at the next opening that does not turn)
        gate_calls_ = 0;
        return;
    }
    const Gate gate{state, want_.has_value(), phase_};
    if (gate_now_ != gate) {
        gate_now_ = gate;
        gate_calls_ = 0;
    }
    if (++gate_calls_ != kGateCalls || gate_said_ == gate) {
        return;
    }
    gate_said_ = gate;
    char line[400];
    std::snprintf(line, sizeof line,
                  "coop: look: the game camera is not turned by the Customize picture (the camera hook hands in "
                  "state %d, the page is asked, a wanted direction: %s, the steer %s): the probe starts only in "
                  "states 3 and 4 with a wanted direction",
                  state, gate.want ? "yes" : "no",
                  phase_ == Phase::lost ? "gave up on it (lost)" : "has not started (idle)");
    Say(line);
}

void Steer::Viewed() {
    if (++since_turn_ == kNoTurnFrames) {
        char line[300];
        std::snprintf(line, sizeof line,
                      "coop: look: the camera hook (hos_coop_turn, patch 0040) was not called in %d frames of the "
                      "Customize picture: the game camera is not turned (no player camera update reaches the hook)",
                      kNoTurnFrames);
        Say(line);
    }
}

std::optional<Angles> Steer::Step(Angles now, int state, bool asked) {
    if (state != 3 && state != 4) {
        // Another camera has the view (an event): its angles are not the ones to put back; the probe starts again.
        phase_ = Phase::idle;
        original_.reset();
        seen_.reset();
        return std::nullopt;
    }
    if (!asked || !want_) {
        phase_ = Phase::idle;
        seen_.reset();
        return Restore();
    }
    if (!std::isfinite(now.yaw) || !std::isfinite(now.pitch)) {
        return std::nullopt;
    }
    switch (phase_) {
    case Phase::lost:
        return Restore();
    case Phase::idle:
        original_ = now;
        phase_ = Phase::probe;
        step_ = waited_ = off_ = 0;
        blocked_ = false;
        held_.reset();
        lift_ = 0;
        seen_.reset();
        [[fallthrough]];
    case Phase::probe: {
        if (step_ > 0) {
            if (!seen_) {
                if (++waited_ > kProbeWait) {
                    Lose("no view seen");
                    return Restore();
                }
                return last_;   // (the same angles until a view of them is seen)
            }
            probe_seen_[step_ - 1] = *seen_;
            seen_.reset();
        }
        if (step_ < kProbeSteps) {
            // From where the camera was, a little more each frame; the pitch towards level (its limits are near 90).
            const double toward = original_->pitch > 0 ? -1.0 : 1.0;
            last_.yaw = static_cast<float>(Wrap(original_->yaw + kProbeYaw * step_));
            last_.pitch = static_cast<float>(original_->pitch + toward * kProbePitch * step_);
            probe_cmd_[step_++] = last_;
            return last_;
        }
        // The eye's turn over the camera's, from the second view to the last (the first may be of the frame before).
        const Angles& c0 = probe_cmd_[1];
        const Angles& c1 = probe_cmd_[kProbeSteps - 1];
        const Direction& s0 = probe_seen_[1];
        const Direction& s1 = probe_seen_[kProbeSteps - 1];
        const double ratio_yaw = Wrap(s1.yaw - s0.yaw) / Wrap(static_cast<double>(c1.yaw) - c0.yaw);
        const double ratio_pitch = (s1.elevation - s0.elevation) / (static_cast<double>(c1.pitch) - c0.pitch);
        const auto follows = [](double r) { return std::fabs(r) >= kRatioMin && std::fabs(r) <= kRatioMax; };
        if (!follows(ratio_yaw) || !follows(ratio_pitch)) {
            char why[96];
            std::snprintf(why, sizeof why, "the eye turned %.2f and %.2f times the camera", ratio_yaw, ratio_pitch);
            Lose(why);
            return Restore();
        }
        sign_yaw_ = ratio_yaw > 0 ? 1 : -1;
        sign_pitch_ = ratio_pitch > 0 ? 1 : -1;
        offset_yaw_ = Wrap(s1.yaw - sign_yaw_ * c1.yaw);
        offset_pitch_ = s1.elevation - sign_pitch_ * c1.pitch;
        last_ = c1;
        phase_ = Phase::drive;
        char line[320];
        std::snprintf(line, sizeof line,
                      "coop: look: the game camera turns with the Customize picture (the eye turns %.2f and %.2f "
                      "times its yaw and pitch; its eye %.2f m from its target, the picture's own %.2f m), so the "
                      "game's collision places the eye where it moves it in",
                      ratio_yaw, ratio_pitch, free_, aim_distance_);
        Say(line);
        return Drive();
    }
    case Phase::drive:
        return Drive();
    }
    return std::nullopt;
}

void Steer::Observe(const Vec3& game_eye, const Vec3& game_target) {
    if (const auto d = DirectionOf(game_target, game_eye)) {
        seen_ = d;
        if (phase_ == Phase::probe || phase_ == Phase::drive) {
            // The best seen at this place, over the whole turn and earlier openings: a page opened in a tight spot
            // learns the open side as the turn reaches it, and keeps it when opened there again.
            if (!free_at_ || std::hypot(game_target[0] - (*free_at_)[0], game_target[1] - (*free_at_)[1],
                                        game_target[2] - (*free_at_)[2]) > kSamePlace) {
                free_ = 0;
                free_at_ = game_target;
            }
            free_ = std::max(free_, std::hypot(game_eye[0] - game_target[0], game_eye[1] - game_target[1],
                                               game_eye[2] - game_target[2]));
        }
    }
}

void Steer::Want(const std::optional<Direction>& want, double aim_distance) {
    want_ = want;
    aim_distance_ = aim_distance;
}

void Steer::Say(std::string line) {
    notes_.push_back(std::move(line));
}

std::optional<std::string> Steer::TakeNote() {
    if (notes_.empty()) {
        return std::nullopt;
    }
    auto n = std::move(notes_.front());
    notes_.pop_front();
    return n;
}

double Steer::Elevation() const {
    if (!want_) {
        return 0.0;
    }
    const double e = want_->elevation;
    return std::min(e + lift_, std::max(e, kLiftTo));
}

std::optional<Shot> Steer::Picture(const Vec3& game_eye, const Vec3& game_target, const Shot& aim,
                                   const Subject& link, const Window& window, double fov_y) {
    if (phase_ != Phase::drive) {
        return aim;
    }
    const double dist = std::hypot(game_eye[0] - game_target[0], game_eye[1] - game_target[1],
                                   game_eye[2] - game_target[2]);
    const double floor = std::max(kBlockedMin, kBlockedShare * free_);
    const double h = link.height > 0 ? link.height : 1.0;
    const Vec3 middle{link.feet[0], link.feet[1] + h * 0.5, link.feet[2]};
    std::optional<Shot> shot;
    const bool far_enough = dist >= floor * (blocked_ ? kClearMargin : 1.0);
    if (far_enough) {
        if (lift_ <= 0 && !(dist < free_ * kPulledIn)) {
            // Open ground: the orbit's own eye, so he fits (the game's camera may sit nearer than it by itself).
            shot = aim;
        } else {
            // Moved in by a wall, or raised over one: from where the game's collision put the eye.
            shot = Framed(game_eye, game_target, aim, link, window,
                          lift_ > 0 ? std::numeric_limits<double>::infinity() : free_, fov_y);
        }
    }
    if (shot) {
        if (blocked_) {
            char line[200];
            std::snprintf(line, sizeof line,
                          "coop: look: the Customize camera is clear again (its eye %.2f m from its target, free "
                          "distance %.2f m)",
                          dist, free_);
            Say(line);
        }
        blocked_ = false;
        held_ = Vec3{shot->eye[0] - middle[0], shot->eye[1] - middle[1], shot->eye[2] - middle[2]};
        lift_ = std::max(lift_ - kLiftStep, 0.0);   // (a clear view: back down to the orbit's elevation)
        return shot;
    }
    if (!blocked_) {
        blocked_ = true;
        char line[320];
        std::snprintf(line, sizeof line,
                      "coop: look: the Customize camera is blocked (its eye %.2f m from its target, free distance "
                      "%.2f m, blocked under %.2f m): %s",
                      dist, free_, floor,
                      held_ ? "the picture holds its last clear view"
                            : "no clear view yet, the flat picture shows and the camera rises to look over it");
        Say(line);
    }
    if (held_) {
        lift_ = std::max(lift_ - kLiftStep, 0.0);   // (it rises only until the first clear view)
        const Vec3 eye{middle[0] + (*held_)[0], middle[1] + (*held_)[1], middle[2] + (*held_)[2]};
        return ReAim(eye, middle, window, fov_y);
    }
    // Too near: raised to look over the wall. Far enough but too steep to put him in the window's middle (the view
    // would tilt past straight down): lowered again.
    lift_ = far_enough ? std::max(lift_ - kLiftStep, 0.0) : std::min(lift_ + kLiftStep, kLiftTo);
    return std::nullopt;
}

namespace {
bool Live(std::optional<Showing> s) {
    return s == Showing::live_child || s == Showing::live_adult || s == Showing::live_event;
}
}  // namespace

const char* Describe(Showing showing) {
    switch (showing) {
    case Showing::live_child: return "live, the camera around your child Link";
    case Showing::live_adult: return "live, the camera around your adult Link";
    case Showing::flat_no_link: return "flat: the co-op hook has no Link this frame (no game, scene or place)";
    case Showing::live_event: return "live, our own event (a cutscene) runs: the camera around your Link";
    case Showing::flat_blocked:
        return "flat: the camera turns around your Link but its eye is blocked and has had no clear view yet";
    }
    return "?";
}

std::optional<Seen> Keep::Take(const std::optional<Seen>& now, std::uint64_t now_ms) {
    if (now) {
        last_ = now;
        seen_ms_ = now_ms;
        return now;
    }
    if (last_ && now_ms >= seen_ms_ && now_ms - seen_ms_ <= kGapMs) {
        return last_;
    }
    last_.reset();
    return std::nullopt;
}

std::optional<std::string> Watch::Drawn(Showing showing, std::uint64_t now_ms) {
    // Not drawn for a while: the page was left; the next opening says its picture again.
    if (last_ && (now_ms < drawn_ms_ || now_ms - drawn_ms_ > kHoldMs)) {
        last_.reset();
    }
    drawn_ms_ = now_ms;
    if (showing != last_) {
        if (Live(showing) && !Live(last_)) {
            live_since_ms_ = now_ms;
            told_unframed_ = false;
        }
        last_ = showing;
        return std::string("coop: look: the Customize picture is ") + Describe(showing);
    }
    if (Live(showing) && !told_unframed_ && now_ms - live_since_ms_ >= kUnframedMs &&
        (!framed_ms_ || *framed_ms_ < live_since_ms_)) {
        told_unframed_ = true;
        return std::string("coop: look: the Customize picture is live but its camera was not framed for ") +
               std::to_string(kUnframedMs / 1000) +
               " s: the game's view routine did not ask (hos_coop_view, patch 0019), so the game's own camera shows";
    }
    return std::nullopt;
}

void Watch::Framed(std::uint64_t now_ms) {
    framed_ms_ = now_ms;
}

}  // namespace CoopLookView
