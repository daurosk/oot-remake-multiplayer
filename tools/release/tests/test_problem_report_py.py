"""Tests for the Linux kit's problem reports (tools/release/kit/linux-longshot/scripts/problem_report.py): the same
checks as problem_report_test.ps1 and problem_send_test.ps1 for the Windows kit. Made-up log lines only.

    python tools/release/tests/test_problem_report_py.py -v
"""

import http.server
import io
import json
import sys
import tempfile
import threading
import unittest
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "kit" / "linux-longshot" / "scripts"))
import problem_report as pr  # noqa: E402

USER, PC = "testuser", "testpc-01"
PROBLEMS = [
    "[hos sead::MainThread] coop: on, server 192.168.1.20:27510, name 'Saria', join password set",
    "[hos sead::MainThread] coop: pose: Player_Child 309 bones, 0 moving; 0 sent/s, 0 received/s, 0 refused/s",
    "[hos Presentation Thread] coop: boss: could not move into the boss room after 5 tries; giving up",
    "[hos Presentation Thread] coop: boss: could not move into the boss room after 5 tries; giving up",
    "[hos host] coop: host: player 2 left: Mido (timed out: nothing heard for 10003 ms from 203.0.113.9:27510)",
    "[hos host] warning: coop: save: could not write /home/testuser/Squadala/data/game/save/slot_01",
    "[hos host] coop: guard: a fault in co-op at 0x80021234 was caught (peer fe80::1c2b:3a4d:5e6f:7a8b%12)",
    "[hos host] coop: invite: could not copy: invite code: ABCDE-FGHJK-LMNPQ-RSTUV",
    "[hos host] coop: player 3 'Ruto' joined",
    "[hos host] coop: boss: could not hand Gohma to Saria or Ruto",
]


def zip_text(path):
    with zipfile.ZipFile(path) as z:
        return {n: z.read(n).decode("utf-8") for n in z.namelist()}


class Masking(unittest.TestCase):
    def test_addresses(self):
        m = pr.Mask()
        t = pr.protect("a 192.168.1.20 b 192.168.1.20 c 203.0.113.9 d 10.0.26200.0 v1.2.3.4 e ::ffff:192.168.1.20", m, USER, PC)
        self.assertEqual(t, "a <lan-1> b <lan-1> c <public-1> d 10.0.26200.0 v1.2.3.4 e <lan-1>")
        self.assertEqual(pr.protect("fe80::1%12 and 2001:db8::5 and 00:11:22:33:44:55", pr.Mask(), USER, PC),
                         "<lan-1> and <public-1> and <mac>")

    def test_secrets_and_names(self):
        t = pr.protect("password = hunter2", pr.Mask(), USER, PC)
        self.assertEqual(t, "password = (removed)")
        self.assertIn("(removed)", pr.protect("invite code: ABCDE-FGHJK-LMNPQ", pr.Mask(), USER, PC))
        self.assertEqual(pr.protect("/home/testuser/x on testpc-01", pr.Mask(), USER, PC), "/home/<user>/x on <pc>")
        self.assertEqual(pr.protect("C:\\Users\\Bob\\x", pr.Mask(), "", ""), "C:\\Users\\<user>\\x")
        self.assertEqual(pr.protect("link is here", pr.Mask(), "link", ""), "link is here")
        self.assertEqual(pr.protect("server = myhost.example.org", pr.Mask(), "", ""), "server = <host>")

    def test_idempotent(self):
        once = pr.protect(" ".join(PROBLEMS), pr.Mask(), USER, PC)
        self.assertEqual(pr.protect(once, pr.Mask(), USER, PC), once)

    def test_problem_lines(self):
        self.assertTrue(pr.is_problem(PROBLEMS[2]))
        self.assertFalse(pr.is_problem(PROBLEMS[1]))
        self.assertFalse(pr.is_problem("[hos host] coop: scene: Lost Woods, entrance 3"))
        self.assertFalse(pr.is_problem("[hos host] coop: guard: crash handler is on"))
        self.assertFalse(pr.is_problem("[hos host] coop: actors: unfailing wrongness"))
        self.assertTrue(pr.is_problem("[hos host] CRASH: access violation at 0x1234"))
        self.assertTrue(pr.is_problem("[hos host] Warning: coop: x"))

    def test_gate(self):
        self.assertTrue(pr.has_address("lost 203.0.113.9"))
        self.assertTrue(pr.has_address("lost myhost.duckdns.org"))
        self.assertTrue(pr.has_address("lost myhost:27510"))
        self.assertFalse(pr.has_address("lost <public-1>:27510 in hookshot_coop.dll"))

    def test_fingerprint(self):
        self.assertEqual(pr.fingerprint("[a] coop: x <lan-1> 5 tries"), pr.fingerprint("[b] coop: x <lan-2> 7 tries"))
        self.assertNotEqual(pr.fingerprint("coop: x"), pr.fingerprint("coop: y"))


class Kit(unittest.TestCase):
    def setUp(self):
        self._td = tempfile.TemporaryDirectory()
        self.root = Path(self._td.name)
        self.kit = pr.Kit(self.root / "kit")
        self.kit.logs.mkdir(parents=True)
        (self.root / "kit" / "scripts").mkdir()
        self.game = self.root / "game"
        (self.game / "bug-reports" / "pending").mkdir(parents=True)

    def tearDown(self):
        self._td.cleanup()

    def log(self, name, run_id, lines):
        p = self.game / name
        p.write_text("\r\n".join([f"[hos host] run_id: {run_id}"] + lines), encoding="utf-8")
        return str(p)

    def add(self, logs, version="kit: test"):
        return pr.add_report(self.kit, version, logs, [self.game], USER, PC)

    def test_url(self):
        self.assertEqual(self.kit.url(), "")
        f = self.root / "kit" / "scripts" / "problem-send-url.txt"
        f.write_text("# comment\n\nhttps://reports.example.org/v1/problem-report\n")
        self.assertEqual(self.kit.url(), "https://reports.example.org/v1/problem-report")
        f.write_text("http://reports.example.org/v1/problem-report\n")
        self.assertEqual(self.kit.url(), "")

    def test_ask(self):
        self.assertTrue(pr.ask("https://x.org/p", 1, lambda s: "y\n", out=lambda *a: None))
        self.assertFalse(pr.ask("https://x.org/p", 1, lambda s: "\n", out=lambda *a: None))
        self.assertFalse(pr.ask("https://x.org/p", 1, lambda s: None, out=lambda *a: None))

    def test_choice(self):
        s = pr.set_choice(self.kit, False)
        self.assertEqual((s["send"], s["install"]), ("off", ""))
        self.assertEqual(pr.read_report_state(self.kit.report_state)["choice"], "off")
        s = pr.set_choice(self.kit, True)
        self.assertRegex(s["install"], r"^[0-9a-f]{32}$")
        self.assertEqual(pr.read_send_state(self.kit.send_state)["install"], s["install"])
        self.assertEqual(pr.read_report_state(self.kit.report_state)["choice"], "on")

    def test_report_masked_once(self):
        pr.set_choice(self.kit, True)
        quiet = self.log("longshot_log.1.txt", "1" * 32, ["[hos host] coop: host: player 2 joined: Mido"])
        self.assertIsNone(self.add([quiet]))
        log = self.log("longshot_log.2.txt", "2" * 32, PROBLEMS)
        (self.game / "bug-reports" / "pending" / "crash-20261121T180000Z.txt").write_text(
            "Exception in longshot+0x1234\nPath: /home/testuser/Squadala/data/game/longshot\nPC: testpc-01\nserver 192.168.1.20\n"
            "last peer Mido, then Ruto; player 4 'Darunia' joined\nDarunia left\n")
        (self.game / "bug-reports" / "pending" / "unclean-20261121T170000Z.txt").write_text("x")
        z = self.add([quiet, log], "kit: Squadala-v0.11.9-linux (release v0.11.9)")
        self.assertRegex(z.name, r"^\d{8}-\d{6}-[0-9a-f]{32}\.zip$")
        files = zip_text(z)
        self.assertEqual(sorted(files), ["crash-record-1.txt", "problem-report.txt"])
        text = files["problem-report.txt"]
        self.assertIn("2x [hos Presentation Thread] coop: boss: could not move", text)
        for secret in ("192.168", "203.0.113", "fe80", "ABCDE", "Mido", "Saria", "Ruto", "testuser"):
            self.assertNotIn(secret, text, secret)
        self.assertNotIn("pose:", text)
        crash = files["crash-record-1.txt"]
        self.assertIn("/home/<user>/Squadala", crash)
        self.assertIn("PC: <pc>", crash)
        self.assertIn("<lan-1>", crash)
        for name in ("Mido", "Ruto", "Darunia"):   # from the logs, and the record's own name line
            self.assertNotIn(name, crash, name)
        self.assertIn("last peer <name>, then <name>", crash)
        self.assertEqual(pr.check_zip(z, USER, PC), "")
        self.assertEqual(len(list(self.kit.logs.glob("coop-report-*.zip"))), 1)
        # never twice
        self.assertIsNone(self.add([quiet, log]))
        again = self.log("longshot_log.3.txt", "3" * 32, [PROBLEMS[2]])
        self.assertIsNone(self.add([again]))
        (self.game / "bug-reports" / "pending" / "crash-20261121T190000Z-12-1.txt").write_text("Exception again near Saria\n")
        only_zip = self.add([quiet, log])   # sessions read before: their names still mask a new crash record
        only = zip_text(only_zip)
        self.assertIn("again near <name>", only["crash-record-1.txt"])
        self.assertIn("1 crash record", only["problem-report.txt"])
        self.assertTrue(only["crash-record-1.txt"].startswith("crash-20261121T190000Z.txt"))
        # once the bot took them, the game's title is told (coop-sent.txt, by their own names)
        self.assertEqual(pr.send_queue(self.kit, "https://x.org/p", lambda *a: ("sent", ""), USER, PC),
                         sorted([(z.name, "sent"), (only_zip.name, "sent")]))
        sent = (self.game / "bug-reports" / "coop-sent.txt").read_text().split()
        self.assertEqual(sorted(sent), ["crash-20261121T180000Z.txt", "crash-20261121T190000Z-12-1.txt"])
        self.assertEqual(list(self.kit.send_dir.glob("*.crashes")), [])

    def test_name_before_join(self):
        log = self.log("longshot_log.4.txt", "4" * 32, [
            "[hos host] coop: net: Nabooru timed out",
            "[hos host] coop: host: player 2 joined: Nabooru"])
        rep = pr.new_report([log], pr.read_report_state(self.kit.report_state), self.kit.logs, "", USER, PC)
        self.assertIn("<name> timed out", rep[2])
        self.assertNotIn("Nabooru", rep[2])

    def test_names_across_sessions(self):
        a = self.log("longshot_log.5.txt", "5" * 32, ["[hos host] coop: host: player 2 joined: Impa"])
        b = self.log("longshot_log.6.txt", "6" * 32, ["[hos host] coop: link: lost Impa"])
        rep = pr.new_report([a, b], pr.read_report_state(self.kit.report_state), self.kit.logs, "", USER, PC)
        self.assertNotIn("Impa", rep[2])

    def make(self, name, files):
        p = self.kit.send_dir / name
        pr.write_zip(files, p)
        return p

    def test_gate_zip(self):
        head = "Squadala! problem report: 1\r\nKit: x\r\n\r\n"
        cases = {"ip": head + "1x lost 203.0.113.9", "user": head + "1x /home/testuser/x", "pc": head + "1x on testpc-01",
                 "host": head + "1x lost myhost.duckdns.org", "code": head + "1x invite code: ABCDE-FGHJK-LMNPQ"}
        for k, v in cases.items():
            self.assertNotEqual(pr.check_zip(self.make(f"{k}.zip", {"problem-report.txt": v}), USER, PC), "", k)
        self.assertTrue(pr.check_zip(self.make("n.zip", {"problem-report.txt": head, "game.log": "x"}), USER, PC).startswith("file name"))
        self.assertEqual(pr.check_zip(self.make("ok.zip", {"problem-report.txt": head + "1x lost <public-1>:27510"}), USER, PC), "")

    def test_queue(self):
        s = pr.set_choice(self.kit, True)
        ok = {"problem-report.txt": "Squadala! problem report: 1\r\nKit: x\r\n\r\n1x coop: lost <public-1>\r\n"}
        a = self.make("20261121-000000-" + "a" * 32 + ".zip", ok)
        self.make("20261121-000001-" + "b" * 32 + ".zip", ok)
        calls = []
        retry = lambda z, u, i, r: (calls.append((i, r)), ("retry", "no answer"))[1]
        res = pr.send_queue(self.kit, "https://x.org/v1/problem-report", retry, USER, PC)
        self.assertEqual(res, [(a.name, "retry")])
        self.assertEqual(calls, [(s["install"], "a" * 32)])
        self.assertIn("not sent yet", (self.kit.send_dir / "result.txt").read_text())
        res = pr.send_queue(self.kit, "https://x.org/v1/problem-report", lambda *a: ("sent", "report #7"), USER, PC)
        self.assertEqual([r for _, r in res], ["sent", "sent"])
        self.assertEqual(len(list((self.kit.send_dir / "sent").glob("*.zip"))), 2)
        self.assertEqual(list(self.kit.send_dir.glob("*.tries")), [])
        self.assertIn("(report #7)", (self.kit.send_dir / "result.txt").read_text())
        self.assertEqual(pr.send_queue(self.kit, "https://x.org/p", lambda *a: self.fail("twice"), USER, PC), [])
        self.make("20261121-000002-" + "c" * 32 + ".zip", {"problem-report.txt": "Squadala!\r\n1x 203.0.113.9\r\n"})
        res = pr.send_queue(self.kit, "https://x.org/p", lambda *a: self.fail("gate"), USER, PC)
        self.assertEqual(res[0][1], "held")
        self.make("20261121-000003-" + "d" * 32 + ".zip", ok)
        self.assertEqual(pr.send_queue(self.kit, "https://x.org/p", lambda *a: ("refused", "answer 422"), USER, PC)[0][1], "refused")
        self.assertEqual(len(list((self.kit.send_dir / "held").glob("*.zip"))), 2)
        self.make("20261121-000004-" + "e" * 32 + ".zip", ok)
        old = pr.SEND_MAX_TRIES
        pr.SEND_MAX_TRIES = 2
        try:
            pr.send_queue(self.kit, "https://x.org/p", retry, USER, PC)
            self.assertEqual(pr.send_queue(self.kit, "https://x.org/p", retry, USER, PC)[0][1], "gave up")
        finally:
            pr.SEND_MAX_TRIES = old
        self.make("20261121-000005-" + "f" * 32 + ".zip", ok)
        pr.set_choice(self.kit, False)
        self.assertEqual(pr.send_queue(self.kit, "https://x.org/p", lambda *a: self.fail("off"), USER, PC), [])
        pr.set_choice(self.kit, True)
        self.assertEqual(pr.send_queue(self.kit, "", lambda *a: self.fail("no url"), USER, PC)[0][1], "retry")

    def test_real_upload(self):
        got = {}

        class H(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                got["path"] = self.path
                got["headers"] = dict(self.headers)
                got["body"] = self.rfile.read(int(self.headers["Content-Length"]))
                body = json.dumps({"ok": True, "report": 12}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass

        srv = http.server.HTTPServer(("127.0.0.1", 0), H)
        t = threading.Thread(target=srv.handle_request, daemon=True)
        t.start()
        z = self.make("x.zip", {"problem-report.txt": "Squadala!\r\n"})
        url = f"http://127.0.0.1:{srv.server_address[1]}/v1/problem-report"
        self.assertEqual(pr.upload(z, url, "1" * 32, "2" * 32, 5), ("sent", "report #12"))
        t.join(5)
        srv.server_close()
        self.assertEqual(got["path"], "/v1/problem-report")
        self.assertEqual(got["headers"]["Content-Type"], "application/zip")
        self.assertEqual(got["headers"]["X-Squadala-Install"], "1" * 32)
        self.assertEqual(got["headers"]["X-Squadala-Report"], "2" * 32)
        self.assertTrue(got["body"].startswith(b"PK\x03\x04"))
        self.assertEqual(pr.upload(z, url, "1" * 32, "2" * 32, 2)[0], "retry")   # nobody listening now


if __name__ == "__main__":
    unittest.main()
