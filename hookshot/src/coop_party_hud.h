// SPDX-License-Identifier: GPL-3.0-or-later
// The other players' hearts on the game's own HUD, from the game's own heart parts.
//
// The game's HUD (PlayerStatus_00, UIPlayerStatusScreen) has one heart gauge object. It makes 20 whole hearts (part
// layout PaHeartGaugeChildNoGauge_00) and one part heart (PaHeartGaugeChildGauge_00), and sets each with its
// stop-at-frame call (main+0x2d2d90) on the heart's animations, read on 1.1.1's main:
//   whole heart, f_833dca10(heart, state): 0 = BaseState 0, HeartState 0 (no heart there); 1 = BaseState 1, HeartState
//     0 (an empty heart); 2 = BaseState 1, HeartState 1 (a full heart);
//   part heart, f_833dcb60(heart, n) = HeartGauge stopped at n; the part heart is moved onto the heart partly filled.
// That object reads Link's own life and sets its hearts again itself, so they cannot show another player (the HUD
// probe's second run). The pack (tools/puppet_pack/party_hud.py) adds our own copies of the same two part layouts to
// PlayerStatus_00, hidden: kSlots players x (kHearts whole hearts + one part heart + one line of the game's text), and
// two more lines ("+N" and our own). This file places, shows and sets them with the same calls. No game object owns the copies, so
// nothing sets them back. Players past the pack's slots, or every player without a pack, get a run-time gauge (Live).
//
// Split for tests (tests/hookshot/party_hud_test.cpp): everything goes through Game.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "coop_native_ui.h"

namespace CoopPartyHud {

inline constexpr int kSlots = 15;      // at most: every other player of a full game (16 players); the pack has
                                       // party_hud.py SLOTS of them (Hud::slots_), the rest go to "+N"
inline constexpr int kHearts = 20;     // party_hud.py HEARTS
inline constexpr int kPerRow = 10;

// main+ offsets of build e0f3ccfaf50065ae (game 1.1.1).
namespace addr {
inline constexpr std::uint64_t kStatusVtable = 0x84c0b710 - 0x80020000;   // UIPlayerStatusScreen
inline constexpr std::uint64_t kAnimFind = 0x43d870;       // (layout, "part/animation") -> animation, 0 none
inline constexpr std::uint64_t kAnimPlayer = 0x20;         // animation+: its player
inline constexpr std::uint64_t kAnimLastFrame = 0x2d1340;  // (player) -> s32 its last frame
inline constexpr std::uint64_t kAnimStopAt = 0x2d2d90;     // (animation, s0 frame): stopped at that frame
inline constexpr std::uint64_t kPaneShow = 0x8022aea0 - 0x80020000;   // (pane, 0/1)
inline constexpr std::uint64_t kPaneAlpha = 0x2d8ac0;      // (pane, s0 alpha 0..1): its own alpha (coop_pause_tab.h)
inline constexpr std::uint64_t kPaneMarkA = 0x815457d0 - 0x80020000;  // what the game's pane mover calls after a move
inline constexpr std::uint64_t kPaneMarkB = 0x8154d540 - 0x80020000;
inline constexpr std::uint64_t kPaneXY = 0x6c;             // pane+: f32 x, y
// pane+: its global matrix, 3 x 4 by column, as moved for the screen's shape (coop_map_page.h kPaneMatrix, which
// GameHudBottom reads the game's HUD by; Longshot host/hos_wide_ui.c PANE_XX 0x28, the translation at PANE_X 0x4c)
inline constexpr std::uint64_t kPaneMatrix = 0x28;
inline constexpr std::uint64_t kPaneFlags = 0xa8;          // pane+: u32; bit 8 placed elsewhere, bit 20 tell the layout
// A text pane inside one of our part copies ("Pa_CoopName_0/T_Text_00"), set as the game's path text call sets each pane
// it matches (coop_pause_tab.h, the same calls run in the game for the pause pages' copies): f_8045b970 (kSetPaneText)
// takes no path, only a pane of the layout itself, so it set nothing on our labels (0.11.13 row 291).
inline constexpr std::uint64_t kPathFind = 0x43c4a0;       // (path, layout, &layout out) -> the name in the last part
inline constexpr std::uint64_t kPaneTable = 0x43c040;      // (layout + 0x40) -> its pane table
inline constexpr std::uint64_t kTablePane = 0x43e300;      // (table, name) -> the pane, 0 none
inline constexpr std::uint64_t kPaneSetMsg = 0x439520;     // (text pane, &{text, bytes, attributes}, 0)
inline constexpr std::uint64_t kPaneMsgAttrs = 0x10;       // text pane+: the message attributes it keeps
inline constexpr std::uint64_t kPaneTextWidth = 0x13c;     // text pane+: f32 the width its text was drawn
inline constexpr std::uint32_t kTextPaneKind = 2;          // pane flags bits 22..25: a text box
// Run-time gauges (R2/R3: a player without a pack copy), made as the status screen makes its own (f_834053e0 at
// 0x83405b70): the HUD's Pa_HeartGauge_00 part layout cloned (f_80791f70), a gauge object built on the clone
// (f_833db800) and set up (f_8078fd00: its 20 whole hearts Pa_NoGauge_00..19 and part hearts Pa_Gauge_00/01, each with
// its 0x78 controller in the layout's animation list). The gauge object's own update (vtable 0x84c011b8 slots 4/5,
// f_8019de20) reads Link's life from the game's data, so ours is taken out of that list at once (its base destructor
// f_80caaf70 only unlinks it) and its children are set with the game's own setters.
inline constexpr std::uint64_t kNew = 0x8005eca0 - 0x80020000;          // (align, bytes): the game's global new
inline constexpr std::uint64_t kClonePart = 0x80791f70 - 0x80020000;    // (0x98 object, part layout, name, 0, 0)
inline constexpr std::uint64_t kAttach = 0x80235dc0 - 0x80020000;       // ([parent+0x18], pane, parent)
inline constexpr std::uint64_t kGaugeMake = 0x833db800 - 0x80020000;    // (0x788 object, part layout)
inline constexpr std::uint64_t kGaugeSetUp = 0x8078fd00 - 0x80020000;   // (gauge, heap 0, 1)
inline constexpr std::uint64_t kGaugeUnlink = 0x80caaf70 - 0x80020000;  // (gauge): out of the animation list
inline constexpr std::uint64_t kHeartSet = 0x833dca10 - 0x80020000;     // (heart, 0 none / 1 empty / 2 full)
inline constexpr std::uint64_t kGaugeFrame = 0x833dcb60 - 0x80020000;   // (part heart, s32 HeartGauge frame)
inline constexpr std::uint64_t kCloneBytes = 0x98, kGaugeBytes = 0x788;
inline constexpr std::uint64_t kPartLayout = 0xa0;         // part pane+: its part layout
inline constexpr std::uint64_t kParentOwner = 0x18;        // pane+: what the attach call takes first
inline constexpr std::uint64_t kGaugeWhole = 0x30;         // gauge+: 20 whole hearts' controllers
inline constexpr std::uint64_t kGaugePart = 0x6c8;         // gauge+: the part heart's controller (then +0x6d0 a second)
inline constexpr std::uint64_t kHeartBase = 0x30;          // controller+: its BaseState animation (the setter's first)
inline constexpr std::uint64_t kHeartGauge = 0x58;         // controller+: HeartGauge (f_833dcb60)
inline constexpr std::uint64_t kPaneScale = 0x84;          // pane+: f32 scale x, y (after translate and rotate; NOT proven)
// No pack: the run-time name lines' text part, from the game's own Common archive (dump romfs UI/Layout2Archive/
// Common.Product.100 *.blarc, read with tools/puppet_pack/ui_pack.read_layout / part_layouts): MenuOverlay_00 (screens
// table UIScreen: LifetimeGroup Resident, ArchiveCategory Common) is the only layout with PaMenuCategory_00 parts
// (Pa_Category_00..03); PaMenuCategory_00's panes: RootPane, N_All_00, N_BgIn_00, N_Bg_00, P_MinusMenuBg_00,
// P_PlusMenuBg_00, N_MenuChangeTextIn_00, N_Text_00, T_Text_00, T_Text_01 (the same two text panes as
// PaEnemyLocation_00, the pack's line). MenuOverlay_00's own top-level text T_BtnL_00 tells its layout from another.
// PaEnemyLocation_00 itself is only in EnemyInfo_00 (Pa_Location_00/01), a screen that is NOT Resident.
inline constexpr const char* kBareScreen = "MenuOverlay_00";
inline constexpr const char* kBarePart = "Pa_Category_00";
inline constexpr const char* kBareCheck = "T_BtnL_00";
inline constexpr const char* kBareHide[] = {"N_BgIn_00", "N_Bg_00", "P_MinusMenuBg_00", "P_PlusMenuBg_00"};
}  // namespace addr

// Where the copies go, in the frame of their parent (N_ScaleL_00: the picture's top left corner, y up; the layout is
// 3840 x 2160). The game's own first heart is at (206, -154) there and its hearts are 78 apart, its rows 61
// (PaHeartGauge_00: Pa_NoGauge_00, Pa_Gauge_00 and Pa_Born_00 sit one heart and one row apart). Ours are kScale of that.
inline constexpr float kScale = 0.75f;                     // party_hud.py HEART_SCALE
inline constexpr float kFirstX = 206.0f - 78.0f * (1.0f - kScale) / 2.0f;
inline constexpr float kStepX = 78.0f * kScale;
inline constexpr float kStepY = 61.0f * kScale;
inline constexpr float kLabelHeight = 76.0f * kScale;      // party_hud.py LABEL_SIZE x LABEL_SCALE
// The lines are LEFT-aligned on the hearts' left edge, as 0.11.12's drawn blocks (coop_draw DrawHud at 8a99851f6: the
// name at the hearts' x, cut with "..." to 30% of the picture's width). The pack's text part (PaEnemyLocation_00) centres
// its text on the pane, so the pane's middle goes half the line's width right of that edge (LabelX). Until hud-fit-0120
// the middle was fixed (541): every line was centred over left-aligned hearts, and the longest one (16 W, "(talking)"
// and "Inside Ganondorf's Castle", 34.7 em) ran 81 px off the screen's left edge (gate-hud shot of 2f734963a, A).
inline constexpr float kLabelLeft = kFirstX - kStepX / 2.0f;   // the first heart's left edge
// The line's em in the parent's units: 14.0 to 15.0 px an em of the 1280 x 720 gate-hud shot of the 3840 layout
// ("Twenty 45/80" 6.27 em 91 px; "(talking)  Inside Ganondorf's Castle 13/40" 18.3 em 273 px; CoopNativeUi::Width with
// the game's font-medium.otf). The larger, so a line is never cut too late.
inline constexpr float kLabelEm = 45.0f;
inline constexpr float kLabelRoom = 0.3f * 3840.0f;            // a line at most 30% of the picture wide (0.11.12's)
inline constexpr float kBlockGap = 45.0f;   // between blocks: a name sits on its own hearts, apart from the hearts
                                            // above it (14 before: a name was as near the last block's hearts)
inline constexpr float kDefaultTop = -330.0f;              // under two rows of hearts and the magic bar
inline constexpr float kLayoutHeight = 2160.0f;
inline constexpr float kLayoutWidth = 3840.0f;

// Row 326 (owner 2026-11-28, split screen halves 8:9 with patch 0047): on a picture narrower than 16:9 Longshot draws
// the HUD's 3840 x 2160 canvas smaller (host/hos_wide_ui.c hos_wide_ui_tick: the projection's rows times k < 1) and
// moves the HUD's corner panes out to the picture's corners, up by 1080 (1/k - 1) and in at the sides; N_ScaleL_00
// moves with them. The game's HUD bottom (GameHudBottom) is a fraction of the canvas's 2160 from its 16:9 top (+1080);
// read as if N_ScaleL_00 still sat there, our blocks went up by that move, into the game's own hearts. So both are
// taken in N_ScaleL_00's frame as it is now: its global matrix's top-left (tx, ty) and scale (sx, sy), in the canvas's
// units, y up from its middle. The 16:9 picture (and a wider one: the HUD keeps the middle 16:9) has it at (-1920, 1080)
// scale 1, which gives the values before row 326 exactly.
struct ParentFrame {
    float left = -kLayoutWidth / 2, top = kLayoutHeight / 2;   // the parent's origin on the canvas (y up)
    float scale_x = 1.0f, scale_y = 1.0f;
};
// The frame from N_ScaleL_00's global matrix (m[0] sx, m[4] sy, m[9] tx, m[10] ty); the 16:9 one when the matrix is
// not believable (not finite, a scale outside 0.25..4, or an origin not in the canvas's top-left quarter or beyond it).
ParentFrame FrameFrom(const float m[12]);
// The picture's height and width in the parent's units: the parent sits at the picture's top-left corner (the HUD's
// corner panes are moved to it), and the picture is centred on the canvas's middle.
float PictureHeight(const ParentFrame& f);
float PictureWidth(const ParentFrame& f);
inline constexpr float kDimAlpha = 150.0f / 255.0f;        // a player not connected: as the drawn blocks (coop_draw)

// The module's game: the chat's, and a call with a float in s0.
class Game : public CoopNativeUi::Game {
public:
    virtual std::optional<std::uint64_t> CallFloat(std::uint64_t function, std::uint64_t x0, float s0) = 0;
};

// One other player.
struct Block {
    std::string text;            // the line over the hearts (name, place, voice)
    std::vector<int> hearts;     // quarters filled in each (CoopHudHearts::Quarters)
    bool dim = false;            // not connected (CoopHudDim): hearts and line at kDimAlpha
    bool double_defence = false; // the Great Fairy's double defence (PlayerStatus.IsDefenseUP; see kDefenceUpHash)
};

// PlayerStatus.IsDefenseUP (docs/items.md): shared by every player (game_items.cpp, R::any), so our own value is
// everyone's. Not in race mode (coop_race_mode.h: no flag is shared, and no message carries another player's): then
// nobody's hearts show it but our own game's.
inline constexpr std::uint32_t kDefenceUpHash = 0x56bbe07cu;

// A player's hearts as the game sets its own: each whole heart's state (0 none, 1 empty, 2 full), and the part heart.
struct Plan {
    std::array<std::uint8_t, kHearts> whole{};
    int part_at = -1;        // the heart the part heart sits on, -1 none
    int part_quarters = 0;   // 1..3
    int hearts = 0;          // shown (at most kHearts)
    friend bool operator==(const Plan&, const Plan&) = default;
};
Plan PlanHearts(const std::vector<int>& quarters);

// HeartGauge's frame for 1..4 quarters on an animation with this last frame. The game's damage path gives 0..3 (a
// fill up to 1/4 -> 0, up to 1/2 -> 1, up to 3/4 -> 2, more -> 3); the HUD probe's first run read a last frame of 100
// on the part heart's own layout, quarters at 25, 50, 75, 100. Both are kept: a short animation takes the game's
// 0..3, a long one the fraction of its length.
float GaugeFrame(int quarters, std::int32_t last_frame);

// Low life, as the game's own pinching hearts start (the original game's rule, Health_IsCritical: at most 1 heart left
// with up to 5, 1 1/2 with up to 10, 2 with up to 15, 2 3/4 above; not read from this game yet). Nothing left (0) is
// not low: the player is down.
bool LowLife(int life_quarters, int max_quarters);
// The heart a change of life glows on (damage: the heart that lost, heal: the heart that filled), -1 none.
int GlowHeart(int was_quarters, int now_quarters, int hearts);
// The frame of a played animation `ms` after it began, at the layouts' 30 frames a second: held at `last` (once), or
// round again (`loop`).
float PlayedFrame(std::uint64_t ms, std::int32_t last, bool loop);
inline constexpr std::uint64_t kGlowMs = 600;     // a glow's time when its animation has no last frame we could read
inline constexpr float kGlowState = 3.0f;         // GlowState while glowing (f_833dc830)

// Where a heart of a block goes, and a block's height, in the parent's frame; `top` is the block's top edge.
std::array<float, 2> HeartAt(float top, int index);
float BlockHeight(int hearts);
// The first block's top from the bottom of the game's own HUD (GameHudBottom: a fraction of the canvas's 2160 down
// from its 16:9 top), in the parent's frame `frame` as it is now (row 326); kDefaultTop none or not on the picture.
// `own`: our own line ("You" and our voice state) goes first, so the blocks start one line lower.
float TopFrom(std::optional<double> game_hud_bottom, bool own = false, const ParentFrame& frame = {});

// A line as it shows: cut at its end with "..." to `room` (CoopNativeUi::Width, the game's font), so every line is
// on the picture whatever the name, voice words, place and language; at least "...". kLabelRoom on the 16:9 picture;
// LabelRoom(frame) 30% of the picture's width as it is now (row 326).
std::string FitLabel(const std::string& text, float room = kLabelRoom);
float LabelRoom(const ParentFrame& frame);
// Where a line's pane goes so its (fitted) text starts at kLabelLeft: the pane's middle, in the parent's units.
float LabelX(const std::string& fitted);
// "+N" for the players not shown (no slot or no room left on the picture); "" none.
std::string MoreText(std::size_t hidden);
// How many of the blocks (their heart counts, in order) are shown from `top` down: at most `slots`, every one on the
// picture, and when some are left out the "+N" line under them too (0.11.12's rule, CoopHudLayout::BlocksThatFit).
std::size_t BlocksThatFit(float top, const std::vector<int>& hearts, std::size_t slots, const ParentFrame& frame = {});

class Hud {
public:
    // Each frame on the game's thread: the blocks shown on the game's HUD. False when the HUD has no copies of ours
    // (an older pack) or is not there now: the other players' hearts are hidden then (nothing is drawn instead).
    // `log` gets a line for each thing learnt once (found or not, last frames).
    // `now_ms` (any steady clock) plays the glow on a change of life and the pinch at low life.
    // `own`: our own line over the blocks ("You", our voice state as words), "" none; shown only with blocks.
    bool Step(Game& game, const std::vector<Block>& blocks, std::optional<double> game_hud_bottom,
              std::vector<std::string>* log = nullptr, std::uint64_t now_ms = 0, const std::string& own = {});
    bool native() const { return native_; }
    // A test's switch (COOP_TEST_PARTY_HUD_NO_PACK=1, scenario hs-party-hud-probe): the pack's panes are not looked
    // for, so a game with the pack shows what a game without it shows (run-time gauges, no name lines).
    void IgnorePack(bool ignore) { ignore_pack_ = ignore; }
    // Run-time gauges and name lines (MakeLive, MakeLiveName: the game's part clone f_80791f70 on the HUD's
    // Pa_HeartGauge_00 / a text part) for players past the pack's slots. OFF in the module (party-hud-crash-c126,
    // reports/party-hud-crash-c126-4.md): the game calls that clone only inside a screen's own set-up, while its
    // layout's archive is built; called from the module afterwards it read data that was no longer the layout's, and
    // every game that ran it crashed in it (6 of 6, at menu -> loading, at a load's end and at play's start). With it
    // off the pack's slots show (hearts and names) and the rest go to "+N", as in 0.11.12. On only in the tests of the
    // clone path itself (tests/hookshot/party_hud_test.cpp), until a clone made in the screen's own set-up is proven.
    void AllowRunTimeClones(bool on) { clones_ = on; }

private:
    struct Pane {
        std::uint64_t pane = 0;
        std::uint64_t base = 0, heart = 0, gauge = 0;   // its animations
        std::int32_t gauge_last = 0;
        std::uint64_t glow_state = 0, glow = 0, pinch = 0;   // the glow and pinch animations (0: not there)
        std::int32_t glow_last = -1, pinch_last = -1;
        float glow_state_at = -1.0f, glow_at = -1.0f, pinch_at = -1.0f;
        // Double defence's look (each one there: at its last frame with it, 0 without)
        std::array<std::uint64_t, 2> defence{};
        std::array<std::int32_t, 2> defence_last{};
        std::array<float, 2> defence_at{-1.0f, -1.0f};
        int shown = -1;                                 // what was last set: -1 unknown
        float alpha = -1.0f;
        int state = -1;
        float frame = -1.0f;
        std::array<float, 2> at{};
        bool placed = false;
    };
    struct Label {
        std::uint64_t pane = 0;
        int shown = -1;
        float alpha = -1.0f;
        bool placed = false;
        std::array<float, 2> at{};
        std::string text;
        bool has_text = false;
        std::array<std::uint64_t, 2> boxes{};   // its text panes T_Text_00, T_Text_01 (0: not found)
        bool said_set = false, said_width = false;   // (logged once per layout)
        int drawn_frames = 0;                        // frames since the text was set, until its width is logged
    };
    bool Find(Game& game, std::uint64_t layout, std::vector<std::string>* log);
    void Show(Game& game, Pane& p, bool shown);
    void Show(Game& game, Label& l, bool shown);
    void Set(Game& game, Pane& p, int state, float frame, const std::array<float, 2>& at, float alpha);
    void Effects(Game& game, Pane& p, bool glowing, std::uint64_t glow_ms, bool pinch, std::uint64_t now_ms,
                 bool defence);
    static void StopAt(Game& game, std::uint64_t anim, float& had, float frame);
    // A line at height `y` (its middle), left-aligned at kLabelLeft and fitted to kLabelRoom.
    void SetLabel(Game& game, std::size_t index, const std::string& text, float y, float alpha,
                  std::vector<std::string>* log);
    std::uint64_t TextPane(Game& game, std::uint64_t layout, const std::string& path, std::string& why);
    static void Alpha(Game& game, std::uint64_t pane, float& had, float alpha);
    std::uint64_t Name(Game& game, const std::string& text);

    int slots_ = 0;                // the pack's complete slots on this layout (an older pack: fewer than kSlots)
    bool said_loading_ = false;    // the last scene-load state logged (CoopNativeUi::SceneLoading)
    std::uint64_t layout_ = 0;     // the layout the panes below were found on (the HUD made again: found again)
    bool native_ = false;
    std::uint64_t block_ = 0;      // guest memory of ours: a name's scratch, then the lines' texts
    std::array<std::array<Pane, kHearts>, kSlots> hearts_{};
    std::array<Pane, kSlots> gauges_{};
    struct Glow {
        std::string who;          // whose life it was (another player in the slot: no glow for the move)
        int life = -1;            // their last life in quarters, -1 none
        int heart = -1;           // glowing on this heart since `since`
        std::uint64_t since = 0;
    };
    std::array<Glow, kSlots> glows_{};
    std::array<Label, kSlots + 2> labels_{};   // then "+N", then our own line

    // A run-time gauge (a slot past the pack's, or every slot without a pack): made the first time a player needs it,
    // never in solo. It hangs in the HUD's layout: when that layout goes (scene, title, HUD made again) its panes go with
    // it and ours are forgotten (Drop), never freed (the gauge object and clone, 0x820 bytes, stay: freeing them while
    // a layout could still hold the clone's panes would be worse than that).
    struct Live {
        bool tried = false;
        std::uint64_t obj = 0, gauge = 0, root = 0;
        std::array<std::uint64_t, kHearts> whole{}, at{};   // controllers, and their part panes in the clone
        std::uint64_t part = 0, part_pane = 0, part2_pane = 0;
        std::int32_t gauge_last = -1;
        std::array<int, kHearts> state{};
        int part_frame = -1, shown = -1, part_shown = -1, part_on = -1;
        float alpha = -1.0f;
        std::array<float, 2> root_at{};
        bool placed = false;
        std::uint64_t name_root = 0;                  // its name line's clone root (MakeLiveName; 0: none)
        std::array<std::uint64_t, 2> name_boxes{};    // and its text panes, given back to labels_ on a kept layout
    };
    bool MakeLive(Game& game, int slot, std::vector<std::string>* log);
    void StepLive(Game& game, int slot, const Plan& plan, float top, float alpha);
    void HideLive(Game& game, Live& v);
    void MakeLiveName(Game& game, int slot, std::vector<std::string>* log);
    void Drop(Game& game);
    std::uint64_t gauge_part_ = 0;   // the HUD's Pa_HeartGauge_00 part layout (0: no run-time gauges)
    std::uint64_t parent_ = 0;       // N_ScaleL_00
    std::uint64_t frame_pane_ = 0;   // N_ScaleL_00 whether run-time clones are on or not: its frame (row 326)
    ParentFrame frame_{};            // its frame this frame (the 16:9 one until read)
    ParentFrame said_frame_{};       // the frame last logged
    bool frame_said_ = false;
    int frame_says_ = 0;
    bool ignore_pack_ = false;
    bool clones_ = false;          // AllowRunTimeClones
    std::uint64_t name_part_ = 0;    // the pack's line of the game's text (PaEnemyLocation_00), else the game's (kBarePart)
    bool name_bare_ = false;         // name_part_ is the game's kBarePart: its backing panes are hidden on each clone
    std::uint64_t BarePart(Game& game, std::uint64_t hud_layout, std::string& why);
    std::array<float, 2> source_at_{};   // Pa_HeartGauge_00's place there
    std::array<Live, kSlots> live_{};
    std::uint64_t live_layout_ = 0;  // the layout live_ hangs in
};

// Test builds, COOP_TEST_PARTY_HUD=1 (scenario hs-party-hud): the blocks of four made-up players, their life going
// down a quarter of a heart every kTestStepMs and round again, so one game alone shows every fill on screenshots;
// the third ("Three") not connected (dimmed); the second has the longest line a player can have (it must fit).
inline constexpr std::uint64_t kTestStepMs = 1500;
std::vector<Block> TestBlocks(std::uint64_t now_ms);
// Our own line then ("You" and a voice state), so the shot shows it over the blocks.
std::string TestOwn();

}  // namespace CoopPartyHud
