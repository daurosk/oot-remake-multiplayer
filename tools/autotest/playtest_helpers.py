"""Mid-level play helpers on top of the play-test bridge client (docs/playtest.md, "Helpers").

They are Python, not module verbs: each one calls the bridge's own commands (`state`, `dump` for the world list, `input`,
`step`, `act`) and is bounded: at most MAX_FRAMES frames (120 by default), and each says why it stopped.

Rule Y (the exact actor the tester picked): a helper that goes to or strikes an actor takes the `id` get_world gave and
remembers the actor's name; it looks the actor up again before every leg and every press, and stops with `gone` when the id
is no longer in the list or its name changed. It never falls back to another actor of the same name.

`both`: pass `InStep(this_game, other_game)` as the client and every frame the helper runs in this game also runs in the
other (the held game of a pair must be stepped by the same frames, or the two drift). Standard library only.
"""
import math
import re

import playtest_client as pc

MAX_FRAMES = 120       # one helper call runs at most this many frames in the game
NEAR_M = 1.5           # "near an actor": within this many metres
INTERACT_M = 1.0       # interact walks this near before it looks for the A prompt
LEG_M = 4.0            # go_to_actor walks in legs of this length, looking the actor up again after each
LEG_FRAMES = 30
STUCK_FRAMES = 60      # no 0.3 m closer to the actor over this many frames: stuck
STRIKE_FRAMES = 12     # a kill or hit waits at most this many frames for the walk's result
STRIKE_LOOK = 2        # ... looking every this many frames
DEATH_FRAMES = 36      # a kill that struck waits at most this many more frames for the actor's death (a guess: S8 saw one
DEATH_LOOK = 6         # ... die only after Link came near it; looking every this many frames
PRESS_FRAMES = 6
CLOSER_M = 0.3         # a leg that brings Link this much nearer is progress


class InStep:
    """Two games in step: every frame the primary game runs through input(), move() or step() the other game runs too. Anything
    else (state, world, act) goes to the primary only. The other game runs what the primary's frame count moved (`start`:
    the primary's frame before the first call, else its last seen frame), so a call that ran fewer frames than it asked
    (cut short, refused after a partial run) never puts the other game ahead; without frame counts, what was asked."""

    def __init__(self, primary, other, start=None):
        self.primary, self.other = primary, other
        self.seen = start if start is not None else getattr(primary, "last_frame", None)
        self.stepped = 0   # frames the other game ran so far
        self.short = 0     # frames asked that the primary did not run (so the other did not either)

    def __getattr__(self, name):
        return getattr(self.primary, name)

    def _ran(self, asked):
        """What the primary ran since the last look: its frame count's move, else `asked`."""
        now = getattr(self.primary, "last_frame", None)
        if self.seen is None or now is None:
            ran = asked
        else:
            ran = max(0, int(now) - int(self.seen))
        if now is not None:
            self.seen = now
        self.short += max(0, asked - ran)
        return ran

    def _other(self, frames):
        while frames > 0:
            n = min(frames, pc.MAX_FRAMES)
            self.other.step(n)
            self.stepped += n
            frames -= n

    def input(self, stick=None, buttons=(), frames=1, rstick=None):
        r = self.primary.input(stick, buttons, frames, rstick)
        self._other(self._ran(frames))
        return r

    def move(self, yaw, strength=1.0, frames=1, check=True):
        r = self.primary.move(yaw, strength, frames, check)
        self._other(self._ran(frames if r.get("ok") else 0))   # (a refused move ran no frames)
        return r

    def step(self, frames=1):
        r = self.primary.step(frames)
        self._other(self._ran(frames))
        return r

    def catch_up(self):
        """Steps the other game by what the primary ran since the last input, move or step (a warp, a test line, a shot,
        or a call that failed after running some frames), from its frame count. Returns the frames added."""
        added = self._ran(0)
        self._other(added)
        return added


def _pos(state):
    p = state.get("pos")
    return p if isinstance(p, (list, tuple)) and len(p) >= 3 else None


def _look(client, ident):
    """(the actor, its world list) for the id now. One `dump`: the walk's newest snapshot."""
    w = client.world()["world"] or {}
    return pc.find_actor(w, ident), w


def _no_list(w):
    """Why the world list cannot be used, or None when the walk ran: `no world list: ...` (solo, an older module) or
    `stale world list: ...` (the walk stopped: a load, the session ended). A list like that says nothing about the actor,
    so a helper reports this, never `gone`."""
    if not w:
        return "no world list: this module has none (an older build)"
    if w.get("stale"):
        return "stale world list: " + str(w.get("why") or "no walk for a while")
    return None if pc.walk_ran(w.get("why")) else "no world list: " + str(w.get("why") or "none")


def _interrupted(st, scene, w):
    """The real reason a helper must stop before it judges the actor: the scene changed, a load is on, or the list is
    unusable (checked in this order: after a door the new scene's list lacks the actor, which is no death)."""
    if st.get("scene") != scene:
        return "scene changed"
    if st.get("loading"):
        return "loading"
    return _no_list(w)


def _gone(a, name):
    return a is None or (name is not None and a.get("name") != name)


def go_to_actor(client, ident, name=None, near_m=NEAR_M, max_frames=MAX_FRAMES, stuck_after=STUCK_FRAMES,
                leg_m=LEG_M):
    """Walk Link toward the actor `ident` (the `id` from get_world) in legs of `leg_m`, looking it up again after every
    leg. `name` (from get_world, the name that came with the id): the actor must still have it. Stops near it
    (`arrived`), or `reason`: gone (no longer in the list, or another name), stuck (`stuck_after` frames without getting
    0.3 m nearer; `moved_m` says how far Link went in all), timed out, no world list, scene changed, loading, event.
    Returns arrived, reason, stuck, moved_m, frames, distance, actor (the last one seen)."""
    a, w = _look(client, ident)
    out = {"arrived": False, "reason": "", "stuck": False, "moved_m": 0.0, "frames": 0, "distance": None, "actor": a}
    name = name or (a or {}).get("name")
    st = pc.game_of(client.state())
    scene, used, still = st.get("scene"), 0, 0
    start = _pos(st)
    prev, leg_n, misses = None, 0, 0
    while True:
        stop = _interrupted(st, scene, w)         # (before `gone`: a scene change or a stale list is not a death)
        if stop:
            return dict(out, reason=stop)
        if _gone(a, name):
            misses += 1
            if misses >= GONE_READS:
                return dict(out, reason="gone")
            client.step(PRESS_FRAMES)             # (one read can miss an actor that is there: look again)
            used += PRESS_FRAMES
            a, w = _look(client, ident)
            st = pc.game_of(client.state())
            continue
        misses = 0
        out["actor"] = a
        pos, at = _pos(st), a.get("pos")
        if pos is None or not at:
            return dict(out, reason=pc.no_position_reason(st) if pos is None else "no position (not in play?)")
        dist = math.hypot(at[0] - pos[0], at[2] - pos[2])
        out.update(distance=round(dist, 2), frames=used,
                   moved_m=round(math.hypot(pos[0] - start[0], pos[2] - start[2]), 2) if start else 0.0)
        if prev is not None:
            still = 0 if dist <= prev - CLOSER_M else still + leg_n
        prev = dist
        if dist <= near_m:
            return dict(out, arrived=True, reason="near")
        if st.get("event"):
            return dict(out, reason="event")
        if still >= stuck_after:
            return dict(out, reason="stuck", stuck=True)
        if used >= max_frames:
            return dict(out, reason="timed out")
        step = max(0.5, min(leg_m, dist - near_m * 0.5))
        r = pc.walk_to(client, pos[0] + (at[0] - pos[0]) / dist * step, pos[2] + (at[2] - pos[2]) / dist * step,
                       tolerance=0.4, max_frames=min(LEG_FRAMES, max_frames - used))
        st = pc.game_of(r["state"]) if isinstance(r.get("state"), dict) else pc.game_of(client.state())
        leg_n = max(1, r.get("frames", 0))
        used += leg_n
        a, w = _look(client, ident)
        st = pc.game_of(client.state())     # (the state at the look, so the scene and the list agree)


TURN_TOL = 10.0        # Link's facing within this many degrees of the point: turned
TURN_STRENGTH = 0.25   # a light push of the stick: Link turns that way and takes only a small step
TURN_FRAMES = 2        # frames of each push
TURN_ROUNDS = 12       # pushes before giving up


def turn_to(client, x, z, tol=TURN_TOL, rounds=TURN_ROUNDS):
    """Turn Link toward (x, z) the way a player does: light pushes of the left stick that way (`move`, TURN_FRAMES
    each) until the state's `facing` is within `tol` degrees. No line writes his yaw. Returns turned, reason, facing,
    off (degrees left), rounds, frames and moved_m (how far the pushes stepped him)."""
    st = pc.game_of(client.state())
    start = st.get("pos")
    out = {"turned": False, "reason": "", "facing": st.get("facing"), "off": None, "rounds": 0, "frames": 0,
           "moved_m": 0.0}
    for r in range(rounds + 1):
        pos, facing = st.get("pos"), st.get("facing")
        if not pos or not start or facing is None:
            out["reason"] = "no place or facing in the state"
            return out
        want = pc.yaw_to(pos, x, z) % 360
        off = (want - float(facing) + 180.0) % 360.0 - 180.0
        out.update(facing=facing, off=round(off, 1), rounds=r, moved_m=round(math.dist(start, pos), 3))
        if abs(off) <= tol:
            out.update(turned=True, reason="ok")
            return out
        if r == rounds:
            break
        m = client.move(want, TURN_STRENGTH, TURN_FRAMES, check=False)
        if not m.get("ok"):
            out["reason"] = "move refused: " + str(m.get("error") or "no reason")
            return out
        out["frames"] += TURN_FRAMES
        st = pc.game_of(client.state())
    out["reason"] = "still %.0f degrees off after %d pushes of the stick" % (out["off"], rounds)
    return out


def face(client, ident, name=None):
    """Turn Link to the actor with the stick (`turn_to`, the actor's place). Looks the actor up first."""
    a, w = _look(client, ident)
    if _no_list(w):
        return {"faced": False, "reason": _no_list(w), "actor": a}
    if _gone(a, name):
        return {"faced": False, "reason": "gone", "actor": a}
    at = a.get("pos")
    if not at:
        return {"faced": False, "reason": "no position", "actor": a}
    t = turn_to(client, at[0], at[2])
    return {"faced": t["turned"], "reason": t["reason"], "actor": a, "turn": t}


def place_facing(client, place_line, point, frames=2):
    """`place X Y Z` and then `face X Z` (the point to look at): the bridge puts Link on its next frames, so a frame pair
    passes between the two lines and after the second. Returns the replies of both and the frames used. A set-up step,
    not play (like `place` itself): the only kit helper that still writes Link's yaw."""
    placed = client.act(place_line)
    client.step(frames)
    faced = client.act("face %.2f %.2f" % point)
    client.step(frames)
    return {"placed": placed, "faced": faced, "facing_at": [round(point[0], 2), round(point[1], 2)],
            "frames": 2 * frames, "state": pc.game_of(client.state())}


def attack(client, ident, button="Y", name=None, max_frames=MAX_FRAMES, press_frames=PRESS_FRAMES):
    """go_to_actor, face, then press `button` (the sword: Y, Longshot's attack button, host/nn_hid.c) in rounds until the
    actor's life is 0 or `max_frames` frames are used. The actor is looked up before the walk and before every press:
    nothing is pressed at an actor that is gone or has another name. Returns result (dead: its life was seen at 0;
    gone: it left the list in the fight, perhaps dead; alive: the frame limit, with a `hint` when its life never moved;
    stopped: the walk or the turn did not finish, or the scene changed, a load began or the list went stale: see
    `stopped`), life_before, life_after, frames, rounds."""
    a, w = _look(client, ident)
    if _no_list(w) or _gone(a, name):
        return {"result": "stopped", "stopped": _no_list(w) or "gone", "life_before": None, "life_after": None,
                "frames": 0, "rounds": 0}
    name = name or a.get("name")
    before = a.get("life")
    out = {"result": "alive", "life_before": before, "life_after": before, "frames": 0, "rounds": 0}
    if isinstance(before, (int, float)) and before <= 0:
        return dict(out, result="dead", note="its life was already 0")
    g = go_to_actor(client, ident, name=name, max_frames=max_frames)
    out["frames"] = g["frames"]
    if not g["arrived"]:
        return dict(out, result="stopped", stopped=g["reason"], moved_m=g["moved_m"], stuck=g["stuck"])
    f = face(client, ident, name)
    if not f["faced"]:
        return dict(out, result="stopped", stopped=f["reason"])
    out["frames"] += 2
    scene = pc.game_of(client.state()).get("scene")
    while out["frames"] < max_frames:
        a, w = _look(client, ident)
        stop = _interrupted(pc.game_of(client.state()), scene, w)
        if stop:
            return dict(out, result="stopped", stopped=stop)
        if _gone(a, name):
            return dict(out, result="gone")
        life = a.get("life")
        out["life_after"] = life
        if isinstance(life, (int, float)) and life <= 0:
            return dict(out, result="dead")
        client.input(None, [button], press_frames)
        client.step(press_frames)
        out["frames"] += 2 * press_frames
        out["rounds"] += 1
    a, w = _look(client, ident)
    stop = _interrupted(pc.game_of(client.state()), scene, w)
    if stop:
        return dict(out, result="stopped", stopped=stop)
    if _gone(a, name):
        return dict(out, result="gone")
    out["life_after"] = a.get("life")
    if isinstance(out["life_after"], (int, float)) and out["life_after"] <= 0:
        out["result"] = "dead"
    elif out["rounds"] and out["life_after"] == before:
        out["hint"] = ("its life never moved in %d rounds (button %s): the sword may not be out, or the "
                       "actor cannot be hurt that way (`fight` locks on first)" % (out["rounds"], button))
    return out


CHANGED = ("life", "max_life", "rupees", "event", "prompt", "equips", "age", "body", "room")


def _changed(before, after):
    out = {k: {"was": before.get(k), "now": after.get(k)} for k in CHANGED if before.get(k) != after.get(k)}
    # the inventory is about 290 entries: only the ones that changed, one by one (under another key, so that the
    # tool layer's `inventory` filter leaves it alone)
    b, a = before.get("inventory") or {}, after.get("inventory") or {}
    diff = {k: {"was": b.get(k, 0), "now": a.get(k, 0)} for k in sorted(set(b) | set(a)) if b.get(k, 0) != a.get(k, 0)}
    if diff:
        out["inventory_changed"] = diff
    return out


ITEM_BUTTON = "ZR"    # the button `assign` puts an item on (lichii/hook/coop_testinput.cpp; Y is the sword)


def use_item(client, item, button=ITEM_BUTTON, frames=PRESS_FRAMES):
    """`assign` the item (the bridge's `assign ITEM`), press `button` for `frames` frames and step as many, and return
    what changed in the state (life, rupees, event, prompt, equips, inventory). The button is ZR, the one `assign` puts
    the item on (the bridge's `assign`, docs/testing.md); Y would swing the sword."""
    before = pc.game_of(client.state())
    client.act("assign " + item)
    client.step(2)
    client.input(None, [button], frames)
    after = pc.game_of(client.step(frames))
    return {"item": item, "button": button, "changed": _changed(before, after), "frames": 2 + 2 * frames,
            "state": after}


NO_PROMPT = ("", "empty", "none", "null", "nothing")


def no_prompt(prompt):
    """True when the A prompt says nothing is there: no value, or the word the game uses for it ("Empty")."""
    return not prompt or (isinstance(prompt, str) and prompt.strip().lower() in NO_PROMPT)


def interact(client, what="talk", frames=PRESS_FRAMES):
    """Press A when the state's `prompt` says an action is there (talk to an NPC, open a chest or a door); returns the
    new prompt and event. With no prompt nothing is pressed."""
    before = pc.game_of(client.state())
    if no_prompt(before.get("prompt")):
        return {"result": "nothing to %s here: the A prompt is empty" % what, "pressed": False,
                "prompt": before.get("prompt"), "event": before.get("event")}
    client.input(None, ["A"], frames)
    after = pc.game_of(client.step(frames))
    return {"result": "pressed A", "pressed": True, "prompt_was": before.get("prompt"), "prompt": after.get("prompt"),
            "event": after.get("event"), "frames": 2 * frames}


def interact_with(client, ident, name=None, near_m=INTERACT_M, max_frames=MAX_FRAMES):
    """go_to_actor, face it, then interact (A only when the state's prompt says an action is there). Returns
    interact's answer with `walk` (go_to_actor's); when the walk or the turn did not get there, `result` says why and
    nothing is pressed."""
    g = go_to_actor(client, ident, name, near_m, max_frames)
    if not g["arrived"]:
        return {"result": "not reached: %s" % g["reason"], "pressed": False, "walk": g}
    f = face(client, ident, name or (g.get("actor") or {}).get("name"))
    if not f["faced"]:
        return {"result": "not faced: %s" % f["reason"], "pressed": False, "walk": g}
    return dict(interact(client, "interact with"), walk=g)


def _death(client, line, name, ident, out, death_frames=None):
    """A `kill` that struck is `ok` only when the actor is seen dead (gone from the list, not alive, or life 0) within
    DEATH_FRAMES more frames: the strike is the hit, the death comes frames later (and, for an actor far from Link, may
    not come: S8). Otherwise `struck: ...` with `died` false (it lives) or null (not checked: no id, or no world list);
    `died_after` is the frames waited. `death_frames` (the `kill` tool's `wait_frames`) replaces DEATH_FRAMES."""
    limit = DEATH_FRAMES if death_frames is None else int(death_frames)
    if not ident:
        return dict(out, result="struck: the hit landed (a kill without an id: its death is not checked; look with "
                                "get_world)", died=None)
    waited = 0
    while True:
        a, w = _look(client, ident)
        why = _no_list(w)
        if why:
            return dict(out, result="struck: the hit landed, but its death could not be checked (%s)" % why, died=None)
        life = (a or {}).get("life")
        if _gone(a, name) or a.get("alive") is False or (isinstance(life, (int, float)) and life <= 0):
            return dict(out, died=True, died_after=waited, frames=out["frames"] + waited)
        if waited >= limit:
            return dict(out, result="struck: the hit landed, but %s still lives after %d frames (a far actor may not "
                                    "die until Link is near it: go near it and look with get_world)" % (name, waited),
                        died=False, died_after=waited, frames=out["frames"] + waited)
        look = min(DEATH_LOOK, limit - waited)      # exactly `limit` frames in all, not a block of DEATH_LOOK over
        client.step(look)
        waited += look


def strike(client, line, name, ident=None, frames=STRIKE_FRAMES, death_frames=None):
    """Send one `kill [ID] NAME` or `hit [ID] NAME N` line (built and checked by mcp_playtest.act_line) and wait up to
    `frames` frames, looking every STRIKE_LOOK, for the objects walk's result in `state.strike`, matched by the strike's
    number (no retry, no other actor). `ident` (the id in the line) is for the words and, for a `kill`, to see the actor
    die. Replies result: `ok` (a `hit`: the walk struck it; a `kill`: it struck and the actor was then seen dead, `died`
    true), `struck: ...` (a kill that struck but the actor was not seen dead: `died` false, or null when not checked), `refused: actor 7 (NAME) is gone`, `refused: actor 7 (NAME) has no life left`, `refused: actor 7 (NAME) is
    here, but co-op does not share it ...`, `refused: no world list ...` (solo refuses an id), `refused: the strike queue
    is full ...`, or `unknown: no result after 12 frames` (the strike may still land: look with get_world)."""
    who = "%d " % ident if ident else ""
    label = "actor %s (%s)" % (who.strip(), name) if who else "an actor named %s" % name
    try:
        r = client.act(line)
    except pc.BridgeError as e:
        why = (e.reply or {}).get("message") or str(e)
        return {"result": "refused: " + why, "line": line}
    seq = r.get("strike_seq")
    if not seq:
        return {"result": "unknown: the bridge gave no strike number (the module is older than the strike numbers; "
                           "look with get_world)", "line": line}
    used, last = 0, None
    while True:
        last = client.state().get("strike")
        if isinstance(last, dict) and last.get("seq", 0) >= seq:
            break
        if used >= frames:
            return {"result": "unknown: no result after %d frames (the strike may still land: look with get_world)"
                    % frames, "line": line, "seq": seq}
        client.step(STRIKE_LOOK)
        used += STRIKE_LOOK
    if last.get("seq") != seq:
        return {"result": "unknown: a newer strike replaced the result", "line": line, "seq": seq}
    word = last.get("result")
    if word == "done":
        out = {"result": "ok", "line": line, "seq": seq, "frames": used}
        return _death(client, line, name, ident, out, death_frames) if line.startswith("kill") else out
    if word == "gone":
        return {"result": "refused: %s is gone" % label, "line": line, "seq": seq, "frames": used}
    if word == "not shared":
        return {"result": "refused: %s is here, but co-op does not share it, so a test strike cannot reach it" % label,
                "line": line, "seq": seq, "frames": used}
    if word == "none with a life":
        return {"result": "refused: %s has no life left" % label if who else "refused: none of that name has a life",
                "line": line, "seq": seq, "frames": used}
    return {"result": "unknown: the walk said %r" % word, "line": line, "seq": seq, "frames": used}


# --- talk: read each text box, press A to the end, pick a reply by its text (docs/playtest.md, "Text boxes") ---------
# The talk ends when the event has ended and no new text came; the cursor is taken to start on the first button.
# Read in a lane run (Kokiri shop, Mido; game 0001-0041): a talk is `MessageDialog_00` (T_Name_00 and T_NameNavi_00 the
# speaker), its reply buttons `PaMessageBtn_00` (ONE pane, T_Btn_00, set once per button). The box's body did not come
# through the wrap there (another setter): BOX_PANES are kept for when it does. Patch 0042 wraps the letter-animated
# pane's set-text (BODY_WRAP, read from main only): a text of a message layout from it is a box, whatever its pane.
MESSAGE_LAYOUTS = ("MessageInGame", "BalloonMessage", "MessageDialog", "MessageLog")
CHOICE_LAYOUTS = ("PaMessageBtn",)
BOX_PANES = ("T_MessageWhiteOut_00", "T_Message_00")
NAME_PANES = ("T_Name_00", "T_NameNavi_00")
BODY_WRAP = 26         # a text's `wrap` from patch 0042's letter-animated pane setter
TALK_BOXES = 20        # a talk reads at most this many boxes
TALK_FRAMES = 1200     # ... and gives up after this many frames with nothing new (no box, name or buttons)
TALK_TOTAL = 7200      # ... and never runs more than this many frames in all
TALK_WAIT = 10         # frames run after each press before the texts are read
TALK_IDLE = 3          # reads in a row with no event and no new text: the talk is over (with an event: press A)
TEXT_NOW = 2 ** 53 - 1  # `text` since this: no texts, only `last` and `hooked`
GONE_READS = 3         # reads in a row without the actor before go_to_actor says `gone` (one read can miss it)


def _message_text(t):
    return any(name in (t.get("layout") or "") for name in MESSAGE_LAYOUTS + CHOICE_LAYOUTS)


def _choice_text(t):
    return any(name in (t.get("layout") or "") for name in CHOICE_LAYOUTS)


PAGE_BREAK = (0, 4)  # the markup tag (group, type) that starts a new page of the same text
VARIABLE_GROUP = 2   # the markup tag group of a variable (an item name, a price, an actor name)


def _param_chars(text, start, size):
    """How many characters of `text` from `start` are `size` bytes of tag parameters, or None when the text ends
    first or a character straddles the end. The bridge sends valid UTF-8 as characters and any other byte as one
    character below U+0100, so a character from U+0100 up is 2 to 4 bytes and a lower one is counted as one."""
    used = 0
    for k in range(start, len(text)):
        if used == size:
            return k - start
        used += len(text[k].encode("utf-8", "surrogatepass")) if ord(text[k]) > 255 else 1
        if used > size:
            return None
    return len(text) - start if used == size else None


def strip_markup(text, variables=True):
    """The words of a box text without its markup. The game's text holds tags as bytes: 0x0e, then group, type and
    size (16-bit little-endian each), then `size` bytes of parameters (a name such as an animation or a variable, a
    pause, a colour). A tag cut short, or one that names more bytes than there are, loses only its first byte. A
    page break becomes one space. A variable (an item name, a price) stays as `{Name}` unless `variables` is false:
    the bridge's text has only its name, the game fills in the value later."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == "\x0e":
            if i + 7 <= n and ord(text[i + 5]) < 256 and ord(text[i + 6]) < 256:
                size = ord(text[i + 5]) | ord(text[i + 6]) << 8
                count = _param_chars(text, i + 7, size)
                if count is not None:
                    group = ord(text[i + 1]) | ord(text[i + 2]) << 8
                    kind = ord(text[i + 3]) | ord(text[i + 4]) << 8
                    if (group, kind) == PAGE_BREAK and out and not out[-1].isspace():
                        out.append(" ")  # a page break ends a sentence: its two sides are not one word
                    elif group == VARIABLE_GROUP and variables:
                        found = re.match(r"[A-Za-z0-9_]+", text[i + 9:i + 7 + count])  # (after the 2-byte name length)
                        if found:
                            out.append("{" + found.group(0) + "}")
                    i += 7 + count
                    continue
            i += 1
            continue
        out.append(c)
        i += 1
    return "".join(out).strip()


def _same_reply(text, reply):
    return " ".join(str(text).split()).casefold() == " ".join(str(reply).split()).casefold()


def _has_reply(text, reply):
    return " ".join(str(reply).split()).casefold() in " ".join(str(text).split()).casefold()


def talk(client, ident=None, name=None, reply=None, max_boxes=TALK_BOXES, max_frames=TALK_FRAMES,
         near_m=INTERACT_M, wait=TALK_WAIT, since=None, max_total=TALK_TOTAL):
    """Talk to whoever the A prompt names, or (with `ident` and `name` from get_world, when no prompt shows yet) walk
    to that actor and face it first. Presses A, then reads each new text (the bridge's `text`): the speaker, the boxes,
    the reply buttons. Presses A to go on, and when buttons show picks the next of `reply` (one text, or a list: one
    per set of buttons, in order) by Down and A. With no reply left it stops and lists the buttons, the talk left open:
    call again with `reply` and `since` (this answer's `since`) to go on from there, no new A first.
    Stops when the talk is over (`done`), or says why: not reached, no prompt, no text (this exe lacks patch 0041),
    choices and no reply, reply not offered, too many boxes, timed out, scene changed, loading.
    Returns result, done, speakers (in order), boxes [{speaker, text, markup, raw}], choices (the last seen), picked
    [{text, index}], since, presses, frames. A box's `text` is its words without the markup; `raw` (only when markup
    was cut) is the text as the game set it. A box set twice in a row is one box; a text of only markup is no box.
    `speaker` is the name pane shown with the box (None when the game shows none). `max_frames` is the frames the
    talk waits with nothing new (a box, a name, buttons): it keeps going while boxes keep coming, up to `max_total`."""
    replies = [] if reply is None else [reply] if isinstance(reply, str) else list(reply)
    out = {"result": None, "pressed": False, "done": False, "speakers": [], "boxes": [], "choices": [], "picked": [],
           "since": None, "presses": 0, "frames": 0}
    try:
        base = client.text(TEXT_NOW)
        hooked = bool(base.get("hooked"))
        seen = base.get("last", 0) if since is None else int(since)
    except pc.BridgeError as e:
        hooked, seen = None, 0
        why = "no text: the bridge has no `text` verb (%s)" % e.code
    if hooked is False:
        why = "no text: this Hookshot has no pane text wrap (patch 0041)"
    st = pc.game_of(client.state())
    if since is not None and hooked and st.get("event"):
        out["frames"] = 0                       # (going on with a talk that is open: no first A)
    else:
        if ident is not None and no_prompt(st.get("prompt")):
            start = interact_with(client, ident, name, near_m)
            out["walk"] = start.get("walk")
        else:
            start = interact(client, "talk")
        out["pressed"] = bool(start.get("pressed"))
        if not start.get("pressed"):
            out["result"] = start["result"]
            return out
        out["presses"], out["frames"] = 1, start.get("frames", 2 * PRESS_FRAMES)
        if not hooked:
            out["result"] = why + "; one A pressed"
            return out
    scene = pc.game_of(client.state()).get("scene")
    speaker, idle, quiet, fresh = None, 0, 0, False
    moved = out["frames"]                       # (the frame count when something new last came)
    while True:
        client.step(wait)
        out["frames"] += wait
        before = seen
        r = client.text(seen)
        seen = r.get("last", seen)
        new = [t for t in r.get("texts", []) if _message_text(t)]
        choices, first = [], None
        for t in new:
            pane, text = t.get("pane") or "", t.get("text") or ""
            if _choice_text(t):
                if text:
                    choices.append(text)
                    first = t.get("seq", before + 1) if first is None else first
            elif pane in NAME_PANES:
                if text and text != speaker:
                    out["speakers"].append(text)
                    moved = out["frames"]
                speaker = text or None
                last = out["boxes"][-1] if out["boxes"] else None
                if speaker and last and not last["done"]:
                    last["speaker"] = speaker   # (the game sets the name after the body: it is this box's)
                fresh = True
            elif (pane in BOX_PANES or t.get("wrap") == BODY_WRAP) and text:
                last = out["boxes"][-1] if out["boxes"] else None
                words = strip_markup(text)
                has_words = bool(strip_markup(text, variables=False))
                if last and not last["done"] and text.startswith(last["_raw"]) and text != last["_raw"]:
                    last.update(_raw=text, text=words)   # (the same box, grown)
                    moved = out["frames"]
                elif last and not last["done"] and text == last["_raw"]:
                    pass                                   # (the same box set again, before any A)
                elif has_words:
                    out["boxes"].append({"speaker": speaker, "text": words, "_raw": text, "done": False,
                                         "markup": bool(t.get("markup")) or words != text.strip()})
                    moved = out["frames"]
                fresh = True
        if choices:
            if choices != out["choices"]:
                moved = out["frames"]
            out["choices"] = choices
            fresh = True
        st = pc.game_of(client.state())
        if st.get("scene") != scene:
            out["result"] = "scene changed"
            break
        if st.get("loading"):
            out["result"] = "loading"
            break
        idle = idle + 1 if not new and not st.get("event") else 0
        quiet = 0 if new else quiet + 1
        if idle >= TALK_IDLE:
            out["result"], out["done"] = "done: the event ended", True
            break
        if len(out["boxes"]) > max_boxes:
            out["result"] = "too many boxes (%d)" % max_boxes
            break
        if out["frames"] - moved >= max_frames:
            out["result"] = "timed out after %d frames with nothing new" % (out["frames"] - moved)
            break
        if out["frames"] >= max_total:
            out["result"] = "timed out: more than %d frames in all" % max_total
            break
        if choices:
            out["since"] = first - 1           # (a call with this `since` reads these buttons again)
            listed = ", ".join(map(repr, choices))
            if len(out["picked"]) >= len(replies):
                out["result"] = "choices showed and no reply is left: %s (the talk is left open: talk again with " \
                                "`reply` and `since`)" % listed
                break
            want = replies[len(out["picked"])]
            at = next((i for i, c in enumerate(choices) if _same_reply(c, want)), None)
            if at is None:
                at = next((i for i, c in enumerate(choices) if _has_reply(c, want)), None)
            if at is None:
                out["result"] = "reply not offered: %r (choices: %s)" % (want, listed)
                break
            for _ in range(at):
                client.input(None, ["DOWN"], PRESS_FRAMES)
                client.step(PRESS_FRAMES)
                out["frames"] += 2 * PRESS_FRAMES
            out["picked"].append({"text": choices[at], "index": at})
        # A when a box, a speaker or the choices came since the last press, or to nudge an event that went quiet.
        if st.get("event") and (fresh or quiet >= TALK_IDLE):
            client.input(None, ["A"], PRESS_FRAMES)
            out["presses"] += 1
            out["frames"] += PRESS_FRAMES
            if out["boxes"]:
                out["boxes"][-1]["done"] = True
            fresh, quiet = False, 0
    for b in out["boxes"]:
        b.pop("done", None)
        raw = b.pop("_raw")
        if raw.strip() != b["text"]:   # (whitespace at the edges alone is not markup)
            b["raw"] = raw
    if out["done"] and len(out["picked"]) < len(replies):
        out["result"] = "done, but no choices showed for %s" % ", ".join(map(repr, replies[len(out["picked"]):]))
        out["done"] = False
    return out
