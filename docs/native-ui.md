# Chat and name tags as the game's own UI (Hookshot)

Co-op's chat and name tags are drawn by the game itself, not over the picture. The module
(`hookshot_coop.dll`, `hookshot/src/coop_native_ui.*`) hands our text to the game's own screens on the game's
thread, at the frame hook. The game's font, frames and animations are used, and the game hides these screens
itself in cutscenes and menus.

Game: main of build `e0f3ccfaf50065ae` (docs/game-builds.md). Addresses are guest addresses, as Hookshot names
functions (`f_<address>`).

## What shows where

| Co-op | Game screen | Layout |
|---|---|---|
| A chat line | the in-game message window: the speaker's name over the line | `MessageInGame_00` (`T_Name_00`, `T_Message_00`) |
| A room notice ("Saria joined") | the banner | `SimpleInfo_00` (`T_Text_00`) |
| What you type | the in-game message window: "you", the text and a caret, changing as you type | `MessageInGame_00` |
| A name tag (on with `LICHII_COOP_NATIVE_TAGS=1` until seen in game) | an NPC balloon over the other player's Link; the game has 16 | `BalloonMessage_00` (`Pa_BalloonMessage_00`..`15`) |

## How

- A game text pane shows text from a message reference: `{const char* utf8, u32 bytes, i64 -1}`. The game keeps
  the pointer, so our texts live in one block of guest memory from the game's malloc (`TextRing`, 48 slots used in
  turn). The pane is set with the game's own setter `f_8045b970(layout, pane name, &reference, 1, 0)`; the pane
  names are strings the game already has.
- The game's message text is UTF-8 with tags (byte `0x0E`, then group, type, size, parameters). Text from other
  players is cleaned first: every byte below `0x20` (the tag byte too) and `DEL` is dropped, a bad UTF-8 sequence
  becomes `?`, and text is cut at a whole character.
- A chat line: the window is found with the game's getter `f_833d2690(*0x84ea4518)` and checked by its vtable
  (`0x84c09678`). The speaker goes into `T_Name_00`, the line's reference into the window (`+0x2b0`), its time in
  game frames into `+0x408` (the game's own default is 180), the show flag `+0x2d0` as the game sets it, then the
  window's open call (vtable slot 5). This is what the game's `f_833d25c0` does with a message of its own.
- A line never covers a message of the game's own: while the window shows a text that is not in our block and its
  time is not out, the line waits (and is dropped after 20 s).
- Lines show one at a time, 3 s plus 60 ms a character (at most 8 s). At most 8 wait; with many players the oldest
  go first.
- A notice: the banner screen is found in the UI manager's list (`**0x84c9e098`, count `+8`, screens `+0x10`) by its
  vtable (`0x84c0d7d8`); our text goes into `T_Text_00`, then its open call.
- Typing: while the chat box is open the typed line is on the message window; a change swaps only the text (no
  opening animation again), closing the box ends it at once, and lines that come meanwhile go to the banner.
- Tags: one balloon part per tag, from part 15 down (the game takes its own balloons from part 0), the nearest players
  first, at most 15. A part's name is in its `T_Text_00` (a fixed text slot each, set only when it changes); its root
  pane (`f_80229650(part + 0x40)`) is moved as the game's `f_8022a3e0` moves one (x, y at `+0x6c`, then the layout
  told) and shown with `f_8022aea0`. The point comes from the tag the overlay would draw (the game's camera, hs-tags),
  turned into the layout's points (1920x1080, centre 0, y up), which is also what the game's projection
  `f_80235ec0` gives its balloons. While the balloons carry the tags, the drawn head tags are off (edge arrows stay).
- Colours: players 1 to 16 each have their own (`CoopNativeUi::Colour`; 1 to 4 as the protocol's slot colours).

## Switches

- `LICHII_COOP_NATIVE_UI=0`: the old look (chat drawn over the picture) only.
- `LICHII_COOP_NATIVE_TAGS=1`: name tags on the game's balloons.
- `LICHII_COOP_NATIVE_PROBE=1`: every 3 s the balloon screen into the log (the game's balloon requests with their
  point, distance and text; the 16 parts' root panes; each tag's scene and layout point).
- Until the game's window has shown one line, the drawn chat lines stay as well (the title, a load, a game build
  without the window). While the chat box is open the drawn lines show too.

## Log lines

- `coop: native ui: chat through the game's message window (texts at 0x...)`
- `coop: native ui: first chat line shown by the game (message window|banner)`
- `coop: native ui: the game's message window is not free now; the line waits`
- `coop: native ui: typing in the game's message window`
- `coop: native ui: name tags on the game's balloons (N shown)`
- `coop: native ui: the game gave no memory for its texts; drawn chat only`

Check in game: `tools\autotest\run.ps1 -Runtime hookshot -Scenario hs-native-chat`.
