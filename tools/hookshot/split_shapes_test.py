"""Patches 0047 (row 301): a local co-op window's own shape, and 0048 (row 325): local co-op's Field of view. Run on Longshot's source with our patches applied (the
linux-longshot job: the kit's data/builder/source after its build.sh --renderer-only):
    python -I -B split_shapes_test.py <patched source> [--cc gcc]
Adapted from Longshot's own tests/test_widescreen_shapes.py (GPL-3.0, as this repo), which the Linux AppImage the kit
patches does not ship. Game-free: host/hos_wide_size.h is compiled as is, and hos_main.c's widescreen block is cut out
of the source and compiled with a fake output size. Checked: HOS_ASPECT is read as a shape ("8:9", "1920x2100", "8/9")
or a number, never "8:9" as 8; a split screen window's narrow HOS_OUTPUT_RES (1:2 to 5:4) is taken from the switch
only, while output_res= and the Insert menu keep 5:4 to 48:9; the pictures of the narrow shapes (height kept, cameras
at the ratio, HUD at full width), and without the switches the same ratios give the game's own picture as before;
0048: HOS_FOV_DEFAULT (the kit's local co-op start sets 150) starts the menu's Field of view there for the run only
(env_override: never saved), only over the row's own 100%, on the row's steps (50..150 by 5).
Exit status 0 when all hold.
"""
import argparse
import os
import re
import subprocess
import tempfile
from pathlib import Path

SIZES = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "host/hos_wide_size.h"

static int near(double a, double b) { return fabs(a - b) < 0.0001; }

int main(void) {
    /* (HOS_ASPECT: shapes and numbers) */
    assert(near(hos_aspect_parse("8:9"), 8.0 / 9.0));
    assert(near(hos_aspect_parse("5:4"), 1.25));
    assert(near(hos_aspect_parse("1920x2100"), 1920.0 / 2100.0));
    assert(near(hos_aspect_parse("1920X2100"), 1920.0 / 2100.0));
    assert(near(hos_aspect_parse("8/9"), 8.0 / 9.0));
    assert(near(hos_aspect_parse("2.3889"), 2.3889));
    assert(near(hos_aspect_parse("0.9143"), 0.9143));
    assert(hos_aspect_parse("") == 0.0 && hos_aspect_parse(NULL) == 0.0 && hos_aspect_parse("x") == 0.0);
    assert(hos_aspect_parse("8:0") == 0.0 && hos_aspect_parse("-1") == 0.0);
    /* (the split clamp: 1:2 to 48:9; the screens' clamp unchanged at 5:4) */
    assert(near(hos_wide_clamp_split(0.3), 0.5) && near(hos_wide_clamp_split(0.8889), 0.8889));
    assert(near(hos_wide_clamp_split(1.6), 1.6) && near(hos_wide_clamp_split(9.0), 48.0 / 9.0));
    assert(near(hos_wide_clamp(0.8889), 1.25));
    /* (output sizes: a half of a 4K screen at 150 % with its title bar, of a 1080p one, a quarter; the menu's unchanged) */
    assert(hos_output_size_valid_split(1920, 2048) && hos_output_size_valid_split(960, 1016));
    assert(hos_output_size_valid_split(1920, 1016) && hos_output_size_valid_split(1280, 2560));
    assert(!hos_output_size_valid_split(1000, 2008) && !hos_output_size_valid_split(312, 600));
    assert(!hos_output_size_valid_split(5120, 5760));
    assert(!hos_output_size_valid(1920, 2048) && hos_output_size_valid(1920, 1440) && !hos_output_size_valid(1080, 1920));
    puts("ok");
    return 0;
}
'''

WIDE = r'''
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/hos_wide_size.h"

volatile uint64_t hos_wide_out, hos_wide_scene;
volatile float hos_wide_ratio, hos_wide_squeeze, hos_wide_margin;
float hos_aspect_ratio;
static unsigned out_w, out_h = 1440;
uint32_t hos_output_size(int height) { return height ? out_h : out_w; }
static void hos_log(const char *f, ...) { (void)f; }

static void wide(float want) {
@BLOCK@
}

int main(int argc, char **argv) {
    out_h = (unsigned)atoi(argv[2]);
    out_w = argc > 3 ? (unsigned)atoi(argv[3]) : out_h * 16 / 9;
    wide((float)atof(argv[1]));
    printf("%llux%llu %llux%llu %.4f %.4f %.4f\n",
           (unsigned long long)(hos_wide_out & 0xffffffffu), (unsigned long long)(hos_wide_out >> 32),
           (unsigned long long)(hos_wide_scene & 0xffffffffu), (unsigned long long)(hos_wide_scene >> 32),
           (double)hos_wide_ratio, (double)hos_wide_squeeze, (double)hos_wide_margin);
    return 0;
}
'''

# (screen ratio, the run switches) -> picture, scene, camera ratio, HUD width, movie margin, at a 1440 output. A half of
# a 16:9 screen is about 8:9. Without the switches the same ratios are the game's own picture, as before 0047.
SPLIT_CASES = (
    ("0.8889", {"HOS_ASPECT": "8:9"}, "1280x1440 960x1080 0.8889 1.0000 0.0000"),
    ("0.9143", {"HOS_OUTPUT_RES": "1920x2100"}, "1320x1440 992x1080 0.9167 1.0000 0.0000"),
    ("1.1", {"HOS_ASPECT": "1.1"}, "1584x1440 1192x1080 1.1000 1.0000 0.0000"),
    ("0.3", {"HOS_ASPECT": "0.3"}, "720x1440 544x1080 0.5000 1.0000 0.0000"),
    ("0.8889", {}, "0x0 0x0 0.0000 0.0000 0.0000"),
    ("1.1", {}, "1800x1440 1352x1080 1.2500 1.0000 0.0000"),
    ("1.3333", {}, "1920x1440 1440x1080 1.3333 1.0000 0.0000"),   # 4:3, unchanged
    ("1.7778", {}, "0x0 0x0 0.0000 0.0000 0.0000"),               # 16:9: the game's own picture
)


def wide_block(src):
    """The sizes, from the 16:9 constant to the else branch's log line, as hos_main.c has them"""
    start = src.index("        const uint32_t output_w = hos_output_size(0), output_h = hos_output_size(1);")
    end = src.index("\n", src.index("hos_log(\"boot: widescreen: a %.4f screen", start))
    return src[start:end] + "\n        }\n"


def build(cc, root, work, name, text):
    source, exe = work / (name + ".c"), work / (name + ".exe")
    source.write_text(text, encoding="utf-8")
    subprocess.run([cc, "-std=c11", "-I", str(root), str(source), "-o", str(exe), "-lm"], check=True)
    return exe


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="Longshot's source with our patches applied")
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    root = args.source.resolve()
    applet = (root / "host/nn_applet.c").read_text(encoding="utf-8")
    main_c = (root / "host/hos_main.c").read_text(encoding="utf-8")
    bad = []

    with tempfile.TemporaryDirectory(prefix="split-shapes-") as tmp:
        work = Path(tmp)
        env = {k: v for k, v in os.environ.items() if k not in ("HOS_WIDE_MAX_PX", "HOS_ASPECT", "HOS_OUTPUT_RES")}
        exe = build(args.cc, root, work, "sizes", SIZES)
        out = subprocess.run([str(exe)], capture_output=True, text=True, check=True, env=env).stdout.strip()
        if out != "ok":
            bad.append("hos_wide_size.h fixture said %r" % out)
        exe = build(args.cc, root, work, "wide", WIDE.replace("@BLOCK@", wide_block(main_c)))
        for want, switch, expect in SPLIT_CASES:
            got = subprocess.run([str(exe), want, "1440"], capture_output=True, text=True, check=True,
                                 env=dict(env, **switch)).stdout.strip()
            if got != expect:
                bad.append("split %s %s: %s, expected %s" % (want, switch, got, expect))

    # (the switch only: output_mode takes HOS_OUTPUT_RES's text with the narrow sizes, output_res= without; the menu's
    # hos_output_parse stays the 5:4 one; HOS_ASPECT goes through hos_aspect_parse)
    mode = applet[applet.index("static int output_mode(void)"):]
    mode = mode[:mode.index("\n}\n")]
    need = (
        (mode, r"output_parse\(e \? e : text, &output_w, &output_h, e != NULL\)", "output_mode: narrow sizes from HOS_OUTPUT_RES only"),
        (applet, r"int hos_output_parse\(const char \*text, uint32_t \*w, uint32_t \*h\) \{[^}]*return output_parse\(text, w, h, 0\);",
         "hos_output_parse: output_res= and the menu at 5:4 to 48:9"),
        (applet, r"narrow \? hos_output_size_valid_split\(a, b\) : hos_output_size_valid\(a, b\)", "output_parse: the size check"),
        (applet, r'narrow \? "320x360" : "640x360", narrow \? "1:2" : "5:4"', "output_parse: the refusal names the size it checked"),
        (main_c, r"float want = e \? \(float\)hos_aspect_parse\(e\) : 0\.0f;", "hos_main.c: HOS_ASPECT through hos_aspect_parse"),
        (main_c, r"const double r = split \? hos_wide_clamp_split\(want\) : hos_wide_clamp\(want\);", "hos_main.c: the split clamp"),
    )
    # (0048: HOS_FOV_DEFAULT over the row's own 100% only, on its steps, for the run only; save() writes the player's value,
    # saved_value, not the run's; the row reaches 150%)
    menu = (root / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    gameplay = (root / "gpu/vk/menu/gameplay.cpp").read_text(encoding="utf-8")
    need += (
        (menu, r'if \(const char\* e = getenv\("HOS_FOV_DEFAULT"\); e && \*e && opts\.fov == 100\) \{\s*'
               r'const int v = std::min\(std::max\(\(atoi\(e\) \+ 2\) / 5 \* 5, 50\), 150\);[^\n]*\n\s*'
               r'if \(v != 100\) env_override\(&opts\.fov, v\);',
         "vk_menu.cpp: HOS_FOV_DEFAULT over the row's 100% only, for the run only"),
        (menu, r'fprintf\(f, "%s=%d\\n", ini_key\(r\), saved_value\(r\.value\)\);', "vk_menu.cpp: save() writes saved_value"),
        (gameplay, r"\.value = &opts\.fov,\s*\.lo = 50, \.hi = 150,", "gameplay.cpp: the Field of view row is 50..150%"),
    )
    for text, pattern, what in need:
        if not re.search(pattern, text):
            bad.append(what)
    if bad:
        raise SystemExit("FAIL:\n  " + "\n  ".join(bad))
    print("PASS: split screen shapes: HOS_ASPECT shapes, narrow HOS_OUTPUT_RES from the switch only, the pictures of "
          "halves down to 1:2, menu sizes and the screens' own pictures unchanged; HOS_FOV_DEFAULT for the run only")


if __name__ == "__main__":
    main()
