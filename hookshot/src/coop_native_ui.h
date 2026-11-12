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
inline constexpr std::uint64_t kInGameTime = 0x408;    // f32: counts down, closes at 0
inline constexpr std::uint64_t kScreenOpenA = 0x1a5;   // s8 and u8: the game sets kInGameShow only when
inline constexpr std::uint64_t kScreenOpenB = 0x1a6;   //   A >= 1 and B == 0 (f_833d25c0)
// The banner (UISimpleInfoScreen, vtable 0x84c0d7d8).
inline constexpr std::uint64_t kInfoVtable = 0x84c0d7d8;
// The balloons (UIBalloonMessageScreen, vtable 0x84c042e8): 16 parts.
inline constexpr std::uint64_t kBalloonVtable = 0x84c042e8;
inline constexpr std::size_t kBalloonParts = 16;
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
    bool Busy(std::uint64_t now_ms) const { return now_ms < busy_until_; }
    std::size_t waiting() const { return waiting_.size(); }

    // How long a line shows: 3 s and 60 ms a character, at most 8 s.
    static float Seconds(std::size_t chars);

private:
    struct Waiting {
        Shown shown;
        std::uint64_t came_ms = 0;
    };
    std::set<std::pair<std::string, std::uint64_t>> seen_;
    std::deque<Waiting> waiting_;
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
    static constexpr std::size_t kBlockBytes = kSlotBytes * kSlots;

    void SetBlock(std::uint64_t base) { base_ = base; }
    bool HasBlock() const { return base_ != 0; }
    // Writes the text (cut to kSlotText at a character) and its Ref; the
    // Ref's guest address, or 0 when it could not be written.
    std::uint64_t Put(Game& game, std::string_view text);

private:
    std::uint64_t base_ = 0;
    std::size_t next_ = 0;
};

// ---------------------------------------------------------------- game steps
// The in-game message window, or 0 (not loaded: the title, a load).
std::uint64_t InGameScreen(Game& game);
// A screen the UI manager has, by its vtable; 0 when none.
std::uint64_t ScreenByVtable(Game& game, std::uint64_t vtable);

// Shows one chat line in the game's message window, as f_833d25c0 does with
// our text: the speaker in T_Name_00, the line as the window's message, its
// time, then open. False when the window is not there or a step failed.
bool ShowLine(Game& game, TextRing& ring, const Shown& line);
// Shows a notice in the game's banner (T_Text_00), then open.
bool ShowNotice(Game& game, TextRing& ring, const Shown& notice);
// The game's projection of a world point: 1920x1080 screen point, or nothing
// when it is behind the camera or there is no camera.
std::optional<std::array<float, 2>> Project(Game& game, std::uint64_t scratch, const std::array<float, 3>& world);

}  // namespace CoopNativeUi
