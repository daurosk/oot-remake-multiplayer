# Chat as the game's own UI

Co-op's chat is shown by the game itself, not drawn over the picture: no box, no panel, no drawn text. The module
(`hookshot/src/coop_native_ui.*`, glue in `coop_hookshot.cpp` `NativeStep`) hands our text to the game's own banner
on the game's thread at the frame hook, so the game's frame, font (in the game's language: Chinese, Japanese and
Korean games show those letters) and in and out animation are used.

Game: main of build `e0f3ccfaf50065ae` (1.1.1, [game-builds.md](game-builds.md)). Addresses are `main+` offsets in the
code; `f_<address>` below is the function at that address with main at `0x80020000`. Each was read again on the 1.1.1
main with `tools/game_scan` for this change (2026-11-23); the screens themselves are in [game-ui.md](game-ui.md).

## What shows where

Everything is on the game's banner, `SimpleInfo_00`: the frame `W_Bg_00` and the line `T_Text_00`.

| Co-op | On the banner |
|---|---|
| A chat line | "Saria: hi", the line in the speaker's colour; a long line in pages, each "Saria: " and what fits after it |
| A room notice ("Saria joined") | the notice, in the game's own colour |
| What you type (T) | "you: hello_" (the end of what you type, a caret, "(not sent)" in red after a send that failed), changing as you type |
| A held line (down but not out: "You are down  0:32", from `coop_dbno_draw`) | kept up while held, in the game's own colour; chat waits |

### Why not the "message window" `MessageInGame_00`

0.11.12's first version put chat lines and the typed line on `MessageInGame_00`. Proof-01112-c run 3 showed them as a
bare word at the bottom of the screen with no frame and no name. Read from the game: `MessageInGame_00` is the game's
voice **subtitle** line, not a message window ([game-ui.md](game-ui.md#the-message-window)):

- its layout has no frame pane at all (`N_Text_00`, `T_MessageWhiteOut_00`, and a name plate `N_Name_00` with
  `P_NameBase_00` and `T_Name_00`); `N_Name_00` is stored hidden;
- its step `f_83409420` plays its `Type` animation (on `N_Name_00`) to the end only while the player's option
  `VoiceSubtitleSpeakerName` ("Subtitle Speaker-Name Display", hash `0x4ac6a8ab`) is on, and to its first frame
  otherwise: no name with the option off;
- its open step `f_83409590` copies the option `VoiceSubtitle` ("Subtitle Display", `0x393669ef`) into `+0x1a8`, which
  the screens' shared code (`f_802f64c0`, `f_802f0944`) tests for an open screen: read as the line not shown with
  subtitles off (inferred, not run).

So chat there depended on two of the player's subtitle options and never had a frame. The banner has its frame in its
own layout (`W_Bg_00`, shown), no option, and the game shows its own short notices on it.

## How

- A text pane shows its text from a message reference `{const char* utf8, u32 bytes, i64 -1}` and keeps the pointer,
  so our texts live in one block of the game's memory from its own malloc (`TextRing`, 48 slots of 512 bytes used in
  turn, one fixed slot, and a scratch for call arguments). A pane is set with the game's setter `f_8045b970(layout,
  pane name, &reference, 1, 0)`; the pane name is a string the game already has (`T_Text_00` `0x83de2306`).
- Other players' text is cleaned first: the game's text has tags (byte `0x0E`, group, type, size, parameters), so every
  byte below `0x20` and `DEL` goes, a bad UTF-8 sequence becomes `?`, and text is cut at a whole character.
- **The banner** is found in the UI manager's list (`**0x84c9e098`, count `+8`, screens `+0x10`) by its vtable
  (`0x84c0d7c8`). A text goes up as the game's own `f_83419b10` puts one of an ordinary kind: its `Type` animation
  (`+0x2b0`, found by its set-up `f_80de2490`) to its first frame with `f_802f0fe0` (only kind `0x1d` plays it to the
  end), the text into `T_Text_00`, then its open call (slot 5, mode 2). A page that follows one of ours still up only
  changes the text.
- **It fits.** `T_Text_00` is 2100 x 86 at 0.8 in its `W_Bg_00` frame (1880 x 136): one line. A page is measured as
  the game draws it: each character by its advance in the game's own font (the HUD pack's `font-medium.otf`, Cetra
  Display Pro Medium, the Latin font of the game's Normal set; its `cmap` and `hmtx` read once, `SetFontWidths`), and
  never wider than 26 em, "speaker: " included (a name at most 9 em). The bar is the game's own: its widest banner text
  in `LayoutMsg/SimpleInfo_00`, in any language of the 1.1.1 romfs, is 27.3 em in that font (German, French, Italian,
  Spanish and Polish lines of 57 to 62 letters; English's widest is 22.0 em), and the game shows it on that pane on
  one line. The layout scales as one with the window, so the same 26 em fits at 16:9 and in the smallest window.
  Without the pack (or a font it cannot read) every character takes a guess that is never narrower than its advance
  in any of the game's Latin fonts (Cetra Display Pro Regular and Medium, Clarimo UD PE Regular and DemiBold: "W" 1.12,
  "m" 1.00, a capital 0.82, a small letter 0.66, "t" 0.50, "i" 0.40, a space 0.30, any other letter 1.12 em). A
  Chinese, Japanese or Korean character is 1 em. A long line is several pages shown in turn, never cut and never past
  the frame. (0.11.12 counted 48 columns, a guess: 48 "W" are 48.7 em, far past the game's widest line.) Not seen on
  screen yet: the LANE1 screenshot list in the worker's report checks English, a wide-letter line and Chinese.
- **The typed line.** It is "you: " then the end of what is typed that fits, a caret (the game font's own
  underscore) and, after a send that failed, "(not sent)" with the whole line in red (`kFailedColour`, through the
  text box's own colours as a speaker's colour is), so it is plain the line did not go; the text stays for another
  Enter, and any change to it clears the mark.
- **Timing.** A page shows 3 s plus 60 ms a column (at most 6 s); co-op closes its own page when its time is over
  (slot 6, only a banner of ours), so it never depends on the banner closing by itself. At most 16 pages wait; when
  more come the oldest line goes, all its pages. A line not begun 20 s after it came is dropped, all of it; a line
  already begun shows to its end.
- **Never over the game's own banner** (`CoopNativeUi::Banner`, every write of ours to it): the banner is free when it is
  shut with no open asked (state `+0x1a6` 0, request `+0x1a5` below 1). When it is up or opening, the text in its
  `T_Text_00` text box (the pointer the text box keeps at `+0xf0`, `f_80459680`) is ours only when it is in our block or
  is the pointer the box held right after our own last set (a game that keeps a copy); anything else, or a box that
  cannot be read, is the game's: our page waits (tried again each second), the held line waits, and nothing of ours
  closes it.
- **Colours.** A chat line is in the speaker's colour (the HUD's and tags' `CoopPuppet::NameColor`): the text pane's
  own two colours (`f_8152c740`, `f_8152c810`; the colour tag has only 6). The pane comes from `f_8078e2b0(layout,
  "T_Text_00")` and is used only when it is a text pane (kind 2) whose text box holds colours; the game's own colours
  are read first and put back for a notice, the held line, and as soon as the banner is not ours.
- **A held line** (`CoopNativeUi::HoldBanner(text)` / `EndBanner()`, from any thread): the banner is opened when it is
  free, its text set only when it changes (two fixed slots after the ring, used in turn: the new text goes into the one
  the text box does not show, so the game never reads a text we are rewriting, and a clock that changes each second
  never walks round the ring; banner-steady-0121), and it is closed (slot 6) when let go, only while the banner is ours. Chat and the typed line
  wait meanwhile.
- **Typing**: while the chat box is open the typed line is on the banner, set again on each change (no opening
  again); chat that comes meanwhile waits; closing the box closes the line at once.
- **Never opened again while it shows** (typing-blink-0121): the game's open (slot 5) with mode 2 plays the banner's
  `open` animation from its start whatever its state (its update `f_802f1380` calls slot 70 `f_80795d20` ->
  `f_802f62e0`: `open`, state back to 1; mode 3 is `open_Direct`). So co-op calls open only on a banner that is shut,
  closing (state 3) or has our close pending (request below 0); on one that is up (state 1 or 2) or asked open
  already, a page, the typed line or the held line only sets its text (`Banner::Raise`). The bridge state's `banner`
  ([playtest.md](playtest.md)) counts the opens, the asks kept, and a banner of ours the game shut or took.

## Switches and fallbacks

- `LICHII_COOP_NATIVE_UI=0`, or a game that gives no memory for the texts: the chat is drawn as before, as text only
  (no box behind the lines or the chat box, no red frame: a send that failed shows in red).
- On the title and during loads the game may have no banner: lines wait (up to 20 s) for it.

## Log lines

- `coop: native ui: chat on the game's banner (SimpleInfo_00; texts at 0x...)`
- `coop: native ui: first chat line|notice shown on the game's banner`
- `coop: native ui: the game's banner is not free now; the line waits`
- `coop: native ui: typing on the game's banner`
- `coop: native ui: banner: ours -> shut (state 0, request 0, page)` and the like: each change of whose the banner is,
  `... open asked while up (...): kept up, not opened again` (the first 300 a run, then every 100th)
- `coop: native ui: banner lines measured with the game's font (N bytes)` (or `... measured by safe guesses`)
- `coop: native ui: the game gave no memory for its texts; chat drawn as before`

## Not proven yet (one LANE1 screenshot run)

Every step above in a game: the banner's frame around our text, the line colour, the close of our own page, the text
box pointer at `+0xf0` as the banner's owner test, that a 26 em page fits at 16:9 and in the smallest window, in
English and in Chinese, and the red of a failed send.
