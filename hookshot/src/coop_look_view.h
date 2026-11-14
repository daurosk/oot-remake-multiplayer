// The Customize page's live picture (docs/puppet.md "The Link customizer"):
// your own Link, the real one in the game, seen through a clear window in
// the menu (hos_coop_menu_picture.h `live`, patch 0019) while the game's
// camera turns slowly around him (hos_coop_view.h, patch 0019). Longshot's
// menu does not stop the game, so the world keeps drawing under it; the
// camera is the game's own view, only its eye and target are ours.
//
// The menu's thread asks for the picture each drawing (Ask); the game's
// thread frames the camera while it is asked for (Camera). Asked for no more
// (the page left, the menu closed): the game's camera again, at once.
#pragma once

#include <array>
#include <cstdint>
#include <optional>

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

// The game's vertical field of view, in radians (not read from the game: its
// camera's projection is not mapped; measured on a screenshot).
inline constexpr double kFovY = 0.785398;   // 45 degrees
// One turn around Link, in seconds; the camera a little above his middle.
inline constexpr double kTurnSeconds = 16.0;
inline constexpr double kElevation = 0.14;   // radians (8 degrees)
// Link's height fills this much of the window's height.
inline constexpr double kFill = 0.85;
// Asked for no later than this ago: still the picture (the menu draws at the
// display's rate, the game frames its camera at its own).
inline constexpr std::uint64_t kHoldMs = 250;

// The shot that shows Link (his feet, his height) in the middle of the
// window, from `yaw` (the direction from him to the eye, as atan2(x, z): the
// game's views' convention), filling kFill of the window's height.
Shot Aim(const std::array<float, 3>& feet, float height, double yaw, const Window& window, double fov_y = kFovY);

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
    std::optional<Shot> Camera(const Vec3& game_eye, const Vec3& game_target, const std::optional<Subject>& link,
                               std::uint64_t now_ms);

private:
    Window window_{};
    std::optional<std::uint64_t> asked_ms_;
    std::optional<std::uint64_t> start_ms_;   // the turn's start; none: no turn
    double start_yaw_ = 0;
};

// The module's one orbit, with your own Link from the co-op hook
// (CoopHook::CurrentMapView). Any thread.
//
// Your own Link can be shown now (the game runs, his place is known, no
// cutscene): the page asks for the live picture, else draws the flat one.
bool LinkShown();
// The menu's thread, each drawing of the live picture.
void Ask(const Window& window);
// hos_coop_view (the game's thread): 1 = eye and target replaced.
bool Camera(double eye[3], double target[3]);

}  // namespace CoopLookView
