"""Row 237 (+ kOnceGifts): the one-time gifts are given once for the group, not once per player (port group G5 of
coord reports/port-sweep-c131.md; old row offline/u5-rando-run.ps1 with scenarios hs-u5-gifts / hs-u5-cow / hs-u5-prep).

The fix (lichii/hook/coop_shop_sale.h, the comment above kOnceGifts): each gift's "already given" Bool (the six Great
Fairy rewards, the randomizer's eight cows, Medigoron's sale, the frogs' seven payouts) is shared by the pickup rule,
in EVERY session, with or without a seed: 0 -> 1 goes out to every game, a 1 never goes back to 0. A second player's
game then reads the gift as given and does not pay it again (the doubling of the report).

What this file proves on the default boot (child kit save, Kakariko), by state and the module log only (no player
sees a flag: no shot):
- test_once_gifts_listed: both games share the whole table (the old row's 'gifts shared' check, u5-rando-run.ps1:179,
  with the count: coop_hook.cpp:572; 22 in a real lane log, coop-kit/ls/shots-lane1/f1-event/20261125-203629/
  game-out-A-1.txt and -B-1.txt).
- test_once_gifts_shared_once: every Bool of the table set in one game reaches the other, both ways (half set by A,
  the host, half by B, the guest); a 0 written back in one game stays in that game (never back to 0 for the group).
  The old row checked ONE of them (Medigoron 0x0e3dad8a, A -> B, u5-rando-run.ps1:194) and the Storms Bool B -> A
  (:209); this checks all of the table, read from the header (ingame/once_gifts.py), never copied.

NOT taken over (needs a randomizer seed lane, not the default boot; see the port report): the seed's own payouts in
game (Saria's bridge gives the seed's item, the cow / frogs pay once), which the old row judged by shots and which need
the seed's game files mounted one boot before the judged one (reports/u5-rando-3.md, "one boot late").
"""
import os
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import once_gifts                      # noqa: E402  (the ONE list: read from coop_shop_sale.h kOnceGifts)
from test_rows import need_child_in_play   # noqa: E402

GIFTS = once_gifts.names()
# coop_hook.cpp:572 LOG_INFO "coop: gifts: sharing {} one-time gift flags (Great Fairies, cows, Medigoron, frogs)";
# the old row's pattern was 'coop: gifts: sharing \d+ one-time gift flags' (u5-rando-run.ps1:170): here the count too.
LISTED = r"coop: gifts: sharing %d one-time gift flags" % len(GIFTS)
SHARE_FRAMES = 300    # the shared story flag's own wait (test_rows.py test_story_flag_shared): a flag relay, same path
HOLD_FRAMES = 120     # how long a game's own 0 must stay its own (the pickup rule: no 0 is ever sent)


def _is1(v):
    return v in (1, True)


def _is0(v):
    return v in (0, False)


def _values(pair, x, names):
    return {n: pair.flag_read(x, n).get("value") for n in names}


def _put_back(pair):
    """Way back: every gift Bool 0 again in both games (each game's own write; a 0 is never sent, so this is local)."""
    for x in ("A", "B"):
        for n in GIFTS:
            pair.flag(x, n, 0)
    pair.step_both(10)


@pytest.mark.budget(60)
@pytest.mark.row("#237 one-time gifts shared: the whole kOnceGifts table (hs-u5-gifts 'gifts shared')")
def test_once_gifts_listed(pair):
    with pair.setup():
        need_child_in_play(pair)
    for x in ("A", "B"):        # written at the session start (the flag list is built at the connect): pos=0
        assert pair.log[x].find(LISTED, pos=0), "%s: no '%s' since the boot (kOnceGifts not shared)" % (x, LISTED)


@pytest.mark.budget(300)
@pytest.mark.row("#237 a one-time gift's Bool set in one game is set in every game, never back to 0 (hs-u5-gifts/cow)")
def test_once_gifts_shared_once(pair):
    by = {"A": GIFTS[0::2], "B": GIFTS[1::2]}   # A is the host (lane boot), B the guest: both ways of the relay
    with pair.setup():
        need_child_in_play(pair)
        bad = []
        for x in ("A", "B"):
            for n in GIFTS:
                r = pair.flag_read(x, n)
                if r.get("kind") != "bool" or not _is0(r.get("value")):
                    bad.append("%s %s=%s (%s)" % (x, n, r.get("value"), r.get("kind")))
        assert not bad, "gift Bools not at 0 / not Bools at the start: %s" % "; ".join(bad[:6])
        pair.notes.append("%d gift Bools from coop_shop_sale.h, all 0 in A and B" % len(GIFTS))
    try:
        for x in ("A", "B"):
            for n in by[x]:
                pair.flag(x, n, 1)

        def all_set():
            missing = ["%s %s" % (x, n) for x in ("A", "B") for n, v in _values(pair, x, GIFTS).items() if not _is1(v)]
            all_set.missing = missing
            return not missing
        all_set.missing = []
        try:
            pair.until("every gift Bool 1 in A and B", all_set, max_frames=SHARE_FRAMES)
        except AssertionError as e:
            raise AssertionError("%s; still 0: %s" % (e, "; ".join(all_set.missing[:8])))
        pair.notes.append("A set %d, B set %d: all %d are 1 in both" % (len(by["A"]), len(by["B"]), len(GIFTS)))

        for n in GIFTS:              # A's own 0: the pickup rule keeps it A's (coop/host/src/flag_watcher.cpp:1061,
            pair.flag("A", n, 0)     # 'A pickup at 0 is never sent'; :1625 'the game reset it: ours only')
        pair.step_both(HOLD_FRAMES)
        b = _values(pair, "B", GIFTS)
        back = [n for n, v in b.items() if not _is1(v)]
        assert not back, "B lost %d gift Bool(s) when A wrote 0 (a 1 must never go back to 0): %s" % (
            len(back), ", ".join(back[:6]))
        pair.notes.append("A wrote 0 to all %d: B kept every 1 for %d frames" % (len(GIFTS), HOLD_FRAMES))
    finally:
        _put_back(pair)
