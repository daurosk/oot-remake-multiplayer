# Play-test data

Facts about the game that a tester found and wrote down, one file per scene: `lowercase-words.json`. Numbers are
the game's own: metres, and yaw in degrees (0 toward +z, 90 toward +x), as `get_state` reports them.

```
{
  "kit": 1,
  "title": "One line",
  "scene": "InsideTheDekuTree",
  "positions": {"chest-front": {"x": 5.45, "y": 108.9, "z": 6.15, "yaw": 0, "note": "What stands here."}},
  "routes": {"name": {"start": "chest-front", "steps": ["chest-beside"], "note": "..."}},
  "menu_paths": {"name": {"from": "Where you start", "presses": ["PLUS", "A"], "note": "..."}},
  "stuck_spots": [{"x": 1.0, "z": 2.0, "radius": 1.5, "note": "Link stops here.", "fix": "Walk around by the left."}],
  "notes": ["Where the numbers came from."]
}
```

- `scene`, `kit` and `positions` are the base; `routes`, `menu_paths`, `stuck_spots` and `notes` are optional.
- A position needs `x` and `z`; `y` and `yaw` (-180 to 360) are optional. A route's `start` and every step are
  position names of the same file. `presses` are bridge button names (`A B X Y L R ZL ZR PLUS MINUS LS RS UP DOWN
  LEFT RIGHT`). A stuck spot has a `radius` (0.1 to 50 m) and a `note`.
- Say where a number came from (a scenario file, a run on a given day): a number nobody can trace is not a fact.
- Never put a game file, a log, a shot or a value dump here. A text over 300 characters is refused.
- `python tools/autotest/kit_library.py check` reads every file.
