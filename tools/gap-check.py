#!/usr/bin/env python3
"""Dumps of a test with a HUD (test_chain --marker ... --gap N): is the HUD of every generated frame
what it is in the real frame, is nothing of it dragged into what is around it, and is the scene
handed to the interpolation free of it, in frames without a HUD capture of their own as well?

    tools/gap-check.py logs/test/NAME [--expect-gaps]

One line per dump ("no capture" where the layer's log says the frame had none) and "ok" or "NOT ok".
--expect-gaps: at least one of the dumps must be such a frame.

What is measured, in levels of 0..255: "hud", the mean difference from the real frame on the HUD
that stays in place (a marker that moves is another matter: tools/marker-check.py); "around", the same on a band of 12 pixels around it, where a HUD that was interpolated
along with the scene shows as a smear; "in scene", the share of the HUD's pixels that still have its
colour in the scene given to the interpolation (all of them where the frames are interpolated as
presented, with the HUD in them). Columns the moving bar of the test scene went over
between the two frames are left out of "around".
"""
import glob, os, re, sys
import numpy as np

STATIC = [(40, 40, 200, 3), (40, 40, 3, 100), (300, 200, 120, 60)]
MARKER = [(0, 0, 72, 3), (0, 45, 72, 3), (0, 3, 3, 42), (69, 3, 3, 42), (12, 12, 20, 24), (40, 14, 3, 20), (46, 14, 3, 20),
          (52, 14, 3, 20), (58, 14, 3, 20)]


def ppm(path):
    data = open(path, "rb").read()
    end = 0
    for _ in range(3):
        end = data.index(b"\n", end) + 1
    w, h = (int(v) for v in data.split(b"\n", 2)[1].split())
    return np.frombuffer(data, dtype=np.uint8, offset=end).reshape(h, w, 3).astype(np.int32)


def magenta(p):
    return (p[:, :, 0] > 200) & (p[:, :, 1] < 110) & (p[:, :, 2] > 170)


def cyan(p):
    return (p[:, :, 0] < 90) & (p[:, :, 1] > 180) & (p[:, :, 2] > 190)


def grow(mask, n):
    out = mask.copy()
    for axis in (0, 1):
        for _ in range(n):
            out = out | np.roll(out, 1, axis=axis) | np.roll(out, -1, axis=axis)
    return out


def bar_columns(p):
    # the white bar of the test scene: columns white in a row of the scene that no HUD reaches
    row = p[420]
    return (row[:, 0] > 200) & (row[:, 1] > 200) & (row[:, 2] > 200)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    folder = sys.argv[1]
    expect = "--expect-gaps" in sys.argv
    log = "".join(open(f, errors="replace").read() for f in glob.glob(folder + "/fg-*.log"))
    without = {int(n) for n in re.findall(r"fg: dump (\d+) written \([^)]*no capture of its own", log)}
    prefixes = sorted(p[: -len("-real.ppm")] for p in glob.glob(folder + "/dump-*-real.ppm"))
    if not prefixes:
        print(folder + ": no dump")
        return 1
    bad = seen = 0
    for prefix in prefixes:
        number = int(prefix.rsplit("-", 1)[1])
        previous, real, generated = (ppm(f"{prefix}-{n}.ppm") for n in ("previous", "real", "generated"))
        # (no scene in the dump: the frames were interpolated as presented, the HUD in them)
        scene = ppm(f"{prefix}-scene.ppm") if os.path.exists(f"{prefix}-scene.ppm") else real
        hud = cyan(real)
        moving = grow(magenta(previous) | magenta(real) | magenta(generated), 14)
        hud = hud & ~moving
        swept = grow(bar_columns(previous)[None, :] | bar_columns(real)[None, :], 16)[0]
        lo, hi = np.nonzero(swept)[0].min(initial=0), np.nonzero(swept)[0].max(initial=0)
        columns = np.zeros(real.shape[1], bool)
        columns[lo : hi + 1] = True  # (all that lies between the bar's two places)
        around = grow(hud, 12) & ~grow(hud, 1)
        around[:, columns] = False
        around &= ~moving
        difference = np.abs(generated - real).mean(axis=2)
        on_hud = float(difference[hud].mean()) if hud.any() else 0.0
        on_band = float(difference[around].mean()) if around.any() else 0.0
        left = 100.0 * float(cyan(scene)[hud].mean()) if hud.any() else 0.0
        ok = on_hud <= 3.0 and on_band <= 3.0 and left <= 1.0
        kind = "no capture" if number in without else "captured"
        seen += number in without
        print(f"dump {number:3d} ({kind:10s}): hud {on_hud:5.2f}, around {on_band:5.2f}, in scene {left:5.1f} %  {'ok' if ok else 'NOT ok'}")
        bad += not ok
    if expect and not seen:
        print("none of the dumps is a frame without a capture")
        bad += 1
    print("gap-check:", "ok" if not bad else f"{bad} NOT ok")
    return 1 if bad else 0


sys.exit(main())
