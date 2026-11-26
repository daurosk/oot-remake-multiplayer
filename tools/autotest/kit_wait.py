"""`wait_until`: step the game in short chunks until a named thing happens, stopping early on trouble (hurt, death, an
event, a scene change). Standard library only; the caller adds the `look`.

    wait_until(client, "enemy_dead", id=3)  ->  (reply, last state)
"""
import kit_tools

WHATS = ("event_over", "scene_changed", "enemy_dead", "enemies_clear", "on_ground", "prompt", "condition")
CHUNK = 8
MAX_FRAMES = 1800
STILL_M = 0.02      # on_ground: height change per look at most this, twice in a row


def _actors(state):
    """The world's actor list, or None when it cannot be trusted: no list, a stale one (a load, or the session ended),
    or an empty one that comes with a `why` (no co-op session). Empty without a `why` is a room with nothing in it."""
    w = state.get("world")
    if not isinstance(w, dict) or w.get("stale"):
        return None
    actors = w.get("actors") or []
    if not actors and w.get("why"):
        return None
    return actors


def _no_list(state):
    w = state.get("world")
    return (w.get("why") if isinstance(w, dict) else None) or "no world list"


def _enemy_dead(state, ident, seen):
    """(met, why): the enemy is listed dead, or no longer listed after it was seen (it may be out of range, not dead:
    `why` says "gone" then). No list, a stale list, or an id never seen, is "do not know"."""
    actors = _actors(state)
    if actors is None:
        return False, _no_list(state)
    for a in actors:
        if a.get("id") == ident:
            seen.add(ident)
            return a.get("alive") is False or a.get("life") == 0, None
    if ident not in seen:
        return False, "enemy %s was never listed" % ident
    return True, "gone from the list (killed, or out of the 40 nearest: check the look)"


def wait_until(client, what, max_frames=300, chunk=CHUNK, ident=None, condition=None):
    """Returns (reply, state). reply: `met`, `what`, `frames_run`, `stopped` (None when met or timed out; else the
    trouble: hurt, dead, event, scene_changed, loading) and `why` when not met."""
    if what not in WHATS:
        raise ValueError("what: one of %s" % ", ".join(WHATS))
    max_frames, chunk = int(max_frames), int(chunk)
    if not 1 <= max_frames <= MAX_FRAMES:
        raise ValueError("max_frames is 1 to %d" % MAX_FRAMES)
    if not 1 <= chunk <= kit_tools.MAX_CHUNK:
        raise ValueError("chunk is 1 to %d" % kit_tools.MAX_CHUNK)
    if what == "enemy_dead":
        try:
            ident = int(ident)
        except (TypeError, ValueError):
            raise ValueError("enemy_dead needs `id`, the number look or get_world gives")
    cond = kit_tools.parse_condition(condition) if what == "condition" else None
    if what == "condition" and cond is None:
        raise ValueError("condition needs `condition`")
    first = client.state()
    state, used, still = first, 0, 0
    prev_y, seen = None, set()
    while True:
        met, why = False, None
        if what == "event_over":
            met = state.get("event") is False
        elif what == "scene_changed":
            met = state.get("scene") != first.get("scene")
        elif what == "enemy_dead":
            met, why = _enemy_dead(state, ident, seen)
        elif what == "enemies_clear":
            actors = _actors(state)
            if actors is None:
                why = _no_list(state)
            else:
                met = not any(a.get("kind") == "enemy" and a.get("alive") is not False
                              and (a.get("dist") is not None and a["dist"] <= 10) for a in actors)
        elif what == "on_ground":
            pos = state.get("pos")
            y = pos[1] if isinstance(pos, list) and len(pos) == 3 else None
            if y is not None and prev_y is not None and abs(y - prev_y) <= STILL_M:
                still += 1
            else:
                still = 0
            prev_y = y
            met = still >= 2
        elif what == "prompt":
            met = bool(state.get("prompt"))
        else:
            met, _, why = kit_tools.evaluate(cond, state)
        out = {"met": met, "what": what, "frames_run": used, "stopped": None}
        if met:
            if why:
                out["note"] = why
            return out, state
        trouble = _trouble(first, state, what)
        if trouble:
            out["stopped"] = trouble
            out["why"] = "stopped early: " + trouble
            return out, state
        if used >= max_frames:
            out["why"] = "not met after %d frames%s" % (used, " (%s)" % why if why else "")
            return out, state
        n = min(chunk, max_frames - used)
        client.step(n)
        used += n
        state = client.state()


def _trouble(first, now, what):
    """Why a wait should end before its time, or None."""
    life, was = now.get("life"), first.get("life")
    if isinstance(life, (int, float)) and life <= 0:
        return "dead"
    if isinstance(life, (int, float)) and isinstance(was, (int, float)) and life < was:
        return "hurt (life %s -> %s)" % (was, life)
    if now.get("loading") and what != "scene_changed":
        return "loading"
    if now.get("scene") != first.get("scene") and what != "scene_changed":
        return "scene_changed"
    if now.get("event") and not first.get("event") and what not in ("event_over", "condition"):
        return "event"
    return None
