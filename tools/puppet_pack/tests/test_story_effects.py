"""Tests for puppet_pack.story_effects (row 129, GitHub #173). No romfs here: the walk and the rules on made-up
flows, and the committed coop_story_effects.inc checked against the hook's shared lists."""
import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))

from puppet_pack import evfl  # noqa: E402
from puppet_pack import story_effects as se  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
N = evfl.NONE
SET = se.SET_BOOL
ACTORS = [("EventSystemActor", "", (SET, "EventTransition")), ("Owl", "", ("Talk",))]
QUERIES = [("EventQueryIsGameDataBool",), ()]


def act(nxt, action, **params):
    return evfl.Event(0, nxt, 0, action, (), (), N, None, None, params)


def flag(name, value, nxt=N):
    return act(nxt, 0, GameDataName=name, Value=value)


def chart(events, entry="Start"):
    return evfl.Flowchart("Owl", ACTORS, events, {entry: 0}, QUERIES)


def walk(c, more=None, transition=lambda _n: None):
    flows = {"Owl": c, **(more or {})}
    return se.walk(flows, "Owl", "Start", lambda _n: None, transition)


class Walk(unittest.TestCase):
    def test_a_switch_makes_what_follows_conditional(self):
        events = [
            flag("A", True, 1),
            evfl.Event(1, N, 0, 0, (2, 3), (), N, None, None, {"GameDataName": "Q"}, 0, (0, 1)),
            flag("B", True, 3),
            flag("C", True),
        ]
        got = {(p.get("GameDataName"), c) for _a, action, p, c in walk(chart(events)) if action == SET}
        self.assertEqual(got, {("A", False), ("B", True), ("C", True)})

    def test_subflows_forks_and_transitions_are_followed(self):
        other = evfl.Flowchart("Next", ACTORS, [flag("T", True)], {"Go": 0}, QUERIES)
        events = [
            evfl.Event(2, N, 0, 0, (), (1, 2), 3, None, None, {}),
            flag("F1", True, 3),
            flag("F2", True, 3),
            evfl.Event(3, 4, 0, 0, (), (), N, None, None, {}),
            act(N, 1, EventName="NextSetting"),
        ]
        got = walk(chart(events), {"Next": other}, lambda n: ("Next", "Go") if n == "NextSetting" else None)
        names = {p.get("GameDataName") for _a, action, p, c in got if action == SET and not c}
        self.assertEqual(names, {"F1", "F2", "T"})


def bools(**rows):
    """{hash: (reset, default)} from name=(reset, default)."""
    return {se.murmur3(k): v for k, v in rows.items()}


def actions(*writes, asked=()):
    out = [("EventSystemActor", SET, {"GameDataName": n, "Value": v}, c) for n, v, c in writes]
    out += [("EventSystemActor", "EventQueryIsGameDataBool", {"GameDataName": n}, False) for n in asked]
    return out


class Classify(unittest.TestCase):
    SAVED_OFF = (se.SAVED, False)

    def run_one(self, writes, rows, shared=(), all_writes=None, setters=None, asked=()):
        flow_writes = all_writes if all_writes is not None else {n: {v} for n, v, _c in writes}
        return se.classify("S", actions(*writes, asked=asked), rows, set(shared), flow_writes, setters or {})

    def test_the_owl_case_is_kept(self):
        kept, out = self.run_one([("HyruleField_ArriveEntrance", True, False)],
                                 bools(HyruleField_ArriveEntrance=self.SAVED_OFF))
        self.assertEqual(kept, [("HyruleField_ArriveEntrance", True)])
        self.assertEqual(out, [])

    def test_each_reason_to_leave_out(self):
        cases = [
            ("behind a switch", dict(writes=[("X", True, True)])),
            ("shared", dict(writes=[("X", True, False)], shared=["X"])),
            ("not a saved flag", dict(writes=[("X", True, False)], rows=bools(X=(1, False)))),
            ("already its default", dict(writes=[("X", True, False)], rows=bools(X=(se.SAVED, True)))),
            ("written both ways", dict(writes=[("X", True, False)], all_writes={"X": {True, False}})),
            ("set by an AI graph", dict(writes=[("X", True, False)],
                                        setters={"X": {("Room: TriggerGameDataSetBoolFalse", False)}})),
            ("asked by a switch", dict(writes=[("X", True, False)], asked=["X"])),
            ("a played flag", dict(writes=[("HavePlayedEvent.Y", True, False)],
                                   rows=bools(**{"HavePlayedEvent.Y": self.SAVED_OFF}))),
        ]
        for why, kw in cases:
            with self.subTest(why):
                kw.setdefault("rows", bools(X=self.SAVED_OFF))
                kept, out = self.run_one(**kw)
                self.assertEqual(kept, [])
                self.assertIn(why, out[0][2])

    def test_a_graph_that_sets_the_same_value_is_fine(self):
        kept, _ = self.run_one([("X", True, False)], bools(X=self.SAVED_OFF),
                               setters={"X": {("Room: TriggerGameDataSetBoolTrue", True)}})
        self.assertEqual(kept, [("X", True)])

    def test_graph_setters_read_the_value_from_the_node_type(self):
        graphs = [("Room", [
            {"type": "TriggerGameDataSetBoolTrue", "imm": {"GameDataName": "A"}, "sdef": {}},
            {"type": "TriggerGameDataSetBoolFalse", "imm": {}, "sdef": {"Name": "B"}},
            {"type": "SeqOneShotSetGameDataBool", "imm": {"GameDataName": "C"}, "sdef": {}},
            {"type": "HoldGameDataGetBoolSimple", "imm": {"GameDataName": "D"}, "sdef": {}},
        ])]
        got = se.graph_setters(graphs)
        self.assertEqual({v for _w, v in got["A"]}, {True})
        self.assertEqual({v for _w, v in got["B"]}, {False})
        self.assertEqual({v for _w, v in got["C"]}, {None})
        self.assertNotIn("D", got)


class Committed(unittest.TestCase):
    def setUp(self):
        with open(os.path.join(REPO, "lichii", "hook", "coop_story_effects.inc"), encoding="utf-8") as f:
            self.inc = f.read()
        self.rows = re.findall(r'\{0x([0-9a-f]{8})u, 0x([0-9a-f]{8})u, (true|false), "(\w+)", "([\w.]+)"\}', self.inc)
        texts = []
        for src in se.SHARED_SOURCES:
            with open(os.path.join(REPO, src), encoding="utf-8") as f:
                texts.append(f.read())
        self.shared = set().union(*(se.quoted_names(t) for t in texts))
        self.played = se.played_settings(texts[:len(se.PLAYED_SOURCES)])

    def test_the_owl_row_is_there(self):
        self.assertIn(("98bf10c1", "804aec99", "true", "HyruleField_Goto_HyruleCastle_Kaepora",
                       "HyruleField_ArriveEntrance"), self.rows)

    def test_rows_hash_their_names_and_follow_the_rules(self):
        self.assertTrue(self.rows)
        for played, flag_hash, _v, setting, name in self.rows:
            with self.subTest(name):
                self.assertEqual(int(played, 16), se.murmur3("HavePlayedEvent." + setting))
                self.assertEqual(int(flag_hash, 16), se.murmur3(name))
                self.assertIn(setting, self.played)
                self.assertNotIn(name, self.shared)
        self.assertEqual(len(self.rows), len({(r[0], r[1]) for r in self.rows}))

    def test_render_writes_the_rows_back(self):
        rows = [(s, n, v == "true") for _p, _f, v, s, n in self.rows]
        self.assertEqual(re.findall(r"\{0x.*\},", se.render(rows, [])), re.findall(r"\{0x.*\},", self.inc))


if __name__ == "__main__":
    unittest.main()
