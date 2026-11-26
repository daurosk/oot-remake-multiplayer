"""A kit pair for the autotest's kit mode (run.ps1 -Kit, hookshot.py run --kit-a/--kit-b): a release kit unzipped
twice, each copy starting test lane 1's game (the same exe paths as every lane run: no firewall prompt).

    python -I tools/autotest/kit_pair.py setup   --zip <kit .zip> [--lane 1] [--game <.nup>] [--dry-run]
    python -I tools/autotest/kit_pair.py restore [--lane 1] [--dry-run]
    python -I tools/autotest/kit_pair.py status  [--lane 1]

`setup` first backs up what a kit's start writes into the lane's games (the co-op module, coop.ini, coop-options.ini,
mods\\ with the puppet pack and the randomizer seeds, tools\\ with the randomizer) into <kit root>\\ls\\laneN-kit-backup,
then unzips the kit into <kit root>\\ls\\kit-A and kit-B and points each one at its game: hookshot\\game-folder.txt =
laneN-A / laneN-B, hookshot\\game-files.txt = the game file (the puppet pack is made from it). A kit whose play.ps1
predates the autotest's hook (OOT_COOP_TEST_KIT, 0.11.1) gets that hook put into its own play.ps1, and every kit whose
test-mode start still activates the game window (no Start-KitTestGame) gets this repo's no-activation start put in
(the function and the title-mode start's if/else), so a test game opening never takes the focus. Nothing else.
`restore` puts the backup back and removes it (the kits stay, for the next setup to replace). Never while a game runs
in the lane. The lane's lock is run.ps1's, as for every lane run; this only prepares and cleans up.
"""
import argparse
import glob
import json
import os
import re
import shutil
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lanes  # noqa: E402

# What the kit's start (hookshot-build.ps1 step 4, play.ps1 title) writes into a game folder, beside longshot.exe.
KEEP = ("hookshot_coop.dll", "coop.ini", "coop-options.ini", "mods", "tools")
MANIFEST = "backup.json"

# The autotest's hook in a kit's play.ps1 (as tools/release/kit/scripts/play.ps1 has it since 0.11.1).
HOOK_MARK = "OOT_COOP_TEST_KIT"
HOOK_EDITS = (
    ("    foreach ($v in 'LICHII_COOP_SERVER', 'LICHII_COOP_PASSWORD', 'OOT_COOP_SESSION_PASSWORD', "
     "'OOT_COOP_JOIN_PASSWORD', 'LICHII_COOP_HOST_SAVE') {\n",
     "    $testKit = \"$env:OOT_COOP_TEST_KIT\"\n"
     "    foreach ($v in 'LICHII_COOP_SERVER', 'LICHII_COOP_PASSWORD', 'OOT_COOP_SESSION_PASSWORD', "
     "'OOT_COOP_JOIN_PASSWORD', 'LICHII_COOP_HOST_SAVE') {\n"
     "        if ($testKit -and $v -in 'LICHII_COOP_SERVER', 'LICHII_COOP_PASSWORD') { continue }\n"),
    ("    [void](Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) -PassThru)\n",
     "    $game = Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) -PassThru\n"
     "    if ($testKit -and $game) { [System.IO.File]::WriteAllText($testKit, \"$($game.Id)`n\") }\n"),
)


# This repo's play.ps1: the one source of Start-KitTestGame (the CreateProcess start without window activation).
PLAY_SRC = os.path.join(HERE, "..", "release", "kit", "scripts", "play.ps1")
START_FN = re.compile(r"# The autotest's kit mode only.*?\n    return \[KitNoActivate\]::Start\(\$Exe, \$Dir\)\n\}\n", re.S)
# The title-mode start of a kit with the hook: Start-Process, then (maybe a problem-send line and) the hook's pid line.
START_LINE = re.compile(
    r"(?m)^    \$game = Start-Process -FilePath \$exe -WorkingDirectory \(Split-Path -Parent \$exe\) -PassThru\n"
    r"(?=(?:    if \(\$game\)[^\n]*\n)?    if \(\$testKit -and \$game\))")
NO_ACTIVATE_MARK = "Start-KitTestGame"


class Fail(Exception):
    pass


def paths(n, kit=None):
    L = lanes.lane(n, kit)
    root = os.path.dirname(L["a"])
    return {"A": L["a"], "B": L["b"], "kitA": os.path.join(root, "kit-A"), "kitB": os.path.join(root, "kit-B"),
            "backup": os.path.join(root, "lane%d-kit-backup" % n)}


def exe_dir(game):
    return os.path.join(game, "bin") if os.path.isfile(os.path.join(game, "bin", "longshot.exe")) else game


def add_hook(text):
    """play.ps1's text with the autotest's hook in (unchanged when it has it); Fail when its lines are not there."""
    if HOOK_MARK in text:
        return text
    crlf = "\r\n" in text
    t = text.replace("\r\n", "\n")
    for old, new in HOOK_EDITS:
        if t.count(old) != 1:
            raise Fail("this kit's play.ps1 has no line %r: the autotest's hook cannot go in" % old.strip()[:60])
        t = t.replace(old, new)
    return t.replace("\n", "\r\n") if crlf else t


def start_function():
    """Start-KitTestGame's text (with its comment) from this repo's play.ps1, LF line ends."""
    with open(PLAY_SRC, encoding="utf-8-sig", newline="") as f:
        text = f.read().replace("\r\n", "\n")
    m = START_FN.search(text)
    if not m:
        raise Fail("%s has no Start-KitTestGame" % PLAY_SRC)
    return m.group(0)


def add_no_activate(text):
    """play.ps1's text (with the autotest's hook, see add_hook) whose test-mode start shows the game window without
    activating it: this repo's Start-KitTestGame put in, and the title-mode start as `if ($testKit) { the no-activation
    start } else { Start-Process }` (unchanged when it has it); Fail when the start's lines are not there."""
    if NO_ACTIVATE_MARK in text:
        return text
    crlf = "\r\n" in text
    t = text.replace("\r\n", "\n")
    if len(START_LINE.findall(t)) != 1:
        raise Fail("this kit's play.ps1 has no title-mode start line the no-activation start can replace")
    new = (start_function() +
           "    if ($testKit) {   # (the autotest's kit mode: no window activation, Start-KitTestGame above)\n"
           "        $game = Get-Process -Id (Start-KitTestGame $exe (Split-Path -Parent $exe))\n"
           "    } else {\n"
           "        $game = Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) -PassThru\n"
           "    }\n")
    t = START_LINE.sub(lambda m: new, t, count=1)
    return t.replace("\n", "\r\n") if crlf else t


def find_game_file(kit_root):
    files = sorted(glob.glob(os.path.join(os.path.dirname(kit_root), "*.nup")))
    return files[0] if files else ""


def remove(p, dry, say):
    if os.path.isdir(p):
        say("  remove %s" % p)
        if not dry:
            shutil.rmtree(p)
    elif os.path.exists(p):
        say("  remove %s" % p)
        if not dry:
            os.remove(p)


def copy(src, dst, dry, say):
    say("  %s -> %s" % (src, dst))
    if dry:
        return
    if os.path.isdir(src):
        shutil.copytree(src, dst)
    else:
        shutil.copy2(src, dst)


def unzip_kit(zip_path, dest, dry, say):
    """The kit's one top folder (oot-coop-<version>\\) unpacked as dest."""
    with zipfile.ZipFile(zip_path) as z:
        names = z.namelist()
        tops = {n.split("/", 1)[0] for n in names if n.strip("/")}
        if len(tops) != 1:
            raise Fail("%s is not a kit zip (one top folder expected, found %d)" % (zip_path, len(tops)))
        top = tops.pop() + "/"
        say("  unzip %s -> %s" % (os.path.basename(zip_path), dest))
        if dry:
            return
        remove(dest, False, lambda s: None)
        for info in z.infolist():
            rel = info.filename[len(top):]
            if not rel or info.filename.endswith("/"):
                continue
            target = os.path.normpath(os.path.join(dest, rel))
            if not target.startswith(os.path.normpath(dest) + os.sep):
                raise Fail("%s: %s is outside the kit folder" % (zip_path, info.filename))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with z.open(info) as s, open(target, "wb") as d:
                shutil.copyfileobj(s, d)


def write_text(path, text, dry, say):
    say("  %s = %s" % (path, text.strip()))
    if not dry:
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)


def setup(n, zip_path, game_file="", dry=False, say=print, kit=None, busy=lanes.running_in):
    p = paths(n, kit)
    if os.path.exists(p["backup"]):
        raise Fail("%s is there already: an earlier setup was not restored (kit_pair.py restore first)" % p["backup"])
    for x in "AB":
        if busy(p[x]):
            raise Fail("a game runs in %s" % p[x])
    if not os.path.isfile(zip_path):
        raise Fail("no kit zip %s" % zip_path)
    game_file = game_file or find_game_file(lanes.kit_root() if kit is None else kit)
    if not game_file or not os.path.isfile(game_file):
        raise Fail("no game file (.nup); pass --game")
    say("lane %d: back up what a kit's start writes into its games" % n)
    manifest = {}
    for x in "AB":
        src, dst = exe_dir(p[x]), os.path.join(p["backup"], x)
        if not dry:
            os.makedirs(dst, exist_ok=True)
        manifest[x] = {"dir": src, "had": []}
        for name in KEEP:
            if os.path.exists(os.path.join(src, name)):
                copy(os.path.join(src, name), os.path.join(dst, name), dry, say)
                manifest[x]["had"].append(name)
    if not dry:
        with open(os.path.join(p["backup"], MANIFEST), "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=1)
    for x in "AB":
        k = p["kit" + x]
        unzip_kit(zip_path, k, dry, say)
        write_text(os.path.join(k, "hookshot", "game-folder.txt"), p[x] + "\n", dry, say)
        write_text(os.path.join(k, "hookshot", "game-files.txt"), os.path.abspath(game_file) + "\n", dry, say)
        play = os.path.join(k, "scripts", "play.ps1")
        if not dry:
            with open(play, encoding="utf-8-sig", newline="") as f:
                text = f.read()
            hooked = add_hook(text)
            if hooked != text:
                say("  %s: the autotest's hook put in (a kit before 0.11.1)" % play)
            quiet = add_no_activate(hooked)
            if quiet != hooked:
                say("  %s: the test-mode start shows the window without activating it (Start-KitTestGame put in)" % play)
            if quiet != text:
                with open(play, "w", encoding="utf-8", newline="") as f:
                    f.write(quiet)
    say("ready: run.ps1 -Lane %d -Kit \"%s\",\"%s\" -Scenario <name>; then kit_pair.py restore --lane %d"
        % (n, p["kitA"], p["kitB"], n))
    return p


def restore(n, dry=False, say=print, kit=None, busy=lanes.running_in):
    p = paths(n, kit)
    mf = os.path.join(p["backup"], MANIFEST)
    if not os.path.isfile(mf):
        raise Fail("no backup in %s (nothing to restore)" % p["backup"])
    for x in "AB":
        if busy(p[x]):
            raise Fail("a game runs in %s" % p[x])
    with open(mf, encoding="utf-8") as f:
        manifest = json.load(f)
    say("lane %d: put back what the kits' start changed" % n)
    for x in "AB":
        d = manifest[x]["dir"]
        for name in KEEP:
            remove(os.path.join(d, name), dry, say)
            if name in manifest[x]["had"]:
                copy(os.path.join(p["backup"], x, name), os.path.join(d, name), dry, say)
    remove(p["backup"], dry, say)
    return p


def status(n, say=print, kit=None):
    p = paths(n, kit)
    say("lane %d backup: %s" % (n, "there (restore it after the run)" if os.path.exists(p["backup"]) else "none"))
    for x in "AB":
        k = p["kit" + x]
        g = os.path.join(k, "hookshot", "game-folder.txt")
        say("kit-%s: %s" % (x, open(g, encoding="utf-8").read().strip() if os.path.isfile(g) else "not set up"))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="what", required=True)
    s = sub.add_parser("setup")
    s.add_argument("--zip", required=True)
    s.add_argument("--game", default="")
    for q in (s, sub.add_parser("restore"), sub.add_parser("status")):
        q.add_argument("--lane", type=int, choices=[1, 2], default=1)
        if q.prog.split()[-1] != "status":
            q.add_argument("--dry-run", action="store_true")
    a = ap.parse_args(argv)
    try:
        if a.what == "setup":
            setup(a.lane, os.path.abspath(a.zip), a.game, a.dry_run)
        elif a.what == "restore":
            restore(a.lane, a.dry_run)
        else:
            status(a.lane)
    except Fail as e:
        print("kit_pair: %s" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
