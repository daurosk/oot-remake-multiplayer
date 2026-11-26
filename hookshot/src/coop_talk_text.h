// SPDX-License-Identifier: GPL-3.0-or-later
// The texts the game puts in its text panes, kept for the play-test bridge's `text` (docs/playtest.md, "Text boxes").
//
// Patch 0041 wraps the game's pane text setter (main+0x439520, kPaneSetMsg: x0 the pane, x1 a reference {text, length
// in bytes, attributes}). The text box keeps the pointer and the length, not a copy (seen in a run, 2026-11-19), so the
// text is copied at the call. Each kept text names its layout and pane, read from the pane as Longshot's
// host/hos_wide_ui.c reads them: a pane is 0xb0 bytes in its layout's array (+0x100), +0x18 is its layout, the
// layout's +0xf8 its file header (+0x18 its name, +0x20 the pane names: a char* each, in the array's order). The
// message window is MessageInGame_00 (T_MessageWhiteOut_00 the text, T_Name_00 the speaker; docs/game-ui.md).
//
// Patch 0042 wraps the two setters that skip 0041's call and call the text box's set-string (main+0x439680)
// themselves (read from main, 2026-11-23; not yet seen in a run): kPaneSetBody, a letter-animated pane's set-text
// (x0 the pane, x1 the text, w2 its length: a talk's box text, which a lane run did not see through 0041), and
// kPaneSetAlias, the set-text of each pane with a matching alias (x0 the pane, x1 the same reference as 0041's).
// Each kept text says which wrap gave it.
//
// The window sets its text again every frame while it is open (docs/game-ui.md, "The message window"), so a text the
// pane already shows is not kept again, unless the pane was not set for more than kSameAgain frames (a later talk
// that puts the same text in the same pane: the speaker's name, the same button); a text that only grows (a box typed out letter by letter, if the game does
// that: not seen yet) replaces its shorter self. Game-free: the reads go through a function, so the unit tests
// (tests/hookshot/talk_text_test.cpp) feed a fake memory.
#pragma once

#include "hos_coop_pane_body.h"
#include "hos_coop_pane_text.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace CoopTalkText {

// The pane's layout in the game (docs/game-ui.md "Layouts and panes"; Longshot's host/hos_wide_ui.c).
inline constexpr std::uint64_t kPaneLayout = 0x18;     // pane: its layout
inline constexpr std::uint64_t kLayoutRes = 0xf8;      // layout: its file header
inline constexpr std::uint64_t kLayoutPanes = 0x100;   // layout: the pane array
inline constexpr std::uint64_t kResName = 0x18;        // header: the layout's name (char*)
inline constexpr std::uint64_t kResPaneNames = 0x20;   // header: the pane names (char* each)
inline constexpr std::uint64_t kPaneSize = 0xb0;      // a pane in the layout's array
// The setter's reference (x1): +0 the text, +8 its length in bytes (u32), +0x10 the attributes.
inline constexpr std::uint64_t kRefText = 0x0;     // reference: the text (char*)
inline constexpr std::uint64_t kRefLength = 0x8;   // reference: its length in bytes (u32)

// The setters wrapped besides 0041's (kPaneSetMsg in coop_pause_tab.h, wrap 25; main offsets, docs/game-ui.md "Text").
inline constexpr std::uint64_t kPaneSetBody = 0x76a460;   // a letter-animated pane's set-text (pane, text, u16
                                                          // length): patch 0042, wrap 26; called only from a
                                                          // LetterAnimControl's vtable slot 7 (main+0x76a390)
inline constexpr std::uint64_t kPaneSetAlias = 0x377a990;  // each pane with a matching alias (pane, &ref): patch
                                                           // 0042, wrap 27

inline constexpr std::size_t kKeep = 256;        // texts kept (the oldest go first)
inline constexpr std::size_t kMaxBytes = 2048;   // of one text (a longer one is cut and says so)
inline constexpr std::size_t kMaxName = 64;      // of a layout or pane name
inline constexpr std::size_t kMaxPanes = 4096;   // panes remembered for the "already shows it" test
inline constexpr std::uint64_t kSameAgain = 30;  // frames a pane went unset before its same text is kept again

// Reads guest memory: false when the address cannot be read.
using ReadFn = std::function<bool(std::uint64_t addr, void* out, std::size_t size)>;

struct Entry {
    std::uint64_t seq = 0;     // 1, 2, 3, ...: the newest has the highest
    std::uint64_t frame = 0;   // the bridge's frame count when it was set
    std::string layout;        // the layout's name ("" when it could not be read)
    std::string pane;          // the pane's name ("" when it could not be read)
    std::string text;          // the bytes as the game gave them (its trailing 0s dropped)
    std::uint32_t length = 0;  // the length the game gave
    bool cut = false;          // longer than kMaxBytes: `text` is its start
    // The wrap that gave it: HOS_COOP_WRAP_PANE_TEXT (set-message), _PANE_BODY (letter-animated) or
    // _PANE_ALIAS (by alias).
    std::uint32_t wrap = HOS_COOP_WRAP_PANE_TEXT;
};

class Log {
   public:
    // The wrap's phase 0: copies the text the setter is given (`pane`, `ref` as x0 and x1) and keeps it when the pane
    // did not show it already, or was not set for more than kSameAgain frames. Any read that fails keeps nothing (or an empty name). Thread-safe.
    void Note(std::uint64_t pane, std::uint64_t ref, std::uint64_t frame, const ReadFn& read,
              std::uint32_t wrap = HOS_COOP_WRAP_PANE_TEXT);
    // The same with the text and its length given as they are (wrap 26: x1, w2; only the low 16 bits of the length
    // count, as the game takes them).
    void NoteText(std::uint64_t pane, std::uint64_t text, std::uint64_t length, std::uint64_t frame, const ReadFn& read,
                  std::uint32_t wrap);
    // The kept texts after `since` (0: all kept), oldest first, of the layouts whose name contains `layout` (empty:
    // every layout). `lost`: texts after `since` that are no longer kept (more than kKeep came since).
    std::vector<Entry> Since(std::uint64_t since, std::string_view layout, bool* lost = nullptr) const;
    std::uint64_t Last() const;   // the newest seq (0: none yet)
    std::uint64_t Seen() const;   // setter calls looked at
    std::size_t PanesTimed() const;   // panes whose last set frame is remembered (at most kMaxPanes)
    void Clear();

   private:
    void Keep(std::uint64_t pane, Entry e, bool again);
    mutable std::mutex mu_;
    std::deque<Entry> kept_;
    std::unordered_map<std::uint64_t, std::string> shown_;      // pane -> its text now
    std::unordered_map<std::uint64_t, std::uint64_t> last_of_;  // pane -> the seq of its newest kept text
    std::unordered_map<std::uint64_t, std::uint64_t> set_at_;   // pane -> the newest frame it was set (any text)
    std::uint64_t seq_ = 0, seen_ = 0;
    std::uint64_t dropped_ = 0;   // the newest seq dropped for room (or cleared)
};

// A 0-ended name at `addr`, at most kMaxName bytes ("" when it cannot be read).
std::string ReadName(std::uint64_t addr, const ReadFn& read);
// The pane's layout name and its own name (either "" when it cannot be read).
void PaneNames(std::uint64_t pane, const ReadFn& read, std::string& layout, std::string& name);
// A text as a JSON string: valid UTF-8 as it is, any other byte and every control character as \u00XX, so a reply
// is always one valid line and the game's own markup bytes stay visible.
std::string JsonText(std::string_view bytes);
// Whether the text has bytes JsonText turns into \u00XX (control characters or bytes that are not UTF-8): the game's
// markup (colour, a pause, a choice), not yet read in a game.
bool HasMarkup(std::string_view bytes);
// The bridge's `text` members (after id, ok, frame, paused): "hooked" (patch 0041's wrap), "body" (patch 0042's),
// "last", "lost", "texts":[{seq, frame, layout, pane, text, length, cut, markup, wrap}].
std::string Members(bool hooked, bool body, std::uint64_t last, bool lost, const std::vector<Entry>& texts);

// The process's one log, filled by the wrap (coop_hookshot.cpp) while the bridge is on.
Log& TheLog();

}  // namespace CoopTalkText
