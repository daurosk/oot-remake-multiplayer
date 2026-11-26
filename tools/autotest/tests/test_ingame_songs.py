"""Regress step 11's selftest (ingame/test_songs.py + ingame/fakes/songs.py + saves/no-ocarina.py; owner regress-port-c),
run by test_ingame.py's load_tests: both tests NO-REF on the fake (look shots, no references), deselected without
their switches / save profile, FAIL (never PASS / NO-REF) under each bug, SETUP-FAIL without the gifts line, SKIP on a
boot whose frogs paid already, and the no-ocarina profile's saves (String64 EquipOcarina emptied) put in and back."""
import os
import shutil
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import save_flags as SF    # noqa: E402
import test_ingame as T    # noqa: E402
from test_save_flags import make_save   # noqa: E402

ROW = ["--env", "LICHII_COOP_CHEATS=1", "--env", "LICHII_COOP_OCARINA_PROBE=1"]
GIVEN = ROW + ["--env", "LICHII_COOP_PUPPET=1"]


def kit_save():
    """A save like the kit's: both ocarinas' flags set and EquipOcarina 'Item_Ocarina_Fairy' (test_save_flags'
    test_no_ocarina_takes_the_ocarina_away's form)."""
    data = make_save({
        0: [(h, 1) for k, h, _ in SF.NO_OCARINA if k == "bool"],
        2: [(h, 1) for k, h, _ in SF.NO_OCARINA if k == "int"],
        16: [(SF.EQUIP_OCARINA, 0)],
    }, pool_extra=b"Item_Ocarina_Fairy".ljust(64, b"\x00"))
    text = struct.unpack_from("<I", data, 8)[0]
    struct.pack_into("<I", data, SF.index(data)[(16, SF.EQUIP_OCARINA)], text)
    return bytes(data), text


def hpp_ocarina():
    """namespace ocarina's hash constants of coop/host/include/coop/host/game_items.hpp, {name: hash}."""
    import re
    path = os.path.join(HERE, "..", "..", "..", "coop", "host", "include", "coop", "host", "game_items.hpp")
    with open(path, encoding="utf-8") as f:
        text = f.read()
    body = text[text.index("namespace ocarina {"):]
    body = body[:body.index("}  // namespace ocarina")]
    return {m.group(1): int(m.group(2), 16)
            for m in re.finditer(r"constexpr std::uint32_t (k\w+) = (0x[0-9a-fA-F]{8});", body)}


class OneSource(unittest.TestCase):
    """The test's, the fake's and save_flags' ocarina hashes are the module's (ONE SOURCE OF TRUTH, review-c123-11)."""

    def test_pinned_to_game_items_hpp(self):
        sys.path.insert(0, T.INGAME)
        import test_songs as S
        k = hpp_ocarina()
        self.assertEqual(int(S.FAIRY_COUNT, 16), k["kFairy"])
        self.assertEqual(int(S.EQUIP, 16), k["kEquip"])
        self.assertEqual(SF.EQUIP_OCARINA, k["kEquip"])
        self.assertEqual({int(h, 16) for h in S.MENDED_BOOLS},   # ocarina_bools_mend for a held Fairy Ocarina
                         {k[n] for n in ("kIsGetFairy", "kGotFairy", "kSlotA", "kSlotB", "kPrepared")})
        self.assertEqual({h for _, h, _ in SF.NO_OCARINA},
                         {k[n] for n in ("kFairy", "kTime", "kIsGetFairy", "kIsGetTime", "kSlotA", "kSlotB",
                                         "kGotFairy", "kGotTime")})

    def test_profile_is_save_flags_no_ocarina(self):
        sys.path.insert(0, T.INGAME)
        import testsave
        self.assertEqual(testsave.load("no-ocarina"), [(SF.TYPES[k], h, v) for k, h, v in SF.NO_OCARINA])
        self.assertEqual(testsave.clear_text_of("no-ocarina"), [SF.EQUIP_OCARINA])


class Songs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.tpl = os.path.join(self.tmp, "tpl")
        self.base, self.text = kit_save()
        for s in ("slot_00", "slot_01"):
            os.makedirs(os.path.join(self.tpl, s))
            with open(os.path.join(self.tpl, s, "progress.sav"), "wb") as f:
                f.write(self.base)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_fake(self, name, k, env_args=ROW, bug=None, extra=()):
        out = os.path.join(self.tmp, name)
        env = {"BT_FAKE_REFS": os.path.join(self.tmp, "no-refs"), "BT_FAKE_SAVE_DIR": self.tpl}
        if bug:
            env["BT_FAKE_SONGS_BUG"] = bug
        p = T.pytest_run([".", "--fake", "--out", out] + list(env_args) + (["--no-rerun"] if bug else []) +
                         list(extra) + ["-q", "-p", "no:cacheprovider", "-k", k], env=env)
        return p, T.summary(out), out

    def why(self, s, test):
        return [t["why"] for t in s["tests"] if t["test"].endswith(test)][0]

    # -- the frogs -----------------------------------------------------------------------------------------------
    def test_frogs_ok_is_no_ref(self):
        p, s, _ = self.run_fake("frogs", "test_frog_songs")
        self.assertEqual(T.results(s), {"test_frog_songs": "NO-REF"}, p.stdout[-2500:])
        self.assertIn("no stored reference", self.why(s, "test_frog_songs"))

    def test_frogs_not_shared_fails(self):
        p, s, _ = self.run_fake("noshare", "test_frog_songs", bug="noshare")
        self.assertEqual(T.results(s), {"test_frog_songs": "FAIL"}, p.stdout[-2500:])
        self.assertIn("A gets the frogs' ZeldaLullaby Bool", self.why(s, "test_frog_songs"))

    def test_song_not_taken_fails(self):
        p, s, _ = self.run_fake("nopay", "test_frog_songs", bug="nopay")
        self.assertEqual(T.results(s), {"test_frog_songs": "FAIL"}, p.stdout[-2500:])
        self.assertIn("the frogs did not pay ZeldaLullaby", self.why(s, "test_frog_songs"))

    def test_no_gifts_line_is_setup_fail(self):
        p, s, _ = self.run_fake("nogifts", "test_frog_songs", bug="nogifts")
        self.assertEqual(T.results(s), {"test_frog_songs": "SETUP-FAIL"}, p.stdout[-2500:])

    def test_repeat_round_two_frogs_skip(self):
        p, s, _ = self.run_fake("rep", "test_frog_songs", extra=["--repeat", "2", "--no-smoke-gate"])
        self.assertEqual(T.results(s), {"test_frog_songs[r1]": "NO-REF", "test_frog_songs[r2]": "SKIP"},
                         p.stdout[-2500:])

    def test_without_the_switches_deselected(self):
        p, s, _ = self.run_fake("none", "test_frog_songs", env_args=())
        self.assertEqual(T.results(s), {}, p.stdout[-1500:])
        self.assertTrue(any(d["test"].endswith("test_frog_songs") for d in s.get("deselected", [])), s)

    # -- the given ocarina ---------------------------------------------------------------------------------------
    def assert_saves_as_before(self, out):
        for x in ("A", "B"):
            for sl in ("slot_00", "slot_01"):
                with open(os.path.join(out, "lane1", "fake", "lane1-" + x, "autotest-save", sl, "progress.sav"),
                          "rb") as f:
                    self.assertEqual(f.read(), self.base, (x, sl))

    def test_given_ocarina_ok_is_no_ref_and_saves_back(self):
        p, s, out = self.run_fake("given", "test_ocarina_given_saria", env_args=GIVEN, extra=["--save", "no-ocarina"])
        self.assertEqual(T.results(s), {"test_ocarina_given_saria": "NO-REF"}, p.stdout[-2500:] + p.stderr[-1500:])
        self.assertIn("no stored reference", self.why(s, "test_ocarina_given_saria"))
        self.assertEqual(s["identity"]["save"]["name"], "no-ocarina")
        self.assert_saves_as_before(out)

    def test_given_ocarina_not_mended_fails(self):
        p, s, _ = self.run_fake("noequip", "test_ocarina_given_saria", env_args=GIVEN, bug="noequip",
                                extra=["--save", "no-ocarina"])
        self.assertEqual(T.results(s), {"test_ocarina_given_saria": "FAIL"}, p.stdout[-2500:])
        self.assertIn("for the held ocarina", self.why(s, "test_ocarina_given_saria"))

    def test_given_ocarina_without_the_save_deselected(self):
        p, s, _ = self.run_fake("nosave", "test_ocarina_given_saria", env_args=GIVEN)
        self.assertEqual(T.results(s), {}, p.stdout[-1500:])
        self.assertIn("no-ocarina", [d.get("needs_save") for d in s.get("deselected", [])
                                     if d["test"].endswith("test_ocarina_given_saria")])

    def test_profile_empties_equip_ocarina_and_takes_the_flags(self):
        import testsave
        folder = os.path.join(self.tmp, "game")
        shutil.copytree(self.tpl, os.path.join(folder, testsave.BASE))
        testsave.put_in([folder], "no-ocarina", testsave.load("no-ocarina"), say=lambda m: None)
        for rel in testsave.saves_of(folder):
            data = SF.read_file(os.path.join(folder, *rel.split("/")))
            self.assertEqual(bytes(data[self.text:self.text + 64]), bytes(64), rel)
            for k, h, _ in SF.NO_OCARINA:
                self.assertEqual(SF.get(data, SF.TYPES[k], h), 0, (rel, hex(h)))
        testsave.undo(folder, say=lambda m: None)
        for rel in testsave.saves_of(folder):
            with open(os.path.join(folder, *rel.split("/")), "rb") as f:
                self.assertEqual(f.read(), self.base, rel)

    def test_profile_on_a_save_without_equip_ocarina_is_an_error(self):
        import testsave
        folder = os.path.join(self.tmp, "game2")
        os.makedirs(os.path.join(folder, testsave.BASE, "slot_00"))
        data = make_save({0: [(h, 1) for k, h, _ in SF.NO_OCARINA if k == "bool"],
                          2: [(h, 1) for k, h, _ in SF.NO_OCARINA if k == "int"]})
        with open(os.path.join(folder, testsave.BASE, "slot_00", "progress.sav"), "wb") as f:
            f.write(bytes(data))
        with self.assertRaisesRegex(testsave.SaveProfileError, "not in this save: 0xb72cd3fc"):
            testsave.put_in([folder], "no-ocarina", testsave.load("no-ocarina"), say=lambda m: None)
        testsave.undo(folder, say=lambda m: None)


if __name__ == "__main__":
    unittest.main()
