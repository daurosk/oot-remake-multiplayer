#pragma once

// Overlay: the other players as markers with name tags, as plain data.
//
// Renderer-agnostic: no drawing, no fonts, no game addresses. The caller
// (the Lichii hook, later the recomp) fills a Camera each frame, hands over
// the players presence shows, and draws the DrawItem list it gets back in
// whatever way it likes. See docs/overlay.md.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop/presence/presence.hpp"

namespace coop::overlay {

using presence::Vec3;

// A 4x4 matrix of floats, column-major (element [col * 4 + row]), as
// OpenGL and most game engines keep them.
using Mat4 = std::array<float, 16>;

// What the game's camera sees this frame. Filled by the caller: the address
// of the game's camera is not yet known, so nothing here reads the game.
// `view` takes world positions to view space, `projection` view space to
// clip space; a point is in front of the camera when its clip w is above 0
// (the usual perspective projections all do this).
struct Camera {
  Mat4 view{};
  Mat4 projection{};
  float viewport_width = 0.0f;   // pixels
  float viewport_height = 0.0f;  // pixels
};

struct Settings {
  // Players nearer than near_distance, or further than far_distance, are
  // not drawn. Measured as depth (clip w: the distance in front of the
  // camera along its view direction), in world units.
  float near_distance = 1.0f;
  float far_distance = 10'000.0f;
  // The marker is marker_size pixels across at reference_distance, and
  // scales with 1 / depth from there, kept within min_size to max_size.
  float marker_size = 24.0f;
  float reference_distance = 300.0f;
  float min_size = 8.0f;
  float max_size = 48.0f;
  // Gap in pixels between the marker's top and the name tag's anchor.
  float tag_gap = 6.0f;
  // An adult player is drawn this far higher (world units), so the marker
  // and name tag stay at the same place on the taller Link as on a child.
  float adult_lift = 0.5f;
  // Players just off screen are drawn as an arrow clamped to the screen
  // edge, edge_margin pixels in. "Just off" means within edge_reach of the
  // screen in normalised device units (the screen is -1 to 1, so 1 lets a
  // player half a screen beyond the edge still show). Off by setting
  // edge_arrows false; then off-screen players are skipped.
  bool edge_arrows = true;
  float edge_reach = 1.0f;
  float edge_margin = 16.0f;
};

// True when every setting is finite, near_distance > 0, far_distance >
// near_distance, the sizes are above 0 with min_size <= max_size, and the
// gaps and reach are not negative.
bool valid(const Settings& settings);

// One player to draw, as presence shows it.
struct Shown {
  std::uint8_t number = 0;
  std::string name;  // empty: "P<number>" is shown
  Vec3 position{};
  std::optional<std::string> scene;  // none: not known, so not drawn
  float fade = 1.0f;                 // presence's fade, 1 shown, 0 gone
  bool adult = false;                // from the player's samples; lifts the marker
};

// The players presence shows at `server_now_us`, lowest number first.
// Calls Presence::shown, so it advances what presence has shown.
std::vector<Shown> collect(presence::Presence& presence, std::uint64_t server_now_us);

struct Marker {
  float x = 0.0f;      // pixels from the left of the viewport
  float y = 0.0f;      // pixels from the top of the viewport
  float depth = 0.0f;  // clip w: distance in front of the camera
  float size = 0.0f;   // pixels across
};

struct NameTag {
  std::string text;
  float x = 0.0f;  // anchor: the bottom centre of the text, in pixels
  float y = 0.0f;
  bool below = false;  // the tag would leave the top of the screen, so it
                       // is anchored under the marker (top centre) instead
};

struct DrawItem {
  std::uint8_t number = 0;
  Marker marker;
  NameTag tag;
  float alpha = 1.0f;  // for both marker and tag, 0 to 1
  // The player is off screen: the marker sits on the screen edge and
  // should be drawn as an arrow pointing at arrow_angle (radians, screen
  // space: 0 right, pi/2 down).
  bool edge = false;
  float arrow_angle = 0.0f;
};

// Why a player is not in the draw list.
enum class Why : std::uint8_t {
  invalid,      // a NaN or infinite position or fade
  faded,        // fade 0 or below
  other_scene,  // not in the local scene, or its scene is not known
  behind,       // behind the camera (clip w 0 or below)
  too_near,     // nearer than Settings::near_distance
  too_far,      // further than Settings::far_distance
  off_screen,   // off screen, and too far off (or edge arrows are off)
};

struct Skipped {
  std::uint8_t number = 0;
  Why why = Why::invalid;
};

enum class Error : std::uint8_t {
  none,
  camera,    // a NaN or infinite matrix entry, or a viewport not above 0
  settings,  // !valid(settings)
};

struct Result {
  std::vector<DrawItem> items;  // far to near, then lowest number first
  std::vector<Skipped> skipped;
  Error error = Error::none;  // anything but none: nothing is drawn
};

class Overlay {
 public:
  explicit Overlay(Settings settings = {});

  const Settings& settings() const { return settings_; }
  void set_settings(const Settings& settings) { settings_ = settings; }

  // The draw list for this frame. Only players in `local_scene` are drawn.
  Result build(const Camera& camera, std::string_view local_scene,
               const std::vector<Shown>& players) const;

 private:
  Settings settings_;
};

// Matrix helpers for tests and tools; the game hands over its own matrices.
Mat4 identity();
Mat4 multiply(const Mat4& a, const Mat4& b);  // a * b
// Right-handed view looking from `eye` at `target`, OpenGL style (the camera
// looks down its -z).
Mat4 look_at(const Vec3& eye, const Vec3& target, const Vec3& up);
// OpenGL-style perspective (clip w = view-space -z). fov_y in radians.
Mat4 perspective(float fov_y, float aspect, float z_near, float z_far);

}  // namespace coop::overlay
