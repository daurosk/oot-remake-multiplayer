"""`look`: one short text for the whole situation, built from one bridge state (docs/playtest.md, "look"). Standard
library only; no game, no picture.

    look(state, previous)  ->  (text, summary)     `summary` is what the next look compares against
"""
import math

NEAR_M = 25.0          # actors farther than this are left out
PER_KIND = {"enemy": 6, "door": 4, "chest": 3, "switch": 3, "item": 3, "npc": 3}
AMMO_WORDS = ("seed", "arrow", "bomb", "nut", "stick", "bottle")
KIND_ORDER = ("enemy", "door", "chest", "switch", "item", "npc")


def bearing(link, pos):
    """World yaw in degrees (0 = +z, 90 = +x) from Link to a point, 0 to 359."""
    return round(math.degrees(math.atan2(pos[0] - link[0], pos[2] - link[2])) % 360)


def side(facing, yaw):
    """ahead, right, behind or left of Link's facing, for a world yaw."""
    d = (yaw - facing + 180) % 360 - 180
    return "ahead" if abs(d) <= 45 else "behind" if abs(d) >= 135 else "right" if d > 0 else "left"


def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _pos(v):
    return isinstance(v, list) and len(v) == 3 and all(_num(x) for x in v)


def _actor_line(a, link, facing):
    where = ""
    if _pos(a.get("pos")) and link is not None:
        yaw = bearing(link, a["pos"])
        where = " %.0fm yaw %d%s" % (a.get("dist") or 0, yaw, " " + side(facing, yaw) if _num(facing) else "")
    life = ""
    if a.get("kind") == "enemy" and _num(a.get("life")):
        life = " life %s/%s" % (a["life"], a.get("max_life") if a.get("max_life") is not None else "?")
    return "%s #%s%s%s" % (a.get("name"), a.get("id"), life, where)


def _ammo(inv):
    return {k: v for k, v in sorted((inv or {}).items())
            if _num(v) and v > 0 and any(w in k.lower() for w in AMMO_WORDS)}


def look(state, previous=None):
    """The text of a look and the summary to give the next look. `state` is a bridge state reply (with its `world`)."""
    lines = []
    s = state
    pos, facing = s.get("pos"), s.get("facing")
    link = pos if _pos(pos) else None
    if s.get("snapshot_stale") or link is None:
        lines.append("scene %s: Link's data is stale or missing (%s). Step a frame and look again."
                     % (s.get("scene"), s.get("not_ready") or "no sample"))
    head = "scene %s room %s" % (s.get("scene"), s.get("room") if s.get("room") is not None else "?")
    if s.get("loading"):
        head += " | LOADING"
    lines.append(head)
    eq = s.get("equips") or {}
    life = "%s/%s" % (s.get("life"), s.get("max_life")) if _num(s.get("life")) else "?"
    lines.append("Link %s, life %s quarter hearts, %s rupees, sword %s shield %s" % (
        _age(s) or "?", life, s.get("rupees", "?"), eq.get("weapon") or "none", eq.get("shield") or "none"))
    if link:
        lines.append("at %.1f %.1f %.1f facing %s" % (link[0], link[1], link[2],
                                                      "%d" % round(facing) if _num(facing) else "?"))
    ammo = _ammo(s.get("inventory"))
    if ammo:
        lines.append("have: " + ", ".join("%s %s" % kv for kv in ammo.items()))
    if s.get("event"):
        lines.append("EVENT running (cutscene or text box). The text itself is not readable through the kit yet; "
                     "step_until event == false, or press A.")
    a_now = s.get("prompt")
    lines.append("A: %s | B: %s | C: not readable" % (a_now if a_now else "nothing to do here",
                                                      "sword" if eq.get("weapon") else "nothing"))
    world = s.get("world") if isinstance(s.get("world"), dict) else None
    seen = {}
    if world is None or (world.get("why") and not world.get("actors")):
        lines.append("actors: none listed (%s)" % ((world or {}).get("why") or "no world list"))
    else:
        actors = [a for a in world.get("actors", []) if a.get("kind") in PER_KIND
                  and (a.get("dist") is None or a["dist"] <= NEAR_M)]
        actors.sort(key=lambda a: (a.get("dist") if _num(a.get("dist")) else 1e9, a.get("id", 0)))
        for kind in KIND_ORDER:
            group = [a for a in actors if a["kind"] == kind]
            live = [a for a in group if a.get("alive") is not False]
            for a in group:
                seen[a.get("id")] = (a.get("name"), kind, a.get("alive") is not False)
            if kind == "enemy" and len(group) > len(live):
                lines.append("enemies dead: %d" % (len(group) - len(live)))
            for a in live[:PER_KIND[kind]]:
                lines.append("%s: %s" % (kind, _actor_line(a, link, facing)))
            if len(live) > PER_KIND[kind]:
                lines.append("%s: %d more" % (kind, len(live) - PER_KIND[kind]))
        if not seen:
            lines.append("actors: nothing of note within %d m" % NEAR_M)
    summary = {"scene": s.get("scene"), "life": s.get("life"), "age": _age(s), "pos": link,
               "event": bool(s.get("event")), "seen": seen, "prompt": a_now}
    lines.extend(changed(previous, summary))
    return "\n".join(lines), summary


def _age(s):
    """Our Link's age: the module's `body` (the name of our Link actor, right at once after an area change) when it
    is known, else Longshot's `age`."""
    return s.get("body") or s.get("age")


def changed(old, new):
    """The `changed:` lines between two summaries (none for the first look)."""
    if not old:
        return ["changed: first look"]
    out = []
    if old["scene"] != new["scene"]:
        out.append("scene %s -> %s" % (old["scene"], new["scene"]))
    if old["life"] != new["life"] and _num(old["life"]) and _num(new["life"]):
        out.append("life %s -> %s" % (old["life"], new["life"]))
    if old["age"] != new["age"]:
        out.append("age %s -> %s" % (old["age"], new["age"]))
    if old["pos"] and new["pos"] and old["scene"] == new["scene"]:
        m = math.hypot(new["pos"][0] - old["pos"][0], new["pos"][2] - old["pos"][2])
        if m >= 0.5:
            out.append("moved %.1f m" % m)
    if old["event"] != new["event"]:
        out.append("event %s" % ("started" if new["event"] else "over"))
    if old["prompt"] != new["prompt"]:
        out.append("A: %s -> %s" % (old["prompt"] or "nothing", new["prompt"] or "nothing"))
    if old["scene"] == new["scene"]:
        for ident, (name, kind, alive) in new["seen"].items():
            before = old["seen"].get(ident)
            if before is None:
                out.append("new %s %s #%s" % (kind, name, ident))
            elif before[2] and not alive:
                out.append("%s %s #%s is dead" % (kind, name, ident))
        for ident, (name, kind, alive) in old["seen"].items():
            if ident not in new["seen"] and alive:
                out.append("gone: %s %s #%s" % (kind, name, ident))
    return ["changed: " + "; ".join(out)] if out else ["changed: nothing"]
