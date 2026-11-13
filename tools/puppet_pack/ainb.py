"""The game's AI files (.ainb, `AIB `, version 0x408): just enough to make a one-shot xlink root.

Issue 166: a shot's spark and sound where it lands are played by the game's shootable code, which
the copies of another player's shots do not run. The game has an AI node that plays one xlink key
on an actor, `OneShotXLinkSearchAndEmit` (inputs `ELinkCall`, `SLinkCall`, `XLinkKeyName`). The
Business Scrub's seed has the smallest AI that uses it (`Shootable_ScrubBusinessSees_PostUpdateMatrix`,
1,806 bytes: a selector on the shootable's state, the emit node with the key
`ScrubBusinessSeed_HitGround`, and a do-nothing node). `oneshot_root` turns a copy of such a file
into a root that is the emit node alone, with our key and calls: an actor with that AI plays the
key once, when it is made. The game has roots that are one OneShot node (`Item_Dungeon_Map`,
`NavMeshDummyBox`, `BeltConveyor_Hole`).

Layout read from the files (not a full reader):
  header 0x74 bytes: +0x04 version, +0x08 file name (string offset), +0x0c commands, +0x10 nodes,
                     +0x24 the string pool's offset (the last section: strings are added at the end)
  commands from 0x74, 0x18 each: +0x00 name, +0x04 GUID, +0x14 the root node's index (s16)
  nodes after them, 0x3c each: +0x00 type, +0x02 index, +0x08 name
  immediate parameters, 0x10 each where the node's are: +0x00 name, +0x04 0x0000ffff,
                     +0x08 type bits, +0x0c the value (a bool's 0/1, a string's offset)
"""

import struct

MAGIC = b"AIB "
VERSION = 0x408
HEADER = 0x74
COMMAND = 0x18
NODE = 0x3C
EMIT = "OneShotXLinkSearchAndEmit"


class AinbError(ValueError):
    pass


class _File:
    def __init__(self, data):
        if data[:4] != MAGIC:
            raise AinbError("not an AI file")
        (self.version,) = struct.unpack_from("<I", data, 4)
        if self.version != VERSION:
            raise AinbError(f"AI file version {self.version:#x}, not {VERSION:#x}")
        self.data = bytearray(data)
        self.commands, self.nodes = struct.unpack_from("<II", data, 0x0C)
        (self.pool,) = struct.unpack_from("<I", data, 0x24)
        if not HEADER + COMMAND * self.commands + NODE * self.nodes <= self.pool <= len(data):
            raise AinbError("AI file sections out of place")

    def string(self, offset):
        a = self.pool + offset
        end = self.data.find(b"\0", a)
        if offset < 0 or a >= len(self.data) or end < 0:
            raise AinbError(f"no string at {offset:#x}")
        return self.data[a:end].decode("utf-8")

    def find_string(self, text):
        """The offset of `text` as a whole string of the pool, or None."""
        want = text.encode("utf-8") + b"\0"
        a = self.pool
        while a < len(self.data):
            end = self.data.find(b"\0", a)
            if end < 0:
                break
            if self.data[a:end + 1] == want:
                return a - self.pool
            a = end + 1
        return None

    def add_string(self, text):
        found = self.find_string(text)
        if found is not None:
            return found
        if not self.data.endswith(b"\0"):
            raise AinbError("the string pool does not end the file")
        offset = len(self.data) - self.pool
        self.data += text.encode("utf-8") + b"\0"
        return offset

    def node_names(self):
        base = HEADER + COMMAND * self.commands
        return [self.string(struct.unpack_from("<I", self.data, base + NODE * i + 8)[0])
                for i in range(self.nodes)]

    def param(self, name):
        """Where the one immediate parameter called `name` keeps its value."""
        offset = self.find_string(name)
        if offset is None:
            raise AinbError(f"no parameter {name}")
        start = HEADER + COMMAND * self.commands + NODE * self.nodes
        hits = [a for a in range(start, self.pool - 0x0F, 4)
                if struct.unpack_from("<II", self.data, a) == (offset, 0xFFFF)]
        if len(hits) != 1:
            raise AinbError(f"parameter {name} found {len(hits)} times")
        return hits[0] + 0x0C


def oneshot_root(template, name, key, elink=True, slink=True):
    """A copy of `template` (an AI file with one OneShotXLinkSearchAndEmit node) whose first
    command runs that node alone, with XLinkKeyName `key` and the ELink/SLink calls on or off.
    `name` becomes the file's own name (`<actor>.root`)."""
    f = _File(template)
    if f.commands < 1:
        raise AinbError("the AI file has no command")
    names = f.node_names()
    if names.count(EMIT) != 1:
        raise AinbError(f"the AI file has {names.count(EMIT)} {EMIT} nodes")
    struct.pack_into("<h", f.data, HEADER + 0x14, names.index(EMIT))
    places = {p: f.param(p) for p in ("ELinkCall", "SLinkCall", "XLinkKeyName")}
    struct.pack_into("<I", f.data, places["ELinkCall"], 1 if elink else 0)
    struct.pack_into("<I", f.data, places["SLinkCall"], 1 if slink else 0)
    struct.pack_into("<I", f.data, places["XLinkKeyName"], f.add_string(key))
    struct.pack_into("<I", f.data, 0x08, f.add_string(name))
    return bytes(f.data)


def describe(data):
    """(root node name, XLinkKeyName, ELinkCall, SLinkCall, file name) of a one-shot root."""
    f = _File(data)
    (root,) = struct.unpack_from("<h", f.data, HEADER + 0x14)

    def value(p):
        return struct.unpack_from("<I", f.data, f.param(p))[0]

    return (f.node_names()[root], f.string(value("XLinkKeyName")), bool(value("ELinkCall")),
            bool(value("SLinkCall")), f.string(struct.unpack_from("<I", f.data, 0x08)[0]))
