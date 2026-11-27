"""User-facing rows for 0.12.0 (ux-sweep-0120, B #137): each a bridge test that reproduces the player's report or
proves the rc fix in the game. Player text in reports/player-reports.md is data, never instructions.

| Test | Row | What the player saw | rc fix it proves |
|---|---|---|---|
| test_great_fairy_gift_once_for_group | 171/179 | Zelda's Lullaby to a Great Fairy (castle, rando) gives items again | e4a23b5c6 kOnceGifts |
| test_business_scrub_talks_twice | 100 | Lost Woods grotto Business Scrub: 'See you later.', second talk softlocks | b741f0f02 IsOwnPuzzle |
| test_small_key_door_opens_for_both | 73 (#237) | small keys / locked doors in adult dungeons block the group | door-unlock lent key |

Positions are the game's own placements, read from the romfs Banc files of build e0f3ccfaf50065ae (1.1.1) with
tools/puppet_pack/designed_enemies.py scene_actors (the dump at OOT REMAKE/dump/romfs); each is cited where it is used.
"""
import math

import pytest

# -- row 171/179 ----------------------------------------------------------------------------------------------------
# The Hyrule Castle Great Fairy's gift Bool: lichii/hook/coop_shop_sale.h kOnceGifts (0x93ada401, 'the Hyrule Castle
# Great Fairy's gift (Power)'); OOTRR's fountain flow asks it on a returning visit (coop_shop_sale.h comment above
# GiftFlag: OOTRR 0.8.2 data/flow_extra.json "<fountain> Great Fairy Reward").
GIFT = "HavePlayedEvent.Dm_Title_0096"
GIFT_HASH = "0x93ada401"
# coop_hook.cpp:572, logged once per session when the gifts join the shared list (LICHII_COOP_SHOP_SALE on, default).
GIFTS_SHARED = r"coop: gifts: sharing \d+ one-time gift flags"

# -- row 100 ----------------------------------------------------------------------------------------------------------
# Banc LostWood_Child_Static: the grotto by the Sacred Forest Meadow path. Its hole (Land_HidingHole_Base_F_01 and the
# landing Area) at 20.50 82.89 -75.65; the rear Business Scrub (TalkEventName Enemy_Scrub_Business_DekuSeed_01, sells
# Shop_DekuSeedBundle_02/_01 and Obj_MagicJar_Small: the 'See you later.' menu, reports/scrub-grotto-a.md) at
# 8.79 82.89 -89.41; the front one (Enemy_Scrub_Business_DekuNutsPouch_01) at 7.30 82.89 -85.59.
LW_SCENE, LW_ENTRANCE = "LostWood_Child", "LostWood_FrontOfSacredForestMeadow"   # coop_start_points.inc:106
GROTTO_Y = 82.89
REAR_SCRUB = (8.79, -89.41)
HOLE = (20.50, -75.65)
SCRUB = "Enemy_Scrub_Business"

# -- row 73 -----------------------------------------------------------------------------------------------------------
# Banc ForestTemple_Static: the first locked door, between the entrance hall (room 0) and the main hall (room 1): its
# two locks Obj_DoorChainLock_A_01 at -0.04 103.03 44.59 (DefaultDungeonRoomIndex 0, our side) and 44.39 (room 1).
# Start point ForestTemple at 0.03 101.54 78.42 (coop_start_points.inc:23, the entrance hall's floor height).
FT_SCENE = "ForestTemple"
LOCK = "Obj_DoorChainLock_A_01"
LOCK_AT = (-0.04, 44.59)
FT_FLOOR = 101.54
# The dungeon's shared small key count: lichii/hook/coop_world.cpp KeyCountOf ('DungeonInfo.<dungeon>.SmallKeyNum').
KEYS = "DungeonInfo.ForestTemple.SmallKeyNum"


def in_play(pair, age, what="both games in play after the warp"):
    """Both games in play after a warp: stepped to a fresh in-play sample (pair.until_in_play, never one state read:
    gifts-237 SETUP-FAIL), then of `age`. The tests' FIRST set-up is pair.need_boot_place(age) instead."""
    pair.until_in_play(what)
    for x in ("A", "B"):
        a = pair.state(x).get("age")
        assert a == age, "%s is %s: this row needs %s Link" % (x, a, age)


def value(pair, x, name):
    v = pair.flag_read(x, name).get("value")
    return 1 if v is True else 0 if v in (False, None) else v


def actors(pair, x, name, near=None, within=4.0):
    """World-list actors of game x called `name` (docs/playtest.md "State": world), within `within` m of near (x, z)."""
    w = pair.state(x).get("world") or {}
    out = []
    for a in w.get("actors") or []:
        if (a.get("name") or "") != name or not a.get("pos"):
            continue
        if near and math.dist((a["pos"][0], a["pos"][2]), near) > within:
            continue
        out.append(a)
    return out


def yaw_to(frm, to):
    """The world yaw from (x, z) frm to to: 0 toward +z, 90 toward +x (docs/playtest.md "Walking": atan2(dx, dz))."""
    return math.degrees(math.atan2(to[0] - frm[0], to[1] - frm[1])) % 360


def toward(p, q, metres):
    d = math.dist(p, q)
    return (p[0] + (q[0] - p[0]) * metres / d, p[1] + (q[1] - p[1]) * metres / d)


# ---------------------------------------------------------------------------------------------------------------------
@pytest.mark.budget(120)
@pytest.mark.row("rows 171/179 Great Fairy gift given once for the group (kOnceGifts, e4a23b5c6)")
def test_great_fairy_gift_once_for_group(pair):
    """Rows 171/179: before e4a23b5c6 the gift Bool was each game's own, so every other player's fountain took its
    next visit as a returning one with the gift not given and gave it again. Now it is shared by the pickup rule
    (coop_shop_sale.h: 0 -> 1 goes out, a 1 never back to 0)."""
    with pair.setup():
        pair.need_boot_place("child")
        for x in ("A", "B"):
            assert pair.log[x].find(GIFTS_SHARED, pos=0), "%s: no '%s' line: the gifts are not shared" % (x, GIFTS_SHARED)
            pair.flag(x, GIFT, 0)       # each game's own 0 (shared only 0 -> 1)
        pair.step_both(30)
        for x in ("A", "B"):
            assert value(pair, x, GIFT) == 0, "%s: %s did not go back to 0" % (x, GIFT)
    pair.flag("A", GIFT, 1)              # A's fountain gave the gift (OOTRR's flow sets the Bool right after it)
    pair.until("B has %s = 1 (the gift is the group's)" % GIFT, lambda: value(pair, "B", GIFT) == 1, max_frames=300)
    pair.flag("B", GIFT, 0)              # a 0 in one game never takes the group's 1 away
    pair.step_both(120)
    assert value(pair, "A", GIFT) == 1, "A: %s went back to 0 after B's 0: the gift would be given again" % GIFT
    pair.notes.append("gift %s (%s) reached B; B's 0 did not reach A" % (GIFT, GIFT_HASH))


# ---------------------------------------------------------------------------------------------------------------------
def talk_until_over(pair, x, what, max_frames=1500):
    """Game x talks (A) and goes on pressing A every 20 frames until the talk event is over (state 'event' false
    after it was true). Returns the frames it took; FAIL when the event never ends (the softlock)."""
    def started():
        pair.press(x, ["A"], frames=4)
        return pair.state(x).get("event")
    pair.until("%s: %s starts" % (x, what), started, max_frames=240)
    spent = 0
    while spent < max_frames:
        pair.press(x, ["A"], frames=4)
        pair.step_both(16)
        spent += 20
        if not pair.state(x).get("event"):
            return spent
    raise AssertionError("%s: %s never ended in %d frames (row 100: the second talk softlocks)" % (x, what, max_frames))


@pytest.mark.budget(240)
@pytest.mark.row("row 100 Lost Woods grotto Business Scrub talks twice (b741f0f02, #253)")
def test_business_scrub_talks_twice(pair):
    """Row 100 (Chu!, 0.11.2 + OOTRR 0.6.0): the rear Business Scrub showed only 'See you later.' (the game's own rule
    with no slingshot and no magic: reports/scrub-grotto-a.md) and a second talk softlocked. Co-op cause fixed in
    b741f0f02: as a shared enemy the owner's place and animations were played over the scrub our Link talked to;
    Enemy_Scrub_Business* are each game's own now (CoopObjects::IsOwnPuzzle). B is in the grotto first (B would be
    the owner under the old rule), then A beats the rear scrub and talks to it twice."""
    spot = toward(REAR_SCRUB, HOLE, 3.0)        # 3 m in front of the rear scrub, on the line to the hole
    spot_b = toward(REAR_SCRUB, HOLE, 6.0)
    with pair.setup():
        pair.need_boot_place("child")
        pair.warp_both(LW_SCENE, LW_ENTRANCE)
        for x, (px, pz) in (("B", spot_b), ("A", spot)):
            pair.act(x, "place %.2f %.2f %.2f" % (px, GROTTO_Y + 0.3, pz))
            pair.act(x, "face %.2f %.2f" % REAR_SCRUB)
            pair.step_both(30)
        for x in ("A", "B"):
            s = pair.state(x)
            assert abs(s["pos"][1] - GROTTO_Y) < 1.5, "%s: not on the grotto floor (y %.2f)" % (x, s["pos"][1])
        pair.step_both(60)
        rear = {x: actors(pair, x, SCRUB, REAR_SCRUB, 2.0) for x in ("A", "B")}
        for x in ("A", "B"):
            assert rear[x], "%s: no %s at the rear spot %s in the world list" % (x, SCRUB, REAR_SCRUB)
    for x in ("A", "B"):        # each game's own shop: the sharing does not know it (docs/playtest.md: owner null)
        owners = sorted({str(a.get("owner")) for a in actors(pair, x, SCRUB, REAR_SCRUB, 8.0)})
        assert owners == ["None"], "%s: Business Scrubs have owner %s: shared copies (row 100's cause)" % (x, owners)
    b_before = rear["B"][0]["pos"]
    # The beating: 'hit ACTOR N' by name (coop_testinput.cpp:557-566, 'hit [ID] ACTOR N'). No id: an id strike resolves
    # in the sharing walk only (docs/playtest.md "State": strike; coop_playtest_core.cpp:735) and these scrubs are
    # each game's own. Nearest of that name = the rear one (A stands 3.0 m from it, 3.76 m from the front one).
    pair.act("A", "hit %s 1" % SCRUB)
    def struck():
        pair.step_both(2)
        return (pair.state("A").get("strike") or {}).get("result")
    res = pair.until("A: the strike's answer", struck, max_frames=120)
    assert res == "done", "A: the hit on the rear scrub answered %r (docs/playtest.md strike)" % res
    def talk_offered():
        pair.step_both(8)
        return pair.state("A").get("prompt") == "Talk" or pair.state("A").get("event")
    try:
        pair.until("A: the rear scrub offers a talk", talk_offered, max_frames=900)
    except AssertionError:
        pair.unproven("the rear Business Scrub never offered a talk after a bridge hit (prompt %r)"
                      % pair.state("A").get("prompt"))
    first = talk_until_over(pair, "A", "the first talk")
    pair.act("A", "face %.2f %.2f" % REAR_SCRUB)
    pair.step_both(30)
    second = talk_until_over(pair, "A", "the second talk")
    pair.press("A", stick=[180, 1.0], frames=30)        # Link can walk again after it
    b_after = actors(pair, "B", SCRUB, REAR_SCRUB, 4.0)
    assert b_after, "B: its rear scrub is gone from its world list after A's talks"
    moved = math.dist(b_before[::2], b_after[0]["pos"][::2])
    assert moved < 1.0, "B: its rear scrub moved %.2f m while A talked to A's (A's scrub was played in B)" % moved
    pair.notes.append("talks ended after %d and %d frames; B's scrub moved %.2f m" % (first, second, moved))


# ---------------------------------------------------------------------------------------------------------------------
def open_lock(pair, x, max_frames=900):
    """Game x leans into the locked door (move toward the lock) pressing A until its lock is gone from x's world list
    (the lock's AI spends the key and deletes itself: docs/dungeon.md "Locked doors"). Returns frames, or None."""
    spent = 0
    while spent < max_frames:
        if not actors(pair, x, LOCK, LOCK_AT, 1.0):
            return spent
        here = pair.state(x)["pos"]
        pair.call(x, "move", yaw=yaw_to((here[0], here[2]), LOCK_AT), strength=0.3, frames=8)
        pair.step_other("B" if x == "A" else "A", 8)
        pair.press(x, ["A"], frames=4)
        spent += 12
    return None


@pytest.mark.budget(300)
@pytest.mark.needs_boot("live-twinrova-room")
@pytest.mark.row("row 73 / #237 small-key door opened by one player opens for the other (adult, Forest Temple)")
def test_small_key_door_opens_for_both(pair):
    """Row 73 (Revirie, 0.11.0): small keys in adult dungeons were the main blocker; #237 (known issue): a locked door
    another player opened may stay shut. The group has one key; A opens the Forest Temple's first locked door; B
    comes to the same door: it must open for B too (the lent key, docs/dungeon.md "Locked doors"), and the group
    must end with 0 keys (one key per door). Adult boot (live-twinrova-room: adult save), then the warp in."""
    with pair.setup():
        pair.warp_both(FT_SCENE, FT_SCENE)
        in_play(pair, "adult")
        # SmallKeyNum is a counted item (game_items.cpp:199, shared as deltas): the two games hold the same count, and
        # only A writes it (a write in both would send the change twice).
        k = {x: value(pair, x, KEYS) for x in ("A", "B")}
        assert k["A"] == k["B"], "the games' Forest Temple keys differ at the start: %s" % k
        pair.flag("A", KEYS, 0)
        pair.until("B: keys 0", lambda: value(pair, "B", KEYS) == 0, max_frames=300)
        missing = [x for x in ("A", "B") if not actors(pair, x, LOCK, LOCK_AT, 1.0)]
        pair.act("A", "place %.2f %.2f %.2f" % (LOCK_AT[0], FT_FLOOR + 0.1, LOCK_AT[1] + 2.0))
        pair.act("B", "place %.2f %.2f %.2f" % (LOCK_AT[0], FT_FLOOR + 0.1, LOCK_AT[1] + 7.0))
        for x in ("A", "B"):
            pair.act(x, "face %.2f %.2f" % LOCK_AT)
        pair.step_both(30)
        pair.flag("A", KEYS, 1)
        pair.until("B has the group's key", lambda: value(pair, "B", KEYS) == 1, max_frames=300)
    if missing:         # outside the set-up: a NO-REF, not a SETUP-FAIL
        pair.unproven("%s: no %s at the first door %s: the boot save has it open" % (missing, LOCK, LOCK_AT))
    a = open_lock(pair, "A")
    assert a is not None, "A: the lock did not open with the group's key (keys A %s)" % value(pair, "A", KEYS)
    pair.until("B: the key spent for the group", lambda: value(pair, "B", KEYS) == 0, max_frames=300)
    pair.act("B", "place %.2f %.2f %.2f" % (LOCK_AT[0] + 0.8, FT_FLOOR + 0.1, LOCK_AT[1] + 2.0))
    pair.act("B", "face %.2f %.2f" % LOCK_AT)
    pair.step_both(30)
    b = open_lock(pair, "B")
    assert b is not None, ("B: the door A opened stays locked for B (#237, row 73): lock still in B's world list, "
                           "keys A %s B %s" % (value(pair, "A", KEYS), value(pair, "B", KEYS)))
    pair.step_both(240)                     # a lent key that reached the group would show here (docs/dungeon.md)
    keys = {x: value(pair, x, KEYS) for x in ("A", "B")}
    assert keys == {"A": 0, "B": 0}, "the group's keys after one door: %s (one key per door)" % keys
    pair.notes.append("A opened in %d frames, B in %d; keys %s" % (a, b, keys))


# ---------------------------------------------------------------------------------------------------------------------
# Row 283: the frame windows and the pass lines are row 83's (test_unfocus.py: Longshot's own 5 s 'vi:' windows,
# nn_vi.c:704/705; mean >= 90 % of the 60 cap, presents over 22 ms <= 5 %), imported, never copied.
from test_unfocus import CAP_FPS, MAX_LATE, MIN_SHARE, POLL_S, frame_rate_cap, windows   # noqa: E402

TR_STAGE = "TwinrovaStage"                     # coop_start_points.inc {"TwinrovaStage", "TwinrovaStage", ...}
TR_PARTS = ("Enemy_DungeonBoss_Koume", "Enemy_DungeonBoss_Kotake", "Enemy_DungeonBoss_Twinrova")   # coop_boss.cpp:41
FINAL_WINDOWS = 3                              # 15 s of play from the final blow on: the boss's death and its event


def judge_windows(x, got):
    fps = [w[0] for w in got]
    late = sum(sum(w[2][3:]) for w in got)
    total = sum(sum(w[2]) for w in got) or 1
    mean = sum(fps) / len(fps)
    return mean, late / total, "%s: fps %s, late %.1f %%" % (x, "/".join("%.1f" % f for f in fps), 100.0 * late / total)


@pytest.mark.budget(240)
@pytest.mark.needs_boot("live-twinrova-room")
@pytest.mark.row("row 283 no frame drops on a boss's final blow (host and guest)")
def test_boss_final_blow_frames(pair):
    """Row 283 (Sephiroth guest, PiscesGamer97 NVIDIA host): frame drops on boss final blows; an AMD host saw none.
    No logs came, so this measures it: Twinrova (the lane's boss boot), the host A strikes every part of the boss
    dead (the bridge's `kill`, cheats on in the boot), then both games run in real time and each game's 5 s present
    windows from the blow on must hold row 83's line. NO-REF unless every kill answered `done` and no part is alive
    after the windows (the blow is proven before it is judged). Kept: every window's fps and late share (notes).
    It leaves the boss dead for the rest of the live-twinrova-room boot: test_small_key_door (ForestTemple, own warp)
    does not need it, and this test needs a fresh boot (or runs first) to find the parts alive."""
    import time
    with pair.setup():
        for x in ("A", "B"):
            cap = frame_rate_cap(pair.games[x]["folder"])
            assert cap == "0", "%s: frame_rate_cap=%s (this row is the 60 cap: 0)" % (x, cap)
        pair.warp_both(TR_STAGE, TR_STAGE, arrive_in_event=True)
    pair.until("both games past the intro", lambda: all(pair.ready(x) for x in ("A", "B")), 3600)
    parts = [a for n in TR_PARTS for a in actors(pair, "A", n) if a.get("alive", True)]
    if not parts:
        pair.unproven("no Twinrova part alive in A's world list after the intro")
    marks = {x: pair.log[x].mark() for x in ("A", "B")}
    for a in parts:
        # 'kill [ID] ACTOR' (coop_testinput.cpp:523-531); one at a time: state.strike keeps only the last strike
        # (docs/playtest.md "strike"), so each answer is read before the next kill is sent.
        seq = (pair.act("A", "kill %s %s" % (a.get("id"), a["name"])) or {}).get("strike_seq")
        def answered():
            pair.step_both(2)
            st = pair.state("A").get("strike") or {}
            return st if st.get("name") == a["name"] and (seq is None or st.get("seq") == seq) else None
        st = pair.until("A: the kill of %s answered" % a["name"], answered, max_frames=120)
        if st.get("result") != "done":
            pair.unproven("A: the kill of %s %s answered %r: no final blow was struck" % (a.get("id"), a["name"],
                                                                                         st.get("result")))
    for x in ("A", "B"):
        pair.call(x, "resume")
    got = {}
    try:
        while True:
            got = {x: windows(pair.log[x].since(marks[x])) for x in ("A", "B")}
            if all(len(w) >= FINAL_WINDOWS for w in got.values()):
                break
            pair.deadline.check("%d vi windows per game after the blow (have %s)" % (
                FINAL_WINDOWS, ", ".join("%s %d" % (x, len(w)) for x, w in sorted(got.items()))))
            time.sleep(POLL_S)
    finally:
        for x in ("A", "B"):
            pair.call(x, "pause")
    # The death must be proven before the windows count as "after the final blow": a kill that did not land would
    # measure plain play and PASS without a blow (review-rows-c136). 15 s of play is far past the walk's 36-frame wait.
    left = [a["name"] for n in TR_PARTS for a in actors(pair, "A", n) if a.get("alive", True) and a.get("life", 1) != 0]
    if left:
        pair.unproven("boss parts still alive in A %d windows after the kills: %s (no final blow measured)"
                      % (FINAL_WINDOWS, left))
    pair.notes.append("boss parts alive in A after: none")
    bad = []
    for x in ("A", "B"):
        mean, late, note = judge_windows(x, got[x][:FINAL_WINDOWS])
        pair.notes.append(note)
        if mean < MIN_SHARE * CAP_FPS or late > MAX_LATE:
            bad.append(note)
    assert not bad, "frame drops after the final blow (row 283): %s" % "; ".join(bad)
