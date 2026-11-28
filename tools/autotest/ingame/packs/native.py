"""--pack native (@pytest.mark.needs_pack('native')): the native puppets' pack (docs/native-puppets.md, plan N1-N5),
made by the TESTED sha's tools/puppet_pack/build.py the way the lane's own pack is made (lanes.make_pack: the kit's
pack command, lanes.PACK_TOOL_ARGS = kit-paths.ps1 Get-KitPackArgs) plus the native switches, and only the files that
come out different from lane A's pack are put in (the lane's pack is 1.3 GB; testpack copies each file into each game).

Which switches: the environment's NATIVE_PUPPETS, a comma list of
    body -> --puppet-body      (N1: every puppet has a body; build.py main)
    lift -> --puppet-liftable  (N2: young puppets liftable as Ruto)
    pvp  -> --puppet-pvp       (N3-N5: the `_Pvp` copies)
default `body`. The made manifest (coop_puppet.json) must say each switch took ("body": true, "liftable": true, "pvp":
true: build.py's manifest), else the make fails (a SETUP-FAIL, never a run on the plain pack).

The build sees the lane's mods as the lane's own build does: build.py find_characters / charsel_native_wanted look for
the Character Select mod beside the pack and in the mods folder of a `contents/<title id>/<pack>` layout, and
mod_list_off reads mods/coop-mods.list. So the build goes into <out>/mods/contents/<title id>/coop-puppet with a
junction <out>/mods/<Character Select> -> lane A's own folder (read only; removed after the build, whatever happens)
and a copy of lane A's coop-mods.list. Without it the pack would have no character puppets and its tables would lack
the Character Select mod's rows (build.py merge_character_table), a different pack from the lane's.

The kit's start merges the pack's tables into the winning mod at every start (hookshot.py pack_start ->
merge_tables.py), after the pack is in, so new table files here are merged as the kit's own would be.

Both lane games get the same files (testpack.put_in), so lane A's and lane B's coop_puppet.json must be the same file
(lanes.make_pack builds both the same way); a lane whose two packs differ is a make failure. A file only the lane's
pack has under romfs/ (a pack made by another tool, left in) would stay in and is a make failure too, except the own
tunic files the kit's start puts into the pack's romfs (hookshot.py pack_start -> self_tunic.put: self_tunic.FIXED and
each colour's `self_files` path of either manifest), which the start takes away and puts again from the new
manifest (dry make on lane1-A 21:1x UTC: 9 such files, Model/Player_*.bfres.zs and Tex/*Angin*/Mt_*). The kit's own
files beside romfs/ (_coop-archives/, coop-merged.txt, ...) are not the build's and are left alone.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys

SWITCHES = {"body": ("--puppet-body", "body"), "lift": ("--puppet-liftable", "liftable"),
            "pvp": ("--puppet-pvp", "pvp")}          # NATIVE_PUPPETS word -> (build.py flag, manifest key it sets)
ENV = "NATIVE_PUPPETS"
DEFAULT = "body"
BUILD_TIMEOUT_S = 1800                               # ESTIMATED: a full pack build (lane-prep gives a prep >= 45 min)
MANIFEST = "coop_puppet.json"


def switches(value=None):
    """The NATIVE_PUPPETS words as a sorted list; an unknown word is an error (no silent plain pack)."""
    value = os.environ.get(ENV, DEFAULT) if value is None else value
    words = sorted({w.strip().lower() for w in value.split(",") if w.strip()})
    bad = [w for w in words if w not in SWITCHES]
    if bad or not words:
        raise RuntimeError("%s=%r: want a comma list of %s" % (ENV, value, ", ".join(sorted(SWITCHES))))
    return words


def file_sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def same_file(a, b):
    try:
        if os.path.getsize(a) != os.path.getsize(b):
            return False
    except OSError:
        return False
    return file_sha(a) == file_sha(b)


def files_under(root):
    """{rel ('/' separated): path} of every file under root."""
    out = {}
    for d, _, names in os.walk(root):
        for n in names:
            p = os.path.join(d, n)
            out[os.path.relpath(p, root).replace(os.sep, "/")] = p
    return out


def link_dir(link, target):
    """A directory junction (Windows: no admin needed) or a symlink elsewhere."""
    if os.name == "nt":
        r = subprocess.run(["cmd", "/c", "mklink", "/J", link, target], capture_output=True, text=True)
        if r.returncode or not os.path.isdir(link):
            raise RuntimeError("mklink /J %s failed: %s" % (link, (r.stdout + r.stderr).strip()))
    else:
        os.symlink(target, link, target_is_directory=True)


def unlink_dir(link):
    """Removes the junction/symlink only (never what it points to)."""
    if not os.path.lexists(link):
        return
    if os.name == "nt":
        os.rmdir(link)        # (a junction: rmdir removes the link, the target is untouched)
    else:
        os.unlink(link)


def read_json(path):
    with open(path, "rb") as f:
        return json.loads(f.read().decode("utf-8"))


def start_files(docs):
    """The romfs files the kit's start puts into the pack itself (self_tunic.put): its FIXED list and every colour's
    own `self_files` path (the same match self_tunic uses), of each manifest in docs; as 'romfs/<path>'."""
    from puppet_pack import self_tunic
    out = {"romfs/" + p for p in self_tunic.FIXED}
    for doc in docs:
        files = doc.get("self_files") if isinstance(doc.get("self_files"), dict) else {}
        for per in files.values():
            if isinstance(per, dict):
                out.update("romfs/" + p for p in per if isinstance(p, str) and self_tunic._OK.match(p))
    return out


def diff(built, lane_pack, keep=()):
    """({rel: built path} of files new or different from the lane's pack, [lane-only rels under romfs/ not in keep])."""
    mine, theirs = files_under(built), files_under(lane_pack)
    changed = {rel: p for rel, p in sorted(mine.items())
               if rel not in theirs or not same_file(p, theirs[rel])}
    stale = sorted(rel for rel in theirs if rel not in mine and rel.startswith("romfs/") and rel not in keep)
    return changed, stale


def make(out, ctx):
    from puppet_pack import build as pp_build
    import lanes
    import testpack

    words = switches()
    if not ctx.romfs or not os.path.isdir(ctx.romfs):
        raise RuntimeError("pack native needs lane A's romfs folder (got %r)" % ctx.romfs)
    game_a = os.path.dirname(os.path.abspath(ctx.romfs))
    lane_pack = os.path.join(game_a, testpack.PACK_REL)
    lane_json = os.path.join(lane_pack, MANIFEST)
    if not os.path.isfile(lane_json):
        raise RuntimeError("lane A has no co-op pack manifest %s" % lane_json)
    game_b = lanes.lane(ctx.lane)["b"] if ctx.lane else None
    if game_b and os.path.isdir(game_b):
        b_json = os.path.join(game_b, testpack.PACK_REL, MANIFEST)
        if not os.path.isfile(b_json) or not same_file(lane_json, b_json):
            raise RuntimeError("lane %s: A's and B's co-op packs differ (%s vs %s): one set of pack files cannot fit "
                               "both" % (ctx.lane, lane_json, b_json))

    mods = os.path.join(out, "mods")
    built = os.path.join(mods, "contents", pp_build.TITLE_ID, pp_build.DEFAULT_MOD_NAME)
    os.makedirs(built)
    cs_src = os.path.join(game_a, "mods", pp_build.CHARACTERS_MOD)
    cs_link = os.path.join(mods, pp_build.CHARACTERS_MOD)
    mods_list = os.path.join(game_a, "mods", "coop-mods.list")
    if os.path.isfile(mods_list):
        shutil.copyfile(mods_list, os.path.join(mods, "coop-mods.list"))
    flags = [SWITCHES[w][0] for w in words]
    cmd = [sys.executable, "-I", "-B", os.path.join(ctx.tools, "puppet_pack", "build.py"), "--romfs", ctx.romfs,
           "--mod-dir", built] + list(lanes.PACK_TOOL_ARGS) + flags
    try:
        if os.path.isdir(cs_src):
            link_dir(cs_link, cs_src)
        ctx.say("pack native: %s (%s)" % (" ".join(flags), "with %s" % pp_build.CHARACTERS_MOD
                                          if os.path.isdir(cs_src) else "no Character Select in lane A"))
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, errors="replace", timeout=BUILD_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            raise RuntimeError("the pack build ran over %d s" % BUILD_TIMEOUT_S)
        with open(os.path.join(out, "build.log"), "w", encoding="utf-8") as f:
            f.write((r.stdout or "") + (r.stderr or ""))
        if r.returncode:
            raise RuntimeError("pack build failed (exit %s): %s" % (
                r.returncode, ((r.stdout or "") + (r.stderr or "")).strip()[-400:]))
    finally:
        unlink_dir(cs_link)

    doc = read_json(os.path.join(built, MANIFEST))
    lane_doc = read_json(lane_json)
    for w in words:
        key = SWITCHES[w][1]
        if doc.get(key) is not True:
            raise RuntimeError("the made pack's manifest has %s=%r, not true (%s did not take)" % (
                key, doc.get(key), SWITCHES[w][0]))
    for key in ("characters", "title_rows", "pause_menu", "charsel_native"):
        if (doc.get(key) is None) != (lane_doc.get(key) is None) or bool(doc.get(key)) != bool(lane_doc.get(key)):
            raise RuntimeError("the made pack's %s (%s) is not the lane pack's (%s): not built as the lane's" % (
                key, json.dumps(doc.get(key))[:80], json.dumps(lane_doc.get(key))[:80]))
    bodies = sum(1 for a in doc.get("actors", []) if a.get("body"))
    changed, stale = diff(built, lane_pack, keep=start_files([doc, lane_doc]))
    if stale:
        raise RuntimeError("lane A's pack has %d romfs files this build does not make (%s): a pack of another tool; "
                           "prep the lane on this sha" % (len(stale), ", ".join(stale[:5])))
    if MANIFEST not in changed:
        raise RuntimeError("the made coop_puppet.json is the lane's own: the switches changed nothing")
    ctx.say("pack native: %s; %d of %d actors with body files; %d files new or changed vs lane A's pack" % (
        ",".join(words), bodies, len(doc.get("actors", [])), len(changed)))
    return changed
