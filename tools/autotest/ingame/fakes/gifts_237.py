"""Fake-game reactions for row 237's one-time gifts (ingame/test_gifts_237.py; owner port-g5-237). Hooks: fakegame.py
docstring. Reacts only to the kOnceGifts Bools (ingame/once_gifts.py, read from lichii/hook/coop_shop_sale.h).

As the module does (coop_hook.cpp:570-573 + :776-779, coop/host/src/flag_watcher.cpp share_pickup_named): at the
connect each game logs 'coop: gifts: sharing 22 one-time gift flags (Great Fairies, cows, Medigoron, frogs)' and
starts every gift Bool at 0; a gift Bool set 0 -> 1 in one game reaches the other 12 frames later (the fake's relay
time, as its shared story flag); a 0 is never sent (flag_watcher.cpp:1061).
BT_FAKE_GIFTS=off: the bug as reported (each gift its own per save): no share line, nothing sent.
=back: a 0 is sent like a 1 (a gift taken back from the group). =some: only the first half of the table is shared.
The tests must FAIL in each of those modes.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import once_gifts   # noqa: E402

GIFTS = once_gifts.names()
LINE = "coop: gifts: sharing %d one-time gift flags (Great Fairies, cows, Medigoron, frogs)"


def _mode():
    return os.environ.get("BT_FAKE_GIFTS", "")


def _shared():
    return GIFTS[: len(GIFTS) // 2] if _mode() == "some" else GIFTS


def setup(game):
    for n in GIFTS:
        game.flags[n] = 0
    if _mode() != "off":
        game.say(LINE % len(_shared()))


def after(game, m, r):
    if m.get("cmd") == "flag":
        share(game, m.get("name"), m.get("value"), r.get("was", 0))


def share(game, name, now, was):
    """Gift Bool NAME set from WAS to NOW in GAME: the module's relay to the other game (flag_watcher.cpp
    share_pickup_named). Called for a bridge `flag` and by a fake whose own game sets a gift (fakes/songs.py: the
    frogs' payout), so every gift goes through this one relay and its BT_FAKE_GIFTS modes."""
    if name not in GIFTS or _mode() == "off" or name not in _shared():
        return
    if now in (1, True) and was in (0, False, None):
        go = 1
    elif now in (0, False) and was in (1, True) and _mode() == "back":
        go = 0
    else:
        return
    o = game.other
    o.later(12, lambda: o.flags.__setitem__(name, go))
