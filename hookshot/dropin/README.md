<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# hookshot/dropin/ (v1 P0 proof, not shipped)

Co-op as a drop-in on a STOCK Longshot game (built by the player's own Longshot Builder), with no Longshot rebuild for
co-op. Design and the per-patch table: `coord-multiplayer/reports/v1-longshot-dropin.md`.

| File | What |
|---|---|
| `dropin.c` | Loaded as `winmm.dll` beside the game's `bin\longshot.exe` (a stock game folder's own `longshot.exe` is the builder's launcher copy). Reads the exe's own COFF symbol table from the file (`f_<guest address>` for every lifted function, plus the host's functions), puts a 5-byte jump at the lifted frame function's entry (`f_83b6e300`, main+0x3b4e300) and logs to `squadala-dropin.log`. `SQUADALA_DROPIN=0`: forwards winmm only. |
| `gen_winmm.py` | Writes the forwarders for every export of this PC's System32 `winmm.dll` (names only; each resolves on its first call). |
| `build.ps1` | Builds `out/winmm.dll` with the kit's llvm-mingw. |
| `selftest.c` | Offline check on a `longshot.exe` file: symbols found, prologues hookable. |
| `proof-run.ps1` | The run: stock v3.1.1 and v3.2.0 from the official builder zips, selftest, drop in, start, frame lines, screenshot, close. |
