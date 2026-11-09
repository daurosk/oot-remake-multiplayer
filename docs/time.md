# Time of day, the same in every game

Every game shows the host's time of day: day or night, the clock, and a jump when a player plays the Sun's Song or the clock moves for an event. So night enemies, lighting and townspeople's schedules match. Code: `lichii/hook/coop_time.{h,cpp}` (the game side) and `coop_time_sync.{h,cpp}` (the rules, tested in `tests/lichii/time_test.cpp`). Message: `TIME_OF_DAY` ([protocol.md](protocol.md)).

Game: main of build e0f3ccfaf50065ae (1.1.1). **Checked** means read from the instructions; **live** means seen in the running game.

## Where the clock is

| What | Where | Status |
|---|---|---|
| The world manager | `[main + 0x4e7eab0]`, the object behind GOT slot 0x4c7e898 that the AI node `ExecuteWmSetTime` (run 0x2890e80) loads | Checked |
| The time object `T` | `[WorldMgr + 0x510]` | Checked: the getter and the setter both start there |
| The current time | A record at `T + 0x20`, or `T + 0x50` while bit 9 (0x200) of the u32 at `T + 8` is set (the getter 0x3452280 picks it and copies its +8 and +0x10) | Checked |
| Seconds since midnight | u32 at record + 8 | **Live**: 10:01 in the Graveyard save read 0x8ceb (36,075); it ran about 120 in-game seconds a real second |
| Days | u32 at record + 0xc | Live: went from 0 to 1 when a set took the clock back to 00:00 |
| The setter | `0x34522d0(WorldMgr, seconds)`. `ExecuteWmSetTime` calls it with `hour * 3600 + minute * 60`; it loads `T` and calls `0xeee640(T, seconds, 1, -1)`, which stores the asked time at `T + 0x48` and sets request bits in `T + 8` | Checked; **live**: called from the svc cave, it set game A's clock to 00:00 (flags then read 0x5c while the change ran) |

`OneShotGameDataSetDayTimeStruct` and `OneShotGameDataSetCurrentTimeStruct` copy the time into GameData structs for the game's own use; no named flag holds the clock (every Bool, Int, Float, UInt and Enum name in the logic graphs, event flows and `main` with Time, Hour, Day, Night or Sun in it was checked against `GameDataList`).

## The rules

- The host sends `TIME_OF_DAY` with its clock about once a second.
- Another game whose clock is more than 120 in-game seconds (about one real second) away from the host's, either way round midnight, sets its clock to the host's, at most once every 2 s.
- A game whose clock moved more than half an hour between two reads (a quarter of a second apart) by itself (the Sun's Song, sleeping, an event) sends `TIME_OF_DAY` with the jump bit; every other game, the host's too, sets its clock to it at once. The jump our own set causes is not sent (3 s quiet after a set).
- No clock (no game, a load) sends nothing, and the first read after it is not a jump.
- The set is the game's own setter, called on the calc thread from the svc cave (`CoopSvc::AddCaller`, order 40), after checking that the world manager and its time object exist.

Switches: `LICHII_COOP_TIME=0` turns it off; `LICHII_COOP_DEBUG_TIME=1` logs the raw time record every 2 s and every message sent; `LICHII_COOP_TIME_TEST_SET=<seconds>[@<delay>]` (tests only) makes the host set its clock <delay> seconds (default 5) after joining, as the Sun's Song would (another game ignores it, so the same switch list serves both games of a test). The cave is patched only with `LICHII_COOP_PUPPET` or `LICHII_COOP_SPAWN` on; without it the clock is read and sent but never set.

## Not done

1. **The two-game check is not finished.** The first live run read both clocks and set game A's to 00:00, but its reader still took the wrong word, so game B was never moved; the reader is fixed (record + 8) and built, not run in two games. To check: `tools/autotest/run.ps1 -Scenario time -NoLoad` with `LICHII_COOP_DEBUG_TIME=1` and `LICHII_COOP_TIME_TEST_SET=0@120` in `-Switches` (both games warp to Kakariko; the host sets midnight two minutes after joining; the other game must log `time: set to 00:00:00` and show the same night sky). By hand: stand outdoors in both games and compare the sky, then play the Sun's Song in one.
2. After the set, A's clock read 00:00 and did not move for the rest of the run (about 10 s), with flags 0x5c. Whether the clock runs again after a set's transition is not known.
3. Whether the Sun's Song moves the clock through the same record (so it is seen as a jump) is not checked; it should, since it changes the time the getter returns.
4. Indoor scenes and dungeons may stop the clock (the record kept the same value for several seconds in game B in the Graveyard): a stopped clock is not a jump, and the host's clock is still taken when it moves.
