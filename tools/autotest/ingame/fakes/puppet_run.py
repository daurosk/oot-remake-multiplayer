"""Fake-game reactions for row 307 (ingame/test_puppet_run.py). Only on a boot with LICHII_COOP_AS_VARS_PROBE=1 (the
test's needs_env), so no other row sees them. Lines are the module's (test_puppet_run.py cites each writer).

- A's co-op pack manifest (coop_puppet.json) gets Malon's child puppet: characters.numbers {"3": 5}, variants
  {"3": ["Child", "Adult"]} (merged into what another fake wrote there).
- B's CharacterMenu_SelectedCharacter set to 5: 60 frames later A logs 'P2 CoopPuppet_Child_C003 spawned after ...';
  set to 0: 'P2 CoopPuppet_Child_Green spawned after ...'.
- Each bridge `move` of B: B logs an as-vars probe report line, A a pose-copy line and its puppet line, and both the
  probe's 'vars' line (coop_puppet.cpp AsVarsProbeFrame): B's own values from the move's strength, A's puppet of B the
  same values (the fix's Writer), and on Malon's spawn A's 'AS variable ... handle' lines (CoopAsVars::Writer::Finish).
Selftest switch BT_FAKE_PUPPET_RUN_BUG (tests/test_ingame_puppet_run.py): noprobe -> B logs no probe line (FAIL);
nospawn -> A never makes Malon's puppet (FAIL: the body's until()); novars -> A's character puppet holds 0 (before
the fix: FAIL).
"""
import json
import os

SWITCH = "LICHII_COOP_AS_VARS_PROBE=1"
CHAR_FLAG = "CharacterMenu_SelectedCharacter"
PACK = os.path.join("mods", "contents", "0400486024DEC000", "coop-puppet")   # testpack.PACK_REL
SPAWN = "coop: puppet: P2 %s spawned after 1.0 s"
PROBE_B = "coop: puppet: as-vars probe: 40 reads, our speed now 4.10 (0.00 to 5.20), 3 words with |r| >= 0.6"
PROBE_A = "coop: puppet: as-vars probe: P2 %s: ctl+0x58>:+0x24 0.000"
VARS = "coop: puppet: as-vars probe: vars | %s L=%.3f V=%.3f S=%.3f bits=%d"
HANDLE_A = "coop: puppet: P2: AS variable '%s' handle %d"
POSE_A = "coop: pose: Player_Child 120 bones, 60 moving; 30 sent/s, 30 received/s, %d written/s, %d refused/s"


def _bug():
    return os.environ.get("BT_FAKE_PUPPET_RUN_BUG", "")


def _on(game):
    return game.env_on(SWITCH)


def setup(game):
    game.pr_actor = "CoopPuppet_Child_Green"
    if game.tag != "A" or not _on(game):
        return
    d = os.path.join(game.folder, PACK)
    os.makedirs(d, exist_ok=True)
    f = os.path.join(d, "coop_puppet.json")
    try:
        with open(f, encoding="utf-8") as fh:
            j = json.load(fh)
    except (OSError, ValueError):
        j = {"tool": "tools/puppet_pack"}
    c = j.setdefault("characters", {})
    c.setdefault("numbers", {})["3"] = 5
    c.setdefault("variants", {})["3"] = ["Child", "Adult"]
    with open(f, "w", encoding="utf-8") as fh:
        json.dump(j, fh)


def _spawn(a, actor):
    a.pr_actor = actor
    a.say(SPAWN % actor)
    if actor.endswith("C003"):
        for i, name in enumerate(("LeftStickLength", "LinearVelXZ", "XZSpeed", "IsLeftStickInput", "IsDash")):
            a.say(HANDLE_A % (name, 58 + i))


def after(game, m, r):
    if not _on(game) or game.tag != "B":
        return
    a = game.other
    if m.get("cmd") == "flag" and m.get("name") == CHAR_FLAG:
        actor = "CoopPuppet_Child_C003" if m.get("value") == 5 else "CoopPuppet_Child_Green"
        if not (_bug() == "nospawn" and actor.endswith("C003")):
            a.later(60, lambda: _spawn(a, actor))
    elif m.get("cmd") == "move":
        if _bug() != "noprobe":
            game.say(PROBE_B)
        char = a.pr_actor.endswith("C003")
        k = float(m.get("strength") or 0.0)
        vals = (k, 5.2 * k, 5.2 * k, (1 if k > 0 else 0) | (2 if k >= 1.0 else 0))
        game.say(VARS % (("ours",) + vals))
        held = (0.0, 0.0, 0.0, 0) if (char and _bug() == "novars") else vals
        a.say(VARS % ("ours", 0.0, 0.0, 0.0, 0) + " | P2 %s L=%.3f V=%.3f S=%.3f bits=%d" % ((a.pr_actor,) + held))
        a.say(POSE_A % (0 if char else 60, 0 if char else 0))
        a.say(PROBE_A % a.pr_actor)
