# SPDX-License-Identifier: GPL-3.0-or-later
"""The kit's Discord status: "Playing Squadala!" (docs/setup.md, "Discord").

Longshot shows a Discord status through the Discord application named in longshot_options.ini's
discord_app_id= (next to the game), with the words of its discord_details= and discord_state= lines
and the art of discord_image= (patch 0026; the co-op module says "Solo" or "Co-op: N players" itself
once it is loaded). The Squadala! application has no Rich Presence art, so the kit writes
discord_image=none: no image is sent and Discord shows the application's icon. Before every
start the kit (scripts\\play.ps1, scripts/start.sh) runs this to put the Squadala! application and the
kit's release into that file:

    discord_status.py --options <game folder>/longshot_options.ini [--version-file VERSION.txt]

Only these lines are the kit's, and only while they hold what the kit writes: a missing line is added,
a line with the kit's value is brought up to date (a new release), and a line the player changed (their
own application id, their own words) is left alone. Every other line stays as it is, in its place.
Turning the status off is the game's Insert menu ("Discord status" row), which this never touches.
Never stops the start: a problem is one line on stdout and exit code 0.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import tempfile

# The Squadala! application on Discord (discord.com/developers/applications; its icon is the status's
# picture). Its Application ID is public. The ONE place the kit names it.
APP_ID = "1571917068994089063"
NAME = "Squadala!"
SOLO = "Solo"
NO_ART = "none"


def release_of(version_file: str | None) -> str:
    """The kit's release from VERSION.txt's first line, "kit: Squadala-v0.11.4 (release v0.11.4)" -> "0.11.4"."""
    if not version_file:
        return ""
    try:
        with open(version_file, encoding="utf-8", errors="replace") as f:
            first = f.readline()
    except OSError:
        return ""
    m = re.search(r"\(release v?([0-9A-Za-z.+-]{1,40})\)", first)
    return m.group(1) if m else ""


def wanted(release: str) -> list[tuple[str, str, object]]:
    """(key, the kit's value, which values are the kit's own) in the order they are added."""
    details = f"{NAME} {release}" if release else NAME
    return [
        ("discord_app_id", APP_ID, lambda v: v == APP_ID),
        # (no Rich Presence art in the application: none = no image, and Discord shows its icon, the logo)
        ("discord_image", NO_ART, lambda v: v == NO_ART),
        # (any release's line: an update replaces the last release's)
        ("discord_details", details, lambda v: v == NAME or v.startswith(NAME + " ")),
        ("discord_state", SOLO, lambda v: v == SOLO),
    ]


def merge(text: str, release: str) -> str:
    """text (the file as it is, '' none) with the kit's lines in it."""
    # (the file's own line ends; a new file gets "\n" on every system: Longshot reads either)
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines(keepends=True)
    if lines and not lines[-1].endswith(("\n", "\r")):
        lines[-1] += newline
    for key, value, ours in wanted(release):
        prefix = key + "="
        found = False
        for i, line in enumerate(lines):
            if not line.startswith(prefix):
                continue
            found = True
            ending = line[len(line.rstrip("\r\n")):]
            if ours(line[len(prefix):].rstrip("\r\n")):
                lines[i] = prefix + value + ending
            # (the game reads the last usable line of a key: one the player wrote wins either way)
        if not found:
            lines.append(prefix + value + newline)
    return "".join(lines)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--options", required=True, help="longshot_options.ini next to the game")
    ap.add_argument("--version-file", help="the kit's VERSION.txt")
    a = ap.parse_args(argv)
    try:
        try:
            with open(a.options, encoding="utf-8", newline="") as f:
                old = f.read()
        except FileNotFoundError:
            old = ""
        new = merge(old, release_of(a.version_file))
        if new == old:
            return 0
        folder = os.path.dirname(os.path.abspath(a.options))
        fd, tmp = tempfile.mkstemp(prefix=".discord-", dir=folder)
        try:
            with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
                f.write(new)
            os.replace(tmp, a.options)
        except BaseException:
            try:
                os.remove(tmp)
            except OSError:
                pass
            raise
    except (OSError, UnicodeDecodeError) as e:
        print(f"Discord status: longshot_options.ini not changed ({e}); Discord shows Longshot's status.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
