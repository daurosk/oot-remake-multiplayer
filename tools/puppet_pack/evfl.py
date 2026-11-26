"""Reads the game's event flow files (Event/EventFlow/*.bfevfl, EVFL version 4) far enough to say
which actions an entry point can run.

Layout (offsets are from the file's start; strings are a u16 length, the text and a 0):
  file      0x20 u16 flowcharts (0: a timeline only); 0x28 u64 -> u64 -> the flowchart
  flowchart "EVFL"; 0x10 u16 actors, actions, queries, events, entry points; 0x20 name;
            0x28 actors; 0x30 events; 0x38 entry point names (a "DIC " dictionary); 0x40 entry points
  actor     0x38 bytes: 0x00 name, 0x08 secondary name, 0x18 action names, 0x20 query names,
            0x30 u16 actions, u16 queries
  event     0x30 bytes: 0x00 name, 0x08 u8 type (0 action, 1 switch, 2 fork, 3 join, 4 subflow)
            action  0x0A u16 next, 0x0C u16 actor, 0x0E u16 action
            switch  0x0A u16 cases, 0x0C u16 actor, 0x0E u16 query, 0x18 cases (8 bytes each: u32 value,
                    u16 event; checked in Common_Camera_Pln and the bosses' death settings, G2)
            fork    0x0A u16 forks, 0x0C u16 join, 0x10 forks (u16 each)
            join    0x0A u16 next
            subflow 0x0A u16 next, 0x18 flowchart name ("" or none: this one), 0x20 entry point name
            action, switch and subflow: 0x10 their parameters (a container, or 0)
  container 0x00 u8 type 1, 0x02 u16 items, 0x08 the items' names (a "DIC "), 0x10 the items (u64 each)
  item      0x00 u8 type (2 int, 3 bool, 4 float, 5 string; others are not read), 0x10 the value
            (a string: u64 to it)
  entry     0x20 bytes: 0x1C u16 main event
  dict      "DIC ", u32 count, then at 0x08 (count + 1) entries of 0x10 (the first the root): 0x08 name
Names only: nothing but the game's own actor, action and entry names is read out.
"""
import struct
from collections import namedtuple

NONE = 0xFFFF

# A switch: `cases` its events, `values` their case values, `query` its query (an index in queries[actor]).
Event = namedtuple("Event", "type next actor action cases forks join flowchart entry params query values",
                   defaults=(NONE, ()))
# actors: [(name, secondary, actions)]; queries: [[query names] per actor]
Flowchart = namedtuple("Flowchart", "name actors events entries queries", defaults=((),))


def parse(data):
    """The flowchart of one .bfevfl file (decompressed)."""
    def u16(o):
        return struct.unpack_from("<H", data, o)[0]

    def u32(o):
        return struct.unpack_from("<I", data, o)[0]

    def u64(o):
        return struct.unpack_from("<Q", data, o)[0]

    def text(p):
        if not p:
            return ""
        return data[p + 2:p + 2 + u16(p)].decode("utf-8")

    def names(dic, n):
        if data[dic:dic + 4] != b"DIC ":
            raise ValueError("no dictionary")
        return [text(u64(dic + 8 + 0x10 * (k + 1) + 8)) for k in range(n)]

    def params(p):
        if not p or data[p] != 1:
            return {}
        out = {}
        for k, name in enumerate(names(u64(p + 8), u16(p + 2))):
            it = u64(p + 0x10 + 8 * k)
            kind = data[it]
            if kind == 2:
                out[name] = struct.unpack_from("<i", data, it + 0x10)[0]
            elif kind == 3:
                out[name] = u32(it + 0x10) != 0
            elif kind == 4:
                out[name] = struct.unpack_from("<f", data, it + 0x10)[0]
            elif kind == 5:
                out[name] = text(u64(it + 0x10))
        return out

    if data[:6] != b"BFEVFL":
        raise ValueError("not a BFEVFL file")
    if u16(0x20) == 0:
        return Flowchart("", [], [], {})  # a timeline only (a movie): no flowchart
    fc = u64(u64(0x28))
    if data[fc:fc + 4] != b"EVFL":
        raise ValueError("no EVFL flowchart")
    n_actors, _, _, n_events, n_entries = (u16(fc + 0x10 + 2 * i) for i in range(5))
    actors, queries = [], []
    for i in range(n_actors):
        a = u64(fc + 0x28) + 0x38 * i
        actions = [text(u64(u64(a + 0x18) + 8 * k)) for k in range(u16(a + 0x30))]
        actors.append((text(u64(a)), text(u64(a + 8)), actions))
        queries.append([text(u64(u64(a + 0x20) + 8 * k)) for k in range(u16(a + 0x32))])
    events = []
    for i in range(n_events):
        e = u64(fc + 0x30) + 0x30 * i
        kind = data[e + 8]
        nxt = actor = action = join = query = NONE
        cases, values, forks, flowchart, entry = (), (), (), "", ""
        if kind == 0:
            nxt, actor, action = u16(e + 0xA), u16(e + 0xC), u16(e + 0xE)
        elif kind == 1:
            actor, query, table = u16(e + 0xC), u16(e + 0xE), u64(e + 0x18)
            values = tuple(u32(table + 8 * k) for k in range(u16(e + 0xA)))
            cases = tuple(u16(table + 8 * k + 4) for k in range(u16(e + 0xA)))
        elif kind == 2:
            join = u16(e + 0xC)
            forks = tuple(u16(u64(e + 0x10) + 2 * k) for k in range(u16(e + 0xA)))
        elif kind == 3:
            nxt = u16(e + 0xA)
        elif kind == 4:
            nxt, flowchart, entry = u16(e + 0xA), text(u64(e + 0x18)), text(u64(e + 0x20))
        prm = params(u64(e + 0x10)) if kind in (0, 1, 4) else {}
        events.append(Event(kind, nxt, actor, action, cases, forks, join, flowchart, entry, prm, query, values))
    entries = {}
    for i, name in enumerate(names(u64(fc + 0x38), n_entries)):
        entries[name] = u16(u64(fc + 0x40) + 0x20 * i + 0x1C)
    return Flowchart(text(u64(fc + 0x20)), actors, events, entries, queries)


def actions_of(flows, flow, entry, load=None, transition=None, answer=None):
    """[(actor, action, params)] every action reachable from `entry` of `flow` (a subflow called is
    ("<subflow>", "flow:entry", params)), through switches, forks,
    subflows (also into other files) and, when `transition(event name)` gives the (flow, entry) an
    EventTransition's EventName plays, through those. `flows` is {name: Flowchart}; `load(name)` may add
    a missing one (None when there is none). An empty entry is the flow's default (EntryPoint0, else its
    first). Unknown entries and files give nothing. `answer(actor, query, params)` may give a switch's case
    value, so only that case is followed (None: every case). The game's Bool queries answer 0 for true:
    IsDreamWorld's case 0 in Get_StoneOfFire_DefeatBoss is the boss rush's banc change (G2, boss_beaten)."""
    out, seen = [], set()
    todo = [(flow, entry)]
    while todo:
        f, e = todo.pop()
        if (f, e) in seen:
            continue
        seen.add((f, e))
        chart = flows.get(f)
        if chart is None and load is not None:
            chart = load(f)
            if chart is not None:
                flows[f] = chart
        if chart is not None and not e and chart.entries:
            e = "EntryPoint0" if "EntryPoint0" in chart.entries else next(iter(chart.entries))  # the default
        if chart is None or e not in chart.entries:
            continue
        stack, visited = [chart.entries[e]], set()
        while stack:
            i = stack.pop()
            if i == NONE or i in visited or i >= len(chart.events):
                continue
            visited.add(i)
            ev = chart.events[i]
            if ev.type == 0:
                if ev.actor < len(chart.actors) and ev.action < len(chart.actors[ev.actor][2]):
                    actor, action = chart.actors[ev.actor][0], chart.actors[ev.actor][2][ev.action]
                    out.append((actor, action, ev.params))
                    target = ev.params.get("EventName") if action == "EventTransition" else None
                    if target and transition is not None and transition(target):
                        todo.append(transition(target))
                stack.append(ev.next)
            elif ev.type == 1:
                value = None
                known = ev.actor < len(chart.actors) and ev.actor < len(chart.queries) \
                    and ev.query < len(chart.queries[ev.actor])
                if answer is not None and known:
                    value = answer(chart.actors[ev.actor][0], chart.queries[ev.actor][ev.query], ev.params)
                if value is None or value not in ev.values:
                    stack.extend(ev.cases)
                else:
                    stack.extend(c for c, v in zip(ev.cases, ev.values) if v == value)
            elif ev.type == 2:
                stack.extend(ev.forks)
                stack.append(ev.join)
            elif ev.type == 3:
                stack.append(ev.next)
            elif ev.type == 4:
                out.append(("<subflow>", f"{ev.flowchart or f}:{ev.entry}", ev.params))
                todo.append((ev.flowchart or f, ev.entry))
                stack.append(ev.next)
    return out
