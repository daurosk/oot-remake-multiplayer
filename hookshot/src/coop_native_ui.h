// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op's chat as the game's own UI (docs/native-ui.md): no overlay, nothing drawn.
//
// Everything is the game's banner (SimpleInfo_00: its W_Bg_00 frame, its T_Text_00 line in the game's font, its own
// in and out animation): the chat lines ("Saria: hi", in the speaker's colour, a long line in pages), the room notices
// ("Ruto joined"), the line being typed (the chat box, T: "you: hello_") and the held line of down but not out.
//
// Not the game's MessageInGame_00: that screen is the game's voice SUBTITLE line (docs/game-ui.md, "The message
// window"): no frame pane at all, its name plate shown only while the player's option "Subtitle Speaker-Name Display"
// is on and the whole line only while "Subtitle Display" is on. Chat there was a bare word at the bottom, no name
// (proof-01112-c run 3), and nothing at all with subtitles off.
//
// How: a game text pane shows its text from a message reference {const char* utf8, u32 bytes, i64 -1} and keeps the
// pointer, so our texts live in one block of guest memory from the game's malloc (TextRing), and the reference goes
// to the game's own pane setter (f_8045b970). The game's functions and fields are in addr, as main+ offsets of build
// e0f3ccfaf50065ae (game 1.1.1, the only one co-op supports), each checked on that build's main (docs/game-ui.md).
//
// Split for tests: the pure parts (Clean, SplitSpeaker, Columns, Width, Wrap, ChatFeed, TypingLine) and the game steps
// (TextTint, Banner), which go through the Game interface (the module's guest memory and its game call), so
// tests/hookshot/native_ui_test.cpp runs them on a pretend game.
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
// main+ offsets (main at 0x80020000 on Longshot and Hookshot). Each checked on the 1.1.1 main with tools/game_scan
// (docs/game-ui.md, "The banner", "Text colour").
namespace addr {
// The banner (UISimpleInfoScreen; what its constructor at 0x8069c778 stores in the object).
inline constexpr std::uint64_t kInfoVtable = 0x4bed7c8;       // 0x84c0d7c8
inline constexpr std::uint64_t kInfoOpenMode = 2;             // slot 5's mode (the game's f_83419b10: 2 or 3)
inline constexpr std::uint64_t kInfoType = 0x2b0;             // its "Type" animation (slot 93 f_80de2490 finds it)
inline constexpr std::uint64_t kAnimFirstFrame = 0x2d0fe0;    // f_802f0fe0 (animation): at its first frame, stopped
// A screen's request and state.
inline constexpr std::uint64_t kScreenRequest = 0x1a5;        // s8: open raises it, close lowers it
inline constexpr std::uint64_t kScreenState = 0x1a6;          // u8: 2 open (slot 72)
inline constexpr std::uint8_t kStateOpen = 2;
// Every screen: its layout; the UI manager's screens.
inline constexpr std::uint64_t kScreenLayout = 0x28;
inline constexpr std::uint64_t kUiManagerGot = 0x4c7e098;     // **this: the UI manager
inline constexpr std::uint64_t kUiCount = 0x8;                // u32
inline constexpr std::uint64_t kUiScreens = 0x10;             // the screens' pointers
inline constexpr int kSlotOpen = 5;                           // vtable slot: open (screen, mode), f_804d6100
inline constexpr int kSlotClose = 6;                          // vtable slot: close (screen, -1), f_802f6990
// Text panes.
inline constexpr std::uint64_t kSetPaneText = 0x43b970;       // f_8045b970 (layout, "Pane", &ref, 1, 0): @ alias too
inline constexpr std::uint64_t kPaneByName = 0x76e2b0;        // f_8078e2b0 (layout, "Pane") -> the pane, or 0
inline constexpr std::uint64_t kPaneFlags = 0xa8;             // u32: bits 22..25 the kind, 2 a text pane
inline constexpr std::uint64_t kSetColour0 = 0x150c740;       // f_8152c740 (text box, float rgba[4]): +0x1d4
inline constexpr std::uint64_t kSetColour1 = 0x150c810;       // f_8152c810 (text box, float rgba[4]): +0x1e4
inline constexpr std::uint64_t kBoxColour0 = 0x1d4;           // the two colours as the text box keeps them
inline constexpr std::uint64_t kBoxColour1 = 0x1e4;
inline constexpr std::uint64_t kBoxText = 0xf0;               // the text box's text pointer (f_80459680 keeps it there)
// Pane names the game has in its read-only data.
inline constexpr std::uint64_t kNameT_Text_00 = 0x3dc2306;    // "T_Text_00"
// The game's malloc (CoopPuppet::addr::kMalloc).
inline constexpr std::uint64_t kMalloc = 0x14c6c90;
}  // namespace addr

// The module's view of the game: guest memory, calls on the game's thread, and where main is.
class Game {
public:
    virtual ~Game() = default;
    virtual bool Read(std::uint64_t address, void* out, std::size_t size) = 0;
    virtual bool Write(std::uint64_t address, const void* in, std::size_t size) = 0;
    // x0..x4; nothing when there is no function there.
    virtual std::optional<std::uint64_t> Call(std::uint64_t function, const std::array<std::uint64_t, 5>& x) = 0;
    virtual std::uint64_t Main() const = 0;
    std::uint64_t At(std::uint64_t main_offset) const { return Main() + main_offset; }
};

// ---------------------------------------------------------------- text
// The game's message text is UTF-8 with tags (byte 0x0E, then u16 group, u16 type, u16 size, the parameters). Text
// from other players must never carry one: Clean drops every byte below 0x20 (0x0E too) and DEL (a tab or a line
// break becomes a space; with `keep_breaks` a '\n' stays, Wrap's own), turns a bad UTF-8 sequence into '?', and cuts
// at a whole character within `max_bytes`.
std::string Clean(std::string_view utf8, std::size_t max_bytes, bool keep_breaks = false);

// A chat line as the hook keeps it ("Saria: hello", "you: hi") split into the speaker and what was said. A line with
// no ": " has no speaker.
struct Said {
    std::string speaker;
    std::string text;
};
Said SplitSpeaker(std::string_view line);

// How many characters a text is, in columns: a Latin letter 1, a Chinese, Japanese or Korean one (and full-width
// forms) 2. Only for how long a page shows (ChatFeed::Seconds): how WIDE it shows is Width.
std::size_t Columns(std::string_view utf8);

// How wide a text shows on the game's text panes, in hundredths of an em (the font's size), each character by its
// advance in the game's own font once SetFontWidths has it, else by kGuess*: never less than the widest of the
// game's Latin fonts (Cetra Display Pro Regular and Medium, Clarimo UD PE Regular and DemiBold, read for this; "W"
// is 1.10 em, "i" 0.28), so a guessed line is never wider than it measures. A Chinese, Japanese or Korean character
// (and full-width forms) is 1 em: those fonts' own full-width advance.
std::size_t Width(std::string_view utf8);
inline constexpr std::size_t kGuessSpace = 30, kGuessNarrow = 40, kGuessSlim = 50, kGuessLower = 66,
                             kGuessUpper = 82, kGuessBroad = 100, kGuessWidest = 112, kWideWidth = 100;
// The game's own font file (an OpenType or TrueType font: the HUD pack's font-medium.otf, the Latin font of the
// game's Normal font set, coop_hud) read for each character's advance (its cmap and hmtx tables). False, and the
// guesses stay, when the bytes are not such a font. From any thread; once is enough.
bool SetFontWidths(const std::vector<std::uint8_t>& font);
// The advances read from a font (SetFontWidths' parse), by code point, in hundredths of an em; empty when the bytes
// are not a font it can read (the cmap needs format 4 or 12). For tests.
std::vector<std::pair<std::uint32_t, std::uint16_t>> FontWidths(const std::vector<std::uint8_t>& font);
void ResetFontWidths();   // the guesses again (tests)

// The text in pages of at most `lines` lines of at most `width` (Width) each (lines joined by '\n', which the game's
// text panes break at), broken at spaces where it can, inside a word only when the word alone is too long. Never
// empty for a text with something in it.
std::vector<std::string> Wrap(std::string_view clean, std::size_t width, std::size_t lines);

// What fits on the banner's line (SimpleInfo_00's T_Text_00 is 2100 x 86 at 0.8 in the 3840x2160 layout, inside its
// W_Bg_00 frame of 1880 x 136: one line). The bar is the game's own: its widest banner text in any language
// (LayoutMsg/SimpleInfo_00, read from the 1.1.1 romfs for this: German, French, Italian, Spanish and Polish lines of
// 57 to 62 letters, 27.3 em in Cetra Display Pro Medium, the font the pack carries; English's widest is 22.0 em) fits
// that pane on one line, so 26 em of ours does, whatever the window size (the layout scales as one). A page is never
// wider; NOT seen on screen yet (the LANE1 screenshot list in reports/menu-chat-01113.md).
inline constexpr std::size_t kBannerWidth = 2600, kBannerLines = 1;
inline constexpr std::size_t kNameWidth = 900;   // a speaker's name at most (the rest of the line is said)

// ---------------------------------------------------------------- chat
inline constexpr std::size_t kSpeakerBytes = 48;
inline constexpr std::size_t kTextBytes = 240;  // the protocol's chat limit is below this

struct ChatIn {            // CoopHook::ChatLine, without the hook (a notice already in the player's language)
    std::string text;
    bool notice = false;
    std::uint64_t age_ms = 0;
    std::uint32_t colour = 0;  // the speaker's colour, 0xRRGGBB; 0: the game's own
};

struct Shown {
    std::string speaker;  // "" for a notice
    std::string text;     // one page: what fits the banner's line after "speaker: "
    bool notice = false;
    bool first = true;          // the line's first page (a later page is never dropped as stale: its line has begun)
    float seconds = 0.0f;       // how long the banner shows it
    std::uint64_t came_ms = 0;  // when it came (ChatFeed)
    std::uint32_t colour = 0;   // the speaker's, 0xRRGGBB; 0: the game's own
};

// The page as the banner's line: "speaker: text", or the text alone.
std::string BannerText(const Shown& page);

// New chat lines in, one page at a time out, as fast as the banner shows them. A line is new once: lines are told
// apart by text and the time they came (now - age, in steps of kSameMs). A long line is several pages, in turn, each
// "speaker: " and what fits after it. While one shows the rest wait; at most kWaiting pages wait (the oldest goes); a
// line older than kStaleMs when its turn comes is dropped, every page of it (a line already begun shows to its end),
// so a burst from 16 players never lags.
class ChatFeed {
public:
    static constexpr std::size_t kWaiting = 16;
    static constexpr std::uint64_t kSameMs = 500;
    static constexpr std::uint64_t kStaleMs = 20000;

    void Update(const std::vector<ChatIn>& lines, std::uint64_t now_ms);
    // The next page to show once the banner is free at `now_ms`; it is then busy for that page's seconds.
    std::optional<Shown> Next(std::uint64_t now_ms);
    // A page Next gave that could not be shown yet: first again, and the banner not busy.
    void PutBack(Shown line);
    bool Busy(std::uint64_t now_ms) const { return now_ms < busy_until_; }
    std::size_t waiting() const { return waiting_.size(); }

    // How long a page shows: 3 s and 60 ms a column, at most 6 s.
    static float Seconds(std::size_t columns);

private:
    std::set<std::pair<std::string, std::uint64_t>> seen_;
    std::deque<Shown> waiting_;
    std::uint64_t busy_until_ = 0;
    bool primed_ = false;  // the lines there before the first Update are not shown again
};

// The line being typed as the banner shows it: the speaker, then the end of the typed text that fits the line after
// "speaker: " ("..." before it when it does not all fit), a caret, and "(not sent)" after a send that failed. A
// failed line is in kFailedColour (the text box's own colours, as a speaker's): it is plain at a glance that it did not
// go, the line kept for another Enter; the caller gives a line that did not fail the speaker's colour.
inline constexpr std::uint32_t kFailedColour = 0xFF5A4A;
inline constexpr float kTypingSeconds = 60.0f;  // kept up while typing (shown again before it runs out)
Shown TypingLine(std::string_view speaker, std::string_view typed, bool failed, std::string_view not_sent = "(not sent)");

// ---------------------------------------------------------------- guest text
// A message reference as the game reads it.
struct Ref {
    std::uint64_t text = 0;
    std::uint32_t bytes = 0;
    std::uint32_t pad = 0;
    std::int64_t end = -1;
};
static_assert(sizeof(Ref) == 24);

// Texts in one guest block, in turn: slot i holds a Ref, then up to kSlotText bytes of text and its 0. A slot is used
// again only after kSlots others, long after its line has left the screen. The block's last kScratchBytes are scratch
// for a call's arguments (a colour).
class TextRing {
public:
    static constexpr std::size_t kSlotBytes = 512;
    static constexpr std::size_t kSlotText = kSlotBytes - sizeof(Ref) - 1;
    static constexpr std::size_t kSlots = 48;
    static constexpr std::size_t kScratchBytes = 64;
    static constexpr std::size_t kFixed = 1;   // slots after the ring written in place (the held banner line)
    static constexpr std::size_t kBlockBytes = kSlotBytes * (kSlots + kFixed) + kScratchBytes;

    void SetBlock(std::uint64_t base) { base_ = base; }
    bool HasBlock() const { return base_ != 0; }
    // Whether a guest address is in the block (a text of ours).
    bool Owns(std::uint64_t address) const { return base_ != 0 && address >= base_ && address < base_ + kBlockBytes; }
    // Writes the text (cleaned again, cut to kSlotText at a character; '\n' kept) and its Ref; the Ref's guest
    // address, or 0 when it could not be written.
    std::uint64_t Put(Game& game, std::string_view text);
    // Fixed slot `index` (< kFixed) written in place with the text: its Ref's address, or 0. Its text pointer never
    // changes, so a line re-set every second (the held line's clock) never walks round the ring.
    std::uint64_t PutFixed(Game& game, std::size_t index, std::string_view text);
    std::uint64_t Scratch() const { return base_ == 0 ? 0 : base_ + kSlotBytes * (kSlots + kFixed); }

private:
    std::uint64_t base_ = 0;
    std::size_t next_ = 0;
};

// ---------------------------------------------------------------- game steps
// A screen the UI manager has, by its vtable (a main+ offset); 0 when none.
std::uint64_t ScreenByVtable(Game& game, std::uint64_t vtable_offset);
// What a screen holds now.
enum class Holds { kNothing, kOurs, kGames };
// The text box of a text pane of the screen's layout, by the game's name string at main+`pane_name`: *(*(pane+8)+8)
// for a pane of kind 2 (text); 0 when there is none.
std::uint64_t PaneTextBox(Game& game, std::uint64_t screen, std::uint64_t pane_name);

// A text pane in a player's colour (the text box's own two colours: the game's colour tag has only 6), and the
// game's own colours back. The game's colours are read once, before the first change.
class TextTint {
public:
    bool Set(Game& game, const TextRing& ring, std::uint64_t screen, std::uint64_t pane_name, std::uint32_t rgb);
    bool Restore(Game& game, const TextRing& ring);
    bool tinted() const { return tinted_; }

private:
    bool Colours(Game& game, const TextRing& ring, const float (&rgba)[8]);
    std::uint64_t box_ = 0;     // the pane's text box
    bool saved_ = false, tinted_ = false;
    float game_[8] = {};        // its two colours as the game had them
    std::uint32_t now_ = 0;
};

// ---------------------------------------------------------------- a held banner
// A line that stays on the game's banner while it is held (down but not out: "You are down  0:32"); chat and notices
// wait meanwhile. From any thread: the module's frame step (Banner::Step, the game's thread) shows it.
void HoldBanner(std::string_view text);   // held, or its text changed
void EndBanner();                         // let go: the banner closes, chat goes on
std::optional<std::string> HeldBanner();  // the held text, if any

// The game's banner (SimpleInfo_00), every write of ours to it. It is never taken from the game: while it is up with
// a text that is not ours (its T_Text_00 text box's pointer is not in our block and not the last one we set), a page
// waits and so does the held line; closing touches only a banner of ours. Each text goes up as the game's own
// f_83419b10 puts one: the ordinary look (its Type animation at its first frame), T_Text_00, then open.
class Banner {
public:
    // What the banner holds now (kNothing: shut, no open asked).
    Holds Holding(Game& game, const TextRing& ring);
    // A page (a chat line's page, a notice, the typed line) for its seconds from `now_ms`, in the speaker's colour
    // (a notice: the game's). Up with ours already: only the text (and colour) change, no opening again. False when
    // there is no banner, it shows the game's own, or the held line has it: the page waits.
    bool Show(Game& game, TextRing& ring, const Shown& page, std::uint64_t now_ms);
    // Our page ends now (the chat box closed): the banner closes if it is ours.
    void End(Game& game, const TextRing& ring);
    // Each frame on the game's thread with the held text: opens the banner when it is shut, sets T_Text_00 (a fixed
    // slot, in place) only when the text changed, closes it once let go; closes a page of ours whose time is over;
    // gives the game's own colour back once the banner is not ours. True while a held line is up or waits for the
    // game's own banner to end (pages wait).
    bool Step(Game& game, TextRing& ring, const std::optional<std::string>& held, std::uint64_t now_ms);
    bool up() const { return up_; }              // the held line is up
    bool showing() const { return page_; }       // a page of ours is up

private:
    bool Set(Game& game, std::uint64_t screen, std::uint64_t ref);
    void Close(Game& game, std::uint64_t screen);
    bool up_ = false, page_ = false;
    std::uint64_t until_ = 0;  // the page's time
    std::string shown_;
    std::uint64_t ours_ = 0;   // the text box's pointer after our last set (the game may copy the text)
    TextTint tint_;
};

}  // namespace CoopNativeUi
