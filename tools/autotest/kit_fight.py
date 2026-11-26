"""`fight`: lock on to one enemy, slash it and guard until it is dead, Link is low, or the frames are used. Standard
library only; the caller adds the `look`. Built on the helpers in playtest_helpers (the enemy is looked up by id before
every round, and a scene change, a load or a stale list stops the fight before anything is judged).

    fight(client, ident, name)  ->  reply (result, enemy life before/after, Link's life, hits, hurt, rounds, hint)

The buttons are Longshot's own map (its host/nn_hid.c: the mouse's left button is Y, "attack", and its right button is
ZL, "shield, lock on"; its tested-configurations page logs a ZL press that held the shield). So lock-on and the shield
are one button, ZL: Link guards by holding ZL without Y. ZL is held through every slash and guard of a fight, from one
`input` to the next (the game is held between them, so no frame sees it let go); only a walk up to the enemy lets it go.
Y is let go after each swing, so every slash is a new press.

The enemy's life is the world list's `life`: the objects walk's Life read (actor +0x270 component table, +0x2b8 the Life
component, +0x1c10 the value, the s32 at +8; docs/addresses.md `kComponentLife`). No other address is read.

Longshot's Toggle ZL/ZR setting (Insert menu, Controls) latches ZL on one press: with it on, each walk-up and new press
turns the lock-on off and on in turn. Fight with it off (the default).
"""
import math

import playtest_client as pc
import playtest_helpers as ph

LOCK = "ZL"
SWORD = "Y"
SHIELD = "ZL"        # the same button as the lock-on: ZL held without the sword raises the shield
REACH_M = 2.2        # the sword reaches about this far; farther than this Link walks up first
APPROACH_FRAMES = 12
SWING_FRAMES = 4     # the sword button held for a swing
RECOVER_FRAMES = 8   # then let go (the lock-on still held), so the next swing is a new press
SHIELD_FRAMES = 10
BACK_OFF_FRAMES = 14
MIN_LIFE = 4         # quarter hearts: at or below this Link backs off and the fight ends ("low")
MAX_FRAMES = 1200
MCP_FRAMES = 600     # the MCP tool's default
STUCK_M = 0.3        # a walk-up that gets less than this much nearer gets nowhere
STUCK_WALKS = 4      # that many in a row: stopped, stuck


def _xz(pos):
    return pos[0], pos[2]


def _dist(link, actor):
    p, q = _xz(link), actor.get("pos")
    if not isinstance(q, (list, tuple)) or len(q) < 3:
        return actor.get("dist")
    return math.hypot(p[0] - q[0], p[1] - q[2])


def _away_yaw(link, actor):
    """The world yaw (0 = +z, 90 = +x) that takes Link straight away from the actor."""
    q = actor["pos"]
    return math.degrees(math.atan2(link[0] - q[0], link[2] - q[2])) % 360


def _buttons(*names):
    """The buttons once each, in order (lock and shield are the same button by default)."""
    out = []
    for n in names:
        if n and n not in out:
            out.append(n)
    return out


def _hold(client, buttons, frames):
    """`input` holds the buttons for `frames` frames and the game is held again after: those frames are the only ones
    that pass (no `step` after it, which would let every button go for as many frames more)."""
    client.input(None, list(buttons), frames)
    return frames


def _slash(client, lock, sword):
    return _hold(client, _buttons(lock, sword), SWING_FRAMES) + _hold(client, _buttons(lock), RECOVER_FRAMES)


def _guard(client, lock, shield):
    return _hold(client, _buttons(lock, shield), SHIELD_FRAMES)


def _event(st):
    return "event" if st.get("event") else None


def fight(client, ident, name=None, max_frames=MAX_FRAMES, lock=LOCK, sword=SWORD, shield=SHIELD, min_life=MIN_LIFE,
          reach_m=REACH_M):
    """Returns result: `dead` (its life was seen at 0), `gone` (it left the list: perhaps dead; the look says),
    `low` (Link's life fell to `min_life` or less: he backed off, BACK_OFF_FRAMES more), `stopped` (see `stopped`: the
    scene changed, a load began, an event is on (a cutscene: nothing is pressed in it), a stale list, a walk-up the game
    refused (`move refused: ...`, no position), or `stuck`: STUCK_WALKS walk-ups in a row that got no 0.3 m nearer),
    `alive` (the frame limit: no round starts that would pass it; `hint` when its life never moved). Also
    `life_before`, `life_after` (the enemy's), `link_before`, `link_after`, `hits` (rounds in which its life went
    down), `hurt` (rounds in which Link's did), `slashes`, `guards`, `rounds`, `frames`."""
    if not 1 <= int(max_frames) <= MAX_FRAMES:
        raise ValueError("max_frames is 1 to %d" % MAX_FRAMES)
    max_frames = int(max_frames)
    a, w = ph._look(client, ident)
    why = ph._no_list(w)
    if why or ph._gone(a, name):
        return {"result": "stopped", "stopped": why or "gone", "rounds": 0, "frames": 0, "hits": 0, "hurt": 0,
                "slashes": 0, "guards": 0}
    name = name or a.get("name")
    st = pc.game_of(client.state())
    scene = st.get("scene")
    before = a.get("life")
    out = {"result": "alive", "life_before": before, "life_after": before, "link_before": st.get("life"),
           "link_after": st.get("life"), "hits": 0, "hurt": 0, "slashes": 0, "guards": 0, "rounds": 0, "frames": 0}
    if isinstance(before, (int, float)) and before <= 0:
        return dict(out, result="dead", note="its life was already 0")
    hurt_last, guarded, still = False, False, 0
    while True:
        stop = ph._interrupted(st, scene, w) or _event(st)
        if stop:
            return dict(out, result="stopped", stopped=stop)
        if ph._gone(a, name):
            return dict(out, result="gone")
        life = a.get("life")
        out["life_after"] = life
        if isinstance(life, (int, float)) and life <= 0:
            return dict(out, result="dead")
        mine = st.get("life")
        out["link_after"] = mine
        if isinstance(mine, (int, float)) and mine <= min_life:
            link = st.get("pos")
            if link and isinstance(a.get("pos"), (list, tuple)):
                client.move(_away_yaw(link, a), 1.0, BACK_OFF_FRAMES, check=False)
                out["frames"] += BACK_OFF_FRAMES
            return dict(out, result="low", note="Link's life is %s (a quarter heart is 1): he backed off" % mine)
        link, dist = st.get("pos"), None
        if link and isinstance(a.get("pos"), (list, tuple)):
            dist = _dist(link, a)
        left = max_frames - out["frames"]
        if dist is not None and dist > reach_m:
            q = a["pos"]
            r = pc.walk_to(client, q[0], q[2], tolerance=reach_m * 0.6, max_frames=min(APPROACH_FRAMES, left),
                           chunk=APPROACH_FRAMES)
            out["frames"] += r.get("frames", 0)
            if not r.get("arrived") and r.get("reason") != "timed out":
                return dict(out, result="stopped", stopped=r.get("reason") or "walk-up failed")
            now = r.get("distance")
            still = still + 1 if not isinstance(now, (int, float)) or dist - now < STUCK_M else 0
            if still >= STUCK_WALKS:
                return dict(out, result="stopped", stopped="stuck",
                            note="%d walk-ups got no %.1f m nearer (%.1f m left)" % (still, STUCK_M, dist))
            guarded = False
        elif hurt_last and not guarded:
            if left < SHIELD_FRAMES:
                break
            out["frames"] += _guard(client, lock, shield)    # block the next blow, then slash again
            out["guards"] += 1
            guarded = True
            still = 0
        else:
            if left < SWING_FRAMES + RECOVER_FRAMES:
                break
            out["frames"] += _slash(client, lock, sword)
            out["slashes"] += 1
            guarded = False
            still = 0
        out["rounds"] += 1
        last_enemy = life
        a, w = ph._look(client, ident)
        st = pc.game_of(client.state())
        was, now_enemy = last_enemy, a.get("life") if isinstance(a, dict) else None
        if isinstance(now_enemy, (int, float)) and isinstance(was, (int, float)) and now_enemy < was:
            out["hits"] += 1
        now_life = st.get("life")
        hurt_last = isinstance(now_life, (int, float)) and isinstance(mine, (int, float)) and now_life < mine
        if hurt_last:
            out["hurt"] += 1
        if out["frames"] >= max_frames:
            break
    stop = ph._interrupted(st, scene, w) or _event(st)
    if stop:
        return dict(out, result="stopped", stopped=stop)
    if ph._gone(a, name):
        return dict(out, result="gone")
    out["life_after"] = a.get("life")
    out["link_after"] = st.get("life")
    if isinstance(out["life_after"], (int, float)) and out["life_after"] <= 0:
        out["result"] = "dead"
    elif out["slashes"] and out["life_after"] == before:
        out["hint"] = ("its life never moved in %d slashes (lock-on %s, sword %s): the sword may not be out or equipped, "
                       "the lock-on may have taken another target, or it cannot be hurt that way (Gohma: only her eye "
                       "while she is stunned, a Deku Seed first)" % (out["slashes"], lock, sword))
    return out
