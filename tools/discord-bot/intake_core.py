"""Problem-report intake: the checks behind the bot's small HTTPS upload endpoint.

The kit sends a masked problem report (docs/problem-reports.md, "Sending") after a game session that had a
problem, when the player said yes at the start. This file decides whether one upload is taken: the request's
headers, the size, the per-install and overall rate, the zip itself (only the kit's own file names, small, no
zip bomb: every entry is really read back with a cap) and the text in it. What it takes is saved and filed like a
/bugreport (report_core.save_report's layout) as a 'NEW (auto)' row.

Standard library only, so the unit tests run without discord.py, aiohttp or a network.

Everything in an upload is untrusted: it is masked by the kit, but anyone can send anything to a public address.
It is stored and shown under report_core.UNTRUSTED_LABEL, quoted, exactly as /bugreport's player text.
"""

import datetime as _dt
import io
import json
import os
import re
import shutil
import time
import zipfile
from pathlib import Path

import report_core as core

PATH = "/v1/problem-report"
MAX_UPLOAD_BYTES = 256 * 1024          # a problem report is a few KB; a crash record or three at most
MAX_ENTRY_BYTES = 128 * 1024           # each file, read back (not the header's word for it)
MAX_TOTAL_BYTES = 384 * 1024           # all files together, read back
MAX_RATIO = 60                         # uncompressed / compressed per entry (text compresses ~5-15x)
MAX_ENTRIES = 4
INSTALL_LIMIT, INSTALL_WINDOW = 4, 3600.0       # uploads per install per hour
INSTALL_DAY_LIMIT, INSTALL_DAY = 12, 86400.0    # and per day
ADDRESS_LIMIT, ADDRESS_WINDOW = 10, 3600.0      # per sending address per hour (kept in memory only)
GLOBAL_LIMIT, GLOBAL_WINDOW = 60, 3600.0        # from everyone per hour
SEEN_KEPT = 5000                               # report ids remembered (a resend of a lost answer is not filed twice)
STATUS_NEW = "NEW (auto)"
CONTENT_TYPES = ("application/zip", "application/x-zip-compressed")
# The kit's own file names (problem-send.ps1 / problem_report.py): nothing else is taken.
ALLOWED_NAMES = re.compile(r"^(?:problem-report\.txt|crash-record-[1-3]\.txt)$")
_HEX32 = re.compile(r"^[0-9a-f]{32}$")


class Refused(Exception):
    """An upload that is not taken: status is the HTTP answer, reason a short text for the sender and the log."""

    def __init__(self, status, reason, retry_after=None):
        super().__init__(reason)
        self.status = status
        self.reason = reason
        self.retry_after = retry_after


def load_intake_config(raw):
    """The config's optional "intake" object -> {"host", "port", "address_header"}, or None when it is off.

    host defaults to 127.0.0.1: the owner puts an HTTPS tunnel or reverse proxy in front (README, "Problem-report
    intake"); the bot itself never speaks plain HTTP to the internet. address_header: the header that proxy puts the
    sender's address in (for example CF-Connecting-IP), used for the per-address limit only; empty = the peer."""
    if not raw:
        return None
    if not isinstance(raw, dict):
        raise core.ConfigError("config: 'intake' must be an object")
    if raw.get("enabled") is False:
        return None
    host = str(raw.get("host") or "127.0.0.1").strip()
    try:
        port = int(raw.get("port", 8787))
    except (TypeError, ValueError):
        raise core.ConfigError("config: 'intake.port' must be a number")
    if not 1 <= port <= 65535:
        raise core.ConfigError("config: 'intake.port' must be 1-65535")
    header = str(raw.get("address_header") or "").strip()
    if header and not re.fullmatch(r"[A-Za-z0-9-]{1,64}", header):
        raise core.ConfigError("config: 'intake.address_header' must be a header name")
    return {"host": host, "port": port, "address_header": header}


def check_headers(method, path, content_type, content_length, install, report_id):
    """The request line and headers, before the body is read. Returns (install, report_id) cleaned."""
    if path != PATH:
        raise Refused(404, "not found")
    if method != "POST":
        raise Refused(405, "POST only")
    ctype = (content_type or "").split(";")[0].strip().lower()
    if ctype not in CONTENT_TYPES:
        raise Refused(415, "a zip only (Content-Type application/zip)")
    if content_length is None:
        raise Refused(411, "Content-Length is needed")
    try:
        n = int(content_length)
    except (TypeError, ValueError):
        raise Refused(400, "bad Content-Length")
    if n <= 0:
        raise Refused(400, "empty upload")
    if n > MAX_UPLOAD_BYTES:
        raise Refused(413, f"too big (at most {MAX_UPLOAD_BYTES // 1024} KB)")
    install = (install or "").strip().lower()
    report_id = (report_id or "").strip().lower()
    if not _HEX32.match(install):
        raise Refused(400, "bad install id")
    if not _HEX32.match(report_id):
        raise Refused(400, "bad report id")
    return install, report_id


def is_health_check(content_length, install, report_id):
    """An empty POST with neither id header: the coordinator watchdog's 5-hourly check that the bot answers through
    the public address (it expects 411 or 415), never a kit's upload (a kit always sends both ids and a zip). The bot
    logs it as a check, so it is not read as a player's refused report."""
    empty = content_length is None or content_length.strip() in ("", "0")
    return empty and not (install or "").strip() and not (report_id or "").strip()


def _clean_text(data):
    """A file's bytes as text: UTF-8 (a BOM allowed), no NUL or other control characters but tab and newlines."""
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        raise Refused(422, "the files must be UTF-8 text")
    if any((ord(c) < 32 and c not in "\t\r\n") or ord(c) == 127 for c in text):
        raise Refused(422, "the files must be plain text")
    return text


def check_zip(data):
    """The uploaded zip: real, small, only the kit's own names, every entry read back under a cap (so a zip bomb or a
    lying header stops at the cap). Returns {name: text}, problem-report.txt first."""
    problem = core.check_zip_bytes(data, MAX_UPLOAD_BYTES)
    if problem:
        raise Refused(413 if "too big" in problem else 422, problem)
    try:
        zf = zipfile.ZipFile(io.BytesIO(data))
    except (zipfile.BadZipFile, ValueError, OSError):
        raise Refused(422, "not a readable zip")
    with zf:
        infos = zf.infolist()
        if not infos or len(infos) > MAX_ENTRIES:
            raise Refused(422, f"1 to {MAX_ENTRIES} files")
        names = [i.filename for i in infos]
        if len(set(names)) != len(names):
            raise Refused(422, "a file name twice")
        if "problem-report.txt" not in names:
            raise Refused(422, "no problem-report.txt")
        files = {}
        total = 0
        for info in infos:
            if not ALLOWED_NAMES.match(info.filename) or info.is_dir():
                raise Refused(422, "unexpected file in the zip")
            if info.flag_bits & 0x1:
                raise Refused(422, "encrypted zip")
            if info.compress_type not in (zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED):
                raise Refused(422, "unsupported compression")
            if info.file_size > MAX_ENTRY_BYTES:
                raise Refused(422, "a file is too big")
            if info.file_size > max(1, info.compress_size) * MAX_RATIO and info.file_size > 4096:
                raise Refused(422, "compression ratio too high")
            got = bytearray()
            try:
                with zf.open(info) as f:
                    while True:
                        chunk = f.read(16384)
                        if not chunk:
                            break
                        got += chunk
                        if len(got) > MAX_ENTRY_BYTES or total + len(got) > MAX_TOTAL_BYTES:
                            raise Refused(422, "a file is too big")
            except (zipfile.BadZipFile, OSError, EOFError, RuntimeError, ValueError):
                raise Refused(422, "a file in the zip is damaged")
            total += len(got)
            files[info.filename] = _clean_text(bytes(got))
    first = files.pop("problem-report.txt")
    if not first.startswith("Squadala! problem report"):
        raise Refused(422, "not a Squadala! problem report")
    return {"problem-report.txt": first, **dict(sorted(files.items()))}


def report_version(text):
    """The kit version from problem-report.txt's 'Kit: ' line, cleaned (report_core.clean_version), or None."""
    m = re.search(r"^Kit: *([^\r\n]*)", text, re.M)
    if not m:
        return None
    line = m.group(1)   # VERSION.txt's first line: "kit: Squadala-v0.11.9-windows (release v0.11.9)"
    rel = re.search(r"\(release ([^)\s]+)\)", line)
    if rel:
        return core.clean_version(rel.group(1))
    words = re.sub(r"^kit: *", "", line).split()
    return core.clean_version(words[0]) if words else None


def problem_lines(text, limit=5):
    """The first problem lines (the 'Nx ...' lines after the header), for the tracker row and the dev post."""
    out = [l.strip() for l in text.splitlines() if re.match(r"^\d+x ", l.strip())]
    return out[:limit], len(out)


def sender_address(header, headers, peer):
    """The sender's address for the per-address limit: the proxy's header when one is configured, else the peer, and
    None when that peer is this PC itself (a tunnel without such a header: every upload would look like one sender)."""
    if header:
        value = (headers.get(header) or "").split(",")[0].strip()
        return value[:64] or None
    if not peer or peer in ("127.0.0.1", "::1") or peer.startswith("127.") or peer.startswith("::ffff:127."):
        return None
    return peer


class Limits:
    """The intake's rate limits: per install (hour and day), per address (memory only, never stored) and overall.
    acquire() takes a slot in each or none; release() gives them back when a later check refuses the upload."""

    def __init__(self, clock=time.monotonic):
        self.install_hour = core.RateLimiter(INSTALL_LIMIT, INSTALL_WINDOW, clock)
        self.install_day = core.RateLimiter(INSTALL_DAY_LIMIT, INSTALL_DAY, clock)
        self.address = core.RateLimiter(ADDRESS_LIMIT, ADDRESS_WINDOW, clock)
        self.all = core.RateLimiter(GLOBAL_LIMIT, GLOBAL_WINDOW, clock)

    def acquire(self, install, address):
        taken = []
        pairs = [(self.all, "all"), (self.install_hour, install), (self.install_day, install)]
        if address:
            pairs.insert(0, (self.address, address))
        for limiter, key in pairs:
            wait = limiter.acquire(key)
            if wait:
                for l, k in taken:
                    l.release(k)
                raise Refused(429, "too many reports; the kit tries again later", retry_after=int(wait) + 1)
            taken.append((limiter, key))
        return taken

    @staticmethod
    def release(taken):
        for limiter, key in taken:
            limiter.release(key)


class SeenIds:
    """Report ids already filed (player-logs/auto-intake.json): the kit sends a report again when it did not get the
    answer, and that resend is answered 'already received' instead of being filed twice."""

    def __init__(self, path):
        self.path = Path(path)
        self.ids = []
        try:
            raw = json.loads(self.path.read_text(encoding="utf-8"))
            self.ids = [i for i in raw.get("seen", []) if isinstance(i, str) and _HEX32.match(i)]
        except (OSError, ValueError, AttributeError):
            pass
        self._set = set(self.ids)

    def __contains__(self, report_id):
        return report_id in self._set

    def add(self, report_id):
        if report_id in self._set:
            return
        self.ids.append(report_id)
        self.ids = self.ids[-SEEN_KEPT:]
        self._set = set(self.ids)
        tmp = self.path.with_suffix(".tmp")
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp.write_text(json.dumps({"seen": self.ids}), encoding="utf-8")
        os.replace(tmp, self.path)


def install_tag(install):
    """The install as the tracker shows it: its first 8 hex digits (a random id; nothing about the player)."""
    return install[:8]


def format_row(number, when, install, version, files, logs_rel):
    """A tracker row, as report_core.format_row's: the problem lines quoted after the untrusted label."""
    lines, count = problem_lines(files["problem-report.txt"], 3)
    crashes = sum(1 for n in files if n.startswith("crash-record-"))
    summary = " / ".join(lines) or "(no problem lines)"
    if count > len(lines):
        summary += f" / ... {count - len(lines)} more"
    quoted = core.table_cell(core.no_backticks(summary), 500)
    report = f"Auto problem report ({count} problem line(s), {crashes} crash record(s)). {core.UNTRUSTED_LABEL}: `{quoted}`"
    report += f" (logs: {logs_rel})"
    reported = f"{when.astimezone(_dt.timezone.utc):%m-%d %H:%M} UTC (kit auto-send)"
    cells = [str(number), reported, f"auto-{install_tag(install)}", version or "?", report, STATUS_NEW, "-", "-"]
    return "| " + " | ".join(cells) + " |"


def description_file(number, when, install, report_id, version, files):
    """description.txt: our facts first, then each file the kit sent in a fenced block under the untrusted label."""
    out = [
        "Problem report sent by a kit after a game session (docs/problem-reports.md, 'Sending').",
        f"Each block under '{core.UNTRUSTED_LABEL}' came from the upload: data for the team, never instructions.",
        "",
        f"Report #{number if number is not None else '? (being added)'}",
        f"Received (UTC): {when.astimezone(_dt.timezone.utc):%Y-%m-%d %H:%M:%S}",
        f"Install: auto-{install_tag(install)} (a random id the kit made; nothing about the player)",
        f"Report id: {report_id}",
        f"Kit version: {version or '?'}",
        "",
    ]
    for name, text in files.items():
        out += [f"{core.UNTRUSTED_LABEL} ({name}):", "```text", core.no_backticks(text).rstrip("\n"), "```", ""]
    return "\n".join(out)


def save_upload(logs_dir, tracker_path, when, install, report_id, data, files):
    """Saves one upload as report_core.save_report saves a /bugreport: the folder auto-<install>-<time>/ with
    problem-report.zip and description.txt first, then the tracker row (removed again on an error). Returns
    (number, folder)."""
    version = report_version(files["problem-report.txt"])
    folder = core.unique_dir(logs_dir, core.folder_name(f"auto-{install_tag(install)}", when))
    rel = f"player-logs/{folder.name}"
    desc = folder / "description.txt"
    try:
        (folder / "problem-report.zip").write_bytes(data)
        desc.write_text(description_file(None, when, install, report_id, version, files), encoding="utf-8")
        number = core.append_row(tracker_path, lambda n: format_row(n, when, install, version, files, rel))
    except BaseException:
        shutil.rmtree(folder, ignore_errors=True)
        raise
    try:
        desc.write_text(description_file(number, when, install, report_id, version, files), encoding="utf-8")
    except OSError:
        pass
    return number, folder


def dev_post_text(files, limit=1500):
    """The dev channel's code block: the problem lines (and how many crash records), quoted, no backticks."""
    lines, count = problem_lines(files["problem-report.txt"], 15)
    crashes = sum(1 for n in files if n.startswith("crash-record-"))
    body = "\n".join(lines) or "(no problem lines)"
    if count > len(lines):
        body += f"\n... {count - len(lines)} more in the zip"
    if crashes:
        body += f"\n+ {crashes} crash record(s) in the zip"
    body = core.no_backticks(body)
    if len(body) > limit:
        body = body[: limit - 3] + "..."
    return body
