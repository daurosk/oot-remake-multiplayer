// SPDX-License-Identifier: GPL-3.0-or-later
// The other players' name tags as the game's own balloons (BalloonMessage_00, the "Hey!" balloons over NPCs), not
// drawn by the overlay. docs/game-ui.md "The balloons" has the screen, its 16 parts and how a pane is moved and shown.
//
// Each frame on the game's thread (the frame hook): the overlay's name tag points (CoopHook::Markers, the game's
// camera) become points of the game's layout, the nearest 15 players get one balloon part each, from the last part
// down (the game takes its own from the first), with the name in the part's T_Text_00 (our text in a block of the
// game's own memory: the text box keeps the pointer), the player's colour on that text and the part's root pane moved
// there and shown. Parts we no longer need are hidden again. Nothing is drawn: no edge arrows, no dots (a player off
// screen has no tag, as an NPC off screen has no balloon).
//
// Split for tests (tests/hookshot/native_tags_test.cpp): the plan (Clean, LayoutPoint, PlanParts) is pure; the game
// steps go through the Game interface.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopNativeTags {

// Offsets in main (main build e0f3ccfaf50065ae; main is at CoopRuntime::MainBase(), 0x80020000 on Longshot). Each was
// read again on 1.1.1's main (native-tags-01112, tools/game_scan + a disassembly): f_8022a3e0 is the pane mover below,
// f_807906c0 makes the 16 parts (part 0 is the layout's own Pa_BalloonMessage_00, 1 to 15 copies of it), f_80235ec0
// (the balloons' projection) returns the point as NDC x 1920, y x 1080: the layout's canvas is 3840 x 2160, centre 0.
namespace addr {
inline constexpr std::uint64_t kBalloonVtable = 0x84c042e8 - 0x80020000;  // UIBalloonMessageScreen
inline constexpr std::size_t kParts = 16;
inline constexpr std::uint64_t kPartTable = 0xc60;    // screen+: 16 part objects, 0x20 apart (f_807906c0)
inline constexpr std::uint64_t kPartStride = 0x20;
inline constexpr std::uint64_t kScreenLayout = 0x28;  // screen+: its layout
inline constexpr std::uint64_t kPartRootPane = 0x80229650 - 0x80020000;  // (part + 0x40) -> the part's root pane
inline constexpr std::uint64_t kPaneShow = 0x8022aea0 - 0x80020000;      // (pane, 0/1)
inline constexpr std::uint64_t kPaneMarkA = 0x815457d0 - 0x80020000;     // what f_8022a3e0 calls after a move
inline constexpr std::uint64_t kPaneMarkB = 0x8154d540 - 0x80020000;
inline constexpr std::uint64_t kPaneByName = 0x8078e2b0 - 0x80020000;    // (layout, name) -> pane or 0
inline constexpr std::uint64_t kSetPaneText = 0x8045b970 - 0x80020000;   // (layout, name, &ref, 1, 0)
inline constexpr std::uint64_t kTextColour0 = 0x8152c740 - 0x80020000;   // (textbox, const float rgba[4])
inline constexpr std::uint64_t kTextColour1 = 0x8152c810 - 0x80020000;
inline constexpr std::uint64_t kMalloc = 0x14c6c90;                     // (bytes) -> guest memory
inline constexpr std::uint64_t kPaneXY = 0x6c;     // pane+: f32 x, y (the translation f_8022a3e0 sets)
inline constexpr std::uint64_t kPaneFlags = 0xa8;  // pane+: u32; bit 8 a pane placed elsewhere, bit 20 tell the layout
inline constexpr std::uint64_t kUiManagerGot = 0x84c9e098 - 0x80020000;  // **this: the UI manager
inline constexpr std::uint64_t kUiCount = 0x8;     // manager+: u32
inline constexpr std::uint64_t kUiScreens = 0x10;  // manager+: the screen pointers
inline constexpr float kHalfWidth = 1920.0f, kHalfHeight = 1080.0f;   // the 3840 x 2160 canvas, from its middle
}  // namespace addr

// The module's view of the game: guest memory and calls on the game's thread.
class Game {
public:
    virtual ~Game() = default;
    virtual std::uint64_t MainBase() const = 0;
    virtual bool Read(std::uint64_t address, void* out, std::size_t size) = 0;
    virtual bool Write(std::uint64_t address, const void* in, std::size_t size) = 0;
    // x0..x4; nothing when there is no function there.
    virtual std::optional<std::uint64_t> Call(std::uint64_t function, const std::array<std::uint64_t, 5>& x) = 0;
};

// A player's name as a game text may hold it: every byte below 0x20 (the game's tag byte 0x0E included) and DEL
// dropped, a bad UTF-8 sequence as '?', cut at a whole character within `max_bytes`.
std::string Clean(std::string_view utf8, std::size_t max_bytes);
inline constexpr std::size_t kNameBytes = 48;

// A name tag where the overlay put it: CoopHook::Marker's tag point in the scene's pixels (width x height).
struct Spot {
    std::string name;
    float x = 0.0f, y = 0.0f;  // the bottom centre of the name, from the top left, y down
    float alpha = 1.0f;
    std::uint32_t rgb = 0xFFFFFF;  // the player's colour
};
// A scene pixel as a point of the game's layout: centre 0, y up, +-1920 across and +-1080 up (the projection's).
std::array<float, 2> LayoutPoint(float x, float y, float width, float height);

// The game has 16 balloon parts; one is left for its own NPCs.
inline constexpr std::size_t kMaxTags = addr::kParts - 1;
// One part per tag: spots come far to near (as CoopHook::Markers), so the nearest get parts first, from the last
// part down; at most kMaxTags. Spots with no name, no alpha or no finite point are left out; a point off the
// canvas too (a tag there would be off screen).
struct PartTag {
    std::size_t part = 0;  // 1..15
    std::string name;      // cleaned
    std::array<float, 2> at{};
    std::uint32_t rgb = 0xFFFFFF;
};
std::vector<PartTag> PlanParts(const std::vector<Spot>& far_to_near, float width, float height);

// A screen the UI manager has, by its vtable (main offset); 0 when none.
std::uint64_t ScreenByVtable(Game& game, std::uint64_t vtable_offset);
// Moves a pane to a layout point as f_8022a3e0 does (x, y kept in the pane, then the layout told). False for a pane
// f_8022a3e0 would place another way (flag bit 8).
bool MovePane(Game& game, std::uint64_t pane, const std::array<float, 2>& at);
// The marking MovePane does after its write (the pane's dirty bit 6, then f_8154d540 tells the layout), for another
// word of the pane written (coop_map_icons.cpp: the arrow's turn).
bool MarkPaneMoved(Game& game, std::uint64_t pane);

// B8's probe (LICHII_COOP_MAP_PROBE=1, a run to learn how the game makes its map icons): the layouts a screen object
// points at straight from its first `bytes` (8-byte words), with the name each layout's resource gives (layout+0xf8
// -> resource, resource+0x18 -> its name), kept when the name has `contains`. Other pointers are left out by that
// name not being 4 or more printable characters.
inline constexpr std::uint64_t kMapMenuVtable = 0x84c08300 - 0x80020000;   // UIMapMenuScreen (the HUD and its maps)
struct Held {
    std::uint64_t offset = 0;  // screen+
    std::uint64_t layout = 0;
    std::string name;
};
std::vector<Held> LayoutsHeld(Game& game, std::uint64_t screen, std::size_t bytes, std::string_view contains);

// Keeps the tags on the game's balloons. Step each frame on the game's thread; HideAll when co-op stops showing tags.
class Balloons {
public:
    // False when the balloon screen is not there (the title, a load) or the game gave no memory for the names.
    bool Step(Game& game, const std::vector<Spot>& far_to_near, float width, float height);
    void HideAll(Game& game);
    std::size_t shown() const { return used_.size(); }
    std::uint64_t screen() const { return screen_; }
    bool no_memory() const { return block_failed_; }

    // The guest block: per part a 256-byte slot {ref (24 bytes), the name and its 0}, then 16 colour slots of 16
    // bytes (rgba floats), then the pane name "T_Text_00".
    static constexpr std::size_t kSlotBytes = 256;
    static constexpr std::size_t kBlockBytes = kSlotBytes * addr::kParts + 16 * addr::kParts + 16;

private:
    bool Block(Game& game);
    bool Name(Game& game, std::size_t part_index, std::uint64_t part, const std::string& name);
    void Colour(Game& game, std::size_t part_index, std::uint64_t part, std::uint32_t rgb);

    std::uint64_t screen_ = 0;
    std::uint64_t block_ = 0;
    bool block_failed_ = false;
    std::vector<std::uint64_t> used_;                       // the root panes shown last frame
    std::array<std::string, addr::kParts> named_{};         // the name each part was given
    std::array<std::uint64_t, addr::kParts> named_on_{};    // ... on which part object
    std::array<std::uint32_t, addr::kParts> rgb_{};         // the colour each part's text was given
    std::array<std::uint64_t, addr::kParts> rgb_on_{};
};

}  // namespace CoopNativeTags
