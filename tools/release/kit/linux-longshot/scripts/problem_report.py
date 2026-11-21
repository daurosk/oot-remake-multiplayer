#!/usr/bin/env python3
"""Problem reports for the Linux kit (docs/problem-reports.md): the twin of the Windows kit's problem-report.ps1 and
problem-send.ps1, in one file. Standard library only.

The co-op module's problems from the finished game sessions (its warnings and errors, lines saying something failed,
was refused, gave up, timed out or was lost, faults the guard caught, crashes), masked as collect-logs masks its zip
(addresses, MAC addresses, the user and PC names, invite codes, passwords) and players' names, each problem in one
report only. With sending on (asked at a start, No by default), the report of a game that had a problem or a crash
(and Longshot's new crash records, masked the same way) goes to the Squadala! report bot after the game closes, never
to Longshot. A quiet session sends nothing.

  problem_report.py at-start      start.sh: asks once (No in 20 s), then the local report or the queue (sent in the
                                  background)
  problem_report.py after-game-bg start.sh, once the game closed (a crash too): after-game in the background
  problem_report.py after-game    the report of those sessions, queued and sent
  problem_report.py send-pending  send what is queued
  problem_report.py now           a report at once, whatever the answer
  problem_report.py on|off|send|nosend   the answers (start-squadala.sh --problem-reports ...): off turns sending off too

Files (data/logs): problem-reports.json (local reports' answer, sessions read, problems reported), problem-send.json
(sending's answer, a random install id, crash records sent), coop-report-<date>.zip (the newest 10), problem-send/
(the queue; sent/ and held/ keep copies; result.txt the last result).
"""

import hashlib
import io
import ipaddress
import json
import os
import re
import secrets
import select
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

try:
    import fcntl
except ImportError:  # (not Linux: the unit tests on Windows; nothing runs at the same time there)
    fcntl = None

# The module's problems, as problem-report.ps1's (case-sensitive; the (?i:...) parts are not)
PROBLEM_LINE = re.compile(
    r"(?i:warning|error): coop|coop: .*\b(?:fail\w*|could not|couldn't|refus\w*|not taken|gave up|"
    r"giving up|stuck|lost|timed out|not written|not sent|damaged|mismatch\w*|wrong)\b|coop: guard: a fault|"
    r"\bCRASH\b|\bcrash record\b|\b(?i:fatal error|unhandled exception)\b")
PROBLEM_ROUTINE = re.compile(
    r"coop: pose: |coop: actors: in \d+ s|invite code is made|camera lost, markers off|"
    r"coop: flags: (by the game's events|native)|so far; |coop: objects: list head|/s.*/s", re.I)
NAME_AT = re.compile(r"player \d+ (?:joined|left): (?P<a>[^(]+?)(?= \(|$)|player \d+ '(?P<b>[^']+)'|"
                     r"Mods: player \d+ \((?P<c>[^)]+)\)|name '(?P<d>[^']+)'")
FILE_EXT = re.compile(r"^(?i:exe|dll|txt|log|ini|json|zip|cpp|hpp|h|ps1|cmd|bat|pak|sav|bin|png|dds|cfg|toml|md|nup|pdb|so|sh|py)$")
ZIPS_KEPT = 10
SEND_ASK_SECONDS = 20
SEND_TIMEOUT = 15
SEND_MAX_TRIES = 30
SEND_MAX_DAYS = 30
SEND_KEPT = 10
SEND_NAMES = re.compile(r"^(?:problem-report\.txt|crash-record-[1-3]\.txt)$")
MAX_CRASH_BYTES = 64 * 1024
COMMON_USERS = {"admin", "administrator", "user", "users", "owner", "player", "guest", "link", "zelda", "navi",
                "default", "public", "deck"}


# --- masking (log-mask.ps1's Protect-LogText) ------------------------------------------------------------------------

def mask_kind4(address):
    m = re.fullmatch(r"(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})", address)
    if not m:
        return ""
    o = [int(x) for x in m.groups()]
    if any(x > 255 for x in o) or address in ("0.0.0.0", "255.255.255.255"):
        return ""
    if o[0] == 127:
        return "loopback"
    if o[0] == 10 or (o[0] == 172 and 16 <= o[1] <= 31) or (o[0] == 192 and o[1] == 168) or (o[0] == 169 and o[1] == 254):
        return "lan"
    if o[0] == 100 and 64 <= o[1] <= 127:
        return "tailscale"
    if o[0] == 25:
        return "hamachi"
    if o[0] == 26:
        return "radmin"
    return "public"


def _ip6(address):
    a = re.sub(r"%.*$", "", address)
    if ":" not in a:
        return None
    try:
        ip = ipaddress.IPv6Address(a)
    except ValueError:
        return None
    return ip


def mask_kind6(address):
    ip = _ip6(address)
    if ip is None or int(ip) == 0:
        return ""
    b = ip.packed
    if ip.is_loopback:
        return "loopback"
    if b[0] == 0xFE and (b[1] & 0xC0) == 0x80:
        return "lan"
    if b[:6] == bytes([0xFD, 0x7A, 0x11, 0x5C, 0xA1, 0xE0]):
        return "tailscale"
    if (b[0] & 0xFE) == 0xFC:
        return "lan"
    if b[:4] == bytes([0x26, 0x20, 0x00, 0x9B]) and not any(b[4:12]):
        return "hamachi"
    return "public"


class Mask:
    """One per zip: the same address gets the same tag in every file."""

    def __init__(self):
        self.tags = {}
        self.counts = {}

    def tag(self, kind, key):
        key = key.lower()
        if key not in self.tags:
            self.counts[kind] = self.counts.get(kind, 0) + 1
            self.tags[key] = f"<{kind}-{self.counts[kind]}>"
        return self.tags[key]


RX_V6 = re.compile(r"(?<![\w:.])(?:[0-9A-Fa-f]{0,4}:){2,7}(?:\d{1,3}(?:\.\d{1,3}){3}|[0-9A-Fa-f]{0,4})(?:%[\w.-]+)?(?![\w:]|\.\d)")
RX_V4 = re.compile(r"(?<![\d.vV])\d{1,3}(?:\.\d{1,3}){3}(?![\d]|\.\d)")


def protect(text, mask, user, pc):
    if not text:
        return text
    t = re.sub(r"(?im)^([ \t]*[\w.]*(?:password|secret|token)\w*[ \t]*[=:])[ \t]*[^\s][^\r\n]*", r"\1 (removed)", text)
    t = re.sub(r"(?i)(?<![\w-])(--?[\w-]*(?:password|secret|token)[\w-]*)(?:[ \t]+|=)(?!-)\S+", r"\1 (removed)", t)
    t = re.sub(r"(?i)(invite code:?)[ \t]*[A-Z2-9][A-Z2-9-]{9,}", r"\1 (removed)", t)
    t = re.sub(r"(?<![0-9A-Fa-f:-])(?:[0-9A-Fa-f]{2}([:-]))(?:[0-9A-Fa-f]{2}\1){4}[0-9A-Fa-f]{2}(?![0-9A-Fa-f:-])", "<mac>", t)
    t = re.sub(r"(?<![\w.])[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}\.[0-9A-Fa-f]{4}(?![\w.])", "<mac>", t)

    def v6(m):
        kind = mask_kind6(m.group(0))
        if not kind:
            return m.group(0)
        ip = _ip6(m.group(0))
        if ip.ipv4_mapped is not None:
            k4 = mask_kind4(str(ip.ipv4_mapped))
            if k4:
                return mask.tag(k4, str(ip.ipv4_mapped))
        return mask.tag(kind, ip.compressed)

    t = RX_V6.sub(v6, t)
    t = RX_V4.sub(lambda m: mask.tag(mask_kind4(m.group(0)), m.group(0)) if mask_kind4(m.group(0)) else m.group(0), t)
    t = re.sub(r"(?im)^([ \t]*server[ \t]*=[ \t]*)(?!localhost\b)([A-Za-z][\w.-]*)", r"\1<host>", t)
    t = re.sub(r"(?i)([A-Z]:[\\/]+Users[\\/]+)[^\\/\r\n\"'<>|:*?]+", r"\1<user>", t)
    if user:
        t = re.sub(r"(/home/)" + re.escape(user) + r"(?=/|$|[^\w])", r"\1<user>", t)
    for name, rep, least in ((user, "<user>", 5), (pc, "<pc>", 3)):
        if name and len(name) >= least and name.lower() not in COMMON_USERS:
            t = re.sub(r"(?i)(?<![\w])" + re.escape(name) + r"(?![\w])", rep, t)
    return t


def has_address(masked):
    """problem-report.ps1's last gate: anything still an address, a host name or a host with a port."""
    for m in re.finditer(r"(?<![\d.])\d{1,3}(?:\.\d{1,3}){3}(?![\d])", masked):
        if mask_kind4(m.group(0)):
            return True
    for m in re.finditer(r"(?<![\w:.])(?:[0-9A-Fa-f]{0,4}:){2,7}(?:\d{1,3}(?:\.\d{1,3}){3}|[0-9A-Fa-f]{0,4})(?:%[\w.-]+)?", masked):
        if mask_kind6(m.group(0)):
            return True
    if re.search(r"(?<![\w.<>-])[A-Za-z0-9][\w-]*(?:\.[\w-]+)*:\d{2,5}(?!\d)", masked):
        return True
    for m in re.finditer(r"(?<![\w.\\/-])(?:[A-Za-z0-9-]+\.)+(?P<tld>[A-Za-z]{2,63})(?![\w-]|\.\w)", masked):
        if not FILE_EXT.match(m.group("tld")):
            return True
    return False


def protect_names(text, names):
    for m in NAME_AT.finditer(text):
        n = next(g for g in m.groups() if g is not None).strip()
        if len(n) >= 2 and not re.fullmatch(r"<[a-z]+(-\d+)?>", n):
            names.add(n)
    for n in sorted(names, key=len, reverse=True):
        text = re.sub(r"(?<!\w)" + re.escape(n) + r"(?!\w)", "<name>", text)
    return text


def fingerprint(masked):
    s = re.sub(r"^\[[^\]]*\]\s*", "", masked)
    s = re.sub(r"<([a-z]+)-\d+>", r"<\1>", s)
    s = re.sub(r"0x[0-9a-fA-F]+|\b[0-9a-f]{8,}\b", "#", s)
    s = re.sub(r"\d+(\.\d+)?", "#", s)
    return hashlib.sha1(s.encode("utf-8")).hexdigest()[:16]


def is_problem(line):
    return bool(PROBLEM_LINE.search(line)) and not PROBLEM_ROUTINE.search(line)


# --- state ------------------------------------------------------------------------------------------------------------

def _read_json(path):
    try:
        j = json.loads(Path(path).read_text(encoding="utf-8-sig"))
        return j if isinstance(j, dict) else {}
    except (OSError, ValueError):
        return {}


def _write_json(path, obj):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".new")
    tmp.write_text(json.dumps(obj, indent=2), encoding="utf-8")
    os.replace(tmp, path)


def read_report_state(path):
    j = _read_json(path)
    sent = j.get("sent") if isinstance(j.get("sent"), dict) else {}
    return {"choice": j.get("choice") if j.get("choice") in ("on", "off") else "",
            "runs": [str(r) for r in j.get("runs") or []], "sent": {str(k): str(v) for k, v in sent.items()}}


def write_report_state(path, state):
    sent = dict(list(state["sent"].items())[-5000:])
    _write_json(path, {"choice": state["choice"], "runs": state["runs"][-200:], "sent": sent})


def read_send_state(path):
    j = _read_json(path)
    install = str(j.get("install") or "")
    return {"send": j.get("send") if j.get("send") in ("on", "off") else "",
            "install": install if re.fullmatch(r"[0-9a-f]{32}", install) else "",
            "asked": str(j.get("asked") or ""), "crashes": [str(c) for c in j.get("crashes") or []]}


def write_send_state(path, state):
    _write_json(path, {"send": state["send"], "install": state["install"], "asked": state["asked"],
                       "crashes": state["crashes"][-200:]})


class Lock:
    """problem-send.ps1's two locks: 'reports' while a report is made, 'queue' while the queue is sent."""

    def __init__(self, logs, which, wait):
        self.path = Path(logs) / f".problem-{which}.lock"
        self.wait = wait
        self.f = None

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.f = open(self.path, "w")
        end = time.monotonic() + self.wait
        while True:
            try:
                if fcntl is not None:
                    fcntl.flock(self.f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return self
            except OSError:
                if time.monotonic() >= end:
                    self.f.close()
                    self.f = None
                    return None
                time.sleep(0.2)

    def __exit__(self, *a):
        if self.f:
            self.f.close()


# --- reading sessions -------------------------------------------------------------------------------------------------

def run_key(path):
    with open(path, "rb") as f:
        head = f.read(65536)
        size = os.fstat(f.fileno()).st_size
    m = re.search(rb"run_id: ([0-9a-f]{16,})", head)
    if m:
        return m.group(1).decode()
    return f"file-{size}-{hashlib.sha1(head).hexdigest()[:20]}"


def add_names(line, names):
    """The players' names a line gives where the module writes a name, added to names."""
    if "player " in line or "name '" in line:
        protect_names(line, names)


def log_names(logs, names):
    """The players' names in whole logs (the name lines only): for a crash record after a session read before."""
    for log in logs:
        try:
            with open(log, encoding="utf-8", errors="replace") as f:
                for line in f:
                    add_names(line.rstrip("\r\n"), names)
        except OSError:
            pass


def read_run(path, mask, user, pc, known, names=None):
    """One session: (key, events), events None for a known session. Players' names are masked once the whole session
    is read, with every name it gave (a problem line before the player's join line too) and those already in names
    (other sessions read now); the session's names are added to names."""
    names = set() if names is None else names
    key = run_key(path)
    if key in known:
        return key, None
    lines = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\r\n")
            add_names(line, names)
            if not is_problem(line):
                continue
            t = line.strip()
            if t in lines:
                lines[t] += 1
            elif len(lines) < 20000:
                lines[t] = 1
    events = {}
    for t, count in lines.items():
        masked = protect_names(protect(t, mask, user, pc), names)
        if has_address(masked):
            continue
        fp = fingerprint(masked)
        if fp in events:
            events[fp]["count"] += count
        else:
            events[fp] = {"line": masked[:300], "count": count}
    return key, events


def write_zip(files, out):
    """files: {name: text}; a standard zip, '/' names, no folders. A half-written zip is removed."""
    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".part")
    try:
        with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
            for name in sorted(files):
                z.writestr(name, files[name].encode("utf-8"))
        os.replace(tmp, out)
    finally:
        if tmp.exists():
            tmp.unlink()
    return out


def new_report(logs, state, out_dir, version="", user="", pc="", names=None):
    """The new problems of the logs not read before: (zip path, count, text) or None. state is brought up to date;
    names gets the players' names met (the crash records are masked with them)."""
    names = set() if names is None else names
    mask = Mask()
    new = {}
    read = []
    for log in logs:
        if not os.path.isfile(log):
            continue
        try:
            key, events = read_run(log, mask, user, pc, set(state["runs"]) | set(read), names)
        except OSError:
            continue
        if events is None:
            continue
        read.append(key)
        for fp, e in events.items():
            if fp in state["sent"]:
                continue
            if fp in new:
                new[fp]["count"] += e["count"]
            else:
                new[fp] = e
    state["runs"] = state["runs"] + read
    if not new:
        return None
    lines = [f"Squadala! problem report: {len(new)} new problem(s) from {len(read)} session(s)",
             f"Kit: {protect(version, mask, user, pc)}",
             "Addresses, names, codes and passwords are masked (as collect-logs masks them). Send with /bugreport.", ""]
    for e in sorted(new.values(), key=lambda e: -e["count"]):
        lines.append(f"{e['count']}x {e['line']}")
    text = "\r\n".join(lines) + "\r\n"
    zip_path = Path(out_dir) / f"coop-report-{time.strftime('%Y%m%d-%H%M%S')}.zip"
    write_zip({"problem-report.txt": text}, zip_path)
    for old in sorted(Path(out_dir).glob("coop-report-*.zip"), reverse=True)[ZIPS_KEPT:]:
        try:
            old.unlink()
        except OSError:
            pass
    today = time.strftime("%Y-%m-%d")
    for fp in new:
        state["sent"][fp] = today
    return zip_path, len(new), text


# --- sending ----------------------------------------------------------------------------------------------------------

class Kit:
    def __init__(self, kit):
        self.kit = Path(kit)
        self.data = self.kit / "data"
        self.logs = self.data / "logs"
        self.game = self.data / "game"
        self.send_dir = self.logs / "problem-send"
        self.report_state = self.logs / "problem-reports.json"
        self.send_state = self.logs / "problem-send.json"

    def version(self):
        try:
            return (self.kit / "VERSION.txt").read_text(encoding="utf-8").splitlines()[0]
        except (OSError, IndexError):
            return ""

    def url(self):
        u = os.environ.get("OOT_COOP_PROBLEM_SEND_URL", "").strip()
        if not u:
            try:
                lines = (self.kit / "scripts" / "problem-send-url.txt").read_text(encoding="utf-8").splitlines()[:20]
                u = next((l.strip() for l in lines if l.strip() and not l.strip().startswith("#")), "")
            except OSError:
                u = ""
        if re.fullmatch(r"https://[A-Za-z0-9.-]+(:\d+)?/[\w./-]*", u):
            return u
        if re.fullmatch(r"http://(127\.0\.0\.1|localhost)(:\d+)?/[\w./-]*", u):
            return u
        return ""

    def game_dirs(self):
        dirs = [self.game]
        for p in Path("/proc").glob("[0-9]*"):
            try:
                exe = os.readlink(p / "exe")
            except OSError:
                continue
            if os.path.basename(exe) == "longshot":
                dirs.append(Path(exe).parent)
        out = []
        for d in dirs:
            if d not in out:
                out.append(d)
        return out

    def logs_to_read(self, running=None):
        """The finished sessions' logs (the running game's own longshot_log.txt is left for later), oldest first."""
        files = []
        for d in self.game_dirs():
            files += list(d.glob("longshot_log*.txt")) + list((d / "logs").glob("longshot_log*.txt"))
        files += list(self.logs.glob("longshot_log*.txt"))
        if running is None:
            running = len(self.game_dirs()) > 1 or game_running()
        files = [f for f in files if not (running and f.name == "longshot_log.txt")]
        seen = []
        for f in sorted(files, key=lambda f: f.stat().st_mtime):
            if str(f) not in seen:
                seen.append(str(f))
        return seen


def game_running():
    for p in Path("/proc").glob("[0-9]*"):
        try:
            if os.path.basename(os.readlink(p / "exe")) == "longshot":
                return True
        except OSError:
            pass
    return False


def set_choice(kit, on):
    s = read_send_state(kit.send_state)
    s["send"] = "on" if on else "off"
    s["asked"] = time.strftime("%Y-%m-%d")
    if on and not s["install"]:
        s["install"] = secrets.token_hex(16)
    write_send_state(kit.send_state, s)
    r = read_report_state(kit.report_state)
    choice = "on" if on else (r["choice"] or "off")
    if r["choice"] != choice:
        r["choice"] = choice
        write_report_state(kit.report_state, r)
    return s


def crash_records(game_dirs, known):
    found = []
    for d in game_dirs:
        p = Path(d) / "bug-reports" / "pending"
        if p.is_dir():
            found += [f for f in p.glob("crash-*.txt") if re.match(r"^crash-\d{8}T\d{6}Z", f.name) and f.name not in known]
    return sorted(found, key=lambda f: f.name)[-3:]


def crash_text(path, mask, user, pc, names=None):
    """A crash record masked as a problem line is: addresses and the rest, players' names (names, and any the record
    itself gives) as <name>, a line the address gate stops left out; 64 KB at most (the first 4000 lines)."""
    names = set() if names is None else names
    raw = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if len(raw) >= 4000:
                break
            raw.append(line.rstrip("\r\n"))
            add_names(raw[-1], names)
    out = []
    size = 0
    for line in raw:
        m = protect_names(protect(line, mask, user, pc), names)
        if has_address(m):
            m = "(a line with an address was left out)"
        if size + len(m) + 2 > MAX_CRASH_BYTES:
            out.append("(cut)")
            break
        out.append(m)
        size += len(m) + 2
    return "\r\n".join(out) + "\r\n"


def add_report(kit, version="", logs=None, game_dirs=None, user="", pc="", wait=60):
    """The report of the sessions since the last one (and the new crash records), queued as
    problem-send/<date>-<id>.zip; the local coop-report zip is written as before. The queued path or None."""
    with Lock(kit.logs, "reports", wait) as lock:
        if lock is None:
            return None
        logs = kit.logs_to_read() if logs is None else logs
        game_dirs = kit.game_dirs() if game_dirs is None else game_dirs
        rs = read_report_state(kit.report_state)
        ss = read_send_state(kit.send_state)
        names = set()
        rep = new_report(logs, rs, kit.logs, version, user, pc, names)
        crashes = crash_records(game_dirs, ss["crashes"])
        if crashes:   # (masked with every player's name in the logs, also those of sessions read before)
            log_names(logs, names)
        write_report_state(kit.report_state, rs)
        if rep is None and not crashes:
            return None
        mask = Mask()
        if rep:
            files = {"problem-report.txt": rep[2]}
        else:
            files = {"problem-report.txt": "\r\n".join([
                f"Squadala! problem report: 0 new problem(s) from 0 session(s), {len(crashes)} crash record(s)",
                f"Kit: {protect(version, mask, user, pc)}",
                "Addresses, names, codes and passwords are masked (as collect-logs masks them). Sent by the kit.", ""]) + "\r\n"}
        for n, c in enumerate(crashes, 1):
            name = re.sub(r"-\d+-\d+(?=\.txt$)", "", c.name)
            files[f"crash-record-{n}.txt"] = name + "\r\n" + crash_text(c, mask, user, pc, names)
        out = kit.send_dir / f"{time.strftime('%Y%m%d-%H%M%S')}-{secrets.token_hex(16)}.zip"
        write_zip(files, out)
        if crashes:
            ss["crashes"] = ss["crashes"] + [c.name for c in crashes]
            write_send_state(kit.send_state, ss)
            # (which crash records this zip holds: once the bot took it, sent_crashes tells the game)
            out.with_name(out.name + ".crashes").write_text("".join(f"{c}\n" for c in crashes), encoding="utf-8")
        return out


def sent_crashes(zip_path):
    """The crash records a sent zip held, added to their game's bug-reports/coop-sent.txt: the game's title then does
    not ask the player to send that crash with /bugreport (hookshot/src/coop_problem_ask.h: we have it already)."""
    try:
        paths = [Path(l) for l in Path(str(zip_path) + ".crashes").read_text(encoding="utf-8").splitlines() if l]
    except OSError:
        return
    for p in paths:
        f = p.parent.parent / "coop-sent.txt"
        try:
            have = [l for l in f.read_text(encoding="utf-8").splitlines() if l] if f.exists() else []
            if p.name not in have:
                f.write_text("\n".join((have + [p.name])[-200:]) + "\n", encoding="utf-8")
        except OSError:
            pass


def check_zip(path, user="", pc=""):
    """The gate before an upload: '' when it may go, else why not (problem-send.ps1's Test-ProblemSendZip)."""
    try:
        size = os.path.getsize(path)
    except OSError:
        return "size"
    if size <= 0 or size > 200 * 1024:
        return "size"
    try:
        z = zipfile.ZipFile(path)
    except (zipfile.BadZipFile, OSError):
        return "not a zip"
    with z:
        infos = z.infolist()
        if not 1 <= len(infos) <= 4:
            return "files"
        if "problem-report.txt" not in [i.filename for i in infos]:
            return "no problem-report.txt"
        for i in infos:
            if not SEND_NAMES.match(i.filename):
                return f"file name {i.filename}"
            if i.file_size > 120 * 1024:
                return f"size of {i.filename}"
            try:
                text = z.read(i).decode("utf-8")
            except (UnicodeDecodeError, zipfile.BadZipFile, OSError):
                return f"not UTF-8: {i.filename}"
            for line in re.split(r"\r?\n", text):
                if not line:
                    continue
                if protect(line, Mask(), user, pc) != line:
                    return f"unmasked text in {i.filename}"
                if has_address(line):
                    return f"an address in {i.filename}"
    return ""


def upload(path, url, install, report_id, timeout=SEND_TIMEOUT):
    """One upload: ('sent'|'duplicate'|'retry'|'refused', why)."""
    data = Path(path).read_bytes()
    req = urllib.request.Request(url, data=data, method="POST", headers={
        "Content-Type": "application/zip", "X-Squadala-Install": install, "X-Squadala-Report": report_id,
        "User-Agent": "Squadala-kit"})
    ctx = ssl.create_default_context()
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *a, **k):
            return None

    opener = urllib.request.build_opener(urllib.request.HTTPSHandler(context=ctx), NoRedirect)
    try:
        with opener.open(req, timeout=timeout) as resp:
            code = resp.status
            body = resp.read(4096).decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        code, body = e.code, ""
    except (urllib.error.URLError, OSError, ValueError):
        return "retry", "no answer"
    if code == 200:
        try:
            j = json.loads(body)
        except ValueError:
            j = {}
        if j.get("duplicate"):
            return "duplicate", ""
        n = j.get("report")
        return "sent", f"report #{n}" if isinstance(n, int) else ""
    if code in (400, 404, 405, 411, 413, 415, 422):
        return "refused", f"answer {code}"
    return "retry", f"answer {code}"


def write_result(kit, text):
    kit.send_dir.mkdir(parents=True, exist_ok=True)
    (kit.send_dir / "result.txt").write_text(f"{time.strftime('%Y-%m-%d %H:%M')} {text}\n", encoding="utf-8")


def send_queue(kit, url=None, send=upload, user="", pc=""):
    url = kit.url() if url is None else url
    results = []
    with Lock(kit.logs, "queue", 5) as lock:
        if lock is None:
            return results
        state = read_send_state(kit.send_state)
        if state["send"] != "on" or not state["install"]:
            return results
        for z in sorted(kit.send_dir.glob("*.zip")) if kit.send_dir.is_dir() else []:
            rid = z.stem.rsplit("-", 1)[-1]
            if not re.fullmatch(r"[0-9a-f]{32}", rid):
                continue
            move = ""
            why = check_zip(z, user, pc)
            if why:
                result, why, move = "held", f"stopped before sending ({why})", "held"
            elif not url:
                result, why = "retry", "no address to send to in this kit yet"
            else:
                result, why = send(z, url, state["install"], rid)
                move = "sent" if result in ("sent", "duplicate") else "held" if result == "refused" else ""
            tries_file = z.with_name(z.name + ".tries")
            if not move:
                try:
                    t = 1 + int(tries_file.read_text().strip())
                except (OSError, ValueError):
                    t = 1
                tries_file.write_text(str(t))
                if t >= SEND_MAX_TRIES or z.stat().st_mtime < time.time() - SEND_MAX_DAYS * 86400:
                    result, why, move = "gave up", f"not sent after {t} tries", "held"
            if move == "sent":
                sent_crashes(z)
            if move:
                try:
                    z.with_name(z.name + ".crashes").unlink()
                except OSError:
                    pass
                to = kit.send_dir / move
                to.mkdir(parents=True, exist_ok=True)
                os.replace(z, to / z.name)
                try:
                    tries_file.unlink()
                except OSError:
                    pass
                for old in sorted(to.glob("*.zip"), reverse=True)[SEND_KEPT:]:
                    old.unlink()
            text = {
                "sent": f"Problem report sent to the Squadala! team{f' ({why})' if why else ''}: {z.name}",
                "duplicate": f"Problem report sent to the Squadala! team (it had it already): {z.name}",
                "retry": f"Problem report not sent yet ({why}); tried again after your next game or start: {z.name}",
                "refused": f"Problem report not taken by the bot ({why}); kept in data/logs/problem-send/held: {z.name}",
            }.get(result, f"Problem report not sent, {why}; kept in data/logs/problem-send/held: {z.name}")
            write_result(kit, text)
            results.append((z.name, result))
            if result == "retry":
                break
    return results


def ask(url, seconds=SEND_ASK_SECONDS, read=None, out=print):
    """The question: y is yes; n, Enter or nothing in time is no; no terminal is no. read: tests (returns a line or None)."""
    q = "Send problem reports to the Squadala! team after your games? [y/N]"
    host = re.sub(r"^https?://([^/:]+).*$", r"\1", url) if url else ""
    out("")
    out("Problem reports: when a game you played had a co-op problem or a crash, the kit can send the team a small")
    out("  report after the game closes: the problem lines from the game's log and the crash record, masked first")
    out("  (IP and MAC addresses, your user and PC names, players' names, invite codes and passwords are taken")
    out("  out; lines that may still hold an address are left out). Nothing is sent after a game without problems.")
    out(f"  It goes to the Squadala! report bot{f' ({host})' if host else ''}, never to Longshot; a copy stays in data/logs/problem-send.")
    out("  Turn it off at any time: bash start-squadala.sh --problem-reports off")
    if read is None:
        if not (sys.stdin.isatty() and sys.stdout.isatty()):
            out(f"{q} n (no terminal: no)")
            return False
        sys.stdout.write(f"{q} (no in {seconds} s) ")
        sys.stdout.flush()
        ready, _, _ = select.select([sys.stdin], [], [], seconds)
        line = sys.stdin.readline() if ready else None
    else:
        line = read(seconds)
    yes = bool(line) and line.strip().lower() in ("y", "yes")
    out("y" if yes else "n")
    return yes


def spawn(kit_path, command):
    """This script in the background, on its own (never waited for; it outlives start.sh)."""
    try:
        subprocess.Popen([sys.executable, "-I", "-B", os.path.abspath(__file__), "--kit", str(kit_path), command],
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         start_new_session=True, close_fds=True)
    except OSError:
        pass


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    kit_path = os.environ.get("KIT") or str(Path(__file__).resolve().parent.parent)
    if len(argv) >= 2 and argv[0] == "--kit":
        kit_path = argv[1]
        argv = argv[2:]
    cmd = argv[0] if argv else "now"
    kit = Kit(kit_path)
    user = os.environ.get("USER") or ""
    try:
        import pwd
        user = pwd.getpwuid(os.getuid()).pw_name or user
    except (ImportError, KeyError):
        pass
    pc = os.uname().nodename if hasattr(os, "uname") else ""
    version = kit.version()
    ss = read_send_state(kit.send_state)

    if cmd in ("send", "nosend"):
        set_choice(kit, cmd == "send")
        print("Sending is on: after a game with a co-op problem or a crash, its masked report goes to the Squadala! team."
              if cmd == "send" else "Sending is off: reports stay on this PC.")
        return 0
    if cmd in ("on", "off"):
        r = read_report_state(kit.report_state)
        r["choice"] = cmd
        write_report_state(kit.report_state, r)
        print("Problem reports are on: each start makes one when your games had a new problem." if cmd == "on"
              else "Problem reports are off.")
        if cmd == "off" and ss["send"] == "on":
            set_choice(kit, False)
            print("Sending problem reports is off too.")
        return 0
    if cmd == "after-game-bg":
        if ss["send"] == "on":
            spawn(kit_path, "after-game")
        return 0
    if cmd == "after-game":
        time.sleep(float(os.environ.get("OOT_COOP_AFTER_GAME_WAIT", "20")))   # Longshot's crash watcher writes its record
        if ss["send"] == "on":
            try:
                add_report(kit, version, user=user, pc=pc)
            except OSError:
                pass
            send_queue(kit, user=user, pc=pc)
        return 0
    if cmd == "send-pending":
        send_queue(kit, user=user, pc=pc)
        return 0
    if cmd == "at-start":
        url = kit.url()
        if not ss["send"] and url:
            yes = ask(url)
            ss = set_choice(kit, yes)
            print("Sending is on. bash start-squadala.sh --problem-reports off turns it off." if yes
                  else "Not sent. bash start-squadala.sh --problem-reports send turns sending on later.")
        if ss["send"] == "on":
            last = kit.send_dir / "result.txt"
            try:
                line = last.read_text(encoding="utf-8").splitlines()[0]
                print(f"Last: {line}")
            except (OSError, IndexError):
                pass
            add_report(kit, version, user=user, pc=pc, wait=5)
            if kit.send_dir.is_dir() and any(kit.send_dir.glob("*.zip")):
                spawn(kit_path, "send-pending")
            return 0
        if read_report_state(kit.report_state)["choice"] != "on":
            return 0
    # at-start with local reports on, or now: the local report
    with Lock(kit.logs, "reports", 30):
        rs = read_report_state(kit.report_state)
        rep = new_report(kit.logs_to_read(), rs, kit.logs, version, user, pc)
        write_report_state(kit.report_state, rs)
    if rep:
        print(f"Problem report: {rep[1]} new problem(s) in {rep[0]}")
        print("Send it with /bugreport in the Squadala! Discord (addresses, names and codes are masked in it).")
    elif cmd == "now":
        print("Problem report: no new problem in your game logs since the last report.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(1)
