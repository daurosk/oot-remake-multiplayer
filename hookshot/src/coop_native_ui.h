// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's chat and name tags as the game's own UI on Hookshot (docs/native-ui.md).
//
// No overlay: the chat lines are the game's in-game message window
// (MessageInGame_00: the speaker's name over the line, the game's font, frame
// and animations), room notices are the game's banner (SimpleInfo_00), and
// the other players' names are the game's NPC balloons (BalloonMessage_00,
// sixteen of them) over their Links, placed with the game's own world to
// screen projection. The game hides all three itself in cutscenes and menus.
//
// How: the game shows a text pane's text from a message reference
// {const char* utf8, u32 bytes, i64 -1} and keeps the pointer, so our text
// lives in one block of guest memory from the game's malloc (TextRing) and the
// reference is handed to the game's own pane setter. The game functions and
// fields are in Addr (main build e0f3ccfaf50065ae, docs/game-builds.md).
//
// Split for tests: the pure parts (Clean, SplitSpeaker, Colour, ChatFeed,
// PlanTags, TextRing) and the game steps (ShowLine, ShowNotice, Project),
// which go through the Game interface (the module's guest memory and call).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CoopNativeUi {

// ---------------------------------------------------------------- the game
// Guest addresses in main of build e0f3ccfaf50065ae (Hookshot names them
// f_<address>; reports of native-chat-tags have how each was found).
namespace addr {
// The in-game message window (UIMessageInGameScreen, vtable 0x84c09678).
inline constexpr std::uint64_t kInGameScreenGlobal = 0x84ea4518;  // *this: the UI manager its getter takes
inline constexpr std::uint64_t kInGameScreenGet = 0x833d2690;     // (manager) -> the screen or 0
inline constexpr std::uint64_t kInGameVtable = 0x84c09678;
inline constexpr std::uint64_t kInGameText = 0x2b0;    // the message reference T_Message_00 is set from
inline constexpr std::uint64_t kInGameShow = 0x2d0;    // u8: 1 = set the text on the next step
inline constexpr std::uint64_t kInGameTime = 0x408;    // f32: game frames left (the game's default 180), closes at 0
inline constexpr float kFramesPerSecond = 30.0f;        // its step (f_802f12b0) is one per 30 fps frame
inline constexpr std::uint64_t kScreenOpenA = 0x1a5;   // s8 and u8: the game sets kInGameShow only when
inline constexpr std::uint64_t kScreenOpenB = 0x1a6;   //   A >= 1 and B == 0 (f_833d25c0)
// The banner (UISimpleInfoScreen, vtable 0x84c0d7d8).
inline constexpr std::uint64_t kInfoVtable = 0x84c0d7d8;
// The balloons (UIBalloonMessageScreen, vtable 0x84c042e8): 16 parts.
inline constexpr std::uint64_t kBalloonVtable = 0x84c042e8;
inline constexpr std::size_t kBalloonParts = 16;
inline constexpr std::uint64_t kBalloonPartTable = 0xc60;  // 16 x {part, type anim, in/out anim}, 0x20 apart (f_807906c0)
inline constexpr std::uint64_t kBalloonPartStride = 0x20;
inline constexpr std::uint64_t kPartRootPane = 0x80229650;  // (part + 0x40) -> the part's root pane
inline constexpr std::uint64_t kPaneShow = 0x8022aea0;      // (pane, 0/1)
inline constexpr std::uint64_t kPaneXY = 0x6c;               // f32 x, y: where f_8022a3e0 keeps them
inline constexpr std::uint64_t kPaneFlags = 0xa8;            // u32: bit 8 another kind of pane, bit 20 tell the layout
inline constexpr std::uint64_t kPaneMarkA = 0x815457d0;      // (pane, bits) and (pane): what f_8022a3e0 calls after
inline constexpr std::uint64_t kPaneMarkB = 0x8154d540;      //   a move, so the layout places it again
// The layout's size: the game's projection gives points in it, the centre at 0, y up.
inline constexpr float kLayoutWidth = 1920.0f;
inline constexpr float kLayoutHeight = 1080.0f;
// Every screen: its layout, and the UI manager's screen list.
inline constexpr std::uint64_t kScreenLayout = 0x28;
inline constexpr std::uint64_t kUiManagerGot = 0x84c9e098;  // **this: the UI manager
inline constexpr std::uint64_t kUiCount = 0x8;               // u32
inline constexpr std::uint64_t kUiScreens = 0x10;            // screen pointers
inline constexpr int kSlotOpen = 5;                          // vtable slot: open (screen, mode)
// A layout's text pane by name: (layout, name, &reference, 1, 0).
inline constexpr std::uint64_t kSetPaneText = 0x8045b970;
// Pane names the game already has in its read-only data.
inline constexpr std::uint64_t kNameT_Name_00 = 0x83d141c6;
inline constexpr std::uint64_t kNameT_Text_00 = 0x83de2306;
// World to screen (out xy in 1920x1080, &world xyz, &depth) -> bool, the
// balloons' own projection.
inline constexpr std::uint64_t kProject = 0x80235ec0;
// The game's malloc (CoopPuppet::addr::kMalloc, main-relative).
inline constexpr std::uint64_t kMallocMain = 0x14c6c90;
}  // namespace addr

// The module's view of the game: guest memory and calls on the game's thread.
class Game {
public:
    virtual ~Game() = default;
    virtual bool Read(std::uint64_t address, void* out, std::size_t size) = 0;
    virtual bool Write(std::uint64_t address, const void* in, std::size_t size) = 0;
    // x0..x4 and s0; nothing when there is no function there.
    virtual std::optional<std::uint64_t> Call(std::uint64_t function, const std::array<std::uint64_t, 5>& x,
                                              float s0 = 0.0f) = 0;
};

// ---------------------------------------------------------------- text
// The game's message text is UTF-8 with tags (byte 0x0E, then u16 group, u16
// type, u16 size, the parameters). Text from other players must never carry
// one: Clean drops every byte below 0x20 (0x0E included) and DEL, turns a bad
// UTF-8 sequence into '?', and cuts at a whole character within `max_bytes`.
std::string Clean(std::string_view utf8, std::size_t max_bytes);

// A chat line as the hook keeps it ("Saria: hello", "you: hi") split into the
// speaker and what was said. A line with no ": " has no speaker.
struct Said {
    std::string speaker;
    std::string text;
};
Said SplitSpeaker(std::string_view line);

// ---------------------------------------------------------------- colours
inline constexpr std::size_t kMaxPlayers = 16;
// A player's colour (0xRRGGBB) for player numbers 1 to 16: 1-4 as the
// protocol's slot colours (coop::protocol::slot_color), 5-16 spread between
// them; anything else white.
std::uint32_t Colour(std::uint8_t number);

// ---------------------------------------------------------------- chat
inline constexpr std::size_t kSpeakerBytes = 48;
inline constexpr std::size_t kTextBytes = 200;  // the protocol's chat limit is below this

struct ChatIn {           // CoopHook::ChatLine, without the hook
    std::string text;
    bool notice = false;
    std::uint64_t age_ms = 0;
};

struct Shown {
    std::string speaker;  // "" for a notice
    std::string text;
    bool notice = false;
    float seconds = 0.0f;  // how long the game shows it
    std::uint64_t came_ms = 0;  // when it came (ChatFeed)
};

// New chat lines in, one line at a time out, as fast as the game's window
// shows them. A line is new once: lines are told apart by text and the time
// they came (now - age, in steps of kSameMs). While one shows the rest wait;
// at most kWaiting wait (the oldest goes); lines older than kStaleMs when
// their turn comes are dropped, so a burst from 16 players never lags.
class ChatFeed {
public:
    static constexpr std::size_t kWaiting = 8;
    static constexpr std::uint64_t kSameMs = 500;
    static constexpr std::uint64_t kStaleMs = 20000;

    void Update(const std::vector<ChatIn>& lines, std::uint64_t now_ms);
    // The next line to show once the window is free at `now_ms`; it is then
    // busy for that line's seconds.
    std::optional<Shown> Next(std::uint64_t now_ms);
    // A line Next gave that could not be shown yet: first again, and the window not busy.
    void PutBack(Shown line);
    bool Busy(std::uint64_t now_ms) const { return now_ms < busy_until_; }
    std::size_t waiting() const { return waiting_.size(); }

    // How long a line shows: 3 s and 60 ms a character, at most 8 s.
    static float Seconds(std::size_t chars);

private:
    std::set<std::pair<std::string, std::uint64_t>> seen_;
    std::deque<Shown> waiting_;
    std::uint64_t busy_until_ = 0;
    bool primed_ = false;  // the lines there before the first Update are not shown again
};

// ---------------------------------------------------------------- tags
struct TagIn {
    std::uint8_t number = 0;
    std::string name;
    std::array<float, 3> head{};  // world position over the head
    bool hidden = false;          // other scene, in a cutscene, not drawn
};

struct Tag {
    std::uint8_t number = 0;
    std::string name;  // cleaned
    std::uint32_t colour = 0;
    std::array<float, 3> head{};
    float alpha = 1.0f;
};

// The game has 16 balloons; one is left for its own NPCs.
inline constexpr std::size_t kMaxTags = addr::kBalloonParts - 1;
inline constexpr float kTagRange = 60.0f;     // game units (about metres)
inline constexpr float kTagFadeFrom = 45.0f;  // full until here, then fades to the range

// The players to tag: not hidden, within kTagRange of `self`, nearest first,
// at most kMaxTags, faded with distance.
std::vector<Tag> PlanTags(const std::vector<TagIn>& players, const std::array<float, 3>& self);

// ---------------------------------------------------------------- guest text
// A message reference as the game reads it.
struct Ref {
    std::uint64_t text = 0;
    std::uint32_t bytes = 0;
    std::uint32_t pad = 0;
    std::int64_t end = -1;
};
static_assert(sizeof(Ref) == 24);

// Texts in one guest block, in turn: slot i holds a Ref then up to
// kSlotText bytes of text and its 0. A slot is used again only after
// kSlots others, long after its line has left the screen.
class TextRing {
public:
    static constexpr std::size_t kSlotBytes = 256;
    static constexpr std::size_t kSlotText = kSlotBytes - sizeof(Ref) - 1;
    static constexpr std::size_t kSlots = 48;
    static constexpr std::size_t kRingBytes = kSlotBytes * kSlots;
    static constexpr std::size_t kBlockBytes = kRingBytes + kSlotBytes * 16;  // the ring, then the fixed slots

    void SetBlock(std::uint64_t base) { base_ = base; }
    bool HasBlock() const { return base_ != 0; }
    // Whether a guest address is in the block (a text of ours).
    bool Owns(std::uint64_t address) const { return base_ != 0 && address >= base_ && address < base_ + kBlockBytes; }
    // Writes the text (cut to kSlotText at a character) and its Ref; the
    // Ref's guest address, or 0 when it could not be written.
    std::uint64_t Put(Game& game, std::string_view text);
    // A fixed slot after the ring (a tag's name: the game keeps showing it from there), written only
    // when the text changes. The Ref's address, or 0.
    static constexpr std::size_t kFixed = 16;
    std::uint64_t PutFixed(Game& game, std::size_t index, std::string_view text);

private:
    std::uint64_t base_ = 0;
    std::size_t next_ = 0;
    std::array<std::string, 16> fixed_{};
    std::array<bool, 16> fixed_set_{};
};

// ---------------------------------------------------------------- game steps
// The in-game message window, or 0 (not loaded: the title, a load).
std::uint64_t InGameScreen(Game& game);
// Whether the window may take a line of ours now: nothing of the game's own on it (its time is
// out, or the text on it is one of ours).
bool InGameFree(Game& game, std::uint64_t screen, const TextRing& ring);
// A screen the UI manager has, by its vtable; 0 when none.
std::uint64_t ScreenByVtable(Game& game, std::uint64_t vtable);

// Shows one chat line in the game's message window, as f_833d25c0 does with
// our text: the speaker in T_Name_00, the line as the window's message, its
// time, then open. False when the window is not there or a step failed.
bool ShowLine(Game& game, TextRing& ring, const Shown& line, bool refresh = false);
// `refresh`: a line that changes while it shows (the typed line): when the window is up, only the
// text and time change (no opening animation again).

// The line being typed as the window shows it: the text, then a caret.
Shown TypingLine(std::string_view speaker, std::string_view typed, bool failed);
inline constexpr float kTypingSeconds = 60.0f;  // kept up while typing (refreshed on each change)
// Ends a line of ours early (the box closed): its time to 0, the window closes on its next step.
// Nothing when the window shows something of the game's own.
bool EndLine(Game& game, const TextRing& ring);
// Shows a notice in the game's banner (T_Text_00), then open.
bool ShowNotice(Game& game, TextRing& ring, const Shown& notice);
// The game's projection of a world point: 1920x1080 screen point, or nothing
// when it is behind the camera or there is no camera.
std::optional<std::array<float, 2>> Project(Game& game, std::uint64_t scratch, const std::array<float, 3>& world);

// ---------------------------------------------------------------- tags on the game's balloons
// A name tag where the overlay would draw it: CoopHook::Marker's tag point in the pixels of the scene
// (width x height).
struct TagSpot {
    std::string name;
    float x = 0.0f, y = 0.0f;  // bottom centre of the name, from the top left, y down
    float alpha = 1.0f;
};
// A scene pixel as a point of the game's layout (centre 0, y up, addr::kLayoutWidth x kLayoutHeight).
std::array<float, 2> LayoutPoint(float x, float y, float width, float height);

// One balloon part per tag, from the last part down (the game takes its own from the first), at most
// kMaxTags; spots come far to near (as CoopHook::Markers), so the nearest get parts first.
struct PartTag {
    std::size_t part = 0;  // 0..15
    std::string name;      // cleaned
    std::array<float, 2> at{};  // layout point
};
std::vector<PartTag> PlanParts(const std::vector<TagSpot>& far_to_near, float width, float height);

// The balloon screen's 16 parts (0 where there is none).
std::array<std::uint64_t, addr::kBalloonParts> BalloonParts(Game& game, std::uint64_t screen);
// The part's root pane, or 0.
std::uint64_t PartRoot(Game& game, std::uint64_t part);
// Moves a pane to a layout point as f_8022a3e0 does (x, y kept in the pane, then the layout told).
bool MovePane(Game& game, std::uint64_t pane, const std::array<float, 2>& at);
bool ShowPane(Game& game, std::uint64_t pane, bool shown);

// Keeps the tags on the game's balloons: each frame on the game's thread. Parts it used and no longer
// needs are hidden again. False when the balloon screen is not there.
class Balloons {
public:
    bool Step(Game& game, TextRing& ring, const std::vector<TagSpot>& far_to_near, float width, float height);
    void HideAll(Game& game);
    std::size_t shown() const { return used_.size(); }

private:
    std::uint64_t screen_ = 0;
    std::vector<std::uint64_t> used_;  // the root panes shown last frame
    std::array<std::string, addr::kBalloonParts> named_{};       // the name each part was given
    std::array<std::uint64_t, addr::kBalloonParts> named_on_{};  // ... on which part object
};

}  // namespace CoopNativeUi
