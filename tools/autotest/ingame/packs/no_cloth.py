"""Pack files `no_cloth` (@pytest.mark.needs_pack('no_cloth'), run flag --pack no_cloth): row 313's MEASURED control,
the puppet pack as it was before cloth-puppet-313 (every puppet without its cloth), on the same module and lane.

The lane's pack is the tested sha's own, made by the kit's command (offline/lib/lane-pack.ps1: tools/release/kit/
scripts/kit-paths.ps1 Get-KitPackArgs = build.py --romfs <romfs> --mod-dir <pack> --title-rows --pause-tab). This
maker makes that pack twice from the tested sha's tools/puppet_pack/build.py over the lane's romfs: once with the
kit's flags (read from Get-KitPackArgs at make time) and once with the same flags plus --no-cloth (build.py:
"builds the pack as it was before", docs/puppet.md
"Cloth (row 313)"). It returns every file of the --no-cloth pack whose bytes differ from the kit-flag pack (the
puppet actor packs, the size table, coop_puppet.json with "cloth": false); testpack.py swaps them in for the boot and
puts the lane's own back after. The cloth's own extra files stay in the lane pack, named by nothing.

Interface (harness-c132): make(out, ctx) -> {path relative to the game's coop-puppet mod dir: built file};
ctx.tools (on sys.path), ctx.romfs, ctx.game_nup, ctx.lane, ctx.say(line).
"""
import filecmp
import os
import re
import subprocess
import sys

KIT_PATHS = os.path.join("release", "kit", "scripts", "kit-paths.ps1")   # under the tested sha's tools folder


def kit_flags(tools):
    """The pack flags of the kit's own command, read from kit-paths.ps1 Get-KitPackArgs (the ONE source: the lane's pack
    is made with it, lane-pack.ps1): its quoted '--' words other than --romfs / --mod-dir (their values are ours)."""
    with open(os.path.join(tools, KIT_PATHS), encoding="utf-8-sig") as f:
        text = f.read()
    m = re.search(r"function Get-KitPackArgs\b[^{]*\{(.*?)\n\}", text, re.S)
    if not m:
        raise RuntimeError("no_cloth: no Get-KitPackArgs in %s" % KIT_PATHS)
    flags = [w for w in re.findall(r"'(--[a-z0-9-]+)'", m.group(1)) if w not in ("--romfs", "--mod-dir")]
    if not flags:
        raise RuntimeError("no_cloth: Get-KitPackArgs has no pack flags (%s)" % m.group(1).strip()[:200])
    return flags


def files(root):
    out = {}
    for d, _, names in os.walk(root):
        for n in names:
            p = os.path.join(d, n)
            out[os.path.relpath(p, root).replace(os.sep, "/")] = p
    return out


def pack(ctx, mod, extra):
    build = os.path.join(ctx.tools, "puppet_pack", "build.py")
    argv = [sys.executable, "-I", "-B", build, "--romfs", ctx.romfs, "--mod-dir", mod] + kit_flags(ctx.tools) + extra
    r = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        raise RuntimeError("no_cloth: build.py %s exit %d: %s" % (" ".join(extra) or "(kit flags)", r.returncode,
                                                                  (r.stderr or r.stdout)[-800:]))
    return files(mod)


def make(out, ctx):
    kit = pack(ctx, os.path.join(out, "kit"), [])
    old = pack(ctx, os.path.join(out, "old"), ["--no-cloth"])
    made = {rel: p for rel, p in old.items() if rel not in kit or not filecmp.cmp(p, kit[rel], shallow=False)}
    if "coop_puppet.json" not in made:
        raise RuntimeError("no_cloth: the --no-cloth pack's coop_puppet.json is the same as the kit's (no cloth "
                           "in this sha's pack tool?)")
    ctx.say("no_cloth: %d of %d pack files differ without the cloth (the kit pack has %d only its own)" % (
        len(made), len(old), len(set(kit) - set(old))))
    return made
