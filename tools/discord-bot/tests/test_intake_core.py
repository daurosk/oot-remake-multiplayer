"""Unit tests for the problem-report intake's checks (no discord.py, no aiohttp, no network)."""

import datetime as dt
import io
import json
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import intake_core as intake  # noqa: E402
import report_core as core  # noqa: E402

WHEN = dt.datetime(2026, 11, 21, 18, 5, 9, tzinfo=dt.timezone.utc)
INSTALL = "0123456789abcdef0123456789abcdef"
RID = "fedcba9876543210fedcba9876543210"
REPORT = ("Squadala! problem report: 2 new problem(s) from 1 session(s)\r\n"
          "Kit: kit: Squadala-v0.11.9-windows (release v0.11.9)\r\n"
          "Addresses, names, codes and passwords are masked (as collect-logs masks them).\r\n\r\n"
          "3x [hos Presentation Thread] coop: boss: could not move into the boss room, giving up\r\n"
          "1x [hos Net] coop: net: <name> timed out | `ping` [x](http://evil)\r\n")


def make_zip(files, method=zipfile.ZIP_DEFLATED):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", method) as z:
        for name, text in files.items():
            z.writestr(name, text if isinstance(text, bytes) else text.encode("utf-8"))
    return buf.getvalue()


class HealthCheck(unittest.TestCase):
    def test_watchdog_empty_post_is_a_check(self):
        self.assertTrue(intake.is_health_check("0", None, None))
        self.assertTrue(intake.is_health_check(None, "", " "))

    def test_a_kit_upload_is_never_a_check(self):
        self.assertFalse(intake.is_health_check("100", INSTALL, RID))
        self.assertFalse(intake.is_health_check("0", INSTALL, RID))   # (an empty body with ids: a broken kit, logged)
        self.assertFalse(intake.is_health_check("100", None, None))   # (a body: someone sent something)


class Headers(unittest.TestCase):
    def ok(self, **kw):
        args = dict(method="POST", path=intake.PATH, content_type="application/zip", content_length="100",
                    install=INSTALL, report_id=RID)
        args.update(kw)
        return intake.check_headers(**args)

    def status(self, **kw):
        with self.assertRaises(intake.Refused) as c:
            self.ok(**kw)
        return c.exception.status

    def test_good(self):
        self.assertEqual(self.ok(install=INSTALL.upper(), content_type="application/zip; charset=x"), (INSTALL, RID))

    def test_refusals(self):
        self.assertEqual(self.status(path="/"), 404)
        self.assertEqual(self.status(path="/v1/problem-report/../x"), 404)
        self.assertEqual(self.status(method="GET"), 405)
        self.assertEqual(self.status(content_type="text/plain"), 415)
        self.assertEqual(self.status(content_type=None), 415)
        self.assertEqual(self.status(content_length=None), 411)
        self.assertEqual(self.status(content_length="x"), 400)
        self.assertEqual(self.status(content_length="0"), 400)
        self.assertEqual(self.status(content_length=str(intake.MAX_UPLOAD_BYTES + 1)), 413)
        self.assertEqual(self.status(install="not-hex"), 400)
        self.assertEqual(self.status(install=INSTALL + "0"), 400)
        self.assertEqual(self.status(report_id=""), 400)


class Zip(unittest.TestCase):
    def refused(self, data):
        with self.assertRaises(intake.Refused) as c:
            intake.check_zip(data)
        return c.exception.reason

    def test_good_zip(self):
        files = intake.check_zip(make_zip({"crash-record-1.txt": "crash\n", "problem-report.txt": REPORT}))
        self.assertEqual(list(files), ["problem-report.txt", "crash-record-1.txt"])
        self.assertIn("giving up", files["problem-report.txt"])

    def test_stored_and_bom(self):
        files = intake.check_zip(make_zip({"problem-report.txt": b"\xef\xbb\xbf" + REPORT.encode()}, zipfile.ZIP_STORED))
        self.assertTrue(files["problem-report.txt"].startswith("Squadala!"))

    def test_not_a_zip(self):
        self.assertIn("not a real zip", self.refused(b"hello"))
        self.assertIn("readable", self.refused(b"PK\x03\x04garbage"))
        self.assertIn("empty", self.refused(b""))

    def test_only_kit_names(self):
        for name in ("../problem-report.txt", "x/problem-report.txt", "crash-record-4.txt", "evil.exe",
                     "Problem-Report.txt"):
            self.assertIn("unexpected", self.refused(make_zip({"problem-report.txt": REPORT, name: "x"})), name)
        self.assertIn("no problem-report.txt", self.refused(make_zip({"crash-record-1.txt": "x"})))
        self.assertIn("files", self.refused(make_zip({"problem-report.txt": REPORT, "crash-record-1.txt": "a",
                                                      "crash-record-2.txt": "b", "crash-record-3.txt": "c",
                                                      "crash-record-1.txt ": "d"})))

    def test_not_ours(self):
        self.assertIn("not a Squadala", self.refused(make_zip({"problem-report.txt": "hello"})))

    def test_zip_bomb(self):
        bomb = make_zip({"problem-report.txt": REPORT, "crash-record-1.txt": "A" * (intake.MAX_ENTRY_BYTES * 3)})
        self.assertLess(len(bomb), intake.MAX_UPLOAD_BYTES)
        self.assertIn("too", self.refused(bomb))
        ratio = make_zip({"problem-report.txt": REPORT, "crash-record-1.txt": "A" * 100000})
        self.assertIn("ratio", self.refused(ratio))

    def test_lying_header(self):
        # The headers say the 300 KB entry holds 10 bytes: never more than that is taken, and the CRC then fails.
        data = make_zip({"problem-report.txt": REPORT, "crash-record-1.txt": bytes(300000)})
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            real = z.getinfo("crash-record-1.txt").file_size
        lied = data.replace(real.to_bytes(4, "little"), (10).to_bytes(4, "little"))
        self.assertNotEqual(lied, data)
        self.assertIn("damaged", self.refused(lied))

    def test_binary_and_bad_utf8(self):
        self.assertIn("plain text", self.refused(make_zip({"problem-report.txt": REPORT + "\x00"})))
        self.assertIn("UTF-8", self.refused(make_zip({"problem-report.txt": REPORT.encode() + b"\xff\xfe"})))

    def test_version_and_lines(self):
        self.assertEqual(intake.report_version(REPORT), "v0.11.9")
        self.assertEqual(intake.report_version("Kit: kit: oot-coop-20261121"), "oot-coop-20261121")
        self.assertIsNone(intake.report_version("no kit line"))
        lines, count = intake.problem_lines(REPORT)
        self.assertEqual(count, 2)
        self.assertTrue(lines[0].startswith("3x "))


class Rates(unittest.TestCase):
    def test_per_install_and_release(self):
        now = [0.0]
        lim = intake.Limits(clock=lambda: now[0])
        for i in range(intake.INSTALL_LIMIT):
            lim.acquire(INSTALL, f"addr{i}")
        with self.assertRaises(intake.Refused) as c:
            lim.acquire(INSTALL, "addr-x")
        self.assertEqual(c.exception.status, 429)
        self.assertGreater(c.exception.retry_after, 0)
        # The refusal took nothing: another install from the same address still passes.
        lim.acquire(RID, "addr-x")
        now[0] += intake.INSTALL_WINDOW + 1
        taken = lim.acquire(INSTALL, "addr-y")
        intake.Limits.release(taken)
        lim.acquire(INSTALL, "addr-y")

    def test_per_address(self):
        lim = intake.Limits(clock=lambda: 0.0)
        for i in range(intake.ADDRESS_LIMIT):
            lim.acquire(f"{i:032x}", "one-address")
        with self.assertRaises(intake.Refused):
            lim.acquire("f" * 32, "one-address")

    def test_per_day(self):
        now = [0.0]
        lim = intake.Limits(clock=lambda: now[0])
        for i in range(intake.INSTALL_DAY_LIMIT):
            lim.acquire(INSTALL, f"a{i}")
            now[0] += intake.INSTALL_WINDOW  # an hour apart: the hourly limit never bites
        with self.assertRaises(intake.Refused):
            lim.acquire(INSTALL, "a-x")


class Address(unittest.TestCase):
    def test_sender(self):
        self.assertEqual(intake.sender_address("CF-Connecting-IP", {"CF-Connecting-IP": "203.0.113.9"}, "127.0.0.1"),
                         "203.0.113.9")
        self.assertEqual(intake.sender_address("X-Forwarded-For", {"X-Forwarded-For": "198.51.100.1, 10.0.0.1"}, "::1"),
                         "198.51.100.1")
        self.assertIsNone(intake.sender_address("CF-Connecting-IP", {}, "127.0.0.1"))
        self.assertIsNone(intake.sender_address("", {}, "127.0.0.1"))
        self.assertEqual(intake.sender_address("", {}, "198.51.100.7"), "198.51.100.7")

    def test_no_address_skips_that_limit(self):
        lim = intake.Limits(clock=lambda: 0.0)
        for i in range(intake.ADDRESS_LIMIT + 5):
            lim.acquire(f"{i:032x}", None)


class Config(unittest.TestCase):
    def test_off_and_defaults(self):
        self.assertIsNone(intake.load_intake_config(None))
        self.assertIsNone(intake.load_intake_config({"enabled": False, "port": 1}))
        self.assertEqual(intake.load_intake_config({"port": 9000}),
                         {"host": "127.0.0.1", "port": 9000, "address_header": ""})
        self.assertEqual(intake.load_intake_config({"address_header": "CF-Connecting-IP"})["address_header"],
                         "CF-Connecting-IP")

    def test_bad(self):
        for raw in ("x", {"port": "x"}, {"port": 70000}, {"address_header": "a b"}):
            with self.assertRaises(core.ConfigError):
                intake.load_intake_config(raw)

    def test_load_config_passes_intake_through(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "secrets" / "discord-bot.json"
            p.parent.mkdir()
            p.write_text(json.dumps({"token": "x" * 30, "dev_channel_id": "123", "intake": {"port": 8787}}))
            cfg = core.load_config(p)
            self.assertEqual(cfg["intake_raw"], {"port": 8787})


class Save(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.dir = Path(self._td.name)
        self.tracker = self.dir / "player-reports.md"
        self.tracker.write_text("# t\n\n| # | a | b | c | d | e | f | g |\n|---|---|---|---|---|---|---|---|\n"
                                "| 209 | x | y | z | r | NEW (bot) | - | - |\n", encoding="utf-8")

    def tearDown(self):
        self._td.cleanup()

    def test_filed_as_new_auto_quoted(self):
        data = make_zip({"problem-report.txt": REPORT, "crash-record-1.txt": "Exception 0xc0000005\n"})
        files = intake.check_zip(data)
        number, folder = intake.save_upload(self.dir / "player-logs", self.tracker, WHEN, INSTALL, RID, data, files)
        self.assertEqual(number, 210)
        self.assertEqual(folder.name, "auto-01234567-20261121-180509")
        self.assertEqual((folder / "problem-report.zip").read_bytes(), data)
        row = self.tracker.read_text(encoding="utf-8").splitlines()[-1]
        self.assertTrue(row.startswith("| 210 | 11-21 18:05 UTC (kit auto-send) | auto-01234567 | v0.11.9 | "))
        self.assertIn("| NEW (auto) |", row)
        self.assertIn(core.UNTRUSTED_LABEL + ": `", row)
        self.assertIn("1 crash record(s)", row)
        self.assertEqual(row.count("|"), 9)          # the player's | became /
        self.assertNotIn("`ping`", row)              # backticks cannot close the code span
        desc = (folder / "description.txt").read_text(encoding="utf-8")
        self.assertIn("Report #210", desc)
        self.assertIn(f"{core.UNTRUSTED_LABEL} (crash-record-1.txt):\n```text\nException", desc)
        self.assertEqual(desc.count("```"), 4)

    def test_dev_post(self):
        text = intake.dev_post_text(intake.check_zip(make_zip({"problem-report.txt": REPORT})))
        self.assertNotIn("`", text)
        self.assertIn("giving up", text)

    def test_seen_ids(self):
        p = self.dir / "player-logs" / "auto-intake.json"
        seen = intake.SeenIds(p)
        self.assertNotIn(RID, seen)
        seen.add(RID)
        self.assertIn(RID, intake.SeenIds(p))
        p.write_text("not json")
        self.assertNotIn(RID, intake.SeenIds(p))


if __name__ == "__main__":
    unittest.main()
