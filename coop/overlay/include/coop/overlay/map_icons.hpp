// SPDX-License-Identifier: GPL-3.0-or-later
// The other players on the game's maps (docs/overlay.md, "Players on the
// map"): where each one's icon goes on the HUD minimap and on the pause
// menu's area and dungeon maps, as plain numbers. No rendering, no game
// addresses: the caller hands over where the map is on the picture.
//
// The game makes each area's and dungeon's map picture by looking straight
// down on it from above (Game/MapCaptureInfo): a picture centred on
// (centre_x, centre_z) that spans `height` metres from its top edge to its
// bottom edge, world +X to the right and world +Z down. A dungeon has floors:
// each floor's picture is cut by the capture camera's near and far planes
// (distances below the camera at camera_y). The puppet pack reads that file
// on the player's PC and writes coop_maps.txt (tools/puppet_pack/
// map_captures.py); nothing from the game is in this repository.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace coop::overlay::map {

using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>;  // x, y, z, w

// One floor of a map picture: the capture camera's planes, in metres below
// the camera. None: that side is not cut.
struct Floor {
  std::optional<float> near_m;
  std::optional<float> far_m;
};

// One map picture (one area, or one dungeon with its floors).
struct Capture {
  std::string location;              // "KokiriForest", "InsideTheDekuTree"
  std::vector<std::string> scenes;   // the scene (Banc) names it is the map of
  float centre_x = 0.0f, centre_z = 0.0f;
  float camera_y = 0.0f;
  float height = 0.0f;               // metres from the picture's top to its bottom
  bool dungeon = false;
  std::vector<Floor> floors;         // at least one
};

// coop_maps.txt: one capture per line,
//   map <location> <centre_x> <centre_z> <camera_y> <height> <0|1 dungeon> <scene,scene,...> [<near>:<far> ...]
// a "-" for a plane that is not cut; '#' starts a comment. A bad line is
// skipped and counted in `bad`.
std::vector<Capture> parse_captures(std::string_view text, int* bad = nullptr);

class Table {
public:
  Table() = default;
  explicit Table(std::vector<Capture> captures) : captures_(std::move(captures)) {}
  // The map of `scene`: by its location name, by one of its scene names, or
  // by a scene name without its era (_Child, _Adult). Null when none.
  const Capture* find(std::string_view scene) const;
  bool empty() const { return captures_.empty(); }
  std::size_t size() const { return captures_.size(); }

private:
  std::vector<Capture> captures_;
};

// The floor a point at world height `y` (the feet) is on: the floor whose
// planes hold y + kFloorEye with the nearest cut above it, else the first
// floor. 0 for a map without floors.
inline constexpr float kFloorEye = 0.5f;
int floor_of(const Capture& capture, float y);

// The picture's place on the screen, in pixels, y down.
struct Box {
  float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
  float cx() const { return x + w * 0.5f; }
  float cy() const { return y + h * 0.5f; }
};

// A player to place.
struct Player {
  std::uint8_t number = 0;
  Vec3 position{};
  std::optional<Quat> rotation;      // none: a dot, not an arrow
  std::optional<std::string> scene;  // none: not known, not shown
  std::uint32_t rgb = 0xFFFFFF;      // their colour, 0xRRGGBB
  float fade = 1.0f;                 // presence's fade
};

// One icon to draw.
struct Icon {
  std::uint8_t number = 0;
  float x = 0.0f, y = 0.0f;          // centre, pixels, y down
  std::optional<float> angle;        // facing: radians, 0 right, pi/2 down
  bool clamped = false;              // off the map: held at its edge
  std::uint32_t rgb = 0xFFFFFF;
  float alpha = 1.0f;
  int floor = 0;                     // their floor (floor_of)
  int floor_delta = 0;               // their floor minus the one shown (0 on an area)
};

// Facing on a map picture from a game rotation: the turned +Z (the way Link
// looks), seen from above. None when it points straight up or down.
std::optional<float> map_angle(const Quat& rotation);

// The HUD minimap: centred on our Link, `metres_across` metres from its left
// edge to its right edge, round or square.
struct Minimap {
  Box box;
  bool round = false;
  float metres_across = 0.0f;
  float edge_inset = 0.0f;  // pixels kept free at the edge for a clamped icon
};

// The minimap's icons for the others in our scene; players in another scene
// or with no scene are left out, so are bad numbers (NaN, a box of no size).
// With a dungeon's `capture`, each icon's floor and its floor_delta from our
// Link's floor (floor_of; 0 elsewhere). `out` is cleared and refilled (its
// capacity kept: no allocation once it holds 16).
void minimap_icons(const Minimap& minimap, std::string_view our_scene, const Vec3& self,
                   const std::vector<Player>& others, std::vector<Icon>& out,
                   const Capture* capture = nullptr);

// The pause menu's map of `capture` drawn in `picture` (the whole map
// picture's box on the screen, its aspect the capture's width to height):
// everyone in one of the capture's scenes, at their place, clamped into the
// picture; `shown_floor` is the floor on screen (dungeons). Others left out.
void picture_icons(const Box& picture, const Capture& capture, int shown_floor,
                   const std::vector<Player>& players, std::vector<Icon>& out);

// One world point on the picture (no clamping).
std::array<float, 2> to_picture(const Box& picture, const Capture& capture, const Vec3& p);

// A ratio for the minimap's zoom (LICHII_COOP_MINIMAP_ZOOM): a finite number
// above 0 and at most 100, else none.
std::optional<float> parse_zoom(std::string_view text);

}  // namespace coop::overlay::map
