"""Prints the unnamed room-state bools of lichii/hook/coop_dungeon.cpp (kAutogenFlags).

Run by a developer against their own romfs dump; nothing from the game is in this tool:

    python -m puppet_pack.autogen_flags --romfs <romfs> [--all]

A room's logic graph can keep a saved bool that has no name in GameDataList: the node
`OperateGameDataAllocAutogenGameData` (inputs IsWrite and, for some, Bool) makes one for its
placement. The game names it

    AutogenGameDataLogicNode.<AiGroup Hash><module path><__GUIDHash as a signed int>

and stores Murmur3 x86 32 (seed 0) of that name in the save's Bool table, as any named flag.
<AiGroup Hash> is the top-level AiGroup's Hash (Banc/<scene>/*.bcett.byml), <module path> is ""
for the group's own graph and, for a nested module, its reference's Path without the last part,
with the "/" left out ("_85c6/_4c4c/AiGroup" -> "_85c6_4c4c"); __GUIDHash is the node's int
immediate. Read from the files and checked against a game three ways (fire-temple-a):
  - the Deku Tree torch plate's bool, seen set in a game (deku-test): 0x0f8f3fa3 =
    "AutogenGameDataLogicNode.1272800122779183173172078894";
  - the Fire Temple's big pillar: its event flow waits on the name itself
    (Event/EventFlow/FireTemple_Gimmick, FireTemple_LargePillarFallCamera,
    EventWaitUntilGameDataTrueWithTimeOut "AutogenGameDataLogicNode.15744991116824525035-1232849570");
  - every name this tool makes for the 13 dungeons, nested modules too, is in the Bool table of
    every save on the developer's PC (1196 saves), and no other spelling tried was.

Each bool is printed with what writes it (the node feeding IsWrite) and a kind:
  room    a room's gimmick state, written true once: a pillar or floor hammered down
          (ActorLogicHammerDrop.Buried), a hammer block stack's piece or the whole stack gone
          (Gimmick_Hammer_Destruction_*), an event asker's EventBool (a trap chest sprung, the
          Deku torch plate, a silver rupee group's reveal), a step switch's first press
  player  one player's own: the refill respawns (a pot, grass or a gossip stone's fairy that
          comes back when THAT player is out of bombs, arrows or sticks: Bool and IsWrite from an
          OperateANDPulse on the actor's Dead), and a record that the player walked into an area
Only "room" ones are shared (--all prints both).
"""
import argparse
import glob
import os
import struct
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    __package__ = "puppet_pack"  # noqa: A001

from . import byml, zs  # noqa: E402
from .eye_switches import _int, read_graph  # noqa: E402

NODE = "OperateGameDataAllocAutogenGameData"
# The scenes whose flags coop_dungeon.cpp shares (its kDungeonScenes).
DUNGEONS = ("BottomOfTheWell", "DodongoCavern", "FireTemple", "ForestTemple", "GanonCastle",
            "GerudoTrainingGround", "IceCavern", "InsideGanonCastle", "InsideJabuJabuBelly",
            "InsideTheDekuTree", "ShadowTemple", "SpiritTemple", "WaterTemple")
PREFIX = "AutogenGameDataLogicNode."


def murmur3(data, seed=0):
    """Murmur3 x86 32, the game's flag hash."""
    if isinstance(data, str):
        data = data.encode("utf-8")
    c1, c2 = 0xCC9E2D51, 0x1B873593
    h = seed
    n = len(data) // 4

    def rotl(x, r):
        return ((x << r) | (x >> (32 - r))) & 0xFFFFFFFF

    for i in range(n):
        k = int.from_bytes(data[4 * i:4 * i + 4], "little")
        k = rotl((k * c1) & 0xFFFFFFFF, 15)
        h ^= (k * c2) & 0xFFFFFFFF
        h = (rotl(h, 13) * 5 + 0xE6546B64) & 0xFFFFFFFF
    tail = data[4 * n:]
    k = 0
    for j in range(len(tail) - 1, -1, -1):
        k ^= tail[j] << (8 * j)
    if tail:
        k = rotl((k * c1) & 0xFFFFFFFF, 15)
        h ^= (k * c2) & 0xFFFFFFFF
    h ^= len(data)
    h ^= h >> 16
    h = (h * 0x85EBCA6B) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0xC2B2AE35) & 0xFFFFFFFF
    return h ^ (h >> 16)


def module_path(ref_path):
    """A nested module's part of the name: its reference's Path without the last part, no "/"."""
    return "".join(ref_path.split("/")[:-1])


def flag_name(group_hash, path, guid):
    """The bool's name; guid is the node's __GUIDHash as stored (an unsigned 32-bit word)."""
    signed = guid - (1 << 32) if guid >= 1 << 31 else guid
    return f"{PREFIX}{group_hash}{path}{signed}"


def int_immediates(data):
    """[{name: value}] per node: its int immediates (__GUIDHash among them)."""
    u32 = lambda o: struct.unpack_from("<I", data, o)[0]  # noqa: E731
    pool = u32(0x24)

    def text(o):
        return data[pool + o: data.index(b"\0", pool + o)].decode("utf-8", "replace")

    commands, count, imm = u32(0x0C), u32(0x10), u32(0x2C)
    table = u32(imm)
    out = []
    for i in range(count):
        body = u32(0x74 + commands * 0x18 + i * 0x3C + 0x14)
        first, n = u32(body), u32(body + 4)
        out.append({text(u32(table + 0x0C * k)): u32(table + 0x0C * k + 8) for k in range(first, first + n)})
    return out


def _source(nodes, node, plug_in):
    """'type[instance].plug' of the node feeding `plug_in`, or ''."""
    for name, src, out in node["in"]["bool"]:
        if name == plug_in and 0 <= src < len(nodes):
            s = nodes[src]
            outs = s["out"]["bool"]
            inst = s["imm"].get("InstanceName", "")
            return f"{s['type']}[{inst}].{outs[out] if 0 <= out < len(outs) else out}"
    return ""


def kind_of(nodes, node, graph_name=""):
    """("room" or "player", why)."""
    if graph_name.startswith("GossipStoneWithFairySpirit"):
        return "player", "a gossip stone's fairy (one player's song)"
    write = _source(nodes, node, "IsWrite")
    value = _source(nodes, node, "Bool")
    if value and write.startswith("OperateANDPulse"):
        return "player", "a refill respawn (written when that player's actor breaks)"
    if write.startswith("OperateANDFixInputTrue"):
        # Ganon's Castle room 24: set as the player walks into an area (HoldActorObserver on Player).
        src = [n for n in node["in"]["bool"] if n[0] == "IsWrite"][0][1]
        feed = _source(nodes, nodes[src], "BoolMulti")
        if "HoldActorObserverBySensorHitInfo" in feed:
            return "player", "one player walked into an area"
    return "room", write


def autogen_flags(romfs, dicts=None, scenes=DUNGEONS):
    """[(hash, scene, graph, writer, kind, name)] sorted by scene and hash, one per bool."""
    if dicts is None:
        dicts = zs.Dicts.from_romfs(romfs)
    graphs = {}

    def graph(logic):
        path = os.path.join(romfs, "Logic", os.path.basename(str(logic)) + "b")
        if path not in graphs:
            if os.path.exists(path):
                data = open(path, "rb").read()
                graphs[path] = (read_graph(data)[0], int_immediates(data))
            else:
                graphs[path] = None
        return graphs[path]

    found = {}
    for folder in sorted(glob.glob(os.path.join(romfs, "Banc", "*"))):
        scene = os.path.basename(folder)
        if scenes is not None and scene not in scenes:
            continue
        for path in sorted(glob.glob(os.path.join(folder, "*.bcett.byml.zs"))):
            with open(path, "rb") as f:
                root = byml.loads(dicts.decompress(f.read()))
            for g in root.get("AiGroups", []) or []:
                top = _int(g["Hash"])
                mods = [("", g.get("Logic", ""))]
                mods += [(module_path(r["Path"]), r["Logic"]) for r in g.get("References", []) or []
                         if "Logic" in r]
                for mpath, logic in mods:
                    gr = graph(logic)
                    if gr is None:
                        continue
                    nodes, ints = gr
                    for i, node in enumerate(nodes):
                        if node["type"] != NODE or "__GUIDHash" not in ints[i]:
                            continue
                        name = flag_name(top, mpath, ints[i]["__GUIDHash"])
                        h = murmur3(name)
                        graph_name = os.path.basename(str(logic)).split(".")[0]
                        kind, why = kind_of(nodes, node, graph_name)
                        found.setdefault(h, (h, scene, graph_name, why, kind, name))
    return sorted(found.values(), key=lambda r: (r[1], r[0]))


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--romfs", required=True)
    p.add_argument("--all", action="store_true", help="also the player-own ones (as comments)")
    a = p.parse_args(argv)
    for h, scene, graph_name, why, kind, name in autogen_flags(a.romfs):
        if kind != "room" and not a.all:
            continue
        lead = "    " if kind == "room" else "    // not shared: "
        print(f'{lead}{{{h:#010x}, "{scene}", "{graph_name}: {why}"}},')
    return 0


if __name__ == "__main__":
    sys.exit(main())
