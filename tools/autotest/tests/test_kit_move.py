"""Tests for kit_move.py (shoot, climb, push, jump) against a small simulated game: no game, no bridge. Each test
builds the world the move meets (a ladder, a block, a gap, a target) and checks the result and the reason.
Run: python -I tools/autotest/tests/test_kit_move.py"""
import math
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import kit_move as km  # noqa: E402
import mcp_playtest  # noqa: E402


class Sim:
    """A held game: Link moves 0.15 m a frame at full stick along the world yaw, stands on `ground(x, z)` (None: a
    pit, which puts him back at `respawn` and takes 2 life unless he runs off the edge at full stick and it is at most
    `jump_m` wide). A ladder (`ladder`: x, z, bottom, top) holds him when he is within 0.6 m of it: the stick up climbs 0.05 m a
    frame. A block moves after Link leaned on it `lean` frames. The camera (`cam_yaw`, `cam_pitch`, from 1 m over
    Link) is in the state's `view` as the game's clip matrix (right-handed, y up); ZL puts it behind Link, drawing
    (ZR) turns Link to it, and while drawing the `aim_stick` ("right", "left" or None) turns it, the stick's up
    looking down (an inverted aim). A shot: ZR held 30 frames or more and let go spends one seed and, when the target
    `takes` shots and the reticle's line passes within 1 degree of it (4 with `aim`: the nudge), takes one life."""

    def __init__(self, pos=(0.0, 0.0, 0.0), ground=None, ladder=None, actors=None, seeds=5, item_owned=True):
        self.pos = list(pos)
        self.ground = ground or (lambda x, z: 0.0)
        self.ladder = ladder
        self.actors = actors or []
        self.inv = {"PouchData.Item_DekuSeed.StockNum": seeds}
        self.item_owned = item_owned
        self.frame, self.life, self.scene, self.event, self.prompt = 100, 12, "Ydan", False, None
        self.facing = 0.0
        self.respawn = list(pos)
        self.jump_m = 3.0
        self.zr = 0
        self.assigned = None
        self.assist = False
        self.lines = []
        self.lean = 0
        self.lean_needed = 20
        self.block_limit = None
        self.event_on_draw = False
        self.hurt_at = None
        self.no_list = False
        self.refuse_move = False   # every `move` answers no_camera and runs no frame
        self.refused = 0
        self.cam_yaw, self.cam_pitch = 0.0, 0.0
        self.aim_stick = "right"
        self.has_view = True
        self.grab_needed = False   # a block moves only after A was held 30 frames at it (prompt "Grab"); a tap climbs
        self.grabbed = False
        self.a_held = 0
        self.wrote_yaw = 0         # `face` lines seen (a written yaw; the kit must turn with the stick)
        self.drift = 0.0           # degrees a frame the view drifts while drawing with no stick (a wobble)
        self.dropped = 0           # grabs let go by a frame without A
        self.cam_keep = 0.0        # 0..1: the share of last frame's view turn kept (an eased camera that coasts on)
        self.cam_vel = [0.0, 0.0]

    # --- the bridge ---
    def state(self):
        return {"scene": self.scene, "pos": list(self.pos), "life": self.life, "max_life": 12, "event": self.event,
                "loading": False, "prompt": self.prompt, "inventory": dict(self.inv), "frame": self.frame,
                "facing": round(self.facing, 2), "view": self._view() if self.has_view else None}

    def _eye(self):
        return (self.pos[0], self.pos[1] + 1.0, self.pos[2])

    def _forward(self):
        y, p = math.radians(self.cam_yaw), math.radians(self.cam_pitch)
        return (math.sin(y) * math.cos(p), math.sin(p), math.cos(y) * math.cos(p))

    def _view(self):
        f = self._forward()
        y = math.radians(self.cam_yaw)
        r = (-math.cos(y), 0.0, math.sin(y))              # right-handed: +z ahead has -x on the right
        u = (r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0])
        e = self._eye()
        dot = lambda a, b: sum(a[i] * b[i] for i in range(3))
        sx, sy = 1.0 / math.tan(math.radians(35)), 1.0 / math.tan(math.radians(30))
        clip = [sx * r[0], sx * r[1], sx * r[2], -sx * dot(r, e),
                sy * u[0], sy * u[1], sy * u[2], -sy * dot(u, e),
                0.0, 0.0, 0.0, 0.0,
                f[0], f[1], f[2], -dot(f, e)]
        return {"yaw": round(self.cam_yaw, 2), "pitch": round(self.cam_pitch, 2), "clip": clip, "y_down": False}

    def _look(self, stick):
        x = y = 0.0
        if stick:
            angle, strength = stick
            x, y = math.sin(math.radians(angle)) * strength, math.cos(math.radians(angle)) * strength
        x = 0.0 if abs(x) < 0.1 else x                    # a dead zone
        y = 0.0 if abs(y) < 0.1 else y
        k = self.cam_keep                                 # the turn speed eases in and coasts on after the stick
        self.cam_vel = [k * self.cam_vel[0] + (1 - k) * 1.5 * x, k * self.cam_vel[1] + (1 - k) * 1.0 * y]
        self.cam_yaw = (self.cam_yaw - self.cam_vel[0]) % 360   # right turns right: the yaw goes down (right-handed)
        self.cam_pitch = max(-80.0, min(80.0, self.cam_pitch - self.cam_vel[1]))

    def world(self):
        if self.no_list:
            return {"frame": self.frame, "world": {"actors": [], "why": "no session (solo)"}}
        return {"frame": self.frame, "world": {"frame": self.frame, "actors": [dict(a, pos=list(a["pos"])) for a in self.actors]}}

    def act(self, line):
        self.lines.append(line)
        word = line.split()
        if word[0] == "assign":
            self.assigned = word[1]
        elif word[0] == "face":
            self.wrote_yaw += 1           # a written yaw: no player can do it, and it turns nothing here
        elif word[0] == "aim":
            self.assist = True
        return {"ok": True}

    def step(self, frames=1):
        self._frames(frames)
        return {"ok": True, "state": self.state()}

    def input(self, stick=None, buttons=(), frames=1, rstick=None):
        for _ in range(frames):
            if "ZL" in buttons:
                self.cam_yaw, self.cam_pitch = self.facing, 0.0
            if "B" in buttons and self.zr:
                self.zr = -1                              # put away: no shot on the let-go
            if "ZR" in buttons and self.zr >= 0:
                self.zr += 1
                if self.zr == 1:
                    self.facing = self.cam_yaw            # the draw turns Link to the camera
                if self.event_on_draw and self.zr == 10:
                    self.event = True
            elif "ZR" not in buttons:
                if self.zr >= 30:
                    self._fire()
                self.zr = 0
            look = None
            if self.zr > 0 and rstick and self.aim_stick == "right":
                look = rstick
            if self.zr > 0 and stick and self.aim_stick == "left":
                look, stick = stick, None
            if self.zr > 0 and (look or self.cam_keep):
                self._look(look)
            if self.zr > 0 and not stick and not rstick:
                self.cam_yaw = (self.cam_yaw + self.drift) % 360
            if self.zr > 0:
                stick = None                              # no walking while aiming
            if "A" in buttons:
                self.a_held += 1
                if self.a_held == 30 and self.prompt == "Grab":
                    self.grabbed, self.prompt = True, "Empty"
            else:
                self._let_go_a()
                self._drop_grab()
            if stick:
                self._stick(stick)
            self._frames(1)
        self._let_go_a()   # the pad goes back to nothing after the frames
        return {"ok": True, "state": self.state()}

    def _drop_grab(self):
        if self.grabbed:                                      # a frame without A lets go of the block
            self.grabbed, self.prompt = False, "Grab"
            self.dropped += 1

    def _let_go_a(self):
        if 0 < self.a_held < 30 and self.prompt == "Grab":   # a tap: up onto the block
            self.pos[1] += 1.0
            self.prompt = None
        self.a_held = 0

    def move(self, yaw, strength=1.0, frames=1, check=True):
        if self.refuse_move:
            self.refused += 1
            return {"ok": False, "error": "no_camera", "no_camera": True, "state": self.state()}
        for _ in range(frames):
            self._drop_grab()                                 # `move` presses no button
            self._walk(yaw, strength)
            self._frames(1)
        return {"ok": True, "stick": [0, strength], "state": self.state()}

    # --- the world ---
    def _frames(self, n):
        self.frame += n
        if self.hurt_at is not None and self.frame >= self.hurt_at:
            self.life -= 2
            self.hurt_at = None

    def _fire(self):
        if self.assigned != "SlingShot" or not self.item_owned:
            return
        n = self.inv["PouchData.Item_DekuSeed.StockNum"]
        if n <= 0:
            return
        self.inv["PouchData.Item_DekuSeed.StockNum"] = n - 1
        f, e = self._forward(), self._eye()
        # The reticle is a little over the middle (km.RETICLE): the shot goes along its line, not the camera's.
        y, p = math.radians(self.cam_yaw), math.radians(self.cam_pitch)
        up = (-math.sin(y) * math.sin(p), math.cos(p), -math.cos(y) * math.sin(p))
        lift = km.RETICLE[1] * math.tan(math.radians(30))
        f = [f[i] + up[i] * lift for i in range(3)]
        for a in self.actors:
            if a.get("takes"):
                d = [a["pos"][i] - e[i] for i in range(3)]
                off = math.degrees(math.acos(max(-1.0, min(1.0, sum(d[i] * f[i] for i in range(3)) /
                                                             (math.hypot(*d) * math.hypot(*f))))))
                if off < (4.0 if self.assist else 1.0):
                    a["life"] -= 1
                    if a["life"] <= 0:
                        a["alive"] = False

    def _on_ladder(self):
        if not self.ladder:
            return False
        x, z, bottom, top = self.ladder
        return math.hypot(self.pos[0] - x, self.pos[2] - z) < 0.6 and bottom - 0.01 <= self.pos[1] <= top + 0.01

    def _stick(self, stick):
        angle, strength = stick
        if self._on_ladder():
            x, z, bottom, top = self.ladder
            dy = 0.05 * strength * (1 if angle == 0 else -1)
            self.pos[1] = max(bottom, min(top, self.pos[1] + dy))
            return
        self._walk((self.cam_yaw - angle) % 360, strength)   # the stick is camera-relative: up = away, right = right

    def _walk(self, yaw, strength):
        if getattr(self, "gap_run", 0.0) > 0.0:   # in the air: the take-off speed and way, whatever the stick
            if strength < 1.0 and getattr(self, "air_needs_stick", False):   # or a stick let go falls short
                self.gap_run = self.jump_m
            yaw, strength = self.air_yaw, 1.0
        self.air_yaw = yaw
        turn = (yaw - self.facing + 180.0) % 360.0 - 180.0
        self.facing = (self.facing + max(-45.0, min(45.0, turn))) % 360   # Link turns toward the stick, 45 a frame
        step = 0.15 * strength
        nx = self.pos[0] + math.sin(math.radians(yaw)) * step
        nz = self.pos[2] + math.cos(math.radians(yaw)) * step
        for a in self.actors:
            if a.get("block") and math.hypot(nx - a["pos"][0], nz - a["pos"][2]) < 0.9:
                if self.grab_needed and not self.grabbed:
                    self.prompt = "Grab"
                    return
                self.lean += 1
                if self.lean < self.lean_needed:
                    return
                if self.block_limit is not None and a["pos"][2] + (nz - self.pos[2]) > self.block_limit:
                    return
                a["pos"][0] += nx - self.pos[0]
                a["pos"][2] += nz - self.pos[2]
        g = self.ground(nx, nz)
        if g is None:
            self.gap_run = getattr(self, "gap_run", 0.0) + step
            if (strength < 1.0 and self.gap_run <= step) or self.gap_run > self.jump_m:   # a walk off drops
                self.pos = list(self.respawn)
                self.life -= 2
                self.gap_run = 0.0
                return
            self.pos = [nx, self.pos[1] + 0.05, nz]   # in the air
            return
        self.gap_run = 0.0
        self.pos = [nx, g, nz]


def eye(life=1, takes=True, pos=(0.0, 3.0, 8.0)):
    return {"id": 7, "name": "Obj_Eye", "kind": "switch", "pos": list(pos), "life": life, "alive": True, "takes": takes}


class Shoot(unittest.TestCase):
    def test_hit_takes_a_seed_and_the_target_life(self):
        g = Sim(actors=[eye()])
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "hit", r)
        self.assertEqual((r["ammo_before"], r["ammo_after"]), (5, 4))
        self.assertIn("assign SlingShot", g.lines)
        self.assertEqual(g.wrote_yaw, 0)             # turned with the stick, no written yaw
        self.assertTrue(r["turn"]["turned"])
        self.assertEqual(r["aim"]["yaw"], 0.0)
        self.assertAlmostEqual(r["aim"]["pitch"], math.degrees(math.atan2(3, 8)), places=1)
        self.assertFalse(r["aim"]["pitch_read"])

    def test_miss_when_the_target_does_not_take_it(self):
        g = Sim(actors=[eye(takes=False)])
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "miss", r)
        self.assertIn("5 -> 4", r["reason"])

    def test_no_ammo_is_not_fired_and_runs_no_frame(self):
        g = Sim(actors=[eye()], seeds=0)
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not fired")
        self.assertIn("no ammo", r["reason"])
        self.assertEqual(g.frame, 100)

    def test_ammo_that_does_not_move_is_not_fired(self):
        g = Sim(actors=[eye()], item_owned=False)
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not fired")
        self.assertIn("did not go down", r["reason"])

    def test_a_help_box_while_drawing(self):
        g = Sim(actors=[eye()])
        g.event_on_draw = True
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not fired")
        self.assertIn("help box", r["reason"])

    def test_turns_the_view_with_the_right_stick_and_checks_the_reticle(self):
        g = Sim(actors=[eye(pos=(3.0, 4.0, 7.0))])
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "hit", r)
        self.assertEqual(r["aim"]["stick"], "right")
        self.assertLessEqual(math.hypot(*r["aim"]["error"]), km.AIM_TOL)
        self.assertGreater(abs(r["aim"]["error_start"][1]), 0.1)   # it started well off: the turn did the aim

    def test_aims_a_view_that_eases_in_and_coasts_on(self):
        # The game's aim view keeps turning a few frames after the stick (NEXT13b: the up probe moved the target
        # sideways as much as the right probe did, and the loop stalled 0.11 off). Measures must wait for it.
        # A slow coast (0.93, 0.95 of the speed kept a frame) must not pass for a steady drift (review-12).
        for keep in (0.8, 0.9, 0.93, 0.95, 0.97):
            with self.subTest(keep=keep):
                g = Sim(actors=[eye(pos=(3.0, 4.0, 7.0))])
                g.cam_keep = keep
                r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
                self.assertEqual(r["result"], "hit", r)
                self.assertLessEqual(math.hypot(*r["aim"]["error"]), km.AIM_TOL)
                (ax, ay), (bx, by) = r["aim"]["gains"]
                self.assertLess(abs(bx), 0.2 * abs(ax), r["aim"])   # the up probe learnt no sideways turn

    def test_a_coast_with_a_drift_still_fires_nothing_off_target(self):
        # A coasting view that also drifts: the settle ends on the drift, and the drift-off check still holds.
        g = Sim(actors=[eye()])
        g.cam_keep, g.drift = 0.9, 0.3
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not aimed", r)
        self.assertEqual(g.inv["PouchData.Item_DekuSeed.StockNum"], 5)

    def test_falls_back_to_the_left_stick(self):
        g = Sim(actors=[eye()])
        g.aim_stick = "left"
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "hit", r)
        self.assertEqual(r["aim"]["stick"], "left")

    def test_no_stick_turns_the_view_is_not_fired(self):
        g = Sim(actors=[eye()])
        g.aim_stick = None
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not aimed", r)
        self.assertIn("neither stick", r["reason"])
        self.assertEqual(g.inv["PouchData.Item_DekuSeed.StockNum"], 5)   # put away with B, no seed spent

    def test_no_view_is_not_fired(self):
        g = Sim(actors=[eye()])
        g.has_view = False
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not aimed", r)
        self.assertIn("no camera view", r["reason"])
        self.assertEqual(g.inv["PouchData.Item_DekuSeed.StockNum"], 5)

    def test_on_screen_projects_and_flips_y_down(self):
        g = Sim()
        v = g._view()
        x, y = km.on_screen(v, (0.0, 1.0, 5.0))
        self.assertAlmostEqual(x, 0.0)
        self.assertAlmostEqual(y, 0.0)
        x, y = km.on_screen(v, (-1.0, 2.0, 5.0))         # -x is the right of a +z view (right-handed), up is up
        self.assertGreater(x, 0.0)
        self.assertGreater(y, 0.0)
        self.assertLess(km.on_screen(dict(v, y_down=True), (-1.0, 2.0, 5.0))[1], 0.0)
        self.assertIsNone(km.on_screen(v, (0.0, 1.0, -5.0)))   # behind
        self.assertIsNone(km.on_screen(None, (0.0, 1.0, 5.0)))

    def test_turns_link_with_the_stick_from_behind_and_writes_no_yaw(self):
        g = Sim(actors=[eye()])
        g.facing = 200.0
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "hit", r)
        self.assertEqual(g.wrote_yaw, 0)
        self.assertGreater(r["turn"]["rounds"], 1)
        self.assertLess(r["turn"]["moved_m"], 0.5)

    def test_a_turn_that_never_comes_is_not_fired(self):
        g = Sim(actors=[eye()])
        g.facing = 180.0
        g._walk = lambda yaw, strength: None          # the stick turns nothing (a cut-scene, a held pose)
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not fired", r)
        self.assertIn("the turn: still", r["reason"])
        self.assertEqual(g.wrote_yaw, 0)
        self.assertEqual(g.inv["PouchData.Item_DekuSeed.StockNum"], 5)

    def test_a_view_that_drifts_off_before_the_let_go_is_not_fired(self):
        g = Sim(actors=[eye()])
        g.drift = 0.3                                 # 9 degrees over the 30-frame hold
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not aimed", r)
        self.assertIn("drifted off before the let-go", r["reason"])
        self.assertEqual(g.inv["PouchData.Item_DekuSeed.StockNum"], 5)
        self.assertEqual(g.actors[0]["life"], 1)

    def test_assist_adds_the_aim_line(self):
        g = Sim(actors=[eye(pos=(4.0, 3.0, 8.0))])
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye", assist=True)
        self.assertIn("aim 4.00 3.00 8.00", g.lines)
        self.assertEqual(r["result"], "hit")

    def test_a_point_target_is_only_fired(self):
        g = Sim()
        r = km.shoot(g, "slingshot", at=[0, 1, 5])
        self.assertEqual(r["result"], "fired")
        self.assertEqual(r["ammo_after"], 4)

    def test_a_scene_change_after_a_point_shot_is_reported(self):
        g = Sim()
        orig = g.step
        g.step = lambda frames=1: (setattr(g, "scene", "Spot04") if g.frame > 200 else None, orig(frames))[1]
        r = km.shoot(g, "slingshot", at=[0, 1, 5])
        self.assertEqual(r["result"], "fired", r)
        self.assertIn("scene changed", r["reason"])
        self.assertIn("not judged", r["reason"])

    def test_no_list_and_gone_and_bad_args(self):
        g = Sim(actors=[eye()])
        g.no_list = True
        r = km.shoot(g, "slingshot", ident=7, name="Obj_Eye")
        self.assertEqual(r["result"], "not fired")
        self.assertIn("no world list", r["reason"])
        g = Sim(actors=[eye()])
        self.assertEqual(km.shoot(g, "slingshot", ident=7, name="Enemy_Other")["reason"], "gone")
        self.assertEqual(km.shoot(g, "slingshot", ident=99)["reason"], "gone")
        with self.assertRaises(ValueError):
            km.shoot(g, "harp", ident=7)
        with self.assertRaises(ValueError):
            km.shoot(g, "slingshot")
        with self.assertRaises(ValueError):
            km.shoot(g, "slingshot", ident=7, at=[1, 2, 3])

    def test_tilt_holds_the_stick_with_zr(self):
        g = Sim(actors=[eye()])
        calls = []
        orig = g.input
        g.input = lambda stick=None, buttons=(), frames=1, rstick=None: (calls.append((stick, list(buttons), frames)),
                                                                         orig(stick, buttons, frames, rstick))[1]
        km.shoot(g, "slingshot", ident=7, name="Obj_Eye", tilt_frames=-6)
        self.assertIn(([180, 1.0], ["ZR"], 6), calls)


class Climb(unittest.TestCase):
    def test_turns_to_the_wall_with_the_stick_and_writes_no_yaw(self):
        g = Sim(ladder=(0.0, 0.0, 0.0, 6.0))
        g.facing = 180.0                                   # back to the wall at +z
        r = km.climb(g, up=True, to_y=4.0, face=[0.0, 1.0])
        self.assertEqual(r["result"], "arrived", r)
        self.assertTrue(r["turn"]["turned"], r)
        self.assertGreater(r["turn"]["rounds"], 0)
        self.assertEqual(g.wrote_yaw, 0)

    def test_up_to_a_height(self):
        g = Sim(ladder=(0.0, 0.0, 0.0, 6.0))
        r = km.climb(g, up=True, to_y=4.0)
        self.assertEqual(r["result"], "arrived", r)
        self.assertGreaterEqual(r["pos"][1], 3.9)

    def test_up_to_the_top_without_a_height(self):
        g = Sim(ladder=(0.0, 0.0, 0.0, 2.0))
        r = km.climb(g)
        self.assertEqual(r["result"], "stopped", r)
        self.assertIn("top", r["reason"])

    def test_the_top_ledge_stops_without_walking_on(self):
        g = Sim(ladder=(0.0, 0.0, 0.0, 2.0))
        orig = g._stick
        top = []

        def stick(s):
            if g.pos[1] >= 2.0 - 1e-9:      # at the top the stick walks Link onto the ledge
                top.append(1)
                g.pos[2] += 0.1
                return
            orig(s)
        g._stick = stick
        r = km.climb(g)
        self.assertEqual(r["result"], "stopped", r)
        self.assertLessEqual(len(top), 2 * km.CHUNK, r)

    def test_down(self):
        g = Sim(pos=(0.0, 5.0, 0.0), ladder=(0.0, 0.0, 0.0, 5.0))
        r = km.climb(g, up=False, to_y=1.0)
        self.assertEqual(r["result"], "arrived", r)

    def test_not_on_a_ladder_walks_across(self):
        g = Sim()
        r = km.climb(g, to_y=3.0)
        self.assertEqual(r["result"], "not climbing", r)
        self.assertIn("walking", r["reason"])

    def test_stuck_when_nothing_moves(self):
        g = Sim(ladder=(0.0, 0.0, 0.0, 0.0))     # a ladder with no height: the stick does nothing
        r = km.climb(g, to_y=3.0)
        self.assertEqual(r["result"], "stuck", r)
        self.assertIn("never climbed", r["reason"])

    def test_hurt_stops_it(self):
        g = Sim(ladder=(0.0, 0.0, 0.0, 9.0))
        g.hurt_at = 120
        r = km.climb(g, to_y=8.0)
        self.assertEqual(r["result"], "hurt", r)

    def test_already_there(self):
        g = Sim(pos=(0.0, 5.0, 0.0))
        self.assertEqual(km.climb(g, to_y=4.0)["result"], "arrived")
        self.assertEqual(g.frame, 100)


def block(pos=(0.0, 0.0, 3.0)):
    return {"id": 3, "name": "Obj_PushBlock", "kind": "other", "pos": list(pos), "alive": True, "block": True}


class Push(unittest.TestCase):
    def test_pushes_the_distance(self):
        g = Sim(actors=[block()])
        r = km.push(g, 3, 2.0, name="Obj_PushBlock")
        self.assertEqual(r["result"], "pushed", r)
        self.assertGreaterEqual(r["moved_m"], 1.85)
        self.assertEqual(r["yaw"], 0.0)

    def test_stuck_when_the_block_is_blocked(self):
        g = Sim(actors=[block()])
        g.block_limit = 3.5
        r = km.push(g, 3, 2.0, name="Obj_PushBlock")
        self.assertEqual(r["result"], "stuck", r)
        self.assertIn("did not move", r["reason"])

    def test_a_refused_move_stops_it(self):
        g = Sim(actors=[block()])
        walk = km.pc.walk_to

        def walk_then_refuse(*a, **k):
            r = walk(*a, **k)
            g.facing = (g.facing + 180.0) % 360         # back to the block: the turn needs the stick
            g.refuse_move = True
            return r
        km.pc.walk_to = walk_then_refuse
        try:
            r = km.push(g, 3, 2.0, name="Obj_PushBlock")
        finally:
            km.pc.walk_to = walk
        self.assertEqual(r["result"], "stopped", r)
        self.assertEqual(r["reason"], "the turn to the block: move refused: no_camera")
        self.assertEqual(g.refused, 1)
        frames_before = r["frames"]
        self.assertEqual(r["moved_m"], 0.0)
        self.assertEqual(g.frame - 100, frames_before)    # only frames that ran are counted

    def test_holds_a_to_grab_before_the_stick(self):
        g = Sim(actors=[block()])
        g.grab_needed = True
        r = km.push(g, 3, 2.0, name="Obj_PushBlock")
        self.assertEqual(r["result"], "pushed", r)
        self.assertEqual(r["grab"], {"prompt": "Grab", "held": True, "after": "Empty"})
        self.assertEqual(g.pos[1], 0.0)   # not climbed onto it
        self.assertEqual(g.dropped, 0)    # A held through the push: the grab never let go

    def test_input_order_a_then_a_with_the_stick(self):
        # The by-hand order: A alone GRAB_FRAMES, then A with the stick toward the block, never the stick alone.
        g = Sim(actors=[block()])
        g.grab_needed = True
        g.cam_yaw = 30.0                  # a camera not along the push: the stick must be turned for it
        calls, orig_input, orig_move = [], g.input, g.move

        def rec_input(stick=None, buttons=(), frames=1, rstick=None):
            calls.append(("input", stick and [round(stick[0], 1), round(stick[1], 2)], sorted(buttons), frames))
            return orig_input(stick, buttons, frames, rstick)

        def rec_move(yaw, strength=1.0, frames=1, check=True):
            calls.append(("move", round(yaw, 1), strength, frames))
            return orig_move(yaw, strength, frames, check)
        g.input, g.move = rec_input, rec_move
        r = km.push(g, 3, 2.0, name="Obj_PushBlock")
        self.assertEqual(r["result"], "pushed", r)
        grab = next(i for i, c in enumerate(calls) if c[0] == "input" and c[2] == ["A"])
        self.assertEqual(calls[grab], ("input", None, ["A"], km.GRAB_FRAMES))
        after = calls[grab + 1:]
        self.assertTrue(after)
        for c in after:
            self.assertEqual(c[0], "input", calls)            # no `move`: it would let go of A
            self.assertEqual(c[2], ["A"], c)
            self.assertEqual(c[1], [30.0, 1.0], c)            # world yaw 0 with the camera at 30: 30 to the right
        self.assertEqual(g.dropped, 0)

    def test_without_the_grab_the_block_does_not_move(self):
        g = Sim(actors=[block()])
        g.grab_needed = True
        orig = g.input
        g.input = lambda stick=None, buttons=(), frames=1, rstick=None: orig(stick, [b for b in buttons if b != "A"],
                                                                             frames, rstick)
        r = km.push(g, 3, 2.0, name="Obj_PushBlock")
        self.assertEqual(r["result"], "stuck", r)
        self.assertEqual(r["moved_m"], 0.0)

    def test_a_tap_climbs_onto_it_and_is_reported(self):
        g = Sim(actors=[block()])
        g.grab_needed = True
        orig = g.input
        g.input = lambda stick=None, buttons=(), frames=1, rstick=None: orig(stick, buttons, min(frames, 4), rstick)
        r = km.push(g, 3, 2.0, name="Obj_PushBlock")
        self.assertEqual(r["result"], "stuck", r)
        self.assertIn("climbed onto the block", r["reason"])

    def test_no_prompt_presses_nothing(self):
        g = Sim(actors=[block()])
        r = km.push(g, 3, 2.0, name="Obj_PushBlock")
        self.assertEqual(r["grab"], {"prompt": None, "held": False})

    def test_no_list_and_gone(self):
        g = Sim(actors=[block()])
        g.no_list = True
        self.assertIn("no world list", km.push(g, 3, 1.0)["reason"])
        g = Sim(actors=[block()])
        self.assertEqual(km.push(g, 4, 1.0)["result"], "gone")


class Jump(unittest.TestCase):
    def gap(self, width):
        return lambda x, z: None if 5.0 < z < 5.0 + width else 0.0

    def test_runs_across_a_gap(self):
        g = Sim(ground=self.gap(2.0))
        r = km.jump(g, 0.0, 10.0, land_y=0.0)
        self.assertEqual(r["result"], "landed", r)
        self.assertTrue(r["airborne"])

    def test_too_wide_is_respawned(self):
        g = Sim(ground=self.gap(6.0))
        r = km.jump(g, 0.0, 14.0, land_y=0.0)
        self.assertEqual(r["result"], "respawned", r)

    def test_fell_below_the_landing(self):
        g = Sim(ground=lambda x, z: 0.0 if z < 5 else -3.0)
        r = km.jump(g, 0.0, 10.0, land_y=0.0)
        self.assertEqual(r["result"], "fell", r)

    def test_a_refused_move_stops_it(self):
        g = Sim(ground=self.gap(2.0))
        g.refuse_move = True
        r = km.jump(g, 0.0, 10.0, land_y=0.0)
        self.assertEqual(r["result"], "stopped", r)
        self.assertEqual(r["reason"], "move refused: no_camera")
        self.assertEqual(r["frames"], 0)
        self.assertEqual(g.refused, 1)

    def test_one_steady_look_in_the_air_is_not_a_landing(self):
        g = Sim(pos=(0.0, 0.0, 9.8))
        ys = iter([0.0, 0.5, 0.5, 0.2, 0.0, 0.0, 0.0, 0.0])
        orig = g.state
        g.state = lambda: dict(orig(), pos=[g.pos[0], next(ys, 0.0), g.pos[2]])
        g.move = lambda yaw, strength=1.0, frames=1, check=True: (g._frames(frames), {"ok": True})[1]
        r = km.jump(g, 0.0, 10.0, land_y=0.0)
        self.assertEqual(r["result"], "landed", r)
        self.assertGreaterEqual(len(r["trace"]), 6, r)

    def test_lets_go_of_the_stick_at_the_point(self):
        # The lane run: Link came down 0.05 m from the point, but the stick stayed at full until the second steady look,
        # so he ran 1.5 m past it and was called stuck. Near the point on the ground the stick lets go.
        g = Sim(pos=(0.0, 7.0, 0.0), ground=lambda x, z: 7.0 if z < 5.0 else (None if z < 6.2 else 0.0))
        r = km.jump(g, 0.0, 6.5, land_y=0.0)
        self.assertEqual(r["result"], "landed", r)
        self.assertTrue(r["airborne"])
        self.assertLessEqual(abs(r["pos"][2] - 6.5), 0.6, r)
        down = next(f for f, _x, y, _z in r["trace"] if y == 0.0)
        self.assertLessEqual(r["frames"] - down, 8, r)   # the landing is called within 8 frames of the touch down

    def test_keeps_the_stick_over_the_gap_for_a_point_near_the_far_edge(self):
        # Review: the point 0.3 m past the far edge is within tolerance while Link is still over the gap. A game that
        # does not keep his speed with the stick at 0 would drop him in; the stick lets go only on the ground.
        g = Sim(pos=(0.0, 7.0, 0.0), ground=lambda x, z: 7.0 if z < 5.0 else (None if z < 6.2 else 0.0))
        g.air_needs_stick = True
        r = km.jump(g, 0.0, 6.5, land_y=0.0)
        self.assertEqual(r["result"], "landed", r)
        self.assertLessEqual(abs(r["pos"][2] - 6.5), 0.6, r)

    def test_a_long_fall_past_the_point_is_not_stuck(self):
        # The lane run: off a 5 m ledge Link flew over the point and fell on past it for more than 4 looks; the stuck
        # count ran in the air and called it stuck before he was down. In the air nothing counts as stuck.
        g = Sim(pos=(0.0, 5.0, 0.0), ground=lambda x, z: 5.0 if z < 1.0 else 0.0)
        orig, fall = g._walk, {"yaw": None}

        def walk(yaw, strength):
            if fall["yaw"] is not None:   # falling: 0.15 m down and 0.1 m on along the take-off way each frame
                y = max(0.0, g.pos[1] - 0.15)
                g.pos = [g.pos[0] + math.sin(math.radians(fall["yaw"])) * 0.1, y,
                         g.pos[2] + math.cos(math.radians(fall["yaw"])) * 0.1]
                if y == 0.0:
                    fall["yaw"] = None
                return
            y0 = g.pos[1]
            orig(yaw, strength)
            if y0 - g.pos[1] > 1.0:
                g.pos[1], fall["yaw"] = y0, yaw
        g._walk = walk
        r = km.jump(g, 0.0, 1.3, land_y=0.0)
        self.assertEqual(r["result"], "landed", r)
        self.assertTrue(r["airborne"])
        self.assertLessEqual(abs(r["pos"][2] - 1.3), 0.6, r)
        self.assertGreater(max(z for _f, _x, _y, z in r["trace"]), 1.3 + 0.6 + 1.0, r)   # he did land well past it

    def test_stuck_at_a_wall(self):
        g = Sim()
        orig = g._walk
        g._walk = lambda yaw, strength: orig(yaw, strength) if g.pos[2] < 2.0 else None
        r = km.jump(g, 0.0, 10.0)
        self.assertEqual(r["result"], "stuck", r)


class Angles(unittest.TestCase):
    def test_aim_angles_and_ammo(self):
        self.assertEqual(km.aim_angles([0, 0, 0], [5, 0, 0]), (90.0, 0.0))
        self.assertEqual(km.aim_angles([0, 0, 0], [0, 5, 0])[1], 90.0)
        self.assertEqual(km.aim_angles([0, 2, 0], [0, 0, -2])[0], 180.0)
        self.assertEqual(km.ammo_of({"inventory": {"PouchData.Item_Arrow.StockNum": 9}}, "Item_Arrow"), 9)
        self.assertEqual(km.ammo_of({"inventory": {"x": 1}}, "Item_Arrow"), 0)
        self.assertIsNone(km.ammo_of({}, "Item_Arrow"))
        self.assertIsNone(km.ammo_of({"inventory": {"x": 1}}, None))


class Mcp(unittest.TestCase):
    def game(self, sim):
        g = mcp_playtest.Game.__new__(mcp_playtest.Game)
        g.client, g.recorder, g.last_look = sim, None, None
        return g

    def test_the_tools_are_listed_and_routed(self):
        names = [t["name"] for t in mcp_playtest.PLAYER_TOOLS]
        for verb in km.VERBS:
            self.assertIn(verb, names)
        g = self.game(Sim(actors=[eye()]))
        out = g._call("shoot", {"item": "slingshot", "id": 7, "name": "Obj_Eye"})
        self.assertEqual(out["result"], "hit")
        self.assertIn("look", out)
        out = g._call("climb", {"to_y": 1.0})
        self.assertEqual(out["result"], "not climbing")
        out = g._call("jump", {"x": 0, "z": 1})
        self.assertEqual(out["result"], "landed")
        out = g._call("push", {"id": 7, "name": "Obj_Eye", "distance_m": 1})
        self.assertEqual(out["result"], "slipped")   # (an eye is no block: Link walks past it)

    def test_bad_arguments_are_refused(self):
        g = self.game(Sim(actors=[eye()]))
        for name, args in (("shoot", {"item": "slingshot", "at": [1, 2]}), ("push", {"distance_m": 1}),
                           ("jump", {"x": "far", "z": 1}), ("climb", {"max_frames": 99999})):
            with self.assertRaises(ValueError, msg=name):
                g._call(name, args)


if __name__ == "__main__":
    unittest.main()
