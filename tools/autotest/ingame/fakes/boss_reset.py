"""Fake-game reactions for rows 138(4)/137's port (ingame/test_boss_reset.py; owner port-g3-save). Lines are the module's
(test_boss_reset.py cites each writer); places are the old scenario's (test_boss_away.py constants).

- Boot BT_FAKE_BOOT=live-deku-gohma-reset: both games in InsideTheDekuTree; each game "loads" its lane folder's
  autotest-save/slot_00/progress.sav (the --save profile's file) into its flags by hash, as the game reads its save
  (the boot itself sets none of Gohma's flags). A name read (Gohma_DefeatBoss) answers from its hash.
- B: Gohma in B's world list once B stood under her ceiling spot (x > 12); a `kill` of her on B kills her: B logs the
  death line 30 frames on, Gohma_DefeatBoss 1. B's max life 12 -> 16 once B reaches the container spot after her death.
- A (host) to the title and back: A's load reads its save again (Gohma_DefeatBoss 0 from the file), and 100 frames on B
  logs 'the fights this session knew are forgotten'. B out of her area (x < 0, room E) after that: the put-back lines
  (Gohma_DefeatBoss = 0 (written), BossDie, BossOut), max life 16 -> 12 (the held-lower line), Gohma alive in B's
  list again.
Selftest switch BT_FAKE_BOSS_RESET_BUG (tests/test_ingame_boss_reset.py): early -> B puts her back at once (still in her
area); noput -> never put back; warp -> put back logged but Gohma_DefeatBoss stays 1 and no Gohma (the warp still up);
nocontainer -> B takes no container; nolower -> no max life put back; nosave -> the games ignore the save file.
"""
import os
import struct

BOOT, SCENE, GOHMA = "live-deku-gohma-reset", "InsideTheDekuTree", "Enemy_DungeonBoss_Gohma"
NAMES = {"Gohma_DefeatBoss": "0x6b093b51"}          # coop_boss_beaten.inc:8
DEFEAT = NAMES["Gohma_DefeatBoss"]
SPOT = [-12.0, 48.5, 66.5]
DEATH = "Dm_Deku_BossOut: our game started the death: the others in the area play it too"
FORGOTTEN = "coop: boss: the host loaded a save: the fights this session knew are forgotten"
PUT = ["coop: boss: Queen Gohma is alive in the host's save: Gohma_DefeatBoss = 0 (written)",
       "coop: boss: Queen Gohma is alive in the host's save: HavePlayedEvent.InsideTheDekuTree_BossDie = 0 (written)",
       "coop: boss: Queen Gohma is alive in the host's save: HavePlayedEvent.Dm_Deku_BossOut = 0 (written)"]
LOWER = ("coop: objects: our Link's max life 16 -> 12 (MaxLife is held lower: the host loaded a save with fewer hearts, "
         "or a boss's one container was taken twice)")


def _bug():
    return os.environ.get("BT_FAKE_BOSS_RESET_BUG", "")


def _on():
    return os.environ.get("BT_FAKE_BOOT", "") == BOOT


def _save_flags(game):
    """{'0x%08x': value} of every entry of the folder's slot_00 save (save_flags.py's table: (hash, value) pairs)."""
    if _bug() == "nosave":
        return {}
    import save_flags as SF
    p = os.path.join(game.folder, "autotest-save", "slot_00", "progress.sav")
    if not os.path.isfile(p):
        return {}
    data = SF.read_file(p)
    out = {}
    for (kind, h), at in SF.index(data).items():
        v = struct.unpack_from("<I", data, at)[0]
        out["0x%08x" % h] = ("0x%08x" % v) if kind == SF.TYPES["enum"] else v
    return out


def setup(game):
    game.br = {"alive": True, "seen": False, "killed": False, "forgot": False, "put": False, "ml": 12}
    if _on():
        game.scene, game.pos = SCENE, list(SPOT)
        game.flags.update(_save_flags(game))


def _b(game):
    return game if game.tag == "B" else game.other


def _here(game):
    return _on() and game.scene == SCENE and not game.at_title


def command(game, m):
    if _on() and m.get("cmd") == "flag_read" and m.get("name") in NAMES:
        return {"ok": True, "kind": "bool", "value": game.flags.get(NAMES[m["name"]])}
    return None


def press(game, buttons):
    if _on() and game.tag == "A" and game.at_title and "A" in buttons:
        b = _b(game)
        game.flags.update(_save_flags(game))           # A's load reads its save file again

        def forgot():
            b.br["forgot"] = True
            b.say(FORGOTTEN)
            if _bug() == "early":
                _put(b)
        b.later(100, forgot)
    return False                                        # (the built-in title press still runs)


def _put(b):
    if b.br["put"] or _bug() == "noput":
        return
    b.br["put"] = True
    for ln in PUT:
        b.say(ln)
    if _bug() != "warp":
        b.flags[DEFEAT] = 0
        b.br["alive"] = True
    if _bug() not in ("nolower", "nocontainer") and b.br["ml"] > 12:
        b.br["ml"] = 12
        b.say(LOWER)


def act(game, words):
    if not (_here(game) and game.tag == "B" and words[0] == "kill" and words[-1] == GOHMA):
        return False
    if game.br["alive"] and game.br["seen"]:
        game.br["alive"], game.br["killed"] = False, True
        game.flags[DEFEAT] = 1
        game.later(30, lambda: game.say(DEATH))
    return True


def state(game, st):
    if not _on():
        return
    b = game.br
    if game.tag == "B" and _here(game):
        if b["killed"] and not b["put"] and abs(game.pos[0] - 9.0) < 2 and _bug() != "nocontainer":
            b["ml"] = max(b["ml"], 16)
        if b["forgot"] and game.pos[0] < 0:
            _put(game)
    st["max_life"] = b["ml"] if st.get("pos") else None


def actors(game):
    if not (_here(game) and game.tag == "B"):
        return []
    b = game.br
    if game.pos[0] > 12:
        b["seen"] = True
    if not b["alive"] or not (b["seen"] or b["put"]):
        return []
    return [{"id": 702, "name": GOHMA, "kind": "enemy", "pos": [18.49, 59.97, 67.79], "owner": 2, "alive": True,
             "life": 21, "max_life": 21}]
