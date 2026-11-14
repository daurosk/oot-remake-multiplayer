// The Customize page's live picture: the game's camera around your own Link
// (coop_look_view.h).
#include "coop_look_view.h"

#include <cmath>

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

Shot Aim(const std::array<float, 3>& feet, float height, double yaw, const Window& window, double fov_y) {
    const Axes a = AxesOf(yaw, kElevation);
    const double t = std::tan(fov_y * 0.5);
    const double h = height > 0 ? height : 1.0;
    const double d = h / (kFill * (window.y1 - window.y0) * 2.0 * t);
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

std::optional<Shot> Orbit::Camera(const Vec3& game_eye, const Vec3& game_target, const std::optional<Subject>& link,
                                  std::uint64_t now_ms) {
    if (!Asked(now_ms) || !link) {
        start_ms_.reset();   // (the next turn starts in front of him again)
        return std::nullopt;
    }
    if (!start_ms_) {
        start_ms_ = now_ms;
        start_yaw_ = FrontYaw(game_eye, game_target);
    }
    const double turn = static_cast<double>(now_ms - *start_ms_) / 1000.0 / kTurnSeconds;
    return Aim(link->feet, link->height, start_yaw_ + kTau * (turn - std::floor(turn)), window_);
}

}  // namespace CoopLookView
