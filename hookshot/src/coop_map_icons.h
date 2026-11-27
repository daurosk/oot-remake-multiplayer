// SPDX-License-Identifier: GPL-3.0-or-later
// The other players on the game's own maps (native-ui-audit item 8, issue #255): the game's map icon part, not drawn
// shapes or text. The pack (tools/puppet_pack/map_icons.py) puts kCopies hidden copies of MapMenu_00's Pa_MapIcon_00
// (layout PaMapIcon_00) under its clip S_MapIcon_00, beside the game's own icons; this module shows one per other
// player, as the game shows Link:
// - the copy's N_Player_00 (PaMapIconPlayer_00: Link's arrow, its N_Type_00 look), turned the way the player faces
//   (N_PlayerRotate_00), the other icon types hidden;
// - the player's name in the copy's N_Entrance_00 > Pa_Entrance_00 (PaMapIconEntrance_00): the game's own place-name
//   label of the map (T_Location_00, the game's font), its entrance pictures hidden, the text in the player's colour
//   as the balloons' names (coop_native_tags.h); "(1 below)" after it for another floor of a dungeon;
// - moved in the clip's own space (the inverse of S_MapIcon_00's global matrix), so the game's clip cuts it at the
//   map's edge, as it cuts its own icons; one past the edge is hidden (the game's clip, not an edge marker).
// When two arrows overlap only the name of the one nearer to us shows. Nobody else on the map (solo): every copy stays
// hidden and nothing of the game's is touched.
//
// The marks come from the present's thread (CoopDraw::MapMarks of the frame's MapShown, Publish), the panes are moved
// on the game's thread (Icons::Step at the frame hook, Taken). A part copy's own panes are in its own layout: found as
// the layout object the copy's pane points at whose resource has the part layout's name (CoopNativeTags::LayoutsHeld),
// then by name with the game's lookup.
//
// Split for tests (tests/hookshot/map_icons_test.cpp): Inverse, ToLocal and Plan are pure.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "coop_native_tags.h"

namespace CoopMapIcons {

inline constexpr char kCopyName[] = "Pa_CoopMapIcon_%02d";   // (tools/puppet_pack/map_icons.py COPY_NAME)
inline constexpr std::size_t kCopies = 15;                    // (MAP_ICONS)
inline constexpr char kClipPane[] = "S_MapIcon_00";
inline constexpr std::uint64_t kPaneMatrix = 0x28;   // pane+: the global matrix, 3 x 4 by column (CoopMapPage)
// pane+: the z turn (radians, as the layout files keep it). 0: NOT KNOWN YET, the arrows are not turned (no guessed
// value ships, worker-rules "NO GUESSED VALUES", B #142). The candidate is +0x68: a .buil pane record keeps scale
// +0x10/+0x14, rotation +0x18, translate +0x1c/+0x20 (tools/puppet_pack/ui_pack.py), and a pane in memory has that
// translate at +0x6c/+0x70 (CoopNativeTags addr::kPaneXY). The probe is the bridge test
// (tools/autotest/ingame/test_map_icons.py, run with --env LICHII_COOP_MAP_TURN_AT=68): the game's own arrow's word
// there must follow our facing a quarter a turn; only then is it set here, citing that run.
// LICHII_COOP_MAP_TURN_AT=<hex> tries one without a rebuild. With no place (0, or TURN_AT=0) the module leaves the
// copies down and the overlay's turned arrows stay (coop_hookshot.cpp MapIcons): never an unturned arrow.
inline constexpr std::uint64_t kPaneTurn = 0;

// One other player on the map shown, in the layout's units (3840 x 2160, x right, y down from the top left).
struct Mark {
    std::uint8_t number = 0;
    float x = 0.0f, y = 0.0f;
    std::optional<float> angle;   // facing on the screen: radians, 0 right, pi/2 down (map::Icon's)
    bool clamped = false;         // past the map's edge
    int floor_delta = 0;          // their floor minus the one shown
    std::uint32_t rgb = 0xFFFFFF;
    std::string name;             // with the floor words; empty: "P<n>"
};
// Our own place on the map shown, the same units (for the overlap rule); none: not known.
struct Frame {
    std::vector<Mark> marks;
    std::optional<std::array<float, 2>> self;
};
void Publish(Frame frame);   // the present's thread
Frame Taken();               // the game's thread: the newest frame

// The 2D part of a 3 x 4 by column matrix (x' = m0 x + m3 y + m9, y' = m1 x + m4 y + m10) inverted:
// {a, b, c, d, tx, ty} with x = a x' + c y' + tx, y = b x' + d y' + ty. None for one that cannot be.
std::optional<std::array<float, 6>> Inverse(const float m[12]);
// A layout point (y down from the top left) in the frame the inverse leaves (y up, as the pane's translation).
std::array<float, 2> ToLocal(const std::array<float, 6>& inverse, float x, float y);

inline constexpr float kOverlap = 64.0f;   // layout units: two arrows closer than this overlap (the arrow's width)
struct Slot {
    std::size_t copy = 0;
    std::array<float, 2> at{};      // in the clip's frame
    std::optional<float> turn;      // the arrow's z turn, radians counter-clockwise from up; none: not turned
    bool arrow = true;              // false: another floor (the game shows Link only on his own)
    std::string name;
    bool name_shown = true;
    std::uint32_t rgb = 0xFFFFFF;
};
// One copy per mark, at most kCopies, in the marks' order; clamped ones and those with no finite point are left out.
std::vector<Slot> Plan(const Frame& frame, const std::array<float, 6>& inverse);

// What the copies show now, for the play-test bridge's `map_native` member (docs/playtest.md): test-only read.
struct State {
    bool up = false, table = false, game_arrow = false;
    std::string why;
    std::uint64_t turn_at = 0;
    std::array<float, 8> game_arrow_words{};   // the game's own arrow's N_PlayerRotate_00 +0x60..+0x7c
    // ProbeArrow only: the same words of the arrows in the game's run-time icon copies under the map screen (Link's
    // arrow among them when the game shows him through a copy, not through its Pa_MapIcon_00 itself), at most 4
    std::vector<std::array<float, 8>> run_arrow_words;
    // ProbeArrow only (probe v2): each arrow's raw words +0x28..+0xbc (38 floats: the global matrix +0x28..+0x54, 3 x 4
    // by column, then the pane's own words), [0] the template's, [1..] the run-time copies' in run_arrow_words' order.
    // The drawn turn is atan2(m[1], m[0]) of the matrix: the test reads it from the frame itself and names the own word
    // that follows it (no guessed place).
    static constexpr std::size_t kRawWords = (0xc0 - 0x28) / 4;
    std::vector<std::array<float, kRawWords>> arrow_raw;
    bool probe = false;                        // true: ProbeArrow's (no copies looked at)
    struct Shown {
        std::size_t copy = 0;
        std::array<float, 2> at{};             // the copy pane's translate (+0x6c) in the clip's frame
        std::optional<float> turn;             // read back at turn_at
        std::optional<float> wrote_turn;       // what Step wrote there
        std::string name;
        bool name_set = false;
    };
    std::vector<Shown> shown;
};
std::string Json(const State& st);
void PublishState(std::string json);   // the game's thread
std::string StateJson();               // the bridge's thread: "null" before the first

class Icons {
public:
    // False when the map screen, its clip or the copies are not there (no pack, the title, a load): the module then
    // draws nothing on the maps either (CoopHookshot: the overlay's map names only while Up() was never true).
    bool Step(CoopNativeTags::Game& game, const Frame& frame);
    void HideAll(CoopNativeTags::Game& game);
    // A scene load (or a new map screen): every pane known is dropped, with no game call (they may be freed).
    void Forget();
    void TurnAt(std::uint64_t pane_offset) { turn_at_ = pane_offset; }   // 0: not turned
    const std::string& why() const { return why_; }
    // The rotation-offset probe: an icon layout's (a PaMapIcon_00 the map screen holds) arrow N_PlayerRotate_00; 0
    // when not found.
    // `table`: the map layout's pane table.
    std::uint64_t ArrowTurnPane(CoopNativeTags::Game& game, std::uint64_t table);
    State Snapshot(CoopNativeTags::Game& game);
    // The turn place's probe on ONE game (solo too: no other player needed), the play-test bridge only: the game's own
    // arrow's words, every pane looked up fresh from the map screen (nothing cached, nothing written or shown).
    State ProbeArrow(CoopNativeTags::Game& game);
    // The probe's lines (coop_map_icons.cpp Icons::Probe): the game's own icon part and its run-time copies.
    std::vector<std::string> Probe(CoopNativeTags::Game& game);                      // what Step last missed (the log)
    bool Up() const { return up_; }
    std::size_t shown() const { return shown_.size(); }
    std::uint64_t copy(std::size_t i) const { return i < kCopies ? copies_[i].pane : 0; }

    // The guest block: the pane names (0-ended, kNameBytes each), then per copy a 256-byte text slot {ref, text}, then
    // per copy 16 bytes of colour.
    static constexpr std::size_t kNameBytes = 32;
    static constexpr std::size_t kNames = 40;
    static constexpr std::size_t kSlotBytes = 256;
    static constexpr std::size_t kBlockBytes = kNames * kNameBytes + kCopies * (kSlotBytes + 16);

private:
    struct Copy {
        std::uint64_t pane = 0;
        std::uint64_t icon = 0, entrance = 0, player = 0;   // the part layouts inside it
        std::uint64_t turn = 0, label = 0;                  // N_PlayerRotate_00, T_Location_00
        std::uint64_t arrow = 0;                            // the icon's N_Player_00
        bool arrow_shown = false;
        bool ready = false, name_set = false;
        std::optional<float> turned;
        std::string named;
        std::uint32_t rgb = 0;
        bool rgb_set = false;
    };
    bool Block(CoopNativeTags::Game& game);
    std::uint64_t Name(const char* name);   // the guest copy of a pane name, 0 when it is not one of ours
    std::uint64_t Find(CoopNativeTags::Game& game, std::uint64_t layout, const char* name);
    std::uint64_t Own(CoopNativeTags::Game& game, std::uint64_t table, const char* name);
    bool Ready(CoopNativeTags::Game& game, std::size_t i);
    void Show(CoopNativeTags::Game& game, std::uint64_t pane, bool shown);

    std::uint64_t screen_ = 0, layout_ = 0, clip_ = 0, table_ = 0;
    std::uint64_t block_ = 0;
    bool block_failed_ = false, up_ = false;
    std::uint64_t turn_at_ = kPaneTurn;
    std::string why_;
    std::vector<std::string> names_;
    std::array<Copy, kCopies> copies_{};
    std::vector<std::size_t> shown_;
};

}  // namespace CoopMapIcons
