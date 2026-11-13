# Making a release of the NX2navi mod

The NX2navi co-op mod ([`nx2navi/README.md`](../nx2navi/README.md)) is handed out as one file,
`OoT Co-op.nx2mod`: a zip with the mod folder's files at its top, the format of NX2navi's launcher
(its `docs/mods-launcher.md`, "Single-file mods"). This page says how to make it, what is in it,
and how a player installs it. The emulator kit has its own release tooling (`tools/release/make_kit.ps1`).

## Make it on this PC

Needs Windows, Python 3.10 or newer, git, and a MinGW compiler: the one the NX2navi launcher installs
is found by `nx2navi/build.py` (`--compiler <path>` names another).

```
python tools/release/make_nx2navi_mod.py              # build, run the tests, pack
python tools/release/make_nx2navi_mod.py --no-tests   # without the unit tests
python tools/release/make_nx2navi_mod.py --version 0.3.0
```

It runs `nx2navi/build.py` (`--tests` unless `--no-tests`; `--compiler` and `--jobs` are passed on), then
writes `build/nx2navi-release/`:

| File | What |
|---|---|
| `OoT Co-op.nx2mod` | The mod. Deterministic: sorted names, fixed dates, so the same files give the same bytes. |
| `SHA256SUMS.txt` | `sha256sum -c` format, for the mod and the notes. |
| `notes.md` | The release notes: version from `nx2navi/mod/mod.json`, the commit, what works and what does not (a fixed text pointing at `nx2navi/README.md`). |

`--version X` does not change anything: the release stops when `mod.json` says another version. Change
the version in `nx2navi/mod/mod.json`, then make the release. The commit in the notes says
"with uncommitted changes" when the tree is not clean: make official releases from a clean commit.

### What it refuses

- **Files not on its list.** Only `mod.json`, the launcher add-on (`mod.json`'s `launcher`), `README.txt`,
  `LICENSE.txt`, `THIRD_PARTY_NOTICES.md`, `bin\*.dll` and `bin\*.exe`, and the `.py` files of
  `tools\puppet_pack` ship. Anything else in the mod folder stops the release.
- **Game data and local state, by name:** `romfs\`, `exefs\`, `coop.ini`, `coop-options.ini`, `settings.json`,
  `coop_puppet.json`, logs, `.pdb`, `.pyc` and `__pycache__`, hidden files, game file types. The puppet pack is
  made on the player's PC from their own game files; the mod holds none.
- **The repo's content guard:** the rules of `tools/check_content.py` (game file extensions, key names, content
  folders, release names in text) on the staged files. Its 2 MB limit is for sources and is not applied to `bin\`.
- **Build machine paths:** none of the build PC's user folder (`\Users\<name>`, either slash, one-byte text or
  UTF-16) nor this checkout's path may appear in any shipped file: the rule of `tools/release/check_paths.ps1`,
  with the trimmed paths of `cmake/trim_paths.cmake` (`build.py` passes `-ffile-prefix-map`).
- **A bad install:** when the NX2navi checkout is next to this repo (`..\NX2navi`, or `--nx2navi <folder>`), the
  finished file is installed with NX2navi's own `mods.install_archive` into a temporary folder and compared file
  by file. Skipped without a checkout.

`--mod-dir <folder> --no-build --out <folder>` packs another mod folder (a copy to try the refusals on, say).

## Make it on GitHub

`.github/workflows/nx2navi-mod.yml` runs only when asked: **Actions, NX2navi mod release, Run workflow**
(with "publish" off or on), or when a tag `nx2navi-v<version>` is pushed. There is no schedule and no run on
branch pushes.

1. The `build` job (Windows) installs MSYS2 UCRT64 with clang, builds with the tests, packs and uploads
   `build/nx2navi-release/*` as the artifact `nx2navi-mod`. For a tag it checks the tag against `mod.json`.
2. The `release` job (the only one with `contents: write`, running no repository code) creates the draft
   pre-release `nx2navi-v<version>` with the mod and `SHA256SUMS.txt`, only for a tag push or a manual run with
   "publish". It is **a draft and a pre-release**: nothing is public until someone opens it on GitHub and
   presses Publish.

Locally, `--publish` does the same with the GitHub CLI (`gh`, logged in): a clean checkout at a commit that is
on GitHub, then `gh release create nx2navi-v<version> ... --draft --prerelease`. It is never done by default.

## What a player does

1. Have NX2navi with its launcher and the mod loader.
2. In the launcher: **Mods, Install a Mod**, pick `OoT Co-op.nx2mod`. Or drop the file into `<game folder>\mods`;
   the launcher installs it the next time it reads the folder and moves the file to `mods\.installed`.
3. Enable the mod. The home screen gets a **Multiplayer** row with Host and Join.

Check the download against `SHA256SUMS.txt` (`certutil -hashfile "OoT Co-op.nx2mod" SHA256`).

**The game must be built with the plugin interface** for the in-game parts (panel, notices, chat box, the other
players as a second Link). An NX2navi build without it still joins a session and sends your presence; the
others' joins, chat and scenes go to the mod's log (`coop-nx2navi.log`). With "Show Other Players" on, the first
Host or Join makes the puppet pack on that PC from the game's own files, using `tools\puppet_pack` from the mod
(Python 3.14, or the `zstandard` package). Details and limits: [`nx2navi/README.md`](../nx2navi/README.md).

## Licence

The mod's programs (`coop_nx2navi.dll`, `oot-coop-server.exe`) are GPL-3.0 and contain ENet (MIT); `LICENSE.txt`
and `THIRD_PARTY_NOTICES.md` go in the file. The repository's notices are followed by the mod's own
(`nx2navi/THIRD_PARTY_NOTICES.md`): the one file copied from NX2navi, `dll/third_party/plugin_api.h`, is copied
unchanged under the permission in its header, and its notice is kept there. The source of a release is the commit
named in `notes.md` (GPL-3.0, section 6): publish a release only from a commit that is on GitHub.
