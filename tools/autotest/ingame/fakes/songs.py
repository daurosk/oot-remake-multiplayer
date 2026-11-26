"""Fake-game reactions for regress step 11's port (ingame/test_songs.py; owner regress-port-c). Each line cites the module.

- the session start logs 'coop: gifts: sharing 22 one-time gift flags (...)' (coop_hook.cpp:572).
- the ocarina: a game whose boot loaded --save no-ocarina (its coop-test-save.json names it, testsave.MANIFEST) holds
  none: no Fairy count, the Bools a get sets 0, EquipOcarina "" (save_flags.NO_OCARINA), and D-pad Left takes nothing
  out (issue 190). Else the kit save's: all held. A `give Item_Ocarina_Fairy` writes the count (docs/playtest.md `give`);
  the other game gets the count 30 frames on (item sync), and each game holding a count mends its own Bools and
  EquipOcarina, logging "coop: items: ocarina '' -> 'Item_Ocarina_Fairy' for the held ocarina" (coop_hook.cpp:3502).
- the frogs (ZoraRiver_Child, B within 1 m of TurnToTarget): D-pad Left opens their talk (event), A opens the prompt;
  a known song (IsGet.Item_Melody_<song>, given) played from ocarina_notes.keys starts their payout talk: three A
  presses, then their Bool (MiniGame_FrogTenors_ZeldaLullaby 0x6e3948e7 / _StormSong 0xd578df95) is 1, the ocarina put
  away, and the other game's copy is 1 30 frames on (kOnceGifts, coop_shop_sale.h).
Selftest switches (tests/test_ingame_songs.py): BT_FAKE_SONGS_BUG=noshare -> the other game never gets a frogs' Bool;
=nopay -> the frogs take no song; =noequip -> a synced ocarina is not mended (the #190 bug: nothing comes out);
=nogifts -> no gifts line at the session start.
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import ocarina_notes as ON   # noqa: E402

RIVER = "ZoraRiver_Child"
FROG_SPOT = (8.90, 102.40, -50.90)
PAID = {"ZeldaLullaby": "0x6e3948e7", "StormSong": "0xd578df95"}
KNOWN = {"ZeldaLullaby": "IsGet.Item_Melody_ZeldaLullaby", "StormSong": "IsGet.Item_Melody_StormSong"}
COUNT, EQUIP = "0xb7df60f1", "0xb72cd3fc"
BOOLS = ("0x42bd296f", "0x137df1ac", "0xaacdef1f", "0xda41879d", "0x171de71f")
TIME = ("0x54e2a3d1", "0x1da2c511", "0xac6a85e3")   # the Ocarina of Time's count, IsGet, Got (save_flags.NO_OCARINA)
FAIRY = "Item_Ocarina_Fairy"


def _bug():
    return os.environ.get("BT_FAKE_SONGS_BUG", "")


def _profile(game):
    try:
        with open(os.path.join(game.folder, "coop-test-save.json"), encoding="utf-8") as f:
            return json.load(f).get("profile")
    except (OSError, ValueError):
        return None


def setup(game):
    held = _profile(game) != "no-ocarina"
    game.flags.update({COUNT: 1 if held else 0, EQUIP: FAIRY if held else ""})
    game.flags.update({h: 1 if held else 0 for h in BOOLS})
    game.flags.update({h: 0 for h in TIME})              # the kit save has no Ocarina of Time (lane1-A slot_00..05)
    game.flags.update({h: 0 for h in PAID.values()})
    game.frogs, game.frog_keys, game.frog_song = None, [], None
    if _bug() != "nogifts":
        game.say("coop: gifts: sharing 22 one-time gift flags (Great Fairies, cows, Medigoron, frogs)")


def _holds(game):
    return bool(game.flags.get(COUNT)) and all(game.flags.get(h) for h in BOOLS) and game.flags.get(EQUIP) == FAIRY


def _mend(game):
    """MendOcarina (coop_hook.cpp:3455-3504): a held count raises the Bools, then names the equip."""
    if not game.flags.get(COUNT) or _bug() == "noequip":
        return
    for h in BOOLS:
        if not game.flags.get(h):
            game.flags[h] = 1
            game.say("coop: items: ocarina bool %s 0 -> 1 for the held ocarina" % h)
    if game.flags.get(EQUIP) == "":
        game.flags[EQUIP] = FAIRY
        game.say("coop: items: ocarina '' -> 'Item_Ocarina_Fairy' for the held ocarina")


def after(game, m, r):
    cmd, name = m.get("cmd"), m.get("name") or m.get("item")
    if cmd == "give" and name == FAIRY:
        game.flags[COUNT] = 1
        game.later(10, lambda: _mend(game))
        o = game.other
        o.later(30, lambda: (o.flags.__setitem__(COUNT, 1), o.later(20, lambda: _mend(o))))
    elif cmd == "give" and name in KNOWN.values():
        game.flags[name] = 1
    elif cmd == "flag_read" and name == EQUIP:
        r.update(kind="text", value=game.flags.get(EQUIP))
    elif cmd == "flag_read" and name == COUNT:
        r.update(kind="int", value=game.flags.get(COUNT))


def _at_frogs(game):
    return (game.tag == "B" and game.scene == RIVER and not game.at_title
            and math.dist(game.pos, FROG_SPOT) < 1.0)


def state(game, st):
    if game.frogs:
        st["event"] = True


def press(game, buttons):
    if "LEFT" in buttons and not _holds(game):
        return True                                      # the #190 bug: nothing comes out
    if not _at_frogs(game):
        return False
    if "LEFT" in buttons and not game.frogs:
        game.frogs = "intro"
        return False                                     # the built-in: the ocarina out + the probe line
    if "A" in buttons and game.frogs == "intro":
        game.frogs = "prompt"
        return True
    if "A" in buttons and game.frogs and game.frogs.startswith("talk"):
        left = int(game.frogs[4:]) - 1
        game.frogs = "talk%d" % left if left else None
        if not left:
            paid, o = PAID[game.frog_song], game.other
            game.flags[paid], game.ocarina = 1, False
            if _bug() != "noshare":
                o.later(30, lambda: o.flags.__setitem__(paid, 1))
        return True
    if game.frogs == "prompt" and game.ocarina:
        game.frog_keys = (game.frog_keys + [b for b in buttons if b in ON.KEY_NOTE])[-6:]
        for song in PAID:
            if game.frog_keys == ON.keys(song) and game.flags.get(KNOWN[song]) and _bug() != "nopay":
                game.frogs, game.frog_song, game.frog_keys = "talk3", song, []
    return False                                         # the built-in notes
